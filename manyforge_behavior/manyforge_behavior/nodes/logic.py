"""Logic / blackboard behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import (
    read_blackboard_binding_value,
    register_blackboard_binding_read,
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
