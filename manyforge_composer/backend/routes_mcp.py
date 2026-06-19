"""HTTP/SSE MCP transport routes — /api/mcp/* (OP-8).

Provides an HTTP-based MCP transport so external agent runtimes (NemoClaw,
OpenClaw, etc.) can consume the same MCP tool surface without embedding
ManyForge in-process.

POST /api/mcp     — JSON-RPC message endpoint (client → server)
GET  /api/mcp/sse — SSE stream for server → client messages

Uses the MCP SDK's StreamableHTTPServerTransport when available, falling
back to a manual SSE + JSON-RPC bridge otherwise.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from typing import Any, Optional

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, StreamingResponse

logger = logging.getLogger(__name__)

# Session eviction: disconnected sessions are purged after this grace period
_SESSION_EVICTION_GRACE_NS = 5 * 60 * 1_000_000_000  # 5 minutes
_SESSION_IDLE_EVICTION_NS = 30 * 60 * 1_000_000_000  # 30 minutes
_SESSION_MAX_COUNT = 1000


def create_mcp_router(state) -> APIRouter:
    """Create HTTP/SSE transport routes for the MCP tool surface.

    ``state`` is the ComposerState instance.  The MCP server is created
    lazily on first request to avoid import failures when the ``mcp``
    package is not installed.
    """
    router = APIRouter()

    # Lazy-init MCP server and session state
    _mcp_server = None
    _sessions: dict[str, _McpSession] = {}

    def _get_mcp_server():
        nonlocal _mcp_server
        if _mcp_server is None:
            from .mcp_server import create_mcp_server
            _mcp_server = create_mcp_server(state)
        return _mcp_server

    def _evict_stale_sessions() -> None:
        """Remove idle/disconnected sessions and enforce the hard cap.

        Called on each new session creation to bound memory growth.
        """
        now = time.monotonic_ns()
        stale = [
            sid
            for sid, s in _sessions.items()
            if (
                now - s.last_seen_ns > _SESSION_IDLE_EVICTION_NS
                or (
                    not s._connected
                    and (now - s.disconnected_at_ns) > _SESSION_EVICTION_GRACE_NS
                )
            )
        ]
        for sid in stale:
            del _sessions[sid]
        if stale:
            logger.debug("Evicted %d stale MCP session(s)", len(stale))
        # Hard cap: if still over limit, drop oldest disconnected first,
        # then oldest idle sessions. POST-only clients may never open SSE,
        # so "connected only" eviction is insufficient.
        if len(_sessions) > _SESSION_MAX_COUNT:
            victims = sorted(
                _sessions.items(),
                key=lambda pair: (
                    0 if not pair[1]._connected else 1,
                    pair[1].last_seen_ns,
                ),
            )
            while len(_sessions) > _SESSION_MAX_COUNT and victims:
                sid, _ = victims.pop(0)
                del _sessions[sid]

    def _get_or_create_session(
        session_id: str,
        principal_id: Optional[str],
    ) -> _McpSession:
        """Create/touch a session and enforce retention consistently.

        Both POST-only clients and SSE clients allocate session records. Keep
        the cap logic in one path so either transport cannot exceed the hard
        bound by one after a non-stale insertion.
        """
        created_session = False
        if session_id not in _sessions:
            _evict_stale_sessions()
            _sessions[session_id] = _McpSession(session_id, principal_id)
            created_session = True

        session = _sessions[session_id]
        session.touch(principal_id)
        if created_session:
            _evict_stale_sessions()
        return session

    @router.post("")
    async def mcp_message(request: Request) -> JSONResponse:
        """Handle a JSON-RPC MCP message from the client."""
        mcp_srv = _get_mcp_server()
        body = await request.json()

        # Extract principal_id from auth context if available
        principal_id = getattr(request.state, "principal_id", None) if hasattr(request, "state") else None

        # Get or create session
        session_id = request.headers.get("x-mcp-session-id", "")
        if not session_id:
            session_id = uuid.uuid4().hex[:12]

        session = _get_or_create_session(session_id, principal_id)

        # Route JSON-RPC to MCP server tools
        try:
            result = await _handle_jsonrpc(mcp_srv, body, session)
            response = JSONResponse(content=result)
            response.headers["x-mcp-session-id"] = session_id
            return response
        except Exception as exc:
            logger.exception("MCP message handling failed")
            return JSONResponse(
                content={
                    "jsonrpc": "2.0",
                    "error": {
                        "code": -32603,
                        "message": str(exc),
                    },
                    "id": body.get("id"),
                },
                status_code=500,
            )

    @router.get("/sse")
    async def mcp_sse(request: Request) -> StreamingResponse:
        """SSE stream for server-to-client MCP messages."""
        mcp_srv = _get_mcp_server()

        session_id = request.query_params.get("session_id", "")
        if not session_id:
            session_id = uuid.uuid4().hex[:12]

        principal_id = getattr(request.state, "principal_id", None) if hasattr(request, "state") else None

        session = _get_or_create_session(session_id, principal_id)

        async def event_stream():
            # Send initial connection event
            yield f"event: endpoint\ndata: {json.dumps({'session_id': session_id, 'endpoint': '/api/mcp'})}\n\n"

            try:
                while True:
                    session.touch()
                    # Check for pending notifications
                    notification = await session.wait_notification(timeout=30.0)
                    if notification is not None:
                        yield f"event: message\ndata: {json.dumps(notification)}\n\n"
                    else:
                        # Send keepalive ping
                        yield f"event: ping\ndata: {json.dumps({'timestamp_ns': time.monotonic_ns()})}\n\n"

                    # Check if client disconnected
                    if await request.is_disconnected():
                        break
            except asyncio.CancelledError:
                pass
            finally:
                # Clean up session after disconnect grace period
                session.mark_disconnected()

        return StreamingResponse(
            event_stream(),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "Connection": "keep-alive",
                "X-Accel-Buffering": "no",
                "x-mcp-session-id": session_id,
            },
        )

    # Private regression-test seam. Session retention must remain bounded, and
    # relying on FastAPI endpoint closure layout makes that test environment-
    # dependent.
    router._manyforge_mcp_sessions = _sessions  # type: ignore[attr-defined]
    mcp_message._manyforge_mcp_sessions = _sessions  # type: ignore[attr-defined]
    mcp_sse._manyforge_mcp_sessions = _sessions  # type: ignore[attr-defined]

    return router


class _McpSession:
    """Tracks state for a single MCP HTTP client session."""

    def __init__(self, session_id: str, principal_id: Optional[str] = None):
        self.session_id = session_id
        self.principal_id = principal_id
        self.created_at_ns = time.monotonic_ns()
        self.last_seen_ns = self.created_at_ns
        self.disconnected_at_ns: int = 0
        self._notification_queue: asyncio.Queue[dict] = asyncio.Queue(maxsize=256)
        self._connected = True

    async def wait_notification(self, timeout: float = 30.0) -> Optional[dict]:
        try:
            return await asyncio.wait_for(
                self._notification_queue.get(),
                timeout=timeout,
            )
        except asyncio.TimeoutError:
            return None

    def send_notification(self, notification: dict) -> None:
        if self._connected:
            try:
                self._notification_queue.put_nowait(notification)
            except asyncio.QueueFull:
                logger.warning("MCP session %s notification queue full", self.session_id)

    def mark_disconnected(self) -> None:
        self._connected = False
        self.disconnected_at_ns = time.monotonic_ns()
        self.last_seen_ns = self.disconnected_at_ns

    def touch(self, principal_id: Optional[str] = None) -> None:
        self.last_seen_ns = time.monotonic_ns()
        if principal_id is not None:
            self.principal_id = principal_id


async def _handle_jsonrpc(mcp_server, body: dict, session: _McpSession) -> dict:
    """Route a JSON-RPC request to the MCP server's tool handlers.

    This bridges the HTTP transport to FastMCP's internal tool dispatch.
    """
    method = body.get("method", "")
    params = body.get("params", {})
    request_id = body.get("id")

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {
                    "tools": {"listChanged": False},
                },
                "serverInfo": {
                    "name": "ManyForge Composer",
                    "version": "1.0.0",
                },
            },
            "id": request_id,
        }

    if method == "tools/list":
        # Get tools from the FastMCP server
        tools = []
        try:
            tool_list = await mcp_server.list_tools()
            for tool in tool_list:
                tools.append({
                    "name": tool.name,
                    "description": tool.description or "",
                    "inputSchema": tool.inputSchema if hasattr(tool, "inputSchema") else {},
                })
        except Exception:
            # Fallback: try to access internal tool registry
            if hasattr(mcp_server, "_tool_manager"):
                for name, tool in mcp_server._tool_manager._tools.items():
                    tools.append({
                        "name": name,
                        "description": getattr(tool, "description", ""),
                        "inputSchema": getattr(tool, "parameters", {}),
                    })
        return {
            "jsonrpc": "2.0",
            "result": {"tools": tools},
            "id": request_id,
        }

    if method == "tools/call":
        tool_name = params.get("name", "")
        tool_args = params.get("arguments", {})
        # Server-authoritative principal: always override with session identity.
        # The session principal comes from the auth middleware, not from the
        # client, so it cannot be spoofed via tool arguments.
        if (
            session.principal_id
            and isinstance(tool_args, dict)
            and "principal_id" in tool_args
            and tool_args["principal_id"] != session.principal_id
        ):
            return {
                "jsonrpc": "2.0",
                "result": {
                    "content": [
                        {
                            "type": "text",
                            "text": "principal_id does not match authenticated session principal",
                        }
                    ],
                    "isError": True,
                },
                "id": request_id,
            }
        if session.principal_id:
            tool_args["principal_id"] = session.principal_id
        try:
            result = await mcp_server.call_tool(tool_name, tool_args)
            # Serialize result content
            content = []
            if hasattr(result, "__iter__"):
                for item in result:
                    if hasattr(item, "text"):
                        content.append({"type": "text", "text": item.text})
                    elif hasattr(item, "model_dump"):
                        dumped = item.model_dump()
                        content.append(dumped)
                    else:
                        content.append({"type": "text", "text": str(item)})
            else:
                content.append({"type": "text", "text": str(result)})
            return {
                "jsonrpc": "2.0",
                "result": {"content": content, "isError": False},
                "id": request_id,
            }
        except Exception as exc:
            return {
                "jsonrpc": "2.0",
                "result": {
                    "content": [{"type": "text", "text": str(exc)}],
                    "isError": True,
                },
                "id": request_id,
            }

    # Unknown method
    return {
        "jsonrpc": "2.0",
        "error": {
            "code": -32601,
            "message": f"Method not found: {method}",
        },
        "id": request_id,
    }
