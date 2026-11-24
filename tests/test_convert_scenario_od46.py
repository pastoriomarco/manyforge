"""Tests for scripts/convert_scenario_od46.py (Scenario files -> collision rules)."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import uuid

import pytest
import yaml


_SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "convert_scenario_od46.py"


def _module():
    name = f"convert_scenario_od46_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(name, _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
                    encoding="utf-8")


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _move(instance_id: str, **parameters) -> dict:
    return {"children": [], "instance_id": instance_id, "node_id": "motion.move_to_pose",
            "parameters": {"target": {"command_target_id": "arm", "component_id": "robot"},
                           **parameters}}


def _program(nodes: list[dict], **extra) -> dict:
    return {
        "api_version": "manyforge.program.v3",
        "catalog": {"api_version": "manyforge.node_catalog.v3", "content_version": "3.0.0"},
        "forest": [{"children": nodes, "instance_id": "root", "node_id": "sequence",
                    "parameters": {}}],
        "name": "demo",
        "program_id": "demo",
        "scene": [],
        **extra,
    }


def _deployment(robot_runtime: dict | None = None) -> dict:
    return {
        "api_version": "manyforge.deployment_intent.v2",
        "attachments": [{"child": {"component_id": "gripper", "interface": "mount"},
                         "parent": {"component_id": "robot", "interface": "flange"}}],
        "components": [
            {"component_id": "robot",
             "configuration": {"runtime_kind": "ros2_control",
                               "runtime_configuration": dict(robot_runtime or {})}},
            {"component_id": "gripper",
             "configuration": {"runtime_kind": "ros2_control", "runtime_configuration": {
                 "gripper_target_configurations": [
                     {"tcp_frame": "tcp", "touch_links": ["base", "left", "right"]}]}}},
        ],
        "name": "demo",
    }


def _policy(policy_id: str, mode: str, robot_end: dict, **zone) -> dict:
    return {
        "api_version": "manyforge.contact_policy.v1", "policy_id": policy_id, "mode": mode,
        "sensing_mode": "geometry_only", "default_unmatched_response": "hard_stop",
        "version": "sha256:" + "0" * 64,
        "zones": [{"zone_id": "zone", "violation_response": "hard_stop",
                   "body_pairs": [{"body_a": robot_end,
                                   "body_b": {"kind": "runtime_object",
                                              "runtime_object_id": "picked"}}],
                   **zone}],
    }


def _nodes(program: dict) -> dict[str, dict]:
    found: dict[str, dict] = {}

    def visit(node: dict) -> None:
        found[node["instance_id"]] = node
        for child in node["children"]:
            visit(child)

    for root in program["forest"]:
        visit(root)
    return found


def test_deployment_clearance_becomes_the_first_rule(tmp_path: Path) -> None:
    project = tmp_path / "demo.scenario"
    project.mkdir()
    _write_json(project / "deployment.yaml", _deployment({"collision_clearance_m": 0.003}))
    assert _module().main([str(project)]) == 0
    converted = _read_json(project / "deployment.yaml")
    assert converted["api_version"] == "manyforge.deployment_intent.v3"
    assert converted["collision_rules"] == [{"clearance_m": 0.003, "rule_id": "everything"}]
    runtimes = [component["configuration"]["runtime_configuration"]
                for component in converted["components"]]
    assert "collision_clearance_m" not in runtimes[0]
    assert runtimes[1]["gripper_target_configurations"] == [{"tcp_frame": "tcp"}]
    # Keys stay in the Composer's sorted layout.
    assert (project / "deployment.yaml").read_text(encoding="utf-8") == json.dumps(
        converted, indent=2, ensure_ascii=False, sort_keys=True) + "\n"


def test_deployment_without_a_clearance_gets_the_default(tmp_path: Path) -> None:
    project = tmp_path / "demo.scenario"
    project.mkdir()
    _write_json(project / "deployment.yaml", _deployment())
    assert _module().main([str(project)]) == 0
    assert _read_json(project / "deployment.yaml")["collision_rules"] == [
        {"clearance_m": 0.001, "rule_id": "everything"}]


def test_robots_with_different_clearances_are_refused(tmp_path: Path, capsys) -> None:
    project = tmp_path / "demo.scenario"
    project.mkdir()
    deployment = _deployment({"collision_clearance_m": 0.003})
    second = copy.deepcopy(deployment["components"][0])
    second["component_id"] = "robot_b"
    second["configuration"]["runtime_configuration"] = {}
    deployment["components"].append(second)
    _write_json(project / "deployment.yaml", deployment)
    before = (project / "deployment.yaml").read_bytes()
    assert _module().main([str(project)]) == 1
    assert (project / "deployment.yaml").read_bytes() == before
    message = capsys.readouterr().err
    assert "robot: 0.003 m" in message and "robot_b: 0.001 m" in message


def test_contact_policies_become_rules_on_their_nodes(tmp_path: Path) -> None:
    probe = _move("probe", contact_policy_id="touch", clearance_m=0.002)
    probe["value_sources"] = {"contact_scene_resource_id": {"kind": "blackboard_key",
                                                           "reference": "trial.object"}}
    withdraw = _move("withdraw", contact_policy_id="leave",
                     contact_scene_resource_id="block")
    pick = {"children": [], "instance_id": "plan_pick", "node_id": "planning.plan_pick_sequence",
            "parameters": {"contact_policy_id": "grasp", "output_key": "pick.plan",
                           "tool_target": {"command_target_id": "gripper",
                                           "component_id": "gripper"}},
            "value_sources": {"selected_object_id": {"kind": "blackboard_field",
                                                    "reference": "selection.result",
                                                    "field": "selected_object_id"}}}
    program = _program(
        [probe, withdraw, pick],
        blackboard={"keys": [
            {"key": "picked", "type": "scene_resource", "access": "read_write",
             "persistence": "run"},
            {"key": "trial.object", "type": "scene_resource", "access": "read_write",
             "persistence": "run", "initial": "block"}]},
        contact_policies=[
            _policy("touch", "contact_grasp", {"kind": "motion_endpoint"},
                    max_penetration_m=0.002, terminal_window_start_fraction=0.7),
            _policy("leave", "contact_release", {"kind": "motion_endpoint"},
                    release_evidence_mode="binary_contact", enforce_max_start_penetration=False,
                    require_strictly_decreasing_penetration=False),
            _policy("grasp", "contact_grasp",
                    {"kind": "component_model_body",
                     "body": {"component_id": "gripper", "collision_body_id": "fingers"}},
                    max_penetration_m=0.001, terminal_window_start_fraction=0.8),
        ])
    path = tmp_path / "program.yaml"
    _write_json(path, program)
    assert _module().main([str(path)]) == 0
    converted = _read_json(path)
    assert converted["api_version"] == "manyforge.program.v4"
    assert converted["catalog"]["content_version"] == "4.0.0"
    assert "contact_policies" not in converted
    # The key that only bound the policies to their object is gone.
    assert [row["key"] for row in converted["blackboard"]["keys"]] == ["trial.object"]
    nodes = _nodes(converted)
    assert "value_sources" not in nodes["probe"]
    assert nodes["probe"]["parameters"]["collision_rules"] == [
        {"clearance_m": 0.002, "rule_id": "everything"},
        {"a": [{"kind": "active_tcp_contact_bodies"}],
         "b": [{"blackboard_key": "trial.object", "kind": "scene_object"}],
         "contact": {"end_from_fraction": 0.7, "max_penetration_m": 0.002},
         "rule_id": "touch"}]
    assert "clearance_m" not in nodes["probe"]["parameters"]
    assert nodes["withdraw"]["parameters"]["collision_rules"] == [
        {"a": [{"kind": "active_tcp_contact_bodies"}],
         "b": [{"kind": "scene_object", "scene_resource_id": "block"}],
         "release": {"enforce_max_start_penetration": False,
                     "release_evidence_mode": "binary_contact",
                     "require_strictly_decreasing_penetration": False},
         "rule_id": "leave"}]
    assert "contact_scene_resource_id" not in nodes["withdraw"]["parameters"]
    assert nodes["plan_pick"]["parameters"]["collision_rules"] == [
        {"a": [{"collision_body_id": "fingers", "component_id": "gripper",
                "kind": "collision_body"}],
         "b": [{"blackboard_key": "selection.result", "field": "selected_object_id",
                "kind": "scene_object"}],
         "contact": {"end_from_fraction": 0.8, "max_penetration_m": 0.001},
         "leg": "descend", "rule_id": "grasp"}]


def test_support_contacts_become_rules_on_the_lift_and_the_place(tmp_path: Path) -> None:
    gripper = {"command_target_id": "gripper", "component_id": "gripper"}
    nodes = [
        _move("approach"),
        {"children": [], "instance_id": "attach", "node_id": "scene.attach_carried_object",
         "parameters": {"scene_resource_id": "part", "target": gripper,
                        "touch_bodies": [{"component_id": "gripper",
                                          "collision_body_id": "fingers"}]}},
        _move("lift"),
        _move("carry"),
        _move("lower"),
        {"children": [], "instance_id": "detach", "node_id": "scene.detach_carried_object",
         "parameters": {"scene_resource_id": "part", "target": gripper}},
        _move("leave"),
    ]
    support = {"support_object_id": "table", "release_evidence_mode": "signed_penetration",
               "enforce_max_start_penetration": True, "max_start_penetration_m": 0.01,
               "release_tolerance_m": 0.001, "allow_recontact": False}
    program = _program(nodes, scene=[
        {"scene_resource_id": "part", "metadata": {"support_contacts": [support]}},
        {"scene_resource_id": "table"}])
    path = tmp_path / "program.yaml"
    _write_json(path, program)
    assert _module().main([str(path)]) == 0
    converted = _read_json(path)
    assert converted["scene"] == [{"scene_resource_id": "part"}, {"scene_resource_id": "table"}]
    found = _nodes(converted)
    selections = {"a": [{"component_id": "gripper", "kind": "attached_object"}],
                  "b": [{"kind": "scene_object", "scene_resource_id": "table"}]}
    assert found["lift"]["parameters"]["collision_rules"] == [
        {**selections, "rule_id": "lift_from_table",
         "release": {"allow_recontact": False, "enforce_max_start_penetration": True,
                     "max_start_penetration_m": 0.01,
                     "release_evidence_mode": "signed_penetration",
                     "release_tolerance_m": 0.001}}]
    assert found["lower"]["parameters"]["collision_rules"] == [
        {**selections, "rule_id": "place_on_table",
         "contact": {"end_from_fraction": 0.8, "max_penetration_m": 0.01}}]
    for untouched in ("approach", "carry", "leave"):
        assert "collision_rules" not in found[untouched]["parameters"]
    # The grip pair stays on the attach node.
    assert found["attach"]["parameters"]["touch_bodies"] == [
        {"collision_body_id": "fingers", "component_id": "gripper"}]


def test_a_bounded_support_contact_becomes_whole_move_contact(tmp_path: Path) -> None:
    gripper = {"command_target_id": "gripper", "component_id": "gripper"}
    nodes = [
        {"children": [], "instance_id": "attach", "node_id": "scene.attach_carried_object",
         "parameters": {"scene_resource_id": "part", "target": gripper,
                        "touch_bodies": [{"component_id": "gripper",
                                          "collision_body_id": "fingers"}]}},
        _move("lift"),
        {"children": [], "instance_id": "detach", "node_id": "scene.detach_carried_object",
         "parameters": {"scene_resource_id": "part", "target": gripper}},
    ]
    support = {"support_object_id": "table", "mode": "bounded_penetration",
               "max_contact_penetration_m": 0.002}
    program = _program(nodes, scene=[
        {"scene_resource_id": "part", "metadata": {"support_contacts": [support]}},
        {"scene_resource_id": "table"}])
    path = tmp_path / "program.yaml"
    _write_json(path, program)
    assert _module().main([str(path)]) == 0
    rules = _nodes(_read_json(path))["lift"]["parameters"]["collision_rules"]
    # Fraction 0: the pair may touch during the whole move, as the bounded contact did.
    assert all(rule["contact"] == {"end_from_fraction": 0, "max_penetration_m": 0.002}
               for rule in rules)
    assert rules


def test_a_support_contact_without_a_lift_is_refused(tmp_path: Path, capsys) -> None:
    program = _program([_move("only_move")], scene=[
        {"scene_resource_id": "part",
         "metadata": {"support_contacts": [{"support_object_id": "table"}]}}])
    path = tmp_path / "program.yaml"
    _write_json(path, program)
    before = path.read_bytes()
    assert _module().main([str(path)]) == 1
    assert path.read_bytes() == before
    assert "'part'" in capsys.readouterr().err


def test_an_undeclared_contact_policy_is_refused(tmp_path: Path, capsys) -> None:
    path = tmp_path / "program.yaml"
    _write_json(path, _program([_move("probe", contact_policy_id="missing",
                                      contact_scene_resource_id="block")]))
    assert _module().main([str(path)]) == 1
    assert "'probe'" in capsys.readouterr().err


def test_scenario_folder_is_converted_once_and_its_members_are_pinned(tmp_path: Path) -> None:
    project = tmp_path / "demo.scenario"
    project.mkdir()
    _write_json(project / "program.yaml", _program([_move("move", clearance_m=0.0)]))
    _write_json(project / "deployment.yaml", _deployment())
    _write_json(project / "scenario.yaml", {
        "api_version": "manyforge.scenario.project.v3",
        "members": {"deployment.yaml": "sha256:" + "1" * 64,
                    "program.yaml": "sha256:" + "2" * 64},
        "saved_status": {"checked_content_digest": "sha256:" + "3" * 64,
                         "last_known_status": "ready", "manyforge_version": "0.4.0"},
    })
    module = _module()
    assert module.main([str(project)]) == 0
    manifest = _read_json(project / "scenario.yaml")
    for member in ("deployment.yaml", "program.yaml"):
        digest = "sha256:" + hashlib.sha256((project / member).read_bytes()).hexdigest()
        assert manifest["members"][member] == digest
    assert manifest["saved_status"]["last_known_status"] == "unknown"
    assert _nodes(_read_json(project / "program.yaml"))["move"]["parameters"][
        "collision_rules"] == [{"clearance_m": 0.0, "rule_id": "everything"}]
    snapshot = {path.name: path.read_bytes() for path in project.iterdir()}
    assert module.main([str(project)]) == 0
    assert {path.name: path.read_bytes() for path in project.iterdir()} == snapshot


def test_a_hand_formatted_program_keeps_its_layout_when_only_the_pins_change(
        tmp_path: Path) -> None:
    text = (
        "api_version: manyforge.program.v3\n"
        "program_id: demo\n"
        "catalog:\n"
        "  api_version: manyforge.node_catalog.v3\n"
        "  content_version: 3.0.0\n"
        "tree:\n"
        "  node_id: motion.move_to_pose\n"
        "  instance_id: move\n"
        "  parameters:\n"
        "    target: {component_id: robot, command_target_id: arm}\n"
        "  children: []\n"
    )
    path = tmp_path / "demo.program.yaml"
    path.write_text(text, encoding="utf-8")
    assert _module().main([str(path)]) == 0
    assert path.read_text(encoding="utf-8") == text.replace(
        "manyforge.program.v3", "manyforge.program.v4").replace(
        "content_version: 3.0.0", "content_version: 4.0.0")
    assert yaml.safe_load(path.read_text(encoding="utf-8"))["tree"]["parameters"]["target"] == {
        "component_id": "robot", "command_target_id": "arm"}


@pytest.mark.parametrize("version", ["manyforge.program.v2", None])
def test_an_unknown_program_version_is_refused(tmp_path: Path, version) -> None:
    program = _program([])
    program["api_version"] = version
    path = tmp_path / "program.yaml"
    _write_json(path, program)
    assert _module().main([str(path)]) == 1
