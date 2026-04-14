"""Deterministic scene-selection context and validation nodes."""

from __future__ import annotations

import hashlib
import json
import math
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import KernelClient, SceneObjectSnapshot
from manyforge_behavior.parameters import BLACKBOARD_REF_RE


def _resolve_blackboard_reference(
    blackboard: py_trees.blackboard.Client, value: Any
) -> Any:
    if isinstance(value, str):
        match = BLACKBOARD_REF_RE.match(value)
        if match is None:
            return value
        key = match.group(1)
        blackboard.register_key(key=key, access=py_trees.common.Access.READ)
        if not blackboard.exists(key):
            raise KeyError(f"Missing blackboard key '{key}'")
        return blackboard.get(key)
    if isinstance(value, list):
        return [_resolve_blackboard_reference(blackboard, item) for item in value]
    if isinstance(value, dict):
        return {
            item_key: _resolve_blackboard_reference(blackboard, item_value)
            for item_key, item_value in value.items()
        }
    return value


def _xyzw_to_rpy_deg(orientation_xyzw: list[float]) -> list[float]:
    x, y, z, w = [float(v) for v in orientation_xyzw]
    sinr_cosp = 2.0 * (w * x + y * z)
    cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
    roll = math.atan2(sinr_cosp, cosr_cosp)

    sinp = 2.0 * (w * y - z * x)
    if abs(sinp) >= 1.0:
        pitch = math.copysign(math.pi / 2.0, sinp)
    else:
        pitch = math.asin(sinp)

    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw = math.atan2(siny_cosp, cosy_cosp)
    return [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)]


def _shape_summary(snapshot: SceneObjectSnapshot) -> dict[str, Any]:
    shape = snapshot.shape
    if shape is None:
        return {"type": "unknown"}
    summary: dict[str, Any] = {"type": shape.type}
    if shape.box_dimensions_m is not None:
        dims = [float(v) for v in shape.box_dimensions_m]
        summary["box_dimensions_m"] = dims
        summary["max_dimension_m"] = max(dims)
        summary["min_dimension_m"] = min(dims)
    if shape.sphere_radius_m is not None:
        radius = float(shape.sphere_radius_m)
        summary["sphere_radius_m"] = radius
        summary["diameter_m"] = radius * 2.0
        summary["max_dimension_m"] = radius * 2.0
        summary["min_dimension_m"] = radius * 2.0
    if shape.cylinder_radius_m is not None:
        summary["cylinder_radius_m"] = float(shape.cylinder_radius_m)
    if shape.cylinder_height_m is not None:
        summary["cylinder_height_m"] = float(shape.cylinder_height_m)
    if shape.mesh_resource_uri is not None:
        summary["mesh_resource_uri"] = str(shape.mesh_resource_uri)
    if shape.mesh_scale is not None:
        summary["mesh_scale"] = [float(v) for v in shape.mesh_scale]
    return summary


def _shape_top_height(snapshot: SceneObjectSnapshot, object_pose: dict[str, Any]) -> float:
    base_z = float(object_pose["position_m"][2])
    shape = snapshot.shape
    if shape is None:
        return base_z
    if shape.type == "box" and shape.box_dimensions_m is not None:
        return base_z + float(shape.box_dimensions_m[2]) / 2.0
    if shape.type == "sphere" and shape.sphere_radius_m is not None:
        return base_z + float(shape.sphere_radius_m)
    if shape.type == "cylinder" and shape.cylinder_height_m is not None:
        return base_z + float(shape.cylinder_height_m) / 2.0
    return base_z


def _flatten_output_fields(prefix: str, value: Any) -> list[tuple[str, Any]]:
    if isinstance(value, dict):
        items: list[tuple[str, Any]] = [(prefix, value)]
        for key, child in value.items():
            items.extend(_flatten_output_fields(f"{prefix}/{key}", child))
        return items
    if isinstance(value, list):
        items: list[tuple[str, Any]] = [(prefix, value)]
        for index, child in enumerate(value):
            items.extend(_flatten_output_fields(f"{prefix}/{index}", child))
        return items
    return [(prefix, value)]


