"""Direct-reference Pick/Place macro caller contracts."""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_composer.backend.draft_session import ComposerDraftSession, DraftSessionError


_ROOT = Path(__file__).resolve().parents[2]


def _arguments():
    return {
        "macro_id": "pick_subtree", "scene_resource_id": "part",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "tool_target": {"component_id": "tool", "command_target_id": "gripper"},
        "base_frame": {"component_id": "robot", "frame_id": "base"},
        "carrier_frame_ref": {"component_id": "tool", "frame_id": "tcp"},
        "tcp_endpoint": {"kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"},
                         "frame": {"component_id": "tool", "frame_id": "tcp"}},
        "touch_bodies": [{"component_id": "tool", "collision_body_id": "fingers"}],
        "tool_position": {"kind": "linear_m", "value_m": 0.0},
        "contact_policy_id": "grasp", "blackboard_namespace_id": "planning",
        "blackboard_key": "planning.pick", "instance_prefix": "pick",
    }


def _document():
    return {"scene": [{"scene_resource_id": "part"}],
            "contact_policies": [{"policy_id": "grasp"}],
            "forest": [], "positions": {}}


def test_macro_preserves_direct_references_and_mounted_tcp():
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    session = object.__new__(ComposerDraftSession)
    document, arguments = _document(), _arguments()
    session._authoring_macro_expand(document, arguments, catalog)
    children = document["forest"][0]["children"]
    assert children[0]["parameters"]["target"] == arguments["target"]
    assert children[0]["parameters"]["tcp_endpoint"] == arguments["tcp_endpoint"]
    assert children[4]["parameters"]["target"] == arguments["tool_target"]
    assert children[4]["parameters"]["touch_bodies"] == arguments["touch_bodies"]


def test_place_macro_preserves_direct_frame_and_targets():
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    session = object.__new__(ComposerDraftSession)
    document, arguments = _document(), _arguments()
    arguments.update(
        macro_id="place_subtree", instance_prefix="place", blackboard_key="planning.place",
        place_pose={"frame_ref": {"kind": "cell_frame"}, "position_m": [0.0, 0.0, 0.0],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}},
        approach_offset_m=[0.0, 0.0, 0.1],
    )
    arguments.pop("contact_policy_id")
    arguments.pop("touch_bodies")
    session._authoring_macro_expand(document, arguments, catalog)
    children = document["forest"][0]["children"]
    assert children[0]["parameters"]["target"] == arguments["target"]
    assert children[0]["parameters"]["carrier_frame_ref"] == arguments["carrier_frame_ref"]
    assert children[4]["parameters"]["target"] == arguments["tool_target"]


@pytest.mark.parametrize("field,value", [
    ("target", {"component_id": "robot"}),
    ("base_frame", {"component_id": "robot", "frame_id": "base", "extra": "x"}),
    ("touch_bodies", [{"component_id": "tool", "collision_body_id": "fingers"}] * 2),
    ("tcp_endpoint", {"kind": "default_of_role", "role": "arm"}),
])
def test_macro_refuses_malformed_refs_without_insertion(field, value):
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    session = object.__new__(ComposerDraftSession)
    document, arguments = _document(), _arguments()
    before = deepcopy(document)
    arguments[field] = value
    with pytest.raises(DraftSessionError):
        session._authoring_macro_expand(document, arguments, catalog)
    assert document == before
