"""Focused fail-closed tests for Composer's durable authoring owner."""

from __future__ import annotations

from typing import Any

import pytest
from fastapi import HTTPException
from manyforge_headless_api.client import HeadlessClientError

from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
    headless_client_http_exception,
)
from manyforge_composer.backend.authoring_transaction_journal import (
    ComposerAuthoringTransactionJournal,
    canonical_digest,
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
    _coordinator: AuthoringAdmissionCoordinator,
    client_id: str,
    *,
    generation: int = 7,
) -> AuthoringMutationEnvelope:
    return AuthoringMutationEnvelope(
        expected_admission_generation=generation,
        request_owner_epoch=f"{client_id}.epoch.1",
        request_id=f"{client_id}.request.1",
    )


def _backend_envelope(
    *,
    request_id: str = "assistant.request.1",
    generation: int = 7,
) -> AuthoringMutationEnvelope:
    return AuthoringMutationEnvelope(
        expected_admission_generation=generation,
        request_owner_epoch="assistant.epoch.1",
        request_id=request_id,
    )


def _owner_proven_no_effect_error() -> HeadlessClientError:
    response = {
        "detail": {
            "code": "program_apply_deployment_not_active",
            "details": {"effect_disposition": "proven_no_effect"},
            "message": "the deployment is not active",
        }
    }
    return HeadlessClientError(
        "program_apply_deployment_not_active",
        "the deployment is not active",
        409,
        details={"effect_disposition": "proven_no_effect"},
        response=response,
    )


def test_stale_backend_generation_has_no_effect(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    effects: list[str] = []

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


def test_request_owners_are_independent_of_browser_transport(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    first = coordinator.execute(
        _envelope(coordinator, "client-a"),
        operation_kind="new_draft",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "new-a"},
        effect=lambda _claim: {"status": "committed-a"},
        post_image=lambda: {"draft_id": "draft.2"},
    )
    second = coordinator.execute(
        _envelope(coordinator, "client-b"),
        operation_kind="new_draft",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "new"},
        effect=lambda _claim: {"status": "committed"},
        post_image=lambda: {"draft_id": "draft.2"},
    )
    assert first == {"status": "committed-a"}
    assert second == {"status": "committed"}
    assert len(headless.claims) == 2
    assert len(headless.settlements) == 2


def test_backend_mutation_uses_kernel_admission_without_browser_state(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()

    # Browser presentation state is deliberately absent from this owner path.
    result = coordinator.execute(
        _backend_envelope(),
        operation_kind="assistant_scene_draft_add_object",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "assistant edit"},
        effect=lambda _claim: {"status": "committed"},
        post_image=lambda: {"draft_id": "draft.1", "revision": 4},
    )

    assert result == {"status": "committed"}
    assert len(headless.claims) == 1
    assert len(headless.settlements) == 1
    assert coordinator.completed_admission_generation(_backend_envelope()) == 8


def test_backend_validation_under_claim_precedes_effect_intent(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    envelope = _backend_envelope(request_id="assistant.request.rejected")
    effects: list[str] = []

    def reject_under_claim(_claim: dict[str, Any]) -> None:
        raise HTTPException(
            status_code=409,
            detail={"code": "assistant_callback_context_stale"},
        )

    with pytest.raises(HTTPException) as rejected:
        coordinator.execute(
            envelope,
            operation_kind="assistant_scene_draft_add_object",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "assistant edit"},
            effect=lambda _claim: effects.append("effect"),
            post_image=lambda: {"draft_id": "draft.1", "revision": 4},
            validate_under_claim=reject_under_claim,
        )

    assert rejected.value.detail["code"] == "assistant_callback_context_stale"
    assert effects == []
    transaction = coordinator._journal.lookup(
        envelope.request_owner_epoch, envelope.request_id
    )
    assert transaction is not None
    assert transaction.effect_intent is None
    assert transaction.complete
    assert headless.settlements[-1]["settlement_kind"] == "proven_no_effect"


