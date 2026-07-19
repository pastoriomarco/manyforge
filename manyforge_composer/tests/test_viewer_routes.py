from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend import routes_viewer
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.models import (
    ViewerFrameV1,
    ViewerDiagnosticsV1,
    ViewerLeaseAckV1,
    ViewerManifestV1,
)
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.viewer_data_plane import ViewerProtocolEncoder
from manyforge_composer.tests.node_environment_support import (
    node_environment_for_catalog,
)

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)


def _state_with_publication(monkeypatch: pytest.MonkeyPatch) -> ComposerState:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    publication = ViewerProtocolEncoder().encode(
        manifest_payload={
            "world_frame": "universe",
            "robots": [],
            "entities": [],
            "frames": [],
            "grippers": [],
            "pose_sources": [],
            "orders": {
                "links": [],
                "entities": [],
                "frames": [],
                "grippers": [],
                "pose_sources": [],
            },
            "object_axes_gizmo_size_m": 0.1,
            "frame_axes_gizmo_size_m": 0.1,
            "capabilities": {},
        },
        frame_payload={
            "timestamp_ns": 1,
            "source_sequence": 1,
            "stale": False,
            "robot_blocks": [],
            "entity_transforms": [],
            "frame_transforms": [],
            "gripper_states": [],
            "pose_source_states": [],
        },
    )
    with state._visual_state_lock:
        state._visual_state_last_publication = publication
    monkeypatch.setattr(
        routes_viewer,
        "ensure_viewer_state_publisher",
        lambda _state, *, reason: None,
    )
    state.request_visual_state_publish = lambda *, reason="": None
    return state


def test_viewer_lease_watchdog_closes_socket_after_expiry() -> None:
    class _ExpiringState:
        def __init__(self) -> None:
            self.calls = 0

        def viewer_demand_grant(self, _client_id: str) -> SimpleNamespace:
            self.calls += 1
            if self.calls > 1:
                raise KeyError("expired")
            return SimpleNamespace(expires_at_ns=time.monotonic_ns() + 1_000_000)

    class _Socket:
        def __init__(self) -> None:
            self.closed: tuple[int, str] | None = None

        async def close(self, *, code: int, reason: str) -> None:
            self.closed = (code, reason)

    async def scenario() -> None:
        state = _ExpiringState()
        socket = _Socket()
        await routes_viewer._watch_viewer_lease(  # noqa: SLF001
            state,  # type: ignore[arg-type]
            socket,  # type: ignore[arg-type]
            "client",
        )
        assert socket.closed == (1001, "Viewer lease expired")

    asyncio.run(scenario())


