"""RoboPlan kernel client for motion planning."""

from __future__ import annotations

import numpy as np
from typing import Any, Dict, List, Optional

from manyforge_behavior.kernel_client import (
    KernelClient,
    ExecutionState,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    ExecutionStatus,
    SceneEditRequest,
    SceneEditResult,
)


class RoboPlanKernelClient(KernelClient):
    """Kernel client that uses RoboPlan for motion planning."""

    def __init__(
        self,
        scene,
        rrt,
        shortcutter,
        toppra,
        ik,
        roboplan_module,
        group_name: str = "arm",
    ) -> None:
        """Initialize with pre-built roboplan objects via dependency injection.
        
        Args:
            scene: RoboPlan Scene object
            rrt: RoboPlan RRT planner object
            shortcutter: RoboPlan PathShortcutter object
            toppra: RoboPlan TOPPRA time-parameterization object
            ik: RoboPlan SimpleIk inverse kinematics object
            roboplan_module: Module providing JointConfiguration, hasCollisionsAlongPath, Box, Sphere, CartesianConfiguration
            group_name: Name of the joint group to use
        """
        self._scene = scene
        self._rrt = rrt
        self._shortcutter = shortcutter
        self._toppra = toppra
        self._ik = ik
        self._roboplan_module = roboplan_module
        self._group_name = group_name
        
        # State machine
        self._state = ExecutionState.IDLE
        self._execution_id = 0
        self._next_id = 1
        self._cached_trajectory = None

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        """Plan and validate a trajectory for the given request.
        
        Args:
            request: ExecuteRequest with goal specification
            
        Returns:
            ExecuteResult with success/failure status
        """
        # Check state - must be IDLE
        if self._state != ExecutionState.IDLE:
            return ExecuteResult(ok=False, error="Not in IDLE state")
        
        eid = self._next_id
        self._next_id += 1
        self._execution_id = eid
        self._state = ExecutionState.PLANNING
        
        try:
            # Get current joint positions as start
            start_q = self._scene.getCurrentJointPositions()
            start_config = self._roboplan_module.JointConfiguration(
                self._scene.getActuatedJointNames(), start_q
            )
            
            # Determine goal configuration
            if request.motion_type == "joint_goal":
                # Direct joint goal
                goal_q = np.array(request.joint_goal_positions_rad)
                goal_config = self._roboplan_module.JointConfiguration(
                    request.joint_goal_names, goal_q
                )
            elif request.motion_type == "pose_goal":
                # Pose goal - solve IK first
                if request.pose_goal is None:
                    self._state = ExecutionState.FAILED
                    return ExecuteResult(ok=False, error="pose_goal is None")
                
                # Build CartesianConfiguration from pose_goal
                tform = np.eye(4)
                tform[0, 3] = request.pose_goal.get("position_m", [0.0, 0.0, 0.0])[0]
                tform[1, 3] = request.pose_goal.get("position_m", [0.0, 0.0, 0.0])[1]
                tform[2, 3] = request.pose_goal.get("position_m", [0.0, 0.0, 0.0])[2]
                # Simple orientation handling - assume identity if not specified
                quat = request.pose_goal.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0])
                # Convert quaternion to rotation matrix (simplified)
                x, y, z, w = quat
                tform[0, 0] = 1 - 2*y*y - 2*z*z
                tform[0, 1] = 2*x*y - 2*z*w
                tform[0, 2] = 2*x*z + 2*y*w
                tform[1, 0] = 2*x*y + 2*z*w
                tform[1, 1] = 1 - 2*x*x - 2*z*z
                tform[1, 2] = 2*y*z - 2*x*w
                tform[2, 0] = 2*x*z - 2*y*w
                tform[2, 1] = 2*y*z + 2*x*w
                tform[2, 2] = 1 - 2*x*x - 2*y*y
                
                cart_config = self._roboplan_module.CartesianConfiguration(
                    request.base_frame, request.tcp_frame, tform
                )
                
                # Solve IK
                ik_solution = self._roboplan_module.JointConfiguration(
                    self._scene.getActuatedJointNames(), start_q.copy()
                )
                if not self._ik.solveIk(cart_config, start_config, ik_solution):
                    self._state = ExecutionState.FAILED
                    return ExecuteResult(ok=False, error="IK failed")
                goal_config = ik_solution
            else:
                self._state = ExecutionState.FAILED
                return ExecuteResult(ok=False, error=f"Unknown motion_type: {request.motion_type}")
            
            # Plan path with RRT
            path = self._rrt.plan(start_config, goal_config)
            
            # Shortcut the path
            path = self._shortcutter.shortcut(path)
            
            # Time-parameterize with TOPPRA
            trajectory = self._toppra.generate(
                path,
                dt=0.01,
                velocity_scale=request.velocity_scale,
                acceleration_scale=request.acceleration_scale,
            )
            
            # Validate collision along path
            for i in range(len(path.positions) - 1):
                q0 = path.positions[i]
                q1 = path.positions[i + 1]
                if self._roboplan_module.hasCollisionsAlongPath(self._scene, q0, q1, 0.01):
                    self._state = ExecutionState.FAILED
                    return ExecuteResult(ok=False, error="Collision detected along path")
            
            # Cache trajectory
            self._cached_trajectory = trajectory
            self._state = ExecutionState.COMPLETED
            return ExecuteResult(ok=True, execution_id=eid)
            
        except Exception as e:
            self._state = ExecutionState.FAILED
            return ExecuteResult(ok=False, error=str(e))

    def stop(self) -> CommandResult:
        """Stop the current execution."""
        stoppable = {
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.EXECUTING,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }
        if self._state not in stoppable:
            return CommandResult(accepted=False, error="Cannot stop from current state")
        self._state = ExecutionState.STOPPED
        return CommandResult(accepted=True)

    def pause(self) -> CommandResult:
        """Pause the current execution."""
        if self._state != ExecutionState.EXECUTING:
            return CommandResult(accepted=False, error="Can only pause from EXECUTING")
        self._state = ExecutionState.PAUSED
        return CommandResult(accepted=True)

    def resume(self) -> CommandResult:
        """Resume a paused execution."""
        if self._state != ExecutionState.PAUSED:
            return CommandResult(accepted=False, error="Can only resume from PAUSED")
        self._state = ExecutionState.EXECUTING
        return CommandResult(accepted=True)

    def reset(self) -> CommandResult:
        """Reset to IDLE state."""
        if self._state == ExecutionState.IDLE:
            return CommandResult(accepted=False, error="Already in IDLE")
        self._state = ExecutionState.IDLE
        return CommandResult(accepted=True)

    def get_status(self) -> ExecutionStatus:
        """Get current execution status."""
        progress = 1.0 if self._state == ExecutionState.COMPLETED else 0.0
        return ExecutionStatus(
            state=self._state,
            execution_id=self._execution_id,
            progress=progress,
        )

    def invalidate_trajectories(self) -> None:
        """Clear cached trajectory."""
        self._cached_trajectory = None

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        """Edit the scene geometry.
        
        Args:
            request: SceneEditRequest with operation details
            
        Returns:
            SceneEditResult with success/failure status
        """
        try:
            # Build transform matrix from pose
            tform = np.eye(4)
            if request.pose_position_m:
                tform[0, 3] = request.pose_position_m[0]
                tform[1, 3] = request.pose_position_m[1]
                tform[2, 3] = request.pose_position_m[2]
            if request.pose_orientation_xyzw:
                x, y, z, w = request.pose_orientation_xyzw
                tform[0, 0] = 1 - 2*y*y - 2*z*z
                tform[0, 1] = 2*x*y - 2*z*w
                tform[0, 2] = 2*x*z + 2*y*w
                tform[1, 0] = 2*x*y + 2*z*w
                tform[1, 1] = 1 - 2*x*x - 2*z*z
                tform[1, 2] = 2*y*z - 2*x*w
                tform[2, 0] = 2*x*z - 2*y*w
                tform[2, 1] = 2*y*z + 2*x*w
                tform[2, 2] = 1 - 2*x*x - 2*y*y
            
            if request.operation == "add":
                if request.shape_type == "box":
                    if request.box_dimensions_m:
                        box = self._roboplan_module.Box(
                            request.box_dimensions_m[0],
                            request.box_dimensions_m[1],
                            request.box_dimensions_m[2],
                        )
                        self._scene.addBoxGeometry(request.object_id, "universe", box, tform, [1, 0, 0, 1])
                    else:
                        return SceneEditResult(success=False, error="box_dimensions_m required for box")
                elif request.shape_type == "sphere":
                    if request.sphere_radius_m is not None:
                        sphere = self._roboplan_module.Sphere(request.sphere_radius_m)
                        self._scene.addSphereGeometry(request.object_id, "universe", sphere, tform, [0, 1, 0, 1])
                    else:
                        return SceneEditResult(success=False, error="sphere_radius_m required for sphere")
                else:
                    return SceneEditResult(success=False, error=f"Unknown shape_type: {request.shape_type}")
            elif request.operation == "remove":
                self._scene.removeGeometry(request.object_id)
            elif request.operation == "update_pose":
                self._scene.updateGeometryPlacement(request.object_id, "universe", tform)
            else:
                return SceneEditResult(success=False, error=f"Unknown operation: {request.operation}")
            
            return SceneEditResult(success=True)
        except ValueError as e:
            return SceneEditResult(success=False, error=str(e))
        except Exception as e:
            return SceneEditResult(success=False, error=str(e))

    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        """Get the pose of a link/frame via forward kinematics.
        
        Args:
            target_frame: The frame to query
            base_frame: The base frame
            
        Returns:
            Dict with position_m and orientation_xyzw, or None on failure
        """
        try:
            # Get current joint positions
            q = self._scene.getCurrentJointPositions()
            
            # Forward kinematics
            tform = self._scene.forwardKinematics(q, target_frame)
            
            # Extract position
            position = [float(tform[0, 3]), float(tform[1, 3]), float(tform[2, 3])]
            
            # Extract 3x3 rotation matrix and convert to quaternion
            R = tform[:3, :3]
            orientation = self._rotation_matrix_to_quaternion(R)
            
            return {
                "position_m": position,
                "orientation_xyzw": orientation,
            }
        except Exception:
            return None

    def _rotation_matrix_to_quaternion(self, R: np.ndarray) -> List[float]:
        """Convert a 3x3 rotation matrix to a quaternion [x, y, z, w].
        
        Uses the method from Shoemake (1985) that handles all cases robustly.
        
        Args:
            R: 3x3 rotation matrix
            
        Returns:
            Quaternion as [x, y, z, w]
        """
        # Compute trace
        trace = R[0, 0] + R[1, 1] + R[2, 2]
        
        if trace > 0:
            s = np.sqrt(trace + 1.0) * 2
            w = 0.25 * s
            x = (R[2, 1] - R[1, 2]) / s
            y = (R[0, 2] - R[2, 0]) / s
            z = (R[1, 0] - R[0, 1]) / s
        elif R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
            s = np.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2
            w = (R[2, 1] - R[1, 2]) / s
            x = 0.25 * s
            y = (R[0, 1] + R[1, 0]) / s
            z = (R[0, 2] + R[2, 0]) / s
        elif R[1, 1] > R[2, 2]:
            s = np.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2
            w = (R[0, 2] - R[2, 0]) / s
            x = (R[0, 1] + R[1, 0]) / s
            y = 0.25 * s
            z = (R[1, 2] + R[2, 1]) / s
        else:
            s = np.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2
            w = (R[1, 0] - R[0, 1]) / s
            x = (R[0, 2] + R[2, 0]) / s
            y = (R[1, 2] + R[2, 1]) / s
            z = 0.25 * s
        
        return [float(x), float(y), float(z), float(w)]
