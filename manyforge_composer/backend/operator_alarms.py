"""Typed four-state operator-alarm lifecycle and thread-safe manager.

This module has no dependency on EventRecorder, HTTP, frontend code, or robot
control. Domain adapters submit immutable condition updates; callers persist
the returned lifecycle facts after the manager lock has been released.
"""
from __future__ import annotations

from collections import Counter, OrderedDict, deque
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
import hashlib
import json
import secrets
import threading
import time
from types import MappingProxyType
from typing import Any, Callable, Iterable, Mapping
import unicodedata

from .operator_alarm_catalog import (
    OPERATOR_ALARM_CATALOG_SCHEMA_VERSION,
    OperatorAlarmCatalog,
    OperatorAlarmDefinition,
)


OPERATOR_ALARM_SCHEMA_VERSION = "manyforge.operator_alarm.v2"
OPERATOR_ALARM_SNAPSHOT_SCHEMA_VERSION = "manyforge.operator_alarm_snapshot.v2"
ALARM_STATES = frozenset(
    {
        "active_unacknowledged",
        "active_acknowledged",
        "returned_unacknowledged",
        "returned_acknowledged",
    }
)
ATTENTION_STATES = frozenset(
    {"active_unacknowledged", "returned_unacknowledged"}
)
ACTIVE_STATES = frozenset({"active_unacknowledged", "active_acknowledged"})
_PRIORITY_ORDER = {"critical": 0, "high": 1, "medium": 2, "low": 3}
_MAX_SCOPE_VALUE_BYTES = 512
_MAX_SOURCE_ID_BYTES = 512
_MAX_SOURCE_EPOCH_BYTES = 512
_MAX_REASON_BYTES = 1024
_MAX_PRINCIPAL_BYTES = 256
_MAX_DISPLAY_ARGS_BYTES = 8 * 1024
_MAX_DETAILS_BYTES = 32 * 1024
_MAX_CORRELATIONS_BYTES = 8 * 1024
_MAX_MAPPING_KEYS = 32
_MAX_EVIDENCE_REF_BYTES = 1024
_DEFAULT_TERMINAL_RETENTION = 4096
_DEFAULT_CONDITION_CAPACITY = 8192
_REQUIRED_DOMAINS = frozenset({"motion", "scene", "runtime_health"})
_METRIC_SAMPLE_RETENTION = 4096
_ACTIVATION_SAMPLE_RETENTION = 4096
_PER_CONDITION_ACTIVATION_RETENTION = 64
_ALARM_FLOOD_WINDOW_NS = 60 * 1_000_000_000
_ALARM_FLOOD_THRESHOLD = 10
_CHATTER_WINDOW_NS = 10 * 60 * 1_000_000_000
_CHATTER_THRESHOLD = 10
_MAX_ASSESSMENT_IDENTITIES = 128


