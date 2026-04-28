from __future__ import annotations

import threading

import pytest


def test_scene_mirror_probe_captures_entities_info_state_and_bounds():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from geometry_msgs.msg import Vector3
    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import EntityCategory, SimulatorFeatures
    from simulation_interfaces.srv import (
        GetEntities,
        GetEntityBounds,
        GetEntityInfo,
        GetEntityState,
        GetSimulatorFeatures,
    )

    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_scene_mirror_fake_sim")
    client_node = rclpy.create_node("manyforge_scene_mirror_client")
    services = []

    def get_features(_request, response):
        response.features.features = [
            SimulatorFeatures.ENTITY_INFO_GETTING,
            SimulatorFeatures.ENTITY_STATE_GETTING,
            SimulatorFeatures.ENTITY_BOUNDS,
            SimulatorFeatures.ENTITY_BOUNDS_BOX,
        ]
        response.features.spawn_formats = ["usd"]
        response.features.custom_info = "fake simulator"
        return response

    def get_entities(request, response):
        response.result.result = response.result.RESULT_OK
        names = ["/World/robot", "/World/table", "/World/block"]
        response.entities = [
            name for name in names if not request.filters.filter or "World" in name
        ]
        return response

    def get_entity_info(request, response):
        response.result.result = response.result.RESULT_OK
        response.info.category.category = (
            EntityCategory.CATEGORY_ROBOT
            if request.entity.endswith("robot")
            else EntityCategory.CATEGORY_OBJECT
        )
        response.info.description = f"description for {request.entity}"
        response.info.tags = ["test", "mirror"]
        return response

    def get_entity_state(request, response):
        response.result.result = response.result.RESULT_OK
        response.state.header.frame_id = "world"
        response.state.header.stamp.sec = 12
        response.state.header.stamp.nanosec = 500_000_000
        response.state.pose.position.x = 1.0 if request.entity.endswith("block") else 0.0
        response.state.pose.position.y = 2.0 if request.entity.endswith("block") else 0.0
        response.state.pose.position.z = 3.0 if request.entity.endswith("block") else 0.0
        response.state.pose.orientation.w = 1.0
        return response

    def get_entity_bounds(_request, response):
        response.result.result = response.result.RESULT_OK
        response.bounds.type = response.bounds.TYPE_BOX
        lower = Vector3(x=-0.05, y=-0.10, z=-0.15)
        upper = Vector3(x=0.05, y=0.10, z=0.15)
        response.bounds.points = [lower, upper]
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(server_node.create_service(GetEntities, "/get_entities", get_entities))
    services.append(
        server_node.create_service(GetEntityInfo, "/get_entity_info", get_entity_info)
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )
    services.append(
        server_node.create_service(GetEntityBounds, "/get_entity_bounds", get_entity_bounds)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        snapshot = client.capture(entity_filter="World", entity_limit=3)

        assert snapshot.errors == ()
        assert snapshot.service_names["get_entities"] == "/get_entities"
        assert "ENTITY_STATE_GETTING" in snapshot.features
        assert "ENTITY_BOUNDS" in snapshot.features
        assert snapshot.spawn_formats == ("usd",)
        assert len(snapshot.entities) == 3

        robot = snapshot.entities[0]
        assert robot.name == "/World/robot"
        assert robot.category_name == "CATEGORY_ROBOT"

        block = snapshot.entities[2]
        assert block.name == "/World/block"
        assert block.category_name == "CATEGORY_OBJECT"
        assert block.pose is not None
        assert block.pose.frame_id == "world"
        assert block.pose.stamp_sec == 12.5
        assert block.pose.position_xyz == (1.0, 2.0, 3.0)
        assert block.bounds is not None
        assert block.bounds.type_name == "TYPE_BOX"
        assert block.bounds.points_xyz == (
            (-0.05, -0.10, -0.15),
            (0.05, 0.10, 0.15),
        )

        configured_snapshot = client.capture(
            entity_names=["/World/block"],
            include_bounds=False,
        )
        assert configured_snapshot.errors == ()
        assert len(configured_snapshot.entities) == 1
        assert configured_snapshot.entities[0].name == "/World/block"
        assert configured_snapshot.entities[0].pose is not None
        assert configured_snapshot.entities[0].bounds is None
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()
