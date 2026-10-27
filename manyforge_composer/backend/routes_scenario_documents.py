"""Editable Scenario Save/Load routes over existing authoring owners."""

from __future__ import annotations

from copy import deepcopy
import hashlib
from pathlib import Path
from typing import Any, Literal, NoReturn
import uuid

from fastapi import APIRouter, HTTPException, Query
from fastapi.routing import APIRoute
from manyforge_behavior.program import Program
from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    capture_deployment_model_sources,
)
from manyforge_headless_api.client import HeadlessClientError
from pydantic import BaseModel, ConfigDict, Field

from .authoring_admission import (
    AuthoringMutationEnvelope,
    OperationEffects,
    draft_session_http_exception,
    headless_client_http_exception,
    raise_operation_failure,
    raise_save_failure,
    scenario_artifact_http_exception,
)
from .authoring_forest import (
    AuthoringForestError,
    derive_candidate,
    empty_authoring,
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
    commit_draft_edit,
    commit_local_authoring,
    require_captured_workspace_identity,
)
from .deployment_draft import DeploymentDraftError
from .draft_session import DraftSessionError
from .routes_deployment_draft import (
    _workspace as deployment_workspace,
    prepare_deployment_discard,
)
from .routes_draft import _workspace as program_workspace, prepare_program_discard
from .scenario_artifacts import (
    SCENARIO_NAME_PATTERN,
    SCENARIO_PROJECT_MEMBERS,
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    capture_scenario_artifact_with_diagnostics,
    discover_scenario_artifacts,
    export_scenario_artifact,
    prepare_scenario_artifact,
    prepared_scenario_source_digest,
    save_scenario_artifact,
    scenario_project_path,
    scenario_project_stem,
)
from .state import ComposerState, canonical_project_path


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
    """Save the Scenario as ``<directory>/<name>.scenario``.

    The one Scenario name is also written as its scenario_id and manifest name;
    the description stays free text.
    """

    directory: str = Field(min_length=1, max_length=4096)
    name: str = Field(pattern=SCENARIO_NAME_PATTERN)
    description: str = Field(default="", max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class LoadScenarioDocumentRequest(_ScenarioFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class NewScenarioDocumentRequest(_ScenarioFence):
    """Create ``<directory>/<name>.scenario``; the name is also its scenario_id."""

    directory: str = Field(min_length=1, max_length=4096)
    name: str = Field(pattern=SCENARIO_NAME_PATTERN)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")
    deployment_choice: Literal["empty"] = Field(alias="deploymentChoice")
    expected_retained_bundle_id: str = Field(alias="expectedRetainedBundleId", min_length=1)
    expected_graph_revision: str = Field(alias="expectedGraphRevision", min_length=1)


class DiscardScenarioRequest(_ScenarioFence):
    """Return every Draft with unapplied changes to its Applied state."""


class ExportScenarioDocumentRequest(_ScenarioFence):
    destination_path: str = Field(
        alias="destinationPath", min_length=1, max_length=4096
    )


def _raise(error: Exception) -> NoReturn:
    if isinstance(error, ModelSourceCaptureError):
        raise scenario_artifact_http_exception(ScenarioArtifactError(
            "scenario_model_source_unavailable", str(error), status_code=409,
        )) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    if isinstance(error, ScenarioArtifactError):
        raise scenario_artifact_http_exception(error) from error
    if isinstance(error, DeploymentDraftError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, DraftSessionError):
        raise draft_session_http_exception(error) from error
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
    if isinstance(error, AuthoringWorkspaceError):
        detail: dict[str, Any] = {"code": error.code, "message": str(error)}
        if isinstance(error.detail, dict) and "workspace" in error.detail:
            detail["workspace"] = error.detail["workspace"]
        raise HTTPException(status_code=error.status_code, detail=detail) from error
    raise error


def _deployment_unapplied(view: dict[str, Any]) -> bool:
    """Something is applied and the Deployment Draft is not it.

    The publication identity (revision) decides when both sides carry one;
    otherwise the backend's document comparison (run admission) decides.
    """

    draft = view.get("draft") or {}
    applied = (view.get("managerApplied") or {}).get("deploymentRevision")
    if draft.get("document") is None or applied is None:
        return False
    revision = draft.get("deploymentRevision")
    if revision is not None:
        return revision != applied
    return (view.get("runAdmission") or {}).get("ready") is not True


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
        saved_path: str | None = None
        # The one effect record of this Save: the HTTP outcome and the claim
        # settlement both come from it, never from an error code.
        effects = OperationEffects()
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
            complete = (
                isinstance(authoring, dict)
                and isinstance(deployment, dict)
                and isinstance(assignment, dict)
            )
            # Ready only when the saved content is exactly what is Applied:
            # both Applied views read at this captured identity, both Drafts
            # match an applied counterpart.
            ready = False
            if complete:
                program_view = program_workspace(state)
                deployment_view = deployment_workspace(state)
                ready = (
                    program_view["authoringWorkspace"] == snapshot.identity.as_dict()
                    and deployment_view["authoringWorkspace"] == snapshot.identity.as_dict()
                    and program_view.get("hasUnappliedChanges") is False
                    and deployment_view["managerApplied"]["deploymentRevision"] is not None
                    and not _deployment_unapplied(deployment_view)
                )
            target = scenario_project_path(body.directory, body.name)
            document = build_scenario_document(
                scenario_id=body.name,
                name=body.name,
                description=body.description.strip(),
                program_draft=authoring,
                deployment_draft=deployment,
                program_role_assignment=assignment,
                manyforge_version=_version(),
                last_known_status=(
                    "ready" if ready else "unknown" if complete else "incomplete"
                ),
            )
            current_target = project["document_path"]
            source_project_root = (
                state.deployment_draft_session.source_project_root
                or current_target
            )
            overwrite = body.confirm_overwrite or (
                current_target is not None
                and str(Path(target).expanduser().resolve()) == current_target
            )
            program_save = (
                state.draft_session.prepare_save(
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                    target_path=str(Path(target) / SCENARIO_PROJECT_MEMBERS["program_draft"]),
                    confirm_overwrite=True,
                )
                if captured_workspace["program"]["candidateAvailable"] else None
            )
            empty_program_save = None
            if (isinstance(authoring, dict) and authoring.get("forest") == []
                    and not authoring.get("scene")):
                retained = state.draft_session.retained_state()
                retained.update(
                    saved_path=str(Path(target) / SCENARIO_PROJECT_MEMBERS["program_draft"]),
                    saved_program_revision=None, saved_document=deepcopy(authoring),
                )
                empty_program_save = state.draft_session.prepare_load_replacement(retained, state.node_catalog)
            deployment_save = (
                state.deployment_draft_session.prepare_save(
                    draft_id=body.deployment_draft_id,
                    expected_revision=body.expected_deployment_draft_revision,
                    target_path=str(Path(target) / SCENARIO_PROJECT_MEMBERS["deployment_draft"]),
                    confirm_overwrite=True,
                )
                if deployment is not None else None
            )

            def install_saved_baselines() -> bool:
                assert saved_path is not None
                program_changed = (
                    state.draft_session.install_prepared_save(
                        program_save,
                        saved_path=str(Path(saved_path) / SCENARIO_PROJECT_MEMBERS["program_draft"]),
                    ) if program_save is not None else False
                )
                if empty_program_save is not None:
                    program_changed = state.draft_session.install_prepared_replacement(empty_program_save)
                deployment_changed = (
                    state.deployment_draft_session.install_prepared_save(
                        deployment_save,
                        saved_path=str(Path(saved_path) / SCENARIO_PROJECT_MEMBERS["deployment_draft"]),
                    ) if deployment_save is not None else False
                )
                return program_changed or deployment_changed

            def prepare_saved_project() -> dict[str, Any]:
                nonlocal saved_path
                if _published_content_digest(state, target) == _document_content_digest(
                    document
                ):
                    # Resolve a prior completed write without repeating its
                    # filesystem effect. This check runs after admission is
                    # acquired, so stale requests cannot create a Save As file.
                    saved_path = str(Path(target).expanduser().resolve())
                else:
                    saved_path = save_scenario_artifact(
                        document,
                        target,
                        allowed_roots=state.allowed_fs_roots,
                        overwrite=overwrite,
                        source_project_path=source_project_root,
                        effects=effects,
                    )
                # Readback of the published project; its failure is a
                # definite "written but not adopted" outcome.
                captured, _, source_digest = capture_scenario_artifact(
                    saved_path, allowed_roots=state.allowed_fs_roots
                )
                return scenario_project_facts(
                    captured,
                    document_path=saved_path,
                    source_document_digest=source_digest,
                )

            # Acquire the local authoring claim before publishing the file. A
            # stale admission generation must not leave a new Scenario behind
            # while reporting that Save was refused. The owner installs the
            # resulting project facts in the same composite commit.
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="save_scenario_document",
                captured_identity=snapshot.identity,
                session="whole-workspace",
                install=install_saved_baselines,
                prepare_project_under_claim=prepare_saved_project,
                effects=effects,
            )
        except Exception as error:
            # Not saved: the marker. Saved but not read back or adopted: the
            # definite outcome naming the written project, so newer in-memory
            # content is never labelled Saved. Otherwise unverified.
            raise_save_failure(
                error,
                effects,
                _raise,
                code="scenario_saved_baseline_not_adopted",
                document="Scenario project",
            )
        # The project is written and adopted as Saved from here on.
        try:
            if saved_path is None:
                raise ScenarioArtifactError(
                    "scenario_saved_path_unavailable",
                    "Scenario Save committed without a published path",
                    status_code=503,
                )
            workspace = program_workspace(state)
        except Exception as error:
            _raise(error)
        return {
            "status": "saved",
            "path": saved_path,
            "scenario_id": body.name,
            "last_known_status": document["saved_status"]["last_known_status"],
            "workspace": workspace,
        }

    @router.post("/api/scenario-documents/load")
    def load_document(body: LoadScenarioDocumentRequest) -> dict[str, Any]:
        return activate_document(body)

    @router.post("/api/scenario-documents/new")
    def new_document(body: NewScenarioDocumentRequest) -> dict[str, Any]:
        return activate_document(body)

    def activate_document(
        body: LoadScenarioDocumentRequest | NewScenarioDocumentRequest,
    ) -> dict[str, Any]:
        committed_path: str | None = None
        created_path: str | None = None
        committed_snapshot = None
        activation: dict[str, Any] | None = None
        creating = isinstance(body, NewScenarioDocumentRequest)
        effects = OperationEffects()
        try:
            _require_fence(state, body)
            captured_workspace = state.authoring_workspace.snapshot()
            _require_captured_workspace_fence(body, captured_workspace.identity)
            authoring_catalog = state.node_catalog
            if creating:
                if not body.confirm_discard:
                    raise ScenarioArtifactError("scenario_discard_confirmation_required",
                        "New closes the current Scenario and clears its Scene; confirmation is required", status_code=409)
                new_path = scenario_project_path(body.directory, body.name)
                if state.headless_client is None:
                    raise ScenarioArtifactError(
                        "scenario_activation_unavailable",
                        "New Scenario requires the configured Manager",
                        status_code=503,
                    )
                _require_scenario_closed(state, state.headless_client.manager_status(), creating=True)
                deployment = {
                    "api_version": "manyforge.deployment_intent.v2",
                    "deployment_artifact_id": "empty_scenario",
                    "name": "Empty Deployment",
                    "components": [], "attachments": [], "policies": {},
                }
                authoring = empty_authoring(authoring_catalog)
                authoring["name"] = body.name
                # One identifier: the new project's folder stem is its id.
                scenario_document = build_scenario_document(
                    scenario_id=body.name,
                    name=body.name, description="", program_draft=authoring,
                    deployment_draft=deployment,
                    program_role_assignment={"component_roles": [], "target_roles": []},
                    manyforge_version=_version(),
                    last_known_status="incomplete",
                )
                scenario_document, target, members, manifest = prepare_scenario_artifact(
                    scenario_document, new_path, allowed_roots=state.allowed_fs_roots,
                )
                if target.exists() or target.is_symlink():
                    raise ScenarioArtifactError("scenario_destination_exists", "New requires a new project folder", status_code=409)
                source_path = str(target)
                source_digest = prepared_scenario_source_digest(members, manifest)
                source_diagnostics = []
            else:
                (scenario_document, source_path, source_digest, source_diagnostics) = capture_scenario_artifact_with_diagnostics(
                    body.path, allowed_roots=state.allowed_fs_roots)
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
            no_program = authoring is not None and authoring.get("forest") == [] and not authoring.get("scene")
            incomplete = (
                (program is None and not no_program)
                or deployment is None
                or scenario_document["program_role_assignment"] is None
            )
            if incomplete or not isinstance(deployment, dict):
                raise ScenarioArtifactError(
                    "scenario_activation_incomplete",
                    "only a complete v2 Scenario document can be loaded",
                    status_code=409,
                )
            # Load establishes Saved from the exact captured source, independently
            # of whether the subsequent Apply succeeds.
            post_state["program"].update(
                saved_path=str(Path(source_path) / SCENARIO_PROJECT_MEMBERS["program_draft"]),
                saved_program_revision=program.program_revision if program is not None else None,
                saved_document=program.to_dict() if program is not None else deepcopy(authoring),
            )
            post_state["deployment"].update(
                saved_path=str(Path(source_path) / SCENARIO_PROJECT_MEMBERS["deployment_draft"]),
                saved_document=deepcopy(deployment),
            )
            if state.headless_client is None:
                raise ScenarioArtifactError(
                    "scenario_activation_unavailable",
                    "Scenario Load requires the configured Manager",
                    status_code=503,
                )
            source_closure = capture_deployment_model_sources(
                deployment, project_root=Path(source_path)
            )
            assignment = deepcopy(scenario_document["program_role_assignment"])
            # The project folder scopes the Scenario's persistent values.
            project_path = canonical_project_path(source_path)
            captured_digest = raw_sha256_digest(
                canonical_json_bytes(
                    {
                        "scenario_id": scenario_document["scenario_id"],
                        "project_path": project_path,
                        "source_document_digest": source_digest,
                        "deployment": deployment,
                        "model_source_closure": source_closure,
                        "program": program.to_dict() if program is not None else None,
                        "program_role_assignment": assignment,
                    }
                )
            )
            manager_status = state.headless_client.manager_status()
            _require_scenario_closed(state, manager_status, creating=creating)
            bundle, graph, _epoch = state._manager_current(manager_status)
            if creating and (body.expected_retained_bundle_id != bundle or body.expected_graph_revision != graph):
                raise ScenarioArtifactError("scenario_activation_preview_changed",
                    "Applied Deployment changed since New was opened; review the choice again", status_code=409)
            activation_intent: dict[str, Any] = {
                "scene_initialization": "initialize",
                "scenario_id": scenario_document["scenario_id"],
                "project_path": project_path,
                "source_document_digest": source_digest,
                "captured_input_digest": captured_digest,
                "deployment": deepcopy(deployment),
                "model_source_closure": source_closure,
                "program": program.to_dict() if program is not None else None,
                "program_authoring": deepcopy(authoring),
                "program_role_assignment": assignment,
                "expected_manager_current": {
                    "retained_bundle_id": bundle,
                    "graph_revision": graph,
                },
                "expected_admission_generation": body.expected_admission_generation,
                "manager_request_id": "scenario-document-activation-"
                + hashlib.sha256(
                    f"{body.request_owner_epoch}:{body.request_id}".encode()
                ).hexdigest()[:32],
                "correlation_id": body.request_id,
                "confirmation_token": None,
                "draft_id": body.draft_id,
                "draft_revision": body.expected_draft_revision,
            }
            # Load is the operator's deliberate confirmation to replace the
            # current Scenario and activate this exact captured candidate, so
            # its one preparation already accepts a disruptive or destructive
            # impact. Manager rechecks the exact preview receipt and current
            # publication when it accepts the operation.
            activation_preview = state.execute_authored_deployment_candidate(
                activation_intent, None, apply=True, prepare_only=True,
                confirmed=True,
            )
            if creating:
                # New begins only from the owner-proved canonical empty cell.
                # Its empty candidate cannot introduce a component effect.
                preview = activation_preview.get("candidate_preview")
                impact = preview.get("impact") if isinstance(preview, dict) else None
                changes = impact.get("components") if isinstance(impact, dict) else None
                if not isinstance(changes, list) or changes:
                    raise ScenarioArtifactError(
                        "scenario_new_impact_changed",
                        "New requires an empty component impact after Close",
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
                    post_state["deployment"],
                    source_project_root=source_path,
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

            coordinator = state.authoring_admission
            if coordinator is None:
                raise ScenarioArtifactError(
                    "scenario_activation_unavailable",
                    "Scenario Load requires authoring admission",
                    status_code=503,
                )
            activation_prepared: dict[str, Any] | None = None

            def validate_load(claim: dict[str, Any]) -> None:
                nonlocal activation_prepared
                acquired_generation = claim.get("acquired_admission_generation")
                if type(acquired_generation) is not int or acquired_generation < 1:
                    raise ScenarioArtifactError(
                        "scenario_activation_claim_invalid",
                        "Authoring claim omitted its acquired admission generation",
                        status_code=503,
                    )
                _require_captured_workspace_fence(
                    body, state.authoring_workspace.identity()
                )
                current_program = state.draft_session.retained_state()
                current_deployment = state.deployment_draft_session.retained_state()
                has_unsaved_drafts = (
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
                )
                if (
                    has_unsaved_drafts
                    and not state.current_drafts_match_remembered_scenario()
                    and not body.confirm_discard
                ):
                    raise ScenarioArtifactError(
                        "scenario_discard_confirmation_required",
                        "Loading would replace unsaved Program or Deployment Draft changes",
                        status_code=409,
                    )
                # Preparation and Manager preview are effect-free and belong
                # to the idle generation in the claim intent. Re-preparing
                # after acquisition is invalid: the claim deliberately closes
                # idle admission. Manager rechecks the exact preview receipt
                # and current publication when it accepts the operation.
                current_bundle, current_graph, _ = state._manager_current(
                    state.headless_client.manager_status()
                )
                if activation_intent["expected_manager_current"] != {
                    "retained_bundle_id": current_bundle,
                    "graph_revision": current_graph,
                }:
                    raise ScenarioArtifactError(
                        "scenario_activation_preview_changed",
                        "Deployment preview changed before the workspace commit",
                        status_code=409,
                    )
                activation_prepared = activation_preview

            def commit_load() -> None:
                nonlocal committed_path, committed_snapshot, created_path
                if creating:
                    created_path = save_scenario_artifact(
                        scenario_document, source_path, allowed_roots=state.allowed_fs_roots,
                        overwrite=False, expected_source_digest=source_digest,
                        effects=effects,
                    )
                committed_snapshot = state.authoring_workspace.replace(
                    expected_identity=captured_workspace.identity,
                    install=install_load,
                    project=scenario_project_facts(
                        scenario_document,
                        document_path=source_path,
                        source_document_digest=source_digest,
                        source_diagnostics=source_diagnostics,
                    ),
                    notify=False,
                )
                committed_path = source_path
                # An opened or created Scenario supersedes a refused startup project.
                state.startup_scenario_notice = None

            def submit_load(claim: dict[str, Any]) -> dict[str, Any]:
                nonlocal activation
                if activation_prepared is None:
                    raise ScenarioArtifactError(
                        "scenario_activation_preparation_missing",
                        "Scenario Load candidate was not validated under its claim",
                        status_code=503,
                    )
                activation = state.execute_authored_deployment_candidate(
                    activation_intent,
                    claim,
                    apply=True,
                    prepared=activation_prepared,
                )
                return activation

            activation = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind="new_scenario_document" if creating else "load_scenario_document",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload={
                    "path": source_path,
                    "source_document_digest": source_digest,
                    "captured_input_digest": captured_digest,
                    "scenario_content_digest": _document_content_digest(
                        scenario_document
                    ),
                },
                validate_under_claim=validate_load,
                submit=submit_load,
                commit_before_submit=commit_load,
                workspace=state.authoring_workspace,
                after_release=state.authoring_workspace.publish_commit_wake,
                effects=effects,
            )
            workspace = program_workspace(state)
            return {
                "status": activation["status"],
                "activation": activation,
                "diagnostics": diagnostics,
                "source_diagnostics": source_diagnostics,
                "path": source_path,
                "scenario_id": scenario_document_id,
                "incomplete": False,
                "workspace": workspace,
                # The exact composite post-image this Load/New committed, as
                # every other authoring route returns it (not its identity).
                "authoringWorkspace": committed_snapshot.as_dict(),
            }
        except Exception as error:
            if created_path is None and effects.published_path is not None:
                created_path = effects.published_path
            if created_path is not None:
                raise HTTPException(status_code=503, detail={
                    "code": "scenario_created_not_activated",
                    "message": "The new Scenario was saved, but activation was not confirmed. No automatic retry was made.",
                    "path": created_path,
                    "cause": error.detail if isinstance(error, HTTPException) else str(error),
                    "activation": activation,
                    "workspace": state.authoring_workspace.identity().as_dict(),
                }) from error
            if committed_path is not None and committed_snapshot is not None:
                # The local workspace was committed before Manager submission.
                # Preserve that observable partial result; never report Load as
                # complete or silently roll it back when Manager rejects or its
                # acceptance cannot be observed.
                if getattr(error, "code", None) == "manager_apply_rejected":
                    raise HTTPException(
                        status_code=409,
                        detail={
                            "code": "manager_apply_rejected",
                            "message": (
                                "The Scenario workspace was replaced, but "
                                f"{error}. Refresh the workspace before another action."
                            ),
                            "path": committed_path,
                            "managerOutcome": "rejected",
                            "causeCode": "manager_apply_rejected",
                            "workspace": committed_snapshot.identity.as_dict(),
                        },
                    ) from error
                manager_outcome = (
                    "accepted"
                    if isinstance(activation, dict)
                    and activation.get("status") == "accepted"
                    else "applied"
                    if isinstance(activation, dict)
                    and activation.get("status") == "applied"
                    else "not_confirmed"
                )
                raise HTTPException(
                    status_code=503,
                    detail={
                        "code": "scenario_load_activation_not_confirmed",
                        "message": "The Scenario workspace was replaced, but Manager activation was not confirmed in this response. Inspect Manager operation status and refresh the workspace before retrying.",
                        "path": committed_path,
                        "managerOutcome": manager_outcome,
                        "causeCode": getattr(error, "code", None),
                        "workspace": committed_snapshot.identity.as_dict(),
                    },
                ) from error
            raise_operation_failure(error, effects, _raise)

    @router.post("/api/scenario-documents/export")
    def export_document(body: ExportScenarioDocumentRequest) -> dict[str, Any]:
        effects = OperationEffects()
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
                    else scenario_project_stem(source_path)
                ),
                name=(
                    project["document_name"]
                    if isinstance(project["document_name"], str)
                    else scenario_project_stem(source_path)
                ),
                description=(
                    project["document_description"]
                    if isinstance(project["document_description"], str)
                    else ""
                ),
                program_draft=captured_workspace["program"]["authoring"],
                deployment_draft=captured_workspace["deployment"]["document"],
                program_role_assignment=project["program_role_assignment"],
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
                source_project_path=(
                    state.deployment_draft_session.source_project_root or source_path
                ),
                allowed_roots=state.allowed_fs_roots,
                effects=effects,
            )
            return {"status": "exported", "path": exported_path}
        except Exception as error:
            raise_operation_failure(error, effects, _raise)

    @router.post("/api/scenario-documents/discard")
    def discard_scenario(body: DiscardScenarioRequest) -> dict[str, Any]:
        """Discard the unapplied Draft changes of the whole Scenario.

        Each document with unapplied changes is returned to its Applied state,
        in one Draft-only whole-workspace commit fenced on the client's
        workspace identity: one publication, or a failed install contains the
        workspace. Nothing reaches the Manager.
        """

        try:
            # Every Draft fence too: a stale one inside the install would
            # contain the workspace instead of refusing.
            _require_captured_workspace_fence(body, state.authoring_workspace.identity())
            steps = []
            if _deployment_unapplied(deployment_workspace(state)):
                steps.append(prepare_deployment_discard(
                    state, body.deployment_draft_id, body.expected_deployment_draft_revision
                ))
            if program_workspace(state).get("hasUnappliedChanges") is True:
                steps.append(prepare_program_discard(
                    state, body.draft_id, body.expected_draft_revision
                ))
            if not steps:
                raise DraftSessionError(
                    "scenario_discard_nothing_to_discard",
                    "Nothing to discard: every Draft matches Applied",
                    status_code=409,
                )

            def validate() -> None:
                for step_validate, _install in steps:
                    step_validate()

            def install() -> bool:
                changed = False
                for _validate, step_install in steps:
                    changed = step_install() or changed
                return changed

            commit_draft_edit(
                state,
                body,
                operation_kind="discard_scenario",
                session="scenario",
                validate=validate,
                install=install,
            )
            # The Deployment workspace follows the shared publication; the
            # Program workspace carries its own Draft-matched identity.
            return program_workspace(state)
        except Exception as error:
            _raise(error)

    return router


def _require_scenario_closed(
    state: ComposerState, manager_status: dict[str, Any], *, creating: bool
) -> None:
    """New and every Load start only after the current owner proves Close.

    The proof comes from the same current Manager head used for preview. A
    Load of the Scenario that is currently applied is no exception: Load
    never reapplies over a current Scenario.
    """
    state._manager_current(manager_status)  # Existing freshness/identity checks.
    canonical_empty = manager_status["status"]["active"].get("canonical_empty")
    if type(canonical_empty) is not bool:
        raise ScenarioArtifactError(
            "scenario_load_current_configuration_unavailable",
            ("New Scenario" if creating else "Scenario Load")
            + " requires the Manager's current canonical-empty proof",
            status_code=409,
        )
    if not canonical_empty:
        raise ScenarioArtifactError(
            "scenario_close_required",
            "Close first before creating a new scenario" if creating
            else "Close first before loading a scenario",
            status_code=409,
        )


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
