"""One composite in-memory authoring workspace owner.

This owner composes the existing Program and Deployment Draft sessions and the
Scenario project facts into one addressable workspace. It adds only what those
owners cannot express on their own:

- one random process-lifetime ``workspace_epoch``;
- one checked, monotonically increasing JavaScript-safe
  ``workspace_revision`` that advances exactly once per real committed change;
- one closed immutable snapshot carrying the composite identity beside the
  Program and Deployment Draft/Saved projections and the project facts.

It owns no validators, compiler or editor logic. Every domain decision stays in
:class:`~manyforge_composer.backend.draft_session.ComposerDraftSession`,
:class:`~manyforge_composer.backend.deployment_draft.DeploymentDraftSession` and
their existing compiler. Runtime, Applied and Manager truth is explicitly *not*
part of this owner; the existing Headless-owned projections remain
authoritative for those facts.

Locking
-------

The workspace lock order is mandatory and one-way::

    workspace lock -> Program session lock -> Deployment session lock

No code may acquire a session lock and then the workspace lock. Everything in
this module follows that order: the workspace lock is taken first, and the
composed sessions are then read or mutated through their own locking public
API.

The workspace lock is a short in-memory serialization boundary only. It MUST
NOT be held during filesystem I/O, Headless/Kernel/Manager calls, source
resolution, Program derivation, Deployment compilation or notification
delivery. The supported shape is: capture or copy briefly, prepare the whole
effect outside the lock, then re-enter once to check the expected composite and
domain identities and commit the already-prepared value. The installed value
must be an in-memory state install; a caller that needs a derivation, a model
load or a network read performs it before calling.

The observer is invoked at most once per real commit, after every workspace and
session lock has been released.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass, field
import logging
import threading
from types import MappingProxyType
import uuid
from typing import TYPE_CHECKING, Any, Callable, Mapping, NoReturn

from .authoring_identity import canonical_digest
from .scenario_artifacts import scenario_project_read_only

if TYPE_CHECKING:
    from .deployment_draft import DeploymentDraftSession
    from .draft_session import ComposerDraftSession


SNAPSHOT_API_VERSION = "manyforge.composer_authoring_workspace_snapshot.v3"

# One JavaScript-safe integer ceiling (2**53 - 1), shared with the Program
# Draft revision. Exhaustion fails closed; it never wraps and never restarts
# the backend as compaction.
AUTHORING_WORKSPACE_MAXIMUM_REVISION = 9_007_199_254_740_991

_PROJECT_FACT_FIELDS = (
    "document_path",
    "read_only",
    "document_id",
    "document_name",
    "document_description",
    "source_diagnostics",
    "source_document_digest",
    "source_program_digest",
    "source_deployment_digest",
)

_PROGRAM_SNAPSHOT_FIELDS = (
    "draftId",
    "draftRevision",
    "programLoaded",
    "authoring",
    "candidateAvailable",
    "programRevision",
    "program",
    "diagnostics",
    "formulaReferences",
    "saved",
)
_PROGRAM_SAVED_FIELDS = ("available", "matchesDraft", "path", "programRevision")

_DEPLOYMENT_SNAPSHOT_FIELDS = (
    "draftId",
    "draftRevision",
    "document",
    "savedPath",
    "savedDocument",
    "differsFromSaved",
)

_LOGGER = logging.getLogger(__name__)


def _freeze_json(value: Any) -> Any:
    if type(value) is dict:
        return MappingProxyType(
            {key: _freeze_json(member) for key, member in value.items()}
        )
    if type(value) is list:
        return tuple(_freeze_json(member) for member in value)
    return value


def _thaw_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw_json(member) for key, member in value.items()}
    if type(value) is tuple:
        return [_thaw_json(member) for member in value]
    return value


def _require_project_shape(value: Mapping[str, Any]) -> None:
    if not isinstance(value, Mapping) or set(value) != set(_PROJECT_FACT_FIELDS):
        _fail(
            "composer_workspace_project_invalid",
            "replacement Scenario project facts have an invalid shape",
            status_code=503,
        )


def scenario_project_facts(
    document: Mapping[str, Any] | None,
    *,
    document_path: str | None = None,
    source_document_digest: str | None = None,
    source_diagnostics: list[dict[str, str]] | None = None,
) -> dict[str, Any]:
    """Derive the closed Scenario project facts for one loaded document.

    A pure derivation, so a caller that must prepare outside every lock — the
    authoring-only document Load, which installs its project facts in the same
    commit as the two Draft replacements — can pass the result to
    :meth:`AuthoringWorkspace.replace` or :meth:`AuthoringWorkspace.remember_project`
    without a second inconsistency window between those two owner calls.
    """

    if document is None:
        # Forgetting the loaded document clears its identity, digests and
        # assignment. The selected project path is a project
        # selection rather than document content, so it is retained unless the
        # caller supplies a replacement path.
        return {
            "document_path": document_path,
            "read_only": scenario_project_read_only(document_path),
            "document_id": None,
            "document_name": None,
            "document_description": None,
            "source_diagnostics": [],
            "source_document_digest": None,
            "source_program_digest": None,
            "source_deployment_digest": None,
        }
    return {
        "document_path": document_path,
        # The built-in Untitled Scenario: Save As only, never saved in place.
        "read_only": scenario_project_read_only(document_path),
        "document_id": document.get("scenario_id"),
        "document_name": document.get("name"),
        "document_description": document.get("description"),
        "source_diagnostics": deepcopy(source_diagnostics or []),
        "source_document_digest": source_document_digest,
        "source_program_digest": canonical_digest(
            {"value": document.get("program_draft")}
        ),
        "source_deployment_digest": canonical_digest(
            {"value": document.get("deployment_draft")}
        ),
    }


class AuthoringWorkspaceError(RuntimeError):
    """One composite workspace operation failed or was contained."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        status_code: int = 409,
        detail: Any | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.detail = message if detail is None else detail


