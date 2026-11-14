"""Event subscription stream client: envelope, apply order,
resume from cursor, dead-link detection, reconnect and stop."""

from __future__ import annotations

from contextlib import contextmanager
import socket
import json
from threading import Event, Thread
import time
from typing import Any

import pytest
import rfc8785

from manyforge_headless_api.execution_event_stream import (
    EVENT_STREAM_API_VERSION,
    ExecutionEventSubscription,
    decode_event_stream_message,
    follow_execution_events,
    open_event_subscription,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.private_transport import PinnedRunRoot


def _message(kind: str, **fields: Any) -> dict[str, Any]:
    return {"api_version": EVENT_STREAM_API_VERSION, "type": kind, **fields}


def _event(sequence: int) -> dict[str, Any]:
    return _message("event", event={"sequence": sequence, "record_kind": "operation_terminal"})


def test_private_subscription_uses_command_v4_and_stream_v2(tmp_path) -> None:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    endpoint = tmp_path / "execution-events.sock"
    listener.bind(str(endpoint))
    endpoint.chmod(0o600)
    listener.listen(1)
    received: list[dict[str, Any]] = []
    def serve() -> None:
        connection, _ = listener.accept()
        with connection:
            received.append(json.loads(connection.recv(65536)))
            connection.send(rfc8785.dumps(_message("heartbeat", cursor=4)))
    worker = Thread(target=serve)
    worker.start()
    try:
        with open_event_subscription(PinnedRunRoot(tmp_path), 3) as subscription:
            assert subscription.receive()["api_version"] == "manyforge.execution_event_stream.v4"
    finally:
        listener.close()
        worker.join(timeout=2)
    assert not worker.is_alive()
    assert received == [{"api_version": "manyforge.execution_command.v4", "command": "subscribe_events",
                         "after_cursor": 3}]


def test_envelope_is_validated_and_an_error_raises_its_code() -> None:
    assert decode_event_stream_message(rfc8785.dumps(_event(3)))["event"]["sequence"] == 3
    for bad in (
        _message("event", event={"sequence": -1}),
        _message("snapshot", state={}),  # No cursor.
        _message("gap", reason="owner_epoch_changed"),  # No longer a stream message.
        _message("unknown"),
        {**_event(1), "api_version": "other"},
    ):
        with pytest.raises(HeadlessApiError) as error:
            decode_event_stream_message(rfc8785.dumps(bad))
        assert error.value.code == "execution_event_stream_invalid"
    with pytest.raises(HeadlessApiError) as error:
        decode_event_stream_message(rfc8785.dumps({
            "api_version": EVENT_STREAM_API_VERSION, "type": "error",
            "error": {"code": "execution_event_subscribers_full", "message": "full", "details": {}},
        }))
    assert error.value.code == "execution_event_subscribers_full"


class _Host:
    """Scripted subscriptions: each connection plays one script; a script
    ending in an exception drops the link."""

    def __init__(self, *scripts: list[Any]) -> None:
        self.scripts = list(scripts)
        self.subscriptions: list[int] = []

    @contextmanager
    def subscribe(self, after_cursor: int):
        self.subscriptions.append(after_cursor)
        script = self.scripts.pop(0) if self.scripts else None
        if script is None:
            raise HeadlessApiError("execution_owner_unavailable", "no host")

        class _Subscription:
            def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
                if not script:
                    if stop is not None:
                        stop.set()
                    return None
                item = script.pop(0)
                if isinstance(item, Exception):
                    raise item
                return item

        yield _Subscription()


def test_follow_applies_in_order_and_resumes_from_the_last_applied_cursor() -> None:
    snapshot = _message("snapshot", cursor=0, reason="subscribed", state={})
    host = _Host(
        [snapshot, _event(1), _event(2), HeadlessApiError("execution_event_stream_dead", "x")],
        [_message("snapshot", cursor=2, reason="subscribed", state={}), _event(3),
         _message("heartbeat", cursor=3)],
    )
    applied: list[Any] = []
    lost: list[BaseException] = []
    follow_execution_events(
        host.subscribe, lambda message: applied.append(message.get("event", {}).get("sequence", message["type"])),
        Event(), on_disconnect=lost.append, backoff_seconds=(0.001, 0.002),
    )
    assert applied == ["snapshot", 1, 2, "snapshot", 3, "heartbeat"]
    assert host.subscriptions == [0, 2]
    assert [error.code for error in lost] == ["execution_event_stream_dead"]  # type: ignore[attr-defined]


def test_a_failed_apply_is_redelivered_after_reconnect() -> None:
    host = _Host(
        [_message("snapshot", cursor=0, reason="subscribed", state={}), _event(1), _event(2)],
        [_message("snapshot", cursor=1, reason="subscribed", state={}), _event(2)],
    )
    seen: list[int] = []

    def apply(message: dict[str, Any]) -> None:
        if message["type"] == "event":
            seen.append(message["event"]["sequence"])
            if seen == [1, 2]:
                raise ValueError("consumer failed on 2")

    follow_execution_events(host.subscribe, apply, Event(), backoff_seconds=(0.001, 0.002))
    assert seen == [1, 2, 2]
    assert host.subscriptions[1] == 1


def test_dead_link_is_detected_on_a_silent_socket_and_stop_ends_a_receive() -> None:
    host, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        subscription = ExecutionEventSubscription(client, dead_link_seconds=0.3)
        host.send(rfc8785.dumps(_message("heartbeat", cursor=4)))
        assert subscription.receive()["cursor"] == 4
        started = time.monotonic()
        with pytest.raises(HeadlessApiError) as error:
            subscription.receive()
        assert error.value.code == "execution_event_stream_dead"
        assert 0.25 <= time.monotonic() - started < 1.0
        stop = Event()
        Thread(target=lambda: (time.sleep(0.05), stop.set())).start()
        assert ExecutionEventSubscription(client, dead_link_seconds=5.0).receive(stop) is None
        host.close()
        with pytest.raises(HeadlessApiError) as error:
            subscription.receive()
        assert error.value.code == "execution_owner_unavailable"
    finally:
        client.close()
        host.close()


class _RecordingStop(Event):
    """A stop whose waits return at once; it records each delay and sets
    itself after ``waits`` waits."""

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


def test_reconnect_delays_double_to_two_seconds_and_reset_after_a_delivered_message() -> None:
    # Characterisation (pinned before the move onto the shared follower):
    # the default backoff runs 0.1, 0.2, 0.4, 0.8, 1.6, 2.0, 2.0 s, and a
    # delivered message resets it to 0.1 s.
    failed: list[Any] = [None] * 7
    host = _Host(
        *failed,
        [_message("snapshot", cursor=0, reason="subscribed", state={}),
         HeadlessApiError("execution_event_stream_dead", "x")],
    )
    stop = _RecordingStop(waits=9)
    follow_execution_events(host.subscribe, lambda message: None, stop)
    assert stop.delays == pytest.approx([0.1, 0.2, 0.4, 0.8, 1.6, 2.0, 2.0, 0.1, 0.2])
    assert len(host.subscriptions) == 9


def test_follow_stops_within_one_receive_slice() -> None:
    host, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)

    @contextmanager
    def subscribe(after_cursor: int):
        yield ExecutionEventSubscription(client, dead_link_seconds=5.0)

    stop = Event()
    follower = Thread(target=follow_execution_events, args=(subscribe, lambda m: None, stop))
    follower.start()
    time.sleep(0.05)
    stop.set()
    follower.join(1.0)
    assert not follower.is_alive()
    client.close()
    host.close()


