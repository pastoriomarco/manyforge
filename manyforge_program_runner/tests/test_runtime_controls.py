"""Run-scoped recovery ledger contracts."""

from __future__ import annotations

import hashlib

from types import MappingProxyType, SimpleNamespace
import time

import pytest

from manyforge_behavior.failure_details import bounded_failure_details, failure_origin
from manyforge_program_runner.runtime_controls import (
    ProgramRuntimeControlError,
    ProgramRuntimeControls,
)


def _kb(*parts) -> bytes:
    """A deterministic 16-byte request key named by text, for readable tests."""

    return hashlib.md5("/".join(map(str, parts)).encode()).digest()


def _key(*parts) -> str:
    """The wire form (32 lowercase hex) of ``_kb``."""

    return _kb(*parts).hex()


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64


def _capture() -> dict[str, object]:
    return {
        "program_run_id": 1,
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
        "observed_state_valid_until_ns": 0,
        "operation_revision": 10,
        "participants": [{"operation_participant_id": {"operation_id": 1, "ordinal": 1}}],
        "recovery_class": "ordinary_operator_stop",
        "status": "action_required",
        "stop_event_id": 1,
    }
    return SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1,
            node_instance_id="move.1",
            node_attempt_id=1,
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
    assert snapshot["schema_version"] == "manyforge.program_runtime_controls.v4"
    assert snapshot["runtime_control_revision"] == 10_001
    assert len(snapshot["audit"]) == 256
    assert snapshot["audit"][0]["event_revision"] == 9_745
    controls.audit_value("after_rotation", value=10_000)
    assert controls.snapshot()["audit"][-1]["event_revision"] == 10_001


def test_retry_state_is_bounded_by_program_nodes_above_old_limit() -> None:
    from manyforge_behavior.program import _MAX_TREE_NODES
    from manyforge_program_runner.runtime_controls import _MAX_RETRY_NODES

    assert _MAX_RETRY_NODES == _MAX_TREE_NODES
    controls = ProgramRuntimeControls(_capture())
    for index in range(1_025):
        controls.note_retry(f"node.{index}")
    snapshot = controls.snapshot()
    assert len(snapshot["retry_counts"]) == 1_025


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
            application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
        intervention=intervention,
        policy=policy,
    )
    assert exhausted["retries_used"] == 2
    assert all(
        action["action_id"] != "retry_runtime_step" for action in exhausted["actions"]
    )
    # Another node has its own count.
    assert controls.retry_permitted("move.2", policy) is True


def test_zero_budget_is_unlimited_beyond_255() -> None:
    controls = ProgramRuntimeControls(_capture())
    unlimited = {"allow_intervention": True, "max_retries": 0}
    for _ in range(300):
        controls.require_retry_capacity("move.1", unlimited)
        controls.note_retry("move.1")
    assert controls.retry_count("move.1") == 300
    controls.require_retry_capacity("move.1", unlimited)
    assert controls.note_retry("move.1") == 301
    # Unlimited never enables a forbidden intervention.
    forbidden = {"allow_intervention": False, "max_retries": 0}
    assert controls.retry_permitted("move.1", forbidden) is False
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.require_retry_capacity("move.1", forbidden)
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


def test_one_reset_owner_event_includes_attribution_and_deduplicates_phase_retry() -> (
    None
):
    controls = ProgramRuntimeControls(_capture())
    # The runner passes the parsed 16-byte key; the audit keeps its wire form.
    request = {
        "request_key": _kb("reset.1"),
        "source_label": "operator",
    }
    controls.mark_reset(**request)
    snapshot = controls.snapshot()
    controls.mark_reset(**request)
    assert controls.snapshot() == snapshot
    controls.mark_reset(**{**request, "request_key": _kb("reset.retry.2")})
    assert controls.snapshot() == snapshot
    assert snapshot["audit"][-1] == {
        "event_revision": 1,
        "kind": "program_runtime_controls_reset",
        "action_id": "reset_cycle",
        "request_key": _key("reset.1"),
        "source_label": "operator",
    }


