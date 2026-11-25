"""Composer carries its request id (and the MCP adapter's mark) to Headless;
the MCP adapter writes one log line per tool call."""

from __future__ import annotations

import logging
from pathlib import Path

import httpx
from fastapi.testclient import TestClient

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessHttpClient


def _state(headless: HeadlessHttpClient) -> ComposerState:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, headless_client=headless)
    state.authoring_admission = None
    return state


def _security(tmp_path: Path) -> SecurityConfig:
    return SecurityConfig(allowed_fs_roots=[str(tmp_path)], mcp_allowed_hosts=["testserver"])


class _Owner:
    """Headless double: keeps each request's identity headers."""

    def __init__(self, status: int = 200, body: dict | None = None) -> None:
        self.headers: list[dict[str, str | None]] = []
        self.status = status
        self.body = body or {"targets": []}

    def __call__(self, request: httpx.Request) -> httpx.Response:
        self.headers.append({
            "request_id": request.headers.get("x-request-id"),
            "transport": request.headers.get("x-manyforge-transport"),
        })
        return httpx.Response(self.status, json=self.body)


def test_a_headless_call_carries_the_composer_request_id(tmp_path) -> None:
    owner = _Owner()
    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(owner)) as http:
        app = create_app(_state(HeadlessHttpClient("http://headless.test", client=http)))
        with TestClient(app) as client:
            named = client.get("/api/execution/targets", headers={"X-Request-ID": "browser.1"})
            minted = client.get("/api/execution/targets")
            marked = client.get(
                "/api/execution/targets",
                headers={"X-Request-ID": "inner.1", "X-ManyForge-Transport": "mcp"},
            )
    assert named.status_code == 200, named.text
    assert owner.headers == [
        {"request_id": "browser.1", "transport": None},
        {"request_id": minted.headers["x-request-id"], "transport": None},
        {"request_id": "inner.1", "transport": "mcp"},
    ]


def _tool_call(client: TestClient, request_id: str):
    return client.post(
        "/mcp/",
        headers={"Accept": "application/json, text/event-stream", "X-Request-ID": request_id},
        json={
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": "manyforge_capabilities", "arguments": {"action": "targets"}},
        },
    )


def test_an_mcp_tool_call_is_marked_and_logged_once(tmp_path, caplog) -> None:
    owner = _Owner()
    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(owner)) as http:
        app = create_app(
            _state(HeadlessHttpClient("http://headless.test", client=http)),
            security=_security(tmp_path),
            enable_mcp=True,
        )
        with TestClient(app) as client, caplog.at_level(
            logging.INFO, logger="manyforge_composer.backend.mcp_adapter"
        ):
            result = _tool_call(client, "outer.1").json()["result"]
    assert not result["isError"], result
    # The inner request is the adapter's, under the outer request's id.
    assert owner.headers == [{"request_id": "outer.1", "transport": "mcp"}]
    [line] = [r for r in caplog.records if r.name == "manyforge_composer.backend.mcp_adapter"]
    assert (line.tool, line.action, line.route, line.status_code, line.error_code) == (
        "manyforge_capabilities", "targets", "GET /api/execution/targets", 200, None,
    )
    assert line.request_id == "outer.1"
    assert line.duration_ms >= 0


def test_a_refused_mcp_tool_call_logs_its_code(tmp_path, caplog) -> None:
    owner = _Owner(409, {"detail": {"code": "execution_targets_unavailable", "message": "x"}})
    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(owner)) as http:
        app = create_app(
            _state(HeadlessHttpClient("http://headless.test", client=http)),
            security=_security(tmp_path),
            enable_mcp=True,
        )
        with TestClient(app) as client, caplog.at_level(
            logging.INFO, logger="manyforge_composer.backend.mcp_adapter"
        ):
            result = _tool_call(client, "outer.2").json()["result"]
    assert result["isError"]
    [line] = [r for r in caplog.records if r.name == "manyforge_composer.backend.mcp_adapter"]
    assert (line.status_code, line.error_code) == (409, "execution_targets_unavailable")
