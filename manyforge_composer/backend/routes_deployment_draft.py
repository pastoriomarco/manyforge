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
    AuthoringOperationError,
    AuthoringMutationEnvelope,
    OperationEffects,
    PARTIAL_EFFECT_OBSERVATION_CODES,
    draft_session_http_exception,
    headless_client_http_exception,
    raise_save_failure,
    refused_before_effect,
    scenario_artifact_http_exception,
)
from .deployment_artifacts import (
    load_deployment_artifact,
    normalize_deployment_save_path,
    save_deployment_artifact,
)
from .deployment_draft import DeploymentDraftError
from .deployment_intent import DeploymentIntentError, rebase_deployment_intent_sources
from .draft_session import DraftSessionError
from .reset_apply_log import log_reset_apply_link
from .state import ComposerState, _controllable_composition_change_kinds
from .authoring_workspace import (
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    scenario_project_facts,
)
from .scenario_artifacts import (
    SCENARIO_PROJECT_MEMBERS,
    ScenarioArtifactError,
    save_scenario_member,
)
from .authoring_workspace_fence import (
    commit_draft_edit,
    commit_local_authoring,
    require_captured_workspace_identity,
)


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_LOGGER = logging.getLogger(__name__)


def _capture_model_sources(
    state: ComposerState, deployment: dict[str, Any]
) -> dict[str, Any]:
    project = state.deployment_source_project_path
    if not isinstance(project, str):
        raise DeploymentDraftError(
            "activation_source_project_unavailable",
            "Activation preparation requires a source project directory",
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


def _require_draftable_composition(
    before: dict[str, Any] | None,
    after: dict[str, Any],
) -> None:
    changed_kinds = _controllable_composition_change_kinds(before, after)
    if changed_kinds is None:
        raise HTTPException(
            status_code=409,
            detail={
                "code": "deployment_activation_classification_unavailable",
                "message": (
                    "This Deployment shape cannot be safely classified for a "
                    "Draft-only edit. Use an explicit Manager activation path."
                ),
                "activationRequired": True,
                "classificationAvailable": False,
            },
        )
    if not changed_kinds:
        return
    raise HTTPException(
        status_code=409,
        detail={
            "code": "deployment_activation_required",
            "message": (
                "This Deployment edit changes live controllable composition; "
                "a Draft-only operation cannot apply it. Use an explicit "
                "Manager activation path."
            ),
            "activationRequired": True,
            "changedGraphKinds": list(changed_kinds),
        },
    )


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
    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )

    def require_workspace_fence(self, identity) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
        )


class _LocalFence(_Fence):
    """Migrated ordinary-edit fence: the Deployment Draft plus the workspace.

    Deployment candidate Preview/Apply/Revert stay on the external-operation
    path and keep the plain :class:`_Fence` shape.
    """

    pass


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


class ActivationScope(BaseModel):
    """The complete activation input an operator confirmed.

    Deployment activation compiles the Deployment Draft together with the
    current Program Draft, the Scenario's Program role assignment and its
    source document. A confirmation names all of them; any change before the
    Apply is sent refuses it so the operator reviews the new input.
    """

    model_config = _DTO

    deployment_draft_id: str = Field(
        alias="deploymentDraftId", min_length=1, max_length=256
    )
    deployment_draft_revision: int = Field(
        alias="deploymentDraftRevision", ge=0, le=9_007_199_254_740_991
    )
    deployment_revision: str | None = Field(
        alias="deploymentRevision", max_length=256
    )
    program_draft_id: str = Field(alias="programDraftId", min_length=1, max_length=256)
    program_draft_revision: int = Field(
        alias="programDraftRevision", ge=0, le=9_007_199_254_740_991
    )
    program_revision: str | None = Field(alias="programRevision", max_length=256)
    source_document_digest: str | None = Field(
        alias="sourceDocumentDigest", max_length=256
    )
    program_role_assignment: dict[str, Any] | None = Field(
        alias="programRoleAssignment"
    )


