"""TCP migration across the original composite and ordinary-node tree."""

from copy import deepcopy

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.pick_place_migration import migrate_composite_pick_place_program
from manyforge_behavior.program import TcpEndpointAliasAmbiguous

from .test_pick_place_migration import _legacy_pick_document as _base_legacy_pick_document, _walk


def _reseal(document):
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )


def _tcp_nodes(document):
    return [node for node in _walk(document["tree"])
            if node.get("node_id", "").startswith(("motion.", "planning."))
            and "tcp_endpoint" in node.get("parameters", {})]


def _legacy_pick_document():
    # The example is regenerated with current selectors. Construct the old
    # selector spelling explicitly before testing the legacy conversion.
    document = _base_legacy_pick_document()
    selector_nodes = {
        "motion.move_to_joint_target", "motion.move_to_pose", "motion.follow_cartesian_path",
        "motion.move_until_signal", "motion.probe_search", "motion.pick_object",
        "motion.place_object", "planning.plan_pick_sequence", "planning.plan_place_sequence",
    }
    for node in _walk(document["tree"]):
        if node.get("node_id") in selector_nodes:
            role = node["parameters"]["target_role"]
            node["parameters"]["tcp_endpoint"] = {"target_role": role, "frame_role": "tcp"}
    _reseal(document)
    return document


def test_composite_and_planning_aliases_convert_before_expansion():
    document = _legacy_pick_document()
    source = deepcopy(document)
    migrated = migrate_composite_pick_place_program(
        document, build_node_environment().catalog
    ).to_dict()
    assert document == source
    nodes = _tcp_nodes(migrated)
    assert any(node["node_id"] == "planning.plan_pick_sequence" for node in nodes)
    assert all(node["parameters"]["tcp_endpoint"] == {"kind": "current"}
               for node in nodes)
    subtree = next(node for node in _walk(migrated["tree"])
                   if node["instance_id"] == "pick_left_part")
    assert [node["parameters"]["tcp_endpoint"] for node in subtree["children"]
            if node["node_id"].startswith(("motion.", "planning."))] == [
                {"kind": "current"} for _ in range(4)
            ]


def test_composite_tool_endpoint_becomes_default_of_role():
    document = _legacy_pick_document()
    for node in _tcp_nodes(document):
        node["parameters"]["tcp_endpoint"] = {"target_role": "gripper", "frame_role": "tcp"}
    planner = next(node for node in _tcp_nodes(document)
                   if node["node_id"] == "planning.plan_place_sequence")
    planner_id = planner["instance_id"]
    planner["parameters"]["tcp_endpoint"] = {"target_role": "arm", "frame_role": "base"}
    _reseal(document)
    migrated = migrate_composite_pick_place_program(
        document, build_node_environment().catalog
    ).to_dict()
    subtree = next(node for node in _walk(migrated["tree"])
                   if node["instance_id"] == "pick_left_part")
    assert all(node["parameters"]["tcp_endpoint"] == {
        "kind": "default_of_role", "role": "gripper"
    } for node in subtree["children"]
        if node["node_id"].startswith(("motion.", "planning.")))
    converted_planner = next(node for node in _walk(migrated["tree"])
                             if node["instance_id"] == planner_id)
    assert converted_planner["parameters"]["tcp_endpoint"] == {
        "kind": "entry", "target_role": "arm", "frame_role": "base"
    }


def test_ambiguous_composite_refuses_before_expansion_without_mutating_source():
    document = _legacy_pick_document()
    nodes = _tcp_nodes(document)
    other = next(node for node in nodes if node["node_id"] == "motion.move_to_pose")
    other["parameters"]["tcp_endpoint"] = {"target_role": "gripper", "frame_role": "tcp"}
    _reseal(document)
    source = deepcopy(document)
    with pytest.raises(TcpEndpointAliasAmbiguous) as refused:
        migrate_composite_pick_place_program(document, build_node_environment().catalog)
    assert refused.value.code == "tcp_endpoint_alias_ambiguous"
    assert ("pick_left_part", "arm") in refused.value.affected
    assert "pick_left_part" in str(refused.value)
    assert document == source
