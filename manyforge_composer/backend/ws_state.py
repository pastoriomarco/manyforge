"""WebSocket state broadcasting for ManyForge Composer."""
from __future__ import annotations

import asyncio
import logging
import time
from typing import TYPE_CHECKING, List

from fastapi import WebSocket, WebSocketDisconnect

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)


class StateHub:
    """Manages WebSocket connections and broadcasts state updates."""

    def __init__(self):
        self._connections: List[WebSocket] = []
        self._running = False
        self._task: asyncio.Task | None = None
        self._state: ComposerState | None = None

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self._connections.append(ws)
        logger.info("WebSocket client connected (total: %d)", len(self._connections))

    def disconnect(self, ws: WebSocket):
        if ws in self._connections:
            self._connections.remove(ws)
            logger.info("WebSocket client disconnected (total: %d)", len(self._connections))
    
    async def broadcast(self, data: dict):
        dead = []
        for ws in self._connections:
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)
    
    def start(self, state: "ComposerState"):
        """Start background broadcasting task."""
        self._running = True
        self._state = state
    
    def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
    
    async def _broadcast_loop(self):
        """Poll state at ~10Hz and broadcast to all connected clients."""
        while self._running:
            if self._state and self._state.cycle_manager is not None:
                data = {
                    "state": self._state.cycle_manager.state.value,
                    "velocityScale": self._state.cycle_manager.velocity_scale,
                    "nodeStatuses": self._state.cycle_manager.get_node_statuses(),
                    "timestamp": time.time(),
                }
            else:
                data = {
                    "state": "idle",
                    "velocityScale": 1.0,
                    "nodeStatuses": {},
                    "timestamp": time.time(),
                }
            await self.broadcast(data)
            await asyncio.sleep(0.1)  # 10 Hz
