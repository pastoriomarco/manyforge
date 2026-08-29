"""Thin HTTP boundary for the separate Composer Deployment Draft."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from typing import Any, Literal, NoReturn

from fastapi import APIRouter, HTTPException
from manyforge_headless_api.client import HeadlessClientError
from pydantic import BaseModel, ConfigDict, Field

from .authoring_admission import (
    AuthoringMutationEnvelope,
    headless_client_http_exception,
)
from .deployment_artifacts import (
    load_deployment_artifact,
    normalize_deployment_save_path,
    save_deployment_artifact,
)
from .deployment_draft import CompiledDeploymentDocument, DeploymentDraftError
from .state import ComposerState


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)


class _Fence(BaseModel):
    model_config = _DTO

    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: int = Field(
        alias="expectedDraftRevision", ge=0, le=9_007_199_254_740_991
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


class OpenDeploymentRequest(_Fence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class UpdateDeploymentRequest(_Fence):
    path: list[str | int] = Field(min_length=1, max_length=32)
    operation: Literal["set", "append", "remove"] = "set"
    value: Any = None


class SaveDeploymentRequest(_Fence):
    pass


class SaveDeploymentAsRequest(_Fence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class DiscardDeploymentRequest(_Fence):
    pass


class RevertDeploymentRequest(_Fence):
    pass


def _raise(error: Exception) -> NoReturn:
    if isinstance(error, DeploymentDraftError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    raise error


def _write_roots(state: ComposerState) -> list[str]:
    if state.allowed_deployment_write_roots is not None:
        return state.allowed_deployment_write_roots
    return state.allowed_fs_roots


def _workspace(state: ComposerState) -> dict[str, Any]:
    workspace = state.deployment_draft_session.snapshot()
    draft = workspace["draft"]
    current_projection: dict[str, Any] | None = None
    current: dict[str, Any] | None = None
    binding: dict[str, Any] | None = None
    scenarios: list[dict[str, Any]] = []
    scenario_catalog_revision: str | None = None
    unavailable: list[str] = []
    if state.headless_client is None:
        unavailable.append("headless_unavailable")
    else:
        try:
            current_projection = state.current_scenario()
            current_value = current_projection.get("current_scenario")
            if isinstance(current_value, dict):
                current = current_value
            catalog = state.headless_client.scenarios()
            scenario_catalog_revision = catalog.get("scenario_catalog_revision")
            rows = catalog.get("scenarios")
            if isinstance(rows, list):
                scenarios = [deepcopy(row) for row in rows if isinstance(row, dict)]
            if current is not None and draft["deploymentRevision"] == current.get(
                "deployment_revision"
            ):
                selection = state.headless_client.scenario_program_selection(
                    current["scenario_id"]
                )
                projected = selection.get("program_binding")
                if isinstance(projected, dict) and all(
                    projected.get(field) == current.get(field)
                    for field in (
                        "program_id",
                        "program_revision",
                        "program_binding_id",
                        "retained_bundle_id",
                        "graph_revision",
                    )
                ):
                    binding = deepcopy(projected)
                else:
                    unavailable.append("program_binding_current_scenario_mismatch")
        except HeadlessClientError as error:
            unavailable.append(error.code)
    draft_revision = draft["deploymentRevision"]
    applied_revision = current.get("deployment_revision") if current is not None else None
    run_ready = draft_revision is None or draft_revision == applied_revision
    matching_scenarios = [
        {
            "scenarioId": row.get("scenario_id"),
            "scenarioCatalogRevision": scenario_catalog_revision,
            "deploymentArtifactId": row.get("deployment_artifact_id"),
            "deploymentRevision": row.get("deployment_revision"),
            "retainedBundleId": row.get("retained_bundle_id"),
            "programId": row.get("program_id"),
            "programRevision": row.get("program_revision"),
            "programBindingId": row.get("program_binding_id"),
            "description": row.get("description"),
        }
        for row in scenarios
        if row.get("deployment_artifact_id") == draft["deploymentArtifactId"]
        and row.get("deployment_revision") == draft_revision
    ]
    retained = state.deployment_draft_session.retained_state()
    saved_document = retained.get("saved_document")
    saved_artifact_id = (
        saved_document.get("deployment_artifact_id")
        if isinstance(saved_document, dict)
        else None
    )
    saved_revision = (
        saved_document.get("deployment_revision")
        if isinstance(saved_document, dict)
        else None
    )
    saved_scenarios = [
        {
            "scenarioId": row.get("scenario_id"),
            "scenarioCatalogRevision": scenario_catalog_revision,
            "deploymentArtifactId": row.get("deployment_artifact_id"),
            "deploymentRevision": row.get("deployment_revision"),
            "retainedBundleId": row.get("retained_bundle_id"),
            "programId": row.get("program_id"),
            "programRevision": row.get("program_revision"),
            "programBindingId": row.get("program_binding_id"),
            "description": row.get("description"),
        }
        for row in scenarios
        if row.get("deployment_artifact_id") == saved_artifact_id
        and row.get("deployment_revision") == saved_revision
    ]
    workspace.update(
        {
            "managerApplied": {
                "deploymentRevision": applied_revision,
                "scenarioId": current.get("scenario_id") if current is not None else None,
            },
            "runAdmission": {
                "ready": run_ready,
                "reason": (
                    None
                    if run_ready
                    else "Deployment Draft differs from Manager Applied"
                ),
            },
            "binding": binding,
            "scenarioCandidates": matching_scenarios,
            "savedScenarioCandidates": saved_scenarios,
            "unavailableReasons": unavailable,
        }
    )
    return workspace


def create_deployment_draft_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    def execute(
        body: _Fence,
        operation_kind: str,
        *,
        prepare,
        effect,
        validate=None,
    ):
        coordinator = state.authoring_admission
        if coordinator is None:
            raise HTTPException(
                status_code=503,
                detail={
                    "code": "authoring_not_ready",
                    "message": "Deployment authoring requires Headless-backed Idle admission",
                },
            )
        try:
            return coordinator.execute(
                body.envelope(),
                operation_kind=operation_kind,
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                prepare_effect=prepare,
                validate_under_claim=validate,
                effect=effect,
                post_image=state.authoring_retained_state,
            )
        except Exception as error:
            _raise(error)

    @router.get("/api/deployment/workspace")
    def read_workspace() -> dict[str, Any]:
        return _workspace(state)

    @router.post("/api/deployment/open-draft")
    def open_draft(body: OpenDeploymentRequest) -> dict[str, Any]:
        prepared: dict[str, Any] = {}

        def validate(_claim: dict[str, Any]) -> dict[str, Any]:
            state.deployment_draft_session.require_open_allowed(
                body.draft_id,
                body.expected_draft_revision,
                confirm_discard=body.confirm_discard,
            )
            source, source_path = load_deployment_artifact(
                body.path, allowed_roots=state.allowed_fs_roots
            )
            prepared["compiled"] = state.deployment_draft_session.preview_source(source)
            prepared["source_path"] = source_path
            return {"source_path": source_path}

        def effect(_claim: dict[str, Any]) -> dict[str, Any]:
            state.deployment_draft_session.commit_open(
                prepared["compiled"],
                prepared["source_path"],
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                confirm_discard=body.confirm_discard,
            )
            return _workspace(state)

        return execute(
            body,
            "open_deployment_draft",
            prepare=lambda: {"effect_class": "local"},
            validate=validate,
            effect=effect,
        )

    @router.post("/api/deployment/draft/mutations")
    def mutate_draft(body: UpdateDeploymentRequest) -> dict[str, Any]:
        prepared: dict[str, CompiledDeploymentDocument] = {}

        def validate(_claim: dict[str, Any]) -> None:
            prepared["compiled"] = state.deployment_draft_session.preview_update(
                body.path,
                body.value,
                body.operation,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )

        def effect(_claim: dict[str, Any]) -> dict[str, Any]:
            state.deployment_draft_session.commit_compiled(
                prepared["compiled"],
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            return _workspace(state)

        return execute(
            body,
            "mutate_deployment_draft",
            prepare=lambda: {"effect_class": "local"},
            validate=validate,
            effect=effect,
        )

    def save(body: SaveDeploymentRequest | SaveDeploymentAsRequest) -> dict[str, Any]:
        state.deployment_draft_session.require_current(
            body.draft_id, body.expected_draft_revision
        )
        snapshot = state.deployment_draft_session.snapshot()
        current_path = snapshot["saved"]["path"]
        if isinstance(body, SaveDeploymentAsRequest):
            target = normalize_deployment_save_path(body.path)
            resolved = str(Path(target).expanduser().parent.resolve() / Path(target).name)
            overwrite = body.confirm_overwrite or resolved == current_path
        else:
            if current_path is None:
                _raise(
                    DeploymentDraftError(
                        "deployment_saved_path_unavailable",
                        "Save requires an existing path; use Save As",
                        status_code=409,
                    )
                )
            target = current_path
            overwrite = True
        canonical = state.deployment_draft_session.canonical_json(
            draft_id=body.draft_id,
            expected_revision=body.expected_draft_revision,
        )
        target_resolved = str(Path(target).expanduser().parent.resolve() / Path(target).name)
        deployment_post = state.deployment_draft_session.retained_state()
        deployment_post["saved_path"] = target_resolved
        deployment_post["saved_document"] = deepcopy(snapshot["draft"]["document"])
        post_state = {
            "schema_version": "manyforge.composer_authoring_workspace.v2",
            "program": state.draft_session.retained_state(),
            "deployment": deployment_post,
        }

        def validate(_claim: dict[str, Any]) -> None:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            current_canonical = state.deployment_draft_session.canonical_json(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            if current_canonical != canonical:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed; refresh before saving",
                    status_code=409,
                )

        def effect(_claim: dict[str, Any]) -> dict[str, Any]:
            saved_path = save_deployment_artifact(
                canonical,
                target,
                allowed_roots=_write_roots(state),
                overwrite=overwrite,
            )
            state.deployment_draft_session.mark_saved(
                saved_path,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            return _workspace(state)

        return execute(
            body,
            "save_deployment_draft",
            prepare=lambda: {
                "schema_version": "manyforge.composer_authoring_effect.v1",
                "operation_kind": "save_deployment_draft",
                "effect_class": "external",
                "external_kind": "save_deployment",
                "target_path": target,
                "overwrite": overwrite,
                "canonical_json": canonical,
                "post_state": post_state,
            },
            validate=validate,
            effect=effect,
        )

    @router.post("/api/deployment/save")
    def save_draft(body: SaveDeploymentRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/save-as")
    def save_draft_as(body: SaveDeploymentAsRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/discard")
    def discard(body: DiscardDeploymentRequest) -> dict[str, Any]:
        prepared: dict[str, CompiledDeploymentDocument] = {}

        def validate(_claim: dict[str, Any]) -> None:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            retained = state.deployment_draft_session.retained_state()
            saved = retained["saved_document"]
            if saved is None:
                raise DeploymentDraftError(
                    "deployment_saved_unavailable",
                    "Deployment has no Saved baseline",
                    status_code=404,
                )
            prepared["compiled"] = state.deployment_draft_session.preview_source(
                json.dumps(saved, ensure_ascii=False, allow_nan=False)
            )

        def effect(_claim: dict[str, Any]) -> dict[str, Any]:
            state.deployment_draft_session.commit_compiled(
                prepared["compiled"],
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            return _workspace(state)

        return execute(
            body,
            "discard_deployment_draft",
            prepare=lambda: {"effect_class": "local"},
            validate=validate,
            effect=effect,
        )

    @router.post("/api/deployment/revert")
    def revert(body: RevertDeploymentRequest) -> dict[str, Any]:
        if state.headless_client is None:
            _raise(
                DeploymentDraftError(
                    "manager_apply_unavailable",
                    "Deployment Revert requires the configured Manager",
                    status_code=503,
                )
            )
        retained = state.deployment_draft_session.retained_state()
        saved = retained["saved_document"]
        if not isinstance(saved, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_saved_unavailable",
                    "Deployment has no Saved baseline",
                    status_code=404,
                )
            )
        saved_compiled = state.deployment_draft_session.preview_source(
            json.dumps(saved, ensure_ascii=False, allow_nan=False)
        )
        workspace = _workspace(state)
        candidates = workspace["savedScenarioCandidates"]
        if not candidates:
            _raise(
                DeploymentDraftError(
                    "deployment_saved_candidate_unavailable",
                    "Saved Deployment has no current curated Apply candidate",
                    status_code=409,
                )
            )
        candidate = candidates[0]
        manager_status = state.headless_client.manager_status()
        active = manager_status["status"]["active"]
        manager_epoch = manager_status["source"]["source_owner_epoch"]
        preview = state.headless_client.manager_deployment_candidate_preview(
            {
                "deployment_artifact_id": candidate["deploymentArtifactId"],
                "deployment_revision": candidate["deploymentRevision"],
                "expected_current": {
                    "graph_revision": active["graph_revision"],
                    "retained_bundle_id": active["retained_bundle_id"],
                },
                "expected_admission_generation": (
                    body.expected_admission_generation
                ),
                "request_owner_epoch": manager_epoch,
            }
        )
        receipt = preview.get("candidate_receipt")
        if preview.get("ok") is not True or not isinstance(receipt, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_saved_candidate_rejected",
                    "Manager rejected the Saved Deployment candidate",
                    status_code=409,
                )
            )
        post_state = state.authoring_retained_state()
        deployment_post = deepcopy(post_state["deployment"])
        if deployment_post["document"] != saved:
            deployment_post["draft_revision"] += 1
        deployment_post["document"] = deepcopy(saved)
        post_state["deployment"] = deployment_post
        manager_request_id = "deployment-revert-" + hashlib.sha256(
            f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
        ).hexdigest()[:32]
        intent = {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "operation_kind": "revert_deployment",
            "effect_class": "external",
            "external_kind": "revert_deployment",
            "manager_apply": {
                "request_id": manager_request_id,
                "request_owner_epoch": manager_epoch,
                "source_label": "manyforge_composer_deployment_revert",
                "correlation_id": body.request_id,
                "deployment_artifact_id": candidate["deploymentArtifactId"],
                "deployment_revision": candidate["deploymentRevision"],
                "expected_current": receipt["expected_current"],
                "reject_if_affected_busy": True,
                "candidate_receipt": receipt,
            },
            "post_state": post_state,
        }

        def validate(_claim: dict[str, Any]) -> None:
            current = state.deployment_draft_session.retained_state()
            if current != retained:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before Revert",
                    status_code=409,
                )

        def effect(claim: dict[str, Any]) -> dict[str, Any]:
            state.apply_manager_deployment_candidate(intent, claim)
            state.deployment_draft_session.commit_compiled(
                saved_compiled,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            return _workspace(state)

        return execute(
            body,
            "revert_deployment",
            prepare=lambda: intent,
            validate=validate,
            effect=effect,
        )

    return router
