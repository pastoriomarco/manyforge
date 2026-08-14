"""Ownership and fail-closed checks for the restored Isaac wire adapters."""

from __future__ import annotations

import inspect
import time
from types import SimpleNamespace

import pytest
import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import Bool

from manyforge_ros_bringup import isaac_gripper_action_bridge
from manyforge_ros_bringup import isaac_topic_command_gate


def test_isaac_stop_topic_translation_uses_one_owner_fact() -> None:
    assert (
        isaac_topic_command_gate.stop_topic_value(owner_command_enabled=True)
        is False
    )
    assert (
        isaac_topic_command_gate.stop_topic_value(owner_command_enabled=False)
        is True
    )


@pytest.mark.parametrize("value", ["true", "false", 1, 0, None])
def test_isaac_stop_topic_translation_rejects_non_boolean_owner_facts(
    value: object,
) -> None:
    with pytest.raises(TypeError, match="must be an exact bool"):
        isaac_topic_command_gate.stop_topic_value(
            owner_command_enabled=value  # type: ignore[arg-type]
        )


def test_owner_topology_requires_one_exact_backend_publisher() -> None:
    exact = SimpleNamespace(
        node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE,
        node_namespace="/",
    )
    foreign = SimpleNamespace(node_name="foreign_gate_owner", node_namespace="/")
    namespaced = SimpleNamespace(
        node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE,
        node_namespace="/foreign",
    )

    assert isaac_topic_command_gate.owner_topology_matches(
        [exact], expected_node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE
    )
    for publishers in (
        [],
        [foreign],
        [namespaced],
        [exact, exact],
        None,
        {"publisher": exact},
    ):
        assert not isaac_topic_command_gate.owner_topology_matches(
            publishers,
            expected_node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE,
        )

    endpoint = SimpleNamespace(
        node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE,
        node_namespace="/",
        endpoint_gid=range(16),
    )
    assert isaac_topic_command_gate.owner_endpoint_identity(
        [endpoint],
        expected_node_name=isaac_topic_command_gate.DEFAULT_OWNER_NODE,
    ) == tuple(range(16))


def test_both_wire_adapters_consume_the_same_retained_owner_fact() -> None:
    gate_source = inspect.getsource(isaac_topic_command_gate)
    gripper_source = inspect.getsource(isaac_gripper_action_bridge)

    assert isaac_topic_command_gate.DEFAULT_ENABLE_TOPIC == (
        "/manyforge/isaac_arm_command_enable"
    )
    assert isaac_topic_command_gate.DEFAULT_OWNER_NODE == (
        "manyforge_ros2_control_execution_backend"
    )
    assert "owner_endpoint_identity" in gate_source
    assert "owner_endpoint_identity" in gripper_source
    for source in (gate_source, gripper_source):
        assert "isaac_demo_supervisor" not in source
        assert "supervisor_enable_topic" not in source
        assert "composer_url" not in source


def test_command_gate_starts_closed_before_any_owner_sample() -> None:
    source = inspect.getsource(isaac_topic_command_gate.IsaacTopicCommandGate)

    assert "self._owner_command_enabled = False" in source
    assert "self._owner_topology_valid = False" in source
    assert "self._reconcile_and_publish()" in source


def test_command_gate_closes_again_when_exact_owner_disappears() -> None:
    rclpy.init()
    owner: Node | None = None
    gate: isaac_topic_command_gate.IsaacTopicCommandGate | None = None
    probe: Node | None = None
    executor = SingleThreadedExecutor()
    observed: list[bool] = []
    qos = QoSProfile(
        depth=1,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        reliability=ReliabilityPolicy.RELIABLE,
    )
    try:
        owner = Node(isaac_topic_command_gate.DEFAULT_OWNER_NODE)
        enable = owner.create_publisher(
            Bool,
            isaac_topic_command_gate.DEFAULT_ENABLE_TOPIC,
            qos,
        )
        gate = isaac_topic_command_gate.IsaacTopicCommandGate()
        probe = Node("manyforge_isaac_gate_test_probe")
        probe.create_subscription(
            Bool,
            isaac_topic_command_gate.DEFAULT_STOP_TOPIC,
            lambda message: observed.append(bool(message.data)),
            qos,
        )
        for node in (owner, gate, probe):
            executor.add_node(node)

        assert _spin_until(executor, lambda: observed and observed[-1] is True)
        enabled = Bool()
        enabled.data = True
        enable.publish(enabled)
        assert _spin_until(executor, lambda: observed and observed[-1] is False)

        executor.remove_node(owner)
        owner.destroy_node()
        owner = None
        assert _spin_until(executor, lambda: observed and observed[-1] is True)
    finally:
        for node in (probe, gate, owner):
            if node is not None:
                executor.remove_node(node)
                node.destroy_node()
        executor.shutdown()
        if rclpy.ok():
            rclpy.shutdown()


def _spin_until(
    executor: SingleThreadedExecutor,
    predicate,
    *,
    timeout_s: float = 2.0,
) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        executor.spin_once(timeout_sec=0.05)
        if predicate():
            return True
    return False
