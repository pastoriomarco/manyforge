"""A collision rule's Scene object named by a blackboard key is read with the
node's other inputs."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    NodeInputError,
    ProgramValueStore,
)
from manyforge_behavior.program import Program

from .v3_fixtures import frame, target

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_MOVE = "probe_move"


def _program(selection: dict[str, Any]) -> Program:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    document["blackboard"] = {
        "namespaces": [{"namespace_id": "cell", "owner": "program"}],
        "keys": [
            {"key": "cell.probe", "type": "scene_resource", "access": "read_write",
             "persistence": "run"},
        ],
    }
    document["tree"]["children"] = [{
        "node_id": "motion.move_to_pose",
        "instance_id": _MOVE,
        "parameters": {
            "target": target("ur5_robot", "arm"),
            "base_frame": frame("ur5_robot", "base"),
            "tcp_endpoint": {"kind": "current"},
            "goal": {
                "frame_ref": {"kind": "target_frame", "frame": frame("ur5_robot", "base")},
                "position_m": [0.3, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
            "collision_rules": [
                {"rule_id": "everything_else", "clearance_m": 0.002},
                {"rule_id": "tip_on_object",
                 "a": [{"kind": "active_tcp_contact_bodies"}],
                 "b": [{"kind": "scene_objects"}, deepcopy(selection)],
                 "contact": {"end_from_fraction": 0.8}},
            ],
        },
        "children": [],
    }]
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v4",
    )
    return Program.from_dict(document, _CATALOG)


def _snapshot(program: Program, store: ProgramValueStore):
    move = next(node for node in program.to_dict()["tree"]["children"]
                if node["instance_id"] == _MOVE)
    resolver = NodeAttemptInputResolver(program=program, catalog=_CATALOG, value_store=store)
    return resolver.snapshot(
        node_id=move["node_id"],
        node_instance_id=_MOVE,
        parameters=move["parameters"],
        value_sources=move.get("value_sources", {}),
    )


def test_a_rule_object_key_is_resolved_to_its_scene_object() -> None:
    program = _program({"kind": "scene_object", "blackboard_key": "cell.probe"})
    store = ProgramValueStore(program)
    store.write("cell.probe", "table")

    rules = _snapshot(program, store).parameters["collision_rules"]

    assert rules[0] == {"rule_id": "everything_else", "clearance_m": 0.002}
    assert rules[1]["b"] == [
        {"kind": "scene_objects"},
        {"kind": "scene_object", "scene_resource_id": "table"},
    ]
    # The authored Program keeps the key.
    authored = program.to_dict()["tree"]["children"][0]["parameters"]["collision_rules"]
    assert authored[1]["b"][1] == {"kind": "scene_object", "blackboard_key": "cell.probe"}


def test_an_empty_rule_object_key_fails_the_node_and_names_the_key() -> None:
    program = _program({"kind": "scene_object", "blackboard_key": "cell.probe"})

    with pytest.raises(NodeInputError) as error:
        _snapshot(program, ProgramValueStore(program))

    assert error.value.details["input_problem"] == "missing"
    assert error.value.details["source_reference"] == "cell.probe"
    assert error.value.details["path"] == "collision_rules[1].b[1].blackboard_key"


def test_a_literal_rule_object_needs_no_blackboard_read() -> None:
    program = _program({"kind": "scene_object", "scene_resource_id": "table"})

    rules = _snapshot(program, ProgramValueStore(program)).parameters["collision_rules"]

    assert rules[1]["b"][1] == {"kind": "scene_object", "scene_resource_id": "table"}
