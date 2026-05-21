"""Point-cloud / octomap streaming - /ws/scene/pointcloud

Native viewer Day 4: WebSocket fan-out for live point-cloud frames
from a perception source to all connected viewer clients. The
protocol is intentionally minimal so a real octomap / depth-sensor
producer can register as a publisher without further coordination
with the viewer.

Publisher API (server-side):
    state.point_cloud_publisher.publish(points, colors=None)
    state.point_cloud_publisher.clear()

Wire format (server → client, JSON):
    { "type": "pointcloud_frame",
      "timestamp_ns": int,
      "points": [[x, y, z], ...],    # world-frame metres
      "colors": [[r, g, b], ...] }   # optional, 0-1 floats

Clear (e.g. when scene is reset):
    { "type": "pointcloud_clear", "timestamp_ns": int }

No frames flow until a publisher registers, which keeps the viewer
overhead at zero on scenarios that don't use octomap. The renderer
on the client side reads the latest frame from a small zustand
store; we deliberately stream individual frames rather than diffs
in this first pass — diff/delta protocol can come in a Day 7+
refinement once a real source is wired and the bandwidth profile
is measurable.
"""
from __future__ import annotations

import asyncio
import logging
import threading
import time
from typing import Optional, Sequence, TYPE_CHECKING

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .security import SecurityConfig, check_ws_bearer_token

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)
_POINTCLOUD_SEND_TIMEOUT_S = 0.25


class PointCloudPublisher:
    """Fan-out hub for live point-cloud frames.

    Owned by ``ComposerState.point_cloud_publisher``. A perception
    source calls ``publish()`` from its own thread; this class
    serialises the frame as JSON and forwards it to every connected
    viewer WS client via ``asyncio.run_coroutine_threadsafe`` (same
    pattern as ``SnapshotBroker.render_sync`` in Day 3).

    State semantics:
    - ``latest_frame`` holds the most recent publish so a newly
      connected client can be sent a starter frame on attach,
      mirroring the publisher's behaviour on the next push without
      forcing the client to wait for the next perception tick.
    - ``clear()`` resets ``latest_frame`` and broadcasts a clear
      message so clients drop their current rendering.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._clients: list[WebSocket] = []
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._latest_frame: Optional[dict] = None

    async def attach(self, ws: WebSocket) -> None:
        await ws.accept()
        loop = asyncio.get_running_loop()
        starter: Optional[dict]
        with self._lock:
            self._clients.append(ws)
            if self._loop is None:
                self._loop = loop
            starter = self._latest_frame
        if starter is not None:
            try:
                await ws.send_json(starter)
            except Exception:
                logger.debug("PointCloud starter-frame send failed", exc_info=True)

    async def detach(self, ws: WebSocket) -> None:
        with self._lock:
            if ws in self._clients:
                self._clients.remove(ws)

    def has_clients(self) -> bool:
        with self._lock:
            return bool(self._clients)

    def publish(
        self,
        points: Sequence[Sequence[float]],
        colors: Optional[Sequence[Sequence[float]]] = None,
    ) -> None:
        """Push a point-cloud frame to every connected viewer.

        ``points`` is a sequence of ``[x, y, z]`` triples in world
        metres. ``colors`` is an optional sequence of ``[r, g, b]``
        triples (0-1 floats); when omitted, the client renders the
        cloud with a default material colour.
        """
        frame = {
            "type": "pointcloud_frame",
            "timestamp_ns": time.monotonic_ns(),
            "points": [[float(p[0]), float(p[1]), float(p[2])] for p in points],
        }
        if colors is not None:
            frame["colors"] = [
                [float(c[0]), float(c[1]), float(c[2])] for c in colors
            ]
        with self._lock:
            self._latest_frame = frame
            clients = list(self._clients)
            loop = self._loop
        if not clients or loop is None:
            return
        for ws in clients:
            asyncio.run_coroutine_threadsafe(self._send_safe(ws, frame), loop)

    def clear(self) -> None:
        message = {"type": "pointcloud_clear", "timestamp_ns": time.monotonic_ns()}
        with self._lock:
            self._latest_frame = None
            clients = list(self._clients)
            loop = self._loop
        if not clients or loop is None:
            return
        for ws in clients:
            asyncio.run_coroutine_threadsafe(self._send_safe(ws, message), loop)

    async def _send_safe(self, ws: WebSocket, frame: dict) -> None:
        try:
            await asyncio.wait_for(
                ws.send_json(frame),
                timeout=_POINTCLOUD_SEND_TIMEOUT_S,
            )
        except Exception:
            logger.debug("PointCloud send failed (client likely disconnected)", exc_info=True)
            await self.detach(ws)


def create_pointcloud_router(
    state: "ComposerState",
    security: SecurityConfig,
) -> APIRouter:
    """Create the point-cloud WS router.

    Mounted at app root because the WS path is absolute
    (``/ws/scene/pointcloud``). The HTTP-level
    ``BearerAuthMiddleware`` doesn't run on WS upgrades, so we
    check the token here when one is configured.
    """
    router = APIRouter()
    publisher = PointCloudPublisher()
    state.point_cloud_publisher = publisher  # type: ignore[attr-defined]

    @router.websocket("/ws/scene/pointcloud")
    async def ws_pointcloud(ws: WebSocket) -> None:
        if security.api_token and not check_ws_bearer_token(ws, security.api_token):
            await ws.close(code=4401, reason="Unauthorized")
            return
        await publisher.attach(ws)
        try:
            while True:
                msg = await ws.receive_text()
                if msg == "ping":
                    await ws.send_text("pong")
        except WebSocketDisconnect:
            pass
        finally:
            await publisher.detach(ws)

    return router
