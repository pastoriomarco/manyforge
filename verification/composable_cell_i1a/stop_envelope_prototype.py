"""Experimental P2 proof for one Kernel-owned Stop envelope.

This module is deliberately unreachable from production packages.  It tests
candidate timing and evidence rules before the I1b normative selection.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from threading import RLock
from typing import Callable, Protocol

from .contracts import (
    ControlDomainId,
    GoalId,
    ResourceId,
    StopCertificate,
    StopPhaseBudget,
    StopScope,
    StopTransactionId,
    StoppedGoalSnapshot,
)


class MonotonicClock(Protocol):
    """The only time source accepted by the prototype."""

    def now_ns(self) -> int: ...


class StopProfile(str, Enum):
    XARM = "xarm"
    UR = "ur"


class WaitState(str, Enum):
    PENDING = "pending"
    COMPLETED = "completed"
    CANCELLED = "cancelled"
    LATE = "late"


class EvidenceDecision(str, Enum):
    ACCEPTED = "accepted"
    DUPLICATE = "duplicate"
    WRONG_SCOPE = "wrong_scope"
    LATE = "late"
    STALE = "stale"
    OUT_OF_ORDER = "out_of_order"
    MOVING = "moving"
    INVALID = "invalid"
    TERMINAL_IGNORED = "terminal_ignored"


@dataclass(frozen=True, order=True)
class ReceiverSequence:
    """Sequence assigned by the receiving Stop owner, never by a peer clock."""

    value: int

    def __post_init__(self) -> None:
        if self.value < 0:
            raise ValueError("receiver sequence must be non-negative")


@dataclass(frozen=True, order=True)
class EvidenceSequence:
    """Adapter-local ordering shared by result and state evidence."""

    value: int

    def __post_init__(self) -> None:
        if self.value < 0:
            raise ValueError("evidence sequence must be non-negative")


@dataclass(frozen=True, order=True)
class EvidenceAgeNs:
    """Age calculated in the evidence receiver's local clock domain."""

    value: int

    def __post_init__(self) -> None:
        if self.value < 0:
            raise ValueError("evidence age must be non-negative")


@dataclass(frozen=True, order=True)
class StopGeneration:
    value: int

    def __post_init__(self) -> None:
        if self.value < 1:
            raise ValueError("Stop generation must be positive")


@dataclass(frozen=True)
class StopTarget:
    """Exact resources and goals selected in one control domain."""

    domain_id: ControlDomainId
    resource_ids: tuple[ResourceId, ...]
    goals: tuple[StoppedGoalSnapshot, ...]

    def __post_init__(self) -> None:
        if not self.resource_ids or not self.goals:
            raise ValueError("Stop target requires resources and goals")
        if len(set(self.resource_ids)) != len(self.resource_ids):
            raise ValueError("Stop target resources must be unique")
        if any(goal.control_domain_id != self.domain_id for goal in self.goals):
            raise ValueError("Stop target goals must belong to its control domain")
        if len(set(self.goal_ids)) != len(self.goal_ids):
            raise ValueError("Stop target goals must be unique")

    @property
    def goal_ids(self) -> tuple[GoalId, ...]:
        return tuple(goal.goal_id for goal in self.goals)


@dataclass(frozen=True)
class EvidenceRecord:
    decision: EvidenceDecision
    domain_id: ControlDomainId
    evidence_kind: str
    receiver_sequence: ReceiverSequence
    received_monotonic_ns: int
    detail: str


@dataclass(frozen=True)
class ControllerResultEvidence:
    domain_id: ControlDomainId
    goal_id: GoalId
    evidence_sequence: EvidenceSequence
    stop_generation: StopGeneration
    received_monotonic_ns: int


@dataclass(frozen=True)
class MeasuredRestEvidence:
    domain_id: ControlDomainId
    sample_id: str
    resource_ids: tuple[str, ...]
    evidence_sequence: EvidenceSequence
    evidence_age_ns: EvidenceAgeNs
    stop_generation: StopGeneration
    received_monotonic_ns: int


