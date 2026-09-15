"""Durable SQLite journal for the single Program-cycle state machine."""

from __future__ import annotations

from collections.abc import Mapping
from contextlib import ExitStack, contextmanager
from copy import deepcopy
from dataclasses import dataclass
import json
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
import unicodedata
from threading import RLock
from typing import Any, Callable, Final, Iterator

from .liveness import RetainedLiveness


LEGACY_SCHEMA_VERSION: Final = 3
SCHEMA_VERSION: Final = 4
_LEGACY_TABLE_SHAPES: Final[dict[str, tuple[tuple[str, str, int, int], ...]]] = {
    "program_cycle_metadata": (
        ("singleton", "INTEGER", 0, 1),
        ("schema_version", "INTEGER", 1, 0),
    ),
    "program_cycle": (
        ("singleton", "INTEGER", 0, 1),
        ("cycle_generation", "INTEGER", 1, 0),
        ("state", "TEXT", 1, 0),
        ("owner_epoch", "TEXT", 1, 0),
        ("program_run_id", "TEXT", 0, 0),
        ("claim_json", "TEXT", 0, 0),
        ("capture_identities_json", "TEXT", 0, 0),
        ("terminal_reference", "TEXT", 0, 0),
        ("start_settlement_json", "TEXT", 0, 0),
        ("release_intent_json", "TEXT", 0, 0),
        ("reset_result_json", "TEXT", 0, 0),
        ("reconciliation_required", "INTEGER", 1, 0),
        ("acquisition_pending_json", "TEXT", 0, 0),
        ("reconciliation_evidence_digest", "TEXT", 0, 0),
    ),
    "program_cycle_requests": (
        ("request_owner_epoch", "TEXT", 1, 1),
        ("request_id", "TEXT", 1, 2),
        ("request_kind", "TEXT", 1, 0),
        ("intent_json", "TEXT", 1, 0),
        ("cycle_generation", "INTEGER", 1, 0),
        ("result_json", "TEXT", 0, 0),
        ("error_json", "TEXT", 0, 0),
    ),
    "program_runner_liveness": (
        ("singleton", "INTEGER", 0, 1),
        ("manager_runtime_epoch", "TEXT", 0, 0),
        ("manager_owner_epoch", "TEXT", 0, 0),
        ("predecessor_expired", "INTEGER", 1, 0),
        ("fault_reference", "TEXT", 0, 0),
    ),
    "program_run_history": (
        ("program_run_id", "TEXT", 0, 1),
        ("run_revision", "INTEGER", 1, 0),
        ("snapshot_json", "TEXT", 1, 0),
        ("terminal", "INTEGER", 1, 0),
    ),
}
_LEGACY_TABLE_SQL: Final[dict[str, str]] = {
    "program_cycle_metadata": f"""
        CREATE TABLE program_cycle_metadata (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          schema_version INTEGER NOT NULL CHECK(schema_version={LEGACY_SCHEMA_VERSION})
        )
    """,
    "program_cycle": """
        CREATE TABLE program_cycle (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          cycle_generation INTEGER NOT NULL CHECK(cycle_generation>0),
          state TEXT NOT NULL,
          owner_epoch TEXT NOT NULL,
          program_run_id TEXT,
          claim_json TEXT,
          capture_identities_json TEXT,
          terminal_reference TEXT,
          start_settlement_json TEXT,
          release_intent_json TEXT,
          reset_result_json TEXT,
          reconciliation_required INTEGER NOT NULL
            CHECK(reconciliation_required IN (0,1)),
          acquisition_pending_json TEXT,
          reconciliation_evidence_digest TEXT
        )
    """,
    "program_cycle_requests": """
        CREATE TABLE program_cycle_requests (
          request_owner_epoch TEXT NOT NULL,
          request_id TEXT NOT NULL,
          request_kind TEXT NOT NULL,
          intent_json TEXT NOT NULL,
          cycle_generation INTEGER NOT NULL,
          result_json TEXT,
          error_json TEXT,
          PRIMARY KEY(request_owner_epoch, request_id)
        )
    """,
    "program_runner_liveness": """
        CREATE TABLE program_runner_liveness (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          manager_runtime_epoch TEXT,
          manager_owner_epoch TEXT,
          predecessor_expired INTEGER NOT NULL
            CHECK(predecessor_expired IN (0,1)),
          fault_reference TEXT
        )
    """,
    "program_run_history": """
        CREATE TABLE program_run_history (
          program_run_id TEXT PRIMARY KEY,
          run_revision INTEGER NOT NULL CHECK(run_revision>0),
          snapshot_json TEXT NOT NULL,
          terminal INTEGER NOT NULL CHECK(terminal IN (0,1))
        )
    """,
}
_REQUIRED_TABLE_SHAPES: Final[dict[str, tuple[tuple[str, str, int, int], ...]]] = {
    "program_cycle_metadata": (
        ("singleton", "INTEGER", 0, 1),
        ("schema_version", "INTEGER", 1, 0),
    ),
    "program_cycle": (
        ("singleton", "INTEGER", 0, 1),
        ("cycle_generation", "INTEGER", 1, 0),
        ("state", "TEXT", 1, 0),
        ("owner_epoch", "TEXT", 1, 0),
        ("program_run_id", "TEXT", 0, 0),
        ("claim_json", "TEXT", 0, 0),
        ("capture_identities_json", "TEXT", 0, 0),
        ("terminal_reference", "TEXT", 0, 0),
        ("start_settlement_json", "TEXT", 0, 0),
        ("release_intent_json", "TEXT", 0, 0),
        ("reset_result_json", "TEXT", 0, 0),
        ("reconciliation_required", "INTEGER", 1, 0),
        ("acquisition_pending_json", "TEXT", 0, 0),
        ("reconciliation_evidence_digest", "TEXT", 0, 0),
        ("execution_control_revision", "INTEGER", 1, 0),
        ("causal_correlation_id", "TEXT", 0, 0),
        ("causal_session_id", "TEXT", 0, 0),
        ("causal_client_sequence", "TEXT", 0, 0),
        ("current_command_kind", "TEXT", 0, 0),
        ("failure_json", "TEXT", 0, 0),
        ("reset_phase", "TEXT", 0, 0),
        ("runtime_controls_json", "TEXT", 0, 0),
    ),
    "program_cycle_inflight_commands": (
        ("request_owner_epoch", "TEXT", 1, 1),
        ("request_id", "TEXT", 1, 2),
        ("request_kind", "TEXT", 1, 0),
        ("intent_json", "TEXT", 1, 0),
        ("cycle_generation", "INTEGER", 1, 0),
    ),
    "program_runner_liveness": _LEGACY_TABLE_SHAPES["program_runner_liveness"],
    "program_run_history": _LEGACY_TABLE_SHAPES["program_run_history"],
}
_REQUIRED_TABLE_SQL: Final[dict[str, str]] = {
    "program_cycle_metadata": f"""
        CREATE TABLE "program_cycle_metadata" (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          schema_version INTEGER NOT NULL CHECK(schema_version={SCHEMA_VERSION})
        )
    """,
    "program_cycle": """
        CREATE TABLE "program_cycle" (
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          cycle_generation INTEGER NOT NULL CHECK(cycle_generation>0),
          state TEXT NOT NULL,
          owner_epoch TEXT NOT NULL,
          program_run_id TEXT,
          claim_json TEXT,
          capture_identities_json TEXT,
          terminal_reference TEXT,
          start_settlement_json TEXT,
          release_intent_json TEXT,
          reset_result_json TEXT,
          reconciliation_required INTEGER NOT NULL
            CHECK(reconciliation_required IN (0,1)),
          acquisition_pending_json TEXT,
          reconciliation_evidence_digest TEXT,
          execution_control_revision INTEGER NOT NULL
            CHECK(execution_control_revision>0),
          causal_correlation_id TEXT,
          causal_session_id TEXT,
          causal_client_sequence TEXT,
          current_command_kind TEXT,
          failure_json TEXT,
          reset_phase TEXT,
          runtime_controls_json TEXT
        )
    """,
    "program_cycle_inflight_commands": """
        CREATE TABLE program_cycle_inflight_commands (
          request_owner_epoch TEXT NOT NULL,
          request_id TEXT NOT NULL,
          request_kind TEXT NOT NULL,
          intent_json TEXT NOT NULL,
          cycle_generation INTEGER NOT NULL,
          PRIMARY KEY(request_owner_epoch, request_id)
        )
    """,
    "program_runner_liveness": _LEGACY_TABLE_SQL["program_runner_liveness"],
    "program_run_history": _LEGACY_TABLE_SQL["program_run_history"],
}
_SCHEMA_SQL: Final = (
    ";\n".join(statement.strip() for statement in _REQUIRED_TABLE_SQL.values()) + ";"
)
STATES: Final = frozenset(
    {
        "idle_after_reset",
        "starting",
        "running",
        "stopping",
        "resetting",
        "terminal_awaiting_reset",
        "recovery_required",
    }
)
_REQUEST_KINDS: Final = frozenset(
    {
        "start",
        "reset",
        "runtime_override_stage",
        "runtime_override_activate",
        "runtime_override_clear",
        "runtime_override_promote",
        "runtime_blackboard_write",
        "manual_result_stage",
        "recovery_action",
        "resume_full_stop",
        "resume_motion",
        "prepare_manual_handover",
        "discard_full_stop",
    }
)
_TERMINAL_RUN_STATES: Final = frozenset({"stopped", "succeeded", "failed"})


