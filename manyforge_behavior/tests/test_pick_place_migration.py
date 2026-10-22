"""Offline removal of the catalog-2.2 composite Pick/Place surface."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.pick_place_migration import (
    migrate_composite_pick_place_program,
)
from manyforge_behavior.program import ProgramValidationError


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _REPOSITORY_ROOT / "examples" / "vision_guided_bin_pick.program.yaml"


def _legacy_pick_document() -> dict:
    document = yaml.safe_load(_PROGRAM_PATH.read_text(encoding="utf-8"))
    branch = document["tree"]["children"][1]["children"][0]
    subtree = branch["children"][1]
    assert subtree["instance_id"] == "pick_left_part"
    parameters = deepcopy(subtree["children"][0]["parameters"])
    result_key = parameters.pop("output_key")
    branch["children"][1] = {
        "node_id": "motion.pick_object",
        "instance_id": subtree["instance_id"],
        "parameters": parameters,
        "recovery_policy": {"allow_intervention": True, "max_retries": 2},
        "operator_stop_policy": "hold",
        "children": [],
    }
    document["blackboard"]["keys"] = [
        row for row in document["blackboard"]["keys"] if row["key"] != result_key
    ]
    document["catalog"]["content_version"] = "2.2.0"
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return document


def test_composite_pick_migrates_to_visible_ordinary_subtree() -> None:
    migrated = migrate_composite_pick_place_program(
        _legacy_pick_document(), build_node_environment().catalog
    ).to_dict()
    subtree = migrated["tree"]["children"][1]["children"][0]["children"][1]

    assert subtree["node_id"] == "sequence"
    assert subtree["instance_id"] == "pick_left_part"
    assert subtree["recovery_policy"] == {"allow_intervention": True, "max_retries": 2}
    assert subtree["operator_stop_policy"] == "hold"
    assert [child["node_id"] for child in subtree["children"]] == [
        "planning.plan_pick_sequence",
        "motion.move_to_pose",
        "motion.follow_cartesian_path",
        "gripper.command_position",
        "scene.attach_carried_object",
        "motion.follow_cartesian_path",
    ]
    result_key = subtree["children"][0]["parameters"]["output_key"]
    result_declaration = next(
        row for row in migrated["blackboard"]["keys"] if row["key"] == result_key
    )
    assert result_declaration["type"] == "pick_planning_result"
    assert [row["field"] for row in result_declaration["fields"]] == [
        "approach_pose",
        "descend_path",
        "pick_pose",
        "retreat_path",
        "retreat_pose",
        "selected_object_id",
        "tool_position",
    ]
    assert all(
        node["node_id"] not in {"motion.pick_object", "motion.place_object"}
        for node in _walk(migrated["tree"])
    )


def test_composite_pick_migration_rejects_unmapped_fields() -> None:
    document = _legacy_pick_document()
    composite = document["tree"]["children"][1]["children"][0]["children"][1]
    composite["parameters"]["unknown_legacy_setting"] = True
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )

    with pytest.raises(ProgramValidationError, match="unknown_legacy_setting"):
        migrate_composite_pick_place_program(
            document, build_node_environment().catalog
        )


def _walk(root: dict):
    pending = [root]
    while pending:
        node = pending.pop()
        yield node
        pending.extend(reversed(node.get("children", [])))


def test_pick_place_template_preserves_one_qualified_tcp_and_separate_carrier() -> None:
    from manyforge_behavior.pick_place_templates import (
        PickPlaceTemplateError, build_pick_place_subtree,
    )
    parameters = {
        'target_role': 'arm', 'tool_target_role': 'gripper',
        'base_frame_role': 'base',
        'tcp_endpoint': {'target_role': 'gripper', 'frame_role': 'tcp'},
        'carrier_frame_role': 'carrier', 'selected_object_id': 'part',
        'tool_position': {'kind': 'named_preset', 'name': 'closed'},
        'contact_policy_id': 'pick.contact',
    }
    subtree = build_pick_place_subtree(
        'pick', instance_id='pick', parameters=parameters, value_sources={},
        result_key='trial.pick', touch_body_roles=['fingers'],
    )
    for child in subtree['children']:
        if child['node_id'].startswith(('planning.', 'motion.')):
            assert child['parameters']['tcp_endpoint'] == parameters['tcp_endpoint']
            assert 'tcp_frame_role' not in child['parameters']
    attach = next(child for child in subtree['children'] if child['node_id'] == 'scene.attach_carried_object')
    assert attach['parameters']['target_role'] == 'gripper'
    assert attach['parameters']['carrier_frame_role'] == 'carrier'
    assert attach['parameters']['touch_body_roles'] == ['fingers']
    parameters['tcp_frame_role'] = 'tcp'
    with pytest.raises(PickPlaceTemplateError, match='tcp_frame_role'):
        build_pick_place_subtree('pick', instance_id='pick', parameters=parameters,
            value_sources={}, result_key='trial.pick', touch_body_roles=['fingers'])
