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


def _message(kind: str, owner: str = "kernel-1", **fields: Any) -> dict[str, Any]:
    return {"api_version": EVENT_STREAM_API_VERSION, "owner_epoch": owner, "type": kind, **fields}


def _event(sequence: int, owner: str = "kernel-1") -> dict[str, Any]:
    return _message("event", owner, event={"sequence": sequence, "record_kind": "operation_terminal"})


def test_private_subscription_uses_command_v3_and_stream_v1(tmp_path) -> None:
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
        with open_event_subscription(PinnedRunRoot(tmp_path), 3, "kernel-1") as subscription:
            assert subscription.receive()["api_version"] == "manyforge.execution_event_stream.v1"
    finally:
        listener.close()
        worker.join(timeout=2)
    assert not worker.is_alive()
    assert received == [{"api_version": "manyforge.execution_command.v3", "command": "subscribe_events",
                         "after_cursor": 3, "owner_epoch": "kernel-1"}]


def test_envelope_is_validated_and_an_error_raises_its_code() -> None:
    assert decode_event_stream_message(rfc8785.dumps(_event(3)))["event"]["sequence"] == 3
    for bad in (
        _message("event", event={"sequence": -1}),
        _message("snapshot", state={}),  # No cursor.
        _message("heartbeat", cursor=1, owner_epoch=""),
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
        self.subscriptions: list[tuple[int, str | None]] = []

    @contextmanager
    def subscribe(self, after_cursor: int, owner_epoch: str | None):
        self.subscriptions.append((after_cursor, owner_epoch))
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
    assert host.subscriptions == [(0, None), (2, "kernel-1")]
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
    assert host.subscriptions[1] == (1, "kernel-1")


def test_gap_keeps_the_old_owner_until_its_snapshot() -> None:
    host = _Host(
        [_message("gap", "kernel-2", reason="owner_epoch_changed"),
         HeadlessApiError("execution_owner_unavailable", "closed")],
        [],
    )
    follow_execution_events(host.subscribe, lambda message: None, stop := Event(),
                            backoff_seconds=(0.001, 0.002))
    assert host.subscriptions[1] == (0, None)
    assert stop.is_set()


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


def test_follow_stops_within_one_receive_slice() -> None:
    host, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)

    @contextmanager
    def subscribe(after_cursor: int, owner_epoch: str | None):
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

_EPOCH = "execution-host.epoch.pump"


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
    return _message("snapshot", _EPOCH, cursor=0, reason="subscribed", state=_host_state(ready=ready))


def _host_row(sequence: int) -> dict[str, Any]:
    return _message("event", _EPOCH, event=_row(_EPOCH, sequence))


def test_streamed_source_answers_identity_and_pages_from_its_buffer() -> None:
    source = StreamedExecutionEvents(lambda cursor, epoch: None, Event())
    with pytest.raises(HeadlessApiError) as error:
        source.identity()
    assert error.value.code == "execution_owner_unavailable"
    source._on_message(_host_snapshot(ready=False))  # noqa: SLF001
    assert ExecutionEventPump._idle_owner_epoch(source.identity()) == _EPOCH  # noqa: SLF001
    source._on_message(_message("state", _EPOCH, state=_host_state()))  # noqa: SLF001
    assert source.identity()["identity"]["owner_epoch"] == _EPOCH
    for sequence in (1, 2, 3):
        source._on_message(_host_row(sequence))  # noqa: SLF001
    first = source.events(_EPOCH, 0, limit=2)
    assert [event["sequence"] for event in first["events"]] == [1, 2] and first["has_more"]
    # Not consumed until a later cursor passes it: a failed ingestion re-reads.
    assert [event["sequence"] for event in source.events(_EPOCH, 0, limit=2)["events"]] == [1, 2]
    rows = [_progress_row(_EPOCH, 0.4)]
    source._on_message(_message("progress", _EPOCH, operation_progress=rows))  # noqa: SLF001
    last = source.events(_EPOCH, 2, limit=2)
    assert [event["sequence"] for event in last["events"]] == [3] and not last["has_more"]
    assert last["next_after_sequence"] == 3 and last["operation_progress"] == rows
    source._on_disconnect(RuntimeError("dead link"))  # noqa: SLF001
    with pytest.raises(HeadlessApiError):
        source.events(_EPOCH, 3, limit=2)


def test_streamed_source_wait_returns_on_the_next_message() -> None:
    source = StreamedExecutionEvents(lambda cursor, epoch: None, Event())
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
        self.subscriptions: list[tuple[int, str | None]] = []

    @contextmanager
    def subscribe_events(self, after_cursor: int, owner_epoch: str | None):
        self.subscriptions.append((after_cursor, owner_epoch))
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
    rows = [_progress_row(_EPOCH, 0.3)]
    execution = _SubscribedExecution(
        _host_snapshot(), _host_row(1), _host_row(2),
        _message("progress", _EPOCH, operation_progress=rows),
    )
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
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
    assert core.completed_calls[-1]["through_source_sequence"] == 2
    assert samples and samples[-1] == rows
    assert execution.subscriptions == [(0, None)]
