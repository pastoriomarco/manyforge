"""What a refusal waits for reaches the request log, never free text."""

from __future__ import annotations

import json

from fastapi.testclient import TestClient

from manyforge_composer.backend.app import _structured_error_details, create_app
from manyforge_composer.backend.state import ComposerState


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


def test_unhandled_exception_keeps_a_typed_500_without_exposing_exception() -> None:
    app = create_app(ComposerState())

    def failing_route() -> None:
        raise RuntimeError("private internal details")

    app.add_api_route("/test-unhandled", failing_route, methods=["GET"])
    app.router.routes.insert(0, app.router.routes.pop())
    with TestClient(app, raise_server_exceptions=False) as client:
        response = client.get("/test-unhandled", headers={"X-Request-ID": "request-500"})
    assert response.status_code == 500
    assert response.json()["detail"] == {
        "code": "composer_internal_failure",
        "message": "Internal Server Error",
        "details": {},
        "producer": "composer",
        "request_id": "request-500",
    }
    assert "private internal details" not in response.text
