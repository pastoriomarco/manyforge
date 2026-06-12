"""Snapshot routes - /ws/scene/snapshot + /api/scene/snapshot/upload/{request_id}

Native R3F visualizer Day 3: round-trips scene snapshot requests
between the backend and the browser-side viewer over a dedicated
WebSocket. The native viewer is the only render source; it fails closed
when no browser snapshot client is connected so the VLM-driven behavior-tree nodes
(`pick_planning`, `selection` in `manyforge_behavior`) keep their
`scene_snapshot_provider(width, height, format) -> bytes` contract.

Flow:
1. A BT node invokes its `scene_snapshot_provider`; this runs on a
   capture worker thread spawned by `SceneSnapshotService`.
2. The provider calls `SnapshotBroker.render_sync(width, height,
   format)`. The broker creates a fresh request_id + threading.Event,
   schedules a coroutine on the WS event loop to push a
   `snapshot_request` message to the connected viewer, then blocks
   the calling thread on the event with a 2 s timeout.
3. The viewer's `SnapshotResponder` (running inside the R3F Canvas)
   resizes the renderer to the requested dimensions, forces one
   render, captures the framebuffer via `toDataURL`, then POSTs the
   bytes to `/api/scene/snapshot/upload/{request_id}`.
4. The upload route hands the bytes to `SnapshotBroker.deliver`,
   which stashes them and signals the event. The waiting provider
   thread unblocks and returns the bytes.

If no client is connected, `render_sync` returns `None` immediately and
callers fail closed.
"""
from __future__ import annotations

import asyncio
import logging
import threading
import uuid
from typing import Optional, TYPE_CHECKING

from fastapi import APIRouter, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from .security import SecurityConfig, check_ws_bearer_token

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)


class SnapshotBroker:
    """Bridge between BT-node-thread callers and the WS-connected viewer."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._clients: list[WebSocket] = []
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._pending: dict[str, threading.Event] = {}
        self._results: dict[str, Optional[bytes]] = {}

    async def attach(self, ws: WebSocket) -> None:
        await ws.accept()
        loop = asyncio.get_running_loop()
        with self._lock:
            self._clients.append(ws)
            if self._loop is None:
                self._loop = loop

    async def detach(self, ws: WebSocket) -> None:
        with self._lock:
            if ws in self._clients:
                self._clients.remove(ws)
            # Fail any pending request that depended on this client. The
            # SnapshotBroker has no per-request client affinity beyond
            # "the current first client"; conservatively wake every
            # pending event so callers can move on.
            if not self._clients:
                for request_id, evt in list(self._pending.items()):
                    self._results[request_id] = None
                    evt.set()

    def has_clients(self) -> bool:
        with self._lock:
            return bool(self._clients)

    def render_sync(
        self,
        width: int,
        height: int,
        format: str,
        *,
        timeout_s: float = 2.0,
    ) -> Optional[bytes]:
        """Block the calling thread on a single snapshot round-trip."""
        with self._lock:
            if not self._clients or self._loop is None:
                return None
            client = self._clients[0]
            loop = self._loop
            request_id = uuid.uuid4().hex
            evt = threading.Event()
            self._pending[request_id] = evt

        message = {
            "type": "snapshot_request",
            "requestId": request_id,
            "width": int(width),
            "height": int(height),
            "format": str(format),
        }

        async def _send() -> None:
            try:
                await client.send_json(message)
            except Exception:
                # Client disconnected mid-send; resolve the waiter to None.
                with self._lock:
                    if request_id in self._pending:
                        self._results[request_id] = None
                        self._pending[request_id].set()

        asyncio.run_coroutine_threadsafe(_send(), loop)

        if not evt.wait(timeout_s):
            with self._lock:
                self._pending.pop(request_id, None)
                self._results.pop(request_id, None)
            logger.debug(
                "Scene snapshot request %s timed out after %.2fs", request_id, timeout_s
            )
            return None
        with self._lock:
            result = self._results.pop(request_id, None)
            self._pending.pop(request_id, None)
        return result

    def deliver(self, request_id: str, image_bytes: Optional[bytes]) -> bool:
        """Resolve a pending request with bytes (or None for a known-failure)."""
        with self._lock:
            evt = self._pending.get(request_id)
            if evt is None:
                return False
            self._results[request_id] = image_bytes
            evt.set()
            return True


def create_snapshot_router(
    state: "ComposerState",
    security: SecurityConfig,
) -> APIRouter:
    """Create the snapshot WS + upload router. Mounted at app root.

    The upload POST route is protected by the app-level
    ``BearerAuthMiddleware`` when ``security.api_token`` is configured.
    The WebSocket needs its own ``Authorization`` (or
    ``?api_token=...``) check because middleware doesn't run on WS
    upgrades — without it any attacker who reaches the port could
    intercept the broker's snapshot_request messages and poison the
    VLM input via the upload route.
    """
    router = APIRouter()
    broker = getattr(state, "snapshot_broker", None)
    if broker is None:
        broker = SnapshotBroker()
    # Attach to state so the runner-provider resolver in state.py can find it
    # without importing the route module.
    state.snapshot_broker = broker  # type: ignore[attr-defined]

    @router.websocket("/ws/scene/snapshot")
    async def ws_snapshot(ws: WebSocket) -> None:
        if security.api_token and not check_ws_bearer_token(ws, security.api_token):
            await ws.close(code=4401, reason="Unauthorized")
            return
        await broker.attach(ws)
        try:
            while True:
                # The viewer's SnapshotResponder doesn't normally send
                # anything once attached. Receive in a loop so the WS
                # stays open until the client disconnects; tolerate
                # 'ping' for liveness sanity checks.
                msg = await ws.receive_text()
                if msg == "ping":
                    await ws.send_text("pong")
        except WebSocketDisconnect:
            pass
        finally:
            await broker.detach(ws)

    @router.post("/api/scene/snapshot/upload/{request_id}")
    async def upload_snapshot(request_id: str, request: Request) -> JSONResponse:
        body = await request.body()
        image_bytes: Optional[bytes] = bytes(body) if body else None
        delivered = broker.deliver(request_id, image_bytes)
        if not delivered:
            return JSONResponse(
                status_code=404,
                content={"detail": "Unknown or expired snapshot request id"},
            )
        return JSONResponse(content={"ok": True, "bytes": len(image_bytes or b"")})

    return router
