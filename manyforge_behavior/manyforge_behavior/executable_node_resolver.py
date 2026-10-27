"""Closed v2 executable-node resolution for the reference py_trees engine.

The retained :class:`~manyforge_behavior.node_environment.NodeEnvironment`
contains immutable implementation identity and provenance.  This module is the
separate build-qualified source registry that turns an exact four-part row into
either one bounded NRT adapter constructor or one explicit future-gate result.

There are deliberately no import strings, filesystem paths, plugin lookups, or
caller-provided factories here.  Adding an implementation is a source change
with tests and a new executable-package/environment identity.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Final

import py_trees
from manyforge_example_nodes import FrameHeightQuery
from manyforge_behavior import qualified_node_extensions as extension

from manyforge_behavior.catalog import CatalogEntry
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.node_registry import (
    BUILTIN_EXECUTABLE_PACKAGE_ID,
    ExecutableRegistryRow,
)
from manyforge_behavior.nodes.control_flow import (
    NodeAttemptParameters,
    ProgramParallel,
    RepeatDecorator,
    RetryDecorator,
)
from manyforge_behavior.nodes.v2_owner import (
    OwnerInvocationAdapter,
    OwnerTaskAdapter,
    GripperCommandAdapter,
    GripperStatusWaitAdapter,
    IoInputWaitAdapter,
    InterruptMotionTaskAdapter,
    IoOutputCommandAdapter,
    MotionTaskAdapter,
)
from manyforge_behavior.nodes.v2_pure import (
    GeometryCheckPoseBounds,
    GeometryCheckPoseDistance,
    GeometryOffsetPose,
    LogicCheckCondition,
    LogicCheckValue,
    LogicSetValue,
    LogicWaitForValue,
    ProgramTimer,
)
from manyforge_behavior.orchestration_ports import (
    FrameTransformError,
    FrameTransformPort,
    NodeAdapterPorts,
    NodeAttemptIdentity,
    NodeAttemptSource,
    OwnerInvocationPort,
    OwnerInvocationResult,
    OwnerNodeInputs,
    OwnerTaskPort,
)
from manyforge_behavior.orchestration_values import NodeAttemptInputResolver
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.program import INTERRUPT_MOTION_NODE_IDS, MOTION_NODE_IDS
from manyforge_behavior.program_clock import ProgramClock

class ExecutableNodeResolverError(ValueError):
    """The closed resolver and retained environment do not agree."""


class ExecutableNodeUnavailableError(RuntimeError):
    """An exact row is known but its responsible restoration gate is open."""

    def __init__(self, reason_code: str, restoration_gates: tuple[str, ...]) -> None:
        self.reason_code = reason_code
        self.restoration_gates = restoration_gates
        gates = ",".join(restoration_gates)
        super().__init__(f"{reason_code}: restoration_gates={gates}")


@dataclass(frozen=True, slots=True)
class NodeAdapterBuildContext:
    """Bounded authored inputs used to construct one orchestration adapter."""

    name: str
    node_instance_id: str
    parameters: Mapping[str, Any]
    children: tuple[py_trees.behaviour.Behaviour, ...] = ()
    value_sources: Mapping[str, Mapping[str, Any]] = field(default_factory=dict)
    input_resolver: NodeAttemptInputResolver | None = None
    attempt_source: NodeAttemptSource | None = None
    ports: NodeAdapterPorts = field(default_factory=NodeAdapterPorts)
    operator_stop_state: OperatorMotionStopState | None = None
    # The nearest self-or-ancestor whose effective operator-stop policy is
    # `hold`: its decorator is what stops ticking this node while that
    # subtree is held. None when nothing above holds it; it keeps running.
    operator_stop_hold_scope: str | None = None
    # The run's one Program clock: waits and the timer read time from it.
    program_clock: ProgramClock | None = None
    # The owner's motion attention check of one captured node (spec 530 §11).
    motion_attention: Callable[[str, str], None] | None = None
    # Receives each automatic retry's audit fact (the run's own audit list).
    audit_sink: Callable[[dict[str, Any]], None] | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.name, str) or not self.name:
            raise ValueError("adapter name must be non-empty text")
        if not isinstance(self.node_instance_id, str) or not self.node_instance_id:
            raise ValueError("node_instance_id must be non-empty text")
        if not isinstance(self.parameters, Mapping):
            raise TypeError("adapter parameters must be a mapping")
        if not isinstance(self.children, tuple) or any(
            not isinstance(child, py_trees.behaviour.Behaviour) for child in self.children
        ):
            raise TypeError("adapter children must be a tuple of py_trees behaviours")
        object.__setattr__(
            self,
            "parameters",
            MappingProxyType(deepcopy(dict(self.parameters))),
        )
        if not isinstance(self.value_sources, Mapping):
            raise TypeError("adapter value_sources must be a mapping")
        object.__setattr__(
            self,
            "value_sources",
            MappingProxyType(
                {
                    key: MappingProxyType(deepcopy(dict(source)))
                    for key, source in self.value_sources.items()
                }
            ),
        )
        if self.input_resolver is not None and not isinstance(
            self.input_resolver, NodeAttemptInputResolver
        ):
            raise TypeError("input_resolver must be a NodeAttemptInputResolver")
        if self.attempt_source is not None and not callable(
            getattr(self.attempt_source, "begin", None)
        ):
            raise TypeError("attempt_source must issue node-attempt identities")
        if not isinstance(self.ports, NodeAdapterPorts):
            raise TypeError("ports must be one NodeAdapterPorts value")
        if self.operator_stop_state is not None and not isinstance(
            self.operator_stop_state, OperatorMotionStopState
        ):
            raise TypeError("operator_stop_state must be one OperatorMotionStopState")
        if self.operator_stop_hold_scope is not None and (
            not isinstance(self.operator_stop_hold_scope, str)
            or not self.operator_stop_hold_scope
        ):
            raise TypeError("operator_stop_hold_scope must be non-empty text")
        if self.program_clock is not None and not isinstance(
            self.program_clock, ProgramClock
        ):
            raise TypeError("program_clock must be the run's ProgramClock")
        if self.motion_attention is not None and not callable(self.motion_attention):
            raise TypeError("motion_attention must be callable")
        if self.audit_sink is not None and not callable(self.audit_sink):
            raise TypeError("audit_sink must be callable")


NodeAdapterBuilder = Callable[[NodeAdapterBuildContext], py_trees.behaviour.Behaviour]


@dataclass(frozen=True, slots=True)
class ExecutableNodeDisposition:
    """Static source-qualified disposition for one exact retained row."""

    row: ExecutableRegistryRow
    adapter_build_qualified: bool
    owner_port_name: str | None
    unavailable_reason_code: str | None
    restoration_gates: tuple[str, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            **self.row.to_dict(),
            "adapter_build_qualified": self.adapter_build_qualified,
            "owner_port_name": self.owner_port_name,
            "unavailable_reason_code": self.unavailable_reason_code,
            "restoration_gates": list(self.restoration_gates),
        }


@dataclass(frozen=True, slots=True)
class _AdapterDefinition:
    row: ExecutableRegistryRow
    builder: NodeAdapterBuilder | None
    owner_port_name: str | None
    unavailable_reason_code: str | None
    restoration_gates: tuple[str, ...]


def _sequence(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return py_trees.composites.Sequence(
        name=context.name,
        memory=True,
        children=list(context.children),
    )


def _fallback(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return py_trees.composites.Selector(
        name=context.name,
        memory=True,
        children=list(context.children),
    )


def _attempt_parameters(
    context: NodeAdapterBuildContext, node_id: str
) -> NodeAttemptParameters:
    """A control node's parameters, resolved from each attempt's snapshot."""

    return NodeAttemptParameters(
        node_id=node_id,
        node_instance_id=context.node_instance_id,
        parameters=context.parameters,
        value_sources=context.value_sources,
        input_resolver=_required_input_resolver(context),
    )


def _parallel(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return ProgramParallel(
        name=context.name,
        children=list(context.children),
        attempt_inputs=_attempt_parameters(context, "parallel"),
    )


def _inverter(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return py_trees.decorators.Inverter(
        name=context.name,
        child=context.children[0],
    )


def _repeat(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return RepeatDecorator(
        name=context.name,
        child=context.children[0],
        attempt_inputs=_attempt_parameters(context, "repeat"),
    )


def _retry(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return RetryDecorator(
        name=context.name,
        child=context.children[0],
        attempt_inputs=_attempt_parameters(context, "retry"),
    )


def _required_program_clock(context: NodeAdapterBuildContext) -> ProgramClock:
    if context.program_clock is None:
        raise ValueError("a timed node requires the run's Program clock")
    return context.program_clock


def _required_input_resolver(
    context: NodeAdapterBuildContext,
) -> NodeAttemptInputResolver:
    if context.input_resolver is None:
        raise ValueError("node adapter requires one node-attempt input resolver")
    return context.input_resolver


def _required_attempt_source(
    context: NodeAdapterBuildContext,
) -> NodeAttemptSource:
    if context.attempt_source is None:
        raise ValueError("owner adapter requires one node-attempt source")
    return context.attempt_source


def _required_port(
    context: NodeAdapterBuildContext,
    port_name: str,
) -> OwnerTaskPort | OwnerInvocationPort:
    port = getattr(context.ports, port_name)
    if port is None:
        raise ValueError(f"owner adapter requires the {port_name} port")
    return port


def _owner_task_builder(
    node_id: str,
    port_name: str,
    *,
    owner_api: str,
    output_key_parameter: str | None = None,
    blackboard_read_parameters: tuple[str, ...] = (),
    require_initial_running: bool = False,
    cancellable: bool = False,
) -> NodeAdapterBuilder:
    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        port = _required_port(context, port_name)
        gripper_command = node_id == "gripper.command_position"
        interrupt_motion = node_id in INTERRUPT_MOTION_NODE_IDS
        motion = node_id in MOTION_NODE_IDS or interrupt_motion
        adapter = (
            GripperCommandAdapter if gripper_command
            else InterruptMotionTaskAdapter if interrupt_motion
            else MotionTaskAdapter if motion
            else IoOutputCommandAdapter if node_id == "io.set_output"
            else OwnerTaskAdapter
        )
        timing = ({
            "program_clock": _required_program_clock(context),
            "operator_stop_hold_scope": context.operator_stop_hold_scope,
        } if gripper_command or motion else {})
        if motion:
            timing["motion_attention"] = context.motion_attention
            timing["audit_sink"] = context.audit_sink
        if gripper_command:
            timing["io_watches"] = getattr(context.ports, "io_watches", None)
            timing["io_signals"] = getattr(context.ports, "io_signals", None)
        if interrupt_motion:
            timing["mock_simulation"] = getattr(context.ports, "mock_simulation", None)
        return adapter(
            **timing,
            name=context.name,
            node_id=node_id,
            node_instance_id=context.node_instance_id,
            attempt_source=_required_attempt_source(context),
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            port=port,  # type: ignore[arg-type]
            owner_port_name=port_name,
            intervention_port=context.ports.program_interventions,
            output_key_parameter=output_key_parameter,
            blackboard_read_parameters=blackboard_read_parameters,
            require_initial_running=require_initial_running,
            operator_stop_state=context.operator_stop_state,
            cancellable=cancellable,
            owner_api=owner_api,
        )

    return build


class FramedObjectPoseQuery:
    """Get object pose: the Scene owner answers in the cell frame; any other
    declared frame is one owner frame transform of the object's own frame
    (carried objects included), never a client-side composition."""

    def __init__(
        self, scene: OwnerInvocationPort, frames: FrameTransformPort | None
    ) -> None:
        self._scene = scene
        self._frames = frames

    def invoke(
        self, node_id: str, identity: NodeAttemptIdentity, inputs: OwnerNodeInputs
    ) -> OwnerInvocationResult:
        frame = inputs.parameters.get("frame")
        if frame is None or dict(frame) == {"kind": "cell_frame"}:
            return self._scene.invoke(node_id, identity, inputs)
        if self._frames is None:
            return OwnerInvocationResult(
                False,
                reason_code="frame_transform_unavailable",
                message="this Program run has no native frame transform owner",
            )
        object_frame = {
            "kind": "scene_resource",
            "scene_resource_id": inputs.parameters["scene_resource_id"],
        }
        try:
            result = self._frames.express_pose(
                identity,
                {
                    "frame_ref": object_frame,
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
                frame,
            )
        except FrameTransformError as exc:
            return OwnerInvocationResult(
                False, reason_code=exc.reason_code, message=str(exc), details=exc.details
            )
        return OwnerInvocationResult(True, output=result.pose, metadata=result.metadata)


def _owner_invocation_builder(
    node_id: str,
    port_name: str,
    *,
    owner_api: str,
    output_key_parameter: str | None = None,
    blackboard_read_parameters: tuple[str, ...] = (),
    transform_port: Callable[[OwnerInvocationPort], OwnerInvocationPort] | None = None,
    framed_port: (
        Callable[[OwnerInvocationPort, FrameTransformPort | None], OwnerInvocationPort] | None
    ) = None,
    cancellable: bool = False,
) -> NodeAdapterBuilder:
    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        port = _required_port(context, port_name)
        if transform_port is not None:
            port = transform_port(port)
        if framed_port is not None:
            port = framed_port(port, context.ports.model_queries)
        common = dict(
            name=context.name,
            node_id=node_id,
            node_instance_id=context.node_instance_id,
            attempt_source=_required_attempt_source(context),
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            port=port,
            output_key_parameter=output_key_parameter,
            blackboard_read_parameters=blackboard_read_parameters,
            cancellable=cancellable,
            owner_api=owner_api,
        )
        if node_id in {"io.get_input", "io.check_input"}:
            common.update(
                mock_simulation=getattr(context.ports, "mock_simulation", None),
                audit_sink=context.audit_sink,
            )
        if node_id != "gripper.wait_for_status":
            return OwnerInvocationAdapter(
                operator_stop_state=context.operator_stop_state,
                **common,  # type: ignore[arg-type]
            )
        # Only a waiter some hold policy can stop ticking sees the Stop
        # state, with the subtree whose hold actually stops it.
        held = (
            context.operator_stop_state is not None
            and context.operator_stop_hold_scope is not None
        )
        return GripperStatusWaitAdapter(
            program_clock=_required_program_clock(context),
            operator_stop_state=context.operator_stop_state if held else None,
            operator_stop_hold_scope=context.operator_stop_hold_scope if held else None,
            **common,  # type: ignore[arg-type]
        )

    return build


def _io_wait_builder(
    node_id: str, *, owner_api: str, cancellable: bool
) -> NodeAdapterBuilder:
    """One level watch per attempt, observed at the tree's own tick cadence."""

    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        held = (
            context.operator_stop_state is not None
            and context.operator_stop_hold_scope is not None
        )
        return IoInputWaitAdapter(
            name=context.name,
            node_id=node_id,
            node_instance_id=context.node_instance_id,
            attempt_source=_required_attempt_source(context),
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            port=_required_port(context, "io_watches"),  # type: ignore[arg-type]
            owner_port_name="io_watches",
            program_clock=_required_program_clock(context),
            operator_stop_state=context.operator_stop_state if held else None,
            operator_stop_hold_scope=context.operator_stop_hold_scope if held else None,
            audit_sink=context.audit_sink,
            mock_simulation=getattr(context.ports, "mock_simulation", None),
            cancellable=cancellable,
            owner_api=owner_api,
        )

    return build


