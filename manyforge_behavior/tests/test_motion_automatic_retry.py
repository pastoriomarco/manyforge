"""Automatic motion retry and the motion operation lifecycle (spec 400 §7,
spec 410 §4.1, DX-1169 close_operation, contract items 25-41)."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import py_trees
import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.nodes import v2_owner
from manyforge_behavior.nodes.v2_owner import MotionTaskAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationRefusedByPriorityError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.recovery import RecoveryAwareNode

_ROOT = Path(__file__).resolve().parents[2]
_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_FAILURE = py_trees.common.Status.FAILURE
_INVALID = py_trees.common.Status.INVALID


class _Time:
    """Program time the test controls; the ProgramClock reads it."""

    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class _Log:
    """One ordered record of owner-visible effects across the fakes."""

    def __init__(self) -> None:
        self.entries: list[tuple[Any, ...]] = []


class _MotionPort:
    """Scripted kernel-operations port: one snapshot per start, optional
    observations, and scripted close outcomes (True, False or an exception)."""

    def __init__(
        self,
        starts: list[OwnerTaskSnapshot | BaseException],
        *,
        observes: list[OwnerTaskSnapshot] | None = None,
        close_effects: list[Any] | None = None,
        log: _Log | None = None,
    ) -> None:
        self._starts = list(starts)
        self._observes = list(observes or [])
        self._close_effects = list(close_effects or [])
        self.log = log or _Log()
        self.starts: list[tuple[NodeAttemptIdentity, Any]] = []
        self.observations = 0
        self.acknowledged: list[str] = []
        self.cancelled: list[str] = []
        self.closes: list[dict[str, Any]] = []
        self.released: list[str] = []
        self._task: OwnerTaskSnapshot | None = None

    def start(self, node_id, identity, inputs) -> OwnerTaskSnapshot:
        self.starts.append((identity, inputs))
        self.log.entries.append(("start",))
        index = min(len(self.starts), len(self._starts)) - 1
        scripted = self._starts[index]
        if isinstance(scripted, BaseException):
            raise scripted
        self._task = replace(scripted, task_id=f"task-{len(self.starts)}")
        return self._task

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations += 1
        if self._observes:
            self._task = replace(self._observes.pop(0), task_id=task_id)
        return self._task

    def cancel(self, task_id, identity) -> None:
        self.cancelled.append(task_id)
        self.log.entries.append(("cancel",))

    def acknowledge(self, task_id, identity) -> None:
        self.acknowledged.append(task_id)

    def close(self, task_id, identity, *, request_id) -> bool:
        self.closes.append({"task_id": task_id, "request_id": request_id, "identity": identity})
        self.log.entries.append(("close",))
        effect = self._close_effects.pop(0) if self._close_effects else True
        if isinstance(effect, BaseException):
            raise effect
        return effect

    def release_preempted(self, task_id, identity) -> None:
        self.released.append(task_id)


class _Interventions:
    def __init__(self, log: _Log) -> None:
        self._log = log
        self.required: list[dict[str, Any]] = []
        self.resolution = None

    def require(self, identity, **kwargs) -> None:
        self.required.append(kwargs)
        self._log.entries.append(("require",))

    def consume_resolution(self, identity):
        resolution, self.resolution = self.resolution, None
        return resolution


class _Attention:
    def __init__(self, log: _Log, *, blocked: bool = False) -> None:
        self._log = log
        self.blocked = blocked
        self.calls: list[tuple[str, str]] = []

    def __call__(self, node_instance_id: str, request_id: str) -> None:
        self.calls.append((node_instance_id, request_id))
        self._log.entries.append(("attention", node_instance_id))
        if self.blocked:
            raise RuntimeError("operator alarm gates these resources")


class _Hold:
    """Operator-Stop hold seconds, as the run's Stop state reports them."""

    def __init__(self) -> None:
        self.held = 0.0

    def held_seconds(self, scope: str) -> float:
        return self.held

    def refresh(self) -> None:
        pass

    def note_task_observation_change(self) -> None:
        pass

    def observation_error(self):
        return None


class _Store:
    def __init__(self, *, refuse: bool = False) -> None:
        self.writes: list[tuple[str, Any, dict[str, Any]]] = []
        self._refuse = refuse

    def write_node_outcome(self, node_instance_id, outcome, *, metadata=None) -> None:
        key = f"node_outcome.{node_instance_id}"
        if self._refuse:
            raise KeyError(f"blackboard key {key!r} is undeclared")
        self.writes.append((key, outcome, dict(metadata or {})))


class _Resolver:
    """Resolved parameters: authored values, then value-source results."""

    def __init__(self, resolved: dict[str, Any] | None = None, store: _Store | None = None) -> None:
        self._resolved = dict(resolved or {})
        self.value_store = store or _Store()

    def snapshot(self, *, node_id, node_instance_id, parameters, value_sources, blackboard_read_parameters=()):
        return SimpleNamespace(parameters={**parameters, **self._resolved}, blackboard={})


class _Attempts:
    def __init__(self) -> None:
        self.count = 0

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        self.count += 1
        return NodeAttemptIdentity(
            program_run_id="run.motion",
            node_instance_id=node_instance_id,
            node_attempt_id=f"attempt.{self.count}",
        )


def _assessment(
    *,
    eligible: bool = True,
    cause: str = "planning_no_effect",
    status: str = "retry_ready",
    reason: str | None = None,
    eligible_close: bool = True,
    execution_state: str = "Failed",
) -> dict[str, Any]:
    if not eligible:
        cause = "not_eligible"
        reason = reason or "failure_cause_not_retryable"
    return {
        "recovery_class": "planning_failed",
        "status": status,
        "execution_state": execution_state,
        "allowed_actions": ["retry_after_scene_repair"],
        "eligible_close": eligible_close,
        "retry_eligibility": {
            "eligible": eligible,
            "cause": cause,
            "effect": "known_no_effect",
            "reason": reason,
        },
    }


_FACT = {
    "operation": {"source_owner_epoch": "kernel.1", "operation_id": "op", "operation_revision": 3},
    "resource_ids": ["arm"],
    "failure": {"code": "planning_failed", "message": "no feasible path"},
    "terminal_facts": [{"kind": "local_failure", "effect": "no_effect"}],
}


def _failed(
    assessment: dict[str, Any] | None = None,
    *,
    pause: bool = False,
    state: OwnerTaskState = OwnerTaskState.FAILED,
) -> OwnerTaskSnapshot:
    metadata: dict[str, Any] = {} if assessment is None else {"recovery_assessment": assessment}
    metadata["operation_id"] = "op"  # An admitted Kernel operation.
    metadata["operation_failure"] = _FACT
    if pause:
        metadata.update(
            authored_failure_policy="pause_for_operator",
            authored_failure_policy_parameter="no_feasible_plan_policy",
        )
    return OwnerTaskSnapshot(
        task_id="task",
        state=state,
        metadata=metadata,
        reason_code="planning_failed",
        message="no feasible path",
    )


def _active(**metadata: Any) -> OwnerTaskSnapshot:
    return OwnerTaskSnapshot(
        task_id="task", state=OwnerTaskState.ACTIVE, metadata={"operation_id": "op", **metadata}
    )


def _succeeded() -> OwnerTaskSnapshot:
    return OwnerTaskSnapshot(task_id="task", state=OwnerTaskState.SUCCEEDED)


