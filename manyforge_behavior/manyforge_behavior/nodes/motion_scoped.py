"""Motion-scoped interrupt behavior-tree nodes.

`MoveUntilSignal` and `ProbeSearchMove` submit scoped execute requests
to the kernel (spec/260 §2.6 + spec/210 §3.10), wait for the terminal
`MotionExecutionOutcome` (spec/210 §3.11), and map the outcome's
`terminal_cause` onto py_trees `Status`. They never poll signals,
never call backend stop, and never arbitrate competing sources — the
kernel owns all of that.

Outcome -> tree-status mapping (spec/260 §B.6):
  - `signal_trip`       → SUCCESS (the desired event happened)
  - `no_signal_trip`    → FAILURE when `not_found_is_failure=True`
                          (default) and SUCCESS otherwise. The default
                          matches `ProbeSearchMove`'s "the probe MUST
                          have contacted" contract; `MoveUntilSignal`
                          deployments that want non-contact to be
                          ordinary completion can opt-out.
  - `completed_normally`→ FAILURE — the scope was dropped before the
                          kernel could arm it (mostly a misconfiguration;
                          surfaces the issue rather than silently
                          succeeding).
  - `stopped`           -> generation-scoped runtime pause
  - `failed`            -> generation-scoped runtime pause with structured cause
  - `interrupted_fault` -> generation-scoped runtime pause

Both nodes write the terminal outcome to a blackboard key
(`scope_outcome_key`, default `motion_scope_outcome`) so downstream
nodes can branch on details (executed stop mode, stop snapshot, etc.)
without re-querying the kernel.

Motion-field parity: these nodes mirror
`MoveManipulatorAction` for velocity/acceleration scale, TCP linear
cap, joint/translation/rotation tolerances, planning/execution
timeouts, cache mode, blackboard pose-goal/relative-move resolution,
RPY→XYZW normalization, pose-source apply callbacks, and the
`stop_requested` blackboard block. Without parity, authored programs
could not control these per-move on scoped nodes and would silently
fall back to kernel defaults.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Optional

import py_trees

from manyforge_behavior.action_span import ActionSpanMixin, read_program_run_id
from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    KernelClient,
    MotionExecutionOutcome,
    MotionInterruptScope,
    MotionInterruptStopProfile,
)
from manyforge_behavior.motion_runtime import (
    MotionExecutionCoordinator,
)
from manyforge_behavior.nodes.motion import (
    _normalize_pose_dict,
    _relative_move_object_ids,
)
from manyforge_behavior.parameters import resolve_blackboard_value
from manyforge_behavior.runtime_pause import (
    RuntimePauseMixin,
    RuntimePauseRegistry,
    RuntimeRecoveryBinding,
)


class _MotionScopedBase(
    ActionSpanMixin,
    RuntimePauseMixin,
    py_trees.behaviour.Behaviour,
):
    """Shared lifecycle for motion-scoped trip nodes. Subclasses
    populate `_node_category` and `_not_found_default`; the rest of
    the execute/await/outcome loop is identical."""

    # Subclasses override.
    _node_category: str = "generic"
    _catalog_node_id: str = "move_until_signal"
    _not_found_default: bool = False
    _allow_natural_completion_default: bool = False
    # Subclasses may override to bias the default motion_type to
    # "pose_goal" (precision contact) vs "joint_goal" (generic).
    _default_motion_type: str = "joint_goal"

    def __init__(self, name: str, **kwargs: Any) -> None:
        runtime_pause_registry = kwargs.pop("runtime_pause_registry", None)
        super().__init__(name=name)
        self._configure_runtime_pause(
            registry=(
                runtime_pause_registry
                if isinstance(runtime_pause_registry, RuntimePauseRegistry)
                else None
            ),
            node_id=self._catalog_node_id,
            domain="motion",
        )
        self._params = kwargs
        self._execution_id: int | None = None
        self._motion_coordinator: MotionExecutionCoordinator | None = None
        self._terminal_outcome: Optional[MotionExecutionOutcome] = None
        self._error_message: Optional[str] = None
        self._move_id = str(kwargs.get("move_id", ""))
        self._scope_id = str(kwargs.get("scope_id", f"{self._move_id}_scope"))
        self._policy_id = str(kwargs.get("policy_id", ""))
        self._activation_mode = str(
            kwargs.get("activation_mode", "require_inactive_at_arm")
        )
        self._stop_mode = str(kwargs.get("stop_mode", "spring_back_hold"))
        self._stop_profile_id = str(
            kwargs.get("stop_profile_id", f"{self._scope_id}_profile")
        )
        self._stop_max_time_s = kwargs.get("stop_max_time_s")
        self._stop_deceleration_time_s = kwargs.get("stop_deceleration_time_s")
        raw_deceleration_limits = kwargs.get(
            "per_joint_max_deceleration_rad_s2",
            kwargs.get("stop_per_joint_max_deceleration_rad_s2", {}),
        )
        if isinstance(raw_deceleration_limits, Mapping):
            self._per_joint_max_deceleration_rad_s2 = {
                str(joint): float(limit)
                for joint, limit in raw_deceleration_limits.items()
            }
        else:
            self._per_joint_max_deceleration_rad_s2 = {}
        self._allowed_fallback_modes = list(
            kwargs.get("allowed_fallback_modes", [])
        )
        self._allow_natural_completion_on_short_path = bool(
            kwargs.get(
                "allow_natural_completion_on_short_path",
                self._allow_natural_completion_default,
            )
        )
        self._not_found_is_failure = bool(
            kwargs.get("not_found_is_failure", self._not_found_default)
        )
        self._result_binding_key = kwargs.get("result_binding_key")
        self._scope_outcome_key = str(
            kwargs.get("scope_outcome_key", "motion_scope_outcome")
        )
        # Motion-field parity with MoveManipulatorAction. Without these, scoped nodes
        # would silently use kernel defaults for tolerances /
        # acceleration / timeouts / cache mode / TCP cap, and authored
        # programs could not control them per-move.
        self._velocity_scale = float(kwargs.get("velocity_scale", 1.0))
        self._acceleration_scale = float(
            kwargs.get("acceleration_scale", 1.0)
        )
        self._max_tcp_linear_speed_mps = kwargs.get(
            "max_tcp_linear_speed_mps"
        )
        self._joint_position_tol_rad = float(
            kwargs.get("joint_position_tol_rad", 0.05)
        )
        self._translation_tol_m = float(
            kwargs.get("translation_tol_m", 0.005)
        )
        self._rotation_tol_rad = float(kwargs.get("rotation_tol_rad", 0.05))
        self._planning_timeout_s = float(
            kwargs.get("planning_timeout_s", 2.0)
        )
        self._execution_timeout_s = float(
            kwargs.get("execution_timeout_s", 10.0)
        )
        self._cache_mode = str(kwargs.get("cache_mode", "default"))
        self._request_error_recoverable = False

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="motion_coordinator", access=py_trees.common.Access.READ
        )
        # Parity with MoveManipulatorAction: same operator-facing
        # blackboard surface (stop_requested gate, runtime velocity
        # slider, global TCP cap, pose-source callbacks).
        for key in (
            "stop_requested",
            "velocity_scale",
            "global_max_tcp_linear_speed_mps",
            "pose_source_apply_callback",
            "pose_source_between_nodes_callback",
            "program_run_id",
        ):
            self.blackboard.register_key(
                key=key, access=py_trees.common.Access.READ
            )
        self.blackboard.register_key(
            key=self._scope_outcome_key,
            access=py_trees.common.Access.WRITE,
        )

    def initialise(self) -> None:
        self._reset_action_span()
        self._execution_id = None
        self._terminal_outcome = None
        self._error_message = None
        self._request_error_recoverable = False

    def _read_optional_blackboard_value(self, key: str) -> Any | None:
        try:
            return self.blackboard.get(key)
        except (KeyError, AttributeError):
            return None

    def _build_scope(self) -> MotionInterruptScope:
        profile = MotionInterruptStopProfile(
            profile_id=self._stop_profile_id,
            stop_mode=self._stop_mode,
            deceleration_time_s=self._stop_deceleration_time_s,
            max_stop_time_s=self._stop_max_time_s,
            per_joint_max_deceleration_rad_s2=dict(
                self._per_joint_max_deceleration_rad_s2
            ),
            allowed_fallback_modes=list(self._allowed_fallback_modes),
            allow_natural_completion_on_short_path=(
                self._stop_mode == "soft_on_path"
                and self._allow_natural_completion_on_short_path
            ),
        )
        return MotionInterruptScope(
            scope_id=self._scope_id,
            policy_id=self._policy_id,
            activation_mode=self._activation_mode,
            result_binding_key=self._result_binding_key,
            stop_profile=profile,
            node_category=self._node_category,
        )

    def _resolve_motion_inputs(
        self,
    ) -> tuple[dict[str, Any] | None, dict[str, Any] | None] | str:
        """Resolve pose_goal and relative_move from authored params or
        blackboard references, then normalize orientations (RPY→XYZW).
        Returns the resolved tuple, or an error string if resolution
        or validation fails. Mirrors `MoveManipulatorAction.update`
        lines 213-276."""
        try:
            pose_goal = resolve_blackboard_value(
                self.blackboard, self._params.get("pose_goal")
            )
            relative_move = resolve_blackboard_value(
                self.blackboard, self._params.get("relative_move")
            )
        except (AttributeError, KeyError) as exc:
            return str(exc)

        if pose_goal is not None and not isinstance(pose_goal, dict):
            return "pose_goal blackboard reference must resolve to a mapping"
        pose_goal = _normalize_pose_dict(pose_goal)

        if relative_move:
            if not isinstance(relative_move, dict):
                return (
                    "relative_move blackboard reference must resolve to a "
                    "mapping"
                )
            relative_move = dict(relative_move)
            target_reference = relative_move.get("target_reference")
            if target_reference:
                if not isinstance(target_reference, dict):
                    return (
                        "relative_move.target_reference must be a mapping"
                    )
                target_reference = dict(target_reference)
                if target_reference.get("type") == "pose":
                    pose_inner = target_reference.get("pose")
                    if pose_inner is not None and not isinstance(
                        pose_inner, dict
                    ):
                        return (
                            "relative_move.target_reference.pose must "
                            "resolve to a mapping"
                        )
                    target_reference["pose"] = _normalize_pose_dict(
                        pose_inner
                    )
                relative_move["target_reference"] = target_reference
            offset_reference = relative_move.get("offset_reference")
            if offset_reference:
                if not isinstance(offset_reference, dict):
                    return (
                        "relative_move.offset_reference must be a mapping"
                    )
                offset_reference = dict(offset_reference)
                if offset_reference.get("type") == "pose":
                    pose_inner = offset_reference.get("pose")
                    if pose_inner is not None and not isinstance(
                        pose_inner, dict
                    ):
                        return (
                            "relative_move.offset_reference.pose must "
                            "resolve to a mapping"
                        )
                    offset_reference["pose"] = _normalize_pose_dict(
                        pose_inner
                    )
                relative_move["offset_reference"] = offset_reference
            if relative_move.get("offset") is not None and not isinstance(
                relative_move.get("offset"), dict
            ):
                return "relative_move.offset must be a mapping"
            relative_move["offset"] = _normalize_pose_dict(
                relative_move.get("offset")
            )

        return pose_goal, relative_move

    def _apply_pose_source_callbacks(
        self, relative_move: dict[str, Any] | None
    ) -> str | None:
        """Invoke pose-source apply callbacks before dispatch.

        The before-planning callback intentionally refreshes all deployment
        `before_planning` bindings; planning collision-checks the whole scene,
        not just objects referenced by this node. The between-nodes callback
        remains scoped to relative-move references.
        """
        referenced_object_ids = _relative_move_object_ids(relative_move)
        callbacks = [
            ("pose_source_apply_callback", "before-planning", []),
        ]
        if referenced_object_ids:
            callbacks.append(
                (
                    "pose_source_between_nodes_callback",
                    "between-nodes",
                    referenced_object_ids,
                )
            )
        for blackboard_key, label, object_ids in callbacks:
            apply_callback = self._read_optional_blackboard_value(
                blackboard_key
            )
            if not callable(apply_callback):
                continue
            try:
                apply_result = apply_callback(
                    object_ids=object_ids,
                    reason=f"motion_node:{self.name}",
                    action_span=self._current_action_span(self.blackboard),
                )
            except Exception as exc:  # noqa: BLE001 — callback boundary
                return f"{label} pose-source apply raised: {exc}"
            if not apply_result.get("success", False):
                return str(
                    apply_result.get("error")
                    or f"{label} pose-source apply failed"
                )
        return None

    def _build_request(self) -> ExecuteRequest | str:
        """Build the ExecuteRequest with full motion-field parity to
        MoveManipulatorAction. Returns the request, or an error string
        when pose/relative-move resolution or pose-source apply fails.
        Subclasses override `_default_motion_type` to bias the default
        but do NOT override this method (the assembly is identical)."""
        self._request_error_recoverable = False
        resolved = self._resolve_motion_inputs()
        if isinstance(resolved, str):
            return resolved
        pose_goal, relative_move = resolved
        cb_error = self._apply_pose_source_callbacks(relative_move)
        if cb_error is not None:
            self._request_error_recoverable = True
            return cb_error

        request = ExecuteRequest(
            robot_id=str(self._params.get("robot_id", "")),
            group_id=str(self._params.get("group_id", "")),
            move_id=self._move_id,
            base_frame=str(self._params.get("base_frame", "")),
            tcp_frame=str(self._params.get("tcp_frame", "")),
            motion_type=str(
                self._params.get("motion_type", self._default_motion_type)
            ),
            joint_goal_names=list(self._params.get("joint_goal_names", [])),
            joint_goal_positions_rad=list(
                self._params.get("joint_goal_positions_rad", [])
            ),
            pose_goal=pose_goal,
            relative_move=relative_move,
            velocity_scale=self._velocity_scale,
            acceleration_scale=self._acceleration_scale,
            max_tcp_linear_speed_mps=(
                None
                if self._max_tcp_linear_speed_mps is None
                else float(self._max_tcp_linear_speed_mps)
            ),
            joint_position_tol_rad=self._joint_position_tol_rad,
            translation_tol_m=self._translation_tol_m,
            rotation_tol_rad=self._rotation_tol_rad,
            planning_timeout_s=self._planning_timeout_s,
            execution_timeout_s=self._execution_timeout_s,
            cache_mode=self._cache_mode,
            program_run_id=read_program_run_id(self.blackboard),
            action_span=self._current_action_span(self.blackboard),
            contact_policy_id=self._params.get("contact_policy_id") or None,
        )

        global_cap = self._read_optional_blackboard_value(
            "global_max_tcp_linear_speed_mps"
        )
        if global_cap and global_cap > 0:
            if (
                request.max_tcp_linear_speed_mps
                and request.max_tcp_linear_speed_mps > 0
            ):
                request.max_tcp_linear_speed_mps = min(
                    request.max_tcp_linear_speed_mps, global_cap
                )
            else:
                request.max_tcp_linear_speed_mps = global_cap

        return request

    def _start(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        if self._motion_coordinator is None:
            coordinator = self._read_optional_blackboard_value(
                "motion_coordinator"
            )
            self._motion_coordinator = (
                coordinator
                if isinstance(coordinator, MotionExecutionCoordinator)
                and coordinator.kernel is kernel
                else MotionExecutionCoordinator(kernel)
            )
        request_or_error = self._build_request()
        if isinstance(request_or_error, str):
            self._error_message = request_or_error
            self.feedback_message = request_or_error
            self._set_outcome(None)
            if self._request_error_recoverable:
                return self._pause_runtime(
                    reason_code="pose_source_apply_failed",
                    message=self._error_message,
                )
            return py_trees.common.Status.FAILURE
        request = request_or_error
        # Force the scope onto every motion-scoped trip.
        request.motion_interrupt_scopes = [self._build_scope()]
        result = self._motion_coordinator.execute(request)
        if not result.ok:
            self._error_message = result.error or "kernel rejected execute"
            self._set_outcome(None)
            return self._pause_runtime(
                reason_code="motion_execute_rejected",
                message=self._error_message,
            )
        self._execution_id = result.execution_id
        return py_trees.common.Status.RUNNING

    def update(self) -> py_trees.common.Status:
        # Parity with MoveManipulatorAction: an operator-driven stop
        # request must block the start of a new scoped move (and hold
        # RUNNING for in-flight moves until cycle manager / terminate
        # cancels them). Without this, Reset/Stop wouldn't prevent a
        # queued scoped trip from dispatching on the next tick.
        stop_requested = self._read_optional_blackboard_value(
            "stop_requested"
        )
        if stop_requested:
            return py_trees.common.Status.RUNNING

        if self._consume_runtime_resume() is not None:
            self._execution_id = None
            self._terminal_outcome = None
            self._error_message = None
            self.feedback_message = ""
        elif self._runtime_pause_active():
            return py_trees.common.Status.RUNNING

        if self._execution_id is None:
            return self._start()
        assert self._motion_coordinator is not None
        update = self._motion_coordinator.update(self._execution_id)
        if update.state == "running":
            return py_trees.common.Status.RUNNING
        # Terminal: query the outcome and map to tree status.
        outcome: Optional[MotionExecutionOutcome] = None
        try:
            outcome = self._motion_coordinator.kernel.get_execution_outcome(
                self._execution_id
            )
        except Exception:  # noqa: BLE001 — outcome surface is optional
            outcome = None
        self._set_outcome(outcome)
        if update.state == "failed":
            self._error_message = update.error or "motion failed"
            return self._pause_runtime(
                reason_code="motion_scoped_execution_failed",
                message=self._error_message,
                details={"execution_id": self._execution_id},
            )
        # update.state == "succeeded": map outcome.terminal_cause.
        # Fail closed when the outcome surface is absent or the record
        # is missing the execution id (spec/210 §3.11: every terminal
        # execution MUST emit exactly one outcome record). A scoped
        # move that completes without an outcome means either the
        # kernel client doesn't implement the surface or the kernel
        # dropped the record. Either way, treating "no outcome" as
        # SUCCESS would silently certify a precision contact never
        # occurred. Honor the spec contract by failing closed.
        if outcome is None or outcome.execution_id != self._execution_id:
            return self._fail_terminal(
                "scoped move reached kernel-COMPLETED but the terminal "
                "outcome surface is unavailable or returned the wrong "
                "execution id; cannot certify the trip happened "
                f"(expected execution_id={self._execution_id}, "
                f"got={outcome.execution_id if outcome else 'None'})"
            )
        return self._status_from_terminal_cause(outcome)

    def runtime_recovery_binding(self) -> RuntimeRecoveryBinding | None:
        if self._execution_id is None or self._runtime_pause_fault is not None:
            return None
        return RuntimeRecoveryBinding(
            node_name=self.name,
            node_id=self._catalog_node_id,
            domain="motion",
            execution_id=self._execution_id,
        )

    def accept_runtime_recovery(
        self,
        *,
        recovery_request_id: str,
        expected_execution_id: int,
    ) -> bool:
        _ = recovery_request_id
        binding = self.runtime_recovery_binding()
        if binding is None or binding.execution_id != expected_execution_id:
            return False
        self._execution_id = None
        self._terminal_outcome = None
        self._error_message = None
        self.feedback_message = ""
        return True

    def _set_outcome(self, outcome: Optional[MotionExecutionOutcome]) -> None:
        self._terminal_outcome = outcome
        try:
            self.blackboard.set(self._scope_outcome_key, outcome)
        except (KeyError, AttributeError):
            pass

    def terminate(self, new_status: py_trees.common.Status) -> None:
        """Cancel any in-flight scoped kernel goal when py_trees halts
        this node. Mirrors `MoveManipulatorAction.terminate` (motion.py).
        Without this, a Reset / selector preemption / tree rebuild
        could leave a scoped motion running on the controller with no
        BT owner — orphaned motion is the worst kind of safety hazard
        for search/contact moves. Best-effort: swallows exceptions
        because the kernel may already be torn down."""
        self._clear_runtime_pause()
        if self._execution_id is None:
            return
        try:
            kernel = self.blackboard.kernel
        except (KeyError, AttributeError):
            return
        try:
            status = kernel.get_status()
        except Exception:  # noqa: BLE001 — kernel may already be torn down
            return
        if status.execution_id != self._execution_id:
            return
        # Importing here avoids a circular import at module load time.
        from manyforge_behavior.kernel_client import ExecutionState
        stoppable_states = {
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.EXECUTING,
            ExecutionState.INTERRUPTED,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }
        if status.state in stoppable_states:
            try:
                kernel.stop()
            except Exception:  # noqa: BLE001 — best-effort cancel
                pass

    def _status_from_terminal_cause(
        self, outcome: MotionExecutionOutcome
    ) -> py_trees.common.Status:
        # Strict no-signal and malformed-outcome cases remain intentional BT
        # failures. Runtime stop/fault causes use the generation-scoped pause
        # registry so a Parallel does not terminate accidentally.
        cause = outcome.terminal_cause
        if cause == "signal_trip":
            return py_trees.common.Status.SUCCESS
        if cause == "no_signal_trip":
            if not self._not_found_is_failure:
                return py_trees.common.Status.SUCCESS
            return self._fail_terminal(
                "no_signal_trip: probe did not contact within the planned "
                "search envelope (move reached its endpoint without "
                "triggering the configured signal)"
            )
        if cause == "completed_normally":
            return self._fail_terminal(
                "scope dropped before arming (completed_normally on a "
                "scoped move is a configuration error)"
            )
        if cause == "stopped":
            self._error_message = "operator stop reached kernel before trip"
            return self._pause_runtime(
                reason_code="motion_scoped_stopped",
                message=self._error_message,
            )
        if cause == "interrupted_fault":
            self._error_message = "fault session policy preempted the move"
            return self._pause_runtime(
                reason_code="motion_scoped_interrupted_fault",
                message=self._error_message,
            )
        # `failed` and anything unexpected.
        self._error_message = (
            outcome.error_message
            or outcome.error_reason
            or outcome.error_code
            or f"motion failed: terminal_cause={cause}"
        )
        return self._pause_runtime(
            reason_code=(outcome.error_code or "motion_scoped_failed"),
            message=self._error_message,
            details={
                "terminal_cause": cause,
                "error_reason": outcome.error_reason,
            },
        )

    def _fail_terminal(self, message: str) -> py_trees.common.Status:
        """Return an intentional BT failure with operator-visible feedback."""
        self._error_message = message
        self.feedback_message = message
        return py_trees.common.Status.FAILURE

    def _pause_runtime(
        self,
        *,
        reason_code: str,
        message: str,
        details: Optional[dict[str, Any]] = None,
    ) -> py_trees.common.Status:
        self.feedback_message = message
        self._raise_runtime_pause(
            reason_code=reason_code,
            message=message,
            details=details,
        )
        return py_trees.common.Status.RUNNING


class MoveUntilSignal(_MotionScopedBase):
    """Generic motion-scoped trip node. Maps `no_signal_trip` to
    SUCCESS by default so deployments where reaching the planned
    endpoint without a trip is a valid completion are well served.
    Pass `not_found_is_failure=True` to enforce contact semantics."""

    _node_category = "generic"
    _catalog_node_id = "move_until_signal"
    _not_found_default = False
    _allow_natural_completion_default = True
    _default_motion_type = "joint_goal"


class ProbeSearchMove(_MotionScopedBase):
    """Precision-contact motion-scoped trip node. `no_signal_trip` is
    a failure by default because the probe MUST have contacted the
    surface to certify a stop position. Activation mode defaults to
    `require_inactive_at_arm` (spec/260 §2.6) to avoid arming on a
    pre-existing contact."""

    _node_category = "precision_contact"
    _catalog_node_id = "probe_search_move"
    _not_found_default = True
    _allow_natural_completion_default = False
    _default_motion_type = "pose_goal"

    def __init__(self, name: str, **kwargs: Any) -> None:
        # Default activation mode for precision contact: require
        # inactive baseline. Explicit overrides win.
        kwargs.setdefault("activation_mode", "require_inactive_at_arm")
        # Precision-contact node category defaults to empty fallback
        # set (spec/210 §3.11 conjunctive rule). Reject any caller
        # passing allowed_fallback_modes; it would be silently
        # dropped at arm time and the operator would be misled.
        if kwargs.get("allowed_fallback_modes"):
            raise ValueError(
                "ProbeSearchMove disallows allowed_fallback_modes; the "
                "precision-contact node category cannot tolerate fallback "
                "(spec/210 §3.11). Use MoveUntilSignal if a fallback is "
                "acceptable for the deployment."
            )
        if kwargs.get("allow_natural_completion_on_short_path"):
            raise ValueError(
                "ProbeSearchMove disallows natural completion on short path; "
                "precision-contact search must prove a controlled stop on trip."
            )
        super().__init__(name=name, **kwargs)
