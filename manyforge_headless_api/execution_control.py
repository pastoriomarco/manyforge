"""Small bounded priority lanes for execution-control transport work.

This module owns no robot, Program, lifecycle, or command state.  It only keeps
ordinary synchronous HTTP/native client calls from consuming the worker needed
to hand a recognized Stop request to its existing reserved owner path.
"""

from __future__ import annotations

import asyncio
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
from enum import Enum
from typing import Any, Callable, Literal, Mapping, Protocol, TypeVar

from .control_protocol import (
    ControlFreshnessAuthority,
    ControlProtocolError,
    ExecutionCommandReceiptDTO,
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
    MAXIMUM_U64,
    is_stop_command,
)


T = TypeVar("T")


class ExecutionControlProjectionPort(Protocol):
    def __call__(self) -> tuple[str, int]: ...


class ExecutionControlDispatchPort(Protocol):
    def __call__(
        self,
        request: ExecutionCommandRequestDTO,
        stamp: ExecutionIngressStamp,
    ) -> tuple[ExecutionIngressResult, Mapping[str, Any] | None]: ...


@dataclass(frozen=True, slots=True)
class ExecutionControlSubmission:
    receipt: ExecutionCommandReceiptDTO
    result: Mapping[str, Any] | None


class ExecutionControlLane(str, Enum):
    """The two transport scheduling classes of execution control."""

    STOP = "stop"
    ORDINARY = "ordinary"


class ExecutionControlBusyError(RuntimeError):
    """Raised before submission when one bounded lane is full."""

    def __init__(self, lane: ExecutionControlLane, maximum_admitted: int) -> None:
        super().__init__(
            f"execution-control {lane.value} lane is at its admission limit "
            f"({maximum_admitted})"
        )
        self.lane = lane
        self.maximum_admitted = maximum_admitted


@dataclass(frozen=True, slots=True)
class ExecutionControlLaneSpec:
    workers: int
    maximum_admitted: int

    def __post_init__(self) -> None:
        if self.workers < 1:
            raise ValueError("workers must be positive")
        if self.maximum_admitted < self.workers:
            raise ValueError("maximum_admitted must be at least workers")


class _BoundedLane:
    def __init__(
        self,
        lane: ExecutionControlLane,
        spec: ExecutionControlLaneSpec,
        *,
        thread_name_prefix: str,
    ) -> None:
        self._lane = lane
        self._spec = spec
        self._thread_name_prefix = thread_name_prefix
        self._capacity = threading.BoundedSemaphore(spec.maximum_admitted)
        self._lock = threading.Lock()
        self._executor: ThreadPoolExecutor | None = None
        self._closed = False
        self._admitted = 0
        self._running = 0
        self._rejected = 0

    def _executor_for_submit(self) -> ThreadPoolExecutor:
        with self._lock:
            if self._closed:
                raise RuntimeError("execution-control client is closed")
            if self._executor is None:
                self._executor = ThreadPoolExecutor(
                    max_workers=self._spec.workers,
                    thread_name_prefix=self._thread_name_prefix,
                )
            return self._executor

    def _invoke(self, callback: Callable[[], T]) -> T:
        with self._lock:
            self._running += 1
        try:
            return callback()
        finally:
            with self._lock:
                self._running -= 1

    def submit(
        self,
        callback: Callable[[], T],
        *,
        on_admitted: Callable[[], None] | None = None,
    ) -> asyncio.Future[T]:
        if not self._capacity.acquire(blocking=False):
            with self._lock:
                self._rejected += 1
            raise ExecutionControlBusyError(
                self._lane,
                self._spec.maximum_admitted,
            )
        with self._lock:
            self._admitted += 1
        try:
            if on_admitted is not None:
                on_admitted()
            executor = self._executor_for_submit()
            future = asyncio.get_running_loop().run_in_executor(
                executor,
                self._invoke,
                callback,
            )
        except BaseException:
            with self._lock:
                self._admitted -= 1
            self._capacity.release()
            raise

        def release_capacity(_completed: asyncio.Future[T]) -> None:
            with self._lock:
                self._admitted -= 1
            self._capacity.release()

        future.add_done_callback(release_capacity)
        # Cancelling a disconnected HTTP handler cannot cancel synchronous
        # transport work already running in a thread. Keep its admission until
        # the worker really finishes so disconnect storms cannot grow the lane.
        return future

    async def run(self, callback: Callable[[], T]) -> T:
        return await asyncio.shield(self.submit(callback))

    def close(self) -> None:
        with self._lock:
            self._closed = True
            executor = self._executor
            self._executor = None
        if executor is not None:
            executor.shutdown(wait=False, cancel_futures=True)

    def status(self) -> dict[str, Any]:
        with self._lock:
            admitted = self._admitted
            running = self._running
            rejected = self._rejected
            open_ = not self._closed
        return {
            "open": open_,
            "workers": self._spec.workers,
            "maximum_admitted": self._spec.maximum_admitted,
            "admitted": admitted,
            "running": running,
            "queued": max(0, admitted - running),
            "rejected": rejected,
        }