def _motion(
    port: _MotionPort,
    clock: ProgramClock,
    *,
    node_id: str = "motion.move_to_pose",
    parameters: dict[str, Any] | None = None,
    resolved: dict[str, Any] | None = None,
    value_sources: dict[str, Any] | None = None,
    log: _Log | None = None,
    attention: _Attention | None = None,
    intervention: _Interventions | None = None,
    hold: _Hold | None = None,
    hold_scope: str | None = None,
    store: _Store | None = None,
) -> MotionTaskAdapter:
    audit: list[dict[str, Any]] = []
    adapter = MotionTaskAdapter(
        name="motion",
        node_id=node_id,
        node_instance_id="motion",
        attempt_source=_Attempts(),
        parameters=dict(parameters or {}),
        value_sources=dict(value_sources or {}),
        input_resolver=_Resolver(resolved, store),
        port=port,
        owner_port_name="kernel_operations",
        intervention_port=intervention,
        operator_stop_state=hold,
        program_clock=clock,
        operator_stop_hold_scope=hold_scope,
        motion_attention=attention or _Attention(log or _Log()),
        audit_sink=audit.append,
        owner_api="kernel.operation",
        cancellable=False,
    )
    adapter.audit_events = audit  # The sink's facts, in decision order.
    return adapter


def _clocked() -> tuple[_Time, ProgramClock]:
    time = _Time()
    return time, ProgramClock(time)


def _tick(adapter: MotionTaskAdapter) -> py_trees.common.Status:
    adapter.tick_once()
    return adapter.status


def test_eligible_failure_closes_before_the_wait_then_submits_a_fresh_execute() -> None:
    log = _Log()
    port = _MotionPort([_failed(_assessment()), _succeeded()], log=log)
    time, clock = _clocked()
    adapter = _motion(port, clock, log=log)

    assert _tick(adapter) is _RUNNING
    # Closed at once: nothing stays resident during the wait.
    assert [entry[0] for entry in log.entries] == ["start", "close"]
    assert adapter.cached_control_state() == "inactive"

    time.now = 0.5
    assert _tick(adapter) is _RUNNING
    assert len(port.starts) == 1

    time.now = 1.0
    assert _tick(adapter) is _SUCCESS  # Attention check, fresh Execute, success.
    assert [entry[0] for entry in log.entries] == ["start", "close", "attention", "start", ]
    assert port.acknowledged == ["task-2"]
    assert port.closes[0]["task_id"] == "task-1"
    assert port.closes[0]["request_id"].startswith("program-close:")
    assert port.starts[0][0].node_attempt_id != port.starts[1][0].node_attempt_id
    # The superseded per-attempt retry context is gone from the owner inputs.
    assert not hasattr(OwnerNodeInputs(parameters={}), "automatic_retry_context")
    assert tuple(adapter.audit_events) == (
        {
            "kind": "program_node_automatic_retry",
            "node_instance_id": "motion",
            "attempt": 2,
            "max_attempts": 4,
            "cause": "planning_no_effect",
            "delay_s": 1.0,
            "code": "planning_failed",
            "message": "no feasible path",
        },
    )


def test_exhausted_budget_closes_every_failed_attempt_and_returns_failure() -> None:
    port = _MotionPort([_failed(_assessment())])
    _time, clock = _clocked()
    adapter = _motion(port, clock, parameters={"automatic_retries": 2, "automatic_retry_delay_s": 0.0})

    for _ in range(10):
        if _tick(adapter) is _FAILURE:
            break
    assert adapter.status is _FAILURE
    assert len(port.starts) == 3  # One first attempt and two retries.
    assert [close["task_id"] for close in port.closes] == ["task-1", "task-2", "task-3"]
    assert adapter.failure_reason_code == "planning_failed"
    assert adapter.operation_failure == {**_FACT, "recovery_class": "planning_failed"}
    assert adapter.cached_control_state() == "inactive"


def test_ineligible_failure_with_an_authored_pause_holds_the_operation_live() -> None:
    log = _Log()
    port = _MotionPort([_failed(_assessment(eligible=False), pause=True)], log=log)
    _time, clock = _clocked()
    interventions = _Interventions(log)
    adapter = _motion(port, clock, intervention=interventions, log=log)

    assert _tick(adapter) is _RUNNING  # Paused for the operator, not retried.
    assert len(port.starts) == 1
    assert port.closes == []
    assert log.entries == [("start",), ("require",)]


def test_ineligible_failure_closes_before_failure_returns_to_the_tree() -> None:
    log = _Log()
    port = _MotionPort([_failed(_assessment(eligible=False))], log=log)
    _time, clock = _clocked()
    adapter = _motion(port, clock, log=log)

    assert _tick(adapter) is _FAILURE
    assert [entry[0] for entry in log.entries] == ["start", "close"]
    assert adapter.cached_control_state() == "inactive"


def test_a_failure_held_by_the_nodes_own_policy_keeps_its_operation_live() -> None:
    port = _MotionPort([_failed(_assessment(eligible=False))])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    adapter.hold_failures_for_intervention()

    assert _tick(adapter) is _FAILURE  # The wrapper holds it with owner actions.
    assert port.closes == []
    assert adapter.owner_recovery_context["owner_task_id"] == "task-1"
    assert adapter.operation_failure is not None


def test_an_operation_not_eligible_to_close_is_retained_and_never_retried() -> None:
    port = _MotionPort([_failed(_assessment(eligible_close=False))])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE  # Retry needs the close; Reset releases it.
    assert port.closes == []
    assert len(port.starts) == 1
    assert adapter.audit_events == []
    assert adapter.cached_control_state() == "unrecoverable"


def test_a_refused_close_before_the_wait_returns_the_failure_retained() -> None:
    port = _MotionPort([_failed(_assessment())], close_effects=[False])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE
    assert len(port.closes) == 1
    assert len(port.starts) == 1
    assert adapter.audit_events == []
    assert adapter.failure_reason_code == "planning_failed"


def test_an_unknown_close_outcome_replays_the_same_request() -> None:
    port = _MotionPort(
        [_failed(_assessment(eligible=False))],
        close_effects=[OwnerTaskSettlementPendingError("the reply was lost"), True],
    )
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _RUNNING  # The node waits on the exact pending close.
    assert _tick(adapter) is _FAILURE  # The same request resumes and is accepted.
    assert port.closes[0]["request_id"] == port.closes[1]["request_id"]
    assert adapter.failure_reason_code == "planning_failed"


@pytest.mark.parametrize(
    "parameters, resolved, retried",
    [
        ({"target_mode": "relative"}, {}, False),  # An offset from the robot's own pose.
        ({"target_mode": "relative", "target_reference": None}, {}, False),
        ({"target_mode": "relative", "target_reference": {"type": "scene"}}, {}, True),
        ({}, {"target_mode": "relative"}, False),  # Mode from a value source, resolved relative.
    ],
)
def test_the_item_seven_grammar_applies_to_the_resolved_attempt(parameters, resolved, retried) -> None:
    port = _MotionPort([_failed(_assessment()), _succeeded()])
    _time, clock = _clocked()
    adapter = _motion(port, clock, parameters=parameters, resolved=resolved)
    status = _tick(adapter)
    assert len(port.starts) == 1
    if retried:
        assert status is _RUNNING
        assert len(adapter.audit_events) == 1
    else:
        assert status is _FAILURE  # Closed and returned, never retried.
        assert adapter.audit_events == []


def test_relative_joint_move_is_never_retried() -> None:
    port = _MotionPort([_failed(_assessment())])
    _time, clock = _clocked()
    adapter = _motion(
        port,
        clock,
        node_id="motion.move_to_joint_target",
        parameters={"target_mode": "relative", "automatic_retries": 3},
    )
    assert _tick(adapter) is _FAILURE
    assert len(port.starts) == 1
    assert adapter.audit_events == []


