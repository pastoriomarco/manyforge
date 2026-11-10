"""Schema model for ManyForge node-catalog parameters.

The node catalog remains the authoring source of truth.  This module defines the
small ManyForge-owned schema subset that can be validated in Python and
projected to JSON Schema for model/tool surfaces.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass, field, replace
from types import MappingProxyType
from typing import Any, Mapping

from manyforge_behavior import interrupt_condition


_SCALAR_TYPES = {"string", "float", "int", "bool"}
_COMPOSITE_TYPES = {"object", "array", "enum"}
# A ``key_value`` holds a value of one Program value type from the closed
# ``value_types``. With ``value_type_from_key`` its type is exactly the declared
# type of the blackboard key named by that parameter; without it the value (or
# its value source) carries one of the admitted types.
_KEY_VALUE_TYPE = "key_value"
# An ``interrupt_condition`` holds one closed per-node interrupt condition
# (interrupt_condition.py); it takes no other schema keys.
_CONDITION_TYPE = "interrupt_condition"
_DIRECT_CONDITION_TYPE = "component_interrupt_condition"
_ALLOWED_SCHEMA_TYPES = (
    _SCALAR_TYPES
    | _COMPOSITE_TYPES
    | {_KEY_VALUE_TYPE, _CONDITION_TYPE, _DIRECT_CONDITION_TYPE}
)
KEY_VALUE_TYPES = ("bool", "float64", "int64", "stored_pose", "string", "string_list")
_KEY_VALUE_ONLY_KEYS = {"value_type_from_key", "value_types"}
_ALLOWED_INLINE_SCHEMA_KEYS = {
    "type",
    "description",
    "properties",
    "required",
    "additional_properties",
    "items",
    "length",
    "min_items",
    "max_items",
    "values",
    "enum_values",
    "constraints",
    "exactly_one",
    "mutually_exclusive",
    "required_when",
    "allowed_when",
    "resolves_to",
    "requires_current_value",
    "semantic_type",
    "semantic_scope_parameter",
    "nullable",
    "value_type_from_key",
    "value_types",
}
_NUMERIC_CONSTRAINT_KEYS = {
    "min_inclusive",
    "max_inclusive",
    "min_exclusive",
    "max_exclusive",
    "not_equal",
}
_SEMANTIC_TYPES = {
    "blackboard_key",
    "collision_body_role",
    "component_role",
    "frame_role",
    "model_role",
    "parameter_ref",
    "resource_role",
    "scene_resource",
    "source_joint",
    "target_role",
    "target_reference",
    "signal_reference",
    "frame_reference",
    "collision_body_reference",
}
_REFERENCE_SEMANTIC_FIELDS = {
    "target_reference": frozenset({"component_id", "command_target_id"}),
    "signal_reference": frozenset({"component_id", "signal_name"}),
    "frame_reference": frozenset({"component_id", "frame_id"}),
    "collision_body_reference": frozenset({"component_id", "collision_body_id"}),
}
_SCOPED_SEMANTIC_TYPES = {
    "collision_body_role",
    "frame_role",
    "model_role",
    "resource_role",
}
_PORTABLE_ID_RE = re.compile(r"^[a-z][a-z0-9_.-]*$")
_CATALOG_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def _strip_schema_descriptions(value: Any, *, in_properties_map: bool = False) -> Any:
    """Deep-copy ``value`` with schema prose annotations removed.

    JSON Schema uses ``description`` as an annotation keyword, but
    ``properties.description`` is a real accepted field named "description".
    Structural hashes must ignore the annotation while preserving property
    names and their schemas.
    """
    if isinstance(value, dict):
        if in_properties_map:
            return {
                property_name: _strip_schema_descriptions(property_schema)
                for property_name, property_schema in value.items()
            }
        result: dict[str, Any] = {}
        for key, item in value.items():
            if key == "description":
                continue
            result[key] = _strip_schema_descriptions(
                item,
                in_properties_map=key in {"properties", "$defs", "definitions"},
            )
        return result
    if isinstance(value, list):
        return [_strip_schema_descriptions(v) for v in value]
    return value


class CatalogSchemaError(Exception):
    """Raised when a catalog schema declaration is invalid."""


def _freeze_json_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType(
            {key: _freeze_json_value(nested) for key, nested in value.items()}
        )
    if isinstance(value, list):
        return tuple(_freeze_json_value(nested) for nested in value)
    return value


@dataclass(frozen=True)
class CatalogValueIssue:
    path: str
    message: str
    error_code: str = "catalog_schema_invalid_value"
    expected_schema: str | None = None
    allowed_values: tuple[str, ...] = ()
    valid_fields: tuple[str, ...] = ()


@dataclass(frozen=True)
class CatalogSchema:
    """Parsed ManyForge schema subset.

    A schema is either a reference (``ref`` set) or an inline typed schema
    (``schema_type`` set).  References use ``#/schema_types/<name>``.
    """

    schema_type: str | None = None
    ref: str | None = None
    description: str = ""
    properties: Mapping[str, "CatalogSchema"] = field(default_factory=dict)
    required: tuple[str, ...] = ()
    additional_properties: bool = False
    additional_property_schema: "CatalogSchema | None" = None
    items: "CatalogSchema | None" = None
    length: int | None = None
    min_items: int | None = None
    max_items: int | None = None
    enum_values: tuple[str, ...] = ()
    constraints: Mapping[str, Any] = field(default_factory=dict)
    exactly_one: tuple[tuple[str, ...], ...] = ()
    mutually_exclusive: tuple[tuple[str, ...], ...] = ()
    required_when: Mapping[str, Any] = field(default_factory=dict)
    allowed_when: Mapping[str, Any] = field(default_factory=dict)
    resolves_to: str | None = None
    requires_current_value: bool = False
    semantic_type: str | None = None
    semantic_scope_parameter: str | None = None
    nullable: bool = False
    value_type_from_key: str | None = None
    value_types: tuple[str, ...] = ()
    # The value the consuming code uses when this field is omitted. It is
    # catalog documentation (and checked against the schema); it is not
    # emitted into the generated JSON Schema.
    has_default: bool = False
    default: Any = None

    def is_ref(self) -> bool:
        return self.ref is not None


@dataclass(frozen=True)
class CatalogSchemaRegistry:
    types: Mapping[str, CatalogSchema] = field(default_factory=dict)

    def resolve(
        self, schema: CatalogSchema, *, stack: tuple[str, ...] = ()
    ) -> CatalogSchema:
        """Resolve a schema reference to its concrete definition."""
        if not schema.is_ref():
            return schema
        name = _schema_type_name_from_ref(schema.ref or "")
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise CatalogSchemaError(f"Schema reference cycle: {cycle}")
        try:
            target = self.types[name]
        except KeyError as exc:
            raise CatalogSchemaError(
                f"Unknown schema reference '{schema.ref}'"
            ) from exc
        return self.resolve(target, stack=(*stack, name))

    def to_json_schema(self, schema: CatalogSchema) -> dict[str, Any]:
        return _to_json_schema(self.resolve(schema), self)

    def schema_hash(self, schema: CatalogSchema) -> str:
        # Hash the STRUCTURE only — prose descriptions are excluded so a
        # docs-only edit does not perturb the hash (a prose
        # improvement must not invalidate a runtime callback that gates on it).
        payload = json.dumps(
            _strip_schema_descriptions(self.to_json_schema(schema)),
            sort_keys=True,
            separators=(",", ":"),
        )
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def parse_schema_registry(raw_types: Any) -> CatalogSchemaRegistry:
    if raw_types in (None, {}):
        return CatalogSchemaRegistry(types=MappingProxyType({}))
    if not isinstance(raw_types, Mapping):
        raise CatalogSchemaError("schema_types must be a mapping")
    parsed: dict[str, CatalogSchema] = {}
    for name, raw_schema in raw_types.items():
        if not isinstance(name, str) or not name.strip():
            raise CatalogSchemaError("schema_types keys must be non-empty strings")
        if name in parsed:
            raise CatalogSchemaError(f"Duplicate schema type '{name}'")
        parsed[name] = parse_schema(raw_schema, path=f"schema_types.{name}")
    registry = CatalogSchemaRegistry(types=MappingProxyType(parsed))
    for name, schema in parsed.items():
        _validate_schema(schema, registry, path=f"schema_types.{name}", stack=(name,))
    return registry


def parse_schema(raw_schema: Any, *, path: str) -> CatalogSchema:
    if not isinstance(raw_schema, Mapping):
        raise CatalogSchemaError(f"{path} must be a mapping")

    if "$ref" in raw_schema:
        raise CatalogSchemaError(f"{path}: $ref is forbidden; use schema_ref")
    ref = raw_schema.get("schema_ref")
    if ref is not None:
        if set(raw_schema.keys()) - {
            "schema_ref",
            "required_when",
            "allowed_when",
            "description",
        }:
            raise CatalogSchemaError(
                f"{path}: schema_ref form permits only conditional annotations "
                "and a description"
            )
        if not isinstance(ref, str) or not ref.strip():
            raise CatalogSchemaError(
                f"{path}: schema reference must be a non-empty string"
            )
        required_when = raw_schema.get("required_when", {})
        allowed_when = raw_schema.get("allowed_when", {})
        if not isinstance(required_when, Mapping):
            raise CatalogSchemaError(f"{path}.required_when must be a mapping")
        if not isinstance(allowed_when, Mapping):
            raise CatalogSchemaError(f"{path}.allowed_when must be a mapping")
        return CatalogSchema(
            ref=ref,
            description=_optional_string(raw_schema, "description", path),
            required_when=_freeze_json_value(required_when),
            allowed_when=_freeze_json_value(allowed_when),
        )

    if "default" in raw_schema:
        parsed = parse_schema(
            {key: value for key, value in raw_schema.items() if key != "default"},
            path=path,
        )
        return replace(parsed, has_default=True, default=raw_schema["default"])

    unknown_keys = sorted(set(raw_schema) - _ALLOWED_INLINE_SCHEMA_KEYS)
    if unknown_keys:
        raise CatalogSchemaError(f"{path}: unsupported schema keys {unknown_keys}")

    schema_type = raw_schema.get("type")
    if schema_type not in _ALLOWED_SCHEMA_TYPES:
        allowed = ", ".join(sorted(_ALLOWED_SCHEMA_TYPES))
        raise CatalogSchemaError(f"{path}: type must be one of: {allowed}")
    if schema_type == _KEY_VALUE_TYPE:
        return _parse_key_value_schema(raw_schema, path=path)
    if schema_type in {_CONDITION_TYPE, _DIRECT_CONDITION_TYPE}:
        unknown = sorted(set(raw_schema) - {"type", "description"})
        if unknown:
            raise CatalogSchemaError(f"{path}: {schema_type} forbids schema keys {unknown}")
        return CatalogSchema(
            schema_type=schema_type,
            description=_optional_string(raw_schema, "description", path),
        )
    present_key_value_keys = sorted(_KEY_VALUE_ONLY_KEYS & set(raw_schema))
    if present_key_value_keys:
        raise CatalogSchemaError(
            f"{path}: {present_key_value_keys} require type={_KEY_VALUE_TYPE}"
        )

    constraints = raw_schema.get("constraints", {})
    if constraints is None:
        constraints = {}
    if not isinstance(constraints, Mapping):
        raise CatalogSchemaError(f"{path}.constraints must be a mapping")
    unknown_constraints = sorted(set(constraints) - _NUMERIC_CONSTRAINT_KEYS)
    if unknown_constraints:
        raise CatalogSchemaError(
            f"{path}.constraints contains unsupported keys {unknown_constraints}"
        )
    if "not_equal" in constraints and (
        schema_type not in {"int", "float"}
        or isinstance(constraints["not_equal"], bool)
        or not isinstance(constraints["not_equal"], (int, float))
        or (
            isinstance(constraints["not_equal"], float)
            and not math.isfinite(constraints["not_equal"])
        )
    ):
        raise CatalogSchemaError(
            f"{path}.constraints.not_equal requires one numeric scalar schema"
        )

    properties: dict[str, CatalogSchema] = {}
    raw_properties = raw_schema.get("properties", {})
    if raw_properties is None:
        raw_properties = {}
    if raw_properties:
        if schema_type != "object":
            raise CatalogSchemaError(f"{path}.properties requires type=object")
        if not isinstance(raw_properties, Mapping):
            raise CatalogSchemaError(f"{path}.properties must be a mapping")
        for prop_name, raw_property in raw_properties.items():
            if not isinstance(prop_name, str) or not prop_name.strip():
                raise CatalogSchemaError(
                    f"{path}.properties keys must be non-empty strings"
                )
            properties[prop_name] = parse_schema(
                raw_property,
                path=f"{path}.properties.{prop_name}",
            )

    required = _string_tuple(raw_schema.get("required", ()), path=f"{path}.required")
    exactly_one = _field_group_tuple(
        raw_schema.get("exactly_one", ()),
        path=f"{path}.exactly_one",
    )
    mutually_exclusive = _field_group_tuple(
        raw_schema.get("mutually_exclusive", ()),
        path=f"{path}.mutually_exclusive",
    )

    raw_items = raw_schema.get("items")
    items = None
    if raw_items is not None:
        if schema_type != "array":
            raise CatalogSchemaError(f"{path}.items requires type=array")
        if isinstance(raw_items, str):
            items = CatalogSchema(schema_type=raw_items)
        else:
            items = parse_schema(raw_items, path=f"{path}.items")

    enum_values = ()
    if schema_type == "enum":
        if "enum_values" in raw_schema:
            raise CatalogSchemaError(f"{path}: enum_values is forbidden; use values")
        values = raw_schema.get("values")
        enum_values = _string_tuple(values, path=f"{path}.values")
        if not enum_values:
            raise CatalogSchemaError(f"{path}: enum requires non-empty values")
        if len(enum_values) != len(set(enum_values)):
            raise CatalogSchemaError(f"{path}: enum values must be unique")

    required_when = raw_schema.get("required_when", {})
    if required_when is None:
        required_when = {}
    if not isinstance(required_when, Mapping):
        raise CatalogSchemaError(f"{path}.required_when must be a mapping")

    allowed_when = raw_schema.get("allowed_when", {})
    if allowed_when is None:
        allowed_when = {}
    if not isinstance(allowed_when, Mapping):
        raise CatalogSchemaError(f"{path}.allowed_when must be a mapping")

    length = _optional_non_negative_int(raw_schema, "length", path)
    min_items = _optional_non_negative_int(raw_schema, "min_items", path)
    max_items = _optional_non_negative_int(raw_schema, "max_items", path)
    if length is not None and (min_items is not None or max_items is not None):
        raise CatalogSchemaError(
            f"{path}: length cannot be combined with min_items/max_items"
        )
    if min_items is not None and max_items is not None and min_items > max_items:
        raise CatalogSchemaError(f"{path}: min_items cannot exceed max_items")

    raw_additional_properties = raw_schema.get("additional_properties", False)
    additional_property_schema = None
    if raw_additional_properties is True:
        raise CatalogSchemaError(
            f"{path}.additional_properties=true is forbidden; declare one schema"
        )
    if raw_additional_properties is False:
        additional_properties = False
    elif isinstance(raw_additional_properties, Mapping):
        if schema_type != "object":
            raise CatalogSchemaError(
                f"{path}.additional_properties schema requires type=object"
            )
        additional_property_schema = parse_schema(
            raw_additional_properties,
            path=f"{path}.additional_properties",
        )
        additional_properties = True
    else:
        raise CatalogSchemaError(
            f"{path}.additional_properties must be false or a schema"
        )

    resolves_to = raw_schema.get("resolves_to")
    if resolves_to is not None and (
        not isinstance(resolves_to, str) or not resolves_to.strip()
    ):
        raise CatalogSchemaError(f"{path}.resolves_to must be a non-empty string")

    requires_current_value = raw_schema.get("requires_current_value", False)
    if not isinstance(requires_current_value, bool):
        raise CatalogSchemaError(f"{path}.requires_current_value must be a boolean")

    semantic_type = raw_schema.get("semantic_type")
    if semantic_type is not None:
        if semantic_type not in _SEMANTIC_TYPES:
            raise CatalogSchemaError(
                f"{path}.semantic_type has unsupported value {semantic_type!r}"
            )
        if semantic_type in _REFERENCE_SEMANTIC_FIELDS:
            expected = _REFERENCE_SEMANTIC_FIELDS[semantic_type]
            if (
                schema_type != "object"
                or set(properties) != expected
                or set(required) != expected
                or len(required) != len(expected)
                or additional_properties
                or any(
                    prop.schema_type != "string" or prop.nullable
                    for prop in properties.values()
                )
            ):
                raise CatalogSchemaError(
                    f"{path}: {semantic_type} must be a closed object with "
                    f"required string fields {sorted(expected)}"
                )
        elif schema_type != "string":
            raise CatalogSchemaError(f"{path}.semantic_type requires type=string")

    semantic_scope_parameter = raw_schema.get("semantic_scope_parameter")
    if semantic_scope_parameter is not None:
        if (
            not isinstance(semantic_scope_parameter, str)
            or _PORTABLE_ID_RE.fullmatch(semantic_scope_parameter) is None
        ):
            raise CatalogSchemaError(
                f"{path}.semantic_scope_parameter must be a portable parameter ID"
            )
        if semantic_type not in _SCOPED_SEMANTIC_TYPES:
            raise CatalogSchemaError(
                f"{path}.semantic_scope_parameter requires a scoped role semantic_type"
            )

    nullable = raw_schema.get("nullable", False)
    if not isinstance(nullable, bool):
        raise CatalogSchemaError(f"{path}.nullable must be a boolean")
    if nullable and schema_type not in _SCALAR_TYPES:
        raise CatalogSchemaError(f"{path}.nullable requires a scalar schema")

    return CatalogSchema(
        schema_type=str(schema_type),
        description=_optional_string(raw_schema, "description", path),
        properties=MappingProxyType(properties),
        required=required,
        additional_properties=additional_properties,
        additional_property_schema=additional_property_schema,
        items=items,
        length=length,
        min_items=min_items,
        max_items=max_items,
        enum_values=enum_values,
        constraints=_freeze_json_value(constraints),
        exactly_one=exactly_one,
        mutually_exclusive=mutually_exclusive,
        required_when=_freeze_json_value(required_when),
        allowed_when=_freeze_json_value(allowed_when),
        resolves_to=resolves_to,
        requires_current_value=requires_current_value,
        semantic_type=semantic_type,
        semantic_scope_parameter=semantic_scope_parameter,
        nullable=nullable,
    )


def _parse_key_value_schema(raw_schema: Mapping[str, Any], *, path: str) -> CatalogSchema:
    unknown = sorted(set(raw_schema) - {"type", "description", *_KEY_VALUE_ONLY_KEYS})
    if unknown:
        raise CatalogSchemaError(f"{path}: {_KEY_VALUE_TYPE} forbids schema keys {unknown}")
    key_parameter = raw_schema.get("value_type_from_key")
    if key_parameter is not None and (
        not isinstance(key_parameter, str) or _PORTABLE_ID_RE.fullmatch(key_parameter) is None
    ):
        raise CatalogSchemaError(
            f"{path}.value_type_from_key must be a portable parameter ID"
        )
    value_types = _string_tuple(raw_schema.get("value_types"), path=f"{path}.value_types")
    if not value_types or not set(value_types) <= set(KEY_VALUE_TYPES):
        raise CatalogSchemaError(
            f"{path}.value_types must be a non-empty subset of {list(KEY_VALUE_TYPES)}"
        )
    if list(value_types) != sorted(value_types):
        raise CatalogSchemaError(f"{path}.value_types must be sorted")
    return CatalogSchema(
        schema_type=_KEY_VALUE_TYPE,
        description=_optional_string(raw_schema, "description", path),
        value_type_from_key=key_parameter,
        value_types=value_types,
    )


def _key_value_matches(
    value_type: str,
    value: Any,
    registry: CatalogSchemaRegistry,
) -> bool:
    if value_type == "bool":
        return isinstance(value, bool)
    if value_type == "int64":
        return (
            isinstance(value, int)
            and not isinstance(value, bool)
            and -(1 << 63) <= value < 1 << 63
        )
    if value_type == "float64":
        return _is_number(value) and math.isfinite(float(value))
    if value_type == "string":
        return isinstance(value, str)
    if value_type == "string_list":
        return isinstance(value, (list, tuple)) and all(isinstance(item, str) for item in value)
    return not validate_value(
        CatalogSchema(ref="#/schema_types/stored_pose"),
        value,
        registry=registry,
    )


def validate_value(
    schema: CatalogSchema,
    value: Any,
    *,
    registry: CatalogSchemaRegistry,
    path: str = "$",
    allow_partial: bool = False,
) -> list[CatalogValueIssue]:
    concrete = registry.resolve(schema)
    return _validate_value(
        concrete, value, registry=registry, path=path, allow_partial=allow_partial
    )


def schema_type_label(schema: CatalogSchema, registry: CatalogSchemaRegistry) -> str:
    concrete = registry.resolve(schema)
    if concrete.schema_type == "enum":
        return "enum"
    if concrete.schema_type == "array":
        if concrete.items is not None:
            item_type = schema_type_label(concrete.items, registry)
            if item_type == "float":
                return "float_list"
            if item_type == "string":
                return "string_list"
        return "list"
    if concrete.schema_type == "object":
        return "dict"
    return str(concrete.schema_type)


def _validate_schema(
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
    *,
    path: str,
    stack: tuple[str, ...],
) -> None:
    if schema.is_ref():
        registry.resolve(schema, stack=stack)
        return
    if schema.has_default:
        issues = validate_value(schema, schema.default, registry=registry, path=f"{path}.default")
        if issues:
            raise CatalogSchemaError(f"{issues[0].path}: {issues[0].message}")
    if schema.schema_type == "object":
        declared = set(schema.properties)
        for field_name in schema.required:
            if field_name not in declared:
                raise CatalogSchemaError(
                    f"{path}.required references unknown field '{field_name}'"
                )
        for groups, group_name in (
            (schema.exactly_one, "exactly_one"),
            (schema.mutually_exclusive, "mutually_exclusive"),
        ):
            for group in groups:
                for field_name in group:
                    if field_name not in declared:
                        raise CatalogSchemaError(
                            f"{path}.{group_name} references unknown field '{field_name}'"
                        )
        for prop_name, prop_schema in schema.properties.items():
            for condition_name, condition in (
                ("required_when", prop_schema.required_when),
                ("allowed_when", prop_schema.allowed_when),
            ):
                for condition_field, expected in condition.items():
                    if isinstance(expected, tuple) and condition_field in declared:
                        selector = registry.resolve(schema.properties[condition_field])
                        if (
                            selector.schema_type != "enum"
                            or not expected
                            or any(type(item) is not str for item in expected)
                            or len(set(expected)) != len(expected)
                            or any(item not in selector.enum_values for item in expected)
                        ):
                            raise CatalogSchemaError(
                                f"{path}.properties.{prop_name}.{condition_name} "
                                "must use a nonempty set of declared enum values"
                            )
            for condition_field in prop_schema.required_when:
                if condition_field not in declared:
                    raise CatalogSchemaError(
                        f"{path}.properties.{prop_name}.required_when references "
                        f"unknown field '{condition_field}'"
                    )
            for condition_field in prop_schema.allowed_when:
                if condition_field not in declared:
                    raise CatalogSchemaError(
                        f"{path}.properties.{prop_name}.allowed_when references "
                        f"unknown field '{condition_field}'"
                    )
            _validate_schema(
                prop_schema,
                registry,
                path=f"{path}.properties.{prop_name}",
                stack=stack,
            )
        if schema.additional_property_schema is not None:
            _validate_schema(
                schema.additional_property_schema,
                registry,
                path=f"{path}.additional_properties",
                stack=stack,
            )
    elif schema.schema_type == "array":
        if schema.items is None:
            raise CatalogSchemaError(f"{path}: array schemas require items")
        _validate_schema(schema.items, registry, path=f"{path}.items", stack=stack)
    elif schema.schema_type == _KEY_VALUE_TYPE:
        if "stored_pose" in schema.value_types:
            registry.resolve(CatalogSchema(ref="#/schema_types/stored_pose"), stack=stack)
    elif schema.schema_type in _SCALAR_TYPES | {"enum", _CONDITION_TYPE, _DIRECT_CONDITION_TYPE}:
        if schema.properties:
            raise CatalogSchemaError(
                f"{path}: scalar/enum schemas cannot declare properties"
            )
        if schema.items is not None:
            raise CatalogSchemaError(
                f"{path}: scalar/enum schemas cannot declare items"
            )
    else:
        raise CatalogSchemaError(
            f"{path}: unsupported schema type '{schema.schema_type}'"
        )
    if schema.resolves_to is not None:
        registry.resolve(CatalogSchema(ref=schema.resolves_to), stack=stack)


def _validate_value(
    schema: CatalogSchema,
    value: Any,
    *,
    registry: CatalogSchemaRegistry,
    path: str,
    allow_partial: bool,
) -> list[CatalogValueIssue]:
    if value is None and schema.nullable:
        return []
    issues: list[CatalogValueIssue] = []
    schema_type = schema.schema_type
    if schema_type == "string":
        if not isinstance(value, str):
            issues.append(_issue(path, "must be a string", schema_type))
    elif schema_type == "float":
        if not _is_number(value):
            issues.append(_issue(path, "must be a number", schema_type))
        else:
            issues.extend(_validate_numeric_constraints(float(value), schema, path))
    elif schema_type == "int":
        if not isinstance(value, int) or isinstance(value, bool):
            issues.append(_issue(path, "must be an integer", schema_type))
        else:
            issues.extend(_validate_numeric_constraints(float(value), schema, path))
    elif schema_type == "bool":
        if not isinstance(value, bool):
            issues.append(_issue(path, "must be a boolean", schema_type))
    elif schema_type == _KEY_VALUE_TYPE:
        # Context-free check against the closed union; the Program validator
        # and the node-attempt resolver narrow it to the named key's type.
        if not any(
            _key_value_matches(value_type, value, registry)
            for value_type in schema.value_types
        ):
            issues.append(
                _issue(
                    path,
                    "must be a value of one of the key types: "
                    + ", ".join(schema.value_types),
                    schema_type,
                )
            )
    elif schema_type in {_CONDITION_TYPE, _DIRECT_CONDITION_TYPE}:
        issue = interrupt_condition.condition_structure_issue(
            value, direct_references=schema_type == _DIRECT_CONDITION_TYPE
        )
        if issue is not None:
            issue_path, message = issue
            issues.append(_issue(path + issue_path[1:], message, schema_type))
    elif schema_type == "enum":
        if not isinstance(value, str) or value not in schema.enum_values:
            issues.append(
                CatalogValueIssue(
                    path=path,
                    message=f"must be one of: {', '.join(schema.enum_values)}",
                    expected_schema="enum",
                    allowed_values=schema.enum_values,
                )
            )
    elif schema_type == "array":
        if not isinstance(value, list):
            issues.append(_issue(path, "must be an array", schema_type))
        else:
            if schema.length is not None and len(value) != schema.length:
                issues.append(
                    _issue(path, f"must contain exactly {schema.length} items", "array")
                )
            if schema.min_items is not None and len(value) < schema.min_items:
                issues.append(
                    _issue(
                        path, f"must contain at least {schema.min_items} items", "array"
                    )
                )
            if schema.max_items is not None and len(value) > schema.max_items:
                issues.append(
                    _issue(
                        path, f"must contain at most {schema.max_items} items", "array"
                    )
                )
            if schema.items is not None:
                for index, item in enumerate(value):
                    issues.extend(
                        validate_value(
                            schema.items,
                            item,
                            registry=registry,
                            path=f"{path}[{index}]",
                            allow_partial=False,
                        )
                    )
    elif schema_type == "object":
        if not isinstance(value, Mapping):
            issues.append(_issue(path, "must be an object", schema_type))
        else:
            if schema.semantic_type in _REFERENCE_SEMANTIC_FIELDS:
                for name in sorted(_REFERENCE_SEMANTIC_FIELDS[schema.semantic_type]):
                    member = value.get(name)
                    if not isinstance(member, str):
                        continue
                    try:
                        encoded_length = len(member.encode("utf-8"))
                    except UnicodeEncodeError:
                        encoded_length = 513
                    valid = (
                        bool(member)
                        and "\x00" not in member
                        and encoded_length <= (512 if name == "signal_name" else 128)
                        and (
                            name == "signal_name"
                            or (name == "component_id" and _PORTABLE_ID_RE.fullmatch(member) is not None)
                            or (name != "component_id" and _CATALOG_ID_RE.fullmatch(member) is not None)
                        )
                    )
                    if not valid:
                        issues.append(
                            _issue(f"{path}.{name}", "is not a valid identifier", "string")
                        )
            supplied = set(value)
            declared = set(schema.properties)
            unknown = tuple(sorted(supplied - declared))
            if not schema.additional_properties and unknown:
                issues.append(
                    CatalogValueIssue(
                        path=path,
                        message=f"must not contain unknown fields: {', '.join(unknown)}",
                        expected_schema="object",
                        valid_fields=tuple(sorted(declared)),
                    )
                )
            elif schema.additional_property_schema is not None:
                for field_name in unknown:
                    issues.extend(
                        validate_value(
                            schema.additional_property_schema,
                            value[field_name],
                            registry=registry,
                            path=f"{path}.{field_name}",
                            allow_partial=False,
                        )
                    )
            if not allow_partial:
                missing = tuple(
                    field for field in schema.required if field not in supplied
                )
                for field_name in missing:
                    issues.append(
                        _issue(f"{path}.{field_name}", "is required", "required")
                    )
            for field_name, prop_schema in schema.properties.items():
                if field_name in value:
                    if prop_schema.allowed_when and not _condition_matches(
                        prop_schema.allowed_when,
                        value,
                    ):
                        conditions = ", ".join(
                            f"{key}={condition_value}"
                            for key, condition_value in sorted(
                                prop_schema.allowed_when.items()
                            )
                        )
                        issues.append(
                            _issue(
                                f"{path}.{field_name}",
                                f"is only allowed when {conditions}",
                                "allowed_when",
                            )
                        )
                    issues.extend(
                        validate_value(
                            prop_schema,
                            value[field_name],
                            registry=registry,
                            path=f"{path}.{field_name}",
                            allow_partial=False,
                        )
                    )
                elif not allow_partial and _required_when_matches(prop_schema, value):
                    issues.append(
                        _issue(
                            f"{path}.{field_name}",
                            "is required by condition",
                            "required_when",
                        )
                    )
            for group in schema.exactly_one:
                count = sum(1 for field_name in group if field_name in supplied)
                if count != 1:
                    issues.append(
                        CatalogValueIssue(
                            path=path,
                            message=f"must contain exactly one of: {', '.join(group)}",
                            expected_schema="exactly_one",
                            valid_fields=group,
                        )
                    )
            for group in schema.mutually_exclusive:
                count = sum(1 for field_name in group if field_name in supplied)
                if count > 1:
                    issues.append(
                        CatalogValueIssue(
                            path=path,
                            message=f"must contain at most one of: {', '.join(group)}",
                            expected_schema="mutually_exclusive",
                            valid_fields=group,
                        )
                    )
    else:
        issues.append(
            _issue(path, f"unsupported schema type '{schema_type}'", str(schema_type))
        )
    return issues


def _to_json_schema(
    schema: CatalogSchema, registry: CatalogSchemaRegistry
) -> dict[str, Any]:
    if schema.is_ref():
        return _to_json_schema(registry.resolve(schema), registry)

    schema_type = schema.schema_type
    result: dict[str, Any] = {}
    if schema.description:
        result["description"] = schema.description

    if schema_type == "string":
        result["type"] = "string"
    elif schema_type == "float":
        result["type"] = "number"
    elif schema_type == "int":
        result["type"] = "integer"
    elif schema_type == "bool":
        result["type"] = "boolean"
    elif schema_type == _KEY_VALUE_TYPE:
        json_types = {
            "bool": {"type": "boolean"},
            "float64": {"type": "number"},
            "int64": {"type": "integer"},
            "string": {"type": "string"},
            "string_list": {"type": "array", "items": {"type": "string"}},
        }
        result["anyOf"] = [
            (
                _to_json_schema(CatalogSchema(ref="#/schema_types/stored_pose"), registry)
                if value_type == "stored_pose"
                else dict(json_types[value_type])
            )
            for value_type in schema.value_types
        ]
        if schema.value_type_from_key is not None:
            result["x-manyforge-value-type-from-key"] = schema.value_type_from_key
        return result
    elif schema_type in {_CONDITION_TYPE, _DIRECT_CONDITION_TYPE}:
        return {
            **result,
            **interrupt_condition.json_schema(
                direct_references=schema_type == _DIRECT_CONDITION_TYPE
            ),
        }
    elif schema_type == "enum":
        result["type"] = "string"
        result["enum"] = list(schema.enum_values)
    elif schema_type == "array":
        result["type"] = "array"
        result["items"] = _to_json_schema(
            schema.items or CatalogSchema(schema_type="string"), registry
        )
        if schema.length is not None:
            result["minItems"] = schema.length
            result["maxItems"] = schema.length
        if schema.min_items is not None:
            result["minItems"] = schema.min_items
        if schema.max_items is not None:
            result["maxItems"] = schema.max_items
    elif schema_type == "object":
        result["type"] = "object"
        result["additionalProperties"] = (
            _to_json_schema(schema.additional_property_schema, registry)
            if schema.additional_property_schema is not None
            else schema.additional_properties
        )
        result["properties"] = {
            name: _to_json_schema(prop_schema, registry)
            for name, prop_schema in sorted(schema.properties.items())
        }
        if schema.semantic_type in _REFERENCE_SEMANTIC_FIELDS:
            for name in _REFERENCE_SEMANTIC_FIELDS[schema.semantic_type]:
                prop = result["properties"][name]
                prop["minLength"] = 1
                prop["maxLength"] = 512 if name == "signal_name" else 128
                if name == "signal_name":
                    prop["x-manyforge-max-utf8-bytes"] = 512
                    prop["pattern"] = "^[^\\u0000]+$"
                else:
                    prop["pattern"] = (
                        "^[a-z][a-z0-9_.-]*$" if name == "component_id"
                        else "^[A-Za-z0-9][A-Za-z0-9_.-]*$"
                    )
        if schema.required:
            result["required"] = list(schema.required)
        all_of: list[dict[str, Any]] = []
        for group in schema.exactly_one:
            all_of.append(
                {"oneOf": [{"required": [field_name]} for field_name in group]}
            )
        for group in schema.mutually_exclusive:
            all_of.append(
                {
                    "not": {
                        "allOf": [{"required": [field_name]} for field_name in group],
                    }
                }
            )
        conditional_required: dict[tuple[tuple[str, Any], ...], list[str]] = {}
        for field_name, prop_schema in sorted(schema.properties.items()):
            if prop_schema.required_when:
                key = tuple(sorted(prop_schema.required_when.items()))
                conditional_required.setdefault(key, []).append(field_name)
        for condition_items, required_fields in sorted(
            conditional_required.items(), key=lambda item: repr(item[0])
        ):
            all_of.append(
                {
                    "if": _json_schema_condition(dict(condition_items)),
                    "then": {"required": sorted(required_fields)},
                }
            )
        for field_name, prop_schema in sorted(schema.properties.items()):
            if prop_schema.allowed_when:
                all_of.append(
                    {
                        "if": {
                            "not": _json_schema_condition(prop_schema.allowed_when),
                        },
                        "then": {"not": {"required": [field_name]}},
                    }
                )
        if all_of:
            result["allOf"] = all_of
    else:
        result["type"] = "string"
    _apply_numeric_json_constraints(result, schema)
    if schema.semantic_type is not None:
        result["x-manyforge-semantic-type"] = schema.semantic_type
    if schema.semantic_scope_parameter is not None:
        result["x-manyforge-semantic-scope-parameter"] = schema.semantic_scope_parameter
    if schema.nullable:
        return {"anyOf": [result, {"type": "null"}]}
    return result


def _schema_type_name_from_ref(ref: str) -> str:
    prefix = "#/schema_types/"
    if not ref.startswith(prefix) or ref == prefix:
        raise CatalogSchemaError(
            f"Schema references must use '#/schema_types/<name>', got '{ref}'"
        )
    return ref[len(prefix) :]


def _optional_string(raw: Mapping[str, Any], key: str, path: str) -> str:
    value = raw.get(key, "")
    if value is None:
        return ""
    if not isinstance(value, str):
        raise CatalogSchemaError(f"{path}.{key} must be a string")
    return value


def _optional_non_negative_int(
    raw: Mapping[str, Any], key: str, path: str
) -> int | None:
    if key not in raw or raw[key] is None:
        return None
    value = raw[key]
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CatalogSchemaError(f"{path}.{key} must be a non-negative integer")
    return value


def _string_tuple(value: Any, *, path: str) -> tuple[str, ...]:
    if value in (None, ()):
        return ()
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item.strip() for item in value
    ):
        raise CatalogSchemaError(f"{path} must be a list of non-empty strings")
    if len(value) != len(set(value)):
        raise CatalogSchemaError(f"{path} must not contain duplicate values")
    return tuple(value)


def _field_group_tuple(value: Any, *, path: str) -> tuple[tuple[str, ...], ...]:
    if value in (None, ()):
        return ()
    if not isinstance(value, list):
        raise CatalogSchemaError(f"{path} must be a list")
    groups: list[tuple[str, ...]] = []
    for index, raw_group in enumerate(value):
        if not isinstance(raw_group, list) or not all(
            isinstance(item, str) and item.strip() for item in raw_group
        ):
            raise CatalogSchemaError(
                f"{path}[{index}] must be a list of non-empty strings"
            )
        if len(raw_group) < 2:
            raise CatalogSchemaError(
                f"{path}[{index}] must contain at least two fields"
            )
        if len(raw_group) != len(set(raw_group)):
            raise CatalogSchemaError(
                f"{path}[{index}] must not contain duplicate fields"
            )
        groups.append(tuple(raw_group))
    return tuple(groups)


def _is_number(value: Any) -> bool:
    return (isinstance(value, int) or isinstance(value, float)) and not isinstance(
        value, bool
    )


def _validate_numeric_constraints(
    value: float,
    schema: CatalogSchema,
    path: str,
) -> list[CatalogValueIssue]:
    issues: list[CatalogValueIssue] = []
    constraints = schema.constraints
    if "min_inclusive" in constraints and value < float(constraints["min_inclusive"]):
        issues.append(
            _issue(
                path, f"must be >= {constraints['min_inclusive']}", "numeric_constraint"
            )
        )
    if "max_inclusive" in constraints and value > float(constraints["max_inclusive"]):
        issues.append(
            _issue(
                path, f"must be <= {constraints['max_inclusive']}", "numeric_constraint"
            )
        )
    if "min_exclusive" in constraints and value <= float(constraints["min_exclusive"]):
        issues.append(
            _issue(
                path, f"must be > {constraints['min_exclusive']}", "numeric_constraint"
            )
        )
    if "max_exclusive" in constraints and value >= float(constraints["max_exclusive"]):
        issues.append(
            _issue(
                path, f"must be < {constraints['max_exclusive']}", "numeric_constraint"
            )
        )
    if "not_equal" in constraints and value == constraints["not_equal"]:
        issues.append(
            _issue(
                path, f"must not equal {constraints['not_equal']}", "numeric_constraint"
            )
        )
    return issues


def _apply_numeric_json_constraints(
    result: dict[str, Any], schema: CatalogSchema
) -> None:
    constraints = schema.constraints
    if "min_inclusive" in constraints:
        result["minimum"] = constraints["min_inclusive"]
    if "max_inclusive" in constraints:
        result["maximum"] = constraints["max_inclusive"]
    if "min_exclusive" in constraints:
        result["exclusiveMinimum"] = constraints["min_exclusive"]
    if "max_exclusive" in constraints:
        result["exclusiveMaximum"] = constraints["max_exclusive"]
    if "not_equal" in constraints:
        result["not"] = {"const": constraints["not_equal"]}


def _required_when_matches(schema: CatalogSchema, value: Mapping[str, Any]) -> bool:
    return _condition_matches(schema.required_when, value)


def _condition_matches(condition: Mapping[str, Any], value: Mapping[str, Any]) -> bool:
    if not condition:
        return False
    return all(
        value.get(key) in expected if isinstance(expected, tuple)
        else value.get(key) == expected
        for key, expected in condition.items()
    )


def _json_schema_condition(condition: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "required": [key for key, _value in sorted(condition.items())],
        "properties": {
            key: ({"enum": list(value)} if isinstance(value, tuple) else {"const": value})
            for key, value in sorted(condition.items())
        },
    }


def _issue(path: str, message: str, expected_schema: str) -> CatalogValueIssue:
    return CatalogValueIssue(
        path=path,
        message=message,
        expected_schema=expected_schema,
    )
