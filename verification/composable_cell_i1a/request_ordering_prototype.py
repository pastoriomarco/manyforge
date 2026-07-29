"""P1 candidate: deterministic caller-neutral ordering and bounded replay."""

from __future__ import annotations

from collections import OrderedDict, deque
from dataclasses import dataclass, fields, is_dataclass
from enum import Enum
import hashlib
import json
from threading import RLock
from typing import Any, Protocol

from verification.composable_cell_i1a.contracts import (
    ControlDomainId,
    ControlRequest,
    RequestId,
)


class MonotonicClock(Protocol):
    def now_ns(self) -> int: ...


class DecisionState(str, Enum):
    ACTIVE = "active"
    QUEUED = "queued"
    ACCEPTED = "accepted"
    REJECTED = "rejected"
    COMPLETED = "completed"
    CANCELLED = "cancelled"


@dataclass(frozen=True)
class RequestDecision:
    request_id: RequestId
    state: DecisionState
    reason: str
    sequence: int
    decided_monotonic_ns: int
    affected_request_ids: tuple[RequestId, ...] = ()


@dataclass(frozen=True)
class ExecutionHostStatus:
    alive: bool
    observed_monotonic_ns: int
    orphaned_request_ids: tuple[RequestId, ...]
    detail: str


@dataclass
class _Entry:
    request: ControlRequest
    digest: str
    decision: RequestDecision
    stop_generations: tuple[tuple[str, int], ...]


