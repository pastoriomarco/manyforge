"""Short-lived Kernel admission for Composer's in-memory workspace."""

from __future__ import annotations

from dataclasses import dataclass
from collections import OrderedDict
from copy import deepcopy
import threading
from typing import TYPE_CHECKING, Any, Callable, Mapping, NoReturn

from fastapi import HTTPException
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.held_run_authoring import held_run_draft_edit_allowed

from .application_identity import format_application_start_id
from .authoring_identity import canonical_digest
from .draft_session import DraftSessionError
from .scenario_artifacts import ScenarioArtifactError

from .authoring_workspace import AuthoringOperationContext

if TYPE_CHECKING:
    from .authoring_workspace import (
        AuthoringWorkspace,
        AuthoringWorkspaceIdentity,
        AuthoringWorkspaceSnapshot,
    )

_LOCAL_MUTATION_SESSIONS = frozenset(
    {"program", "deployment", "project", "whole-workspace"}
)
PARTIAL_EFFECT_OBSERVATION_CODES = frozenset(
    {"manager_apply_submission_uncertain", "manager_apply_acceptance_invalid"}
)
# Headless client codes for a reply the Composer never received.
_REPLY_LOST_CODES = frozenset(
    {"headless_http_timeout", "headless_http_unavailable", "headless_transport_error"}
)


def _answer_lost(error: Exception) -> bool:
    """Whether a claim request failed without a definite owner answer.

    A lost Composer→Headless reply, or Headless's own lost Kernel answer
    (``details.outcome_unknown``), leaves the claim's state unknown; any other
    typed refusal is the owner's definite answer.
    """

    if not isinstance(error, HeadlessClientError):
        return True
    return error.code in _REPLY_LOST_CODES or error.details.get("outcome_unknown") is True


def _release_not_exact(error: Exception) -> bool:
    """The Kernel's definite answer that the exact claim is no longer held."""

    return (
        isinstance(error, HeadlessClientError)
        and error.code == "stale_generation"
        and error.details.get("reason") == "claim_release_not_exact"
    )


class AuthoringOperationError(RuntimeError):
    """Typed failure at a bounded authoring-operation boundary."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(slots=True)
class OperationEffects:
    """The durable or externally visible effects of one operation, as they happen.

    One operation boundary owns one record and hands it to its writers and to
    the admission coordinator. A writer records a destination when it first
    changes it and again when the destination is proven to hold the intended
    document; the coordinator records workspace commits and Manager
    submissions. The same record decides both the HTTP outcome (the
    ``"effect": "none"`` marker only while nothing is recorded) and the local
    claim settlement (``proven_no_effect`` only while nothing is recorded).
    Acquiring and releasing an otherwise effect-free claim records nothing.
    """

    destination: str | None = None
    published: bool = False
    workspace: bool = False
    submission: bool = False

    def destination_changed(self, path: str) -> None:
        """A destination changed; its content is not yet proven."""

        self.destination = path

    def destination_published(self, path: str) -> None:
        """The destination holds exactly the intended document."""

        self.destination = path
        self.published = True

    def workspace_committed(self) -> None:
        """A Draft/workspace change committed, or its outcome is unknown."""

        self.workspace = True

    def submitted(self) -> None:
        """A Manager/Kernel submission was made beyond the local claim."""

        self.submission = True

    @property
    def none(self) -> bool:
        return self.destination is None and not self.workspace and not self.submission

    @property
    def published_path(self) -> str | None:
        return self.destination if self.published else None

    @property
    def settlement_kind(self) -> str:
        return "proven_no_effect" if self.none else "owner_commit"


def refused_before_effect(status_code: int, detail: Mapping[str, Any]) -> HTTPException:
    """One structured refusal of a request that left no effect.

    ``"effect": "none"`` tells the client that this request left no durable or
    externally visible effect: no committed Draft/workspace change, no file
    written or published, no Manager or Kernel submission (an effect-free local
    claim acquired and released is not an effect), so the client may release
    its submission guard. A raise site that proves this passes it explicitly;
    an operation boundary adds it from its :class:`OperationEffects`. Any other
    failure leaves the field out and its outcome stays unconfirmed.
    """

    return HTTPException(status_code=status_code, detail={**detail, "effect": "none"})


def raise_operation_failure(
    error: Exception,
    effects: OperationEffects,
    translate: Callable[[Exception], NoReturn],
) -> NoReturn:
    """Raise one failure of an effect-tracked operation as its HTTP outcome.

    ``translate`` is the route's error mapping. With no recorded effect the
    translated refusal carries the marker; after a destination changed it
    names that destination and never carries the marker. An ``HTTPException``
    raised by an owner (for example an unsettled claim) is kept unchanged.
    """

    if isinstance(error, HTTPException):
        raise error
    try:
        translate(error)
    except HTTPException as translated:
        detail = translated.detail
        if not isinstance(detail, Mapping):
            raise
        if effects.none:
            raise refused_before_effect(translated.status_code, detail) from error
        if effects.destination is not None and "path" not in detail:
            raise HTTPException(
                status_code=translated.status_code,
                detail={**detail, "path": effects.destination},
            ) from error
        raise
    raise error


def raise_save_failure(
    error: Exception,
    effects: OperationEffects,
    translate: Callable[[Exception], NoReturn],
    *,
    code: str,
    document: str,
) -> NoReturn:
    """Raise one Save-like failure as Not saved, Saved-not-adopted or unverified.

    Once the destination is proven to hold the intended document, a later
    readback or workspace failure is the definite ``code`` outcome: it names
    the written path, says the document was not adopted as the Saved
    baseline, and never carries the marker.
    """

    path = effects.published_path
    if path is None or isinstance(error, HTTPException):
        raise_operation_failure(error, effects, translate)
    cause_detail = getattr(error, "detail", None)
    current = (
        cause_detail.get("workspace") if isinstance(cause_detail, Mapping) else None
    )
    cause_code = getattr(error, "code", None)
    detail: dict[str, Any] = {
        "code": code,
        "message": (
            f"The {document} was written to {path}, but a newer workspace "
            "revision was not adopted as Saved"
            if cause_code == "composer_workspace_stale"
            else f"The {document} was written to {path}, but it was not "
            f"adopted as the Saved baseline: {error}"
        ),
        "path": path,
        "causeCode": cause_code if isinstance(cause_code, str) else type(error).__name__,
    }
    if current is not None:
        detail["currentWorkspace"] = current
    raise HTTPException(status_code=409, detail=detail) from error


def draft_session_http_exception(error: DraftSessionError) -> HTTPException:
    """Every Draft-session refusal precedes the Draft's atomic state change."""

    detail: dict[str, Any] = {"code": error.code, "message": str(error)}
    if error.detail != str(error):
        detail["context"] = error.detail
    return refused_before_effect(error.status_code, detail)


