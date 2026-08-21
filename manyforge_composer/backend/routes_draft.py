"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    verify_content_identity,
)
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
_APPLIED_FIELDS = frozenset(
    {
        "assistant_mode",
        "baseline_program_binding_id",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "preparation_digest",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "schema_version",
        "source",
        "typed_parameter_overrides",
    }
)
_APPLIED_SOURCE_FIELDS = frozenset(
    {
        "record_digest",
        "semantic_content_digest",
        "source_generation",
        "source_owner_epoch",
        "source_owner_id",
    }
)
_PROGRAM_REQUIRED_FIELDS = frozenset(
    {
        "api_version",
        "program_id",
        "program_revision",
        "program_profile",
        "name",
        "catalog",
        "component_roles",
        "target_roles",
        "tree",
    }
)
_PROGRAM_FIELDS = _PROGRAM_REQUIRED_FIELDS | frozenset(
    {
        "blackboard",
        "compatibility",
        "contact_policies",
        "description",
        "parameters",
        "scene",
        "viewer",
    }
)
_PROGRAM_BINDING_FIELDS = frozenset(
    {
        "api_version",
        "component_role_bindings",
        "contact_grasp_bindings",
        "graph_revision",
        "node_environment",
        "program_binding_id",
        "program_id",
        "program_revision",
        "provenance",
        "retained_bundle_id",
        "target_role_bindings",
    }
)


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


class NewDraftRequest(_DraftFence):
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


class DiscardDraftRequest(_DraftFence):
    pass


class RevertDraftRequest(_DraftFence):
    pass


class LoadProgramRequest(ApplyDraftRequest):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class _ProgramApplyPending(RuntimeError):
    def __init__(self, result: dict[str, Any]) -> None:
        super().__init__("Program Apply is waiting for deployment convergence")
        self.result = result


@dataclass(frozen=True, slots=True)
class _AppliedContext:
    scenario_id: str
    scenario_catalog_revision: str
    program_revision: str
    record_digest: str
    program_document: dict[str, Any]
    binding_document: dict[str, Any]
    projection_document: dict[str, Any]


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


def _sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 71
        and value.startswith("sha256:")
        and all(character in "0123456789abcdef" for character in value[7:])
    )


