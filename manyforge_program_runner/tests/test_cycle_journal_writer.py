"""The Program-cycle journal keeps its state in memory; one writer copies it to the file."""

from __future__ import annotations

import hashlib
import logging
import os
from pathlib import Path
import sqlite3
from threading import Event, Thread
from typing import Any, Callable

import pytest

from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.cycle_journal_writer import CYCLE_COLUMNS

_WRITER_LOGGER = "manyforge_program_runner.cycle_journal_writer"


def _k(*parts: str) -> bytes:
    return hashlib.md5("/".join(parts).encode()).digest()


def _snapshot(run: int, revision: int, state: str) -> dict[str, Any]:
    return {"capture": {"program_run_id": run}, "run_revision": revision, "state": state}


def _completes(call: Callable[[], Any], timeout_s: float = 5.0) -> Any:
    """Run ``call`` on another thread; fail if it does not return in time."""

    outcome: dict[str, Any] = {}

    def run() -> None:
        try:
            outcome["value"] = call()
        except BaseException as error:  # re-raised on the test thread
            outcome["error"] = error

    thread = Thread(target=run, daemon=True)
    thread.start()
    thread.join(timeout_s)
    assert not thread.is_alive(), "journal call waited for the writer"
    if "error" in outcome:
        raise outcome["error"]
    return outcome.get("value")


def _block_writer(journal: ProgramCycleJournal, monkeypatch) -> tuple[Event, Event]:
    """Make the writer's commit wait on ``release``; ``entered`` marks it waiting."""

    entered, release = Event(), Event()
    apply = journal._writer._apply

    def blocked(record):
        entered.set()
        assert release.wait(10.0)
        apply(record)

    monkeypatch.setattr(journal._writer, "_apply", blocked)
    return entered, release


def _memory(journal: ProgramCycleJournal) -> tuple[Any, ...]:
    with journal._lock:
        return (
            tuple(journal._cycle[column] for column in CYCLE_COLUMNS),
            dict(journal._inflight),
            {run: row[:3] for run, row in journal._history.items()},
            [(key, *row) for key, row in journal._persistent.items()],
        )


def _file(path: Path) -> tuple[Any, ...]:
    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        return (
            database.execute(
                "SELECT " + ",".join(CYCLE_COLUMNS) + " FROM program_cycle"
            ).fetchone(),
            {
                row[0]: tuple(row[1:])
                for row in database.execute(
                    "SELECT * FROM program_cycle_inflight_commands"
                )
            },
            {
                row[0]: tuple(row[1:])
                for row in database.execute("SELECT * FROM program_run_history")
            },
            database.execute(
                "SELECT * FROM program_persistent_value_requests ORDER BY position"
            ).fetchall(),
        )
    finally:
        database.close()


def _scripted_cycle(journal: ProgramCycleJournal) -> None:
    """One Start, terminal and Reset, then one persistent-value request."""

    start, reset, value = _k("caller", "start"), _k("caller", "reset"), _k("caller", "value")
    assert journal.record_request(
        request_key=start, kind="start", intent={"program": 1}, cycle_generation=1
    ).disposition == "new"
    assert journal.record_request(
        request_key=start, kind="start", intent={"program": 1}, cycle_generation=1
    ).disposition == "in_progress"
    journal.begin_acquire(
        expected_generation=1, provisional_run_id=7, evidence={"e": 1},
        causal_request_key=start,
    )
    journal.remember_acquired_claim(
        expected_generation=1, provisional_run_id=7, claim={"claim_generation": 3}
    )
    journal.transition(
        expected_generation=1, expected_states=("starting",), state="running",
        program_run_id=7, capture_identities={"c": 1},
        run_snapshot=_snapshot(7, 1, "running"),
    )
    journal.complete_request(request_key=start, result={})
    assert journal.observe_terminal(
        expected_generation=1, program_run_id=7, terminal_reference="sha256:" + "a" * 64,
        snapshot=_snapshot(7, 2, "stopped"),
    )
    journal.record_request(
        request_key=reset, kind="reset", intent={"reload_initial_scene": False},
        cycle_generation=1,
    )
    assert journal.begin_resetting(
        expected_generation=1, program_run_id=7, causal_request_key=reset
    )
    journal.remember_release_intent(
        expected_generation=1, claim_generation=3, release={"r": 1}
    )
    journal.publish_reset(expected_generation=1, request_key=reset, result={})
    journal.record_request(
        request_key=value, kind="persistent_value_set", intent={}, cycle_generation=2
    )
    journal.complete_persistent_value_request(request_key=value, outcome="applied")


