"""Program version refusal happens before identity verification."""

from copy import deepcopy
from types import SimpleNamespace
from pathlib import Path

import pytest

from manyforge_behavior.program import (
    ProgramFormatUnsupported,
    ProgramValidationError,
    Program,
    _validate_node_capabilities,
    _validate_mock_simulation,
    _validate_initial_scene_pose,
    _validate_contact_selector,
    _goal_frame,
    _validate_program_document,
    _validate_semantic_value,
    _program_identifier,
    require_program_format,
    motion_tcp_selection,
    active_tcp_key,
    parse_active_tcp_key,
)
from manyforge_behavior.catalog_schema import parse_schema_registry
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.canonical_artifact import compute_content_identity


def test_v3_refuses_v2_before_identity_or_catalog_validation():
    source = {"api_version": "manyforge.program.v2", "program_revision": "invalid"}
    before = deepcopy(source)
    with pytest.raises(ProgramFormatUnsupported) as failure:
        _validate_program_document(
            source, catalog=None, source="test", api_version="manyforge.program.v3"
        )
    assert failure.value.code == "program_format_unsupported"
    assert failure.value.details == {
        "actual_api_version": "manyforge.program.v2",
        "supported_api_version": "manyforge.program.v3",
    }
    assert source == before
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    with pytest.raises(ProgramFormatUnsupported) as public_failure:
        Program.from_dict(source, catalog)
    assert public_failure.value.details == failure.value.details


def test_format_guard_accepts_v3_forest_without_mutation():
    forest = {"api_version": "manyforge.program.v3", "forest": []}
    before = deepcopy(forest)
    require_program_format(forest, supported_api_version="manyforge.program.v3")
    assert forest == before


def test_active_tcp_key_encodes_and_parses_exact_target_identity():
    key = active_tcp_key("lite6.robot", "arm.main")
    assert key == "active_tcp/lite6.robot/arm.main"
    assert parse_active_tcp_key(key) == ("lite6.robot", "arm.main")
    assert parse_active_tcp_key("active_tcp/lite6.robot/arm.main/extra") is None
    assert parse_active_tcp_key("active_tcp/lite6.robot/") is None
    assert parse_active_tcp_key("active_tcp.lite6.robot.arm.main") is None
    assert active_tcp_key("robot", "2_ARM") == "active_tcp/robot/2_ARM"
    assert parse_active_tcp_key("active_tcp/robot/2_ARM") == ("robot", "2_ARM")
    with pytest.raises(ValueError):
        active_tcp_key("lite6/robot", "arm")
    with pytest.raises(ProgramValidationError):
        _program_identifier("user/key", path="blackboard.key")


def test_reference_object_is_not_interpreted_as_a_role_identifier():
    registry = parse_schema_registry(
        {"target": {
            "type": "object",
            "semantic_type": "target_reference",
            "properties": {
                "component_id": {"type": "string"},
                "command_target_id": {"type": "string"},
            },
            "required": ["component_id", "command_target_id"],
        }}
    )
    context = SimpleNamespace(catalog=SimpleNamespace(schema_registry=registry))
    _validate_semantic_value(
        registry.types["target"],
        {"component_id": "robot", "command_target_id": "arm"},
        path="tree.parameters.target",
        context=context,
        node_parameters={},
    )


def test_active_catalog_exposes_direct_reference_parameters():
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    assert catalog.content_version == "3.0.0"
    for node_id, parameter, semantic in (
        ("motion.move_to_joint_target", "target", "target_reference"),
        ("motion.move_to_joint_target", "base_frame", "frame_reference"),
        ("io.set_output", "signal", "signal_reference"),
        ("scene.attach_carried_object", "carrier_frame_ref", "frame_reference"),
    ):
        entry = catalog.get(node_id)
        field = next(item for item in entry.parameters if item.id == parameter)
        assert catalog.schema_registry.resolve(field.schema).semantic_type == semantic


@pytest.mark.parametrize(
    ("semantic", "identity"),
    [("target_reference", "command_target_id"), ("signal_reference", "signal_name")],
)
def test_capability_selector_accepts_direct_reference(semantic, identity):
    registry = parse_schema_registry(
        {"reference": {
            "type": "object",
            "semantic_type": semantic,
            "properties": {
                "component_id": {"type": "string"},
                identity: {"type": "string"},
            },
            "required": ["component_id", identity],
        }}
    )
    owner = SimpleNamespace(_schema_registry=registry)
    parameter = SimpleNamespace(schema=registry.types["reference"])
    result = NodeCatalog._parse_capabilities(
        owner,
        [{"capability_id": "io.gpio.get_input", "scope": "target_binding",
          "target_parameter": "reference"}],
        parameters={"reference": parameter},
        label="node.required_capabilities",
    )
    assert result[0].target_parameter == "reference"


def test_direct_target_capability_is_deferred_to_shared_validator():
    requirement = SimpleNamespace(
        when_parameter=None,
        scope="target_binding",
        target_parameter="target",
        capability_id="planning.motion.pose_goal",
    )
    entry = SimpleNamespace(
        parameters=[SimpleNamespace(id="target", has_default=False)],
        required_capabilities=[requirement],
    )
    context = SimpleNamespace(
        direct_references=True,
        target_effect_count=0,
        execution_groups_supported=True,
    )
    _validate_node_capabilities(
        entry,
        {"target": {"component_id": "robot", "command_target_id": "arm"}},
        path="tree",
        context=context,
    )
    assert context.target_effect_count == 1


