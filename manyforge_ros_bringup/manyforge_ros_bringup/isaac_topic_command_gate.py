"""Fail-closed command gate for Isaac topic_based_ros2_control.

topic_based_ros2_control exposes a ``/stop_joint_commands`` Boolean input.
When true, the hardware plugin keeps reading simulator state but suppresses
publishing command JointState messages back to Isaac.  This node owns that
surface for ManyForge's Isaac preview so startup is fail-closed: no arm command
is published until the ManyForge execution backend explicitly enables it.
"""

from __future__ import annotations

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import Bool


class IsaacTopicCommandGate(Node):
    """Translate a ManyForge command-enable signal to /stop_joint_commands."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_topic_command_gate")
        self.declare_parameter("enable_topic", "/manyforge/isaac_arm_command_enable")
        self.declare_parameter("stop_topic", "/stop_joint_commands")
        self.declare_parameter("publish_rate_hz", 20.0)
        self.declare_parameter("default_enabled", False)

        self._enabled = bool(self.get_parameter("default_enabled").value)
        enable_topic = str(self.get_parameter("enable_topic").value)
        stop_topic = str(self.get_parameter("stop_topic").value)
        publish_rate_hz = float(self.get_parameter("publish_rate_hz").value)
        if publish_rate_hz <= 0.0:
            raise ValueError("publish_rate_hz must be > 0")

        enable_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._stop_pub = self.create_publisher(Bool, stop_topic, 1)
        self.create_subscription(Bool, enable_topic, self._on_enable, enable_qos)
        self.create_timer(1.0 / publish_rate_hz, self._publish_stop_state)
        self._publish_stop_state()
        self.get_logger().info(
            f"Isaac topic command gate publishing {stop_topic} "
            f"from enable topic {enable_topic}"
        )

    def _on_enable(self, msg: Bool) -> None:
        self._enabled = bool(msg.data)
        self._publish_stop_state()

    def _publish_stop_state(self) -> None:
        msg = Bool()
        msg.data = not self._enabled
        self._stop_pub.publish(msg)


def main() -> None:
    rclpy.init()
    node = IsaacTopicCommandGate()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