def scenario_artifact_http_exception(error: ScenarioArtifactError) -> HTTPException:
    """A code cannot tell whether a write already happened: never marked here."""

    detail = {"code": error.code, "message": str(error)}
    if error.details is not None:
        detail["context"] = error.details
    return HTTPException(status_code=error.status_code, detail=detail)


def headless_client_http_exception(
    error: HeadlessClientError, *, before_effect: bool = False
) -> HTTPException:
    """``before_effect``: the caller proves this request had no effect yet."""

    detail: dict[str, Any] = {"code": error.code, "message": str(error)}
    if error.details:
        detail["details"] = error.details
    status_code = error.status_code or 503
    if before_effect:
        return refused_before_effect(status_code, detail)
    return HTTPException(status_code=status_code, detail=detail)


@dataclass(frozen=True, slots=True)
class AuthoringMutationEnvelope:
    """One browser authoring request: its admission evidence, `A` and key.

    ``application_start_id`` is the browser's `A` (forwarded unchanged to
    Headless, which alone checks it); ``request_key`` names the browser's
    request in the submission-outcome readback.
    """

    expected_admission_generation: int
    application_start_id: int
    request_key: bytes


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
    # The release carries the key of the request that causes it (contract
    # 1.15.2), so a resent settlement reuses it.
    settle_request_key: bytes
    claim: dict[str, Any] | None = None
    settlement_kind: str = "proven_no_effect"
    publish_wake: Callable[[], None] | None = None
    # The Manager request naming this claim as its parent was sent: from then
    # on the Manager owns the claim, and Composer never releases it.
    sent: bool = False


