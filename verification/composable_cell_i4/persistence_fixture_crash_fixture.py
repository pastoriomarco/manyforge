"""Selected independent process-crash fixtures for the I4 SQLite substrate.

The child commands intentionally terminate through ``os._exit`` so Python
destructors and adapter ``close`` cannot turn the test into a graceful-shutdown
case.  ``inspect_store_raw`` reads SQLite directly and does not use the
reference adapter's projections. These fixtures cover only the four named
windows; they do not claim every persistence boundary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
from typing import Any, Final

from manyforge_supervision.persistence.reference_adapter import (
    BootstrapIdentity,
    ComponentManagerPersistenceFixture,
    FixtureCasIntent,
    JournalEvent,
    OperationKind,
    OperationRef,
)


BOOTSTRAP_ID: Final = "fixture-bootstrap"
BOOTSTRAP_GENERATION: Final = 1
BOOTSTRAP_BUNDLE: Final = "fixture-canonical-empty-bundle"
BOOTSTRAP_GRAPH: Final = "fixture-canonical-empty-graph"
BOOTSTRAP_FIXTURE_CAS_ID: Final = "fixture-bootstrap-cas"

EXIT_AFTER_BOOTSTRAP_COMMIT: Final = 71
EXIT_DURING_UNCOMMITTED_APPEND: Final = 72
EXIT_AFTER_FIXTURE_CAS_COMMIT: Final = 73
EXIT_DURING_SCHEMA_INITIALIZATION: Final = 74


def fixture_digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode("ascii")).hexdigest()


def bootstrap_identity() -> BootstrapIdentity:
    return BootstrapIdentity(
        bootstrap_id=BOOTSTRAP_ID,
        generation=BOOTSTRAP_GENERATION,
        intent_digest=fixture_digest("fixture-bootstrap-intent"),
        target_bundle_identity=BOOTSTRAP_BUNDLE,
        target_graph_revision=BOOTSTRAP_GRAPH,
    )


def bootstrap_ref() -> OperationRef:
    return OperationRef(
        OperationKind.BOOTSTRAP,
        BOOTSTRAP_ID,
        BOOTSTRAP_GENERATION,
    )


def bootstrap_fixture_cas_intent(
    operation_identity_digest: str,
) -> FixtureCasIntent:
    return FixtureCasIntent(
        fixture_cas_id=BOOTSTRAP_FIXTURE_CAS_ID,
        operation_ref=bootstrap_ref(),
        operation_identity_digest=operation_identity_digest,
    )


def inspect_store_raw(database_path: Path) -> dict[str, Any]:
    """Inspect committed state without importing adapter projections."""

    connection = sqlite3.connect(
        str(database_path),
        isolation_level=None,
        timeout=10.0,
    )
    connection.row_factory = sqlite3.Row
    try:
        metadata = connection.execute(
            """
            SELECT schema_id, schema_version, cell_id
            FROM persistence_metadata
            WHERE singleton = 1
            """
        ).fetchone()
        head = connection.execute(
            """
            SELECT generation, bundle_identity, graph_revision,
                   fixture_cas_id, receipt_digest
            FROM fixture_cas_head
            WHERE singleton = 1
            """
        ).fetchone()
        entries = connection.execute(
            """
            SELECT entry_id, event_kind, sequence
            FROM manager_journal_entries
            ORDER BY operation_kind, operation_id,
                     operation_generation, sequence
            """
        ).fetchall()
        return {
            "metadata": None
            if metadata is None
            else {
                "schema_id": metadata["schema_id"],
                "schema_version": int(metadata["schema_version"]),
                "cell_id": metadata["cell_id"],
            },
            "operation_count": int(
                connection.execute(
                    "SELECT COUNT(*) FROM manager_operations"
                ).fetchone()[0]
            ),
            "journal_entries": [
                {
                    "entry_id": row["entry_id"],
                    "event_kind": row["event_kind"],
                    "sequence": int(row["sequence"]),
                }
                for row in entries
            ],
            "fixture_cas_receipt_count": int(
                connection.execute(
                    "SELECT COUNT(*) FROM fixture_cas_receipts"
                ).fetchone()[0]
            ),
            "fixture_cas_head": None
            if head is None
            else {
                "generation": int(head["generation"]),
                "bundle_identity": head["bundle_identity"],
                "graph_revision": head["graph_revision"],
                "fixture_cas_id": head["fixture_cas_id"],
                "receipt_digest": head["receipt_digest"],
            },
        }
    finally:
        connection.close()


def _bootstrap_commit_then_crash(database_path: Path, cell_id: str) -> None:
    store = ComponentManagerPersistenceFixture(database_path, cell_id=cell_id)
    operation = store.ensure_bootstrap(bootstrap_identity()).record
    store.append_journal_event(
        operation.ref,
        JournalEvent(
            entry_id="bootstrap-target-journaled",
            event_kind="bootstrap_target_journaled",
            payload=b'{"fixture":"canonical-empty"}',
        ),
    )
    os._exit(EXIT_AFTER_BOOTSTRAP_COMMIT)


def _uncommitted_append_then_crash(
    database_path: Path,
    marker_path: Path,
) -> None:
    """Insert inside an open writer transaction and die before commit."""

    connection = sqlite3.connect(
        str(database_path),
        isolation_level=None,
        timeout=10.0,
    )
    connection.execute("PRAGMA foreign_keys = ON")
    connection.execute("BEGIN IMMEDIATE")
    digest = fixture_digest("uncommitted-placeholder")
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
        ) VALUES (
            'bootstrap',
            ?,
            ?,
            2,
            'never-committed',
            'fixture_uncommitted',
            X'7B7D',
            ?,
            ?,
            ?
        )
        """,
        (
            BOOTSTRAP_ID,
            BOOTSTRAP_GENERATION,
            digest,
            digest,
            digest,
        ),
    )
    marker_path.write_text("inserted-before-crash", encoding="utf-8")
    os._exit(EXIT_DURING_UNCOMMITTED_APPEND)


