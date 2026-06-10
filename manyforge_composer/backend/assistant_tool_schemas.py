"""JSON Schemas for built-in assistant bridge tools."""
from __future__ import annotations

import copy
import hashlib
import json
from typing import Any


def _strip_schema_descriptions(value: Any, *, in_properties_map: bool = False) -> Any:
    """Deep-copy with schema prose annotations removed.

    ``description`` is also a valid property name under JSON Schema
    ``properties``. Keep those field names structural while excluding only the
    annotation keyword from schema objects.
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


def _stable_schema_hash(schema: dict[str, Any]) -> str:
    # Structural hash only — prose descriptions excluded so a docs-only schema
    # edit does not invalidate in-flight bridge callbacks that gate on
    # inputSchemaHash (ADR-0023 §6).
    payload = json.dumps(
        _strip_schema_descriptions(schema), sort_keys=True, separators=(",", ":")
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


_ORIENTATION_FIELD_KEYS = {
    "orientation_xyzw",
    "orientation_rpy_deg",
    "rotate_local_rpy_deg",
    "rotate_world_rpy_deg",
    "rpy_delta_deg",
    "orientation_from",
}


def _allowed_orientation_fields(field_policy: dict[str, Any] | None) -> set[str] | None:
    if not field_policy:
        return None
    raw = field_policy.get("allowedOrientationFields")
    if raw is None:
        raw = field_policy.get("orientationFields")
    if raw is None:
        return None
    if not isinstance(raw, list):
        return set()
    return {
        item
        for item in raw
        if isinstance(item, str) and item in _ORIENTATION_FIELD_KEYS
    }


def _schema_depends_only_on_disallowed_orientation(
    schema: dict[str, Any],
    disallowed: set[str],
) -> bool:
    required = schema.get("required")
    if isinstance(required, list) and required:
        return all(item in disallowed for item in required if isinstance(item, str))
    return False


def _narrow_schema_for_field_policy(
    schema: dict[str, Any],
    field_policy: dict[str, Any] | None,
) -> dict[str, Any]:
    allowed_orientation = _allowed_orientation_fields(field_policy)
    if allowed_orientation is None:
        return copy.deepcopy(schema)
    disallowed_orientation = _ORIENTATION_FIELD_KEYS - allowed_orientation

    def narrow(value: Any) -> Any:
        if isinstance(value, list):
            narrowed_items = []
            for item in value:
                if (
                    isinstance(item, dict)
                    and _schema_depends_only_on_disallowed_orientation(
                        item,
                        disallowed_orientation,
                    )
                ):
                    continue
                narrowed_item = narrow(item)
                if (
                    isinstance(narrowed_item, dict)
                    and _schema_depends_only_on_disallowed_orientation(
                        narrowed_item,
                        disallowed_orientation,
                    )
                ):
                    continue
                narrowed_items.append(narrowed_item)
            return narrowed_items
        if not isinstance(value, dict):
            return copy.deepcopy(value)

        result: dict[str, Any] = {}
        for key, item in value.items():
            if key == "properties" and isinstance(item, dict):
                result[key] = {
                    prop_name: narrow(prop_schema)
                    for prop_name, prop_schema in item.items()
                    if prop_name not in disallowed_orientation
                }
                continue
            if key == "required" and isinstance(item, list):
                result[key] = [
                    required_name
                    for required_name in item
                    if required_name not in disallowed_orientation
                ]
                continue
            result[key] = narrow(item)
        return result

    return narrow(schema)


def _with_description(schema: dict[str, Any], description: str | None) -> dict[str, Any]:
    if description and "description" not in schema:
        schema = copy.deepcopy(schema)
        schema["description"] = description
    return schema


def _legacy_param_json_schema(param: Any) -> dict[str, Any]:
    param_type = getattr(param, "type", "dict")
    if param_type == "string":
        schema: dict[str, Any] = {"type": "string"}
    elif param_type == "float":
        schema = {"type": "number"}
    elif param_type == "int":
        schema = {"type": "integer"}
    elif param_type == "bool":
        schema = {"type": "boolean"}
    elif param_type == "enum":
        schema = {"type": "string", "enum": list(getattr(param, "enum_values", []) or [])}
    elif param_type == "float_list":
        schema = {"type": "array", "items": {"type": "number"}}
    elif param_type == "string_list":
        schema = {"type": "array", "items": {"type": "string"}}
    elif param_type == "pose":
        schema = {
            "type": "object",
            "additionalProperties": True,
            "properties": copy.deepcopy(_SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]),
        }
    elif param_type == "dict":
        schema = {"type": "object", "additionalProperties": True}
    else:
        schema = {}

    constraints = getattr(param, "constraints", {}) or {}
    if param_type in {"float", "int"} and isinstance(constraints, dict):
        if "min_inclusive" in constraints:
            schema["minimum"] = constraints["min_inclusive"]
        if "max_inclusive" in constraints:
            schema["maximum"] = constraints["max_inclusive"]
        if "min_exclusive" in constraints:
            schema["exclusiveMinimum"] = constraints["min_exclusive"]
        if "max_exclusive" in constraints:
            schema["exclusiveMaximum"] = constraints["max_exclusive"]
    return _with_description(schema, getattr(param, "description", ""))


def _key_ref_json_schema(param: Any) -> dict[str, Any]:
    expected_type = getattr(param, "blackboard_value_type", None)
    description = "Declared blackboard key reference."
    if expected_type:
        description += f" The referenced key must declare type `{expected_type}`."
    return {
        "type": "object",
        "additionalProperties": False,
        "required": ["key_ref"],
        "properties": {
            "key_ref": {
                "type": "string",
                "minLength": 1,
                "description": description,
            },
        },
    }


def _field_ref_json_schema(param: Any) -> dict[str, Any]:
    expected_type = getattr(param, "blackboard_value_type", None)
    expected_contract = getattr(param, "blackboard_contract", None)
    description = "Declared blackboard namespace field reference."
    if expected_type:
        description += f" The referenced field must declare type `{expected_type}`."
    if expected_contract:
        description += f" The namespace must declare contract `{expected_contract}`."
    return {
        "type": "object",
        "additionalProperties": False,
        "required": ["field_ref"],
        "properties": {
            "field_ref": {
                "type": "object",
                "additionalProperties": False,
                "required": ["namespace", "field"],
                "properties": {
                    "namespace": {
                        "type": "string",
                        "minLength": 1,
                        "description": "ID from program.blackboard.namespaces.",
                    },
                    "field": {
                        "type": "string",
                        "minLength": 1,
                        "description": "Field name declared by the namespace.",
                    },
                },
                "description": description,
            },
        },
    }


def _namespace_ref_json_schema(param: Any) -> dict[str, Any]:
    expected_contract = getattr(param, "blackboard_contract", None)
    description = "Declared blackboard namespace reference."
    if expected_contract:
        description += f" The namespace must declare contract `{expected_contract}`."
    return {
        "type": "object",
        "additionalProperties": False,
        "required": ["namespace_ref"],
        "properties": {
            "namespace_ref": {
                "type": "string",
                "minLength": 1,
                "description": description,
            },
        },
    }


def _with_blackboard_binding_forms(param: Any, literal_schema: dict[str, Any]) -> dict[str, Any]:
    role = getattr(param, "blackboard_role", None)
    if role in {"namespace_reader", "namespace_writer"}:
        return _namespace_ref_json_schema(param)
    if role == "key_writer":
        return _key_ref_json_schema(param)
    if role == "key_reader":
        return {
            "anyOf": [
                _key_ref_json_schema(param),
                _field_ref_json_schema(param),
            ],
        }
    if role == "literal_or_key_reader":
        return {
            "anyOf": [
                literal_schema,
                _key_ref_json_schema(param),
                _field_ref_json_schema(param),
            ],
        }
    if role == "field_reader":
        return {
            "anyOf": [
                literal_schema,
                _field_ref_json_schema(param),
            ],
        }
    return literal_schema


def _catalog_param_json_schema(param: Any, catalog: Any) -> dict[str, Any]:
    if getattr(param, "schema", None) is not None:
        schema = catalog.schema_registry.to_json_schema(param.schema)
        if getattr(param, "default", None) is not None:
            schema = copy.deepcopy(schema)
            schema["default"] = param.default
        schema = _with_description(schema, getattr(param, "description", ""))
        return _with_description(
            _with_blackboard_binding_forms(param, schema),
            getattr(param, "description", ""),
        )
    schema = _legacy_param_json_schema(param)
    if getattr(param, "default", None) is not None:
        schema = copy.deepcopy(schema)
        schema["default"] = param.default
    return _with_description(
        _with_blackboard_binding_forms(param, schema),
        getattr(param, "description", ""),
    )


def _catalog_entry_params_json_schema(entry: Any, catalog: Any) -> dict[str, Any]:
    properties: dict[str, Any] = {}
    required: list[str] = []
    for param in getattr(entry, "parameters", []):
        properties[param.name] = _catalog_param_json_schema(param, catalog)
        if getattr(param, "required", False) and getattr(param, "default", None) is None:
            required.append(param.name)

    schema: dict[str, Any] = {
        "type": "object",
        "additionalProperties": False,
        "properties": properties,
    }
    if required:
        schema["required"] = required
    return schema


def _catalog_entry_insert_node_schema(entry: Any, catalog: Any) -> dict[str, Any]:
    schema = copy.deepcopy(_TREE_NODE_PAYLOAD_FOR_INSERT_SCHEMA)
    schema["description"] = f"Insert payload for catalog node `{entry.id}`."
    schema["properties"]["id"] = {
        "type": "string",
        "enum": [entry.id],
        "description": f"Catalog identifier for {entry.id}.",
    }
    params_schema = _catalog_entry_params_json_schema(entry, catalog)
    schema["properties"]["params"] = params_schema
    if params_schema.get("required"):
        required = list(schema.get("required", []))
        if "params" not in required:
            required.append("params")
        schema["required"] = required
    return schema


def _catalog_entry_param_patch_schema(entry: Any, catalog: Any) -> dict[str, Any]:
    params_schema = _catalog_entry_params_json_schema(entry, catalog)
    patch_schema = copy.deepcopy(params_schema)
    patch_schema.pop("required", None)
    patch_schema["description"] = (
        f"Sparse parameter patch for an existing `{entry.id}` node. Include only "
        "parameters being changed; the callback validates the resolved target "
        "node before mutation."
    )
    return patch_schema


def _catalog_entries_for_projection(
    catalog: Any | None,
    *,
    node_ids: set[str] | None = None,
) -> list[Any]:
    if catalog is None:
        return []
    return [
        entry
        for entry in getattr(catalog, "entries", [])
        if node_ids is None or entry.id in node_ids
    ]


def _catalog_projected_insert_node_schema(
    catalog: Any | None,
    *,
    node_ids: set[str] | None = None,
) -> dict[str, Any]:
    schema = copy.deepcopy(_TREE_DRAFT_INSERT_NODE_SCHEMA)
    entries = _catalog_entries_for_projection(catalog, node_ids=node_ids)
    if not entries:
        return schema

    variants = [
        _catalog_entry_insert_node_schema(entry, catalog)
        for entry in sorted(entries, key=lambda candidate: candidate.id)
    ]
    schema["properties"]["node"] = {
        "anyOf": [*variants, copy.deepcopy(_TREE_NODE_SCHEMA_STRING_FORM)],
        "description": (
            "Concrete catalog-projected node payload. Prefer one of the "
            "structured variants keyed by `id`; the stringified JSON form "
            "is retained only as a compatibility fallback."
        ),
    }
    return schema


def _catalog_projected_update_node_params_schema(
    catalog: Any | None,
    *,
    node_ids: set[str] | None = None,
) -> dict[str, Any]:
    schema = copy.deepcopy(_TREE_DRAFT_UPDATE_NODE_PARAMS_SCHEMA)
    entries = _catalog_entries_for_projection(catalog, node_ids=node_ids)
    if not entries:
        return schema

    variants = [
        _catalog_entry_param_patch_schema(entry, catalog)
        for entry in sorted(entries, key=lambda candidate: candidate.id)
        if getattr(entry, "parameters", [])
    ]
    if not variants:
        return schema

    schema["properties"]["params"] = {
        "anyOf": variants,
        "description": (
            "Catalog-projected sparse parameter patch. Pick the variant matching "
            "the target node's catalog id; unknown parameter names are rejected "
            "by the callback after nodeName resolves to the concrete draft node."
        ),
    }
    return schema


def _catalog_schema_type_json_schema(
    catalog: Any | None,
    schema_type_name: str,
) -> dict[str, Any] | None:
    registry = getattr(catalog, "schema_registry", None)
    schema_types = getattr(registry, "types", None)
    if registry is None or not hasattr(schema_types, "get"):
        return None
    schema = schema_types.get(schema_type_name)
    if schema is None:
        return None
    return registry.to_json_schema(schema)


def _with_replaced_description(
    schema: dict[str, Any],
    description: str,
) -> dict[str, Any]:
    projected = copy.deepcopy(schema)
    projected["description"] = description
    return projected


def _property_schema(
    schema: dict[str, Any],
    property_name: str,
) -> dict[str, Any] | None:
    properties = schema.get("properties")
    if not isinstance(properties, dict):
        return None
    property_schema = properties.get(property_name)
    if not isinstance(property_schema, dict):
        return None
    return copy.deepcopy(property_schema)


def _projected_property_or_fallback(
    projected_schema: dict[str, Any],
    property_name: str,
    fallback_schema: dict[str, Any],
) -> dict[str, Any]:
    return _property_schema(projected_schema, property_name) or copy.deepcopy(fallback_schema)


def _scene_pose_schema_from_catalog(catalog: Any | None) -> dict[str, Any] | None:
    pose_schema = _catalog_schema_type_json_schema(catalog, "pose_authoring_input")
    if pose_schema is None:
        return None
    projected = copy.deepcopy(pose_schema)
    properties = projected.setdefault("properties", {})
    if isinstance(properties, dict):
        frame_schema = properties.get("frame_id")
        if isinstance(frame_schema, dict):
            frame_schema["enum"] = ["universe", "world", "World", "/World"]
            frame_schema["description"] = (
                "World-scene frame for a detached Composer draft object. "
                "The callback normalizes common world-frame aliases to the "
                "active backend's canonical scene frame. Use explicit "
                "attach_object_to_link / detach_object_from_link tree nodes "
                "for attachment semantics."
            )
        fallback_pose = _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]
        for field_name in _ORIENTATION_FIELD_KEYS:
            fallback_field = fallback_pose.get(field_name)
            target_field = properties.get(field_name)
            if isinstance(fallback_field, dict) and isinstance(target_field, dict):
                description = fallback_field.get("description")
                if isinstance(description, str) and description:
                    target_field.setdefault("description", description)
    return projected


def _scene_resource_schema_for_catalog(catalog: Any | None) -> dict[str, Any]:
    scene_resource = copy.deepcopy(_SCENE_RESOURCE_SCHEMA)
    shape_schema = _catalog_schema_type_json_schema(catalog, "shape")
    if shape_schema is not None:
        scene_resource["properties"]["shape"] = shape_schema
    pose_schema = _scene_pose_schema_from_catalog(catalog)
    if pose_schema is not None:
        scene_resource["properties"]["pose"] = pose_schema
    return scene_resource


def _catalog_projected_scene_draft_single_object_schema(
    catalog: Any | None,
) -> dict[str, Any]:
    if catalog is None:
        return copy.deepcopy(_SCENE_DRAFT_SINGLE_OBJECT_SCHEMA)

    scene_resource = _scene_resource_schema_for_catalog(catalog)
    shape_schema = scene_resource["properties"]["shape"]
    pose_schema = scene_resource["properties"]["pose"]
    fallback_shape = _SCENE_RESOURCE_SCHEMA["properties"]["shape"]
    fallback_pose = _SCENE_RESOURCE_SCHEMA["properties"]["pose"]
    fallback_shape_properties = fallback_shape["properties"]
    fallback_pose_properties = fallback_pose["properties"]

    schema = copy.deepcopy(_SCENE_DRAFT_SINGLE_OBJECT_SCHEMA)
    properties = schema["properties"]
    properties["shape"] = copy.deepcopy(shape_schema)
    properties["pose"] = copy.deepcopy(pose_schema)

    shape_type_schema = _projected_property_or_fallback(
        shape_schema,
        "type",
        properties["shapeType"],
    )
    properties["shapeType"] = _with_replaced_description(
        shape_type_schema,
        "Convenience alias for shape.type.",
    )
    properties["shape_type"] = _with_replaced_description(
        shape_type_schema,
        "Convenience alias for shape.type.",
    )

    box_dimensions_schema = _projected_property_or_fallback(
        shape_schema,
        "box_dimensions_m",
        fallback_shape_properties["box_dimensions_m"],
    )
    for alias in ("size", "dimensions", "boxDimensionsM", "box_dimensions_m"):
        properties[alias] = copy.deepcopy(box_dimensions_schema)

    for alias, field_name in (
        ("sphereRadiusM", "sphere_radius_m"),
        ("sphere_radius_m", "sphere_radius_m"),
        ("cylinderRadiusM", "cylinder_radius_m"),
        ("cylinder_radius_m", "cylinder_radius_m"),
        ("cylinderHeightM", "cylinder_height_m"),
        ("cylinder_height_m", "cylinder_height_m"),
        ("meshResourceUri", "mesh_resource_uri"),
        ("mesh_resource_uri", "mesh_resource_uri"),
        ("meshScale", "mesh_scale"),
        ("mesh_scale", "mesh_scale"),
    ):
        properties[alias] = _projected_property_or_fallback(
            shape_schema,
            field_name,
            fallback_shape_properties[field_name],
        )

    properties["frameId"] = _projected_property_or_fallback(
        pose_schema,
        "frame_id",
        fallback_pose_properties["frame_id"],
    )
    properties["position"] = _projected_property_or_fallback(
        pose_schema,
        "position_m",
        fallback_pose_properties["position_m"],
    )
    for field_name in ("frame_id", "position_m", *sorted(_ORIENTATION_FIELD_KEYS)):
        properties[field_name] = _projected_property_or_fallback(
            pose_schema,
            field_name,
            fallback_pose_properties[field_name],
        )

    properties["sceneResource"] = {
        **scene_resource,
        "description": "Legacy wrapper form accepted for compatibility.",
    }
    return schema


def _catalog_projected_scene_draft_upsert_objects_schema(
    catalog: Any | None,
) -> dict[str, Any]:
    if catalog is None:
        return copy.deepcopy(_SCENE_DRAFT_UPSERT_OBJECTS_SCHEMA)
    schema = copy.deepcopy(_SCENE_DRAFT_UPSERT_OBJECTS_SCHEMA)
    schema["properties"]["sceneResources"]["items"] = (
        _catalog_projected_scene_draft_single_object_schema(catalog)
    )
    return schema


_NO_ARGUMENT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}


_CATALOG_READ_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": (
        "Reads node-catalog entries that this assistant mode is permitted "
        "to use. Returns each entry's catalog identifier, its kind, the "
        "child arity it accepts, the full set of parameter declarations "
        "the entry expects, and any deployment capabilities required to "
        "use it. Use this when the per-request manifest's nodeCatalog "
        "summary needs to be expanded to the full per-parameter "
        "description, or when filtering by kind or category. The result "
        "is read-only metadata."
    ),
    "properties": {
        "entryIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "description": (
                "Optional list of catalog identifiers to return. Omit to "
                "receive every entry in the allowed-nodes set."
            ),
        },
        "category": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Optional category filter. Only catalog entries declared "
                "in the matching category are returned."
            ),
        },
        "kind": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Optional kind filter. Only catalog entries whose declared "
                "kind matches are returned."
            ),
        },
        "includeParameters": {
            "type": "boolean",
            "description": (
                "When true, the returned entries include the full "
                "per-parameter declaration. When false, only the "
                "structural fields are returned."
            ),
        },
    },
}


_SKILLS_READ_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": (
        "Reads skill-catalog entries that this assistant mode is "
        "permitted to use. Skills are higher-level, deployment-defined "
        "activities that expand into one or more behavior-tree nodes; "
        "each entry declares the deployment capabilities it requires "
        "and the tools needed to perform it. Use this when planning a "
        "request that may be implementable via a skill rather than by "
        "hand-assembling individual nodes. The result is read-only."
    ),
    "properties": {
        "skillIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "description": (
                "Optional list of skill identifiers to return. Omit to "
                "receive every entry in the allowed-skills set."
            ),
        },
        "includeBodies": {
            "type": "boolean",
            "description": (
                "When true, the returned entries include the body "
                "describing how each skill expands into behavior-tree "
                "nodes. When false, only the metadata is returned."
            ),
        },
    },
}


_SCENE_RESOURCE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["objectId", "shape", "pose"],
    "properties": {
        "objectId": {
            "type": "string",
            "minLength": 1,
            "description": "Stable scene resource ID in the Composer draft.",
        },
        "shape": {
            "type": "object",
            "additionalProperties": True,
            "required": ["type"],
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["box", "sphere", "cylinder", "mesh"],
                },
                "box_dimensions_m": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                },
                "sphere_radius_m": {"type": "number"},
                "cylinder_radius_m": {"type": "number"},
                "cylinder_height_m": {"type": "number"},
                "mesh_resource_uri": {"type": "string", "minLength": 1},
                "mesh_scale": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                },
            },
        },
        "pose": {
            "type": "object",
            "additionalProperties": True,
            "required": ["frame_id", "position_m"],
            "properties": {
                "frame_id": {
                    "type": "string",
                    "enum": ["universe", "world", "World", "/World"],
                    "description": (
                        "World-scene frame for a detached Composer draft object. "
                        "The callback normalizes common world-frame aliases to the "
                        "active backend's canonical scene frame. Use explicit "
                        "attach_object_to_link / detach_object_from_link tree nodes "
                        "for attachment semantics."
                    ),
                },
                "position_m": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                },
                "orientation_rpy_deg": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": (
                        "Orientation as roll/pitch/yaw in DEGREES. Alternatively "
                        "provide orientation_xyzw (quaternion) — e.g. copy the "
                        "orientation_xyzw shown for this object in scene state "
                        "verbatim; do not hand-convert a quaternion into degrees."
                    ),
                },
                "orientation_xyzw": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 4,
                    "maxItems": 4,
                    "description": (
                        "Orientation as a quaternion [x, y, z, w] — the same form "
                        "scene state reports. Accepted as an alternative to "
                        "orientation_rpy_deg; omit for identity."
                    ),
                },
                "rotate_local_rpy_deg": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": (
                        "RELATIVE rotation: rotate the object's CURRENT orientation "
                        "by this roll/pitch/yaw in DEGREES, expressed in the object's "
                        "own (local) axes. Use for requests like 'rotate it 15° about "
                        "X'. Only valid when the object already exists; supply exactly "
                        "one orientation form."
                    ),
                },
                "rotate_world_rpy_deg": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": (
                        "RELATIVE rotation expressed in the world/scene axes instead "
                        "of the object's local axes. Same rules as rotate_local_rpy_deg."
                    ),
                },
                "rpy_delta_deg": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": (
                        "RELATIVE adjustment by ADDING this roll/pitch/yaw delta (degrees) "
                        "component-wise to the object's current RPY angles (e.g. current "
                        "[0,11,-40] + [0,73,91] -> [0,84,51]). This is a per-axis angle "
                        "nudge, not a rigid-body rotation about the axes (use "
                        "rotate_local_rpy_deg for that). Only valid when the object exists; "
                        "supply exactly one orientation form."
                    ),
                },
                "orientation_from": {
                    "type": "object",
                    "additionalProperties": True,
                    "description": (
                        "REFERENCE: adopt orientation from another scene object, e.g. "
                        "{\"type\": \"object\", \"id\": \"graspable\"}. Use for 'face it "
                        "the same way as <object>'. Supply exactly one orientation form."
                    ),
                    "properties": {
                        "type": {"type": "string", "enum": ["object"]},
                        "id": {"type": "string"},
                    },
                },
            },
        },
    },
}


_SCENE_DRAFT_SINGLE_OBJECT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": True,
    "required": ["objectId"],
    "description": (
        "Payload describing a single detached Composer scene-draft "
        "object. The object identifier, the shape declaration, and the "
        "pose declaration are conveyed at the top level of the payload. "
        "The callback also accepts a number of compatibility aliases "
        "that mirror the canonical fields under alternate names; the "
        "canonical fields are preferred when both are available."
    ),
    # 2026-05-06 v8.1 probe: a JSON-Schema `examples` array previously
    # lived here with a worked payload using objectId "obstacle_01".
    # The MCP wrapper's null-arg validator passes the examples through
    # to the model on validation errors, AND the manifest exposes them
    # via inputSchema.examples on tools/list. The Nemotron-3-Nano-Omni
    # model in this regime (T=0.6, top_p=0.95, thinking off) takes the
    # literal value too seriously: on the "Add a small obstacle…" task
    # it called scene_draft_add_object 116 times in a single turn, all
    # with objectId="obstacle_01". The bridge upserts on duplicate ID,
    # every call returns ok, and the model has no signal it is done.
    # Removing the examples lets the model invent its own objectId,
    # which is what worked on the 2026-05-06 lane-parity probe (6
    # turns / 12.7 s). The MCP wrapper null-arg validator is the
    # load-bearing safety net; schema examples are net-negative for
    # this model regime. Re-add only if the failure mode shifts.
    "properties": {
        "objectId": _SCENE_RESOURCE_SCHEMA["properties"]["objectId"],
        "shape": _SCENE_RESOURCE_SCHEMA["properties"]["shape"],
        "pose": _SCENE_RESOURCE_SCHEMA["properties"]["pose"],
        "shapeType": {
            "type": "string",
            "enum": ["box", "sphere", "cylinder", "mesh"],
            "description": "Convenience alias for shape.type.",
        },
        "shape_type": {
            "type": "string",
            "enum": ["box", "sphere", "cylinder", "mesh"],
            "description": "Convenience alias for shape.type.",
        },
        "size": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 3,
            "maxItems": 3,
            "description": "Convenience alias for a box's [x, y, z] dimensions in meters.",
        },
        "dimensions": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 3,
            "maxItems": 3,
            "description": "Convenience alias for a box's [x, y, z] dimensions in meters.",
        },
        "boxDimensionsM": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 3,
            "maxItems": 3,
            "description": "Box dimensions [x, y, z] in meters.",
        },
        "box_dimensions_m": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["box_dimensions_m"],
        "radius": {
            "type": "number",
            "description": "Convenience alias for sphere radius, or cylinder radius when shapeType is cylinder.",
        },
        "diameter": {
            "type": "number",
            "description": "Convenience alias for sphere diameter, or cylinder diameter when shapeType is cylinder.",
        },
        "sphereRadiusM": {
            "type": "number",
            "description": "Sphere radius in meters.",
        },
        "sphere_radius_m": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["sphere_radius_m"],
        "cylinderRadiusM": {
            "type": "number",
            "description": "Cylinder radius in meters.",
        },
        "cylinder_radius_m": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["cylinder_radius_m"],
        "cylinderHeightM": {
            "type": "number",
            "description": "Cylinder height in meters along its local Z axis.",
        },
        "cylinder_height_m": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["cylinder_height_m"],
        "meshResourceUri": {
            "type": "string",
            "minLength": 1,
            "description": "Mesh resource URI or path.",
        },
        "mesh_resource_uri": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["mesh_resource_uri"],
        "meshScale": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 3,
            "maxItems": 3,
            "description": "Mesh scale [x, y, z].",
        },
        "mesh_scale": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["mesh_scale"],
        "position": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 3,
            "maxItems": 3,
            "description": "Convenience alias for pose.position_m.",
        },
        "position_m": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["position_m"],
        "frameId": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["frame_id"],
        "frame_id": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["frame_id"],
        "orientation_xyzw": {
            "type": "array",
            "items": {"type": "number"},
            "minItems": 4,
            "maxItems": 4,
            "description": "Object orientation as [x, y, z, w]. Omit for identity.",
        },
        "orientation_rpy_deg": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["orientation_rpy_deg"],
        "rotate_local_rpy_deg": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["rotate_local_rpy_deg"],
        "rotate_world_rpy_deg": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["rotate_world_rpy_deg"],
        "rpy_delta_deg": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["rpy_delta_deg"],
        "orientation_from": _SCENE_RESOURCE_SCHEMA["properties"]["pose"]["properties"]["orientation_from"],
        "sceneResource": {
            **_SCENE_RESOURCE_SCHEMA,
            "description": "Legacy wrapper form accepted for compatibility.",
        },
        "forceDiscardOverrides": {
            "type": "boolean",
            "description": "Discard unsaved scene-resource overrides with the same objectId.",
        },
    },
}


_SCENE_DRAFT_UPSERT_OBJECTS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["sceneResources"],
    "description": (
        "Inserts new static scene resources or replaces existing ones in "
        "a single batched call. The semantics match the single-object add "
        "and update tools: compile-time configuration, not per-cycle "
        "mutation. Useful when a request introduces multiple permanent "
        "objects at once. Duplicate identifiers within a single batch "
        "are rejected; the caller must assign unique identifiers across "
        "the batch."
    ),
    "properties": {
        "sceneResources": {
            "type": "array",
            "items": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
            "minItems": 1,
            "description": (
                "List of scene-resource payloads to insert or replace. "
                "Each payload follows the single-object scene-draft "
                "schema. Identifiers must be unique within the list."
            ),
        },
        "forceDiscardOverrides": {
            "type": "boolean",
            "description": (
                "When true, any unsaved scene-resource overrides with "
                "identifiers matching this batch are discarded before "
                "the upsert is applied."
            ),
        },
    },
}


_SCENE_DRAFT_REMOVE_OBJECTS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["objectIds"],
    "description": (
        "Removes a batch of static scene resources from the program's "
        "scene-resource list. The resources cease to exist for any "
        "subsequent cycle iteration. Do not use this tool when a request "
        "describes removing an object only at a specific moment during "
        "execution; that case calls for a runtime-mutation behavior-tree "
        "node, inserted via the tree-draft tools instead."
    ),
    "properties": {
        "objectIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
            "description": (
                "List of identifiers naming scene resources that must "
                "already exist in the draft. Identifiers absent from the "
                "draft are rejected."
            ),
        },
        "forceDiscardOverrides": {
            "type": "boolean",
            "description": (
                "When true, any unsaved scene-resource overrides with "
                "identifiers matching this batch are discarded before "
                "the removal is applied."
            ),
        },
    },
}


# 2026-05-06: modificationPolicy / recoveryPolicy intentionally OMITTED
# from these node schemas. The fields are advanced metadata
# (governance / blackboard / runtime-override knobs) that small models
# cannot reliably produce — they default to placeholder shapes like
# {"type": "string"} or {"type": "replace"} that pass JSON-Schema
# additionalProperties:True validation but get rejected deeper in
# program-validation as "not a recognized policy field". On the
# 2026-05-06 v8.1 probe of "remove graspable at end of cycle and
# re-add", the model emitted {"type": "replace"} on every
# tree-draft-insert_node call and looped 130x on the same broken
# payload before the bridge timed out.
#
# Same precedent applied to _TREE_NODE_WRAPPER_SCHEMA earlier; this
# is the matching prevention for insert_node / replace_subtree.
# Removing the fields from the schema means the LLM no longer sees
# them advertised on tools/list, so it stops emitting them. Real
# callers that genuinely need the policy fields can still send them
# via the broader assistant tool catalog (the deployment's
# bridge/tools endpoint accepts them) — they're just not part of the
# narrowed assistant-mode surface the LLM sees.
_TREE_NODE_CHILD_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": True,
    "required": ["id", "name"],
    "properties": {
        "id": {"type": "string", "minLength": 1},
        "name": {"type": "string", "minLength": 1},
        "params": {"type": "object", "additionalProperties": True},
        "children": {"type": "array", "items": {"type": "object", "additionalProperties": True}},
    },
}


# 2026-05-06: nested-payload accept-both-forms schema. The model
# (Nemotron-3-Nano-Omni T=0.6 top_p=0.95) sometimes emits `node` (or
# `subtree`) as a JSON-encoded STRING instead of an inline object —
# even though the schema clearly types it as object. We saw this
# fail the user's GUI prompt with "node: must be object" and the
# model stopped without retrying. Solution: accept either an inline
# object (preferred) OR a stringified JSON; the MCP wrapper /
# backend normalizes the string form back to an object before
# forwarding. Defensive precedent: same logic as _strip_policy_stubs.
_TREE_NODE_SCHEMA_STRING_FORM: dict[str, Any] = {
    "type": "string",
    "minLength": 2,
    "description": (
        "Stringified JSON form of the node object (DISCOURAGED — "
        "prefer the inline object form). Accepted as a fallback "
        "because models sometimes serialize the object as a JSON "
        "string. The bridge parses it back to an object before "
        "forwarding. Must be a single JSON object, not an array."
    ),
}


_TREE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id", "name"],
    "properties": {
        "id": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Catalog identifier of the kind of node being inserted. "
                "Must be one of the identifiers declared in the deployment's "
                "node catalog and present in the allowed-nodes set for this "
                "assistant mode; consult the per-request manifest's nodeCatalog "
                "for the authoritative list and for each entry's kind, accepted "
                "child arity, and parameter declarations. The catalog identifier "
                "is distinct from this node's instance name and must not be a "
                "descriptive label, a runtime role, or a textual variant of an "
                "existing identifier — variants that merely resemble a real "
                "catalog identifier are rejected."
            ),
        },
        "name": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Caller-chosen instance name for this node in the program "
                "tree. The instance name is a free-form descriptive label "
                "and must be unique across the entire tree. The instance "
                "name is distinct from the catalog identifier and is not "
                "looked up in the catalog. Collision with an existing "
                "instance name is rejected with a structural-error response; "
                "on retry, choose a different label."
            ),
        },
        "params": {
            "type": "object",
            "additionalProperties": True,
            "description": (
                "Parameter values for this node, keyed by parameter name. "
                "The set of accepted parameter names, the type each accepts, "
                "and which are required, are declared on the catalog entry "
                "for this node's identifier and must be looked up there "
                "rather than guessed. The catalog entry is also the source "
                "of truth on whether a parameter is nested inside another "
                "parameter or is a sibling at the top level of params; do "
                "not infer nesting from a parameter's name. The catalog "
                "identifier itself is not a parameter and must not appear "
                "as a key here."
            ),
        },
        "children": {
            "type": "array",
            "items": _TREE_NODE_CHILD_SCHEMA,
        },
    },
}


# Insert-specific payload: same as _TREE_NODE_SCHEMA but `name` is moved
# OUT of the payload and up to the tool's top-level `nodeName` argument.
# This matches the rest of the tree_draft_* family (update_node_params,
# delete_node, move_node, change_node_kind all take `nodeName` at
# top-level) and removes the duplication that was confusing the model
# (it kept setting both top-level nodeName AND node.name).
_TREE_NODE_PAYLOAD_FOR_INSERT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id"],
    "properties": {
        "id": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Catalog identifier of the kind of node being inserted. "
                "Must be one of the identifiers declared in the deployment's "
                "node catalog and present in the allowed-nodes set for this "
                "assistant mode; consult the per-request manifest's nodeCatalog "
                "for the authoritative list and for each entry's kind, accepted "
                "child arity, and parameter declarations. The catalog identifier "
                "is distinct from this node's instance name (which is passed "
                "as the top-level `nodeName` argument) and must not be a "
                "descriptive label, a runtime role, or a textual variant of "
                "an existing identifier."
            ),
        },
        # 2026-06-01 (validator-loop fix): accept-and-ignore `name`.
        # Many models duplicate the top-level `nodeName` argument inside
        # `node.name` (mirroring the older `_TREE_NODE_SCHEMA` shape used
        # by replace_subtree/wrap_node). Previously `additionalProperties:
        # false` rejected this, producing the misleading "node: must not
        # have additional properties / node: must be string" error that
        # sent models into a death spiral (object → string → object).
        # The handler reads `nodeName` from the top level; `node.name` is
        # accepted but not used. If models pass mismatched values, the
        # top-level `nodeName` wins.
        "name": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Optional duplicate of the top-level `nodeName` argument. "
                "Accepted for compatibility with payloads that mirror the "
                "replace_subtree/wrap_node shape; the handler ignores this "
                "field and uses the top-level `nodeName` instead. Prefer "
                "omitting this and passing the instance name only at the "
                "top level."
            ),
        },
        "params": {
            "type": "object",
            "additionalProperties": True,
            "description": (
                "Parameter values for this node, keyed by parameter name. "
                "The set of accepted parameter names, the type each accepts, "
                "and which are required, are declared on the catalog entry "
                "for this node's identifier and must be looked up there "
                "rather than guessed. The catalog entry is the source of "
                "truth on whether a parameter is nested inside another "
                "parameter or is a sibling at the top level of params."
            ),
        },
        "children": {
            "type": "array",
            "items": _TREE_NODE_CHILD_SCHEMA,
        },
    },
}


_TREE_DRAFT_INSERT_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "node"],
    "description": (
        "Inserts a new node into the program's behavior-tree draft. This "
        "tool only authors runtime behavior — nodes that the program will "
        "execute during a cycle. It is NEVER the right tool for adding a "
        "static obstacle, prop, or fixture that should exist for the "
        "entire program run: those are scene resources and are authored "
        "with scene_draft_add_object (or its upsert/remove siblings). "
        "Inserting an `add_collision_object` (or analogous) BT node "
        "under a sequence creates a runtime mutation that fires every "
        "time that branch executes, which is almost never what the "
        "operator wanted when they asked for a static object. "
        "The new node's instance name goes in `nodeName` (top-level, "
        "matching the convention of other tree_draft_* tools). The "
        "node's catalog kind and parameters go in `node` (which carries "
        "`id` and `params`). The parent under which to insert is "
        "specified by exactly ONE of: `parentName` (with optional "
        "0-based `index`; omit `index` to append at the end), OR "
        "`afterName`, OR `beforeName`. Use `afterName`/`beforeName` "
        "for relative positioning (\"after X\", \"before X\") — they "
        "resolve the parent and index from the tree automatically. "
        "Unknown names are rejected with a structural-error response "
        "that lists the legal candidates. "
        "The new node's catalog identifier and parameters are taken from "
        "the per-request manifest's node-catalog entries; every accepted "
        "identifier, every accepted parameter name, every required "
        "parameter, and every parameter's accepted type is declared in "
        "the catalog and must be looked up there rather than guessed. "
        "The parent's declared arity is the binding constraint on "
        "insertion; an arity-saturated parent is rejected with a "
        "structural-error response that names the parent and reports its "
        "allowed arity. Pose and shape payloads must follow the concrete "
        "schema projected for the selected catalog node."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name for the new node being inserted. Free-form "
                "descriptive label; must be unique across the entire tree."
            ),
        },
        "node": {
            "anyOf": [_TREE_NODE_PAYLOAD_FOR_INSERT_SCHEMA, _TREE_NODE_SCHEMA_STRING_FORM],
            "description": (
                "Payload for the new node carrying its catalog `id` and "
                "`params` (and optional `children`). Do NOT include `name` "
                "here — the instance name is passed as the top-level "
                "`nodeName` argument. Structured object preferred; a "
                "JSON-encoded string is also accepted."
            ),
        },
        "parentName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the parent that should receive the new "
                "node as a child. Mutually exclusive with afterName and "
                "beforeName."
            ),
        },
        "index": {
            "type": "integer",
            "minimum": 0,
            "description": (
                "0-based position within the parent's child list. "
                "Requires parentName; omit to append at the end."
            ),
        },
        "afterName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Insert immediately AFTER this named sibling. Use for "
                "\"after X\" / \"following X\" prompts. Mutually "
                "exclusive with parentName, index, beforeName."
            ),
        },
        "beforeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Insert immediately BEFORE this named sibling. Use for "
                "\"before X\" prompts. Mutually exclusive with "
                "parentName, index, afterName."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_UPDATE_NODE_PARAMS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "params"],
    "description": (
        "Changes the parameter values on a node already present in the "
        "draft tree, without altering the tree's structure. The node is "
        "identified by its instance name. The parameter payload must "
        "satisfy the catalog entry's parameter declarations; sending a "
        "parameter outside that declaration, or omitting a required "
        "parameter while in non-merge mode, is rejected with a "
        "structural-error response that lists the parameters actually "
        "sent and the parameters expected by the catalog. Choose the "
        "merge option when the request only adjusts a subset of values "
        "and the remaining values should be preserved."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node whose parameters are to be "
                "modified. Must match a node already present in the draft."
            ),
        },
        "params": {
            "type": "object",
            "additionalProperties": True,
            "description": (
                "Parameter values to apply, keyed by parameter name. The "
                "set of accepted parameter names and the type each accepts "
                "are declared on the catalog entry for the target node's "
                "identifier and must be looked up there rather than guessed."
            ),
        },
        "merge": {
            "type": "boolean",
            "description": (
                "When true, parameters not present in this payload are "
                "preserved at their current values. When false, the "
                "payload fully replaces the node's parameter set, and any "
                "required parameter not present in the payload is rejected."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_DELETE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName"],
    "description": (
        "Removes a non-root node and the entire subtree rooted at it from "
        "the draft. The node is identified by its instance name. Removing "
        "the root is not permitted; if the request implies the entire "
        "program should be cleared, that is a different operation outside "
        "this tool. Deletion is also rejected if it would leave the "
        "remaining parent below its declared minimum child count; in that "
        "case the caller must move the node's subtree somewhere else "
        "first, or restructure the parent itself, before the deletion "
        "becomes legal."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node to remove. The node and its "
                "entire subtree are deleted in one operation."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_MOVE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "newParentName"],
    "description": (
        "Moves an existing node and its subtree to a new position in the "
        "tree. Both the node and the destination parent are identified "
        "by their instance names. The node can move under a different "
        "parent, or stay under the same parent at a different index. "
        "The destination parent's catalog-declared arity must accept "
        "the additional child; the move is rejected with a structural-"
        "error response otherwise. Use this tool when a request "
        "describes reorganizing existing behavior rather than "
        "introducing new behavior. "
        "This is also the right tool when the user says \"swap the "
        "order\", \"swap A with B\", \"reorder A and B\", or any other "
        "phrasing that implies repositioning two siblings: pass the "
        "name of one of them and the index where it should land. "
        "It is NOT `tree_draft_change_node_kind` (formerly named "
        "tree_draft_swap_node) — that tool changes a node's catalog "
        "kind in place, not its position."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node to move. The node's subtree "
                "moves with it as a single unit."
            ),
        },
        "newParentName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the parent that will receive the moved "
                "node. The parent's arity must accept the additional child."
            ),
        },
        "index": {
            "type": "integer",
            "minimum": 0,
            "description": (
                "Optional position in the new parent's child list at which "
                "to insert the moved node. Omit to append at the end."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_REPLACE_SUBTREE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "subtree"],
    "description": (
        "Atomically replaces a node and its entire subtree with a new "
        "subtree in the draft. The target is identified by its instance "
        "name and must already exist in the draft tree; if the tree's "
        "node names are not already known, call the program-read tool "
        "first rather than guess. Use this tool for restructurings that "
        "would otherwise require a long sequence of insert, wrap, move, "
        "and delete operations — when the change is large enough that a "
        "transaction-style replacement is more reliable than incremental "
        "edits. The new subtree must be a valid program tree on its own "
        "under this assistant mode's allowed catalog: the root's catalog "
        "kind determines the root's arity, every nested node identifier "
        "must be in the allowed-nodes set for this mode, and instance "
        "names within the new subtree must not collide with names "
        "already present in the rest of the tree. The subtree payload "
        "may be passed either as a structured object (preferred) or as "
        "a JSON-encoded string fallback. "
        "Prefer a more surgical primitive whenever it fits: for adding "
        "a parent above an existing node while preserving its subtree "
        "intact, use wrap_node. For changing a node's catalog id while "
        "keeping its name, parent, and children intact, use swap_node. "
        "For changing only the parameter values on a node, use "
        "update_node_params. Reach for replace_subtree only when the "
        "new subtree's leaves and structure genuinely differ from what "
        "is already in place; if it would re-emit any leaf already in "
        "the draft tree, one of the surgical primitives is better."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node whose subtree is to be replaced "
                "in full. The node and everything below it are removed and "
                "the new subtree takes their position."
            ),
        },
        "subtree": {
            "anyOf": [_TREE_NODE_SCHEMA, _TREE_NODE_SCHEMA_STRING_FORM],
            "description": (
                "Payload describing the replacement subtree, including its "
                "root and every descendant. The structured-object form is "
                "preferred; a JSON-encoded string is accepted as a "
                "fallback and normalized to an object before validation."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


# wrap_node's wrapper deliberately does NOT accept `children` — the handler
# inserts the target node's existing subtree as the only child. Accepting
# children here would create ambiguity (does the model want them merged with
# the target? replaced by the target? prepended/appended?). Reject upfront.
_TREE_NODE_WRAPPER_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id", "name"],
    "properties": {
        "id": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Catalog identifier of the wrapping node. Must be one of "
                "the identifiers declared in the deployment's node catalog "
                "and present in the allowed-nodes set for this assistant "
                "mode, and its kind must be one that accepts at least one "
                "child (a control-flow or decorator entry). Consult the "
                "per-request manifest's nodeCatalog for the authoritative "
                "list. The catalog identifier is distinct from this node's "
                "instance name and must not be a descriptive label or a "
                "textual variant that merely resembles a real catalog "
                "identifier."
            ),
        },
        "name": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Caller-chosen instance name for the wrapping node in the "
                "program tree. The instance name is a free-form descriptive "
                "label and must be unique across the entire tree."
            ),
        },
        "params": {"type": "object", "additionalProperties": True},
        # NOTE: modificationPolicy / recoveryPolicy are intentionally omitted
        # from the wrapper schema. Wrappers (decorators / control-flow) don't
        # use those fields, and small models reliably emit invalid placeholder
        # values like {"type": "string"} that fail program validation. Field
        # absence here keeps schema-aware callers from including them.
    },
}


_TREE_DRAFT_CHANGE_NODE_KIND_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "newId"],
    "description": (
        "Changes the catalog identifier (the kind) of an existing node "
        "in place while preserving the node's instance name, its "
        "parent, and its entire subtree. Use this tool when a request "
        "describes switching one node kind for a structurally "
        "compatible one — for example, replacing a decorator with a "
        "different decorator of the same arity, substituting one "
        "composite for a sibling composite of the same arity, or "
        "substituting one scene-create leaf for a related scene-create "
        "leaf — without rewriting any of the surrounding tree. The "
        "instance name is preserved unconditionally so other parts of "
        "the program that reference this node by name (parameter "
        "references, blackboard bindings) keep working. "
        "This tool does NOT reorder siblings. To reorder two named "
        "nodes (e.g. \"swap the order of A and B\", \"swap A with B\", "
        "\"reorder A and B\"), use `tree_draft_move_node` to relocate "
        "one of them to the desired index inside the same parent. "
        "The new catalog identifier must be in the deployment catalog "
        "and in this assistant mode's allowed-nodes set. The current "
        "child count must satisfy the new identifier's arity range; if "
        "it does not, the call is rejected with a structural-error "
        "response and the caller must adjust the children before "
        "retrying. The merged parameters (the existing node's params "
        "overlaid by whatever the caller supplies under params) must "
        "satisfy the new identifier's parameter declarations; the "
        "paramsMode option controls whether the caller's payload "
        "replaces the existing params, merges into them, or strictly "
        "rejects when the existing params are not all accepted. "
        "Catalog entries may declare a swap_class field naming a "
        "compatibility family; when both the current and new entries "
        "carry such a field and the families differ, the call is "
        "rejected unless force is set to true. Entries without a "
        "declared family are validated structurally only. "
        "Prefer this tool over replace_subtree whenever the goal is to "
        "change a node's kind while keeping its position and children "
        "intact — replace_subtree would force the caller to re-emit "
        "the entire subtree from scratch, including every leaf that "
        "this assistant mode might not even be allowed to author. "
        "Prefer wrap_node when the goal is to add a new parent above "
        "an existing node rather than to change the existing node's "
        "kind."
    ),
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node whose catalog identifier is "
                "to be changed. Must match a node already present in "
                "the draft tree. The instance name itself is preserved."
            ),
        },
        "newId": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Catalog identifier of the new kind. Must be one of the "
                "identifiers declared in the deployment's node catalog "
                "and present in the allowed-nodes set for this "
                "assistant mode. Look this up in the per-request "
                "manifest's nodeCatalog rather than guess."
            ),
        },
        "params": {
            "type": "object",
            "additionalProperties": True,
            "description": (
                "Parameter overlay applied during the swap. Combined "
                "with the existing node's params according to "
                "paramsMode and validated against the new identifier's "
                "parameter declarations. Old params not declared by "
                "the new identifier are dropped and reported back in "
                "the response under droppedParams; new required "
                "parameters not present in either the existing params "
                "or this overlay are rejected as a structural error."
            ),
        },
        "paramsMode": {
            "type": "string",
            "enum": ["replace", "merge", "strict"],
            "description": (
                "Controls how the supplied params combine with the "
                "existing node's params. In replace mode the supplied "
                "params win and any existing param not present in the "
                "supplied set is dropped. In merge mode the supplied "
                "params overlay existing params, preserving any "
                "existing param the new identifier still accepts. In "
                "strict mode the call is rejected if any existing "
                "param is not accepted by the new identifier. Defaults "
                "to replace."
            ),
        },
        "force": {
            "type": "boolean",
            "description": (
                "Set to true to bypass the swap_class compatibility "
                "check when both the current and new catalog entries "
                "carry a swap_class field and the families differ. "
                "Has no effect when either entry has no declared "
                "swap_class; in that case the swap proceeds on "
                "structural validation alone."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_WRAP_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["targetName", "wrapper"],
    "description": (
        "Wraps an existing node with a new parent, making the existing "
        "node the wrapper's only child while the wrapper takes the "
        "existing node's former position in the tree. The target is "
        "identified by its instance name; passing the literal protocol "
        "token for the current root targets the root without requiring "
        "its name. If the tree's node names are not already known, call "
        "the program-read tool first rather than guess. "
        "The wrapper's catalog identifier must be looked up in the "
        "manifest and must be a control-flow or decorator entry; "
        "arbitrary text variants that merely resemble a real catalog "
        "identifier are rejected. The wrapper instance name must be "
        "unique across the entire program tree; collision with an "
        "existing name is rejected with a structural-error response, "
        "and the caller should retry with a different name rather than "
        "retry with the same one. The wrapper payload must not carry "
        "its own children, because the handler inserts the target's "
        "existing subtree as the wrapper's only child and supplying "
        "additional children would be ambiguous. "
        "A common pattern this tool enables: when a decorator's "
        "single-child arity prevents inserting multiple steps directly "
        "under it, wrap the decorator's existing child with a "
        "multi-child parent first, then insert the additional steps "
        "under that new multi-child parent. "
        "Prefer this tool over replace_subtree whenever the goal is to "
        "add a parent above an existing node while preserving the "
        "existing subtree intact — replace_subtree would force the "
        "caller to re-emit the entire subtree from scratch. To replace "
        "an existing wrapper with a different wrapper kind in place, "
        "use swap_node instead."
    ),
    # Examples removed 2026-05-06 — see _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA
    # comment for the full reason. tl;dr: model copies example payloads
    # literally, leading to upsert loops. The MCP wrapper null-arg
    # validator is the load-bearing safety net; schema examples are
    # net-negative for this model regime.
    "properties": {
        "targetName": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Instance name of the node to wrap, or the literal "
                "protocol token for the current root if the root itself "
                "is the target. Wrapping the root produces a new root."
            ),
        },
        "wrapper": _TREE_NODE_WRAPPER_SCHEMA,
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_PROGRAM_PARAMETER_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["name", "type"],
    "properties": {
        "name": {"type": "string", "minLength": 1},
        "type": {
            "type": "string",
            "enum": ["string", "float", "int", "bool", "enum", "pose", "float_list", "string_list", "dict"],
        },
        "default": {},
        "constraints": {"type": "object", "additionalProperties": True},
        "description": {"type": "string"},
    },
}


_PROGRAM_DRAFT_UPSERT_PARAMETERS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["parameters"],
    "description": (
        "Declares or replaces program-level parameter declarations in "
        "the draft. Program parameters are typed values bound at "
        "program-load time and referenced by name from inside node "
        "parameters; they let the same node react to different inputs "
        "across runs without rewriting the tree. Replacement is keyed "
        "by parameter name."
    ),
    "properties": {
        "parameters": {
            "type": "array",
            "items": _PROGRAM_PARAMETER_SCHEMA,
            "minItems": 1,
            "description": (
                "List of program-parameter declarations to insert or "
                "replace. Each declaration carries a name, a type, and "
                "optional default and constraint metadata."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_PROGRAM_DRAFT_REMOVE_PARAMETERS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["names"],
    "description": (
        "Removes program-level parameter declarations from the draft. "
        "If a node's parameter still references the removed name, the "
        "program will fail validation; the caller is responsible for "
        "clearing those references first."
    ),
    "properties": {
        "names": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
            "description": (
                "List of program-parameter names to remove. Each name "
                "must currently exist in the draft."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_NAMESPACE_FIELD_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["name", "type"],
    "properties": {
        "name": {"type": "string", "minLength": 1},
        "type": {
            "type": "string",
            "enum": ["string", "float", "int", "bool", "pose", "float_list", "string_list", "dict"],
        },
        "description": {"type": "string"},
    },
}


_BLACKBOARD_NAMESPACE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id", "contract", "prefix"],
    "properties": {
        "id": {"type": "string", "minLength": 1},
        "contract": {"type": "string", "minLength": 1},
        "prefix": {"type": "string", "minLength": 1},
        "description": {"type": "string"},
        "fields": {
            "type": "array",
            "items": _BLACKBOARD_NAMESPACE_FIELD_SCHEMA,
        },
    },
}


_BLACKBOARD_KEY_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id", "type", "key"],
    "properties": {
        "id": {"type": "string", "minLength": 1},
        "type": {
            "type": "string",
            "enum": ["string", "float", "int", "bool", "pose", "float_list", "string_list", "dict"],
        },
        "key": {"type": "string", "minLength": 1},
        "description": {"type": "string"},
    },
}


_BLACKBOARD_DRAFT_UPSERT_NAMESPACES_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["namespaces"],
    "description": (
        "Declares or replaces blackboard namespace declarations in the "
        "draft. Blackboard namespaces group related runtime values "
        "written by one node and read by others, with a contract "
        "describing the expected shape. Replacement is keyed by "
        "namespace identifier."
    ),
    "properties": {
        "namespaces": {
            "type": "array",
            "items": _BLACKBOARD_NAMESPACE_SCHEMA,
            "minItems": 1,
            "description": (
                "List of namespace declarations to insert or replace, "
                "each carrying its identifier, contract, prefix, and "
                "optional field declarations."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_REMOVE_NAMESPACES_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["ids"],
    "description": (
        "Removes blackboard namespace declarations from the draft. If "
        "any node still binds to the removed namespace, the program "
        "will fail validation; the caller is responsible for clearing "
        "those bindings first."
    ),
    "properties": {
        "ids": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
            "description": (
                "List of namespace identifiers to remove. Each must "
                "currently exist in the draft."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_UPSERT_KEYS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["keys"],
    "description": (
        "Declares or replaces blackboard key declarations in the draft. "
        "A blackboard key names a typed slot that nodes can read or "
        "write. Replacement is keyed by key identifier."
    ),
    "properties": {
        "keys": {
            "type": "array",
            "items": _BLACKBOARD_KEY_SCHEMA,
            "minItems": 1,
            "description": (
                "List of key declarations to insert or replace, each "
                "carrying its identifier, type, and key path."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_REMOVE_KEYS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["ids"],
    "description": (
        "Removes blackboard key declarations from the draft. If any "
        "node still binds to the removed key, the program will fail "
        "validation; the caller is responsible for clearing those "
        "bindings first."
    ),
    "properties": {
        "ids": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
            "description": (
                "List of key identifiers to remove. Each must currently "
                "exist in the draft."
            ),
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


ASSISTANT_TOOL_INPUT_SCHEMAS: dict[str, dict[str, Any]] = {
    "catalog_read": _CATALOG_READ_SCHEMA,
    "skills_read": _SKILLS_READ_SCHEMA,
    "deployment_capabilities_read": _NO_ARGUMENT_SCHEMA,
    "program_read": _NO_ARGUMENT_SCHEMA,
    "program_validate": _NO_ARGUMENT_SCHEMA,
    "status_read": _NO_ARGUMENT_SCHEMA,
    "scene_inspect": _NO_ARGUMENT_SCHEMA,
    "scene_draft_add_object": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
    "scene_draft_update_object": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
    "scene_draft_upsert_objects": _SCENE_DRAFT_UPSERT_OBJECTS_SCHEMA,
    "scene_draft_remove_objects": _SCENE_DRAFT_REMOVE_OBJECTS_SCHEMA,
    "tree_draft_insert_node": _TREE_DRAFT_INSERT_NODE_SCHEMA,
    "tree_draft_update_node_params": _TREE_DRAFT_UPDATE_NODE_PARAMS_SCHEMA,
    "tree_draft_delete_node": _TREE_DRAFT_DELETE_NODE_SCHEMA,
    "tree_draft_move_node": _TREE_DRAFT_MOVE_NODE_SCHEMA,
    "tree_draft_replace_subtree": _TREE_DRAFT_REPLACE_SUBTREE_SCHEMA,
    "tree_draft_change_node_kind": _TREE_DRAFT_CHANGE_NODE_KIND_SCHEMA,
    "tree_draft_wrap_node": _TREE_DRAFT_WRAP_NODE_SCHEMA,
    "program_draft_upsert_parameters": _PROGRAM_DRAFT_UPSERT_PARAMETERS_SCHEMA,
    "program_draft_remove_parameters": _PROGRAM_DRAFT_REMOVE_PARAMETERS_SCHEMA,
    "blackboard_draft_upsert_namespaces": _BLACKBOARD_DRAFT_UPSERT_NAMESPACES_SCHEMA,
    "blackboard_draft_remove_namespaces": _BLACKBOARD_DRAFT_REMOVE_NAMESPACES_SCHEMA,
    "blackboard_draft_upsert_keys": _BLACKBOARD_DRAFT_UPSERT_KEYS_SCHEMA,
    "blackboard_draft_remove_keys": _BLACKBOARD_DRAFT_REMOVE_KEYS_SCHEMA,
}


def assistant_tool_input_schema(
    tool_id: str,
    *,
    catalog: Any | None = None,
    node_ids: set[str] | None = None,
    field_policy: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Return a defensive copy of a known built-in assistant tool schema."""
    if tool_id == "tree_draft_insert_node":
        schema = _catalog_projected_insert_node_schema(catalog, node_ids=node_ids)
        return _narrow_schema_for_field_policy(schema, field_policy)
    if tool_id == "tree_draft_update_node_params":
        schema = _catalog_projected_update_node_params_schema(
            catalog,
            node_ids=node_ids,
        )
        return _narrow_schema_for_field_policy(schema, field_policy)
    if tool_id in {"scene_draft_add_object", "scene_draft_update_object"}:
        schema = _catalog_projected_scene_draft_single_object_schema(catalog)
        return _narrow_schema_for_field_policy(schema, field_policy)
    if tool_id == "scene_draft_upsert_objects":
        schema = _catalog_projected_scene_draft_upsert_objects_schema(catalog)
        return _narrow_schema_for_field_policy(schema, field_policy)
    schema = ASSISTANT_TOOL_INPUT_SCHEMAS.get(tool_id)
    if schema is None:
        return None
    return _narrow_schema_for_field_policy(schema, field_policy)


