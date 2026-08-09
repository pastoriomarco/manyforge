"""Exact generated-schema validation for Composer Draft callbacks."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from manyforge_composer.backend.assistant_schema_validation import (
    CallbackSchemaValidationError,
    validate_callback_arguments,
)
from manyforge_composer.backend.draft_session import SUPPORTED_DRAFT_MUTATIONS


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_REGISTRY_PATH = (
    _REPOSITORY_ROOT
    / "manyforge_deployment"
    / "resources"
    / "assistant"
    / "assistant_instrument_registry_source.v1.json"
)


def _tools() -> dict[str, dict]:
    registry = json.loads(_REGISTRY_PATH.read_text(encoding="utf-8"))
    return {row["tool_id"]: row for row in registry["tools"]}


def _box_resource(identity: str = "fixture") -> dict:
    return {
        "scene_resource_id": identity,
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.2, 0.3]},
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.0, 0.0, 0.0],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
    }


def test_draft_owner_tool_set_is_exactly_the_generated_callback_set() -> None:
    draft_tools = {
        tool_id: row
        for tool_id, row in _tools().items()
        if row["effect"] == "composer_draft_mutating"
    }
    generated = set(draft_tools)

    assert generated == set(SUPPORTED_DRAFT_MUTATIONS)
    assert len(generated) == 17
    assert all(
        "force_discard_overrides"
        not in json.dumps(row["input_schema"], sort_keys=True)
        for row in draft_tools.values()
    )


def test_generated_scene_schema_accepts_only_the_strict_v1_envelope() -> None:
    schema = _tools()["scene_draft_add_object"]["input_schema"]

    validate_callback_arguments({"scene_resource": _box_resource()}, schema)

    aliased = _box_resource()
    aliased["objectId"] = aliased.pop("scene_resource_id")
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments({"scene_resource": aliased}, schema)

    with pytest.raises(CallbackSchemaValidationError, match="unknown properties"):
        validate_callback_arguments(
            {
                "scene_resource": _box_resource(),
                "force_discard_overrides": True,
            },
            schema,
        )


def test_generated_relative_orientation_and_tree_union_schemas_are_enforced() -> None:
    tools = _tools()
    update_resource = _box_resource("table")
    update_resource["pose"].pop("orientation")
    update_resource["pose"]["rotate_local_rpy_deg"] = [0.0, 0.0, 90.0]
    validate_callback_arguments(
        {"scene_resource": update_resource},
        tools["scene_draft_update_object"]["input_schema"],
    )

    update_node = {
        "instance_id": "move_to_pick",
        "expected_node_id": "motion.move_to_joint_target",
        "parameters": {"planning_timeout_s": 3.0},
    }
    validate_callback_arguments(
        update_node,
        tools["tree_draft_update_node_params"]["input_schema"],
    )
    update_node["expected_node_id"] = "timer"
    with pytest.raises(CallbackSchemaValidationError, match="schema branch"):
        validate_callback_arguments(
            update_node,
            tools["tree_draft_update_node_params"]["input_schema"],
        )


def test_tree_pose_update_schema_retains_strict_relative_patch_authoring() -> None:
    schema = _tools()["tree_draft_update_node_params"]["input_schema"]
    payload = {
        "instance_id": "scene_update",
        "expected_node_id": "scene.upsert_object",
        "parameters": {
            "pose": {"rotate_local_rpy_deg": [0.0, 73.0, 91.0]},
        },
    }

    validate_callback_arguments(payload, schema)
    payload["parameters"]["pose"]["rotate_world_rpy_deg"] = [0.0, 0.0, 1.0]
    with pytest.raises(CallbackSchemaValidationError, match="schema branch"):
        validate_callback_arguments(payload, schema)

    undeclared_authoring_shape = {
        "instance_id": "motion_update",
        "expected_node_id": "motion.move_to_pose",
        "parameters": {
            "goal": {"rotate_local_rpy_deg": [0.0, 0.0, 1.0]},
        },
    }
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(undeclared_authoring_shape, schema)


def test_wrap_schema_forbids_model_supplied_children() -> None:
    schema = _tools()["tree_draft_wrap_node"]["input_schema"]
    payload = {
        "target_instance_id": "@root",
        "wrapper": {
            "node_id": "fallback",
            "instance_id": "fallback_root",
            "parameters": {},
        },
    }
    validate_callback_arguments(payload, schema)

    payload["wrapper"]["children"] = []
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(payload, schema)


def test_schema_validator_rejects_non_json_and_unsupported_schema_contracts() -> None:
    with pytest.raises(CallbackSchemaValidationError, match="expected JSON type"):
        validate_callback_arguments(True, {"type": "integer"})
    with pytest.raises(CallbackSchemaValidationError, match="unsupported keywords"):
        validate_callback_arguments([], {"type": "array", "contains": {}})
