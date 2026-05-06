"""JSON Schemas for built-in assistant bridge tools."""
from __future__ import annotations

import copy
from typing import Any


_NO_ARGUMENT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}


_CATALOG_READ_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "entryIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "description": "Optional catalog entry IDs to return. Omit for all entries.",
        },
        "category": {
            "type": "string",
            "minLength": 1,
            "description": "Optional catalog category filter.",
        },
        "kind": {
            "type": "string",
            "minLength": 1,
            "description": "Optional catalog kind filter such as action, composite, or decorator.",
        },
        "includeParameters": {
            "type": "boolean",
            "description": "Include parameter metadata for returned entries.",
        },
    },
}


_SKILLS_READ_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "skillIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "description": "Optional skill IDs to return. Omit for all skills.",
        },
        "includeBodies": {
            "type": "boolean",
            "description": "Include skill body/expansion details.",
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
        "Single detached Composer draft scene object. Prefer sending "
        "objectId, shape, and pose directly at top level. The callback also "
        "accepts legacy sceneResource wrappers and common aliases such as "
        "shapeType, size, dimensions, radius, diameter, and position."
    ),
    # 2026-05-06 v8.1 probe: a JSON-Schema `examples` array previously
    # lived here with a worked payload using objectId "obstacle_01".
    # The MCP wrapper's null-arg validator passes the examples through
    # to the model on validation errors, AND the manifest exposes them
    # via inputSchema.examples on tools/list. The Nemotron-3-Nano-Omni
    # model in this regime (T=0.6, top_p=0.95, thinking off) takes the
    # literal value too seriously: on the "Add a small obstacle…" task
    # it called scene.draft.add_object 116 times in a single turn, all
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
            "enum": ["box", "sphere"],
            "description": "Convenience alias for shape.type.",
        },
        "shape_type": {
            "type": "string",
            "enum": ["box", "sphere"],
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
            "description": "Convenience alias for a sphere radius in meters.",
        },
        "diameter": {
            "type": "number",
            "description": "Convenience alias for a sphere diameter in meters.",
        },
        "sphereRadiusM": {
            "type": "number",
            "description": "Sphere radius in meters.",
        },
        "sphere_radius_m": _SCENE_RESOURCE_SCHEMA["properties"]["shape"]["properties"]["sphere_radius_m"],
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
    "properties": {
        "sceneResources": {
            "type": "array",
            "items": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
            "minItems": 1,
            "description": (
                "Scene resources to insert or replace in the Composer draft. "
                "Duplicate objectId values in one request are rejected."
            ),
        },
        "forceDiscardOverrides": {
            "type": "boolean",
            "description": "Discard unsaved scene-resource overrides with matching objectIds.",
        },
    },
}


_SCENE_DRAFT_REMOVE_OBJECTS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["objectIds"],
    "properties": {
        "objectIds": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
            "description": "Scene object IDs that must already exist in the Composer draft.",
        },
        "forceDiscardOverrides": {
            "type": "boolean",
            "description": "Discard unsaved scene-resource overrides with matching objectIds.",
        },
    },
}


_TREE_NODE_CHILD_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": True,
    "required": ["id", "name"],
    "properties": {
        "id": {"type": "string", "minLength": 1},
        "name": {"type": "string", "minLength": 1},
        "params": {"type": "object", "additionalProperties": True},
        "modificationPolicy": {"type": "object", "additionalProperties": {"type": "string"}},
        "recoveryPolicy": {"type": "object", "additionalProperties": True},
        "children": {"type": "array", "items": {"type": "object", "additionalProperties": True}},
    },
}


_TREE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["id", "name"],
    "properties": {
        "id": {"type": "string", "minLength": 1},
        "name": {"type": "string", "minLength": 1},
        "params": {"type": "object", "additionalProperties": True},
        "modificationPolicy": {"type": "object", "additionalProperties": {"type": "string"}},
        "recoveryPolicy": {"type": "object", "additionalProperties": True},
        "children": {
            "type": "array",
            "items": _TREE_NODE_CHILD_SCHEMA,
        },
    },
}


