"""Python adapter classes bridging C++ port interfaces to RoboPlan mock objects."""

from __future__ import annotations

import time
import threading
from typing import Any, Dict, List, Optional

import manyforge_core_py
import numpy as np

from manyforge_planning.roboplan_mock import (
    MockRoboPlan,
    MockScene,
    MockRRT,
    MockPathShortcutter,
    MockTOPPRA,
    MockSimpleIk,
    MockJointConfiguration,
    MockJointPath,
    MockJointTrajectory,
)


class RoboPlanPlannerAdapter(manyforge_core_py.IPlannerBackend):
    """Adapter implementing IPlannerBackend using MockRoboPlan components."""

    def __init__(
        self,
        scene: MockScene,
        rrt: MockRRT,
        shortcutter: MockPathShortcutter,
        toppra: MockTOPPRA,
        ik: MockSimpleIk,
    ) -> None:
        # Call parent __init__ as required by pybind11 trampoline
        super().__init__()
        self._scene = scene
        self._rrt = rrt
        self._shortcutter = shortcutter
        self._toppra = toppra
        self._ik = ik

    def plan(self, request: manyforge_core_py.PlanRequestDTO) -> manyforge_core_py.PlanResultDTO:
        """
        Plan a trajectory from start state to goal.

        Args:
            request: The planning request containing start state and goal.

        Returns:
            PlanResultDTO with trajectory on success, or error on failure.
        """
        result = manyforge_core_py.PlanResultDTO()

        try:
            # Extract start state
            start_state = request.start_state
            start_positions = list(start_state.positions_rad)
            joint_names = list(start_state.joint_names)

            start_config = MockJointConfiguration(joint_names, start_positions)

            # Extract goal and plan
            motion = request.motion

            if hasattr(motion, "joint_names"):
                # Joint goal
                goal_positions = list(motion.positions_rad)
                goal_config = MockJointConfiguration(joint_names, goal_positions)

                # Plan path using RRT
                path = self._rrt.plan(start_config, goal_config)

                # Shortcut the path
                smoothed_path = self._shortcutter.shortcut(path)

                # Time-parameterize using TOPPRA
                trajectory = self._toppra.generate(smoothed_path, dt=0.01)

            elif hasattr(motion, "goal"):
                # Pose goal - use IK to convert to joint goal
                goal_pose = motion.goal

                # Create cartesian configuration for IK
                cart_config = MockRoboPlan.CartesianConfiguration(
                    request.base_frame, request.tcp_frame, None
                )

                # Create solution container
                solution = MockJointConfiguration(joint_names, start_positions.copy())

                # Solve IK (simplified - just returns start as placeholder)
                if not self._ik.solveIk(cart_config, start_config, solution):
                    error = manyforge_core_py.ErrorDTO(
                        "IK_FAILED", "Inverse kinematics failed to find solution", {}
                    )
                    result.error = error
                    return result

                goal_positions = list(solution.positions)
                goal_config = MockJointConfiguration(joint_names, goal_positions)

                # Plan path using RRT
                path = self._rrt.plan(start_config, goal_config)

                # Shortcut the path
                smoothed_path = self._shortcutter.shortcut(path)

                # Time-parameterize using TOPPRA
                trajectory = self._toppra.generate(smoothed_path, dt=0.01)

            else:
                error = manyforge_core_py.ErrorDTO(
                    "INVALID_MOTION_TYPE", f"Unknown motion type: {type(motion)}", {}
                )
                result.error = error
                return result

            # Convert MockJointTrajectory to TrajectoryDTO
            waypoints = []
            for i, t in enumerate(trajectory.times):
                wp = manyforge_core_py.TrajectoryWaypointDTO(
                    t,
                    list(trajectory.positions[i]),
                    list(trajectory.velocities[i]) if trajectory.velocities else [],
                    list(trajectory.accelerations[i]) if trajectory.accelerations else [],
                )
                waypoints.append(wp)

            traj_dto = manyforge_core_py.TrajectoryDTO(
                trajectory.joint_names, waypoints
            )
            result.trajectory = traj_dto

        except RuntimeError as e:
            error = manyforge_core_py.ErrorDTO(
                "PLANNING_FAILED", str(e), {}
            )
            result.error = error

        return result


