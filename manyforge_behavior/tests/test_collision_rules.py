"""The collision rule row shape shared by motion nodes and the Deployment."""

from __future__ import annotations

from copy import deepcopy

import pytest

from manyforge_behavior.collision_rules import (
    CollisionRuleError,
    release_with_defaults,
    sent_collision_rules,
    validate_collision_rules,
)


_PAIR = {
    "a": [{"kind": "collision_body", "component_id": "gripper", "collision_body_id": "gripper_pads"}],
    "b": [{"kind": "attached_object", "component_id": "gripper"}],
}
_MOVE_PAIR = {
    "a": [{"kind": "active_tcp_contact_bodies"}],
    "b": [{"kind": "scene_object", "scene_resource_id": "part_1"}],
}


def _reason(rules, **options) -> str:
    with pytest.raises(CollisionRuleError) as refused:
        validate_collision_rules(rules, path="collision_rules", **options)
    return refused.value.reason


def test_deployment_rules_start_with_the_row_without_selections() -> None:
    rules = [
        {"rule_id": "everything", "clearance_m": 0.001},
        {"rule_id": "pads_on_part", **_PAIR, "contact": {"max_penetration_m": 0.001}},
        {"rule_id": "arms_apart",
         "a": [{"kind": "component", "component_id": "robot_a"},
               {"kind": "link", "component_id": "robot_a", "link": "link5"}],
         "b": [{"kind": "component", "component_id": "robot_b"}, {"kind": "scene_objects"}],
         "clearance_m": 0.02},
    ]
    before = deepcopy(rules)
    validate_collision_rules(rules, path="collision_rules", move_rules=False)
    assert rules == before

    assert _reason([], move_rules=False) == "collision_rule_selection_invalid"
    assert _reason(rules[1:], move_rules=False) == "collision_rule_selection_invalid"
    assert _reason([rules[1], rules[0]], move_rules=False) == "collision_rule_selection_invalid"


@pytest.mark.parametrize("selection", [
    {"kind": "scene_object", "scene_resource_id": "part_1"},
    {"kind": "scene_object", "blackboard_key": "trial.part"},
    {"kind": "active_tcp_contact_bodies"},
])
def test_move_only_selections_are_refused_in_a_deployment(selection) -> None:
    row = {"rule_id": "tip", "a": [selection], "b": _PAIR["b"], "contact": {}}
    validate_collision_rules([row], path="collision_rules", move_rules=True)
    assert _reason(
        [{"rule_id": "everything", "clearance_m": 0.001}, row], move_rules=False
    ) == "collision_rule_selection_invalid"


def test_model_groups_are_deployment_selections_only() -> None:
    group = {"kind": "model_group", "component_id": "robot_a", "group": "manipulator"}
    row = {"rule_id": "arm_scene", "a": [group], "b": [{"kind": "scene_objects"}],
           "clearance_m": 0.01}
    validate_collision_rules(
        [{"rule_id": "everything", "clearance_m": 0.001}, row],
        path="collision_rules", move_rules=False,
    )
    assert _reason([row], move_rules=True) == "collision_rule_selection_invalid"
    missing = {**row, "a": [{"kind": "model_group", "component_id": "robot_a"}]}
    assert _reason(
        [{"rule_id": "everything", "clearance_m": 0.001}, missing], move_rules=False
    ) == "collision_rule_selection_invalid"


def test_move_rules_are_optional_and_take_one_row_without_selections() -> None:
    validate_collision_rules([], path="collision_rules", move_rules=True)
    validate_collision_rules(
        [{"rule_id": "tip", **_MOVE_PAIR, "contact": {"end_from_fraction": 0.8}},
         {"rule_id": "everything_else", "clearance_m": 0.0}],
        path="collision_rules", move_rules=True,
    )
    assert _reason(
        [{"rule_id": "one", "clearance_m": 0.0}, {"rule_id": "two", "clearance_m": 0.001}],
        move_rules=True,
    ) == "collision_rule_selection_invalid"