def test_delay_counts_on_the_program_clock_and_stands_still_while_halted() -> None:
    port = _MotionPort([_failed(_assessment()), _succeeded()])
    time, clock = _clocked()
    adapter = _motion(port, clock)
    _tick(adapter)
    time.now = 0.5
    clock.freeze()  # Halt: nothing is resident, so Execute after Halt is free.
    assert adapter.reconcile_full_stop_task() == "inactive"
    time.now = 10.0
    assert _tick(adapter) is _RUNNING
    assert len(port.starts) == 1
    clock.resume()
    time.now = 10.25  # Program time: 0.5 before the Halt, 0.25 after it.
    assert _tick(adapter) is _RUNNING
    assert len(port.starts) == 1
    time.now = 10.5
    _tick(adapter)
    assert len(port.starts) == 2


def test_operator_stop_hold_pauses_the_wait_and_a_release_restarts_the_budget() -> None:
    port = _MotionPort([_failed(_assessment())])
    time, clock = _clocked()
    hold = _Hold()
    adapter = _motion(
        port, clock, hold=hold, hold_scope="motion",
        parameters={"automatic_retries": 1, "automatic_retry_delay_s": 1.0},
    )
    _tick(adapter)
    time.now = 0.5
    hold.held = 5.0  # The Stop holds the node for five seconds.
    time.now = 5.5
    adapter.observe_while_operator_stop_held()
    assert len(port.starts) == 1
    assert adapter.cached_control_state() == "inactive"  # Resume is not blocked.
    adapter.restart_retry_series()  # The operator's Resume released the hold.
    time.now = 6.0  # 1.0 s of Program time outside the hold has passed.
    _tick(adapter)
    assert len(port.starts) == 2
    # The released series has its full budget again: one more retry.
    _tick(adapter)
    assert [event["attempt"] for event in adapter.audit_events] == [2, 2]


def test_attention_block_after_the_close_fails_without_a_fresh_execute() -> None:
    log = _Log()
    port = _MotionPort([_failed(_assessment()), _succeeded()], log=log)
    time, clock = _clocked()
    attention = _Attention(log, blocked=True)
    adapter = _motion(port, clock, attention=attention, log=log)
    _tick(adapter)
    time.now = 1.0
    assert _tick(adapter) is _FAILURE
    assert len(port.starts) == 1
    assert [entry[0] for entry in log.entries] == ["start", "close", "attention"]
    assert attention.calls and attention.calls[0][0] == "motion"
    assert adapter.failure_reason_code == "motion_attention_blocked"


def test_operator_retry_after_exhaustion_starts_a_new_series_with_the_full_budget() -> None:
    log = _Log()
    port = _MotionPort(
        [_failed(_assessment(), pause=True), _failed(_assessment(), pause=True), _failed(_assessment(), pause=True), _succeeded()],
        log=log,
    )
    _time, clock = _clocked()
    interventions = _Interventions(log)
    adapter = _motion(
        port, clock, parameters={"automatic_retries": 1, "automatic_retry_delay_s": 0.0}, intervention=interventions, log=log
    )
    for _ in range(4):
        _tick(adapter)  # Automatic retry, then the exhausted failure pauses.
    assert len(port.starts) == 2
    assert len(interventions.required) == 1
    assert len(port.closes) == 1  # Only the retried attempt; the held one stays live.

    interventions.resolution = ("retry", None)  # The operator's Retry.
    _tick(adapter)  # Consumes the resolution: a new series starts.
    _tick(adapter)  # The fresh attempt is submitted and fails eligibly again.
    assert len(port.starts) == 3
    assert [event["attempt"] for event in adapter.audit_events] == [2, 2]
    for _ in range(4):
        if _tick(adapter) is _SUCCESS:
            break
    assert adapter.status is _SUCCESS


def test_a_failure_arriving_before_its_assessment_settles_waits_read_only() -> None:
    unsettled = _failed(None)
    in_flight = _failed({**_assessment(), "retry_eligibility": {"eligible": False, "cause": "not_eligible", "effect": "effect_uncertain", "reason": "operation_in_flight"}})
    port = _MotionPort([unsettled], observes=[_failed({"status": "waiting", "allowed_actions": []}), in_flight, _failed(_assessment())])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _RUNNING  # The assessment is missing: observe again.
    assert _tick(adapter) is _RUNNING  # Waiting: still observing.
    assert port.closes == []
    assert _tick(adapter) is _RUNNING  # Now settled and eligible: closed, then waits.
    assert port.observations == 3
    assert len(port.closes) == 1
    assert len(adapter.audit_events) == 1


_STALE_PREDECESSORS = [
    pytest.param(
        {
            "status": "unavailable",
            "execution_state": "Executing",
            "allowed_actions": [],
            "eligible_close": False,
            "retry_eligibility": {
                "eligible": False,
                "cause": "not_eligible",
                "effect": "effect_uncertain",
                "reason": "not_retryable_state",
            },
        },
        id="unavailable-executing",
    ),
    pytest.param(
        {**_assessment(), "execution_state": "Planning"},
        id="retry-ready-planning",
    ),
]


@pytest.mark.parametrize("stale", _STALE_PREDECESSORS)
def test_a_present_predecessor_assessment_is_not_taken_as_final(stale) -> None:
    port = _MotionPort([_failed(stale)], observes=[_failed(stale), _failed(_assessment())])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _RUNNING  # The cached predecessor describes another execution.
    assert port.closes == []
    assert _tick(adapter) is _RUNNING  # The failed execution's own assessment: decided.
    assert port.observations == 2
    assert len(port.closes) == 1
    assert len(adapter.audit_events) == 1


def test_a_malformed_assessment_read_is_surfaced_as_the_owner_failure() -> None:
    malformed = OwnerTaskSnapshot(
        task_id="task",
        state=OwnerTaskState.FAILED,
        metadata={
            "operation_id": "op",
            "recovery_assessment_failure": {
                "reason_code": "owner_recovery_assessment_invalid",
                "message": "headless recovery assessment has no owner projection",
            }
        },
        reason_code="planning_failed",
        message="no feasible path",
    )
    port = _MotionPort([malformed])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE  # Not waited for: a permanent read failure.
    assert adapter.failure_reason_code == "owner_recovery_assessment_invalid"
    assert port.closes == []
    assert len(port.starts) == 1


def test_an_unknown_admission_is_never_closed_or_waited_for() -> None:
    unknown = OwnerTaskSnapshot(
        task_id="task",
        state=OwnerTaskState.FAILED,
        metadata={"request_id": "program-node:1"},
        reason_code="operation_admission_outcome_unknown",
        message="the reply was lost",
    )
    port = _MotionPort([unknown])
    _time, clock = _clocked()
    adapter = _motion(port, clock)  # The default budget: still no retry.

    assert _tick(adapter) is _FAILURE  # Retained for Reset's reconciliation.
    assert port.closes == []
    assert port.observations == 0


def test_the_retry_audit_fact_carries_the_failed_attempt_code_or_null() -> None:
    silent = OwnerTaskSnapshot(
        task_id="task",
        state=OwnerTaskState.FAILED,
        metadata={"recovery_assessment": _assessment(), "operation_id": "op"},
        reason_code="planning_failed",
        message="",
    )
    port = _MotionPort([silent, _succeeded()])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    _tick(adapter)
    (event,) = adapter.audit_events

    assert event["code"] == "planning_failed"
    assert event["message"] is None  # Absent, so never invented.
    assert event["cause"] == "planning_no_effect"


def test_a_run_ending_during_a_retry_wait_has_nothing_resident_to_report() -> None:
    port = _MotionPort([_failed(_assessment()), _succeeded()])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    _tick(adapter)
    assert len(port.closes) == 1

    adapter.stop(_INVALID)  # The operation was closed before the wait.

    assert port.cancelled == []
    assert len(port.closes) == 1
    assert not adapter.has_pending_drain
    assert adapter.status is _INVALID


