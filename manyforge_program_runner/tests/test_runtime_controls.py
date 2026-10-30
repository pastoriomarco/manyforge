"""C2 run-scoped recovery ledger contracts."""

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
    return {"allow_intervention": True, "max_retries": 2}


def _first_enabled(offer: dict[str, object]) -> str | None:
    """The first enabled action of an offer, in the producer's order."""
    return next((row["action_id"] for row in offer["actions"] if row["enabled"]), None)


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
    assert snapshot["schema_version"] == "manyforge.program_runtime_controls.v3"
    assert snapshot["runtime_control_revision"] == 10_001
    assert len(snapshot["audit"]) == 256
    assert snapshot["audit"][0]["event_revision"] == 9_745
    restored = ProgramRuntimeControls.restore(_capture(), snapshot)
    restored.audit_value("after_restart", value=10_000)
    assert restored.snapshot()["audit"][-1]["event_revision"] == 10_001


@pytest.mark.parametrize(
    "schema_version",
    [
        "manyforge.program_runtime_controls.v2",
        "manyforge.program_runtime_controls.v1",
    ],
)
def test_superseded_control_snapshots_are_refused_not_converted(
    schema_version: str,
) -> None:
    superseded = {
        **ProgramRuntimeControls(_capture()).snapshot(),
        "schema_version": schema_version,
        "has_active_runtime_overrides": False,
        "has_staged_runtime_overrides": False,
        "has_stale_runtime_overrides": False,
        "overrides": [],
        "promotion_proposals": [],
    }
    with pytest.raises(ValueError, match="unsupported schema"):
        ProgramRuntimeControls.restore(_capture(), superseded)
    current = ProgramRuntimeControls(_capture()).snapshot()
    current["overrides"] = []
    with pytest.raises(ValueError, match="invalid shape"):
        ProgramRuntimeControls.restore(_capture(), current)


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
    unlimited = {"allow_intervention": True, "max_retries": 0}
    controls.note_retry("first")
    controls.note_retry("second")
    before = controls.snapshot()
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("third", unlimited)
    assert error.value.code == "program_runtime_retry_count_exhausted"
    assert controls.snapshot() == before
    with pytest.raises(ProgramRuntimeControlError):
        controls.note_retry("third")
    assert controls.snapshot() == before


def test_positive_budget_is_exhausted_explicitly_for_offer_and_command() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()
    intervention.owner_task_id = None
    intervention.owner_assessment = None
    policy = {"allow_intervention": True, "max_retries": 2}
    for used in range(2):
        assert controls.retry_permitted("move.1", policy) is True
        controls.require_retry_capacity("move.1", policy)
        offer = controls.recovery_offer(
            cycle_generation=6,
            owner_epoch="runner.epoch.1",
            intervention=intervention,
            policy=policy,
        )
        assert offer["retries_used"] == used
        assert _first_enabled(offer) == "retry_runtime_step"
        assert controls.note_retry("move.1") == used + 1
    assert controls.retry_permitted("move.1", policy) is False
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("move.1", policy)
    assert error.value.code == "program_runtime_retry_limit_reached"
    exhausted = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=policy,
    )
    assert exhausted["retries_used"] == 2
    assert all(
        action["action_id"] != "retry_runtime_step" for action in exhausted["actions"]
    )
    # Exhaustion survives retained-state reopen; another node has its own count.
    reopened = ProgramRuntimeControls.restore(_capture(), controls.snapshot())
    assert reopened.retry_permitted("move.1", policy) is False
    assert reopened.retry_permitted("move.2", policy) is True


def test_zero_budget_is_unlimited_beyond_255_across_reopen() -> None:
    controls = ProgramRuntimeControls(_capture())
    unlimited = {"allow_intervention": True, "max_retries": 0}
    for _ in range(300):
        controls.require_retry_capacity("move.1", unlimited)
        controls.note_retry("move.1")
    assert controls.retry_count("move.1") == 300
    reopened = ProgramRuntimeControls.restore(_capture(), controls.snapshot())
    assert reopened.retry_count("move.1") == 300
    reopened.require_retry_capacity("move.1", unlimited)
    assert reopened.note_retry("move.1") == 301
    # Unlimited never enables a forbidden intervention.
    forbidden = {"allow_intervention": False, "max_retries": 0}
    assert reopened.retry_permitted("move.1", forbidden) is False
    with pytest.raises(ProgramRuntimeControlError) as error:
        reopened.require_retry_capacity("move.1", forbidden)
    assert error.value.code == "program_runtime_retry_forbidden_by_policy"


