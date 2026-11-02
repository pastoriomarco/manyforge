from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from threading import Event

import pytest

from verification.composable_cell_i1a.contracts import (
    ControlBindingId,
    ControlDomainId,
    GoalId,
    OperationId,
    ResourceId,
    StopPhaseBudget,
    StopScope,
    StopTransactionId,
    StoppedGoalSnapshot,
)
from verification.composable_cell_i1a.fakes import FakeClock
from verification.composable_cell_i1a.stop_envelope_prototype import (
    CancellationSignal,
    CancellablePhaseWait,
    EvidenceAgeNs,
    EvidenceDecision,
    EvidenceSequence,
    StopEnvelopePrototype,
    StopGeneration,
    StopProfile,
    StopTarget,
    WaitState,
)


DOMAIN_A = ControlDomainId("domain-a")
DOMAIN_B = ControlDomainId("domain-b")
GENERATION = StopGeneration(7)


class SteppingClock:
    """Advances after every read to expose accidental TOCTOU reads."""

    def __init__(self, start_ns: int = 0) -> None:
        self.value = start_ns

    def now_ns(self) -> int:
        observed = self.value
        self.value += 1
        return observed


def resource(domain_id: ControlDomainId, suffix: str = "arm") -> ResourceId:
    return ResourceId(f"{domain_id}.{suffix}")


def goal(domain_id: ControlDomainId, suffix: str = "motion") -> GoalId:
    return GoalId(f"{domain_id}.{suffix}")


def goal_snapshot(
    domain_id: ControlDomainId,
    suffix: str = "motion",
) -> StoppedGoalSnapshot:
    return StoppedGoalSnapshot(
        control_domain_id=domain_id,
        operation_id=OperationId(f"{domain_id}.{suffix}.operation"),
        control_binding_id=ControlBindingId(f"{domain_id}.{suffix}.binding"),
        goal_id=goal(domain_id, suffix),
    )


def envelope(
    clock,
    *,
    stop_id: str = "stop-1",
    deadline_ns: int = 1_100,
    domains: tuple[ControlDomainId, ...] = (DOMAIN_A,),
    freshness_ns: int = 20,
) -> StopEnvelopePrototype:
    return StopEnvelopePrototype(
        clock=clock,
        stop_id=StopTransactionId(stop_id),
        scope=StopScope(control_domain_ids=domains),
        absolute_deadline_ns=deadline_ns,
        targets=tuple(
            StopTarget(
                domain_id=domain_id,
                resource_ids=(resource(domain_id),),
                goals=(goal_snapshot(domain_id),),
            )
            for domain_id in domains
        ),
        measured_sample_freshness_ns=freshness_ns,
        stop_generation=GENERATION,
    )


def result(
    stop: StopEnvelopePrototype,
    domain_id: ControlDomainId,
    *,
    goal_id: GoalId | None = None,
    sequence: int = 1,
    generation: StopGeneration = GENERATION,
) -> EvidenceDecision:
    return stop.record_controller_result(
        domain_id=domain_id,
        goal_id=goal_id or goal(domain_id),
        evidence_sequence=EvidenceSequence(sequence),
        stop_generation=generation,
    )


def rest(
    stop: StopEnvelopePrototype,
    domain_id: ControlDomainId,
    *,
    sample_id: str = "rest",
    resources: tuple[ResourceId, ...] | None = None,
    sequence: int = 2,
    age_ns: int = 0,
    generation: StopGeneration = GENERATION,
    at_rest: bool = True,
    complete: bool = True,
) -> EvidenceDecision:
    return stop.record_measured_rest(
        domain_id=domain_id,
        sample_id=f"{domain_id}.{sample_id}",
        resource_ids=resources or (resource(domain_id),),
        evidence_sequence=EvidenceSequence(sequence),
        evidence_age_ns=EvidenceAgeNs(age_ns),
        stop_generation=generation,
        at_rest=at_rest,
        complete=complete,
    )


def settle_domain(
    stop: StopEnvelopePrototype,
    domain_id: ControlDomainId,
) -> None:
    assert result(stop, domain_id) is EvidenceDecision.ACCEPTED
    assert rest(stop, domain_id) is EvidenceDecision.ACCEPTED


