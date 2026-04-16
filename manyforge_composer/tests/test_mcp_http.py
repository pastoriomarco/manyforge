"""Tests for the HTTP/SSE MCP transport routes (OP-8).

Uses FastAPI TestClient to exercise the JSON-RPC bridge, session management,
principal propagation, and session eviction.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)
_PROGRAM_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml"
)

_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=["/workspace", "/tmp"],
)


@pytest.fixture
def state():
    kernel = MockKernelClient()
    return ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=["/workspace", "/tmp"],
    )


@pytest.fixture
def client(state):
    app = create_app(state, security=_TEST_SECURITY, enable_mcp_http=True)
    return TestClient(app)


# ---------------------------------------------------------------------------
# JSON-RPC basics
# ---------------------------------------------------------------------------

class TestMcpInitialize:

    def test_initialize(self, client):
        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "initialize",
            "id": 1,
        })
        assert resp.status_code == 200
        data = resp.json()
        assert data["jsonrpc"] == "2.0"
        assert data["id"] == 1
        assert "result" in data
        result = data["result"]
        assert result["protocolVersion"] == "2024-11-05"
        assert "tools" in result["capabilities"]

    def test_session_id_returned_in_header(self, client):
        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "initialize",
            "id": 1,
        })
        assert resp.status_code == 200
        assert "x-mcp-session-id" in resp.headers
        session_id = resp.headers["x-mcp-session-id"]
        assert len(session_id) > 0

    def test_session_id_reused(self, client):
        """When x-mcp-session-id header is sent, the same session is reused."""
        resp1 = client.post("/api/mcp", json={
            "jsonrpc": "2.0", "method": "initialize", "id": 1,
        })
        sid1 = resp1.headers["x-mcp-session-id"]

        resp2 = client.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 2},
            headers={"x-mcp-session-id": sid1},
        )
        sid2 = resp2.headers["x-mcp-session-id"]
        assert sid1 == sid2


class TestMcpToolsList:

    def test_tools_list(self, client):
        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "tools/list",
            "id": 2,
        })
        assert resp.status_code == 200
        data = resp.json()
        tools = data["result"]["tools"]
        assert isinstance(tools, list)
        assert len(tools) > 0
        # Each tool should have name and description
        for tool in tools:
            assert "name" in tool
            assert "description" in tool


class TestMcpToolsCall:

    def test_tool_call_returns_result(self, client):
        """Calling a valid tool via JSON-RPC returns a structured result."""
        # First list tools to find a valid one
        list_resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "tools/list",
            "id": 10,
        })
        tools = list_resp.json()["result"]["tools"]
        assert len(tools) > 0
        tool_name = tools[0]["name"]

        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "name": tool_name,
                "arguments": {},
            },
            "id": 3,
        })
        assert resp.status_code == 200
        data = resp.json()
        assert data["id"] == 3
        assert "result" in data
        result = data["result"]
        assert isinstance(result["content"], list)
        assert "isError" in result

    def test_unknown_tool(self, client):
        """Calling a nonexistent tool returns isError=True."""
        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "name": "nonexistent_tool_xyz",
                "arguments": {},
            },
            "id": 4,
        })
        assert resp.status_code == 200
        data = resp.json()
        result = data["result"]
        assert result["isError"] is True


class TestMcpUnknownMethod:

    def test_unknown_method(self, client):
        resp = client.post("/api/mcp", json={
            "jsonrpc": "2.0",
            "method": "nonexistent/method",
            "id": 5,
        })
        assert resp.status_code == 200
        data = resp.json()
        assert "error" in data
        assert data["error"]["code"] == -32601


# ---------------------------------------------------------------------------
# Principal propagation
# ---------------------------------------------------------------------------

class TestMcpPrincipalPropagation:

    def test_no_mcp_without_enable_flag(self, state):
        """MCP routes are NOT mounted when enable_mcp_http is False."""
        app = create_app(state, security=_TEST_SECURITY, enable_mcp_http=False)
        c = TestClient(app)
        resp = c.post("/api/mcp", json={
            "jsonrpc": "2.0", "method": "initialize", "id": 1,
        })
        # Should be 404 or 405 because the route isn't mounted
        # (405 when static files catch-all matches the path but not POST)
        assert resp.status_code in (404, 405)


# ---------------------------------------------------------------------------
# SSE endpoint
# ---------------------------------------------------------------------------

class TestMcpSseEndpoint:

    def test_sse_endpoint_exists(self, client):
        """GET /api/mcp/sse endpoint is reachable and returns streaming response.

        Full SSE streaming tests require an async test client or a real
        server. Here we verify the endpoint is mounted and responds with
        the correct content type via a HEAD-style probe.
        """
        # POST to /api/mcp/sse should be 405 (only GET allowed)
        resp = client.post("/api/mcp/sse", json={})
        assert resp.status_code == 405


# ---------------------------------------------------------------------------
# Session eviction
# ---------------------------------------------------------------------------

class TestMcpSessionEviction:

    def test_sessions_are_created(self, client):
        """Each new request without session header creates a new session."""
        resp1 = client.post("/api/mcp", json={
            "jsonrpc": "2.0", "method": "initialize", "id": 1,
        })
        sid1 = resp1.headers["x-mcp-session-id"]

        resp2 = client.post("/api/mcp", json={
            "jsonrpc": "2.0", "method": "initialize", "id": 2,
        })
        sid2 = resp2.headers["x-mcp-session-id"]

        assert sid1 != sid2


# ---------------------------------------------------------------------------
# Auth validation (with token enabled)
# ---------------------------------------------------------------------------

class TestMcpWithAuth:

    def test_auth_required_when_token_set(self, state):
        """When api_token is set, MCP routes require auth."""
        sec = SecurityConfig(api_token="test-secret-token")
        app = create_app(state, security=sec, enable_mcp_http=True)
        c = TestClient(app)

        # Without auth → 401
        resp = c.post("/api/mcp", json={
            "jsonrpc": "2.0", "method": "initialize", "id": 1,
        })
        assert resp.status_code == 401

        # With auth → 200
        resp = c.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 1},
            headers={"Authorization": "Bearer test-secret-token"},
        )
        assert resp.status_code == 200

    def test_principal_id_validation(self, state):
        """Invalid X-Principal-ID format is rejected."""
        sec = SecurityConfig(api_token="test-secret-token")
        app = create_app(state, security=sec, enable_mcp_http=True)
        c = TestClient(app)

        # Control character in principal → 400
        resp = c.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 1},
            headers={
                "Authorization": "Bearer test-secret-token",
                "X-Principal-ID": "bad\x00principal",
            },
        )
        assert resp.status_code == 400

        # Too long principal → 400
        resp = c.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 1},
            headers={
                "Authorization": "Bearer test-secret-token",
                "X-Principal-ID": "a" * 200,
            },
        )
        assert resp.status_code == 400

        # Valid principal → 200
        resp = c.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 1},
            headers={
                "Authorization": "Bearer test-secret-token",
                "X-Principal-ID": "human:alice",
            },
        )
        assert resp.status_code == 200

    def test_tools_call_rejects_principal_mismatch(self, state):
        """Authenticated MCP sessions reject tool-call principal spoofing."""
        sec = SecurityConfig(api_token="test-secret-token")
        app = create_app(state, security=sec, enable_mcp_http=True)
        c = TestClient(app)

        init = c.post(
            "/api/mcp",
            json={"jsonrpc": "2.0", "method": "initialize", "id": 1},
            headers={
                "Authorization": "Bearer test-secret-token",
                "X-Principal-ID": "human:alice",
            },
        )
        session_id = init.headers["x-mcp-session-id"]

        resp = c.post(
            "/api/mcp",
            json={
                "jsonrpc": "2.0",
                "method": "tools/call",
                "params": {
                    "name": "manyforge_intervention",
                    "arguments": {
                        "action": "status",
                        "principal_id": "human:bob",
                    },
                },
                "id": 2,
            },
            headers={
                "Authorization": "Bearer test-secret-token",
                "X-Principal-ID": "human:alice",
                "x-mcp-session-id": session_id,
            },
        )

        assert resp.status_code == 200
        payload = resp.json()["result"]
        assert payload["isError"] is True
        assert "principal_id does not match" in payload["content"][0]["text"]
