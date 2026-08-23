"""Bounded durable transaction journal for Composer-owned authoring effects."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sqlite3
import threading
import time
import uuid
from typing import Any, Mapping


_SCHEMA_VERSION = 1
_MAXIMUM_TRANSACTIONS = 1024
_DOMAIN = b"manyforge.composer.authoring-transaction.v1\0"


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


def canonical_digest(value: Mapping[str, Any]) -> str:
    encoded = json.dumps(
        dict(value),
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    return f"sha256:{hashlib.sha256(encoded).hexdigest()}"


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
        state_root = state_root.resolve()
        if not state_root.is_dir():
            raise AuthoringJournalError(
                "composer_authoring_state_root_invalid",
                "Composer authoring state root must be a pre-existing directory",
            )
        self._capacity = maximum_transactions
        self._lock = threading.RLock()
        self._database = sqlite3.connect(
            state_root / "composer-authoring-transactions.sqlite3",
            isolation_level="IMMEDIATE",
            check_same_thread=False,
        )
        try:
            self._database.execute("PRAGMA journal_mode=WAL")
            self._database.execute("PRAGMA synchronous=FULL")
            self._database.execute("PRAGMA foreign_keys=ON")
            self._initialize()
            self.owner_epoch = self._begin_owner_epoch()
        except Exception:
            self._database.close()
            raise

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
