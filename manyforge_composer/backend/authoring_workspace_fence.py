"""Shared composite-workspace fence and local commit for migrated requests.

Every migrated ordinary authoring request carries the expected workspace epoch
and revision in addition to its Program or Deployment domain fence. The route
captures one composite identity from the workspace owner, requires the
client-issued fence to match that captured identity, and hands the same identity
to the commit, which rechecks it atomically under the workspace lock.

A mismatch is a typed stale rejection that returns the current identity, so the
client refreshes the workspace instead of retrying a blind edit. A lost
response therefore cannot repeat an edit: the retry names the previous
revision and is rejected.

The client fence is deliberate and mandatory. Deploying one protocol with
optional fence fields and a second protocol without them would leave a stale
edit unguarded and would leave temporary compatibility code behind after the
cutover, so a not-yet-migrated caller keeps its old request model instead of a
weakened new one.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Callable, Mapping

from fastapi import HTTPException

from .authoring_admission import LocalAuthoringCommit

if TYPE_CHECKING:
    from .authoring_workspace import AuthoringWorkspaceIdentity
    from .state import ComposerState


# One JavaScript-safe integer ceiling (2**53 - 1), shared with the Program and
# Deployment Draft revision bounds the fence fields are validated against.
MAXIMUM_WORKSPACE_FENCE_INTEGER = 9_007_199_254_740_991


def require_captured_workspace_identity(
    expected_epoch: str,
    expected_revision: int,
    identity: "AuthoringWorkspaceIdentity",
) -> None:
    """Require one client-issued composite fence against a captured identity."""

    if (
        expected_epoch != identity.workspace_epoch
        or expected_revision != identity.workspace_revision
    ):
        raise HTTPException(
            status_code=409,
            detail={
                "code": "composer_workspace_stale",
                "message": (
                    "the Composer authoring workspace advanced; refresh the "
                    "workspace identity and retry"
                ),
                "workspace": identity.as_dict(),
            },
        )


def commit_local_authoring(
    state: "ComposerState",
    body: Any,
    *,
    operation_kind: str,
    captured_identity: "AuthoringWorkspaceIdentity | None" = None,
    install: Callable[[], bool] | None = None,
    validate_under_claim: Callable[[], None] | None = None,
    session: str = "program",
    project: Mapping[str, Any] | None = None,
    prepare_project_under_claim: Callable[[], Mapping[str, Any]] | None = None,
) -> LocalAuthoringCommit:
    """Commit one already-prepared value through the local admission path.

    The caller has prepared outside every lock and validated its own domain
    precondition. ``body`` supplies the composite fence, the Program or
    Deployment domain fence and the admission envelope; the captured identity is
    required from the client fence and then handed to the commit, which rechecks
    it atomically together with the domain fence.
    """

    coordinator = state.authoring_admission
    if coordinator is None:
        raise HTTPException(
            status_code=503,
            detail={
                "code": "authoring_not_ready",
                "message": "authoritative authoring admission is unavailable",
            },
        )
    identity: "AuthoringWorkspaceIdentity" = (
        state.authoring_workspace.identity()
        if captured_identity is None
        else captured_identity
    )
    body.require_workspace_fence(identity)
    return coordinator.execute_local_mutation(
        body.admission_envelope(),
        operation_kind=operation_kind,
        draft_id=body.draft_id,
        draft_revision=body.expected_draft_revision,
        payload=body.model_dump(mode="json", by_alias=True),
        workspace=state.authoring_workspace,
        install=install,
        validate_under_claim=validate_under_claim,
        session=session,
        expected_identity=identity,
        project=project,
        prepare_project_under_claim=prepare_project_under_claim,
    )


def commit_draft_edit(
    state: "ComposerState",
    body: Any,
    *,
    operation_kind: str,
    install: Callable[[], bool],
    session: str = "program",
    validate: Callable[[], None] | None = None,
) -> LocalAuthoringCommit:
    """Commit one Draft-only edit (no cell claim; see ``execute_draft_edit``).

    The same composite and domain fences as :func:`commit_local_authoring`
    apply; admission is Idle authoring or a held run whose motion is
    confirmed stopped.
    """

    coordinator = state.authoring_admission
    if coordinator is None:
        raise HTTPException(
            status_code=503,
            detail={
                "code": "authoring_not_ready",
                "message": "authoritative authoring admission is unavailable",
            },
        )
    identity: "AuthoringWorkspaceIdentity" = state.authoring_workspace.identity()
    body.require_workspace_fence(identity)
    return coordinator.execute_draft_edit(
        body.admission_envelope(),
        operation_kind=operation_kind,
        workspace=state.authoring_workspace,
        draft_id=body.draft_id,
        expected_identity=identity,
        install=install,
        session=session,
        validate=validate,
    )


__all__ = [
    "MAXIMUM_WORKSPACE_FENCE_INTEGER",
    "commit_draft_edit",
    "commit_local_authoring",
    "require_captured_workspace_identity",
]
