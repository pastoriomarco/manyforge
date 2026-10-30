"""An accepted failure-hold release advances the execution-control revision.

The operation-failure condition clears only under a later accepted control
revision. A release keeps the cycle running, so the Runner itself must
advance the revision once for it; the adapter never infers a release.
"""

from __future__ import annotations

from copy import deepcopy
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot
from manyforge_behavior.program_run_owner import ProgramRunOwnerOutcomeUnknownError
from manyforge_headless_api.control_protocol import ControlFreshnessAuthority
from manyforge_headless_api.control_state import HeadlessControlStateSource
from manyforge_headless_api.execution_control import ExecutionControlIngress
from manyforge_program_runner.models import CycleRecoveryActionRequest
from manyforge_program_runner.runtime import ProgramRunnerError

from .test_runtime import (
    _DIGEST_A,
    _Host,
    _Owner,
    _PreparedRunAdmission,
    _Snapshot,
    _bypass_scene_preparation,
    _reconcile_runtime,
    _start_request,
)

_RUN = "program-cycle.1"


def _row(node: str, attempt: str, operation: str) -> dict[str, Any]:
    return {
        "node_instance_id": node,
        "node_attempt_id": attempt,
        "operator_facing_run_revision": 2,
        "operation": {
            "source_owner_epoch": "kernel.1",
            "operation_id": operation,
            "operation_revision": 1,
        },
        "resource_ids": ["robot.arm"],
        "failure": {"code": "planning_failed", "message": "original failure"},
        "recovery_class": "failed_validation",
        "terminal_facts": [{"kind": "local_failure", "effect": "no_effect"}],
    }


def _publish(owner: _Owner, rows: list[dict[str, Any]], state: str) -> _Snapshot:
    """Replace the run with the owner's next revision and these failure rows."""

    current = owner.rows[_RUN]
    value = current.to_dict()
    snapshot = _Snapshot(
        _RUN,
        owner_id=value["owner_id"],
        owner_epoch=value["owner_epoch"],
        state=state,
        revision=current.run_revision + 1,
    )
    snapshot.capture = current.capture  # The run's captured policy stays.
    snapshot._value["operation_failures"] = deepcopy(rows)  # noqa: SLF001
    owner.rows[_RUN] = snapshot
    return snapshot


