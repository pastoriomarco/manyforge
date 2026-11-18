"""OD-215: the disk check stops operation while the disk cannot keep records.

Free space comes from an injected reader; no disk is filled.
"""

from __future__ import annotations

from pathlib import Path
import threading
import time
from typing import Any

import pytest

from manyforge_headless_api.disk_check import (
    EVENT_HISTORY,
    OPERATION_JOURNAL,
    DiskRecordsCheck,
    RecordFolder,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_operator.run_session import DiskReading

FLOOR = 10_000_000_000
MARGIN = 1_000_000_000
JOURNAL = Path("/state/current")
HISTORY = Path("/state/history")


class Harness:
    def __init__(self, *, same_device: bool = True) -> None:
        self.now = 0.0
        self.free = {JOURNAL: 50_000_000_000, HISTORY: 50_000_000_000}
        self.inodes: dict[Path, int | None] = {JOURNAL: 1000, HISTORY: 1000}
        self.devices = {JOURNAL: 1, HISTORY: 1 if same_device else 2}
        self.history = {"queue_full": False, "storage_full": False}
        self.journal = (False, False)
        self.calls: list[tuple[str, Any]] = []
        self.reads = 0
        self.halt_response: dict[str, Any] | Exception = {"ok": True, "accepted": {"event_id": 1}}
        self.check = DiskRecordsCheck(
            folders=(RecordFolder(OPERATION_JOURNAL, JOURNAL), RecordFolder(EVENT_HISTORY, HISTORY)),
            floor_bytes=FLOOR,
            margin_bytes=MARGIN,
            history_signals=lambda: dict(self.history),
            journal_signals=lambda: self.journal,
            halt=self.halt,
            observe_condition=self.observe,
            reader=self.read,
            clock=lambda: self.now,
        )

    def read(self, path: Path) -> tuple[int, DiskReading]:
        self.reads += 1
        return self.devices[path], DiskReading(10**12, self.free[path], self.inodes[path])

    def halt(self) -> dict[str, Any]:
        self.calls.append(("halt", None))
        if isinstance(self.halt_response, Exception):
            raise self.halt_response
        return self.halt_response

    def observe(self, condition: Any) -> dict[str, Any]:
        self.calls.append(("alarm", condition))
        return {"changed": True}

    def step(self, seconds: float = 0.1) -> None:
        self.now += seconds
        self.check.step()

    def halts(self) -> int:
        return sum(1 for kind, _ in self.calls if kind == "halt")

    def alarms(self) -> list[Any]:
        return [value for kind, value in self.calls if kind == "alarm"]


def test_below_the_floor_sends_one_halt_then_raises_the_alarm() -> None:
    harness = Harness()
    harness.step()
    assert harness.calls == []
    harness.free[JOURNAL] = harness.free[HISTORY] = 4_000_000_000
    harness.now += 1.0
    harness.step()
    assert [kind for kind, _ in harness.calls] == ["halt", "alarm"]
    alarm = harness.alarms()[0]
    assert alarm["active"] is True
    details = alarm["details"]
    assert details["cause"] == "below_floor"
    assert details["path"] == str(JOURNAL)
    assert details["free_bytes"] == 4_000_000_000
    assert details["floor_bytes"] == FLOOR
    assert details["margin_bytes"] == MARGIN
    assert details["writer"] is None
    assert details["failure"] == {
        "message": f"Disk nearly full: 4 GB free on the disk that holds {JOURNAL}. "
        "ManyForge stops below 10 GB. Free at least 7 GB on that disk."
    }
    # It lasts: no second Halt, and the alarm's facts stay those of entry.
    for _ in range(30):
        harness.free[JOURNAL] = harness.free[HISTORY] = 3_000_000_000
        harness.step(0.2)
    assert harness.halts() == 1
    assert len(harness.alarms()) == 1


def test_no_free_inode_counts_as_below_the_floor() -> None:
    harness = Harness()
    harness.inodes[HISTORY] = 0
    harness.devices[HISTORY] = 2
    harness.step()
    assert [kind for kind, _ in harness.calls] == ["halt", "alarm"]
    assert harness.alarms()[0]["details"]["path"] == str(HISTORY)
    assert harness.alarms()[0]["details"]["free_bytes"] == 0


@pytest.mark.parametrize(
    ("full", "writer", "path", "text"),
    [
        ("journal", OPERATION_JOURNAL, JOURNAL, "the operation journal"),
        ("history", EVENT_HISTORY, HISTORY, "the event history"),
    ],
)
def test_a_full_queue_enters(full: str, writer: str, path: Path, text: str) -> None:
    harness = Harness()
    if full == "journal":
        harness.journal = (True, False)
    else:
        harness.history["queue_full"] = True
    harness.step()
    assert [kind for kind, _ in harness.calls] == ["halt", "alarm"]
    details = harness.alarms()[0]["details"]
    assert details["cause"] == "queue_full"
    assert details["writer"] == writer
    assert details["path"] == str(path)
    assert details["failure"]["message"] == (
        f"Disk not taking records: {text} could not write its records ({path}). Check the disk."
    )


def test_a_single_failed_record_does_not_enter_but_rereads_free_space_at_once() -> None:
    harness = Harness()
    harness.step()
    reads = harness.reads
    # A write that failed because the disk was full (or the store's own
    # limit): free space is read again at once, and is above the floor.
    harness.history["storage_full"] = True
    harness.journal = (False, True)
    harness.step()
    assert harness.reads > reads
    for _ in range(20):
        harness.step()
    assert harness.calls == []
    # The same backstop enters when the reading is below the floor.
    harness.history["storage_full"] = False
    harness.step()
    harness.free[JOURNAL] = harness.free[HISTORY] = 1_000_000
    harness.history["storage_full"] = True
    harness.step()
    assert [kind for kind, _ in harness.calls] == ["halt", "alarm"]


def test_leaving_needs_the_floor_plus_margin_and_empty_queues() -> None:
    harness = Harness()
    harness.free[JOURNAL] = harness.free[HISTORY] = 5_000_000_000
    harness.history["queue_full"] = True
    harness.step()
    assert harness.check.active
    # Back above the floor but below floor plus margin: it lasts.
    harness.free[JOURNAL] = harness.free[HISTORY] = FLOOR + MARGIN - 1
    harness.history["queue_full"] = False
    harness.step(1.0)
    assert harness.check.active
    # Floor plus margin, but a queue is not written empty yet: it lasts.
    harness.free[JOURNAL] = harness.free[HISTORY] = FLOOR + MARGIN
    harness.journal = (True, False)
    harness.step(1.0)
    assert harness.check.active
    harness.journal = (False, False)
    harness.step(1.0)
    assert not harness.check.active
    assert harness.alarms()[-1] == {"active": False, "details": {}}
    # Nothing resumes by itself: the only effects were one Halt and the alarm.
    assert [kind for kind, _ in harness.calls] == ["halt", "alarm", "alarm"]


def test_a_new_entry_after_leaving_sends_a_new_halt() -> None:
    harness = Harness()
    harness.journal = (True, False)
    harness.step()
    harness.journal = (False, False)
    harness.step()
    assert not harness.check.active
    harness.history["queue_full"] = True
    harness.step()
    assert harness.halts() == 2
    assert [alarm["active"] for alarm in harness.alarms()] == [True, False, True]


def test_a_halt_that_cannot_be_sent_is_not_retried_and_the_alarm_still_rises() -> None:
    harness = Harness()
    harness.halt_response = HeadlessApiError("execution_owner_unavailable", "no host")
    harness.journal = (True, False)
    for _ in range(10):
        harness.step()
    assert harness.halts() == 1
    assert harness.alarms()[0]["active"] is True


def test_an_alarm_update_that_fails_is_retried_until_it_holds() -> None:
    harness = Harness()
    failures = [HeadlessApiError("operator_alarm_capacity_exhausted", "full")]

    def observe(condition: Any) -> dict[str, Any]:
        harness.calls.append(("alarm", condition))
        if failures:
            raise failures.pop()
        return {"changed": True}

    harness.check._observe_condition = observe  # type: ignore[attr-defined]
    harness.journal = (True, False)
    harness.step()
    harness.step()
    harness.step()
    assert harness.halts() == 1
    assert len(harness.alarms()) == 2


def test_a_blocked_free_space_read_delays_neither_the_pump_nor_a_stop() -> None:
    entered = threading.Event()
    release = threading.Event()
    core_lock = threading.Lock()  # stands for the Headless core lock

    def blocked_read(_path: Path) -> tuple[int, DiskReading]:
        entered.set()
        release.wait(10.0)  # a stalled disk
        return 1, DiskReading(10**12, 50_000_000_000, 1000)

    check = DiskRecordsCheck(
        folders=(RecordFolder(OPERATION_JOURNAL, JOURNAL),),
        floor_bytes=FLOOR,
        margin_bytes=MARGIN,
        history_signals=lambda: {"queue_full": False, "storage_full": False},
        journal_signals=lambda: (False, False),
        halt=lambda: {"ok": True},
        observe_condition=lambda _condition: {"changed": False},
        reader=blocked_read,
    )
    check.start()
    try:
        assert entered.wait(5.0)
        # While the read blocks: the pump's work (a core call under the core
        # lock) and a Stop (its own lane) both run at once.
        started = time.monotonic()
        with core_lock:
            pass
        stop_done = threading.Event()
        threading.Thread(target=stop_done.set, name="stop-lane").start()
        assert stop_done.wait(1.0)
        assert time.monotonic() - started < 1.0
        # Shutdown does not wait for the stalled read.
        started = time.monotonic()
        check.shutdown()
        assert time.monotonic() - started < 2.0
    finally:
        release.set()
