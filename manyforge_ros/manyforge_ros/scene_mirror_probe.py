"""Read-only ROS simulation scene mirror probe.

The probe intentionally keeps semantics narrow: it discovers the standard
``simulation_interfaces`` services, captures entity metadata and live state, and
returns a structured snapshot.  It does not mutate the simulator and it does
not guess which entities are task-relevant.
"""

from __future__ import annotations

import argparse
import json
import time
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from typing import Any


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

    def with_discovered(self, discovered: "SimulationServiceNames") -> "SimulationServiceNames":
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
    """Client for a read-only snapshot of a simulator scene."""

    _SERVICE_TYPES = {
        "get_simulator_features": "simulation_interfaces/srv/GetSimulatorFeatures",
        "get_entities": "simulation_interfaces/srv/GetEntities",
        "get_entity_info": "simulation_interfaces/srv/GetEntityInfo",
        "get_entity_state": "simulation_interfaces/srv/GetEntityState",
        "get_entity_bounds": "simulation_interfaces/srv/GetEntityBounds",
    }

    def __init__(
        self,
        node: object,
        *,
        service_names: SimulationServiceNames | None = None,
        service_timeout_s: float = 2.0,
    ) -> None:
        self.node = node
        self.service_timeout_s = service_timeout_s
        self.service_names = service_names or SimulationServiceNames()

        self._rclpy = _import_module("rclpy")
        self._srv = _import_module("simulation_interfaces.srv")
        self._msg = _import_module("simulation_interfaces.msg")

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
        service_names = self._resolve_service_names()
        features, spawn_formats, custom_info, feature_errors = self._capture_features(
            service_names
        )

        if entity_names is not None:
            entities = tuple(entity_names)
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
                entity_filter=entity_filter,
            )
        if entity_limit is not None:
            entities = entities[:entity_limit]

        feature_set = set(features)
        can_query_info = (
            include_info
            and service_names.get_entity_info is not None
            and self._feature_enabled_or_unknown(feature_set, "ENTITY_INFO_GETTING")
        )
        can_query_state = (
            include_state
            and service_names.get_entity_state is not None
            and self._feature_enabled_or_unknown(feature_set, "ENTITY_STATE_GETTING")
        )
        can_query_bounds = (
            include_bounds
            and service_names.get_entity_bounds is not None
            and self._feature_enabled_or_unknown(feature_set, "ENTITY_BOUNDS")
        )

        snapshots = []
        for entity in entities:
            snapshots.append(
                self._capture_entity(
                    entity,
                    service_names=service_names,
                    query_info=can_query_info,
                    query_state=can_query_state,
                    query_bounds=can_query_bounds,
                )
            )

        return SceneMirrorSnapshot(
            service_names=service_names.to_dict(),
            features=features,
            spawn_formats=spawn_formats,
            custom_info=custom_info,
            entities=tuple(snapshots),
            errors=feature_errors + entity_errors,
        )

    def _resolve_service_names(self) -> SimulationServiceNames:
        discovered = self._discover_service_names()
        service_names = self.service_names.with_discovered(discovered)

        deadline = time.monotonic() + self.service_timeout_s
        while service_names.get_entities is None and time.monotonic() < deadline:
            time.sleep(0.05)
            discovered = self._discover_service_names()
            service_names = self.service_names.with_discovered(discovered)

        return service_names

    def _discover_service_names(self) -> SimulationServiceNames:
        services = self.node.get_service_names_and_types()
        by_type: dict[str, list[str]] = {}
        for service_name, service_types in services:
            for service_type in service_types:
                by_type.setdefault(service_type, []).append(service_name)

        selected: dict[str, str | None] = {}
        for field_name, service_type in self._SERVICE_TYPES.items():
            candidates = sorted(by_type.get(service_type, []), key=_service_sort_key)
            selected[field_name] = candidates[0] if candidates else None

        return SimulationServiceNames(**selected)

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
        except Exception as exc:
            return (), (), "", (f"GetSimulatorFeatures failed: {exc}",)
        features_msg = response.features
        return (
            tuple(self._constant_names(self._msg.SimulatorFeatures, features_msg.features)),
            tuple(features_msg.spawn_formats),
            features_msg.custom_info,
            (),
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
            response = self._call_service(service_name, self._srv.GetEntities, request)
        except Exception as exc:
            return (), (f"GetEntities failed: {exc}",)
        if not self._result_ok(response.result):
            return (), (f"GetEntities failed: {self._result_message(response.result)}",)
        return tuple(response.entities), ()

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
                info_response = self._call_entity_service(
                    service_names.get_entity_info,
                    self._srv.GetEntityInfo,
                    entity,
                )
                if self._result_ok(info_response.result):
                    category_code = int(info_response.info.category.category)
                    category_name = self._constant_name(
                        self._msg.EntityCategory,
                        category_code,
                    )
                    description = info_response.info.description
                    tags = tuple(info_response.info.tags)
                else:
                    errors.append(
                        f"GetEntityInfo failed: {self._result_message(info_response.result)}"
                    )
            except Exception as exc:
                errors.append(f"GetEntityInfo failed: {exc}")

        if query_state:
            try:
                state_response = self._call_entity_service(
                    service_names.get_entity_state,
                    self._srv.GetEntityState,
                    entity,
                )
                if self._result_ok(state_response.result):
                    pose = self._pose_snapshot(state_response.state)
                else:
                    errors.append(
                        f"GetEntityState failed: {self._result_message(state_response.result)}"
                    )
            except Exception as exc:
                errors.append(f"GetEntityState failed: {exc}")

        if query_bounds:
            try:
                bounds_response = self._call_entity_service(
                    service_names.get_entity_bounds,
                    self._srv.GetEntityBounds,
                    entity,
                )
                if self._result_ok(bounds_response.result):
                    bounds = self._bounds_snapshot(bounds_response.bounds)
                else:
                    errors.append(
                        f"GetEntityBounds failed: {self._result_message(bounds_response.result)}"
                    )
            except Exception as exc:
                errors.append(f"GetEntityBounds failed: {exc}")

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
            raise RuntimeError(f"{service_type.__name__} service is not available")
        request = service_type.Request()
        request.entity = entity
        return self._call_service(service_name, service_type, request)

    def _call_service(self, service_name: str, service_type: type, request: object) -> object:
        client = self.node.create_client(service_type, service_name)
        try:
            if not client.wait_for_service(timeout_sec=self.service_timeout_s):
                raise TimeoutError(f"Timed out waiting for service {service_name}")
            future = client.call_async(request)
            return self._wait_for_future(future)
        finally:
            self.node.destroy_client(client)

    def _wait_for_future(self, future: object) -> object:
        deadline = time.monotonic() + self.service_timeout_s
        executor = getattr(self.node, "executor", None)
        if executor is None or not getattr(executor, "is_spinning", False):
            remaining = max(0.0, deadline - time.monotonic())
            self._rclpy.spin_until_future_complete(
                self.node,
                future,
                timeout_sec=remaining,
            )
            if future.done():
                return future.result()
        else:
            while time.monotonic() < deadline:
                if future.done():
                    return future.result()
                time.sleep(0.01)
        raise TimeoutError("Timed out waiting for simulation_interfaces response")

    def _feature_enabled_or_unknown(self, feature_set: set[str], feature_name: str) -> bool:
        return not feature_set or feature_name in feature_set

    def _pose_snapshot(self, state: object) -> PoseSnapshot:
        stamp = state.header.stamp
        pose = state.pose
        stamp_sec = float(stamp.sec) + float(stamp.nanosec) / 1_000_000_000.0
        return PoseSnapshot(
            frame_id=state.header.frame_id,
            stamp_sec=stamp_sec,
            position_xyz=(
                float(pose.position.x),
                float(pose.position.y),
                float(pose.position.z),
            ),
            orientation_xyzw=(
                float(pose.orientation.x),
                float(pose.orientation.y),
                float(pose.orientation.z),
                float(pose.orientation.w),
            ),
        )

    def _bounds_snapshot(self, bounds: object) -> BoundsSnapshot:
        return BoundsSnapshot(
            type_code=int(bounds.type),
            type_name=self._constant_name(self._msg.Bounds, int(bounds.type)),
            points_xyz=tuple(
                (float(point.x), float(point.y), float(point.z))
                for point in bounds.points
            ),
        )

    def _constant_names(self, cls: type, values: list[int]) -> list[str]:
        return [self._constant_name(cls, int(value)) for value in values]

    @staticmethod
    def _constant_name(cls: type, value: int) -> str:
        for name, candidate in vars(cls).items():
            if name.isupper() and isinstance(candidate, int) and candidate == value:
                return name
        return f"UNKNOWN_{value}"

    @staticmethod
    def _result_ok(result: object) -> bool:
        return int(result.result) == int(result.RESULT_OK)

    @staticmethod
    def _result_message(result: object) -> str:
        return f"{result.result}: {result.error_message}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filter", default="", help="POSIX regex entity filter")
    parser.add_argument(
        "--entity",
        action="append",
        default=None,
        help="Explicit entity path to query; may be repeated.",
    )
    parser.add_argument("--limit", type=int, default=None, help="Maximum entity count")
    parser.add_argument("--timeout", type=float, default=2.0, help="Service timeout")
    args = parser.parse_args()

    rclpy = _import_module("rclpy")
    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_scene_mirror_probe")
    try:
        client = SimulationSceneMirrorClient(node, service_timeout_s=args.timeout)
        snapshot = client.capture(
            entity_filter=args.filter,
            entity_names=args.entity,
            entity_limit=args.limit,
        )
        print(json.dumps(snapshot.to_dict(), indent=2, sort_keys=True))
        return 0 if not snapshot.errors else 2
    finally:
        node.destroy_node()
        rclpy.shutdown()


def _service_sort_key(service_name: str) -> tuple[int, str]:
    preferred = {
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
    return (0 if service_name in preferred else 1, service_name)


def _import_module(module_name: str) -> Any:
    import importlib

    return importlib.import_module(module_name)


if __name__ == "__main__":
    raise SystemExit(main())
