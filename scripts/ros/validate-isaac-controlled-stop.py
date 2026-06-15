#!/usr/bin/env python3
"""Live validation for the Isaac ManyForge-JTC controlled-stop lane.

This script is intentionally stricter than the lightweight arm-action smoke:
it requires the raw Isaac joint-state publisher, the ManyForge JTC stop
surface, the JTC speed-scaling input, and an open command gate before it
commands motion. By default it runs the two chunk-12 soft_on_path acceptance
probes:

1. `soft_on_path` under stable speed scaling below 1.0.
2. speed-scaling source change during stop, which must transition to hard_stop.

Pass `--cases spring_back_hold` for the chunk-A spring-back probe.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import asdict, dataclass
from typing import Any, Callable, Sequence

import rclpy
from control_msgs.action import FollowJointTrajectory
from control_msgs.msg import SpeedScalingFactor
from manyforge_joint_trajectory_controller.msg import ManyforgeStopComplete
from manyforge_joint_trajectory_controller.srv import ManyforgeStopTrigger
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import JointState
from std_msgs.msg import Bool
from trajectory_msgs.msg import JointTrajectoryPoint


ARM_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]

DEFAULT_CASES = ["stable_scaling_soft_on_path", "scaling_change_hard_stop"]
ALL_CASES = [*DEFAULT_CASES, "spring_back_hold"]


@dataclass
class CaseResult:
    name: str
    passed: bool
    details: dict[str, Any]


class ControlledStopProbe(Node):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__("manyforge_isaac_controlled_stop_validator")
        self.args = args
        self.joints = list(args.required_joints)
        self.latest_joint_state: dict[str, float] = {}
        self.latest_isaac_joint_state: dict[str, float] = {}
        self.latest_gate_closed: bool | None = None
        self.completions: dict[int, ManyforgeStopComplete] = {}

        self.action_client = ActionClient(
            self, FollowJointTrajectory, args.action_name
        )
        self.stop_client = self.create_client(
            ManyforgeStopTrigger, args.stop_trigger_service
        )
        scaling_qos = QoSProfile(
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        )
        self.scaling_pub = self.create_publisher(
            SpeedScalingFactor, args.speed_scaling_topic, scaling_qos
        )
        self.create_subscription(
            JointState, args.joint_state_topic, self._on_joint_state, 10
        )
        self.create_subscription(
            JointState, args.isaac_joint_state_topic, self._on_isaac_joint_state, 10
        )
        self.create_subscription(
            Bool, args.stop_joint_commands_topic, self._on_stop_joint_commands, 10
        )
        qos = QoSProfile(
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        )
        self.create_subscription(
            ManyforgeStopComplete,
            args.stop_complete_topic,
            self._on_stop_complete,
            qos,
        )

    def _positions_by_name(self, msg: JointState) -> dict[str, float]:
        values = dict(zip(msg.name, msg.position, strict=False))
        if all(joint in values for joint in self.joints):
            return {joint: float(values[joint]) for joint in self.joints}
        return {}

    def _on_joint_state(self, msg: JointState) -> None:
        positions = self._positions_by_name(msg)
        if positions:
            self.latest_joint_state = positions

    def _on_isaac_joint_state(self, msg: JointState) -> None:
        positions = self._positions_by_name(msg)
        if positions:
            self.latest_isaac_joint_state = positions

    def _on_stop_joint_commands(self, msg: Bool) -> None:
        self.latest_gate_closed = bool(msg.data)

    def _on_stop_complete(self, msg: ManyforgeStopComplete) -> None:
        self.completions[int(msg.seq)] = msg

    def spin_until(self, predicate: Callable[[], bool], timeout_s: float) -> bool:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.05)
            if predicate():
                return True
        return predicate()

    def spin_for(self, duration_s: float) -> None:
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            rclpy.spin_once(
                self,
                timeout_sec=min(0.05, max(0.0, deadline - time.monotonic())),
            )

    def require_ready(self) -> None:
        checks = [
            (
                "raw Isaac joint-state publisher",
                lambda: self.count_publishers(self.args.isaac_joint_state_topic) > 0,
            ),
            (
                "raw Isaac joint-state sample",
                lambda: bool(self.latest_isaac_joint_state),
            ),
            ("aggregated joint-state sample", lambda: bool(self.latest_joint_state)),
            ("FollowJointTrajectory action", lambda: self.action_client.wait_for_server(0.0)),
            ("ManyForge stop trigger service", lambda: self.stop_client.service_is_ready()),
            (
                "ManyForge stop completion publisher",
                lambda: self.count_publishers(self.args.stop_complete_topic) > 0,
            ),
            (
                "JTC speed-scaling input subscriber",
                lambda: self.count_subscribers(self.args.speed_scaling_topic) > 0,
            ),
            (
                "open Isaac command gate",
                lambda: self.latest_gate_closed is False,
            ),
        ]
        missing: list[str] = []
        deadline = time.monotonic() + self.args.ready_timeout_s
        while time.monotonic() < deadline:
            rclpy.spin_once(self, timeout_sec=0.1)
            missing = [label for label, pred in checks if not pred()]
            if not missing:
                return
        raise RuntimeError("Readiness failed: " + ", ".join(missing))

    def publish_scaling(self, factor: float, duration_s: float) -> None:
        msg = SpeedScalingFactor()
        msg.factor = float(factor)
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            self.scaling_pub.publish(msg)
            rclpy.spin_once(self, timeout_sec=0.02)

    def _point(
        self,
        positions: Sequence[float],
        t_s: float,
        velocities: Sequence[float] | None = None,
    ) -> JointTrajectoryPoint:
        point = JointTrajectoryPoint()
        point.positions = [float(v) for v in positions]
        # Real ManyForge kernel trajectories carry per-joint velocities; mirror
        # that so the controller-native stop samples a complete state. Default
        # to zeros (at-rest waypoints) when an explicit profile is not given.
        point.velocities = [
            float(v) for v in (velocities if velocities is not None else [0.0] * len(positions))
        ]
        point.time_from_start.sec = int(t_s)
        point.time_from_start.nanosec = int((t_s - int(t_s)) * 1_000_000_000)
        return point

    def send_curved_goal(self, seq: int) -> Any:
        start = [self.latest_joint_state[joint] for joint in self.joints]
        scale = float(self.args.motion_scale)
        mid = list(start)
        end = list(start)
        mid[0] += 0.06 * scale
        mid[1] -= 0.04 * scale
        mid[2] += 0.03 * scale
        end[0] += 0.12 * scale
        end[1] -= 0.08 * scale
        end[2] += 0.06 * scale

        goal = FollowJointTrajectory.Goal()
        goal.trajectory.joint_names = list(self.joints)
        goal.trajectory.points = [
            self._point(start, 0.0),
            self._point(mid, self.args.trajectory_duration_s / 2.0),
            self._point(end, self.args.trajectory_duration_s),
        ]

        future = self.action_client.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, future, timeout_sec=self.args.command_timeout_s)
        handle = future.result()
        if handle is None or not handle.accepted:
            raise RuntimeError(f"trajectory goal {seq} was not accepted")
        return handle

    def trigger_stop(self, seq: int, stop_mode: str) -> None:
        request = ManyforgeStopTrigger.Request()
        request.seq = int(seq)
        request.profile_id = f"isaac_live_{stop_mode}_{seq}"
        request.stop_mode = stop_mode
        request.deceleration_time_s = self.args.deceleration_time_s
        request.max_stop_time_s = self.args.max_stop_time_s
        request.allowed_fallback_modes = (
            ["hard_stop"] if stop_mode == "soft_on_path" else []
        )
        request.hard_stop_level = "immediate"
        if stop_mode == "soft_on_path":
            request.limit_joint_names = list(self.joints)
            request.max_deceleration_rad_s2 = [
                self.args.max_deceleration_rad_s2 for _ in self.joints
            ]
            request.max_jerk_rad_s3 = [self.args.max_jerk_rad_s3 for _ in self.joints]
        request.request_timeout_s = self.args.command_timeout_s
        request.allow_natural_completion = False
        future = self.stop_client.call_async(request)
        rclpy.spin_until_future_complete(self, future, timeout_sec=self.args.command_timeout_s)
        response = future.result()
        if response is None:
            raise RuntimeError(f"stop trigger {seq} did not return a response")
        if not response.accepted:
            raise RuntimeError(
                f"stop trigger {seq} rejected: {response.reason} {response.message}"
            )

    def wait_completion(self, seq: int) -> ManyforgeStopComplete:
        if not self.spin_until(
            lambda: seq in self.completions, self.args.completion_timeout_s
        ):
            raise RuntimeError(f"stop completion {seq} was not published")
        return self.completions[seq]

    def run_soft_on_path_case(
        self, name: str, seq: int, change_scaling: bool
    ) -> CaseResult:
        self.publish_scaling(self.args.low_scaling_factor, 0.4)
        handle = self.send_curved_goal(seq)
        self.spin_for(self.args.pre_stop_delay_s)
        self.trigger_stop(seq, "soft_on_path")
        if change_scaling:
            self.spin_for(self.args.post_stop_scaling_change_delay_s)
            self.publish_scaling(self.args.changed_scaling_factor, 0.3)
        completion = self.wait_completion(seq)
        result_future = handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=1.0)

        details = message_to_dict(completion)
        if change_scaling:
            expected = (
                completion.success
                and completion.backend_stop_confirmed
                and completion.measured_stop_confirmed
                and completion.scaling_source_changed
                and completion.scaling_change_hard_stop
                and completion.hard_stop_transitioned
                and completion.executed_stop_mode == "hard_stop"
            )
        else:
            expected = (
                completion.success
                and not completion.backend_stop_confirmed
                and not completion.measured_stop_confirmed
                and not completion.scaling_source_changed
                and not completion.hard_stop_transitioned
                and completion.executed_stop_mode == "soft_on_path"
                and completion.reason == "soft_on_path_terminal_hold_installed"
                and completion.terminal_hold_result == "terminal_hold_installed"
                and len(completion.terminal_joint_names) == len(self.joints)
                and len(completion.terminal_positions) == len(self.joints)
                and completion.path_deviation_max_rad
                <= self.args.max_path_deviation_rad
                and math.isclose(
                    completion.scaling_at_trip,
                    self.args.low_scaling_factor,
                    abs_tol=self.args.scaling_tolerance,
                )
            )
        return CaseResult(name=name, passed=bool(expected), details=details)

    def run_spring_back_case(self, seq: int) -> CaseResult:
        self.publish_scaling(self.args.low_scaling_factor, 0.4)
        handle = self.send_curved_goal(seq)
        self.spin_for(self.args.pre_stop_delay_s)
        self.trigger_stop(seq, "spring_back_hold")
        completion = self.wait_completion(seq)
        result_future = handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=1.0)

        details = message_to_dict(completion)
        expected = (
            completion.success
            and not completion.backend_stop_confirmed
            and not completion.measured_stop_confirmed
            and completion.requested_stop_mode == "spring_back_hold"
            and completion.executed_stop_mode == "spring_back_hold"
            and completion.command_source == "controller_native_spring_back_hold"
            and completion.reason == "spring_back_hold_terminal_hold_installed"
            and completion.terminal_hold_result == "terminal_hold_installed"
            and len(completion.terminal_joint_names) == len(self.joints)
            and len(completion.terminal_positions) == len(self.joints)
            and not completion.hard_stop_transitioned
            and completion.spring_back_distance_rad
            >= self.args.min_spring_back_distance_rad
            and math.isclose(
                completion.scaling_at_trip,
                self.args.low_scaling_factor,
                abs_tol=self.args.scaling_tolerance,
            )
        )
        return CaseResult(
            name="spring_back_hold",
            passed=bool(expected),
            details=details,
        )


def message_to_dict(msg: ManyforgeStopComplete) -> dict[str, Any]:
    return {
        "seq": int(msg.seq),
        "accepted": bool(msg.accepted),
        "success": bool(msg.success),
        "backend_stop_confirmed": bool(msg.backend_stop_confirmed),
        "requested_stop_mode": msg.requested_stop_mode,
        "executed_stop_mode": msg.executed_stop_mode,
        "command_source": msg.command_source,
        "reason": msg.reason,
        "message": msg.message,
        "fallback_reason": msg.fallback_reason,
        "scaling_at_trip": float(msg.scaling_at_trip),
        "scaling_min_seen": float(msg.scaling_min_seen),
        "scaling_max_seen": float(msg.scaling_max_seen),
        "scaling_source_id": msg.scaling_source_id,
        "scaling_source_changed": bool(msg.scaling_source_changed),
        "scaling_change_hard_stop": bool(msg.scaling_change_hard_stop),
        "hard_stop_level": msg.hard_stop_level,
        "natural_completion_result": msg.natural_completion_result,
        "terminal_hold_result": msg.terminal_hold_result,
        "terminal_joint_names": list(msg.terminal_joint_names),
        "terminal_positions": [float(value) for value in msg.terminal_positions],
        "measured_stop_confirmed": bool(msg.measured_stop_confirmed),
        "measured_velocity_max_rad_s": float(msg.measured_velocity_max_rad_s),
        "stopped_velocity_tolerance_rad_s": float(
            msg.stopped_velocity_tolerance_rad_s
        ),
        "latch_to_stop_latency_ns": int(msg.latch_to_stop_latency_ns),
        "confirmation_latency_ns": int(msg.confirmation_latency_ns),
        "path_deviation_max_rad": float(msg.path_deviation_max_rad),
        "spring_back_distance_rad": float(msg.spring_back_distance_rad),
        "completed_by_natural_completion": bool(msg.completed_by_natural_completion),
        "hard_stop_transitioned": bool(msg.hard_stop_transitioned),
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--action-name", default="/isaac_arm_controller/follow_joint_trajectory")
    parser.add_argument("--joint-state-topic", default="/joint_states")
    parser.add_argument("--isaac-joint-state-topic", default="/isaac_joint_states")
    parser.add_argument("--stop-trigger-service", default="/isaac_arm_controller/manyforge_stop_trigger")
    parser.add_argument("--stop-complete-topic", default="/isaac_arm_controller/manyforge_stop_complete")
    parser.add_argument("--speed-scaling-topic", default="/isaac_arm_controller/speed_scaling_input")
    parser.add_argument("--stop-joint-commands-topic", default="/stop_joint_commands")
    parser.add_argument("--required-joints", nargs="+", default=ARM_JOINTS)
    parser.add_argument("--ready-timeout-s", type=float, default=20.0)
    parser.add_argument("--command-timeout-s", type=float, default=5.0)
    parser.add_argument("--completion-timeout-s", type=float, default=3.0)
    parser.add_argument("--trajectory-duration-s", type=float, default=3.0)
    parser.add_argument("--pre-stop-delay-s", type=float, default=0.45)
    parser.add_argument("--post-stop-scaling-change-delay-s", type=float, default=0.08)
    parser.add_argument("--deceleration-time-s", type=float, default=1.0)
    parser.add_argument("--max-stop-time-s", type=float, default=1.5)
    parser.add_argument("--low-scaling-factor", type=float, default=0.5)
    parser.add_argument("--changed-scaling-factor", type=float, default=1.0)
    parser.add_argument("--motion-scale", type=float, default=1.0)
    parser.add_argument("--seq-base", type=int, default=None)
    parser.add_argument("--cases", nargs="+", choices=ALL_CASES, default=DEFAULT_CASES)
    parser.add_argument("--scaling-tolerance", type=float, default=0.08)
    parser.add_argument("--max-path-deviation-rad", type=float, default=0.03)
    parser.add_argument("--min-spring-back-distance-rad", type=float, default=0.001)
    parser.add_argument("--max-deceleration-rad-s2", type=float, default=10.0)
    parser.add_argument("--max-jerk-rad-s3", type=float, default=50.0)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    rclpy.init()
    node = ControlledStopProbe(args)
    results: list[CaseResult] = []
    try:
        node.require_ready()
        seq_base = (
            int(args.seq_base)
            if args.seq_base is not None
            else time.time_ns() // 1_000_000
        )
        seq = seq_base
        for case in args.cases:
            if case == "stable_scaling_soft_on_path":
                results.append(node.run_soft_on_path_case(case, seq, False))
            elif case == "scaling_change_hard_stop":
                results.append(node.run_soft_on_path_case(case, seq, True))
            elif case == "spring_back_hold":
                results.append(node.run_spring_back_case(seq))
            seq += 1
        passed = all(result.passed for result in results)
        print(json.dumps({"passed": passed, "cases": [asdict(r) for r in results]}, indent=2))
        return 0 if passed else 1
    except Exception as exc:
        print(json.dumps({"passed": False, "error": str(exc), "cases": [asdict(r) for r in results]}, indent=2), file=sys.stderr)
        return 1
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
