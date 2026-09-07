"""Read-only typed fake-owner qualification for the Isaac scene observer."""

from __future__ import annotations

from types import SimpleNamespace

from geometry_msgs.msg import Vector3
import pytest
from simulation_interfaces.msg import Bounds, EntityCategory, SimulatorFeatures
from simulation_interfaces.srv import (
    GetEntities,
    GetEntityBounds,
    GetEntityInfo,
    GetEntityState,
    GetSimulatorFeatures,
)

from manyforge_ros import scene_mirror_probe


class _Future:
    def __init__(self, result: object) -> None:
        self._result = result

    def done(self) -> bool:
        return True

    def result(self) -> object:
        return self._result


class _Client:
    def __init__(self, handler) -> None:
        self._handler = handler

    def wait_for_service(self, *, timeout_sec: float) -> bool:
        return timeout_sec > 0.0

    def call_async(self, request: object) -> _Future:
        return _Future(self._handler(request))


class _Node:
    def __init__(self, handlers: dict[str, object]) -> None:
        self.handlers = handlers
        self.executor = SimpleNamespace(is_spinning=True)
        self.created: list[str] = []
        self.destroyed = 0

    def get_service_names_and_types(self):
        return [
            (name, [service_type])
            for name, service_type in (
                (
                    "/get_simulator_features",
                    "simulation_interfaces/srv/GetSimulatorFeatures",
                ),
                ("/get_entities", "simulation_interfaces/srv/GetEntities"),
                ("/get_entity_info", "simulation_interfaces/srv/GetEntityInfo"),
                ("/get_entity_state", "simulation_interfaces/srv/GetEntityState"),
                (
                    "/get_entity_bounds",
                    "simulation_interfaces/srv/GetEntityBounds",
                ),
            )
        ]

    def create_client(self, _service_type: type, service_name: str) -> _Client:
        self.created.append(service_name)
        return _Client(self.handlers[service_name])

    def destroy_client(self, _client: _Client) -> None:
        self.destroyed += 1


def _ok_response(service_type: type) -> object:
    response = service_type.Response()
    if hasattr(response, "result"):
        response.result.result = response.result.RESULT_OK
    return response


