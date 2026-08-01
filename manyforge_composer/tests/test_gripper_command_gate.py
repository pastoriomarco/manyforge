"""Fail-before-state tests for the I6/N gripper-command client gate."""

import pytest

from manyforge_composer.backend import gripper_command
from manyforge_composer.backend.gripper_command import (
    GRIPPER_COMMAND_RESTORATION_GATES,
    GripperCommandClient,
    GripperCommandClientUnavailableError,
    require_gripper_command_client,
)


class _PoisonState:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"gripper gate accessed former state: {name}")


def test_client_construction_fails_before_state_or_backend_access() -> None:
    poison = _PoisonState()
    with pytest.raises(
        GripperCommandClientUnavailableError,
        match="composable_cell_gripper_command_client_unavailable",
    ):
        GripperCommandClient(poison, kernel=poison, deployment=poison)


def test_gate_fails_before_command_payload_inspection() -> None:
    poison = _PoisonState()
    with pytest.raises(GripperCommandClientUnavailableError):
        require_gripper_command_client(poison, target=poison)

    assert GRIPPER_COMMAND_RESTORATION_GATES == ("I6", "N", "I11", "T")


def test_singleton_command_and_admission_helpers_are_absent() -> None:
    assert not hasattr(gripper_command, "validate_gripper_command")
    assert not hasattr(gripper_command, "gripper_joint_limits")
    assert not hasattr(gripper_command, "gripper_cycle_busy_detail")
    assert not hasattr(gripper_command, "gripper_busy_detail")