def test_transport_independent_commit_replays_without_second_claim(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    effects: list[str] = []
    envelope = _envelope(coordinator, "client-a")
    result = coordinator.execute(
        envelope,
        operation_kind="save_draft",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "save"},
        effect=lambda _claim: effects.append("save") or {"saved": True},
        post_image=lambda: {"draft_id": "draft.1", "revision": 3},
    )

    assert result == {"saved": True}
    assert effects == ["save"]
    assert coordinator.mutation_ready
    assert headless.settlements[0]["settlement_kind"] == "owner_commit"

    # The exact completed request is a durable owner outcome. It must replay
    # without acquiring or settling a second Kernel claim.
    claims = len(headless.claims)
    settlements = len(headless.settlements)
    replayed = coordinator.execute(
        envelope,
        operation_kind="save_draft",
        draft_id="draft.1",
        draft_revision=3,
        payload={"operation": "save"},
        effect=lambda _claim: effects.append("save"),
        post_image=lambda: {},
    )
    assert replayed == result
    assert effects == ["save"]
    assert len(headless.claims) == claims
    assert len(headless.settlements) == settlements

    with pytest.raises(HTTPException) as conflict:
        coordinator.execute(
            envelope,
            operation_kind="save_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "different"},
            effect=lambda _claim: None,
            post_image=lambda: {},
        )
    assert conflict.value.detail["code"] == "composer_authoring_request_id_conflict"


def test_post_release_followup_runs_only_after_durable_release_and_on_replay(
    tmp_path,
) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    envelope = _envelope(coordinator, "activation")
    observations: list[int] = []

    def after_release() -> None:
        observations.append(len(headless.settlements))

    arguments = {
        "operation_kind": "apply_draft",
        "draft_id": "draft.1",
        "draft_revision": 3,
        "payload": {"operation": "apply"},
        "effect": lambda _claim: {"applied": True},
        "post_image": lambda: {"draft_id": "draft.1", "revision": 3},
        "after_release": after_release,
    }
    assert coordinator.execute(envelope, **arguments) == {"applied": True}
    assert observations == [1]

    assert coordinator.execute(envelope, **arguments) == {"applied": True}
    assert observations == [1, 1]
    assert len(headless.claims) == 1


def test_owner_proven_external_rejection_settles_and_replays_exactly(
    tmp_path,
) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    envelope = _envelope(coordinator, "client-a")

    def reject(_claim):
        error = _owner_proven_no_effect_error()
        raise headless_client_http_exception(error) from error

    with pytest.raises(HTTPException) as rejected:
        coordinator.execute(
            envelope,
            operation_kind="apply_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "apply"},
            effect=reject,
            post_image=lambda: {},
            prepare_effect=lambda: {
                "effect_class": "external",
                "external_kind": "program_apply",
            },
        )

    assert rejected.value.status_code == 409
    assert rejected.value.detail == {
        "code": "program_apply_deployment_not_active",
        "message": "the deployment is not active",
    }
    assert coordinator.mutation_ready
    assert headless.settlements[-1]["settlement_kind"] == "proven_no_effect"

    with pytest.raises(HTTPException) as replayed:
        coordinator.execute(
            envelope,
            operation_kind="apply_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "apply"},
            effect=lambda _claim: pytest.fail("exact replay reran the effect"),
            post_image=lambda: {},
        )
    assert replayed.value.status_code == rejected.value.status_code
    assert replayed.value.detail == rejected.value.detail


def test_unproven_external_rejection_remains_recovery_blocked(tmp_path) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()

    def reject(_claim):
        error = HeadlessClientError(
            "headless_http_unavailable",
            "the response was lost",
        )
        raise headless_client_http_exception(error) from error

    with pytest.raises(HTTPException):
        coordinator.execute(
            _envelope(coordinator, "client-a"),
            operation_kind="apply_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload={"operation": "apply"},
            effect=reject,
            post_image=lambda: {},
            prepare_effect=lambda: {
                "effect_class": "external",
                "external_kind": "program_apply",
            },
        )

    assert not coordinator.mutation_ready
    assert headless.settlements == []


def test_completed_replay_rejects_unknown_durable_outcome_without_effect(
    tmp_path,
) -> None:
    headless = _Headless()
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)  # type: ignore[arg-type]
    coordinator.recover()
    envelope = _envelope(coordinator, "client-a")
    payload = {"operation": "save"}
    transaction = coordinator._journal.record_intent(
        request_owner_epoch=envelope.request_owner_epoch,
        request_id=envelope.request_id,
        operation_kind="save_draft",
        payload_digest=canonical_digest(payload),
        draft_id="draft.1",
        draft_revision=3,
        admission_generation=7,
        claim_intent={"owner_transaction_id": "corrupt.fixture"},
    )
    coordinator._journal.record(
        transaction,
        "outcome_json",
        {"kind": "unsupported", "body": {"saved": True}},
    )
    coordinator._journal.record(
        transaction,
        "release_receipt_json",
        {"disposition": "released.fixture"},
    )
    effects: list[str] = []

    with pytest.raises(HTTPException) as rejected:
        coordinator.execute(
            envelope,
            operation_kind="save_draft",
            draft_id="draft.1",
            draft_revision=3,
            payload=payload,
            effect=lambda _claim: effects.append("save"),
            post_image=lambda: {},
        )

    assert rejected.value.status_code == 503
    assert rejected.value.detail["code"] == "composer_authoring_outcome_invalid"
    assert effects == []
    assert headless.claims == []
    assert headless.settlements == []


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
        outcome={
            "kind": "mapping",
            "settlement_kind": "owner_commit",
            "body": {"ok": True},
        },
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
        "settlement_kind": "owner_commit",
        "body": {"status": "replayed"},
    }