def _pure_leaf(
    adapter: Callable[..., py_trees.behaviour.Behaviour],
    *,
    timed: bool = False,
) -> NodeAdapterBuilder:
    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        return adapter(
            name=context.name,
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            attempt_source=_required_attempt_source(context),
            frame_transforms=context.ports.model_queries,
            # A timed node reads the run's Program clock. Only one some hold
            # policy can stop ticking sees the Stop state, with the subtree
            # whose hold actually stops it.
            **({"program_clock": _required_program_clock(context)} if timed else {}),
            **({"operator_stop_state": context.operator_stop_state,
                "operator_stop_hold_scope": context.operator_stop_hold_scope}
               if timed
               and context.operator_stop_state is not None
               and context.operator_stop_hold_scope is not None else {}),
        )

    return build


_PURE_BUILDERS: Final[Mapping[str, NodeAdapterBuilder]] = MappingProxyType(
    {
        "sequence_v1": _sequence,
        "fallback_v1": _fallback,
        "parallel_v1": _parallel,
        "inverter_v1": _inverter,
        "repeat_v1": _repeat,
        "retry_v1": _retry,
        "timer_v1": _pure_leaf(ProgramTimer, timed=True),
        "logic_check_condition_v1": _pure_leaf(LogicCheckCondition),
        "logic_set_value_v1": _pure_leaf(LogicSetValue),
        "logic_check_value_v1": _pure_leaf(LogicCheckValue),
        "logic_wait_for_value_v1": _pure_leaf(LogicWaitForValue, timed=True),
        "geometry_check_pose_distance_v1": _pure_leaf(GeometryCheckPoseDistance),
        "geometry_offset_pose_v1": _pure_leaf(GeometryOffsetPose),
        "geometry_check_pose_bounds_v1": _pure_leaf(GeometryCheckPoseBounds),
    }
)
_OWNER_NEUTRAL_APIS: Final = frozenset(
    {
        "geometry.compare",
        "orchestration.blackboard",
        "orchestration.clock",
        "orchestration.control_flow",
    }
)
_TASK_OWNER_PORTS: Final[Mapping[str, str]] = MappingProxyType(
    {
        "assistant.model_request": "assistant_requests",
        "assistant.scene_context": "assistant_requests",
        "assistant.scene_selection": "assistant_requests",
        "kernel.operation": "kernel_operations",
        "kernel.reset": "kernel_controls",
        "kernel.runtime_scaling": "kernel_controls",
        "kernel.stop": "kernel_controls",
        "kernel.full_stop": "kernel_controls",
        "kernel.recovery": "kernel_controls",
        "planning.pick_sequence": "kernel_operations",
        "planning.place_sequence": "kernel_operations",
        "simulator.control": "simulator_control",
    }
)
_INVOCATION_OWNER_PORTS: Final[Mapping[str, str]] = MappingProxyType(
    {
        "model.kinematics": "model_queries",
        "scene.carried_attachment": "scene_transactions",
        "scene.query": "scene_transactions",
        "scene.transaction": "scene_transactions",
    }
)
def _output_key_parameter(entry: CatalogEntry) -> str | None:
    if len(entry.blackboard_writes) > 1:
        raise ExecutableNodeResolverError(f"node {entry.id!r} has multiple output bindings")
    return entry.blackboard_writes[0].key_parameter if entry.blackboard_writes else None


