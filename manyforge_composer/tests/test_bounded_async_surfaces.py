"""Regression tests for bounded backend async surfaces and in-memory stores."""
from __future__ import annotations

import asyncio
import threading
import time
from pathlib import Path
from typing import Any

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend import routes_mcp
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.routes_pointcloud import PointCloudPublisher
from manyforge_composer.backend.routes_snapshot import SnapshotBroker
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)
_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=[str(_REPO_ROOT), "/workspace", "/tmp"],
)


def _state() -> ComposerState:
    return ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        allowed_fs_roots=[str(_REPO_ROOT), "/workspace", "/tmp"],
    )


def _mcp_sessions_from_client(client: TestClient) -> dict[str, Any]:
    def find_session_store(value: Any, seen: set[int]) -> dict[str, Any] | None:
        if id(value) in seen:
            return None
        seen.add(id(value))
        if (
            isinstance(value, dict)
            and value
            and all(isinstance(session, routes_mcp._McpSession) for session in value.values())
        ):
            return value
        closure = getattr(value, "__closure__", None)
        for cell in closure or ():
            found = find_session_store(cell.cell_contents, seen)
            if found is not None:
                return found
        return None

    for route in client.app.routes:
        if getattr(route, "path", None) != "/api/mcp":
            continue
        endpoint = getattr(route, "endpoint", None)
        found = find_session_store(endpoint, set())
        if found is not None:
            return found
    raise AssertionError("MCP session store closure not found")


def _mcp_initialize(client: TestClient, session_id: str) -> None:
    response = client.post(
        "/api/mcp",
        json={"jsonrpc": "2.0", "method": "initialize", "id": session_id},
        headers={"x-mcp-session-id": session_id},
    )
    assert response.status_code == 200


class _FakeClock:
    def __init__(self, now_ns: int = 1_000_000_000) -> None:
        self.now_ns = now_ns

    def monotonic_ns(self) -> int:
        return self.now_ns

    def advance(self, delta_ns: int) -> None:
        self.now_ns += delta_ns


def test_mcp_idle_sessions_are_evicted_on_new_session(monkeypatch: pytest.MonkeyPatch) -> None:
    clock = _FakeClock()
    monkeypatch.setattr(routes_mcp.time, "monotonic_ns", clock.monotonic_ns)

    with TestClient(create_app(_state(), security=_TEST_SECURITY, enable_mcp_http=True)) as client:
        _mcp_initialize(client, "idle-old")
        sessions = _mcp_sessions_from_client(client)
        assert "idle-old" in sessions

        clock.advance(routes_mcp._SESSION_IDLE_EVICTION_NS + 1)
        _mcp_initialize(client, "new-session")

        assert "idle-old" not in sessions
        assert "new-session" in sessions


def test_mcp_disconnected_sessions_are_evicted_after_grace(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    clock = _FakeClock()
    monkeypatch.setattr(routes_mcp.time, "monotonic_ns", clock.monotonic_ns)

    with TestClient(create_app(_state(), security=_TEST_SECURITY, enable_mcp_http=True)) as client:
        _mcp_initialize(client, "dead-client")
        sessions = _mcp_sessions_from_client(client)
        sessions["dead-client"].mark_disconnected()

        clock.advance(routes_mcp._SESSION_EVICTION_GRACE_NS + 1)
        _mcp_initialize(client, "replacement")

        assert "dead-client" not in sessions
        assert "replacement" in sessions


def test_mcp_session_store_is_bounded_during_session_creation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_mcp, "_SESSION_MAX_COUNT", 3)

    with TestClient(create_app(_state(), security=_TEST_SECURITY, enable_mcp_http=True)) as client:
        for index in range(10):
            _mcp_initialize(client, f"session-{index:02d}")

        sessions = _mcp_sessions_from_client(client)

    assert len(sessions) <= 3
    assert "session-00" not in sessions
    assert "session-06" not in sessions
    assert "session-07" in sessions
    assert "session-08" in sessions
    assert "session-09" in sessions


