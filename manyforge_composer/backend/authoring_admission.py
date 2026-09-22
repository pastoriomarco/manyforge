"""Durable Composer authoring transaction owner."""

from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import math
from pathlib import Path
import threading
from typing import TYPE_CHECKING, Any, Callable, Mapping, TypeVar
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

if TYPE_CHECKING:
    from .authoring_workspace import (
        AuthoringWorkspace,
        AuthoringWorkspaceIdentity,
        AuthoringWorkspaceSnapshot,
    )


_LOCAL_MUTATION_SESSIONS = frozenset({"program", "deployment", "whole-workspace"})

_T = TypeVar("_T")
_LOGGER = logging.getLogger(__name__)

# These failures occur after an external Apply may already be accepted. Keep
# their owner-issued type through the authoring transaction so the HTTP
# boundary can report an observation timeout without pretending no effect or
# degrading it to generic service unavailability.
PARTIAL_EFFECT_OBSERVATION_CODES = frozenset(
    {
        "manager_apply_observation_timeout",
        "manager_apply_observation_interrupted",
        "deployment_candidate_selection_timeout",
    }
)
PARTIAL_EFFECT_OBSERVATION_TIMEOUT_CODES = frozenset(
    {
        "manager_apply_observation_timeout",
        "deployment_candidate_selection_timeout",
    }
)


def headless_client_http_exception(error: HeadlessClientError) -> HTTPException:
    """Project one parsed Headless HTTP failure without changing its meaning."""

    return HTTPException(
        status_code=error.status_code or 503,
        detail={"code": error.code, "message": str(error)},
    )


def _headless_proves_no_effect(error: HeadlessClientError) -> bool:
    """Accept no-effect proof only from one parsed authoritative response."""

    return (
        error.status_code is not None
        and error.response is not None
        and error.details.get("effect_disposition") == "proven_no_effect"
    )


@dataclass(frozen=True, slots=True)
class AuthoringMutationEnvelope:
    """One mutation fenced by backend admission and durable request identity."""

    expected_admission_generation: int
    request_owner_epoch: str
    request_id: str


