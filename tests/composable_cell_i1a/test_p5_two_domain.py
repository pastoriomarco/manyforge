from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from threading import Barrier

import pytest

from verification.composable_cell_i1a.contracts import (
    AttachmentRevision,
    BindingRevision,
    CapabilityId,
    CommandTargetId,
    ControllableComponentId,
    ControlBindingId,
    ControlDomainId,
    ControllerTopologyId,
    ControllerTopologyRevision,
    GoalId,
    GraphRevision,
    InterfaceContractRevision,
    ModelRevision,
    OperationId,
    OperationParticipant,
    OperationRequest,
    RequestId,
    RequestMetadata,
    ResourceId,
    RouteId,
    RouteGeneration,
    SceneRevision,
    StopRequest,
    StopScope,
    StopTransactionId,
    ValidationRevision,
)
from verification.composable_cell_i1a.fakes import FakeClock
from verification.composable_cell_i1a.two_domain_prototype import (
    CallbackDecision,
    ControllerRoute,
    FakeControlDomain,
    RetainedControlBinding,
    RouteKind,
    StopReplayConflict,
    TwoDomainCoordinatorPrototype,
)


DOMAIN_A = ControlDomainId("domain-a")
DOMAIN_B = ControlDomainId("domain-b")
GRAPH_REVISION = GraphRevision(7)
VALIDATION_REVISION = ValidationRevision(4)


def metadata(label: str, clock: FakeClock) -> RequestMetadata:
    return RequestMetadata(
        request_id=RequestId(f"request-{label}"),
        source_label="p5-test",
        transport="in_process",
        submitted_monotonic_ns=clock.now_ns(),
    )


def participant(
    domain_id: ControlDomainId,
    binding: str,
    *resources: str,
    route_id: str = "direct",
    route_generation: int = 1,
) -> OperationParticipant:
    return OperationParticipant(
        controllable_component_id=ControllableComponentId(
            f"{domain_id}.component"
        ),
        command_target_id=CommandTargetId(f"{binding}.target"),
        capability_id=CapabilityId(f"{binding}.capability"),
        control_binding_id=ControlBindingId(binding),
        binding_revision=BindingRevision(1),
        control_domain_id=domain_id,
        model_revision=ModelRevision("model-1"),
        scene_revision=SceneRevision("scene-1"),
        attachment_revision=AttachmentRevision("attachment-1"),
        interface_contract_revision=InterfaceContractRevision("interface-1"),
        controller_topology_id=ControllerTopologyId(f"{domain_id}.topology"),
        topology_revision=ControllerTopologyRevision(1),
        route_id=RouteId(route_id),
        route_generation=RouteGeneration(route_generation),
        validation_revision=VALIDATION_REVISION,
        resource_ids=tuple(ResourceId(item) for item in resources),
    )


def operation(
    clock: FakeClock,
    operation_id: str,
    *participants: OperationParticipant,
    coordinated: bool = False,
) -> OperationRequest:
    return OperationRequest(
        metadata=metadata(operation_id, clock),
        operation_id=OperationId(operation_id),
        expected_graph_revision=GRAPH_REVISION,
        participants=tuple(participants),
        coordinated=coordinated,
    )


def stop_request(
    clock: FakeClock,
    stop_id: str,
    scope: StopScope,
) -> StopRequest:
    return StopRequest(
        metadata=metadata(stop_id, clock),
        stop_id=StopTransactionId(stop_id),
        scope=scope,
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )


def make_domain(
    domain_id: ControlDomainId,
    prefix: str,
) -> FakeControlDomain:
    return FakeControlDomain(
        domain_id=domain_id,
        owned_resources=(
            ResourceId(f"{prefix}.arm"),
            ResourceId(f"{prefix}.wrist"),
            ResourceId(f"{prefix}.tool"),
        ),
        routes=(
            ControllerRoute(
                route_id="direct",
                kind=RouteKind.DIRECT,
                controllers=(f"{prefix}.direct_jtc",),
            ),
            ControllerRoute(
                route_id="chained",
                kind=RouteKind.CHAINED,
                controllers=(
                    f"{prefix}.trajectory_filter",
                    f"{prefix}.forward_controller",
                ),
            ),
        ),
        initial_route_id="direct",
    )


