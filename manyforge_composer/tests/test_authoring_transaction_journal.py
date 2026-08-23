"""Durable Composer authoring journal identity and crash-window regressions."""

from __future__ import annotations

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
            ("outcome_json", {"kind": "mapping", "body": {}}),
            ("post_image_json", {"draft_id": "draft.1"}),
        ),
        (
            ("acquire_attempted", True),
            ("claim_json", {"claim": 1}),
            ("effect_intent_json", {"operation_kind": "save_draft"}),
            ("outcome_json", {"kind": "mapping", "body": {}}),
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
        outcome={"kind": "mapping", "body": {"saved": True}},
    )
    restored = ComposerAuthoringTransactionJournal(tmp_path)
    row = restored.lookup("browser.1", "request.1")
    assert row is not None
    assert row.post_image == {"draft_id": "draft.2", "revision": 4}
    assert row.outcome == {"kind": "mapping", "body": {"saved": True}}
    assert restored.retained_workspace() == row.post_image


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
