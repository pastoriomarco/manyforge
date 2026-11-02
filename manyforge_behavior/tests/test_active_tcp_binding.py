"""The Current TCP input is tied to the node's captured moving role."""

import pytest

from manyforge_behavior.executable_node_resolver import _derived_active_tcp_key


@pytest.mark.parametrize("node_id", [
    "motion.move_to_pose", "planning.plan_pick_sequence", "planning.plan_place_sequence",
])
def test_current_selector_binds_role_scoped_key(node_id):
    assert _derived_active_tcp_key(node_id, {"target_role": "arm.left"}) == "active_tcp.arm.left"
    assert _derived_active_tcp_key(node_id, {
        "target_role": "arm.left", "tcp_endpoint": {"kind": "current"},
    }) == "active_tcp.arm.left"


@pytest.mark.parametrize("selector", [
    {"kind": "default_of_role", "role": "tool"},
    {"kind": "entry", "target_role": "tool", "frame_role": "tip"},
])
def test_explicit_selector_does_not_bind_current(selector):
    assert _derived_active_tcp_key("motion.move_to_pose", {
        "target_role": "arm", "tcp_endpoint": selector,
    }) is None


def test_set_tcp_current_binds_role_scoped_key():
    assert _derived_active_tcp_key("motion.set_tcp", {
        "target_role": "arm.left", "tcp": {"kind": "current"},
    }) == "active_tcp.arm.left"
    assert _derived_active_tcp_key("motion.set_tcp", {
        "target_role": "arm", "tcp": {"kind": "entry", "target_role": "arm", "frame_role": "tcp"},
    }) is None
    assert _derived_active_tcp_key("motion.set_tcp", {"target_role": "arm"}) is None


def test_non_motion_does_not_bind_current():
    assert _derived_active_tcp_key("io.set_output", {"target_role": "arm"}) is None