def test_an_abandoned_active_move_is_cancelled_then_closed_before_a_new_execute() -> None:
    log = _Log()
    interrupted = _failed(
        _assessment(eligible=False, execution_state="Stopped"), state=OwnerTaskState.INTERRUPTED
    )
    port = _MotionPort(
        [_active(), _succeeded()],
        observes=[_active(), interrupted],
        log=log,
    )
    _time, clock = _clocked()
    adapter = _motion(port, clock, log=log)
    assert _tick(adapter) is _RUNNING

    adapter.stop(_INVALID)  # A Fallback/Parallel abandons the running move.
    assert port.cancelled == ["task-1"]
    assert adapter.has_pending_drain
    assert adapter.cached_control_state() == "active"  # Its Stop still settles.

    assert _tick(adapter) is _RUNNING  # Re-entered: still stopping, nothing submitted.
    assert len(port.starts) == 1
    assert _tick(adapter) is _SUCCESS  # Settled: closed, then the new Execute.
    assert [entry[0] for entry in log.entries] == ["start", "cancel", "close", "start"]
    assert port.closes[0]["task_id"] == "task-1"
    assert not adapter.has_pending_drain


def test_an_abandoned_move_not_eligible_to_close_is_left_for_reset() -> None:
    interrupted = _failed(
        _assessment(eligible=False, execution_state="Stopped", eligible_close=False),
        state=OwnerTaskState.INTERRUPTED,
    )
    port = _MotionPort([_active()], observes=[interrupted])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    _tick(adapter)
    adapter.stop(_INVALID)

    assert adapter.drain_invalidated() is True  # The run's per-tick drain.
    assert port.closes == []
    assert not adapter.has_pending_drain


def test_a_logically_preempted_move_ends_with_the_default_failure_and_its_outcome() -> None:
    store = _Store()
    preempted = _active(
        preemption={
            "kind": "preempted",
            "by": {"operation_id": "op.2", "node_instance_id": "other", "node_attempt_id": "a.2"},
        }
    )
    port = _MotionPort([_active()], observes=[preempted])
    _time, clock = _clocked()
    adapter = _motion(port, clock, store=store)
    _tick(adapter)

    assert _tick(adapter) is _FAILURE  # At acceptance; the Kernel owns the handover.
    assert adapter.failure_reason_code == "motion_preempted"
    assert port.closes == [] and port.cancelled == [] and port.acknowledged == []
    assert port.released == ["task-1"]
    # History records the preemption (no automatic retry was decided).
    assert [event["kind"] for event in adapter.audit_events] == ["program_node_preempted"]
    assert [(key, value) for key, value, _meta in store.writes] == [
        ("node_outcome.motion", "preempted")
    ]
    assert store.writes[0][2]["node_attempt_id"] == "attempt.1"


@pytest.mark.parametrize("kind, outcome", [("preempted", "preempted"), ("superseded_unsent", "superseded")])
def test_a_preempted_result_set_to_success_succeeds(kind, outcome) -> None:
    store = _Store()
    preempted = _active(
        preemption={
            "kind": kind,
            "by": {"operation_id": "op.2", "node_instance_id": "other", "node_attempt_id": "a.2"},
        }
    )
    port = _MotionPort([preempted])
    _time, clock = _clocked()
    adapter = _motion(
        port, clock, parameters={"preempted_result": "success"}, store=store
    )
    assert _tick(adapter) is _SUCCESS
    assert [value for _key, value, _meta in store.writes] == [outcome]


def test_a_newcomer_refused_by_priority_ends_at_once_with_its_configured_result() -> None:
    store = _Store()
    refused = OwnerInvocationRefusedByPriorityError(
        "refused", {"operation_id": "op.1", "node_instance_id": "winner", "node_attempt_id": "a.1"}
    )
    port = _MotionPort([refused])
    _time, clock = _clocked()
    adapter = _motion(port, clock, store=store)

    assert _tick(adapter) is _FAILURE  # No queueing, no waiting, no retry.
    assert adapter.failure_reason_code == "refused_by_priority"
    assert "winner" in adapter.failure_message
    assert adapter.preempted is True  # Straight to the tree, never held.
    assert len(port.starts) == 1
    assert [value for _key, value, _meta in store.writes] == ["refused_by_priority"]
    assert adapter.audit_events == [
        {
            "kind": "program_node_preempted",
            "node_instance_id": "motion",
            "outcome": "refused_by_priority",
            "by_node_instance_id": "winner",
            "by_operation_id": "op.1",
            "result": "failure",
        }
    ]


def test_a_refused_by_priority_node_under_an_intervention_policy_returns_its_result() -> None:
    refused = OwnerInvocationRefusedByPriorityError(
        "refused", {"operation_id": "op.1", "node_instance_id": "winner", "node_attempt_id": "a.1"}
    )
    port = _MotionPort([refused])
    _time, clock = _clocked()
    adapter = _motion(
        port,
        clock,
        parameters={
            "automatic_retries": 5,
            "automatic_retry_delay_s": 0.0,
            "preempted_result": "success",
        },
    )
    adapter.hold_failures_for_intervention()
    recovery = _RecoveryPort()
    node = RecoveryAwareNode(
        child=adapter,
        node_instance_id="motion",
        policy=SimpleNamespace(recovery_policy={"allow_intervention": True, "max_retries": 0}),
        attempt_source=_Attempts(),
        recovery_port=recovery,
    )

    node.tick_once()
    assert node.status is _SUCCESS  # Its literal preempted_result.
    assert recovery.required == [] and len(port.starts) == 1


@pytest.mark.parametrize(
    "starts, status, outcome",
    [([_succeeded()], _SUCCESS, "completed"), ([_failed(_assessment(eligible=False))], _FAILURE, "failed")],
)
def test_the_node_outcome_key_receives_completed_or_failed_once(starts, status, outcome) -> None:
    store = _Store()
    port = _MotionPort(starts)
    _time, clock = _clocked()
    adapter = _motion(port, clock, store=store)
    assert _tick(adapter) is status
    assert [(key, value) for key, value, _meta in store.writes] == [("node_outcome.motion", outcome)]
    assert store.writes[0][2]["node_attempt_id"] == "attempt.1"


def test_the_node_outcome_keeps_its_value_until_the_node_ends_again() -> None:
    store = _Store()
    port = _MotionPort([_succeeded(), _active(), _failed(_assessment(eligible=False))])
    _time, clock = _clocked()
    adapter = _motion(port, clock, store=store)
    assert _tick(adapter) is _SUCCESS
    adapter.stop(_SUCCESS)
    assert _tick(adapter) is _RUNNING  # Started again: nothing cleared or written.
    assert [value for _key, value, _meta in store.writes] == ["completed"]


def test_a_rejected_outcome_write_fails_the_node() -> None:
    _time, clock = _clocked()
    refused = _motion(_MotionPort([_succeeded()]), clock, store=_Store(refuse=True))
    assert _tick(refused) is _FAILURE
    assert refused.failure_reason_code == "owner_output_rejected"


def test_program_refuses_a_value_source_on_the_literal_only_motion_settings() -> None:
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    for name in (
        "automatic_retries",
        "automatic_retry_delay_s",
        "preemption_priority",
        "preempted_result",
    ):
        document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
        # The historical source keeps its own selector; select the current catalog.
        document["catalog"] = {
            "api_version": catalog.api_version,
            "content_version": catalog.content_version,
        }
        move = document["tree"]["children"][0]
        move["value_sources"][name] = {"kind": "program_parameter", "reference": "pick_velocity_scale"}
        move["value_sources"] = dict(sorted(move["value_sources"].items()))
        document["program_revision"] = compute_content_identity(
            document,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version="manyforge.program.v2",
        )
        with pytest.raises(ProgramValidationError, match=name):
            Program.from_dict(document, catalog)


