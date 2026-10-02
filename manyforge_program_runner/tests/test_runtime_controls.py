"""C2 run-scoped recovery and override ledger contracts."""

from __future__ import annotations

from types import MappingProxyType, SimpleNamespace
import time

import pytest

from manyforge_behavior.failure_details import bounded_failure_details, failure_origin
from manyforge_program_runner.runtime_controls import (
    ProgramRuntimeControlError,
    ProgramRuntimeControls,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64


def _capture() -> dict[str, object]:
    return {
        "program_run_id": "program-run.1",
        "program_id": "program.1",
        "program_revision": _DIGEST_A,
        "program_binding_id": _DIGEST_B,
        "retained_bundle_id": _DIGEST_A,
        "graph_revision": _DIGEST_B,
        "node_environment_digest": _DIGEST_A,
    }


def _fences() -> dict[str, int]:
    return {
        "program_generation": 11,
        "deployment_generation": 12,
        "scene_admission_epoch": 13,
        "pose_source_apply_generation": 14,
    }


def _intervention(
    *,
    acknowledged: bool = False,
    policy_parameter: str = "recovery_policy",
):
    assessment = {
        "acknowledged_non_moving": acknowledged,
        "allowed_actions": ["acknowledge_and_continue"],
        "assessment_revision": 7,
        "control_command_epoch": 8,
        "evidence_refs": ["event.1"],
        "execution_id": 9,
        "latch_id": "latch.1",
        "observed_state_valid_until_ns": 0,
        "operation_revision": 10,
        "participants": [{"operation_participant_id": "participant.1"}],
        "recovery_class": "stop_fault_intervention",
        "status": "action_required",
        "stop_certificate_id": "certificate.1",
        "stop_certificate_revision": 2,
        "stop_generation": 3,
        "stop_id": "stop.1",
    }
    return SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id="program-run.1",
            node_instance_id="move.1",
            node_attempt_id="attempt.1",
        ),
        owner_assessment=assessment,
        owner_task_id="operation.1",
        reason_code="motion_intervention",
        message="motion owner refused the path (path_blocked)",
        policy_parameter=policy_parameter,
        # A failure pause carries the owner's bounded failure; an operator
        # Stop is not a failure.
        details=(
            None
            if policy_parameter == "operator_stop_policy"
            else bounded_failure_details({"reason": "path_blocked"})
        ),
        origin=(
            None
            if policy_parameter == "operator_stop_policy"
            else failure_origin(
                "owner", node_instance_id="move.1", owner_api="kernel.operation"
            )
        ),
    )


def _policy() -> dict[str, object]:
    return {
        "allow_intervention": True,
        "allowed_recovery_actions": ["intervention", "runtime_override"],
        "allowed_skills": [],
        "max_retries": 2,
        "persistence_ceiling": "runtime_override",
    }


def test_revision_read_does_not_construct_a_full_snapshot(monkeypatch) -> None:
    controls = ProgramRuntimeControls(_capture())
    controls.audit_value("test_control", value={"nested": [1, 2, 3]})
    expected = controls.snapshot()["runtime_control_revision"]

    def unexpected_copy(_value):
        raise AssertionError("a revision read must not copy the retained control state")

    monkeypatch.setattr(
        "manyforge_program_runner.runtime_controls.deepcopy", unexpected_copy
    )
    for _ in range(1_000):
        assert controls.revision == expected


def test_recent_control_trace_rotates_without_a_lifetime_command_ceiling() -> None:
    controls = ProgramRuntimeControls(_capture())
    for value in range(10_000):
        controls.audit_value("runtime_blackboard_written", key="counter", value=value)
    snapshot = controls.snapshot()
    assert snapshot["schema_version"] == "manyforge.program_runtime_controls.v2"
    assert snapshot["runtime_control_revision"] == 10_001
    assert len(snapshot["audit"]) == 256
    assert snapshot["audit"][0]["event_revision"] == 9_745
    restored = ProgramRuntimeControls.restore(_capture(), snapshot)
    restored.audit_value("after_restart", value=10_000)
    assert restored.snapshot()["audit"][-1]["event_revision"] == 10_001


