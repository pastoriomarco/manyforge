"""Experimental P5 proof for two independent fake control domains.

The coordinator is synchronous and ROS-free.  It is selection evidence only;
production code must not import or promote it as a compatibility surface.
"""

from __future__ import annotations

import resource
import hashlib
import threading
import time
from dataclasses import dataclass, field, replace
from enum import Enum
from functools import wraps
from threading import RLock
from typing import Callable, ParamSpec, TypeVar

from .contracts import (
    ControlBindingId,
    ControlDomainId,
    GoalId,
    GraphRevision,
    OperationId,
    OperationParticipant,
    OperationRequest,
    OperationStatus,
    RequestId,
    RequestMetadata,
    ResourceId,
    RouteId,
    RouteGeneration,
    StopCertificate,
    StopRequest,
    StopScope,
    StopTransactionId,
    StoppedGoalSnapshot,
    canonical_json,
)
from .stop_envelope_prototype import (
    EvidenceAgeNs,
    EvidenceSequence,
    MonotonicClock,
    StopEnvelopePrototype,
    StopGeneration,
    StopTarget,
)


_P = ParamSpec("_P")
_R = TypeVar("_R")


def _serialized(method: Callable[_P, _R]) -> Callable[_P, _R]:
    """Serialize every coordinator mutation through its one re-entrant lock."""

    @wraps(method)
    def wrapper(*args: _P.args, **kwargs: _P.kwargs) -> _R:
        coordinator = args[0]
        with coordinator._lock:
            return method(*args, **kwargs)

    return wrapper


class RouteKind(str, Enum):
    DIRECT = "direct"
    CHAINED = "chained"


class CallbackDecision(str, Enum):
    ACCEPTED = "accepted"
    STALE_OPERATION = "stale_operation"
    STALE_GOAL = "stale_goal"
    STALE_ROUTE = "stale_route"
    DUPLICATE = "duplicate"
    WRONG_DOMAIN = "wrong_domain"
    INVALID = "invalid"


@dataclass(frozen=True)
class ControllerRoute:
    route_id: str
    kind: RouteKind
    controllers: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.route_id:
            raise ValueError("route_id is required")
        if self.kind is RouteKind.DIRECT and len(self.controllers) != 1:
            raise ValueError("a direct route has exactly one controller")
        if self.kind is RouteKind.CHAINED and len(self.controllers) < 2:
            raise ValueError("a chained route has at least two controllers")
        if len(set(self.controllers)) != len(self.controllers):
            raise ValueError("route controllers must be unique")


@dataclass(frozen=True)
class DomainCall:
    action: str
    operation_ids: tuple[OperationId, ...]
    route_generation: RouteGeneration
    stop_id: StopTransactionId | None = None


@dataclass(frozen=True)
class TraceEvent:
    sequence: int
    action: str
    domain_id: ControlDomainId | None
    operation_id: OperationId | None


@dataclass(frozen=True)
class PartialDispatchEvidence:
    operation_id: OperationId
    failed_binding_id: ControlBindingId
    dispatched_bindings: tuple[ControlBindingId, ...]
    stop_id: StopTransactionId
    stop_outcome: str
    stopped_domains: tuple[ControlDomainId, ...]
    physical_rollback_claimed: bool = False


@dataclass(frozen=True)
class ControllerLossEvidence:
    domain_id: ControlDomainId
    operation_id: OperationId
    stop_id: StopTransactionId | None
    stopped_surviving_domains: tuple[ControlDomainId, ...]
    fenced_resources: tuple[ResourceId, ...]


@dataclass(frozen=True)
class HostLossEvidence:
    active_operation_ids: tuple[OperationId, ...]
    domain_mutations_before: int
    domain_mutations_after: int
    status_only: bool


@dataclass(frozen=True)
class IsolationMeasurement:
    rt_ticks: tuple[tuple[ControlDomainId, int], ...]
    nrt_calls: tuple[tuple[ControlDomainId, int], ...]
    nrt_calls_from_rt: tuple[tuple[ControlDomainId, int], ...]
    active_resource_locks: int
    fenced_resources: int
    process_threads: int
    max_rss_kib: int
    fake_dds_entities: int


@dataclass(frozen=True)
class RtPathMeasurement:
    """Informational process-local timing for the fake RT-path boundary."""

    samples: int
    elapsed_ns: int
    process_cpu_ns: int
    p50_ns: int
    p95_ns: int
    max_ns: int
    p95_jitter_from_median_ns: int
    process_load_ratio: float
    threads_before: int
    threads_after: int
    max_rss_kib: int
    nrt_calls_from_rt: int
    fake_dds_entities_created: int


@dataclass(frozen=True)
class RetainedControlBinding:
    """Minimal retained registry row used to validate operation participants."""

    participant: OperationParticipant

    def __post_init__(self) -> None:
        if not isinstance(self.participant, OperationParticipant):
            raise ValueError("retained binding requires an operation participant")

    @property
    def control_binding_id(self) -> ControlBindingId:
        return self.participant.control_binding_id

    @property
    def control_domain_id(self) -> ControlDomainId:
        return self.participant.control_domain_id

    @property
    def resource_ids(self) -> tuple[ResourceId, ...]:
        return self.participant.resource_ids


@dataclass(frozen=True)
class _StopReplay:
    request: StopRequest
    request_digest: str
    certificate: StopCertificate
    full_scope: bool


class StopReplayConflict(ValueError):
    pass


class DomainActionError(RuntimeError):
    pass


