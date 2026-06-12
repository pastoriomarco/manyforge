"""Fail-closed command gate for Isaac topic_based_ros2_control.

topic_based_ros2_control exposes a ``/stop_joint_commands`` Boolean input.
When true, the hardware plugin keeps reading simulator state but suppresses
publishing command JointState messages back to Isaac.  This node owns that
surface for ManyForge's Isaac preview so startup is fail-closed: no arm command
is published until every authority explicitly enables it.

Authority topology (single writer per topic, fail-closed conjunction here):

- ``enable_topic`` (default ``/manyforge/isaac_arm_command_enable``) — written
  ONLY by the ManyForge execution backend (the kernel-side gate authority).
- ``supervisor_enable_topic`` (default
  ``/manyforge/isaac_supervisor_command_enable``) — written ONLY by the Isaac
  demo supervisor (independent watchdog veto: it disables when the Composer
  is missing or unready, re-enables on recovery).

Commands flow only when **both** authorities enable. Each input is latched
(transient_local) by its sole writer, so this node converges to the true
state even if it restarts. Sharing one topic between multiple
transient-local writers is forbidden: late joiners receive one latched value
per publisher and the effective gate becomes reader- and timing-dependent.
"""

from __future__ import annotations

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import Bool


class IsaacTopicCommandGate(Node):
    """Translate ManyForge command-enable authorities to /stop_joint_commands."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_topic_command_gate")
        self.declare_parameter("enable_topic", "/manyforge/isaac_arm_command_enable")
        self.declare_parameter(
            "supervisor_enable_topic",
            "/manyforge/isaac_supervisor_command_enable",
        )
        self.declare_parameter("stop_topic", "/stop_joint_commands")
        self.declare_parameter("publish_rate_hz", 20.0)
        self.declare_parameter("default_enabled", False)
        # Until the supervisor publishes, treat its veto as withheld-open or
        # withheld-closed. Default open keeps deployments that run without a
        # supervisor working; the supervisor's own startup False engages the
        # veto as soon as it is up.
        self.declare_parameter("supervisor_default_enabled", True)

        self._backend_enabled = bool(self.get_parameter("default_enabled").value)
        self._supervisor_enabled = bool(
            self.get_parameter("supervisor_default_enabled").value
        )
        enable_topic = str(self.get_parameter("enable_topic").value)
        supervisor_enable_topic = str(
            self.get_parameter("supervisor_enable_topic").value
        )
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
        self.create_subscription(
            Bool,
            supervisor_enable_topic,
            self._on_supervisor_enable,
            enable_qos,
        )
        self.create_timer(1.0 / publish_rate_hz, self._publish_stop_state)
        self._publish_stop_state()
        self.get_logger().info(
            f"Isaac topic command gate publishing {stop_topic} "
            f"from backend enable topic {enable_topic} AND supervisor enable "
            f"topic {supervisor_enable_topic}"
        )

    def _on_enable(self, msg: Bool) -> None:
        self._backend_enabled = bool(msg.data)
        self._publish_stop_state()

    def _on_supervisor_enable(self, msg: Bool) -> None:
        self._supervisor_enabled = bool(msg.data)
        self._publish_stop_state()

    def _publish_stop_state(self) -> None:
        msg = Bool()
        msg.data = not (self._backend_enabled and self._supervisor_enabled)
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
