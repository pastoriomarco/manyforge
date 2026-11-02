from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import hashlib
from threading import Barrier

from verification.composable_cell_i1a.contracts import (
    BindingRevision,
    ControlDomainId,
    ControlRequest,
    ControllerTopologyRevision,
    DomainRequestPrecondition,
    GraphRevision,
    RequestId,
    RequestKind,
    RequestMetadata,
    RequestPreconditions,
    RequestScope,
    ResourceId,
    RouteGeneration,
)
from verification.composable_cell_i1a.fakes import FakeClock
from verification.composable_cell_i1a.request_ordering_prototype import (
    DecisionState,
    DeterministicRequestArbiter,
)


def request(
    request_id: str,
    kind: RequestKind,
    *,
    domains: tuple[str, ...] = (),
    resources: tuple[str, ...] = (),
    source: str = "composer",
    payload: str | None = None,
    expected_graph_revision: int | None = 0,
    expected_binding_revision: int | None = None,
    expected_topology_revision: int | None = None,
    expected_route_generation: int | None = None,
) -> ControlRequest:
    payload_text = payload or request_id
    return ControlRequest(
        metadata=RequestMetadata(
            request_id=RequestId(request_id),
            source_label=source,
            transport="in_process",
        ),
        kind=kind,
        scope=RequestScope(
            control_domain_ids=tuple(
                ControlDomainId(item) for item in domains
            ),
            resource_ids=tuple(ResourceId(item) for item in resources),
        ),
        preconditions=RequestPreconditions(
            expected_graph_revision=(
                None
                if expected_graph_revision is None
                else GraphRevision(expected_graph_revision)
            ),
            domains=tuple(
                DomainRequestPrecondition(
                    control_domain_id=ControlDomainId(domain),
                    expected_binding_revision=(
                        None
                        if expected_binding_revision is None
                        else BindingRevision(expected_binding_revision)
                    ),
                    expected_topology_revision=(
                        None
                        if expected_topology_revision is None
                        else ControllerTopologyRevision(
                            expected_topology_revision
                        )
                    ),
                    expected_route_generation=(
                        None
                        if expected_route_generation is None
                        else RouteGeneration(expected_route_generation)
                    ),
                )
                for domain in domains
            ),
        ),
        payload_sha256=hashlib.sha256(payload_text.encode()).hexdigest(),
    )


def arbiter(
    clock: FakeClock,
    *,
    queue_capacity: int = 2,
    replay_capacity: int = 8,
) -> DeterministicRequestArbiter:
    return DeterministicRequestArbiter(
        clock=clock,
        queue_capacity=queue_capacity,
        replay_capacity=replay_capacity,
    )


def test_duplicate_replays_exact_decision_conflict_rejects_and_replay_is_bounded() -> None:
    clock = FakeClock(start_ns=10)
    candidate = arbiter(clock, replay_capacity=2)
    first_request = request(
        "stop-1",
        RequestKind.STOP,
        domains=("domain-a",),
        expected_graph_revision=None,
    )

    first = candidate.submit(first_request)
    clock.advance_ns(100)
    replay = candidate.submit(first_request)
    conflict = candidate.submit(
        request(
            "stop-1",
            RequestKind.STOP,
            domains=("domain-a",),
            payload="different",
            expected_graph_revision=None,
        )
    )

    assert replay == first
    assert replay.decided_monotonic_ns == 10
    assert conflict.state is DecisionState.REJECTED
    assert conflict.reason == "conflicting_request_id"
    candidate.settle_stop(RequestId("stop-1"))
    candidate.submit(
        request(
            "stop-2",
            RequestKind.STOP,
            domains=("domain-a",),
            expected_graph_revision=None,
        )
    )
    candidate.settle_stop(RequestId("stop-2"))
    candidate.submit(
        request(
            "stop-3",
            RequestKind.STOP,
            domains=("domain-a",),
            expected_graph_revision=None,
        )
    )
    candidate.settle_stop(RequestId("stop-3"))
    assert candidate.replay_size == 2
    assert candidate.decision(RequestId("stop-1")) is None