def enrich_assistant_tool_descriptor(
    tool: dict[str, Any],
    *,
    catalog: Any | None = None,
    node_ids: set[str] | None = None,
    field_policy: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Attach the canonical ``inputSchema`` to a built-in assistant tool descriptor.

    SINGLE-SOURCE-OF-TRUTH PARITY CLEANUP (not a behavioural fix). The curated
    schema in ``ASSISTANT_TOOL_INPUT_SCHEMAS`` is the schema the mode manifest
    (``GET /api/assistant/modes/<mode>``) serves to the gateway lanes
    (OpenClaw / Hermes via ``manyforge-mcp-bridge``). The deployment-resolved
    catalog, however, carries a *legacy reduced* schema for the scene/tree tools
    under ``input_schema`` / ``parameters`` (a flat alias set, e.g. ``size`` /
    ``dimensions``, with no nested ``shape`` / ``pose`` and only ``objectId``
    required). Because the provider-request envelope (direct lane) carries these
    descriptors verbatim and the direct bridge's ``_tool_parameters_schema``
    reads ``parameters`` -> ``inputSchema`` -> ``input_schema``, the direct lane
    was sending vLLM a *different* tool schema than the gateway lanes. We
    override with the curated schema and drop the legacy keys so all lanes see
    one descriptor source. The catalog hash is keyed on tool *ids* only, so this
    does not perturb it.

    NOTE — this does NOT measurably change the smoke scores. Investigation
    (2026-06-09) showed the direct-lane scene/tree calls already complete (200)
    and ``state_after`` already passes with the reduced schema: the model emits
    *accepted* flat aliases (top-level ``box_dimensions_m``, ``shape.shape_type``)
    which Composer normalises correctly. Those cases fail the *test scorer*,
    not Composer, because the corpus ``args_contain`` golden only credits the
    nested ``shape.type`` / ``shape.box_dims`` form. The real lever is the
    scorer contract (semantic-effect-first + accepted-form aliases) and,
    longer-term, splitting the *accepted* runtime schema from a canonical
    *model-facing* schema. See docs/operations/LANE_BRINGUP.md (schema-parity
    note); full scorer-strictness analysis archived in NemoClaw-Thor (manyforge/docs/archive/THREE-LANE-SCORER-NOTE.md).
    """
    enriched = copy.deepcopy(tool)
    tool_id = enriched.get("id") or enriched.get("name")
    if isinstance(tool_id, str):
        schema = assistant_tool_input_schema(
            tool_id,
            catalog=catalog,
            node_ids=node_ids,
            field_policy=field_policy,
        )
        if schema is not None:
            enriched["inputSchema"] = schema
            enriched["inputSchemaHash"] = _stable_schema_hash(schema)
            enriched.pop("input_schema", None)
            enriched.pop("parameters", None)
            return enriched
    # Unknown tool (no curated schema): preserve any declared schema verbatim.
    return enriched


def enrich_assistant_tool_descriptors(
    tools: list[dict[str, Any]],
    *,
    catalog: Any | None = None,
    node_ids: set[str] | None = None,
    field_policy: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    """Add known schemas to all built-in assistant tool descriptors."""
    return [
        enrich_assistant_tool_descriptor(
            tool,
            catalog=catalog,
            node_ids=node_ids,
            field_policy=field_policy,
        )
        for tool in tools
    ]
