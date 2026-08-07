from __future__ import annotations

import asyncio
import json
from typing import Any, Iterator

from fastapi.testclient import TestClient
from fastapi import WebSocketDisconnect
import httpx
import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend import headless_ws
from manyforge_composer.backend.headless_ws import serve_headless_state
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


def _runtime() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.headless_runtime_snapshot.v1",
        "complete": True,
        "projection_owner_epoch": "projection.epoch.1",
        "projection_generation": 4,
    }


def _alarms() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.operator_alarm_snapshot.v2",
        "owner_epoch": "alarm.epoch.1",
        "generation": 3,
        "state_revision": 5,
        "alarms": [],
        "unacknowledged_membership": [],
    }


@pytest.fixture
def headless_client() -> Iterator[
    tuple[HeadlessHttpClient, list[dict[str, Any]], dict[str, Any]]
]:
    requests: list[dict[str, Any]] = []
    environment = build_node_environment()
    manifest = environment.manifest
    node_environment = {
        "schema_version": "manyforge.node_environment_projection.v1",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "catalog_id": manifest.catalog_id,
        "content_version": manifest.catalog_content_version,
        "catalog_digest": manifest.catalog_digest,
        "environment_digest": manifest.environment_digest,
        "catalog": environment.catalog.raw_data,
        "manifest": manifest.to_dict(),
        "availability": {
            "binding_context": "unselected",
            "execution_available": False,
            "implementations": [
                {
                    "authoring_available": True,
                    "engine_id": row.engine_id,
                    "executable_package_id": row.executable_package_id,
                    "execution_available": False,
                    "implementation_id": row.implementation_id,
                    "implementation_identity_resolved": True,
                    "node_id": row.node_id,
                    "reason_codes": [
                        "program_binding_unselected",
                        "program_runtime_owner_unavailable",
                    ],
                }
                for row in manifest.executable_registry
            ],
            "manager_runtime_converged": True,
            "portable_authoring_available": True,
            "reason_codes": [
                "program_binding_unselected",
                "program_runtime_owner_unavailable",
            ],
            "restoration_gates": ["I6", "N"],
        },
        "source": {
            "projection_generation": 4,
            "projection_owner_epoch": "projection.epoch.1",
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "receiver_age_ns": 10,
            "source_generation": 2,
            "source_owner_epoch": "manager.epoch.1",
            "source_owner_id": "component-manager.cell-a",
            "source_content_digest": "sha256:" + "a" * 64,
        },
    }

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content) if request.content else None
        requests.append(
            {
                "method": request.method,
                "path": request.url.path,
                "query": request.url.query.decode("ascii"),
                "body": body,
            }
        )
        path = request.url.path
        if path == "/api/health":
            payload = {
                "schema_version": "manyforge.headless_health.v1",
                "status_scope": "headless_projection",
                "status": "degraded",
                "headless_projection_ready": False,
                "robot_readiness": {
                    "available": False,
                    "state": None,
                    "source_owner_id": None,
                    "reason_code": "robot_readiness_owner_unavailable",
                },
            }
        elif path == "/api/runtime":
            payload = _runtime()
        elif path == "/api/manager/status":
            payload = {"schema_version": "manyforge.manager_status_projection.v1"}
        elif path == "/api/manager/graph":
            payload = {"schema_version": "manyforge.manager_graph_projection.v1"}
        elif path == "/api/manager/diff":
            payload = {"schema_version": "manyforge.manager_diff_projection.v1"}
        elif path == "/api/node-environment":
            payload = node_environment
        elif path == "/api/manager/apply":
            return httpx.Response(
                503,
                json={
                    "error": {
                        "code": "kernel_owner_unavailable",
                        "message": "returns at I6",
                    },
                    "ok": False,
                },
                request=request,
            )
        elif path == "/api/execution/stops" or path.startswith(
            "/api/execution/operations/"
        ):
            payload = {
                "api_version": "manyforge.execution_command_response.v1",
                "ok": True,
                "operation": {"operation_id": "operation-1"},
            }
        elif path == "/api/scene":
            payload = {"schema_version": "manyforge.scene_projection.v1"}
        elif path == "/api/execution/alarms":
            payload = _alarms()
        elif path == "/api/execution/alarms/metrics":
            payload = {"schema_version": "manyforge.operator_alarm_metrics.v1"}
        elif path.endswith("/acknowledge") or path.endswith("/acknowledge-all"):
            payload = {
                "schema_version": "manyforge.headless_api_mutation_response.v1"
            }
        elif path == "/api/execution/history":
            payload = {"schema_version": "manyforge.historian_page.v1", "events": []}
        elif path == "/api/execution/history/health":
            payload = {
                "schema_version": "manyforge.historian_health.v1",
                "available": True,
            }
        elif path == "/api/execution/history/reconcile":
            payload = {
                "schema_version": "manyforge.historian_reconciliation.v1",
                "reconciled": True,
            }
        else:
            return httpx.Response(404, json={"detail": "unknown"}, request=request)
        return httpx.Response(200, json=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        yield client, requests, node_environment
    finally:
        raw.close()


def _composer(client: HeadlessHttpClient) -> TestClient:
    return TestClient(create_app(ComposerState(headless_client=client)))


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "alarm.epoch.1",
        "source_label": "composer_ui",
        "correlation_id": None,
    }