# Headless event pump on the subscription ----------------------

from manyforge_headless_api.execution import (  # noqa: E402
    ExecutionEventPump,
    StreamedExecutionEvents,
)
from manyforge_headless_api.tests.test_execution_events import (  # noqa: E402
    _RecordingCore,
    _event as _row,
    _progress_row,
)

_APPLICATION_START_ID = "3f9a0000000000a1"


def _host_state(*, ready: bool = True) -> dict[str, Any]:
    return {
        "admission_generation": 1,
        "carried_object_last_losses": [],
        "carried_object_lost_signals": [],
        "execution_owner_ready": ready,
        "interrupt_rule_catalog": None,
        "interrupt_rule_state_baseline": None,
        "runtime_faults": {
            "operation_journal_write_fault": None,
            "program_runner_loss": None,
            "kernel_internal_fault": None,
        },
    }


def _host_snapshot(*, ready: bool = True) -> dict[str, Any]:
    return _message("snapshot", cursor=0, reason="subscribed", state=_host_state(ready=ready))


def _host_row(sequence: int) -> dict[str, Any]:
    return _message("event", event=_row(sequence))


def test_streamed_source_answers_identity_and_pages_from_its_buffer() -> None:
    source = StreamedExecutionEvents(lambda cursor: None, Event(), _APPLICATION_START_ID)
    with pytest.raises(HeadlessApiError) as error:
        source.identity()
    assert error.value.code == "execution_owner_unavailable"
    source._on_message(_host_snapshot(ready=False))  # noqa: SLF001
    assert (ExecutionEventPump._idle_application_start_id(source.identity())  # noqa: SLF001
            == _APPLICATION_START_ID)
    source._on_message(_message("state", state=_host_state()))  # noqa: SLF001
    assert source.identity() == {
        "ok": True, "identity": {"application_start_id": _APPLICATION_START_ID}}
    for sequence in (1, 2, 3):
        source._on_message(_host_row(sequence))  # noqa: SLF001
    first = source.events(0, limit=2)
    assert [event["sequence"] for event in first["events"]] == [1, 2] and first["has_more"]
    # Not consumed until a later cursor passes it: a failed ingestion re-reads.
    assert [event["sequence"] for event in source.events(0, limit=2)["events"]] == [1, 2]
    rows = [_progress_row(0.4)]
    source._on_message(_message("progress", operation_progress=rows))  # noqa: SLF001
    last = source.events(2, limit=2)
    assert [event["sequence"] for event in last["events"]] == [3] and not last["has_more"]
    assert last["next_after_sequence"] == 3 and last["operation_progress"] == rows
    source._on_disconnect(RuntimeError("dead link"))  # noqa: SLF001
    with pytest.raises(HeadlessApiError):
        source.events(3, limit=2)


