"""CppKernelClient - Python wrapper for ExecutionKernel implementing KernelClient."""

from __future__ import annotations

import manyforge_core_py
from manyforge_behavior.kernel_client import (
    KernelClient,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    ExecutionStatus,
    ExecutionState,
    SceneEditRequest,
    SceneEditResult,
)


class CppKernelClient(KernelClient):
    """
    A Python class that implements manyforge_behavior.kernel_client.KernelClient
    by wrapping a manyforge_core_py.ExecutionKernel instance.

    The adapter translates between Python dataclass DTOs and C++ bound DTOs.
    """

    def __init__(
        self,
        planner_backend: manyforge_core_py.IPlannerBackend,
        planning_context: manyforge_core_py.IPlanningContext,
        execution_backend: manyforge_core_py.IExecutionBackend,
        clock: manyforge_core_py.IClock,
        scheduler: manyforge_core_py.IScheduler,
        cache: manyforge_core_py.TrajectoryCache | None = None,
    ) -> None:
        """
        Initialize the CppKernelClient with the required backends.

        Args:
            planner_backend: The planner backend implementation.
            planning_context: The planning context implementation.
            execution_backend: The execution backend implementation.
            clock: The clock implementation.
            scheduler: The scheduler implementation.
            cache: Optional trajectory cache (creates default if not provided).
        """
        if cache is None:
            cache = manyforge_core_py.TrajectoryCache()

        # Keep Python references to prevent GC while C++ kernel holds raw refs
        self._planner_backend = planner_backend
        self._planning_context = planning_context
        self._execution_backend = execution_backend
        self._clock = clock
        self._scheduler = scheduler

        self._kernel = manyforge_core_py.ExecutionKernel(
            planner_backend,
            planning_context,
            execution_backend,
            clock,
            scheduler,
            cache,
        )
        self._cache = cache

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        """
        Execute a motion request.

        Args:
            request: The execute request.

        Returns:
            The execute result.
        """
        # Convert Python ExecuteRequest to C++ ExecuteRequestDTO
        tolerances = manyforge_core_py.TolerancesDTO(
            request.joint_position_tol_rad,
            request.translation_tol_m,
            request.rotation_tol_rad,
        )
        speed_profile = manyforge_core_py.SpeedProfileDTO(
            request.velocity_scale,
            request.acceleration_scale,
            request.max_tcp_linear_speed_mps,
        )
        timeouts = manyforge_core_py.TimeoutsDTO(
            request.planning_timeout_s,
            request.execution_timeout_s,
        )

        # Convert motion type to appropriate DTO
        if request.motion_type == "joint_goal":
            motion = manyforge_core_py.JointGoalDTO(
                request.joint_goal_names,
                request.joint_goal_positions_rad,
            )
        elif request.motion_type == "pose_goal":
            if request.pose_goal is None:
                return ExecuteResult(ok=False, error="pose_goal required for pose_goal motion type")
            pos = manyforge_core_py.Vector3DTO(
                request.pose_goal["position_m"][0],
                request.pose_goal["position_m"][1],
                request.pose_goal["position_m"][2],
            )
            orient = manyforge_core_py.QuaternionDTO(
                request.pose_goal["orientation_xyzw"][0],
                request.pose_goal["orientation_xyzw"][1],
                request.pose_goal["orientation_xyzw"][2],
                request.pose_goal["orientation_xyzw"][3],
            )
            goal_pose = manyforge_core_py.Pose3DTO(pos, orient)
            motion = manyforge_core_py.PoseGoalDTO(goal_pose)
        else:
            return ExecuteResult(ok=False, error=f"Unknown motion type: {request.motion_type}")

        cache_mode = (
            manyforge_core_py.CacheMode.kDefault
            if request.cache_mode == "default"
            else manyforge_core_py.CacheMode.kDisable
        )

        execute_request = manyforge_core_py.ExecuteRequestDTO(
            request.robot_id,
            request.group_id,
            request.move_id,
            request.base_frame,
            request.tcp_frame,
            tolerances,
            speed_profile,
            timeouts,
            cache_mode,
            motion,
        )

        try:
            execution_id = self._kernel.execute(execute_request)
            return ExecuteResult(ok=True, execution_id=execution_id)
        except RuntimeError as e:
            return ExecuteResult(ok=False, error=str(e))

    def stop(self) -> CommandResult:
        """
        Stop the current execution.

        Returns:
            The command result.
        """
        result = self._kernel.stop()
        return CommandResult(accepted=result.accepted, error=result.error if result.error else None)

    def pause(self) -> CommandResult:
        """
        Pause the current execution.

        Returns:
            The command result.
        """
        result = self._kernel.pause()
        return CommandResult(accepted=result.accepted, error=result.error if result.error else None)

    def resume(self) -> CommandResult:
        """
        Resume the paused execution.

        Returns:
            The command result.
        """
        result = self._kernel.resume()
        return CommandResult(accepted=result.accepted, error=result.error if result.error else None)

    def reset(self) -> CommandResult:
        """
        Reset the kernel to the idle state.

        Returns:
            The command result.
        """
        result = self._kernel.reset()
        return CommandResult(accepted=result.accepted, error=result.error if result.error else None)

    def get_status(self) -> ExecutionStatus:
        """
        Get the current execution status.

        Returns:
            The execution status.
        """
        status = self._kernel.get_status()
        state = self._cpp_state_to_python(status.state)
        return ExecutionStatus(
            state=state,
            execution_id=status.execution_id,
            progress=status.progress,
        )

    def invalidate_trajectories(self) -> None:
        """Invalidate all cached trajectories."""
        self._cache.invalidate_all()

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        """
        Edit the scene (add/remove/update objects).

        Args:
            request: The scene edit request.

        Returns:
            The scene edit result.
        """
        # This is a stub implementation - scene editing is not directly
        # supported by the ExecutionKernel. The planning context handles
        # scene editing, but it's not exposed through the kernel interface.
        return SceneEditResult(success=False, error="Scene editing not supported through kernel")

    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> dict[str, float] | None:
        """
        Get the pose of a link/frame.

        Args:
            target_frame: The target frame.
            base_frame: The base frame.

        Returns:
            The pose as a dictionary, or None if not available.
        """
        # This is a stub implementation - link pose queries are not directly
        # supported by the ExecutionKernel. The planning context handles
        # forward kinematics, but it's not exposed through the kernel interface.
        return None

    def _cpp_state_to_python(self, cpp_state: manyforge_core_py.ExecutionState) -> ExecutionState:
        """Convert C++ ExecutionState to Python ExecutionState."""
        state_map = {
            manyforge_core_py.ExecutionState.kIdle: ExecutionState.IDLE,
            manyforge_core_py.ExecutionState.kPlanning: ExecutionState.PLANNING,
            manyforge_core_py.ExecutionState.kValidating: ExecutionState.VALIDATING,
            manyforge_core_py.ExecutionState.kExecuting: ExecutionState.EXECUTING,
            manyforge_core_py.ExecutionState.kPaused: ExecutionState.PAUSED,
            manyforge_core_py.ExecutionState.kStopping: ExecutionState.STOPPING,
            manyforge_core_py.ExecutionState.kStopped: ExecutionState.STOPPED,
            manyforge_core_py.ExecutionState.kCompleted: ExecutionState.COMPLETED,
            manyforge_core_py.ExecutionState.kFailed: ExecutionState.FAILED,
        }
        return state_map.get(cpp_state, ExecutionState.IDLE)
