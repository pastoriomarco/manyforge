"""Composer forwards the GUI I/O read and Mock simulation unchanged."""

from typing import Any

from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_headless_api.client import HeadlessClientError

BINDING = "sha256:" + "b" * 64


class _Headless:
    def __init__(self) -> None:
        self.calls: list[tuple[str, dict[str, Any]]] = []
        self.refuse = False

    def read_io_signal(self, body):
        self.calls.append(("read", body))
        return {"ok": True, "command": "query_target", "target_observation": {}}

    def set_mock_io_input(self, body):
        self.calls.append(("mock", body))
        if self.refuse:
            raise HeadlessClientError(
                "io_mock_input_required", "Signal is not an installed Mock input", 409
            )
        return {"ok": True, "command": "set_mock_io_input", "target_observation": {}}


def _app(headless: _Headless) -> TestClient:
    app = FastAPI()
    app.include_router(create_headless_router(headless, object()))  # type: ignore[arg-type]
    return TestClient(app)


def _read() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.target_observation_query.v2",
        "owner_epoch": "owner.1",
        "program_binding_id": BINDING,
        "kind": "io_signal",
        "signal": {"component_id": "station", "signal_name": "part_present"},
    }


def _mock() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.mock_io_input_request.v2",
        "owner_epoch": "owner.1",
        "program_binding_id": BINDING,
        "signal": {"component_id": "station", "signal_name": "part_present"},
        "expected_source_epoch": "2",
        "value": False,
    }


def test_composer_io_routes_are_strict_stateless_forwarders() -> None:
    headless = _Headless()
    with _app(headless) as client:
        read = client.post("/api/execution/io-signals/read", json=_read())
        mock = client.post("/api/execution/io-signals/mock-input", json=_mock())
        forged = client.post(
            "/api/execution/io-signals/mock-input",
            json={**_mock(), "trusted_principal_id": "forged"},
        )
        wrong = client.post(
            "/api/execution/io-signals/read", json={**_read(), "kind": "gripper_status"}
        )
    assert read.status_code == 200 and mock.status_code == 200
    assert forged.status_code == 422 and wrong.status_code == 422
    assert headless.calls == [("read", _read()), ("mock", _mock())]


def test_composer_keeps_the_host_refusal_code() -> None:
    headless = _Headless()
    headless.refuse = True
    with _app(headless) as client:
        response = client.post("/api/execution/io-signals/mock-input", json=_mock())
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "io_mock_input_required"
