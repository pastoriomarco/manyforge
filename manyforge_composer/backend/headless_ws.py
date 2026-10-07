"""Transparent, demand-driven Composer proxy for Headless `/ws/control`."""

from __future__ import annotations

import asyncio
from contextlib import suppress
from dataclasses import dataclass
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
    MAXIMUM_U64,
)
from websockets.asyncio.client import connect as connect_websocket
from websockets.exceptions import ConnectionClosed

from .authoring_admission import AuthoringAdmissionCoordinator
from .authoring_workspace import AuthoringWorkspace


_SEND_TIMEOUT_SECONDS = 0.25
_STATE_MESSAGE = re.compile(r'"message_kind"\s*:\s*"(?:snapshot|delta)"')
_SNAPSHOT_MESSAGE = re.compile(r'"message_kind"\s*:\s*"snapshot"')
_SERVER_SEQUENCE = re.compile(r'("server_sequence"\s*:\s*")([1-9][0-9]*)(")')
# Starts, resumes and recovery actions go through Composer's HTTP admission,
# which checks the shared Draft under the authoring serialization lock.
_HTTP_ADMITTED_COMMANDS = frozenset({
    "program_run", "program_motion_start", "program_motion_resume",
    "program_resume_full_stop", "operation_start", "operation_resume",
    "execution_recovery",
})
_LOGGER = logging.getLogger(__name__)


@dataclass(eq=False, slots=True)
class _LocalStateSubscription:
    loop: asyncio.AbstractEventLoop
    wake: asyncio.Event