def test_existing_role_capability_validation_is_unchanged():
    requirement = SimpleNamespace(
        when_parameter=None,
        scope="target_binding",
        target_parameter="target_role",
        capability_id="planning.motion.pose_goal",
    )
    entry = SimpleNamespace(
        parameters=[SimpleNamespace(id="target_role", has_default=False)],
        required_capabilities=[requirement],
    )
    context = SimpleNamespace(
        direct_references=False,
        target_roles={"arm": {"required_capabilities": []}},
        target_effect_count=0,
        execution_groups_supported=True,
    )
    with pytest.raises(ProgramValidationError, match="does not declare required capability"):
        _validate_node_capabilities(entry, {"target_role": "arm"}, path="tree", context=context)


def test_mock_signal_rows_are_matched_by_direct_identity():
    rows = [{"signal": {"component_id": "station_io", "signal_name": "CI0"},
             "start_value": False}]
    _validate_mock_simulation(
        rows, frozenset({("station_io", "CI0")}), path="tree", direct_references=True
    )
    with pytest.raises(ProgramValidationError, match="not read by this node"):
        _validate_mock_simulation(
            rows, frozenset({("other_io", "CI0")}), path="tree", direct_references=True
        )


@pytest.mark.parametrize("tree", [
    {"node_id": "kernel.full_stop", "instance_id": "stop", "parameters": {}, "children": []},
    {"node_id": "io.set_output", "instance_id": "set", "parameters": {
        "signal": {"component_id": "robot_io", "signal_name": "TO0"}, "value": True
    }, "children": []},
    {"node_id": "motion.move_to_joint_target", "instance_id": "move", "parameters": {
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "base_frame": {"component_id": "robot", "frame_id": "base"},
        "goal": [{"joint_name": "joint1", "position_rad": 0.0}],
        "tcp_endpoint": {"kind": "current"},
    }, "children": []},
    {"node_id": "motion.move_to_pose", "instance_id": "pose", "parameters": {
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "base_frame": {"component_id": "robot", "frame_id": "base"},
        "goal": {"frame_ref": {"kind": "target_frame", "frame": {
            "component_id": "robot", "frame_id": "base"
        }}, "position_m": [0.0, 0.0, 0.0], "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}},
        "tcp_endpoint": {"kind": "entry", "target": {
            "component_id": "robot", "command_target_id": "arm"
        }, "frame": {"component_id": "tool", "frame_id": "tcp"}},
    }, "children": []},
])
def test_v3_program_root_loads_without_role_declarations(tree):
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    document = {
        "api_version": "manyforge.program.v3",
        "program_id": "simple",
        "program_revision": "sha256:" + "0" * 64,
        "program_profile": "composable-cell-v1",
        "name": "Simple",
        "catalog": {"api_version": catalog.api_version, "content_version": catalog.content_version},
        "tree": tree,
    }
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version="manyforge.program.v3"
    )
    normalized, *_ = _validate_program_document(
        document, catalog=catalog, source="test", api_version="manyforge.program.v3"
    )
    assert "component_roles" not in normalized
    assert "target_roles" not in normalized
    assert Program.from_dict(document, catalog).to_dict() == normalized
    if tree["node_id"] in {"motion.move_to_joint_target", "motion.move_to_pose"}:
        assert "active_tcp/robot/arm" in Program.from_dict(document, catalog).blackboard_keys


def test_direct_tcp_selection_keeps_current_and_target_default_distinct():
    assert motion_tcp_selection({}, direct_references=True) == {"tcp_endpoint": {"kind": "current"}}
    selector = {"kind": "default_of_target", "target": {
        "component_id": "gripper", "command_target_id": "gripper"
    }}
    assert motion_tcp_selection({"tcp_endpoint": selector}, direct_references=True) == {
        "tcp_endpoint": selector
    }
    mounted = {"kind": "entry", "target": {
        "component_id": "robot", "command_target_id": "arm"
    }, "frame": {"component_id": "tool", "frame_id": "tcp"}}
    assert motion_tcp_selection({"tcp_endpoint": mounted}, direct_references=True) == {
        "tcp_endpoint": mounted
    }
    with pytest.raises(ProgramValidationError):
        motion_tcp_selection(
            {"tcp_endpoint": {"kind": "current", "target": selector["target"]}},
            direct_references=True,
        )


def test_scene_and_contact_selectors_use_component_owned_features():
    _validate_initial_scene_pose(
        {"frame_ref": {"kind": "component_frame", "frame": {
            "component_id": "robot", "frame_id": "base"
        }}, "position_m": [0.0, 0.0, 0.0], "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}},
        path="scene[0].pose", component_roles={}, direct_references=True,
    )
    kind, _ = _validate_contact_selector(
        {"kind": "component_model_body", "body": {
            "component_id": "gripper", "collision_body_id": "gripper_collision"
        }},
        path="contact.body", component_roles={}, target_roles={}, blackboard_keys={},
        direct_references=True,
    )
    assert kind == "component_model_body"


def test_direct_goal_frame_recognizes_only_the_selected_base():
    base = {"component_id": "robot", "frame_id": "base"}
    pose = {"frame_ref": {"kind": "target_frame", "frame": base}}
    assert _goal_frame(pose, {"base_frame": base}, direct_references=True) is None
    assert _goal_frame(
        pose,
        {"base_frame": {"component_id": "other", "frame_id": "base"}},
        direct_references=True,
    ) == pose["frame_ref"]