def test_a_tree_reentry_is_a_new_node_attempt_with_its_own_automatic_budget() -> None:
    # Maintainer decision (P-1): a decorator's re-run is a new node attempt. It
    # has its own automatic budget; no counter is shared between attempts.
    port = _MotionPort([_failed(_assessment())])
    _time, clock = _clocked()
    adapter = _motion(port, clock, parameters={"automatic_retries": 1, "automatic_retry_delay_s": 0.0})

    for _ in range(3):
        if _tick(adapter) is _FAILURE:
            break
    assert adapter.status is _FAILURE  # The first series' budget is spent.
    assert len(port.starts) == 2

    assert _tick(adapter) is _RUNNING  # Re-entered: a fresh attempt, a fresh budget.
    assert len(port.starts) == 3
    assert len(adapter.audit_events) == 2


# --- Close reader (SX-1972..1974) ---------------------------------------------


def test_close_permission_decides_first_over_legacy_labels_and_allowed_actions() -> None:
    # A proven never-dispatched failure: legacy reset_required status, an
    # intervention_required execution state, no retry admissible and no
    # close_operation offered; the exact current eligible_close still closes.
    assessment = {
        **_assessment(eligible=False, status="reset_required", execution_state="InterventionRequired"),
        "allowed_actions": [],
        "retry_admissible": False,
    }
    port = _MotionPort([_failed(assessment)])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE
    assert len(port.closes) == 1
    assert port.observations == 0  # Decided on the exact current read.
    assert adapter.cached_control_state() == "inactive"


def _uncertain(assessment: dict[str, Any] | None) -> OwnerTaskSnapshot:
    task = _failed(assessment)
    fact = {**_FACT, "terminal_facts": [{"kind": "local_failure", "effect": "effect_uncertain"}]}
    return replace(task, metadata={**task.metadata, "operation_failure": fact})


@pytest.mark.parametrize(
    "task",
    [
        pytest.param(_uncertain(None), id="effect-uncertain-unassessed"),
        pytest.param(
            _failed({**_assessment(eligible_close=False), "status": "waiting", "execution_state": "InterventionRequired"}),
            id="unresolved-intervention",
        ),
    ],
)
def test_no_close_permission_with_uncertain_effect_or_intervention_is_retained_at_once(task) -> None:
    port = _MotionPort([task])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE  # Never waited on.
    assert port.closes == []
    assert port.observations == 0
    assert adapter.cached_control_state() == "unrecoverable"  # Identity retained.


def test_an_uncertain_failure_under_an_intervention_policy_is_held_with_its_identity() -> None:
    port = _MotionPort([_uncertain(None)])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    adapter.hold_failures_for_intervention()

    assert _tick(adapter) is _FAILURE  # The policy wrapper holds this failure.
    assert port.closes == []
    assert adapter.owner_recovery_context["owner_task_id"] == "task-1"


@pytest.mark.parametrize("unavailable", [False, True], ids=["unassessed", "observation-unavailable"])
def test_an_assessment_that_stays_unavailable_is_reread_boundedly_then_retained(
    monkeypatch, unavailable
) -> None:
    monkeypatch.setattr(v2_owner, "_ASSESSMENT_REREAD_LIMIT", 4)
    port = _MotionPort([_failed(None)], observes=[_failed(None)] * 10)
    if unavailable:
        def observe(task_id):
            port.observations += 1
            raise OwnerTaskObservationUnavailableError("owner unreachable")
        port.observe = observe
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    statuses = [_tick(adapter) for _ in range(4)]
    assert statuses == [_RUNNING, _RUNNING, _RUNNING, _FAILURE]  # Never indefinitely RUNNING.
    assert port.observations == 5  # Only exact rereads of the same operation.
    assert port.closes == []
    assert len(port.starts) == 1
    assert adapter.failure_reason_code == "planning_failed"
    assert adapter.cached_control_state() == "unrecoverable"  # Retained for Reset.


def test_a_transiently_unavailable_assessment_closes_once_it_is_readable() -> None:
    port = _MotionPort(
        [_failed(None)],
        observes=[_failed(None), _failed(_assessment(eligible=False))],
    )
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _RUNNING
    assert _tick(adapter) is _FAILURE  # Readable now: closed, then the failure.
    assert len(port.closes) == 1


def test_a_successful_read_showing_no_residency_is_already_retired() -> None:
    retired = _failed(None)
    retired = replace(retired, metadata={**retired.metadata, "resident": False})
    port = _MotionPort([_failed(None)], observes=[retired])
    _time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _FAILURE
    assert port.closes == []  # Not a cleanup target.
    assert adapter.cached_control_state() == "inactive"


def test_an_abandoned_move_whose_assessment_stays_unavailable_is_left_for_reset(monkeypatch) -> None:
    monkeypatch.setattr(v2_owner, "_ASSESSMENT_REREAD_LIMIT", 3)
    interrupted = _failed(None, state=OwnerTaskState.INTERRUPTED)
    port = _MotionPort([_active()], observes=[_active()] + [interrupted] * 10)
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    _tick(adapter)
    adapter.stop(_INVALID)

    # One observation still active, then two unavailable rereads, then left.
    assert [adapter.drain_invalidated() for _ in range(4)] == [False, False, False, True]
    assert port.closes == []
    assert not adapter.has_pending_drain


# --- Preemption goes straight to the tree (maintainer decision) ------------------


class _RecoveryPort:
    def __init__(self) -> None:
        self.required: list[Any] = []

    def require(self, identity, **kwargs) -> None:
        self.required.append(identity)

    def consume_resolution(self, identity):
        return None


def test_a_preempted_node_under_an_intervention_policy_is_neither_held_nor_retried() -> None:
    preempted = _active(
        preemption={
            "kind": "preempted",
            "by": {"operation_id": "op.2", "node_instance_id": "other", "node_attempt_id": "a.2"},
        }
    )
    port = _MotionPort([_active()], observes=[preempted])
    _time, clock = _clocked()
    adapter = _motion(port, clock, parameters={"automatic_retries": 5, "automatic_retry_delay_s": 0.0})
    adapter.hold_failures_for_intervention()
    policy = SimpleNamespace(recovery_policy={"allow_intervention": True, "max_retries": 0})
    recovery = _RecoveryPort()
    attempts = _Attempts()
    node = RecoveryAwareNode(
        child=adapter,
        node_instance_id="motion",
        policy=policy,
        attempt_source=attempts,
        recovery_port=recovery,
    )

    node.tick_once()
    assert node.status is _RUNNING
    node.tick_once()
    assert node.status is _FAILURE  # Its preempted_result, straight to the tree.
    assert recovery.required == []
    assert len(port.starts) == 1 and port.closes == []
    assert adapter.audit_events == [
        {
            "kind": "program_node_preempted",
            "node_instance_id": "motion",
            "outcome": "preempted",
            "by_node_instance_id": "other",
            "by_operation_id": "op.2",
            "result": "failure",
        }
    ]


def test_raw_co_participant_facts_never_replace_the_selected_cause_and_permission() -> None:
    # SX-1985: every participant's raw terminal fact is kept. Close, retry and
    # hold read the selected cause and the Kernel's eligible_close/assessment,
    # never a fact list that would have to mirror the selected error.
    task = _failed(_assessment())
    mixed = {
        **_FACT,
        "terminal_facts": [
            {"kind": "local_failure", "effect": "no_effect"},
            {"kind": "computed_completion", "effect": "effect_settled"},
        ],
    }
    port = _MotionPort(
        [replace(task, metadata={**task.metadata, "operation_failure": mixed}), _succeeded()]
    )
    time, clock = _clocked()
    adapter = _motion(port, clock)

    assert _tick(adapter) is _RUNNING  # Closed, then the automatic retry waits.
    assert len(port.closes) == 1
    assert adapter.audit_events[0]["cause"] == "planning_no_effect"
    time.now = 1.0
    assert _tick(adapter) is _SUCCESS


