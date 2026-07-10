"""NativeKernelClient — KernelClient backed by the all-C++ native kernel.

Only 1 Python↔C++ boundary crossing per call (no trampolines).
"""

from __future__ import annotations

import math
import time
from typing import Any, Dict, Optional

from manyforge_behavior.kernel_client import (
    ActionSpan,
    KernelClient,
    GripperCommandRequest,
    GripperCommandState,
    GripperCommandStatus,
    gripper_effective_command_request,
    gripper_observed_position,
    gripper_request_status_metadata,
    normalize_gripper_evidence,
    ExecutionState,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    ExecutionStatus,
    ExecutionSnapshot,
    MotionExecutionOutcome,
    MotionInterruptScope,
    MotionInterruptStopProfile,
    MotionPreviewResult,
    MotionTripOutcome,
    ObjectState,
    RobotStateSnapshot,
    SceneObjectSnapshot,
    SceneShape,
    SceneEditRequest,
    SceneEditResult,
    Trajectory,
    TrajectoryWaypoint,
)
from manyforge_behavior.reason_codes import ReasonCode

# Native bindings expose a stable state_name. Do not decode by numeric enum
# value: inserting a C++ enum member must not silently remap terminal states in
# Composer again.
_STATE_NAME_MAP = {
    "Idle": ExecutionState.IDLE,
    "Planning": ExecutionState.PLANNING,
    "Validating": ExecutionState.VALIDATING,
    "Executing": ExecutionState.EXECUTING,
    "Interrupted": ExecutionState.INTERRUPTED,
    "Paused": ExecutionState.PAUSED,
    "Stopping": ExecutionState.STOPPING,
    "Stopped": ExecutionState.STOPPED,
    "Completed": ExecutionState.COMPLETED,
    "Failed": ExecutionState.FAILED,
    "InterventionRequired": ExecutionState.INTERVENTION_REQUIRED,
}


def _state_from_native(payload) -> ExecutionState:
    state_name = payload.get("state_name")
    if state_name is None:
        raise ValueError(
            "native kernel payload is missing required stable state_name"
        )
    mapped = _STATE_NAME_MAP.get(str(state_name))
    if mapped is None:
        raise ValueError(f"unknown native kernel state_name: {state_name!r}")
    return mapped


_GRIPPER_STATE_NAME_MAP = {
    "idle": GripperCommandState.IDLE,
    "executing": GripperCommandState.EXECUTING,
    "completed": GripperCommandState.COMPLETED,
    "failed": GripperCommandState.FAILED,
}


def _gripper_state_from_native(payload) -> GripperCommandState:
    state_name = payload.get("state")
    if state_name is None:
        raise ValueError("native gripper payload is missing required state")
    mapped = _GRIPPER_STATE_NAME_MAP.get(str(state_name))
    if mapped is None:
        raise ValueError(f"unknown native gripper state: {state_name!r}")
    return mapped


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


def _serialize_motion_interrupt_scope(
    scope: MotionInterruptScope,
) -> Dict[str, Any]:
    """Serialize a Python MotionInterruptScope DTO into the
    dict shape the pybind `execute` parser consumes. Field names mirror
    the C++ MotionInterruptScopeDTO members so downstream parsing is
    straightforward."""
    out: Dict[str, Any] = {
        "scope_id": scope.scope_id,
        "policy_id": scope.policy_id,
        "activation_mode": scope.activation_mode,
        "required": bool(scope.required),
        "node_category": scope.node_category,
    }
    if scope.arm_timeout_ns is not None:
        out["arm_timeout_ns"] = int(scope.arm_timeout_ns)
    if scope.result_binding_key is not None:
        out["result_binding_key"] = scope.result_binding_key
    if scope.stop_profile is not None:
        profile = scope.stop_profile
        sp: Dict[str, Any] = {
            "profile_id": profile.profile_id,
            "stop_mode": profile.stop_mode,
            "allow_natural_completion_on_short_path": bool(
                profile.allow_natural_completion_on_short_path
            ),
            "require_backend_confirmation": bool(
                profile.require_backend_confirmation
            ),
        }
        if profile.deceleration_time_s is not None:
            sp["deceleration_time_s"] = float(profile.deceleration_time_s)
        if profile.max_stop_time_s is not None:
            sp["max_stop_time_s"] = float(profile.max_stop_time_s)
        if profile.settle_velocity_threshold_rad_s is not None:
            sp["settle_velocity_threshold_rad_s"] = float(
                profile.settle_velocity_threshold_rad_s
            )
        if profile.joint_deceleration_profile_id is not None:
            sp["joint_deceleration_profile_id"] = (
                profile.joint_deceleration_profile_id
            )
        if profile.per_joint_max_deceleration_rad_s2:
            sp["per_joint_max_deceleration_rad_s2"] = dict(
                profile.per_joint_max_deceleration_rad_s2
            )
        if profile.allowed_fallback_modes:
            sp["allowed_fallback_modes"] = list(profile.allowed_fallback_modes)
        out["stop_profile"] = sp
    return out