def test_checked_revision_exhaustion_precedes_effect_and_preserves_reset() -> None:
    controls = ProgramRuntimeControls(_capture())
    controls.note_retry("move.1")
    maximum = 9_007_199_254_740_991
    # No public path reaches the revision limit in a test; seed the ledger.
    with controls._lock:  # noqa: SLF001
        controls._revision = maximum - 1  # noqa: SLF001
        controls._audit = [  # noqa: SLF001
            {"event_revision": index, "kind": "prior_event"}
            for index in range(maximum - 257, maximum - 1)
        ]
    snapshot = controls.snapshot()
    with pytest.raises(ProgramRuntimeControlError) as error:
        controls.note_retry("move.1")
    assert error.value.code == "program_runtime_control_revision_exhausted"
    assert controls.snapshot() == snapshot
    controls.mark_reset(request_key=_kb("reset.1"))
    assert controls.revision == maximum


def test_recovery_offer_is_stable_policy_filtered_and_generation_fenced() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    offer = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=_intervention(),
        policy=_policy(),
    )
    assert offer is not None
    assert offer["schema_version"] == "manyforge.cycle_recovery_offer.v6"
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
            application_start_id="3f9a000000000001",
            intervention=_intervention(),
            policy=_policy(),
        )
        == offer
    )

    forbidden = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=_intervention(),
        policy={**_policy(), "allow_intervention": False},
    )
    assert forbidden is not None
    assert forbidden["offer_revision"] == offer["offer_revision"] + 1
    assert _first_enabled(forbidden) is None
    assert all(not action["enabled"] for action in forbidden["actions"])

    acknowledged = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
        intervention=_intervention(acknowledged=True),
        policy=_policy(),
    )
    pending = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
        }
    )
    offer = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=intervention,
        policy=_policy(), hold_revision=3,
    )
    newer = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=intervention,
        policy=_policy(), hold_revision=4,
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
        application_start_id="3f9a000000000001",
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
            # The Stop-time sample is already past the Kernel's freshness
            # budget when the offer is built (the live "born expired" case).
            "observed_state_valid_until_ns": time.monotonic_ns() - 1,
        }
    )
    settled = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
            "stop_event_id": 0,
        }
    )

    assert (
        controls.recovery_offer(
            cycle_generation=6,
            application_start_id="3f9a000000000001",
            intervention=intervention,
            policy={**_policy(), "allow_intervention": False, "max_retries": 0},
        )
        is None
    )


def test_recovery_offer_carries_no_stop_identity() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    offer = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=_intervention(),
        policy=_policy(),
    )

    assert offer is not None
    for removed in (
        "stop_event_id",
        "stop_id",
        "stop_generation",
        "stop_certificate_id",
        "stop_certificate_revision",
    ):
        assert removed not in offer


def test_operator_stop_reset_requires_the_operation_stop_event() -> None:
    """Settled = every participant stopped (backend_stop_confirmed) and the
    operation names its Stop event; no certificate or owner generation."""
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
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
            "stop_event_id": 0,
        }
    )
    policy = {**_policy(), "allow_intervention": False, "max_retries": 0}

    def reset_enabled() -> bool:
        offer = controls.recovery_offer(
            cycle_generation=6,
            application_start_id="3f9a000000000001",
            intervention=intervention,
            policy=policy,
        )
        assert offer is not None
        return next(
            row for row in offer["actions"] if row["action_id"] == "reset_cycle"
        )["enabled"]

    assert reset_enabled() is False
    intervention.owner_assessment["stop_event_id"] = 4
    assert reset_enabled() is True
    intervention.owner_assessment["backend_stop_confirmed"] = False
    assert reset_enabled() is False