class _BlackboardOutputMixin:
    _bb: py_trees.blackboard.Client
    _output_key_prefix: str

    def _write_output(self, key: str, value: Any) -> None:
        self._bb.set(f"{self._output_key_prefix}/{key}", value)

    def _write_output_fields(self, parsed: Any) -> None:
        previous_keys = self._read_previous_field_keys()
        field_items: list[tuple[str, Any]] = []
        if isinstance(parsed, (dict, list)):
            field_items = list(
                _flatten_output_fields(
                    f"{self._output_key_prefix}/fields",
                    parsed,
                )
            )

        next_keys = [key for key, _ in field_items]
        for key in previous_keys:
            if key not in next_keys:
                py_trees.blackboard.Blackboard.unset(key)

        for key, value in field_items:
            py_trees.blackboard.Blackboard.set(key, value)

        self._write_output("field_keys", next_keys)

    def _read_previous_field_keys(self) -> list[str]:
        try:
            value = self._bb.get(f"{self._output_key_prefix}/field_keys")
        except KeyError:
            return []
        if isinstance(value, list):
            return [str(item) for item in value]
        return []


class BuildSceneSelectionContextNode(_BlackboardOutputMixin, py_trees.behaviour.Behaviour):
    """Build an authoritative object-selection context from backend scene state."""

    def __init__(
        self,
        name: str,
        *,
        selection_request_text: str,
        output_key_prefix: str,
        object_source: str = "backend_scene",
        candidate_object_ids: Optional[list[str]] = None,
        image_source: str = "none",
        image_snapshot_width: int = 640,
        image_snapshot_height: int = 480,
        image_snapshot_format: str = "jpeg",
        base_frame: str = "",
        scene_snapshot_provider: Optional[
            Callable[[int, int, str], Optional[bytes]]
        ] = None,
    ):
        super().__init__(name=name)
        self._selection_request_text = selection_request_text
        self._output_key_prefix = output_key_prefix
        self._object_source = object_source
        self._candidate_object_ids = candidate_object_ids if candidate_object_ids is not None else []
        self._image_source = image_source
        self._image_snapshot_width = image_snapshot_width
        self._image_snapshot_height = image_snapshot_height
        self._image_snapshot_format = image_snapshot_format
        self._base_frame = base_frame
        self._scene_snapshot_provider = scene_snapshot_provider
        self._bb = py_trees.blackboard.Client(name=f"build_scene_selection_context_{name}")
        self._output_keys_registered = False

    def _ensure_output_keys(self) -> None:
        if self._output_keys_registered:
            return
        for key in (
            f"{self._output_key_prefix}/response",
            f"{self._output_key_prefix}/status",
            f"{self._output_key_prefix}/field_keys",
        ):
            self._bb.register_key(key=key, access=py_trees.common.Access.WRITE)
        self._bb.register_key(key="kernel", access=py_trees.common.Access.READ)
        self._output_keys_registered = True

    def initialise(self) -> None:
        self._ensure_output_keys()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self._bb.kernel
        try:
            selection_request_text = str(
                _resolve_blackboard_reference(self._bb, self._selection_request_text)
            )
            candidate_object_ids = _resolve_blackboard_reference(
                self._bb, self._candidate_object_ids
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            self._write_failure_response(
                selection_request_text=None,
                candidate_count=0,
                outcome="selection_request_unavailable",
            )
            return py_trees.common.Status.FAILURE

        if not selection_request_text.strip():
            self.feedback_message = "selection_request_text must not be empty"
            self._write_failure_response(
                selection_request_text="",
                candidate_count=0,
                outcome="selection_request_empty",
            )
            return py_trees.common.Status.FAILURE

        if self._object_source != "backend_scene":
            self.feedback_message = f"Unsupported object_source: {self._object_source}"
            self._write_failure_response(
                selection_request_text=selection_request_text,
                candidate_count=0,
                outcome="unsupported_object_source",
            )
            return py_trees.common.Status.FAILURE

        if candidate_object_ids and not isinstance(candidate_object_ids, list):
            self.feedback_message = "candidate_object_ids must resolve to a list"
            self._write_failure_response(
                selection_request_text=selection_request_text,
                candidate_count=0,
                outcome="invalid_candidate_filter",
            )
            return py_trees.common.Status.FAILURE

        allowed_object_ids = {
            str(object_id) for object_id in (candidate_object_ids or []) if str(object_id)
        }
        scene_objects = kernel.get_scene_objects()
        candidates_raw = [
            snapshot
            for snapshot in scene_objects
            if not allowed_object_ids or snapshot.object_id in allowed_object_ids
        ]

        if not candidates_raw:
            self.feedback_message = "No candidate scene objects available"
            self._write_failure_response(
                selection_request_text=selection_request_text,
                candidate_count=0,
                outcome="no_candidates",
            )
            return py_trees.common.Status.FAILURE

        image_context: dict[str, Any] = {
            "source": self._image_source,
            "role": "auxiliary" if self._image_source == "hmi_snapshot" else "none",
            "is_canonical_view": False,
            "view_semantics": (
                "current_hmi_view_may_be_user_controlled"
                if self._image_source == "hmi_snapshot"
                else "no_image_used"
            ),
        }
        if self._image_source == "hmi_snapshot":
            provider = self._scene_snapshot_provider
            if provider is None:
                self.feedback_message = (
                    "image_source=hmi_snapshot requested but no scene snapshot provider is configured"
                )
                self._write_failure_response(
                    selection_request_text=selection_request_text,
                    candidate_count=0,
                    outcome="image_source_unavailable",
                )
                return py_trees.common.Status.FAILURE
            snapshot_bytes = provider(
                self._image_snapshot_width,
                self._image_snapshot_height,
                self._image_snapshot_format,
            )
            if snapshot_bytes is None:
                self.feedback_message = "image_source=hmi_snapshot requested but no render is available"
                self._write_failure_response(
                    selection_request_text=selection_request_text,
                    candidate_count=0,
                    outcome="image_snapshot_unavailable",
                )
                return py_trees.common.Status.FAILURE
            image_context.update(
                {
                    "available": True,
                    "format": self._image_snapshot_format,
                    "width": self._image_snapshot_width,
                    "height": self._image_snapshot_height,
                    "sha256": hashlib.sha256(snapshot_bytes).hexdigest(),
                }
            )
        elif self._image_source != "none":
            self.feedback_message = f"Unsupported image_source: {self._image_source}"
            self._write_failure_response(
                selection_request_text=selection_request_text,
                candidate_count=0,
                outcome="unsupported_image_source",
            )
            return py_trees.common.Status.FAILURE

        candidates: list[dict[str, Any]] = []
        for snapshot in sorted(candidates_raw, key=lambda item: item.object_id):
            pose = kernel.get_object_pose(snapshot.object_id, self._base_frame)
            if pose is None:
                continue
            orientation_xyzw = list(pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]))
            candidate = {
                "object_id": snapshot.object_id,
                "shape": _shape_summary(snapshot),
                "pose": {
                    "frame_id": self._base_frame,
                    "position_m": [float(v) for v in pose.get("position_m", [0.0, 0.0, 0.0])],
                    "orientation_xyzw": orientation_xyzw,
                    "orientation_rpy_deg": _xyzw_to_rpy_deg(orientation_xyzw),
                },
                "attached": bool(snapshot.attached),
                "frame_id": snapshot.frame_id or "universe",
                "top_z_m": _shape_top_height(snapshot, pose),
            }
            candidate["spatial"] = {
                "distance_from_base_xy_m": math.hypot(
                    candidate["pose"]["position_m"][0],
                    candidate["pose"]["position_m"][1],
                ),
                "distance_from_base_m": math.sqrt(
                    candidate["pose"]["position_m"][0] ** 2
                    + candidate["pose"]["position_m"][1] ** 2
                    + candidate["pose"]["position_m"][2] ** 2
                ),
            }
            candidates.append(candidate)

        if not candidates:
            self.feedback_message = "No candidate scene objects resolved in base frame"
            self._write_failure_response(
                selection_request_text=selection_request_text,
                candidate_count=0,
                outcome="no_resolved_candidates",
            )
            return py_trees.common.Status.FAILURE

        by_y_desc = [candidate["object_id"] for candidate in sorted(
            candidates, key=lambda item: item["pose"]["position_m"][1], reverse=True
        )]
        by_x_desc = [candidate["object_id"] for candidate in sorted(
            candidates, key=lambda item: item["pose"]["position_m"][0], reverse=True
        )]
        by_z_desc = [candidate["object_id"] for candidate in sorted(
            candidates, key=lambda item: item["top_z_m"], reverse=True
        )]

        context = {
            "selection_request_text": selection_request_text,
            "authoritative_source": self._object_source,
            "base_frame": self._base_frame,
            "spatial_semantics": {
                "frame_id": self._base_frame,
                "assumptions": [
                    "object_ids_and_poses_come_from_backend_scene_and_are_authoritative",
                    "left_means_greater_positive_y_in_base_frame",
                    "right_means_more_negative_y_in_base_frame",
                    "front_means_greater_positive_x_in_base_frame",
                    "back_means_more_negative_x_in_base_frame",
                    "up_means_greater_positive_z_in_base_frame",
                    "down_means_more_negative_z_in_base_frame",
                    "if_an_image_is_attached_it_is_auxiliary_evidence_only",
                ],
                "rankings": {
                    "left_to_right_by_base_y_desc": by_y_desc,
                    "front_to_back_by_base_x_desc": by_x_desc,
                    "high_to_low_by_top_z_desc": by_z_desc,
                },
            },
            "image_context": image_context,
            "candidate_object_ids": [candidate["object_id"] for candidate in candidates],
            "candidate_count": len(candidates),
            "candidates": candidates,
        }
        response = dict(context)
        response["scene_context_json"] = json.dumps(context, indent=2, sort_keys=False)

        self.feedback_message = (
            f"Built selection context with {len(candidates)} candidate object(s)"
        )
        self._write_output("response", response)
        self._write_output("status", "built")
        self._write_output_fields(response)
        return py_trees.common.Status.SUCCESS

    def _write_failure_response(
        self,
        *,
        selection_request_text: Optional[str],
        candidate_count: int,
        outcome: str,
    ) -> None:
        response = {
            "selection_request_text": selection_request_text,
            "candidate_count": candidate_count,
            "outcome": outcome,
        }
        self._write_output("response", response)
        self._write_output("status", "failed")
        self._write_output_fields(response)