class HeadlessStateSource:
    """Own no state semantics; enrich and proxy only while browsers demand it."""

    def __init__(
        self,
        client: HeadlessHttpClient,
        *,
        admission: AuthoringAdmissionCoordinator,
        workspace: AuthoringWorkspace,
    ) -> None:
        self._client = client
        self._admission = admission
        self._workspace = workspace
        self._lock = threading.Lock()
        self._closed = False
        self._local_revision = 0
        self._enrich_lock = threading.Lock()
        self._local_subscriptions: set[_LocalStateSubscription] = set()
        # Subscribe to the composite workspace owner. It is the only writer of
        # the two Draft sessions, so one ordinary edit, Save, whole-project Load
        # or Deployment edit all reach this single wake instead of only the
        # Program Draft that the old Program-only subscription covered.
        self._workspace.set_observer(self.notify_local_state_change)

    @property
    def control_websocket_url(self) -> str:
        return self._client.control_websocket_url

    async def acquire(self) -> _LocalStateSubscription:
        with self._lock:
            if self._closed:
                raise RuntimeError("Composer control proxy is closed")
            if len(self._local_subscriptions) >= CONTROL_MAXIMUM_CLIENTS:
                raise RuntimeError("Composer control client limit reached")
            subscription = _LocalStateSubscription(
                loop=asyncio.get_running_loop(),
                wake=asyncio.Event(),
            )
            self._local_subscriptions.add(subscription)
            return subscription

    async def release(self, subscription: _LocalStateSubscription) -> None:
        with self._lock:
            self._local_subscriptions.discard(subscription)

    async def close(self) -> None:
        with self._lock:
            self._closed = True
        self._workspace.set_observer(None)

    def notify_local_state_change(self) -> None:
        """Wake each browser once when the composite workspace advances.

        Each subscription holds one ``asyncio.Event``, so concurrent commits
        coalesce into a single pending wake, and the newest identity is read
        when that wake is served rather than when it was raised.
        """

        with self._lock:
            if self._closed:
                return
            subscriptions = tuple(self._local_subscriptions)
        for subscription in subscriptions:
            subscription.loop.call_soon_threadsafe(subscription.wake.set)

    def _local_shell(self, *, complete: bool) -> dict[str, Any]:
        # A contained workspace still publishes its last identity beside the
        # containment, so every client shows it without attempting an edit.
        identity, containment = self._workspace.publication()
        self._local_revision += 1
        return {
            "complete": complete,
            "authoring_workspace": {
                "workspace_epoch": identity.workspace_epoch,
                "workspace_revision": identity.workspace_revision,
                "program_draft_id": identity.program_draft_id,
                "program_draft_revision": identity.program_draft_revision,
                "deployment_draft_id": identity.deployment_draft_id,
                "deployment_draft_revision": identity.deployment_draft_revision,
            },
            "authoring_workspace_containment": containment,
            "receiver_epoch": self._admission.receiver_epoch,
            "schema_version": "manyforge.composer_headless_state.v2",
            "snapshot_sequence": self._local_revision,
        }

    def enrich(self, payload: str) -> str:
        if len(payload.encode("utf-8")) > CONTROL_MAXIMUM_SNAPSHOT_BYTES:
            raise ValueError("Headless control projection is oversized")
        # Owner deltas are already sparse, validated control-protocol frames.
        # Composer's local Draft identity changes on its own direct wake, so
        # parsing and re-encoding every unrelated owner delta would add no fact.
        if _SNAPSHOT_MESSAGE.search(payload) is None:
            return payload
        value = json.loads(payload)
        if not isinstance(value, dict):
            raise ValueError("Headless control projection is not an object")
        kind = value.get("message_kind")
        if kind != "snapshot":
            raise ValueError("Headless snapshot classification is invalid")
        with self._enrich_lock:
            shell = value.get("shell")
            if isinstance(shell, dict):
                shell.update(self._local_shell(complete=True))
                value["shell"] = shell
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

    def local_delta(self, template: str) -> str:
        """Build one tiny latest-only composite workspace identity delta."""

        value = json.loads(template)
        if not isinstance(value, dict):
            raise ValueError("Headless control projection is not an object")
        with self._enrich_lock:
            local = {
                "schema_version": value.get("schema_version"),
                "message_kind": "delta",
                "control_session_id": value.get("control_session_id"),
                "server_sequence": value.get("server_sequence"),
                "event_class": "cold",
                "control": None,
                "shell": self._local_shell(complete=False),
            }
            encoded = json.dumps(
                local,
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
        if len(encoded.encode("utf-8")) > CONTROL_MAXIMUM_SNAPSHOT_BYTES:
            raise ValueError("Composer local control projection is oversized")
        return encoded

async def serve_headless_state(
    websocket: WebSocket,
    source: HeadlessStateSource,
) -> None:
    """Proxy one ordered control session without polling or semantic reduction."""

    try:
        local_subscription = await source.acquire()
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
            downstream_sequence = 0
            latest_state_payload: str | None = None

            def downstream(payload: str) -> str:
                nonlocal downstream_sequence
                if _SERVER_SEQUENCE.search(payload) is None:
                    return payload
                if downstream_sequence >= MAXIMUM_U64:
                    raise ValueError("Composer control sequence requires renewal")
                downstream_sequence += 1
                return _SERVER_SEQUENCE.sub(
                    rf"\g<1>{downstream_sequence}\g<3>",
                    payload,
                    count=1,
                )

            async def browser_to_owner() -> None:
                while True:
                    message = await websocket.receive_text()
                    if len(message.encode("utf-8")) > CONTROL_MAXIMUM_MESSAGE_BYTES:
                        raise ValueError("control client message is oversized")
                    # Composer must check its shared Draft before a new Run or
                    # motion start. Those commands use the admitted HTTP path;
                    # forwarding one here would bypass that check. Stop and
                    # the observation session stay on the priority WS lane.
                    try:
                        frame = json.loads(message)
                    except json.JSONDecodeError:
                        frame = None
                    if isinstance(frame, dict) and frame.get("message_kind") == "command":
                        command = frame.get("command")
                        envelope = (
                            command.get("command_envelope")
                            if isinstance(command, dict) else None
                        )
                        if (
                            isinstance(envelope, dict)
                            and envelope.get("command_kind") in _HTTP_ADMITTED_COMMANDS
                        ):
                            await websocket.close(
                                code=1008,
                                reason="this command requires Composer admission",
                            )
                            raise WebSocketDisconnect()
                    await upstream.send(message)

            async def owner_to_browser() -> None:
                nonlocal latest_state_payload
                iterator = upstream.__aiter__()
                owner_next = asyncio.create_task(iterator.__anext__())
                local_next = asyncio.create_task(local_subscription.wake.wait())
                try:
                    while True:
                        done, _pending = await asyncio.wait(
                            {owner_next, local_next},
                            return_when=asyncio.FIRST_COMPLETED,
                        )
                        if owner_next in done:
                            try:
                                message = owner_next.result()
                            except StopAsyncIteration:
                                return
                            if not isinstance(message, str):
                                raise ValueError("Headless control frame is not text")
                            is_state = _STATE_MESSAGE.search(message) is not None
                            # Only the initial snapshot needs Composer-local
                            # enrichment. Owner deltas and control messages stay
                            # byte-for-byte transparent on the hot relay.
                            payload = (
                                await asyncio.to_thread(source.enrich, message)
                                if _SNAPSHOT_MESSAGE.search(message) is not None
                                else message
                            )
                            if is_state:
                                latest_state_payload = payload
                            await asyncio.wait_for(
                                websocket.send_text(downstream(payload)),
                                timeout=_SEND_TIMEOUT_SECONDS,
                            )
                            owner_next = asyncio.create_task(iterator.__anext__())
                        if local_next in done:
                            local_subscription.wake.clear()
                            if latest_state_payload is not None:
                                payload = source.local_delta(latest_state_payload)
                                await asyncio.wait_for(
                                    websocket.send_text(downstream(payload)),
                                    timeout=_SEND_TIMEOUT_SECONDS,
                                )
                                latest_state_payload = payload
                            local_next = asyncio.create_task(
                                local_subscription.wake.wait()
                            )
                finally:
                    owner_next.cancel()
                    local_next.cancel()
                    await asyncio.gather(
                        owner_next,
                        local_next,
                        return_exceptions=True,
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
        await source.release(local_subscription)
        with suppress(RuntimeError, WebSocketDisconnect):
            await websocket.close(code=1000)


__all__ = ["HeadlessStateSource", "serve_headless_state"]
