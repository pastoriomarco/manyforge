from __future__ import annotations

from collections.abc import Callable
import json
import os
from pathlib import Path
import socket
import threading
from typing import Any

from fastapi.testclient import TestClient
import pytest
import rfc8785

from manyforge_headless_api.app import create_app
from manyforge_headless_api.cclc import (
    CCLC_OPEN_RESPONSE_VERSION,
    CCLC_SOCKET_FILE,
    CCLCOpenRequestClient,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService


def _response(command: str, result: dict[str, Any] | None) -> dict[str, Any]:
    return {
        "api_version": CCLC_OPEN_RESPONSE_VERSION,
        "command": command,
        "error": None,
        "ok": True,
        "result": result,
    }


def _serve_once(
    root: Path,
    handler: Callable[[dict[str, Any]], bytes],
) -> threading.Thread:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(root / CCLC_SOCKET_FILE))
    os.chmod(root / CCLC_SOCKET_FILE, 0o600)
    listener.listen(1)

    def run() -> None:
        try:
            connection, _address = listener.accept()
            with connection:
                request = connection.recv(4 * 1024 * 1024)
                connection.send(handler(json.loads(request)))
        finally:
            listener.close()

    thread = threading.Thread(target=run)
    thread.start()
    return thread


def test_client_forwards_one_canonical_addressed_request(tmp_path: Path) -> None:
    observed: list[dict[str, Any]] = []

    def handler(request: dict[str, Any]) -> bytes:
        observed.append(request)
        return rfc8785.dumps(
            _response(
                "component_snapshot",
                {
                    "controllable_component_id": "component.arm",
                    "component_lifecycle_generation": 4,
                },
            )
        )

    thread = _serve_once(tmp_path, handler)
    result = CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert result["component_lifecycle_generation"] == 4
    assert observed == [
        {
            "api_version": "manyforge.cclc_open_request.v1",
            "command": "component_snapshot",
            "payload": {"controllable_component_id": "component.arm"},
        }
    ]


def test_client_reads_the_cclc_owner_identity(tmp_path: Path) -> None:
    observed: list[dict[str, Any]] = []

    def handler(request: dict[str, Any]) -> bytes:
        observed.append(request)
        return rfc8785.dumps(
            _response(
                "owner_identity",
                {"request_owner_epoch": "cclc-owner.test"},
            )
        )

    thread = _serve_once(tmp_path, handler)
    result = CCLCOpenRequestClient(tmp_path).owner_identity()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert result == {"request_owner_epoch": "cclc-owner.test"}
    assert observed[0]["command"] == "owner_identity"
    assert observed[0]["payload"] == {}


def test_client_rejects_owner_error_and_noncanonical_response(tmp_path: Path) -> None:
    def error_handler(request: dict[str, Any]) -> bytes:
        return rfc8785.dumps(
            {
                "api_version": CCLC_OPEN_RESPONSE_VERSION,
                "command": request["command"],
                "error": {
                    "code": "missing_reference",
                    "details": {"effect": "not_started"},
                    "message": "component is absent",
                },
                "ok": False,
                "result": None,
            }
        )

    thread = _serve_once(tmp_path, error_handler)
    with pytest.raises(HeadlessApiError, match="component is absent") as captured:
        CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    assert captured.value.code == "missing_reference"
    assert captured.value.details == {"effect": "not_started"}
    thread.join(timeout=2)
    (tmp_path / CCLC_SOCKET_FILE).unlink()

    thread = _serve_once(
        tmp_path,
        lambda _request: b'{"ok": true, "api_version":"invalid"}',
    )
    with pytest.raises(HeadlessApiError) as captured:
        CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    assert captured.value.code == "cclc_open_response_invalid"
    thread.join(timeout=2)


def _lifecycle_request(*, confirmed: bool) -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "request.lifecycle.1",
            "request_owner_epoch": "cli.epoch.1",
            "source_label": "test-client",
            "correlation_id": None,
        },
        "controllable_component_id": "component.arm",
        "intent": "enable",
        "expected_graph_revision": "graph.1",
        "expected_component_lifecycle_generation": 4,
        "expected_control_domains": [
            {
                "control_domain_id": "domain.arm",
                "control_domain_revision": "domain.revision.1",
                "controller_topology_id": "topology.arm",
                "topology_revision": "topology.revision.1",
                "expected_active_control_configuration": {
                    "presence": "absent",
                    "active_control_configuration_id": None,
                    "active_control_configuration_generation": None,
                },
                "expected_control_bindings": [
                    {
                        "control_binding_id": "binding.arm",
                        "control_binding_revision": "binding.revision.1",
                        "control_route_id": "route.arm",
                    }
                ],
            }
        ],
        "reason": "operator-confirmed",
        "confirmed": confirmed,
        "expected_admission_generation": 7,
    }