class CancellationSignal:
    """Synchronous cancellation publication with deterministic wake-up."""

    def __init__(self) -> None:
        self._cancelled = False
        self._callbacks: dict[int, Callable[[], None]] = {}
        self._next_subscription = 1

    @property
    def cancelled(self) -> bool:
        return self._cancelled

    def subscribe(self, callback: Callable[[], None]) -> int:
        subscription = self._next_subscription
        self._next_subscription += 1
        if self._cancelled:
            callback()
            return subscription
        self._callbacks[subscription] = callback
        return subscription

    def unsubscribe(self, subscription: int) -> None:
        self._callbacks.pop(subscription, None)

    def cancel(self) -> bool:
        if self._cancelled:
            return False
        self._cancelled = True
        callbacks = tuple(self._callbacks.values())
        self._callbacks.clear()
        for callback in callbacks:
            callback()
        return True


class CancellablePhaseWait:
    """A wait-state proof with no thread, sleep, or wall-clock dependency."""

    def __init__(
        self,
        clock: MonotonicClock,
        budget: StopPhaseBudget,
        cancellation: CancellationSignal,
    ) -> None:
        self._clock = clock
        self.budget = budget
        self.state = WaitState.PENDING
        self.awakened_monotonic_ns: int | None = None
        self._cancellation = cancellation
        self._subscription = 0
        self._subscription = cancellation.subscribe(self._wake_cancelled)

    def _settle(self, state: WaitState, event_monotonic_ns: int) -> bool:
        if self.state is not WaitState.PENDING:
            return False
        self.state = state
        self.awakened_monotonic_ns = event_monotonic_ns
        self._cancellation.unsubscribe(self._subscription)
        return True

    def _wake_cancelled(self) -> None:
        self._settle(WaitState.CANCELLED, self._clock.now_ns())

    def complete(self) -> bool:
        completed_at_ns = self._clock.now_ns()
        state = (
            WaitState.COMPLETED
            if completed_at_ns <= self.budget.absolute_deadline_ns
            else WaitState.LATE
        )
        return self._settle(state, completed_at_ns)

    def observe_deadline(self) -> bool:
        """Expire only after the boundary, leaving equality for evidence."""

        observed_at_ns = self._clock.now_ns()
        if observed_at_ns <= self.budget.absolute_deadline_ns:
            return False
        return self._settle(WaitState.LATE, observed_at_ns)


_PROFILE_WEIGHTS: dict[StopProfile, tuple[tuple[str, int], ...]] = {
    StopProfile.XARM: (
        ("cancel_goal", 2),
        ("vendor_stop", 5),
        ("measured_rest", 3),
    ),
    StopProfile.UR: (
        ("cancel_goal", 3),
        ("controller_deceleration", 4),
        ("measured_rest", 3),
    ),
}