class FakeControlDomain:
    """One synchronous fake controller-manager/control failure domain."""

    def __init__(
        self,
        *,
        domain_id: ControlDomainId,
        owned_resources: tuple[ResourceId, ...],
        routes: tuple[ControllerRoute, ...],
        initial_route_id: str,
    ) -> None:
        if not owned_resources or len(set(owned_resources)) != len(owned_resources):
            raise ValueError("owned_resources must be non-empty and unique")
        route_map = {route.route_id: route for route in routes}
        if len(route_map) != len(routes) or initial_route_id not in route_map:
            raise ValueError("routes must be unique and include the initial route")

        self.domain_id = domain_id
        self.owned_resources = frozenset(owned_resources)
        self.routes = route_map
        self.active_route_id = initial_route_id
        self._route_generation_value = 1
        self.controller_available = True
        self.faulted = False

        self.calls: list[DomainCall] = []
        self.route_transition_log: list[tuple[str, str]] = []
        self.prepare_attempts: list[OperationId] = []
        self.dispatch_attempts: list[OperationId] = []
        self.dispatch_log: list[OperationId] = []
        self.stop_log: list[DomainCall] = []
        self._prepared: dict[
            tuple[OperationId, ControlBindingId], OperationParticipant
        ] = {}
        self._goals: dict[tuple[OperationId, ControlBindingId], GoalId] = {}
        self._goal_sequence = 0

        self._fail_prepare: set[OperationId] = set()
        self._fail_dispatch: set[OperationId] = set()
        self._fail_stop_ids: set[StopTransactionId] = set()
        self.rt_tick_count = 0
        self.nrt_call_count = 0
        self.nrt_calls_from_rt = 0
        self._inside_rt_tick = False
        self.mutation_count = 0

    @property
    def active_route(self) -> ControllerRoute:
        return self.routes[self.active_route_id]

    @property
    def route_generation(self) -> RouteGeneration:
        return RouteGeneration(self._route_generation_value)

    def fail_prepare_for(self, operation_id: OperationId) -> None:
        self._fail_prepare.add(operation_id)

    def fail_dispatch_for(self, operation_id: OperationId) -> None:
        self._fail_dispatch.add(operation_id)

    def fail_stop_for(self, stop_id: StopTransactionId) -> None:
        self._fail_stop_ids.add(stop_id)

    def _record_nrt(self) -> None:
        self.nrt_call_count += 1
        if self._inside_rt_tick:
            self.nrt_calls_from_rt += 1

    def rt_tick(self) -> int:
        """Represent the controller update lane without calling NRT work."""

        self._inside_rt_tick = True
        try:
            self.rt_tick_count += 1
            return len(self._goals)
        finally:
            self._inside_rt_tick = False

    def prepare(
        self,
        request: OperationRequest,
        participant: OperationParticipant,
    ) -> None:
        self._record_nrt()
        self.prepare_attempts.append(request.operation_id)
        self.calls.append(
            DomainCall(
                action="prepare",
                operation_ids=(request.operation_id,),
                route_generation=self.route_generation,
            )
        )
        if not self.controller_available or self.faulted:
            raise DomainActionError("controller domain is unavailable")
        if not set(participant.resource_ids).issubset(self.owned_resources):
            raise DomainActionError("participant requests an unowned resource")
        if request.operation_id in self._fail_prepare:
            self._fail_prepare.remove(request.operation_id)
            raise DomainActionError("injected prepare failure")
        self._prepared[
            (request.operation_id, participant.control_binding_id)
        ] = participant

    def abort_prepare(self, operation_id: OperationId) -> None:
        self._record_nrt()
        for key in tuple(self._prepared):
            if key[0] == operation_id:
                self._prepared.pop(key, None)
        self.calls.append(
            DomainCall(
                action="abort_prepare",
                operation_ids=(operation_id,),
                route_generation=self.route_generation,
            )
        )

    def dispatch(
        self,
        operation_id: OperationId,
        control_binding_id: ControlBindingId,
    ) -> GoalId:
        self._record_nrt()
        self.dispatch_attempts.append(operation_id)
        self.calls.append(
            DomainCall(
                action="dispatch_attempt",
                operation_ids=(operation_id,),
                route_generation=self.route_generation,
            )
        )
        if not self.controller_available or self.faulted:
            raise DomainActionError("controller domain is unavailable")
        key = (operation_id, control_binding_id)
        if key not in self._prepared:
            raise DomainActionError("operation was not prepared")
        if operation_id in self._fail_dispatch:
            self._fail_dispatch.remove(operation_id)
            raise DomainActionError("injected dispatch failure")

        self._goal_sequence += 1
        goal_id = GoalId(f"{self.domain_id}.goal.{self._goal_sequence}")
        self._goals[key] = goal_id
        self.dispatch_log.append(operation_id)
        self.mutation_count += 1
        self.calls.append(
            DomainCall(
                action="dispatch",
                operation_ids=(operation_id,),
                route_generation=self.route_generation,
            )
        )
        return goal_id

    def stop(
        self,
        *,
        stop_id: StopTransactionId,
        operation_ids: tuple[OperationId, ...],
    ) -> None:
        self._record_nrt()
        call = DomainCall(
            action="stop",
            operation_ids=operation_ids,
            route_generation=self.route_generation,
            stop_id=stop_id,
        )
        self.calls.append(call)
        if not self.controller_available:
            raise DomainActionError("controller domain is lost")
        if stop_id in self._fail_stop_ids:
            self._fail_stop_ids.remove(stop_id)
            raise DomainActionError("injected stop failure")
        for operation_id in operation_ids:
            for key in tuple(self._goals):
                if key[0] == operation_id:
                    self._goals.pop(key, None)
            for key in tuple(self._prepared):
                if key[0] == operation_id:
                    self._prepared.pop(key, None)
        self.stop_log.append(call)
        self.mutation_count += 1

    def mark_completed(
        self,
        operation_id: OperationId,
        control_binding_id: ControlBindingId,
    ) -> None:
        self._record_nrt()
        key = (operation_id, control_binding_id)
        self._goals.pop(key, None)
        self._prepared.pop(key, None)

    def switch_route(self, route_id: str) -> RouteGeneration:
        self._record_nrt()
        if route_id not in self.routes:
            raise DomainActionError(f"unknown route: {route_id}")
        if route_id == self.active_route_id:
            return self.route_generation

        for controller in reversed(self.active_route.controllers):
            self.route_transition_log.append(("deactivate", controller))
        for controller in self.routes[route_id].controllers:
            self.route_transition_log.append(("activate", controller))
        self.active_route_id = route_id
        self._route_generation_value += 1
        self.mutation_count += 1
        self.calls.append(
            DomainCall(
                action="switch_route",
                operation_ids=(),
                route_generation=self.route_generation,
            )
        )
        return self.route_generation

    def lose_controller(self) -> None:
        self._record_nrt()
        self.controller_available = False


@dataclass
class _ParticipantState:
    participant: OperationParticipant
    route_generation: RouteGeneration
    goal_id: GoalId | None = None
    phase: str = "locked"


@dataclass
class _OperationRecord:
    request: OperationRequest
    participants: list[_ParticipantState]
    phase: str
    terminal_detail: str = ""
    stop_certificate: StopCertificate | None = None
    accepted_callbacks: set[GoalId] = field(default_factory=set)
    settlement_phase: str | None = None

    def status(self) -> OperationStatus:
        return OperationStatus(
            operation_id=self.request.operation_id,
            phase=self.phase,
            dispatched_bindings=tuple(
                state.participant.control_binding_id
                for state in self.participants
                if state.goal_id is not None
            ),
            terminal_detail=self.terminal_detail,
        )


