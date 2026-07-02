"""Tests for Isaac gripper bridge helper logic."""

from __future__ import annotations

import pytest

from manyforge_ros_bringup.isaac_gripper_command import (
    map_gripper_positions,
    select_pneumatic_effort,
)


def test_select_pneumatic_effort_uses_close_effort_for_closed_target() -> None:
    assert select_pneumatic_effort(
        target_position=0.0,
        open_position=0.0081,
        closed_position=0.0,
        open_effort=5.0,
        close_effort=-5.0,
    ) == pytest.approx(-5.0)


def test_select_pneumatic_effort_uses_open_effort_for_open_target() -> None:
    assert select_pneumatic_effort(
        target_position=0.0081,
        open_position=0.0081,
        closed_position=0.0,
        open_effort=5.0,
        close_effort=-5.0,
    ) == pytest.approx(5.0)


def test_map_gripper_positions_preserves_nominal_joint_target() -> None:
    assert map_gripper_positions(
        command_position=0.0081,
        multipliers=[1.0],
        offsets=[0.0],
    ) == pytest.approx([0.0081])
