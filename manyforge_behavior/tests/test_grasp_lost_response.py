"""The attach node's literal grasp-lost response (DX-2563, contract §1)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program, ProgramValidationError

_ROOT = Path(__file__).resolve().parents[2]
_DEFAULT = {
    "raise_alarm": True,
    "stop_motion": True,
    "set_key": None,
    "detach": True,
}


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


def _attach(document: dict[str, Any]) -> dict[str, Any]:
    pending = [document["tree"]]
    while pending:
        node = pending.pop()
        if node["node_id"] == "scene.attach_carried_object":
            return node
        pending.extend(node.get("children", []))
    raise AssertionError("example Program has no attach node")


def _program(
    keys: tuple[dict[str, Any], ...] = (),
    *,
    on_grasp_lost: Any = ...,
    value_sources: dict[str, Any] | None = None,
) -> Program:
    catalog = _catalog()
    document = Program(
        _ROOT / "examples/pick_and_place.program.yaml", catalog
    ).to_dict()
    document["blackboard"]["keys"] = sorted(
        [*document["blackboard"]["keys"], *keys], key=lambda row: row["key"]
    )
    attach = _attach(document)
    if on_grasp_lost is not ...:
        attach["parameters"]["on_grasp_lost"] = on_grasp_lost
    if value_sources is not None:
        attach["value_sources"] = value_sources
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, catalog)


def test_omitted_response_keeps_defaults_and_legacy_program_shape() -> None:
    parameter = next(
        row
        for row in _catalog().get("scene.attach_carried_object").parameters
        if row.id == "on_grasp_lost"
    )
    assert parameter.required is False
    assert parameter.default == _DEFAULT
    parsed = _program()
    attach = _attach(parsed.to_dict())
    assert "on_grasp_lost" not in attach["parameters"]


def test_set_key_requires_a_declared_writable_bool_key() -> None:
    _program(
        keys=(_key("part_lost", "bool", initial=False),),
        on_grasp_lost={"set_key": "part_lost"},
    )
    _program(
        keys=(_key("part_lost", "bool", access="write"),),
        on_grasp_lost={"set_key": "part_lost"},
    )
    _program(on_grasp_lost={"set_key": None})


@pytest.mark.parametrize(
    "keys,response,message",
    [
        ((), {"set_key": "part_lost"}, "does not name a declared blackboard key"),
        (
            (_key("part_lost", "int64"),),
            {"set_key": "part_lost"},
            "declares type 'int64', expected 'bool'",
        ),
        (
            (_key("part_lost", "bool", access="read"),),
            {"set_key": "part_lost"},
            "does not permit write access",
        ),
        (
            (_key("part_lost", "bool", access="read", expression="true"),),
            {"set_key": "part_lost"},
            "names computed key 'part_lost'",
        ),
        (
            (_key("part_lost", "bool"),),
            {"set_key": "part_lost", "raise_alarm": True, "stop_motion": False},
            "must be true when raise_alarm is true",
        ),
    ],
)
def test_response_rejects_invalid_key_and_alarm_stop_combinations(
    keys, response, message
) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(keys=keys, on_grasp_lost=response)


@pytest.mark.parametrize(
    "response,message",
    [
        ({"set_key": False}, "must be a string"),
        ({"set_key": "missing", "extra": True}, "must not contain unknown fields"),
        ({"raise_alarm": 1}, "must be a boolean"),
    ],
)
def test_response_is_a_closed_literal_object(response, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(on_grasp_lost=response)


def test_response_flags_are_independent_when_alarm_is_disabled() -> None:
    _program(
        on_grasp_lost={
            "raise_alarm": False,
            "stop_motion": False,
            "detach": False,
        }
    )


def test_response_object_cannot_be_supplied_by_a_value_source() -> None:
    with pytest.raises(ProgramValidationError, match="cannot dynamically select"):
        _program(
            value_sources={
                "on_grasp_lost": {
                    "kind": "blackboard_key",
                    "reference": "response",
                }
            }
        )
