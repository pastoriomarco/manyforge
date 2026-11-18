from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

import manyforge_core_py as mf


def _context() -> mf.PublicRequestContextDTO:
    return mf.PublicRequestContextDTO(
        request_key=mf.RequestKey(bytes(range(16))),
        source_label="python-test",
    )


def test_stop_selector_factories_are_exact_and_read_only() -> None:
    operation_target = mf.StopOperationTargetDTO(
        operation_id=mf.OperationNumber(1),
        expected_operation_revision=mf.OperationRevision(3),
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

    full = mf.FullStopIntentDTO(request_context=_context())
    assert full.field_error() is None
    assert not hasattr(full, "expected_stop_generation")
    assert not hasattr(mf.StopIntentDTO, "for_program_runs")
    assert not hasattr(mf, "StopProgramRunTargetDTO")
    assert (
        mf.StopResolutionDispositionDTO.kStaleButUnambiguouslyResolved.name
        == "kStaleButUnambiguouslyResolved"
    )
    assert mf.StopResolutionDispositionDTO.kInvalid.name == "kInvalid"
    for removed in (
        "kRequestIdentityConflict",
        "kOverlappingStopTransactions",
        "kEmergencyReserveCoalesced",
        "kExplicitFullStop",
    ):
        assert not hasattr(mf.StopScopeResolutionReasonDTO, removed)

    run_end = mf.ProgramRunEndIntentDTO(
        request_context=_context(), program_run_id=mf.ProgramRunNumber(3)
    )
    assert run_end.program_run_id == mf.ProgramRunNumber(3)


def _accepted(**overrides: object) -> mf.StopAcceptedDTO:
    fields: dict[str, object] = dict(
        request_key=mf.RequestKey(bytes(range(16))),
        replay_status=mf.RequestReplayStatusDTO.kAcceptedNew,
        event_id=mf.StopEventId(1),
        requested=mf.StopKind.kOperation,
        applied=mf.StopKind.kOperation,
        widen_reason=mf.StopWidenReason.kNone,
        program_run_id=None,
        stale_diagnostics=[],
        stale_total=0,
        covered=[],
        hold_revision=0,
        accepted_at_ns=8_000_000_000,
    )
    fields.update(overrides)
    return mf.StopAcceptedDTO(**fields)


def test_stop_acceptance_carries_no_shared_stop_budget() -> None:
    # Each robot's Stop deadlines come from its own profile at Stop time;
    # the acceptance carries only its time.
    accepted = _accepted()
    assert accepted.field_error() is None
    assert accepted.accepted_at_ns == 8_000_000_000
    for removed in (
        "fixed_outer_duration_ns",
        "fixed_kernel_completion_reserve_ns",
        "remaining_outer_duration_ns",
        "participant_budget_allocations",
        "requested_selector_digest",
        "captured_scope_digest",
    ):
        assert not hasattr(accepted, removed)
    assert not hasattr(mf, "StopAcceptedPhaseBudgetDTO")
    assert not hasattr(mf, "StopAcceptedParticipantBudgetDTO")

    with pytest.raises(AttributeError):
        accepted.accepted_at_ns = 900


def test_stop_accepted_and_full_stop_round_trip_their_fields() -> None:
    attribution = mf.RequestAttributionDTO()
    attribution.principal_id = "operator-1"
    # An unverified attribution names no principal.
    unverified = mf.FullStopIntentDTO(request_context=_context(), attribution=attribution)
    assert unverified.field_error() == "full_stop_request_attribution_invalid"
    attribution.assurance = mf.RequestAssuranceDTO.kExternallyAuthenticated
    full = mf.FullStopIntentDTO(request_context=_context(), attribution=attribution)
    assert full.field_error() is None
    assert full.request_context == _context()
    assert full.attribution.principal_id == "operator-1"
    assert full.attribution.assurance == mf.RequestAssuranceDTO.kExternallyAuthenticated
    assert full.attribution.transport == attribution.transport
    assert full == mf.FullStopIntentDTO(_context(), attribution)
    assert full != mf.FullStopIntentDTO(_context())

    stale = mf.StopStaleDiagnosticDTO(
        operation_id=mf.OperationNumber(2),
        expected_operation_revision=mf.OperationRevision(3),
        current_operation_revision=mf.OperationRevision(4),
    )
    covered = mf.StopCoveredParticipantDTO(
        operation_id=mf.OperationNumber(2), participant_ordinal=1, started=True
    )
    accepted = _accepted(
        event_id=mf.StopEventId(9),
        requested=mf.StopKind.kGroups,
        applied=mf.StopKind.kStopAll,
        widen_reason=mf.StopWidenReason.kUnknownTarget,
        program_run_id=mf.ProgramRunNumber(5),
        stale_diagnostics=[stale],
        stale_total=1,
        covered=[covered],
        hold_revision=7,
    )
    assert accepted.field_error() is None
    assert accepted.request_key == mf.RequestKey(bytes(range(16)))
    assert accepted.replay_status == mf.RequestReplayStatusDTO.kAcceptedNew
    assert accepted.event_id == mf.StopEventId(9)
    assert accepted.requested == mf.StopKind.kGroups
    assert accepted.applied == mf.StopKind.kStopAll
    assert accepted.widen_reason == mf.StopWidenReason.kUnknownTarget
    assert accepted.program_run_id == mf.ProgramRunNumber(5)
    assert accepted.stale_diagnostics == [stale]
    assert accepted.stale_diagnostics[0].current_operation_revision == mf.OperationRevision(4)
    assert accepted.stale_total == 1
    assert accepted.covered == [covered]
    assert accepted.covered[0].started
    assert accepted.hold_revision == 7
    assert accepted.accepted_at_ns == 8_000_000_000
    assert accepted == _accepted(
        event_id=mf.StopEventId(9),
        requested=mf.StopKind.kGroups,
        applied=mf.StopKind.kStopAll,
        widen_reason=mf.StopWidenReason.kUnknownTarget,
        program_run_id=mf.ProgramRunNumber(5),
        stale_diagnostics=[stale],
        stale_total=1,
        covered=[covered],
        hold_revision=7,
    )
    assert accepted != _accepted()


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


def test_request_key_is_sixteen_bytes_and_lowercase_hex_on_the_wire() -> None:
    key = mf.RequestKey(bytes.fromhex("41" + "00" * 6 + "03" + "00" * 7 + "07"))
    assert key.value == bytes.fromhex("41" + "00" * 6 + "03" + "00" * 7 + "07")
    assert key.hex() == "41000000000000030000000000000007"
    assert mf.RequestKey.from_hex(key.hex().upper()) == key
    assert hash(mf.RequestKey.from_hex(key.hex())) == hash(key)
    with pytest.raises(ValueError):
        mf.RequestKey(b"\x00" * 15)
    with pytest.raises(ValueError):
        mf.RequestKey.from_hex("request.stop.1")


def test_operation_participant_id_is_operation_number_and_ordinal() -> None:
    first = mf.OperationParticipantId(operation_id=mf.OperationNumber(7), ordinal=1)
    assert first.operation_id == mf.OperationNumber(7)
    assert first.ordinal == 1
    assert first == mf.OperationParticipantId(mf.OperationNumber(7), 1)
    assert first != mf.OperationParticipantId(mf.OperationNumber(7), 2)
    assert first != mf.OperationParticipantId(mf.OperationNumber(8), 1)
    assert hash(first) == hash(mf.OperationParticipantId(mf.OperationNumber(7), 1))
    assert repr(first) == "OperationParticipantId(7, 1)"
