"""Scene manipulation behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import KernelClient, SceneEditRequest
from manyforge_behavior.nodes.motion import _rpy_deg_to_xyzw
from manyforge_behavior.parameters import resolve_blackboard_value


class _SceneCallbackMixin:
    blackboard: py_trees.blackboard.Client

    def _register_scene_callback_key(self) -> None:
        self.blackboard.register_key(
            key="scene_edit_callback", access=py_trees.common.Access.READ
        )

    def _get_scene_callback(self) -> Optional[Callable]:
        try:
            callback = self.blackboard.get("scene_edit_callback")
        except (KeyError, AttributeError):
            return None
        return callback if callable(callback) else None

    def _emit_scene_callback(self, operation: str, request: SceneEditRequest) -> None:
        callback = self._get_scene_callback()
        if callback is not None:
            callback(operation, request)


def _resolve_collision_object_request(
    blackboard: py_trees.blackboard.Client,
    params: dict[str, Any],
    *,
    operation: str,
) -> SceneEditRequest:
    object_id = resolve_blackboard_value(blackboard, params.get("object_id", ""))
    shape_type = resolve_blackboard_value(blackboard, params.get("shape_type"))
    box_dimensions_m = resolve_blackboard_value(blackboard, params.get("box_dimensions_m"))
    sphere_radius_m = resolve_blackboard_value(blackboard, params.get("sphere_radius_m"))
    pose = resolve_blackboard_value(blackboard, params.get("pose", {}))
    frame_id = resolve_blackboard_value(blackboard, params.get("frame_id"))
    orient = pose.get("orientation_xyzw") or (
        _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
        if "orientation_rpy_deg" in pose else None
    )
    return SceneEditRequest(
        operation=operation,
        object_id=object_id,
        shape_type=shape_type,
        box_dimensions_m=box_dimensions_m,
        sphere_radius_m=sphere_radius_m,
        pose_position_m=pose.get("position_m"),
        pose_orientation_xyzw=orient,
        frame_id=frame_id,
    )


class AddCollisionObject(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
    """Add a collision object to the backend scene via the kernel."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            request = _resolve_collision_object_request(
                self.blackboard,
                self._params,
                operation="add",
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        result = kernel.scene_edit(request)
        if result.success:
            self._emit_scene_callback("add", request)
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class UpsertCollisionObject(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
    """Insert or replace a collision object in the backend scene via the kernel."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            request = _resolve_collision_object_request(
                self.blackboard,
                self._params,
                operation="upsert",
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        result = kernel.scene_edit(request)
        if result.success:
            self._emit_scene_callback("upsert", request)
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class RemoveCollisionObject(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
    """Remove a collision object from the backend scene."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            object_id = resolve_blackboard_value(
                self.blackboard, self._params.get("object_id", "")
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        request = SceneEditRequest(
            operation="remove",
            object_id=object_id,
        )
        result = kernel.scene_edit(request)
        if result.success:
            self._emit_scene_callback("remove", request)
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class UpdateCollisionObjectPose(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
    """Update the pose of an existing collision object."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            object_id = resolve_blackboard_value(
                self.blackboard, self._params.get("object_id", "")
            )
            pose = resolve_blackboard_value(
                self.blackboard, self._params.get("pose", {})
            )
            frame_id = resolve_blackboard_value(
                self.blackboard, self._params.get("frame_id")
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        orient = pose.get("orientation_xyzw") or (
            _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
            if "orientation_rpy_deg" in pose else None
        )
        request = SceneEditRequest(
            operation="update_pose",
            object_id=object_id,
            pose_position_m=pose.get("position_m"),
            pose_orientation_xyzw=orient,
            frame_id=frame_id,
        )
        result = kernel.scene_edit(request)
        if result.success:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class AttachObjectToLink(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
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
        self.blackboard.register_key(key="gripper_configs", access=py_trees.common.Access.READ)
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            object_id = resolve_blackboard_value(
                self.blackboard, self._params.get("object_id", "")
            )
            link_frame = resolve_blackboard_value(
                self.blackboard, self._params.get("link_frame", "tool0")
            )
            explicit_touch_links = resolve_blackboard_value(
                self.blackboard, self._params.get("touch_links")
            )
            gripper_id = resolve_blackboard_value(
                self.blackboard, self._params.get("gripper_id", "")
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE

        registry: dict = getattr(self.blackboard, "scene_objects", {})
        obj_info = registry.get(object_id, {})
        touch_links = None
        if explicit_touch_links is not None:
            if not isinstance(explicit_touch_links, list) or any(
                not isinstance(link_name, str) or not link_name
                for link_name in explicit_touch_links
            ):
                self.feedback_message = (
                    "attach_object_to_link.touch_links must resolve to a list of non-empty strings"
                )
                return py_trees.common.Status.FAILURE
            touch_links = list(explicit_touch_links)
        elif isinstance(gripper_id, str) and gripper_id:
            configs = getattr(self.blackboard, "gripper_configs", {})
            config = configs.get(gripper_id)
            if not isinstance(config, dict):
                self.feedback_message = f"Unknown gripper_id '{gripper_id}'"
                return py_trees.common.Status.FAILURE
            config_touch_links = config.get("touch_links", [])
            if not isinstance(config_touch_links, list) or any(
                not isinstance(link_name, str) or not link_name
                for link_name in config_touch_links
            ):
                self.feedback_message = (
                    f"gripper '{gripper_id}' has invalid touch_links configuration"
                )
                return py_trees.common.Status.FAILURE
            touch_links = list(config_touch_links)

        request = SceneEditRequest(
            operation="attach",
            object_id=object_id,
            shape_type=obj_info.get("shape_type"),
            box_dimensions_m=obj_info.get("box_dimensions_m"),
            sphere_radius_m=obj_info.get("sphere_radius_m"),
            frame_id=link_frame,
            touch_links=touch_links,
        )
        result = kernel.scene_edit(request)
        if not result.success:
            self.logger.error(f"Failed to attach '{object_id}': {result.error}")
            return py_trees.common.Status.FAILURE

        if obj_info:
            pose = kernel.get_object_pose(object_id, link_frame)
            if pose is not None:
                self._emit_scene_callback(
                    "remove", SceneEditRequest(operation="remove", object_id=object_id)
                )
                self._emit_scene_callback(
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


class DetachObjectFromLink(_SceneCallbackMixin, py_trees.behaviour.Behaviour):
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
        self._register_scene_callback_key()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        try:
            object_id = resolve_blackboard_value(
                self.blackboard, self._params.get("object_id", "")
            )
            target_frame = resolve_blackboard_value(
                self.blackboard, self._params.get("frame_id", "universe")
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
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

        if obj_info:
            pose = kernel.get_object_pose(object_id, target_frame)
            if pose is not None:
                self._emit_scene_callback(
                    "remove", SceneEditRequest(operation="remove", object_id=object_id)
                )
                self._emit_scene_callback(
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
