"""Strict v2 node-catalog model and validator.

The catalog is portable authored meaning.  It contains stable runner
implementation identities, not Python imports or runtime availability.
"""

from __future__ import annotations

import ipaddress
import re
from copy import deepcopy
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    load_strict_json_yaml,
    verify_omitted_field_digest,
)
from manyforge_behavior.catalog_schema import (
    CatalogSchema,
    CatalogSchemaError,
    CatalogSchemaRegistry,
    parse_schema,
    parse_schema_registry,
    validate_value,
)


EXPECTED_API_VERSION = "manyforge.node_catalog.v2"
EXPECTED_CONTENT_VERSION = "2.7.0"
BUILTIN_CATALOG_ID = "manyforge_builtin"
EXPECTED_BUILTIN_CATALOG_DIGEST = (
    "sha256:b9d90f6ec180af69c9f342b4d7c995668693ef16854e7ab8df177381c5db9806"
)
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
_COMPOSABLE_PROFILE_RE = re.compile(r"^composable-cell-v(\d+)$")
_ID_RE = re.compile(r"^[a-z][a-z0-9_.-]*$")
_ENTRY_KINDS = {"leaf", "decorator", "composite"}
_STABILITY_LEVELS = {"stable", "experimental", "deprecated"}
_CATEGORIES = {
    "assistant",
    "control_flow",
    "controller",
    "end_effector",
    "geometry",
    "gripper",
    "io",
    "lifecycle",
    "logic",
    "model",
    "motion",
    "perception",
    "recovery",
    "scene",
    "simulation",
    "state",
}
_OWNER_APIS = {
    "assistant.model_request",
    "assistant.scene_context",
    "assistant.scene_selection",
    "cclc.control_route",
    "cclc.lifecycle",
    "geometry.compare",
    "kernel.command_target",
    "kernel.full_stop",
    "kernel.operation",
    "kernel.recovery",
    "kernel.reset",
    "kernel.runtime_scaling",
    "kernel.stop",
    "model.kinematics",
    "orchestration.blackboard",
    "orchestration.clock",
    "orchestration.control_flow",
    "planning.pick_sequence",
    "planning.place_sequence",
    "scene.carried_attachment",
    "scene.query",
    "scene.transaction",
    "simulator.control",
}

_TARGET_CAPABILITIES = {
    "execution.contact.controller_reported",
    "execution.contact.force_torque",
    "execution.contact.geometric",
    "execution.contact.geometric_signed_penetration",
    "execution.control.admittance",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.reset",
    "execution.runtime_scale.operation_scoped",
    "execution.stop.motion.hard_deceleration",
    "execution.stop.motion.soft_on_path",
    "execution.stop.motion.spring_back_hold",
    "execution.stop.soft",
    "execution.trajectory.execute",
    "io.gpio.get_input",
    "io.gpio.set_output",
    "io.gpio.wait_for_input",
    "model.kinematics.fk",
    "model.limits.joint",
    "planning.motion.cartesian_path",
    "planning.motion.joint_goal",
    "planning.motion.named_target",
    "planning.motion.pose_goal",
    "planning.profile.balanced",
    "planning.profile.first_feasible",
    "planning.profile.quality_bounded",
    "planning.trajectory.time_parameterized",
    "scene.pose.observe",
    "state.observed.current",
    "tool.gripper.command",
    "tool.gripper.feedback",
    "validation.trajectory.non_collision",
}
_CELL_CAPABILITIES = {
    "collision.query.detailed",
    "collision.query.pre_execution",
    "collision.query.runtime",
    "execution.interrupt.contact_policy",
    "execution.interrupt.resource_scoped",
    "observability.logging.operational",
    "observability.trace.kernel",
    "observability.trace.scene",
    "perception.pose_updates",
    "scene.edit.add",
    "scene.edit.attach_detach",
    "scene.edit.remove",
    "scene.edit.update_pose",
    "scene.edit.upsert",
    "scene.object.query",
    "scene.projection.revision_bound",
    "scene.reference.resolve",
    "scene.shape.box",
    "scene.shape.cylinder",
    "scene.shape.mesh",
    "scene.shape.sphere",
    "simulator.control.pause",
    "simulator.control.play",
    "simulator.control.reset",
    "simulator.control.step",
    "simulator.entity_state.set",
}
_DOMAIN_CAPABILITIES = {
    "component.lifecycle.disable",
    "component.lifecycle.enable",
    "component.lifecycle.observe",
    "component.lifecycle.prepare",
    "component.lifecycle.recover",
    "control_domain.controller.load_unload",
    "control_domain.controller.observe",
    "control_domain.controller.switch",
}
CAPABILITY_SCOPES = MappingProxyType(
    {
        **{capability: "target_binding" for capability in _TARGET_CAPABILITIES},
        **{capability: "cell_global" for capability in _CELL_CAPABILITIES},
        **{capability: "control_domain" for capability in _DOMAIN_CAPABILITIES},
    }
)

