"""The one bounded failure representation carried by Program-run records.

A failure is described where it happens, once, by four fields: a reason
code, a human message, bounded structured ``details`` and an ``origin``.
Run status, the recorded diagnostic and the recovery offer carry ``details``
and ``origin`` unchanged; nobody re-derives them from the message.

Every producer bounds its details with :func:`bounded_failure_details` and
names where the failure happened with :func:`failure_origin`.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from math import isfinite
from types import MappingProxyType
from typing import Any, Final

import rfc8785

from .catalog_schema import parse_schema_registry, validate_value


# The registered detail keys, in the order they are carried. A key outside
# this set is not carried: the message already holds the human text.
FAILURE_DETAIL_KEYS: Final = (
    # An owner's refusal: its precise cause and any failed reference.
    "reason",
    "reference_type",
    "reference",
    "reference_id",
    # A node input that could not be resolved (`input_failure_details`).
    "input_problem",
    "path",
    "source_kind",
    "source_reference",
    # Required ErrorDTO details a run may surface.
    "rule_id",
    "component_id",
    "retryable",
    "required_action",
)
MAX_FAILURE_DETAIL_ENTRIES: Final = 16
MAX_FAILURE_DETAIL_TEXT_BYTES: Final = 256
MAX_FAILURE_DETAILS_BYTES: Final = 2_048

FAILURE_ORIGIN_KINDS: Final = frozenset({"program_node", "owner", "program_runner"})
# Origins that always name the exact failed node instance.
_NODE_ORIGIN_KINDS: Final = frozenset({"program_node", "owner"})
MAX_FAILURE_ORIGIN_TEXT_BYTES: Final = 256

_MAX_SAFE_INTEGER: Final = 9_007_199_254_740_991
_OMITTED: Final = object()
_DIRECT_MEMBERS: Final = {
    "target": "command_target_id", "signal": "signal_name",
    "frame": "frame_id", "collision_body": "collision_body_id",
}
_DIRECT_REFERENCE_SCHEMAS = parse_schema_registry({
    kind: {
        "type": "object", "semantic_type": f"{kind}_reference",
        "required": ["component_id", member], "additional_properties": False,
        "properties": {"component_id": {"type": "string"}, member: {"type": "string"}},
    }
    for kind, member in _DIRECT_MEMBERS.items()
})


def _bounded_text(value: str) -> str | object:
    # Never shortened: a shortened identity or path would be a plausible but
    # different value. The message still names the cause.
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError:
        return _OMITTED
    return value if len(encoded) <= MAX_FAILURE_DETAIL_TEXT_BYTES else _OMITTED


def _bounded_value(value: object) -> object:
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value if abs(value) <= _MAX_SAFE_INTEGER else _OMITTED
    if isinstance(value, float):
        if not isfinite(value) or (value.is_integer() and abs(value) > _MAX_SAFE_INTEGER):
            return _OMITTED
        return value
    if isinstance(value, str):
        return _bounded_text(value)
    # Nested objects, arrays and any other value are not carried.
    return _OMITTED


def bounded_failure_details(details: object) -> Mapping[str, Any] | None:
    """Project one failure's structured details into the bounded shape.

    Only registered keys are carried, in registered order. An entry outside
    its bound is left out, never shortened: a string longer than 256 UTF-8
    bytes, an unregistered nested object or array, a non-finite number, an integer
    outside the I-JSON range, or an entry that would take the canonical
    JSON past 2,048 bytes. ``None`` when nothing remains (or nothing was
    given).
    """

    if not isinstance(details, Mapping):
        return None
    bounded: dict[str, Any] = {}
    for key in FAILURE_DETAIL_KEYS:
        if len(bounded) >= MAX_FAILURE_DETAIL_ENTRIES:
            break
        if key not in details:
            continue
        if key == "reference":
            kind = details.get("reference_type")
            raw = details[key]
            if (not isinstance(kind, str) or kind not in _DIRECT_MEMBERS
                    or not isinstance(raw, Mapping)
                    or any(not isinstance(key, str) for key in raw)):
                continue
            value = dict(raw)
            if validate_value(_DIRECT_REFERENCE_SCHEMAS.types[kind], value, registry=_DIRECT_REFERENCE_SCHEMAS):
                continue
        else:
            value = _bounded_value(details[key])
        if value is _OMITTED:
            continue
        candidate = {**bounded, key: value}
        if len(rfc8785.dumps(candidate)) > MAX_FAILURE_DETAILS_BYTES:
            continue
        bounded = candidate
    if "reference" in bounded:
        bounded["reference"] = MappingProxyType(bounded["reference"])
    return MappingProxyType(bounded) if bounded else None


def exception_failure_details(error: BaseException) -> Mapping[str, Any] | None:
    """The bounded details an error carries, if any.

    An input-resolution error and a client-side owner error carry them as
    ``details``; an owner's HTTP rejection carries them in its response's
    ``error.details``.
    """

    details = getattr(error, "details", None)
    if not isinstance(details, Mapping):
        response = getattr(error, "response", None)
        owner_error = response.get("error") if isinstance(response, Mapping) else None
        details = (
            owner_error.get("details") if isinstance(owner_error, Mapping) else None
        )
    return bounded_failure_details(details)


def _origin_text(value: object, *, field: str) -> str | None:
    if value is None:
        return None
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > MAX_FAILURE_ORIGIN_TEXT_BYTES
    ):
        raise ValueError(f"failure origin {field} must be absent or bounded text")
    return value


def failure_origin(
    kind: str,
    *,
    node_instance_id: str | None = None,
    owner_api: str | None = None,
) -> Mapping[str, Any]:
    """Where one failure happened, as every run record carries it.

    ``program_node``: the node itself failed (input, expression, timeout,
    condition). ``owner``: an owner refused or failed the node's request;
    ``owner_api`` is the catalog owner API when known. Both always name the
    exact node instance. ``program_runner``: the runner ended or paused the
    run; ``node_instance_id`` only when one exact node is known.
    """

    if not isinstance(kind, str) or kind not in FAILURE_ORIGIN_KINDS:
        raise ValueError(f"failure origin kind {kind!r} is unsupported")
    node_instance_id = _origin_text(node_instance_id, field="node_instance_id")
    if node_instance_id is None and kind in _NODE_ORIGIN_KINDS:
        raise ValueError(f"a {kind} failure origin names its node instance")
    owner_api = _origin_text(owner_api, field="owner_api")
    if owner_api is not None and kind != "owner":
        raise ValueError("only an owner failure origin names an owner API")
    return MappingProxyType(
        {
            "kind": kind,
            "node_instance_id": node_instance_id,
            "owner_api": owner_api,
        }
    )


@dataclass(frozen=True, slots=True)
class NodeFailureOrigin:
    """A node's own, partial origin: its kind and owner API.

    A node does not attribute its failure to an instance; the run, which
    knows the exact authored node, completes it with :meth:`for_node`.
    """

    kind: str
    owner_api: str | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.kind, str) or self.kind not in _NODE_ORIGIN_KINDS:
            raise ValueError("a node failure origin is program_node or owner")
        _origin_text(self.owner_api, field="owner_api")
        if self.owner_api is not None and self.kind != "owner":
            raise ValueError("only an owner failure origin names an owner API")

    def for_node(self, node_instance_id: str) -> Mapping[str, Any]:
        return failure_origin(
            self.kind, node_instance_id=node_instance_id, owner_api=self.owner_api
        )


def require_failure_origin(origin: object) -> Mapping[str, Any] | None:
    """Return one exact origin (or ``None``), refusing any other shape."""

    if origin is None:
        return None
    if not isinstance(origin, Mapping) or set(origin.keys()) != {
        "kind",
        "node_instance_id",
        "owner_api",
    }:
        raise ValueError("failure origin has an invalid shape")
    return failure_origin(
        origin["kind"],
        node_instance_id=origin["node_instance_id"],
        owner_api=origin["owner_api"],
    )


def require_failure_details(details: object) -> Mapping[str, Any] | None:
    """Return details already in the bounded shape, refusing anything else."""

    if details is None:
        return None
    bounded = bounded_failure_details(details)
    if bounded is None or dict(bounded) != dict(details):  # type: ignore[call-overload]
        raise ValueError("failure details are outside their bound")
    return bounded


def failure_wire_fields(
    details: Mapping[str, Any] | None, origin: Mapping[str, Any] | None
) -> dict[str, Any]:
    """The ``details`` and ``origin`` fields of one run record."""

    return {
        "details": None if details is None else {
            key: dict(value) if key == "reference" and isinstance(value, Mapping) else value
            for key, value in details.items()
        },
        "origin": None if origin is None else dict(origin),
    }


@dataclass(frozen=True, slots=True)
class FailureRecord:
    """One failure: reason code, message, bounded details and origin."""

    reason_code: str
    message: str
    details: Mapping[str, Any] | None
    origin: Mapping[str, Any] | None

    def __post_init__(self) -> None:
        if (
            not isinstance(self.reason_code, str)
            or not self.reason_code
            or len(self.reason_code.encode("utf-8")) > 256
        ):
            raise ValueError("failure reason_code must be bounded non-empty text")
        if not isinstance(self.message, str) or len(self.message.encode("utf-8")) > 4_096:
            raise ValueError("failure message must be bounded text")
        object.__setattr__(self, "details", require_failure_details(self.details))
        object.__setattr__(self, "origin", require_failure_origin(self.origin))


__all__ = [
    "FAILURE_DETAIL_KEYS",
    "FAILURE_ORIGIN_KINDS",
    "FailureRecord",
    "NodeFailureOrigin",
    "MAX_FAILURE_DETAILS_BYTES",
    "MAX_FAILURE_DETAIL_ENTRIES",
    "MAX_FAILURE_DETAIL_TEXT_BYTES",
    "MAX_FAILURE_ORIGIN_TEXT_BYTES",
    "bounded_failure_details",
    "exception_failure_details",
    "failure_origin",
    "failure_wire_fields",
    "require_failure_details",
    "require_failure_origin",
]
