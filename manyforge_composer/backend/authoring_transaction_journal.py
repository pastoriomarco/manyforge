"""Bounded durable transaction journal for Composer-owned authoring effects."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import threading
import time
import uuid
from typing import Any, Mapping

from .authoring_identity import canonical_digest


_SCHEMA_VERSION = 1
_MAXIMUM_TRANSACTIONS = 1024
_DOMAIN = b"manyforge.composer.authoring-transaction.v1\0"
_DATABASE_NAME = "composer-authoring-transactions.sqlite3"
_DATABASE_LEAVES = (
    _DATABASE_NAME,
    f"{_DATABASE_NAME}-journal",
    f"{_DATABASE_NAME}-wal",
    f"{_DATABASE_NAME}-shm",
)


class AuthoringJournalError(RuntimeError):
    """Typed fail-closed journal/recovery failure."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class AuthoringTransaction:
    request_owner_epoch: str
    request_id: str
    owner_transaction_id: str
    operation_kind: str
    payload_digest: str
    draft_id: str
    draft_revision: int
    admission_generation: int
    owner_epoch: str
    claim_intent: dict[str, Any]
    acquire_attempted: bool
    claim: dict[str, Any] | None
    effect_intent: dict[str, Any] | None
    outcome: dict[str, Any] | None
    post_image: dict[str, Any] | None
    release_intent: dict[str, Any] | None
    release_receipt: dict[str, Any] | None

    @property
    def complete(self) -> bool:
        return self.release_receipt is not None


def owner_transaction_id(request_owner_epoch: str, request_id: str) -> str:
    digest = hashlib.sha256(
        _DOMAIN + request_owner_epoch.encode() + b"\0" + request_id.encode()
    ).hexdigest()
    return f"composer-authoring.{digest}"


