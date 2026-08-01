"""Focused tests for retained MCP key normalization."""

from __future__ import annotations

from manyforge_composer.backend.mcp_client_primitives import (
    camel_to_snake,
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
