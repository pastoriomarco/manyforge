"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from pathlib import Path
from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.client import HeadlessClientError
from pydantic import BaseModel, ConfigDict, Field

from .draft_session import DraftSessionError, OPERATOR_DRAFT_MUTATIONS
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
        "scene_draft_add_object",
        "scene_draft_remove_objects",
        "scene_draft_update_object",
        "scene_draft_upsert_objects",
        "tree_draft_change_node_kind",
        "tree_draft_delete_node",
        "tree_draft_insert_node",
        "tree_draft_move_node",
        "tree_draft_replace_subtree",
        "tree_draft_update_node_params",
        "tree_draft_wrap_node",
        *OPERATOR_DRAFT_MUTATIONS,
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


class ApplyDraftRequest(_DraftFence):
    scenario_id: str = Field(
        alias="scenarioId",
        min_length=1,
        max_length=128,
        pattern=r"^[a-z][a-z0-9_.-]*$",
    )
    scenario_catalog_revision: str = Field(
        alias="scenarioCatalogRevision",
        pattern=r"^sha256:[0-9a-f]{64}$",
    )


class LoadProgramRequest(ApplyDraftRequest):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class _ProgramApplyPending(RuntimeError):
    def __init__(self, result: dict[str, Any]) -> None:
        super().__init__("Program Apply is waiting for deployment convergence")
        self.result = result


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
    if isinstance(error, HeadlessClientError):
        raise HTTPException(
            status_code=error.status_code or 503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    raise error


def _active_catalog(state: ComposerState):
    if state.headless_client is None:
        return state.node_catalog
    return read_active_node_environment(state).catalog


def _applied_context(
    state: ComposerState,
) -> tuple[str | None, str | None, dict[str, Any] | None]:
    if state.headless_client is None:
        return None, None, {
            "code": "program_apply_unavailable",
            "message": "authoritative Program Apply requires a configured headless API",
            "restorationGates": ["N7"],
        }
    try:
        applied = state.headless_client.program_applied()
    except HeadlessClientError as error:
        if error.code == "program_applied_selection_not_found":
            return None, None, {
                "code": error.code,
                "message": str(error),
                "restorationGates": [],
            }
        return None, None, {
            "code": error.code,
            "message": str(error),
            "restorationGates": [],
        }
    source = applied.get("source")
    program_revision = applied.get("program_revision")
    record_digest = source.get("record_digest") if isinstance(source, dict) else None
    if (
        applied.get("schema_version") != "manyforge.applied_program_selection.v1"
        or not isinstance(program_revision, str)
        or not isinstance(record_digest, str)
    ):
        return None, None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program projection",
            "restorationGates": [],
        }
    return program_revision, record_digest, None


def _workspace(state: ComposerState) -> dict[str, Any]:
    revision, _record_digest, reason = _applied_context(state)
    return state.draft_session.workspace_snapshot(
        applied_program_revision=revision,
        applied_unavailable_reason=reason,
    )