class ComposerAuthoringTransactionJournal:
    """SQLite owner journal; it is deliberately not general Draft history."""

    def __init__(
        self, state_root: Path, *, maximum_transactions: int = _MAXIMUM_TRANSACTIONS
    ) -> None:
        if not 1 <= maximum_transactions <= _MAXIMUM_TRANSACTIONS:
            raise ValueError("authoring journal capacity is invalid")
        self._capacity = maximum_transactions
        self._lock = threading.RLock()
        self._state_root = Path(state_root)
        self._root_descriptor = self._open_state_root(self._state_root)
        try:
            self._root_identity = self._identity(os.fstat(self._root_descriptor))
            self._database_identity = self._prepare_database_leaf()
            self._database = sqlite3.connect(
                f"file:/proc/self/fd/{self._root_descriptor}/{_DATABASE_NAME}?mode=rw",
                uri=True,
                isolation_level="IMMEDIATE",
                check_same_thread=False,
            )
            self._validate_storage()
            self._database.execute("PRAGMA journal_mode=WAL")
            self._database.execute("PRAGMA synchronous=FULL")
            self._database.execute("PRAGMA foreign_keys=ON")
            self._initialize()
            self.owner_epoch = self._begin_owner_epoch()
            self._validate_storage()
        except Exception:
            database = getattr(self, "_database", None)
            if database is not None:
                database.close()
            os.close(self._root_descriptor)
            self._root_descriptor = -1
            raise

    @staticmethod
    def _identity(metadata: os.stat_result) -> tuple[int, int]:
        return metadata.st_dev, metadata.st_ino

    @staticmethod
    def _safe_directory(metadata: os.stat_result) -> bool:
        required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        return (
            stat.S_ISDIR(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & required == required
        )

    @staticmethod
    def _safe_database_file(metadata: os.stat_result) -> bool:
        return (
            stat.S_ISREG(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and metadata.st_nlink == 1
            and metadata.st_mode & 0o777 == 0o600
        )

    @classmethod
    def _open_state_root(cls, state_root: Path) -> int:
        try:
            before = state_root.lstat()
            canonical = state_root.resolve(strict=True)
            descriptor = os.open(
                state_root,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as error:
            raise AuthoringJournalError(
                "composer_authoring_state_root_invalid",
                f"Composer authoring state root is unavailable: {error}",
            ) from error
        try:
            opened = os.fstat(descriptor)
            after = state_root.lstat()
            identity = cls._identity(before)
            if (
                not state_root.is_absolute()
                or canonical != state_root
                or any(
                    not cls._safe_directory(metadata)
                    or cls._identity(metadata) != identity
                    for metadata in (before, opened, after)
                )
            ):
                raise AuthoringJournalError(
                    "composer_authoring_state_root_invalid",
                    "Composer authoring state root is not one safe canonical "
                    "directory capability",
                )
            return descriptor
        except Exception:
            os.close(descriptor)
            raise

    def _prepare_database_leaf(self) -> tuple[int, int]:
        try:
            before = os.stat(
                _DATABASE_NAME,
                dir_fd=self._root_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            orphaned_sidecars: list[str] = []
            for leaf in _DATABASE_LEAVES[1:]:
                try:
                    os.stat(
                        leaf,
                        dir_fd=self._root_descriptor,
                        follow_symlinks=False,
                    )
                except FileNotFoundError:
                    continue
                except OSError as error:
                    raise AuthoringJournalError(
                        "composer_authoring_journal_invalid",
                        "Composer authoring journal absence cannot be verified: "
                        f"{error}",
                    ) from error
                orphaned_sidecars.append(leaf)
            if orphaned_sidecars:
                raise AuthoringJournalError(
                    "composer_authoring_journal_invalid",
                    "Composer authoring journal main database is absent but "
                    "orphan sidecars remain: " + ", ".join(orphaned_sidecars),
                )
            try:
                descriptor = os.open(
                    _DATABASE_NAME,
                    os.O_RDWR
                    | os.O_CREAT
                    | os.O_EXCL
                    | os.O_CLOEXEC
                    | os.O_NOFOLLOW,
                    0o600,
                    dir_fd=self._root_descriptor,
                )
            except OSError as error:
                raise AuthoringJournalError(
                    "composer_authoring_journal_invalid",
                    f"Composer authoring journal cannot be reserved: {error}",
                ) from error
            try:
                os.fchmod(descriptor, 0o600)
                os.fsync(descriptor)
                before = os.fstat(descriptor)
                if not self._safe_database_file(before):
                    raise AuthoringJournalError(
                        "composer_authoring_journal_invalid",
                        "new Composer authoring journal leaf is unsafe",
                    )
            finally:
                os.close(descriptor)
            os.fsync(self._root_descriptor)
        except OSError as error:
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                f"Composer authoring journal cannot be inspected: {error}",
            ) from error
        if not self._safe_database_file(before):
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                "Composer authoring journal type, owner, link count, or mode is unsafe",
            )
        identity = self._identity(before)
        try:
            descriptor = os.open(
                _DATABASE_NAME,
                os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=self._root_descriptor,
            )
            try:
                opened = os.fstat(descriptor)
                after = os.stat(
                    _DATABASE_NAME,
                    dir_fd=self._root_descriptor,
                    follow_symlinks=False,
                )
                if any(
                    not self._safe_database_file(metadata)
                    or self._identity(metadata) != identity
                    for metadata in (opened, after)
                ):
                    raise AuthoringJournalError(
                        "composer_authoring_journal_invalid",
                        "Composer authoring journal changed while opening",
                    )
            finally:
                os.close(descriptor)
            for leaf in _DATABASE_LEAVES[1:]:
                self._validate_optional_leaf(leaf)
        except OSError as error:
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                f"Composer authoring journal cannot be opened safely: {error}",
            ) from error
        return identity

    def _validate_optional_leaf(self, leaf: str) -> None:
        try:
            metadata = os.stat(
                leaf,
                dir_fd=self._root_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            return
        if not self._safe_database_file(metadata):
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                f"Composer authoring journal sidecar is unsafe: {leaf}",
            )

    def _validate_storage(self) -> None:
        if self._root_descriptor < 0:
            raise AuthoringJournalError(
                "composer_authoring_journal_closed",
                "Composer authoring journal is closed",
            )
        try:
            opened_root = os.fstat(self._root_descriptor)
            current_root = self._state_root.lstat()
            current_database = os.stat(
                _DATABASE_NAME,
                dir_fd=self._root_descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                f"Composer authoring journal storage is unavailable: {error}",
            ) from error
        if (
            any(
                not self._safe_directory(metadata)
                or self._identity(metadata) != self._root_identity
                for metadata in (opened_root, current_root)
            )
            or not self._safe_database_file(current_database)
            or self._identity(current_database) != self._database_identity
        ):
            raise AuthoringJournalError(
                "composer_authoring_journal_invalid",
                "Composer authoring journal storage capability was replaced or changed",
            )
        for leaf in _DATABASE_LEAVES[1:]:
            self._validate_optional_leaf(leaf)

    def close(self) -> None:
        with self._lock:
            if self._root_descriptor < 0:
                return
            database = getattr(self, "_database", None)
            if database is not None:
                database.close()
            os.close(self._root_descriptor)
            self._root_descriptor = -1

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _initialize(self) -> None:
        version = self._database.execute("PRAGMA user_version").fetchone()[0]
        if version not in (0, _SCHEMA_VERSION):
            raise AuthoringJournalError(
                "composer_authoring_journal_incompatible",
                "Composer authoring journal schema is incompatible",
            )
        self._database.executescript(
            """
            CREATE TABLE IF NOT EXISTS owner_epochs(
              owner_epoch TEXT PRIMARY KEY,
              started_at_ns INTEGER NOT NULL,
              active INTEGER NOT NULL CHECK(active IN (0,1))
            ) STRICT;
            CREATE UNIQUE INDEX IF NOT EXISTS one_active_owner
              ON owner_epochs(active) WHERE active=1;
            CREATE TABLE IF NOT EXISTS transactions(
              request_owner_epoch TEXT NOT NULL,
              request_id TEXT NOT NULL,
              owner_transaction_id TEXT NOT NULL UNIQUE,
              operation_kind TEXT NOT NULL,
              payload_digest TEXT NOT NULL,
              draft_id TEXT NOT NULL,
              draft_revision INTEGER NOT NULL,
              admission_generation INTEGER NOT NULL,
              owner_epoch TEXT NOT NULL,
              claim_intent_json TEXT NOT NULL,
              acquire_attempted INTEGER NOT NULL DEFAULT 0 CHECK(acquire_attempted IN (0,1)),
              claim_json TEXT,
              effect_intent_json TEXT,
              outcome_json TEXT,
              post_image_json TEXT,
              release_intent_json TEXT,
              release_receipt_json TEXT,
              PRIMARY KEY(request_owner_epoch,request_id)
            ) STRICT;
            CREATE TABLE IF NOT EXISTS retained_workspace(
              singleton INTEGER PRIMARY KEY CHECK(singleton=1),
              post_image_json TEXT NOT NULL,
              owner_transaction_id TEXT NOT NULL
            ) STRICT;
            """
        )
        if version == 0:
            self._database.execute(f"PRAGMA user_version={_SCHEMA_VERSION}")
        check = self._database.execute("PRAGMA integrity_check").fetchone()
        if check != ("ok",):
            raise AuthoringJournalError(
                "composer_authoring_journal_corrupt",
                "Composer authoring journal integrity check failed",
            )

    def _begin_owner_epoch(self) -> str:
        epoch = f"composer-authoring-{uuid.uuid4().hex}"
        with self._database:
            self._database.execute("UPDATE owner_epochs SET active=0 WHERE active=1")
            self._database.execute("DELETE FROM owner_epochs WHERE active=0")
            self._database.execute(
                "INSERT INTO owner_epochs(owner_epoch,started_at_ns,active) VALUES(?,?,1)",
                (epoch, time.time_ns()),
            )
        return epoch

    @staticmethod
    def _json(value: Mapping[str, Any]) -> str:
        return json.dumps(
            dict(value),
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )

    @staticmethod
    def _decode(value: str | None) -> dict[str, Any] | None:
        parsed = None if value is None else json.loads(value)
        if parsed is not None and not isinstance(parsed, dict):
            raise AuthoringJournalError(
                "composer_authoring_journal_corrupt",
                "Composer authoring journal contains a non-object receipt",
            )
        return parsed

    def _row(self, row: tuple[Any, ...]) -> AuthoringTransaction:
        return AuthoringTransaction(
            *row[:9],
            self._decode(row[9]) or {},
            bool(row[10]),
            *(self._decode(item) for item in row[11:17]),
        )

    def lookup(
        self, request_owner_epoch: str, request_id: str
    ) -> AuthoringTransaction | None:
        with self._lock:
            self._validate_storage()
            row = self._database.execute(
                "SELECT request_owner_epoch,request_id,owner_transaction_id,"
                "operation_kind,payload_digest,draft_id,draft_revision,"
                "admission_generation,owner_epoch,claim_intent_json,"
                "acquire_attempted,claim_json,effect_intent_json,outcome_json,"
                "post_image_json,release_intent_json,release_receipt_json "
                "FROM transactions WHERE request_owner_epoch=? AND request_id=?",
                (request_owner_epoch, request_id),
            ).fetchone()
            return None if row is None else self._row(row)

    def unresolved(self) -> list[AuthoringTransaction]:
        with self._lock:
            self._validate_storage()
            rows = self._database.execute(
                "SELECT request_owner_epoch,request_id,owner_transaction_id,"
                "operation_kind,payload_digest,draft_id,draft_revision,"
                "admission_generation,owner_epoch,claim_intent_json,"
                "acquire_attempted,claim_json,effect_intent_json,outcome_json,"
                "post_image_json,release_intent_json,release_receipt_json "
                "FROM transactions WHERE release_receipt_json IS NULL "
                "ORDER BY rowid"
            ).fetchall()
            return [self._row(row) for row in rows]

    def retained_workspace(self) -> dict[str, Any] | None:
        with self._lock:
            self._validate_storage()
            row = self._database.execute(
                "SELECT post_image_json FROM retained_workspace WHERE singleton=1"
            ).fetchone()
            return None if row is None else self._decode(row[0])

    def record_intent(
        self,
        *,
        request_owner_epoch: str,
        request_id: str,
        operation_kind: str,
        payload_digest: str,
        draft_id: str,
        draft_revision: int,
        admission_generation: int,
        claim_intent: Mapping[str, Any],
    ) -> AuthoringTransaction:
        with self._lock:
            self._validate_storage()
            existing = self.lookup(request_owner_epoch, request_id)
            transaction_id = owner_transaction_id(request_owner_epoch, request_id)
            immutable = (
                transaction_id,
                operation_kind,
                payload_digest,
                draft_id,
                draft_revision,
                admission_generation,
            )
            if existing is not None:
                if immutable != (
                    existing.owner_transaction_id,
                    existing.operation_kind,
                    existing.payload_digest,
                    existing.draft_id,
                    existing.draft_revision,
                    existing.admission_generation,
                ):
                    raise AuthoringJournalError(
                        "composer_authoring_request_id_conflict",
                        "authoring request ID is bound to another exact intent",
                    )
                return existing
            count = self._database.execute(
                "SELECT count(*) FROM transactions"
            ).fetchone()[0]
            if count >= self._capacity:
                with self._database:
                    self._database.execute(
                        "DELETE FROM transactions WHERE rowid=("
                        "SELECT rowid FROM transactions "
                        "WHERE release_receipt_json IS NOT NULL ORDER BY rowid LIMIT 1)"
                    )
                count = self._database.execute(
                    "SELECT count(*) FROM transactions"
                ).fetchone()[0]
            if count >= self._capacity:
                raise AuthoringJournalError(
                    "composer_authoring_journal_full",
                    "bounded Composer authoring transaction journal is full",
                )
            with self._database:
                self._database.execute(
                    "INSERT INTO transactions(request_owner_epoch,request_id,"
                    "owner_transaction_id,operation_kind,payload_digest,draft_id,"
                    "draft_revision,admission_generation,owner_epoch,claim_intent_json)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (
                        request_owner_epoch,
                        request_id,
                        transaction_id,
                        operation_kind,
                        payload_digest,
                        draft_id,
                        draft_revision,
                        admission_generation,
                        self.owner_epoch,
                        self._json(claim_intent),
                    ),
                )
            return self.lookup(  # type: ignore[return-value]
                request_owner_epoch, request_id
            )

    def record(self, transaction: AuthoringTransaction, field: str, value: Mapping[str, Any] | bool) -> None:
        columns = {
            "acquire_attempted",
            "claim_json",
            "effect_intent_json",
            "outcome_json",
            "post_image_json",
            "release_intent_json",
            "release_receipt_json",
        }
        if field not in columns:
            raise ValueError("unknown authoring journal receipt")
        encoded: Any = int(value) if field == "acquire_attempted" else self._json(value)  # type: ignore[arg-type]
        predicate = (
            "acquire_attempted IN (0,1)"
            if field == "acquire_attempted"
            else f"({field} IS NULL OR {field}=?)"
        )
        parameters: tuple[Any, ...] = (
            encoded,
            transaction.request_owner_epoch,
            transaction.request_id,
        )
        if field != "acquire_attempted":
            parameters += (encoded,)
        with self._lock:
            self._validate_storage()
            with self._database:
                changed = self._database.execute(
                    f"UPDATE transactions SET {field}=? WHERE request_owner_epoch=? "
                    f"AND request_id=? AND {predicate}",
                    parameters,
                ).rowcount
                if changed != 1:
                    raise AuthoringJournalError(
                        "composer_authoring_journal_conflict",
                        "authoring transaction receipt conflicts with durable state",
                    )
                if field == "post_image_json":
                    self._database.execute(
                        "INSERT INTO retained_workspace(singleton,post_image_json,"
                        "owner_transaction_id) VALUES(1,?,?) "
                        "ON CONFLICT(singleton) DO UPDATE SET "
                        "post_image_json=excluded.post_image_json,"
                        "owner_transaction_id=excluded.owner_transaction_id",
                        (encoded, transaction.owner_transaction_id),
                    )

    def record_effect_outcome(
        self,
        transaction: AuthoringTransaction,
        *,
        post_image: Mapping[str, Any],
        outcome: Mapping[str, Any],
    ) -> None:
        """Atomically retain the local post-image and its replayable outcome."""

        encoded_post_image = self._json(post_image)
        encoded_outcome = self._json(outcome)
        with self._lock:
            self._validate_storage()
            with self._database:
                changed = self._database.execute(
                    "UPDATE transactions SET post_image_json=?,outcome_json=? "
                    "WHERE request_owner_epoch=? AND request_id=? AND "
                    "((post_image_json IS NULL AND outcome_json IS NULL) OR "
                    "(post_image_json=? AND outcome_json=?))",
                    (
                        encoded_post_image,
                        encoded_outcome,
                        transaction.request_owner_epoch,
                        transaction.request_id,
                        encoded_post_image,
                        encoded_outcome,
                    ),
                ).rowcount
                if changed != 1:
                    raise AuthoringJournalError(
                        "composer_authoring_journal_conflict",
                        "authoring effect outcome conflicts with durable state",
                    )
                self._database.execute(
                    "INSERT INTO retained_workspace(singleton,post_image_json,"
                    "owner_transaction_id) VALUES(1,?,?) "
                    "ON CONFLICT(singleton) DO UPDATE SET "
                    "post_image_json=excluded.post_image_json,"
                    "owner_transaction_id=excluded.owner_transaction_id",
                    (encoded_post_image, transaction.owner_transaction_id),
                )


__all__ = [
    "AuthoringJournalError",
    "AuthoringTransaction",
    "ComposerAuthoringTransactionJournal",
    "canonical_digest",
    "owner_transaction_id",
]