def test_composer_projects_headless_reads_without_local_runtime_state(
    headless_client,
) -> None:
    client, requests, node_environment = headless_client
    with _composer(client) as composer:
        health = composer.get("/api/health").json()
        assert health["mode"] == "headless_client"
        assert health["components"]["headless_projection"] == "degraded"
        assert health["headless"]["headless_projection_ready"] is False
        assert "ready" not in health["headless"]
        assert health["headless"]["robot_readiness"]["available"] is False
        assert composer.get("/api/runtime").json() == _runtime()
        assert composer.get("/api/manager/status").status_code == 200
        assert composer.get("/api/manager/graph").status_code == 200
        assert composer.get("/api/manager/diff").status_code == 200
        assert composer.get("/api/node-environment").json() == node_environment
        catalog = composer.get("/api/catalog").json()
        assert catalog["environmentDigest"] == node_environment["environment_digest"]
        projected = composer.get("/api/nodes/environment").json()
        assert projected["active"] == {
            "retainedBundleId": "bundle.1",
            "graphRevision": "graph.1",
        }
        assert projected["availability"]["execution_available"] is False
        assert composer.get("/api/scene").status_code == 200
        assert composer.get(
            "/api/execution/alarms?limit=3&ordering=attention_newest"
        ).json() == _alarms()
        assert composer.get("/api/execution/history").status_code == 200

    assert all(item["method"] == "GET" for item in requests)
    alarm_request = next(
        item for item in requests if item["path"] == "/api/execution/alarms"
    )
    assert alarm_request["query"] == "limit=3&ordering=attention_newest"


def test_composer_forwards_only_strict_attributed_alarm_intent(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    body = {
        **_caller(),
        "activation_id": "activation.1",
        "expected_record_revision": 2,
        "reason": None,
        "confirmed": True,
    }
    with _composer(client) as composer:
        accepted = composer.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json=body,
        )
        assert accepted.status_code == 200
        forged = composer.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={**body, "trusted_principal_id": "forged"},
        )
        assert forged.status_code == 422

    forwarded = next(item for item in requests if item["method"] == "POST")
    assert forwarded["body"] == body
    assert "trusted_principal_id" not in forwarded["body"]


def test_composer_operation_controls_are_strict_stateless_forwarders(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "composer_ui",
        "correlation_id": None,
    }
    control = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": None,
    }
    stop = {
        "request_context": context,
        "operations": [
            {
                "operation_id": "operation-1",
                "expected_operation_revision": 3,
                "expected_stop_generation": 0,
            }
        ],
        "reason": "operator Stop",
    }
    scale = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "expected_runtime_scale_generation": 0,
        "scale": 0.5,
    }

    with _composer(client) as composer:
        for endpoint in ("pause", "resume", "reset", "acknowledge-completed"):
            response = composer.post(
                f"/api/execution/operations/operation-1/{endpoint}",
                json=control,
            )
            assert response.status_code == 200, response.text
        assert composer.post("/api/execution/stops", json=stop).status_code == 200
        assert composer.post(
            "/api/execution/operations/operation-1/runtime-scale", json=scale
        ).status_code == 200
        assert composer.get(
            "/api/execution/operations/operation-1/runtime-scale"
        ).status_code == 200
        mismatch = composer.post(
            "/api/execution/operations/operation-2/pause", json=control
        )

    assert mismatch.status_code == 400
    forwarded = [
        item
        for item in requests
        if item["path"] == "/api/execution/stops"
        or item["path"].startswith("/api/execution/operations/")
    ]
    assert [item["path"].rsplit("/", 1)[-1] for item in forwarded] == [
        "pause",
        "resume",
        "reset",
        "acknowledge-completed",
        "stops",
        "runtime-scale",
        "runtime-scale",
    ]
    assert forwarded[0]["body"] == control
    assert forwarded[4]["body"] == stop
    assert forwarded[5]["body"] == scale
    assert forwarded[6]["method"] == "GET"


