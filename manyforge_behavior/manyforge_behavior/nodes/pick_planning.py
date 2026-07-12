"""Deterministic pick-sequence planning nodes for perception-guided workflows."""

from __future__ import annotations

import copy
import itertools
import math
import time
from typing import Any, Callable, Optional

import py_trees

from manyforge_behavior.action_span import ActionSpanMixin, read_program_run_id
from manyforge_behavior.geometry import (
    normalize_quat as _normalize_quat,
    quat_mul as _quat_mul,
    quat_rotate_vector as _quat_rotate_vector,
    quats_close as _quats_close,
    rpy_deg_to_xyzw as _rpy_deg_to_xyzw,
    xyzw_to_rpy_deg as _xyzw_to_rpy_deg,
)
from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecuteRequest,
    KernelClient,
    MotionPreviewResult,
    SceneObjectSnapshot,
)
from manyforge_behavior.nodes.blackboard_output import BlackboardOutputMixin
from manyforge_behavior.parameters import resolve_blackboard_value
from manyforge_behavior.reason_codes import ReasonCode
from manyforge_behavior.runtime_pause import RuntimePauseMixin, RuntimePauseRegistry


_SCENE_SNAPSHOT_WAIT_TIMEOUT_S = 2.0
_VECTOR_EPSILON = 1e-9


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


def _default_offset_reference(base_frame: str) -> dict[str, str]:
    return {"type": "frame", "id": base_frame}


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
    include_rotation: bool = True,
) -> list[tuple[list[float], list[float]]]:
    pos_min = list(envelope.get("position_min_m", [0.0, 0.0, 0.0]))
    pos_max = list(envelope.get("position_max_m", [0.0, 0.0, 0.0]))
    x_values = _axis_values(pos_min[0], pos_max[0], translation_step_m)
    y_values = _axis_values(pos_min[1], pos_max[1], translation_step_m)
    z_values = _axis_values(pos_min[2], pos_max[2], translation_step_m)
    if include_rotation:
        rot_min = list(envelope.get("orientation_rpy_min_deg", [0.0, 0.0, 0.0]))
        rot_max = list(envelope.get("orientation_rpy_max_deg", [0.0, 0.0, 0.0]))
        r_values = _axis_values(rot_min[0], rot_max[0], rotation_step_deg)
        p_values = _axis_values(rot_min[1], rot_max[1], rotation_step_deg)
        yv_values = _axis_values(rot_min[2], rot_max[2], rotation_step_deg)
    else:
        r_values = [0.0]
        p_values = [0.0]
        yv_values = [0.0]
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


def _dot(a: list[float], b: list[float]) -> float:
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2])


def _cross(a: list[float], b: list[float]) -> list[float]:
    return [
        (a[1] * b[2]) - (a[2] * b[1]),
        (a[2] * b[0]) - (a[0] * b[2]),
        (a[0] * b[1]) - (a[1] * b[0]),
    ]


def _norm(v: list[float]) -> float:
    return math.sqrt(_dot(v, v))


def _normalize_vector(v: list[float]) -> list[float]:
    length = _norm(v)
    if length < _VECTOR_EPSILON:
        raise ValueError("cannot normalize near-zero vector")
    return [value / length for value in v]


def _project_onto_plane(v: list[float], normal: list[float]) -> list[float]:
    scale = _dot(v, normal)
    return [
        v[0] - scale * normal[0],
        v[1] - scale * normal[1],
        v[2] - scale * normal[2],
    ]


def _quat_from_rotation_columns(
    x_axis: list[float],
    y_axis: list[float],
    z_axis: list[float],
) -> list[float]:
    m00, m01, m02 = x_axis[0], y_axis[0], z_axis[0]
    m10, m11, m12 = x_axis[1], y_axis[1], z_axis[1]
    m20, m21, m22 = x_axis[2], y_axis[2], z_axis[2]
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        w = (m21 - m12) / s
        x = 0.25 * s
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = 0.25 * s
        z = (m12 + m21) / s
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        w = (m10 - m01) / s
        x = (m02 + m20) / s
        y = (m12 + m21) / s
        z = 0.25 * s
    return _normalize_quat([x, y, z, w])


def _vertical_down_orientation_from_object(object_pose: dict[str, Any]) -> list[float]:
    object_orientation = object_pose.get("orientation_xyzw")
    if object_orientation is None:
        raise ValueError("object pose has no orientation_xyzw")
    object_q = _normalize_quat(object_orientation)
    world_down = [0.0, 0.0, -1.0]
    object_x = _quat_rotate_vector(object_q, [1.0, 0.0, 0.0])
    new_x = _project_onto_plane(object_x, world_down)
    if _norm(new_x) < _VECTOR_EPSILON:
        object_y = _quat_rotate_vector(object_q, [0.0, 1.0, 0.0])
        new_x = _project_onto_plane(object_y, world_down)
    if _norm(new_x) < _VECTOR_EPSILON:
        new_x = [1.0, 0.0, 0.0]
    else:
        new_x = _normalize_vector(new_x)

    new_y = _cross(world_down, new_x)
    if _norm(new_y) < _VECTOR_EPSILON:
        new_y = [0.0, -1.0, 0.0]
    else:
        new_y = _normalize_vector(new_y)

    return _quat_from_rotation_columns(new_x, new_y, world_down)


def _apply_min_tcp_z(position_m: list[float], min_tcp_z_m: float | None) -> bool:
    if min_tcp_z_m is None:
        return False
    if position_m[2] >= min_tcp_z_m:
        return False
    position_m[2] = float(min_tcp_z_m)
    return True