class ExecutionControlClient:
    """One reserved Stop lane plus one no-wait ordinary-command lane."""

    def __init__(
        self,
        *,
        stop: ExecutionControlLaneSpec = ExecutionControlLaneSpec(2, 2),
        ordinary: ExecutionControlLaneSpec = ExecutionControlLaneSpec(2, 2),
        thread_name_prefix: str = "manyforge_execution_control",
    ) -> None:
        self._lanes = {
            ExecutionControlLane.STOP: _BoundedLane(
                ExecutionControlLane.STOP,
                stop,
                thread_name_prefix=f"{thread_name_prefix}_stop",
            ),
            ExecutionControlLane.ORDINARY: _BoundedLane(
                ExecutionControlLane.ORDINARY,
                ordinary,
                thread_name_prefix=f"{thread_name_prefix}_ordinary",
            ),
        }

    async def stop(self, callback: Callable[[], T]) -> T:
        return await self._lanes[ExecutionControlLane.STOP].run(callback)

    async def ordinary(self, callback: Callable[[], T]) -> T:
        return await self._lanes[ExecutionControlLane.ORDINARY].run(callback)

    def start(
        self,
        lane: ExecutionControlLane,
        callback: Callable[[], T],
        *,
        on_admitted: Callable[[], None] | None = None,
    ) -> asyncio.Future[T]:
        return self._lanes[lane].submit(callback, on_admitted=on_admitted)

    def close(self) -> None:
        for lane in self._lanes.values():
            lane.close()

    def status(self) -> dict[str, dict[str, Any]]:
        return {lane.value: self._lanes[lane].status() for lane in self._lanes}


