"""Short-lived Kernel admission for Composer's in-memory workspace."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import threading
from typing import TYPE_CHECKING, Any, Callable, Mapping
import uuid

from fastapi import HTTPException
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
import rfc8785

from .authoring_identity import canonical_digest

if TYPE_CHECKING:
    from .authoring_workspace import (
        AuthoringWorkspace,
        AuthoringWorkspaceIdentity,
        AuthoringWorkspaceSnapshot,
    )

_LOCAL_MUTATION_SESSIONS = frozenset(
    {"program", "deployment", "project", "whole-workspace"}
)
_TRANSACTION_DOMAIN = b"manyforge.composer.local-authoring.v1\0"
_CONTENT_IDENTITY_DOMAIN = b"manyforge.content-identity.v1"
_OWNER_ROLE_ORDER = (
    "program_runner",
    "authoring_owner",
    "manager",
    "component_lifecycle_owner",
    "authored_scene_owner",
    "manual_operation_owner",
    "intervention_or_recovery_owner",
)
PARTIAL_EFFECT_OBSERVATION_CODES = frozenset(
    {"manager_apply_submission_uncertain"}
)


class AuthoringOperationError(RuntimeError):
    """Typed failure at a bounded authoring-operation boundary."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def headless_client_http_exception(error: HeadlessClientError) -> HTTPException:
    return HTTPException(
        status_code=error.status_code or 503,
        detail={"code": error.code, "message": str(error)},
    )


def owner_transaction_id(request_owner_epoch: str, request_id: str) -> str:
    digest = hashlib.sha256(
        _TRANSACTION_DOMAIN
        + request_owner_epoch.encode("utf-8")
        + b"\0"
        + request_id.encode("utf-8")
    ).hexdigest()
    return f"composer-local-authoring.{digest}"


@dataclass(frozen=True, slots=True)
class AuthoringMutationEnvelope:
    expected_admission_generation: int
    request_owner_epoch: str
    request_id: str


@dataclass(frozen=True, slots=True)
class LocalAuthoringCommit:
    snapshot: "AuthoringWorkspaceSnapshot"
    admission_generation: int


@dataclass(slots=True)
class _PendingLocalClaim:
    acquire_request: dict[str, Any]
    owner_transaction_id: str
    operation_kind: str
    payload_digest: str
    claim: dict[str, Any] | None = None
    settlement_kind: str = "proven_no_effect"
    publish_wake: Callable[[], None] | None = None