def _normalized_sql(value: str) -> str:
    return " ".join(value.split())


def _bounded_identity(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and unicodedata.normalize("NFC", value) == value
        and len(value.encode("utf-8")) <= 256
    )


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


@dataclass(frozen=True, slots=True)
class CycleRecord:
    cycle_generation: int
    state: str
    owner_epoch: str
    program_run_id: str | None
    claim: dict[str, Any] | None
    capture_identities: dict[str, Any] | None
    terminal_reference: str | None
    start_settlement: dict[str, Any] | None
    release_intent: dict[str, Any] | None
    reset_result: dict[str, Any] | None
    reconciliation_required: bool
    acquisition_pending: dict[str, Any] | None
    reconciliation_evidence_digest: str | None
    execution_control_revision: int
    causal_correlation_id: str | None
    causal_session_id: str | None
    causal_client_sequence: str | None
    current_command_kind: str | None
    failure: dict[str, Any] | None
    reset_phase: str | None
    runtime_controls: dict[str, Any] | None


@dataclass(frozen=True, slots=True)
class RequestRecord:
    disposition: str
    result: dict[str, Any] | None = None
    error: dict[str, Any] | None = None


class ProgramCycleJournal:
    """One-row cycle state plus a bounded set of unresolved commands."""

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

    @staticmethod
    def _copy_open_file(source: int, destination: Path) -> None:
        destination_descriptor = os.open(
            destination,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            os.lseek(source, 0, os.SEEK_SET)
            while True:
                chunk = os.read(source, 1024 * 1024)
                if not chunk:
                    break
                remaining = memoryview(chunk)
                while remaining:
                    written = os.write(destination_descriptor, remaining)
                    if written <= 0:
                        raise RuntimeError(
                            "Program-cycle journal snapshot write stalled"
                        )
                    remaining = remaining[written:]
        finally:
            os.close(destination_descriptor)

    @staticmethod
    @contextmanager
    def _read_only_snapshot(
        path: Path,
        *,
        parent_descriptor: int,
        main_metadata: os.stat_result,
    ) -> Iterator[str]:
        """Expose the logical database without mutating its live files."""

        rollback_name = f"{path.name}-journal"
        try:
            os.stat(
                rollback_name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            pass
        except OSError as error:
            raise RuntimeError(
                "Program-cycle rollback journal cannot be inspected: " f"{error}"
            ) from error
        else:
            raise RuntimeError(
                "Program-cycle journal has an unsupported rollback sidecar"
            )

        wal_name = f"{path.name}-wal"
        shm_name = f"{path.name}-shm"
        wal_metadata = ProgramCycleJournal._sidecar_metadata(
            parent_descriptor,
            wal_name,
            label="WAL sidecar",
        )
        shm_metadata = ProgramCycleJournal._sidecar_metadata(
            parent_descriptor,
            shm_name,
            label="shared-memory sidecar",
        )
        if wal_metadata is None and shm_metadata is not None:
            raise RuntimeError("Program-cycle journal has shared memory without a WAL")

        main_descriptor = -1
        wal_descriptor = -1
        try:
            main_descriptor = os.open(
                path.name,
                os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=parent_descriptor,
            )
            opened_main = os.fstat(main_descriptor)
            if (opened_main.st_dev, opened_main.st_ino) != (
                main_metadata.st_dev,
                main_metadata.st_ino,
            ) or (opened_main.st_size, opened_main.st_mtime_ns) != (
                main_metadata.st_size,
                main_metadata.st_mtime_ns,
            ):
                raise RuntimeError("Program-cycle journal changed before snapshot")
            opened_wal: os.stat_result | None = None
            if wal_metadata is not None:
                wal_descriptor = os.open(
                    wal_name,
                    os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=parent_descriptor,
                )
                opened_wal = os.fstat(wal_descriptor)
                if (opened_wal.st_dev, opened_wal.st_ino) != (
                    wal_metadata.st_dev,
                    wal_metadata.st_ino,
                ) or (opened_wal.st_size, opened_wal.st_mtime_ns) != (
                    wal_metadata.st_size,
                    wal_metadata.st_mtime_ns,
                ):
                    raise RuntimeError(
                        "Program-cycle journal WAL changed before snapshot"
                    )
            with tempfile.TemporaryDirectory(
                prefix="manyforge-cycle-preflight-"
            ) as temporary_root:
                snapshot = Path(temporary_root) / path.name
                ProgramCycleJournal._copy_open_file(main_descriptor, snapshot)
                if wal_descriptor >= 0:
                    ProgramCycleJournal._copy_open_file(
                        wal_descriptor,
                        Path(f"{snapshot}-wal"),
                    )
                after_main = os.fstat(main_descriptor)
                after_wal = os.fstat(wal_descriptor) if wal_descriptor >= 0 else None
                current_main = ProgramCycleJournal._sidecar_metadata(
                    parent_descriptor,
                    path.name,
                    label="database",
                )
                current_wal = ProgramCycleJournal._sidecar_metadata(
                    parent_descriptor,
                    wal_name,
                    label="WAL sidecar",
                )
                current_shm = ProgramCycleJournal._sidecar_metadata(
                    parent_descriptor,
                    shm_name,
                    label="shared-memory sidecar",
                )
                try:
                    os.stat(
                        rollback_name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    rollback_appeared = False
                except OSError as error:
                    raise RuntimeError(
                        "Program-cycle rollback journal changed during snapshot: "
                        f"{error}"
                    ) from error
                else:
                    rollback_appeared = True
                if (
                    rollback_appeared
                    or current_main is None
                    or (
                        after_main.st_dev,
                        after_main.st_ino,
                        after_main.st_size,
                        after_main.st_mtime_ns,
                    )
                    != (
                        main_metadata.st_dev,
                        main_metadata.st_ino,
                        main_metadata.st_size,
                        main_metadata.st_mtime_ns,
                    )
                    or (
                        current_main.st_dev,
                        current_main.st_ino,
                        current_main.st_size,
                        current_main.st_mtime_ns,
                    )
                    != (
                        main_metadata.st_dev,
                        main_metadata.st_ino,
                        main_metadata.st_size,
                        main_metadata.st_mtime_ns,
                    )
                    or ((current_wal is None) != (wal_metadata is None))
                    or (
                        current_wal is not None
                        and wal_metadata is not None
                        and after_wal is not None
                        and (
                            (
                                after_wal.st_dev,
                                after_wal.st_ino,
                                after_wal.st_size,
                                after_wal.st_mtime_ns,
                            )
                            != (
                                wal_metadata.st_dev,
                                wal_metadata.st_ino,
                                wal_metadata.st_size,
                                wal_metadata.st_mtime_ns,
                            )
                            or (
                                current_wal.st_dev,
                                current_wal.st_ino,
                                current_wal.st_size,
                                current_wal.st_mtime_ns,
                            )
                            != (
                                wal_metadata.st_dev,
                                wal_metadata.st_ino,
                                wal_metadata.st_size,
                                wal_metadata.st_mtime_ns,
                            )
                        )
                    )
                    or (
                        (current_shm is None) != (shm_metadata is None)
                        or (
                            current_shm is not None
                            and shm_metadata is not None
                            and (current_shm.st_dev, current_shm.st_ino)
                            != (shm_metadata.st_dev, shm_metadata.st_ino)
                        )
                    )
                ):
                    raise RuntimeError(
                        "Program-cycle journal changed while snapshotting"
                    )
                suffix = (
                    "?mode=ro" if wal_metadata is not None else "?mode=ro&immutable=1"
                )
                yield f"{snapshot.as_uri()}{suffix}"
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal cannot be snapshotted: {error}"
            ) from error
        finally:
            if wal_descriptor >= 0:
                os.close(wal_descriptor)
            if main_descriptor >= 0:
                os.close(main_descriptor)

    @staticmethod
    def _preflight_existing(
        path: Path,
        *,
        parent_descriptor: int,
    ) -> tuple[int, int, int]:
        """Validate existing durable state through a strictly read-only handle."""

        try:
            metadata = os.stat(
                path.name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            raise RuntimeError(
                f"Program-cycle journal cannot be inspected: {error}"
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
                "Program-cycle journal is not one safe owner-held regular file"
            )
        database: sqlite3.Connection | None = None
        snapshot_stack = ExitStack()
        try:
            snapshot_uri = snapshot_stack.enter_context(
                ProgramCycleJournal._read_only_snapshot(
                    path,
                    parent_descriptor=parent_descriptor,
                    main_metadata=metadata,
                )
            )
            try:
                database = sqlite3.connect(
                    snapshot_uri,
                    uri=True,
                    timeout=10.0,
                    isolation_level=None,
                    check_same_thread=False,
                )
            except (OSError, sqlite3.Error, ValueError) as error:
                raise RuntimeError(
                    "Program-cycle journal cannot be opened read-only: " f"{error}"
                ) from error
            database.execute("PRAGMA query_only=ON")
            check = database.execute("PRAGMA quick_check").fetchall()
            if check != [("ok",)]:
                raise RuntimeError("Program-cycle journal integrity check failed")
            tables = {
                row[0]
                for row in database.execute(
                    "SELECT name FROM sqlite_schema WHERE type='table'"
                )
            }
            if "program_cycle_metadata" not in tables:
                raise RuntimeError("Program-cycle journal metadata is absent")
            metadata_rows = database.execute(
                "SELECT singleton,schema_version FROM program_cycle_metadata"
            ).fetchall()
            if metadata_rows == [(1, LEGACY_SCHEMA_VERSION)]:
                schema_version = LEGACY_SCHEMA_VERSION
                expected_shapes = _LEGACY_TABLE_SHAPES
                expected_sql = _LEGACY_TABLE_SQL
            elif metadata_rows == [(1, SCHEMA_VERSION)]:
                schema_version = SCHEMA_VERSION
                expected_shapes = _REQUIRED_TABLE_SHAPES
                expected_sql = _REQUIRED_TABLE_SQL
            else:
                raise RuntimeError("Program-cycle journal schema is unsupported")
            expected_tables = set(expected_shapes)
            if tables != expected_tables:
                raise RuntimeError(
                    "Program-cycle journal schema has unsupported tables"
                )
            schema_rows = {
                row[0]: row[1]
                for row in database.execute(
                    "SELECT name,sql FROM sqlite_schema WHERE type='table'"
                )
            }
            for table, expected_shape in expected_shapes.items():
                rows = database.execute(f"PRAGMA table_info({table})").fetchall()
                shape = tuple(
                    (row[1], str(row[2]).upper(), row[3], row[5]) for row in rows
                )
                schema_sql = schema_rows.get(table)
                if (
                    shape != expected_shape
                    or any(row[4] is not None for row in rows)
                    or not isinstance(schema_sql, str)
                    or _normalized_sql(schema_sql)
                    != _normalized_sql(expected_sql[table])
                ):
                    raise RuntimeError(
                        f"Program-cycle journal table {table} has unsupported shape"
                    )
            unsupported_objects = database.execute(
                "SELECT type,name FROM sqlite_schema "
                "WHERE type IN ('view','trigger') "
                "OR (type='index' AND sql IS NOT NULL)"
            ).fetchall()
            if unsupported_objects:
                raise RuntimeError(
                    "Program-cycle journal schema has unsupported objects"
                )
            cycle_rows = database.execute("SELECT * FROM program_cycle").fetchall()
            if (
                len(cycle_rows) != 1
                or cycle_rows[0][0] != 1
                or type(cycle_rows[0][1]) is not int
                or cycle_rows[0][1] < 1
                or cycle_rows[0][2] not in STATES
                or not _bounded_identity(cycle_rows[0][3])
                or (
                    cycle_rows[0][4] is not None
                    and not _bounded_identity(cycle_rows[0][4])
                )
                or type(cycle_rows[0][11]) is not int
                or cycle_rows[0][11] not in {0, 1}
                or any(
                    value is not None and not isinstance(value, str)
                    for value in (cycle_rows[0][7], cycle_rows[0][13])
                )
            ):
                raise RuntimeError("Program-cycle journal owner row is corrupt")
            for index in (5, 6, 8, 9, 10, 12):
                _strict_object(cycle_rows[0][index])

            if schema_version == SCHEMA_VERSION:
                if (
                    type(cycle_rows[0][14]) is not int
                    or not 1 <= cycle_rows[0][14] <= 9_007_199_254_740_991
                    or any(
                        value is not None and not _bounded_identity(value)
                        for value in cycle_rows[0][15:19]
                    )
                ):
                    raise RuntimeError("Program-cycle control identity is corrupt")
                _strict_object(cycle_rows[0][19])
                if cycle_rows[0][20] is not None and not _bounded_identity(
                    cycle_rows[0][20]
                ):
                    raise RuntimeError("Program-cycle reset phase is corrupt")
                _strict_object(cycle_rows[0][21])

            request_count = 0
            command_table = (
                "program_cycle_requests"
                if schema_version == LEGACY_SCHEMA_VERSION
                else "program_cycle_inflight_commands"
            )
            selected_columns = (
                "request_owner_epoch,request_id,request_kind,intent_json,"
                "cycle_generation,result_json,error_json"
                if schema_version == LEGACY_SCHEMA_VERSION
                else "request_owner_epoch,request_id,request_kind,intent_json,"
                "cycle_generation"
            )
            reset_request_count = 0
            for request in database.execute(
                f"SELECT {selected_columns} FROM {command_table}"
            ):
                request_count += 1
                reset_request_count += request[2] == "reset"
                if (
                    (
                        request_count > 4_096
                        and (
                            schema_version != SCHEMA_VERSION
                            or request_count > 4_097
                            or reset_request_count != 1
                        )
                    )
                    or not _bounded_identity(request[0])
                    or not _bounded_identity(request[1])
                    or request[2] not in _REQUEST_KINDS
                    or type(request[4]) is not int
                    or request[4] < 1
                    or (
                        schema_version == LEGACY_SCHEMA_VERSION
                        and request[5] is not None
                        and request[6] is not None
                    )
                ):
                    raise RuntimeError("Program-cycle request ledger is corrupt")
                _strict_object(request[3], required=True)
                if schema_version == LEGACY_SCHEMA_VERSION:
                    _strict_object(request[5])
                    _strict_object(request[6])

            liveness_rows = database.execute(
                "SELECT singleton,manager_runtime_epoch,manager_owner_epoch,"
                "predecessor_expired,fault_reference "
                "FROM program_runner_liveness"
            ).fetchall()
            if (
                len(liveness_rows) != 1
                or liveness_rows[0][0] != 1
                or any(
                    value is not None and not _bounded_identity(value)
                    for value in liveness_rows[0][1:3]
                )
                or type(liveness_rows[0][3]) is not int
                or liveness_rows[0][3] not in {0, 1}
                or (
                    liveness_rows[0][4] is not None
                    and not isinstance(liveness_rows[0][4], str)
                )
            ):
                raise RuntimeError("Program-cycle liveness row is corrupt")

            history_count = 0
            for history in database.execute(
                "SELECT program_run_id,run_revision,snapshot_json,terminal "
                "FROM program_run_history"
            ):
                history_count += 1
                snapshot = _strict_object(history[2], required=True)
                capture = snapshot.get("capture") if snapshot is not None else None
                if (
                    history_count > 4_096
                    or not _bounded_identity(history[0])
                    or type(history[1]) is not int
                    or history[1] < 1
                    or type(history[3]) is not int
                    or history[3] not in {0, 1}
                    or not isinstance(capture, dict)
                    or capture.get("program_run_id") != history[0]
                    or snapshot.get("run_revision") != history[1]
                    or (snapshot.get("state") in _TERMINAL_RUN_STATES)
                    != bool(history[3])
                ):
                    raise RuntimeError("Program-run history row is corrupt")
            after = os.stat(
                path.name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
            if (
                (after.st_dev, after.st_ino) != (metadata.st_dev, metadata.st_ino)
                or not stat.S_ISREG(after.st_mode)
                or after.st_uid != os.geteuid()
                or after.st_nlink != 1
                or after.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or after.st_mode & (stat.S_IRUSR | stat.S_IWUSR)
                != (stat.S_IRUSR | stat.S_IWUSR)
            ):
                raise RuntimeError(
                    "Program-cycle journal changed during read-only preflight"
                )
        except sqlite3.Error as error:
            raise RuntimeError(
                f"Program-cycle journal read-only preflight failed: {error}"
            ) from error
        finally:
            if database is not None:
                database.close()
            snapshot_stack.close()
        return metadata.st_dev, metadata.st_ino, schema_version

    def __init__(
        self,
        path: Path,
        *,
        owner_epoch: str,
        maximum_inflight_commands: int = 4_096,
        maximum_retained_runs: int = 4_096,
    ) -> None:
        if not 1 <= maximum_inflight_commands <= 4_096:
            raise ValueError("maximum_inflight_commands must be in [1, 4096]")
        self._maximum_inflight_commands = maximum_inflight_commands
        if not 1 <= maximum_retained_runs <= 4_096:
            raise ValueError("maximum_retained_runs must be in [1, 4096]")
        self._maximum_retained_runs = maximum_retained_runs
        if (
            not owner_epoch
            or unicodedata.normalize("NFC", owner_epoch) != owner_epoch
            or len(owner_epoch.encode("utf-8")) > 256
        ):
            raise ValueError("owner_epoch must be bounded non-empty NFC text")
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
                try:
                    os.stat(
                        path.name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    )
                    exists = True
                except FileNotFoundError:
                    exists = False
                except OSError as error:
                    raise RuntimeError(
                        f"Program-cycle journal cannot be inspected: {error}"
                    ) from error
                new_database = not exists
                expected_identity: tuple[int, int]
                existing_schema_version = SCHEMA_VERSION
                if exists:
                    preflight = self._preflight_existing(
                        path,
                        parent_descriptor=parent_descriptor,
                    )
                    expected_identity = preflight[:2]
                    existing_schema_version = preflight[2]
                else:
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
        self._database = database
        try:
            self._database.execute("PRAGMA foreign_keys=ON")
            self._database.execute("PRAGMA journal_mode=WAL")
            self._database.execute("PRAGMA synchronous=FULL")
            if new_database:
                self._database.executescript(_SCHEMA_SQL)
            elif existing_schema_version == LEGACY_SCHEMA_VERSION:
                self._migrate_v3()
            with self._transaction():
                if new_database:
                    self._database.execute(
                        "INSERT INTO program_cycle_metadata VALUES(1,?)",
                        (SCHEMA_VERSION,),
                    )
                    self._database.execute(
                        "INSERT INTO program_runner_liveness "
                        "VALUES(1,NULL,NULL,0,NULL)"
                    )
                    self._database.execute(
                        """
                        INSERT INTO program_cycle
                        (singleton,cycle_generation,state,owner_epoch,program_run_id,
                         claim_json,capture_identities_json,terminal_reference,
                         start_settlement_json,
                         reset_result_json,reconciliation_required,
                         acquisition_pending_json,reconciliation_evidence_digest,
                         release_intent_json,execution_control_revision,
                         causal_correlation_id,causal_session_id,
                         causal_client_sequence,current_command_kind,
                         failure_json,reset_phase,runtime_controls_json)
                        VALUES(1,1,'idle_after_reset',?,NULL,NULL,NULL,NULL,NULL,NULL,0,NULL,NULL,NULL,1,NULL,NULL,NULL,NULL,NULL,NULL,NULL)
                        """,
                        (owner_epoch,),
                    )
                row = self.read()
                if not new_database and row.state == "resetting":
                    # A process cannot truthfully continue an in-memory Reset
                    # call after restart.  Preserve its durable phase, make
                    # the interruption explicit, and require the replacement
                    # owner to reconcile with Kernel before that phase can be
                    # resumed by a fresh Reset command.
                    phase = row.reset_phase
                    failure = {
                        "code": "program_cycle_reset_interrupted",
                        "message": "Program Runner restarted during Reset",
                    }
                    if _bounded_identity(phase):
                        failure["failed_phase"] = phase
                    self._database.execute(
                        """
                        UPDATE program_cycle
                        SET state='recovery_required',failure_json=?,
                            reconciliation_required=1,
                            execution_control_revision=execution_control_revision+1
                        WHERE singleton=1 AND state='resetting'
                        """,
                        (self._dump(failure),),
                    )
                    row = self.read()
                if not new_database and (
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
                if row.state == "idle_after_reset":
                    # Only unresolved commands are retained.  An idle current
                    # cycle proves any predecessor pre-effect command cannot
                    # still own a physical effect.
                    self._database.execute(
                        "DELETE FROM program_cycle_inflight_commands"
                    )
        except BaseException:
            self._database.close()
            os.close(self._parent_descriptor)
            self._parent_descriptor = -1
            raise

    def _migrate_v3(self) -> None:
        """Atomically narrow v3's lifetime ledger to current/in-flight state."""

        self._database.execute("BEGIN IMMEDIATE")
        try:
            self._database.execute(
                """
                CREATE TABLE program_cycle_v4 (
                  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                  cycle_generation INTEGER NOT NULL CHECK(cycle_generation>0),
                  state TEXT NOT NULL,
                  owner_epoch TEXT NOT NULL,
                  program_run_id TEXT,
                  claim_json TEXT,
                  capture_identities_json TEXT,
                  terminal_reference TEXT,
                  start_settlement_json TEXT,
                  release_intent_json TEXT,
                  reset_result_json TEXT,
                  reconciliation_required INTEGER NOT NULL
                    CHECK(reconciliation_required IN (0,1)),
                  acquisition_pending_json TEXT,
                  reconciliation_evidence_digest TEXT,
                  execution_control_revision INTEGER NOT NULL
                    CHECK(execution_control_revision>0),
                  causal_correlation_id TEXT,
                  causal_session_id TEXT,
                  causal_client_sequence TEXT,
                  current_command_kind TEXT,
                  failure_json TEXT,
                  reset_phase TEXT,
                  runtime_controls_json TEXT
                )
                """
            )
            self._database.execute(
                """
                INSERT INTO program_cycle_v4
                SELECT singleton,cycle_generation,state,owner_epoch,program_run_id,
                       claim_json,capture_identities_json,terminal_reference,
                       start_settlement_json,release_intent_json,
                       reset_result_json,reconciliation_required,
                       acquisition_pending_json,reconciliation_evidence_digest,
                       1,NULL,NULL,NULL,NULL,NULL,NULL,NULL
                FROM program_cycle
                """
            )
            self._database.execute(
                """
                CREATE TABLE program_cycle_inflight_commands (
                  request_owner_epoch TEXT NOT NULL,
                  request_id TEXT NOT NULL,
                  request_kind TEXT NOT NULL,
                  intent_json TEXT NOT NULL,
                  cycle_generation INTEGER NOT NULL,
                  PRIMARY KEY(request_owner_epoch, request_id)
                )
                """
            )
            self._database.execute(
                """
                INSERT INTO program_cycle_inflight_commands
                SELECT request_owner_epoch,request_id,request_kind,intent_json,
                       cycle_generation
                FROM program_cycle_requests
                WHERE result_json IS NULL AND error_json IS NULL
                """
            )
            self._database.execute("DROP TABLE program_cycle_requests")
            self._database.execute("DROP TABLE program_cycle")
            self._database.execute(
                "ALTER TABLE program_cycle_v4 RENAME TO program_cycle"
            )
            self._database.execute(
                """
                CREATE TABLE program_cycle_metadata_v4 (
                  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                  schema_version INTEGER NOT NULL CHECK(schema_version=4)
                )
                """
            )
            self._database.execute("INSERT INTO program_cycle_metadata_v4 VALUES(1,4)")
            self._database.execute("DROP TABLE program_cycle_metadata")
            self._database.execute(
                "ALTER TABLE program_cycle_metadata_v4 "
                "RENAME TO program_cycle_metadata"
            )
        except BaseException:
            self._database.execute("ROLLBACK")
            raise
        self._database.execute("COMMIT")

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

    def _validate_storage(self) -> None:
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
        provisional_run_id: str,
        evidence: Mapping[str, Any],
        correlation_id: str | None = None,
        session_id: str | None = None,
        client_sequence: str | None = None,
    ) -> None:
        """Durably mark the inherently uncertain acquire/response checkpoint."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET state='starting', program_run_id=?, acquisition_pending_json=?,
                    execution_control_revision=execution_control_revision+1,
                    causal_correlation_id=?,causal_session_id=?,
                    causal_client_sequence=?,current_command_kind='program_run',
                    failure_json=NULL,reset_phase=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state='idle_after_reset' AND claim_json IS NULL
                  AND acquisition_pending_json IS NULL
                """,
                (
                    provisional_run_id,
                    self._dump(evidence),
                    correlation_id,
                    session_id,
                    client_sequence,
                    expected_generation,
                ),
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
                  AND state IN ('starting','stopping') AND claim_json IS NULL
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

    def refresh_pending_acquire(
        self,
        *,
        expected_generation: int,
        provisional_run_id: str,
        evidence: Mapping[str, Any],
    ) -> None:
        """Replace pre-acquire evidence after scene preparation advanced admission."""

        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET acquisition_pending_json=?
                WHERE singleton=1 AND cycle_generation=?
                  AND state='starting' AND program_run_id=?
                  AND claim_json IS NULL AND acquisition_pending_json IS NOT NULL
                """,
                (
                    self._dump(evidence),
                    expected_generation,
                    provisional_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle acquire evidence is stale")

    def mark_acquire_unknown(self, *, expected_generation: int) -> None:
        """Fail closed without inventing the claim hidden by response loss."""
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET reconciliation_required=1,
                    state='recovery_required',
                    execution_control_revision=execution_control_revision+1,
                    failure_json=(
                      '{"code":"program_cycle_acquire_outcome_unknown"}'
                    )
                WHERE singleton=1 AND cycle_generation=?
                  AND state IN ('starting','stopping') AND claim_json IS NULL
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
                SET state='idle_after_reset', program_run_id=NULL,
                    acquisition_pending_json=NULL,
                    execution_control_revision=execution_control_revision+1,
                    current_command_kind=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state='starting' AND claim_json IS NULL
                  AND acquisition_pending_json IS NOT NULL
                """,
                (expected_generation,),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle rejected acquire is stale")

    @contextmanager
    def _transaction(self) -> Iterator[None]:
        committed = False
        with self._lock:
            self._validate_storage()
            self._database.execute("BEGIN IMMEDIATE")
            try:
                yield
            except BaseException:
                self._database.execute("ROLLBACK")
                raise
            self._database.execute("COMMIT")
            committed = True
        observer = self._change_observer if committed else None
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
            self._validate_storage()
            row = self._database.execute(
                """
                SELECT cycle_generation,state,owner_epoch,program_run_id,claim_json,
                       capture_identities_json,terminal_reference,
                       start_settlement_json,release_intent_json,
                       reset_result_json,
                       reconciliation_required,acquisition_pending_json,
                       reconciliation_evidence_digest,
                       execution_control_revision,causal_correlation_id,
                       causal_session_id,causal_client_sequence,
                       current_command_kind,failure_json,reset_phase,
                       runtime_controls_json
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
            self._load(row[9]),
            bool(row[10]),
            self._load(row[11]),
            row[12],
            row[13],
            row[14],
            row[15],
            row[16],
            row[17],
            self._load(row[18]),
            row[19],
            self._load(row[20]),
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
                  AND state IN ('terminal_awaiting_reset','resetting')
                  AND reconciliation_required=0
                  AND CAST(json_extract(claim_json,'$.claim_generation') AS INTEGER)=?
                  AND (release_intent_json IS NULL OR release_intent_json=?)
                """,
                (encoded, expected_generation, claim_generation, encoded),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle release intent is stale")

    def pending_commands(self) -> tuple[dict[str, Any], ...]:
        """Bounded current unresolved evidence, never an execution/retry queue."""
        with self._lock:
            self._validate_storage()
            return tuple(
                {
                    "request_owner_epoch": row[0],
                    "request_id": row[1],
                    "kind": row[2],
                    "intent": self._load(row[3]),
                    "cycle_generation": row[4],
                }
                for row in self._database.execute(
                    "SELECT request_owner_epoch,request_id,request_kind,intent_json,cycle_generation "
                    "FROM program_cycle_inflight_commands ORDER BY request_owner_epoch,request_id"
                )
            )

    def record_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        kind: str,
        intent: Mapping[str, Any],
        cycle_generation: int,
    ) -> RequestRecord:
        if kind not in _REQUEST_KINDS:
            raise ValueError("Program-cycle request kind is unsupported")
        encoded = self._dump(intent)
        with self._transaction():
            current = self._database.execute(
                """
                SELECT request_kind,intent_json
                FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).fetchone()
            if current is not None:
                if current[0] != kind or current[1] != encoded:
                    raise ValueError(
                        "request identity was reused with different intent"
                    )
                return RequestRecord("in_progress")
            total, resets = self._database.execute(
                "SELECT COUNT(*),COALESCE(SUM(request_kind='reset'),0) "
                "FROM program_cycle_inflight_commands"
            ).fetchone()
            count = resets if kind == "reset" else total - resets
            # Exactly one Reset occurrence remains admissible when the
            # ordinary guard is full. An uncertain Reset must be continued,
            # not appended repeatedly into this reserved slot.
            capacity = 1 if kind == "reset" else self._maximum_inflight_commands
            if count >= capacity or resets > 1:
                raise OverflowError("Program-cycle in-flight command bound is full")
            self._database.execute(
                """
                INSERT INTO program_cycle_inflight_commands VALUES(?,?,?,?,?)
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
        details: Mapping[str, Any] | None = None,
    ) -> None:
        """Forget a proven no-effect command; rejection lives in status/history."""
        del code, message, details
        with self._transaction():
            changed = self._database.execute(
                """
                DELETE FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle in-flight command is absent")

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
                DELETE FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle in-flight command is absent")

    def transition(
        self,
        *,
        expected_generation: int,
        expected_states: tuple[str, ...],
        state: str,
        program_run_id: str | None = None,
        claim: Mapping[str, Any] | None = None,
        capture_identities: Mapping[str, Any] | None = None,
        run_snapshot: Mapping[str, Any] | None = None,
        terminal_reference: str | None = None,
        start_settlement: Mapping[str, Any] | None = None,
        reconciliation_required: bool = False,
        correlation_id: str | None = None,
        session_id: str | None = None,
        client_sequence: str | None = None,
        command_kind: str | None = None,
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
                    start_settlement_json=COALESCE(?,start_settlement_json),
                    reconciliation_required=?, acquisition_pending_json=NULL,
                    execution_control_revision=execution_control_revision+1,
                    causal_correlation_id=COALESCE(?,causal_correlation_id),
                    causal_session_id=COALESCE(?,causal_session_id),
                    causal_client_sequence=COALESCE(?,causal_client_sequence),
                    current_command_kind=COALESCE(?,current_command_kind)
                WHERE singleton=1 AND cycle_generation=?
                  AND state IN ({placeholders})
                """,
                (
                    state,
                    program_run_id,
                    self._dump(claim),
                    self._dump(capture_identities),
                    terminal_reference,
                    self._dump(start_settlement),
                    int(reconciliation_required),
                    correlation_id,
                    session_id,
                    client_sequence,
                    command_kind,
                    expected_generation,
                    *expected_states,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle transition is stale")
            if run_snapshot is not None:
                self._retain_run_snapshot_locked(run_snapshot)

    @staticmethod
    def _run_snapshot_identity(snapshot: Mapping[str, Any]) -> tuple[str, int]:
        capture = snapshot.get("capture")
        program_run_id = (
            capture.get("program_run_id") if isinstance(capture, Mapping) else None
        )
        revision = snapshot.get("run_revision")
        if (
            not isinstance(program_run_id, str)
            or not program_run_id
            or type(revision) is not int
            or revision < 1
        ):
            raise ValueError("Program-run history snapshot has invalid identity")
        return program_run_id, revision

    def _retain_run_snapshot_locked(self, snapshot: Mapping[str, Any]) -> None:
        program_run_id, revision = self._run_snapshot_identity(snapshot)
        encoded = self._dump(snapshot)
        assert encoded is not None
        terminal = snapshot.get("state") in {"stopped", "succeeded", "failed"}
        existing = self._database.execute(
            "SELECT run_revision,snapshot_json,terminal FROM program_run_history "
            "WHERE program_run_id=?",
            (program_run_id,),
        ).fetchone()
        if existing is None:
            count = self._database.execute(
                "SELECT COUNT(*) FROM program_run_history"
            ).fetchone()
            if count is None or count[0] >= self._maximum_retained_runs:
                removed = self._database.execute(
                    """
                    DELETE FROM program_run_history
                    WHERE rowid=(
                      SELECT rowid FROM program_run_history
                      WHERE terminal=1 ORDER BY rowid LIMIT 1
                    )
                    """
                ).rowcount
                if removed != 1:
                    raise OverflowError(
                        "Program-run history has no resolved row to rotate"
                    )
            self._database.execute(
                "INSERT INTO program_run_history VALUES(?,?,?,?)",
                (program_run_id, revision, encoded, int(terminal)),
            )
            return
        if existing[2] and encoded != existing[1]:
            raise RuntimeError("terminal Program-run history is immutable")
        if revision < existing[0] or (
            revision == existing[0] and encoded != existing[1]
        ):
            raise RuntimeError("Program-run history revision is divergent")
        if revision > existing[0]:
            self._database.execute(
                "UPDATE program_run_history SET run_revision=?,snapshot_json=?,terminal=? "
                "WHERE program_run_id=?",
                (revision, encoded, int(terminal), program_run_id),
            )

    def retain_run_snapshot(self, snapshot: Mapping[str, Any]) -> None:
        with self._transaction():
            self._retain_run_snapshot_locked(snapshot)

    def require_run_history_capacity(self) -> None:
        with self._lock:
            self._validate_storage()
            count = self._database.execute(
                "SELECT COUNT(*) FROM program_run_history"
            ).fetchone()
            terminal = self._database.execute(
                "SELECT 1 FROM program_run_history WHERE terminal=1 LIMIT 1"
            ).fetchone()
        if count is None or (
            count[0] >= self._maximum_retained_runs and terminal is None
        ):
            raise OverflowError("Program-run history has no resolved row to rotate")

    def read_run_snapshot(self, program_run_id: str) -> dict[str, Any] | None:
        with self._lock:
            self._validate_storage()
            row = self._database.execute(
                "SELECT snapshot_json FROM program_run_history WHERE program_run_id=?",
                (program_run_id,),
            ).fetchone()
        return None if row is None else self._load(row[0])

    def read_runtime_controls(self, program_run_id: str) -> dict[str, Any] | None:
        """Read only the current run's bounded durable control snapshot."""

        if not _bounded_identity(program_run_id):
            raise ValueError("Program-run identity is invalid")
        with self._lock:
            self._validate_storage()
            row = self._database.execute(
                "SELECT program_run_id,runtime_controls_json FROM program_cycle "
                "WHERE singleton=1"
            ).fetchone()
        if row is None or row[0] != program_run_id:
            return None
        return deepcopy(self._load(row[1]))

    def list_run_snapshots(self, *, limit: int) -> tuple[list[dict[str, Any]], bool]:
        if type(limit) is not int or not 1 <= limit <= self._maximum_retained_runs:
            raise ValueError("Program-run history limit is invalid")
        with self._lock:
            self._validate_storage()
            rows = self._database.execute(
                "SELECT snapshot_json FROM program_run_history "
                "ORDER BY program_run_id LIMIT ?",
                (limit + 1,),
            ).fetchall()
        values = [self._load(row[0]) for row in rows[:limit]]
        return [value for value in values if value is not None], len(rows) > limit

    def observe_terminal(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        terminal_reference: str,
        snapshot: Mapping[str, Any],
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
                self._retain_run_snapshot_locked(snapshot)
                return False
            if state not in {"running", "stopping"}:
                return False
            self._retain_run_snapshot_locked(snapshot)
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET state='terminal_awaiting_reset', terminal_reference=?,
                    execution_control_revision=execution_control_revision+1
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
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        correlation_id: str | None = None,
        session_id: str | None = None,
        client_sequence: str | None = None,
    ) -> None:
        """Close current work before containment, retaining any uncertainty."""
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
                UPDATE program_cycle SET state='stopping',
                    execution_control_revision=execution_control_revision+
                      CASE WHEN state IN ('starting','running','recovery_required') THEN 1 ELSE 0 END,
                    current_command_kind='program_stop',
                    causal_correlation_id=COALESCE(?,causal_correlation_id),
                    causal_session_id=COALESCE(?,causal_session_id),
                    causal_client_sequence=COALESCE(?,causal_client_sequence)
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND (state IN ('starting','running','stopping') OR
                    (state='recovery_required' AND reset_phase IS NULL
                     AND release_intent_json IS NULL AND acquisition_pending_json IS NULL
                     AND claim_json IS NOT NULL))
                """,
                (
                    correlation_id,
                    session_id,
                    client_sequence,
                    expected_generation,
                    program_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle stopping transition is stale")

    def begin_resetting(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        correlation_id: str,
        session_id: str | None = None,
        client_sequence: str | None = None,
        expected_recovery: CycleRecord | None = None,
    ) -> bool:
        """Atomically close ordinary admission before Reset performs Stop."""

        with self._transaction():
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
            row = self._database.execute(
                """
                SELECT state,program_run_id,causal_correlation_id
                FROM program_cycle
                WHERE singleton=1 AND cycle_generation=?
                """,
                (expected_generation,),
            ).fetchone()
            if row == ("resetting", program_run_id, correlation_id):
                return False
            eligible_states = {"running", "terminal_awaiting_reset"}
            if expected_recovery is not None:
                eligible_states.add("recovery_required")
            if row is None or row[0] not in eligible_states or row[1] != program_run_id:
                raise RuntimeError("Program-cycle Reset transition is stale")
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET state='resetting',reset_phase='stopping',
                    execution_control_revision=execution_control_revision+1,
                    causal_correlation_id=?,causal_session_id=?,
                    causal_client_sequence=?,current_command_kind='program_reset',
                    failure_json=NULL
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state IN ('running','terminal_awaiting_reset','recovery_required')
                """,
                (
                    correlation_id,
                    session_id,
                    client_sequence,
                    expected_generation,
                    program_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset transition is stale")
            return True

    def settle_reset_recovery(
        self,
        *,
        expected: CycleRecord,
        manager_runtime_epoch: str | None,
        manager_owner_epoch: str | None,
    ) -> None:
        """Persist an externally proved containment join, not guessed outcomes."""
        if (
            expected.state != "resetting"
            or expected.reset_phase != "settling"
            or not expected.reconciliation_required
            or expected.claim is None
            or expected.acquisition_pending is not None
            or expected.release_intent is not None
        ):
            raise RuntimeError("Program-cycle containment discharge is invalid")
        with self._transaction():
            live = self.liveness()
            if (
                self.read() != expected
                or live.expired
                or live.manager_runtime_epoch != manager_runtime_epoch
                or live.manager_owner_epoch != manager_owner_epoch
            ):
                raise RuntimeError("Program-cycle containment discharge is stale")
            self._database.execute(
                "UPDATE program_cycle SET reset_phase='restoring_scene', "
                "reconciliation_required=0,failure_json=NULL, "
                "execution_control_revision=execution_control_revision+1 WHERE singleton=1"
            )

    def update_reset_phase(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        expected_phase: str,
        phase: str,
        terminal_reference: str | None = None,
    ) -> None:
        for value in (expected_phase, phase):
            if not _bounded_identity(value):
                raise ValueError("Reset phase must be bounded non-empty NFC text")
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET reset_phase=?,terminal_reference=COALESCE(?,terminal_reference),
                    execution_control_revision=execution_control_revision+1
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state='resetting' AND reset_phase=?
                """,
                (
                    phase,
                    terminal_reference,
                    expected_generation,
                    program_run_id,
                    expected_phase,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset phase is stale")

    def fail_reset(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        code: str,
        message: str,
        reconciliation_required: bool = False,
    ) -> None:
        with self._transaction():
            row = self._database.execute(
                """
                SELECT reset_phase FROM program_cycle
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state='resetting'
                """,
                (expected_generation, program_run_id),
            ).fetchone()
            if row is None or not _bounded_identity(row[0]):
                raise RuntimeError("Program-cycle failed Reset transition is stale")
            failure = self._dump(
                {
                    "code": code,
                    "message": message,
                    "failed_phase": row[0],
                }
            )
            changed = self._database.execute(
                """
                UPDATE program_cycle SET state='recovery_required',failure_json=?,
                    reconciliation_required=MAX(reconciliation_required,?),
                    execution_control_revision=execution_control_revision+1
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state='resetting'
                """,
                (
                    failure,
                    int(reconciliation_required),
                    expected_generation,
                    program_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle failed Reset transition is stale")

    def resume_resetting(
        self,
        *,
        expected_generation: int,
        program_run_id: str,
        expected_failure_code: str,
        phase: str,
        correlation_id: str,
        session_id: str | None = None,
        client_sequence: str | None = None,
        allow_legacy_scene_failure: bool = False,
    ) -> None:
        """Resume only the exact failed Reset phase under a fresh command."""

        for value in (expected_failure_code, phase, correlation_id):
            if not _bounded_identity(value):
                raise ValueError("Reset recovery identity must be bounded text")
        with self._transaction():
            row = self._database.execute(
                """
                SELECT failure_json,reset_phase,reconciliation_required,
                       release_intent_json
                FROM program_cycle
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state='recovery_required'
                """,
                (expected_generation, program_run_id),
            ).fetchone()
            if row is None:
                raise RuntimeError("Program-cycle Reset recovery is stale")
            failure = self._load(row[0])
            failed_phase = (
                failure.get("failed_phase") if isinstance(failure, dict) else None
            )
            legacy_scene_failure = bool(
                allow_legacy_scene_failure
                and isinstance(failure, dict)
                and failure.get("code")
                == "program_cycle_reset_scene_preparation_failed"
                and row[1] == "failed"
                and bool(row[2])
                and row[3] is None
                and phase == "restoring_scene"
            )
            if (
                not isinstance(failure, dict)
                or failure.get("code") != expected_failure_code
                or (
                    not legacy_scene_failure
                    and (bool(row[2]) or row[1] != phase or failed_phase != phase)
                )
            ):
                raise RuntimeError("Program-cycle Reset recovery is not retryable")
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET state='resetting',reset_phase=?,reconciliation_required=0,
                    failure_json=NULL,
                    execution_control_revision=execution_control_revision+1,
                    causal_correlation_id=?,causal_session_id=?,
                    causal_client_sequence=?,current_command_kind='program_reset'
                WHERE singleton=1 AND cycle_generation=? AND program_run_id=?
                  AND state='recovery_required'
                """,
                (
                    phase,
                    correlation_id,
                    session_id,
                    client_sequence,
                    expected_generation,
                    program_run_id,
                ),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset recovery is stale")

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
        with self._transaction():
            changed = self._database.execute(
                """
                UPDATE program_cycle SET reconciliation_required=1,
                    state='recovery_required',
                    execution_control_revision=execution_control_revision+1,
                    failure_json=COALESCE(failure_json,?)
                WHERE singleton=1 AND cycle_generation=?
                """,
                (retained_failure, expected_generation),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle reconciliation latch is stale")

    def liveness(self) -> RetainedLiveness:
        """Read only durable identities/evidence, never a deadline or age."""
        with self._lock:
            self._validate_storage()
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
        allow_contained_reset: bool = False,
    ) -> None:
        """Finalize the local successor after authoritative reconciliation."""
        with self._transaction():
            row = self._database.execute(
                """
                SELECT state,owner_epoch,acquisition_pending_json
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
            if row is None:
                raise RuntimeError("Program-cycle journal is corrupt")
            if row[0] not in {"idle_after_reset", "terminal_awaiting_reset"} and not (
                allow_contained_reset and row[0] == "recovery_required"
            ):
                raise RuntimeError(
                    "active Program cycle is not contained for reconciliation"
                )
            if row[2] is not None:
                raise RuntimeError("unknown Program-cycle acquisition is unresolved")
            if require_terminal_release and row[0] == "terminal_awaiting_reset":
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

    def prepare_liveness_reconciliation(
        self,
        *,
        predecessor_owner_epoch: str,
        evidence_digest: str,
        require_terminal_release: bool = True,
        allow_contained_reset: bool = False,
    ) -> None:
        """Retain one replaceable intent without claiming the Kernel effect."""
        if not _bounded_identity(predecessor_owner_epoch):
            raise ValueError("predecessor owner epoch must be bounded identity text")
        if (
            not isinstance(evidence_digest, str)
            or len(evidence_digest) != 71
            or not evidence_digest.startswith("sha256:")
            or any(
                character not in "0123456789abcdef" for character in evidence_digest[7:]
            )
        ):
            raise ValueError("reconciliation evidence must be a SHA-256 digest")
        with self._transaction():
            row = self._database.execute(
                """
                SELECT state,acquisition_pending_json,release_intent_json
                FROM program_cycle WHERE singleton=1
                """
            ).fetchone()
            if row is None:
                raise RuntimeError("Program-cycle journal is corrupt")
            if row[0] not in {"idle_after_reset", "terminal_awaiting_reset"} and not (
                allow_contained_reset and row[0] == "recovery_required"
            ):
                raise RuntimeError(
                    "active Program cycle is not contained for reconciliation"
                )
            if row[1] is not None:
                raise RuntimeError("unknown Program-cycle acquisition is unresolved")
            if (
                require_terminal_release
                and row[0] == "terminal_awaiting_reset"
                and row[2] is None
            ):
                raise RuntimeError(
                    "terminal Program-cycle release is not durably retained"
                )
            self._database.execute(
                """
                UPDATE program_cycle
                SET owner_epoch=?,reconciliation_required=1,
                    reconciliation_evidence_digest=?
                WHERE singleton=1
                """,
                (predecessor_owner_epoch, evidence_digest),
            )

    def complete_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        result: Mapping[str, Any],
    ) -> None:
        del result
        with self._transaction():
            changed = self._database.execute(
                """
                DELETE FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle in-flight command is absent")

    def complete_control_request(
        self,
        *,
        owner_epoch: str,
        request_id: str,
        result: Mapping[str, Any],
        run_snapshot: Mapping[str, Any],
    ) -> None:
        """Atomically retain one C2 result and its exact updated run view."""

        projection = result.get("runtime_controls")
        if isinstance(projection, Mapping) and isinstance(
            projection.get("runtime_controls"), Mapping
        ):
            projection = projection["runtime_controls"]
        if not isinstance(projection, Mapping):
            raise ValueError("runtime-control result omitted its current projection")
        encoded_controls = self._dump(projection)
        with self._transaction():
            self._retain_run_snapshot_locked(run_snapshot)
            self._database.execute(
                "UPDATE program_cycle SET runtime_controls_json=? WHERE singleton=1",
                (encoded_controls,),
            )
            changed = self._database.execute(
                """
                DELETE FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (owner_epoch, request_id),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle in-flight command is absent")

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
            request = self._database.execute(
                """
                SELECT cycle_generation FROM program_cycle_inflight_commands
                WHERE request_owner_epoch=? AND request_id=?
                """,
                (request_owner_epoch, request_id),
            ).fetchone()
            if request != (expected_generation,):
                raise RuntimeError("Program-cycle Reset in-flight command is absent")
            changed = self._database.execute(
                """
                UPDATE program_cycle
                SET cycle_generation=cycle_generation+1,
                    state='idle_after_reset', owner_epoch=?,
                    program_run_id=NULL, claim_json=NULL,
                    capture_identities_json=NULL, terminal_reference=NULL,
                    start_settlement_json=NULL,
                    release_intent_json=NULL,
                    reset_result_json=?, reconciliation_required=0,
                    acquisition_pending_json=NULL,
                    reconciliation_evidence_digest=NULL,
                    execution_control_revision=execution_control_revision+1,
                    current_command_kind=NULL,failure_json=NULL,reset_phase=NULL
                    ,runtime_controls_json=NULL
                WHERE singleton=1 AND cycle_generation=?
                  AND state IN ('terminal_awaiting_reset','resetting')
                """,
                (owner_epoch, encoded, expected_generation),
            ).rowcount
            if changed != 1:
                raise RuntimeError("Program-cycle Reset is stale")
            # Reaching the successor Idle cycle resolves every command from the
            # predecessor generation. Keeping any of those identities would
            # turn a bounded in-flight guard back into a lifetime ledger.
            self._database.execute(
                """
                DELETE FROM program_cycle_inflight_commands
                WHERE cycle_generation=?
                """,
                (expected_generation,),
            )

    def close(self) -> None:
        with self._lock:
            if self._parent_descriptor < 0:
                return
            self._database.close()
            os.close(self._parent_descriptor)
            self._parent_descriptor = -1


__all__ = [
    "CycleRecord",
    "ProgramCycleJournal",
    "RequestRecord",
    "SCHEMA_VERSION",
    "STATES",
]
