"""Scene manipulation behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import logging
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.action_span import ActionSpanMixin
from manyforge_behavior.geometry import rpy_deg_to_xyzw as _rpy_deg_to_xyzw
from manyforge_behavior.kernel_client import (
    GripperCommandState,
    KernelClient,
    SceneEditRequest,
)
from manyforge_behavior.parameters import resolve_blackboard_value
from manyforge_behavior.runtime_pause import RuntimePauseMixin, RuntimePauseRegistry


logger = logging.getLogger(__name__)


_CONFIRMATION_POLICY_EVIDENCE_LABELS: dict[str, set[str]] = {
    "operator_confirm": {
        "operator_confirm",
        "operator_grasp_confirmed",
        "grasp_confirmed",
    },
    "discrete_signal": {
        "discrete_signal",
        "part_present_signal",
        "pressure_switch",
        "vacuum_switch",
        "grasp_confirmed",
    },
    "perception": {
        "perception_grasp_confirmed",
        "measured_attachment_baseline",
        "attached_drift_monitor",
        "grasp_confirmed",
    },
    "force_or_current": {
        "force_or_current",
        "force_threshold",
        "current_threshold",
        "driver_stall",
        "grasp_confirmed",
    },
}

_POSITIVE_GRASP_EVIDENCE_STATES = {
    "confirmed",
    "verified",
    "grasp_confirmed",
}

_NEGATIVE_GRASP_EVIDENCE_STATES = {
    "unverified",
    "faulted_unverified",
    "pressure_or_part_lost",
}


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
            try:
                callback(operation, request)
            except Exception:  # noqa: BLE001 - observer must not change authority.
                logger.exception("Scene observer callback failed after %s", operation)


class _SceneMutationNode(
    ActionSpanMixin,
    RuntimePauseMixin,
    _SceneCallbackMixin,
    py_trees.behaviour.Behaviour,
):
    _catalog_node_id = "scene_mutation"

    def __init__(self, name: str, **kwargs: Any):
        runtime_pause_registry = kwargs.pop("runtime_pause_registry", None)
        super().__init__(name=name)
        self._params = kwargs
        self._configure_runtime_pause(
            registry=(
                runtime_pause_registry
                if isinstance(runtime_pause_registry, RuntimePauseRegistry)
                else None
            ),
            node_id=self._catalog_node_id,
            domain="scene",
        )

    def _runtime_pause_gate(self) -> Optional[py_trees.common.Status]:
        if self._consume_runtime_resume() is not None:
            self.feedback_message = ""
            return None
        if self._runtime_pause_active():
            return py_trees.common.Status.RUNNING
        return None

    def _scene_backend_failure(
        self,
        *,
        operation: str,
        message: str,
        object_id: str,
        details: Optional[dict[str, Any]] = None,
    ) -> py_trees.common.Status:
        self.feedback_message = message
        self._raise_runtime_pause(
            reason_code="scene_backend_refused",
            message=message,
            details={
                "operation": operation,
                "object_id": object_id,
                **(details or {}),
            },
        )
        return py_trees.common.Status.RUNNING

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._clear_runtime_pause()


def _resolve_collision_object_request(
    blackboard: py_trees.blackboard.Client,
    params: dict[str, Any],
    *,
    operation: str,
) -> SceneEditRequest:
    object_id = resolve_blackboard_value(blackboard, params.get("object_id", ""))
    shape = resolve_blackboard_value(blackboard, params.get("shape"))
    if not isinstance(shape, dict):
        raise KeyError("shape")
    shape_type = resolve_blackboard_value(blackboard, shape.get("type"))
    box_dimensions_m = resolve_blackboard_value(blackboard, shape.get("box_dimensions_m"))
    sphere_radius_m = resolve_blackboard_value(blackboard, shape.get("sphere_radius_m"))
    cylinder_radius_m = resolve_blackboard_value(blackboard, shape.get("cylinder_radius_m"))
    cylinder_height_m = resolve_blackboard_value(blackboard, shape.get("cylinder_height_m"))
    mesh_resource_uri = resolve_blackboard_value(blackboard, shape.get("mesh_resource_uri"))
    mesh_scale = resolve_blackboard_value(blackboard, shape.get("mesh_scale"))
    support_contacts = resolve_blackboard_value(blackboard, params.get("support_contacts"))
    pose = resolve_blackboard_value(blackboard, params.get("pose", {}))
    # frame_id lives INSIDE pose (PoseStamped-shaped) — same shape as
    # scene-resource pose. The previous separate-top-level form was
    # an asymmetry between the tree-node and scene-resource schemas
    # that small models tripped on consistently; unified in the v0.1
    # cleanup. See spec 410 §4 and spec 485 §2.10.
    frame_id = pose.get("frame_id") if isinstance(pose, dict) else None
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
        cylinder_radius_m=cylinder_radius_m,
        cylinder_height_m=cylinder_height_m,
        mesh_resource_uri=mesh_resource_uri,
        mesh_scale=mesh_scale,
        pose_position_m=pose.get("position_m"),
        pose_orientation_xyzw=orient,
        frame_id=frame_id,
        support_contacts=support_contacts,
    )


def _shape_fields_from_registry(obj_info: dict[str, Any]) -> dict[str, Any]:
    return {
        "shape_type": obj_info.get("shape_type"),
        "box_dimensions_m": obj_info.get("box_dimensions_m"),
        "sphere_radius_m": obj_info.get("sphere_radius_m"),
        "cylinder_radius_m": obj_info.get("cylinder_radius_m"),
        "cylinder_height_m": obj_info.get("cylinder_height_m"),
        "mesh_resource_uri": obj_info.get("mesh_resource_uri"),
        "mesh_scale": obj_info.get("mesh_scale"),
        "support_contacts": obj_info.get("support_contacts"),
    }


class AddCollisionObject(_SceneMutationNode):
    """Add a collision object to the backend scene via the kernel."""

    _catalog_node_id = "add_collision_object"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
        kernel: KernelClient = self.blackboard.kernel
        try:
            request = _resolve_collision_object_request(
                self.blackboard,
                self._params,
                operation="add",
            )
            request.action_span = self._current_action_span(self.blackboard)
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="add",
                object_id=request.object_id,
                message=f"Scene add failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if result.success:
            self._emit_scene_callback("add", request)
            return py_trees.common.Status.SUCCESS
        return self._scene_backend_failure(
            operation="add",
            object_id=request.object_id,
            message=result.error or f"Failed to add '{request.object_id}'",
        )


class UpsertCollisionObject(_SceneMutationNode):
    """Insert or replace a collision object in the backend scene via the kernel."""

    _catalog_node_id = "upsert_collision_object"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
        kernel: KernelClient = self.blackboard.kernel
        try:
            request = _resolve_collision_object_request(
                self.blackboard,
                self._params,
                operation="upsert",
            )
            request.action_span = self._current_action_span(self.blackboard)
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="upsert",
                object_id=request.object_id,
                message=f"Scene upsert failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if result.success:
            self._emit_scene_callback("upsert", request)
            return py_trees.common.Status.SUCCESS
        return self._scene_backend_failure(
            operation="upsert",
            object_id=request.object_id,
            message=result.error or f"Failed to upsert '{request.object_id}'",
        )


class RemoveCollisionObject(_SceneMutationNode):
    """Remove a collision object from the backend scene."""

    _catalog_node_id = "remove_collision_object"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
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
            action_span=self._current_action_span(self.blackboard),
        )
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="remove",
                object_id=request.object_id,
                message=f"Scene remove failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if result.success:
            self._emit_scene_callback("remove", request)
            return py_trees.common.Status.SUCCESS
        return self._scene_backend_failure(
            operation="remove",
            object_id=request.object_id,
            message=result.error or f"Failed to remove '{request.object_id}'",
        )


class UpdateCollisionObjectPose(_SceneMutationNode):
    """Update the pose of an existing collision object."""

    _catalog_node_id = "update_collision_object_pose"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
        kernel: KernelClient = self.blackboard.kernel
        try:
            object_id = resolve_blackboard_value(
                self.blackboard, self._params.get("object_id", "")
            )
            pose = resolve_blackboard_value(
                self.blackboard, self._params.get("pose", {})
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            return py_trees.common.Status.FAILURE
        # frame_id is sourced from pose (PoseStamped). See the
        # _resolve_collision_object_request comment for context.
        frame_id = pose.get("frame_id") if isinstance(pose, dict) else None
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
            action_span=self._current_action_span(self.blackboard),
        )
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="update_pose",
                object_id=request.object_id,
                message=f"Scene pose update failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if result.success:
            self._emit_scene_callback("update_pose", request)
            return py_trees.common.Status.SUCCESS
        return self._scene_backend_failure(
            operation="update_pose",
            object_id=request.object_id,
            message=result.error or f"Failed to update '{request.object_id}'",
        )


class AttachObjectToLink(_SceneMutationNode):
    """Attach a scene object to a robot link.

    Delegates the authoritative attach operation to the backend so the
    frame transition is owned by backend scene state, not by behavior code.

    Params:
        object_id: object to attach
        link_frame: robot link to attach to (e.g. "tool0")
    """

    _catalog_node_id = "attach_object_to_link"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)
        self._grasp_failure_recoverable = False
        self._grasp_failure_reason = ""

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="gripper_configs", access=py_trees.common.Access.READ)
        self.blackboard.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.WRITE
        )
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()
        self._grasp_failure_recoverable = False
        self._grasp_failure_reason = ""
        try:
            self.blackboard.set("gripper_fault_details", None)
        except (KeyError, AttributeError):
            pass

    def _record_attach_fault(
        self,
        reason_code: str,
        message: str,
        *,
        object_id: str,
        link_frame: str,
        gripper_id: str,
        grasp_confirmation_policy: str,
        status: Any | None = None,
    ) -> None:
        try:
            details: dict[str, Any] = {
                "reason_code": reason_code,
                "surface": "attach_object_to_link",
                "message": message,
                "object_id": object_id,
                "link_frame": link_frame,
                "gripper_id": gripper_id,
                "grasp_confirmation_policy": grasp_confirmation_policy,
            }
            if status is not None:
                state = getattr(status, "state", "")
                details["gripper_state"] = getattr(state, "value", state)
                for attr in (
                    "actuation_kind",
                    "state_authority",
                    "completion_policy",
                    "transport_kind",
                    "commanded_state",
                    "evidence_state",
                ):
                    value = getattr(status, attr, "")
                    if value not in (None, "", [], {}):
                        details[attr] = value
                status_reason_code = getattr(status, "reason_code", "")
                if status_reason_code:
                    details["gripper_status_reason_code"] = status_reason_code
                evidence = getattr(status, "evidence", None)
                if evidence:
                    details["evidence"] = list(evidence)
            self.blackboard.set("gripper_fault_details", details)
        except (KeyError, AttributeError):
            pass

    @staticmethod
    def _grasp_confirmation_satisfied(
        grasp_confirmation_policy: str,
        status: Any,
    ) -> bool:
        evidence_state = str(getattr(status, "evidence_state", "") or "").strip()
        if evidence_state in _POSITIVE_GRASP_EVIDENCE_STATES:
            return True
        if evidence_state in _NEGATIVE_GRASP_EVIDENCE_STATES:
            return False

        evidence = {
            str(item)
            for item in (getattr(status, "evidence", []) or [])
            if str(item)
        }
        accepted_labels = _CONFIRMATION_POLICY_EVIDENCE_LABELS.get(
            grasp_confirmation_policy,
            set(),
        )
        return bool(evidence.intersection(accepted_labels))

    def _validate_grasp_confirmation(
        self,
        *,
        kernel: KernelClient,
        object_id: str,
        link_frame: str,
        gripper_id: str,
        grasp_confirmation_policy: str,
    ) -> bool:
        self._grasp_failure_recoverable = False
        self._grasp_failure_reason = ""
        if not grasp_confirmation_policy:
            self.feedback_message = (
                f"gripper '{gripper_id}' is missing grasp_confirmation_policy; "
                "attach refused"
            )
            self._record_attach_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
            )
            return False

        if grasp_confirmation_policy == "none_open_loop":
            return True

        if grasp_confirmation_policy not in _CONFIRMATION_POLICY_EVIDENCE_LABELS:
            self.feedback_message = (
                f"gripper '{gripper_id}' has unsupported "
                f"grasp_confirmation_policy='{grasp_confirmation_policy}'"
            )
            self._record_attach_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
            )
            return False

        try:
            status = kernel.get_gripper_status(gripper_id)
        except Exception as exc:  # noqa: BLE001 - backend contract boundary
            self.feedback_message = (
                f"gripper '{gripper_id}' status unavailable for "
                f"grasp_confirmation_policy={grasp_confirmation_policy}: {exc}"
            )
            self._record_attach_fault(
                "stale_gripper_state",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
            )
            self._grasp_failure_recoverable = True
            self._grasp_failure_reason = "stale_gripper_state"
            return False

        status_policy = str(
            getattr(status, "grasp_confirmation_policy", "") or ""
        ).strip()
        if status_policy and status_policy != grasp_confirmation_policy:
            self.feedback_message = (
                f"gripper '{gripper_id}' status policy '{status_policy}' does not "
                f"match attach policy '{grasp_confirmation_policy}'"
            )
            self._record_attach_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
                status=status,
            )
            return False

        if getattr(status, "state", None) == GripperCommandState.FAILED:
            self.feedback_message = (
                f"gripper '{gripper_id}' is faulted; attach refused"
            )
            self._record_attach_fault(
                "grasp_confirmation_lost",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
                status=status,
            )
            self._grasp_failure_recoverable = True
            self._grasp_failure_reason = "grasp_confirmation_lost"
            return False

        evidence_state = str(getattr(status, "evidence_state", "") or "").strip()
        if evidence_state in {"faulted_unverified", "pressure_or_part_lost"}:
            self.feedback_message = (
                f"gripper '{gripper_id}' grasp confirmation is {evidence_state}; "
                "attach refused"
            )
            self._record_attach_fault(
                "grasp_confirmation_lost",
                self.feedback_message,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
                status=status,
            )
            self._grasp_failure_recoverable = True
            self._grasp_failure_reason = "grasp_confirmation_lost"
            return False

        if self._grasp_confirmation_satisfied(grasp_confirmation_policy, status):
            return True

        evidence = ",".join(getattr(status, "evidence", []) or []) or "none"
        evidence_state = evidence_state or "none"
        self.feedback_message = (
            f"gripper '{gripper_id}' lacks verified grasp evidence for "
            f"grasp_confirmation_policy={grasp_confirmation_policy}; "
            f"evidence={evidence} evidence_state={evidence_state}"
        )
        self._record_attach_fault(
            "command_estimate_attach_rejected",
            self.feedback_message,
            object_id=object_id,
            link_frame=link_frame,
            gripper_id=gripper_id,
            grasp_confirmation_policy=grasp_confirmation_policy,
            status=status,
        )
        self._grasp_failure_recoverable = True
        self._grasp_failure_reason = "command_estimate_attach_rejected"
        return False

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
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
        grasp_confirmation_policy = ""
        gripper_config = None
        if isinstance(gripper_id, str) and gripper_id:
            configs = getattr(self.blackboard, "gripper_configs", {})
            gripper_config = configs.get(gripper_id)
            if not isinstance(gripper_config, dict):
                self.feedback_message = f"Unknown gripper_id '{gripper_id}'"
                return py_trees.common.Status.FAILURE
            grasp_confirmation_policy = str(
                gripper_config.get("grasp_confirmation_policy", "") or ""
            )
            if not self._validate_grasp_confirmation(
                kernel=kernel,
                object_id=object_id,
                link_frame=link_frame,
                gripper_id=gripper_id,
                grasp_confirmation_policy=grasp_confirmation_policy,
            ):
                if self._grasp_failure_recoverable:
                    self._raise_runtime_pause(
                        reason_code=self._grasp_failure_reason,
                        message=self.feedback_message,
                        details={
                            "object_id": object_id,
                            "gripper_id": gripper_id,
                        },
                    )
                    return py_trees.common.Status.RUNNING
                return py_trees.common.Status.FAILURE
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
        elif gripper_config is not None:
            config_touch_links = gripper_config.get("touch_links", [])
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
            **_shape_fields_from_registry(obj_info),
            frame_id=link_frame,
            touch_links=touch_links,
            action_span=self._current_action_span(self.blackboard),
        )
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="attach",
                object_id=request.object_id,
                message=f"Scene attach failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if not result.success:
            self.logger.error(f"Failed to attach '{object_id}': {result.error}")
            return self._scene_backend_failure(
                operation="attach",
                object_id=request.object_id,
                message=result.error or f"Failed to attach '{object_id}'",
            )

        self._emit_scene_callback("attach", request)
        if grasp_confirmation_policy == "none_open_loop":
            self.feedback_message = (
                "Attached using grasp_confirmation_policy=none_open_loop; "
                "object holding is an explicit open-loop assumption"
            )
            self.logger.warning(self.feedback_message)
        elif grasp_confirmation_policy:
            self.feedback_message = (
                f"Attached using grasp_confirmation_policy={grasp_confirmation_policy}"
            )

        if obj_info:
            try:
                pose = kernel.get_object_pose(object_id, link_frame)
            except Exception:  # noqa: BLE001 - post-commit observer only.
                logger.exception(
                    "Post-attach visual pose refresh failed for %s", object_id
                )
                pose = None
            if pose is not None:
                self._emit_scene_callback(
                    "remove", SceneEditRequest(operation="remove", object_id=object_id)
                )
                self._emit_scene_callback(
                    "add",
                    SceneEditRequest(
                        operation="add",
                        object_id=object_id,
                        **_shape_fields_from_registry(obj_info),
                        pose_position_m=list(pose["position_m"]),
                        pose_orientation_xyzw=list(pose["orientation_xyzw"]),
                        frame_id=link_frame,
                    ),
                )

        return py_trees.common.Status.SUCCESS


class DetachObjectFromLink(_SceneMutationNode):
    """Detach an object from a robot link and place it in the world frame.

    Delegates the authoritative detach/reparent operation to the backend.

    Params:
        object_id: object identifier to detach
        frame_id: target frame after detach (defaults to "universe")
    """

    _catalog_node_id = "detach_object_from_link"

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name, **kwargs)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(key="scene_objects", access=py_trees.common.Access.READ)
        self._register_scene_callback_key()

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        gated = self._runtime_pause_gate()
        if gated is not None:
            return gated
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
            **_shape_fields_from_registry(obj_info),
            frame_id=target_frame,
            action_span=self._current_action_span(self.blackboard),
        )
        try:
            result = kernel.scene_edit(request)
        except Exception as exc:  # noqa: BLE001 - backend runtime boundary.
            return self._scene_backend_failure(
                operation="detach",
                object_id=request.object_id,
                message=f"Scene detach failed: {exc}",
                details={"exception_type": type(exc).__name__},
            )
        if not result.success:
            self.logger.error(f"Failed to detach '{object_id}': {result.error}")
            return self._scene_backend_failure(
                operation="detach",
                object_id=request.object_id,
                message=result.error or f"Failed to detach '{object_id}'",
            )

        self._emit_scene_callback("detach", request)

        if obj_info:
            try:
                pose = kernel.get_object_pose(object_id, target_frame)
            except Exception:  # noqa: BLE001 - post-commit observer only.
                logger.exception(
                    "Post-detach visual pose refresh failed for %s", object_id
                )
                pose = None
            if pose is not None:
                self._emit_scene_callback(
                    "remove", SceneEditRequest(operation="remove", object_id=object_id)
                )
                self._emit_scene_callback(
                    "add",
                    SceneEditRequest(
                        operation="add",
                        object_id=object_id,
                        **_shape_fields_from_registry(obj_info),
                        pose_position_m=list(pose["position_m"]),
                        pose_orientation_xyzw=list(pose["orientation_xyzw"]),
                        frame_id=target_frame,
                    ),
                )

        return py_trees.common.Status.SUCCESS
