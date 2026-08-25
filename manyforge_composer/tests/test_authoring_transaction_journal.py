"""Durable Composer authoring journal identity and crash-window regressions."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import gc
import os
from pathlib import Path
import sqlite3

import pytest

from manyforge_composer.backend.authoring_transaction_journal import (
    AuthoringJournalError,
    ComposerAuthoringTransactionJournal,
    owner_transaction_id,
)


def _intent(transaction_id: str) -> dict:
    return {
        "claim_kind": "authoring_mutation",
        "owner_role": "authoring_owner",
        "owner_epoch": "composer.1",
        "owner_transaction_id": transaction_id,
        "request_id": "request.1",
        "request_payload_digest": "sha256:" + "1" * 64,
        "expected_admission_generation": 7,
        "scope": {"resources": [], "configurations": []},
        "program_cycle_evidence": None,
        "intervention_evidence": None,
    }


def _record(journal: ComposerAuthoringTransactionJournal, payload: str = "1"):
    transaction_id = owner_transaction_id("browser.1", "request.1")
    return journal.record_intent(
        request_owner_epoch="browser.1",
        request_id="request.1",
        operation_kind="save_draft",
        payload_digest="sha256:" + payload * 64,
        draft_id="draft.1",
        draft_revision=3,
        admission_generation=7,
        claim_intent=_intent(transaction_id),
    )


def test_exact_replay_and_conflict_survive_owner_epoch_rotation(tmp_path) -> None:
    first = ComposerAuthoringTransactionJournal(tmp_path)
    transaction = _record(first)
    first.record(transaction, "acquire_attempted", True)
    successor = ComposerAuthoringTransactionJournal(tmp_path)
    assert successor.owner_epoch != first.owner_epoch
    assert _record(successor).owner_transaction_id == transaction.owner_transaction_id
    with pytest.raises(AuthoringJournalError) as conflict:
        _record(successor, "2")
    assert conflict.value.code == "composer_authoring_request_id_conflict"


def test_capacity_is_bounded_before_new_intent(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path, maximum_transactions=1)
    _record(journal)
    with pytest.raises(AuthoringJournalError) as full:
        journal.record_intent(
            request_owner_epoch="browser.2",
            request_id="request.2",
            operation_kind="apply_draft",
            payload_digest="sha256:" + "2" * 64,
            draft_id="draft.1",
            draft_revision=3,
            admission_generation=8,
            claim_intent=_intent(owner_transaction_id("browser.2", "request.2")),
        )
    assert full.value.code == "composer_authoring_journal_full"


@pytest.mark.parametrize(
    "receipts",
    [
        (),
        (("acquire_attempted", True),),
        (("acquire_attempted", True), ("claim_json", {"claim": 1})),
        (
            ("acquire_attempted", True),
            ("claim_json", {"claim": 1}),
            ("effect_intent_json", {"operation_kind": "save_draft"}),
        ),
        (
            ("acquire_attempted", True),
            ("claim_json", {"claim": 1}),
            ("effect_intent_json", {"operation_kind": "save_draft"}),
            (
                "outcome_json",
                {
                    "kind": "mapping",
                    "settlement_kind": "owner_commit",
                    "body": {},
                },
            ),
            ("post_image_json", {"draft_id": "draft.1"}),
        ),
        (
            ("acquire_attempted", True),
            ("claim_json", {"claim": 1}),
            ("effect_intent_json", {"operation_kind": "save_draft"}),
            (
                "outcome_json",
                {
                    "kind": "mapping",
                    "settlement_kind": "owner_commit",
                    "body": {},
                },
            ),
            ("post_image_json", {"draft_id": "draft.1"}),
            ("release_intent_json", {"settlement_kind": "owner_commit"}),
        ),
    ],
)
def test_each_acquire_effect_release_crash_window_is_rehydrated(
    tmp_path, receipts
) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    transaction = _record(journal)
    for field, value in receipts:
        journal.record(transaction, field, value)
        transaction = journal.lookup("browser.1", "request.1")
        assert transaction is not None
    successor = ComposerAuthoringTransactionJournal(tmp_path)
    unresolved = successor.unresolved()
    assert len(unresolved) == 1
    assert unresolved[0].owner_transaction_id == transaction.owner_transaction_id


def test_post_image_outcome_and_workspace_are_one_atomic_replay_unit(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    transaction = _record(journal)
    journal.record_effect_outcome(
        transaction,
        post_image={"draft_id": "draft.2", "revision": 4},
        outcome={
            "kind": "mapping",
            "settlement_kind": "owner_commit",
            "body": {"saved": True},
        },
    )
    restored = ComposerAuthoringTransactionJournal(tmp_path)
    row = restored.lookup("browser.1", "request.1")
    assert row is not None
    assert row.post_image == {"draft_id": "draft.2", "revision": 4}
    assert row.outcome == {
        "kind": "mapping",
        "settlement_kind": "owner_commit",
        "body": {"saved": True},
    }
    assert restored.retained_workspace() == row.post_image


def test_post_image_outcome_rolls_back_if_workspace_retention_fails(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    transaction = _record(journal)
    journal._database.execute(
        "CREATE TRIGGER reject_retained_workspace BEFORE INSERT ON "
        "retained_workspace BEGIN SELECT RAISE(ABORT, 'injected failure'); END"
    )

    with pytest.raises(sqlite3.IntegrityError, match="injected failure"):
        journal.record_effect_outcome(
            transaction,
            post_image={"draft_id": "draft.2", "revision": 4},
            outcome={
                "kind": "mapping",
                "settlement_kind": "owner_commit",
                "body": {"saved": True},
            },
        )

    restored = journal.lookup("browser.1", "request.1")
    assert restored is not None
    assert restored.post_image is None
    assert restored.outcome is None
    assert journal.retained_workspace() is None


def test_pruning_reuses_completed_capacity_but_never_unresolved(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path, maximum_transactions=2)
    first = _record(journal)
    journal.record(first, "release_receipt_json", {"status": "released"})
    second = journal.record_intent(
        request_owner_epoch="browser.2",
        request_id="request.2",
        operation_kind="apply_draft",
        payload_digest="sha256:" + "2" * 64,
        draft_id="draft.1",
        draft_revision=3,
        admission_generation=8,
        claim_intent=_intent(owner_transaction_id("browser.2", "request.2")),
    )
    third = journal.record_intent(
        request_owner_epoch="browser.3",
        request_id="request.3",
        operation_kind="save_draft",
        payload_digest="sha256:" + "3" * 64,
        draft_id="draft.1",
        draft_revision=3,
        admission_generation=8,
        claim_intent=_intent(owner_transaction_id("browser.3", "request.3")),
    )
    assert journal.lookup("browser.1", "request.1") is None
    assert journal.lookup("browser.2", "request.2") == second
    assert journal.lookup("browser.3", "request.3") == third
    with pytest.raises(AuthoringJournalError) as full:
        journal.record_intent(
            request_owner_epoch="browser.4",
            request_id="request.4",
            operation_kind="save_draft",
            payload_digest="sha256:" + "4" * 64,
            draft_id="draft.1",
            draft_revision=3,
            admission_generation=8,
            claim_intent=_intent(owner_transaction_id("browser.4", "request.4")),
        )
    assert full.value.code == "composer_authoring_journal_full"


def test_owner_rotation_keeps_owner_epoch_table_bounded(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    for _ in range(8):
        journal = ComposerAuthoringTransactionJournal(tmp_path)
    count = journal._database.execute("SELECT count(*) FROM owner_epochs").fetchone()[0]
    assert count == 1


def test_concurrent_requests_serialize_shared_sqlite_connection(tmp_path) -> None:
    journal = ComposerAuthoringTransactionJournal(tmp_path)

    def record(index: int) -> str:
        request_owner_epoch = f"browser.{index}"
        request_id = f"request.{index}"
        transaction_id = owner_transaction_id(request_owner_epoch, request_id)
        transaction = journal.record_intent(
            request_owner_epoch=request_owner_epoch,
            request_id=request_id,
            operation_kind="save_draft",
            payload_digest=f"sha256:{index:064x}",
            draft_id="draft.1",
            draft_revision=3,
            admission_generation=7,
            claim_intent=_intent(transaction_id),
        )
        journal.record(transaction, "acquire_attempted", True)
        return transaction.owner_transaction_id

    with ThreadPoolExecutor(max_workers=16) as executor:
        transaction_ids = list(executor.map(record, range(64)))

    assert len(set(transaction_ids)) == 64
    assert len(journal.unresolved()) == 64
    assert all(transaction.acquire_attempted for transaction in journal.unresolved())


def test_state_root_must_be_one_canonical_non_symlink_capability(tmp_path) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    alias = tmp_path / "state-alias"
    alias.symlink_to(state_root, target_is_directory=True)

    with pytest.raises(AuthoringJournalError) as symlinked:
        ComposerAuthoringTransactionJournal(alias)
    assert symlinked.value.code == "composer_authoring_state_root_invalid"

    relative = Path(os.path.relpath(state_root, Path.cwd()))
    with pytest.raises(AuthoringJournalError) as noncanonical:
        ComposerAuthoringTransactionJournal(relative)
    assert noncanonical.value.code == "composer_authoring_state_root_invalid"
    assert list(state_root.iterdir()) == []


@pytest.mark.parametrize("leaf_kind", ["symlink", "hardlink"])
def test_unsafe_existing_database_leaf_is_rejected_without_mutation(
    tmp_path, leaf_kind: str
) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    external = tmp_path / "external.sqlite3"
    external.write_bytes(b"external")
    external.chmod(0o600)
    database = state_root / "composer-authoring-transactions.sqlite3"
    if leaf_kind == "symlink":
        database.symlink_to(external)
    else:
        os.link(external, database)
    before = external.read_bytes()

    with pytest.raises(AuthoringJournalError) as unsafe:
        ComposerAuthoringTransactionJournal(state_root)
    assert unsafe.value.code == "composer_authoring_journal_invalid"
    assert external.read_bytes() == before


@pytest.mark.parametrize("suffix", ["-journal", "-wal", "-shm"])
def test_absent_database_rejects_orphan_sidecar_without_mutation(
    tmp_path: Path,
    suffix: str,
) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    sidecar = state_root / f"composer-authoring-transactions.sqlite3{suffix}"
    sidecar.write_bytes(b"orphaned durable state\n")
    sidecar.chmod(0o600)
    before = (sidecar.read_bytes(), sidecar.stat())

    with pytest.raises(AuthoringJournalError) as orphaned:
        ComposerAuthoringTransactionJournal(state_root)

    assert orphaned.value.code == "composer_authoring_journal_invalid"
    assert not (state_root / "composer-authoring-transactions.sqlite3").exists()
    assert sidecar.read_bytes() == before[0]
    after = sidecar.stat()
    assert (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) == (
        before[1].st_dev,
        before[1].st_ino,
        before[1].st_size,
        before[1].st_mtime_ns,
    )


def test_failed_construction_does_not_leak_the_state_root_descriptor(
    tmp_path: Path,
) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    external = tmp_path / "external.sqlite3"
    external.write_bytes(b"unsafe external state\n")
    database = state_root / "composer-authoring-transactions.sqlite3"
    database.symlink_to(external)
    before = len(os.listdir("/proc/self/fd"))

    for _ in range(32):
        with pytest.raises(AuthoringJournalError):
            ComposerAuthoringTransactionJournal(state_root)
    gc.collect()

    assert len(os.listdir("/proc/self/fd")) <= before + 1


def test_replaced_state_root_is_rejected_before_the_next_transaction(tmp_path) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    journal = ComposerAuthoringTransactionJournal(state_root)
    retained = tmp_path / "retained-state"
    state_root.rename(retained)
    state_root.mkdir(mode=0o700)

    with pytest.raises(AuthoringJournalError) as replaced:
        journal.lookup("browser.1", "request.1")
    assert replaced.value.code == "composer_authoring_journal_invalid"
    assert list(state_root.iterdir()) == []