def test_successor_settles_owner_proven_external_no_effect(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path, effect_class="external")
    headless = _RecoveryHeadless(_startup_receipt(status="held_exact", claim=claim))

    def recover(_transaction, _claim):
        raise _owner_proven_no_effect_error()

    coordinator = AuthoringAdmissionCoordinator(
        headless,
        tmp_path,
        recover_effect=recover,
    )
    coordinator.recover()

    assert coordinator.mutation_ready
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"
    row = coordinator._journal.lookup(
        transaction.request_owner_epoch, transaction.request_id
    )
    assert row is not None and row.outcome == {
        "kind": "http_exception",
        "settlement_kind": "proven_no_effect",
        "status_code": 409,
        "detail": {
            "code": "program_apply_deployment_not_active",
            "message": "the deployment is not active",
        },
        "headers": None,
    }


def test_successor_uses_durable_no_effect_disposition_before_release_intent(
    tmp_path,
) -> None:
    transaction, claim = _durable_transaction(tmp_path, effect_class="external")
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record(
        transaction,
        "outcome_json",
        {
            "kind": "http_exception",
            "settlement_kind": "proven_no_effect",
            "status_code": 409,
            "detail": {
                "code": "program_apply_deployment_not_active",
                "message": "the deployment is not active",
            },
            "headers": None,
        },
    )
    headless = _RecoveryHeadless(_startup_receipt(status="held_exact", claim=claim))
    coordinator = AuthoringAdmissionCoordinator(
        headless,
        tmp_path,
        recover_effect=lambda *_args: pytest.fail(
            "durable no-effect outcome reran recovery"
        ),
    )

    coordinator.recover()

    assert coordinator.mutation_ready
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"


def test_successor_rejects_mismatched_release_receipt(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record_effect_outcome(
        transaction,
        post_image={"draft_id": "draft.1", "revision": 3},
        outcome={
            "kind": "mapping",
            "settlement_kind": "owner_commit",
            "body": {},
        },
    )
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="released_exact",
        claim=claim,
        matched_release={"settlement_kind": "different"},
    )
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(receipt),
        tmp_path,
        restore_post_image=lambda _image: None,
    )
    coordinator.recover()
    assert not coordinator.mutation_ready


def test_successor_retires_unresolved_release_from_exact_reconciliation(tmp_path) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record_effect_outcome(
        transaction,
        post_image={"draft_id": "draft.1", "revision": 3},
        outcome={
            "kind": "mapping",
            "settlement_kind": "owner_commit",
            "body": {},
        },
    )
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="reconciled_exact",
        claim=claim,
        matched_reconcile_settlement={"predecessor_claim": claim},
    )
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(receipt),
        tmp_path,
        restore_post_image=lambda _image: None,
    )
    coordinator.recover()
    assert coordinator.mutation_ready
    assert coordinator._journal.unresolved() == []


def test_successor_rejects_mismatched_reconciliation_predecessor_claim(
    tmp_path,
) -> None:
    transaction, claim = _durable_transaction(tmp_path)
    journal = ComposerAuthoringTransactionJournal(tmp_path)
    journal.record_effect_outcome(
        transaction,
        post_image={"draft_id": "draft.1", "revision": 3},
        outcome={
            "kind": "mapping",
            "settlement_kind": "owner_commit",
            "body": {},
        },
    )
    journal.record(transaction, "release_intent_json", {"settlement_kind": "owner_commit"})
    receipt = _startup_receipt(
        status="reconciled_exact",
        claim=claim,
        matched_reconcile_settlement={
            "predecessor_claim": {**claim, "claim_generation": 99}
        },
    )
    coordinator = AuthoringAdmissionCoordinator(
        _RecoveryHeadless(receipt),
        tmp_path,
        restore_post_image=lambda _image: None,
    )
    coordinator.recover()
    assert not coordinator.mutation_ready