# Interrupt motion nodes (P5, I5): the adapter maps the typed rule result -----


def _rule_row(cause: str, stop_result: str | None, *, trip: bool = True) -> dict[str, Any]:
    return {
        "registration": {
            "owner_epoch": "kernel-1",
            "policy_id": "door_rule",
            "scope": {"kind": "operation", "operation_id": "op"},
        },
        "evidence": {
            "state": "retired",
            "trip": {"triggering_acquisition": None} if trip else None,
            "source_losses": [],
            "evaluation_error": None,
        },
        "stop_id": "stop" if stop_result else None,
        "stop_generation": 1 if stop_result else None,
        "stop_certificate_id": "cert" if stop_result else None,
        "stop_certificate_revision": 1 if stop_result else None,
        "stop_result": stop_result,
        "terminal_cause": cause,
        "error": None,
    }


def _interrupt_task(
    state: OwnerTaskState, row: dict[str, Any] | None, **metadata: Any
) -> OwnerTaskSnapshot:
    participants = [{"participant_id": "arm", "io_rule_outcomes": [] if row is None else [row]}]
    meta: dict[str, Any] = {
        "operation_id": "op",
        "interrupt_operation": {"operation_id": "op", "participants": participants},
        **metadata,
    }
    if state is not OwnerTaskState.SUCCEEDED:
        meta.setdefault("recovery_assessment", _assessment(eligible=False, execution_state="Stopped"))
        meta.setdefault("operation_failure", _FACT)
    return OwnerTaskSnapshot(
        task_id="task", state=state, metadata=meta,
        reason_code="operation_stopped" if state is not OwnerTaskState.SUCCEEDED else None,
        message="native operation was interrupted" if state is not OwnerTaskState.SUCCEEDED else "",
    )


def _interrupt_motion(port: _MotionPort, node_id: str = "motion.move_until_signal", **parameters: Any):
    _, clock = _clocked()
    store = _Store()
    audit: list[dict[str, Any]] = []
    adapter = v2_owner.InterruptMotionTaskAdapter(
        name="probe",
        node_id=node_id,
        node_instance_id="probe",
        attempt_source=_Attempts(),
        parameters=dict(parameters),
        value_sources={},
        input_resolver=_Resolver(None, store),
        port=port,
        owner_port_name="kernel_operations",
        intervention_port=None,
        program_clock=clock,
        motion_attention=_Attention(_Log()),
        audit_sink=audit.append,
        owner_api="kernel.operation",
        cancellable=True,
    )
    return adapter, store, audit


def test_interrupt_adapter_uses_the_mapper_for_a_contained_trip(monkeypatch) -> None:
    calls: list[tuple[Any, ...]] = []
    real = v2_owner.map_interrupt_result

    def spy(node_id, parameters, operation, **kwargs):
        calls.append((node_id, dict(parameters), operation["operation_id"], kwargs))
        return real(node_id, parameters, operation, **kwargs)

    monkeypatch.setattr(v2_owner, "map_interrupt_result", spy)
    port = _MotionPort([_interrupt_task(
        OwnerTaskState.INTERRUPTED, _rule_row("signal_trip", "succeeded_nominal"),
        recovery_assessment=_assessment(execution_state="Stopped"),
    )])
    adapter, store, audit = _interrupt_motion(port)
    assert _tick(adapter) is _SUCCESS
    assert calls and calls[0][0] == "motion.move_until_signal" and calls[0][2] == "op"
    # The stopped operation is closed before the success.
    assert len(port.closes) == 1
    assert store.writes[0][:2] == ("node_outcome.probe", "signal_trip")
    assert audit[0]["kind"] == "program_node_interrupt_result"
    assert (audit[0]["outcome"], audit[0]["result"]) == ("signal_trip", "success")
    assert "door_rule tripped" in audit[0]["message"]


def _captured_stop_assessment(execution_state: str = "Stopped", **overrides: Any) -> dict[str, Any]:
    """The participant recovery assessment as captured on the real wire
    (process48): Core's TitleCase execution_state, close permitted, and no
    close_operation in allowed_actions (Core skips it for CloseOperation)."""

    return {
        "schema_version": "manyforge.kernel_recovery_assessment.v1",
        "allowed_actions": ["replan_from_stopped_state", "reset_cycle"],
        "assessment_revision": 5,
        "backend_stop_confirmed": True,
        "eligible_close": True,
        "execution_id": 2,
        "execution_state": execution_state,
        "reason_code": "stop_certified",
        "recovery_class": "ordinary_operator_stop",
        "retry_admissible": True,
        "retry_eligibility": {
            "cause": "not_eligible", "effect": "effect_uncertain",
            "eligible": False, "reason": "not_retryable_state",
        },
        "status": "retry_ready",
        "stop_certificate_id": "cert",
        "stop_certificate_revision": 1,
        "stop_owner_active": False,
        "operation_revision": 2,
        "stop_id": "stop",
        "stop_generation": 1,
        **overrides,
    }


def test_interrupt_adapter_closes_a_contained_trip_on_the_captured_stopped_assessment() -> None:
    task = _interrupt_task(
        OwnerTaskState.INTERRUPTED, _rule_row("signal_trip", "succeeded_nominal"),
        recovery_assessment=_captured_stop_assessment(),
    )
    port = _MotionPort([task], observes=[task] * 5)
    adapter, store, _ = _interrupt_motion(port)
    assert _tick(adapter) is _SUCCESS
    assert len(port.closes) == 1  # Closed once on the exact current read.
    assert port.observations == 0
    assert store.writes[0][1] == "signal_trip"


def test_interrupt_adapter_retains_a_trip_whose_assessment_requires_intervention() -> None:
    task = _interrupt_task(
        OwnerTaskState.INTERRUPTED, _rule_row("signal_trip", "succeeded_nominal"),
        recovery_assessment=_captured_stop_assessment(
            "InterventionRequired", eligible_close=False, status="reset_required",
        ),
    )
    port = _MotionPort([task], observes=[task] * 5)
    adapter, _, _ = _interrupt_motion(port)
    assert _tick(adapter) is _FAILURE  # Decided at once, never waited on.
    assert port.closes == [] and port.observations == 0
    assert adapter.failure_reason_code == "motion_interrupt_operation_retained"


@pytest.mark.parametrize("spelling", ["stopped", "intervention_required", "STOPPED", "Unknown"])
def test_interrupt_adapter_does_not_decide_on_an_unknown_execution_state_spelling(
    monkeypatch, spelling
) -> None:
    # Only Core's exact TitleCase spellings qualify: an unknown one is neither
    # stopped nor intervention, so the bounded reread runs unchanged.
    monkeypatch.setattr(v2_owner, "_ASSESSMENT_REREAD_LIMIT", 3)
    task = _interrupt_task(
        OwnerTaskState.INTERRUPTED, _rule_row("signal_trip", "succeeded_nominal"),
        recovery_assessment=_captured_stop_assessment(spelling),
    )
    port = _MotionPort([task], observes=[task] * 10)
    adapter, _, _ = _interrupt_motion(port)
    assert [_tick(adapter) for _ in range(3)] == [_RUNNING, _RUNNING, _FAILURE]
    assert port.observations == 3
    assert port.closes == []
    assert adapter.failure_reason_code == "motion_interrupt_operation_retained"