def _fail(
    code: str,
    message: str,
    *,
    status_code: int = 409,
    detail: Any | None = None,
) -> NoReturn:
    raise AuthoringWorkspaceError(
        code,
        message,
        status_code=status_code,
        detail=detail,
    )


@dataclass(frozen=True, slots=True)
class AuthoringOperationContext:
    """Caller identity reported once if this commit contains the workspace.

    It changes no commit decision: the owner only logs it at containment and
    publishes ``written_path`` beside the containment cause.
    """

    operation: str
    request_owner_epoch: str | None = None
    request_id: str | None = None
    owner_transaction_id: str | None = None
    written_path: str | None = None


@dataclass(frozen=True, slots=True)
class AuthoringWorkspaceIdentity:
    """One composite identity: the workspace fence plus both domain fences."""

    workspace_epoch: str
    workspace_revision: int
    program_draft_id: str
    program_draft_revision: int
    deployment_draft_id: str
    deployment_draft_revision: int

    def as_dict(self) -> dict[str, Any]:
        return {
            "workspaceEpoch": self.workspace_epoch,
            "workspaceRevision": self.workspace_revision,
            "programDraftId": self.program_draft_id,
            "programDraftRevision": self.program_draft_revision,
            "deploymentDraftId": self.deployment_draft_id,
            "deploymentDraftRevision": self.deployment_draft_revision,
        }


