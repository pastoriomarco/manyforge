"""The single Program-cycle state machine, held in memory and written behind to SQLite."""

from __future__ import annotations

from collections.abc import Mapping
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import logging
import os
from pathlib import Path
import sqlite3
import stat
import unicodedata
from threading import RLock
from typing import Any, Callable, Final, Iterator

import rfc8785

from manyforge_headless_api.program_cycle import ScenePreparationFailureDTO

from .cycle_journal_writer import CYCLE_COLUMNS, CycleJournalWriter, JournalCommit

# The version of the file this runner creates; every start creates a new file.
# v12: `program_cycle_metadata.records_lost`, set by the writer when records of
# this start were not written.
SCHEMA_VERSION: Final = 12
# Bound on draining the writer and closing the file at shutdown.
CLOSE_TIMEOUT_SECONDS: Final = 1.0
# How many of the most recent completed persistent-value requests are kept.
MAXIMUM_COMPLETED_PERSISTENT_VALUE_REQUESTS: Final = 16_384
MAXIMUM_FINISHED_HISTORY_BYTES: Final = 256 * 1024 * 1024
_LOGGER = logging.getLogger(__name__)
_REQUIRED_TABLE_SQL: Final[dict[str, str]] = {
    "program_cycle_metadata": f"""
        CREATE TABLE program_cycle_metadata (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          schema_version INTEGER NOT NULL CHECK(schema_version={SCHEMA_VERSION}),
          records_lost INTEGER NOT NULL DEFAULT 0 CHECK(records_lost IN (0,1))
        )
    """,
    "program_cycle": """
        CREATE TABLE program_cycle (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          cycle_generation INTEGER NOT NULL CHECK(cycle_generation>0),
          state TEXT NOT NULL,
          program_run_id INTEGER
            CHECK(program_run_id IS NULL OR program_run_id>0),
          claim_json TEXT,
          capture_identities_json TEXT,
          terminal_reference TEXT,
          start_settlement_json TEXT,
          release_intent_json TEXT,
          reconciliation_required INTEGER NOT NULL
            CHECK(reconciliation_required IN (0,1)),
          acquisition_pending_json TEXT,
          execution_control_revision INTEGER NOT NULL
            CHECK(execution_control_revision>0),
          causal_request_key BLOB
            CHECK(causal_request_key IS NULL OR length(causal_request_key)=16),
          current_command_kind TEXT,
          failure_json TEXT,
          reset_phase TEXT,
          reset_effect_claim_json TEXT
        )
    """,
    "program_cycle_inflight_commands": """
        CREATE TABLE program_cycle_inflight_commands (
          request_key BLOB NOT NULL CHECK(length(request_key)=16) PRIMARY KEY,
          request_kind TEXT NOT NULL,
          intent_json TEXT NOT NULL,
          cycle_generation INTEGER NOT NULL
        )
    """,
    "program_run_history": """
        CREATE TABLE program_run_history (
          program_run_id INTEGER PRIMARY KEY CHECK(program_run_id>0),
          run_revision INTEGER NOT NULL CHECK(run_revision>0),
          snapshot_json TEXT NOT NULL,
          terminal INTEGER NOT NULL CHECK(terminal IN (0,1))
        )
    """,
    # Identities of completed persistent-value requests (Set, Reset, Clear),
    # so a resend is refused instead of applied again. Bounded: the oldest
    # rows by insertion `position` are trimmed. Node writes carry no request
    # and never appear here.
    "program_persistent_value_requests": """
        CREATE TABLE program_persistent_value_requests (
          request_key BLOB NOT NULL CHECK(length(request_key)=16) PRIMARY KEY,
          action TEXT NOT NULL CHECK(action IN ('set','reset','clear')),
          outcome TEXT NOT NULL CHECK(outcome IN ('applied')),
          position INTEGER NOT NULL UNIQUE CHECK(position>0)
        )
    """,
}
_SCHEMA_SQL: Final = (
    ";\n".join(statement.strip() for statement in _REQUIRED_TABLE_SQL.values()) + ";"
)
STATES: Final = frozenset(
    {
        "idle_after_reset",
        "starting",
        "running",
        "resetting",
        "terminal_awaiting_reset",
        "recovery_required",
    }
)
_REQUEST_KINDS: Final = frozenset(
    {
        "start",
        "reset",
        "runtime_blackboard_write",
        "manual_result_stage",
        "recovery_action",
        "resume_halt",
        "start_motion",
        "resume_motion",
        "prepare_manual_handover",
        "persistent_value_set",
        "persistent_value_reset",
        "persistent_values_clear",
    }
)
# The persistent-value action each request kind performs.
_PERSISTENT_VALUE_ACTIONS: Final = {
    "persistent_value_set": "set",
    "persistent_value_reset": "reset",
    "persistent_values_clear": "clear",
}
_PERSISTENT_VALUE_OUTCOMES: Final = frozenset({"applied"})
_TERMINAL_RUN_STATES: Final = frozenset({"stopped", "succeeded", "failed"})


def _bounded_identity(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and unicodedata.normalize("NFC", value) == value
        and len(value.encode("utf-8")) <= 256
    )


_JSON_INTEGER_LIMIT: Final = 9_007_199_254_740_991


def _is_request_key(value: object) -> bool:
    return type(value) is bytes and len(value) == 16


def _is_run_number(value: object) -> bool:
    return type(value) is int and 0 < value <= _JSON_INTEGER_LIMIT


def _settlement_reference(settlement: Mapping[str, Any]) -> str:
    """The terminal reference the runtime records for a Start settlement."""

    return "sha256:" + hashlib.sha256(rfc8785.dumps(dict(settlement))).hexdigest()


def _strict_object(
    value: str | None, *, required: bool = False
) -> dict[str, Any] | None:
    if value is None:
        if required:
            raise RuntimeError("Program-cycle journal JSON value is absent")
        return None

    def reject_constant(constant: str) -> None:
        raise ValueError(f"non-finite JSON value {constant}")

    def object_without_duplicates(
        pairs: list[tuple[str, Any]],
    ) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key}")
            result[key] = item
        return result

    try:
        decoded = json.loads(
            value,
            parse_constant=reject_constant,
            object_pairs_hook=object_without_duplicates,
        )
    except (TypeError, ValueError) as error:
        raise RuntimeError(
            f"Program-cycle journal contains invalid JSON: {error}"
        ) from error
    if not isinstance(decoded, dict):
        raise RuntimeError("Program-cycle journal JSON value is not an object")
    return decoded


def _prior_scene_preparation_failure(failure: Mapping[str, Any] | None) -> dict[str, Any] | None:
    if failure is None:
        return None
    prior = failure if failure.get("code") == "program_scene_preparation_failed" else failure.get("prior_scene_preparation_failure")
    if prior is None:
        return None
    return ScenePreparationFailureDTO.model_validate(prior).model_dump(mode="json")


