#!/usr/bin/env python3
"""Live Isaac Lite6 smoke validator.

This is the focused Lane B' qualification probe for the Lite6 scaffold. It
expects Isaac Sim to be running and streaming the Lite6 scene, then validates
the complete command surface:

* stock JTC FollowJointTrajectory action for joint1..joint6;
* aggregated controller joint state for the arm;
* Isaac raw joint state for the lite gripper;
* either the legacy standard GripperCommand action exposed by the compatibility
  bridge, or direct Isaac JointState effort commands for Lite6 pneumatic
  profiles;
* raw /isaac_joint_commands_gripper messages emitted by the selected gripper
  command path.

The motion is intentionally small and reversible.
"""

from __future__ import annotations

import argparse
from datetime import UTC, datetime
import json
from pathlib import Path
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Sequence

import rclpy
from control_msgs.action import FollowJointTrajectory, GripperCommand
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.publisher import Publisher
from sensor_msgs.msg import JointState
from trajectory_msgs.msg import JointTrajectoryPoint


LITE6_ARM_JOINTS = ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
LITE6_GRIPPER_JOINT = "right_finger_joint"
RESULT_SCHEMA = "manyforge.isaac_lite6_live_smoke.v1"


@dataclass
class SmokeResult:
    passed: bool
    details: dict[str, object] = field(default_factory=dict)