@dataclass(frozen=True, slots=True)
class AuthoringWorkspaceSnapshot:
    """One closed immutable workspace capture.

    ``program``, ``deployment`` and ``project`` are deep copies taken at one
    consistent revision. The shape is closed: construction rejects any
    additional or missing member so no caller can smuggle a second workspace
    owner into the payload.
    """

    workspace_epoch: str
    workspace_revision: int
    program: Mapping[str, Any] = field(repr=False)
    deployment: Mapping[str, Any] = field(repr=False)
    project: Mapping[str, Any] = field(repr=False)

    def __post_init__(self) -> None:
        if (
            type(self.workspace_epoch) is not str
            or not self.workspace_epoch
            or type(self.workspace_revision) is not int
            or not 1
            <= self.workspace_revision
            <= AUTHORING_WORKSPACE_MAXIMUM_REVISION
        ):
            raise ValueError("authoring workspace snapshot identity is invalid")
        for value, expected, name in (
            (self.program, _PROGRAM_SNAPSHOT_FIELDS, "program"),
            (self.deployment, _DEPLOYMENT_SNAPSHOT_FIELDS, "deployment"),
            (self.project, _PROJECT_FACT_FIELDS, "project"),
        ):
            if type(value) is not dict or set(value) != set(expected):
                raise ValueError(
                    f"authoring workspace snapshot {name} projection is not closed"
                )
        saved = self.program["saved"]
        if type(saved) is not dict or set(saved) != set(_PROGRAM_SAVED_FIELDS):
            raise ValueError(
                "authoring workspace snapshot Program Saved projection is not closed"
            )
        # Freezing constructs a detached tree; the session snapshots passed by
        # the owner are already copies, so another deepcopy would only multiply
        # the cost of large Program and Deployment documents.
        object.__setattr__(self, "program", _freeze_json(self.program))
        object.__setattr__(self, "deployment", _freeze_json(self.deployment))
        object.__setattr__(self, "project", _freeze_json(self.project))

    @property
    def identity(self) -> AuthoringWorkspaceIdentity:
        return AuthoringWorkspaceIdentity(
            workspace_epoch=self.workspace_epoch,
            workspace_revision=self.workspace_revision,
            program_draft_id=self.program["draftId"],
            program_draft_revision=self.program["draftRevision"],
            deployment_draft_id=self.deployment["draftId"],
            deployment_draft_revision=self.deployment["draftRevision"],
        )

    def as_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": SNAPSHOT_API_VERSION,
            "workspaceEpoch": self.workspace_epoch,
            "workspaceRevision": self.workspace_revision,
            "program": _thaw_json(self.program),
            "deployment": _thaw_json(self.deployment),
            "project": _thaw_json(self.project),
        }


