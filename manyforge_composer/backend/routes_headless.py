"""Explicit thin Composer routes over the independent headless HTTP API."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import Annotated, Any, Literal
from urllib.parse import quote

from fastapi import APIRouter, HTTPException, Path, Query, Request
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, ConfigDict, Field, model_validator

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.close_wire import LifecycleOwnerSettlementRequest, LifecycleTargetRetirementIntent
from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
)
from manyforge_headless_api.control_protocol import (
    ExecutionCommandRequestDTO,
    is_stop_command,
)
from manyforge_headless_api.models import (
    CanonicalEmptyActivationPreparationRequest,
    ManagerDeploymentCandidatePreviewRequest,
    ManagerApplyRequest,
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ComponentLifecycleRequest,
    ComponentProcessStopBody,
    FullStopIntentRequest,
    HistorianReconcileRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES,
    decode_runtime_scale_selector_query,
    RouteSelectionRequest,
    SceneTransactionRequest,
    StopIntentRequest,
    StopFaultRecoveryIntentRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionResumeRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramCycleResetRequest,
    ProgramNodeCorrectionPreviewRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
    RuntimeBlackboardWriteRequest,
    SkillExpansionRequest,
)
from manyforge_headless_api.history_query import (
    HistorianCaptureRequest,
    HistorianQueryRequest,
    HistorianDiscoveryRequest,
    HistorianDebugRequest,
)

from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
)
from .draft_session import DraftSessionError
from .authoring_workspace import AuthoringWorkspaceIdentity, AuthoringWorkspaceError
from .source_model_client import (
    ActiveSourceModelProjectionError,
    read_active_source_model_context,
)
from .state import ComposerState


_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_ScenarioPath = Annotated[
    str, Path(min_length=1, max_length=128, pattern=r"^[a-z][a-z0-9_.-]*$")
]
_MAXIMUM_ALARM_PAGE = 128
_MAXIMUM_HISTORY_PAGE = 32
_SCENE_RESULT_FIELDS = {
    "api_version",
    "committed_projection_revision",
    "error",
    "ok",
    "projection_status",
    "replay_status",
    "request_id",
    "request_owner_epoch",
    "transaction_sequence",
    "update_status",
}


class _SceneWorkspaceIdentity(BaseModel):
    """The existing composite identity, carried only by Close's Scene cleanup."""
    model_config = ConfigDict(extra="forbid", strict=True)
    workspace_epoch: str = Field(alias="workspaceEpoch", min_length=1, max_length=256)
    workspace_revision: int = Field(alias="workspaceRevision", ge=1, le=_MAXIMUM_SAFE_INTEGER)
    program_draft_id: str = Field(alias="programDraftId", min_length=1, max_length=256)
    program_draft_revision: int = Field(alias="programDraftRevision", ge=0, le=_MAXIMUM_SAFE_INTEGER)
    deployment_draft_id: str = Field(alias="deploymentDraftId", min_length=1, max_length=256)
    deployment_draft_revision: int = Field(alias="deploymentDraftRevision", ge=0, le=_MAXIMUM_SAFE_INTEGER)

    def identity(self) -> AuthoringWorkspaceIdentity:
        return AuthoringWorkspaceIdentity(**self.model_dump())


class ComposerSceneTransactionRequest(SceneTransactionRequest):
    """Scene-owner request with a Draft fence only for authored mutations."""

    model_config = ConfigDict(extra="forbid")

    expected_authoring_workspace: _SceneWorkspaceIdentity | None = None
    expected_draft_id: str | None = Field(default=None, min_length=1, max_length=256)
    expected_draft_revision: int | None = Field(
        default=None,
        ge=0,
        le=_MAXIMUM_SAFE_INTEGER,
    )

    @model_validator(mode="after")
    def validate_authority_scope(self) -> "ComposerSceneTransactionRequest":
        if self.expected_authoring_workspace is not None and self.operation.kind not in {"remove_object", "set_collision_policy", "detach_carried_object_at_current_pose"}:
            raise ValueError("the composite Close fence is only valid for Close Scene cleanup")
        authored = self.operation.kind in {
            "add_object",
            "upsert_object",
            "remove_object",
            "update_object_pose",
            "set_collision_policy",
        }
        draft_fenced = (
            self.expected_draft_id is not None
            and self.expected_draft_revision is not None
        )
        if authored and (
            self.expected_admission_generation is None or not draft_fenced
        ):
            raise ValueError(
                "authored scene mutation requires admission and Program Draft fences"
            )
        if not authored and (
            self.expected_admission_generation is not None
            or self.expected_draft_id is not None
            or self.expected_draft_revision is not None
        ):
            raise ValueError(
                "runtime-owned scene mutation cannot carry authoring fences"
            )
        return self


