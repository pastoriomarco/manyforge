from threading import Event, Semaphore, Thread

import pytest

from manyforge_program_runner.diagnostics import (
    ProgramDiagnosticPublisher,
    ProgramNodeAttemptPublisher,
)


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
@pytest.mark.parametrize("routine_state", ["succeeded", "held"])
def test_important_capture_evicts_oldest_routine_without_reordering_survivors(
    important_state, routine_state,
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
        publisher.submit({"state": routine_state, "label": "old_routine"})
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


def test_close_waits_for_the_in_flight_delivery_and_counts_pending_as_lost():
    """OD-309: close joins without a limit; the delivery's end ends the wait."""
    entered, answer = Event(), Event()
    records = []

    def send(record):
        entered.set()
        answer.wait()
        records.append(record)
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send, capacity=2)
    publisher.submit({"state": "failed", "label": "in_flight"})
    assert entered.wait(5)
    publisher.submit({"state": "succeeded", "label": "pending"})
    closer = Thread(target=publisher.close, name="test-diagnostics-close")
    closer.start()
    with publisher._condition:
        assert publisher._condition.wait_for(lambda: publisher._closed, timeout=5)
    # Held by the in-flight delivery alone; the pending one is dropped.
    assert closer.is_alive()
    assert publisher.health()["lost_count"] == "1"
    answer.set()
    closer.join(5)
    assert not closer.is_alive()
    assert [record["label"] for record in records] == ["in_flight"]
    assert not publisher._thread.is_alive()


def test_node_attempts_batch_bounded_and_never_block_the_producer():
    """OD-375: submit never waits on a send in flight; the queue holds 512,
    a POST carries at most 64, and the overflow is dropped and counted."""
    entered, release, drained = Event(), Event(), Event()
    batches = []

    def send(batch):
        entered.set()
        assert release.wait(5)
        batches.append(batch)
        if sum(len(sent["attempts"]) for sent in batches) == 1 + 512:
            drained.set()
        return {"recorded": len(batch["attempts"])}

    publisher = ProgramNodeAttemptPublisher(send, "0123456789abcdef")
    try:
        publisher.submit({"node_attempt_id": 0})
        assert entered.wait(5)
        for attempt_id in range(1, 1001):
            publisher.submit({"node_attempt_id": attempt_id})
        health = publisher.health()
        assert health["enqueued_count"] == 512
        assert health["lost_count"] == "488"
        release.set()
        assert drained.wait(5)
    finally:
        release.set()
        publisher.close()
    assert batches[0] == {
        "schema_version": "manyforge.program_node_attempt_batch.v1",
        "application_start_id": "0123456789abcdef",
        "attempts": [{"node_attempt_id": 0}],
    }
    assert [len(batch["attempts"]) for batch in batches[1:]] == [64] * 8
    assert [fact["node_attempt_id"] for batch in batches for fact in batch["attempts"]] == list(
        range(513)
    )
    assert publisher.health()["sent_count"] == "513"


def test_node_attempts_lost_by_a_failed_or_unanswered_send_are_counted():
    answers = iter([OSError("headless unavailable"), {"ok": True}, {"recorded": 1}])
    sends = Semaphore(0)

    def send(batch):
        sends.release()
        answer = next(answers)
        if isinstance(answer, Exception):
            raise answer
        return answer

    publisher = ProgramNodeAttemptPublisher(send, "0123456789abcdef")
    try:
        for _ in range(3):
            publisher.submit({"node_attempt_id": 1})
            assert sends.acquire(timeout=5)  # One batch per submit.
    finally:
        publisher.close()  # Joins the last send.
    health = publisher.health()
    assert (health["lost_count"], health["sent_count"]) == ("2", "1")


def test_node_attempts_close_drops_pending_and_waits_for_the_send_in_flight():
    entered, answer = Event(), Event()
    batches = []

    def send(batch):
        entered.set()
        answer.wait()
        batches.append(batch)
        return {"recorded": len(batch["attempts"])}

    publisher = ProgramNodeAttemptPublisher(send, "0123456789abcdef")
    publisher.submit({"label": "in_flight"})
    assert entered.wait(5)
    publisher.submit({"label": "pending"})
    closer = Thread(target=publisher.close, name="test-node-attempts-close")
    closer.start()
    with publisher._condition:
        assert publisher._condition.wait_for(lambda: publisher._closed, timeout=5)
    assert closer.is_alive()
    publisher.submit({"label": "after_close"})
    assert publisher.health()["lost_count"] == "2"
    answer.set()
    closer.join(5)
    assert not closer.is_alive()
    assert [batch["attempts"] for batch in batches] == [[{"label": "in_flight"}]]