def _fixture_cas_commit_then_crash(
    database_path: Path,
    cell_id: str,
) -> None:
    store = ComponentManagerPersistenceFixture(database_path, cell_id=cell_id)
    operation = store.ensure_bootstrap(bootstrap_identity()).record
    store.advance_fixture_head(
        bootstrap_fixture_cas_intent(operation.identity_digest)
    )
    os._exit(EXIT_AFTER_FIXTURE_CAS_COMMIT)


def _schema_initialization_then_crash(
    database_path: Path,
    marker_path: Path,
) -> None:
    """Create one schema object in a transaction and die before commit."""

    connection = sqlite3.connect(
        str(database_path),
        isolation_level=None,
        timeout=10.0,
    )
    connection.execute("PRAGMA journal_mode = WAL")
    connection.execute("PRAGMA synchronous = FULL")
    connection.execute("BEGIN EXCLUSIVE")
    connection.execute(
        """
        CREATE TABLE persistence_metadata (
            singleton INTEGER PRIMARY KEY,
            schema_id TEXT NOT NULL
        )
        """
    )
    marker_path.write_text("schema-object-created", encoding="utf-8")
    os._exit(EXIT_DURING_SCHEMA_INITIALIZATION)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode",
        choices=(
            "bootstrap-commit-then-crash",
            "uncommitted-append-then-crash",
            "fixture-cas-commit-then-crash",
            "schema-initialization-then-crash",
            "inspect",
        ),
    )
    parser.add_argument("database_path", type=Path)
    parser.add_argument("--cell-id", default="fixture-cell")
    parser.add_argument("--marker-path", type=Path)
    args = parser.parse_args()

    if args.mode == "bootstrap-commit-then-crash":
        _bootstrap_commit_then_crash(args.database_path, args.cell_id)
    if args.mode == "uncommitted-append-then-crash":
        if args.marker_path is None:
            parser.error("--marker-path is required for uncommitted append")
        _uncommitted_append_then_crash(
            args.database_path,
            args.marker_path,
        )
    if args.mode == "fixture-cas-commit-then-crash":
        _fixture_cas_commit_then_crash(args.database_path, args.cell_id)
    if args.mode == "schema-initialization-then-crash":
        if args.marker_path is None:
            parser.error("--marker-path is required for schema initialization")
        _schema_initialization_then_crash(
            args.database_path,
            args.marker_path,
        )
    if args.mode == "inspect":
        print(
            json.dumps(
                inspect_store_raw(args.database_path),
                sort_keys=True,
                separators=(",", ":"),
            )
        )


if __name__ == "__main__":
    main()
