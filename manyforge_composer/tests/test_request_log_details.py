"""What a refusal waits for reaches the request log, never free text."""

from __future__ import annotations

import json

from manyforge_composer.backend.app import _structured_error_details


def _body(details: object) -> bytes:
    return json.dumps({"detail": {
        "code": "execution_rejected", "message": "free text", "details": details,
    }}).encode()


def test_identifier_details_of_a_refusal_are_logged() -> None:
    assert _structured_error_details(_body({
        "blocking_conditions": "operations_active,resources_not_free",
        "active_operation_ids": "operation.7",
        "busy_resource_ids": "resource.arm/joints,io.output.9257de14",
    })) == {
        "active_operation_ids": "operation.7",
        "blocking_conditions": "operations_active,resources_not_free",
        "busy_resource_ids": "resource.arm/joints,io.output.9257de14",
    }


def test_free_text_nested_or_oversized_details_are_never_logged() -> None:
    assert _structured_error_details(_body({
        "reason": "contains spaces and user text",
        "nested": {"a": "b"},
        "Upper": "x",
        "long": "x" * 1025,
    })) is None
    assert _structured_error_details(_body("not an object")) is None
    assert _structured_error_details(b"not json") is None
    many = {f"key_{index}": "v" for index in range(20)}
    assert len(_structured_error_details(_body(many)) or {}) == 8