class OperatorAlarmError(RuntimeError):
    """Stable validation, ordering, capacity, or acknowledgement error."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _alarm_error(code: str, message: str) -> OperatorAlarmError:
    return OperatorAlarmError(code, message)


def _nfc_text(
    value: Any,
    *,
    field_name: str,
    max_bytes: int,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str) or (not value and not allow_empty):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"{field_name} must be {qualifier}",
        )
    if value != unicodedata.normalize("NFC", value):
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"{field_name} must use Unicode NFC normalization",
        )
    if len(value.encode("utf-8")) > max_bytes:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"{field_name} exceeds {max_bytes} UTF-8 bytes",
        )
    return value


def _freeze_json(value: Any, *, field_name: str) -> Any:
    if value is None or type(value) in {bool, int, str}:
        if isinstance(value, str) and value != unicodedata.normalize("NFC", value):
            raise _alarm_error(
                "operator_alarm_condition_update_invalid",
                f"{field_name} strings must use Unicode NFC normalization",
            )
        return value
    if type(value) is float:
        if value != value or value in {float("inf"), float("-inf")}:
            raise _alarm_error(
                "operator_alarm_condition_update_invalid",
                f"{field_name} contains a non-finite number",
            )
        return value
    if isinstance(value, Mapping):
        frozen: dict[str, Any] = {}
        for key, item in value.items():
            if not isinstance(key, str) or not key:
                raise _alarm_error(
                    "operator_alarm_condition_update_invalid",
                    f"{field_name} keys must be non-empty strings",
                )
            normalized_key = _nfc_text(
                key,
                field_name=f"{field_name} key",
                max_bytes=256,
            )
            frozen[normalized_key] = _freeze_json(
                item,
                field_name=f"{field_name}.{normalized_key}",
            )
        return MappingProxyType(frozen)
    if isinstance(value, (list, tuple)):
        return tuple(
            _freeze_json(item, field_name=f"{field_name}[]") for item in value
        )
    raise _alarm_error(
        "operator_alarm_condition_update_invalid",
        f"{field_name} contains a non-JSON value",
    )


def _thaw_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw_json(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw_json(item) for item in value]
    return value


def _json_size(value: Any, *, field_name: str, max_bytes: int) -> None:
    try:
        encoded = json.dumps(
            _thaw_json(value),
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"{field_name} is not bounded JSON: {exc}",
        ) from exc
    if len(encoded) > max_bytes:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"{field_name} exceeds {max_bytes} serialized UTF-8 bytes",
        )


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _duration_summary(values: Iterable[int]) -> dict[str, int]:
    ordered = sorted(max(0, int(value)) for value in values)
    if not ordered:
        return {
            "count": 0,
            "minNs": 0,
            "p50Ns": 0,
            "p95Ns": 0,
            "p99Ns": 0,
            "maxNs": 0,
        }

    def percentile(numerator: int, denominator: int) -> int:
        rank = max(0, (len(ordered) * numerator + denominator - 1) // denominator - 1)
        return ordered[min(rank, len(ordered) - 1)]

    return {
        "count": len(ordered),
        "minNs": ordered[0],
        "p50Ns": percentile(50, 100),
        "p95Ns": percentile(95, 100),
        "p99Ns": percentile(99, 100),
        "maxNs": ordered[-1],
    }


@dataclass(frozen=True, slots=True)
class AlarmConditionUpdate:
    """Immutable update submitted by one registered domain adapter."""

    definition_id: str
    scope: Mapping[str, str]
    active: bool
    source_id: str
    source_epoch: str
    source_sequence: int
    observed_at_ns: int
    observed_at_wall: str
    quality: str = "good"
    display_args: Mapping[str, Any] = field(default_factory=dict)
    details: Mapping[str, Any] = field(default_factory=dict)
    evidence_refs: tuple[str, ...] = ()
    correlations: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "scope", MappingProxyType(dict(self.scope)))
        object.__setattr__(
            self,
            "display_args",
            _freeze_json(self.display_args, field_name="display_args"),
        )
        object.__setattr__(
            self,
            "details",
            _freeze_json(self.details, field_name="details"),
        )
        object.__setattr__(
            self,
            "correlations",
            _freeze_json(self.correlations, field_name="correlations"),
        )
        object.__setattr__(self, "evidence_refs", tuple(self.evidence_refs))


@dataclass(frozen=True, slots=True)
class AlarmSourceCursor:
    source_id: str
    source_epoch: str
    source_epoch_order: int
    source_sequence: int
    fingerprint: str
    observed_at_ns: int


@dataclass(frozen=True, slots=True)
class OperatorAlarmRecord:
    catalog_schema_version: str
    catalog_version: str
    definition_version: int
    definition_id: str
    condition_id: str
    activation_id: str
    scope: Mapping[str, str]
    alarm_class: str
    priority: str
    title: str
    message: str
    consequence: str
    operator_response: str
    requires_motion_attention: bool
    lifecycle_state: str
    active: bool
    acknowledged: bool
    activation_count: int
    occurrence_count: int
    record_revision: int
    activated_at_ns: int
    activated_at_wall: str
    first_seen_at_ns: int
    first_seen_at_wall: str
    last_seen_at_ns: int
    last_seen_at_wall: str
    returned_at_ns: int | None
    returned_at_wall: str | None
    acknowledged_at_ns: int | None
    acknowledged_at_wall: str | None
    acknowledgement_principal: str | None
    acknowledgement_reason: str | None
    source_id: str
    source_epoch: str
    source_sequence: int
    source_quality: str
    display_args: Mapping[str, Any]
    details: Mapping[str, Any]
    evidence_refs: tuple[str, ...]
    correlations: Mapping[str, Any]

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": OPERATOR_ALARM_SCHEMA_VERSION,
            "catalogSchemaVersion": self.catalog_schema_version,
            "catalogVersion": self.catalog_version,
            "definitionVersion": self.definition_version,
            "definitionId": self.definition_id,
            "conditionId": self.condition_id,
            "activationId": self.activation_id,
            "scope": dict(self.scope),
            "alarmClass": self.alarm_class,
            "priority": self.priority,
            "title": self.title,
            "message": self.message,
            "consequence": self.consequence,
            "operatorResponse": self.operator_response,
            "requiresMotionAttention": self.requires_motion_attention,
            "lifecycleState": self.lifecycle_state,
            "active": self.active,
            "acknowledged": self.acknowledged,
            "activationCount": self.activation_count,
            "occurrenceCount": self.occurrence_count,
            "recordRevision": self.record_revision,
            "activatedAtNs": self.activated_at_ns,
            "activatedAt": self.activated_at_wall,
            "firstSeenAtNs": self.first_seen_at_ns,
            "firstSeenAt": self.first_seen_at_wall,
            "lastSeenAtNs": self.last_seen_at_ns,
            "lastSeenAt": self.last_seen_at_wall,
            "returnedAtNs": self.returned_at_ns,
            "returnedAt": self.returned_at_wall,
            "acknowledgedAtNs": self.acknowledged_at_ns,
            "acknowledgedAt": self.acknowledged_at_wall,
            "acknowledgementPrincipal": self.acknowledgement_principal,
            "acknowledgementReason": self.acknowledgement_reason,
            "sourceId": self.source_id,
            "sourceEpoch": self.source_epoch,
            "sourceSequence": self.source_sequence,
            "sourceQuality": self.source_quality,
            "displayArgs": _thaw_json(self.display_args),
            "details": _thaw_json(self.details),
            "evidenceRefs": list(self.evidence_refs),
            "correlations": _thaw_json(self.correlations),
        }


@dataclass(frozen=True, slots=True)
class AlarmPointState:
    condition_id: str
    activation_count: int = 0
    record: OperatorAlarmRecord | None = None
    cursor: AlarmSourceCursor | None = None


@dataclass(frozen=True, slots=True)
class AlarmLifecycleFact:
    event_type: str
    definition_id: str | None
    condition_id: str | None
    activation_id: str | None
    previous_state: str | None
    current_state: str | None
    record_revision: int | None
    manager_generation: int
    occurred_at_ns: int
    occurred_at_wall: str
    source_id: str | None = None
    source_epoch: str | None = None
    source_sequence: int | None = None
    principal_id: str | None = None
    reason: str | None = None
    correlations: Mapping[str, Any] = field(default_factory=dict)
    details: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "correlations",
            _freeze_json(self.correlations, field_name="fact.correlations"),
        )
        object.__setattr__(
            self,
            "details",
            _freeze_json(self.details, field_name="fact.details"),
        )

    def to_event_details(self) -> dict[str, Any]:
        return {
            "definition_id": self.definition_id,
            "condition_id": self.condition_id,
            "activation_id": self.activation_id,
            "previous_state": self.previous_state,
            "current_state": self.current_state,
            "record_revision": self.record_revision,
            "manager_generation": self.manager_generation,
            "source_id": self.source_id,
            "source_epoch": self.source_epoch,
            "source_sequence": self.source_sequence,
            "principal_id": self.principal_id,
            "reason": self.reason,
            "occurred_at_ns": self.occurred_at_ns,
            "occurred_at": self.occurred_at_wall,
            "correlations": _thaw_json(self.correlations),
            **_thaw_json(self.details),
        }


@dataclass(frozen=True, slots=True)
class AlarmTransitionResult:
    point: AlarmPointState
    status: str
    changed: bool
    membership_changed: bool = False
    activation_changed: bool = False
    lifecycle_facts: tuple[AlarmLifecycleFact, ...] = ()
    terminal_record: OperatorAlarmRecord | None = None


@dataclass(frozen=True, slots=True)
class OperatorAlarmMutation:
    status: str
    changed: bool
    manager_generation: int
    records: tuple[OperatorAlarmRecord, ...]
    lifecycle_facts: tuple[AlarmLifecycleFact, ...]


@dataclass(frozen=True, slots=True, order=True)
class AlarmAcknowledgementMember:
    condition_id: str
    activation_id: str
    record_revision: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "conditionId": self.condition_id,
            "activationId": self.activation_id,
            "recordRevision": self.record_revision,
        }


@dataclass(frozen=True, slots=True)
class AlarmSourceRegistration:
    domain: str
    source_id: str
    adapter_id: str
    source_epoch: str
    source_epoch_order: int
    quality: str = "unknown"
    snapshot_complete: bool = False


def _validate_update(
    catalog: OperatorAlarmCatalog,
    definition: OperatorAlarmDefinition,
    update: AlarmConditionUpdate,
) -> None:
    del catalog
    _nfc_text(
        update.definition_id,
        field_name="definition_id",
        max_bytes=_MAX_SOURCE_ID_BYTES,
    )
    expected_scope = set(definition.scope_fields)
    actual_scope = set(update.scope)
    if actual_scope != expected_scope:
        raise _alarm_error(
            "operator_alarm_scope_invalid",
            f"{definition.definition_id} scope must contain exactly "
            f"{list(definition.scope_fields)}",
        )
    for field_name in definition.scope_fields:
        _nfc_text(
            update.scope[field_name],
            field_name=f"scope.{field_name}",
            max_bytes=_MAX_SCOPE_VALUE_BYTES,
        )
    if type(update.active) is not bool:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            "active must be boolean",
        )
    _nfc_text(
        update.source_id,
        field_name="source_id",
        max_bytes=_MAX_SOURCE_ID_BYTES,
    )
    _nfc_text(
        update.source_epoch,
        field_name="source_epoch",
        max_bytes=_MAX_SOURCE_EPOCH_BYTES,
    )
    if type(update.source_sequence) is not int or update.source_sequence < 0:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            "source_sequence must be a non-negative integer",
        )
    if type(update.observed_at_ns) is not int or update.observed_at_ns < 0:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            "observed_at_ns must be a non-negative integer",
        )
    _nfc_text(
        update.observed_at_wall,
        field_name="observed_at_wall",
        max_bytes=256,
    )
    if update.quality not in {"good", "unknown"}:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            "quality must be 'good' or 'unknown'",
        )
    if len(update.display_args) > _MAX_MAPPING_KEYS:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"display_args may contain at most {_MAX_MAPPING_KEYS} keys",
        )
    if len(update.correlations) > _MAX_MAPPING_KEYS:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"correlations may contain at most {_MAX_MAPPING_KEYS} keys",
        )
    _json_size(
        update.display_args,
        field_name="display_args",
        max_bytes=_MAX_DISPLAY_ARGS_BYTES,
    )
    _json_size(update.details, field_name="details", max_bytes=_MAX_DETAILS_BYTES)
    _json_size(
        update.correlations,
        field_name="correlations",
        max_bytes=_MAX_CORRELATIONS_BYTES,
    )
    if len(update.evidence_refs) > definition.max_evidence_refs:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"evidence_refs exceeds catalog limit {definition.max_evidence_refs}",
        )
    if len(update.evidence_refs) != len(set(update.evidence_refs)):
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            "evidence_refs must be unique",
        )
    for index, evidence_ref in enumerate(update.evidence_refs):
        _nfc_text(
            evidence_ref,
            field_name=f"evidence_refs[{index}]",
            max_bytes=_MAX_EVIDENCE_REF_BYTES,
        )


def canonical_condition_id(
    catalog: OperatorAlarmCatalog,
    definition: OperatorAlarmDefinition,
    scope: Mapping[str, str],
) -> str:
    """Derive the stable opaque condition id using byte-length framing."""

    values = [
        catalog.schema_version,
        definition.definition_id,
        str(definition.version),
    ]
    for field_name in definition.scope_fields:
        values.extend((field_name, scope[field_name]))
    framed = bytearray()
    for value in values:
        encoded = value.encode("utf-8")
        framed.extend(str(len(encoded)).encode("ascii"))
        framed.extend(b":")
        framed.extend(encoded)
        framed.extend(b";")
    digest = hashlib.sha256(bytes(framed)).hexdigest()[:24]
    return f"condition-{digest}"


def _update_fingerprint(update: AlarmConditionUpdate) -> str:
    value = {
        "definition_id": update.definition_id,
        "scope": dict(update.scope),
        "active": update.active,
        "source_id": update.source_id,
        "source_epoch": update.source_epoch,
        "source_sequence": update.source_sequence,
        "observed_at_ns": update.observed_at_ns,
        "observed_at_wall": update.observed_at_wall,
        "quality": update.quality,
        "display_args": _thaw_json(update.display_args),
        "details": _thaw_json(update.details),
        "evidence_refs": list(update.evidence_refs),
        "correlations": _thaw_json(update.correlations),
    }
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _next_cursor(
    update: AlarmConditionUpdate,
    *,
    source_epoch_order: int,
    fingerprint: str,
) -> AlarmSourceCursor:
    return AlarmSourceCursor(
        source_id=update.source_id,
        source_epoch=update.source_epoch,
        source_epoch_order=source_epoch_order,
        source_sequence=update.source_sequence,
        fingerprint=fingerprint,
        observed_at_ns=update.observed_at_ns,
    )


def _validate_ordering(
    point: AlarmPointState,
    update: AlarmConditionUpdate,
    *,
    source_epoch_order: int,
    fingerprint: str,
) -> str:
    cursor = point.cursor
    if cursor is None:
        return "new"
    if update.source_id != cursor.source_id:
        raise _alarm_error(
            "operator_alarm_source_authority_conflict",
            "A different source authority cannot update the current condition",
        )
    if source_epoch_order < cursor.source_epoch_order:
        raise _alarm_error(
            "operator_alarm_source_epoch_stale",
            "Condition update belongs to an older source epoch",
        )
    if source_epoch_order == cursor.source_epoch_order:
        if update.source_epoch != cursor.source_epoch:
            raise _alarm_error(
                "operator_alarm_source_epoch_conflict",
                "One source epoch order cannot name two epochs",
            )
        if update.source_sequence < cursor.source_sequence:
            raise _alarm_error(
                "operator_alarm_source_sequence_stale",
                "Condition update sequence is older than the accepted sequence",
            )
        if update.source_sequence == cursor.source_sequence:
            if fingerprint == cursor.fingerprint:
                return "duplicate"
            raise _alarm_error(
                "operator_alarm_source_sequence_conflict",
                "One source sequence contains conflicting condition data",
            )
    return "new"


def _message(
    definition: OperatorAlarmDefinition,
    update: AlarmConditionUpdate,
) -> str:
    arguments = dict(update.scope)
    arguments.update(_thaw_json(update.display_args))
    try:
        rendered = definition.message_template.format_map(arguments)
    except (KeyError, ValueError) as exc:
        raise _alarm_error(
            "operator_alarm_condition_update_invalid",
            f"Could not render {definition.definition_id} message: {exc}",
        ) from exc
    return _nfc_text(
        rendered,
        field_name="rendered message",
        max_bytes=1024,
    )


def _merge_evidence(
    existing: tuple[str, ...],
    incoming: tuple[str, ...],
    *,
    limit: int,
) -> tuple[str, ...]:
    merged = list(existing)
    for value in incoming:
        if value not in merged:
            merged.append(value)
    return tuple(merged[-limit:])


def _merge_mapping(
    existing: Mapping[str, Any],
    incoming: Mapping[str, Any],
    *,
    field_name: str,
    max_bytes: int,
    max_keys: int | None = None,
) -> Mapping[str, Any]:
    if not incoming:
        return existing
    merged = _thaw_json(existing)
    merged.update(_thaw_json(incoming))
    frozen = _freeze_json(merged, field_name=field_name)
    if max_keys is not None and len(frozen) > max_keys:
        return incoming
    try:
        _json_size(frozen, field_name=field_name, max_bytes=max_bytes)
    except OperatorAlarmError:
        # The incoming update was validated independently. Prefer its latest
        # bounded payload over allowing accumulated context to grow without
        # bound across repeated observations.
        return incoming
    return frozen


def _lifecycle_fact(
    event_type: str,
    record: OperatorAlarmRecord,
    *,
    previous_state: str | None,
    occurred_at_ns: int,
    occurred_at_wall: str,
    principal_id: str | None = None,
    reason: str | None = None,
) -> AlarmLifecycleFact:
    return AlarmLifecycleFact(
        event_type=event_type,
        definition_id=record.definition_id,
        condition_id=record.condition_id,
        activation_id=record.activation_id,
        previous_state=previous_state,
        current_state=record.lifecycle_state,
        record_revision=record.record_revision,
        manager_generation=0,
        occurred_at_ns=occurred_at_ns,
        occurred_at_wall=occurred_at_wall,
        source_id=record.source_id,
        source_epoch=record.source_epoch,
        source_sequence=record.source_sequence,
        principal_id=principal_id,
        reason=reason,
        correlations=record.correlations,
    )


def _new_activation_record(
    catalog: OperatorAlarmCatalog,
    definition: OperatorAlarmDefinition,
    point: AlarmPointState,
    update: AlarmConditionUpdate,
    *,
    activation_id: str,
) -> OperatorAlarmRecord:
    previous_revision = point.record.record_revision if point.record is not None else 0
    activation_count = point.activation_count + 1
    return OperatorAlarmRecord(
        catalog_schema_version=catalog.schema_version,
        catalog_version=catalog.catalog_version,
        definition_version=definition.version,
        definition_id=definition.definition_id,
        condition_id=point.condition_id,
        activation_id=activation_id,
        scope=MappingProxyType(dict(update.scope)),
        alarm_class=definition.alarm_class,
        priority=definition.priority,
        title=definition.title,
        message=_message(definition, update),
        consequence=definition.consequence,
        operator_response=definition.operator_response,
        requires_motion_attention=definition.requires_motion_attention,
        lifecycle_state="active_unacknowledged",
        active=True,
        acknowledged=False,
        activation_count=activation_count,
        occurrence_count=1,
        record_revision=previous_revision + 1,
        activated_at_ns=update.observed_at_ns,
        activated_at_wall=update.observed_at_wall,
        first_seen_at_ns=update.observed_at_ns,
        first_seen_at_wall=update.observed_at_wall,
        last_seen_at_ns=update.observed_at_ns,
        last_seen_at_wall=update.observed_at_wall,
        returned_at_ns=None,
        returned_at_wall=None,
        acknowledged_at_ns=None,
        acknowledged_at_wall=None,
        acknowledgement_principal=None,
        acknowledgement_reason=None,
        source_id=update.source_id,
        source_epoch=update.source_epoch,
        source_sequence=update.source_sequence,
        source_quality=update.quality,
        display_args=update.display_args,
        details=update.details,
        evidence_refs=update.evidence_refs,
        correlations=update.correlations,
    )


def reduce_condition_update(
    catalog: OperatorAlarmCatalog,
    definition: OperatorAlarmDefinition,
    point: AlarmPointState,
    update: AlarmConditionUpdate,
    *,
    source_epoch_order: int,
    activation_id: str,
) -> AlarmTransitionResult:
    """Pure lifecycle reducer for one validated condition update."""

    _validate_update(catalog, definition, update)
    expected_condition_id = canonical_condition_id(catalog, definition, update.scope)
    if point.condition_id != expected_condition_id:
        raise _alarm_error(
            "operator_alarm_condition_identity_mismatch",
            "Condition point does not match the update's canonical identity",
        )
    fingerprint = _update_fingerprint(update)
    order = _validate_ordering(
        point,
        update,
        source_epoch_order=source_epoch_order,
        fingerprint=fingerprint,
    )
    if order == "duplicate":
        return AlarmTransitionResult(point=point, status="duplicate", changed=False)
    cursor = _next_cursor(
        update,
        source_epoch_order=source_epoch_order,
        fingerprint=fingerprint,
    )
    record = point.record

    if not update.active:
        if update.quality == "unknown" and record is not None and record.active:
            raise _alarm_error(
                "operator_alarm_clear_quality_unknown",
                "Unknown source quality cannot clear an active condition",
            )
        if record is None or record.lifecycle_state in {
            "returned_unacknowledged",
            "returned_acknowledged",
        }:
            return AlarmTransitionResult(
                point=replace(point, cursor=cursor),
                status="normal_observed",
                changed=True,
            )
        previous_state = record.lifecycle_state
        acknowledged = record.acknowledged
        next_state = (
            "returned_acknowledged"
            if acknowledged
            else "returned_unacknowledged"
        )
        returned = replace(
            record,
            lifecycle_state=next_state,
            active=False,
            record_revision=record.record_revision + 1,
            returned_at_ns=update.observed_at_ns,
            returned_at_wall=update.observed_at_wall,
            source_id=update.source_id,
            source_epoch=update.source_epoch,
            source_sequence=update.source_sequence,
            source_quality=update.quality,
            details=_merge_mapping(
                record.details,
                update.details,
                field_name="details",
                max_bytes=_MAX_DETAILS_BYTES,
            ),
            evidence_refs=_merge_evidence(
                record.evidence_refs,
                update.evidence_refs,
                limit=definition.max_evidence_refs,
            ),
            correlations=_merge_mapping(
                record.correlations,
                update.correlations,
                field_name="correlations",
                max_bytes=_MAX_CORRELATIONS_BYTES,
                max_keys=_MAX_MAPPING_KEYS,
            ),
        )
        fact = _lifecycle_fact(
            "operator_alarm_returned",
            returned,
            previous_state=previous_state,
            occurred_at_ns=update.observed_at_ns,
            occurred_at_wall=update.observed_at_wall,
        )
        return AlarmTransitionResult(
            point=AlarmPointState(
                condition_id=point.condition_id,
                activation_count=point.activation_count,
                record=returned,
                cursor=cursor,
            ),
            status="returned",
            changed=True,
            lifecycle_facts=(fact,),
            terminal_record=returned if acknowledged else None,
        )

    if record is None or record.lifecycle_state in {
        "returned_unacknowledged",
        "returned_acknowledged",
    }:
        previous_state = record.lifecycle_state if record is not None else None
        activated = _new_activation_record(
            catalog,
            definition,
            point,
            update,
            activation_id=activation_id,
        )
        fact = _lifecycle_fact(
            "operator_alarm_activated",
            activated,
            previous_state=previous_state,
            occurred_at_ns=update.observed_at_ns,
            occurred_at_wall=update.observed_at_wall,
        )
        return AlarmTransitionResult(
            point=AlarmPointState(
                condition_id=point.condition_id,
                activation_count=activated.activation_count,
                record=activated,
                cursor=cursor,
            ),
            status="activated" if previous_state is None else "recurred",
            changed=True,
            membership_changed=previous_state != "returned_unacknowledged",
            activation_changed=True,
            lifecycle_facts=(fact,),
        )

    last_seen_at_ns = record.last_seen_at_ns
    last_seen_at_wall = record.last_seen_at_wall
    if update.observed_at_ns >= record.last_seen_at_ns:
        last_seen_at_ns = update.observed_at_ns
        last_seen_at_wall = update.observed_at_wall
    repeated = replace(
        record,
        message=_message(definition, update),
        occurrence_count=record.occurrence_count + 1,
        record_revision=record.record_revision + 1,
        last_seen_at_ns=last_seen_at_ns,
        last_seen_at_wall=last_seen_at_wall,
        source_epoch=update.source_epoch,
        source_sequence=update.source_sequence,
        source_quality=update.quality,
        display_args=_merge_mapping(
            record.display_args,
            update.display_args,
            field_name="display_args",
            max_bytes=_MAX_DISPLAY_ARGS_BYTES,
            max_keys=_MAX_MAPPING_KEYS,
        ),
        details=_merge_mapping(
            record.details,
            update.details,
            field_name="details",
            max_bytes=_MAX_DETAILS_BYTES,
        ),
        evidence_refs=_merge_evidence(
            record.evidence_refs,
            update.evidence_refs,
            limit=definition.max_evidence_refs,
        ),
        correlations=_merge_mapping(
            record.correlations,
            update.correlations,
            field_name="correlations",
            max_bytes=_MAX_CORRELATIONS_BYTES,
            max_keys=_MAX_MAPPING_KEYS,
        ),
    )
    return AlarmTransitionResult(
        point=AlarmPointState(
            condition_id=point.condition_id,
            activation_count=point.activation_count,
            record=repeated,
            cursor=cursor,
        ),
        status="active_repeat",
        changed=True,
    )


def reduce_acknowledgement(
    definition: OperatorAlarmDefinition,
    point: AlarmPointState,
    *,
    activation_id: str,
    expected_record_revision: int,
    principal_id: str,
    reason: str | None,
    acknowledged_at_ns: int,
    acknowledged_at_wall: str,
) -> AlarmTransitionResult:
    """Pure exact-activation acknowledgement reducer."""

    record = point.record
    if record is None:
        raise _alarm_error(
            "operator_alarm_not_found",
            f"Unknown operator alarm condition: {point.condition_id}",
        )
    if record.activation_id != activation_id:
        raise _alarm_error(
            "operator_alarm_activation_stale",
            "The alarm activation changed before acknowledgement",
        )
    if record.acknowledged:
        return AlarmTransitionResult(
            point=point,
            status="already_acknowledged",
            changed=False,
        )
    if record.record_revision != expected_record_revision:
        raise _alarm_error(
            "operator_alarm_record_revision_stale",
            "The alarm record changed before acknowledgement",
        )
    if definition.acknowledgement_reason_required and not reason:
        raise _alarm_error(
            "operator_alarm_acknowledgement_reason_required",
            "This alarm definition requires an acknowledgement reason",
        )
    previous_state = record.lifecycle_state
    next_state = (
        "active_acknowledged" if record.active else "returned_acknowledged"
    )
    acknowledged = replace(
        record,
        lifecycle_state=next_state,
        acknowledged=True,
        record_revision=record.record_revision + 1,
        acknowledged_at_ns=acknowledged_at_ns,
        acknowledged_at_wall=acknowledged_at_wall,
        acknowledgement_principal=principal_id,
        acknowledgement_reason=reason,
    )
    fact = _lifecycle_fact(
        "operator_alarm_acknowledged",
        acknowledged,
        previous_state=previous_state,
        occurred_at_ns=acknowledged_at_ns,
        occurred_at_wall=acknowledged_at_wall,
        principal_id=principal_id,
        reason=reason,
    )
    return AlarmTransitionResult(
        point=replace(point, record=acknowledged),
        status="acknowledged",
        changed=True,
        membership_changed=True,
        lifecycle_facts=(fact,),
        terminal_record=acknowledged if not acknowledged.active else None,
    )


class OperatorAlarmManager:
    """Single-lock manager for current alarm points and bounded chronology."""

    def __init__(
        self,
        catalog: OperatorAlarmCatalog,
        *,
        required_domains: Iterable[str] = _REQUIRED_DOMAINS,
        terminal_retention: int = _DEFAULT_TERMINAL_RETENTION,
        condition_capacity: int = _DEFAULT_CONDITION_CAPACITY,
        monotonic_clock: Callable[[], int] = time.monotonic_ns,
        wall_clock: Callable[[], str] = _utc_now,
        activation_factory: Callable[[], str] | None = None,
    ) -> None:
        if terminal_retention < 1:
            raise ValueError("terminal_retention must be positive")
        if condition_capacity < 1:
            raise ValueError("condition_capacity must be positive")
        required = frozenset(str(domain) for domain in required_domains)
        if not required or any(not domain for domain in required):
            raise ValueError("required_domains must contain non-empty names")
        self.catalog = catalog
        self._required_domains = required
        self._terminal_retention = int(terminal_retention)
        self._condition_capacity = int(condition_capacity)
        self._monotonic_clock = monotonic_clock
        self._wall_clock = wall_clock
        self._activation_factory = activation_factory or (
            lambda: f"activation-{secrets.token_hex(12)}"
        )
        self._lock = threading.Lock()
        self._points: OrderedDict[str, AlarmPointState] = OrderedDict()
        self._terminal_records: deque[OperatorAlarmRecord] = deque()
        self._sources: dict[str, AlarmSourceRegistration] = {}
        self._generation = 0
        self._state_revision = 0
        self._activation_serial = 0
        self._health_errors: dict[str, str] = {}
        self._metrics: Counter[str] = Counter()
        self._acknowledgement_latency_ns: deque[int] = deque(
            maxlen=_METRIC_SAMPLE_RETENTION
        )
        self._completed_standing_duration_ns: deque[int] = deque(
            maxlen=_METRIC_SAMPLE_RETENTION
        )
        self._ingest_latency_ns: deque[int] = deque(
            maxlen=_METRIC_SAMPLE_RETENTION
        )
        self._recent_activations: deque[int] = deque(
            maxlen=_ACTIVATION_SAMPLE_RETENTION
        )
        self._condition_activations: dict[str, deque[int]] = {}

    @classmethod
    def unavailable(cls, *, code: str, message: str) -> "OperatorAlarmManager":
        """Build a read-capable, fail-closed manager after catalog failure."""

        manager = cls(
            OperatorAlarmCatalog(
                schema_version=OPERATOR_ALARM_CATALOG_SCHEMA_VERSION,
                catalog_version="unavailable",
                definitions=MappingProxyType({}),
                sha256="",
            )
        )
        manager.mark_health_unavailable(code=code, message=message)
        manager._metrics["catalog_validation_failures"] += 1
        return manager

    @property
    def required_domains(self) -> frozenset[str]:
        return self._required_domains

    def register_source(
        self,
        *,
        domain: str,
        source_id: str,
        adapter_id: str,
        source_epoch: str,
        source_epoch_order: int,
    ) -> AlarmSourceRegistration:
        domain = _nfc_text(
            domain,
            field_name="domain",
            max_bytes=128,
        )
        source_id = _nfc_text(
            source_id,
            field_name="source_id",
            max_bytes=_MAX_SOURCE_ID_BYTES,
        )
        adapter_id = _nfc_text(
            adapter_id,
            field_name="adapter_id",
            max_bytes=_MAX_SOURCE_ID_BYTES,
        )
        source_epoch = _nfc_text(
            source_epoch,
            field_name="source_epoch",
            max_bytes=_MAX_SOURCE_EPOCH_BYTES,
        )
        if type(source_epoch_order) is not int or source_epoch_order < 0:
            raise _alarm_error(
                "operator_alarm_source_registration_invalid",
                "source_epoch_order must be a non-negative integer",
            )
        catalog_adapters = {
            definition.adapter_id for definition in self.catalog.definitions.values()
        }
        if adapter_id not in catalog_adapters:
            raise _alarm_error(
                "operator_alarm_source_adapter_unknown",
                f"No catalog definition uses adapter {adapter_id}",
            )
        adapter_domains = {
            definition.alarm_class
            for definition in self.catalog.definitions.values()
            if definition.adapter_id == adapter_id
        }
        if domain not in adapter_domains:
            raise _alarm_error(
                "operator_alarm_source_domain_mismatch",
                f"Adapter {adapter_id} cannot register for domain {domain}",
            )
        registration = AlarmSourceRegistration(
            domain=domain,
            source_id=source_id,
            adapter_id=adapter_id,
            source_epoch=source_epoch,
            source_epoch_order=source_epoch_order,
        )
        with self._lock:
            previous = self._sources.get(source_id)
            if previous is not None:
                if previous.domain != domain or previous.adapter_id != adapter_id:
                    raise _alarm_error(
                        "operator_alarm_source_registration_conflict",
                        f"Source {source_id} changed domain or adapter authority",
                    )
                if source_epoch_order < previous.source_epoch_order:
                    raise _alarm_error(
                        "operator_alarm_source_epoch_stale",
                        f"Source {source_id} registered an older epoch",
                    )
                if source_epoch_order == previous.source_epoch_order:
                    if source_epoch != previous.source_epoch:
                        raise _alarm_error(
                            "operator_alarm_source_epoch_conflict",
                            f"Source {source_id} reused an epoch order",
                        )
                    return previous
            self._sources[source_id] = registration
            self._state_revision += 1
            return registration

    def complete_source_snapshot(
        self,
        *,
        source_id: str,
        source_epoch: str,
        quality: str = "good",
    ) -> AlarmSourceRegistration:
        if quality not in {"good", "unknown"}:
            raise _alarm_error(
                "operator_alarm_source_quality_invalid",
                "Source quality must be 'good' or 'unknown'",
            )
        with self._lock:
            registration = self._sources.get(source_id)
            if registration is None:
                raise _alarm_error(
                    "operator_alarm_source_unregistered",
                    f"Unknown alarm source: {source_id}",
                )
            if registration.source_epoch != source_epoch:
                raise _alarm_error(
                    "operator_alarm_source_epoch_stale",
                    f"Source {source_id} snapshot belongs to a stale epoch",
                )
            updated = replace(
                registration,
                quality=quality,
                snapshot_complete=True,
            )
            self._sources[source_id] = updated
            if quality == "unknown":
                self._metrics["source_quality_unknown_observations"] += 1
            self._state_revision += 1
            return updated

    def mark_source_quality_unknown(
        self, *, source_id: str, source_epoch: str
    ) -> AlarmSourceRegistration:
        with self._lock:
            registration = self._sources.get(source_id)
            if registration is None:
                raise _alarm_error(
                    "operator_alarm_source_unregistered",
                    f"Unknown alarm source: {source_id}",
                )
            if registration.source_epoch != source_epoch:
                raise _alarm_error(
                    "operator_alarm_source_epoch_stale",
                    f"Source {source_id} quality belongs to a stale epoch",
                )
            updated = replace(
                registration,
                quality="unknown",
                snapshot_complete=False,
            )
            self._sources[source_id] = updated
            self._metrics["source_quality_unknown_observations"] += 1
            self._state_revision += 1
            return updated

    def _prune_condition_for_capacity_locked(self) -> bool:
        for condition_id, point in tuple(self._points.items()):
            if point.record is None or point.record.lifecycle_state == (
                "returned_acknowledged"
            ):
                self._points.pop(condition_id, None)
                self._condition_activations.pop(condition_id, None)
                return True
        return False

    def _ensure_condition_capacity_locked(self, condition_id: str) -> None:
        if condition_id in self._points:
            return
        while len(self._points) >= self._condition_capacity:
            if self._prune_condition_for_capacity_locked():
                continue
            self._health_errors["operator_alarm_condition_capacity_exhausted"] = (
                "Operator-alarm condition capacity is exhausted; existing live "
                "attention was retained"
            )
            self._metrics["capacity_rejections"] += 1
            self._state_revision += 1
            raise _alarm_error(
                "operator_alarm_condition_capacity_exhausted",
                "Operator-alarm condition capacity is exhausted",
            )

    def _append_terminal_locked(self, record: OperatorAlarmRecord) -> None:
        self._terminal_records.append(record)
        while len(self._terminal_records) > self._terminal_retention:
            evicted = self._terminal_records.popleft()
            point = self._points.get(evicted.condition_id)
            if (
                point is not None
                and point.record is not None
                and point.record.activation_id == evicted.activation_id
                and point.record.lifecycle_state == "returned_acknowledged"
            ):
                self._points[evicted.condition_id] = replace(point, record=None)

    def _stamp_facts_locked(
        self, facts: tuple[AlarmLifecycleFact, ...]
    ) -> tuple[AlarmLifecycleFact, ...]:
        return tuple(
            replace(fact, manager_generation=self._generation) for fact in facts
        )

    def submit(self, update: AlarmConditionUpdate) -> OperatorAlarmMutation:
        ingest_started_ns = time.perf_counter_ns()
        definition = self.catalog.definition(update.definition_id)
        _validate_update(self.catalog, definition, update)
        condition_id = canonical_condition_id(self.catalog, definition, update.scope)
        activation_seed = self._activation_factory()
        activation_seed = _nfc_text(
            activation_seed,
            field_name="activation seed",
            max_bytes=220,
        )
        with self._lock:
            registration = self._sources.get(update.source_id)
            if registration is None:
                self._metrics["rejected_updates"] += 1
                raise _alarm_error(
                    "operator_alarm_source_unregistered",
                    f"Unknown alarm source: {update.source_id}",
                )
            if registration.adapter_id != definition.adapter_id:
                self._metrics["rejected_updates"] += 1
                raise _alarm_error(
                    "operator_alarm_source_adapter_mismatch",
                    f"Source {update.source_id} cannot update {definition.definition_id}",
                )
            if registration.source_epoch != update.source_epoch:
                self._metrics["stale_updates"] += 1
                self._metrics["stale_epoch_updates"] += 1
                raise _alarm_error(
                    "operator_alarm_source_epoch_stale",
                    "Condition update does not belong to the registered current epoch",
                )
            self._ensure_condition_capacity_locked(condition_id)
            point = self._points.get(
                condition_id,
                AlarmPointState(condition_id=condition_id),
            )
            activation_id = f"{activation_seed}-{self._activation_serial + 1}"
            try:
                transition = reduce_condition_update(
                    self.catalog,
                    definition,
                    point,
                    update,
                    source_epoch_order=registration.source_epoch_order,
                    activation_id=activation_id,
                )
            except OperatorAlarmError as exc:
                self._metrics["rejected_updates"] += 1
                if exc.code in {
                    "operator_alarm_source_epoch_stale",
                    "operator_alarm_source_sequence_stale",
                }:
                    self._metrics["stale_updates"] += 1
                if exc.code == "operator_alarm_source_epoch_stale":
                    self._metrics["stale_epoch_updates"] += 1
                if exc.code == "operator_alarm_source_sequence_stale":
                    self._metrics["out_of_order_sequence_updates"] += 1
                if exc.code in {
                    "operator_alarm_source_epoch_conflict",
                    "operator_alarm_source_sequence_conflict",
                    "operator_alarm_source_authority_conflict",
                }:
                    self._metrics["conflicting_updates"] += 1
                raise
            if not transition.changed:
                self._metrics["duplicate_updates"] += 1
                self._ingest_latency_ns.append(
                    time.perf_counter_ns() - ingest_started_ns
                )
                record = point.record
                return OperatorAlarmMutation(
                    status=transition.status,
                    changed=False,
                    manager_generation=self._generation,
                    records=(record,) if record is not None else (),
                    lifecycle_facts=(),
                )
            self._points[condition_id] = transition.point
            self._points.move_to_end(condition_id)
            if transition.activation_changed:
                self._activation_serial += 1
            if transition.membership_changed or transition.activation_changed:
                self._generation += 1
            self._state_revision += 1
            if transition.terminal_record is not None:
                self._append_terminal_locked(transition.terminal_record)
            if update.quality == "unknown":
                self._sources[update.source_id] = replace(
                    registration,
                    quality="unknown",
                    snapshot_complete=False,
                )
                self._metrics["source_quality_unknown_observations"] += 1
            self._metrics["accepted_updates"] += 1
            if transition.status in {"activated", "recurred"}:
                self._metrics["activations"] += 1
                self._recent_activations.append(update.observed_at_ns)
                condition_activations = self._condition_activations.setdefault(
                    condition_id,
                    deque(maxlen=_PER_CONDITION_ACTIVATION_RETENTION),
                )
                condition_activations.append(update.observed_at_ns)
                if transition.status == "recurred":
                    self._metrics["recurrences"] += 1
            elif transition.status == "returned":
                self._metrics["returns"] += 1
                previous_record = point.record
                if (
                    previous_record is not None
                    and previous_record.lifecycle_state == "active_acknowledged"
                    and previous_record.acknowledged_at_ns is not None
                ):
                    self._completed_standing_duration_ns.append(
                        max(0, update.observed_at_ns - previous_record.acknowledged_at_ns)
                    )
            elif transition.status == "active_repeat":
                self._metrics["repeat_occurrences"] += 1
            self._metrics["peak_live_condition_points"] = max(
                self._metrics["peak_live_condition_points"],
                len(self._points),
            )
            self._ingest_latency_ns.append(
                time.perf_counter_ns() - ingest_started_ns
            )
            facts = self._stamp_facts_locked(transition.lifecycle_facts)
            record = transition.point.record
            return OperatorAlarmMutation(
                status=transition.status,
                changed=True,
                manager_generation=self._generation,
                records=(record,) if record is not None else (),
                lifecycle_facts=facts,
            )

    def acknowledge(
        self,
        *,
        condition_id: str,
        activation_id: str,
        expected_record_revision: int,
        principal_id: str,
        reason: str | None = None,
        confirmed: bool,
    ) -> OperatorAlarmMutation:
        if not confirmed:
            raise _alarm_error(
                "operator_alarm_acknowledgement_confirmation_required",
                "Alarm acknowledgement requires explicit confirmation",
            )
        principal_id = _nfc_text(
            principal_id,
            field_name="principal_id",
            max_bytes=_MAX_PRINCIPAL_BYTES,
        )
        activation_id = _nfc_text(
            activation_id,
            field_name="activation_id",
            max_bytes=256,
        )
        if type(expected_record_revision) is not int or expected_record_revision < 1:
            raise _alarm_error(
                "operator_alarm_acknowledgement_invalid",
                "expected_record_revision must be a positive integer",
            )
        if reason is not None:
            reason = _nfc_text(
                reason,
                field_name="reason",
                max_bytes=_MAX_REASON_BYTES,
            )
        acknowledged_at_ns = self._monotonic_clock()
        acknowledged_at_wall = self._wall_clock()
        with self._lock:
            point = self._points.get(condition_id)
            if point is None or point.record is None:
                raise _alarm_error(
                    "operator_alarm_not_found",
                    f"Unknown operator alarm condition: {condition_id}",
                )
            definition = self.catalog.definition(point.record.definition_id)
            transition = reduce_acknowledgement(
                definition,
                point,
                activation_id=activation_id,
                expected_record_revision=expected_record_revision,
                principal_id=principal_id,
                reason=reason,
                acknowledged_at_ns=acknowledged_at_ns,
                acknowledged_at_wall=acknowledged_at_wall,
            )
            if not transition.changed:
                return OperatorAlarmMutation(
                    status=transition.status,
                    changed=False,
                    manager_generation=self._generation,
                    records=(point.record,),
                    lifecycle_facts=(),
                )
            self._points[condition_id] = transition.point
            self._points.move_to_end(condition_id)
            self._generation += 1
            self._state_revision += 1
            self._metrics["acknowledgements"] += 1
            if point.record is not None:
                self._acknowledgement_latency_ns.append(
                    max(0, acknowledged_at_ns - point.record.activated_at_ns)
                )
            if transition.terminal_record is not None:
                self._append_terminal_locked(transition.terminal_record)
            facts = self._stamp_facts_locked(transition.lifecycle_facts)
            return OperatorAlarmMutation(
                status=transition.status,
                changed=True,
                manager_generation=self._generation,
                records=(transition.point.record,),
                lifecycle_facts=facts,
            )

    def acknowledge_all(
        self,
        *,
        members: Iterable[AlarmAcknowledgementMember],
        expected_generation: int,
        principal_id: str,
        reason: str | None = None,
        confirmed: bool,
    ) -> OperatorAlarmMutation:
        if not confirmed:
            raise _alarm_error(
                "operator_alarm_acknowledge_all_confirmation_required",
                "Acknowledge all requires explicit confirmation",
            )
        principal_id = _nfc_text(
            principal_id,
            field_name="principal_id",
            max_bytes=_MAX_PRINCIPAL_BYTES,
        )
        if reason is not None:
            reason = _nfc_text(
                reason,
                field_name="reason",
                max_bytes=_MAX_REASON_BYTES,
            )
        requested = tuple(sorted(members))
        if len(requested) != len(set(requested)):
            raise _alarm_error(
                "operator_alarm_acknowledge_all_duplicate_member",
                "Acknowledge-all members must be unique",
            )
        acknowledged_at_ns = self._monotonic_clock()
        acknowledged_at_wall = self._wall_clock()
        with self._lock:
            if type(expected_generation) is not int or (
                expected_generation != self._generation
            ):
                raise _alarm_error(
                    "operator_alarm_manager_generation_stale",
                    "Alarm membership changed while acknowledgement was pending",
                )
            current = tuple(sorted(self._unacknowledged_membership_locked()))
            if requested != current:
                raise _alarm_error(
                    "operator_alarm_membership_mismatch",
                    "Acknowledge-all must name the exact current membership",
                )
            if not current:
                return OperatorAlarmMutation(
                    status="already_empty",
                    changed=False,
                    manager_generation=self._generation,
                    records=(),
                    lifecycle_facts=(),
                )
            transitions: list[AlarmTransitionResult] = []
            for member in current:
                point = self._points[member.condition_id]
                record = point.record
                assert record is not None
                definition = self.catalog.definition(record.definition_id)
                transitions.append(
                    reduce_acknowledgement(
                        definition,
                        point,
                        activation_id=member.activation_id,
                        expected_record_revision=member.record_revision,
                        principal_id=principal_id,
                        reason=reason,
                        acknowledged_at_ns=acknowledged_at_ns,
                        acknowledged_at_wall=acknowledged_at_wall,
                    )
                )
            for member, transition in zip(current, transitions, strict=True):
                self._points[member.condition_id] = transition.point
                self._points.move_to_end(member.condition_id)
                if transition.terminal_record is not None:
                    self._append_terminal_locked(transition.terminal_record)
            self._generation += 1
            self._state_revision += 1
            self._metrics["acknowledgements"] += len(transitions)
            self._metrics["bulk_acknowledgements"] += 1
            for member in current:
                original = self._points[member.condition_id].record
                # The point has already been replaced above. Transition output
                # retains the original activation timestamp, so use it here.
                if original is not None:
                    self._acknowledgement_latency_ns.append(
                        max(0, acknowledged_at_ns - original.activated_at_ns)
                    )
            facts = tuple(
                fact for transition in transitions for fact in transition.lifecycle_facts
            )
            membership_payload = [member.to_dict() for member in current]
            membership_digest = hashlib.sha256(
                json.dumps(
                    membership_payload,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode("utf-8")
            ).hexdigest()
            facts += (
                AlarmLifecycleFact(
                    event_type="operator_alarms_acknowledged",
                    definition_id=None,
                    condition_id=None,
                    activation_id=None,
                    previous_state=None,
                    current_state=None,
                    record_revision=None,
                    manager_generation=self._generation,
                    occurred_at_ns=acknowledged_at_ns,
                    occurred_at_wall=acknowledged_at_wall,
                    principal_id=principal_id,
                    reason=reason,
                    details={
                        "member_count": len(current),
                        "membership_sha256": membership_digest,
                    },
                ),
            )
            facts = self._stamp_facts_locked(facts)
            return OperatorAlarmMutation(
                status="acknowledged_all",
                changed=True,
                manager_generation=self._generation,
                records=tuple(
                    transition.point.record
                    for transition in transitions
                    if transition.point.record is not None
                ),
                lifecycle_facts=facts,
            )

    def _unacknowledged_membership_locked(
        self,
    ) -> tuple[AlarmAcknowledgementMember, ...]:
        return tuple(
            AlarmAcknowledgementMember(
                condition_id=condition_id,
                activation_id=point.record.activation_id,
                record_revision=point.record.record_revision,
            )
            for condition_id, point in self._points.items()
            if point.record is not None
            and point.record.lifecycle_state in ATTENTION_STATES
        )

    def _domain_status_locked(self) -> dict[str, dict[str, Any]]:
        result: dict[str, dict[str, Any]] = {}
        domains = self._required_domains | {
            registration.domain for registration in self._sources.values()
        }
        for domain in sorted(domains):
            sources = [
                registration
                for registration in self._sources.values()
                if registration.domain == domain
            ]
            reconciled = bool(sources) and all(
                source.snapshot_complete and source.quality == "good"
                for source in sources
            )
            result[domain] = {
                "required": domain in self._required_domains,
                "reconciled": reconciled,
                "sourceCount": len(sources),
                "unknownSourceCount": sum(
                    1 for source in sources if source.quality != "good"
                ),
            }
        return result

    def snapshot(
        self,
        *,
        limit: int | None = None,
        states: Iterable[str] | None = None,
    ) -> dict[str, Any]:
        timestamp_ns = self._monotonic_clock()
        selected_states = None if states is None else frozenset(states)
        if selected_states is not None and not selected_states.issubset(ALARM_STATES):
            raise _alarm_error(
                "operator_alarm_state_filter_invalid",
                "Alarm state filter contains an unsupported state",
            )
        if limit is not None and (type(limit) is not int or limit < 0):
            raise _alarm_error(
                "operator_alarm_limit_invalid",
                "Alarm limit must be a non-negative integer",
            )
        with self._lock:
            live_records = [
                point.record
                for point in self._points.values()
                if point.record is not None
                and point.record.lifecycle_state != "returned_acknowledged"
            ]
            live_records.sort(
                key=lambda record: (
                    0 if record.lifecycle_state in ATTENTION_STATES else 1,
                    _PRIORITY_ORDER[record.priority],
                    -record.activated_at_ns,
                    record.condition_id,
                )
            )
            filtered = (
                live_records
                if selected_states is None
                else [
                    record
                    for record in live_records
                    if record.lifecycle_state in selected_states
                ]
            )
            effective_limit = len(filtered) if limit is None else limit
            page = filtered[:effective_limit]
            membership = tuple(sorted(self._unacknowledged_membership_locked()))
            state_counts = Counter(record.lifecycle_state for record in live_records)
            priority_counts = Counter(record.priority for record in live_records)
            class_counts = Counter(record.alarm_class for record in live_records)
            domains = self._domain_status_locked()
            reconciled = not self._health_errors and all(
                status["reconciled"]
                for status in domains.values()
                if status["required"]
            )
            active_count = sum(record.active for record in live_records)
            active_acknowledged_count = sum(
                record.lifecycle_state == "active_acknowledged"
                for record in live_records
            )
            motion_attention_count = sum(
                record.requires_motion_attention
                and record.lifecycle_state in ATTENTION_STATES
                for record in live_records
            )
            return {
                "schemaVersion": OPERATOR_ALARM_SNAPSHOT_SCHEMA_VERSION,
                "catalogVersion": self.catalog.catalog_version,
                "generation": self._generation,
                "stateRevision": self._state_revision,
                "reconciled": reconciled,
                "health": {
                    "state": "available" if not self._health_errors else "unavailable",
                    "details": dict(self._health_errors),
                },
                "domains": domains,
                "liveCount": len(live_records),
                "unacknowledgedCount": len(membership),
                "motionAttentionCount": motion_attention_count,
                "activeCount": active_count,
                "activeAcknowledgedCount": active_acknowledged_count,
                "unacknowledgedMembership": [
                    member.to_dict() for member in membership
                ],
                "countsByState": {
                    state: state_counts.get(state, 0) for state in sorted(ALARM_STATES)
                },
                "countsByPriority": {
                    priority: priority_counts.get(priority, 0)
                    for priority in _PRIORITY_ORDER
                },
                "countsByClass": dict(sorted(class_counts.items())),
                "alarms": [record.to_dict() for record in page],
                "truncated": len(page) < len(filtered),
                "timestampNs": timestamp_ns,
            }

    def terminal_records(self, *, limit: int | None = None) -> list[dict[str, Any]]:
        if limit is not None and (type(limit) is not int or limit < 0):
            raise _alarm_error(
                "operator_alarm_limit_invalid",
                "Terminal alarm limit must be a non-negative integer",
            )
        with self._lock:
            records = list(reversed(self._terminal_records))
            if limit is not None:
                records = records[:limit]
            return [record.to_dict() for record in records]

    def attention_block_reason(self) -> dict[str, Any] | None:
        snapshot = self.snapshot()
        if not snapshot["reconciled"] or snapshot["health"]["state"] != "available":
            return {
                "errorCode": "operator_alarm_state_unavailable",
                "message": "Operator-alarm state is not reconciled and healthy",
                "details": {
                    "generation": snapshot["generation"],
                    "domains": snapshot["domains"],
                    "health": snapshot["health"],
                },
            }
        blocking = [
            record
            for record in snapshot["alarms"]
            if record["requiresMotionAttention"]
            and record["lifecycleState"] in ATTENTION_STATES
        ]
        if snapshot["unacknowledgedCount"] and blocking:
            return {
                "errorCode": "unacknowledged_operator_alarms",
                "message": "Operator alarms require acknowledgement before motion",
                "details": {
                    "generation": snapshot["generation"],
                    "unacknowledgedMembership": snapshot[
                        "unacknowledgedMembership"
                    ],
                },
            }
        return None

    def mark_health_unavailable(self, *, code: str, message: str) -> None:
        code = _nfc_text(code, field_name="health code", max_bytes=256)
        message = _nfc_text(message, field_name="health message", max_bytes=1024)
        with self._lock:
            self._health_errors[code] = message
            self._state_revision += 1

    def reconcile_health(self, *, code: str) -> None:
        with self._lock:
            if self._health_errors.pop(code, None) is not None:
                self._state_revision += 1

    def revision_token(self) -> tuple[int, int]:
        """Return the cheap immutable-cache invalidation token."""
        with self._lock:
            return (self._generation, self._state_revision)

    def metrics(self) -> dict[str, Any]:
        now_ns = self._monotonic_clock()
        with self._lock:
            live_records = [
                point.record
                for point in self._points.values()
                if point.record is not None
                and point.record.lifecycle_state != "returned_acknowledged"
            ]
            counts_by_state = Counter(
                record.lifecycle_state for record in live_records
            )
            counts_by_priority = Counter(record.priority for record in live_records)
            counts_by_class = Counter(record.alarm_class for record in live_records)
            current_standing = [
                max(0, now_ns - record.acknowledged_at_ns)
                for record in live_records
                if record.lifecycle_state == "active_acknowledged"
                and record.acknowledged_at_ns is not None
            ]
            recent_flood_count = sum(
                observed_at_ns >= now_ns - _ALARM_FLOOD_WINDOW_NS
                for observed_at_ns in self._recent_activations
            )
            chatter_rows = sorted(
                (
                    {
                        "conditionId": condition_id,
                        "activationsInWindow": sum(
                            observed_at_ns >= now_ns - _CHATTER_WINDOW_NS
                            for observed_at_ns in activations
                        ),
                    }
                    for condition_id, activations in self._condition_activations.items()
                ),
                key=lambda row: (-row["activationsInWindow"], row["conditionId"]),
            )
            chatter_candidates = [
                row
                for row in chatter_rows
                if row["activationsInWindow"] > _CHATTER_THRESHOLD
            ]
            current_attention = sum(
                record.lifecycle_state in ATTENTION_STATES for record in live_records
            )
            return {
                "schemaVersion": "manyforge.operator_alarm_metrics.v1",
                "totals": dict(self._metrics),
                "gauges": {
                    "liveConditionPoints": len(self._points),
                    "liveAlarms": len(live_records),
                    "unacknowledgedAttention": current_attention,
                    "motionAttention": sum(
                        record.requires_motion_attention
                        and record.lifecycle_state in ATTENTION_STATES
                        for record in live_records
                    ),
                    "activeStanding": counts_by_state.get(
                        "active_acknowledged", 0
                    ),
                    "terminalRecords": len(self._terminal_records),
                    "unknownQualitySources": sum(
                        registration.quality != "good"
                        for registration in self._sources.values()
                    ),
                    "managerGeneration": self._generation,
                    "managerStateRevision": self._state_revision,
                    "countsByState": {
                        state: counts_by_state.get(state, 0)
                        for state in sorted(ALARM_STATES)
                    },
                    "countsByPriority": {
                        priority: counts_by_priority.get(priority, 0)
                        for priority in _PRIORITY_ORDER
                    },
                    "countsByClass": dict(sorted(counts_by_class.items())),
                },
                "distributions": {
                    "ingestLatencyNs": _duration_summary(self._ingest_latency_ns),
                    "acknowledgementLatencyNs": _duration_summary(
                        self._acknowledgement_latency_ns
                    ),
                    "completedStandingDurationNs": _duration_summary(
                        self._completed_standing_duration_ns
                    ),
                    "currentStandingDurationNs": _duration_summary(
                        current_standing
                    ),
                },
                "assessment": {
                    "alarmFloodCandidate": (
                        recent_flood_count > _ALARM_FLOOD_THRESHOLD
                    ),
                    "activationsInFloodWindow": recent_flood_count,
                    "alarmFloodWindowNs": _ALARM_FLOOD_WINDOW_NS,
                    "alarmFloodThreshold": _ALARM_FLOOD_THRESHOLD,
                    "chatteringCandidateCount": len(chatter_candidates),
                    "chatteringCandidates": chatter_candidates[
                        :_MAX_ASSESSMENT_IDENTITIES
                    ],
                    "chatteringCandidatesTruncated": (
                        len(chatter_candidates) > _MAX_ASSESSMENT_IDENTITIES
                    ),
                    "chatterWindowNs": _CHATTER_WINDOW_NS,
                    "chatterThreshold": _CHATTER_THRESHOLD,
                    "claimBoundary": "engineering_assessment_not_safety_limit",
                },
                "retention": {
                    "durationSamples": _METRIC_SAMPLE_RETENTION,
                    "activationSamples": _ACTIVATION_SAMPLE_RETENTION,
                    "perConditionActivationSamples": (
                        _PER_CONDITION_ACTIVATION_RETENTION
                    ),
                },
                "timestampNs": now_ns,
            }