@dataclass(frozen=True, slots=True)
class CycleRecord:
    cycle_generation: int
    state: str
    program_run_id: int | None
    claim: dict[str, Any] | None
    capture_identities: dict[str, Any] | None
    terminal_reference: str | None
    start_settlement: dict[str, Any] | None
    release_intent: dict[str, Any] | None
    reconciliation_required: bool
    acquisition_pending: dict[str, Any] | None
    execution_control_revision: int
    causal_request_key: bytes | None
    current_command_kind: str | None
    failure: dict[str, Any] | None
    reset_phase: str | None
    reset_effect_claim: dict[str, Any] | None = None


@dataclass(frozen=True, slots=True)
class RequestRecord:
    disposition: str
    result: dict[str, Any] | None = None
    error: dict[str, Any] | None = None


class _Stage:
    """One method's working copy, swapped in whole when the method returns."""

    __slots__ = ("cycle", "tables", "history_terminal_bytes", "operations")

    def __init__(self, journal: "ProgramCycleJournal") -> None:
        self.cycle = dict(journal._cycle)
        self.tables: dict[str, dict[Any, Any]] = {}
        self.history_terminal_bytes = journal._history_terminal_bytes
        self.operations: list[tuple[str, tuple[Any, ...]]] = []


class ProgramCycleJournal:
    """One-row cycle state plus a bounded set of unresolved commands.

    Completed persistent-value requests also keep their identities, bounded,
    so a resend is refused instead of applied again. The state lives in
    memory under one lock; every change is posted, in order, to one writer.
    """

    @staticmethod
    @contextmanager
    def _trusted_parent(path: Path) -> Iterator[int]:
        """Pin one pre-existing owner-held directory without repairing it."""

        parent = path.parent
        try:
            if parent.resolve(strict=True) != parent:
                raise RuntimeError(
                    "Program-cycle journal parent must be one canonical directory"
                )
            before = parent.lstat()
        except (OSError, RuntimeError) as error:
            raise RuntimeError(
                f"Program-cycle journal parent cannot be inspected: {error}"
            ) from error
        required_owner_access = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        if (
            not stat.S_ISDIR(before.st_mode)
            or before.st_uid != os.geteuid()
            or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or before.st_mode & required_owner_access != required_owner_access
        ):
            raise RuntimeError(
                "Program-cycle journal parent is not one safe owner-held directory"
            )
        try:
            descriptor = os.open(
                parent,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal parent cannot be opened: {error}"
            ) from error
        try:
            opened = os.fstat(descriptor)
            if (
                (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino)
                or not stat.S_ISDIR(opened.st_mode)
                or opened.st_uid != os.geteuid()
                or opened.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or opened.st_mode & required_owner_access != required_owner_access
            ):
                raise RuntimeError("Program-cycle journal parent changed while opening")
            yield descriptor
            after = parent.lstat()
            if (
                (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino)
                or not stat.S_ISDIR(after.st_mode)
                or after.st_uid != os.geteuid()
                or after.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or after.st_mode & required_owner_access != required_owner_access
            ):
                raise RuntimeError("Program-cycle journal parent changed during open")
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal parent changed during open: {error}"
            ) from error
        finally:
            os.close(descriptor)

    @staticmethod
    def _sidecar_metadata(
        parent_descriptor: int,
        name: str,
        *,
        label: str,
    ) -> os.stat_result | None:
        try:
            metadata = os.stat(
                name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            return None
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal {label} cannot be inspected: {error}"
            ) from error
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or metadata.st_nlink != 1
            or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or metadata.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
            != (stat.S_IRUSR | stat.S_IWUSR)
        ):
            raise RuntimeError(
                f"Program-cycle journal {label} is not one safe owner-held file"
            )
        return metadata

    def __init__(
        self,
        path: Path,
        *,
        maximum_inflight_commands: int = 4_096,
        maximum_retained_runs: int = 4_096,
        maximum_finished_history_bytes: int = MAXIMUM_FINISHED_HISTORY_BYTES,
        maximum_completed_persistent_value_requests: int = (
            MAXIMUM_COMPLETED_PERSISTENT_VALUE_REQUESTS
        ),
    ) -> None:
        if not 1 <= maximum_inflight_commands <= 4_096:
            raise ValueError("maximum_inflight_commands must be in [1, 4096]")
        self._maximum_inflight_commands = maximum_inflight_commands
        if (
            not 1
            <= maximum_completed_persistent_value_requests
            <= MAXIMUM_COMPLETED_PERSISTENT_VALUE_REQUESTS
        ):
            raise ValueError(
                "maximum_completed_persistent_value_requests must be in "
                f"[1, {MAXIMUM_COMPLETED_PERSISTENT_VALUE_REQUESTS}]"
            )
        self._maximum_completed_persistent_value_requests = (
            maximum_completed_persistent_value_requests
        )
        if not 1 <= maximum_retained_runs <= 4_096:
            raise ValueError("maximum_retained_runs must be in [1, 4096]")
        self._maximum_retained_runs = maximum_retained_runs
        if type(maximum_finished_history_bytes) is not int or maximum_finished_history_bytes < 1:
            raise ValueError("maximum_finished_history_bytes must be a positive integer")
        self._maximum_finished_history_bytes = maximum_finished_history_bytes
        self._oversize_current_run_logged: int | None = None
        self._lock = RLock()
        self._change_observer: Callable[[], None] | None = None
        path = Path(path)
        if not path.is_absolute() or path.name in {"", ".", ".."}:
            raise ValueError("Program-cycle journal path must be an absolute file")
        database: sqlite3.Connection | None = None
        parent_capability = -1
        parent_identity: tuple[int, int] | None = None
        try:
            with self._trusted_parent(path) as parent_descriptor:
                parent_metadata = os.fstat(parent_descriptor)
                parent_identity = (parent_metadata.st_dev, parent_metadata.st_ino)
                orphaned_sidecars: list[str] = []
                for suffix in ("-journal", "-wal", "-shm"):
                    sidecar_name = f"{path.name}{suffix}"
                    try:
                        os.stat(
                            sidecar_name,
                            dir_fd=parent_descriptor,
                            follow_symlinks=False,
                        )
                    except FileNotFoundError:
                        continue
                    except OSError as error:
                        raise RuntimeError(
                            "Program-cycle journal absence cannot be "
                            f"verified: {error}"
                        ) from error
                    orphaned_sidecars.append(sidecar_name)
                if orphaned_sidecars:
                    raise RuntimeError(
                        "Program-cycle journal main database is absent but "
                        "orphan sidecars remain: " + ", ".join(orphaned_sidecars)
                    )
                try:
                    descriptor = os.open(
                        path.name,
                        os.O_RDWR
                        | os.O_CREAT
                        | os.O_EXCL
                        | os.O_CLOEXEC
                        | os.O_NOFOLLOW,
                        0o600,
                        dir_fd=parent_descriptor,
                    )
                except OSError as error:
                    raise RuntimeError(
                        f"Program-cycle journal cannot be reserved: {error}"
                    ) from error
                try:
                    os.fchmod(descriptor, 0o600)
                    os.fsync(descriptor)
                    reserved = os.fstat(descriptor)
                    expected_identity = (reserved.st_dev, reserved.st_ino)
                finally:
                    os.close(descriptor)
                os.fsync(parent_descriptor)
                try:
                    current = os.stat(
                        path.name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    )
                except OSError as error:
                    raise RuntimeError(
                        f"Program-cycle journal changed after preflight: {error}"
                    ) from error
                if (
                    (current.st_dev, current.st_ino) != expected_identity
                    or not stat.S_ISREG(current.st_mode)
                    or current.st_uid != os.geteuid()
                    or current.st_nlink != 1
                    or current.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or current.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
                    != (stat.S_IRUSR | stat.S_IWUSR)
                ):
                    raise RuntimeError("Program-cycle journal changed after preflight")
                try:
                    parent_capability = os.dup(parent_descriptor)
                    descriptor_path = (
                        Path("/proc/self/fd") / str(parent_capability) / path.name
                    )
                    database = sqlite3.connect(
                        f"{descriptor_path.as_uri()}?mode=rw",
                        uri=True,
                        timeout=10.0,
                        isolation_level=None,
                        check_same_thread=False,
                    )
                except (OSError, sqlite3.Error, ValueError) as error:
                    raise RuntimeError(
                        f"Program-cycle journal cannot be opened: {error}"
                    ) from error
                try:
                    current = os.stat(
                        path.name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    )
                except OSError as error:
                    raise RuntimeError(
                        "Program-cycle journal changed while opening for use: "
                        f"{error}"
                    ) from error
                if (
                    (current.st_dev, current.st_ino) != expected_identity
                    or not stat.S_ISREG(current.st_mode)
                    or current.st_uid != os.geteuid()
                    or current.st_nlink != 1
                    or current.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or current.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
                    != (stat.S_IRUSR | stat.S_IWUSR)
                ):
                    raise RuntimeError(
                        "Program-cycle journal changed while opening for use"
                    )
        except BaseException:
            if database is not None:
                database.close()
            if parent_capability >= 0:
                os.close(parent_capability)
            raise
        if database is None or parent_capability < 0 or parent_identity is None:
            raise RuntimeError("Program-cycle journal did not open")
        self._path = path
        self._parent_descriptor = parent_capability
        self._parent_identity = parent_identity
        self._database_identity = expected_identity
        # The memory is the state; the file is a copy written behind by one writer.
        self._cycle: dict[str, Any] = dict.fromkeys(CYCLE_COLUMNS)
        self._cycle.update(
            cycle_generation=1,
            state="idle_after_reset",
            reconciliation_required=0,
            execution_control_revision=1,
        )
        self._inflight: dict[bytes, tuple[str, str, int]] = {}
        # Run id -> (run_revision, snapshot_json, terminal, UTF-8 byte length).
        self._history: dict[int, tuple[int, str, int, int]] = {}
        self._history_terminal_bytes = 0
        # Request key -> (action, outcome, position), in position order.
        self._persistent: dict[bytes, tuple[str, str, int]] = {}
        self._stage: _Stage | None = None
        self._sequence = 0
        self._closed = False
        try:
            database.execute("PRAGMA foreign_keys=ON")
            database.execute("PRAGMA journal_mode=WAL")
            database.execute(f"PRAGMA journal_size_limit={maximum_finished_history_bytes}")
            database.execute("PRAGMA synchronous=NORMAL")
            database.executescript(_SCHEMA_SQL)
            database.execute("BEGIN IMMEDIATE")
            database.execute(
                "INSERT INTO program_cycle_metadata VALUES(1,?,0)", (SCHEMA_VERSION,)
            )
            database.execute(
                "INSERT INTO program_cycle(singleton," + ",".join(CYCLE_COLUMNS) + ") "
                "VALUES(1," + ",".join("?" for _ in CYCLE_COLUMNS) + ")",
                self._cycle_row(self._cycle),
            )
            database.execute("COMMIT")
        except BaseException:
            database.close()
            os.close(self._parent_descriptor)
            self._parent_descriptor = -1
            raise
        self._writer = CycleJournalWriter(
            database,
            validate_storage=self._validate_storage,
            close_storage=self._close_parent,
        )

    @staticmethod
    def _safe_parent(metadata: os.stat_result) -> bool:
        required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        return (
            stat.S_ISDIR(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & required == required
        )

    @staticmethod
    def _safe_database(metadata: os.stat_result) -> bool:
        return (
            stat.S_ISREG(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and metadata.st_nlink == 1
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
            == (stat.S_IRUSR | stat.S_IWUSR)
        )

    def _close_parent(self) -> None:
        """Release the directory capability; called by the writer after the file closed."""

        os.close(self._parent_descriptor)
        self._parent_descriptor = -1

    def _validate_storage(self) -> None:
        """Check the file is still the one created; the writer runs it before each commit."""

        if self._parent_descriptor < 0:
            raise RuntimeError("Program-cycle journal is closed")
        try:
            opened_parent = os.fstat(self._parent_descriptor)
            current_parent = self._path.parent.lstat()
            current_database = os.stat(
                self._path.name,
                dir_fd=self._parent_descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal storage is unavailable: {error}"
            ) from error
        if (
            any(
                not self._safe_parent(metadata)
                or (metadata.st_dev, metadata.st_ino) != self._parent_identity
                for metadata in (opened_parent, current_parent)
            )
            or not self._safe_database(current_database)
            or (current_database.st_dev, current_database.st_ino)
            != self._database_identity
        ):
            raise RuntimeError(
                "Program-cycle journal storage capability was replaced or changed"
            )
        rollback = self._sidecar_metadata(
            self._parent_descriptor,
            f"{self._path.name}-journal",
            label="rollback sidecar",
        )
        if rollback is not None:
            raise RuntimeError(
                "Program-cycle journal has an unsupported rollback sidecar"
            )
        for suffix, label in (
            ("-wal", "WAL sidecar"),
            ("-shm", "shared-memory sidecar"),
        ):
            self._sidecar_metadata(
                self._parent_descriptor,
                f"{self._path.name}{suffix}",
                label=label,
            )

    def begin_acquire(
        self,
        *,
        expected_generation: int,
        provisional_run_id: int,
        evidence: Mapping[str, Any],
        causal_request_key: bytes | None = None,
    ) -> None:
        """Mark the inherently uncertain acquire/response checkpoint."""
        with self._transaction() as stage:
            encoded = self._dump(evidence)
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] == "idle_after_reset"
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is None
            ):
                raise RuntimeError("Program-cycle acquire checkpoint is stale")
            row.update(
                state="starting",
                program_run_id=provisional_run_id,
                acquisition_pending_json=encoded,
                execution_control_revision=row["execution_control_revision"] + 1,
                causal_request_key=causal_request_key,
                current_command_kind="program_run",
                failure_json=None,
                reset_phase=None,
            )

    def remember_acquired_claim(
        self,
        *,
        expected_generation: int,
        provisional_run_id: int,
        claim: Mapping[str, Any],
    ) -> None:
        """Record the acquired claim before further reads.

        In ``recovery_required`` this records the claim a Reset's exact
        acquire replay returned."""
        with self._transaction() as stage:
            encoded = self._dump(claim)
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] in {"starting", "recovery_required"}
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is not None
            ):
                raise RuntimeError("Program-cycle claim persistence is stale")
            row.update(
                program_run_id=provisional_run_id,
                claim_json=encoded,
                acquisition_pending_json=None,
            )

    def refresh_pending_acquire(
        self,
        *,
        expected_generation: int,
        provisional_run_id: int,
        evidence: Mapping[str, Any],
    ) -> None:
        """Replace pre-acquire evidence after scene preparation advanced admission."""

        with self._transaction() as stage:
            encoded = self._dump(evidence)
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] == "starting"
                and row["program_run_id"] == provisional_run_id
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is not None
            ):
                raise RuntimeError("Program-cycle acquire evidence is stale")
            row["acquisition_pending_json"] = encoded

    def mark_acquire_unknown(self, *, expected_generation: int) -> None:
        """Fail closed without inventing the claim hidden by response loss."""
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] == "starting"
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is not None
            ):
                raise RuntimeError("Program-cycle unknown acquire is stale")
            row.update(
                reconciliation_required=1,
                state="recovery_required",
                execution_control_revision=row["execution_control_revision"] + 1,
                failure_json='{"code":"program_cycle_acquire_outcome_unknown"}',
            )

    def cancel_rejected_acquire(self, *, expected_generation: int) -> None:
        """Clear a checkpoint only after an explicit native rejection packet.

        In ``recovery_required`` the rejection is a Reset's exact acquire
        replay; it also clears the latch and failure that the unknown set."""
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] in {"starting", "recovery_required"}
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is not None
            ):
                raise RuntimeError("Program-cycle rejected acquire is stale")
            row.update(
                state="idle_after_reset",
                program_run_id=None,
                acquisition_pending_json=None,
                execution_control_revision=row["execution_control_revision"] + 1,
                current_command_kind=None,
                reconciliation_required=0,
                failure_json=None,
            )

    @staticmethod
    def _cycle_row(cycle: Mapping[str, Any]) -> tuple[Any, ...]:
        return tuple(cycle[column] for column in CYCLE_COLUMNS)

    def _require_open(self) -> None:
        if self._closed:
            raise RuntimeError("Program-cycle journal is closed")

    def _row(self) -> dict[str, Any]:
        """The cycle row this thread sees: its own staged copy inside a method."""

        return self._cycle if self._stage is None else self._stage.cycle

    def _table(self, name: str, *, write: bool = False) -> dict[Any, Any]:
        """A row table as this thread sees it; ``write`` gives the staged copy."""

        stage = self._stage
        if stage is None:
            return getattr(self, name)
        table = stage.tables.get(name)
        if table is None:
            table = getattr(self, name)
            if write:
                table = dict(table)
                stage.tables[name] = table
        return table

    @contextmanager
    def _transaction(self) -> Iterator[_Stage]:
        """Change a staged copy; swap it in and post it only if the method returns."""

        with self._lock:
            self._require_open()
            stage = _Stage(self)
            self._stage = stage
            try:
                yield stage
            finally:
                self._stage = None
            cycle_row = None
            if stage.cycle != self._cycle:
                self._cycle = stage.cycle
                cycle_row = self._cycle_row(stage.cycle)
            for name, table in stage.tables.items():
                setattr(self, name, table)
            self._history_terminal_bytes = stage.history_terminal_bytes
            if cycle_row is not None or stage.operations:
                # Posted under the lock, so the file order is the state order.
                self._sequence += 1
                self._writer.post(
                    JournalCommit(self._sequence, cycle_row, tuple(stage.operations))
                )
        observer = self._change_observer
        if observer is not None:
            try:
                observer()
            except Exception:
                # Projection wakes are deliberately outside journal
                # correctness. The next owner transition/current snapshot
                # repairs a dropped notification without affecting state.
                pass

    def set_change_observer(self, observer: Callable[[], None] | None) -> None:
        """Install the single non-authoritative current-state wake callback."""

        if observer is not None and not callable(observer):
            raise TypeError("change observer must be callable")
        with self._lock:
            self._change_observer = observer

    @staticmethod
    def _load(value: str | None) -> dict[str, Any] | None:
        return _strict_object(value)

    @staticmethod
    def _dump(value: Mapping[str, Any] | None) -> str | None:
        return (
            None
            if value is None
            else json.dumps(
                dict(value),
                allow_nan=False,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            )
        )

    def read(self) -> CycleRecord:
        with self._lock:
            self._require_open()
            row = self._row()
        # Decoded from the stored text: new objects on every call.
        return CycleRecord(
            row["cycle_generation"],
            row["state"],
            row["program_run_id"],
            self._load(row["claim_json"]),
            self._load(row["capture_identities_json"]),
            row["terminal_reference"],
            self._load(row["start_settlement_json"]),
            self._load(row["release_intent_json"]),
            bool(row["reconciliation_required"]),
            self._load(row["acquisition_pending_json"]),
            row["execution_control_revision"],
            row["causal_request_key"],
            row["current_command_kind"],
            self._load(row["failure_json"]),
            row["reset_phase"],
            self._load(row["reset_effect_claim_json"]),
        )

    def remember_release_intent(
        self,
        *,
        expected_generation: int,
        claim_generation: int,
        release: Mapping[str, Any],
    ) -> None:
        """Bind the exact Reset release before invoking the Kernel."""
        encoded = self._dump(release)
        with self._transaction() as stage:
            row = stage.cycle
            claim = self._load(row["claim_json"])
            # The claim's generation compared as SQLite's integer cast would.
            stored_generation = None if claim is None else claim.get("claim_generation")
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] in {"terminal_awaiting_reset", "resetting"}
                and row["reconciliation_required"] == 0
                and isinstance(stored_generation, (int, float))
                and int(stored_generation) == claim_generation
                and row["release_intent_json"] in {None, encoded}
            ):
                raise RuntimeError("Program-cycle release intent is stale")
            row["release_intent_json"] = encoded

    def status_snapshot(self) -> tuple[CycleRecord, tuple[dict[str, Any], ...]]:
        """Read cycle and unresolved command evidence under one journal lock."""
        with self._lock:
            return self.read(), self.pending_commands()

    def pending_commands(self) -> tuple[dict[str, Any], ...]:
        """Bounded current unresolved evidence, never an execution/retry queue."""
        with self._lock:
            self._require_open()
            rows = sorted(self._table("_inflight").items())
        return tuple(
            {
                "request_key": request_key,
                "kind": kind,
                "intent": self._load(intent_json),
                "cycle_generation": generation,
            }
            for request_key, (kind, intent_json, generation) in rows
        )

    def _inflight_rows(self) -> dict[bytes, tuple[str, str, int]]:
        """A copy of the in-flight rows as stored (tests)."""

        with self._lock:
            return dict(self._inflight)

    def record_request(
        self,
        *,
        request_key: bytes,
        kind: str,
        intent: Mapping[str, Any],
        cycle_generation: int,
    ) -> RequestRecord:
        if kind not in _REQUEST_KINDS:
            raise ValueError("Program-cycle request kind is unsupported")
        encoded = self._dump(intent)
        with self._transaction() as stage:
            if kind in _PERSISTENT_VALUE_ACTIONS:
                completed = self._table("_persistent").get(request_key)
                if completed is not None:
                    # Already applied: refused by its identity, never again.
                    return RequestRecord(
                        "completed",
                        result={"action": completed[0], "outcome": completed[1]},
                    )
            inflight = self._table("_inflight")
            current = inflight.get(request_key)
            if current is not None:
                same_intent = current[1] == encoded
                if kind == "reset" and current[0] == kind:
                    # Missing selectors in a request mean off.
                    retained_intent = self._load(current[1])
                    compared_intent = dict(intent)
                    for key in ("reload_initial_scene", "reset_robot_position"):
                        retained_intent.setdefault(key, False)
                        compared_intent.setdefault(key, False)
                    same_intent = self._dump(retained_intent) == self._dump(compared_intent)
                if current[0] != kind or not same_intent:
                    raise ValueError(
                        "request identity was reused with different intent"
                    )
                return RequestRecord("in_progress")
            total = len(inflight)
            resets = sum(row[0] == "reset" for row in inflight.values())
            count = resets if kind == "reset" else total - resets
            # Exactly one Reset occurrence remains admissible when the
            # ordinary guard is full. An uncertain Reset must be continued,
            # not appended repeatedly into this reserved slot.
            capacity = 1 if kind == "reset" else self._maximum_inflight_commands
            if count >= capacity or resets > 1:
                raise OverflowError("Program-cycle in-flight command bound is full")
            self._table("_inflight", write=True)[request_key] = (
                kind,
                encoded,
                cycle_generation,
            )
            stage.operations.append(
                ("inflight_put", (request_key, kind, encoded, cycle_generation))
            )
        return RequestRecord("new")

    def reject_request(
        self,
        *,
        request_key: bytes,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        """Forget a proven no-effect command; rejection lives in status/history."""
        del code, message, details
        with self._transaction() as stage:
            if not self._delete_inflight(stage, request_key):
                raise RuntimeError("Program-cycle in-flight command is absent")

    def _delete_inflight(self, stage: _Stage, request_key: bytes) -> bool:
        if request_key not in self._table("_inflight"):
            return False
        del self._table("_inflight", write=True)[request_key]
        stage.operations.append(("inflight_delete", (request_key,)))
        return True

    def transition(
        self,
        *,
        expected_generation: int,
        expected_states: tuple[str, ...],
        state: str,
        program_run_id: int | None = None,
        claim: Mapping[str, Any] | None = None,
        capture_identities: Mapping[str, Any] | None = None,
        run_snapshot: Mapping[str, Any] | None = None,
        terminal_reference: str | None = None,
        start_settlement: Mapping[str, Any] | None = None,
        reconciliation_required: bool = False,
        causal_request_key: bytes | None = None,
        command_kind: str | None = None,
        failure: Mapping[str, Any] | None = None,
    ) -> None:
        if state not in STATES:
            raise ValueError("invalid Program-cycle state")
        if "terminal_awaiting_reset" in expected_states:
            raise ValueError("terminal Program-cycle evidence is immutable")
        with self._transaction() as stage:
            encoded_claim = self._dump(claim)
            encoded_capture = self._dump(capture_identities)
            encoded_settlement = self._dump(start_settlement)
            encoded_failure = self._dump(failure)
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] in expected_states
            ):
                raise RuntimeError("Program-cycle transition is stale")

            def kept(column: str, value: Any) -> Any:
                return row[column] if value is None else value

            row.update(
                state=state,
                program_run_id=kept("program_run_id", program_run_id),
                claim_json=kept("claim_json", encoded_claim),
                capture_identities_json=kept("capture_identities_json", encoded_capture),
                terminal_reference=kept("terminal_reference", terminal_reference),
                start_settlement_json=kept("start_settlement_json", encoded_settlement),
                reconciliation_required=int(reconciliation_required),
                acquisition_pending_json=None,
                execution_control_revision=row["execution_control_revision"] + 1,
                causal_request_key=kept("causal_request_key", causal_request_key),
                current_command_kind=kept("current_command_kind", command_kind),
                failure_json=(
                    encoded_failure if row["failure_json"] is None else row["failure_json"]
                ),
            )
            if run_snapshot is not None:
                self._retain_run_snapshot_locked(run_snapshot)

    @staticmethod
    def _run_snapshot_identity(snapshot: Mapping[str, Any]) -> tuple[int, int]:
        capture = snapshot.get("capture")
        program_run_id = (
            capture.get("program_run_id") if isinstance(capture, Mapping) else None
        )
        revision = snapshot.get("run_revision")
        if (
            not _is_run_number(program_run_id)
            or type(revision) is not int
            or revision < 1
        ):
            raise ValueError("Program-run history snapshot has invalid identity")
        return program_run_id, revision

    def _put_history(
        self, stage: _Stage, program_run_id: int, row: tuple[int, str, int, int]
    ) -> None:
        history = self._table("_history", write=True)
        previous = history.get(program_run_id)
        if previous is not None and previous[2]:
            stage.history_terminal_bytes -= previous[3]
        history[program_run_id] = row
        if row[2]:
            stage.history_terminal_bytes += row[3]
        stage.operations.append(("history_put", (program_run_id, *row[:3])))

    def _delete_history(self, stage: _Stage, program_run_id: int) -> None:
        removed = self._table("_history", write=True).pop(program_run_id)
        if removed[2]:
            stage.history_terminal_bytes -= removed[3]
        stage.operations.append(("history_delete", (program_run_id,)))

    def _oldest_finished_run(self, current_id: int | None) -> int | None:
        # The run id is the table's rowid, so the oldest row is the lowest id.
        return min(
            (
                program_run_id
                for program_run_id, row in self._table("_history").items()
                if row[2] and program_run_id != current_id
            ),
            default=None,
        )

    def _retain_run_snapshot_locked(self, snapshot: Mapping[str, Any]) -> None:
        stage = self._stage
        assert stage is not None
        program_run_id, revision = self._run_snapshot_identity(snapshot)
        encoded = self._dump(snapshot)
        assert encoded is not None
        terminal = int(snapshot.get("state") in _TERMINAL_RUN_STATES)
        stored = (revision, encoded, terminal, len(encoded.encode("utf-8")))
        history = self._table("_history")
        existing = history.get(program_run_id)
        if existing is None:
            if len(history) >= self._maximum_retained_runs:
                victim = self._oldest_finished_run(stage.cycle["program_run_id"])
                if victim is None:
                    raise OverflowError(
                        "Program-run history has no resolved row to rotate"
                    )
                self._delete_history(stage, victim)
            self._put_history(stage, program_run_id, stored)
            self._prune_finished_history_locked()
            return
        if existing[2] and encoded != existing[1]:
            raise RuntimeError("terminal Program-run history is immutable")
        if revision < existing[0] or (
            revision == existing[0] and encoded != existing[1]
        ):
            raise RuntimeError("Program-run history revision is divergent")
        if revision > existing[0]:
            self._put_history(stage, program_run_id, stored)
        self._prune_finished_history_locked()

    def _prune_finished_history_locked(self) -> None:
        """Bound finished snapshot payloads without deleting the current run."""
        stage = self._stage
        assert stage is not None
        current_id = stage.cycle["program_run_id"]
        while stage.history_terminal_bytes > self._maximum_finished_history_bytes:
            oldest = self._oldest_finished_run(current_id)
            if oldest is None:
                if current_id is not None and self._oversize_current_run_logged != current_id:
                    _LOGGER.warning("Current Program run history exceeds the finished-history budget: %s", current_id)
                    self._oversize_current_run_logged = current_id
                break
            self._delete_history(stage, oldest)

    def retain_run_snapshot(self, snapshot: Mapping[str, Any]) -> None:
        with self._transaction():
            self._retain_run_snapshot_locked(snapshot)

    def require_run_history_capacity(self) -> None:
        with self._lock:
            self._require_open()
            history = self._table("_history")
            full = len(history) >= self._maximum_retained_runs and not any(
                row[2] for row in history.values()
            )
        if full:
            raise OverflowError("Program-run history has no resolved row to rotate")

    def read_run_snapshot(self, program_run_id: int) -> dict[str, Any] | None:
        with self._lock:
            self._require_open()
            row = self._table("_history").get(program_run_id)
        return None if row is None else self._load(row[1])

    def list_run_snapshots(self, *, limit: int) -> tuple[list[dict[str, Any]], bool]:
        if type(limit) is not int or not 1 <= limit <= self._maximum_retained_runs:
            raise ValueError("Program-run history limit is invalid")
        with self._lock:
            self._require_open()
            history = self._table("_history")
            rows = [history[key][1] for key in sorted(history)[: limit + 1]]
        values = [self._load(row) for row in rows[:limit]]
        return [value for value in values if value is not None], len(rows) > limit

    def observe_terminal(
        self,
        *,
        expected_generation: int,
        program_run_id: int,
        terminal_reference: str,
        snapshot: Mapping[str, Any],
    ) -> bool:
        """Record one exact terminal fact, idempotently across observer races."""
        with self._transaction() as stage:
            row = stage.cycle
            if (
                row["cycle_generation"] != expected_generation
                or row["program_run_id"] != program_run_id
            ):
                return False
            if row["state"] == "terminal_awaiting_reset":
                if row["terminal_reference"] != terminal_reference:
                    raise RuntimeError("terminal Program-cycle evidence conflicts")
                self._retain_run_snapshot_locked(snapshot)
                return False
            if row["state"] != "running":
                return False
            self._retain_run_snapshot_locked(snapshot)
            row.update(
                state="terminal_awaiting_reset",
                terminal_reference=terminal_reference,
                execution_control_revision=row["execution_control_revision"] + 1,
            )
            return True

    def remember_reset_effect_claim(
        self, *, expected_generation: int, expected: Mapping[str, Any] | None,
        retained: Mapping[str, Any],
    ) -> None:
        """Record exact acquisition/receipt/release before crossing the owner boundary."""
        encoded = self._dump(retained)
        previous = None if expected is None else self._dump(expected)
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] == "resetting"
                and row["claim_json"] is None
                and row["reset_effect_claim_json"] == previous
            ):
                raise RuntimeError("Program Reset effect claim register is stale")
            row.update(
                reset_effect_claim_json=encoded,
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    def begin_idle_resetting(
        self,
        *,
        expected_generation: int,
        causal_request_key: bytes,
        phase: str,
    ) -> None:
        """Fence explicit no-run Reset effects with the existing lifecycle."""
        if phase not in {"restoring_scene", "restoring_position"}:
            raise ValueError("Idle Reset must begin with a selected effect")
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] == "idle_after_reset"
                and row["program_run_id"] is None
                and row["claim_json"] is None
                and row["acquisition_pending_json"] is None
                and row["reset_effect_claim_json"] is None
                and row["reconciliation_required"] == 0
            ):
                raise RuntimeError("Idle Program-cycle Reset transition is stale")
            row.update(
                state="resetting",
                reset_phase=phase,
                execution_control_revision=row["execution_control_revision"] + 1,
                causal_request_key=causal_request_key,
                current_command_kind="program_reset",
            )

    def begin_resetting(
        self,
        *,
        expected_generation: int,
        program_run_id: int,
        causal_request_key: bytes,
        expected_recovery: CycleRecord | None = None,
    ) -> bool:
        """Atomically close ordinary admission before Reset performs Stop."""

        with self._transaction() as stage:
            if expected_recovery is not None and (
                self.read() != expected_recovery
                or not expected_recovery.reconciliation_required
                or expected_recovery.state
                not in {"recovery_required", "terminal_awaiting_reset"}
                or expected_recovery.reset_phase not in {None, "stopping", "settling"}
                or expected_recovery.acquisition_pending is not None
                or expected_recovery.release_intent is not None
                or expected_recovery.claim is None
            ):
                raise RuntimeError("Program-cycle recovery Reset is stale")
            row = stage.cycle
            current = (
                None
                if row["cycle_generation"] != expected_generation
                else (row["state"], row["program_run_id"], row["causal_request_key"])
            )
            if current == ("resetting", program_run_id, causal_request_key):
                return False
            eligible_states = {"running", "terminal_awaiting_reset"}
            if expected_recovery is not None:
                eligible_states.add("recovery_required")
            if (
                current is None
                or current[0] not in eligible_states
                or current[1] != program_run_id
            ):
                raise RuntimeError("Program-cycle Reset transition is stale")
            row.update(
                state="resetting",
                reset_phase="stopping",
                execution_control_revision=row["execution_control_revision"] + 1,
                causal_request_key=causal_request_key,
                current_command_kind="program_reset",
                failure_json=None,
            )
            return True

    def settle_reset_recovery(
        self,
        *,
        expected: CycleRecord,
        phase: str = "restoring_scene",
    ) -> None:
        """Persist an externally proved containment join, not guessed outcomes."""
        if (
            expected.state != "resetting"
            or expected.reset_phase != "settling"
            or not expected.reconciliation_required
            or expected.claim is None
            or expected.acquisition_pending is not None
            or expected.release_intent is not None
            or phase not in {"restoring_scene", "restoring_position", "releasing"}
        ):
            raise RuntimeError("Program-cycle containment discharge is invalid")
        with self._transaction() as stage:
            if self.read() != expected:
                raise RuntimeError("Program-cycle containment discharge is stale")
            row = stage.cycle
            row.update(
                reset_phase=phase,
                reconciliation_required=0,
                failure_json=None,
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    def update_reset_phase(
        self,
        *,
        expected_generation: int,
        program_run_id: int | None,
        expected_phase: str,
        phase: str,
        terminal_reference: str | None = None,
        scene_preparation_completed: bool = False,
    ) -> None:
        for value in (expected_phase, phase):
            if not _bounded_identity(value):
                raise ValueError("Reset phase must be bounded non-empty NFC text")
        if scene_preparation_completed and (
            expected_phase != "restoring_scene" or phase not in {"restoring_position", "releasing"}
        ):
            raise ValueError("Scene completion requires its exact next Reset phase")
        with self._transaction() as stage:
            row = stage.cycle
            encoded_failure = row["failure_json"]
            if scene_preparation_completed:
                current = self.read()
                failure = current.failure
                if failure is not None and failure.get("code") == "program_scene_preparation_failed":
                    failure = None
                elif failure is not None and "prior_scene_preparation_failure" in failure:
                    failure = {key: value for key, value in failure.items() if key != "prior_scene_preparation_failure"}
                encoded_failure = None if failure is None else self._dump(failure)
            if not (
                row["cycle_generation"] == expected_generation
                and row["program_run_id"] == program_run_id
                and row["state"] == "resetting"
                and row["reset_phase"] == expected_phase
            ):
                raise RuntimeError("Program-cycle Reset phase is stale")
            row.update(
                failure_json=encoded_failure,
                reset_phase=phase,
                terminal_reference=(
                    row["terminal_reference"]
                    if terminal_reference is None
                    else terminal_reference
                ),
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    def fail_reset(
        self,
        *,
        expected_generation: int,
        program_run_id: int | None,
        code: str,
        message: str,
        reconciliation_required: bool = False,
    ) -> None:
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["program_run_id"] == program_run_id
                and row["state"] == "resetting"
                and _bounded_identity(row["reset_phase"])
            ):
                raise RuntimeError("Program-cycle failed Reset transition is stale")
            failure = {
                    "code": code,
                    "message": message,
                    "failed_phase": row["reset_phase"],
                }
            prior = _prior_scene_preparation_failure(self._load(row["failure_json"]))
            if prior is not None:
                failure["prior_scene_preparation_failure"] = prior
            row.update(
                state="recovery_required",
                failure_json=self._dump(failure),
                reconciliation_required=max(
                    row["reconciliation_required"], int(reconciliation_required)
                ),
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    def resume_resetting(
        self,
        *,
        expected_generation: int,
        program_run_id: int | None,
        expected_failure_code: str,
        phase: str,
        causal_request_key: bytes,
        allow_reconciliation: bool = False,
    ) -> None:
        """Resume only the exact failed Reset phase under a fresh command.

        ``allow_reconciliation``: the phase first settles its unknown Kernel
        exchange (receipt read or exact replay), so the latch may be set."""

        for value in (expected_failure_code, phase):
            if not _bounded_identity(value):
                raise ValueError("Reset recovery identity must be bounded text")
        if not _is_request_key(causal_request_key):
            raise ValueError("Reset recovery request key must be 16 bytes")
        with self._transaction() as stage:
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["program_run_id"] == program_run_id
                and row["state"] == "recovery_required"
            ):
                raise RuntimeError("Program-cycle Reset recovery is stale")
            failure = self._load(row["failure_json"])
            failed_phase = (
                failure.get("failed_phase") if isinstance(failure, dict) else None
            )
            latched = bool(row["reconciliation_required"])
            if (
                not isinstance(failure, dict)
                or failure.get("code") != expected_failure_code
                or (latched and not allow_reconciliation)
                or row["reset_phase"] != phase
                or (
                    failed_phase != phase
                    # A latched run release records no failed phase.
                    and not (
                        allow_reconciliation and latched
                        and phase == "releasing" and failed_phase is None
                    )
                )
            ):
                raise RuntimeError("Program-cycle Reset recovery is not retryable")
            prior = _prior_scene_preparation_failure(failure)
            row.update(
                state="resetting",
                reset_phase=phase,
                reconciliation_required=0,
                failure_json=None if prior is None else self._dump(prior),
                execution_control_revision=row["execution_control_revision"] + 1,
                causal_request_key=causal_request_key,
                current_command_kind="program_reset",
            )

    def record_scene_preparation_failure(
        self, *, expected_generation: int, failure: Mapping[str, Any]
    ) -> None:
        if failure.get("code") != "program_scene_preparation_failed":
            raise ValueError("invalid Scene preparation failure")
        encoded = self._dump(failure)
        with self._transaction() as stage:
            row = stage.cycle
            if not self._is_clean_idle(row, expected_generation):
                raise RuntimeError("Scene preparation failure is stale")
            row.update(
                failure_json=row["failure_json"] if row["failure_json"] is not None else encoded,
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    @staticmethod
    def _is_clean_idle(row: Mapping[str, Any], expected_generation: int) -> bool:
        return (
            row["cycle_generation"] == expected_generation
            and row["state"] == "idle_after_reset"
            and row["claim_json"] is None
            and row["program_run_id"] is None
            and row["acquisition_pending_json"] is None
            and row["reconciliation_required"] == 0
        )

    def complete_scene_preparation(self, *, expected_generation: int) -> None:
        with self._transaction() as stage:
            current = self.read()
            failure = current.failure
            if failure is not None and failure.get("code") != "program_scene_preparation_failed":
                raise RuntimeError("Scene preparation cannot clear another failure")
            row = stage.cycle
            if not self._is_clean_idle(row, expected_generation):
                raise RuntimeError("Scene preparation completion is stale")
            row.update(
                failure_json=None,
                execution_control_revision=row["execution_control_revision"] + 1,
            )

    def publish_idle_reset(
        self, *, expected_generation: int, request_key: bytes,
        result: Mapping[str, Any]
    ) -> None:
        del result
        with self._transaction() as stage:
            request = self._table("_inflight").get(request_key)
            if request is None or request[0] != "reset" or request[2] != expected_generation:
                raise RuntimeError("Idle Reset in-flight request is absent")
            row = stage.cycle
            if not self._is_clean_idle(row, expected_generation):
                raise RuntimeError("Idle Reset completion is stale")
            row.update(
                cycle_generation=row["cycle_generation"] + 1,
                execution_control_revision=row["execution_control_revision"] + 1,
            )
            self._delete_inflight(stage, request_key)

    def require_reconciliation(
        self,
        *,
        expected_generation: int,
        failure: Mapping[str, Any] | None = None,
    ) -> None:
        """Latch an unresolved cross-owner effect without changing owner facts."""
        retained_failure = self._dump(
            failure or {"code": "program_cycle_reconciliation_required"}
        )
        with self._transaction() as stage:
            row = stage.cycle
            if row["cycle_generation"] != expected_generation:
                raise RuntimeError("Program-cycle reconciliation latch is stale")
            row.update(
                reconciliation_required=1,
                state="recovery_required",
                execution_control_revision=row["execution_control_revision"] + 1,
                failure_json=(
                    retained_failure if row["failure_json"] is None else row["failure_json"]
                ),
            )

    def complete_request(
        self,
        *,
        request_key: bytes,
        result: Mapping[str, Any],
    ) -> None:
        del result
        with self._transaction() as stage:
            if not self._delete_inflight(stage, request_key):
                raise RuntimeError("Program-cycle in-flight command is absent")

    def complete_persistent_value_request(
        self,
        *,
        request_key: bytes,
        outcome: str,
    ) -> None:
        """Resolve one persistent-value request and keep its identity.

        The identity joins the bounded completed-request table in the same
        change, so a resend is refused; the oldest identities beyond the
        bound are trimmed.
        """

        if outcome not in _PERSISTENT_VALUE_OUTCOMES:
            raise ValueError("persistent-value request outcome is unsupported")
        with self._transaction() as stage:
            request = self._table("_inflight").get(request_key)
            if request is None or request[0] not in _PERSISTENT_VALUE_ACTIONS:
                raise RuntimeError("Program-cycle in-flight command is absent")
            self._delete_inflight(stage, request_key)
            completed = self._table("_persistent", write=True)
            # Positions only grow, so the last row holds the largest.
            position = (
                next(reversed(completed.values()))[2] + 1 if completed else 1
            )
            action = _PERSISTENT_VALUE_ACTIONS[request[0]]
            completed[request_key] = (action, outcome, position)
            stage.operations.append(
                ("persistent_insert", (request_key, action, outcome, position))
            )
            trim_at_or_below = position - self._maximum_completed_persistent_value_requests
            oldest = [
                key for key, row in completed.items() if row[2] <= trim_at_or_below
            ]
            for key in oldest:
                del completed[key]
            if oldest:
                stage.operations.append(("persistent_trim", (trim_at_or_below,)))

    def complete_control_request(
        self,
        *,
        request_key: bytes,
        result: Mapping[str, Any],
        run_snapshot: Mapping[str, Any],
        advance_execution_control: bool = False,
    ) -> None:
        """Atomically retain one runtime-control effect's exact updated run view.

        ``advance_execution_control`` advances the execution-control revision
        once in the same change; an accepted release of a failure hold
        uses it so the release is a later accepted control revision.
        """

        del result
        with self._transaction() as stage:
            self._retain_run_snapshot_locked(run_snapshot)
            if advance_execution_control:
                row = stage.cycle
                row["execution_control_revision"] = row["execution_control_revision"] + 1
            if not self._delete_inflight(stage, request_key):
                raise RuntimeError("Program-cycle in-flight command is absent")

    def publish_reset(
        self,
        *,
        expected_generation: int,
        request_key: bytes,
        result: Mapping[str, Any],
    ) -> None:
        del result
        with self._transaction() as stage:
            request = self._table("_inflight").get(request_key)
            if request is None or request[2] != expected_generation:
                raise RuntimeError("Program-cycle Reset in-flight command is absent")
            prior = (
                None if self._load(request[1]).get("reload_initial_scene", False)
                else _prior_scene_preparation_failure(self.read().failure)
            )
            encoded_prior = None if prior is None else self._dump(prior)
            row = stage.cycle
            if not (
                row["cycle_generation"] == expected_generation
                and row["state"] in {"terminal_awaiting_reset", "resetting"}
            ):
                raise RuntimeError("Program-cycle Reset is stale")
            row.update(
                cycle_generation=row["cycle_generation"] + 1,
                state="idle_after_reset",
                program_run_id=None,
                claim_json=None,
                capture_identities_json=None,
                terminal_reference=None,
                start_settlement_json=None,
                release_intent_json=None,
                reconciliation_required=0,
                acquisition_pending_json=None,
                execution_control_revision=row["execution_control_revision"] + 1,
                current_command_kind=None,
                failure_json=encoded_prior,
                reset_phase=None,
                reset_effect_claim_json=None,
            )
            # Reaching the successor Idle cycle resolves every command from the
            # predecessor generation. Keeping any of those identities would
            # turn a bounded in-flight guard back into a lifetime ledger.
            for key in [
                key
                for key, inflight in self._table("_inflight").items()
                if inflight[2] == expected_generation
            ]:
                self._delete_inflight(stage, key)
            self._prune_finished_history_locked()

    def close(self, timeout_s: float = CLOSE_TIMEOUT_SECONDS) -> None:
        """Stop changes, let the writer drain and close the file, bounded by ``timeout_s``.

        On timeout the records not yet written are lost and the file keeps its WAL.
        """

        with self._lock:
            if self._closed:
                return
            self._closed = True
        self._writer.close(timeout_s)


__all__ = [
    "CycleRecord",
    "MAXIMUM_COMPLETED_PERSISTENT_VALUE_REQUESTS",
    "ProgramCycleJournal",
    "RequestRecord",
    "SCHEMA_VERSION",
    "STATES",
]
