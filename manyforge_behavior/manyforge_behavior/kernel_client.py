"""Abstract kernel interface and mock implementation for testing."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
import math
import threading
import time
from typing import Any, Deque, Dict, List, Optional


class ExecutionState(Enum):
    IDLE = "idle"
    PLANNING = "planning"
    VALIDATING = "validating"
    EXECUTING = "executing"
    INTERRUPTED = "interrupted"
    PAUSED = "paused"
    STOPPING = "stopping"
    STOPPED = "stopped"
    COMPLETED = "completed"
    FAILED = "failed"
    INTERVENTION_REQUIRED = "intervention_required"


class GripperCommandState(Enum):
    IDLE = "idle"
    EXECUTING = "executing"
    COMPLETED = "completed"
    FAILED = "failed"


@dataclass
class MotionInterruptStopProfile:
    """Controlled-stop profile carried by a motion-scoped
    interrupt scope (mirrors `MotionInterruptStopProfileDTO` in
    spec/210 §3.11). `stop_mode` is one of "spring_back_hold",
    "soft_on_path", or "hard_deceleration"."""

    profile_id: str
    stop_mode: str
    deceleration_time_s: Optional[float] = None
    max_stop_time_s: Optional[float] = None
    joint_deceleration_profile_id: Optional[str] = None
    per_joint_max_deceleration_rad_s2: Dict[str, float] = field(default_factory=dict)
    allowed_fallback_modes: List[str] = field(default_factory=list)
    allow_natural_completion_on_short_path: bool = False
    require_backend_confirmation: bool = True


@dataclass
class MotionInterruptScope:
    """Per-move binding that arms a `kMove`-scoped
    interrupt policy for exactly one execute request (spec/260 §2.6).
    `node_category` is one of "generic" (default; MoveUntilSignal) or
    "precision_contact" (ProbeSearchMove). `activation_mode` is one of
    "require_inactive_at_arm", "fresh_edge_after_arm",
    "trip_if_already_active", or "maintain_level_with_hysteresis".
    `complete_move_on_signal` scopes MUST carry a `stop_profile`."""

    scope_id: str
    policy_id: str
    activation_mode: str = "require_inactive_at_arm"
    arm_timeout_ns: Optional[int] = None
    result_binding_key: Optional[str] = None
    required: bool = True
    stop_profile: Optional[MotionInterruptStopProfile] = None
    node_category: str = "generic"


@dataclass
class MotionTripOutcome:
    """Successful signal_trip outcome carried inside a
    `MotionExecutionOutcome`. Mirrors `MotionTripOutcomeDTO`."""

    latch_id: str = ""
    source_id: str = ""
    policy_id: str = ""
    scope_id: str = ""
    executed_stop_mode: str = ""
    stop_command_source: str = ""
    backend_stop_confirmed: bool = False
    stop_distance_rad: Optional[float] = None
    initial_path_deviation_rad: Optional[float] = None
    path_deviation_max_rad: Optional[float] = None
    fallback_reason: Optional[str] = None
    signal_to_latch_ns: int = 0
    latch_to_backend_stop_call_ns: int = 0
    latch_to_stop_confirmed_ns: int = 0
    result_binding_key: Optional[str] = None


@dataclass
class MotionExecutionOutcome:
    """Immutable terminal outcome for one motion execution
    (mirrors `MotionExecutionOutcomeDTO`, spec/210 §3.11). The kernel
    emits one per terminal execution; behavior-tree nodes consume it
    via `KernelClient.get_execution_outcome(execution_id)`."""

    execution_id: int = 0
    move_id: str = ""
    # One of "completed_normally" | "signal_trip" | "no_signal_trip" |
    # "stopped" | "failed" | "interrupted_fault".
    terminal_cause: str = "completed_normally"
    trip: Optional[MotionTripOutcome] = None
    error_code: Optional[str] = None
    error_message: Optional[str] = None
    error_reason: Optional[str] = None


@dataclass
class JointDynamicsLimit:
    """Per-joint dynamics cap for one joint (Phase 4-P5.1K).

    Each field is an ABSOLUTE cap in SI units, or None meaning "no cap from
    this source for this dimension". The kernel resolves the effective cap as
    the conservative minimum across model/backend/controller/deployment sources
    and enforces velocity/acceleration in TOPP-RA time-parameterization. Jerk is
    carried but only enforceable where the active predictor constrains jerk; the
    discrete TOPP-RA path rejects a jerk cap as `unsupported_joint_dynamics_limit`
    rather than claiming enforcement."""

    max_velocity_rad_s: Optional[float] = None
    max_acceleration_rad_s2: Optional[float] = None
    max_jerk_rad_s3: Optional[float] = None


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
    # Per-joint absolute dynamics caps keyed by canonical drivable joint name
    # (Phase 4-P5.1K). Empty preserves prior behavior. Carried through the
    # native request channel into TOPP-RA; velocity_scale is applied AFTER the
    # absolute per-joint velocity cap is resolved.
    per_joint_dynamics_limits: Dict[str, JointDynamicsLimit] = field(
        default_factory=dict
    )
    joint_position_tol_rad: float = 0.05
    translation_tol_m: float = 0.005
    rotation_tol_rad: float = 0.05
    planning_timeout_s: float = 2.0
    execution_timeout_s: float = 10.0
    cache_mode: str = "default"
    # Per-move bindings that arm move-scoped interrupt
    # policies (spec/210 §3.10 + spec/260 §2.6). Empty list preserves
    # free-move semantics; non-empty arms each scope atomically with
    # dispatch and reaps on every terminal execution state.
    motion_interrupt_scopes: List[MotionInterruptScope] = field(default_factory=list)


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
    state_source: str = ""
    source_timestamp_ns: int = 0
    source_age_ns: int = 0
    error: Optional[str] = None
    error_code: Optional[str] = None
    error_details: Dict[str, str] = field(default_factory=dict)


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
    # Stable fault token from the backend fault taxonomy; None when accepted
    # or when the rejecting surface has no classified cause.
    reason_code: Optional[str] = None


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
    operation: str  # "add", "upsert", "remove", "update_pose", "attach", "detach"
    object_id: str
    shape_type: Optional[str] = None
    box_dimensions_m: Optional[List[float]] = None
    sphere_radius_m: Optional[float] = None
    cylinder_radius_m: Optional[float] = None
    cylinder_height_m: Optional[float] = None
    mesh_resource_uri: Optional[str] = None
    mesh_scale: Optional[List[float]] = None
    pose_position_m: Optional[List[float]] = None
    pose_orientation_xyzw: Optional[List[float]] = None
    frame_id: Optional[str] = None
    touch_links: Optional[List[str]] = None
    support_contacts: Optional[List[Dict[str, Any]]] = None
    source_timestamp_ns: Optional[int] = None
    source_freshness_budget_ns: Optional[int] = None


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
    max_effort: Optional[float] = None


@dataclass
class GripperCommandStatus:
    state: GripperCommandState = GripperCommandState.IDLE
    gripper_id: str = ""
    progress: float = 0.0
    current_position: Optional[float] = None
    target_position: Optional[float] = None
    error: Optional[str] = None
    # Stable fault token from the backend fault taxonomy; None unless FAILED.
    reason_code: Optional[str] = None


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

    if request.operation not in {"add", "upsert", "remove", "update_pose", "attach", "detach"}:
        return f"Unsupported scene edit operation: {request.operation}"

    if request.operation in {"add", "upsert"}:
        if request.shape_type not in {"box", "sphere", "cylinder", "mesh"}:
            return f"{request.operation} requires shape_type to be one of: box, sphere, cylinder, mesh"
        if request.shape_type == "box":
            if not _is_vector(request.box_dimensions_m, 3):
                return f"{request.operation} with shape_type=box requires box_dimensions_m=[x, y, z]"
            if any(v <= 0.0 for v in request.box_dimensions_m):
                return "box_dimensions_m values must be > 0"
        if request.shape_type == "sphere":
            if request.sphere_radius_m is None or request.sphere_radius_m <= 0.0:
                return f"{request.operation} with shape_type=sphere requires sphere_radius_m > 0"
        if request.shape_type == "cylinder":
            if request.cylinder_radius_m is None or request.cylinder_radius_m <= 0.0:
                return f"{request.operation} with shape_type=cylinder requires cylinder_radius_m > 0"
            if request.cylinder_height_m is None or request.cylinder_height_m <= 0.0:
                return f"{request.operation} with shape_type=cylinder requires cylinder_height_m > 0"
        if request.shape_type == "mesh":
            if not request.mesh_resource_uri:
                return f"{request.operation} with shape_type=mesh requires mesh_resource_uri"
            if request.mesh_scale is not None:
                if not _is_vector(request.mesh_scale, 3):
                    return "mesh_scale must have exactly 3 elements"
                if any(v <= 0.0 for v in request.mesh_scale):
                    return "mesh_scale values must be > 0"
        # Authored creation always carries a full pose (spec 420 §4A.2,
        # create-exception revoked): the sink never invents position or
        # orientation.
        if request.pose_position_m is None:
            return f"{request.operation} requires pose_position_m"
        if not _is_vector(request.pose_position_m, 3):
            return "pose_position_m must have exactly 3 elements"
        if request.pose_orientation_xyzw is None:
            return (
                f"{request.operation} requires pose_orientation_xyzw — the "
                "scene-edit sink never defaults an orientation"
            )
        if not _is_vector(request.pose_orientation_xyzw, 4):
            return "pose_orientation_xyzw must have exactly 4 elements"
        if request.support_contacts is not None:
            if not isinstance(request.support_contacts, list):
                return "support_contacts must be a list"
            for index, contact in enumerate(request.support_contacts):
                if not isinstance(contact, dict):
                    return f"support_contacts[{index}] must be a mapping"
                support_object_id = contact.get("support_object_id")
                if not isinstance(support_object_id, str) or not support_object_id:
                    return f"support_contacts[{index}].support_object_id is required"
                max_start_penetration_m = contact.get("max_start_penetration_m", 0.0)
                if (
                    not isinstance(max_start_penetration_m, (int, float))
                    or max_start_penetration_m < 0.0
                ):
                    return f"support_contacts[{index}].max_start_penetration_m must be >= 0"
                mode = contact.get("mode", "contact_release")
                if mode not in {"contact_release", "contact_process"}:
                    return (
                        f"support_contacts[{index}].mode must be contact_release "
                        "or contact_process"
                    )
                for field_name in (
                    "release_tolerance_m",
                    "min_release_progress_m",
                    "min_separation_clearance_m",
                ):
                    value = contact.get(field_name, 0.0)
                    if not isinstance(value, (int, float)) or value < 0.0:
                        return f"support_contacts[{index}].{field_name} must be >= 0"
                for field_name in (
                    "require_separation_by_end",
                    "require_strictly_decreasing_penetration",
                    "allow_recontact",
                ):
                    if field_name in contact and not isinstance(contact[field_name], bool):
                        return f"support_contacts[{index}].{field_name} must be boolean"

    if request.operation == "update_pose":
        if request.pose_position_m is None:
            return "update_pose requires pose_position_m"
        if not _is_vector(request.pose_position_m, 3):
            return "pose_position_m must have exactly 3 elements"
        # AUTHORED update_pose (no source timing) must carry an orientation —
        # spec 420 §4A.2. A pose-SOURCE update (live mirroring; identified by
        # source_timestamp_ns) may be position-only: the sink then keeps the
        # object's last known orientation, never identity.
        if request.pose_orientation_xyzw is None and request.source_timestamp_ns is None:
            return (
                "update_pose requires pose_orientation_xyzw for authored edits; "
                "only pose-source updates (with source_timestamp_ns) may omit it "
                "to keep the last known orientation"
            )
        if request.pose_orientation_xyzw is not None and not _is_vector(request.pose_orientation_xyzw, 4):
            return "pose_orientation_xyzw must have exactly 4 elements"
        source_timestamp = request.source_timestamp_ns
        source_budget = request.source_freshness_budget_ns
        if (source_timestamp is None) != (source_budget is None):
            return (
                "update_pose source_timestamp_ns and "
                "source_freshness_budget_ns must be provided together"
            )
        if source_timestamp is not None:
            if not isinstance(source_timestamp, int) or source_timestamp < 0:
                return "source_timestamp_ns must be a non-negative integer"
            if not isinstance(source_budget, int) or source_budget <= 0:
                return "source_freshness_budget_ns must be a positive integer"

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
    def acknowledge_completed(self, execution_id: int) -> CommandResult: ...

    @abstractmethod
    def acknowledge_intervention(
        self, principal_id: str, reason: str
    ) -> CommandResult:
        """Explicitly clear `kInterventionRequired`.

        Mirrors the C++ kernel API. Callers (cycle manager Run-side
        auto-ack, MCP, operator UI) hold the policy decision about
        whether the leftover state is safe to clear; the kernel only
        enforces fail-closed semantics. `execute()` rejects while the
        kernel is in `INTERVENTION_REQUIRED` until this is called.
        """
        ...

    def acknowledge_interrupted_for_retry(
        self, principal_id: str, reason: str
    ) -> CommandResult:
        """Clear an interrupted runtime-collision latch for operator retry.

        This differs from reset(): it must not command the backend or reset
        scene/robot state. Unsupported clients fail closed.
        """
        return CommandResult(
            accepted=False,
            error="acknowledge_interrupted_for_retry not supported by this kernel client",
        )

    def set_runtime_velocity_scale(
        self, scale: float, principal_id: str | None = None
    ) -> None:
        """Forward the viewer runtime velocity-scale control to the kernel.

        ADR-0018: this is a live execution control, not a behavior-tree
        parameter override. Implementations clamp finite values to [0, 1] and
        apply them to discrete execution without stopping the active action.
        `principal_id` is optional because not every direct kernel caller has
        supervisory actor context.
        """
        # Default implementation: no-op. Native/mocked clients that support
        # runtime scaling override.
        return None

    def set_command_gate(self, enabled: bool) -> bool:
        """Request the backend command-enable gate (single gate authority).

        The execution backend is the only publisher of the gate topic;
        flows that need command suppression (simulator reset) must request
        it through the kernel client instead of publishing the topic
        themselves. Returns False (fail-closed) when the backend has no
        gate surface.
        """
        return False

    def command_gate_enabled(self) -> bool:
        """Last gate value requested from the backend gate authority."""
        return False

    @abstractmethod
    def get_status(self) -> ExecutionStatus: ...

    @abstractmethod
    def get_execution_snapshot(self) -> ExecutionSnapshot: ...

    def get_execution_outcome(
        self, execution_id: int
    ) -> Optional[MotionExecutionOutcome]:
        """Return the immutable terminal MotionExecutionOutcome
        for `execution_id`, or None when no outcome has been recorded.

        Default returns None so kernel clients that do not yet implement
        the outcome surface (legacy mocks) keep building. Production
        kernel clients (NativeKernelClient, ROS bridges) MUST override.
        """
        return None

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

    def set_robot_base_pose(self, pose: Optional[Dict[str, Any]]) -> None:
        """Set the semantic robot base pose for backends that support it.

        Backends that do not model a movable robot base may ignore this. Runner
        calls it before applying program scene resources so adapter backends can
        project world-frame objects into their internal planning frame.
        """

    @abstractmethod
    def get_link_pose(
        self, target_frame: str, base_frame: str
    ) -> Optional[Dict[str, Any]]: ...

    def get_link_poses(
        self, target_frames: List[str], base_frame: str
    ) -> Dict[str, Dict[str, Any]]:
        """Return poses for multiple frames.

        Backends with native batch FK should override this method. The default
        loop preserves compatibility for simple mocks and non-native clients,
        but observer routes should still call this surface so native backends
        can avoid one scene-lock acquisition per visual frame.
        """
        poses: Dict[str, Dict[str, Any]] = {}
        for target_frame in target_frames:
            pose = self.get_link_pose(target_frame, base_frame)
            if pose is not None:
                poses[target_frame] = pose
        return poses

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

    def update_gripper_commands(self) -> None:
        """Advance any client-owned simulated gripper commands.

        Real adapters should normally publish state independently and may leave
        this as a no-op. Mock/simulated clients use it to keep status reads
        side-effect free while still supporting time-based command progress.
        """

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
        self._active_goal_positions_rad: List[float] = []
        self._last_joint_positions_rad: List[float] = []
        # Mock command-gate authority state (see set_command_gate).
        self.command_gate_requests: List[bool] = []
        self._command_gate_enabled = False
        self.last_execute_request: Optional[ExecuteRequest] = None
        self.execute_count = 0
        self.stop_count = 0
        self.pause_count = 0
        self.resume_count = 0
        self.reset_count = 0
        self.acknowledge_interrupted_for_retry_count = 0
        self.invalidate_count = 0
        self.runtime_velocity_scale = 1.0
        self.runtime_velocity_scale_updates: List[float] = []
        self.runtime_velocity_scale_principal_ids: List[Optional[str]] = []
        self.scene_edits: List[SceneEditRequest] = []
        self.link_pose_calls: List[tuple[str, str]] = []
        # Outcome store keyed by execution_id, populated
        # automatically when execute() returns. Tests can override the
        # default outcome shape via `next_outcome_factory`; if unset,
        # the mock emits `completed_normally` for free moves and
        # `no_signal_trip` for moves carrying motion_interrupt_scopes.
        self._outcomes: Dict[int, MotionExecutionOutcome] = {}
        self.next_outcome_factory = None  # type: ignore[assignment]
        # Optional one-shot preview-motion result injection for tests. When
        # set, the next preview_motion() consumes it and returns it verbatim
        # (mirrors next_outcome_factory). Lets tests drive ready/blocked
        # candidate planning with structured diagnostics (IK failure,
        # joint-limit, collision) without a real planner.
        self.next_preview_result: Optional[MotionPreviewResult] = None
        # Allow tests to inject failures
        self.fail_next_execute = False
        self.fail_next_scene_edit = False
        self._objects: Dict[str, Dict[str, Any]] = {}
        self.robot_base_pose: Optional[Dict[str, Any]] = None
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
        self._last_joint_positions_rad = [0.0 for _ in self._joint_names]
        self.last_gripper_command_request: Optional[GripperCommandRequest] = None
        self.gripper_command_count = 0
        self.fail_next_gripper_command = False
        self._gripper_commands: Dict[str, Dict[str, Any]] = {}
        self._gripper_positions: Dict[str, float] = {}
        # Gripper command/position state is mutated by the executing tree and
        # read by status polls (and test threads injecting failures). RLock:
        # command_gripper re-enters via update_gripper_commands/
        # get_gripper_status.
        self._gripper_lock = threading.RLock()
        # Kernel event buffer (mirrors C++ KernelEventCollector)
        self._kernel_events: Deque[Dict[str, Any]] = deque(maxlen=4096)

    def _emit_kernel_event(
        self,
        event_type: str,
        *,
        from_state: Optional[str] = None,
        to_state: Optional[str] = None,
        error: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None,
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
        if details:
            event["details"] = dict(details)
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
        self._active_goal_positions_rad = self._positions_for_request(request)
        self._emit_kernel_event("plan_started")
        if self._execute_duration_s > 0:
            self._state = ExecutionState.EXECUTING
            self._execute_start = time.monotonic()
            self._emit_kernel_event("state_transition", from_state="idle", to_state="executing")
            self._emit_kernel_event("execute_started")
        else:
            self._state = ExecutionState.COMPLETED
            self._last_joint_positions_rad = list(self._active_goal_positions_rad)
            self._metrics["executions_completed"] += 1
            self._emit_kernel_event("state_transition", from_state="idle", to_state="completed")
            self._emit_kernel_event("execute_completed")
            # Stamp an outcome for the completed execution.
            # Tests can override via `next_outcome_factory`; default
            # picks `no_signal_trip` for scoped moves and
            # `completed_normally` for free moves so the natural
            # MoveUntilSignal/ProbeSearchMove test path produces the
            # spec-required outcome shape.
            outcome = self._stamp_outcome(eid, request)
            self._outcomes[eid] = outcome
        return ExecuteResult(ok=True, execution_id=eid)

    def _positions_for_request(self, request: ExecuteRequest) -> List[float]:
        if (
            request.motion_type == "joint_goal"
            and request.joint_goal_positions_rad
        ):
            return [float(v) for v in request.joint_goal_positions_rad]
        return list(self._last_joint_positions_rad)

    def _stamp_outcome(
        self, execution_id: int, request: ExecuteRequest
    ) -> MotionExecutionOutcome:
        if callable(self.next_outcome_factory):
            outcome = self.next_outcome_factory(execution_id, request)
            self.next_outcome_factory = None
            return outcome
        if request.motion_interrupt_scopes:
            return MotionExecutionOutcome(
                execution_id=execution_id,
                move_id=request.move_id,
                terminal_cause="no_signal_trip",
            )
        return MotionExecutionOutcome(
            execution_id=execution_id,
            move_id=request.move_id,
            terminal_cause="completed_normally",
        )

    def get_execution_outcome(
        self, execution_id: int
    ) -> Optional[MotionExecutionOutcome]:
        return self._outcomes.get(execution_id)

    def preview_motion(
        self,
        request: ExecuteRequest,
        start_state: Optional[RobotStateSnapshot] = None,
    ) -> MotionPreviewResult:
        """Preview a motion without mutating execution state.

        The mock keeps this deliberately simple: it accepts all requests unless
        a failure has been injected via ``fail_next_execute`` and returns the
        requested pose as the final TCP pose for pose-goal motions. Tests that
        need a specific structured outcome (IK failure, joint-limit, collision)
        set ``next_preview_result`` to a prebuilt MotionPreviewResult.
        """
        if self.next_preview_result is not None:
            result = self.next_preview_result
            self.next_preview_result = None
            if result.ok:
                self._metrics["plans_succeeded"] += 1
                self._metrics["validations_passed"] += 1
            else:
                self._metrics["plans_failed"] += 1
            return result
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
            ExecutionState.INTERRUPTED,
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

    def acknowledge_completed(self, execution_id: int) -> CommandResult:
        status = self.get_status()
        if status.state != ExecutionState.COMPLETED:
            return CommandResult(
                accepted=False,
                error="Can only acknowledge a COMPLETED execution",
            )
        if status.execution_id != int(execution_id):
            return CommandResult(
                accepted=False,
                error=(
                    "Execution acknowledgement id mismatch: "
                    f"current={status.execution_id}, requested={execution_id}"
                ),
            )
        prev = self._state.value
        self._state = ExecutionState.IDLE
        self._active_move_id = ""
        self._emit_kernel_event(
            "acknowledge_completed",
            details={"acknowledged_execution_id": int(execution_id)},
        )
        self._emit_kernel_event("state_transition", from_state=prev, to_state="idle")
        return CommandResult(accepted=True)

    def acknowledge_intervention(
        self, principal_id: str, reason: str
    ) -> CommandResult:
        if self._state != ExecutionState.INTERVENTION_REQUIRED:
            return CommandResult(
                accepted=False,
                error=(
                    "acknowledge_intervention is valid only from "
                    f"INTERVENTION_REQUIRED; current state={self._state.value}"
                ),
            )
        prev = self._state.value
        self._state = ExecutionState.IDLE
        self._active_move_id = ""
        self._emit_kernel_event(
            "acknowledge_intervention",
            details={
                "principal_id": principal_id,
                "reason": reason,
                "prev_state": prev,
            },
        )
        self._emit_kernel_event(
            "state_transition", from_state=prev, to_state="idle"
        )
        return CommandResult(accepted=True)

    def acknowledge_interrupted_for_retry(
        self, principal_id: str, reason: str
    ) -> CommandResult:
        self.acknowledge_interrupted_for_retry_count += 1
        if self._state != ExecutionState.INTERRUPTED:
            return CommandResult(
                accepted=False,
                error=(
                    "acknowledge_interrupted_for_retry is valid only from "
                    f"INTERRUPTED; current state={self._state.value}"
                ),
            )
        prev = self._state.value
        self._state = ExecutionState.IDLE
        self._execution_id = 0
        self._active_move_id = ""
        self._emit_kernel_event(
            "acknowledge_interrupted_for_retry",
            details={
                "principal_id": principal_id,
                "reason": reason,
                "prev_state": prev,
            },
        )
        self._emit_kernel_event(
            "state_transition", from_state=prev, to_state="idle"
        )
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
                self._last_joint_positions_rad = list(self._active_goal_positions_rad)
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
            joint_positions_rad=list(self._last_joint_positions_rad),
        )

    def invalidate_trajectories(self) -> None:
        self.invalidate_count += 1
        self._metrics["cache_invalidations"] += 1

    def set_runtime_velocity_scale(
        self, scale: float, principal_id: str | None = None
    ) -> None:
        if not isinstance(scale, (int, float)) or math.isnan(scale) or math.isinf(scale):
            return
        clamped = max(0.0, min(1.0, float(scale)))
        self.runtime_velocity_scale = clamped
        self.runtime_velocity_scale_updates.append(clamped)
        self.runtime_velocity_scale_principal_ids.append(principal_id)

    def set_command_gate(self, enabled: bool) -> bool:
        # Mock gate authority: records requests so composer simulation-reset
        # tests can assert close/reopen ordering.
        self.command_gate_requests.append(bool(enabled))
        self._command_gate_enabled = bool(enabled)
        return True

    def command_gate_enabled(self) -> bool:
        return self._command_gate_enabled

    def set_robot_base_pose(self, pose: Optional[Dict[str, Any]]) -> None:
        self.robot_base_pose = None if not pose else dict(pose)
        self.invalidate_trajectories()

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
                "cylinder_radius_m": request.cylinder_radius_m,
                "cylinder_height_m": request.cylinder_height_m,
                "mesh_resource_uri": request.mesh_resource_uri,
                "mesh_scale": list(request.mesh_scale or [1.0, 1.0, 1.0])
                if request.shape_type == "mesh"
                else None,
                "position_m": list(request.pose_position_m),
                "orientation_xyzw": list(request.pose_orientation_xyzw),
                "frame_id": request.frame_id or "universe",
            }
        elif request.operation == "upsert":
            if state_before.exists and state_before.attached:
                error = "Cannot upsert attached object"
                self._record_scene_trace(
                    event=request.operation,
                    object_id=request.object_id,
                    success=False,
                    frame_id=request.frame_id,
                    attached=state_before.attached,
                    pose_in_frame=None,
                    pose_in_universe=pose_before,
                    error=error,
                )
                return SceneEditResult(success=False, error=error)
            self._objects[request.object_id] = {
                "shape_type": request.shape_type,
                "box_dimensions_m": list(request.box_dimensions_m or []) or None,
                "sphere_radius_m": request.sphere_radius_m,
                "cylinder_radius_m": request.cylinder_radius_m,
                "cylinder_height_m": request.cylinder_height_m,
                "mesh_resource_uri": request.mesh_resource_uri,
                "mesh_scale": list(request.mesh_scale or [1.0, 1.0, 1.0])
                if request.shape_type == "mesh"
                else None,
                "position_m": list(request.pose_position_m),
                "orientation_xyzw": list(request.pose_orientation_xyzw),
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
            tracked["position_m"] = list(request.pose_position_m)
            if request.pose_orientation_xyzw is not None:
                tracked["orientation_xyzw"] = list(request.pose_orientation_xyzw)
            # else: pose-source position-only update — keep the last known
            # orientation (spec 420 §4A.2 mirroring semantics), never identity.
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
            elif tracked.get("shape_type") == "cylinder":
                shape = SceneShape(
                    type="cylinder",
                    cylinder_radius_m=tracked.get("cylinder_radius_m"),
                    cylinder_height_m=tracked.get("cylinder_height_m"),
                )
            elif tracked.get("shape_type") == "mesh":
                shape = SceneShape(
                    type="mesh",
                    mesh_resource_uri=tracked.get("mesh_resource_uri"),
                    mesh_scale=list(tracked.get("mesh_scale") or [1.0, 1.0, 1.0]),
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
        with self._gripper_lock:
            self.gripper_command_count += 1
            self.last_gripper_command_request = request
            if self.fail_next_gripper_command:
                self.fail_next_gripper_command = False
                return CommandResult(
                accepted=False,
                error="Gripper command failed (injected)",
                reason_code="gripper_goal_rejected",
            )

            self.update_gripper_commands()
            current_status = self.get_gripper_status(request.gripper_id)
            if current_status.state == GripperCommandState.EXECUTING:
                return CommandResult(
                    accepted=False,
                    error=f"Gripper '{request.gripper_id}' is already executing a command",
                    reason_code="gripper_busy",
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

    def update_gripper_commands(self) -> None:
        with self._gripper_lock:
            for gripper_id in list(self._gripper_commands):
                status = self._compute_gripper_status(gripper_id)
                command = self._gripper_commands.get(gripper_id)
                if command is None:
                    continue
                command["state"] = status.state
                command["current_position"] = status.current_position
                command["progress"] = status.progress
                if status.state == GripperCommandState.COMPLETED:
                    self._gripper_positions[gripper_id] = (
                        float(status.current_position)
                        if status.current_position is not None
                        else self._gripper_positions.get(gripper_id, 0.0)
                    )

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        with self._gripper_lock:
            command = self._gripper_commands.get(gripper_id)
            if command is None:
                return GripperCommandStatus(
                    state=GripperCommandState.IDLE,
                    gripper_id=gripper_id,
                    current_position=self._gripper_positions.get(gripper_id),
                )

            return GripperCommandStatus(
                state=command["state"],
                gripper_id=gripper_id,
                progress=float(command.get("progress", 0.0)),
                current_position=command.get("current_position"),
                target_position=command.get("target_position"),
                error=command.get("error"),
            )
