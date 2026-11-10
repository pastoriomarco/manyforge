"""Assistant instruments built from the loaded node catalog.

The node catalog is the one source of what the assistant sees about Program
nodes. When Headless starts it builds, from the catalog it loaded:

* one descriptor per ``stability: stable`` node: a one-line summary (the
  first sentence of its description), its child bounds and its JSON Schemas
  (the bridge validates each node at call time against them);
* the tool descriptors: hand-defined descriptions and argument shapes; the
  tree tools take one generic node shape whose ``node_id`` is an enum of the
  authorable nodes.

The authorable node set is derived per active Deployment: the stable nodes
whose required capabilities the Deployment offers. Nothing here is stored;
``instrument_registry_digest`` is the digest of the generated content.
"""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
import hashlib
import json
import re
from typing import Any, Final, Iterable, Mapping

from manyforge_behavior import interrupt_condition
from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
from manyforge_behavior.catalog import CatalogEntry, NodeCatalog
from manyforge_behavior.catalog_schema import CatalogSchema

MODE_PROJECTION_VERSION: Final = "manyforge.assistant_mode_projection.v2"
MODE_CATALOG_VERSION: Final = "manyforge.assistant_mode_catalog.v1"
INDEX_VERSION: Final = "manyforge.assistant_mode_index.v2"
INSTRUMENTS_VERSION: Final = "manyforge.assistant_instruments.v1"
_CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1"
NODE_TOOL_IDS: Final = frozenset(
    {
        "tree_draft_change_node_kind",
        "tree_draft_insert_node",
        "tree_draft_replace_subtree",
        "tree_draft_update_node_params",
        "tree_draft_wrap_node",
    }
)
_SENTENCE_END: Final = re.compile(r"(?<=[.!?])\s")
# Parameter groups of which a node gives at most one (the node code requires
# exactly one at run time; a missing one is a completeness matter). The
# catalog notation has no parameter-level form for this; its entry
# descriptions state it.
_EXCLUSIVE_PARAMETERS: Final = {
    "scene.attach_carried_object": ("carrier_frame", "carrier_frame_ref"),
}


MAX_ITEMS = 4096
IDENTIFIER: dict[str, Any] = {
    "type": "string",
    "pattern": "^[a-z][a-z0-9_.-]*$",
    "maxLength": 256,
}
INSTANCE_OR_ROOT: dict[str, Any] = {"oneOf": [copy.deepcopy(IDENTIFIER), {"const": "@root"}]}
ROOT_CLAUSE = (
    "@root names the Program's current root node; to put a new node above it, wrap @root"
)
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


# An interrupt condition's schema is self-referential: its "#" names the
# condition. Embedded in a node schema, "#" would name the node, so each one
# moves to a root `$defs` entry (named for its catalog schema type) whose
# nested refs name that entry.
_CONDITION_SCHEMAS: Final = {
    name: _strip_descriptions(interrupt_condition.json_schema(direct_references=direct))
    for name, direct in (
        ("interrupt_condition", False),
        ("component_interrupt_condition", True),
    )
}


def _with_root_ref(value: Any, target: str) -> Any:
    if isinstance(value, dict):
        if value == {"$ref": "#"}:
            return {"$ref": target}
        return {key: _with_root_ref(item, target) for key, item in value.items()}
    if isinstance(value, list):
        return [_with_root_ref(item, target) for item in value]
    return value


def _hoist_conditions(schema: dict[str, Any]) -> dict[str, Any]:
    """`schema` with each embedded condition schema moved to root `$defs`."""

    definitions: dict[str, Any] = {}

    def visit(value: Any) -> Any:
        if isinstance(value, dict):
            for name, condition in _CONDITION_SCHEMAS.items():
                if value == condition:
                    target = f"#/$defs/{name}"
                    definitions[name] = _with_root_ref(condition, target)
                    return {"$ref": target}
            return {key: visit(item) for key, item in value.items()}
        if isinstance(value, list):
            return [visit(item) for item in value]
        return value

    result = visit(schema)
    if definitions:
        result["$defs"] = {**result.get("$defs", {}), **definitions}
    return result


