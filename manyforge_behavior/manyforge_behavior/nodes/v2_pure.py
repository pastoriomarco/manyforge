"""Pure v2 py_trees adapters over Program-run orchestration values."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import py_trees

from manyforge_behavior.nodes.geometry import (
    quaternion_angular_distance_rad,
    translation_distance_m,
)
from manyforge_behavior.nodes.logic import boolean_condition_matches
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueError,
)


class _SnapshotAdapter(py_trees.behaviour.Behaviour):
    """Capture one immutable parameter snapshot at node-attempt start."""

    node_id: str

    def __init__(
        self,
        *,
        name: str,
        parameters: Mapping[str, Any],
        value_sources: Mapping[str, Mapping[str, Any]],
        input_resolver: NodeAttemptInputResolver,
    ) -> None:
        super().__init__(name=name)
        self._node_instance_id = name
        self._parameters = dict(parameters)
        self._value_sources = {
            key: dict(source) for key, source in value_sources.items()
        }
        self._input_resolver = input_resolver
        self._inputs: Mapping[str, Any] | None = None
        self._blackboard_inputs: Mapping[str, Any] | None = None
        self._input_error: str | None = None

    def initialise(self) -> None:
        self._inputs = None
        self._blackboard_inputs = None
        self._input_error = None
        try:
            snapshot = self._input_resolver.snapshot(
                node_id=self.node_id,
                node_instance_id=self._node_instance_id,
                parameters=self._parameters,
                value_sources=self._value_sources,
                blackboard_read_parameters=(
                    ("pose_key_a", "pose_key_b")
                    if self.node_id == "geometry.check_pose_distance"
                    else ()
                ),
            )
            self._inputs = snapshot.parameters
            self._blackboard_inputs = snapshot.blackboard
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            self._input_error = str(exc)
            self.feedback_message = f"node_input_unavailable: {exc}"

    def _resolved_inputs(self) -> Mapping[str, Any] | None:
        return self._inputs if self._input_error is None else None


class LogicCheckBool(_SnapshotAdapter):
    node_id = "logic.check_bool"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        return (
            py_trees.common.Status.SUCCESS
            if boolean_condition_matches(
                inputs["actual_value"], expected=inputs["expected_value"]
            )
            else py_trees.common.Status.FAILURE
        )


class LogicSetBool(_SnapshotAdapter):
    node_id = "logic.set_bool"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            self._input_resolver.value_store.write(inputs["key"], inputs["value"])
        except ProgramValueError as exc:
            self.feedback_message = f"blackboard_write_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.SUCCESS


class LogicWaitForBool(_SnapshotAdapter):
    node_id = "logic.wait_for_bool"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            value = self._input_resolver.value_store.read(inputs["key"])
        except KeyError:
            return py_trees.common.Status.RUNNING
        except ProgramValueError as exc:
            self.feedback_message = f"blackboard_read_failed: {exc}"
            return py_trees.common.Status.FAILURE
        if type(value) is not bool:
            self.feedback_message = "blackboard_read_failed: value is not boolean"
            return py_trees.common.Status.FAILURE
        return (
            py_trees.common.Status.SUCCESS
            if value is inputs["expected_value"]
            else py_trees.common.Status.RUNNING
        )


def _stored_pose_parts(value: object) -> tuple[object, object]:
    if not isinstance(value, Mapping):
        raise ProgramValueError("stored pose is not a mapping")
    position = value.get("position_m")
    orientation = value.get("orientation")
    if not isinstance(orientation, Mapping) or "xyzw" not in orientation:
        raise ProgramValueError("stored pose has no xyzw orientation")
    return position, orientation["xyzw"]


class GeometryCheckPoseDistance(_SnapshotAdapter):
    node_id = "geometry.check_pose_distance"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            if self._blackboard_inputs is None:
                raise ProgramValueError("pose snapshot is absent")
            first = self._blackboard_inputs["pose_key_a"]
            second = self._blackboard_inputs["pose_key_b"]
            first_position, first_orientation = _stored_pose_parts(first)
            second_position, second_orientation = _stored_pose_parts(second)
            translation = translation_distance_m(
                first_position,  # type: ignore[arg-type]
                second_position,  # type: ignore[arg-type]
            )
            rotation = quaternion_angular_distance_rad(
                first_orientation,  # type: ignore[arg-type]
                second_orientation,  # type: ignore[arg-type]
            )
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            self.feedback_message = f"pose_comparison_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return (
            py_trees.common.Status.SUCCESS
            if translation <= inputs["translation_threshold_m"]
            and rotation <= inputs["rotation_threshold_rad"]
            else py_trees.common.Status.FAILURE
        )


__all__ = [
    "GeometryCheckPoseDistance",
    "LogicCheckBool",
    "LogicSetBool",
    "LogicWaitForBool",
]
