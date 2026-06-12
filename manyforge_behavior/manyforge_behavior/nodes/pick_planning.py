"""Deterministic pick-sequence planning nodes for perception-guided workflows."""

from __future__ import annotations

import itertools
import math
import time
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    KernelClient,
    SceneObjectSnapshot,
)
from manyforge_behavior.nodes.blackboard_output import BlackboardOutputMixin
from manyforge_behavior.parameters import resolve_blackboard_value


_SCENE_SNAPSHOT_WAIT_TIMEOUT_S = 2.0


# Canonical conversion is defined once in manyforge_behavior.geometry.
from manyforge_behavior.geometry import rpy_deg_to_xyzw as _rpy_deg_to_xyzw


def _default_pick_offset_envelope() -> dict[str, Any]:
    return {
        "position_min_m": [0.0, 0.0, 0.0],
        "position_max_m": [0.0, 0.0, 0.0],
        "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
        "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
    }


def _default_approach_offset_envelope() -> dict[str, Any]:
    return {
        "position_min_m": [0.0, 0.0, 0.05],
        "position_max_m": [0.0, 0.0, 0.10],
        "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
        "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
    }


def _axis_values(min_value: float, max_value: float, step: float) -> list[float]:
    if math.isclose(min_value, max_value, abs_tol=1e-9):
        return [float(min_value)]
    if min_value > max_value + 1e-9:
        raise ValueError(
            f"envelope minimum must be <= maximum (got {min_value} > {max_value})"
        )
    if step <= 0.0:
        raise ValueError("grid step must be > 0")
    values: list[float] = []
    current = min_value
    while current <= max_value + 1e-9:
        values.append(round(current, 9))
        current += step
    if not math.isclose(values[-1], max_value, abs_tol=1e-9):
        values.append(float(max_value))
    return values


def _iter_offset_vectors(
    envelope: dict[str, Any],
    *,
    translation_step_m: float,
    rotation_step_deg: float,
) -> list[tuple[list[float], list[float]]]:
    pos_min = list(envelope.get("position_min_m", [0.0, 0.0, 0.0]))
    pos_max = list(envelope.get("position_max_m", [0.0, 0.0, 0.0]))
    rot_min = list(envelope.get("orientation_rpy_min_deg", [0.0, 0.0, 0.0]))
    rot_max = list(envelope.get("orientation_rpy_max_deg", [0.0, 0.0, 0.0]))
    x_values = _axis_values(pos_min[0], pos_max[0], translation_step_m)
    y_values = _axis_values(pos_min[1], pos_max[1], translation_step_m)
    z_values = _axis_values(pos_min[2], pos_max[2], translation_step_m)
    r_values = _axis_values(rot_min[0], rot_max[0], rotation_step_deg)
    p_values = _axis_values(rot_min[1], rot_max[1], rotation_step_deg)
    yv_values = _axis_values(rot_min[2], rot_max[2], rotation_step_deg)
    return [
        ([x, y, z], [r, p, yaw])
        for x, y, z, r, p, yaw in itertools.product(
            x_values, y_values, z_values, r_values, p_values, yv_values
        )
    ]


def _shape_top_height(snapshot: SceneObjectSnapshot, object_pose: dict[str, Any]) -> float:
    base_z = float(object_pose["position_m"][2])
    shape = snapshot.shape
    if shape is None:
        return base_z
    if shape.type == "box" and shape.box_dimensions_m is not None:
        return base_z + float(shape.box_dimensions_m[2]) / 2.0
    if shape.type == "sphere" and shape.sphere_radius_m is not None:
        return base_z + float(shape.sphere_radius_m)
    return base_z


def _compose_pose(
    *,
    object_pose: dict[str, Any],
    object_top_z: float,
    base_orientation_rpy_deg: list[float],
    position_offset_m: list[float],
    orientation_offset_rpy_deg: list[float],
) -> dict[str, Any]:
    position = [
        float(object_pose["position_m"][0]) + float(position_offset_m[0]),
        float(object_pose["position_m"][1]) + float(position_offset_m[1]),
        float(object_top_z) + float(position_offset_m[2]),
    ]
    orientation_rpy_deg = [
        float(base_orientation_rpy_deg[0]) + float(orientation_offset_rpy_deg[0]),
        float(base_orientation_rpy_deg[1]) + float(orientation_offset_rpy_deg[1]),
        float(base_orientation_rpy_deg[2]) + float(orientation_offset_rpy_deg[2]),
    ]
    return {
        "position_m": position,
        "orientation_xyzw": _rpy_deg_to_xyzw(orientation_rpy_deg),
        "orientation_rpy_deg": orientation_rpy_deg,
    }


