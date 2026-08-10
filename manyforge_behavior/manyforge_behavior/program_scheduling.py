"""Bounded production scheduling for Program runs.

The host owns tick cadence only.  Program state remains in ``ProgramRunOwner``
and robot/scene/model semantics remain in their focused owners.
"""

from __future__ import annotations

from collections.abc import Callable
from concurrent.futures import Future, ThreadPoolExecutor
from threading import Condition, RLock, Thread
import time
from typing import Final

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    ProgramInterventionSnapshot,
)
from manyforge_behavior.program_run_owner import (
    ProgramRunAdmission,
    ProgramRunOwner,
    ProgramRunSnapshot,
    ProgramRunState,
)


_MAX_SCHEDULED_RUNS: Final = 4_096
_MAX_TICK_WORKERS: Final = 32
_MIN_TICK_INTERVAL_S: Final = 0.001
_MAX_TICK_INTERVAL_S: Final = 1.0


class ProgramSchedulingError(RuntimeError):
    """The bounded scheduler or intervention registry is unavailable."""


class ProgramInterventionRegistry:
    """Retain at most one exact authored intervention per Program run."""

    def __init__(self, *, maximum_retained_runs: int = _MAX_SCHEDULED_RUNS) -> None:
        if (
            type(maximum_retained_runs) is not int
            or not 1 <= maximum_retained_runs <= _MAX_SCHEDULED_RUNS
        ):
            raise ValueError("intervention retention is outside its bound")
        self._maximum_retained_runs = maximum_retained_runs
        self._records: dict[str, ProgramInterventionSnapshot] = {}
        self._lock = RLock()

    def require(
        self,
        identity: NodeAttemptIdentity,
        *,
        reason_code: str,
        message: str,
        policy_parameter: str,
    ) -> ProgramInterventionSnapshot:
        record = ProgramInterventionSnapshot(
            identity=identity,
            reason_code=reason_code,
            message=message,
            policy_parameter=policy_parameter,
        )
        with self._lock:
            current = self._records.get(identity.program_run_id)
            if current is not None:
                if current != record:
                    raise ProgramSchedulingError(
                        "Program run already requires another intervention"
                    )
                return current
            if len(self._records) >= self._maximum_retained_runs:
                raise ProgramSchedulingError("intervention retention is full")
            self._records[identity.program_run_id] = record
            return record

    def read(self, program_run_id: str) -> ProgramInterventionSnapshot | None:
        if not isinstance(program_run_id, str) or not program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        with self._lock:
            return self._records.get(program_run_id)

    def release(self, program_run_id: str) -> None:
        if not isinstance(program_run_id, str) or not program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        with self._lock:
            self._records.pop(program_run_id, None)


