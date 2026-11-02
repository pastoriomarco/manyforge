"""Dedicated bounded viewer-observation transport."""

from __future__ import annotations

import asyncio
from contextlib import suppress
import json
from typing import TYPE_CHECKING, Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .security import SecurityConfig

if TYPE_CHECKING:
    from .execution_observation_stream import ExecutionObservationSubscription
    from .state import ComposerState


_SEND_TIMEOUT_SECONDS = 0.25
_MAXIMUM_CREDIT_BYTES = 4_096


def _credit_identity(message: str) -> tuple[tuple[str, str, int, int] | None, int]:
    from .execution_observation_stream import OBSERVATION_CREDIT_SCHEMA

    if len(message.encode("utf-8")) > _MAXIMUM_CREDIT_BYTES:
        raise ValueError("execution observation credit is oversized")
    try:
        value = json.loads(message)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValueError("execution observation credit is not JSON") from error
    if (
        not isinstance(value, dict)
        or set(value) != {"last_applied", "pose_rate_hz", "schema_version"}
        or value.get("schema_version") != OBSERVATION_CREDIT_SCHEMA
    ):
        raise ValueError("execution observation credit has an invalid shape")
    rate = value.get("pose_rate_hz")
    if type(rate) is not int or not 5 <= rate <= 60:
        raise ValueError("viewer pose rate must be an integer in 5..60 Hz")
    cursor = value.get("last_applied")
    if cursor is None:
        return None, rate
    if (
        not isinstance(cursor, dict)
        or set(cursor) != {"owner_epoch", "source_epoch", "source_sequence", "delivery_sequence"}
        or not isinstance(cursor.get("owner_epoch"), str)
        or not cursor["owner_epoch"]
        or len(cursor["owner_epoch"].encode("utf-8")) > 2_048
        or not isinstance(cursor.get("source_epoch"), str)
        or not cursor["source_epoch"]
        or len(cursor["source_epoch"].encode("utf-8")) > 2_048
        or type(cursor.get("source_sequence")) is not int
        or not 0 <= cursor["source_sequence"] <= 9_007_199_254_740_991
        or type(cursor.get("delivery_sequence")) is not int
        or not 0 <= cursor["delivery_sequence"] <= 9_007_199_254_740_991
    ):
        raise ValueError("execution observation credit cursor is invalid")
    return (
        (
            cursor["owner_epoch"],
            cursor["source_epoch"],
            cursor["source_sequence"],
            cursor["delivery_sequence"],
        ),
        rate,
    )


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
        try:
            subscription: ExecutionObservationSubscription = source.subscribe()
        except RuntimeError:
            await websocket.close(
                code=1013,
                reason="execution observation client limit reached",
            )
            return

        last_sent_at: float | None = None
        send_complete = asyncio.Event()
        send_complete.set()

        async def send_latest() -> None:
            nonlocal last_sent_at
            from .execution_observation_stream import (
                ExecutionObservationSourceUnavailable,
            )

            while True:
                await subscription.wake.wait()
                try:
                    payload = source.take_payload(subscription)
                except ExecutionObservationSourceUnavailable:
                    await websocket.close(
                        code=1013, reason="execution observation source unavailable"
                    )
                    return
                if payload is None:
                    continue
                send_complete.clear()
                await asyncio.wait_for(
                    websocket.send_text(payload),
                    timeout=_SEND_TIMEOUT_SECONDS,
                )
                last_sent_at = asyncio.get_running_loop().time()
                send_complete.set()

        async def receive_credit() -> None:
            from .execution_observation_stream import (
                OBSERVATION_DEMAND_LEASE_SECONDS,
            )

            while True:
                message = await asyncio.wait_for(
                    websocket.receive_text(),
                    timeout=OBSERVATION_DEMAND_LEASE_SECONDS,
                )
                identity, rate = _credit_identity(message)
                await send_complete.wait()
                if last_sent_at is not None:
                    remaining = last_sent_at + 1.0 / rate - asyncio.get_running_loop().time()
                    if remaining > 0:
                        await asyncio.sleep(remaining)
                source.grant_credit(subscription, identity)

        sender = asyncio.create_task(send_latest())
        receiver = asyncio.create_task(receive_credit())
        try:
            await asyncio.wait(
                {sender, receiver},
                return_when=asyncio.FIRST_COMPLETED,
            )
        except (WebSocketDisconnect, asyncio.TimeoutError, ValueError):
            pass
        finally:
            for task in (sender, receiver):
                task.cancel()
            for task in (sender, receiver):
                with suppress(
                    asyncio.CancelledError,
                    WebSocketDisconnect,
                    asyncio.TimeoutError,
                    ValueError,
                ):
                    await task
            source.unsubscribe(subscription)
            with suppress(WebSocketDisconnect, RuntimeError):
                await websocket.close()

    return router


__all__ = ["create_viewer_router"]
