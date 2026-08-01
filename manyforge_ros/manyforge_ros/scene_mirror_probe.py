"""Pure scene-snapshot records plus the pre-I9 mirror boundary.

The former live ROS scene client is retained in Git blob
``07de241694d2e7738a55d8c6c74bab1ef5d11b20``.  Its effectful client, CLI, and
dynamic import hook occupied lines 88-424, 427-454, and 473-476.  I9/I must
restore service discovery and capture through the selected simulation adapter.
The records and mechanical translations remain here; in particular,
``PoseSnapshot`` is also the owner-neutral value consumed by the pose-source
sidecar.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from itertools import islice
from math import hypot, isfinite
from numbers import Real
from typing import Any, Final


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I9/I"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live simulation scene mirroring requires the "
    f"{RESTORATION_GATE} restoration gate"
)
HISTORICAL_SOURCE_BLOB: Final = "07de241694d2e7738a55d8c6c74bab1ef5d11b20"
HISTORICAL_TEST_BLOB: Final = "a911cb32c772fd1e672676eb8d319213eebab8cb"

_MAX_GRAPH_ROWS: Final = 4096
_MAX_IDENTIFIER_CHARS: Final = 1024

_SERVICE_TYPES: Final = {
    "get_simulator_features": "simulation_interfaces/srv/GetSimulatorFeatures",
    "get_entities": "simulation_interfaces/srv/GetEntities",
    "get_entity_info": "simulation_interfaces/srv/GetEntityInfo",
    "get_entity_state": "simulation_interfaces/srv/GetEntityState",
    "get_entity_bounds": "simulation_interfaces/srv/GetEntityBounds",
}
_PREFERRED_SERVICE_NAMES: Final = {
    "/get_simulator_features",
    "/get_entities",
    "/get_entity_info",
    "/get_entity_state",
    "/get_entity_bounds",
    "/isaacsim/GetSimulatorFeatures",
    "/isaacsim/GetEntities",
    "/isaacsim/GetEntityInfo",
    "/isaacsim/GetEntityState",
    "/isaacsim/GetEntityBounds",
}


@dataclass(frozen=True)
class PoseSnapshot:
    frame_id: str
    stamp_sec: float | None
    position_xyz: tuple[float, float, float]
    orientation_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True)
class BoundsSnapshot:
    type_code: int
    type_name: str
    points_xyz: tuple[tuple[float, float, float], ...]


@dataclass(frozen=True)
class EntitySnapshot:
    name: str
    category_code: int | None = None
    category_name: str | None = None
    description: str | None = None
    tags: tuple[str, ...] = ()
    pose: PoseSnapshot | None = None
    bounds: BoundsSnapshot | None = None
    errors: tuple[str, ...] = ()


@dataclass(frozen=True)
class SceneMirrorSnapshot:
    service_names: dict[str, str | None]
    features: tuple[str, ...]
    spawn_formats: tuple[str, ...]
    custom_info: str
    entities: tuple[EntitySnapshot, ...]
    errors: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class SimulationServiceNames:
    get_simulator_features: str | None = None
    get_entities: str | None = None
    get_entity_info: str | None = None
    get_entity_state: str | None = None
    get_entity_bounds: str | None = None

    def __post_init__(self) -> None:
        for field_name in _SERVICE_TYPES:
            value = getattr(self, field_name)
            if value is not None:
                object.__setattr__(
                    self,
                    field_name,
                    _bounded_text(value, label=field_name),
                )

    def with_discovered(
        self,
        discovered: "SimulationServiceNames",
    ) -> "SimulationServiceNames":
        """Keep configured names and fill only missing fields from discovery."""

        return SimulationServiceNames(
            get_simulator_features=(
                self.get_simulator_features or discovered.get_simulator_features
            ),
            get_entities=self.get_entities or discovered.get_entities,
            get_entity_info=self.get_entity_info or discovered.get_entity_info,
            get_entity_state=self.get_entity_state or discovered.get_entity_state,
            get_entity_bounds=self.get_entity_bounds or discovered.get_entity_bounds,
        )

    def to_dict(self) -> dict[str, str | None]:
        return {
            "get_simulator_features": self.get_simulator_features,
            "get_entities": self.get_entities,
            "get_entity_info": self.get_entity_info,
            "get_entity_state": self.get_entity_state,
            "get_entity_bounds": self.get_entity_bounds,
        }


def select_simulation_service_names(
    services: Sequence[tuple[str, Sequence[str]]],
) -> SimulationServiceNames:
    """Select standard simulation services from an already-read graph."""

    by_type: dict[str, list[str]] = {}
    graph_rows = _bounded_items(services, label="service graph")
    service_type_rows = 0
    for row in graph_rows:
        try:
            service_name, service_types = row
        except (TypeError, ValueError) as exc:
            raise ValueError(
                "service graph rows must contain a name and type sequence"
            ) from exc
        normalized_name = _bounded_text(service_name, label="service name")
        normalized_types = _bounded_items(
            service_types,
            label=f"service types for {normalized_name}",
        )
        service_type_rows += len(normalized_types)
        if service_type_rows > _MAX_GRAPH_ROWS:
            raise ValueError("service graph type rows exceed the adapter bound")
        for service_type in normalized_types:
            normalized_type = _bounded_text(service_type, label="service type")
            by_type.setdefault(normalized_type, []).append(normalized_name)

    selected: dict[str, str | None] = {}
    for field_name, service_type in _SERVICE_TYPES.items():
        candidates = sorted(by_type.get(service_type, []), key=_service_sort_key)
        selected[field_name] = candidates[0] if candidates else None
    return SimulationServiceNames(**selected)


def feature_enabled_or_unknown(feature_set: set[str], feature_name: str) -> bool:
    """Allow an operation when capability reporting is absent or affirmative."""

    if len(feature_set) > _MAX_GRAPH_ROWS:
        raise ValueError("simulator feature rows exceed the adapter bound")
    normalized_features = {
        _bounded_text(feature, label="simulator feature") for feature in feature_set
    }
    normalized_name = _bounded_text(feature_name, label="simulator feature name")
    return not normalized_features or normalized_name in normalized_features


def pose_snapshot_from_state(state: Any) -> PoseSnapshot:
    """Mechanically translate a state-shaped value without ROS access."""

    stamp = state.header.stamp
    pose = state.pose
    stamp_sec = _timestamp_seconds(stamp)
    return PoseSnapshot(
        frame_id=_bounded_text(state.header.frame_id, label="pose frame_id"),
        stamp_sec=stamp_sec,
        position_xyz=(
            _finite_float(pose.position.x, label="pose position.x"),
            _finite_float(pose.position.y, label="pose position.y"),
            _finite_float(pose.position.z, label="pose position.z"),
        ),
        orientation_xyzw=_normalized_quaternion(
            (
                _finite_float(pose.orientation.x, label="pose orientation.x"),
                _finite_float(pose.orientation.y, label="pose orientation.y"),
                _finite_float(pose.orientation.z, label="pose orientation.z"),
                _finite_float(pose.orientation.w, label="pose orientation.w"),
            )
        ),
    )


def bounds_snapshot_from_value(bounds: Any, *, bounds_type: type) -> BoundsSnapshot:
    """Mechanically translate a bounds-shaped value without ROS access."""

    type_code = _exact_int(bounds.type, label="bounds type")
    points = _bounded_items(bounds.points, label="bounds points")
    return BoundsSnapshot(
        type_code=type_code,
        type_name=constant_name(bounds_type, type_code),
        points_xyz=tuple(
            (
                _finite_float(point.x, label="bounds point.x"),
                _finite_float(point.y, label="bounds point.y"),
                _finite_float(point.z, label="bounds point.z"),
            )
            for point in points
        ),
    )


def constant_name(constants: type, value: int) -> str:
    """Return a stable symbolic name for an integer message constant."""

    normalized_value = _exact_int(value, label="message constant value")
    constant_rows = vars(constants)
    if len(constant_rows) > _MAX_GRAPH_ROWS:
        raise ValueError("message constant rows exceed the adapter bound")
    for name, candidate in constant_rows.items():
        if (
            name.isupper()
            and type(candidate) is int
            and candidate == normalized_value
        ):
            return _bounded_text(name, label="message constant name")
    return f"UNKNOWN_{normalized_value}"


def constant_names(constants: type, values: Sequence[int]) -> list[str]:
    """Translate integer message constants without importing their package."""

    bounded_values = _bounded_items(values, label="message constant values")
    return [constant_name(constants, value) for value in bounded_values]


def main() -> int:
    """Fail before argument parsing, ROS import, or simulator service discovery."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


