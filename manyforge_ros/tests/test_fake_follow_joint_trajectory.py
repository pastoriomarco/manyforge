from __future__ import annotations

import threading
import time

import pytest


def _wait_for_future(future, timeout_s: float = 5.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if future.done():
            return future.result()
        time.sleep(0.01)
    raise TimeoutError("Timed out waiting for ROS future")


def _point(joint_trajectory_point_cls, positions: list[float], seconds: int):
    point = joint_trajectory_point_cls()
    point.positions = positions
    point.time_from_start.sec = seconds
    return point


def test_fake_follow_joint_trajectory_accepts_and_completes_goal():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")
    pytest.importorskip("trajectory_msgs")

    from action_msgs.msg import GoalStatus
    from control_msgs.action import FollowJointTrajectory
    from rclpy.action import ActionClient
    from rclpy.executors import MultiThreadedExecutor
    from trajectory_msgs.msg import JointTrajectoryPoint

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_fake_fjt_server_test")
    client_node = rclpy.create_node("manyforge_fake_fjt_client_test")
    fake_server = FakeFollowJointTrajectoryServer(server_node)
    client = ActionClient(
        client_node,
        FollowJointTrajectory,
        fake_server.action_name,
    )
    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        assert client.wait_for_server(timeout_sec=2.0)

        goal = FollowJointTrajectory.Goal()
        goal.trajectory.joint_names = ["joint_1", "joint_2"]
        goal.trajectory.points = [
            _point(JointTrajectoryPoint, [0.0, 0.0], 1),
            _point(JointTrajectoryPoint, [0.1, -0.1], 2),
        ]

        goal_handle = _wait_for_future(client.send_goal_async(goal))
        assert goal_handle.accepted

        result_response = _wait_for_future(goal_handle.get_result_async())
        assert result_response.status == GoalStatus.STATUS_SUCCEEDED
        assert result_response.result.error_code == FollowJointTrajectory.Result.SUCCESSFUL
        assert result_response.result.error_string == "fake trajectory completed"
        assert len(fake_server.goals) == 1
        assert fake_server.goals[0].joint_names == ["joint_1", "joint_2"]
        assert fake_server.goals[0].point_count == 2
    finally:
        fake_server.destroy()
        client.destroy()
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()
