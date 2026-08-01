"""Immutable alarm-history values and the typed I5/H0 owner gate.

The four-state reducer is itself alarm-lifecycle semantics, even when written
as a pure function.  It therefore does not remain in Composer beside the one
future headless owner.  Its exact historical implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` in source blob
``268fb082d9098da53b92a764b6002dd1076d5130`` (the full reducers start at
lines 798 and 990); transition-table acceptance input is in historical test
blob ``085187dd6464001d635ac216b387cf97d8c962bd`` at lines 226 and 266.

Only bounded, immutable history-value mechanics remain here.  Their current
field names are porting input, not a declaration of the future H0 wire: the
final owner-issued DTO must use the approved owner epoch and snapshot
generation contract.  Composer retains no catalog, reduction, identity,
acknowledgement, reconciliation, interlock, persistence, or producer owner.
"""

from __future__ import annotations

import math
import unicodedata
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Final, Mapping


OPERATOR_ALARM_OWNER_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_operator_alarm_owner_unavailable"
)
OPERATOR_ALARM_OWNER_RESTORATION_GATES: Final = ("I5", "H0")
_MAX_JSON_DEPTH: Final = 8
_MAX_JSON_NODES: Final = 4_096
_MAX_MAPPING_ITEMS: Final = 64
_MAX_SEQUENCE_ITEMS: Final = 256
_MAX_TEXT_BYTES: Final = 32 * 1024
_MAX_TOTAL_TEXT_BYTES: Final = 256 * 1024
_MAX_INTEGER_BITS: Final = 64
_MAX_EVENT_TYPE_BYTES: Final = 128
_MAX_IDENTITY_BYTES: Final = 1_024
_MAX_WALL_TIME_BYTES: Final = 128
_MAX_REASON_BYTES: Final = 4_096
_CANONICAL_EVENT_DETAIL_KEYS: Final = frozenset(
    {
        "definition_id",
        "condition_id",
        "activation_id",
        "previous_state",
        "current_state",
        "record_revision",
        "manager_generation",
        "source_id",
        "source_epoch",
        "source_sequence",
        "principal_id",
        "reason",
        "occurred_at_ns",
        "occurred_at",
        "correlations",
    }
)


class OperatorAlarmOwnerUnavailableError(RuntimeError):
    """Raised before Composer-local alarm state or collaborators are touched."""


class _FreezeBudget:
    __slots__ = ("nodes", "text_bytes")

    def __init__(self) -> None:
        self.nodes = 0
        self.text_bytes = 0

    def consume_node(self, *, field_name: str) -> None:
        self.nodes += 1
        if self.nodes > _MAX_JSON_NODES:
            raise ValueError(f"{field_name} exceeds the bounded value node count")

    def consume_text(
        self,
        value: Any,
        *,
        field_name: str,
        max_bytes: int,
        allow_empty: bool = True,
    ) -> str:
        if type(value) is not str:
            qualifier = "a string" if allow_empty else "a non-empty string"
            raise TypeError(f"{field_name} must be {qualifier}")
        if len(value) > max_bytes:
            raise ValueError(f"{field_name} exceeds {max_bytes} UTF-8 bytes")
        if not allow_empty and not value.strip():
            raise TypeError(f"{field_name} must be a non-empty string")
        if value != unicodedata.normalize("NFC", value):
            raise ValueError(f"{field_name} must use Unicode NFC normalization")
        try:
            encoded_length = len(value.encode("utf-8"))
        except UnicodeEncodeError as exc:
            raise ValueError(f"{field_name} must be valid UTF-8 text") from exc
        if encoded_length > max_bytes:
            raise ValueError(f"{field_name} exceeds {max_bytes} UTF-8 bytes")
        self.text_bytes += encoded_length
        if self.text_bytes > _MAX_TOTAL_TEXT_BYTES:
            raise ValueError("alarm history fact exceeds its total text bound")
        return value


def _bounded_nonnegative_integer(value: Any, *, field_name: str) -> int:
    if type(value) is not int:
        raise TypeError(f"{field_name} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{field_name} must be non-negative")
    if value.bit_length() > _MAX_INTEGER_BITS:
        raise ValueError(f"{field_name} exceeds the integer bound")
    return value


def _bounded_optional_integer(value: Any, *, field_name: str) -> int | None:
    if value is None:
        return None
    return _bounded_nonnegative_integer(value, field_name=field_name)


def _bounded_optional_text(
    value: Any,
    *,
    field_name: str,
    budget: _FreezeBudget,
    max_bytes: int,
) -> str | None:
    if value is None:
        return None
    return budget.consume_text(
        value,
        field_name=field_name,
        max_bytes=max_bytes,
        allow_empty=False,
    )


