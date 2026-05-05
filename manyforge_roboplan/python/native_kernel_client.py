"""NativeKernelClient — KernelClient backed by the all-C++ native kernel.

Only 1 Python↔C++ boundary crossing per call (no trampolines).
"""

from __future__ import annotations

import math
import time
from typing import Any, Dict, Optional

from manyforge_behavior.kernel_client import (
    KernelClient,
    GripperCommandRequest,
    GripperCommandState,
    GripperCommandStatus,
    ExecutionState,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    ExecutionStatus,
    ExecutionSnapshot,
    MotionPreviewResult,
    ObjectState,
    RobotStateSnapshot,
    SceneObjectSnapshot,
    SceneShape,
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


def _rpy_deg_to_xyzw(rpy_deg):
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


def _normalize_pose_dict(pose: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if pose is None:
        return None
    if "orientation_rpy_deg" in pose and "orientation_xyzw" not in pose:
        normalized = dict(pose)
        normalized["orientation_xyzw"] = _rpy_deg_to_xyzw(
            normalized.pop("orientation_rpy_deg")
        )
        return normalized
    return pose


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
        self._active_gripper_commands: Dict[str, Dict[str, Any]] = {}
        self._gripper_status_cache: Dict[str, GripperCommandStatus] = {}

    def _get_group_joint_index(self, group_id: str, joint_name: str) -> int:
        joint_names = list(self._bundle.get_group_joint_names(group_id))
        if joint_name not in joint_names:
            raise ValueError(
                f"Joint '{joint_name}' was not found in gripper group '{group_id}'"
            )
        return joint_names.index(joint_name)

    def _get_group_joint_positions(self, group_id: str) -> list[float]:
        return list(self._bundle.get_group_joint_positions(group_id))

    def _compute_gripper_command_status(self, gripper_id: str) -> GripperCommandStatus:
        command = self._active_gripper_commands.get(gripper_id)
        if command is None:
            return self._gripper_status_cache.get(
                gripper_id,
                GripperCommandStatus(
                    state=GripperCommandState.IDLE,
                    gripper_id=gripper_id,
                ),
            )

        elapsed = time.monotonic() - float(command["start_time"])
        duration = float(command["duration_s"])
        progress = min(max(elapsed / duration, 0.0), 1.0)
        current_position = (
            float(command["start_position"])
            + (float(command["target_position"]) - float(command["start_position"])) * progress
        )
        group_positions = self._get_group_joint_positions(command["group_id"])
        group_positions[command["joint_index"]] = current_position
        self._bundle.set_group_joint_positions(command["group_id"], group_positions)

        status = GripperCommandStatus(
            state=(
                GripperCommandState.COMPLETED
                if progress >= 1.0
                else GripperCommandState.EXECUTING
            ),
            gripper_id=gripper_id,
            progress=progress,
            current_position=current_position,
            target_position=float(command["target_position"]),
        )
        self._gripper_status_cache[gripper_id] = status
        if status.state == GripperCommandState.COMPLETED:
            self._active_gripper_commands.pop(gripper_id, None)
        return status

    def update_gripper_commands(self) -> None:
        for gripper_id in list(self._active_gripper_commands):
            self._compute_gripper_command_status(gripper_id)

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        relative_move = request.relative_move
        if relative_move is not None:
            relative_move = dict(relative_move)
            target_reference = relative_move.get("target_reference")
            if target_reference is not None:
                target_reference = dict(target_reference)
                if target_reference.get("type") == "pose":
                    target_reference["pose"] = _normalize_pose_dict(target_reference.get("pose"))
                relative_move["target_reference"] = target_reference
            offset_reference = relative_move.get("offset_reference")
            if offset_reference is not None:
                offset_reference = dict(offset_reference)
                if offset_reference.get("type") == "pose":
                    offset_reference["pose"] = _normalize_pose_dict(offset_reference.get("pose"))
                relative_move["offset_reference"] = offset_reference
            relative_move["offset"] = _normalize_pose_dict(relative_move.get("offset"))

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
        elif request.motion_type == "relative_move":
            req_dict["relative_move"] = relative_move

        result = self._bundle.execute(req_dict)

        return ExecuteResult(
            ok=result["ok"],
            execution_id=result.get("execution_id", 0),
            error=result.get("error"),
        )

    def preview_motion(
        self,
        request: ExecuteRequest,
        start_state: Optional[RobotStateSnapshot] = None,
    ) -> MotionPreviewResult:
        relative_move = request.relative_move
        if relative_move is not None:
            relative_move = dict(relative_move)
            target_reference = relative_move.get("target_reference")
            if target_reference is not None:
                target_reference = dict(target_reference)
                if target_reference.get("type") == "pose":
                    target_reference["pose"] = _normalize_pose_dict(target_reference.get("pose"))
                relative_move["target_reference"] = target_reference
            offset_reference = relative_move.get("offset_reference")
            if offset_reference is not None:
                offset_reference = dict(offset_reference)
                if offset_reference.get("type") == "pose":
                    offset_reference["pose"] = _normalize_pose_dict(offset_reference.get("pose"))
                relative_move["offset_reference"] = offset_reference
            relative_move["offset"] = _normalize_pose_dict(relative_move.get("offset"))

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
            "translation_tol_m": request.translation_tol_m,
            "rotation_tol_rad": request.rotation_tol_rad,
            "cache_mode": request.cache_mode,
        }

        if request.max_tcp_linear_speed_mps is not None:
            req_dict["max_tcp_linear_speed_mps"] = request.max_tcp_linear_speed_mps

        if request.motion_type == "joint_goal":
            req_dict["joint_goal_names"] = request.joint_goal_names
            req_dict["joint_goal_positions_rad"] = request.joint_goal_positions_rad
        elif request.motion_type == "pose_goal":
            req_dict["pose_goal"] = request.pose_goal
        elif request.motion_type == "relative_move":
            req_dict["relative_move"] = relative_move

        start_state_dict = None
        if start_state is not None:
            start_state_dict = {
                "joint_names": list(start_state.joint_names),
                "positions_rad": list(start_state.positions_rad),
                "velocities_rad_s": list(start_state.velocities_rad_s),
            }

        result = self._bundle.preview_motion(req_dict, start_state_dict)
        final_state = result.get("final_state")
        return MotionPreviewResult(
            ok=result.get("ok", False),
            error=result.get("error"),
            error_details=(
                None
                if result.get("error_details") is None
                else dict(result.get("error_details", {}))
            ),
            trajectory_duration_s=result.get("trajectory_duration_s"),
            trajectory_waypoint_count=result.get("trajectory_waypoint_count"),
            final_state=(
                None
                if final_state is None
                else RobotStateSnapshot(
                    joint_names=list(final_state.get("joint_names", [])),
                    positions_rad=list(final_state.get("positions_rad", [])),
                    velocities_rad_s=list(final_state.get("velocities_rad_s", [])),
                )
            ),
            final_tcp_pose=(
                None
                if result.get("final_tcp_pose") is None
                else {
                    "position_m": list(result["final_tcp_pose"]["position_m"]),
                    "orientation_xyzw": list(result["final_tcp_pose"]["orientation_xyzw"]),
                }
            ),
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

    def acknowledge_completed(self, execution_id: int) -> CommandResult:
        r = self._bundle.acknowledge_completed(int(execution_id))
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

    def get_scene_object_ids(self) -> list[str]:
        return list(self._bundle.get_scene_object_ids())

    def get_joint_names(self) -> list[str]:
        return list(self._bundle.get_joint_names())

    def get_scene_objects(self) -> list[SceneObjectSnapshot]:
        objects: list[SceneObjectSnapshot] = []
        for object_data in self._bundle.get_scene_objects():
            shape_data = object_data.get("shape")
            shape = None
            if shape_data is not None:
                shape = SceneShape(
                    type=shape_data["type"],
                    box_dimensions_m=list(shape_data["box_dimensions_m"])
                    if shape_data.get("box_dimensions_m") is not None
                    else None,
                    sphere_radius_m=shape_data.get("sphere_radius_m"),
                    cylinder_radius_m=shape_data.get("cylinder_radius_m"),
                    cylinder_height_m=shape_data.get("cylinder_height_m"),
                    mesh_resource_uri=shape_data.get("mesh_resource_uri"),
                    mesh_scale=list(shape_data["mesh_scale"])
                    if shape_data.get("mesh_scale") is not None
                    else None,
                )
            pose_in_universe = object_data.get("pose_in_universe")
            if pose_in_universe is not None:
                pose_in_universe = {
                    "position_m": list(pose_in_universe["position_m"]),
                    "orientation_xyzw": list(pose_in_universe["orientation_xyzw"]),
                }
            objects.append(
                SceneObjectSnapshot(
                    object_id=object_data["object_id"],
                    shape=shape,
                    pose_in_universe=pose_in_universe,
                    attached=bool(object_data.get("attached", False)),
                    frame_id=object_data.get("frame_id"),
                )
            )
        return objects

    def get_metrics(self) -> Dict[str, int]:
        return dict(self._bundle.get_metrics())

    def command_gripper(self, request: GripperCommandRequest) -> CommandResult:
        self.update_gripper_commands()
        current = self.get_gripper_status(request.gripper_id)
        if current.state == GripperCommandState.EXECUTING:
            return CommandResult(
                accepted=False,
                error=f"Gripper '{request.gripper_id}' is already executing a command",
            )

        try:
            joint_index = self._get_group_joint_index(
                request.group_id,
                request.joint_name,
            )
            group_positions = self._get_group_joint_positions(request.group_id)
        except Exception as exc:
            return CommandResult(accepted=False, error=str(exc))

        start_position = float(group_positions[joint_index])
        if request.duration_s <= 0.0:
            group_positions[joint_index] = float(request.target_position)
            self._bundle.set_group_joint_positions(request.group_id, group_positions)
            self._gripper_status_cache[request.gripper_id] = GripperCommandStatus(
                state=GripperCommandState.COMPLETED,
                gripper_id=request.gripper_id,
                progress=1.0,
                current_position=float(request.target_position),
                target_position=float(request.target_position),
            )
            self._active_gripper_commands.pop(request.gripper_id, None)
            return CommandResult(accepted=True)

        self._active_gripper_commands[request.gripper_id] = {
            "group_id": request.group_id,
            "joint_name": request.joint_name,
            "joint_index": joint_index,
            "start_position": start_position,
            "target_position": float(request.target_position),
            "duration_s": float(request.duration_s),
            "start_time": time.monotonic(),
        }
        self._gripper_status_cache[request.gripper_id] = GripperCommandStatus(
            state=GripperCommandState.EXECUTING,
            gripper_id=request.gripper_id,
            progress=0.0,
            current_position=start_position,
            target_position=float(request.target_position),
        )
        return CommandResult(accepted=True)

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        return self._gripper_status_cache.get(
            gripper_id,
            GripperCommandStatus(
                state=GripperCommandState.IDLE,
                gripper_id=gripper_id,
            ),
        )

    def drain_kernel_events(self) -> list[dict]:
        return list(self._bundle.drain_kernel_events())

    def get_scene_trace(self, limit: Optional[int] = None) -> list[dict]:
        return list(self._bundle.get_scene_trace(-1 if limit is None else int(limit)))

    def clear_scene_trace(self) -> None:
        self._bundle.clear_scene_trace()

    def invalidate_trajectories(self) -> None:
        self._bundle.invalidate_trajectories()

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        if request.operation == "upsert":
            state = self.get_object_state(request.object_id)
            if state.exists and state.attached:
                return SceneEditResult(success=False, error="Cannot upsert attached object")
            # Skip the no-op remove when the object doesn't exist yet.
            # Each remove triggers an LIFO replay in the C++ context, so a
            # redundant "Could not find object" round trip is pure noise in the
            # scene trace and forces the planner to churn pairs unnecessarily.
            if state.exists:
                remove_result = self.scene_edit(
                    SceneEditRequest(operation="remove", object_id=request.object_id)
                )
                if not remove_result.success:
                    return remove_result
            return self.scene_edit(
                SceneEditRequest(
                    operation="add",
                    object_id=request.object_id,
                    shape_type=request.shape_type,
                    box_dimensions_m=request.box_dimensions_m,
                    sphere_radius_m=request.sphere_radius_m,
                    pose_position_m=request.pose_position_m,
                    pose_orientation_xyzw=request.pose_orientation_xyzw,
                    frame_id=request.frame_id,
                    touch_links=request.touch_links,
                )
            )

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
        if request.touch_links is not None:
            req_dict["touch_links"] = list(request.touch_links)

        try:
            result = self._bundle.scene_edit(req_dict)
        except Exception as exc:
            return SceneEditResult(success=False, error=str(exc))
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

    def get_object_pose(
        self, object_id: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        result = self._bundle.get_object_pose(object_id, base_frame)
        if result is None:
            return None
        return {
            "position_m": list(result["position_m"]),
            "orientation_xyzw": list(result["orientation_xyzw"]),
        }

    def get_object_state(self, object_id: str) -> ObjectState:
        result = self._bundle.get_object_state(object_id)
        return ObjectState(
            object_id=result.get("object_id", object_id),
            exists=result.get("exists", False),
            attached=result.get("attached", False),
            frame_id=result.get("frame_id"),
        )
