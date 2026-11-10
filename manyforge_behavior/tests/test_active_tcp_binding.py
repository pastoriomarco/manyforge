"""The Current TCP input is tied to the node's captured moving target."""

import pytest

from manyforge_behavior.executable_node_resolver import _derived_active_tcp_key


_LEFT = {"component_id": "robot.left", "command_target_id": "arm.left"}
_ARM = {"component_id": "robot", "command_target_id": "arm"}


@pytest.mark.parametrize("node_id", [
    "motion.move_to_pose", "planning.plan_pick_sequence", "planning.plan_place_sequence",
])
def test_current_selector_binds_target_scoped_key(node_id):
    assert _derived_active_tcp_key(node_id, {"target": _LEFT}) == "active_tcp/robot.left/arm.left"
    assert _derived_active_tcp_key(node_id, {
        "target": _LEFT, "tcp_endpoint": {"kind": "current"},
    }) == "active_tcp/robot.left/arm.left"


@pytest.mark.parametrize("selector", [
    {"kind": "default_of_target", "target": {"component_id": "tool", "command_target_id": "gripper"}},
    {"kind": "entry", "target": {"component_id": "tool", "command_target_id": "gripper"},
     "frame": {"component_id": "tool", "frame_id": "tip"}},
])
def test_explicit_selector_does_not_bind_current(selector):
    assert _derived_active_tcp_key("motion.move_to_pose", {
        "target": _ARM, "tcp_endpoint": selector,
    }) is None


def test_set_tcp_current_binds_target_scoped_key():
    assert _derived_active_tcp_key("motion.set_tcp", {
        "target": _LEFT, "tcp": {"kind": "current"},
    }) == "active_tcp/robot.left/arm.left"
    assert _derived_active_tcp_key("motion.set_tcp", {
        "target": _ARM, "tcp": {"kind": "entry", "target": _ARM,
                                "frame": {"component_id": "robot", "frame_id": "tcp"}},
    }) is None
    assert _derived_active_tcp_key("motion.set_tcp", {"target": _ARM}) is None


def test_non_motion_does_not_bind_current():
    assert _derived_active_tcp_key("io.set_output", {"target": _ARM}) is None
