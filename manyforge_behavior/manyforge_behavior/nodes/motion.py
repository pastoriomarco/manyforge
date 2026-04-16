"""Motion behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import math
from typing import Any

import py_trees

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    KernelClient,
)
from manyforge_behavior.parameters import resolve_blackboard_value


def _rpy_deg_to_xyzw(rpy_deg: list[float]) -> list[float]:
    """Convert roll/pitch/yaw in degrees to quaternion [x, y, z, w] (ZYX convention)."""
    r, p, y = [math.radians(v) for v in rpy_deg]
    cr, sr = math.cos(r / 2), math.sin(r / 2)
    cp, sp = math.cos(p / 2), math.sin(p / 2)
    cy, sy = math.cos(y / 2), math.sin(y / 2)
    return [
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    ]


def _normalize_pose_dict(pose: dict[str, Any] | None) -> dict[str, Any] | None:
    """Return a pose dict with orientation normalized to orientation_xyzw."""
    if pose is None:
        return None
    if "orientation_rpy_deg" in pose and "orientation_xyzw" not in pose:
        normalized = dict(pose)
        normalized["orientation_xyzw"] = _rpy_deg_to_xyzw(
            normalized.pop("orientation_rpy_deg")
        )
        return normalized
    return pose


class MoveManipulatorAction(py_trees.behaviour.Behaviour):
    """Execute a motion request through the kernel (plan + validate + execute)."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs
        self._started = False
        self._move_id = kwargs.get("move_id", "")
        self._base_velocity_scale = float(kwargs.get("velocity_scale", 1.0))

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        for key in (
            "stop_requested",
            "velocity_scale",
            "global_max_tcp_linear_speed_mps",
        ):
            self.blackboard.register_key(key=key, access=py_trees.common.Access.READ)

    def initialise(self) -> None:
        self._started = False
        self._base_velocity_scale = float(self._params.get("velocity_scale", 1.0))

    def _read_optional_blackboard_value(self, key: str) -> Any | None:
        try:
            return self.blackboard.get(key)
        except (KeyError, AttributeError):
            return None

    def update(self) -> py_trees.common.Status:
        stop_requested = self._read_optional_blackboard_value("stop_requested")
        if stop_requested:
            return py_trees.common.Status.RUNNING  # block, don't start new motion

        effective_velocity_scale = self._base_velocity_scale
        bb_velocity_scale = self._read_optional_blackboard_value("velocity_scale")
        if bb_velocity_scale is not None and 0.0 < bb_velocity_scale <= 1.0:
            effective_velocity_scale *= float(bb_velocity_scale)

        kernel: KernelClient = self.blackboard.kernel

        if not self._started:
            try:
                pose_goal = resolve_blackboard_value(
                    self.blackboard, self._params.get("pose_goal")
                )
                relative_move = resolve_blackboard_value(
                    self.blackboard, self._params.get("relative_move")
                )
            except (AttributeError, KeyError) as exc:
                self.feedback_message = str(exc)
                return py_trees.common.Status.FAILURE

            if pose_goal is not None and not isinstance(pose_goal, dict):
                self.feedback_message = "pose_goal blackboard reference must resolve to a mapping"
                return py_trees.common.Status.FAILURE
            pose_goal = _normalize_pose_dict(pose_goal)

            if relative_move:
                if not isinstance(relative_move, dict):
                    self.feedback_message = "relative_move blackboard reference must resolve to a mapping"
                    return py_trees.common.Status.FAILURE
                relative_move = dict(relative_move)
                target_reference = relative_move.get("target_reference")
                if target_reference:
                    if not isinstance(target_reference, dict):
                        self.feedback_message = "relative_move.target_reference must be a mapping"
                        return py_trees.common.Status.FAILURE
                    target_reference = dict(target_reference)
                    if target_reference.get("type") == "pose":
                        if (
                            target_reference.get("pose") is not None
                            and not isinstance(target_reference.get("pose"), dict)
                        ):
                            self.feedback_message = (
                                "relative_move.target_reference.pose must resolve to a mapping"
                            )
                            return py_trees.common.Status.FAILURE
                        target_reference["pose"] = _normalize_pose_dict(target_reference.get("pose"))
                    relative_move["target_reference"] = target_reference
                offset_reference = relative_move.get("offset_reference")
                if offset_reference:
                    if not isinstance(offset_reference, dict):
                        self.feedback_message = "relative_move.offset_reference must be a mapping"
                        return py_trees.common.Status.FAILURE
                    offset_reference = dict(offset_reference)
                    if offset_reference.get("type") == "pose":
                        if (
                            offset_reference.get("pose") is not None
                            and not isinstance(offset_reference.get("pose"), dict)
                        ):
                            self.feedback_message = (
                                "relative_move.offset_reference.pose must resolve to a mapping"
                            )
                            return py_trees.common.Status.FAILURE
                        offset_reference["pose"] = _normalize_pose_dict(offset_reference.get("pose"))
                    relative_move["offset_reference"] = offset_reference
                if relative_move.get("offset") is not None and not isinstance(
                    relative_move.get("offset"), dict
                ):
                    self.feedback_message = "relative_move.offset must be a mapping"
                    return py_trees.common.Status.FAILURE
                relative_move["offset"] = _normalize_pose_dict(
                    relative_move.get("offset")
                )

            request = ExecuteRequest(
                robot_id=self._params.get("robot_id", ""),
                group_id=self._params.get("group_id", ""),
                move_id=self._params.get("move_id", ""),
                base_frame=self._params.get("base_frame", ""),
                tcp_frame=self._params.get("tcp_frame", ""),
                motion_type=self._params.get("motion_type", "joint_goal"),
                joint_goal_names=self._params.get("joint_goal_names", []),
                joint_goal_positions_rad=self._params.get(
                    "joint_goal_positions_rad", []
                ),
                pose_goal=pose_goal,
                relative_move=relative_move,
                velocity_scale=effective_velocity_scale,
                acceleration_scale=float(
                    self._params.get("acceleration_scale", 1.0)
                ),
                max_tcp_linear_speed_mps=(
                    None
                    if self._params.get("max_tcp_linear_speed_mps") is None
                    else float(self._params.get("max_tcp_linear_speed_mps"))
                ),
                joint_position_tol_rad=float(
                    self._params.get("joint_position_tol_rad", 0.05)
                ),
                translation_tol_m=float(
                    self._params.get("translation_tol_m", 0.005)
                ),
                rotation_tol_rad=float(
                    self._params.get("rotation_tol_rad", 0.05)
                ),
                planning_timeout_s=float(
                    self._params.get("planning_timeout_s", 2.0)
                ),
                execution_timeout_s=float(
                    self._params.get("execution_timeout_s", 10.0)
                ),
                cache_mode=self._params.get("cache_mode", "default"),
            )

            global_cap = self._read_optional_blackboard_value(
                "global_max_tcp_linear_speed_mps"
            )
            if global_cap and global_cap > 0:
                if request.max_tcp_linear_speed_mps and request.max_tcp_linear_speed_mps > 0:
                    request.max_tcp_linear_speed_mps = min(
                        request.max_tcp_linear_speed_mps, global_cap
                    )
                else:
                    request.max_tcp_linear_speed_mps = global_cap

            result = kernel.execute(request)
            self._started = True
            if not result.ok:
                return py_trees.common.Status.FAILURE

        status = kernel.get_status()
        if status.state == ExecutionState.COMPLETED:
            return py_trees.common.Status.SUCCESS
        if status.state == ExecutionState.FAILED:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING


class ResetTrajectories(py_trees.behaviour.Behaviour):
    """Invalidate all cached trajectories."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        kernel.invalidate_trajectories()
        return py_trees.common.Status.SUCCESS
