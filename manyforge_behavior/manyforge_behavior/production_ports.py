"""Production client ports from bounded Program orchestration to native owners.

This module translates already-validated Program node inputs into the public
headless request vocabulary. It owns no planning, frame resolution, scene
truth, gripper state, or Stop semantics; those remain in the native owners.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from hashlib import sha256
import math
from threading import RLock
from typing import Any, Final

from manyforge_behavior.geometry import normalize_quat, rpy_deg_to_xyzw
from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
)
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)


_ACTIVE_STATES: Final = frozenset(
    {
        ExecutionState.QUEUED.value,
        ExecutionState.RESOLVING.value,
        ExecutionState.PLANNING.value,
        ExecutionState.VALIDATING.value,
        ExecutionState.PREPARED.value,
        ExecutionState.DISPATCHING.value,
        ExecutionState.EXECUTING.value,
        ExecutionState.PAUSED.value,
        ExecutionState.STOPPING.value,
    }
)
_INTERRUPTED_STATES: Final = frozenset(
    {
        ExecutionState.STOPPED.value,
        ExecutionState.INTERRUPTED.value,
        ExecutionState.CANCELLED_PRE_DISPATCH.value,
    }
)
_FAILED_STATES: Final = frozenset(
    {
        ExecutionState.FAILED.value,
        ExecutionState.INTERVENTION_REQUIRED.value,
    }
)
_MOTION_NODE_IDS: Final = frozenset(
    {
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "motion.follow_cartesian_path",
    }
)
_MAX_CONTEXTS: Final = 4_096
_MAX_TASKS: Final = 65_536


class ProductionPortError(RuntimeError):
    """A captured client context or native owner projection is inconsistent."""


def _mapping(value: object, *, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProductionPortError(f"{field} must be a mapping")
    return value


def _text(value: object, *, field: str, maximum_bytes: int = 512) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        raise ProductionPortError(f"{field} must be bounded non-empty text")
    return value


def _number(value: object, *, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProductionPortError(f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ProductionPortError(f"{field} must be finite")
    return result


def _request_identity(prefix: str, *coordinates: str) -> str:
    digest = sha256(b"\x00".join(value.encode("utf-8") for value in coordinates))
    return f"{prefix}:{digest.hexdigest()}"


def _sequence(value: object, *, field: str, length: int | None = None) -> Sequence[Any]:
    if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
        raise ProductionPortError(f"{field} must be a sequence")
    if length is not None and len(value) != length:
        raise ProductionPortError(f"{field} must contain {length} values")
    return value


def _pose3(
    value: object,
    *,
    field: str,
    target_role_id: str,
    base_frame_role_id: str,
) -> dict[str, Any]:
    pose = _mapping(value, field=field)
    frame = pose.get("frame_ref")
    if frame is not None:
        frame_value = _mapping(frame, field=f"{field}.frame_ref")
        if (
            frame_value.get("kind") != "target_frame"
            or frame_value.get("target_role") != target_role_id
            or frame_value.get("frame_role") != base_frame_role_id
        ):
            raise ProductionPortError(
                f"{field}.frame_ref is not the selected target base frame"
            )
    position = _sequence(pose.get("position_m"), field=f"{field}.position_m", length=3)
    orientation = _mapping(pose.get("orientation"), field=f"{field}.orientation")
    if set(orientation) == {"xyzw"}:
        quaternion = normalize_quat(
            _sequence(
                orientation["xyzw"],
                field=f"{field}.orientation.xyzw",
                length=4,
            )
        )
    elif set(orientation) == {"rpy_deg"}:
        quaternion = rpy_deg_to_xyzw(
            _sequence(
                orientation["rpy_deg"],
                field=f"{field}.orientation.rpy_deg",
                length=3,
            )
        )
    else:
        raise ProductionPortError(
            f"{field}.orientation must contain exactly xyzw or rpy_deg"
        )
    return {
        "position_m": {
            "x_m": _number(position[0], field=f"{field}.position_m[0]"),
            "y_m": _number(position[1], field=f"{field}.position_m[1]"),
            "z_m": _number(position[2], field=f"{field}.position_m[2]"),
        },
        "orientation": {
            "x": quaternion[0],
            "y": quaternion[1],
            "z": quaternion[2],
            "w": quaternion[3],
        },
    }


def _reference(
    value: object,
    *,
    field: str,
    target_role_id: str,
    base_frame_role_id: str,
) -> dict[str, Any] | None:
    if value is None:
        return None
    reference = dict(_mapping(value, field=field))
    reference_type = reference.get("type")
    identity_fields = {
        "frame_role": "frame_role_id",
        "component_body_role": "collision_body_role_id",
        "scene_resource": "scene_resource_id",
        "runtime_object": "runtime_object_id",
    }
    if reference_type in identity_fields:
        identity_field = identity_fields[reference_type]
        if set(reference) != {"type", identity_field}:
            raise ProductionPortError(f"{field} has a malformed reference")
        _text(reference[identity_field], field=f"{field}.{identity_field}")
        return reference
    if reference_type == "pose" and set(reference) == {"type", "pose"}:
        return {
            "type": "pose",
            "pose": _pose3(
                reference["pose"],
                field=f"{field}.pose",
                target_role_id=target_role_id,
                base_frame_role_id=base_frame_role_id,
            ),
        }
    raise ProductionPortError(f"{field} has an unsupported reference form")


@dataclass(frozen=True, slots=True)
class _TargetBinding:
    target_role_id: str
    component_role_id: str
    capabilities: Mapping[str, Mapping[str, Any]]


@dataclass(frozen=True, slots=True)
class _BoundContext:
    capture: ProgramRunPortContext
    targets: Mapping[str, _TargetBinding]


@dataclass(slots=True)
class _OperationTask:
    identity: NodeAttemptIdentity
    node_id: str
    operation_id: str
    stop_requested: bool = False


class NativeKernelProgramPort:
    """Captured context plus separate native task and Program-scope clients."""

    def __init__(self, kernel: KernelClient) -> None:
        if not isinstance(kernel, KernelClient):
            raise TypeError("kernel must be one bounded KernelClient")
        self._kernel = kernel
        self._contexts: dict[str, _BoundContext] = {}
        self._tasks: dict[str, _OperationTask] = {}
        self._lock = RLock()
        self.operations = _NativeKernelOperationTaskPort(self)
        self.scope = _NativeKernelProgramScopePort(self)

    @staticmethod
    def _parse_context(context: ProgramRunPortContext) -> _BoundContext:
        binding = context.program_binding
        rows = binding.get("target_role_bindings")
        if not isinstance(rows, (list, tuple)) or not rows:
            raise ProductionPortError(
                "Program Binding has no target-role bindings"
            )
        targets: dict[str, _TargetBinding] = {}
        for index, raw_row in enumerate(rows):
            row = _mapping(raw_row, field=f"target_role_bindings[{index}]")
            target_role_id = _text(
                row.get("target_role_id"),
                field=f"target_role_bindings[{index}].target_role_id",
            )
            component_role_id = _text(
                row.get("component_role_id"),
                field=f"target_role_bindings[{index}].component_role_id",
            )
            raw_capabilities = row.get("capability_bindings")
            if not isinstance(raw_capabilities, (list, tuple)):
                raise ProductionPortError(
                    f"target role {target_role_id!r} has no capability bindings"
                )
            capabilities: dict[str, Mapping[str, Any]] = {}
            for raw_capability in raw_capabilities:
                capability = _mapping(
                    raw_capability,
                    field=f"target role {target_role_id!r} capability",
                )
                capability_id = _text(
                    capability.get("capability_id"), field="capability_id"
                )
                if capability_id in capabilities:
                    raise ProductionPortError(
                        f"target role {target_role_id!r} repeats a capability"
                    )
                capabilities[capability_id] = capability
            if target_role_id in targets:
                raise ProductionPortError("Program Binding repeats a target role")
            targets[target_role_id] = _TargetBinding(
                target_role_id,
                component_role_id,
                capabilities,
            )
        return _BoundContext(context, targets)

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("bind requires one exact Program-run port context")
        parsed = self._parse_context(context)
        with self._lock:
            current = self._contexts.get(context.program_run_id)
            if current is not None:
                if current != parsed:
                    raise ProductionPortError("Program-run identity was rebound")
                return
            if len(self._contexts) >= _MAX_CONTEXTS:
                raise ProductionPortError("Program-run client-context retention is full")
            self._contexts[context.program_run_id] = parsed

    def unbind(self, program_run_id: str) -> None:
        with self._lock:
            if any(
                task.identity.program_run_id == program_run_id
                for task in self._tasks.values()
            ):
                raise ProductionPortError(
                    "cannot unbind a Program run with retained operation tasks"
                )
            self._contexts.pop(program_run_id, None)

    def _context(self, program_run_id: str) -> _BoundContext:
        with self._lock:
            try:
                return self._contexts[program_run_id]
            except KeyError as exc:
                raise ProductionPortError(
                    "Program-run client context is not bound"
                ) from exc

    @staticmethod
    def _target(
        context: _BoundContext,
        target_role_id: str,
        capability_id: str,
    ) -> _TargetBinding:
        try:
            target = context.targets[target_role_id]
        except KeyError as exc:
            raise ProductionPortError(
                f"Program Binding has no target role {target_role_id!r}"
            ) from exc
        if capability_id not in target.capabilities:
            raise ProductionPortError(
                f"target role {target_role_id!r} lacks {capability_id!r}"
            )
        return target

    def _owner_status(self, context: _BoundContext) -> tuple[str, dict[str, Any]]:
        envelope = self._kernel.status()
        status = _mapping(envelope.get("status"), field="execution status")
        owner_epoch = _text(status.get("owner_epoch"), field="owner_epoch")
        if (
            status.get("graph_revision") != context.capture.graph_revision
            or status.get("retained_bundle_id")
            != context.capture.retained_bundle_id
        ):
            raise ProductionPortError(
                "native execution owner and captured Program deployment disagree"
            )
        scene = dict(_mapping(status.get("scene_revision"), field="scene_revision"))
        if (
            not isinstance(scene.get("scene_id"), str)
            or type(scene.get("revision_id")) is not int
            or scene["revision_id"] < 1
            or not isinstance(scene.get("content_hash"), str)
        ):
            raise ProductionPortError("native execution owner has no exact scene revision")
        return owner_epoch, scene

    @staticmethod
    def _motion_spec(
        node_id: str,
        parameters: Mapping[str, Any],
        *,
        target_role_id: str,
        base_frame_role_id: str,
    ) -> dict[str, Any]:
        if node_id == "motion.move_to_joint_target":
            goals = _sequence(parameters.get("goal"), field="goal")
            joint_names: list[str] = []
            positions: list[float] = []
            for index, raw_goal in enumerate(goals):
                goal = _mapping(raw_goal, field=f"goal[{index}]")
                joint_names.append(
                    _text(goal.get("joint_name"), field=f"goal[{index}].joint_name")
                )
                positions.append(
                    _number(
                        goal.get("position_rad"),
                        field=f"goal[{index}].position_rad",
                    )
                )
            return {
                "kind": "joint_goal",
                "joint_names": joint_names,
                "positions_rad": positions,
            }
        if node_id == "motion.move_to_pose":
            return {
                "kind": "pose_goal",
                "goal": _pose3(
                    parameters.get("goal"),
                    field="goal",
                    target_role_id=target_role_id,
                    base_frame_role_id=base_frame_role_id,
                ),
            }
        if node_id == "motion.follow_cartesian_path":
            path = _sequence(parameters.get("path"), field="path")
            return {
                "kind": "cartesian_path",
                "waypoints": [
                    _pose3(
                        waypoint,
                        field=f"path[{index}]",
                        target_role_id=target_role_id,
                        base_frame_role_id=base_frame_role_id,
                    )
                    for index, waypoint in enumerate(path)
                ],
                "max_translation_step_m": parameters["max_translation_step_m"],
                "max_rotation_step_rad": parameters["max_rotation_step_rad"],
                "speed_mode": "bounded",
                "max_linear_speed_mps": parameters["max_linear_speed_mps"],
                "max_angular_speed_rad_s": parameters["max_angular_speed_rad_s"],
                "max_linear_acceleration_mps2": parameters[
                    "max_linear_acceleration_mps2"
                ],
                "max_angular_acceleration_rad_s2": parameters[
                    "max_angular_acceleration_rad_s2"
                ],
                "fallback_policy": parameters["fallback_policy"],
            }
        raise ProductionPortError(f"unsupported Kernel motion node {node_id!r}")

    def _motion_payload(
        self,
        node_id: str,
        parameters: Mapping[str, Any],
        identity: NodeAttemptIdentity,
    ) -> tuple[_BoundContext, _TargetBinding, dict[str, Any]]:
        context = self._context(identity.program_run_id)
        target_role_id = _text(parameters.get("target_role"), field="target_role")
        target = self._target(
            context, target_role_id, "execution.trajectory.execute"
        )
        base_frame_role_id = _text(
            parameters.get("base_frame_role"), field="base_frame_role"
        )
        tcp_frame_role_id = _text(
            parameters.get("tcp_frame_role"), field="tcp_frame_role"
        )
        target_mode = parameters.get("target_mode", "absolute")
        if target_mode not in {"absolute", "relative"}:
            raise ProductionPortError("motion target_mode is unsupported")
        target_reference = _reference(
            parameters.get("target_reference"),
            field="target_reference",
            target_role_id=target_role_id,
            base_frame_role_id=base_frame_role_id,
        )
        offset_reference = _reference(
            parameters.get("offset_reference"),
            field="offset_reference",
            target_role_id=target_role_id,
            base_frame_role_id=base_frame_role_id,
        )
        runtime_object_ids = {
            reference["runtime_object_id"]
            for reference in (target_reference, offset_reference)
            if reference is not None and reference["type"] == "runtime_object"
        }
        if runtime_object_ids:
            raise ProductionPortError(
                "runtime-object motion references require the N7-a5.3b "
                "scene-binding join"
            )
        payload = {
            "kind": "motion",
            "move_id": _request_identity(
                "move", identity.program_run_id, identity.node_attempt_id
            ),
            "base_frame_role_id": base_frame_role_id,
            "tcp_frame_role_id": tcp_frame_role_id,
            "tolerances": {
                "joint_position_tol_rad": parameters["joint_position_tol_rad"],
                "translation_tol_m": parameters["translation_tol_m"],
                "rotation_tol_rad": parameters["rotation_tol_rad"],
            },
            "speed_profile": {
                "velocity_scale": parameters["velocity_scale"],
                "acceleration_scale": parameters["acceleration_scale"],
                "max_tcp_linear_speed_mps": parameters.get(
                    "max_tcp_linear_speed_mps"
                ),
            },
            "timeouts": {
                "planning_timeout_s": parameters["planning_timeout_s"],
                "execution_timeout_s": parameters["execution_timeout_s"],
            },
            "cache_mode": parameters["cache_mode"],
            "planning_profile": parameters["planning_profile"],
            "target_mode": target_mode,
            "target_reference": target_reference,
            "offset_reference": offset_reference,
            "motion": self._motion_spec(
                node_id,
                parameters,
                target_role_id=target_role_id,
                base_frame_role_id=base_frame_role_id,
            ),
            "contact_policy_id": parameters.get("contact_policy_id"),
            "per_joint_dynamics_limits": dict(
                parameters.get("per_joint_dynamics_limits", {})
            ),
            "motion_interrupt_scopes": [],
            "runtime_object_bindings": [],
        }
        return context, target, payload

    def _gripper_payload(
        self,
        parameters: Mapping[str, Any],
        identity: NodeAttemptIdentity,
    ) -> tuple[_BoundContext, _TargetBinding, dict[str, Any]]:
        context = self._context(identity.program_run_id)
        target_role_id = _text(parameters.get("target_role"), field="target_role")
        target = self._target(context, target_role_id, "tool.gripper.command")
        capability = target.capabilities["tool.gripper.command"]
        command_schema = _mapping(
            capability.get("command_schema"), field="gripper command_schema"
        )
        schema_id = _text(
            command_schema.get("command_schema_id"), field="command_schema_id"
        )
        schema_revision = _text(
            command_schema.get("command_schema_revision"),
            field="command_schema_revision",
        )
        position = dict(_mapping(parameters.get("position"), field="position"))
        max_effort_value = parameters.get("max_effort")
        max_effort = (
            None
            if max_effort_value is None
            else dict(_mapping(max_effort_value, field="max_effort"))
        )
        return context, target, {
            "kind": "capability_command",
            "command_schema_id": schema_id,
            "command_schema_revision": schema_revision,
            "command_value": {
                "kind": "gripper_position",
                "position": position,
                "max_effort": max_effort,
            },
            "completion_timeout_s": parameters["completion_timeout_s"],
            "caller_options": [
                {
                    "option_id": "command_duration_s",
                    "value_s": parameters.get("command_duration_s", 0.0),
                }
            ],
        }

    def _start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        if not isinstance(identity, NodeAttemptIdentity):
            raise TypeError("operation start requires one node-attempt identity")
        if not isinstance(inputs, OwnerNodeInputs):
            raise TypeError("operation start requires immutable owner inputs")
        if node_id in _MOTION_NODE_IDS:
            context, target, payload = self._motion_payload(
                node_id, inputs.parameters, identity
            )
            capability_id = "execution.trajectory.execute"
        elif node_id == "gripper.command_position":
            context, target, payload = self._gripper_payload(
                inputs.parameters, identity
            )
            capability_id = "tool.gripper.command"
        else:
            raise ProductionPortError(
                f"Kernel operation port does not implement {node_id!r}"
            )
        owner_epoch, scene_revision = self._owner_status(context)
        request_id = _request_identity(
            "program-node", identity.program_run_id, identity.node_attempt_id
        )
        span = ActionSpan(
            action_source="program_node",
            action_span_id=identity.node_attempt_id,
            program_run_id=identity.program_run_id,
            node_instance_id=identity.node_instance_id,
            node_attempt_id=identity.node_attempt_id,
            parent_node_attempt_id=identity.parent_node_attempt_id,
        )
        accepted = self._kernel.execute(
            {
                "request_context": {
                    "request_id": request_id,
                    "request_owner_epoch": owner_epoch,
                    "source_label": "manyforge_behavior",
                    "correlation_id": identity.program_run_id,
                },
                "program_id": context.capture.program_id,
                "program_revision": context.capture.program_revision,
                "program_binding_id": context.capture.program_binding_id,
                "program_run_id": context.capture.program_run_id,
                "expected_graph_revision": context.capture.graph_revision,
                "expected_retained_bundle_id": context.capture.retained_bundle_id,
                "expected_scene_revision": scene_revision,
                "participants": [
                    {
                        "component_role_id": target.component_role_id,
                        "target_role_id": target.target_role_id,
                        "required_capability_id": capability_id,
                        "action_span": span.as_request(),
                        "payload": payload,
                    }
                ],
                "coordination_policy": "independent",
            }
        )
        accepted_value = _mapping(accepted.get("accepted"), field="acceptance")
        operation_id = _text(
            accepted_value.get("operation_id"), field="accepted.operation_id"
        )
        with self._lock:
            current = self._tasks.get(operation_id)
            task = _OperationTask(identity, node_id, operation_id)
            if current is not None and current != task:
                raise ProductionPortError("native operation identity was reused")
            if current is None:
                if len(self._tasks) >= _MAX_TASKS:
                    raise ProductionPortError("native operation-task retention is full")
                self._tasks[operation_id] = task
        return OwnerTaskSnapshot(
            task_id=operation_id,
            state=OwnerTaskState.ACTIVE,
            metadata={"operation_id": operation_id},
        )

    @staticmethod
    def _terminal_error(operation: Mapping[str, Any]) -> tuple[str, str]:
        participants = operation.get("participants")
        if isinstance(participants, list):
            for participant in participants:
                if not isinstance(participant, Mapping):
                    continue
                error = participant.get("terminal_error")
                if isinstance(error, Mapping):
                    code = error.get("code")
                    message = error.get("message")
                    if isinstance(code, str) and code:
                        return code, message if isinstance(message, str) else ""
        return "operation_failed", "native operation did not succeed"

    def _observation(self, task: _OperationTask) -> tuple[dict[str, Any], OwnerTaskSnapshot]:
        envelope = self._kernel.operation(task.operation_id)
        operation = dict(_mapping(envelope.get("operation"), field="operation"))
        if (
            operation.get("operation_id") != task.operation_id
            or operation.get("program_run_id") != task.identity.program_run_id
        ):
            raise ProductionPortError("native operation observation changed identity")
        state = operation.get("state")
        metadata = {
            "operation_id": task.operation_id,
            "operation_revision": operation.get("operation_revision"),
        }
        if state in _ACTIVE_STATES:
            snapshot = OwnerTaskSnapshot(
                task_id=task.operation_id,
                state=OwnerTaskState.ACTIVE,
                metadata=metadata,
            )
        elif state == ExecutionState.COMPLETED.value:
            snapshot = OwnerTaskSnapshot(
                task_id=task.operation_id,
                state=OwnerTaskState.SUCCEEDED,
                metadata=metadata,
            )
        elif state in _INTERRUPTED_STATES:
            snapshot = OwnerTaskSnapshot(
                task_id=task.operation_id,
                state=OwnerTaskState.INTERRUPTED,
                metadata=metadata,
                reason_code=f"operation_{state}",
                message="native operation was interrupted",
            )
        elif state in _FAILED_STATES:
            reason_code, message = self._terminal_error(operation)
            snapshot = OwnerTaskSnapshot(
                task_id=task.operation_id,
                state=OwnerTaskState.FAILED,
                metadata=metadata,
                reason_code=reason_code,
                message=message,
            )
        else:
            raise ProductionPortError("native operation has an unknown state")
        return operation, snapshot

    def _task(self, task_id: str) -> _OperationTask:
        with self._lock:
            try:
                return self._tasks[task_id]
            except KeyError as exc:
                raise ProductionPortError("native operation task is not retained") from exc

    def _cancel(self, task: _OperationTask, reason: str | None) -> None:
        operation, snapshot = self._observation(task)
        if snapshot.state is not OwnerTaskState.ACTIVE:
            return
        with self._lock:
            if task.stop_requested or operation.get("state") == "stopping":
                return
            task.stop_requested = True
        revision = operation.get("operation_revision")
        stop_generation = operation.get("active_stop_generation")
        if type(revision) is not int or revision < 1:
            raise ProductionPortError("native operation revision is invalid")
        if stop_generation is None:
            stop_generation = 0
        if type(stop_generation) is not int or stop_generation < 0:
            raise ProductionPortError("native Stop generation is invalid")
        try:
            context = self._context(task.identity.program_run_id)
            owner_epoch, _scene = self._owner_status(context)
            self._kernel.stop(
                {
                    "request_context": {
                        "request_id": _request_identity(
                            "program-stop",
                            task.identity.program_run_id,
                            task.operation_id,
                        ),
                        "request_owner_epoch": owner_epoch,
                        "source_label": "manyforge_behavior",
                        "correlation_id": task.identity.program_run_id,
                    },
                    "operations": [
                        {
                            "operation_id": task.operation_id,
                            "expected_operation_revision": revision,
                            "expected_stop_generation": stop_generation,
                        }
                    ],
                    "reason": reason,
                }
            )
        except Exception:
            with self._lock:
                task.stop_requested = False
            raise

    def _cancel_task(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        task = self._task(task_id)
        if task.identity != identity:
            raise ProductionPortError("operation cancellation changed node attempt")
        self._cancel(task, "Program node was invalidated")

    def _acknowledge_task(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
    ) -> None:
        task = self._task(task_id)
        if task.identity != identity:
            raise ProductionPortError("operation acknowledgement changed node attempt")
        operation, snapshot = self._observation(task)
        if snapshot.state is not OwnerTaskState.SUCCEEDED:
            raise ProductionPortError("only a completed operation can be acknowledged")
        revision = operation.get("operation_revision")
        if type(revision) is not int or revision < 1:
            raise ProductionPortError("native operation revision is invalid")
        context = self._context(identity.program_run_id)
        owner_epoch, _scene = self._owner_status(context)
        self._kernel.acknowledge_completed(
            task.operation_id,
            {
                "request_context": {
                    "request_id": _request_identity(
                        "program-ack", identity.program_run_id, task.operation_id
                    ),
                    "request_owner_epoch": owner_epoch,
                    "source_label": "manyforge_behavior",
                    "correlation_id": identity.program_run_id,
                },
                "operation_id": task.operation_id,
                "expected_operation_revision": revision,
                "reason": "Program node consumed completed evidence",
            },
        )
        with self._lock:
            self._tasks.pop(task.operation_id, None)

    def _observe_scope(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        with self._lock:
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        try:
            active_count = sum(
                self._observation(task)[1].state is OwnerTaskState.ACTIVE
                for task in tasks
            )
        except Exception as exc:
            return ProgramRunScopeSnapshot(
                program_run_id=program_run_id,
                state=ProgramRunScopeState.UNKNOWN,
                active_operation_count=None,
                reason_code=(str(exc) or type(exc).__name__)[:256],
            )
        return ProgramRunScopeSnapshot(
            program_run_id=program_run_id,
            state=(
                ProgramRunScopeState.ACTIVE
                if active_count
                else ProgramRunScopeState.SETTLED
            ),
            active_operation_count=active_count,
        )

    def _request_stop(
        self,
        program_run_id: str,
        reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        with self._lock:
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        for task in tasks:
            self._cancel(task, reason)
        return self._observe_scope(program_run_id)

    def _observe_task(self, task_id: str) -> OwnerTaskSnapshot:
        _operation, snapshot = self._observation(self._task(task_id))
        return snapshot


class _NativeKernelOperationTaskPort:
    """Unambiguous OwnerTaskPort view over one native Program client."""

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        return self._owner._start(node_id, identity, inputs)

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return self._owner._observe_task(task_id)

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._owner._cancel_task(task_id, identity)

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._owner._acknowledge_task(task_id, identity)


class _NativeKernelProgramScopePort:
    """Unambiguous ProgramRunScopePort view over one native Program client."""

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner

    def request_stop(
        self,
        program_run_id: str,
        reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        return self._owner._request_stop(program_run_id, reason)

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return self._owner._observe_scope(program_run_id)


__all__ = ["NativeKernelProgramPort", "ProductionPortError"]
