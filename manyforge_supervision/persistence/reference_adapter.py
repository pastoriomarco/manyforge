"""Bounded SQLite reference adapter for the I4 fixture substrate.

The versioned SQL artifact beside this module is the implementation-neutral
contract. This Python adapter exercises selected transaction, durability,
reopen, and replay properties while the selected final C++ Component Manager
owner is implemented. It is not the final Manager state machine.

The adapter deliberately persists only fixture mechanics:

* immutable bootstrap and Apply identity rows;
* append-only opaque journal events with a consistency-check hash chain; and
* one generic compare-and-swap fixture head.

The fixture CAS is not ``IActiveBundlePublicationStore`` and its values are
not ``ActiveBundlePublicationRequestDTO`` or
``ActiveBundlePublicationReceiptDTO``.  The final C++ owner must consume those
normative DTOs directly.  This Python adapter is deleted after that owner and
its black-box tests replace the fixture; it is never retained as a production
compatibility layer.

It performs no compilation, process effect, scene mutation, Kernel fence,
robot/controller lifecycle action, readiness inference, or alarm operation.
The final I3 wire types are not yet available here.  Bundle, graph, request,
and owner-epoch values are therefore validated bounded opaque spellings.  The
store never interprets them and cannot promote them into qualified I3 values.
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass
from enum import StrEnum
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import threading
import time
from typing import Final


PERSISTENCE_SCHEMA_ID: Final = (
    "manyforge.internal.component_manager.persistence-fixture.i4.v1"
)
PERSISTENCE_SCHEMA_VERSION: Final = 1
PERSISTENCE_FIXTURE_SQLITE_APPLICATION_ID: Final = 1_296_454_480

SQLITE_SIGNED_INT64_MAX: Final = (1 << 63) - 1
MAXIMUM_OPERATIONS: Final = 4096
MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION: Final = 512
MAXIMUM_TOTAL_JOURNAL_ENTRIES: Final = 16384
MAXIMUM_FIXTURE_CAS_RECEIPTS: Final = 4096
MAXIMUM_PAYLOAD_BYTES: Final = 1024 * 1024
MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES: Final = 512 * 1024 * 1024
MAXIMUM_IDENTIFIER_BYTES: Final = 256
SQLITE_BUSY_TIMEOUT_MS: Final = 10_000
SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES: Final = 64 * 1024 * 1024

_SCHEMA_PATH = (
    Path(__file__).parent
    / "schema"
    / "001_component_manager_fixture_persistence.sql"
)
_SHA256_RE = re.compile(r"sha256:[0-9a-f]{64}\Z")
_DIGEST_PREFIX = b"manyforge.component-manager-persistence-fixture.v1"


class PersistenceError(RuntimeError):
    """Base persistence-substrate error."""


class PersistenceConflictError(PersistenceError):
    """An immutable identity or compare-and-swap precondition conflicted."""


class PersistenceLimitError(PersistenceError):
    """A fixed resource ceiling would be exceeded."""


class PersistenceIntegrityError(PersistenceError):
    """Stored schema, bytes, identities, or hash chains are inconsistent."""


class PersistenceClosedError(PersistenceError):
    """The caller used a closed adapter."""


class OperationKind(StrEnum):
    BOOTSTRAP = "bootstrap"
    APPLY = "apply"


def _bounded_identity(value: object, label: str, *, optional: bool = False) -> str:
    if optional and value == "":
        return ""
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError(
            f"{label} must be a bounded printable ASCII identity"
        ) from exc
    if (
        not encoded
        or len(encoded) > MAXIMUM_IDENTIFIER_BYTES
        or any(byte < 0x21 or byte > 0x7E for byte in encoded)
    ):
        raise ValueError(f"{label} must be a bounded printable ASCII identity")
    return value


def _positive_integer(value: object, label: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value <= 0
        or value > SQLITE_SIGNED_INT64_MAX
    ):
        raise ValueError(
            f"{label} must be a positive signed SQLite int64"
        )
    return value


def _nonnegative_integer(value: object, label: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > SQLITE_SIGNED_INT64_MAX
    ):
        raise ValueError(
            f"{label} must be a non-negative signed SQLite int64"
        )
    return value


def _sha256_digest(value: object, label: str) -> str:
    if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{label} must be a lowercase sha256 digest")
    return value


def _raw_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _semantic_digest(domain: str, payload: Mapping[str, object]) -> str:
    canonical = json.dumps(
        payload,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("ascii")
    preimage = b"\0".join(
        (
            _DIGEST_PREFIX,
            domain.encode("ascii"),
            canonical,
        )
    )
    return _raw_digest(preimage)


def _schema_bytes() -> bytes:
    try:
        payload = _SCHEMA_PATH.read_bytes()
    except OSError as exc:
        raise PersistenceIntegrityError(
            f"cannot read persistence schema artifact: {_SCHEMA_PATH}"
        ) from exc
    if not payload:
        raise PersistenceIntegrityError("persistence schema artifact is empty")
    return payload


def _schema_digest() -> str:
    return _raw_digest(_schema_bytes())


def _schema_statements() -> tuple[str, ...]:
    statements: list[str] = []
    pending = ""
    for line in _schema_bytes().decode("utf-8").splitlines(keepends=True):
        pending += line
        if sqlite3.complete_statement(pending):
            statement = pending.strip()
            pending = ""
            if statement and not statement.startswith("--"):
                statements.append(statement)
            elif statement:
                # A leading comment can precede a complete CREATE statement.
                uncommented = "\n".join(
                    candidate
                    for candidate in statement.splitlines()
                    if not candidate.lstrip().startswith("--")
                ).strip()
                if uncommented:
                    statements.append(uncommented)
    if pending.strip():
        raise PersistenceIntegrityError(
            "persistence schema artifact ends with incomplete SQL"
        )
    if not statements:
        raise PersistenceIntegrityError(
            "persistence schema artifact has no statements"
        )
    return tuple(statements)


def _normalized_sql(value: str | None) -> str:
    if value is None:
        return ""
    return " ".join(value.split())


def _schema_objects(connection: sqlite3.Connection) -> dict[tuple[str, str], str]:
    rows = connection.execute(
        """
        SELECT type, name, sql
        FROM sqlite_master
        WHERE type IN ('table', 'index', 'trigger')
          AND name NOT LIKE 'sqlite_%'
        ORDER BY type, name
        """
    ).fetchall()
    return {
        (str(row[0]), str(row[1])): _normalized_sql(row[2])
        for row in rows
    }


def _expected_schema_objects() -> dict[tuple[str, str], str]:
    connection = sqlite3.connect(":memory:", isolation_level=None)
    try:
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("BEGIN EXCLUSIVE")
        for statement in _schema_statements():
            connection.execute(statement)
        connection.commit()
        return _schema_objects(connection)
    finally:
        connection.close()


@dataclass(frozen=True, slots=True)
class OperationRef:
    kind: OperationKind
    operation_id: str
    generation: int

    def __post_init__(self) -> None:
        if not isinstance(self.kind, OperationKind):
            raise ValueError("operation kind is invalid")
        _bounded_identity(self.operation_id, "operation_id")
        _positive_integer(self.generation, "operation generation")


@dataclass(frozen=True, slots=True)
class BootstrapIdentity:
    bootstrap_id: str
    generation: int
    intent_digest: str
    target_bundle_identity: str
    target_graph_revision: str

    def __post_init__(self) -> None:
        _bounded_identity(self.bootstrap_id, "bootstrap_id")
        _positive_integer(self.generation, "bootstrap generation")
        _sha256_digest(self.intent_digest, "intent_digest")
        _bounded_identity(
            self.target_bundle_identity,
            "target_bundle_identity",
        )
        _bounded_identity(
            self.target_graph_revision,
            "target_graph_revision",
        )


@dataclass(frozen=True, slots=True)
class ApplyIdentity:
    apply_id: str
    generation: int
    request_id: str
    manager_owner_epoch: str
    intent_digest: str
    expected_fixture_generation: int
    predecessor_bundle_identity: str
    predecessor_graph_revision: str
    target_bundle_identity: str
    target_graph_revision: str

    def __post_init__(self) -> None:
        for label, value in (
            ("apply_id", self.apply_id),
            ("request_id", self.request_id),
            ("manager_owner_epoch", self.manager_owner_epoch),
            (
                "predecessor_bundle_identity",
                self.predecessor_bundle_identity,
            ),
            (
                "predecessor_graph_revision",
                self.predecessor_graph_revision,
            ),
            ("target_bundle_identity", self.target_bundle_identity),
            ("target_graph_revision", self.target_graph_revision),
        ):
            _bounded_identity(value, label)
        _positive_integer(self.generation, "apply generation")
        _sha256_digest(self.intent_digest, "intent_digest")
        _positive_integer(
            self.expected_fixture_generation,
            "expected_fixture_generation",
        )


@dataclass(frozen=True, slots=True)
class OperationRecord:
    ref: OperationRef
    request_id: str
    manager_owner_epoch: str
    intent_digest: str
    expected_fixture_generation: int
    predecessor_bundle_identity: str
    predecessor_graph_revision: str
    target_bundle_identity: str
    target_graph_revision: str
    identity_digest: str
    created_sequence: int

    def __post_init__(self) -> None:
        if not isinstance(self.ref, OperationRef):
            raise TypeError("ref must be an OperationRef")
        _sha256_digest(self.intent_digest, "intent_digest")
        _nonnegative_integer(
            self.expected_fixture_generation,
            "expected_fixture_generation",
        )
        for label, value in (
            ("request_id", self.request_id),
            ("manager_owner_epoch", self.manager_owner_epoch),
            (
                "predecessor_bundle_identity",
                self.predecessor_bundle_identity,
            ),
            (
                "predecessor_graph_revision",
                self.predecessor_graph_revision,
            ),
        ):
            _bounded_identity(value, label, optional=True)
        _bounded_identity(
            self.target_bundle_identity,
            "target_bundle_identity",
        )
        _bounded_identity(
            self.target_graph_revision,
            "target_graph_revision",
        )
        _sha256_digest(self.identity_digest, "identity_digest")
        _positive_integer(self.created_sequence, "created_sequence")
        if self.ref.kind is OperationKind.BOOTSTRAP:
            if (
                self.request_id
                or self.manager_owner_epoch
                or self.expected_fixture_generation != 0
                or self.predecessor_bundle_identity
                or self.predecessor_graph_revision
            ):
                raise ValueError("bootstrap operation shape is invalid")
        elif (
            not self.request_id
            or not self.manager_owner_epoch
            or self.expected_fixture_generation <= 0
            or not self.predecessor_bundle_identity
            or not self.predecessor_graph_revision
        ):
            raise ValueError("Apply operation shape is invalid")


@dataclass(frozen=True, slots=True)
class OperationWriteResult:
    record: OperationRecord
    inserted: bool


@dataclass(frozen=True, slots=True)
class JournalEvent:
    entry_id: str
    event_kind: str
    payload: bytes

    def __post_init__(self) -> None:
        _bounded_identity(self.entry_id, "entry_id")
        _bounded_identity(self.event_kind, "event_kind")
        if not isinstance(self.payload, bytes):
            raise TypeError("journal payload must be immutable bytes")
        if len(self.payload) > MAXIMUM_PAYLOAD_BYTES:
            raise PersistenceLimitError("journal payload exceeds fixed limit")


@dataclass(frozen=True, slots=True)
class JournalEntry:
    ref: OperationRef
    sequence: int
    entry_id: str
    event_kind: str
    payload: bytes
    payload_digest: str
    previous_entry_digest: str
    entry_digest: str

    def __post_init__(self) -> None:
        if not isinstance(self.ref, OperationRef):
            raise TypeError("ref must be an OperationRef")
        _positive_integer(self.sequence, "journal sequence")
        _bounded_identity(self.entry_id, "entry_id")
        _bounded_identity(self.event_kind, "event_kind")
        if not isinstance(self.payload, bytes):
            raise TypeError("journal payload must be immutable bytes")
        if len(self.payload) > MAXIMUM_PAYLOAD_BYTES:
            raise PersistenceLimitError("journal payload exceeds fixed limit")
        _sha256_digest(self.payload_digest, "payload_digest")
        _sha256_digest(self.previous_entry_digest, "previous_entry_digest")
        _sha256_digest(self.entry_digest, "entry_digest")


@dataclass(frozen=True, slots=True)
class JournalAppendResult:
    entry: JournalEntry
    inserted: bool


@dataclass(frozen=True, slots=True)
class FixtureCasIntent:
    fixture_cas_id: str
    operation_ref: OperationRef
    operation_identity_digest: str

    def __post_init__(self) -> None:
        _bounded_identity(self.fixture_cas_id, "fixture_cas_id")
        if not isinstance(self.operation_ref, OperationRef):
            raise TypeError("operation_ref must be an OperationRef")
        _sha256_digest(
            self.operation_identity_digest,
            "operation_identity_digest",
        )


@dataclass(frozen=True, slots=True)
class FixtureCasReceipt:
    fixture_cas_id: str
    fixture_cas_intent_digest: str
    operation_ref: OperationRef
    operation_identity_digest: str
    expected_fixture_generation: int
    predecessor_bundle_identity: str
    predecessor_graph_revision: str
    committed_fixture_generation: int
    target_bundle_identity: str
    target_graph_revision: str
    receipt_digest: str
    created_sequence: int

    def __post_init__(self) -> None:
        _bounded_identity(self.fixture_cas_id, "fixture_cas_id")
        _sha256_digest(
            self.fixture_cas_intent_digest,
            "fixture_cas_intent_digest",
        )
        if not isinstance(self.operation_ref, OperationRef):
            raise TypeError("operation_ref must be an OperationRef")
        _sha256_digest(
            self.operation_identity_digest,
            "operation_identity_digest",
        )
        _nonnegative_integer(
            self.expected_fixture_generation,
            "expected_fixture_generation",
        )
        for label, value in (
            (
                "predecessor_bundle_identity",
                self.predecessor_bundle_identity,
            ),
            (
                "predecessor_graph_revision",
                self.predecessor_graph_revision,
            ),
        ):
            _bounded_identity(value, label, optional=True)
        _positive_integer(
            self.committed_fixture_generation,
            "committed_fixture_generation",
        )
        _bounded_identity(
            self.target_bundle_identity,
            "target_bundle_identity",
        )
        _bounded_identity(
            self.target_graph_revision,
            "target_graph_revision",
        )
        _sha256_digest(self.receipt_digest, "receipt_digest")
        _positive_integer(self.created_sequence, "created_sequence")
        if (
            self.committed_fixture_generation
            != self.expected_fixture_generation + 1
        ):
            raise ValueError(
                "committed fixture generation is not the CAS successor"
            )


@dataclass(frozen=True, slots=True)
class FixtureCasWriteResult:
    receipt: FixtureCasReceipt
    inserted: bool


@dataclass(frozen=True, slots=True)
class FixtureCasHead:
    generation: int
    bundle_identity: str
    graph_revision: str
    fixture_cas_id: str
    receipt_digest: str
    operation_ref: OperationRef

    def __post_init__(self) -> None:
        _positive_integer(self.generation, "fixture head generation")
        _bounded_identity(self.bundle_identity, "bundle_identity")
        _bounded_identity(self.graph_revision, "graph_revision")
        _bounded_identity(self.fixture_cas_id, "fixture_cas_id")
        _sha256_digest(self.receipt_digest, "receipt_digest")
        if not isinstance(self.operation_ref, OperationRef):
            raise TypeError("operation_ref must be an OperationRef")


@dataclass(frozen=True, slots=True)
class IntegrityReport:
    cell_id: str
    schema_id: str
    schema_version: int
    schema_artifact_digest: str
    operation_count: int
    journal_entry_count: int
    fixture_cas_receipt_count: int
    fixture_head_generation: int
    fixture_head_receipt_digest: str

    def __post_init__(self) -> None:
        _bounded_identity(self.cell_id, "cell_id")
        _bounded_identity(self.schema_id, "schema_id")
        _positive_integer(self.schema_version, "schema_version")
        _sha256_digest(
            self.schema_artifact_digest,
            "schema_artifact_digest",
        )
        for label, value in (
            ("operation_count", self.operation_count),
            ("journal_entry_count", self.journal_entry_count),
            ("fixture_cas_receipt_count", self.fixture_cas_receipt_count),
            ("fixture_head_generation", self.fixture_head_generation),
        ):
            _nonnegative_integer(value, label)
        if self.fixture_head_receipt_digest:
            _sha256_digest(
                self.fixture_head_receipt_digest,
                "fixture_head_receipt_digest",
            )


def _operation_identity_payload(
    *,
    cell_id: str,
    kind: OperationKind,
    operation_id: str,
    generation: int,
    request_id: str,
    manager_owner_epoch: str,
    intent_digest: str,
    expected_fixture_generation: int,
    predecessor_bundle_identity: str,
    predecessor_graph_revision: str,
    target_bundle_identity: str,
    target_graph_revision: str,
) -> dict[str, object]:
    return {
        "cell_id": cell_id,
        "expected_fixture_generation": (
            expected_fixture_generation
        ),
        "intent_digest": intent_digest,
        "manager_owner_epoch": manager_owner_epoch,
        "operation_generation": generation,
        "operation_id": operation_id,
        "operation_kind": kind.value,
        "predecessor_bundle_identity": predecessor_bundle_identity,
        "predecessor_graph_revision": predecessor_graph_revision,
        "request_id": request_id,
        "target_bundle_identity": target_bundle_identity,
        "target_graph_revision": target_graph_revision,
    }


def _operation_identity_digest(
    *,
    cell_id: str,
    kind: OperationKind,
    operation_id: str,
    generation: int,
    request_id: str,
    manager_owner_epoch: str,
    intent_digest: str,
    expected_fixture_generation: int,
    predecessor_bundle_identity: str,
    predecessor_graph_revision: str,
    target_bundle_identity: str,
    target_graph_revision: str,
) -> str:
    return _semantic_digest(
        "operation-identity",
        _operation_identity_payload(
            cell_id=cell_id,
            kind=kind,
            operation_id=operation_id,
            generation=generation,
            request_id=request_id,
            manager_owner_epoch=manager_owner_epoch,
            intent_digest=intent_digest,
            expected_fixture_generation=(
                expected_fixture_generation
            ),
            predecessor_bundle_identity=predecessor_bundle_identity,
            predecessor_graph_revision=predecessor_graph_revision,
            target_bundle_identity=target_bundle_identity,
            target_graph_revision=target_graph_revision,
        ),
    )


def _journal_entry_digest(
    *,
    cell_id: str,
    ref: OperationRef,
    sequence: int,
    entry_id: str,
    event_kind: str,
    payload_digest: str,
    previous_entry_digest: str,
) -> str:
    return _semantic_digest(
        "journal-entry",
        {
            "cell_id": cell_id,
            "entry_id": entry_id,
            "event_kind": event_kind,
            "operation_generation": ref.generation,
            "operation_id": ref.operation_id,
            "operation_kind": ref.kind.value,
            "payload_digest": payload_digest,
            "previous_entry_digest": previous_entry_digest,
            "sequence": sequence,
        },
    )


def _fixture_cas_intent_digest(
    *,
    cell_id: str,
    intent: FixtureCasIntent,
) -> str:
    return _semantic_digest(
        "fixture-cas-intent",
        {
            "cell_id": cell_id,
            "operation_generation": intent.operation_ref.generation,
            "operation_id": intent.operation_ref.operation_id,
            "operation_identity_digest": (
                intent.operation_identity_digest
            ),
            "operation_kind": intent.operation_ref.kind.value,
            "fixture_cas_id": intent.fixture_cas_id,
        },
    )


def _fixture_cas_receipt_digest(
    *,
    cell_id: str,
    fixture_cas_id: str,
    fixture_cas_intent_digest: str,
    operation: OperationRecord,
    committed_fixture_generation: int,
) -> str:
    return _semantic_digest(
        "fixture-cas-receipt",
        {
            "cell_id": cell_id,
            "expected_fixture_generation": (
                operation.expected_fixture_generation
            ),
            "operation_generation": operation.ref.generation,
            "operation_id": operation.ref.operation_id,
            "operation_identity_digest": operation.identity_digest,
            "operation_kind": operation.ref.kind.value,
            "predecessor_bundle_identity": (
                operation.predecessor_bundle_identity
            ),
            "predecessor_graph_revision": (
                operation.predecessor_graph_revision
            ),
            "fixture_cas_id": fixture_cas_id,
            "fixture_cas_intent_digest": fixture_cas_intent_digest,
            "committed_fixture_generation": committed_fixture_generation,
            "target_bundle_identity": operation.target_bundle_identity,
            "target_graph_revision": operation.target_graph_revision,
        },
    )


class ComponentManagerPersistenceFixture:
    """Thread-safe fixture adapter over one fixed-cell SQLite store."""

    def __init__(
        self,
        database_path: str | os.PathLike[str],
        *,
        cell_id: str,
    ) -> None:
        self._cell_id = _bounded_identity(cell_id, "cell_id")
        self._database_path = self._validated_path(database_path)
        self._lock = threading.RLock()
        self._closed = False
        try:
            self._connection = sqlite3.connect(
                str(self._database_path),
                isolation_level=None,
                timeout=SQLITE_BUSY_TIMEOUT_MS / 1000,
                check_same_thread=False,
            )
            self._connection.row_factory = sqlite3.Row
            self._configure_connection()
            self._initialize_or_validate_schema()
            self.verify_integrity()
        except Exception:
            connection = getattr(self, "_connection", None)
            if connection is not None:
                connection.close()
            raise

    @property
    def cell_id(self) -> str:
        return self._cell_id

    @property
    def database_path(self) -> Path:
        return self._database_path

    def __enter__(self) -> ComponentManagerPersistenceFixture:
        self._require_open()
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            try:
                # A pinned reader can make this best-effort checkpoint report
                # busy. Closing the writer remains correct; the WAL target is
                # deliberately not asserted as a hard footprint limit.
                self._connection.execute(
                    "PRAGMA wal_checkpoint(TRUNCATE)"
                ).fetchone()
            finally:
                self._connection.close()
                self._closed = True

    @staticmethod
    def _validated_path(
        value: str | os.PathLike[str],
    ) -> Path:
        path = Path(value)
        if str(path) in {"", ":memory:"}:
            raise ValueError("a durable filesystem database path is required")
        if path.is_symlink():
            raise ValueError("persistence database path cannot be a symlink")
        if path.exists() and not path.is_file():
            raise ValueError(
                "persistence database path must be a regular file"
            )
        parent = path.parent
        if not parent.exists() or not parent.is_dir():
            raise ValueError("persistence database parent must exist")
        return path

    def _configure_connection(self) -> None:
        connection = self._connection
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute(
            f"PRAGMA busy_timeout = {SQLITE_BUSY_TIMEOUT_MS}"
        )
        journal_mode = self._configure_wal_with_bounded_retry()
        if journal_mode != "wal":
            raise PersistenceIntegrityError(
                "SQLite WAL journal mode is required"
            )
        connection.execute("PRAGMA synchronous = FULL")
        synchronous = int(
            connection.execute("PRAGMA synchronous").fetchone()[0]
        )
        if synchronous != 2:
            raise PersistenceIntegrityError(
                "SQLite FULL synchronous mode is required"
            )
        connection.execute("PRAGMA trusted_schema = OFF")
        connection.execute("PRAGMA recursive_triggers = OFF")
        connection.execute("PRAGMA wal_autocheckpoint = 1000")
        connection.execute(
            "PRAGMA journal_size_limit = "
            f"{SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES}"
        )
        page_size = int(
            connection.execute("PRAGMA page_size").fetchone()[0]
        )
        maximum_pages = max(
            1,
            MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES // page_size,
        )
        connection.execute(f"PRAGMA max_page_count = {maximum_pages}")
        if hasattr(connection, "setlimit"):
            connection.setlimit(
                sqlite3.SQLITE_LIMIT_LENGTH,
                MAXIMUM_PAYLOAD_BYTES + 64 * 1024,
            )

    def _configure_wal_with_bounded_retry(self) -> str:
        """Handle SQLite's special concurrent first-open WAL race.

        ``PRAGMA journal_mode`` may return ``SQLITE_BUSY`` immediately even
        after a connection-level busy timeout has been installed.  This
        bounded NRT startup retry is transport glue only; it is not a Manager
        semantic deadline or scheduling source.
        """

        deadline = time.monotonic() + SQLITE_BUSY_TIMEOUT_MS / 1000
        while True:
            try:
                row = self._connection.execute(
                    "PRAGMA journal_mode = WAL"
                ).fetchone()
                if row is None:
                    raise PersistenceIntegrityError(
                        "SQLite did not report its journal mode"
                    )
                return str(row[0]).lower()
            except sqlite3.OperationalError as exc:
                code = getattr(exc, "sqlite_errorcode", None)
                busy = code in {sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED}
                if not busy or time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)

    def _initialize_or_validate_schema(self) -> None:
        with self._lock:
            self._connection.execute("BEGIN EXCLUSIVE")
            try:
                existing = _schema_objects(self._connection)
                if not existing:
                    for statement in _schema_statements():
                        self._connection.execute(statement)
                    self._connection.execute(
                        """
                        INSERT INTO persistence_metadata (
                            singleton,
                            schema_id,
                            schema_version,
                            schema_artifact_digest,
                            cell_id,
                            maximum_operations,
                            maximum_journal_entries_per_operation,
                            maximum_total_journal_entries,
                            maximum_fixture_cas_receipts,
                            maximum_payload_bytes,
                            maximum_main_database_page_image_bytes
                        ) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        (
                            PERSISTENCE_SCHEMA_ID,
                            PERSISTENCE_SCHEMA_VERSION,
                            _schema_digest(),
                            self._cell_id,
                            MAXIMUM_OPERATIONS,
                            MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION,
                            MAXIMUM_TOTAL_JOURNAL_ENTRIES,
                            MAXIMUM_FIXTURE_CAS_RECEIPTS,
                            MAXIMUM_PAYLOAD_BYTES,
                            MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES,
                        ),
                    )
                    self._connection.execute(
                        """
                        INSERT INTO fixture_cas_head (
                            singleton,
                            generation,
                            bundle_identity,
                            graph_revision,
                            fixture_cas_id,
                            receipt_digest,
                            operation_kind,
                            operation_id,
                            operation_generation
                        ) VALUES (1, 0, '', '', '', '', '', '', 0)
                        """
                    )
                elif ("table", "persistence_metadata") not in existing:
                    raise PersistenceIntegrityError(
                        "non-empty database lacks persistence metadata"
                    )
                self._validate_metadata_locked()
                self._validate_schema_objects_locked()
                self._connection.commit()
            except Exception:
                self._connection.rollback()
                raise

    def _validate_metadata_locked(self) -> sqlite3.Row:
        if (
            int(
                self._connection.execute(
                    "PRAGMA application_id"
                ).fetchone()[0]
            )
            != PERSISTENCE_FIXTURE_SQLITE_APPLICATION_ID
        ):
            raise PersistenceIntegrityError(
                "SQLite application ID mismatch"
            )
        if (
            int(
                self._connection.execute(
                    "PRAGMA user_version"
                ).fetchone()[0]
            )
            != PERSISTENCE_SCHEMA_VERSION
        ):
            raise PersistenceIntegrityError(
                "SQLite user version mismatch"
            )
        rows = self._connection.execute(
            "SELECT * FROM persistence_metadata"
        ).fetchall()
        if len(rows) != 1 or int(rows[0]["singleton"]) != 1:
            raise PersistenceIntegrityError(
                "persistence metadata singleton is invalid"
            )
        row = rows[0]
        if row["schema_id"] != PERSISTENCE_SCHEMA_ID:
            raise PersistenceIntegrityError("persistence schema ID mismatch")
        if int(row["schema_version"]) != PERSISTENCE_SCHEMA_VERSION:
            raise PersistenceIntegrityError(
                "persistence schema version mismatch"
            )
        if row["schema_artifact_digest"] != _schema_digest():
            raise PersistenceIntegrityError(
                "persistence schema artifact digest mismatch"
            )
        if row["cell_id"] != self._cell_id:
            raise PersistenceConflictError(
                "persistence store belongs to a different cell"
            )
        expected_limits = {
            "maximum_operations": MAXIMUM_OPERATIONS,
            "maximum_journal_entries_per_operation": (
                MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION
            ),
            "maximum_total_journal_entries": (
                MAXIMUM_TOTAL_JOURNAL_ENTRIES
            ),
            "maximum_fixture_cas_receipts": MAXIMUM_FIXTURE_CAS_RECEIPTS,
            "maximum_payload_bytes": MAXIMUM_PAYLOAD_BYTES,
            "maximum_main_database_page_image_bytes": (
                MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES
            ),
        }
        for field, expected in expected_limits.items():
            if int(row[field]) != expected:
                raise PersistenceIntegrityError(
                    f"persistence limit mismatch: {field}"
                )
        return row

    def _validate_schema_objects_locked(self) -> None:
        if _schema_objects(self._connection) != _expected_schema_objects():
            raise PersistenceIntegrityError(
                "SQLite schema objects differ from the versioned artifact"
            )

    @contextmanager
    def _write_transaction(self) -> Iterator[sqlite3.Connection]:
        with self._lock:
            self._require_open()
            self._connection.execute("BEGIN IMMEDIATE")
            try:
                yield self._connection
                self._connection.commit()
            except sqlite3.OperationalError as exc:
                self._connection.rollback()
                if getattr(exc, "sqlite_errorcode", None) == sqlite3.SQLITE_FULL:
                    raise PersistenceLimitError(
                        "SQLite main database page-image ceiling or storage "
                        "capacity is exhausted"
                    ) from exc
                raise
            except Exception:
                self._connection.rollback()
                raise

    def ensure_bootstrap(
        self,
        identity: BootstrapIdentity,
    ) -> OperationWriteResult:
        if not isinstance(identity, BootstrapIdentity):
            raise TypeError("identity must be a BootstrapIdentity")
        return self._ensure_operation(
            kind=OperationKind.BOOTSTRAP,
            operation_id=identity.bootstrap_id,
            generation=identity.generation,
            request_id="",
            manager_owner_epoch="",
            intent_digest=identity.intent_digest,
            expected_fixture_generation=0,
            predecessor_bundle_identity="",
            predecessor_graph_revision="",
            target_bundle_identity=identity.target_bundle_identity,
            target_graph_revision=identity.target_graph_revision,
        )

    def ensure_apply(
        self,
        identity: ApplyIdentity,
    ) -> OperationWriteResult:
        if not isinstance(identity, ApplyIdentity):
            raise TypeError("identity must be an ApplyIdentity")
        return self._ensure_operation(
            kind=OperationKind.APPLY,
            operation_id=identity.apply_id,
            generation=identity.generation,
            request_id=identity.request_id,
            manager_owner_epoch=identity.manager_owner_epoch,
            intent_digest=identity.intent_digest,
            expected_fixture_generation=(
                identity.expected_fixture_generation
            ),
            predecessor_bundle_identity=(
                identity.predecessor_bundle_identity
            ),
            predecessor_graph_revision=(
                identity.predecessor_graph_revision
            ),
            target_bundle_identity=identity.target_bundle_identity,
            target_graph_revision=identity.target_graph_revision,
        )

    def _ensure_operation(
        self,
        *,
        kind: OperationKind,
        operation_id: str,
        generation: int,
        request_id: str,
        manager_owner_epoch: str,
        intent_digest: str,
        expected_fixture_generation: int,
        predecessor_bundle_identity: str,
        predecessor_graph_revision: str,
        target_bundle_identity: str,
        target_graph_revision: str,
    ) -> OperationWriteResult:
        ref = OperationRef(kind, operation_id, generation)
        identity_digest = _operation_identity_digest(
            cell_id=self._cell_id,
            kind=kind,
            operation_id=operation_id,
            generation=generation,
            request_id=request_id,
            manager_owner_epoch=manager_owner_epoch,
            intent_digest=intent_digest,
            expected_fixture_generation=(
                expected_fixture_generation
            ),
            predecessor_bundle_identity=predecessor_bundle_identity,
            predecessor_graph_revision=predecessor_graph_revision,
            target_bundle_identity=target_bundle_identity,
            target_graph_revision=target_graph_revision,
        )
        with self._write_transaction() as connection:
            if kind is OperationKind.APPLY:
                request_row = connection.execute(
                    """
                    SELECT * FROM manager_operations
                    WHERE operation_kind = 'apply'
                      AND manager_owner_epoch = ?
                      AND request_id = ?
                    """,
                    (manager_owner_epoch, request_id),
                ).fetchone()
                if request_row is not None:
                    request_record = self._operation_from_row(request_row)
                    if (
                        request_record.ref != ref
                        or request_record.identity_digest != identity_digest
                    ):
                        raise PersistenceConflictError(
                            "Apply request identity already exists with "
                            "different immutable content"
                        )
                    return OperationWriteResult(
                        record=request_record,
                        inserted=False,
                    )
            existing = self._operation_row_locked(connection, ref)
            if existing is not None:
                record = self._operation_from_row(existing)
                if record.identity_digest != identity_digest:
                    raise PersistenceConflictError(
                        "operation identity already exists with different "
                        "immutable content"
                    )
                return OperationWriteResult(record=record, inserted=False)
            if kind is OperationKind.BOOTSTRAP:
                other_bootstrap = connection.execute(
                    """
                    SELECT 1 FROM manager_operations
                    WHERE operation_kind = 'bootstrap'
                    """
                ).fetchone()
                if other_bootstrap is not None:
                    raise PersistenceConflictError(
                        "a different bootstrap identity is already fixed"
                    )
            count = int(
                connection.execute(
                    "SELECT COUNT(*) FROM manager_operations"
                ).fetchone()[0]
            )
            if count >= MAXIMUM_OPERATIONS:
                raise PersistenceLimitError(
                    "operation record limit is exhausted"
                )
            created_sequence = count + 1
            connection.execute(
                """
                INSERT INTO manager_operations (
                    operation_kind,
                    operation_id,
                    operation_generation,
                    request_id,
                    manager_owner_epoch,
                    intent_digest,
                    expected_fixture_generation,
                    predecessor_bundle_identity,
                    predecessor_graph_revision,
                    target_bundle_identity,
                    target_graph_revision,
                    identity_digest,
                    created_sequence
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    kind.value,
                    operation_id,
                    generation,
                    request_id,
                    manager_owner_epoch,
                    intent_digest,
                    expected_fixture_generation,
                    predecessor_bundle_identity,
                    predecessor_graph_revision,
                    target_bundle_identity,
                    target_graph_revision,
                    identity_digest,
                    created_sequence,
                ),
            )
            row = self._operation_row_locked(connection, ref)
            assert row is not None
            return OperationWriteResult(
                record=self._operation_from_row(row),
                inserted=True,
            )

    def operation(self, ref: OperationRef) -> OperationRecord | None:
        if not isinstance(ref, OperationRef):
            raise TypeError("ref must be an OperationRef")
        with self._lock:
            self._require_open()
            row = self._operation_row_locked(self._connection, ref)
            return None if row is None else self._operation_from_row(row)

    def operations(self) -> tuple[OperationRecord, ...]:
        with self._lock:
            self._require_open()
            rows = self._connection.execute(
                """
                SELECT * FROM manager_operations
                ORDER BY created_sequence
                """
            ).fetchall()
            return tuple(self._operation_from_row(row) for row in rows)

    @staticmethod
    def _operation_row_locked(
        connection: sqlite3.Connection,
        ref: OperationRef,
    ) -> sqlite3.Row | None:
        return connection.execute(
            """
            SELECT * FROM manager_operations
            WHERE operation_kind = ?
              AND operation_id = ?
              AND operation_generation = ?
            """,
            (ref.kind.value, ref.operation_id, ref.generation),
        ).fetchone()

    @staticmethod
    def _operation_from_row(row: sqlite3.Row) -> OperationRecord:
        return OperationRecord(
            ref=OperationRef(
                OperationKind(row["operation_kind"]),
                row["operation_id"],
                int(row["operation_generation"]),
            ),
            request_id=row["request_id"],
            manager_owner_epoch=row["manager_owner_epoch"],
            intent_digest=row["intent_digest"],
            expected_fixture_generation=int(
                row["expected_fixture_generation"]
            ),
            predecessor_bundle_identity=(
                row["predecessor_bundle_identity"]
            ),
            predecessor_graph_revision=(
                row["predecessor_graph_revision"]
            ),
            target_bundle_identity=row["target_bundle_identity"],
            target_graph_revision=row["target_graph_revision"],
            identity_digest=row["identity_digest"],
            created_sequence=int(row["created_sequence"]),
        )

    def append_journal_event(
        self,
        ref: OperationRef,
        event: JournalEvent,
    ) -> JournalAppendResult:
        if not isinstance(ref, OperationRef):
            raise TypeError("ref must be an OperationRef")
        if not isinstance(event, JournalEvent):
            raise TypeError("event must be a JournalEvent")
        payload_digest = _raw_digest(event.payload)
        with self._write_transaction() as connection:
            operation_row = self._operation_row_locked(connection, ref)
            if operation_row is None:
                raise PersistenceConflictError(
                    "journal event references an unknown operation"
                )
            operation = self._operation_from_row(operation_row)
            existing = connection.execute(
                """
                SELECT * FROM manager_journal_entries
                WHERE operation_kind = ?
                  AND operation_id = ?
                  AND operation_generation = ?
                  AND entry_id = ?
                """,
                (
                    ref.kind.value,
                    ref.operation_id,
                    ref.generation,
                    event.entry_id,
                ),
            ).fetchone()
            if existing is not None:
                entry = self._journal_from_row(existing)
                if (
                    entry.event_kind != event.event_kind
                    or entry.payload != event.payload
                    or entry.payload_digest != payload_digest
                ):
                    raise PersistenceConflictError(
                        "journal entry ID already exists with different "
                        "immutable content"
                    )
                return JournalAppendResult(entry=entry, inserted=False)

            per_operation_count = int(
                connection.execute(
                    """
                    SELECT COUNT(*) FROM manager_journal_entries
                    WHERE operation_kind = ?
                      AND operation_id = ?
                      AND operation_generation = ?
                    """,
                    (
                        ref.kind.value,
                        ref.operation_id,
                        ref.generation,
                    ),
                ).fetchone()[0]
            )
            if (
                per_operation_count
                >= MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION
            ):
                raise PersistenceLimitError(
                    "per-operation journal limit is exhausted"
                )
            total_count = int(
                connection.execute(
                    "SELECT COUNT(*) FROM manager_journal_entries"
                ).fetchone()[0]
            )
            if total_count >= MAXIMUM_TOTAL_JOURNAL_ENTRIES:
                raise PersistenceLimitError(
                    "total journal entry limit is exhausted"
                )
            previous = connection.execute(
                """
                SELECT entry_digest
                FROM manager_journal_entries
                WHERE operation_kind = ?
                  AND operation_id = ?
                  AND operation_generation = ?
                ORDER BY sequence DESC
                LIMIT 1
                """,
                (
                    ref.kind.value,
                    ref.operation_id,
                    ref.generation,
                ),
            ).fetchone()
            sequence = per_operation_count + 1
            previous_digest = (
                operation.identity_digest
                if previous is None
                else previous["entry_digest"]
            )
            entry_digest = _journal_entry_digest(
                cell_id=self._cell_id,
                ref=ref,
                sequence=sequence,
                entry_id=event.entry_id,
                event_kind=event.event_kind,
                payload_digest=payload_digest,
                previous_entry_digest=previous_digest,
            )
            connection.execute(
                """
                INSERT INTO manager_journal_entries (
                    operation_kind,
                    operation_id,
                    operation_generation,
                    sequence,
                    entry_id,
                    event_kind,
                    payload,
                    payload_digest,
                    previous_entry_digest,
                    entry_digest
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    ref.kind.value,
                    ref.operation_id,
                    ref.generation,
                    sequence,
                    event.entry_id,
                    event.event_kind,
                    sqlite3.Binary(event.payload),
                    payload_digest,
                    previous_digest,
                    entry_digest,
                ),
            )
            row = connection.execute(
                """
                SELECT * FROM manager_journal_entries
                WHERE operation_kind = ?
                  AND operation_id = ?
                  AND operation_generation = ?
                  AND sequence = ?
                """,
                (
                    ref.kind.value,
                    ref.operation_id,
                    ref.generation,
                    sequence,
                ),
            ).fetchone()
            assert row is not None
            return JournalAppendResult(
                entry=self._journal_from_row(row),
                inserted=True,
            )

    def journal(self, ref: OperationRef) -> tuple[JournalEntry, ...]:
        if not isinstance(ref, OperationRef):
            raise TypeError("ref must be an OperationRef")
        with self._lock:
            self._require_open()
            rows = self._connection.execute(
                """
                SELECT * FROM manager_journal_entries
                WHERE operation_kind = ?
                  AND operation_id = ?
                  AND operation_generation = ?
                ORDER BY sequence
                """,
                (ref.kind.value, ref.operation_id, ref.generation),
            ).fetchall()
            return tuple(self._journal_from_row(row) for row in rows)

    @staticmethod
    def _journal_from_row(row: sqlite3.Row) -> JournalEntry:
        return JournalEntry(
            ref=OperationRef(
                OperationKind(row["operation_kind"]),
                row["operation_id"],
                int(row["operation_generation"]),
            ),
            sequence=int(row["sequence"]),
            entry_id=row["entry_id"],
            event_kind=row["event_kind"],
            payload=bytes(row["payload"]),
            payload_digest=row["payload_digest"],
            previous_entry_digest=row["previous_entry_digest"],
            entry_digest=row["entry_digest"],
        )

    def advance_fixture_head(
        self,
        intent: FixtureCasIntent,
    ) -> FixtureCasWriteResult:
        if not isinstance(intent, FixtureCasIntent):
            raise TypeError("intent must be a FixtureCasIntent")
        fixture_cas_intent_digest = _fixture_cas_intent_digest(
            cell_id=self._cell_id,
            intent=intent,
        )
        with self._write_transaction() as connection:
            existing = connection.execute(
                """
                SELECT * FROM fixture_cas_receipts
                WHERE fixture_cas_id = ?
                """,
                (intent.fixture_cas_id,),
            ).fetchone()
            if existing is not None:
                receipt = self._fixture_cas_receipt_from_row(existing)
                if (
                    receipt.fixture_cas_intent_digest
                    != fixture_cas_intent_digest
                    or receipt.operation_ref != intent.operation_ref
                    or receipt.operation_identity_digest
                    != intent.operation_identity_digest
                ):
                    raise PersistenceConflictError(
                        "fixture CAS ID already exists with different "
                        "immutable content"
                    )
                return FixtureCasWriteResult(
                    receipt=receipt,
                    inserted=False,
                )

            operation_row = self._operation_row_locked(
                connection,
                intent.operation_ref,
            )
            if operation_row is None:
                raise PersistenceConflictError(
                    "fixture CAS references an unknown operation"
                )
            operation = self._operation_from_row(operation_row)
            if (
                operation.identity_digest
                != intent.operation_identity_digest
            ):
                raise PersistenceConflictError(
                    "fixture CAS operation identity digest mismatch"
                )
            if (
                operation.expected_fixture_generation
                == SQLITE_SIGNED_INT64_MAX
            ):
                raise PersistenceLimitError(
                    "fixture CAS generation exhausted signed SQLite int64"
                )
            current = connection.execute(
                "SELECT * FROM fixture_cas_head WHERE singleton = 1"
            ).fetchone()
            if current is None:
                raise PersistenceIntegrityError(
                    "fixture CAS head singleton is missing"
                )
            if (
                int(current["generation"])
                != operation.expected_fixture_generation
                or current["bundle_identity"]
                != operation.predecessor_bundle_identity
                or current["graph_revision"]
                != operation.predecessor_graph_revision
            ):
                raise PersistenceConflictError(
                    "fixture CAS head precondition failed"
                )
            fixture_cas_receipt_count = int(
                connection.execute(
                    "SELECT COUNT(*) FROM fixture_cas_receipts"
                ).fetchone()[0]
            )
            if fixture_cas_receipt_count >= MAXIMUM_FIXTURE_CAS_RECEIPTS:
                raise PersistenceLimitError(
                    "fixture CAS receipt limit is exhausted"
                )
            if (
                operation.ref.kind is OperationKind.APPLY
                and fixture_cas_receipt_count == 0
            ):
                raise PersistenceConflictError(
                    "the first fixture CAS advance must be bootstrap"
                )
            committed_fixture_generation = (
                operation.expected_fixture_generation + 1
            )
            receipt_digest = _fixture_cas_receipt_digest(
                cell_id=self._cell_id,
                fixture_cas_id=intent.fixture_cas_id,
                fixture_cas_intent_digest=fixture_cas_intent_digest,
                operation=operation,
                committed_fixture_generation=committed_fixture_generation,
            )
            created_sequence = fixture_cas_receipt_count + 1
            connection.execute(
                """
                INSERT INTO fixture_cas_receipts (
                    fixture_cas_id,
                    fixture_cas_intent_digest,
                    operation_kind,
                    operation_id,
                    operation_generation,
                    operation_identity_digest,
                    expected_fixture_generation,
                    predecessor_bundle_identity,
                    predecessor_graph_revision,
                    committed_fixture_generation,
                    target_bundle_identity,
                    target_graph_revision,
                    receipt_digest,
                    created_sequence
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    intent.fixture_cas_id,
                    fixture_cas_intent_digest,
                    operation.ref.kind.value,
                    operation.ref.operation_id,
                    operation.ref.generation,
                    operation.identity_digest,
                    operation.expected_fixture_generation,
                    operation.predecessor_bundle_identity,
                    operation.predecessor_graph_revision,
                    committed_fixture_generation,
                    operation.target_bundle_identity,
                    operation.target_graph_revision,
                    receipt_digest,
                    created_sequence,
                ),
            )
            updated = connection.execute(
                """
                UPDATE fixture_cas_head
                SET generation = ?,
                    bundle_identity = ?,
                    graph_revision = ?,
                    fixture_cas_id = ?,
                    receipt_digest = ?,
                    operation_kind = ?,
                    operation_id = ?,
                    operation_generation = ?
                WHERE singleton = 1
                  AND generation = ?
                  AND bundle_identity = ?
                  AND graph_revision = ?
                """,
                (
                    committed_fixture_generation,
                    operation.target_bundle_identity,
                    operation.target_graph_revision,
                    intent.fixture_cas_id,
                    receipt_digest,
                    operation.ref.kind.value,
                    operation.ref.operation_id,
                    operation.ref.generation,
                    operation.expected_fixture_generation,
                    operation.predecessor_bundle_identity,
                    operation.predecessor_graph_revision,
                ),
            )
            if updated.rowcount != 1:
                raise PersistenceConflictError(
                    "fixture CAS head advance lost its race"
                )
            row = connection.execute(
                """
                SELECT * FROM fixture_cas_receipts
                WHERE fixture_cas_id = ?
                """,
                (intent.fixture_cas_id,),
            ).fetchone()
            assert row is not None
            return FixtureCasWriteResult(
                receipt=self._fixture_cas_receipt_from_row(row),
                inserted=True,
            )

    def fixture_cas_head(self) -> FixtureCasHead | None:
        with self._lock:
            self._require_open()
            row = self._connection.execute(
                "SELECT * FROM fixture_cas_head WHERE singleton = 1"
            ).fetchone()
            if row is None:
                raise PersistenceIntegrityError(
                    "fixture CAS head singleton is missing"
                )
            if int(row["generation"]) == 0:
                return None
            return self._fixture_cas_head_from_row(row)

    def fixture_cas_receipt(
        self,
        fixture_cas_id: str,
    ) -> FixtureCasReceipt | None:
        _bounded_identity(fixture_cas_id, "fixture_cas_id")
        with self._lock:
            self._require_open()
            row = self._connection.execute(
                """
                SELECT * FROM fixture_cas_receipts
                WHERE fixture_cas_id = ?
                """,
                (fixture_cas_id,),
            ).fetchone()
            return (
                None
                if row is None
                else self._fixture_cas_receipt_from_row(row)
            )

    def fixture_cas_receipts(self) -> tuple[FixtureCasReceipt, ...]:
        with self._lock:
            self._require_open()
            rows = self._connection.execute(
                """
                SELECT * FROM fixture_cas_receipts
                ORDER BY committed_fixture_generation
                """
            ).fetchall()
            return tuple(
                self._fixture_cas_receipt_from_row(row)
                for row in rows
            )

    @staticmethod
    def _fixture_cas_receipt_from_row(row: sqlite3.Row) -> FixtureCasReceipt:
        return FixtureCasReceipt(
            fixture_cas_id=row["fixture_cas_id"],
            fixture_cas_intent_digest=row["fixture_cas_intent_digest"],
            operation_ref=OperationRef(
                OperationKind(row["operation_kind"]),
                row["operation_id"],
                int(row["operation_generation"]),
            ),
            operation_identity_digest=row["operation_identity_digest"],
            expected_fixture_generation=int(
                row["expected_fixture_generation"]
            ),
            predecessor_bundle_identity=(
                row["predecessor_bundle_identity"]
            ),
            predecessor_graph_revision=(
                row["predecessor_graph_revision"]
            ),
            committed_fixture_generation=int(row["committed_fixture_generation"]),
            target_bundle_identity=row["target_bundle_identity"],
            target_graph_revision=row["target_graph_revision"],
            receipt_digest=row["receipt_digest"],
            created_sequence=int(row["created_sequence"]),
        )

    @staticmethod
    def _fixture_cas_head_from_row(row: sqlite3.Row) -> FixtureCasHead:
        return FixtureCasHead(
            generation=int(row["generation"]),
            bundle_identity=row["bundle_identity"],
            graph_revision=row["graph_revision"],
            fixture_cas_id=row["fixture_cas_id"],
            receipt_digest=row["receipt_digest"],
            operation_ref=OperationRef(
                OperationKind(row["operation_kind"]),
                row["operation_id"],
                int(row["operation_generation"]),
            ),
        )

    def verify_integrity(self) -> IntegrityReport:
        with self._lock:
            self._require_open()
            self._connection.execute("BEGIN")
            try:
                report = self._verify_integrity_locked()
                self._connection.commit()
                return report
            except Exception:
                self._connection.rollback()
                raise

    def _verify_integrity_locked(self) -> IntegrityReport:
        integrity_rows = self._connection.execute(
            "PRAGMA integrity_check"
        ).fetchall()
        if [str(row[0]) for row in integrity_rows] != ["ok"]:
            raise PersistenceIntegrityError("SQLite integrity_check failed")
        foreign_rows = self._connection.execute(
            "PRAGMA foreign_key_check"
        ).fetchall()
        if foreign_rows:
            raise PersistenceIntegrityError(
                "SQLite foreign-key integrity check failed"
            )
        metadata = self._validate_metadata_locked()
        self._validate_schema_objects_locked()

        operation_count = int(
            self._connection.execute(
                "SELECT COUNT(*) FROM manager_operations"
            ).fetchone()[0]
        )
        journal_count = int(
            self._connection.execute(
                "SELECT COUNT(*) FROM manager_journal_entries"
            ).fetchone()[0]
        )
        fixture_cas_receipt_count = int(
            self._connection.execute(
                "SELECT COUNT(*) FROM fixture_cas_receipts"
            ).fetchone()[0]
        )
        if operation_count > MAXIMUM_OPERATIONS:
            raise PersistenceIntegrityError("operation count exceeds limit")
        if journal_count > MAXIMUM_TOTAL_JOURNAL_ENTRIES:
            raise PersistenceIntegrityError(
                "journal entry count exceeds total limit"
            )
        if fixture_cas_receipt_count > MAXIMUM_FIXTURE_CAS_RECEIPTS:
            raise PersistenceIntegrityError(
                "fixture CAS receipt count exceeds limit"
            )

        operations = self.operations()
        if tuple(item.created_sequence for item in operations) != tuple(
            range(1, operation_count + 1)
        ):
            raise PersistenceIntegrityError(
                "operation creation sequence is not contiguous"
            )
        operation_by_ref = {item.ref: item for item in operations}
        bootstrap_count = sum(
            item.ref.kind is OperationKind.BOOTSTRAP
            for item in operations
        )
        if bootstrap_count > 1:
            raise PersistenceIntegrityError(
                "more than one bootstrap operation exists"
            )
        for operation in operations:
            expected_digest = _operation_identity_digest(
                cell_id=self._cell_id,
                kind=operation.ref.kind,
                operation_id=operation.ref.operation_id,
                generation=operation.ref.generation,
                request_id=operation.request_id,
                manager_owner_epoch=operation.manager_owner_epoch,
                intent_digest=operation.intent_digest,
                expected_fixture_generation=(
                    operation.expected_fixture_generation
                ),
                predecessor_bundle_identity=(
                    operation.predecessor_bundle_identity
                ),
                predecessor_graph_revision=(
                    operation.predecessor_graph_revision
                ),
                target_bundle_identity=(
                    operation.target_bundle_identity
                ),
                target_graph_revision=(
                    operation.target_graph_revision
                ),
            )
            if operation.identity_digest != expected_digest:
                raise PersistenceIntegrityError(
                    "operation identity digest mismatch"
                )
            entries = self.journal(operation.ref)
            if len(entries) > MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION:
                raise PersistenceIntegrityError(
                    "per-operation journal count exceeds limit"
                )
            previous_digest = operation.identity_digest
            for expected_sequence, entry in enumerate(entries, start=1):
                if entry.sequence != expected_sequence:
                    raise PersistenceIntegrityError(
                        "journal sequence is not contiguous"
                    )
                if entry.payload_digest != _raw_digest(entry.payload):
                    raise PersistenceIntegrityError(
                        "journal payload digest mismatch"
                    )
                if entry.previous_entry_digest != previous_digest:
                    raise PersistenceIntegrityError(
                        "journal hash-chain predecessor mismatch"
                    )
                expected_entry_digest = _journal_entry_digest(
                    cell_id=self._cell_id,
                    ref=entry.ref,
                    sequence=entry.sequence,
                    entry_id=entry.entry_id,
                    event_kind=entry.event_kind,
                    payload_digest=entry.payload_digest,
                    previous_entry_digest=entry.previous_entry_digest,
                )
                if entry.entry_digest != expected_entry_digest:
                    raise PersistenceIntegrityError(
                        "journal entry digest mismatch"
                    )
                previous_digest = entry.entry_digest

        receipts = self.fixture_cas_receipts()
        prior_generation = 0
        prior_bundle = ""
        prior_graph = ""
        for expected_sequence, receipt in enumerate(receipts, start=1):
            if receipt.created_sequence != expected_sequence:
                raise PersistenceIntegrityError(
                    "fixture CAS creation sequence is not contiguous"
                )
            if receipt.committed_fixture_generation != expected_sequence:
                raise PersistenceIntegrityError(
                    "fixture CAS generation is not contiguous"
                )
            if (
                receipt.expected_fixture_generation
                != prior_generation
                or receipt.predecessor_bundle_identity != prior_bundle
                or receipt.predecessor_graph_revision != prior_graph
            ):
                raise PersistenceIntegrityError(
                    "fixture CAS predecessor chain mismatch"
                )
            if expected_sequence == 1:
                if receipt.operation_ref.kind is not OperationKind.BOOTSTRAP:
                    raise PersistenceIntegrityError(
                        "first fixture CAS advance is not bootstrap"
                    )
            elif receipt.operation_ref.kind is OperationKind.BOOTSTRAP:
                raise PersistenceIntegrityError(
                    "bootstrap fixture CAS appears after generation one"
                )
            operation = operation_by_ref.get(receipt.operation_ref)
            if operation is None:
                raise PersistenceIntegrityError(
                    "fixture CAS references a missing operation"
                )
            if (
                receipt.operation_identity_digest
                != operation.identity_digest
                or receipt.target_bundle_identity
                != operation.target_bundle_identity
                or receipt.target_graph_revision
                != operation.target_graph_revision
                or receipt.expected_fixture_generation
                != operation.expected_fixture_generation
                or receipt.predecessor_bundle_identity
                != operation.predecessor_bundle_identity
                or receipt.predecessor_graph_revision
                != operation.predecessor_graph_revision
            ):
                raise PersistenceIntegrityError(
                    "fixture CAS does not match its immutable operation"
                )
            intent = FixtureCasIntent(
                fixture_cas_id=receipt.fixture_cas_id,
                operation_ref=receipt.operation_ref,
                operation_identity_digest=(
                    receipt.operation_identity_digest
                ),
            )
            expected_intent_digest = _fixture_cas_intent_digest(
                cell_id=self._cell_id,
                intent=intent,
            )
            if (
                receipt.fixture_cas_intent_digest
                != expected_intent_digest
            ):
                raise PersistenceIntegrityError(
                    "fixture CAS intent digest mismatch"
                )
            expected_receipt_digest = _fixture_cas_receipt_digest(
                cell_id=self._cell_id,
                fixture_cas_id=receipt.fixture_cas_id,
                fixture_cas_intent_digest=expected_intent_digest,
                operation=operation,
                committed_fixture_generation=(
                    receipt.committed_fixture_generation
                ),
            )
            if receipt.receipt_digest != expected_receipt_digest:
                raise PersistenceIntegrityError(
                    "fixture CAS receipt digest mismatch"
                )
            prior_generation = receipt.committed_fixture_generation
            prior_bundle = receipt.target_bundle_identity
            prior_graph = receipt.target_graph_revision

        head_rows = self._connection.execute(
            "SELECT * FROM fixture_cas_head"
        ).fetchall()
        if len(head_rows) != 1 or int(head_rows[0]["singleton"]) != 1:
            raise PersistenceIntegrityError(
                "fixture CAS head singleton is invalid"
            )
        head_row = head_rows[0]
        if not receipts:
            if (
                int(head_row["generation"]) != 0
                or head_row["bundle_identity"]
                or head_row["graph_revision"]
                or head_row["fixture_cas_id"]
                or head_row["receipt_digest"]
                or head_row["operation_kind"]
                or head_row["operation_id"]
                or int(head_row["operation_generation"]) != 0
            ):
                raise PersistenceIntegrityError(
                    "empty fixture CAS history has a non-empty head"
                )
            fixture_head_receipt_digest = ""
        else:
            last = receipts[-1]
            active = self._fixture_cas_head_from_row(head_row)
            if (
                active.generation != last.committed_fixture_generation
                or active.bundle_identity != last.target_bundle_identity
                or active.graph_revision != last.target_graph_revision
                or active.fixture_cas_id != last.fixture_cas_id
                or active.receipt_digest != last.receipt_digest
                or active.operation_ref != last.operation_ref
            ):
                raise PersistenceIntegrityError(
                    "fixture CAS head does not match its last receipt"
                )
            fixture_head_receipt_digest = active.receipt_digest

        page_size = int(
            self._connection.execute("PRAGMA page_size").fetchone()[0]
        )
        page_count = int(
            self._connection.execute("PRAGMA page_count").fetchone()[0]
        )
        if page_size * page_count > MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES:
            raise PersistenceIntegrityError(
                "SQLite main database page image exceeds its ceiling"
            )
        return IntegrityReport(
            cell_id=metadata["cell_id"],
            schema_id=metadata["schema_id"],
            schema_version=int(metadata["schema_version"]),
            schema_artifact_digest=metadata["schema_artifact_digest"],
            operation_count=operation_count,
            journal_entry_count=journal_count,
            fixture_cas_receipt_count=fixture_cas_receipt_count,
            fixture_head_generation=prior_generation,
            fixture_head_receipt_digest=fixture_head_receipt_digest,
        )

    def _require_open(self) -> None:
        if self._closed:
            raise PersistenceClosedError(
                "persistence reference adapter is closed"
            )


__all__ = [
    "ApplyIdentity",
    "BootstrapIdentity",
    "ComponentManagerPersistenceFixture",
    "FixtureCasHead",
    "FixtureCasIntent",
    "FixtureCasReceipt",
    "FixtureCasWriteResult",
    "IntegrityReport",
    "JournalAppendResult",
    "JournalEntry",
    "JournalEvent",
    "MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES",
    "MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION",
    "MAXIMUM_OPERATIONS",
    "MAXIMUM_PAYLOAD_BYTES",
    "MAXIMUM_FIXTURE_CAS_RECEIPTS",
    "MAXIMUM_TOTAL_JOURNAL_ENTRIES",
    "OperationKind",
    "OperationRecord",
    "OperationRef",
    "OperationWriteResult",
    "PERSISTENCE_SCHEMA_ID",
    "PERSISTENCE_SCHEMA_VERSION",
    "PERSISTENCE_FIXTURE_SQLITE_APPLICATION_ID",
    "PersistenceClosedError",
    "PersistenceConflictError",
    "PersistenceError",
    "PersistenceIntegrityError",
    "PersistenceLimitError",
    "SQLITE_SIGNED_INT64_MAX",
    "SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES",
]