@pytest.mark.parametrize("rules,reason", [
    ("not a list", "collision_rule_value_invalid"),
    ([{"rule_id": f"r{index}", **_PAIR, "contact": {}} for index in range(65)],
     "collision_rules_bound_exceeded"),
    ([{"rule_id": "", "clearance_m": 0.001}], "collision_rule_id_invalid"),
    ([{"rule_id": "x" * 129, "clearance_m": 0.001}], "collision_rule_id_invalid"),
    ([{"rule_id": "same", "clearance_m": 0.001}, {"rule_id": "same", **_PAIR, "contact": {}}],
     "collision_rule_id_duplicate"),
    ([{"rule_id": "half", "a": _PAIR["a"], "contact": {}}], "collision_rule_selection_invalid"),
    ([{"rule_id": "empty", "a": [], "b": _PAIR["b"], "contact": {}}],
     "collision_rule_selection_invalid"),
    ([{"rule_id": "many", "a": _PAIR["a"] * 17, "b": _PAIR["b"], "contact": {}}],
     "collision_rule_selection_invalid"),
    ([{"rule_id": "kind", "a": [{"kind": "model_group", "component_id": "robot", "group": "arm"}],
       "b": _PAIR["b"], "contact": {}}], "collision_rule_selection_invalid"),
    ([{"rule_id": "extra", "a": [{"kind": "scene_objects", "link": "link5"}], "b": _PAIR["b"],
       "contact": {}}], "collision_rule_selection_invalid"),
    ([{"rule_id": "missing", "a": [{"kind": "link", "component_id": "robot"}], "b": _PAIR["b"],
       "contact": {}}], "collision_rule_selection_invalid"),
    ([{"rule_id": "field", "a": [{"kind": "scene_object", "scene_resource_id": "part_1",
                                  "field": "selected_object_id"}],
       "b": _PAIR["b"], "contact": {}}], "collision_rule_selection_invalid"),
    ([{"rule_id": "none", **_PAIR}], "collision_rule_value_invalid"),
    ([{"rule_id": "two", **_PAIR, "contact": {}, "release": {}}], "collision_rule_value_invalid"),
    ([{"rule_id": "all", "contact": {}}], "collision_rule_value_invalid"),
    ([{"rule_id": "wide", "clearance_m": 0.0501}], "collision_rule_value_invalid"),
    ([{"rule_id": "bool", "clearance_m": True}], "collision_rule_value_invalid"),
    ([{"rule_id": "nan", "clearance_m": float("nan")}], "collision_rule_value_invalid"),
    ([{"rule_id": "deep", **_PAIR, "contact": {"max_penetration_m": -1e-6}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "late", **_PAIR, "contact": {"end_from_fraction": 1.01}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "key", **_PAIR, "contact": {"during": "end"}}], "collision_rule_value_invalid"),
    # OD-356: a spring's step limit needs its stroke and a value above 0.
    ([{"rule_id": "spring", **_PAIR, "contact": {"max_compression_step_m": 0.002}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "spring", **_PAIR,
       "contact": {"max_penetration_m": 0.01, "max_compression_step_m": 0}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "mode", **_PAIR, "release": {"release_evidence_mode": "distance"}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "binary", **_PAIR, "release": {"release_evidence_mode": "binary_contact",
                                                 "max_start_penetration_m": 0.001}}],
     "collision_rule_value_invalid"),
    ([{"rule_id": "leg", "leg": "descend", **_PAIR, "contact": {}}],
     "collision_rule_value_invalid"),
])
def test_malformed_rows_are_refused_with_their_reason(rules, reason) -> None:
    assert _reason(rules, move_rules=True) == reason


def test_sequence_rows_name_their_leg_and_have_one_row_without_selections() -> None:
    rules = [
        {"rule_id": "grasp", "leg": "descend", **_MOVE_PAIR, "contact": {"end_from_fraction": 0.8}},
        {"rule_id": "near", "leg": "descend", "clearance_m": 0.0},
    ]
    validate_collision_rules(rules, path="collision_rules", move_rules=True, legs=True)
    assert _reason([{**rules[0], "leg": "tool"}], move_rules=True, legs=True) == (
        "collision_rule_value_invalid"
    )
    assert _reason(
        [{key: value for key, value in rules[0].items() if key != "leg"}],
        move_rules=True, legs=True,
    ) == "collision_rule_value_invalid"
    assert _reason(
        [rules[1], {"rule_id": "again", "leg": "approach", "clearance_m": 0.001}],
        move_rules=True, legs=True,
    ) == "collision_rule_selection_invalid"


def test_release_defaults_follow_the_evidence_mode() -> None:
    assert release_with_defaults({}) == {
        "release_evidence_mode": "signed_penetration",
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.0,
        "min_release_progress_m": 0.0,
        "release_tolerance_m": 1e-5,
        "require_strictly_decreasing_penetration": True,
        "require_separation_by_end": True,
        "allow_recontact": False,
    }
    assert release_with_defaults(
        {"release_evidence_mode": "binary_contact", "allow_recontact": True}
    ) == {
        "release_evidence_mode": "binary_contact",
        "enforce_max_start_penetration": False,
        "max_start_penetration_m": 0.0,
        "min_release_progress_m": 0.0,
        "release_tolerance_m": 0.0,
        "require_strictly_decreasing_penetration": False,
        "require_separation_by_end": True,
        "allow_recontact": True,
    }


def test_sent_rows_drop_the_leg_and_spell_out_a_release() -> None:
    rules = [
        {"rule_id": "grasp", "leg": "descend", **_MOVE_PAIR,
         "contact": {"max_penetration_m": 0.002}},
        {"rule_id": "lift", "leg": "retreat", **_PAIR, "release": {"allow_recontact": True}},
        {"rule_id": "near", "leg": "descend", "clearance_m": 0},
    ]
    assert sent_collision_rules(rules, leg="approach") == []
    assert sent_collision_rules(rules, leg="descend") == [
        {"rule_id": "grasp", **_MOVE_PAIR, "contact": {"max_penetration_m": 0.002}},
        {"rule_id": "near", "clearance_m": 0.0},
    ]
    [lift] = sent_collision_rules(rules, leg="retreat")
    assert lift["release"] == {**release_with_defaults({}), "allow_recontact": True}
    assert set(lift) == {"rule_id", "a", "b", "release"}