def test_retry_counter_exhaustion_is_explicit_never_wraparound(monkeypatch) -> None:
    monkeypatch.setattr("manyforge_program_runner.runtime_controls._MAX_RETRY_COUNT", 3)
    controls = ProgramRuntimeControls(_capture())
    unlimited = {"allow_intervention": True, "max_retries": 0}
    for _ in range(3):
        controls.note_retry("move.1")
    before = controls.snapshot()
    assert controls.retry_permitted("move.1", unlimited) is False
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("move.1", unlimited)
    assert error.value.code == "program_runtime_retry_count_exhausted"
    with pytest.raises(ProgramRuntimeControlError):
        controls.note_retry("move.1")
    assert controls.snapshot() == before


@pytest.mark.parametrize("count", [0, -1, True, 2**53, 1.0])
def test_retained_retry_counts_are_exact_positive_integers(count) -> None:
    snapshot = ProgramRuntimeControls(_capture()).snapshot()
    snapshot["retry_counts"] = {"move.1": count}
    with pytest.raises(ValueError, match="retry count is invalid"):
        ProgramRuntimeControls.restore(_capture(), snapshot)


def test_one_reset_owner_event_includes_attribution_and_deduplicates_phase_retry() -> (
    None
):
    controls = ProgramRuntimeControls(_capture())
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
        "event_revision": 1,
        "kind": "program_runtime_controls_reset",
        "action_id": "reset_cycle",
        **request,
    }


def test_checked_revision_exhaustion_precedes_effect_and_preserves_reset() -> None:
    controls = ProgramRuntimeControls(_capture())
    controls.note_retry("move.1")
    snapshot = controls.snapshot()
    maximum = 9_007_199_254_740_991
    snapshot["runtime_control_revision"] = maximum - 1
    snapshot["audit"] = [
        {"event_revision": index, "kind": "prior_event"}
        for index in range(maximum - 257, maximum - 1)
    ]
    restored = ProgramRuntimeControls.restore(_capture(), snapshot)
    with pytest.raises(ProgramRuntimeControlError) as error:
        restored.note_retry("move.1")
    assert error.value.code == "program_runtime_control_revision_exhausted"
    assert restored.snapshot() == snapshot
    restored.mark_reset(request_id="reset.1")
    assert restored.revision == maximum


def test_recovery_offer_is_stable_policy_filtered_and_generation_fenced() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(),
        policy=_policy(),
    )
    assert offer is not None
    assert offer["schema_version"] == "manyforge.cycle_recovery_offer.v5"
    assert offer["policy"] == _policy()
    assert offer["retries_used"] == 0
    # The held node's own failure is carried unchanged.
    assert offer["reason_code"] == "motion_intervention"
    assert offer["message"] == "motion owner refused the path (path_blocked)"
    assert offer["details"] == {"reason": "path_blocked"}
    assert offer["origin"] == {
        "kind": "owner",
        "node_instance_id": "move.1",
        "owner_api": "kernel.operation",
    }
    assert "valid_until_ns" not in offer
    assert _first_enabled(offer) == "acknowledge_and_continue"
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
    assert _first_enabled(forbidden) is None
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
    assert _first_enabled(pending) != "retry_runtime_step"


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
            # The Stop-time sample is already past the Kernel's freshness
            # budget when the offer is built (the live "born expired" case).
            "observed_state_valid_until_ns": time.monotonic_ns() - 1,
        }
    )
    settled = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={**_policy(), "allow_intervention": False, "max_retries": 0},
    )
    assert settled is not None
    assert "valid_until_ns" not in settled
    settled_actions = {row["action_id"]: row for row in settled["actions"]}
    # Offers do not expire: freshness is checked when the action is pressed.
    assert settled_actions["reset_cycle"]["enabled"] is True
    assert settled_actions["reset_cycle"]["disabled_reason_code"] is None
    assert settled_actions["replan_from_stopped_state"]["enabled"] is True


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
    policy = MappingProxyType(_policy())

    offer = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=policy,
    )

    assert offer is not None
    assert offer["participants"] == [{"operation_participant_id": "participant.1"}]
    assert offer["policy"] == {"allow_intervention": True, "max_retries": 2}
    assert _first_enabled(offer) == "retry_motion_from_current_state"


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