def rig() -> tuple[
    FakeClock,
    FakeControlDomain,
    FakeControlDomain,
    TwoDomainCoordinatorPrototype,
]:
    clock = FakeClock(start_ns=100)
    domain_a = make_domain(DOMAIN_A, "a")
    domain_b = make_domain(DOMAIN_B, "b")
    coordinator = TwoDomainCoordinatorPrototype(
        clock=clock,
        graph_revision=GRAPH_REVISION,
        domains=(domain_a, domain_b),
        binding_registry=tuple(
            RetainedControlBinding(binding)
            for binding in (
                participant(DOMAIN_A, "binding-a", "a.arm"),
                participant(
                    DOMAIN_A,
                    "binding-a-2",
                    "a.arm",
                    "a.wrist",
                ),
                participant(DOMAIN_A, "binding-a-new", "a.arm"),
                participant(DOMAIN_A, "binding-a-tool", "a.tool"),
                participant(DOMAIN_B, "binding-b", "b.arm"),
                participant(DOMAIN_B, "binding-b-tool", "b.tool"),
            )
        ),
        stop_budget_ns=1_000,
        measured_sample_freshness_ns=10,
    )
    return clock, domain_a, domain_b, coordinator


def test_disjoint_independent_operations_run_while_overlap_is_rejected() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    first = operation(
        clock,
        "op-a",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    second = operation(
        clock,
        "op-b",
        participant(DOMAIN_B, "binding-b", "b.arm"),
    )

    assert coordinator.start(first).phase == "executing"
    assert coordinator.start(second).phase == "executing"
    assert domain_a.dispatch_log == [OperationId("op-a")]
    assert domain_b.dispatch_log == [OperationId("op-b")]

    overlap = operation(
        clock,
        "op-overlap",
        participant(DOMAIN_A, "binding-a-2", "a.arm", "a.wrist"),
    )
    overlap_status = coordinator.start(overlap)
    assert overlap_status.phase == "rejected"
    assert overlap_status.terminal_detail == "resource_overlap:a.arm"
    assert OperationId("op-overlap") not in domain_a.prepare_attempts


def test_prepare_all_barrier_aborts_before_any_dispatch() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "coordinated-prepare-failure",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    domain_b.fail_prepare_for(request.operation_id)

    status = coordinator.start(request)

    assert status.phase == "failed_pre_dispatch"
    assert status.dispatched_bindings == ()
    assert domain_a.dispatch_attempts == []
    assert domain_b.dispatch_attempts == []
    assert domain_a.dispatch_log == []
    assert domain_b.dispatch_log == []
    assert coordinator.active_resource_locks == {}
    assert any(call.action == "abort_prepare" for call in domain_a.calls)


def test_first_dispatch_failure_still_has_zero_physical_dispatch() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "first-dispatch-failure",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    domain_a.fail_dispatch_for(request.operation_id)

    status = coordinator.start(request)

    assert status.phase == "failed_pre_dispatch"
    assert status.dispatched_bindings == ()
    assert domain_a.dispatch_attempts == [request.operation_id]
    assert domain_b.dispatch_attempts == []
    assert domain_a.dispatch_log == []
    assert domain_b.dispatch_log == []
    assert coordinator.active_resource_locks == {}


def test_coordinated_success_prepares_every_domain_before_dispatch() -> None:
    clock, _, _, coordinator = rig()
    request = operation(
        clock,
        "coordinated-success",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )

    assert coordinator.start(request).phase == "executing"
    prepare_sequences = [
        event.sequence for event in coordinator.trace if event.action == "prepare_ok"
    ]
    dispatch_sequences = [
        event.sequence
        for event in coordinator.trace
        if event.action == "dispatch_attempt"
    ]
    assert max(prepare_sequences) < min(dispatch_sequences)

    goal_a, generation_a = coordinator.participant_goal(
        request.operation_id, DOMAIN_A
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_a,
            route_generation=generation_a,
            outcome="completed",
        )
        is CallbackDecision.ACCEPTED
    )
    assert set(coordinator.active_resource_locks) == {
        ResourceId("a.arm"),
        ResourceId("b.arm"),
    }

    goal_b, generation_b = coordinator.participant_goal(
        request.operation_id, DOMAIN_B
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_B,
            goal_id=goal_b,
            route_generation=generation_b,
            outcome="completed",
        )
        is CallbackDecision.ACCEPTED
    )
    assert coordinator.operation_status(request.operation_id).phase == "completed"
    assert coordinator.active_resource_locks == {}


