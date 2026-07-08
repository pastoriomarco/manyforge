"""Deterministic fake FollowJointTrajectory action server.

The fake server is intended for adapter contract tests.  It gives ManyForge a
stable ROS target for goal acceptance, feedback, success, and cancellation
without requiring Isaac Sim, MoveIt, or real robot drivers.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from control_msgs.action import FollowJointTrajectory
from rclpy.action import ActionServer, CancelResponse, GoalResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from trajectory_msgs.msg import JointTrajectoryPoint


@dataclass
class FakeTrajectoryGoalRecord:
    joint_names: list[str]
    point_count: int
    points_positions: list[list[float]] = field(default_factory=list)
    path_tolerance_positions: list[float] = field(default_factory=list)
    goal_tolerance_positions: list[float] = field(default_factory=list)
    canceled: bool = False


@dataclass
class FakeFollowJointTrajectoryServer:
    """Small FollowJointTrajectory action server for ROS integration tests.

    ``reentrant_callbacks=True`` models a controller action server whose cancel
    path can progress while the goal execution callback is still active. Use it
    for cancellation-latency probes; leave it false when a test deliberately
    wants the simpler single-callback-group fake.
    """

    node: object
    action_name: str = "/fake_arm_controller/follow_joint_trajectory"
    feedback_delay_s: float = 0.0
    reject_empty_goals: bool = True
    abort_after_feedback: bool = False
    feedback_error_positions: list[float] | None = None
    actual_position_offsets: list[float] | None = None
    hold_after_first_feedback_s: float = 0.0
    hold_cancel_poll_period_s: float = 0.01
    abort_error_code: int = FollowJointTrajectory.Result.PATH_TOLERANCE_VIOLATED
    abort_error_string: str = "fake path tolerance violation"
    reentrant_callbacks: bool = False
    goals: list[FakeTrajectoryGoalRecord] = field(default_factory=list)

    def __post_init__(self) -> None:
        callback_group = ReentrantCallbackGroup() if self.reentrant_callbacks else None
        self._server = ActionServer(
            self.node,
            FollowJointTrajectory,
            self.action_name,
            execute_callback=self._execute_callback,
            goal_callback=self._goal_callback,
            cancel_callback=self._cancel_callback,
            callback_group=callback_group,
        )

    def destroy(self) -> None:
        self._server.destroy()

    def _goal_callback(self, goal_request: FollowJointTrajectory.Goal) -> int:
        trajectory = goal_request.trajectory
        if self.reject_empty_goals and (
            not trajectory.joint_names or not trajectory.points
        ):
            return GoalResponse.REJECT
        return GoalResponse.ACCEPT

    def _cancel_callback(self, _goal_handle: object) -> int:
        return CancelResponse.ACCEPT

    def _execute_callback(self, goal_handle: object) -> FollowJointTrajectory.Result:
        trajectory = goal_handle.request.trajectory
        record = FakeTrajectoryGoalRecord(
            joint_names=list(trajectory.joint_names),
            point_count=len(trajectory.points),
            points_positions=[list(point.positions) for point in trajectory.points],
            path_tolerance_positions=[
                float(tolerance.position)
                for tolerance in goal_handle.request.path_tolerance
            ],
            goal_tolerance_positions=[
                float(tolerance.position)
                for tolerance in goal_handle.request.goal_tolerance
            ],
        )
        self.goals.append(record)

        for index, point in enumerate(trajectory.points):
            if goal_handle.is_cancel_requested:
                record.canceled = True
                goal_handle.canceled()
                return self._result(0, "canceled")

            feedback = FollowJointTrajectory.Feedback()
            feedback.joint_names = list(trajectory.joint_names)
            feedback.desired = point
            feedback.actual = JointTrajectoryPoint()
            feedback.actual.positions = list(point.positions)
            feedback.actual.velocities = list(point.velocities)
            feedback.actual.accelerations = list(point.accelerations)
            if self.actual_position_offsets is not None:
                feedback.actual.positions = [
                    value + offset
                    for value, offset in zip(
                        feedback.actual.positions,
                        self.actual_position_offsets,
                        strict=False,
                    )
                ]
            feedback.error = JointTrajectoryPoint()
            if self.feedback_error_positions is not None:
                feedback.error.positions = list(self.feedback_error_positions)
            else:
                feedback.error.positions = [0.0 for _ in trajectory.joint_names]
            goal_handle.publish_feedback(feedback)

            if index == 0 and self.hold_after_first_feedback_s > 0:
                deadline = time.monotonic() + self.hold_after_first_feedback_s
                poll_period = max(0.001, self.hold_cancel_poll_period_s)
                while time.monotonic() < deadline:
                    if goal_handle.is_cancel_requested:
                        record.canceled = True
                        goal_handle.canceled()
                        return self._result(0, "canceled")
                    time.sleep(min(poll_period, deadline - time.monotonic()))

            if self.feedback_delay_s > 0:
                time.sleep(self.feedback_delay_s)

            if self.abort_after_feedback and index == 0:
                goal_handle.abort()
                return self._result(self.abort_error_code, self.abort_error_string)

        goal_handle.succeed()
        return self._result(
            FollowJointTrajectory.Result.SUCCESSFUL,
            "fake trajectory completed",
        )

    @staticmethod
    def _result(error_code: int, error_string: str) -> FollowJointTrajectory.Result:
        result = FollowJointTrajectory.Result()
        result.error_code = error_code
        result.error_string = error_string
        return result