def _translate(error: HeadlessClientError) -> HTTPException:
    return HTTPException(
        status_code=error.status_code or 503,
        detail={
            "code": error.code,
            "details": error.details,
            "message": str(error),
        },
    )


def _call(callback):
    try:
        return callback()
    except HeadlessClientError as error:
        raise _translate(error) from error


# Every recovery action except Reset resumes the run (Retry, continue).
_RESETTING_RECOVERY_ACTIONS = frozenset({"reset_cycle"})


def _resumes_execution(command_kind: str, payload: Any) -> bool:
    """Commands that start or resume Program progression or motion."""

    if command_kind == "execution_recovery":
        action = payload.get("action_id") if isinstance(payload, dict) else None
        return action not in _RESETTING_RECOVERY_ACTIONS
    return command_kind.startswith("program_")


def _require_applied_authoring_drafts(state: ComposerState) -> None:
    """Refuse Execute, Run, Resume and Retry while Draft edits are unapplied.

    Composer owns the Draft, so it admits these commands only when the Draft
    is exactly what is Applied; the edits must be Applied (which Resets a
    held run first) or Discarded.
    """

    draft = state.deployment_draft_session.snapshot()["draft"]["document"]
    program_snapshot = state.draft_session.snapshot()
    program_revision = program_snapshot["program_revision"]
    if program_snapshot.get("program_loaded") is True and program_revision is None:
        raise HTTPException(
            status_code=409,
            detail={
                "code": "execution_program_draft_unapplied",
                "message": (
                    "Apply or Discard first: the Program Draft has unapplied "
                    "changes"
                ),
            },
        )
    if draft is None and program_revision is None:
        return
    try:
        projection = state.published_scenario()
    except HeadlessClientError as error:
        raise _translate(error) from error
    published = projection.get("published_scenario")
    if program_revision is not None and (
        projection.get("ready") is not True
        or not isinstance(published, dict)
        or published.get("program_revision") != program_revision
    ):
        raise HTTPException(
            status_code=409,
            detail={
                "code": "execution_program_draft_unapplied",
                "message": (
                    "Apply or Discard first: the Program Draft has unapplied "
                    "changes"
                ),
            },
        )
    if draft is None:
        return
    applied = (
        state.applied_deployment_for_publication(published)
        if projection.get("ready") is True and isinstance(published, dict)
        else None
    )
    if applied != draft:
        raise HTTPException(
            status_code=409,
            detail={
                "code": "execution_deployment_draft_unapplied",
                "message": (
                    "Apply or Discard first: the Deployment Draft has unapplied "
                    "changes"
                ),
            },
        )


def _call_scene(callback, body: SceneTransactionRequest):
    try:
        return callback()
    except HeadlessClientError as error:
        response = error.response
        context = body.request_context
        if (
            error.status_code is not None
            and isinstance(response, dict)
            and set(response) == _SCENE_RESULT_FIELDS
            and response.get("api_version") == "manyforge.manager_command_response.v1"
            and response.get("ok") is False
            and response.get("request_id") == context.request_id
            and response.get("request_owner_epoch") == context.request_owner_epoch
            and isinstance(response.get("error"), dict)
            and response["error"].get("code") == error.code
        ):
            return JSONResponse(status_code=error.status_code, content=response)
        raise _translate(error) from error


