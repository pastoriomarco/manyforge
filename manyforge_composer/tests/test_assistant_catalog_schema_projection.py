from __future__ import annotations

import hashlib
import json

import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_composer.backend.assistant_tool_schemas import (
    _stable_schema_hash,
    assistant_tool_input_schema,
    enrich_assistant_tool_descriptor,
)


def _minimal_v1_catalog() -> dict:
    return {
        "api_version": "manyforge.node_catalog.v1",
        "catalog_version": "0.2.0",
        "schema_types": {
            "orientation_reference": {
                "type": "object",
                "additional_properties": False,
                "required": ["source"],
                "properties": {
                    "source": {
                        "type": "enum",
                        "values": ["current_pose", "scene_object", "frame"],
                    },
                    "object_id": {
                        "type": "string",
                        "required_when": {"source": "scene_object"},
                    },
                    "frame_id": {
                        "type": "string",
                        "required_when": {"source": "frame"},
                    },
                },
            },
            "stored_pose": {
                "type": "object",
                "additional_properties": False,
                "required": ["frame_id", "position_m"],
                "exactly_one": [["orientation_xyzw", "orientation_rpy_deg"]],
                "properties": {
                    "frame_id": {"type": "string"},
                    "position_m": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                    "orientation_xyzw": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 4,
                    },
                    "orientation_rpy_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                },
            },
            "pose_authoring_input": {
                "type": "object",
                "additional_properties": False,
                "required": ["frame_id", "position_m"],
                "resolves_to": "#/schema_types/stored_pose",
                "exactly_one": [
                    [
                        "orientation_xyzw",
                        "orientation_rpy_deg",
                        "rotate_local_rpy_deg",
                        "rotate_world_rpy_deg",
                        "rpy_delta_deg",
                        "orientation_from",
                    ]
                ],
                "properties": {
                    "frame_id": {"type": "string"},
                    "position_m": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                    "orientation_xyzw": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 4,
                    },
                    "orientation_rpy_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                    },
                    "rotate_local_rpy_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                        "requires_current_value": True,
                    },
                    "rotate_world_rpy_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                        "requires_current_value": True,
                    },
                    "rpy_delta_deg": {
                        "type": "array",
                        "items": {"type": "float"},
                        "length": 3,
                        "requires_current_value": True,
                    },
                    "orientation_from": {
                        "schema_ref": "#/schema_types/orientation_reference",
                    },
                },
            },
            "shape": {
                "type": "object",
                "additional_properties": False,
                "required": ["type"],
                "properties": {
                    "type": {
                        "type": "enum",
                        "values": ["box", "sphere", "cylinder", "mesh"],
                    },
                    "box_dimensions_m": {
                        "type": "array",
                        "items": {
                            "type": "float",
                            "constraints": {"min_exclusive": 0},
                        },
                        "length": 3,
                        "required_when": {"type": "box"},
                    },
                },
            },
        },
        "entries": [
            {
                "id": "sequence",
                "name": "Sequence",
                "kind": "composite",
                "children": {"min": 0, "max": "unbounded"},
                "parameters": [],
                "runners": {"py_trees": {"class": "py_trees.composites.Sequence"}},
            },
            {
                "id": "upsert_collision_object",
                "name": "Upsert Collision Object",
                "kind": "leaf",
                "children": {"min": 0, "max": 0},
                "parameters": [
                    {
                        "name": "object_id",
                        "required": True,
                        "schema": {"type": "string"},
                        "blackboard_role": "literal_or_key_reader",
                        "blackboard_value_type": "string",
                    },
                    {
                        "name": "pose",
                        "required": True,
                        "schema_ref": "#/schema_types/stored_pose",
                    },
                    {
                        "name": "shape",
                        "required": True,
                        "schema_ref": "#/schema_types/shape",
                    },
                ],
                "runners": {
                    "py_trees": {
                        "class": "manyforge_behavior.nodes.scene.UpsertCollisionObject",
                    }
                },
            },
        ],
    }


