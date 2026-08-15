"""Thin GripperCommand-to-Isaac JointState transport adapter.

ManyForge's retained C++ backend owns command meaning, lifecycle admission,
timeouts, cancellation, and status. This process only adapts the standard
``GripperCommand`` action used by the UR/Robotiq profile to Isaac's raw
``JointState`` command and observed-state topics. It consumes the same single
backend command-enable fact as the arm Stop-topic adapter and fails closed on
publisher loss or ambiguity.
"""

from __future__ import annotations

import math
import threading
import time
from collections.abc import Sequence

import rclpy
from control_msgs.action import GripperCommand
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import JointState
from std_msgs.msg import Bool

from manyforge_ros_bringup.isaac_gripper_command import (
    SettledCompletionWindow,
    SingleGoalReservation,
    coherent_joint_positions,
    finite_float,
    joint_positions_match,
    joint_state_contract_ready,
    log_readiness_transition,
    map_gripper_positions,
)
from manyforge_ros_bringup.isaac_topic_command_gate import (
    DEFAULT_ENABLE_TOPIC,
    DEFAULT_OWNER_NODE,
    DEFAULT_OWNER_NAMESPACE,
    owner_endpoint_identity,
)


DEFAULT_COMMAND_JOINT_NAMES = ["finger_joint"]
DEFAULT_COMMAND_MULTIPLIERS = [1.0]
DEFAULT_COMMAND_OFFSETS = [0.0]