def test_manual_result_requires_policy_and_non_moving_owner_evidence() -> None:
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

    # A past owner observation deadline no longer disables it.
    intervention.owner_assessment["observed_state_valid_until_ns"] = (
        time.monotonic_ns() - 1
    )
    held = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
    assert held is not None
    assert held["manual_result"] == {
        "enabled": True,
        "disabled_reason_code": None,
    }


def test_past_owner_observation_deadline_keeps_every_recovery_action() -> None:
    """Offers do not expire while the run is held: a past Kernel observation
    deadline neither disables an action nor withdraws Retry."""

    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(acknowledged=True)
    fresh = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy=_policy(),
    )
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
    assert "valid_until_ns" not in offer
    # The deadline is not offer semantics: same offer, same command fence.
    assert offer["offer_id"] == fresh["offer_id"]
    assert offer["offer_revision"] == fresh["offer_revision"]
    assert offer["actions"] == fresh["actions"]
    assert _first_enabled(offer) == "acknowledge_and_continue"
    assert any(
        action["action_id"] == "retry_runtime_step" and action["enabled"]
        for action in offer["actions"]
    )
    assert offer["manual_result"]["enabled"] is True


def test_never_dispatched_owner_failure_offers_retry_under_captured_policy() -> None:
    """A held pre-dispatch failure (IK had no solution) never moved the robot.

    The owner classifies it as a retryable no-effect failure; the offer then
    enables Retry exactly by the captured policy predicate, with Unlimited
    (``max_retries == 0``) allowing it and a forbidden intervention not.
    """

    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention()
    intervention.owner_assessment = {
        "acknowledged_non_moving": False,
        "allowed_actions": ["retry_after_scene_repair", "reset_cycle"],
        "assessment_revision": 15,
        "control_command_epoch": 0,
        "evidence_refs": [],
        "execution_id": 22,
        "latch_id": "",
        "observed_state_valid_until_ns": 0,
        "operation_revision": 1,
        "participants": [{"operation_participant_id": "participant.1"}],
        "recovery_class": "failed_validation",
        "retry_admissible": True,
        "status": "retry_ready",
    }
    intervention.reason_code = "ik_failed"

    offer = controls.recovery_offer(
        cycle_generation=5,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={"allow_intervention": True, "max_retries": 0},
    )

    assert offer is not None
    assert offer["status"] == "retry_ready"
    assert offer["recovery_class"] == "failed_validation"
    assert _first_enabled(offer) == "retry_after_scene_repair"
    retry = next(
        action
        for action in offer["actions"]
        if action["action_id"] == "retry_after_scene_repair"
    )
    assert retry["enabled"] is True
    assert retry["disabled_reason_code"] is None

    forbidden = controls.recovery_offer(
        cycle_generation=5,
        owner_epoch="runner.epoch.1",
        intervention=intervention,
        policy={"allow_intervention": False, "max_retries": 0},
    )
    assert forbidden is not None
    assert _first_enabled(forbidden) is None
    assert {
        action["disabled_reason_code"]
        for action in forbidden["actions"]
        if action["action_id"] == "retry_after_scene_repair"
    } == {"recovery_action_forbidden_by_captured_policy"}


def _continue_action(offer):
    return next(
        action for action in offer["actions"] if action["action_id"] == "continue_with_failure"
    )


def test_continue_is_offered_for_failure_holds_with_confirmation_and_close_fact() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    held = _intervention()
    not_closable = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=held, policy=_policy()
    )
    action = _continue_action(not_closable)
    assert action["requires_confirmation"] is True
    assert action["performs_backend_reset"] is False
    assert action["enabled"] is False
    assert action["disabled_reason_code"] == "recovery_close_not_eligible"

    closable = _intervention()
    closable.owner_assessment = {**closable.owner_assessment, "eligible_close": True}
    offer = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=closable, policy=_policy()
    )
    assert _continue_action(offer)["enabled"] is True
    # Continue releases motion: it waits for confirmed containment like Retry.
    pending = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=closable,
        policy=_policy(),
        containment_pending=True,
    )
    assert _continue_action(pending)["enabled"] is False
    assert _continue_action(pending)["disabled_reason_code"] == (
        "failure_pause_containment_unconfirmed"
    )
    # A step without an owner task has nothing to close.
    logic = _intervention()
    logic.owner_task_id = None
    logic.owner_assessment = None
    assert _continue_action(
        controls.recovery_offer(
            cycle_generation=6, owner_epoch="runner.epoch.1", intervention=logic, policy=_policy()
        )
    )["enabled"] is True
    stop = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=_intervention(policy_parameter="operator_stop_policy"),
        policy=_policy(),
    )
    assert all(row["action_id"] != "continue_with_failure" for row in stop["actions"])