def _canonical(value: Any) -> Any:
    if is_dataclass(value):
        return {field.name: _canonical(getattr(value, field.name)) for field in fields(value)}
    if isinstance(value, Enum):
        return _canonical(value.value)
    if isinstance(value, dict):
        return {
            str(key): _canonical(item)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, (tuple, list)):
        return [_canonical(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def _request_digest(request: ControlRequest) -> str:
    metadata = request.metadata
    stable = {
        "metadata": {
            "request_id": str(metadata.request_id),
            "source_label": metadata.source_label,
            "transport": metadata.transport,
            "correlation_id": metadata.correlation_id,
        },
        "kind": _canonical(request.kind),
        "scope": _canonical(request.scope),
        "preconditions": _canonical(request.preconditions),
        "payload_sha256": request.payload_sha256,
    }
    encoded = json.dumps(stable, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _kind(request: ControlRequest) -> str:
    value = request.kind.value if isinstance(request.kind, Enum) else request.kind
    return str(value).lower()


def _domains(request: ControlRequest) -> frozenset[str]:
    return frozenset(str(item) for item in request.scope.control_domain_ids)


def _resources(request: ControlRequest) -> frozenset[str]:
    return frozenset(str(item) for item in request.scope.resource_ids)


def _resource_overlap(left: ControlRequest, right: ControlRequest) -> bool:
    return bool(_resources(left) & _resources(right))


def _scope_matches(left: ControlRequest, right: ControlRequest) -> bool:
    if _kind(left) == "full_stop":
        return True
    left_resources = _resources(left)
    right_resources = _resources(right)
    if left_resources and right_resources and left_resources & right_resources:
        return True
    left_domains = _domains(left)
    right_domains = _domains(right)
    return bool(left_domains and right_domains and left_domains & right_domains)


def _is_stop(request: ControlRequest) -> bool:
    return _kind(request) in {"stop", "full_stop"}


def _is_motion(request: ControlRequest) -> bool:
    return _kind(request) in {"execute", "run", "start", "resume"}


def _is_apply(request: ControlRequest) -> bool:
    return _kind(request) == "apply"


def _is_control_mutation(request: ControlRequest) -> bool:
    return _kind(request) in {
        "disable",
        "route_switch",
        "select_control_route",
        "select_route",
    }


class DeterministicRequestArbiter:
    """Bounded decision/replay ledger with explicit domain/resource ordering.

    The arbiter produces decisions only.  Execution-host loss updates status
    and orphan evidence; it cannot call a controller, vendor adapter, or Stop.
    """

    def __init__(
        self,
        *,
        clock: MonotonicClock,
        queue_capacity: int,
        replay_capacity: int,
        graph_revision: int = 0,
    ) -> None:
        if queue_capacity < 0:
            raise ValueError("queue_capacity_must_be_non_negative")
        if replay_capacity < 1:
            raise ValueError("replay_capacity_must_be_positive")
        self._clock = clock
        self._queue_capacity = queue_capacity
        self._replay_capacity = replay_capacity
        self._graph_revision = graph_revision
        self._topology_revisions: dict[str, int] = {}
        self._route_generations: dict[str, int] = {}
        self._binding_revisions: dict[str, int] = {}
        self._entries: dict[str, _Entry] = {}
        self._terminal_order: OrderedDict[str, None] = OrderedDict()
        self._active: OrderedDict[str, None] = OrderedDict()
        self._queued: deque[str] = deque()
        self._stop_generation = 0
        self._domain_stop_generations: dict[str, int] = {}
        self._stop_requested: set[str] = set()
        self._pending_stops: set[str] = set()
        self._sequence = 0
        self._host_alive = True
        self._orphaned: set[str] = set()
        self._host_history: list[ExecutionHostStatus] = []
        self._lock = RLock()

    def submit(self, request: ControlRequest) -> RequestDecision:
        """Order one request atomically across concurrent transport callers."""

        with self._lock:
            return self._submit_locked(request)

    def _submit_locked(self, request: ControlRequest) -> RequestDecision:
        request_id = str(request.metadata.request_id)
        digest = _request_digest(request)
        existing = self._entries.get(request_id)
        if existing is not None:
            if existing.digest == digest:
                return existing.decision
            return RequestDecision(
                request_id=request.metadata.request_id,
                state=DecisionState.REJECTED,
                reason="conflicting_request_id",
                sequence=existing.decision.sequence,
                decided_monotonic_ns=self._clock.now_ns(),
            )

        if not _is_stop(request):
            stale_reason = self._stale_reason(request)
            if stale_reason is not None:
                return self._record(
                    request, digest, DecisionState.REJECTED, stale_reason
                )

        if _is_stop(request):
            return self._accept_stop(request, digest)

        if _is_motion(request) and not self._host_alive:
            return self._record(
                request,
                digest,
                DecisionState.REJECTED,
                "execution_host_unavailable",
            )
        if _is_motion(request):
            blocking_stop = next(
                (
                    self._entries[item].request
                    for item in self._pending_stops
                    if _scope_matches(self._entries[item].request, request)
                ),
                None,
            )
            if blocking_stop is not None:
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    (
                        "full_stop_settlement_pending"
                        if _kind(blocking_stop) == "full_stop"
                        else "stop_settlement_pending"
                    ),
                )

        active_requests = tuple(
            self._entries[request_id].request for request_id in self._active
        )
        queued_requests = tuple(
            self._entries[request_id].request for request_id in self._queued
        )

        if _is_apply(request):
            if any(
                _is_motion(other) and _scope_matches(request, other)
                for other in active_requests + queued_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    "apply_affected_resources_busy",
                )
            return self._activate(request, digest, "apply_admitted")

        if _is_motion(request):
            if any(
                _is_apply(other) and _scope_matches(request, other)
                for other in active_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    "apply_busy",
                )
            if any(
                _is_control_mutation(other) and _scope_matches(request, other)
                for other in active_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    "control_mutation_busy",
                )
            if any(
                _is_motion(other) and _resource_overlap(request, other)
                for other in active_requests + queued_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    "overlapping_resource_conflict",
                )
            domain_busy = any(
                _is_motion(other) and bool(_domains(request) & _domains(other))
                for other in active_requests
            )
            if domain_busy:
                if len(self._queued) >= self._queue_capacity:
                    return self._record(
                        request,
                        digest,
                        DecisionState.REJECTED,
                        "queue_backpressure",
                    )
                return self._enqueue(request, digest)
            return self._activate(request, digest, "dispatched")

        if _is_control_mutation(request):
            if any(
                _is_motion(other) and _scope_matches(request, other)
                for other in active_requests + queued_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    f"{_kind(request)}_execution_conflict",
                )
            if any(
                (_is_apply(other) or _is_control_mutation(other))
                and _scope_matches(request, other)
                for other in active_requests
            ):
                return self._record(
                    request,
                    digest,
                    DecisionState.REJECTED,
                    "control_mutation_conflict",
                )
            return self._activate(request, digest, "control_mutation_admitted")

        return self._activate(request, digest, "admitted")

    def complete(
        self, request_id: RequestId, *, reason: str = "completed"
    ) -> tuple[RequestDecision, ...]:
        """Complete one active request and deterministically drain the queue."""

        with self._lock:
            return self._complete_locked(request_id, reason=reason)

    def _complete_locked(
        self, request_id: RequestId, *, reason: str
    ) -> tuple[RequestDecision, ...]:
        key = str(request_id)
        if key not in self._active:
            raise ValueError("request_not_active")
        self._active.pop(key)
        self._orphaned.discard(key)
        self._stop_requested.discard(key)
        entry = self._entries[key]
        completed = self._replace_decision(
            entry, DecisionState.COMPLETED, reason
        )
        self._remember_terminal(key)
        activated: list[RequestDecision] = [completed]
        activated.extend(self._drain_queue())
        return tuple(activated)

    def reconcile_orphan(
        self, request_id: RequestId, *, terminal_reason: str
    ) -> tuple[RequestDecision, ...]:
        with self._lock:
            key = str(request_id)
            if key not in self._orphaned:
                raise ValueError("request_not_orphaned")
            return self._complete_locked(request_id, reason=terminal_reason)

    def settle_stop(
        self,
        request_id: RequestId,
        *,
        terminal_reason: str = "stop_terminal_settled",
    ) -> RequestDecision:
        """Release a Stop fence only after explicit terminal settlement."""

        with self._lock:
            key = str(request_id)
            if key not in self._pending_stops:
                raise ValueError("stop_not_pending")
            self._pending_stops.remove(key)
            entry = self._entries[key]
            decision = self._replace_decision(
                entry,
                DecisionState.COMPLETED,
                terminal_reason,
            )
            self._remember_terminal(key)
            return decision

    def observe_external_revision(
        self,
        *,
        graph_revision: int | None = None,
        topology_revisions: dict[ControlDomainId | str, int] | None = None,
        route_generations: dict[ControlDomainId | str, int] | None = None,
        binding_revisions: dict[ControlDomainId | str, int] | None = None,
    ) -> tuple[RequestDecision, ...]:
        """Record external state and retire queued requests made stale by it."""

        with self._lock:
            return self._observe_external_revision_locked(
                graph_revision=graph_revision,
                topology_revisions=topology_revisions,
                route_generations=route_generations,
                binding_revisions=binding_revisions,
            )

    def _observe_external_revision_locked(
        self,
        *,
        graph_revision: int | None,
        topology_revisions: dict[ControlDomainId | str, int] | None,
        route_generations: dict[ControlDomainId | str, int] | None,
        binding_revisions: dict[ControlDomainId | str, int] | None,
    ) -> tuple[RequestDecision, ...]:
        if graph_revision is not None:
            self._graph_revision = graph_revision
        if topology_revisions:
            self._topology_revisions.update(
                {str(key): value for key, value in topology_revisions.items()}
            )
        if route_generations:
            self._route_generations.update(
                {str(key): value for key, value in route_generations.items()}
            )
        if binding_revisions:
            self._binding_revisions.update(
                {str(key): value for key, value in binding_revisions.items()}
            )

        retired: list[RequestDecision] = []
        retained: deque[str] = deque()
        while self._queued:
            request_id = self._queued.popleft()
            entry = self._entries[request_id]
            reason = self._stale_reason(entry.request)
            if reason is None:
                retained.append(request_id)
                continue
            retired.append(
                self._replace_decision(
                    entry, DecisionState.CANCELLED, f"{reason}_while_queued"
                )
            )
            self._remember_terminal(request_id)
        self._queued = retained
        return tuple(retired)

    def observe_execution_host(self, *, alive: bool) -> ExecutionHostStatus:
        """Publish host status without inventing Stop or controller effects."""

        with self._lock:
            self._host_alive = alive
            if not alive:
                self._orphaned.update(
                    request_id
                    for request_id in self._active
                    if _is_motion(self._entries[request_id].request)
                )
            status = ExecutionHostStatus(
                alive=alive,
                observed_monotonic_ns=self._clock.now_ns(),
                orphaned_request_ids=tuple(
                    self._entries[item].request.metadata.request_id
                    for item in sorted(self._orphaned)
                ),
                detail=(
                    "status_only_no_automatic_stop_or_resume"
                    if not alive
                    else "recovered_requires_explicit_orphan_reconciliation"
                ),
            )
            self._host_history.append(status)
            return status

    def observe_client_disconnect(self, source_label: str) -> tuple[RequestId, ...]:
        """Return affected requests; ownership and ordering remain unchanged."""

        with self._lock:
            return tuple(
                entry.request.metadata.request_id
                for entry in self._entries.values()
                if entry.request.metadata.source_label == source_label
                and entry.decision.state
                in {DecisionState.ACTIVE, DecisionState.QUEUED}
            )

    def decision(self, request_id: RequestId) -> RequestDecision | None:
        with self._lock:
            entry = self._entries.get(str(request_id))
            return None if entry is None else entry.decision

    @property
    def active_request_ids(self) -> tuple[RequestId, ...]:
        with self._lock:
            return tuple(
                self._entries[item].request.metadata.request_id
                for item in self._active
            )

    @property
    def queued_request_ids(self) -> tuple[RequestId, ...]:
        with self._lock:
            return tuple(
                self._entries[item].request.metadata.request_id
                for item in self._queued
            )

    @property
    def stop_requested_request_ids(self) -> tuple[RequestId, ...]:
        with self._lock:
            return tuple(
                self._entries[item].request.metadata.request_id
                for item in sorted(self._stop_requested)
            )

    @property
    def pending_stop_request_ids(self) -> tuple[RequestId, ...]:
        with self._lock:
            return tuple(
                self._entries[item].request.metadata.request_id
                for item in sorted(self._pending_stops)
            )

    @property
    def stop_generation(self) -> int:
        with self._lock:
            return self._stop_generation

    @property
    def replay_size(self) -> int:
        with self._lock:
            return len(self._terminal_order)

    @property
    def host_history(self) -> tuple[ExecutionHostStatus, ...]:
        with self._lock:
            return tuple(self._host_history)

    def _accept_stop(
        self, request: ControlRequest, digest: str
    ) -> RequestDecision:
        self._stop_generation += 1
        stop_domains = _domains(request)
        if stop_domains:
            for domain in stop_domains:
                self._domain_stop_generations[domain] = (
                    self._domain_stop_generations.get(domain, 0) + 1
                )

        cancelled: list[RequestId] = []
        retained: deque[str] = deque()
        while self._queued:
            request_id = self._queued.popleft()
            entry = self._entries[request_id]
            if _scope_matches(request, entry.request):
                cancelled.append(entry.request.metadata.request_id)
                self._replace_decision(
                    entry, DecisionState.CANCELLED, "superseded_by_stop"
                )
                self._remember_terminal(request_id)
            else:
                retained.append(request_id)
        self._queued = retained

        stopped_active = [
            request_id
            for request_id in self._active
            if _is_motion(self._entries[request_id].request)
            and _scope_matches(request, self._entries[request_id].request)
        ]
        self._stop_requested.update(stopped_active)
        affected = tuple(cancelled) + tuple(
            self._entries[item].request.metadata.request_id
            for item in stopped_active
        )
        decision = self._record(
            request,
            digest,
            DecisionState.ACCEPTED,
            "stop_precedence",
            affected_request_ids=affected,
        )
        request_id = str(request.metadata.request_id)
        self._pending_stops.add(request_id)
        self._terminal_order.pop(request_id, None)
        return decision

    def _activate(
        self, request: ControlRequest, digest: str, reason: str
    ) -> RequestDecision:
        decision = self._record(
            request, digest, DecisionState.ACTIVE, reason
        )
        self._active[str(request.metadata.request_id)] = None
        return decision

    def _enqueue(
        self, request: ControlRequest, digest: str
    ) -> RequestDecision:
        decision = self._record(
            request, digest, DecisionState.QUEUED, "domain_serialized"
        )
        self._queued.append(str(request.metadata.request_id))
        return decision

    def _record(
        self,
        request: ControlRequest,
        digest: str,
        state: DecisionState,
        reason: str,
        *,
        affected_request_ids: tuple[RequestId, ...] = (),
    ) -> RequestDecision:
        self._sequence += 1
        decision = RequestDecision(
            request_id=request.metadata.request_id,
            state=state,
            reason=reason,
            sequence=self._sequence,
            decided_monotonic_ns=self._clock.now_ns(),
            affected_request_ids=affected_request_ids,
        )
        generations = tuple(
            sorted(
                (
                    domain,
                    self._domain_stop_generations.get(domain, 0),
                )
                for domain in _domains(request)
            )
        )
        self._entries[str(request.metadata.request_id)] = _Entry(
            request=request,
            digest=digest,
            decision=decision,
            stop_generations=generations,
        )
        if state in {
            DecisionState.ACCEPTED,
            DecisionState.REJECTED,
            DecisionState.COMPLETED,
            DecisionState.CANCELLED,
        }:
            self._remember_terminal(str(request.metadata.request_id))
        return decision

    def _replace_decision(
        self, entry: _Entry, state: DecisionState, reason: str
    ) -> RequestDecision:
        self._sequence += 1
        entry.decision = RequestDecision(
            request_id=entry.request.metadata.request_id,
            state=state,
            reason=reason,
            sequence=self._sequence,
            decided_monotonic_ns=self._clock.now_ns(),
        )
        return entry.decision

    def _remember_terminal(self, request_id: str) -> None:
        if request_id in self._pending_stops:
            return
        self._terminal_order.pop(request_id, None)
        self._terminal_order[request_id] = None
        while len(self._terminal_order) > self._replay_capacity:
            evicted, _ = self._terminal_order.popitem(last=False)
            entry = self._entries.get(evicted)
            if entry is not None and entry.decision.state not in {
                DecisionState.ACTIVE,
                DecisionState.QUEUED,
            }:
                del self._entries[evicted]

    def _drain_queue(self) -> tuple[RequestDecision, ...]:
        if not self._host_alive:
            # Old queued motion is not resumed merely because its predecessor
            # became terminal while the execution host is unavailable.
            return ()
        activated: list[RequestDecision] = []
        retained: deque[str] = deque()
        while self._queued:
            request_id = self._queued.popleft()
            entry = self._entries[request_id]
            if self._queued_request_was_stopped(entry):
                self._replace_decision(
                    entry, DecisionState.CANCELLED, "superseded_by_stop"
                )
                self._remember_terminal(request_id)
                continue
            if any(
                _scope_matches(self._entries[item].request, entry.request)
                for item in self._pending_stops
            ):
                retained.append(request_id)
                continue
            stale_reason = self._stale_reason(entry.request)
            if stale_reason is not None:
                self._replace_decision(
                    entry,
                    DecisionState.CANCELLED,
                    f"{stale_reason}_while_queued",
                )
                self._remember_terminal(request_id)
                continue
            active_requests = tuple(
                self._entries[item].request for item in self._active
            )
            if any(
                _resource_overlap(entry.request, other)
                or bool(_domains(entry.request) & _domains(other))
                for other in active_requests
            ):
                retained.append(request_id)
                continue
            decision = self._replace_decision(
                entry, DecisionState.ACTIVE, "dequeued"
            )
            self._active[request_id] = None
            activated.append(decision)
        self._queued = retained
        return tuple(activated)

    def _queued_request_was_stopped(self, entry: _Entry) -> bool:
        recorded = dict(entry.stop_generations)
        return any(
            self._domain_stop_generations.get(domain, 0) != generation
            for domain, generation in recorded.items()
        )

    def _stale_reason(self, request: ControlRequest) -> str | None:
        preconditions = request.preconditions
        if (
            preconditions.expected_graph_revision is not None
            and int(preconditions.expected_graph_revision)
            != self._graph_revision
        ):
            return "stale_graph_revision"
        by_domain = {
            str(item.control_domain_id): item
            for item in preconditions.domains
        }
        for domain in _domains(request):
            domain_preconditions = by_domain.get(domain)
            if domain_preconditions is None:
                continue
            expected = domain_preconditions.expected_topology_revision
            if expected is not None and int(expected) != self._topology_revisions.get(
                domain, 0
            ):
                return "stale_topology_revision"
            expected = domain_preconditions.expected_route_generation
            if expected is not None and int(expected) != self._route_generations.get(
                domain, 0
            ):
                return "stale_route_generation"
            expected = domain_preconditions.expected_binding_revision
            if expected is not None and int(expected) != self._binding_revisions.get(
                domain, 0
            ):
                return "stale_binding_revision"
        return None
