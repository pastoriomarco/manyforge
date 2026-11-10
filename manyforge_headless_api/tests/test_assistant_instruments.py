"""Assistant instruments built from the loaded node catalog (OD-75)."""

from __future__ import annotations

import json
from typing import Any

import pytest

from manyforge_headless_api.assistant_instruments import (
    INDEX_VERSION,
    MODE_CATALOG_VERSION,
    NODE_TOOL_IDS,
    GenerationError,
    _with_argument_types,
    framed_digest,
    schema_hash,
    summary,
)
from manyforge_headless_api.service import _assistant_instruments, _assistant_mode


def _native(tool_ids: list[str]) -> dict[str, Any]:
    return {
        "grant_policy": {"approval_mode": None, "review_policy": "required", "session_kind": None},
        "mode": "composer-assistant",
        "schema_version": "manyforge.assistant_mode_projection.v2",
        "assistant_component_id": "assistant",
        "runs_in": ["no_program_active"],
        "skills": [],
        "compact_history": False,
        "constrained_regeneration": False,
        "stuck_loop_recovery": True,
        "tools": [{"effect": "composer_draft_mutating", "tool_id": tool_id} for tool_id in tool_ids],
        "source": {"available": True},
    }


def _node_ids(projection: dict[str, Any]) -> list[str]:
    return [row["node_id"] for row in projection["nodes"]]


def test_node_set_is_stable_entries_whose_capabilities_the_deployment_offers() -> None:
    without_io = _assistant_mode(_native(["tree_draft_insert_node"]), [])
    io_offers = ["io.gpio.get_input", "io.gpio.set_output", "io.gpio.wait_for_input"]
    with_io = _assistant_mode(_native(["tree_draft_insert_node"]), io_offers)

    assert "sequence" in _node_ids(without_io)
    assert "io.set_output" not in _node_ids(without_io)
    assert "motion.move_to_pose" not in _node_ids(without_io)
    assert {"io.set_output", "io.get_input", "io.check_input", "io.wait_for_input"} <= set(
        _node_ids(with_io)
    )
    # Experimental entries are never authorable by the assistant.
    everything = [entry.id for entry in _assistant_instruments().entries()]
    assert "example.check_frame_height" not in everything
    assert with_io["tools"][0]["argument_types"] == {"node": "node"}
    node_type = with_io["types"]["node"]
    node_id = node_type["properties"]["node_id"]
    if "$ref" in node_id:
        node_id = node_type["$defs"][node_id["$ref"].rsplit("/", 1)[-1]]
    enum = node_id["enum"]
    assert enum == _node_ids(with_io)
    assert with_io["catalog_hash"] != without_io["catalog_hash"]


def test_assistant_nodes_need_a_usable_model_query() -> None:
    assistant_nodes = {
        "assistant.build_scene_selection",
        "assistant.query_model",
        "assistant.validate_scene_selection",
    }
    without = _assistant_mode(_native(["tree_draft_insert_node"]), [])
    with_query = _assistant_mode(
        _native(["tree_draft_insert_node"]), ["assistant.model.query"]
    )
    assert not assistant_nodes & set(_node_ids(without))
    assert assistant_nodes <= set(_node_ids(with_query))


def test_projection_digests_cover_the_generated_content() -> None:
    projection = _assistant_mode(_native(sorted(NODE_TOOL_IDS)), [])
    semantic = {key: value for key, value in projection.items() if key != "source"}
    assert projection["source"]["source_content_digest"] == framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v2",
        semantic,
    )
    assert projection["catalog_hash"] == framed_digest(
        "assistant-mode-catalog",
        MODE_CATALOG_VERSION,
        {key: projection[key] for key in ("grant_policy", "mode", "nodes", "skills", "tools")},
    )
    assert projection["instrument_registry_digest"] == _assistant_instruments().registry_digest
    for tool in projection["tools"]:
        assert tool["input_schema_hash"] == schema_hash(tool["input_schema"])
        assert tool["input_schema"]["type"] == "object"
        assert all("type" in value for value in tool["input_schema"]["properties"].values())
        # Small tools: the node schemas live in the node rows, not in the tools.
        assert len(json.dumps(tool["input_schema"])) < 8_000


def test_projection_carries_the_assistant_component_and_its_cell_states() -> None:
    projection = _assistant_mode(_native([]), [])
    assert projection["schema_version"] == "manyforge.assistant_mode_projection.v2"
    assert projection["assistant_component_id"] == "assistant"
    assert projection["runs_in"] == ["no_program_active"]
    assert "provider" not in projection


