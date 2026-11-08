#!/usr/bin/env python3
"""Isaac/ros2_control feedback reducers and unavailable live-measurement gate.

This is a diagnostic script for the ManyForge Isaac preview lane.  It samples
the same ROS surfaces the C++ ROS execution adapter uses:

* /isaac_arm_controller/controller_state: preferred execution feedback
* /joint_states: ros2_control aggregated observed state
* /isaac_joint_states: raw Isaac hardware-input topic for diagnostics

The owner-neutral cadence and tracking reducers remain available to the future
focused adapter.  The former ROS subscriber is intentionally absent; its exact
implementation is preserved at ManyForge commit
08222188293df8e12f90dc72e204c21f22f92ea1, blob
d5166babcd9960cb8bb340915db435131419d622.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from dataclasses import dataclass
from numbers import Real
from typing import Any, Callable, Sequence


DEFAULT_REQUIRED_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]
UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: live Isaac controller feedback "
    "measurement requires the focused ROS/Isaac adapter, not available yet"
)
MAX_TOPIC_SAMPLES = 1_000_000
MAX_JOINTS = 256
MAX_JOINT_NAME_BYTES = 256
MAX_TOPIC_NAME_BYTES = 1_024


def _finite_real(value: object, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise TypeError(f"{label} must be a real number, not bool")
    try:
        result = float(value)
    except (OverflowError, ValueError) as exc:
        raise ValueError(f"{label} must be finite") from exc
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _bounded_text(value: object, *, label: str, limit: int) -> str:
    if type(value) is not str or not value:
        raise TypeError(f"{label} must be an exact non-empty string")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} must be valid UTF-8 text") from exc
    if len(encoded) > limit:
        raise ValueError(f"{label} exceeds the {limit}-byte text bound")
    return value


def _bounded_sequence(value: object, *, label: str) -> Sequence[Any]:
    if isinstance(value, (str, bytes, bytearray)) or not isinstance(value, Sequence):
        raise TypeError(f"{label} must be a sequence")
    if len(value) > MAX_JOINTS:
        raise ValueError(f"{label} exceeds the {MAX_JOINTS}-item bound")
    return value


def _joint_names(value: object, *, label: str) -> list[str]:
    names = _bounded_sequence(value, label=label)
    checked: list[str] = []
    seen: set[str] = set()
    for index, name in enumerate(names):
        checked_name = _bounded_text(
            name,
            label=f"{label}[{index}]",
            limit=MAX_JOINT_NAME_BYTES,
        )
        if checked_name in seen:
            raise ValueError(f"{label} contains duplicate joint {checked_name!r}")
        checked.append(checked_name)
        seen.add(checked_name)
    return checked


def _finite_positions(value: object, *, label: str) -> list[float]:
    positions = _bounded_sequence(value, label=label)
    return [
        _finite_real(item, label=f"{label}[{index}]")
        for index, item in enumerate(positions)
    ]


@dataclass
class TopicStats:
    count: int = 0
    first_t: float | None = None
    last_t: float | None = None
    interval_count: int = 0
    interval_sum_s: float = 0.0
    interval_min_s: float | None = None
    interval_max_s: float | None = None

    def observe(self, now: Real) -> None:
        now_value = _finite_real(now, label="topic observation clock")
        if now_value < 0.0:
            raise ValueError("topic observation clock must be non-negative")
        if self.count >= MAX_TOPIC_SAMPLES:
            raise ValueError(
                f"topic observations exceed the {MAX_TOPIC_SAMPLES}-sample bound"
            )
        if self.last_t is not None:
            if now_value < self.last_t:
                raise ValueError("topic observation clock must be monotonic")
            interval = now_value - self.last_t
            interval_sum = self.interval_sum_s + interval
            if not math.isfinite(interval_sum):
                raise ValueError("topic interval sum must remain finite")
            self.interval_count += 1
            self.interval_sum_s = interval_sum
            self.interval_min_s = (
                interval
                if self.interval_min_s is None
                else min(self.interval_min_s, interval)
            )
            self.interval_max_s = (
                interval
                if self.interval_max_s is None
                else max(self.interval_max_s, interval)
            )
        if self.first_t is None:
            self.first_t = now_value
        self.last_t = now_value
        self.count += 1

    def summary(self) -> dict[str, Any]:
        duration = (
            self.last_t - self.first_t
            if self.first_t is not None and self.last_t is not None
            else 0.0
        )
        hz = self.interval_count / duration if duration > 0.0 else 0.0
        result: dict[str, Any] = {
            "count": self.count,
            "hz": hz,
            "duration_s": duration,
        }
        if self.interval_count:
            if self.interval_min_s is None or self.interval_max_s is None:
                raise ValueError("topic interval state is incomplete")
            result.update(
                {
                    "interval_min_s": self.interval_min_s,
                    "interval_mean_s": self.interval_sum_s / self.interval_count,
                    "interval_max_s": self.interval_max_s,
                }
            )
        return result


def _positions_by_name(names: object, positions: object) -> dict[str, float]:
    checked_names = _joint_names(names, label="joint names")
    checked_positions = _finite_positions(positions, label="joint positions")
    if len(checked_names) != len(checked_positions):
        raise ValueError("joint names and positions must have equal length")
    return dict(zip(checked_names, checked_positions, strict=True))


def _point_positions(point: Any) -> list[float]:
    return _finite_positions(
        getattr(point, "positions", []),
        label="controller point positions",
    )


def _controller_actual_point(msg: Any) -> Any:
    feedback = getattr(msg, "feedback", None)
    return feedback if feedback is not None else getattr(msg, "actual", None)


def _controller_error_point(msg: Any) -> Any:
    return getattr(msg, "error", None)


class FeedbackEvidenceAccumulator:
    """ROS-message-shaped feedback reduction without ROS ownership or I/O."""

    def __init__(
        self,
        args: argparse.Namespace,
        *,
        now: Callable[[], float] = time.monotonic,
    ) -> None:
        if not callable(now):
            raise TypeError("now must be callable")
        required_joints = _joint_names(
            args.required_joints,
            label="required joints",
        )
        if not required_joints:
            raise ValueError("required joints must be non-empty")
        controller_state_topic = _bounded_text(
            args.controller_state_topic,
            label="controller state topic",
            limit=MAX_TOPIC_NAME_BYTES,
        )
        joint_states_topic = _bounded_text(
            args.joint_states_topic,
            label="joint states topic",
            limit=MAX_TOPIC_NAME_BYTES,
        )
        isaac_joint_states_topic = _bounded_text(
            args.isaac_joint_states_topic,
            label="Isaac joint states topic",
            limit=MAX_TOPIC_NAME_BYTES,
        )
        topics = (
            controller_state_topic,
            joint_states_topic,
            isaac_joint_states_topic,
        )
        if len(set(topics)) != len(topics):
            raise ValueError("feedback topic names must be distinct")

        self._now = now
        self.started_at = _finite_real(self._now(), label="monotonic clock")
        if self.started_at < 0.0:
            raise ValueError("monotonic clock must be non-negative")
        self._last_now = self.started_at
        self.required_joints = required_joints
        self.controller_state_topic = controller_state_topic
        self.joint_states_topic = joint_states_topic
        self.isaac_joint_states_topic = isaac_joint_states_topic
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

    def _sample_now(self) -> float:
        now = _finite_real(self._now(), label="monotonic clock")
        if now < self._last_now:
            raise ValueError("monotonic clock regressed")
        self._last_now = now
        return now

    def _missing_required(self, positions: dict[str, float]) -> bool:
        return any(joint not in positions for joint in self.required_joints)

    def observe_controller_state(self, msg: Any) -> None:
        actual_point = _controller_actual_point(msg)
        error_point = _controller_error_point(msg)
        checked_names: list[str] = []
        if actual_point is not None or error_point is not None:
            checked_names = _joint_names(
                getattr(msg, "joint_names", []),
                label="controller joint names",
            )

        controller_positions: dict[str, float] | None = None
        if actual_point is not None:
            actual_positions = _point_positions(actual_point)
            controller_positions = _positions_by_name(
                checked_names,
                actual_positions,
            )

        maximum_error = self.max_abs_feedback_error_rad
        maximum_error_joint = self.max_abs_feedback_error_joint
        if error_point is not None:
            errors = _point_positions(error_point)
            if len(errors) != len(checked_names):
                raise ValueError(
                    "controller joint names and error positions must have equal length"
                )
            for name, error in zip(checked_names, errors, strict=True):
                abs_error = abs(error)
                if abs_error > maximum_error:
                    maximum_error = abs_error
                    maximum_error_joint = name

        self.controller_stats.observe(self._sample_now())
        if controller_positions is not None:
            self.latest_controller_positions = controller_positions
            if self._missing_required(controller_positions):
                self.missing_required_samples["controller_state"] += 1
        self.max_abs_feedback_error_rad = maximum_error
        self.max_abs_feedback_error_joint = maximum_error_joint

    def observe_joint_state(self, msg: Any) -> None:
        positions = _positions_by_name(
            getattr(msg, "name", []),
            getattr(msg, "position", []),
        )
        self.joint_state_stats.observe(self._sample_now())
        self.latest_joint_state_positions = positions
        if self._missing_required(positions):
            self.missing_required_samples["joint_states"] += 1

    def observe_isaac_joint_state(self, msg: Any) -> None:
        positions = _positions_by_name(
            getattr(msg, "name", []),
            getattr(msg, "position", []),
        )
        self.isaac_joint_state_stats.observe(self._sample_now())
        self.latest_isaac_joint_state_positions = positions
        if self._missing_required(positions):
            self.missing_required_samples["isaac_joint_states"] += 1

    def result(self) -> dict[str, Any]:
        return {
            "duration_s": self._sample_now() - self.started_at,
            "topics": {
                self.controller_state_topic: self.controller_stats.summary(),
                self.joint_states_topic: self.joint_state_stats.summary(),
                self.isaac_joint_states_topic: self.isaac_joint_state_stats.summary(),
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


def build_argument_parser() -> argparse.ArgumentParser:
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
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