class AuthoringAdmissionCoordinator:
    """One process-lifetime local claim owner with no disk replay ledger."""

    def __init__(self, client: HeadlessHttpClient) -> None:
        self._client = client
        self.owner_epoch = f"composer-authoring-{uuid.uuid4().hex}"
        self.receiver_epoch = f"composer-snapshot-{uuid.uuid4().hex}"
        self._ready = True
        self._blocked_reason: str | None = None
        self._local_claim_lock = threading.Lock()
        self._local_mutation_lock = threading.Lock()
        self._pending_local_claim: _PendingLocalClaim | None = None

    @property
    def mutation_ready(self) -> bool:
        return self._ready

    @property
    def blocked_reason(self) -> str | None:
        return self._blocked_reason

    @property
    def local_claim_pending(self) -> bool:
        """Expose whether this process still owns an unsettled local claim."""

        with self._local_claim_lock:
            return self._pending_local_claim is not None

    def _block(self, reason: str) -> None:
        self._ready = False
        self._blocked_reason = reason

    @staticmethod
    def _unavailable(code: str, message: str) -> HTTPException:
        return HTTPException(
            status_code=503, detail={"code": code, "message": message}
        )

    def _require_mutation_ready(self) -> None:
        if not self._ready:
            raise self._unavailable(
                "composer_authoring_recovery_blocked",
                self._blocked_reason or "authoring is contained",
            )

    def recover(self) -> None:
        """Settle this process' claim, then replace an obsolete owner epoch."""

        self._settle_pending_local_claim()
        self._reconcile_owner_epoch()
        self._ready = True
        self._blocked_reason = None

    def _reconcile_owner_epoch(self) -> None:
        """Rotate only the short-lived authoring owner from an exact inventory."""

        probe_id = "composer-authoring-owner-inventory"
        probe_digest = canonical_digest(
            {"owner_epoch": self.owner_epoch, "probe": probe_id}
        )
        intent = {
            "claim_kind": "authoring_mutation",
            "owner_role": "authoring_owner",
            "owner_epoch": self.owner_epoch,
            "owner_transaction_id": probe_id,
            "request_id": probe_id,
            "request_payload_digest": probe_digest,
            "expected_admission_generation": 1,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
        try:
            response = self._client.recover_authoring_transaction(
                {
                    "intent": intent,
                    "expected_claim": None,
                    "expected_release": None,
                }
            )
        except HeadlessClientError as error:
            self._block(f"authoring owner inventory is unavailable: {error}")
            raise self._unavailable(error.code, str(error)) from error
        receipt = response.get("recovery_receipt")
        if not isinstance(receipt, Mapping):
            self._block("authoring owner inventory is malformed")
            raise self._unavailable(
                "composer_authoring_recovery_invalid",
                "Headless omitted the authoring owner inventory",
            )
        kernel_epoch = receipt.get("kernel_owner_epoch")
        held_claims = receipt.get("held_claims")
        owners = receipt.get("current_owner_epochs")
        if (
            not isinstance(kernel_epoch, str)
            or not kernel_epoch
            or not isinstance(held_claims, list)
            or not isinstance(owners, list)
        ):
            self._block("authoring owner inventory is malformed")
            raise self._unavailable(
                "composer_authoring_recovery_invalid",
                "Headless returned a malformed authoring owner inventory",
            )
        current_authoring = [
            owner
            for owner in owners
            if isinstance(owner, Mapping)
            and owner.get("owner_role") == "authoring_owner"
        ]
        if len(current_authoring) == 1 and current_authoring[0].get(
            "owner_epoch"
        ) == self.owner_epoch:
            return
        if len(current_authoring) > 1:
            self._block("authoring owner inventory is ambiguous")
            raise self._unavailable(
                "composer_authoring_recovery_invalid",
                "Headless returned more than one authoring owner",
            )
        next_owners = [dict(owner) for owner in owners if isinstance(owner, Mapping)]
        if len(next_owners) != len(owners):
            self._block("authoring owner inventory is malformed")
            raise self._unavailable(
                "composer_authoring_recovery_invalid",
                "Headless returned a malformed owner row",
            )
        if current_authoring:
            for owner in next_owners:
                if owner.get("owner_role") == "authoring_owner":
                    owner["owner_epoch"] = self.owner_epoch
        else:
            next_owners.append(
                {"owner_role": "authoring_owner", "owner_epoch": self.owner_epoch}
            )
        order = {role: index for index, role in enumerate(_OWNER_ROLE_ORDER)}
        try:
            next_owners.sort(key=lambda owner: order[owner["owner_role"]])
        except (KeyError, TypeError) as error:
            self._block("authoring owner inventory names an unknown role")
            raise self._unavailable(
                "composer_authoring_recovery_invalid",
                "Headless returned an unknown admission owner role",
            ) from error
        evidence_payload = rfc8785.dumps(
            {"owner_epoch": self.owner_epoch, "owner_role": "authoring_owner"}
        )
        evidence_preimage = b"\0".join(
            (
                _CONTENT_IDENTITY_DOMAIN,
                b"cell-admission-owner-epoch-reconciliation-evidence",
                b"1.0",
                evidence_payload,
            )
        )
        evidence_digest = "sha256:" + hashlib.sha256(evidence_preimage).hexdigest()
        try:
            self._client.reconcile_authoring_owner(
                {
                    "kernel_owner_epoch": kernel_epoch,
                    "retained_claims": held_claims,
                    "current_owner_epochs": next_owners,
                    "reconciliation_evidence_digest": evidence_digest,
                }
            )
        except HeadlessClientError as error:
            self._block(f"authoring owner reconciliation failed: {error}")
            raise self._unavailable(error.code, str(error)) from error

    def execute_local_mutation(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        workspace: "AuthoringWorkspace",
        install: Callable[[], bool] | None = None,
        validate_under_claim: Callable[[], None] | None = None,
        session: str = "program",
        expected_identity: "AuthoringWorkspaceIdentity | None" = None,
        project: Mapping[str, Any] | None = None,
    ) -> LocalAuthoringCommit:
        if session not in _LOCAL_MUTATION_SESSIONS:
            raise ValueError("local mutation session is not supported")
        if session == "project":
            if not isinstance(project, Mapping):
                raise TypeError("a project commit requires prepared project facts")
        elif not callable(install):
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
                validate_under_claim=validate_under_claim,
                session=session,
                expected_identity=expected_identity,
                project=project,
            )

    def execute_external_submission(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        validate_under_claim: Callable[[dict[str, Any]], None],
        submit: Callable[[dict[str, Any]], Any],
        commit_before_submit: Callable[[], None] | None = None,
        after_release: Callable[[], None] | None = None,
    ) -> Any:
        """Fence one immutable submission; Manager owns it after acceptance."""

        with self._local_mutation_lock:
            self._settle_pending_local_claim()
            self._require_mutation_ready()
            payload_digest = canonical_digest(
                {
                    "operation_kind": operation_kind,
                    "draft_id": draft_id,
                    "draft_revision": draft_revision,
                    "payload": dict(payload),
                }
            )
            claim = self._acquire_local_claim(
                transaction_id=owner_transaction_id(
                    envelope.request_owner_epoch, envelope.request_id
                ),
                payload_digest=payload_digest,
                expected_admission_generation=envelope.expected_admission_generation,
                operation_kind=operation_kind,
            )
            try:
                validate_under_claim(claim)
                if commit_before_submit is not None:
                    commit_before_submit()
            except Exception:
                self._settle_pending_local_claim("proven_no_effect")
                raise
            try:
                result = submit(claim)
            except Exception as error:
                if getattr(error, "code", None) in PARTIAL_EFFECT_OBSERVATION_CODES:
                    self._block("accepted Manager operation requires observation")
                else:
                    self._settle_pending_local_claim("proven_no_effect")
                raise
            # An accepted Manager operation adopts this exact claim as its
            # parent admission.  Releasing it here would race the asynchronous
            # progression lane and reopen authoring during maintenance.  The
            # Manager releases the claim from durable terminal evidence.
            transferred = (
                isinstance(result, Mapping)
                and result.get("status") == "accepted"
                and isinstance(result.get("apply_id"), str)
            )
            if transferred:
                self._forget_transferred_local_claim(claim)
            else:
                self._settle_pending_local_claim("owner_commit")
            if after_release is not None:
                after_release()
            return result

    def _forget_transferred_local_claim(self, claim: Mapping[str, Any]) -> None:
        """Drop local tracking after Manager durably adopts the exact claim."""

        with self._local_claim_lock:
            pending = self._pending_local_claim
            if pending is None or pending.claim != dict(claim):
                self._block("accepted Manager operation did not match its local claim")
                raise self._unavailable(
                    "composer_authoring_claim_transfer_invalid",
                    "the accepted Manager operation did not adopt the exact claim",
                )
            self._pending_local_claim = None

    def _execute_local_mutation_locked(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        draft_id: str,
        draft_revision: int,
        payload: Mapping[str, Any],
        workspace: "AuthoringWorkspace",
        install: Callable[[], bool] | None,
        validate_under_claim: Callable[[], None] | None,
        session: str,
        expected_identity: "AuthoringWorkspaceIdentity | None",
        project: Mapping[str, Any] | None,
    ) -> LocalAuthoringCommit:
        if workspace.failed_closed is not None:
            raise self._unavailable(
                "composer_workspace_failed_closed",
                f"the authoring workspace is contained: {workspace.failed_closed}",
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
                detail={"code": "authoring_not_ready", "message": "editing requires Idle"},
            )
        if admission.get("admission_generation") != envelope.expected_admission_generation:
            raise HTTPException(
                status_code=409,
                detail={"code": "stale_generation", "message": "admission changed"},
            )
        revision_before = workspace.revision
        self._acquire_local_claim(
            transaction_id=owner_transaction_id(
                envelope.request_owner_epoch, envelope.request_id
            ),
            payload_digest=payload_digest,
            expected_admission_generation=envelope.expected_admission_generation,
            operation_kind=operation_kind,
        )
        try:
            self._require_mutation_ready()
            if validate_under_claim is not None:
                validate_under_claim()
        except Exception:
            self._settle_pending_local_claim("proven_no_effect")
            raise
        installed: list[bool] = []

        def tracked_install() -> bool:
            assert install is not None
            changed = install()
            installed.append(changed)
            return changed

        with self._local_claim_lock:
            assert self._pending_local_claim is not None
            self._pending_local_claim.settlement_kind = "owner_commit"
        try:
            snapshot = self._commit_local(
                session=session,
                workspace=workspace,
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=(tracked_install if install is not None else None),
                project=project,
            )
        except Exception as error:
            if not installed and workspace.failed_closed is None:
                self._settle_pending_local_claim("proven_no_effect")
                raise
            self._block(f"local install outcome is unknown: {error}")
            raise
        if workspace.revision != revision_before:
            with self._local_claim_lock:
                assert self._pending_local_claim is not None
                self._pending_local_claim.publish_wake = workspace.publish_commit_wake
        settled = self._settle_pending_local_claim()
        return LocalAuthoringCommit(
            snapshot=snapshot,
            admission_generation=self._settled_local_generation(settled),
        )

    @staticmethod
    def _commit_local(
        *,
        session: str,
        workspace: "AuthoringWorkspace",
        draft_id: str,
        expected_identity: "AuthoringWorkspaceIdentity | None",
        install: Callable[[], bool] | None,
        project: Mapping[str, Any] | None,
    ) -> "AuthoringWorkspaceSnapshot":
        if session == "whole-workspace":
            assert install is not None
            return workspace.replace(
                expected_identity=expected_identity,
                install=install,
                project=project,
                notify=False,
            )
        if session == "project":
            assert project is not None
            return workspace.commit_project(
                expected_identity=expected_identity, facts=project, notify=False
            )
        if session == "deployment":
            assert install is not None
            return workspace.commit_deployment(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
                notify=False,
            )
        assert install is not None
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
        request = {
            "request_id": transaction_id,
            "request_owner_epoch": self.owner_epoch,
            "source_label": "manyforge_composer",
            "correlation_id": None,
            "expected_admission_generation": expected_admission_generation,
            "request_payload_digest": payload_digest,
            "owner_transaction_id": transaction_id,
        }
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
            if error.code in {"headless_http_timeout", "headless_transport_error"}:
                self._block(f"local authoring acquire is uncertain: {error}")
            else:
                with self._local_claim_lock:
                    self._pending_local_claim = None
            raise self._unavailable(error.code, str(error)) from error
        claim = response.get("claim")
        if not isinstance(claim, dict):
            self._block("local authoring acquire omitted its claim")
            raise self._unavailable(
                "composer_authoring_claim_invalid", "Headless omitted the claim"
            )
        with self._local_claim_lock:
            assert self._pending_local_claim is not None
            self._pending_local_claim.claim = claim
        return claim

    @staticmethod
    def _release_evidence(pending: _PendingLocalClaim) -> str:
        return canonical_digest(
            {
                "owner_transaction_id": pending.owner_transaction_id,
                "operation_kind": pending.operation_kind,
                "payload_digest": pending.payload_digest,
                "settlement_kind": pending.settlement_kind,
            }
        )

    def _settle_pending_local_claim(
        self, settlement_kind: str | None = None
    ) -> dict[str, Any] | None:
        with self._local_claim_lock:
            pending = self._pending_local_claim
            if pending is not None and settlement_kind is not None:
                pending.settlement_kind = settlement_kind
        if pending is None:
            return None
        if pending.claim is None:
            try:
                replay = self._client.acquire_authoring_transaction(
                    pending.acquire_request
                )
            except Exception as error:
                self._block(f"local authoring acquire is unresolved: {error}")
                raise self._unavailable(
                    "composer_authoring_claim_uncertain",
                    "the exact local claim remains unresolved",
                ) from error
            claim = replay.get("claim")
            if not isinstance(claim, dict):
                raise self._unavailable(
                    "composer_authoring_claim_invalid", "Headless omitted the claim"
                )
            pending.claim = claim
        try:
            response = self._client.settle_authoring_transaction(
                {
                    "claim": pending.claim,
                    "settlement_kind": pending.settlement_kind,
                    "evidence_digest": self._release_evidence(pending),
                }
            )
        except Exception as error:
            self._block(f"local authoring settlement is uncertain: {error}")
            raise self._unavailable(
                "composer_authoring_release_uncertain",
                "the exact local claim remains unsettled",
            ) from error
        with self._local_claim_lock:
            self._pending_local_claim = None
        if pending.publish_wake is not None:
            pending.publish_wake()
        return response

    def _settled_local_generation(self, settlement: Mapping[str, Any] | None) -> int:
        admission = settlement.get("admission") if isinstance(settlement, Mapping) else None
        generation = (
            admission.get("admission_generation")
            if isinstance(admission, Mapping)
            else settlement.get("admission_generation")
            if isinstance(settlement, Mapping)
            else None
        )
        if type(generation) is int and 1 <= generation <= 9_007_199_254_740_991:
            return generation
        self._block("local settlement omitted admission generation")
        raise self._unavailable(
            "composer_authoring_outcome_invalid",
            "the settled local claim omitted its admission generation",
        )


__all__ = [
    "AuthoringAdmissionCoordinator",
    "AuthoringOperationError",
    "AuthoringMutationEnvelope",
    "LocalAuthoringCommit",
    "PARTIAL_EFFECT_OBSERVATION_CODES",
    "headless_client_http_exception",
]
