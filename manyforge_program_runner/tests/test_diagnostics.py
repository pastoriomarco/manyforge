from datetime import datetime, timezone
from threading import Event, Semaphore, Thread
import time

import pytest

from manyforge_program_runner.diagnostics import (
    ProgramDiagnosticPublisher,
    ProgramNodeAttemptPublisher,
    program_run_event,
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
        "schema_version": "manyforge.program_node_attempt_batch.v2",
        "application_start_id": "0123456789abcdef",
        "attempts": [{"node_attempt_id": 0}],
        "run_events": [],
        "lost_since_last_batch": None,
    }
    # The 488 dropped while the first send was in flight go with the next.
    assert batches[1]["lost_since_last_batch"]["count"] == 488
    assert [batch["lost_since_last_batch"] for batch in batches[2:]] == [None] * 7
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


def test_run_events_share_the_node_attempt_queue_batch_and_loss_count():
    """OD-381 P2/P3: run events travel in the same bounded queue and batch
    as node attempts, in their own list; a lost one is counted the same way."""
    entered, release, drained = Event(), Event(), Event()
    batches = []

    def send(batch):
        entered.set()
        assert release.wait(5)
        batches.append(batch)
        if len(batches) == 2:
            drained.set()
        return {"recorded": len(batch["attempts"]) + len(batch["run_events"])}

    publisher = ProgramNodeAttemptPublisher(send, "0123456789abcdef", capacity=3)
    try:
        publisher.submit({"node_attempt_id": 0})
        assert entered.wait(5)
        publisher.submit_run_event({"event": "program_run_started"})
        publisher.submit({"node_attempt_id": 1})
        publisher.submit_run_event({"event": "cycle_recovery_action_applied"})
        publisher.submit_run_event({"event": "program_run_finished"})  # Full.
        assert publisher.health()["lost_count"] == "1"
        release.set()
        assert drained.wait(5)
    finally:
        release.set()
        publisher.close()
    assert batches[1] == {
        "schema_version": "manyforge.program_node_attempt_batch.v2",
        "application_start_id": "0123456789abcdef",
        "attempts": [{"node_attempt_id": 1}],
        "run_events": [
            {"event": "program_run_started"},
            {"event": "cycle_recovery_action_applied"},
        ],
        "lost_since_last_batch": batches[1]["lost_since_last_batch"],
    }
    assert batches[1]["lost_since_last_batch"]["count"] == 1
    assert publisher.health()["sent_count"] == "4"


def test_losses_since_the_last_delivered_batch_go_with_the_next_batch():
    """OD-382 P1c: the count and UTC span of the entries lost since the last
    delivered batch ride in the next one; a failed batch hands its own
    count and the one it carried on to the batch after it."""
    answers = iter([{"recorded": 1}, OSError("headless unavailable"), {"recorded": 1},
                    {"recorded": 1}])
    entered, release = Event(), Event()
    sends = Semaphore(0)
    batches = []

    def send(batch):
        batches.append(batch)
        entered.set()
        assert release.wait(5)
        sends.release()
        answer = next(answers)
        if isinstance(answer, Exception):
            raise answer
        return answer

    before = datetime.now(timezone.utc)
    publisher = ProgramNodeAttemptPublisher(send, "0123456789abcdef", capacity=1)
    try:
        publisher.submit({"node_attempt_id": 1})
        assert entered.wait(5)
        publisher.submit({"node_attempt_id": 2})  # Queued.
        publisher.submit({"node_attempt_id": 3})  # Full: lost.
        publisher.submit({"node_attempt_id": 4})  # Full: lost.
        release.set()
        assert sends.acquire(timeout=5) and sends.acquire(timeout=5)  # 1, then 2 fails.
        publisher.submit({"node_attempt_id": 5})
        assert sends.acquire(timeout=5)
        publisher.submit({"node_attempt_id": 6})
        assert sends.acquire(timeout=5)
    finally:
        release.set()
        publisher.close()
    losses = [batch["lost_since_last_batch"] for batch in batches]
    assert losses[0] is None and losses[3] is None
    assert losses[1]["count"] == 2
    first = datetime.fromisoformat(losses[1]["first_lost_wall"])
    last = datetime.fromisoformat(losses[1]["last_lost_wall"])
    assert before <= first <= last <= datetime.now(timezone.utc)
    # Batch 2 failed: its own entry and the two it carried go with batch 3.
    assert losses[2]["count"] == 3
    assert losses[2]["first_lost_wall"] == losses[1]["first_lost_wall"]
    assert datetime.fromisoformat(losses[2]["last_lost_wall"]) >= last
    assert publisher.health()["lost_count"] == "3"


def test_program_run_event_is_stamped_at_production():
    before_ns = time.monotonic_ns()
    before_wall = datetime.now(timezone.utc)
    event = program_run_event(7, "program_run_finished", "failed", {"run_revision": 3},
                              {"code": "program_tree_failed", "message": "x"})
    assert before_ns <= int(event["occurred_at_ns"]) <= time.monotonic_ns()
    assert before_wall <= datetime.fromisoformat(event["occurred_at_wall"]) <= datetime.now(
        timezone.utc)
    assert {key: event[key] for key in ("program_run_id", "event", "outcome", "error", "facts")} == {
        "program_run_id": 7, "event": "program_run_finished", "outcome": "failed",
        "error": {"code": "program_tree_failed", "message": "x"},
        "facts": {"run_revision": 3},
    }
