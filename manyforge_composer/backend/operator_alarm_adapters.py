"""Explicit authoritative-domain adapters for the initial alarm catalog.

No operational log severity or free-form event name is consulted here. Each
method accepts a current-state publication from the domain named by the
catalog, translates it to typed condition updates, and delivers lifecycle facts
to the historian only after the manager mutation has committed.
"""
from __future__ import annotations

from collections import Counter, deque
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import threading
import time
from typing import Any, Callable, Iterable, Mapping

from .operator_alarms import (
    AlarmConditionUpdate,
    AlarmLifecycleFact,
    OperatorAlarmError,
    OperatorAlarmManager,
    OperatorAlarmMutation,
)


_MOTION_DEFINITIONS = frozenset(
    {
        "manyforge.motion.runtime_collision",
        "manyforge.motion.execution_fault",
    }
)
_POSE_DEFINITION = "manyforge.scene.pose_apply_fault"
_INITIALIZATION_DEFINITION = "manyforge.runtime.scene_initialization_fault"
_PROVIDER_DEFINITION = "manyforge.runtime.provider_unavailable"
_ROBOT_LIFECYCLE_DEFINITION = "manyforge.runtime.robot_lifecycle_fault"
_SOURCE_CONFIG = {
    "motion": (
        "manyforge.condition.motion",
        "manyforge.adapter.motion_condition",
    ),
    "scene": (
        "manyforge.condition.scene",
        "manyforge.adapter.scene_condition",
    ),
    "runtime_health": (
        "manyforge.condition.runtime_health",
        "manyforge.adapter.runtime_health_condition",
    ),
}
_POSE_CLEAR_SKIP_REASONS = frozenset(
    {
        "attached_object",
        "pose_source_duplicate_no_op",
        "pose_source_unchanged_without_runtime_freshness",
    }
)
_MOTION_FAULT_CLASSES = frozenset(
    {"unknown_fault", "stop_fault_intervention"}
)
_PROVIDER_UNAVAILABLE_STATES = frozenset(
    {"fault", "error", "offline", "degraded"}
)
_PERSISTENCE_HEALTH_CODE = "operator_alarm_lifecycle_persistence_failed"
_ADAPTER_HEALTH_CODE = "operator_alarm_adapter_delivery_failed"
_MOTION_IDENTITY_HEALTH_CODE = "operator_alarm_motion_identity_unavailable"
_PERSISTENCE_OVERFLOW_HEALTH_CODE = "operator_alarm_lifecycle_outbox_overflow"
_MAX_PENDING_LIFECYCLE_FACTS = 8192


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _bounded_text(value: Any, *, fallback: str, limit: int = 512) -> str:
    text = str(value or "").strip()
    return (text or fallback)[:limit]


def _runtime_pair(details: Mapping[str, Any] | None) -> str | None:
    if not isinstance(details, Mapping):
        return None
    merged = dict(details)
    nested = details.get("error_details")
    if isinstance(nested, Mapping):
        for key, value in nested.items():
            merged.setdefault(str(key), value)
    raw = next(
        (
            merged.get(key)
            for key in (
                "contact_pair",
                "colliding_pair",
                "collision_result_first_failure_pair",
                "first_failure_pair",
            )
            if merged.get(key)
        ),
        None,
    )
    if isinstance(raw, (list, tuple)) and len(raw) == 2:
        parts = [str(value).strip() for value in raw]
    elif isinstance(raw, str) and "<->" in raw:
        parts = [part.strip() for part in raw.split("<->", 1)]
    elif isinstance(raw, str) and raw.strip():
        return raw.strip()[:512]
    else:
        return None
    if not all(parts):
        return None
    return " <-> ".join(sorted(parts))[:512]


def _details_digest(value: Mapping[str, Any] | None) -> str:
    try:
        encoded = json.dumps(
            dict(value or {}),
            sort_keys=True,
            separators=(",", ":"),
            default=str,
        ).encode("utf-8")
    except (TypeError, ValueError):
        encoded = repr(value).encode("utf-8", errors="replace")
    return hashlib.sha256(encoded).hexdigest()


