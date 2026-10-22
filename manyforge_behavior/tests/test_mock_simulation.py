"""Node Mock simulation (contract item 133): catalog shape and Program
validation of the literal-only ``mock_simulation`` rows."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.interrupt_condition import condition_signals
from manyforge_behavior.program import Program, ProgramValidationError

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_MOTION = ("motion.move_until_signal", "motion.probe_search")
_IO = ("io.get_input", "io.check_input", "io.wait_for_input")
_GOAL = {
    "frame_ref": {"kind": "target_frame", "target_role": "arm", "frame_role": "base"},
    "position_m": [0.3, 0.0, 0.2],
    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
}


def _signal(resource_role: str) -> dict[str, Any]:
    return {"signal": {"target_role": "arm", "resource_role": resource_role}}


def _compare(resource_role: str) -> dict[str, Any]:
    return {"compare": {"operand": _signal(resource_role), "op": "==", "value": True}}


_CONDITION = {
    "any": [
        _compare("part_present"),
        {"not": _compare("door_closed")},
        {"compare": {"operand": {"key": "cell.count"}, "op": ">", "value": 1}},
    ]
}


def _row(resource_role: str = "part_present", start: Any = False, change: Any = None):
    row: dict[str, Any] = {**_signal(resource_role), "start_value": start}
    if change is not None:
        row["change"] = change
    return row


def _contact_policy() -> dict[str, Any]:
    policy: dict[str, Any] = {
        "api_version": "manyforge.contact_policy.v1",
        "default_unmatched_response": "hard_stop",
        "mode": "contact_grasp",
        "policy_id": "probe_contact",
        "sensing_mode": "geometry_only",
        "version": "sha256:" + "0" * 64,
        "zones": [
            {
                "body_pairs": [
                    {
                        "body_a": {
                            "collision_body_role_id": "arm_collision",
                            "component_role_id": "robot",
                            "kind": "component_model_body",
                        },
                        "body_b": {"kind": "runtime_object", "runtime_object_id": "cell.probe"},
                    }
                ],
                "max_penetration_m": 0.002,
                "terminal_window_start_fraction": 0.8,
                "violation_response": "hard_stop",
                "zone_id": "probe_tip",
            }
        ],
    }
    policy["version"] = compute_content_identity(
        policy,
        own_field="version",
        identity_domain="catalog-record-revision/contact_policy",
        api_version="manyforge.contact_policy.v1",
    )
    return policy


def _program(
    node_id: str,
    rows: Any = None,
    *,
    value_sources: dict[str, Any] | None = None,
) -> Program:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    arm = document["target_roles"]["arm"]
    arm["required_resource_roles"] = sorted(
        {*arm["required_resource_roles"], "part_present", "door_closed"}
    )
    capabilities = {row["capability_id"] for row in arm["required_capabilities"]}
    for requirement in _CATALOG.get(node_id).required_capabilities:
        if requirement.scope == "target_binding" and requirement.when_parameter is None:
            capabilities.add(requirement.capability_id)
    arm["required_capabilities"] = [
        {"capability_id": capability, "parameters": {}, "disposition": "required"}
        for capability in sorted(capabilities)
    ]
    document["blackboard"] = {
        "namespaces": [{"namespace_id": "cell", "owner": "program"}],
        "keys": [
            {"key": "cell.count", "type": "int64", "access": "read", "persistence": "run"},
            {"key": "cell.probe", "type": "scene_resource", "access": "read_write",
             "persistence": "run"},
            {"key": "cell.seen", "type": "bool", "access": "read_write", "persistence": "run"},
        ],
    }
    if node_id == "motion.probe_search":
        document["contact_policies"] = [_contact_policy()]
    if node_id in _MOTION:
        parameters: dict[str, Any] = {
            "target_role": "arm",
            "base_frame_role": "base",
            "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"},
            "goal": deepcopy(_GOAL),
            "stop_mode": "soft_on_path",
            "interrupt_condition": deepcopy(_CONDITION),
        }
        if node_id == "motion.probe_search":
            parameters["contact_policy_id"] = "probe_contact"
            parameters["contact_scene_resource_id"] = "table"
    else:
        parameters = {"target_role": "arm", "resource_role": "part_present"}
        if node_id == "io.get_input":
            parameters["output_key"] = "cell.seen"
        else:
            parameters["expected_value"] = True
    if rows is not None:
        parameters["mock_simulation"] = rows
    node: dict[str, Any] = {
        "node_id": node_id,
        "instance_id": "reader",
        "parameters": parameters,
        "children": [],
    }
    if value_sources:
        node["value_sources"] = value_sources
    document["tree"]["children"] = [node]
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, _CATALOG)


# Catalog -----------------------------------------------------------------


@pytest.mark.parametrize("node_id", _MOTION + _IO)
def test_signal_reading_nodes_declare_an_optional_mock_simulation(node_id: str) -> None:
    parameters = {parameter.id: parameter for parameter in _CATALOG.get(node_id).parameters}
    mock = parameters["mock_simulation"]
    assert not mock.required and not mock.has_default
    schema = _CATALOG.schema_registry.resolve(mock.schema)
    assert schema.schema_type == "array"
    assert schema.max_items is None  # no authored-row cap (Track 2 review)


def test_other_nodes_do_not_take_a_mock_simulation() -> None:
    assert all(
        "mock_simulation" not in {parameter.id for parameter in entry.parameters}
        for entry in _CATALOG.entries
        if entry.id not in _MOTION + _IO
    )


def _issues(value: Any) -> list[Any]:
    schema = next(
        parameter.schema
        for parameter in _CATALOG.get("io.get_input").parameters
        if parameter.id == "mock_simulation"
    )
    return validate_value(schema, value, registry=_CATALOG.schema_registry, path="$")


@pytest.mark.parametrize(
    "rows",
    [
        [],
        [_row()],
        [_row(start=True, change={"after_ms": 0, "value": False})],
        [_row(change={"after_ms": 9_007_199_254_740_991, "value": True})],
    ],
)
def test_schema_accepts_well_formed_rows(rows: Any) -> None:
    assert _issues(rows) == []


@pytest.mark.parametrize(
    "rows",
    [
        [_row(start=1)],
        [_row(start="true")],
        [{"signal": {"target_role": "arm"}, "start_value": True}],
        [{**_signal("part_present")}],
        [{**_row(), "extra": True}],
        [_row(change={"after_ms": -1, "value": True})],
        [_row(change={"after_ms": 1.5, "value": True})],
        [_row(change={"after_ms": 10})],
        [_row(change={"after_ms": 10, "value": 0})],
        {"rows": []},
    ],
)
def test_schema_refuses_malformed_rows(rows: Any) -> None:
    assert _issues(rows)


# Program validation ------------------------------------------------------


def test_condition_signals_are_the_signal_operands() -> None:
    assert condition_signals(_CONDITION) == {
        ("arm", "part_present"),
        ("arm", "door_closed"),
    }


@pytest.mark.parametrize("node_id", _MOTION)
def test_motion_nodes_accept_rows_for_their_condition_signals(node_id: str) -> None:
    _program(node_id, [_row("part_present", change={"after_ms": 250, "value": True})])
    _program(node_id, [_row("part_present"), _row("door_closed", start=True)])
    _program(node_id, [])


@pytest.mark.parametrize("node_id", _IO)
def test_io_nodes_accept_a_row_for_their_own_signal(node_id: str) -> None:
    _program(node_id, [_row("part_present", start=True, change={"after_ms": 0, "value": False})])


@pytest.mark.parametrize("node_id", _MOTION + _IO)
def test_nodes_without_mock_simulation_still_validate(node_id: str) -> None:
    _program(node_id)


@pytest.mark.parametrize(
    ("node_id", "rows"),
    [
        ("motion.move_until_signal", [_row("arm_joints")]),
        ("motion.probe_search", [_row("arm_joints")]),
        ("io.get_input", [_row("door_closed")]),
        ("io.check_input", [_row("door_closed")]),
        ("io.wait_for_input", [_row("door_closed")]),
    ],
)
def test_a_signal_the_node_does_not_read_is_refused(node_id: str, rows: Any) -> None:
    with pytest.raises(ProgramValidationError, match="mock_simulation_signal_unused"):
        _program(node_id, rows)


@pytest.mark.parametrize("node_id", ["motion.move_until_signal", "io.wait_for_input"])
def test_a_duplicate_signal_is_refused(node_id: str) -> None:
    with pytest.raises(ProgramValidationError, match="mock_simulation_signal_duplicate"):
        _program(node_id, [_row(start=True), _row(start=False)])


@pytest.mark.parametrize(
    "rows",
    [
        [_row(start=1)],
        [_row(change={"after_ms": 5, "value": "on"})],
        [_row(change={"after_ms": -5, "value": True})],
    ],
)
def test_non_bool_or_negative_rows_are_refused(rows: Any) -> None:
    with pytest.raises(ProgramValidationError, match="mock_simulation_invalid"):
        _program("io.check_input", rows)


def test_an_unsafe_integer_offset_is_refused() -> None:
    with pytest.raises(Exception, match="safe range"):
        _program("io.check_input", [_row(change={"after_ms": 2**53, "value": True})])


@pytest.mark.parametrize(
    "reference",
    ["mock_simulation", "mock_simulation.start_value", "mock_simulation.change.value"],
)
@pytest.mark.parametrize("node_id", ["motion.probe_search", "io.get_input"])
def test_a_value_source_is_refused(node_id: str, reference: str) -> None:
    with pytest.raises(ProgramValidationError, match="mock_simulation_value_source_refused"):
        _program(
            node_id,
            [_row()],
            value_sources={reference: {"kind": "blackboard_key", "reference": "cell.seen"}},
        )
