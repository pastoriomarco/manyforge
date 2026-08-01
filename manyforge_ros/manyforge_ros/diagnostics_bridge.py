"""ROS-neutral diagnostic mapping and thin ROS message translation.

The pure mapping remains usable during the composable-cell cutover.  The
former effectful bridge polled Composer-owned runtime endpoints and is not
retained as a hidden fallback; the module entrypoint stays unavailable until
the headless diagnostic projection exists.

The mapping produces one ``DiagnosticStatus`` per active backend
fault (level/values mapped from the backend fault taxonomy) and an OK
heartbeat when healthy. The heartbeat is what drives PASSED/healing on
AUTOSAR-DEM-style consumers (e.g. ros2_medkit's stock diagnostic bridge),
so it is not optional.

Deliberately medkit-free: this module depends only on standard
``diagnostic_msgs`` and the ManyForge fault taxonomy. Vendor-specific
enrichment (discovery hints, direct fault reporting) lives in the separate
``manyforge_medkit_adapter`` package.

The mapping core (``build_diagnostic_statuses``) is pure data-in/data-out and
``statuses_to_msg`` is the only retained ROS translation seam.
"""

from __future__ import annotations

import json
import math
import re
import sys
from typing import Any, Final, Optional

from manyforge_behavior.fault_taxonomy import (
    ROS2_CONTROL_BACKEND_FAULT_CLASSES,
    classify_digest_fault,
)

# diagnostic_msgs/DiagnosticStatus levels (bytes); kept as ints here so the
# mapping core stays import-free.
LEVEL_OK = 0
LEVEL_WARN = 1
LEVEL_ERROR = 2
LEVEL_STALE = 3

# Defensive bounds for this retained projection seam. They are not a
# declaration of the future H0 wire contract.
MAX_ID_CHARS: Final = 256
MAX_DETAIL_CHARS: Final = 2048
MAX_DETAIL_ROWS: Final = 64
MAX_STATUS_ROWS: Final = 64
MAX_STATUS_VALUE_ROWS: Final = 64
MAX_FAULT_CODE_CHARS: Final = 64
_MAX_JSON_DEPTH: Final = 8
_MAX_JSON_NODES: Final = 256

UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I5/H0"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: ROS diagnostics publication requires the "
    f"{RESTORATION_GATE} headless diagnostic projection"
)

_SEVERITY_TO_LEVEL = {
    "INFO": LEVEL_OK,
    "WARN": LEVEL_WARN,
    "ERROR": LEVEL_ERROR,
    # diagnostic_msgs has no CRITICAL level; CRITICAL maps to ERROR and the
    # operational severity is preserved in values["manyforge_severity"]
    # (medkit-style consumers needing debounce bypass use the medkit
    # adapter, which reports CRITICAL natively).
    "CRITICAL": LEVEL_ERROR,
}

_SURFACE_TO_COMPONENT = {
    "ros2_control_execution_backend": "execution",
    "command_gripper": "gripper",
}

_FAULT_CODE_SAFE = re.compile(r"[^A-Z0-9_]")
_RESERVED_VALUE_KEYS = frozenset(
    {"state", "manyforge_severity", "fault_code", "reason_code"}
)