def test_disjoint_domains_run_concurrently_same_domain_queues_and_backpressures() -> None:
    clock = FakeClock()
    candidate = arbiter(clock, queue_capacity=1)

    first = candidate.submit(
        request(
            "run-a",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    second = candidate.submit(
        request(
            "run-b",
            RequestKind.START,
            domains=("domain-b",),
            resources=("joint/b",),
        )
    )
    overlap = candidate.submit(
        request(
            "run-overlap",
            RequestKind.START,
            domains=("domain-c",),
            resources=("joint/a",),
        )
    )
    queued = candidate.submit(
        request(
            "run-queued",
            RequestKind.START,
            domains=("domain-a",),
            resources=("tool/a",),
        )
    )
    saturated = candidate.submit(
        request(
            "run-saturated",
            RequestKind.START,
            domains=("domain-a",),
            resources=("camera/a",),
        )
    )

    assert first.state is DecisionState.ACTIVE
    assert second.state is DecisionState.ACTIVE
    assert candidate.active_request_ids == (
        RequestId("run-a"),
        RequestId("run-b"),
    )
    assert overlap.reason == "overlapping_resource_conflict"
    assert queued.state is DecisionState.QUEUED
    assert saturated.reason == "queue_backpressure"

    transitions = candidate.complete(RequestId("run-a"))
    assert transitions[0].state is DecisionState.COMPLETED
    assert transitions[1].request_id == RequestId("run-queued")
    assert transitions[1].state is DecisionState.ACTIVE


def test_concurrent_transport_submissions_are_atomically_serialized() -> None:
    clock = FakeClock()
    request_count = 8
    candidate = arbiter(clock, queue_capacity=request_count - 1)
    barrier = Barrier(request_count)

    def submit(index: int):
        barrier.wait()
        return candidate.submit(
            request(
                f"run-{index}",
                RequestKind.START,
                domains=("domain-a",),
                resources=(f"joint/{index}",),
            )
        )

    with ThreadPoolExecutor(max_workers=request_count) as executor:
        decisions = tuple(executor.map(submit, range(request_count)))

    assert sum(item.state is DecisionState.ACTIVE for item in decisions) == 1
    assert sum(item.state is DecisionState.QUEUED for item in decisions) == 7
    assert sorted(item.sequence for item in decisions) == list(
        range(1, request_count + 1)
    )
    assert len(candidate.active_request_ids) == 1
    assert len(candidate.queued_request_ids) == 7
    assert set(candidate.active_request_ids).isdisjoint(
        candidate.queued_request_ids
    )


def test_stale_revision_is_rejected_and_external_change_retires_queued_work() -> None:
    clock = FakeClock()
    candidate = arbiter(clock)
    candidate.observe_external_revision(
        graph_revision=3,
        binding_revisions={ControlDomainId("domain-a"): 4},
    )

    stale_graph = candidate.submit(
        request(
            "stale-graph",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=2,
            expected_binding_revision=4,
        )
    )
    stale_binding = candidate.submit(
        request(
            "stale-binding",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=3,
            expected_binding_revision=3,
        )
    )
    active = candidate.submit(
        request(
            "active",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=3,
            expected_binding_revision=4,
        )
    )
    queued = candidate.submit(
        request(
            "queued",
            RequestKind.START,
            domains=("domain-a",),
            resources=("tool/a",),
            expected_graph_revision=3,
            expected_binding_revision=4,
        )
    )

    assert stale_graph.reason == "stale_graph_revision"
    assert stale_binding.reason == "stale_binding_revision"
    assert active.state is DecisionState.ACTIVE
    assert queued.state is DecisionState.QUEUED
    retired = candidate.observe_external_revision(
        binding_revisions={ControlDomainId("domain-a"): 5}
    )
    assert len(retired) == 1
    assert retired[0].request_id == RequestId("queued")
    assert retired[0].reason == "stale_binding_revision_while_queued"
    assert candidate.decision(RequestId("active")).state is DecisionState.ACTIVE


def test_external_topology_and_route_changes_reject_or_retire_stale_work() -> None:
    clock = FakeClock()
    candidate = arbiter(clock)
    domain = ControlDomainId("domain-a")
    candidate.observe_external_revision(
        graph_revision=3,
        topology_revisions={domain: 7},
        route_generations={domain: 11},
    )

    stale_topology = candidate.submit(
        request(
            "stale-topology",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=3,
            expected_topology_revision=6,
            expected_route_generation=11,
        )
    )
    stale_route = candidate.submit(
        request(
            "stale-route",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=3,
            expected_topology_revision=7,
            expected_route_generation=10,
        )
    )
    active = candidate.submit(
        request(
            "active-current-route",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
            expected_graph_revision=3,
            expected_topology_revision=7,
            expected_route_generation=11,
        )
    )
    queued = candidate.submit(
        request(
            "queued-current-route",
            RequestKind.START,
            domains=("domain-a",),
            resources=("tool/a",),
            expected_graph_revision=3,
            expected_topology_revision=7,
            expected_route_generation=11,
        )
    )

    assert stale_topology.reason == "stale_topology_revision"
    assert stale_route.reason == "stale_route_generation"
    assert active.state is DecisionState.ACTIVE
    assert queued.state is DecisionState.QUEUED

    retired = candidate.observe_external_revision(
        route_generations={domain: 12}
    )

    assert [item.request_id for item in retired] == [
        RequestId("queued-current-route")
    ]
    assert retired[0].reason == "stale_route_generation_while_queued"
    assert candidate.decision(RequestId("active-current-route")).state is (
        DecisionState.ACTIVE
    )


def test_stop_precedes_saturated_queue_and_cancels_queued_work() -> None:
    clock = FakeClock()
    candidate = arbiter(clock, queue_capacity=1)
    active = candidate.submit(
        request(
            "active",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    queued = candidate.submit(
        request(
            "queued",
            RequestKind.START,
            domains=("domain-a",),
            resources=("tool/a",),
        )
    )
    assert active.state is DecisionState.ACTIVE
    assert queued.state is DecisionState.QUEUED

    stop_request = request(
        "stop",
        RequestKind.STOP,
        domains=("domain-a",),
        expected_graph_revision=None,
    )
    stop = candidate.submit(stop_request)

    assert stop.state is DecisionState.ACCEPTED
    assert stop.reason == "stop_precedence"
    assert set(stop.affected_request_ids) == {
        RequestId("active"),
        RequestId("queued"),
    }
    assert candidate.queued_request_ids == ()
    assert candidate.stop_requested_request_ids == (RequestId("active"),)
    assert candidate.decision(RequestId("queued")).state is DecisionState.CANCELLED
    assert candidate.submit(stop_request) == stop


def test_full_stop_fences_new_motion_until_explicit_terminal_settlement() -> None:
    clock = FakeClock()
    candidate = arbiter(clock)
    active = candidate.submit(
        request(
            "active-before-full-stop",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    full_stop_request = request(
        "full-stop",
        RequestKind.FULL_STOP,
        domains=("domain-a",),
        expected_graph_revision=None,
    )

    accepted = candidate.submit(full_stop_request)
    assert accepted.state is DecisionState.ACCEPTED
    assert candidate.pending_stop_request_ids == (RequestId("full-stop"),)
    blocked = candidate.submit(
        request(
            "blocked-during-settlement",
            RequestKind.START,
            domains=("domain-b",),
            resources=("joint/b",),
        )
    )
    assert blocked.reason == "full_stop_settlement_pending"

    candidate.complete(active.request_id, reason="stopped_by_full_stop")
    settled = candidate.settle_stop(RequestId("full-stop"))
    assert settled.state is DecisionState.COMPLETED
    assert candidate.pending_stop_request_ids == ()
    admitted = candidate.submit(
        request(
            "after-settlement",
            RequestKind.START,
            domains=("domain-b",),
            resources=("joint/b",),
        )
    )
    assert admitted.state is DecisionState.ACTIVE


def test_apply_disable_and_route_conflicts_are_explicit() -> None:
    clock = FakeClock()
    candidate = arbiter(clock)
    assert (
        candidate.submit(
            request(
                "run",
                RequestKind.START,
                domains=("domain-a",),
                resources=("joint/a",),
            )
        ).state
        is DecisionState.ACTIVE
    )

    apply_while_running = candidate.submit(
        request(
            "apply-busy",
            RequestKind.APPLY,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    disable_while_running = candidate.submit(
        request(
            "disable-busy",
            RequestKind.DISABLE,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    route_while_running = candidate.submit(
        request(
            "route-busy",
            RequestKind.SELECT_ROUTE,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    assert apply_while_running.reason == "apply_affected_resources_busy"
    assert disable_while_running.reason == "disable_execution_conflict"
    assert route_while_running.reason == "select_route_execution_conflict"

    candidate.complete(RequestId("run"))
    active_apply = candidate.submit(
        request(
            "apply",
            RequestKind.APPLY,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    run_while_apply = candidate.submit(
        request(
            "run-while-apply",
            RequestKind.START,
            domains=("domain-a",),
            resources=("joint/a",),
        )
    )
    assert active_apply.state is DecisionState.ACTIVE
    assert run_while_apply.reason == "apply_busy"


def test_client_and_execution_host_loss_are_status_only_until_reconciled() -> None:
    clock = FakeClock()
    candidate = arbiter(clock)
    run_request = request(
        "run",
        RequestKind.START,
        domains=("domain-a",),
        resources=("joint/a",),
        source="plc",
    )
    active = candidate.submit(run_request)
    queued = candidate.submit(
        request(
            "queued-before-host-loss",
            RequestKind.START,
            domains=("domain-a",),
            resources=("tool/a",),
            source="plc",
        )
    )
    assert queued.state is DecisionState.QUEUED
    before_stop_generation = candidate.stop_generation

    disconnected = candidate.observe_client_disconnect("plc")
    lost = candidate.observe_execution_host(alive=False)

    assert disconnected == (
        RequestId("run"),
        RequestId("queued-before-host-loss"),
    )
    assert lost.orphaned_request_ids == (RequestId("run"),)
    assert candidate.stop_generation == before_stop_generation
    assert candidate.decision(RequestId("run")) == active
    assert candidate.stop_requested_request_ids == ()
    unavailable = candidate.submit(
        request(
            "new-run",
            RequestKind.START,
            domains=("domain-b",),
            resources=("joint/b",),
        )
    )
    assert unavailable.reason == "execution_host_unavailable"

    transitions = candidate.reconcile_orphan(
        RequestId("run"), terminal_reason="observed_orphan_terminal"
    )
    assert transitions[0].state is DecisionState.COMPLETED
    assert transitions[0].reason == "observed_orphan_terminal"
    assert len(transitions) == 1
    assert candidate.decision(RequestId("queued-before-host-loss")).state is (
        DecisionState.QUEUED
    )
    assert candidate.active_request_ids == ()

    recovered = candidate.observe_execution_host(alive=True)
    assert "explicit_orphan_reconciliation" in recovered.detail
    assert candidate.decision(RequestId("run")).state is DecisionState.COMPLETED
    assert candidate.decision(RequestId("queued-before-host-loss")).state is (
        DecisionState.QUEUED
    )
