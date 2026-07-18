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
   format)`. The broker first waits for one generation-fenced visual build,
   then sends the required producer/frame identity with a fresh request id.
   Admission is capacity one and the complete prepare/capture/upload path
   shares one 2 s deadline.
3. The viewer's `SnapshotResponder` (running inside the R3F Canvas)
   waits for and applies that exact frame, renders into a reusable offscreen
   target without resizing the visible canvas, then POSTs the bytes to
   `/api/scene/snapshot/upload/{request_id}`.
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
import time
import uuid
from typing import Callable, Optional, TYPE_CHECKING

from fastapi import APIRouter, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from .security import SecurityConfig, check_ws_bearer_token
from .routes_scene import ensure_viewer_state_publisher

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)


class SnapshotBroker:
    """Bridge between BT-node-thread callers and the WS-connected viewer."""

    def __init__(
        self,
        *,
        prepare_capture: Callable[[float], tuple[str, int] | None] | None = None,
    ) -> None:
        self._lock = threading.Lock()
        self._clients: list[WebSocket] = []
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._prepare_capture = prepare_capture
        self._capture_admitted = False
        self._pending: dict[str, threading.Event] = {}
        self._results: dict[str, Optional[bytes]] = {}
        self._requests_total = 0
        self._completed_total = 0
        self._failed_total = 0
        self._timeouts_total = 0
        self._unavailable_total = 0
        self._busy_rejected_total = 0
        self._preparation_failed_total = 0
        self._deliveries_rejected_total = 0
        self._bytes_total = 0
        self._last_completion_ns: int | None = None

    def configure_prepare_capture(
        self,
        callback: Callable[[float], tuple[str, int] | None] | None,
    ) -> None:
        """Install the one-shot generation barrier before runtime starts."""
        with self._lock:
            if self._capture_admitted or self._pending:
                raise RuntimeError("cannot reconfigure snapshot preparation while active")
            self._prepare_capture = callback

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
        """Block boundedly on one prepared, generation-matched round-trip."""
        deadline = time.monotonic() + max(0.0, float(timeout_s))
        with self._lock:
            self._requests_total += 1
            if self._capture_admitted:
                self._busy_rejected_total += 1
                self._failed_total += 1
                return None
            if not self._clients or self._loop is None:
                self._unavailable_total += 1
                self._failed_total += 1
                return None
            self._capture_admitted = True
            prepare_capture = self._prepare_capture

        required_frame: tuple[str, int] | None = None
        if prepare_capture is not None:
            remaining = deadline - time.monotonic()
            if remaining > 0.0:
                try:
                    required_frame = prepare_capture(remaining)
                except Exception:
                    logger.exception("Scene snapshot preparation failed")
            if required_frame is None:
                with self._lock:
                    self._capture_admitted = False
                    self._preparation_failed_total += 1
                    self._failed_total += 1
                    self._last_completion_ns = time.monotonic_ns()
                return None

        with self._lock:
            if not self._clients or self._loop is None:
                self._capture_admitted = False
                self._unavailable_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
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
        if required_frame is not None:
            message["requiredProducerEpoch"] = required_frame[0]
            message["requiredFrameSequence"] = required_frame[1]
        message["timeoutMs"] = max(
            1,
            int(max(0.0, deadline - time.monotonic()) * 1000.0),
        )

        async def _send() -> None:
            try:
                await client.send_json(message)
            except Exception:
                # Client disconnected mid-send; resolve the waiter to None.
                with self._lock:
                    if request_id in self._pending:
                        self._results[request_id] = None
                        self._pending[request_id].set()

        try:
            asyncio.run_coroutine_threadsafe(_send(), loop)
        except RuntimeError:
            with self._lock:
                self._pending.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        remaining = max(0.0, deadline - time.monotonic())
        if not evt.wait(remaining):
            with self._lock:
                self._pending.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._timeouts_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            logger.debug(
                "Scene snapshot request %s timed out after %.2fs", request_id, timeout_s
            )
            return None
        with self._lock:
            result = self._results.pop(request_id, None)
            self._pending.pop(request_id, None)
            self._capture_admitted = False
            self._last_completion_ns = time.monotonic_ns()
            if result is None:
                self._failed_total += 1
            else:
                self._completed_total += 1
                self._bytes_total += len(result)
        return result

    def deliver(self, request_id: str, image_bytes: Optional[bytes]) -> bool:
        """Resolve a pending request with bytes (or None for a known-failure)."""
        with self._lock:
            evt = self._pending.get(request_id)
            if evt is None:
                self._deliveries_rejected_total += 1
                return False
            self._results[request_id] = image_bytes
            evt.set()
            return True

    def status(self) -> dict[str, int | None]:
        """Return bounded qualification/health evidence without client I/O."""
        with self._lock:
            return {
                "client_count": len(self._clients),
                "pending_count": len(self._pending),
                "requests_total": self._requests_total,
                "completed_total": self._completed_total,
                "failed_total": self._failed_total,
                "timeouts_total": self._timeouts_total,
                "unavailable_total": self._unavailable_total,
                "busy_rejected_total": self._busy_rejected_total,
                "preparation_failed_total": self._preparation_failed_total,
                "deliveries_rejected_total": self._deliveries_rejected_total,
                "bytes_total": self._bytes_total,
                "last_completion_ns": self._last_completion_ns,
            }


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

    def prepare_capture(timeout_s: float) -> tuple[str, int] | None:
        ensure_viewer_state_publisher(state, reason="snapshot_capture")
        generation = state.request_visual_state_one_shot(reason="snapshot_capture")
        result = state.wait_for_visual_state_one_shot(
            generation=generation,
            timeout_s=timeout_s,
        )
        if result is None or result.get("success") is not True:
            return None
        publication = state.last_viewer_publication()
        if publication is None:
            return None
        return publication.producer_epoch, publication.frame_sequence

    configure_prepare = getattr(broker, "configure_prepare_capture", None)
    if callable(configure_prepare):
        configure_prepare(prepare_capture)
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

    @router.get("/api/scene/snapshot/status")
    async def snapshot_status() -> dict[str, object]:
        return {
            "schema_version": "manyforge.viewer.snapshot_status.v1",
            **broker.status(),
        }

    return router