def test_xarm_and_ur_phase_deadlines_share_one_absolute_envelope() -> None:
    clock = FakeClock(start_ns=100)
    stop = envelope(clock)

    xarm = stop.profile_budgets(StopProfile.XARM)
    ur = stop.profile_budgets(StopProfile.UR)

    assert [phase.name for phase in xarm] == [
        "xarm.cancel_goal",
        "xarm.vendor_stop",
        "xarm.measured_rest",
    ]
    assert [phase.absolute_deadline_ns for phase in xarm] == [300, 800, 1_100]
    assert [phase.absolute_deadline_ns for phase in ur] == [400, 800, 1_100]
    assert xarm[-1].absolute_deadline_ns == ur[-1].absolute_deadline_ns
    clock.advance_ns(750)
    assert (
        stop.nested_budget("controller_ack", 500, parent=xarm[1])
        .absolute_deadline_ns
        == xarm[1].absolute_deadline_ns
    )


def test_cancellation_wakes_wait_without_deadline_poll() -> None:
    clock = FakeClock(start_ns=10)
    stop = envelope(clock, deadline_ns=1_010)
    cancellation = CancellationSignal()
    wait = CancellablePhaseWait(
        clock,
        stop.profile_budgets(StopProfile.XARM)[1],
        cancellation,
    )

    clock.advance_ns(17)
    assert cancellation.cancel()
    assert wait.state is WaitState.CANCELLED
    assert wait.awakened_monotonic_ns == 27
    assert not wait.complete()
    assert not cancellation.cancel()


def test_equal_deadline_is_timely_and_clock_is_sampled_once_per_event() -> None:
    clock = SteppingClock()
    stop = envelope(clock, deadline_ns=2, freshness_ns=0)

    assert result(stop, DOMAIN_A) is EvidenceDecision.ACCEPTED
    assert stop.evidence_log[-1].received_monotonic_ns == 1
    assert rest(stop, DOMAIN_A) is EvidenceDecision.ACCEPTED
    assert stop.evidence_log[-1].received_monotonic_ns == 2
    assert stop.certificate is not None
    assert stop.certificate.completed_monotonic_ns == 2

    wait_clock = SteppingClock(start_ns=10)
    wait = CancellablePhaseWait(
        wait_clock,
        StopPhaseBudget("equal-boundary", 10),
        CancellationSignal(),
    )
    assert wait.complete()
    assert wait.awakened_monotonic_ns == 10


def test_late_evidence_cannot_mutate_terminal_state_or_audit_log() -> None:
    clock = FakeClock(start_ns=0)
    stop = envelope(clock, deadline_ns=10)
    clock.advance_ns(11)

    assert result(stop, DOMAIN_A) is EvidenceDecision.LATE
    certificate = stop.observe_deadline()
    assert certificate is not None
    before = (
        stop.evidence_log,
        stop.certificates,
        stop.terminal_publish_attempts,
    )

    assert rest(stop, DOMAIN_A) is EvidenceDecision.TERMINAL_IGNORED
    assert stop.fail(outcome="failed", detail="late fail") is certificate
    assert (
        stop.evidence_log,
        stop.certificates,
        stop.terminal_publish_attempts,
    ) == before


def test_generation_order_age_and_exact_resource_scope_are_local() -> None:
    clock = FakeClock(start_ns=100)
    stop = envelope(clock, deadline_ns=200, freshness_ns=5)

    assert rest(stop, DOMAIN_A) is EvidenceDecision.OUT_OF_ORDER
    assert (
        result(stop, DOMAIN_A, generation=StopGeneration(6))
        is EvidenceDecision.STALE
    )
    assert (
        result(stop, DOMAIN_A, goal_id=GoalId("other.goal"))
        is EvidenceDecision.WRONG_SCOPE
    )
    assert result(stop, DOMAIN_A, sequence=10) is EvidenceDecision.ACCEPTED
    assert result(stop, DOMAIN_A, sequence=10) is EvidenceDecision.DUPLICATE
    assert result(stop, DOMAIN_A, sequence=11) is EvidenceDecision.INVALID
    assert (
        rest(
            stop,
            DOMAIN_A,
            sample_id="wrong-resource",
            resources=(ResourceId("domain-a.tool"),),
            sequence=11,
        )
        is EvidenceDecision.WRONG_SCOPE
    )
    assert (
        rest(stop, DOMAIN_A, sample_id="old-sequence", sequence=10)
        is EvidenceDecision.OUT_OF_ORDER
    )
    assert (
        rest(stop, DOMAIN_A, sample_id="stale-age", sequence=11, age_ns=6)
        is EvidenceDecision.STALE
    )
    assert (
        rest(stop, DOMAIN_A, sample_id="moving", sequence=11, at_rest=False)
        is EvidenceDecision.MOVING
    )
    assert (
        rest(stop, DOMAIN_A, sample_id="fresh", sequence=11)
        is EvidenceDecision.ACCEPTED
    )
    assert stop.certificate is not None


