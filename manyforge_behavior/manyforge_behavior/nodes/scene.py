"""Scene manipulation behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import math
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest
from manyforge_behavior.nodes.motion import _rpy_deg_to_xyzw


def _pose_to_matrix(pos: list[float], orient_xyzw: list[float]) -> list[list[float]]:
    """Build 4x4 matrix from position + quaternion (xyzw)."""
    x, y, z, w = orient_xyzw
    return [
        [1-2*y*y-2*z*z, 2*x*y-2*z*w, 2*x*z+2*y*w, pos[0]],
        [2*x*y+2*z*w, 1-2*x*x-2*z*z, 2*y*z-2*x*w, pos[1]],
        [2*x*z-2*y*w, 2*y*z+2*x*w, 1-2*x*x-2*y*y, pos[2]],
        [0, 0, 0, 1],
    ]


def _matrix_to_pose(m: list[list[float]]) -> tuple[list[float], list[float]]:
    """Extract position + quaternion (xyzw) from 4x4 matrix."""
    pos = [m[0][3], m[1][3], m[2][3]]
    # Rotation matrix to quaternion
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    return pos, [x, y, z, w]


def _invert_matrix(m: list[list[float]]) -> list[list[float]]:
    """Invert a 4x4 rigid-body transform: [R|t] -> [R^T | -R^T*t]."""
    # Transpose 3x3 rotation
    rt = [[m[j][i] for j in range(3)] for i in range(3)]
    # -R^T * t
    t = [m[0][3], m[1][3], m[2][3]]
    nt = [-(rt[i][0]*t[0] + rt[i][1]*t[1] + rt[i][2]*t[2]) for i in range(3)]
    return [
        [rt[0][0], rt[0][1], rt[0][2], nt[0]],
        [rt[1][0], rt[1][1], rt[1][2], nt[1]],
        [rt[2][0], rt[2][1], rt[2][2], nt[2]],
        [0, 0, 0, 1],
    ]


def _multiply_matrices(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    """Multiply two 4x4 matrices."""
    return [
        [sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
        for i in range(4)
    ]


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

    Removes the object from its current frame, computes the relative transform
    from the link to the object at the current robot state, and re-adds the
    object attached to the link frame.

    Params:
        object_id: object to attach
        link_frame: robot link to attach to (e.g. "tool0")
        base_frame: base frame for FK (default: from blackboard)
    """

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="base_frame", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        object_id = self._params.get("object_id", "")
        link_frame = self._params.get("link_frame", "tool0")
        base_frame = self._params.get("base_frame", "") or self.blackboard.base_frame

        # Look up object metadata from registry
        registry: dict = getattr(self.blackboard, "scene_objects", {})
        obj_info = registry.get(object_id)
        if not obj_info:
            self.logger.error(f"Object '{object_id}' not found in scene registry")
            return py_trees.common.Status.FAILURE

        # Get link pose in world frame via FK
        link_pose = kernel.get_link_pose(link_frame, base_frame)
        if not link_pose:
            self.logger.error(f"FK failed for {link_frame}")
            return py_trees.common.Status.FAILURE

        # Build world transforms
        obj_pos = obj_info.get("position_m", [0, 0, 0])
        obj_orient = obj_info.get("orientation_xyzw", [0, 0, 0, 1])
        t_obj = _pose_to_matrix(obj_pos, obj_orient)

        link_pos = list(link_pose["position_m"])
        link_orient = list(link_pose["orientation_xyzw"])
        t_link = _pose_to_matrix(link_pos, link_orient)

        # T_rel = T_link^-1 * T_object
        t_link_inv = _invert_matrix(t_link)
        t_rel = _multiply_matrices(t_link_inv, t_obj)
        rel_pos, rel_orient = _matrix_to_pose(t_rel)

        # Remove from current frame
        remove_req = SceneEditRequest(operation="remove", object_id=object_id)
        result = kernel.scene_edit(remove_req)
        if not result.success:
            self.logger.error(f"Failed to remove '{object_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        # Re-add attached to link
        attached_id = object_id + "_attached"
        add_req = SceneEditRequest(
            operation="add",
            object_id=attached_id,
            shape_type=obj_info.get("shape_type"),
            box_dimensions_m=obj_info.get("box_dimensions_m"),
            sphere_radius_m=obj_info.get("sphere_radius_m"),
            pose_position_m=rel_pos,
            pose_orientation_xyzw=rel_orient,
            frame_id=link_frame,
        )
        result = kernel.scene_edit(add_req)
        if not result.success:
            self.logger.error(f"Failed to attach '{attached_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        # Update registry: remove old, add attached
        registry.pop(object_id, None)
        registry[attached_id] = {
            "shape_type": obj_info.get("shape_type"),
            "box_dimensions_m": obj_info.get("box_dimensions_m"),
            "sphere_radius_m": obj_info.get("sphere_radius_m"),
            "position_m": rel_pos,
            "orientation_xyzw": rel_orient,
            "frame_id": link_frame,
        }
        self.blackboard.scene_objects = registry

        # Notify Viser
        cb = _get_scene_callback()
        if cb:
            cb("remove", remove_req)
            cb("add", add_req)

        return py_trees.common.Status.SUCCESS


class DetachObjectFromLink(py_trees.behaviour.Behaviour):
    """Detach an object from a robot link and place it in the world frame.

    Computes the object's current world pose from the link's FK and the
    stored relative transform, then removes the attached object and re-adds
    it at the computed world pose.

    Params:
        object_id: original object_id (the attached one is object_id + "_attached")
        link_frame: robot link it's attached to (default: "tool0")
        base_frame: base frame for FK
    """

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="base_frame", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        object_id = self._params.get("object_id", "")
        link_frame = self._params.get("link_frame", "tool0")
        base_frame = self._params.get("base_frame", "") or self.blackboard.base_frame

        attached_id = object_id + "_attached"
        registry: dict = getattr(self.blackboard, "scene_objects", {})
        obj_info = registry.get(attached_id)
        if not obj_info:
            self.logger.error(f"Attached object '{attached_id}' not found in registry")
            return py_trees.common.Status.FAILURE

        # Get link pose in world
        link_pose = kernel.get_link_pose(link_frame, base_frame)
        if not link_pose:
            self.logger.error(f"FK failed for {link_frame}")
            return py_trees.common.Status.FAILURE

        # Compute world pose: T_world = T_link * T_rel
        link_pos = list(link_pose["position_m"])
        link_orient = list(link_pose["orientation_xyzw"])
        t_link = _pose_to_matrix(link_pos, link_orient)
        rel_pos = obj_info.get("position_m", [0, 0, 0])
        rel_orient = obj_info.get("orientation_xyzw", [0, 0, 0, 1])
        t_rel = _pose_to_matrix(rel_pos, rel_orient)
        t_world = _multiply_matrices(t_link, t_rel)
        world_pos, world_orient = _matrix_to_pose(t_world)

        # Remove attached object
        remove_req = SceneEditRequest(operation="remove", object_id=attached_id)
        result = kernel.scene_edit(remove_req)
        if not result.success:
            self.logger.error(f"Failed to remove '{attached_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        # Re-add in world frame with original object_id
        add_req = SceneEditRequest(
            operation="add",
            object_id=object_id,
            shape_type=obj_info.get("shape_type"),
            box_dimensions_m=obj_info.get("box_dimensions_m"),
            sphere_radius_m=obj_info.get("sphere_radius_m"),
            pose_position_m=world_pos,
            pose_orientation_xyzw=world_orient,
            frame_id="universe",
        )
        result = kernel.scene_edit(add_req)
        if not result.success:
            self.logger.error(f"Failed to place '{object_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        # Update registry
        registry.pop(attached_id, None)
        registry[object_id] = {
            "shape_type": obj_info.get("shape_type"),
            "box_dimensions_m": obj_info.get("box_dimensions_m"),
            "sphere_radius_m": obj_info.get("sphere_radius_m"),
            "position_m": world_pos,
            "orientation_xyzw": world_orient,
            "frame_id": "universe",
        }
        self.blackboard.scene_objects = registry

        # Notify Viser
        cb = _get_scene_callback()
        if cb:
            cb("remove", remove_req)
            cb("add", add_req)

        return py_trees.common.Status.SUCCESS
