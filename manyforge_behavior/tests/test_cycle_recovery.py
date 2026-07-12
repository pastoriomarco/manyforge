from __future__ import annotations

from dataclasses import replace
import json

from manyforge_behavior.cycle_recovery import (
    CycleRecoveryActionId,
    CycleRecoveryCoordinator,
    CycleRecoveryFacts,
    RunRecoveryDisposition,
)
from manyforge_behavior.runtime_pause import RuntimePauseRegistry


def _facts(**overrides):
    values = dict(
        cycle_state="stopped",
        motion_stop_state="idle",
        has_resumable_context=True,
        cycle_error_code=None,
        active_runtime_fault_ids=(),
        active_runtime_fault_domains=(),
        intervention_active=False,
        direct_control_active=False,
        lifecycle_command_pending=False,
        cycle_generation=4,
        program_generation=7,
        deployment_generation=3,
        scene_admission_epoch=11,
        pose_source_apply_generation=9,
        live_collision_world_revision=101,
    )
    values.update(overrides)
    return CycleRecoveryFacts(**values)


def _ready_assessment(**overrides):
    values = dict(
        assessment_revision=5,
        control_command_epoch=8,
        execution_id=42,
        execution_state="interrupted",
        recovery_class="runtime_collision",
        status="retry_ready",
        latch_id="latch-42",
        stop_certificate_id="cert-42",
        stop_owner_generation=6,
        # Absolute monotonic deadline. Keep the common fixture fresh without
        # making otherwise-identical projections differ by construction.
        observed_state_valid_until_ns=1 << 62,
        allowed_actions=["retry_motion_from_current_state", "reset_cycle"],
        reason_code="recovery_stop_certified",
        evidence_refs=["cert-42", "kernel-event:77"],
    )
    values.update(overrides)
    return values


def test_offer_is_immutable_and_live_world_revision_is_diagnostic_only():
    published = []
    coordinator = CycleRecoveryCoordinator(on_offer_published=published.append)
    first = coordinator.project(_ready_assessment(), _facts())
    assert first is not None

    second = coordinator.project(
        _ready_assessment(),
        replace(_facts(), live_collision_world_revision=999),
    )

    assert second is first
    assert second.offer_id == first.offer_id
    assert second.offer_revision == first.offer_revision
    assert second.live_collision_world_revision == 101
    assert len(published) == 1


def test_structural_scene_epoch_publishes_new_offer_and_stales_old_request():
    coordinator = CycleRecoveryCoordinator()
    first = coordinator.project(_ready_assessment(), _facts())
    assert first is not None
    second = coordinator.project(
        _ready_assessment(),
        replace(_facts(), scene_admission_epoch=12),
    )
    assert second is not None
    assert second.offer_revision == first.offer_revision + 1
    validation = coordinator.validate_request(
        {
            "request_id": "request-1",
            "offer_id": first.offer_id,
            "offer_revision": first.offer_revision,
            "action_id": "retry_motion_from_current_state",
        },
        second,
    )
    assert not validation.accepted
    assert validation.error["details"]["reason"] == "recovery_offer_stale"


def test_plain_paused_cycle_has_no_recovery_offer():
    coordinator = CycleRecoveryCoordinator()
    offer = coordinator.project(
        _ready_assessment(
            execution_state="paused",
            recovery_class="none",
            status="unavailable",
            allowed_actions=[],
        ),
        _facts(),
    )
    assert offer is None


def test_run_directive_consumes_only_retry_ready_nonconfirming_primary():
    coordinator = CycleRecoveryCoordinator()
    coordinator.project(_ready_assessment(), _facts())

    directive = coordinator.resolve_run_request()

    assert directive.disposition == RunRecoveryDisposition.APPLY_PRIMARY
    assert directive.action_id == CycleRecoveryActionId.RETRY_MOTION_FROM_CURRENT_STATE
    assert directive.reason_code == "operator_run_primary_recovery"


def test_run_directive_preserves_explicit_operator_choice():
    coordinator = CycleRecoveryCoordinator()
    coordinator.project(
        _ready_assessment(
            status="action_required",
            allowed_actions=["acknowledge_and_continue", "reset_cycle"],
            reason_code="operator_acknowledgement_required",
        ),
        _facts(),
    )

    directive = coordinator.resolve_run_request()

    assert directive.disposition == RunRecoveryDisposition.EXPLICIT_ACTION_REQUIRED
    assert directive.action_id is None
    assert directive.reason_code == "operator_acknowledgement_required"


def test_run_directive_bypasses_when_no_offer_exists():
    directive = CycleRecoveryCoordinator().resolve_run_request()
    assert directive.disposition == RunRecoveryDisposition.BYPASS
    assert directive.offer is None


