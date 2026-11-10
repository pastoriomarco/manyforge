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
        "program_draft_remove_contact_policies", "program_draft_remove_parameters",
        "program_draft_upsert_contact_policies", "program_draft_upsert_parameters",
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
    assert len(generated) == 20
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


def _grasp_policy() -> dict:
    return {
        "api_version": "manyforge.contact_policy.v1",
        "policy_id": "grasp_contact",
        "mode": "contact_grasp",
        "sensing_mode": "geometry_only",
        "default_unmatched_response": "hard_stop",
        "zones": [{
            "zone_id": "fingertips",
            "body_pairs": [{
                "body_a": {"kind": "component_model_body",
                           "body": {"component_id": "gripper", "collision_body_id": "Finger_Left"}},
                "body_b": {"kind": "runtime_object", "runtime_object_id": "pick_target"},
            }],
            "max_penetration_m": 0.002,
            "terminal_window_start_fraction": 0.8,
            "violation_response": "hard_stop",
        }],
    }


def test_generated_contact_policy_schema_accepts_the_v1_grasp_and_release_shapes() -> None:
    tools = _tools()
    upsert = tools["program_draft_upsert_contact_policies"]
    remove = tools["program_draft_remove_contact_policies"]
    assert upsert["argument_types"] == {"contact_policies": "contact_policy[]"}
    assert upsert["input_schema"]["properties"]["contact_policies"]["type"] == "array"
    assert remove["input_schema"]["properties"]["contact_policy_ids"]["type"] == "array"
    assert "contact_grasp" in upsert["description"]
    assert "planning.plan_pick_sequence" in upsert["description"]
    schema = _types()["contact_policy"]

    validate_callback_arguments({"contact_policies": [_grasp_policy()]}, upsert["input_schema"])
    validate_callback_arguments({"contact_policy_ids": ["grasp_contact"]}, remove["input_schema"])
    validate_callback_arguments(_grasp_policy(), schema)
    release = _grasp_policy()
    release["mode"] = "contact_release"
    zone = release["zones"][0]
    del zone["max_penetration_m"], zone["terminal_window_start_fraction"]
    zone["release_evidence_mode"] = "binary_contact"
    zone["require_separation_by_end"] = True
    validate_callback_arguments(release, schema)

    with pytest.raises(CallbackSchemaValidationError):
        validate_callback_arguments({"contact_policy_ids": []}, remove["input_schema"])
    for mutate in (
        lambda policy: policy.update(version="sha256:" + "0" * 64),  # Composer computes it
        lambda policy: policy.update(sensing_mode="force"),
        lambda policy: policy["zones"].append(policy["zones"][0]),
        lambda policy: policy["zones"][0].pop("max_penetration_m"),
        lambda policy: policy["zones"][0].update(max_penetration_m=0),
        lambda policy: policy["zones"][0]["body_pairs"][0].update(body_b={"kind": "scene"}),
    ):
        invalid = _grasp_policy()
        mutate(invalid)
        with pytest.raises(CallbackSchemaValidationError):
            validate_callback_arguments(invalid, schema)


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
