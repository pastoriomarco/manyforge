"""Bounded Program-run values and one-shot node-attempt input resolution."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import dataclass
from math import isfinite
from threading import RLock
from types import MappingProxyType
from typing import Any

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import (
    CatalogSchema,
    CatalogSchemaRegistry,
    validate_value,
)
from manyforge_behavior.program import (
    BlackboardFieldDeclaration,
    BlackboardKeyDeclaration,
    Program,
    ProgramParameterDeclaration,
    ProgramTreeNode,
)


class ProgramValueError(ValueError):
    """A declared Program value is missing, malformed, or inaccessible."""


_FIXED_RUNTIME_OVERRIDE_SEMANTICS = frozenset(
    {
        "blackboard_key",
        "collision_body_role",
        "component_role",
        "frame_role",
        "model_role",
        "parameter_ref",
        "resource_role",
        "target_role",
    }
)


def _schema_carries_fixed_runtime_semantics(
    schema: CatalogSchema,
    *,
    registry: CatalogSchemaRegistry,
) -> bool:
    resolved = registry.resolve(schema)
    if resolved.semantic_type in _FIXED_RUNTIME_OVERRIDE_SEMANTICS:
        return True
    if resolved.schema_type == "object":
        return any(
            _schema_carries_fixed_runtime_semantics(child, registry=registry)
            for child in resolved.properties.values()
        ) or (
            resolved.additional_property_schema is not None
            and _schema_carries_fixed_runtime_semantics(
                resolved.additional_property_schema,
                registry=registry,
            )
        )
    return bool(
        resolved.schema_type == "array"
        and resolved.items is not None
        and _schema_carries_fixed_runtime_semantics(
            resolved.items,
            registry=registry,
        )
    )


def _preserves_runtime_semantics(
    schema: CatalogSchema,
    current: Any,
    candidate: Any,
    *,
    registry: CatalogSchemaRegistry,
) -> bool:
    resolved = registry.resolve(schema)
    if resolved.semantic_type in _FIXED_RUNTIME_OVERRIDE_SEMANTICS:
        return current == candidate
    if resolved.schema_type == "object":
        if not isinstance(current, Mapping) or not isinstance(candidate, Mapping):
            return not _schema_carries_fixed_runtime_semantics(
                resolved,
                registry=registry,
            )
        for key, child in resolved.properties.items():
            if key in current or key in candidate:
                if key not in current or key not in candidate:
                    if _schema_carries_fixed_runtime_semantics(
                        child,
                        registry=registry,
                    ):
                        return False
                    continue
                if not _preserves_runtime_semantics(
                    child,
                    current[key],
                    candidate[key],
                    registry=registry,
                ):
                    return False
        extra_schema = resolved.additional_property_schema
        if extra_schema is not None:
            for key in set(current) | set(candidate):
                if key in resolved.properties:
                    continue
                if key not in current or key not in candidate:
                    if _schema_carries_fixed_runtime_semantics(
                        extra_schema,
                        registry=registry,
                    ):
                        return False
                    continue
                if not _preserves_runtime_semantics(
                    extra_schema,
                    current[key],
                    candidate[key],
                    registry=registry,
                ):
                    return False
        return True
    if resolved.schema_type == "array" and resolved.items is not None:
        if not isinstance(current, (list, tuple)) or not isinstance(
            candidate, (list, tuple)
        ):
            return not _schema_carries_fixed_runtime_semantics(
                resolved.items,
                registry=registry,
            )
        if len(current) != len(candidate) and _schema_carries_fixed_runtime_semantics(
            resolved.items,
            registry=registry,
        ):
            return False
        shared = min(len(current), len(candidate))
        return all(
            _preserves_runtime_semantics(
                resolved.items,
                current[index],
                candidate[index],
                registry=registry,
            )
            for index in range(shared)
        )
    return True


class ProgramRuntimeOverrideStore:
    """Schema-checked param patches consumed only by later node attempts.

    The Program Runner owns lifecycle and audit around these values. This
    store owns only validation and the atomic active-value read used by the
    input resolver; it never mutates Program/Binding or an accepted request.
    """

    def __init__(self, *, program: Program, catalog: NodeCatalog) -> None:
        if not isinstance(program, Program):
            raise TypeError("runtime overrides require one validated Program")
        if not isinstance(catalog, NodeCatalog):
            raise TypeError("runtime overrides require one validated catalog")
        self._catalog = catalog
        self._nodes: dict[str, ProgramTreeNode] = {}

        def visit(node: ProgramTreeNode) -> None:
            self._nodes[node.instance_id] = node
            for child in node.children:
                visit(child)

        visit(program.tree)
        self._active: dict[tuple[str, str], Any] = {}
        self._lock = RLock()

    def validate(self, node_instance_id: str, parameter_id: str, value: Any) -> Any:
        try:
            node = self._nodes[node_instance_id]
        except KeyError as exc:
            raise ProgramValueError(
                f"node instance {node_instance_id!r} is not in the captured Program"
            ) from exc
        entry = self._catalog.get(node.node_id)
        declaration = next(
            (item for item in entry.parameters if item.id == parameter_id),
            None,
        )
        if declaration is None:
            raise ProgramValueError(
                f"node {node.node_id!r} has no parameter {parameter_id!r}"
            )
        binding_parameters = {
            parameter
            for requirement in entry.required_capabilities
            for parameter in (
                requirement.target_parameter,
                requirement.component_parameter,
                requirement.when_parameter,
            )
            if parameter is not None
        }
        if parameter_id in binding_parameters:
            raise ProgramValueError(
                f"node parameter {parameter_id!r} selects captured binding meaning"
            )
        copied = _copy_runtime_value(value)
        issues = validate_value(
            declaration.schema,
            copied,
            registry=self._catalog.schema_registry,
            path=f"node[{node.node_id}].runtime_override.{parameter_id}",
        )
        if issues:
            first = issues[0]
            raise ProgramValueError(f"{first.path}: {first.message}")
        current = node.parameters.get(
            parameter_id,
            declaration.default if declaration.has_default else None,
        )
        if not _preserves_runtime_semantics(
            declaration.schema,
            current,
            copied,
            registry=self._catalog.schema_registry,
        ):
            raise ProgramValueError(
                f"node parameter {parameter_id!r} changes captured semantic selection"
            )
        return copied

    def node_id(self, node_instance_id: str) -> str:
        """Return the immutable catalog node kind for one captured instance."""

        try:
            return self._nodes[node_instance_id].node_id
        except KeyError as exc:
            raise ProgramValueError(
                f"node instance {node_instance_id!r} is not in the captured Program"
            ) from exc

    def node_context(self, node_instance_id: str) -> dict[str, Any]:
        """Return only the captured facts needed to derive an owner snapshot."""

        try:
            node = self._nodes[node_instance_id]
        except KeyError as exc:
            raise ProgramValueError(
                f"node instance {node_instance_id!r} is not in the captured Program"
            ) from exc
        return {
            "node_id": node.node_id,
            "parameters": _copy_runtime_value(node.parameters),
        }

    def activate(self, node_instance_id: str, parameter_id: str, value: Any) -> None:
        validated = self.validate(node_instance_id, parameter_id, value)
        with self._lock:
            self._active[(node_instance_id, parameter_id)] = validated

    def clear(self, node_instance_id: str, parameter_id: str) -> None:
        with self._lock:
            self._active.pop((node_instance_id, parameter_id), None)

    def for_attempt(self, node_instance_id: str) -> Mapping[str, Any]:
        with self._lock:
            return MappingProxyType(
                {
                    parameter_id: _copy_runtime_value(value)
                    for (instance_id, parameter_id), value in self._active.items()
                    if instance_id == node_instance_id
                }
            )


def _copy_runtime_value(value: Any) -> Any:
    """Thaw immutable catalog/Program JSON without leaking shared containers."""

    if isinstance(value, Mapping):
        return {key: _copy_runtime_value(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_copy_runtime_value(nested) for nested in value]
    return deepcopy(value)


@dataclass(frozen=True, slots=True)
class NodeAttemptInputSnapshot:
    """One atomic node-attempt view of authored and Program-run values."""

    parameters: Mapping[str, Any]
    blackboard: Mapping[str, Any]
    embedded_values: Mapping[str, Any]


@dataclass(frozen=True, slots=True)
class ProgramValueRecord:
    """One atomic logical payload plus owner-issued diagnostic metadata."""

    status: str
    value: Any
    metadata: Mapping[str, Any]

    def __post_init__(self) -> None:
        if not isinstance(self.status, str) or not self.status:
            raise ProgramValueError("Program value status must be non-empty text")
        if not isinstance(self.metadata, Mapping):
            raise TypeError("Program value metadata must be a mapping")
        if any(not isinstance(key, str) or not key for key in self.metadata):
            raise ProgramValueError(
                "Program value metadata keys must be non-empty text"
            )
        if "status" in self.metadata:
            raise ProgramValueError(
                "Program value metadata cannot replace the record status"
            )
        object.__setattr__(self, "value", _copy_runtime_value(self.value))
        object.__setattr__(
            self,
            "metadata",
            MappingProxyType(_copy_runtime_value(self.metadata)),
        )

    def to_bundle(self) -> dict[str, Any]:
        return {
            "meta": {"status": self.status, **_copy_runtime_value(self.metadata)},
            "value": _copy_runtime_value(self.value),
        }


def _matches_scalar_type(value: object, declared_type: str) -> bool:
    if declared_type == "bool":
        return type(value) is bool
    if declared_type == "float64":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return False
        try:
            return isfinite(float(value))
        except OverflowError:
            return False
    if declared_type == "int64":
        return type(value) is int and -(2**53 - 1) <= value <= 2**53 - 1
    if declared_type in {"scene_resource", "string"}:
        return isinstance(value, str) and bool(value)
    if declared_type == "string_list":
        return isinstance(value, list) and all(isinstance(item, str) for item in value)
    return isinstance(value, Mapping)


def _validate_fields(
    value: object,
    fields: Mapping[str, BlackboardFieldDeclaration],
    *,
    label: str,
    parent_type: str,
) -> None:
    if not fields:
        return
    if not isinstance(value, Mapping) or set(value) != set(fields):
        raise ProgramValueError(
            f"{label} must contain exactly declared fields {sorted(fields)}"
        )
    for field_name, declaration in fields.items():
        field_value = value[field_name]
        empty_selection_identity = (
            parent_type in {"model_response", "scene_selection_result"}
            and field_name == "selected_object_id"
            and declaration.type == "scene_resource"
            and field_value == ""
            and value.get("outcome") in {"no_match", "ambiguous"}
        )
        empty_model_reason = (
            parent_type in {"model_response", "scene_selection_result"}
            and field_name == "reason"
            and declaration.type == "string"
            and field_value == ""
        )
        if (
            not empty_selection_identity
            and not empty_model_reason
            and not _matches_scalar_type(field_value, declaration.type)
        ):
            raise ProgramValueError(
                f"{label}.{field_name} does not match {declaration.type}"
            )


def _validate_blackboard_value(
    value: object,
    declaration: BlackboardKeyDeclaration,
    *,
    label: str,
) -> None:
    if not _matches_scalar_type(value, declaration.type):
        raise ProgramValueError(f"{label} does not match {declaration.type}")
    _validate_fields(
        value,
        declaration.fields,
        label=label,
        parent_type=declaration.type,
    )


def _validate_program_parameter(
    value: object,
    declaration: ProgramParameterDeclaration,
    *,
    label: str,
) -> None:
    if not _matches_scalar_type(value, declaration.type):
        raise ProgramValueError(f"{label} does not match {declaration.type}")
    constraints = declaration.constraints
    if declaration.type in {"float64", "int64"}:
        numeric = float(value)
        for name, predicate in (
            ("min_inclusive", lambda bound: numeric >= float(bound)),
            ("min_exclusive", lambda bound: numeric > float(bound)),
            ("max_inclusive", lambda bound: numeric <= float(bound)),
            ("max_exclusive", lambda bound: numeric < float(bound)),
        ):
            if name in constraints and not predicate(constraints[name]):
                raise ProgramValueError(f"{label} violates {name}")
    if declaration.type in {"string", "string_list"}:
        length = len(value)  # type: ignore[arg-type]
        if "min_length" in constraints and length < constraints["min_length"]:
            raise ProgramValueError(f"{label} violates min_length")
        if "max_length" in constraints and length > constraints["max_length"]:
            raise ProgramValueError(f"{label} violates max_length")


class ProgramValueStore:
    """Thread-safe logical-key store; engine storage paths never become meaning.

    Typed Program reads expose only the declared payload.  The complete atomic
    record remains available for audit/diagnostics so owner metadata does not
    become an author-controlled field or interfere with declared dataflow.
    """

    def __init__(self, declarations: Mapping[str, BlackboardKeyDeclaration]) -> None:
        self._declarations = MappingProxyType(dict(declarations))
        self._values: dict[str, ProgramValueRecord] = {}
        self._lock = RLock()

    @classmethod
    def from_program(cls, program: Program) -> "ProgramValueStore":
        if not isinstance(program, Program):
            raise TypeError("value store requires one validated Program")
        return cls(program.blackboard_keys)

    def read(self, key: str) -> Any:
        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.access not in {"read", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not readable")
        with self._lock:
            if key not in self._values:
                raise KeyError(key)
            return _copy_runtime_value(self._values[key].value)

    def write(
        self,
        key: str,
        value: Any,
        *,
        status: str = "succeeded",
        metadata: Mapping[str, Any] | None = None,
    ) -> None:
        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.access not in {"write", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not writable")
        _validate_blackboard_value(
            value,
            declaration,
            label=f"blackboard key {key!r}",
        )
        record = ProgramValueRecord(
            status=status,
            value=value,
            metadata=metadata or {},
        )
        with self._lock:
            self._values[key] = record

    def read_bundle(self, key: str) -> dict[str, Any]:
        """Return the atomic diagnostic record without changing logical reads."""

        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.access not in {"read", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not readable")
        with self._lock:
            if key not in self._values:
                raise KeyError(key)
            return self._values[key].to_bundle()

    def contains(self, key: str) -> bool:
        with self._lock:
            return key in self._values

    def snapshot(self, keys: tuple[str, ...]) -> Mapping[str, Any]:
        if keys != tuple(sorted(set(keys))):
            raise ProgramValueError(
                "blackboard snapshot keys must be unique and canonical"
            )
        declarations: dict[str, BlackboardKeyDeclaration] = {}
        for key in keys:
            try:
                declaration = self._declarations[key]
            except KeyError as exc:
                raise ProgramValueError(
                    f"blackboard key {key!r} is undeclared"
                ) from exc
            if declaration.access not in {"read", "read_write"}:
                raise ProgramValueError(f"blackboard key {key!r} is not readable")
            declarations[key] = declaration
        with self._lock:
            missing = [key for key in keys if key not in self._values]
            if missing:
                raise KeyError(missing[0])
            return MappingProxyType(
                {
                    key: _copy_runtime_value(self._values[key].value)
                    for key in declarations
                }
            )


class NodeAttemptInputResolver:
    """Resolve all declared dynamic inputs exactly once for one node attempt."""

    def __init__(
        self,
        *,
        program: Program,
        catalog: NodeCatalog,
        value_store: ProgramValueStore,
        program_parameter_values: Mapping[str, Any] | None = None,
        runtime_overrides: ProgramRuntimeOverrideStore | None = None,
    ) -> None:
        if not isinstance(program, Program):
            raise TypeError("input resolver requires one validated Program")
        if not isinstance(catalog, NodeCatalog):
            raise TypeError("input resolver requires one validated NodeCatalog")
        if not isinstance(value_store, ProgramValueStore):
            raise TypeError("input resolver requires one ProgramValueStore")
        if runtime_overrides is not None and not isinstance(
            runtime_overrides, ProgramRuntimeOverrideStore
        ):
            raise TypeError(
                "runtime_overrides must be one ProgramRuntimeOverrideStore"
            )
        values = {
            parameter_id: _copy_runtime_value(declaration.default)
            for parameter_id, declaration in program.parameters.items()
        }
        for parameter_id, value in dict(program_parameter_values or {}).items():
            try:
                declaration = program.parameters[parameter_id]
            except KeyError as exc:
                raise ProgramValueError(
                    f"Program parameter {parameter_id!r} is undeclared"
                ) from exc
            _validate_program_parameter(
                value,
                declaration,
                label=f"Program parameter {parameter_id!r}",
            )
            values[parameter_id] = _copy_runtime_value(value)
        self._program = program
        self._catalog = catalog
        self._value_store = value_store
        self._program_parameters = MappingProxyType(values)
        self._runtime_overrides = runtime_overrides

    @property
    def value_store(self) -> ProgramValueStore:
        return self._value_store

    def snapshot(
        self,
        *,
        node_id: str,
        node_instance_id: str,
        parameters: Mapping[str, Any],
        value_sources: Mapping[str, Mapping[str, Any]],
        blackboard_read_parameters: tuple[str, ...] = (),
    ) -> NodeAttemptInputSnapshot:
        entry = self._catalog.get(node_id)
        by_id = {parameter.id: parameter for parameter in entry.parameters}
        resolved: dict[str, Any] = {
            parameter.id: _copy_runtime_value(parameter.default)
            for parameter in entry.parameters
            if parameter.has_default
        }
        resolved.update(_copy_runtime_value(parameters))

        def source_reference(source: Mapping[str, Any]) -> tuple[str, str]:
            kind = source.get("kind")
            reference = source.get("reference")
            if kind not in {
                "program_parameter",
                "blackboard_key",
                "blackboard_field",
            }:
                raise ProgramValueError("value source kind is unsupported")
            if not isinstance(reference, str) or not reference:
                raise ProgramValueError("value source reference is invalid")
            return kind, reference

        embedded_sources: dict[str, Mapping[str, Any]] = {}

        def collect_embedded(
            schema: Any,
            value: Any,
            *,
            path: str,
        ) -> None:
            if schema.ref == "#/schema_types/value_source":
                if not isinstance(value, Mapping):
                    raise ProgramValueError(
                        f"embedded value source {path!r} is not a mapping"
                    )
                embedded_sources[path] = value
                return
            concrete = self._catalog.schema_registry.resolve(schema)
            if concrete.schema_type == "object" and isinstance(value, Mapping):
                for name, nested in value.items():
                    child = concrete.properties.get(name)
                    if child is None:
                        child = concrete.additional_property_schema
                    if child is not None:
                        collect_embedded(child, nested, path=f"{path}/{name}")
            elif (
                concrete.schema_type == "array"
                and isinstance(value, (list, tuple))
                and concrete.items is not None
            ):
                for index, nested in enumerate(value):
                    collect_embedded(
                        concrete.items,
                        nested,
                        path=f"{path}/{index}",
                    )

        for parameter_id, value in resolved.items():
            if parameter_id in by_id:
                collect_embedded(
                    by_id[parameter_id].schema,
                    value,
                    path=f"/{parameter_id}",
                )

        blackboard_keys: set[str] = set()
        for source in (*value_sources.values(), *embedded_sources.values()):
            kind, reference = source_reference(source)
            if kind != "program_parameter":
                blackboard_keys.add(reference)
        for parameter_id in blackboard_read_parameters:
            try:
                key = resolved[parameter_id]
            except KeyError as exc:
                raise ProgramValueError(
                    f"blackboard key parameter {parameter_id!r} is absent"
                ) from exc
            if not isinstance(key, str) or not key:
                raise ProgramValueError(
                    f"blackboard key parameter {parameter_id!r} is invalid"
                )
            blackboard_keys.add(key)
        blackboard_snapshot = self._value_store.snapshot(tuple(sorted(blackboard_keys)))

        def resolve_source(source: Mapping[str, Any]) -> Any:
            kind, reference = source_reference(source)
            if kind == "program_parameter":
                try:
                    return _copy_runtime_value(self._program_parameters[reference])
                except KeyError as exc:
                    raise ProgramValueError(
                        f"Program parameter {reference!r} has no run value"
                    ) from exc
            value = blackboard_snapshot[reference]
            if kind == "blackboard_field":
                field = source.get("field")
                if (
                    not isinstance(field, str)
                    or not field
                    or not isinstance(value, Mapping)
                    or field not in value
                ):
                    raise ProgramValueError(
                        f"blackboard field {reference!r}.{field} is unavailable"
                    )
                value = value[field]
            return _copy_runtime_value(value)

        for parameter_id, source in value_sources.items():
            resolved[parameter_id] = resolve_source(source)

        # Patches intentionally win over authored/value-source values, but
        # only for this new one-shot attempt snapshot.
        if self._runtime_overrides is not None:
            resolved.update(
                _copy_runtime_value(
                    self._runtime_overrides.for_attempt(node_instance_id)
                )
            )

        unknown = set(resolved) - set(by_id)
        if unknown:
            raise ProgramValueError(
                f"node {node_id!r} has unknown resolved parameters {sorted(unknown)}"
            )
        missing = {
            parameter.id
            for parameter in entry.parameters
            if parameter.required and parameter.id not in resolved
        }
        if missing:
            raise ProgramValueError(
                f"node {node_id!r} is missing parameters {sorted(missing)}"
            )
        for parameter_id, value in resolved.items():
            issues = validate_value(
                by_id[parameter_id].schema,
                value,
                registry=self._catalog.schema_registry,
                path=f"node[{node_id}].parameters.{parameter_id}",
            )
            if issues:
                first = issues[0]
                raise ProgramValueError(f"{first.path}: {first.message}")
        declared_blackboard = {
            parameter_id: _copy_runtime_value(
                blackboard_snapshot[resolved[parameter_id]]
            )
            for parameter_id in blackboard_read_parameters
        }
        return NodeAttemptInputSnapshot(
            parameters=MappingProxyType(_copy_runtime_value(resolved)),
            blackboard=MappingProxyType(declared_blackboard),
            embedded_values=MappingProxyType(
                {
                    path: resolve_source(source)
                    for path, source in embedded_sources.items()
                }
            ),
        )


__all__ = [
    "NodeAttemptInputResolver",
    "NodeAttemptInputSnapshot",
    "ProgramValueError",
    "ProgramValueRecord",
    "ProgramValueStore",
    "ProgramRuntimeOverrideStore",
]