class TwoDomainCoordinatorPrototype:
    """Coordinates exactly two fakes while retaining exact resource locks."""

    _ACTIVE_PHASES = {
        "executing",
        "partially_stopped",
        "stop_failed",
        "partial_dispatch_stop_failed",
        "controller_lost_stop_failed",
        "failed_stop_failed",
        "route_changed_stop_failed",
    }

    def __init__(
        self,
        *,
        clock: MonotonicClock,
        graph_revision: GraphRevision,
        domains: tuple[FakeControlDomain, FakeControlDomain],
        binding_registry: tuple[RetainedControlBinding, ...],
        stop_budget_ns: int = 1_000_000_000,
        measured_sample_freshness_ns: int = 50_000_000,
    ) -> None:
        domain_map = {domain.domain_id: domain for domain in domains}
        if len(domain_map) != 2:
            raise ValueError("P5 requires exactly two distinct control domains")
        if stop_budget_ns <= 0 or measured_sample_freshness_ns < 0:
            raise ValueError("invalid Stop timing budget")
        bindings = {
            binding.control_binding_id: binding for binding in binding_registry
        }
        if len(bindings) != len(binding_registry):
            raise ValueError("retained binding IDs must be unique")
        if not bindings:
            raise ValueError("a retained binding registry is required")
        for binding in bindings.values():
            domain = domain_map.get(binding.control_domain_id)
            if domain is None:
                raise ValueError("retained binding references an unknown domain")
            if not set(binding.resource_ids).issubset(domain.owned_resources):
                raise ValueError("retained binding references an unowned resource")

        self.clock = clock
        self.graph_revision = graph_revision
        self.domains = domain_map
        self.binding_registry = bindings
        self.stop_budget_ns = stop_budget_ns
        self.measured_sample_freshness_ns = measured_sample_freshness_ns
        self._lock = RLock()
        self._operations: dict[OperationId, _OperationRecord] = {}
        self._resource_locks: dict[ResourceId, OperationId] = {}
        self._fenced_resources: set[ResourceId] = set()
        self._stop_replays: dict[StopTransactionId, _StopReplay] = {}
        self._pending_full_stop_handoffs: set[StopTransactionId] = set()
        self._stop_generation = 0
        self._trace: list[TraceEvent] = []
        self._trace_sequence = 0
        self.partial_dispatch_evidence: list[PartialDispatchEvidence] = []
        self.host_available = True

    @property
    @_serialized
    def trace(self) -> tuple[TraceEvent, ...]:
        return tuple(self._trace)

    @property
    @_serialized
    def active_resource_locks(self) -> dict[ResourceId, OperationId]:
        return dict(self._resource_locks)

    @property
    @_serialized
    def fenced_resources(self) -> frozenset[ResourceId]:
        return frozenset(self._fenced_resources)

    @property
    @_serialized
    def pending_full_stop_handoffs(self) -> frozenset[StopTransactionId]:
        return frozenset(self._pending_full_stop_handoffs)

    def _record_trace(
        self,
        action: str,
        *,
        domain_id: ControlDomainId | None = None,
        operation_id: OperationId | None = None,
    ) -> None:
        self._trace_sequence += 1
        self._trace.append(
            TraceEvent(
                sequence=self._trace_sequence,
                action=action,
                domain_id=domain_id,
                operation_id=operation_id,
            )
        )

    def _rejected(self, request: OperationRequest, detail: str) -> OperationStatus:
        return OperationStatus(
            operation_id=request.operation_id,
            phase="rejected",
            terminal_detail=detail,
        )

    @_serialized
    def start(self, request: OperationRequest) -> OperationStatus:
        if not self.host_available:
            return self._rejected(request, "execution_host_unavailable")
        if self._pending_full_stop_handoffs:
            return self._rejected(request, "full_stop_settlement_pending")
        if request.expected_graph_revision != self.graph_revision:
            return self._rejected(request, "stale_graph_revision")
        if request.operation_id in self._operations:
            return self._rejected(request, "duplicate_operation_id")
        if not request.participants:
            return self._rejected(request, "participants_required")
        if request.coordinated and len(request.participants) < 2:
            return self._rejected(
                request, "coordinated_operation_requires_two_participants"
            )
        if not request.coordinated and len(request.participants) != 1:
            return self._rejected(request, "independent_operation_has_one_participant")

        requested_resources = [
            resource_id
            for participant in request.participants
            for resource_id in participant.resource_ids
        ]
        if len(set(requested_resources)) != len(requested_resources):
            return self._rejected(request, "overlapping_participants")
        conflict = next(
            (
                resource_id
                for resource_id in requested_resources
                if resource_id in self._resource_locks
                or resource_id in self._fenced_resources
            ),
            None,
        )
        if conflict is not None:
            return self._rejected(request, f"resource_overlap:{conflict}")

        states: list[_ParticipantState] = []
        for participant in request.participants:
            domain = self.domains.get(participant.control_domain_id)
            if domain is None:
                return self._rejected(request, "unknown_control_domain")
            retained = self.binding_registry.get(participant.control_binding_id)
            if retained is None:
                return self._rejected(request, "unknown_control_binding")
            if participant != retained.participant:
                if participant.binding_revision != retained.participant.binding_revision:
                    return self._rejected(request, "stale_binding_revision")
                if (
                    participant.control_domain_id
                    != retained.participant.control_domain_id
                ):
                    return self._rejected(request, "binding_domain_mismatch")
                if participant.resource_ids != retained.participant.resource_ids:
                    return self._rejected(request, "binding_resource_mismatch")
                return self._rejected(request, "stale_binding_snapshot")
            states.append(
                _ParticipantState(
                    participant=participant,
                    route_generation=domain.route_generation,
                )
            )

        for resource_id in requested_resources:
            self._resource_locks[resource_id] = request.operation_id
        record = _OperationRecord(
            request=request,
            participants=states,
            phase="preparing",
        )
        self._operations[request.operation_id] = record

        prepared: list[_ParticipantState] = []
        try:
            for state in states:
                domain = self.domains[state.participant.control_domain_id]
                self._record_trace(
                    "prepare_attempt",
                    domain_id=domain.domain_id,
                    operation_id=request.operation_id,
                )
                domain.prepare(request, state.participant)
                state.phase = "prepared"
                prepared.append(state)
                self._record_trace(
                    "prepare_ok",
                    domain_id=domain.domain_id,
                    operation_id=request.operation_id,
                )
        except DomainActionError as error:
            for state in prepared:
                domain = self.domains[state.participant.control_domain_id]
                domain.abort_prepare(request.operation_id)
                state.phase = "aborted"
            self._release_operation_resources(record)
            record.phase = "failed_pre_dispatch"
            record.terminal_detail = str(error)
            self._record_trace(
                "pre_dispatch_failed", operation_id=request.operation_id
            )
            return record.status()

        dispatched: list[_ParticipantState] = []
        for state in states:
            domain = self.domains[state.participant.control_domain_id]
            self._record_trace(
                "dispatch_attempt",
                domain_id=domain.domain_id,
                operation_id=request.operation_id,
            )
            try:
                state.goal_id = domain.dispatch(
                    request.operation_id,
                    state.participant.control_binding_id,
                )
            except DomainActionError as error:
                state.phase = "dispatch_failed"
                for undispatched in states:
                    if undispatched.goal_id is None:
                        owner = self.domains[
                            undispatched.participant.control_domain_id
                        ]
                        owner.abort_prepare(request.operation_id)
                if not dispatched:
                    self._release_operation_resources(record)
                    record.phase = "failed_pre_dispatch"
                    record.terminal_detail = str(error)
                    self._record_trace(
                        "zero_dispatch_failed", operation_id=request.operation_id
                    )
                    return record.status()
                return self._settle_partial_dispatch(
                    record=record,
                    dispatched=dispatched,
                    failed=state,
                    detail=str(error),
                )
            state.phase = "dispatched"
            dispatched.append(state)
            self._record_trace(
                "dispatch_ok",
                domain_id=domain.domain_id,
                operation_id=request.operation_id,
            )

        record.phase = "executing"
        return record.status()

    def _settle_partial_dispatch(
        self,
        *,
        record: _OperationRecord,
        dispatched: list[_ParticipantState],
        failed: _ParticipantState,
        detail: str,
    ) -> OperationStatus:
        stop_id = StopTransactionId(
            f"partial-dispatch-stop:{record.request.operation_id}"
        )
        stop_request = StopRequest(
            metadata=RequestMetadata(
                request_id=RequestId(
                    f"partial-dispatch-request:{record.request.operation_id}"
                ),
                source_label="p5-coordinator",
                transport="in_process",
                submitted_monotonic_ns=self.clock.now_ns(),
            ),
            stop_id=stop_id,
            scope=StopScope(operation_ids=(record.request.operation_id,)),
            absolute_deadline_ns=self.clock.now_ns() + self.stop_budget_ns,
        )
        certificate, stopped_domains = self._stop_selection(
            stop_request,
            [(record, state) for state in dispatched],
            success_outcome="stopped_after_partial_dispatch",
            success_detail="stopped exactly the physically dispatched subset",
        )
        stopped_set = set(stopped_domains)
        for state in record.participants:
            domain_id = state.participant.control_domain_id
            if state in dispatched and domain_id in stopped_set:
                state.phase = "stopped"
                self._release_participant_resources(record, state)
            elif state in dispatched:
                state.phase = "stop_failed"
                self._fence_participant_resources(state)
            else:
                self._release_participant_resources(record, state)

        record.settlement_phase = "partial_dispatch"
        record.phase = (
            "partial_dispatch"
            if certificate.outcome == "stopped_after_partial_dispatch"
            else "partial_dispatch_stop_failed"
        )
        record.stop_certificate = certificate
        record.terminal_detail = (
            f"{detail}; stopped dispatched subset; no physical rollback claimed"
            if record.phase == "partial_dispatch"
            else f"{detail}; dispatched subset Stop remains uncertified; "
            "no physical rollback claimed"
        )
        evidence = PartialDispatchEvidence(
            operation_id=record.request.operation_id,
            failed_binding_id=failed.participant.control_binding_id,
            dispatched_bindings=tuple(
                state.participant.control_binding_id for state in dispatched
            ),
            stop_id=stop_id,
            stop_outcome=certificate.outcome,
            stopped_domains=certificate.stopped_domains,
        )
        self.partial_dispatch_evidence.append(evidence)
        self._record_trace(
            "partial_dispatch_recorded", operation_id=record.request.operation_id
        )
        return record.status()

    def _release_participant_resources(
        self,
        record: _OperationRecord,
        state: _ParticipantState,
    ) -> None:
        for resource_id in state.participant.resource_ids:
            if self._resource_locks.get(resource_id) == record.request.operation_id:
                del self._resource_locks[resource_id]

    def _release_operation_resources(self, record: _OperationRecord) -> None:
        for state in record.participants:
            self._release_participant_resources(record, state)

    def _fence_participant_resources(self, state: _ParticipantState) -> None:
        self._fenced_resources.update(state.participant.resource_ids)

    def _unfence_participant_resources(self, state: _ParticipantState) -> None:
        for resource_id in state.participant.resource_ids:
            self._fenced_resources.discard(resource_id)

    @_serialized
    def operation_status(self, operation_id: OperationId) -> OperationStatus | None:
        record = self._operations.get(operation_id)
        return None if record is None else record.status()

    @_serialized
    def participant_goal(
        self,
        operation_id: OperationId,
        domain_id: ControlDomainId,
        control_binding_id: ControlBindingId | None = None,
    ) -> tuple[GoalId, RouteGeneration]:
        record = self._operations[operation_id]
        matches = [
            state
            for state in record.participants
            if state.participant.control_domain_id == domain_id
            and (
                control_binding_id is None
                or state.participant.control_binding_id == control_binding_id
            )
        ]
        if not matches:
            raise KeyError((domain_id, control_binding_id))
        if len(matches) != 1:
            raise ValueError("binding ID is required for a multi-binding domain")
        state = matches[0]
        if state.goal_id is None:
            raise ValueError("participant was not dispatched")
        return state.goal_id, state.route_generation

    def _selected_participants(
        self, scope: StopScope
    ) -> list[tuple[_OperationRecord, _ParticipantState]]:
        operation_ids = set(scope.operation_ids)
        domain_ids = set(scope.control_domain_ids)
        resource_ids = set(scope.resource_ids)
        selected: list[tuple[_OperationRecord, _ParticipantState]] = []
        for record in self._operations.values():
            if record.phase not in self._ACTIVE_PHASES:
                continue
            for state in record.participants:
                if state.phase not in {"dispatched", "stop_failed"}:
                    continue
                if (
                    record.request.operation_id in operation_ids
                    or state.participant.control_domain_id in domain_ids
                    or bool(
                        set(state.participant.resource_ids) & resource_ids
                    )
                ):
                    selected.append((record, state))
        return selected

    def _stop_digest(self, request: StopRequest, *, full_scope: bool) -> str:
        encoded = f"{canonical_json(request)}|full_scope={full_scope}".encode()
        return hashlib.sha256(encoded).hexdigest()

    def _existing_stop(
        self,
        request: StopRequest,
        *,
        full_scope: bool,
    ) -> StopCertificate | None:
        replay = self._stop_replays.get(request.stop_id)
        if replay is None:
            return None
        if replay.request_digest != self._stop_digest(
            request, full_scope=full_scope
        ):
            raise StopReplayConflict(
                "Stop ID was reused with different scope, deadline, or metadata"
            )
        return replay.certificate

    def _remember_stop(
        self,
        request: StopRequest,
        certificate: StopCertificate,
        *,
        full_scope: bool,
    ) -> None:
        self._stop_replays[request.stop_id] = _StopReplay(
            request=request,
            request_digest=self._stop_digest(request, full_scope=full_scope),
            certificate=certificate,
            full_scope=full_scope,
        )

    @_serialized
    def stop(self, request: StopRequest) -> StopCertificate:
        return self._stop(request, full_scope=False)

    def _stop(
        self,
        request: StopRequest,
        *,
        full_scope: bool,
    ) -> StopCertificate:
        existing = self._existing_stop(request, full_scope=full_scope)
        if existing is not None:
            return existing
        if not self.host_available:
            certificate = StopCertificate(
                stop_id=request.stop_id,
                outcome="not_accepted",
                completed_monotonic_ns=self.clock.now_ns(),
                detail="execution_host_unavailable; status only",
            )
            self._remember_stop(request, certificate, full_scope=full_scope)
            return certificate

        selected = self._selected_participants(request.scope)
        if not selected:
            certificate = StopCertificate(
                stop_id=request.stop_id,
                outcome="not_accepted",
                completed_monotonic_ns=self.clock.now_ns(),
                detail="scope matched no dispatched participant",
            )
            self._remember_stop(request, certificate, full_scope=full_scope)
            return certificate
        if full_scope:
            self._pending_full_stop_handoffs.add(request.stop_id)

        certificate, stopped_domains = self._stop_selection(
            request,
            selected,
            full_scope=full_scope,
        )
        stopped_set = set(stopped_domains)
        affected_records: dict[OperationId, _OperationRecord] = {}
        for record, state in selected:
            affected_records[record.request.operation_id] = record
            if state.participant.control_domain_id in stopped_set:
                state.phase = "stopped"
                self._release_participant_resources(record, state)
                self._unfence_participant_resources(state)
            else:
                state.phase = "stop_failed"
                self._fence_participant_resources(state)
            record.stop_certificate = certificate

        for record in affected_records.values():
            phases = {state.phase for state in record.participants}
            if "stop_failed" in phases:
                record.phase = (
                    f"{record.settlement_phase}_stop_failed"
                    if record.settlement_phase is not None
                    else "stop_failed"
                )
                record.terminal_detail = f"Stop {request.stop_id} failed"
            elif record.settlement_phase is not None:
                record.phase = record.settlement_phase
                record.terminal_detail = (
                    f"Stop {request.stop_id} completed required containment"
                )
            elif phases <= {"stopped", "completed"}:
                record.phase = "stopped"
                record.terminal_detail = f"Stop {request.stop_id} settled"
            else:
                record.phase = "partially_stopped"
                record.terminal_detail = f"Stop {request.stop_id} was scoped"
        return certificate

    def _stop_selection(
        self,
        request: StopRequest,
        selected: list[tuple[_OperationRecord, _ParticipantState]],
        *,
        success_outcome: str = "stopped",
        success_detail: str = "scoped controller results and measured rest settled",
        full_scope: bool = False,
    ) -> tuple[StopCertificate, tuple[ControlDomainId, ...]]:
        existing = self._existing_stop(request, full_scope=full_scope)
        if existing is not None:
            return existing, existing.stopped_domains
        if not self.host_available:
            certificate = StopCertificate(
                stop_id=request.stop_id,
                outcome="not_accepted",
                completed_monotonic_ns=self.clock.now_ns(),
                detail="execution_host_unavailable; status only",
            )
            self._remember_stop(request, certificate, full_scope=full_scope)
            return certificate, ()
        requested_at_ns = self.clock.now_ns()
        if request.absolute_deadline_ns < requested_at_ns:
            certificate = StopCertificate(
                stop_id=request.stop_id,
                outcome="deadline_exceeded",
                completed_monotonic_ns=requested_at_ns,
                detail="Stop request arrived after its absolute deadline",
            )
            self._remember_stop(request, certificate, full_scope=full_scope)
            return certificate, ()

        by_domain: dict[
            ControlDomainId, list[tuple[_OperationRecord, _ParticipantState]]
        ] = {}
        for record, state in selected:
            by_domain.setdefault(
                state.participant.control_domain_id, []
            ).append((record, state))
        target_domains = tuple(by_domain)
        self._stop_generation += 1
        stop_generation = StopGeneration(self._stop_generation)
        targets = tuple(
            StopTarget(
                domain_id=domain_id,
                resource_ids=tuple(
                    dict.fromkeys(
                        resource_id
                        for _, state in entries
                        for resource_id in state.participant.resource_ids
                    )
                ),
                goals=tuple(
                    StoppedGoalSnapshot(
                        control_domain_id=domain_id,
                        operation_id=record.request.operation_id,
                        control_binding_id=state.participant.control_binding_id,
                        goal_id=state.goal_id,
                    )
                    for record, state in entries
                    if state.goal_id is not None
                ),
            )
            for domain_id, entries in by_domain.items()
        )
        envelope = StopEnvelopePrototype(
            clock=self.clock,
            stop_id=request.stop_id,
            scope=request.scope,
            absolute_deadline_ns=request.absolute_deadline_ns,
            targets=targets,
            measured_sample_freshness_ns=self.measured_sample_freshness_ns,
            stop_generation=stop_generation,
            requested_monotonic_ns=requested_at_ns,
            success_outcome=success_outcome,
            success_detail=success_detail,
        )
        errors: list[str] = []
        for domain_id, entries in by_domain.items():
            operation_ids = tuple(
                dict.fromkeys(
                    record.request.operation_id for record, _ in entries
                )
            )
            domain = self.domains[domain_id]
            self._record_trace("stop_attempt", domain_id=domain_id)
            try:
                domain.stop(
                    stop_id=request.stop_id,
                    operation_ids=operation_ids,
                )
            except DomainActionError as error:
                errors.append(f"{domain_id}:{error}")
                self._record_trace("stop_failed", domain_id=domain_id)
                continue

            evidence_sequence = 0
            for _, state in entries:
                if state.goal_id is None:
                    errors.append(
                        f"{domain_id}:selected participant has no goal"
                    )
                    continue
                evidence_sequence += 1
                envelope.record_controller_result(
                    domain_id=domain_id,
                    goal_id=state.goal_id,
                    evidence_sequence=EvidenceSequence(evidence_sequence),
                    stop_generation=stop_generation,
                )
            target = next(
                target for target in targets if target.domain_id == domain_id
            )
            evidence_sequence += 1
            envelope.record_measured_rest(
                domain_id=domain_id,
                sample_id=f"{request.stop_id}:{domain_id}:rest",
                resource_ids=target.resource_ids,
                evidence_sequence=EvidenceSequence(evidence_sequence),
                evidence_age_ns=EvidenceAgeNs(0),
                stop_generation=stop_generation,
                at_rest=True,
            )
            self._record_trace("stop_settled", domain_id=domain_id)

        if errors:
            certificate = envelope.fail(
                outcome="stop_failed",
                detail="; ".join(errors),
            )
        else:
            certificate = envelope.certificate
            if certificate is None:
                certificate = envelope.fail(
                    outcome="stop_failed",
                    detail="missing terminal evidence",
                )
        self._remember_stop(request, certificate, full_scope=full_scope)
        return certificate, certificate.stopped_domains

    @_serialized
    def full_stop(
        self,
        *,
        metadata: RequestMetadata,
        stop_id: StopTransactionId,
        absolute_deadline_ns: int,
    ) -> StopCertificate:
        existing = self._stop_replays.get(stop_id)
        if existing is not None:
            return self._stop(
                replace(
                    existing.request,
                    metadata=metadata,
                    absolute_deadline_ns=absolute_deadline_ns,
                ),
                full_scope=True,
            )
        active_ids = tuple(
            operation_id
            for operation_id, record in self._operations.items()
            if record.phase in self._ACTIVE_PHASES
        )
        return self._stop(
            StopRequest(
                metadata=metadata,
                stop_id=stop_id,
                scope=StopScope(operation_ids=active_ids),
                absolute_deadline_ns=absolute_deadline_ns,
            ),
            full_scope=True,
        )

    @_serialized
    def settle_stop_handoff(self, stop_id: StopTransactionId) -> StopCertificate:
        replay = self._stop_replays.get(stop_id)
        if replay is None or not replay.full_scope:
            raise ValueError("full Stop has no terminal settlement")
        self._pending_full_stop_handoffs.discard(stop_id)
        return replay.certificate

    @_serialized
    def controller_callback(
        self,
        *,
        operation_id: OperationId,
        domain_id: ControlDomainId,
        goal_id: GoalId,
        route_generation: RouteGeneration,
        outcome: str,
    ) -> CallbackDecision:
        if not self.host_available:
            return CallbackDecision.STALE_OPERATION
        record = self._operations.get(operation_id)
        if record is None or record.phase not in self._ACTIVE_PHASES:
            return CallbackDecision.STALE_OPERATION
        domain_states = tuple(
            participant
            for participant in record.participants
            if participant.participant.control_domain_id == domain_id
        )
        if not domain_states:
            return CallbackDecision.WRONG_DOMAIN
        state = next(
            (
                participant
                for participant in domain_states
                if participant.goal_id == goal_id
            ),
            None,
        )
        if state is None:
            return CallbackDecision.STALE_GOAL
        if goal_id in record.accepted_callbacks:
            return CallbackDecision.DUPLICATE
        if state.phase != "dispatched":
            return CallbackDecision.STALE_OPERATION
        if state.goal_id != goal_id:
            return CallbackDecision.STALE_GOAL
        domain = self.domains[domain_id]
        if (
            state.route_generation != route_generation
            or domain.route_generation != route_generation
        ):
            return CallbackDecision.STALE_ROUTE
        if outcome not in {"completed", "failed"}:
            return CallbackDecision.INVALID

        record.accepted_callbacks.add(goal_id)
        domain.mark_completed(
            operation_id,
            state.participant.control_binding_id,
        )
        state.phase = outcome
        if outcome == "failed":
            record.settlement_phase = "failed"
            self._fence_participant_resources(state)
            survivors = [
                participant
                for participant in record.participants
                if participant is not state and participant.phase == "dispatched"
            ]
            if survivors:
                stop_id = StopTransactionId(
                    f"callback-failure-stop:{domain_id}:{operation_id}"
                )
                request = StopRequest(
                    metadata=RequestMetadata(
                        request_id=RequestId(
                            f"callback-failure-request:{operation_id}"
                        ),
                        source_label="p5-controller-callback",
                        transport="in_process",
                        submitted_monotonic_ns=self.clock.now_ns(),
                    ),
                    stop_id=stop_id,
                    scope=StopScope(operation_ids=(operation_id,)),
                    absolute_deadline_ns=self.clock.now_ns()
                    + self.stop_budget_ns,
                )
                certificate, stopped_domains = self._stop_selection(
                    request,
                    [(record, participant) for participant in survivors],
                    success_outcome="survivors_stopped_after_participant_failure",
                )
                stopped_set = set(stopped_domains)
                for participant in survivors:
                    if (
                        participant.participant.control_domain_id
                        in stopped_set
                    ):
                        participant.phase = "stopped"
                        self._release_participant_resources(
                            record, participant
                        )
                        self._unfence_participant_resources(participant)
                    else:
                        participant.phase = "stop_failed"
                        self._fence_participant_resources(participant)
                record.stop_certificate = certificate
            record.phase = (
                "failed_stop_failed"
                if any(
                    participant.phase == "stop_failed"
                    for participant in record.participants
                )
                else "failed"
            )
            record.terminal_detail = f"{domain_id} controller callback failed"
            return CallbackDecision.ACCEPTED

        if all(
            participant.phase in {"completed", "failed", "stopped"}
            for participant in record.participants
        ):
            self._release_operation_resources(record)
            phases = {participant.phase for participant in record.participants}
            if "failed" in phases:
                record.phase = "failed"
            elif "stopped" in phases:
                record.phase = "stopped"
            else:
                record.phase = "completed"
            record.terminal_detail = f"controller callback: {outcome}"
        return CallbackDecision.ACCEPTED

    @_serialized
    def switch_route(
        self,
        *,
        domain_id: ControlDomainId,
        route_id: str,
    ) -> RouteGeneration | None:
        if not self.host_available:
            return None
        domain = self.domains[domain_id]
        if any(
            resource_id in self._resource_locks
            or resource_id in self._fenced_resources
            for resource_id in domain.owned_resources
        ):
            return None
        generation = domain.switch_route(route_id)
        for binding_id, retained in tuple(self.binding_registry.items()):
            if retained.control_domain_id != domain_id:
                continue
            self.binding_registry[binding_id] = RetainedControlBinding(
                replace(
                    retained.participant,
                    route_id=RouteId(route_id),
                    route_generation=generation,
                )
            )
        self._record_trace("route_switched", domain_id=domain_id)
        return generation

    @_serialized
    def reconcile_external_route_change(
        self, domain_id: ControlDomainId
    ) -> tuple[OperationId, ...]:
        """Fence changed-route goals and contain their coordinated peers."""

        if not self.host_available:
            return ()
        domain = self.domains[domain_id]
        affected = [
            record
            for record in self._operations.values()
            if record.phase in self._ACTIVE_PHASES
            and any(
                state.participant.control_domain_id == domain_id
                and state.phase == "dispatched"
                and state.route_generation != domain.route_generation
                for state in record.participants
            )
        ]
        for record in affected:
            record.settlement_phase = "route_changed"
            changed_states = [
                state
                for state in record.participants
                if state.participant.control_domain_id == domain_id
                and state.phase == "dispatched"
                and state.route_generation != domain.route_generation
            ]
            for state in changed_states:
                state.phase = "route_changed"
                self._fence_participant_resources(state)

            survivors = [
                state
                for state in record.participants
                if state.participant.control_domain_id != domain_id
                and state.phase == "dispatched"
            ]
            if survivors:
                stop_id = StopTransactionId(
                    f"route-change-stop:{domain_id}:{record.request.operation_id}"
                )
                request = StopRequest(
                    metadata=RequestMetadata(
                        request_id=RequestId(
                            f"route-change-request:{record.request.operation_id}"
                        ),
                        source_label="p5-route-reconciliation",
                        transport="in_process",
                        submitted_monotonic_ns=self.clock.now_ns(),
                    ),
                    stop_id=stop_id,
                    scope=StopScope(
                        operation_ids=(record.request.operation_id,)
                    ),
                    absolute_deadline_ns=self.clock.now_ns()
                    + self.stop_budget_ns,
                )
                certificate, stopped_domains = self._stop_selection(
                    request,
                    [(record, state) for state in survivors],
                    success_outcome="survivors_stopped_after_route_change",
                )
                stopped_set = set(stopped_domains)
                for state in survivors:
                    if state.participant.control_domain_id in stopped_set:
                        state.phase = "stopped"
                        self._release_participant_resources(record, state)
                        self._unfence_participant_resources(state)
                    else:
                        state.phase = "stop_failed"
                        self._fence_participant_resources(state)
                record.stop_certificate = certificate

            record.phase = (
                "route_changed_stop_failed"
                if any(
                    state.phase == "stop_failed"
                    for state in record.participants
                )
                else "route_changed"
            )
            record.terminal_detail = (
                f"{domain_id} route generation changed; old goal fenced"
            )
            self._record_trace(
                "external_route_change_reconciled",
                domain_id=domain_id,
                operation_id=record.request.operation_id,
            )
        return tuple(record.request.operation_id for record in affected)

    @_serialized
    def report_domain_fault(
        self,
        *,
        domain_id: ControlDomainId,
        stop_id: StopTransactionId,
        absolute_deadline_ns: int,
    ) -> StopCertificate:
        if not self.host_available:
            return self.stop(
                StopRequest(
                    metadata=RequestMetadata(
                        request_id=RequestId(f"fault-request:{stop_id}"),
                        source_label="p5-domain-fault",
                        transport="in_process",
                        submitted_monotonic_ns=self.clock.now_ns(),
                    ),
                    stop_id=stop_id,
                    scope=StopScope(control_domain_ids=(domain_id,)),
                    absolute_deadline_ns=absolute_deadline_ns,
                )
            )
        certificate = self.stop(
            StopRequest(
                metadata=RequestMetadata(
                    request_id=RequestId(f"fault-request:{stop_id}"),
                    source_label="p5-domain-fault",
                    transport="in_process",
                    submitted_monotonic_ns=self.clock.now_ns(),
                ),
                stop_id=stop_id,
                scope=StopScope(control_domain_ids=(domain_id,)),
                absolute_deadline_ns=absolute_deadline_ns,
            )
        )
        self.domains[domain_id].faulted = True
        self._record_trace("domain_faulted", domain_id=domain_id)
        return certificate

    @_serialized
    def report_collision(
        self,
        *,
        domain_id: ControlDomainId,
        stop_id: StopTransactionId,
        absolute_deadline_ns: int,
        consequence_scope: str,
    ) -> StopCertificate:
        affected_operation_ids = tuple(
            operation_id
            for operation_id, record in self._operations.items()
            if record.phase in self._ACTIVE_PHASES
            and any(
                state.participant.control_domain_id == domain_id
                for state in record.participants
            )
        )
        if consequence_scope == "local":
            scope = StopScope(control_domain_ids=(domain_id,))
        elif consequence_scope == "operation":
            scope = StopScope(operation_ids=affected_operation_ids)
        elif consequence_scope == "full":
            scope = StopScope(
                operation_ids=tuple(
                    operation_id
                    for operation_id, record in self._operations.items()
                    if record.phase in self._ACTIVE_PHASES
                )
            )
        else:
            raise ValueError("unknown collision consequence scope")
        return self.stop(
            StopRequest(
                metadata=RequestMetadata(
                    request_id=RequestId(f"collision-request:{stop_id}"),
                    source_label="p5-collision",
                    transport="in_process",
                    submitted_monotonic_ns=self.clock.now_ns(),
                ),
                stop_id=stop_id,
                scope=scope,
                absolute_deadline_ns=absolute_deadline_ns,
            )
        )

    @_serialized
    def report_controller_loss(
        self, domain_id: ControlDomainId
    ) -> tuple[ControllerLossEvidence, ...]:
        if not self.host_available:
            self._record_trace(
                "controller_loss_ignored_host_unavailable",
                domain_id=domain_id,
            )
            return ()
        lost_domain = self.domains[domain_id]
        lost_domain.lose_controller()
        evidence_rows: list[ControllerLossEvidence] = []
        affected = [
            record
            for record in self._operations.values()
            if record.phase in self._ACTIVE_PHASES
            and any(
                state.participant.control_domain_id == domain_id
                and state.phase == "dispatched"
                for state in record.participants
            )
        ]
        for record in affected:
            record.settlement_phase = "controller_lost"
            lost_states = [
                state
                for state in record.participants
                if state.participant.control_domain_id == domain_id
                and state.phase == "dispatched"
            ]
            for state in lost_states:
                state.phase = "controller_lost"
                self._fence_participant_resources(state)

            survivors = [
                state
                for state in record.participants
                if state.participant.control_domain_id != domain_id
                and state.phase == "dispatched"
            ]
            stop_id: StopTransactionId | None = None
            stopped_domains: tuple[ControlDomainId, ...] = ()
            if survivors:
                stop_id = StopTransactionId(
                    f"controller-loss-stop:{domain_id}:{record.request.operation_id}"
                )
                stop_request = StopRequest(
                    metadata=RequestMetadata(
                        request_id=RequestId(
                            f"controller-loss-request:{record.request.operation_id}"
                        ),
                        source_label="p5-controller-loss",
                        transport="in_process",
                        submitted_monotonic_ns=self.clock.now_ns(),
                    ),
                    stop_id=stop_id,
                    scope=StopScope(operation_ids=(record.request.operation_id,)),
                    absolute_deadline_ns=self.clock.now_ns() + self.stop_budget_ns,
                )
                certificate, stopped_domains = self._stop_selection(
                    stop_request,
                    [(record, state) for state in survivors],
                    success_outcome="survivors_stopped_after_controller_loss",
                )
                stopped_set = set(stopped_domains)
                for state in survivors:
                    if state.participant.control_domain_id in stopped_set:
                        state.phase = "stopped"
                        self._release_participant_resources(record, state)
                        self._unfence_participant_resources(state)
                    else:
                        state.phase = "stop_failed"
                        self._fence_participant_resources(state)
                record.stop_certificate = certificate

            record.phase = (
                "controller_lost_stop_failed"
                if any(
                    state.phase == "stop_failed"
                    for state in record.participants
                )
                else "controller_lost"
            )
            record.terminal_detail = (
                f"{domain_id} lost; affected resources remain fenced"
            )
            fenced = tuple(
                resource_id
                for state in lost_states
                for resource_id in state.participant.resource_ids
            )
            evidence_rows.append(
                ControllerLossEvidence(
                    domain_id=domain_id,
                    operation_id=record.request.operation_id,
                    stop_id=stop_id,
                    stopped_surviving_domains=stopped_domains,
                    fenced_resources=fenced,
                )
            )
        self._record_trace("controller_lost", domain_id=domain_id)
        return tuple(evidence_rows)

    @_serialized
    def report_execution_host_loss(self) -> HostLossEvidence:
        before = sum(domain.mutation_count for domain in self.domains.values())
        active_ids = tuple(
            operation_id
            for operation_id, record in self._operations.items()
            if record.phase in self._ACTIVE_PHASES
        )
        self.host_available = False
        self._record_trace("execution_host_lost_status_only")
        after = sum(domain.mutation_count for domain in self.domains.values())
        return HostLossEvidence(
            active_operation_ids=active_ids,
            domain_mutations_before=before,
            domain_mutations_after=after,
            status_only=before == after,
        )

    @_serialized
    def measure_isolation(self) -> IsolationMeasurement:
        ordered = tuple(
            sorted(self.domains.values(), key=lambda item: str(item.domain_id))
        )
        return IsolationMeasurement(
            rt_ticks=tuple(
                (domain.domain_id, domain.rt_tick_count) for domain in ordered
            ),
            nrt_calls=tuple(
                (domain.domain_id, domain.nrt_call_count) for domain in ordered
            ),
            nrt_calls_from_rt=tuple(
                (domain.domain_id, domain.nrt_calls_from_rt)
                for domain in ordered
            ),
            active_resource_locks=len(self._resource_locks),
            fenced_resources=len(self._fenced_resources),
            process_threads=threading.active_count(),
            max_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            fake_dds_entities=0,
        )

    @_serialized
    def measure_rt_path(self, *, iterations: int) -> RtPathMeasurement:
        """Measure fake update calls without asserting a production RT budget."""

        if iterations < 1:
            raise ValueError("iterations must be positive")
        ordered = tuple(
            sorted(self.domains.values(), key=lambda item: str(item.domain_id))
        )
        nrt_before = sum(domain.nrt_calls_from_rt for domain in ordered)
        threads_before = threading.active_count()
        elapsed_start = time.perf_counter_ns()
        cpu_start = time.process_time_ns()
        samples: list[int] = []
        for _ in range(iterations):
            for domain in ordered:
                sample_start = time.perf_counter_ns()
                domain.rt_tick()
                samples.append(time.perf_counter_ns() - sample_start)
        process_cpu_ns = time.process_time_ns() - cpu_start
        elapsed_ns = time.perf_counter_ns() - elapsed_start
        ordered_samples = sorted(samples)
        p50_ns = ordered_samples[(len(ordered_samples) - 1) // 2]
        p95_ns = ordered_samples[
            max(0, (95 * len(ordered_samples) + 99) // 100 - 1)
        ]
        return RtPathMeasurement(
            samples=len(samples),
            elapsed_ns=elapsed_ns,
            process_cpu_ns=process_cpu_ns,
            p50_ns=p50_ns,
            p95_ns=p95_ns,
            max_ns=ordered_samples[-1],
            p95_jitter_from_median_ns=max(0, p95_ns - p50_ns),
            process_load_ratio=(
                0.0 if elapsed_ns == 0 else process_cpu_ns / elapsed_ns
            ),
            threads_before=threads_before,
            threads_after=threading.active_count(),
            max_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            nrt_calls_from_rt=(
                sum(domain.nrt_calls_from_rt for domain in ordered)
                - nrt_before
            ),
            fake_dds_entities_created=0,
        )