_FORBIDDEN_PORTABLE_NAMES = {
    "action",
    "action_name",
    "adapter_parameters",
    "admission_evidence",
    "alarm_condition",
    "authority",
    "base_frame",
    "binding_id",
    "component_id",
    "controllable_component_id",
    "control_binding_id",
    "control_domain_id",
    "controller",
    "controller_name",
    "credentials",
    "endpoint",
    "gripper_id",
    "group_id",
    "joint_names",
    "lease",
    "link_names",
    "measured_state",
    "model_id",
    "model_instance_id",
    "namespace",
    "operation_id",
    "operation_revision",
    "physical_ip",
    "prefix",
    "prepared_operation",
    "principal",
    "provider_parameters",
    "recovery_offer",
    "robot_id",
    "robot_ip",
    "runtime_injections",
    "runtime_unit_id",
    "secret",
    "secret_ref",
    "session",
    "stop_certificate",
    "tcp_frame",
    "topic",
    "topic_name",
    "touch_links",
    "trusted_evidence",
    "validation_evidence",
}
_FORBIDDEN_BLACKBOARD_TERMS = {
    "admission",
    "alarm",
    "authority",
    "controller_route",
    "evidence",
    "lease",
    "lifecycle",
    "operation_ref",
    "recovery_ref",
    "session",
    "stop_requested",
}

_TOP_FIELDS = {
    "api_version",
    "content_version",
    "catalog_id",
    "catalog_digest",
    "schema_types",
    "entries",
    "skill_cross_references",
}
_ENTRY_REQUIRED_FIELDS = {
    "id",
    "name",
    "category",
    "description",
    "kind",
    "children",
    "parameters",
    "required_capabilities",
    "execution",
    "runners",
}
_ENTRY_OPTIONAL_FIELDS = {
    "stability",
    "tags",
    "profiles",
    "blackboard",
    "owner_context",
    "outcomes",
    "observability",
    "assistant",
}


class CatalogValidationError(ValueError):
    """The descriptor is not the closed v2 catalog contract."""


@dataclass(frozen=True)
class CatalogParameter:
    id: str
    required: bool
    schema: CatalogSchema
    has_default: bool = False
    default: Any = None


@dataclass(frozen=True)
class CatalogCapabilityRequirement:
    capability_id: str
    scope: str
    target_parameter: str | None = None
    component_parameter: str | None = None
    when_parameter: str | None = None
    when_equals: Any = None
    effectful: bool = False


@dataclass(frozen=True)
class CatalogBlackboardBinding:
    key_parameter: str
    value_type: str
    missing_key: str | None = None


@dataclass(frozen=True)
class CatalogExecution:
    blocking: bool
    cancellable: bool
    owner_api: str


@dataclass(frozen=True)
class CatalogOwnerContext:
    kind: str
    required: bool


@dataclass(frozen=True)
class CatalogEntry:
    id: str
    name: str
    category: str
    description: str
    kind: str
    children_min: int
    children_max: int
    parameters: tuple[CatalogParameter, ...]
    required_capabilities: tuple[CatalogCapabilityRequirement, ...]
    execution: CatalogExecution
    runner_implementation_ids: Mapping[str, str]
    stability: str = "stable"
    tags: tuple[str, ...] = ()
    min_program_profile: str = "composable-cell-v1"
    blackboard_reads: tuple[CatalogBlackboardBinding, ...] = ()
    blackboard_writes: tuple[CatalogBlackboardBinding, ...] = ()
    owner_context: CatalogOwnerContext | None = None
    outcomes: Mapping[str, str] = field(default_factory=lambda: MappingProxyType({}))
    event_family: str | None = None


def _closed(
    value: Mapping[str, Any],
    *,
    required: set[str],
    allowed: set[str],
    label: str,
) -> None:
    missing = required - set(value)
    unknown = set(value) - allowed
    if missing:
        raise CatalogValidationError(f"{label} is missing required fields {sorted(missing)}")
    if unknown:
        raise CatalogValidationError(f"{label} has unknown fields {sorted(unknown)}")


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CatalogValidationError(f"{label} must be a mapping")
    return value