class ValidateSceneSelectionNode(_BlackboardOutputMixin, py_trees.behaviour.Behaviour):
    """Validate a model-selected object against a deterministic candidate manifest."""

    def __init__(
        self,
        name: str,
        *,
        selection_context_prefix: str,
        selection_response_prefix: str,
        output_key_prefix: str,
        allow_no_match: bool = False,
        allow_ambiguous: bool = False,
    ):
        super().__init__(name=name)
        self._selection_context_prefix = selection_context_prefix
        self._selection_response_prefix = selection_response_prefix
        self._output_key_prefix = output_key_prefix
        self._allow_no_match = allow_no_match
        self._allow_ambiguous = allow_ambiguous
        self._bb = py_trees.blackboard.Client(name=f"validate_scene_selection_{name}")
        self._output_keys_registered = False

    def _ensure_output_keys(self) -> None:
        if self._output_keys_registered:
            return
        for key in (
            f"{self._output_key_prefix}/response",
            f"{self._output_key_prefix}/status",
            f"{self._output_key_prefix}/field_keys",
        ):
            self._bb.register_key(key=key, access=py_trees.common.Access.WRITE)
        self._bb.register_key(
            key=f"{self._selection_context_prefix}/fields",
            access=py_trees.common.Access.READ,
        )
        self._bb.register_key(
            key=f"{self._selection_response_prefix}/fields",
            access=py_trees.common.Access.READ,
        )
        self._output_keys_registered = True

    def initialise(self) -> None:
        self._ensure_output_keys()

    def update(self) -> py_trees.common.Status:
        try:
            context = self._bb.get(f"{self._selection_context_prefix}/fields")
            response = self._bb.get(f"{self._selection_response_prefix}/fields")
        except KeyError as exc:
            self.feedback_message = str(exc)
            self._write_failure_response(
                outcome="selection_input_unavailable",
                selected_object_id=None,
                selected_candidate=None,
                confidence=0.0,
                reason=str(exc),
                matched_attributes=[],
            )
            return py_trees.common.Status.FAILURE

        if not isinstance(context, dict):
            self.feedback_message = "selection context is not a structured object"
            self._write_failure_response(
                outcome="invalid_selection_context",
                selected_object_id=None,
                selected_candidate=None,
                confidence=0.0,
                reason="selection context is not a structured object",
                matched_attributes=[],
            )
            return py_trees.common.Status.FAILURE
        if not isinstance(response, dict):
            self.feedback_message = "selection response is not a structured object"
            self._write_failure_response(
                outcome="invalid_selection_response",
                selected_object_id=None,
                selected_candidate=None,
                confidence=0.0,
                reason="selection response is not a structured object",
                matched_attributes=[],
            )
            return py_trees.common.Status.FAILURE

        outcome = str(response.get("outcome", "") or "")
        selected_object_id = str(response.get("selected_object_id", "") or "")
        reason = str(response.get("reason", "") or "")
        confidence_raw = response.get("confidence", 0.0)
        confidence = (
            float(confidence_raw)
            if isinstance(confidence_raw, (int, float)) and not isinstance(confidence_raw, bool)
            else 0.0
        )
        matched_attributes = response.get("matched_attributes", [])
        if not isinstance(matched_attributes, list):
            matched_attributes = []
        matched_attributes = [str(item) for item in matched_attributes]

        candidates = context.get("candidates", [])
        if not isinstance(candidates, list):
            candidates = []
        candidate_by_id = {
            str(candidate.get("object_id")): candidate
            for candidate in candidates
            if isinstance(candidate, dict) and candidate.get("object_id")
        }

        if outcome not in {"selected", "no_match", "ambiguous"}:
            self.feedback_message = f"Unsupported selection outcome: {outcome!r}"
            self._write_failure_response(
                outcome="invalid_selection_outcome",
                selected_object_id=selected_object_id or None,
                selected_candidate=None,
                confidence=confidence,
                reason=reason or f"Unsupported outcome {outcome!r}",
                matched_attributes=matched_attributes,
            )
            return py_trees.common.Status.FAILURE

        if outcome == "no_match":
            self.feedback_message = reason or "Selection returned no_match"
            self._write_failure_response(
                outcome="no_match",
                selected_object_id=None,
                selected_candidate=None,
                confidence=confidence,
                reason=reason,
                matched_attributes=matched_attributes,
            )
            return py_trees.common.Status.SUCCESS if self._allow_no_match else py_trees.common.Status.FAILURE

        if outcome == "ambiguous":
            self.feedback_message = reason or "Selection returned ambiguous"
            self._write_failure_response(
                outcome="ambiguous",
                selected_object_id=None,
                selected_candidate=None,
                confidence=confidence,
                reason=reason,
                matched_attributes=matched_attributes,
            )
            return py_trees.common.Status.SUCCESS if self._allow_ambiguous else py_trees.common.Status.FAILURE

        if not selected_object_id:
            self.feedback_message = "selected outcome requires selected_object_id"
            self._write_failure_response(
                outcome="selected_object_id_empty",
                selected_object_id=None,
                selected_candidate=None,
                confidence=confidence,
                reason=reason,
                matched_attributes=matched_attributes,
            )
            return py_trees.common.Status.FAILURE

        selected_candidate = candidate_by_id.get(selected_object_id)
        if selected_candidate is None:
            self.feedback_message = (
                f"selected_object_id '{selected_object_id}' is not in the candidate set"
            )
            self._write_failure_response(
                outcome="selected_object_not_in_context",
                selected_object_id=selected_object_id,
                selected_candidate=None,
                confidence=confidence,
                reason=reason,
                matched_attributes=matched_attributes,
            )
            return py_trees.common.Status.FAILURE

        validated = {
            "outcome": "selected",
            "selected_object_id": selected_object_id,
            "selected_candidate": selected_candidate,
            "candidate_count": len(candidate_by_id),
            "selection_request_text": context.get("selection_request_text"),
            "matched_attributes": matched_attributes,
            "confidence": confidence,
            "reason": reason,
        }
        self.feedback_message = f"Validated selection of '{selected_object_id}'"
        self._write_output("response", validated)
        self._write_output("status", "validated")
        self._write_output_fields(validated)
        return py_trees.common.Status.SUCCESS

    def _write_failure_response(
        self,
        *,
        outcome: str,
        selected_object_id: Optional[str],
        selected_candidate: Optional[dict[str, Any]],
        confidence: float,
        reason: str,
        matched_attributes: list[str],
    ) -> None:
        response = {
            "outcome": outcome,
            "selected_object_id": selected_object_id,
            "selected_candidate": selected_candidate,
            "confidence": confidence,
            "reason": reason,
            "matched_attributes": matched_attributes,
        }
        self._write_output("response", response)
        self._write_output("status", "failed")
        self._write_output_fields(response)
