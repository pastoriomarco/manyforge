from threading import Event

import pytest

from manyforge_program_runner.diagnostics import ProgramDiagnosticPublisher


def test_slow_historian_never_blocks_producer_or_accumulates_unbounded_work():
    entered, release, drained = Event(), Event(), Event()
    records = []

    def send(record):
        entered.set()
        assert release.wait(2)
        records.append(record)
        if len(records) == 2:
            drained.set()
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send, capacity=1)
    try:
        assert publisher._thread is None
        publisher.submit({"state": "failed"})
        assert entered.wait(2)
        publisher.submit({"state": "succeeded"})
        for _ in range(1000):
            publisher.submit({"state": "failed"})
        assert publisher.health()["enqueued_count"] == 1
        assert publisher.health()["lost_count"] == "1000"
        release.set()
        assert drained.wait(2)
    finally:
        release.set()
        publisher.close()
    assert len(records) == 2
    assert records[-1]["state"] == "failed"
    assert records[-1]["delivery_loss_count"] == 1000


@pytest.mark.parametrize("important_state", ["failed", "intervention_required"])
def test_important_capture_evicts_oldest_routine_without_reordering_survivors(
    important_state,
):
    entered, release, drained = Event(), Event(), Event()
    records = []

    def send(record):
        entered.set()
        assert release.wait(2)
        records.append(record)
        if len(records) == 4:
            drained.set()
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send, capacity=3)
    try:
        publisher.submit({"state": "succeeded", "label": "in_flight"})
        assert entered.wait(2)
        publisher.submit({"state": "failed", "label": "protected"})
        publisher.submit({"state": "succeeded", "label": "old_routine"})
        publisher.submit({"state": "succeeded", "label": "new_routine"})
        publisher.submit({"state": important_state, "label": "incoming"})
        assert publisher.health()["enqueued_count"] == 3
        assert publisher.health()["lost_count"] == "1"
        release.set()
        assert drained.wait(2)
    finally:
        release.set()
        publisher.close()
    assert [record["label"] for record in records] == [
        "in_flight",
        "protected",
        "new_routine",
        "incoming",
    ]
    assert records[-1]["delivery_loss_count"] == 1


@pytest.mark.parametrize("important_state", ["failed", "intervention_required"])
def test_routine_flood_cannot_displace_pending_important_capture(important_state):
    entered, release, drained = Event(), Event(), Event()
    records = []

    def send(record):
        entered.set()
        assert release.wait(2)
        records.append(record)
        if len(records) == 2:
            drained.set()
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send, capacity=1)
    try:
        publisher.submit({"state": "succeeded"})
        assert entered.wait(2)
        publisher.submit({"state": important_state})
        for _ in range(1000):
            publisher.submit({"state": "succeeded"})
        assert publisher.health()["enqueued_count"] == 1
        assert publisher.health()["lost_count"] == "1000"
        release.set()
        assert drained.wait(2)
    finally:
        release.set()
        publisher.close()
    assert records[-1]["state"] == important_state
    assert records[-1]["delivery_loss_count"] == 1000


def test_failed_delivery_is_not_retried_and_next_capture_retains_gap():
    failed, sent = Event(), Event()
    records = []

    def send(record):
        records.append(record)
        if len(records) == 1:
            failed.set()
            raise OSError("headless unavailable")
        sent.set()
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send)
    try:
        publisher.submit({"state": "failed"})
        assert failed.wait(2)
        # A barrier on the existing recorder lock observes the committed loss.
        with publisher._condition:
            publisher._condition.wait_for(lambda: publisher._lost == 1, timeout=2)
        publisher.submit({"state": "succeeded"})
        assert sent.wait(2)
    finally:
        publisher.close()
    assert len(records) == 2
    assert records[1]["delivery_loss_count"] == 1


def test_thread_start_failure_cannot_fail_program_transition(monkeypatch):
    class UnavailableThread:
        def __init__(self, **_kwargs):
            pass

        def start(self):
            raise RuntimeError("thread unavailable")

    monkeypatch.setattr("manyforge_program_runner.diagnostics.Thread", UnavailableThread)
    publisher = ProgramDiagnosticPublisher(lambda _: {"recorded": True})
    publisher.submit({"state": "succeeded"})
    assert publisher.health()["delivery_failed"]
    assert publisher.health()["lost_count"] == "1"
    assert publisher.health()["enqueued_count"] == 0
    publisher.close()
