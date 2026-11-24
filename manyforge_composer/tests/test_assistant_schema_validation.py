"""Exact generated-schema validation for Composer Draft callbacks."""

from __future__ import annotations

import json

import pytest

from manyforge_composer.backend.assistant_schema_validation import (
    CallbackSchemaValidationError,
    validate_callback_arguments,
)
from manyforge_composer.backend.draft_session import SUPPORTED_DRAFT_MUTATIONS


def _projection() -> dict:
    """Mode projection as Headless builds it from the loaded catalog."""

    from manyforge_headless_api.service import _assistant_instruments, _assistant_mode

    tool_ids = [
        "blackboard_draft_remove_keys", "blackboard_draft_remove_namespaces",
        "blackboard_draft_upsert_keys", "blackboard_draft_upsert_namespaces",
        "program_draft_remove_parameters", "program_draft_upsert_parameters",
        "scene_draft_add_object", "scene_draft_remove_objects",
        "scene_draft_update_object", "scene_draft_upsert_objects",
        "tree_draft_change_node_kind", "tree_draft_delete_node", "tree_draft_insert_node",
        "tree_draft_move_node", "tree_draft_rename_node", "tree_draft_replace_subtree",
        "tree_draft_update_node_params", "tree_draft_wrap_node",
    ]
    offers = sorted(
        {
            requirement.capability_id
            for entry in _assistant_instruments().entries()
            for requirement in entry.required_capabilities
        }
    )
    native = {
        "grant_policy": {"approval_mode": None, "review_policy": "required", "session_kind": None},
        "mode": "composer-assistant",
        "skills": [],
        "tools": [
            {"effect": "composer_draft_mutating", "tool_id": tool_id} for tool_id in tool_ids
        ],
        "source": {},
    }
    return _assistant_mode(native, offers)


def _tools() -> dict[str, dict]:
    return {row["tool_id"]: row for row in _projection()["tools"]}


def _nodes() -> dict[str, dict]:
    return {row["node_id"]: row for row in _projection()["nodes"]}


def _types() -> dict[str, dict]:
    """Full shapes of the tools' named argument types (generated)."""
    return _projection()["types"]


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
    assert len(generated) == 18
    assert all(
        "force_discard_overrides"
        not in json.dumps(row["input_schema"], sort_keys=True)
        for row in draft_tools.values()
    )


def test_generated_scene_schema_accepts_only_the_strict_v1_envelope() -> None:
    schema = _tools()["scene_draft_add_object"]["input_schema"]
    assert _tools()["scene_draft_add_object"]["argument_types"] == {
        "scene_resource": "scene_resource"
    }

    validate_callback_arguments({"scene_resource": _box_resource()}, schema)
    validate_callback_arguments(_box_resource(), _types()["scene_resource"])

    aliased = _box_resource()
    aliased["objectId"] = aliased.pop("scene_resource_id")
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(aliased, _types()["scene_resource"])

    with pytest.raises(CallbackSchemaValidationError, match="unknown properties"):
        validate_callback_arguments(
            {
                "scene_resource": _box_resource(),
                "force_discard_overrides": True,
            },
            schema,
        )


def test_collision_rules_are_node_parameters_without_a_dedicated_tool() -> None:
    assert not any("contact_polic" in tool_id for tool_id in _tools())
    assert "contact_policy" not in _types()
    nodes = _nodes()
    contact = {
        "rule_id": "tip_on_part",
        "a": [{"kind": "active_tcp_contact_bodies"}],
        "b": [{"kind": "scene_object", "blackboard_key": "trial.probe_object"}],
        "contact": {"max_penetration_m": 0.002, "end_from_fraction": 0.8},
    }
    rules = [{"rule_id": "everything_else", "clearance_m": 0.002}, contact]
    validate_callback_arguments(
        {"parameters": {"collision_rules": rules}},
        nodes["motion.probe_search"]["update_schema"],
    )
    # A pick or place sequence row names the leg it applies to.
    validate_callback_arguments(
        {"parameters": {"collision_rules": [{**contact, "leg": "descend"}]}},
        nodes["planning.plan_pick_sequence"]["update_schema"],
    )
    for invalid in (
        {**contact, "contact": {"max_depth_m": 0.002}},
        {**contact, "a": [{"kind": "model_group"}]},
        {**contact, "leg": "descend"},
    ):
        with pytest.raises(CallbackSchemaValidationError):
            validate_callback_arguments(
                {"parameters": {"collision_rules": [invalid]}},
                nodes["motion.probe_search"]["update_schema"],
            )
    # Rule values are plain numbers: the parameter takes no value source.
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(
            {"value_sources": {"collision_rules": {
                "kind": "blackboard_key", "reference": "trial.rules"}}},
            nodes["motion.probe_search"]["update_schema"],
        )


def test_generated_relative_orientation_and_tree_union_schemas_are_enforced() -> None:
    tools = _tools()
    update_resource = _box_resource("table")
    update_resource["pose"].pop("orientation")
    update_resource["pose"]["rotate_local_rpy_deg"] = [0.0, 0.0, 90.0]
    validate_callback_arguments(update_resource, _types()["scene_resource_edit"])

    update_node = {
        "instance_id": "move_to_pick",
        "expected_node_id": "motion.move_to_joint_target",
        "parameters": {"planning_timeout_s": 3.0},
    }
    validate_callback_arguments(
        update_node,
        tools["tree_draft_update_node_params"]["input_schema"],
    )
    # Node parameters are checked per node, against the node's own schema.
    nodes = _nodes()
    patch = {"parameters": update_node["parameters"]}
    validate_callback_arguments(patch, nodes["motion.move_to_joint_target"]["update_schema"])
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(patch, nodes["timer"]["update_schema"])


def test_tree_pose_update_schema_retains_strict_relative_patch_authoring() -> None:
    nodes = _nodes()
    schema = nodes["scene.upsert_object"]["update_schema"]
    payload = {"parameters": {"pose": {"rotate_local_rpy_deg": [0.0, 73.0, 91.0]}}}

    validate_callback_arguments(payload, schema)
    payload["parameters"]["pose"]["rotate_world_rpy_deg"] = [0.0, 0.0, 1.0]
    with pytest.raises(CallbackSchemaValidationError, match="schema branch"):
        validate_callback_arguments(payload, schema)

    undeclared_authoring_shape = {"parameters": {"goal": {"rotate_local_rpy_deg": [0.0, 0.0, 1.0]}}}
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(
            undeclared_authoring_shape, nodes["motion.move_to_pose"]["update_schema"]
        )


def test_wrap_schema_forbids_model_supplied_children() -> None:
    schema = _types()["wrapper_node"]
    wrapper = {"node_id": "fallback", "instance_id": "fallback_root", "parameters": {}}
    validate_callback_arguments(wrapper, schema)

    wrapper["children"] = []
    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments(wrapper, schema)


def test_schema_validator_rejects_non_json_and_unsupported_schema_contracts() -> None:
    with pytest.raises(CallbackSchemaValidationError, match="expected JSON type"):
        validate_callback_arguments(True, {"type": "integer"})
    with pytest.raises(CallbackSchemaValidationError, match="unsupported keywords"):
        validate_callback_arguments([], {"type": "array", "contains": {}})