def _deserialize_motion_execution_outcome(
    raw: Optional[Dict[str, Any]],
) -> Optional[MotionExecutionOutcome]:
    """Deserialize the dict returned by the native bundle's
    get_execution_outcome into the Python DTO. Returns None when the
    native surface returns a falsy value (no outcome recorded yet)."""
    if not raw:
        return None
    outcome = MotionExecutionOutcome(
        execution_id=int(raw.get("execution_id", 0)),
        move_id=str(raw.get("move_id", "")),
        terminal_cause=str(raw.get("terminal_cause", "completed_normally")),
        action_span=_deserialize_action_span(raw.get("action_span")),
        contact_policy_id=raw.get("contact_policy_id"),
    )
    trip = raw.get("trip")
    if trip:
        outcome.trip = MotionTripOutcome(
            latch_id=str(trip.get("latch_id", "")),
            source_id=str(trip.get("source_id", "")),
            policy_id=str(trip.get("policy_id", "")),
            scope_id=str(trip.get("scope_id", "")),
            executed_stop_mode=str(trip.get("executed_stop_mode", "")),
            stop_command_source=str(trip.get("stop_command_source", "")),
            backend_stop_confirmed=bool(trip.get("backend_stop_confirmed", False)),
            stop_distance_rad=trip.get("stop_distance_rad"),
            initial_path_deviation_rad=trip.get("initial_path_deviation_rad"),
            path_deviation_max_rad=trip.get("path_deviation_max_rad"),
            fallback_reason=trip.get("fallback_reason"),
            signal_to_latch_ns=int(trip.get("signal_to_latch_ns", 0)),
            latch_to_backend_stop_call_ns=int(
                trip.get("latch_to_backend_stop_call_ns", 0)
            ),
            latch_to_stop_confirmed_ns=int(
                trip.get("latch_to_stop_confirmed_ns", 0)
            ),
            result_binding_key=trip.get("result_binding_key"),
        )
        # Full outcome fidelity.
        # Snapshot source/state/captured_at_ns + requested-mode +
        # diagnostics + profile_id surface back through the Python DTO
        # so BT consumers can audit the trip without re-querying the
        # kernel. Extra fields land on `MotionTripOutcome` as attribute
        # writes; the dataclass has the named fields above, and the
        # rest live on a dynamic attribute bag so we don't churn the
        # Python DTO schema with every backend addition.
        outcome.trip.stop_snapshot_source = trip.get("stop_snapshot_source")
        outcome.trip.stop_snapshot_captured_at_ns = int(
            trip.get("stop_snapshot_captured_at_ns", 0)
        )
        outcome.trip.stop_snapshot_robot_state = trip.get(
            "stop_snapshot_robot_state"
        )
        # Measured settle signal (M3): surface confirmed-at-rest evidence to
        # BT/HMI/audit consumers. Absent => settle UNKNOWN (no measured velocity).
        outcome.trip.stop_snapshot_max_abs_velocity_rad_s = trip.get(
            "stop_snapshot_max_abs_velocity_rad_s"
        )
        outcome.trip.stop_snapshot_at_rest = trip.get("stop_snapshot_at_rest")
        outcome.trip.requested_stop_mode = trip.get("requested_stop_mode")
        outcome.trip.requested_stop_profile_id = trip.get(
            "requested_stop_profile_id"
        )
        outcome.trip.diagnostics = trip.get("diagnostics") or {}
    err = raw.get("error")
    if err:
        outcome.error_code = err.get("code")
        outcome.error_message = err.get("message")
        details = err.get("details") or {}
        outcome.error_reason = details.get("reason")
    return outcome


def _serialize_per_joint_dynamics_limits(
    request: ExecuteRequest,
) -> Optional[Dict[str, Dict[str, float]]]:
    """Serialize ExecuteRequest.per_joint_dynamics_limits into the native dict
    shape (Phase 4-P5.1K). Only present (non-None) fields are emitted; returns
    None when there are no caps so the request dict omits the key (empty map =
    prior behavior)."""
    limits = getattr(request, "per_joint_dynamics_limits", None)
    if not limits:
        return None
    out: Dict[str, Dict[str, float]] = {}
    for joint_name, limit in limits.items():
        entry: Dict[str, float] = {}
        v = getattr(limit, "max_velocity_rad_s", None)
        a = getattr(limit, "max_acceleration_rad_s2", None)
        j = getattr(limit, "max_jerk_rad_s3", None)
        if v is not None:
            entry["max_velocity_rad_s"] = float(v)
        if a is not None:
            entry["max_acceleration_rad_s2"] = float(a)
        if j is not None:
            entry["max_jerk_rad_s3"] = float(j)
        if entry:
            out[str(joint_name)] = entry
    return out or None


def _serialize_action_span(span: Optional[ActionSpan]) -> Optional[Dict[str, str]]:
    if span is None:
        return None
    payload: Dict[str, str] = {
        "action_source": str(span.action_source),
        "action_span_id": str(span.action_span_id),
    }
    if span.program_run_id:
        payload["program_run_id"] = str(span.program_run_id)
    if span.node_instance_id:
        payload["node_instance_id"] = str(span.node_instance_id)
    if span.node_attempt_id:
        payload["node_attempt_id"] = str(span.node_attempt_id)
    if span.parent_node_attempt_id:
        payload["parent_node_attempt_id"] = str(span.parent_node_attempt_id)
    return payload


def _deserialize_action_span(raw: Any) -> Optional[ActionSpan]:
    if not isinstance(raw, dict):
        return None
    return ActionSpan(
        action_source=str(raw.get("action_source", "system")),
        action_span_id=str(raw.get("action_span_id", "")),
        program_run_id=raw.get("program_run_id"),
        node_instance_id=raw.get("node_instance_id"),
        node_attempt_id=raw.get("node_attempt_id"),
        parent_node_attempt_id=raw.get("parent_node_attempt_id"),
    )


