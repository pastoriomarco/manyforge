"""ROS-free helper logic for Isaac gripper JointState commands."""

from __future__ import annotations

import math
from typing import Sequence


def map_gripper_positions(
    command_position: float,
    multipliers: Sequence[float],
    offsets: Sequence[float],
) -> list[float]:
    """Map a single command joint into Isaac's command vector."""

    if len(multipliers) != len(offsets):
        raise ValueError("multipliers and offsets must have the same length")
    return [
        float(command_position) * float(multiplier) + float(offset)
        for multiplier, offset in zip(multipliers, offsets)
    ]


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