def test_legacy_control_trace_is_validated_before_bounded_conversion() -> None:
    legacy = ProgramRuntimeControls(_capture()).snapshot()
    legacy["schema_version"] = "manyforge.program_runtime_controls.v1"
    legacy["runtime_control_revision"] = 501
    legacy["audit"] = [
        {"event_revision": index, "kind": "legacy_event"} for index in range(1, 501)
    ]
    restored = ProgramRuntimeControls.restore(_capture(), legacy)
    assert restored.revision == 501
    assert len(restored.snapshot()["audit"]) == 256
    # Reading neither reinterprets nor rewrites the original retained bytes.
    assert len(legacy["audit"]) == 500
    assert legacy["schema_version"].endswith(".v1")
    legacy["audit"][0]["event_revision"] = 99
    with pytest.raises(ValueError, match="audit is invalid"):
        ProgramRuntimeControls.restore(_capture(), legacy)


@pytest.mark.parametrize("damage", ["gap", "empty", "short", "bool_revision"])
def test_recent_control_trace_rejects_gaps_and_false_revision_types(damage) -> None:
    controls = ProgramRuntimeControls(_capture())
    for _ in range(300):
        controls.audit_value("event")
    snapshot = controls.snapshot()
    if damage == "gap":
        snapshot["audit"][0]["event_revision"] += 1
    elif damage == "empty":
        snapshot["audit"] = []
    elif damage == "short":
        snapshot["audit"].pop(0)
    else:
        snapshot["audit"][0]["event_revision"] = True
    with pytest.raises(ValueError):
        ProgramRuntimeControls.restore(_capture(), snapshot)


def _stage(controls, value, *, node="move.1", attempt="attempt.1"):
    return controls.stage_override(
        node_instance_id=node,
        node_attempt_id=attempt,
        parameter_id="velocity_scale",
        value=value,
        source_observation_id=None,
        source_owner_generation=None,
        validator=lambda **values: values["value"],
    )


def test_resolved_overrides_retire_but_pending_promotions_and_current_intent_do_not(
    monkeypatch,
) -> None:
    monkeypatch.setattr("manyforge_program_runner.runtime_controls._MAX_OVERRIDES", 8)
    controls = ProgramRuntimeControls(_capture())
    protected = _stage(controls, 0.25, node="promoted")
    proposal = controls.request_promotion(
        protected["override_id"],
        node_id="motion.move_to_joint_target",
        expected_draft_id="draft.1",
        expected_draft_revision=1,
    )
    controls.clear_override(protected["override_id"], clearer=lambda **_: None)
    staged = _stage(controls, 0.5, node="staged")
    active = _stage(controls, 0.75, node="active")
    controls.activate_override(active["override_id"], activator=lambda **_: None)
    for attempt in range(100):
        row = _stage(controls, 0.1, attempt=f"attempt.{attempt}")
        controls.clear_override(row["override_id"], clearer=lambda **_: None)
        assert len(controls.snapshot()["overrides"]) <= 8
    snapshot = controls.snapshot()
    by_id = {row["override_id"]: row for row in snapshot["overrides"]}
    assert by_id[protected["override_id"]]["status"] == "cleared"
    assert by_id[staged["override_id"]]["status"] == "staged"
    assert by_id[active["override_id"]]["status"] == "active"
    assert snapshot["promotion_proposals"] == [proposal]
    assert ProgramRuntimeControls.restore(_capture(), snapshot).snapshot() == snapshot


def test_capacity_still_limits_simultaneous_unresolved_overrides(monkeypatch) -> None:
    monkeypatch.setattr("manyforge_program_runner.runtime_controls._MAX_OVERRIDES", 2)
    controls = ProgramRuntimeControls(_capture())
    _stage(controls, 0.1, node="first")
    _stage(controls, 0.2, node="second")
    before = controls.snapshot()
    with pytest.raises(ProgramRuntimeControlError) as error:
        _stage(controls, 0.3, node="third")
    assert error.value.code == "program_runtime_override_capacity_full"
    assert controls.snapshot() == before


