"""Geometry / frames behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import math
from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import (
    read_blackboard_binding_value,
    register_blackboard_binding_read,
)
from manyforge_behavior.kernel_client import KernelClient


class GetLinkPose(py_trees.behaviour.Behaviour):
    """Query FK for a link pose and store result on the blackboard."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._target_frame = kwargs.get("target_frame", "")
        self._base_frame = kwargs.get("base_frame", "")
        self._output_key = kwargs.get("output_key", "")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key=self._output_key, access=py_trees.common.Access.WRITE
        )

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        pose = kernel.get_link_pose(self._target_frame, self._base_frame)
        if pose is None:
            return py_trees.common.Status.FAILURE
        self.blackboard.set(self._output_key, pose)
        return py_trees.common.Status.SUCCESS


class CheckPoseDistance(py_trees.behaviour.Behaviour):
    """Condition: succeeds if distance between two poses is within thresholds."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._pose_key_a = kwargs.get("pose_key_a", "")
        self._pose_key_b = kwargs.get("pose_key_b", "")
        self._translation_threshold_m = float(
            kwargs.get("translation_threshold_m", 0.01)
        )
        self._rotation_threshold_rad = float(
            kwargs.get("rotation_threshold_rad", 0.1)
        )

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._pose_key_a)
        register_blackboard_binding_read(self.blackboard, self._pose_key_b)

    def update(self) -> py_trees.common.Status:
        try:
            pose_a = read_blackboard_binding_value(self.blackboard, self._pose_key_a)
            pose_b = read_blackboard_binding_value(self.blackboard, self._pose_key_b)
        except KeyError:
            return py_trees.common.Status.FAILURE

        # Translation distance
        pos_a = pose_a.get("position_m", [0, 0, 0])
        pos_b = pose_b.get("position_m", [0, 0, 0])
        trans_dist = math.sqrt(
            sum((a - b) ** 2 for a, b in zip(pos_a, pos_b))
        )
        if trans_dist > self._translation_threshold_m:
            return py_trees.common.Status.FAILURE

        # Rotation distance (quaternion angular distance)
        q_a = pose_a.get("orientation_xyzw", [0, 0, 0, 1])
        q_b = pose_b.get("orientation_xyzw", [0, 0, 0, 1])
        dot = sum(a * b for a, b in zip(q_a, q_b))
        dot = max(-1.0, min(1.0, abs(dot)))  # abs handles double-cover
        rot_dist = 2.0 * math.acos(dot)
        if rot_dist > self._rotation_threshold_rad:
            return py_trees.common.Status.FAILURE

        return py_trees.common.Status.SUCCESS