def test_composer_preserves_headless_unavailability_code(
    headless_client,
) -> None:
    client, _requests, _node_environment = headless_client
    with _composer(client) as composer:
        response = composer.post(
            "/api/manager/apply",
            json={
                **_caller(),
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "a" * 64,
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "reject_if_affected_busy": True,
            },
        )
    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "kernel_owner_unavailable"


def test_composer_websocket_streams_owner_snapshots_only(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer(client) as composer:
        with composer.websocket_connect("/ws/state") as websocket:
            payload = websocket.receive_json()
            assert payload == {
                "schema_version": "manyforge.composer_headless_state.v1",
                "runtime": _runtime(),
                "alarms": _alarms(),
            }
    assert {item["path"] for item in requests} == {
        "/api/runtime",
        "/api/execution/alarms",
    }
    alarm_request = next(
        item for item in requests if item["path"] == "/api/execution/alarms"
    )
    assert alarm_request["query"] == "limit=32&ordering=attention_newest"


def test_composer_websocket_deduplicates_an_unchanged_owner_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class RecordingWebSocket:
        def __init__(self) -> None:
            self.accepted = False
            self.messages: list[str] = []

        async def accept(self) -> None:
            self.accepted = True

        async def send_text(self, message: str) -> None:
            self.messages.append(message)

    class FailingClient:
        def __init__(self) -> None:
            self.polls = 0

        def runtime(self) -> dict[str, Any]:
            self.polls += 1
            if self.polls == 3:
                raise WebSocketDisconnect()
            raise HeadlessClientError(
                "headless_http_unavailable",
                "headless owner unavailable",
            )

        def alarms(self, *, limit: int, ordering: str) -> dict[str, Any]:
            del limit
            assert ordering == "attention_newest"
            return {}

    websocket = RecordingWebSocket()
    monkeypatch.setattr(headless_ws, "POLL_INTERVAL_SECONDS", 0)

    asyncio.run(serve_headless_state(websocket, FailingClient()))  # type: ignore[arg-type]

    assert websocket.accepted is True
    assert [json.loads(message) for message in websocket.messages] == [
        {
            "schema_version": "manyforge.composer_headless_state.v1",
            "runtime": None,
            "alarms": None,
            "error": {
                "code": "headless_http_unavailable",
                "message": "headless owner unavailable",
            },
        }
    ]


def test_composer_websocket_reports_an_oversized_projection_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class RecordingWebSocket:
        def __init__(self) -> None:
            self.messages: list[str] = []

        async def accept(self) -> None:
            return None

        async def send_text(self, message: str) -> None:
            self.messages.append(message)

    class SnapshotClient:
        def __init__(self) -> None:
            self.polls = 0

        def runtime(self) -> dict[str, Any]:
            self.polls += 1
            if self.polls == 2:
                raise WebSocketDisconnect()
            return _runtime()

        def alarms(self, *, limit: int, ordering: str) -> dict[str, Any]:
            assert limit == 32
            assert ordering == "attention_newest"
            return _alarms()

    original_encode = headless_ws._encode

    def fail_snapshot_only(value: dict[str, Any]) -> str:
        if value.get("runtime") is not None:
            raise ValueError("injected oversized snapshot")
        return original_encode(value)

    websocket = RecordingWebSocket()
    monkeypatch.setattr(headless_ws, "_encode", fail_snapshot_only)
    monkeypatch.setattr(headless_ws, "POLL_INTERVAL_SECONDS", 0)

    asyncio.run(serve_headless_state(websocket, SnapshotClient()))  # type: ignore[arg-type]

    assert [json.loads(message) for message in websocket.messages] == [
        {
            "schema_version": "manyforge.composer_headless_state.v1",
            "runtime": None,
            "alarms": None,
            "error": {
                "code": "headless_websocket_projection_oversized",
                "message": "headless WebSocket projection exceeds its byte bound",
            },
        }
    ]


def test_composer_rejects_a_mismatched_active_environment_without_local_fallback(
    headless_client,
) -> None:
    client, _requests, node_environment = headless_client
    node_environment["environment_digest"] = "sha256:" + "0" * 64
    with _composer(client) as composer:
        catalog = composer.get("/api/catalog")
        environment = composer.get("/api/nodes/environment")
    assert catalog.status_code == 503
    assert environment.status_code == 503
    assert catalog.json()["detail"]["code"] == (
        "node_environment_projection_invalid"
    )


def test_composer_does_not_fall_back_when_the_headless_environment_is_lost() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            503,
            json={
                "detail": {
                    "code": "manager_source_unavailable",
                    "message": "Manager publication is unavailable",
                }
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with _composer(client) as composer:
            response = composer.get("/api/catalog")
    finally:
        raw.close()
    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "manager_source_unavailable"
