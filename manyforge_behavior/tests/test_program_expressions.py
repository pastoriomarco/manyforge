"""Program validation of computed keys and path-keyed value sources (V13.5)."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest
import yaml

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program, ProgramValidationError

_ROOT = Path(__file__).resolve().parents[2]


def _catalog() -> NodeCatalog:
    return NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def _key(key: str, value_type: str, **extra: Any) -> dict[str, Any]:
    return {
        "key": key,
        "type": value_type,
        "access": "read_write",
        "persistence": "run",
        **extra,
    }


def _computed(key: str, value_type: str, expression: str, **extra: Any) -> dict[str, Any]:
    return _key(key, value_type, access="read", expression=expression, **extra)


def _parameter(parameter_id: str, value_type: str, default: Any) -> dict[str, Any]:
    return {
        "parameter_id": parameter_id,
        "type": value_type,
        "default": default,
    }


_PARAMETERS = (
    _parameter("grasp_offset", "float64", 0.01),
    _parameter("pitch", "float64", 0.05),
    _parameter("tube_length", "float64", 0.2),
)


def _pose(orientation: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [0.1, 0.2, 0.3],
        "orientation": orientation or {"rpy_deg": [0.0, 0.0, 90.0]},
    }


def _offset_node(
    value_sources: dict[str, Any] | None = None, **parameters: Any
) -> dict[str, Any]:
    node = {
        "node_id": "geometry.offset_pose",
        "instance_id": "offset",
        "parameters": {
            "pose": _pose(),
            "offset": {"position_m": [0.0, 0.0, 0.1], "rpy_deg": [0.0, 0.0, 0.0]},
            "output_key": "out",
            **parameters,
        },
        "children": [],
    }
    if value_sources is not None:
        node["value_sources"] = value_sources
    return node


def _base_program_document() -> dict[str, Any]:
    # Construct the v3 document explicitly: historical role assignments are
    # deliberately not converted into direct references.
    return {
        "api_version": "manyforge.program.v3",
        "program_id": "expression_fixture",
        "program_revision": "sha256:" + "0" * 64,
        "program_profile": "composable-cell-v1",
        "name": "Expression fixture",
        "catalog": {"api_version": "manyforge.node_catalog.v3", "content_version": _catalog().content_version},
        "parameters": [],
    }


def _program(
    *children: dict[str, Any],
    keys: tuple[dict[str, Any], ...] = (),
) -> Program:
    document = _base_program_document()
    document["parameters"] = sorted(
        [*document["parameters"], *deepcopy(_PARAMETERS)],
        key=lambda row: row["parameter_id"],
    )
    document["blackboard"] = {
        "namespaces": [],
        "keys": sorted(
            [
                _key("index", "int64", initial=2),
                _key("out", "stored_pose"),
                _key("place", "stored_pose"),
                *keys,
            ],
            key=lambda row: row["key"],
        ),
    }
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": list(children) or [_offset_node()],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return Program.from_dict(document, _catalog())


def _expression(text: str) -> dict[str, str]:
    return {"kind": "expression", "expression": text}


# --- computed keys ---------------------------------------------------------


def test_computed_keys_are_checked_and_ordered_by_what_they_read() -> None:
    program = _program(
        keys=(
            _computed("grasp_z", "float64", "-param.tube_length / 2 + param.grasp_offset"),
            _computed("approach_z", "float64", "key.grasp_z + 0.05"),
            _computed("next", "int64", "key.index + 1"),
        )
    )
    expressions = program.expressions
    assert set(expressions.computed_keys) == {"approach_z", "grasp_z", "next"}
    order = expressions.computed_key_order
    assert order.index("grasp_z") < order.index("approach_z")
    assert program.blackboard_keys["grasp_z"].expression.startswith("-param")


@pytest.mark.parametrize(
    ("row", "message"),
    [
        (
            _computed("z", "float64", "1.0", initial=0.0),
            r"keys\[\d+\]\.initial: a computed key has no initial value",
        ),
        (
            _computed("z", "float64", "1.0", persistence="persistent"),
            "a computed key cannot be persistent",
        ),
        (
            _key("z", "float64", expression="1.0"),
            "a computed key is read-only for nodes and the operator; declare access read",
        ),
        (
            _computed("z", "stored_pose", "1.0"),
            "a computed key must be bool, int64, float64 or string",
        ),
        (
            _computed("z", "float64", "param.missing * 2"),
            r"keys\[\d+\]\.expression: unknown_name at character 0: param.missing is not declared",
        ),
        (
            _computed("z", "float64", "key.index > 1"),
            "destination_type_mismatch",
        ),
        (
            _computed("z", "float64", "1 / 0"),
            "division_by_zero",
        ),
        (
            _computed("z", "float64", "(1 +"),
            "at character 4",
        ),
    ],
)
def test_computed_key_declarations_are_refused_by_name(row, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(keys=(row,))


def test_computed_key_cycles_are_refused_before_anything_runs() -> None:
    with pytest.raises(
        ProgramValidationError,
        match="computed keys read each other in a cycle: a -> b -> a",
    ):
        _program(
            keys=(
                _computed("a", "int64", "key.b + 1"),
                _computed("b", "int64", "key.a + 1"),
            )
        )
    with pytest.raises(ProgramValidationError, match="cycle: self -> self"):
        _program(keys=(_computed("self", "int64", "key.self + 1"),))


def test_nodes_can_never_write_a_computed_key() -> None:
    with pytest.raises(ProgramValidationError, match="names computed key 'z'"):
        _program(
            {
                "node_id": "logic.set_value",
                "instance_id": "set",
                "parameters": {"key": "z", "value": 1.0},
                "children": [],
            },
            keys=(_computed("z", "float64", "1.0"),),
        )


def test_expressions_name_keys_they_cannot_read() -> None:
    with pytest.raises(ProgramValidationError, match="key.sink does not permit reads"):
        _program(
            keys=(
                _key("sink", "float64", access="write"),
                _computed("z", "float64", "key.sink"),
            )
        )
    with pytest.raises(
        ProgramValidationError,
        match="key.names has type string_list, which expressions cannot read",
    ):
        _program(
            keys=(
                _key("names", "string_list"),
                _computed("z", "float64", "key.names"),
            )
        )


# --- value sources by path -------------------------------------------------


def test_field_expressions_and_nested_bindings_are_accepted_by_path() -> None:
    program = _program(
        _offset_node(
            {
                "offset.position_m[2]": _expression("key.index * param.pitch"),
                "pose.orientation.rpy_deg[2]": _expression("key.index * 45.0"),
                "pose.position_m[0]": {"kind": "blackboard_key", "reference": "z"},
            }
        ),
        keys=(_computed("z", "float64", "-param.tube_length / 2"),),
    )
    node = program.tree.children[0]
    # The literal stays in parameters; it is restored when a source is removed.
    assert node.parameters["offset"]["position_m"] == [0.0, 0.0, 0.1]
    assert set(program.expressions.value_expressions) == {
        ("offset", "offset.position_m[2]"),
        ("offset", "pose.orientation.rpy_deg[2]"),
    }


def test_a_whole_value_source_may_keep_its_literal() -> None:
    program = _program(
        _offset_node({"pose": {"kind": "blackboard_key", "reference": "place"}})
    )
    node = program.tree.children[0]
    assert "pose" in node.parameters and "pose" in node.value_sources


@pytest.mark.parametrize(
    ("sources", "message"),
    [
        (
            {
                "pose": {"kind": "blackboard_key", "reference": "place"},
                "pose.position_m[0]": _expression("1.0"),
            },
            r"value_sources\.pose\.position_m\[0\]: overlaps the value_sources entry 'pose'",
        ),
        (
            {"offset.position_m[02]": _expression("1.0")},
            "path_index_invalid",
        ),
        (
            {"offset.position_m[3]": _expression("1.0")},
            "names no value in the node's parameters",
        ),
        (
            {"offset.missing": _expression("1.0")},
            "names no value in the node's parameters",
        ),
        (
            {"absent": _expression("1.0")},
            r"supplies unknown catalog parameters \['absent'\]",
        ),
        (
            {"pose.frame_ref.kind": {"kind": "program_parameter", "reference": "pitch"}},
            "stay literal",
        ),
        (
            {"pose.frame_ref": {"kind": "blackboard_key", "reference": "place"}},
            "incompatible with the destination schema",
        ),
        (
            {"output_key": {"kind": "program_parameter", "reference": "pitch"}},
            "cannot dynamically select runtime/binding semantics",
        ),
        (
            {"offset_frame": _expression('"pose"')},
            "an expression computes only a bool, int64, float64 or string value",
        ),
        (
            {"pose": _expression("1.0")},
            "an expression computes only a bool, int64, float64 or string value",
        ),
        (
            {"offset.position_m[0]": _expression("key.index > 1")},
            r"offset\.position_m\[0\]\.expression: destination_type_mismatch",
        ),
        (
            {"offset.position_m[0]": {"kind": "blackboard_key", "reference": "place"}},
            "incompatible with the destination schema",
        ),
    ],
)
def test_path_keyed_sources_are_refused_by_name(sources, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(_offset_node(sources))


def test_unsorted_value_source_paths_are_refused() -> None:
    node = _offset_node()
    node["value_sources"] = {
        "pose.position_m[0]": _expression("1.0"),
        "offset.position_m[0]": _expression("1.0"),
    }
    with pytest.raises(ProgramValidationError, match="keys must be canonical-sorted"):
        _program(node)


def test_quaternion_components_are_bound_only_as_a_whole() -> None:
    xyzw = _pose({"xyzw": [0.0, 0.0, 0.0, 1.0]})
    with pytest.raises(ProgramValidationError, match="quaternion components depend on each other"):
        _program(_offset_node({"pose.orientation.xyzw[3]": _expression("1.0")}, pose=xyzw))
    # A per-component formula needs an rpy_deg literal to replace.
    with pytest.raises(ProgramValidationError, match="names no value"):
        _program(
            _offset_node({"pose.orientation.rpy_deg[0]": _expression("1.0")}, pose=xyzw)
        )


def test_key_typed_values_take_expressions_of_the_key_type() -> None:
    def set_count(expression: str) -> dict[str, Any]:
        return {
            "node_id": "logic.set_value",
            "instance_id": "set",
            "parameters": {"key": "index", "value": 0},
            "value_sources": {"value": _expression(expression)},
            "children": [],
        }

    program = _program(set_count("key.index + 1"))
    assert ("set", "value") in program.expressions.value_expressions
    with pytest.raises(ProgramValidationError, match="destination_type_mismatch"):
        _program(set_count("key.index / 2"))
    pose_field = {
        "node_id": "logic.set_value",
        "instance_id": "set",
        "parameters": {"key": "place", "value": _pose()},
        "value_sources": {"value.position_m[2]": _expression("param.pitch")},
        "children": [],
    }
    _program(pose_field)
    with pytest.raises(ProgramValidationError, match="int64 values have no fields"):
        _program(
            {
                **set_count("1"),
                "value_sources": {"value.x": _expression("1")},
            }
        )


# The refusals shared with the C++ Program contract (test_program_contract.cpp).
_SOURCE_CONFORMANCE = yaml.safe_load(
    (
        _ROOT
        / "manyforge_deployment/resources/expression/"
        "program_value_source_conformance.v1.yaml"
    ).read_text(encoding="utf-8")
)


def _conformance_program(case: dict[str, Any]) -> Program:
    template = deepcopy(_SOURCE_CONFORMANCE["nodes"][case["node"]])
    node = {
        **template,
        "parameters": {**template["parameters"], **deepcopy(case.get("parameters", {}))},
        "value_sources": deepcopy(case["value_sources"]),
        "children": [],
    }
    document = _base_program_document()
    document["parameters"] = sorted(
        [*document["parameters"], *deepcopy(_SOURCE_CONFORMANCE["parameters"])],
        key=lambda row: row["parameter_id"],
    )
    document["blackboard"] = {
        "namespaces": [],
        "keys": deepcopy(_SOURCE_CONFORMANCE["blackboard_keys"]),
    }
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": [node],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return Program.from_dict(document, _catalog())


@pytest.mark.parametrize(
    "case", _SOURCE_CONFORMANCE["accepted"], ids=lambda case: case["name"]
)
def test_shared_value_source_cases_are_accepted(case: dict[str, Any]) -> None:
    _conformance_program(case)


@pytest.mark.parametrize(
    "case", _SOURCE_CONFORMANCE["refused"], ids=lambda case: case["name"]
)
@pytest.mark.slow
def test_shared_value_source_cases_are_refused(case: dict[str, Any]) -> None:
    with pytest.raises(ProgramValidationError) as raised:
        _conformance_program(case)
    assert case["reason"] in str(raised.value)


# The pose-value refusals shared with the C++ Program contract
# (test_program_contract.cpp).
_POSE_CONFORMANCE = yaml.safe_load(
    (
        _ROOT
        / "manyforge_deployment/resources/expression/"
        "program_pose_value_conformance.v1.yaml"
    ).read_text(encoding="utf-8")
)


def _pose_conformance_program(case: dict[str, Any]) -> Program:
    node = deepcopy(_POSE_CONFORMANCE["node"])
    if "value" in case:
        node["parameters"]["value"] = deepcopy(case["value"])
    document = _base_program_document()
    document["scene"] = deepcopy(_POSE_CONFORMANCE["scene"])
    document["blackboard"] = {
        "namespaces": [],
        "keys": sorted(
            deepcopy([*_POSE_CONFORMANCE["blackboard_keys"], *case.get("keys", [])]),
            key=lambda row: row["key"],
        ),
    }
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": [{**node, "children": []}],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return Program.from_dict(document, _catalog())


@pytest.mark.parametrize(
    "case", _POSE_CONFORMANCE["accepted"], ids=lambda case: case["name"]
)
def test_shared_pose_value_cases_are_accepted(case: dict[str, Any]) -> None:
    _pose_conformance_program(case)


@pytest.mark.parametrize(
    "case", _POSE_CONFORMANCE["refused"], ids=lambda case: case["name"]
)
@pytest.mark.slow
def test_shared_pose_value_cases_are_refused(case: dict[str, Any]) -> None:
    with pytest.raises(ProgramValidationError) as raised:
        _pose_conformance_program(case)
    assert case["reason"] in str(raised.value)
