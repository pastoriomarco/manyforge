"""Thin HTTP boundary for the separate Composer Deployment Draft."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
import logging
from pathlib import Path
from typing import Any, Literal, NoReturn

from fastapi import APIRouter, HTTPException
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    raw_sha256_digest,
)
from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    capture_deployment_model_sources,
)
from manyforge_headless_api.client import HeadlessClientError
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .authoring_admission import (
    AuthoringMutationEnvelope,
    PARTIAL_EFFECT_OBSERVATION_CODES,
    PARTIAL_EFFECT_OBSERVATION_TIMEOUT_CODES,
    headless_client_http_exception,
)
from .authoring_transaction_journal import AuthoringJournalError
from .deployment_artifacts import (
    load_deployment_artifact,
    normalize_deployment_save_path,
    save_deployment_artifact,
)
from .deployment_draft import DeploymentDraftError
from .draft_session import DraftSessionError
from .state import ComposerState
from .authoring_workspace import (
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
)
from .authoring_workspace_fence import (
    commit_local_authoring,
    require_captured_workspace_identity,
)


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_LOGGER = logging.getLogger(__name__)


def _capture_model_sources(
    state: ComposerState, deployment: dict[str, Any]
) -> dict[str, Any]:
    project = state.scenario_document_path
    if not isinstance(project, str):
        raise DeploymentDraftError(
            "activation_source_project_unavailable",
            "Activation preparation requires an open Scenario project",
            status_code=409,
        )
    try:
        return capture_deployment_model_sources(
            deployment, project_root=Path(project)
        )
    except ModelSourceCaptureError as error:
        raise DeploymentDraftError(
            "activation_model_source_invalid",
            str(error),
            status_code=409,
        ) from error


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

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )


class _LocalFence(_Fence):
    """Migrated ordinary-edit fence: the Deployment Draft plus the workspace.

    Deployment candidate Preview/Apply/Revert stay on the external-operation
    path and keep the plain :class:`_Fence` shape.
    """

    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )

    def require_workspace_fence(
        self, identity: "AuthoringWorkspaceIdentity"
    ) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
        )


class OpenDeploymentRequest(_LocalFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class UpdateDeploymentRequest(_LocalFence):
    path: list[str | int] = Field(min_length=1, max_length=32)
    operation: Literal["set", "append", "remove"] = "set"
    value: Any = None


class UpdateControlledStopRequest(_LocalFence):
    controllable_component_id: str = Field(min_length=1, max_length=256)
    command_target_id: str = Field(min_length=1, max_length=256)
    profile: dict[str, Any] | None
    timing: dict[str, Any] | None


class UpdateMotionTargetLimitRequest(_LocalFence):
    controllable_component_id: str = Field(min_length=1, max_length=256)
    command_target_id: str = Field(min_length=1, max_length=256)
    maximum_tcp_linear_speed_mps: float = Field(gt=0.0, le=1000.0)


class SaveDeploymentRequest(_LocalFence):
    pass


class SaveDeploymentAsRequest(_LocalFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class DiscardDeploymentRequest(_LocalFence):
    pass


class RevertDeploymentRequest(_Fence):
    pass


class PreviewDeploymentCandidateRequest(_Fence):
    pass


class ApplyDeploymentCandidateRequest(_Fence):
    confirmation_token: str | None = Field(
        default=None,
        alias="confirmationToken",
        min_length=71,
        max_length=71,
        pattern=r"^sha256:[0-9a-f]{64}$",
    )
    preparation_request_owner_epoch: str | None = Field(
        default=None,
        alias="preparationRequestOwnerEpoch",
        min_length=1,
        max_length=256,
    )
    preparation_request_id: str | None = Field(
        default=None,
        alias="preparationRequestId",
        min_length=1,
        max_length=256,
    )

    @model_validator(mode="after")
    def require_preparation_identity(self) -> "ApplyDeploymentCandidateRequest":
        if self.confirmation_token is not None and (
            self.preparation_request_owner_epoch is None
            or self.preparation_request_id is None
        ):
            raise ValueError(
                "confirmationToken requires both "
                "preparationRequestOwnerEpoch and preparationRequestId"
            )
        return self


# One bounded observation deadline can expire after its Manager effect was
# already accepted.  Those typed partial timeouts name the accepted Apply, so
# they are gateway/observation timeouts rather than generic server failures;
# their code and message stay exactly as the state owner raised them.
def _raise(error: Exception) -> NoReturn:
    if isinstance(error, AuthoringWorkspaceError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringJournalError)
        and error.code == "program_scene_preparation_unavailable"
    ):
        raise HTTPException(
            status_code=503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringJournalError)
        and error.code in PARTIAL_EFFECT_OBSERVATION_TIMEOUT_CODES
    ):
        raise HTTPException(
            status_code=504,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringJournalError)
        and error.code in PARTIAL_EFFECT_OBSERVATION_CODES
    ):
        raise HTTPException(
            status_code=503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, DeploymentDraftError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    if isinstance(error, DraftSessionError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    raise error


def _write_roots(state: ComposerState) -> list[str]:
    if state.allowed_deployment_write_roots is not None:
        return state.allowed_deployment_write_roots
    return state.allowed_fs_roots


def _published_deployment_canonical(
    state: ComposerState, target: str
) -> str | None:
    """Return the content identity already published at one exact target.

    Save resolves a lost response by rereading the exact target and comparing
    this identity with the canonical content captured for that request. A
    missing or unreadable target returns ``None``, so the ordinary overwrite
    rules still apply.
    """

    try:
        source, _path = load_deployment_artifact(
            target, allowed_roots=_write_roots(state)
        )
        return state.deployment_draft_session.preview_source(source).canonical_json
    except (DeploymentDraftError, OSError, ValueError):
        return None


def _workspace(state: ComposerState) -> dict[str, Any]:
    authoring = state.authoring_workspace.snapshot()
    deployment = authoring.as_dict()["deployment"]
    document = deployment["document"]
    saved_document = deployment["savedDocument"]
    workspace = {
        "schemaVersion": "manyforge.composer_deployment_workspace.v1",
        "draft": {
            "id": deployment["draftId"],
            "revision": deployment["draftRevision"],
            "document": document,
            "deploymentArtifactId": (
                document.get("deployment_artifact_id")
                if isinstance(document, dict)
                else None
            ),
            "deploymentRevision": (
                document.get("deployment_revision")
                if isinstance(document, dict)
                else None
            ),
            "graphRevision": (
                document.get("graph_revision")
                if isinstance(document, dict)
                else None
            ),
            "differsFromSaved": deployment["differsFromSaved"],
        },
        "saved": {
            "path": deployment["savedPath"],
            "deploymentRevision": (
                saved_document.get("deployment_revision")
                if isinstance(saved_document, dict)
                else None
            ),
        },
    }
    workspace["authoringWorkspace"] = authoring.identity.as_dict()
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
            # Manager publication is the identity used by authoring controls.
            # It is intentionally independent from runtime convergence: an
            # Apply may have a durable active projection while native children
            # are still settling.  Keep the strict projection below for
            # execution admission only.
            current_projection = state.published_scenario()
            current_value = current_projection.get("published_scenario")
            if isinstance(current_value, dict):
                current = current_value
            elif not current_projection.get("ready"):
                # Keep the workspace tolerant of legacy/lightweight Headless
                # doubles that expose only the strict current projection. In
                # the real stack this fallback is equivalent whenever the
                # published read is available; a genuinely pending runtime
                # never yields a strict current Scenario and therefore cannot
                # mask the publication read-model.
                strict_projection = state.current_scenario()
                strict_value = strict_projection.get("current_scenario")
                if isinstance(strict_value, dict):
                    current = strict_value
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
    applied_revision = (
        current.get("deployment_revision") if current is not None else None
    )
    # The workspace's Apply/Discard/Revert state is publication identity, not
    # native runtime liveness.  Runtime controls independently remain gated by
    # the strict current_scenario/component readiness projection.
    run_ready = draft_revision is None or draft_revision == applied_revision
    candidates: dict[tuple[str, bool], dict[str, Any] | None] = {}
    selections: dict[str, dict[str, Any] | None] = {}

    def candidate(
        row: dict[str, Any], *, allow_saved_baseline: bool = False
    ) -> dict[str, Any] | None:
        # The catalog owns the pairing, not copied Program identity fields.
        # Resolve only matching Draft/Saved entries through the existing pure
        # selection read, once per workspace response (never on heartbeat).
        scenario_id = row["scenario_id"]
        cache_key = (scenario_id, allow_saved_baseline)
        if cache_key not in candidates:
            candidates[cache_key] = None
            try:
                if scenario_id not in selections:
                    selections[scenario_id] = (
                        state.headless_client.scenario_load_selection(scenario_id)
                    )
                selection = selections[scenario_id]
                if not isinstance(selection, dict):
                    raise HeadlessClientError(
                        "scenario_candidate_selection_unavailable",
                        "Scenario selection was unavailable",
                    )
                selection_mismatch = (
                    selection["scenario_catalog_revision"]
                    != scenario_catalog_revision
                    or any(
                        selection.get(key) != row.get(key)
                        for key in (
                            "scenario_id",
                            "deployment_artifact_id",
                            "deployment_revision",
                            "retained_bundle_id",
                            "program_binding_id",
                        )
                    )
                )
                # After a Deployment Apply, the active selection can be a
                # Manager-produced closure while the catalog row remains the
                # maintained saved candidate.  That is valid for Revert: the
                # saved artifact and catalog row still identify the exact
                # candidate to restore, and the current Program identity is
                # only informational here.
                saved_baseline_retargeted = (
                    selection["scenario_catalog_revision"]
                    != scenario_catalog_revision
                    or any(
                        selection.get(key) != row.get(key)
                        for key in (
                            "deployment_artifact_id",
                            "deployment_revision",
                            "retained_bundle_id",
                        )
                    )
                )
                if selection_mismatch and not (
                    allow_saved_baseline and saved_baseline_retargeted
                ):
                    unavailable.append("scenario_candidate_selection_mismatch")
                else:
                    candidates[cache_key] = {
                        "scenarioId": scenario_id,
                        "scenarioCatalogRevision": scenario_catalog_revision,
                        "deploymentArtifactId": row["deployment_artifact_id"],
                        "deploymentRevision": row["deployment_revision"],
                        "retainedBundleId": row["retained_bundle_id"],
                        "programId": selection["program_id"],
                        "programRevision": selection["program_revision"],
                        "programBindingId": row["program_binding_id"],
                        "description": row.get("description"),
                    }
            except HeadlessClientError as error:
                unavailable.append(error.code)
        return candidates[cache_key]

    matching_scenarios = [
        resolved
        for row in scenarios
        if row.get("deployment_artifact_id") == draft["deploymentArtifactId"]
        and row.get("deployment_revision") == draft_revision
        and (resolved := candidate(row)) is not None
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
        resolved
        for row in scenarios
        if row.get("deployment_artifact_id") == saved_artifact_id
        and row.get("deployment_revision") == saved_revision
        and (resolved := candidate(row, allow_saved_baseline=True)) is not None
    ]
    workspace.update(
        {
            "managerApplied": {
                "deploymentRevision": applied_revision,
                "scenarioId": (
                    current.get("scenario_id") if current is not None else None
                ),
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
        after_release=None,
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
                body.admission_envelope(),
                operation_kind=operation_kind,
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                prepare_effect=prepare,
                validate_under_claim=validate,
                effect=effect,
                post_image=state.authoring_retained_state,
                after_release=after_release,
            )
        except Exception as error:
            _raise(error)

    @router.get("/api/deployment/workspace")
    def read_workspace() -> dict[str, Any]:
        return _workspace(state)

    @router.post("/api/deployment/open-draft")
    def open_draft(body: OpenDeploymentRequest) -> dict[str, Any]:
        try:
            state.deployment_draft_session.require_open_allowed(
                body.draft_id,
                body.expected_draft_revision,
                confirm_discard=body.confirm_discard,
            )
            # The artifact read and its compilation happen outside every owner
            # lock; the commit installs the compiled value atomically and
            # rechecks the discard decision under the composite fence.
            source, source_path = load_deployment_artifact(
                body.path, allowed_roots=state.allowed_fs_roots
            )
            compiled = state.deployment_draft_session.preview_source(source)
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="open_deployment_draft",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_open(
                    compiled,
                    source_path,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                    confirm_discard=body.confirm_discard,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/mutations")
    def mutate_draft(body: UpdateDeploymentRequest) -> dict[str, Any]:
        try:
            compiled = state.deployment_draft_session.preview_update(
                body.path,
                body.value,
                body.operation,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="mutate_deployment_draft",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/controlled-stop")
    def update_controlled_stop(body: UpdateControlledStopRequest) -> dict[str, Any]:
        try:
            compiled = (
                state.deployment_draft_session.preview_controlled_stop_update(
                    body.profile,
                    controllable_component_id=body.controllable_component_id,
                    command_target_id=body.command_target_id,
                    timing=body.timing,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="mutate_deployment_controlled_stop",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/motion-target-limit")
    def update_motion_target_limit(
        body: UpdateMotionTargetLimitRequest,
    ) -> dict[str, Any]:
        try:
            compiled = (
                state.deployment_draft_session.preview_motion_target_limit_update(
                    controllable_component_id=body.controllable_component_id,
                    command_target_id=body.command_target_id,
                    maximum_tcp_linear_speed_mps=(body.maximum_tcp_linear_speed_mps),
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="mutate_deployment_motion_target_limit",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

    def save(body: SaveDeploymentRequest | SaveDeploymentAsRequest) -> dict[str, Any]:
        """Persist one exact Deployment Draft and adopt it as the Saved baseline.

        The capture, the write and the baseline adoption are three separate
        steps: the write happens outside every owner lock and the adoption is one
        composite commit fenced by the captured workspace identity.
        """

        target = body.path if isinstance(body, SaveDeploymentAsRequest) else None
        try:
            prepared = state.deployment_draft_session.prepare_save(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                target_path=(
                    normalize_deployment_save_path(target)
                    if target is not None
                    else None
                ),
                confirm_overwrite=(
                    body.confirm_overwrite
                    if isinstance(body, SaveDeploymentAsRequest)
                    else True
                ),
            )
            if _published_deployment_canonical(state, prepared.target_path) == (
                prepared.canonical_json
            ):
                # This exact content is already published at the target, so a
                # lost response is resolved without repeating the write.
                saved_path = prepared.resolved_target
            else:
                saved_path = save_deployment_artifact(
                    prepared.canonical_json,
                    prepared.target_path,
                    allowed_roots=_write_roots(state),
                    overwrite=prepared.overwrite,
                )
            try:
                commit = commit_local_authoring(
                    state,
                    body,
                    operation_kind=prepared.tool_id,
                    session="deployment",
                    install=lambda: state.deployment_draft_session.install_prepared_save(
                        prepared, saved_path=saved_path
                    ),
                )
            except AuthoringWorkspaceError as error:
                if error.code != "composer_workspace_stale":
                    raise
                raise HTTPException(
                    status_code=409,
                    detail={
                        "code": "deployment_saved_baseline_not_adopted",
                        "message": (
                            "the Deployment artifact was written but a newer "
                            "workspace revision was not adopted as Saved"
                        ),
                        "path": saved_path,
                        "currentWorkspace": error.detail.get("workspace"),
                    },
                ) from error
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/save")
    def save_draft(body: SaveDeploymentRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/save-as")
    def save_draft_as(body: SaveDeploymentAsRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/discard")
    def discard(body: DiscardDeploymentRequest) -> dict[str, Any]:
        try:
            # Discard only restores the local Draft from the authoritative
            # Manager publication.  It must remain usable while the newly
            # published runtime is still converging; requiring the strict
            # execution-ready Scenario here made the cycle fail closed during
            # the normal post-Apply settling window.  Keep a strict fallback
            # for lightweight authoring doubles that expose no publication
            # projection.
            def read_applied() -> tuple[dict[str, Any], str]:
                current_projection = state.published_scenario()
                current = current_projection.get("current_scenario")
                if not isinstance(current, dict):
                    current = current_projection.get("published_scenario")
                if not isinstance(current, dict):
                    strict_projection = state.current_scenario()
                    current = strict_projection.get("current_scenario")
                    current_projection = strict_projection
                if current_projection.get("ready") is not True or not isinstance(
                    current, dict
                ):
                    raise DeploymentDraftError(
                        "deployment_applied_unavailable",
                        "Deployment Discard requires one exact Manager Applied Scenario",
                        status_code=409,
                    )
                if state.headless_client is None:
                    raise DeploymentDraftError(
                        "deployment_applied_unavailable",
                        "Deployment Discard requires the configured Manager",
                        status_code=503,
                    )
                selection = state.headless_client.scenario_load_selection(
                    current["scenario_id"]
                )
                applied = selection.get("deployment")
                if (
                    not isinstance(applied, dict)
                    or applied.get("deployment_revision")
                    != current.get("deployment_revision")
                ):
                    raise DeploymentDraftError(
                        "deployment_applied_projection_mismatch",
                        "Applied Scenario did not return its exact Deployment document",
                        status_code=409,
                    )
                identity = raw_sha256_digest(
                    canonical_json_bytes(
                        {
                            "scenario_id": current["scenario_id"],
                            "deployment": applied,
                        }
                    )
                )
                return applied, identity

            applied, applied_identity = read_applied()
            # The publication read and compilation stay outside owner locks.
            # Once admission is claimed, re-read its exact identity before the
            # local install; Manager publication is external to the workspace
            # revision and therefore needs this explicit validation.
            compiled = state.deployment_draft_session.preview_source(
                json.dumps(applied, ensure_ascii=False, allow_nan=False)
            )

            def validate_applied_under_claim() -> None:
                _current, current_identity = read_applied()
                if current_identity != applied_identity:
                    raise DeploymentDraftError(
                        "deployment_applied_changed",
                        "Manager Applied Deployment changed before Discard committed",
                        status_code=409,
                    )

            commit = commit_local_authoring(
                state,
                body,
                operation_kind="discard_deployment_draft",
                session="deployment",
                validate_under_claim=validate_applied_under_claim,
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise(error)

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
        program = state.draft_session.retained_state().get("program")
        assignment = deepcopy(state.scenario_program_role_assignment)
        source_document_digest = state.scenario_source_document_digest
        overrides = deepcopy(state.scenario_typed_parameter_overrides)
        if (
            not isinstance(program, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_document_digest, str)
        ):
            _raise(
                DeploymentDraftError(
                    "activation_input_incomplete",
                    "Deployment Revert requires a complete saved Scenario input",
                    status_code=409,
                )
            )
        source_closure = _capture_model_sources(state, saved)
        captured_input_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "source_document_digest": source_document_digest,
                    "deployment": saved,
                    "model_source_closure": source_closure,
                    "program": program,
                    "program_role_assignment": assignment,
                    "typed_parameter_overrides": overrides,
                }
            )
        )
        current_bundle, current_graph, _ = state._manager_current(
            state.headless_client.manager_status()
        )
        manager_request_id = (
            "deployment-revert-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32]
        )
        post_state = state.authoring_retained_state()
        deployment_post = deepcopy(post_state["deployment"])
        if deployment_post["document"] != saved:
            deployment_post["draft_revision"] += 1
        deployment_post["document"] = deepcopy(saved)
        post_state["deployment"] = deployment_post
        intent = {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "operation_kind": "revert_deployment",
            "effect_class": "external",
            "external_kind": "revert_deployment",
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "program": deepcopy(program),
            "program_role_assignment": assignment,
            "typed_parameter_overrides": overrides,
            "deployment": deepcopy(saved),
            "expected_manager_current": {
                "retained_bundle_id": current_bundle,
                "graph_revision": current_graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "draft_id": body.draft_id,
            "draft_revision": body.expected_draft_revision,
            "manager_request_id": manager_request_id,
            "correlation_id": body.request_id,
            "post_state": post_state,
        }

        prepared_candidate: dict[str, Any] | None = None

        def validate(_claim: dict[str, Any]) -> dict[str, Any]:
            nonlocal prepared_candidate
            current = state.deployment_draft_session.retained_state()
            if current != retained:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before Revert",
                    status_code=409,
                )
            if _capture_model_sources(state, saved) != source_closure:
                raise DeploymentDraftError(
                    "activation_model_source_changed",
                    "A model source changed during activation capture",
                    status_code=409,
                )
            prepared_candidate = state.execute_authored_deployment_candidate(
                intent, _claim, apply=True, prepare_only=True
            )
            return {"prepared_deployment_candidate": prepared_candidate}

        def effect(claim: dict[str, Any]) -> dict[str, Any]:
            if prepared_candidate is None:
                raise DeploymentDraftError(
                    "deployment_revert_preparation_missing",
                    "Deployment Revert was not prepared under its claim",
                    status_code=409,
                )
            state.execute_authored_deployment_candidate(
                intent,
                claim,
                apply=True,
                prepared=prepared_candidate,
            )
            return _workspace(state)

        return execute(
            body,
            "revert_deployment",
            prepare=lambda: intent,
            validate=validate,
            effect=effect,
        )

    def authored_candidate(
        body: PreviewDeploymentCandidateRequest | ApplyDeploymentCandidateRequest,
        *,
        apply: bool,
    ) -> dict[str, Any]:
        operation_kind = (
            "apply_deployment_candidate"
            if apply
            else "preview_deployment_candidate"
        )
        coordinator = state.authoring_admission
        if coordinator is not None:
            try:
                replayed, result = coordinator.replay_or_recover_exact(
                    body.admission_envelope(),
                    operation_kind=operation_kind,
                    draft_id=body.draft_id,
                    draft_revision=body.expected_draft_revision,
                    payload=body.model_dump(mode="json", by_alias=True),
                )
            except Exception as error:
                _raise(error)
            if replayed:
                operation = result.get("result") if isinstance(result, dict) else None
                if (
                    apply
                    and isinstance(operation, dict)
                    and operation.get("status") == "applied"
                ):
                    try:
                        state.prepare_current_applied_scene(
                            request_owner_epoch=body.request_owner_epoch,
                            request_id=body.request_id,
                        )
                    except (AuthoringJournalError, HeadlessClientError) as error:
                        _LOGGER.warning(
                            "authoring_post_release_followup_pending",
                            extra={
                                "operation_kind": operation_kind,
                                "request_owner_epoch": body.request_owner_epoch,
                                "request_id": body.request_id,
                                "error_code": getattr(error, "code", None),
                            },
                        )
                    except Exception:
                        _LOGGER.exception(
                            "authoring_post_release_followup_failed",
                            extra={
                                "operation_kind": operation_kind,
                                "request_owner_epoch": body.request_owner_epoch,
                                "request_id": body.request_id,
                            },
                        )
                return result
        if state.headless_client is None:
            _raise(
                DeploymentDraftError(
                    "deployment_candidate_unavailable",
                    "Deployment preview and Apply require the configured Manager",
                    status_code=503,
                )
            )

        def manager_current() -> tuple[str, str]:
            try:
                bundle, graph, _epoch = state._manager_current(
                    state.headless_client.manager_status()
                )
                return bundle, graph
            except DraftSessionError as error:
                raise DeploymentDraftError(
                    error.code, str(error), status_code=error.status_code
                ) from error

        state.deployment_draft_session.require_current(
            body.draft_id, body.expected_draft_revision
        )
        deployment_state = state.deployment_draft_session.retained_state()
        deployment = deployment_state.get("document")
        if not isinstance(deployment, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_draft_unavailable",
                    "Open a Deployment Draft before previewing it",
                    status_code=404,
                )
            )
        program = state.draft_session.retained_state().get("program")
        assignment = deepcopy(state.scenario_program_role_assignment)
        source_document_digest = state.scenario_source_document_digest
        overrides = deepcopy(state.scenario_typed_parameter_overrides)
        if (
            not isinstance(program, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_document_digest, str)
        ):
            _raise(
                DeploymentDraftError(
                    "activation_input_incomplete",
                    "Save or load one complete Scenario document with Program, "
                    "Deployment, and explicit role assignment before preparation",
                    status_code=409,
                )
            )
        retained_confirmation_input: dict[str, Any] | None = None
        if (
            isinstance(body, ApplyDeploymentCandidateRequest)
            and body.confirmation_token is not None
        ):
            preparation_owner_epoch = body.preparation_request_owner_epoch
            preparation_request_id = body.preparation_request_id
            assert isinstance(preparation_owner_epoch, str)
            assert isinstance(preparation_request_id, str)
            try:
                retained_confirmation_input = (
                    state.retained_deployment_candidate_input(
                        preparation_owner_epoch,
                        preparation_request_id,
                    )
                )
            except AuthoringJournalError as error:
                raise HTTPException(
                    status_code=409,
                    detail={"code": error.code, "message": str(error)},
                ) from error
            source_closure = deepcopy(
                retained_confirmation_input["model_source_closure"]
            )
            captured_input_digest = retained_confirmation_input[
                "captured_input_digest"
            ]
        else:
            source_closure = _capture_model_sources(state, deployment)
            captured_input_digest = raw_sha256_digest(
                canonical_json_bytes(
                    {
                        "source_document_digest": source_document_digest,
                        "deployment": deployment,
                        "model_source_closure": source_closure,
                        "program": program,
                        "program_role_assignment": assignment,
                        "typed_parameter_overrides": overrides,
                    }
                )
            )
        current_bundle, current_graph = manager_current()
        pre_state = state.authoring_retained_state()
        intent = {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "operation_kind": operation_kind,
            "effect_class": "external",
            "external_kind": (
                "deployment_candidate_apply"
                if apply
                else "deployment_candidate_preview"
            ),
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "deployment": deepcopy(deployment),
            "program": deepcopy(program),
            "program_role_assignment": assignment,
            "typed_parameter_overrides": overrides,
            "expected_manager_current": {
                "retained_bundle_id": current_bundle,
                "graph_revision": current_graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "manager_request_id": "deployment-candidate-manager-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32],
            "correlation_id": body.request_id,
            "confirmation_token": (
                body.confirmation_token
                if isinstance(body, ApplyDeploymentCandidateRequest)
                else None
            ),
            "preparation_request_owner_epoch": (
                body.preparation_request_owner_epoch
                if isinstance(body, ApplyDeploymentCandidateRequest)
                and body.confirmation_token is not None
                else body.request_owner_epoch
            ),
            "preparation_request_id": (
                body.preparation_request_id
                if isinstance(body, ApplyDeploymentCandidateRequest)
                and body.confirmation_token is not None
                else body.request_id
            ),
            "draft_id": body.draft_id,
            "draft_revision": body.expected_draft_revision,
            "post_state": pre_state,
        }

        prepared: dict[str, Any] = {}

        def validate(_claim: dict[str, Any]) -> dict[str, Any] | None:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            if state.deployment_draft_session.retained_state() != deployment_state:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before candidate compilation",
                    status_code=409,
                )
            latest_bundle, latest_graph = manager_current()
            if (
                state.draft_session.retained_state().get("program") != program
                or state.scenario_program_role_assignment != assignment
                or state.scenario_source_document_digest != source_document_digest
                or state.scenario_typed_parameter_overrides != overrides
                or (
                    retained_confirmation_input is None
                    and _capture_model_sources(state, deployment) != source_closure
                )
                or (latest_bundle, latest_graph) != (current_bundle, current_graph)
            ):
                raise DeploymentDraftError(
                    "deployment_candidate_source_changed",
                    "Captured authoring input or Manager selection changed "
                    "before activation preparation",
                    status_code=409,
                )
            # Compile or resolve the retained confirmation exactly once under
            # this request's fresh claim. The complete, JSON-safe preparation
            # is journaled as the effect intent before Manager mutation, so
            # recovery reuses the same candidate and request rather than
            # compiling or previewing again.
            try:
                prepared["candidate"] = (
                    state.execute_authored_deployment_candidate(
                        intent,
                        _claim,
                        apply=apply,
                        prepare_only=True,
                    )
                )
            except AuthoringJournalError as error:
                if error.code not in {"confirmation_expired", "confirmation_stale"}:
                    raise
                raise HTTPException(
                    status_code=409,
                    detail={"code": error.code, "message": str(error)},
                ) from error
            except HeadlessClientError as error:
                raise headless_client_http_exception(error) from error
            return {"prepared_deployment_candidate": prepared["candidate"]}

        completed_apply = False

        def effect(claim: dict[str, Any]) -> dict[str, Any]:
            nonlocal completed_apply
            try:
                result = state.execute_authored_deployment_candidate(
                    intent,
                    claim,
                    apply=apply,
                    prepared=prepared.get("candidate"),
                )
            except Exception as error:
                candidate = prepared.get("candidate")
                phases = (
                    candidate.get("phases_ms")
                    if isinstance(candidate, dict)
                    else None
                )
                manager_request = (
                    candidate.get("manager_apply")
                    if isinstance(candidate, dict)
                    else None
                )
                if not isinstance(phases, dict):
                    failure_phase = "preparation"
                elif (
                    manager_request is not None
                    and "manager_apply_ms" not in phases
                ):
                    failure_phase = "manager_apply"
                elif "scenario_selection_ms" not in phases:
                    failure_phase = "scenario_selection"
                elif "program_rebind_ms" not in phases:
                    failure_phase = "program_rebind"
                else:
                    failure_phase = "workspace_projection"
                _LOGGER.exception(
                    "deployment_candidate_activation_failed",
                    extra={
                        "request_owner_epoch": body.request_owner_epoch,
                        "request_id": body.request_id,
                        "draft_id": body.draft_id,
                        "draft_revision": body.expected_draft_revision,
                        "failure_phase": failure_phase,
                        "error_code": getattr(error, "code", None),
                        "phases_ms": phases,
                    },
                )
                raise
            completed_apply = result.get("status") == "applied"
            return {
                "schemaVersion": (
                    "manyforge.composer_deployment_candidate_operation.v1"
                ),
                "workspace": _workspace(state),
                "result": result,
            }

        return execute(
            body,
            intent["operation_kind"],
            prepare=lambda: intent,
            validate=validate,
            effect=effect,
            after_release=(
                lambda: (
                    state.prepare_current_applied_scene(
                        request_owner_epoch=body.request_owner_epoch,
                        request_id=body.request_id,
                    )
                    if completed_apply
                    else None
                )
            ),
        )

    @router.post("/api/deployment/draft/preview")
    def preview_authored_candidate(
        body: PreviewDeploymentCandidateRequest,
    ) -> dict[str, Any]:
        return authored_candidate(body, apply=False)

    @router.post("/api/deployment/draft/apply")
    def apply_authored_candidate(
        body: ApplyDeploymentCandidateRequest,
    ) -> dict[str, Any]:
        return authored_candidate(body, apply=True)

    return router
