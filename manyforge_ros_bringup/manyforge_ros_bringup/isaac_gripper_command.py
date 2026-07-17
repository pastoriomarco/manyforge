"""ROS-free helper logic for Isaac gripper JointState commands."""

from __future__ import annotations

import math
import threading
from dataclasses import dataclass
from typing import Protocol, Sequence


class ReadinessLogger(Protocol):
    def info(self, message: str) -> object: ...

    def warning(self, message: str) -> object: ...


def log_readiness_transition(
    logger: ReadinessLogger,
    *,
    ready: bool,
    previous: bool | None,
    required_joint_names: Sequence[str],
) -> None:
    """Log readiness transitions at severity-stable rclpy call sites."""

    detail = "expected finite state for %s" % ", ".join(required_joint_names)
    # rclpy keys logging call state by source location and rejects a severity
    # change at one call site, even when the method was selected dynamically.
    if previous is None:
        state = "ready" if ready else "not ready"
        logger.info(f"Isaac gripper state contract {state}: {detail}")
    elif ready:
        logger.info(f"Isaac gripper state contract ready: {detail}")
    else:
        logger.warning(f"Isaac gripper state contract not ready: {detail}")


def coherent_joint_positions(
    joint_names: Sequence[str], joint_positions: Sequence[float]
) -> dict[str, float] | None:
    """Normalize one unambiguous, finite JointState position sample."""

    if len(joint_names) != len(set(joint_names)):
        return None
    if len(joint_positions) != len(joint_names):
        return None
    positions = {
        name: float(joint_positions[index])
        for index, name in enumerate(joint_names)
    }
    return (
        positions
        if all(finite_float(value) for value in positions.values())
        else None
    )


class SingleGoalReservation:
    """Serialize action goals before they can mutate a shared command target."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._reserved = False

    def try_acquire(self) -> bool:
        with self._lock:
            if self._reserved:
                return False
            self._reserved = True
            return True

    def release(self) -> None:
        with self._lock:
            self._reserved = False


def map_gripper_positions(
    command_position: float,
    multipliers: Sequence[float],
    offsets: Sequence[float],
) -> list[float]:
    """Map a single command joint into Isaac's command vector."""

    if len(multipliers) != len(offsets):
        raise ValueError("multipliers and offsets must have the same length")
    if not finite_float(command_position):
        raise ValueError("command_position must be finite")
    if not all(finite_float(value) for value in (*multipliers, *offsets)):
        raise ValueError("multipliers and offsets must be finite")
    return [
        float(command_position) * float(multiplier) + float(offset)
        for multiplier, offset in zip(multipliers, offsets)
    ]


def joint_positions_match(
    observed_positions: dict[str, float],
    joint_names: Sequence[str],
    target_positions: Sequence[float],
    tolerance: float,
) -> bool:
    """Return whether one coherent observation matches every mapped target."""

    if len(joint_names) != len(target_positions):
        raise ValueError("joint_names and target_positions must have the same length")
    if not finite_float(tolerance) or tolerance < 0.0:
        raise ValueError("tolerance must be finite and >= 0")
    return all(
        name in observed_positions
        and finite_float(observed_positions[name])
        and finite_float(target)
        and abs(float(observed_positions[name]) - float(target)) <= tolerance
        for name, target in zip(joint_names, target_positions)
    )


def joint_state_contract_ready(
    observed_positions: dict[str, float],
    required_joint_names: Sequence[str],
) -> bool:
    """Return whether one observation contains finite state for every joint."""

    return bool(required_joint_names) and all(
        name in observed_positions and finite_float(observed_positions[name])
        for name in required_joint_names
    )


@dataclass
class SettledCompletionWindow:
    """Require matching, distinct feedback samples over a bounded settle window."""

    settle_time_s: float
    min_samples: int
    matching_since: float | None = None
    matching_samples: int = 0
    last_generation: int = -1

    def __post_init__(self) -> None:
        if not finite_float(self.settle_time_s) or self.settle_time_s < 0.0:
            raise ValueError("settle_time_s must be finite and >= 0")
        if self.min_samples < 1:
            raise ValueError("min_samples must be >= 1")

    def observe(self, *, now: float, generation: int, matches: bool) -> bool:
        if not finite_float(now):
            raise ValueError("now must be finite")
        if not matches:
            self.matching_since = None
            self.matching_samples = 0
            self.last_generation = max(self.last_generation, generation)
            return False

        # Generations are monotonic state-sample identities. Ignore duplicate or
        # out-of-order observations so one sample can never satisfy the count.
        if generation > self.last_generation:
            if self.matching_since is None:
                self.matching_since = now
            self.matching_samples += 1
            self.last_generation = generation

        return (
            self.matching_since is not None
            and self.matching_samples >= self.min_samples
            and now - self.matching_since >= self.settle_time_s
        )


def select_pneumatic_effort(
    target_position: float,
    open_position: float,
    closed_position: float,
    open_effort: float,
    close_effort: float,
) -> float:
    """Select open/close effort from the nominal semantic target position."""

    target = float(target_position)
    open_distance = abs(target - float(open_position))
    closed_distance = abs(target - float(closed_position))
    return float(close_effort) if closed_distance <= open_distance else float(open_effort)


def finite_float(value: float) -> bool:
    """Return whether value can be represented as a finite float."""

    return math.isfinite(float(value))
