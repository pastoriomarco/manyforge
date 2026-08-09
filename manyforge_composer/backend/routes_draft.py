"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from pathlib import Path
from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, ConfigDict, Field

from .draft_session import DraftSessionError
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    normalize_program_save_path,
    save_program_artifact,
)
from .state import ComposerState


_AUTHORING_MUTATIONS = frozenset(
    {
        "blackboard_draft_remove_keys",
        "blackboard_draft_remove_namespaces",
        "blackboard_draft_upsert_keys",
        "blackboard_draft_upsert_namespaces",
        "program_draft_remove_parameters",
        "program_draft_upsert_parameters",
        "tree_draft_change_node_kind",
        "tree_draft_delete_node",
        "tree_draft_insert_node",
        "tree_draft_move_node",
        "tree_draft_replace_subtree",
        "tree_draft_update_node_params",
        "tree_draft_wrap_node",
    }
)
_DTO_CONFIG = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)


class _DraftFence(BaseModel):
    model_config = _DTO_CONFIG

    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: int = Field(
        alias="expectedDraftRevision",
        ge=0,
        le=9_007_199_254_740_991,
    )


class DraftMutationRequest(_DraftFence):
    mutation: str = Field(min_length=1, max_length=128)
    arguments: dict[str, Any]


class OpenDraftRequest(_DraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class SaveDraftRequest(_DraftFence):
    pass


class SaveDraftAsRequest(_DraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


def _raise_workspace_error(error: Exception) -> NoReturn:
    if isinstance(error, (DraftSessionError, ProgramArtifactError)):
        detail: dict[str, Any] = {
            "code": error.code,
            "message": str(error),
        }
        if isinstance(error, DraftSessionError) and error.detail != str(error):
            detail["context"] = error.detail
        raise HTTPException(status_code=error.status_code, detail=detail) from error
    if isinstance(error, ActiveNodeEnvironmentProjectionError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    raise error


def _active_catalog(state: ComposerState):
    if state.headless_client is None:
        return state.node_catalog
    return read_active_node_environment(state).catalog


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose revision-fenced Draft/Saved authoring without Apply or runtime truth."""

    router = APIRouter()

    @router.get("/api/program/draft")
    def read_draft() -> dict[str, Any]:
        return state.draft_session.snapshot()

    @router.get("/api/program/workspace")
    def read_workspace() -> dict[str, Any]:
        return state.draft_session.workspace_snapshot()

    @router.post("/api/program/draft/mutations")
    def mutate_draft(body: DraftMutationRequest) -> dict[str, Any]:
        if body.mutation not in _AUTHORING_MUTATIONS:
            raise HTTPException(
                status_code=404,
                detail={
                    "code": "composer_authoring_mutation_unsupported",
                    "message": (
                        f"unsupported Program authoring mutation: {body.mutation}"
                    ),
                },
            )
        try:
            result = state.draft_session.mutate(
                tool_id=body.mutation,
                arguments=body.arguments,
                expected_revision=body.expected_draft_revision,
                catalog=_active_catalog(state),
                draft_id=body.draft_id,
            )
        except Exception as error:
            _raise_workspace_error(error)
        return {
            "mutation": result.mutation,
            "details": result.details,
            "workspace": state.draft_session.workspace_snapshot(),
        }

    @router.post("/api/program/open-draft")
    def open_draft(body: OpenDraftRequest) -> dict[str, Any]:
        try:
            state.draft_session.require_replace_allowed(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
            )
            program, source_path = load_program_artifact(
                body.path,
                allowed_roots=state.allowed_fs_roots,
                catalog=_active_catalog(state),
            )
            return state.draft_session.replace_program(
                program,
                source_path=source_path,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/save")
    def save_draft(body: SaveDraftRequest) -> dict[str, Any]:
        try:
            return state.draft_session.save_program(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                target_path=None,
                writer=lambda program, path: save_program_artifact(
                    program,
                    path,
                    allowed_roots=state.allowed_fs_roots,
                    overwrite=True,
                ),
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/save-as")
    def save_draft_as(body: SaveDraftAsRequest) -> dict[str, Any]:
        try:
            normalized = normalize_program_save_path(body.path)
            try:
                candidate = Path(normalized).expanduser()
                resolved_candidate = str(candidate.parent.resolve() / candidate.name)
            except (OSError, RuntimeError, ValueError):
                resolved_candidate = normalized
            current_path = state.draft_session.workspace_snapshot()["saved"]["path"]
            overwrite = body.confirm_overwrite or resolved_candidate == current_path
            return state.draft_session.save_program(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                target_path=normalized,
                writer=lambda program, path: save_program_artifact(
                    program,
                    path,
                    allowed_roots=state.allowed_fs_roots,
                    overwrite=overwrite,
                ),
            )
        except Exception as error:
            _raise_workspace_error(error)

    return router


__all__ = ["create_draft_router"]
