"""Logic / blackboard behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any

import py_trees


class CheckKeyBoolValue(py_trees.behaviour.Behaviour):
    """Condition: succeeds if blackboard key holds the expected bool value."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._expected = kwargs.get("expected_value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key=self._key, access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        try:
            value = self.blackboard.get(self._key)
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
        self.blackboard.register_key(
            key=self._key, access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        try:
            value = self.blackboard.get(self._key)
        except KeyError:
            return py_trees.common.Status.RUNNING
        if value == self._expected:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.RUNNING