def test_recovery_offer_projects_immutable_owner_facts_and_policy() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    intervention = _intervention(policy_parameter="operator_stop_policy")
    intervention.owner_assessment = MappingProxyType(
        {
            **intervention.owner_assessment,
            "allowed_actions": ("retry_motion_from_current_state",),
            "participants": (
                MappingProxyType({"operation_participant_id": {"operation_id": 1, "ordinal": 1}}),
            ),
            "recovery_class": "ordinary_operator_stop",
            "status": "retry_ready",
        }
    )
    policy = MappingProxyType(_policy())

    offer = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=intervention,
        policy=policy,
    )

    assert offer is not None
    assert offer["participants"] == [{"operation_participant_id": {"operation_id": 1, "ordinal": 1}}]
    assert offer["policy"] == {"allow_intervention": True, "max_retries": 2}
    assert _first_enabled(offer) == "retry_motion_from_current_state"


def test_recovery_offer_names_only_a_kernel_operation_number() -> None:
    def offer_for(owner_task_id):
        intervention = _intervention()
        intervention.owner_task_id = owner_task_id
        return ProgramRuntimeControls(
            _capture(), recovery_fences=_fences()
        ).recovery_offer(
            cycle_generation=6,
            application_start_id="3f9a000000000001",
            intervention=intervention,
            policy=_policy(),
        )

    with_operation = offer_for(5)
    # A task with no operation (e.g. a Pick that found no feasible candidate)
    # is keyed by its text task id; the offer names no operation then.
    without_operation = offer_for("0123456789abcdef0123456789abcdef")
    assert with_operation is not None and without_operation is not None
    assert with_operation["operation_id"] == 5
    assert without_operation["operation_id"] is None
    volatile = {"operation_id", "generated_at_ns"}
    assert {
        key: value for key, value in without_operation.items() if key not in volatile
    } == {key: value for key, value in with_operation.items() if key not in volatile}


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
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
        intervention=intervention,
        policy=_policy(),
    )
    intervention.owner_assessment["observed_state_valid_until_ns"] = (
        time.monotonic_ns() - 1
    )

    offer = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
        "observed_state_valid_until_ns": 0,
        "operation_revision": 1,
        "participants": [{"operation_participant_id": {"operation_id": 1, "ordinal": 1}}],
        "recovery_class": "failed_validation",
        "retry_admissible": True,
        "status": "retry_ready",
    }
    intervention.reason_code = "ik_failed"

    offer = controls.recovery_offer(
        cycle_generation=5,
        application_start_id="3f9a000000000001",
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
        application_start_id="3f9a000000000001",
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
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=held, policy=_policy()
    )
    action = _continue_action(not_closable)
    assert action["requires_confirmation"] is True
    assert action["performs_backend_reset"] is False
    assert action["enabled"] is False
    assert action["disabled_reason_code"] == "recovery_close_not_eligible"

    closable = _intervention()
    closable.owner_assessment = {**closable.owner_assessment, "eligible_close": True}
    offer = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=closable, policy=_policy()
    )
    assert _continue_action(offer)["enabled"] is True
    # Continue releases motion: it waits for confirmed containment like Retry.
    pending = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
            cycle_generation=6, application_start_id="3f9a000000000001", intervention=logic, policy=_policy()
        )
    )["enabled"] is True
    stop = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
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
        "recovery_class": "ordinary_program_stop",
        "status": "retry_ready",
    }
    policy = {"allow_intervention": True, "max_retries": 1}
    offer = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=held, policy=policy
    )
    actions = {row["action_id"]: row for row in offer["actions"]}
    assert actions["replan_from_stopped_state"]["enabled"] is True
    assert _continue_action(offer)["enabled"] is True
    assert "reset_cycle" in actions
    assert offer["recovery_class"] == "ordinary_program_stop"
    controls.note_retry("move.1")  # The one allowed Retry is used.
    exhausted = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=held, policy=policy
    )
    retry = next(
        row for row in exhausted["actions"] if row["action_id"] == "replan_from_stopped_state"
    )
    assert retry["enabled"] is False
    assert retry["disabled_reason_code"] == "recovery_retry_limit_reached"
    assert _continue_action(exhausted)["enabled"] is True


