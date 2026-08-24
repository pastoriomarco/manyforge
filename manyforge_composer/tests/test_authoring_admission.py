"""Focused fail-closed tests for Composer's per-client authoring barrier."""

from __future__ import annotations

from typing import Any

import pytest
from fastapi import HTTPException

from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
)
from manyforge_composer.backend.authoring_transaction_journal import (
    ComposerAuthoringTransactionJournal,
    owner_transaction_id,
)


class _Headless:
    def __init__(self) -> None:
        self.generation = 7
        self.allowed = True
        self.effects: list[str] = []
        self.claims: list[dict[str, Any]] = []
        self.settlements: list[dict[str, Any]] = []
        self.reconciliations: list[dict[str, Any]] = []

    def authoring_admission(self) -> dict[str, Any]:
        return {
            "authoring_allowed": self.allowed,
            "admission_generation": self.generation,
        }

    def acquire_authoring_transaction(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        self.claims.append(body)
        return {
            "claim": {
                "intent": {
                    "claim_kind": "authoring_mutation",
                    "owner_role": "authoring_owner",
                },
                "claim_generation": 1,
                "acquired_admission_generation": self.generation,
            }
        }

    def settle_authoring_transaction(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        self.settlements.append(body)
        return {"admission": {"admission_generation": self.generation + 1}}

    def recover_authoring_transaction(self, _body: dict[str, Any]) -> dict[str, Any]:
        return {
            "recovery_receipt": {
                "status": "unknown",
                "kernel_owner_epoch": "kernel.1",
                "held_claims": [],
                "current_owner_epochs": [],
            }
        }

    def reconcile_authoring_owner(self, _body: dict[str, Any]) -> dict[str, Any]:
        self.reconciliations.append(_body)
        return {"admission": {"admission_generation": self.generation}}


class _RecoveryHeadless(_Headless):
    def __init__(self, receipt: dict[str, Any]) -> None:
        super().__init__()
        self.receipt = receipt
        self.recoveries: list[dict[str, Any]] = []

    def recover_authoring_transaction(self, body: dict[str, Any]) -> dict[str, Any]:
        self.recoveries.append(body)
        return {"recovery_receipt": self.receipt}


def _durable_transaction(root, *, effect_class: str | None = None):
    journal = ComposerAuthoringTransactionJournal(root)
    transaction = journal.record_intent(
        request_owner_epoch="browser.1",
        request_id="request.1",
        operation_kind="save_draft",
        payload_digest="sha256:" + "1" * 64,
        draft_id="draft.1",
        draft_revision=3,
        admission_generation=7,
        claim_intent={
            "claim_kind": "authoring_mutation",
            "owner_role": "authoring_owner",
            "owner_epoch": "composer.1",
            "owner_transaction_id": owner_transaction_id("browser.1", "request.1"),
            "request_id": "request.1",
            "request_payload_digest": "sha256:" + "1" * 64,
            "expected_admission_generation": 7,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        },
    )
    claim = {"intent": transaction.claim_intent, "claim_generation": 1}
    journal.record(transaction, "claim_json", claim)
    if effect_class is not None:
        journal.record(
            transaction,
            "effect_intent_json",
            {
                "schema_version": "manyforge.composer_authoring_effect.v1",
                "effect_class": effect_class,
                "operation_kind": "save_draft",
            },
        )
    return transaction, claim


def _startup_receipt(*, status="unknown", claim=None, **extra):
    receipt = {
        "status": status,
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [],
        "current_owner_epochs": [],
    }
    if claim is not None:
        receipt["matched_claim"] = claim
    receipt.update(extra)
    return receipt


def _envelope(
    coordinator: AuthoringAdmissionCoordinator,
    client_id: str,
    *,
    sequence: int = 1,
    generation: int = 7,
) -> AuthoringMutationEnvelope:
    return AuthoringMutationEnvelope(
        client_id=client_id,
        snapshot_receiver_epoch=coordinator.receiver_epoch,
        snapshot_sequence=sequence,
        expected_admission_generation=generation,
        request_owner_epoch=f"{client_id}.epoch.1",
        request_id=f"{client_id}.request.1",
    )


def _publish(
    coordinator: AuthoringAdmissionCoordinator,
    client_id: str,
    *,
    sequence: int = 1,
) -> None:
    coordinator.publish_complete_snapshot(
        client_id=client_id,
        sequence=sequence,
        admission_generation=7,
        draft_id="draft.1",
        draft_revision=3,
    )


def test_pre_snapshot_and_stale_generation_have_no_effect(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    effects: list[str] = []

    with pytest.raises(HTTPException) as missing:
        coordinator.execute(
            _envelope(coordinator, "client-a"),
            operation_kind="save_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "save"},
            effect=lambda _claim: effects.append("save"),
            post_image=lambda: {},
        )
    assert missing.value.detail["code"] == "authoring_client_snapshot_stale"

    _publish(coordinator, "client-a")
    headless.generation = 8
    with pytest.raises(HTTPException) as stale:
        coordinator.execute(
            _envelope(coordinator, "client-a"),
            operation_kind="save_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "save"},
            effect=lambda _claim: effects.append("save"),
            post_image=lambda: {},
        )
    assert stale.value.detail["code"] == "stale_generation"
    assert effects == []
    assert headless.claims == []


def test_first_bootstrap_skips_owner_reconciliation(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    assert coordinator.mutation_ready
    assert headless.reconciliations == []


def test_clean_idle_rotation_replaces_only_authoring_owner(tmp_path) -> None:
    receipt = _startup_receipt(
        current_owner_epochs=[
            {"owner_role": "program_runner", "owner_epoch": "runner.1"},
            {"owner_role": "authoring_owner", "owner_epoch": "author.old"},
        ]
    )
    headless = _RecoveryHeadless(receipt)
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)
    coordinator.recover()
    assert coordinator.mutation_ready
    request = headless.reconciliations[0]
    assert request["retained_claims"] == []
    assert request["current_owner_epochs"] == [
        {"owner_role": "program_runner", "owner_epoch": "runner.1"},
        {"owner_role": "authoring_owner", "owner_epoch": coordinator.owner_epoch},
    ]
    assert request["reconciliation_evidence_digest"] == (
        coordinator._owner_reconciliation_digest(
            "authoring_owner", coordinator.owner_epoch
        )
    )


def test_absent_owner_with_predecessor_claim_fails_closed(tmp_path) -> None:
    predecessor = {
        "intent": {
            "owner_role": "authoring_owner",
            "owner_epoch": "author.old",
        }
    }
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(_startup_receipt(held_claims=[predecessor])),
        tmp_path,
    )
    coordinator.recover()
    assert not coordinator.mutation_ready


def test_disconnected_client_does_not_block_another_current_client(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    _publish(coordinator, "client-a")
    _publish(coordinator, "client-b")
    coordinator.disconnect("client-a")

    with pytest.raises(HTTPException):
        coordinator.execute(
            _envelope(coordinator, "client-a"),
            operation_kind="new_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "new"},
            effect=lambda _claim: None,
            post_image=lambda: {},
        )
    result = coordinator.execute(
        _envelope(coordinator, "client-b"),
        operation_kind="new_draft",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "new"},
        effect=lambda _claim: {"status": "committed"},
        post_image=lambda: {"draft_id": "draft.2"},
    )
    assert result == {"status": "committed"}
    assert len(headless.claims) == 1
    assert len(headless.settlements) == 1


def test_snapshot_rotation_after_claim_releases_proven_no_effect(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    _publish(coordinator, "client-a")
    effects: list[str] = []
    acquire = headless.acquire_authoring_transaction

    def acquire_while_snapshot_rotates(body: dict[str, Any]) -> dict[str, Any]:
        result = acquire(body)
        _publish(coordinator, "client-a", sequence=2)
        return result

    headless.acquire_authoring_transaction = acquire_while_snapshot_rotates  # type: ignore[method-assign]
    with pytest.raises(HTTPException) as stale:
        coordinator.execute(
            _envelope(coordinator, "client-a"),
            operation_kind="save_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "save"},
            effect=lambda _claim: effects.append("save"),
            post_image=lambda: {},
        )

    assert stale.value.detail["code"] == "authoring_client_snapshot_stale"
    assert effects == []
    assert coordinator.mutation_ready
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"


def test_successor_recovers_lost_acquire_only_from_exact_claim(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    headless = _RecoveryHeadless(
        _startup_receipt(status="held_exact", claim=claim)
    )
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)
    coordinator.recover()
    assert coordinator.mutation_ready
    assert coordinator._journal.lookup(
        transaction.request_owner_epoch, transaction.request_id
    ).claim == claim


def test_successor_rejects_mismatched_acquire_claim(tmp_path) -> None:
    _transaction, claim = _durable_transaction(tmp_path)
    mismatched = {**claim, "intent": {**claim["intent"], "request_id": "other"}}
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(_startup_receipt(status="held_exact", claim=mismatched)),
        tmp_path,
    )
    coordinator.recover()
    assert not coordinator.mutation_ready


def test_successor_restores_completed_local_post_image_before_opening(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path, effect_class="local")
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record_effect_outcome(
        transaction,
        post_image={"draft_id": "draft.2", "revision": 4},
        outcome={"kind": "mapping", "body": {"ok": True}},
    )
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    restored: list[dict[str, Any]] = []
    receipt = _startup_receipt(
        status="released_exact",
        claim=claim,
        matched_release={"settlement_kind": "owner_commit"},
    )
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(receipt), tmp_path, restore_post_image=restored.append
    )
    coordinator.recover()
    assert coordinator.mutation_ready
    assert restored == [{"draft_id": "draft.2", "revision": 4}]


def test_successor_proves_local_interrupted_effect_had_no_effect(tmp_path) -> None:
    _transaction, claim = _durable_transaction(tmp_path, effect_class="local")
    headless = _RecoveryHeadless(_startup_receipt(status="held_exact", claim=claim))
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)
    coordinator.recover()
    assert coordinator.mutation_ready
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"