def _definition_for(
    row: ExecutableRegistryRow,
    entry: CatalogEntry,
) -> _AdapterDefinition:
    expected_package = (
        extension.PACKAGE_ID if row.node_id == extension.NODE_ID else BUILTIN_EXECUTABLE_PACKAGE_ID
    )
    if row.executable_package_id != expected_package:
        raise ExecutableNodeResolverError(
            "node implementation belongs to another qualified package"
        )
    if row.engine_id != "py_trees":
        raise ExecutableNodeResolverError(
            f"engine {row.engine_id!r} has no build-qualified resolver"
        )
    if entry.runner_implementation_ids.get(row.engine_id) != row.implementation_id:
        raise ExecutableNodeResolverError(
            f"row for {row.node_id!r} disagrees with catalog runner metadata"
        )
    if row.executable_package_id == extension.PACKAGE_ID:
        if (
            row.node_id != extension.NODE_ID
            or row.implementation_id != extension.IMPLEMENTATION_ID
            or entry.execution.owner_api != "model.kinematics"
        ):
            raise ExecutableNodeResolverError("extension row has no reviewed adapter")
        return _AdapterDefinition(
            row,
            _owner_invocation_builder(
                row.node_id,
                "model_queries",
                owner_api=entry.execution.owner_api,
                transform_port=FrameHeightQuery,
            ),
            "model_queries",
            None,
            (),
        )
    pure_builder = _PURE_BUILDERS.get(row.implementation_id)
    if pure_builder is not None:
        if entry.execution.owner_api not in _OWNER_NEUTRAL_APIS:
            raise ExecutableNodeResolverError(
                f"pure implementation {row.implementation_id!r} is bound to "
                f"effectful owner API {entry.execution.owner_api!r}"
            )
        return _AdapterDefinition(row, pure_builder, None, None, ())
    if entry.execution.owner_api in _OWNER_NEUTRAL_APIS:
        raise ExecutableNodeResolverError(
            f"owner-neutral implementation {row.implementation_id!r} has no "
            "build-qualified algorithm"
        )

    output_key_parameter = _output_key_parameter(entry)
    blackboard_read_parameters = tuple(binding.key_parameter for binding in entry.blackboard_reads)
    task_port = _TASK_OWNER_PORTS.get(entry.execution.owner_api)
    invocation_port = _INVOCATION_OWNER_PORTS.get(entry.execution.owner_api)
    if entry.execution.owner_api == "kernel.command_target":
        if row.node_id in {"gripper.command_position", "io.set_output"}:
            task_port = "kernel_operations"
        elif row.node_id in {"gripper.check_status", "gripper.wait_for_status"}:
            invocation_port = "gripper_status"
        elif row.node_id in {"io.get_input", "io.check_input"}:
            invocation_port = "io_signals"
        elif row.node_id == "io.wait_for_input":
            return _AdapterDefinition(
                row,
                _io_wait_builder(
                    row.node_id,
                    owner_api=entry.execution.owner_api,
                    cancellable=entry.execution.cancellable,
                ),
                "io_watches",
                None,
                (),
            )
    if task_port is not None and invocation_port is not None:
        raise ExecutableNodeResolverError(f"node {row.node_id!r} has ambiguous owner-port binding")
    if task_port is not None:
        return _AdapterDefinition(
            row,
            _owner_task_builder(
                row.node_id,
                task_port,
                owner_api=entry.execution.owner_api,
                output_key_parameter=output_key_parameter,
                blackboard_read_parameters=blackboard_read_parameters,
                require_initial_running=row.node_id == "assistant.query_model",
                cancellable=entry.execution.cancellable,
            ),
            task_port,
            None,
            (),
        )
    if invocation_port is not None:
        return _AdapterDefinition(
            row,
            _owner_invocation_builder(
                row.node_id,
                invocation_port,
                owner_api=entry.execution.owner_api,
                output_key_parameter=output_key_parameter,
                blackboard_read_parameters=blackboard_read_parameters,
                framed_port=(
                    FramedObjectPoseQuery if row.node_id == "scene.get_object_pose" else None
                ),
                cancellable=entry.execution.cancellable,
            ),
            invocation_port,
            None,
            (),
        )
    raise ExecutableNodeResolverError(
        f"node {row.node_id!r} has no build-qualified adapter binding"
    )


