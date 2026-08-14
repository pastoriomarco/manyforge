"""Read-only, bounded ``simulation_interfaces`` scene observer.

The probe discovers the selected simulator owner's standard read services and
captures metadata, pose, and bounds without mutating the world or becoming a
second scene authority.
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import time
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from itertools import islice
from numbers import Real
from typing import Any, Final


HISTORICAL_SOURCE_BLOB: Final = "07de241694d2e7738a55d8c6c74bab1ef5d11b20"
HISTORICAL_TEST_BLOB: Final = "a911cb32c772fd1e672676eb8d319213eebab8cb"

_MAX_GRAPH_ROWS: Final = 4096
_MAX_IDENTIFIER_CHARS: Final = 1024
_MAX_TEXT_CHARS: Final = 4096

_SERVICE_TYPES: Final = {
    "get_simulator_features": "simulation_interfaces/srv/GetSimulatorFeatures",
    "get_entities": "simulation_interfaces/srv/GetEntities",
    "get_entity_info": "simulation_interfaces/srv/GetEntityInfo",
    "get_entity_state": "simulation_interfaces/srv/GetEntityState",
    "get_entity_bounds": "simulation_interfaces/srv/GetEntityBounds",
}
_PREFERRED_SERVICE_LEAVES: Final = {
    "get_simulator_features": "get_simulator_features",
    "get_entities": "get_entities",
    "get_entity_info": "get_entity_info",
    "get_entity_state": "get_entity_state",
    "get_entity_bounds": "get_entity_bounds",
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


class SimulationSceneMirrorClient:
    """Capture a read-only snapshot from one simulator service graph."""

    def __init__(
        self,
        node: object,
        *,
        service_names: SimulationServiceNames | None = None,
        service_timeout_s: float = 2.0,
    ) -> None:
        if not callable(getattr(node, "get_service_names_and_types", None)):
            raise TypeError("node must expose the rclpy graph API")
        self.node = node
        self.service_names = service_names or SimulationServiceNames()
        self.service_timeout_s = _bounded_timeout(service_timeout_s)
        self._rclpy = importlib.import_module("rclpy")
        self._srv = importlib.import_module("simulation_interfaces.srv")
        self._msg = importlib.import_module("simulation_interfaces.msg")

    def capture(
        self,
        *,
        entity_filter: str = "",
        entity_names: Sequence[str] | None = None,
        entity_limit: int | None = None,
        include_info: bool = True,
        include_state: bool = True,
        include_bounds: bool = True,
    ) -> SceneMirrorSnapshot:
        """Capture one bounded service-backed scene snapshot."""

        normalized_filter = _bounded_optional_text(
            entity_filter,
            label="entity filter",
            maximum=_MAX_IDENTIFIER_CHARS,
        )
        explicit_entities = (
            None
            if entity_names is None
            else _bounded_unique_names(entity_names, label="entity names")
        )
        if entity_limit is not None and (
            type(entity_limit) is not int
            or not 0 <= entity_limit <= _MAX_GRAPH_ROWS
        ):
            raise ValueError(
                f"entity_limit must be an integer in [0, {_MAX_GRAPH_ROWS}]"
            )
        for name, value in (
            ("include_info", include_info),
            ("include_state", include_state),
            ("include_bounds", include_bounds),
        ):
            if type(value) is not bool:
                raise TypeError(f"{name} must be an exact bool")

        required = {"get_simulator_features"}
        if explicit_entities is None:
            required.add("get_entities")
        if include_info:
            required.add("get_entity_info")
        if include_state:
            required.add("get_entity_state")
        if include_bounds:
            required.add("get_entity_bounds")
        service_names = self._resolve_service_names(required)
        features, spawn_formats, custom_info, feature_errors = (
            self._capture_features(service_names)
        )

        if explicit_entities is not None:
            entities = explicit_entities
            entity_errors: tuple[str, ...] = ()
        elif service_names.get_entities is None:
            return SceneMirrorSnapshot(
                service_names=service_names.to_dict(),
                features=features,
                spawn_formats=spawn_formats,
                custom_info=custom_info,
                entities=(),
                errors=feature_errors + ("GetEntities service not available",),
            )
        else:
            entities, entity_errors = self._capture_entity_names(
                service_names.get_entities,
                entity_filter=normalized_filter,
            )
        if entity_limit is not None:
            entities = entities[:entity_limit]

        feature_set = set(features)
        query_info = (
            include_info
            and service_names.get_entity_info is not None
            and feature_enabled_or_unknown(feature_set, "ENTITY_INFO_GETTING")
        )
        query_state = (
            include_state
            and service_names.get_entity_state is not None
            and feature_enabled_or_unknown(feature_set, "ENTITY_STATE_GETTING")
        )
        query_bounds = (
            include_bounds
            and service_names.get_entity_bounds is not None
            and feature_enabled_or_unknown(feature_set, "ENTITY_BOUNDS")
        )
        snapshots = tuple(
            self._capture_entity(
                entity,
                service_names=service_names,
                query_info=query_info,
                query_state=query_state,
                query_bounds=query_bounds,
            )
            for entity in entities
        )
        missing_requested = tuple(
            f"{field_name} service not available"
            for field_name in sorted(required)
            if getattr(service_names, field_name) is None
        )
        return SceneMirrorSnapshot(
            service_names=service_names.to_dict(),
            features=features,
            spawn_formats=spawn_formats,
            custom_info=custom_info,
            entities=snapshots,
            errors=feature_errors + entity_errors + missing_requested,
        )

    def _resolve_service_names(
        self,
        required: set[str],
    ) -> SimulationServiceNames:
        deadline = time.monotonic() + self.service_timeout_s
        while True:
            graph_by_type = _simulation_service_graph_by_type(
                self.node.get_service_names_and_types()
            )
            configured = _validated_configured_service_names(
                self.service_names,
                graph_by_type,
            )
            selected = configured.with_discovered(
                _select_simulation_service_names(graph_by_type)
            )
            if all(getattr(selected, field) is not None for field in required):
                return selected
            if time.monotonic() >= deadline:
                return selected
            time.sleep(0.05)

    def _capture_features(
        self,
        service_names: SimulationServiceNames,
    ) -> tuple[tuple[str, ...], tuple[str, ...], str, tuple[str, ...]]:
        if service_names.get_simulator_features is None:
            return (), (), "", ("GetSimulatorFeatures service not available",)
        try:
            response = self._call_service(
                service_names.get_simulator_features,
                self._srv.GetSimulatorFeatures,
                self._srv.GetSimulatorFeatures.Request(),
            )
            features_msg = response.features
            features = tuple(
                constant_names(self._msg.SimulatorFeatures, features_msg.features)
            )
            spawn_formats = _bounded_unique_names(
                features_msg.spawn_formats,
                label="simulator spawn formats",
            )
            custom_info = _bounded_optional_text(
                features_msg.custom_info,
                label="simulator custom info",
                maximum=_MAX_TEXT_CHARS,
            )
            return features, spawn_formats, custom_info, ()
        except Exception as error:
            return (), (), "", (
                f"GetSimulatorFeatures failed: {_safe_error(error)}",
            )

    def _capture_entity_names(
        self,
        service_name: str,
        *,
        entity_filter: str,
    ) -> tuple[tuple[str, ...], tuple[str, ...]]:
        request = self._srv.GetEntities.Request()
        request.filters.filter = entity_filter
        try:
            response = self._call_service(
                service_name,
                self._srv.GetEntities,
                request,
            )
            if not self._result_ok(response.result):
                return (), (
                    f"GetEntities failed: {self._result_message(response.result)}",
                )
            return (
                _bounded_unique_names(response.entities, label="entity names"),
                (),
            )
        except Exception as error:
            return (), (f"GetEntities failed: {_safe_error(error)}",)

    def _capture_entity(
        self,
        entity: str,
        *,
        service_names: SimulationServiceNames,
        query_info: bool,
        query_state: bool,
        query_bounds: bool,
    ) -> EntitySnapshot:
        errors: list[str] = []
        category_code: int | None = None
        category_name: str | None = None
        description: str | None = None
        tags: tuple[str, ...] = ()
        pose: PoseSnapshot | None = None
        bounds: BoundsSnapshot | None = None

        if query_info:
            try:
                response = self._call_entity_service(
                    service_names.get_entity_info,
                    self._srv.GetEntityInfo,
                    entity,
                )
                if self._result_ok(response.result):
                    category_code = int(response.info.category.category)
                    category_name = constant_name(
                        self._msg.EntityCategory,
                        category_code,
                    )
                    description = _bounded_optional_text(
                        response.info.description,
                        label="entity description",
                        maximum=_MAX_TEXT_CHARS,
                    )
                    tags = _bounded_unique_names(
                        response.info.tags,
                        label="entity tags",
                    )
                else:
                    errors.append(
                        "GetEntityInfo failed: "
                        + self._result_message(response.result)
                    )
            except Exception as error:
                errors.append(f"GetEntityInfo failed: {_safe_error(error)}")
        if query_state:
            try:
                response = self._call_entity_service(
                    service_names.get_entity_state,
                    self._srv.GetEntityState,
                    entity,
                )
                if self._result_ok(response.result):
                    pose = pose_snapshot_from_state(response.state)
                else:
                    errors.append(
                        "GetEntityState failed: "
                        + self._result_message(response.result)
                    )
            except Exception as error:
                errors.append(f"GetEntityState failed: {_safe_error(error)}")
        if query_bounds:
            try:
                response = self._call_entity_service(
                    service_names.get_entity_bounds,
                    self._srv.GetEntityBounds,
                    entity,
                )
                if self._result_ok(response.result):
                    bounds = bounds_snapshot_from_value(
                        response.bounds,
                        bounds_type=self._msg.Bounds,
                    )
                else:
                    errors.append(
                        "GetEntityBounds failed: "
                        + self._result_message(response.result)
                    )
            except Exception as error:
                errors.append(f"GetEntityBounds failed: {_safe_error(error)}")
        return EntitySnapshot(
            name=entity,
            category_code=category_code,
            category_name=category_name,
            description=description,
            tags=tags,
            pose=pose,
            bounds=bounds,
            errors=tuple(errors),
        )

    def _call_entity_service(
        self,
        service_name: str | None,
        service_type: type,
        entity: str,
    ) -> object:
        if service_name is None:
            raise RuntimeError(f"{service_type.__name__} service is unavailable")
        request = service_type.Request()
        request.entity = entity
        return self._call_service(service_name, service_type, request)

    def _call_service(
        self,
        service_name: str,
        service_type: type,
        request: object,
    ) -> object:
        client = self.node.create_client(service_type, service_name)
        try:
            if not client.wait_for_service(timeout_sec=self.service_timeout_s):
                raise TimeoutError(f"service unavailable: {service_name}")
            future = client.call_async(request)
            return self._wait_for_future(future)
        finally:
            self.node.destroy_client(client)

    def _wait_for_future(self, future: object) -> object:
        deadline = time.monotonic() + self.service_timeout_s
        executor = getattr(self.node, "executor", None)
        if executor is None or not getattr(executor, "is_spinning", False):
            self._rclpy.spin_until_future_complete(
                self.node,
                future,
                timeout_sec=max(0.0, deadline - time.monotonic()),
            )
        else:
            while time.monotonic() < deadline and not future.done():
                time.sleep(0.01)
        if not future.done():
            raise TimeoutError("simulation_interfaces response timed out")
        response = future.result()
        if response is None:
            raise RuntimeError("simulation_interfaces returned no response")
        return response

    @staticmethod
    def _result_ok(result: object) -> bool:
        return int(result.result) == int(result.RESULT_OK)

    @staticmethod
    def _result_message(result: object) -> str:
        return _safe_error(f"{result.result}: {result.error_message}")


def select_simulation_service_names(
    services: Sequence[tuple[str, Sequence[str]]],
) -> SimulationServiceNames:
    """Select each exact service type deterministically from a bounded graph."""

    return _select_simulation_service_names(
        _simulation_service_graph_by_type(services)
    )


def _simulation_service_graph_by_type(
    services: Sequence[tuple[str, Sequence[str]]],
) -> dict[str, tuple[str, ...]]:
    """Normalize one bounded graph without trusting configured service names."""

    by_type: dict[str, list[str]] = {}
    graph_rows = _bounded_items(services, label="service graph")
    service_type_rows = 0
    for row in graph_rows:
        try:
            service_name, service_types = row
        except (TypeError, ValueError) as error:
            raise ValueError(
                "service graph rows must contain a name and type sequence"
            ) from error
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
    return {
        service_type: tuple(sorted(set(service_names)))
        for service_type, service_names in by_type.items()
    }


def _select_simulation_service_names(
    by_type: dict[str, tuple[str, ...]],
) -> SimulationServiceNames:
    selected: dict[str, str | None] = {}
    for field_name, service_type in _SERVICE_TYPES.items():
        candidates = sorted(
            set(by_type.get(service_type, [])),
            key=lambda value: _service_sort_key(value, field_name),
        )
        selected[field_name] = candidates[0] if candidates else None
    return SimulationServiceNames(**selected)


def _validated_configured_service_names(
    configured: SimulationServiceNames,
    by_type: dict[str, tuple[str, ...]],
) -> SimulationServiceNames:
    return SimulationServiceNames(
        **{
            field_name: (
                service_name
                if service_name in by_type.get(service_type, ())
                else None
            )
            for field_name, service_type in _SERVICE_TYPES.items()
            if (service_name := getattr(configured, field_name)) is not None
        }
    )


def feature_enabled_or_unknown(feature_set: set[str], feature_name: str) -> bool:
    if len(feature_set) > _MAX_GRAPH_ROWS:
        raise ValueError("simulator feature rows exceed the adapter bound")
    normalized_features = {
        _bounded_text(feature, label="simulator feature") for feature in feature_set
    }
    normalized_name = _bounded_text(feature_name, label="simulator feature name")
    return not normalized_features or normalized_name in normalized_features


def pose_snapshot_from_state(state: Any) -> PoseSnapshot:
    stamp = state.header.stamp
    pose = state.pose
    return PoseSnapshot(
        frame_id=_bounded_text(state.header.frame_id, label="pose frame_id"),
        stamp_sec=_timestamp_seconds(stamp),
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
    normalized_value = _exact_int(value, label="message constant value")
    constant_rows = vars(constants)
    if len(constant_rows) > _MAX_GRAPH_ROWS:
        raise ValueError("message constant rows exceed the adapter bound")
    for name, candidate in constant_rows.items():
        if name.isupper() and type(candidate) is int and candidate == normalized_value:
            return _bounded_text(name, label="message constant name")
    return f"UNKNOWN_{normalized_value}"


def constant_names(constants: type, values: Sequence[int]) -> list[str]:
    bounded_values = _bounded_items(values, label="message constant values")
    return [constant_name(constants, value) for value in bounded_values]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filter", default="", help="POSIX regex entity filter")
    parser.add_argument("--entity", action="append", default=None)
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()
    rclpy = importlib.import_module("rclpy")
    initialized_here = not rclpy.ok()
    if initialized_here:
        rclpy.init(args=None)
    node = rclpy.create_node("manyforge_scene_mirror_probe")
    try:
        snapshot = SimulationSceneMirrorClient(
            node,
            service_timeout_s=args.timeout,
        ).capture(
            entity_filter=args.filter,
            entity_names=args.entity,
            entity_limit=args.limit,
        )
        print(
            json.dumps(
                snapshot.to_dict(),
                sort_keys=True,
                separators=(",", ":"),
            )
        )
        return 0 if not snapshot.errors else 2
    finally:
        node.destroy_node()
        if initialized_here and rclpy.ok():
            rclpy.shutdown()


def _service_sort_key(service_name: str, field_name: str) -> tuple[int, str]:
    normalized = service_name.lower().rstrip("/")
    preferred = _PREFERRED_SERVICE_LEAVES[field_name]
    return (
        0 if normalized == preferred or normalized.endswith("/" + preferred) else 1,
        service_name,
    )


def _bounded_items(value: Any, *, label: str) -> tuple[Any, ...]:
    if isinstance(value, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be a row sequence")
    try:
        items = tuple(islice(iter(value), _MAX_GRAPH_ROWS + 1))
    except TypeError as error:
        raise ValueError(f"{label} must be a row sequence") from error
    if len(items) > _MAX_GRAPH_ROWS:
        raise ValueError(f"{label} exceed the adapter bound")
    return items


def _bounded_unique_names(value: Any, *, label: str) -> tuple[str, ...]:
    names = tuple(
        _bounded_text(item, label=label)
        for item in _bounded_items(value, label=label)
    )
    if len(set(names)) != len(names):
        raise ValueError(f"{label} must be unique")
    return names


def _bounded_text(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    text = value.strip()
    if not text:
        raise ValueError(f"{label} must be non-empty")
    return text


def _bounded_optional_text(value: Any, *, label: str, maximum: int) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > maximum:
        raise ValueError(f"{label} exceeds the adapter bound")
    return value.strip()


def _bounded_timeout(value: float) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError("service timeout must be numeric")
    try:
        timeout = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise ValueError("service timeout must be numeric") from error
    if not math.isfinite(timeout) or not 0.05 <= timeout <= 120.0:
        raise ValueError("service timeout must be finite and in [0.05, 120]")
    return timeout


def _finite_float(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a numeric value, not text or boolean")
    try:
        number = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise ValueError(f"{label} must be numeric") from error
    if not math.isfinite(number):
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
    except OverflowError as error:
        raise ValueError("timestamp must be finite") from error
    if not math.isfinite(value):
        raise ValueError("timestamp must be finite")
    return value


def _normalized_quaternion(
    quaternion: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    norm = math.hypot(*quaternion)
    if not math.isfinite(norm) or norm <= 1e-12:
        raise ValueError("pose orientation quaternion must have finite nonzero norm")
    return tuple(value / norm for value in quaternion)  # type: ignore[return-value]


def _safe_error(value: object) -> str:
    return str(value or "")[:_MAX_TEXT_CHARS]


if __name__ == "__main__":
    raise SystemExit(main())
