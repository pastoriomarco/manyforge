"""Editable Scenario Save/Load routes over existing authoring owners."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from typing import Any, NoReturn
import uuid

from fastapi import APIRouter, HTTPException, Query
from manyforge_behavior.program import Program, ProgramValidationError
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .authoring_admission import (
    AuthoringMutationEnvelope,
    headless_client_http_exception,
)
from .authoring_forest import (
    AuthoringForestError,
    derive_candidate,
    from_program,
    validate_invariants,
)
from .authoring_transaction_journal import AuthoringJournalError, canonical_digest
from .deployment_draft import DeploymentDraftError, compile_deployment_document
from .draft_session import DraftSessionError
from .scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    discover_scenario_artifacts,
    load_scenario_artifact,
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
    path: str | None = Field(default=None, min_length=1, max_length=4096)
    qualified_scenario_id: str | None = Field(
        default=None,
        alias="qualifiedScenarioId",
        pattern=r"^[a-z][a-z0-9._-]{0,127}$",
    )
    alternate_deployment_path: str | None = Field(
        default=None, alias="alternateDeploymentPath", max_length=4096
    )
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")

    @model_validator(mode="after")
    def require_one_scenario_source(self) -> "LoadScenarioDocumentRequest":
        if (self.path is None) == (self.qualified_scenario_id is None):
            raise ValueError("exactly one Scenario path or qualified Scenario is required")
        return self


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
    try:
        from manyforge_headless_api.client import HeadlessClientError

        if isinstance(error, HeadlessClientError):
            raise headless_client_http_exception(error) from error
    except ImportError:
        pass
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


def _catalog_rows(state: ComposerState) -> tuple[str | None, list[dict[str, Any]]]:
    if state.headless_client is None:
        return None, []
    catalog = state.headless_client.scenarios()
    revision = catalog.get("scenario_catalog_revision")
    rows = catalog.get("scenarios")
    return (
        revision if isinstance(revision, str) else None,
        [deepcopy(row) for row in rows if isinstance(row, dict)]
        if isinstance(rows, list)
        else [],
    )


def _base_scenario_for_deployment(
    state: ComposerState,
    deployment_artifact_id: str,
    preferred: str | None,
) -> tuple[str, str]:
    revision, rows = _catalog_rows(state)
    if revision is None:
        raise ScenarioArtifactError(
            "scenario_catalog_unavailable",
            "The qualified Scenario catalog is unavailable",
            status_code=503,
        )
    available = [
        row.get("scenario_id")
        for row in rows
        if isinstance(row.get("scenario_id"), str)
    ]
    if preferred is not None:
        if preferred in available:
            return preferred, revision
        raise ScenarioArtifactError(
            "scenario_base_unavailable",
            f"The recorded base Scenario is not installed: {preferred}",
            status_code=409,
        )
    matches = [
        row.get("scenario_id")
        for row in rows
        if row.get("deployment_artifact_id") == deployment_artifact_id
        and isinstance(row.get("scenario_id"), str)
    ]
    if len(matches) == 1:
        return matches[0], revision
    if not matches:
        raise ScenarioArtifactError(
            "scenario_deployment_template_unavailable",
            "No qualified Scenario can compile this Deployment artifact",
            status_code=409,
        )
    raise ScenarioArtifactError(
        "scenario_deployment_template_ambiguous",
        "The Deployment matches multiple qualified Scenarios; select its base explicitly",
        status_code=409,
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
            catalog_revision, catalog_rows = _catalog_rows(state)
            qualified = [
                {
                    "source_kind": "qualified",
                    "path": None,
                    "scenario_id": row.get("scenario_id"),
                    "name": row.get("scenario_id"),
                    "description": row.get("description") or "",
                    "base_scenario_id": row.get("scenario_id"),
                    "saved_manyforge_version": None,
                    "display_status": "ready",
                    "status_detail": "Qualified by the installed Scenario catalog.",
                }
                for row in catalog_rows
                if isinstance(row.get("scenario_id"), str)
            ]
            return {
                "api_version": "manyforge.scenario_document_index.v1",
                "directory": current_directory,
                "catalog_revision": catalog_revision,
                "entries": [*saved, *qualified],
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/save")
    def save_document(body: SaveScenarioDocumentRequest) -> dict[str, Any]:
        if state.authoring_admission is None:
            raise HTTPException(
                status_code=503,
                detail={
                    "code": "scenario_save_unavailable",
                    "message": "Scenario Save requires Headless-backed authoring",
                },
            )
        try:
            _require_fence(state, body)
            program_state = state.draft_session.retained_state()
            deployment_state = state.deployment_draft_session.retained_state()
            current = state.current_scenario().get("current_scenario")
            base = (
                current.get("scenario_id")
                if isinstance(current, dict)
                else state.scenario_base_scenario_id
            )
            program = program_state.get("program")
            deployment = deployment_state.get("document")
            ready = bool(
                isinstance(current, dict)
                and isinstance(program, dict)
                and isinstance(deployment, dict)
                and program.get("program_revision") == current.get("program_revision")
                and deployment.get("deployment_revision")
                == current.get("deployment_revision")
            )
            last_known_status = (
                "ready"
                if ready
                else "unknown"
                if isinstance(program, dict) and isinstance(deployment, dict)
                else "incomplete"
            )
            document = build_scenario_document(
                scenario_id=body.scenario_id,
                name=body.name.strip(),
                description=body.description.strip(),
                base_scenario_id=base,
                program_draft=program_state.get("authoring"),
                deployment_draft=deployment,
                manyforge_version=_version(),
                last_known_status=last_known_status,
            )
            target = normalize_scenario_save_path(body.path)
            current_target = state.scenario_document_path
            overwrite = body.confirm_overwrite or (
                current_target is not None
                and str(Path(target).expanduser().resolve()) == current_target
            )
            intent = {
                "schema_version": "manyforge.composer_authoring_effect.v1",
                "operation_kind": "save_scenario_document",
                "effect_class": "external",
                "external_kind": "save_scenario_document",
                "target_path": target,
                "overwrite": overwrite,
                "scenario_document": document,
                "post_state": state.authoring_retained_state(),
            }

            def validate(_claim: dict[str, Any]) -> None:
                _require_fence(state, body)
                if (
                    state.draft_session.retained_state() != program_state
                    or state.deployment_draft_session.retained_state()
                    != deployment_state
                ):
                    raise ScenarioArtifactError(
                        "scenario_draft_stale",
                        "Program or Deployment Draft changed before Save",
                        status_code=409,
                    )

            def effect(_claim: dict[str, Any]) -> dict[str, Any]:
                saved_path = save_scenario_artifact(
                    document,
                    target,
                    allowed_roots=state.allowed_fs_roots,
                    overwrite=overwrite,
                )
                state.scenario_document_path = saved_path
                state.scenario_base_scenario_id = base
                state.remember_scenario_document(document)
                return {
                    "status": "saved",
                    "path": saved_path,
                    "scenario_id": body.scenario_id,
                    "last_known_status": document["saved_status"]["last_known_status"],
                    "workspace": state.draft_session.workspace_snapshot(
                        applied_program_revision=(
                            current.get("program_revision")
                            if isinstance(current, dict)
                            else None
                        )
                    ),
                }

            return state.authoring_admission.execute(
                body.envelope(),
                operation_kind="save_scenario_document",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                prepare_effect=lambda: intent,
                validate_under_claim=validate,
                effect=effect,
                post_image=state.authoring_retained_state,
            )
        except Exception as error:
            _raise(error)

    @router.post("/api/scenario-documents/load")
    def load_document(body: LoadScenarioDocumentRequest) -> dict[str, Any]:
        if state.authoring_admission is None or state.headless_client is None:
            raise HTTPException(
                status_code=503,
                detail={
                    "code": "scenario_load_unavailable",
                    "message": "Scenario Load requires Headless-backed authoring",
                },
            )
        prepared: dict[str, Any] = {}

        def prepare() -> dict[str, Any]:
            _require_fence(state, body)
            qualified_catalog_revision: str | None = None
            qualified_selection: dict[str, Any] | None = None
            authoring_catalog = state.node_catalog
            scenario_document: dict[str, Any] | None = None
            if body.path is not None:
                scenario_document, source_path = load_scenario_artifact(
                    body.path, allowed_roots=state.allowed_fs_roots
                )
                scenario_document_id = scenario_document["scenario_id"]
                authoring = scenario_document["program_draft"]
                deployment_document = scenario_document["deployment_draft"]
                base = scenario_document["base_scenario_id"]
            else:
                assert body.qualified_scenario_id is not None
                qualified_selection = state.headless_client.scenario_load_selection(
                    body.qualified_scenario_id
                )
                try:
                    selected_program = Program.from_dict(
                        deepcopy(qualified_selection["program"]),
                        authoring_catalog,
                        path=f"<qualified-scenario:{body.qualified_scenario_id}>",
                    )
                except ProgramValidationError as error:
                    raise ScenarioArtifactError(
                        "scenario_program_draft_invalid",
                        f"qualified Scenario {body.qualified_scenario_id}: "
                        f"program: {error}",
                        status_code=409,
                    ) from error
                scenario_document_id = body.qualified_scenario_id
                authoring = from_program(selected_program)
                deployment_document = deepcopy(qualified_selection["deployment"])
                source_path = None
                base = body.qualified_scenario_id
                qualified_catalog_revision = qualified_selection[
                    "scenario_catalog_revision"
                ]
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
                        f"{source_path or f'qualified Scenario {scenario_document_id}'}: "
                        f"program_draft: {error}",
                    ) from error
            deployment_source_text: str | None = None
            alternate_source: str | None = None
            if body.alternate_deployment_path:
                path = body.alternate_deployment_path
                if path.lower().endswith((".scenario.yaml", ".scenario.yml")):
                    alternate, alternate_source = load_scenario_artifact(
                        path, allowed_roots=state.allowed_fs_roots
                    )
                    deployment_document = alternate["deployment_draft"]
                    base = alternate["base_scenario_id"]
                else:
                    from .deployment_artifacts import load_deployment_artifact

                    source, alternate_source = load_deployment_artifact(
                        path, allowed_roots=state.allowed_fs_roots
                    )
                    deployment_source_text = source
                    base = None
            try:
                compiled = (
                    None
                    if deployment_document is None and deployment_source_text is None
                    else compile_deployment_document(
                        deployment_source_text
                        if deployment_source_text is not None
                        else json.dumps(
                            deployment_document, ensure_ascii=False, allow_nan=False
                        )
                    )
                )
            except DeploymentDraftError as error:
                deployment_source = alternate_source or source_path
                raise ScenarioArtifactError(
                    "scenario_deployment_draft_invalid",
                    f"{deployment_source}: deployment_draft: {error}",
                ) from error
            deployment = None if compiled is None else compiled.document
            post_state = _workspace_post_state(
                state, authoring, program, diagnostics, deployment
            )
            incomplete = program is None or compiled is None
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
            intent: dict[str, Any] = {
                "schema_version": "manyforge.composer_authoring_effect.v1",
                "operation_kind": "load_scenario_document",
                "effect_class": "local" if incomplete else "external",
                "external_kind": (
                    None if incomplete else "scenario_document_load"
                ),
                "source_path": source_path,
                "scenario_document_path": source_path,
                "scenario_document_id": scenario_document_id,
                "base_scenario_id": base,
                "alternate_deployment_source": alternate_source,
                "post_state": post_state,
                "scenario_source_program_digest": canonical_digest(
                    {"value": post_state["program"]["authoring"]}
                ),
                "scenario_source_deployment_digest": canonical_digest(
                    {"value": post_state["deployment"]["document"]}
                ),
            }
            direct_qualified = False
            if not incomplete:
                assert program is not None and compiled is not None
                if body.alternate_deployment_path is None and base is not None:
                    exact_selection = qualified_selection
                    if exact_selection is None:
                        exact_selection = state.headless_client.scenario_load_selection(
                            base
                        )
                    if (
                        exact_selection["program"] == program.to_dict()
                        and exact_selection["deployment"] == compiled.document
                    ):
                        qualified_intent = state.prepare_scenario_load(
                            scenario_id=base,
                            expected_catalog_revision=exact_selection[
                                "scenario_catalog_revision"
                            ],
                            confirm_discard=body.confirm_discard,
                            request_id=body.request_id,
                            expected_admission_generation=(
                                body.expected_admission_generation
                            ),
                        )
                        if (
                            qualified_intent["selection"]["program"]
                            != program.to_dict()
                            or qualified_intent["selection"]["deployment"]
                            != compiled.document
                        ):
                            raise ScenarioArtifactError(
                                "scenario_catalog_revision_stale",
                                "The qualified Scenario changed during Load",
                                status_code=409,
                            )
                        qualified_intent.update(intent)
                        qualified_intent.update(
                            {
                                "operation_kind": "load_scenario_document",
                                "external_kind": "scenario_load",
                                "post_state": post_state,
                            }
                        )
                        intent = qualified_intent
                        direct_qualified = True
                if not direct_qualified:
                    scenario_id, catalog_revision = _base_scenario_for_deployment(
                        state, compiled.deployment_artifact_id, base
                    )
                    if (
                        qualified_catalog_revision is not None
                        and qualified_catalog_revision != catalog_revision
                    ):
                        raise ScenarioArtifactError(
                            "scenario_catalog_revision_stale",
                            "The qualified Scenario catalog changed during Load",
                            status_code=409,
                        )
                    status = state.headless_client.manager_status()
                    current_bundle, current_graph, _manager_epoch = state._manager_current(
                        status
                    )
                    applied = state._optional_applied(allow_inactive=True)
                    source = applied.get("source") if isinstance(applied, dict) else None
                    expected_digest = (
                        source.get("record_digest") if isinstance(source, dict) else None
                    )
                    intent.update(
                        {
                            "scenario_id": scenario_id,
                            "expected_scenario_catalog_revision": catalog_revision,
                            "deployment": compiled.document,
                            "program": program.to_dict(),
                            "expected_applied_record_digest": expected_digest,
                            "expected_manager_current": {
                                "retained_bundle_id": current_bundle,
                                "graph_revision": current_graph,
                            },
                            "expected_admission_generation": body.expected_admission_generation,
                            "manager_request_id": "scenario-document-manager-"
                            + hashlib.sha256(
                                f"{body.request_owner_epoch}:{body.request_id}".encode()
                            ).hexdigest()[:32],
                            "correlation_id": body.request_id,
                        }
                    )
            prepared.update(
                {
                    "source_path": source_path,
                    "scenario_document_id": scenario_document_id,
                    "post_state": post_state,
                    "incomplete": incomplete,
                    "direct_qualified": direct_qualified,
                    "intent": intent,
                    "program_state": state.draft_session.retained_state(),
                    "deployment_state": state.deployment_draft_session.retained_state(),
                    "base_scenario_id": base,
                    "authoring_catalog": authoring_catalog,
                    "scenario_document": scenario_document,
                }
            )
            return intent

        def validate(_claim: dict[str, Any]) -> None:
            _require_fence(state, body)
            if (
                state.draft_session.retained_state() != prepared["program_state"]
                or state.deployment_draft_session.retained_state()
                != prepared["deployment_state"]
            ):
                raise ScenarioArtifactError(
                    "scenario_draft_stale",
                    "Program or Deployment Draft changed before Load",
                    status_code=409,
                )

        def effect(claim: dict[str, Any]) -> dict[str, Any]:
            if prepared["incomplete"]:
                state._restore_authoring_post_image(
                    prepared["post_state"],
                    catalog=prepared["authoring_catalog"],
                )
                result = {
                    "status": "opened",
                    "activation": None,
                    "diagnostics": prepared["post_state"]["program"]["diagnostics"],
                }
            else:
                if prepared["direct_qualified"]:
                    loaded = state._execute_scenario_load(
                        prepared["intent"], claim
                    )
                    return {
                        "status": "loaded",
                        "path": prepared["source_path"],
                        "scenario_id": prepared["scenario_document_id"],
                        "activation": loaded["headless"],
                        "diagnostics": [],
                        "workspace": loaded["workspace"],
                    }
                return state.execute_scenario_document_load(
                    prepared["intent"], claim
                )
            state.scenario_document_path = prepared["source_path"]
            state.scenario_base_scenario_id = prepared["base_scenario_id"]
            state.remember_scenario_document(
                prepared.get("scenario_document")
            )
            return {
                **result,
                "path": prepared["source_path"],
                "scenario_id": prepared["scenario_document_id"],
                "workspace": state.draft_session.workspace_snapshot(
                    applied_program_revision=None
                ),
            }

        try:
            result = state.authoring_admission.execute(
                body.envelope(),
                operation_kind="load_scenario_document",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                prepare_effect=prepare,
                validate_under_claim=validate,
                effect=effect,
                post_image=state.authoring_retained_state,
                after_release=lambda: (
                    state.prepare_current_applied_scene(
                        request_owner_epoch=body.request_owner_epoch,
                        request_id=body.request_id,
                    )
                    if prepared and not prepared["incomplete"]
                    else None
                ),
            )
            return result
        except Exception as error:
            _raise(error)

    return router
