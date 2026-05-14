#!/usr/bin/env python3
"""Send a tiny reversible FollowJointTrajectory goal to the Isaac arm bridge."""

from __future__ import annotations

import argparse
import sys
import time
from typing import Sequence

import rclpy
from control_msgs.action import FollowJointTrajectory
from rclpy.action import ActionClient
from sensor_msgs.msg import JointState
from trajectory_msgs.msg import JointTrajectoryPoint


ARM_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]


def _wait_for_joint_state(
    node: rclpy.node.Node,
    topic: str,
    required_joints: Sequence[str],
    timeout_s: float,
) -> dict[str, float]:
    latest: dict[str, float] = {}
    deadline = time.monotonic() + timeout_s

    def on_joint_state(msg: JointState) -> None:
        nonlocal latest
        values = dict(zip(msg.name, msg.position, strict=False))
        if all(joint in values for joint in required_joints):
            latest = {joint: float(values[joint]) for joint in required_joints}

    sub = node.create_subscription(JointState, topic, on_joint_state, 10)
    try:
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
            if latest:
                return latest
    finally:
        node.destroy_subscription(sub)
    raise TimeoutError(
        f"Timed out waiting for {topic} with joints {', '.join(required_joints)}"
    )


def _point(positions: Sequence[float], time_from_start_s: float) -> JointTrajectoryPoint:
    point = JointTrajectoryPoint()
    point.positions = [float(value) for value in positions]
    point.time_from_start.sec = int(time_from_start_s)
    point.time_from_start.nanosec = int(
        (time_from_start_s - int(time_from_start_s)) * 1_000_000_000
    )
    return point


def _send_goal(
    node: rclpy.node.Node,
    client: ActionClient,
    joints: Sequence[str],
    positions: Sequence[float],
    duration_s: float,
    timeout_s: float,
) -> None:
    goal = FollowJointTrajectory.Goal()
    goal.trajectory.joint_names = list(joints)
    goal.trajectory.points = [_point(positions, duration_s)]

    send_future = client.send_goal_async(goal)
    rclpy.spin_until_future_complete(node, send_future, timeout_sec=timeout_s)
    goal_handle = send_future.result()
    if goal_handle is None:
        raise RuntimeError("FollowJointTrajectory goal did not receive a response")
    if not goal_handle.accepted:
        raise RuntimeError("FollowJointTrajectory goal was rejected")

    result_future = goal_handle.get_result_async()
    rclpy.spin_until_future_complete(node, result_future, timeout_sec=timeout_s)
    result = result_future.result()
    if result is None:
        raise RuntimeError("FollowJointTrajectory goal did not finish")
    if result.result.error_code != FollowJointTrajectory.Result.SUCCESSFUL:
        raise RuntimeError(
            "FollowJointTrajectory failed: "
            f"{result.result.error_code} {result.result.error_string}"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--action-name",
        default="/isaac_arm_controller/follow_joint_trajectory",
    )
    parser.add_argument("--joint-state-topic", default="/joint_states")
    parser.add_argument("--delta-rad", type=float, default=0.02)
    parser.add_argument("--duration-s", type=float, default=0.8)
    parser.add_argument("--timeout-s", type=float, default=10.0)
    args = parser.parse_args(argv)

    rclpy.init()
    node = rclpy.create_node("manyforge_isaac_arm_action_smoke")
    try:
        start = _wait_for_joint_state(
            node,
            args.joint_state_topic,
            ARM_JOINTS,
            args.timeout_s,
        )
        start_positions = [start[joint] for joint in ARM_JOINTS]
        probe_positions = list(start_positions)
        probe_positions[0] += args.delta_rad

        client = ActionClient(node, FollowJointTrajectory, args.action_name)
        if not client.wait_for_server(timeout_sec=args.timeout_s):
            raise RuntimeError(f"Action server not available: {args.action_name}")

        _send_goal(
            node,
            client,
            ARM_JOINTS,
            probe_positions,
            args.duration_s,
            args.timeout_s,
        )
        _send_goal(
            node,
            client,
            ARM_JOINTS,
            start_positions,
            args.duration_s,
            args.timeout_s,
        )
        print(
            "Isaac arm action smoke passed: "
            f"moved {ARM_JOINTS[0]} by {args.delta_rad:.3f} rad and restored."
        )
        return 0
    except Exception as exc:
        print(f"Isaac arm action smoke failed: {exc}", file=sys.stderr)
        return 1
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
