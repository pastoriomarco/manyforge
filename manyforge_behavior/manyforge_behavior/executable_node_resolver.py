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

from manyforge_behavior.catalog import CatalogEntry
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.node_registry import ExecutableRegistryRow
from manyforge_behavior.nodes.control_flow import RepeatDecorator, RetryDecorator
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter, OwnerTaskAdapter
from manyforge_behavior.nodes.v2_pure import (
    GeometryCheckPoseDistance,
    LogicCheckBool,
    LogicSetBool,
    LogicWaitForBool,
)
from manyforge_behavior.orchestration_ports import (
    NodeAdapterPorts,
    NodeAttemptSource,
    OwnerInvocationPort,
    OwnerTaskPort,
)
from manyforge_behavior.orchestration_values import NodeAttemptInputResolver

I7_ADAPTER_REASON: Final = "cclc_node_adapter_unavailable_until_i7"
I8_ADAPTER_REASON: Final = "kernel_policy_node_adapter_unavailable_until_i8"
I9_ADAPTER_REASON: Final = "simulation_node_adapter_unavailable_until_i9"
I11_ADAPTER_REASON: Final = "semantic_io_node_adapter_unavailable_until_i11"


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

    def __post_init__(self) -> None:
        if not isinstance(self.name, str) or not self.name:
            raise ValueError("adapter name must be non-empty text")
        if not isinstance(self.node_instance_id, str) or not self.node_instance_id:
            raise ValueError("node_instance_id must be non-empty text")
        if not isinstance(self.parameters, Mapping):
            raise TypeError("adapter parameters must be a mapping")
        if not isinstance(self.children, tuple) or any(
            not isinstance(child, py_trees.behaviour.Behaviour)
            for child in self.children
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


def _parallel(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    success_policy = context.parameters.get("success_policy", "all")
    if success_policy == "all":
        policy = py_trees.common.ParallelPolicy.SuccessOnAll()
    elif success_policy == "one":
        policy = py_trees.common.ParallelPolicy.SuccessOnOne()
    else:
        raise ValueError("parallel success_policy must be 'all' or 'one'")
    return py_trees.composites.Parallel(
        name=context.name,
        policy=policy,
        children=list(context.children),
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
        num_cycles=context.parameters["num_cycles"],
    )


def _retry(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    return RetryDecorator(
        name=context.name,
        child=context.children[0],
        num_attempts=context.parameters["num_attempts"],
    )


def _timer(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
    duration = context.parameters["duration_s"]
    if isinstance(duration, bool) or not isinstance(duration, (int, float)):
        raise TypeError("duration_s must be a number")
    duration_s = float(duration)
    if not 0.0 < duration_s <= 86_400.0:
        raise ValueError("duration_s must be in (0, 86400]")
    return py_trees.timers.Timer(name=context.name, duration=duration_s)


def _required_input_resolver(
    context: NodeAdapterBuildContext,
) -> NodeAttemptInputResolver:
    if context.input_resolver is None:
        raise ValueError("leaf adapter requires one node-attempt input resolver")
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
    output_key_parameter: str | None = None,
    blackboard_read_parameters: tuple[str, ...] = (),
    require_initial_running: bool = False,
) -> NodeAdapterBuilder:
    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        port = _required_port(context, port_name)
        return OwnerTaskAdapter(
            name=context.name,
            node_id=node_id,
            node_instance_id=context.node_instance_id,
            attempt_source=_required_attempt_source(context),
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            port=port,  # type: ignore[arg-type]
            intervention_port=context.ports.program_interventions,
            output_key_parameter=output_key_parameter,
            blackboard_read_parameters=blackboard_read_parameters,
            require_initial_running=require_initial_running,
        )

    return build


def _owner_invocation_builder(
    node_id: str,
    port_name: str,
    *,
    output_key_parameter: str | None = None,
    blackboard_read_parameters: tuple[str, ...] = (),
) -> NodeAdapterBuilder:
    def build(context: NodeAdapterBuildContext) -> py_trees.behaviour.Behaviour:
        port = _required_port(context, port_name)
        return OwnerInvocationAdapter(
            name=context.name,
            node_id=node_id,
            node_instance_id=context.node_instance_id,
            attempt_source=_required_attempt_source(context),
            parameters=context.parameters,
            value_sources=context.value_sources,
            input_resolver=_required_input_resolver(context),
            port=port,  # type: ignore[arg-type]
            output_key_parameter=output_key_parameter,
            blackboard_read_parameters=blackboard_read_parameters,
        )

    return build


def _logic_check_bool(
    context: NodeAdapterBuildContext,
) -> py_trees.behaviour.Behaviour:
    return LogicCheckBool(
        name=context.name,
        parameters=context.parameters,
        value_sources=context.value_sources,
        input_resolver=_required_input_resolver(context),
    )


def _logic_set_bool(
    context: NodeAdapterBuildContext,
) -> py_trees.behaviour.Behaviour:
    return LogicSetBool(
        name=context.name,
        parameters=context.parameters,
        value_sources=context.value_sources,
        input_resolver=_required_input_resolver(context),
    )


def _logic_wait_for_bool(
    context: NodeAdapterBuildContext,
) -> py_trees.behaviour.Behaviour:
    return LogicWaitForBool(
        name=context.name,
        parameters=context.parameters,
        value_sources=context.value_sources,
        input_resolver=_required_input_resolver(context),
    )


def _geometry_check_pose_distance(
    context: NodeAdapterBuildContext,
) -> py_trees.behaviour.Behaviour:
    return GeometryCheckPoseDistance(
        name=context.name,
        parameters=context.parameters,
        value_sources=context.value_sources,
        input_resolver=_required_input_resolver(context),
    )


_PURE_BUILDERS: Final[Mapping[str, NodeAdapterBuilder]] = MappingProxyType(
    {
        "sequence_v1": _sequence,
        "fallback_v1": _fallback,
        "parallel_v1": _parallel,
        "inverter_v1": _inverter,
        "repeat_v1": _repeat,
        "retry_v1": _retry,
        "timer_v1": _timer,
        "logic_check_bool_v1": _logic_check_bool,
        "logic_set_bool_v1": _logic_set_bool,
        "logic_wait_for_bool_v1": _logic_wait_for_bool,
        "geometry_check_pose_distance_v1": _geometry_check_pose_distance,
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
        "kernel.pick_place_sequence": "pick_place",
        "planning.pick_sequence": "pick_place",
    }
)
_INVOCATION_OWNER_PORTS: Final[Mapping[str, str]] = MappingProxyType(
    {
        "kernel.pause": "kernel_controls",
        "kernel.reset": "kernel_controls",
        "kernel.resume": "kernel_controls",
        "kernel.runtime_scaling": "kernel_controls",
        "kernel.stop": "kernel_controls",
        "model.kinematics": "model_queries",
        "scene.carried_attachment": "scene_transactions",
        "scene.transaction": "scene_transactions",
    }
)
_DEFERRED_BY_NODE: Final[Mapping[str, tuple[str, tuple[str, ...]]]] = MappingProxyType(
    {
        "controller.select_route": (I7_ADAPTER_REASON, ("I7", "L0")),
        "io.get_input": (I11_ADAPTER_REASON, ("I11", "T")),
        "io.set_output": (I11_ADAPTER_REASON, ("I11", "T")),
        "io.wait_for_input": (I11_ADAPTER_REASON, ("I11", "T")),
        "kernel.full_stop": (I8_ADAPTER_REASON, ("I8", "P5")),
        "lifecycle.disable_component": (I7_ADAPTER_REASON, ("I7", "L0")),
        "lifecycle.enable_component": (I7_ADAPTER_REASON, ("I7", "L0")),
        "lifecycle.prepare_component": (I7_ADAPTER_REASON, ("I7", "L0")),
        "lifecycle.recover_component": (I7_ADAPTER_REASON, ("I7", "L0")),
        "motion.move_until_signal": (I8_ADAPTER_REASON, ("I8", "P5")),
        "motion.probe_search": (I8_ADAPTER_REASON, ("I8", "P5")),
        "recovery.apply_offered_action": (
            I8_ADAPTER_REASON,
            ("I8", "P5"),
        ),
        "simulation.reset": (I9_ADAPTER_REASON, ("I9", "I")),
        "simulation.set_entity_pose": (I9_ADAPTER_REASON, ("I9", "I")),
        "simulation.set_state": (I9_ADAPTER_REASON, ("I9", "I")),
        "simulation.step": (I9_ADAPTER_REASON, ("I9", "I")),
    }
)


def _output_key_parameter(entry: CatalogEntry) -> str | None:
    if len(entry.blackboard_writes) > 1:
        raise ExecutableNodeResolverError(
            f"node {entry.id!r} has multiple output bindings"
        )
    return entry.blackboard_writes[0].key_parameter if entry.blackboard_writes else None


def _definition_for(
    row: ExecutableRegistryRow,
    entry: CatalogEntry,
) -> _AdapterDefinition:
    if row.engine_id != "py_trees":
        raise ExecutableNodeResolverError(
            f"engine {row.engine_id!r} has no build-qualified resolver"
        )
    if entry.runner_implementation_ids.get(row.engine_id) != row.implementation_id:
        raise ExecutableNodeResolverError(
            f"row for {row.node_id!r} disagrees with catalog runner metadata"
        )
    deferred = _DEFERRED_BY_NODE.get(row.node_id)
    if deferred is not None:
        reason_code, restoration_gates = deferred
        return _AdapterDefinition(
            row,
            None,
            None,
            reason_code,
            restoration_gates,
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
    blackboard_read_parameters = tuple(
        binding.key_parameter for binding in entry.blackboard_reads
    )
    task_port = _TASK_OWNER_PORTS.get(entry.execution.owner_api)
    invocation_port = _INVOCATION_OWNER_PORTS.get(entry.execution.owner_api)
    if entry.execution.owner_api == "kernel.command_target":
        if row.node_id == "gripper.command_position":
            task_port = "kernel_operations"
        elif row.node_id == "gripper.check_status":
            invocation_port = "gripper_status"
    if task_port is not None and invocation_port is not None:
        raise ExecutableNodeResolverError(
            f"node {row.node_id!r} has ambiguous owner-port binding"
        )
    if task_port is not None:
        return _AdapterDefinition(
            row,
            _owner_task_builder(
                row.node_id,
                task_port,
                output_key_parameter=output_key_parameter,
                blackboard_read_parameters=blackboard_read_parameters,
                require_initial_running=row.node_id == "assistant.query_model",
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
                output_key_parameter=output_key_parameter,
                blackboard_read_parameters=blackboard_read_parameters,
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
            raise ExecutableNodeResolverError(
                "retained environment contains duplicate exact rows"
            )
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
            for definition in sorted(
                self._definitions.values(), key=lambda item: item.row
            )
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
