"""Client of the execution host's event subscription stream (contract §8.1).

One SEQPACKET connection to ``execution-events.sock`` carries one
subscription: the client sends ``subscribe_events {after_cursor,
owner_epoch}`` and then receives one message per packet: ``snapshot`` (first,
and after a ``gap``; events continue strictly after its ``cursor``),
``event`` (one journal row as in an ``events`` page), ``state`` (the
``events`` reply facts without page and progress, when they change),
``progress`` (the reply's ``operation_progress`` rows, when they change),
``heartbeat`` (every second, with the cursor sent so far), ``gap`` or
``error``. ``follow_execution_events`` keeps one subscription alive: it
resumes from the last cursor it delivered, treats three seconds without any
message as a dead link and reconnects with backoff.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
import logging
from pathlib import Path
import select
import socket
from threading import Event
import time
from typing import Any, Final

import rfc8785

from .native import HeadlessApiError, strict_json_object
from .private_transport import PinnedRunRoot, PrivateTransportCapabilityError

EXECUTION_EVENTS_SOCKET_FILE: Final = "execution-events.sock"
EVENT_STREAM_API_VERSION: Final = "manyforge.execution_event_stream.v1"
EVENT_STREAM_MESSAGE_TYPES: Final = frozenset(
    {"snapshot", "event", "state", "progress", "heartbeat", "gap"}
)
# Heartbeats arrive every second; three missed ones are a dead link.
DEAD_LINK_SECONDS: Final = 3.0
RECONNECT_BACKOFF_SECONDS: Final = (0.1, 2.0)
MAX_EVENT_STREAM_MESSAGE_BYTES: Final = 4 * 1024 * 1024
_MAX_SAFE: Final = 9_007_199_254_740_991
# A stop is seen within one receive slice.
_RECEIVE_SLICE_SECONDS: Final = 0.25

_LOGGER = logging.getLogger(__name__)


def _counter(value: Any) -> int | None:
    return value if type(value) is int and 0 <= value <= _MAX_SAFE else None


def decode_event_stream_message(payload: bytes) -> dict[str, Any]:
    """One validated message envelope; an ``error`` raises its code."""

    try:
        message = strict_json_object(
            payload.decode("utf-8"),
            label="event stream message",
            maximum_bytes=MAX_EVENT_STREAM_MESSAGE_BYTES,
        )
    except (UnicodeDecodeError, ValueError) as error:
        raise HeadlessApiError(
            "execution_event_stream_invalid", "event stream message is not JSON"
        ) from error
    if message.get("api_version") != EVENT_STREAM_API_VERSION:
        raise HeadlessApiError(
            "execution_event_stream_invalid", "event stream message has another version"
        )
    kind = message.get("type")
    if kind == "error":
        error = message.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        text = error.get("message") if isinstance(error, dict) else None
        raise HeadlessApiError(
            code if isinstance(code, str) and code else "execution_event_stream_failed",
            text if isinstance(text, str) else "event stream ended",
        )
    owner_epoch = message.get("owner_epoch")
    if (
        kind not in EVENT_STREAM_MESSAGE_TYPES
        or not isinstance(owner_epoch, str)
        or not owner_epoch
        or (kind in ("snapshot", "heartbeat") and _counter(message.get("cursor")) is None)
        or (kind in ("snapshot", "state") and not isinstance(message.get("state"), dict))
        or (kind == "progress" and not isinstance(message.get("operation_progress"), list))
        or (
            kind == "event"
            and (
                not isinstance(message.get("event"), dict)
                or _counter(message["event"].get("sequence")) is None
            )
        )
    ):
        raise HeadlessApiError(
            "execution_event_stream_invalid", "event stream message has an invalid envelope"
        )
    return message


class ExecutionEventSubscription:
    """One open subscription; ``receive`` returns the next message."""

    def __init__(self, client: socket.socket, *, dead_link_seconds: float) -> None:
        self._client = client
        self._dead_link_seconds = dead_link_seconds

    def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
        """The next message; None once ``stop`` is set. Raises on EOF, a dead
        link (no message for ``dead_link_seconds``), or an ``error``."""

        deadline = time.monotonic() + self._dead_link_seconds
        while True:
            if stop is not None and stop.is_set():
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HeadlessApiError(
                    "execution_event_stream_dead",
                    "event stream sent no message or heartbeat in time",
                )
            readable, _write, _error = select.select(
                [self._client], [], [], min(remaining, _RECEIVE_SLICE_SECONDS)
            )
            if readable:
                break
        try:
            payload, _ancillary, flags, _address = self._client.recvmsg(
                MAX_EVENT_STREAM_MESSAGE_BYTES
            )
        except OSError as error:
            raise HeadlessApiError(
                "execution_owner_unavailable", f"event stream failed: {error}"
            ) from error
        if not payload:
            raise HeadlessApiError("execution_owner_unavailable", "event stream closed")
        if flags & socket.MSG_TRUNC:
            raise HeadlessApiError(
                "execution_event_stream_invalid", "event stream message is oversized"
            )
        return decode_event_stream_message(payload)

    def close(self) -> None:
        self._client.close()


@contextmanager
def open_event_subscription(
    run_root: PinnedRunRoot,
    after_cursor: int,
    owner_epoch: str | None,
    *,
    dead_link_seconds: float = DEAD_LINK_SECONDS,
) -> Iterator[ExecutionEventSubscription]:
    """Connect and subscribe strictly after ``after_cursor`` of
    ``owner_epoch`` (None: the current owner)."""

    if _counter(after_cursor) is None or (
        owner_epoch is not None and (not owner_epoch or len(owner_epoch.encode()) > 2_048)
    ):
        raise ValueError("event subscription cursor exceeds its fixed bounds")
    request = rfc8785.dumps(
        {
            "after_cursor": after_cursor,
            "api_version": "manyforge.execution_command.v1",
            "command": "subscribe_events",
            "owner_epoch": owner_epoch,
        }
    )
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, MAX_EVENT_STREAM_MESSAGE_BYTES)
        client.settimeout(DEAD_LINK_SECONDS)
        with run_root.socket_address(EXECUTION_EVENTS_SOCKET_FILE) as address:
            client.connect(address)
        if client.send(request) != len(request):
            raise HeadlessApiError(
                "execution_command_transport_failure", "subscribe packet was truncated"
            )
    except PrivateTransportCapabilityError as error:
        client.close()
        raise HeadlessApiError(
            "execution_owner_unavailable", f"event stream socket is unavailable: {error}"
        ) from error
    except OSError as error:
        client.close()
        raise HeadlessApiError(
            "execution_owner_unavailable", f"event stream connect failed: {error}"
        ) from error
    except BaseException:
        client.close()
        raise
    subscription = ExecutionEventSubscription(client, dead_link_seconds=dead_link_seconds)
    try:
        yield subscription
    finally:
        subscription.close()


Subscribe = Callable[[int, "str | None"], Any]


def follow_execution_events(
    subscribe: Subscribe,
    on_message: Callable[[Mapping[str, Any]], None],
    stop: Event,
    *,
    on_disconnect: Callable[[BaseException], None] | None = None,
    backoff_seconds: tuple[float, float] = RECONNECT_BACKOFF_SECONDS,
) -> None:
    """Keep one subscription until ``stop``: deliver each message in order,
    resume from the last delivered cursor and owner epoch after any failure
    (connect, dead link, EOF, error message, or ``on_message`` raising), with
    backoff doubling up to its bound. ``subscribe(after_cursor,
    owner_epoch)`` returns a context manager yielding a subscription."""

    cursor, owner_epoch = 0, None
    delay = backoff_seconds[0]
    while not stop.is_set():
        try:
            with subscribe(cursor, owner_epoch) as subscription:
                while True:
                    message = subscription.receive(stop)
                    if message is None:
                        return
                    on_message(message)
                    # Advance only after the message was applied.
                    kind = message["type"]
                    if kind != "gap":  # A gap's snapshot sets the new owner.
                        owner_epoch = message["owner_epoch"]
                    if kind in ("snapshot", "heartbeat"):
                        cursor = message["cursor"]
                    elif kind == "event":
                        cursor = message["event"]["sequence"]
                    delay = backoff_seconds[0]
        except Exception as error:
            _LOGGER.warning("execution event stream reconnects: %s", error)
            if on_disconnect is not None:
                try:
                    on_disconnect(error)
                except Exception:
                    _LOGGER.exception("event stream disconnect handler failed")
        if stop.wait(delay):
            return
        delay = min(delay * 2, backoff_seconds[1])


def run_root_subscriber(run_root: Path | PinnedRunRoot) -> Subscribe:
    """A ``subscribe`` callable for ``follow_execution_events``."""

    pinned = run_root if isinstance(run_root, PinnedRunRoot) else PinnedRunRoot(run_root)
    return lambda after_cursor, owner_epoch: open_event_subscription(
        pinned, after_cursor, owner_epoch
    )


__all__ = [
    "DEAD_LINK_SECONDS",
    "EVENT_STREAM_API_VERSION",
    "EXECUTION_EVENTS_SOCKET_FILE",
    "ExecutionEventSubscription",
    "decode_event_stream_message",
    "follow_execution_events",
    "open_event_subscription",
    "run_root_subscriber",
]