def _add_request_correlation(req_dict: Dict[str, Any], request: ExecuteRequest) -> None:
    if request.program_run_id:
        req_dict["program_run_id"] = request.program_run_id
    action_span = _serialize_action_span(getattr(request, "action_span", None))
    if action_span:
        req_dict["action_span"] = action_span
    if getattr(request, "contact_policy_id", None):
        req_dict["contact_policy_id"] = str(request.contact_policy_id)


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

    def __init__(
        self,
        bundle,
        *,
        robot_model_authority_metadata: Optional[Dict[str, Any]] = None,
        robot_model_authority_temp_dir: Any = None,
    ) -> None:
        """Initialize with a NativeKernelBundle from manyforge_roboplan_py.

        Args:
            bundle: NativeKernelBundle returned by create_native_kernel()
        """
        self._bundle = bundle
        self.robot_model_authority_metadata = dict(
            robot_model_authority_metadata or {}
        )
        self._robot_model_authority_temp_dir = robot_model_authority_temp_dir
        self._canonical_to_view_joint_names = dict(
            self.robot_model_authority_metadata.get(
                "canonical_to_view_joint_names",
                {},
            )
            or {}
        )
        self._view_to_canonical_joint_names = dict(
            self.robot_model_authority_metadata.get(
                "view_to_canonical_joint_names",
                {},
            )
            or {}
        )
        self._group_commandable_joint_names = {
            str(group_id): [str(joint) for joint in joints]
            for group_id, joints in (
                self.robot_model_authority_metadata.get(
                    "group_commandable_joint_names",
                    {},
                )
                or {}
            ).items()
        }
        attach_metadata = getattr(
            self._bundle,
            "set_robot_model_authority_metadata",
            None,
        )
        if callable(attach_metadata) and self.robot_model_authority_metadata:
            attach_metadata(self.robot_model_authority_metadata)
        self._active_gripper_commands: Dict[str, Dict[str, Any]] = {}
        self._gripper_status_cache: Dict[str, GripperCommandStatus] = {}
        self._gripper_semantic_metadata: Dict[str, Dict[str, Any]] = {}

    def _view_joint_name(self, canonical_joint_name: str) -> str:
        return self._canonical_to_view_joint_names.get(
            canonical_joint_name,
            canonical_joint_name,
        )

    def _get_group_joint_index(self, group_id: str, joint_name: str) -> int:
        commandable = self._group_commandable_joint_names.get(group_id)
        joint_names = (
            list(commandable)
            if commandable
            else list(self._bundle.get_group_joint_names(group_id))
        )
        group_positions = self._get_group_joint_positions(group_id)
        if len(joint_names) != len(group_positions):
            commandable = self._group_commandable_joint_names.get(group_id)
            if commandable and joint_names != list(commandable):
                joint_names = list(commandable)
        if len(joint_names) != len(group_positions):
            raise ValueError(
                f"Gripper group '{group_id}' reports {len(joint_names)} joints "
                f"but {len(group_positions)} position slots"
            )
        if joint_name not in joint_names:
            raise ValueError(
                f"Joint '{joint_name}' was not found in gripper group '{group_id}'"
            )
        return joint_names.index(joint_name)

    def _get_group_joint_positions(self, group_id: str) -> list[float]:
        commandable_positions = getattr(
            self._bundle,
            "get_group_commandable_joint_positions",
            None,
        )
        if callable(commandable_positions):
            return list(commandable_positions(group_id))
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
            commanded_position=command.get("commanded_position"),
            observed_position=gripper_observed_position(
                str(command.get("state_authority", "")),
                current_position,
            ),
            commanded_state=str(command.get("commanded_state", "")),
            actuation_kind=str(command.get("actuation_kind", "")),
            state_authority=str(command.get("state_authority", "")),
            completion_policy=str(command.get("completion_policy", "")),
            grasp_confirmation_policy=str(
                command.get("grasp_confirmation_policy", "")
            ),
            transport_kind=str(command.get("transport_kind", "")),
            evidence=normalize_gripper_evidence(command.get("evidence")),
            evidence_state=str(command.get("evidence_state", "")),
        )
        self._gripper_status_cache[gripper_id] = status
        if status.state == GripperCommandState.COMPLETED:
            self._active_gripper_commands.pop(gripper_id, None)
        return status

    def update_gripper_commands(self) -> None:
        if hasattr(self._bundle, "update_ros_grippers"):
            try:
                self._bundle.update_ros_grippers()
            except RuntimeError:
                pass
        for gripper_id in list(self._active_gripper_commands):
            self._compute_gripper_command_status(gripper_id)

    def _request_to_native_dict(self, request: ExecuteRequest) -> dict[str, Any]:
        relative_move = request.relative_move
        if relative_move is not None:
            relative_move = dict(relative_move)
            target_reference = relative_move.get("target_reference")
            if target_reference is not None:
                target_reference = dict(target_reference)
                if target_reference.get("type") == "pose":
                    target_reference["pose"] = _normalize_pose_dict(
                        target_reference.get("pose")
                    )
                relative_move["target_reference"] = target_reference
            offset_reference = relative_move.get("offset_reference")
            if offset_reference is not None:
                offset_reference = dict(offset_reference)
                if offset_reference.get("type") == "pose":
                    offset_reference["pose"] = _normalize_pose_dict(
                        offset_reference.get("pose")
                    )
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

        _add_request_correlation(req_dict, request)

        dyn_limits = _serialize_per_joint_dynamics_limits(request)
        if dyn_limits is not None:
            req_dict["per_joint_dynamics_limits"] = dyn_limits

        if request.motion_type == "joint_goal":
            req_dict["joint_goal_names"] = request.joint_goal_names
            req_dict["joint_goal_positions_rad"] = request.joint_goal_positions_rad
        elif request.motion_type == "pose_goal":
            req_dict["pose_goal"] = request.pose_goal
        elif request.motion_type == "relative_move":
            req_dict["relative_move"] = relative_move
        return req_dict

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

        _add_request_correlation(req_dict, request)

        dyn_limits = _serialize_per_joint_dynamics_limits(request)
        if dyn_limits is not None:
            req_dict["per_joint_dynamics_limits"] = dyn_limits

        if request.motion_type == "joint_goal":
            req_dict["joint_goal_names"] = request.joint_goal_names
            req_dict["joint_goal_positions_rad"] = request.joint_goal_positions_rad
        elif request.motion_type == "pose_goal":
            req_dict["pose_goal"] = request.pose_goal
        elif request.motion_type == "relative_move":
            req_dict["relative_move"] = relative_move

        # Motion-scoped interrupt scopes (spec/210 §3.10 +
        # spec/260 §2.6). Each scope arms a move-scoped policy for
        # exactly this execute request; the kernel arms them
        # atomically after validation and reaps on every terminal
        # state. Without this serialization, the scope DTOs carried
        # by `MoveUntilSignal`/`ProbeSearchMove` would silently
        # disappear at the Python/native boundary and the move would
        # run unscoped.
        if request.motion_interrupt_scopes:
            req_dict["motion_interrupt_scopes"] = [
                _serialize_motion_interrupt_scope(scope)
                for scope in request.motion_interrupt_scopes
            ]

        result = self._bundle.execute(req_dict)

        return ExecuteResult(
            ok=result["ok"],
            execution_id=result.get("execution_id", 0),
            error=result.get("error"),
        )

    def get_execution_outcome(self, execution_id):
        """Fetch the terminal MotionExecutionOutcome for a
        completed scoped execution from the native kernel."""
        try:
            raw = self._bundle.get_execution_outcome(int(execution_id))
        except (AttributeError, RuntimeError):
            # Native bundle predates the outcome store. Return None
            # so callers (BT nodes) fail closed per spec/210 §3.11.
            return None
        return _deserialize_motion_execution_outcome(raw)

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

        _add_request_correlation(req_dict, request)

        dyn_limits = _serialize_per_joint_dynamics_limits(request)
        if dyn_limits is not None:
            req_dict["per_joint_dynamics_limits"] = dyn_limits

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

    def plan_motion(
        self,
        request: ExecuteRequest,
        start_state: Optional[RobotStateSnapshot] = None,
    ) -> tuple[MotionPreviewResult, Optional[Trajectory]]:
        """Plan and validate a motion, returning the trajectory for external executors.

        This is intentionally narrower than ``execute``: the native RoboPlan
        context remains the planning and validation authority, while callers
        such as the ROS 2 execution adapter own the final transport dispatch.
        """
        start_state_dict = None
        if start_state is not None:
            start_state_dict = {
                "joint_names": list(start_state.joint_names),
                "positions_rad": list(start_state.positions_rad),
                "velocities_rad_s": list(start_state.velocities_rad_s),
            }

        result = self._bundle.plan_motion(
            self._request_to_native_dict(request),
            start_state_dict,
        )
        final_state = result.get("final_state")
        preview = MotionPreviewResult(
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
                    "orientation_xyzw": list(
                        result["final_tcp_pose"]["orientation_xyzw"]
                    ),
                }
            ),
        )
        if not preview.ok:
            return preview, None

        trajectory_data = result.get("trajectory")
        if trajectory_data is None:
            return (
                MotionPreviewResult(
                    ok=False,
                    error="Native plan_motion did not return a trajectory",
                ),
                None,
            )
        waypoints = [
            TrajectoryWaypoint(
                time_from_start_s=float(wp["time_from_start_s"]),
                positions_rad=list(wp["positions_rad"]),
            )
            for wp in trajectory_data.get("waypoints", [])
        ]
        trajectory = Trajectory(
            joint_names=list(trajectory_data.get("joint_names", [])),
            waypoints=waypoints,
        )
        return preview, trajectory

    def stop(self) -> CommandResult:
        r = self._bundle.stop()
        return self._command_result(r)

    def pause(self) -> CommandResult:
        r = self._bundle.pause()
        return self._command_result(r)

    def control_command_epoch(self) -> int:
        return int(self._bundle.control_command_epoch())

    def bump_control_command_epoch(self) -> int:
        return int(self._bundle.bump_control_command_epoch())

    def clock_anchor(self) -> dict:
        anchor = self._bundle.clock_anchor()
        return {
            "monotonic_ns": int(anchor["monotonic_ns"]),
            "wall_time_ns": int(anchor["wall_time_ns"]),
        }

    def set_flight_recorder_dir(self, directory: str) -> None:
        self._bundle.set_flight_recorder_dir(str(directory))

    def dump_flight_record(self, reason: str) -> str:
        return str(self._bundle.dump_flight_record(str(reason)))

    def drain_kernel_events_with_stats(self) -> dict:
        stats = self._bundle.drain_kernel_events_with_stats()
        return {
            "events": list(stats["events"]),
            "dropped_total": int(stats["dropped_total"]),
        }

    def _command_result(self, result: dict) -> CommandResult:
        details = result.get("error_details") or {}
        reason_code = details.get("reason_code") or details.get("reason")
        return CommandResult(
            accepted=result["accepted"],
            error=result.get("error"),
            reason_code=reason_code,
            details=dict(details),
        )

    def resume(self, *, expected_epoch: int | None = None) -> CommandResult:
        r = self._bundle.resume(expected_epoch=expected_epoch)
        return self._command_result(r)

    def reset(self, *, expected_epoch: int | None = None) -> CommandResult:
        r = self._bundle.reset(expected_epoch=expected_epoch)
        return self._command_result(r)

    def acknowledge_completed(
        self, execution_id: int, *, expected_epoch: int | None = None
    ) -> CommandResult:
        r = self._bundle.acknowledge_completed(
            int(execution_id), expected_epoch=expected_epoch
        )
        return self._command_result(r)

    def acknowledge_intervention(
        self,
        principal_id: str,
        reason: str,
        *,
        expected_epoch: int | None = None,
    ) -> CommandResult:
        r = self._bundle.acknowledge_intervention(
            str(principal_id), str(reason), expected_epoch=expected_epoch
        )
        return self._command_result(r)

    def acknowledge_intervention_for_retry(
        self,
        principal_id: str,
        reason: str,
        *,
        expected_epoch: int | None = None,
    ) -> CommandResult:
        r = self._bundle.acknowledge_intervention_for_retry(
            str(principal_id), str(reason), expected_epoch=expected_epoch
        )
        return self._command_result(r)

    def acknowledge_failed_validation_for_retry(
        self,
        principal_id: str,
        reason: str,
        *,
        expected_epoch: int | None = None,
    ) -> CommandResult:
        r = self._bundle.acknowledge_failed_validation_for_retry(
            str(principal_id), str(reason), expected_epoch=expected_epoch
        )
        return self._command_result(r)

    def acknowledge_interrupted_for_retry(
        self,
        principal_id: str,
        reason: str,
        *,
        expected_epoch: int | None = None,
    ) -> CommandResult:
        r = self._bundle.acknowledge_interrupted_for_retry(
            str(principal_id), str(reason), expected_epoch=expected_epoch
        )
        return self._command_result(r)

    def set_runtime_velocity_scale(
        self, scale: float, principal_id: str | None = None
    ) -> None:
        # ADR-0018: forward runtime UI velocity-scale changes to the C++
        # kernel/backend. The kernel validates finiteness and clamps to [0, 1].
        import math

        if not isinstance(scale, (int, float)) or math.isnan(scale) or math.isinf(scale):
            return
        self._bundle.set_runtime_velocity_scale(float(scale), principal_id)

    def get_motion_interrupt_stop_capability(self) -> dict:
        """Return the active native backend's controlled-stop capability.

        Composer may only have a KernelClient handle for native/composer-only
        deployments. Forwarding the bundle capability here keeps the same
        fail-closed stop-profile gate available whether Composer cached the
        raw NativeKernelBundle or only the wrapped NativeKernelClient.
        """
        return dict(self._bundle.get_motion_interrupt_stop_capability())

    def close(self) -> None:
        """Deterministically quiesce and release the native bundle.

        Shutdown-safety sequence (idempotent):
        1. best-effort stop of any in-flight execution,
        2. explicit command-gate close — the gate must not depend on the
           native destructor happening to run at interpreter exit,
        3. drop the bundle reference so refcounting can destroy the native
           backend (cancel goals, close gate, join monitor) now instead of
           at an unspecified GC point.

        The client is unusable afterwards; any further call fails loudly on
        the missing bundle.
        """
        bundle = getattr(self, "_bundle", None)
        if bundle is None:
            return
        try:
            state = _state_from_native(bundle.get_status())
            if state in {
                ExecutionState.PLANNING,
                ExecutionState.VALIDATING,
                ExecutionState.EXECUTING,
                ExecutionState.INTERRUPTED,
                ExecutionState.PAUSED,
                ExecutionState.STOPPING,
            }:
                bundle.stop()
        except Exception:  # noqa: BLE001 — best-effort quiesce; gate close still runs
            pass
        try:
            bundle.set_command_gate(False)
        except RuntimeError:
            # Non-ros2_control execution backend: no gate surface.
            pass
        except Exception:  # noqa: BLE001 — gate close is best-effort by transport
            pass
        try:
            bundle.close()
        finally:
            self._bundle = None

    def set_command_gate(self, enabled: bool) -> bool:
        """Request the command-enable gate from the backend gate authority.

        The ros2_control backend is the single publisher of the gate topic
        (reliable + transient_local); external flows such as the
        simulator-reset sequence must go through here rather than publishing
        the topic themselves. Returns False when the deployment configures
        no gate topic or the backend is not ros2_control.
        """
        try:
            return bool(self._bundle.set_command_gate(bool(enabled)))
        except RuntimeError:
            # Non-ros2_control execution backend: no gate surface.
            return False

    def command_gate_enabled(self) -> bool:
        try:
            return bool(self._bundle.command_gate_enabled())
        except RuntimeError:
            return False

    def get_status(self) -> ExecutionStatus:
        s = self._bundle.get_status()
        return ExecutionStatus(
            state=_state_from_native(s),
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
            state=_state_from_native(s),
            progress=s.get("progress", 0.0),
            joint_positions_rad=list(s.get("joint_positions_rad", [])),
            state_source=s.get("state_source", ""),
            source_timestamp_ns=int(s.get("source_timestamp_ns", 0)),
            source_age_ns=int(s.get("source_age_ns", 0)),
            error=s.get("error"),
            error_code=s.get("error_code"),
            error_details=dict(s.get("error_details", {})),
            action_span=_deserialize_action_span(s.get("action_span")),
            contact_policy_id=s.get("contact_policy_id"),
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
            pose_in_frame = object_data.get("pose_in_frame")
            if pose_in_frame is not None:
                pose_in_frame = {
                    "position_m": list(pose_in_frame["position_m"]),
                    "orientation_xyzw": list(pose_in_frame["orientation_xyzw"]),
                }
            objects.append(
                SceneObjectSnapshot(
                    object_id=object_data["object_id"],
                    shape=shape,
                    pose_in_frame=pose_in_frame,
                    pose_in_universe=pose_in_universe,
                    attached=bool(object_data.get("attached", False)),
                    frame_id=object_data.get("frame_id"),
                    support_contacts=[
                        dict(item)
                        for item in object_data.get("support_contacts", [])
                        if isinstance(item, dict)
                    ],
                )
            )
        return objects

    def get_metrics(self) -> Dict[str, int]:
        return dict(self._bundle.get_metrics())

    def command_gripper(self, request: GripperCommandRequest) -> CommandResult:
        effective_request, semantic_error = gripper_effective_command_request(
            request
        )
        if effective_request is None:
            return CommandResult(
                accepted=False,
                error=semantic_error or "Invalid gripper command semantics",
                reason_code="invalid_gripper_semantics",
            )
        request = effective_request
        self._gripper_semantic_metadata[request.gripper_id] = (
            gripper_request_status_metadata(request)
        )
        if (
            request.transport_kind != "native_kinematic"
            and hasattr(self._bundle, "command_ros_gripper")
        ):
            try:
                result = self._bundle.command_ros_gripper(
                    request.gripper_id,
                    float(request.target_position),
                    float(request.duration_s),
                    0.0 if request.max_effort is None else float(request.max_effort),
                )
            except RuntimeError:
                result = None
            if result is not None:
                return CommandResult(
                    accepted=bool(result.get("accepted", False)),
                    error=result.get("error"),
                    reason_code=result.get("reason_code"),
                )

        self.update_gripper_commands()
        current = self.get_gripper_status(request.gripper_id)
        supersedable_grasp_loss = (
            current.state == GripperCommandState.FAILED
            and current.evidence_state == "pressure_or_part_lost"
            and current.reason_code == "grasp_confirmation_lost"
        )
        if (
            current.state == GripperCommandState.FAILED
            and current.reason_code
            and not supersedable_grasp_loss
        ):
            return CommandResult(
                accepted=False,
                error=current.error or "Gripper state is unavailable",
                reason_code=current.reason_code,
            )
        if current.state == GripperCommandState.EXECUTING:
            return CommandResult(
                accepted=False,
                error=f"Gripper '{request.gripper_id}' is already executing a command",
                reason_code="gripper_busy",
            )

        try:
            joint_index = self._get_group_joint_index(
                request.group_id,
                request.joint_name,
            )
            group_positions = self._get_group_joint_positions(request.group_id)
        except Exception as exc:
            return CommandResult(
                accepted=False,
                error=str(exc),
                reason_code=ReasonCode.GRIPPER_STATE_UNAVAILABLE.value,
            )

        start_position = float(group_positions[joint_index])
        if request.duration_s <= 0.0:
            group_positions[joint_index] = float(request.target_position)
            self._bundle.set_group_joint_positions(request.group_id, group_positions)
            metadata = gripper_request_status_metadata(
                request,
                current_position=float(request.target_position),
            )
            self._gripper_semantic_metadata[request.gripper_id] = metadata
            self._gripper_status_cache[request.gripper_id] = GripperCommandStatus(
                state=GripperCommandState.COMPLETED,
                gripper_id=request.gripper_id,
                progress=1.0,
                current_position=float(request.target_position),
                target_position=float(request.target_position),
                **metadata,
            )
            self._active_gripper_commands.pop(request.gripper_id, None)
            return CommandResult(accepted=True)

        metadata = gripper_request_status_metadata(
            request,
            current_position=start_position,
        )
        self._gripper_semantic_metadata[request.gripper_id] = metadata
        self._active_gripper_commands[request.gripper_id] = {
            "group_id": request.group_id,
            "joint_name": request.joint_name,
            "joint_index": joint_index,
            "start_position": start_position,
            "target_position": float(request.target_position),
            "duration_s": float(request.duration_s),
            "start_time": time.monotonic(),
            **metadata,
        }
        self._gripper_status_cache[request.gripper_id] = GripperCommandStatus(
            state=GripperCommandState.EXECUTING,
            gripper_id=request.gripper_id,
            progress=0.0,
            current_position=start_position,
            target_position=float(request.target_position),
            **metadata,
        )
        return CommandResult(accepted=True)

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        cached = self._gripper_status_cache.get(gripper_id)
        if (
            cached is not None
            and cached.evidence_state == "pressure_or_part_lost"
            and cached.reason_code == "grasp_confirmation_lost"
        ):
            return cached
        if hasattr(self._bundle, "get_ros_gripper_status"):
            try:
                status = self._bundle.get_ros_gripper_status(gripper_id)
            except RuntimeError as exc:
                return GripperCommandStatus(
                    state=GripperCommandState.FAILED,
                    gripper_id=gripper_id,
                    error=str(exc),
                    reason_code=ReasonCode.GRIPPER_STATE_UNAVAILABLE.value,
                )
            if status is not None:
                metadata = self._gripper_semantic_metadata.get(gripper_id, {})
                current_position = status.get("current_position")
                observed_position = status.get(
                    "observed_position",
                    metadata.get("observed_position"),
                )
                if observed_position is None:
                    observed_position = gripper_observed_position(
                        str(metadata.get("state_authority", "")),
                        current_position,
                    )
                return GripperCommandStatus(
                    state=_gripper_state_from_native(status),
                    gripper_id=gripper_id,
                    progress=float(status.get("progress", 0.0)),
                    current_position=current_position,
                    target_position=status.get("target_position"),
                    commanded_position=status.get(
                        "commanded_position",
                        metadata.get("commanded_position"),
                    ),
                    observed_position=observed_position,
                    commanded_state=str(
                        status.get(
                            "commanded_state",
                            metadata.get("commanded_state", ""),
                        )
                    ),
                    actuation_kind=str(
                        status.get(
                            "actuation_kind",
                            metadata.get("actuation_kind", ""),
                        )
                    ),
                    state_authority=str(
                        status.get(
                            "state_authority",
                            metadata.get("state_authority", ""),
                        )
                    ),
                    completion_policy=str(
                        status.get(
                            "completion_policy",
                            metadata.get("completion_policy", ""),
                        )
                    ),
                    grasp_confirmation_policy=str(
                        status.get(
                            "grasp_confirmation_policy",
                            metadata.get("grasp_confirmation_policy", ""),
                        )
                    ),
                    transport_kind=str(
                        status.get(
                            "transport_kind",
                            metadata.get("transport_kind", ""),
                        )
                    ),
                    evidence=normalize_gripper_evidence(
                        status.get("evidence", metadata.get("evidence", []))
                    ),
                    evidence_state=str(
                        status.get(
                            "evidence_state",
                            metadata.get("evidence_state", ""),
                        )
                    ),
                    error=status.get("error"),
                    reason_code=status.get("reason_code"),
                )
        return self._gripper_status_cache.get(
            gripper_id,
            GripperCommandStatus(
                state=GripperCommandState.IDLE,
                gripper_id=gripper_id,
            ),
        )

    def mark_gripper_grasp_lost(
        self,
        *,
        gripper_id: str,
        evidence_label: str,
        message: str,
        reason_code: str = "grasp_confirmation_lost",
    ) -> CommandResult:
        label = str(evidence_label or "").strip()
        if not label:
            return CommandResult(
                accepted=False,
                error="grasp loss evidence_label is required",
                reason_code="invalid_gripper_semantics",
            )
        mark_ros = getattr(self._bundle, "mark_ros_gripper_grasp_lost", None)
        if callable(mark_ros):
            try:
                result = mark_ros(
                    gripper_id,
                    label,
                    str(message or "gripper grasp confirmation was lost"),
                    str(reason_code or "grasp_confirmation_lost"),
                )
            except RuntimeError:
                result = None
            if isinstance(result, dict) and bool(result.get("accepted")):
                self._gripper_status_cache.pop(gripper_id, None)
                return CommandResult(accepted=True)
            if isinstance(result, dict) and result.get("reason_code") != (
                "gripper_not_configured"
            ):
                return CommandResult(
                    accepted=False,
                    error=str(result.get("error") or "gripper grasp-loss mark failed"),
                    reason_code=str(
                        result.get("reason_code") or "invalid_gripper_semantics"
                    ),
                )
        current = self.get_gripper_status(gripper_id)
        if current.state == GripperCommandState.IDLE and not (
            current.evidence
            or current.commanded_state
            or current.target_position is not None
        ):
            return CommandResult(
                accepted=False,
                error=f"Gripper '{gripper_id}' has no command status to invalidate",
                reason_code="stale_gripper_state",
            )
        evidence = normalize_gripper_evidence(current.evidence)
        if label not in evidence:
            evidence.append(label)
        failed = GripperCommandStatus(
            state=GripperCommandState.FAILED,
            gripper_id=gripper_id,
            progress=current.progress,
            current_position=current.current_position,
            target_position=current.target_position,
            commanded_position=current.commanded_position,
            observed_position=current.observed_position,
            commanded_state=current.commanded_state,
            actuation_kind=current.actuation_kind,
            state_authority=current.state_authority,
            completion_policy=current.completion_policy,
            grasp_confirmation_policy=current.grasp_confirmation_policy,
            transport_kind=current.transport_kind,
            evidence=evidence,
            evidence_state="pressure_or_part_lost",
            error=str(message or "gripper grasp confirmation was lost"),
            reason_code=str(reason_code or "grasp_confirmation_lost"),
        )
        self._active_gripper_commands.pop(gripper_id, None)
        self._gripper_status_cache[gripper_id] = failed
        return CommandResult(accepted=True)

    def drain_kernel_events(self) -> list[dict]:
        return list(self._bundle.drain_kernel_events())

    def get_scene_trace(self, limit: Optional[int] = None) -> list[dict]:
        return list(self._bundle.get_scene_trace(-1 if limit is None else int(limit)))

    def clear_scene_trace(self) -> None:
        self._bundle.clear_scene_trace()

    def scene_update_status_counters(self) -> Dict[str, int]:
        if not hasattr(self._bundle, "scene_update_status_counters"):
            return {}
        return {
            str(key): int(value)
            for key, value in dict(self._bundle.scene_update_status_counters()).items()
        }

    def invalidate_trajectories(self, action_span: Optional[ActionSpan] = None) -> None:
        _ = action_span
        self._bundle.invalidate_trajectories()

    def set_robot_base_pose(self, pose: Optional[Dict[str, Any]]) -> None:
        normalized = _normalize_pose_dict(pose) or {}
        self._bundle.set_robot_base_pose(normalized)

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        if request.operation == "upsert":
            state = self.get_object_state(request.object_id)
            if state.exists and state.attached:
                return SceneEditResult(success=False, error="Cannot upsert attached object")
            # Skip the no-op remove when the object doesn't exist yet.
            # A redundant "Could not find object" round trip is pure noise in
            # the scene trace and forces the planner to churn pairs unnecessarily.
            if state.exists:
                remove_result = self.scene_edit(
                    SceneEditRequest(
                        operation="remove",
                        object_id=request.object_id,
                        action_span=request.action_span,
                    )
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
                    cylinder_radius_m=request.cylinder_radius_m,
                    cylinder_height_m=request.cylinder_height_m,
                    mesh_resource_uri=request.mesh_resource_uri,
                    mesh_scale=request.mesh_scale,
                    pose_position_m=request.pose_position_m,
                    pose_orientation_xyzw=request.pose_orientation_xyzw,
                    frame_id=request.frame_id,
                    touch_links=request.touch_links,
                    support_contacts=request.support_contacts,
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
        if request.cylinder_radius_m is not None:
            req_dict["cylinder_radius_m"] = request.cylinder_radius_m
        if request.cylinder_height_m is not None:
            req_dict["cylinder_height_m"] = request.cylinder_height_m
        if request.mesh_resource_uri is not None:
            req_dict["mesh_resource_uri"] = request.mesh_resource_uri
        if request.mesh_scale is not None:
            req_dict["mesh_scale"] = request.mesh_scale
        if request.pose_position_m:
            req_dict["pose_position_m"] = request.pose_position_m
        if request.pose_orientation_xyzw:
            req_dict["pose_orientation_xyzw"] = request.pose_orientation_xyzw
        if request.frame_id:
            req_dict["frame_id"] = request.frame_id
        if request.touch_links is not None:
            req_dict["touch_links"] = list(request.touch_links)
        if request.support_contacts is not None:
            req_dict["support_contacts"] = list(request.support_contacts)
        if request.source_timestamp_ns is not None:
            req_dict["source_timestamp_ns"] = int(request.source_timestamp_ns)
        if request.source_freshness_budget_ns is not None:
            req_dict["source_freshness_budget_ns"] = int(
                request.source_freshness_budget_ns
            )
        if request.source_id is not None:
            req_dict["source_id"] = str(request.source_id)
        if request.scene_admission_role is not None:
            req_dict["scene_admission_role"] = str(request.scene_admission_role)
        if request.scene_admission_lease_max_age_ns is not None:
            req_dict["scene_admission_lease_max_age_ns"] = int(
                request.scene_admission_lease_max_age_ns
            )
        if request.scene_admission_target_drift_tolerance_m is not None:
            req_dict["scene_admission_target_drift_tolerance_m"] = float(
                request.scene_admission_target_drift_tolerance_m
            )
        if request.scene_admission_target_angular_tolerance_rad is not None:
            req_dict["scene_admission_target_angular_tolerance_rad"] = float(
                request.scene_admission_target_angular_tolerance_rad
            )
        if request.scene_admission_obstacle_drift_tolerance_m is not None:
            req_dict["scene_admission_obstacle_drift_tolerance_m"] = float(
                request.scene_admission_obstacle_drift_tolerance_m
            )
        if request.scene_admission_obstacle_angular_tolerance_rad is not None:
            req_dict["scene_admission_obstacle_angular_tolerance_rad"] = float(
                request.scene_admission_obstacle_angular_tolerance_rad
            )
        if request.scene_admission_allow_runtime_dynamic_obstacle:
            req_dict["scene_admission_allow_runtime_dynamic_obstacle"] = True

        try:
            result = self._bundle.scene_edit(req_dict)
        except Exception as exc:
            return SceneEditResult(success=False, error=str(exc))
        return SceneEditResult(
            success=result.get("success", False),
            error=result.get("error"),
            update_status=result.get("update_status"),
            collision_world_revision_changed=bool(
                result.get("collision_world_revision_changed", False)
            ),
            collision_world_freshness_changed=bool(
                result.get("collision_world_freshness_changed", False)
            ),
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

    def collision_check_positions(self, positions_rad: list[float]) -> dict[str, Any]:
        """Check backend collisions for active-group joint positions."""
        return dict(self._bundle.collision_check_positions(list(positions_rad)))

    def collision_compare_positions(
        self,
        positions_rad: list[float],
        *,
        waypoint_index: int | None = None,
    ) -> dict[str, Any]:
        """Compare legacy RoboPlan and ManyForge collision engines."""
        return dict(
            self._bundle.collision_compare_positions(
                list(positions_rad),
                waypoint_index,
            )
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

    def get_link_poses(
        self, target_frames: list[str], base_frame: str
    ) -> Dict[str, Dict[str, Any]]:
        raw = self._bundle.get_link_poses(list(target_frames), base_frame)
        poses: Dict[str, Dict[str, Any]] = {}
        for link_name, pose in dict(raw).items():
            poses[str(link_name)] = {
                "position_m": list(pose["position_m"]),
                "orientation_xyzw": list(pose["orientation_xyzw"]),
            }
        return poses

    def compute_target_link_poses(
        self, request: Dict[str, Any]
    ) -> Dict[str, Any]:
        """Compute link poses at a hypothetical robot configuration.

        Used by the Composer Move-To / validated Robot Control candidate
        "ghost robot" preview (no-motion authoring).
        Request shape:
            {
              "kind": "cartesian" | "joint",
              "base_frame": str,
              "link_names": [link_name, ...],
              # for joint:
              "joint_names": [...], "joint_positions_rad": [...],
              # for cartesian:
              "tcp_frame": str,
              "tcp_position_m": [x, y, z],
              "tcp_orientation_xyzw": [x, y, z, w],
              # for cartesian, optional seed:
              "joint_names": [...], "joint_positions_rad": [...],
            }
        Returns:
            {"reachable": True, "joint_names": [...],
             "joint_positions_rad": [...], "link_poses": {link: pose}}
          or:
            {"reachable": False, "reason": "ik_failed",
             "tcp_pose": {position_m, orientation_xyzw}}
        """
        raw = self._bundle.compute_target_link_poses(request)
        out: Dict[str, Any] = {"reachable": bool(raw.get("reachable", False))}
        if not out["reachable"]:
            out["reason"] = str(raw.get("reason", "ik_failed"))
            tcp = raw.get("tcp_pose")
            if tcp is not None:
                out["tcp_pose"] = {
                    "position_m": list(tcp["position_m"]),
                    "orientation_xyzw": list(tcp["orientation_xyzw"]),
                }
            return out
        out["joint_names"] = list(raw.get("joint_names", []))
        out["joint_positions_rad"] = [float(v) for v in raw.get("joint_positions_rad", [])]
        link_poses: Dict[str, Dict[str, Any]] = {}
        for link_name, pose in (raw.get("link_poses") or {}).items():
            link_poses[str(link_name)] = {
                "position_m": list(pose["position_m"]),
                "orientation_xyzw": list(pose["orientation_xyzw"]),
            }
        out["link_poses"] = link_poses
        return out

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
