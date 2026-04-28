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
from trajectory_msgs.msg import JointTrajectoryPoint


@dataclass
class FakeTrajectoryGoalRecord:
    joint_names: list[str]
    point_count: int
    canceled: bool = False


@dataclass
class FakeFollowJointTrajectoryServer:
    """Small FollowJointTrajectory action server for ROS integration tests."""

    node: object
    action_name: str = "/fake_arm_controller/follow_joint_trajectory"
    feedback_delay_s: float = 0.0
    reject_empty_goals: bool = True
    goals: list[FakeTrajectoryGoalRecord] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._server = ActionServer(
            self.node,
            FollowJointTrajectory,
            self.action_name,
            execute_callback=self._execute_callback,
            goal_callback=self._goal_callback,
            cancel_callback=self._cancel_callback,
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
        )
        self.goals.append(record)

        for point in trajectory.points:
            if goal_handle.is_cancel_requested:
                record.canceled = True
                goal_handle.canceled()
                return self._result(0, "canceled")

            feedback = FollowJointTrajectory.Feedback()
            feedback.joint_names = list(trajectory.joint_names)
            feedback.desired = point
            feedback.actual = point
            feedback.error = JointTrajectoryPoint()
            feedback.error.positions = [0.0 for _ in trajectory.joint_names]
            goal_handle.publish_feedback(feedback)

            if self.feedback_delay_s > 0:
                time.sleep(self.feedback_delay_s)

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