def test_clean_collision_stop_failure_hold_offers_retry_and_continue() -> None:
    """OD-373 (A): a collision Stop contained cleanly ends Stopped; its failure
    hold offers the Kernel's replan from the stopped state as Retry (bounded
    by max_retries) and Continue. A collision Stop that was not clean keeps
    neither: Continue waits for a close the Kernel does not grant."""

    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    held = _intervention()
    held.reason_code = "execution_aborted"
    held.owner_assessment = {
        **held.owner_assessment,
        "allowed_actions": ["replan_from_stopped_state", "reset_cycle"],
        "eligible_close": True,
        "execution_state": "Stopped",
        "recovery_class": "runtime_collision",
        "retry_admissible": True,
        "status": "retry_ready",
    }
    policy = {"allow_intervention": True, "max_retries": 1}
    offer = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=held, policy=policy
    )
    actions = {row["action_id"]: row for row in offer["actions"]}
    assert actions["replan_from_stopped_state"]["enabled"] is True
    assert _continue_action(offer)["enabled"] is True
    assert offer["recovery_class"] == "runtime_collision"
    controls.note_retry("move.1")  # The one allowed Retry is used.
    exhausted = controls.recovery_offer(
        cycle_generation=6, application_start_id="3f9a000000000001", intervention=held, policy=policy
    )
    retry = next(
        row for row in exhausted["actions"] if row["action_id"] == "replan_from_stopped_state"
    )
    assert retry["enabled"] is False
    assert retry["disabled_reason_code"] == "recovery_retry_limit_reached"
    assert _continue_action(exhausted)["enabled"] is True

    escalated = _intervention()
    escalated.identity = SimpleNamespace(
        program_run_id=1, node_instance_id="move.2", node_attempt_id=2
    )
    escalated.owner_assessment = {
        **escalated.owner_assessment,
        "allowed_actions": ["reset_cycle"],
        "eligible_close": False,
        "execution_state": "InterventionRequired",
        "recovery_class": "runtime_collision",
        "status": "reset_required",
    }
    not_clean = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=escalated,
        policy=policy,
    )
    assert all(
        row["action_id"] != "replan_from_stopped_state" for row in not_clean["actions"]
    )
    assert _continue_action(not_clean)["enabled"] is False
    assert _continue_action(not_clean)["disabled_reason_code"] == "recovery_close_not_eligible"


def test_each_held_node_keeps_its_own_stable_offer() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())
    first = _intervention()
    second = _intervention()
    second.identity = SimpleNamespace(
        program_run_id=1, node_instance_id="move.2", node_attempt_id=2
    )

    def offer(intervention):
        return controls.recovery_offer(
            cycle_generation=6, application_start_id="3f9a000000000001", intervention=intervention, policy=_policy()
        )

    one, two = offer(first), offer(second)
    assert one["node_instance_id"] == "move.1" and two["node_instance_id"] == "move.2"
    assert one["offer_id"] != two["offer_id"]
    # Publishing the other hold does not churn this hold's offer revision.
    assert offer(first)["offer_revision"] == one["offer_revision"]
    assert offer(second)["offer_revision"] == two["offer_revision"]


def _question(**changes) -> MappingProxyType:
    """An open operator question as the run owner publishes it (OD-342)."""
    question = {
        "node_instance_id": "ask_bin_full",
        "node_attempt_id": 41,
        "message": "Bin is full. Empty it, then choose.",
        "severity": "warning",
        "options": (
            MappingProxyType(
                {"option_id": "continue", "label": "Bin emptied, continue", "result": "success"}
            ),
            MappingProxyType(
                {"option_id": "abort", "label": "Abort this cycle", "result": "failure"}
            ),
        ),
        "hold_motion_group_ids": ("left_arm",),
    }
    question.update(changes)
    return MappingProxyType(question)


