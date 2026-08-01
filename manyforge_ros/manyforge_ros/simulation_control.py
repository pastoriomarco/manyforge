"""Pure simulator-control records plus the pre-I9 unavailable boundary.

The former live ROS client is retained in Git blob
``769e97215b0df885dd7002ca20ded0e0469b6ef7``.  Its effectful client,
one-shot runner, and dynamic import hook occupied lines 47-415, 418-463, and
519-522.  I9/I must restore those capabilities through the selected simulation
adapter; this module deliberately keeps only owner-neutral records, validation,
service selection, and pose math.
"""

from __future__ import annotations

import math
import sys
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from itertools import islice
from numbers import Real
from typing import Any, Final


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I9/I"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: simulator mutation requires the "
    f"{RESTORATION_GATE} restoration gate"
)
HISTORICAL_SOURCE_BLOB: Final = "769e97215b0df885dd7002ca20ded0e0469b6ef7"
HISTORICAL_TEST_BLOB: Final = "1d9c3d6bb93f7ccc7f377ef7e151e91f338f890f"

_MAX_ADAPTER_ROWS: Final = 4096
_MAX_IDENTIFIER_CHARS: Final = 1024

RESET_SERVICE_TYPE: Final = "simulation_interfaces/srv/ResetSimulation"
SET_SIMULATION_STATE_SERVICE_TYPE: Final = (
    "simulation_interfaces/srv/SetSimulationState"
)
STEP_SIMULATION_SERVICE_TYPE: Final = "simulation_interfaces/srv/StepSimulation"
SET_ENTITY_STATE_SERVICE_TYPE: Final = "simulation_interfaces/srv/SetEntityState"

_RESET_SCOPE_VALUES: Final = {"state"}
_SIMULATION_STATE_VALUES: Final = {"playing", "paused"}
_PREFERRED_SERVICES_BY_TYPE: Final = {
    RESET_SERVICE_TYPE: {
        "/reset_simulation",
        "/ResetSimulation",
        "/isaacsim/reset_simulation",
        "/isaacsim/ResetSimulation",
    },
    SET_SIMULATION_STATE_SERVICE_TYPE: {
        "/set_simulation_state",
        "/SetSimulationState",
        "/isaacsim/set_simulation_state",
        "/isaacsim/SetSimulationState",
    },
    STEP_SIMULATION_SERVICE_TYPE: {
        "/step_simulation",
        "/StepSimulation",
        "/isaacsim/step_simulation",
        "/isaacsim/StepSimulation",
    },
    SET_ENTITY_STATE_SERVICE_TYPE: {
        "/set_entity_state",
        "/SetEntityState",
        "/isaacsim/set_entity_state",
        "/isaacsim/SetEntityState",
    },
}


class SimulationControlUnavailableError(RuntimeError):
    """Raised when the pre-I9 one-shot mutation surface is invoked."""


@dataclass(frozen=True)
class SimulationResetResult:
    """Structured outcome record retained for future thin adapters."""

    success: bool
    service_name: str | None
    scope: str
    duration_s: float
    result_code: int | None = None
    result_message: str = ""
    error: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class SimulationControlResult:
    """Structured simulator-control outcome retained without an active owner."""

    success: bool
    operation: str
    service_name: str | None
    duration_s: float
    result_code: int | None = None
    result_message: str = ""
    error: str = ""
    state: str | None = None
    steps: int | None = None
    entity: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def reset_once(
    *,
    scope: str = "state",
    service_prefix: str = "auto",
    service_timeout_s: float = 3.0,
) -> SimulationResetResult:
    """Reject the old one-shot mutation entrypoint before any effects."""

    del scope, service_prefix, service_timeout_s
    raise SimulationControlUnavailableError(UNAVAILABLE_MESSAGE)


