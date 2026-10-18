"""System-owned node outcome keys (contract items 42, 43)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import ProgramValueError, ProgramValueStore
from manyforge_behavior.program import (
    NODE_ENDED_OUTCOMES,
    NODE_OUTCOME_VALUES,
    Program,
    ProgramValidationError,
    node_outcome_key,
    program_value_declarations,
)

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_KEY = "node_outcome.move_to_pick"


def _document() -> dict[str, Any]:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    return document


def _program(document: dict[str, Any]) -> Program:
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, _CATALOG)


def _append(document: dict[str, Any], node: dict[str, Any]) -> None:
    document["tree"]["children"].append({"children": [], **node})


def test_every_motion_node_has_an_implicit_read_only_run_outcome_key() -> None:
    program = _program(_document())

    declaration = program.blackboard_keys[_KEY]
    assert node_outcome_key("move_to_pick") == _KEY
    assert (declaration.type, declaration.access, declaration.persistence) == (
        "string",
        "read",
        "run",
    )
    # The system defines the start value `none` (contract 105).
    assert (declaration.has_initial, declaration.initial) == (True, "none")
    assert declaration.expression is None
    # Only motion nodes get one, and the authored document stays unchanged.
    assert [key for key in program.blackboard_keys if key.startswith("node_outcome.")] == [_KEY]
    assert _KEY not in str(program.to_dict())


def test_other_nodes_read_an_outcome_key_like_any_declared_key() -> None:
    document = _document()
    _append(
        document,
        {
            "node_id": "logic.check_value",
            "instance_id": "check_move",
            "parameters": {"key": _KEY, "value": "completed"},
        },
    )
    assert _KEY in _program(document).blackboard_keys


@pytest.mark.parametrize(
    "mutate, message",
    [
        (
            lambda bb: bb["namespaces"].append({"namespace_id": "node_outcome", "owner": "program"}),
            "node_outcome is reserved",
        ),
        (
            lambda bb: bb["keys"].append(
                {"key": _KEY, "type": "string", "access": "read_write", "persistence": "run"}
            ),
            "reserved node_outcome namespace",
        ),
    ],
)
def test_an_authored_declaration_in_the_reserved_namespace_is_refused(mutate, message) -> None:
    document = _document()
    blackboard = document.setdefault("blackboard", {"namespaces": [], "keys": []})
    mutate(blackboard)
    with pytest.raises(ProgramValidationError, match=message):
        _program(document)


def test_an_authored_write_to_an_outcome_key_is_refused() -> None:
    document = _document()
    _append(
        document,
        {
            "node_id": "logic.set_value",
            "instance_id": "forge_outcome",
            "parameters": {"key": _KEY, "value": "completed"},
        },
    )
    with pytest.raises(ProgramValidationError, match="system-owned node outcome key"):
        _program(document)


def test_draft_declarations_include_the_forest_motion_nodes() -> None:
    document = _document()
    tree = document.pop("tree")
    document["forest"] = [tree]
    _parameters, keys = program_value_declarations(document, catalog=_CATALOG)
    assert _KEY in keys


def test_only_the_system_writer_sets_an_outcome_and_reset_clears_it() -> None:
    program = _program(_document())
    store = ProgramValueStore(program)
    assert store.read(_KEY) == "none"  # No result yet in this Run (contract 105).
    with pytest.raises(ProgramValueError, match="not writable"):
        store.write(_KEY, "completed")
    for refused in ("aborted", "none"):  # `none` is the start value, never an ending.
        with pytest.raises(ProgramValueError, match="not a node outcome"):
            store.write_node_outcome("move_to_pick", refused)
    with pytest.raises(ProgramValueError, match="undeclared"):
        store.write_node_outcome("remove_table", "completed")

    assert NODE_OUTCOME_VALUES[-1] == "none"
    for outcome in NODE_ENDED_OUTCOMES:
        store.write_node_outcome("move_to_pick", outcome, metadata={"node_attempt_id": "a"})
        assert store.read(_KEY) == outcome
    # A new run's store starts at `none` again.
    assert ProgramValueStore(program).read(_KEY) == "none"
    # Reset (the between-Run store) returns it to `none`; other run keys empty.
    store.clear_run_values()
    assert store.read(_KEY) == "none"


@pytest.mark.parametrize("name", ["node_outcome", "interrupt_state", "node_outcome.pick", "interrupt_state.guard"])
def test_program_parameters_cannot_use_a_system_name(name) -> None:
    document = _document()
    document.setdefault("parameters", []).append({"parameter_id": name, "type": "bool", "default": False})
    document["parameters"].sort(key=lambda row: row["parameter_id"])
    with pytest.raises(ProgramValidationError, match="reserved system name"):
        _program(document)


@pytest.mark.parametrize("name", ["node_outcome", "interrupt_state"])
def test_a_root_key_named_like_a_system_namespace_is_refused(name) -> None:
    document = _document()
    blackboard = document.setdefault("blackboard", {"namespaces": [], "keys": []})
    blackboard["keys"].append({"key": name, "type": "bool", "access": "read_write", "persistence": "run"})
    blackboard["keys"].sort(key=lambda row: row["key"])
    with pytest.raises(ProgramValidationError, match=f"reserved {name} namespace"):
        _program(document)

