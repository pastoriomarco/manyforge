"""Thin HTTP boundary for the separate Composer Deployment Draft."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
import logging
from pathlib import Path
from typing import Any, Literal, NoReturn

from fastapi import APIRouter, HTTPException
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
from .deployment_draft import CompiledDeploymentDocument, DeploymentDraftError
from .draft_session import DraftSessionError
from .state import ComposerState


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_LOGGER = logging.getLogger(__name__)


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


class UpdateControlledStopRequest(_Fence):
    control_binding_id: str = Field(min_length=1, max_length=256)
    control_binding_revision: str = Field(min_length=1, max_length=256)
    profile: dict[str, Any] | None
    timing: dict[str, Any] | None


class UpdateMotionTargetLimitRequest(_Fence):
    controllable_component_id: str = Field(min_length=1, max_length=256)
    command_target_id: str = Field(min_length=1, max_length=256)
    maximum_tcp_linear_speed_mps: float = Field(gt=0.0, le=1000.0)


class SaveDeploymentRequest(_Fence):
    pass


class SaveDeploymentAsRequest(_Fence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class DiscardDeploymentRequest(_Fence):
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
                body.envelope(),
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

    @router.post("/api/deployment/draft/controlled-stop")
    def update_controlled_stop(body: UpdateControlledStopRequest) -> dict[str, Any]:
        prepared: dict[str, CompiledDeploymentDocument] = {}

        def validate(_claim: dict[str, Any]) -> None:
            prepared["compiled"] = (
                state.deployment_draft_session.preview_controlled_stop_update(
                    body.profile,
                    control_binding_id=body.control_binding_id,
                    control_binding_revision=body.control_binding_revision,
                    timing=body.timing,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
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
            "mutate_deployment_controlled_stop",
            prepare=lambda: {"effect_class": "local"},
            validate=validate,
            effect=effect,
        )

    @router.post("/api/deployment/draft/motion-target-limit")
    def update_motion_target_limit(
        body: UpdateMotionTargetLimitRequest,
    ) -> dict[str, Any]:
        prepared: dict[str, CompiledDeploymentDocument] = {}

        def validate(_claim: dict[str, Any]) -> None:
            prepared["compiled"] = (
                state.deployment_draft_session.preview_motion_target_limit_update(
                    controllable_component_id=body.controllable_component_id,
                    command_target_id=body.command_target_id,
                    maximum_tcp_linear_speed_mps=(body.maximum_tcp_linear_speed_mps),
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
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
            "mutate_deployment_motion_target_limit",
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
            resolved = str(
                Path(target).expanduser().parent.resolve() / Path(target).name
            )
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
        target_resolved = str(
            Path(target).expanduser().parent.resolve() / Path(target).name
        )
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
            # Discard only restores the local Draft from the authoritative
            # Manager publication.  It must remain usable while the newly
            # published runtime is still converging; requiring the strict
            # execution-ready Scenario here made the cycle fail closed during
            # the normal post-Apply settling window.  Keep a strict fallback
            # for lightweight authoring doubles that expose no publication
            # projection.
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
            prepared["compiled"] = state.deployment_draft_session.preview_source(
                json.dumps(applied, ensure_ascii=False, allow_nan=False)
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
        # Revert changes the active Deployment, so it must also rebind the
        # already Applied Program to the reopened Scenario closure.  Capture
        # that exact Program and its compare-and-set digest before touching
        # Manager; otherwise Manager can become active with an old Program
        # record, leaving the cell in an execution-blocked mismatch.
        try:
            applied_program = state.headless_client.program_applied()
        except HeadlessClientError as error:
            _raise(
                DeploymentDraftError(
                    "deployment_applied_program_unavailable",
                    "Deployment Revert requires the current Applied Program "
                    f"to be active ({error.code})",
                    status_code=409,
                )
            )
        program = applied_program.get("program")
        source = applied_program.get("source")
        applied_record_digest = (
            source.get("record_digest") if isinstance(source, dict) else None
        )
        if not isinstance(program, dict) or not isinstance(applied_record_digest, str):
            _raise(
                DeploymentDraftError(
                    "deployment_applied_program_unavailable",
                    "Deployment Revert requires the exact current Applied Program",
                    status_code=409,
                )
            )
        try:
            target_selection = state.headless_client.scenario_load_selection(
                candidate["scenarioId"]
            )
        except HeadlessClientError as error:
            _raise(
                DeploymentDraftError(
                    "deployment_saved_candidate_unavailable",
                    "Saved Deployment candidate could not be reopened "
                    f"({error.code})",
                    status_code=409,
                )
            )
        if target_selection.get("program") != program:
            _raise(
                DeploymentDraftError(
                    "deployment_saved_candidate_program_mismatch",
                    "Saved Deployment candidate belongs to another Applied Program",
                    status_code=409,
                )
            )
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
                "expected_admission_generation": (body.expected_admission_generation),
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
        manager_request_id = (
            "deployment-revert-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32]
        )
        manager_apply = {
            "request_id": manager_request_id,
            "request_owner_epoch": manager_epoch,
            "source_label": "manyforge_composer_deployment_revert",
            "correlation_id": body.request_id,
            "deployment_artifact_id": candidate["deploymentArtifactId"],
            "deployment_revision": candidate["deploymentRevision"],
            "expected_current": receipt["expected_current"],
            "reject_if_affected_busy": True,
            "candidate_receipt": receipt,
        }
        # A Revert is also valid when the saved candidate is already active
        # (for example after reopening an unchanged workspace).  Do not send
        # a redundant Manager Apply: the strict owner treats that as an
        # invalid staged transition.  Program rebind and authoring journaling
        # still run below, while a genuinely different active candidate keeps
        # the normal Manager transition.
        if (
            active.get("graph_revision") == receipt.get("candidate_graph_revision")
            and active.get("retained_bundle_id")
            == receipt.get("candidate_retained_bundle_id")
        ):
            manager_apply = None
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
            "manager_apply": manager_apply,
            "scenario_id": candidate["scenarioId"],
            "expected_scenario_catalog_revision": candidate[
                "scenarioCatalogRevision"
            ],
            "program": deepcopy(program),
            "expected_applied_record_digest": applied_record_digest,
            "deployment": deepcopy(saved),
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
            compilation = {
                "schema_version": "manyforge.deployment_candidate_compilation.v1",
                "scenario_id": candidate["scenarioId"],
                "scenario_catalog_revision": candidate[
                    "scenarioCatalogRevision"
                ],
                "deployment_artifact_id": candidate["deploymentArtifactId"],
                "deployment_revision": candidate["deploymentRevision"],
                "retained_bundle_id": candidate["retainedBundleId"],
                "graph_revision": receipt["candidate_graph_revision"],
                "program_id": target_selection["program_id"],
                "program_revision": target_selection["program_revision"],
                "program_binding_id": candidate["programBindingId"],
            }
            prepared_candidate = {
                "compilation": compilation,
                "candidate_preview": preview,
                "manager_apply": intent["manager_apply"],
                "confirmation_required": False,
                "confirmation_token": None,
                "preparation": None,
                "phases_ms": {},
            }
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
                    body.envelope(),
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
        current_projection = state.current_scenario()
        current = current_projection.get("current_scenario")
        if current_projection.get("ready") is not True or not isinstance(current, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_current_scenario_unavailable",
                    "Deployment preview requires one exact current Scenario",
                    status_code=409,
                )
            )
        if deployment_state["document"].get("deployment_artifact_id") != current.get(
            "deployment_artifact_id"
        ):
            _raise(
                DeploymentDraftError(
                    "deployment_candidate_artifact_mismatch",
                    "The Deployment Draft addresses another artifact; load its "
                    "Scenario before previewing it",
                    status_code=409,
                )
            )
        applied = state.headless_client.program_applied()
        program = applied.get("program")
        source = applied.get("source")
        record_digest = (
            source.get("record_digest") if isinstance(source, dict) else None
        )
        if not isinstance(program, dict) or not isinstance(record_digest, str):
            _raise(
                DeploymentDraftError(
                    "deployment_applied_program_unavailable",
                    "Deployment preview requires the exact current Applied Program",
                    status_code=409,
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
            "scenario_id": current["scenario_id"],
            "expected_scenario_catalog_revision": current["scenario_catalog_revision"],
            "deployment": deepcopy(deployment),
            "program": deepcopy(program),
            "expected_applied_record_digest": record_digest,
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
            latest_current = state.current_scenario().get("current_scenario")
            latest_applied = state.headless_client.program_applied()
            latest_source = latest_applied.get("source")
            latest_digest = (
                latest_source.get("record_digest")
                if isinstance(latest_source, dict)
                else None
            )
            latest_bundle, latest_graph = manager_current()
            if (
                latest_current != current
                or latest_applied.get("program") != program
                or latest_digest != record_digest
                or (latest_bundle, latest_graph) != (current_bundle, current_graph)
            ):
                raise DeploymentDraftError(
                    "deployment_candidate_source_changed",
                    "Current Scenario, Applied Program, or Manager selection "
                    "changed before candidate compilation",
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