def test_operator_question_offer_is_stable_and_shares_the_offer_revision() -> None:
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences())

    def ask(question, *, cycle_generation: int = 6):
        return controls.operator_question_offer(
            cycle_generation=cycle_generation,
            application_start_id="3f9a000000000001",
            question=question,
        )

    offer = ask(_question())
    assert offer == {
        "schema_version": "manyforge.program_operator_question.v1",
        "offer_id": offer["offer_id"],
        "offer_revision": 1,
        "program_run_id": 1,
        "node_instance_id": "ask_bin_full",
        "node_attempt_id": 41,
        "message": "Bin is full. Empty it, then choose.",
        "severity": "warning",
        "options": [
            {"option_id": "continue", "label": "Bin emptied, continue", "result": "success"},
            {"option_id": "abort", "label": "Abort this cycle", "result": "failure"},
        ],
        "hold_motion_group_ids": ["left_arm"],
    }
    assert offer["offer_id"].startswith("sha256:") and len(offer["offer_id"]) == 71
    # An unchanged question keeps its offer id and revision.
    assert ask(_question()) == offer
    # A caller cannot change the retained offer.
    offer["options"].clear()
    assert ask(_question())["options"]

    changed = ask(_question(message="Bin is still full."))
    assert changed["offer_revision"] == 2
    assert changed["offer_id"] != offer["offer_id"]
    # A new attempt is a new question.
    retried = ask(_question(message="Bin is still full.", node_attempt_id=42))
    assert retried["offer_revision"] == 3
    assert retried["node_attempt_id"] == 42

    # Recovery offers advance the same revision counter.
    recovery = controls.recovery_offer(
        cycle_generation=6,
        application_start_id="3f9a000000000001",
        intervention=_intervention(),
        policy=_policy(),
    )
    assert recovery is not None and recovery["offer_revision"] == 4
    other = ask(_question(node_instance_id="ask_tools"))
    assert other["offer_revision"] == 5
    assert other["offer_id"] != offer["offer_id"]
    # Publishing another question does not churn this one.
    assert ask(_question(message="Bin is still full.", node_attempt_id=42)) == retried

    # The cycle generation fences the offer.
    fenced = ask(
        _question(message="Bin is still full.", node_attempt_id=42), cycle_generation=7
    )
    assert fenced["offer_revision"] == 6
    assert fenced["offer_id"] != retried["offer_id"]


def test_each_audit_event_goes_to_history_stamped_and_the_ring_is_unchanged() -> None:
    """OD-381 P3: the one append point hands each audit event, with its
    production time, to History; the ring's entries keep their shape."""

    sent: list[dict[str, object]] = []
    controls = ProgramRuntimeControls(_capture(), recovery_fences=_fences(), history=sent.append)
    before_ns = time.monotonic_ns()
    controls.audit_value(
        "cycle_recovery_action_applied",
        offer_id="offer.1", action_id="retry_runtime_step",
        node_instance_id="move.1", node_attempt_id=2, cycle_resumed=True,
    )
    controls.note_retry("move.1")
    controls.audit_value("program_motion_groups_stopped", node_attempt_id=2, group_ids=["arm"])
    controls.mark_reset(request_key=_kb("reset"), source_label="composer_ui")
    after_ns = time.monotonic_ns()

    audit = controls.snapshot()["audit"]
    assert [entry["kind"] for entry in audit] == [
        "cycle_recovery_action_applied", "program_node_retry",
        "program_motion_groups_stopped", "program_runtime_controls_reset",
    ]
    assert audit[0] == {
        "event_revision": 1, "kind": "cycle_recovery_action_applied", "offer_id": "offer.1",
        "action_id": "retry_runtime_step", "node_instance_id": "move.1",
        "node_attempt_id": 2, "cycle_resumed": True,
    }
    assert [(event["program_run_id"], event["event"], event["outcome"], event["error"])
            for event in sent] == [
        (1, entry["kind"], "recorded", None) for entry in audit]
    assert [event["facts"] for event in sent] == [
        {name: value for name, value in entry.items() if name != "kind"} for entry in audit]
    for event in sent:
        assert before_ns <= int(event["occurred_at_ns"]) <= after_ns
        assert event["occurred_at_wall"].endswith("+00:00")
    # The History copy never aliases the ring entry itself.
    sent[0]["facts"]["offer_id"] = "changed"
    assert controls.snapshot()["audit"][0]["offer_id"] == "offer.1"