def test_post_first_dispatch_failure_stops_only_dispatched_subset() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "coordinated-partial",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    domain_b.fail_dispatch_for(request.operation_id)

    status = coordinator.start(request)

    assert status.phase == "partial_dispatch"
    assert status.dispatched_bindings == (ControlBindingId("binding-a"),)
    assert domain_a.dispatch_log == [request.operation_id]
    assert domain_b.dispatch_log == []
    assert len(domain_a.stop_log) == 1
    assert domain_a.stop_log[0].operation_ids == (request.operation_id,)
    assert domain_b.stop_log == []
    assert coordinator.active_resource_locks == {}

    evidence = coordinator.partial_dispatch_evidence
    assert len(evidence) == 1
    assert evidence[0].dispatched_bindings == (ControlBindingId("binding-a"),)
    assert evidence[0].stopped_domains == (DOMAIN_A,)
    assert evidence[0].stop_outcome == "stopped_after_partial_dispatch"
    assert not evidence[0].physical_rollback_claimed
    assert "no physical rollback claimed" in status.terminal_detail


def test_partial_dispatch_stop_failure_remains_retryable_until_rest() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "coordinated-partial-retry",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    partial_stop_id = StopTransactionId(
        "partial-dispatch-stop:coordinated-partial-retry"
    )
    domain_a.fail_stop_for(partial_stop_id)
    domain_b.fail_dispatch_for(request.operation_id)

    status = coordinator.start(request)

    assert status.phase == "partial_dispatch_stop_failed"
    assert coordinator.partial_dispatch_evidence[-1].stop_outcome == "stop_failed"
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})
    assert coordinator.active_resource_locks == {
        ResourceId("a.arm"): request.operation_id
    }

    retry = coordinator.stop(
        stop_request(
            clock,
            "partial-dispatch-retry",
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert retry.outcome == "stopped"
    assert retry.stopped_domains == (DOMAIN_A,)
    assert coordinator.operation_status(request.operation_id).phase == (
        "partial_dispatch"
    )
    assert coordinator.fenced_resources == frozenset()
    assert coordinator.active_resource_locks == {}
    assert len(domain_a.stop_log) == 1
    assert domain_b.stop_log == []


def test_failed_stop_stays_selectable_for_a_fresh_stop_transaction() -> None:
    clock, domain_a, _, coordinator = rig()
    request = operation(
        clock,
        "retryable-stop-op",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    coordinator.start(request)
    failed_stop_id = StopTransactionId("failed-stop")
    domain_a.fail_stop_for(failed_stop_id)

    failed = coordinator.stop(
        stop_request(
            clock,
            str(failed_stop_id),
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert failed.outcome == "stop_failed"
    assert coordinator.operation_status(request.operation_id).phase == "stop_failed"
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})

    retried = coordinator.stop(
        stop_request(
            clock,
            "fresh-stop",
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert retried.outcome == "stopped"
    assert retried.stopped_domains == (DOMAIN_A,)
    assert coordinator.operation_status(request.operation_id).phase == "stopped"
    assert coordinator.fenced_resources == frozenset()
    assert coordinator.active_resource_locks == {}


def test_resource_scoped_stop_leaves_peer_running_then_full_stop_settles_it() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request_a = operation(
        clock,
        "op-a",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    request_b = operation(
        clock,
        "op-b",
        participant(DOMAIN_B, "binding-b", "b.arm"),
    )
    coordinator.start(request_a)
    coordinator.start(request_b)
    goal_a, generation_a = coordinator.participant_goal(
        request_a.operation_id, DOMAIN_A
    )
    scoped = stop_request(
        clock,
        "stop-a",
        StopScope(resource_ids=(ResourceId("a.arm"),)),
    )

    certificate_a = coordinator.stop(scoped)

    assert certificate_a.outcome == "stopped"
    assert certificate_a.stopped_domains == (DOMAIN_A,)
    assert coordinator.operation_status(request_a.operation_id).phase == "stopped"
    assert coordinator.operation_status(request_b.operation_id).phase == "executing"
    assert len(domain_a.stop_log) == 1
    assert domain_b.stop_log == []
    assert (
        coordinator.controller_callback(
            operation_id=request_a.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_a,
            route_generation=generation_a,
            outcome="completed",
        )
        is CallbackDecision.STALE_OPERATION
    )

    assert coordinator.stop(scoped) is certificate_a
    assert len(domain_a.stop_log) == 1
    certificate_b = coordinator.full_stop(
        metadata=metadata("full", clock),
        stop_id=StopTransactionId("full-stop"),
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )
    assert certificate_b.stopped_domains == (DOMAIN_B,)
    assert coordinator.operation_status(request_b.operation_id).phase == "stopped"
    assert len(domain_b.stop_log) == 1


def test_full_stop_uses_one_certificate_for_two_live_domains() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request_a = operation(
        clock,
        "full-a",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    request_b = operation(
        clock,
        "full-b",
        participant(DOMAIN_B, "binding-b", "b.arm"),
    )
    coordinator.start(request_a)
    coordinator.start(request_b)
    stop_id = StopTransactionId("full-both")
    stop_metadata = metadata("full-both", clock)

    certificate = coordinator.full_stop(
        metadata=stop_metadata,
        stop_id=stop_id,
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )

    assert certificate.stop_id == stop_id
    assert certificate.stopped_domains == (DOMAIN_A, DOMAIN_B)
    assert certificate.measured_rest_domains == (DOMAIN_A, DOMAIN_B)
    assert len(domain_a.stop_log) == 1
    assert len(domain_b.stop_log) == 1
    assert coordinator.operation_status(request_a.operation_id).phase == "stopped"
    assert coordinator.operation_status(request_b.operation_id).phase == "stopped"
    assert (
        coordinator.full_stop(
            metadata=stop_metadata,
            stop_id=stop_id,
            absolute_deadline_ns=clock.now_ns() + 1_000,
        )
        is certificate
    )
    assert len(domain_a.stop_log) == 1
    assert len(domain_b.stop_log) == 1


def test_one_domain_fault_is_scoped_and_does_not_mutate_peer() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request_a = operation(
        clock,
        "faulted-op",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    request_b = operation(
        clock,
        "peer-op",
        participant(DOMAIN_B, "binding-b", "b.arm"),
    )
    coordinator.start(request_a)
    coordinator.start(request_b)

    certificate = coordinator.report_domain_fault(
        domain_id=DOMAIN_A,
        stop_id=StopTransactionId("fault-stop"),
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )

    assert certificate.stopped_domains == (DOMAIN_A,)
    assert domain_a.faulted
    assert len(domain_a.stop_log) == 1
    assert domain_b.stop_log == []
    assert coordinator.operation_status(request_b.operation_id).phase == "executing"


def test_operation_scoped_collision_stops_both_coordinated_participants() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    route_generation = coordinator.switch_route(
        domain_id=DOMAIN_A,
        route_id="chained",
    )
    assert route_generation is not None
    assert domain_a.active_route.kind is RouteKind.CHAINED
    assert domain_b.active_route.kind is RouteKind.DIRECT
    request = operation(
        clock,
        "collision-op",
        participant(
            DOMAIN_A,
            "binding-a",
            "a.arm",
            route_id="chained",
            route_generation=int(route_generation),
        ),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    coordinator.start(request)

    certificate = coordinator.report_collision(
        domain_id=DOMAIN_A,
        stop_id=StopTransactionId("collision-stop"),
        absolute_deadline_ns=clock.now_ns() + 1_000,
        consequence_scope="operation",
    )

    assert certificate.stopped_domains == (DOMAIN_A, DOMAIN_B)
    assert len(domain_a.stop_log) == 1
    assert len(domain_b.stop_log) == 1
    assert coordinator.operation_status(request.operation_id).phase == "stopped"


def test_external_route_generation_rejects_old_callback_and_is_reconciled() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "external-route-op",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    coordinator.start(request)
    goal_a, old_generation = coordinator.participant_goal(
        request.operation_id, DOMAIN_A
    )

    domain_a.switch_route("chained")
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_a,
            route_generation=old_generation,
            outcome="completed",
        )
        is CallbackDecision.STALE_ROUTE
    )

    assert coordinator.reconcile_external_route_change(DOMAIN_A) == (
        request.operation_id,
    )
    assert coordinator.operation_status(request.operation_id).phase == (
        "route_changed"
    )
    assert domain_a.stop_log == []
    assert len(domain_b.stop_log) == 1
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})


def test_direct_chained_route_change_and_stale_callbacks_are_fenced() -> None:
    clock, domain_a, _, coordinator = rig()
    old_generation = domain_a.route_generation

    new_generation = coordinator.switch_route(
        domain_id=DOMAIN_A,
        route_id="chained",
    )

    assert new_generation is not None
    assert new_generation != old_generation
    assert domain_a.active_route.kind is RouteKind.CHAINED
    assert domain_a.route_transition_log == [
        ("deactivate", "a.direct_jtc"),
        ("activate", "a.trajectory_filter"),
        ("activate", "a.forward_controller"),
    ]

    request = operation(
        clock,
        "route-op",
        participant(
            DOMAIN_A,
            "binding-a",
            "a.arm",
            route_id="chained",
            route_generation=int(new_generation),
        ),
    )
    coordinator.start(request)
    goal_id, generation = coordinator.participant_goal(
        request.operation_id, DOMAIN_A
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=GoalId("stale-goal"),
            route_generation=generation,
            outcome="completed",
        )
        is CallbackDecision.STALE_GOAL
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_id,
            route_generation=RouteGeneration(999),
            outcome="completed",
        )
        is CallbackDecision.STALE_ROUTE
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_id,
            route_generation=generation,
            outcome="completed",
        )
        is CallbackDecision.ACCEPTED
    )
    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_id,
            route_generation=generation,
            outcome="completed",
        )
        is CallbackDecision.STALE_OPERATION
    )