def test_interrupt_adapter_maps_no_signal_per_node_default() -> None:
    # move_until_signal: natural completion is success.
    port = _MotionPort([_interrupt_task(OwnerTaskState.SUCCEEDED, _rule_row("no_signal_trip", None, trip=False))])
    adapter, store, _ = _interrupt_motion(port)
    assert _tick(adapter) is _SUCCESS
    assert port.acknowledged == ["task-1"]
    assert store.writes[0][1] == "no_signal_trip"
    # probe_search: natural completion is failure, acknowledged first.
    port = _MotionPort([_interrupt_task(OwnerTaskState.SUCCEEDED, _rule_row("no_signal_trip", None, trip=False))])
    adapter, store, audit = _interrupt_motion(port, "motion.probe_search")
    assert _tick(adapter) is _FAILURE
    assert port.acknowledged == ["task-1"]
    assert store.writes[0][1] == "no_signal_trip"
    assert adapter.feedback_message.startswith("motion_no_signal_trip_mapped_failure")
    assert audit[0]["result"] == "failure"


def test_interrupt_adapter_escalated_trip_needs_the_containing_stage() -> None:
    row = _rule_row("signal_trip", "succeeded_escalated")
    terminal = {
        "details_truncated": False, "stop_id": "stop", "stop_generation": 1,
        "stop_certificate_id": "cert", "stop_certificate_revision": 1,
        "attempted_stages": [{
            "authority": "authoritative", "error": None, "operation_id": "op", "outcome": "succeeded",
            "participant_id": "arm", "participant_stop_invocation_plan_digest": "plan",
            "scope_key": {"subject": {"kind": "participant", "operation_id": "op", "participant_id": "arm"}},
            "stage": {"stage_id": "controller_goal_cancel", "stage_index": 1},
        }],
        "participant_settlement": [{
            "scope_key": {"subject": {"kind": "participant", "operation_id": "op", "participant_id": "arm"}},
            "participant_stop_invocation_plan_digest": "plan",
        }],
    }
    chain = ["controlled_stop", "controller_goal_cancel", "software_hard_stop"]
    allowed = _interrupt_task(
        OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal=terminal, interrupt_stop_chain=chain,
        recovery_assessment=_assessment(execution_state="Stopped"),
    )
    adapter, store, _ = _interrupt_motion(_MotionPort([allowed]), success_through_stage_id="controller_goal_cancel")
    assert _tick(adapter) is _SUCCESS
    assert store.writes[0][1] == "signal_trip"
    # Without the Stop terminal projection the stage is unproven: Failure.
    unproven = _interrupt_task(OwnerTaskState.INTERRUPTED, row, interrupt_stop_chain=chain)
    adapter, store, _ = _interrupt_motion(_MotionPort([unproven]), success_through_stage_id="controller_goal_cancel")
    assert _tick(adapter) is _FAILURE
    assert adapter.feedback_message.startswith("motion_interrupt_containing_stage_unproven")
    assert store.writes[0][1] == "failed"


def _escalated_terminal() -> dict[str, Any]:
    scope = {"subject": {"kind": "participant", "operation_id": "op", "participant_id": "arm"}}
    return {
        "details_truncated": False, "stop_id": "stop", "stop_generation": 1,
        "stop_certificate_id": "cert", "stop_certificate_revision": 1,
        "attempted_stages": [{
            "authority": "authoritative", "error": None, "operation_id": "op", "outcome": "succeeded",
            "participant_id": "arm", "participant_stop_invocation_plan_digest": "plan",
            "scope_key": scope, "stage": {"stage_id": "controller_goal_cancel", "stage_index": 1},
        }],
        "participant_settlement": [{"scope_key": scope, "participant_stop_invocation_plan_digest": "plan"}],
    }


def test_interrupt_adapter_waits_while_the_stop_terminal_is_pending() -> None:
    # SX-2226: the record may follow the terminal operation; the node stays
    # RUNNING and re-observes once per tick until it is there.
    row = _rule_row("signal_trip", "succeeded_escalated")
    chain = ["controlled_stop", "controller_goal_cancel", "software_hard_stop"]
    meta = dict(interrupt_stop_chain=chain, recovery_assessment=_assessment(execution_state="Stopped"))
    pending = _interrupt_task(OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal_pending=True, **meta)
    recorded = _interrupt_task(
        OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal=_escalated_terminal(), **meta)
    port = _MotionPort([pending], observes=[pending, recorded])
    adapter, store, audit = _interrupt_motion(port, success_through_stage_id="controller_goal_cancel")
    assert _tick(adapter) is _RUNNING and port.observations == 0
    assert _tick(adapter) is _RUNNING and port.observations == 1
    assert audit == [] and store.writes == []
    assert _tick(adapter) is _SUCCESS and port.observations == 2
    assert store.writes[0][1] == "signal_trip"


def test_interrupt_adapter_maps_an_unproven_stop_terminal_after_the_wait() -> None:
    # failed / truncated / missing or unknown projection: the port attaches
    # no usable record and no pending flag, so the node fails as unproven.
    row = _rule_row("signal_trip", "succeeded_escalated")
    chain = ["controlled_stop", "controller_goal_cancel", "software_hard_stop"]
    meta = dict(interrupt_stop_chain=chain, recovery_assessment=_assessment(execution_state="Stopped"))
    pending = _interrupt_task(OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal_pending=True, **meta)
    truncated = {**_escalated_terminal(), "details_truncated": True, "attempted_stages": []}
    for unproven in (
        _interrupt_task(OwnerTaskState.INTERRUPTED, row, **meta),
        _interrupt_task(OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal=truncated, **meta),
    ):
        port = _MotionPort([pending], observes=[unproven])
        adapter, store, _ = _interrupt_motion(port, success_through_stage_id="controller_goal_cancel")
        assert _tick(adapter) is _RUNNING
        assert _tick(adapter) is _FAILURE
        assert adapter.feedback_message.startswith("motion_interrupt_containing_stage_unproven")
        assert store.writes[0][1] == "failed"


def test_interrupt_adapter_keeps_ordinary_stop_handling(monkeypatch) -> None:
    seen: list[str] = []
    monkeypatch.setattr(
        v2_owner.MotionTaskAdapter, "_settle",
        lambda self, task: seen.append(task.task_id) or _FAILURE,
    )
    port = _MotionPort([_interrupt_task(OwnerTaskState.INTERRUPTED, _rule_row("stopped", "succeeded_nominal"))])
    adapter, _, audit = _interrupt_motion(port)
    assert _tick(adapter) is _FAILURE
    assert seen == ["task-1"] and audit == []


def test_interrupt_nodes_are_qualified_and_build_the_interrupt_adapter() -> None:
    from manyforge_behavior import executable_node_resolver as resolver
    from manyforge_behavior.node_environment import build_node_environment

    dispositions = {
        item.row.node_id: item
        for item in resolver.ExecutableNodeResolver(build_node_environment()).dispositions
    }
    for node_id in ("motion.move_until_signal", "motion.probe_search"):
        assert dispositions[node_id].adapter_build_qualified
        assert dispositions[node_id].owner_port_name == "kernel_operations"
        assert dispositions[node_id].unavailable_reason_code is None
    build = resolver._owner_task_builder(
        "motion.probe_search", "kernel_operations", owner_api="kernel.operation", cancellable=True,
    )
    context = SimpleNamespace(
        ports=SimpleNamespace(kernel_operations=_MotionPort([]), program_interventions=None),
        program_clock=_clocked()[1], operator_stop_hold_scope=None, motion_attention=None,
        audit_sink=None, name="probe", node_instance_id="probe", attempt_source=_Attempts(),
        parameters={}, value_sources={}, input_resolver=_Resolver(), operator_stop_state=None,
    )
    assert type(build(context)) is v2_owner.InterruptMotionTaskAdapter