def _applied_context(
    state: ComposerState,
) -> tuple[_AppliedContext | None, dict[str, Any] | None]:
    if state.headless_client is None:
        return None, {
            "code": "program_apply_unavailable",
            "message": "authoritative Program Apply requires a configured headless API",
            "restorationGates": [],
        }
    try:
        applied = state.headless_client.program_applied()
    except HeadlessClientError as error:
        return None, {
            "code": error.code,
            "message": str(error),
            "restorationGates": [],
        }
    if not isinstance(applied, dict):
        return None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program projection",
            "restorationGates": [],
        }
    source = applied.get("source")
    program = applied.get("program")
    binding = applied.get("program_binding")
    program_id = applied.get("program_id")
    program_revision = applied.get("program_revision")
    program_binding_id = applied.get("program_binding_id")
    record_digest = source.get("record_digest") if isinstance(source, dict) else None
    node_environment = (
        binding.get("node_environment") if isinstance(binding, dict) else None
    )
    try:
        program_identity = (
            verify_content_identity(
                program,
                own_field="program_revision",
                identity_domain="program-revision",
                api_version="manyforge.program.v1",
                label="applied_program.program",
            )
            if isinstance(program, dict)
            else None
        )
        binding_identity = (
            verify_content_identity(
                binding,
                own_field="program_binding_id",
                identity_domain="program-binding-id",
                api_version="manyforge.program_binding.v1",
                label="applied_program.program_binding",
            )
            if isinstance(binding, dict)
            else None
        )
    except (StrictArtifactError, TypeError, ValueError):
        program_identity = None
        binding_identity = None
    if (
        set(applied) != _APPLIED_FIELDS
        or applied.get("schema_version")
        != "manyforge.applied_program_selection.v1"
        or applied.get("assistant_mode")
        not in {"required", "optional", "forbidden"}
        or not _sha256(applied.get("baseline_program_binding_id"))
        or not _sha256(applied.get("deployment_revision"))
        or not _sha256(applied.get("node_environment_digest"))
        or not _sha256(applied.get("preparation_digest"))
        or not _sha256(applied.get("scenario_catalog_revision"))
        or not isinstance(applied.get("scenario_id"), str)
        or not applied.get("scenario_id")
        or not isinstance(applied.get("deployment_artifact_id"), str)
        or not applied.get("deployment_artifact_id")
        or not isinstance(applied.get("typed_parameter_overrides"), dict)
        or not isinstance(program_id, str)
        or not program_id
        or not _sha256(program_revision)
        or not isinstance(program, dict)
        or not _PROGRAM_REQUIRED_FIELDS.issubset(program)
        or not set(program).issubset(_PROGRAM_FIELDS)
        or not isinstance(program.get("catalog"), dict)
        or not isinstance(program.get("component_roles"), dict)
        or not isinstance(program.get("target_roles"), dict)
        or not isinstance(program.get("tree"), dict)
        or program.get("program_id") != program_id
        or program_identity != program_revision
        or not isinstance(binding, dict)
        or set(binding) != _PROGRAM_BINDING_FIELDS
        or binding.get("program_id") != program_id
        or binding.get("program_revision") != program_revision
        or binding_identity != program_binding_id
        or binding.get("retained_bundle_id") != applied.get("retained_bundle_id")
        or binding.get("graph_revision") != applied.get("graph_revision")
        or binding.get("api_version") != "manyforge.program_binding.v1"
        or not isinstance(binding.get("component_role_bindings"), list)
        or not all(
            isinstance(row, dict)
            for row in binding.get("component_role_bindings", ())
        )
        or not isinstance(binding.get("target_role_bindings"), list)
        or not all(
            isinstance(row, dict)
            for row in binding.get("target_role_bindings", ())
        )
        or not isinstance(binding.get("contact_grasp_bindings"), list)
        or not all(
            isinstance(row, dict)
            for row in binding.get("contact_grasp_bindings", ())
        )
        or not isinstance(binding.get("provenance"), dict)
        or not isinstance(node_environment, dict)
        or set(node_environment)
        != {
            "catalog_id",
            "content_version",
            "catalog_digest",
            "environment_digest",
        }
        or not _sha256(node_environment.get("catalog_digest"))
        or not _sha256(node_environment.get("environment_digest"))
        or node_environment.get("environment_digest")
        != applied.get("node_environment_digest")
        or not isinstance(source, dict)
        or set(source) != _APPLIED_SOURCE_FIELDS
        or not _sha256(record_digest)
        or not _sha256(source.get("semantic_content_digest"))
        or type(source.get("source_generation")) is not int
        or source.get("source_generation", 0) < 1
        or not isinstance(source.get("source_owner_id"), str)
        or not source.get("source_owner_id")
        or not isinstance(source.get("source_owner_epoch"), str)
        or not source.get("source_owner_epoch")
    ):
        return None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program projection",
            "restorationGates": [],
        }
    return (
        _AppliedContext(
            scenario_id=applied["scenario_id"],
            scenario_catalog_revision=applied["scenario_catalog_revision"],
            program_revision=program_revision,
            record_digest=record_digest,
            program_document=program,
            binding_document=binding,
            projection_document=applied,
        ),
        None,
    )


def _workspace(state: ComposerState) -> dict[str, Any]:
    context, reason = _applied_context(state)
    return state.draft_session.workspace_snapshot(
        applied_program_revision=(
            context.program_revision if context is not None else None
        ),
        applied_unavailable_reason=reason,
    )