class _LoopThread:
    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        assert self._ready.wait(timeout=2.0)

    def _run(self) -> None:
        asyncio.set_event_loop(self.loop)
        self._ready.set()
        self.loop.run_forever()

    def stop(self) -> None:
        self.loop.call_soon_threadsafe(self.loop.stop)
        self._thread.join(timeout=2.0)
        self.loop.close()


class _SnapshotClient:
    def __init__(self) -> None:
        self.messages: list[dict[str, Any]] = []

    async def send_json(self, message: dict[str, Any]) -> None:
        self.messages.append(message)


def test_snapshot_broker_rejects_late_response_after_timeout() -> None:
    loop_thread = _LoopThread()
    snapshot_client = _SnapshotClient()
    broker = SnapshotBroker()
    try:
        with broker._lock:
            broker._clients.append(snapshot_client)  # type: ignore[arg-type]
            broker._loop = loop_thread.loop

        assert broker.render_sync(320, 240, "jpeg", timeout_s=0.01) is None

        deadline = time.monotonic() + 2.0
        while not snapshot_client.messages and time.monotonic() < deadline:
            time.sleep(0.001)
        assert snapshot_client.messages
        late_request_id = snapshot_client.messages[0]["requestId"]
        assert broker.deliver(late_request_id, b"late-image") is False
        with broker._lock:
            assert broker._pending == {}
            assert broker._results == {}
    finally:
        loop_thread.stop()


def test_snapshot_upload_route_rejects_unknown_or_expired_request_id() -> None:
    with TestClient(create_app(_state(), security=_TEST_SECURITY)) as client:
        response = client.post(
            "/api/scene/snapshot/upload/not-a-live-request",
            content=b"late-image",
        )

    assert response.status_code == 404
    assert response.json()["detail"] == "Unknown or expired snapshot request id"


class _FailingPointCloudClient:
    async def send_json(self, _message: dict[str, Any]) -> None:
        raise RuntimeError("client disconnected")


def test_pointcloud_publisher_removes_dead_client_after_send_failure() -> None:
    loop_thread = _LoopThread()
    publisher = PointCloudPublisher()
    dead_client = _FailingPointCloudClient()
    try:
        with publisher._lock:
            publisher._clients.append(dead_client)  # type: ignore[arg-type]
            publisher._loop = loop_thread.loop

        publisher.publish([[0.0, 0.0, 0.0]])

        deadline = time.monotonic() + 2.0
        while publisher.has_clients() and time.monotonic() < deadline:
            time.sleep(0.001)
        assert not publisher.has_clients()
    finally:
        loop_thread.stop()


def test_assistant_request_store_prunes_completed_records(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend import state as state_module

    monkeypatch.setattr(state_module, "_MAX_ASSISTANT_REQUEST_RECORDS", 3)
    composer_state = _state()

    for index in range(5):
        request_id = f"req-{index}"
        composer_state.start_assistant_request(
            request_id=request_id,
            conversation_id=f"conv-{index}",
            provider_id="test-provider",
        )
        composer_state.finish_assistant_request(request_id, status="completed")

    assert composer_state.get_assistant_request("req-0") is None
    assert composer_state.get_assistant_request("req-1") is None
    assert composer_state.get_assistant_request("req-4") is not None
    assert len(composer_state._assistant_requests) == 3


def test_assistant_proposal_store_prunes_oldest_staged_records(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend import state as state_module

    monkeypatch.setattr(state_module, "_MAX_ASSISTANT_PROPOSAL_RECORDS", 3)
    composer_state = _state()

    for index in range(5):
        composer_state.stage_assistant_proposals(
            conversation_id="conv",
            proposals=[
                {
                    "id": f"proposal-{index}",
                    "type": "tree_node_add",
                    "title": f"Proposal {index}",
                    "summary": "test",
                    "status": "draft",
                    "reviewRequired": True,
                    "payload": {"parentName": "root", "node": {"id": "sequence"}},
                    "warnings": [],
                }
            ],
        )

    proposal_ids = [record.proposal_id for record in composer_state.list_assistant_proposals()]
    assert proposal_ids == ["proposal-2", "proposal-3", "proposal-4"]
    assert composer_state.get_assistant_proposal("proposal-0") is None
