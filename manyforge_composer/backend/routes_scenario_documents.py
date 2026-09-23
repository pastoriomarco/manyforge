"""Editable Scenario Save/Load routes over existing authoring owners."""

from __future__ import annotations

from copy import deepcopy
import hashlib
from pathlib import Path
from typing import Any, NoReturn
import uuid

from fastapi import APIRouter, HTTPException, Query
from fastapi.routing import APIRoute
from manyforge_behavior.program import Program
from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
from manyforge_behavior.model_source_capture import capture_deployment_model_sources
from pydantic import BaseModel, ConfigDict, Field

from .authoring_admission import AuthoringMutationEnvelope
from .authoring_forest import (
    AuthoringForestError,
    derive_candidate,
    validate_invariants,
)
from .authoring_identity import canonical_digest
from .authoring_admission import AuthoringOperationError
from .authoring_workspace import (
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    scenario_project_facts,
)
from .authoring_workspace_fence import (
    commit_local_authoring,
    require_captured_workspace_identity,
)
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
    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )
    expected_admission_generation: int = Field(
        alias="expectedAdmissionGeneration", ge=1, le=9_007_199_254_740_991
    )
    request_owner_epoch: str = Field(
        alias="requestOwnerEpoch", min_length=1, max_length=256
    )
    request_id: str = Field(alias="requestId", min_length=1, max_length=256)

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )

    def require_workspace_fence(
        self, identity: AuthoringWorkspaceIdentity
    ) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
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
    activate: bool = False


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
    if isinstance(error, AuthoringOperationError):
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


def _document_content_digest(document: dict[str, Any]) -> str:
    """Return the exact identity of the complete persisted Scenario document."""

    return canonical_digest(document)


def _published_content_digest(state: ComposerState, target: str) -> str | None:
    """Return the content identity already published at one exact target.

    A lost response is resolved by rereading the exact target and comparing this
    identity with the content captured for that request; a missing or different
    target returns ``None``, so the ordinary overwrite rules still apply.
    """

    try:
        document, _source, _digest = capture_scenario_artifact(
            target, allowed_roots=state.allowed_fs_roots
        )
    except (ScenarioArtifactError, OSError):
        return None
    try:
        return _document_content_digest(document)
    except (ScenarioArtifactError, OSError, TypeError, ValueError):
        return None


