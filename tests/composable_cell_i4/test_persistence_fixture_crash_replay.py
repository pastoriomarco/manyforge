from __future__ import annotations

from pathlib import Path
import sqlite3
import subprocess
import sys

from manyforge_supervision.persistence.reference_adapter import (
    ComponentManagerPersistenceFixture,
    JournalEvent,
)
from verification.composable_cell_i4.persistence_fixture_crash_fixture import (
    BOOTSTRAP_BUNDLE,
    BOOTSTRAP_FIXTURE_CAS_ID,
    EXIT_AFTER_BOOTSTRAP_COMMIT,
    EXIT_AFTER_FIXTURE_CAS_COMMIT,
    EXIT_DURING_UNCOMMITTED_APPEND,
    EXIT_DURING_SCHEMA_INITIALIZATION,
    bootstrap_identity,
    bootstrap_fixture_cas_intent,
    inspect_store_raw,
)


def run_fixture(
    mode: str,
    database: Path,
    *,
    marker: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        "-m",
        "verification.composable_cell_i4.persistence_fixture_crash_fixture",
        mode,
        str(database),
        "--cell-id",
        "fixture-cell",
    ]
    if marker is not None:
        command.extend(("--marker-path", str(marker)))
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
    )


def test_committed_bootstrap_fixture_survives_exit_and_replays_exactly(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"

    child = run_fixture("bootstrap-commit-then-crash", database)

    assert child.returncode == EXIT_AFTER_BOOTSTRAP_COMMIT
    raw = inspect_store_raw(database)
    assert raw["operation_count"] == 1
    assert raw["fixture_cas_receipt_count"] == 0
    assert raw["fixture_cas_head"]["generation"] == 0
    assert raw["journal_entries"] == [
        {
            "entry_id": "bootstrap-target-journaled",
            "event_kind": "bootstrap_target_journaled",
            "sequence": 1,
        }
    ]

    reopened = ComponentManagerPersistenceFixture(
        database,
        cell_id="fixture-cell",
    )
    operation_replay = reopened.ensure_bootstrap(bootstrap_identity())
    event_replay = reopened.append_journal_event(
        operation_replay.record.ref,
        JournalEvent(
            entry_id="bootstrap-target-journaled",
            event_kind="bootstrap_target_journaled",
            payload=b'{"fixture":"canonical-empty"}',
        ),
    )

    assert not operation_replay.inserted
    assert not event_replay.inserted
    assert reopened.verify_integrity().journal_entry_count == 1


def test_schema_initialization_is_atomic_across_process_death(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    marker = tmp_path / "schema.marker"

    child = run_fixture(
        "schema-initialization-then-crash",
        database,
        marker=marker,
    )

    assert child.returncode == EXIT_DURING_SCHEMA_INITIALIZATION
    assert marker.read_text(encoding="utf-8") == "schema-object-created"
    connection = sqlite3.connect(database)
    user_tables = connection.execute(
        """
        SELECT name FROM sqlite_master
        WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
        """
    ).fetchall()
    connection.close()
    assert user_tables == []

    initialized = ComponentManagerPersistenceFixture(
        database,
        cell_id="fixture-cell",
    )
    assert initialized.verify_integrity().operation_count == 0


def test_uncommitted_writer_is_rolled_back_after_process_death(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    marker = tmp_path / "inserted.marker"
    committed = run_fixture("bootstrap-commit-then-crash", database)
    assert committed.returncode == EXIT_AFTER_BOOTSTRAP_COMMIT

    child = run_fixture(
        "uncommitted-append-then-crash",
        database,
        marker=marker,
    )

    assert child.returncode == EXIT_DURING_UNCOMMITTED_APPEND
    assert marker.read_text(encoding="utf-8") == "inserted-before-crash"
    raw = inspect_store_raw(database)
    assert [
        row["entry_id"] for row in raw["journal_entries"]
    ] == ["bootstrap-target-journaled"]

    reopened = ComponentManagerPersistenceFixture(
        database,
        cell_id="fixture-cell",
    )
    assert reopened.verify_integrity().journal_entry_count == 1


def test_fixture_cas_commit_before_journal_receipt_is_detectable_and_replayable(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    committed = run_fixture("bootstrap-commit-then-crash", database)
    assert committed.returncode == EXIT_AFTER_BOOTSTRAP_COMMIT

    child = run_fixture("fixture-cas-commit-then-crash", database)

    assert child.returncode == EXIT_AFTER_FIXTURE_CAS_COMMIT
    raw = inspect_store_raw(database)
    assert raw["fixture_cas_receipt_count"] == 1
    assert raw["fixture_cas_head"]["generation"] == 1
    assert raw["fixture_cas_head"]["bundle_identity"] == BOOTSTRAP_BUNDLE
    assert (
        raw["fixture_cas_head"]["fixture_cas_id"]
        == BOOTSTRAP_FIXTURE_CAS_ID
    )
    assert [
        row["entry_id"] for row in raw["journal_entries"]
    ] == ["bootstrap-target-journaled"]

    reopened = ComponentManagerPersistenceFixture(
        database,
        cell_id="fixture-cell",
    )
    operation = reopened.ensure_bootstrap(bootstrap_identity()).record
    fixture_cas_replay = reopened.advance_fixture_head(
        bootstrap_fixture_cas_intent(operation.identity_digest)
    )
    receipt_record = reopened.append_journal_event(
        operation.ref,
        JournalEvent(
            entry_id="bootstrap-fixture-cas-receipt-recorded",
            event_kind="bootstrap_fixture_cas_receipt_recorded",
            payload=fixture_cas_replay.receipt.receipt_digest.encode("ascii"),
        ),
    )

    assert not fixture_cas_replay.inserted
    assert receipt_record.inserted
    report = reopened.verify_integrity()
    assert report.fixture_cas_receipt_count == 1
    assert report.fixture_head_generation == 1
    assert report.journal_entry_count == 2


def test_raw_inspector_is_independent_of_adapter_projection(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    child = run_fixture("bootstrap-commit-then-crash", database)
    assert child.returncode == EXIT_AFTER_BOOTSTRAP_COMMIT

    inspected = run_fixture("inspect", database)

    assert inspected.returncode == 0
    assert '"operation_count":1' in inspected.stdout
    assert '"schema_version":1' in inspected.stdout