def test_successor_replays_external_recovery_callback_exactly(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path, effect_class="external")
    recovered: list[str] = []
    headless = _RecoveryHeadless(_startup_receipt(status="held_exact", claim=claim))
    coordinator = AuthoringAdmissionCoordinator(
        headless,
        tmp_path,
        restore_post_image=lambda image: recovered.append(image["draft_id"]),
        recover_effect=lambda _tx, _claim: (
            {"status": "replayed"},
            {"draft_id": "draft.external", "revision": 9},
        ),
    )
    coordinator.recover()
    assert coordinator.mutation_ready
    assert recovered == ["draft.external"]
    assert headless.settlements[0]["settlement_kind"] == "owner_commit"
    row = coordinator._journal.lookup(
        transaction.request_owner_epoch, transaction.request_id
    )
    assert row is not None and row.outcome == {
        "kind": "mapping",
        "body": {"status": "replayed"},
    }


def test_successor_rejects_mismatched_release_receipt(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record(transaction, "outcome_json", {"kind": "mapping", "body": {}})
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="released_exact",
        claim=claim,
        matched_release={"settlement_kind": "different"},
    )
    coordinator = AuthoringAdmissionCoordinator(_RecoveryHeadless(receipt), tmp_path)
    coordinator.recover()
    assert not coordinator.mutation_ready


def test_successor_retires_unresolved_release_from_exact_reconciliation(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record(transaction, "outcome_json", {"kind": "mapping", "body": {}})
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="reconciled_exact",
        claim=claim,
        matched_reconcile_settlement={"predecessor_claim": claim},
    )
    coordinator = AuthoringAdmissionCoordinator(_RecoveryHeadless(receipt), tmp_path)
    coordinator.recover()
    assert coordinator.mutation_ready
    assert coordinator._journal.unresolved() == []


def test_successor_rejects_mismatched_reconciliation_predecessor_claim(
    tmp_path,
) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record(transaction, "outcome_json", {"kind": "mapping", "body": {}})
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="reconciled_exact",
        claim=claim,
        matched_reconcile_settlement={
            "predecessor_claim": {**claim, "claim_generation": 99}
        },
    )
    coordinator = AuthoringAdmissionCoordinator(_RecoveryHeadless(receipt), tmp_path)
    coordinator.recover()
    assert not coordinator.mutation_ready