class ExecutableNodeResolver:
    """Resolve only the exact rows captured by one immutable environment."""

    def __init__(self, environment: NodeEnvironment) -> None:
        if not isinstance(environment, NodeEnvironment):
            raise TypeError("resolver requires one retained NodeEnvironment")
        definitions = {
            row: _definition_for(row, environment.catalog.get(row.node_id))
            for row in environment.registry.rows
        }
        if len(definitions) != len(environment.registry.rows):
            raise ExecutableNodeResolverError("retained environment contains duplicate exact rows")
        self._environment_digest = environment.environment_digest
        self._definitions = MappingProxyType(definitions)

    @property
    def environment_digest(self) -> str:
        return self._environment_digest

    @property
    def dispositions(self) -> tuple[ExecutableNodeDisposition, ...]:
        return tuple(
            ExecutableNodeDisposition(
                row=definition.row,
                adapter_build_qualified=definition.builder is not None,
                owner_port_name=definition.owner_port_name,
                unavailable_reason_code=definition.unavailable_reason_code,
                restoration_gates=definition.restoration_gates,
            )
            for definition in sorted(self._definitions.values(), key=lambda item: item.row)
        )

    def disposition(self, row: ExecutableRegistryRow) -> ExecutableNodeDisposition:
        try:
            definition = self._definitions[row]
        except KeyError as exc:
            raise ExecutableNodeResolverError(
                "executable row is not part of the captured environment"
            ) from exc
        return ExecutableNodeDisposition(
            row=definition.row,
            adapter_build_qualified=definition.builder is not None,
            owner_port_name=definition.owner_port_name,
            unavailable_reason_code=definition.unavailable_reason_code,
            restoration_gates=definition.restoration_gates,
        )

    def instantiate(
        self,
        row: ExecutableRegistryRow,
        context: NodeAdapterBuildContext,
    ) -> py_trees.behaviour.Behaviour:
        try:
            definition = self._definitions[row]
        except KeyError as exc:
            raise ExecutableNodeResolverError(
                "executable row is not part of the captured environment"
            ) from exc
        if definition.builder is None:
            assert definition.unavailable_reason_code is not None
            raise ExecutableNodeUnavailableError(
                definition.unavailable_reason_code,
                definition.restoration_gates,
            )
        try:
            behavior = definition.builder(context)
        except (ExecutableNodeResolverError, ExecutableNodeUnavailableError):
            raise
        except (Exception, SystemExit) as exc:
            raise ExecutableNodeResolverError(
                f"adapter construction failed for {row.node_id!r}: {exc}"
            ) from exc
        if not isinstance(behavior, py_trees.behaviour.Behaviour):
            raise ExecutableNodeResolverError(
                f"adapter for {row.node_id!r} did not return a py_trees behaviour"
            )
        return behavior


def build_executable_node_resolver(
    environment: NodeEnvironment,
) -> ExecutableNodeResolver:
    """Build the sole closed resolver for one retained environment."""

    return ExecutableNodeResolver(environment)


__all__ = [
    "ExecutableNodeDisposition",
    "ExecutableNodeResolver",
    "ExecutableNodeResolverError",
    "ExecutableNodeUnavailableError",
    "NodeAdapterBuildContext",
    "build_executable_node_resolver",
]