def _require_captured_workspace_fence(
    body: _ScenarioFence, identity: AuthoringWorkspaceIdentity
) -> None:
    """Require the captured composite identity to match the client's fence.

    ``_require_fence`` gives the established typed stale errors; this closes the
    window between that check and the capture, so the bytes written and the
    saved baseline adopted always come from the revision the client fenced.
    """

    require_captured_workspace_identity(
        body.expected_workspace_epoch,
        body.expected_workspace_revision,
        identity,
    )
    if (
        identity.program_draft_id != body.draft_id
        or identity.program_draft_revision != body.expected_draft_revision
        or identity.deployment_draft_id != body.deployment_draft_id
        or identity.deployment_draft_revision
        != body.expected_deployment_draft_revision
    ):
        raise HTTPException(
            status_code=409,
            detail={
                "code": "scenario_workspace_stale",
                "message": (
                    "the authoring workspace advanced while this request was "
                    "captured; refresh the workspace identity and retry"
                ),
                "workspace": identity.as_dict(),
            },
        )


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
            # One immutable composite capture: the project written and the saved
            # baseline adopted come from the same consistent revision, and the
            # identity is rechecked atomically when that baseline is adopted.
            snapshot = state.authoring_workspace.snapshot()
            _require_captured_workspace_fence(body, snapshot.identity)
            # ``as_dict`` is the detached plain-JSON projection; the snapshot's
            # own members are recursively read-only.
            captured_workspace = snapshot.as_dict()
            project = captured_workspace["project"]
            authoring = captured_workspace["program"]["authoring"]
            deployment = captured_workspace["deployment"]["document"]
            assignment = project["program_role_assignment"]
            overrides = project["typed_parameter_overrides"]
            complete = (
                isinstance(authoring, dict)
                and isinstance(deployment, dict)
                and isinstance(assignment, dict)
            )
            document = build_scenario_document(
                scenario_id=body.scenario_id,
                name=body.name.strip(),
                description=body.description.strip(),
                program_draft=authoring,
                deployment_draft=deployment,
                program_role_assignment=assignment,
                typed_parameter_overrides=overrides,
                manyforge_version=_version(),
                last_known_status="unknown" if complete else "incomplete",
            )
            target = normalize_scenario_save_path(body.path)
            current_target = project["document_path"]
            overwrite = body.confirm_overwrite or (
                current_target is not None
                and str(Path(target).expanduser().resolve()) == current_target
            )
            if _published_content_digest(state, target) == _document_content_digest(
                document
            ):
                # This exact content is already published at the target, so a
                # lost response is resolved without repeating the write.
                saved_path = str(Path(target).expanduser().resolve())
            else:
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
            # One composite commit adopts the written project path, Saved
            # baseline and Scenario facts together, fenced by the identity the
            # bytes were captured at. A concurrent edit keeps the correctly
            # written file and refuses only the baseline.
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="save_scenario_document",
                captured_identity=snapshot.identity,
                session="project",
                project=scenario_project_facts(
                    captured,
                    document_path=saved_path,
                    source_document_digest=source_digest,
                ),
            )
            return {
                "status": "saved",
                "path": saved_path,
                "scenario_id": body.scenario_id,
                "last_known_status": document["saved_status"]["last_known_status"],
                "workspace": state.draft_session.workspace_snapshot(
                    applied_program_revision=None
                ),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except AuthoringWorkspaceError as error:
            if error.code != "composer_workspace_stale":
                _raise(error)
            # The project is correctly written; only the Saved baseline is
            # refused, so newer in-memory content is never labelled Saved.
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "scenario_saved_baseline_not_adopted",
                    "message": (
                        "the Scenario project was written but a newer "
                        "workspace revision was not adopted as Saved"
                    ),
                    "currentWorkspace": error.detail.get("workspace"),
                },
            ) from error
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
            activation_intent: dict[str, Any] | None = None
            activation_prepared: dict[str, Any] | None = None
            if body.activate:
                if incomplete or program is None or not isinstance(deployment, dict):
                    raise ScenarioArtifactError(
                        "scenario_activation_incomplete",
                        "only a complete v2 Scenario document can be activated",
                        status_code=409,
                    )
                if state.headless_client is None:
                    raise ScenarioArtifactError(
                        "scenario_activation_unavailable",
                        "Scenario activation requires the configured Manager",
                        status_code=503,
                    )
                source_closure = capture_deployment_model_sources(
                    deployment, project_root=Path(source_path)
                )
                assignment = deepcopy(
                    scenario_document["program_role_assignment"]
                )
                overrides = deepcopy(
                    scenario_document["typed_parameter_overrides"]
                )
                captured_digest = raw_sha256_digest(
                    canonical_json_bytes(
                        {
                            "scenario_id": scenario_document["scenario_id"],
                            "source_document_digest": source_digest,
                            "deployment": deployment,
                            "model_source_closure": source_closure,
                            "program": program.to_dict(),
                            "program_role_assignment": assignment,
                            "typed_parameter_overrides": overrides,
                        }
                    )
                )
                bundle, graph, _epoch = state._manager_current(
                    state.headless_client.manager_status()
                )
                activation_intent = {
                    "scenario_id": scenario_document["scenario_id"],
                    "source_document_digest": source_digest,
                    "captured_input_digest": captured_digest,
                    "deployment": deepcopy(deployment),
                    "model_source_closure": source_closure,
                    "program": program.to_dict(),
                    "program_role_assignment": assignment,
                    "typed_parameter_overrides": overrides,
                    "expected_manager_current": {
                        "retained_bundle_id": bundle,
                        "graph_revision": graph,
                    },
                    "expected_admission_generation": (
                        body.expected_admission_generation
                    ),
                    "manager_request_id": "scenario-document-activation-"
                    + hashlib.sha256(
                        f"{body.request_owner_epoch}:{body.request_id}".encode()
                    ).hexdigest()[:32],
                    "correlation_id": body.request_id,
                    "confirmation_token": None,
                    "draft_id": body.draft_id,
                    "draft_revision": body.expected_draft_revision,
                }
                activation_prepared = state.execute_authored_deployment_candidate(
                    activation_intent, None, apply=True, prepare_only=True
                )
                if activation_prepared.get("confirmation_required") is True:
                    raise ScenarioArtifactError(
                        "scenario_activation_confirmation_required",
                        "Scenario activation requires explicit Deployment confirmation",
                        status_code=409,
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
            # Both retained Draft replacements are validated outside every
            # owner lock, and the whole load commits as one workspace
            # replacement: the successor Program and Deployment identities and
            # the Scenario project facts land in a single fenced commit.
            program_replacement = state.draft_session.prepare_load_replacement(
                post_state["program"], authoring_catalog
            )
            deployment_replacement = (
                state.deployment_draft_session.prepare_load_replacement(
                    post_state["deployment"]
                )
            )

            def install_load() -> bool:
                program_changed = state.draft_session.install_prepared_replacement(
                    program_replacement
                )
                deployment_changed = (
                    state.deployment_draft_session.install_prepared_replacement(
                        deployment_replacement
                    )
                )
                return program_changed or deployment_changed

            commit = commit_local_authoring(
                state,
                body,
                operation_kind="load_scenario_document",
                session="whole-workspace",
                project=scenario_project_facts(
                    scenario_document,
                    document_path=source_path,
                    source_document_digest=source_digest,
                    source_diagnostics=source_diagnostics,
                ),
                install=install_load,
            )
            activation = None
            if activation_intent is not None and activation_prepared is not None:
                coordinator = state.authoring_admission
                if coordinator is None:
                    raise ScenarioArtifactError(
                        "scenario_activation_unavailable",
                        "Scenario activation requires authoring admission",
                        status_code=503,
                    )
                activation_intent["expected_admission_generation"] = (
                    commit.admission_generation
                )
                manager_apply = activation_prepared.get("manager_apply")
                if isinstance(manager_apply, dict):
                    manager_apply["expected_admission_generation"] = (
                        commit.admission_generation
                    )
                activation_envelope = AuthoringMutationEnvelope(
                    expected_admission_generation=commit.admission_generation,
                    request_owner_epoch=body.request_owner_epoch,
                    request_id=body.request_id + ".activate",
                )
                committed_identity = commit.snapshot.identity()

                def validate_activation(_claim: dict[str, Any]) -> None:
                    if state.authoring_workspace.identity() != committed_identity:
                        raise ScenarioArtifactError(
                            "scenario_activation_workspace_changed",
                            "Scenario workspace changed before activation submission",
                            status_code=409,
                        )

                activation = coordinator.execute_external_submission(
                    activation_envelope,
                    operation_kind="activate_scenario_document",
                    draft_id=commit.snapshot.program["draftId"],
                    draft_revision=commit.snapshot.program["draftRevision"],
                    payload={
                        "source_document_digest": source_digest,
                        "captured_input_digest": activation_intent[
                            "captured_input_digest"
                        ],
                    },
                    validate_under_claim=validate_activation,
                    submit=lambda claim: state.execute_authored_deployment_candidate(
                        activation_intent,
                        claim,
                        apply=True,
                        prepared=activation_prepared,
                    ),
                )
            return {
                "status": "accepted" if activation is not None else "opened",
                "activation": activation,
                "diagnostics": diagnostics,
                "source_diagnostics": source_diagnostics,
                "path": source_path,
                "scenario_id": scenario_document_id,
                "incomplete": incomplete,
                "workspace": state.draft_session.workspace_snapshot(
                    applied_program_revision=None
                ),
                "authoringWorkspace": commit.snapshot.as_dict(),
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
            # Export reads one immutable composite capture and never changes the
            # workspace, so it takes no local commit and no saved baseline.
            snapshot = state.authoring_workspace.snapshot()
            _require_captured_workspace_fence(body, snapshot.identity)
            captured_workspace = snapshot.as_dict()
            project = captured_workspace["project"]
            source_path = project["document_path"]
            if not isinstance(source_path, str):
                raise ScenarioArtifactError(
                    "scenario_export_source_unavailable",
                    "Save the Scenario project before exporting it",
                    status_code=409,
                )
            document = build_scenario_document(
                scenario_id=(
                    project["document_id"]
                    if isinstance(project["document_id"], str)
                    else Path(source_path).name
                ),
                name=(
                    project["document_name"]
                    if isinstance(project["document_name"], str)
                    else Path(source_path).name
                ),
                description=(
                    project["document_description"]
                    if isinstance(project["document_description"], str)
                    else ""
                ),
                program_draft=captured_workspace["program"]["authoring"],
                deployment_draft=captured_workspace["deployment"]["document"],
                program_role_assignment=project["program_role_assignment"],
                typed_parameter_overrides=project["typed_parameter_overrides"],
                manyforge_version=_version(),
                last_known_status="unknown",
            )
            destination = str(Path(body.destination_path).expanduser().resolve())
            if _published_content_digest(state, destination) == (
                _document_content_digest(document)
            ):
                # The exact content is already published at the destination, so
                # a lost response is resolved without repeating the export.
                return {"status": "exported", "path": destination}
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


def load_exact_scenario_document(
    state: ComposerState, body: LoadScenarioDocumentRequest
) -> dict[str, Any]:
    """Use the one Scenario-document Open/activate implementation at startup.

    Startup is intentionally not a second loader. Constructing the bounded
    router only binds its handlers; invoking this exact handler performs the
    same validation, workspace CAS, admission claim and Manager submission as
    the public API without starting an HTTP client against ourselves.
    """

    router = create_scenario_document_router(state)
    route = next(
        route
        for route in router.routes
        if isinstance(route, APIRoute)
        and route.path == "/api/scenario-documents/load"
    )
    return route.endpoint(body)


__all__ = [
    "LoadScenarioDocumentRequest",
    "create_scenario_document_router",
    "load_exact_scenario_document",
]
