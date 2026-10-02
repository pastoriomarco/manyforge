"""Run the shared expression conformance cases through the Python binding.

The C++ engine test runs the same file; both read every scalar as text and
interpret it by its declared type, so they check exactly the same values.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any

import pytest
import yaml

from manyforge_headless_py import expression

CONFORMANCE = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment"
    / "resources"
    / "expression"
    / "conformance.v1.yaml"
)
ROOT = yaml.load(CONFORMANCE.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
CASES = ROOT["cases"]


def _type(node: Any) -> Any:
    return node if isinstance(node, str) else {k: _type(v) for k, v in node.items()}


def _value(node: Any, declared: Any) -> Any:
    if declared == "bool":
        assert node in ("true", "false")
        return node == "true"
    if declared == "int64":
        return int(node)
    if declared == "float64":
        return float(node)
    if declared == "string":
        return node
    if declared == "pose":
        return {
            "position_m": [float(item) for item in node["position_m"]],
            "orientation": {
                form: [float(item) for item in items]
                for form, items in node["orientation"].items()
            },
        }
    return {name: _value(item, declared[name]) for name, item in node.items()}


def _environment(node: dict[str, Any]) -> dict[str, Any]:
    return {
        section: {name: _type(item) for name, item in rows.items()}
        for section, rows in node.items()
    }


def _values(node: dict[str, Any], environment: dict[str, Any]) -> dict[str, Any]:
    return {
        section: {
            name: _value(item, environment[section][name])
            for name, item in rows.items()
        }
        for section, rows in node.items()
    }


def _expect_diagnostic(expected: dict[str, str], actual: dict[str, Any]) -> None:
    assert actual["code"] == expected["code"], actual
    assert actual["message"]
    if "position" in expected:
        assert actual["position"] == int(expected["position"]), actual
    if "length" in expected:
        assert actual["length"] == int(expected["length"]), actual
    if "name" in expected:
        assert actual["name"] == expected["name"], actual


@pytest.mark.parametrize("case", CASES, ids=[case["id"] for case in CASES])
def test_conformance_case(case: dict[str, Any]) -> None:
    error = case.get("error")
    phase = error["phase"] if error else None
    limits = {name: int(value) for name, value in case.get("limits", {}).items()}
    if phase == "parse":
        with pytest.raises(expression.ExpressionError) as raised:
            expression.parse(case["expression"], limits)
        assert len(raised.value.diagnostics) == 1
        _expect_diagnostic(error, raised.value.diagnostics[0])
        return
    parsed = expression.parse(case["expression"], limits)
    assert parsed.text == case["expression"]
    environment = _environment(case.get("environment", ROOT["environment"]))
    if phase == "check":
        with pytest.raises(expression.ExpressionError) as raised:
            parsed.check(environment, case["destination"])
        diagnostics = raised.value.diagnostics
        _expect_diagnostic(error, diagnostics[0])
        if "diagnostic_count" in case:
            assert len(diagnostics) == int(case["diagnostic_count"])
        return
    checked = parsed.check(environment, case["destination"])
    assert checked.result_type == case["destination"]
    values = _values(case.get("values", ROOT["values"]), environment)
    if phase == "evaluate":
        with pytest.raises(expression.ExpressionError) as raised:
            checked.evaluate(values)
        _expect_diagnostic(error, raised.value.diagnostics[0])
        return
    assert phase is None
    result = checked.evaluate(values)
    expected = _value(case["result"], case["destination"])
    assert type(result) is type(expected)
    if "tolerance" in case:
        assert math.isclose(result, expected, rel_tol=0.0, abs_tol=float(case["tolerance"]))
    else:
        assert result == expected


def test_limits_are_the_approved_values() -> None:
    assert expression.LIMITS == {
        "text_length": 1024,
        "nesting_depth": 64,
        "tokens": 512,
        "syntax_nodes": 256,
        "evaluation_steps": 256,
    }
    with pytest.raises(ValueError, match="approved"):
        expression.parse("1", {"tokens": 513})
    with pytest.raises(ValueError, match="unknown expression limit"):
        expression.parse("1", {"steps": 1})


def test_error_is_a_value_error_with_structured_diagnostics() -> None:
    assert issubclass(expression.ExpressionError, ValueError)
    with pytest.raises(expression.ExpressionError) as raised:
        expression.parse("key.a").check(
            {"keys": {"b": "int64"}}, "int64"
        )
    assert str(raised.value) == raised.value.diagnostics[0]["message"]
    assert raised.value.diagnostics == [
        {
            "code": "unknown_name",
            "message": "key.a is not declared",
            "position": 0,
            "length": 5,
            "name": "key.a",
        }
    ]


def test_references_and_program_pose_shape() -> None:
    checked = expression.parse(
        "key.index + param.goal.yaw + key.goal.x"
    ).check(
        {
            "parameters": {"goal": "pose"},
            "keys": {"index": "int64", "goal": "pose"},
        },
        "float64",
    )
    assert checked.references == [("param", "goal"), ("key", "goal"), ("key", "index")]
    pose = {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [1, 0.5, 0],
        "orientation": {"rpy_deg": [0, 0, 30]},
    }
    assert checked.evaluate(
        {"parameters": {"goal": pose}, "keys": {"index": 2, "goal": pose}}
    ) == 33.0
    missing = {"parameters": {"goal": pose}, "keys": {"goal": pose}}
    with pytest.raises(expression.ExpressionError) as raised:
        checked.evaluate(missing)
    assert raised.value.diagnostics[0]["code"] == "missing_value"
    assert raised.value.diagnostics[0]["name"] == "key.index"


def test_values_are_mapped_strictly() -> None:
    checked = expression.parse("key.flag && key.count > 0").check(
        {"keys": {"flag": "bool", "count": "int64"}}, "bool"
    )
    assert checked.evaluate({"keys": {"flag": True, "count": 1}}) is True
    with pytest.raises(TypeError):
        checked.evaluate({"keys": {"flag": 1, "count": 1}})
    with pytest.raises(TypeError):
        checked.evaluate({"keys": {"flag": True, "count": True}})
    with pytest.raises(ValueError, match="unknown section"):
        checked.evaluate({"key": {}})
    pose_check = expression.parse("key.p.x").check({"keys": {"p": "pose"}}, "float64")
    with pytest.raises(ValueError, match="unknown field"):
        pose_check.evaluate(
            {"keys": {"p": {"position_m": [0, 0, 0], "orientation": {"xyzw": [0, 0, 0, 1]}, "pose": 1}}}
        )
    with pytest.raises(ValueError, match="exactly one"):
        pose_check.evaluate(
            {"keys": {"p": {"position_m": [0, 0, 0], "orientation": {"rpy_deg": [0, 0, 0], "xyzw": [0, 0, 0, 1]}}}}
        )
    record_check = expression.parse("key.r.a").check(
        {"keys": {"r": {"a": "float64"}}}, "float64"
    )
    with pytest.raises(ValueError, match="not declared"):
        record_check.evaluate({"keys": {"r": {"a": 1.0, "b": 2.0}}})
    with pytest.raises(ValueError, match="unknown expression type"):
        expression.parse("1").check({}, "number")
    with pytest.raises(ValueError, match="destination"):
        expression.parse("1").check({}, "pose")


@pytest.mark.parametrize(
    ("text", "code"),
    [
        ("(" * 65 + "1" + ")" * 65, "nesting_too_deep"),
        ("1" + " " * 1024, "text_too_long"),
        ("1+" * 300 + "1", "too_many_tokens"),
        ("1" + "+1" * 128, "syntax_tree_too_large"),
        ("9" * 1000, "integer_literal_out_of_range"),
        ('"' + "a" * 1000, "unterminated_string"),
        ("1 + \x00", "invalid_character"),
        ("é", "invalid_character"),
    ],
)
def test_pathological_inputs_fail_structurally(text: str, code: str) -> None:
    with pytest.raises(expression.ExpressionError) as raised:
        expression.parse(text)
    assert raised.value.diagnostics[0]["code"] == code