def _list(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise CatalogValidationError(f"{label} must be a list")
    return value


def _identifier(value: Any, label: str) -> str:
    if not isinstance(value, str) or _ID_RE.fullmatch(value) is None:
        raise CatalogValidationError(f"{label} must be a stable lowercase identifier")
    return value


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise CatalogValidationError(f"{label} must be a non-empty string")
    return value


def _boolean(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise CatalogValidationError(f"{label} must be a boolean")
    return value


def _integer(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise CatalogValidationError(f"{label} must be an integer >= {minimum}")
    return value


def _unique_identifiers(value: Any, label: str) -> tuple[str, ...]:
    result = tuple(
        _identifier(item, f"{label}[{index}]") for index, item in enumerate(_list(value, label))
    )
    if len(result) != len(set(result)):
        raise CatalogValidationError(f"{label} contains duplicate values")
    return result


def _scan_portable_value(value: Any, label: str) -> None:
    if isinstance(value, dict):
        for key, nested in value.items():
            if key in _FORBIDDEN_PORTABLE_NAMES:
                raise CatalogValidationError(f"{label} contains forbidden runtime field {key!r}")
            _scan_portable_value(nested, f"{label}.{key}")
    elif isinstance(value, list):
        for index, nested in enumerate(value):
            _scan_portable_value(nested, f"{label}[{index}]")
    elif isinstance(value, str):
        try:
            ipaddress.ip_address(value)
        except ValueError:
            return
        raise CatalogValidationError(f"{label} contains a physical IP address")


def _freeze_json_value(value: Any) -> Any:
    if isinstance(value, dict):
        return MappingProxyType({key: _freeze_json_value(nested) for key, nested in value.items()})
    if isinstance(value, list):
        return tuple(_freeze_json_value(nested) for nested in value)
    return value


def _schema_semantic_type(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
) -> str | None:
    return registry.resolve(schema).semantic_type


def _schema_contains_semantic_type(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
    semantic_type: str,
) -> bool:
    resolved = registry.resolve(schema)
    return (
        resolved.semantic_type == semantic_type
        or any(
            _schema_contains_semantic_type(nested, registry, semantic_type)
            for nested in resolved.properties.values()
        )
        or (
            resolved.additional_property_schema is not None
            and _schema_contains_semantic_type(
                resolved.additional_property_schema,
                registry,
                semantic_type,
            )
        )
        or (
            resolved.items is not None
            and _schema_contains_semantic_type(resolved.items, registry, semantic_type)
        )
    )


def _schema_semantic_scope_parameters(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
    *,
    visited: frozenset[int] = frozenset(),
) -> frozenset[str]:
    resolved = registry.resolve(schema)
    identity = id(resolved)
    if identity in visited:
        return frozenset()
    nested = (*resolved.properties.values(),)
    if resolved.items is not None:
        nested = (*nested, resolved.items)
    if resolved.additional_property_schema is not None:
        nested = (*nested, resolved.additional_property_schema)
    scopes = {
        child_scope
        for child in nested
        for child_scope in _schema_semantic_scope_parameters(
            child,
            registry,
            visited=visited | {identity},
        )
    }
    if resolved.semantic_scope_parameter is not None:
        scopes.add(resolved.semantic_scope_parameter)
    return frozenset(scopes)


def _schema_nests_key_typed_value(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
    *,
    visited: frozenset[int] = frozenset(),
) -> bool:
    resolved = registry.resolve(schema)
    identity = id(resolved)
    if identity in visited:
        return False
    nested = (*resolved.properties.values(),)
    if resolved.items is not None:
        nested = (*nested, resolved.items)
    if resolved.additional_property_schema is not None:
        nested = (*nested, resolved.additional_property_schema)
    return any(
        registry.resolve(child).value_type_from_key is not None
        or _schema_nests_key_typed_value(child, registry, visited=visited | {identity})
        for child in nested
    )


def _schema_enum_values(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
) -> tuple[str, ...]:
    resolved = registry.resolve(schema)
    return resolved.enum_values if resolved.schema_type == "enum" else ()


def _authoring_schema_names(
    registry: CatalogSchemaRegistry,
) -> frozenset[str]:
    result: set[str] = set()
    targets: set[str] = set()
    for name, schema in registry.types.items():
        if schema.resolves_to is None:
            continue
        if schema.schema_type != "object":
            raise CatalogValidationError(
                f"schema_types.{name}.resolves_to requires an object schema"
            )
        target_prefix = "#/schema_types/"
        if not schema.resolves_to.startswith(target_prefix):
            raise CatalogValidationError(
                f"schema_types.{name}.resolves_to is not a local schema reference"
            )
        target_name = schema.resolves_to[len(target_prefix) :]
        try:
            target = registry.types[target_name]
        except KeyError as exc:
            raise CatalogValidationError(
                f"schema_types.{name}.resolves_to references unknown schema " f"{target_name!r}"
            ) from exc
        if target.resolves_to is not None:
            raise CatalogValidationError(
                f"schema_types.{name}.resolves_to must target a persisted schema"
            )
        if target_name in targets:
            raise CatalogValidationError(
                f"multiple authoring schemas resolve to schema_types.{target_name}"
            )
        result.add(name)
        targets.add(target_name)
    return frozenset(result)


def _schema_references_authoring(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
    authoring_names: frozenset[str],
    *,
    stack: tuple[str, ...] = (),
) -> bool:
    if schema.ref is not None:
        name = schema.ref.removeprefix("#/schema_types/")
        if name in authoring_names:
            return True
        if name in stack:
            raise CatalogValidationError(f"schema reference cycle: {' -> '.join((*stack, name))}")
        target = registry.types.get(name)
        if target is None:
            raise CatalogValidationError(f"unknown schema reference {schema.ref!r}")
        return _schema_references_authoring(
            target,
            registry,
            authoring_names,
            stack=(*stack, name),
        )
    if any(
        _schema_references_authoring(
            child,
            registry,
            authoring_names,
            stack=stack,
        )
        for child in schema.properties.values()
    ):
        return True
    if schema.additional_property_schema is not None and _schema_references_authoring(
        schema.additional_property_schema,
        registry,
        authoring_names,
        stack=stack,
    ):
        return True
    return schema.items is not None and _schema_references_authoring(
        schema.items,
        registry,
        authoring_names,
        stack=stack,
    )


class NodeCatalog:
    """Immutable parsed form of one exact v2 descriptor."""

    def __init__(self, path: str | Path):
        self._path = Path(path)
        try:
            data = load_strict_json_yaml(self._path)
        except StrictArtifactError as exc:
            raise CatalogValidationError(str(exc)) from exc
        self._initialize(data)

    @classmethod
    def from_data(
        cls,
        data: dict[str, Any],
        *,
        source: str = "<effective-catalog>",
    ) -> "NodeCatalog":
        """Validate already parsed JSON-model content.

        This is a compiler/testing boundary, not a permissive compatibility
        loader.  It applies the same complete v2 checks as file loading.
        """

        self = cls.__new__(cls)
        self._path = Path(source)
        self._initialize(deepcopy(data))
        return self

    def _initialize(self, data: dict[str, Any]) -> None:
        _closed(data, required=_TOP_FIELDS, allowed=_TOP_FIELDS, label=str(self._path))
        if data["api_version"] != EXPECTED_API_VERSION:
            raise CatalogValidationError(
                f"{self._path}.api_version must be {EXPECTED_API_VERSION!r}"
            )
        if data["content_version"] != EXPECTED_CONTENT_VERSION:
            raise CatalogValidationError(
                f"{self._path}.content_version must be {EXPECTED_CONTENT_VERSION!r}"
            )
        self._catalog_id = _identifier(data["catalog_id"], f"{self._path}.catalog_id")
        try:
            self._catalog_digest = verify_omitted_field_digest(
                data,
                own_field="catalog_digest",
                label=str(self._path),
            )
        except StrictArtifactError as exc:
            raise CatalogValidationError(str(exc)) from exc

        try:
            self._schema_registry = parse_schema_registry(data["schema_types"])
        except CatalogSchemaError as exc:
            raise CatalogValidationError(str(exc)) from exc
        authoring_names = _authoring_schema_names(self._schema_registry)

        raw_entries = _list(data["entries"], f"{self._path}.entries")
        entries: dict[str, CatalogEntry] = {}
        for index, raw_entry in enumerate(raw_entries):
            label = f"{self._path}.entries[{index}]"
            entry = self._parse_entry(
                _mapping(raw_entry, label),
                label=label,
                authoring_names=authoring_names,
            )
            if entry.id in entries:
                raise CatalogValidationError(
                    f"{self._path}.entries contains duplicate node ID {entry.id!r}"
                )
            entries[entry.id] = entry

        cross_references = _list(
            data["skill_cross_references"],
            f"{self._path}.skill_cross_references",
        )
        if cross_references:
            raise CatalogValidationError(
                "non-empty skill_cross_references are not defined by the "
                f"frozen {EXPECTED_CONTENT_VERSION} packet"
            )
        if (
            self._catalog_id != BUILTIN_CATALOG_ID
            or self._catalog_digest != EXPECTED_BUILTIN_CATALOG_DIGEST
        ):
            raise CatalogValidationError(
                f"catalog does not match the frozen built-in v2/{EXPECTED_CONTENT_VERSION} packet"
            )

        self._entries = MappingProxyType(entries)
        self._raw_data = deepcopy(data)

    def _parse_entry(
        self,
        raw: dict[str, Any],
        *,
        label: str,
        authoring_names: frozenset[str],
    ) -> CatalogEntry:
        _closed(
            raw,
            required=_ENTRY_REQUIRED_FIELDS,
            allowed=_ENTRY_REQUIRED_FIELDS | _ENTRY_OPTIONAL_FIELDS,
            label=label,
        )
        entry_id = _identifier(raw["id"], f"{label}.id")
        name = _text(raw["name"], f"{label}.name")
        category = _identifier(raw["category"], f"{label}.category")
        if category not in _CATEGORIES:
            raise CatalogValidationError(f"{label}.category is unsupported")
        description = _text(raw["description"], f"{label}.description")
        kind = _text(raw["kind"], f"{label}.kind")
        if kind not in _ENTRY_KINDS:
            raise CatalogValidationError(f"{label}.kind is unsupported")

        children = _mapping(raw["children"], f"{label}.children")
        _closed(
            children,
            required={"min", "max"},
            allowed={"min", "max"},
            label=f"{label}.children",
        )
        children_min = _integer(children["min"], f"{label}.children.min")
        children_max = _integer(children["max"], f"{label}.children.max")
        if children_min > children_max or children_max > 4096:
            raise CatalogValidationError(f"{label}.children has invalid bounds")
        expected = {
            "leaf": (0, 0),
            "decorator": (1, 1),
        }.get(kind)
        if expected is not None and (children_min, children_max) != expected:
            raise CatalogValidationError(f"{label}.{kind} must have children min/max {expected}")
        if kind == "composite" and children_min < 1:
            raise CatalogValidationError(f"{label}.composite must require at least one child")

        parameters = self._parse_parameters(
            raw["parameters"],
            label=f"{label}.parameters",
            authoring_names=authoring_names,
        )
        parameter_by_id = {parameter.id: parameter for parameter in parameters}
        for parameter in parameters:
            for scope_parameter_id in _schema_semantic_scope_parameters(
                parameter.schema,
                self._schema_registry,
            ):
                scope_parameter = parameter_by_id.get(scope_parameter_id)
                if scope_parameter is None:
                    raise CatalogValidationError(
                        f"{label}.parameters.{parameter.id} semantic scope references "
                        f"unknown parameter {scope_parameter_id!r}"
                    )
                if (
                    _schema_semantic_type(
                        scope_parameter.schema,
                        self._schema_registry,
                    )
                    != "target_role"
                ):
                    raise CatalogValidationError(
                        f"{label}.parameters.{parameter.id} semantic scope must "
                        "reference a target_role parameter"
                    )
        if (
            any(
                _schema_contains_semantic_type(
                    parameter.schema, self._schema_registry, "source_joint"
                )
                for parameter in parameters
            )
            and sum(
                _schema_semantic_type(parameter.schema, self._schema_registry) == "target_role"
                for parameter in parameters
            )
            != 1
        ):
            raise CatalogValidationError(
                f"{label}.parameters with source_joint semantics require "
                "exactly one target_role parameter"
            )
        requirements = self._parse_capabilities(
            raw["required_capabilities"],
            parameters=parameter_by_id,
            label=f"{label}.required_capabilities",
        )
        execution = self._parse_execution(raw["execution"], f"{label}.execution")
        runners = self._parse_runners(raw["runners"], f"{label}.runners")

        stability = raw.get("stability", "stable")
        if stability not in _STABILITY_LEVELS:
            raise CatalogValidationError(f"{label}.stability is unsupported")
        tags = _unique_identifiers(raw["tags"], f"{label}.tags") if "tags" in raw else ()
        min_profile = "composable-cell-v1"
        if "profiles" in raw:
            profiles = _mapping(raw["profiles"], f"{label}.profiles")
            _closed(
                profiles,
                required={"min_program_profile"},
                allowed={"min_program_profile"},
                label=f"{label}.profiles",
            )
            min_profile = _identifier(
                profiles["min_program_profile"],
                f"{label}.profiles.min_program_profile",
            )
            if min_profile != "composable-cell-v1":
                raise CatalogValidationError(
                    f"{label}.profiles.min_program_profile must be "
                    "'composable-cell-v1' in the frozen packet"
                )

        reads: tuple[CatalogBlackboardBinding, ...] = ()
        writes: tuple[CatalogBlackboardBinding, ...] = ()
        if "blackboard" in raw:
            reads, writes = self._parse_blackboard(
                raw["blackboard"],
                parameters=parameter_by_id,
                label=f"{label}.blackboard",
            )

        self._validate_key_value_parameters(
            parameters,
            bindings=(*reads, *writes),
            label=f"{label}.parameters",
        )

        owner_context = None
        if "owner_context" in raw:
            context = _mapping(raw["owner_context"], f"{label}.owner_context")
            _closed(
                context,
                required={"kind", "required"},
                allowed={"kind", "required"},
                label=f"{label}.owner_context",
            )
            context_kind = _text(context["kind"], f"{label}.owner_context.kind")
            if context_kind not in {"current_operation", "current_recovery_offer"}:
                raise CatalogValidationError(f"{label}.owner_context.kind is unsupported")
            if (
                _boolean(
                    context["required"],
                    f"{label}.owner_context.required",
                )
                is not True
            ):
                raise CatalogValidationError(f"{label}.owner_context.required must be true")
            owner_context = CatalogOwnerContext(kind=context_kind, required=True)

        outcomes: dict[str, str] = {}
        if kind == "leaf":
            outcome_map = _mapping(raw.get("outcomes", {}), f"{label}.outcomes")
            # Outcomes describe the leaf's own work. A cancellable leaf
            # reports `interrupted` when it is stopped (Program Stop, Reset,
            # cancel, a parent ending it), never `failure`; a non-cancellable
            # leaf never reports it. `failure` is declared when the leaf's own
            # work can fail (the timer's wait cannot). Separately, any node,
            # whatever it declares, fails an attempt whose inputs cannot be
            # resolved before its work starts (spec 440 §4.6), so no
            # consumer may refuse or hide that `failure` for lack of a
            # declared `failure` outcome.
            interrupted = {"interrupted"} if execution.cancellable else set()
            if outcome_map or execution.cancellable:
                _closed(
                    outcome_map,
                    required={"success", *interrupted},
                    allowed={"success", "failure", *interrupted},
                    label=f"{label}.outcomes",
                )
                outcomes = {
                    outcome: _identifier(status, f"{label}.outcomes.{outcome}")
                    for outcome, status in outcome_map.items()
                }
        elif "outcomes" in raw:
            raise CatalogValidationError(f"{label}: non-leaf entries cannot declare outcomes")

        event_family = None
        if "observability" in raw:
            observability = _mapping(raw["observability"], f"{label}.observability")
            _closed(
                observability,
                required={"event_family"},
                allowed={"event_family"},
                label=f"{label}.observability",
            )
            event_family = _identifier(
                observability["event_family"],
                f"{label}.observability.event_family",
            )

        if "assistant" in raw:
            assistant = _mapping(raw["assistant"], f"{label}.assistant")
            if assistant:
                raise CatalogValidationError(
                    f"{label}.assistant metadata is not defined by the frozen packet"
                )

        return CatalogEntry(
            id=entry_id,
            name=name,
            category=category,
            description=description,
            kind=kind,
            children_min=children_min,
            children_max=children_max,
            parameters=parameters,
            required_capabilities=requirements,
            execution=execution,
            runner_implementation_ids=MappingProxyType(runners),
            stability=str(stability),
            tags=tags,
            min_program_profile=min_profile,
            blackboard_reads=reads,
            blackboard_writes=writes,
            owner_context=owner_context,
            outcomes=MappingProxyType(outcomes),
            event_family=event_family,
        )

    def _validate_key_value_parameters(
        self,
        parameters: tuple[CatalogParameter, ...],
        *,
        bindings: tuple[CatalogBlackboardBinding, ...],
        label: str,
    ) -> None:
        """A key-typed parameter names one key parameter whose bindings admit
        exactly the declared value types, so validators never guess a type."""

        parameter_ids = {parameter.id for parameter in parameters}
        for parameter in parameters:
            if _schema_nests_key_typed_value(parameter.schema, self._schema_registry):
                raise CatalogValidationError(
                    f"{label}.{parameter.id} nests a key-typed value; only a "
                    "whole parameter may be typed by a key"
                )
            if parameter.schema.schema_type != "key_value":
                if self._schema_registry.resolve(parameter.schema).schema_type == "key_value":
                    raise CatalogValidationError(
                        f"{label}.{parameter.id} key_value schema must be inline"
                    )
                continue
            key_parameter = parameter.schema.value_type_from_key
            if key_parameter is None:
                raise CatalogValidationError(
                    f"{label}.{parameter.id} key_value parameter needs value_type_from_key"
                )
            if key_parameter not in parameter_ids:
                raise CatalogValidationError(
                    f"{label}.{parameter.id}.value_type_from_key references "
                    f"unknown parameter {key_parameter!r}"
                )
            bound_types = {
                binding.value_type
                for binding in bindings
                if binding.key_parameter == key_parameter
            }
            if bound_types != set(parameter.schema.value_types):
                raise CatalogValidationError(
                    f"{label}.{parameter.id}.value_types must equal the "
                    f"blackboard binding types of {key_parameter!r}"
                )
            if parameter.has_default:
                raise CatalogValidationError(
                    f"{label}.{parameter.id} is typed by a key and cannot declare a default"
                )

    def _parse_parameters(
        self,
        raw_parameters: Any,
        *,
        label: str,
        authoring_names: frozenset[str],
    ) -> tuple[CatalogParameter, ...]:
        parameters: list[CatalogParameter] = []
        seen: set[str] = set()
        for index, raw_parameter in enumerate(_list(raw_parameters, label)):
            parameter_label = f"{label}[{index}]"
            parameter = _mapping(raw_parameter, parameter_label)
            _closed(
                parameter,
                required={"id", "required"},
                allowed={"id", "required", "schema", "schema_ref", "default"},
                label=parameter_label,
            )
            parameter_id = _identifier(parameter["id"], f"{parameter_label}.id")
            if parameter_id in seen:
                raise CatalogValidationError(
                    f"{label} contains duplicate parameter ID {parameter_id!r}"
                )
            if parameter_id in _FORBIDDEN_PORTABLE_NAMES:
                raise CatalogValidationError(
                    f"{parameter_label}.id is forbidden runtime identity/evidence"
                )
            seen.add(parameter_id)
            required = _boolean(parameter["required"], f"{parameter_label}.required")
            if ("schema" in parameter) == ("schema_ref" in parameter):
                raise CatalogValidationError(
                    f"{parameter_label} must declare exactly one of schema or schema_ref"
                )
            try:
                schema = parse_schema(
                    (
                        {"schema_ref": parameter["schema_ref"]}
                        if "schema_ref" in parameter
                        else parameter["schema"]
                    ),
                    path=f"{parameter_label}.schema",
                )
                self._schema_registry.resolve(schema)
            except CatalogSchemaError as exc:
                raise CatalogValidationError(str(exc)) from exc
            if _schema_references_authoring(
                schema,
                self._schema_registry,
                authoring_names,
            ):
                raise CatalogValidationError(
                    f"{parameter_label} cannot persist an authoring-only schema"
                )
            has_default = "default" in parameter
            default = parameter.get("default")
            if has_default:
                _scan_portable_value(default, f"{parameter_label}.default")
                issues = validate_value(
                    schema,
                    default,
                    registry=self._schema_registry,
                    path=f"{parameter_label}.default",
                )
                if issues:
                    raise CatalogValidationError(f"{issues[0].path}: {issues[0].message}")
            parameters.append(
                CatalogParameter(
                    id=parameter_id,
                    required=required,
                    schema=schema,
                    has_default=has_default,
                    default=_freeze_json_value(default),
                )
            )
        return tuple(parameters)

    def _parse_capabilities(
        self,
        raw_requirements: Any,
        *,
        parameters: Mapping[str, CatalogParameter],
        label: str,
    ) -> tuple[CatalogCapabilityRequirement, ...]:
        result: list[CatalogCapabilityRequirement] = []
        seen: set[tuple[Any, ...]] = set()
        for index, raw_requirement in enumerate(_list(raw_requirements, label)):
            requirement_label = f"{label}[{index}]"
            requirement = _mapping(raw_requirement, requirement_label)
            _closed(
                requirement,
                required={"capability_id", "scope"},
                allowed={
                    "capability_id",
                    "scope",
                    "target_parameter",
                    "component_parameter",
                    "when",
                    "effectful",
                },
                label=requirement_label,
            )
            capability_id = _identifier(
                requirement["capability_id"],
                f"{requirement_label}.capability_id",
            )
            expected_scope = CAPABILITY_SCOPES.get(capability_id)
            if expected_scope is None:
                raise CatalogValidationError(f"{requirement_label}.capability_id is not registered")
            scope = _identifier(requirement["scope"], f"{requirement_label}.scope")
            if scope != expected_scope:
                raise CatalogValidationError(
                    f"{requirement_label}.scope must be {expected_scope!r} "
                    f"for {capability_id!r}"
                )
            target = requirement.get("target_parameter")
            component = requirement.get("component_parameter")
            if target is not None:
                target = _identifier(target, f"{requirement_label}.target_parameter")
            if component is not None:
                component = _identifier(
                    component,
                    f"{requirement_label}.component_parameter",
                )
            if scope == "cell_global" and (target is not None or component is not None):
                raise CatalogValidationError(f"{requirement_label}: cell_global forbids selectors")
            if scope == "target_binding" and (target is None or component is not None):
                raise CatalogValidationError(
                    f"{requirement_label}: target_binding requires only " "target_parameter"
                )
            if scope == "control_domain" and ((target is None) == (component is None)):
                raise CatalogValidationError(
                    f"{requirement_label}: control_domain requires exactly one selector"
                )
            for selector, semantic_type, field_name in (
                (target, "target_role", "target_parameter"),
                (component, "component_role", "component_parameter"),
            ):
                if selector is None:
                    continue
                parameter = parameters.get(selector)
                if parameter is None:
                    raise CatalogValidationError(
                        f"{requirement_label}.{field_name} references unknown "
                        f"parameter {selector!r}"
                    )
                if _schema_semantic_type(parameter.schema, self._schema_registry) != semantic_type:
                    raise CatalogValidationError(
                        f"{requirement_label}.{field_name} must select a "
                        f"{semantic_type} parameter"
                    )

            when_parameter = None
            when_equals = None
            if "when" in requirement:
                condition = _mapping(
                    requirement["when"],
                    f"{requirement_label}.when",
                )
                _closed(
                    condition,
                    required={"parameter", "equals"},
                    allowed={"parameter", "equals"},
                    label=f"{requirement_label}.when",
                )
                when_parameter = _identifier(
                    condition["parameter"],
                    f"{requirement_label}.when.parameter",
                )
                parameter = parameters.get(when_parameter)
                if parameter is None:
                    raise CatalogValidationError(
                        f"{requirement_label}.when references unknown parameter"
                    )
                when_equals = condition["equals"]
                if when_equals not in _schema_enum_values(
                    parameter.schema,
                    self._schema_registry,
                ):
                    raise CatalogValidationError(
                        f"{requirement_label}.when must match a declared enum value"
                    )
            effectful = _boolean(
                requirement.get("effectful", False),
                f"{requirement_label}.effectful",
            )
            if effectful and scope != "target_binding":
                raise CatalogValidationError(
                    f"{requirement_label}.effectful is valid only for target_binding"
                )
            identity = (
                capability_id,
                scope,
                target,
                component,
                when_parameter,
                repr(when_equals),
            )
            if identity in seen:
                raise CatalogValidationError(f"{label} contains duplicate requirement {identity!r}")
            seen.add(identity)
            result.append(
                CatalogCapabilityRequirement(
                    capability_id=capability_id,
                    scope=scope,
                    target_parameter=target,
                    component_parameter=component,
                    when_parameter=when_parameter,
                    when_equals=_freeze_json_value(when_equals),
                    effectful=effectful,
                )
            )
        return tuple(result)

    def _parse_execution(self, raw_execution: Any, label: str) -> CatalogExecution:
        execution = _mapping(raw_execution, label)
        _closed(
            execution,
            required={"blocking", "cancellable", "owner_api"},
            allowed={"blocking", "cancellable", "owner_api"},
            label=label,
        )
        owner_api = _identifier(execution["owner_api"], f"{label}.owner_api")
        if owner_api not in _OWNER_APIS:
            raise CatalogValidationError(
                f"{label}.owner_api is not an approved semantic client API"
            )
        return CatalogExecution(
            blocking=_boolean(execution["blocking"], f"{label}.blocking"),
            cancellable=_boolean(execution["cancellable"], f"{label}.cancellable"),
            owner_api=owner_api,
        )

    @staticmethod
    def _parse_runners(raw_runners: Any, label: str) -> dict[str, str]:
        runners = _mapping(raw_runners, label)
        _closed(
            runners,
            required={"py_trees"},
            allowed={"py_trees"},
            label=label,
        )
        metadata = _mapping(runners["py_trees"], f"{label}.py_trees")
        _closed(
            metadata,
            required={"implementation_id"},
            allowed={"implementation_id"},
            label=f"{label}.py_trees",
        )
        return {
            "py_trees": _identifier(
                metadata["implementation_id"],
                f"{label}.py_trees.implementation_id",
            )
        }

    def _parse_blackboard(
        self,
        raw_blackboard: Any,
        *,
        parameters: Mapping[str, CatalogParameter],
        label: str,
    ) -> tuple[
        tuple[CatalogBlackboardBinding, ...],
        tuple[CatalogBlackboardBinding, ...],
    ]:
        blackboard = _mapping(raw_blackboard, label)
        _closed(
            blackboard,
            required={"reads", "writes"},
            allowed={"reads", "writes"},
            label=label,
        )
        result: dict[str, tuple[CatalogBlackboardBinding, ...]] = {}
        for direction in ("reads", "writes"):
            bindings: list[CatalogBlackboardBinding] = []
            seen: set[tuple[str, str]] = set()
            for index, raw_binding in enumerate(
                _list(blackboard[direction], f"{label}.{direction}")
            ):
                binding_label = f"{label}.{direction}[{index}]"
                binding = _mapping(raw_binding, binding_label)
                allowed = {"key_parameter", "value_type"}
                if direction == "reads":
                    allowed.add("missing_key")
                _closed(
                    binding,
                    required={"key_parameter", "value_type"},
                    allowed=allowed,
                    label=binding_label,
                )
                key_parameter = _identifier(
                    binding["key_parameter"],
                    f"{binding_label}.key_parameter",
                )
                parameter = parameters.get(key_parameter)
                if parameter is None:
                    raise CatalogValidationError(
                        f"{binding_label}.key_parameter references unknown parameter"
                    )
                if (
                    _schema_semantic_type(parameter.schema, self._schema_registry)
                    != "blackboard_key"
                ):
                    raise CatalogValidationError(
                        f"{binding_label}.key_parameter must select a " "blackboard_key parameter"
                    )
                value_type = _identifier(
                    binding["value_type"],
                    f"{binding_label}.value_type",
                )
                if {
                    key_parameter.lower(),
                    value_type.lower(),
                } & _FORBIDDEN_BLACKBOARD_TERMS:
                    raise CatalogValidationError(
                        f"{binding_label} declares a forbidden execution-control "
                        "blackboard protocol"
                    )
                missing_key = None
                if direction == "reads":
                    missing_key = _text(
                        binding.get("missing_key"),
                        f"{binding_label}.missing_key",
                    )
                    if missing_key not in {"failure", "wait"}:
                        raise CatalogValidationError(f"{binding_label}.missing_key is unsupported")
                identity = (key_parameter, value_type)
                if identity in seen:
                    raise CatalogValidationError(f"{label}.{direction} contains duplicate binding")
                seen.add(identity)
                bindings.append(
                    CatalogBlackboardBinding(
                        key_parameter=key_parameter,
                        value_type=value_type,
                        missing_key=missing_key,
                    )
                )
            result[direction] = tuple(bindings)
        return result["reads"], result["writes"]

    def get(self, node_id: str) -> CatalogEntry:
        return self._entries[node_id]

    @property
    def entries(self) -> tuple[CatalogEntry, ...]:
        return tuple(self._entries.values())

    @property
    def api_version(self) -> str:
        return EXPECTED_API_VERSION

    @property
    def content_version(self) -> str:
        return EXPECTED_CONTENT_VERSION

    @property
    def catalog_id(self) -> str:
        return self._catalog_id

    @property
    def catalog_digest(self) -> str:
        return self._catalog_digest

    @property
    def schema_registry(self) -> CatalogSchemaRegistry:
        return self._schema_registry

    @property
    def raw_data(self) -> dict[str, Any]:
        return deepcopy(self._raw_data)


def profile_satisfied(min_profile: str, target: str) -> bool:
    """Return whether ``target`` is in the same profile family and new enough."""

    if min_profile == target:
        return True
    minimum = _COMPOSABLE_PROFILE_RE.fullmatch(min_profile)
    selected = _COMPOSABLE_PROFILE_RE.fullmatch(target)
    if minimum is None or selected is None:
        return False
    return int(selected.group(1)) >= int(minimum.group(1))
