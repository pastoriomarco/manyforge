"""Logic / blackboard behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import time
from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import (
    read_blackboard_binding_value,
    register_blackboard_binding_read,
)
from manyforge_behavior.kernel_client import (
    GripperCommandRequest,
    GripperCommandState,
    KernelClient,
)


class CheckKeyBoolValue(py_trees.behaviour.Behaviour):
    """Condition: succeeds if blackboard key holds the expected bool value."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._expected = kwargs.get("expected_value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.FAILURE
        if value == self._expected:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class SetKeyBoolValue(py_trees.behaviour.Behaviour):
    """Set a blackboard key to a boolean value. Always succeeds."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._value = kwargs.get("value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key=self._key, access=py_trees.common.Access.WRITE
        )

    def update(self) -> py_trees.common.Status:
        self.blackboard.set(self._key, self._value)
        return py_trees.common.Status.SUCCESS


class WaitForKeyBool(py_trees.behaviour.Behaviour):
    """Block until a blackboard key holds the expected boolean value."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._expected = kwargs.get("expected_value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.RUNNING
        if value == self._expected:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.RUNNING


class CheckSignalBool(CheckKeyBoolValue):
    """Condition: succeeds if a declared boolean signal matches the expected level."""


class SetSignalBool(SetKeyBoolValue):
    """Write a declared boolean signal level and succeed."""


class WaitForSignalBool(WaitForKeyBool):
    """Block until a declared boolean signal reaches the expected level."""


class WaitForSignalEdge(py_trees.behaviour.Behaviour):
    """Block until a declared boolean signal produces the configured edge."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._edge = kwargs.get("edge", "rising")
        self._last_value: bool | None = None

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def initialise(self) -> None:
        self._last_value = None

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.RUNNING

        if not isinstance(value, bool):
            self.feedback_message = (
                f"Signal '{self._key}' must be boolean for edge detection, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE

        if self._last_value is None:
            self._last_value = value
            return py_trees.common.Status.RUNNING

        previous = self._last_value
        self._last_value = value

        if self._edge == "rising":
            matched = (previous is False and value is True)
        elif self._edge == "falling":
            matched = (previous is True and value is False)
        else:
            matched = previous != value

        return (
            py_trees.common.Status.SUCCESS
            if matched
            else py_trees.common.Status.RUNNING
        )


class CheckGripperState(py_trees.behaviour.Behaviour):
    """Condition: succeeds if the declared gripper state matches the expectation."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._expected_state = kwargs.get("expected_state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._state_key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._state_key)
        except KeyError:
            return py_trees.common.Status.FAILURE
        if not isinstance(value, bool):
            self.feedback_message = (
                f"Gripper state key '{self._state_key}' must be boolean, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE
        expected = self._expected_state == "closed"
        return (
            py_trees.common.Status.SUCCESS
            if value == expected
            else py_trees.common.Status.FAILURE
        )


class SetGripperState(py_trees.behaviour.Behaviour):
    """Set the declared logical gripper state and succeed."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._state = kwargs.get("state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key=self._state_key, access=py_trees.common.Access.WRITE
        )

    def update(self) -> py_trees.common.Status:
        self.blackboard.set(self._state_key, self._state == "closed")
        return py_trees.common.Status.SUCCESS


class CommandGripper(py_trees.behaviour.Behaviour):
    """Command an actuated gripper and block until the command completes."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._gripper_id = kwargs.get("gripper_id", "")
        self._command = kwargs.get("command", "close")
        self._state_key = kwargs.get("state_key", "")
        self._timeout_s = float(kwargs.get("timeout_s", 5.0))
        self._started = False
        self._deadline = 0.0

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="gripper_configs", access=py_trees.common.Access.READ
        )
        if self._state_key:
            self.blackboard.register_key(
                key=self._state_key, access=py_trees.common.Access.WRITE
            )

    def initialise(self) -> None:
        self._started = False
        self._deadline = 0.0

    def _load_gripper_config(self) -> dict[str, Any] | None:
        try:
            configs = self.blackboard.get("gripper_configs")
        except (KeyError, AttributeError):
            return None
        if not isinstance(configs, dict):
            return None
        config = configs.get(self._gripper_id)
        if not isinstance(config, dict):
            return None
        return config

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        config = self._load_gripper_config()
        if config is None:
            self.feedback_message = (
                f"Gripper '{self._gripper_id}' is not configured in the deployment"
            )
            return py_trees.common.Status.FAILURE

        command_mode = str(config.get("command_mode", ""))
        if command_mode != "simulated_joint":
            self.feedback_message = (
                f"command_gripper currently supports command_mode=simulated_joint, "
                f"got '{command_mode}'"
            )
            return py_trees.common.Status.FAILURE
        if self._command not in {"open", "close"}:
            self.feedback_message = (
                f"command_gripper.command must be 'open' or 'close', got '{self._command}'"
            )
            return py_trees.common.Status.FAILURE

        target_position = (
            float(config["closed_position"])
            if self._command == "close"
            else float(config["open_position"])
        )

        if not self._started:
            result = kernel.command_gripper(
                GripperCommandRequest(
                    gripper_id=self._gripper_id,
                    group_id=str(config["group_id"]),
                    joint_name=str(config["joint_name"]),
                    target_position=target_position,
                    duration_s=float(config["motion_time_s"]),
                )
            )
            self._started = True
            self._deadline = time.monotonic() + self._timeout_s
            if not result.accepted:
                self.feedback_message = result.error or "Gripper command was rejected"
                return py_trees.common.Status.FAILURE

        status = kernel.get_gripper_status(self._gripper_id)
        if status.state == GripperCommandState.COMPLETED:
            if self._state_key:
                self.blackboard.set(self._state_key, self._command == "close")
            return py_trees.common.Status.SUCCESS
        if status.state == GripperCommandState.FAILED:
            self.feedback_message = status.error or "Gripper command failed"
            return py_trees.common.Status.FAILURE

        now = time.monotonic()
        if now >= self._deadline:
            self.feedback_message = (
                f"Timed out waiting for gripper '{self._gripper_id}' command "
                f"'{self._command}' to complete"
            )
            return py_trees.common.Status.FAILURE

        self.feedback_message = (
            f"progress={status.progress:.2f} "
            f"position={status.current_position if status.current_position is not None else 'n/a'}"
        )
        return py_trees.common.Status.RUNNING


class WaitForGripperState(py_trees.behaviour.Behaviour):
    """Block until the declared logical gripper state matches the expectation."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._expected_state = kwargs.get("expected_state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._state_key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._state_key)
        except KeyError:
            return py_trees.common.Status.RUNNING
        if not isinstance(value, bool):
            self.feedback_message = (
                f"Gripper state key '{self._state_key}' must be boolean, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE
        expected = self._expected_state == "closed"
        return (
            py_trees.common.Status.SUCCESS
            if value == expected
            else py_trees.common.Status.RUNNING
        )
