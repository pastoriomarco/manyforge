"""System-owned motion group state keys (OD-339)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import ProgramValueError, ProgramValueStore
from manyforge_behavior.program import (
    MOTION_GROUP_STATE_VALUES,
    Program,
    ProgramValidationError,
    motion_group_state_key,
    program_value_declarations,
)

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_ALL = "motion_group_state.all"
_ARM = "motion_group_state.arm"


def _document(*, arm_group: bool = False) -> dict[str, Any]:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    if arm_group:
        document["tree"]["children"][0]["execution_groups"] = ["arm"]
        document["execution_groups"] = {"arm": {"execution_policy": "independent"}}
    return document


def _program(document: dict[str, Any]) -> Program:
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v4",
    )
    return Program.from_dict(document, _CATALOG)


def _append(document: dict[str, Any], node: dict[str, Any]) -> None:
    document["tree"]["children"].append({"children": [], **node})


def test_every_motion_group_has_an_implicit_read_only_state_key() -> None:
    program = _program(_document(arm_group=True))

    assert motion_group_state_key("arm") == _ARM
    keys = [key for key in program.blackboard_keys if key.startswith("motion_group_state.")]
    # One per group of the runtime controls: every authored group and `all`.
    assert keys == sorted(f"motion_group_state.{group}" for group in program.execution_selections)
    assert keys == [_ALL, _ARM]
    declaration = program.blackboard_keys[_ARM]
    assert (declaration.type, declaration.access, declaration.persistence) == (
        "string",
        "read",
        "run",
    )
    # No current Run until the owner writes the run's first facts.
    assert (declaration.has_initial, declaration.initial) == (True, "unavailable")
    assert declaration.expression is None
    assert _ARM not in str(program.to_dict())


def test_a_program_without_named_groups_has_the_all_key_only() -> None:
    program = _program(_document())
    assert [
        key for key in program.blackboard_keys if key.startswith("motion_group_state.")
    ] == [_ALL]


def test_nodes_read_a_group_state_key_like_any_declared_key() -> None:
    document = _document(arm_group=True)
    _append(
        document,
        {
            "node_id": "logic.check_value",
            "instance_id": "arm_stopped",
            "parameters": {"key": _ARM, "value": "held"},
        },
    )
    assert _ARM in _program(document).blackboard_keys


def test_reading_the_state_of_a_group_the_program_does_not_have_is_refused() -> None:
    document = _document()
    _append(
        document,
        {
            "node_id": "logic.check_value",
            "instance_id": "arm_stopped",
            "parameters": {"key": _ARM, "value": "held"},
        },
    )
    with pytest.raises(ProgramValidationError, match="does not name a declared blackboard key"):
        _program(document)


def test_an_authored_write_to_a_group_state_key_is_refused() -> None:
    document = _document()
    _append(
        document,
        {
            "node_id": "logic.set_value",
            "instance_id": "forge_state",
            "parameters": {"key": _ALL, "value": "held"},
        },
    )
    with pytest.raises(ProgramValidationError, match="system-owned motion group state key"):
        _program(document)


@pytest.mark.parametrize(
    "mutate, message",
    [
        (
            lambda bb: bb["namespaces"].append(
                {"namespace_id": "motion_group_state", "owner": "program"}
            ),
            "motion_group_state is reserved",
        ),
        (
            lambda bb: bb["keys"].append(
                {"key": _ALL, "type": "string", "access": "read_write", "persistence": "run"}
            ),
            "reserved motion_group_state namespace",
        ),
    ],
)
def test_an_authored_declaration_in_the_reserved_namespace_is_refused(mutate, message) -> None:
    document = _document()
    blackboard = document.setdefault("blackboard", {"namespaces": [], "keys": []})
    mutate(blackboard)
    with pytest.raises(ProgramValidationError, match=message):
        _program(document)


def test_draft_declarations_include_the_forest_groups() -> None:
    document = _document(arm_group=True)
    document["forest"] = [document.pop("tree")]
    _parameters, keys = program_value_declarations(document, catalog=_CATALOG)
    assert {_ALL, _ARM} <= set(keys)


def test_only_the_system_writer_sets_a_group_state() -> None:
    program = _program(_document(arm_group=True))
    store = ProgramValueStore(program)
    assert store.read(_ARM) == "unavailable"
    with pytest.raises(ProgramValueError, match="not writable"):
        store.write(_ARM, "held")
    with pytest.raises(ProgramValueError, match="not a motion group state"):
        store.write_motion_group_state("arm", "stopped")
    with pytest.raises(ProgramValueError, match="undeclared"):
        store.write_motion_group_state("tools", "held")
    for state in MOTION_GROUP_STATE_VALUES:
        store.write_motion_group_state("arm", state)
        assert store.read(_ARM) == state
    # The owner is its only writer: a value reset leaves it as written.
    store.write_motion_group_state("arm", "held")
    store.clear_run_values()
    assert store.read(_ARM) == "held"
