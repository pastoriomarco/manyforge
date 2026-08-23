"""Per-client barrier plus durable Composer authoring transaction owner."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from threading import RLock
import time
from typing import Any, Callable, Mapping, TypeVar
import uuid

from fastapi import HTTPException
from fastapi.responses import JSONResponse
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.cell_admission import (
    cell_admission_owner_epoch_reconciliation_evidence_digest,
)

from .authoring_transaction_journal import (
    AuthoringJournalError,
    AuthoringTransaction,
    ComposerAuthoringTransactionJournal,
    canonical_digest,
    owner_transaction_id,
)

_T = TypeVar("_T")
_MAX_CLIENTS = 1024
_FRESH_SECONDS = 2.0


@dataclass(frozen=True, slots=True)
class AuthoringMutationEnvelope:
    client_id: str
    snapshot_receiver_epoch: str
    snapshot_sequence: int
    expected_admission_generation: int
    request_owner_epoch: str
    request_id: str


@dataclass(slots=True)
class _ClientSnapshot:
    receiver_epoch: str
    sequence: int
    admission_generation: int
    draft_id: str
    draft_revision: int
    observed_at: float


class AuthoringAdmissionCoordinator:
    """Composer owner integration; Kernel remains admission authority."""

    def __init__(
        self,
        client: HeadlessHttpClient,
        state_root: Path,
        *,
        restore_post_image: Callable[[dict[str, Any]], None] | None = None,
        recover_effect: Callable[
            [AuthoringTransaction, dict[str, Any]],
            tuple[Any, Mapping[str, Any]] | None,
        ]
        | None = None,
    ) -> None:
        self._client = client
        self._journal = ComposerAuthoringTransactionJournal(state_root)
        self._restore_post_image = restore_post_image
        self._recover_effect = recover_effect
        self.owner_epoch = self._journal.owner_epoch
        self.receiver_epoch = f"composer-snapshot-{uuid.uuid4().hex}"
        self._lock = RLock()
        self._snapshots: dict[str, _ClientSnapshot] = {}
        self._ready = False
        self._blocked_reason: str | None = "authoring recovery has not run"

    @property
    def mutation_ready(self) -> bool:
        return self._ready

    @staticmethod
    def _unavailable(code: str, message: str) -> HTTPException:
        return HTTPException(status_code=503, detail={"code": code, "message": message})

    def recover(self) -> None:
        """Resolve predecessor rows, then rotate the exact Kernel owner role."""

        try:
            retained = self._journal.retained_workspace()
            if retained is not None:
                if self._restore_post_image is None:
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_invalid",
                        "retained Composer workspace has no restore owner",
                    )
                self._restore_post_image(retained)
            receipt: dict[str, Any] | None = None
            for transaction in self._journal.unresolved():
                response = self._client.recover_authoring_transaction(
                    {
                        "intent": transaction.claim_intent,
                        "expected_claim": transaction.claim,
                        "expected_release": transaction.release_intent,
                    }
                )
                receipt = response.get("recovery_receipt")
                if not isinstance(receipt, dict):
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_invalid",
                        "Headless omitted the exact Kernel recovery receipt",
                    )
                self._recover_transaction(transaction, receipt)

            # Refresh the complete Kernel inventory after predecessor
            # settlement; a per-transaction receipt may predate its release.
            receipt = None
            if receipt is None:
                probe_id = owner_transaction_id(self.owner_epoch, "startup-probe")
                probe_intent = {
                    "claim_kind": "authoring_mutation",
                    "owner_role": "authoring_owner",
                    "owner_epoch": self.owner_epoch,
                    "owner_transaction_id": probe_id,
                    "request_id": "composer-startup-probe",
                    "request_payload_digest": canonical_digest({"probe": probe_id}),
                    "expected_admission_generation": 1,
                    "scope": {"resources": [], "configurations": []},
                    "program_cycle_evidence": None,
                    "intervention_evidence": None,
                }
                response = self._client.recover_authoring_transaction(
                    {
                        "intent": probe_intent,
                        "expected_claim": None,
                        "expected_release": None,
                    }
                )
                receipt = response.get("recovery_receipt")
            if not isinstance(receipt, dict):
                raise AuthoringJournalError(
                    "composer_authoring_recovery_invalid",
                    "Kernel owner-epoch evidence is unavailable",
                )
            retained = receipt.get("held_claims")
            if not isinstance(retained, list):
                raise AuthoringJournalError(
                    "composer_authoring_recovery_invalid",
                    "Kernel held-claim inventory is unavailable",
                )
            authoring_owners = [
                item
                for item in receipt.get("current_owner_epochs", [])
                if isinstance(item, dict)
                and item.get("owner_role") == "authoring_owner"
            ]
            predecessor_claims = [
                claim
                for claim in retained
                if isinstance(claim, dict)
                and isinstance(claim.get("intent"), dict)
                and claim["intent"].get("owner_role") == "authoring_owner"
                and claim["intent"].get("owner_epoch") != self.owner_epoch
            ]
            if not authoring_owners:
                if predecessor_claims or self._journal.unresolved():
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_conflict",
                        "Kernel omitted authoring ownership while predecessor claims remain",
                    )
                # A role is installed only by the first successful Acquire.
                self._blocked_reason = None
                self._ready = True
                return
            if len(authoring_owners) != 1:
                raise AuthoringJournalError(
                    "composer_authoring_recovery_conflict",
                    "Kernel authoring-owner inventory is not exact",
                )
            if authoring_owners[0].get("owner_epoch") == self.owner_epoch:
                if predecessor_claims:
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_conflict",
                        "current authoring epoch retains a predecessor claim",
                    )
                self._blocked_reason = None
                self._ready = True
                return
            owners = [
                item
                for item in receipt.get("current_owner_epochs", [])
                if item.get("owner_role") != "authoring_owner"
            ]
            owners.append(
                {"owner_role": "authoring_owner", "owner_epoch": self.owner_epoch}
            )
            owner_order = {
                role: index
                for index, role in enumerate(
                    (
                        "program_runner",
                        "authoring_owner",
                        "manager",
                        "component_lifecycle_owner",
                        "authored_scene_owner",
                        "manual_operation_owner",
                        "intervention_or_recovery_owner",
                    )
                )
            }
            owners.sort(key=lambda item: owner_order[item["owner_role"]])
            reconciliation = {
                "kernel_owner_epoch": receipt.get("kernel_owner_epoch"),
                "retained_claims": retained,
                "current_owner_epochs": owners,
                "reconciliation_evidence_digest": self._owner_reconciliation_digest(
                    "authoring_owner", self.owner_epoch
                ),
            }
            self._client.reconcile_authoring_owner(reconciliation)
        except Exception as error:
            self._blocked_reason = str(error)
            self._ready = False
            return
        self._blocked_reason = None
        self._ready = True

    @staticmethod
    def _owner_reconciliation_digest(owner_role: str, owner_epoch: str) -> str:
        return cell_admission_owner_epoch_reconciliation_evidence_digest(
            owner_role, owner_epoch
        )

    def _recover_transaction(
        self, transaction: AuthoringTransaction, receipt: Mapping[str, Any]
    ) -> None:
        status = receipt.get("status")
        if status == "conflicting":
            raise AuthoringJournalError(
                "composer_authoring_recovery_conflict",
                "Kernel recovery evidence contradicts the Composer journal",
            )
        if transaction.complete:
            if not self._receipt_settles_exact(transaction, receipt):
                raise AuthoringJournalError(
                    "composer_authoring_recovery_conflict",
                    "durable release receipt is not present in Kernel",
                )
            return
        if transaction.claim is None and status == "held_exact":
            matched_claim = receipt.get("matched_claim")
            if not isinstance(matched_claim, dict):
                raise AuthoringJournalError(
                    "composer_authoring_acquire_unknown",
                    "Kernel held-claim recovery omitted the exact claim",
                )
            if matched_claim.get("intent") != transaction.claim_intent:
                raise AuthoringJournalError(
                    "composer_authoring_acquire_unknown",
                    "Kernel held a different authoring claim intent",
                )
            self._journal.record(transaction, "claim_json", matched_claim)
            transaction = self._journal.lookup(
                transaction.request_owner_epoch, transaction.request_id
            )
            assert transaction is not None
        if transaction.claim is None:
            if transaction.acquire_attempted and status != "unknown":
                raise AuthoringJournalError(
                    "composer_authoring_acquire_unknown",
                    "acquire outcome cannot be proven from the exact receipt",
                )
            self._journal.record(
                transaction,
                "outcome_json",
                self._recovered_no_effect_outcome(transaction),
            )
            transaction = self._journal.lookup(
                transaction.request_owner_epoch, transaction.request_id
            )
            assert transaction is not None
            self._journal.record(
                transaction,
                "release_receipt_json",
                {"disposition": "proven_not_acquired"},
            )
            return
        if status not in {"held_exact", "released_exact", "reconciled_exact"}:
            raise AuthoringJournalError(
                "composer_authoring_claim_unknown",
                "the predecessor authoring claim is not exactly recoverable",
            )
        if receipt.get("matched_claim") != transaction.claim:
            raise AuthoringJournalError(
                "composer_authoring_claim_unknown",
                "Kernel recovery did not match the exact durable authoring claim",
            )
        if status in {"released_exact", "reconciled_exact"}:
            if transaction.outcome is None or not self._receipt_settles_exact(
                transaction, receipt
            ):
                raise AuthoringJournalError(
                    "composer_authoring_outcome_unavailable",
                    "Kernel released a transaction without a durable owner outcome",
                )
            self._journal.record(
                transaction,
                "release_receipt_json",
                {"disposition": status, "recovered": True},
            )
            return

        settlement = "owner_commit"
        if transaction.outcome is None:
            recovered_effect = None
            external_effect = (
                transaction.effect_intent is not None
                and transaction.effect_intent.get("effect_class") == "external"
            )
            if (
                external_effect
                and self._recover_effect is not None
            ):
                recovered_effect = self._recover_effect(
                    transaction, transaction.claim
                )
            if recovered_effect is not None:
                result, post_image = recovered_effect
                self._journal.record_effect_outcome(
                    transaction,
                    post_image=post_image,
                    outcome=self._stored_result(result),
                )
                if self._restore_post_image is not None:
                    self._restore_post_image(dict(post_image))
                transaction = self._journal.lookup(
                    transaction.request_owner_epoch, transaction.request_id
                )
                assert transaction is not None
            elif not external_effect:
                settlement = "proven_no_effect"
                self._journal.record(
                    transaction,
                    "outcome_json",
                    self._recovered_no_effect_outcome(transaction),
                )
                transaction = self._journal.lookup(
                    transaction.request_owner_epoch, transaction.request_id
                )
                assert transaction is not None
            else:
                raise AuthoringJournalError(
                    "composer_authoring_effect_unprovable",
                    f"{transaction.operation_kind} recovery lacks exact post-image evidence",
                )
        release = self._release(transaction, settlement)
        self._journal.record(transaction, "release_intent_json", release)
        response = self._client.settle_authoring_transaction(
            {
                "claim": transaction.claim,
                "settlement_kind": settlement,
                "evidence_digest": release["evidence_digest"],
            }
        )
        self._journal.record(transaction, "release_receipt_json", response)

    @staticmethod
    def _receipt_settles_exact(
        transaction: AuthoringTransaction, receipt: Mapping[str, Any]
    ) -> bool:
        if transaction.claim is None or transaction.release_intent is None:
            return False
        status = receipt.get("status")
        if receipt.get("matched_claim") != transaction.claim:
            return False
        if status == "released_exact":
            return receipt.get("matched_release") == transaction.release_intent
        if status != "reconciled_exact":
            return False
        settlement = receipt.get("matched_reconcile_settlement")
        return (
            isinstance(settlement, dict)
            and settlement.get("predecessor_claim") == transaction.claim
        )

    @staticmethod
    def _recovered_no_effect_outcome(
        transaction: AuthoringTransaction,
    ) -> dict[str, Any]:
        return {
            "kind": "json_response",
            "status_code": 503,
            "body": {
                "detail": {
                    "code": "composer_authoring_effect_not_committed",
                    "message": (
                        "the interrupted authoring operation was proven not committed; "
                        "refresh and retry"
                    ),
                    "ownerTransactionId": transaction.owner_transaction_id,
                }
            },
        }

    def publish_complete_snapshot(
        self,
        *,
        client_id: str,
        sequence: int,
        admission_generation: int,
        draft_id: str,
        draft_revision: int,
    ) -> None:
        if not self._ready:
            raise ValueError(self._blocked_reason or "authoring recovery is blocked")
        if not client_id or sequence < 1:
            raise ValueError("snapshot identity is invalid")
        with self._lock:
            previous = self._snapshots.get(client_id)
            if previous is not None and sequence <= previous.sequence:
                raise ValueError("snapshot sequence must increase")
            if previous is None and len(self._snapshots) >= _MAX_CLIENTS:
                oldest = min(
                    self._snapshots, key=lambda key: self._snapshots[key].observed_at
                )
                del self._snapshots[oldest]
            self._snapshots[client_id] = _ClientSnapshot(
                receiver_epoch=self.receiver_epoch,
                sequence=sequence,
                admission_generation=admission_generation,
                draft_id=draft_id,
                draft_revision=draft_revision,
                observed_at=time.monotonic(),
            )

    def disconnect(self, client_id: str) -> None:
        with self._lock:
            self._snapshots.pop(client_id, None)

    def _require_snapshot(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        draft_id: str,
        draft_revision: int,
    ) -> None:
        if not self._ready:
            raise self._unavailable(
                "composer_authoring_recovery_blocked",
                self._blocked_reason or "authoring recovery is blocked",
            )
        with self._lock:
            snapshot = self._snapshots.get(envelope.client_id)
            valid = (
                snapshot is not None
                and snapshot.receiver_epoch == envelope.snapshot_receiver_epoch
                and snapshot.sequence == envelope.snapshot_sequence
                and snapshot.admission_generation
                == envelope.expected_admission_generation
                and snapshot.draft_id == draft_id
                and snapshot.draft_revision == draft_revision
                and time.monotonic() - snapshot.observed_at <= _FRESH_SECONDS
            )
        if not valid:
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "authoring_client_snapshot_stale",
                    "message": "authoring requires one complete current client snapshot",
                },
            )

    @staticmethod
    def _stored_result(result: Any) -> dict[str, Any]:
        if isinstance(result, JSONResponse):
            return {
                "kind": "json_response",
                "status_code": result.status_code,
                "body": json.loads(result.body),
            }
        if isinstance(result, dict):
            return {"kind": "mapping", "body": result}
        raise AuthoringJournalError(
            "composer_authoring_result_invalid",
            "authoring owner result is not replayable JSON",
        )

    @staticmethod
    def _replay(outcome: Mapping[str, Any]) -> Any:
        body = outcome.get("body")
        if outcome.get("kind") == "json_response":
            return JSONResponse(status_code=int(outcome["status_code"]), content=body)
        return body

    @staticmethod
    def _release(
        transaction: AuthoringTransaction, settlement_kind: str
    ) -> dict[str, Any]:
        return {
            "settlement_kind": settlement_kind,
            "evidence_digest": canonical_digest(
                {
                    "owner_transaction_id": transaction.owner_transaction_id,
                    "settlement_kind": settlement_kind,
                    "outcome": transaction.outcome,
                }
            ),
        }

    def execute(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        effect: Callable[[dict[str, Any]], _T],
        post_image: Callable[[], Mapping[str, Any]],
        prepare_effect: Callable[[], Mapping[str, Any]] | None = None,
    ) -> _T:
        self._require_snapshot(
            envelope, draft_id=draft_id, draft_revision=draft_revision
        )
        payload_digest = canonical_digest(payload)
        replay = self._journal.lookup(
            envelope.request_owner_epoch, envelope.request_id
        )
        if replay is not None:
            if replay.payload_digest != payload_digest or replay.operation_kind != operation_kind:
                raise self._unavailable(
                    "composer_authoring_request_id_conflict",
                    "authoring request ID is bound to another operation",
                )
            if replay.complete and replay.outcome is not None:
                return self._replay(replay.outcome)
            raise self._unavailable(
                "composer_authoring_recovery_blocked",
                "the exact authoring request has an unresolved durable outcome",
            )

        prepared_effect = (
            dict(prepare_effect())
            if prepare_effect is not None
            else {
                "operation_kind": operation_kind,
                "payload_digest": payload_digest,
            }
        )
        admission = self._client.authoring_admission()
        if admission.get("authoring_allowed") is not True:
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "authoring_not_ready",
                    "message": "editing requires idle execution and healthy components",
                },
            )
        if admission.get("admission_generation") != envelope.expected_admission_generation:
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "stale_generation",
                    "message": "authoring admission generation changed",
                },
            )
        transaction_id = owner_transaction_id(
            envelope.request_owner_epoch, envelope.request_id
        )
        intent = {
            "claim_kind": "authoring_mutation",
            "owner_role": "authoring_owner",
            "owner_epoch": self.owner_epoch,
            "owner_transaction_id": transaction_id,
            "request_id": transaction_id,
            "request_payload_digest": payload_digest,
            "expected_admission_generation": envelope.expected_admission_generation,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
        try:
            transaction = self._journal.record_intent(
                request_owner_epoch=envelope.request_owner_epoch,
                request_id=envelope.request_id,
                operation_kind=operation_kind,
                payload_digest=payload_digest,
                draft_id=draft_id,
                draft_revision=draft_revision,
                admission_generation=envelope.expected_admission_generation,
                claim_intent=intent,
            )
            self._journal.record(transaction, "acquire_attempted", True)
            claim_response = self._client.acquire_authoring_transaction(
                {
                    "request_id": transaction_id,
                    "request_owner_epoch": self.owner_epoch,
                    "source_label": "manyforge_composer",
                    "correlation_id": None,
                    "expected_admission_generation": envelope.expected_admission_generation,
                    "request_payload_digest": payload_digest,
                    "owner_transaction_id": transaction_id,
                }
            )
            claim = claim_response.get("claim")
            if not isinstance(claim, dict):
                raise AuthoringJournalError(
                    "authoring_claim_invalid",
                    "Headless omitted the exact authoring claim",
                )
            self._journal.record(transaction, "claim_json", claim)
            transaction = self._journal.lookup(
                envelope.request_owner_epoch, envelope.request_id
            )
            assert transaction is not None
            self._require_snapshot(
                envelope, draft_id=draft_id, draft_revision=draft_revision
            )
            self._journal.record(
                transaction,
                "effect_intent_json",
                prepared_effect,
            )
            result = effect(claim)
            outcome = self._stored_result(result)
            self._journal.record_effect_outcome(
                transaction, post_image=post_image(), outcome=outcome
            )
            transaction = self._journal.lookup(
                envelope.request_owner_epoch, envelope.request_id
            )
            assert transaction is not None
            release = self._release(transaction, "owner_commit")
            self._journal.record(transaction, "release_intent_json", release)
            receipt = self._client.settle_authoring_transaction(
                {
                    "claim": claim,
                    "settlement_kind": "owner_commit",
                    "evidence_digest": release["evidence_digest"],
                }
            )
            self._journal.record(transaction, "release_receipt_json", receipt)
            return result
        except HTTPException:
            # HTTP status cannot prove no effect after acquire/effect intent.
            self._ready = False
            self._blocked_reason = (
                "authoring effect outcome requires durable recovery"
            )
            raise
        except (AuthoringJournalError, HeadlessClientError) as error:
            self._ready = False
            self._blocked_reason = str(error)
            raise self._unavailable(
                getattr(error, "code", "composer_authoring_unavailable"),
                str(error),
            ) from error
        except Exception:
            self._ready = False
            self._blocked_reason = (
                "authoring effect outcome requires durable recovery"
            )
            raise


__all__ = ["AuthoringAdmissionCoordinator", "AuthoringMutationEnvelope"]