def main() -> int:
    """Fail before argument parsing, ROS import, or simulator mutation."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


def select_simulation_service_name(
    candidates: Sequence[str],
    *,
    service_type: str,
    service_prefix: str = "auto",
) -> str | None:
    """Select one service deterministically without reading the ROS graph."""

    normalized_candidates = _bounded_service_candidates(candidates)
    normalized_service_type = _bounded_text(service_type, label="service type")
    ordered = sorted(
        normalized_candidates,
        key=lambda name: _service_sort_key(name, normalized_service_type),
    )
    if not ordered:
        return None

    if service_prefix is None:
        normalized_prefix = "auto"
    else:
        if not isinstance(service_prefix, str):
            raise ValueError("service prefix must be a string")
        if len(service_prefix) > _MAX_IDENTIFIER_CHARS:
            raise ValueError("service prefix exceeds the adapter bound")
        normalized_prefix = service_prefix.strip() or "auto"
    if normalized_prefix == "auto":
        return ordered[0]

    prefix = "/" + normalized_prefix.strip("/")
    scoped = [
        name for name in ordered if name == prefix or name.startswith(prefix + "/")
    ]
    return scoped[0] if scoped else None


def normalize_reset_scope(scope: str) -> str:
    """Normalize the retained reset scope without ROS message constants."""

    normalized = _bounded_enum_token(scope, label="ResetSimulation scope")
    if normalized not in _RESET_SCOPE_VALUES:
        allowed = ", ".join(sorted(_RESET_SCOPE_VALUES))
        raise ValueError(f"ResetSimulation scope must be one of: {allowed}")
    return normalized


def normalize_simulation_state(state: str) -> str:
    """Normalize the historical play/pause aliases without mutating state."""

    state_key = _bounded_enum_token(state, label="SetSimulationState state")
    normalized = {
        "play": "playing",
        "playing": "playing",
        "pause": "paused",
        "paused": "paused",
    }.get(state_key)
    if normalized not in _SIMULATION_STATE_VALUES:
        allowed = ", ".join(sorted(_SIMULATION_STATE_VALUES))
        raise ValueError(f"SetSimulationState state must be one of: {allowed}")
    return normalized


def normalize_entity_pose(
    pose: dict[str, Any],
) -> tuple[
    str,
    tuple[float, float, float],
    tuple[float, float, float, float],
]:
    """Validate a pose payload and return fixed-shape numeric components."""

    if not isinstance(pose, dict):
        raise ValueError("SetEntityState pose must be a mapping")

    raw_frame_id = pose.get("frame_id")
    if raw_frame_id in (None, ""):
        raise ValueError("SetEntityState pose.frame_id is required")
    frame_id = _bounded_text(
        raw_frame_id,
        label="SetEntityState pose.frame_id",
    )

    position = pose.get("position_m")
    if not isinstance(position, list) or len(position) != 3:
        raise ValueError("SetEntityState pose.position_m must have length 3")

    orientation = pose.get("orientation_xyzw")
    if orientation is None and "orientation_rpy_deg" in pose:
        orientation = rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
    if not isinstance(orientation, list) or len(orientation) != 4:
        raise ValueError(
            "SetEntityState pose must include orientation_xyzw or orientation_rpy_deg"
        )

    normalized_position = tuple(
        _finite_float(value, label=f"SetEntityState pose.position_m[{index}]")
        for index, value in enumerate(position)
    )
    normalized_orientation = _normalized_quaternion(
        tuple(
            _finite_float(
                value,
                label=f"SetEntityState pose.orientation_xyzw[{index}]",
            )
            for index, value in enumerate(orientation)
        )
    )
    return frame_id, normalized_position, normalized_orientation


def rpy_deg_to_xyzw(rpy_deg: Any) -> list[float]:
    """Convert roll/pitch/yaw degrees to an XYZW quaternion."""

    if not isinstance(rpy_deg, list) or len(rpy_deg) != 3:
        raise ValueError("orientation_rpy_deg must have length 3")
    rpy = tuple(
        _finite_float(value, label=f"orientation_rpy_deg[{index}]")
        for index, value in enumerate(rpy_deg)
    )
    roll = math.radians(rpy[0])
    pitch = math.radians(rpy[1])
    yaw = math.radians(rpy[2])

    cr = math.cos(roll * 0.5)
    sr = math.sin(roll * 0.5)
    cp = math.cos(pitch * 0.5)
    sp = math.sin(pitch * 0.5)
    cy = math.cos(yaw * 0.5)
    sy = math.sin(yaw * 0.5)
    return list(
        _normalized_quaternion(
            (
                sr * cp * cy - cr * sp * sy,
                cr * sp * cy + sr * cp * sy,
                cr * cp * sy - sr * sp * cy,
                cr * cp * cy + sr * sp * sy,
            )
        )
    )


def _service_sort_key(service_name: str, service_type: str) -> tuple[int, str]:
    preferred = _PREFERRED_SERVICES_BY_TYPE.get(service_type, set())
    return (0 if service_name in preferred else 1, service_name)


def _bounded_service_candidates(candidates: Sequence[str]) -> tuple[str, ...]:
    if isinstance(candidates, (str, bytes, bytearray)):
        raise ValueError("service candidates must be a row sequence")
    try:
        rows = tuple(islice(iter(candidates), _MAX_ADAPTER_ROWS + 1))
    except TypeError as exc:
        raise ValueError("service candidates must be a row sequence") from exc
    if len(rows) > _MAX_ADAPTER_ROWS:
        raise ValueError("service candidates exceed the adapter bound")
    normalized = tuple(
        _bounded_text(candidate, label="service candidate") for candidate in rows
    )
    if len(set(normalized)) != len(normalized):
        raise ValueError("service candidates must be unique")
    return normalized


def _bounded_text(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    text = value.strip()
    if not text:
        raise ValueError(f"{label} must be non-empty")
    if len(text) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    return text


def _bounded_enum_token(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    return value.strip().lower()


def _finite_float(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a numeric value, not text or boolean")
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{label} must be numeric") from exc
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def _normalized_quaternion(
    quaternion: tuple[float, ...],
) -> tuple[float, float, float, float]:
    if len(quaternion) != 4:
        raise ValueError("orientation quaternion must have length 4")
    norm = math.hypot(*quaternion)
    if not math.isfinite(norm) or norm <= 1e-12:
        raise ValueError("orientation quaternion must have finite nonzero norm")
    return tuple(value / norm for value in quaternion)  # type: ignore[return-value]


if __name__ == "__main__":
    raise SystemExit(main())