def _catalog(tmp_path) -> NodeCatalog:
    path = tmp_path / "node_catalog.yaml"
    path.write_text(yaml.safe_dump(_minimal_v1_catalog(), sort_keys=False))
    return NodeCatalog(path)


def _variant_for_id(schema: dict, node_id: str) -> dict:
    for variant in schema["properties"]["node"]["anyOf"]:
        variant_id = variant.get("properties", {}).get("id", {})
        if variant_id.get("enum") == [node_id]:
            return variant
    raise AssertionError(f"missing variant for {node_id}")


def _patch_variant_with_property(schema: dict, property_name: str) -> dict:
    for variant in schema["properties"]["params"]["anyOf"]:
        properties = variant.get("properties", {})
        if property_name in properties:
            return variant
    raise AssertionError(f"missing params patch variant with {property_name}")


def test_insert_node_schema_projects_visible_catalog_entry_params(tmp_path):
    catalog = _catalog(tmp_path)

    enriched = enrich_assistant_tool_descriptor(
        {"id": "tree_draft_insert_node"},
        catalog=catalog,
        node_ids={"upsert_collision_object"},
    )

    schema = enriched["inputSchema"]
    upsert = _variant_for_id(schema, "upsert_collision_object")
    params = upsert["properties"]["params"]

    assert "params" in upsert["required"]
    assert params["additionalProperties"] is False
    assert params["required"] == ["object_id", "pose", "shape"]
    assert params["properties"]["pose"]["additionalProperties"] is False
    assert params["properties"]["pose"]["required"] == ["frame_id", "position_m"]
    assert params["properties"]["shape"]["properties"]["type"]["enum"] == [
        "box",
        "sphere",
        "cylinder",
        "mesh",
    ]
    conditional = params["properties"]["shape"]["allOf"][0]
    assert conditional["if"]["required"] == ["type"]
    assert conditional["then"]["required"] == ["box_dimensions_m"]


def test_insert_node_schema_limits_variants_to_visible_nodes(tmp_path):
    catalog = _catalog(tmp_path)

    enriched = enrich_assistant_tool_descriptor(
        {"id": "tree_draft_insert_node"},
        catalog=catalog,
        node_ids={"sequence"},
    )

    node_variants = enriched["inputSchema"]["properties"]["node"]["anyOf"]
    ids = {
        variant.get("properties", {}).get("id", {}).get("enum", [None])[0]
        for variant in node_variants
        if isinstance(variant.get("properties"), dict)
    }

    assert ids == {"sequence"}


def test_insert_node_schema_projects_blackboard_binding_forms(tmp_path):
    catalog = _catalog(tmp_path)

    enriched = enrich_assistant_tool_descriptor(
        {"id": "tree_draft_insert_node"},
        catalog=catalog,
        node_ids={"upsert_collision_object"},
    )

    upsert = _variant_for_id(enriched["inputSchema"], "upsert_collision_object")
    object_id = upsert["properties"]["params"]["properties"]["object_id"]

    variants = object_id["anyOf"]
    assert variants[0]["type"] == "string"
    assert variants[1]["required"] == ["key_ref"]
    assert variants[1]["properties"]["key_ref"]["type"] == "string"
    assert variants[2]["required"] == ["field_ref"]
    assert variants[2]["properties"]["field_ref"]["required"] == ["namespace", "field"]


def test_update_node_params_schema_projects_sparse_catalog_param_patches(tmp_path):
    catalog = _catalog(tmp_path)

    enriched = enrich_assistant_tool_descriptor(
        {"id": "tree_draft_update_node_params"},
        catalog=catalog,
        node_ids={"upsert_collision_object"},
        field_policy={"allowedOrientationFields": ["orientation_rpy_deg"]},
    )

    schema = enriched["inputSchema"]
    params = schema["properties"]["params"]
    upsert_patch = _patch_variant_with_property(schema, "pose")

    assert "anyOf" in params
    assert upsert_patch["additionalProperties"] is False
    assert "required" not in upsert_patch
    assert set(upsert_patch["properties"]) == {"object_id", "pose", "shape"}
    pose = upsert_patch["properties"]["pose"]
    assert "orientation_rpy_deg" in pose["properties"]
    assert "orientation_xyzw" not in pose["properties"]


