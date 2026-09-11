"""Production client ports from bounded Program orchestration to native owners.

This module translates already-validated Program node inputs into the public
headless request vocabulary. It owns no planning, frame resolution, scene
truth, gripper state, or Stop semantics; those remain in the native owners.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass, field
from hashlib import sha256
from heapq import heapify, heappop, heappush
import json
import math
from threading import RLock
import time
from typing import Any, Callable, Final, Protocol

from manyforge_behavior.geometry import (
    normalize_quat,
    rpy_deg_to_xyzw,
)
from manyforge_behavior.json_scalars import nonnegative_int64
from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelOperationObservationUnavailableError,
    KernelOperationSettlementPendingError,
)
from manyforge_behavior.orchestration_ports import (
    PROGRAM_MOTION_STOP_RECOVERY_CLASSES,
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerInvocationPendingError,
    OwnerInvocationHeldError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.scene_client import (
    SceneClient,
    SceneOutcomeUnknownError,
    SceneTransactionRejectedError,
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
_MAX_SCENE_NODE_STALE_RETRIES: Final = 16
_SCENE_INITIALIZATION_SETTLEMENT_TIMEOUT_S: Final = 2.0
_SCENE_INITIALIZATION_SETTLEMENT_POLL_S: Final = 0.01
_PROGRAM_RESET_SETTLEMENT_TIMEOUT_S: Final = 5.0
_PROGRAM_RESET_SETTLEMENT_POLL_S: Final = 0.01
_AUTHORED_SCENE_OPERATIONS: Final = frozenset(
    {
        "add_object",
        "upsert_object",
        "remove_object",
        "update_object_pose",
        "set_collision_policy",
    }
)


class ProductionPortError(RuntimeError):
    """A captured client context or native owner projection is inconsistent."""


def _mapping(value: object, *, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProductionPortError(f"{field} must be a mapping")
    return value


def _text(value: object, *, field: str, maximum_bytes: int = 512) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum_bytes:
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
            raise ProductionPortError(f"{field}.frame_ref is not the selected target base frame")
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
        raise ProductionPortError(f"{field}.orientation must contain exactly xyzw or rpy_deg")
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


def _scene_pose3(value: object, *, field: str) -> dict[str, Any]:
    pose = _mapping(value, field=field)
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
        raise ProductionPortError(f"{field}.orientation must contain exactly xyzw or rpy_deg")
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


def _scene_shape(value: object, *, field: str) -> dict[str, Any]:
    shape = _mapping(value, field=field)
    kind = shape.get("type")
    if kind == "box" and set(shape) == {"type", "box_dimensions_m"}:
        dimensions = _sequence(
            shape["box_dimensions_m"],
            field=f"{field}.box_dimensions_m",
            length=3,
        )
        return {
            "type": "box",
            "box_dimensions_m": {
                "x_m": _number(dimensions[0], field=f"{field}.box_dimensions_m[0]"),
                "y_m": _number(dimensions[1], field=f"{field}.box_dimensions_m[1]"),
                "z_m": _number(dimensions[2], field=f"{field}.box_dimensions_m[2]"),
            },
        }
    if kind == "sphere" and set(shape) == {"type", "sphere_radius_m"}:
        return {
            "type": "sphere",
            "sphere_radius_m": _number(shape["sphere_radius_m"], field=f"{field}.sphere_radius_m"),
        }
    if kind == "cylinder" and set(shape) == {
        "type",
        "cylinder_radius_m",
        "cylinder_height_m",
    }:
        return {
            "type": "cylinder",
            "cylinder_radius_m": _number(
                shape["cylinder_radius_m"], field=f"{field}.cylinder_radius_m"
            ),
            "cylinder_height_m": _number(
                shape["cylinder_height_m"], field=f"{field}.cylinder_height_m"
            ),
        }
    if kind == "mesh" and set(shape) == {
        "type",
        "mesh_resource_uri",
        "mesh_scale",
    }:
        scale = _sequence(shape["mesh_scale"], field=f"{field}.mesh_scale", length=3)
        return {
            "type": "mesh",
            "mesh_resource_uri": _text(
                shape["mesh_resource_uri"],
                field=f"{field}.mesh_resource_uri",
                maximum_bytes=4096,
            ),
            "mesh_scale": {
                "x_m": _number(scale[0], field=f"{field}.mesh_scale[0]"),
                "y_m": _number(scale[1], field=f"{field}.mesh_scale[1]"),
                "z_m": _number(scale[2], field=f"{field}.mesh_scale[2]"),
            },
        }
    raise ProductionPortError(f"{field} has an unsupported shape")


def _scene_support_contacts(value: object, *, field: str) -> list[dict[str, Any]]:
    if value is None:
        return []
    metadata = _mapping(value, field=field)
    return [
        dict(_mapping(contact, field=f"{field}.support_contacts[{index}]"))
        for index, contact in enumerate(
            _sequence(
                metadata.get("support_contacts", ()),
                field=f"{field}.support_contacts",
            )
        )
    ]


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
class _ContactGraspBinding:
    contact_policy_id: str
    runtime_object_id: str


@dataclass(frozen=True, slots=True)
class _BoundContext:
    capture: ProgramRunPortContext
    targets: Mapping[str, _TargetBinding]
    contact_grasps: Mapping[str, _ContactGraspBinding]
    request_owner_epoch: str


class _SceneProgramTransport(Protocol):
    def authoring_admission(self) -> dict[str, Any]: ...

    def scene(self) -> dict[str, Any]: ...

    def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def manager_graph(self) -> dict[str, Any]: ...


@dataclass(frozen=True, slots=True)
class _SceneTargetBinding:
    target_role_id: str
    component_role_id: str
    controllable_component_id: str
    command_target_id: str
    frames: Mapping[str, tuple[str, str]]
    collision_body_roles: frozenset[str]


@dataclass(frozen=True, slots=True)
class _SceneBoundContext:
    capture: ProgramRunPortContext
    component_models: Mapping[tuple[str, str], tuple[str, str]]
    component_frames: Mapping[tuple[str, str, str], dict[str, str]]
    targets: Mapping[str, _SceneTargetBinding]


@dataclass(frozen=True, slots=True)
class _SceneView:
    request_owner_epoch: str
    projection_revision: dict[str, Any]
    cell_id: str
    objects: Mapping[str, Mapping[str, Any]]
    attachments: tuple[Mapping[str, Any], ...]


@dataclass(slots=True)
class _OperationTask:
    identity: NodeAttemptIdentity
    node_id: str
    operation_id: str
    request_owner_epoch: str
    stop_requested: bool = False
    # A successful observation is immutable until its exact acknowledgement.
    # Retain only the revision needed by that next command so acknowledge does
    # not repeat both the operation read and owner-identity read on every node.
    completed_revision: int | None = field(default=None, compare=False)


@dataclass(slots=True)
class _PickPlaceTask:
    identity: NodeAttemptIdentity
    node_id: str
    sequence_id: str
    kind: str
    selected_object_id: str
    planning_result: Mapping[str, Any]
    snapshot: OwnerTaskSnapshot
    active_stop_operation_id: str | None = None
    scope_stop_requested: bool = False


@dataclass(slots=True)
class _ProgramScopeStop:
    owner_epoch: str
    pending: int = 0
    generation: int = 0
    active: tuple[str, int, int] | None = None
    unresolved: object | None = None


class NativeKernelProgramPort:
    """Captured context plus separate native task and Program-scope clients."""

    def __init__(
        self,
        kernel: KernelClient,
        *,
        current_owner_epoch: Callable[[], str] | None = None,
    ) -> None:
        if not isinstance(kernel, KernelClient):
            raise TypeError("kernel must be one bounded KernelClient")
        if current_owner_epoch is not None and not callable(current_owner_epoch):
            raise TypeError("current_owner_epoch must be callable")
        self._kernel = kernel
        self._current_owner_epoch = current_owner_epoch
        self._contexts: dict[str, _BoundContext] = {}
        self._tasks: dict[str, _OperationTask] = {}
        self._pick_place_tasks: dict[str, _PickPlaceTask] = {}
        self._scope_stops: dict[str, _ProgramScopeStop] = {}
        self._lock = RLock()
        self.operations = _NativeKernelOperationTaskPort(self)
        self.controls = _NativeKernelControlTaskPort(self)
        self.queries = _NativeTargetQueryPort(self)
        self.pick_place = _NativePickPlaceTaskPort(self)
        self.scope = _NativeKernelProgramScopePort(self)

    @staticmethod
    def _parse_context(
        context: ProgramRunPortContext,
        *,
        request_owner_epoch: str,
    ) -> _BoundContext:
        binding = context.program_binding
        rows = binding.get("target_role_bindings")
        if not isinstance(rows, (list, tuple)) or not rows:
            raise ProductionPortError("Program Binding has no target-role bindings")
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
                capability_id = _text(capability.get("capability_id"), field="capability_id")
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
        contact_grasps: dict[str, _ContactGraspBinding] = {}
        raw_contact_grasps = binding.get("contact_grasp_bindings", ())
        if not isinstance(raw_contact_grasps, (list, tuple)):
            raise ProductionPortError("Program Binding contact-grasp bindings are invalid")
        for index, raw_contact in enumerate(raw_contact_grasps):
            contact = _mapping(
                raw_contact,
                field=f"contact_grasp_bindings[{index}]",
            )
            policy_id = _text(
                contact.get("contact_policy_id"),
                field=f"contact_grasp_bindings[{index}].contact_policy_id",
            )
            runtime_object_id = _text(
                contact.get("runtime_object_id"),
                field=f"contact_grasp_bindings[{index}].runtime_object_id",
            )
            if policy_id in contact_grasps:
                raise ProductionPortError("Program Binding repeats a contact-grasp policy")
            contact_grasps[policy_id] = _ContactGraspBinding(
                policy_id,
                runtime_object_id,
            )
        return _BoundContext(
            context,
            targets,
            contact_grasps,
            _text(request_owner_epoch, field="request_owner_epoch"),
        )

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("bind requires one exact Program-run port context")
        owner_epoch, _scene = self._owner_status_for_capture(context)
        parsed = self._parse_context(
            context,
            request_owner_epoch=owner_epoch,
        )
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
            stop = self._scope_stops.get(program_run_id)
            has_scope_stop = stop is not None
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
            pick_place_tasks = tuple(
                task
                for task in self._pick_place_tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        if has_scope_stop:
            scope = self._observe_scope(program_run_id)
            if scope.state is not ProgramRunScopeState.SETTLED:
                raise ProductionPortError(
                    "cannot unbind a Program run before aggregate Stop settlement"
                )
            with self._lock:
                for task in pick_place_tasks:
                    self._pick_place_tasks.pop(task.sequence_id, None)
                self._contexts.pop(program_run_id, None)
            return
        completed: list[_OperationTask] = []
        for task in tasks:
            _operation, snapshot = self._observation(task)
            if snapshot.state is OwnerTaskState.ACTIVE:
                raise ProductionPortError(
                    "cannot unbind a Program run with active native operations"
                )
            if snapshot.state is OwnerTaskState.SUCCEEDED:
                completed.append(task)
        # A sequence may settle between its final owner transition and the
        # next tree tick. Consume successful evidence now. Interrupted/failed
        # operations keep only their small identity row until Program Reset
        # releases the corresponding resident Kernel operation.
        for task in completed:
            self._acknowledge_task(task.operation_id, task.identity)
        terminal_pick_place: list[_PickPlaceTask] = []
        for task in pick_place_tasks:
            snapshot = self._observe_pick_place_task(task.sequence_id)
            if snapshot.state is OwnerTaskState.ACTIVE:
                raise ProductionPortError(
                    "cannot unbind a Program run with an active pick/place sequence"
                )
            terminal_pick_place.append(task)
        with self._lock:
            for task in terminal_pick_place:
                self._pick_place_tasks.pop(task.sequence_id, None)
            self._contexts.pop(program_run_id, None)

    def _context(self, program_run_id: str) -> _BoundContext:
        with self._lock:
            try:
                return self._contexts[program_run_id]
            except KeyError as exc:
                raise ProductionPortError("Program-run client context is not bound") from exc

    def _require_scope_open(self, program_run_id: str) -> None:
        """Reject fresh local work after the Program owner accepted Stop.

        The native Kernel independently fences the same exact run while an
        admission is crossing this check, so this local lifecycle guard never
        has to block Stop behind ordinary command work.
        """

        with self._lock:
            stop = self._scope_stops.get(program_run_id)
            if stop is not None:
                raise ProductionPortError("Program-run scope is stopped; fresh work is unavailable")

    @staticmethod
    def _target(
        context: _BoundContext,
        target_role_id: str,
        capability_id: str | None,
    ) -> _TargetBinding:
        try:
            target = context.targets[target_role_id]
        except KeyError as exc:
            raise ProductionPortError(
                f"Program Binding has no target role {target_role_id!r}"
            ) from exc
        if capability_id is not None and capability_id not in target.capabilities:
            raise ProductionPortError(f"target role {target_role_id!r} lacks {capability_id!r}")
        return target

    def _owner_status_for_capture(
        self,
        capture: ProgramRunPortContext,
    ) -> tuple[str, dict[str, Any]]:
        envelope = self._kernel.identity()
        status = _mapping(envelope.get("identity"), field="execution identity")
        owner_epoch = _text(status.get("owner_epoch"), field="owner_epoch")
        if (
            status.get("graph_revision") != capture.graph_revision
            or status.get("retained_bundle_id") != capture.retained_bundle_id
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

    def _owner_status(self, context: _BoundContext) -> tuple[str, dict[str, Any]]:
        return self._owner_status_for_capture(context.capture)

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
                joint_names.append(_text(goal.get("joint_name"), field=f"goal[{index}].joint_name"))
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
                "max_linear_acceleration_mps2": parameters["max_linear_acceleration_mps2"],
                "max_angular_acceleration_rad_s2": parameters["max_angular_acceleration_rad_s2"],
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
        target = self._target(context, target_role_id, "execution.trajectory.execute")
        base_frame_role_id = _text(parameters.get("base_frame_role"), field="base_frame_role")
        tcp_frame_role_id = _text(parameters.get("tcp_frame_role"), field="tcp_frame_role")
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
                "runtime-object motion references require the N7-a5.3b " "scene-binding join"
            )
        payload = {
            "kind": "motion",
            "move_id": _request_identity("move", identity.program_run_id, identity.node_attempt_id),
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
                "max_tcp_linear_speed_mps": parameters.get("max_tcp_linear_speed_mps"),
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
            "per_joint_dynamics_limits": dict(parameters.get("per_joint_dynamics_limits", {})),
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
        command_schema = _mapping(capability.get("command_schema"), field="gripper command_schema")
        schema_id = _text(command_schema.get("command_schema_id"), field="command_schema_id")
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
        return (
            context,
            target,
            {
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
            },
        )

    @staticmethod
    def _pick_synthesis(
        parameters: Mapping[str, Any],
        *,
        target_role_id: str,
        base_frame_role_id: str,
    ) -> dict[str, Any]:
        synthesis = _mapping(parameters.get("synthesis"), field="synthesis")

        def envelope(name: str) -> dict[str, list[float]]:
            value = _mapping(synthesis.get(name), field=f"synthesis.{name}")
            return {
                key: [
                    _number(item, field=f"synthesis.{name}.{key}[{index}]")
                    for index, item in enumerate(
                        _sequence(
                            value.get(key),
                            field=f"synthesis.{name}.{key}",
                            length=3,
                        )
                    )
                ]
                for key in (
                    "position_min_m",
                    "position_max_m",
                    "orientation_rpy_min_deg",
                    "orientation_rpy_max_deg",
                )
            }

        def reference(name: str) -> dict[str, Any] | None:
            return _reference(
                synthesis.get(name),
                field=f"synthesis.{name}",
                target_role_id=target_role_id,
                base_frame_role_id=base_frame_role_id,
            )

        base_orientation = synthesis.get("base_tool_orientation_rpy_deg", (180.0, 0.0, 0.0))
        return {
            "pick_offset_envelope": envelope("pick_offset_envelope"),
            "approach_offset_envelope": envelope("approach_offset_envelope"),
            "pick_offset_reference": reference("pick_offset_reference"),
            "approach_offset_reference": reference("approach_offset_reference"),
            "base_tool_orientation_rpy_deg": [
                _number(value, field=f"base_tool_orientation_rpy_deg[{index}]")
                for index, value in enumerate(
                    _sequence(
                        base_orientation,
                        field="base_tool_orientation_rpy_deg",
                        length=3,
                    )
                )
            ],
            "translation_step_m": _number(
                synthesis.get("translation_step_m", 0.01),
                field="translation_step_m",
            ),
            "rotation_step_deg": _number(
                synthesis.get("rotation_step_deg", 15.0),
                field="rotation_step_deg",
            ),
            "force_vertical_pick": synthesis.get("force_vertical_pick", False),
            "min_tcp_z_m": synthesis.get("min_tcp_z_m"),
            "maximum_candidate_count": synthesis.get("maximum_candidate_count", 4096),
            "search_strategy": synthesis.get("search_strategy", "first_feasible"),
        }

    @staticmethod
    def _phase_motion_payload(
        *,
        identity: NodeAttemptIdentity,
        parameters: Mapping[str, Any],
        phase: str,
        motion: Mapping[str, Any],
        contact_policy_id: str | None = None,
        runtime_object_id: str | None = None,
        selected_object_id: str | None = None,
    ) -> dict[str, Any]:
        options = _mapping(parameters.get("motion_options", {}), field="motion_options")
        velocity_key = {
            "approach": "approach_velocity_scale",
            "descend": "descend_velocity_scale",
            "retreat": "retreat_velocity_scale",
        }[phase]
        runtime_bindings = []
        if runtime_object_id is not None or selected_object_id is not None:
            if runtime_object_id is None or selected_object_id is None:
                raise ProductionPortError("pick runtime-object binding is incomplete")
            runtime_bindings = [
                {
                    "runtime_object_id": runtime_object_id,
                    "scene_resource_id": selected_object_id,
                }
            ]
        return {
            "kind": "motion",
            "move_id": _request_identity(
                "pick-place-move",
                identity.program_run_id,
                identity.node_attempt_id,
                phase,
            ),
            "base_frame_role_id": _text(parameters.get("base_frame_role"), field="base_frame_role"),
            "tcp_frame_role_id": _text(parameters.get("tcp_frame_role"), field="tcp_frame_role"),
            "tolerances": {
                "joint_position_tol_rad": options.get("joint_position_tol_rad", 0.05),
                "translation_tol_m": options.get("translation_tol_m", 0.005),
                "rotation_tol_rad": options.get("rotation_tol_rad", 0.05),
            },
            "speed_profile": {
                "velocity_scale": options.get(velocity_key, 0.35),
                "acceleration_scale": options.get("acceleration_scale", 1.0),
                "max_tcp_linear_speed_mps": options.get("max_tcp_linear_speed_mps"),
            },
            "timeouts": {
                "planning_timeout_s": options.get("planning_timeout_s", 2.0),
                "execution_timeout_s": options.get("execution_timeout_s", 10.0),
            },
            "cache_mode": options.get("cache_mode", "disable"),
            "planning_profile": parameters.get("planning_profile", "first_feasible"),
            "target_mode": "absolute",
            "target_reference": None,
            "offset_reference": None,
            "motion": dict(motion),
            "contact_policy_id": contact_policy_id,
            "per_joint_dynamics_limits": {},
            "motion_interrupt_scopes": [],
            "runtime_object_bindings": runtime_bindings,
        }

    @staticmethod
    def _cartesian_motion(
        waypoints: Sequence[Mapping[str, Any]],
    ) -> dict[str, Any]:
        return {
            "kind": "cartesian_path",
            "waypoints": [dict(waypoint) for waypoint in waypoints],
            "max_translation_step_m": 0.005,
            "max_rotation_step_rad": 0.05,
            "speed_mode": "bounded",
            "max_linear_speed_mps": 0.1,
            "max_angular_speed_rad_s": 0.5,
            "max_linear_acceleration_mps2": 0.5,
            "max_angular_acceleration_rad_s2": 2.5,
            "fallback_policy": "fail_closed",
        }

    @staticmethod
    def _phase_operation(
        *,
        context: _BoundContext,
        target: _TargetBinding,
        capability_id: str,
        payload: Mapping[str, Any],
        identity: NodeAttemptIdentity,
        owner_epoch: str,
        scene_revision: Mapping[str, Any],
        phase: str,
    ) -> dict[str, Any]:
        child_attempt_id = f"{identity.node_attempt_id}:{phase}"
        span = ActionSpan(
            action_source="program_node",
            action_span_id=child_attempt_id,
            program_run_id=identity.program_run_id,
            node_instance_id=f"{identity.node_instance_id}/{phase}",
            node_attempt_id=child_attempt_id,
            parent_node_attempt_id=identity.node_attempt_id,
        )
        return {
            "request_context": {
                "request_id": _request_identity(
                    "pick-place-phase",
                    identity.program_run_id,
                    identity.node_attempt_id,
                    phase,
                ),
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
            "expected_scene_revision": dict(scene_revision),
            "participants": [
                {
                    "component_role_id": target.component_role_id,
                    "target_role_id": target.target_role_id,
                    "required_capability_id": capability_id,
                    "action_span": span.as_request(),
                    "execution_scope": {
                        "node_instance_id": identity.node_instance_id,
                        "node_attempt_id": identity.node_attempt_id,
                    },
                    "payload": dict(payload),
                }
            ],
            "coordination_policy": "independent",
        }

    def _pick_place_intent(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        parameters: Mapping[str, Any],
    ) -> tuple[str, str, dict[str, Any]]:
        context = self._context(identity.program_run_id)
        target_role_id = _text(parameters.get("target_role"), field="target_role")
        arm = self._target(
            context,
            target_role_id,
            "execution.trajectory.execute",
        )
        tool_role_id = _text(parameters.get("tool_target_role"), field="tool_target_role")
        tool = self._target(context, tool_role_id, "tool.gripper.command")
        owner_epoch, scene_revision = self._owner_status(context)
        selected_object_id = _text(parameters.get("selected_object_id"), field="selected_object_id")
        kind = "place" if node_id == "motion.place_object" else "pick"
        sequence_id = _request_identity(
            kind,
            identity.program_run_id,
            identity.node_attempt_id,
        )

        identity_pose = {
            "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        }
        if kind == "pick":
            contact_policy_id = _text(
                parameters.get("contact_policy_id"), field="contact_policy_id"
            )
            try:
                contact = context.contact_grasps[contact_policy_id]
            except KeyError as exc:
                raise ProductionPortError(
                    "Program Binding has no exact pick contact-grasp policy"
                ) from exc
            approach_motion: Mapping[str, Any] = {
                "kind": "pose_goal",
                "goal": identity_pose,
            }
            descend_motion = self._cartesian_motion((identity_pose, identity_pose))
            retreat_motion = self._cartesian_motion((identity_pose, identity_pose))
            synthesis = self._pick_synthesis(
                parameters,
                target_role_id=target_role_id,
                base_frame_role_id=_text(
                    parameters.get("base_frame_role"), field="base_frame_role"
                ),
            )
        else:
            contact_policy_id = None
            contact = None
            place_pose = _pose3(
                parameters.get("place_pose"),
                field="place_pose",
                target_role_id=target_role_id,
                base_frame_role_id=_text(
                    parameters.get("base_frame_role"), field="base_frame_role"
                ),
            )
            offset = [
                _number(value, field=f"approach_offset_m[{index}]")
                for index, value in enumerate(
                    _sequence(
                        parameters.get("approach_offset_m", (0.0, 0.0, 0.05)),
                        field="approach_offset_m",
                        length=3,
                    )
                )
            ]
            approach_pose = {
                "position_m": {
                    axis: place_pose["position_m"][axis] + offset[index]
                    for index, axis in enumerate(("x_m", "y_m", "z_m"))
                },
                "orientation": dict(place_pose["orientation"]),
            }
            approach_motion = {"kind": "pose_goal", "goal": approach_pose}
            descend_motion = self._cartesian_motion((approach_pose, place_pose))
            retreat_motion = self._cartesian_motion((place_pose, approach_pose))
            synthesis = None

        phase_payloads = {
            "approach": self._phase_motion_payload(
                identity=identity,
                parameters=parameters,
                phase="approach",
                motion=approach_motion,
            ),
            "descend": self._phase_motion_payload(
                identity=identity,
                parameters=parameters,
                phase="descend",
                motion=descend_motion,
                contact_policy_id=contact_policy_id,
                runtime_object_id=(None if contact is None else contact.runtime_object_id),
                selected_object_id=(None if contact is None else selected_object_id),
            ),
            "retreat": self._phase_motion_payload(
                identity=identity,
                parameters=parameters,
                phase="retreat",
                motion=retreat_motion,
            ),
        }
        _tool_context, _tool_target, tool_payload = self._gripper_payload(
            {
                "target_role": tool_role_id,
                "position": parameters.get("tool_position"),
                "max_effort": parameters.get("tool_max_effort"),
                "command_duration_s": parameters.get("tool_command_duration_s", 0.0),
                "completion_timeout_s": parameters.get("tool_completion_timeout_s", 30.0),
            },
            identity,
        )
        operations = {
            phase: self._phase_operation(
                context=context,
                target=(tool if phase == "tool" else arm),
                capability_id=(
                    "tool.gripper.command" if phase == "tool" else "execution.trajectory.execute"
                ),
                payload=(tool_payload if phase == "tool" else phase_payloads[phase]),
                identity=identity,
                owner_epoch=owner_epoch,
                scene_revision=scene_revision,
                phase=phase,
            )
            for phase in ("approach", "descend", "tool", "retreat")
        }
        intent = {
            "sequence_id": sequence_id,
            "selected_object_id": selected_object_id,
            "carrier_frame_role_id": _text(
                parameters.get("carrier_frame_role"),
                field="carrier_frame_role",
            ),
            "approach_operation": operations["approach"],
            "descend_operation": operations["descend"],
            "tool_operation": operations["tool"],
            "retreat_operation": operations["retreat"],
        }
        if kind == "pick":
            intent["selected_runtime_object_id"] = contact.runtime_object_id
            intent["synthesis"] = synthesis
        return kind, selected_object_id, intent

    @staticmethod
    def _validated_planning_result(
        response: Mapping[str, Any],
        *,
        sequence_id: str,
        kind: str,
        selected_object_id: str,
    ) -> dict[str, Any]:
        result = dict(_mapping(response.get("planning_result"), field="planning_result"))
        status = result.get("status")
        if (
            result.get("schema_version") != "manyforge.pick_place_planning_result.v1"
            or result.get("sequence_id") != sequence_id
            or result.get("kind") != kind
            or result.get("selected_object_id") != selected_object_id
            or status not in {"planned", "no_feasible_candidate"}
            or type(result.get("attempted_candidate_count")) is not int
            or result["attempted_candidate_count"] < 0
            or not isinstance(result.get("reports"), list)
            or (status == "planned") != isinstance(result.get("plan"), Mapping)
        ):
            raise ProductionPortError("pick/place planning response changed its public identity")
        return result

    @staticmethod
    def _no_feasible_candidate_message(
        planning: Mapping[str, Any],
    ) -> str:
        prefix = "No feasible pick/place candidate was found"
        reports = planning.get("reports")
        if not isinstance(reports, (list, tuple)):
            return prefix
        for report in reversed(reports):
            if not isinstance(report, Mapping):
                continue
            stages = report.get("stages")
            if not isinstance(stages, (list, tuple)):
                continue
            for stage in reversed(stages):
                if not isinstance(stage, Mapping) or stage.get("succeeded") is not False:
                    continue
                error = stage.get("error")
                if not isinstance(error, Mapping):
                    continue
                stage_name = stage.get("stage")
                code = error.get("code")
                message = error.get("message")
                details = error.get("details")
                reason = details.get("reason") if isinstance(details, Mapping) else None
                diagnostic = next(
                    (
                        value
                        for value in (message, reason, code)
                        if isinstance(value, str) and value
                    ),
                    None,
                )
                if diagnostic is None:
                    continue
                stage_text = (
                    f" at {stage_name}" if isinstance(stage_name, str) and stage_name else ""
                )
                return f"{prefix}; last rejection{stage_text}: {diagnostic}"[:4_096]
        return prefix

    @staticmethod
    def _portable_pick_planning_output(
        planning: Mapping[str, Any],
        *,
        target_role_id: str,
        base_frame_role_id: str,
    ) -> dict[str, Any]:
        """Project diagnostic candidate geometry, never its prepared plan."""

        candidate = _mapping(
            planning.get("last_attempted_candidate"),
            field="last_attempted_candidate",
        )

        def stored_pose(name: str) -> dict[str, Any]:
            pose = _mapping(candidate.get(name), field=name)
            position = _mapping(pose.get("position_m"), field=f"{name}.position_m")
            orientation = _mapping(
                pose.get("orientation"),
                field=f"{name}.orientation",
            )
            return {
                "frame_ref": {
                    "kind": "target_frame",
                    "target_role": target_role_id,
                    "frame_role": base_frame_role_id,
                },
                "position_m": [
                    _number(position.get(axis), field=f"{name}.position_m.{axis}")
                    for axis in ("x_m", "y_m", "z_m")
                ],
                "orientation": {
                    "xyzw": [
                        _number(
                            orientation.get(axis),
                            field=f"{name}.orientation.{axis}",
                        )
                        for axis in ("x", "y", "z", "w")
                    ]
                },
            }

        return {
            "approach_pose": stored_pose("approach_pose"),
            "pick_pose": stored_pose("pick_pose"),
            "retreat_pose": stored_pose("retreat_pose"),
            "selected_object_id": _text(
                planning.get("selected_object_id"),
                field="selected_object_id",
            ),
        }

    @staticmethod
    def _sequence_snapshot(
        task: _PickPlaceTask,
        status: Mapping[str, Any],
    ) -> OwnerTaskSnapshot:
        phase = status.get("phase")
        if (
            status.get("schema_version") != "manyforge.pick_place_sequence_status.v1"
            or status.get("sequence_id") != task.sequence_id
            or status.get("kind") != task.kind
            or status.get("selected_object_id") != task.selected_object_id
        ):
            raise ProductionPortError("pick/place sequence response changed its public identity")
        metadata: dict[str, Any] = {
            "sequence_id": task.sequence_id,
            "planning_result": dict(task.planning_result),
            "sequence_status": dict(status),
        }
        if phase in {
            "approach",
            "descend",
            "tool_command",
            "scene_transition",
            "retreat",
        }:
            return OwnerTaskSnapshot(
                task_id=task.sequence_id,
                state=OwnerTaskState.ACTIVE,
                metadata=metadata,
            )
        if phase == "completed":
            return OwnerTaskSnapshot(
                task_id=task.sequence_id,
                state=OwnerTaskState.SUCCEEDED,
                metadata=metadata,
            )
        error = status.get("terminal_error")
        error_value = error if isinstance(error, Mapping) else {}
        reason_code = error_value.get("code")
        message = error_value.get("message")
        if not isinstance(reason_code, str) or not reason_code:
            reason_code = f"pick_place_{phase or 'failed'}"
        if not isinstance(message, str):
            message = "native pick/place sequence did not complete"
        if phase == "intervention_required":
            metadata.update(
                {
                    "authored_failure_policy": "pause_for_operator",
                    "authored_failure_policy_parameter": "owner_terminal_phase",
                }
            )
        if phase not in {"failed", "intervention_required"}:
            raise ProductionPortError("pick/place sequence has an unknown phase")
        return OwnerTaskSnapshot(
            task_id=task.sequence_id,
            state=OwnerTaskState.FAILED,
            metadata=metadata,
            reason_code=reason_code,
            message=message,
        )

    def _start_pick_place(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        if node_id not in {
            "planning.plan_pick_sequence",
            "motion.pick_object",
            "motion.place_object",
        }:
            raise ProductionPortError(f"pick/place port does not implement {node_id!r}")
        kind, selected_object_id, intent = self._pick_place_intent(
            node_id,
            identity,
            inputs.parameters,
        )

        def admit() -> OwnerTaskSnapshot:
            if node_id == "planning.plan_pick_sequence":
                response = self._kernel.plan_pick(intent)
            elif node_id == "motion.pick_object":
                response = self._kernel.start_pick(intent)
            else:
                response = self._kernel.start_place(intent)
            response_value = _mapping(response, field="pick/place response")
            planning = self._validated_planning_result(
                response_value,
                sequence_id=intent["sequence_id"],
                kind=kind,
                selected_object_id=selected_object_id,
            )
            metadata: dict[str, Any] = {
                "sequence_id": intent["sequence_id"],
                "planning_result": planning,
            }
            if planning["status"] == "no_feasible_candidate":
                if kind == "pick":
                    metadata.update(
                        {
                            "authored_failure_policy": inputs.parameters.get(
                                "no_feasible_plan_policy",
                                "pause_for_operator",
                            ),
                            "authored_failure_policy_parameter": ("no_feasible_plan_policy"),
                        }
                    )
                snapshot = OwnerTaskSnapshot(
                    task_id=intent["sequence_id"],
                    state=OwnerTaskState.FAILED,
                    metadata=metadata,
                    reason_code="no_feasible_candidate",
                    message=self._no_feasible_candidate_message(planning),
                )
            elif node_id == "planning.plan_pick_sequence":
                snapshot = OwnerTaskSnapshot(
                    task_id=intent["sequence_id"],
                    state=OwnerTaskState.SUCCEEDED,
                    output=self._portable_pick_planning_output(
                        planning,
                        target_role_id=_text(
                            inputs.parameters.get("target_role"),
                            field="target_role",
                        ),
                        base_frame_role_id=_text(
                            inputs.parameters.get("base_frame_role"),
                            field="base_frame_role",
                        ),
                    ),
                    metadata=metadata,
                )
            else:
                status = _mapping(
                    response_value.get("sequence_status"),
                    field="sequence_status",
                )
                provisional = _PickPlaceTask(
                    identity,
                    node_id,
                    intent["sequence_id"],
                    kind,
                    selected_object_id,
                    planning,
                    OwnerTaskSnapshot(
                        task_id=intent["sequence_id"],
                        state=OwnerTaskState.ACTIVE,
                    ),
                )
                snapshot = self._sequence_snapshot(provisional, status)
            task = _PickPlaceTask(
                identity,
                node_id,
                intent["sequence_id"],
                kind,
                selected_object_id,
                planning,
                snapshot,
            )
            with self._lock:
                current = self._pick_place_tasks.get(task.sequence_id)
                if current is not None and (
                    current.identity != identity or current.node_id != node_id
                ):
                    raise ProductionPortError("pick/place task identity was reused")
                if current is None:
                    if len(self._tasks) + len(self._pick_place_tasks) >= _MAX_TASKS:
                        raise ProductionPortError("native task retention is full")
                    self._pick_place_tasks[task.sequence_id] = task
                else:
                    task = current
            return task.snapshot

        self._require_scope_open(identity.program_run_id)
        return admit()

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
            context, target, payload = self._motion_payload(node_id, inputs.parameters, identity)
            capability_id = "execution.trajectory.execute"
        elif node_id == "gripper.command_position":
            context, target, payload = self._gripper_payload(inputs.parameters, identity)
            capability_id = "tool.gripper.command"
        else:
            raise ProductionPortError(f"Kernel operation port does not implement {node_id!r}")
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

        def admit() -> OwnerTaskSnapshot:
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
                    "expected_retained_bundle_id": (context.capture.retained_bundle_id),
                    "expected_scene_revision": scene_revision,
                    "participants": [
                        {
                            "component_role_id": target.component_role_id,
                            "target_role_id": target.target_role_id,
                            "required_capability_id": capability_id,
                            "action_span": span.as_request(),
                            "execution_scope": {
                                "node_instance_id": identity.node_instance_id,
                                "node_attempt_id": identity.node_attempt_id,
                            },
                            "payload": payload,
                        }
                    ],
                    "coordination_policy": "independent",
                }
            )
            accepted_value = _mapping(accepted.get("accepted"), field="acceptance")
            operation_id = _text(
                accepted_value.get("operation_id"),
                field="accepted.operation_id",
            )
            with self._lock:
                current = self._tasks.get(operation_id)
                task = _OperationTask(
                    identity,
                    node_id,
                    operation_id,
                    owner_epoch,
                )
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

        self._require_scope_open(identity.program_run_id)
        return admit()

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

    def _recovery_assessment(
        self, operation: Mapping[str, Any], operation_id: str
    ) -> Mapping[str, Any] | None:
        try:
            assessment = self._kernel.recovery_assessment(operation_id)
        except Exception:
            return None
        if assessment.get("operation_revision") != operation.get("operation_revision"):
            return None
        return assessment

    def _observation(self, task: _OperationTask) -> tuple[dict[str, Any], OwnerTaskSnapshot]:
        try:
            envelope = self._kernel.operation(task.operation_id)
        except KernelOperationObservationUnavailableError as exc:
            raise OwnerTaskObservationUnavailableError(
                "accepted native operation is temporarily unavailable to observe"
            ) from exc
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
        stop_id = operation.get("active_stop_id")
        stop_generation = operation.get("active_stop_generation")
        if (
            isinstance(stop_id, str)
            and stop_id
            and type(stop_generation) is int
            and stop_generation > 0
        ):
            metadata["active_stop_id"] = stop_id
            metadata["active_stop_generation"] = stop_generation
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
            assessment = self._recovery_assessment(operation, task.operation_id)
            if assessment is not None:
                metadata["recovery_assessment"] = assessment
                if assessment.get("recovery_class") in (PROGRAM_MOTION_STOP_RECOVERY_CLASSES):
                    metadata["authored_failure_policy"] = "pause_for_operator"
                    metadata["authored_failure_policy_parameter"] = "operator_stop_policy"
            snapshot = OwnerTaskSnapshot(
                task_id=task.operation_id,
                state=OwnerTaskState.INTERRUPTED,
                metadata=metadata,
                reason_code=f"operation_{state}",
                message="native operation was interrupted",
            )
        elif state in _FAILED_STATES:
            assessment = self._recovery_assessment(operation, task.operation_id)
            if assessment is not None:
                metadata["recovery_assessment"] = assessment
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

    def _apply_operation_recovery(
        self,
        operation_id: str,
        identity: NodeAttemptIdentity,
        operation: Mapping[str, Any],
        snapshot: OwnerTaskSnapshot,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Mapping[str, Any]:
        if snapshot.state not in {
            OwnerTaskState.FAILED,
            OwnerTaskState.INTERRUPTED,
        }:
            raise ProductionPortError("operation recovery is not current")
        assessment = snapshot.metadata.get("recovery_assessment")
        revision = operation.get("operation_revision")
        if not isinstance(assessment, Mapping) or type(revision) is not int:
            raise ProductionPortError("operation recovery identity is incomplete")
        try:
            result = self._kernel.recover(
                operation_id,
                revision,
                {
                    "request_id": request_id,
                    "action": action,
                    "principal_id": "program_runner",
                    "expected_assessment_revision": assessment.get("assessment_revision"),
                    "expected_control_command_epoch": assessment.get("control_command_epoch"),
                    "expected_execution_id": assessment.get("execution_id"),
                    "expected_latch_id": assessment.get("latch_id", ""),
                    "expected_stop_certificate_id": assessment.get("stop_certificate_id", ""),
                    "expected_stop_owner_generation": assessment.get("stop_owner_generation", 0),
                    "reason": reason or "",
                },
            )
        except Exception as error:
            code = getattr(error, "code", None)
            if code not in {
                "operation_recovery_invalid",
                "operation_recovery_stale",
                "operation_recovery_blocked_by_stop_fault",
                "request_id_conflict",
                "request_ledger_full",
            }:
                raise
            return {
                "request_id": request_id,
                "action": action,
                "accepted": False,
                "resulting_assessment": dict(assessment),
                "error": {
                    "code": code,
                    "message": str(error) or "operation recovery was rejected",
                },
            }
        resulting = result.get("resulting_assessment")
        if isinstance(resulting, Mapping):
            result = {
                **result,
                "resulting_assessment": {
                    **resulting,
                    "operation_revision": revision,
                    "participants": assessment.get("participants", []),
                    "acknowledged_non_moving": (
                        result.get("accepted") is True and action == "acknowledge_and_continue"
                    ),
                },
            }
        # Program Resume owns its Stop-fence handoff. Operation recovery must
        # neither release a newer independent Stop nor reinterpret acceptance
        # through a fallible follow-up observation.
        return result

    def _recover_task(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Mapping[str, Any]:
        task = self._task(task_id)
        if task.identity != identity:
            raise ProductionPortError("operation recovery changed node attempt")
        operation, snapshot = self._observation(task)
        result = self._apply_operation_recovery(
            task.operation_id,
            identity,
            operation,
            snapshot,
            action=action,
            request_id=request_id,
            reason=reason,
        )
        if result.get("accepted") is True:
            # Kernel recovery made this stopped occurrence historical.  The
            # behavior-tree retry owns the fresh attempt, so retaining the old
            # task would make the shared native backend's next snapshot appear
            # under both identities and grow Program-scope work per Resume.
            with self._lock:
                if self._tasks.get(task_id) is task:
                    self._tasks.pop(task_id, None)
        return result

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
        with self._lock:
            revision = task.completed_revision
        if revision is None:
            operation, snapshot = self._observation(task)
            if snapshot.state is not OwnerTaskState.SUCCEEDED:
                raise ProductionPortError("only a completed operation can be acknowledged")
            revision = operation.get("operation_revision")
        if type(revision) is not int or revision < 1:
            raise ProductionPortError("native operation revision is invalid")
        try:
            self._kernel.acknowledge_completed(
                task.operation_id,
                {
                    "request_context": {
                        "request_id": _request_identity(
                            "program-ack", identity.program_run_id, task.operation_id
                        ),
                        "request_owner_epoch": task.request_owner_epoch,
                        "source_label": "manyforge_behavior",
                        "correlation_id": identity.program_run_id,
                    },
                    "operation_id": task.operation_id,
                    "expected_operation_revision": revision,
                    "reason": "Program node consumed completed evidence",
                },
            )
        except KernelOperationSettlementPendingError as error:
            raise OwnerTaskSettlementPendingError(
                "operation completion is still settling"
            ) from error
        with self._lock:
            self._tasks.pop(task.operation_id, None)

    def _observe_scope(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        with self._lock:
            stop = self._scope_stops.get(program_run_id)
            if stop is not None and stop.unresolved is not None:
                return ProgramRunScopeSnapshot(
                    program_run_id,
                    ProgramRunScopeState.UNKNOWN,
                    None,
                    "program_run_stop_outcome_unknown",
                )
            if stop is not None and stop.pending:
                return ProgramRunScopeSnapshot(program_run_id, ProgramRunScopeState.ACTIVE, 0)
            scope_stop = stop.active if stop is not None else None
            stop_owner_epoch = stop.owner_epoch if stop is not None else None
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
            pick_place_tasks = tuple(
                task
                for task in self._pick_place_tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        if scope_stop is not None:
            stop_id, stop_generation, retained_count = scope_stop
            try:
                status = _mapping(
                    self._kernel.stop_status(stop_id).get("stop_status"),
                    field="aggregate Stop status",
                )
                state = _text(status.get("state"), field="Stop status state")
                if (
                    type(status.get("stop_generation")) is not int
                    or status["stop_generation"] != stop_generation
                ):
                    raise ProductionPortError("program_run_stop_generation_mismatch")
            except Exception as exc:
                return ProgramRunScopeSnapshot(
                    program_run_id=program_run_id,
                    state=ProgramRunScopeState.UNKNOWN,
                    active_operation_count=None,
                    reason_code=(str(exc) or type(exc).__name__)[:256],
                )
            with self._lock:
                if (
                    self._scope_stops.get(program_run_id) is not stop
                    or stop.owner_epoch != stop_owner_epoch
                    or stop.active != scope_stop
                    or stop.pending
                    or stop.unresolved is not None
                ):
                    return ProgramRunScopeSnapshot(
                        program_run_id,
                        ProgramRunScopeState.UNKNOWN,
                        None,
                        "program_run_stop_observation_changed",
                    )
            if state in {"accepted", "stopping", "certifying"}:
                return ProgramRunScopeSnapshot(
                    program_run_id,
                    ProgramRunScopeState.ACTIVE,
                    retained_count,
                )
            if state == "succeeded":
                return ProgramRunScopeSnapshot(
                    program_run_id,
                    ProgramRunScopeState.SETTLED,
                    0,
                )
            return ProgramRunScopeSnapshot(
                program_run_id=program_run_id,
                state=ProgramRunScopeState.UNKNOWN,
                active_operation_count=None,
                reason_code=f"program_run_stop_{state}",
            )
        try:
            active_count = sum(
                self._observation(task)[1].state is OwnerTaskState.ACTIVE for task in tasks
            )
            active_count += sum(
                self._observe_pick_place_task(task.sequence_id).state is OwnerTaskState.ACTIVE
                for task in pick_place_tasks
            )
        except Exception as exc:
            return ProgramRunScopeSnapshot(
                program_run_id=program_run_id,
                state=ProgramRunScopeState.UNKNOWN,
                active_operation_count=None,
                reason_code=(str(exc) or type(exc).__name__)[:256],
            )
        with self._lock:
            if program_run_id in self._scope_stops:
                return ProgramRunScopeSnapshot(
                    program_run_id,
                    ProgramRunScopeState.UNKNOWN,
                    None,
                    "program_run_stop_observation_changed",
                )
        return ProgramRunScopeSnapshot(
            program_run_id=program_run_id,
            state=(ProgramRunScopeState.ACTIVE if active_count else ProgramRunScopeState.SETTLED),
            active_operation_count=active_count,
        )

    def _request_stop(
        self,
        program_run_id: str,
        stop_request_id: str,
        reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        _text(stop_request_id, field="stop_request_id", maximum_bytes=2_048)
        with self._lock:
            existing_stop = self._scope_stops.get(program_run_id)
            request_owner_epoch = existing_stop.owner_epoch if existing_stop is not None else None
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
            pick_place_tasks = tuple(
                task
                for task in self._pick_place_tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        # Every distinct Stop reaches the owner: it refreshes the run fence
        # even when no further physical containment is necessary. A local
        # "already stopped" shortcut could let an older Resume erase a newer
        # Stop. Exact retry/no-effect semantics already belong to the Kernel.
        retained_count = len(tasks) + len(pick_place_tasks)
        # A zero local task count does not prove that the run is idle: its first
        # operation may be crossing the native Execute boundary. Always hand
        # the semantic Program Stop to the Kernel, whose current-run and
        # pending-admission capture can close that race without making Stop
        # wait for the ordinary command lane.
        # A delayed Starting Stop can arrive after Start has contained its
        # own aborted admission and retired its temporary context. Reuse that
        # exact Stop responder, without an ordinary-lane identity lookup.
        if request_owner_epoch is None:
            try:
                context = self._context(program_run_id)
                request_owner_epoch = context.request_owner_epoch
            except ProductionPortError:
                # Starting may precede tree binding. The Kernel can fence the
                # claimed run without any local task or operation existing.
                request_owner_epoch = _text(
                    (
                        self._current_owner_epoch()
                        if self._current_owner_epoch is not None
                        else _mapping(
                            self._kernel.identity().get("identity"),
                            field="execution identity",
                        ).get("owner_epoch")
                    ),
                    field="owner_epoch",
                )
        with self._lock:
            stop = self._scope_stops.get(program_run_id)
            if stop is None:
                stop = _ProgramScopeStop(request_owner_epoch)
                self._scope_stops[program_run_id] = stop
            # Retain a replacement responder only as Stop attribution, never
            # as a new execution binding or an ordinary-lane owner read.
            request_owner_epoch = stop.owner_epoch
            stop.pending += 1
            unresolved = stop.unresolved
        try:
            response = self._kernel.stop_program_run(
                {
                    "request_context": {
                        "request_id": _request_identity(
                            "program-scope-stop",
                            program_run_id,
                            stop_request_id,
                        ),
                        "request_owner_epoch": request_owner_epoch,
                        "source_label": "manyforge_behavior",
                        "correlation_id": program_run_id,
                    },
                    "program_run_id": program_run_id,
                    "reason": reason,
                }
            )
            accepted = _mapping(response.get("accepted"), field="Program-run Stop acceptance")
            response_owner_epoch = _text(
                response.get("owner_epoch"), field="Stop response owner_epoch"
            )
            if response_owner_epoch != request_owner_epoch:
                with self._lock:
                    if stop.owner_epoch == request_owner_epoch:
                        stop.owner_epoch = response_owner_epoch
                        stop.generation = 0
                        stop.active = None
                raise ProductionPortError("Program-run Stop responder incarnation changed")
            stop_id = _text(accepted.get("stop_id"), field="stop_id")
            stop_generation = accepted.get("stop_generation")
            if type(stop_generation) is not int or stop_generation < 1:
                raise ProductionPortError("Program-run Stop generation is invalid")
            with self._lock:
                if (
                    self._scope_stops.get(program_run_id) is stop
                    and stop.owner_epoch == request_owner_epoch
                    and stop_generation > stop.generation
                ):
                    stop.generation = stop_generation
                    stop.active = (stop_id, stop_generation, retained_count)
                    for task in tasks:
                        task.stop_requested = True
                    for task in pick_place_tasks:
                        task.scope_stop_requested = True
                elif stop.active is not None:
                    retained_count = stop.active[2]
                if (
                    stop.owner_epoch == request_owner_epoch
                    and stop.unresolved is unresolved
                    and accepted.get("replay_status") == "accepted_new"
                ):
                    # A subsequent explicit Stop recaptures an earlier unknown
                    # outcome. An old replay is not a new capture, nor may a
                    # failure during this request be cleared by its reply.
                    stop.unresolved = None
        except Exception:
            with self._lock:
                stop.unresolved = object()
            raise
        finally:
            with self._lock:
                stop.pending -= 1
                # Retain the watermark only while replies can still arrive.
                # Releasing a newer fence cannot let an older reply recreate
                # it, and completed handoffs leave no lifetime receipt map.
                if (
                    stop.pending == 0
                    and stop.active is None
                    and stop.unresolved is None
                    and self._scope_stops.get(program_run_id) is stop
                ):
                    self._scope_stops.pop(program_run_id)
        # Acceptance fenced the exact Kernel-owned run scope. Settlement is a
        # later observation; do not add a synchronous per-task read sweep here.
        return ProgramRunScopeSnapshot(
            program_run_id,
            ProgramRunScopeState.ACTIVE,
            retained_count,
        )

    @staticmethod
    def _resettable_operation(
        envelope: Mapping[str, Any],
        task: _OperationTask,
    ) -> tuple[Mapping[str, Any], bool]:
        operation = _mapping(envelope.get("operation"), field="native operation")
        if operation.get("operation_id") != task.operation_id:
            raise ProductionPortError("native operation changed identity")
        if operation.get("program_run_id") != task.identity.program_run_id:
            raise ProductionPortError("native operation changed Program run")
        resident = envelope.get("resident")
        if type(resident) is not bool:
            raise ProductionPortError("native operation residency is invalid")
        return operation, resident

    def _reset_scope(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        with self._lock:
            tasks = tuple(
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            )
        if not tasks:
            settled = self._finish_scope_stop(program_run_id, release_fence=False)
            if settled.state is not ProgramRunScopeState.SETTLED:
                raise ProductionPortError("Program-run Stop changed during Reset cleanup")
            with self._lock:
                self._pick_place_tasks = {
                    task_id: task
                    for task_id, task in self._pick_place_tasks.items()
                    if task.identity.program_run_id != program_run_id
                }
            return ProgramRunScopeSnapshot(
                program_run_id,
                ProgramRunScopeState.SETTLED,
                0,
            )

        status = _mapping(self._kernel.identity().get("identity"), field="execution identity")
        owner_epoch = _text(status.get("owner_epoch"), field="owner_epoch")
        for task in tasks:
            operation, resident = self._resettable_operation(
                self._kernel.operation(task.operation_id), task
            )
            if resident:
                state = _text(operation.get("state"), field="operation state")
                if state in _ACTIVE_STATES:
                    raise ProductionPortError(
                        "cannot reset a Program run with active native operations"
                    )
                revision = operation.get("operation_revision")
                if type(revision) is not int or revision < 1:
                    raise ProductionPortError("native operation revision is invalid")
                self._kernel.reset(
                    task.operation_id,
                    {
                        "request_context": {
                            "request_id": _request_identity(
                                "program-reset", program_run_id, task.operation_id
                            ),
                            "request_owner_epoch": owner_epoch,
                            "source_label": "manyforge_behavior",
                            "correlation_id": program_run_id,
                        },
                        "operation_id": task.operation_id,
                        "expected_operation_revision": revision,
                        "reason": "Program-cycle Reset released terminal work",
                    },
                )
                deadline = time.monotonic() + _PROGRAM_RESET_SETTLEMENT_TIMEOUT_S
                while True:
                    _operation, resident = self._resettable_operation(
                        self._kernel.operation(task.operation_id), task
                    )
                    if not resident:
                        break
                    if time.monotonic() >= deadline:
                        raise ProductionPortError("native operation Reset did not settle")
                    time.sleep(_PROGRAM_RESET_SETTLEMENT_POLL_S)
            with self._lock:
                self._tasks.pop(task.operation_id, None)

        settled = self._finish_scope_stop(program_run_id, release_fence=False)
        if settled.state is not ProgramRunScopeState.SETTLED:
            raise ProductionPortError("Program-run Stop changed during Reset cleanup")
        with self._lock:
            self._pick_place_tasks = {
                task_id: task
                for task_id, task in self._pick_place_tasks.items()
                if task.identity.program_run_id != program_run_id
            }

        return ProgramRunScopeSnapshot(
            program_run_id,
            ProgramRunScopeState.SETTLED,
            0,
        )

    def _finish_scope_stop(
        self,
        program_run_id: str,
        *,
        release_fence: bool,
        expected_stop: tuple[str, int] | None = None,
    ) -> ProgramRunScopeSnapshot:
        """Retire a proved handoff; only Resume reopens Kernel admission.

        Destructive cycle Reset keeps the existing whole-run fence through
        scene restoration. The parent claim's release retires that fence and
        capture; local receipt cleanup is not permission for fresh node work.
        """

        with self._lock:
            stop = self._scope_stops.get(program_run_id)
            if stop is not None and stop.pending:
                raise ProductionPortError("Program-run Stop requests are still in flight")
            if stop is not None and stop.unresolved is not None:
                raise ProductionPortError(
                    "Program-run Stop outcome must be reconciled before release"
                )
            scope_stop = stop.active if stop is not None else None
        if scope_stop is not None:
            stop_id, stop_generation, _retained_count = scope_stop
            if expected_stop is not None and expected_stop != (
                stop_id,
                stop_generation,
            ):
                raise ProductionPortError("Program-run Stop handoff is stale")
            status = _mapping(
                self._kernel.stop_status(stop_id).get("stop_status"),
                field="aggregate Stop status",
            )
            if status.get("state") != "succeeded":
                raise ProductionPortError("Program-run Stop cannot be released before settlement")
            if (
                type(status.get("stop_generation")) is not int
                or status["stop_generation"] != stop_generation
            ):
                raise ProductionPortError("Program-run Stop settlement generation is stale")
            if release_fence:
                self._kernel.release_program_run_stop_fence(
                    program_run_id,
                    stop_id,
                    stop_generation,
                )
            with self._lock:
                if self._scope_stops.get(program_run_id) is stop and stop.active == scope_stop:
                    stop.active = None
                    if stop.pending == 0 and stop.unresolved is None:
                        self._scope_stops.pop(program_run_id)
        return self._observe_scope(program_run_id)

    def _observe_task(self, task_id: str) -> OwnerTaskSnapshot:
        task = self._task(task_id)
        operation, snapshot = self._observation(task)
        if snapshot.state is OwnerTaskState.SUCCEEDED:
            revision = operation.get("operation_revision")
            if type(revision) is not int or revision < 1:
                raise ProductionPortError("native operation revision is invalid")
            with self._lock:
                current = self._tasks.get(task_id)
                if current is task:
                    current.completed_revision = revision
        return snapshot

    def _pick_place_task(self, task_id: str) -> _PickPlaceTask:
        with self._lock:
            try:
                return self._pick_place_tasks[task_id]
            except KeyError as exc:
                raise ProductionPortError("native pick/place task is not retained") from exc

    def _observe_pick_place_task(self, task_id: str) -> OwnerTaskSnapshot:
        task = self._pick_place_task(task_id)
        with self._lock:
            if task.snapshot.state is not OwnerTaskState.ACTIVE:
                return task.snapshot
        response = self._kernel.pick_place_sequence(task.sequence_id)
        status = _mapping(
            _mapping(response, field="pick/place response").get("sequence_status"),
            field="sequence_status",
        )
        snapshot = self._sequence_snapshot(task, status)
        if task.scope_stop_requested and snapshot.state in {
            OwnerTaskState.FAILED,
            OwnerTaskState.INTERRUPTED,
        }:
            operation_id = status.get("active_operation_id")
            if isinstance(operation_id, str) and operation_id:
                envelope = self._kernel.operation(operation_id)
                operation = dict(_mapping(envelope.get("operation"), field="operation"))
                if (
                    operation.get("operation_id") != operation_id
                    or operation.get("program_run_id") != task.identity.program_run_id
                ):
                    raise ProductionPortError("pick/place stopped operation changed identity")
                if operation.get("state") in _INTERRUPTED_STATES:
                    assessment = self._recovery_assessment(operation, operation_id)
                    if (
                        assessment is not None
                        and assessment.get("recovery_class") in PROGRAM_MOTION_STOP_RECOVERY_CLASSES
                    ):
                        metadata = dict(snapshot.metadata)
                        metadata.update(
                            {
                                "active_operation_id": operation_id,
                                "recovery_assessment": assessment,
                                "authored_failure_policy": "pause_for_operator",
                                "authored_failure_policy_parameter": ("operator_stop_policy"),
                            }
                        )
                        stop_id = operation.get("active_stop_id")
                        stop_generation = operation.get("active_stop_generation")
                        if (
                            isinstance(stop_id, str)
                            and stop_id
                            and type(stop_generation) is int
                            and stop_generation > 0
                        ):
                            metadata["active_stop_id"] = stop_id
                            metadata["active_stop_generation"] = stop_generation
                        snapshot = OwnerTaskSnapshot(
                            task_id=task.sequence_id,
                            state=OwnerTaskState.INTERRUPTED,
                            metadata=metadata,
                            reason_code=(f"operation_{operation.get('state')}"),
                            message="native pick/place sequence was interrupted",
                        )
        with self._lock:
            current = self._pick_place_tasks.get(task.sequence_id)
            if current is not task:
                raise ProductionPortError("pick/place task changed during observation")
            task.snapshot = snapshot
            return task.snapshot

    def _recover_pick_place_task(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Mapping[str, Any]:
        task = self._pick_place_task(task_id)
        if task.identity != identity:
            raise ProductionPortError("pick/place recovery changed node attempt")
        snapshot = self._observe_pick_place_task(task_id)
        operation_id = snapshot.metadata.get("active_operation_id")
        if (
            snapshot.state is not OwnerTaskState.INTERRUPTED
            or not isinstance(operation_id, str)
            or not operation_id
        ):
            raise ProductionPortError("pick/place recovery is not current")
        operation = dict(
            _mapping(
                self._kernel.operation(operation_id).get("operation"),
                field="operation",
            )
        )
        result = self._apply_operation_recovery(
            operation_id,
            identity,
            operation,
            snapshot,
            action=action,
            request_id=request_id,
            reason=reason,
        )
        if result.get("accepted") is True:
            with self._lock:
                self._pick_place_tasks.pop(task_id, None)
        return result

    def _cancel_pick_place_task(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
        *,
        reason: str | None = "Program node was invalidated",
    ) -> None:
        task = self._pick_place_task(task_id)
        if task.identity != identity:
            raise ProductionPortError("pick/place cancellation changed node attempt")
        with self._lock:
            if task.scope_stop_requested:
                return
        snapshot = self._observe_pick_place_task(task_id)
        if snapshot.state is not OwnerTaskState.ACTIVE:
            return
        status = _mapping(
            snapshot.metadata.get("sequence_status"),
            field="sequence_status",
        )
        operation_id = status.get("active_operation_id")
        if not isinstance(operation_id, str) or not operation_id:
            raise ProductionPortError("active pick/place sequence has no cancellable operation")
        with self._lock:
            if task.active_stop_operation_id == operation_id:
                return
            task.active_stop_operation_id = operation_id
        try:
            operation = dict(
                _mapping(
                    self._kernel.operation(operation_id).get("operation"),
                    field="operation",
                )
            )
            if (
                operation.get("operation_id") != operation_id
                or operation.get("program_run_id") != identity.program_run_id
            ):
                raise ProductionPortError("pick/place active operation changed identity")
            if operation.get("state") not in _ACTIVE_STATES:
                return
            revision = operation.get("operation_revision")
            generation = operation.get("active_stop_generation")
            if type(revision) is not int or revision < 1:
                raise ProductionPortError("native operation revision is invalid")
            if generation is None:
                generation = 0
            if type(generation) is not int or generation < 0:
                raise ProductionPortError("native Stop generation is invalid")
            context = self._context(identity.program_run_id)
            owner_epoch, _scene = self._owner_status(context)
            self._kernel.stop(
                {
                    "request_context": {
                        "request_id": _request_identity(
                            "program-pick-place-stop",
                            identity.program_run_id,
                            task.sequence_id,
                            operation_id,
                        ),
                        "request_owner_epoch": owner_epoch,
                        "source_label": "manyforge_behavior",
                        "correlation_id": identity.program_run_id,
                    },
                    "operations": [
                        {
                            "operation_id": operation_id,
                            "expected_operation_revision": revision,
                            "expected_stop_generation": generation,
                        }
                    ],
                    "reason": reason,
                }
            )
        except Exception:
            with self._lock:
                if task.active_stop_operation_id == operation_id:
                    task.active_stop_operation_id = None
            raise

    def _acknowledge_pick_place_task(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
    ) -> None:
        task = self._pick_place_task(task_id)
        if task.identity != identity:
            raise ProductionPortError("pick/place acknowledgement changed node attempt")
        snapshot = self._observe_pick_place_task(task_id)
        if snapshot.state is not OwnerTaskState.SUCCEEDED:
            raise ProductionPortError("only successful pick/place work can be acknowledged")
        with self._lock:
            self._pick_place_tasks.pop(task_id, None)


@dataclass(slots=True)
class _ControlTask:
    identity: NodeAttemptIdentity
    node_id: str
    query: Mapping[str, Any] | None = None
    operation: Mapping[str, Any] | None = None
    accepted: Mapping[str, Any] = field(default_factory=dict)


class _NativeKernelControlTaskPort:
    """One-shot controls; only pending observations are retained locally."""

    _CAPABILITIES = {
        "kernel.pause_operation": "execution.pause",
        "kernel.resume_operation": "execution.resume",
        "kernel.reset_operation": "execution.reset",
        "kernel.stop_operation": "execution.stop.soft",
        "kernel.set_runtime_scale": "execution.runtime_scale.operation_scoped",
        # Recovery authority is the exact current offer, not a capability.
        "recovery.apply_offered_action": None,
    }

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner
        self._pending: dict[str, _ControlTask] = {}

    def start(
        self, node_id: str, identity: NodeAttemptIdentity, inputs: OwnerNodeInputs
    ) -> OwnerTaskSnapshot:
        if node_id not in self._CAPABILITIES and node_id != "kernel.full_stop":
            raise ProductionPortError("Kernel control node is unsupported")
        task_id = _request_identity(
            "program-control", identity.program_run_id, identity.node_attempt_id
        )
        task = _ControlTask(identity, node_id)
        with self._owner._lock:
            if task_id in self._pending:
                raise ProductionPortError("control attempt was already submitted")
            if len(self._pending) >= _MAX_TASKS:
                raise ProductionPortError("too many pending Kernel control nodes")
            self._pending[task_id] = task
        try:
            context = self._owner._context(identity.program_run_id)
            caller = {
                "request_id": task_id,
                "request_owner_epoch": context.request_owner_epoch,
                "source_label": "manyforge_behavior",
                "correlation_id": identity.program_run_id,
            }
            kernel = self._owner._kernel
            if node_id == "kernel.full_stop":
                # Full Stop's unambiguous cell scope is resolved by the Kernel
                # even with stale attribution. Never gate its reserved lane on
                # an ordinary read merely to refresh diagnostic generation.
                response = kernel.full_stop(
                    {"request_context": caller, "expected_stop_generation": 0}
                )
            else:
                target = self._owner._target(
                    context,
                    _text(inputs.parameters.get("target_role"), field="target_role"),
                    self._CAPABILITIES[node_id],
                )
                task.query = {
                    "schema_version": "manyforge.program_operation_query.v1",
                    "owner_epoch": context.request_owner_epoch,
                    "program_binding_id": context.capture.program_binding_id,
                    "program_run_id": identity.program_run_id,
                    "target_role_id": target.target_role_id,
                }
                selected = kernel.query_operation(task.query)
                operation = dict(_mapping(selected.get("operation"), field="operation"))
                if (
                    selected.get("owner_epoch") != context.request_owner_epoch
                    or selected.get("target_role_id") != target.target_role_id
                    or operation.get("program_binding_id") != context.capture.program_binding_id
                    or operation.get("program_run_id") != identity.program_run_id
                ):
                    raise ProductionPortError("control context crossed its captured scope")
                task.operation = operation
                operation_id = _text(operation.get("operation_id"), field="operation_id")
                revision = operation.get("operation_revision")
                if type(revision) is not int or revision < 1:
                    raise ProductionPortError("control operation revision is invalid")
                if node_id == "recovery.apply_offered_action":
                    result = self._recover(task, inputs)
                    with self._owner._lock:
                        self._pending.pop(task_id, None)
                    return OwnerTaskSnapshot(
                        task_id,
                        (
                            OwnerTaskState.SUCCEEDED
                            if result.get("accepted") is True
                            else OwnerTaskState.FAILED
                        ),
                        metadata={
                            "operation_id": operation_id,
                            "recovery_result": result,
                        },
                        reason_code=(
                            None if result.get("accepted") is True else "recovery_rejected"
                        ),
                    )
                request = {
                    "request_context": caller,
                    "operation_id": operation_id,
                    "expected_operation_revision": revision,
                }
                if node_id == "kernel.stop_operation":
                    generation = operation.get("active_stop_generation")
                    response = kernel.stop(
                        {
                            "request_context": caller,
                            "operations": [
                                {
                                    "operation_id": operation_id,
                                    "expected_operation_revision": revision,
                                    "expected_stop_generation": (
                                        0 if generation is None else generation
                                    ),
                                }
                            ],
                            "reason": "Program Stop node",
                        }
                    )
                elif node_id == "kernel.set_runtime_scale":
                    response = kernel.set_operation_runtime_scale(
                        {
                            **request,
                            "expected_runtime_scale_generation": operation.get(
                                "runtime_scale_generation"
                            ),
                            "scale": _number(inputs.parameters.get("scale"), field="scale"),
                        }
                    )
                else:
                    method = {
                        "kernel.pause_operation": kernel.pause,
                        "kernel.resume_operation": kernel.resume,
                        "kernel.reset_operation": kernel.reset,
                    }[node_id]
                    response = method(
                        operation_id,
                        {
                            **request,
                            "reason": node_id,
                            "execution_scope": {
                                "node_instance_id": identity.node_instance_id,
                                "node_attempt_id": identity.node_attempt_id,
                            },
                        },
                    )
            task.accepted = dict(_mapping(response.get("accepted"), field="accepted control"))
            return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE)
        except Exception:
            with self._owner._lock:
                self._pending.pop(task_id, None)
            raise

    def _recover(self, task: _ControlTask, inputs: OwnerNodeInputs) -> Mapping[str, Any]:
        assert task.operation is not None
        operation = task.operation
        operation_id = _text(operation.get("operation_id"), field="operation_id")
        action = _text(inputs.parameters.get("recovery_action"), field="recovery_action")
        assessment = self._owner._recovery_assessment(operation, operation_id)
        if assessment is None:
            raise ProductionPortError("the exact current recovery offer is unavailable")
        actions = assessment.get("allowed_actions")
        if (
            action
            not in {
                "retry_motion_from_current_state",
                "acknowledge_and_continue",
                "retry_after_scene_repair",
                "replan_from_stopped_state",
            }
            or not isinstance(actions, list)
            or action not in actions
        ):
            raise ProductionPortError("the authored recovery action is not currently offered")
        if operation.get("state") in _INTERRUPTED_STATES:
            state = OwnerTaskState.INTERRUPTED
        elif operation.get("state") in _FAILED_STATES:
            state = OwnerTaskState.FAILED
        else:
            raise ProductionPortError("operation recovery is not current")
        result = self._owner._apply_operation_recovery(
            operation_id,
            task.identity,
            operation,
            OwnerTaskSnapshot(
                operation_id,
                state,
                metadata={"recovery_assessment": assessment},
                reason_code=f"operation_{operation['state']}",
            ),
            action=action,
            request_id=_request_identity(
                "program-control",
                task.identity.program_run_id,
                task.identity.node_attempt_id,
            ),
            reason="Program recovery node",
        )
        if result.get("accepted") is True:
            with self._owner._lock:
                previous = self._owner._tasks.get(operation_id)
                if (
                    previous is not None
                    and previous.identity.program_run_id == task.identity.program_run_id
                ):
                    self._owner._tasks.pop(operation_id, None)
        return result

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        try:
            return self._observe(task_id)
        except KernelOperationObservationUnavailableError as error:
            raise OwnerTaskObservationUnavailableError(str(error)) from error
        except Exception as error:
            if getattr(error, "code", None) in {
                "execution_owner_unavailable",
                "execution_operation_observation_changed",
            }:
                raise OwnerTaskObservationUnavailableError(str(error)) from error
            with self._owner._lock:
                self._pending.pop(task_id, None)
            raise

    def _observe(self, task_id: str) -> OwnerTaskSnapshot:
        with self._owner._lock:
            task = self._pending.get(task_id)
        if task is None:
            raise ProductionPortError("control task is not pending")
        kernel = self._owner._kernel
        state = OwnerTaskState.ACTIVE
        reason = None
        if task.node_id in {"kernel.stop_operation", "kernel.full_stop"}:
            stop_id = _text(task.accepted.get("stop_id"), field="stop_id")
            stop = _mapping(kernel.stop_status(stop_id).get("stop_status"), field="Stop status")
            if stop.get("stop_id") != stop_id or stop.get("stop_generation") != task.accepted.get(
                "stop_generation"
            ):
                raise ProductionPortError("control Stop observation changed identity")
            if stop.get("state") == "succeeded":
                state = OwnerTaskState.SUCCEEDED
            elif stop.get("state") not in {"accepted", "stopping", "certifying"}:
                state, reason = OwnerTaskState.FAILED, "stop_not_certified"
        elif task.node_id == "kernel.set_runtime_scale":
            response = kernel.query_operation(task.query)
            operation = _mapping(response.get("operation"), field="operation")
            scale = _mapping(response.get("runtime_scale"), field="runtime scale")
            if operation.get("operation_id") != task.operation["operation_id"] or scale.get(
                "generation"
            ) != task.accepted.get("runtime_scale_generation"):
                state, reason = OwnerTaskState.FAILED, "runtime_scale_superseded"
            elif scale.get("state") == "applied":
                state = OwnerTaskState.SUCCEEDED
            elif scale.get("state") != "pending":
                state, reason = OwnerTaskState.FAILED, "runtime_scale_not_applied"
        else:
            envelope = kernel.operation(task.operation["operation_id"])
            operation = _mapping(envelope.get("operation"), field="operation")
            if envelope.get("owner_epoch") != task.query["owner_epoch"]:
                raise ProductionPortError("control observation changed owner")
            if (
                operation.get("operation_id") != task.operation["operation_id"]
                or operation.get("program_run_id") != task.identity.program_run_id
                or operation.get("program_binding_id") != task.operation["program_binding_id"]
            ):
                raise ProductionPortError("control observation changed operation")
            last_control = _mapping(envelope.get("last_control"), field="last control")
            if last_control.get("request_id") != task_id or last_control.get(
                "operation_revision_after_acceptance"
            ) != task.accepted.get("operation_revision_after_acceptance"):
                state, reason = OwnerTaskState.FAILED, "operation_control_superseded"
            elif envelope.get("control_pending") is True:
                pass  # observed Paused/Executing is not completed provider work
            elif envelope.get("control_pending") is not False:
                raise ProductionPortError("control completion is unavailable")
            elif task.node_id == "kernel.reset_operation":
                if envelope.get("resident") is False:
                    state = OwnerTaskState.SUCCEEDED
                else:
                    state, reason = OwnerTaskState.FAILED, "operation_reset_failed"
            elif task.node_id == "kernel.pause_operation" and operation.get("state") in {
                "paused",
                "completed",
            }:
                state = OwnerTaskState.SUCCEEDED
            elif task.node_id == "kernel.resume_operation" and operation.get("state") in {
                "executing",
                "completed",
            }:
                state = OwnerTaskState.SUCCEEDED
            elif operation.get("state") not in _ACTIVE_STATES:
                state, reason = OwnerTaskState.FAILED, "operation_control_interrupted"
        if state is not OwnerTaskState.ACTIVE:
            with self._owner._lock:
                self._pending.pop(task_id, None)
        return OwnerTaskSnapshot(task_id, state, reason_code=reason)

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        # Consumption of a control result is not an operation Reset or ACK.
        if task_id != _request_identity(
            "program-control", identity.program_run_id, identity.node_attempt_id
        ):
            raise ProductionPortError("control result consumption changed node attempt")

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self.acknowledge(task_id, identity)
        # Invalidation cancels only observation. The Kernel retains accepted
        # effects; the Program scope's independent Stop/reset path settles them.
        with self._owner._lock:
            self._pending.pop(task_id, None)


class _NativeTargetQueryPort:
    """On-demand native observations; no client-side model or gripper state."""

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner

    def invoke(
        self, node_id: str, identity: NodeAttemptIdentity, inputs: OwnerNodeInputs
    ) -> OwnerInvocationResult:
        if node_id not in {
            "model.get_frame_pose",
            "gripper.check_status",
            "gripper.wait_for_status",
        }:
            raise ProductionPortError("target query node is unsupported")
        context = self._owner._context(identity.program_run_id)
        frame_query = node_id == "model.get_frame_pose"
        confirmation = inputs.parameters.get("expected_state") == "grasp_confirmed"
        target = self._owner._target(
            context,
            _text(inputs.parameters.get("target_role"), field="target_role"),
            (
                "model.kinematics.fk"
                if frame_query
                else (
                    "tool.gripper.confirmation.operator"
                    if confirmation
                    else "tool.gripper.feedback"
                )
            ),
        )
        query = {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": context.request_owner_epoch,
            "program_binding_id": context.capture.program_binding_id,
            "target_role_id": target.target_role_id,
            "kind": (
                "frame_pose"
                if frame_query
                else ("gripper_confirmation" if confirmation else "gripper_status")
            ),
        }
        if frame_query:
            query["frame_role_id"] = _text(inputs.parameters.get("frame_role"), field="frame_role")
        response = self._owner._kernel.query_target(query)
        observation = _mapping(response.get("target_observation"), field="target_observation")
        expected = {**query, "schema_version": "manyforge.target_observation.v1"}
        if any(observation.get(key) != value for key, value in expected.items()):
            raise ProductionPortError("native target observation crossed the captured query")
        metadata = {key: observation[key] for key in ("source_epoch", "source_sequence")}
        value = _mapping(observation.get("value"), field="target observation value")
        if frame_query:
            position = _mapping(value.get("position_m"), field="frame position")
            orientation = _mapping(value.get("orientation"), field="frame orientation")
            return OwnerInvocationResult(
                True,
                output={
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [
                        _number(position.get(axis), field=axis) for axis in ("x_m", "y_m", "z_m")
                    ],
                    "orientation": {
                        "xyzw": [
                            _number(orientation.get(axis), field=axis)
                            for axis in ("x", "y", "z", "w")
                        ]
                    },
                },
                metadata=metadata,
            )
        expected_state = inputs.parameters.get("expected_state")
        metadata["command_scope"] = tuple(
            value[key]
            for key in (
                "command_id",
                "generation",
                "source_id",
                "source_revision",
                "source_epoch",
                "active_control_configuration_id",
                "active_control_configuration_generation",
            )
        )
        metadata["command_state"] = value["state"]
        if expected_state == "grasp_confirmed":
            confirmed = value.get("grasp_confirmed")
            if confirmed is None:
                return OwnerInvocationResult(
                    False,
                    metadata=metadata,
                    reason_code="gripper_status_not_matched",
                    message="Waiting for current qualified operator confirmation.",
                )
            evidence = _mapping(value.get("evidence"), field="grasp evidence")
            # Headless and Runner share the qualified host monotonic clock.
            # Recheck at consumption: the HTTP read may outlive this proof
            # even while the node's longer wait deadline is still valid.
            if not (
                nonnegative_int64(evidence.get("observed_at_ns"))
                <= time.monotonic_ns()
                < nonnegative_int64(evidence.get("expires_at_ns"))
            ):
                return OwnerInvocationResult(
                    False,
                    metadata=metadata,
                    reason_code="grasp_confirmation_not_current",
                    message="Grasp evidence is not current at consumption.",
                )
            if confirmed is False:
                return OwnerInvocationResult(
                    False,
                    metadata=metadata,
                    reason_code="grasp_not_confirmed",
                    message="The qualified source explicitly did not confirm the grasp.",
                )
            matched = confirmed is True
        else:
            expected_native = {
                "idle": "idle",
                "moving": "executing",
                "reached": "completed",
                "failed": "failed",
            }
            if expected_state not in expected_native:
                raise ProductionPortError("gripper expected state is unsupported")
            matched = value.get("state") == expected_native[expected_state]
        return OwnerInvocationResult(
            matched,
            metadata=metadata,
            reason_code=None if matched else "gripper_status_not_matched",
        )


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

    def recover(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Mapping[str, Any]:
        return self._owner._recover_task(
            task_id,
            identity,
            action=action,
            request_id=request_id,
            reason=reason,
        )


class _NativePickPlaceTaskPort:
    """OwnerTaskPort view over native pick/place planning and sequencing."""

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        return self._owner._start_pick_place(node_id, identity, inputs)

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return self._owner._observe_pick_place_task(task_id)

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._owner._cancel_pick_place_task(task_id, identity)

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._owner._acknowledge_pick_place_task(task_id, identity)

    def recover(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Mapping[str, Any]:
        return self._owner._recover_pick_place_task(
            task_id,
            identity,
            action=action,
            request_id=request_id,
            reason=reason,
        )


class _NativeKernelProgramScopePort:
    """Unambiguous ProgramRunScopePort view over one native Program client."""

    def __init__(self, owner: NativeKernelProgramPort) -> None:
        self._owner = owner

    def request_stop(
        self,
        program_run_id: str,
        stop_request_id: str,
        reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        return self._owner._request_stop(
            program_run_id,
            stop_request_id,
            reason,
        )

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return self._owner._observe_scope(program_run_id)

    def execution_scope(self, program_run_id: str) -> Mapping[str, Any]:
        context = self._owner._context(program_run_id)
        scope = self._owner._kernel.read_program_execution_scope(program_run_id)
        if scope.get("request_owner_epoch") != context.request_owner_epoch:
            raise ProductionPortError("Program execution scope owner changed")
        return scope

    def stop_selection(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        return self._owner._kernel.stop_program_selection(request)

    def release_selection(
        self, request: Mapping[str, Any], expected_selection_revision: int
    ) -> Mapping[str, Any]:
        return self._owner._kernel.release_program_selection(request, expected_selection_revision)

    def capture_stop(self, program_run_id: str) -> tuple[str, int] | None:
        with self._owner._lock:
            stop = self._owner._scope_stops.get(program_run_id)
            if stop is not None and stop.pending:
                raise ProductionPortError("Program-run Stop requests are still in flight")
            if stop is not None and stop.unresolved is not None:
                raise ProductionPortError("Program-run Stop outcome is unknown")
            if stop is None or stop.active is None:
                return None
            return stop.active[:2]

    def release_stop(
        self, program_run_id: str, *, expected_stop: tuple[str, int] | None = None
    ) -> ProgramRunScopeSnapshot:
        return self._owner._finish_scope_stop(
            program_run_id, release_fence=True, expected_stop=expected_stop
        )

    def reset(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return self._owner._reset_scope(program_run_id)


class HeadlessSceneProgramPort:
    """Stateless scene intents plus captured Program-role translation.

    The native logical-scene owner remains the only scene authority. This port
    keeps only immutable Program Binding context and reads fresh owner truth for
    every preparation, verification, or node attempt.
    """

    def __init__(
        self,
        transport: _SceneProgramTransport,
        *,
        program_node_evidence: (
            Callable[[NodeAttemptIdentity, str], Mapping[str, Any]] | None
        ) = None,
    ) -> None:
        for method in (
            "authoring_admission",
            "scene",
            "commit_scene_transaction",
            "manager_graph",
        ):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"scene transport has no callable {method}")
        self._transport = transport
        self._program_node_evidence = program_node_evidence
        if program_node_evidence is not None and not callable(
            getattr(transport, "scene_transaction_status", None)
        ):
            raise TypeError("runtime Scene transport has no exact result lookup")
        self._scene = SceneClient(transport)
        self._contexts: dict[str, _SceneBoundContext] = {}
        self._prepared: dict[str, dict[str, Any]] = {}
        self._lock = RLock()

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("bind requires one exact Program-run port context")
        parsed = self._parse_context(context)
        with self._lock:
            current = self._contexts.get(context.program_run_id)
            if current is not None:
                if current != parsed:
                    raise ProductionPortError("Program-run scene context was rebound")
                return
            if len(self._contexts) >= _MAX_CONTEXTS:
                raise ProductionPortError("Program-run scene-context retention is full")
            self._contexts[context.program_run_id] = parsed

    def unbind(self, program_run_id: str) -> None:
        with self._lock:
            self._contexts.pop(program_run_id, None)

    @staticmethod
    def _parse_context(context: ProgramRunPortContext) -> _SceneBoundContext:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("scene context must be one Program-run port context")
        binding = context.program_binding
        component_rows = _sequence(
            binding.get("component_role_bindings"),
            field="component_role_bindings",
        )
        component_models: dict[tuple[str, str], tuple[str, str]] = {}
        component_ids: dict[str, str] = {}
        for index, raw_row in enumerate(component_rows):
            row = _mapping(raw_row, field=f"component_role_bindings[{index}]")
            role = _text(
                row.get("component_role_id"),
                field=f"component_role_bindings[{index}].component_role_id",
            )
            component_id = _text(
                row.get("controllable_component_id"),
                field=(f"component_role_bindings[{index}].controllable_component_id"),
            )
            if role in component_ids:
                raise ProductionPortError("Program Binding repeats a component role")
            component_ids[role] = component_id
            models = _sequence(
                row.get("model_role_bindings"),
                field=f"component_role_bindings[{index}].model_role_bindings",
            )
            for model_index, raw_model in enumerate(models):
                model = _mapping(
                    raw_model,
                    field=(
                        f"component_role_bindings[{index}]." f"model_role_bindings[{model_index}]"
                    ),
                )
                model_role = _text(model.get("model_role_id"), field="model_role_id")
                key = (role, model_role)
                value = (
                    _text(model.get("model_instance_id"), field="model_instance_id"),
                    _text(model.get("model_revision"), field="model_revision"),
                )
                if key in component_models:
                    raise ProductionPortError("Program Binding repeats a component/model role")
                component_models[key] = value

        target_rows = _sequence(binding.get("target_role_bindings"), field="target_role_bindings")
        targets: dict[str, _SceneTargetBinding] = {}
        for index, raw_row in enumerate(target_rows):
            row = _mapping(raw_row, field=f"target_role_bindings[{index}]")
            target_role = _text(
                row.get("target_role_id"),
                field=f"target_role_bindings[{index}].target_role_id",
            )
            component_role = _text(
                row.get("component_role_id"),
                field=f"target_role_bindings[{index}].component_role_id",
            )
            component_id = _text(
                row.get("controllable_component_id"),
                field=f"target_role_bindings[{index}].controllable_component_id",
            )
            if component_ids.get(component_role) != component_id:
                raise ProductionPortError("target and component role bindings disagree")
            frames: dict[str, tuple[str, str]] = {}
            for raw_frame in _sequence(row.get("frame_role_bindings"), field="frame_role_bindings"):
                frame = _mapping(raw_frame, field="frame_role_binding")
                frame_role = _text(frame.get("frame_role_id"), field="frame_role_id")
                value = (
                    _text(frame.get("model_instance_id"), field="model_instance_id"),
                    _text(frame.get("model_revision"), field="model_revision"),
                )
                if frame_role in frames:
                    raise ProductionPortError(f"target role {target_role!r} repeats a frame role")
                frames[frame_role] = value
            body_roles: list[str] = []
            for raw_body in _sequence(
                row.get("collision_body_role_bindings"),
                field="collision_body_role_bindings",
            ):
                body = _mapping(raw_body, field="collision_body_role_binding")
                body_roles.append(
                    _text(
                        body.get("collision_body_role_id"),
                        field="collision_body_role_id",
                    )
                )
            if len(body_roles) != len(set(body_roles)):
                raise ProductionPortError(
                    f"target role {target_role!r} repeats a collision-body role"
                )
            if target_role in targets:
                raise ProductionPortError("Program Binding repeats a target role")
            targets[target_role] = _SceneTargetBinding(
                target_role_id=target_role,
                component_role_id=component_role,
                controllable_component_id=component_id,
                command_target_id=_text(row.get("command_target_id"), field="command_target_id"),
                frames=frames,
                collision_body_roles=frozenset(body_roles),
            )

        component_frames: dict[tuple[str, str, str], dict[str, str]] = {}
        for (component_role, model_role), (
            model_instance_id,
            model_revision,
        ) in component_models.items():
            component_id = component_ids[component_role]
            for target in targets.values():
                if target.controllable_component_id != component_id:
                    continue
                for frame_role, frame_binding in target.frames.items():
                    if frame_binding[0] != model_instance_id:
                        continue
                    if frame_binding[1] != model_revision:
                        raise ProductionPortError(
                            "component and target frame model revisions disagree"
                        )
                    key = (component_role, model_role, frame_role)
                    frame = {
                        "kind": "component_frame",
                        "controllable_component_id": component_id,
                        "model_instance_id": model_instance_id,
                        "frame_role_id": frame_role,
                    }
                    previous = component_frames.get(key)
                    if previous is not None and previous != frame:
                        raise ProductionPortError(
                            "Program Binding gives an ambiguous component frame"
                        )
                    component_frames[key] = frame
        return _SceneBoundContext(
            capture=context,
            component_models=component_models,
            component_frames=component_frames,
            targets=targets,
        )

    @staticmethod
    def _target(context: _SceneBoundContext, target_role_id: str) -> _SceneTargetBinding:
        try:
            return context.targets[target_role_id]
        except KeyError as exc:
            raise ProductionPortError(
                f"Program Binding has no target role {target_role_id!r}"
            ) from exc

    def _context(self, program_run_id: str) -> _SceneBoundContext:
        with self._lock:
            try:
                return self._contexts[program_run_id]
            except KeyError as exc:
                raise ProductionPortError("Program-run scene context is not initialized") from exc

    def _read_view(self, context: _SceneBoundContext) -> _SceneView:
        projection = self._scene.snapshot()
        logical = _mapping(projection.get("logical_scene"), field="logical_scene")
        if (
            logical.get("available") is not True
            or logical.get("freshness") != "fresh"
            or list(_sequence(logical.get("known_gaps"), field="known_gaps"))
        ):
            raise ProductionPortError("logical-scene owner truth is not fresh")
        owner_epoch = _text(logical.get("source_owner_epoch"), field="scene source_owner_epoch")
        payload = _mapping(logical.get("payload"), field="logical_scene.payload")
        if payload.get("api_version") != "manyforge.logical_scene_source.v1":
            raise ProductionPortError("logical-scene payload version is unsupported")
        access = _mapping(payload.get("access"), field="logical_scene.access")
        if (
            access.get("scene_writes_open") is not True
            or access.get("projection_leases_open") is not True
        ):
            raise ProductionPortError("logical-scene writes are fenced")
        active = dict(
            _mapping(
                access.get("active_projection"),
                field="logical_scene.active_projection",
            )
        )
        if (
            active.get("retained_bundle_id") != context.capture.retained_bundle_id
            or active.get("graph_revision") != context.capture.graph_revision
        ):
            raise ProductionPortError(
                "logical-scene owner and captured Program deployment disagree"
            )
        active_scene = _mapping(active.get("scene_revision"), field="active scene revision")
        _text(active_scene.get("scene_id"), field="scene_id")
        _text(active_scene.get("content_hash"), field="scene content_hash")
        if type(active_scene.get("revision_id")) is not int or active_scene["revision_id"] < 1:
            raise ProductionPortError("active scene revision is invalid")
        snapshot = _mapping(payload.get("snapshot"), field="logical_scene.snapshot")
        if dict(_mapping(snapshot.get("revision"), field="snapshot revision")) != dict(
            active_scene
        ):
            raise ProductionPortError("logical-scene snapshot and active projection disagree")
        cell_id = _text(snapshot.get("cell_id"), field="cell_id")
        objects: dict[str, Mapping[str, Any]] = {}
        object_ids: list[str] = []
        for index, raw_object in enumerate(
            _sequence(snapshot.get("objects"), field="scene objects")
        ):
            scene_object = _mapping(raw_object, field=f"scene objects[{index}]")
            scene_resource_id = _text(
                scene_object.get("scene_resource_id"), field="scene_resource_id"
            )
            object_ids.append(scene_resource_id)
            objects[scene_resource_id] = scene_object
        if object_ids != sorted(object_ids) or len(object_ids) != len(objects):
            raise ProductionPortError("logical-scene objects are not canonical")
        attachments: list[Mapping[str, Any]] = []
        attachment_ids: list[str] = []
        for index, raw_attachment in enumerate(
            _sequence(
                snapshot.get("carried_object_attachments"),
                field="carried_object_attachments",
            )
        ):
            attachment = _mapping(raw_attachment, field=f"carried_object_attachments[{index}]")
            attachment_ids.append(
                _text(
                    attachment.get("carried_object_attachment_id"),
                    field="carried_object_attachment_id",
                )
            )
            attachments.append(attachment)
        if attachment_ids != sorted(attachment_ids) or len(attachment_ids) != len(
            set(attachment_ids)
        ):
            raise ProductionPortError("logical-scene attachments are not canonical")
        return _SceneView(owner_epoch, active, cell_id, objects, tuple(attachments))

    def _commit(
        self,
        *,
        context: _SceneBoundContext,
        request_id: str,
        correlation_id: str,
        request_owner_epoch: str,
        expected_projection_revision: Mapping[str, Any],
        operation: Mapping[str, Any],
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
        program_node_evidence: Mapping[str, Any] | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        body: dict[str, Any] = {
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": request_owner_epoch,
                "source_label": "manyforge.behavior.program_scene",
                "correlation_id": correlation_id,
            },
            "expected_projection_revision": dict(expected_projection_revision),
            "operation": dict(operation),
        }
        if program_node_evidence is not None:
            body["program_node_evidence"] = dict(program_node_evidence)
        if operation.get("kind") in _AUTHORED_SCENE_OPERATIONS or program_node_evidence is not None:
            if program_reset_scene_authorization is not None:
                authorization = dict(program_reset_scene_authorization)
                if set(authorization) != {
                    "program_runner_owner_epoch",
                    "program_run_id",
                    "cycle_claim_generation",
                    "terminal_or_settlement_reference",
                }:
                    raise ProductionPortError("Program Reset scene authorization is malformed")
                _text(
                    authorization["program_runner_owner_epoch"],
                    field="program_runner_owner_epoch",
                )
                _text(
                    authorization["program_run_id"],
                    field="program_run_id",
                )
                generation = authorization["cycle_claim_generation"]
                if type(generation) is not int or not 1 <= generation <= 9_007_199_254_740_991:
                    raise ProductionPortError("Program Reset scene claim generation is invalid")
                reference = _text(
                    authorization["terminal_or_settlement_reference"],
                    field="terminal_or_settlement_reference",
                )
                if (
                    len(reference) != 71
                    or not reference.startswith("sha256:")
                    or any(character not in "0123456789abcdef" for character in reference[7:])
                ):
                    raise ProductionPortError("Program Reset scene settlement reference is invalid")
                body["program_reset_scene_authorization"] = authorization
                body["request_context"]["request_id"] = _request_identity(
                    "scene-reset", request_id, str(generation)
                )
            else:
                admission = self._transport.authoring_admission()
                generation = admission.get("admission_generation")
                if type(generation) is not int or not 1 <= generation <= 9_007_199_254_740_991:
                    raise ProductionPortError(
                        "authoring-admission owner omitted its current generation"
                    )
                body["expected_admission_generation"] = generation
                # A no-effect stale-admission rejection is durably replayed by the
                # logical-scene owner. Include the observed generation so a later
                # retry cannot be trapped behind that obsolete outcome.
                body["request_context"]["request_id"] = _request_identity(
                    "scene-admission", request_id, str(generation)
                )
        try:
            response = self._scene.commit(body)
        except SceneOutcomeUnknownError as error:
            if program_node_evidence is None:
                raise

            # Retain this one exact call in its node adapter. A later tick only
            # reads its result; it never sends the transaction a second time.
            def observe() -> OwnerInvocationResult | None:
                try:
                    response = self._scene.receipt(body)
                except SceneTransactionRejectedError as rejected:
                    self._raise_if_stop_prevented_admission(rejected)
                    return OwnerInvocationResult(
                        False, reason_code=rejected.code, message=str(rejected)
                    )
                if response is None:
                    return None
                committed = self._committed_revision(
                    context, expected_projection_revision, response
                )
                return OwnerInvocationResult(
                    True,
                    output={"committed_projection_revision": committed},
                    metadata={
                        "transaction_sequence": response.get("transaction_sequence"),
                        "update_status": response.get("update_status"),
                    },
                )

            raise OwnerInvocationPendingError(
                "Scene transaction outcome is unconfirmed", observe
            ) from error
        return self._committed_revision(context, expected_projection_revision, response), response

    @staticmethod
    def _raise_if_stop_prevented_admission(error: SceneTransactionRejectedError) -> None:
        details = error.response["error"].get("details", {})
        if isinstance(details, dict) and details.get("reason") == "program_node_stop_fence_active":
            revision = details.get("selection_revision")
            if isinstance(revision, str) and revision.isdecimal() and len(revision) <= 16:
                raise OwnerInvocationHeldError(int(revision)) from error

    @staticmethod
    def _committed_revision(
        context: _SceneBoundContext,
        expected_projection_revision: Mapping[str, Any],
        response: Mapping[str, Any],
    ) -> dict[str, Any]:
        committed = dict(
            _mapping(
                response.get("committed_projection_revision"),
                field="committed_projection_revision",
            )
        )
        expected_scene = _mapping(
            expected_projection_revision.get("scene_revision"),
            field="expected scene revision",
        )
        committed_scene = _mapping(
            committed.get("scene_revision"), field="committed scene revision"
        )
        if (
            committed.get("retained_bundle_id") != context.capture.retained_bundle_id
            or committed.get("graph_revision") != context.capture.graph_revision
            or committed_scene.get("scene_id") != expected_scene.get("scene_id")
        ):
            raise ProductionPortError(
                "scene commit changed captured structural or lineage identity"
            )
        return committed

    @staticmethod
    def _initial_pose_and_frame(
        context: _SceneBoundContext,
        raw_pose: object,
        *,
        cell_id: str,
        field: str,
    ) -> tuple[dict[str, Any], dict[str, str]]:
        pose = _mapping(raw_pose, field=field)
        frame = _mapping(pose.get("frame_ref"), field=f"{field}.frame_ref")
        kind = frame.get("kind")
        if kind == "cell_frame" and set(frame) == {"kind"}:
            frame_ref = {"kind": "cell_frame", "cell_id": cell_id}
        elif kind == "component_frame" and set(frame) == {
            "kind",
            "component_role",
            "model_role",
            "frame_role",
        }:
            key = (
                _text(frame["component_role"], field="component_role"),
                _text(frame["model_role"], field="model_role"),
                _text(frame["frame_role"], field="frame_role"),
            )
            try:
                frame_ref = dict(context.component_frames[key])
            except KeyError as exc:
                raise ProductionPortError(
                    "Program Binding has no exact initial-scene component frame"
                ) from exc
        else:
            raise ProductionPortError("initial scene pose has an invalid frame")
        return _scene_pose3(pose, field=field), frame_ref

    def _node_pose_and_frame(
        self,
        context: _SceneBoundContext,
        view: _SceneView,
        raw_pose: object,
        *,
        field: str,
    ) -> tuple[dict[str, Any], dict[str, str]]:
        pose = _mapping(raw_pose, field=field)
        frame = _mapping(pose.get("frame_ref"), field=f"{field}.frame_ref")
        converted = _scene_pose3(pose, field=field)
        kind = frame.get("kind")
        if kind == "cell_frame" and set(frame) == {"kind"}:
            return converted, {"kind": "cell_frame", "cell_id": view.cell_id}
        if kind == "target_frame" and set(frame) == {
            "kind",
            "target_role",
            "frame_role",
        }:
            target = self._target(context, _text(frame["target_role"], field="target_role"))
            frame_role = _text(frame["frame_role"], field="frame_role")
            try:
                model_instance_id, _model_revision = target.frames[frame_role]
            except KeyError as exc:
                raise ProductionPortError("Program Binding has no exact target frame") from exc
            return converted, {
                "kind": "component_frame",
                "controllable_component_id": target.controllable_component_id,
                "model_instance_id": model_instance_id,
                "frame_role_id": frame_role,
            }
        if kind == "scene_resource" and set(frame) == {
            "kind",
            "scene_resource_id",
        }:
            reference_id = _text(frame["scene_resource_id"], field="scene_resource_id")
            return converted, {
                "kind": "scene_resource",
                "scene_resource_id": reference_id,
            }
        raise ProductionPortError("scene pose has an unsupported frame")

    @staticmethod
    def _removal_order(objects: Mapping[str, Mapping[str, Any]]) -> tuple[str, ...]:
        dependent_count = {scene_resource_id: 0 for scene_resource_id in objects}
        supports_by_object: dict[str, tuple[str, ...]] = {}
        for scene_resource_id, scene_object in objects.items():
            supports: list[str] = []
            for raw_contact in _sequence(
                scene_object.get("support_contacts", ()),
                field="support_contacts",
            ):
                contact = _mapping(raw_contact, field="support_contact")
                support_id = _text(contact.get("support_object_id"), field="support_object_id")
                if support_id not in dependent_count:
                    raise ProductionPortError(
                        "logical scene contains an unknown support dependency"
                    )
                supports.append(support_id)
            if len(supports) != len(set(supports)):
                raise ProductionPortError("logical scene contains a duplicate support dependency")
            supports_by_object[scene_resource_id] = tuple(supports)
            for support_id in supports:
                dependent_count[support_id] += 1

        removable = [
            scene_resource_id for scene_resource_id, count in dependent_count.items() if count == 0
        ]
        heapify(removable)
        ordered: list[str] = []
        while removable:
            scene_resource_id = heappop(removable)
            ordered.append(scene_resource_id)
            for support_id in supports_by_object[scene_resource_id]:
                dependent_count[support_id] -= 1
                if dependent_count[support_id] == 0:
                    heappush(removable, support_id)
        if len(ordered) != len(objects):
            raise ProductionPortError("logical scene contains a cyclic support dependency")
        return tuple(ordered)

    def _component_attachment_revisions(
        self, context: _SceneBoundContext, component_id: str
    ) -> list[str]:
        projection = self._transport.manager_graph()
        if projection.get("schema_version") != "manyforge.manager_graph_projection.v1":
            raise ProductionPortError("Manager graph projection is unsupported")
        source = _mapping(projection.get("source"), field="manager graph source")
        if (
            source.get("available") is not True
            or source.get("freshness") != "fresh"
            or list(_sequence(source.get("known_gaps"), field="known_gaps"))
        ):
            raise ProductionPortError("Manager graph is not fresh")
        if (
            projection.get("retained_bundle_id") != context.capture.retained_bundle_id
            or projection.get("graph_revision") != context.capture.graph_revision
        ):
            raise ProductionPortError("Manager graph and captured Program deployment disagree")
        graph = _mapping(projection.get("cell_graph"), field="cell_graph")
        component_ids = {
            _text(
                _mapping(row, field="controllable_component").get("controllable_component_id"),
                field="controllable_component_id",
            )
            for row in _sequence(
                graph.get("controllable_components"),
                field="controllable_components",
            )
        }
        if component_id not in component_ids:
            raise ProductionPortError("captured carrier component is absent from the Manager graph")
        parent_by_child: dict[str, tuple[str, str]] = {}
        for raw_edge in _sequence(
            graph.get("component_attachments"), field="component_attachments"
        ):
            edge = _mapping(raw_edge, field="component_attachment")
            child = _mapping(edge.get("child"), field="component_attachment.child")
            parent = _mapping(edge.get("parent"), field="component_attachment.parent")
            child_id = _text(
                child.get("controllable_component_id"),
                field="child.controllable_component_id",
            )
            value = (
                _text(
                    parent.get("controllable_component_id"),
                    field="parent.controllable_component_id",
                ),
                _text(
                    edge.get("component_attachment_revision"),
                    field="component_attachment_revision",
                ),
            )
            if child_id in parent_by_child:
                raise ProductionPortError("Manager graph gives a component more than one parent")
            parent_by_child[child_id] = value
        revisions: list[str] = []
        visited: set[str] = set()
        current = component_id
        while current in parent_by_child:
            if current in visited:
                raise ProductionPortError("Manager component attachments are cyclic")
            visited.add(current)
            current, revision = parent_by_child[current]
            revisions.append(revision)
        if len(revisions) != len(set(revisions)):
            raise ProductionPortError("Manager component attachment revisions are not unique")
        return sorted(revisions)

    @staticmethod
    def _desired_scene_digest(
        context: _SceneBoundContext,
        operations: Sequence[Mapping[str, Any]],
    ) -> str:
        encoded = json.dumps(
            {
                "applied_record_digest": context.capture.applied_record_digest,
                "program_revision": context.capture.program_revision,
                "program_binding_id": context.capture.program_binding_id,
                "retained_bundle_id": context.capture.retained_bundle_id,
                "graph_revision": context.capture.graph_revision,
                "objects": [dict(operation) for operation in operations],
            },
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        return f"sha256:{sha256(encoded).hexdigest()}"

    @staticmethod
    def _object_matches_operation(
        view: _SceneView,
        operation: Mapping[str, Any],
    ) -> bool:
        scene_resource_id = _text(operation.get("scene_resource_id"), field="scene_resource_id")
        current = view.objects.get(scene_resource_id)
        if current is None or (
            list(_sequence(current.get("collision_shapes"), field="collision_shapes"))
            != [operation.get("shape")]
            or list(
                _sequence(
                    current.get("support_contacts", ()),
                    field="support_contacts",
                )
            )
            != operation.get("support_contacts")
            or current.get("provider_evidence") is not None
        ):
            return False
        frame_ref = _mapping(operation.get("frame_ref"), field="frame_ref")
        # Cell-frame intent has the same normalized representation in the
        # owner snapshot. Component-frame intent is resolved by the native
        # owner, so only an owner-issued prepared proof can compare it.
        if frame_ref.get("kind") != "cell_frame":
            return False
        return current.get("placement") == {
            "kind": "cell_fixed",
            "cell_id": view.cell_id,
            "pose_in_cell": operation.get("pose"),
        }

    @staticmethod
    def _scene_matches_operations(
        view: _SceneView,
        operations: Sequence[Mapping[str, Any]],
    ) -> bool:
        if view.attachments:
            return False
        expected_ids = tuple(
            _text(operation.get("scene_resource_id"), field="scene_resource_id")
            for operation in operations
        )
        return bool(
            tuple(view.objects) == tuple(sorted(expected_ids))
            and all(
                HeadlessSceneProgramPort._object_matches_operation(view, operation)
                for operation in operations
            )
        )

    @staticmethod
    def _proof(
        context: _SceneBoundContext,
        view: _SceneView,
        desired_scene_digest: str,
    ) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.prepared_program_scene.v1",
            "applied_record_digest": context.capture.applied_record_digest,
            "program_revision": context.capture.program_revision,
            "program_binding_id": context.capture.program_binding_id,
            "retained_bundle_id": context.capture.retained_bundle_id,
            "graph_revision": context.capture.graph_revision,
            "desired_scene_digest": desired_scene_digest,
            "scene_owner_epoch": view.request_owner_epoch,
            "projection_revision": deepcopy(view.projection_revision),
        }

    @staticmethod
    def _proof_matches(
        context: _SceneBoundContext,
        view: _SceneView,
        desired_scene_digest: str,
        proof: Mapping[str, Any],
    ) -> bool:
        return dict(proof) == HeadlessSceneProgramPort._proof(context, view, desired_scene_digest)

    def _remember_proof(self, desired_scene_digest: str, proof: Mapping[str, Any]) -> None:
        with self._lock:
            if desired_scene_digest not in self._prepared and len(self._prepared) >= _MAX_CONTEXTS:
                self._prepared.pop(next(iter(self._prepared)))
            self._prepared[desired_scene_digest] = deepcopy(dict(proof))

    def _initial_scene_operations(
        self,
        context: _SceneBoundContext,
        initial_scene: Sequence[Mapping[str, Any]],
        *,
        cell_id: str,
    ) -> list[dict[str, Any]]:
        rows = list(initial_scene)
        ids = [
            _text(
                _mapping(row, field=f"initial_scene[{index}]").get("scene_resource_id"),
                field=f"initial_scene[{index}].scene_resource_id",
            )
            for index, row in enumerate(rows)
        ]
        if ids != sorted(ids) or len(ids) != len(set(ids)):
            raise ProductionPortError("initial scene is not canonical")
        operations: list[tuple[bool, dict[str, Any]]] = []
        for index, raw_row in enumerate(rows):
            row = _mapping(raw_row, field=f"initial_scene[{index}]")
            pose, frame_ref = self._initial_pose_and_frame(
                context,
                row.get("pose"),
                cell_id=cell_id,
                field=f"initial_scene[{index}].pose",
            )
            resource_roles = {
                _text(role, field=f"initial_scene[{index}].resource_roles")
                for role in _sequence(
                    row.get("resource_roles", ()),
                    field=f"initial_scene[{index}].resource_roles",
                )
            }
            operations.append(
                (
                    "pick_object" in resource_roles,
                    {
                        "kind": "add_object",
                        "scene_resource_id": ids[index],
                        "shape": _scene_shape(
                            row.get("shape"),
                            field=f"initial_scene[{index}].shape",
                        ),
                        "pose": pose,
                        "frame_ref": frame_ref,
                        "support_contacts": _scene_support_contacts(
                            row.get("metadata"),
                            field=f"initial_scene[{index}].metadata",
                        ),
                    },
                )
            )
        operations.sort(key=lambda item: (item[0], item[1]["scene_resource_id"]))
        return [operation for _pick_object, operation in operations]

    def prepare_initial_scene(
        self,
        context: ProgramRunPortContext,
        initial_scene: Sequence[Mapping[str, Any]],
        *,
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        parsed = self._parse_context(context)
        self.bind(context)
        with self._lock:
            current = self._contexts.get(context.program_run_id)
            if current != parsed:
                raise ProductionPortError("Program-run scene context was rebound")
        view = self._read_view(parsed)
        operations = self._initial_scene_operations(parsed, initial_scene, cell_id=view.cell_id)
        desired_scene_digest = self._desired_scene_digest(parsed, operations)
        with self._lock:
            cached = self._prepared.get(desired_scene_digest)
        if (
            cached is not None and self._proof_matches(parsed, view, desired_scene_digest, cached)
        ) or self._scene_matches_operations(view, operations):
            proof = self._proof(parsed, view, desired_scene_digest)
            self._remember_proof(desired_scene_digest, proof)
            return proof

        expected = view.projection_revision
        ordinal = 0

        def commit(operation: Mapping[str, Any], coordinate: str) -> None:
            nonlocal expected, ordinal
            ordinal += 1
            attempt = 0
            deadline = time.monotonic() + _SCENE_INITIALIZATION_SETTLEMENT_TIMEOUT_S
            while True:
                try:
                    expected, _response = self._commit(
                        context=parsed,
                        request_id=_request_identity(
                            "scene-init",
                            context.program_run_id,
                            str(ordinal),
                            coordinate,
                            str(attempt),
                        ),
                        correlation_id=context.program_run_id,
                        request_owner_epoch=view.request_owner_epoch,
                        expected_projection_revision=expected,
                        operation=operation,
                        program_reset_scene_authorization=(program_reset_scene_authorization),
                    )
                    return
                except Exception as error:
                    code = getattr(error, "code", None)
                    if code not in {
                        "stale_revision",
                        "stale_generation",
                        "authoring_not_ready",
                    }:
                        raise
                    if time.monotonic() >= deadline:
                        raise ProductionPortError(
                            "logical-scene initialization could not acquire "
                            "stable scene and authoring revisions"
                        ) from error
                    time.sleep(_SCENE_INITIALIZATION_SETTLEMENT_POLL_S)
                    refreshed = self._read_view(parsed)
                    if refreshed.request_owner_epoch != view.request_owner_epoch:
                        raise ProductionPortError(
                            "logical-scene owner changed during initialization"
                        ) from error
                    expected = refreshed.projection_revision
                    attempt += 1

        for attachment in view.attachments:
            attachment_id = _text(
                attachment.get("carried_object_attachment_id"),
                field="carried_object_attachment_id",
            )
            commit(
                {
                    "kind": "detach_carried_object_at_current_pose",
                    "carried_object_attachment_id": attachment_id,
                    "expected_carried_object_attachment_revision": _text(
                        attachment.get("carried_object_attachment_revision"),
                        field="carried_object_attachment_revision",
                    ),
                    "expected_scene_resource_id": _text(
                        attachment.get("scene_resource_id"),
                        field="scene_resource_id",
                    ),
                    "expected_controllable_component_id": _text(
                        attachment.get("controllable_component_id"),
                        field="controllable_component_id",
                    ),
                    "expected_command_target_id": _text(
                        attachment.get("command_target_id"),
                        field="command_target_id",
                    ),
                },
                f"detach:{attachment_id}",
            )
        if view.attachments:
            refreshed = self._read_view(parsed)
            if refreshed.request_owner_epoch != view.request_owner_epoch:
                raise ProductionPortError("logical-scene owner changed during initialization")
            view = refreshed
            expected = view.projection_revision

        desired_ids = {str(operation["scene_resource_id"]) for operation in operations}
        for operation in operations:
            if self._object_matches_operation(view, operation):
                continue
            scene_resource_id = str(operation["scene_resource_id"])
            replacement = dict(operation)
            if scene_resource_id in view.objects:
                replacement["kind"] = "upsert_object"
            commit(replacement, f"upsert:{scene_resource_id}")
        for scene_resource_id in self._removal_order(view.objects):
            if scene_resource_id in desired_ids:
                continue
            commit(
                {
                    "kind": "remove_object",
                    "scene_resource_id": scene_resource_id,
                },
                f"remove:{scene_resource_id}",
            )
        verified = self._read_view(parsed)
        if (
            verified.request_owner_epoch != view.request_owner_epoch
            or verified.projection_revision != expected
            or verified.attachments
            or tuple(verified.objects)
            != tuple(sorted(str(item["scene_resource_id"]) for item in operations))
        ):
            raise ProductionPortError(
                "logical-scene owner did not publish the exact initialized scene"
            )
        proof = self._proof(parsed, verified, desired_scene_digest)
        with self._lock:
            if self._contexts.get(context.program_run_id) != parsed:
                raise ProductionPortError("Program-run scene context was rebound")
        self._remember_proof(desired_scene_digest, proof)
        return proof

    def verify_initial_scene(
        self,
        context: ProgramRunPortContext,
        initial_scene: Sequence[Mapping[str, Any]],
        proof: Mapping[str, Any],
    ) -> None:
        parsed = self._parse_context(context)
        view = self._read_view(parsed)
        operations = self._initial_scene_operations(parsed, initial_scene, cell_id=view.cell_id)
        desired_scene_digest = self._desired_scene_digest(parsed, operations)
        if not self._proof_matches(parsed, view, desired_scene_digest, proof):
            raise ProductionPortError("prepared logical scene changed before Program capture")

    def release(self, program_run_id: str) -> None:
        self.unbind(program_run_id)

    def invoke(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerInvocationResult:
        try:
            context = self._context(identity.program_run_id)
            if self._program_node_evidence is None:
                raise ProductionPortError("runtime Scene node admission is unavailable")
            owner_api = (
                "scene.carried_attachment"
                if node_id in {"scene.attach_carried_object", "scene.detach_carried_object"}
                else "scene.transaction"
            )
            evidence = self._program_node_evidence(identity, owner_api)
            view = self._read_view(context)
            parameters = inputs.parameters
            if node_id in {"scene.add_object", "scene.upsert_object"}:
                pose, frame_ref = self._node_pose_and_frame(
                    context, view, parameters.get("pose"), field="pose"
                )
                operation = {
                    "kind": ("add_object" if node_id == "scene.add_object" else "upsert_object"),
                    "scene_resource_id": _text(
                        parameters.get("scene_resource_id"),
                        field="scene_resource_id",
                    ),
                    "shape": _scene_shape(parameters.get("shape"), field="shape"),
                    "pose": pose,
                    "frame_ref": frame_ref,
                    "support_contacts": [],
                }
            elif node_id == "scene.remove_object":
                operation = {
                    "kind": "remove_object",
                    "scene_resource_id": _text(
                        parameters.get("scene_resource_id"),
                        field="scene_resource_id",
                    ),
                }
            elif node_id == "scene.update_object_pose":
                pose, frame_ref = self._node_pose_and_frame(
                    context, view, parameters.get("pose"), field="pose"
                )
                operation = {
                    "kind": "update_object_pose",
                    "scene_resource_id": _text(
                        parameters.get("scene_resource_id"),
                        field="scene_resource_id",
                    ),
                    "pose": pose,
                    "frame_ref": frame_ref,
                    "provider_evidence": None,
                    "expected_attached": None,
                }
            elif node_id == "scene.attach_carried_object":
                target_role = _text(parameters.get("target_role"), field="target_role")
                target = self._target(context, target_role)
                frame_role = _text(
                    parameters.get("carrier_frame_role"),
                    field="carrier_frame_role",
                )
                if frame_role not in target.frames:
                    raise ProductionPortError("Program Binding has no exact carrier frame")
                touch_body_roles = [
                    _text(value, field="touch_body_role")
                    for value in _sequence(
                        parameters.get("touch_body_roles"),
                        field="touch_body_roles",
                    )
                ]
                if len(touch_body_roles) != len(set(touch_body_roles)) or any(
                    role not in target.collision_body_roles for role in touch_body_roles
                ):
                    raise ProductionPortError(
                        "touch-body roles are duplicate or absent from the binding"
                    )
                scene_resource_id = _text(
                    parameters.get("scene_resource_id"),
                    field="scene_resource_id",
                )
                operation = {
                    "kind": "attach_carried_object",
                    "carried_object_attachment_id": _request_identity(
                        "carried-object",
                        identity.program_run_id,
                        scene_resource_id,
                        target_role,
                    ),
                    "scene_resource_id": scene_resource_id,
                    "controllable_component_id": target.controllable_component_id,
                    "command_target_id": target.command_target_id,
                    "frame_role_id": frame_role,
                    "touch_body_roles": sorted(touch_body_roles),
                    "expected_component_attachment_revisions": (
                        self._component_attachment_revisions(
                            context, target.controllable_component_id
                        )
                    ),
                }
            elif node_id == "scene.detach_carried_object":
                scene_resource_id = _text(
                    parameters.get("scene_resource_id"),
                    field="scene_resource_id",
                )
                target = self._target(
                    context,
                    _text(parameters.get("target_role"), field="target_role"),
                )
                matches = [
                    attachment
                    for attachment in view.attachments
                    if attachment.get("scene_resource_id") == scene_resource_id
                ]
                if len(matches) != 1:
                    raise ProductionPortError(
                        "scene resource has no unique carried-object attachment"
                    )
                attachment = matches[0]
                if (
                    attachment.get("controllable_component_id") != target.controllable_component_id
                    or attachment.get("command_target_id") != target.command_target_id
                ):
                    raise ProductionPortError("carried-object attachment belongs to another target")
                operation = {
                    "kind": "detach_carried_object_at_current_pose",
                    "carried_object_attachment_id": _text(
                        attachment.get("carried_object_attachment_id"),
                        field="carried_object_attachment_id",
                    ),
                    "expected_carried_object_attachment_revision": _text(
                        attachment.get("carried_object_attachment_revision"),
                        field="carried_object_attachment_revision",
                    ),
                    "expected_scene_resource_id": scene_resource_id,
                    "expected_controllable_component_id": (target.controllable_component_id),
                    "expected_command_target_id": target.command_target_id,
                }
            else:
                raise ProductionPortError(f"scene Program port does not implement {node_id!r}")
            base_request_id = _request_identity(
                "scene-node",
                identity.program_run_id,
                identity.node_instance_id,
                identity.node_attempt_id,
                node_id,
            )
            for attempt in range(_MAX_SCENE_NODE_STALE_RETRIES):
                request_id = (
                    base_request_id
                    if attempt == 0
                    else _request_identity("scene-node-retry", base_request_id, str(attempt))
                )
                try:
                    committed, response = self._commit(
                        context=context,
                        request_id=request_id,
                        correlation_id=identity.node_attempt_id,
                        request_owner_epoch=view.request_owner_epoch,
                        expected_projection_revision=view.projection_revision,
                        operation=operation,
                        program_node_evidence=evidence,
                    )
                except Exception as error:
                    if (
                        getattr(error, "code", None) != "stale_revision"
                        or attempt + 1 == _MAX_SCENE_NODE_STALE_RETRIES
                    ):
                        raise
                    refreshed = self._read_view(context)
                    if refreshed.request_owner_epoch != view.request_owner_epoch:
                        raise ProductionPortError(
                            "logical-scene owner changed during node mutation"
                        ) from error
                    view = refreshed
                    continue
                return OwnerInvocationResult(
                    True,
                    output={"committed_projection_revision": committed},
                    metadata={
                        "transaction_sequence": response.get("transaction_sequence"),
                        "update_status": response.get("update_status"),
                        "stale_revision_retry_count": attempt,
                    },
                )
            raise AssertionError("scene-node retry loop did not terminate")
        except OwnerInvocationPendingError:
            raise
        except Exception as exc:
            if isinstance(exc, SceneTransactionRejectedError):
                self._raise_if_stop_prevented_admission(exc)
            reason_code = getattr(exc, "code", None)
            if not isinstance(reason_code, str) or not reason_code:
                reason_code = "scene_program_request_failed"
            return OwnerInvocationResult(
                False,
                reason_code=reason_code,
                message=(str(exc) or type(exc).__name__)[:1024],
            )


__all__ = [
    "HeadlessSceneProgramPort",
    "NativeKernelProgramPort",
    "ProductionPortError",
]
