"""Provisional ROS-free contracts for the isolated I1a prototypes.

These records are selection evidence, not production or wire contracts. Their
schema is deliberately experimental and has no compatibility promise.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, fields, is_dataclass
from enum import Enum
import hashlib
import json
from pathlib import PurePosixPath
import re
import types
from typing import Any, ClassVar, TypeVar, Union, get_args, get_origin, get_type_hints


SCHEMA_ID = "manyforge.experimental.composable_cell.i1a.v0"
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class ContractError(ValueError):
    """Raised when an experimental contract is malformed or ambiguous."""


@dataclass(frozen=True, slots=True)
class StringValue:
    value: str

    def __post_init__(self) -> None:
        if not isinstance(self.value, str) or not self.value.strip():
            raise ContractError(f"{type(self).__name__} must be a non-empty string")
        if self.value != self.value.strip():
            raise ContractError(f"{type(self).__name__} must not contain edge spaces")

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True, slots=True)
class RevisionValue:
    value: str | int

    def __post_init__(self) -> None:
        if isinstance(self.value, bool) or not isinstance(self.value, (str, int)):
            raise ContractError(f"{type(self).__name__} must be a string or integer")
        if isinstance(self.value, str) and (
            not self.value.strip() or self.value != self.value.strip()
        ):
            raise ContractError(f"{type(self).__name__} must be non-empty and trimmed")
        if isinstance(self.value, int) and self.value < 0:
            raise ContractError(f"{type(self).__name__} must be non-negative")

    def __str__(self) -> str:
        return str(self.value)

    def __int__(self) -> int:
        try:
            return int(self.value)
        except (TypeError, ValueError) as exc:
            raise ContractError(
                f"{type(self).__name__} {self.value!r} is not numeric"
            ) from exc


@dataclass(frozen=True, slots=True)
class CounterRevisionValue(RevisionValue):
    """A monotonic revision used for ordering and compare-and-swap."""

    value: int

    def __post_init__(self) -> None:
        if isinstance(self.value, bool) or not isinstance(self.value, int):
            raise ContractError(
                f"{type(self).__name__} must be a non-negative integer"
            )
        if self.value < 0:
            raise ContractError(
                f"{type(self).__name__} must be a non-negative integer"
            )


class RuntimeUnitId(StringValue):
    pass


class ComponentTypeId(StringValue):
    pass


class ControllableComponentId(StringValue):
    pass


class CommandTargetId(StringValue):
    pass


class CapabilityId(StringValue):
    pass


class ModelAssetId(StringValue):
    pass


class ModelInstanceId(StringValue):
    pass


class ControlDomainId(StringValue):
    pass


class ControlBindingId(StringValue):
    pass


class AttachmentId(StringValue):
    pass


class ControllerTopologyId(StringValue):
    pass


class ControllerId(StringValue):
    pass


class RouteId(StringValue):
    pass


class ApplyTransactionId(StringValue):
    pass


class RequestId(StringValue):
    pass


class LifecycleTransactionId(StringValue):
    pass


class OperationId(StringValue):
    pass


class StopTransactionId(StringValue):
    pass


class GoalId(StringValue):
    pass


class ResourceId(StringValue):
    pass


class RouteSelectionTransactionId(StringValue):
    pass


class AlarmId(StringValue):
    pass


class AlarmOccurrenceId(StringValue):
    pass


class GraphRevision(CounterRevisionValue):
    pass


class RuntimeEpoch(RevisionValue):
    pass


class ModelRevision(RevisionValue):
    pass


class InterfaceContractRevision(RevisionValue):
    pass


class ControllerTopologyRevision(CounterRevisionValue):
    pass


class RouteGeneration(CounterRevisionValue):
    pass


class BindingRevision(CounterRevisionValue):
    pass


class AttachmentRevision(RevisionValue):
    pass


class SceneRevision(RevisionValue):
    pass


class ValidationRevision(RevisionValue):
    pass


class CompilerRevision(RevisionValue):
    pass


class CatalogRevision(RevisionValue):
    pass


class ComponentStateRevision(CounterRevisionValue):
    pass


class ControlDomainStateRevision(CounterRevisionValue):
    pass


class CellStateRevision(CounterRevisionValue):
    pass


class AlarmRevision(CounterRevisionValue):
    pass


class Sha256Digest(StringValue):
    def __post_init__(self) -> None:
        super().__post_init__()
        if _SHA256_RE.fullmatch(self.value) is None:
            raise ContractError("Sha256Digest must be 64 lowercase hexadecimal chars")


def _encode(value: Any) -> Any:
    if isinstance(value, (StringValue, RevisionValue)):
        return value.value
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value):
        return {
            field.name: _encode(getattr(value, field.name))
            for field in fields(value)
        }
    if isinstance(value, Mapping):
        return {
            str(key): _encode(item)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, (tuple, list)):
        return [_encode(item) for item in value]
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    raise ContractError(f"cannot serialize {type(value).__name__}")


def canonical_json(value: Any) -> str:
    return json.dumps(
        _encode(value),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    )


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def _decode(annotation: Any, raw: Any, *, path: str) -> Any:
    origin = get_origin(annotation)
    args = get_args(annotation)
    if origin in (Union, types.UnionType):
        if raw is None and type(None) in args:
            return None
        failures: list[str] = []
        for candidate in args:
            if candidate is type(None):
                continue
            try:
                return _decode(candidate, raw, path=path)
            except (ContractError, TypeError, ValueError) as exc:
                failures.append(str(exc))
        raise ContractError(f"{path} matches no union member: {'; '.join(failures)}")
    if origin is tuple:
        if not isinstance(raw, list):
            raise ContractError(f"{path} must be an array")
        if len(args) == 2 and args[1] is Ellipsis:
            return tuple(_decode(args[0], item, path=f"{path}[]") for item in raw)
        if len(raw) != len(args):
            raise ContractError(f"{path} must have {len(args)} items")
        return tuple(
            _decode(item_type, item, path=f"{path}[{index}]")
            for index, (item_type, item) in enumerate(zip(args, raw, strict=True))
        )
    if origin in (dict, Mapping):
        if not isinstance(raw, dict):
            raise ContractError(f"{path} must be an object")
        key_type, value_type = args
        return {
            _decode(key_type, key, path=f"{path}.key"): _decode(
                value_type, value, path=f"{path}.{key}"
            )
            for key, value in raw.items()
        }
    if isinstance(annotation, type) and issubclass(
        annotation, (StringValue, RevisionValue)
    ):
        return annotation(raw)
    if isinstance(annotation, type) and issubclass(annotation, Enum):
        try:
            return annotation(raw)
        except ValueError as exc:
            raise ContractError(f"{path} has unknown enum value {raw!r}") from exc
    if isinstance(annotation, type) and issubclass(annotation, ContractRecord):
        if not isinstance(raw, dict):
            raise ContractError(f"{path} must be an object")
        return annotation.from_dict(raw)
    if annotation is Any:
        return raw
    if annotation is str:
        if not isinstance(raw, str):
            raise ContractError(f"{path} must be a string")
        return raw
    if annotation is bool:
        if not isinstance(raw, bool):
            raise ContractError(f"{path} must be a boolean")
        return raw
    if annotation is int:
        if isinstance(raw, bool) or not isinstance(raw, int):
            raise ContractError(f"{path} must be an integer")
        return raw
    if annotation is float:
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            raise ContractError(f"{path} must be a number")
        return float(raw)
    raise ContractError(f"{path} uses unsupported annotation {annotation!r}")


_Record = TypeVar("_Record", bound="ContractRecord")


@dataclass(frozen=True, slots=True)
class ContractRecord:
    """Strict deterministic record shared only by the I1a prototypes."""

    SCHEMA: ClassVar[str] = SCHEMA_ID

    def to_dict(self) -> dict[str, Any]:
        encoded = _encode(self)
        assert isinstance(encoded, dict)
        return encoded

    def to_json(self) -> str:
        return canonical_json(self)

    def sha256(self) -> str:
        return canonical_sha256(self)

    @classmethod
    def from_dict(cls: type[_Record], raw: Mapping[str, Any]) -> _Record:
        if not isinstance(raw, Mapping):
            raise ContractError(f"{cls.__name__} must be an object")
        hints = get_type_hints(cls)
        expected = {field.name for field in fields(cls)}
        actual = set(raw)
        if actual != expected:
            unknown = sorted(actual - expected)
            missing = sorted(expected - actual)
            raise ContractError(
                f"{cls.__name__} fields mismatch; unknown={unknown}, missing={missing}"
            )
        decoded = {
            field.name: _decode(
                hints[field.name],
                raw[field.name],
                path=f"{cls.__name__}.{field.name}",
            )
            for field in fields(cls)
        }
        return cls(**decoded)

    @classmethod
    def from_json(cls: type[_Record], payload: str) -> _Record:
        try:
            raw = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ContractError(f"invalid {cls.__name__} JSON") from exc
        if not isinstance(raw, dict):
            raise ContractError(f"{cls.__name__} JSON must contain an object")
        return cls.from_dict(raw)


def _unique(values: tuple[Any, ...], *, label: str) -> None:
    if len(values) != len(set(values)):
        raise ContractError(f"{label} must be unique")


def _require_instances(
    values: tuple[Any, ...],
    expected_type: type[Any],
    *,
    label: str,
) -> None:
    if not isinstance(values, tuple) or any(
        not isinstance(value, expected_type) for value in values
    ):
        raise ContractError(
            f"{label} must contain only {expected_type.__name__} values"
        )


def _required_trimmed(value: str, *, label: str) -> None:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ContractError(f"{label} must be a non-empty trimmed string")


def _reject_directed_cycle(
    nodes: tuple[Any, ...],
    dependencies: Mapping[Any, tuple[Any, ...]],
    *,
    label: str,
) -> None:
    """Reject a cycle in a small immutable prototype graph."""

    visiting: set[Any] = set()
    visited: set[Any] = set()

    def visit(node: Any) -> None:
        if node in visiting:
            raise ContractError(f"{label} contains a cycle")
        if node in visited:
            return
        visiting.add(node)
        for dependency in dependencies.get(node, ()):
            visit(dependency)
        visiting.remove(node)
        visited.add(node)

    for node in nodes:
        visit(node)


@dataclass(frozen=True, slots=True)
class CapabilitySpec(ContractRecord):
    capability_id: CapabilityId
    operation_kind: str
    required_interfaces: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not self.operation_kind:
            raise ContractError("operation_kind is required")
        _unique(self.required_interfaces, label="required_interfaces")


@dataclass(frozen=True, slots=True)
class CommandTargetSpec(ContractRecord):
    command_target_id: CommandTargetId
    srdf_groups: tuple[str, ...]
    resource_ids: tuple[ResourceId, ...]
    capability_ids: tuple[CapabilityId, ...]

    def __post_init__(self) -> None:
        _unique(self.srdf_groups, label="srdf_groups")
        _unique(self.resource_ids, label="resource_ids")
        _unique(self.capability_ids, label="capability_ids")


@dataclass(frozen=True, slots=True)
class ModelAssetRecord(ContractRecord):
    model_asset_id: ModelAssetId
    model_revision: ModelRevision
    source_uri: str
    content_sha256: Sha256Digest

    def __post_init__(self) -> None:
        if not self.source_uri:
            raise ContractError("source_uri is required")


@dataclass(frozen=True, slots=True)
class ComponentTypeRecord(ContractRecord):
    component_type_id: ComponentTypeId
    model_asset_id: ModelAssetId
    command_targets: tuple[CommandTargetSpec, ...]
    capabilities: tuple[CapabilitySpec, ...]

    def __post_init__(self) -> None:
        _unique(
            tuple(item.command_target_id for item in self.command_targets),
            label="command target IDs",
        )
        _unique(
            tuple(item.capability_id for item in self.capabilities),
            label="capability IDs",
        )
        known = {item.capability_id for item in self.capabilities}
        for target in self.command_targets:
            if not set(target.capability_ids).issubset(known):
                raise ContractError("command target references unknown capability")


@dataclass(frozen=True, slots=True)
class ControlInterfaceContract(ContractRecord):
    interface_contract_revision: InterfaceContractRevision
    ordered_resources: tuple[ResourceId, ...]
    command_interfaces: tuple[str, ...]
    state_interfaces: tuple[str, ...]

    def __post_init__(self) -> None:
        _unique(self.ordered_resources, label="ordered_resources")
        _unique(self.command_interfaces, label="command_interfaces")
        _unique(self.state_interfaces, label="state_interfaces")


@dataclass(frozen=True, slots=True)
class ControllerSpec(ContractRecord):
    controller_id: ControllerId
    plugin: str
    claimed_resources: tuple[ResourceId, ...]
    chain_inputs: tuple[ControllerId, ...] = ()

    def __post_init__(self) -> None:
        if not self.plugin:
            raise ContractError("controller plugin is required")
        _unique(self.claimed_resources, label="claimed_resources")
        _unique(self.chain_inputs, label="chain_inputs")


@dataclass(frozen=True, slots=True)
class ControlRouteSpec(ContractRecord):
    route_id: RouteId
    capability_id: CapabilityId
    controllers: tuple[ControllerId, ...]
    chained: bool

    def __post_init__(self) -> None:
        if not self.controllers:
            raise ContractError("control route requires controllers")
        _unique(self.controllers, label="route controllers")


@dataclass(frozen=True, slots=True)
class ControllerTopologyRecord(ContractRecord):
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision
    controllers: tuple[ControllerSpec, ...]
    routes: tuple[ControlRouteSpec, ...]
    activation_sets: tuple[tuple[ControllerId, ...], ...]

    def __post_init__(self) -> None:
        controller_ids = tuple(item.controller_id for item in self.controllers)
        _unique(controller_ids, label="controller IDs")
        _unique(tuple(item.route_id for item in self.routes), label="route IDs")
        known = set(controller_ids)
        by_id = {item.controller_id: item for item in self.controllers}
        for controller in self.controllers:
            if not set(controller.chain_inputs).issubset(known):
                raise ContractError("controller chain references unknown controller")
        _reject_directed_cycle(
            controller_ids,
            {
                controller.controller_id: controller.chain_inputs
                for controller in self.controllers
            },
            label="controller chain",
        )
        for route in self.routes:
            if not set(route.controllers).issubset(known):
                raise ContractError("route references unknown controller")
            if route.chained:
                if len(route.controllers) < 2:
                    raise ContractError(
                        "chained route requires at least two controllers"
                    )
                for upstream, downstream in zip(
                    route.controllers,
                    route.controllers[1:],
                    strict=False,
                ):
                    if upstream not in by_id[downstream].chain_inputs:
                        raise ContractError(
                            "chained route controller direction is invalid"
                        )
            elif len(route.controllers) != 1:
                raise ContractError(
                    "non-chained route requires exactly one controller"
                )
        normalized_activation_sets: list[frozenset[ControllerId]] = []
        for activation_set in self.activation_sets:
            if not activation_set:
                raise ContractError("activation set cannot be empty")
            _unique(activation_set, label="activation-set controllers")
            if not set(activation_set).issubset(known):
                raise ContractError("activation set references unknown controller")
            normalized = frozenset(activation_set)
            if normalized in normalized_activation_sets:
                raise ContractError("activation sets must be unique")
            normalized_activation_sets.append(normalized)
            claimed_by_resource: dict[ResourceId, ControllerId] = {}
            for controller_id in activation_set:
                for resource_id in by_id[controller_id].claimed_resources:
                    previous = claimed_by_resource.setdefault(
                        resource_id,
                        controller_id,
                    )
                    if previous != controller_id:
                        raise ContractError(
                            "activation set has a resource-claim conflict"
                        )
        for route in self.routes:
            if not any(
                set(route.controllers).issubset(activation_set)
                for activation_set in normalized_activation_sets
            ):
                raise ContractError(
                    "route controllers have no legal activation set"
                )


@dataclass(frozen=True, slots=True)
class RuntimeUnitSpec(ContractRecord):
    runtime_unit_id: RuntimeUnitId
    kind: str
    dependencies: tuple[RuntimeUnitId, ...] = ()
    command: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not self.kind:
            raise ContractError("runtime unit kind is required")
        _unique(self.dependencies, label="runtime unit dependencies")


@dataclass(frozen=True, slots=True)
class ControllableComponentSpec(ContractRecord):
    controllable_component_id: ControllableComponentId
    component_type_id: ComponentTypeId
    model_instance_id: ModelInstanceId
    control_binding_ids: tuple[ControlBindingId, ...] = ()

    def __post_init__(self) -> None:
        _unique(self.control_binding_ids, label="component control bindings")


@dataclass(frozen=True, slots=True)
class ModelInstanceSpec(ContractRecord):
    model_instance_id: ModelInstanceId
    model_asset_id: ModelAssetId
    prefix: str
    parent_frame: str = "world"

    def __post_init__(self) -> None:
        if not self.prefix:
            raise ContractError("model instance prefix is required")
        if not self.parent_frame:
            raise ContractError("model instance parent_frame is required")


@dataclass(frozen=True, slots=True)
class ControlDomainSpec(ContractRecord):
    control_domain_id: ControlDomainId
    runtime_unit_id: RuntimeUnitId
    interface_contract_revision: InterfaceContractRevision
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision


@dataclass(frozen=True, slots=True)
class ControlBindingSpec(ContractRecord):
    control_binding_id: ControlBindingId
    binding_revision: BindingRevision
    controllable_component_id: ControllableComponentId
    control_domain_id: ControlDomainId
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    route_id: RouteId
    interface_contract_revision: InterfaceContractRevision
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision
    resource_ids: tuple[ResourceId, ...]
    endpoints: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.resource_ids:
            raise ContractError("control binding requires resources")
        _require_instances(
            self.resource_ids,
            ResourceId,
            label="binding resources",
        )
        _unique(self.resource_ids, label="binding resources")
        if not self.endpoints:
            raise ContractError("control binding requires endpoints")
        for endpoint in self.endpoints:
            _required_trimmed(endpoint, label="binding endpoint")
        _unique(self.endpoints, label="binding endpoints")


@dataclass(frozen=True, slots=True)
class AttachmentEdgeSpec(ContractRecord):
    attachment_id: AttachmentId
    attachment_revision: AttachmentRevision
    parent_model_instance_id: ModelInstanceId
    child_model_instance_id: ModelInstanceId
    parent_frame: str
    child_frame: str

    def __post_init__(self) -> None:
        if self.parent_model_instance_id == self.child_model_instance_id:
            raise ContractError("attachment cannot self-reference")
        if not self.parent_frame or not self.child_frame:
            raise ContractError("attachment frames are required")


@dataclass(frozen=True, slots=True)
class CellGraph(ContractRecord):
    schema: str
    graph_revision: GraphRevision
    runtime_units: tuple[RuntimeUnitSpec, ...] = ()
    controllable_components: tuple[ControllableComponentSpec, ...] = ()
    model_instances: tuple[ModelInstanceSpec, ...] = ()
    control_domains: tuple[ControlDomainSpec, ...] = ()
    control_bindings: tuple[ControlBindingSpec, ...] = ()
    attachment_edges: tuple[AttachmentEdgeSpec, ...] = ()

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError(f"unsupported CellGraph schema {self.schema!r}")
        collections: tuple[tuple[str, tuple[Any, ...], str], ...] = (
            ("runtime units", self.runtime_units, "runtime_unit_id"),
            (
                "controllable components",
                self.controllable_components,
                "controllable_component_id",
            ),
            ("model instances", self.model_instances, "model_instance_id"),
            ("control domains", self.control_domains, "control_domain_id"),
            ("control bindings", self.control_bindings, "control_binding_id"),
            ("attachment edges", self.attachment_edges, "attachment_id"),
        )
        for label, values, identity_field in collections:
            _unique(
                tuple(getattr(item, identity_field) for item in values),
                label=label,
            )
        runtime_ids = {item.runtime_unit_id for item in self.runtime_units}
        for unit in self.runtime_units:
            if not set(unit.dependencies).issubset(runtime_ids):
                raise ContractError("runtime unit references unknown dependency")
            if unit.runtime_unit_id in unit.dependencies:
                raise ContractError("runtime unit cannot depend on itself")
        _reject_directed_cycle(
            tuple(runtime_ids),
            {
                unit.runtime_unit_id: unit.dependencies
                for unit in self.runtime_units
            },
            label="runtime dependency graph",
        )
        model_ids = {item.model_instance_id for item in self.model_instances}
        _unique(
            tuple(item.prefix for item in self.model_instances),
            label="model instance prefixes",
        )
        binding_ids = {item.control_binding_id for item in self.control_bindings}
        binding_by_id = {
            item.control_binding_id: item for item in self.control_bindings
        }
        component_by_id = {
            item.controllable_component_id: item
            for item in self.controllable_components
        }
        component_ids = {
            item.controllable_component_id for item in self.controllable_components
        }
        domain_ids = {item.control_domain_id for item in self.control_domains}
        domain_by_id = {
            item.control_domain_id: item for item in self.control_domains
        }
        for component in self.controllable_components:
            if component.model_instance_id not in model_ids:
                raise ContractError("component references unknown model instance")
            if not set(component.control_binding_ids).issubset(binding_ids):
                raise ContractError("component references unknown control binding")
            for binding_id in component.control_binding_ids:
                if (
                    binding_by_id[binding_id].controllable_component_id
                    != component.controllable_component_id
                ):
                    raise ContractError(
                        "component and binding references are not reciprocal"
                    )
        for binding in self.control_bindings:
            if binding.controllable_component_id not in component_ids:
                raise ContractError("binding references unknown component")
            if binding.control_domain_id not in domain_ids:
                raise ContractError("binding references unknown control domain")
            domain = domain_by_id[binding.control_domain_id]
            if (
                binding.interface_contract_revision
                != domain.interface_contract_revision
            ):
                raise ContractError(
                    "binding interface revision does not match its control domain"
                )
            if binding.controller_topology_id != domain.controller_topology_id:
                raise ContractError(
                    "binding topology does not match its control domain"
                )
            if binding.topology_revision != domain.topology_revision:
                raise ContractError(
                    "binding topology revision does not match its control domain"
                )
            owner = component_by_id[binding.controllable_component_id]
            if binding.control_binding_id not in owner.control_binding_ids:
                raise ContractError(
                    "component and binding references are not reciprocal"
                )
        for domain in self.control_domains:
            if domain.runtime_unit_id not in runtime_ids:
                raise ContractError("domain references unknown runtime unit")
        for edge in self.attachment_edges:
            if (
                edge.parent_model_instance_id not in model_ids
                or edge.child_model_instance_id not in model_ids
            ):
                raise ContractError("attachment references unknown model instance")
        attachment_parents: dict[ModelInstanceId, ModelInstanceId] = {}
        for edge in self.attachment_edges:
            if edge.child_model_instance_id in attachment_parents:
                raise ContractError(
                    "attachment child must have exactly one parent edge"
                )
            attachment_parents[edge.child_model_instance_id] = (
                edge.parent_model_instance_id
            )
        _reject_directed_cycle(
            tuple(model_ids),
            {
                child: (parent,)
                for child, parent in attachment_parents.items()
            },
            label="attachment graph",
        )


@dataclass(frozen=True, slots=True)
class RequestMetadata(ContractRecord):
    request_id: RequestId
    source_label: str
    transport: str
    correlation_id: str = ""
    submitted_monotonic_ns: int = 0

    def __post_init__(self) -> None:
        if not self.source_label or not self.transport:
            raise ContractError("request source_label and transport are required")
        if self.submitted_monotonic_ns < 0:
            raise ContractError("submitted_monotonic_ns must be non-negative")


@dataclass(frozen=True, slots=True)
class RuntimeUnitObservation(ContractRecord):
    runtime_unit_id: RuntimeUnitId
    runtime_epoch: RuntimeEpoch
    health: str
    observed_monotonic_ns: int
    process_id: int | None = None
    detail: str = ""


@dataclass(frozen=True, slots=True)
class ObservedState(ContractRecord):
    schema: str
    graph_revision: GraphRevision
    observed_monotonic_ns: int
    runtime_units: tuple[RuntimeUnitObservation, ...]

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError("unexpected ObservedState schema")
        _unique(
            tuple(item.runtime_unit_id for item in self.runtime_units),
            label="runtime observations",
        )


@dataclass(frozen=True, slots=True)
class ApplyRequest(ContractRecord):
    metadata: RequestMetadata
    apply_id: ApplyTransactionId
    expected_graph_revision: GraphRevision
    desired_graph: CellGraph
    busy_disposition: str = "reject_if_affected_busy"


@dataclass(frozen=True, slots=True)
class ApplyStatus(ContractRecord):
    apply_id: ApplyTransactionId
    phase: str
    from_graph_revision: GraphRevision
    target_graph_revision: GraphRevision
    affected_runtime_units: tuple[RuntimeUnitId, ...] = ()
    detail: str = ""


class RequestKind(str, Enum):
    START = "start"
    STOP = "stop"
    FULL_STOP = "full_stop"
    APPLY = "apply"
    PREPARE = "prepare"
    ENABLE = "enable"
    DISABLE = "disable"
    RECOVER = "recover"
    SELECT_ROUTE = "select_route"
    RESUME = "resume"
    EXECUTE = "execute"


@dataclass(frozen=True, slots=True)
class RequestScope(ContractRecord):
    control_domain_ids: tuple[ControlDomainId, ...] = ()
    resource_ids: tuple[ResourceId, ...] = ()

    def __post_init__(self) -> None:
        if not self.control_domain_ids and not self.resource_ids:
            raise ContractError("ordinary request scope cannot be empty")
        _require_instances(
            self.control_domain_ids,
            ControlDomainId,
            label="scope domains",
        )
        _require_instances(
            self.resource_ids,
            ResourceId,
            label="scope resources",
        )
        _unique(self.control_domain_ids, label="scope domains")
        _unique(self.resource_ids, label="scope resources")


@dataclass(frozen=True, slots=True)
class DomainRequestPrecondition(ContractRecord):
    control_domain_id: ControlDomainId
    expected_control_domain_state_revision: ControlDomainStateRevision | None = None
    expected_topology_revision: ControllerTopologyRevision | None = None
    expected_route_generation: RouteGeneration | None = None
    expected_binding_revision: BindingRevision | None = None


@dataclass(frozen=True, slots=True)
class RequestPreconditions(ContractRecord):
    expected_graph_revision: GraphRevision | None = None
    domains: tuple[DomainRequestPrecondition, ...] = ()

    def __post_init__(self) -> None:
        _unique(
            tuple(item.control_domain_id for item in self.domains),
            label="request precondition domains",
        )


@dataclass(frozen=True, slots=True)
class ControlRequest(ContractRecord):
    metadata: RequestMetadata
    kind: RequestKind
    scope: RequestScope
    preconditions: RequestPreconditions
    payload_sha256: str

    def __post_init__(self) -> None:
        if _SHA256_RE.fullmatch(self.payload_sha256) is None:
            raise ContractError("payload_sha256 must be lowercase SHA-256")
        scope_domains = set(self.scope.control_domain_ids)
        precondition_domains = {
            item.control_domain_id for item in self.preconditions.domains
        }
        if not precondition_domains.issubset(scope_domains):
            raise ContractError(
                "request preconditions reference a domain outside request scope"
            )


class LifecycleIntent(str, Enum):
    PREPARE = "prepare"
    ENABLE = "enable"
    DISABLE = "disable"
    RECOVER = "recover"


class LifecyclePhase(str, Enum):
    ACCEPTED = "accepted"
    RUNNING = "running"
    SUCCEEDED = "succeeded"
    REJECTED = "rejected"
    FAILED = "failed"


class LifecycleReadiness(str, Enum):
    UNKNOWN = "unknown"
    NOT_READY = "not_ready"
    READY = "ready"
    DISABLED = "disabled"
    FAULTED = "faulted"


@dataclass(frozen=True, slots=True)
class LifecycleRequest(ContractRecord):
    metadata: RequestMetadata
    lifecycle_transaction_id: LifecycleTransactionId
    controllable_component_id: ControllableComponentId
    intent: LifecycleIntent
    expected_graph_revision: GraphRevision
    expected_component_state_revision: ComponentStateRevision


@dataclass(frozen=True, slots=True)
class LifecycleStatus(ContractRecord):
    lifecycle_transaction_id: LifecycleTransactionId
    controllable_component_id: ControllableComponentId
    intent: LifecycleIntent
    phase: LifecyclePhase
    component_state_revision: ComponentStateRevision
    readiness: LifecycleReadiness
    detail: str = ""


class RouteSelectionPhase(str, Enum):
    ACCEPTED = "accepted"
    SWITCHING = "switching"
    SELECTED = "selected"
    REJECTED = "rejected"
    FAILED = "failed"


@dataclass(frozen=True, slots=True)
class RouteSelectionPreconditions(ContractRecord):
    expected_control_domain_state_revision: ControlDomainStateRevision
    expected_interface_contract_revision: InterfaceContractRevision
    expected_topology_revision: ControllerTopologyRevision
    expected_route_generation: RouteGeneration
    expected_binding_revision: BindingRevision


@dataclass(frozen=True, slots=True)
class RouteSelectionRequest(ContractRecord):
    metadata: RequestMetadata
    route_selection_id: RouteSelectionTransactionId
    controllable_component_id: ControllableComponentId
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    control_domain_id: ControlDomainId
    control_binding_id: ControlBindingId
    route_id: RouteId
    preconditions: RouteSelectionPreconditions


@dataclass(frozen=True, slots=True)
class RouteSelectionStatus(ContractRecord):
    route_selection_id: RouteSelectionTransactionId
    controllable_component_id: ControllableComponentId
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    control_domain_id: ControlDomainId
    control_binding_id: ControlBindingId
    route_id: RouteId
    phase: RouteSelectionPhase
    control_domain_state_revision: ControlDomainStateRevision
    interface_contract_revision: InterfaceContractRevision
    topology_revision: ControllerTopologyRevision
    route_generation: RouteGeneration
    binding_revision: BindingRevision
    detail: str = ""


@dataclass(frozen=True, slots=True)
class OperationParticipant(ContractRecord):
    controllable_component_id: ControllableComponentId
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    control_domain_id: ControlDomainId
    control_binding_id: ControlBindingId
    binding_revision: BindingRevision
    model_revision: ModelRevision
    scene_revision: SceneRevision
    attachment_revision: AttachmentRevision | None
    interface_contract_revision: InterfaceContractRevision
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision
    route_id: RouteId
    route_generation: RouteGeneration
    validation_revision: ValidationRevision
    resource_ids: tuple[ResourceId, ...]

    def __post_init__(self) -> None:
        if not self.resource_ids:
            raise ContractError("operation participant requires resources")
        _require_instances(
            self.resource_ids,
            ResourceId,
            label="participant resources",
        )
        _unique(self.resource_ids, label="participant resources")


@dataclass(frozen=True, slots=True)
class OperationRequest(ContractRecord):
    metadata: RequestMetadata
    operation_id: OperationId
    expected_graph_revision: GraphRevision
    participants: tuple[OperationParticipant, ...]
    coordinated: bool = False

    def __post_init__(self) -> None:
        if not self.participants:
            raise ContractError("operation requires participants")
        _unique(
            tuple(item.control_binding_id for item in self.participants),
            label="operation bindings",
        )


@dataclass(frozen=True, slots=True)
class OperationStatus(ContractRecord):
    operation_id: OperationId
    phase: str
    dispatched_bindings: tuple[ControlBindingId, ...] = ()
    terminal_detail: str = ""


@dataclass(frozen=True, slots=True)
class StopScope(ContractRecord):
    operation_ids: tuple[OperationId, ...] = ()
    control_domain_ids: tuple[ControlDomainId, ...] = ()
    resource_ids: tuple[ResourceId, ...] = ()

    def __post_init__(self) -> None:
        _require_instances(
            self.operation_ids,
            OperationId,
            label="Stop operation IDs",
        )
        _require_instances(
            self.control_domain_ids,
            ControlDomainId,
            label="Stop domain IDs",
        )
        _require_instances(
            self.resource_ids,
            ResourceId,
            label="Stop resource IDs",
        )
        _unique(self.operation_ids, label="Stop operation IDs")
        _unique(self.control_domain_ids, label="Stop domain IDs")
        _unique(self.resource_ids, label="Stop resource IDs")


@dataclass(frozen=True, slots=True)
class StopRequest(ContractRecord):
    metadata: RequestMetadata
    stop_id: StopTransactionId
    scope: StopScope
    absolute_deadline_ns: int

    def __post_init__(self) -> None:
        if self.absolute_deadline_ns < 0:
            raise ContractError("Stop deadline must be non-negative")


@dataclass(frozen=True, slots=True)
class StopPhaseBudget(ContractRecord):
    name: str
    absolute_deadline_ns: int

    def __post_init__(self) -> None:
        if not self.name or self.absolute_deadline_ns < 0:
            raise ContractError("invalid Stop phase budget")


@dataclass(frozen=True, slots=True)
class StoppedGoalSnapshot(ContractRecord):
    control_domain_id: ControlDomainId
    operation_id: OperationId
    control_binding_id: ControlBindingId
    goal_id: GoalId


@dataclass(frozen=True, slots=True)
class StopCertificate(ContractRecord):
    stop_id: StopTransactionId
    outcome: str
    completed_monotonic_ns: int
    stopped_domains: tuple[ControlDomainId, ...] = ()
    stopped_resources: tuple[ResourceId, ...] = ()
    stopped_goals: tuple[StoppedGoalSnapshot, ...] = ()
    measured_rest_domains: tuple[ControlDomainId, ...] = ()
    detail: str = ""

    def __post_init__(self) -> None:
        _required_trimmed(self.outcome, label="Stop outcome")
        if self.completed_monotonic_ns < 0:
            raise ContractError("Stop completion time must be non-negative")
        _require_instances(
            self.stopped_resources,
            ResourceId,
            label="stopped resources",
        )
        _unique(self.stopped_domains, label="stopped domains")
        _unique(self.stopped_resources, label="stopped resources")
        _unique(
            tuple(item.goal_id for item in self.stopped_goals),
            label="stopped goals",
        )
        _unique(
            self.measured_rest_domains,
            label="measured-rest domains",
        )


class AlarmState(str, Enum):
    ACTIVE_UNACKNOWLEDGED = "active_unacknowledged"
    ACTIVE_ACKNOWLEDGED = "active_acknowledged"
    RESOLVED_UNACKNOWLEDGED = "resolved_unacknowledged"
    RESOLVED_ACKNOWLEDGED = "resolved_acknowledged"


class AlarmSeverity(str, Enum):
    INFO = "info"
    WARNING = "warning"
    ERROR = "error"


@dataclass(frozen=True, slots=True)
class AlarmSnapshot(ContractRecord):
    alarm_id: AlarmId
    occurrence_id: AlarmOccurrenceId
    alarm_revision: AlarmRevision
    state: AlarmState
    severity: AlarmSeverity
    condition_code: str
    subject_kind: str
    subject_id: str
    message: str

    def __post_init__(self) -> None:
        for label, value in (
            ("alarm condition_code", self.condition_code),
            ("alarm subject_kind", self.subject_kind),
            ("alarm subject_id", self.subject_id),
            ("alarm message", self.message),
        ):
            _required_trimmed(value, label=label)


@dataclass(frozen=True, slots=True)
class ActiveRouteSnapshot(ContractRecord):
    control_binding_id: ControlBindingId
    binding_revision: BindingRevision
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    route_id: RouteId
    route_generation: RouteGeneration


@dataclass(frozen=True, slots=True)
class ComponentStateSnapshot(ContractRecord):
    controllable_component_id: ControllableComponentId
    component_state_revision: ComponentStateRevision
    readiness: LifecycleReadiness
    active_bindings: tuple[ActiveRouteSnapshot, ...] = ()
    detail: str = ""

    def __post_init__(self) -> None:
        _unique(
            tuple(item.control_binding_id for item in self.active_bindings),
            label="component active bindings",
        )


@dataclass(frozen=True, slots=True)
class ControlDomainStateSnapshot(ContractRecord):
    control_domain_id: ControlDomainId
    control_domain_state_revision: ControlDomainStateRevision
    runtime_unit_id: RuntimeUnitId
    runtime_epoch: RuntimeEpoch
    health: str
    interface_contract_revision: InterfaceContractRevision
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision
    active_routes: tuple[ActiveRouteSnapshot, ...] = ()
    detail: str = ""

    def __post_init__(self) -> None:
        _required_trimmed(self.health, label="control-domain health")
        _unique(
            tuple(item.control_binding_id for item in self.active_routes),
            label="control-domain active bindings",
        )


@dataclass(frozen=True, slots=True)
class CellStateSnapshot(ContractRecord):
    schema: str
    cell_state_revision: CellStateRevision
    graph_revision: GraphRevision
    scene_revision: SceneRevision
    observed_monotonic_ns: int
    runtime_units: tuple[RuntimeUnitObservation, ...]
    components: tuple[ComponentStateSnapshot, ...]
    control_domains: tuple[ControlDomainStateSnapshot, ...]
    active_operations: tuple[OperationId, ...]
    alarms: tuple[AlarmSnapshot, ...]

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError("unexpected cell-state schema")
        if self.observed_monotonic_ns < 0:
            raise ContractError("cell-state observation time must be non-negative")
        _unique(
            tuple(item.runtime_unit_id for item in self.runtime_units),
            label="cell-state runtime units",
        )
        _unique(
            tuple(item.controllable_component_id for item in self.components),
            label="cell-state components",
        )
        _unique(
            tuple(item.control_domain_id for item in self.control_domains),
            label="cell-state control domains",
        )
        _unique(self.active_operations, label="cell-state active operations")
        _unique(
            tuple(item.alarm_id for item in self.alarms),
            label="cell-state alarms",
        )


@dataclass(frozen=True, slots=True)
class SymbolBinding(ContractRecord):
    logical_name: str
    runtime_name: str
    kind: str

    def __post_init__(self) -> None:
        if not all(
            value and value == value.strip()
            for value in (self.logical_name, self.runtime_name, self.kind)
        ):
            raise ContractError("compiled symbol fields must be non-empty and trimmed")


@dataclass(frozen=True, slots=True)
class CompiledSymbolMap(ContractRecord):
    model_instance_id: ModelInstanceId
    symbols: tuple[SymbolBinding, ...]

    def __post_init__(self) -> None:
        _unique(
            tuple((item.kind, item.logical_name) for item in self.symbols),
            label="logical symbols",
        )
        _unique(
            tuple((item.kind, item.runtime_name) for item in self.symbols),
            label="runtime symbols",
        )


@dataclass(frozen=True, slots=True)
class RetainedArtifact(ContractRecord):
    name: str
    kind: str
    relative_path: str
    content_sha256: Sha256Digest

    def __post_init__(self) -> None:
        if not self.name or not self.kind or not self.relative_path:
            raise ContractError("retained artifact fields are required")
        if self.name != self.name.strip() or self.kind != self.kind.strip():
            raise ContractError("retained artifact name and kind must be trimmed")
        if "\\" in self.relative_path:
            raise ContractError("retained artifact path must use POSIX separators")
        candidate = PurePosixPath(self.relative_path)
        if (
            candidate.is_absolute()
            or candidate == PurePosixPath(".")
            or candidate.as_posix() != self.relative_path
            or any(part in {"", ".", ".."} for part in candidate.parts)
        ):
            raise ContractError(
                "retained artifact path must be a safe normalized relative path"
            )


@dataclass(frozen=True, slots=True)
class CompiledBindingSnapshot(ContractRecord):
    control_binding_id: ControlBindingId
    binding_revision: BindingRevision
    controllable_component_id: ControllableComponentId
    control_domain_id: ControlDomainId
    command_target_id: CommandTargetId
    capability_id: CapabilityId
    route_id: RouteId
    interface_contract_revision: InterfaceContractRevision
    controller_topology_id: ControllerTopologyId
    topology_revision: ControllerTopologyRevision
    ordered_resources: tuple[ResourceId, ...]
    endpoints: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.ordered_resources:
            raise ContractError("compiled binding requires ordered resources")
        _require_instances(
            self.ordered_resources,
            ResourceId,
            label="compiled binding resources",
        )
        _unique(self.ordered_resources, label="compiled binding resources")
        if not self.endpoints:
            raise ContractError("compiled binding requires endpoints")
        for endpoint in self.endpoints:
            _required_trimmed(endpoint, label="compiled binding endpoint")
        _unique(self.endpoints, label="compiled binding endpoints")


@dataclass(frozen=True, slots=True)
class RetainedBundleManifest(ContractRecord):
    schema: str
    compiler_revision: CompilerRevision
    catalog_revision: CatalogRevision
    graph_revision: GraphRevision
    artifacts: tuple[RetainedArtifact, ...]
    symbol_maps: tuple[CompiledSymbolMap, ...]
    bindings: tuple[CompiledBindingSnapshot, ...]
    predecessor_sha256: Sha256Digest | None = None

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError("unexpected retained-bundle schema")
        _unique(tuple(item.name for item in self.artifacts), label="artifact names")
        _unique(
            tuple(item.relative_path for item in self.artifacts),
            label="artifact paths",
        )
        _unique(
            tuple(item.model_instance_id for item in self.symbol_maps),
            label="compiled model instances",
        )
        _unique(
            tuple(item.control_binding_id for item in self.bindings),
            label="compiled bindings",
        )
        _unique(
            tuple(
                (symbol.kind, symbol.runtime_name)
                for symbol_map in self.symbol_maps
                for symbol in symbol_map.symbols
            ),
            label="global runtime symbols",
        )


class ClientDisposition(str, Enum):
    ACCEPTED = "accepted"
    RUNNING = "running"
    SUCCEEDED = "succeeded"
    REJECTED = "rejected"
    FAILED = "failed"


PublicRequestPayload = (
    ControlRequest
    | ApplyRequest
    | LifecycleRequest
    | RouteSelectionRequest
    | OperationRequest
    | StopRequest
)


@dataclass(frozen=True, slots=True)
class PublicClientRequest(ContractRecord):
    schema: str
    kind: RequestKind
    payload: PublicRequestPayload

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError("unexpected public-client request schema")
        if self.kind is RequestKind.APPLY:
            valid = isinstance(self.payload, ApplyRequest)
        elif self.kind in {
            RequestKind.PREPARE,
            RequestKind.ENABLE,
            RequestKind.DISABLE,
            RequestKind.RECOVER,
        }:
            valid = isinstance(self.payload, LifecycleRequest) and (
                self.payload.intent.value == self.kind.value
            )
        elif self.kind is RequestKind.SELECT_ROUTE:
            valid = isinstance(self.payload, RouteSelectionRequest)
        elif self.kind is RequestKind.EXECUTE:
            valid = isinstance(self.payload, OperationRequest)
        elif self.kind in {RequestKind.STOP, RequestKind.FULL_STOP}:
            valid = isinstance(self.payload, StopRequest)
        else:
            valid = isinstance(self.payload, ControlRequest) and (
                self.payload.kind is self.kind
            )
        if not valid:
            raise ContractError(
                "public-client request kind does not match its typed payload"
            )


PublicStatusPayload = (
    ApplyStatus
    | LifecycleStatus
    | RouteSelectionStatus
    | OperationStatus
    | StopCertificate
    | None
)


@dataclass(frozen=True, slots=True)
class PublicClientStatus(ContractRecord):
    schema: str
    request_id: RequestId
    kind: RequestKind
    disposition: ClientDisposition
    cell_state_revision: CellStateRevision
    payload: PublicStatusPayload
    detail: str = ""

    def __post_init__(self) -> None:
        if self.schema != SCHEMA_ID:
            raise ContractError("unexpected public-client status schema")
        if self.payload is None:
            return
        if self.kind is RequestKind.APPLY:
            valid = isinstance(self.payload, ApplyStatus)
        elif self.kind in {
            RequestKind.PREPARE,
            RequestKind.ENABLE,
            RequestKind.DISABLE,
            RequestKind.RECOVER,
        }:
            valid = isinstance(self.payload, LifecycleStatus) and (
                self.payload.intent.value == self.kind.value
            )
        elif self.kind is RequestKind.SELECT_ROUTE:
            valid = isinstance(self.payload, RouteSelectionStatus)
        elif self.kind in {RequestKind.START, RequestKind.EXECUTE, RequestKind.RESUME}:
            valid = isinstance(self.payload, OperationStatus)
        elif self.kind in {RequestKind.STOP, RequestKind.FULL_STOP}:
            valid = isinstance(self.payload, StopCertificate)
        else:
            valid = False
        if not valid:
            raise ContractError(
                "public-client status kind does not match its typed payload"
            )
