from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

import manyforge_core_py as mf


def _context() -> mf.PublicRequestContextDTO:
    return mf.PublicRequestContextDTO(
        request_id=mf.RequestId("request.stop.1"),
        request_owner_epoch=mf.RequestOwnerEpoch("kernel.epoch.1"),
        source_label="python-test",
        correlation_id="correlation.1",
    )


def test_stop_selector_factories_are_exact_and_read_only() -> None:
    operation_target = mf.StopOperationTargetDTO(
        operation_id=mf.OperationId("operation.1"),
        expected_operation_revision=mf.OperationRevision(3),
        expected_stop_generation=mf.StopGeneration(0),
    )
    operation = mf.StopIntentDTO.for_operations(
        request_context=_context(), operations=[operation_target], reason="operator Stop"
    )
    assert operation.field_error() is None
    assert operation.selector_kind == "operations"
    assert operation.operations == [operation_target]
    assert operation.components is None
    assert operation.resources is None

    component_target = mf.StopComponentTargetDTO(
        controllable_component_id=mf.ControllableComponentId("robot.a"),
        expected_graph_revision=mf.GraphRevision("sha256:graph-a"),
        expected_component_lifecycle_generation=mf.ComponentLifecycleGeneration(4),
    )
    component = mf.StopIntentDTO.for_components(
        request_context=_context(), components=[component_target]
    )
    assert component.field_error() is None
    assert component.selector_kind == "components"
    assert component.components == [component_target]

    resource_target = mf.StopResourceTargetDTO(
        resource_id=mf.ResourceId("robot.a.joint.1"),
        expected_graph_revision=mf.GraphRevision("sha256:graph-a"),
        expected_operation_resource_generation=12,
        expected_stop_generation=mf.StopGeneration(0),
    )
    resource = mf.StopIntentDTO.for_resources(
        request_context=_context(), resources=[resource_target]
    )
    assert resource.field_error() is None
    assert resource.selector_kind == "resources"
    assert resource.resources == [resource_target]

    with pytest.raises(AttributeError):
        resource.reason = "changed"


def test_stop_validation_and_distinct_full_stop_are_public() -> None:
    empty = mf.StopIntentDTO.for_operations(request_context=_context(), operations=[])
    assert empty.field_error() == "stop_operation_targets_not_canonical"

    invalid_component = mf.StopIntentDTO.for_components(
        request_context=_context(),
        components=[
            mf.StopComponentTargetDTO(
                controllable_component_id=mf.ControllableComponentId("robot.a"),
                expected_graph_revision=mf.GraphRevision("sha256:graph-a"),
                expected_component_lifecycle_generation=mf.ComponentLifecycleGeneration(0),
            )
        ],
    )
    assert invalid_component.field_error() == "stop_component_target_invalid"

    full = mf.FullStopIntentDTO(
        request_context=_context(), expected_stop_generation=mf.StopGeneration(0)
    )
    assert full.field_error() is None
    assert full.expected_stop_generation.value == 0
    assert (
        mf.StopResolutionDispositionDTO.kStaleButUnambiguouslyResolved.name
        == "kStaleButUnambiguouslyResolved"
    )
    assert mf.StopScopeResolutionReasonDTO.kRequestIdentityConflict.name == (
        "kRequestIdentityConflict"
    )
    assert mf.StopScopeResolutionReasonDTO.kOverlappingStopTransactions.name == (
        "kOverlappingStopTransactions"
    )
    assert mf.StopScopeResolutionReasonDTO.kEmergencyReserveCoalesced.name == (
        "kEmergencyReserveCoalesced"
    )


def test_stop_acceptance_carries_no_shared_stop_budget() -> None:
    # Each robot's Stop deadlines come from its own profile at Stop time;
    # the acceptance carries only its time.
    affected = mf.StopAffectedOperationRevisionDTO(
        operation_id=mf.OperationId("operation.1"),
        operation_revision_after_acceptance=mf.OperationRevision(4),
    )
    accepted = mf.StopAcceptedDTO(
        request_id=mf.RequestId("request.stop.1"),
        replay_status=mf.RequestReplayStatusDTO.kAcceptedNew,
        stop_id=mf.StopId("stop.1"),
        stop_generation=mf.StopGeneration(8),
        requested_selector_digest=mf.ContentDigest("sha256:" + "1" * 64),
        resolution_disposition=mf.StopResolutionDispositionDTO.kExact,
        resolution_reason=mf.StopScopeResolutionReasonDTO.kExactCurrentScope,
        stale_diagnostics=[],
        captured_scope_digest=mf.ContentDigest("sha256:" + "2" * 64),
        joined_predecessor_stops=[],
        affected_operation_revisions=[affected],
        accepted_at_ns=8_000_000_000,
    )
    assert accepted.field_error() is None
    assert accepted.accepted_at_ns == 8_000_000_000
    assert accepted.affected_operation_revisions == [affected]
    assert accepted.joined_predecessor_stops == []
    for removed in (
        "fixed_outer_duration_ns",
        "fixed_kernel_completion_reserve_ns",
        "remaining_outer_duration_ns",
        "participant_budget_allocations",
    ):
        assert not hasattr(accepted, removed)
    assert not hasattr(mf, "StopAcceptedPhaseBudgetDTO")
    assert not hasattr(mf, "StopAcceptedParticipantBudgetDTO")

    with pytest.raises(AttributeError):
        accepted.accepted_at_ns = 900


def test_resolved_stop_base_plan_golden_is_shared_exactly_with_cpp() -> None:
    golden_path = (
        Path(__file__).resolve().parents[2]
        / "test"
        / "golden"
        / "resolved_stop_base_plan.v1.json"
    )
    carrier = golden_path.read_bytes()
    assert carrier.endswith(b"\n")
    canonical_bytes = carrier[:-1]

    plan_id = mf.verify_resolved_stop_base_plan_canonical_bytes(canonical_bytes)
    assert plan_id == mf.ResolvedStopBasePlanId(
        # Same golden id as test_resolved_stop_base_plan_codec.cpp.
        "sha256:7fce11a5a4d9b14157cfd341eeefd7c1f34a4ca88cdef384a2b517600a050662"
    )
    assert hashlib.sha256(canonical_bytes).hexdigest() == (
        "9d76981b81e07bf90cf983988df44250948fd45bf553057e7daa0b48674620fa"
    )

    with pytest.raises(ValueError):
        mf.verify_resolved_stop_base_plan_canonical_bytes(b" " + canonical_bytes)