def _require_activation_scope(state: ComposerState, scope: ActivationScope) -> None:
    """Refuse, before any effect, when the confirmed input changed."""

    deployment = state.deployment_draft_session.retained_state()
    document = deployment.get("document")
    program = state.draft_session.snapshot()
    if (
        deployment.get("draft_id") != scope.deployment_draft_id
        or deployment.get("draft_revision") != scope.deployment_draft_revision
        or (document.get("deployment_revision") if isinstance(document, dict) else None)
        != scope.deployment_revision
        or program.get("draft_id") != scope.program_draft_id
        or program.get("draft_revision") != scope.program_draft_revision
        or program.get("program_revision") != scope.program_revision
        or state.scenario_source_document_digest != scope.source_document_digest
        or state.scenario_program_role_assignment != scope.program_role_assignment
    ):
        raise refused_before_effect(
            409,
            {
                "code": "activation_input_changed_after_confirmation",
                "message": "Draft changed after confirmation; review again",
            },
        )


def _applied_program_revision(state: ComposerState) -> str | None:
    """The Applied Program revision, or None when it cannot be read now."""

    try:
        applied = state.headless_client.program_applied()
    except HeadlessClientError:
        return None
    revision = applied.get("program_revision") if isinstance(applied, dict) else None
    return revision if isinstance(revision, str) and revision else None


class ApplyDeploymentCandidateRequest(_Fence):
    # The shared reference of an Apply that first Reset a held run; the
    # Manager record keeps it instead of this request's own identity.
    correlation_id: str | None = Field(
        default=None, alias="correlationId", min_length=1, max_length=256
    )
    # The complete input the operator confirmed (see ActivationScope).
    expected_activation_scope: ActivationScope | None = Field(
        default=None, alias="expectedActivationScope"
    )
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