def _pose_reference_orientation_xyzw(pose: dict[str, Any]) -> list[float]:
    has_quat = pose.get("orientation_xyzw") is not None
    has_rpy = pose.get("orientation_rpy_deg") is not None
    if has_quat and has_rpy:
        quat = _normalize_quat(pose["orientation_xyzw"])
        rpy_quat = _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
        if not _quats_close(quat, rpy_quat):
            raise ValueError("pose orientation_xyzw and orientation_rpy_deg disagree")
        return quat
    if has_quat:
        return _normalize_quat(pose["orientation_xyzw"])
    if has_rpy:
        return _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
    raise ValueError("pose reference requires orientation_xyzw or orientation_rpy_deg")


def _normalize_offset_reference(
    value: Any,
    *,
    base_frame: str,
) -> dict[str, Any]:
    if value is None:
        return _default_offset_reference(base_frame)
    if not isinstance(value, dict):
        raise ValueError("must be a mapping")
    reference_type = value.get("type")
    if reference_type not in {"frame", "object", "robot_link", "pose"}:
        raise ValueError(
            "type must be one of ['frame', 'object', 'pose', 'robot_link']"
        )
    if reference_type == "pose":
        pose = value.get("pose")
        if not isinstance(pose, dict):
            raise ValueError("pose is required when type='pose'")
        _pose_reference_orientation_xyzw(pose)
        return {"type": "pose", "pose": dict(pose)}
    reference_id = value.get("id")
    if isinstance(reference_id, str) and reference_id.strip():
        return {"type": reference_type, "id": reference_id.strip()}
    if reference_type == "frame":
        return _default_offset_reference(base_frame)
    raise ValueError(f"id is required when type='{reference_type}'")


def _reference_trace_value(reference: dict[str, Any]) -> dict[str, Any]:
    if reference.get("type") == "pose":
        return {
            "type": "pose",
            "pose": dict(reference.get("pose") or {}),
        }
    return {
        "type": str(reference.get("type")),
        "id": str(reference.get("id") or ""),
    }


def _is_candidate_tcp_reference(reference: dict[str, Any], tcp_frame: str) -> bool:
    """True when a pick-planning reference names the generated TCP frame.

    Other ``robot_link`` references are sampled from the live backend state.
    The node's own TCP frame is different: at planning time its relevant axes
    are the candidate TCP pose being synthesized, not the robot's current TCP
    pose before the pick move has been planned.
    """
    frame = str(tcp_frame or "").strip()
    return (
        bool(frame)
        and reference.get("type") == "robot_link"
        and str(reference.get("id") or "").strip() == frame
    )


def _candidate_pick_orientation_xyzw(
    *,
    force_vertical_pick: bool,
    vertical_orientation_xyzw: list[float] | None,
    base_orientation_rpy_deg: list[float],
    pick_orientation_offset_rpy_deg: list[float],
) -> list[float]:
    if force_vertical_pick:
        if vertical_orientation_xyzw is None:
            raise ValueError("vertical orientation is required")
        return _normalize_quat(vertical_orientation_xyzw)
    return _rpy_deg_to_xyzw(
        [
            float(base_orientation_rpy_deg[0]) + float(pick_orientation_offset_rpy_deg[0]),
            float(base_orientation_rpy_deg[1]) + float(pick_orientation_offset_rpy_deg[1]),
            float(base_orientation_rpy_deg[2]) + float(pick_orientation_offset_rpy_deg[2]),
        ]
    )


def _reference_orientation_in_base(
    kernel: KernelClient,
    reference: dict[str, Any],
    *,
    base_frame: str,
) -> list[float]:
    reference_type = reference.get("type")
    if reference_type == "frame":
        frame_id = str(reference.get("id") or base_frame)
        if frame_id in ("", base_frame):
            return [0.0, 0.0, 0.0, 1.0]
        frame_pose = kernel.get_link_pose(frame_id, base_frame)
        if frame_pose is None:
            raise ValueError(f"frame '{frame_id}' is unavailable")
        return _normalize_quat(frame_pose["orientation_xyzw"])
    if reference_type == "robot_link":
        link_id = str(reference.get("id") or "")
        link_pose = kernel.get_link_pose(link_id, base_frame)
        if link_pose is None:
            raise ValueError(f"robot link '{link_id}' is unavailable")
        return _normalize_quat(link_pose["orientation_xyzw"])
    if reference_type == "object":
        object_id = str(reference.get("id") or "")
        object_pose = kernel.get_object_pose(object_id, base_frame)
        if object_pose is None:
            raise ValueError(f"object '{object_id}' is unavailable")
        return _normalize_quat(object_pose["orientation_xyzw"])
    if reference_type == "pose":
        pose = reference.get("pose")
        if not isinstance(pose, dict):
            raise ValueError("pose reference must contain a pose mapping")
        pose_orientation = _pose_reference_orientation_xyzw(pose)
        frame_id = str(pose.get("frame_id") or pose.get("frameId") or base_frame)
        if frame_id in ("", base_frame):
            return pose_orientation
        frame_pose = kernel.get_link_pose(frame_id, base_frame)
        if frame_pose is None:
            raise ValueError(f"pose reference frame '{frame_id}' is unavailable")
        return _quat_mul(frame_pose["orientation_xyzw"], pose_orientation)
    raise ValueError(f"unsupported reference type: {reference_type!r}")