def _bounded_detail_value(value: Any, *, depth: int = 0) -> Any:
    """Return deterministic, JSON-safe diagnostic evidence below manager bounds."""

    if value is None or type(value) in {bool, int}:
        return value
    if type(value) is float:
        return value if value == value and value not in {float("inf"), float("-inf")} else str(value)
    if isinstance(value, str):
        return value[:1024]
    if depth >= 2:
        return _bounded_text(value, fallback="unavailable", limit=1024)
    if isinstance(value, Mapping):
        return {
            _bounded_text(key, fallback="unknown", limit=128): _bounded_detail_value(
                item,
                depth=depth + 1,
            )
            for key, item in list(sorted(value.items(), key=lambda pair: str(pair[0])))[:24]
        }
    if isinstance(value, (list, tuple)):
        return [
            _bounded_detail_value(item, depth=depth + 1)
            for item in list(value)[:16]
        ]
    return _bounded_text(value, fallback="unavailable", limit=1024)


def _bounded_details(value: Mapping[str, Any] | None) -> dict[str, Any]:
    bounded = _bounded_detail_value(dict(value or {}))
    return bounded if isinstance(bounded, dict) else {"summary": bounded}


@dataclass(frozen=True, slots=True)
class _AdapterSource:
    domain: str
    source_id: str
    adapter_id: str
    epoch: str
    epoch_order: int
    sequence: int = 0