def _detail_flag(error_details: Optional[dict[str, Any]], key: str) -> bool:
    if not error_details:
        return False
    value = error_details.get(key)
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes"}
    return False


def _preview_failure_message(
    *,
    stage: str,
    preview_error: Optional[str],
    error_details: Optional[dict[str, Any]],
    attempted_candidates: int,
) -> str:
    parts = [f"{stage} preview failed on candidate {attempted_candidates}"]
    if preview_error:
        parts.append(preview_error)
    if error_details:
        failure_stage = error_details.get("failure_stage")
        if failure_stage:
            parts.append(f"native_stage={failure_stage}")
        if _detail_flag(error_details, "start_in_collision"):
            parts.append("start_state_in_collision=true")
        if _detail_flag(error_details, "goal_in_collision"):
            parts.append("goal_in_collision=true")
        contact_a = error_details.get("contact_body_a")
        contact_b = error_details.get("contact_body_b")
        if contact_a or contact_b:
            parts.append(
                f"contact=({contact_a or 'unknown'}, {contact_b or 'unknown'})"
            )
    return " | ".join(parts)


class PlanPickSequenceNode(BlackboardOutputMixin, py_trees.behaviour.Behaviour):
    """Deterministically generate and preview a three-leg pick sequence."""

    def __init__(
        self,
        name: str,
        *,
        selected_object_id: str,
        output_key_prefix: str,
        object_source: str = "backend_scene",
        image_source: str = "none",
        image_snapshot_width: int = 640,
        image_snapshot_height: int = 480,
        image_snapshot_format: str = "jpeg",
        tcp_frame: str = "tool0",
        pick_offset_envelope: Optional[dict[str, Any]] = None,
        approach_offset_envelope: Optional[dict[str, Any]] = None,
        search_strategy: str = "first_feasible_grid",
        translation_step_m: float = 0.01,
        rotation_step_deg: float = 15.0,
        planning_timeout_s: float = 2.0,
        velocity_scale: float = 0.35,
        acceleration_scale: float = 1.0,
        max_tcp_linear_speed_mps: Optional[float] = None,
        robot_id: str = "",
        group_id: str = "",
        base_frame: str = "",
        scene_snapshot_provider: Optional[
            Callable[[int, int, str], Optional[bytes]]
        ] = None,
    ):
        super().__init__(name=name)
        self._selected_object_id = selected_object_id
        self._output_key_prefix = output_key_prefix
        self._object_source = object_source
        self._image_source = image_source
        self._image_snapshot_width = image_snapshot_width
        self._image_snapshot_height = image_snapshot_height
        self._image_snapshot_format = image_snapshot_format
        self._tcp_frame = tcp_frame
        self._pick_offset_envelope = dict(
            pick_offset_envelope or _default_pick_offset_envelope()
        )
        self._approach_offset_envelope = dict(
            approach_offset_envelope or _default_approach_offset_envelope()
        )
        self._search_strategy = search_strategy
        self._translation_step_m = translation_step_m
        self._rotation_step_deg = rotation_step_deg
        self._planning_timeout_s = planning_timeout_s
        self._velocity_scale = velocity_scale
        self._acceleration_scale = acceleration_scale
        self._max_tcp_linear_speed_mps = max_tcp_linear_speed_mps
        self._robot_id = robot_id
        self._group_id = group_id
        self._base_frame = base_frame
        self._scene_snapshot_provider = scene_snapshot_provider
        self._bb = py_trees.blackboard.Client(name=f"plan_pick_sequence_{name}")
        self._output_keys_registered = False
        self._awaiting_scene_snapshot = False
        self._scene_snapshot_wait_started_ns = 0

    def _ensure_output_keys(self) -> None:
        if self._output_keys_registered:
            return
        self._bb.register_key(
            key=self._output_key_prefix,
            access=py_trees.common.Access.WRITE,
        )
        self._bb.register_key(key="kernel", access=py_trees.common.Access.READ)
        self._bb.register_key(
            key="pose_source_apply_callback",
            access=py_trees.common.Access.READ,
        )
        self._bb.register_key(
            key="pose_source_between_nodes_callback",
            access=py_trees.common.Access.READ,
        )
        self._output_keys_registered = True

    def _apply_pose_source_latches(
        self,
        *,
        selected_object_id: str,
    ) -> py_trees.common.Status | None:
        callbacks = (
            ("pose_source_apply_callback", "before-planning"),
            ("pose_source_between_nodes_callback", "between-nodes"),
        )
        for blackboard_key, label in callbacks:
            try:
                callback = self._bb.get(blackboard_key)
            except (AttributeError, KeyError):
                callback = None
            if not callable(callback):
                continue
            try:
                result = callback(
                    object_ids=[selected_object_id],
                    reason=f"pick_sequence:{self.name}",
                )
            except Exception as exc:  # noqa: BLE001 - callback boundary
                self.feedback_message = f"{label} pose-source apply raised: {exc}"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="pose_source_apply_failed",
                    failure_detail=f"{label}: {exc}",
                )
                return py_trees.common.Status.FAILURE
            if not result.get("success", False):
                error = str(result.get("error") or f"{label} pose-source apply failed")
                self.feedback_message = error
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="pose_source_apply_failed",
                    failure_detail=f"{label}: {error}",
                )
                return py_trees.common.Status.FAILURE
        return None

    def initialise(self) -> None:
        self._ensure_output_keys()
        self._awaiting_scene_snapshot = False
        self._scene_snapshot_wait_started_ns = 0

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self._bb.kernel
        try:
            selected_object_id = str(
                resolve_blackboard_value(self._bb, self._selected_object_id)
            )
        except (AttributeError, KeyError) as exc:
            self.feedback_message = str(exc)
            self._write_failure_response(
                selected_object_id=None,
                attempted_candidates=0,
                outcome="selected_object_unavailable",
            )
            return py_trees.common.Status.FAILURE

        if not selected_object_id:
            self.feedback_message = "selected_object_id must not be empty"
            self._write_failure_response(
                selected_object_id=None,
                attempted_candidates=0,
                outcome="selected_object_empty",
            )
            return py_trees.common.Status.FAILURE

        if self._object_source != "backend_scene":
            self.feedback_message = f"Unsupported object_source: {self._object_source}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="unsupported_object_source",
            )
            return py_trees.common.Status.FAILURE

        if self._search_strategy not in {"first_feasible_grid", "best_cost_grid"}:
            self.feedback_message = (
                f"Unsupported search_strategy: {self._search_strategy}"
            )
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="unsupported_search_strategy",
            )
            return py_trees.common.Status.FAILURE

        pose_source_status = self._apply_pose_source_latches(
            selected_object_id=selected_object_id
        )
        if pose_source_status is not None:
            return pose_source_status

        if self._image_source == "viewer_snapshot":
            provider = self._scene_snapshot_provider
            if provider is None:
                self.feedback_message = (
                    "image_source=viewer_snapshot requested but no scene snapshot provider is configured"
                )
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="image_source_unavailable",
                )
                return py_trees.common.Status.FAILURE
            try:
                snapshot_bytes = provider(
                    self._image_snapshot_width,
                    self._image_snapshot_height,
                    self._image_snapshot_format,
                )
            except Exception as exc:
                self.feedback_message = f"image_snapshot_unavailable: {exc}"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="image_snapshot_unavailable",
                )
                return py_trees.common.Status.FAILURE
            if snapshot_bytes is None:
                if self._scene_snapshot_wait_started_ns == 0:
                    self._scene_snapshot_wait_started_ns = time.monotonic_ns()
                elapsed_s = (
                    time.monotonic_ns() - self._scene_snapshot_wait_started_ns
                ) / 1_000_000_000
                if elapsed_s >= _SCENE_SNAPSHOT_WAIT_TIMEOUT_S:
                    self.feedback_message = (
                        f"image_snapshot_unavailable_after_{_SCENE_SNAPSHOT_WAIT_TIMEOUT_S:.1f}s"
                    )
                    self._write_failure_response(
                        selected_object_id=selected_object_id,
                        attempted_candidates=0,
                        outcome="image_snapshot_unavailable",
                    )
                    return py_trees.common.Status.FAILURE
                self._awaiting_scene_snapshot = True
                self.feedback_message = "waiting_for_scene_snapshot"
                self._write_namespace_output(
                    None,
                    status="waiting_for_scene_snapshot",
                )
                return py_trees.common.Status.RUNNING
            self._awaiting_scene_snapshot = False
            self._scene_snapshot_wait_started_ns = 0
            # The current deterministic planner does not consume the image yet,
            # but successful acquisition still exercises the image-backed path.
        elif self._image_source != "none":
            self.feedback_message = f"Unsupported image_source: {self._image_source}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="unsupported_image_source",
            )
            return py_trees.common.Status.FAILURE

        scene_snapshot = next(
            (obj for obj in kernel.get_scene_objects() if obj.object_id == selected_object_id),
            None,
        )
        object_pose = kernel.get_object_pose(selected_object_id, self._base_frame)
        if scene_snapshot is None or object_pose is None:
            self.feedback_message = f"Object '{selected_object_id}' not found in backend scene"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="object_not_found",
            )
            return py_trees.common.Status.FAILURE

        object_top_z = _shape_top_height(scene_snapshot, object_pose)
        base_orientation = [180.0, 0.0, 0.0]
        try:
            pick_offsets = _iter_offset_vectors(
                self._pick_offset_envelope,
                translation_step_m=self._translation_step_m,
                rotation_step_deg=self._rotation_step_deg,
            )
        except ValueError as exc:
            self.feedback_message = f"Invalid pick_offset_envelope: {exc}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="invalid_pick_offset_envelope",
            )
            return py_trees.common.Status.FAILURE

        try:
            approach_offsets = _iter_offset_vectors(
                self._approach_offset_envelope,
                translation_step_m=self._translation_step_m,
                rotation_step_deg=self._rotation_step_deg,
            )
        except ValueError as exc:
            self.feedback_message = f"Invalid approach_offset_envelope: {exc}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="invalid_approach_offset_envelope",
            )
            return py_trees.common.Status.FAILURE

        attempted_candidates = 0
        best_candidate: Optional[dict[str, Any]] = None
        last_failure_detail: Optional[str] = None

        for (pick_pos_offset, pick_rot_offset) in pick_offsets:
            pick_pose = _compose_pose(
                object_pose=object_pose,
                object_top_z=object_top_z,
                base_orientation_rpy_deg=base_orientation,
                position_offset_m=pick_pos_offset,
                orientation_offset_rpy_deg=pick_rot_offset,
            )
            for (approach_pos_offset, approach_rot_offset) in approach_offsets:
                attempted_candidates += 1
                approach_pose = {
                    "position_m": [
                        pick_pose["position_m"][0] + float(approach_pos_offset[0]),
                        pick_pose["position_m"][1] + float(approach_pos_offset[1]),
                        pick_pose["position_m"][2] + float(approach_pos_offset[2]),
                    ],
                    "orientation_rpy_deg": [
                        pick_pose["orientation_rpy_deg"][0] + float(approach_rot_offset[0]),
                        pick_pose["orientation_rpy_deg"][1] + float(approach_rot_offset[1]),
                        pick_pose["orientation_rpy_deg"][2] + float(approach_rot_offset[2]),
                    ],
                }
                approach_pose["orientation_xyzw"] = _rpy_deg_to_xyzw(
                    approach_pose["orientation_rpy_deg"]
                )

                approach_preview = kernel.preview_motion(
                    self._make_pose_goal_request("pick_approach_preview", approach_pose)
                )
                if not approach_preview.ok:
                    last_failure_detail = _preview_failure_message(
                        stage="approach",
                        preview_error=approach_preview.error,
                        error_details=approach_preview.error_details,
                        attempted_candidates=attempted_candidates,
                    )
                    if _detail_flag(
                        approach_preview.error_details, "start_in_collision"
                    ):
                        self.feedback_message = last_failure_detail
                        self._write_failure_response(
                            selected_object_id=selected_object_id,
                            attempted_candidates=attempted_candidates,
                            outcome="start_state_in_collision",
                            failure_detail=last_failure_detail,
                        )
                        return py_trees.common.Status.FAILURE
                    continue

                pick_preview = kernel.preview_motion(
                    self._make_pose_goal_request("pick_descend_preview", pick_pose),
                    start_state=approach_preview.final_state,
                )
                if not pick_preview.ok:
                    last_failure_detail = _preview_failure_message(
                        stage="pick",
                        preview_error=pick_preview.error,
                        error_details=pick_preview.error_details,
                        attempted_candidates=attempted_candidates,
                    )
                    continue

                exit_preview = kernel.preview_motion(
                    self._make_pose_goal_request("pick_exit_preview", approach_pose),
                    start_state=pick_preview.final_state,
                )
                if not exit_preview.ok:
                    last_failure_detail = _preview_failure_message(
                        stage="exit",
                        preview_error=exit_preview.error,
                        error_details=exit_preview.error_details,
                        attempted_candidates=attempted_candidates,
                    )
                    continue

                duration_s = (
                    float(approach_preview.trajectory_duration_s or 0.0)
                    + float(pick_preview.trajectory_duration_s or 0.0)
                    + float(exit_preview.trajectory_duration_s or 0.0)
                )
                waypoint_count = (
                    int(approach_preview.trajectory_waypoint_count or 0)
                    + int(pick_preview.trajectory_waypoint_count or 0)
                    + int(exit_preview.trajectory_waypoint_count or 0)
                )
                planning_score = duration_s + (0.01 * waypoint_count)
                candidate = {
                    "selected_object_id": selected_object_id,
                    "approach_pose": {
                        "position_m": list(approach_pose["position_m"]),
                        "orientation_xyzw": list(approach_pose["orientation_xyzw"]),
                    },
                    "pick_pose": {
                        "position_m": list(pick_pose["position_m"]),
                        "orientation_xyzw": list(pick_pose["orientation_xyzw"]),
                    },
                    "attempted_candidates": attempted_candidates,
                    "planning_score": planning_score,
                    "outcome": "planned",
                }
                if self._search_strategy == "first_feasible_grid":
                    self._write_response(candidate)
                    return py_trees.common.Status.SUCCESS
                if best_candidate is None or planning_score < float(
                    best_candidate["planning_score"]
                ):
                    best_candidate = candidate

        if best_candidate is not None:
            self._write_response(best_candidate)
            return py_trees.common.Status.SUCCESS

        self.feedback_message = (
            f"No feasible pick sequence found for '{selected_object_id}' "
            f"after {attempted_candidates} candidate(s)"
        )
        if last_failure_detail:
            self.feedback_message = (
                f"{self.feedback_message}. Last failure: {last_failure_detail}"
            )
        self._write_failure_response(
            selected_object_id=selected_object_id,
            attempted_candidates=attempted_candidates,
            outcome="no_feasible_plan",
            failure_detail=last_failure_detail,
        )
        return py_trees.common.Status.FAILURE

    def _make_pose_goal_request(self, move_id: str, pose: dict[str, Any]) -> ExecuteRequest:
        return ExecuteRequest(
            robot_id=self._robot_id,
            group_id=self._group_id,
            move_id=move_id,
            base_frame=self._base_frame,
            tcp_frame=self._tcp_frame,
            motion_type="pose_goal",
            pose_goal={
                "position_m": list(pose["position_m"]),
                "orientation_xyzw": list(pose["orientation_xyzw"]),
            },
            velocity_scale=self._velocity_scale,
            acceleration_scale=self._acceleration_scale,
            max_tcp_linear_speed_mps=self._max_tcp_linear_speed_mps,
            planning_timeout_s=self._planning_timeout_s,
            execution_timeout_s=0.0,
            cache_mode="disable",
        )

    def _write_response(self, response: dict[str, Any]) -> None:
        self._write_namespace_output(response, status="planned")

    def _write_failure_response(
        self,
        *,
        selected_object_id: Optional[str],
        attempted_candidates: int,
        outcome: str,
        failure_detail: Optional[str] = None,
    ) -> None:
        response = {
            "selected_object_id": selected_object_id,
            "approach_pose": None,
            "pick_pose": None,
            "attempted_candidates": attempted_candidates,
            "planning_score": None,
            "outcome": outcome,
            "failure_detail": failure_detail,
        }
        self._write_namespace_output(response, status="failed")