def test_input_schema_hash_preserves_description_property_name():
    empty_schema = {"type": "object", "properties": {}}
    description_string_schema = {
        "type": "object",
        "properties": {
            "description": {"type": "string", "description": "prose"},
        },
    }
    description_integer_schema = {
        "type": "object",
        "properties": {
            "description": {"type": "integer", "description": "prose"},
        },
    }

    assert _stable_schema_hash(empty_schema) != _stable_schema_hash(
        description_string_schema
    )
    assert _stable_schema_hash(description_string_schema) != _stable_schema_hash(
        description_integer_schema
    )


def test_scene_draft_schema_projects_catalog_shape_and_authoring_pose(tmp_path):
    catalog = _catalog(tmp_path)

    enriched = enrich_assistant_tool_descriptor(
        {"id": "scene_draft_update_object"},
        catalog=catalog,
    )

    schema = enriched["inputSchema"]
    shape = schema["properties"]["shape"]
    pose = schema["properties"]["pose"]
    scene_resource = schema["properties"]["sceneResource"]

    assert shape["additionalProperties"] is False
    assert pose["additionalProperties"] is False
    assert pose["properties"]["frame_id"]["enum"] == ["universe", "world", "World", "/World"]
    assert "rpy_delta_deg" in pose["properties"]
    assert schema["properties"]["shapeType"]["enum"] == ["box", "sphere", "cylinder", "mesh"]
    assert schema["properties"]["size"]["minItems"] == 3
    assert schema["properties"]["position"]["minItems"] == 3
    assert scene_resource["properties"]["shape"]["additionalProperties"] is False
    assert scene_resource["properties"]["pose"]["additionalProperties"] is False


def test_field_policy_narrows_orientation_fields_and_schema_hash(tmp_path):
    catalog = _catalog(tmp_path)

    base = enrich_assistant_tool_descriptor(
        {"id": "scene_draft_update_object"},
        catalog=catalog,
    )
    narrowed = enrich_assistant_tool_descriptor(
        {"id": "scene_draft_update_object"},
        catalog=catalog,
        field_policy={"allowedOrientationFields": ["rpy_delta_deg"]},
    )

    pose_properties = narrowed["inputSchema"]["properties"]["pose"]["properties"]
    top_properties = narrowed["inputSchema"]["properties"]
    assert "rpy_delta_deg" in pose_properties
    assert "rpy_delta_deg" in top_properties
    assert "rotate_local_rpy_deg" not in pose_properties
    assert "orientation_xyzw" not in pose_properties
    assert "orientation_rpy_deg" not in top_properties
    pose_exactly_one = narrowed["inputSchema"]["properties"]["pose"]["allOf"][0]["oneOf"]
    assert pose_exactly_one == [{"required": ["rpy_delta_deg"]}]
    assert narrowed["inputSchemaHash"] != base["inputSchemaHash"]
    # Structural hash (prose excluded): recompute via the canonical helper.
    assert narrowed["inputSchemaHash"] == _stable_schema_hash(narrowed["inputSchema"])


def test_insert_node_field_policy_narrows_catalog_projected_pose(tmp_path):
    catalog = _catalog(tmp_path)

    schema = assistant_tool_input_schema(
        "tree_draft_insert_node",
        catalog=catalog,
        node_ids={"upsert_collision_object"},
        field_policy={"allowedOrientationFields": ["orientation_rpy_deg"]},
    )
    assert schema is not None
    upsert = _variant_for_id(schema, "upsert_collision_object")
    pose = upsert["properties"]["params"]["properties"]["pose"]

    assert "orientation_rpy_deg" in pose["properties"]
    assert "orientation_xyzw" not in pose["properties"]