class IsaacGripperActionBridge(Node):
    """Adapt one standard gripper action to one exact Isaac raw transport."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_gripper_action_bridge")
        self._callback_group = ReentrantCallbackGroup()
        self._declare_parameters()
        self._load_parameters()
        self._validate_config()

        self._lock = threading.Lock()
        self._latest_positions: dict[str, float] = {}
        self._latest_state_time = 0.0
        self._latest_state_generation = 0
        self._command_enabled = False
        self._owner_topology_valid = False
        self._sample_owner_identity: tuple[int, ...] | None = None
        self._active_target_positions: list[float] | None = None
        self._goal_reservation = SingleGoalReservation()
        self._last_readiness_value: bool | None = None
        self._last_readiness_publish_time = 0.0

        self._command_publisher = self.create_publisher(
            JointState,
            self._command_topic,
            10,
        )
        latched_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._readiness_publisher = self.create_publisher(
            Bool,
            self._readiness_topic,
            latched_qos,
        )
        self._state_subscription = self.create_subscription(
            JointState,
            self._state_topic,
            self._on_joint_state,
            10,
            callback_group=self._callback_group,
        )
        self._gate_subscription = self.create_subscription(
            Bool,
            self._command_enable_topic,
            self._on_command_enable,
            latched_qos,
            callback_group=self._callback_group,
        )
        self._timer = self.create_timer(
            self._publish_period_s,
            self._reconcile,
            callback_group=self._callback_group,
        )
        self._action_server = ActionServer(
            self,
            GripperCommand,
            self._action_name,
            execute_callback=self._execute_goal,
            goal_callback=self._goal_callback,
            cancel_callback=self._cancel_callback,
            callback_group=self._callback_group,
        )
        self._reconcile()
        self.get_logger().info(
            "Isaac gripper adapter ready: %s -> %s, owner gate %s"
            % (self._action_name, self._command_topic, self._command_enable_topic)
        )

    def _declare_parameters(self) -> None:
        self.declare_parameter(
            "action_name",
            DEFAULT_OWNER_NAMESPACE
            + "/robotiq_gripper_controller/gripper_cmd",
        )
        self.declare_parameter("command_topic", "/isaac_gripper_state")
        self.declare_parameter("state_topic", "/isaac_joint_states")
        self.declare_parameter(
            "readiness_topic",
            DEFAULT_OWNER_NAMESPACE + "/manyforge/isaac_gripper_bridge/ready",
        )
        self.declare_parameter("command_enable_topic", DEFAULT_ENABLE_TOPIC)
        self.declare_parameter("expected_owner_node", DEFAULT_OWNER_NODE)
        self.declare_parameter(
            "expected_owner_namespace", DEFAULT_OWNER_NAMESPACE
        )
        self.declare_parameter("commanded_joint", "finger_joint")
        self.declare_parameter("command_joint_names", DEFAULT_COMMAND_JOINT_NAMES)
        self.declare_parameter("command_multipliers", DEFAULT_COMMAND_MULTIPLIERS)
        self.declare_parameter("command_offsets", DEFAULT_COMMAND_OFFSETS)
        self.declare_parameter("completion_joint_names", DEFAULT_COMMAND_JOINT_NAMES)
        self.declare_parameter(
            "completion_multipliers", DEFAULT_COMMAND_MULTIPLIERS
        )
        self.declare_parameter("completion_offsets", DEFAULT_COMMAND_OFFSETS)
        self.declare_parameter("completion_tolerance_rad", 0.04)
        self.declare_parameter("completion_settle_time_s", 0.1)
        self.declare_parameter("completion_min_samples", 3)
        self.declare_parameter("state_freshness_timeout_s", 1.0)
        self.declare_parameter("goal_timeout_s", 20.0)
        self.declare_parameter("publish_period_s", 0.05)
        self.declare_parameter("hold_last_target", True)
        self.declare_parameter("require_state_before_goal", True)
        self.declare_parameter("require_all_state_joints", True)

    def _load_parameters(self) -> None:
        text = lambda name: str(self.get_parameter(name).value)
        array = lambda name: list(self.get_parameter(name).value)
        self._action_name = text("action_name")
        self._command_topic = text("command_topic")
        self._state_topic = text("state_topic")
        self._readiness_topic = text("readiness_topic")
        self._command_enable_topic = text("command_enable_topic")
        self._expected_owner_node = text("expected_owner_node")
        self._expected_owner_namespace = text("expected_owner_namespace")
        self._commanded_joint = text("commanded_joint")
        self._command_joint_names = array("command_joint_names")
        self._command_multipliers = array("command_multipliers")
        self._command_offsets = array("command_offsets")
        self._completion_joint_names = array("completion_joint_names")
        self._completion_multipliers = array("completion_multipliers")
        self._completion_offsets = array("completion_offsets")
        self._completion_tolerance_rad = float(
            self.get_parameter("completion_tolerance_rad").value
        )
        self._completion_settle_time_s = float(
            self.get_parameter("completion_settle_time_s").value
        )
        self._completion_min_samples = int(
            self.get_parameter("completion_min_samples").value
        )
        self._state_freshness_timeout_s = float(
            self.get_parameter("state_freshness_timeout_s").value
        )
        self._goal_timeout_s = float(self.get_parameter("goal_timeout_s").value)
        self._publish_period_s = float(
            self.get_parameter("publish_period_s").value
        )
        self._hold_last_target = bool(self.get_parameter("hold_last_target").value)
        self._require_state_before_goal = bool(
            self.get_parameter("require_state_before_goal").value
        )
        self._require_all_state_joints = bool(
            self.get_parameter("require_all_state_joints").value
        )

    def _validate_config(self) -> None:
        for name, value in (
            ("action_name", self._action_name),
            ("command_topic", self._command_topic),
            ("state_topic", self._state_topic),
            ("readiness_topic", self._readiness_topic),
            ("command_enable_topic", self._command_enable_topic),
            ("expected_owner_node", self._expected_owner_node),
            ("expected_owner_namespace", self._expected_owner_namespace),
            ("commanded_joint", self._commanded_joint),
        ):
            if not value or len(value.encode("utf-8")) > 1024:
                raise ValueError(f"{name} must be bounded non-empty text")
        if not self._expected_owner_namespace.startswith("/"):
            raise ValueError("expected_owner_namespace must be absolute")
        self._validate_mapping(
            "command",
            self._command_joint_names,
            self._command_multipliers,
            self._command_offsets,
        )
        self._validate_mapping(
            "completion",
            self._completion_joint_names,
            self._completion_multipliers,
            self._completion_offsets,
        )
        if self._commanded_joint not in self._command_joint_names:
            raise ValueError("commanded_joint must be in command_joint_names")
        if self._commanded_joint not in self._completion_joint_names:
            raise ValueError("commanded_joint must be in completion_joint_names")
        if not finite_float(self._completion_tolerance_rad) or not (
            0.0 <= self._completion_tolerance_rad <= math.pi
        ):
            raise ValueError("completion_tolerance_rad must be in [0, pi]")
        if not finite_float(self._completion_settle_time_s) or not (
            0.0 <= self._completion_settle_time_s <= 10.0
        ):
            raise ValueError("completion_settle_time_s must be in [0, 10]")
        if not 1 <= self._completion_min_samples <= 10_000:
            raise ValueError("completion_min_samples must be in [1, 10000]")
        for name, value, minimum, maximum in (
            ("state_freshness_timeout_s", self._state_freshness_timeout_s, 0.01, 60.0),
            ("goal_timeout_s", self._goal_timeout_s, 0.1, 120.0),
            ("publish_period_s", self._publish_period_s, 0.005, 1.0),
        ):
            if not finite_float(value) or not minimum <= value <= maximum:
                raise ValueError(f"{name} must be in [{minimum}, {maximum}]")

    @staticmethod
    def _validate_mapping(
        label: str,
        names: Sequence[str],
        multipliers: Sequence[float],
        offsets: Sequence[float],
    ) -> None:
        if not 1 <= len(names) <= 64 or len(set(names)) != len(names):
            raise ValueError(f"{label}_joint_names must contain 1..64 unique names")
        if len(multipliers) != len(names) or len(offsets) != len(names):
            raise ValueError(f"{label} mapping lengths must match")
        if any(not name or len(name.encode("utf-8")) > 256 for name in names):
            raise ValueError(f"{label}_joint_names contain invalid names")
        if not all(finite_float(value) for value in (*multipliers, *offsets)):
            raise ValueError(f"{label} mapping values must be finite")

    def _owner_endpoint_identity(self) -> tuple[int, ...] | None:
        try:
            publishers = self.get_publishers_info_by_topic(
                self._command_enable_topic
            )
        except Exception:
            return None
        return owner_endpoint_identity(
            publishers,
            expected_node_name=self._expected_owner_node,
            expected_node_namespace=self._expected_owner_namespace,
        )

    def _on_command_enable(self, message: Bool) -> None:
        identity = self._owner_endpoint_identity()
        topology_valid = identity is not None
        with self._lock:
            self._sample_owner_identity = identity
            self._owner_topology_valid = topology_valid
            self._command_enabled = bool(message.data) if topology_valid else False
            if not self._command_enabled:
                self._active_target_positions = None
        self._publish_readiness_if_due(force=True)

    def _on_joint_state(self, message: JointState) -> None:
        positions = coherent_joint_positions(message.name, message.position)
        with self._lock:
            self._latest_positions = positions or {}
            self._latest_state_time = time.monotonic()
            self._latest_state_generation += 1
        self._publish_readiness_if_due()

    def _goal_callback(self, request: GripperCommand.Goal) -> GoalResponse:
        if not finite_float(request.command.position):
            return GoalResponse.REJECT
        if not finite_float(request.command.max_effort) or request.command.max_effort < 0:
            return GoalResponse.REJECT
        if not self._commands_enabled():
            self.get_logger().warning("Rejecting gripper goal: command gate closed")
            return GoalResponse.REJECT
        if self._require_state_before_goal and not self._state_is_fresh():
            return GoalResponse.REJECT
        if self._require_all_state_joints and not self._all_state_joints_present():
            return GoalResponse.REJECT
        if not self._goal_reservation.try_acquire():
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _cancel_callback(self, _goal_handle: object) -> CancelResponse:
        return CancelResponse.ACCEPT

    def _execute_goal(self, goal_handle: object) -> GripperCommand.Result:
        try:
            return self._execute_reserved_goal(goal_handle)
        finally:
            self._goal_reservation.release()

    def _execute_reserved_goal(self, goal_handle: object) -> GripperCommand.Result:
        target = float(goal_handle.request.command.position)
        command_positions = map_gripper_positions(
            target,
            self._command_multipliers,
            self._command_offsets,
        )
        completion_positions = map_gripper_positions(
            target,
            self._completion_multipliers,
            self._completion_offsets,
        )
        with self._lock:
            self._active_target_positions = list(command_positions)
            first_generation = self._latest_state_generation
        self._publish_command(command_positions)
        window = SettledCompletionWindow(
            settle_time_s=self._completion_settle_time_s,
            min_samples=self._completion_min_samples,
            last_generation=first_generation,
        )
        deadline = time.monotonic() + self._goal_timeout_s
        feedback = GripperCommand.Feedback()
        result = GripperCommand.Result()

        while rclpy.ok():
            if not self._commands_enabled():
                goal_handle.abort()
                return self._finish_result(result, target, reached=False, stalled=True)
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                return self._finish_result(result, target, reached=False, stalled=False)
            matches, generation = self._completion_observation(completion_positions)
            reached = window.observe(
                now=time.monotonic(),
                generation=generation,
                matches=matches,
            )
            feedback.position = self._current_position(target)
            feedback.effort = 0.0
            feedback.stalled = False
            feedback.reached_goal = reached
            goal_handle.publish_feedback(feedback)
            if reached:
                goal_handle.succeed()
                return self._finish_result(
                    result,
                    target,
                    reached=True,
                    stalled=False,
                    retain_target=self._hold_last_target,
                )
            if time.monotonic() >= deadline:
                goal_handle.abort()
                return self._finish_result(result, target, reached=False, stalled=True)
            self._publish_command(command_positions)
            time.sleep(self._publish_period_s)

        goal_handle.abort()
        return self._finish_result(result, target, reached=False, stalled=True)

    def _finish_result(
        self,
        result: GripperCommand.Result,
        fallback: float,
        *,
        reached: bool,
        stalled: bool,
        retain_target: bool = False,
    ) -> GripperCommand.Result:
        result.position = self._current_position(fallback)
        result.effort = 0.0
        result.stalled = stalled
        result.reached_goal = reached
        if not retain_target:
            with self._lock:
                self._active_target_positions = None
        return result

    def _commands_enabled(self) -> bool:
        with self._lock:
            return self._owner_topology_valid and self._command_enabled

    def _state_is_fresh(self) -> bool:
        with self._lock:
            return self._state_is_fresh_locked(time.monotonic())

    def _state_is_fresh_locked(self, now: float) -> bool:
        return (
            self._latest_state_time > 0.0
            and bool(self._latest_positions)
            and now - self._latest_state_time <= self._state_freshness_timeout_s
        )

    def _all_state_joints_present(self) -> bool:
        with self._lock:
            return joint_state_contract_ready(
                self._latest_positions,
                self._completion_joint_names,
            )

    def _completion_observation(
        self,
        targets: Sequence[float],
    ) -> tuple[bool, int]:
        with self._lock:
            generation = self._latest_state_generation
            if not self._state_is_fresh_locked(time.monotonic()):
                return False, generation
            names = self._completion_joint_names
            selected_targets = targets
            if not self._require_all_state_joints:
                index = names.index(self._commanded_joint)
                names = [self._commanded_joint]
                selected_targets = [targets[index]]
            return (
                joint_positions_match(
                    self._latest_positions,
                    names,
                    selected_targets,
                    self._completion_tolerance_rad,
                ),
                generation,
            )

    def _current_position(self, fallback: float) -> float:
        with self._lock:
            value = self._latest_positions.get(self._commanded_joint)
        return float(value) if value is not None and finite_float(value) else fallback

    def _reconcile(self) -> None:
        identity = self._owner_endpoint_identity()
        with self._lock:
            topology_valid = (
                identity is not None and identity == self._sample_owner_identity
            )
            self._owner_topology_valid = topology_valid
            if not topology_valid:
                self._command_enabled = False
                self._sample_owner_identity = None
                self._active_target_positions = None
            target = (
                list(self._active_target_positions)
                if self._active_target_positions is not None
                and self._command_enabled
                and topology_valid
                else None
            )
        if target is not None:
            self._publish_command(target)
        self._publish_readiness_if_due()

    def _publish_readiness_if_due(self, *, force: bool = False) -> None:
        now = time.monotonic()
        with self._lock:
            ready = (
                self._owner_topology_valid
                and self._state_is_fresh_locked(now)
                and joint_state_contract_ready(
                    self._latest_positions,
                    self._completion_joint_names,
                )
            )
            if (
                not force
                and ready == self._last_readiness_value
                and now - self._last_readiness_publish_time < 1.0
            ):
                return
            previous = self._last_readiness_value
            changed = previous != ready
            self._last_readiness_value = ready
            self._last_readiness_publish_time = now
        message = Bool()
        message.data = ready
        self._readiness_publisher.publish(message)
        if changed:
            log_readiness_transition(
                self.get_logger(),
                ready=ready,
                previous=previous,
                required_joint_names=self._completion_joint_names,
            )

    def _publish_command(self, positions: Sequence[float]) -> None:
        message = JointState()
        message.header.stamp = self.get_clock().now().to_msg()
        message.name = list(self._command_joint_names)
        message.position = [float(value) for value in positions]
        self._command_publisher.publish(message)


def main() -> int:
    rclpy.init()
    node: IsaacGripperActionBridge | None = None
    executor: MultiThreadedExecutor | None = None
    try:
        node = IsaacGripperActionBridge()
        executor = MultiThreadedExecutor(num_threads=2)
        executor.add_node(node)
        executor.spin()
        return 0
    finally:
        if executor is not None and node is not None:
            executor.remove_node(node)
        if node is not None:
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