@pytest.mark.parametrize(
    "native",
    [
        {
            "ai_model_component_id": "assistant_model",
            "assistant_component_id": "assistant",
            "assistant_projection_digest": "sha256:" + "a" * 64,
            "modes": [
                {
                    "description": "Draft authoring",
                    "grant_policy": {},
                    "mode": "composer-assistant",
                    "runs_in": ["no_program_active"],
                }
            ],
        },
        # No assistant component: an empty index.
        {
            "ai_model_component_id": "assistant_model",
            "assistant_component_id": None,
            "assistant_projection_digest": "sha256:" + "a" * 64,
            "modes": [],
        },
        # No assistant settings in the bundle at all.
        {
            "ai_model_component_id": None,
            "assistant_component_id": None,
            "assistant_projection_digest": None,
            "modes": [],
        },
    ],
    ids=["assistant", "no-assistant-component", "no-assistant-settings"],
)
def test_index_v2_passes_native_fields_and_digests_them(native: dict[str, Any]) -> None:
    raw = {
        **native,
        "schema_version": INDEX_VERSION,
        "source": {"available": True},
    }
    index = _assistant_instruments().compose_index(raw)
    semantic = {key: value for key, value in index.items() if key != "source"}
    assert INDEX_VERSION == "manyforge.assistant_mode_index.v2"
    assert semantic == {
        key: value for key, value in raw.items() if key != "source"
    } | {"instrument_registry_digest": _assistant_instruments().registry_digest}
    assert index["source"]["source_content_digest"] == framed_digest(
        "manyforge-assistant-mode-index-semantic", INDEX_VERSION, semantic
    )


def test_node_rows_carry_summary_bounds_and_schemas() -> None:
    projection = _assistant_mode(_native([]), [])
    row = next(row for row in projection["nodes"] if row["node_id"] == "repeat")
    assert row["children"] == {"max": 1, "min": 1}
    assert row["summary"] == summary(row["summary"])
    assert row["node_schema"]["properties"]["node_id"] == {"const": "repeat"}
    assert "instance_id" not in row["replacement_schema"]["properties"]
    assert set(row["update_schema"]["properties"]) <= {"parameters", "value_sources"}


def test_summary_is_the_first_sentence() -> None:
    assert summary("Moves the arm. Fails when blocked.") == "Moves the arm."
    assert summary("One sentence only") == "One sentence only"


def test_isaac_inspection_is_offered_only_with_a_simulator() -> None:
    tools = ["inspect_isaac_scene", "scene_inspect"]
    mock = _assistant_mode(_native(tools), [])
    isaac = _assistant_mode(_native(tools), ["simulator.entity_state.set"])
    assert [row["tool_id"] for row in mock["tools"]] == ["scene_inspect"]
    assert [row["tool_id"] for row in isaac["tools"]] == tools


def test_tools_that_place_under_root_explain_the_root_alias() -> None:
    from manyforge_headless_api.assistant_instruments import ROOT_CLAUSE, _tools

    tools = _tools()
    for tool_id in ("tree_draft_insert_node", "tree_draft_move_node", "tree_draft_wrap_node"):
        assert ROOT_CLAUSE in tools[tool_id].description


def test_update_tool_says_how_one_node_overrides_a_bound_parameter() -> None:
    from manyforge_headless_api.assistant_instruments import _tools

    assert "own value for a bound parameter" in _tools()["tree_draft_update_node_params"].description


def test_no_projected_schema_refers_to_its_root() -> None:
    """A self-referential condition schema moves to `$defs`; "#" would name the node."""

    instruments = _assistant_instruments()
    projection = instruments.compose_mode(
        _native(sorted(NODE_TOOL_IDS)), [entry.id for entry in instruments.entries()])
    text = json.dumps(projection)
    assert '"$ref": "#"' not in text
    seek = next(row for row in projection["nodes"] if row["node_id"] == "motion.move_until_signal")
    schema = seek["node_schema"]
    assert schema["properties"]["parameters"]["properties"]["interrupt_condition"] == {
        "$ref": "#/$defs/component_interrupt_condition"}
    condition = schema["$defs"]["component_interrupt_condition"]
    assert condition["properties"]["not"] == {"$ref": "#/$defs/component_interrupt_condition"}


def test_bare_reference_and_one_of_arguments_get_their_implied_type() -> None:
    schema = {
        "type": "object",
        "additionalProperties": False,
        "$defs": {"node": {"type": "object", "properties": {"node_id": {"type": "string"}}}},
        "properties": {
            "node": {"$ref": "#/$defs/node"},
            "parent": {"oneOf": [{"type": "string"}, {"const": "@root"}]},
            "mixed": {"oneOf": [{"type": "string"}, {"type": "integer"}]},
        },
    }
    typed = _with_argument_types(schema)
    assert typed["properties"]["node"] == {"type": "object", "$ref": "#/$defs/node"}
    assert typed["properties"]["parent"]["type"] == "string"
    assert "type" not in typed["properties"]["mixed"]


def test_top_level_alternatives_become_one_closed_typed_object() -> None:
    schema = {
        "oneOf": [
            {"type": "object", "additionalProperties": False,
             "properties": {"instance_id": {"type": "string"}, "parameters": {"type": "object"}}},
            {"type": "object", "additionalProperties": False,
             "properties": {"instance_id": {"type": "string"}, "merge": {"type": "boolean"}}},
        ]
    }
    typed = _with_argument_types(schema)
    assert typed["type"] == "object"
    assert typed["additionalProperties"] is False
    assert typed["properties"] == {
        "instance_id": {"type": "string"},
        "merge": {"type": "boolean"},
        "parameters": {"type": "object"},
    }
    assert typed["oneOf"] == schema["oneOf"]


def test_top_level_alternatives_with_conflicting_types_are_refused() -> None:
    schema = {
        "oneOf": [
            {"type": "object", "properties": {"value": {"type": "string"}}},
            {"type": "object", "properties": {"value": {"type": "number"}}},
        ]
    }
    with pytest.raises(GenerationError):
        _with_argument_types(schema)
