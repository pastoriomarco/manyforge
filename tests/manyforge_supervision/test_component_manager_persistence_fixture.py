from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import fields, replace
import hashlib
from pathlib import Path
import sqlite3

import pytest

from manyforge_supervision.persistence import (
    reference_adapter as persistence_fixture,
)
from manyforge_supervision.persistence.reference_adapter import (
    SQLITE_SIGNED_INT64_MAX,
    SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES,
    MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION,
    MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES,
    MAXIMUM_PAYLOAD_BYTES,
    ApplyIdentity,
    BootstrapIdentity,
    ComponentManagerPersistenceFixture,
    FixtureCasHead,
    FixtureCasIntent,
    FixtureCasReceipt,
    JournalEvent,
    OperationKind,
    OperationRecord,
    OperationRef,
    PERSISTENCE_SCHEMA_ID,
    PERSISTENCE_SCHEMA_VERSION,
    PersistenceClosedError,
    PersistenceConflictError,
    PersistenceIntegrityError,
    PersistenceLimitError,
)


def digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode("ascii")).hexdigest()


def bootstrap(
    *,
    bootstrap_id: str = "bootstrap-1",
    generation: int = 1,
    target_bundle: str = "canonical-empty-bundle",
    target_graph: str = "canonical-empty-graph",
) -> BootstrapIdentity:
    return BootstrapIdentity(
        bootstrap_id=bootstrap_id,
        generation=generation,
        intent_digest=digest(f"intent:{bootstrap_id}:{generation}"),
        target_bundle_identity=target_bundle,
        target_graph_revision=target_graph,
    )


def apply_identity(
    *,
    apply_id: str = "apply-1",
    generation: int = 1,
    expected_fixture_generation: int = 1,
    predecessor_bundle: str = "canonical-empty-bundle",
    predecessor_graph: str = "canonical-empty-graph",
    target_bundle: str = "cell-bundle-1",
    target_graph: str = "cell-graph-1",
) -> ApplyIdentity:
    return ApplyIdentity(
        apply_id=apply_id,
        generation=generation,
        request_id=f"request-{apply_id}",
        manager_owner_epoch="manager-epoch-1",
        intent_digest=digest(f"intent:{apply_id}:{generation}"),
        expected_fixture_generation=expected_fixture_generation,
        predecessor_bundle_identity=predecessor_bundle,
        predecessor_graph_revision=predecessor_graph,
        target_bundle_identity=target_bundle,
        target_graph_revision=target_graph,
    )


def store(tmp_path: Path) -> ComponentManagerPersistenceFixture:
    return ComponentManagerPersistenceFixture(
        tmp_path / "manager.sqlite3",
        cell_id="cell-a",
    )


def fixture_cas(
    fixture_cas_id: str,
    operation_record: OperationRecord,
) -> FixtureCasIntent:
    return FixtureCasIntent(
        fixture_cas_id=fixture_cas_id,
        operation_ref=operation_record.ref,
        operation_identity_digest=operation_record.identity_digest,
    )


def advance_bootstrap_fixture_head(
    subject: ComponentManagerPersistenceFixture,
) -> tuple[OperationRecord, FixtureCasReceipt]:
    operation = subject.ensure_bootstrap(bootstrap()).record
    receipt = subject.advance_fixture_head(
        fixture_cas("bootstrap-fixture_cas", operation)
    ).receipt
    return operation, receipt