def test_replacing_active_override_does_not_let_old_clear_erase_new_value() -> None:
    controls = ProgramRuntimeControls(_capture())
    effects = []
    old = _stage(controls, 0.1)
    controls.activate_override(
        old["override_id"], activator=lambda **v: effects.append(v)
    )
    new = _stage(controls, 0.2)

    def reject(**_):
        raise RuntimeError("owner rejected replacement")

    before = controls.snapshot()
    with pytest.raises(RuntimeError, match="owner rejected"):
        controls.activate_override(new["override_id"], activator=reject)
    assert controls.snapshot() == before
    controls.activate_override(
        new["override_id"], activator=lambda **v: effects.append(v)
    )
    controls.clear_override(
        old["override_id"], clearer=lambda **_: pytest.fail("cleared new value")
    )
    assert len(effects) == 2
    active = [
        row for row in controls.snapshot()["overrides"] if row["status"] == "active"
    ]
    assert [row["override_id"] for row in active] == [new["override_id"]]
    again = _stage(controls, 0.1)
    assert again["override_id"] == old["override_id"]
    assert again["status"] == "staged"
    assert again["override_revision"] > old["override_revision"]
    controls.activate_override(
        again["override_id"], activator=lambda **v: effects.append(v)
    )
    assert effects[-1]["value"] == 0.1
    cleared = controls.clear_override(again["override_id"], clearer=lambda **_: None)
    restaged = _stage(controls, 0.1)
    assert restaged["status"] == "staged"
    assert restaged["override_revision"] == cleared["override_revision"] + 1
    assert (
        ProgramRuntimeControls.restore(_capture(), controls.snapshot()).snapshot()
        == controls.snapshot()
    )


def test_retry_state_is_bounded_by_program_nodes_and_reopens_above_old_limit() -> None:
    from manyforge_behavior.program import _MAX_TREE_NODES
    from manyforge_program_runner.runtime_controls import _MAX_RETRY_NODES

    assert _MAX_RETRY_NODES == _MAX_TREE_NODES
    controls = ProgramRuntimeControls(_capture())
    for index in range(1_025):
        controls.note_retry(f"node.{index}")
    snapshot = controls.snapshot()
    assert len(snapshot["retry_counts"]) == 1_025
    assert ProgramRuntimeControls.restore(_capture(), snapshot).snapshot() == snapshot


def test_retry_preflight_does_not_evict_an_exhausted_policy_allowance(
    monkeypatch,
) -> None:
    monkeypatch.setattr("manyforge_program_runner.runtime_controls._MAX_RETRY_NODES", 2)
    controls = ProgramRuntimeControls(_capture())
    controls.note_retry("first")
    controls.note_retry("second")
    before = controls.snapshot()
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("third")
    assert error.value.code == "program_runtime_retry_nodes_full"
    assert controls.snapshot() == before
    for _ in range(254):
        controls.note_retry("first")
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("first")
    assert error.value.code == "program_runtime_retry_limit_reached"
    assert controls.retry_count("first") == 255


def test_one_reset_owner_event_includes_attribution_and_deduplicates_phase_retry() -> (
    None
):
    controls = ProgramRuntimeControls(_capture())
    _stage(controls, 0.5)
    request = {
        "request_id": "reset.1",
        "source_label": "operator",
        "correlation_id": "action.1",
    }
    controls.mark_reset(**request)
    snapshot = controls.snapshot()
    controls.mark_reset(**request)
    assert controls.snapshot() == snapshot
    controls.mark_reset(**{**request, "request_id": "reset.retry.2"})
    assert controls.snapshot() == snapshot
    assert snapshot["audit"][-1] == {
        "event_revision": 2,
        "kind": "program_runtime_controls_reset",
        "action_id": "reset_cycle",
        **request,
    }


def test_checked_revision_exhaustion_precedes_effect_and_preserves_reset() -> None:
    controls = ProgramRuntimeControls(_capture())
    staged = _stage(controls, 0.5)
    snapshot = controls.snapshot()
    maximum = 9_007_199_254_740_991
    snapshot["runtime_control_revision"] = maximum - 1
    snapshot["audit"] = [
        {"event_revision": index, "kind": "prior_event"}
        for index in range(maximum - 257, maximum - 1)
    ]
    restored = ProgramRuntimeControls.restore(_capture(), snapshot)
    with pytest.raises(ProgramRuntimeControlError) as error:
        restored.activate_override(
            staged["override_id"], activator=lambda **_: pytest.fail("effect")
        )
    assert error.value.code == "program_runtime_control_revision_exhausted"
    assert restored.snapshot() == snapshot
    restored.mark_reset()
    assert restored.revision == maximum
    assert restored.snapshot()["overrides"][0]["status"] == "stale"