def test_f6a_program_stop_failure_hold_offers_retry_continue_and_reset() -> None:
    """(d) A Program-requested Stop held by an intervention recovery_policy:
    Retry from the stopped state (bounded by max_retries), Continue (the node
    fails, the tree goes on into its Fallback) and Reset are offered."""

    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    held = _intervention()
    held.reason_code = "interrupt_fault"
    held.owner_assessment = {
        **held.owner_assessment,
        "allowed_actions": ["replan_from_stopped_state", "reset_cycle"],
        "eligible_close": True,
        "latch_id": "",
        "recovery_class": "ordinary_program_stop",
        "status": "retry_ready",
    }
    policy = {"allow_intervention": True, "max_retries": 1}
    offer = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=held, policy=policy
    )
    actions = {row["action_id"]: row for row in offer["actions"]}
    assert actions["replan_from_stopped_state"]["enabled"] is True
    assert _continue_action(offer)["enabled"] is True
    assert "reset_cycle" in actions
    assert offer["recovery_class"] == "ordinary_program_stop"
    controls.note_retry("move.1")  # The one allowed Retry is used.
    exhausted = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=held, policy=policy
    )
    retry = next(
        row for row in exhausted["actions"] if row["action_id"] == "replan_from_stopped_state"
    )
    assert retry["enabled"] is False
    assert retry["disabled_reason_code"] == "recovery_retry_limit_reached"
    assert _continue_action(exhausted)["enabled"] is True


def test_item11_unconfirmed_stop_hold_offers_continue_as_its_acknowledge() -> None:
    """Item 11: an unconfirmed Program Stop holds whatever the node's policy,
    on an undecided assessment. Its Continue (the operator's Acknowledge) is
    enabled anyway, still with confirmation, and still waits for confirmed
    containment like any Continue."""

    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    held = _intervention()
    held.reason_code = "program_stop_settlement_unconfirmed"
    held.owner_assessment = {
        **held.owner_assessment,
        "eligible_close": False,
        "observed_state_valid_until_ns": time.monotonic_ns() - 1,
        "recovery_class": "ordinary_program_stop",
        "status": "waiting",
    }
    default_policy = {"allow_intervention": False, "max_retries": 0}
    offer = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=held, policy=default_policy
    )
    action = _continue_action(offer)
    assert action["enabled"] is True
    assert action["disabled_reason_code"] is None
    assert action["requires_confirmation"] is True
    assert offer["reason_code"] == "program_stop_settlement_unconfirmed"
    pending = controls.recovery_offer(
        cycle_generation=6,
        owner_epoch="runner.epoch.1",
        intervention=held,
        policy=default_policy,
        containment_pending=True,
    )
    assert _continue_action(pending)["enabled"] is False
    assert _continue_action(pending)["disabled_reason_code"] == (
        "failure_pause_containment_unconfirmed"
    )
    # Any other hold keeps its policy and close gates.
    held.reason_code = "interrupt_fault"
    other = controls.recovery_offer(
        cycle_generation=6, owner_epoch="runner.epoch.1", intervention=held, policy=default_policy
    )
    assert _continue_action(other)["enabled"] is False


def test_each_held_node_keeps_its_own_stable_offer() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    first = _intervention()
    second = _intervention()
    second.identity = SimpleNamespace(
        program_run_id="program-run.1", node_instance_id="move.2", node_attempt_id="attempt.2"
    )

    def offer(intervention):
        return controls.recovery_offer(
            cycle_generation=6, owner_epoch="runner.epoch.1", intervention=intervention, policy=_policy()
        )

    one, two = offer(first), offer(second)
    assert one["node_instance_id"] == "move.1" and two["node_instance_id"] == "move.2"
    assert one["offer_id"] != two["offer_id"]
    # Publishing the other hold does not churn this hold's offer revision.
    assert offer(first)["offer_revision"] == one["offer_revision"]
    assert offer(second)["offer_revision"] == two["offer_revision"]