class OperatorAlarmAdapters:
    """One serialized adapter layer for all catalogued condition authorities."""

    def __init__(
        self,
        manager: OperatorAlarmManager,
        *,
        lifecycle_sink: Callable[[tuple[AlarmLifecycleFact, ...]], Any] | None = None,
        monotonic_clock: Callable[[], int] = time.monotonic_ns,
        wall_clock: Callable[[], str] = _utc_now,
        provider_freshness_s: float = 6.0,
    ) -> None:
        if provider_freshness_s <= 0:
            raise ValueError("provider_freshness_s must be positive")
        self.manager = manager
        self._lifecycle_sink = lifecycle_sink
        self._monotonic_clock = monotonic_clock
        self._wall_clock = wall_clock
        self._provider_freshness_ns = int(provider_freshness_s * 1_000_000_000)
        self._lock = threading.Lock()
        self._persistence_lock = threading.Lock()
        self._metrics_lock = threading.Lock()
        self._pending_lifecycle_facts: deque[AlarmLifecycleFact] = deque()
        self._metrics: Counter[str] = Counter()
        self._sources: dict[str, _AdapterSource] = {}
        self._condition_states: dict[
            tuple[str, tuple[tuple[str, str], ...]], tuple[bool, str]
        ] = {}
        self._runtime_lane = "composer"
        self._robot_resource = "unbound-robot"
        self._pose_scopes: tuple[dict[str, str], ...] = ()
        self._required_providers: set[str] = set()
        self._provider_records: dict[str, dict[str, Any]] = {}
        self._provider_waiting_since_ns: dict[str, int] = {}
        self._provider_expectation_started_ns = self._monotonic_clock()
        self._robot_lifecycle_fault_scope: dict[str, str] | None = None
        self._scene_ready = True
        self._motion_signature: tuple[Any, ...] | None = None
        self._register_epoch_locked("composer-startup", epoch_order=0)

    def _record_metric(self, name: str, amount: int = 1) -> None:
        with self._metrics_lock:
            self._metrics[name] += int(amount)

    def _register_epoch_locked(self, epoch: str, *, epoch_order: int) -> None:
        sources: dict[str, _AdapterSource] = {}
        for domain, (source_id, adapter_id) in _SOURCE_CONFIG.items():
            self.manager.register_source(
                domain=domain,
                source_id=source_id,
                adapter_id=adapter_id,
                source_epoch=epoch,
                source_epoch_order=epoch_order,
            )
            sources[domain] = _AdapterSource(
                domain=domain,
                source_id=source_id,
                adapter_id=adapter_id,
                epoch=epoch,
                epoch_order=epoch_order,
            )
        self._sources = sources

    def _source_for_definition(self, definition_id: str) -> _AdapterSource:
        if definition_id in _MOTION_DEFINITIONS:
            return self._sources["motion"]
        if definition_id == _POSE_DEFINITION:
            return self._sources["scene"]
        if definition_id in {
            _INITIALIZATION_DEFINITION,
            _PROVIDER_DEFINITION,
            _ROBOT_LIFECYCLE_DEFINITION,
        }:
            return self._sources["runtime_health"]
        raise OperatorAlarmError(
            "operator_alarm_adapter_definition_unknown",
            f"No explicit adapter owns {definition_id}",
        )

    def _key(
        self,
        definition_id: str,
        scope: Mapping[str, str],
    ) -> tuple[str, tuple[tuple[str, str], ...]]:
        definition = self.manager.catalog.definition(definition_id)
        return (
            definition_id,
            tuple((field, str(scope[field])) for field in definition.scope_fields),
        )

    def _submit_locked(
        self,
        *,
        definition_id: str,
        scope: Mapping[str, str],
        active: bool,
        quality: str = "good",
        details: Mapping[str, Any] | None = None,
        evidence_refs: Iterable[str] = (),
        correlations: Mapping[str, Any] | None = None,
        force_repeat: bool = False,
    ) -> OperatorAlarmMutation | None:
        key = self._key(definition_id, scope)
        previous = self._condition_states.get(key)
        if previous == (active, quality) and not force_repeat:
            self._record_metric("expected_lifecycle_suppressions")
            return None
        source = self._source_for_definition(definition_id)
        next_source = _AdapterSource(
            domain=source.domain,
            source_id=source.source_id,
            adapter_id=source.adapter_id,
            epoch=source.epoch,
            epoch_order=source.epoch_order,
            sequence=source.sequence + 1,
        )
        update = AlarmConditionUpdate(
            definition_id=definition_id,
            scope=dict(scope),
            active=active,
            source_id=next_source.source_id,
            source_epoch=next_source.epoch,
            source_sequence=next_source.sequence,
            observed_at_ns=self._monotonic_clock(),
            observed_at_wall=self._wall_clock(),
            quality=quality,
            details=dict(details or {}),
            evidence_refs=tuple(dict.fromkeys(str(ref) for ref in evidence_refs if ref)),
            correlations=dict(correlations or {}),
        )
        try:
            mutation = self.manager.submit(update)
        except Exception as exc:
            self._record_metric("adapter_delivery_failures")
            self.manager.mark_health_unavailable(
                code=_ADAPTER_HEALTH_CODE,
                message=f"{definition_id} adapter delivery failed: {exc}",
            )
            raise
        self._sources[source.domain] = next_source
        self._condition_states[key] = (active, quality)
        self.manager.reconcile_health(code=_ADAPTER_HEALTH_CODE)
        return mutation

    def _deliver(
        self,
        mutations: Iterable[OperatorAlarmMutation | None],
    ) -> list[int]:
        facts = tuple(
            fact
            for mutation in mutations
            if mutation is not None
            for fact in mutation.lifecycle_facts
        )
        return self.deliver_lifecycle_facts(facts)

    def deliver_lifecycle_facts(
        self,
        facts: Iterable[AlarmLifecycleFact],
    ) -> list[int]:
        """Queue committed facts and flush them in order outside manager locks."""

        incoming = tuple(facts)
        persisted_indices: list[int] = []
        with self._persistence_lock:
            available = _MAX_PENDING_LIFECYCLE_FACTS - len(
                self._pending_lifecycle_facts
            )
            if len(incoming) > available:
                self._pending_lifecycle_facts.extend(incoming[: max(0, available)])
                self._record_metric(
                    "lifecycle_persistence_overflow_facts",
                    len(incoming) - max(0, available),
                )
                self.manager.mark_health_unavailable(
                    code=_PERSISTENCE_OVERFLOW_HEALTH_CODE,
                    message=(
                        "Operator-alarm lifecycle outbox is full; audit continuity "
                        "cannot be established"
                    ),
                )
            else:
                self._pending_lifecycle_facts.extend(incoming)
            if self._lifecycle_sink is None:
                # Unit-level adapters may deliberately omit a historian. The
                # production Composer always supplies one.
                self._pending_lifecycle_facts.clear()
                self._record_metric(
                    "lifecycle_facts_discarded_no_sink", len(incoming)
                )
                return persisted_indices
            while self._pending_lifecycle_facts:
                fact = self._pending_lifecycle_facts[0]
                try:
                    result = self._lifecycle_sink((fact,))
                except Exception as exc:
                    self._record_metric("lifecycle_persistence_failures")
                    self.manager.mark_health_unavailable(
                        code=_PERSISTENCE_HEALTH_CODE,
                        message=(
                            "Operator-alarm lifecycle persistence failed: "
                            f"{exc}"
                        ),
                    )
                    return persisted_indices
                self._pending_lifecycle_facts.popleft()
                self._record_metric("lifecycle_facts_persisted")
                if isinstance(result, (list, tuple)):
                    persisted_indices.extend(
                        int(value) for value in result if isinstance(value, int)
                    )
            self.manager.reconcile_health(code=_PERSISTENCE_HEALTH_CODE)
        return persisted_indices

    def flush_lifecycle_facts(self) -> list[int]:
        return self.deliver_lifecycle_facts(())

    def persistence_status(self) -> dict[str, Any]:
        with self._persistence_lock:
            with self._metrics_lock:
                metrics = dict(self._metrics)
            return {
                "pendingLifecycleFacts": len(self._pending_lifecycle_facts),
                "capacity": _MAX_PENDING_LIFECYCLE_FACTS,
                "metrics": metrics,
            }

    def initialize_empty_runtime(self) -> None:
        """Publish the authoritative empty/startup snapshot."""

        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            mutations.append(
                self._submit_locked(
                    definition_id=_INITIALIZATION_DEFINITION,
                    scope={"runtime_lane": self._runtime_lane},
                    active=False,
                    details={"reason": "empty_runtime_normal"},
                )
            )
            for source in self._sources.values():
                self.manager.complete_source_snapshot(
                    source_id=source.source_id,
                    source_epoch=source.epoch,
                    quality="good",
                )
        self._deliver(mutations)

    def begin_runtime_epoch(
        self,
        *,
        epoch: str,
        runtime_lane: str,
        robot_resource: str,
        pose_scopes: Iterable[Mapping[str, str]],
        required_providers: Iterable[str],
    ) -> None:
        """Retire old conditions and fence all adapters to one new runtime."""

        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            old_active = [
                (definition_id, dict(scope_items))
                for (definition_id, scope_items), (active, _quality)
                in self._condition_states.items()
                if active
            ]
            next_order = max(
                (source.epoch_order for source in self._sources.values()),
                default=-1,
            ) + 1
            self._register_epoch_locked(epoch, epoch_order=next_order)
            for definition_id, scope in old_active:
                mutations.append(
                    self._submit_locked(
                        definition_id=definition_id,
                        scope=scope,
                        active=False,
                        details={"reason": "runtime_epoch_retired"},
                        correlations={"retired_by_epoch": epoch},
                    )
                )
            self._condition_states.clear()
            self._runtime_lane = _bounded_text(
                runtime_lane,
                fallback="composer",
            )
            self._robot_resource = _bounded_text(
                robot_resource,
                fallback="unbound-robot",
            )
            unique_pose_scopes: dict[
                tuple[str, str, str], dict[str, str]
            ] = {}
            for raw_scope in pose_scopes:
                scope = {
                    "pose_source_id": _bounded_text(
                        raw_scope.get("pose_source_id"), fallback="unknown-source"
                    ),
                    "object_id": _bounded_text(
                        raw_scope.get("object_id"), fallback="unknown-object"
                    ),
                    "apply_policy_id": _bounded_text(
                        raw_scope.get("apply_policy_id"), fallback="unknown-policy"
                    ),
                }
                unique_pose_scopes[
                    (
                        scope["pose_source_id"],
                        scope["object_id"],
                        scope["apply_policy_id"],
                    )
                ] = scope
            self._pose_scopes = tuple(
                unique_pose_scopes[key] for key in sorted(unique_pose_scopes)
            )
            self._required_providers = {
                _bounded_text(provider, fallback="unknown-provider")
                for provider in required_providers
                if str(provider or "").strip()
            }
            self._provider_records.clear()
            self._provider_waiting_since_ns.clear()
            self._provider_expectation_started_ns = self._monotonic_clock()
            self._robot_lifecycle_fault_scope = None
            self._scene_ready = False
            self._motion_signature = None
            mutations.append(
                self._submit_locked(
                    definition_id=_INITIALIZATION_DEFINITION,
                    scope={"runtime_lane": self._runtime_lane},
                    active=False,
                    details={"reason": "initialization_expected"},
                    correlations={"initialization_token": epoch},
                )
            )
        self._deliver(mutations)

    def _publish_pose_normals_locked(self, *, reason: str) -> list[OperatorAlarmMutation | None]:
        return [
            self._submit_locked(
                definition_id=_POSE_DEFINITION,
                scope=scope,
                active=False,
                details={"reason": reason},
            )
            for scope in self._pose_scopes
        ]

    def scene_initialization_failed(
        self,
        *,
        error: str,
        initialization_token: str | None,
    ) -> None:
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            self._scene_ready = False
            mutations.append(
                self._submit_locked(
                    definition_id=_INITIALIZATION_DEFINITION,
                    scope={"runtime_lane": self._runtime_lane},
                    active=True,
                    details={"error_summary": _bounded_text(error, fallback="unknown")},
                    evidence_refs=(
                        (
                            f"scene-initialization:{initialization_token}"
                            if initialization_token
                            else "scene-initialization:current"
                        ),
                    ),
                    correlations={"initialization_token": initialization_token},
                    force_repeat=True,
                )
            )
            mutations.extend(
                self._publish_pose_normals_locked(
                    reason="initialization_failed_pose_apply_inapplicable"
                )
            )
            scene_source = self._sources["scene"]
            self.manager.complete_source_snapshot(
                source_id=scene_source.source_id,
                source_epoch=scene_source.epoch,
                quality="good",
            )
            self._reconcile_runtime_source_locked()
        self._deliver(mutations)

    def scene_initialization_opened(
        self,
        *,
        initialization_token: str,
    ) -> None:
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            self._scene_ready = True
            mutations.append(
                self._submit_locked(
                    definition_id=_INITIALIZATION_DEFINITION,
                    scope={"runtime_lane": self._runtime_lane},
                    active=False,
                    details={"reason": "initialization_succeeded"},
                    correlations={"initialization_token": initialization_token},
                )
            )
            mutations.extend(
                self._publish_pose_normals_locked(reason="post_initialization_normal")
            )
            scene_source = self._sources["scene"]
            self.manager.complete_source_snapshot(
                source_id=scene_source.source_id,
                source_epoch=scene_source.epoch,
                quality="good",
            )
            mutations.extend(self._evaluate_providers_locked(self._monotonic_clock()))
        self._deliver(mutations)

    def observe_pose_apply(self, result: Mapping[str, Any]) -> None:
        """Set/clear per-object pose faults from one serialized apply result."""

        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            if not self._scene_ready or bool(result.get("generation_invalidated")):
                return
            policy = _bounded_text(result.get("policy"), fallback="unknown-policy")
            required_missing = {
                str(value) for value in result.get("required_missing", []) if value
            }
            by_identity = {
                (
                    scope["pose_source_id"],
                    scope["object_id"],
                    scope["apply_policy_id"],
                ): scope
                for scope in self._pose_scopes
            }
            seen: set[tuple[str, str, str]] = set()
            for raw_detail in result.get("details", []):
                if not isinstance(raw_detail, Mapping):
                    continue
                source_id = str(raw_detail.get("source_id") or "")
                object_id = str(raw_detail.get("object_id") or "")
                key = (source_id, object_id, policy)
                scope = by_identity.get(key)
                if scope is None:
                    continue
                seen.add(key)
                status = str(raw_detail.get("status") or "")
                skip_reason = str(raw_detail.get("skip_reason") or "")
                active = status == "rejected" or (
                    object_id in required_missing
                    and not (
                        status == "skipped" and skip_reason in _POSE_CLEAR_SKIP_REASONS
                    )
                )
                establishes_normal = status == "applied" or (
                    status == "skipped" and skip_reason in _POSE_CLEAR_SKIP_REASONS
                )
                if not active and not establishes_normal:
                    continue
                mutations.append(
                    self._submit_locked(
                        definition_id=_POSE_DEFINITION,
                        scope=scope,
                        active=active,
                        details={
                            "apply_result": _bounded_details(raw_detail),
                            "generation": result.get("generation"),
                            "reason": result.get("reason"),
                        },
                        evidence_refs=(
                            "pose-apply:"
                            f"{result.get('generation', 0)}:{source_id}:{object_id}:{policy}",
                        ),
                        correlations={
                            "program_generation": result.get("program_generation"),
                            "deployment_generation": result.get(
                                "deployment_generation"
                            ),
                            "pose_source_apply_generation": result.get("generation"),
                        },
                        force_repeat=active,
                    )
                )
            for object_id in required_missing:
                for key, scope in by_identity.items():
                    if key in seen or key[1] != object_id or key[2] != policy:
                        continue
                    mutations.append(
                        self._submit_locked(
                            definition_id=_POSE_DEFINITION,
                            scope=scope,
                            active=True,
                            details={
                                "error_summary": _bounded_text(
                                    result.get("error"), fallback="pose apply failed"
                                ),
                                "generation": result.get("generation"),
                                "reason": result.get("reason"),
                            },
                            evidence_refs=(
                                "pose-apply:"
                                f"{result.get('generation', 0)}:{key[0]}:{object_id}:{policy}",
                            ),
                            force_repeat=True,
                        )
                    )
        self._deliver(mutations)

    def motion_recovery_snapshot(
        self,
        *,
        offer: Mapping[str, Any] | None,
        error_code: str | None,
        error_details: Mapping[str, Any] | None,
    ) -> None:
        """Project the current recovery authority into motion conditions."""

        offer_payload = dict(offer or {})
        signature = (
            offer_payload.get("offerId"),
            offer_payload.get("offerRevision"),
            offer_payload.get("recoveryClass"),
            offer_payload.get("status"),
            error_code,
            _details_digest(error_details),
        )
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            if signature == self._motion_signature:
                return
            self._motion_signature = signature
            target: tuple[str, dict[str, str]] | None = None
            recovery_class = str(offer_payload.get("recoveryClass") or "")
            if offer and recovery_class == "runtime_collision":
                pair = _runtime_pair(error_details)
                if pair is None:
                    self.manager.mark_health_unavailable(
                        code=_MOTION_IDENTITY_HEALTH_CODE,
                        message=(
                            "Runtime collision recovery is active but its normalized "
                            "contact pair is unavailable"
                        ),
                    )
                    motion = self._sources["motion"]
                    self.manager.mark_source_quality_unknown(
                        source_id=motion.source_id,
                        source_epoch=motion.epoch,
                    )
                    return
                self.manager.reconcile_health(code=_MOTION_IDENTITY_HEALTH_CODE)
                target = (
                    "manyforge.motion.runtime_collision",
                    {
                        "robot_resource": self._robot_resource,
                        "contact_pair": pair,
                    },
                )
            elif offer and recovery_class in _MOTION_FAULT_CLASSES:
                fault_code = _bounded_text(
                    error_code or offer_payload.get("reasonCode"),
                    fallback="unknown_motion_fault",
                )
                target = (
                    "manyforge.motion.execution_fault",
                    {
                        "robot_resource": self._robot_resource,
                        "fault_code": fault_code,
                    },
                )

            target_key = self._key(*target) if target is not None else None
            for condition_key, (active, _quality) in tuple(
                self._condition_states.items()
            ):
                definition_id, scope_items = condition_key
                if definition_id not in _MOTION_DEFINITIONS or not active:
                    continue
                if condition_key == target_key:
                    continue
                mutations.append(
                    self._submit_locked(
                        definition_id=definition_id,
                        scope=dict(scope_items),
                        active=False,
                        details={"reason": "motion_recovery_context_resolved"},
                        correlations={
                            "replacement_offer_id": offer_payload.get("offerId")
                        },
                    )
                )
            if target is not None:
                definition_id, scope = target
                evidence = [str(value) for value in offer_payload.get("evidenceRefs", [])]
                for field_name in ("latchId", "stopCertificateId", "offerId"):
                    if offer_payload.get(field_name):
                        evidence.append(f"{field_name}:{offer_payload[field_name]}")
                mutations.append(
                    self._submit_locked(
                        definition_id=definition_id,
                        scope=scope,
                        active=True,
                        details={
                            "recovery_class": recovery_class,
                            "recovery_status": offer_payload.get("status"),
                            "reason_code": offer_payload.get("reasonCode"),
                            "error_details": _bounded_details(error_details),
                        },
                        evidence_refs=evidence[:32],
                        correlations={
                            "recovery_offer_id": offer_payload.get("offerId"),
                            "latch_id": offer_payload.get("latchId"),
                            "execution_id": offer_payload.get("executionId"),
                            "program_generation": offer_payload.get(
                                "programGeneration"
                            ),
                            "deployment_generation": offer_payload.get(
                                "deploymentGeneration"
                            ),
                        },
                    )
                )
            motion = self._sources["motion"]
            self.manager.complete_source_snapshot(
                source_id=motion.source_id,
                source_epoch=motion.epoch,
                quality="good",
            )
        self._deliver(mutations)

    def motion_occurrence(
        self,
        *,
        error_details: Mapping[str, Any],
        evidence_ref: str,
        correlations: Mapping[str, Any] | None = None,
    ) -> None:
        """Count new native evidence only when its condition is already active."""

        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            pair = _runtime_pair(error_details)
            if pair is None:
                return
            scope = {
                "robot_resource": self._robot_resource,
                "contact_pair": pair,
            }
            if self._condition_states.get(
                self._key("manyforge.motion.runtime_collision", scope)
            ) != (True, "good"):
                return
            mutations.append(
                self._submit_locked(
                    definition_id="manyforge.motion.runtime_collision",
                    scope=scope,
                    active=True,
                    details={"error_details": _bounded_details(error_details)},
                    evidence_refs=(evidence_ref,),
                    correlations=correlations,
                    force_repeat=True,
                )
            )
        self._deliver(mutations)

    def observe_provider_status(
        self,
        *,
        provider_id: str,
        payload: Mapping[str, Any],
        received_at_ns: int,
    ) -> None:
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            if provider_id not in self._required_providers:
                return
            record = {
                "readiness": _bounded_text(
                    payload.get("readiness"),
                    fallback="unknown",
                    limit=128,
                ),
                "canExecute": payload.get("canExecute"),
                "userMessage": _bounded_text(
                    payload.get("userMessage"),
                    fallback="",
                    limit=1024,
                ),
                "status": _bounded_details(payload),
            }
            record["receivedAtNs"] = int(received_at_ns)
            self._provider_records[provider_id] = record
            mutations.extend(self._evaluate_providers_locked(self._monotonic_clock()))
        self._deliver(mutations)

    def observe_robot_lifecycle(
        self,
        payload: Mapping[str, Any],
    ) -> None:
        """Project one fresh provider snapshot into the generic alarm model."""

        if (
            payload.get("providerHealthy") is not True
            or payload.get("fresh") is not True
        ):
            # Stale/absent transport is owned by provider_unavailable. Never
            # clear a last-known physical condition from missing evidence.
            return
        adapter_status = payload.get("adapterStatus")
        adapter_status = (
            dict(adapter_status)
            if isinstance(adapter_status, Mapping)
            else {}
        )
        error_code = str(adapter_status.get("error_code") or "unknown")
        warning_code = str(adapter_status.get("warning_code") or "unknown")
        fault_code: str | None = None
        if payload.get("reconciliationRequired") is True:
            fault_code = "lifecycle_outcome_uncertain"
        elif payload.get("faultPresent") == "true":
            fault_code = f"xarm_error_{error_code}"
        elif payload.get("warningPresent") == "true":
            fault_code = f"xarm_warning_{warning_code}"
        elif payload.get("recoveryRequired") == "true":
            fault_code = "recovery_required"

        next_scope = (
            {
                "robot_resource": _bounded_text(
                    payload.get("robotId"),
                    fallback=self._robot_resource,
                ),
                "fault_code": _bounded_text(
                    fault_code,
                    fallback="unknown_lifecycle_fault",
                ),
            }
            if fault_code is not None
            else None
        )
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            previous = self._robot_lifecycle_fault_scope
            if previous is not None and previous != next_scope:
                mutations.append(
                    self._submit_locked(
                        definition_id=_ROBOT_LIFECYCLE_DEFINITION,
                        scope=previous,
                        active=False,
                        details={
                            "reason": "fresh_provider_snapshot_cleared_condition",
                            "snapshot_generation": payload.get(
                                "snapshotGeneration"
                            ),
                        },
                    )
                )
            if next_scope is not None and previous != next_scope:
                mutations.append(
                    self._submit_locked(
                        definition_id=_ROBOT_LIFECYCLE_DEFINITION,
                        scope=next_scope,
                        active=True,
                        details={
                            "snapshot_generation": payload.get(
                                "snapshotGeneration"
                            ),
                            "reconciliation_required": payload.get(
                                "reconciliationRequired"
                            ),
                            "fault_present": payload.get("faultPresent"),
                            "warning_present": payload.get("warningPresent"),
                            "recovery_required": payload.get(
                                "recoveryRequired"
                            ),
                            "adapter_status": _bounded_details(adapter_status),
                            "last_terminal_transaction": _bounded_detail_value(
                                payload.get("lastTerminalTransaction")
                            ),
                        },
                        evidence_refs=(
                            "robot-lifecycle:"
                            f"{payload.get('providerId') or 'unknown'}:"
                            f"{payload.get('snapshotGeneration') or 0}",
                        ),
                    )
                )
            self._robot_lifecycle_fault_scope = next_scope
        self._deliver(mutations)

    def check_provider_freshness(self, *, now_ns: int | None = None) -> None:
        mutations: list[OperatorAlarmMutation | None] = []
        with self._lock:
            mutations.extend(
                self._evaluate_providers_locked(
                    self._monotonic_clock() if now_ns is None else int(now_ns)
                )
            )
        self._deliver(mutations)

    def _provider_classification_locked(
        self,
        provider_id: str,
        *,
        now_ns: int,
    ) -> tuple[str, dict[str, Any]]:
        record = self._provider_records.get(provider_id)
        if record is None:
            if (
                now_ns - self._provider_expectation_started_ns
                >= self._provider_freshness_ns
            ):
                return "unavailable", {"reason": "provider_status_absent"}
            return "unknown", {"reason": "provider_status_pending"}
        received_at_ns = int(record.get("receivedAtNs") or 0)
        if received_at_ns <= 0 or now_ns - received_at_ns >= self._provider_freshness_ns:
            return "unavailable", {
                "reason": "provider_status_stale",
                "received_at_ns": received_at_ns,
                "freshness_ns": self._provider_freshness_ns,
            }
        readiness = str(record.get("readiness") or "").lower()
        can_execute = record.get("canExecute")
        if (
            record.get("status", {}).get("providerHealthy") is True
            or readiness == "ready" and can_execute is not False
        ):
            self._provider_waiting_since_ns.pop(provider_id, None)
            return "ready", {"reason": "provider_ready", "status": record}
        if readiness in _PROVIDER_UNAVAILABLE_STATES:
            self._provider_waiting_since_ns.pop(provider_id, None)
            return "unavailable", {
                "reason": f"provider_{readiness}",
                "status": record,
            }
        waiting_since = self._provider_waiting_since_ns.setdefault(provider_id, now_ns)
        if now_ns - waiting_since >= self._provider_freshness_ns:
            return "unavailable", {
                "reason": "provider_not_ready_timeout",
                "status": record,
            }
        return "unknown", {"reason": "provider_not_ready_pending", "status": record}

    def _evaluate_providers_locked(
        self,
        now_ns: int,
    ) -> list[OperatorAlarmMutation | None]:
        if not self._scene_ready:
            return []
        mutations: list[OperatorAlarmMutation | None] = []
        classifications: dict[str, str] = {}
        for provider_id in sorted(self._required_providers):
            classification, details = self._provider_classification_locked(
                provider_id,
                now_ns=now_ns,
            )
            classifications[provider_id] = classification
            scope = {
                "provider_id": provider_id,
                "runtime_lane": self._runtime_lane,
            }
            if classification == "unknown":
                continue
            mutations.append(
                self._submit_locked(
                    definition_id=_PROVIDER_DEFINITION,
                    scope=scope,
                    active=classification == "unavailable",
                    details=details,
                    evidence_refs=(f"provider-status:{provider_id}",),
                )
            )
        runtime = self._sources["runtime_health"]
        if all(value != "unknown" for value in classifications.values()):
            self.manager.complete_source_snapshot(
                source_id=runtime.source_id,
                source_epoch=runtime.epoch,
                quality="good",
            )
        else:
            self.manager.mark_source_quality_unknown(
                source_id=runtime.source_id,
                source_epoch=runtime.epoch,
            )
        return mutations

    def _reconcile_runtime_source_locked(self) -> None:
        if not self._required_providers:
            runtime = self._sources["runtime_health"]
            self.manager.complete_source_snapshot(
                source_id=runtime.source_id,
                source_epoch=runtime.epoch,
                quality="good",
            )

    def source_status(self) -> dict[str, Any]:
        with self._lock:
            return {
                "runtimeLane": self._runtime_lane,
                "robotResource": self._robot_resource,
                "sceneReady": self._scene_ready,
                "poseScopeCount": len(self._pose_scopes),
                "requiredProviders": sorted(self._required_providers),
                "sources": {
                    domain: {
                        "sourceId": source.source_id,
                        "epoch": source.epoch,
                        "epochOrder": source.epoch_order,
                        "sequence": source.sequence,
                    }
                    for domain, source in sorted(self._sources.items())
                },
            }
