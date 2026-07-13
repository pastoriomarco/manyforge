"""Motion behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import math
from typing import Any

import py_trees

from manyforge_behavior.action_span import (
    ActionSpanMixin,
    read_program_run_id,
)
from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    KernelClient,
)
from manyforge_behavior.motion_runtime import MotionExecutionCoordinator
from manyforge_behavior.parameters import resolve_blackboard_value
from manyforge_behavior.runtime_pause import (
    RuntimePauseMixin,
    RuntimePauseRegistry,
    RuntimeRecoveryBinding,
)


# Canonical conversion is defined once in manyforge_behavior.geometry; re-exported
# here under the historical private name for existing in-module callers.
from manyforge_behavior.geometry import rpy_deg_to_xyzw as _rpy_deg_to_xyzw


def _normalize_pose_dict(pose: dict[str, Any] | None) -> dict[str, Any] | None:
    """Return a pose dict with orientation normalized to orientation_xyzw."""
    if pose is None:
        return None
    if "orientation_rpy_deg" in pose and "orientation_xyzw" not in pose:
        normalized = dict(pose)
        normalized["orientation_xyzw"] = _rpy_deg_to_xyzw(
            normalized.pop("orientation_rpy_deg")
        )
        return normalized
    return pose


def _relative_move_object_ids(relative_move: dict[str, Any] | None) -> list[str]:
    """Return scene object IDs referenced by a relative move request."""
    if not isinstance(relative_move, dict):
        return []
    object_ids: list[str] = []
    for key in ("target_reference", "offset_reference"):
        reference = relative_move.get(key)
        if not isinstance(reference, dict):
            continue
        if reference.get("type") != "object":
            continue
        object_id = reference.get("id")
        if isinstance(object_id, str) and object_id and object_id not in object_ids:
            object_ids.append(object_id)
    return object_ids


class MoveManipulatorAction(
    ActionSpanMixin,
    RuntimePauseMixin,
    py_trees.behaviour.Behaviour,
):
    """Execute a motion request through the kernel (plan + validate + execute)."""

    def __init__(self, name: str, **kwargs: Any):
        runtime_pause_registry = kwargs.pop("runtime_pause_registry", None)
        super().__init__(name=name)
        self._configure_runtime_pause(
            registry=(
                runtime_pause_registry
                if isinstance(runtime_pause_registry, RuntimePauseRegistry)
                else None
            ),
            node_id="move_manipulator_action",
            domain="motion",
        )
        self._params = kwargs
        self._started = False
        self._execution_id: int | None = None
        self._motion_coordinator: MotionExecutionCoordinator | None = None
        self._move_id = kwargs.get("move_id", "")
        self._base_velocity_scale = float(kwargs.get("velocity_scale", 1.0))
        self._planning_qualification_action_span = None
        self._planning_qualification_started = False
        self._planning_qualification_terminal = False

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="motion_coordinator", access=py_trees.common.Access.READ
        )
        for key in (
            "stop_requested",
            "velocity_scale",
            "global_max_tcp_linear_speed_mps",
            "pose_source_apply_callback",
            "pose_source_between_nodes_callback",
            "program_run_id",
        ):
            self.blackboard.register_key(key=key, access=py_trees.common.Access.READ)

    def _read_program_run_id(self):
        return read_program_run_id(self.blackboard)

    def initialise(self) -> None:
        self._reset_action_span()
        self._started = False
        self._execution_id = None
        self._motion_coordinator = None
        self._base_velocity_scale = float(self._params.get("velocity_scale", 1.0))
        self._planning_qualification_action_span = None
        self._planning_qualification_started = False
        self._planning_qualification_terminal = False

    def _planning_qualification_span(self):
        if self._planning_qualification_action_span is None:
            self._planning_qualification_action_span = self._current_action_span(
                self.blackboard,
                parent_node_attempt_id=self._params.get("parent_node_attempt_id")
                or None,
            )
        return self._planning_qualification_action_span

    def _record_planning_qualification_boundary(
        self, kernel: KernelClient, phase: str
    ) -> None:
        if phase == "started":
            if self._planning_qualification_started:
                return
            self._planning_qualification_started = True
        else:
            if (
                not self._planning_qualification_started
                or self._planning_qualification_terminal
            ):
                return
            self._planning_qualification_terminal = True
        try:
            kernel.record_planning_qualification_behavior_boundary(
                action_span=self._planning_qualification_span(),
                move_id=str(self._params.get("move_id", "")),
                phase=phase,
            )
        except Exception:  # noqa: BLE001 - evidence must not own motion behavior
            return

    def _read_optional_blackboard_value(self, key: str) -> Any | None:
        try:
            return self.blackboard.get(key)
        except (KeyError, AttributeError):
            return None

    def _enter_soft_fail(
        self,
        error_message: str,
        *,
        reason_code: str,
        details: dict[str, Any] | None = None,
    ) -> py_trees.common.Status:
        """Capture the failure cause and hold Status.RUNNING until resume/reset.

        The motion node never returns Status.FAILURE on a runtime motion
        error (planning failure, collision, execution timeout). Instead
        it stores the cause, signals the cycle manager via blackboard,
        and holds RUNNING. The cycle manager observes the signal in its
        tick loop, surfaces the error, and holds motion while
        continue-policy branches keep ticking.

        The exact fault generation is resumed only after the server accepts a
        versioned recovery action. Reset destroys the tree externally;
        terminate() cancels any in-flight kernel goal.
        """
        self.feedback_message = error_message
        self._raise_runtime_pause(
            reason_code=reason_code,
            message=error_message,
            details=details,
        )
        return py_trees.common.Status.RUNNING

    def _clear_soft_fail_state(self) -> None:
        """Reset internal + blackboard soft-fail state so the next tick re-issues the goal."""
        self._started = False
        self._execution_id = None
        self.feedback_message = ""

    def runtime_recovery_binding(self) -> RuntimeRecoveryBinding | None:
        if (
            not self._started
            or self._execution_id is None
            or self._runtime_pause_fault is not None
        ):
            return None
        return RuntimeRecoveryBinding(
            node_name=self.name,
            node_id="move_manipulator_action",
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
        self._clear_soft_fail_state()
        return True

    def update(self) -> py_trees.common.Status:
        stop_requested = self._read_optional_blackboard_value("stop_requested")
        if stop_requested:
            return py_trees.common.Status.RUNNING  # block, don't start new motion

        # Only an exact, accepted recovery request can wake this fault
        # generation. Observer reads and generic Run cannot mutate it.
        resume_request_id = self._consume_runtime_resume()
        if resume_request_id is not None:
            self._clear_soft_fail_state()
        elif self._runtime_pause_active():
            return py_trees.common.Status.RUNNING

        # This is the authored per-move planning/execution scale. The viewer runtime
        # velocity slider is a separate live control surface: CycleManager forwards
        # it to the kernel/backend, which retimes in-flight execution without
        # stopping or re-dispatching this behavior node.
        effective_velocity_scale = self._base_velocity_scale

        kernel: KernelClient = self.blackboard.kernel
        if self._motion_coordinator is None:
            coordinator = self._read_optional_blackboard_value("motion_coordinator")
            self._motion_coordinator = (
                coordinator
                if isinstance(coordinator, MotionExecutionCoordinator)
                and coordinator.kernel is kernel
                else MotionExecutionCoordinator(kernel)
            )

        if not self._started:
            self._record_planning_qualification_boundary(kernel, "started")
            try:
                pose_goal = resolve_blackboard_value(
                    self.blackboard, self._params.get("pose_goal")
                )
                relative_move = resolve_blackboard_value(
                    self.blackboard, self._params.get("relative_move")
                )
            except (AttributeError, KeyError) as exc:
                self.feedback_message = str(exc)
                return py_trees.common.Status.FAILURE

            if pose_goal is not None and not isinstance(pose_goal, dict):
                self.feedback_message = "pose_goal blackboard reference must resolve to a mapping"
                return py_trees.common.Status.FAILURE
            pose_goal = _normalize_pose_dict(pose_goal)

            if relative_move:
                if not isinstance(relative_move, dict):
                    self.feedback_message = "relative_move blackboard reference must resolve to a mapping"
                    return py_trees.common.Status.FAILURE
                relative_move = dict(relative_move)
                target_reference = relative_move.get("target_reference")
                if target_reference:
                    if not isinstance(target_reference, dict):
                        self.feedback_message = "relative_move.target_reference must be a mapping"
                        return py_trees.common.Status.FAILURE
                    target_reference = dict(target_reference)
                    if target_reference.get("type") == "pose":
                        if (
                            target_reference.get("pose") is not None
                            and not isinstance(target_reference.get("pose"), dict)
                        ):
                            self.feedback_message = (
                                "relative_move.target_reference.pose must resolve to a mapping"
                            )
                            return py_trees.common.Status.FAILURE
                        target_reference["pose"] = _normalize_pose_dict(target_reference.get("pose"))
                    relative_move["target_reference"] = target_reference
                offset_reference = relative_move.get("offset_reference")
                if offset_reference:
                    if not isinstance(offset_reference, dict):
                        self.feedback_message = "relative_move.offset_reference must be a mapping"
                        return py_trees.common.Status.FAILURE
                    offset_reference = dict(offset_reference)
                    if offset_reference.get("type") == "pose":
                        if (
                            offset_reference.get("pose") is not None
                            and not isinstance(offset_reference.get("pose"), dict)
                        ):
                            self.feedback_message = (
                                "relative_move.offset_reference.pose must resolve to a mapping"
                            )
                            return py_trees.common.Status.FAILURE
                        offset_reference["pose"] = _normalize_pose_dict(offset_reference.get("pose"))
                    relative_move["offset_reference"] = offset_reference
                if relative_move.get("offset") is not None and not isinstance(
                    relative_move.get("offset"), dict
                ):
                    self.feedback_message = "relative_move.offset must be a mapping"
                    return py_trees.common.Status.FAILURE
                relative_move["offset"] = _normalize_pose_dict(
                    relative_move.get("offset")
                )

            referenced_object_ids = _relative_move_object_ids(relative_move)
            callbacks = [
                (
                    "pose_source_apply_callback",
                    "before-planning",
                    [],
                ),
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
                except Exception as exc:  # noqa: BLE001 - callback boundary
                    return self._enter_soft_fail(
                        f"{label} pose-source apply raised: {exc}",
                        reason_code="pose_source_apply_failed",
                        details={"phase": label, "exception_type": type(exc).__name__},
                    )
                if not apply_result.get("success", False):
                    error = apply_result.get("error") or (
                        f"{label} pose-source apply failed"
                    )
                    return self._enter_soft_fail(
                        str(error),
                        reason_code="pose_source_apply_failed",
                        details={"phase": label},
                    )

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
                pose_goal=pose_goal,
                relative_move=relative_move,
                velocity_scale=effective_velocity_scale,
                acceleration_scale=float(
                    self._params.get("acceleration_scale", 1.0)
                ),
                max_tcp_linear_speed_mps=(
                    None
                    if self._params.get("max_tcp_linear_speed_mps") is None
                    else float(self._params.get("max_tcp_linear_speed_mps"))
                ),
                joint_position_tol_rad=float(
                    self._params.get("joint_position_tol_rad", 0.05)
                ),
                translation_tol_m=float(
                    self._params.get("translation_tol_m", 0.005)
                ),
                rotation_tol_rad=float(
                    self._params.get("rotation_tol_rad", 0.05)
                ),
                planning_timeout_s=float(
                    self._params.get("planning_timeout_s", 2.0)
                ),
                execution_timeout_s=float(
                    self._params.get("execution_timeout_s", 10.0)
                ),
                cache_mode=self._params.get("cache_mode", "default"),
                program_run_id=self._read_program_run_id(),
                action_span=self._planning_qualification_span(),
                contact_policy_id=self._params.get("contact_policy_id") or None,
            )

            global_cap = self._read_optional_blackboard_value(
                "global_max_tcp_linear_speed_mps"
            )
            if global_cap and global_cap > 0:
                if request.max_tcp_linear_speed_mps and request.max_tcp_linear_speed_mps > 0:
                    request.max_tcp_linear_speed_mps = min(
                        request.max_tcp_linear_speed_mps, global_cap
                    )
                else:
                    request.max_tcp_linear_speed_mps = global_cap

            result = self._motion_coordinator.execute(request)
            self._started = True
            if not result.ok:
                # Planning rejected at submission. Soft-fail so the operator
                # can adjust the scene/parameters and Run again to retry.
                return self._enter_soft_fail(
                    result.error or "Motion execution rejected",
                    reason_code="motion_execute_rejected",
                )
            self._execution_id = result.execution_id

        if self._execution_id is None:
            self.feedback_message = "Motion execution id was not recorded"
            return py_trees.common.Status.FAILURE
        update = self._motion_coordinator.update(self._execution_id)
        if update.state == "succeeded":
            self._record_planning_qualification_boundary(kernel, "succeeded")
            return py_trees.common.Status.SUCCESS
        if update.state == "failed":
            # Kernel reached FAILED or STOPPED for our owned execution_id
            # (collision detected mid-execute, planning timeout, joint-limit,
            # etc.). Soft-fail with the kernel's actual cause string —
            # which now flows through motion_runtime.update() pulling from
            # ExecutionSnapshot.error. The operator can Run again to retry
            # the same goal or Reset to clear the cycle.
            return self._enter_soft_fail(
                update.error or "Motion execution failed",
                reason_code="motion_execution_failed",
                details={"execution_id": self._execution_id},
            )
        return py_trees.common.Status.RUNNING

    def terminate(self, new_status: py_trees.common.Status) -> None:
        """Cancel any in-flight kernel goal when py_trees halts this node.

        Called when the parent halts the node (e.g., the tree is being
        rebuilt after Reset, Stop, or an owned execution transition).
        Without this, a kernel goal mid-execution would continue running
        even though the BT no longer owns it — orphaned motion, the worst
        kind of safety hazard. We issue kernel.stop() if the current
        kernel state matches our owned execution_id and is in a stoppable
        state. Best-effort: kernel API may not be available in all
        configurations (e.g., during shutdown), so swallow exceptions.
        """
        try:
            kernel = self.blackboard.kernel
        except (KeyError, AttributeError):
            kernel = None
        if kernel is not None:
            phase = (
                "succeeded"
                if new_status == py_trees.common.Status.SUCCESS
                else "failed"
                if new_status == py_trees.common.Status.FAILURE
                else "abandoned"
            )
            self._record_planning_qualification_boundary(kernel, phase)
        self._clear_runtime_pause()
        if not self._started or self._execution_id is None:
            return
        if kernel is None:
            return
        try:
            status = kernel.get_status()
        except Exception:  # noqa: BLE001 — kernel may already be torn down
            return
        if status.execution_id != self._execution_id:
            return
        # Only cancel if the kernel is doing something cancellable for our
        # goal. COMPLETED/IDLE/STOPPED/FAILED are already terminal and
        # need no cancel call.
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


class ResetTrajectories(ActionSpanMixin, py_trees.behaviour.Behaviour):
    """Invalidate all cached trajectories."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )

    def initialise(self) -> None:
        self._reset_action_span()

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        kernel.invalidate_trajectories(
            action_span=self._current_action_span(self.blackboard)
        )
        return py_trees.common.Status.SUCCESS
