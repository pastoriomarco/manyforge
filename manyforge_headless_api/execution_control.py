"""Small bounded priority lanes for execution-control transport work.

This module owns no robot, Program, lifecycle, or command state.  It only keeps
ordinary synchronous HTTP/native client calls from consuming the worker needed
to hand a recognized Stop request to its existing reserved owner path.
"""

from __future__ import annotations

import asyncio
import itertools
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
from enum import Enum
from typing import Any, Callable, Literal, Mapping, Protocol, TypeVar

from .control_protocol import (
    ControlFreshnessAuthority,
    ExecutionCommandReceiptDTO,
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
    is_stop_command,
)


T = TypeVar("T")


class ExecutionControlProjectionPort(Protocol):
    """The Program cycle's current execution-control revision."""

    def __call__(self) -> int: ...


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
    """Workers of one lane and its admission limit.

    ``maximum_admitted`` is ``None`` for the Stop lane: a Stop is never
    refused; one beyond the workers waits in that lane's FIFO, behind Stops
    only.
    """

    workers: int
    maximum_admitted: int | None

    def __post_init__(self) -> None:
        if self.workers < 1:
            raise ValueError("workers must be positive")
        if self.maximum_admitted is not None and self.maximum_admitted < self.workers:
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
        self._capacity = (
            None
            if spec.maximum_admitted is None
            else threading.BoundedSemaphore(spec.maximum_admitted)
        )
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
        capacity = self._capacity
        if capacity is not None and not capacity.acquire(blocking=False):
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
            if capacity is not None:
                capacity.release()
            raise

        def release_capacity(_completed: asyncio.Future[T]) -> None:
            with self._lock:
                self._admitted -= 1
            if capacity is not None:
                capacity.release()

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
    """One never-refusing Stop lane plus one no-wait ordinary-command lane."""

    def __init__(
        self,
        *,
        stop: ExecutionControlLaneSpec = ExecutionControlLaneSpec(2, None),
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
        application_start_id: str,
        authority: ControlFreshnessAuthority | None = None,
        lanes: ExecutionControlClient | None = None,
        clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        if not callable(projection) or not callable(dispatch):
            raise TypeError("execution-control ingress ports must be callable")
        self._projection = projection
        self._dispatch = dispatch
        self._application_start_id = application_start_id
        # Control session numbers: a counter from 1 per start (request key
        # class 0 origin). Sessions open on the event loop; ``count`` needs
        # no lock.
        self._session_numbers = itertools.count(1)
        self._authority = authority or ControlFreshnessAuthority()
        self._lanes = lanes or ExecutionControlClient()
        self._owns_lanes = lanes is None
        self._clock_ns = clock_ns
        self._lifecycle_lock = threading.Lock()
        self._active_control_sessions = 0
        self._sequence_lock = threading.Lock()
        self._received_sequence = 0

    @property
    def application_start_id(self) -> str:
        return self._application_start_id

    def next_control_session_number(self) -> int:
        return next(self._session_numbers)

    @property
    def active_control_sessions(self) -> int:
        with self._lifecycle_lock:
            return self._active_control_sessions

    def open_control_session(self) -> None:
        with self._lifecycle_lock:
            self._active_control_sessions += 1

    def close_control_session(self) -> None:
        with self._lifecycle_lock:
            if self._active_control_sessions < 1:
                raise RuntimeError("control session registration underflow")
            self._active_control_sessions -= 1

    def current_freshness(self):
        return self.freshness_for(self._projection())

    def freshness_for(self, control_revision: int):
        """Issue from an already-cached exact owner projection (heartbeat path)."""

        return self._authority.issue(execution_control_revision=control_revision)

    def _next_received_sequence(self) -> int:
        with self._sequence_lock:
            self._received_sequence += 1
            return self._received_sequence

    def _submit_sync(
        self,
        request: ExecutionCommandRequestDTO,
        stamp: ExecutionIngressStamp,
        received_at_ns: int,
    ) -> ExecutionControlSubmission:
        envelope = request.command_envelope
        if not is_stop_command(envelope.command_kind):
            valid_until_ns = self._authority.validate(
                envelope.freshness,
                current_control_revision=self._projection(),
                received_at_ns=received_at_ns,
            )
            # The envelope DTO already pins expected_* to the token fields, and
            # validate() rejected any token not matching this same projection.
            stamp = replace(stamp, valid_until_monotonic_ns=valid_until_ns)
        disposition, result = self._dispatch(request, stamp)
        receipt = ExecutionCommandReceiptDTO(
            schema_version="manyforge.execution_command_receipt.v3",
            control_session_number=envelope.control_session_number,
            client_sequence=envelope.client_sequence,
            received_server_sequence=str(stamp.received_server_sequence),
            received_monotonic_ns=str(stamp.received_monotonic_ns),
            disposition=disposition.disposition,
            code=disposition.code,
            execution_control_revision=disposition.execution_control_revision,
            current_identity=(
                None if disposition.current_identity is None else dict(disposition.current_identity)
            ),
            request_key=disposition.request_key,
            command=None if disposition.command is None else dict(disposition.command),
        )
        return ExecutionControlSubmission(receipt=receipt, result=result)

    def rejection_receipt(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        code: str,
        execution_control_revision: int | None = None,
    ) -> ExecutionCommandReceiptDTO:
        """Project a failed ingress decision without retaining a receipt."""

        control_revision = (
            self._projection()
            if execution_control_revision is None
            else execution_control_revision
        )
        envelope = request.command_envelope
        return ExecutionCommandReceiptDTO(
            schema_version="manyforge.execution_command_receipt.v3",
            control_session_number=envelope.control_session_number,
            client_sequence=envelope.client_sequence,
            received_server_sequence=str(self._next_received_sequence()),
            received_monotonic_ns=str(self._clock_ns()),
            disposition="rejected",
            code=code,
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
        received_monotonic_ns: int | None = None,
    ) -> ExecutionControlSubmission:
        """``received_monotonic_ns``: when Composer received a command it
        forwards, on the shared monotonic clock; freshness is aged to that
        moment instead of this ingress's own receipt."""

        return await asyncio.shield(
            self._start_submission(
                request,
                transport_kind=transport_kind,
                trusted_principal_id=trusted_principal_id,
                assurance=assurance,
                received_monotonic_ns=received_monotonic_ns,
            )
        )

    def _start_submission(
        self,
        request: ExecutionCommandRequestDTO,
        *,
        transport_kind: Literal["rest", "websocket", "ros2", "local"],
        trusted_principal_id: str | None,
        assurance: str | None,
        received_monotonic_ns: int | None = None,
    ) -> asyncio.Future[ExecutionControlSubmission]:
        stamp: ExecutionIngressStamp | None = None

        def mark_admitted() -> None:
            nonlocal stamp
            stamp = ExecutionIngressStamp(
                transport_kind=transport_kind,
                received_server_sequence=self._next_received_sequence(),
                received_monotonic_ns=self._clock_ns(),
                trusted_principal_id=trusted_principal_id,
                assurance=assurance,
            )

        def callback() -> ExecutionControlSubmission:
            if stamp is None:
                raise RuntimeError("execution-control admission stamp is missing")
            # Freshness is aged to the command's receipt, not to this worker
            # turn: the projection read and lane wait never expire it.
            return self._submit_sync(
                request,
                stamp,
                stamp.received_monotonic_ns
                if received_monotonic_ns is None
                else received_monotonic_ns,
            )

        lane = (
            ExecutionControlLane.STOP
            if is_stop_command(request.command_envelope.command_kind)
            else ExecutionControlLane.ORDINARY
        )
        return self._lanes.start(
            lane,
            callback,
            on_admitted=mark_admitted,
        )

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
