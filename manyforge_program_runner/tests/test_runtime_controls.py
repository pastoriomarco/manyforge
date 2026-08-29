"""C2 run-scoped recovery and override ledger contracts."""

from __future__ import annotations

from types import MappingProxyType, SimpleNamespace
import time

import pytest

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
        policy_parameter=policy_parameter,
    )


def _policy() -> dict[str, object]:
    return {
        "allow_intervention": True,
        "allowed_recovery_actions": ["intervention", "runtime_override"],
        "allowed_skills": [],
        "max_retries": 2,
        "persistence_ceiling": "runtime_override",
    }


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
    assert controls.stage_override(
        node_instance_id="move.1",
        node_attempt_id="attempt.1",
        parameter_id="velocity_scale",
        value=0.5,
        source_observation_id="observation.1",
        source_owner_generation=4,
        validator=lambda **values: values["value"],
    ) == staged

    active = controls.activate_override(
        staged["override_id"], activator=lambda **values: activations.append(values)
    )
    assert active["status"] == "active"
    assert len(activations) == 1
    assert controls.activate_override(
        staged["override_id"], activator=lambda **values: activations.append(values)
    ) == active
    assert len(activations) == 1

    restored = ProgramRuntimeControls.restore(_capture(), controls.snapshot())
    cleared = restored.clear_override(
        staged["override_id"], clearer=lambda **values: clears.append(values)
    )
    assert cleared["status"] == "cleared"
    assert clears == [
        {"node_instance_id": "move.1", "parameter_id": "velocity_scale"}
    ]


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
    assert offer["valid_until_ns"] is None
    assert offer["primary_action"] == "acknowledge_and_continue"
    assert all(offer[field] == value for field, value in _fences().items())
    assert controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(),
        policy=_policy(),
    ) == offer

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


def test_operator_stop_recovery_uses_owner_fence_not_failure_retry_policy() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment["allowed_actions"] = [
        "replan_from_stopped_state"
    ]
    intervention.owner_assessment["recovery_class"] = "ordinary_operator_stop"
    intervention.owner_assessment["status"] = "retry_ready"
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
    assert offer["manual_result"] == {
        "enabled": False,
        "disabled_reason_code": "operator_stop_requires_owner_recovery",
    }


def test_recovery_offer_projects_immutable_owner_facts_and_policy() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment = MappingProxyType(
        {
            **intervention.owner_assessment,
            "allowed_actions": ("retry_motion_from_current_state",),
            "participants": (
                MappingProxyType(
                    {"operation_participant_id": "participant.1"}
                ),
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
    assert offer["participants"] == [
        {"operation_participant_id": "participant.1"}
    ]
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
        action["action_id"] != "retry_runtime_step"
        for action in offer["actions"]
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
        action["action_id"] != "retry_runtime_step"
        for action in before["actions"]
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