def _service_sort_key(service_name: str) -> tuple[int, str]:
    return (0 if service_name in _PREFERRED_SERVICE_NAMES else 1, service_name)


def _bounded_items(value: Any, *, label: str) -> tuple[Any, ...]:
    if isinstance(value, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be a row sequence")
    try:
        items = tuple(islice(iter(value), _MAX_GRAPH_ROWS + 1))
    except TypeError as exc:
        raise ValueError(f"{label} must be a row sequence") from exc
    if len(items) > _MAX_GRAPH_ROWS:
        raise ValueError(f"{label} exceed the adapter bound")
    return items


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


def _finite_float(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a numeric value, not text or boolean")
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{label} must be numeric") from exc
    if not isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def _exact_int(value: Any, *, label: str) -> int:
    if type(value) is not int:
        raise ValueError(f"{label} must be an integer")
    return value


def _timestamp_seconds(stamp: Any) -> float:
    sec = _exact_int(getattr(stamp, "sec", None), label="timestamp sec")
    nanosec = _exact_int(
        getattr(stamp, "nanosec", None),
        label="timestamp nanosec",
    )
    if not 0 <= nanosec < 1_000_000_000:
        raise ValueError("timestamp nanosec must be in [0, 1000000000)")
    try:
        value = float(sec) + float(nanosec) * 1e-9
    except OverflowError as exc:
        raise ValueError("timestamp must be finite") from exc
    if not isfinite(value):
        raise ValueError("timestamp must be finite")
    return value


def _normalized_quaternion(
    quaternion: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    norm = hypot(*quaternion)
    if not isfinite(norm) or norm <= 1e-12:
        raise ValueError("pose orientation quaternion must have finite nonzero norm")
    return tuple(value / norm for value in quaternion)  # type: ignore[return-value]


if __name__ == "__main__":
    raise SystemExit(main())
