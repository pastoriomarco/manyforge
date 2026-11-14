"""The one writer that copies the Program-cycle state into its SQLite file."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
import logging
import sqlite3
from threading import Condition, Thread
from typing import Any, Final

_LOGGER = logging.getLogger(__name__)

# Records waiting for the writer; a post to a full queue drops the record.
MAXIMUM_QUEUED_RECORDS: Final = 4_096

CYCLE_COLUMNS: Final = (
    "cycle_generation",
    "state",
    "program_run_id",
    "claim_json",
    "capture_identities_json",
    "terminal_reference",
    "start_settlement_json",
    "release_intent_json",
    "reconciliation_required",
    "acquisition_pending_json",
    "execution_control_revision",
    "causal_request_key",
    "current_command_kind",
    "failure_json",
    "reset_phase",
    "reset_effect_claim_json",
)

_UPDATE_CYCLE: Final = (
    "UPDATE program_cycle SET "
    + ",".join(f"{column}=?" for column in CYCLE_COLUMNS)
    + " WHERE singleton=1"
)
# Row images replace what is there, so a record lost to a failed commit
# never makes a later record fail on a key.
_OPERATIONS: Final = {
    "inflight_put": "INSERT OR REPLACE INTO program_cycle_inflight_commands VALUES(?,?,?,?)",
    "inflight_delete": "DELETE FROM program_cycle_inflight_commands WHERE request_key=?",
    "history_put": (
        "INSERT INTO program_run_history VALUES(?,?,?,?) "
        "ON CONFLICT(program_run_id) DO UPDATE SET run_revision=excluded.run_revision,"
        "snapshot_json=excluded.snapshot_json,terminal=excluded.terminal"
    ),
    "history_delete": "DELETE FROM program_run_history WHERE program_run_id=?",
    "persistent_insert": "INSERT OR REPLACE INTO program_persistent_value_requests VALUES(?,?,?,?)",
    "persistent_trim": "DELETE FROM program_persistent_value_requests WHERE position<=?",
    # The writer's own mark that records of this start were not written; never posted.
    "records_lost": "UPDATE program_cycle_metadata SET records_lost=1 WHERE singleton=1",
}


@dataclass(frozen=True, slots=True)
class JournalCommit:
    """One state change as explicit rows: the full cycle row and ordered row edits."""

    sequence: int
    cycle_row: tuple[Any, ...] | None
    operations: tuple[tuple[str, tuple[Any, ...]], ...]


_END: Final = object()


class CycleJournalWriter:
    """Applies posted records in order, one SQLite transaction each, on its own thread.

    It never takes the journal's lock. A record that is not written (queue
    full, failed commit, not drained at shutdown) is counted and the writer
    carries on. A loss episode starts at the first record not written and ends
    when the queue has been written empty again; the writer logs one line when
    an episode starts and, in the first episode, writes the file's
    ``records_lost`` mark. ``close`` logs one line with the counts.
    """

    def __init__(
        self,
        database: sqlite3.Connection,
        *,
        validate_storage: Callable[[], None],
        close_storage: Callable[[], None],
        maximum_queued_records: int = MAXIMUM_QUEUED_RECORDS,
    ) -> None:
        self._database = database
        self._validate_storage = validate_storage
        self._close_storage = close_storage
        self._maximum_queued_records = maximum_queued_records
        self._queue: deque[Any] = deque()
        self._condition = Condition()
        self._busy = False
        self._ended = False
        self.failed_commits = 0
        self.dropped_records = 0
        self.not_drained_records = 0
        self.handled_sequence = 0
        # Set by `post` under the queue lock; the writer thread reports it.
        self._drops_unreported = False
        # Writer thread only.
        self._in_episode = False
        self._mark: str | None = None  # None, "written" or "pending"
        self._thread = Thread(
            target=self._run,
            name="program-cycle-journal-writer",
            daemon=True,
        )
        self._thread.start()

    def post(self, record: JournalCommit) -> None:
        """Queue one record; memory work only, never waits.

        Called under the journal lock: a full queue drops the record and only
        counts it; the writer thread reports it.
        """

        with self._condition:
            if len(self._queue) >= self._maximum_queued_records:
                self.dropped_records += 1
                self._drops_unreported = True
                return
            self._queue.append(record)
            self._condition.notify()

    def wait_idle(self, timeout_s: float | None = None) -> bool:
        """Wait until every posted record was handled (tests and shutdown)."""

        with self._condition:
            return self._condition.wait_for(
                lambda: not self._queue and not self._busy, timeout_s
            )

    def close(self, timeout_s: float) -> bool:
        """Drain, close the file and stop; False when the bound is exceeded.

        Logs one line when records of this start were not written.
        """

        with self._condition:
            if not self._ended:
                self._ended = True
                self._queue.append(_END)
                self._condition.notify()
        self._thread.join(timeout_s)
        finished = not self._thread.is_alive()
        with self._condition:
            if not finished:
                self.not_drained_records = sum(
                    1 for record in self._queue if record is not _END
                )
            counts = (self.dropped_records, self.failed_commits, self.not_drained_records)
        if not finished:
            _LOGGER.warning(
                "Program-cycle journal writer did not finish within %.1f s; records "
                "not written: queue full %d, failed %d, not drained %d",
                timeout_s,
                *counts,
            )
        elif any(counts):
            _LOGGER.warning(
                "Program-cycle journal records not written in this start: "
                "queue full %d, failed %d, not drained %d",
                *counts,
            )
        return finished

    def _run(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(lambda: bool(self._queue))
                record = self._queue.popleft()
                self._busy = True
                dropped = self._drops_unreported
                self._drops_unreported = False
            if dropped:
                self._record_lost("queue full")
            if record is _END:
                if self._mark == "pending":
                    self._write_mark()
                try:
                    self._database.close()
                    self._close_storage()
                except Exception:
                    _LOGGER.exception("Program-cycle journal file did not close cleanly")
                with self._condition:
                    self._busy = False
                    self._condition.notify_all()
                return
            try:
                self._apply(record)
                written = True
            except Exception as error:
                written = False
                self.failed_commits += 1
                self._record_lost(
                    f"failed write of record {record.sequence}: "
                    f"{type(error).__name__}: {error}"
                )
            with self._condition:
                if written and not self._queue and not self._drops_unreported:
                    self._in_episode = False
                self.handled_sequence = record.sequence
                self._busy = False
                self._condition.notify_all()

    def _record_lost(self, cause: str) -> None:
        """Writer thread only: the first loss of an episode logs one line and marks the file."""

        if self._in_episode:
            return
        self._in_episode = True
        _LOGGER.error(
            "Program-cycle journal records are not being written (%s); counting "
            "them until the queue is written empty again",
            cause,
        )
        if self._mark is None:
            self._mark = "written" if self._write_mark() else "pending"

    def _write_mark(self) -> bool:
        """Set the file's ``records_lost`` mark; False when it could not be written."""

        try:
            self._apply(JournalCommit(0, None, (("records_lost", ()),)))
        except Exception:
            return False
        return True

    def _apply(self, record: JournalCommit) -> None:
        self._validate_storage()
        database = self._database
        database.execute("BEGIN IMMEDIATE")
        try:
            if record.cycle_row is not None:
                database.execute(_UPDATE_CYCLE, record.cycle_row)
            for name, parameters in record.operations:
                database.execute(_OPERATIONS[name], parameters)
            database.execute("COMMIT")
        except BaseException:
            if database.in_transaction:
                database.execute("ROLLBACK")
            raise


__all__ = [
    "CYCLE_COLUMNS",
    "CycleJournalWriter",
    "JournalCommit",
    "MAXIMUM_QUEUED_RECORDS",
]
