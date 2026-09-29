"""One shared composite authoring workspace projection.

Every browser, reload and reconnect of one workbench backend reads this single
authoritative workspace. The projection carries the composite
``workspace_epoch``/``workspace_revision`` beside the Program Draft/Saved
projection, the Deployment Draft/Saved projection and the Scenario project
facts.

Runtime, Applied and Manager truth is deliberately absent: the existing
Headless-owned projections remain authoritative for those facts, and the
existing Program and Deployment workspace routes stay as thin projections until
their generated-contract and supported external consumers are inventoried.
"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException, Query

from .authoring_workspace import AuthoringWorkspaceError
from .authoring_admission import AuthoringMutationEnvelope
from .state import ComposerState


def create_authoring_workspace_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/api/authoring/submission-outcome")
    def read_submission_outcome(
        request_owner_epoch: str = Query(min_length=1, max_length=256),
        request_id: str = Query(min_length=1, max_length=256),
        expected_admission_generation: int = Query(ge=1, le=9_007_199_254_740_991),
    ) -> dict[str, Any]:
        """Read exact confirmed evidence; never retry, settle or release a claim."""
        if state.authoring_admission is None:
            raise HTTPException(status_code=503, detail={
                "code": "authoring_owner_unavailable", "message": "Authoring owner is unavailable",
            })
        return state.authoring_admission.submission_outcome(AuthoringMutationEnvelope(
            expected_admission_generation, request_owner_epoch, request_id,
        ))

    @router.get("/api/authoring/workspace")
    def read_authoring_workspace() -> dict[str, Any]:
        """Capture the current workspace once, without taking any effect."""

        try:
            return state.authoring_workspace.snapshot().as_dict()
        except AuthoringWorkspaceError as error:
            raise HTTPException(
                status_code=error.status_code,
                detail={"code": error.code, "message": str(error)},
            ) from error

    return router


__all__ = ["create_authoring_workspace_router"]
