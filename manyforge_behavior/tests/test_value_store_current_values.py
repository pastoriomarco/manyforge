"""The value store's display read of every declared key (OD-353)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import ProgramValueStore
from manyforge_behavior.program import Program

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def _store() -> ProgramValueStore:
    document: dict[str, Any] = load_strict_json_yaml(
        _ROOT / "examples/pick_and_place_minimal.program.yaml"
    )
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {"key": "count", "type": "int64", "access": "read_write",
             "persistence": "run", "initial": 2},
            {"key": "label", "type": "string", "access": "write", "persistence": "run"},
            {"key": "twice", "type": "int64", "access": "read", "persistence": "run",
             "expression": "key.count * 2"},
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v4",
    )
    return ProgramValueStore(Program.from_dict(document, _CATALOG))


def test_every_declared_key_is_listed_in_key_order_with_its_type_and_value() -> None:
    store = _store()
    rows = store.current_values()

    assert [row["key"] for row in rows] == sorted(row["key"] for row in rows)
    by_key = {row["key"]: row for row in rows}
    assert by_key["count"] == {
        "key": "count", "type": "int64", "value_state": "value", "value": 2,
    }
    # Computed keys are evaluated in the same view; a write-only key is shown.
    assert by_key["twice"] == {
        "key": "twice", "type": "int64", "value_state": "value", "value": 4,
    }
    assert by_key["label"] == {"key": "label", "type": "string", "value_state": "no_value"}
    # System keys are declared keys too.
    assert by_key["node_outcome.move_to_pick"]["value"] == "none"

    store.write("count", 5)
    assert {row["key"]: row for row in store.current_values()}["twice"]["value"] == 10


def test_a_value_beyond_the_diagnostic_bounds_is_omitted_not_clipped() -> None:
    store = _store()
    store.write("label", "x" * 2049)

    row = {row["key"]: row for row in store.current_values()}["label"]
    assert row == {"key": "label", "type": "string", "value_state": "omitted"}
