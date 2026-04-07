"""NativeKernelClient — KernelClient backed by the all-C++ native kernel.

Only 1 Python↔C++ boundary crossing per call (no trampolines).
"""

from __future__ import annotations

from typing import Any, Dict, Optional

from manyforge_behavior.kernel_client import (
    KernelClient,
    ExecutionState,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    ExecutionStatus,
    ExecutionSnapshot,
    SceneEditRequest,
    SceneEditResult,
    Trajectory,
    TrajectoryWaypoint,
)

# State int → ExecutionState mapping (matches C++ ExecutionState enum order)
_STATE_MAP = {
    0: ExecutionState.IDLE,
    1: ExecutionState.PLANNING,
    2: ExecutionState.VALIDATING,
    3: ExecutionState.EXECUTING,
    4: ExecutionState.PAUSED,
    5: ExecutionState.STOPPING,
    6: ExecutionState.STOPPED,
    7: ExecutionState.COMPLETED,
    8: ExecutionState.FAILED,
}


class NativeKernelClient(KernelClient):
    """KernelClient backed by an all-C++ NativeKernelBundle.

    The kernel, planner, planning context, and execution backend are all
    native C++ — no pybind11 trampolines on the hot path.
    """

    def __init__(self, bundle) -> None:
        """Initialize with a NativeKernelBundle from manyforge_roboplan_py.

        Args:
            bundle: NativeKernelBundle returned by create_native_kernel()
        """
        self._bundle = bundle

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        req_dict = {
            "robot_id": request.robot_id,
            "group_id": request.group_id,
            "move_id": request.move_id,
            "base_frame": request.base_frame,
            "tcp_frame": request.tcp_frame,
            "motion_type": request.motion_type,
            "velocity_scale": request.velocity_scale,
            "acceleration_scale": request.acceleration_scale,
            "planning_timeout_s": request.planning_timeout_s,
            "execution_timeout_s": request.execution_timeout_s,
            "joint_position_tol_rad": request.joint_position_tol_rad,
            "cache_mode": request.cache_mode,
        }

        if request.max_tcp_linear_speed_mps is not None:
            req_dict["max_tcp_linear_speed_mps"] = request.max_tcp_linear_speed_mps

        if request.motion_type == "joint_goal":
            req_dict["joint_goal_names"] = request.joint_goal_names
            req_dict["joint_goal_positions_rad"] = request.joint_goal_positions_rad
        elif request.motion_type == "pose_goal":
            req_dict["pose_goal"] = request.pose_goal

        result = self._bundle.execute(req_dict)

        return ExecuteResult(
            ok=result["ok"],
            execution_id=result.get("execution_id", 0),
            error=result.get("error"),
        )

    def stop(self) -> CommandResult:
        r = self._bundle.stop()
        return CommandResult(accepted=r["accepted"], error=r.get("error"))

    def pause(self) -> CommandResult:
        r = self._bundle.pause()
        return CommandResult(accepted=r["accepted"], error=r.get("error"))

    def resume(self) -> CommandResult:
        r = self._bundle.resume()
        return CommandResult(accepted=r["accepted"], error=r.get("error"))

    def reset(self) -> CommandResult:
        r = self._bundle.reset()
        return CommandResult(accepted=r["accepted"], error=r.get("error"))

    def get_status(self) -> ExecutionStatus:
        s = self._bundle.get_status()
        return ExecutionStatus(
            state=_STATE_MAP.get(s["state"], ExecutionState.IDLE),
            execution_id=s.get("execution_id", 0),
            progress=s.get("progress", 0.0),
            active_move_id=s.get("active_move_id", ""),
        )

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        s = self._bundle.get_execution_snapshot()
        return ExecutionSnapshot(
            timestamp_ns=s.get("timestamp_ns", 0),
            sequence_number=s.get("sequence_number", 0),
            execution_id=s.get("execution_id", 0),
            move_id=s.get("move_id", ""),
            state=_STATE_MAP.get(s.get("state", 0), ExecutionState.IDLE),
            progress=s.get("progress", 0.0),
            joint_positions_rad=list(s.get("joint_positions_rad", [])),
            error=s.get("error"),
        )

    def get_metrics(self) -> Dict[str, int]:
        return dict(self._bundle.get_metrics())

    def invalidate_trajectories(self) -> None:
        self._bundle.invalidate_trajectories()

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        req_dict = {
            "operation": request.operation,
            "object_id": request.object_id,
        }
        if request.shape_type:
            req_dict["shape_type"] = request.shape_type
        if request.box_dimensions_m:
            req_dict["box_dimensions_m"] = request.box_dimensions_m
        if request.sphere_radius_m is not None:
            req_dict["sphere_radius_m"] = request.sphere_radius_m
        if request.pose_position_m:
            req_dict["pose_position_m"] = request.pose_position_m
        if request.pose_orientation_xyzw:
            req_dict["pose_orientation_xyzw"] = request.pose_orientation_xyzw
        if request.frame_id:
            req_dict["frame_id"] = request.frame_id

        result = self._bundle.scene_edit(req_dict)
        return SceneEditResult(
            success=result.get("success", False),
            error=result.get("error"),
        )

    def get_cached_trajectory(self, move_id: str) -> Optional[Trajectory]:
        result = self._bundle.get_cached_trajectory(move_id)
        if result is None:
            return None
        waypoints = [
            TrajectoryWaypoint(
                time_from_start_s=wp["time_from_start_s"],
                positions_rad=list(wp["positions_rad"]),
            )
            for wp in result["waypoints"]
        ]
        return Trajectory(
            joint_names=list(result["joint_names"]),
            waypoints=waypoints,
        )

    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        result = self._bundle.get_link_pose(target_frame, base_frame)
        if result is None:
            return None
        return {
            "position_m": list(result["position_m"]),
            "orientation_xyzw": list(result["orientation_xyzw"]),
        }