class AuthoringAdmissionCoordinator:
    """One process-lifetime local claim owner with no disk replay ledger."""

    def __init__(self, client: HeadlessHttpClient, application_start_id: int) -> None:
        self._client = client
        # Composer's own `A`: the authoring owner is (authoring_owner, A).
        self.application_start_id = application_start_id
        self._ready = True
        self._blocked_reason: str | None = None
        self._local_claim_lock = threading.Lock()
        self._local_mutation_lock = threading.Lock()
        self._pending_local_claim: _PendingLocalClaim | None = None
        # Bounded readback of already-confirmed outcomes, not a replay ledger.
        # Missing/evicted/restarted evidence is unknown, never proven no-effect.
        self._outcome_lock = threading.Lock()
        self._outcomes: OrderedDict[AuthoringMutationEnvelope, dict[str, Any]] = OrderedDict()

    def submission_outcome(self, envelope: AuthoringMutationEnvelope) -> dict[str, Any]:
        with self._outcome_lock:
            outcome = deepcopy(self._outcomes.get(envelope))
        return {
            "schema_version": "manyforge.authoring_submission_outcome.v2",
            "application_start_id": format_application_start_id(
                envelope.application_start_id
            ),
            "request_key": envelope.request_key.hex(),
            "expected_admission_generation": envelope.expected_admission_generation,
            "outcome": outcome,
        }

    def _record_outcome(
        self, envelope: AuthoringMutationEnvelope, operation_kind: str,
        apply_identity: dict[str, Any] | None = None,
    ) -> None:
        with self._outcome_lock:
            self._outcomes[envelope] = {
                "operation_kind": operation_kind,
                "kind": "manager_accepted" if apply_identity is not None else "settled",
                "apply": apply_identity,
            }
            self._outcomes.move_to_end(envelope)
            while len(self._outcomes) > 128:
                self._outcomes.popitem(last=False)

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

    def manager_submission_sent(self, claim: Mapping[str, Any]) -> None:
        """Record that the Manager request naming ``claim`` is being sent.

        Called by the submitter immediately before the Manager request leaves
        Composer. From here on the Manager owns the claim: it releases it when
        its Apply ends, and New releases it after a failed Apply.
        """

        with self._local_claim_lock:
            pending = self._pending_local_claim
            if pending is not None and pending.claim == dict(claim):
                pending.sent = True

    def settle_before_preparation(self) -> None:
        """Settle a claim left by a lost answer before a preparation reads admission.

        An Apply, Load or New prepares its candidate before the coordinator
        runs, and that preparation waits for open admission; a claim this
        process may still hold would refuse it. A contained authoring state
        keeps its claim for diagnosis and is refused by the coordinator.
        """

        with self._local_mutation_lock:
            if self._ready:
                self._settle_pending_local_claim()

    def _forget_sent_claim(self) -> None:
        """Drop the record of a claim the Manager now owns."""

        with self._local_claim_lock:
            self._pending_local_claim = None

    def _claim_sent(self) -> bool:
        with self._local_claim_lock:
            return self._pending_local_claim is not None and self._pending_local_claim.sent

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
            raise refused_before_effect(
                503,
                {
                    "code": "composer_authoring_recovery_blocked",
                    "message": self._blocked_reason or "authoring is contained",
                },
            )

    def _require_authoring_allowed(self) -> None:
        admission = self._client.authoring_admission()
        if admission.get("authoring_allowed") is not True:
            raise refused_before_effect(
                409, {"code": "authoring_not_ready", "message": (
                    "Authoring is locked by the backend: a run, a manual operation or "
                    "an unfinished Apply holds it. Nothing changed. A failed Apply is "
                    "cleared with New; if nothing is in progress and New does not "
                    "help, restart ManyForge.")}
            )

    @staticmethod
    def _require_workspace_open(workspace: "AuthoringWorkspace") -> None:
        if workspace.failed_closed is not None:
            raise refused_before_effect(
                503,
                {
                    "code": "composer_workspace_failed_closed",
                    "message": (
                        "the authoring workspace is contained: "
                        f"{workspace.failed_closed}"
                    ),
                },
            )

    def _draft_edit_allowed(self) -> bool:
        """Idle, or the published held-run rule applied to the owners' facts.

        Kernel authoring admission (its claims and generation) is not read: a
        Draft edit has no external effect. The held-run function is the one
        that computes the control-state fact the browser reads; any read
        failure is "not editable".
        """

        try:
            cycle = self._client.program_cycle_status()
            if isinstance(cycle, Mapping) and cycle.get("state") == "idle_after_reset":
                return True
            return held_run_draft_edit_allowed(
                cycle, self._client.execution_control_state()
            )
        except Exception:
            return False

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
        prepare_project_under_claim: Callable[[], Mapping[str, Any]] | None = None,
        effects: OperationEffects | None = None,
    ) -> LocalAuthoringCommit:
        """Commit one local change under a short-lived Kernel authoring claim.

        ``effects`` is the operation's effect record. Its writers record a
        destination they change under the claim; a failure then settles the
        claim as ``owner_commit``, and only an operation that recorded nothing
        settles as ``proven_no_effect``.
        """

        if session not in _LOCAL_MUTATION_SESSIONS:
            raise ValueError("local mutation session is not supported")
        if session == "project":
            if (project is None) == (prepare_project_under_claim is None):
                raise TypeError(
                    "a project commit requires exactly one prepared project value "
                    "or under-claim project preparer"
                )
        elif session == "whole-workspace":
            if not callable(install):
                raise TypeError("local mutation install must be callable")
            if project is not None and prepare_project_under_claim is not None:
                raise TypeError("a workspace commit accepts only one project source")
        elif project is not None or prepare_project_under_claim is not None:
            raise TypeError("project facts are only valid for a project commit")
        elif not callable(install):
            raise TypeError("local mutation install must be callable")
        with self._local_mutation_lock:
            result = self._execute_local_mutation_locked(
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
                prepare_project_under_claim=prepare_project_under_claim,
                effects=OperationEffects() if effects is None else effects,
            )
            self._record_outcome(envelope, operation_kind)
            return result

    def execute_draft_edit(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        operation_kind: str,
        workspace: "AuthoringWorkspace",
        draft_id: str,
        expected_identity: "AuthoringWorkspaceIdentity",
        install: Callable[[], bool],
        session: str,
        validate: Callable[[], None] | None = None,
    ) -> LocalAuthoringCommit:
        """Commit one edit that changes only the Draft; it takes no cell claim.

        A Draft edit has no external effect, so it takes no Kernel authoring
        claim and is not fenced by the admission generation: another client's
        Apply, a manual operation or a component change never refuses it. It
        is admitted when the Program cycle is Idle or when a held Program run
        has all its motion confirmed stopped (Headless ``held_run_authoring``).
        The commit is serialized with Composer-mediated execution starts and
        external submissions by the same process-local lock, so Execute, Run,
        Resume and Retry sent through Composer observe either the old or the
        new Draft. A Run or resume sent to Headless directly does not take this
        lock (as before); it is admitted by Headless/Runner, which compare the
        caller's Draft revision with Applied, and only Applied ever runs.
        """

        if session not in {"program", "deployment", "scenario"}:
            raise ValueError("a Draft edit commits a Draft session or the whole Scenario")
        if not callable(install):
            raise TypeError("Draft edit install must be callable")
        with self._local_mutation_lock:
            self._require_workspace_open(workspace)
            self._settle_pending_local_claim()
            self._require_mutation_ready()
            if not self._draft_edit_allowed():
                raise refused_before_effect(
                    409,
                    {
                        "code": "authoring_not_ready",
                        "message": (
                            "editing the Draft requires Idle or a Program run "
                            "whose motion is confirmed stopped"
                        ),
                    },
                )
            if validate is not None:
                validate()
            context = AuthoringOperationContext(
                operation=operation_kind,
                application_start_id=envelope.application_start_id,
                request_key=envelope.request_key,
            )
            if session == "scenario":
                # Both Drafts in one whole-workspace commit; each install
                # fences its own Draft identity.
                snapshot = workspace.replace(
                    expected_identity=expected_identity,
                    install=install,
                    context=context,
                )
            else:
                commit = (
                    workspace.commit_deployment
                    if session == "deployment"
                    else workspace.commit_program
                )
                snapshot = commit(
                    draft_id=draft_id,
                    expected_identity=expected_identity,
                    install=install,
                    context=context,
                )
            self._record_outcome(envelope, operation_kind)
            return LocalAuthoringCommit(
                snapshot=snapshot,
                admission_generation=envelope.expected_admission_generation,
            )

    def submit_workspace_fenced(self, callback: Callable[[], Any]) -> Any:
        """Serialize a workspace-fenced owner submission with local authoring commits.

        The callback checks the current workspace and submits to Headless while
        the same short lock used by local mutations is held. Headless/Kernel
        still own effect admission and return its authoritative receipt.
        Stop uses its independent priority lane and never takes this lock.
        """

        with self._local_mutation_lock:
            return callback()

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
        effects: OperationEffects | None = None,
        workspace: "AuthoringWorkspace | None" = None,
    ) -> Any:
        """Fence one immutable submission; Manager owns its claim once sent.

        ``effects`` is the operation's effect record (see
        :meth:`execute_local_mutation`); a committed ``commit_before_submit``
        is recorded in it, so a later definite Manager rejection settles the
        claim as ``owner_commit``, not ``proven_no_effect``. ``workspace`` is
        the owner ``commit_before_submit`` commits to; if that commit contains
        it, the claim stays held for diagnosis.
        """

        if effects is None:
            effects = OperationEffects()
        with self._local_mutation_lock:
            self._settle_pending_local_claim()
            self._require_mutation_ready()
            self._require_authoring_allowed()
            payload_digest = canonical_digest(
                {
                    "operation_kind": operation_kind,
                    "draft_id": draft_id,
                    "draft_revision": draft_revision,
                    "payload": dict(payload),
                }
            )
            claim = self._acquire_local_claim(
                envelope=envelope,
                claim_key=envelope.request_key,
                payload_digest=payload_digest,
                expected_admission_generation=envelope.expected_admission_generation,
                operation_kind=operation_kind,
            )
            try:
                validate_under_claim(claim)
                if commit_before_submit is not None:
                    # As on the local path: from the commit on, the retained
                    # claim settles as owner_commit; a refusal proven to leave
                    # no effect still settles from its recorded effects below.
                    with self._local_claim_lock:
                        assert self._pending_local_claim is not None
                        self._pending_local_claim.settlement_kind = "owner_commit"
                    commit_before_submit()
                    effects.workspace_committed()
            except Exception as error:
                if workspace is not None and workspace.failed_closed is not None:
                    # A contained install is never a proven no-op.
                    effects.workspace_committed()
                    self._block(f"local install outcome is unknown: {error}")
                    raise
                self._settle_failed_operation(envelope, operation_kind, effects)
                raise
            try:
                result = submit(claim)
            except Exception as error:
                if (
                    getattr(error, "code", None) == "manager_apply_rejected"
                    or not self._claim_sent()
                ):
                    # A definite refusal, or a failure before the Manager
                    # request was sent: Composer still owns the claim.
                    self._settle_failed_operation(envelope, operation_kind, effects)
                else:
                    # Sent with an uncertain outcome: the Manager owns the
                    # claim (released when its Apply ends, or by New after a
                    # failed one), so Composer forgets it and never releases it.
                    effects.submitted()
                    self._forget_sent_claim()
                raise
            # An accepted Manager operation adopts this exact claim as its
            # parent admission.  Releasing it here would race the asynchronous
            # progression lane and reopen authoring during maintenance.  The
            # Manager releases the claim from durable terminal evidence.
            if (
                isinstance(result, Mapping)
                and result.get("status") == "accepted"
                and type(result.get("apply_id")) is int
                and result["apply_id"] > 0
                and type(result.get("apply_generation")) is int
                and result["apply_generation"] > 0
            ):
                self._forget_transferred_local_claim(claim)
            elif (
                isinstance(result, Mapping)
                and result.get("status") == "applied"
                and result.get("apply_id") is None
                and result.get("manager_apply") is None
            ):
                self._settle_pending_local_claim("owner_commit")
            else:
                # Neither accepted nor applied: an unsent claim is released
                # now; a sent one is the Manager's.
                if self._claim_sent():
                    effects.submitted()
                    self._forget_sent_claim()
                else:
                    self._settle_failed_operation(envelope, operation_kind, effects)
                raise self._unavailable(
                    "manager_apply_acceptance_invalid",
                    "Manager submission outcome cannot be safely classified",
                )
            if after_release is not None:
                after_release()
            self._record_outcome(
                envelope, operation_kind,
                {"apply_id": result["apply_id"], "apply_generation": result["apply_generation"]}
                if result.get("status") == "accepted" else None,
            )
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
        prepare_project_under_claim: Callable[[], Mapping[str, Any]] | None,
        effects: OperationEffects,
    ) -> LocalAuthoringCommit:
        self._require_workspace_open(workspace)
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
        self._require_authoring_allowed()
        # The envelope's admission generation is claim evidence only; the
        # Kernel admits the claim on its current facts.
        revision_before = workspace.revision
        claim_key = envelope.request_key
        self._acquire_local_claim(
            envelope=envelope,
            claim_key=claim_key,
            payload_digest=payload_digest,
            expected_admission_generation=envelope.expected_admission_generation,
            operation_kind=operation_kind,
        )
        try:
            self._require_mutation_ready()
            if validate_under_claim is not None:
                validate_under_claim()
            if prepare_project_under_claim is not None:
                project = prepare_project_under_claim()
                if not isinstance(project, Mapping):
                    raise TypeError(
                        "under-claim project preparation returned invalid facts"
                    )
        except Exception:
            # A destination written under the claim is a known effect.
            self._settle_failed_operation(envelope, operation_kind, effects)
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
                # Every destination this operation writes is recorded before
                # the commit, so a containment reports the written file.
                context=AuthoringOperationContext(
                    operation=operation_kind,
                    application_start_id=envelope.application_start_id,
                    request_key=envelope.request_key,
                    owner_transaction_id=claim_key.hex(),
                    written_path=effects.destination,
                ),
            )
        except Exception as error:
            if not installed and workspace.failed_closed is None:
                # The workspace refused the commit before any install: settle
                # from what the operation recorded (for example a Save whose
                # destination is already written).
                self._settle_failed_operation(envelope, operation_kind, effects)
                raise
            # An in-memory install whose outcome is unknown contains the
            # workspace; the claim stays held for diagnosis.
            effects.workspace_committed()
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
        context: AuthoringOperationContext,
    ) -> "AuthoringWorkspaceSnapshot":
        if session == "whole-workspace":
            assert install is not None
            return workspace.replace(
                expected_identity=expected_identity,
                install=install,
                project=project,
                notify=False,
                context=context,
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
                context=context,
            )
        assert install is not None
        return workspace.commit_program(
            draft_id=draft_id,
            expected_identity=expected_identity,
            install=install,
            notify=False,
            context=context,
        )

    def _acquire_local_claim(
        self,
        *,
        envelope: AuthoringMutationEnvelope,
        claim_key: bytes,
        payload_digest: str,
        expected_admission_generation: int,
        operation_kind: str,
    ) -> dict[str, Any]:
        # The claim carries the key of the request it derives from (the
        # browser's, or Composer's own for a server request; contract 1.15.2);
        # the browser's `A` is forwarded
        # unchanged so Headless refuses a request built against another start.
        transaction_id = claim_key.hex()
        request = {
            "request_key": transaction_id,
            "application_start_id": format_application_start_id(
                envelope.application_start_id
            ),
            "source_label": "manyforge_composer",
            "expected_admission_generation": expected_admission_generation,
        }
        with self._local_claim_lock:
            self._pending_local_claim = _PendingLocalClaim(
                acquire_request=request,
                owner_transaction_id=transaction_id,
                operation_kind=operation_kind,
                payload_digest=payload_digest,
                settle_request_key=claim_key,
            )
        try:
            response = self._client.acquire_authoring_transaction(request)
        except HeadlessClientError as error:
            if not _answer_lost(error):
                with self._local_claim_lock:
                    self._pending_local_claim = None
            # A lost answer keeps the exact request: the next action replays
            # it and settles what it finds.
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

    def _settle_failed_operation(
        self,
        envelope: AuthoringMutationEnvelope,
        operation_kind: str,
        effects: OperationEffects,
    ) -> None:
        """Settle the claim of a failed operation from its recorded effects."""

        self._settle_pending_local_claim(effects.settlement_kind)
        if not effects.none:
            # A definite post-effect outcome: a client rechecking this exact
            # request reads its claim as settled, not as unknown.
            self._record_outcome(envelope, operation_kind)

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
                if not _answer_lost(error):
                    # The Kernel refused the exact replay: this request holds
                    # no claim (released, expired or never admitted).
                    with self._local_claim_lock:
                        self._pending_local_claim = None
                    return None
                # Still no answer: this attempt is refused, the record kept.
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
                    "request_key": pending.settle_request_key.hex(),
                    "application_start_id": pending.acquire_request[
                        "application_start_id"
                    ],
                    "claim": pending.claim,
                    "settlement_kind": pending.settlement_kind,
                    "evidence_digest": self._release_evidence(pending),
                }
            )
        except Exception as error:
            if not _release_not_exact(error):
                # No definite answer: this attempt is refused, the record kept
                # for the next action's release.
                raise self._unavailable(
                    "composer_authoring_release_uncertain",
                    "the exact local claim remains unsettled",
                ) from error
            # The Kernel no longer holds the exact claim (an earlier release
            # whose answer was lost): it is settled.
            response = None
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
    "OperationEffects",
    "PARTIAL_EFFECT_OBSERVATION_CODES",
    "draft_session_http_exception",
    "headless_client_http_exception",
    "raise_operation_failure",
    "raise_save_failure",
    "refused_before_effect",
    "scenario_artifact_http_exception",
]
