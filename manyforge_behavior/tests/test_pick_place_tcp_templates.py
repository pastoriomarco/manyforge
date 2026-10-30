"""TCP selector defaults and independence in Pick/Place expansion."""

from copy import deepcopy

import pytest

from manyforge_behavior.pick_place_templates import (
    PickPlaceTemplateError,
    build_pick_place_subtree,
)


def _parameters(kind):
    values = {
        "target_role": "arm",
        "tool_target_role": "gripper",
        "base_frame_role": "base",
        "carrier_frame_role": "carrier",
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
    {"kind": "default_of_role", "role": "gripper"},
    {"kind": "entry", "target_role": "gripper", "frame_role": "tcp"},
])
def test_template_gives_planner_and_moves_independent_equal_selectors(kind, selector):
    parameters = _parameters(kind)
    if selector is not None:
        parameters["tcp_endpoint"] = deepcopy(selector)
    subtree = build_pick_place_subtree(
        kind, instance_id=kind, parameters=parameters, value_sources={},
        result_key="trial.result", touch_body_roles=["fingers"] if kind == "pick" else (),
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
            result_key="trial.result", touch_body_roles=["fingers"],
        )