class StopEnvelopePrototype:
    """Aggregates one scoped Stop transaction into one certificate."""

    def __init__(
        self,
        *,
        clock: MonotonicClock,
        stop_id: StopTransactionId,
        scope: StopScope,
        absolute_deadline_ns: int,
        targets: tuple[StopTarget, ...],
        measured_sample_freshness_ns: int,
        stop_generation: StopGeneration = StopGeneration(1),
        requested_monotonic_ns: int | None = None,
        success_outcome: str = "stopped",
        success_detail: str = (
            "controller result followed by fresh measured-rest evidence"
        ),
    ) -> None:
        requested_at_ns = (
            clock.now_ns()
            if requested_monotonic_ns is None
            else requested_monotonic_ns
        )
        if absolute_deadline_ns < requested_at_ns:
            raise ValueError("Stop deadline is already in the past")
        if measured_sample_freshness_ns < 0:
            raise ValueError("measured_sample_freshness_ns must be non-negative")

        if not targets:
            raise ValueError("a Stop envelope requires at least one target")
        deduplicated_domains = tuple(target.domain_id for target in targets)
        if len(set(deduplicated_domains)) != len(deduplicated_domains):
            raise ValueError("Stop targets must have unique domains")
        scoped_domains = set(scope.control_domain_ids)
        if not scoped_domains.issubset(set(deduplicated_domains)):
            raise ValueError("an explicitly scoped domain is missing from the targets")
        targeted_resources = {
            resource_id for target in targets for resource_id in target.resource_ids
        }
        if not set(scope.resource_ids).issubset(targeted_resources):
            raise ValueError("an explicitly scoped resource is missing from the targets")

        self.clock = clock
        self.stop_id = stop_id
        self.scope = scope
        self.targets = targets
        self.stop_generation = stop_generation
        self.requested_monotonic_ns = requested_at_ns
        self.absolute_deadline_ns = absolute_deadline_ns
        self.target_domains = deduplicated_domains
        self.measured_sample_freshness_ns = measured_sample_freshness_ns
        self.success_outcome = success_outcome
        self.success_detail = success_detail

        self._target_by_domain = {
            target.domain_id: target for target in self.targets
        }
        self._controller_results: dict[
            tuple[ControlDomainId, GoalId], ControllerResultEvidence
        ] = {}
        self._measured_rest: dict[ControlDomainId, MeasuredRestEvidence] = {}
        self._seen_sample_ids: set[tuple[ControlDomainId, str]] = set()
        self._evidence_log: list[EvidenceRecord] = []
        self._certificate: StopCertificate | None = None
        self._certificates: list[StopCertificate] = []
        self._lock = RLock()
        self._receiver_sequence = 0
        self._terminal_publish_attempts = 0

    @property
    def evidence_log(self) -> tuple[EvidenceRecord, ...]:
        with self._lock:
            return tuple(self._evidence_log)

    @property
    def certificate(self) -> StopCertificate | None:
        with self._lock:
            return self._certificate

    @property
    def certificates(self) -> tuple[StopCertificate, ...]:
        with self._lock:
            return tuple(self._certificates)

    @property
    def terminal_publish_attempts(self) -> int:
        with self._lock:
            return self._terminal_publish_attempts

    def profile_budgets(self, profile: StopProfile) -> tuple[StopPhaseBudget, ...]:
        """Derive every phase boundary from the one envelope deadline."""

        phases = _PROFILE_WEIGHTS[profile]
        total_weight = sum(weight for _, weight in phases)
        envelope_ns = self.absolute_deadline_ns - self.requested_monotonic_ns
        cumulative_weight = 0
        budgets: list[StopPhaseBudget] = []
        for index, (name, weight) in enumerate(phases):
            cumulative_weight += weight
            phase_deadline = self.requested_monotonic_ns + (
                envelope_ns * cumulative_weight // total_weight
            )
            if index == len(phases) - 1:
                phase_deadline = self.absolute_deadline_ns
            budgets.append(
                StopPhaseBudget(
                    name=f"{profile.value}.{name}",
                    absolute_deadline_ns=phase_deadline,
                )
            )
        return tuple(budgets)

    def nested_budget(
        self,
        name: str,
        duration_ns: int,
        *,
        parent: StopPhaseBudget | None = None,
    ) -> StopPhaseBudget:
        if duration_ns < 0:
            raise ValueError("duration_ns must be non-negative")
        outer_deadline = (
            parent.absolute_deadline_ns
            if parent is not None
            else self.absolute_deadline_ns
        )
        return StopPhaseBudget(
            name=name,
            absolute_deadline_ns=min(
                self.clock.now_ns() + duration_ns,
                outer_deadline,
                self.absolute_deadline_ns,
            ),
        )

    def _record_decision(
        self,
        decision: EvidenceDecision,
        domain_id: ControlDomainId,
        evidence_kind: str,
        detail: str,
        received_monotonic_ns: int,
    ) -> EvidenceDecision:
        self._receiver_sequence += 1
        self._evidence_log.append(
            EvidenceRecord(
                decision=decision,
                domain_id=domain_id,
                evidence_kind=evidence_kind,
                receiver_sequence=ReceiverSequence(self._receiver_sequence),
                received_monotonic_ns=received_monotonic_ns,
                detail=detail,
            )
        )
        return decision

    def _common_evidence_rejection(
        self,
        domain_id: ControlDomainId,
        evidence_kind: str,
        received_monotonic_ns: int,
        stop_generation: StopGeneration,
    ) -> EvidenceDecision | None:
        # A terminal certificate freezes *all* evidence state, including the
        # audit log.  Callers still receive a deterministic result, but a late
        # delivery cannot mutate an already terminal transaction.
        if self._certificate is not None:
            return EvidenceDecision.TERMINAL_IGNORED
        if domain_id not in self.target_domains:
            return self._record_decision(
                EvidenceDecision.WRONG_SCOPE,
                domain_id,
                evidence_kind,
                "domain is outside this Stop scope",
                received_monotonic_ns,
            )
        if stop_generation != self.stop_generation:
            return self._record_decision(
                EvidenceDecision.STALE,
                domain_id,
                evidence_kind,
                "evidence belongs to a different Stop generation",
                received_monotonic_ns,
            )
        if received_monotonic_ns > self.absolute_deadline_ns:
            return self._record_decision(
                EvidenceDecision.LATE,
                domain_id,
                evidence_kind,
                "evidence arrived after the absolute Stop deadline",
                received_monotonic_ns,
            )
        return None

    def record_controller_result(
        self,
        *,
        domain_id: ControlDomainId,
        goal_id: GoalId,
        evidence_sequence: EvidenceSequence,
        stop_generation: StopGeneration,
    ) -> EvidenceDecision:
        with self._lock:
            received_at_ns = self.clock.now_ns()
            rejected = self._common_evidence_rejection(
                domain_id,
                "controller_result",
                received_at_ns,
                stop_generation,
            )
            if rejected is not None:
                return rejected
            target = self._target_by_domain[domain_id]
            if goal_id not in target.goal_ids:
                return self._record_decision(
                    EvidenceDecision.WRONG_SCOPE,
                    domain_id,
                    "controller_result",
                    "goal is outside the exact Stop selection",
                    received_at_ns,
                )

            key = (domain_id, goal_id)
            existing = self._controller_results.get(key)
            if existing is not None:
                decision = (
                    EvidenceDecision.DUPLICATE
                    if existing.evidence_sequence == evidence_sequence
                    else EvidenceDecision.INVALID
                )
                return self._record_decision(
                    decision,
                    domain_id,
                    "controller_result",
                    (
                        "duplicate result"
                        if decision is EvidenceDecision.DUPLICATE
                        else "conflicting result sequence for the selected goal"
                    ),
                    received_at_ns,
                )

            self._controller_results[key] = ControllerResultEvidence(
                domain_id=domain_id,
                goal_id=goal_id,
                evidence_sequence=evidence_sequence,
                stop_generation=stop_generation,
                received_monotonic_ns=received_at_ns,
            )
            return self._record_decision(
                EvidenceDecision.ACCEPTED,
                domain_id,
                "controller_result",
                "selected goal result accepted; measured rest still required",
                received_at_ns,
            )

    def record_measured_rest(
        self,
        *,
        domain_id: ControlDomainId,
        sample_id: str,
        resource_ids: tuple[str, ...],
        evidence_sequence: EvidenceSequence,
        evidence_age_ns: EvidenceAgeNs,
        stop_generation: StopGeneration,
        at_rest: bool,
        complete: bool = True,
    ) -> EvidenceDecision:
        with self._lock:
            received_at_ns = self.clock.now_ns()
            rejected = self._common_evidence_rejection(
                domain_id,
                "measured_rest",
                received_at_ns,
                stop_generation,
            )
            if rejected is not None:
                return rejected
            target = self._target_by_domain[domain_id]
            if tuple(resource_ids) != target.resource_ids:
                return self._record_decision(
                    EvidenceDecision.WRONG_SCOPE,
                    domain_id,
                    "measured_rest",
                    "sample resources do not exactly match the Stop selection",
                    received_at_ns,
                )
            sample_key = (domain_id, sample_id)
            if sample_key in self._seen_sample_ids:
                return self._record_decision(
                    EvidenceDecision.DUPLICATE,
                    domain_id,
                    "measured_rest",
                    "sample_id was already observed",
                    received_at_ns,
                )
            self._seen_sample_ids.add(sample_key)

            if not complete:
                return self._record_decision(
                    EvidenceDecision.INVALID,
                    domain_id,
                    "measured_rest",
                    "sample is incomplete",
                    received_at_ns,
                )
            results = tuple(
                self._controller_results.get((domain_id, goal_id))
                for goal_id in target.goal_ids
            )
            if any(result is None for result in results):
                return self._record_decision(
                    EvidenceDecision.OUT_OF_ORDER,
                    domain_id,
                    "measured_rest",
                    "all selected goal results must arrive before measured rest",
                    received_at_ns,
                )
            if evidence_age_ns.value > self.measured_sample_freshness_ns:
                return self._record_decision(
                    EvidenceDecision.STALE,
                    domain_id,
                    "measured_rest",
                    "receiver-local evidence age exceeds the freshness budget",
                    received_at_ns,
                )
            result_sequences = tuple(
                result.evidence_sequence.value
                for result in results
                if result is not None
            )
            if (
                result_sequences
                and evidence_sequence.value <= max(result_sequences)
            ):
                return self._record_decision(
                    EvidenceDecision.OUT_OF_ORDER,
                    domain_id,
                    "measured_rest",
                    "sample sequence must follow every selected goal result",
                    received_at_ns,
                )
            if not at_rest:
                return self._record_decision(
                    EvidenceDecision.MOVING,
                    domain_id,
                    "measured_rest",
                    "fresh measured state is not at rest",
                    received_at_ns,
                )

            self._measured_rest[domain_id] = MeasuredRestEvidence(
                domain_id=domain_id,
                sample_id=sample_id,
                resource_ids=tuple(resource_ids),
                evidence_sequence=evidence_sequence,
                evidence_age_ns=evidence_age_ns,
                stop_generation=stop_generation,
                received_monotonic_ns=received_at_ns,
            )
            decision = self._record_decision(
                EvidenceDecision.ACCEPTED,
                domain_id,
                "measured_rest",
                "fresh measured-rest evidence accepted",
                received_at_ns,
            )
            if set(self._measured_rest) == set(self.target_domains):
                self._publish_terminal_locked(
                    outcome=self.success_outcome,
                    detail=self.success_detail,
                    completed_monotonic_ns=received_at_ns,
                )
            return decision

    def publish_terminal(
        self,
        *,
        outcome: str,
        detail: str,
        completed_monotonic_ns: int | None = None,
    ) -> StopCertificate:
        """Publish at most one immutable certificate; the first owner wins."""

        with self._lock:
            if self._certificate is not None:
                return self._certificate
            if completed_monotonic_ns is None:
                completed_monotonic_ns = self.clock.now_ns()
            return self._publish_terminal_locked(
                outcome=outcome,
                detail=detail,
                completed_monotonic_ns=completed_monotonic_ns,
            )

    def _publish_terminal_locked(
        self,
        *,
        outcome: str,
        detail: str,
        completed_monotonic_ns: int,
    ) -> StopCertificate:
        if self._certificate is not None:
            return self._certificate
        self._terminal_publish_attempts += 1
        if (
            outcome == self.success_outcome
            and set(self._measured_rest) != set(self.target_domains)
        ):
            raise ValueError(
                "a successful Stop certificate requires measured rest "
                "for every target domain"
            )
        stopped_targets = tuple(
            target
            for target in self.targets
            if all(
                (target.domain_id, goal_id) in self._controller_results
                for goal_id in target.goal_ids
            )
            and target.domain_id in self._measured_rest
        )
        certificate = StopCertificate(
            stop_id=self.stop_id,
            outcome=outcome,
            completed_monotonic_ns=completed_monotonic_ns,
            stopped_domains=tuple(
                target.domain_id for target in stopped_targets
            ),
            stopped_resources=tuple(
                resource_id
                for target in stopped_targets
                for resource_id in target.resource_ids
            ),
            stopped_goals=tuple(
                goal for target in stopped_targets for goal in target.goals
            ),
            measured_rest_domains=tuple(
                domain_id
                for domain_id in self.target_domains
                if domain_id in self._measured_rest
            ),
            detail=detail,
        )
        self._certificate = certificate
        self._certificates.append(certificate)
        return certificate

    def fail(self, *, outcome: str, detail: str) -> StopCertificate:
        return self.publish_terminal(outcome=outcome, detail=detail)

    def observe_deadline(self) -> StopCertificate | None:
        """The equal instant remains open; only a strictly late poll expires."""

        with self._lock:
            if self._certificate is not None:
                return self._certificate
            observed_at_ns = self.clock.now_ns()
            if observed_at_ns <= self.absolute_deadline_ns:
                return None
            return self._publish_terminal_locked(
                outcome="deadline_exceeded",
                detail=(
                    "fresh measured-rest settlement did not beat "
                    "the Stop deadline"
                ),
                completed_monotonic_ns=observed_at_ns,
            )
