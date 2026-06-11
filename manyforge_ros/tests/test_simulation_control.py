from __future__ import annotations

import threading
import time

import pytest


def test_simulation_control_resets_state_and_publishes_command_gate():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from simulation_interfaces.srv import ResetSimulation
    from std_msgs.msg import Bool

    from manyforge_ros.simulation_control import SimulationControlClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_sim_control_fake_sim")
    client_node = rclpy.create_node("manyforge_sim_control_client")
    scopes: list[int] = []
    gate_messages: list[bool] = []

    def reset_service(request, response):
        scopes.append(int(request.scope))
        response.result.result = response.result.RESULT_OK
        return response

    service = server_node.create_service(
        ResetSimulation,
        "/reset_simulation",
        reset_service,
    )
    gate_qos = QoSProfile(
        depth=1,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        reliability=ReliabilityPolicy.RELIABLE,
    )
    subscription = server_node.create_subscription(
        Bool,
        "/manyforge/test_command_enable",
        lambda msg: gate_messages.append(bool(msg.data)),
        gate_qos,
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationControlClient(client_node, service_timeout_s=2.0)
        client.publish_command_gate(
            "/manyforge/test_command_enable",
            enabled=False,
            settle_s=0.1,
        )
        result = client.reset(scope="state")
        client.publish_command_gate(
            "/manyforge/test_command_enable",
            enabled=True,
            settle_s=0.1,
        )

        deadline = time.monotonic() + 1.0
        while len(gate_messages) < 2 and time.monotonic() < deadline:
            time.sleep(0.01)

        assert result.success is True
        assert result.service_name == "/reset_simulation"
        assert scopes == [ResetSimulation.Request.SCOPE_STATE]
        assert gate_messages[:2] == [False, True]
    finally:
        server_node.destroy_service(service)
        server_node.destroy_subscription(subscription)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_simulation_control_surfaces_reset_service_failure():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.srv import ResetSimulation

    from manyforge_ros.simulation_control import SimulationControlClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_sim_control_failure_fake_sim")
    client_node = rclpy.create_node("manyforge_sim_control_failure_client")

    def reset_service(_request, response):
        response.result.result = response.result.RESULT_OPERATION_FAILED
        response.result.error_message = "reset rejected"
        return response

    service = server_node.create_service(
        ResetSimulation,
        "/isaacsim/reset_simulation",
        reset_service,
    )
    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationControlClient(client_node, service_timeout_s=2.0)
        result = client.reset(scope="state")

        assert result.success is False
        assert result.service_name == "/isaacsim/reset_simulation"
        assert result.result_code == ResetSimulation.Response().result.RESULT_OPERATION_FAILED
        assert "reset rejected" in result.error
    finally:
        server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_simulation_control_state_step_and_entity_services():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulationState
    from simulation_interfaces.srv import (
        SetEntityState,
        SetSimulationState,
        StepSimulation,
    )

    from manyforge_ros.simulation_control import SimulationControlClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_sim_control_ops_fake_sim")
    client_node = rclpy.create_node("manyforge_sim_control_ops_client")
    requested_states: list[int] = []
    requested_steps: list[int] = []
    requested_entities: list[tuple[str, str, list[float], list[float]]] = []

    def set_state_service(request, response):
        requested_states.append(int(request.state.state))
        response.result.result = response.result.RESULT_OK
        return response

    def step_service(request, response):
        requested_steps.append(int(request.steps))
        response.result.result = response.result.RESULT_OK
        return response

    def set_entity_state_service(request, response):
        pose = request.state.pose
        requested_entities.append(
            (
                request.entity,
                request.state.header.frame_id,
                [pose.position.x, pose.position.y, pose.position.z],
                [
                    pose.orientation.x,
                    pose.orientation.y,
                    pose.orientation.z,
                    pose.orientation.w,
                ],
            )
        )
        response.result.result = response.result.RESULT_OK
        return response

    services = [
        server_node.create_service(
            SetSimulationState,
            "/set_simulation_state",
            set_state_service,
        ),
        server_node.create_service(
            StepSimulation,
            "/step_simulation",
            step_service,
        ),
        server_node.create_service(
            SetEntityState,
            "/set_entity_state",
            set_entity_state_service,
        ),
    ]
    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationControlClient(client_node, service_timeout_s=2.0)
        state_result = client.set_simulation_state(state="paused")
        step_result = client.step(steps=3)
        entity_result = client.set_entity_state(
            entity="/World/box",
            pose={
                "frame_id": "world",
                "position_m": [1.0, 2.0, 3.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        )

        assert state_result.success is True
        assert step_result.success is True
        assert entity_result.success is True
        assert requested_states == [SimulationState.STATE_PAUSED]
        assert requested_steps == [3]
        assert requested_entities == [
            (
                "/World/box",
                "world",
                [1.0, 2.0, 3.0],
                [0.0, 0.0, 0.0, 1.0],
            )
        ]
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()
