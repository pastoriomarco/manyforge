"""TCP selector defaults and independence in Pick/Place expansion."""

from copy import deepcopy

import pytest

from manyforge_behavior.pick_place_templates import (
    PickPlaceTemplateError,
    build_pick_place_subtree,
)


def _parameters(kind):
    values = {
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "tool_target": {"component_id": "tool", "command_target_id": "gripper"},
        "base_frame": {"component_id": "robot", "frame_id": "base"},
        "carrier_frame_ref": {"component_id": "tool", "frame_id": "carrier"},
        "selected_object_id": "part",
        "tool_position": {"kind": "named_preset", "name": "closed"},
    }
    if kind == "pick":
        values["contact_policy_id"] = "pick.contact"
    else:
        values["place_pose"] = {"position_m": [0, 0, 0], "orientation": {"xyzw": [0, 0, 0, 1]}}
    return values


@pytest.mark.parametrize("kind", ["pick", "place"])
@pytest.mark.parametrize("selector", [
    None,
    {"kind": "current"},
    {"kind": "default_of_target", "target": {"component_id": "robot", "command_target_id": "arm"}},
    {"kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"}, "frame": {"component_id": "tool", "frame_id": "tcp"}},
])
def test_template_gives_planner_and_moves_independent_equal_selectors(kind, selector):
    parameters = _parameters(kind)
    if selector is not None:
        parameters["tcp_endpoint"] = deepcopy(selector)
    subtree = build_pick_place_subtree(
        kind, instance_id=kind, parameters=parameters, value_sources={},
        result_key="trial.result", touch_bodies=[{"component_id": "tool", "collision_body_id": "fingers"}] if kind == "pick" else (),
        wrapper_fields={"operator_stop_policy": "hold"},
    )
    relevant = [child for child in subtree["children"]
                if child["node_id"].startswith(("planning.", "motion."))]
    assert len(relevant) == 4
    expected = selector or {"kind": "current"}
    assert all(child["parameters"]["tcp_endpoint"] == expected for child in relevant)
    assert subtree["operator_stop_policy"] == "hold"
    assert all("operator_stop_policy" not in child for child in subtree["children"])
    relevant[0]["parameters"]["tcp_endpoint"]["kind"] = "changed"
    assert all(child["parameters"]["tcp_endpoint"] == expected for child in relevant[1:])
    if selector is not None:
        assert parameters["tcp_endpoint"] == selector


def test_template_rejects_dynamic_tcp_selector():
    parameters = _parameters("pick")
    with pytest.raises(PickPlaceTemplateError, match="selectors cannot be dynamic"):
        build_pick_place_subtree(
            "pick", instance_id="pick", parameters=parameters,
            value_sources={"tcp_endpoint": {"kind": "blackboard_key", "reference": "tcp"}},
            result_key="trial.result", touch_bodies=[{"component_id": "tool", "collision_body_id": "fingers"}],
        )


@pytest.mark.parametrize("kind", ["pick", "place"])
def test_template_maps_tool_wait_for_duration_to_the_gripper_node(kind):
    parameters = _parameters(kind)
    parameters.update(tool_command_duration_s=0.8, tool_wait_for_duration=True)
    subtree = build_pick_place_subtree(
        kind, instance_id=kind, parameters=parameters, value_sources={},
        result_key="trial.result", touch_bodies=[{"component_id": "tool", "collision_body_id": "fingers"}] if kind == "pick" else (),
    )
    [gripper] = [child for child in subtree["children"]
                 if child["node_id"] == "gripper.command_position"]
    assert gripper["parameters"]["command_duration_s"] == 0.8
    assert gripper["parameters"]["wait_for_duration"] is True


def test_pick_attachment_forwards_selected_tool_frame_and_bodies():
    parameters = _parameters("pick")
    body = {"component_id": "tool", "collision_body_id": "fingers"}
    subtree = build_pick_place_subtree(
        "pick", instance_id="pick", parameters=parameters, value_sources={},
        result_key="trial.result", touch_bodies=[body],
    )
    [attach] = [child for child in subtree["children"] if child["node_id"] == "scene.attach_carried_object"]
    assert attach["parameters"]["target"] == parameters["tool_target"]
    assert attach["parameters"]["carrier_frame_ref"] == parameters["carrier_frame_ref"]
    assert attach["parameters"]["touch_bodies"] == [body]