class Lite6SmokeProbe(Node):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__("manyforge_isaac_lite6_smoke_probe")
        self.args = args
        self.arm_joints = list(args.arm_joints)
        self.gripper_joint = args.gripper_joint
        self.latest_arm_state: dict[str, float] = {}
        self.latest_isaac_state: dict[str, float] = {}
        self.latest_gripper_command: dict[str, float] = {}
        self.latest_gripper_effort: dict[str, float] = {}

        self.arm_client = ActionClient(
            self, FollowJointTrajectory, args.arm_action_name
        )
        self.gripper_client = (
            ActionClient(self, GripperCommand, args.gripper_action_name)
            if args.gripper_transport_kind == "ros_gripper_command"
            else None
        )
        self.direct_gripper_pub: Publisher | None = (
            self.create_publisher(JointState, args.gripper_command_topic, 10)
            if args.gripper_transport_kind == "isaac_joint_state_effort"
            else None
        )
        self.create_subscription(
            JointState, args.arm_joint_state_topic, self._on_arm_joint_state, 10
        )
        self.create_subscription(
            JointState, args.isaac_joint_state_topic, self._on_isaac_joint_state, 10
        )
        self.create_subscription(
            JointState,
            args.gripper_command_topic,
            self._on_gripper_command,
            10,
        )

    def _positions_for(
        self, msg: JointState, required_joints: Sequence[str]
    ) -> dict[str, float]:
        values = {
            name: float(msg.position[index])
            for index, name in enumerate(msg.name)
            if index < len(msg.position)
        }
        if not all(joint in values for joint in required_joints):
            return {}
        return {joint: values[joint] for joint in required_joints}

    def _on_arm_joint_state(self, msg: JointState) -> None:
        positions = self._positions_for(msg, self.arm_joints)
        if positions:
            self.latest_arm_state = positions

    def _on_isaac_joint_state(self, msg: JointState) -> None:
        positions = self._positions_for(msg, [*self.arm_joints, self.gripper_joint])
        if positions:
            self.latest_isaac_state = positions

    def _on_gripper_command(self, msg: JointState) -> None:
        positions = self._positions_for(msg, [self.gripper_joint])
        if positions:
            self.latest_gripper_command = positions
        efforts = {
            name: float(msg.effort[index])
            for index, name in enumerate(msg.name)
            if index < len(msg.effort)
        }
        if self.gripper_joint in efforts:
            self.latest_gripper_effort = {
                self.gripper_joint: efforts[self.gripper_joint]
            }

    def spin_until(self, predicate, timeout_s: float) -> bool:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.05)
            if predicate():
                return True
        return predicate()

    def spin_for(self, duration_s: float) -> None:
        deadline = time.monotonic() + max(0.0, float(duration_s))
        while time.monotonic() < deadline:
            rclpy.spin_once(
                self, timeout_sec=min(0.05, max(0.0, deadline - time.monotonic()))
            )

    def require_ready(self) -> None:
        deadline = time.monotonic() + self.args.ready_timeout_s
        missing: list[str] = []
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.1)
            missing = []
            if not self.latest_arm_state:
                missing.append(
                    f"{self.args.arm_joint_state_topic} with {self.arm_joints}"
                )
            if not self.latest_isaac_state:
                missing.append(
                    f"{self.args.isaac_joint_state_topic} with "
                    f"{self.arm_joints + [self.gripper_joint]}"
                )
            if not self.arm_client.wait_for_server(timeout_sec=0.0):
                missing.append(self.args.arm_action_name)
            if (
                self.args.gripper_transport_kind == "ros_gripper_command"
                and self.gripper_client is not None
                and not self.gripper_client.wait_for_server(timeout_sec=0.0)
            ):
                missing.append(self.args.gripper_action_name)
            if not missing:
                return
        raise RuntimeError("readiness failed: " + ", ".join(missing))

    def _trajectory_point(
        self, positions: Sequence[float], duration_s: float
    ) -> JointTrajectoryPoint:
        point = JointTrajectoryPoint()
        point.positions = [float(value) for value in positions]
        point.time_from_start.sec = int(duration_s)
        point.time_from_start.nanosec = int(
            round((duration_s - int(duration_s)) * 1_000_000_000)
        )
        return point

    def send_arm_goal(self, positions: Sequence[float]) -> dict[str, object]:
        goal = FollowJointTrajectory.Goal()
        goal.trajectory.joint_names = list(self.arm_joints)
        goal.trajectory.points = [
            self._trajectory_point(positions, self.args.arm_motion_duration_s)
        ]

        send_future = self.arm_client.send_goal_async(goal)
        rclpy.spin_until_future_complete(
            self, send_future, timeout_sec=self.args.command_timeout_s
        )
        goal_handle = send_future.result()
        if goal_handle is None:
            raise RuntimeError("FollowJointTrajectory goal did not receive a response")
        if not goal_handle.accepted:
            raise RuntimeError("FollowJointTrajectory goal was rejected")

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(
            self, result_future, timeout_sec=self.args.command_timeout_s
        )
        result = result_future.result()
        if result is None:
            raise RuntimeError("FollowJointTrajectory goal did not finish")
        if result.result.error_code != FollowJointTrajectory.Result.SUCCESSFUL:
            raise RuntimeError(
                "FollowJointTrajectory failed: "
                f"{result.result.error_code} {result.result.error_string}"
            )
        rclpy.spin_once(self, timeout_sec=0.05)
        target = {
            joint: float(positions[index]) for index, joint in enumerate(self.arm_joints)
        }
        final_arm = dict(self.latest_arm_state)
        final_isaac = {
            joint: self.latest_isaac_state[joint]
            for joint in self.arm_joints
            if joint in self.latest_isaac_state
        }
        if final_arm:
            max_abs_error = max(
                abs(final_arm[joint] - target[joint]) for joint in self.arm_joints
            )
        else:
            max_abs_error = None
        return {
            "target_positions_rad": target,
            "final_arm_state_rad": final_arm,
            "final_isaac_state_rad": final_isaac,
            "max_abs_target_error_rad": max_abs_error,
        }

    def send_gripper_goal(self, position: float) -> dict[str, object]:
        self.latest_gripper_command = {}
        self.latest_gripper_effort = {}
        if self.args.gripper_transport_kind == "isaac_joint_state_effort":
            return self.send_direct_gripper_command(position)
        if self.gripper_client is None:
            raise RuntimeError("GripperCommand action client is not configured")
        goal = GripperCommand.Goal()
        goal.command.position = float(position)
        goal.command.max_effort = float(self.args.gripper_max_effort)

        send_future = self.gripper_client.send_goal_async(goal)
        rclpy.spin_until_future_complete(
            self, send_future, timeout_sec=self.args.command_timeout_s
        )
        goal_handle = send_future.result()
        if goal_handle is None:
            raise RuntimeError("GripperCommand goal did not receive a response")
        if not goal_handle.accepted:
            raise RuntimeError("GripperCommand goal was rejected")

        saw_raw_command = self.spin_until(
            lambda: self.latest_gripper_command.get(self.gripper_joint) is not None,
            min(1.0, self.args.command_timeout_s),
        )
        if not saw_raw_command:
            raise RuntimeError(
                f"bridge did not publish {self.gripper_joint} on "
                f"{self.args.gripper_command_topic}"
            )

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(
            self, result_future, timeout_sec=self.args.command_timeout_s
        )
        result = result_future.result()
        if result is None:
            raise RuntimeError("GripperCommand goal did not finish")
        if not result.result.reached_goal:
            raise RuntimeError(
                "GripperCommand failed to reach target: "
                f"position={result.result.position} stalled={result.result.stalled}"
            )
        rclpy.spin_once(self, timeout_sec=0.05)
        return {
            "transport_kind": "ros_gripper_command",
            "target_position": float(position),
            "raw_command_position": self.latest_gripper_command.get(
                self.gripper_joint
            ),
            "result_position": float(result.result.position),
            "reached_goal": bool(result.result.reached_goal),
            "stalled": bool(result.result.stalled),
            "isaac_state_position": self.latest_isaac_state.get(self.gripper_joint),
        }

    def send_direct_gripper_command(self, position: float) -> dict[str, object]:
        if self.direct_gripper_pub is None:
            raise RuntimeError("direct gripper publisher is not configured")
        closing = abs(position - self.args.gripper_close_position) <= abs(
            position - self.args.gripper_open_position
        )
        effort = (
            float(self.args.gripper_close_effort)
            if closing
            else float(self.args.gripper_open_effort)
        )
        command = JointState()
        command.header.stamp = self.get_clock().now().to_msg()
        command.name = [self.gripper_joint]
        if self.args.gripper_publish_nominal_position:
            command.position = [float(position)]
        command.effort = [effort]
        self.direct_gripper_pub.publish(command)

        saw_raw_command = self.spin_until(
            lambda: (
                self.latest_gripper_command.get(self.gripper_joint) is not None
                or self.latest_gripper_effort.get(self.gripper_joint) is not None
            ),
            min(1.0, self.args.command_timeout_s),
        )
        if not saw_raw_command:
            raise RuntimeError(
                f"direct gripper command was not observed on "
                f"{self.args.gripper_command_topic}"
            )
        saw_isaac_state = self.spin_until(
            lambda: self.latest_isaac_state.get(self.gripper_joint) is not None,
            min(float(self.args.gripper_dwell_s), self.args.command_timeout_s),
        )
        if not saw_isaac_state:
            raise RuntimeError(
                f"simulator gripper state for {self.gripper_joint} was not observed "
                f"on {self.args.isaac_joint_state_topic}"
            )
        dwell_start = time.monotonic()
        self.spin_for(float(self.args.gripper_dwell_s))
        dwell_elapsed_s = time.monotonic() - dwell_start
        return {
            "transport_kind": "isaac_joint_state_effort",
            "actuation_kind": "pneumatic",
            "state_authority": "sim_observed",
            "completion_policy": "dwell_elapsed",
            "target_position": float(position),
            "raw_command_position": self.latest_gripper_command.get(
                self.gripper_joint
            ),
            "raw_command_effort": self.latest_gripper_effort.get(
                self.gripper_joint
            ),
            "dwell_s": float(self.args.gripper_dwell_s),
            "dwell_elapsed_s": dwell_elapsed_s,
            "isaac_state_position": self.latest_isaac_state.get(self.gripper_joint),
        }

    def run(self) -> SmokeResult:
        self.require_ready()
        start = [self.latest_arm_state[joint] for joint in self.arm_joints]
        probe = list(start)
        probe[0] += float(self.args.arm_delta_rad)

        probe_result = self.send_arm_goal(probe)
        close_result = self.send_gripper_goal(self.args.gripper_close_position)
        open_result = self.send_gripper_goal(self.args.gripper_open_position)
        restore_result = self.send_arm_goal(start)

        return SmokeResult(
            passed=True,
            details={
                "schema": RESULT_SCHEMA,
                "arm_action": self.args.arm_action_name,
                "gripper_action": (
                    self.args.gripper_action_name
                    if self.args.gripper_transport_kind == "ros_gripper_command"
                    else "not_required"
                ),
                "gripper_transport_kind": self.args.gripper_transport_kind,
                "arm_joint_state_topic": self.args.arm_joint_state_topic,
                "isaac_joint_state_topic": self.args.isaac_joint_state_topic,
                "arm_joints": list(self.arm_joints),
                "gripper_joint": self.gripper_joint,
                "start_positions_rad": {
                    joint: float(start[index])
                    for index, joint in enumerate(self.arm_joints)
                },
                "probe_positions_rad": {
                    joint: float(probe[index])
                    for index, joint in enumerate(self.arm_joints)
                },
                "arm_delta_rad": float(self.args.arm_delta_rad),
                "arm_motion_duration_s": float(self.args.arm_motion_duration_s),
                "gripper_open_position": self.args.gripper_open_position,
                "gripper_close_position": self.args.gripper_close_position,
                "gripper_open_effort": self.args.gripper_open_effort,
                "gripper_close_effort": self.args.gripper_close_effort,
                "gripper_dwell_s": self.args.gripper_dwell_s,
                "gripper_command_topic": self.args.gripper_command_topic,
                "arm_probe_result": probe_result,
                "gripper_close_result": close_result,
                "gripper_open_result": open_result,
                "arm_restore_result": restore_result,
            },
        )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--arm-action-name",
        default="/isaac_arm_controller/follow_joint_trajectory",
    )
    parser.add_argument(
        "--gripper-action-name",
        default="/lite_gripper_controller/gripper_cmd",
    )
    parser.add_argument(
        "--gripper-transport-kind",
        choices=("ros_gripper_command", "isaac_joint_state_effort"),
        default="ros_gripper_command",
    )
    parser.add_argument("--arm-joint-state-topic", default="/joint_states")
    parser.add_argument("--isaac-joint-state-topic", default="/isaac_joint_states")
    parser.add_argument(
        "--gripper-command-topic", default="/isaac_joint_commands_gripper"
    )
    parser.add_argument("--arm-joints", nargs="+", default=LITE6_ARM_JOINTS)
    parser.add_argument("--gripper-joint", default=LITE6_GRIPPER_JOINT)
    parser.add_argument("--arm-delta-rad", type=float, default=0.03)
    parser.add_argument("--arm-motion-duration-s", type=float, default=0.8)
    parser.add_argument("--gripper-open-position", type=float, default=0.0081)
    parser.add_argument("--gripper-close-position", type=float, default=0.0)
    parser.add_argument("--gripper-max-effort", type=float, default=10.0)
    parser.add_argument("--gripper-open-effort", type=float, default=5.0)
    parser.add_argument("--gripper-close-effort", type=float, default=-5.0)
    parser.add_argument("--gripper-dwell-s", type=float, default=0.8)
    parser.add_argument(
        "--gripper-publish-nominal-position",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument("--ready-timeout-s", type=float, default=15.0)
    parser.add_argument("--command-timeout-s", type=float, default=10.0)
    parser.add_argument(
        "--output",
        default=None,
        help="Optional path for the JSON result artifact.",
    )
    parser.add_argument("--json", action="store_true")
    return parser.parse_args(argv)


def stamp_payload(payload: dict[str, object], started_at_utc: str) -> dict[str, object]:
    details = payload.setdefault("details", {})
    if isinstance(details, dict):
        details.setdefault("schema", RESULT_SCHEMA)
        details["started_at_utc"] = started_at_utc
        details["ended_at_utc"] = datetime.now(UTC).isoformat()
    return payload


def emit_payload(payload: dict[str, object], args: argparse.Namespace) -> None:
    body = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(body + "\n", encoding="utf-8")
    if args.json:
        print(body)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    started_at_utc = datetime.now(UTC).isoformat()
    rclpy.init()
    node = Lite6SmokeProbe(args)
    try:
        result = node.run()
        payload = stamp_payload(asdict(result), started_at_utc)
        emit_payload(payload, args)
        if not args.json:
            print(
                "Isaac Lite6 smoke passed: small arm move restored, "
                "gripper close/open completed, raw gripper command observed."
            )
        return 0
    except Exception as exc:  # noqa: BLE001 - operator-facing smoke failure
        payload = stamp_payload(
            asdict(
                SmokeResult(
                    passed=False,
                    details={"schema": RESULT_SCHEMA, "error": str(exc)},
                )
            ),
            started_at_utc,
        )
        emit_payload(payload, args)
        if not args.json:
            print(f"Isaac Lite6 smoke failed: {exc}", file=sys.stderr)
        return 1
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
