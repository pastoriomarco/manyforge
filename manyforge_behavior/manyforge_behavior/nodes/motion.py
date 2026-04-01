"""Motion behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    KernelClient,
)


class MoveManipulatorAction(py_trees.behaviour.Behaviour):
    """Execute a motion request through the kernel (plan + validate + execute)."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs
        self._started = False

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def initialise(self) -> None:
        self._started = False

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel

        if not self._started:
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
                pose_goal=self._params.get("pose_goal"),
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