def _apply_program(
    state: ComposerState,
    *,
    program: Program,
    scenario_id: str,
    scenario_catalog_revision: str,
    expected_applied_record_digest: str | None,
) -> dict[str, Any]:
    if state.headless_client is None:
        raise DraftSessionError(
            "program_apply_unavailable",
            "authoritative Program Apply requires a configured headless API",
            status_code=503,
        )
    try:
        candidate = Program.from_dict(
            program.to_dict(),
            _active_catalog(state),
            path="<composer-program-apply>",
        )
    except ProgramValidationError as error:
        raise DraftSessionError(
            "composer_draft_validation_failed",
            str(error),
            detail=str(error),
        ) from error
    result = state.headless_client.program_apply(
        {
            "scenario_id": scenario_id,
            "scenario_catalog_revision": scenario_catalog_revision,
            "program": candidate.to_dict(),
            "expected_applied_record_digest": expected_applied_record_digest,
        }
    )
    status = result.get("status")
    applied = result.get("applied")
    if (
        result.get("schema_version") != "manyforge.program_apply_result.v1"
        or status not in {"applied", "pending_deployment"}
        or (status == "pending_deployment" and applied is not None)
        or (
            status == "applied"
            and (
                not isinstance(applied, dict)
                or applied.get("program_revision") != candidate.program_revision
                or not isinstance(applied.get("source"), dict)
                or not isinstance(applied["source"].get("record_digest"), str)
            )
        )
    ):
        raise DraftSessionError(
            "program_apply_response_invalid",
            "headless Program Apply returned a divergent result",
            status_code=502,
        )
    return result


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose revision-fenced authoring and thin authoritative Apply/Load."""

    router = APIRouter()

    @router.get("/api/program/draft")
    def read_draft() -> dict[str, Any]:
        return state.draft_session.snapshot()

    @router.get("/api/program/workspace")
    def read_workspace() -> dict[str, Any]:
        return _workspace(state)

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
            "workspace": _workspace(state),
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
            state.draft_session.replace_program(
                program,
                source_path=source_path,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
            )
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/save")
    def save_draft(body: SaveDraftRequest) -> dict[str, Any]:
        try:
            state.draft_session.save_program(
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
            return _workspace(state)
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
            state.draft_session.save_program(
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
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/apply")
    def apply_draft(body: ApplyDraftRequest) -> JSONResponse:
        try:
            _applied_revision, expected_record_digest, reason = _applied_context(
                state
            )
            if (
                reason is not None
                and reason["code"] != "program_applied_selection_not_found"
            ):
                raise DraftSessionError(
                    reason["code"],
                    reason["message"],
                    status_code=503,
                )
            result = state.draft_session.apply_current_program(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                applier=lambda program: _apply_program(
                    state,
                    program=program,
                    scenario_id=body.scenario_id,
                    scenario_catalog_revision=body.scenario_catalog_revision,
                    expected_applied_record_digest=expected_record_digest,
                ),
            )
            return JSONResponse(
                status_code=200 if result.get("status") == "applied" else 202,
                content={
                    "status": result.get("status"),
                    "headless": result,
                    "workspace": _workspace(state),
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/load")
    def load_program(body: LoadProgramRequest) -> JSONResponse:
        try:
            applied_revision, expected_record_digest, reason = _applied_context(state)
            if (
                reason is not None
                and reason["code"] != "program_applied_selection_not_found"
            ):
                raise DraftSessionError(
                    reason["code"],
                    reason["message"],
                    status_code=503,
                )

            def apply_loaded(program: Program) -> dict[str, Any]:
                result = _apply_program(
                    state,
                    program=program,
                    scenario_id=body.scenario_id,
                    scenario_catalog_revision=body.scenario_catalog_revision,
                    expected_applied_record_digest=expected_record_digest,
                )
                if result.get("status") != "applied":
                    raise _ProgramApplyPending(result)
                selected_revision, selected_record, selected_reason = (
                    _applied_context(state)
                )
                applied = result["applied"]
                if (
                    selected_reason is not None
                    or selected_revision != program.program_revision
                    or selected_record != applied["source"]["record_digest"]
                ):
                    raise DraftSessionError(
                        "program_apply_selection_stale",
                        "Applied Program changed before Load could commit its "
                        "local Draft and Saved identities",
                        status_code=409,
                    )
                return result

            result, _snapshot = state.draft_session.load_and_apply_program(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
                current_applied_program_revision=applied_revision,
                loader=lambda: load_program_artifact(
                    body.path,
                    allowed_roots=state.allowed_fs_roots,
                    catalog=_active_catalog(state),
                ),
                applier=apply_loaded,
            )
            return JSONResponse(
                status_code=200,
                content={
                    "status": "loaded",
                    "headless": result,
                    "workspace": _workspace(state),
                },
            )
        except _ProgramApplyPending as pending:
            return JSONResponse(
                status_code=202,
                content={
                    "status": "pending_deployment",
                    "headless": pending.result,
                    "workspace": _workspace(state),
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    return router


__all__ = ["create_draft_router"]
