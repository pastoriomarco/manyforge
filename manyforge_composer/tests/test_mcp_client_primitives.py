"""Focused tests for retained MCP key normalization."""

from __future__ import annotations

import pytest

from manyforge_composer.backend.mcp_client_primitives import (
    MCP_HEADLESS_READ_TOOLS,
    McpHeadlessReadError,
    camel_to_snake,
    invoke_headless_read_tool,
    snakecase_keys,
)


def test_camel_to_snake_handles_acronyms_and_boundaries() -> None:
    assert camel_to_snake("runtimeGenerationId") == "runtime_generation_id"
    assert camel_to_snake("HTTPStatus") == "http_status"
    assert camel_to_snake("already_snake") == "already_snake"


def test_snakecase_keys_recurses_without_changing_values() -> None:
    payload = {
        "runtimeGenerationId": "generation-1",
        "nestedItems": [{"HTTPStatus": 200}],
    }

    assert snakecase_keys(payload) == {
        "runtime_generation_id": "generation-1",
        "nested_items": [{"http_status": 200}],
    }
    assert payload["nestedItems"][0] == {"HTTPStatus": 200}


class RecordingReader:
    def __init__(self) -> None:
        self.calls = []

    def invoke(self, operation, arguments=None):
        self.calls.append((operation, arguments))
        return {"schema_version": "manyforge.historian_summary_page.v1"}


def test_mcp_adapter_exposes_only_the_closed_h0_read_catalog() -> None:
    reader = RecordingReader()

    result = invoke_headless_read_tool(
        reader,
        "manyforge_event_history",
        {"limit": 4},
    )

    assert result["schema_version"] == "manyforge.historian_summary_page.v1"
    assert reader.calls == [("history", {"limit": 4})]
    assert MCP_HEADLESS_READ_TOOLS["manyforge_node_environment"] == (
        "node_environment"
    )
    assert "manyforge_manager_apply" not in MCP_HEADLESS_READ_TOOLS
    with pytest.raises(McpHeadlessReadError, match="unsupported"):
        invoke_headless_read_tool(reader, "manyforge_run_program", {})
    assert len(reader.calls) == 1
