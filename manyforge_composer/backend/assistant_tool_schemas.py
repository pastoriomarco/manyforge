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
        "allowed arity. Pose-bearing leaves declare their reference "
        "frame as a parameter that is a sibling of the pose object, not "
        "as a key inside the pose object."
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


def assistant_tool_input_schema(tool_id: str) -> dict[str, Any] | None:
    """Return a defensive copy of a known built-in assistant tool schema."""
    schema = ASSISTANT_TOOL_INPUT_SCHEMAS.get(tool_id)
    if schema is None:
        return None
    return copy.deepcopy(schema)


def enrich_assistant_tool_descriptor(tool: dict[str, Any]) -> dict[str, Any]:
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
        schema = assistant_tool_input_schema(tool_id)
        if schema is not None:
            enriched["inputSchema"] = schema
            enriched.pop("input_schema", None)
            enriched.pop("parameters", None)
            return enriched
    # Unknown tool (no curated schema): preserve any declared schema verbatim.
    return enriched


def enrich_assistant_tool_descriptors(tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Add known schemas to all built-in assistant tool descriptors."""
    return [enrich_assistant_tool_descriptor(tool) for tool in tools]