class _Harness:
    """A real ProgramRuntime and cycle journal over a failure-holding owner."""

    def __init__(self, monkeypatch, tmp_path, policy_parameter: str) -> None:
        monkeypatch.setattr(
            "manyforge_program_runner.runtime.build_applied_program_run_admission",
            lambda *_args, **_kwargs: _PreparedRunAdmission(),
        )
        _bypass_scene_preparation(monkeypatch)
        runtime, journal, _admission = _reconcile_runtime(
            tmp_path,
            journal_owner_epoch="runner.1",
            runtime_owner_epoch="runner.1",
            inventory={},
        )
        owner = _Owner()
        host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
        runtime._owner, runtime._host = owner, host  # noqa: SLF001
        runtime.start()
        runtime.start_run(_start_request())
        self.runtime, self.journal, self.owner, self.host = runtime, journal, owner, host
        owner.rows[_RUN].capture.node_policy_by_instance_id = {
            node: SimpleNamespace(
                recovery_policy={"allow_intervention": True, "max_retries": 3}
            )
            for node in ("move.1", "move.2")
        }
        self.rows = [
            _row("move.1", "attempt.1", "operation.1"),
            _row("move.2", "attempt.2", "operation.2"),
        ]
        self.holds = {
            row["node_attempt_id"]: SimpleNamespace(
                identity=SimpleNamespace(
                    program_run_id=_RUN,
                    node_instance_id=row["node_instance_id"],
                    node_attempt_id=row["node_attempt_id"],
                ),
                owner_task_id=row["operation"]["operation_id"],
                owner_port_name="kernel",
                policy_parameter=policy_parameter,
            )
            for row in self.rows
        }
        _publish(owner, self.rows, "intervention_required")
        owner.intervention = lambda _run, *, node_attempt_id=None: self.holds.get(
            node_attempt_id or "attempt.1"
        )
        owner.operator_stop_scope = lambda _run: OperatorMotionStopSnapshot()
        self.offer = {
            "offer_id": _DIGEST_A,
            "offer_revision": 1,
            "node_attempt_id": "attempt.1",
            "actions": [],
        }
        runtime._recovery_offer = lambda *_a, **_k: deepcopy(self.offer)  # noqa: SLF001
        self.effects: list[str] = []
        self.resolve_error: Exception | None = None

        def resolve(_run, *, node_attempt_id, **_kwargs):
            # The owner's resolve_intervention: pop this hold's failure row.
            if self.resolve_error is not None:
                raise self.resolve_error
            self.effects.append(node_attempt_id)
            self.rows = [
                row for row in self.rows if row["node_attempt_id"] != node_attempt_id
            ]
            self.holds.pop(node_attempt_id, None)
            return _publish(
                owner, self.rows, "intervention_required" if self.rows else "running"
            )

        # The owner's assessment re-read at a manual-result press (None:
        # unchanged); the Program's non-moving acknowledgement is kept.
        self.fresh: dict[str, Any] | None = None
        self.refresh_error: Exception | None = None

        def refresh(_run, *, node_attempt_id):
            if self.refresh_error is not None:
                raise self.refresh_error
            hold = self.holds[node_attempt_id]
            if self.fresh is not None:
                acknowledged = (hold.owner_assessment or {}).get("acknowledged_non_moving")
                hold.owner_assessment = {
                    **self.fresh, "acknowledged_non_moving": acknowledged is True,
                }
            return hold

        owner.refresh_failure_assessment = refresh
        owner.continue_failure_pause = lambda run, **_k: owner.read(run)
        owner.retry_failure_pause = lambda run, **_k: owner.read(run)
        owner.apply_owner_recovery = lambda *_a, **_k: {"accepted": True}
        # Acknowledge closes no hold: the run view advances, its rows stay.
        owner.note_runtime_control_change = lambda _run: _publish(
            owner, self.rows, "intervention_required"
        )
        host.resume_recovery = resolve
        host.resume_motion = lambda run, **_k: resolve(run, node_attempt_id="attempt.1")

    def apply(self, action_id: str, request_id: str) -> dict[str, Any]:
        self.offer["actions"] = [{"action_id": action_id, "enabled": True}]
        return self.runtime.apply_recovery_action(
            CycleRecoveryActionRequest.model_validate(
                {
                    "request_id": request_id,
                    "request_owner_epoch": "client.1",
                    "source_label": "test",
                    "expected_owner_epoch": self.runtime.owner_epoch,
                    "offer_id": self.offer["offer_id"],
                    "offer_revision": self.offer["offer_revision"],
                    "action_id": action_id,
                }
            )
        )

    def revision(self) -> int:
        return self.journal.read().execution_control_revision


@pytest.fixture
def harness(monkeypatch, tmp_path, request):
    created: list[_Harness] = []

    def make(policy_parameter: str = "stop_policy") -> _Harness:
        value = _Harness(monkeypatch, tmp_path, policy_parameter)
        created.append(value)
        return value

    yield make
    for value in created:
        value.runtime.shutdown()


class _Observer:
    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []

    def __call__(self, condition_json: str) -> dict[str, Any]:
        import json

        self.calls.append(json.loads(condition_json))
        return {"status": "observed", "changed": True}

    def states(self) -> list[tuple[str, str, int]]:
        return [
            (
                call["state"],
                call["occurrence"]["node_attempt_id"],
                call["execution_control_revision"],
            )
            for call in self.calls
        ]


