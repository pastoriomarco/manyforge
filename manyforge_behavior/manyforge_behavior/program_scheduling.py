"""Bounded production scheduling for Program runs.

The host owns tick cadence only.  Program state remains in ``ProgramRunOwner``
and robot/scene/model semantics remain in their focused owners.
"""

from __future__ import annotations

from collections.abc import Callable
from collections.abc import Mapping
from copy import deepcopy
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
        self._resolutions: dict[str, tuple[NodeAttemptIdentity, str, object]] = {}
        self._lock = RLock()

    def require(
        self,
        identity: NodeAttemptIdentity,
        *,
        reason_code: str,
        message: str,
        policy_parameter: str,
        owner_port_name: str | None = None,
        owner_task_id: str | None = None,
        owner_assessment: Mapping[str, object] | None = None,
    ) -> ProgramInterventionSnapshot:
        record = ProgramInterventionSnapshot(
            identity=identity,
            reason_code=reason_code,
            message=message,
            policy_parameter=policy_parameter,
            owner_port_name=owner_port_name,
            owner_task_id=owner_task_id,
            owner_assessment=owner_assessment,
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

    def refresh_owner_assessment(
        self,
        identity: NodeAttemptIdentity,
        assessment: Mapping[str, object],
    ) -> ProgramInterventionSnapshot:
        with self._lock:
            current = self._records.get(identity.program_run_id)
            if current is None or current.identity != identity:
                raise ProgramSchedulingError(
                    "Program recovery assessment identifies a stale attempt"
                )
            updated = ProgramInterventionSnapshot(
                identity=current.identity,
                reason_code=current.reason_code,
                message=current.message,
                policy_parameter=current.policy_parameter,
                owner_port_name=current.owner_port_name,
                owner_task_id=current.owner_task_id,
                owner_assessment=assessment,
            )
            self._records[identity.program_run_id] = updated
            return updated

    def resolve(
        self,
        identity: NodeAttemptIdentity,
        *,
        action: str,
        result: object = None,
    ) -> None:
        if action not in {"retry", "manual_success", "manual_failure"}:
            raise ValueError("Program recovery resolution is unsupported")
        with self._lock:
            current = self._records.get(identity.program_run_id)
            if current is None or current.identity != identity:
                raise ProgramSchedulingError(
                    "Program recovery resolution is stale"
                )
            existing = self._resolutions.get(identity.program_run_id)
            resolution = (identity, action, deepcopy(result))
            if existing is not None and existing != resolution:
                raise ProgramSchedulingError(
                    "Program recovery already has another resolution"
                )
            self._resolutions[identity.program_run_id] = resolution

    def consume_resolution(
        self, identity: NodeAttemptIdentity
    ) -> tuple[str, object] | None:
        with self._lock:
            current = self._resolutions.get(identity.program_run_id)
            if current is None:
                return None
            if current[0] != identity:
                raise ProgramSchedulingError(
                    "Program recovery resolution identifies another attempt"
                )
            self._resolutions.pop(identity.program_run_id, None)
            self._records.pop(identity.program_run_id, None)
            return current[1], deepcopy(current[2])

    def release(self, program_run_id: str) -> None:
        if not isinstance(program_run_id, str) or not program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        with self._lock:
            self._records.pop(program_run_id, None)
            self._resolutions.pop(program_run_id, None)


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
        terminal_observer: Callable[[ProgramRunSnapshot], None] | None = None,
        state_observer: Callable[[ProgramRunSnapshot], None] | None = None,
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
        if terminal_observer is not None and not callable(terminal_observer):
            raise TypeError("terminal observer must be callable")
        if state_observer is not None and not callable(state_observer):
            raise TypeError("state observer must be callable")
        self._owner = owner
        self._tick_interval_s = float(tick_interval_s)
        self._maximum_scheduled_runs = maximum_scheduled_runs
        self._maximum_workers = maximum_workers
        self._monotonic = monotonic
        self._terminal_observer = terminal_observer
        self._state_observer = state_observer
        self._observed_revisions: dict[str, tuple[int, int]] = {}
        # The value is the latest command-side wake revision for this run.
        # A worker carries the revision it was submitted under, so its older
        # completion cannot remove a wake issued by a concurrent command.
        self._scheduled: dict[str, int] = {}
        self._inflight: dict[
            str, tuple[int, Future[ProgramRunSnapshot]]
        ] = {}
        self._schedule_revision = 0
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
            self._schedule_locked(program_run_id)

    def _schedule_locked(self, program_run_id: str) -> None:
        if (
            program_run_id not in self._scheduled
            and len(self._scheduled) >= self._maximum_scheduled_runs
        ):
            raise ProgramSchedulingError("scheduled Program-run set is full")
        self._schedule_revision += 1
        self._scheduled[program_run_id] = self._schedule_revision
        self._condition.notify_all()

    def start_run(
        self,
        admission: ProgramRunAdmission,
        *,
        program_run_id: str | None = None,
    ) -> ProgramRunSnapshot:
        snapshot = self._owner.start(
            admission,
            program_run_id=program_run_id,
        )
        self._schedule_or_stop(snapshot)
        self._observe_state(snapshot)
        return snapshot

    def _schedule_or_stop(self, snapshot: ProgramRunSnapshot) -> None:
        """Schedule admitted work or contain it if capacity changed meanwhile."""

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

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        snapshot = self._owner.stop(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )
        if snapshot.state in {
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }:
            self.schedule(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def stop_motion(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        """Stop addressed work while leaving the Program tree observable."""

        snapshot = self._owner.stop_motion(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )
        self.schedule(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def full_stop(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        """Contain the run and remove its preserved tree from scheduling."""

        snapshot = self._owner.full_stop(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )
        if snapshot.state is ProgramRunState.FULL_STOPPING:
            self.schedule(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def resume_full_stop(
        self,
        program_run_id: str,
        *,
        request_id: str,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Recover the frozen occurrence before making it schedulable."""

        with self._condition:
            self._raise_if_unavailable()
            if program_run_id not in self._scheduled and (
                len(self._scheduled) >= self._maximum_scheduled_runs
            ):
                raise ProgramSchedulingError("scheduled Program-run set is full")
        # Provider recovery may block; it must not hold the scheduler mutex
        # needed to publish/schedule a newer priority Stop. Admission and
        # Resume share the existing containment-on-scheduling-failure path.
        snapshot = self._owner.resume_full_stop(
            program_run_id,
            request_id=request_id,
            reason=reason,
        )
        self._schedule_or_stop(snapshot)
        self._observe_state(snapshot)
        return snapshot

    def reset_run(self, program_run_id: str) -> ProgramRunSnapshot:
        """Release the exact terminal run's resident owner effects."""

        snapshot = self._owner.reset_effects(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def reset_retained_run(self, program_run_id: str) -> None:
        """Reset a proven terminal run whose behavior-tree owner rotated."""

        self._owner.reset_retained_effects(program_run_id)

    def forget_reset_run(self, program_run_id: str) -> None:
        """Release scheduler presentation state and the reset resident tree."""

        with self._condition:
            if (
                program_run_id in self._scheduled
                or program_run_id in self._inflight
            ):
                raise ProgramSchedulingError(
                    "a scheduled Program run cannot be forgotten"
                )
        self._owner.forget_reset_run(program_run_id)
        with self._condition:
            self._observed_revisions.pop(program_run_id, None)

    def resume_recovery(
        self,
        program_run_id: str,
        *,
        node_attempt_id: str,
        action: str,
        result: object = None,
    ) -> ProgramRunSnapshot:
        if not isinstance(program_run_id, str) or not program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        with self._condition:
            self._raise_if_unavailable()
            if program_run_id not in self._scheduled and (
                len(self._scheduled) >= self._maximum_scheduled_runs
            ):
                raise ProgramSchedulingError("scheduled Program-run set is full")
            snapshot = self._owner.resolve_intervention(
                program_run_id,
                node_attempt_id=node_attempt_id,
                action=action,
                result=result,
            )
            self._schedule_locked(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def read(self, program_run_id: str) -> ProgramRunSnapshot:
        return self._owner.read(program_run_id)

    def wait_terminal(
        self,
        program_run_id: str,
        *,
        timeout_s: float = 5.0,
    ) -> ProgramRunSnapshot:
        """Wait on owner progress notifications, never by polling its state."""

        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, (int, float))
            or not 0.0 < float(timeout_s) <= 60.0
        ):
            raise ValueError("terminal wait timeout is outside (0, 60]")
        deadline = self._monotonic() + float(timeout_s)
        with self._condition:
            while True:
                self._raise_if_unavailable()
                snapshot = self._owner.read(program_run_id)
                if snapshot.state in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                }:
                    return snapshot
                remaining = deadline - self._monotonic()
                if remaining <= 0.0:
                    raise ProgramSchedulingError(
                        "Program run did not settle before its terminal deadline"
                    )
                self._condition.wait(timeout=remaining)

    def _requires_advance(self, snapshot: ProgramRunSnapshot) -> bool:
        if snapshot.state in {
            ProgramRunState.RUNNING,
            ProgramRunState.FULL_STOPPING,
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }:
            return True
        return (
            snapshot.state is ProgramRunState.INTERVENTION_REQUIRED
            and self._owner.operator_stop_observation_required(
                snapshot.capture.program_run_id
            )
        )

    def _current_after_worker(
        self,
        completed: ProgramRunSnapshot,
    ) -> ProgramRunSnapshot:
        """Join a worker result to any newer command-side owner transition."""

        current = self._owner.read(completed.capture.program_run_id)
        if current.run_revision < completed.run_revision:
            raise ProgramSchedulingError(
                "Program-run owner revision regressed after a tick"
            )
        return current

    def advance_ready_once(self) -> tuple[ProgramRunSnapshot, ...]:
        with self._condition:
            self._raise_if_unavailable()
            if self._thread is not None:
                raise ProgramSchedulingError(
                    "manual Program tick is unavailable while the host is running"
                )
            scheduled = tuple(sorted(self._scheduled.items()))
        futures = tuple(
            (
                program_run_id,
                schedule_revision,
                self._executor.submit(
                    self._owner.advance_once,
                    program_run_id,
                ),
            )
            for program_run_id, schedule_revision in scheduled
        )
        snapshots = [
            self._current_after_worker(future.result())
            for _program_run_id, _schedule_revision, future in futures
        ]
        remove: list[tuple[str, int]] = []
        for (program_run_id, schedule_revision), snapshot in zip(
            scheduled,
            snapshots,
            strict=True,
        ):
            if not self._requires_advance(snapshot):
                remove.append((program_run_id, schedule_revision))
        if remove:
            with self._condition:
                for program_run_id, schedule_revision in remove:
                    if self._scheduled.get(program_run_id) == schedule_revision:
                        self._scheduled.pop(program_run_id, None)
        self._observe_terminals(snapshots)
        self._observe_states(snapshots)
        return tuple(snapshots)

    def _observe_state(self, snapshot: ProgramRunSnapshot) -> None:
        """Wake presentation without making it part of scheduling correctness."""

        observer = self._state_observer
        if observer is None:
            return
        program_run_id = snapshot.capture.program_run_id
        revision = (snapshot.run_revision, snapshot.node_status_revision)
        with self._condition:
            current = self._observed_revisions.get(program_run_id)
            if current is not None and (
                revision[0] <= current[0] and revision[1] <= current[1]
            ):
                return
        try:
            observer(snapshot)
        except Exception:
            # A status consumer cannot stall or fail Program execution.  It
            # will recover from the next current-snapshot wake.
            return
        with self._condition:
            current = self._observed_revisions.get(program_run_id)
            if current is None:
                self._observed_revisions[program_run_id] = revision
            else:
                self._observed_revisions[program_run_id] = (
                    max(current[0], revision[0]),
                    max(current[1], revision[1]),
                )

    def _observe_states(self, snapshots: list[ProgramRunSnapshot]) -> None:
        for snapshot in snapshots:
            self._observe_state(snapshot)

    def _observe_terminals(self, snapshots: list[ProgramRunSnapshot]) -> None:
        observer = self._terminal_observer
        if observer is None:
            return
        try:
            for snapshot in snapshots:
                if snapshot.state not in {
                    ProgramRunState.RUNNING,
                    ProgramRunState.INTERVENTION_REQUIRED,
                    ProgramRunState.FULL_STOPPING,
                    ProgramRunState.FULL_STOPPED,
                    ProgramRunState.STOPPING,
                    ProgramRunState.FINALIZING,
                }:
                    observer(snapshot)
        except Exception as error:
            with self._condition:
                self._worker_failure = error
                self._condition.notify_all()
            raise ProgramSchedulingError(
                "Program terminal observer failed"
            ) from error

    def _run(self) -> None:
        next_tick = self._monotonic()
        while True:
            completed: list[ProgramRunSnapshot] = []
            observed: list[ProgramRunSnapshot] = []
            with self._condition:
                if self._shutdown:
                    return
                for program_run_id, inflight in tuple(self._inflight.items()):
                    schedule_revision, future = inflight
                    if not future.done():
                        continue
                    self._inflight.pop(program_run_id)
                    try:
                        snapshot = self._current_after_worker(future.result())
                    except Exception as error:
                        self._worker_failure = error
                        self._condition.notify_all()
                        return
                    observed.append(snapshot)
                    if not self._requires_advance(snapshot):
                        removed = (
                            self._scheduled.get(program_run_id)
                            == schedule_revision
                        )
                        if removed:
                            self._scheduled.pop(program_run_id, None)
                        if removed and snapshot.state is not (
                            ProgramRunState.INTERVENTION_REQUIRED
                        ):
                            completed.append(snapshot)

                now = self._monotonic()
                delay = next_tick - now
                if completed:
                    pass
                elif not self._scheduled:
                    self._condition.wait()
                elif delay > 0.0:
                    self._condition.wait(
                        timeout=delay
                    )
                else:
                    for program_run_id, schedule_revision in sorted(
                        self._scheduled.items()
                    ):
                        if program_run_id in self._inflight:
                            continue
                        future = self._executor.submit(
                            self._owner.advance_once,
                            program_run_id,
                        )
                        self._inflight[program_run_id] = (
                            schedule_revision,
                            future,
                        )
                        future.add_done_callback(
                            lambda _future: self._notify_worker_completion()
                        )
                    next_tick = now + self._tick_interval_s
            if completed:
                try:
                    self._observe_terminals(completed)
                except ProgramSchedulingError:
                    return
            if observed:
                self._observe_states(observed)

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