def test_override_lifecycle_is_exact_idempotent_and_restart_safe() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    activations: list[dict[str, object]] = []
    clears: list[dict[str, object]] = []

    staged = controls.stage_override(
        node_instance_id="move.1",
        node_attempt_id="attempt.1",
        parameter_id="velocity_scale",
        value=0.5,
        source_observation_id="observation.1",
        source_owner_generation=4,
        validator=lambda **values: values["value"],
    )
    assert (
        controls.stage_override(
            node_instance_id="move.1",
            node_attempt_id="attempt.1",
            parameter_id="velocity_scale",
            value=0.5,
            source_observation_id="observation.1",
            source_owner_generation=4,
            validator=lambda **values: values["value"],
        )
        == staged
    )

    active = controls.activate_override(
        staged["override_id"], activator=lambda **values: activations.append(values)
    )
    assert active["status"] == "active"
    assert len(activations) == 1
    assert (
        controls.activate_override(
            staged["override_id"], activator=lambda **values: activations.append(values)
        )
        == active
    )
    assert len(activations) == 1

    restored = ProgramRuntimeControls.restore(_capture(), controls.snapshot())
    cleared = restored.clear_override(
        staged["override_id"], clearer=lambda **values: clears.append(values)
    )
    assert cleared["status"] == "cleared"
    assert clears == [{"node_instance_id": "move.1", "parameter_id": "velocity_scale"}]


def test_promotion_becomes_materializable_only_after_reset_and_never_applies() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    staged = controls.stage_override(
        node_instance_id="move.1",
        node_attempt_id="attempt.1",
        parameter_id="velocity_scale",
        value=0.25,
        source_observation_id=None,
        source_owner_generation=None,
        validator=lambda **values: values["value"],
    )
    proposal = controls.request_promotion(
        staged["override_id"],
        node_id="motion.move_to_joint_target",
        expected_draft_id="draft.1",
        expected_draft_revision=5,
    )
    assert proposal["status"] == "awaiting_reset"
    controls.mark_reset()
    snapshot = controls.snapshot()
    assert snapshot["overrides"][0]["status"] == "stale"
    assert snapshot["promotion_proposals"][0]["status"] == "ready"
    assert snapshot["promotion_proposals"][0]["proposal_id"] == (
        proposal["proposal_id"]
    )


def test_recovery_offer_is_stable_policy_filtered_and_generation_fenced() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(),
        policy=_policy(),
    )
    assert offer is not None
    assert offer["schema_version"] == "manyforge.cycle_recovery_offer.v3"
    # The held node's own failure is carried unchanged.
    assert offer["reason_code"] == "motion_intervention"
    assert offer["message"] == "motion owner refused the path (path_blocked)"
    assert offer["details"] == {"reason": "path_blocked"}
    assert offer["origin"] == {
        "kind": "owner",
        "node_instance_id": "move.1",
        "owner_api": "kernel.operation",
    }
    assert offer["valid_until_ns"] is None
    assert offer["primary_action"] == "acknowledge_and_continue"
    assert all(offer[field] == value for field, value in _fences().items())
    assert (
        controls.recovery_offer(
            cycle_generation=6,
            owner_epoch="runner.epoch.1",
            intervention=_intervention(),
            policy=_policy(),
        )
        == offer
    )

    forbidden = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(),
        policy={**_policy(), "allow_intervention": False},
    )
    assert forbidden is not None
    assert forbidden["offer_revision"] == offer["offer_revision"] + 1
    assert forbidden["primary_action"] is None
    assert all(not action["enabled"] for action in forbidden["actions"])

    acknowledged = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(acknowledged=True),
        policy=_policy(),
    )
    assert acknowledged is not None
    assert acknowledged["status"] == "retry_ready"
    assert any(
        action["action_id"] == "retry_runtime_step" and action["enabled"]
        for action in acknowledged["actions"]
    )