def _freeze_value(
    value: Any,
    *,
    field_name: str,
    depth: int,
    budget: _FreezeBudget,
    ancestors: set[int],
) -> Any:
    if depth > _MAX_JSON_DEPTH:
        raise ValueError(f"{field_name} exceeds nesting depth {_MAX_JSON_DEPTH}")
    budget.consume_node(field_name=field_name)

    if value is None or type(value) is bool:
        return value
    if type(value) is int:
        if value.bit_length() > _MAX_INTEGER_BITS:
            raise ValueError(f"{field_name} contains an oversized integer")
        return value
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError(f"{field_name} contains a non-finite number")
        return value
    if type(value) is str:
        return budget.consume_text(
            value,
            field_name=field_name,
            max_bytes=_MAX_TEXT_BYTES,
        )
    if type(value) is dict:
        if len(value) > _MAX_MAPPING_ITEMS:
            raise ValueError(
                f"{field_name} exceeds {_MAX_MAPPING_ITEMS} mapping entries"
            )
        identity = id(value)
        if identity in ancestors:
            raise ValueError(f"{field_name} contains a container cycle")
        ancestors.add(identity)
        frozen: dict[str, Any] = {}
        try:
            for key, item in value.items():
                if type(key) is not str:
                    raise TypeError(f"{field_name} contains a non-string mapping key")
                bounded_key = budget.consume_text(
                    key,
                    field_name=f"{field_name} mapping key",
                    max_bytes=_MAX_IDENTITY_BYTES,
                    allow_empty=False,
                )
                frozen[bounded_key] = _freeze_value(
                    item,
                    field_name=field_name,
                    depth=depth + 1,
                    budget=budget,
                    ancestors=ancestors,
                )
        finally:
            ancestors.remove(identity)
        return MappingProxyType(frozen)
    if type(value) in {list, tuple}:
        if len(value) > _MAX_SEQUENCE_ITEMS:
            raise ValueError(
                f"{field_name} exceeds {_MAX_SEQUENCE_ITEMS} sequence entries"
            )
        identity = id(value)
        if identity in ancestors:
            raise ValueError(f"{field_name} contains a container cycle")
        ancestors.add(identity)
        try:
            return tuple(
                _freeze_value(
                    item,
                    field_name=field_name,
                    depth=depth + 1,
                    budget=budget,
                    ancestors=ancestors,
                )
                for item in value
            )
        finally:
            ancestors.remove(identity)
    raise TypeError(f"{field_name} contains a non-JSON value")


def _freeze_mapping(
    value: Any,
    *,
    field_name: str,
    budget: _FreezeBudget,
) -> Mapping[str, Any]:
    if type(value) is not dict:
        raise TypeError(f"{field_name} must be a built-in mapping")
    frozen = _freeze_value(
        value,
        field_name=field_name,
        depth=0,
        budget=budget,
        ancestors=set(),
    )
    return frozen


def _thaw_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw_value(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw_value(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class AlarmLifecycleFact:
    """Immutable transition history input emitted by the future alarm owner."""

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
        budget = _FreezeBudget()
        budget.consume_text(
            self.event_type,
            field_name="event_type",
            max_bytes=_MAX_EVENT_TYPE_BYTES,
            allow_empty=False,
        )
        for field_name in (
            "definition_id",
            "condition_id",
            "activation_id",
            "previous_state",
            "current_state",
            "source_id",
            "source_epoch",
            "principal_id",
        ):
            _bounded_optional_text(
                getattr(self, field_name),
                field_name=field_name,
                budget=budget,
                max_bytes=_MAX_IDENTITY_BYTES,
            )
        _bounded_optional_text(
            self.reason,
            field_name="reason",
            budget=budget,
            max_bytes=_MAX_REASON_BYTES,
        )
        budget.consume_text(
            self.occurred_at_wall,
            field_name="occurred_at_wall",
            max_bytes=_MAX_WALL_TIME_BYTES,
            allow_empty=False,
        )
        _bounded_optional_integer(
            self.record_revision,
            field_name="record_revision",
        )
        _bounded_nonnegative_integer(
            self.manager_generation,
            field_name="manager_generation",
        )
        _bounded_nonnegative_integer(
            self.occurred_at_ns,
            field_name="occurred_at_ns",
        )
        _bounded_optional_integer(
            self.source_sequence,
            field_name="source_sequence",
        )
        frozen_correlations = _freeze_mapping(
            self.correlations,
            field_name="correlations",
            budget=budget,
        )
        frozen_details = _freeze_mapping(
            self.details,
            field_name="details",
            budget=budget,
        )
        collisions = _CANONICAL_EVENT_DETAIL_KEYS.intersection(frozen_details)
        if collisions:
            raise ValueError(
                "details cannot replace canonical alarm-history fields: "
                f"{sorted(collisions)}"
            )
        object.__setattr__(
            self,
            "correlations",
            frozen_correlations,
        )
        object.__setattr__(
            self,
            "details",
            frozen_details,
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
            "correlations": _thaw_value(self.correlations),
            **_thaw_value(self.details),
        }


class OperatorAlarmManager:
    """Unavailable compatibility name; alarm truth is not owned by Composer."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise OperatorAlarmOwnerUnavailableError(
            f"{OPERATOR_ALARM_OWNER_UNAVAILABLE_REASON_CODE}: the single "
            "headless alarm owner returns at I5/H0"
        )