def test_streamed_source_wait_returns_on_the_next_message() -> None:
    source = StreamedExecutionEvents(lambda cursor: None, Event(), _APPLICATION_START_ID)
    source._on_message(_host_snapshot())  # noqa: SLF001
    source.wait(0.0)  # Consume the snapshot's wake.
    Thread(target=lambda: (time.sleep(0.05), source._on_message(_host_row(1)))).start()  # noqa: SLF001
    started = time.monotonic()
    source.wait(2.0)
    assert time.monotonic() - started < 1.0


class _SubscribedExecution:
    """An execution port with only the subscription and the Stop-fault read:
    an ``identity`` or ``events`` poll would fail with AttributeError."""

    def __init__(self, *script: dict[str, Any]) -> None:
        self.script = list(script)
        self.subscriptions: list[int] = []

    @contextmanager
    def subscribe_events(self, after_cursor: int):
        self.subscriptions.append(after_cursor)
        script = self.script

        class _Subscription:
            def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
                while not script:
                    assert stop is not None
                    if stop.wait(0.02):
                        return None
                return script.pop(0)

        yield _Subscription()


def test_event_pump_ingests_rows_and_progress_from_the_subscription_only() -> None:
    core = _RecordingCore()
    rows = [_progress_row(0.3)]
    execution = _SubscribedExecution(
        _host_snapshot(), _host_row(1), _host_row(2),
        _message("progress", operation_progress=rows),
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1,
    )
    samples: list[Any] = []
    pump.set_progress_observer(samples.append)
    pump.start()
    try:
        deadline = time.monotonic() + 3.0
        while (len(core.ingested) < 2 or not samples) and time.monotonic() < deadline:
            time.sleep(0.01)
    finally:
        pump.shutdown()
    assert pump.last_error is None
    assert [row["source_sequence"] for row in core.ingested] == [1, 2]
    assert all("source_owner_epoch" not in row for row in core.ingested)
    assert core.completed_calls[-1]["through_source_sequence"] == 2
    assert samples and samples[-1] == rows
    assert execution.subscriptions == [0]