def test_idle_kernel_runtime_fault_gets_cycle_only_exact_retry_action():
    coordinator = CycleRecoveryCoordinator()
    offer = coordinator.project(
        _ready_assessment(
            execution_state="idle",
            recovery_class="none",
            status="unavailable",
            allowed_actions=[],
            reason_code="no_recovery_active",
            evidence_refs=[],
        ),
        _facts(
            cycle_state="running",
            motion_stop_state="held",
            active_runtime_fault_ids=("fault-1",),
            active_runtime_fault_domains=("gripper",),
        ),
    )
    assert offer is not None
    assert offer.recovery_class == "runtime_domain_fault"
    assert offer.primary_action == "retry_runtime_step"
    assert [action.action_id for action in offer.actions] == [
        "retry_runtime_step",
        "reset_cycle",
    ]
    assert "runtime_fault:fault-1" in offer.evidence_refs


def test_runtime_pause_registry_resume_is_exact_and_generation_scoped():
    registry = RuntimePauseRegistry()
    first = registry.raise_fault(
        node_name="grip",
        node_id="command_gripper",
        domain="gripper",
        reason_code="gripper_command_failed",
        message="failed",
    )
    second = registry.raise_fault(
        node_name="model",
        node_id="query_model",
        domain="model",
        reason_code="model_call_failed",
        message="failed",
    )

    assert registry.request_resume(
        (first.fault_id,), recovery_request_id="recovery-1"
    )
    assert registry.consume_resume(
        first.fault_id, expected_generation=first.generation + 1
    ) is None
    assert registry.consume_resume(
        first.fault_id, expected_generation=first.generation
    ) == "recovery-1"
    assert registry.get(second.fault_id).state == "active"


def test_runtime_pause_registry_resume_batch_is_transactional():
    registry = RuntimePauseRegistry()
    first = registry.raise_fault(
        node_name="move",
        node_id="move_manipulator_action",
        domain="motion",
        reason_code="execution_failed",
        message="failed",
    )
    second = registry.raise_fault(
        node_name="grip",
        node_id="command_gripper",
        domain="gripper",
        reason_code="gripper_command_failed",
        message="failed",
    )

    assert not registry.request_resume(
        (first.fault_id, "missing-fault"),
        recovery_request_id="recovery-invalid",
    )
    assert registry.get(first.fault_id).state == "active"
    assert registry.get(second.fault_id).state == "active"

    assert registry.request_resume(
        (first.fault_id, second.fault_id),
        recovery_request_id="recovery-valid",
    )
    assert registry.get(first.fault_id).state == "resume_requested"
    assert registry.get(second.fault_id).state == "resume_requested"


def test_runtime_pause_registry_returns_defensive_detail_copies_and_clears_exactly():
    registry = RuntimePauseRegistry()
    source_details = {"nested": {"value": 1}}
    fault = registry.raise_fault(
        node_name="model",
        node_id="query_model",
        domain="model",
        reason_code="model_call_failed",
        message="failed",
        details=source_details,
    )
    source_details["nested"]["value"] = 2
    read = registry.get(fault.fault_id)
    assert read.details == {"nested": {"value": 1}}
    read.details["nested"]["value"] = 3
    assert registry.get(fault.fault_id).details == {"nested": {"value": 1}}

    assert not registry.clear(
        fault.fault_id,
        expected_generation=fault.generation + 1,
    )
    assert registry.get(fault.fault_id) is not None
    assert registry.clear(
        fault.fault_id,
        expected_generation=fault.generation,
    )
    assert registry.get(fault.fault_id) is None


def test_runtime_pause_registry_bounds_and_json_normalizes_nested_diagnostics():
    registry = RuntimePauseRegistry()
    cyclic: dict[str, object] = {}
    cyclic["self"] = cyclic

    fault = registry.raise_fault(
        node_name="model",
        node_id="query_model",
        domain="model",
        reason_code="model_call_failed",
        message="failed" * 1000,
        details={
            "large": "x" * 20_000,
            "deep": {"a": {"b": {"c": {"d": {"e": {"f": 1}}}}}},
            "cycle": cyclic,
            "non_finite": float("nan"),
            "opaque": object(),
            **{f"key-{index}": index for index in range(80)},
        },
    )

    details = registry.get(fault.fault_id).details
    assert len(details) <= 64
    assert len(fault.message) == 2048
    assert details["_manyforge_details_truncated"] is True
    assert len(details["large"]) == 1024
    assert "<max_depth>" in json.dumps(details["deep"])
    assert details["cycle"]["self"] == "<cycle>"
    assert details["non_finite"] == "<non_finite:nan>"
    assert details["opaque"].startswith("<unsupported:")
    assert json.loads(json.dumps(details, allow_nan=False)) == details
    assert len(json.dumps(details)) < 20_000


def test_runtime_pause_registry_clear_all_invalidates_multiple_faults():
    registry = RuntimePauseRegistry()
    for index in range(3):
        registry.raise_fault(
            node_name=f"node-{index}",
            node_id="runtime_node",
            domain="runtime",
            reason_code="runtime_failure",
            message="failed",
        )
    before_revision = registry.revision

    registry.clear_all()

    assert registry.active_faults() == ()
    assert registry.revision == before_revision + 1
