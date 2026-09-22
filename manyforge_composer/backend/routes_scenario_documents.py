"""Editable Scenario Save/Load routes over existing authoring owners."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any, NoReturn
import uuid

from fastapi import APIRouter, HTTPException, Query
from manyforge_behavior.program import Program
from pydantic import BaseModel, ConfigDict, Field

from .authoring_admission import AuthoringMutationEnvelope
from .authoring_forest import (
    AuthoringForestError,
    derive_candidate,
    validate_invariants,
)
from .authoring_transaction_journal import AuthoringJournalError
from .deployment_draft import DeploymentDraftError
from .draft_session import DraftSessionError
from .scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    capture_scenario_artifact_with_diagnostics,
    convert_legacy_scenario_artifact,
    discover_scenario_artifacts,
    export_scenario_artifact,
    normalize_scenario_save_path,
    save_scenario_artifact,
)
from .state import ComposerState


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)


class _ScenarioFence(BaseModel):
    model_config = _DTO

    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: int = Field(
        alias="expectedDraftRevision", ge=0, le=9_007_199_254_740_991
    )
    deployment_draft_id: str = Field(
        alias="deploymentDraftId", min_length=1, max_length=256
    )
    expected_deployment_draft_revision: int = Field(
        alias="expectedDeploymentDraftRevision", ge=0, le=9_007_199_254_740_991
    )
    expected_admission_generation: int = Field(
        alias="expectedAdmissionGeneration", ge=1, le=9_007_199_254_740_991
    )
    request_owner_epoch: str = Field(
        alias="requestOwnerEpoch", min_length=1, max_length=256
    )
    request_id: str = Field(alias="requestId", min_length=1, max_length=256)

    def envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )


class SaveScenarioDocumentRequest(_ScenarioFence):
    path: str = Field(min_length=1, max_length=4096)
    scenario_id: str = Field(alias="scenarioId", pattern=r"^[a-z][a-z0-9._-]{0,127}$")
    name: str = Field(min_length=1, max_length=256)
    description: str = Field(default="", max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class LoadScenarioDocumentRequest(_ScenarioFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class ConvertLegacyScenarioDocumentRequest(BaseModel):
    model_config = _DTO

    source_path: str = Field(alias="sourcePath", min_length=1, max_length=4096)
    destination_path: str = Field(
        alias="destinationPath", min_length=1, max_length=4096
    )
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class ExportScenarioDocumentRequest(_ScenarioFence):
    destination_path: str = Field(
        alias="destinationPath", min_length=1, max_length=4096
    )


def _raise(error: Exception) -> NoReturn:
    if isinstance(error, ScenarioArtifactError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, DeploymentDraftError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, DraftSessionError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, AuthoringForestError):
        raise HTTPException(
            status_code=400,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, AuthoringJournalError):
        raise HTTPException(
            status_code=409,
            detail={"code": error.code, "message": str(error)},
        ) from error
    raise error


def _version() -> str:
    path = Path(__file__).resolve().parents[2] / "VERSION"
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError:
        return "development"
    return value or "development"


def _default_directory(state: ComposerState) -> str | None:
    for root_value in state.allowed_fs_roots:
        root = Path(root_value)
        examples = root / "examples"
        if examples.is_dir() and not examples.is_symlink():
            return str(examples.resolve())
    return state.allowed_fs_roots[0] if state.allowed_fs_roots else None


def _program_post_state(
    state: ComposerState,
    authoring: dict[str, Any] | None,
    program: Program | None,
    diagnostics: list[dict[str, str]],
) -> dict[str, Any]:
    current = state.draft_session.retained_state()
    return {
        "schema_version": current["schema_version"],
        "draft_id": f"draft-scenario-{uuid.uuid4().hex}",
        "draft_revision": 0 if authoring is None else 1,
        "authoring": deepcopy(authoring),
        "program": None if program is None else program.to_dict(),
        "diagnostics": deepcopy(diagnostics),
        "saved_path": None,
        "saved_program_revision": None,
        "saved_document": None,
    }


def _deployment_post_state(
    state: ComposerState,
    deployment: dict[str, Any] | None,
) -> dict[str, Any]:
    current = state.deployment_draft_session.retained_state()
    return {
        "schema_version": current["schema_version"],
        "draft_id": f"deployment-draft-scenario-{uuid.uuid4().hex}",
        "draft_revision": 0 if deployment is None else 1,
        "document": deepcopy(deployment),
        "saved_path": None,
        "saved_document": None,
    }


def _workspace_post_state(
    state: ComposerState,
    authoring: dict[str, Any] | None,
    program: Program | None,
    diagnostics: list[dict[str, str]],
    deployment: dict[str, Any] | None,
) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.composer_authoring_workspace.v2",
        "program": _program_post_state(state, authoring, program, diagnostics),
        "deployment": _deployment_post_state(state, deployment),
    }


def _require_fence(state: ComposerState, body: _ScenarioFence) -> None:
    state.draft_session.require_identity(
        body.draft_id, body.expected_draft_revision
    )
    state.deployment_draft_session.require_current(
        body.deployment_draft_id,
        body.expected_deployment_draft_revision,
    )


def create_scenario_document_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/api/scenario-documents")
    def list_documents(
        directory: str | None = Query(default=None, max_length=4096),
    ) -> dict[str, Any]:
        try:
            selected = directory or _default_directory(state)
            current_directory, saved = discover_scenario_artifacts(
                selected,
                allowed_roots=state.allowed_fs_roots,
                manyforge_version=_version(),
            )
            return {
                "api_version": "manyforge.scenario_document_index.v1",
                "directory": current_directory,
                "entries": saved,
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/save")
    def save_document(body: SaveScenarioDocumentRequest) -> dict[str, Any]:
        try:
            _require_fence(state, body)
            program_state = state.draft_session.retained_state()
            deployment_state = state.deployment_draft_session.retained_state()
            deployment = deployment_state.get("document")
            assignment = deepcopy(state.scenario_program_role_assignment)
            complete = (
                isinstance(program_state.get("authoring"), dict)
                and isinstance(deployment, dict)
                and isinstance(assignment, dict)
            )
            document = build_scenario_document(
                scenario_id=body.scenario_id,
                name=body.name.strip(),
                description=body.description.strip(),
                program_draft=program_state.get("authoring"),
                deployment_draft=deployment,
                program_role_assignment=assignment,
                typed_parameter_overrides=deepcopy(
                    state.scenario_typed_parameter_overrides
                ),
                manyforge_version=_version(),
                last_known_status="unknown" if complete else "incomplete",
            )
            target = normalize_scenario_save_path(body.path)
            current_target = state.scenario_document_path
            overwrite = body.confirm_overwrite or (
                current_target is not None
                and str(Path(target).expanduser().resolve()) == current_target
            )
            saved_path = save_scenario_artifact(
                document,
                target,
                allowed_roots=state.allowed_fs_roots,
                overwrite=overwrite,
                source_project_path=current_target,
            )
            captured, _, source_digest = capture_scenario_artifact(
                saved_path, allowed_roots=state.allowed_fs_roots
            )
            state.scenario_document_path = saved_path
            state.remember_scenario_document(
                captured, source_document_digest=source_digest
            )
            return {
                "status": "saved",
                "path": saved_path,
                "scenario_id": body.scenario_id,
                "last_known_status": document["saved_status"]["last_known_status"],
                "workspace": state.draft_session.workspace_snapshot(
                    applied_program_revision=None
                ),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/load")
    def load_document(body: LoadScenarioDocumentRequest) -> dict[str, Any]:
        try:
            _require_fence(state, body)
            authoring_catalog = state.node_catalog
            (
                scenario_document,
                source_path,
                source_digest,
                source_diagnostics,
            ) = capture_scenario_artifact_with_diagnostics(
                body.path, allowed_roots=state.allowed_fs_roots
            )
            scenario_document_id = scenario_document["scenario_id"]
            authoring = scenario_document["program_draft"]
            deployment = deepcopy(scenario_document["deployment_draft"])
            program: Program | None = None
            diagnostics: list[dict[str, str]] = []
            if authoring is not None:
                try:
                    validate_invariants(authoring, authoring_catalog)
                    program, diagnostics = derive_candidate(
                        authoring, authoring_catalog
                    )
                except AuthoringForestError as error:
                    raise ScenarioArtifactError(
                        "scenario_program_draft_invalid",
                        f"{source_path}: program_draft: {error}",
                    ) from error
            post_state = _workspace_post_state(
                state, authoring, program, diagnostics, deployment
            )
            incomplete = (
                program is None
                or deployment is None
                or scenario_document["program_role_assignment"] is None
            )
            current_program = state.draft_session.retained_state()
            current_deployment = state.deployment_draft_session.retained_state()
            if not body.confirm_discard and not state.current_drafts_match_remembered_scenario() and (
                (
                    current_program.get("program") is not None
                    and current_program.get("program")
                    != current_program.get("saved_document")
                )
                or (
                    current_deployment.get("document") is not None
                    and current_deployment.get("document")
                    != current_deployment.get("saved_document")
                )
            ):
                raise ScenarioArtifactError(
                    "scenario_discard_confirmation_required",
                    "Loading would replace unsaved Program or Deployment Draft changes",
                    status_code=409,
                )
            state._restore_authoring_post_image(post_state, catalog=authoring_catalog)
            state.scenario_document_path = source_path
            state.remember_scenario_document(
                scenario_document,
                source_document_digest=source_digest,
                source_diagnostics=source_diagnostics,
            )
            return {
                "status": "opened",
                "activation": None,
                "diagnostics": diagnostics,
                "source_diagnostics": source_diagnostics,
                "path": source_path,
                "scenario_id": scenario_document_id,
                "incomplete": incomplete,
                "workspace": state.draft_session.workspace_snapshot(
                    applied_program_revision=None
                ),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/convert-legacy")
    def convert_legacy_document(
        body: ConvertLegacyScenarioDocumentRequest,
    ) -> dict[str, Any]:
        try:
            saved_path = convert_legacy_scenario_artifact(
                body.source_path,
                body.destination_path,
                allowed_roots=state.allowed_fs_roots,
                overwrite=body.confirm_overwrite,
            )
            return {
                "status": "converted",
                "source_path": str(Path(body.source_path).expanduser().resolve()),
                "path": saved_path,
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/export")
    def export_document(body: ExportScenarioDocumentRequest) -> dict[str, Any]:
        try:
            _require_fence(state, body)
            source_path = state.scenario_document_path
            if source_path is None:
                raise ScenarioArtifactError(
                    "scenario_export_source_unavailable",
                    "Save the Scenario project before exporting it",
                    status_code=409,
                )
            program_state = state.draft_session.retained_state()
            deployment_state = state.deployment_draft_session.retained_state()
            document = build_scenario_document(
                scenario_id=(
                    state.scenario_document_id
                    if isinstance(state.scenario_document_id, str)
                    else Path(source_path).name
                ),
                name=(
                    state.scenario_document_name
                    if isinstance(state.scenario_document_name, str)
                    else Path(source_path).name
                ),
                description=(
                    state.scenario_document_description
                    if isinstance(state.scenario_document_description, str)
                    else ""
                ),
                program_draft=program_state.get("authoring"),
                deployment_draft=deployment_state.get("document"),
                program_role_assignment=deepcopy(
                    state.scenario_program_role_assignment
                ),
                typed_parameter_overrides=deepcopy(
                    state.scenario_typed_parameter_overrides
                ),
                manyforge_version=_version(),
                last_known_status="unknown",
            )
            exported_path = export_scenario_artifact(
                document,
                body.destination_path,
                source_project_path=source_path,
                allowed_roots=state.allowed_fs_roots,
            )
            return {"status": "exported", "path": exported_path}
        except Exception as error:
            _raise(error)

    return router