class RoboPlanPlanningContextAdapter(manyforge_core_py.IPlanningContext):
    """Adapter implementing IPlanningContext using MockScene."""

    def __init__(self, scene: MockScene) -> None:
        # Call parent __init__ as required by pybind11 trampoline
        super().__init__()
        self._scene = scene

    def get_current_state(self) -> manyforge_core_py.RobotStateDTO:
        """Get the current robot state."""
        joint_names = self._scene.getActuatedJointNames()
        positions = list(self._scene.getCurrentJointPositions())
        velocities = [0.0] * len(positions)

        return manyforge_core_py.RobotStateDTO(joint_names, positions, velocities)

    def validate_trajectory(
        self,
        trajectory: manyforge_core_py.TrajectoryDTO,
        options: manyforge_core_py.ValidationOptionsDTO,
    ) -> manyforge_core_py.ValidationResultDTO:
        """Validate a trajectory for collisions and kinematic constraints."""
        result = manyforge_core_py.ValidationResultDTO()

        try:
            # Check each waypoint for collisions
            for wp in trajectory.waypoints:
                q = list(wp.positions_rad)
                if self._scene.hasCollisions(q):
                    result.error = manyforge_core_py.ErrorDTO(
                        "COLLISION_DETECTED",
                        "Trajectory passes through collision",
                        {},
                    )
                    return result

            result.valid = True

        except Exception as e:
            result.error = manyforge_core_py.ErrorDTO(
                "VALIDATION_FAILED", str(e), {}
            )

        return result

    def collision_check(
        self, request: manyforge_core_py.CollisionCheckRequestDTO
    ) -> manyforge_core_py.CollisionCheckResultDTO:
        """Check if a robot state is in collision."""
        result = manyforge_core_py.CollisionCheckResultDTO()

        q = list(request.robot_state.positions_rad)
        in_collision = self._scene.hasCollisions(q)
        result.in_collision = in_collision

        return result

    def forward_kinematics(
        self, request: manyforge_core_py.FkRequestDTO
    ) -> manyforge_core_py.Pose3DTO:
        """Compute forward kinematics for a given state."""
        q = list(request.robot_state.positions_rad)
        frame = request.target_frame

        # Get 4x4 transform from scene
        transform = self._scene.forwardKinematics(q, frame)

        # Extract position and orientation
        position = manyforge_core_py.Vector3DTO(
            float(transform[0, 3]), float(transform[1, 3]), float(transform[2, 3])
        )

        # Identity orientation for simplicity
        orientation = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)

        return manyforge_core_py.Pose3DTO(position, orientation)

    def get_joint_limits(
        self, robot_id: str, group_id: str
    ) -> manyforge_core_py.JointLimitsDTO:
        """Get joint limits for a robot group."""
        joint_names = self._scene.getActuatedJointNames()
        n = len(joint_names)

        pos_lower, pos_upper = self._scene.getPositionLimitVectors()
        vel_lower, vel_upper = self._scene.getVelocityLimitVectors()
        accel_lower, accel_upper = self._scene.getAccelerationLimitVectors()

        return manyforge_core_py.JointLimitsDTO(
            joint_names,
            list(pos_lower),
            list(pos_upper),
            list(vel_upper),  # max velocities
            list(accel_upper),  # max accelerations
        )

    def add_object(
        self, request: manyforge_core_py.AddObjectRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        """Add an object to the scene."""
        result = manyforge_core_py.SceneEditResultDTO()

        try:
            name = request.object_id
            shape = request.shape
            pose = request.pose
            parent = request.frame_id

            # Extract position from pose
            pos = pose.position_m
            tform = np.eye(4)
            tform[0, 3] = pos.x_m
            tform[1, 3] = pos.y_m
            tform[2, 3] = pos.z_m

            # Check shape type and add appropriate geometry
            if hasattr(shape, "dimensions_m"):
                # Box shape
                dims = shape.dimensions_m
                box = MockRoboPlan.Box(dims.x_m, dims.y_m, dims.z_m)
                self._scene.addBoxGeometry(name, parent, box, tform, [1, 0, 0, 1])
            elif hasattr(shape, "radius_m"):
                # Sphere shape
                sphere = MockRoboPlan.Sphere(shape.radius_m)
                self._scene.addSphereGeometry(name, parent, sphere, tform, [0, 1, 0, 1])
            else:
                # Default to box
                box = MockRoboPlan.Box(0.1, 0.1, 0.1)
                self._scene.addBoxGeometry(name, parent, box, tform, [1, 0, 0, 1])

            result.success = True

        except Exception as e:
            result.error = manyforge_core_py.ErrorDTO(
                "SCENE_EDIT_FAILED", f"Failed to add object: {e}", {}
            )

        return result

    def remove_object(
        self, request: manyforge_core_py.RemoveObjectRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        """Remove an object from the scene."""
        result = manyforge_core_py.SceneEditResultDTO()

        try:
            name = request.object_id
            self._scene.removeGeometry(name)
            result.success = True

        except Exception as e:
            result.error = manyforge_core_py.ErrorDTO(
                "SCENE_EDIT_FAILED", f"Failed to remove object: {e}", {}
            )

        return result

    def update_object_pose(
        self, request: manyforge_core_py.UpdateObjectPoseRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        """Update the pose of an object in the scene."""
        result = manyforge_core_py.SceneEditResultDTO()

        try:
            name = request.object_id
            pose = request.pose
            parent = request.frame_id

            # Extract position from pose
            pos = pose.position_m
            tform = np.eye(4)
            tform[0, 3] = pos.x_m
            tform[1, 3] = pos.y_m
            tform[2, 3] = pos.z_m

            self._scene.updateGeometryPlacement(name, parent, tform)
            result.success = True

        except Exception as e:
            result.error = manyforge_core_py.ErrorDTO(
                "SCENE_EDIT_FAILED", f"Failed to update object pose: {e}", {}
            )

        return result


class RoboPlanExecutionAdapter(manyforge_core_py.IExecutionBackend):
    """Adapter implementing IExecutionBackend for trajectory execution."""

    def __init__(self) -> None:
        # Call parent __init__ as required by pybind11 trampoline
        super().__init__()
        self._current_trajectory: Optional[manyforge_core_py.TrajectoryDTO] = None
        self._execution_handle: Optional[manyforge_core_py.ExecutionHandle] = None
        self._is_paused = False
        self._is_stopped = False

    def execute_trajectory(
        self, trajectory: manyforge_core_py.TrajectoryDTO
    ) -> manyforge_core_py.ExecutionHandle:
        """Start executing a trajectory."""
        self._current_trajectory = trajectory
        self._is_paused = False
        self._is_stopped = False

        handle = manyforge_core_py.ExecutionHandle()
        self._execution_handle = handle

        return handle

    def stop(self) -> manyforge_core_py.StopResultDTO:
        """Stop the current execution."""
        result = manyforge_core_py.StopResultDTO()

        self._is_stopped = True
        self._is_paused = False
        result.accepted = True

        return result

    def pause(self) -> manyforge_core_py.PauseResultDTO:
        """Pause the current execution."""
        result = manyforge_core_py.PauseResultDTO()

        if self._current_trajectory is not None:
            self._is_paused = True
            result.accepted = True
        else:
            result.error = manyforge_core_py.ErrorDTO(
                "NO_EXECUTION", "No trajectory currently executing", {}
            )

        return result

    def resume(
        self, trajectory: manyforge_core_py.TrajectoryDTO
    ) -> manyforge_core_py.ExecutionHandle:
        """Resume execution from a paused state."""
        self._is_paused = False

        handle = manyforge_core_py.ExecutionHandle()
        self._execution_handle = handle

        return handle

    def reset(self) -> manyforge_core_py.ResetResultDTO:
        """Reset the execution backend to initial state."""
        result = manyforge_core_py.ResetResultDTO()

        self._current_trajectory = None
        self._execution_handle = None
        self._is_paused = False
        self._is_stopped = False
        result.accepted = True

        return result


class WallClock(manyforge_core_py.IClock):
    """Adapter implementing IClock using Python's time module."""

    def __init__(self) -> None:
        # Call parent __init__ as required by pybind11 trampoline
        super().__init__()

    def now(self) -> int:
        """Get current time in nanoseconds."""
        return int(time.time() * 1e9)


class ThreadScheduler(manyforge_core_py.IScheduler):
    """Adapter implementing IScheduler using threading.Timer."""

    def __init__(self) -> None:
        # Call parent __init__ as required by pybind11 trampoline
        super().__init__()
        self._timers: Dict[int, threading.Timer] = {}
        self._handle_counter = 0
        self._lock = threading.Lock()

    def schedule(
        self, deadline: int, callback: callable
    ) -> int:
        """Schedule a callback to run at the given deadline (nanoseconds)."""
        with self._lock:
            self._handle_counter += 1
            handle = self._handle_counter

            # Convert nanoseconds to seconds
            now_ns = int(time.time() * 1e9)
            delay_s = max(0, (deadline - now_ns) / 1e9)

            timer = threading.Timer(delay_s, callback)
            self._timers[handle] = timer
            timer.start()

            return handle

    def cancel(self, handle: int) -> None:
        """Cancel a scheduled callback."""
        with self._lock:
            if handle in self._timers:
                self._timers[handle].cancel()
                del self._timers[handle]