_TREE_DRAFT_INSERT_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["parentName", "node"],
    "properties": {
        "parentName": {
            "type": "string",
            "minLength": 1,
            "description": "Existing parent node instance name.",
        },
        "node": _TREE_NODE_SCHEMA,
        "index": {
            "type": "integer",
            "minimum": 0,
            "description": "Optional insertion index. Omit to append.",
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_UPDATE_NODE_PARAMS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "params"],
    "properties": {
        "nodeName": {"type": "string", "minLength": 1},
        "params": {"type": "object", "additionalProperties": True},
        "merge": {
            "type": "boolean",
            "description": "Merge into existing params when true; replace params when false.",
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_DELETE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName"],
    "properties": {
        "nodeName": {
            "type": "string",
            "minLength": 1,
            "description": "Existing non-root node instance name to delete with its subtree.",
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_MOVE_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "newParentName"],
    "properties": {
        "nodeName": {"type": "string", "minLength": 1},
        "newParentName": {"type": "string", "minLength": 1},
        "index": {
            "type": "integer",
            "minimum": 0,
            "description": "Optional destination child index. Omit to append.",
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_TREE_DRAFT_REPLACE_SUBTREE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "subtree"],
    "properties": {
        "nodeName": {"type": "string", "minLength": 1},
        "subtree": _TREE_NODE_SCHEMA,
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
                "Node catalog id (NOT a role or descriptive label). Must exactly "
                "match an entry in the deployment's node catalog and be in this "
                "assistant mode's catalog.nodes allowlist. Common control-flow "
                "ids: 'sequence', 'fallback', 'parallel', 'repeat', 'retry', "
                "'inverter'. Do NOT invent variants like 'repeat_root', "
                "'repeat_node', 'sequence_root', etc. — those will be rejected. "
                "Use the literal catalog id."
            ),
        },
        "name": {
            "type": "string",
            "minLength": 1,
            "description": (
                "Caller-chosen instance name for this node in the program tree "
                "(any descriptive label, e.g., 'cycle_loop' for a repeat or "
                "'main_sequence' for a sequence). This is a label, not a "
                "catalog id."
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


_TREE_DRAFT_WRAP_NODE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["targetName", "wrapper"],
    "description": (
        "Wrap a behavior-tree node with a new parent (decorator or control-flow). "
        "wrapper.id MUST be a literal node-catalog id like 'repeat', 'sequence', "
        "'fallback', 'parallel', 'retry', 'inverter'. Do NOT invent variants like "
        "'repeat_root' or 'repeat_node'. Use targetName='@root' to wrap the "
        "current root without needing to know its name. "
        "wrapper.name MUST be unique across the program tree — the bridge "
        "rejects duplicate node-instance names with HTTP 400. If your call "
        "fails with 'Duplicate node instance name: …', pick a different "
        "wrapper.name (e.g. add a suffix) and retry; do not call again with "
        "the same name."
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
                "Name of the behavior-tree node to wrap, or '@root' to wrap the "
                "current root without needing to know its name. Wrapping the "
                "root produces a new root."
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
    "properties": {
        "parameters": {
            "type": "array",
            "items": _PROGRAM_PARAMETER_SCHEMA,
            "minItems": 1,
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_PROGRAM_DRAFT_REMOVE_PARAMETERS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["names"],
    "properties": {
        "names": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
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
    "properties": {
        "namespaces": {
            "type": "array",
            "items": _BLACKBOARD_NAMESPACE_SCHEMA,
            "minItems": 1,
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_REMOVE_NAMESPACES_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["ids"],
    "properties": {
        "ids": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_UPSERT_KEYS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["keys"],
    "properties": {
        "keys": {
            "type": "array",
            "items": _BLACKBOARD_KEY_SCHEMA,
            "minItems": 1,
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


_BLACKBOARD_DRAFT_REMOVE_KEYS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["ids"],
    "properties": {
        "ids": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
            "minItems": 1,
        },
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


ASSISTANT_TOOL_INPUT_SCHEMAS: dict[str, dict[str, Any]] = {
    "catalog.read": _CATALOG_READ_SCHEMA,
    "skills.read": _SKILLS_READ_SCHEMA,
    "deployment.capabilities.read": _NO_ARGUMENT_SCHEMA,
    "program.read": _NO_ARGUMENT_SCHEMA,
    "program.validate": _NO_ARGUMENT_SCHEMA,
    "status.read": _NO_ARGUMENT_SCHEMA,
    "scene.inspect": _NO_ARGUMENT_SCHEMA,
    "scene.draft.add_object": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
    "scene.draft.update_object": _SCENE_DRAFT_SINGLE_OBJECT_SCHEMA,
    "scene.draft.upsert_objects": _SCENE_DRAFT_UPSERT_OBJECTS_SCHEMA,
    "scene.draft.remove_objects": _SCENE_DRAFT_REMOVE_OBJECTS_SCHEMA,
    "tree.draft.insert_node": _TREE_DRAFT_INSERT_NODE_SCHEMA,
    "tree.draft.update_node_params": _TREE_DRAFT_UPDATE_NODE_PARAMS_SCHEMA,
    "tree.draft.delete_node": _TREE_DRAFT_DELETE_NODE_SCHEMA,
    "tree.draft.move_node": _TREE_DRAFT_MOVE_NODE_SCHEMA,
    "tree.draft.replace_subtree": _TREE_DRAFT_REPLACE_SUBTREE_SCHEMA,
    "tree.draft.wrap_node": _TREE_DRAFT_WRAP_NODE_SCHEMA,
    "program.draft.upsert_parameters": _PROGRAM_DRAFT_UPSERT_PARAMETERS_SCHEMA,
    "program.draft.remove_parameters": _PROGRAM_DRAFT_REMOVE_PARAMETERS_SCHEMA,
    "blackboard.draft.upsert_namespaces": _BLACKBOARD_DRAFT_UPSERT_NAMESPACES_SCHEMA,
    "blackboard.draft.remove_namespaces": _BLACKBOARD_DRAFT_REMOVE_NAMESPACES_SCHEMA,
    "blackboard.draft.upsert_keys": _BLACKBOARD_DRAFT_UPSERT_KEYS_SCHEMA,
    "blackboard.draft.remove_keys": _BLACKBOARD_DRAFT_REMOVE_KEYS_SCHEMA,
}


def assistant_tool_input_schema(tool_id: str) -> dict[str, Any] | None:
    """Return a defensive copy of a known built-in assistant tool schema."""
    schema = ASSISTANT_TOOL_INPUT_SCHEMAS.get(tool_id)
    if schema is None:
        return None
    return copy.deepcopy(schema)


def enrich_assistant_tool_descriptor(tool: dict[str, Any]) -> dict[str, Any]:
    """Add a known `inputSchema` to a tool descriptor when one is not declared."""
    enriched = copy.deepcopy(tool)
    if any(key in enriched for key in ("inputSchema", "input_schema", "parameters")):
        return enriched
    tool_id = enriched.get("id") or enriched.get("name")
    if not isinstance(tool_id, str):
        return enriched
    schema = assistant_tool_input_schema(tool_id)
    if schema is not None:
        enriched["inputSchema"] = schema
    return enriched


def enrich_assistant_tool_descriptors(tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Add known schemas to all built-in assistant tool descriptors."""
    return [enrich_assistant_tool_descriptor(tool) for tool in tools]