def test_controller_loss_fences_lost_resources_and_stops_survivor() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    request = operation(
        clock,
        "controller-loss-op",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    coordinator.start(request)
    unrelated = operation(
        clock,
        "unrelated-b",
        participant(DOMAIN_B, "binding-b-tool", "b.tool"),
    )
    assert coordinator.start(unrelated).phase == "executing"

    evidence = coordinator.report_controller_loss(DOMAIN_A)

    assert len(evidence) == 1
    assert evidence[0].operation_id == request.operation_id
    assert evidence[0].stopped_surviving_domains == (DOMAIN_B,)
    assert evidence[0].fenced_resources == (ResourceId("a.arm"),)
    assert domain_a.stop_log == []
    assert len(domain_b.stop_log) == 1
    assert domain_b.stop_log[0].operation_ids == (request.operation_id,)
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})
    assert coordinator.operation_status(request.operation_id).phase == "controller_lost"
    assert coordinator.operation_status(unrelated.operation_id).phase == "executing"

    rejected = operation(
        clock,
        "after-loss",
        participant(DOMAIN_A, "binding-a-new", "a.arm"),
    )
    assert coordinator.start(rejected).terminal_detail == "resource_overlap:a.arm"


def test_controller_loss_survivor_stop_failure_can_be_retried() -> None:
    clock, _, domain_b, coordinator = rig()
    request = operation(
        clock,
        "controller-loss-retry",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    coordinator.start(request)
    domain_b.fail_stop_for(
        StopTransactionId(
            "controller-loss-stop:domain-a:controller-loss-retry"
        )
    )

    evidence = coordinator.report_controller_loss(DOMAIN_A)

    assert evidence[0].stopped_surviving_domains == ()
    assert coordinator.operation_status(request.operation_id).phase == (
        "controller_lost_stop_failed"
    )
    assert coordinator.fenced_resources == frozenset(
        {ResourceId("a.arm"), ResourceId("b.arm")}
    )

    retry = coordinator.stop(
        stop_request(
            clock,
            "controller-loss-retry-stop",
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert retry.stopped_domains == (DOMAIN_B,)
    assert coordinator.operation_status(request.operation_id).phase == (
        "controller_lost"
    )
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})


def test_failed_callback_survivor_stop_can_be_retried() -> None:
    clock, _, domain_b, coordinator = rig()
    request = operation(
        clock,
        "callback-failure-retry",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_B, "binding-b", "b.arm"),
        coordinated=True,
    )
    coordinator.start(request)
    domain_b.fail_stop_for(
        StopTransactionId(
            "callback-failure-stop:domain-a:callback-failure-retry"
        )
    )
    goal_a, generation_a = coordinator.participant_goal(
        request.operation_id, DOMAIN_A
    )

    assert (
        coordinator.controller_callback(
            operation_id=request.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_a,
            route_generation=generation_a,
            outcome="failed",
        )
        is CallbackDecision.ACCEPTED
    )
    assert coordinator.operation_status(request.operation_id).phase == (
        "failed_stop_failed"
    )
    assert coordinator.fenced_resources == frozenset(
        {ResourceId("a.arm"), ResourceId("b.arm")}
    )

    retry = coordinator.stop(
        stop_request(
            clock,
            "callback-failure-retry-stop",
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert retry.stopped_domains == (DOMAIN_B,)
    assert coordinator.operation_status(request.operation_id).phase == "failed"
    assert coordinator.fenced_resources == frozenset({ResourceId("a.arm")})


def test_execution_host_loss_is_status_only_and_closes_new_admission() -> None:
    clock, domain_a, domain_b, coordinator = rig()
    active = operation(
        clock,
        "active-at-host-loss",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    coordinator.start(active)
    goal_id, generation = coordinator.participant_goal(
        active.operation_id, DOMAIN_A
    )
    status_before = coordinator.operation_status(active.operation_id)
    calls_before = (len(domain_a.calls), len(domain_b.calls))

    evidence = coordinator.report_execution_host_loss()

    assert evidence.active_operation_ids == (active.operation_id,)
    assert evidence.status_only
    assert evidence.domain_mutations_before == evidence.domain_mutations_after
    assert (len(domain_a.calls), len(domain_b.calls)) == calls_before
    assert coordinator.operation_status(active.operation_id) == status_before
    assert (
        coordinator.controller_callback(
            operation_id=active.operation_id,
            domain_id=DOMAIN_A,
            goal_id=goal_id,
            route_generation=generation,
            outcome="completed",
        )
        is CallbackDecision.STALE_OPERATION
    )
    assert (
        coordinator.switch_route(domain_id=DOMAIN_B, route_id="chained")
        is None
    )
    host_stop = coordinator.stop(
        stop_request(
            clock,
            "host-loss-stop",
            StopScope(operation_ids=(active.operation_id,)),
        )
    )
    assert host_stop.outcome == "not_accepted"
    fault_stop = coordinator.report_domain_fault(
        domain_id=DOMAIN_A,
        stop_id=StopTransactionId("host-loss-fault"),
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )
    assert fault_stop.outcome == "not_accepted"
    assert not domain_a.faulted
    collision_stop = coordinator.report_collision(
        domain_id=DOMAIN_A,
        stop_id=StopTransactionId("host-loss-collision"),
        absolute_deadline_ns=clock.now_ns() + 1_000,
        consequence_scope="full",
    )
    assert collision_stop.outcome == "not_accepted"
    assert coordinator.report_controller_loss(DOMAIN_A) == ()
    assert domain_a.controller_available
    assert (len(domain_a.calls), len(domain_b.calls)) == calls_before

    rejected = operation(
        clock,
        "rejected-after-host-loss",
        participant(DOMAIN_B, "binding-b", "b.arm"),
    )
    status = coordinator.start(rejected)
    assert status.phase == "rejected"
    assert status.terminal_detail == "execution_host_unavailable"


def test_retained_binding_snapshot_is_checked_before_prepare() -> None:
    clock, domain_a, _, coordinator = rig()
    retained = participant(DOMAIN_A, "binding-a", "a.arm")
    candidates = (
        replace(retained, binding_revision=BindingRevision(2)),
        replace(retained, control_domain_id=DOMAIN_B),
        replace(retained, resource_ids=(ResourceId("a.tool"),)),
        replace(retained, model_revision=ModelRevision("model-2")),
    )
    expected = (
        "stale_binding_revision",
        "binding_domain_mismatch",
        "binding_resource_mismatch",
        "stale_binding_snapshot",
    )

    for index, (candidate, reason) in enumerate(zip(candidates, expected)):
        status = coordinator.start(
            operation(clock, f"invalid-binding-{index}", candidate)
        )
        assert status.phase == "rejected"
        assert status.terminal_detail == reason

    assert domain_a.prepare_attempts == []


def test_two_participants_in_one_domain_keep_distinct_goals_and_stop_together() -> None:
    clock, domain_a, _, coordinator = rig()
    request = operation(
        clock,
        "arm-and-tool",
        participant(DOMAIN_A, "binding-a", "a.arm"),
        participant(DOMAIN_A, "binding-a-tool", "a.tool"),
        coordinated=True,
    )

    assert coordinator.start(request).phase == "executing"
    arm_goal, _ = coordinator.participant_goal(
        request.operation_id,
        DOMAIN_A,
        ControlBindingId("binding-a"),
    )
    tool_goal, _ = coordinator.participant_goal(
        request.operation_id,
        DOMAIN_A,
        ControlBindingId("binding-a-tool"),
    )
    assert arm_goal != tool_goal
    with pytest.raises(ValueError, match="binding ID is required"):
        coordinator.participant_goal(request.operation_id, DOMAIN_A)

    certificate = coordinator.stop(
        stop_request(
            clock,
            "same-domain-stop",
            StopScope(operation_ids=(request.operation_id,)),
        )
    )
    assert certificate.outcome == "stopped"
    assert certificate.stopped_domains == (DOMAIN_A,)
    assert certificate.measured_rest_domains == (DOMAIN_A,)
    assert domain_a.stop_log[-1].operation_ids == (request.operation_id,)
    assert coordinator.active_resource_locks == {}


def test_stop_id_replay_requires_exact_scope_deadline_and_metadata() -> None:
    clock, domain_a, _, coordinator = rig()
    request = operation(
        clock,
        "replay-target",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    coordinator.start(request)
    stop = stop_request(
        clock,
        "exact-replay",
        StopScope(operation_ids=(request.operation_id,)),
    )
    certificate = coordinator.stop(stop)
    before = (
        tuple(domain_a.stop_log),
        coordinator.operation_status(request.operation_id),
        coordinator.trace,
    )

    assert coordinator.stop(stop) is certificate
    with pytest.raises(StopReplayConflict, match="different scope"):
        coordinator.stop(
            replace(stop, absolute_deadline_ns=stop.absolute_deadline_ns + 1)
        )
    with pytest.raises(StopReplayConflict, match="different scope"):
        coordinator.stop(
            replace(
                stop,
                scope=StopScope(control_domain_ids=(DOMAIN_A,)),
            )
        )
    assert (
        tuple(domain_a.stop_log),
        coordinator.operation_status(request.operation_id),
        coordinator.trace,
    ) == before


def test_coordinator_serializes_concurrent_overlapping_admission() -> None:
    clock, domain_a, _, coordinator = rig()
    barrier = Barrier(2)
    requests = (
        operation(
            clock,
            "concurrent-a",
            participant(DOMAIN_A, "binding-a", "a.arm"),
        ),
        operation(
            clock,
            "concurrent-b",
            participant(DOMAIN_A, "binding-a-new", "a.arm"),
        ),
    )

    def start(request: OperationRequest):
        barrier.wait()
        return coordinator.start(request)

    with ThreadPoolExecutor(max_workers=2) as executor:
        statuses = tuple(executor.map(start, requests))

    assert sorted(status.phase for status in statuses) == [
        "executing",
        "rejected",
    ]
    assert len(domain_a.dispatch_log) == 1
    assert len(coordinator.active_resource_locks) == 1


def test_full_stop_fences_new_motion_until_explicit_terminal_handoff() -> None:
    clock, _, _, coordinator = rig()
    running = operation(
        clock,
        "before-full-stop",
        participant(DOMAIN_A, "binding-a", "a.arm"),
    )
    assert coordinator.start(running).phase == "executing"
    stop_id = StopTransactionId("handoff-full-stop")
    certificate = coordinator.full_stop(
        metadata=metadata("handoff-full-stop", clock),
        stop_id=stop_id,
        absolute_deadline_ns=clock.now_ns() + 1_000,
    )
    assert certificate.outcome == "stopped"
    assert coordinator.pending_full_stop_handoffs == frozenset({stop_id})

    blocked = operation(
        clock,
        "blocked-before-handoff",
        participant(DOMAIN_A, "binding-a-new", "a.arm"),
    )
    assert coordinator.start(blocked).terminal_detail == (
        "full_stop_settlement_pending"
    )

    assert coordinator.settle_stop_handoff(stop_id) is certificate
    assert coordinator.pending_full_stop_handoffs == frozenset()
    admitted = operation(
        clock,
        "admitted-after-handoff",
        participant(DOMAIN_A, "binding-a-new", "a.arm"),
    )
    assert coordinator.start(admitted).phase == "executing"


def test_rt_ticks_do_not_call_nrt_or_create_fake_dds_work() -> None:
    _, domain_a, domain_b, coordinator = rig()
    nrt_before = (domain_a.nrt_call_count, domain_b.nrt_call_count)
    for _ in range(128):
        domain_a.rt_tick()
        domain_b.rt_tick()

    measurement = coordinator.measure_isolation()

    assert measurement.rt_ticks == ((DOMAIN_A, 128), (DOMAIN_B, 128))
    assert (domain_a.nrt_call_count, domain_b.nrt_call_count) == nrt_before
    assert measurement.nrt_calls_from_rt == ((DOMAIN_A, 0), (DOMAIN_B, 0))
    assert measurement.fake_dds_entities == 0
    assert measurement.process_threads >= 1
    assert measurement.max_rss_kib > 0


def test_rt_path_probe_records_latency_jitter_load_and_resource_boundaries() -> None:
    _, domain_a, domain_b, coordinator = rig()

    measurement = coordinator.measure_rt_path(iterations=256)

    assert measurement.samples == 512
    assert measurement.elapsed_ns > 0
    assert measurement.process_cpu_ns > 0
    assert 0 <= measurement.p50_ns <= measurement.p95_ns <= measurement.max_ns
    assert (
        measurement.p95_jitter_from_median_ns
        == measurement.p95_ns - measurement.p50_ns
    )
    assert measurement.process_load_ratio >= 0
    assert measurement.threads_before == measurement.threads_after
    assert measurement.max_rss_kib > 0
    assert measurement.nrt_calls_from_rt == 0
    assert measurement.fake_dds_entities_created == 0
    assert domain_a.rt_tick_count == domain_b.rt_tick_count == 256