def test_viewer_websocket_delivers_manifest_then_frame_and_acknowledges_clamped_lease(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = _state_with_publication(monkeypatch)
    client = TestClient(create_app(state))

    with client.websocket_connect("/ws/scene/viewer") as websocket:
        manifest = json.loads(websocket.receive_text())
        frame = json.loads(websocket.receive_text())
        ViewerManifestV1.model_validate(manifest)
        ViewerFrameV1.model_validate(frame)
        assert manifest["type"] == "manifest"
        assert frame["type"] == "frame"
        assert frame["manifest_revision"] == manifest["manifest_revision"]

        websocket.send_json(
            {
                "type": "lease",
                "schema_version": "manyforge.viewer.control.v1",
                "request_id": "lease-1",
                "visibility": "hidden",
                "requested_rate_hz": 1000.0,
                "layers": ["canonical", "measured_robot", "diagnostics"],
            }
        )
        acknowledgement = json.loads(websocket.receive_text())
        ViewerLeaseAckV1.model_validate(acknowledgement)
        assert acknowledgement["type"] == "lease_ack"
        assert acknowledgement["request_id"] == "lease-1"
        assert acknowledgement["visibility"] == "hidden"
        assert acknowledgement["granted_rate_hz"] == 30.0
        assert acknowledgement["server_max_rate_hz"] == 30.0
        assert acknowledgement["expires_at_ns"] > acknowledgement["timestamp_ns"]
        diagnostics = json.loads(websocket.receive_text())
        ViewerDiagnosticsV1.model_validate(diagnostics)
        assert diagnostics["type"] == "diagnostics"
        assert diagnostics["demand_client_count"] == 1
        assert diagnostics["producer_healthy"] is False
        assert diagnostics["producer_health_reason"] == "publisher_not_running"
        assert diagnostics["publisher_running"] is False
        assert diagnostics["worker_alive"] is False
        assert state.viewer_demand_summary().active is False


def test_viewer_websocket_rejects_a_control_message_without_v1_schema(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = _state_with_publication(monkeypatch)
    client = TestClient(create_app(state))

    with client.websocket_connect("/ws/scene/viewer") as websocket:
        websocket.receive_text()
        websocket.receive_text()
        websocket.send_json(
            {
                "type": "lease",
                "visibility": "visible",
                "requested_rate_hz": 15.0,
                "layers": ["canonical"],
            }
        )
        with pytest.raises(WebSocketDisconnect) as closed:
            websocket.receive_text()
        assert closed.value.code == 1008


def test_viewer_websocket_uses_the_existing_bearer_auth_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = _state_with_publication(monkeypatch)
    security = SecurityConfig(api_token="viewer-secret")
    client = TestClient(create_app(state, security=security))

    with pytest.raises(WebSocketDisconnect) as denied:
        with client.websocket_connect("/ws/scene/viewer"):
            pass
    assert denied.value.code == 4401

    with client.websocket_connect(
        "/ws/scene/viewer?token=viewer-secret"
    ) as websocket:
        assert json.loads(websocket.receive_text())["type"] == "manifest"
        assert json.loads(websocket.receive_text())["type"] == "frame"


def test_viewer_websocket_rejects_qualification_delay_when_fixture_gate_is_off(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("MANYFORGE_VIEWER_QUALIFICATION_FIXTURES", raising=False)
    state = _state_with_publication(monkeypatch)
    client = TestClient(create_app(state))

    with pytest.raises(WebSocketDisconnect) as denied:
        with client.websocket_connect(
            "/ws/scene/viewer?qualification_send_delay_ms=250"
        ):
            pass
    assert denied.value.code == 1008


def test_viewer_websocket_exposes_gated_per_client_qualification_delay(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("MANYFORGE_VIEWER_QUALIFICATION_FIXTURES", "ON")
    state = _state_with_publication(monkeypatch)
    client = TestClient(create_app(state))

    with client.websocket_connect(
        "/ws/scene/viewer?qualification_send_delay_ms=25"
    ) as websocket:
        assert json.loads(websocket.receive_text())["type"] == "manifest"
        assert json.loads(websocket.receive_text())["type"] == "frame"
        status = client.get("/api/scene/viewer/status").json()
        client_status = next(iter(status["fanout"]["clients"].values()))
        assert client_status["qualification_send_delay_ms"] == 25.0


def test_viewer_websocket_records_bounded_client_reported_qualification_window(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = _state_with_publication(monkeypatch)
    client = TestClient(create_app(state))

    with client.websocket_connect("/ws/scene/viewer") as websocket:
        websocket.receive_text()
        websocket.receive_text()
        websocket.send_json(
            {
                "type": "client_diagnostics",
                "schema_version": "manyforge.viewer.control.v1",
                "visibility": "focused",
                "window_duration_ms": 5000.0,
                "websocket_messages": 77,
                "websocket_bytes": 12345,
                "manifest_messages": 1,
                "frame_messages": 75,
                "dropped_frame_gaps": 2,
                "resync_requests": 0,
                "materializations": 75,
                "materialization_total_ms": 12.0,
                "materialization_max_ms": 1.1,
                "react_commits": 12,
                "react_commit_total_ms": 6.0,
                "react_commit_max_ms": 1.2,
                "rendered_frames": 75,
                "draw_calls_max": 42,
                "triangles_max": 250000,
                "canvas_pixels_max": 640000,
                "render_interval_p95_ms": 70.0,
                "render_interval_max_ms": 85.0,
                "long_task_supported": True,
                "long_tasks": 1,
                "long_task_total_ms": 51.0,
                "long_task_max_ms": 51.0,
                "heap_used_bytes": 1000000,
                "topology_rebuilds": 1,
                "transform_applies": 75,
                "renderer_vendor": "Mesa",
                "renderer_name": "llvmpipe",
                "renderer_software": True,
                "effective_dpr": 0.75,
                "requested_rate_hz": 5.0,
            }
        )
        # A following lease acknowledgement establishes that the prior control
        # message has been consumed before the status read.
        websocket.send_json(
            {
                "type": "lease",
                "schema_version": "manyforge.viewer.control.v1",
                "visibility": "focused",
                "requested_rate_hz": 15.0,
                "layers": ["canonical", "diagnostics"],
            }
        )
        assert json.loads(websocket.receive_text())["type"] == "lease_ack"
        status = client.get("/api/scene/viewer/status").json()

        client_status = next(iter(status["fanout"]["clients"].values()))
        assert client_status["client_diagnostics"]["frame_messages"] == 75
        assert client_status["client_diagnostics"]["long_tasks"] == 1
        assert client_status["client_diagnostics"]["renderer_software"] is True
        assert client_status["client_diagnostics"]["requested_rate_hz"] == 5.0
        assert client_status["client_diagnostics_received_at_ns"] > 0
