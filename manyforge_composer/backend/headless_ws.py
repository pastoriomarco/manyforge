"""Bounded WebSocket transport for unmodified headless owner snapshots."""

from __future__ import annotations

import asyncio
import json
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


POLL_INTERVAL_SECONDS = 0.25
SEND_TIMEOUT_SECONDS = 0.25
ALARM_PAGE_LIMIT = 32
MAXIMUM_MESSAGE_BYTES = 16 * 1024 * 1024


def _encode(value: dict[str, Any]) -> str:
    encoded = json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    if len(encoded.encode("utf-8")) > MAXIMUM_MESSAGE_BYTES:
        raise ValueError("headless WebSocket projection exceeds its byte bound")
    return encoded


async def serve_headless_state(
    websocket: WebSocket,
    client: HeadlessHttpClient,
) -> None:
    """Poll immutable projections per connection; retain no runtime truth."""

    await websocket.accept()
    previous_identity: tuple[object, ...] | None = None
    try:
        while True:
            try:
                runtime, alarms = await asyncio.gather(
                    asyncio.to_thread(client.runtime),
                    asyncio.to_thread(
                        client.alarms,
                        limit=ALARM_PAGE_LIMIT,
                        ordering="attention_newest",
                    ),
                )
                identity = (
                    "snapshot",
                    runtime.get("projection_owner_epoch"),
                    runtime.get("projection_generation"),
                    alarms.get("owner_epoch"),
                    alarms.get("generation"),
                    alarms.get("state_revision"),
                )
                if identity != previous_identity:
                    payload = {
                        "schema_version": "manyforge.composer_headless_state.v1",
                        "runtime": runtime,
                        "alarms": alarms,
                    }
                    try:
                        encoded = _encode(payload)
                    except ValueError:
                        identity = (
                            "error",
                            "headless_websocket_projection_oversized",
                        )
                        encoded = _encode(
                            {
                                "schema_version": (
                                    "manyforge.composer_headless_state.v1"
                                ),
                                "runtime": None,
                                "alarms": None,
                                "error": {
                                    "code": (
                                        "headless_websocket_projection_oversized"
                                    ),
                                    "message": (
                                        "headless WebSocket projection exceeds "
                                        "its byte bound"
                                    ),
                                },
                            }
                        )
                    await asyncio.wait_for(
                        websocket.send_text(encoded),
                        timeout=SEND_TIMEOUT_SECONDS,
                    )
                    previous_identity = identity
            except HeadlessClientError as error:
                identity = ("error", error.code, str(error))
                payload = {
                    "schema_version": "manyforge.composer_headless_state.v1",
                    "runtime": None,
                    "alarms": None,
                    "error": {"code": error.code, "message": str(error)},
                }
                if identity != previous_identity:
                    try:
                        encoded = _encode(payload)
                    except ValueError:
                        identity = (
                            "error",
                            "headless_websocket_error_projection_oversized",
                        )
                        encoded = _encode(
                            {
                                "schema_version": (
                                    "manyforge.composer_headless_state.v1"
                                ),
                                "runtime": None,
                                "alarms": None,
                                "error": {
                                    "code": (
                                        "headless_websocket_error_projection_oversized"
                                    ),
                                    "message": (
                                        "headless WebSocket error projection "
                                        "exceeds its byte bound"
                                    ),
                                },
                            }
                        )
                    await asyncio.wait_for(
                        websocket.send_text(encoded),
                        timeout=SEND_TIMEOUT_SECONDS,
                    )
                    previous_identity = identity
            await asyncio.sleep(POLL_INTERVAL_SECONDS)
    except (WebSocketDisconnect, asyncio.TimeoutError):
        return


__all__ = ["serve_headless_state"]
