"""Shared private stream client: the bound, codes, timeout and backoff a
caller passes are the ones applied."""

from __future__ import annotations

from contextlib import contextmanager
import json
import socket
from threading import Event, Thread
import time
from typing import Any

import pytest

from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.private_stream import (
    TIMED_OUT,
    PrivateStreamSubscription,
    follow_private_stream,
)


def _subscription(client: socket.socket, *, max_bytes: int = 1024) -> PrivateStreamSubscription:
    return PrivateStreamSubscription(
        client,
        max_bytes=max_bytes,
        decode=lambda payload: json.loads(payload),
        unavailable_code="example_unavailable",
        invalid_code="example_invalid",
        label="example stream",
    )


def test_receive_decodes_times_out_and_stops() -> None:
    host, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        subscription = _subscription(client)
        host.send(b'{"n": 1}')
        assert subscription.receive(None, 1.0) == {"n": 1}
        started = time.monotonic()
        assert subscription.receive(None, 0.3) is TIMED_OUT
        assert 0.25 <= time.monotonic() - started < 1.0
        stop = Event()
        Thread(target=lambda: (time.sleep(0.05), stop.set())).start()
        started = time.monotonic()
        assert subscription.receive(stop, 5.0) is None
        assert time.monotonic() - started < 0.5  # Within one slice.
    finally:
        client.close()
        host.close()


def test_receive_uses_the_bound_and_codes_passed_in() -> None:
    host, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        subscription = _subscription(client, max_bytes=8)
        host.send(b'{"long": 1}')
        with pytest.raises(HeadlessApiError) as error:
            subscription.receive(None, 1.0)
        assert error.value.code == "example_invalid"
        host.close()
        with pytest.raises(HeadlessApiError) as error:
            subscription.receive(None, 1.0)
        assert error.value.code == "example_unavailable"
    finally:
        client.close()
        host.close()


class _RecordingStop(Event):
    def __init__(self, waits: int) -> None:
        super().__init__()
        self.delays: list[float] = []
        self._waits = waits

    def wait(self, timeout: float | None = None) -> bool:
        assert timeout is not None
        self.delays.append(timeout)
        if len(self.delays) >= self._waits:
            self.set()
        return self.is_set()


def test_follow_uses_the_backoff_passed_in_and_resets_after_a_frame() -> None:
    scripts: list[list[Any] | None] = [
        None, None, None, None, [{"n": 1}, RuntimeError("drop")], None]
    lost: list[BaseException] = []
    delivered: list[Any] = []

    @contextmanager
    def connect():
        script = scripts.pop(0) if scripts else None
        if script is None:
            raise HeadlessApiError("example_unavailable", "no server")

        class _Subscription:
            def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
                item = script.pop(0)
                if isinstance(item, Exception):
                    raise item
                return item

        yield _Subscription()

    stop = _RecordingStop(waits=6)
    follow_private_stream(
        connect, delivered.append, stop, backoff_seconds=(0.1, 0.5),
        on_disconnect=lost.append, label="example stream",
    )
    assert stop.delays == pytest.approx([0.1, 0.2, 0.4, 0.5, 0.1, 0.2])
    assert delivered == [{"n": 1}]
    assert len(lost) == 6


def test_follow_survives_a_raising_disconnect_handler() -> None:
    attempts: list[int] = []

    @contextmanager
    def connect():
        attempts.append(1)
        raise HeadlessApiError("example_unavailable", "no server")
        yield  # pragma: no cover

    def handler(error: BaseException) -> None:
        raise ValueError("handler failed")

    stop = _RecordingStop(waits=2)
    follow_private_stream(
        connect, lambda message: None, stop, backoff_seconds=(0.1, 0.5),
        on_disconnect=handler, label="example stream",
    )
    assert len(attempts) == 2
