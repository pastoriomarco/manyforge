"""Durable SQLite journal for the single Program-cycle state machine."""

from __future__ import annotations

from collections.abc import Mapping
from contextlib import contextmanager
from dataclasses import dataclass
import json
from pathlib import Path
import sqlite3
from threading import RLock
from typing import Any, Final, Iterator

from .liveness import RetainedLiveness


SCHEMA_VERSION: Final = 1
STATES: Final = frozenset(
    {
        "idle_after_reset",
        "starting",
        "running",
        "stopping",
        "terminal_awaiting_reset",
    }
)


@dataclass(frozen=True, slots=True)
class CycleRecord:
    cycle_generation: int
    state: str
    owner_epoch: str
    program_run_id: str | None
    claim: dict[str, Any] | None
    capture_identities: dict[str, Any] | None
    terminal_reference: str | None
    release_intent: dict[str, Any] | None
    reset_result: dict[str, Any] | None
    reconciliation_required: bool
    acquisition_pending: dict[str, Any] | None
    reconciliation_evidence_digest: str | None


@dataclass(frozen=True, slots=True)
class RequestRecord:
    disposition: str
    result: dict[str, Any] | None = None
    error: dict[str, Any] | None = None


class ProgramCycleJournal:
    """One-row cycle state plus immutable, durable request replay."""

    def __init__(
        self,
        path: Path,
        *,
        owner_epoch: str,
        maximum_requests: int = 4_096,
    ) -> None:
        if not 1 <= maximum_requests <= 4_096:
            raise ValueError("maximum_requests must be in [1, 4096]")
        self._maximum_requests = maximum_requests
        self._lock = RLock()
        path.parent.mkdir(parents=True, exist_ok=True)
        self._database = sqlite3.connect(
            path, timeout=10.0, isolation_level=None, check_same_thread=False
        )
        self._database.execute("PRAGMA foreign_keys=ON")
        self._database.execute("PRAGMA journal_mode=WAL")
        self._database.execute("PRAGMA synchronous=FULL")
        self._database.executescript(
            """
            CREATE TABLE IF NOT EXISTS program_cycle_metadata (
              singleton INTEGER PRIMARY KEY CHECK(singleton=1),
              schema_version INTEGER NOT NULL CHECK(schema_version=1)
            );
            CREATE TABLE IF NOT EXISTS program_cycle (
              singleton INTEGER PRIMARY KEY CHECK(singleton=1),
              cycle_generation INTEGER NOT NULL CHECK(cycle_generation>0),
              state TEXT NOT NULL,
              owner_epoch TEXT NOT NULL,
              program_run_id TEXT,
              claim_json TEXT,
              capture_identities_json TEXT,
              terminal_reference TEXT,
              release_intent_json TEXT,
              reset_result_json TEXT,
              reconciliation_required INTEGER NOT NULL
                CHECK(reconciliation_required IN (0,1)),
              acquisition_pending_json TEXT,
              reconciliation_evidence_digest TEXT
            );
            CREATE TABLE IF NOT EXISTS program_cycle_requests (
              request_owner_epoch TEXT NOT NULL,
              request_id TEXT NOT NULL,
              request_kind TEXT NOT NULL,
              intent_json TEXT NOT NULL,
              cycle_generation INTEGER NOT NULL,
              result_json TEXT,
              error_json TEXT,
              PRIMARY KEY(request_owner_epoch, request_id)
            );
            CREATE TABLE IF NOT EXISTS program_runner_liveness (
              singleton INTEGER PRIMARY KEY CHECK(singleton=1),
              manager_runtime_epoch TEXT,
              manager_owner_epoch TEXT,
              predecessor_expired INTEGER NOT NULL
                CHECK(predecessor_expired IN (0,1)),
              fault_reference TEXT
            );
            INSERT OR IGNORE INTO program_runner_liveness
            VALUES(1,NULL,NULL,0,NULL);
            """
        )
        columns = {
            row[1]
            for row in self._database.execute(
                "PRAGMA table_info(program_cycle_requests)"
            )
        }
        if "error_json" not in columns:
            self._database.execute(
                "ALTER TABLE program_cycle_requests ADD COLUMN error_json TEXT"
            )
        cycle_columns = {
            row[1]
            for row in self._database.execute("PRAGMA table_info(program_cycle)")
        }
        if "release_intent_json" not in cycle_columns:
            self._database.execute(
                "ALTER TABLE program_cycle ADD COLUMN release_intent_json TEXT"
            )
        with self._transaction():
            self._database.execute(
                "INSERT OR IGNORE INTO program_cycle_metadata VALUES(1,?)",
                (SCHEMA_VERSION,),
            )
            created = self._database.execute(
                """
                INSERT OR IGNORE INTO program_cycle
                (singleton,cycle_generation,state,owner_epoch,program_run_id,
                 claim_json,capture_identities_json,terminal_reference,
                 reset_result_json,reconciliation_required,
                 acquisition_pending_json,reconciliation_evidence_digest,
                 release_intent_json)
                VALUES(1,1,'idle_after_reset',?,NULL,NULL,NULL,NULL,NULL,0,NULL,NULL,NULL)
                """,
                (owner_epoch,),
            ).rowcount == 1
            row = self.read()
            if not created and (
                row.state != "idle_after_reset"
                or row.claim is not None
                or row.acquisition_pending is not None
            ):
                self._database.execute(
                    """
                    UPDATE program_cycle SET reconciliation_required=1
                    WHERE singleton=1
                    """
                )
                row = self.read()
            if row.owner_epoch != owner_epoch:
                self._database.execute(
                    """
                    UPDATE program_cycle SET reconciliation_required=1
                    WHERE singleton=1
                    """
                )

    def begin_acquire(
        self,
        *,
        expected_generation: int,
        provisional_run_id: str,
        evidence: Mapping[str, Any],
    ) -> None:
        """Durably mark the inherently uncertain acquire/response checkpoint."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET program_run_id=?, acquisition_pending_json=?
                WHERE singleton=1 AND cycle_generation=?
                  AND state='idle_after_reset' AND claim_json IS NULL
                  AND acquisition_pending_json IS NULL
                """,
                (provisional_run_id, self._dump(evidence), expected_generation),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle acquire checkpoint is stale")

    def remember_acquired_claim(
        self,
        *,
        expected_generation: int,
        provisional_run_id: str,
        claim: Mapping[str, Any],
    ) -> None:
        """Close the acquire/local-journal crash window before further reads."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET program_run_id=?, claim_json=?, acquisition_pending_json=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state='idle_after_reset' AND claim_json IS NULL
                  AND acquisition_pending_json IS NOT NULL
                """,
                (
                    provisional_run_id,
                    self._dump(claim),
                    expected_generation,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle claim persistence is stale")

    def mark_acquire_unknown(self, *, expected_generation: int) -> None:
        """Fail closed without inventing the claim hidden by response loss."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET reconciliation_required=1
                WHERE singleton=1 AND cycle_generation=?
                  AND state='idle_after_reset' AND claim_json IS NULL
                  AND acquisition_pending_json IS NOT NULL
                """,
                (expected_generation,),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle unknown acquire is stale")

    def cancel_rejected_acquire(self, *, expected_generation: int) -> None:
        """Clear a checkpoint only after an explicit native rejection packet."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET program_run_id=NULL, acquisition_pending_json=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state='idle_after_reset' AND claim_json IS NULL
                  AND acquisition_pending_json IS NOT NULL
                """,
                (expected_generation,),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle rejected acquire is stale")

    @contextmanager
    def _transaction(self) -> Iterator[None]:
        with self._lock:
            self._database.execute("BEGIN IMMEDIATE")
            try:
                yield
            except BaseException:
                self._database.execute("ROLLBACK")
                raise
            self._database.execute("COMMIT")

    @staticmethod
    def _load(value: str | None) -> dict[str, Any] | None:
        return None if value is None else json.loads(value)

    @staticmethod
    def _dump(value: Mapping[str, Any] | None) -> str | None:
        return (
            None
            if value is None
            else json.dumps(dict(value), sort_keys=True, separators=(",", ":"))
        )

    def read(self) -> CycleRecord:
        with self._lock:
            row = self._database.execute(
                """
                SELECT cycle_generation,state,owner_epoch,program_run_id,claim_json,
                       capture_identities_json,terminal_reference,release_intent_json,
                       reset_result_json,
                       reconciliation_required,acquisition_pending_json,
                       reconciliation_evidence_digest
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
        if row is None or row[1] not in STATES:
            raise RuntimeError("Program-cycle journal is corrupt")
        return CycleRecord(
            row[0],
            row[1],
            row[2],
            row[3],
            self._load(row[4]),
            self._load(row[5]),
            row[6],
            self._load(row[7]),
            self._load(row[8]),
            bool(row[9]),
            self._load(row[10]),
            row[11],
        )

    def remember_release_intent(
        self,
        *,
        expected_generation: int,
        claim_generation: int,
        release: Mapping[str, Any],
    ) -> None:
        """Durably bind the exact Reset release before invoking the Kernel."""
        encoded = self._dump(release)
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET release_intent_json=?
                WHERE singleton=1 AND cycle_generation=?
                  AND state='terminal_awaiting_reset'
                  AND reconciliation_required=0
                  AND CAST(json_extract(claim_json,'$.claim_generation') AS INTEGER)=?
                  AND (release_intent_json IS NULL OR release_intent_json=?)
                """,
                (encoded, expected_generation, claim_generation, encoded),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle release intent is stale")

    def record_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        kind: str,
        intent: Mapping[str, Any],
        cycle_generation: int,
    ) -> RequestRecord:
        encoded = self._dump(intent)
        with self._transaction():
            current = self._database.execute(
                """
                SELECT request_kind,intent_json,result_json,error_json
                FROM program_cycle_requests
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).fetchone()
            if current is not None:
                if current[0] != kind or current[1] != encoded:
                    raise ValueError(
                        "request identity was reused with different intent"
                    )
                if current[2] is not None:
                    return RequestRecord("succeeded", result=self._load(current[2]))
                if current[3] is not None:
                    return RequestRecord("rejected", error=self._load(current[3]))
                return RequestRecord("in_progress")
            count = self._database.execute(
                "SELECT COUNT(*) FROM program_cycle_requests"
            ).fetchone()[0]
            if count >= self._maximum_requests:
                raise OverflowError("Program-cycle request ledger is full")
            self._database.execute(
                """
                INSERT INTO program_cycle_requests VALUES(?,?,?,?,?,NULL,NULL)
                """,
                (owner_epoch, request_id, kind, encoded, cycle_generation),
            )
        return RequestRecord("new")

    def reject_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        code: str,
        message: str,
    ) -> None:
        """Durably retain one exact typed no-success result."""
        encoded = self._dump({"code": code, "message": message})
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle_requests
                SET error_json=COALESCE(error_json,?)
                WHERE request_owner_epoch=? AND request_id=?
                  AND result_json IS NULL
                """,
                (encoded, owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle replay request is absent")

    def cancel_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
    ) -> None:
        """Remove an explicitly rejected, retryable request before any effect."""
        with self._transaction():
            changed = self._database.execute(
                """
                DELETE FROM program_cycle_requests
                WHERE request_owner_epoch=? AND request_id=?
                  AND result_json IS NULL AND error_json IS NULL
                """,
                (owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle replay request is absent")

    def transition(
        self,
        *,
        expected_generation: int,
        expected_states: tuple[str, ...],
        state: str,
        program_run_id: str | None = None,
        claim: Mapping[str, Any] | None = None,
        capture_identities: Mapping[str, Any] | None = None,
        terminal_reference: str | None = None,
        reconciliation_required: bool = False,
    ) -> None:
        if state not in STATES:
            raise ValueError("invalid Program-cycle state")
        if "terminal_awaiting_reset" in expected_states:
            raise ValueError("terminal Program-cycle evidence is immutable")
        placeholders = ",".join("?" for _ in expected_states)
        with self._transaction():
            changed = self._database.execute(
                f"""
                UPDATE program_cycle
                SET state=?, program_run_id=COALESCE(?,program_run_id),
                    claim_json=COALESCE(?,claim_json),
                    capture_identities_json=COALESCE(?,capture_identities_json),
                    terminal_reference=COALESCE(?,terminal_reference),
                    reconciliation_required=?, acquisition_pending_json=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state IN ({placeholders})
                """,
                (
                    state,
                    program_run_id,
                    self._dump(claim),
                    self._dump(capture_identities),
                    terminal_reference,
                    int(reconciliation_required),
                    expected_generation,
                    *expected_states,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle transition is stale")

    def observe_terminal(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        terminal_reference: str,
    ) -> bool:
        """Record one exact terminal fact, idempotently across observer races."""
        with self._transaction():
            row = self._database.execute(
                """
                SELECT cycle_generation,state,program_run_id,terminal_reference
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
            if row is None:
                raise RuntimeError("Program-cycle journal is corrupt")
            generation, state, current_run_id, current_reference = row
            if generation != expected_generation or current_run_id != program_run_id:
                return False
            if state == "terminal_awaiting_reset":
                if current_reference != terminal_reference:
                    raise RuntimeError("terminal Program-cycle evidence conflicts")
                return False
            if state not in {"running", "stopping"}:
                return False
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET state='terminal_awaiting_reset', terminal_reference=?
                WHERE singleton=1 AND cycle_generation=?
                  AND program_run_id=? AND state IN ('running','stopping')
                """,
                (
                    terminal_reference,
                    expected_generation,
                    program_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle terminal observation is stale")
            return True

    def mark_stopping(
        self, *, expected_generation: int, program_run_id: str
    ) -> None:
        """Publish stopping unless an exact terminal observer already won."""
        with self._transaction():
            row = self._database.execute(
                """
                SELECT cycle_generation,state,program_run_id
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
            if row == (
                expected_generation,
                "terminal_awaiting_reset",
                program_run_id,
            ):
                return
            changed = self._database.execute(
                """
                UPDATE program_cycle SET state='stopping'
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state IN ('running','stopping')
                """,
                (expected_generation, program_run_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle stopping transition is stale")

    def require_reconciliation(self, *, expected_generation: int) -> None:
        """Latch an unresolved cross-owner effect without changing owner facts."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET reconciliation_required=1
                WHERE singleton=1 AND cycle_generation=?
                """,
                (expected_generation,),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle reconciliation latch is stale")

    def liveness(self) -> RetainedLiveness:
        """Read only durable identities/evidence, never a deadline or age."""
        with self._lock:
            row = self._database.execute(
                """
                SELECT manager_runtime_epoch,manager_owner_epoch,
                       predecessor_expired,fault_reference
                FROM program_runner_liveness WHERE singleton=1
                """
            ).fetchone()
        if row is None:
            raise RuntimeError("Program Runner liveness journal is corrupt")
        return RetainedLiveness(row[0], row[1], bool(row[2]), row[3])

    def retain_liveness_loss(
        self,
        *,
        manager_runtime_epoch: str,
        manager_owner_epoch: str | None,
        fault_reference: str,
        expected_generation: int,
    ) -> None:
        """Atomically latch Manager loss and cycle reconciliation evidence."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_runner_liveness
                SET manager_runtime_epoch=?,manager_owner_epoch=?,
                    predecessor_expired=1,fault_reference=?
                WHERE singleton=1
                """,
                (
                    manager_runtime_epoch,
                    manager_owner_epoch,
                    fault_reference,
                ),
            ).rowcount
            cycle_changed = self._database.execute(
                """
                UPDATE program_cycle
                SET reconciliation_required=1,
                    reconciliation_evidence_digest=?
                WHERE singleton=1 AND cycle_generation=?
                """,
                (fault_reference, expected_generation),
            ).rowcount
            if changed != 1 or cycle_changed != 1:
                raise RuntimeError("Program Runner liveness loss is stale")

    def retain_liveness_manager(
        self, *, manager_runtime_epoch: str, manager_owner_epoch: str
    ) -> None:
        """Retain the accepted Manager identity without persisting receipt age."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_runner_liveness
                SET manager_runtime_epoch=?,manager_owner_epoch=?,
                    predecessor_expired=0
                WHERE singleton=1
                """,
                (manager_runtime_epoch, manager_owner_epoch),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program Runner liveness identity is absent")

    def reconcile_liveness(
        self,
        *,
        owner_epoch: str,
        evidence_digest: str,
        require_terminal_release: bool = True,
    ) -> None:
        """Prepare an idempotent successor row for exact Kernel reconciliation."""
        with self._transaction():
            row = self._database.execute(
                """
                SELECT state,owner_epoch,acquisition_pending_json
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
            if row is None:
                raise RuntimeError("Program-cycle journal is corrupt")
            if row[0] not in {"idle_after_reset", "terminal_awaiting_reset"}:
                raise RuntimeError(
                    "active Program cycle is not contained for reconciliation"
                )
            if row[2] is not None:
                raise RuntimeError("unknown Program-cycle acquisition is unresolved")
            if (
                require_terminal_release
                and row[0] == "terminal_awaiting_reset"
            ):
                release = self._database.execute(
                    "SELECT release_intent_json FROM program_cycle WHERE singleton=1"
                ).fetchone()
                if release is None or release[0] is None:
                    raise RuntimeError(
                        "terminal Program-cycle release is not durably retained"
                    )
            self._database.execute(
                """
                UPDATE program_cycle
                SET owner_epoch=?,reconciliation_required=0,
                    reconciliation_evidence_digest=?
                WHERE singleton=1
                """,
                (owner_epoch, evidence_digest),
            )

    def complete_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        result: Mapping[str, Any],
    ) -> None:
        encoded = self._dump(result)
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle_requests SET result_json=COALESCE(result_json,?)
                WHERE request_owner_epoch=? AND request_id=?
                  AND error_json IS NULL
                """,
                (encoded, owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle replay request is absent")

    def publish_reset(
        self,
        *,
        expected_generation: int,
        owner_epoch: str,
        request_owner_epoch: str,
        request_id: str,
        result: Mapping[str, Any],
    ) -> None:
        encoded = self._dump(result)
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET cycle_generation=cycle_generation+1,
                    state='idle_after_reset', owner_epoch=?,
                    program_run_id=NULL, claim_json=NULL,
                    capture_identities_json=NULL, terminal_reference=NULL,
                    release_intent_json=NULL,
                    reset_result_json=?, reconciliation_required=0,
                    acquisition_pending_json=NULL,
                    reconciliation_evidence_digest=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state='terminal_awaiting_reset'
                """,
                (owner_epoch, encoded, expected_generation),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset is stale")
            changed = self._database.execute(
                """
                UPDATE program_cycle_requests SET result_json=COALESCE(result_json,?)
                WHERE request_owner_epoch=? AND request_id=?
                  AND error_json IS NULL
                """,
                (encoded, request_owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset replay request is absent")

    def close(self) -> None:
        with self._lock:
            self._database.close()


__all__ = [
    "CycleRecord",
    "ProgramCycleJournal",
    "RequestRecord",
    "SCHEMA_VERSION",
    "STATES",
]