@dataclass(slots=True)
class _PendingLocalClaim:
    """One bounded in-memory local claim, including an uncertain Acquire."""

    acquire_request: dict[str, Any]
    owner_transaction_id: str
    operation_kind: str
    payload_digest: str
    claim: dict[str, Any] | None = None
    settlement_kind: str = "proven_no_effect"
    publish_wake: Callable[[], None] | None = None


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
        self._ready = False
        self._blocked_reason: str | None = "authoring recovery has not run"
        # At most one unresolved local claim is retained, in memory only. The
        # composite workspace revision makes a lost-response retry reject stale,
        # so no local request ledger is needed. One lock serializes the local
        # sequence so a single pending slot can never be overwritten; its order
        # is always local-mutation lock, then workspace lock, then session lock.
        self._local_claim_lock = threading.Lock()
        self._local_mutation_lock = threading.Lock()
        self._pending_local_claim: _PendingLocalClaim | None = None

    @property
    def mutation_ready(self) -> bool:
        return self._ready

    @property
    def local_claim_pending(self) -> bool:
        """Report whether one unresolved local claim is still retained."""

        with self._local_claim_lock:
            return self._pending_local_claim is not None

    def _block(self, reason: str) -> None:
        """Contain authoring after an unknown local-mutation outcome."""

        self._ready = False
        self._blocked_reason = reason

    @staticmethod
    def _unavailable(code: str, message: str) -> HTTPException:
        return HTTPException(status_code=503, detail={"code": code, "message": message})

    def recover(self) -> None:
        """Resolve predecessor rows, then rotate the exact Kernel owner role."""

        # Recovery and local mutation both operate on the single bounded local
        # claim slot. Serialize the complete recovery pass with local edits so
        # neither can settle or replace evidence owned by the other.
        with self._local_mutation_lock:
            self._recover_locked()

    def _recover_locked(self) -> None:
        """Run recovery while the local-mutation serialization lock is held."""

        try:
            # Settle one retained local claim before the durable predecessor
            # sweep: an unreleased claim of our own epoch is not proof that
            # authoring is available, and that sweep cannot see an unjournaled
            # local claim.
            try:
                self._settle_pending_local_claim()
            except HTTPException as error:
                detail = error.detail
                raise AuthoringJournalError(
                    "composer_authoring_release_uncertain",
                    (
                        detail.get("message")
                        if isinstance(detail, dict)
                        else "the retained local authoring claim is unsettled"
                    ),
                ) from error

            receipt: dict[str, Any] | None = None
            restored_post_image: dict[str, Any] | None = None
            for transaction in self._journal.unresolved():
                had_outcome = transaction.outcome is not None
                response = self._client.recover_authoring_transaction(
                    {
                        "intent": transaction.claim_intent,
                        "expected_claim": transaction.claim,
                        "expected_release": self._kernel_release(transaction),
                    }
                )
                receipt = response.get("recovery_receipt")
                if not isinstance(receipt, dict):
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_invalid",
                        "Headless omitted the exact Kernel recovery receipt",
                    )
                self._recover_transaction(transaction, receipt)
                recovered = self._journal.lookup(
                    transaction.request_owner_epoch, transaction.request_id
                )
                if (
                    not had_outcome
                    and recovered is not None
                    and recovered.post_image is not None
                ):
                    # _recover_transaction restored this newly produced image.
                    restored_post_image = dict(recovered.post_image)

            # An unresolved external effect owns the next post-image. Settle
            # it from its immutable intent before restoring mutable workspace
            # state; the predecessor image may legitimately name the runtime
            # that the accepted Apply just replaced.
            retained = self._journal.retained_workspace()
            if retained is not None and retained != restored_post_image:
                if self._restore_post_image is None:
                    raise AuthoringJournalError(
                        "composer_authoring_recovery_invalid",
                        "retained Composer workspace has no restore owner",
                    )
                self._restore_post_image(retained)

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
            detail = str(error)
            if detail != self._blocked_reason:
                _LOGGER.warning("Composer authoring recovery pending: %s", detail)
            self._blocked_reason = detail
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
        if transaction.outcome is not None:
            self._validate_transaction_outcome(transaction)
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
                {
                    "disposition": "proven_not_acquired",
                    "admission_generation": receipt.get("admission_generation"),
                },
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
                {
                    "disposition": status,
                    "recovered": True,
                    "admission_generation": receipt.get("admission_generation"),
                },
            )
            return

        settlement = (
            str(transaction.outcome["settlement_kind"])
            if transaction.outcome is not None
            else "owner_commit"
        )
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
                try:
                    recovered_effect = self._recover_effect(
                        transaction, transaction.claim
                    )
                except HeadlessClientError as error:
                    if not _headless_proves_no_effect(error):
                        raise
                    settlement = "proven_no_effect"
                    self._journal.record(
                        transaction,
                        "outcome_json",
                        self._stored_http_exception(
                            headless_client_http_exception(error)
                        ),
                    )
                    transaction = self._journal.lookup(
                        transaction.request_owner_epoch,
                        transaction.request_id,
                    )
                    assert transaction is not None
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
            elif settlement != "proven_no_effect":
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
    def _kernel_release(
        transaction: AuthoringTransaction,
    ) -> dict[str, Any] | None:
        """Rebuild the exact Kernel release from durable owner evidence."""

        claim = transaction.claim
        settlement = transaction.release_intent
        intent = claim.get("intent") if isinstance(claim, dict) else None
        if settlement is None:
            return None
        if not isinstance(intent, dict) or not isinstance(settlement, dict):
            raise AuthoringJournalError(
                "composer_authoring_release_invalid",
                "durable authoring release lacks its exact claim evidence",
            )
        try:
            return {
                "owner_role": intent["owner_role"],
                "owner_epoch": intent["owner_epoch"],
                "owner_transaction_id": intent["owner_transaction_id"],
                "request_id": intent["request_id"],
                "request_payload_digest": intent["request_payload_digest"],
                "acquired_admission_generation": claim[
                    "acquired_admission_generation"
                ],
                "claim_generation": claim["claim_generation"],
                "settlement": dict(settlement),
            }
        except KeyError as error:
            raise AuthoringJournalError(
                "composer_authoring_release_invalid",
                "durable authoring release lacks its exact claim evidence",
            ) from error

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
            return receipt.get(
                "matched_release"
            ) == AuthoringAdmissionCoordinator._kernel_release(transaction)
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
            "settlement_kind": "proven_no_effect",
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

    def _require_mutation_ready(self) -> None:
        if not self._ready:
            raise self._unavailable(
                "composer_authoring_recovery_blocked",
                self._blocked_reason or "authoring recovery is blocked",
            )

    def completed_admission_generation(
        self,
        envelope: AuthoringMutationEnvelope,
    ) -> int:
        """Return the exact post-settlement generation for a completed request."""

        transaction = self._journal.lookup(
            envelope.request_owner_epoch, envelope.request_id
        )
        receipt = (
            transaction.release_receipt
            if transaction is not None and transaction.complete
            else None
        )
        admission = receipt.get("admission") if isinstance(receipt, dict) else None
        generation = (
            admission.get("admission_generation")
            if isinstance(admission, dict)
            else receipt.get("admission_generation")
            if isinstance(receipt, dict)
            else None
        )
        if type(generation) is int and 1 <= generation <= 9_007_199_254_740_991:
            return generation
        self._ready = False
        self._blocked_reason = (
            "completed authoring transaction omitted its admission generation"
        )
        raise self._unavailable(
            "composer_authoring_outcome_invalid",
            self._blocked_reason,
        )

    def prepared_authoring_transaction(
        self, request_owner_epoch: str, request_id: str
    ) -> AuthoringTransaction | None:
        """Return one durable preparation record for confirmation checks.

        This is the only typed exposure of retained preparation evidence;
        owners validate it themselves and no route reads the journal SQL.
        """

        return self._journal.lookup(request_owner_epoch, request_id)

    def replay_or_recover_exact(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
    ) -> tuple[bool, Any]:
        """Settle one known request before consulting mutable current state.

        This is the existing journal recovery path exposed at the HTTP entry
        boundary. It never creates an operation: an unknown request returns
        ``(False, None)`` and follows ordinary validation. A known request is
        identity-checked, observed/reconciled exactly, and replayed.
        """

        transaction = self._journal.lookup(
            envelope.request_owner_epoch, envelope.request_id
        )
        if transaction is None:
            return False, None
        payload_digest = canonical_digest(payload)
        if (
            transaction.payload_digest != payload_digest
            or transaction.operation_kind != operation_kind
            or transaction.draft_id != draft_id
            or transaction.draft_revision != draft_revision
            or transaction.admission_generation
            != envelope.expected_admission_generation
        ):
            raise self._unavailable(
                "composer_authoring_request_id_conflict",
                "authoring request ID is bound to another operation",
            )

        if not transaction.complete:
            response = self._client.recover_authoring_transaction(
                {
                    "intent": transaction.claim_intent,
                    "expected_claim": transaction.claim,
                    "expected_release": self._kernel_release(transaction),
                }
            )
            receipt = response.get("recovery_receipt")
            if not isinstance(receipt, dict):
                raise AuthoringJournalError(
                    "composer_authoring_recovery_invalid",
                    "Headless omitted the exact Kernel recovery receipt",
                )
            self._recover_transaction(transaction, receipt)
            transaction = self._journal.lookup(
                envelope.request_owner_epoch, envelope.request_id
            )
            if transaction is None or not transaction.complete:
                raise AuthoringJournalError(
                    "composer_authoring_recovery_blocked",
                    "the exact authoring request remains unresolved",
                )
            if not self._journal.unresolved():
                self._blocked_reason = None
                self._ready = True

        self._validate_transaction_outcome(transaction)
        assert transaction.outcome is not None
        return True, self._replay(transaction.outcome)

    @staticmethod
    def _run_post_release_followup(
        callback: Callable[[], None],
        *,
        operation_kind: str,
        envelope: AuthoringMutationEnvelope,
    ) -> None:
        """Run a post-commit refresh without undoing a durable mutation."""

        try:
            callback()
        except (AuthoringJournalError, HeadlessClientError) as error:
            _LOGGER.warning(
                "authoring_post_release_followup_pending",
                extra={
                    "operation_kind": operation_kind,
                    "request_owner_epoch": envelope.request_owner_epoch,
                    "request_id": envelope.request_id,
                    "error_code": getattr(error, "code", None),
                    "error": str(error),
                },
            )
        except Exception:
            # The owner commit and claim release are already durable.  Keep
            # that truth visible even if a non-authoring follow-up has a
            # programming error, while retaining the traceback for diagnosis.
            _LOGGER.exception(
                "authoring_post_release_followup_failed",
                extra={
                    "operation_kind": operation_kind,
                    "request_owner_epoch": envelope.request_owner_epoch,
                    "request_id": envelope.request_id,
                },
            )

    @staticmethod
    def _stored_result(result: Any) -> dict[str, Any]:
        if isinstance(result, JSONResponse):
            stored = {
                "kind": "json_response",
                "settlement_kind": "owner_commit",
                "status_code": result.status_code,
                "body": json.loads(result.body),
            }
        elif isinstance(result, dict):
            stored = {
                "kind": "mapping",
                "settlement_kind": "owner_commit",
                "body": result,
            }
        else:
            raise AuthoringJournalError(
                "composer_authoring_result_invalid",
                "authoring owner result is not replayable JSON",
            )
        AuthoringAdmissionCoordinator._validate_outcome(stored)
        return stored

    @staticmethod
    def _stored_http_exception(error: HTTPException) -> dict[str, Any]:
        stored = {
            "kind": "http_exception",
            "settlement_kind": "proven_no_effect",
            "status_code": error.status_code,
            "detail": error.detail,
            "headers": (
                None if error.headers is None else dict(error.headers)
            ),
        }
        AuthoringAdmissionCoordinator._validate_outcome(stored)
        return stored

    @staticmethod
    def _is_exact_json(value: Any) -> bool:
        if value is None or isinstance(value, (str, bool)):
            return True
        if type(value) is int:
            return True
        if type(value) is float:
            return math.isfinite(value)
        if isinstance(value, list):
            return all(
                AuthoringAdmissionCoordinator._is_exact_json(item)
                for item in value
            )
        if isinstance(value, dict):
            return all(
                isinstance(key, str)
                and AuthoringAdmissionCoordinator._is_exact_json(item)
                for key, item in value.items()
            )
        return False

    @staticmethod
    def _validate_outcome(outcome: Mapping[str, Any]) -> None:
        kind = outcome.get("kind")
        status = outcome.get("status_code")
        settlement = outcome.get("settlement_kind")
        common_valid = settlement in {"owner_commit", "proven_no_effect"}
        if kind == "mapping":
            valid = (
                set(outcome) == {"kind", "settlement_kind", "body"}
                and settlement == "owner_commit"
                and isinstance(outcome.get("body"), dict)
            )
        elif kind == "json_response":
            valid = (
                set(outcome)
                == {"kind", "settlement_kind", "status_code", "body"}
                and common_valid
                and type(status) is int
                and 100 <= status <= 599
            )
        elif kind == "http_exception":
            headers = outcome.get("headers")
            valid = (
                set(outcome)
                == {
                    "kind",
                    "settlement_kind",
                    "status_code",
                    "detail",
                    "headers",
                }
                and settlement == "proven_no_effect"
                and type(status) is int
                and 100 <= status <= 599
                and (
                    headers is None
                    or (
                        isinstance(headers, dict)
                        and all(
                            isinstance(key, str) and isinstance(value, str)
                            for key, value in headers.items()
                        )
                    )
                )
            )
        else:
            valid = False
        if not valid or not AuthoringAdmissionCoordinator._is_exact_json(
            dict(outcome)
        ):
            raise AuthoringJournalError(
                "composer_authoring_outcome_invalid",
                "durable authoring outcome is not one exact supported HTTP result",
            )

    @staticmethod
    def _validate_transaction_outcome(
        transaction: AuthoringTransaction,
    ) -> None:
        if transaction.outcome is None:
            raise AuthoringJournalError(
                "composer_authoring_outcome_invalid",
                "authoring transaction has no durable outcome",
            )
        AuthoringAdmissionCoordinator._validate_outcome(transaction.outcome)
        committed = transaction.outcome["settlement_kind"] == "owner_commit"
        if committed != (transaction.post_image is not None):
            raise AuthoringJournalError(
                "composer_authoring_outcome_invalid",
                "durable authoring outcome and post-image disagree",
            )

    @staticmethod
    def _replay(outcome: Mapping[str, Any]) -> Any:
        AuthoringAdmissionCoordinator._validate_outcome(outcome)
        kind = outcome["kind"]
        if kind == "http_exception":
            headers = outcome.get("headers")
            raise HTTPException(
                status_code=int(outcome["status_code"]),
                detail=outcome.get("detail"),
                headers=(dict(headers) if isinstance(headers, Mapping) else None),
            )
        body = outcome.get("body")
        if kind == "json_response":
            return JSONResponse(status_code=int(outcome["status_code"]), content=body)
        return body

    @staticmethod
    def _release(
        transaction: AuthoringTransaction, settlement_kind: str
    ) -> dict[str, Any]:
        AuthoringAdmissionCoordinator._validate_transaction_outcome(transaction)
        assert transaction.outcome is not None
        if transaction.outcome["settlement_kind"] != settlement_kind:
            raise AuthoringJournalError(
                "composer_authoring_outcome_invalid",
                "authoring settlement contradicts its durable outcome",
            )
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
        validate_under_claim: (
            Callable[[dict[str, Any]], Mapping[str, Any] | None] | None
        ) = None,
        after_release: Callable[[], None] | None = None,
    ) -> _T:
        payload_digest = canonical_digest(payload)
        replay = self._journal.lookup(
            envelope.request_owner_epoch, envelope.request_id
        )
        if replay is not None:
            if (
                replay.payload_digest != payload_digest
                or replay.operation_kind != operation_kind
                or replay.draft_id != draft_id
                or replay.draft_revision != draft_revision
                or replay.admission_generation
                != envelope.expected_admission_generation
            ):
                raise self._unavailable(
                    "composer_authoring_request_id_conflict",
                    "authoring request ID is bound to another operation",
                )
            if replay.complete and replay.outcome is not None:
                try:
                    self._validate_transaction_outcome(replay)
                    result = self._replay(replay.outcome)
                except AuthoringJournalError as error:
                    self._ready = False
                    self._blocked_reason = str(error)
                    raise self._unavailable(error.code, str(error)) from error
                if (
                    replay.outcome["settlement_kind"] == "owner_commit"
                    and after_release is not None
                ):
                    self._run_post_release_followup(
                        after_release,
                        operation_kind=operation_kind,
                        envelope=envelope,
                    )
                return result
            raise self._unavailable(
                "composer_authoring_recovery_blocked",
                "the exact authoring request has an unresolved durable outcome",
            )

        self._require_mutation_ready()

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
            # The browser is presentation only. Recheck backend-owned
            # readiness after acquiring the exact durable claim; Draft and
            # Scenario identities are validated by the operation under it.
            self._require_mutation_ready()
            if validate_under_claim is not None:
                validated_effect = validate_under_claim(claim)
                if validated_effect is not None:
                    prepared_effect.update(validated_effect)
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
        except HTTPException as error:
            transaction = self._journal.lookup(
                envelope.request_owner_epoch, envelope.request_id
            )
            headless_error = (
                error.__cause__
                if isinstance(error.__cause__, HeadlessClientError)
                else None
            )
            if (
                transaction is not None
                and transaction.claim is not None
                and transaction.outcome is None
                and (
                    transaction.effect_intent is None
                    or (
                        headless_error is not None
                        and _headless_proves_no_effect(headless_error)
                    )
                )
            ):
                try:
                    self._journal.record(
                        transaction,
                        "outcome_json",
                        self._stored_http_exception(error),
                    )
                    transaction = self._journal.lookup(
                        envelope.request_owner_epoch, envelope.request_id
                    )
                    assert transaction is not None
                    release = self._release(transaction, "proven_no_effect")
                    self._journal.record(
                        transaction, "release_intent_json", release
                    )
                    receipt = self._client.settle_authoring_transaction(
                        {
                            "claim": transaction.claim,
                            "settlement_kind": "proven_no_effect",
                            "evidence_digest": release["evidence_digest"],
                        }
                    )
                    self._journal.record(
                        transaction, "release_receipt_json", receipt
                    )
                    raise error
                except HTTPException:
                    raise
                except (AuthoringJournalError, HeadlessClientError) as recovery_error:
                    self._ready = False
                    self._blocked_reason = str(recovery_error)
                    raise self._unavailable(
                        getattr(
                            recovery_error,
                            "code",
                            "composer_authoring_unavailable",
                        ),
                        str(recovery_error),
                    ) from recovery_error
            # An HTTP status after effect intent cannot prove no effect.
            self._ready = False
            self._blocked_reason = (
                "authoring effect outcome requires durable recovery"
            )
            raise
        except AuthoringJournalError as error:
            self._ready = False
            self._blocked_reason = str(error)
            if error.code in PARTIAL_EFFECT_OBSERVATION_CODES:
                raise
            raise self._unavailable(
                error.code,
                str(error),
            ) from error
        except HeadlessClientError as error:
            self._ready = False
            self._blocked_reason = str(error)
            raise self._unavailable(error.code, str(error)) from error
        except Exception:
            self._ready = False
            self._blocked_reason = (
                "authoring effect outcome requires durable recovery"
            )
            raise
        if after_release is not None:
            # This idempotent follow-up runs only after the owner commit and
            # claim release are durable, so a transient preparation failure
            # cannot be misclassified as an unknown claim or false Apply
            # failure.  Replays use the same Applied identity and retry it.
            self._run_post_release_followup(
                after_release,
                operation_kind=operation_kind,
                envelope=envelope,
            )
        return result

    # -- local workspace mutations (UA2D.4) -------------------------------

    def execute_local_mutation(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        workspace: "AuthoringWorkspace",
        install: Callable[[], bool],
        session: str = "program",
        expected_identity: "AuthoringWorkspaceIdentity | None" = None,
        project: Mapping[str, Any] | None = None,
    ) -> "AuthoringWorkspaceSnapshot":
        """Commit one ordinary workspace mutation under the exact Kernel claim.

        The caller has already prepared its immutable value; this method owns the
        whole protocol so no caller emulates it. The sequence is fixed:
        admission, acquire the exact Kernel authoring claim, one fenced
        workspace commit with the wake deferred, settle that claim, then at most
        one wake. Nothing is journaled and nothing is persisted.

        ``session`` selects the fenced commit: ``program`` and ``deployment`` are
        ordinary Draft commits and ``whole-workspace`` is the replacement that
        may rotate a Draft identity.

        A refusal that installed nothing settles as ``proven_no_effect`` and
        preserves the caller's typed error. An uncertain install or settlement
        retains the exact claim evidence in memory, contains authoring and stays
        closed until :meth:`recover` settles that one claim. One lock serializes
        local mutations so the single retained-claim slot can never be
        overwritten by a concurrent edit.
        """

        if session not in _LOCAL_MUTATION_SESSIONS:
            raise ValueError("local mutation session is not supported")
        if not callable(install):
            raise TypeError("local mutation install must be callable")
        with self._local_mutation_lock:
            return self._execute_local_mutation_locked(
                envelope,
                operation_kind=operation_kind,
                draft_id=draft_id,
                draft_revision=draft_revision,
                payload=payload,
                workspace=workspace,
                install=install,
                session=session,
                expected_identity=expected_identity,
                project=project,
            )

    def _execute_local_mutation_locked(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        workspace: "AuthoringWorkspace",
        install: Callable[[], bool],
        session: str,
        expected_identity: "AuthoringWorkspaceIdentity | None",
        project: Mapping[str, Any] | None,
    ) -> "AuthoringWorkspaceSnapshot":
        """Run the fixed local sequence with the single pending slot owned."""

        if workspace.failed_closed is not None:
            raise self._unavailable(
                "composer_workspace_failed_closed",
                "the Composer authoring workspace is contained: "
                f"{workspace.failed_closed}",
            )
        payload_digest = canonical_digest(
            {
                "operation_kind": operation_kind,
                "draft_id": draft_id,
                "draft_revision": draft_revision,
                "payload": dict(payload),
            }
        )
        self._settle_pending_local_claim()
        self._require_mutation_ready()
        admission = self._client.authoring_admission()
        if admission.get("authoring_allowed") is not True:
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "authoring_not_ready",
                    "message": "editing requires idle execution and healthy components",
                },
            )
        if (
            admission.get("admission_generation")
            != envelope.expected_admission_generation
        ):
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "stale_generation",
                    "message": "authoring admission generation changed",
                },
            )
        revision_before = workspace.revision
        transaction_id = owner_transaction_id(
            envelope.request_owner_epoch, envelope.request_id
        )
        claim = self._acquire_local_claim(
            transaction_id=transaction_id,
            payload_digest=payload_digest,
            expected_admission_generation=envelope.expected_admission_generation,
            operation_kind=operation_kind,
        )
        # The browser is presentation only. Recheck backend-owned readiness
        # after acquiring the exact claim, as the durable path does.
        self._require_mutation_ready()
        installed: list[bool] = []

        def tracked_install() -> bool:
            changed = install()
            installed.append(changed)
            return changed

        # Once installation begins, a failure is not proof of no effect. A
        # refusal before the installer runs changes this back to
        # ``proven_no_effect`` in the exception path below.
        with self._local_claim_lock:
            assert self._pending_local_claim is not None
            self._pending_local_claim.settlement_kind = "owner_commit"
        try:
            snapshot = self._commit_local(
                session=session,
                workspace=workspace,
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=tracked_install,
                project=project,
            )
        except Exception as error:
            if not installed and workspace.failed_closed is None:
                # The prepared install never ran and the workspace is still
                # consistent, so the claim is released truthfully as a proven
                # no-effect and the caller's typed refusal is preserved.
                self._settle_pending_local_claim("proven_no_effect")
                raise
            self._block(
                "local authoring install outcome is unknown: "
                f"{type(error).__name__}: {error}"
            )
            raise
        if workspace.revision != revision_before:
            # Retain the one deferred wake with the claim. If settlement is
            # temporarily unavailable, recover() publishes it after settling
            # this exact claim instead of losing the committed notification.
            with self._local_claim_lock:
                assert self._pending_local_claim is not None
                self._pending_local_claim.publish_wake = (
                    workspace.publish_commit_wake
                )
        self._settle_pending_local_claim()
        return snapshot

    @staticmethod
    def _commit_local(
        *,
        session: str,
        workspace: "AuthoringWorkspace",
        draft_id: str,
        expected_identity: "AuthoringWorkspaceIdentity | None",
        install: Callable[[], bool],
        project: Mapping[str, Any] | None,
    ) -> "AuthoringWorkspaceSnapshot":
        """Commit one prepared value with the workspace wake deferred."""

        if session == "whole-workspace":
            return workspace.replace(
                expected_identity=expected_identity,
                install=install,
                project=project,
                notify=False,
            )
        if session == "deployment":
            return workspace.commit_deployment(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
                notify=False,
            )
        return workspace.commit_program(
            draft_id=draft_id,
            expected_identity=expected_identity,
            install=install,
            notify=False,
        )

    def _acquire_local_claim(
        self,
        *,
        transaction_id: str,
        payload_digest: str,
        expected_admission_generation: int,
        operation_kind: str,
    ) -> dict[str, Any]:
        """Acquire the exact Kernel authoring claim for one local mutation."""

        request = {
            "request_id": transaction_id,
            "request_owner_epoch": self.owner_epoch,
            "source_label": "manyforge_composer",
            "correlation_id": None,
            "expected_admission_generation": expected_admission_generation,
            "request_payload_digest": payload_digest,
            "owner_transaction_id": transaction_id,
        }
        # Retain the request before transport. If Acquire succeeds remotely but
        # its response is lost, recover() replays this exact identity and then
        # settles it as proven-no-effect because installation never began.
        with self._local_claim_lock:
            self._pending_local_claim = _PendingLocalClaim(
                acquire_request=request,
                owner_transaction_id=transaction_id,
                operation_kind=operation_kind,
                payload_digest=payload_digest,
            )
        try:
            response = self._client.acquire_authoring_transaction(request)
        except HeadlessClientError as error:
            self._block(
                "local authoring claim acquire is uncertain: " + str(error)
            )
            raise self._unavailable(error.code, str(error)) from error
        claim = response.get("claim")
        if not isinstance(claim, dict):
            self._block("local authoring claim acquire omitted the exact claim")
            raise self._unavailable(
                "composer_authoring_claim_invalid",
                "Headless omitted the exact authoring claim",
            )
        with self._local_claim_lock:
            assert self._pending_local_claim is not None
            self._pending_local_claim.claim = claim
        return claim

    @staticmethod
    def _local_release_evidence(
        pending: _PendingLocalClaim, settlement_kind: str
    ) -> str:
        """Derive the release evidence for one retained local claim."""

        return canonical_digest(
            {
                "owner_transaction_id": pending.owner_transaction_id,
                "operation_kind": pending.operation_kind,
                "payload_digest": pending.payload_digest,
                "settlement_kind": settlement_kind,
            }
        )

    def _settle_pending_local_claim(
        self, settlement_kind: str | None = None
    ) -> dict[str, Any] | None:
        """Settle the one retained local claim, if any.

        Returns the Kernel admission projection, or ``None`` when no local claim
        is outstanding. An uncertain settlement keeps the exact claim evidence,
        contains authoring and raises a typed unavailability; it never infers a
        release from elapsed time and never rolls back a committed edit.
        """

        with self._local_claim_lock:
            pending = self._pending_local_claim
            if pending is not None and settlement_kind is not None:
                pending.settlement_kind = settlement_kind
        if pending is None:
            return None
        if pending.claim is None:
            try:
                acquired = self._client.acquire_authoring_transaction(
                    pending.acquire_request
                )
            except Exception as error:
                self._block(
                    "local authoring claim acquire is uncertain: "
                    f"{type(error).__name__}: {error}"
                )
                raise self._unavailable(
                    "composer_authoring_claim_uncertain",
                    "the exact local authoring claim is unresolved; authoring "
                    "stays closed until recovery can replay its identity",
                ) from error
            claim = acquired.get("claim")
            if not isinstance(claim, dict):
                self._block("replayed local authoring acquire omitted the claim")
                raise self._unavailable(
                    "composer_authoring_claim_invalid",
                    "Headless omitted the replayed local authoring claim",
                )
            with self._local_claim_lock:
                pending.claim = claim
        try:
            response = self._client.settle_authoring_transaction(
                {
                    "claim": pending.claim,
                    "settlement_kind": pending.settlement_kind,
                    "evidence_digest": self._local_release_evidence(
                        pending, pending.settlement_kind
                    ),
                }
            )
        except Exception as error:
            self._block(
                "local authoring claim settlement is uncertain: "
                f"{type(error).__name__}: {error}"
            )
            raise self._unavailable(
                "composer_authoring_release_uncertain",
                "the exact local authoring claim is unsettled; authoring stays "
                "closed until the existing recovery call settles it",
            ) from error
        with self._local_claim_lock:
            self._pending_local_claim = None
        if pending.publish_wake is not None:
            pending.publish_wake()
        return response


__all__ = [
    "AuthoringAdmissionCoordinator",
    "AuthoringMutationEnvelope",
    "PARTIAL_EFFECT_OBSERVATION_CODES",
    "PARTIAL_EFFECT_OBSERVATION_TIMEOUT_CODES",
    "headless_client_http_exception",
]