class ExecutionControlIngress:
    """One freshness/stamping boundary shared by every transport adapter."""

    def __init__(
        self,
        *,
        projection: ExecutionControlProjectionPort,
        dispatch: ExecutionControlDispatchPort,
        authority: ControlFreshnessAuthority | None = None,
        lanes: ExecutionControlClient | None = None,
        clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        if not callable(projection) or not callable(dispatch):
            raise TypeError("execution-control ingress ports must be callable")
        self._projection = projection
        self._dispatch = dispatch
        self._authority = authority or ControlFreshnessAuthority()
        self._lanes = lanes or ExecutionControlClient()
        self._owns_lanes = lanes is None
        self._clock_ns = clock_ns
        self._lifecycle_lock = threading.Lock()
        self._active_control_sessions = 0
        self._sequence_lock = threading.Lock()
        self._received_sequence = 0

    @property
    def receiver_incarnation(self) -> str:
        with self._lifecycle_lock:
            return self._authority.receiver_incarnation

    @property
    def active_control_sessions(self) -> int:
        with self._lifecycle_lock:
            return self._active_control_sessions

    def _rollover_required_locked(self) -> bool:
        with self._sequence_lock:
            received_sequence_exhausted = self._received_sequence >= MAXIMUM_U64
        return self._authority.rollover_required or received_sequence_exhausted

    def _rotate_if_quiescent_locked(self) -> bool:
        if not self._rollover_required_locked():
            return False
        admitted = sum(lane["admitted"] for lane in self._lanes.status().values())
        if self._active_control_sessions or admitted:
            return False
        self._authority.rotate_quiescent(
            active_sessions=0,
            inflight_commands=0,
        )
        with self._sequence_lock:
            self._received_sequence = 0
        return True

    def _try_rotate_if_quiescent(self) -> bool:
        with self._lifecycle_lock:
            return self._rotate_if_quiescent_locked()

    def open_control_session(self) -> None:
        with self._lifecycle_lock:
            self._rotate_if_quiescent_locked()
            if self._rollover_required_locked():
                raise ControlProtocolError(
                    "control_sequence_rollover_required",
                    "control receiver must finish its current sessions before rollover",
                )
            self._active_control_sessions += 1

    def close_control_session(self) -> None:
        with self._lifecycle_lock:
            if self._active_control_sessions < 1:
                raise RuntimeError("control session registration underflow")
            self._active_control_sessions -= 1
            self._rotate_if_quiescent_locked()

    def current_freshness(self):
        owner_epoch, control_revision = self._projection()
        return self.freshness_for(owner_epoch, control_revision)

    def freshness_for(self, owner_epoch: str, control_revision: int):
        """Issue from an already-cached exact owner projection (heartbeat path)."""

        with self._lifecycle_lock:
            self._rotate_if_quiescent_locked()
            return self._authority.issue(
                semantic_owner_epoch=owner_epoch,
                execution_control_revision=control_revision,
            )

    def _next_received_sequence(self) -> int:
        with self._sequence_lock:
            if self._received_sequence >= MAXIMUM_U64:
                raise ControlProtocolError(
                    "control_sequence_rollover_required",
                    "control ingress sequence requires quiescent rotation",
                )
            self._received_sequence += 1
            return self._received_sequence

    def _submit_sync(
        self,
        request: ExecutionCommandRequestDTO,
        stamp: ExecutionIngressStamp,
    ) -> ExecutionControlSubmission:
        envelope = request.command_envelope
        if not is_stop_command(envelope.command_kind):
            owner_epoch, control_revision = self._projection()
            valid_until_ns = self._authority.validate(
                envelope.freshness,
                current_owner_epoch=owner_epoch,
                current_control_revision=control_revision,
            )
            # The envelope DTO already pins expected_* to the token fields, and
            # validate() rejected any token not matching this same projection.
            stamp = replace(stamp, valid_until_monotonic_ns=valid_until_ns)
        disposition, result = self._dispatch(request, stamp)
        receipt = ExecutionCommandReceiptDTO(
            schema_version="manyforge.execution_command_receipt.v1",
            correlation_id=envelope.correlation_id,
            control_session_id=envelope.control_session_id,
            client_sequence=envelope.client_sequence,
            receiver_incarnation=stamp.receiver_incarnation,
            received_server_sequence=str(stamp.received_server_sequence),
            received_monotonic_ns=str(stamp.received_monotonic_ns),
            disposition=disposition.disposition,
            code=disposition.code,
            semantic_owner_epoch=disposition.semantic_owner_epoch,
            execution_control_revision=disposition.execution_control_revision,
            current_identity=(
                None if disposition.current_identity is None else dict(disposition.current_identity)
            ),
        )
        return ExecutionControlSubmission(receipt=receipt, result=result)

    def rejection_receipt(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        code: str,
        semantic_owner_epoch: str | None = None,
        execution_control_revision: int | None = None,
    ) -> ExecutionCommandReceiptDTO:
        """Project a failed ingress decision without retaining a receipt."""

        if semantic_owner_epoch is None or execution_control_revision is None:
            owner_epoch, control_revision = self._projection()
        else:
            owner_epoch = semantic_owner_epoch
            control_revision = execution_control_revision
        envelope = request.command_envelope
        return ExecutionCommandReceiptDTO(
            schema_version="manyforge.execution_command_receipt.v1",
            correlation_id=envelope.correlation_id,
            control_session_id=envelope.control_session_id,
            client_sequence=envelope.client_sequence,
            receiver_incarnation=self.receiver_incarnation,
            received_server_sequence=str(self._next_received_sequence()),
            received_monotonic_ns=str(self._clock_ns()),
            disposition="rejected",
            code=code,
            semantic_owner_epoch=owner_epoch,
            execution_control_revision=control_revision,
            current_identity=None,
        )

    async def submit(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        transport_kind: Literal["rest", "websocket", "ros2", "local"],
        trusted_principal_id: str | None = None,
        assurance: str | None = None,
    ) -> ExecutionControlSubmission:
        return await asyncio.shield(
            self._start_submission(
                request,
                transport_kind=transport_kind,
                trusted_principal_id=trusted_principal_id,
                assurance=assurance,
            )
        )

    def _start_submission(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        transport_kind: Literal["rest", "websocket", "ros2", "local"],
        trusted_principal_id: str | None,
        assurance: str | None,
    ) -> asyncio.Future[ExecutionControlSubmission]:
        stamp: ExecutionIngressStamp | None = None

        def mark_admitted() -> None:
            nonlocal stamp
            stamp = ExecutionIngressStamp(
                transport_kind=transport_kind,
                receiver_incarnation=self._authority.receiver_incarnation,
                received_server_sequence=self._next_received_sequence(),
                received_monotonic_ns=self._clock_ns(),
                trusted_principal_id=trusted_principal_id,
                assurance=assurance,
            )

        def callback() -> ExecutionControlSubmission:
            if stamp is None:
                raise RuntimeError("execution-control admission stamp is missing")
            return self._submit_sync(
                request,
                stamp,
            )

        lane = (
            ExecutionControlLane.STOP
            if is_stop_command(request.command_envelope.command_kind)
            else ExecutionControlLane.ORDINARY
        )
        with self._lifecycle_lock:
            self._rotate_if_quiescent_locked()
            if self._rollover_required_locked():
                raise ControlProtocolError(
                    "control_sequence_rollover_required",
                    "control receiver must reach a quiescent boundary before admission",
                )
            future = self._lanes.start(
                lane,
                callback,
                on_admitted=mark_admitted,
            )
        future.add_done_callback(lambda _done: self._try_rotate_if_quiescent())
        return future

    def start(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        transport_kind: Literal["rest", "websocket", "ros2", "local"],
        trusted_principal_id: str | None = None,
        assurance: str | None = None,
    ) -> asyncio.Future[ExecutionControlSubmission]:
        """Admit one command to an existing lane without awaiting its effect."""

        return self._start_submission(
            request,
            transport_kind=transport_kind,
            trusted_principal_id=trusted_principal_id,
            assurance=assurance,
        )

    def close(self) -> None:
        if self._owns_lanes:
            self._lanes.close()


__all__ = [
    "ExecutionControlBusyError",
    "ExecutionControlClient",
    "ExecutionControlIngress",
    "ExecutionControlLane",
    "ExecutionControlLaneSpec",
    "ExecutionControlSubmission",
]
