"""Abstract kernel interface and mock implementation for testing."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional


class ExecutionState(Enum):
    IDLE = "idle"
    PLANNING = "planning"
    VALIDATING = "validating"
    EXECUTING = "executing"
    PAUSED = "paused"
    STOPPING = "stopping"
    STOPPED = "stopped"
    COMPLETED = "completed"
    FAILED = "failed"


@dataclass
class ExecuteRequest:
    robot_id: str
    group_id: str
    move_id: str
    base_frame: str
    tcp_frame: str
    motion_type: str  # "joint_goal" or "pose_goal"
    joint_goal_names: List[str] = field(default_factory=list)
    joint_goal_positions_rad: List[float] = field(default_factory=list)
    pose_goal: Optional[Dict[str, Any]] = None
    velocity_scale: float = 1.0
    acceleration_scale: float = 1.0
    max_tcp_linear_speed_mps: Optional[float] = None
    joint_position_tol_rad: float = 0.05
    translation_tol_m: float = 0.005
    rotation_tol_rad: float = 0.05
    planning_timeout_s: float = 2.0
    execution_timeout_s: float = 10.0
    cache_mode: str = "default"


@dataclass
class ExecutionStatus:
    state: ExecutionState = ExecutionState.IDLE
    execution_id: int = 0
    progress: float = 0.0


@dataclass
class CommandResult:
    accepted: bool = False
    error: Optional[str] = None


@dataclass
class ExecuteResult:
    ok: bool = False
    execution_id: int = 0
    error: Optional[str] = None
    trajectory_duration_s: Optional[float] = None
    trajectory_waypoint_count: Optional[int] = None


@dataclass
class SceneEditRequest:
    operation: str  # "add", "remove", "update_pose"
    object_id: str
    shape_type: Optional[str] = None
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    pose_position_m: Optional[List[float]] = None
    pose_orientation_xyzw: Optional[List[float]] = None
    frame_id: Optional[str] = None


@dataclass
class SceneEditResult:
    success: bool = False
    error: Optional[str] = None


class KernelClient(ABC):
    @abstractmethod
    def execute(self, request: ExecuteRequest) -> ExecuteResult: ...

    @abstractmethod
    def stop(self) -> CommandResult: ...

    @abstractmethod
    def pause(self) -> CommandResult: ...

    @abstractmethod
    def resume(self) -> CommandResult: ...

    @abstractmethod
    def reset(self) -> CommandResult: ...

    @abstractmethod
    def get_status(self) -> ExecutionStatus: ...

    @abstractmethod
    def invalidate_trajectories(self) -> None: ...

    @abstractmethod
    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult: ...

    @abstractmethod
    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]: ...


class MockKernelClient(KernelClient):
    """Mock kernel for testing. Simulates state transitions and records calls."""

    def __init__(self) -> None:
        self._state = ExecutionState.IDLE
        self._execution_id = 0
        self._next_id = 1
        self.execute_count = 0
        self.stop_count = 0
        self.pause_count = 0
        self.resume_count = 0
        self.reset_count = 0
        self.invalidate_count = 0
        self.scene_edits: List[SceneEditRequest] = []
        self.link_pose_calls: List[tuple[str, str]] = []
        # Allow tests to inject failures
        self.fail_next_execute = False
        self.fail_next_scene_edit = False

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        self.execute_count += 1
        # Auto-reset from terminal states, matching C++ ExecutionKernel behavior
        terminal_states = {ExecutionState.COMPLETED, ExecutionState.FAILED, ExecutionState.STOPPED}
        if self._state in terminal_states:
            self._state = ExecutionState.IDLE
        if self._state != ExecutionState.IDLE:
            return ExecuteResult(ok=False, error="Not in IDLE state")
        if self.fail_next_execute:
            self.fail_next_execute = False
            self._state = ExecutionState.FAILED
            return ExecuteResult(ok=False, error="Execution failed (injected)")
        eid = self._next_id
        self._next_id += 1
        self._execution_id = eid
        self._state = ExecutionState.COMPLETED
        return ExecuteResult(ok=True, execution_id=eid)

    def stop(self) -> CommandResult:
        self.stop_count += 1
        stoppable = {
            ExecutionState.EXECUTING,
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }
        if self._state not in stoppable:
            return CommandResult(accepted=False, error="Cannot stop from current state")
        self._state = ExecutionState.STOPPED
        return CommandResult(accepted=True)

    def pause(self) -> CommandResult:
        self.pause_count += 1
        if self._state != ExecutionState.EXECUTING:
            return CommandResult(
                accepted=False, error="Can only pause from EXECUTING"
            )
        self._state = ExecutionState.PAUSED
        return CommandResult(accepted=True)

    def resume(self) -> CommandResult:
        self.resume_count += 1
        if self._state != ExecutionState.PAUSED:
            return CommandResult(
                accepted=False, error="Can only resume from PAUSED"
            )
        self._state = ExecutionState.EXECUTING
        return CommandResult(accepted=True)

    def reset(self) -> CommandResult:
        self.reset_count += 1
        if self._state == ExecutionState.IDLE:
            return CommandResult(
                accepted=False, error="Already in IDLE"
            )
        self._state = ExecutionState.IDLE
        return CommandResult(accepted=True)

    def get_status(self) -> ExecutionStatus:
        progress = 1.0 if self._state == ExecutionState.COMPLETED else 0.0
        return ExecutionStatus(
            state=self._state,
            execution_id=self._execution_id,
            progress=progress,
        )

    def invalidate_trajectories(self) -> None:
        self.invalidate_count += 1

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        self.scene_edits.append(request)
        if self.fail_next_scene_edit:
            self.fail_next_scene_edit = False
            return SceneEditResult(success=False, error="Scene edit failed (injected)")
        return SceneEditResult(success=True)

    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        self.link_pose_calls.append((target_frame, base_frame))
        return {
            "position_m": [0.5, 0.0, 0.6],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