@pytest.fixture
def adapter():
    ingress = ExecutionControlIngress(
        projection=lambda: ("runner.1", 4),
        dispatch=lambda _request, _stamp: None,
        authority=ControlFreshnessAuthority(
            receiver_incarnation="receiver.1", secret=b"x" * 32
        ),
    )
    observer = _Observer()
    source = HeadlessControlStateSource(
        SimpleNamespace(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
        program_failure_observer=observer,
    )
    try:
        yield source, observer
    finally:
        ingress.close()


def _observe(source: HeadlessControlStateSource, harness: _Harness) -> dict[str, Any]:
    # The Runner's own published cycle projection, not a fixture cycle.
    cycle = harness.runtime.cycle_status()
    source._observe_program_failures(cycle)  # noqa: SLF001
    return cycle


# Every accepted failure-hold release kind of apply_recovery_action.
_RELEASES = [
    ("stop_policy", "continue_with_failure"),
    ("stop_policy", "retry_runtime_step"),
    ("stop_policy", "retry_motion_from_current_state"),
    ("stop_policy", "retry_after_scene_repair"),
    ("operator_stop_policy", "retry_runtime_step"),
    ("operator_stop_policy", "retry_motion_from_current_state"),
]


@pytest.mark.parametrize(("policy", "action_id"), _RELEASES)
def test_each_release_advances_the_revision_once_and_the_cycle_stays_running(
    harness, policy, action_id
) -> None:
    run = harness(policy)
    before = run.revision()
    result = run.apply(action_id, "release.1")
    assert result["accepted"] is True and run.effects == ["attempt.1"]
    assert run.revision() == before + 1
    assert run.journal.read().state == "running"
    assert not run.journal.read().reconciliation_required
    # The second hold's release advances it once more.
    run.offer["node_attempt_id"] = "attempt.2"
    run.owner.intervention = lambda _run, *, node_attempt_id=None: run.holds.get(
        "attempt.2"
    )
    run.apply(action_id, "release.2")
    assert run.revision() == before + 2
    assert run.journal.read().state == "running"


def test_continue_then_retry_each_advance_once(harness) -> None:
    run = harness()
    before = run.revision()
    run.apply("continue_with_failure", "continue.1")
    assert run.revision() == before + 1
    run.offer["node_attempt_id"] = "attempt.2"
    run.owner.intervention = lambda _run, *, node_attempt_id=None: run.holds.get(
        "attempt.2"
    )
    run.apply("retry_runtime_step", "retry.1")
    assert run.revision() == before + 2
    assert run.journal.read().state == "running"


@pytest.mark.parametrize(("policy", "action_id"), _RELEASES)
def test_real_runner_release_clears_the_exact_occurrence_and_keeps_the_sibling(
    harness, adapter, policy, action_id
) -> None:
    run = harness(policy)
    source, observer = adapter
    held = _observe(source, run)
    activation = held["execution_control_revision"]
    assert observer.states() == [
        ("active", "attempt.1", activation),
        ("active", "attempt.2", activation),
    ]
    run.apply(action_id, "release.1")
    _observe(source, run)
    # Only the released occurrence clears; the sibling hold stays active.
    assert observer.states()[2:] == [("clear", "attempt.1", activation + 1)]
    cleared = observer.calls[-1]
    assert cleared["operation"]["operation_id"] == "operation.1"
    assert cleared["program_run_id"] == _RUN
    _observe(source, run)
    assert len(observer.calls) == 3


def test_acknowledge_closes_no_hold_and_neither_advances_nor_clears(
    harness, adapter
) -> None:
    run = harness("operator_stop_policy")
    source, observer = adapter
    _observe(source, run)
    before = run.revision()
    result = run.apply("acknowledge_and_continue", "ack.1")
    assert result["accepted"] is True and run.effects == []
    assert run.revision() == before
    _observe(source, run)
    assert [state for state, *_ in observer.states()] == ["active", "active"]


def test_refused_release_neither_advances_nor_clears(harness, adapter) -> None:
    run = harness()
    source, observer = adapter
    _observe(source, run)
    before = run.revision()
    # A stale offer revision is refused before any effect.
    run.offer["actions"] = [{"action_id": "continue_with_failure", "enabled": True}]
    with pytest.raises(ProgramRunnerError) as refused:
        run.runtime.apply_recovery_action(
            CycleRecoveryActionRequest.model_validate(
                {
                    "request_id": "stale.1",
                    "request_owner_epoch": "client.1",
                    "source_label": "test",
                    "expected_owner_epoch": run.runtime.owner_epoch,
                    "offer_id": run.offer["offer_id"],
                    "offer_revision": 2,
                    "action_id": "continue_with_failure",
                }
            )
        )
    assert refused.value.code == "cycle_recovery_offer_stale"
    assert run.revision() == before and run.effects == []
    _observe(source, run)
    assert [state for state, *_ in observer.states()] == ["active", "active"]


def test_stale_press_refusal_keeps_the_owner_typed_reason(harness, adapter) -> None:
    """Offers do not expire, so freshness is checked at the press: the owner's
    refusal reaches the caller with its code, producer and typed details."""
    run = harness("operator_stop_policy")
    source, observer = adapter
    _observe(source, run)
    before = run.revision()
    run.owner.apply_owner_recovery = lambda *_a, **_k: {
        "accepted": False,
        "error": {
            "code": "recovery_action_rejected",
            "message": "Recovery action rejected",
            "producer": "kernel",
            "details": {
                "reason": "observed_state_stale",
                "assessment_status": "waiting",
            },
        },
    }
    with pytest.raises(ProgramRunnerError) as refused:
        run.apply("acknowledge_and_continue", "stale-press.1")
    assert refused.value.code == "recovery_action_rejected"
    assert str(refused.value) == "Recovery action rejected"
    assert refused.value.producer == "kernel"
    assert refused.value.details["reason"] == "observed_state_stale"
    assert refused.value.details["assessment_status"] == "waiting"
    assert refused.value.details["recovery_offer"] == run.offer
    assert run.revision() == before and run.effects == []
    _observe(source, run)
    assert [state for state, *_ in observer.states()] == ["active", "active"]


def test_unknown_release_outcome_keeps_the_held_occurrence_active(
    harness, adapter
) -> None:
    run = harness()
    source, observer = adapter
    _observe(source, run)
    run.resolve_error = ProgramRunOwnerOutcomeUnknownError("release reply lost")
    with pytest.raises(ProgramRunnerError) as unknown:
        run.apply("continue_with_failure", "unknown.1")
    assert unknown.value.code == "program_cycle_reconciliation_required"
    assert run.effects == []
    # The release bump is not reached; the existing require_reconciliation
    # may itself advance the revision. The alarm stays because the hold's
    # row is still published, so neither occurrence clears.
    cycle = _observe(source, run)
    assert len(cycle["run"]["operation_failures"]) == 2
    assert all(state != "clear" for state, *_ in observer.states())


@pytest.mark.parametrize("request_id", ["release.1", "release.again"])
def test_replayed_or_duplicate_release_does_not_advance_again(
    harness, adapter, request_id
) -> None:
    run = harness()
    source, observer = adapter
    _observe(source, run)
    run.apply("continue_with_failure", "release.1")
    released = run.revision()
    _observe(source, run)
    # The same command (or a new one) for the already released hold.
    with pytest.raises(ProgramRunnerError):
        run.apply("continue_with_failure", request_id)
    assert run.revision() == released and run.effects == ["attempt.1"]
    _observe(source, run)
    assert observer.states()[2:] == [("clear", "attempt.1", released)]


@pytest.mark.parametrize(
    ("outcome", "action"),
    [("succeeded", "manual_success"), ("failed", "manual_failure")],
)
def test_real_runner_manual_result_advances_once_and_clears_only_its_occurrence(
    harness, adapter, outcome, action
) -> None:
    from manyforge_program_runner.models import ManualResultStageRequest

    run = harness()
    source, observer = adapter
    # The held operation's owner confirmed the non-moving state.
    run.holds["attempt.1"].owner_assessment = {"acknowledged_non_moving": True}
    actions: list[str] = []
    results: list[Any] = []
    release = run.host.resume_recovery

    def resume_recovery(run_id, **kwargs):
        actions.append(kwargs["action"])
        results.append(kwargs.get("result"))
        return release(run_id, **kwargs)

    run.host.resume_recovery = resume_recovery
    activation = _observe(source, run)["execution_control_revision"]
    result = run.runtime.stage_manual_result(
        _RUN,
        ManualResultStageRequest.model_validate(
            {
                "request_id": "manual.1",
                "request_owner_epoch": "client.1",
                "source_label": "test",
                "program_run_id": _RUN,
                "expected_owner_epoch": run.runtime.owner_epoch,
                "expected_run_revision": run.owner.read(_RUN).run_revision,
                "node_instance_id": "move.1",
                "node_attempt_id": "attempt.1",
                "outcome": outcome,
                # SX-610: a failed result never carries Continue's marker,
                # even when a caller sends it as output.
                **({"output": "continue_with_failure"} if outcome == "failed" else {}),
            }
        ),
    )
    assert result["accepted"] is True and actions == [action]
    if outcome == "failed":
        assert results == [None]
    assert run.revision() == activation + 1
    assert run.journal.read().state == "running"
    assert not run.journal.read().reconciliation_required
    _observe(source, run)
    assert observer.states()[2:] == [("clear", "attempt.1", activation + 1)]
    _observe(source, run)
    assert len(observer.calls) == 3


def _stage_manual_success(run, request_id: str):
    from manyforge_program_runner.models import ManualResultStageRequest

    return run.runtime.stage_manual_result(
        _RUN,
        ManualResultStageRequest.model_validate(
            {
                "request_id": request_id,
                "request_owner_epoch": "client.1",
                "source_label": "test",
                "program_run_id": _RUN,
                "expected_owner_epoch": run.runtime.owner_epoch,
                "expected_run_revision": run.owner.read(_RUN).run_revision,
                "node_instance_id": "move.1",
                "node_attempt_id": "attempt.1",
                "outcome": "succeeded",
            }
        ),
    )


_AT_REST = {
    "observed_state_available": True,
    "observed_state_fresh": True,
    "observed_state_at_rest": True,
}


@pytest.mark.parametrize(
    ("fresh", "refresh_error", "reason"),
    [
        ({**_AT_REST, "observed_state_fresh": False, "observed_state_at_rest": False},
         None, "observed_state_stale"),
        ({**_AT_REST, "observed_state_at_rest": False}, None, "observed_state_not_at_rest"),
        ({**_AT_REST, "observed_state_available": False}, None, "observed_state_unavailable"),
        (None, RuntimeError("owner unreachable"), "owner_assessment_unavailable"),
    ],
)
def test_manual_result_rechecks_the_owner_at_the_press(
    harness, adapter, fresh, refresh_error, reason
) -> None:
    """SX-648: offers do not expire, so a manual success re-reads the owner's
    assessment at the press. Old non-moving evidence with a stale, moving or
    unreadable robot state is refused typed, before any release."""

    run = harness()
    source, observer = adapter
    run.holds["attempt.1"].owner_assessment = {
        "acknowledged_non_moving": True,
        "observed_state_available": True,
        "observed_state_valid_until_ns": 0,
    }
    run.fresh, run.refresh_error = fresh, refresh_error
    _observe(source, run)
    before = run.revision()
    with pytest.raises(ProgramRunnerError) as refused:
        _stage_manual_success(run, "manual.stale")
    assert refused.value.code == "recovery_action_rejected"
    assert refused.value.details["reason"] == reason
    assert refused.value.details["action"] == "manual_result"
    assert run.effects == [] and run.revision() == before
    assert not run.journal.read().reconciliation_required


def test_manual_result_with_an_unreadable_owner_deadline_refuses_cleanly(harness, adapter) -> None:
    """SX-677: an owner deadline the exact scalar parser rejects is the typed
    unavailable-assessment refusal: no release, no reconciliation, no 500."""

    run = harness()
    source, _observer = adapter
    run.holds["attempt.1"].owner_assessment = {
        "acknowledged_non_moving": True,
        "observed_state_available": True,
    }
    run.fresh = {**_AT_REST, "observed_state_valid_until_ns": "not-a-number"}
    _observe(source, run)
    before = run.revision()
    with pytest.raises(ProgramRunnerError) as refused:
        _stage_manual_success(run, "manual.invalid")
    assert refused.value.code == "recovery_action_rejected"
    assert refused.value.details["reason"] == "owner_assessment_unavailable"
    assert run.effects == [] and run.revision() == before
    assert not run.journal.read().reconciliation_required


def test_manual_result_with_a_fresh_at_rest_owner_releases_the_hold(harness, adapter) -> None:
    run = harness()
    source, _observer = adapter
    run.holds["attempt.1"].owner_assessment = {
        "acknowledged_non_moving": True,
        "observed_state_available": True,
        "observed_state_valid_until_ns": 0,
    }
    run.fresh = dict(_AT_REST)
    _observe(source, run)
    result = _stage_manual_success(run, "manual.fresh")
    assert result["accepted"] is True and run.effects == ["attempt.1"]