def test_store_initialization_pins_schema_cell_and_durability_pragmas(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)

    report = subject.verify_integrity()
    connection = subject._connection

    assert report.schema_id == PERSISTENCE_SCHEMA_ID
    assert report.schema_version == PERSISTENCE_SCHEMA_VERSION
    assert report.cell_id == "cell-a"
    assert report.operation_count == 0
    assert report.journal_entry_count == 0
    assert report.fixture_cas_receipt_count == 0
    assert report.fixture_head_generation == 0
    assert subject.fixture_cas_head() is None
    assert connection.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
    assert connection.execute("PRAGMA synchronous").fetchone()[0] == 2
    assert connection.execute("PRAGMA foreign_keys").fetchone()[0] == 1
    assert connection.execute("PRAGMA busy_timeout").fetchone()[0] == 10_000
    assert (
        connection.execute("PRAGMA journal_size_limit").fetchone()[0]
        == SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES
    )
    page_size = connection.execute("PRAGMA page_size").fetchone()[0]
    max_page_count = connection.execute(
        "PRAGMA max_page_count"
    ).fetchone()[0]
    assert page_size * max_page_count <= MAXIMUM_MAIN_DATABASE_PAGE_IMAGE_BYTES


def test_reopen_requires_the_exact_cell_and_schema(tmp_path: Path) -> None:
    database = tmp_path / "manager.sqlite3"
    ComponentManagerPersistenceFixture(database, cell_id="cell-a").close()

    reopened = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    assert reopened.verify_integrity().cell_id == "cell-a"
    reopened.close()

    with pytest.raises(
        PersistenceConflictError,
        match="different cell",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-b")


def test_wal_target_is_not_a_hard_limit_while_reader_pins_snapshot(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    wal_path = Path(f"{database}-wal")
    subject = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    operation = subject.ensure_bootstrap(bootstrap()).record
    reader = sqlite3.connect(database, isolation_level=None)
    small_fixture_target = 4096

    try:
        reader.execute("BEGIN")
        reader.execute(
            "SELECT COUNT(*) FROM manager_journal_entries"
        ).fetchone()
        subject._connection.execute(
            f"PRAGMA journal_size_limit = {small_fixture_target}"
        )
        for sequence in range(24):
            subject.append_journal_event(
                operation.ref,
                JournalEvent(
                    f"wal-entry-{sequence}",
                    "wal_fixture_event",
                    b"x" * 4096,
                ),
            )

        assert wal_path.stat().st_size > small_fixture_target
        subject._connection.execute("PRAGMA busy_timeout = 50")
        blocked_checkpoint = subject._connection.execute(
            "PRAGMA wal_checkpoint(TRUNCATE)"
        ).fetchone()
        assert blocked_checkpoint[0] == 1
        assert wal_path.stat().st_size > small_fixture_target
    finally:
        reader.rollback()
        reader.close()

    completed_checkpoint = subject._connection.execute(
        "PRAGMA wal_checkpoint(TRUNCATE)"
    ).fetchone()
    assert completed_checkpoint[0] == 0
    assert not wal_path.exists() or wal_path.stat().st_size == 0
    subject._connection.execute(
        "PRAGMA journal_size_limit = "
        f"{SQLITE_WAL_CHECKPOINT_TRUNCATION_TARGET_BYTES}"
    )
    subject._connection.execute("PRAGMA busy_timeout = 10000")
    assert subject.verify_integrity().journal_entry_count == 24


@pytest.mark.parametrize("invalid_cell_id", ["cell-é", "cell-\ud800"])
def test_identity_validation_rejects_non_ascii_and_surrogates_cleanly(
    tmp_path: Path,
    invalid_cell_id: str,
) -> None:
    with pytest.raises(
        ValueError,
        match="bounded printable ASCII identity",
    ):
        ComponentManagerPersistenceFixture(
            tmp_path / "manager.sqlite3",
            cell_id=invalid_cell_id,
        )


def test_persisted_integers_are_bounded_to_signed_sqlite_int64() -> None:
    too_large = SQLITE_SIGNED_INT64_MAX + 1

    with pytest.raises(ValueError, match="signed SQLite int64"):
        bootstrap(generation=too_large)
    with pytest.raises(ValueError, match="signed SQLite int64"):
        apply_identity(expected_fixture_generation=too_large)
    with pytest.raises(ValueError, match="signed SQLite int64"):
        OperationRef(OperationKind.APPLY, "apply", too_large)


def test_nonempty_unknown_or_partial_schema_is_never_adopted(
    tmp_path: Path,
) -> None:
    database = tmp_path / "partial.sqlite3"
    connection = sqlite3.connect(database)
    connection.execute("CREATE TABLE unrelated(value TEXT)")
    connection.commit()
    connection.close()

    with pytest.raises(
        PersistenceIntegrityError,
        match="lacks persistence metadata",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-a")

    connection = sqlite3.connect(database)
    names = {
        row[0]
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'"
        )
    }
    connection.close()
    assert names == {"unrelated"}


def test_schema_drift_is_rejected_even_with_if_not_exists_ddl(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    ComponentManagerPersistenceFixture(database, cell_id="cell-a").close()

    connection = sqlite3.connect(database)
    connection.execute("CREATE TABLE injected_drift(value TEXT)")
    connection.commit()
    connection.close()

    with pytest.raises(
        PersistenceIntegrityError,
        match="schema objects differ",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-a")


def test_bootstrap_identity_is_immutable_idempotent_and_singleton(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    first = subject.ensure_bootstrap(bootstrap())
    replay = subject.ensure_bootstrap(bootstrap())

    assert first.inserted
    assert not replay.inserted
    assert replay.record == first.record

    with pytest.raises(
        PersistenceConflictError,
        match="different immutable content",
    ):
        subject.ensure_bootstrap(
            replace(
                bootstrap(),
                target_graph_revision="other-graph",
            )
        )
    with pytest.raises(
        PersistenceConflictError,
        match="different bootstrap identity",
    ):
        subject.ensure_bootstrap(bootstrap(bootstrap_id="bootstrap-2"))


def test_apply_identity_is_immutable_and_retains_request_boundary(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    first = subject.ensure_apply(apply_identity())
    replay = subject.ensure_apply(apply_identity())

    assert first.inserted
    assert not replay.inserted
    assert replay.record == first.record
    assert first.record.request_id == "request-apply-1"
    assert first.record.manager_owner_epoch == "manager-epoch-1"

    with pytest.raises(
        PersistenceConflictError,
        match="different immutable content",
    ):
        subject.ensure_apply(
            replace(
                apply_identity(),
                target_bundle_identity="other-bundle",
            )
        )


def test_apply_request_identity_has_exact_replay_and_unique_database_key(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    identity = apply_identity()
    first = subject.ensure_apply(identity)
    replay = subject.ensure_apply(identity)

    assert first.inserted
    assert not replay.inserted
    assert replay.record == first.record

    conflicting = replace(
        identity,
        apply_id="different-apply",
        generation=2,
        intent_digest=digest("different-intent"),
    )
    with pytest.raises(
        PersistenceConflictError,
        match="Apply request identity",
    ):
        subject.ensure_apply(conflicting)

    with pytest.raises(sqlite3.IntegrityError, match="UNIQUE"):
        subject._connection.execute(
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
            )
            SELECT operation_kind,
                   'raw-duplicate-request',
                   operation_generation + 1,
                   request_id,
                   manager_owner_epoch,
                   intent_digest,
                   expected_fixture_generation,
                   predecessor_bundle_identity,
                   predecessor_graph_revision,
                   target_bundle_identity,
                   target_graph_revision,
                   identity_digest,
                   created_sequence + 1
            FROM manager_operations
            WHERE operation_kind = 'apply'
            """
        )


def test_journal_is_append_only_hash_chained_and_exactly_replayable(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    operation = subject.ensure_bootstrap(bootstrap()).record
    first_event = JournalEvent(
        "target-recorded",
        "bootstrap_target_recorded",
        b'{"target":"empty"}',
    )
    second_event = JournalEvent(
        "store-verified",
        "bootstrap_store_verified",
        b'{"verified":true}',
    )

    first = subject.append_journal_event(operation.ref, first_event)
    replay = subject.append_journal_event(operation.ref, first_event)
    second = subject.append_journal_event(operation.ref, second_event)

    assert first.inserted
    assert not replay.inserted
    assert replay.entry == first.entry
    assert second.entry.sequence == 2
    assert (
        second.entry.previous_entry_digest
        == first.entry.entry_digest
    )
    assert [
        entry.entry_id for entry in subject.journal(operation.ref)
    ] == ["target-recorded", "store-verified"]
    assert subject.verify_integrity().journal_entry_count == 2


def test_journal_rejects_conflict_unknown_operation_and_oversized_payload(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    operation = subject.ensure_bootstrap(bootstrap()).record
    event = JournalEvent("entry", "event", b"one")
    subject.append_journal_event(operation.ref, event)

    with pytest.raises(PersistenceConflictError, match="different immutable"):
        subject.append_journal_event(
            operation.ref,
            replace(event, payload=b"two"),
        )
    with pytest.raises(PersistenceConflictError, match="unknown operation"):
        subject.append_journal_event(
            OperationRef(OperationKind.APPLY, "unknown", 1),
            JournalEvent("entry", "event", b"payload"),
        )
    assert len(
        JournalEvent(
            "maximum",
            "event",
            b"x" * MAXIMUM_PAYLOAD_BYTES,
        ).payload
    ) == MAXIMUM_PAYLOAD_BYTES
    with pytest.raises(PersistenceLimitError, match="payload exceeds"):
        JournalEvent(
            "too-large",
            "event",
            b"x" * (MAXIMUM_PAYLOAD_BYTES + 1),
        )


def test_journal_enforces_the_exact_per_operation_ceiling(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    operation = subject.ensure_bootstrap(bootstrap()).record

    for sequence in range(MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION):
        subject.append_journal_event(
            operation.ref,
            JournalEvent(
                f"entry-{sequence}",
                "bounded_event",
                b"",
            ),
        )

    with pytest.raises(PersistenceLimitError, match="limit is exhausted"):
        subject.append_journal_event(
            operation.ref,
            JournalEvent("entry-over-limit", "bounded_event", b""),
        )
    assert len(subject.journal(operation.ref)) == (
        MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION
    )
    assert subject.verify_integrity().journal_entry_count == (
        MAXIMUM_JOURNAL_ENTRIES_PER_OPERATION
    )


def test_bootstrap_and_apply_fixture_cas_form_one_cas_chain(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    bootstrap_operation, bootstrap_receipt = advance_bootstrap_fixture_head(subject)

    assert bootstrap_receipt.committed_fixture_generation == 1
    assert bootstrap_receipt.operation_ref == bootstrap_operation.ref
    assert subject.fixture_cas_head() == FixtureCasHead(
        generation=1,
        bundle_identity="canonical-empty-bundle",
        graph_revision="canonical-empty-graph",
        fixture_cas_id="bootstrap-fixture_cas",
        receipt_digest=bootstrap_receipt.receipt_digest,
        operation_ref=bootstrap_operation.ref,
    )

    apply_operation = subject.ensure_apply(apply_identity()).record
    apply_receipt = subject.advance_fixture_head(
        fixture_cas("apply-fixture_cas", apply_operation)
    ).receipt

    assert apply_receipt.expected_fixture_generation == 1
    assert apply_receipt.committed_fixture_generation == 2
    assert apply_receipt.predecessor_bundle_identity == (
        "canonical-empty-bundle"
    )
    assert subject.fixture_cas_head() == FixtureCasHead(
        generation=2,
        bundle_identity="cell-bundle-1",
        graph_revision="cell-graph-1",
        fixture_cas_id="apply-fixture_cas",
        receipt_digest=apply_receipt.receipt_digest,
        operation_ref=apply_operation.ref,
    )
    assert subject.verify_integrity().fixture_cas_receipt_count == 2


def test_fixture_cas_is_idempotent_and_conflicting_id_is_rejected(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    operation = subject.ensure_bootstrap(bootstrap()).record
    intent = fixture_cas("fixture_cas-1", operation)

    first = subject.advance_fixture_head(intent)
    replay = subject.advance_fixture_head(intent)

    assert first.inserted
    assert not replay.inserted
    assert replay.receipt == first.receipt

    with pytest.raises(PersistenceConflictError, match="different immutable"):
        subject.advance_fixture_head(
            replace(intent, operation_identity_digest=digest("wrong"))
        )


def test_fixture_cas_requires_exact_operation_and_cas_predecessor(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    unknown = FixtureCasIntent(
        fixture_cas_id="unknown-fixture_cas",
        operation_ref=OperationRef(OperationKind.BOOTSTRAP, "missing", 1),
        operation_identity_digest=digest("missing"),
    )
    with pytest.raises(PersistenceConflictError, match="unknown operation"):
        subject.advance_fixture_head(unknown)

    operation, _ = advance_bootstrap_fixture_head(subject)
    stale_apply = subject.ensure_apply(
        apply_identity(
            apply_id="stale",
            expected_fixture_generation=2,
            predecessor_bundle="future-bundle",
            predecessor_graph="future-graph",
        )
    ).record
    with pytest.raises(PersistenceConflictError, match="precondition failed"):
        subject.advance_fixture_head(fixture_cas("stale-cas", stale_apply))

    wrong_digest = FixtureCasIntent(
        fixture_cas_id="wrong-digest",
        operation_ref=operation.ref,
        operation_identity_digest=digest("wrong"),
    )
    with pytest.raises(PersistenceConflictError, match="digest mismatch"):
        subject.advance_fixture_head(wrong_digest)


def test_first_fixture_cas_cannot_skip_bootstrap(tmp_path: Path) -> None:
    subject = store(tmp_path)
    apply_operation = subject.ensure_apply(
        apply_identity(
            expected_fixture_generation=1,
            predecessor_bundle="not-active",
            predecessor_graph="not-active",
        )
    ).record

    with pytest.raises(PersistenceConflictError):
        subject.advance_fixture_head(fixture_cas("apply-first", apply_operation))


def test_fixture_cas_rejects_generation_without_int64_successor(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    operation = subject.ensure_apply(
        apply_identity(
            expected_fixture_generation=SQLITE_SIGNED_INT64_MAX,
        )
    ).record

    with pytest.raises(PersistenceLimitError, match="generation exhausted"):
        subject.advance_fixture_head(
            fixture_cas("exhausted-generation", operation)
        )


def test_single_adapter_serializes_threaded_exact_replay(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)

    with ThreadPoolExecutor(max_workers=16) as executor:
        results = tuple(
            executor.map(
                lambda _: subject.ensure_bootstrap(bootstrap()),
                range(64),
            )
        )

    assert sum(result.inserted for result in results) == 1
    assert len({result.record.identity_digest for result in results}) == 1
    assert subject.verify_integrity().operation_count == 1


def test_two_adapters_share_sqlite_single_writer_and_cas(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    first = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    second = ComponentManagerPersistenceFixture(database, cell_id="cell-a")

    with ThreadPoolExecutor(max_workers=2) as executor:
        bootstrap_results = tuple(
            executor.map(
                lambda subject: subject.ensure_bootstrap(bootstrap()),
                (first, second),
            )
        )
    assert sum(result.inserted for result in bootstrap_results) == 1
    bootstrap_operation = bootstrap_results[0].record
    assert bootstrap_results[1].record == bootstrap_operation

    event = JournalEvent("shared-entry", "shared_event", b"same")
    with ThreadPoolExecutor(max_workers=2) as executor:
        journal_results = tuple(
            executor.map(
                lambda subject: subject.append_journal_event(
                    bootstrap_operation.ref,
                    event,
                ),
                (first, second),
            )
        )
    assert sum(result.inserted for result in journal_results) == 1
    assert journal_results[0].entry == journal_results[1].entry

    first.advance_fixture_head(
        fixture_cas("bootstrap-fixture_cas", bootstrap_operation)
    )
    apply_a = first.ensure_apply(
        apply_identity(
            apply_id="apply-a",
            target_bundle="bundle-a",
            target_graph="graph-a",
        )
    ).record
    apply_b = second.ensure_apply(
        apply_identity(
            apply_id="apply-b",
            target_bundle="bundle-b",
            target_graph="graph-b",
        )
    ).record

    def attempt(
        subject: ComponentManagerPersistenceFixture,
        fixture_cas_id: str,
        operation: OperationRecord,
    ) -> str:
        try:
            subject.advance_fixture_head(fixture_cas(fixture_cas_id, operation))
            return "advanced"
        except PersistenceConflictError:
            return "conflict"

    with ThreadPoolExecutor(max_workers=2) as executor:
        results = (
            executor.submit(attempt, first, "cas-a", apply_a),
            executor.submit(attempt, second, "cas-b", apply_b),
        )
        outcomes = tuple(result.result() for result in results)

    assert sorted(outcomes) == ["advanced", "conflict"]
    assert first.verify_integrity().fixture_head_generation == 2
    assert second.verify_integrity().fixture_head_generation == 2


def test_two_adapters_cannot_split_one_apply_request_identity(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    first = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    second = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    identity = apply_identity()
    conflicting = replace(
        identity,
        apply_id="competing-apply",
        generation=2,
        intent_digest=digest("competing-intent"),
    )

    def attempt(
        subject: ComponentManagerPersistenceFixture,
        candidate: ApplyIdentity,
    ) -> str:
        try:
            subject.ensure_apply(candidate)
            return candidate.apply_id
        except PersistenceConflictError:
            return "conflict"

    with ThreadPoolExecutor(max_workers=2) as executor:
        outcomes = tuple(
            future.result()
            for future in (
                executor.submit(attempt, first, identity),
                executor.submit(attempt, second, conflicting),
            )
        )

    assert outcomes.count("conflict") == 1
    assert len(first.operations()) == 1
    assert first.operations() == second.operations()


def test_concurrent_first_open_converges_one_exact_schema(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"

    def open_and_verify() -> tuple[str, int]:
        subject = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
        try:
            report = subject.verify_integrity()
            return report.schema_artifact_digest, report.operation_count
        finally:
            subject.close()

    with ThreadPoolExecutor(max_workers=4) as executor:
        results = tuple(executor.map(lambda _: open_and_verify(), range(4)))

    assert len({result[0] for result in results}) == 1
    assert {result[1] for result in results} == {0}
    reopened = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    assert reopened.verify_integrity().operation_count == 0


def test_reopen_verifies_consistency_head_and_immutable_schema(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    subject = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    operation, _ = advance_bootstrap_fixture_head(subject)
    subject.append_journal_event(
        operation.ref,
        JournalEvent("entry", "event", b"payload"),
    )
    expected = subject.verify_integrity()
    subject.close()

    reopened = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    assert reopened.verify_integrity() == expected
    reopened.close()


def test_fixture_cas_head_corruption_is_rejected_on_reopen(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    subject = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    advance_bootstrap_fixture_head(subject)
    subject.close()

    connection = sqlite3.connect(database)
    connection.execute(
        "UPDATE fixture_cas_head SET bundle_identity = 'corrupted'"
    )
    connection.commit()
    connection.close()

    with pytest.raises(
        PersistenceIntegrityError,
        match="fixture CAS head does not match",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-a")


def test_journal_payload_corruption_is_rejected_after_schema_is_restored(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    subject = ComponentManagerPersistenceFixture(database, cell_id="cell-a")
    operation = subject.ensure_bootstrap(bootstrap()).record
    subject.append_journal_event(
        operation.ref,
        JournalEvent("entry", "event", b"original"),
    )
    subject.close()

    connection = sqlite3.connect(database)
    trigger_sql = connection.execute(
        """
        SELECT sql FROM sqlite_master
        WHERE type = 'trigger'
          AND name = 'manager_journal_entries_no_update'
        """
    ).fetchone()[0]
    connection.execute(
        "DROP TRIGGER manager_journal_entries_no_update"
    )
    connection.execute(
        "UPDATE manager_journal_entries SET payload = X'74616D7065726564'"
    )
    connection.execute(trigger_sql)
    connection.commit()
    connection.close()

    with pytest.raises(
        PersistenceIntegrityError,
        match="payload digest mismatch",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-a")


def test_orphan_journal_row_is_rejected_by_foreign_key_check(
    tmp_path: Path,
) -> None:
    database = tmp_path / "manager.sqlite3"
    ComponentManagerPersistenceFixture(database, cell_id="cell-a").close()
    placeholder = digest("placeholder")

    connection = sqlite3.connect(database)
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
            'apply',
            'missing-operation',
            1,
            1,
            'orphan-entry',
            'orphan_event',
            X'',
            ?,
            ?,
            ?
        )
        """,
        (placeholder, placeholder, placeholder),
    )
    connection.commit()
    connection.close()

    with pytest.raises(
        PersistenceIntegrityError,
        match="foreign-key",
    ):
        ComponentManagerPersistenceFixture(database, cell_id="cell-a")


def test_immutable_rows_are_database_enforced(tmp_path: Path) -> None:
    subject = store(tmp_path)
    operation, receipt = advance_bootstrap_fixture_head(subject)
    subject.append_journal_event(
        operation.ref,
        JournalEvent("entry", "event", b"payload"),
    )
    connection = subject._connection

    for statement in (
        "UPDATE manager_operations SET target_bundle_identity = 'x'",
        "DELETE FROM manager_operations",
        "UPDATE manager_journal_entries SET event_kind = 'x'",
        "DELETE FROM manager_journal_entries",
        "UPDATE fixture_cas_receipts SET target_bundle_identity = 'x'",
        "DELETE FROM fixture_cas_receipts",
        "UPDATE persistence_metadata SET cell_id = 'x'",
        "DELETE FROM persistence_metadata",
    ):
        with pytest.raises(sqlite3.IntegrityError, match="immutable"):
            connection.execute(statement)

    assert subject.fixture_cas_head().receipt_digest == (
        receipt.receipt_digest
    )


def test_reference_adapter_exposes_no_effect_or_robot_semantics(
    tmp_path: Path,
) -> None:
    subject = store(tmp_path)
    method_names = {
        name
        for name in dir(subject)
        if callable(getattr(subject, name))
    }
    for forbidden in (
        "apply",
        "publish_active",
        "active_publication",
        "start",
        "stop",
        "restart",
        "enable",
        "disable",
        "move",
        "recover",
        "resume",
        "reset",
        "acquire_fence",
        "stage_scene",
        "finalize_scene",
        "infer_readiness",
    ):
        assert forbidden not in method_names

    for normative_name in (
        "ActiveBundlePublicationRequestDTO",
        "ActiveBundlePublicationReceiptDTO",
        "IActiveBundlePublicationStore",
    ):
        assert not hasattr(persistence_fixture, normative_name)

    operation_fields = {field.name for field in fields(ApplyIdentity)}
    assert "retained_bundle" not in operation_fields
    assert "compiler_qualified_ir" not in operation_fields
    assert "runtime_epoch" not in operation_fields
    assert "robot_ready" not in operation_fields


def test_closed_adapter_rejects_future_use(tmp_path: Path) -> None:
    subject = store(tmp_path)
    subject.close()
    subject.close()

    with pytest.raises(PersistenceClosedError):
        subject.verify_integrity()
