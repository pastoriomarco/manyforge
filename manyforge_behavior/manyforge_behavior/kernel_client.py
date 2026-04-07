"""Abstract kernel interface and mock implementation for testing."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
import math
import time
from typing import Any, Deque, Dict, List, Optional


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
    motion_type: str  # "joint_goal", "pose_goal", or "relative_move"
    joint_goal_names: List[str] = field(default_factory=list)
    joint_goal_positions_rad: List[float] = field(default_factory=list)
    pose_goal: Optional[Dict[str, Any]] = None
    relative_move: Optional[Dict[str, Any]] = None
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
    active_move_id: str = ""


@dataclass
class ExecutionSnapshot:
    timestamp_ns: int = 0
    sequence_number: int = 0
    execution_id: int = 0
    move_id: str = ""
    state: ExecutionState = ExecutionState.IDLE
    progress: float = 0.0
    joint_positions_rad: List[float] = field(default_factory=list)
    error: Optional[str] = None


@dataclass
class ObjectState:
    object_id: str = ""
    exists: bool = False
    attached: bool = False
    frame_id: Optional[str] = None


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
    final_state: Optional[str] = None
    final_error: Optional[str] = None


@dataclass
class TrajectoryWaypoint:
    time_from_start_s: float = 0.0
    positions_rad: List[float] = field(default_factory=list)


@dataclass
class Trajectory:
    joint_names: List[str] = field(default_factory=list)
    waypoints: List[TrajectoryWaypoint] = field(default_factory=list)


@dataclass
class SceneEditRequest:
    operation: str  # "add", "remove", "update_pose", "attach", "detach"
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


def _pose_to_matrix(pos: List[float], orient_xyzw: List[float]) -> List[List[float]]:
    """Build 4x4 matrix from position + quaternion (xyzw)."""
    x, y, z, w = orient_xyzw
    return [
        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * z * w, 2 * x * z + 2 * y * w, pos[0]],
        [2 * x * y + 2 * z * w, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w, pos[1]],
        [2 * x * z - 2 * y * w, 2 * y * z + 2 * x * w, 1 - 2 * x * x - 2 * y * y, pos[2]],
        [0, 0, 0, 1],
    ]


def _matrix_to_pose(m: List[List[float]]) -> tuple[List[float], List[float]]:
    """Extract position + quaternion (xyzw) from 4x4 matrix."""
    pos = [m[0][3], m[1][3], m[2][3]]
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    return pos, [x, y, z, w]


def _multiply_matrices(a: List[List[float]], b: List[List[float]]) -> List[List[float]]:
    """Multiply two 4x4 matrices."""
    return [
        [sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
        for i in range(4)
    ]


def _invert_matrix(m: List[List[float]]) -> List[List[float]]:
    """Invert a rigid 4x4 transform."""
    rt = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    nt = [-(rt[i][0] * t[0] + rt[i][1] * t[1] + rt[i][2] * t[2]) for i in range(3)]
    return [
        [rt[0][0], rt[0][1], rt[0][2], nt[0]],
        [rt[1][0], rt[1][1], rt[1][2], nt[1]],
        [rt[2][0], rt[2][1], rt[2][2], nt[2]],
        [0, 0, 0, 1],
    ]


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
    def get_execution_snapshot(self) -> ExecutionSnapshot: ...

    @abstractmethod
    def invalidate_trajectories(self) -> None: ...

    @abstractmethod
    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult: ...

    @abstractmethod
    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]: ...

    @abstractmethod
    def get_object_pose(
        self, object_id: str, base_frame: str
    ) -> Optional[Dict[str, Any]]: ...

    @abstractmethod
    def get_object_state(self, object_id: str) -> ObjectState: ...

    def get_scene_trace(self, limit: Optional[int] = None) -> List[Dict[str, Any]]:
        """Return recent backend scene trace events, newest last."""
        return []

    def clear_scene_trace(self) -> None:
        """Clear any retained backend scene trace events."""

    def get_cached_trajectory(self, move_id: str) -> Optional[Trajectory]:
        """Get cached trajectory for a move. Returns None if not available."""
        return None


class MockKernelClient(KernelClient):
    """Mock kernel for testing. Simulates state transitions and records calls.

    Args:
        execute_duration_s: Simulated execution time per move. When > 0, execute()
            transitions to EXECUTING and get_status() returns COMPLETED only after
            the duration elapses. Set to 0 (default) for instant completion in tests.
    """

    def __init__(self, execute_duration_s: float = 0.0) -> None:
        self._state = ExecutionState.IDLE
        self._execution_id = 0
        self._active_move_id = ""
        self._next_id = 1
        self._snapshot_sequence = 0
        self._execute_duration_s = execute_duration_s
        self._execute_start: Optional[float] = None
        self.last_execute_request: Optional[ExecuteRequest] = None
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
        self._objects: Dict[str, Dict[str, Any]] = {}
        self._scene_trace: Deque[Dict[str, Any]] = deque(maxlen=4096)

    def _record_scene_trace(
        self,
        *,
        event: str,
        object_id: str,
        success: bool,
        frame_id: Optional[str] = None,
        attached: Optional[bool] = None,
        pose_in_frame: Optional[Dict[str, Any]] = None,
        pose_in_universe: Optional[Dict[str, Any]] = None,
        error: Optional[str] = None,
    ) -> None:
        self._scene_trace.append(
            {
                "timestamp_ns": time.monotonic_ns(),
                "source": "backend",
                "event": event,
                "object_id": object_id,
                "success": success,
                "frame_id": frame_id,
                "attached": attached,
                "pose_in_frame": pose_in_frame,
                "pose_in_universe": pose_in_universe,
                "error": error,
            }
        )

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        import time
        self.execute_count += 1
        self.last_execute_request = request
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
        self._active_move_id = request.move_id
        if self._execute_duration_s > 0:
            self._state = ExecutionState.EXECUTING
            self._execute_start = time.monotonic()
        else:
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
        self._active_move_id = ""
        return CommandResult(accepted=True)

    def get_status(self) -> ExecutionStatus:
        import time
        # Transition EXECUTING → COMPLETED when duration elapses
        if (self._state == ExecutionState.EXECUTING
                and self._execute_start is not None
                and self._execute_duration_s > 0):
            elapsed = time.monotonic() - self._execute_start
            if elapsed >= self._execute_duration_s:
                self._state = ExecutionState.COMPLETED
                self._execute_start = None
                return ExecutionStatus(
                    state=self._state,
                    execution_id=self._execution_id,
                    progress=1.0,
                    active_move_id=self._active_move_id,
                )
            return ExecutionStatus(
                state=self._state,
                execution_id=self._execution_id,
                progress=min(elapsed / self._execute_duration_s, 1.0),
                active_move_id=self._active_move_id,
            )
        progress = 1.0 if self._state == ExecutionState.COMPLETED else 0.0
        return ExecutionStatus(
            state=self._state,
            execution_id=self._execution_id,
            progress=progress,
            active_move_id=self._active_move_id,
        )

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        import time
        status = self.get_status()
        self._snapshot_sequence += 1
        return ExecutionSnapshot(
            timestamp_ns=time.monotonic_ns(),
            sequence_number=self._snapshot_sequence,
            execution_id=status.execution_id,
            move_id=self._active_move_id,
            state=status.state,
            progress=status.progress,
        )

    def invalidate_trajectories(self) -> None:
        self.invalidate_count += 1

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        self.scene_edits.append(request)
        state_before = self.get_object_state(request.object_id)
        pose_before = self.get_object_pose(request.object_id, "universe")
        if self.fail_next_scene_edit:
            self.fail_next_scene_edit = False
            error = "Scene edit failed (injected)"
            self._record_scene_trace(
                event=request.operation,
                object_id=request.object_id,
                success=False,
                frame_id=request.frame_id,
                attached=state_before.attached if state_before.exists else None,
                pose_in_frame=None,
                pose_in_universe=pose_before,
                error=error,
            )
            return SceneEditResult(success=False, error=error)
        error: Optional[str] = None
        if request.operation == "add":
            self._objects[request.object_id] = {
                "position_m": list(request.pose_position_m or [0.0, 0.0, 0.0]),
                "orientation_xyzw": list(request.pose_orientation_xyzw or [0.0, 0.0, 0.0, 1.0]),
                "frame_id": request.frame_id or "universe",
            }
        elif request.operation == "update_pose":
            tracked = self._objects.setdefault(request.object_id, {})
            tracked["position_m"] = list(request.pose_position_m or [0.0, 0.0, 0.0])
            tracked["orientation_xyzw"] = list(
                request.pose_orientation_xyzw or [0.0, 0.0, 0.0, 1.0]
            )
            tracked["frame_id"] = request.frame_id or tracked.get("frame_id", "universe")
        elif request.operation == "remove":
            self._objects.pop(request.object_id, None)
        elif request.operation == "attach":
            tracked = self._objects.get(request.object_id)
            if tracked is None:
                error = "Object not found"
                self._record_scene_trace(
                    event=request.operation,
                    object_id=request.object_id,
                    success=False,
                    frame_id=request.frame_id,
                    attached=state_before.attached if state_before.exists else None,
                    pose_in_frame=None,
                    pose_in_universe=pose_before,
                    error=error,
                )
                return SceneEditResult(success=False, error=error)
            target_frame = request.frame_id or "tool0"
            world_pose = self.get_object_pose(request.object_id, "universe")
            frame_pose = self.get_link_pose(target_frame, "universe")
            if world_pose is None or frame_pose is None:
                error = "Failed to resolve attach frames"
                self._record_scene_trace(
                    event=request.operation,
                    object_id=request.object_id,
                    success=False,
                    frame_id=target_frame,
                    attached=state_before.attached if state_before.exists else None,
                    pose_in_frame=None,
                    pose_in_universe=world_pose,
                    error=error,
                )
                return SceneEditResult(success=False, error=error)
            t_frame = _pose_to_matrix(
                list(frame_pose["position_m"]),
                list(frame_pose["orientation_xyzw"]),
            )
            t_world_object = _pose_to_matrix(
                list(world_pose["position_m"]),
                list(world_pose["orientation_xyzw"]),
            )
            local_pos, local_orient = _matrix_to_pose(
                _multiply_matrices(_invert_matrix(t_frame), t_world_object)
            )
            tracked["position_m"] = local_pos
            tracked["orientation_xyzw"] = local_orient
            tracked["frame_id"] = target_frame
        elif request.operation == "detach":
            tracked = self._objects.get(request.object_id)
            if tracked is None:
                error = "Object not found"
                self._record_scene_trace(
                    event=request.operation,
                    object_id=request.object_id,
                    success=False,
                    frame_id=request.frame_id,
                    attached=state_before.attached if state_before.exists else None,
                    pose_in_frame=None,
                    pose_in_universe=pose_before,
                    error=error,
                )
                return SceneEditResult(success=False, error=error)
            target_frame = request.frame_id or "universe"
            world_pose = self.get_object_pose(request.object_id, "universe")
            if world_pose is None:
                error = "Failed to resolve world pose"
                self._record_scene_trace(
                    event=request.operation,
                    object_id=request.object_id,
                    success=False,
                    frame_id=target_frame,
                    attached=state_before.attached if state_before.exists else None,
                    pose_in_frame=None,
                    pose_in_universe=None,
                    error=error,
                )
                return SceneEditResult(success=False, error=error)
            if target_frame == "universe":
                tracked["position_m"] = list(world_pose["position_m"])
                tracked["orientation_xyzw"] = list(world_pose["orientation_xyzw"])
                tracked["frame_id"] = target_frame
            else:
                frame_pose = self.get_link_pose(target_frame, "universe")
                if frame_pose is None:
                    error = "Failed to resolve detach frame"
                    self._record_scene_trace(
                        event=request.operation,
                        object_id=request.object_id,
                        success=False,
                        frame_id=target_frame,
                        attached=state_before.attached if state_before.exists else None,
                        pose_in_frame=None,
                        pose_in_universe=world_pose,
                        error=error,
                    )
                    return SceneEditResult(success=False, error=error)
                t_frame = _pose_to_matrix(
                    list(frame_pose["position_m"]),
                    list(frame_pose["orientation_xyzw"]),
                )
                t_world_object = _pose_to_matrix(
                    list(world_pose["position_m"]),
                    list(world_pose["orientation_xyzw"]),
                )
                local_pos, local_orient = _matrix_to_pose(
                    _multiply_matrices(_invert_matrix(t_frame), t_world_object)
                )
                tracked["position_m"] = local_pos
                tracked["orientation_xyzw"] = local_orient
                tracked["frame_id"] = target_frame
        result = SceneEditResult(success=True)
        state_after = self.get_object_state(request.object_id)
        pose_in_frame = None
        pose_in_universe = None
        frame_id = request.frame_id
        attached = None
        if request.operation == "remove":
            frame_id = state_before.frame_id
            attached = state_before.attached if state_before.exists else None
            pose_in_universe = pose_before
        elif state_after.exists:
            frame_id = state_after.frame_id
            attached = state_after.attached
            if frame_id is not None:
                pose_in_frame = self.get_object_pose(request.object_id, frame_id)
            pose_in_universe = self.get_object_pose(request.object_id, "universe")
        self._record_scene_trace(
            event=request.operation,
            object_id=request.object_id,
            success=True,
            frame_id=frame_id,
            attached=attached,
            pose_in_frame=pose_in_frame,
            pose_in_universe=pose_in_universe,
        )
        return result

    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        self.link_pose_calls.append((target_frame, base_frame))
        if target_frame in ("", "universe", base_frame, "base_link"):
            return {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        return {
            "position_m": [0.5, 0.0, 0.6],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

    def get_object_pose(
        self, object_id: str, base_frame: str
    ) -> Optional[Dict[str, Any]]:
        tracked = self._objects.get(object_id)
        if tracked is None:
            return None
        position = list(tracked.get("position_m", [0.0, 0.0, 0.0]))
        orientation = list(tracked.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]))
        frame_id = tracked.get("frame_id", "universe")
        if frame_id not in ("", "universe", base_frame):
            frame_pose = self.get_link_pose(frame_id, base_frame)
            if frame_pose is None:
                return None
            t_frame = _pose_to_matrix(
                list(frame_pose["position_m"]),
                list(frame_pose["orientation_xyzw"]),
            )
            t_local = _pose_to_matrix(position, orientation)
            position, orientation = _matrix_to_pose(_multiply_matrices(t_frame, t_local))
        return {
            "position_m": position,
            "orientation_xyzw": orientation,
        }

    def get_object_state(self, object_id: str) -> ObjectState:
        tracked = self._objects.get(object_id)
        if tracked is None:
            return ObjectState(object_id=object_id)
        frame_id = tracked.get("frame_id", "universe")
        return ObjectState(
            object_id=object_id,
            exists=True,
            attached=frame_id not in ("", "universe", "base_link"),
            frame_id=frame_id,
        )

    def get_scene_trace(self, limit: Optional[int] = None) -> List[Dict[str, Any]]:
        trace = list(self._scene_trace)
        if limit is not None and limit >= 0:
            return trace[-limit:]
        return trace

    def clear_scene_trace(self) -> None:
        self._scene_trace.clear()
