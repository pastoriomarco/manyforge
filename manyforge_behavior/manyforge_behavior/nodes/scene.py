"""Scene manipulation behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest
from manyforge_behavior.nodes.motion import _rpy_deg_to_xyzw


def _get_scene_callback() -> Optional[Callable]:
    """Try to get the scene_edit_callback from blackboard."""
    try:
        bb = py_trees.blackboard.Client(name="scene_cb_read")
        bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.READ)
        return bb.scene_edit_callback
    except (KeyError, AttributeError):
        return None


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
        orient = pose.get("orientation_xyzw") or (
            _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
            if "orientation_rpy_deg" in pose else None
        )
        request = SceneEditRequest(
            operation="add",
            object_id=self._params.get("object_id", ""),
            shape_type=self._params.get("shape_type"),
            box_dimensions_m=self._params.get("box_dimensions_m"),
            sphere_radius_m=self._params.get("sphere_radius_m"),
            pose_position_m=pose.get("position_m"),
            pose_orientation_xyzw=orient,
            frame_id=self._params.get("frame_id"),
        )
        result = kernel.scene_edit(request)
        if result.success:
            cb = _get_scene_callback()
            if cb:
                cb("add", request)
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
            cb = _get_scene_callback()
            if cb:
                cb("remove", request)
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
        orient = pose.get("orientation_xyzw") or (
            _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
            if "orientation_rpy_deg" in pose else None
        )
        request = SceneEditRequest(
            operation="update_pose",
            object_id=self._params.get("object_id", ""),
            pose_position_m=pose.get("position_m"),
            pose_orientation_xyzw=orient,
            frame_id=self._params.get("frame_id"),
        )
        result = kernel.scene_edit(request)
        if result.success:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class AttachObjectToLink(py_trees.behaviour.Behaviour):
    """Attach a scene object to a robot link.

    Delegates the authoritative attach operation to the backend so the
    frame transition is owned by backend scene state, not by behavior code.

    Params:
        object_id: object to attach
        link_frame: robot link to attach to (e.g. "tool0")
    """

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.READ)

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        object_id = self._params.get("object_id", "")
        link_frame = self._params.get("link_frame", "tool0")

        registry: dict = getattr(self.blackboard, "scene_objects", {})
        obj_info = registry.get(object_id, {})

        request = SceneEditRequest(
            operation="attach",
            object_id=object_id,
            shape_type=obj_info.get("shape_type"),
            box_dimensions_m=obj_info.get("box_dimensions_m"),
            sphere_radius_m=obj_info.get("sphere_radius_m"),
            frame_id=link_frame,
        )
        result = kernel.scene_edit(request)
        if not result.success:
            self.logger.error(f"Failed to attach '{object_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        cb = _get_scene_callback()
        if cb and obj_info:
            pose = kernel.get_object_pose(object_id, link_frame)
            if pose is not None:
                cb("remove", SceneEditRequest(operation="remove", object_id=object_id))
                cb(
                    "add",
                    SceneEditRequest(
                        operation="add",
                        object_id=object_id,
                        shape_type=obj_info.get("shape_type"),
                        box_dimensions_m=obj_info.get("box_dimensions_m"),
                        sphere_radius_m=obj_info.get("sphere_radius_m"),
                        pose_position_m=list(pose["position_m"]),
                        pose_orientation_xyzw=list(pose["orientation_xyzw"]),
                        frame_id=link_frame,
                    ),
                )

        return py_trees.common.Status.SUCCESS


class DetachObjectFromLink(py_trees.behaviour.Behaviour):
    """Detach an object from a robot link and place it in the world frame.

    Delegates the authoritative detach/reparent operation to the backend.

    Params:
        object_id: object identifier to detach
        frame_id: target frame after detach (defaults to "universe")
    """

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.READ)

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        object_id = self._params.get("object_id", "")
        target_frame = self._params.get("frame_id", "universe")
        registry: dict = getattr(self.blackboard, "scene_objects", {})
        obj_info = registry.get(object_id, {})

        request = SceneEditRequest(
            operation="detach",
            object_id=object_id,
            shape_type=obj_info.get("shape_type"),
            box_dimensions_m=obj_info.get("box_dimensions_m"),
            sphere_radius_m=obj_info.get("sphere_radius_m"),
            frame_id=target_frame,
        )
        result = kernel.scene_edit(request)
        if not result.success:
            self.logger.error(f"Failed to detach '{object_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        cb = _get_scene_callback()
        if cb and obj_info:
            pose = kernel.get_object_pose(object_id, target_frame)
            if pose is not None:
                cb("remove", SceneEditRequest(operation="remove", object_id=object_id))
                cb(
                    "add",
                    SceneEditRequest(
                        operation="add",
                        object_id=object_id,
                        shape_type=obj_info.get("shape_type"),
                        box_dimensions_m=obj_info.get("box_dimensions_m"),
                        sphere_radius_m=obj_info.get("sphere_radius_m"),
                        pose_position_m=list(pose["position_m"]),
                        pose_orientation_xyzw=list(pose["orientation_xyzw"]),
                        frame_id=target_frame,
                    ),
                )

        return py_trees.common.Status.SUCCESS
