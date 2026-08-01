"""Generic WebSocket fanout plus the fenced headless-state projection hook."""

from __future__ import annotations

import asyncio
import json
from typing import Any, Final, TYPE_CHECKING

from fastapi import WebSocket

if TYPE_CHECKING:
    from .state import ComposerState


_WS_SEND_TIMEOUT_S: Final = 0.25
_WS_MAX_PAYLOAD_BYTES: Final = 2 * 1024 * 1024
STATE_PROJECTION_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_state_projection_unavailable"
)
STATE_PROJECTION_RESTORATION_GATE: Final = "H0"


class StateProjectionUnavailableError(RuntimeError):
    """The headless typed state snapshot has not been connected."""


class StateHub:
    """Event-loop-owned transport for already-built payloads only.

    It does not poll Composer state, assemble runtime truth, drain events, or
    start a background task. Those old semantic-owner responsibilities return
    only through the H0 headless snapshot client.
    """

    def __init__(self) -> None:
        self._connections: list[WebSocket] = []

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self._connections.append(websocket)

    def disconnect(self, websocket: WebSocket) -> None:
        if websocket in self._connections:
            self._connections.remove(websocket)

    def connection_count(self) -> int:
        return len(self._connections)

    async def broadcast(self, payload: dict[str, Any]) -> None:
        """Boundedly deliver one caller-built JSON object to each observer."""
        encoded = json.dumps(
            payload,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        if len(encoded.encode("utf-8")) > _WS_MAX_PAYLOAD_BYTES:
            raise ValueError("state transport payload exceeds bounded size")

        async def send(websocket: WebSocket) -> WebSocket | None:
            try:
                await asyncio.wait_for(
                    websocket.send_text(encoded),
                    timeout=_WS_SEND_TIMEOUT_S,
                )
                return None
            except Exception:
                return websocket

        failed = await asyncio.gather(*(send(item) for item in tuple(self._connections)))
        for websocket in failed:
            if websocket is not None:
                self.disconnect(websocket)

    def start(self, _state: "ComposerState") -> None:
        """Fail before reading state or creating any polling task."""
        raise StateProjectionUnavailableError(
            f"{STATE_PROJECTION_UNAVAILABLE_REASON_CODE}: runtime state "
            "projection returns from the H0 headless snapshot owner"
        )

    def stop(self) -> None:
        """No-op: this transport owns no background task."""


__all__ = ["StateHub", "StateProjectionUnavailableError"]
