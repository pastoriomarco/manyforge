"""WebSocket state broadcasting for ManyForge Composer."""
from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import TYPE_CHECKING, Any, List

from fastapi import WebSocket, WebSocketDisconnect
from .cycle_diagnostics import active_cycle_error_fields
from .models import StateUpdate, RuntimeOverrideStatusResponse

if TYPE_CHECKING:
    from .state import ComposerState

logger = logging.getLogger(__name__)
_WS_SEND_TIMEOUT_S = 0.25


class StateHub:
    """Manages WebSocket connections and broadcasts state updates."""

    def __init__(self):
        self._connections: List[WebSocket] = []
        self._running = False
        self._task: asyncio.Task | None = None
        self._state: ComposerState | None = None
        self._last_broadcast_event_index = 0
        self._last_kernel_event_index = 0
        self._last_kernel_event_source_identity: int | None = None

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self._connections.append(ws)
        logger.info("WebSocket client connected (total: %d)", len(self._connections))

    def disconnect(self, ws: WebSocket):
        if ws in self._connections:
            self._connections.remove(ws)
            logger.info("WebSocket client disconnected (total: %d)", len(self._connections))

    def connection_count(self) -> int:
        """Return the event-loop-owned state-channel client count."""
        return len(self._connections)
    
    async def broadcast(self, data: dict):
        dead = []
        connections = list(self._connections)
        # Serialize ONCE per frame; send_json would re-encode the same
        # payload for every connected client.
        text = json.dumps(data)

        async def _send(ws: WebSocket) -> WebSocket | None:
            try:
                await asyncio.wait_for(ws.send_text(text), timeout=_WS_SEND_TIMEOUT_S)
                return None
            except Exception:
                return ws

        results = await asyncio.gather(
            *(_send(ws) for ws in connections),
            return_exceptions=False,
        )
        for ws in results:
            if ws is not None:
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

    def _collect_frame(self, *, include_payload: bool) -> dict[str, Any] | None:
        state = self._state
        has_program = state is not None and state.program is not None
        if state is not None and hasattr(state, "event_recorder"):
            state.event_recorder.observe(state)
        native_kernel_events = []
        manager = state.cycle_manager if state is not None else None
        if manager is not None:
            source_identity = id(manager)
            since = (
                self._last_kernel_event_index
                if self._last_kernel_event_source_identity == source_identity
                else 0
            )
            manager.drain_kernel_events_now()
            native_kernel_events = manager.get_kernel_events_since(since)
            if hasattr(state, "event_recorder"):
                # Report monotonic loss counters even for an empty batch. A
                # collector/manager drop may be the only new fact after another
                # consumer advanced this source's retained event cursor.
                state.event_recorder.record_native_kernel_events(
                    native_kernel_events,
                    pipeline_status=(
                        manager.kernel_event_pipeline_status()
                    ),
                    source_identity=source_identity,
                )
            if native_kernel_events:
                self._last_kernel_event_index = native_kernel_events[-1].get(
                    "event_index", self._last_kernel_event_index
                )
            else:
                self._last_kernel_event_index = since
            self._last_kernel_event_source_identity = source_identity

        if not include_payload:
            return None

        kernel_events = []
        if state is not None and hasattr(state, "event_recorder"):
            kernel_events = state.event_recorder.get_events_since(
                self._last_broadcast_event_index,
                include_node_transitions=False,
            )
            if kernel_events:
                self._last_broadcast_event_index = kernel_events[-1]["event_index"]

        if state is not None and state.cycle_manager is not None:
            error, error_code, error_details = active_cycle_error_fields(
                state.cycle_manager
            )
            runtime_override_status = None
            if state.program is not None:
                runtime_override_status = RuntimeOverrideStatusResponse(
                    **state.runtime_override_status()
                ).model_dump(by_alias=True)
            return StateUpdate(
                state=state.cycle_manager.state.value,
                motionStopState=getattr(
                    state.cycle_manager,
                    "motion_stop_state",
                    "idle",
                ),
                velocityScale=state.cycle_manager.velocity_scale,
                nodeStatuses=state.cycle_manager.get_node_statuses(),
                kernelEvents=kernel_events,
                nativeKernelEvents=native_kernel_events,
                error=error,
                errorCode=error_code,
                errorDetails=error_details,
                recoveryOffer=state.cycle_manager.get_recovery_offer(),
                programLoaded=bool(has_program),
                programSessionGeneration=state.program_session_generation,
                runtimeOverrideStatus=runtime_override_status,
                timestamp=time.time(),
            ).model_dump(by_alias=True)

        runtime_override_status = None
        if state is not None and state.program is not None:
            runtime_override_status = RuntimeOverrideStatusResponse(
                **state.runtime_override_status()
            ).model_dump(by_alias=True)
        return StateUpdate(
            state="idle",
            motionStopState="idle",
            velocityScale=1.0,
            nodeStatuses={},
            kernelEvents=kernel_events,
            nativeKernelEvents=[],
            recoveryOffer=None,
            programLoaded=bool(has_program),
            programSessionGeneration=(
                state.program_session_generation if state is not None else 0
            ),
            runtimeOverrideStatus=runtime_override_status,
            timestamp=time.time(),
        ).model_dump(by_alias=True)

    async def _broadcast_loop(self):
        """Poll state at ~10Hz and broadcast to all connected clients.

        The recorder feed (observe + native kernel event drain) runs on
        every tick regardless of clients — durable history must not
        depend on a browser being connected. The pydantic StateUpdate
        construction and the broadcast are skipped with no clients: that
        is pure per-frame serialization work for an empty audience.
        """
        while self._running:
            try:
                # Keep a fresh monotonic<->wall clock anchor near every event
                # window (rate-limited inside; observability plan OW2).
                refresh_anchor = getattr(self._state, "refresh_clock_anchor", None)
                if refresh_anchor is not None:
                    await asyncio.to_thread(refresh_anchor)
                data = await asyncio.to_thread(
                    self._collect_frame,
                    include_payload=bool(self._connections),
                )
                if data is None:
                    await asyncio.sleep(0.1)
                    continue
                if self._connections:
                    await self.broadcast(data)
                await asyncio.sleep(0.1)  # 10 Hz
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("WebSocket state broadcast iteration failed")
                await asyncio.sleep(0.5)
