"""Isaac Sim gripper action bridge.

This node exposes the standard ``control_msgs/action/GripperCommand`` surface
used by ros2_control and real gripper drivers, then translates each command
into the raw ``sensor_msgs/JointState`` message consumed by Isaac Sim action
graphs. The raw Isaac topic is intentionally isolated here so ManyForge runtime
code does not depend on simulator-specific command topics.
"""

from __future__ import annotations

import threading
import time
from typing import Sequence

import rclpy
from control_msgs.action import GripperCommand
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from sensor_msgs.msg import JointState

from manyforge_ros_bringup.isaac_gripper_command import (
    finite_float,
    map_gripper_positions,
    select_pneumatic_effort,
)


# Command only the canonical Robotiq drive joint. The remaining USD articulation
# joints are mechanically coupled/passive state and must not receive competing
# position targets.
DEFAULT_JOINT_NAMES = ["finger_joint"]
DEFAULT_MULTIPLIERS = [1.0]
DEFAULT_OFFSETS = [0.0] * len(DEFAULT_JOINT_NAMES)


class IsaacGripperActionBridge(Node):
    """Translate standard gripper action goals into Isaac JointState commands."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_gripper_action_bridge")
        self._callback_group = ReentrantCallbackGroup()

        self.declare_parameter(
            "action_name", "/robotiq_gripper_controller/gripper_cmd"
        )
        self.declare_parameter("command_topic", "/isaac_gripper_state")
        self.declare_parameter("state_topic", "/isaac_joint_states")
        self.declare_parameter("commanded_joint", "finger_joint")
        self.declare_parameter("joint_names", DEFAULT_JOINT_NAMES)
        self.declare_parameter("multipliers", DEFAULT_MULTIPLIERS)
        self.declare_parameter("offsets", DEFAULT_OFFSETS)
        self.declare_parameter("completion_tolerance_rad", 0.04)
        self.declare_parameter("actuation_kind", "position_commanded")
        self.declare_parameter("completion_policy", "position_reached")
        self.declare_parameter("open_position", 0.0)
        self.declare_parameter("closed_position", 0.0)
        self.declare_parameter("open_effort", 0.0)
        self.declare_parameter("close_effort", 0.0)
        self.declare_parameter("dwell_s", 0.0)
        self.declare_parameter("publish_nominal_position", True)
        self.declare_parameter("state_freshness_timeout_s", 1.0)
        self.declare_parameter("goal_timeout_s", 5.0)
        self.declare_parameter("publish_period_s", 0.05)
        self.declare_parameter("hold_last_target", True)
        self.declare_parameter("require_state_before_goal", True)
        self.declare_parameter("require_all_state_joints", True)

        self._action_name = (
            self.get_parameter("action_name").get_parameter_value().string_value
        )
        self._command_topic = (
            self.get_parameter("command_topic").get_parameter_value().string_value
        )
        self._state_topic = (
            self.get_parameter("state_topic").get_parameter_value().string_value
        )
        self._commanded_joint = (
            self.get_parameter("commanded_joint").get_parameter_value().string_value
        )
        self._joint_names = list(
            self.get_parameter("joint_names")
            .get_parameter_value()
            .string_array_value
        )
        self._multipliers = list(
            self.get_parameter("multipliers")
            .get_parameter_value()
            .double_array_value
        )
        self._offsets = list(
            self.get_parameter("offsets").get_parameter_value().double_array_value
        )
        self._completion_tolerance_rad = (
            self.get_parameter("completion_tolerance_rad")
            .get_parameter_value()
            .double_value
        )
        self._actuation_kind = (
            self.get_parameter("actuation_kind").get_parameter_value().string_value
        )
        self._completion_policy = (
            self.get_parameter("completion_policy")
            .get_parameter_value()
            .string_value
        )
        self._open_position = (
            self.get_parameter("open_position").get_parameter_value().double_value
        )
        self._closed_position = (
            self.get_parameter("closed_position").get_parameter_value().double_value
        )
        self._open_effort = (
            self.get_parameter("open_effort").get_parameter_value().double_value
        )
        self._close_effort = (
            self.get_parameter("close_effort").get_parameter_value().double_value
        )
        self._dwell_s = (
            self.get_parameter("dwell_s").get_parameter_value().double_value
        )
        self._publish_nominal_position = (
            self.get_parameter("publish_nominal_position")
            .get_parameter_value()
            .bool_value
        )
        self._state_freshness_timeout_s = (
            self.get_parameter("state_freshness_timeout_s")
            .get_parameter_value()
            .double_value
        )
        self._goal_timeout_s = (
            self.get_parameter("goal_timeout_s").get_parameter_value().double_value
        )
        self._publish_period_s = max(
            0.005,
            self.get_parameter("publish_period_s").get_parameter_value().double_value,
        )
        self._hold_last_target = (
            self.get_parameter("hold_last_target").get_parameter_value().bool_value
        )
        self._require_state_before_goal = (
            self.get_parameter("require_state_before_goal")
            .get_parameter_value()
            .bool_value
        )
        self._require_all_state_joints = (
            self.get_parameter("require_all_state_joints")
            .get_parameter_value()
            .bool_value
        )

        self._validate_config()

        self._lock = threading.Lock()
        self._latest_positions: dict[str, float] = {}
        self._latest_state_time = 0.0
        self._active_target: float | None = None
        self._active_target_positions: list[float] | None = None
        self._active_target_efforts: list[float] | None = None

        self._publisher = self.create_publisher(
            JointState, self._command_topic, 10
        )
        self._state_subscription = self.create_subscription(
            JointState,
            self._state_topic,
            self._on_joint_state,
            10,
            callback_group=self._callback_group,
        )
        self._timer = self.create_timer(
            self._publish_period_s,
            self._publish_active_target,
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

        self.get_logger().info(
            "Isaac gripper bridge ready: %s -> %s using %d joints"
            % (self._action_name, self._command_topic, len(self._joint_names))
        )

    def _validate_config(self) -> None:
        if not self._action_name:
            raise ValueError("action_name must not be empty")
        if not self._command_topic:
            raise ValueError("command_topic must not be empty")
        if not self._state_topic:
            raise ValueError("state_topic must not be empty")
        if not self._joint_names:
            raise ValueError("joint_names must not be empty")
        if len(set(self._joint_names)) != len(self._joint_names):
            raise ValueError("joint_names must not contain duplicates")
        if len(self._multipliers) != len(self._joint_names):
            raise ValueError("multipliers length must match joint_names length")
        if len(self._offsets) != len(self._joint_names):
            raise ValueError("offsets length must match joint_names length")
        if self._commanded_joint not in self._joint_names:
            raise ValueError("commanded_joint must be listed in joint_names")
        if self._completion_tolerance_rad < 0.0:
            raise ValueError("completion_tolerance_rad must be >= 0")
        if self._actuation_kind not in {"position_commanded", "pneumatic"}:
            raise ValueError(
                "actuation_kind must be position_commanded or pneumatic"
            )
        if self._completion_policy not in {"position_reached", "dwell_elapsed"}:
            raise ValueError(
                "completion_policy must be position_reached or dwell_elapsed"
            )
        if self._actuation_kind == "pneumatic":
            if self._completion_policy != "dwell_elapsed":
                raise ValueError(
                    "pneumatic actuation requires completion_policy=dwell_elapsed"
                )
            for field_name, value in (
                ("open_position", self._open_position),
                ("closed_position", self._closed_position),
                ("open_effort", self._open_effort),
                ("close_effort", self._close_effort),
                ("dwell_s", self._dwell_s),
            ):
                if not finite_float(value):
                    raise ValueError(f"{field_name} must be finite")
            if abs(self._open_position - self._closed_position) <= 1e-9:
                raise ValueError(
                    "pneumatic open_position and closed_position must differ"
                )
            if self._dwell_s < 0.0:
                raise ValueError("dwell_s must be >= 0")
        elif self._completion_policy != "position_reached":
            raise ValueError(
                "position_commanded actuation requires "
                "completion_policy=position_reached"
            )
        if self._state_freshness_timeout_s < 0.0:
            raise ValueError("state_freshness_timeout_s must be >= 0")
        if self._goal_timeout_s < 0.0:
            raise ValueError("goal_timeout_s must be >= 0")

    def _on_joint_state(self, msg: JointState) -> None:
        now = time.monotonic()
        positions = {
            name: float(msg.position[index])
            for index, name in enumerate(msg.name)
            if index < len(msg.position)
        }
        with self._lock:
            self._latest_positions = positions
            self._latest_state_time = now

    def _goal_callback(self, goal_request: GripperCommand.Goal) -> GoalResponse:
        del goal_request
        if self._require_state_before_goal and not self._state_is_fresh():
            self.get_logger().warn(
                "Rejecting gripper goal because no fresh Isaac gripper state "
                "sample is available"
            )
            return GoalResponse.REJECT
        if self._require_all_state_joints and not self._all_state_joints_present():
            self.get_logger().warn(
                "Rejecting gripper goal because Isaac state is missing one or "
                "more configured gripper joints"
            )
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _cancel_callback(self, _goal_handle) -> CancelResponse:
        return CancelResponse.ACCEPT

    def _execute_goal(self, goal_handle) -> GripperCommand.Result:
        target = float(goal_handle.request.command.position)
        target_positions = map_gripper_positions(
            target, self._multipliers, self._offsets
        )
        start = time.monotonic()
        deadline = None if self._goal_timeout_s == 0.0 else start + self._goal_timeout_s

        with self._lock:
            self._active_target = target
            self._active_target_positions = target_positions
            self._active_target_efforts = self._target_efforts(target)
        self._publish_command(target_positions, self._target_efforts(target))

        feedback = GripperCommand.Feedback()
        result = GripperCommand.Result()

        if self._actuation_kind == "pneumatic":
            return self._execute_pneumatic_goal(
                goal_handle=goal_handle,
                target=target,
                target_positions=target_positions,
                start=start,
                deadline=deadline,
                feedback=feedback,
                result=result,
            )

        while rclpy.ok():
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                with self._lock:
                    self._active_target = None
                    self._active_target_positions = None
                    self._active_target_efforts = None
                result.position = self._current_commanded_position(target)
                result.effort = 0.0
                result.stalled = False
                result.reached_goal = False
                return result

            reached_goal = self._target_reached(target_positions)
            current_position = self._current_commanded_position(target)
            feedback.position = current_position
            feedback.effort = 0.0
            feedback.stalled = False
            feedback.reached_goal = reached_goal
            goal_handle.publish_feedback(feedback)

            if reached_goal:
                goal_handle.succeed()
                result.position = current_position
                result.effort = 0.0
                result.stalled = False
                result.reached_goal = True
                return result

            if deadline is not None and time.monotonic() >= deadline:
                goal_handle.abort()
                result.position = current_position
                result.effort = 0.0
                result.stalled = True
                result.reached_goal = False
                return result

            self._publish_command(target_positions, self._target_efforts(target))
            time.sleep(self._publish_period_s)

        goal_handle.abort()
        result.position = self._current_commanded_position(target)
        result.effort = 0.0
        result.stalled = True
        result.reached_goal = False
        return result

    def _execute_pneumatic_goal(
        self,
        *,
        goal_handle,
        target: float,
        target_positions: Sequence[float],
        start: float,
        deadline: float | None,
        feedback: GripperCommand.Feedback,
        result: GripperCommand.Result,
    ) -> GripperCommand.Result:
        effort = select_pneumatic_effort(
            target,
            self._open_position,
            self._closed_position,
            self._open_effort,
            self._close_effort,
        )
        efforts = [effort for _ in self._joint_names]
        dwell_deadline = start + self._dwell_s

        while rclpy.ok():
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                with self._lock:
                    self._active_target = None
                    self._active_target_positions = None
                    self._active_target_efforts = None
                result.position = self._current_commanded_position(target)
                result.effort = effort
                result.stalled = False
                result.reached_goal = False
                return result

            current_position = self._current_commanded_position(target)
            reached_nominal = self._target_reached(target_positions)
            feedback.position = current_position
            feedback.effort = effort
            feedback.stalled = not reached_nominal
            feedback.reached_goal = False
            goal_handle.publish_feedback(feedback)

            now = time.monotonic()
            if now >= dwell_deadline:
                goal_handle.succeed()
                with self._lock:
                    self._active_target = None
                    self._active_target_positions = None
                    self._active_target_efforts = None
                result.position = current_position
                result.effort = effort
                result.stalled = not reached_nominal
                # The GripperCommand action lacks a "dwell elapsed" result bit.
                # Return reached_goal=true only as the action success contract;
                # ManyForge deployment/status semantics carry the real evidence.
                result.reached_goal = True
                return result

            if deadline is not None and now >= deadline:
                goal_handle.abort()
                result.position = current_position
                result.effort = effort
                result.stalled = True
                result.reached_goal = False
                return result

            self._publish_command(target_positions, efforts)
            time.sleep(self._publish_period_s)

        goal_handle.abort()
        result.position = self._current_commanded_position(target)
        result.effort = effort
        result.stalled = True
        result.reached_goal = False
        return result

    def _state_is_fresh(self) -> bool:
        if self._state_freshness_timeout_s == 0.0:
            return True
        with self._lock:
            if self._latest_state_time <= 0.0:
                return False
            return (
                time.monotonic() - self._latest_state_time
                <= self._state_freshness_timeout_s
            )

    def _all_state_joints_present(self) -> bool:
        with self._lock:
            return all(name in self._latest_positions for name in self._joint_names)

    def _target_reached(self, target_positions: Sequence[float]) -> bool:
        with self._lock:
            if not self._latest_positions:
                return False
            if self._require_all_state_joints:
                return all(
                    name in self._latest_positions
                    and abs(self._latest_positions[name] - target) <= self._completion_tolerance_rad
                    for name, target in zip(self._joint_names, target_positions)
                )
            if self._commanded_joint not in self._latest_positions:
                return False
            index = self._joint_names.index(self._commanded_joint)
            return (
                abs(
                    self._latest_positions[self._commanded_joint]
                    - target_positions[index]
                )
                <= self._completion_tolerance_rad
            )

    def _current_commanded_position(self, fallback: float) -> float:
        with self._lock:
            return float(self._latest_positions.get(self._commanded_joint, fallback))

    def _publish_active_target(self) -> None:
        if not self._hold_last_target:
            return
        with self._lock:
            target_positions = (
                list(self._active_target_positions)
                if self._active_target_positions is not None
                else None
            )
            target_efforts = (
                list(self._active_target_efforts)
                if self._active_target_efforts is not None
                else None
            )
        if target_positions is not None:
            self._publish_command(target_positions, target_efforts)

    def _target_efforts(self, target: float) -> list[float]:
        if self._actuation_kind != "pneumatic":
            return [0.0 for _ in self._joint_names]
        effort = select_pneumatic_effort(
            target,
            self._open_position,
            self._closed_position,
            self._open_effort,
            self._close_effort,
        )
        return [effort for _ in self._joint_names]

    def _publish_command(
        self,
        positions: Sequence[float],
        efforts: Sequence[float] | None = None,
    ) -> None:
        msg = JointState()
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.name = list(self._joint_names)
        if self._publish_nominal_position:
            msg.position = [float(position) for position in positions]
        if efforts is not None:
            msg.effort = [float(effort) for effort in efforts]
        self._publisher.publish(msg)


def main() -> None:
    rclpy.init()
    node = IsaacGripperActionBridge()
    executor = MultiThreadedExecutor(num_threads=2)
    executor.add_node(node)
    try:
        executor.spin()
    finally:
        executor.remove_node(node)
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
