"""Observer-safe visual scene snapshot helpers.

This module is intentionally independent from RoboPlan's mutable planning
scene. It builds a small kinematics-only Pinocchio model from the active URDF
and computes visual link poses from execution snapshots. The planner/collision
scene remains the authority for planning; this surface is for rendering and
diagnostics only.
"""
from __future__ import annotations

import logging
import math
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .models import SceneRobotResponse, VisualSceneSnapshotResponse
from .state import ComposerState

logger = logging.getLogger(__name__)


class VisualSnapshotUnavailable(RuntimeError):
    """Raised when the visual snapshot path cannot serve a response."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(detail or reason)
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True)
class _VisualFkModel:
    model_key: str
    model: Any
    joint_q_index: dict[str, tuple[int, int]]


def _string_map(raw: Any) -> dict[str, str]:
    if not isinstance(raw, dict):
        return {}
    out: dict[str, str] = {}
    for key, value in raw.items():
        if key is None or value is None:
            continue
        out[str(key)] = str(value)
    return out


def _visual_fk_urdf_path(
    *,
    canonical_urdf_path: str,
    metadata: dict[str, Any],
) -> str:
    """Return the URDF path the visual FK model should load.

    Composer serves canonical visual meshes and public metadata, but Pinocchio
    may need the same private import view as RoboPlan for parser-order-sensitive
    mimic joints. The private view is a consumer artifact; public names remain
    canonical through the joint-name maps below.
    """

    if not metadata:
        return canonical_urdf_path
    if not bool(metadata.get("view_changed", False)):
        return canonical_urdf_path
    view_urdf_path = str(metadata.get("view_urdf_path") or "")
    if not view_urdf_path:
        raise VisualSnapshotUnavailable(
            "visual_model_view_unavailable",
            "Robot Model Authority reports a private view but no view_urdf_path",
        )
    if not Path(view_urdf_path).exists():
        raise VisualSnapshotUnavailable(
            "visual_model_view_unavailable",
            f"Robot Model Authority private view does not exist: {view_urdf_path}",
        )
    return view_urdf_path


_cache_lock = threading.Lock()
_visual_fk_cache: dict[str, _VisualFkModel] = {}
_visual_fk_thread_local = threading.local()
_sequence_lock = threading.Lock()
_sequence_number = 0
_MAX_VISUAL_FK_MODELS = 8


def _next_sequence_number() -> int:
    global _sequence_number
    with _sequence_lock:
        _sequence_number += 1
        return _sequence_number


def _load_visual_fk_model(state: ComposerState) -> _VisualFkModel:
    from . import routes_urdf

    model_key = routes_urdf.active_urdf_model_key(state)
    if not model_key:
        raise VisualSnapshotUnavailable("visual_model_unavailable")
    cached = _visual_fk_cache.get(model_key)
    if cached is not None:
        return cached

    entry = routes_urdf.get_urdf_entry(state)
    if entry is None:
        raise VisualSnapshotUnavailable("visual_model_unavailable")

    metadata = routes_urdf.robot_model_authority_metadata(state)
    try:
        import pinocchio as pin
    except Exception as exc:  # pragma: no cover - depends on deployment image
        raise VisualSnapshotUnavailable("pinocchio_unavailable", str(exc)) from exc

    with _cache_lock:
        cached = _visual_fk_cache.get(model_key)
        if cached is not None:
            return cached
        visual_urdf_path = _visual_fk_urdf_path(
            canonical_urdf_path=entry.urdf_path,
            metadata=metadata,
        )
        try:
            model = pin.buildModelFromUrdf(visual_urdf_path, True)
        except Exception as exc:
            raise VisualSnapshotUnavailable("visual_model_load_failed", str(exc)) from exc
        view_to_canonical = _string_map(
            metadata.get("view_to_canonical_joint_names")
        )
        joint_q_index: dict[str, tuple[int, int]] = {}
        for joint_id, joint_name in enumerate(model.names):
            if joint_id == 0:
                continue
            model_joint_name = str(joint_name)
            try:
                nq = int(model.nqs[joint_id])
                q_index = int(model.idx_qs[joint_id])
            except Exception:
                continue
            index_value = (q_index, nq)
            joint_q_index[model_joint_name] = index_value
            canonical_joint_name = view_to_canonical.get(model_joint_name)
            if canonical_joint_name:
                joint_q_index[canonical_joint_name] = index_value
        loaded = _VisualFkModel(
            model_key=model_key,
            model=model,
            joint_q_index=joint_q_index,
        )
        _visual_fk_cache[model_key] = loaded
        while len(_visual_fk_cache) > _MAX_VISUAL_FK_MODELS:
            _visual_fk_cache.pop(next(iter(_visual_fk_cache)))
        return loaded


def _visual_fk_data_for_model(fk_model: _VisualFkModel) -> Any:
    """Return mutable Pinocchio Data owned by the current thread.

    Pinocchio ``Model`` is read-only after load, but ``Data`` is mutable scratch
    space. Caching it per thread avoids per-publish allocation without sharing
    mutable kinematic state between the publisher, tests, or any future visual
    worker.
    """

    data_by_model = getattr(_visual_fk_thread_local, "data_by_model", None)
    if data_by_model is None:
        data_by_model = {}
        _visual_fk_thread_local.data_by_model = data_by_model
    data = data_by_model.get(fk_model.model_key)
    if data is None:
        data = fk_model.model.createData()
        data_by_model[fk_model.model_key] = data
    return data


def _rpy_matrix(roll: float, pitch: float, yaw: float) -> list[list[float]]:
    cr, sr = math.cos(roll), math.sin(roll)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    # URDF fixed-axis RPY = Rz(yaw) * Ry(pitch) * Rx(roll).
    return [
        [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
        [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
        [-sp, cp * sr, cp * cr],
    ]


def _quat_matrix(quat_xyzw: list[float]) -> list[list[float]]:
    x, y, z, w = [float(v) for v in quat_xyzw]
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm <= 0.0 or not math.isfinite(norm):
        raise ValueError("orientation quaternion is not finite")
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    return [
        [1.0 - 2.0 * y * y - 2.0 * z * z, 2.0 * x * y - 2.0 * z * w, 2.0 * x * z + 2.0 * y * w],
        [2.0 * x * y + 2.0 * z * w, 1.0 - 2.0 * x * x - 2.0 * z * z, 2.0 * y * z - 2.0 * x * w],
        [2.0 * x * z - 2.0 * y * w, 2.0 * y * z + 2.0 * x * w, 1.0 - 2.0 * x * x - 2.0 * y * y],
    ]


def _base_pose_se3(base_pose: dict[str, Any], reference_frame: str):
    frame_id = str(base_pose.get("frame_id") or base_pose.get("frameId") or reference_frame)
    if frame_id != reference_frame:
        return None
    position = base_pose.get("position_m") or base_pose.get("positionM") or [0.0, 0.0, 0.0]
    if not isinstance(position, list) or len(position) != 3:
        return None
    orientation_xyzw = base_pose.get("orientation_xyzw") or base_pose.get("orientationXYZW")
    if orientation_xyzw is not None:
        if not isinstance(orientation_xyzw, list) or len(orientation_xyzw) != 4:
            return None
        rotation = _quat_matrix(orientation_xyzw)
    else:
        rpy = base_pose.get("orientation_rpy_rad") or base_pose.get("orientationRPYRad")
        if rpy is None:
            rpy_deg = base_pose.get("orientation_rpy_deg") or base_pose.get("orientationRPYDeg")
            if isinstance(rpy_deg, list) and len(rpy_deg) == 3:
                rpy = [math.radians(float(v)) for v in rpy_deg]
        if isinstance(rpy, list) and len(rpy) == 3:
            rotation = _rpy_matrix(float(rpy[0]), float(rpy[1]), float(rpy[2]))
        else:
            rotation = _quat_matrix([0.0, 0.0, 0.0, 1.0])
    try:
        import pinocchio as pin
        import numpy as np

        return pin.SE3(np.array(rotation), np.array([float(v) for v in position]))
    except Exception:
        return None


def _pose_from_se3(placement: Any) -> dict[str, list[float]]:
    try:
        import pinocchio as pin

        quat = pin.Quaternion(placement.rotation)
        quat.normalize()
        coeffs = quat.coeffs()  # x, y, z, w
        return {
            "position_m": [float(v) for v in placement.translation],
            "orientation_xyzw": [float(coeffs[i]) for i in range(4)],
        }
    except Exception as exc:
        raise VisualSnapshotUnavailable("visual_fk_failed", str(exc)) from exc


def relative_pose_from_world(
    *,
    child_pose: dict[str, Any],
    parent_pose: dict[str, Any],
) -> dict[str, list[float]]:
    """Return child pose expressed in the parent frame.

    Both input poses are world/reference-frame poses from a visual snapshot.
    The helper is used by observer routes to derive compatibility fields such
    as ``tcp_pose_in_base`` without calling live planner FK.
    """

    try:
        import numpy as np
        import pinocchio as pin

        parent = pin.SE3(
            np.array(_quat_matrix(parent_pose["orientation_xyzw"])),
            np.array([float(v) for v in parent_pose["position_m"]]),
        )
        child = pin.SE3(
            np.array(_quat_matrix(child_pose["orientation_xyzw"])),
            np.array([float(v) for v in child_pose["position_m"]]),
        )
        return _pose_from_se3(parent.inverse() * child)
    except VisualSnapshotUnavailable:
        raise
    except Exception as exc:
        raise VisualSnapshotUnavailable("visual_relative_pose_failed", str(exc)) from exc


def compute_robot_visual_link_poses(
    state: ComposerState,
    *,
    joint_names: list[str],
    joint_positions_rad: list[float] | None,
    link_names: list[str],
    reference_frame: str,
    base_frame: str,
    base_pose: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    """Compute visual FK link poses from observer-safe joint inputs."""
    if not joint_names or joint_positions_rad is None:
        raise VisualSnapshotUnavailable("joint_state_unavailable")
    if len(joint_names) != len(joint_positions_rad):
        raise VisualSnapshotUnavailable("joint_state_shape_mismatch")
    if not link_names:
        raise VisualSnapshotUnavailable("visual_links_unavailable")

    try:
        import pinocchio as pin
    except Exception as exc:  # pragma: no cover - depends on deployment image
        raise VisualSnapshotUnavailable("pinocchio_unavailable", str(exc)) from exc

    fk_model = _load_visual_fk_model(state)
    model = fk_model.model
    data = _visual_fk_data_for_model(fk_model)
    q = pin.neutral(model)
    for name, position in zip(joint_names, joint_positions_rad):
        q_index_nq = fk_model.joint_q_index.get(str(name))
        if q_index_nq is None:
            continue
        q_index, nq = q_index_nq
        if nq == 1:
            q[q_index] = float(position)
        else:
            # Non-scalar joints are not part of current drivable robot groups.
            # Leave them neutral rather than guessing a representation.
            continue

    try:
        pin.forwardKinematics(model, data, q)
        pin.updateFramePlacements(model, data)
    except Exception as exc:
        raise VisualSnapshotUnavailable("visual_fk_failed", str(exc)) from exc

    try:
        reference_pose = (
            data.oMf[model.getFrameId(reference_frame)]
            if model.existFrame(reference_frame)
            else pin.SE3.Identity()
        )
        reference_inverse = reference_pose.inverse()
    except Exception as exc:
        raise VisualSnapshotUnavailable("reference_frame_unavailable", str(exc)) from exc

    base_adjustment = None
    authored_base_pose = _base_pose_se3(base_pose, reference_frame) if base_pose else None
    if authored_base_pose is not None and model.existFrame(base_frame):
        model_base_in_reference = reference_inverse * data.oMf[model.getFrameId(base_frame)]
        base_adjustment = authored_base_pose * model_base_in_reference.inverse()

    link_poses: dict[str, dict[str, Any]] = {}
    for link_name in link_names:
        if not model.existFrame(link_name):
            continue
        try:
            placement = reference_inverse * data.oMf[model.getFrameId(link_name)]
            if base_adjustment is not None:
                placement = base_adjustment * placement
            link_poses[str(link_name)] = _pose_from_se3(placement)
        except Exception:
            logger.debug("visual FK skipped unresolved link %s", link_name, exc_info=True)

    if not link_poses:
        raise VisualSnapshotUnavailable("visual_fk_empty")

    return link_poses


def build_robot_visual_snapshot(
    state: ComposerState,
    *,
    robot: SceneRobotResponse | None,
    joint_names: list[str],
    joint_positions_rad: list[float] | None,
    link_names: list[str],
    reference_frame: str,
    base_frame: str,
    base_pose: dict[str, Any],
    grippers: list[Any] | None = None,
    pose_sources: list[Any] | None = None,
) -> VisualSceneSnapshotResponse:
    """Build an immutable robot visual snapshot from observer-safe inputs."""
    if robot is None:
        raise VisualSnapshotUnavailable("robot_metadata_unavailable")
    link_poses = compute_robot_visual_link_poses(
        state,
        joint_names=joint_names,
        joint_positions_rad=joint_positions_rad,
        link_names=link_names,
        reference_frame=reference_frame,
        base_frame=base_frame,
        base_pose=base_pose,
    )
    fk_model = _load_visual_fk_model(state)
    return VisualSceneSnapshotResponse(
        sequence_number=_next_sequence_number(),
        timestamp_ns=time.monotonic_ns(),
        world_frame=reference_frame,
        model_key=fk_model.model_key,
        stale=False,
        robot=robot,
        link_poses=link_poses,
        grippers=list(grippers or []),
        pose_sources=list(pose_sources or []),
    )
