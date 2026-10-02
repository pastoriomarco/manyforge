#!/usr/bin/env python3
"""Generate or verify the compiler-owned assistant instrument source.

The active v2 node catalog remains the source of truth for node parameters.
This tool projects that catalog into strict, standalone JSON Schemas for the
bounded assistant authoring surface and checks the maintained mode profile
against the same exact tool/node IDs.  The C++ compiler embeds and independently
decodes the generated source; neither Composer nor the bridge owns a registry.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
from typing import Any

from manyforge_behavior.catalog import CatalogEntry, NodeCatalog
from manyforge_behavior.catalog_schema import CatalogSchema
from manyforge_behavior.skill_loader import default_skills_dir, load_skill_catalog


SOURCE_API_VERSION = "manyforge.assistant_instrument_registry_source.v1"
MAX_ITEMS = 4096
IDENTIFIER: dict[str, Any] = {
    "type": "string",
    "pattern": "^[a-z][a-z0-9_.-]*$",
    "maxLength": 256,
}
INSTANCE_OR_ROOT: dict[str, Any] = {"oneOf": [copy.deepcopy(IDENTIFIER), {"const": "@root"}]}
BOUNDED_TEXT: dict[str, Any] = {"type": "string", "maxLength": 4096}
# One whole-parameter value source in an assistant proposal: a Program
# parameter, a blackboard key, or one declared field of a blackboard key.
_FIELD_KIND: dict[str, Any] = {
    "properties": {"kind": {"const": "blackboard_field"}},
    "required": ["kind"],
}
VALUE_SOURCE: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "field": {"type": "string"},
        "kind": {
            "type": "string",
            "enum": ["program_parameter", "blackboard_key", "blackboard_field"],
        },
        "reference": {"type": "string"},
    },
    "required": ["kind", "reference"],
    "allOf": [
        {"if": copy.deepcopy(_FIELD_KIND), "then": {"required": ["field"]}},
        {
            "if": {"not": copy.deepcopy(_FIELD_KIND)},
            "then": {"not": {"required": ["field"]}},
        },
    ],
}

# Exact translation of the maintained pre-cutover scene-authoring allowlist.
# The old aggregate motion node becomes the three explicit v2 motion forms.
ASSISTANT_NODE_IDS = (
    "fallback",
    "gripper.command_position",
    "inverter",
    "logic.set_value",
    "logic.wait_for_value",
    "motion.follow_cartesian_path",
    "motion.move_to_joint_target",
    "motion.move_to_pose",
    "parallel",
    "repeat",
    "retry",
    "scene.add_object",
    "scene.attach_carried_object",
    "scene.detach_carried_object",
    "scene.remove_object",
    "scene.update_object_pose",
    "scene.upsert_object",
    "sequence",
    "timer",
)

QUERY_TOOL_IDS = (
    "inspect_isaac_scene",
    "program_read",
    "scene_inspect",
)

COMPOSER_TOOL_IDS = (
    "blackboard_draft_remove_keys",
    "blackboard_draft_remove_namespaces",
    "blackboard_draft_upsert_keys",
    "blackboard_draft_upsert_namespaces",
    "catalog_read",
    "deployment_capabilities_read",
    "inspect_isaac_scene",
    "program_draft_remove_parameters",
    "program_draft_upsert_parameters",
    "program_read",
    "propose_scene_object_nodes",
    "propose_scene_objects",
    "scene_draft_add_object",
    "scene_draft_remove_objects",
    "scene_draft_update_object",
    "scene_draft_upsert_objects",
    "scene_inspect",
    "skills_read",
    "tree_draft_change_node_kind",
    "tree_draft_delete_node",
    "tree_draft_insert_node",
    "tree_draft_move_node",
    "tree_draft_rename_node",
    "tree_draft_replace_subtree",
    "tree_draft_update_node_params",
    "tree_draft_wrap_node",
)

PROGRAM_TYPES = (
    "bool",
    "float64",
    "int64",
    "model_response",
    "pick_planning_result",
    "scene_resource",
    "scene_selection_context",
    "scene_selection_result",
    "stored_pose",
    "string",
    "string_list",
)
STRUCTURED_PROGRAM_TYPES = {
    "model_response",
    "pick_planning_result",
    "scene_selection_context",
    "scene_selection_result",
}
RUNTIME_MUTABILITY = ("immutable", "before_run")
BLACKBOARD_ACCESS = ("read", "write", "read_write")
BLACKBOARD_PERSISTENCE = ("run", "persistent")
VALUE_SOURCE_FORBIDDEN_PARAMETER_IDS = {
    "contact_policy_id",
    "model_endpoint_alias",
    "prompt_template_id",
}
VALUE_SOURCE_FORBIDDEN_SEMANTICS = {
    "blackboard_key",
    "collision_body_role",
    "component_role",
    "frame_role",
    "resource_role",
    "source_joint",
    "target_role",
}


class GenerationError(ValueError):
    """The catalog, maintained profile, or generated source is inconsistent."""


def _object(
    properties: dict[str, Any],
    *,
    required: tuple[str, ...] = (),
    all_of: list[dict[str, Any]] | None = None,
    one_of: list[dict[str, Any]] | None = None,
    defs: dict[str, Any] | None = None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "type": "object",
        "additionalProperties": False,
        "properties": properties,
    }
    if required:
        result["required"] = list(required)
    if all_of:
        result["allOf"] = all_of
    if one_of:
        result["oneOf"] = one_of
    if defs:
        result["$defs"] = defs
    return result


def _array(
    items: dict[str, Any],
    *,
    minimum: int | None = None,
    maximum: int = MAX_ITEMS,
    unique: bool = False,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "type": "array",
        "items": items,
        "maxItems": maximum,
    }
    if minimum is not None:
        result["minItems"] = minimum
    if unique:
        result["uniqueItems"] = True
    return result


def _string_array(*, minimum: int | None = None) -> dict[str, Any]:
    return _array(copy.deepcopy(IDENTIFIER), minimum=minimum, unique=True)


def _strip_descriptions(value: Any, *, in_properties: bool = False) -> Any:
    """Keep field names named description, but remove schema annotations."""
    if isinstance(value, dict):
        if in_properties:
            return {key: _strip_descriptions(item) for key, item in value.items()}
        return {
            key: _strip_descriptions(
                item,
                in_properties=key in {"properties", "$defs", "definitions"},
            )
            for key, item in value.items()
            if key != "description"
        }
    if isinstance(value, list):
        return [_strip_descriptions(item) for item in value]
    return copy.deepcopy(value)


def _factor_schema(schema: dict[str, Any]) -> dict[str, Any]:
    """Deduplicate repeated closed sub-schemas with local content refs.

    Motion nodes share large pose/dynamics/value-source shapes. Keeping each
    tool standalone is required, but repeating those shapes inside one tool is
    not. Local ``$defs`` preserve exact JSON-Schema meaning while materially
    reducing retained projections and model-request payloads.
    """

    counts: dict[str, int] = {}
    originals: dict[str, dict[str, Any]] = {}

    def identity(value: dict[str, Any]) -> tuple[str, int]:
        canonical = json.dumps(value, sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest(), len(canonical)

    def visit(value: dict[str, Any]) -> None:
        digest, size = identity(value)
        if size >= 160 and "$ref" not in value:
            counts[digest] = counts.get(digest, 0) + 1
            originals.setdefault(digest, value)
        for key, item in value.items():
            if key in {"properties", "$defs", "definitions"} and isinstance(item, dict):
                for child in item.values():
                    if isinstance(child, dict):
                        visit(child)
            elif key in {"items", "contains", "if", "then", "else", "not"}:
                if isinstance(item, dict):
                    visit(item)
            elif key == "additionalProperties" and isinstance(item, dict):
                visit(item)
            elif key in {"allOf", "anyOf", "oneOf"} and isinstance(item, list):
                for child in item:
                    if isinstance(child, dict):
                        visit(child)

    visit(schema)
    candidates = {digest for digest, count in counts.items() if count > 1}
    if not candidates:
        return copy.deepcopy(schema)
    names = {digest: f"s_{digest[:16]}" for digest in sorted(candidates)}
    if len(set(names.values())) != len(names):
        raise GenerationError("assistant schema definition-name collision")

    def render(value: dict[str, Any], *, keep: str | None = None) -> dict[str, Any]:
        digest, _size = identity(value)
        if digest in candidates and digest != keep:
            return {"$ref": f"#/$defs/{names[digest]}"}
        result: dict[str, Any] = {}
        for key, item in value.items():
            if key in {"properties", "$defs", "definitions"} and isinstance(item, dict):
                result[key] = {
                    name: (render(child) if isinstance(child, dict) else copy.deepcopy(child))
                    for name, child in item.items()
                }
            elif key in {"items", "contains", "if", "then", "else", "not"}:
                result[key] = render(item) if isinstance(item, dict) else copy.deepcopy(item)
            elif key == "additionalProperties" and isinstance(item, dict):
                result[key] = render(item)
            elif key in {"allOf", "anyOf", "oneOf"} and isinstance(item, list):
                result[key] = [
                    render(child) if isinstance(child, dict) else copy.deepcopy(child)
                    for child in item
                ]
            else:
                result[key] = copy.deepcopy(item)
        return result

    root_digest, _root_size = identity(schema)
    result = render(schema, keep=root_digest)
    definitions = dict(result.get("$defs", {}))
    overlapping_names = set(definitions) & set(names.values())
    if overlapping_names:
        raise GenerationError(
            f"assistant schema generated definitions overlap existing names: "
            f"{sorted(overlapping_names)}"
        )
    for digest in sorted(candidates):
        definitions[names[digest]] = render(originals[digest], keep=digest)
    result["$defs"] = definitions
    return result


def _catalog_schema(catalog: NodeCatalog, schema: CatalogSchema) -> dict[str, Any]:
    return _strip_descriptions(catalog.schema_registry.to_json_schema(schema))


def _authoring_patch_schema(
    catalog: NodeCatalog,
    persisted_schema: CatalogSchema,
) -> dict[str, Any] | None:
    """Project a catalog-declared authoring input as a partial Draft patch."""

    if persisted_schema.ref is None:
        return None
    matches = [
        schema
        for schema in catalog.schema_registry.types.values()
        if schema.resolves_to == persisted_schema.ref
    ]
    if not matches:
        return None
    if len(matches) != 1:
        raise GenerationError(f"multiple authoring schemas resolve to {persisted_schema.ref}")
    authoring = matches[0]
    if authoring.schema_type != "object" or len(authoring.exactly_one) != 1:
        raise GenerationError(
            f"authoring schema for {persisted_schema.ref} is not one closed " "single-intent object"
        )
    rendered = _catalog_schema(catalog, authoring)
    properties = rendered.get("properties")
    if type(properties) is not dict:
        raise GenerationError(f"authoring schema for {persisted_schema.ref} has no properties")
    intent_fields = authoring.exactly_one[0]
    retained_fields = tuple(sorted(set(properties) - set(intent_fields)))
    if not retained_fields:
        raise GenerationError(
            f"authoring schema for {persisted_schema.ref} has no patchable " "non-intent field"
        )
    no_intent_branch = {
        "not": {"anyOf": [{"required": [field_name]} for field_name in intent_fields]},
        "anyOf": [{"required": [field_name]} for field_name in retained_fields],
    }
    result = _object(
        copy.deepcopy(properties),
        one_of=[
            *({"required": [field_name]} for field_name in intent_fields),
            no_intent_branch,
        ],
    )
    result["minProperties"] = 1
    return result


def _semantic_types(
    catalog: NodeCatalog,
    schema: CatalogSchema,
    *,
    seen: frozenset[int] = frozenset(),
) -> frozenset[str]:
    identity = id(schema)
    if identity in seen:
        return frozenset()
    concrete = catalog.schema_registry.resolve(schema)
    result = {concrete.semantic_type} if concrete.semantic_type else set()
    next_seen = seen | {identity}
    for child in concrete.properties.values():
        result.update(_semantic_types(catalog, child, seen=next_seen))
    if concrete.items is not None:
        result.update(_semantic_types(catalog, concrete.items, seen=next_seen))
    if concrete.additional_property_schema is not None:
        result.update(_semantic_types(catalog, concrete.additional_property_schema, seen=next_seen))
    return frozenset(result)


def _value_source_parameter_ids(catalog: NodeCatalog, entry: CatalogEntry) -> frozenset[str]:
    selectors = {
        selector
        for requirement in entry.required_capabilities
        for selector in (
            requirement.when_parameter,
            requirement.target_parameter,
            requirement.component_parameter,
        )
        if selector is not None
    }
    result: set[str] = set()
    for parameter in entry.parameters:
        semantics = _semantic_types(catalog, parameter.schema)
        concrete = catalog.schema_registry.resolve(parameter.schema)
        if (
            parameter.id in VALUE_SOURCE_FORBIDDEN_PARAMETER_IDS
            or parameter.id in selectors
            or "source_joint" in semantics
            or concrete.semantic_type in VALUE_SOURCE_FORBIDDEN_SEMANTICS
        ):
            continue
        result.add(parameter.id)
    return frozenset(result)


def _nested_presence(field: str, nested: str) -> dict[str, Any]:
    return {
        "required": [field],
        "properties": {field: {"required": [nested]}},
    }


def _node_configuration_schema(
    catalog: NodeCatalog,
    entry: CatalogEntry,
    *,
    include_instance: bool,
    complete: bool,
    include_children: bool,
) -> dict[str, Any]:
    literal_properties = {
        parameter.id: _catalog_schema(catalog, parameter.schema)
        for parameter in sorted(entry.parameters, key=lambda item: item.id)
    }
    value_source_ids = _value_source_parameter_ids(catalog, entry)
    value_source_properties = {
        parameter_id: copy.deepcopy(VALUE_SOURCE)
        for parameter_id in sorted(value_source_ids)
    }
    required_literals = tuple(
        sorted(
            parameter.id
            for parameter in entry.parameters
            if complete
            and parameter.required
            and not parameter.has_default
            and parameter.id not in value_source_ids
        )
    )
    parameters = _object(literal_properties, required=required_literals)
    properties: dict[str, Any] = {
        "node_id": {"const": entry.id},
        "parameters": parameters,
    }
    required = ["node_id", "parameters"]
    if include_instance:
        properties["instance_id"] = copy.deepcopy(IDENTIFIER)
        required.append("instance_id")
    if value_source_properties:
        value_sources = _object(value_source_properties)
        value_sources["minProperties"] = 1
        properties["value_sources"] = value_sources
    if include_children:
        properties["children"] = _array(
            {"$ref": "#/$defs/node"},
            minimum=entry.children_min,
            maximum=entry.children_max,
        )
        required.append("children")

    all_of: list[dict[str, Any]] = []
    for parameter_id in sorted(value_source_ids):
        # Assistant proposals use the whole-parameter subset: a parameter is
        # supplied literally or through one top-level value source. (Programs
        # may keep a literal beside its source for "Type values"; a proposal
        # never needs one.)
        all_of.append(
            {
                "not": {
                    "allOf": [
                        _nested_presence("parameters", parameter_id),
                        _nested_presence("value_sources", parameter_id),
                    ]
                }
            }
        )
    if complete:
        for parameter in sorted(entry.parameters, key=lambda item: item.id):
            if (
                not parameter.required
                or parameter.has_default
                or parameter.id not in value_source_ids
            ):
                continue
            all_of.append(
                {
                    "oneOf": [
                        _nested_presence("parameters", parameter.id),
                        _nested_presence("value_sources", parameter.id),
                    ]
                }
            )
    return _object(
        properties,
        required=tuple(required),
        all_of=all_of,
    )


def _node_union(catalog: NodeCatalog, *, recursive: bool) -> dict[str, Any]:
    variants = [
        _node_configuration_schema(
            catalog,
            catalog.get(node_id),
            include_instance=True,
            complete=True,
            include_children=recursive,
        )
        for node_id in ASSISTANT_NODE_IDS
    ]
    return {"oneOf": variants}


def _node_tool_schema(catalog: NodeCatalog, *, replace: bool) -> dict[str, Any]:
    target_field = "instance_id"
    payload_field = "subtree" if replace else "node"
    location = (
        {}
        if replace
        else {
            "parent_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "index": {"type": "integer", "minimum": 0, "maximum": MAX_ITEMS},
            "after_instance_id": copy.deepcopy(IDENTIFIER),
            "before_instance_id": copy.deepcopy(IDENTIFIER),
        }
    )
    properties: dict[str, Any] = {
        payload_field: {"$ref": "#/$defs/node"},
        **location,
    }
    required: tuple[str, ...]
    one_of: list[dict[str, Any]] | None = None
    if replace:
        properties[target_field] = copy.deepcopy(INSTANCE_OR_ROOT)
        required = (target_field, payload_field)
    else:
        required = (payload_field,)
        one_of = [
            {"required": ["parent_instance_id"]},
            {"required": ["after_instance_id"]},
            {"required": ["before_instance_id"]},
        ]
        # index is meaningful only with an explicit parent.
        one_of[0]["properties"] = {}
        one_of[1]["not"] = {"required": ["index"]}
        one_of[2]["not"] = {"required": ["index"]}
    return _object(
        properties,
        required=required,
        one_of=one_of,
        defs={"node": _node_union(catalog, recursive=True)},
    )


def _update_node_schema(catalog: NodeCatalog) -> dict[str, Any]:
    variants: list[dict[str, Any]] = []
    for node_id in ASSISTANT_NODE_IDS:
        entry = catalog.get(node_id)
        config = _node_configuration_schema(
            catalog,
            entry,
            include_instance=False,
            complete=False,
            include_children=False,
        )
        config_properties = config["properties"]
        parameters = copy.deepcopy(config_properties["parameters"])
        for parameter in entry.parameters:
            authoring_patch = _authoring_patch_schema(catalog, parameter.schema)
            if authoring_patch is not None:
                parameters["properties"][parameter.id] = authoring_patch
        properties: dict[str, Any] = {
            "instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "expected_node_id": {"const": node_id},
            "parameters": parameters,
            "merge": {"type": "boolean"},
        }
        if "value_sources" in config_properties:
            properties["value_sources"] = config_properties["value_sources"]
        all_of = list(config.get("allOf", []))
        all_of.append(
            {
                "anyOf": [
                    {"required": ["parameters"]},
                    {"required": ["value_sources"]},
                ]
            }
        )
        variants.append(
            _object(
                properties,
                required=("instance_id", "expected_node_id"),
                all_of=all_of,
            )
        )
    return {"oneOf": variants}


def _change_kind_schema(catalog: NodeCatalog) -> dict[str, Any]:
    variants = [
        _node_configuration_schema(
            catalog,
            catalog.get(node_id),
            include_instance=False,
            complete=False,
            include_children=False,
        )
        for node_id in ASSISTANT_NODE_IDS
    ]
    return _object(
        {
            "instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "replacement": {"oneOf": variants},
            "parameters_mode": {
                "type": "string",
                "enum": ["replace", "merge", "strict"],
            },
        },
        required=("instance_id", "replacement", "parameters_mode"),
    )


def _wrap_node_schema(catalog: NodeCatalog) -> dict[str, Any]:
    variants = [
        _node_configuration_schema(
            catalog,
            catalog.get(node_id),
            include_instance=True,
            complete=True,
            include_children=False,
        )
        for node_id in ASSISTANT_NODE_IDS
        if catalog.get(node_id).children_min <= 1 <= catalog.get(node_id).children_max
    ]
    return _object(
        {
            "target_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "wrapper": {"oneOf": variants},
        },
        required=("target_instance_id", "wrapper"),
    )


def _orientation_schema() -> dict[str, Any]:
    return _object(
        {
            "xyzw": _array({"type": "number"}, minimum=4, maximum=4),
            "rpy_deg": _array({"type": "number"}, minimum=3, maximum=3),
        },
        one_of=[{"required": ["xyzw"]}, {"required": ["rpy_deg"]}],
    )


def _initial_scene_pose_schema() -> dict[str, Any]:
    frame_ref = {
        "oneOf": [
            _object({"kind": {"const": "cell_frame"}}, required=("kind",)),
            _object(
                {
                    "kind": {"const": "component_frame"},
                    "component_role": copy.deepcopy(IDENTIFIER),
                    "model_role": copy.deepcopy(IDENTIFIER),
                    "frame_role": copy.deepcopy(IDENTIFIER),
                },
                required=("kind", "component_role", "model_role", "frame_role"),
            ),
        ]
    }
    return _object(
        {
            "frame_ref": frame_ref,
            "position_m": _array({"type": "number"}, minimum=3, maximum=3),
            "orientation": _orientation_schema(),
        },
        required=("frame_ref", "position_m", "orientation"),
    )


def _scene_pose_authoring_schema(catalog: NodeCatalog) -> dict[str, Any]:
    absolute = _initial_scene_pose_schema()
    properties = copy.deepcopy(absolute["properties"])
    properties.update(
        {
            "rotate_local_rpy_deg": _array({"type": "number"}, minimum=3, maximum=3),
            "rotate_world_rpy_deg": _array({"type": "number"}, minimum=3, maximum=3),
            "rpy_delta_deg": _array({"type": "number"}, minimum=3, maximum=3),
            "orientation_from": _catalog_schema(
                catalog, catalog.schema_registry.types["orientation_reference"]
            ),
        }
    )
    return _object(
        properties,
        required=("frame_ref", "position_m"),
        one_of=[
            {"required": ["orientation"]},
            {"required": ["rotate_local_rpy_deg"]},
            {"required": ["rotate_world_rpy_deg"]},
            {"required": ["rpy_delta_deg"]},
            {"required": ["orientation_from"]},
        ],
    )


def _scene_resource_schema(catalog: NodeCatalog, *, authoring_pose: bool = False) -> dict[str, Any]:
    return _object(
        {
            "scene_resource_id": copy.deepcopy(IDENTIFIER),
            "shape": _catalog_schema(catalog, catalog.schema_registry.types["shape"]),
            "pose": (
                _scene_pose_authoring_schema(catalog)
                if authoring_pose
                else _initial_scene_pose_schema()
            ),
            "resource_roles": _string_array(minimum=1),
            "collision_policy_ids": _string_array(minimum=1),
        },
        required=("scene_resource_id", "shape", "pose"),
    )


def _constraints_schema(*, numeric: str | None = None, length: bool = False) -> dict[str, Any]:
    properties: dict[str, Any] = {}
    all_of: list[dict[str, Any]] = []
    if numeric is not None:
        bound = {"type": numeric}
        properties.update(
            {
                "min_inclusive": bound,
                "min_exclusive": bound,
                "max_inclusive": bound,
                "max_exclusive": bound,
            }
        )
        all_of.extend(
            [
                {"not": {"required": ["min_inclusive", "min_exclusive"]}},
                {"not": {"required": ["max_inclusive", "max_exclusive"]}},
            ]
        )
    if length:
        length_bound = {"type": "integer", "minimum": 0, "maximum": MAX_ITEMS}
        properties.update({"min_length": length_bound, "max_length": length_bound})
    return _object(properties, all_of=all_of)


def _authored_value_schemas(catalog: NodeCatalog) -> dict[str, dict[str, Any]]:
    """Schemas of authored parameter defaults and blackboard initial values."""

    return {
        "bool": {"type": "boolean"},
        "float64": {"type": "number"},
        "int64": {"type": "integer"},
        "scene_resource": copy.deepcopy(IDENTIFIER),
        "stored_pose": _catalog_schema(catalog, catalog.schema_registry.types["stored_pose"]),
        "string": {"type": "string", "maxLength": 1_000_000},
        "string_list": _array({"type": "string", "maxLength": 1_000_000}, maximum=MAX_ITEMS),
    }


def _program_parameter_schema(catalog: NodeCatalog) -> dict[str, Any]:
    default_schemas = _authored_value_schemas(catalog)
    variants: list[dict[str, Any]] = []
    for parameter_type in PROGRAM_TYPES:
        if parameter_type in STRUCTURED_PROGRAM_TYPES:
            default_schema: dict[str, Any] = {"type": "null"}
        else:
            default_schema = default_schemas[parameter_type]
        if parameter_type == "float64":
            constraints = _constraints_schema(numeric="number")
        elif parameter_type == "int64":
            constraints = _constraints_schema(numeric="integer")
        elif parameter_type in {"string", "string_list"}:
            constraints = _constraints_schema(length=True)
        else:
            constraints = _constraints_schema()
        variants.append(
            _object(
                {
                    "parameter_id": copy.deepcopy(IDENTIFIER),
                    "type": {"const": parameter_type},
                    "default": default_schema,
                    "constraints": constraints,
                    "runtime_mutability": {
                        "type": "string",
                        "enum": list(RUNTIME_MUTABILITY),
                    },
                    "description": copy.deepcopy(BOUNDED_TEXT),
                },
                required=(
                    "parameter_id",
                    "type",
                    "default",
                    "runtime_mutability",
                ),
            )
        )
    return {"oneOf": variants}


def _blackboard_key_schema(catalog: NodeCatalog) -> dict[str, Any]:
    initial_schemas = _authored_value_schemas(catalog)
    field = _object(
        {
            "field": copy.deepcopy(IDENTIFIER),
            "type": {"type": "string", "enum": list(PROGRAM_TYPES)},
        },
        required=("field", "type"),
    )
    variants: list[dict[str, Any]] = []
    for key_type in PROGRAM_TYPES:
        properties: dict[str, Any] = {
            "key": copy.deepcopy(IDENTIFIER),
            "type": {"const": key_type},
            "access": {"type": "string", "enum": list(BLACKBOARD_ACCESS)},
            "persistence": {
                "type": "string",
                "enum": list(BLACKBOARD_PERSISTENCE),
            },
            "description": copy.deepcopy(BOUNDED_TEXT),
        }
        if key_type in STRUCTURED_PROGRAM_TYPES:
            properties["fields"] = _array(field, minimum=1)
        else:
            properties["initial"] = copy.deepcopy(initial_schemas[key_type])
        variants.append(
            _object(
                properties,
                required=("key", "type", "access", "persistence"),
            )
        )
    return {"oneOf": variants}


def _inspection_report_schema(catalog: NodeCatalog) -> dict[str, Any]:
    candidate = _object(
        {
            "source_root": {"type": "string", "minLength": 1, "maxLength": 4096},
            "scene_resource": _scene_resource_schema(catalog),
            "collider_paths": _array(
                {"type": "string", "minLength": 1, "maxLength": 4096},
                unique=True,
            ),
            "warnings": _array(copy.deepcopy(BOUNDED_TEXT)),
        },
        required=("source_root", "scene_resource"),
    )
    return _object(
        {"objects": _array(candidate, maximum=MAX_ITEMS)},
        required=("objects",),
    )


def _tool(
    tool_id: str,
    effect: str,
    description: str,
    input_schema: dict[str, Any],
) -> dict[str, Any]:
    return {
        "tool_id": tool_id,
        "effect": effect,
        "description": description,
        "input_schema": _factor_schema(input_schema),
    }


def _tools(catalog: NodeCatalog) -> list[dict[str, Any]]:
    no_args = _object({})
    scene_resource = _scene_resource_schema(catalog)
    authored_scene_resource = _scene_resource_schema(catalog, authoring_pose=True)
    add_scene = _object(
        {"scene_resource": scene_resource},
        required=("scene_resource",),
    )
    update_scene = _object(
        {"scene_resource": authored_scene_resource},
        required=("scene_resource",),
    )
    tools = [
        _tool(
            "alarms_read",
            "read_only",
            "Read the active headless alarm snapshot.",
            no_args,
        ),
        _tool(
            "catalog_read",
            "read_only",
            "Read bounded metadata for active behavior-node catalog entries.",
            _object(
                {
                    "entry_ids": _string_array(),
                    "category": copy.deepcopy(IDENTIFIER),
                    "kind": {
                        "type": "string",
                        "enum": ["leaf", "decorator", "composite"],
                    },
                    "include_parameters": {"type": "boolean"},
                }
            ),
        ),
        _tool(
            "deployment_capabilities_read",
            "read_only",
            "Read capabilities of the active retained deployment.",
            no_args,
        ),
        _tool(
            "history_read",
            "read_only",
            "Read one bounded page from the headless historian.",
            _object(
                {
                    "cursor": {"type": "string", "minLength": 1, "maxLength": 1024},
                    "limit": {"type": "integer", "minimum": 1, "maximum": 32},
                }
            ),
        ),
        _tool(
            "inspect_isaac_scene",
            "read_only",
            "Read a normalized Isaac scene-inspection report without mutation.",
            _object(
                {
                    "include_subtrees": _array(
                        {"type": "string", "minLength": 1, "maxLength": 4096},
                        unique=True,
                    ),
                    "exclude_subtrees": _array(
                        {"type": "string", "minLength": 1, "maxLength": 4096},
                        unique=True,
                    ),
                    "probe_prims": _array(
                        {"type": "string", "minLength": 1, "maxLength": 4096},
                        unique=True,
                    ),
                    "output_profile": {
                        "type": "string",
                        "enum": ["llm_scene_authoring", "raw_inventory"],
                    },
                    "mesh_export_dir": {
                        "type": "string",
                        "minLength": 1,
                        "maxLength": 4096,
                    },
                    "mesh_source_preference": {
                        "type": "string",
                        "enum": ["auto", "original", "collider", "primitive"],
                    },
                }
            ),
        ),
        _tool(
            "program_read",
            "read_only",
            "Read the current Composer Draft Program.",
            no_args,
        ),
        _tool(
            "program_validate",
            "read_only",
            "Validate the current Composer Draft Program without mutation.",
            no_args,
        ),
        _tool(
            "scene_inspect",
            "read_only",
            "Read the active logical-scene snapshot.",
            no_args,
        ),
        _tool(
            "skills_read",
            "read_only",
            "Read bounded metadata for active skill catalog entries.",
            _object(
                {
                    "skill_ids": _string_array(),
                    "include_bodies": {"type": "boolean"},
                }
            ),
        ),
        _tool(
            "status_read",
            "read_only",
            "Read the active headless runtime status snapshot.",
            no_args,
        ),
        _tool(
            "propose_scene_object_nodes",
            "proposal",
            "Build reviewable v2 scene-operation node proposals from scene resources.",
            _object(
                {
                    "scene_resources": _array(scene_resource, minimum=1),
                    "operations": _array(
                        {
                            "type": "string",
                            "enum": [
                                "scene.add_object",
                                "scene.attach_carried_object",
                                "scene.detach_carried_object",
                                "scene.remove_object",
                                "scene.upsert_object",
                            ],
                        },
                        minimum=1,
                        unique=True,
                    ),
                    "target_role": copy.deepcopy(IDENTIFIER),
                    "carrier_frame_role": copy.deepcopy(IDENTIFIER),
                    "touch_body_roles": _string_array(minimum=1),
                },
                required=("scene_resources", "operations"),
                all_of=[
                    {
                        "if": {
                            "properties": {
                                "operations": {"contains": {"const": "scene.attach_carried_object"}}
                            },
                            "required": ["operations"],
                        },
                        "then": {
                            "required": [
                                "target_role",
                                "carrier_frame_role",
                                "touch_body_roles",
                            ]
                        },
                    },
                    {
                        "if": {
                            "properties": {
                                "operations": {"contains": {"const": "scene.detach_carried_object"}}
                            },
                            "required": ["operations"],
                        },
                        "then": {"required": ["target_role"]},
                    },
                ],
            ),
        ),
        _tool(
            "propose_scene_objects",
            "proposal",
            "Convert normalized scene-inspection candidates into reviewable Draft resources.",
            _object(
                {
                    "inspection_report": _inspection_report_schema(catalog),
                    "source_roots": _array(
                        {"type": "string", "minLength": 1, "maxLength": 4096},
                        unique=True,
                    ),
                    "scene_resource_id_prefix": {"type": "string", "maxLength": 256},
                    "mesh_resource_overrides": _array(
                        _object(
                            {
                                "source_root": {
                                    "type": "string",
                                    "minLength": 1,
                                    "maxLength": 4096,
                                },
                                "mesh_resource_uri": {
                                    "type": "string",
                                    "minLength": 1,
                                    "maxLength": 4096,
                                },
                            },
                            required=("source_root", "mesh_resource_uri"),
                        ),
                        unique=True,
                    ),
                    "max_proposals": {
                        "type": "integer",
                        "minimum": 1,
                        "maximum": MAX_ITEMS,
                    },
                },
                required=("inspection_report",),
            ),
        ),
        _tool(
            "scene_draft_add_object",
            "composer_draft_mutating",
            "Add one detached logical scene resource to the in-memory Composer Draft.",
            add_scene,
        ),
        _tool(
            "scene_draft_remove_objects",
            "composer_draft_mutating",
            "Remove declared logical scene resources from the in-memory Composer Draft.",
            _object(
                {"scene_resource_ids": _string_array(minimum=1)},
                required=("scene_resource_ids",),
            ),
        ),
        _tool(
            "scene_draft_update_object",
            "composer_draft_mutating",
            "Replace one existing logical scene resource in the in-memory Composer Draft.",
            update_scene,
        ),
        _tool(
            "scene_draft_upsert_objects",
            "composer_draft_mutating",
            "Insert or replace logical scene resources in the in-memory Composer Draft.",
            _object(
                {"scene_resources": _array(authored_scene_resource, minimum=1)},
                required=("scene_resources",),
            ),
        ),
        _tool(
            "tree_draft_change_node_kind",
            "composer_draft_mutating",
            "Change one Draft node to an exact v2 kind while preserving its position and children.",
            _change_kind_schema(catalog),
        ),
        _tool(
            "tree_draft_delete_node",
            "composer_draft_mutating",
            "Delete one Draft node, detaching its direct children as retained roots.",
            _object(
                {"instance_id": copy.deepcopy(IDENTIFIER)},
                required=("instance_id",),
            ),
        ),
        _tool(
            "tree_draft_insert_node",
            "composer_draft_mutating",
            "Insert one exact v2 node or subtree into the in-memory Composer Draft.",
            _node_tool_schema(catalog, replace=False),
        ),
        _tool(
            "tree_draft_move_node",
            "composer_draft_mutating",
            "Move one existing non-root Draft subtree under another parent.",
            _object(
                {
                    "instance_id": copy.deepcopy(IDENTIFIER),
                    "new_parent_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
                    "index": {"type": "integer", "minimum": 0, "maximum": MAX_ITEMS},
                },
                required=("instance_id", "new_parent_instance_id"),
            ),
        ),
        _tool(
            "tree_draft_rename_node",
            "composer_draft_mutating",
            "Rename one Draft node's canonical instance identity atomically.",
            _object(
                {
                    "instance_id": copy.deepcopy(IDENTIFIER),
                    "new_instance_id": copy.deepcopy(IDENTIFIER),
                },
                required=("instance_id", "new_instance_id"),
            ),
        ),
        _tool(
            "tree_draft_replace_subtree",
            "composer_draft_mutating",
            "Atomically replace one Draft subtree with an exact v2 subtree.",
            _node_tool_schema(catalog, replace=True),
        ),
        _tool(
            "tree_draft_update_node_params",
            "composer_draft_mutating",
            "Update exact catalog parameters on one existing Draft node.",
            _update_node_schema(catalog),
        ),
        _tool(
            "tree_draft_wrap_node",
            "composer_draft_mutating",
            "Wrap one Draft node with an exact v2 parent that accepts one child.",
            _wrap_node_schema(catalog),
        ),
        _tool(
            "program_draft_remove_parameters",
            "composer_draft_mutating",
            "Remove Program parameter declarations from the in-memory Composer Draft.",
            _object(
                {"parameter_ids": _string_array(minimum=1)},
                required=("parameter_ids",),
            ),
        ),
        _tool(
            "program_draft_upsert_parameters",
            "composer_draft_mutating",
            "Insert or replace strict v1 Program parameter declarations in the Draft.",
            _object(
                {"parameters": _array(_program_parameter_schema(catalog), minimum=1)},
                required=("parameters",),
            ),
        ),
        _tool(
            "blackboard_draft_remove_keys",
            "composer_draft_mutating",
            "Remove blackboard key declarations from the in-memory Composer Draft.",
            _object(
                {"keys": _string_array(minimum=1)},
                required=("keys",),
            ),
        ),
        _tool(
            "blackboard_draft_remove_namespaces",
            "composer_draft_mutating",
            "Remove blackboard namespaces from the in-memory Composer Draft.",
            _object(
                {"namespace_ids": _string_array(minimum=1)},
                required=("namespace_ids",),
            ),
        ),
        _tool(
            "blackboard_draft_upsert_keys",
            "composer_draft_mutating",
            "Insert or replace strict v1 blackboard key declarations in the Draft.",
            _object(
                {"keys": _array(_blackboard_key_schema(catalog), minimum=1)},
                required=("keys",),
            ),
        ),
        _tool(
            "blackboard_draft_upsert_namespaces",
            "composer_draft_mutating",
            "Insert or replace strict v1 blackboard namespaces in the Draft.",
            _object(
                {
                    "namespaces": _array(
                        _object(
                            {
                                "namespace_id": copy.deepcopy(IDENTIFIER),
                                "owner": {"const": "program"},
                                "description": copy.deepcopy(BOUNDED_TEXT),
                            },
                            required=("namespace_id", "owner"),
                        ),
                        minimum=1,
                    ),
                },
                required=("namespaces",),
            ),
        ),
    ]
    tools.sort(key=lambda row: row["tool_id"])
    return tools


def _load_json(path: Path, label: str) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise GenerationError(f"{label} is not one plain file: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GenerationError(f"cannot read {label}: {error}") from error
    if type(value) is not dict:
        raise GenerationError(f"{label} root must be an object")
    return value


def _verify_profile(profile_path: Path, tool_ids: set[str]) -> None:
    profile = _load_json(profile_path, "maintained assistant profiles")
    try:
        modes = profile["mode_profiles"]["scene_authoring"]
        query = modes["query"]["catalog"]
        composer = modes["composer-assistant"]["catalog"]
    except (KeyError, TypeError) as error:
        raise GenerationError("maintained scene-authoring profile is incomplete") from error
    checks = (
        ("query.tools", query.get("tools"), list(QUERY_TOOL_IDS)),
        ("query.skills", query.get("skills"), []),
        ("query.nodes", query.get("nodes"), []),
        ("composer-assistant.tools", composer.get("tools"), list(COMPOSER_TOOL_IDS)),
        ("composer-assistant.skills", composer.get("skills"), []),
        ("composer-assistant.nodes", composer.get("nodes"), list(ASSISTANT_NODE_IDS)),
    )
    for label, actual, expected in checks:
        if actual != expected:
            raise GenerationError(
                f"maintained assistant {label} is stale: {actual!r} != {expected!r}"
            )
    unresolved = (set(QUERY_TOOL_IDS) | set(COMPOSER_TOOL_IDS)) - tool_ids
    if unresolved:
        raise GenerationError(f"maintained mode references unresolved tools: {sorted(unresolved)}")


def generate(catalog_path: Path, profile_path: Path) -> dict[str, Any]:
    catalog = NodeCatalog(catalog_path)
    missing_nodes = sorted(set(ASSISTANT_NODE_IDS) - {entry.id for entry in catalog.entries})
    if missing_nodes:
        raise GenerationError(f"assistant node translation is unresolved: {missing_nodes}")
    tools = _tools(catalog)
    tool_ids = [row["tool_id"] for row in tools]
    if tool_ids != sorted(set(tool_ids)):
        raise GenerationError("generated tool IDs are not unique and canonical-sorted")
    _verify_profile(profile_path, set(tool_ids))
    skills = load_skill_catalog(default_skills_dir(), catalog=catalog, tool_ids=set(tool_ids))
    return {
        "api_version": SOURCE_API_VERSION,
        "skills": [
            {
                "skill_id": row["skill_id"],
                "requires_tools": sorted(row.get("requires_tools", [])),
            }
            for row in skills.artifacts()
        ],
        "tools": tools,
        "nodes": [{"node_id": node_id} for node_id in ASSISTANT_NODE_IDS],
    }


def _render(value: dict[str, Any]) -> str:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--assistant-profiles", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        rendered = _render(generate(args.catalog.resolve(), args.assistant_profiles.resolve()))
        if args.check:
            current = args.output.resolve().read_text(encoding="utf-8")
            if current != rendered:
                raise GenerationError(f"assistant instrument source is stale: {args.output}")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
    except (GenerationError, OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