def test_a_spring_contact_row_is_accepted() -> None:
    # OD-356: stroke 10 mm, at most 2 mm deeper per 1 mm sample.
    validate_collision_rules(
        [{"rule_id": "holder", **_PAIR,
          "contact": {"max_penetration_m": 0.01, "max_compression_step_m": 0.002,
                      "end_from_fraction": 0}}],
        path="collision_rules", move_rules=True)


# OD-371: a move switches a generated tool's spring slider on.
_SLIDER_ROW = {
    "rule_id": "tube_into_endplate",
    "spring_slider": {"component_id": "tube_holder", "element_id": "holder_rod"},
    "b": [{"kind": "scene_object", "scene_resource_id": "endplate"}],
    "contact": {"end_from_fraction": 0},
}
_SLIDERS = {
    ("tube_holder", "holder_rod"): {
        "body": {"component_id": "tube_holder", "collision_body_id": "holder_rod"},
        "axis": [0.0, 0.0, 1.0], "stroke_m": 0.02, "max_compression_step_m": 0.002,
    },
}


def test_a_spring_slider_row_is_a_move_rule_with_b_and_an_end_window() -> None:
    validate_collision_rules([_SLIDER_ROW], path="collision_rules", move_rules=True)
    without_contact = {key: value for key, value in _SLIDER_ROW.items() if key != "contact"}
    validate_collision_rules([without_contact], path="collision_rules", move_rules=True)
    validate_collision_rules(
        [{**_SLIDER_ROW, "leg": "descend"}], path="collision_rules", move_rules=True, legs=True
    )
    assert _reason(
        [{"rule_id": "everything", "clearance_m": 0.001}, _SLIDER_ROW], move_rules=False
    ) == "collision_rule_selection_invalid"


@pytest.mark.parametrize("row,reason", [
    ({**_SLIDER_ROW, "a": _PAIR["a"]}, "collision_rule_selection_invalid"),
    ({key: value for key, value in _SLIDER_ROW.items() if key != "b"},
     "collision_rule_selection_invalid"),
    ({**_SLIDER_ROW, "spring_slider": {"component_id": "tube_holder"}},
     "collision_rule_selection_invalid"),
    ({**_SLIDER_ROW, "contact": {"max_penetration_m": 0.01}}, "collision_rule_value_invalid"),
    ({**_SLIDER_ROW, "contact": {"end_from_fraction": 2}}, "collision_rule_value_invalid"),
    ({key: value for key, value in _SLIDER_ROW.items() if key != "contact"}
     | {"clearance_m": 0.0}, "collision_rule_value_invalid"),
])
def test_malformed_spring_slider_rows_are_refused(row, reason) -> None:
    assert _reason([row], move_rules=True) == reason


def test_an_attached_object_may_name_the_body_that_carries_it() -> None:
    carried = {"kind": "attached_object", "component_id": "tube_holder",
               "collision_body_role_id": "holder_rod"}
    validate_collision_rules(
        [{"rule_id": "tube", "a": [carried], "b": _MOVE_PAIR["b"], "contact": {}}],
        path="collision_rules", move_rules=True,
    )
    assert _reason(
        [{"rule_id": "tube", "a": [{**carried, "collision_body_role_id": ""}],
          "b": _MOVE_PAIR["b"], "contact": {}}], move_rules=True,
    ) == "collision_rule_selection_invalid"


def test_a_sent_spring_slider_row_is_the_slider_and_its_objects_as_one_spring() -> None:
    assert sent_collision_rules([_SLIDER_ROW], spring_sliders=_SLIDERS) == [{
        "rule_id": "tube_into_endplate",
        "a": [
            {"kind": "collision_body", "component_id": "tube_holder",
             "collision_body_id": "holder_rod"},
            {"kind": "attached_object", "component_id": "tube_holder",
             "collision_body_role_id": "holder_rod"},
        ],
        "b": [{"kind": "scene_object", "scene_resource_id": "endplate"}],
        "contact": {
            "end_from_fraction": 0.0,
            "max_penetration_m": 0.02,
            "max_compression_step_m": 0.002,
            "spring_group": {"component_id": "tube_holder", "element_id": "holder_rod",
                             "axis": [0.0, 0.0, 1.0]},
        },
    }]
    # Without an end window the contact default applies at the execution host.
    [sent] = sent_collision_rules(
        [{key: value for key, value in _SLIDER_ROW.items() if key != "contact"}],
        spring_sliders=_SLIDERS,
    )
    assert "end_from_fraction" not in sent["contact"]
    with pytest.raises(CollisionRuleError) as refused:
        sent_collision_rules([_SLIDER_ROW], spring_sliders={})
    assert refused.value.reason == "collision_rule_spring_slider_unresolved"
