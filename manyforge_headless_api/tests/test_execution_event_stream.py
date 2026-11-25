"""The event pump's source on the core's event subscription: identity and
pages from the delivered messages, the wait, and ingestion from the
subscription only. The follower itself (resume, dead link, reconnect) is the
core's (``manyforge_headless/test/test_gateway_events.cpp``)."""

from __future__ import annotations

from contextlib import contextmanager
from threading import Event, Thread
import time
from typing import Any

import pytest

from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.paged_event_stream import (
    EVENT_STREAM_API_VERSION,
    FollowedEventSource,
    _Subscription,
)


def _message(kind: str, **fields: Any) -> dict[str, Any]:
    return {"api_version": EVENT_STREAM_API_VERSION, "type": kind, **fields}


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
    subscription = _Subscription()
    source = StreamedExecutionEvents(subscription, _APPLICATION_START_ID)
    with pytest.raises(HeadlessApiError) as error:
        source.identity()
    assert error.value.code == "execution_owner_unavailable"
    subscription.push(_host_snapshot(ready=False))
    assert (ExecutionEventPump._idle_application_start_id(source.identity())  # noqa: SLF001
            == _APPLICATION_START_ID)
    subscription.push(_message("state", state=_host_state()))
    assert source.identity() == {
        "ok": True, "identity": {"application_start_id": _APPLICATION_START_ID}}
    for sequence in (1, 2, 3):
        subscription.push(_host_row(sequence))
    first = source.events(0, limit=2)
    assert [event["sequence"] for event in first["events"]] == [1, 2] and first["has_more"]
    # Not consumed until a later cursor passes it: a failed ingestion re-reads.
    assert [event["sequence"] for event in source.events(0, limit=2)["events"]] == [1, 2]
    rows = [_progress_row(0.4)]
    subscription.push(_message("progress", operation_progress=rows))
    last = source.events(2, limit=2)
    assert [event["sequence"] for event in last["events"]] == [3] and not last["has_more"]
    assert last["next_after_sequence"] == 3 and last["operation_progress"] == rows
    assert last["runtime_faults"] == _host_state()["runtime_faults"]
    subscription.push({"type": "disconnected", "code": "execution_event_stream_dead",
                       "message": "dead link"})
    with pytest.raises(HeadlessApiError):
        source.events(3, limit=2)
    # The next snapshot reconnects the source.
    subscription.push(_host_snapshot())
    assert source.events(3, limit=2)["events"] == []


def test_streamed_source_buffer_is_bounded_and_loses_nothing() -> None:
    # A full buffer takes no more messages: they stay in the core
    # subscription (which resumes on its own connection when it fills).
    subscription = _Subscription()
    source = StreamedExecutionEvents(subscription, _APPLICATION_START_ID)
    bound = StreamedExecutionEvents.MAX_BUFFERED_EVENTS
    subscription.push(_host_snapshot())
    for sequence in range(1, bound + 11):
        subscription.push(_host_row(sequence))
    page = source.events(0, limit=16)
    assert page["has_more"] and len(source._events) == bound  # noqa: SLF001
    assert len(subscription._queue) == 10  # noqa: SLF001
    seen: list[int] = []
    after = 0
    while True:
        page = source.events(after, limit=16)
        seen.extend(event["sequence"] for event in page["events"])
        after = page["next_after_sequence"]
        if not page["has_more"]:
            break
    assert seen == list(range(1, bound + 11))
    assert len(source._events) <= bound  # noqa: SLF001


def test_streamed_source_drops_signal_sample_age_only() -> None:
    subscription = _Subscription()
    source = StreamedExecutionEvents(subscription, _APPLICATION_START_ID)
    sample = {"signal_id": "door", "value": True, "age_ns": 7}
    subscription.push(_message("snapshot", cursor=0, reason="subscribed",
                               state={**_host_state(), "signal_samples": [sample]}))
    model = source.events(0)["component_model"]
    assert model["signal_samples"] == [{"signal_id": "door", "value": True}]
    assert model["component_states"] is None


def test_streamed_source_wait_returns_on_the_next_message() -> None:
    subscription = _Subscription()
    source = StreamedExecutionEvents(subscription, _APPLICATION_START_ID)
    subscription.push(_host_snapshot())
    source.wait(0.0)  # Takes the snapshot.
    Thread(target=lambda: (time.sleep(0.05), subscription.push(_host_row(1)))).start()
    started = time.monotonic()
    source.wait(2.0)
    assert time.monotonic() - started < 1.0
    # A closed subscription ends a wait at once.
    source.close()
    started = time.monotonic()
    source.wait(2.0)
    assert time.monotonic() - started < 1.0


class _SubscribedExecution(FollowedEventSource):
    """An event source with only the scripted subscription: an ``identity``
    or ``events`` poll would fail with AttributeError."""

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
