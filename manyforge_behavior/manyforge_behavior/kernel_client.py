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


class GripperCommandState(Enum):
    IDLE = "idle"
    EXECUTING = "executing"
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
    touch_links: Optional[List[str]] = None


@dataclass
class SceneEditResult:
    success: bool = False
    error: Optional[str] = None


@dataclass
class SceneShape:
    type: str
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    cylinder_radius_m: Optional[float] = None
    cylinder_height_m: Optional[float] = None
    mesh_resource_uri: Optional[str] = None
    mesh_scale: Optional[List[float]] = None


@dataclass
class SceneObjectSnapshot:
    object_id: str
    shape: Optional[SceneShape] = None
    pose_in_universe: Optional[Dict[str, Any]] = None
    attached: bool = False
    frame_id: Optional[str] = None


@dataclass
class RobotStateSnapshot:
    joint_names: List[str] = field(default_factory=list)
    positions_rad: List[float] = field(default_factory=list)
    velocities_rad_s: List[float] = field(default_factory=list)


@dataclass
class MotionPreviewResult:
    ok: bool = False
    error: Optional[str] = None
    error_details: Optional[Dict[str, Any]] = None
    trajectory_duration_s: Optional[float] = None
    trajectory_waypoint_count: Optional[int] = None
    final_state: Optional[RobotStateSnapshot] = None
    final_tcp_pose: Optional[Dict[str, Any]] = None


@dataclass
class GripperCommandRequest:
    gripper_id: str
    group_id: str
    joint_name: str
    target_position: float
    duration_s: float


@dataclass
class GripperCommandStatus:
    state: GripperCommandState = GripperCommandState.IDLE
    gripper_id: str = ""
    progress: float = 0.0
    current_position: Optional[float] = None
    target_position: Optional[float] = None
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


def _is_vector(values: Optional[List[float]], length: int) -> bool:
    return values is not None and len(values) == length


def _validate_scene_edit_request(request: SceneEditRequest) -> Optional[str]:
    if not request.object_id:
        return "object_id must not be empty"

    if request.operation not in {"add", "remove", "update_pose", "attach", "detach"}:
        return f"Unsupported scene edit operation: {request.operation}"

    if request.operation == "add":
        if request.shape_type not in {"box", "sphere"}:
            return "add requires shape_type to be one of: box, sphere"
        if request.shape_type == "box":
            if not _is_vector(request.box_dimensions_m, 3):
                return "add with shape_type=box requires box_dimensions_m=[x, y, z]"
            if any(v <= 0.0 for v in request.box_dimensions_m):
                return "box_dimensions_m values must be > 0"
        if request.shape_type == "sphere":
            if request.sphere_radius_m is None or request.sphere_radius_m <= 0.0:
                return "add with shape_type=sphere requires sphere_radius_m > 0"
        if request.pose_position_m is not None and not _is_vector(request.pose_position_m, 3):
            return "pose_position_m must have exactly 3 elements"
        if request.pose_orientation_xyzw is not None and not _is_vector(request.pose_orientation_xyzw, 4):
            return "pose_orientation_xyzw must have exactly 4 elements"

    if request.operation == "update_pose":
        if request.pose_position_m is None:
            return "update_pose requires pose_position_m"
        if not _is_vector(request.pose_position_m, 3):
            return "pose_position_m must have exactly 3 elements"
        if request.pose_orientation_xyzw is not None and not _is_vector(request.pose_orientation_xyzw, 4):
            return "pose_orientation_xyzw must have exactly 4 elements"

    if request.operation == "attach" and not request.frame_id:
        return "attach requires frame_id"
    if request.touch_links is not None:
        if not isinstance(request.touch_links, list):
            return "touch_links must be a list of strings"
        if any(not isinstance(link, str) or not link for link in request.touch_links):
            return "touch_links must contain only non-empty strings"

    return None


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
    def preview_motion(
        self,
        request: ExecuteRequest,
        start_state: Optional[RobotStateSnapshot] = None,
    ) -> MotionPreviewResult:
        """Plan + validate a motion without executing it."""
        ...

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

    @abstractmethod
    def get_scene_objects(self) -> list[SceneObjectSnapshot]:
        """Return backend-authoritative scene objects with current shape and pose."""
        ...

    def get_scene_trace(self, limit: Optional[int] = None) -> List[Dict[str, Any]]:
        """Return recent backend scene trace events, newest last."""
        return []

    def clear_scene_trace(self) -> None:
        """Clear any retained backend scene trace events."""

    def drain_kernel_events(self) -> List[Dict[str, Any]]:
        """Drain buffered native kernel events (returns and clears buffer).

        Returns a list of event dicts with keys: type, execution_id,
        timestamp_ns, and optionally from_state/to_state, error, details.
        """
        return []

    def get_cached_trajectory(self, move_id: str) -> Optional[Trajectory]:
        """Get cached trajectory for a move. Returns None if not available."""
        return None

    @abstractmethod
    def get_scene_object_ids(self) -> list[str]:
        """Return IDs of all collision objects currently in the scene."""
        ...

    @abstractmethod
    def get_joint_names(self) -> list[str]:
        """Return ordered joint names for the active robot group."""
        ...

    @abstractmethod
    def get_metrics(self) -> dict[str, int]:
        """Return performance/diagnostic counters."""
        ...

    @abstractmethod
    def command_gripper(self, request: GripperCommandRequest) -> CommandResult: ...

    @abstractmethod
    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus: ...


