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


class MoveManipulatorAction(py_trees.behaviour.Behaviour):
    """Execute a motion request through the kernel (plan + validate + execute)."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs
        self._started = False
        self._move_id = kwargs.get("move_id", "")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def initialise(self) -> None:
        self._started = False

    def update(self) -> py_trees.common.Status:
        # Check stop flag — block new motions if stop is requested
        try:
            stop_bb = py_trees.blackboard.Client(name=self.name + "_stop")
            stop_bb.register_key(key="stop_requested", access=py_trees.common.Access.READ)
            if stop_bb.stop_requested:
                return py_trees.common.Status.RUNNING  # block, don't start new motion
        except (KeyError, AttributeError):
            pass  # No stop flag on blackboard, continue normally

        # Read velocity_scale from blackboard and multiply with per-move scale
        try:
            vs_bb = py_trees.blackboard.Client(name=self.name + "_vs")
            vs_bb.register_key(key="velocity_scale", access=py_trees.common.Access.READ)
            bb_velocity_scale = vs_bb.velocity_scale
            if bb_velocity_scale is not None and 0.0 < bb_velocity_scale <= 1.0:
                self._params["velocity_scale"] = float(self._params.get("velocity_scale", 1.0)) * bb_velocity_scale
        except (KeyError, AttributeError):
            pass

        kernel: KernelClient = self.blackboard.kernel

        if not self._started:
            # Convert orientation_rpy_deg to orientation_xyzw if needed
            pose_goal = self._params.get("pose_goal")
            if pose_goal and "orientation_rpy_deg" in pose_goal and "orientation_xyzw" not in pose_goal:
                pose_goal = dict(pose_goal)
                pose_goal["orientation_xyzw"] = _rpy_deg_to_xyzw(pose_goal.pop("orientation_rpy_deg"))

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
                velocity_scale=float(self._params.get("velocity_scale", 1.0)),
                acceleration_scale=float(
                    self._params.get("acceleration_scale", 1.0)
                ),
                planning_timeout_s=float(
                    self._params.get("planning_timeout_s", 2.0)
                ),
                execution_timeout_s=float(
                    self._params.get("execution_timeout_s", 10.0)
                ),
                cache_mode=self._params.get("cache_mode", "default"),
            )

            # Clamp per-move TCP speed to global deployment cap
            try:
                cap_bb = py_trees.blackboard.Client(name=self.name + "_tcp_cap")
                cap_bb.register_key(key="global_max_tcp_linear_speed_mps", access=py_trees.common.Access.READ)
                global_cap = cap_bb.global_max_tcp_linear_speed_mps
                if global_cap and global_cap > 0:
                    if request.max_tcp_linear_speed_mps and request.max_tcp_linear_speed_mps > 0:
                        request.max_tcp_linear_speed_mps = min(request.max_tcp_linear_speed_mps, global_cap)
                    else:
                        request.max_tcp_linear_speed_mps = global_cap
            except (KeyError, AttributeError):
                pass  # No deployment loaded, no clamping

            result = kernel.execute(request)
            self._started = True
            if not result.ok:
                return py_trees.common.Status.FAILURE

        status = kernel.get_status()
        if status.state == ExecutionState.COMPLETED:
            # Fire move-complete callback for trajectory animation
            try:
                cb_bb = py_trees.blackboard.Client(name=self.name + "_cb_read")
                cb_bb.register_key(
                    key="move_complete_callback", access=py_trees.common.Access.READ
                )
                callback = cb_bb.move_complete_callback
                if callback and self._move_id:
                    callback(self._move_id, kernel)
            except (KeyError, AttributeError):
                pass
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