class AuthoringWorkspace:
    """Own one composite workspace revision over the two Draft sessions."""

    def __init__(
        self,
        *,
        program_session: "ComposerDraftSession",
        deployment_session: "DeploymentDraftSession",
        workspace_epoch: str | None = None,
    ) -> None:
        if workspace_epoch is not None and (
            type(workspace_epoch) is not str or not workspace_epoch
        ):
            raise ValueError("workspace epoch must be a non-empty string")
        self._lock = threading.RLock()
        self._program = program_session
        self._deployment = deployment_session
        self._epoch = workspace_epoch or f"workspace-{uuid.uuid4().hex}"
        self._revision = 1
        self._failed_closed: str | None = None
        self._contained_written_path: str | None = None
        self._containment_report: dict[str, Any] | None = None
        self._observer: Callable[[], None] | None = None
        self._document_path: str | None = None
        self._document_id: str | None = None
        self._document_name: str | None = None
        self._document_description: str | None = None
        self._source_diagnostics: list[dict[str, str]] = []
        self._source_document_digest: str | None = None
        self._source_program_digest: str | None = None
        self._source_deployment_digest: str | None = None

    # -- identity ---------------------------------------------------------

    @property
    def epoch(self) -> str:
        return self._epoch

    @property
    def failed_closed(self) -> str | None:
        """Return the containment reason, or ``None`` while the workspace is usable.

        A non-``None`` value means an in-memory install violated its contract,
        so the workspace refuses further commits and a caller must not treat a
        refused commit as a proven no-op. This diagnostic read stays available
        while the workspace is contained.
        """

        with self._lock:
            return self._failed_closed

    def publication(
        self,
    ) -> tuple[AuthoringWorkspaceIdentity, dict[str, Any] | None]:
        """Return the published identity and the containment record, or ``None``.

        A diagnostic read for the control-stream shell. It stays available
        while the workspace is contained, so every client sees the containment
        without attempting an edit. A contained identity is only the last
        observed one and fences nothing: every commit and fenced read refuses.
        """

        with self._lock:
            containment = (
                None
                if self._failed_closed is None
                else {
                    "cause": self._failed_closed,
                    "saved_path": self._contained_written_path,
                }
            )
            return self._identity_locked(), containment

    @property
    def revision(self) -> int:
        with self._lock:
            self._require_usable_locked()
            return self._revision

    def identity(self) -> AuthoringWorkspaceIdentity:
        with self._lock:
            self._require_usable_locked()
            return self._identity_locked()

    def require_identity(self, expected: AuthoringWorkspaceIdentity) -> None:
        """Require one exact composite identity without changing the workspace."""

        with self._lock:
            self._require_usable_locked()
            self._require_identity_locked(expected)

    # -- observer ---------------------------------------------------------

    def set_observer(self, observer: Callable[[], None] | None) -> None:
        """Install the one latest-only presentation wake callback."""

        if observer is not None and not callable(observer):
            raise TypeError("Authoring workspace observer must be callable")
        with self._lock:
            self._observer = observer

    # -- reads ------------------------------------------------------------

    def project_facts(self) -> dict[str, Any]:
        with self._lock:
            self._require_usable_locked()
            return self._project_facts_locked()

    def snapshot(self) -> AuthoringWorkspaceSnapshot:
        with self._lock:
            self._require_usable_locked()
            return self._snapshot_locked()

    # -- commits ----------------------------------------------------------

    def remember_project(
        self,
        *,
        expected_identity: AuthoringWorkspaceIdentity | None = None,
        document: Mapping[str, Any] | None,
        document_path: str | None = None,
        source_document_digest: str | None = None,
        source_diagnostics: list[dict[str, str]] | None = None,
        notify: bool = True,
    ) -> AuthoringWorkspaceSnapshot:
        """Install the Scenario project facts in one composite commit.

        ``notify=False`` suppresses the wake so the local admission path can
        publish exactly one wake after the Kernel authoring claim is settled;
        the caller then calls :meth:`publish_commit_wake`.
        """

        with self._lock:
            captured, advanced = self._commit_project_locked(
                expected_identity=expected_identity,
                document=document,
                document_path=document_path,
                source_document_digest=source_document_digest,
                source_diagnostics=source_diagnostics,
                explicit=None,
            )
        if advanced and notify:
            self._notify_once()
        return captured

    def commit_project(
        self,
        *,
        expected_identity: AuthoringWorkspaceIdentity | None = None,
        facts: Mapping[str, Any],
        notify: bool = True,
    ) -> AuthoringWorkspaceSnapshot:
        """Commit one already-prepared Scenario project-facts replacement.

        This replaces only project metadata. A Save that also adopts Program
        or Deployment Saved baselines uses :meth:`replace` to commit those
        session changes and the matching project facts together.
        """

        _require_project_shape(facts)
        with self._lock:
            captured, advanced = self._commit_project_locked(
                expected_identity=expected_identity,
                document=None,
                document_path=None,
                source_document_digest=None,
                source_diagnostics=None,
                explicit=facts,
            )
        if advanced and notify:
            self._notify_once()
        return captured

    def _commit_project_locked(
        self,
        *,
        expected_identity: AuthoringWorkspaceIdentity | None,
        document: Mapping[str, Any] | None,
        document_path: str | None,
        source_document_digest: str | None,
        source_diagnostics: list[dict[str, str]] | None,
        explicit: Mapping[str, Any] | None,
    ) -> tuple[AuthoringWorkspaceSnapshot, bool]:
        """Install project facts and report whether the composite advanced."""

        self._require_usable_locked()
        self._require_capacity_locked()
        if expected_identity is not None:
            self._require_identity_locked(expected_identity)
        before = self._project_facts_locked()
        self._remember_project_locked(
            document=document,
            document_path=document_path,
            source_document_digest=source_document_digest,
            source_diagnostics=source_diagnostics,
            explicit=explicit,
        )
        changed = self._finish_commit_locked(
            self._project_facts_locked() != before
        )
        return self._snapshot_locked(), changed

    def publish_commit_wake(self) -> None:
        """Publish the one latest-only wake for an already committed change.

        The local admission path commits with ``notify=False`` so the workspace
        wake follows the exact Kernel claim settlement instead of preceding it.
        """

        self._notify_once()

    def commit_program(
        self,
        *,
        draft_id: str,
        expected_identity: AuthoringWorkspaceIdentity | None = None,
        install: Callable[[], bool],
        notify: bool = True,
        context: AuthoringOperationContext | None = None,
    ) -> AuthoringWorkspaceSnapshot:
        """Commit one already-prepared Program Draft change.

        ``install`` performs an in-memory state install only and returns whether
        it changed workspace state. It must fence the Program Draft identity
        itself through the existing session API; this owner additionally
        verifies that the install did not rotate the Program Draft identity.
        ``notify=False`` defers the single wake to
        :meth:`publish_commit_wake`, so the local admission path can publish it
        after the Kernel authoring claim is settled. ``context`` identifies the
        caller's operation if this commit contains the workspace.
        """

        return self._commit(
            session="program",
            draft_id=draft_id,
            expected_identity=expected_identity,
            install=install,
            notify=notify,
            context=context,
        )

    def commit_deployment(
        self,
        *,
        draft_id: str,
        expected_identity: AuthoringWorkspaceIdentity | None = None,
        install: Callable[[], bool],
        notify: bool = True,
        context: AuthoringOperationContext | None = None,
    ) -> AuthoringWorkspaceSnapshot:
        """Commit one already-prepared Deployment Draft change.

        Shares the contract of :meth:`commit_program` for the Deployment
        session.
        """

        return self._commit(
            session="deployment",
            draft_id=draft_id,
            expected_identity=expected_identity,
            install=install,
            notify=notify,
            context=context,
        )

    def replace(
        self,
        *,
        expected_identity: AuthoringWorkspaceIdentity | None = None,
        install: Callable[[], bool],
        project: Mapping[str, Any] | None = None,
        notify: bool = True,
        context: AuthoringOperationContext | None = None,
    ) -> AuthoringWorkspaceSnapshot:
        """Replace the whole workspace, allowing both Draft identities to rotate.

        Used by whole-project Load, New and Discard-style replacements whose
        prepared value already contains the successor Draft identities. An
        optional ``project`` payload installs the matching Scenario project
        facts in the same single-commit boundary. Save uses the same boundary
        without rotating Draft identities, adopting the written Saved baselines.
        """

        if project is not None:
            # Reject malformed prepared input before any session can change.
            _require_project_shape(project)
        try:
            with self._lock:
                self._require_usable_locked()
                self._require_capacity_locked()
                if expected_identity is not None:
                    self._require_identity_locked(expected_identity)
                identity_before = self._identity_locked()
                changed = self._run_install_locked(
                    "whole-workspace", install, context
                )
                identity_after = self._identity_locked()
                if not changed and identity_after != identity_before:
                    self._contain_locked(
                        "whole-workspace",
                        "whole-workspace install changed a Draft identity "
                        "while reporting no change",
                        context,
                    )
                    _fail(
                        "composer_workspace_install_scope_invalid",
                        "Composer authoring workspace was contained after an "
                        "in-memory install changed state outside its result",
                        status_code=503,
                        detail={"cause": self._failed_closed},
                    )
                project_before = self._project_facts_locked()
                if project is not None:
                    self._remember_project_locked(
                        document=None,
                        document_path=None,
                        source_document_digest=None,
                        source_diagnostics=None,
                        explicit=project,
                    )
                changed = changed or self._project_facts_locked() != project_before
                advanced = self._finish_commit_locked(changed)
                captured = self._snapshot_locked()
        finally:
            self._publish_containment()
        if advanced and notify:
            self._notify_once()
        return captured

    # -- internals --------------------------------------------------------

    def _commit(
        self,
        *,
        session: str,
        draft_id: str,
        expected_identity: AuthoringWorkspaceIdentity | None,
        install: Callable[[], bool],
        notify: bool = True,
        context: AuthoringOperationContext | None = None,
    ) -> AuthoringWorkspaceSnapshot:
        if not callable(install):
            raise TypeError("authoring workspace install must be callable")
        try:
            with self._lock:
                self._require_usable_locked()
                self._require_capacity_locked()
                if expected_identity is not None:
                    self._require_identity_locked(expected_identity)
                self._require_domain_identity_locked(session, draft_id)
                before = self._identity_locked()
                changed = self._run_install_locked(session, install, context)
                self._require_domain_identity_locked(
                    session, draft_id, after=True, context=context
                )
                after = self._identity_locked()
                self._require_commit_scope_locked(
                    session, before, after, changed, context
                )
                advanced = self._finish_commit_locked(changed)
                captured = self._snapshot_locked()
        finally:
            self._publish_containment()
        if advanced and notify:
            self._notify_once()
        return captured

    def _finish_commit_locked(self, changed: bool) -> bool:
        """Advance the composite revision iff a real domain change committed."""

        if not changed:
            # A validated no-op commits nothing and does not advance.
            return False
        if self._revision >= AUTHORING_WORKSPACE_MAXIMUM_REVISION:
            _fail(
                "composer_workspace_revision_exhausted",
                "Composer authoring workspace revision is exhausted; "
                "restart the Composer backend and reload the saved project",
                status_code=503,
            )
        self._revision += 1
        return True

    def _contain_locked(
        self,
        label: str,
        cause: str,
        context: AuthoringOperationContext | None,
    ) -> None:
        """Contain the workspace: the one writer of the containment state.

        Containment is never cleared in-process; a Composer restart is the
        recovery. The report is logged and published once by
        :meth:`_publish_containment` after the lock is released.
        """

        if self._failed_closed is not None:
            return
        self._failed_closed = cause
        self._contained_written_path = (
            None if context is None else context.written_path
        )
        self._containment_report = {
            "workspace_epoch": self._epoch,
            "workspace_revision": self._revision,
            "install_label": label,
            "operation": None if context is None else context.operation,
            "cause": cause,
            "written_path": self._contained_written_path,
            "request_owner_epoch": (
                None if context is None else context.request_owner_epoch
            ),
            "request_id": None if context is None else context.request_id,
            "owner_transaction_id": (
                None if context is None else context.owner_transaction_id
            ),
        }

    def _publish_containment(self) -> None:
        """Log and wake once for a containment this call caused, unlocked."""

        with self._lock:
            report = self._containment_report
            self._containment_report = None
        if report is None:
            return
        _LOGGER.error("composer_authoring_workspace_contained", extra=report)
        self._notify_once()

    def _run_install_locked(
        self,
        label: str,
        install: Callable[[], bool],
        context: AuthoringOperationContext | None = None,
    ) -> bool:
        """Run one prevalidated no-I/O install and contain contract failures."""

        if not callable(install):
            raise TypeError("authoring workspace install must be callable")
        try:
            changed = install()
        except Exception as error:
            self._contain_locked(
                label,
                f"{label} in-memory install failed: "
                f"{type(error).__name__}: {error}",
                context,
            )
            _fail(
                "composer_workspace_install_failed",
                "Composer authoring workspace was contained after an "
                "in-memory install failed",
                status_code=503,
                detail={"cause": self._failed_closed},
            )
        if type(changed) is not bool:
            self._contain_locked(
                label,
                f"{label} in-memory install did not return a change result",
                context,
            )
            _fail(
                "composer_workspace_install_contract_invalid",
                "Composer authoring workspace was contained after an "
                "in-memory install violated its contract",
                status_code=503,
                detail={"cause": self._failed_closed},
            )
        return changed

    def _require_commit_scope_locked(
        self,
        session: str,
        before: AuthoringWorkspaceIdentity,
        after: AuthoringWorkspaceIdentity,
        changed: bool,
        context: AuthoringOperationContext | None = None,
    ) -> None:
        other_changed = (
            (
                before.deployment_draft_id,
                before.deployment_draft_revision,
            )
            != (
                after.deployment_draft_id,
                after.deployment_draft_revision,
            )
            if session == "program"
            else (
                before.program_draft_id,
                before.program_draft_revision,
            )
            != (
                after.program_draft_id,
                after.program_draft_revision,
            )
        )
        if other_changed or (not changed and before != after):
            self._contain_locked(
                session,
                f"{session} in-memory install changed state outside its result",
                context,
            )
            _fail(
                "composer_workspace_install_scope_invalid",
                "Composer authoring workspace was contained after an "
                "in-memory install changed an unexpected domain",
                status_code=503,
                detail={"cause": self._failed_closed},
            )

    def _require_capacity_locked(self) -> None:
        if self._revision >= AUTHORING_WORKSPACE_MAXIMUM_REVISION:
            _fail(
                "composer_workspace_revision_exhausted",
                "Composer authoring workspace revision is exhausted; "
                "restart the Composer backend and reload the saved project",
                status_code=503,
            )

    def _require_usable_locked(self) -> None:
        if self._failed_closed is not None:
            _fail(
                "composer_workspace_failed_closed",
                "Composer authoring workspace is contained after a commit that "
                f"violated its contract: {self._failed_closed}",
                status_code=503,
                detail={"cause": self._failed_closed},
            )

    def _identity_locked(self) -> AuthoringWorkspaceIdentity:
        return AuthoringWorkspaceIdentity(
            workspace_epoch=self._epoch,
            workspace_revision=self._revision,
            program_draft_id=self._program.draft_id,
            program_draft_revision=self._program.revision,
            deployment_draft_id=self._deployment.draft_id,
            deployment_draft_revision=self._deployment.revision,
        )

    def _require_identity_locked(
        self, expected: AuthoringWorkspaceIdentity
    ) -> None:
        if not isinstance(expected, AuthoringWorkspaceIdentity):
            raise TypeError(
                "expected authoring workspace identity must be "
                "an AuthoringWorkspaceIdentity"
            )
        current = self._identity_locked()
        if expected != current:
            _fail(
                "composer_workspace_stale",
                "Composer authoring workspace changed; refresh before retrying",
                status_code=409,
                detail={
                    "workspaceEpoch": current.workspace_epoch,
                    "workspaceRevision": current.workspace_revision,
                    "workspace": current.as_dict(),
                },
            )

    def _require_domain_identity_locked(
        self,
        session: str,
        draft_id: str,
        *,
        after: bool = False,
        context: AuthoringOperationContext | None = None,
    ) -> None:
        current = (
            self._program.draft_id
            if session == "program"
            else self._deployment.draft_id
        )
        if current == draft_id:
            return
        if after:
            # An ordinary install must never rotate a Draft identity. The
            # in-memory state now contradicts the composite fence, so contain
            # the workspace instead of committing or retrying.
            self._contain_locked(
                session,
                f"{session} Draft identity rotated inside an ordinary "
                f"workspace commit ({draft_id} -> {current})",
                context,
            )
        _fail(
            "composer_workspace_domain_identity_changed",
            f"{session} Draft identity changed during one "
            "ordinary workspace commit",
            status_code=503,
            detail={
                "session": session,
                "expectedDraftId": draft_id,
                "currentDraftId": current,
            },
        )

    def _project_facts_locked(self) -> dict[str, Any]:
        return {
            "document_path": self._document_path,
            "read_only": scenario_project_read_only(self._document_path),
            "document_id": self._document_id,
            "document_name": self._document_name,
            "document_description": self._document_description,
            "source_diagnostics": deepcopy(self._source_diagnostics),
            "source_document_digest": self._source_document_digest,
            "source_program_digest": self._source_program_digest,
            "source_deployment_digest": self._source_deployment_digest,
        }

    def _remember_project_locked(
        self,
        *,
        document: Mapping[str, Any] | None,
        document_path: str | None,
        source_document_digest: str | None,
        source_diagnostics: list[dict[str, str]] | None,
        explicit: Mapping[str, Any] | None = None,
    ) -> None:
        if explicit is not None:
            _require_project_shape(explicit)
            facts = explicit
        elif document is None:
            facts = scenario_project_facts(
                None,
                document_path=(
                    document_path
                    if document_path is not None
                    else self._document_path
                ),
            )
        else:
            facts = scenario_project_facts(
                document,
                document_path=document_path,
                source_document_digest=source_document_digest,
                source_diagnostics=source_diagnostics,
            )
        self._document_path = facts["document_path"]
        self._document_id = facts["document_id"]
        self._document_name = facts["document_name"]
        self._document_description = facts["document_description"]
        self._source_diagnostics = deepcopy(facts["source_diagnostics"])
        self._source_document_digest = facts["source_document_digest"]
        self._source_program_digest = facts["source_program_digest"]
        self._source_deployment_digest = facts["source_deployment_digest"]

    def _snapshot_locked(self) -> AuthoringWorkspaceSnapshot:
        program_state = self._program.retained_state()
        deployment_state = self._deployment.retained_state()
        saved_program_revision = program_state["saved_program_revision"]
        program_document = program_state["program"]
        return AuthoringWorkspaceSnapshot(
            workspace_epoch=self._epoch,
            workspace_revision=self._revision,
            program={
                "draftId": program_state["draft_id"],
                "draftRevision": program_state["draft_revision"],
                "programLoaded": program_state["authoring"] is not None,
                "authoring": program_state["authoring"],
                "candidateAvailable": program_document is not None,
                "programRevision": (
                    program_document["program_revision"]
                    if isinstance(program_document, dict)
                    else None
                ),
                "program": program_document,
                "diagnostics": program_state["diagnostics"],
                "formulaReferences": self._program.formula_references(),
                "saved": {
                    "available": program_state["saved_path"] is not None,
                    "matchesDraft": (
                        program_state["saved_path"] is not None
                        and program_state["authoring"] == program_state["saved_document"]
                        if saved_program_revision is None else
                        program_state["authoring"] is not None
                        and isinstance(program_document, dict)
                        and saved_program_revision is not None
                        and program_document["program_revision"]
                        == saved_program_revision
                    ),
                    "path": program_state["saved_path"],
                    "programRevision": saved_program_revision,
                },
            },
            deployment={
                "draftId": deployment_state["draft_id"],
                "draftRevision": deployment_state["draft_revision"],
                "document": deployment_state["document"],
                "savedPath": deployment_state["saved_path"],
                "savedDocument": deployment_state["saved_document"],
                "differsFromSaved": (
                    deployment_state["document"]
                    != deployment_state["saved_document"]
                    if deployment_state["document"] is not None
                    else False
                ),
            },
            project=self._project_facts_locked(),
        )

    def _notify_once(self) -> None:
        """Wake at most once per real commit or containment, after every lock
        is released."""

        with self._lock:
            observer = self._observer
        if observer is None:
            return
        try:
            observer()
        except Exception:
            # A presentation wake cannot roll back or relabel an already
            # committed workspace mutation.
            _LOGGER.exception("Composer authoring workspace wake failed")


__all__ = [
    "AUTHORING_WORKSPACE_MAXIMUM_REVISION",
    "SNAPSHOT_API_VERSION",
    "AuthoringOperationContext",
    "AuthoringWorkspace",
    "AuthoringWorkspaceError",
    "AuthoringWorkspaceIdentity",
    "AuthoringWorkspaceSnapshot",
    "scenario_project_facts",
]