class MockKernelClient(KernelClient):
    """Mock kernel for testing. Simulates state transitions and records calls.

    Args:
        execute_duration_s: Simulated execution time per move. When > 0, execute()
            transitions to EXECUTING and get_status() returns COMPLETED only after
            the duration elapses. Set to 0 (default) for instant completion in tests.
    """

    def __init__(self, execute_duration_s: float = 0.0, joint_names: Optional[List[str]] = None) -> None:
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
        # Metrics counters
        self._metrics = {
            "cache_hits": 0,
            "cache_misses": 0,
            "cache_invalidations": 0,
            "plans_succeeded": 0,
            "plans_failed": 0,
            "validations_passed": 0,
            "validations_failed": 0,
            "executions_completed": 0,
            "executions_failed": 0,
            "stops_issued": 0,
            "collision_detections": 0,
            "snapshots_published": 0,
        }
        # Joint names (configurable for tests)
        self._joint_names = joint_names if joint_names is not None else ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
        self.last_gripper_command_request: Optional[GripperCommandRequest] = None
        self.gripper_command_count = 0
        self.fail_next_gripper_command = False
        self._gripper_commands: Dict[str, Dict[str, Any]] = {}
        self._gripper_positions: Dict[str, float] = {}
        # Kernel event buffer (mirrors C++ KernelEventCollector)
        self._kernel_events: Deque[Dict[str, Any]] = deque(maxlen=4096)

    def _emit_kernel_event(
        self,
        event_type: str,
        *,
        from_state: Optional[str] = None,
        to_state: Optional[str] = None,
        error: Optional[str] = None,
    ) -> None:
        event: Dict[str, Any] = {
            "type": event_type,
            "execution_id": self._execution_id,
            "timestamp_ns": time.monotonic_ns(),
        }
        if from_state is not None:
            event["from_state"] = from_state
        if to_state is not None:
            event["to_state"] = to_state
        if error is not None:
            event["error"] = {"code": -1, "message": error}
        self._kernel_events.append(event)

    def drain_kernel_events(self) -> List[Dict[str, Any]]:
        events = list(self._kernel_events)
        self._kernel_events.clear()
        return events

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
            prev = self._state.value
            self._state = ExecutionState.IDLE
            self._emit_kernel_event("state_transition", from_state=prev, to_state="idle")
        if self._state != ExecutionState.IDLE:
            return ExecuteResult(ok=False, error="Not in IDLE state")
        if self.fail_next_execute:
            self.fail_next_execute = False
            self._state = ExecutionState.FAILED
            self._metrics["executions_failed"] += 1
            self._emit_kernel_event("state_transition", from_state="idle", to_state="failed")
            self._emit_kernel_event("error_raised", error="Execution failed (injected)")
            return ExecuteResult(ok=False, error="Execution failed (injected)")
        eid = self._next_id
        self._next_id += 1
        self._execution_id = eid
        self._active_move_id = request.move_id
        self._emit_kernel_event("plan_started")
        if self._execute_duration_s > 0:
            self._state = ExecutionState.EXECUTING
            self._execute_start = time.monotonic()
            self._emit_kernel_event("state_transition", from_state="idle", to_state="executing")
            self._emit_kernel_event("execute_started")
        else:
            self._state = ExecutionState.COMPLETED
            self._metrics["executions_completed"] += 1
            self._emit_kernel_event("state_transition", from_state="idle", to_state="completed")
            self._emit_kernel_event("execute_completed")
        return ExecuteResult(ok=True, execution_id=eid)

    def preview_motion(
        self,
        request: ExecuteRequest,
        start_state: Optional[RobotStateSnapshot] = None,
    ) -> MotionPreviewResult:
        """Preview a motion without mutating execution state.

        The mock keeps this deliberately simple: it accepts all requests unless
        a failure has been injected via ``fail_next_execute`` and returns the
        requested pose as the final TCP pose for pose-goal motions.
        """
        if self.fail_next_execute:
            self.fail_next_execute = False
            self._metrics["plans_failed"] += 1
            return MotionPreviewResult(ok=False, error="Preview planning failed (injected)")

        self._metrics["plans_succeeded"] += 1
        self._metrics["validations_passed"] += 1
        final_state = start_state or RobotStateSnapshot(
            joint_names=list(self._joint_names),
            positions_rad=[0.0 for _ in self._joint_names],
            velocities_rad_s=[],
        )
        final_tcp_pose: Optional[Dict[str, Any]] = None
        if request.motion_type == "pose_goal" and request.pose_goal is not None:
            final_tcp_pose = {
                "position_m": list(request.pose_goal.get("position_m", [])),
                "orientation_xyzw": list(request.pose_goal.get("orientation_xyzw", [])),
            }
        return MotionPreviewResult(
            ok=True,
            trajectory_duration_s=1.0,
            trajectory_waypoint_count=2,
            final_state=final_state,
            final_tcp_pose=final_tcp_pose,
        )

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
        prev = self._state.value
        self._metrics["stops_issued"] += 1
        self._state = ExecutionState.STOPPED
        self._emit_kernel_event("stop_requested")
        self._emit_kernel_event("state_transition", from_state=prev, to_state="stopped")
        return CommandResult(accepted=True)

    def pause(self) -> CommandResult:
        self.pause_count += 1
        if self._state != ExecutionState.EXECUTING:
            return CommandResult(
                accepted=False, error="Can only pause from EXECUTING"
            )
        self._state = ExecutionState.PAUSED
        self._emit_kernel_event("pause_requested")
        self._emit_kernel_event("state_transition", from_state="executing", to_state="paused")
        return CommandResult(accepted=True)

    def resume(self) -> CommandResult:
        self.resume_count += 1
        if self._state != ExecutionState.PAUSED:
            return CommandResult(
                accepted=False, error="Can only resume from PAUSED"
            )
        self._state = ExecutionState.EXECUTING
        self._emit_kernel_event("resume_requested")
        self._emit_kernel_event("state_transition", from_state="paused", to_state="executing")
        return CommandResult(accepted=True)

    def reset(self) -> CommandResult:
        self.reset_count += 1
        if self._state == ExecutionState.IDLE:
            return CommandResult(
                accepted=False, error="Already in IDLE"
            )
        prev = self._state.value
        self._state = ExecutionState.IDLE
        self._active_move_id = ""
        self._emit_kernel_event("state_transition", from_state=prev, to_state="idle")
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
                self._metrics["executions_completed"] += 1
                self._emit_kernel_event("execute_completed")
                self._emit_kernel_event("state_transition", from_state="executing", to_state="completed")
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
        self._metrics["snapshots_published"] += 1
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
        self._metrics["cache_invalidations"] += 1

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
        validation_error = _validate_scene_edit_request(request)
        if validation_error is not None:
            self._record_scene_trace(
                event=request.operation,
                object_id=request.object_id,
                success=False,
                frame_id=request.frame_id,
                attached=state_before.attached if state_before.exists else None,
                pose_in_frame=None,
                pose_in_universe=pose_before,
                error=validation_error,
            )
            return SceneEditResult(success=False, error=validation_error)
        error: Optional[str] = None
        if request.operation == "add":
            if request.object_id in self._objects:
                error = "Object already exists"
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
            self._objects[request.object_id] = {
                "shape_type": request.shape_type,
                "box_dimensions_m": list(request.box_dimensions_m or []) or None,
                "sphere_radius_m": request.sphere_radius_m,
                "position_m": list(request.pose_position_m or [0.0, 0.0, 0.0]),
                "orientation_xyzw": list(request.pose_orientation_xyzw or [0.0, 0.0, 0.0, 1.0]),
                "frame_id": request.frame_id or "universe",
            }
        elif request.operation == "update_pose":
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
            tracked["position_m"] = list(request.pose_position_m or [0.0, 0.0, 0.0])
            tracked["orientation_xyzw"] = list(
                request.pose_orientation_xyzw or [0.0, 0.0, 0.0, 1.0]
            )
            tracked["frame_id"] = request.frame_id or tracked.get("frame_id", "universe")
        elif request.operation == "remove":
            if request.object_id not in self._objects:
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
        else:
            error = f"Unsupported scene edit operation: {request.operation}"
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

    def get_scene_objects(self) -> list[SceneObjectSnapshot]:
        """Return authoritative object snapshots for the current mock scene."""
        objects: list[SceneObjectSnapshot] = []
        for object_id in sorted(self._objects):
            tracked = self._objects[object_id]
            shape = None
            if tracked.get("shape_type") == "box":
                shape = SceneShape(
                    type="box",
                    box_dimensions_m=list(tracked.get("box_dimensions_m") or []),
                )
            elif tracked.get("shape_type") == "sphere":
                shape = SceneShape(
                    type="sphere",
                    sphere_radius_m=tracked.get("sphere_radius_m"),
                )
            objects.append(
                SceneObjectSnapshot(
                    object_id=object_id,
                    shape=shape,
                    pose_in_universe=self.get_object_pose(object_id, "universe"),
                    attached=tracked.get("frame_id") not in {None, "universe", "base_link"},
                    frame_id=tracked.get("frame_id"),
                )
            )
        return objects

    def get_scene_object_ids(self) -> list[str]:
        """Return IDs of all collision objects currently in the scene."""
        return [scene_object.object_id for scene_object in self.get_scene_objects()]

    def get_joint_names(self) -> list[str]:
        """Return ordered joint names for the active robot group."""
        return self._joint_names

    def get_metrics(self) -> dict[str, int]:
        """Return performance/diagnostic counters."""
        return dict(self._metrics)

    def _compute_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        command = self._gripper_commands.get(gripper_id)
        if command is None:
            return GripperCommandStatus(
                state=GripperCommandState.IDLE,
                gripper_id=gripper_id,
                current_position=self._gripper_positions.get(gripper_id),
            )

        state = command["state"]
        progress = float(command.get("progress", 0.0))
        current_position = command.get("current_position")
        target_position = command.get("target_position")

        if state == GripperCommandState.EXECUTING:
            elapsed = time.monotonic() - float(command["start_time"])
            duration = float(command["duration_s"])
            progress = min(max(elapsed / duration, 0.0), 1.0)
            current_position = (
                float(command["start_position"])
                + (float(command["target_position"]) - float(command["start_position"])) * progress
            )
            if progress >= 1.0:
                state = GripperCommandState.COMPLETED
                current_position = float(command["target_position"])

        return GripperCommandStatus(
            state=state,
            gripper_id=gripper_id,
            progress=progress,
            current_position=(
                None if current_position is None else float(current_position)
            ),
            target_position=(
                None if target_position is None else float(target_position)
            ),
            error=command.get("error"),
        )

    def command_gripper(self, request: GripperCommandRequest) -> CommandResult:
        self.gripper_command_count += 1
        self.last_gripper_command_request = request
        if self.fail_next_gripper_command:
            self.fail_next_gripper_command = False
            return CommandResult(accepted=False, error="Gripper command failed (injected)")

        current_status = self.get_gripper_status(request.gripper_id)
        if current_status.state == GripperCommandState.EXECUTING:
            return CommandResult(
                accepted=False,
                error=f"Gripper '{request.gripper_id}' is already executing a command",
            )

        current_position = current_status.current_position
        if current_position is None:
            current_position = self._gripper_positions.get(request.gripper_id, 0.0)
        if request.duration_s <= 0.0:
            self._gripper_positions[request.gripper_id] = request.target_position
            self._gripper_commands[request.gripper_id] = {
                "state": GripperCommandState.COMPLETED,
                "current_position": request.target_position,
                "target_position": request.target_position,
                "progress": 1.0,
                "error": None,
            }
            return CommandResult(accepted=True)

        self._gripper_commands[request.gripper_id] = {
            "state": GripperCommandState.EXECUTING,
            "group_id": request.group_id,
            "joint_name": request.joint_name,
            "start_position": current_position,
            "current_position": current_position,
            "target_position": request.target_position,
            "duration_s": request.duration_s,
            "start_time": time.monotonic(),
            "progress": 0.0,
            "error": None,
        }
        return CommandResult(accepted=True)

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        status = self._compute_gripper_status(gripper_id)
        if status.state == GripperCommandState.COMPLETED:
            self._gripper_positions[gripper_id] = (
                float(status.current_position)
                if status.current_position is not None
                else self._gripper_positions.get(gripper_id, 0.0)
            )
            command = self._gripper_commands.get(gripper_id)
            if command is not None:
                command["state"] = GripperCommandState.COMPLETED
                command["current_position"] = status.current_position
                command["progress"] = 1.0
        return status