def _program_write_roots(state: ComposerState) -> list[str]:
    """Use the explicit Program-write boundary, or preserve legacy callers."""

    if state.allowed_program_write_roots is not None:
        return state.allowed_program_write_roots
    return state.allowed_fs_roots


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

    @router.post("/api/program/new-draft")
    def new_draft(body: NewDraftRequest) -> dict[str, Any]:
        try:
            def applied_revision() -> str | None:
                context, _reason = _applied_context(state)
                return context.program_revision if context is not None else None

            state.draft_session.new_draft(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                confirm_discard=body.confirm_discard,
                applied_revision_reader=applied_revision,
                catalog_reader=lambda: _active_catalog(state),
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
                    allowed_roots=_program_write_roots(state),
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
                    allowed_roots=_program_write_roots(state),
                    overwrite=overwrite,
                ),
            )
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/apply")
    def apply_draft(body: ApplyDraftRequest) -> JSONResponse:
        try:
            context, reason = _applied_context(state)
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
                    expected_applied_record_digest=(
                        context.record_digest if context is not None else None
                    ),
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
            context, reason = _applied_context(state)
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
                    expected_applied_record_digest=(
                        context.record_digest if context is not None else None
                    ),
                )
                if result.get("status") != "applied":
                    raise _ProgramApplyPending(result)
                selected, selected_reason = _applied_context(state)
                applied = result["applied"]
                if (
                    selected_reason is not None
                    or selected is None
                    or selected.program_document != program.to_dict()
                    or selected.projection_document != applied
                    or selected.record_digest
                    != applied["source"]["record_digest"]
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
                current_applied_program_revision=(
                    context.program_revision if context is not None else None
                ),
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

    @router.post("/api/program/discard")
    def discard_draft(body: DiscardDraftRequest) -> dict[str, Any]:
        try:
            def read_applied() -> Program:
                context, reason = _applied_context(state)
                if reason is not None or context is None:
                    raise DraftSessionError(
                        (
                            reason["code"]
                            if reason
                            else "program_applied_selection_invalid"
                        ),
                        reason["message"] if reason else "Applied Program is absent",
                        status_code=(
                            404
                            if reason
                            and reason["code"]
                            == "program_applied_selection_not_found"
                            else 502
                            if reason
                            and reason["code"]
                            == "program_applied_selection_invalid"
                            else 503
                        ),
                    )
                try:
                    candidate = Program.from_dict(
                        context.program_document,
                        _active_catalog(state),
                        path="<composer-program-discard>",
                    )
                except ProgramValidationError as error:
                    raise DraftSessionError(
                        "composer_applied_program_validation_failed",
                        str(error),
                        status_code=409,
                        detail=str(error),
                    ) from error
                if candidate.program_revision != context.program_revision:
                    raise DraftSessionError(
                        "program_applied_selection_invalid",
                        "headless Applied Program content is divergent",
                        status_code=502,
                    )
                return candidate

            return state.draft_session.discard_to_applied(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                reader=read_applied,
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/revert")
    def revert_draft(body: RevertDraftRequest) -> JSONResponse:
        try:
            def prepare_revert(
                saved_document: dict[str, Any],
            ) -> tuple[Program, Program, _AppliedContext]:
                context, reason = _applied_context(state)
                if reason is not None or context is None:
                    raise DraftSessionError(
                        (
                            reason["code"]
                            if reason
                            else "program_applied_selection_invalid"
                        ),
                        reason["message"] if reason else "Applied Program is absent",
                        status_code=(
                            404
                            if reason
                            and reason["code"]
                            == "program_applied_selection_not_found"
                            else
                            502
                            if reason
                            and reason["code"]
                            == "program_applied_selection_invalid"
                            else 503
                        ),
                    )
                catalog = _active_catalog(state)
                try:
                    validated_saved = Program.from_dict(
                        saved_document,
                        catalog,
                        path="<composer-program-revert-saved>",
                    )
                    applied = Program.from_dict(
                        context.program_document,
                        catalog,
                        path="<composer-program-revert-applied>",
                    )
                except ProgramValidationError as error:
                    raise DraftSessionError(
                        "composer_revert_program_validation_failed",
                        str(error),
                        status_code=409,
                        detail=str(error),
                    ) from error
                return validated_saved, applied, context

            def apply_saved(
                program: Program,
                context: _AppliedContext,
            ) -> dict[str, Any]:
                result = _apply_program(
                    state,
                    program=program,
                    scenario_id=context.scenario_id,
                    scenario_catalog_revision=context.scenario_catalog_revision,
                    expected_applied_record_digest=context.record_digest,
                )
                if result.get("status") != "applied":
                    return result
                selected, selected_reason = _applied_context(state)
                receipt = result["applied"]
                if (
                    selected_reason is not None
                    or selected is None
                    or selected.program_document != program.to_dict()
                    or selected.program_revision != program.program_revision
                    or selected.projection_document != receipt
                    or selected.record_digest
                    != receipt["source"]["record_digest"]
                ):
                    raise DraftSessionError(
                        "program_apply_selection_stale",
                        "Applied Program changed before Revert could commit its "
                        "local Draft identity",
                        status_code=409,
                    )
                return result

            result, _snapshot = state.draft_session.revert_saved_after_apply(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                prepare=prepare_revert,
                applier=apply_saved,
            )
            status = (
                "reverted"
                if result is None or result.get("status") == "applied"
                else "pending_deployment"
            )
            return JSONResponse(
                status_code=200 if status == "reverted" else 202,
                content={
                    "status": status,
                    "headless": result,
                    "workspace": (
                        _snapshot if status == "reverted" else _workspace(state)
                    ),
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    return router


__all__ = ["create_draft_router"]