class ProgramRunTickHost:
    """Advance many Program runs through one fixed-size production pool."""

    def __init__(
        self,
        owner: ProgramRunOwner,
        *,
        tick_interval_s: float = 0.01,
        maximum_scheduled_runs: int = _MAX_SCHEDULED_RUNS,
        maximum_workers: int = 4,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        if not isinstance(owner, ProgramRunOwner):
            raise TypeError("tick host requires one ProgramRunOwner")
        if (
            isinstance(tick_interval_s, bool)
            or not isinstance(tick_interval_s, (int, float))
            or not _MIN_TICK_INTERVAL_S
            <= float(tick_interval_s)
            <= _MAX_TICK_INTERVAL_S
        ):
            raise ValueError("tick interval is outside its fixed bound")
        if (
            type(maximum_scheduled_runs) is not int
            or not 1 <= maximum_scheduled_runs <= _MAX_SCHEDULED_RUNS
        ):
            raise ValueError("scheduled-run bound is invalid")
        if (
            type(maximum_workers) is not int
            or not 1 <= maximum_workers <= _MAX_TICK_WORKERS
        ):
            raise ValueError("tick-worker bound is invalid")
        if not callable(monotonic):
            raise TypeError("monotonic clock must be callable")
        self._owner = owner
        self._tick_interval_s = float(tick_interval_s)
        self._maximum_scheduled_runs = maximum_scheduled_runs
        self._maximum_workers = maximum_workers
        self._monotonic = monotonic
        self._scheduled: set[str] = set()
        self._inflight: dict[str, Future[ProgramRunSnapshot]] = {}
        self._condition = Condition(RLock())
        self._thread: Thread | None = None
        self._shutdown = False
        self._worker_failure: Exception | None = None
        self._executor = ThreadPoolExecutor(
            max_workers=maximum_workers,
            thread_name_prefix="manyforge-program-tick-worker",
        )

    def _raise_if_unavailable(self) -> None:
        if self._shutdown:
            raise ProgramSchedulingError("Program tick host is shut down")
        if self._worker_failure is not None:
            raise ProgramSchedulingError("Program tick worker failed") from (
                self._worker_failure
            )

    def start(self) -> None:
        with self._condition:
            self._raise_if_unavailable()
            if self._thread is not None:
                return
            self._thread = Thread(
                target=self._run,
                name="manyforge-program-tick",
                daemon=True,
            )
            self._thread.start()

    def schedule(self, program_run_id: str) -> None:
        if not isinstance(program_run_id, str) or not program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        with self._condition:
            self._raise_if_unavailable()
            if (
                program_run_id not in self._scheduled
                and len(self._scheduled) >= self._maximum_scheduled_runs
            ):
                raise ProgramSchedulingError("scheduled Program-run set is full")
            self._scheduled.add(program_run_id)
            self._condition.notify_all()

    def start_run(self, admission: ProgramRunAdmission) -> ProgramRunSnapshot:
        snapshot = self._owner.start(admission)
        try:
            self.schedule(snapshot.capture.program_run_id)
        except Exception as scheduling_error:
            # Admission already created an owner entry. Fail closed and prove
            # its exact cleanup before returning the scheduling failure.
            try:
                cleaned = self._owner.stop(
                    snapshot.capture.program_run_id,
                    reason="Program tick scheduling failed",
                )
            except Exception as cleanup_error:
                raise ProgramSchedulingError(
                    "Program tick scheduling and admitted-run cleanup failed"
                ) from cleanup_error
            if cleaned.state not in {
                ProgramRunState.STOPPED,
                ProgramRunState.SUCCEEDED,
                ProgramRunState.FAILED,
            }:
                raise ProgramSchedulingError(
                    "Program tick scheduling failed before admitted-run "
                    "cleanup settled"
                ) from scheduling_error
            raise
        return snapshot

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        snapshot = self._owner.stop(program_run_id, reason=reason)
        if snapshot.state in {
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }:
            self.schedule(program_run_id)
        return snapshot

    def read(self, program_run_id: str) -> ProgramRunSnapshot:
        return self._owner.read(program_run_id)

    def advance_ready_once(self) -> tuple[ProgramRunSnapshot, ...]:
        with self._condition:
            self._raise_if_unavailable()
            if self._thread is not None:
                raise ProgramSchedulingError(
                    "manual Program tick is unavailable while the host is running"
                )
            program_run_ids = tuple(sorted(self._scheduled))
        futures = tuple(
            (
                program_run_id,
                self._executor.submit(
                    self._owner.advance_once,
                    program_run_id,
                ),
            )
            for program_run_id in program_run_ids
        )
        snapshots = [future.result() for _program_run_id, future in futures]
        remove: list[str] = []
        for program_run_id, snapshot in zip(
            program_run_ids,
            snapshots,
            strict=True,
        ):
            if snapshot.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.STOPPING,
                ProgramRunState.FINALIZING,
            }:
                remove.append(program_run_id)
        if remove:
            with self._condition:
                for program_run_id in remove:
                    self._scheduled.discard(program_run_id)
        return tuple(snapshots)

    def _run(self) -> None:
        next_tick = self._monotonic()
        while True:
            with self._condition:
                if self._shutdown:
                    return
                for program_run_id, future in tuple(self._inflight.items()):
                    if not future.done():
                        continue
                    self._inflight.pop(program_run_id)
                    try:
                        snapshot = future.result()
                    except Exception as error:
                        self._worker_failure = error
                        self._condition.notify_all()
                        return
                    if snapshot.state not in {
                        ProgramRunState.RUNNING,
                        ProgramRunState.STOPPING,
                        ProgramRunState.FINALIZING,
                    }:
                        self._scheduled.discard(program_run_id)

                now = self._monotonic()
                delay = next_tick - now
                if not self._scheduled:
                    self._condition.wait()
                    continue
                if delay > 0.0:
                    self._condition.wait(
                        timeout=delay
                    )
                    continue
                for program_run_id in sorted(self._scheduled):
                    if program_run_id in self._inflight:
                        continue
                    future = self._executor.submit(
                        self._owner.advance_once,
                        program_run_id,
                    )
                    self._inflight[program_run_id] = future
                    future.add_done_callback(
                        lambda _future: self._notify_worker_completion()
                    )
                next_tick = now + self._tick_interval_s

    def _notify_worker_completion(self) -> None:
        with self._condition:
            self._condition.notify_all()

    def shutdown(self, *, timeout_s: float = 5.0) -> None:
        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, (int, float))
            or not 0.0 < float(timeout_s) <= 60.0
        ):
            raise ValueError("shutdown timeout is outside its fixed bound")
        with self._condition:
            thread = self._thread
            self._shutdown = True
            self._condition.notify_all()
        if thread is not None:
            thread.join(timeout=float(timeout_s))
            if thread.is_alive():
                raise ProgramSchedulingError("Program tick worker did not stop")
        self._executor.shutdown(wait=True, cancel_futures=True)


__all__ = [
    "ProgramInterventionRegistry",
    "ProgramRunTickHost",
    "ProgramSchedulingError",
]