class FakeCCLC:
    def __init__(self) -> None:
        self.lifecycle: list[dict[str, Any]] = []

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        return {"controllable_component_id": component_id}

    def owner_identity(self) -> dict[str, Any]:
        return {"request_owner_epoch": "cclc-owner.http-test"}

    def route_templates(self, component_id: str) -> dict[str, Any]:
        return {
            "controllable_component_id": component_id,
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
            "templates": [],
        }

    def submit_lifecycle(self, intent: dict[str, Any]) -> dict[str, Any]:
        self.lifecycle.append(intent)
        return {
            "acceptance": None,
            "replay_status": "request_id_conflict",
            "status": None,
        }

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None:
        return None

    def lifecycle_request_result(self, intent):
        return {
            "replay_status": "replayed_exact",
            "acceptance": None,
            "status": None,
            "rejection": {
                "code": "invalid_state_transition",
                "message": "Held",
                "details": {
                    "effect": "not_started",
                    "reason": "program_node_stop_fence_active",
                    "selection_revision": "7",
                },
            },
        }

    def submit_route(self, intent: dict[str, Any]) -> dict[str, Any]:
        return {
            "acceptance": None,
            "replay_status": "request_ledger_full",
            "status": None,
        }

    def route_request_result(self, intent):
        self.route_result_request = intent
        return None

    def route_status(self, transaction_id: str) -> dict[str, Any] | None:
        return None


class FakeAdmission:
    def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
        return {
            "intent": intent,
            "claim_generation": 1,
            "acquired_admission_generation": 7,
            "accepted_at_ns": 1,
        }

    def release(self, _release: dict[str, Any]) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 8}


def test_http_lifecycle_route_is_thin_and_requires_confirmation() -> None:
    fake = FakeCCLC()
    service = HeadlessService(  # type: ignore[arg-type]
        object(), object(), cell_admission=FakeAdmission()
    )
    client = TestClient(create_app(service=service, cclc=fake))

    rejected = client.post(
        "/api/cclc/lifecycle/requests",
        json=_lifecycle_request(confirmed=False),
    )
    assert rejected.status_code == 400
    assert rejected.json()["detail"]["code"] == "cclc_confirmation_required"
    assert fake.lifecycle == []

    accepted = client.post(
        "/api/cclc/lifecycle/requests",
        json=_lifecycle_request(confirmed=True),
    )
    assert accepted.status_code == 200
    assert accepted.json()["replay_status"] == "request_id_conflict"
    assert fake.lifecycle[0]["expected_admission_generation"] == 7
    assert len(fake.lifecycle) == 1
    assert "confirmed" not in fake.lifecycle[0]
    result = client.post(
        "/api/cclc/lifecycle/requests/result", json=_lifecycle_request(confirmed=False)
    )
    assert result.status_code == 200
    assert result.json()["result"]["rejection"]["details"]["selection_revision"] == "7"
    assert len(fake.lifecycle) == 1  # The lookup must never call submit.

    route_request = {
        "request_context": _lifecycle_request(confirmed=False)["request_context"],
        "expected_admission_generation": 7,
        "expected_retained_bundle_id": "bundle.1",
        "expected_graph_revision": "graph.1",
        "desired_configuration": {
            "control_domain_id": "domain.arm",
            "control_domain_revision": "domain.revision.1",
            "controller_topology_id": "topology.arm",
            "topology_revision": "topology.revision.1",
            "activation_set_id": "activation.arm",
            "binding_routes": [
                {
                    "control_binding_id": "binding.arm",
                    "control_binding_revision": "binding.revision.1",
                    "control_route_id": "route.arm",
                }
            ],
        },
        "expected_active_control_configuration": {"presence": "absent"},
        "component_expectations": [
            {
                "controllable_component_id": "component.arm",
                "expected_component_lifecycle_generation": 4,
                "expected_component_lifecycle_state": "active",
            }
        ],
        "confirmed": False,
    }
    # This service fixture has no authoring read. Result observation must not
    # ask for current Idle, confirm or resubmit the original command.
    route = client.post("/api/cclc/routes/requests/result", json=route_request)
    assert route.status_code == 200, route.text
    assert route.json() == {"result": None}
    assert (
        fake.route_result_request["request_context"]["request_owner_epoch"]
        == "cli.epoch.1"
    )
    assert "confirmed" not in fake.route_result_request
    assert fake.route_result_request["expected_admission_generation"] == 7
    # Legacy lookup preserves absence; it is not a new command or a request
    # to silently substitute the current generation.
    del route_request["expected_admission_generation"]
    assert client.post("/api/cclc/routes/requests/result", json=route_request).status_code == 200
    assert "expected_admission_generation" not in fake.route_result_request
    route_request["confirmed"] = True
    assert client.post("/api/cclc/routes/requests", json=route_request).status_code == 422

    snapshot = client.get("/api/cclc/components/component.arm")
    assert snapshot.json() == {"controllable_component_id": "component.arm"}
    assert client.get("/api/cclc/owner").json() == {
        "request_owner_epoch": "cclc-owner.http-test"
    }
    templates = client.get("/api/cclc/components/component.arm/route-templates")
    assert templates.json()["templates"] == []
    assert client.get("/api/cclc/lifecycle/transactions/lifecycle.1").json() == {
        "status": None
    }

    invalid = _lifecycle_request(confirmed=True)
    invalid["caller_asserted_principal"] = "operator"
    assert client.post("/api/cclc/lifecycle/requests", json=invalid).status_code == 422
