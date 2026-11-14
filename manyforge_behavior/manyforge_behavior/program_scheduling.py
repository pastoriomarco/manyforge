"""Bounded production scheduling for Program runs.

The host owns tick cadence only.  Program state remains in ``ProgramRunOwner``
and robot/scene/model semantics remain in their focused owners.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from collections.abc import Collection, Mapping
from copy import deepcopy
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field, replace
import logging
from threading import Condition, RLock, Thread
import time
from typing import Any, Final

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    ProgramInterventionSnapshot,
    require_run_number,
)
from manyforge_behavior.program_run_owner import (
    RUN_FROZEN_LEVELS,
    ProgramRunAdmission,
    ProgramRunHoldFacts,
    ProgramRunOwner,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunSnapshot,
    ProgramRunState,
    RunIntent,
)


_MAX_SCHEDULED_RUNS: Final = 4_096
_MAX_TICK_WORKERS: Final = 32
_MIN_TICK_INTERVAL_S: Final = 0.001
_MAX_TICK_INTERVAL_S: Final = 1.0
# Late ticks (OD-213): the typed log line of an episode of a run's late
# ticks. While one episode lasts, a line is also written every
# `_LATE_TICK_LINE_EVERY` late ticks; a run writes at most
# `_MAX_LATE_TICK_LINES_PER_RUN` lines, after which its late ticks are only
# counted.
PROGRAM_TICKS_LATE_CODE: Final = "program_ticks_late"
_LATE_TICK_LINE_EVERY: Final = 1_000
_MAX_LATE_TICK_LINES_PER_RUN: Final = 64

_LOGGER = logging.getLogger(__name__)


class ProgramSchedulingError(RuntimeError):
    """The bounded scheduler or intervention registry is unavailable."""


@dataclass(frozen=True, slots=True)
class ProgramRunTickOverruns:
    """A run's late ticks so far: ticks that ended after their period."""

    late_ticks: int
    longest_tick_ns: int
    episodes: int


@dataclass(slots=True)
class _TickOverrunCounter:
    """One run's late-tick counts; changed under the tick host's condition."""

    late_ticks: int = 0
    longest_tick_ns: int = 0
    episodes: int = 0
    lines_written: int = 0
    # The open episode (consecutive late ticks); 0 late ticks = none open.
    episode_late_ticks: int = 0
    episode_longest_ns: int = 0

    def line_locked(
        self, program_run_id: int, period_ns: int, *, episode_ended: bool
    ) -> tuple[object, ...] | None:
        """The arguments of one episode line, or None when the run's lines are used up."""

        if self.lines_written >= _MAX_LATE_TICK_LINES_PER_RUN:
            return None
        self.lines_written += 1
        return (
            PROGRAM_TICKS_LATE_CODE,
            program_run_id,
            period_ns,
            self.episode_late_ticks,
            self.episode_longest_ns,
            int(episode_ended),
            self.late_ticks,
            self.longest_tick_ns,
            self.episodes,
            self.lines_written,
            _MAX_LATE_TICK_LINES_PER_RUN,
        )

    def end_episode_locked(
        self, program_run_id: int, period_ns: int
    ) -> tuple[object, ...] | None:
        if self.episode_late_ticks == 0:
            return None
        line = self.line_locked(program_run_id, period_ns, episode_ended=True)
        self.episode_late_ticks = 0
        self.episode_longest_ns = 0
        return line


def _write_late_tick_line(line: tuple[object, ...] | None) -> None:
    if line is None:
        return
    _LOGGER.warning(
        "manyforge_fault code=%s program_run_id=%d period_ns=%d late_ticks=%d "
        "longest_tick_ns=%d episode_ended=%d late_ticks_total=%d "
        "longest_tick_ns_total=%d episodes_total=%d line=%d max_lines=%d",
        *line,
    )


@dataclass(slots=True)
class _InterventionEntry:
    record: ProgramInterventionSnapshot
    resolution: tuple[str, object] | None = None


@dataclass(slots=True)
class _RunInterventions:
    node_ids: frozenset[str]
    entries: dict[str, _InterventionEntry] = field(default_factory=dict)


