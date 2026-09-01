"""Dedicated bounded viewer-observation transport."""

from __future__ import annotations

import asyncio
from contextlib import suppress
from typing import TYPE_CHECKING, Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .security import SecurityConfig

if TYPE_CHECKING:
    from .execution_observation_stream import ExecutionObservationSubscription
    from .state import ComposerState


_SEND_TIMEOUT_SECONDS = 0.25


def create_viewer_router(
    state: "ComposerState",
    _security: SecurityConfig,
) -> APIRouter:
    """Expose one demand-bound source without sharing shell/control writes."""

    if state.execution_observation_source is None:
        raise ValueError("viewer observation source is unavailable")
    source = state.execution_observation_source
    router = APIRouter()

    @router.get("/api/scene/viewer/status")
    async def viewer_status() -> dict[str, Any]:
        return source.status()

    @router.websocket("/ws/scene/viewer")
    async def viewer_observation(websocket: WebSocket) -> None:
        await websocket.accept()
        subscription: ExecutionObservationSubscription = source.subscribe()

        async def send_latest() -> None:
            while True:
                payload = await subscription.queue.get()
                await asyncio.wait_for(
                    websocket.send_text(payload),
                    timeout=_SEND_TIMEOUT_SECONDS,
                )

        async def wait_for_disconnect() -> None:
            while True:
                message = await websocket.receive()
                if message.get("type") == "websocket.disconnect":
                    return
                # This observer is deliberately one-way. Any inbound payload
                # ends the session rather than creating a hidden command lane.
                return

        sender = asyncio.create_task(send_latest())
        receiver = asyncio.create_task(wait_for_disconnect())
        try:
            await asyncio.wait(
                {sender, receiver},
                return_when=asyncio.FIRST_COMPLETED,
            )
        except (WebSocketDisconnect, asyncio.TimeoutError):
            pass
        finally:
            for task in (sender, receiver):
                task.cancel()
            for task in (sender, receiver):
                with suppress(
                    asyncio.CancelledError,
                    WebSocketDisconnect,
                    asyncio.TimeoutError,
                ):
                    await task
            source.unsubscribe(subscription)
            with suppress(WebSocketDisconnect, RuntimeError):
                await websocket.close()

    return router


__all__ = ["create_viewer_router"]