def test_failure_retry_waits_for_confirmed_containment() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    confirmed = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(acknowledged=True),
        policy=_policy(),
    )
    pending = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(acknowledged=True),
        policy=_policy(),
        containment_pending=True,
    )
    assert confirmed is not None and pending is not None
    retry = next(
        action for action in confirmed["actions"]
        if action["action_id"] == "retry_runtime_step"
    )
    assert retry["enabled"] is True
    # A different offer: an earlier Retry click cannot land on it.
    assert pending["offer_id"] != confirmed["offer_id"]
    for action in pending["actions"]:
        if action["action_id"] not in {"acknowledge_and_continue", "reset_cycle"}:
            assert action["enabled"] is False
            assert action["disabled_reason_code"] == (
                "failure_pause_containment_unconfirmed"
            )
    assert pending["primary_action"] != "retry_runtime_step"


def test_operator_stop_recovery_uses_owner_fence_not_failure_retry_policy() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(
        acknowledged=True,
        policy_parameter="operator_stop_policy",
    )
    intervention.owner_assessment["allowed_actions"] = [
        "replan_from_stopped_state",
        "reset_cycle",
    ]
    intervention.owner_assessment.update(
        {
            "backend_stop_confirmed": True,
            "execution_state": "Stopped",
            "recovery_class": "ordinary_operator_stop",
            "retry_admissible": True,
            "status": "retry_ready",
            "stop_owner_generation": 3,
        }
    )
    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={**_policy(), "allow_intervention": False, "max_retries": 0},
    )
    assert offer is not None
    action = next(
        row
        for row in offer["actions"]
        if row["action_id"] == "replan_from_stopped_state"
    )
    assert action["enabled"] is True
    reset = next(row for row in offer["actions"] if row["action_id"] == "reset_cycle")
    assert reset == {
        "action_id": "reset_cycle",
        "label_key": "recovery.reset_cycle",
        "requires_confirmation": True,
        "performs_backend_reset": True,
        "enabled": True,
        "disabled_reason_code": None,
    }
    assert offer["manual_result"] == {
        "enabled": False,
        "disabled_reason_code": "operator_stop_requires_owner_recovery",
    }
    # An operator Stop is not a failure: no details, no origin.
    assert offer["details"] is None and offer["origin"] is None
    selected = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=intervention,
        policy=_policy(), selection_revision=3,
    )
    newer = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=intervention,
        policy=_policy(), selection_revision=4,
    )
    assert newer["offer_id"] != selected["offer_id"]
    assert newer["offer_revision"] == selected["offer_revision"] + 1


def test_operator_stop_reset_waits_for_proven_non_moving_state() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment["allowed_actions"] = [
        "replan_from_stopped_state",
        "reset_cycle",
    ]

    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={**_policy(), "allow_intervention": False, "max_retries": 0},
    )

    assert offer is not None
    reset = next(row for row in offer["actions"] if row["action_id"] == "reset_cycle")
    assert reset["enabled"] is False
    assert reset["disabled_reason_code"] == ("operator_stop_not_physically_settled")

    intervention.owner_assessment.update(
        {
            "backend_stop_confirmed": True,
            "execution_state": "Stopped",
            "recovery_class": "ordinary_operator_stop",
            "retry_admissible": True,
            "status": "retry_ready",
            "stop_owner_generation": 3,
            "observed_state_valid_until_ns": time.monotonic_ns() - 1,
        }
    )
    expired = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={**_policy(), "allow_intervention": False, "max_retries": 0},
    )
    assert expired is not None
    expired_reset = next(
        row for row in expired["actions"] if row["action_id"] == "reset_cycle"
    )
    assert expired_reset["enabled"] is False
    assert expired_reset["disabled_reason_code"] == (
        "recovery_owner_observation_expired"
    )


def test_released_operator_stop_projects_no_stale_offer() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment.update(
        {
            "allowed_actions": [],
            "recovery_class": "none",
            "status": "unavailable",
            "stop_id": "",
            "stop_generation": 0,
            "stop_certificate_id": "",
            "stop_certificate_revision": 0,
        }
    )

    assert (
        controls.recovery_offer(
            cycle_generation=6,
            owner_epoch="runner.epoch.1",
            intervention=intervention,
            policy={**_policy(), "allow_intervention": False, "max_retries": 0},
        )
        is None
    )