# Deployment Apply refusals raised before admission or any Manager submission
# (or under the claim before it, which then settles as proven no-effect): a
# no-op Apply, and a confirmed activation input that changed meanwhile.
def _raise(error: Exception) -> NoReturn:
    if isinstance(error, AuthoringWorkspaceError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringOperationError)
        and error.code == "program_scene_preparation_unavailable"
    ):
        raise HTTPException(
            status_code=503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringOperationError)
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
    if isinstance(error, ScenarioArtifactError):
        raise scenario_artifact_http_exception(error) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    if isinstance(error, DraftSessionError):
        raise draft_session_http_exception(error) from error
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
    current: dict[str, Any] | None = None
    binding: dict[str, Any] | None = None
    unavailable: list[str] = []
    if state.headless_client is None:
        unavailable.append("headless_unavailable")
    else:
        try:
            projection = state.published_scenario()
            value = projection.get("published_scenario")
            if isinstance(value, dict):
                current = value
            elif not projection.get("ready"):
                strict = state.current_scenario()
                value = strict.get("current_scenario")
                if isinstance(value, dict):
                    current = value
        except HeadlessClientError as error:
            unavailable.append(error.code)
    draft_revision = draft["deploymentRevision"]
    applied_revision = (
        current.get("deployment_revision") if current is not None else None
    )
    applied_document = (
        state.applied_deployment_for_publication(current)
        if current is not None
        else None
    )
    run_ready = applied_document is not None and document == applied_document
    # Catalog candidates were a legacy package-selection view. Authoring now
    # operates only on the open Scenario and its explicit Saved baseline.
    matching_scenarios: list[dict[str, Any]] = []
    saved_scenarios: list[dict[str, Any]] = []
    workspace.update(
        {
            "managerApplied": {
                "activationOrder": (
                    {
                        "retainedBundleId": current["retained_bundle_id"],
                        "graphRevision": current["graph_revision"],
                        "componentIds": [
                            row["component_id"] for row in applied_document["components"]
                        ],
                    }
                    if applied_document is not None and current is not None
                    else None
                ),
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
        commit_before_submit=None,
        after_release=None,
    ):
        try:
            prepare()
            coordinator = state.authoring_admission
            if coordinator is None:
                raise DeploymentDraftError(
                    "authoring_not_ready",
                    "authoritative authoring admission is unavailable",
                    status_code=503,
                )
            identity = state.authoring_workspace.identity()
            body.require_workspace_fence(identity)
            # Preparation needs idle admission. Capture its immutable result
            # before taking the claim; under the claim only recheck the input
            # fence. Manager validates the original preview and parent claim.
            validation = validate(None) if validate is not None else None
            candidate = (
                validation.get("prepared_deployment_candidate")
                if isinstance(validation, dict)
                else None
            )
            if isinstance(candidate, dict) and candidate.get("confirmation_required"):
                return {
                    "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                    "workspace": _workspace(state),
                    "result": effect(None),
                }

            def validate_submission(claim: dict[str, Any]) -> None:
                body.require_workspace_fence(state.authoring_workspace.identity())
                if validate is not None:
                    validate(claim)

            result = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind=operation_kind,
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                validate_under_claim=validate_submission,
                submit=effect,
                commit_before_submit=commit_before_submit,
                after_release=after_release,
            )
            return {
                "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                "workspace": _workspace(state),
                "result": result,
            }
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
            current_document = state.deployment_draft_session.retained_state().get(
                "document"
            )
            _require_draftable_composition(current_document, compiled.document)
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
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/mutations")
    def mutate_draft(body: UpdateDeploymentRequest) -> dict[str, Any]:
        try:
            before = state.deployment_draft_session.retained_state()
            if (
                before.get("draft_id") != body.draft_id
                or before.get("draft_revision") != body.expected_draft_revision
            ):
                state.deployment_draft_session.require_current(
                    body.draft_id, body.expected_draft_revision
                )
            compiled = state.deployment_draft_session.preview_update(
                body.path,
                body.value,
                body.operation,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            _require_draftable_composition(
                before.get("document"), compiled.document
            )
            commit_draft_edit(
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
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
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
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
            )
            commit_draft_edit(
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
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
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
            commit_draft_edit(
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
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    def save(body: SaveDeploymentRequest | SaveDeploymentAsRequest) -> dict[str, Any]:
        """Persist one exact Deployment Draft and adopt it as the Saved baseline.

        The capture, the write and the baseline adoption are three separate
        steps: the write happens only after admission, under the local
        authoring claim, so an admission refusal writes nothing; the adoption
        is one composite commit fenced by the captured workspace identity.
        One effect record decides both the HTTP outcome and the claim
        settlement: Not saved, Saved but not adopted (naming the written
        path), or unverified.
        """

        effects = OperationEffects()
        try:
            save_under_claim(body, effects)
        except Exception as error:
            raise_save_failure(
                error,
                effects,
                _raise,
                code="deployment_saved_baseline_not_adopted",
                document="Deployment Draft",
            )
        # The Deployment workspace carries its own Draft-matched identity;
        # the composite snapshot must not replace it (it breaks that contract).
        try:
            return _workspace(state)
        except Exception as error:
            _raise(error)

    def save_under_claim(
        body: SaveDeploymentRequest | SaveDeploymentAsRequest,
        effects: OperationEffects,
    ) -> None:
        target = body.path if isinstance(body, SaveDeploymentAsRequest) else None
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
        project_path = state.authoring_workspace.snapshot().project["document_path"]
        scenario_member = (
            target is None and project_path is not None
            and prepared.resolved_target == str(
                Path(project_path) / SCENARIO_PROJECT_MEMBERS["deployment_draft"]
            )
        )
        saved_path = prepared.resolved_target
        source_root = state.deployment_draft_session.source_project_root

        def write_artifact_under_claim() -> None:
            nonlocal saved_path
            # The adoption's fences, checked before the write: a stale
            # Save is refused before any effect.
            body.require_workspace_fence(state.authoring_workspace.identity())
            state.deployment_draft_session.require_current(
                prepared.draft_id, prepared.base_revision
            )
            # A prior completed write of this exact document is resolved
            # without repeating it.
            intended = prepared.canonical_json
            if _published_deployment_canonical(state, prepared.target_path) == intended:
                return
            try:
                saved_path = save_deployment_artifact(
                    intended,
                    prepared.target_path,
                    allowed_roots=_write_roots(state),
                    overwrite=prepared.overwrite,
                )
            except Exception:
                # One atomic replacement: the target holds either its
                # previous content (Not saved) or exactly this document.
                if _published_deployment_canonical(
                    state, prepared.target_path
                ) == intended:
                    effects.destination_published(saved_path)
                raise
            effects.destination_published(saved_path)

        def prepare_saved_project() -> dict[str, Any]:
            deployment = json.loads(prepared.canonical_json)
            if source_root is not None and source_root != project_path:
                try:
                    deployment = rebase_deployment_intent_sources(
                        deployment,
                        source_project_root=Path(source_root),
                        destination_project_root=Path(project_path),
                    )
                except DeploymentIntentError as error:
                    raise DeploymentDraftError(
                        "scenario_deployment_intent_invalid", str(error),
                        status_code=400,
                    ) from error
            document, path, digest = save_scenario_member(
                project_path, "deployment_draft", deployment,
                allowed_roots=state.allowed_fs_roots, effects=effects,
            )
            return scenario_project_facts(
                document, document_path=path, source_document_digest=digest
            )

        # Both forms are written under the claim, after admission: a
        # Scenario member with its manifest, a standalone artifact as the
        # under-claim step before the commit.
        commit_local_authoring(
            state,
            body,
            operation_kind=prepared.tool_id,
            session="whole-workspace" if scenario_member else "deployment",
            validate_under_claim=(
                None if scenario_member else write_artifact_under_claim
            ),
            prepare_project_under_claim=(
                prepare_saved_project if scenario_member else None
            ),
            install=lambda: state.deployment_draft_session.install_prepared_save(
                prepared, saved_path=saved_path
            ),
            effects=effects,
        )

    @router.post("/api/deployment/save")
    def save_draft(body: SaveDeploymentRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/save-as")
    def save_draft_as(body: SaveDeploymentAsRequest) -> dict[str, Any]:
        return save(body)

    @router.post("/api/deployment/discard")
    def discard(body: DiscardDeploymentRequest) -> dict[str, Any]:
        try:
            # Discard restores only the captured authoring input whose exact
            # candidate was published by Manager and Program binding.
            def read_applied() -> tuple[dict[str, Any], str]:
                current_projection = state.published_scenario()
                current = current_projection.get("published_scenario")
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
                applied = state.applied_deployment_for_publication(current)
                if applied is None:
                    raise DeploymentDraftError(
                        "deployment_applied_projection_mismatch",
                        "this Composer process has no exact Applied Deployment baseline",
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

            def validate_applied_unchanged() -> None:
                _current, current_identity = read_applied()
                if current_identity != applied_identity:
                    raise DeploymentDraftError(
                        "deployment_applied_changed",
                        "Manager Applied Deployment changed before Discard committed",
                        status_code=409,
                    )

            commit_draft_edit(
                state,
                body,
                operation_kind="discard_deployment_draft",
                session="deployment",
                validate=validate_applied_unchanged,
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/revert", status_code=202)
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
        scenario_id = state.scenario_document_id
        if (
            not isinstance(program, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_document_digest, str)
            or not isinstance(scenario_id, str)
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
                    "scenario_id": scenario_id,
                    "source_document_digest": source_document_digest,
                    "deployment": saved,
                    "model_source_closure": source_closure,
                    "program": program,
                    "program_role_assignment": assignment,
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
        compiled_saved = state.deployment_draft_session.preview_source(
            json.dumps(saved, ensure_ascii=False, allow_nan=False)
        )
        intent = {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "scene_initialization": "initialize",
            "operation_kind": "revert_deployment",
            "effect_class": "external",
            "external_kind": "revert_deployment",
            "scenario_id": scenario_id,
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "program": deepcopy(program),
            "program_role_assignment": assignment,
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
        }

        prepared_candidate: dict[str, Any] | None = None

        def validate(_claim: dict[str, Any] | None) -> dict[str, Any]:
            nonlocal prepared_candidate
            current = state.deployment_draft_session.retained_state()
            if current != retained:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before Revert",
                    status_code=409,
                )
            if (
                _claim is not None
                and _capture_model_sources(state, saved) != source_closure
            ):
                raise DeploymentDraftError(
                    "activation_model_source_changed",
                    "A model source changed during activation capture",
                    status_code=409,
                )
            if _claim is None:
                prepared_candidate = state.execute_authored_deployment_candidate(
                    intent, None, apply=True, prepare_only=True
                )
            return {"prepared_deployment_candidate": prepared_candidate}

        def effect(claim: dict[str, Any] | None) -> dict[str, Any]:
            if prepared_candidate is None:
                raise DeploymentDraftError(
                    "deployment_revert_preparation_missing",
                    "Deployment Revert has no captured preparation",
                    status_code=409,
                )
            result = state.execute_authored_deployment_candidate(
                intent,
                claim,
                apply=True,
                prepared=prepared_candidate,
            )
            return result

        def commit_saved_before_submit() -> None:
            identity = state.authoring_workspace.identity()
            state.authoring_workspace.commit_deployment(
                draft_id=body.draft_id,
                expected_identity=identity,
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled_saved,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
                notify=False,
            )

        if not apply:
            # Preview is an effect-free calculation. It uses the same exact
            # input fence as Apply, but never acquires authoring admission or
            # creates a Manager operation.
            try:
                identity = state.authoring_workspace.identity()
                body.require_workspace_fence(identity)
                validate(None)
                body.require_workspace_fence(state.authoring_workspace.identity())
                result = effect(None)
                return {
                    "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                    "workspace": _workspace(state),
                    "result": result,
                }
            except Exception as error:
                _raise(error)

        return execute(
            body,
            "revert_deployment",
            prepare=lambda: intent,
            validate=validate,
            commit_before_submit=commit_saved_before_submit,
            effect=effect,
            after_release=state.authoring_workspace.publish_commit_wake,
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
        scenario_id = state.scenario_document_id
        if (
            not isinstance(program, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_document_digest, str)
            or not isinstance(scenario_id, str)
        ):
            _raise(
                DeploymentDraftError(
                    "activation_input_incomplete",
                    "Save or load one complete Scenario document with Program, "
                    "Deployment, and explicit role assignment before preparation",
                    status_code=409,
                )
            )
        scope = (
            body.expected_activation_scope
            if isinstance(body, ApplyDeploymentCandidateRequest)
            else None
        )
        if scope is not None:
            _require_activation_scope(state, scope)
        source_closure = _capture_model_sources(state, deployment)
        if apply:
            # Nothing to apply: every input already is the Manager's Applied
            # activation. Refused before admission, so no effect is possible.
            # An Applied fact that cannot be read now never proves a no-op.
            try:
                publication = state.published_scenario().get("published_scenario")
            except HeadlessClientError:
                publication = None
            applied_context = (
                state.applied_activation_for_publication(publication)
                if isinstance(publication, dict)
                else None
            )
            program_revision = (
                program.get("program_revision") if isinstance(program, dict) else None
            )
            if (
                applied_context is not None
                and applied_context.get("deployment") == deployment
                and applied_context.get("model_source_closure") == source_closure
                and applied_context.get("program_role_assignment") == assignment
                and applied_context.get("source_document_digest") == source_document_digest
                and applied_context.get("scenario_id") == scenario_id
                and isinstance(program_revision, str)
                and _applied_program_revision(state) == program_revision
            ):
                raise refused_before_effect(
                    409,
                    {
                        "code": "deployment_apply_nothing_to_apply",
                        "message": (
                            "Nothing to apply: the Deployment and Program "
                            "Drafts already match Applied"
                        ),
                    },
                )
        captured_input_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "scenario_id": scenario_id,
                    "source_document_digest": source_document_digest,
                    "deployment": deployment,
                    "model_source_closure": source_closure,
                    "program": program,
                    "program_role_assignment": assignment,
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
            "scenario_id": scenario_id,
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "deployment": deepcopy(deployment),
            "program": deepcopy(program),
            "program_role_assignment": assignment,
            "expected_manager_current": {
                "retained_bundle_id": current_bundle,
                "graph_revision": current_graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "scene_initialization": "initialize",
            "manager_request_id": "deployment-candidate-manager-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32],
            "correlation_id": (
                body.correlation_id
                if isinstance(body, ApplyDeploymentCandidateRequest)
                and body.correlation_id is not None
                else body.request_id
            ),
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

        def validate(_claim: dict[str, Any] | None) -> dict[str, Any] | None:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            if scope is not None:
                _require_activation_scope(state, scope)
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
                or (
                    _claim is not None
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
            # Compile or resolve the confirmation exactly once before Manager
            # mutation. Manager durably owns the accepted operation; Composer
            # retains no external-effect replay journal.
            if _claim is not None:
                return {"prepared_deployment_candidate": prepared["candidate"]}
            try:
                prepared["candidate"] = (
                    state.execute_authored_deployment_candidate(
                        intent,
                        _claim,
                        apply=apply,
                        prepare_only=True,
                    )
                )
            except AuthoringOperationError as error:
                if error.code not in {"confirmation_expired", "confirmation_stale"}:
                    raise
                raise HTTPException(
                    status_code=409,
                    detail={"code": error.code, "message": str(error)},
                ) from error
            except HeadlessClientError as error:
                raise headless_client_http_exception(error) from error
            return {"prepared_deployment_candidate": prepared["candidate"]}

        def effect(claim: dict[str, Any] | None) -> dict[str, Any]:
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
                    failure_phase = "manager_submission"
                else:
                    failure_phase = "candidate_submission"
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
            return result

        if not apply:
            try:
                body.require_workspace_fence(state.authoring_workspace.identity())
                validate(None)
                result = effect(None)
                return {
                    "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                    "workspace": _workspace(state),
                    "result": result,
                }
            except Exception as error:
                _raise(error)

        return execute(
            body,
            intent["operation_kind"],
            prepare=lambda: intent,
            validate=validate,
            effect=effect,
        )

    @router.post("/api/deployment/draft/preview")
    def preview_authored_candidate(
        body: PreviewDeploymentCandidateRequest,
    ) -> dict[str, Any]:
        return authored_candidate(body, apply=False)

    @router.post("/api/deployment/draft/apply", status_code=202)
    def apply_authored_candidate(
        body: ApplyDeploymentCandidateRequest,
    ) -> dict[str, Any]:
        if body.correlation_id is None:
            return authored_candidate(body, apply=True)
        try:
            response = authored_candidate(body, apply=True)
        except HTTPException as error:
            detail = error.detail if isinstance(error.detail, dict) else {}
            log_reset_apply_link(
                "deployment_apply",
                correlation_id=body.correlation_id,
                request_id=body.request_id,
                outcome="error",
                error_code=detail.get("code"),
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
            )
            raise
        result = response.get("result") if isinstance(response, dict) else None
        manager = result.get("manager_apply") if isinstance(result, dict) else None
        log_reset_apply_link(
            "deployment_apply",
            correlation_id=body.correlation_id,
            request_id=body.request_id,
            outcome=str(result.get("status")) if isinstance(result, dict) else "unknown",
            apply_id=manager.get("apply_id") if isinstance(manager, dict) else None,
            apply_generation=(
                manager.get("apply_generation") if isinstance(manager, dict) else None
            ),
            draft_id=body.draft_id,
            draft_revision=body.expected_draft_revision,
        )
        return response

    return router
