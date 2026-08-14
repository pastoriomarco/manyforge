"""Fail-closed adapter from the retained backend gate to Isaac's Stop topic.

The C++ ros2_control execution backend is the sole command-admission owner and
the sole publisher of ``/manyforge/isaac_arm_command_enable``. This node owns
no admission policy: it mechanically inverts that one Boolean fact for
``topic_based_ros2_control`` and closes the gate whenever the exact publisher
topology disappears or becomes ambiguous.
"""

from __future__ import annotations

import math
from typing import Final

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import Bool


DEFAULT_ENABLE_TOPIC: Final = "/manyforge/isaac_arm_command_enable"
DEFAULT_STOP_TOPIC: Final = "/stop_joint_commands"
DEFAULT_OWNER_NODE: Final = "manyforge_ros2_control_execution_backend"
DEFAULT_OWNER_NAMESPACE: Final = "/"


def stop_topic_value(*, owner_command_enabled: bool) -> bool:
    """Translate one owner-issued enable fact to the vendor stop-topic wire."""

    if type(owner_command_enabled) is not bool:
        raise TypeError("owner_command_enabled must be an exact bool")
    return not owner_command_enabled


def owner_topology_matches(
    publishers: object,
    *,
    expected_node_name: str,
    expected_node_namespace: str = DEFAULT_OWNER_NAMESPACE,
) -> bool:
    """Require exactly one publisher with the retained owner's full name."""

    if not isinstance(publishers, (list, tuple)):
        return False
    if (
        not expected_node_name
        or not expected_node_namespace.startswith("/")
        or len(publishers) != 1
    ):
        return False
    return (
        getattr(publishers[0], "node_name", None) == expected_node_name
        and getattr(publishers[0], "node_namespace", None)
        == expected_node_namespace
    )


def owner_endpoint_identity(
    publishers: object,
    *,
    expected_node_name: str,
    expected_node_namespace: str = DEFAULT_OWNER_NAMESPACE,
) -> tuple[int, ...] | None:
    """Return the exact DDS endpoint identity for one valid owner topology."""

    if not owner_topology_matches(
        publishers,
        expected_node_name=expected_node_name,
        expected_node_namespace=expected_node_namespace,
    ):
        return None
    try:
        identity = tuple(int(value) for value in publishers[0].endpoint_gid)
    except (AttributeError, TypeError, ValueError):
        return None
    if (
        not identity
        or len(identity) > 64
        or any(value < 0 or value > 255 for value in identity)
    ):
        return None
    return identity


class IsaacTopicCommandGate(Node):
    """Mechanical one-owner enable-to-Stop translator."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_topic_command_gate")
        self.declare_parameter("enable_topic", DEFAULT_ENABLE_TOPIC)
        self.declare_parameter("stop_topic", DEFAULT_STOP_TOPIC)
        self.declare_parameter("expected_owner_node", DEFAULT_OWNER_NODE)
        self.declare_parameter(
            "expected_owner_namespace", DEFAULT_OWNER_NAMESPACE
        )
        self.declare_parameter("publish_rate_hz", 20.0)

        self._enable_topic = str(self.get_parameter("enable_topic").value)
        stop_topic = str(self.get_parameter("stop_topic").value)
        self._expected_owner_node = str(
            self.get_parameter("expected_owner_node").value
        )
        self._expected_owner_namespace = str(
            self.get_parameter("expected_owner_namespace").value
        )
        publish_rate_hz = float(self.get_parameter("publish_rate_hz").value)
        if not self._enable_topic or not stop_topic:
            raise ValueError("enable_topic and stop_topic must be non-empty")
        if not self._expected_owner_node:
            raise ValueError("expected_owner_node must be non-empty")
        if (
            not self._expected_owner_namespace.startswith("/")
            or len(self._expected_owner_namespace.encode("utf-8")) > 1024
        ):
            raise ValueError(
                "expected_owner_namespace must be bounded and absolute"
            )
        if not math.isfinite(publish_rate_hz) or not 1.0 <= publish_rate_hz <= 200.0:
            raise ValueError("publish_rate_hz must be finite and in [1, 200]")

        self._owner_command_enabled = False
        self._owner_topology_valid = False
        self._sample_owner_identity: tuple[int, ...] | None = None
        self._last_published_stop: bool | None = None

        enable_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        stop_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._stop_publisher = self.create_publisher(Bool, stop_topic, stop_qos)
        self._enable_subscription = self.create_subscription(
            Bool,
            self._enable_topic,
            self._on_enable,
            enable_qos,
        )
        self._timer = self.create_timer(
            1.0 / publish_rate_hz,
            self._reconcile_and_publish,
        )
        self._reconcile_and_publish()
        self.get_logger().info(
            "Isaac Stop-topic adapter is fail-closed on %s from sole owner %s"
            % (self._enable_topic, self._expected_owner_node)
        )

    def _owner_endpoint_identity(self) -> tuple[int, ...] | None:
        try:
            publishers = self.get_publishers_info_by_topic(self._enable_topic)
        except Exception:
            return None
        return owner_endpoint_identity(
            publishers,
            expected_node_name=self._expected_owner_node,
            expected_node_namespace=self._expected_owner_namespace,
        )

    def _on_enable(self, message: Bool) -> None:
        identity = self._owner_endpoint_identity()
        topology_valid = identity is not None
        self._sample_owner_identity = identity
        self._owner_topology_valid = topology_valid
        self._owner_command_enabled = bool(message.data) if topology_valid else False
        self._publish_stop_state()

    def _reconcile_and_publish(self) -> None:
        identity = self._owner_endpoint_identity()
        topology_valid = (
            identity is not None and identity == self._sample_owner_identity
        )
        if not topology_valid:
            self._owner_command_enabled = False
            self._sample_owner_identity = None
        self._owner_topology_valid = topology_valid
        self._publish_stop_state()

    def _publish_stop_state(self) -> None:
        stopped = stop_topic_value(
            owner_command_enabled=(
                self._owner_topology_valid and self._owner_command_enabled
            )
        )
        message = Bool()
        message.data = stopped
        self._stop_publisher.publish(message)
        if stopped != self._last_published_stop:
            detail = "closed" if stopped else "open"
            self.get_logger().info(f"Isaac command gate is {detail}")
            self._last_published_stop = stopped


def main() -> int:
    rclpy.init()
    node: IsaacTopicCommandGate | None = None
    try:
        node = IsaacTopicCommandGate()
        rclpy.spin(node)
        return 0
    finally:
        if node is not None:
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