def test_scene_mirror_captures_info_pose_bounds_and_per_entity_failure() -> None:
    observed_filters: list[str] = []

    def features(_request):
        response = GetSimulatorFeatures.Response()
        response.features.features = [
            int(SimulatorFeatures.ENTITY_INFO_GETTING),
            int(SimulatorFeatures.ENTITY_STATE_GETTING),
            int(SimulatorFeatures.ENTITY_BOUNDS),
        ]
        response.features.spawn_formats = ["usd"]
        response.features.custom_info = "fake selected owner"
        return response

    def entities(request):
        observed_filters.append(request.filters.filter)
        response = _ok_response(GetEntities)
        response.entities = ["/World/a", "/World/b"]
        return response

    def info(request):
        response = _ok_response(GetEntityInfo)
        response.info.category.category = int(EntityCategory.CATEGORY_OBJECT)
        response.info.description = "entity " + request.entity
        response.info.tags = ["pickable"]
        return response

    def state(request):
        response = _ok_response(GetEntityState)
        response.state.header.frame_id = "world"
        response.state.header.stamp.sec = 12
        response.state.header.stamp.nanosec = 500_000_000
        response.state.pose.position.x = 1.0 if request.entity.endswith("a") else 2.0
        response.state.pose.orientation.w = 1.0
        return response

    def bounds(request):
        response = GetEntityBounds.Response()
        if request.entity.endswith("b"):
            response.result.result = response.result.RESULT_OPERATION_FAILED
            response.result.error_message = "bounds unavailable"
            return response
        response.result.result = response.result.RESULT_OK
        response.bounds.type = int(Bounds.TYPE_BOX)
        response.bounds.points = [
            Vector3(x=-0.5, y=-0.25, z=-0.1),
            Vector3(x=0.5, y=0.25, z=0.1),
        ]
        return response

    node = _Node(
        {
            "/get_simulator_features": features,
            "/get_entities": entities,
            "/get_entity_info": info,
            "/get_entity_state": state,
            "/get_entity_bounds": bounds,
        }
    )
    snapshot = scene_mirror_probe.SimulationSceneMirrorClient(
        node,
        service_timeout_s=0.5,
    ).capture(entity_filter="/World/.*")

    assert observed_filters == ["/World/.*"]
    assert snapshot.errors == ()
    assert snapshot.features == (
        "ENTITY_INFO_GETTING",
        "ENTITY_STATE_GETTING",
        "ENTITY_BOUNDS",
    )
    assert snapshot.spawn_formats == ("usd",)
    assert [entity.name for entity in snapshot.entities] == [
        "/World/a",
        "/World/b",
    ]
    first, second = snapshot.entities
    assert first.category_name == "CATEGORY_OBJECT"
    assert first.pose == scene_mirror_probe.PoseSnapshot(
        frame_id="world",
        stamp_ns=12500000000,
        position_xyz=(1.0, 0.0, 0.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    assert first.bounds is not None and first.bounds.type_name == "TYPE_BOX"
    assert second.pose is not None and second.pose.position_xyz[0] == 2.0
    assert second.bounds is None
    assert second.errors == ("GetEntityBounds failed: 4: bounds unavailable",)
    assert node.destroyed == len(node.created) == 8
    assert not hasattr(node, "create_publisher")


def test_explicit_entities_do_not_require_get_entities_service() -> None:
    configured = scene_mirror_probe.SimulationServiceNames(
        get_simulator_features="/get_simulator_features",
    )
    node = _Node(
        {
            "/get_simulator_features": lambda _r: GetSimulatorFeatures.Response(),
            "/get_entities": lambda _r: _ok_response(GetEntities),
            "/get_entity_info": lambda _r: _ok_response(GetEntityInfo),
            "/get_entity_state": lambda _r: _ok_response(GetEntityState),
            "/get_entity_bounds": lambda _r: _ok_response(GetEntityBounds),
        }
    )
    snapshot = scene_mirror_probe.SimulationSceneMirrorClient(
        node,
        service_names=configured,
    ).capture(
        entity_names=["/World/a"],
        entity_limit=0,
        include_info=False,
        include_state=False,
        include_bounds=False,
    )

    assert snapshot.entities == ()
    assert "/get_entities" not in node.created


def test_configured_service_name_requires_the_exact_graph_type() -> None:
    configured = scene_mirror_probe.SimulationServiceNames(
        get_simulator_features="/wrong_type",
    )
    node = _Node(
        {
            "/get_simulator_features": lambda _r: GetSimulatorFeatures.Response(),
        }
    )
    original_graph = node.get_service_names_and_types
    node.get_service_names_and_types = lambda: original_graph() + [
        ("/wrong_type", ["simulation_interfaces/srv/GetEntities"]),
    ]

    snapshot = scene_mirror_probe.SimulationSceneMirrorClient(
        node,
        service_names=configured,
        service_timeout_s=0.05,
    ).capture(
        entity_names=[],
        include_info=False,
        include_state=False,
        include_bounds=False,
    )

    assert snapshot.service_names["get_simulator_features"] == (
        "/get_simulator_features"
    )
    assert "/wrong_type" not in node.created


def test_scene_snapshot_records_and_service_merge_remain_owner_neutral() -> None:
    pose = scene_mirror_probe.PoseSnapshot(
        frame_id="world",
        stamp_ns=12500000000,
        position_xyz=(1.0, 2.0, 3.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    snapshot = scene_mirror_probe.SceneMirrorSnapshot(
        service_names={"get_entities": "/get_entities"},
        features=("ENTITY_STATE_GETTING",),
        spawn_formats=("usd",),
        custom_info="fixture",
        entities=(scene_mirror_probe.EntitySnapshot(name="/World/a", pose=pose),),
    )
    assert snapshot.to_dict()["entities"][0]["pose"]["frame_id"] == "world"

    configured = scene_mirror_probe.SimulationServiceNames(
        get_entities="/configured/get_entities"
    )
    discovered = scene_mirror_probe.SimulationServiceNames(
        get_entities="/get_entities",
        get_entity_state="/get_entity_state",
    )
    assert configured.with_discovered(discovered).get_entities == (
        "/configured/get_entities"
    )
    assert configured.with_discovered(discovered).get_entity_state == (
        "/get_entity_state"
    )


def test_service_selection_is_pure_deterministic_and_bounded() -> None:
    selected = scene_mirror_probe.select_simulation_service_names(
        [
            ("/vendor/entities", ["simulation_interfaces/srv/GetEntities"]),
            ("/get_entities", ("simulation_interfaces/srv/GetEntities",)),
            (
                "/isaacsim/get_entity_state",
                ["simulation_interfaces/srv/GetEntityState"],
            ),
        ]
    )
    assert selected.get_entities == "/get_entities"
    assert selected.get_entity_state == "/isaacsim/get_entity_state"
    assert selected.get_entity_info is None
    service_row = ("/get_entities", ["simulation_interfaces/srv/GetEntities"])
    with pytest.raises(ValueError, match="service graph exceed"):
        scene_mirror_probe.select_simulation_service_names([service_row] * 4097)


def test_pose_and_bounds_translation_is_finite_and_normalized() -> None:
    state = SimpleNamespace(
        header=SimpleNamespace(
            frame_id="world",
            stamp=SimpleNamespace(sec=12, nanosec=500_000_000),
        ),
        pose=SimpleNamespace(
            position=SimpleNamespace(x=1, y=2, z=3),
            orientation=SimpleNamespace(x=0, y=0, z=0, w=2),
        ),
    )
    bounds = SimpleNamespace(
        type=4,
        points=[SimpleNamespace(x=-0.5, y=-0.25, z=-0.1)],
    )

    assert scene_mirror_probe.pose_snapshot_from_state(state) == (
        scene_mirror_probe.PoseSnapshot(
            frame_id="world",
            stamp_ns=12500000000,
            position_xyz=(1.0, 2.0, 3.0),
            orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
    )

    class BoundsConstants:
        TYPE_BOX = 4

    assert scene_mirror_probe.bounds_snapshot_from_value(
        bounds,
        bounds_type=BoundsConstants,
    ).type_name == "TYPE_BOX"
    state.header.stamp.nanosec = 1_000_000_000
    with pytest.raises(ValueError, match="nanosec must be"):
        scene_mirror_probe.pose_snapshot_from_state(state)
    state.header.stamp.nanosec = 0
    state.pose.position.x = float("inf")
    with pytest.raises(ValueError, match="position.x must be finite"):
        scene_mirror_probe.pose_snapshot_from_state(state)


def test_features_constants_and_bounds_reject_unbounded_or_invalid_rows() -> None:
    assert scene_mirror_probe.feature_enabled_or_unknown(set(), "ENTITY_BOUNDS")
    assert not scene_mirror_probe.feature_enabled_or_unknown(
        {"ENTITY_STATE_GETTING"},
        "ENTITY_BOUNDS",
    )
    with pytest.raises(ValueError, match="feature rows exceed"):
        scene_mirror_probe.feature_enabled_or_unknown(
            {f"feature-{index}" for index in range(4097)},
            "feature-0",
        )

    class BoundsConstants:
        TYPE_BOX = 4

    assert scene_mirror_probe.constant_names(BoundsConstants, [4, 99]) == [
        "TYPE_BOX",
        "UNKNOWN_99",
    ]
    with pytest.raises(ValueError, match="bounds type must be an integer"):
        scene_mirror_probe.bounds_snapshot_from_value(
            SimpleNamespace(type=True, points=[]),
            bounds_type=BoundsConstants,
        )


def test_historical_live_case_is_replaced_by_active_i9_coverage() -> None:
    assert scene_mirror_probe.HISTORICAL_SOURCE_BLOB == (
        "07de241694d2e7738a55d8c6c74bab1ef5d11b20"
    )
    assert scene_mirror_probe.HISTORICAL_TEST_BLOB == (
        "a911cb32c772fd1e672676eb8d319213eebab8cb"
    )
