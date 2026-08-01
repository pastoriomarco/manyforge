"""Thin lifecycle observation/evidence client primitives for I7/L0.

The old Composer ROS provider-v1 transport, caller sessions, leases, local
readiness/admission reducer, command serialization, and reconciliation owner
are absent. Generic CCLC transaction truth returns in C++; Python retains only
bounded owner-issued observation presentation and history reduction.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
import logging
import math
import threading
from pathlib import Path
from types import MappingProxyType
from typing import Any, Callable, Final, Mapping, NoReturn
import unicodedata


_LOGGER = logging.getLogger(__name__)
_TERMINAL_PHASES: Final = frozenset({"succeeded", "failed", "outcome_unknown"})
_MAX_JSON_DEPTH: Final = 8
_MAX_JSON_NODES: Final = 4_096
_MAX_MAPPING_ITEMS: Final = 64
_MAX_SEQUENCE_ITEMS: Final = 256
_MAX_TEXT_BYTES: Final = 32 * 1024
_MAX_TOTAL_TEXT_BYTES: Final = 256 * 1024
_MAX_INTEGER_BITS: Final = 64
_MAX_IDENTITY_BYTES: Final = 256
_MAX_FACET_KEY_BYTES: Final = 128
_MAX_ADAPTER_EVIDENCE_ITEMS: Final = 32
_MAX_ADAPTER_KEY_BYTES: Final = 128
_MAX_ADAPTER_VALUE_BYTES: Final = 512
_MAX_TERMINAL_REASON_BYTES: Final = 512
ROBOT_LIFECYCLE_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_robot_lifecycle_client_unavailable"
)
ROBOT_LIFECYCLE_RESTORATION_GATES: Final = ("I7", "L0")
REAL_ROBOT_LIFECYCLE_QUALIFICATION_GATES: Final = ("I10", "R")
REAL_ROBOT_RECORD_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_real_robot_record_validator_unavailable"
)


class RobotLifecycleClientUnavailableError(RuntimeError):
    """Raised before ROS creation, state access, or robot effects."""


class RealRobotRuntimeRecordUnavailableError(RuntimeError):
    """Artifact validation must move to the deployment/runtime-product owner."""


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
        strip: bool = False,
    ) -> str:
        if type(value) is not str:
            qualifier = "a string" if allow_empty else "a non-empty string"
            raise TypeError(f"{field_name} must be {qualifier}")
        bounded = value.strip() if strip else value
        if not allow_empty and not bounded:
            raise ValueError(f"{field_name} must not be empty")
        if len(bounded) > max_bytes:
            raise ValueError(f"{field_name} exceeds {max_bytes} UTF-8 bytes")
        if bounded != unicodedata.normalize("NFC", bounded):
            raise ValueError(f"{field_name} must use Unicode NFC normalization")
        try:
            encoded_length = len(bounded.encode("utf-8"))
        except UnicodeEncodeError as exc:
            raise ValueError(f"{field_name} must be valid UTF-8 text") from exc
        if encoded_length > max_bytes:
            raise ValueError(f"{field_name} exceeds {max_bytes} UTF-8 bytes")
        self.text_bytes += encoded_length
        if self.text_bytes > _MAX_TOTAL_TEXT_BYTES:
            raise ValueError("lifecycle observation exceeds its total text bound")
        return bounded


def _bounded_nonnegative_integer(value: Any, *, field_name: str) -> int:
    if type(value) is not int:
        raise TypeError(f"{field_name} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{field_name} must be non-negative")
    if value.bit_length() > _MAX_INTEGER_BITS:
        raise ValueError(f"{field_name} exceeds the integer bound")
    return value


def _bounded_positive_integer(value: Any, *, field_name: str) -> int:
    bounded = _bounded_nonnegative_integer(value, field_name=field_name)
    if bounded == 0:
        raise ValueError(f"{field_name} must be positive")
    return bounded


def _freeze_lifecycle_value(
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
                    raise TypeError(
                        f"{field_name} contains a non-string mapping key"
                    )
                bounded_key = budget.consume_text(
                    key,
                    field_name=f"{field_name} mapping key",
                    max_bytes=_MAX_FACET_KEY_BYTES,
                    allow_empty=False,
                )
                frozen[bounded_key] = _freeze_lifecycle_value(
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
                _freeze_lifecycle_value(
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


def _freeze_material_facets(
    value: Any,
    *,
    budget: _FreezeBudget,
) -> Mapping[str, Any]:
    if type(value) is not dict:
        raise TypeError("material_facets must be a built-in mapping")
    if len(value) > _MAX_MAPPING_ITEMS:
        raise ValueError(f"material_facets exceeds {_MAX_MAPPING_ITEMS} entries")
    budget.consume_node(field_name="material_facets")
    identity = id(value)
    ancestors = {identity}
    frozen: dict[str, Any] = {}
    for raw_key, item in value.items():
        if type(raw_key) is not str:
            raise TypeError("material_facets contains a non-string mapping key")
        key = budget.consume_text(
            raw_key,
            field_name="material facet key",
            max_bytes=_MAX_FACET_KEY_BYTES,
            allow_empty=False,
            strip=True,
        )
        if key in frozen:
            raise ValueError("material facet keys must be unique after trimming")
        frozen[key] = _freeze_lifecycle_value(
            item,
            field_name=f"material facet '{key}'",
            depth=1,
            budget=budget,
            ancestors=ancestors,
        )
    return MappingProxyType(frozen)


def _freeze_adapter_evidence(
    value: Any,
    *,
    budget: _FreezeBudget,
) -> Mapping[str, str]:
    if type(value) is not dict:
        raise TypeError("adapter_evidence must be a built-in mapping")
    if len(value) > _MAX_ADAPTER_EVIDENCE_ITEMS:
        raise ValueError(
            f"adapter_evidence exceeds {_MAX_ADAPTER_EVIDENCE_ITEMS} entries"
        )
    budget.consume_node(field_name="adapter_evidence")
    frozen: dict[str, str] = {}
    for raw_key, raw_value in value.items():
        key = budget.consume_text(
            raw_key,
            field_name="adapter evidence key",
            max_bytes=_MAX_ADAPTER_KEY_BYTES,
            allow_empty=False,
            strip=True,
        )
        if key in frozen:
            raise ValueError("adapter evidence keys must be unique after trimming")
        frozen[key] = budget.consume_text(
            raw_value,
            field_name=f"adapter evidence '{key}'",
            max_bytes=_MAX_ADAPTER_VALUE_BYTES,
        )
    return MappingProxyType(frozen)


def _thaw_lifecycle_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {
            key: _thaw_lifecycle_value(item)
            for key, item in value.items()
        }
    if isinstance(value, tuple):
        return [_thaw_lifecycle_value(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class LifecycleTerminalObservation:
    """Bounded owner-issued terminal request evidence, not caller authority."""

    request_id: str
    action_id: str
    result_revision: int
    phase: str
    result_reason: str = ""

    def __post_init__(self) -> None:
        budget = _FreezeBudget()
        request_id = budget.consume_text(
            self.request_id,
            field_name="terminal request_id",
            max_bytes=_MAX_IDENTITY_BYTES,
            allow_empty=False,
            strip=True,
        )
        action_id = budget.consume_text(
            self.action_id,
            field_name="terminal action_id",
            max_bytes=_MAX_IDENTITY_BYTES,
            allow_empty=False,
            strip=True,
        )
        _bounded_positive_integer(
            self.result_revision,
            field_name="terminal result_revision",
        )
        phase = budget.consume_text(
            self.phase,
            field_name="terminal phase",
            max_bytes=32,
            allow_empty=False,
        )
        if phase not in _TERMINAL_PHASES:
            raise ValueError("terminal phase is invalid")
        result_reason = budget.consume_text(
            self.result_reason,
            field_name="terminal result_reason",
            max_bytes=_MAX_TERMINAL_REASON_BYTES,
        )
        object.__setattr__(self, "request_id", request_id)
        object.__setattr__(self, "action_id", action_id)
        object.__setattr__(self, "phase", phase)
        object.__setattr__(self, "result_reason", result_reason)


@dataclass(frozen=True, slots=True)
class LifecycleObservation:
    """Transport-neutral snapshot of facts already issued by the I7 owner."""

    component_id: str
    owner_epoch: str
    source_revision: int
    observed_at_ns: int
    fresh_until_ns: int
    material_facets: Mapping[str, Any] = field(default_factory=dict)
    adapter_evidence: Mapping[str, str] = field(default_factory=dict)
    last_terminal: LifecycleTerminalObservation | None = None

    def __post_init__(self) -> None:
        budget = _FreezeBudget()
        component_id = budget.consume_text(
            self.component_id,
            field_name="component_id",
            max_bytes=_MAX_IDENTITY_BYTES,
            allow_empty=False,
            strip=True,
        )
        owner_epoch = budget.consume_text(
            self.owner_epoch,
            field_name="owner_epoch",
            max_bytes=_MAX_IDENTITY_BYTES,
            allow_empty=False,
            strip=True,
        )
        _bounded_positive_integer(self.source_revision, field_name="source_revision")
        _bounded_nonnegative_integer(
            self.observed_at_ns,
            field_name="observed_at_ns",
        )
        _bounded_nonnegative_integer(
            self.fresh_until_ns,
            field_name="fresh_until_ns",
        )
        if self.fresh_until_ns < self.observed_at_ns:
            raise ValueError("observation freshness interval is invalid")
        facets = _freeze_material_facets(self.material_facets, budget=budget)
        adapter = _freeze_adapter_evidence(self.adapter_evidence, budget=budget)
        if (
            self.last_terminal is not None
            and type(self.last_terminal) is not LifecycleTerminalObservation
        ):
            raise TypeError(
                "last_terminal must be a LifecycleTerminalObservation or None"
            )
        object.__setattr__(self, "component_id", component_id)
        object.__setattr__(self, "owner_epoch", owner_epoch)
        object.__setattr__(self, "material_facets", facets)
        object.__setattr__(self, "adapter_evidence", adapter)


def reduce_lifecycle_observation(
    observation: LifecycleObservation,
    *,
    now_ns: int,
) -> dict[str, Any]:
    """Present owner-issued facts and receiver-local freshness without admission."""
    _bounded_nonnegative_integer(now_ns, field_name="now_ns")
    return {
        "component_id": observation.component_id,
        "owner_epoch": observation.owner_epoch,
        "source_revision": observation.source_revision,
        "observed_at_ns": observation.observed_at_ns,
        "fresh_until_ns": observation.fresh_until_ns,
        "observation_fresh": now_ns <= observation.fresh_until_ns,
        "material_facets": _thaw_lifecycle_value(observation.material_facets),
        "adapter_evidence": dict(observation.adapter_evidence),
    }


class RobotLifecycleTransitionRecorder:
    """Reduce source-ordered observations into bounded diagnostic history.

    The recorder emits one baseline per owner epoch, material deltas in source
    order, and each terminal result once. It never derives readiness, admission,
    command authority, vendor causality, or a recovery policy.
    """

    def __init__(
        self,
        event_sink: Callable[..., None],
        *,
        material_adapter_status_keys: tuple[str, ...] = (),
    ) -> None:
        if type(material_adapter_status_keys) is not tuple:
            raise TypeError("material_adapter_status_keys must be a tuple")
        if len(material_adapter_status_keys) > 32:
            raise ValueError("material adapter key count exceeds 32")
        self._event_sink = event_sink
        budget = _FreezeBudget()
        bounded_keys = tuple(
            budget.consume_text(
                key,
                field_name="material adapter status key",
                max_bytes=_MAX_ADAPTER_KEY_BYTES,
                allow_empty=False,
                strip=True,
            )
            for key in material_adapter_status_keys
        )
        if len(set(bounded_keys)) != len(bounded_keys):
            raise ValueError("material adapter status keys must be unique")
        self._material_adapter_status_keys = bounded_keys
        self._lock = threading.Lock()
        self._identity: tuple[str, str] | None = None
        self._last_revision = 0
        self._last_facets: dict[str, Any] = {}
        self._seen_terminals: deque[tuple[str, int, str]] = deque(maxlen=64)

    def _emit(self, action: str, details: dict[str, Any]) -> None:
        try:
            self._event_sink(action, details=details)
        except Exception:
            _LOGGER.exception(
                "Failed to record robot lifecycle history",
                extra={"robot_lifecycle_history_action": action},
            )

    def observe(self, observation: LifecycleObservation) -> None:
        identity = (observation.component_id, observation.owner_epoch)
        facets = dict(observation.material_facets)
        for key in self._material_adapter_status_keys:
            if key in observation.adapter_evidence:
                facets[f"adapter.{key}"] = observation.adapter_evidence[key]

        with self._lock:
            identity_changed = identity != self._identity
            if identity_changed:
                self._identity = identity
                self._last_revision = 0
                self._last_facets = {}
                self._seen_terminals.clear()
            if observation.source_revision <= self._last_revision:
                return

            source = {
                "component_id": observation.component_id,
                "owner_epoch": observation.owner_epoch,
                "source_revision": observation.source_revision,
                "observed_at_ns": observation.observed_at_ns,
                "causal_attribution": "not_inferred",
            }
            if identity_changed:
                self._emit(
                    "observation_baseline",
                    {
                        **source,
                        "facets": _thaw_lifecycle_value(facets),
                        "adapter_evidence": dict(observation.adapter_evidence),
                    },
                )
            else:
                changes: dict[str, dict[str, Any]] = {}
                for key in sorted(set(self._last_facets).union(facets)):
                    present_before = key in self._last_facets
                    present_now = key in facets
                    previous = self._last_facets.get(key)
                    current = facets.get(key)
                    if present_before == present_now and previous == current:
                        continue
                    changes[key] = {
                        "previous": _thaw_lifecycle_value(previous),
                        "current": _thaw_lifecycle_value(current),
                    }
                if changes:
                    self._emit(
                        "observed_transition",
                        {
                            **source,
                            "changed_facets": list(changes),
                            "changes": changes,
                            "adapter_evidence": dict(observation.adapter_evidence),
                        },
                    )
            self._last_revision = observation.source_revision
            self._last_facets = facets

            terminal = observation.last_terminal
            if terminal is not None:
                terminal_key = (
                    terminal.request_id,
                    terminal.result_revision,
                    terminal.phase,
                )
                if terminal_key not in self._seen_terminals:
                    self._emit(
                        "request_terminal",
                        {
                            **source,
                            "request": {
                                "request_id": terminal.request_id,
                                "action_id": terminal.action_id,
                                "result_revision": terminal.result_revision,
                                "phase": terminal.phase,
                                "result_reason": terminal.result_reason,
                            },
                        },
                    )
                    self._seen_terminals.append(terminal_key)


def require_robot_lifecycle_client() -> NoReturn:
    raise RobotLifecycleClientUnavailableError(
        f"{ROBOT_LIFECYCLE_CLIENT_UNAVAILABLE_REASON_CODE}: lifecycle client "
        "returns at I7/L0 on the caller-neutral CCLC/open-request contract"
    )


class RobotLifecycleClient:
    """Stable import marker; construction performs no ROS or thread effects."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        require_robot_lifecycle_client()


def validate_real_robot_runtime_record(
    *,
    deployment_path: Path,
    execution_config: Mapping[str, Any],
) -> NoReturn:
    """Gate the obsolete Composer validator before path or digest access."""
    del deployment_path, execution_config
    raise RealRobotRuntimeRecordUnavailableError(
        f"{REAL_ROBOT_RECORD_UNAVAILABLE_REASON_CODE}: generic artifact "
        "integrity moves to the deployment owner; xArm qualification returns "
        "at I10/R"
    )


__all__ = [
    "LifecycleObservation",
    "LifecycleTerminalObservation",
    "RealRobotRuntimeRecordUnavailableError",
    "RobotLifecycleClient",
    "RobotLifecycleClientUnavailableError",
    "RobotLifecycleTransitionRecorder",
    "reduce_lifecycle_observation",
    "require_robot_lifecycle_client",
    "validate_real_robot_runtime_record",
]
