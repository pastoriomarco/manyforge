#!/usr/bin/env python3
"""Measure Isaac/ros2_control state cadence and controller tracking error.

This is a diagnostic script for the ManyForge Isaac preview lane.  It samples
the same ROS surfaces the C++ ROS execution adapter uses:

* /isaac_arm_controller/controller_state: preferred execution feedback
* /joint_states: ros2_control aggregated observed state
* /isaac_joint_states: raw Isaac hardware-input topic for diagnostics

It intentionally does not command motion.  Run it while a trajectory is
executing to measure controller tracking error, or while idle to measure state
cadence/freshness.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import time
from dataclasses import dataclass, field
from typing import Any

import rclpy
from control_msgs.msg import JointTrajectoryControllerState
from rclpy.node import Node
from sensor_msgs.msg import JointState


DEFAULT_REQUIRED_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]


@dataclass
class TopicStats:
    count: int = 0
    first_t: float | None = None
    last_t: float | None = None
    intervals_s: list[float] = field(default_factory=list)

    def observe(self, now: float) -> None:
        if self.last_t is not None:
            self.intervals_s.append(now - self.last_t)
        if self.first_t is None:
            self.first_t = now
        self.last_t = now
        self.count += 1

    def summary(self) -> dict[str, Any]:
        duration = (self.last_t - self.first_t) if self.first_t is not None and self.last_t is not None else 0.0
        hz = (self.count - 1) / duration if self.count > 1 and duration > 0.0 else 0.0
        result: dict[str, Any] = {
            "count": self.count,
            "hz": hz,
            "duration_s": duration,
        }
        if self.intervals_s:
            result.update(
                {
                    "interval_min_s": min(self.intervals_s),
                    "interval_mean_s": statistics.fmean(self.intervals_s),
                    "interval_max_s": max(self.intervals_s),
                }
            )
        return result


def _positions_by_name(names: list[str], positions: list[float]) -> dict[str, float]:
    return {name: float(positions[i]) for i, name in enumerate(names) if i < len(positions)}


def _point_positions(point: Any) -> list[float]:
    return [float(value) for value in getattr(point, "positions", [])]


def _controller_actual_point(msg: JointTrajectoryControllerState) -> Any:
    return getattr(msg, "feedback", None) or getattr(msg, "actual", None)


def _controller_error_point(msg: JointTrajectoryControllerState) -> Any:
    return getattr(msg, "error", None)


class FeedbackProbe(Node):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__("manyforge_isaac_controller_feedback_probe")
        self.args = args
        self.started_at = time.monotonic()
        self.required_joints = list(args.required_joints)
        self.controller_stats = TopicStats()
        self.joint_state_stats = TopicStats()
        self.isaac_joint_state_stats = TopicStats()
        self.max_abs_feedback_error_rad = 0.0
        self.max_abs_feedback_error_joint: str | None = None
        self.latest_controller_positions: dict[str, float] = {}
        self.latest_joint_state_positions: dict[str, float] = {}
        self.latest_isaac_joint_state_positions: dict[str, float] = {}
        self.missing_required_samples = {
            "controller_state": 0,
            "joint_states": 0,
            "isaac_joint_states": 0,
        }

        self.create_subscription(
            JointTrajectoryControllerState,
            args.controller_state_topic,
            self._on_controller_state,
            10,
        )
        self.create_subscription(JointState, args.joint_states_topic, self._on_joint_state, 10)
        self.create_subscription(
            JointState,
            args.isaac_joint_states_topic,
            self._on_isaac_joint_state,
            10,
        )

    def _missing_required(self, positions: dict[str, float]) -> bool:
        return any(joint not in positions for joint in self.required_joints)

    def _on_controller_state(self, msg: JointTrajectoryControllerState) -> None:
        self.controller_stats.observe(time.monotonic())
        actual_point = _controller_actual_point(msg)
        if actual_point is not None:
            self.latest_controller_positions = _positions_by_name(
                list(msg.joint_names),
                _point_positions(actual_point),
            )
            if self._missing_required(self.latest_controller_positions):
                self.missing_required_samples["controller_state"] += 1

        error_point = _controller_error_point(msg)
        if error_point is None:
            return
        for index, error in enumerate(_point_positions(error_point)):
            if not math.isfinite(error):
                continue
            abs_error = abs(error)
            if abs_error > self.max_abs_feedback_error_rad:
                self.max_abs_feedback_error_rad = abs_error
                if index < len(msg.joint_names):
                    self.max_abs_feedback_error_joint = msg.joint_names[index]
                else:
                    self.max_abs_feedback_error_joint = f"index_{index}"

    def _on_joint_state(self, msg: JointState) -> None:
        self.joint_state_stats.observe(time.monotonic())
        self.latest_joint_state_positions = _positions_by_name(list(msg.name), list(msg.position))
        if self._missing_required(self.latest_joint_state_positions):
            self.missing_required_samples["joint_states"] += 1

    def _on_isaac_joint_state(self, msg: JointState) -> None:
        self.isaac_joint_state_stats.observe(time.monotonic())
        self.latest_isaac_joint_state_positions = _positions_by_name(list(msg.name), list(msg.position))
        if self._missing_required(self.latest_isaac_joint_state_positions):
            self.missing_required_samples["isaac_joint_states"] += 1

    def result(self) -> dict[str, Any]:
        return {
            "duration_s": time.monotonic() - self.started_at,
            "topics": {
                self.args.controller_state_topic: self.controller_stats.summary(),
                self.args.joint_states_topic: self.joint_state_stats.summary(),
                self.args.isaac_joint_states_topic: self.isaac_joint_state_stats.summary(),
            },
            "controller_tracking": {
                "max_abs_feedback_error_rad": self.max_abs_feedback_error_rad,
                "max_abs_feedback_error_joint": self.max_abs_feedback_error_joint,
            },
            "missing_required_samples": self.missing_required_samples,
            "latest_positions": {
                "controller_state": self.latest_controller_positions,
                "joint_states": self.latest_joint_state_positions,
                "isaac_joint_states": self.latest_isaac_joint_state_positions,
            },
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument(
        "--controller-state-topic",
        default="/isaac_arm_controller/controller_state",
    )
    parser.add_argument("--joint-states-topic", default="/joint_states")
    parser.add_argument("--isaac-joint-states-topic", default="/isaac_joint_states")
    parser.add_argument(
        "--required-joints",
        nargs="+",
        default=DEFAULT_REQUIRED_JOINTS,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rclpy.init()
    node = FeedbackProbe(args)
    deadline = time.monotonic() + args.duration
    try:
        while rclpy.ok() and time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.05)
    finally:
        print(json.dumps(node.result(), indent=2, sort_keys=True))
        node.destroy_node()
        rclpy.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