def create_headless_router(
    client: HeadlessHttpClient,
    state: ComposerState,
    execution_controls: ExecutionControlClient | None = None,
) -> APIRouter:
    """Expose only bounded reads and attributed intents; own no reducer."""

    owns_execution_controls = execution_controls is None
    execution_controls = execution_controls or ExecutionControlClient(
        thread_name_prefix="manyforge_composer_control",
    )

    @asynccontextmanager
    async def lifespan(_app):
        try:
            yield
        finally:
            if owns_execution_controls:
                execution_controls.close()

    router = APIRouter(lifespan=lifespan)

    async def control_call(callback, *, stop: bool = False):
        try:
            lane = execution_controls.stop if stop else execution_controls.ordinary
            # Encode in the same bounded worker as the synchronous owner call.
            # A plain mapping would be walked and encoded on the shared ASGI
            # loop, where a large read response could delay an accepted Stop.
            return await lane(lambda: JSONResponse(content=callback()))
        except HeadlessClientError as error:
            raise _translate(error) from error
        except ExecutionControlBusyError as error:
            raise HTTPException(
                status_code=503,
                detail={
                    "code": "execution_control_ingress_busy",
                    "message": str(error),
                    "details": {
                        "lane": error.lane.value,
                        "maximum_admitted": error.maximum_admitted,
                    },
                },
            ) from error

    @router.get("/api/runtime")
    def runtime() -> dict[str, Any]:
        return _call(client.runtime)

    @router.get("/api/component-overview")
    def component_overview() -> Response:
        return Response(
            content=_call(client.component_overview_payload),
            media_type="application/json",
        )

    @router.get("/api/manager/status")
    def manager_status() -> dict[str, Any]:
        return _call(client.manager_status)

    @router.post("/api/activation-preparations")
    def prepare_canonical_empty(body: CanonicalEmptyActivationPreparationRequest) -> dict[str, Any]:
        return _call(lambda: client.prepare_activation(body.model_dump(mode="json")))

    @router.post("/api/manager/deployment-candidates/preview")
    def preview_manager_candidate(body: ManagerDeploymentCandidatePreviewRequest) -> dict[str, Any]:
        return _call(lambda: client.manager_deployment_candidate_preview(body.model_dump(mode="json")))

    @router.post("/api/manager/apply")
    def apply_manager_candidate(body: ManagerApplyRequest) -> dict[str, Any]:
        return _call(lambda: client.manager_apply(body.model_dump(mode="json")))

    @router.get("/api/manager/canonical-empty-selection")
    def canonical_empty_selection() -> dict[str, Any]:
        return _call(client.canonical_empty_selection)

    @router.get("/api/cclc/targets/retirement-offer")
    def lifecycle_target_retirement_offer() -> dict[str, Any]:
        return _call(client.lifecycle_target_retirement_offer)

    @router.post("/api/cclc/targets/retire")
    def retire_lifecycle_targets(body: LifecycleTargetRetirementIntent) -> dict[str, Any]:
        return _call(lambda: client.retire_lifecycle_targets(body.model_dump(mode="json")))

    @router.post("/api/cclc/targets/retire/result")
    def lifecycle_target_retirement_result(body: LifecycleTargetRetirementIntent) -> dict[str, Any]:
        return _call(lambda: client.lifecycle_target_retirement_result(body.model_dump(mode="json")))

    @router.post("/api/cclc/targets/settlement")
    def lifecycle_owner_settlement(body: LifecycleOwnerSettlementRequest) -> dict[str, Any]:
        return _call(lambda: client.lifecycle_owner_settlement(body.model_dump(mode="json")))

    @router.get("/api/manager/graph")
    def manager_graph() -> dict[str, Any]:
        return _call(client.manager_graph)

    @router.get("/api/manager/diff")
    def manager_diff() -> dict[str, Any]:
        return _call(client.manager_diff)

    @router.get("/api/cclc/components/{component_id}")
    def cclc_component(
        component_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.cclc_component(component_id))

    @router.get("/api/cclc/owner")
    def cclc_owner_identity() -> dict[str, Any]:
        return _call(client.cclc_owner_identity)

    @router.get("/api/cclc/components/{component_id}/route-templates")
    def cclc_route_templates(
        component_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.cclc_route_templates(component_id))

    @router.get("/api/cclc/components/{component_id}/process/stop-offer")
    def component_process_stop_offer(component_id: Annotated[str, Path(min_length=1, max_length=128)]) -> dict[str, Any]:
        return _call(lambda: client.component_process_stop_offer(component_id))

    @router.post("/api/cclc/components/{component_id}/process/stop")
    def stop_component_process(
        component_id: Annotated[str, Path(min_length=1, max_length=128)],
        body: ComponentProcessStopBody,
    ) -> dict[str, Any]:
        if component_id != body.request.controllable_component_id:
            raise HTTPException(status_code=400, detail={"code": "invalid_request", "message": "Process Stop component path does not match its request", "details": {}, "request_id": body.request.request_context.request_id})
        return _call(lambda: client.stop_component_process(component_id, body.model_dump(mode="json")))

    @router.post("/api/cclc/components/{component_id}/process/stop/result")
    def component_process_stop_result(
        component_id: Annotated[str, Path(min_length=1, max_length=128)],
        body: ComponentProcessStopBody,
    ) -> dict[str, Any]:
        if component_id != body.request.controllable_component_id:
            raise HTTPException(status_code=400, detail={"code": "invalid_request", "message": "Process Stop component path does not match its request", "details": {}, "request_id": body.request.request_context.request_id})
        return _call(lambda: client.component_process_stop_result(component_id, body.model_dump(mode="json")))

    @router.post("/api/cclc/lifecycle/requests")
    def cclc_lifecycle_submit(
        body: ComponentLifecycleRequest,
    ) -> dict[str, Any]:
        payload = body.model_dump(mode="json")
        if payload.get("close_incident_recovery") is None:
            payload.pop("close_incident_recovery", None)
        if payload.get("program_node_evidence") is None:
            payload.pop("program_node_evidence", None)
        return _call(lambda: client.submit_component_lifecycle(payload))

    @router.get("/api/cclc/lifecycle/transactions/{transaction_id}")
    def cclc_lifecycle_status(
        transaction_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.component_lifecycle_status(transaction_id))

    @router.post("/api/cclc/lifecycle/requests/result")
    def cclc_lifecycle_request_result(
        body: ComponentLifecycleRequest,
    ) -> dict[str, Any]:
        payload = body.model_dump(mode="json")
        if payload.get("close_incident_recovery") is None:
            payload.pop("close_incident_recovery", None)
        if payload.get("program_node_evidence") is None:
            payload.pop("program_node_evidence", None)
        return _call(lambda: client.component_lifecycle_request_result(payload))

    @router.post("/api/cclc/routes/requests")
    def cclc_route_submit(body: RouteSelectionRequest) -> dict[str, Any]:
        return _call(
            lambda: client.submit_route_selection(body.model_dump(mode="json"))
        )

    @router.get("/api/cclc/routes/transactions/{transaction_id}")
    def cclc_route_status(
        transaction_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.route_selection_status(transaction_id))

    @router.get("/api/node-environment")
    def node_environment() -> dict[str, Any]:
        return _call(client.node_environment)

    @router.get("/api/source-model")
    def source_model() -> dict[str, Any]:
        try:
            return read_active_source_model_context(state).payload
        except (
            ActiveNodeEnvironmentProjectionError,
            ActiveSourceModelProjectionError,
        ) as error:
            raise HTTPException(
                status_code=error.status_code,
                detail={"code": error.code, "message": str(error)},
            ) from error

    @router.get("/api/scenarios/current")
    def current_scenario() -> dict[str, Any]:
        try:
            return state.current_scenario()
        except HeadlessClientError as error:
            raise _translate(error) from error

    @router.get("/api/program/applied")
    def program_applied() -> dict[str, Any]:
        return _call(client.program_applied)

    @router.get("/api/program/applied/status")
    def program_applied_status() -> dict[str, Any]:
        # Derived executable/non-executable status of the Applied Program.
        return _call(client.program_applied_status)

    @router.get("/api/program/runs")
    def program_runs(
        active_only: bool = False,
        limit: Annotated[int, Query(ge=1, le=4_096)] = 128,
    ) -> dict[str, Any]:
        return _call(
            lambda: client.program_runs(
                active_only=active_only,
                limit=limit,
            )
        )

    @router.post("/api/program/runs")
    async def program_run_start(body: ProgramRunStartRequest) -> dict[str, Any]:
        def submit():
            draft = state.draft_session.snapshot()
            if draft.get("program_revision") != body.draft_program_revision:
                raise HTTPException(
                    status_code=409,
                    detail={
                        "code": "program_run_draft_applied_mismatch",
                        "message": "Run does not identify the current authoritative Draft",
                    },
                )
            _require_applied_authoring_drafts(state)
            return client.program_run_start(body.model_dump(mode="json"))

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.get("/api/cycle/status")
    def program_cycle_status() -> dict[str, Any]:
        return _call(client.program_cycle_status)

    @router.post("/api/program/cycle/reset")
    async def program_cycle_reset(
        body: ProgramCycleResetRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: client.program_cycle_reset(body.model_dump(mode="json"))
        )

    @router.get("/api/program/runs/{program_run_id}/runtime-controls")
    def program_runtime_controls(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.program_runtime_controls(program_run_id))

    def require_control_path(program_run_id: str, body: Any) -> None:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": "Program-run identity in path and body must match",
                },
            )

    @router.post("/api/program/runs/{program_run_id}/node-correction-previews")
    def program_node_correction_preview(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramNodeCorrectionPreviewRequest,
    ) -> dict[str, Any]:
        """Advisory preview only: not an execution command or Draft change."""
        require_control_path(program_run_id, body)
        return _call(
            lambda: client.program_node_correction_preview(
                program_run_id, body.model_dump(mode="json")
            )
        )

    @router.post("/api/program/runs/{program_run_id}/blackboard-values")
    async def write_runtime_blackboard(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeBlackboardWriteRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id,
                "blackboard-values",
                body.model_dump(mode="json"),
            )
        )

    def require_scenario_path(scenario_id: str, body: Any) -> None:
        if body.scenario_id != scenario_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "persistent_key_path_invalid",
                    "message": "Scenario identity in path and body must match",
                },
            )

    @router.get("/api/program/scenarios/{scenario_id}/persistent-values")
    def program_persistent_values(scenario_id: _ScenarioPath) -> dict[str, Any]:
        return _call(lambda: client.program_persistent_values(scenario_id))

    @router.post("/api/program/scenarios/{scenario_id}/persistent-values")
    def set_program_persistent_value(
        scenario_id: _ScenarioPath,
        body: PersistentKeyValueSetRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return _call(lambda: client.program_persistent_value_action(
            scenario_id, "set", body.model_dump(mode="json")
        ))

    @router.post("/api/program/scenarios/{scenario_id}/persistent-values/reset")
    def reset_program_persistent_value(
        scenario_id: _ScenarioPath,
        body: PersistentKeyValueResetRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return _call(lambda: client.program_persistent_value_action(
            scenario_id, "reset", body.model_dump(mode="json")
        ))

    @router.post("/api/program/scenarios/{scenario_id}/persistent-values/clear")
    def clear_program_persistent_values(
        scenario_id: _ScenarioPath,
        body: PersistentKeyValuesClearRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return _call(lambda: client.program_persistent_value_action(
            scenario_id, "clear", body.model_dump(mode="json")
        ))

    @router.post("/api/program/runs/{program_run_id}/manual-result")
    async def stage_manual_result(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ManualResultStageRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id, "manual-result", body.model_dump(mode="json")
            )
        )

    @router.post("/api/cycle/recovery-actions")
    async def cycle_recovery_action(
        body: CycleRecoveryActionRequest,
    ) -> dict[str, Any]:
        payload = body.model_dump(mode="json")

        def submit():
            if _resumes_execution("execution_recovery", payload):
                _require_applied_authoring_drafts(state)
            return client.cycle_recovery_action(payload)

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.get("/api/program/runs/{program_run_id}")
    def program_run_read(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.program_run(program_run_id))

    @router.post("/api/program/runs/{program_run_id}/stop")
    async def program_run_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": ("Program-run identity in path and body must match"),
                },
            )
        return await control_call(
            lambda: client.program_run_stop(
                program_run_id,
                body.model_dump(mode="json"),
            ),
            stop=True,
        )

    @router.post("/api/program/runs/{program_run_id}/motion-stop")
    async def program_run_motion_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionStopRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": ("Program-run identity in path and body must match"),
                },
            )
        return await control_call(
            lambda: client.program_run_motion_stop(
                program_run_id,
                body.model_dump(mode="json"),
            ),
            stop=True,
        )

    @router.post("/api/program/runs/{program_run_id}/motion-start")
    async def program_run_motion_start(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionStartRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": "Program-run identity in path and body must match",
                },
            )
        def submit():
            _require_applied_authoring_drafts(state)
            return client.program_run_motion_start(
                program_run_id, body.model_dump(mode="json")
            )

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.post("/api/program/runs/{program_run_id}/resume-motion")
    async def program_run_motion_resume(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionResumeRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": "Program-run identity in path and body must match",
                },
            )
        def submit():
            _require_applied_authoring_drafts(state)
            return client.program_run_motion_resume(
                program_run_id, body.model_dump(mode="json")
            )

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.post("/api/program/runs/{program_run_id}/full-stop")
    async def program_run_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": ("Program-run identity in path and body must match"),
                },
            )
        return await control_call(
            lambda: client.program_run_full_stop(
                program_run_id,
                body.model_dump(mode="json"),
            ),
            stop=True,
        )

    @router.post("/api/program/runs/{program_run_id}/resume-full-stop")
    async def program_run_resume_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": ("Program-run identity in path and body must match"),
                },
            )
        def submit():
            _require_applied_authoring_drafts(state)
            return client.program_run_resume_full_stop(
                program_run_id, body.model_dump(mode="json")
            )

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.post("/api/program/runs/{program_run_id}/reset")
    async def program_run_reset_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramFullStopResetRequest,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "program_run_path_invalid",
                    "message": ("Program-run identity in path and body must match"),
                },
            )
        return await control_call(
            lambda: client.program_run_reset_full_stop(
                program_run_id,
                body.model_dump(mode="json"),
            )
        )

    @router.get("/api/assistant/modes")
    def assistant_modes() -> dict[str, Any]:
        return _call(client.assistant_modes)

    @router.get("/api/assistant/modes/{mode_id}")
    def assistant_mode(
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        return _call(lambda: client.assistant_mode(mode_id))

    @router.get("/api/manager/apply/status")
    def manager_apply_status(
        apply_id: Annotated[str, Query(min_length=1, max_length=256)],
        apply_generation: Annotated[int, Query(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
    ) -> dict[str, Any]:
        return _call(lambda: client.manager_apply_status(apply_id, apply_generation))

    @router.get("/api/execution/diagnostics")
    def execution_diagnostics() -> dict[str, Any]:
        return _call(client.execution_diagnostics)

    @router.get("/api/execution/identity")
    def execution_identity() -> dict[str, Any]:
        return _call(client.execution_identity)

    @router.get("/api/execution/targets")
    def execution_targets() -> dict[str, Any]:
        return _call(client.execution_targets)

    @router.get("/api/execution/control-state")
    def execution_control_state() -> dict[str, Any]:
        return _call(client.execution_control_state)

    @router.get("/api/control/state")
    def control_projection() -> dict[str, Any]:
        return _call(client.control_projection)

    @router.post("/api/control/commands")
    async def submit_control_command(
        body: ExecutionCommandRequestDTO,
    ) -> JSONResponse:
        kind = body.command_envelope.command_kind
        start = kind in {
            "program_run", "program_motion_start", "program_motion_resume",
            "program_resume_full_stop", "operation_start", "execution_recovery",
        }
        request = body.model_dump(mode="json")

        def submit():
            if start and _resumes_execution(kind, request.get("payload")):
                _require_applied_authoring_drafts(state)
            return client.submit_control_command(request)

        assert state.authoring_admission is not None
        return await control_call(
            lambda: (
                state.authoring_admission.submit_workspace_fenced(submit)
                if start else submit()
            ),
            stop=is_stop_command(kind),
        )

    @router.post("/api/execution/operations")
    async def execution_submit(body: OperationIntentRequest) -> dict[str, Any]:
        def submit():
            # Manual requests address the active runtime, never the Draft.
            # Keep the shared admission serialization and Kernel checks below.
            return client.execute(body.model_dump(mode="json"))

        assert state.authoring_admission is not None
        return await control_call(
            lambda: state.authoring_admission.submit_workspace_fenced(submit)
        )

    @router.post("/api/execution/motion-previews")
    def execution_preview_motion(
        body: OperationIntentRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.preview_motion(body.model_dump(mode="json")))

    @router.post("/api/execution/manual-handover")
    def execution_read_manual_handover(
        body: OperationIntentRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.read_manual_handover(body.model_dump(mode="json")))

    @router.post("/api/execution/capability-command-previews")
    def execution_preview_capability_command(
        body: OperationIntentRequest,
    ) -> dict[str, Any]:
        return _call(
            lambda: client.preview_capability_command(body.model_dump(mode="json"))
        )

    @router.get("/api/execution/operations/{operation_id}")
    def execution_read(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.operation(operation_id))

    @router.post("/api/execution/pick-place/plans/pick")
    def execution_plan_pick(
        body: PickSequencePlanningRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.plan_pick(body.model_dump(mode="json")))

    @router.post("/api/execution/pick-place/plans/place")
    def execution_plan_place(
        body: PlaceSequencePlanningRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.plan_place(body.model_dump(mode="json")))

    def require_matching_operation(
        path_operation_id: str,
        body_operation_id: str,
    ) -> None:
        if path_operation_id != body_operation_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "execution_operation_path_invalid",
                    "message": (
                        "operation identity in the path and request body must match"
                    ),
                },
            )

    @router.post("/api/execution/operations/{operation_id}/reset")
    async def execution_reset(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(
            lambda: client.reset_operation(operation_id, body.model_dump())
        )

    @router.post("/api/execution/operations/{operation_id}/acknowledge-completed")
    async def execution_acknowledge_completed(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(
            lambda: client.acknowledge_completed_operation(
                operation_id, body.model_dump()
            )
        )

    @router.post("/api/execution/stops")
    async def execution_stop(body: StopIntentRequest) -> dict[str, Any]:
        return await control_call(
            lambda: client.stop_operations(
                body.model_dump(mode="json", exclude_unset=True)
            ),
            stop=True,
        )

    @router.post("/api/execution/full-stop")
    async def execution_full_stop(
        body: FullStopIntentRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: client.full_stop(body.model_dump()),
            stop=True,
        )

    @router.get("/api/execution/stop-fault")
    def execution_stop_fault() -> dict[str, Any]:
        return _call(client.stop_fault)

    @router.post("/api/execution/stop-fault/recover")
    async def execution_recover_stop_fault(
        body: StopFaultRecoveryIntentRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: client.recover_stop_fault(
                body.model_dump(mode="json", exclude_unset=True)
            )
        )

    @router.post("/api/execution/runtime-scale-control")
    async def execution_set_runtime_scale_control(
        body: RuntimeScaleControlRequest,
    ) -> dict[str, Any]:
        return await control_call(
            lambda: client.set_runtime_scale_control(body.model_dump(mode="json"))
        )

    @router.get("/api/execution/runtime-scale-control")
    def execution_get_runtime_scale_control(
        request: Request,
        program_binding_id: Annotated[str, Query(min_length=1, max_length=256)],
        component_role_id: Annotated[
            str | None, Query(min_length=1, max_length=256)
        ] = None,
        target_role_id: Annotated[
            str | None, Query(min_length=1, max_length=256)
        ] = None,
        group_targets: Annotated[
            str | None, Query(max_length=MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES)
        ] = None,
    ) -> dict[str, Any]:
        selector_keys = (
            "program_binding_id",
            "component_role_id",
            "target_role_id",
            "group_targets",
        )
        if any(len(request.query_params.getlist(key)) > 1 for key in selector_keys):
            raise HTTPException(
                422, "runtime-scale selector query keys must be singular"
            )
        if group_targets is None:
            payload = {
                "program_binding_id": program_binding_id,
                "component_role_id": component_role_id,
                "target_role_id": target_role_id,
            }
        else:
            if component_role_id is not None or target_role_id is not None:
                raise HTTPException(422, "runtime-scale selector has mixed forms")
            try:
                payload = {
                    "program_binding_id": program_binding_id,
                    "group_targets": decode_runtime_scale_selector_query(
                        group_targets
                    ),
                }
            except ValueError:
                raise HTTPException(422, "group_targets must be strict JSON")
        try:
            selector = RuntimeScaleControlSelectorRequest.model_validate(payload)
        except ValueError as exc:
            raise HTTPException(422, str(exc))
        return _call(
            lambda: client.runtime_scale_control(selector.model_dump(mode="json"))
        )

    @router.get("/api/scene")
    def scene() -> dict[str, Any]:
        return _call(client.scene)

    @router.post("/api/scene/transactions")
    def scene_commit(body: ComposerSceneTransactionRequest):
        owner_request = SceneTransactionRequest.model_validate(
            body.model_dump(
                mode="json",
                exclude={"expected_draft_id", "expected_draft_revision", "expected_authoring_workspace"},
            )
        )

        def commit_owner_scene():
            return _call_scene(
                lambda: client.commit_scene_transaction(
                    owner_request.model_dump(
                        mode="json",
                        exclude={"program_reset_scene_authorization"},
                    )
                ),
                owner_request,
            )

        if body.expected_authoring_workspace is not None:
            if state.authoring_admission is None:
                raise HTTPException(503, detail={"code": "authoring_not_ready", "message": "Authoring admission is unavailable"})

            def submit_closed_scene():
                state.authoring_workspace.require_identity(body.expected_authoring_workspace.identity())
                if body.expected_draft_id is None:
                    return commit_owner_scene()
                return state.draft_session.execute_if_current(
                    draft_id=body.expected_draft_id,
                    expected_revision=body.expected_draft_revision,
                    effect=commit_owner_scene,
                )

            try:
                return state.authoring_admission.submit_workspace_fenced(submit_closed_scene)
            except (AuthoringWorkspaceError, DraftSessionError) as error:
                raise HTTPException(error.status_code, detail={"code": error.code, "message": str(error)}) from error
        if body.expected_draft_id is None:
            return commit_owner_scene()
        try:
            return state.draft_session.execute_if_current(
                draft_id=body.expected_draft_id,
                expected_revision=body.expected_draft_revision,
                effect=commit_owner_scene,
            )
        except DraftSessionError as error:
            raise HTTPException(
                status_code=error.status_code,
                detail={
                    "code": "scene_transaction_draft_stale",
                    "message": (
                        "Scene edit does not identify the current authoritative Draft"
                    ),
                },
            ) from error

    @router.get("/api/execution/alarms")
    def alarms(
        limit: Annotated[int, Query(ge=0, le=_MAXIMUM_ALARM_PAGE)] = 32,
        states: Annotated[list[str] | None, Query(max_length=4)] = None,
        ordering: Literal["default", "attention_newest"] = "default",
    ) -> dict[str, Any]:
        return _call(
            lambda: client.alarms(
                limit=limit,
                states=states or (),
                ordering=ordering,
            )
        )

    @router.get("/api/execution/alarms/metrics")
    def alarm_metrics() -> dict[str, Any]:
        return _call(client.alarm_metrics)

    @router.post("/api/execution/alarms/{condition_id}/acknowledge")
    def acknowledge_alarm(
        condition_id: Annotated[str, Path(pattern=r"^condition-[0-9a-f]{24}$")],
        body: AcknowledgeAlarmRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.acknowledge_alarm(condition_id, body.model_dump()))

    @router.post("/api/execution/alarms/acknowledge-all")
    def acknowledge_all(body: AcknowledgeAllAlarmsRequest) -> dict[str, Any]:
        return _call(lambda: client.acknowledge_all(body.model_dump()))

    @router.get("/api/execution/history")
    def history(
        query: Annotated[HistorianQueryRequest, Query()],
    ) -> dict[str, Any]:
        # The bounded history page can still be large. This synchronous route
        # already runs in a read worker; encode there so history pressure does
        # not stall the event loop used by execution controls.
        return Response(
            content=_call(
                lambda: client.history_payload(params=query.query_parameters())
            ),
            media_type="application/json",
        )

    @router.get("/api/execution/history/export")
    def history_export(query: Annotated[HistorianQueryRequest, Query()]) -> Response:
        return Response(
            content=_call(
                lambda: client.get_json_payload(
                    "/api/execution/history/export", params=query.query_parameters()
                )
            ),
            media_type="application/json",
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-events.json"'
            },
        )

    @router.get("/api/execution/history/captures")
    def history_captures(
        query: Annotated[HistorianCaptureRequest, Query()]
    ) -> dict[str, Any]:
        return _call(
            lambda: client.history_captures(
                params=[(key, value) for key, value in query.model_dump(exclude_none=True).items()]
            )
        )

    @router.get("/api/execution/history/discovery")
    def history_discovery(
        query: Annotated[HistorianDiscoveryRequest, Query()]
    ) -> Response:
        return Response(
            content=_call(
                lambda: client.get_json_payload(
                    "/api/execution/history/discovery", params=query.model_dump()
                )
            ),
            media_type="application/json",
        )

    @router.get("/api/debug/export")
    def debug_export(query: Annotated[HistorianDebugRequest, Query()]) -> Response:
        return Response(
            content=_call(
                lambda: client.get_json_payload(
                    "/api/debug/export", params=query.history_query().query_parameters()
                )
            ),
            media_type="application/json",
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-debug.json"'
            },
        )

    @router.get("/api/debug/export/ros2-medkit")
    def normalized_trace_export(
        query: Annotated[HistorianDebugRequest, Query()]
    ) -> Response:
        return Response(
            content=_call(
                lambda: client.get_json_payload(
                    "/api/debug/export/ros2-medkit",
                    params=query.history_query().query_parameters(),
                )
            ),
            media_type="application/json",
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-trace.json"'
            },
        )

    @router.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return _call(client.historian_health)

    @router.get("/api/execution/program-runs/{program_run_id}/admitted-speeds")
    def program_run_admitted_speeds(
        program_run_id: Annotated[str, Path(min_length=1, max_length=1024)],
    ) -> dict[str, Any]:
        return _call(lambda: client.request(
            "GET",
            f"/api/execution/program-runs/{quote(program_run_id, safe='')}/admitted-speeds",
        ))

    @router.get("/api/skills")
    def skills() -> dict[str, Any]:
        return _call(lambda: client.request("GET", "/api/skills"))

    @router.post("/api/skills/expand")
    def expand_skill(body: SkillExpansionRequest) -> dict[str, Any]:
        return _call(
            lambda: client.request("POST", "/api/skills/expand", body=body.model_dump())
        )

    @router.get("/api/program/diagnostics/health")
    def program_diagnostic_health() -> dict[str, Any]:
        return _call(lambda: client.request("GET", "/api/program/diagnostics/health"))

    @router.get("/api/execution/history/{sequence}")
    def historian_detail(
        sequence: Annotated[int, Path(ge=1, le=9_007_199_254_740_991)],
    ) -> dict[str, Any]:
        return _call(lambda: client.history_detail(sequence))

    @router.post("/api/execution/history/reconcile")
    def historian_reconcile(body: HistorianReconcileRequest) -> dict[str, Any]:
        return _call(lambda: client.reconcile_historian(body.model_dump()))

    return router


__all__ = ["create_headless_router"]