@pytest.mark.parametrize("node_id", ["motion.move_until_signal", "motion.probe_search"])
def test_interrupt_adapter_maps_a_trip_to_its_authored_outcome(node_id) -> None:
    trip = _rule_row("signal_trip", "succeeded_nominal")
    for on_signal, status in (("success", _SUCCESS), ("failure", _FAILURE)):
        port = _MotionPort([_interrupt_task(
            OwnerTaskState.INTERRUPTED, trip, recovery_assessment=_assessment(execution_state="Stopped"),
        )])
        adapter, store, audit = _interrupt_motion(port, node_id, on_signal=on_signal)
        assert _tick(adapter) is status
        assert store.writes[0][1] == "signal_trip"
        assert audit[0]["outcome"] == "signal_trip"
        assert len(port.starts) == 1


@pytest.mark.parametrize("node_id", ["motion.move_until_signal", "motion.probe_search"])
def test_interrupt_adapter_maps_no_trip_to_its_authored_outcome(node_id) -> None:
    for on_no_signal, status in (("success", _SUCCESS), ("failure", _FAILURE)):
        port = _MotionPort([_interrupt_task(
            OwnerTaskState.SUCCEEDED, _rule_row("no_signal_trip", None, trip=False))])
        adapter, store, _ = _interrupt_motion(port, node_id, on_no_signal=on_no_signal)
        assert _tick(adapter) is status
        assert store.writes[0][1] == "no_signal_trip"


def test_interrupt_adapter_fails_a_trip_contained_beyond_its_success_limit() -> None:
    row = _rule_row("signal_trip", "succeeded_escalated")
    terminal = _escalated_terminal()
    terminal["attempted_stages"][0]["stage"] = {"stage_id": "software_hard_stop", "stage_index": 2}
    chain = ["controlled_stop", "controller_goal_cancel", "software_hard_stop"]
    port = _MotionPort([_interrupt_task(
        OwnerTaskState.INTERRUPTED, row, interrupt_stop_terminal=terminal, interrupt_stop_chain=chain,
        recovery_assessment=_assessment(execution_state="Stopped"),
    )])
    adapter, store, audit = _interrupt_motion(port, success_through_stage_id="controller_goal_cancel")
    # Normal failure handling: the node fails, the operation is not retried.
    assert _tick(adapter) is _FAILURE
    assert adapter.feedback_message.startswith("motion_interrupt_stop_failed")
    assert store.writes[0][1] == "failed"
    assert len(port.starts) == 1
    assert [event["kind"] for event in audit] == ["program_node_interrupt_result"]


def test_interrupt_adapter_fails_a_source_loss_without_automatic_retry() -> None:
    # D2: the guard source was lost; the Kernel stopped the move. Even an
    # assessment that would allow a retry starts no second attempt.
    loss = {"source_id": "station.io", "source_epoch": "e1", "kind": "stale"}
    row = _rule_row("failed", "succeeded_nominal", trip=False)
    row["evidence"]["source_losses"] = [loss]
    task = _interrupt_task(
        OwnerTaskState.INTERRUPTED, row, recovery_assessment=_assessment(execution_state="Stopped"))
    port = _MotionPort([task, _succeeded()])
    adapter, store, audit = _interrupt_motion(port)
    assert _tick(adapter) is _FAILURE
    assert len(port.starts) == 1
    assert adapter.feedback_message.startswith("interrupt_source_unavailable")
    assert "station.io" in adapter.feedback_message
    assert store.writes[0][1] == "failed"
    assert not any(event["kind"] == "program_node_automatic_retry" for event in audit)


def test_interrupt_adapter_never_retries_an_ordinary_failure_automatically() -> None:
    # An eligible planning failure would retry a plain move (spec 410 §4.1).
    port = _MotionPort([_failed(_assessment()), _succeeded()])
    adapter, _, audit = _interrupt_motion(port)
    assert _tick(adapter) is _FAILURE
    assert len(port.starts) == 1
    assert not any(event["kind"] == "program_node_automatic_retry" for event in audit)


def test_probe_search_refused_by_its_unavailable_contact_capability_fails() -> None:
    # The Core refuses an unprovided precision-contact capability before any
    # effect (Apply refuses it first; spec 260 §12.9): a Failure, no trip.
    from manyforge_behavior.kernel_client import KernelOperationAdmissionRejectedError

    refusal = KernelOperationAdmissionRejectedError(
        "unsupported_capability",
        "execution.interrupt.contact_policy has no provider in the Applied cell",
    )
    port = _MotionPort([refusal])
    adapter, store, audit = _interrupt_motion(port, "motion.probe_search")
    assert _tick(adapter) is _FAILURE
    assert "execution.interrupt.contact_policy" in adapter.feedback_message
    assert store.writes[0][1] == "failed"
    assert len(port.starts) == 1 and port.closes == []
    assert audit == []


def _takeover(path: str, cause: dict[str, Any] | None) -> dict[str, Any]:
    return {
        "incumbent_operation_id": "op.1",
        "path": path,
        "cause": cause,
        "decided_at_ns": 9,
        "elapsed_since_acceptance_ns": 40_500_000,
        "preparation_budget_ns": 30_000_000,
        "stop_certificate": None,
    }


@pytest.mark.parametrize(
    "path, cause, code, message, stage",
    [
        ("spring_back_hold", {"code": "takeover_late", "message": "late", "details": {}},
         "takeover_late", "late", None),
        ("spring_back_hold", {
            "code": "planning_timeout", "message": "out of time",
            "details": {"failure_stage": "spring_back_planning", "planning_elapsed_ns": "5"},
        }, "planning_timeout", "out of time", "spring_back_planning"),
        ("ordinary_containment", None, None, None, None),
    ],
)
def test_a_takeover_fallback_is_recorded_once_per_attempt(path, cause, code, message, stage) -> None:
    takeover = _takeover(path, cause)
    port = _MotionPort(
        [_active()],
        observes=[_active(motion_takeover=takeover)] * 3 + [_succeeded()],
    )
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    statuses = [_tick(adapter) for _ in range(5)]

    assert statuses == [_RUNNING] * 4 + [_SUCCESS]  # The outcome is unchanged.
    assert tuple(adapter.audit_events) == (
        {
            "kind": "program_node_takeover_fallback",
            "node_instance_id": "motion",
            "incumbent_operation_id": "op.1",
            "path": path,
            "code": code,
            "message": message,
            "failure_stage": stage,
            "preparation_ms": 40.5,
            "budget_ms": 30.0,
        },
    )


def test_a_takeover_fallback_is_recorded_again_for_a_fresh_attempt() -> None:
    fallback = _active(motion_takeover=_takeover("spring_back_hold", None))
    port = _MotionPort([fallback], observes=[fallback, _succeeded()])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    assert _tick(adapter) is _RUNNING
    assert _tick(adapter) is _RUNNING
    assert len(adapter.audit_events) == 1
    adapter._begin_attempt(restart_series=True)  # A new node attempt.
    port._observes = [fallback]
    _tick(adapter)
    _tick(adapter)
    assert [event["kind"] for event in adapter.audit_events] == [
        "program_node_takeover_fallback"
    ] * 2


@pytest.mark.parametrize(
    "metadata",
    [{}, {"motion_takeover": _takeover("moving_replacement", None)}],
)
def test_a_smooth_or_absent_takeover_records_nothing(metadata) -> None:
    port = _MotionPort([_active(**metadata)], observes=[_active(**metadata), _succeeded()])
    _time, clock = _clocked()
    adapter = _motion(port, clock)
    for _ in range(3):
        _tick(adapter)
    assert adapter.status is _SUCCESS
    assert adapter.audit_events == []
