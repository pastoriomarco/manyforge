"""Portable legacy TCP conversion and ambiguous Load refusal."""

from copy import deepcopy

from manyforge_behavior.program import TcpEndpointAliasAmbiguous
from manyforge_behavior.tcp_migration import migrate_tcp_selectors


def _node(instance, moving, owner, frame, node_id="motion.move_to_pose"):
    return {
        "instance_id": instance,
        "node_id": node_id,
        "parameters": {
            "target_role": moving,
            "tcp_endpoint": {"target_role": owner, "frame_role": frame},
        },
    }


def test_single_robot_alias_converts_to_current():
    nodes = [_node("first", "arm", "arm", "tcp"), _node("second", "arm", "arm", "tcp")]
    assert migrate_tcp_selectors(nodes) == ()
    assert [node["parameters"]["tcp_endpoint"] for node in nodes] == [
        {"kind": "current"}, {"kind": "current"}
    ]


def test_mixed_alias_refuses_all_conversion_and_names_each_move():
    nodes = [
        _node("move.one", "arm", "arm", "tcp"),
        _node("move.two", "arm", "arm", "tcp"),
        _node("tool", "arm", "gripper", "tcp"),
        _node("explicit", "arm", "gripper", "probe"),
    ]
    original = deepcopy(nodes)
    affected = migrate_tcp_selectors(nodes)
    assert affected == (("move.one", "arm"), ("move.two", "arm"))
    assert nodes == original
    error = TcpEndpointAliasAmbiguous(affected)
    assert error.code == "tcp_endpoint_alias_ambiguous"
    assert "move move.one on role arm" in str(error)
    assert "move move.two on role arm" in str(error)


def test_tool_alias_and_named_frame_convert():
    nodes = [_node("tool", "arm", "gripper", "tcp"),
             _node("explicit", "arm", "gripper", "probe")]
    assert migrate_tcp_selectors(nodes) == ()
    assert [node["parameters"]["tcp_endpoint"] for node in nodes] == [
        {"kind": "default_of_role", "role": "gripper"},
        {"kind": "entry", "target_role": "gripper", "frame_role": "probe"},
    ]


def test_new_and_malformed_selectors_are_not_rewritten():
    nodes = [_node("one", "arm", "arm", "tcp"), _node("two", "arm", "arm", "tcp")]
    nodes[0]["parameters"]["tcp_endpoint"] = {"kind": "current"}
    nodes[1]["parameters"]["tcp_endpoint"]["extra"] = "x"
    original = deepcopy(nodes)
    assert migrate_tcp_selectors(nodes) == ()
    assert nodes == original


def test_two_robots_each_with_own_alias_convert_independently():
    nodes = [_node("lite", "lite6_arm", "lite6_arm", "tcp"),
             _node("ur", "ur5e_arm", "ur5e_arm", "tcp")]
    assert migrate_tcp_selectors(nodes) == ()
    assert [node["parameters"]["tcp_endpoint"] for node in nodes] == [
        {"kind": "current"}, {"kind": "current"}
    ]


def test_planning_and_composites_participate_in_same_robot_check():
    nodes = [
        _node("plan", "arm", "arm", "tcp", "planning.plan_pick_sequence"),
        _node("pick", "arm", "gripper", "tcp", "motion.pick_object"),
        _node("ur", "ur5e_arm", "ur5e_arm", "tcp"),
    ]
    original = deepcopy(nodes)
    assert migrate_tcp_selectors(nodes) == (("plan", "arm"),)
    assert nodes == original


def test_all_roles_drop_unused_tcp_requirements():
    nodes = [_node("robot", "arm", "arm", "tcp"),
             _node("tool", "hand", "hand", "tcp")]
    document = {
        "tree": {"children": nodes},
        "target_roles": {
            "arm": {"required_frame_roles": ["base", "tcp"]},
            "hand": {"required_frame_roles": ["tcp", "tip"]},
            "other": {"required_frame_roles": ["tcp"]},
        },
    }
    assert migrate_tcp_selectors(nodes, document) == ()
    assert document["target_roles"] == {
        "arm": {"required_frame_roles": ["base"]},
        "hand": {"required_frame_roles": ["tip"]},
        "other": {"required_frame_roles": []},
    }


def test_explicit_tcp_reference_preserves_its_role_only():
    nodes = [_node("robot", "arm", "arm", "tcp"),
             _node("tool", "hand", "hand", "tcp")]
    document = {
        "tree": {"children": nodes},
        "blackboard": {"allowed_frames": [
            {"kind": "target_frame", "target_role": "arm", "frame_role": "tcp"}
        ]},
        "target_roles": {"arm": {"required_frame_roles": ["tcp"]},
                         "hand": {"required_frame_roles": ["tcp"]}},
    }
    assert migrate_tcp_selectors(nodes, document) == ()
    assert document["target_roles"]["arm"]["required_frame_roles"] == ["tcp"]
    assert document["target_roles"]["hand"]["required_frame_roles"] == []


def test_unscoped_or_sourced_tcp_literal_conservatively_preserves_requirements():
    nodes = [_node("robot", "arm", "arm", "tcp")]
    document = {"tree": {"children": nodes},
                "parameters": [{"default": "tcp"}],
                "target_roles": {"arm": {"required_frame_roles": ["tcp"]}}}
    assert migrate_tcp_selectors(nodes, document) == ()
    assert document["target_roles"]["arm"]["required_frame_roles"] == ["tcp"]


def test_unrestricted_dynamic_pose_keeps_requirements_without_tcp_literal():
    nodes = [_node("robot", "arm", "arm", "tcp")]
    document = {"tree": {"children": nodes},
                "blackboard": {"keys": [{"key": "pose", "type": "stored_pose"}]},
                "target_roles": {"arm": {"required_frame_roles": ["tcp"]},
                                 "hand": {"required_frame_roles": ["tcp"]}}}
    assert migrate_tcp_selectors(nodes, document) == ()
    assert document["target_roles"]["arm"]["required_frame_roles"] == ["tcp"]
    assert document["target_roles"]["hand"]["required_frame_roles"] == ["tcp"]
    before = deepcopy(document)
    assert migrate_tcp_selectors(nodes, document) == ()
    assert document == before