def test_recovery_offer_normalizes_absent_stop_identity_to_null() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()
    intervention.owner_assessment.update(
        {
            "stop_id": "",
            "stop_generation": 0,
            "stop_certificate_id": "",
            "stop_certificate_revision": 0,
        }
    )

    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )

    assert offer is not None
    assert offer["stop_id"] is None
    assert offer["stop_generation"] is None
    assert offer["stop_certificate_id"] is None
    assert offer["stop_certificate_revision"] is None


def test_recovery_offer_projects_immutable_owner_facts_and_policy() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment = MappingProxyType(
        {
            **intervention.owner_assessment,
            "allowed_actions": ("retry_motion_from_current_state",),
            "participants": (
                MappingProxyType({"operation_participant_id": "participant.1"}),
            ),
            "recovery_class": "ordinary_operator_stop",
            "status": "retry_ready",
        }
    )
    policy = MappingProxyType(
        {
            **_policy(),
            "allowed_recovery_actions": ("intervention",),
        }
    )

    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=policy,
    )

    assert offer is not None
    assert offer["participants"] == [{"operation_participant_id": "participant.1"}]
    assert offer["policy"]["allowed_recovery_actions"] == ["intervention"]
    assert offer["primary_action"] == "retry_motion_from_current_state"


def test_recovery_fence_shape_is_closed() -> None:
    with pytest.raises(ValueError, match="exact recovery fences"):
        ProgramRuntimeControls(
            _capture(),
            recovery_fences={**_fences(), "browser_snapshot": 99},
        )


def test_owner_backed_retry_requires_confirmed_non_moving_state() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()
    intervention.owner_assessment = None
    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert offer is not None
    assert all(
        action["action_id"] != "retry_runtime_step" for action in offer["actions"]
    )

    intervention.owner_task_id = None
    ownerless = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert ownerless is not None
    assert any(
        action["action_id"] == "retry_runtime_step" and action["enabled"]
        for action in ownerless["actions"]
    )


def test_runtime_override_only_policy_requires_an_active_node_override() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()
    intervention.owner_task_id = None
    intervention.owner_assessment = None
    policy = {
        **_policy(),
        "allow_intervention": False,
        "allowed_recovery_actions": ["runtime_override"],
    }
    before = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=policy,
    )
    assert before is not None
    assert all(
        action["action_id"] != "retry_runtime_step" for action in before["actions"]
    )

    staged = controls.stage_override(
        node_instance_id="move.1",
        node_attempt_id="attempt.1",
        parameter_id="velocity_scale",
        value=0.5,
        source_observation_id=None,
        source_owner_generation=None,
        validator=lambda **values: values["value"],
    )
    controls.activate_override(staged["override_id"], activator=lambda **_values: None)
    intervention.identity = SimpleNamespace(
        program_run_id="program-run.1",
        node_instance_id="move.1",
        node_attempt_id="attempt.2",
    )
    after = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=policy,
    )
    assert after is not None
    assert any(
        action["action_id"] == "retry_runtime_step" and action["enabled"]
        for action in after["actions"]
    )


def test_manual_result_requires_policy_and_fresh_non_moving_owner_evidence() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()

    waiting = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert waiting is not None
    assert waiting["manual_result"] == {
        "enabled": False,
        "disabled_reason_code": "manual_result_owner_not_acknowledged",
    }

    intervention.owner_assessment["acknowledged_non_moving"] = True
    ready = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert ready is not None
    assert ready["manual_result"] == {
        "enabled": True,
        "disabled_reason_code": None,
    }

    intervention.owner_assessment["observed_state_valid_until_ns"] = (
        time.monotonic_ns() - 1
    )
    expired = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert expired is not None
    assert expired["manual_result"] == {
        "enabled": False,
        "disabled_reason_code": "manual_result_owner_observation_expired",
    }


def test_expired_owner_observation_disables_every_recovery_effect() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(acknowledged=True)
    intervention.owner_assessment["observed_state_valid_until_ns"] = (
        time.monotonic_ns() - 1
    )

    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )

    assert offer is not None
    assert offer["primary_action"] is None
    assert all(not action["enabled"] for action in offer["actions"])
    assert {
        action["disabled_reason_code"]
        for action in offer["actions"]
        if action["action_id"] != "reset_cycle"
    } == {"recovery_owner_observation_expired"}