def _compose_pose(
    *,
    object_pose: dict[str, Any],
    object_top_z: float,
    position_offset_m: list[float],
    orientation_offset_rpy_deg: list[float],
    base_orientation_rpy_deg: list[float] | None = None,
    base_orientation_xyzw: list[float] | None = None,
    min_tcp_z_m: float | None = None,
) -> dict[str, Any]:
    position = [
        float(object_pose["position_m"][0]) + float(position_offset_m[0]),
        float(object_pose["position_m"][1]) + float(position_offset_m[1]),
        float(object_top_z) + float(position_offset_m[2]),
    ]
    min_tcp_z_applied = _apply_min_tcp_z(position, min_tcp_z_m)
    if base_orientation_xyzw is not None:
        orientation_xyzw = _normalize_quat(base_orientation_xyzw)
        orientation_rpy_deg = _xyzw_to_rpy_deg(orientation_xyzw)
    elif base_orientation_rpy_deg is not None:
        orientation_rpy_deg = [
            float(base_orientation_rpy_deg[0]) + float(orientation_offset_rpy_deg[0]),
            float(base_orientation_rpy_deg[1]) + float(orientation_offset_rpy_deg[1]),
            float(base_orientation_rpy_deg[2]) + float(orientation_offset_rpy_deg[2]),
        ]
        orientation_xyzw = _rpy_deg_to_xyzw(orientation_rpy_deg)
    else:
        raise ValueError("base orientation is required")
    return {
        "position_m": position,
        "orientation_xyzw": orientation_xyzw,
        "orientation_rpy_deg": orientation_rpy_deg,
        "min_tcp_z_applied": min_tcp_z_applied,
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


def _detail_string(error_details: dict[str, Any], key: str) -> Optional[str]:
    value = error_details.get(key)
    if value is None:
        return None
    text = str(value).strip()
    return text or None


def _detail_float(error_details: dict[str, Any], key: str) -> Optional[float]:
    value = error_details.get(key)
    if value is None:
        return None
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(numeric):
        return None
    return numeric


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
        reason = _detail_string(error_details, "reason") or _detail_string(
            error_details, "reason_code"
        )
        if reason:
            parts.append(f"reason={reason}")
        if _detail_flag(error_details, "start_in_collision"):
            parts.append("start_state_in_collision=true")
        if _detail_flag(error_details, "goal_in_collision"):
            parts.append("goal_in_collision=true")
        pair = _detail_string(
            error_details, "collision_result_first_failure_pair"
        )
        if pair:
            parts.append(f"collision_pair={pair}")
        classification = _detail_string(
            error_details, "collision_result_first_failure_classification"
        )
        if classification:
            parts.append(f"classification={classification}")
        waypoint_index = _detail_string(error_details, "collision_result_waypoint_index")
        if waypoint_index:
            parts.append(f"waypoint={waypoint_index}")
        current_penetration = _detail_float(
            error_details,
            "collision_result_raw_contact_release_current_penetration_m",
        )
        max_start_penetration = _detail_float(
            error_details,
            "collision_result_raw_contact_release_max_start_penetration_m",
        )
        if current_penetration is not None or max_start_penetration is not None:
            support_parts: list[str] = []
            if current_penetration is not None:
                support_parts.append(
                    f"current_penetration_m={current_penetration:.6f}"
                )
            if max_start_penetration is not None:
                support_parts.append(
                    f"max_start_penetration_m={max_start_penetration:.6f}"
                )
            enforce = _detail_string(
                error_details,
                "collision_result_raw_contact_release_enforce_max_start_penetration",
            )
            if enforce:
                support_parts.append(f"enforce_max_start_penetration={enforce}")
            parts.append(f"support_contact_policy=({', '.join(support_parts)})")
        contact_a = error_details.get("contact_body_a")
        contact_b = error_details.get("contact_body_b")
        if contact_a or contact_b:
            parts.append(
                f"contact=({contact_a or 'unknown'}, {contact_b or 'unknown'})"
            )
    return " | ".join(parts)


def _json_safe_detail_value(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, (list, tuple)):
        return [_json_safe_detail_value(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe_detail_value(item) for key, item in value.items()}
    return str(value)


def _preview_failure_causes(
    *,
    preview_error: Optional[str],
    error_details: Optional[dict[str, Any]],
) -> list[str]:
    details = error_details or {}
    causes: list[ReasonCode] = []
    failure_stage = str(details.get("failure_stage") or "").lower()
    classification = str(
        details.get("collision_result_first_failure_classification") or ""
    ).lower()
    error_text = str(preview_error or "").lower()

    if _detail_flag(details, "start_in_collision"):
        causes.append(ReasonCode.START_STATE_IN_COLLISION)
    if "collision_aware_ik" in failure_stage:
        causes.append(ReasonCode.COLLISION_AWARE_IK_FAILURE)
    elif "ik" in failure_stage or "ik failed" in error_text:
        causes.append(ReasonCode.RAW_IK_FAILURE)
    if (
        _detail_flag(details, "goal_in_collision")
        or "collision_validation" in failure_stage
        or details.get("collision_result_first_failure_pair") is not None
        or details.get("collision_pairs") is not None
    ):
        causes.append(ReasonCode.COLLISION_VALIDATION_FAILURE)
    if "support_contact" in failure_stage or "support_contact" in classification:
        causes.append(ReasonCode.SUPPORT_CONTACT_REJECTION)
    if (
        details.get("contact_grasp_violation") is not None
        or details.get("contact_grasp_policy_id") is not None
    ):
        causes.append(ReasonCode.CONTACT_POLICY_REJECTION)

    if not causes:
        causes.append(ReasonCode.PREVIEW_FAILED)
    return list(dict.fromkeys(cause.value for cause in causes))


def _stage_checks_for_failure(causes: list[str]) -> tuple[str, str]:
    ik_failures = {
        ReasonCode.RAW_IK_FAILURE.value,
        ReasonCode.COLLISION_AWARE_IK_FAILURE.value,
        ReasonCode.PREVIEW_FAILED.value,
    }
    if any(cause in ik_failures for cause in causes):
        return "failure", "not_run"
    return "success", "failure"


class PlanPickSequenceNode(
    ActionSpanMixin,
    BlackboardOutputMixin,
    RuntimePauseMixin,
    py_trees.behaviour.Behaviour,
):
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
        pick_offset_reference: Optional[dict[str, Any]] = None,
        approach_offset_envelope: Optional[dict[str, Any]] = None,
        approach_offset_reference: Optional[dict[str, Any]] = None,
        search_strategy: str = "first_feasible_grid",
        translation_step_m: float = 0.01,
        rotation_step_deg: float = 15.0,
        force_vertical_pick: bool = False,
        enable_min_tcp_z: Optional[bool] = None,
        min_tcp_z_m: Optional[float] = None,
        planning_timeout_s: float = 2.0,
        velocity_scale: float = 0.35,
        acceleration_scale: float = 1.0,
        max_tcp_linear_speed_mps: Optional[float] = None,
        contact_policy_id: Optional[str] = None,
        approach_move_node_name: str = "",
        pick_move_node_name: str = "",
        retreat_move_node_name: str = "",
        robot_id: str = "",
        group_id: str = "",
        base_frame: str = "",
        scene_snapshot_provider: Optional[
            Callable[[int, int, str], Optional[bytes]]
        ] = None,
        no_feasible_plan_policy: str = "pause_for_operator",
        runtime_pause_registry: Optional[RuntimePauseRegistry] = None,
    ):
        super().__init__(name=name)
        self._configure_runtime_pause(
            registry=runtime_pause_registry,
            node_id="plan_pick_sequence",
            domain="planning",
        )
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
        self._pick_offset_reference = pick_offset_reference
        self._approach_offset_envelope = dict(
            approach_offset_envelope or _default_approach_offset_envelope()
        )
        self._approach_offset_reference = approach_offset_reference
        self._search_strategy = search_strategy
        self._translation_step_m = translation_step_m
        self._rotation_step_deg = rotation_step_deg
        self._force_vertical_pick = bool(force_vertical_pick)
        self._enable_min_tcp_z = enable_min_tcp_z
        self._min_tcp_z_m = min_tcp_z_m
        self._planning_timeout_s = planning_timeout_s
        self._velocity_scale = velocity_scale
        self._acceleration_scale = acceleration_scale
        self._max_tcp_linear_speed_mps = max_tcp_linear_speed_mps
        self._contact_policy_id = contact_policy_id
        self._approach_move_node_name = approach_move_node_name
        self._pick_move_node_name = pick_move_node_name
        self._retreat_move_node_name = retreat_move_node_name
        self._robot_id = robot_id
        self._group_id = group_id
        self._base_frame = base_frame
        self._scene_snapshot_provider = scene_snapshot_provider
        self._no_feasible_plan_policy = str(no_feasible_plan_policy)
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
        self._bb.register_key(key="program_run_id", access=py_trees.common.Access.READ)
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
                    action_span=self._current_action_span(self._bb),
                )
            except Exception as exc:  # noqa: BLE001 - callback boundary
                self.feedback_message = f"{label} pose-source apply raised: {exc}"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="pose_source_apply_failed",
                    failure_detail=f"{label}: {exc}",
                )
                return self._pause_planning(
                    reason_code="pose_source_apply_failed",
                    message=self.feedback_message,
                    details={"phase": label, "exception_type": type(exc).__name__},
                )
            if not result.get("success", False):
                error = str(result.get("error") or f"{label} pose-source apply failed")
                self.feedback_message = error
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="pose_source_apply_failed",
                    failure_detail=f"{label}: {error}",
                )
                return self._pause_planning(
                    reason_code="pose_source_apply_failed",
                    message=self.feedback_message,
                    details={"phase": label},
                )
        return None

    def initialise(self) -> None:
        self._ensure_output_keys()
        self._reset_action_span()
        self._awaiting_scene_snapshot = False
        self._scene_snapshot_wait_started_ns = 0

    def update(self) -> py_trees.common.Status:
        if self._consume_runtime_resume() is not None:
            self._awaiting_scene_snapshot = False
            self._scene_snapshot_wait_started_ns = 0
            self.feedback_message = ""
        elif self._runtime_pause_active():
            return py_trees.common.Status.RUNNING

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
            return self._pause_planning(
                reason_code="selected_object_unavailable",
                message=self.feedback_message,
            )

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

        if self._no_feasible_plan_policy not in {
            "pause_for_operator",
            "condition_failure",
        }:
            self.feedback_message = (
                "no_feasible_plan_policy must be pause_for_operator or "
                "condition_failure"
            )
            return py_trees.common.Status.FAILURE

        min_tcp_z_enabled = (
            self._min_tcp_z_m is not None
            if self._enable_min_tcp_z is None
            else bool(self._enable_min_tcp_z)
        )
        min_tcp_z_m: float | None = None
        if min_tcp_z_enabled:
            if self._min_tcp_z_m is None:
                self.feedback_message = (
                    "min_tcp_z_m is required when enable_min_tcp_z is true"
                )
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="invalid_min_tcp_z",
                    failure_detail=self.feedback_message,
                )
                return py_trees.common.Status.FAILURE
            try:
                min_tcp_z_m = float(self._min_tcp_z_m)
            except (TypeError, ValueError):
                self.feedback_message = "min_tcp_z_m must be a finite non-negative number"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="invalid_min_tcp_z",
                    failure_detail=self.feedback_message,
                )
                return py_trees.common.Status.FAILURE
            if not math.isfinite(min_tcp_z_m) or min_tcp_z_m < 0.0:
                self.feedback_message = "min_tcp_z_m must be a finite non-negative number"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="invalid_min_tcp_z",
                    failure_detail=self.feedback_message,
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
                return self._pause_planning(
                    reason_code="image_snapshot_unavailable",
                    message=self.feedback_message,
                    details={"exception_type": type(exc).__name__},
                )
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
                    return self._pause_planning(
                        reason_code="image_snapshot_unavailable",
                        message=self.feedback_message,
                    )
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

        try:
            scene_snapshot = next(
                (
                    obj
                    for obj in kernel.get_scene_objects()
                    if obj.object_id == selected_object_id
                ),
                None,
            )
            object_pose = kernel.get_object_pose(
                selected_object_id, self._base_frame
            )
        except Exception as exc:  # noqa: BLE001 - backend observation boundary.
            self.feedback_message = (
                f"Backend scene lookup failed for '{selected_object_id}': {exc}"
            )
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="selected_object_unavailable",
                failure_detail=str(exc),
            )
            return self._pause_planning(
                reason_code="selected_object_unavailable",
                message=self.feedback_message,
                details={"exception_type": type(exc).__name__},
            )
        if scene_snapshot is None or object_pose is None:
            self.feedback_message = f"Object '{selected_object_id}' not found in backend scene"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="object_not_found",
            )
            return self._pause_planning(
                reason_code="selected_object_unavailable",
                message=self.feedback_message,
                details={"selected_object_id": selected_object_id},
            )

        object_top_z = _shape_top_height(scene_snapshot, object_pose)
        base_orientation = [180.0, 0.0, 0.0]
        base_orientation_xyzw: list[float] | None = None
        if self._force_vertical_pick:
            try:
                base_orientation_xyzw = _vertical_down_orientation_from_object(object_pose)
            except ValueError as exc:
                self.feedback_message = f"Invalid object orientation for vertical pick: {exc}"
                self._write_failure_response(
                    selected_object_id=selected_object_id,
                    attempted_candidates=0,
                    outcome="invalid_object_orientation",
                    failure_detail=str(exc),
                )
                return self._pause_planning(
                    reason_code="selected_object_pose_invalid",
                    message=self.feedback_message,
                    details={"selected_object_id": selected_object_id},
                )
        try:
            pick_offsets = _iter_offset_vectors(
                self._pick_offset_envelope,
                translation_step_m=self._translation_step_m,
                rotation_step_deg=self._rotation_step_deg,
                include_rotation=not self._force_vertical_pick,
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
            pick_offset_reference = _normalize_offset_reference(
                self._pick_offset_reference,
                base_frame=self._base_frame,
            )
            pick_offset_orientation = (
                None
                if _is_candidate_tcp_reference(
                    pick_offset_reference,
                    self._tcp_frame,
                )
                else _reference_orientation_in_base(
                    kernel,
                    pick_offset_reference,
                    base_frame=self._base_frame,
                )
            )
        except ValueError as exc:
            self.feedback_message = f"Invalid pick_offset_reference: {exc}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="invalid_pick_offset_reference",
                failure_detail=str(exc),
            )
            if "unavailable" in str(exc).lower():
                return self._pause_planning(
                    reason_code="pick_reference_unavailable",
                    message=self.feedback_message,
                )
            return py_trees.common.Status.FAILURE

        try:
            approach_offsets = _iter_offset_vectors(
                self._approach_offset_envelope,
                translation_step_m=self._translation_step_m,
                rotation_step_deg=self._rotation_step_deg,
                include_rotation=not self._force_vertical_pick,
            )
        except ValueError as exc:
            self.feedback_message = f"Invalid approach_offset_envelope: {exc}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="invalid_approach_offset_envelope",
            )
            return py_trees.common.Status.FAILURE

        try:
            approach_offset_reference = _normalize_offset_reference(
                self._approach_offset_reference,
                base_frame=self._base_frame,
            )
            approach_offset_orientation = (
                None
                if _is_candidate_tcp_reference(
                    approach_offset_reference,
                    self._tcp_frame,
                )
                else _reference_orientation_in_base(
                    kernel,
                    approach_offset_reference,
                    base_frame=self._base_frame,
                )
            )
        except ValueError as exc:
            self.feedback_message = f"Invalid approach_offset_reference: {exc}"
            self._write_failure_response(
                selected_object_id=selected_object_id,
                attempted_candidates=0,
                outcome="invalid_approach_offset_reference",
                failure_detail=str(exc),
            )
            if "unavailable" in str(exc).lower():
                return self._pause_planning(
                    reason_code="approach_reference_unavailable",
                    message=self.feedback_message,
                )
            return py_trees.common.Status.FAILURE

        attempted_candidates = 0
        best_candidate: Optional[dict[str, Any]] = None
        last_candidate_response: Optional[dict[str, Any]] = None
        last_failure_detail: Optional[str] = None
        candidate_stage_reports: list[dict[str, Any]] = []

        for (pick_pos_offset, pick_rot_offset) in pick_offsets:
            candidate_pick_orientation = _candidate_pick_orientation_xyzw(
                force_vertical_pick=self._force_vertical_pick,
                vertical_orientation_xyzw=base_orientation_xyzw,
                base_orientation_rpy_deg=base_orientation,
                pick_orientation_offset_rpy_deg=pick_rot_offset,
            )
            pick_pos_offset_base = _quat_rotate_vector(
                candidate_pick_orientation
                if pick_offset_orientation is None
                else pick_offset_orientation,
                pick_pos_offset,
            )
            pick_pose = _compose_pose(
                object_pose=object_pose,
                object_top_z=object_top_z,
                position_offset_m=pick_pos_offset_base,
                orientation_offset_rpy_deg=pick_rot_offset,
                base_orientation_rpy_deg=(
                    None if self._force_vertical_pick else base_orientation
                ),
                base_orientation_xyzw=base_orientation_xyzw,
                min_tcp_z_m=min_tcp_z_m,
            )
            for (approach_pos_offset, approach_rot_offset) in approach_offsets:
                attempted_candidates += 1
                approach_pos_offset_base = _quat_rotate_vector(
                    pick_pose["orientation_xyzw"]
                    if approach_offset_orientation is None
                    else approach_offset_orientation,
                    approach_pos_offset,
                )
                approach_pose = {
                    "position_m": [
                        pick_pose["position_m"][0] + float(approach_pos_offset_base[0]),
                        pick_pose["position_m"][1] + float(approach_pos_offset_base[1]),
                        pick_pose["position_m"][2] + float(approach_pos_offset_base[2]),
                    ],
                }
                approach_min_z_applied = _apply_min_tcp_z(
                    approach_pose["position_m"],
                    min_tcp_z_m,
                )
                if self._force_vertical_pick:
                    approach_pose["orientation_xyzw"] = list(pick_pose["orientation_xyzw"])
                    approach_pose["orientation_rpy_deg"] = list(pick_pose["orientation_rpy_deg"])
                else:
                    approach_pose["orientation_rpy_deg"] = [
                        pick_pose["orientation_rpy_deg"][0] + float(approach_rot_offset[0]),
                        pick_pose["orientation_rpy_deg"][1] + float(approach_rot_offset[1]),
                        pick_pose["orientation_rpy_deg"][2] + float(approach_rot_offset[2]),
                    ]
                    approach_pose["orientation_xyzw"] = _rpy_deg_to_xyzw(
                        approach_pose["orientation_rpy_deg"]
                    )

                retreat_pose = {
                    "position_m": list(approach_pose["position_m"]),
                    "orientation_xyzw": list(approach_pose["orientation_xyzw"]),
                    "orientation_rpy_deg": list(approach_pose["orientation_rpy_deg"]),
                }
                candidate_stage_report = self._new_candidate_stage_report(
                    selected_object_id=selected_object_id,
                    candidate_index=attempted_candidates,
                    approach_pose=approach_pose,
                    pick_pose=pick_pose,
                    retreat_pose=retreat_pose,
                )
                candidate_stage_reports.append(candidate_stage_report)
                candidate_response = self._make_candidate_response(
                    selected_object_id=selected_object_id,
                    approach_pose=approach_pose,
                    pick_pose=pick_pose,
                    retreat_pose=retreat_pose,
                    attempted_candidates=attempted_candidates,
                    planning_score=None,
                    outcome="candidate_generated",
                    min_tcp_z_enabled=min_tcp_z_enabled,
                    min_tcp_z_m=min_tcp_z_m,
                    min_tcp_z_applied=bool(
                        pick_pose.get("min_tcp_z_applied") or approach_min_z_applied
                    ),
                    pick_offset_reference=pick_offset_reference,
                    approach_offset_reference=approach_offset_reference,
                    candidate_stage_report=candidate_stage_report,
                    candidate_stage_reports=candidate_stage_reports,
                )
                last_candidate_response = candidate_response
                self._write_candidate_response(candidate_response)

                try:
                    approach_preview = kernel.preview_motion(
                        self._make_pose_goal_request(
                            "pick_approach_preview", approach_pose
                        )
                    )
                except Exception as exc:  # noqa: BLE001 - planner boundary.
                    self.feedback_message = f"approach preview raised: {exc}"
                    self._write_failure_response(
                        selected_object_id=selected_object_id,
                        attempted_candidates=attempted_candidates,
                        outcome="preview_runtime_failed",
                        failure_detail=self.feedback_message,
                        candidate_response=last_candidate_response,
                        candidate_stage_reports=candidate_stage_reports,
                    )
                    return self._pause_planning(
                        reason_code="pick_preview_runtime_failed",
                        message=self.feedback_message,
                        details={"stage": "approach", "exception_type": type(exc).__name__},
                    )
                if not approach_preview.ok:
                    self._mark_stage_failure(
                        candidate_stage_report,
                        stage="approach",
                        preview=approach_preview,
                    )
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
                            candidate_response=last_candidate_response,
                            candidate_stage_reports=candidate_stage_reports,
                        )
                        return self._pause_planning(
                            reason_code="pick_start_state_in_collision",
                            message=self.feedback_message,
                            details=(approach_preview.error_details or {}),
                        )
                    continue
                self._mark_stage_success(
                    candidate_stage_report,
                    stage="approach",
                    preview=approach_preview,
                )

                try:
                    pick_preview = kernel.preview_motion(
                        self._make_pose_goal_request(
                            "pick_descend_preview", pick_pose
                        ),
                        start_state=approach_preview.final_state,
                    )
                except Exception as exc:  # noqa: BLE001 - planner boundary.
                    self.feedback_message = f"pick preview raised: {exc}"
                    self._write_failure_response(
                        selected_object_id=selected_object_id,
                        attempted_candidates=attempted_candidates,
                        outcome="preview_runtime_failed",
                        failure_detail=self.feedback_message,
                        candidate_response=last_candidate_response,
                        candidate_stage_reports=candidate_stage_reports,
                    )
                    return self._pause_planning(
                        reason_code="pick_preview_runtime_failed",
                        message=self.feedback_message,
                        details={"stage": "pick", "exception_type": type(exc).__name__},
                    )
                if not pick_preview.ok:
                    self._mark_stage_failure(
                        candidate_stage_report,
                        stage="pick",
                        preview=pick_preview,
                    )
                    last_failure_detail = _preview_failure_message(
                        stage="pick",
                        preview_error=pick_preview.error,
                        error_details=pick_preview.error_details,
                        attempted_candidates=attempted_candidates,
                    )
                    continue
                self._mark_stage_success(
                    candidate_stage_report,
                    stage="pick",
                    preview=pick_preview,
                )

                try:
                    exit_preview = kernel.preview_motion(
                        self._make_pose_goal_request(
                            "pick_exit_preview", approach_pose
                        ),
                        start_state=pick_preview.final_state,
                    )
                except Exception as exc:  # noqa: BLE001 - planner boundary.
                    self.feedback_message = f"exit preview raised: {exc}"
                    self._write_failure_response(
                        selected_object_id=selected_object_id,
                        attempted_candidates=attempted_candidates,
                        outcome="preview_runtime_failed",
                        failure_detail=self.feedback_message,
                        candidate_response=last_candidate_response,
                        candidate_stage_reports=candidate_stage_reports,
                    )
                    return self._pause_planning(
                        reason_code="pick_preview_runtime_failed",
                        message=self.feedback_message,
                        details={"stage": "exit", "exception_type": type(exc).__name__},
                    )
                if not exit_preview.ok:
                    self._mark_stage_failure(
                        candidate_stage_report,
                        stage="retreat",
                        preview=exit_preview,
                    )
                    last_failure_detail = _preview_failure_message(
                        stage="exit",
                        preview_error=exit_preview.error,
                        error_details=exit_preview.error_details,
                        attempted_candidates=attempted_candidates,
                    )
                    continue
                self._mark_stage_success(
                    candidate_stage_report,
                    stage="retreat",
                    preview=exit_preview,
                )

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
                candidate = dict(candidate_response)
                candidate["planning_score"] = planning_score
                candidate["outcome"] = "planned"
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
            candidate_response=last_candidate_response,
            candidate_stage_reports=candidate_stage_reports,
        )
        if self._no_feasible_plan_policy == "condition_failure":
            return py_trees.common.Status.FAILURE
        return self._pause_planning(
            reason_code="no_feasible_pick_plan",
            message=self.feedback_message,
            details={
                "selected_object_id": selected_object_id,
                "attempted_candidates": attempted_candidates,
                "last_failure_detail": last_failure_detail,
            },
        )

    def _pause_planning(
        self,
        *,
        reason_code: str,
        message: str,
        details: Optional[dict[str, Any]] = None,
    ) -> py_trees.common.Status:
        self._raise_runtime_pause(
            reason_code=reason_code,
            message=message,
            details=details,
        )
        return py_trees.common.Status.RUNNING

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._clear_runtime_pause()

    def _pose_output(self, pose: dict[str, Any]) -> dict[str, Any]:
        return {
            "frame_id": self._base_frame,
            "position_m": list(pose["position_m"]),
            "orientation_xyzw": list(pose["orientation_xyzw"]),
        }

    def _new_candidate_stage_report(
        self,
        *,
        selected_object_id: str,
        candidate_index: int,
        approach_pose: dict[str, Any],
        pick_pose: dict[str, Any],
        retreat_pose: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "candidate_index": candidate_index,
            "selected_object_id": selected_object_id,
            "contact_policy_id": self._contact_policy_id,
            "stages": [
                self._initial_stage_report(
                    "approach",
                    "pick_approach_preview",
                    approach_pose,
                ),
                self._initial_stage_report("pick", "pick_descend_preview", pick_pose),
                self._initial_stage_report("retreat", "pick_exit_preview", retreat_pose),
            ],
        }

    def _initial_stage_report(
        self,
        stage: str,
        move_id: str,
        pose: dict[str, Any],
    ) -> dict[str, Any]:
        return {
            "stage": stage,
            "move_id": move_id,
            "pose_goal": self._pose_output(pose),
            "status": "not_run",
            "kinematic_ik": "not_run",
            "collision_check": "not_run",
            "failures": [],
        }

    @staticmethod
    def _stage_report(report: dict[str, Any], stage: str) -> dict[str, Any]:
        for stage_report in report.get("stages", []):
            if stage_report.get("stage") == stage:
                return stage_report
        raise KeyError(f"Unknown pick stage: {stage}")

    def _mark_stage_success(
        self,
        report: dict[str, Any],
        *,
        stage: str,
        preview: MotionPreviewResult,
    ) -> None:
        stage_report = self._stage_report(report, stage)
        stage_report["status"] = "success"
        stage_report["kinematic_ik"] = "success"
        stage_report["collision_check"] = "success"
        stage_report["trajectory_duration_s"] = preview.trajectory_duration_s
        stage_report["trajectory_waypoint_count"] = preview.trajectory_waypoint_count

    def _mark_stage_failure(
        self,
        report: dict[str, Any],
        *,
        stage: str,
        preview: MotionPreviewResult,
    ) -> None:
        stage_report = self._stage_report(report, stage)
        causes = _preview_failure_causes(
            preview_error=preview.error,
            error_details=preview.error_details,
        )
        kinematic_ik, collision_check = _stage_checks_for_failure(causes)
        stage_report["status"] = "failure"
        stage_report["kinematic_ik"] = kinematic_ik
        stage_report["collision_check"] = collision_check
        failure: dict[str, Any] = {
            "cause": causes[0],
            "causes": causes,
        }
        if preview.error:
            failure["message"] = preview.error
        if preview.error_details:
            failure["details"] = {
                str(key): _json_safe_detail_value(value)
                for key, value in preview.error_details.items()
            }
        stage_report["failures"] = [failure]

    def _make_candidate_response(
        self,
        *,
        selected_object_id: str,
        approach_pose: dict[str, Any],
        pick_pose: dict[str, Any],
        retreat_pose: dict[str, Any],
        attempted_candidates: int,
        planning_score: Optional[float],
        outcome: str,
        min_tcp_z_enabled: bool,
        min_tcp_z_m: Optional[float],
        min_tcp_z_applied: bool,
        pick_offset_reference: dict[str, Any],
        approach_offset_reference: dict[str, Any],
        candidate_stage_report: Optional[dict[str, Any]] = None,
        candidate_stage_reports: Optional[list[dict[str, Any]]] = None,
    ) -> dict[str, Any]:
        response = {
            "selected_object_id": selected_object_id,
            "approach_pose": self._pose_output(approach_pose),
            "pick_pose": self._pose_output(pick_pose),
            "retreat_pose": self._pose_output(retreat_pose),
            "attempted_candidates": attempted_candidates,
            "planning_score": planning_score,
            "outcome": outcome,
            "force_vertical_pick": self._force_vertical_pick,
            "enable_min_tcp_z": min_tcp_z_enabled,
            "min_tcp_z_m": min_tcp_z_m,
            "min_tcp_z_applied": min_tcp_z_applied,
            "pick_offset_reference": _reference_trace_value(pick_offset_reference),
            "approach_offset_reference": _reference_trace_value(
                approach_offset_reference
            ),
        }
        if candidate_stage_report is not None:
            response["candidate_stage_report"] = candidate_stage_report
        if candidate_stage_reports is not None:
            response["candidate_stage_reports"] = list(candidate_stage_reports)
        return response

    def _child_action_span(self, move_id: str) -> ActionSpan:
        parent = self._current_action_span(self._bb)
        child_attempt_id = f"{parent.node_attempt_id}:{move_id}"
        return ActionSpan(
            action_source="program_node",
            action_span_id=child_attempt_id,
            program_run_id=read_program_run_id(self._bb),
            node_instance_id=f"{parent.node_instance_id}/{move_id}",
            node_attempt_id=child_attempt_id,
            parent_node_attempt_id=parent.node_attempt_id,
        )

    def _make_pose_goal_request(self, move_id: str, pose: dict[str, Any]) -> ExecuteRequest:
        return ExecuteRequest(
            robot_id=self._robot_id,
            group_id=self._group_id,
            move_id=move_id,
            base_frame=self._base_frame,
            tcp_frame=self._tcp_frame,
            motion_type="pose_goal",
            pose_goal={
                "frame_id": self._base_frame,
                "position_m": list(pose["position_m"]),
                "orientation_xyzw": list(pose["orientation_xyzw"]),
            },
            velocity_scale=self._velocity_scale,
            acceleration_scale=self._acceleration_scale,
            max_tcp_linear_speed_mps=self._max_tcp_linear_speed_mps,
            planning_timeout_s=self._planning_timeout_s,
            execution_timeout_s=0.0,
            cache_mode="disable",
            program_run_id=read_program_run_id(self._bb),
            action_span=self._child_action_span(move_id),
            contact_policy_id=self._contact_policy_id,
        )

    def _write_candidate_response(self, response: dict[str, Any]) -> None:
        self._populate_linked_move_nodes(response)
        self._write_namespace_output(copy.deepcopy(response), status="candidate")

    def _write_response(self, response: dict[str, Any]) -> None:
        self._populate_linked_move_nodes(response)
        self._write_namespace_output(copy.deepcopy(response), status="planned")

    def _write_failure_response(
        self,
        *,
        selected_object_id: Optional[str],
        attempted_candidates: int,
        outcome: str,
        failure_detail: Optional[str] = None,
        candidate_response: Optional[dict[str, Any]] = None,
        candidate_stage_reports: Optional[list[dict[str, Any]]] = None,
    ) -> None:
        response = dict(candidate_response or {})
        response.setdefault("selected_object_id", selected_object_id)
        response.setdefault("approach_pose", None)
        response.setdefault("pick_pose", None)
        response.setdefault("retreat_pose", None)
        response["attempted_candidates"] = attempted_candidates
        response["planning_score"] = None
        response["outcome"] = outcome
        response["failure_detail"] = failure_detail
        if candidate_stage_reports is not None:
            response["candidate_stage_reports"] = list(candidate_stage_reports)
            if candidate_stage_reports:
                response["candidate_stage_report"] = candidate_stage_reports[-1]
        if candidate_response is not None:
            self._populate_linked_move_nodes(response)
        self._write_namespace_output(copy.deepcopy(response), status="failed")

    def _populate_linked_move_nodes(self, response: dict[str, Any]) -> None:
        name_to_pose = {
            str(self._approach_move_node_name or ""): response.get("approach_pose"),
            str(self._pick_move_node_name or ""): response.get("pick_pose"),
            str(self._retreat_move_node_name or ""): response.get("retreat_pose"),
        }
        name_to_pose = {
            name: pose
            for name, pose in name_to_pose.items()
            if name and isinstance(pose, dict)
        }
        if not name_to_pose:
            return
        for behaviour in self._iter_tree_behaviours():
            pose = name_to_pose.get(getattr(behaviour, "name", ""))
            if pose is None:
                continue
            params = getattr(behaviour, "_params", None)
            if not isinstance(params, dict):
                continue
            if params.get("motion_type") != "pose_goal":
                continue
            params["pose_goal"] = {
                "frame_id": pose["frame_id"],
                "position_m": list(pose["position_m"]),
                "orientation_xyzw": list(pose["orientation_xyzw"]),
            }
            if self._contact_policy_id:
                params["contact_policy_id"] = self._contact_policy_id
            params["parent_node_attempt_id"] = self._current_action_span(
                self._bb
            ).node_attempt_id

    def _iter_tree_behaviours(self):
        root: Any = self
        seen: set[int] = set()
        while getattr(root, "parent", None) is not None:
            if id(root) in seen:
                break
            seen.add(id(root))
            root = root.parent

        stack = [root]
        seen.clear()
        while stack:
            behaviour = stack.pop()
            if id(behaviour) in seen:
                continue
            seen.add(id(behaviour))
            yield behaviour
            for child in reversed(list(getattr(behaviour, "children", []) or [])):
                stack.append(child)
            for attr in ("decorated", "child"):
                child = getattr(behaviour, attr, None)
                if child is not None:
                    stack.append(child)