def _bounded_text(
    value: object,
    *,
    label: str,
    limit: int,
    allow_empty: bool = False,
    identifier: bool = False,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise ValueError(f"{label} must be {qualifier}")
    if len(value) > limit:
        raise ValueError(f"{label} exceeds the {limit}-character bound")
    if identifier and value and value != value.strip():
        raise ValueError(f"{label} must not contain surrounding whitespace")
    return value


def _optional_bounded_text(
    value: object,
    *,
    label: str,
    limit: int,
    identifier: bool = False,
) -> str | None:
    if value is None:
        return None
    return _bounded_text(
        value,
        label=label,
        limit=limit,
        identifier=identifier,
    )


def _validate_json_value(
    value: object,
    *,
    label: str,
    depth: int,
    budget: list[int],
) -> None:
    budget[0] -= 1
    if budget[0] < 0:
        raise ValueError(
            f"{label} exceeds the {_MAX_JSON_NODES}-node JSON value bound"
        )
    if depth > _MAX_JSON_DEPTH:
        raise ValueError(
            f"{label} exceeds the {_MAX_JSON_DEPTH}-level JSON value bound"
        )
    if value is None or type(value) is bool:
        return
    if type(value) is str:
        _bounded_text(
            value,
            label=label,
            limit=MAX_DETAIL_CHARS,
            allow_empty=True,
        )
        return
    if type(value) is int:
        if not -(2**63) <= value <= (2**63) - 1:
            raise ValueError(f"{label} integer is outside the signed 64-bit bound")
        return
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError(f"{label} must be finite")
        return
    if type(value) is list:
        if len(value) > MAX_DETAIL_ROWS:
            raise ValueError(
                f"{label} exceeds the {MAX_DETAIL_ROWS}-row JSON array bound"
            )
        for index, item in enumerate(value):
            _validate_json_value(
                item,
                label=f"{label}[{index}]",
                depth=depth + 1,
                budget=budget,
            )
        return
    if type(value) is dict:
        if len(value) > MAX_DETAIL_ROWS:
            raise ValueError(
                f"{label} exceeds the {MAX_DETAIL_ROWS}-row JSON object bound"
            )
        for key, item in value.items():
            _bounded_text(
                key,
                label=f"{label} key",
                limit=MAX_ID_CHARS,
                identifier=True,
            )
            _validate_json_value(
                item,
                label=f"{label}.{key}",
                depth=depth + 1,
                budget=budget,
            )
        return
    raise ValueError(f"{label} must be JSON-compatible")


def _project_detail_value(value: object, *, label: str) -> str:
    _validate_json_value(value, label=label, depth=0, budget=[_MAX_JSON_NODES])
    if type(value) is str:
        projected = value
    else:
        projected = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    return _bounded_text(
        projected,
        label=f"{label} projection",
        limit=MAX_DETAIL_CHARS,
        allow_empty=True,
    )


def _select_optional_alias(
    value: dict[str, Any],
    *,
    camel_key: str,
    snake_key: str,
    label: str,
) -> object:
    if camel_key in value and snake_key in value:
        raise ValueError(f"{label} must use exactly one supported field spelling")
    if camel_key in value:
        return value[camel_key]
    return value.get(snake_key)


def _validated_digest_fault(digest: object):
    """Validate the retained snapshot input before using its classifier.

    This bounds the historical JSON-shaped projection only; it does not
    define or anticipate the future H0 transport contract.
    """

    if type(digest) is not dict:
        raise ValueError("digest must be a JSON object")

    if "state" not in digest:
        raise ValueError("digest.state is required")
    _bounded_text(
        digest["state"],
        label="digest.state",
        limit=MAX_ID_CHARS,
        identifier=True,
    )
    if "error" not in digest:
        raise ValueError("digest.error is required")
    error_message = digest["error"]
    if error_message is not None:
        _bounded_text(
            error_message,
            label="digest.error",
            limit=MAX_DETAIL_CHARS,
        )
    error_code = _select_optional_alias(
        digest,
        camel_key="errorCode",
        snake_key="error_code",
        label="digest error code",
    )
    _optional_bounded_text(
        error_code,
        label="digest error code",
        limit=MAX_ID_CHARS,
        identifier=True,
    )
    details = _select_optional_alias(
        digest,
        camel_key="errorDetails",
        snake_key="error_details",
        label="digest error details",
    )
    if details is None:
        details = {}
    if type(details) is not dict:
        raise ValueError("digest error details must be a JSON object")
    if len(details) > MAX_DETAIL_ROWS:
        raise ValueError(
            f"digest error details exceeds the {MAX_DETAIL_ROWS}-row bound"
        )

    for key, value in details.items():
        _bounded_text(
            key,
            label="digest error detail key",
            limit=MAX_ID_CHARS,
            identifier=True,
        )
        # reason_code is the classified source and is emitted once by the
        # reducer; every other reserved key would spoof a reducer-owned row.
        if key in _RESERVED_VALUE_KEYS and key != "reason_code":
            raise ValueError(
                f"digest error detail key {key!r} conflicts with a reserved status key"
            )
        _project_detail_value(value, label=f"digest error detail {key!r}")

    for key in ("reason_code", "backend", "surface"):
        if key in details:
            _optional_bounded_text(
                details[key],
                label=f"digest error detail {key}",
                limit=MAX_ID_CHARS,
                identifier=True,
            )
            if key == "reason_code" and details[key] == "":
                raise ValueError("digest error detail reason_code must not be empty")

    fault = classify_digest_fault(digest)
    if (error_message is not None) != (fault.reason_code is not None):
        raise ValueError(
            "digest error message and structured reason must be present together"
        )
    return fault


def external_fault_code(reason_code: str) -> str:
    """MANYFORGE_-prefixed UPPER_SNAKE fault code (<=64 chars, [A-Z0-9_]).

    Known taxonomy tokens use their declared alias; anything else is
    sanitized to the same convention so downstream aggregation never sees a
    malformed code.
    """
    reason_code = _bounded_text(
        reason_code,
        label="reason_code",
        limit=MAX_ID_CHARS,
        identifier=True,
    )
    fault = ROS2_CONTROL_BACKEND_FAULT_CLASSES.get(reason_code)
    if fault is not None:
        return fault.external_alias
    sanitized = _FAULT_CODE_SAFE.sub("_", reason_code.upper()).strip("_")
    return f"MANYFORGE_{sanitized or 'UNKNOWN_FAULT'}"[:MAX_FAULT_CODE_CHARS]


def fault_severity(reason_code: str) -> str:
    reason_code = _bounded_text(
        reason_code,
        label="reason_code",
        limit=MAX_ID_CHARS,
        identifier=True,
    )
    fault = ROS2_CONTROL_BACKEND_FAULT_CLASSES.get(reason_code)
    return fault.severity if fault is not None else "ERROR"


def build_diagnostic_statuses(
    digest: Optional[dict[str, Any]],
    *,
    deployment_name: str,
) -> list[dict[str, Any]]:
    """Map an execution digest to DiagnosticStatus-shaped dicts.

    Returns dicts with keys: name, level, message, hardware_id, values
    (list of (key, value) string pairs). Source unavailable (digest ``None``)
    is reported as a WARN status rather than silence, so consumers can
    distinguish "healthy" from "unobserved".
    """
    deployment_name = _bounded_text(
        deployment_name,
        label="deployment_name",
        limit=MAX_ID_CHARS,
        identifier=True,
    )
    if digest is None:
        return [
            {
                "name": "manyforge/bridge",
                "level": LEVEL_WARN,
                "message": "Runtime diagnostics unavailable; state unobserved",
                "hardware_id": deployment_name,
                "values": [("observed", "false")],
            }
        ]

    fault = _validated_digest_fault(digest)
    error_details = fault.details
    error_message = fault.message
    reason_code = fault.reason_code
    state = fault.state

    if not fault.active:
        return [
            {
                "name": "manyforge/execution",
                "level": LEVEL_OK,
                "message": "ok",
                "hardware_id": deployment_name,
                "values": [("state", state or "unknown")],
            }
        ]

    component = "runtime"
    component = _SURFACE_TO_COMPONENT.get(
        error_details.get("surface") or "", "runtime"
    )
    if error_details.get("backend") == "ros2_control":
        component = "execution"

    severity = fault_severity(reason_code) if reason_code else "ERROR"
    values: list[tuple[str, str]] = [
        ("state", state or "unknown"),
        ("manyforge_severity", severity),
        ("fault_code", external_fault_code(reason_code or "unknown_fault")),
    ]
    if reason_code:
        values.append(("reason_code", reason_code))
    for key in sorted(error_details):
        if key == "reason_code":
            continue
        values.append(
            (
                key,
                _project_detail_value(
                    error_details[key], label=f"digest error detail {key!r}"
                ),
            )
        )
    if len(values) > MAX_STATUS_VALUE_ROWS:
        raise ValueError(
            f"diagnostic values exceeds the {MAX_STATUS_VALUE_ROWS}-row bound"
        )

    return [
        {
            "name": f"manyforge/{component}",
            "level": _SEVERITY_TO_LEVEL.get(severity, LEVEL_ERROR),
            "message": error_message or "ManyForge backend fault",
            "hardware_id": deployment_name,
            "values": values,
        }
    ]


def statuses_to_msg(statuses: list[dict[str, Any]], stamp) -> Any:
    """Build a DiagnosticArray message (requires diagnostic_msgs)."""
    if type(statuses) is not list:
        raise ValueError("statuses must be a list")
    if len(statuses) > MAX_STATUS_ROWS:
        raise ValueError(f"statuses exceeds the {MAX_STATUS_ROWS}-row bound")

    expected_fields = {"name", "level", "message", "hardware_id", "values"}
    validated: list[tuple[int, str, str, str, list[tuple[str, str]]]] = []
    status_names: set[str] = set()
    for index, status in enumerate(statuses):
        if type(status) is not dict or set(status) != expected_fields:
            raise ValueError(
                f"statuses[{index}] must contain exactly {sorted(expected_fields)}"
            )
        name = _bounded_text(
            status["name"],
            label=f"statuses[{index}].name",
            limit=MAX_ID_CHARS,
            identifier=True,
        )
        if name in status_names:
            raise ValueError(f"statuses contains duplicate name {name!r}")
        status_names.add(name)
        level = status["level"]
        if type(level) is not int or level not in (
            LEVEL_OK,
            LEVEL_WARN,
            LEVEL_ERROR,
            LEVEL_STALE,
        ):
            raise ValueError(f"statuses[{index}].level must be an integer from 0 to 3")
        message = _bounded_text(
            status["message"],
            label=f"statuses[{index}].message",
            limit=MAX_DETAIL_CHARS,
            allow_empty=True,
        )
        hardware_id = _bounded_text(
            status["hardware_id"],
            label=f"statuses[{index}].hardware_id",
            limit=MAX_ID_CHARS,
            identifier=True,
        )
        values = status["values"]
        if type(values) is not list:
            raise ValueError(f"statuses[{index}].values must be a list")
        if len(values) > MAX_STATUS_VALUE_ROWS:
            raise ValueError(
                f"statuses[{index}].values exceeds the "
                f"{MAX_STATUS_VALUE_ROWS}-row bound"
            )
        validated_values: list[tuple[str, str]] = []
        value_keys: set[str] = set()
        for value_index, row in enumerate(values):
            if type(row) is not tuple or len(row) != 2:
                raise ValueError(
                    f"statuses[{index}].values[{value_index}] must be a "
                    "two-string tuple"
                )
            key = _bounded_text(
                row[0],
                label=f"statuses[{index}].values[{value_index}].key",
                limit=MAX_ID_CHARS,
                identifier=True,
            )
            value = _bounded_text(
                row[1],
                label=f"statuses[{index}].values[{value_index}].value",
                limit=MAX_DETAIL_CHARS,
                allow_empty=True,
            )
            if key in value_keys:
                raise ValueError(
                    f"statuses[{index}].values contains duplicate key {key!r}"
                )
            value_keys.add(key)
            validated_values.append((key, value))
        validated.append((level, name, message, hardware_id, validated_values))

    from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue

    array = DiagnosticArray()
    array.header.stamp = stamp
    for level, name, detail, hardware_id, values in validated:
        message = DiagnosticStatus()
        message.level = bytes([level])
        message.name = name
        message.message = detail
        message.hardware_id = hardware_id
        message.values = [
            KeyValue(key=key, value=value) for key, value in values
        ]
        array.status.append(message)
    return array


def main(argv: list[str] | None = None) -> int:
    """Fail before argument parsing, source access, ROS, or publication."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