def test_multiple_goals_in_one_domain_all_bind_to_one_exact_rest_sample() -> None:
    clock = FakeClock(start_ns=0)
    resources = (resource(DOMAIN_A), resource(DOMAIN_A, "gripper"))
    goal_snapshots = (
        goal_snapshot(DOMAIN_A),
        goal_snapshot(DOMAIN_A, "gripper"),
    )
    goals = tuple(snapshot.goal_id for snapshot in goal_snapshots)
    stop = StopEnvelopePrototype(
        clock=clock,
        stop_id=StopTransactionId("arm-and-gripper"),
        scope=StopScope(resource_ids=resources),
        absolute_deadline_ns=100,
        targets=(
            StopTarget(
                domain_id=DOMAIN_A,
                resource_ids=resources,
                goals=goal_snapshots,
            ),
        ),
        measured_sample_freshness_ns=10,
        stop_generation=GENERATION,
    )

    assert result(stop, DOMAIN_A, goal_id=goals[0], sequence=1).value == "accepted"
    assert (
        rest(
            stop,
            DOMAIN_A,
            resources=resources,
            sequence=3,
        )
        is EvidenceDecision.OUT_OF_ORDER
    )
    assert result(stop, DOMAIN_A, goal_id=goals[1], sequence=2).value == "accepted"
    assert (
        rest(
            stop,
            DOMAIN_A,
            sample_id="all-rest",
            resources=resources,
            sequence=3,
        )
        is EvidenceDecision.ACCEPTED
    )
    assert stop.certificate is not None
    assert stop.certificate.stopped_domains == (DOMAIN_A,)
    assert stop.certificate.stopped_resources == resources
    assert stop.certificate.stopped_goals == goal_snapshots


def test_one_serialized_terminal_owner_wins_race_and_freezes_evidence() -> None:
    clock = FakeClock(start_ns=0)
    stop = envelope(clock, deadline_ns=100)
    first_terminal = Event()

    def fail_first():
        certificate = stop.fail(outcome="operator_failed", detail="first receiver")
        first_terminal.set()
        return certificate

    def evidence_after_terminal():
        first_terminal.wait()
        return result(stop, DOMAIN_A)

    def deadline_after_terminal():
        first_terminal.wait()
        clock.advance_ns(101)
        return stop.observe_deadline()

    with ThreadPoolExecutor(max_workers=3) as executor:
        fail_future = executor.submit(fail_first)
        evidence_future = executor.submit(evidence_after_terminal)
        deadline_future = executor.submit(deadline_after_terminal)
        winner = fail_future.result()
        assert evidence_future.result() is EvidenceDecision.TERMINAL_IGNORED
        assert deadline_future.result() is winner

    assert stop.certificate is winner
    assert stop.certificates == (winner,)
    assert stop.evidence_log == ()
    assert stop.terminal_publish_attempts == 1


def test_success_certificate_cannot_bypass_measured_rest() -> None:
    stop = envelope(FakeClock(), deadline_ns=100)
    with pytest.raises(ValueError, match="requires measured rest"):
        stop.publish_terminal(outcome="stopped", detail="uncertified")
    assert stop.certificate is None


def test_two_domains_share_one_stop_id_and_certificate() -> None:
    clock = FakeClock(start_ns=500)
    stop = envelope(
        clock,
        stop_id="cell-stop",
        deadline_ns=700,
        domains=(DOMAIN_A, DOMAIN_B),
    )

    assert result(stop, ControlDomainId("outside")) is EvidenceDecision.WRONG_SCOPE
    settle_domain(stop, DOMAIN_A)
    assert stop.certificate is None
    settle_domain(stop, DOMAIN_B)
    assert stop.certificate is not None
    assert stop.certificate.stopped_domains == (DOMAIN_A, DOMAIN_B)
    assert stop.certificate.measured_rest_domains == (DOMAIN_A, DOMAIN_B)