@pytest.fixture
def journal(tmp_path: Path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    yield journal
    journal.close()


def test_a_slow_writer_never_delays_journal_commands_or_reads(
    journal: ProgramCycleJournal, tmp_path: Path, monkeypatch
) -> None:
    entered, release = _block_writer(journal, monkeypatch)

    _completes(lambda: _scripted_cycle(journal))
    assert entered.wait(5.0)
    record = _completes(journal.read)
    status, pending = _completes(journal.status_snapshot)
    runs, more = _completes(lambda: journal.list_run_snapshots(limit=4))

    # The state is in memory while the file is behind.
    assert (record.state, record.cycle_generation) == ("idle_after_reset", 2)
    assert status == record and pending == ()
    assert runs == [_snapshot(7, 2, "stopped")] and not more
    assert journal._writer.handled_sequence < journal._sequence
    memory = _memory(journal)

    release.set()
    assert journal._writer.wait_idle(5.0)
    assert journal._writer.handled_sequence == journal._sequence
    journal.close()
    assert _file(tmp_path / "cycle.sqlite3") == memory
    assert journal._writer.failed_commits == 0


class _FailingCommit:
    """The writer's connection, with COMMIT failing while ``fail`` is set."""

    def __init__(self, database: sqlite3.Connection) -> None:
        self._database = database
        self.fail = True

    def execute(self, sql: str, *parameters: Any) -> Any:
        if sql == "COMMIT" and self.fail:
            raise sqlite3.OperationalError("disk I/O error")
        return self._database.execute(sql, *parameters)

    @property
    def in_transaction(self) -> bool:
        return self._database.in_transaction

    def close(self) -> None:
        self._database.close()


def test_a_failing_writer_leaves_every_journal_method_unchanged(
    journal: ProgramCycleJournal, tmp_path: Path, caplog
) -> None:
    caplog.set_level(logging.ERROR, logger=_WRITER_LOGGER)
    assert journal._writer.wait_idle(5.0)
    failing = _FailingCommit(journal._writer._database)
    journal._writer._database = failing

    _scripted_cycle(journal)
    assert journal._writer.wait_idle(5.0)
    posted = journal._sequence
    assert journal._writer.failed_commits == posted > 1
    errors = [r for r in caplog.records if "disk I/O error" in r.getMessage()]
    assert len(errors) == 1
    assert journal.read().cycle_generation == 2

    # Once commits succeed again, later records reach the file.
    failing.fail = False
    later = _k("caller", "later")
    assert journal.record_request(
        request_key=later, kind="runtime_blackboard_write", intent={}, cycle_generation=2
    ).disposition == "new"
    journal.begin_acquire(expected_generation=2, provisional_run_id=8, evidence={"e": 2})
    assert journal._writer.wait_idle(5.0)
    assert journal._writer.failed_commits == posted
    memory = _memory(journal)
    journal.close()
    stored = _file(tmp_path / "cycle.sqlite3")
    # The cycle row is a full image, so it is current; the failed row edits are lost.
    assert stored[0] == memory[0]
    assert stored[1] == {later: ("runtime_blackboard_write", "{}", 2)}


def test_records_from_many_threads_reach_the_file_in_sequence_order(
    journal: ProgramCycleJournal, tmp_path: Path, monkeypatch
) -> None:
    handled: list[int] = []
    apply = journal._writer._apply

    def recording(record):
        handled.append(record.sequence)
        apply(record)

    monkeypatch.setattr(journal._writer, "_apply", recording)

    def worker(base: int) -> None:
        for index in range(40):
            key = _k("worker", str(base), str(index))
            journal.record_request(
                request_key=key, kind="runtime_blackboard_write",
                intent={"index": index}, cycle_generation=1,
            )
            if index % 5:
                journal.reject_request(request_key=key, code="x", message="x")
            if index % 10 == 0:
                journal.retain_run_snapshot(_snapshot(base * 100 + index + 1, 1, "succeeded"))

    threads = [Thread(target=worker, args=(base,)) for base in (1, 2, 3)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(10.0)
    assert journal._writer.wait_idle(5.0)

    assert handled == list(range(1, journal._sequence + 1))
    memory = _memory(journal)
    assert len(memory[1]) == 24 and len(memory[2]) == 12
    journal.close()
    assert _file(tmp_path / "cycle.sqlite3") == memory
    assert journal._writer.failed_commits == journal._writer.dropped_records == 0


def test_close_drains_the_writer_and_leaves_one_file(
    journal: ProgramCycleJournal, tmp_path: Path
) -> None:
    path = tmp_path / "cycle.sqlite3"
    _scripted_cycle(journal)
    memory = _memory(journal)

    journal.close()
    journal.close()

    assert not Path(f"{path}-wal").exists()
    assert not Path(f"{path}-shm").exists()
    assert not journal._writer._thread.is_alive()
    assert _file(path) == memory
    for call in (
        journal.read,
        journal.status_snapshot,
        journal.pending_commands,
        lambda: journal.list_run_snapshots(limit=1),
        lambda: journal.read_run_snapshot(7),
        lambda: journal.record_request(
            request_key=_k("caller", "late"), kind="start", intent={}, cycle_generation=2
        ),
    ):
        with pytest.raises(RuntimeError, match="Program-cycle journal is closed"):
            call()


class _AuditedConnection:
    """The writer's connection, noting any statement run while the journal lock is held."""

    def __init__(self, database: sqlite3.Connection, journal: ProgramCycleJournal,
                 violations: list[str]) -> None:
        self._database = database
        self._journal = journal
        self._violations = violations

    def execute(self, sql: str, *parameters: Any) -> Any:
        if self._journal._lock._is_owned():
            self._violations.append(f"sqlite execute under the journal lock: {sql}")
        return self._database.execute(sql, *parameters)

    @property
    def in_transaction(self) -> bool:
        return self._database.in_transaction

    def close(self) -> None:
        self._database.close()


def test_no_file_work_runs_under_the_journal_lock(
    journal: ProgramCycleJournal, monkeypatch
) -> None:
    violations: list[str] = []
    assert journal._writer.wait_idle(5.0)
    journal._writer._database = _AuditedConnection(
        journal._writer._database, journal, violations
    )
    apply = journal._writer._apply

    def audited_apply(record):
        if journal._lock._is_owned():
            violations.append("writer holds the journal lock")
        apply(record)

    fsync = os.fsync

    def audited_fsync(descriptor: int) -> None:
        if journal._lock._is_owned():
            violations.append("fsync under the journal lock")
        fsync(descriptor)

    monkeypatch.setattr(journal._writer, "_apply", audited_apply)
    monkeypatch.setattr(os, "fsync", audited_fsync)

    _scripted_cycle(journal)
    journal.read()
    journal.status_snapshot()
    assert journal._writer.wait_idle(5.0)
    journal.close()

    assert violations == []
    assert journal._writer.failed_commits == 0
    assert journal._writer.handled_sequence == journal._sequence


def test_a_full_writer_queue_drops_the_record_without_waiting(
    journal: ProgramCycleJournal, monkeypatch, caplog
) -> None:
    caplog.set_level(logging.ERROR, logger=_WRITER_LOGGER)
    entered, release = _block_writer(journal, monkeypatch)
    journal._writer._maximum_queued_records = 1

    def record(name: str) -> None:
        journal.record_request(
            request_key=_k("caller", name), kind="runtime_blackboard_write",
            intent={}, cycle_generation=1,
        )

    _completes(lambda: record("first"))
    assert entered.wait(5.0)
    _completes(lambda: record("queued"))
    _completes(lambda: record("dropped"))

    assert journal._writer.dropped_records == 1
    assert _k("caller", "dropped") in journal._inflight_rows()
    release.set()
    assert journal._writer.wait_idle(5.0)
    assert len([r for r in caplog.records if "(queue full)" in r.getMessage()]) == 1
    assert journal._writer.handled_sequence == 2
    assert journal._writer.failed_commits == 0


def test_a_replaced_parent_directory_is_logged_and_counted_by_the_writer(
    tmp_path: Path, caplog
) -> None:
    caplog.set_level(logging.ERROR, logger=_WRITER_LOGGER)
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    journal = ProgramCycleJournal(state_root / "cycle.sqlite3")
    try:
        state_root.rename(tmp_path / "retained-state")
        state_root.mkdir(mode=0o700)

        assert journal.read().state == "idle_after_reset"
        assert journal.record_request(
            request_key=_k("caller", "start"), kind="start", intent={}, cycle_generation=1
        ).disposition == "new"
        assert journal._writer.wait_idle(5.0)

        assert journal._writer.failed_commits == 1
        assert any("replaced or changed" in r.getMessage() for r in caplog.records)
        assert len(journal.pending_commands()) == 1
    finally:
        journal.close()
    assert list(state_root.iterdir()) == []


def test_an_unexpected_rollback_sidecar_is_logged_and_counted_by_the_writer(
    tmp_path: Path, caplog
) -> None:
    caplog.set_level(logging.ERROR, logger=_WRITER_LOGGER)
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path)
    try:
        rollback = Path(f"{path}-journal")
        rollback.write_bytes(b"unexpected rollback state")
        rollback.chmod(0o600)

        assert journal.read().state == "idle_after_reset"
        assert journal.record_request(
            request_key=_k("caller", "start"), kind="start", intent={}, cycle_generation=1
        ).disposition == "new"
        assert journal._writer.wait_idle(5.0)

        assert journal._writer.failed_commits == 1
        assert any(
            "unsupported rollback sidecar" in r.getMessage() for r in caplog.records
        )
    finally:
        journal.close()


_EPISODE = "are not being written"


class _Gate:
    """Holds the writer before each write while closed; ``entered`` marks it held."""

    def __init__(self, journal: ProgramCycleJournal, monkeypatch) -> None:
        self.opened, self.entered = Event(), Event()
        self.opened.set()
        self.marks = 0
        apply = journal._writer._apply

        def gated(record):
            if not self.opened.is_set():
                self.entered.set()
                assert self.opened.wait(10.0)
            if record.operations == (("records_lost", ()),):
                self.marks += 1
            apply(record)

        monkeypatch.setattr(journal._writer, "_apply", gated)

    def close(self) -> None:
        self.entered.clear()
        self.opened.clear()


def _post(journal: ProgramCycleJournal, name: str) -> None:
    journal.record_request(
        request_key=_k("caller", name), kind="runtime_blackboard_write",
        intent={}, cycle_generation=1,
    )


def _episode_lines(caplog) -> list[logging.LogRecord]:
    return [r for r in caplog.records if _EPISODE in r.getMessage()]


def _records_lost(path: Path) -> int:
    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        return database.execute(
            "SELECT records_lost FROM program_cycle_metadata WHERE singleton=1"
        ).fetchone()[0]
    finally:
        database.close()


def test_a_full_queue_starts_one_loss_episode_until_the_queue_is_written_empty(
    journal: ProgramCycleJournal, tmp_path: Path, monkeypatch, caplog
) -> None:
    caplog.set_level(logging.WARNING, logger=_WRITER_LOGGER)
    gate = _Gate(journal, monkeypatch)
    journal._writer._maximum_queued_records = 1

    def episode(prefix: str, drops: int) -> None:
        gate.close()
        _completes(lambda: _post(journal, prefix + "-held"))
        assert gate.entered.wait(5.0)
        _completes(lambda: _post(journal, prefix + "-queued"))
        for index in range(drops):
            _completes(lambda: _post(journal, f"{prefix}-dropped-{index}"))
        gate.opened.set()
        assert journal._writer.wait_idle(5.0)

    episode("first", 3)
    assert journal._writer.dropped_records == 3
    assert [r.getMessage() for r in _episode_lines(caplog)] == [
        "Program-cycle journal records are not being written (queue full); "
        "counting them until the queue is written empty again"
    ]
    assert gate.marks == 1

    # The queue was written empty: the next drop starts a new episode.
    episode("second", 2)
    assert journal._writer.dropped_records == 5
    assert len(_episode_lines(caplog)) == 2
    assert gate.marks == 1
    # Lines are written by the writer thread, never by the poster.
    assert {r.threadName for r in _episode_lines(caplog)} == {
        "program-cycle-journal-writer"
    }

    journal.close()
    assert _records_lost(tmp_path / "cycle.sqlite3") == 1
    assert [r.getMessage() for r in caplog.records if "this start" in r.getMessage()] == [
        "Program-cycle journal records not written in this start: "
        "queue full 5, failed 0, not drained 0"
    ]
    assert journal._writer.failed_commits == 0


def test_failed_writes_start_one_episode_and_the_mark_is_retried_at_close(
    journal: ProgramCycleJournal, tmp_path: Path, monkeypatch, caplog
) -> None:
    caplog.set_level(logging.WARNING, logger=_WRITER_LOGGER)
    assert journal._writer.wait_idle(5.0)
    gate = _Gate(journal, monkeypatch)
    failing = _FailingCommit(journal._writer._database)
    journal._writer._database = failing

    for name in ("a", "b", "c"):
        _post(journal, name)
    assert journal._writer.wait_idle(5.0)
    assert journal._writer.failed_commits == 3
    assert len(_episode_lines(caplog)) == 1
    assert "failed write of record" in _episode_lines(caplog)[0].getMessage()
    # The mark failed with the records; it is not retried while running.
    assert gate.marks == 1

    failing.fail = False
    _post(journal, "written")
    assert journal._writer.wait_idle(5.0)
    failing.fail = True
    _post(journal, "failed-again")
    assert journal._writer.wait_idle(5.0)
    failing.fail = False
    _post(journal, "written-again")
    assert journal._writer.wait_idle(5.0)
    assert journal._writer.failed_commits == 4
    assert len(_episode_lines(caplog)) == 2
    assert gate.marks == 1

    journal.close()
    assert gate.marks == 2
    assert _records_lost(tmp_path / "cycle.sqlite3") == 1
    assert [r.getMessage() for r in caplog.records if "this start" in r.getMessage()] == [
        "Program-cycle journal records not written in this start: "
        "queue full 0, failed 4, not drained 0"
    ]


def test_a_start_without_loss_leaves_the_mark_clear_and_logs_nothing(
    journal: ProgramCycleJournal, tmp_path: Path, caplog
) -> None:
    caplog.set_level(logging.WARNING, logger=_WRITER_LOGGER)
    _scripted_cycle(journal)
    journal.close()
    assert _records_lost(tmp_path / "cycle.sqlite3") == 0
    assert caplog.records == []
