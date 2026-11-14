"""Client side shared by ManyForge's private SEQPACKET streams.

One connection carries one subscription; each packet is one frame.
``PrivateStreamSubscription`` receives in short slices so a stop is seen
promptly, and reports a quiet link as ``TIMED_OUT`` for the stream's own
liveness rule. ``follow_private_stream`` keeps one subscription alive:
it reconnects after any failure with a backoff that doubles up to its bound
and starts again from its lower end after every delivered frame. What a
stream subscribes with, how it validates a frame, and what it resumes from
stay with that stream.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from contextlib import AbstractContextManager
import enum
import logging
import select
import socket
from threading import Event
import time
from typing import Any, Final

from .native import HeadlessApiError

# A stop is seen within one receive slice.
RECEIVE_SLICE_SECONDS: Final = 0.25

_LOGGER = logging.getLogger(__name__)


class StreamTimeout(enum.Enum):
    """No frame arrived within the receive's timeout."""

    TIMED_OUT = enum.auto()


TIMED_OUT: Final = StreamTimeout.TIMED_OUT


class PrivateStreamSubscription:
    """One open subscription; ``receive`` returns the next frame."""

    def __init__(
        self,
        client: socket.socket,
        *,
        max_bytes: int,
        decode: Callable[[bytes], dict[str, Any]],
        unavailable_code: str,
        invalid_code: str,
        label: str,
    ) -> None:
        self._client = client
        self._max_bytes = max_bytes
        self._decode = decode
        self._unavailable_code = unavailable_code
        self._invalid_code = invalid_code
        self._label = label

    def receive(
        self, stop: Event | None, timeout_seconds: float
    ) -> dict[str, Any] | StreamTimeout | None:
        """The next decoded frame; None once ``stop`` is set; ``TIMED_OUT``
        when no frame arrived within ``timeout_seconds``. Raises
        ``unavailable_code`` on EOF or a socket error and ``invalid_code`` on
        a frame above ``max_bytes``; ``decode`` raises for an invalid one."""

        deadline = time.monotonic() + timeout_seconds
        while True:
            if stop is not None and stop.is_set():
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return TIMED_OUT
            readable, _write, _error = select.select(
                [self._client], [], [], min(remaining, RECEIVE_SLICE_SECONDS)
            )
            if readable:
                break
        try:
            payload, _ancillary, flags, _address = self._client.recvmsg(self._max_bytes)
        except OSError as error:
            raise HeadlessApiError(
                self._unavailable_code, f"{self._label} failed: {error}"
            ) from error
        if not payload:
            raise HeadlessApiError(self._unavailable_code, f"{self._label} closed")
        if flags & socket.MSG_TRUNC:
            raise HeadlessApiError(
                self._invalid_code, f"{self._label} message is oversized"
            )
        return self._decode(payload)

    def close(self) -> None:
        self._client.close()


def follow_private_stream(
    connect: Callable[[], AbstractContextManager[Any]],
    on_message: Callable[[Mapping[str, Any]], None],
    stop: Event,
    *,
    backoff_seconds: tuple[float, float],
    on_disconnect: Callable[[BaseException], None] | None = None,
    label: str,
    logger: logging.Logger = _LOGGER,
) -> None:
    """Keep one subscription until ``stop``: deliver each frame in order to
    ``on_message``; after any failure (connect, a raising ``receive``, or
    ``on_message`` raising) call ``on_disconnect`` and reconnect after the
    current delay, which doubles up to ``backoff_seconds[1]`` and returns to
    ``backoff_seconds[0]`` after every delivered frame. ``connect()`` returns
    a context manager yielding an object with ``receive(stop)`` (a frame, or
    None once ``stop`` is set)."""

    delay = backoff_seconds[0]
    while not stop.is_set():
        try:
            with connect() as subscription:
                while True:
                    message = subscription.receive(stop)
                    if message is None:
                        return
                    on_message(message)
                    delay = backoff_seconds[0]
        except Exception as error:
            logger.warning("%s reconnects: %s", label, error)
            if on_disconnect is not None:
                try:
                    on_disconnect(error)
                except Exception:
                    logger.exception("%s disconnect handler failed", label)
        if stop.wait(delay):
            return
        delay = min(delay * 2, backoff_seconds[1])


__all__ = [
    "RECEIVE_SLICE_SECONDS",
    "TIMED_OUT",
    "PrivateStreamSubscription",
    "StreamTimeout",
    "follow_private_stream",
]
