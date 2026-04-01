"""Scene manipulation behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest


class AddCollisionObject(py_trees.behaviour.Behaviour):
    """Add a collision object to the backend scene via the kernel."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        pose = self._params.get("pose", {})
        request = SceneEditRequest(
            operation="add",
            object_id=self._params.get("object_id", ""),
            shape_type=self._params.get("shape_type"),
            box_dimensions_m=self._params.get("box_dimensions_m"),
            sphere_radius_m=self._params.get("sphere_radius_m"),
            pose_position_m=pose.get("position_m"),
            pose_orientation_xyzw=pose.get("orientation_xyzw"),
            frame_id=self._params.get("frame_id"),
        )
        result = kernel.scene_edit(request)
        if result.success:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class RemoveCollisionObject(py_trees.behaviour.Behaviour):
    """Remove a collision object from the backend scene."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        request = SceneEditRequest(
            operation="remove",
            object_id=self._params.get("object_id", ""),
        )
        result = kernel.scene_edit(request)
        if result.success:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class UpdateCollisionObjectPose(py_trees.behaviour.Behaviour):
    """Update the pose of an existing collision object."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        pose = self._params.get("pose", {})
        request = SceneEditRequest(
            operation="update_pose",
            object_id=self._params.get("object_id", ""),
            pose_position_m=pose.get("position_m"),
            pose_orientation_xyzw=pose.get("orientation_xyzw"),
            frame_id=self._params.get("frame_id"),
        )
        result = kernel.scene_edit(request)
        if result.success:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE
