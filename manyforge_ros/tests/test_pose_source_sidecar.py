from __future__ import annotations

import threading

import pytest


def test_simulation_interfaces_pose_source_polls_latest_wins_state():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulatorFeatures
    from simulation_interfaces.srv import GetEntityState, GetSimulatorFeatures

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        POSE_SOURCE_STATUS_STALE,
        PoseSourceBinding,
        PoseSourceMeasuredTcpBinding,
        SimulationInterfacesPoseSource,
    )
    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_pose_source_fake_sim")
    client_node = rclpy.create_node("manyforge_pose_source_client")
    services = []
    x_position = {"value": 1.0}
    now = {"value": 100.0}

    def get_features(_request, response):
        response.features.features = [SimulatorFeatures.ENTITY_STATE_GETTING]
        return response

    def get_entity_state(request, response):
        response.result.result = response.result.RESULT_OK
        response.state.header.frame_id = "world"
        response.state.header.stamp.sec = int(now["value"])
        if request.entity == "/World/tool0":
            response.state.pose.position.x = 0.5
            response.state.pose.position.y = 0.0
            response.state.pose.position.z = 0.25
        else:
            response.state.pose.position.x = x_position["value"]
            response.state.pose.position.y = 2.0
            response.state.pose.position.z = 3.0
        response.state.pose.orientation.w = 1.0
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        source = SimulationInterfacesPoseSource(
            client=client,
            pose_source_id="isaac_scene_objects",
            bindings=[
                PoseSourceBinding(
                    object_id="isaac_cheese_box",
                    entity="/mac_n_cheese_centered",
                    apply_policy="observe_only",
                )
            ],
            measured_tcp=PoseSourceMeasuredTcpBinding(
                entity="/World/tool0",
                tcp_frame_id="tool0",
                frame_id="world",
            ),
            max_update_hz=4.0,
            stale_after_s=1.0,
            now_fn=lambda: now["value"],
        )

        first = source.poll_once()
        assert first.status == POSE_SOURCE_STATUS_FRESH
        assert len(first.updates) == 1
        assert first.updates[0].pose.position_xyz == (1.0, 2.0, 3.0)

        x_position["value"] = 4.0
        now["value"] = 100.25
        second = source.poll_once()
        assert second.status == POSE_SOURCE_STATUS_FRESH
        assert second.updates[0].pose.position_xyz == (4.0, 2.0, 3.0)

        live = source.state.snapshot(monotonic_s=100.25)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        assert live.objects[0].status == POSE_SOURCE_STATUS_FRESH
        assert live.objects[0].update is not None
        assert live.objects[0].update.pose.position_xyz == (4.0, 2.0, 3.0)
        assert live.measured_tcp is not None
        assert live.measured_tcp.tcp_frame_id == "tool0"
        assert live.measured_tcp.pose.position_xyz == (0.5, 0.0, 0.25)
        payload = live.to_dict()
        assert payload["measuredTcp"]["tcpFrameId"] == "tool0"
        assert payload["measuredTcp"]["pose"]["positionXYZ"] == [0.5, 0.0, 0.25]

        stale = source.state.snapshot(monotonic_s=102.0)
        assert stale.status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].age_s == pytest.approx(1.75)
        assert stale.measured_tcp_age_s == pytest.approx(1.75)
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_simulation_interfaces_pose_source_rejects_missing_entity_pose():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulatorFeatures
    from simulation_interfaces.srv import GetEntityState, GetSimulatorFeatures

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_REJECTED,
        PoseSourceBinding,
        SimulationInterfacesPoseSource,
    )
    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_pose_source_fake_reject_sim")
    client_node = rclpy.create_node("manyforge_pose_source_reject_client")
    services = []

    def get_features(_request, response):
        response.features.features = [SimulatorFeatures.ENTITY_STATE_GETTING]
        return response

    def get_entity_state(_request, response):
        response.result.result = 999
        response.result.error_message = "entity not found"
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        source = SimulationInterfacesPoseSource(
            client=client,
            pose_source_id="isaac_scene_objects",
            bindings=[
                PoseSourceBinding(
                    object_id="isaac_cheese_box",
                    entity="/missing",
                    apply_policy="observe_only",
                )
            ],
            now_fn=lambda: 10.0,
        )

        result = source.poll_once()
        assert result.status == POSE_SOURCE_STATUS_REJECTED
        assert result.updates == ()
        assert len(result.rejections) == 1
        assert result.rejections[0].reason == "source_error"
        assert "entity not found" in result.rejections[0].detail

        state = source.state.snapshot(monotonic_s=10.0)
        assert state.status == POSE_SOURCE_STATUS_REJECTED
        assert state.objects[0].rejection == result.rejections[0]
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()
