"""WebSocket state broadcasting for ManyForge Composer."""
from __future__ import annotations

import asyncio
import logging
import time
from typing import TYPE_CHECKING, List

from fastapi import WebSocket, WebSocketDisconnect
from .models import StateUpdate, RuntimeOverrideStatusResponse

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
        self._last_broadcast_event_index = 0
        self._last_kernel_event_index = 0

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
        self._last_broadcast_event_index = 0
        self._last_kernel_event_index = 0
    
    def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
    
    async def _broadcast_loop(self):
        """Poll state at ~10Hz and broadcast to all connected clients."""
        while self._running:
            has_program = self._state and self._state.program is not None
            kernel_events = []
            if self._state is not None and hasattr(self._state, "event_recorder"):
                self._state.event_recorder.observe(self._state)
                kernel_events = self._state.event_recorder.get_events_since(
                    self._last_broadcast_event_index,
                    include_node_transitions=False,
                )
                if kernel_events:
                    self._last_broadcast_event_index = kernel_events[-1]["event_index"]
            native_kernel_events = []
            if self._state and self._state.cycle_manager is not None:
                native_kernel_events = self._state.cycle_manager.get_kernel_events_since(
                    self._last_kernel_event_index
                )
                if native_kernel_events:
                    self._last_kernel_event_index = native_kernel_events[-1].get(
                        "event_index", self._last_kernel_event_index
                    )

            if self._state and self._state.cycle_manager is not None:
                runtime_override_status = None
                if self._state.program is not None:
                    runtime_override_status = RuntimeOverrideStatusResponse(
                        **self._state.runtime_override_status()
                    ).model_dump(by_alias=True)
                data = StateUpdate(
                    state=self._state.cycle_manager.state.value,
                    velocityScale=self._state.cycle_manager.velocity_scale,
                    nodeStatuses=self._state.cycle_manager.get_node_statuses(),
                    kernelEvents=kernel_events,
                    nativeKernelEvents=native_kernel_events,
                    error=self._state.cycle_manager.error,
                    programLoaded=bool(has_program),
                    programSessionGeneration=self._state.program_session_generation,
                    runtimeOverrideStatus=runtime_override_status,
                    timestamp=time.time(),
                ).model_dump(by_alias=True)
            else:
                runtime_override_status = None
                if self._state is not None and self._state.program is not None:
                    runtime_override_status = RuntimeOverrideStatusResponse(
                        **self._state.runtime_override_status()
                    ).model_dump(by_alias=True)
                data = StateUpdate(
                    state="idle",
                    velocityScale=1.0,
                    nodeStatuses={},
                    kernelEvents=kernel_events,
                    nativeKernelEvents=[],
                    programLoaded=bool(has_program),
                    programSessionGeneration=(
                        self._state.program_session_generation if self._state is not None else 0
                    ),
                    runtimeOverrideStatus=runtime_override_status,
                    timestamp=time.time(),
                ).model_dump(by_alias=True)
            await self.broadcast(data)
            await asyncio.sleep(0.1)  # 10 Hz
