"""System-owned global rule state keys interrupt_state.<rule_id> (contracts 63, 64)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    program_value_declarations,
    require_interrupt_state_rules,
)

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_KEY = "interrupt_state.light_curtain"


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


def _append(document: dict[str, Any], node_id: str, instance_id: str, **parameters: Any) -> None:
    document["tree"]["children"].append(
        {"children": [], "node_id": node_id, "instance_id": instance_id, "parameters": parameters}
    )


def test_a_program_check_on_a_rule_state_validates_as_a_read_only_key() -> None:
    document = _document()
    _append(document, "logic.check_value", "curtain_clear", key=_KEY, value="clear")
    program = _program(document)

    declaration = program.blackboard_keys[_KEY]
    assert (declaration.type, declaration.access) == ("string", "read")
    assert "clear | tripped | latched" in declaration.description
    assert program.interrupt_state_rule_ids == ("light_curtain",)
    # The authored document declares nothing; only the validation view does.
    assert all(row["key"] != _KEY for row in program.to_dict().get("blackboard", {}).get("keys", []))
    # Only referenced rules are declared.
    assert [key for key in program.blackboard_keys if key.startswith("interrupt_state.")] == [_KEY]


def test_a_write_to_a_rule_state_key_stays_refused() -> None:
    document = _document()
    _append(document, "logic.set_value", "forge_state", key=_KEY, value="clear")
    with pytest.raises(ProgramValidationError, match="system-owned interrupt rule state key"):
        _program(document)


def test_a_computed_key_may_combine_rule_states() -> None:
    document = _document()
    blackboard = document.setdefault("blackboard", {"namespaces": [], "keys": []})
    blackboard["namespaces"].append({"namespace_id": "cell", "owner": "program"})
    blackboard["namespaces"].sort(key=lambda row: row["namespace_id"])
    blackboard["keys"].append({
        "key": "cell.curtain_latched", "type": "bool", "access": "read", "persistence": "run",
        "expression": f'key.{_KEY} == "latched"',
    })
    blackboard["keys"].sort(key=lambda row: row["key"])
    program = _program(document)
    assert program.interrupt_state_rule_ids == ("light_curtain",)


def test_draft_declarations_include_referenced_rule_states() -> None:
    document = _document()
    _append(document, "logic.check_value", "curtain_clear", key=_KEY, value="clear")
    document["forest"] = [document.pop("tree")]
    _parameters, keys = program_value_declarations(document, catalog=_CATALOG)
    assert _KEY in keys


def test_the_deployment_join_refuses_an_undeclared_rule() -> None:
    document = _document()
    _append(document, "logic.check_value", "curtain_clear", key=_KEY, value="clear")
    program = _program(document)
    require_interrupt_state_rules(program, ["light_curtain", "part_lost"])
    with pytest.raises(ProgramValidationError, match="does not declare: light_curtain"):
        require_interrupt_state_rules(program, ["part_lost"])
    # The Runner's Applied rule ids extend the declarations and keep the reads.
    applied = program.with_interrupt_state_keys(["light_curtain", "part_lost"])
    assert "interrupt_state.part_lost" in applied.blackboard_keys
    assert applied.interrupt_state_rule_ids == ("light_curtain",)