def _factor_schema(schema: dict[str, Any]) -> dict[str, Any]:
    """Deduplicate repeated closed sub-schemas with local content refs.

    Motion nodes share large pose/dynamics/value-source shapes. Keeping each
    tool standalone is required, but repeating those shapes inside one tool is
    not. Local ``$defs`` preserve exact JSON-Schema meaning while materially
    reducing retained projections and model-request payloads.
    """

    schema = _hoist_conditions(schema)
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
        return schema
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


def _orientation_schema() -> dict[str, Any]:
    return _object(
        {
            "xyzw": _array({"type": "number"}, minimum=4, maximum=4),
            "rpy_deg": _array({"type": "number"}, minimum=3, maximum=3),
        },
        one_of=[{"required": ["xyzw"]}, {"required": ["rpy_deg"]}],
    )


def _initial_scene_pose_schema(catalog: NodeCatalog) -> dict[str, Any]:
    # Program v3: a component frame is a direct {component_id, frame_id}.
    frame_ref = {
        "oneOf": [
            _object({"kind": {"const": "cell_frame"}}, required=("kind",)),
            _object(
                {
                    "kind": {"const": "component_frame"},
                    "frame": _catalog_schema(
                        catalog, catalog.schema_registry.types["frame_reference"]
                    ),
                },
                required=("kind", "frame"),
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
    absolute = _initial_scene_pose_schema(catalog)
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
                else _initial_scene_pose_schema(catalog)
            ),
            "resource_roles": _string_array(minimum=1),
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
                    "description": copy.deepcopy(BOUNDED_TEXT),
                },
                required=("parameter_id", "type", "default"),
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


def _contact_policy_schema() -> dict[str, Any]:
    """A Program contact policy, manyforge.contact_policy.v1 (the qualified
    geometry-only grasp/release profile). Composer checks the rest (pair
    rules, canonical order) and computes its version."""

    selector = {
        "oneOf": [
            _object(
                {
                    "kind": {"const": "component_model_body"},
                    "body": _object(
                        {
                            "component_id": copy.deepcopy(IDENTIFIER),
                            "collision_body_id": {
                                "type": "string", "minLength": 1, "maxLength": 128,
                            },
                        },
                        required=("component_id", "collision_body_id"),
                    ),
                },
                required=("kind", "body"),
            ),
            _object({"kind": {"const": "motion_endpoint"}}, required=("kind",)),
            _object(
                {
                    "kind": {"const": "runtime_object"},
                    "runtime_object_id": copy.deepcopy(IDENTIFIER),
                },
                required=("kind", "runtime_object_id"),
            ),
        ]
    }
    pair = _object(
        {"body_a": copy.deepcopy(selector), "body_b": copy.deepcopy(selector)},
        required=("body_a", "body_b"),
    )
    common = {
        "zone_id": copy.deepcopy(IDENTIFIER),
        "body_pairs": _array(pair, minimum=1),
        "violation_response": {"const": "hard_stop"},
    }
    metric = {"type": "number", "minimum": 0, "maximum": 1000}
    zones = {
        "contact_grasp": _object(
            {
                **copy.deepcopy(common),
                "max_penetration_m": {"type": "number", "exclusiveMinimum": 0},
                "terminal_window_start_fraction": {
                    "type": "number", "minimum": 0, "maximum": 1,
                },
            },
            required=(
                "zone_id",
                "body_pairs",
                "violation_response",
                "max_penetration_m",
                "terminal_window_start_fraction",
            ),
        ),
        "contact_release": _object(
            {
                **copy.deepcopy(common),
                "release_evidence_mode": {
                    "type": "string", "enum": ["signed_penetration", "binary_contact"],
                },
                "max_start_penetration_m": copy.deepcopy(metric),
                "min_release_progress_m": copy.deepcopy(metric),
                "release_tolerance_m": copy.deepcopy(metric),
                "enforce_max_start_penetration": {"type": "boolean"},
                "require_strictly_decreasing_penetration": {"type": "boolean"},
                "require_separation_by_end": {"type": "boolean"},
                "allow_recontact": {"type": "boolean"},
            },
            required=("zone_id", "body_pairs", "violation_response", "release_evidence_mode"),
        ),
    }
    return {
        "oneOf": [
            _object(
                {
                    "api_version": {"const": "manyforge.contact_policy.v1"},
                    "policy_id": copy.deepcopy(IDENTIFIER),
                    "mode": {"const": mode},
                    "sensing_mode": {"const": "geometry_only"},
                    "default_unmatched_response": {"const": "hard_stop"},
                    "zones": _array(zone, minimum=1, maximum=1),
                },
                required=(
                    "api_version",
                    "policy_id",
                    "mode",
                    "sensing_mode",
                    "default_unmatched_response",
                    "zones",
                ),
            )
            for mode, zone in zones.items()
        ]
    }


_CONST_TYPES ={str: "string", bool: "boolean", int: "integer", float: "number", dict: "object", list: "array"}


def _implied_type(schema: dict[str, Any], definitions: dict[str, Any]) -> str | None:
    """The one JSON type every alternative of ``schema`` already requires, if any."""

    while "$ref" in schema:
        schema = definitions[schema["$ref"].removeprefix("#/$defs/")]
    if isinstance(schema.get("type"), str):
        return schema["type"]
    if "const" in schema:
        return _CONST_TYPES.get(type(schema["const"]))
    if "properties" in schema:
        return "object"
    alternatives = schema.get("oneOf") or schema.get("anyOf")
    if alternatives:
        types = {_implied_type(item, definitions) for item in alternatives}
        if len(types) == 1:
            return types.pop()
    return None


def _with_argument_types(schema: dict[str, Any]) -> dict[str, Any]:
    """State each top-level argument's JSON type where the schema already implies it.

    OpenAI-compatible servers parse tool-call arguments with the declared
    top-level ``type`` of each parameter (vLLM's ``qwen3_coder`` parser returns
    an undeclared nested object as a JSON string). Adding a type that every
    alternative already requires keeps the accepted values identical.
    """

    result = copy.deepcopy(schema)
    definitions = result.get("$defs", {})
    alternatives = result.get("oneOf")
    if "properties" not in result and alternatives:
        merged: dict[str, set[str | None]] = {}
        for alternative in alternatives:
            for name, value in alternative.get("properties", {}).items():
                merged.setdefault(name, set()).add(_implied_type(value, definitions))
        untyped = sorted(name for name, types in merged.items() if len(types) != 1 or None in types)
        if untyped:
            raise GenerationError(f"assistant tool arguments have no single JSON type: {untyped}")
        # Each alternative is closed over a subset of these names, so closing
        # the union keeps the accepted values identical.
        result["type"] = "object"
        result["properties"] = {name: {"type": types.pop()} for name, types in sorted(merged.items())}
        result["additionalProperties"] = False
    for name, value in result.get("properties", {}).items():
        implied = _implied_type(value, definitions)
        if "type" not in value and implied is not None:
            result["properties"][name] = {"type": implied, **value}
    return result




def framed_digest(domain: str, version: str, value: Any) -> str:
    """The ManyForge domain-separated content identity of ``value``."""

    return raw_sha256_digest(
        b"\0".join(
            (
                _CONTENT_IDENTITY_PREFIX,
                domain.encode("ascii"),
                version.encode("ascii"),
                canonical_json_bytes(value),
            )
        )
    )


def schema_hash(schema: Mapping[str, Any]) -> str:
    return raw_sha256_digest(canonical_json_bytes(schema))


def summary(description: str) -> str:
    """The first sentence of a catalog description."""

    return _SENTENCE_END.split(description.strip(), maxsplit=1)[0]


# One generated clause naming what value_sources is and its forms.
VALUE_SOURCES_CLAUSE: Final = (
    "value_sources (sibling of parameters) binds a parameter to a Program parameter or "
    "blackboard key: {<parameter>: {kind: "
    + "|".join(VALUE_SOURCE["properties"]["kind"]["enum"])
    + ", reference: <id>, field?}}"
)
MOTION_DEFAULTS_CLAUSE: Final = (
    "Omit velocity_scale, acceleration_scale, translation_tol_m and blend_radius_m "
    "to inherit the Program's motion defaults; set a literal or a source only to override"
)


def _generic_node(node_ids: list[str], *, instance: bool, children: bool) -> dict[str, Any]:
    properties: dict[str, Any] = {
        "node_id": {"type": "string", "enum": list(node_ids)},
        "parameters": {"type": "object"},
        "value_sources": {
            "type": "object",
            "description": "Optional. " + VALUE_SOURCES_CLAUSE + ".",
        },
    }
    required = ["node_id", "parameters"]
    if instance:
        properties["instance_id"] = copy.deepcopy(IDENTIFIER)
        required.append("instance_id")
    result = _object(properties, required=tuple(required))
    result["description"] = (
        "A Program node: node_id, its own instance_id, parameters of its catalog entry; "
        + VALUE_SOURCES_CLAUSE
        + ". "
        + MOTION_DEFAULTS_CLAUSE
        + "."
    )
    if children:
        # Optional; omitted means no children.
        result["properties"]["children"] = _array({"$ref": "#/$defs/node"})
        result = {**result, "$defs": {"node": copy.deepcopy(result)}}
    return result


def _typed(type_name: str) -> dict[str, Any]:
    """A top-level argument whose shape is the named type (catalog_read types)."""

    return {"type": "object", "description": f"A {type_name} (catalog_read types: {type_name})."}


def _typed_list(type_name: str) -> dict[str, Any]:
    return {
        "type": "array",
        "minItems": 1,
        "maxItems": MAX_ITEMS,
        "items": {"type": "object"},
        "description": f"A list of {type_name} (catalog_read types: {type_name}).",
    }


def _insert_schema(*, replace: bool) -> dict[str, Any]:
    payload_field = "subtree" if replace else "node"
    properties: dict[str, Any] = {payload_field: _typed("node")}
    one_of: list[dict[str, Any]] | None = None
    if replace:
        properties["instance_id"] = copy.deepcopy(INSTANCE_OR_ROOT)
        required: tuple[str, ...] = ("instance_id", payload_field)
    else:
        properties.update(
            {
                "parent_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
                "index": {"type": "integer", "minimum": 0, "maximum": MAX_ITEMS},
                "after_instance_id": copy.deepcopy(IDENTIFIER),
                "before_instance_id": copy.deepcopy(IDENTIFIER),
            }
        )
        required = (payload_field,)
        # index is meaningful only with an explicit parent.
        one_of = [
            {"required": ["parent_instance_id"], "properties": {}},
            {"required": ["after_instance_id"], "not": {"required": ["index"]}},
            {"required": ["before_instance_id"], "not": {"required": ["index"]}},
        ]
    return _object(properties, required=required, one_of=one_of)


def _update_schema(node_ids: list[str]) -> dict[str, Any]:
    return _object(
        {
            "instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "expected_node_id": {"type": "string", "enum": list(node_ids)},
            "parameters": {"type": "object"},
            "value_sources": {"type": "object"},
            "merge": {"type": "boolean"},
        },
        required=("instance_id", "expected_node_id"),
    )


def _change_kind_schema() -> dict[str, Any]:
    return _object(
        {
            "instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "replacement": _typed("replacement_node"),
            "parameters_mode": {"type": "string", "enum": ["replace", "merge", "strict"]},
        },
        required=("instance_id", "replacement", "parameters_mode"),
    )


def _wrap_schema() -> dict[str, Any]:
    return _object(
        {
            "target_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
            "wrapper": _typed("wrapper_node"),
        },
        required=("target_instance_id", "wrapper"),
    )


def _node_descriptor(catalog: NodeCatalog, entry: CatalogEntry) -> dict[str, Any]:
    """Summary, child bounds and the per-node schemas the bridge validates."""

    update = _node_configuration_schema(
        catalog, entry, include_instance=False, complete=False, include_children=False
    )
    update_properties = {"parameters": copy.deepcopy(update["properties"]["parameters"])}
    for parameter in entry.parameters:
        patch = _authoring_patch_schema(catalog, parameter.schema)
        if patch is not None:
            update_properties["parameters"]["properties"][parameter.id] = patch
    if "value_sources" in update["properties"]:
        update_properties["value_sources"] = update["properties"]["value_sources"]
    update_all_of = list(update.get("allOf", []))
    update_all_of.append(
        {"anyOf": [{"required": ["parameters"]}, {"required": ["value_sources"]}]}
    )
    # Shape and types only, like the Draft: a missing required parameter is
    # a Draft diagnostic (authoring_parameter_required), not a refusal.
    node = _node_configuration_schema(
        catalog, entry, include_instance=True, complete=False, include_children=False
    )
    group = _EXCLUSIVE_PARAMETERS.get(entry.id)
    if group is not None:
        node.setdefault("allOf", []).append(
            {
                "not": {
                    "allOf": [
                        {
                            "anyOf": [
                                _nested_presence("parameters", name),
                                _nested_presence("value_sources", name),
                            ]
                        }
                        for name in group
                    ]
                }
            }
        )
    return {
        "children": {"max": entry.children_max, "min": entry.children_min},
        "node_id": entry.id,
        "node_schema": _factor_schema(node),
        "replacement_schema": _factor_schema(update),
        "summary": summary(entry.description),
        "update_schema": _factor_schema(_object(update_properties, all_of=update_all_of)),
    }


@dataclass(frozen=True)
class _Tool:
    description: str
    schema: dict[str, Any] | None  # None: built per derived node set
    # Top-level arguments whose shape is a named type ("name" or "name[]").
    argument_types: Mapping[str, str] = field(default_factory=dict)


_STRINGS = {"type": "string", "minLength": 1, "maxLength": 4096}
# A tool offered only when the active Deployment offers this capability
# (like the nodes): the Isaac inspection needs a simulator.
_TOOL_REQUIREMENTS: Final = {"inspect_isaac_scene": "simulator.entity_state.set"}


def _tools() -> dict[str, _Tool]:
    no_args = _object({})
    return {
        "alarms_read": _Tool("Read the active alarm snapshot.", no_args),
        "catalog_read": _Tool(
            "Read node catalog entries (what a node does, when it completes or fails; "
            "with include_parameters its parameters and value types) and, with types, "
            "the shape of named argument and value types.",
            _object(
                {
                    "entry_ids": _string_array(),
                    "category": copy.deepcopy(IDENTIFIER),
                    "kind": {"type": "string", "enum": ["leaf", "decorator", "composite"]},
                    "include_parameters": {"type": "boolean"},
                    "types": _string_array(),
                }
            ),
        ),
        "deployment_capabilities_read": _Tool(
            "Read the capabilities of the active Deployment.", no_args
        ),
        "history_read": _Tool(
            "Read one page of the runtime history.",
            _object(
                {
                    "cursor": {"type": "string", "minLength": 1, "maxLength": 1024},
                    "limit": {"type": "integer", "minimum": 1, "maximum": 32},
                }
            ),
        ),
        "inspect_isaac_scene": _Tool(
            "Read the Isaac scene inspection report without changing it.",
            _object(
                {
                    "include_subtrees": _array(copy.deepcopy(_STRINGS), unique=True),
                    "exclude_subtrees": _array(copy.deepcopy(_STRINGS), unique=True),
                    "probe_prims": _array(copy.deepcopy(_STRINGS), unique=True),
                    "output_profile": {
                        "type": "string",
                        "enum": ["llm_scene_authoring", "raw_inventory"],
                    },
                    "mesh_export_dir": copy.deepcopy(_STRINGS),
                    "mesh_source_preference": {
                        "type": "string",
                        "enum": ["auto", "original", "collider", "primitive"],
                    },
                }
            ),
        ),
        "program_read": _Tool(
            "Read the Draft Program, or with instance_id one subtree of it.",
            _object({"instance_id": {
                **copy.deepcopy(IDENTIFIER),
                "description": "omit instance_id to read the whole Program",
            }}),
        ),
        "program_validate": _Tool("Validate the Draft Program without changing it.", no_args),
        "scene_inspect": _Tool(
            "Read the Draft scene: the scene resources you edit, in the shape the "
            "scene_draft tools take.",
            no_args,
        ),
        "skills_read": _Tool(
            "Read skill catalog entries.",
            _object({"skill_ids": _string_array(), "include_bodies": {"type": "boolean"}}),
        ),
        "status_read": _Tool("Read the active runtime status.", no_args),
        "scene_draft_add_object": _Tool(
            "Declare one scene resource in the Draft; a declared resource is present when "
            "every run starts. For a resource that appears during the run, also add Program "
            "nodes: scene.remove_object early, then scene.add_object, or scene.upsert_object.",
            _object({"scene_resource": _typed("scene_resource")}, required=("scene_resource",)),
            {"scene_resource": "scene_resource"},
        ),
        "scene_draft_remove_objects": _Tool(
            "Remove scene resources from the Draft.",
            _object(
                {"scene_resource_ids": _string_array(minimum=1)},
                required=("scene_resource_ids",),
            ),
        ),
        "scene_draft_update_object": _Tool(
            "Replace one existing scene resource in the Draft.",
            _object(
                {"scene_resource": _typed("scene_resource_edit")}, required=("scene_resource",)
            ),
            {"scene_resource": "scene_resource_edit"},
        ),
        "scene_draft_upsert_objects": _Tool(
            "Insert or replace scene resources in the Draft.",
            _object(
                {"scene_resources": _typed_list("scene_resource_edit")},
                required=("scene_resources",),
            ),
            {"scene_resources": "scene_resource_edit[]"},
        ),
        "tree_draft_change_node_kind": _Tool(
            "Change one Draft node to another listed node, keeping its position and "
            "children.",
            _change_kind_schema(),
            {"replacement": "replacement_node"},
        ),
        "tree_draft_delete_node": _Tool(
            "Delete one Draft node; its direct children become roots.",
            _object({"instance_id": copy.deepcopy(IDENTIFIER)}, required=("instance_id",)),
        ),
        "tree_draft_insert_node": _Tool(
            "Insert one node or subtree into the Draft. Place it with exactly one of "
            "parent_instance_id (optional index), after_instance_id or before_instance_id; "
            "every node, children included, needs its own instance_id. Build large "
            "structures step by step: an incomplete composite is allowed and shows a "
            "diagnostic until completed. " + ROOT_CLAUSE + ". " + VALUE_SOURCES_CLAUSE + ".",
            _insert_schema(replace=False),
            {"node": "node"},
        ),
        "tree_draft_move_node": _Tool(
            "Move one existing non-root Draft subtree under another parent. "
            + ROOT_CLAUSE + ".",
            _object(
                {
                    "instance_id": copy.deepcopy(IDENTIFIER),
                    "new_parent_instance_id": copy.deepcopy(INSTANCE_OR_ROOT),
                    "index": {
                        "type": "integer", "minimum": 0, "maximum": MAX_ITEMS,
                        "description": "0-based position among the new parent's children "
                        "after the move (the moved node not counted); omitted = last",
                    },
                },
                required=("instance_id", "new_parent_instance_id"),
            ),
        ),
        "tree_draft_rename_node": _Tool(
            "Rename one Draft node's instance_id.",
            _object(
                {
                    "instance_id": copy.deepcopy(IDENTIFIER),
                    "new_instance_id": copy.deepcopy(IDENTIFIER),
                },
                required=("instance_id", "new_instance_id"),
            ),
        ),
        "tree_draft_replace_subtree": _Tool(
            "Replace one Draft subtree with a new node or subtree; every node, children "
            "included, needs its own instance_id.",
            _insert_schema(replace=True),
            {"subtree": "node"},
        ),
        "tree_draft_update_node_params": _Tool(
            "Update parameters (merge keeps the others) or value_sources of one existing "
            "Draft node; " + VALUE_SOURCES_CLAUSE + ". To give one node its own value for "
            "a bound parameter, set it in parameters: with merge a literal replaces that "
            "parameter's value_sources entry on this node only.",
            None,
        ),
        "tree_draft_wrap_node": _Tool(
            "Wrap one Draft node in a new parent node that takes one child; the wrapper "
            "needs its own instance_id and no children. " + ROOT_CLAUSE + ".",
            _wrap_schema(),
            {"wrapper": "wrapper_node"},
        ),
        "program_draft_remove_parameters": _Tool(
            "Remove Program parameter declarations from the Draft.",
            _object({"parameter_ids": _string_array(minimum=1)}, required=("parameter_ids",)),
        ),
        "program_draft_upsert_parameters": _Tool(
            "Insert or replace Program parameter declarations in the Draft.",
            _object(
                {"parameters": _typed_list("parameter_declaration")},
                required=("parameters",),
            ),
            {"parameters": "parameter_declaration[]"},
        ),
        "program_draft_remove_contact_policies": _Tool(
            "Remove Program contact policies from the Draft.",
            _object(
                {"contact_policy_ids": _string_array(minimum=1)},
                required=("contact_policy_ids",),
            ),
        ),
        "program_draft_upsert_contact_policies": _Tool(
            "Insert or replace Program contact policies in the Draft (by policy_id). A node's "
            "contact_policy_id must name a declared policy: planning.plan_pick_sequence needs "
            "one with mode contact_grasp, so declare it first if program_read shows none. A "
            "contact_grasp policy has one zone with one body pair joining {kind: "
            "motion_endpoint} (the moving tool) or a component collision body to {kind: "
            "runtime_object, runtime_object_id: <key>}, where <key> is a declared "
            "blackboard key of type scene_resource (declare one with "
            "blackboard_draft_upsert_keys if needed), plus max_penetration_m (for example "
            "0.002) and terminal_window_start_fraction (0 to 1, for example 0.8). Composer "
            "computes the version.",
            _object(
                {"contact_policies": _typed_list("contact_policy")},
                required=("contact_policies",),
            ),
            {"contact_policies": "contact_policy[]"},
        ),
        "blackboard_draft_remove_keys": _Tool(
            "Remove blackboard key declarations from the Draft.",
            _object({"keys": _string_array(minimum=1)}, required=("keys",)),
        ),
        "blackboard_draft_remove_namespaces": _Tool(
            "Remove blackboard namespaces from the Draft.",
            _object({"namespace_ids": _string_array(minimum=1)}, required=("namespace_ids",)),
        ),
        "blackboard_draft_upsert_keys": _Tool(
            "Insert or replace blackboard key declarations in the Draft.",
            _object({"keys": _typed_list("blackboard_key")}, required=("keys",)),
            {"keys": "blackboard_key[]"},
        ),
        "blackboard_draft_upsert_namespaces": _Tool(
            "Insert or replace blackboard namespaces in the Draft.",
            _object(
                {"namespaces": _typed_list("blackboard_namespace")},
                required=("namespaces",),
            ),
            {"namespaces": "blackboard_namespace[]"},
        ),
    }


def _fixed_types(catalog: NodeCatalog) -> dict[str, dict[str, Any]]:
    """Named argument types, generated from the Draft's contract schemas."""

    return {
        "blackboard_key": _blackboard_key_schema(catalog),
        "blackboard_namespace": _object(
            {
                "namespace_id": copy.deepcopy(IDENTIFIER),
                "owner": {"const": "program"},
                "description": copy.deepcopy(BOUNDED_TEXT),
            },
            required=("namespace_id", "owner"),
        ),
        "contact_policy": _contact_policy_schema(),
        "parameter_declaration": _program_parameter_schema(catalog),
        "scene_resource": _scene_resource_schema(catalog),
        "scene_resource_edit": _scene_resource_schema(catalog, authoring_pose=True),
    }


def _node_types(nodes: list[str], wrappers: list[str]) -> dict[str, dict[str, Any]]:
    return {
        "node": _generic_node(nodes, instance=True, children=True),
        "replacement_node": _generic_node(nodes, instance=False, children=False),
        "wrapper_node": _generic_node(wrappers, instance=True, children=False),
    }


def _tool_schema(tool_id: str, tool: _Tool, nodes: list[str]) -> dict[str, Any]:
    schema = tool.schema if tool.schema is not None else _update_schema(nodes)
    return _factor_schema(_with_argument_types(schema))


class AssistantInstruments:
    """Everything the assistant sees about nodes, built once per catalog."""

    def __init__(self, catalog: NodeCatalog) -> None:
        self._entries = {
            entry.id: entry for entry in catalog.entries if entry.stability == "stable"
        }
        self._nodes = {
            node_id: _node_descriptor(catalog, entry)
            for node_id, entry in sorted(self._entries.items())
        }
        self._tools = _tools()
        self._types = {
            name: _factor_schema(schema) for name, schema in _fixed_types(catalog).items()
        }
        # Fixed tool schemas are built once; the update tool per node set.
        self._fixed = {
            tool_id: _tool_schema(tool_id, tool, [])
            for tool_id, tool in self._tools.items()
            if tool.schema is not None
        }
        everything = sorted(self._nodes)
        self.registry_digest = framed_digest(
            "assistant-instruments",
            INSTRUMENTS_VERSION,
            {
                "nodes": list(self._nodes.values()),
                "tools": [
                    self._tool_row(tool_id, "", everything) for tool_id in sorted(self._tools)
                ],
                "types": self._all_types(everything),
            },
        )

    def entries(self) -> Iterable[CatalogEntry]:
        """The stable catalog entries the derived node set is chosen from."""

        return self._entries.values()

    def _wrappers(self, nodes: list[str]) -> list[str]:
        return [
            node_id
            for node_id in nodes
            if self._nodes[node_id]["children"]["min"]
            <= 1
            <= self._nodes[node_id]["children"]["max"]
        ]

    def _all_types(self, nodes: list[str]) -> dict[str, Any]:
        types = dict(self._types)
        types.update(
            {
                name: _factor_schema(schema)
                for name, schema in _node_types(nodes, self._wrappers(nodes)).items()
            }
        )
        return dict(sorted(types.items()))

    def _tool_row(self, tool_id: str, effect: str, nodes: list[str]) -> dict[str, Any]:
        tool = self._tools[tool_id]
        schema = (
            self._fixed[tool_id] if tool.schema is not None else _tool_schema(tool_id, tool, nodes)
        )
        return {
            "argument_types": dict(sorted(tool.argument_types.items())),
            "description": tool.description,
            "effect": effect,
            "input_schema": schema,
            "input_schema_hash": schema_hash(schema),
            "tool_id": tool_id,
        }

    def compose_mode(
        self,
        native: Mapping[str, Any],
        node_ids: Iterable[str],
        offers: Iterable[str] | None = None,
    ) -> dict[str, Any]:
        """Join the catalog part into one native mode projection.

        ``native`` is the Headless core projection (tool identities and
        effects, its ``source`` envelope without ``source_content_digest``);
        ``node_ids`` is the derived node set of the active Deployment.
        """

        nodes = sorted(node_ids)
        projection = copy.deepcopy(dict(native))
        source = projection.pop("source")
        offered = None if offers is None else set(offers)
        projection["tools"] = [
            self._tool_row(row["tool_id"], row["effect"], nodes)
            for row in native["tools"]
            if offered is None
            or _TOOL_REQUIREMENTS.get(row["tool_id"]) is None
            or _TOOL_REQUIREMENTS[row["tool_id"]] in offered
        ]
        projection["nodes"] = [self._nodes[node_id] for node_id in nodes]
        # The full shapes of the tools' named argument types (bridge
        # validation and catalog_read types).
        projection["types"] = self._all_types(nodes)
        projection["instrument_registry_digest"] = self.registry_digest
        projection["catalog_hash"] = framed_digest(
            "assistant-mode-catalog",
            MODE_CATALOG_VERSION,
            {
                "grant_policy": projection["grant_policy"],
                "mode": projection["mode"],
                "nodes": projection["nodes"],
                "skills": projection["skills"],
                "tools": projection["tools"],
            },
        )
        source["source_content_digest"] = framed_digest(
            "manyforge-assistant-mode-projection-semantic",
            MODE_PROJECTION_VERSION,
            projection,
        )
        projection["source"] = source
        return projection

    def compose_index(self, native: Mapping[str, Any]) -> dict[str, Any]:
        index = copy.deepcopy(dict(native))
        source = index.pop("source")
        index["instrument_registry_digest"] = self.registry_digest
        source["source_content_digest"] = framed_digest(
            "manyforge-assistant-mode-index-semantic", INDEX_VERSION, index
        )
        index["source"] = source
        return index


__all__ = [
    "AssistantInstruments",
    "INDEX_VERSION",
    "MODE_CATALOG_VERSION",
    "MODE_PROJECTION_VERSION",
    "NODE_TOOL_IDS",
    "framed_digest",
    "schema_hash",
    "summary",
]
