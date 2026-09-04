"""Transparent, demand-driven Composer proxy for Headless `/ws/control`."""

from __future__ import annotations

import asyncio
from contextlib import suppress
from copy import deepcopy
import json
import logging
import re
import threading
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.control_protocol import (
    CONTROL_MAXIMUM_CLIENTS,
    CONTROL_MAXIMUM_MESSAGE_BYTES,
    CONTROL_MAXIMUM_SNAPSHOT_BYTES,
)
from websockets.asyncio.client import connect as connect_websocket
from websockets.exceptions import ConnectionClosed

from .authoring_admission import AuthoringAdmissionCoordinator
from .draft_session import ComposerDraftSession


_SEND_TIMEOUT_SECONDS = 0.25
_STATE_MESSAGE = re.compile(r'"message_kind"\s*:\s*"(?:snapshot|delta)"')
_LOGGER = logging.getLogger(__name__)


class HeadlessStateSource:
    """Own no state semantics; enrich and proxy only while browsers demand it."""

    def __init__(
        self,
        client: HeadlessHttpClient,
        *,
        admission: AuthoringAdmissionCoordinator,
        draft_session: ComposerDraftSession,
    ) -> None:
        self._client = client
        self._admission = admission
        self._draft_session = draft_session
        self._lock = asyncio.Lock()
        self._clients = 0
        self._closed = False
        self._latest_overview: dict[str, Any] | None = None
        self._local_revision = 0
        self._enrich_lock = threading.Lock()

    @property
    def control_websocket_url(self) -> str:
        return self._client.control_websocket_url

    async def acquire(self) -> None:
        async with self._lock:
            if self._closed:
                raise RuntimeError("Composer control proxy is closed")
            if self._clients >= CONTROL_MAXIMUM_CLIENTS:
                raise RuntimeError("Composer control client limit reached")
            self._clients += 1

    async def release(self) -> None:
        async with self._lock:
            self._clients = max(0, self._clients - 1)

    async def close(self) -> None:
        async with self._lock:
            self._closed = True

    def enrich(self, payload: str) -> str:
        if len(payload.encode("utf-8")) > CONTROL_MAXIMUM_SNAPSHOT_BYTES:
            raise ValueError("Headless control projection is oversized")
        value = json.loads(payload)
        if not isinstance(value, dict):
            raise ValueError("Headless control projection is not an object")
        kind = value.get("message_kind")
        if kind not in {"snapshot", "delta"}:
            # Receipts and heartbeats are already strict owner messages. A
            # transparent proxy must not parse-copy-encode them on the command
            # response path merely to return identical bytes.
            return payload
        with self._enrich_lock:
            shell = value.get("shell")
            if isinstance(shell, dict):
                # json.loads() already produced a private tree for this frame.
                draft = self._draft_session.snapshot()
                self._local_revision += 1
                shell["draft_identity"] = {
                    "draft_id": draft["draft_id"],
                    "draft_revision": draft["draft_revision"],
                }
                shell["receiver_epoch"] = self._admission.receiver_epoch
                shell["snapshot_sequence"] = self._local_revision
                shell["schema_version"] = "manyforge.composer_headless_state.v2"
                shell["complete"] = True
                value["shell"] = shell
                overview = shell.get("component_overview")
                if isinstance(overview, dict):
                    self._latest_overview = overview
            encoded = json.dumps(
                value,
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
        if len(encoded.encode("utf-8")) > CONTROL_MAXIMUM_SNAPSHOT_BYTES:
            raise ValueError("Composer control projection is oversized")
        return encoded

    def current_component_overview(self) -> dict[str, Any] | None:
        value = self._latest_overview
        return None if value is None else deepcopy(value)


async def serve_headless_state(
    websocket: WebSocket,
    source: HeadlessStateSource,
) -> None:
    """Proxy one ordered control session without polling or semantic reduction."""

    try:
        await source.acquire()
    except RuntimeError:
        await websocket.close(code=1013, reason="control client unavailable")
        return
    await websocket.accept()
    tasks: set[asyncio.Task[None]] = set()
    try:
        async with connect_websocket(
            source.control_websocket_url,
            # Control and hot-state deltas are small and latency-sensitive.
            # Compressing them on both sides of this local relay only adds
            # event-loop/GIL work and can delay the independent Stop path.
            compression=None,
            max_size=CONTROL_MAXIMUM_SNAPSHOT_BYTES,
            open_timeout=1.0,
            close_timeout=0.25,
        ) as upstream:
            async def browser_to_owner() -> None:
                while True:
                    message = await websocket.receive_text()
                    if len(message.encode("utf-8")) > CONTROL_MAXIMUM_MESSAGE_BYTES:
                        raise ValueError("control client message is oversized")
                    await upstream.send(message)

            async def owner_to_browser() -> None:
                async for message in upstream:
                    if not isinstance(message, str):
                        raise ValueError("Headless control frame is not text")
                    payload = (
                        await asyncio.to_thread(source.enrich, message)
                        if _STATE_MESSAGE.search(message)
                        else source.enrich(message)
                    )
                    await asyncio.wait_for(
                        websocket.send_text(payload),
                        timeout=_SEND_TIMEOUT_SECONDS,
                    )

            tasks = {
                asyncio.create_task(browser_to_owner()),
                asyncio.create_task(owner_to_browser()),
            }
            done, _pending = await asyncio.wait(
                tasks,
                return_when=asyncio.FIRST_EXCEPTION,
            )
            for task in done:
                task.result()
    except (ConnectionClosed, WebSocketDisconnect):
        pass
    except (
        asyncio.TimeoutError,
        ConnectionError,
        OSError,
        RuntimeError,
        ValueError,
    ) as error:
        _LOGGER.warning(
            "control proxy closed fail-closed: %s: %s",
            type(error).__name__,
            error,
        )
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        for task in tasks:
            with suppress(
                asyncio.CancelledError,
                ConnectionClosed,
                ConnectionError,
                OSError,
                RuntimeError,
                ValueError,
                WebSocketDisconnect,
            ):
                await task
        await source.release()
        with suppress(RuntimeError, WebSocketDisconnect):
            await websocket.close(code=1000)


__all__ = ["HeadlessStateSource", "serve_headless_state"]