class ProgramInterventionRegistry:
    """One current attempt per captured node, retired with its Program run."""

    def __init__(self, *, maximum_retained_runs: int = _MAX_SCHEDULED_RUNS) -> None:
        if (
            type(maximum_retained_runs) is not int
            or not 1 <= maximum_retained_runs <= _MAX_SCHEDULED_RUNS
        ):
            raise ValueError("intervention retention is outside its bound")
        self._maximum_retained_runs = maximum_retained_runs
        self._runs: dict[str, _RunInterventions] = {}
        self._lock = RLock()

    def register_run(
        self, program_run_id: int, node_instance_ids: Collection[str]
    ) -> None:
        require_run_number(program_run_id)
        if not node_instance_ids or any(
            not isinstance(node, str) or not node for node in node_instance_ids
        ):
            raise ValueError("interventions require captured Program nodes")
        nodes = frozenset(node_instance_ids)
        with self._lock:
            if program_run_id in self._runs:
                raise ProgramSchedulingError(
                    "Program intervention capture is already registered"
                )
            if len(self._runs) >= self._maximum_retained_runs:
                raise ProgramSchedulingError("intervention retention is full")
            self._runs[program_run_id] = _RunInterventions(nodes)

    def _run(self, identity: NodeAttemptIdentity) -> _RunInterventions:
        run = self._runs.get(identity.program_run_id)
        if run is None or identity.node_instance_id not in run.node_ids:
            raise ProgramSchedulingError(
                "intervention identifies an uncaptured Program node"
            )
        return run

    def _entry(self, identity: NodeAttemptIdentity) -> _InterventionEntry:
        entry = self._run(identity).entries.get(identity.node_instance_id)
        if entry is None or entry.record.identity != identity:
            raise ProgramSchedulingError("Program recovery identifies a stale attempt")
        return entry

    def require(
        self,
        identity: NodeAttemptIdentity,
        *,
        reason_code: str,
        message: str,
        policy_parameter: str,
        owner_port_name: str | None = None,
        owner_task_id: int | str | None = None,
        owner_assessment: Mapping[str, object] | None = None,
        details: Mapping[str, object] | None = None,
        origin: Mapping[str, object] | None = None,
    ) -> ProgramInterventionSnapshot:
        record = ProgramInterventionSnapshot(
            identity=identity,
            reason_code=reason_code,
            message=message,
            policy_parameter=policy_parameter,
            owner_port_name=owner_port_name,
            owner_task_id=owner_task_id,
            owner_assessment=owner_assessment,
            details=details,
            origin=origin,
        )
        with self._lock:
            run = self._run(identity)
            current = run.entries.get(identity.node_instance_id)
            if current is not None:
                if current.record != record:
                    raise ProgramSchedulingError(
                        "Program node already requires another intervention"
                    )
                return current.record
            run.entries[identity.node_instance_id] = _InterventionEntry(record)
            return record

    def read(self, program_run_id: int) -> ProgramInterventionSnapshot | None:
        require_run_number(program_run_id)
        with self._lock:
            run = self._runs.get(program_run_id)
            if run is None:
                return None
            # Preserve the single-focus presentation. An ordinary intervention
            # must not be hidden by an operator Stop that allows ticking.
            first = None
            for entry in run.entries.values():
                if entry.resolution is not None:
                    continue
                if entry.record.policy_parameter != "operator_stop_policy":
                    return entry.record
                if first is None:
                    first = entry.record
            return first

    def read_holds(
        self, program_run_id: int
    ) -> tuple[ProgramInterventionSnapshot, ...]:
        """Every unresolved failure hold of the run, in arrival order.

        Each held node is its own hold (operator Stops are not listed); the
        runner publishes one offer per hold.
        """
        with self._lock:
            run = self._runs.get(program_run_id)
            if run is None:
                return ()
            return tuple(
                entry.record
                for entry in run.entries.values()
                if entry.resolution is None
                and entry.record.policy_parameter != "operator_stop_policy"
            )

    def has_operator_stop(self, program_run_id: int) -> bool:
        """Scheduling query without allocating an all-entry projection."""
        with self._lock:
            run = self._runs.get(program_run_id)
            return run is not None and any(
                entry.record.policy_parameter == "operator_stop_policy"
                for entry in run.entries.values()
            )

    def read_all(self, program_run_id: int) -> tuple[ProgramInterventionSnapshot, ...]:
        with self._lock:
            run = self._runs.get(program_run_id)
            return (
                ()
                if run is None
                else tuple(entry.record for entry in run.entries.values())
            )

    def refresh_owner_assessment(
        self,
        identity: NodeAttemptIdentity,
        assessment: Mapping[str, object],
    ) -> ProgramInterventionSnapshot:
        with self._lock:
            entry = self._entry(identity)
            updated = replace(entry.record, owner_assessment=assessment)
            entry.record = updated
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
            entry = self._entry(identity)
            resolution = (action, deepcopy(result))
            if entry.resolution is not None and entry.resolution != resolution:
                raise ProgramSchedulingError(
                    "Program recovery already has another resolution"
                )
            entry.resolution = resolution

    def consume_resolution(
        self, identity: NodeAttemptIdentity
    ) -> tuple[str, object] | None:
        with self._lock:
            run = self._run(identity)
            entry = run.entries.get(identity.node_instance_id)
            if entry is None:
                return None
            if entry.record.identity != identity:
                raise ProgramSchedulingError(
                    "Program recovery resolution identifies another attempt"
                )
            if entry.resolution is None:
                return None
            action, result = entry.resolution
            del run.entries[identity.node_instance_id]
            return action, deepcopy(result)

    def release(self, program_run_id: int) -> None:
        require_run_number(program_run_id)
        with self._lock:
            self._runs.pop(program_run_id, None)


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
        self._inflight: dict[str, tuple[int, Future[ProgramRunSnapshot]]] = {}
        self._schedule_revision = 0
        # Per run: the latest Kernel hold facts, applied on the run's next
        # tick (the current run and at most one ending run).
        self._hold_facts: dict[int, ProgramRunHoldFacts] = {}
        # Runs whose ticks are enabled: scheduled at least once. Hold facts
        # wake only these; a run started with `defer_ticks` keeps its facts
        # for its first tick until its explicit `schedule`.
        self._ticks_enabled: set[int] = set()
        # Per run: its late ticks (OD-213), kept until the run is forgotten.
        self._tick_period_ns = round(self._tick_interval_s * 1e9)
        self._tick_overruns: dict[int, _TickOverrunCounter] = {}
        # End-of-run work handed off by the scheduler thread: run one at a
        # time on the tick pool, in hand-off order, by one drain job.
        self._terminal_work: deque[ProgramRunSnapshot] = deque()
        self._terminal_draining = False
        # Per run: hand-offs whose end-of-run work has not completed yet.
        self._terminal_pending: dict[int, int] = {}
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

    def submit_run_work(self, work: Callable[[], None]) -> Future[None]:
        """Run one Run-owned side task (BB key publication) on the tick pool."""

        with self._condition:
            self._raise_if_unavailable()
        return self._executor.submit(work)

    def schedule(self, program_run_id: int) -> None:
        require_run_number(program_run_id)
        with self._condition:
            self._raise_if_unavailable()
            self._schedule_locked(program_run_id)

    def _schedule_locked(self, program_run_id: int) -> None:
        if (
            program_run_id not in self._scheduled
            and len(self._scheduled) >= self._maximum_scheduled_runs
        ):
            raise ProgramSchedulingError("scheduled Program-run set is full")
        self._schedule_revision += 1
        self._scheduled[program_run_id] = self._schedule_revision
        self._ticks_enabled.add(program_run_id)
        self._condition.notify_all()

    def start_run(
        self,
        admission: ProgramRunAdmission,
        *,
        program_run_id: int | None = None,
        defer_ticks: bool = False,
    ) -> ProgramRunSnapshot:
        snapshot = self._owner.start(
            admission,
            program_run_id=program_run_id,
        )
        if not defer_ticks:
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
            # The run could not enter the scheduled set, so no tick takes it:
            # its Stop, tree part included, runs here.
            program_run_id = snapshot.capture.program_run_id
            try:
                cleaned = self._owner.finish_stop(
                    program_run_id,
                    self._owner.send_stop(
                        program_run_id, reason="Program tick scheduling failed"
                    ),
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
        program_run_id: int,
        *,
        reason: str | None = None,
        stop_request_key: bytes | None = None,
    ) -> ProgramRunSnapshot:
        """Forced Program End (the runner's internal run-ending operation):
        marked and sent at once, even while a tick is in progress; then its
        tree part is applied by the run's tick."""
        snapshot = self._stop_through_tick(
            program_run_id, reason=reason, stop_request_key=stop_request_key
        )
        if snapshot.state in {
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }:
            self.schedule(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def _stop_through_tick(
        self,
        program_run_id: int,
        *,
        reason: str | None,
        stop_request_key: bytes | None,
    ) -> ProgramRunSnapshot:
        sent = self._owner.send_stop(
            program_run_id, reason=reason, stop_request_key=stop_request_key
        )
        return self._through_tick(
            program_run_id, lambda: self._owner.finish_stop(program_run_id, sent)
        )

    def _through_tick(self, program_run_id: int, apply: Callable[[], Any]) -> Any:
        """Hand one command to the run's tick and wait for its result
        (OD-146). Its refusal or error is raised here, as it was raised by
        the command itself."""

        intent = self._owner.post_to_tick(program_run_id, apply)
        try:
            self.schedule(program_run_id)
        except Exception:
            if self._owner.withdraw_from_tick(program_run_id, intent):
                raise
            # A tick in progress took it already: its result is the answer.
        return self._await_tick_result(intent)

    @staticmethod
    def _await_tick_result(intent: RunIntent) -> Any:
        """The one wait of a request for the run's tick (OD-146). It waits
        without a limit, as the request waited for the run's lock before."""

        intent.done.wait()
        if intent.error is not None:
            raise intent.error
        return intent.result

    def resume_motion(
        self,
        program_run_id: int,
        *,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        try:
            return self._through_tick(
                program_run_id,
                lambda: self._owner.resume_motion(
                    program_run_id,
                    expected_hold_revision=expected_hold_revision,
                    request_key=request_key,
                    group_id=group_id,
                    reason=reason,
                ),
            )
        finally:
            # Cleanup or an explicit reconciliation may have changed the
            # current view even when the release reply was lost. No retries.
            self._observe_after_motion_control(program_run_id)

    def start_motion(
        self,
        program_run_id: int,
        *,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Release one initially Ready selection, then wake its Program."""

        try:
            return self._through_tick(
                program_run_id,
                lambda: self._owner.start_motion(
                    program_run_id,
                    expected_hold_revision=expected_hold_revision,
                    request_key=request_key,
                    group_id=group_id,
                    reason=reason,
                ),
            )
        finally:
            # The release may have completed even if its reply was lost.
            # Observe once and never retry the effect.
            self._observe_after_motion_control(program_run_id)

    def prepare_manual_handover(
        self,
        program_run_id: int,
        *,
        node_indices: tuple[int, ...],
        expected_run_revision: int,
        expected_hold_revision: int,
        request_key: bytes,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        try:
            return self._through_tick(
                program_run_id,
                lambda: self._owner.prepare_manual_handover(
                    program_run_id,
                    node_indices=node_indices,
                    expected_run_revision=expected_run_revision,
                    expected_hold_revision=expected_hold_revision,
                    request_key=request_key,
                    reason=reason,
                ),
            )
        finally:
            self._observe_after_motion_control(program_run_id)

    def retry_failure_pause(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        action: str | None,
        request_key: bytes,
        reason: str | None,
    ) -> ProgramRunSnapshot:
        """Retry of one failure hold, applied by the run's tick."""

        return self._through_tick(
            program_run_id,
            lambda: self._owner.retry_failure_pause(
                program_run_id,
                node_attempt_id=node_attempt_id,
                action=action,
                request_key=request_key,
                reason=reason,
            ),
        )

    def continue_failure_pause(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        request_key: bytes,
        reason: str | None,
    ) -> ProgramRunSnapshot:
        """Continue of one failure hold, applied by the run's tick."""

        return self._through_tick(
            program_run_id,
            lambda: self._owner.continue_failure_pause(
                program_run_id,
                node_attempt_id=node_attempt_id,
                request_key=request_key,
                reason=reason,
            ),
        )

    def _observe_after_motion_control(self, program_run_id: int) -> None:
        snapshot = self._owner.read(program_run_id)
        if snapshot.state in {
            ProgramRunState.RUNNING,
            ProgramRunState.INTERVENTION_REQUIRED,
        }:
            self.schedule(program_run_id)
        self._observe_state(snapshot)

    def apply_hold_facts(self, facts: ProgramRunHoldFacts) -> None:
        """Keep the Kernel's latest hold facts for their run and wake it.

        Called on the runner's event-stream follower thread; memory only (no
        owner transition, no Kernel or journal call). The run's next tick
        reconciles the owner against these facts (``advance_once``). Facts
        for a run the owner does not hold yet, or holds but has not been
        scheduled yet (started with ``defer_ticks``), are kept and applied on
        its first tick; facts of earlier runs the owner no longer holds are
        dropped.
        """

        if not isinstance(facts, ProgramRunHoldFacts):
            raise TypeError("apply_hold_facts requires ProgramRunHoldFacts")
        run_id = facts.program_run_id
        with self._condition:
            self._hold_facts[run_id] = facts
            earlier = tuple(run for run in self._hold_facts if run != run_id)
        retired = tuple(run for run in earlier if not self._owner.has_run(run))
        held = self._owner.has_run(run_id)
        with self._condition:
            for run in retired:
                self._hold_facts.pop(run, None)
            # A run started with `defer_ticks` is not ticked before its
            # explicit `schedule`; its first tick applies the kept facts.
            enabled = run_id in self._ticks_enabled
        if held and enabled:
            self.schedule(run_id)

    def _advance(self, program_run_id: int) -> ProgramRunSnapshot:
        """One tick of the run with its latest Kernel hold facts.

        The tick's start and end are measured against the tick period; a tick
        that ends after its period is counted, never stopped or failed
        (OD-213: its cause is removed, a tick is not limited by a timer).
        """

        with self._condition:
            facts = self._hold_facts.get(program_run_id)
        started = self._monotonic()
        snapshot = self._owner.advance_once(program_run_id, hold_facts=facts)
        self._count_tick(program_run_id, self._monotonic() - started)
        return snapshot

    def _count_tick(self, program_run_id: int, duration_s: float) -> None:
        """Count a late tick; one line per episode of consecutive late ticks."""

        duration_ns = round(duration_s * 1e9)
        late = duration_ns > self._tick_period_ns
        with self._condition:
            counter = self._tick_overruns.get(program_run_id)
            if not late:
                line = (
                    None
                    if counter is None
                    else counter.end_episode_locked(
                        program_run_id, self._tick_period_ns
                    )
                )
            else:
                if counter is None:
                    counter = _TickOverrunCounter()
                    self._tick_overruns[program_run_id] = counter
                if counter.episode_late_ticks == 0:
                    counter.episodes += 1
                counter.late_ticks += 1
                counter.longest_tick_ns = max(counter.longest_tick_ns, duration_ns)
                counter.episode_late_ticks += 1
                counter.episode_longest_ns = max(
                    counter.episode_longest_ns, duration_ns
                )
                line = (
                    counter.line_locked(
                        program_run_id, self._tick_period_ns, episode_ended=False
                    )
                    if counter.episode_late_ticks % _LATE_TICK_LINE_EVERY == 0
                    else None
                )
        _write_late_tick_line(line)

    def _end_tick_episode_locked(
        self, program_run_id: int
    ) -> tuple[object, ...] | None:
        """The run left the scheduled set: its open episode ends."""

        counter = self._tick_overruns.get(program_run_id)
        if counter is None:
            return None
        return counter.end_episode_locked(program_run_id, self._tick_period_ns)

    def read_tick_overruns(self, program_run_id: int) -> ProgramRunTickOverruns:
        """The run's late ticks so far (memory only)."""

        with self._condition:
            counter = self._tick_overruns.get(program_run_id)
            if counter is None:
                return ProgramRunTickOverruns(0, 0, 0)
            return ProgramRunTickOverruns(
                counter.late_ticks, counter.longest_tick_ns, counter.episodes
            )

    def resume_halt(
        self,
        program_run_id: int,
        *,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Execute after Halt: release the Kernel's freeze, then make the run
        schedulable."""

        with self._condition:
            self._raise_if_unavailable()
            if program_run_id not in self._scheduled and (
                len(self._scheduled) >= self._maximum_scheduled_runs
            ):
                raise ProgramSchedulingError("scheduled Program-run set is full")
        # Provider recovery may block; it must not hold the scheduler mutex
        # needed to publish/schedule a newer priority Stop. Admission and
        # Resume share the existing containment-on-scheduling-failure path.
        snapshot = self._through_tick(
            program_run_id,
            lambda: self._owner.resume_halt(program_run_id, reason=reason),
        )
        self._schedule_or_stop(snapshot)
        self._observe_state(snapshot)
        return snapshot

    def contain_reset_run(
        self, program_run_id: int, *, stop_request_key: bytes
    ) -> ProgramRunSnapshot:
        """Use existing workers/notifications to observe terminal Reset's Stop.
        It waits until containment settles (see `wait_terminal`)."""
        self._through_tick(
            program_run_id,
            lambda: self._owner.begin_reset_containment(
                program_run_id, stop_request_key=stop_request_key
            ),
        )
        try:
            self.schedule(program_run_id)
            return self.wait_terminal(
                program_run_id, require_reset_containment=True
            )
        except ProgramSchedulingError as error:
            raise ProgramRunOwnerOutcomeUnknownError(
                "Reset containment wait did not confirm settlement"
            ) from error

    def reset_run(self, program_run_id: int) -> ProgramRunSnapshot:
        """Release the exact terminal run's resident owner effects."""

        # Not handed to the tick: it touches no tree, and a tick scheduled
        # for it could make the ``forget_reset_run`` that follows refuse the
        # run as still scheduled.
        snapshot = self._owner.reset_effects(program_run_id)
        self._observe_state(snapshot)
        return snapshot

    def reset_retained_run(self, program_run_id: int) -> None:
        """Reset a proven terminal run whose behavior-tree owner rotated."""

        self._owner.reset_retained_effects(program_run_id)

    def forget_reset_run(self, program_run_id: int) -> None:
        """Release scheduler presentation state and the reset resident tree."""

        with self._condition:
            if program_run_id in self._scheduled or program_run_id in self._inflight:
                raise ProgramSchedulingError(
                    "a scheduled Program run cannot be forgotten"
                )
            # The run's end-of-run work runs on the tick pool. It completes
            # before its run is forgotten: Reset holds the runner's command
            # slot until this returns, so no next run starts before that work
            # (the run end, rules back to start, the journal's terminal fact)
            # has run, as when the scheduler thread did it itself.
            while program_run_id in self._terminal_pending:
                if self._shutdown:
                    raise ProgramSchedulingError("Program tick host is shut down")
                self._condition.wait()
        self._owner.forget_reset_run(program_run_id)
        with self._condition:
            self._observed_revisions.pop(program_run_id, None)
            self._hold_facts.pop(program_run_id, None)
            self._ticks_enabled.discard(program_run_id)
            self._tick_overruns.pop(program_run_id, None)

    def resume_recovery(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        action: str,
        result: object = None,
    ) -> ProgramRunSnapshot:
        require_run_number(program_run_id)
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

    def read(self, program_run_id: int) -> ProgramRunSnapshot:
        return self._owner.read(program_run_id)

    def wait_terminal(
        self,
        program_run_id: int,
        *,
        require_reset_containment: bool = False,
    ) -> ProgramRunSnapshot:
        """Wait on owner progress notifications, never by polling its state.

        No time limit (OD-214): the wait ends when the run settles, which a
        Stop also brings about, or when this host shuts down or a tick worker
        fails. A lost execution host ends the application (its exit), and a
        late one is Halted by the Manager's status probe.
        """

        with self._condition:
            while True:
                self._raise_if_unavailable()
                snapshot = self._owner.read(program_run_id)
                if snapshot.state in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                }:
                    if not require_reset_containment:
                        return snapshot
                    settled = self._owner.reset_containment_settled(program_run_id)
                    if (
                        settled
                        and program_run_id not in self._scheduled
                        and program_run_id not in self._inflight
                    ):
                        return snapshot
                self._condition.wait()

    def _requires_advance(self, snapshot: ProgramRunSnapshot) -> bool:
        if snapshot.state in {
            ProgramRunState.RUNNING,
            ProgramRunState.HALTING,
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }:
            return True
        if snapshot.state in {
            ProgramRunState.STOPPED,
            ProgramRunState.SUCCEEDED,
            ProgramRunState.FAILED,
        }:
            return self._owner.reset_containment_pending(
                snapshot.capture.program_run_id
            )
        return (
            snapshot.state is ProgramRunState.INTERVENTION_REQUIRED
            and (
                self._owner.operator_stop_observation_required(
                    snapshot.capture.program_run_id
                )
                or self._owner.failure_pause_stop_pending(
                    snapshot.capture.program_run_id
                )
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
                    self._advance,
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
        lines = []
        if remove:
            with self._condition:
                for program_run_id, schedule_revision in remove:
                    if self._scheduled.get(program_run_id) == schedule_revision:
                        self._scheduled.pop(program_run_id, None)
                        lines.append(self._end_tick_episode_locked(program_run_id))
        for line in lines:
            _write_late_tick_line(line)
        try:
            self._observe_terminals(snapshots)
        finally:
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

    def _observes_terminal(self, snapshot: ProgramRunSnapshot) -> bool:
        return self._terminal_observer is not None and snapshot.state not in {
            ProgramRunState.RUNNING,
            ProgramRunState.INTERVENTION_REQUIRED,
            ProgramRunState.HALTING,
            ProgramRunState.HALTED,
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }

    def _observe_terminals(self, snapshots: list[ProgramRunSnapshot]) -> None:
        observer = self._terminal_observer
        if observer is None:
            return
        try:
            for snapshot in snapshots:
                if self._observes_terminal(snapshot):
                    observer(snapshot)
        except Exception as error:
            with self._condition:
                self._worker_failure = error
                self._condition.notify_all()
            raise ProgramSchedulingError("Program terminal observer failed") from error

    def _hand_off_terminal_locked(self, snapshot: ProgramRunSnapshot) -> None:
        """Scheduler thread: queue one run's end-of-run work for the pool.

        Memory only. One drain job runs the queue in hand-off order, so the
        work runs once per hand-off and one at a time, as on this thread
        before (OD-213).
        """

        program_run_id = snapshot.capture.program_run_id
        self._terminal_work.append(snapshot)
        self._terminal_pending[program_run_id] = (
            self._terminal_pending.get(program_run_id, 0) + 1
        )
        if not self._terminal_draining:
            self._terminal_draining = True
            self._executor.submit(self._drain_terminal_work)

    def _drain_terminal_work(self) -> None:
        """Tick pool: run the queued end-of-run work, then its state wake."""

        failed = False
        while True:
            with self._condition:
                if not self._terminal_work:
                    self._terminal_draining = False
                    return
                snapshot = self._terminal_work.popleft()
            try:
                if not failed:
                    self._observe_terminals([snapshot])
            except ProgramSchedulingError:
                # Latched as the worker failure: the scheduler thread ends and
                # no later end-of-run work runs, as when this thread was it.
                failed = True
            finally:
                self._observe_state(snapshot)
                program_run_id = snapshot.capture.program_run_id
                with self._condition:
                    remaining = self._terminal_pending[program_run_id] - 1
                    if remaining:
                        self._terminal_pending[program_run_id] = remaining
                    else:
                        del self._terminal_pending[program_run_id]
                    self._condition.notify_all()

    def _run(self) -> None:
        next_tick = self._monotonic()
        while True:
            handed_off = False
            observed: list[ProgramRunSnapshot] = []
            lines: list[tuple[object, ...] | None] = []
            with self._condition:
                if self._shutdown or self._worker_failure is not None:
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
                    if not self._requires_advance(snapshot):
                        removed = (
                            self._scheduled.get(program_run_id) == schedule_revision
                        )
                        if removed:
                            self._scheduled.pop(program_run_id, None)
                            lines.append(
                                self._end_tick_episode_locked(program_run_id)
                            )
                        if removed and self._observes_terminal(snapshot):
                            # Its end-of-run work and then its state wake
                            # run on the pool; this thread only hands off.
                            self._hand_off_terminal_locked(snapshot)
                            handed_off = True
                            continue
                    observed.append(snapshot)

                now = self._monotonic()
                delay = next_tick - now
                if observed or handed_off:
                    self._condition.notify_all()
                elif not self._scheduled:
                    self._condition.wait()
                elif delay > 0.0:
                    self._condition.wait(timeout=delay)
                else:
                    for program_run_id, schedule_revision in sorted(
                        self._scheduled.items()
                    ):
                        if program_run_id in self._inflight:
                            continue
                        future = self._executor.submit(
                            self._advance,
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
            for line in lines:
                _write_late_tick_line(line)
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
        # No tick runs any more: nothing still queued on a run's sender is sent.
        self._owner.close_run_senders()


__all__ = [
    "ProgramInterventionRegistry",
    "ProgramRunHoldFacts",
    "RUN_FROZEN_LEVELS",
    "ProgramRunTickHost",
    "ProgramSchedulingError",
]
