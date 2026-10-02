"""The shared value-path conformance cases, run by the Python validator."""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from manyforge_behavior import value_path
from manyforge_behavior.value_path import (
    ValuePathError,
    format_value_path,
    parse_value_path,
    path_covers,
    replace_at_path,
    value_at_path,
)

_CONFORMANCE = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment/resources/expression/value_path_conformance.v1.yaml"
)
_ROOT = yaml.load(_CONFORMANCE.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)


def _segments(rows: list[dict[str, str]]) -> tuple[object, ...]:
    return tuple(row["name"] if "name" in row else int(row["index"]) for row in rows)


def test_limits_match_the_conformance_file() -> None:
    limits = _ROOT["limits"]
    assert int(limits["max_length"]) == value_path.MAX_PATH_LENGTH
    assert int(limits["max_segments"]) == value_path.MAX_PATH_SEGMENTS
    assert int(limits["max_name_length"]) == value_path.MAX_NAME_LENGTH
    assert int(limits["max_index_digits"]) == value_path.MAX_INDEX_DIGITS


@pytest.mark.parametrize("case", _ROOT["valid"], ids=lambda case: case["path"][:40])
def test_valid_paths_parse_and_format_back(case: dict) -> None:
    segments = _segments(case["segments"])
    assert parse_value_path(case["path"]) == segments
    assert format_value_path(segments) == case["path"]


@pytest.mark.parametrize("case", _ROOT["invalid"], ids=lambda case: case["path"][:40])
def test_invalid_paths_are_refused_with_their_code(case: dict) -> None:
    with pytest.raises(ValuePathError) as raised:
        parse_value_path(case["path"])
    assert raised.value.code == case["code"]


def test_values_are_read_and_replaced_at_a_path() -> None:
    goal = {"position_m": [0.1, 0.2, 0.3], "orientation": {"rpy_deg": [0, 0, 90]}}
    path = parse_value_path("goal.position_m[2]")
    assert value_at_path(goal, path) == 0.3
    assert replace_at_path(goal, path, -0.1)["position_m"] == [0.1, 0.2, -0.1]
    assert replace_at_path(goal, ("goal",), 7) == 7
    variables: dict = {}
    assert replace_at_path(variables, ("template_variables", "text"), "a") == {"text": "a"}
    with pytest.raises(KeyError):
        replace_at_path(goal, parse_value_path("goal.position_m[3]"), 0.0)
    with pytest.raises(KeyError):
        value_at_path(goal, parse_value_path("goal.position_m[3]"))
    with pytest.raises(KeyError):
        value_at_path(goal, parse_value_path("goal.orientation.xyzw"))
    assert path_covers(("goal",), path)
    assert not path_covers(path, ("goal",))
