"""Explicit thin Composer routes over the independent headless HTTP API."""

from __future__ import annotations

from contextlib import asynccontextmanager
from typing import Annotated, Any, Literal

from fastapi import APIRouter, HTTPException, Path, Query
from fastapi.responses import JSONResponse, Response
from pydantic import ConfigDict, Field, model_validator

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
)
from manyforge_headless_api.control_protocol import (
    ExecutionCommandRequestDTO,
    is_stop_command,
)
from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ComponentLifecycleRequest,
    FullStopIntentRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
    ManagerDeploymentCandidatePreviewRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    RouteSelectionRequest,
    SceneTransactionRequest,
    StopIntentRequest,
    StopFaultRecoveryIntentRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionResumeRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramCycleResetRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
    SkillExpansionRequest,
)
from manyforge_headless_api.history_query import (
    HistorianQueryRequest,
    HistorianDiscoveryRequest,
    HistorianDebugRequest,
)

from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
)
from .draft_session import DraftSessionError
from .source_model_client import (
    ActiveSourceModelProjectionError,
    read_active_source_model_context,
)
from .state import ComposerState


_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
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


class ComposerSceneTransactionRequest(SceneTransactionRequest):
    """Scene-owner request with a Draft fence only for authored mutations."""

    model_config = ConfigDict(extra="forbid")

    expected_draft_id: str | None = Field(default=None, min_length=1, max_length=256)
    expected_draft_revision: int | None = Field(
        default=None,
        ge=0,
        le=_MAXIMUM_SAFE_INTEGER,
    )

    @model_validator(mode="after")
    def validate_authority_scope(self) -> "ComposerSceneTransactionRequest":
        authored = self.operation.kind in {
            "add_object",
            "upsert_object",
            "remove_object",
            "update_object_pose",
            "set_collision_policy",
        }
        draft_fenced = (
            self.expected_draft_id is not None and self.expected_draft_revision is not None
        )
        if authored and (self.expected_admission_generation is None or not draft_fenced):
            raise ValueError("authored scene mutation requires admission and Program Draft fences")
        if not authored and (
            self.expected_admission_generation is not None
            or self.expected_draft_id is not None
            or self.expected_draft_revision is not None
        ):
            raise ValueError("runtime-owned scene mutation cannot carry authoring fences")
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

    @router.post("/api/cclc/lifecycle/requests")
    def cclc_lifecycle_submit(
        body: ComponentLifecycleRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.submit_component_lifecycle(body.model_dump(mode="json")))

    @router.get("/api/cclc/lifecycle/transactions/{transaction_id}")
    def cclc_lifecycle_status(
        transaction_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.component_lifecycle_status(transaction_id))

    @router.post("/api/cclc/routes/requests")
    def cclc_route_submit(body: RouteSelectionRequest) -> dict[str, Any]:
        return _call(lambda: client.submit_route_selection(body.model_dump(mode="json")))

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

    @router.get("/api/scenarios")
    def scenarios() -> dict[str, Any]:
        return _call(client.scenarios)

    @router.get("/api/scenarios/current")
    def current_scenario() -> dict[str, Any]:
        try:
            return state.current_scenario()
        except HeadlessClientError as error:
            raise _translate(error) from error

    @router.get("/api/program/applied")
    def program_applied() -> dict[str, Any]:
        return _call(client.program_applied)

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
        draft = state.draft_session.snapshot()
        if draft.get("program_revision") != body.draft_program_revision:
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "program_run_draft_applied_mismatch",
                    "message": "Run does not identify the current authoritative Draft",
                },
            )
        deployment_revision = state.deployment_draft_session.snapshot()["draft"][
            "deploymentRevision"
        ]
        if deployment_revision is not None:
            try:
                current_projection = state.current_scenario()
            except HeadlessClientError as error:
                raise _translate(error) from error
            current = current_projection.get("current_scenario")
            if (
                current_projection.get("ready") is not True
                or not isinstance(current, dict)
                or current.get("deployment_revision") != deployment_revision
            ):
                raise HTTPException(
                    status_code=409,
                    detail={
                        "code": "program_run_deployment_draft_applied_mismatch",
                        "message": (
                            "Run requires the current Deployment Draft to equal "
                            "Manager Applied; Saved may differ"
                        ),
                    },
                )
        return await control_call(lambda: client.program_run_start(body.model_dump(mode="json")))

    @router.get("/api/cycle/status")
    def program_cycle_status() -> dict[str, Any]:
        return _call(client.program_cycle_status)

    @router.post("/api/program/cycle/reset")
    async def program_cycle_reset(
        body: ProgramCycleResetRequest,
    ) -> dict[str, Any]:
        return await control_call(lambda: client.program_cycle_reset(body.model_dump(mode="json")))

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

    @router.post("/api/program/runs/{program_run_id}/runtime-overrides")
    async def stage_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeOverrideStageRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id, "runtime-overrides", body.model_dump(mode="json")
            )
        )

    @router.post("/api/program/runs/{program_run_id}/runtime-overrides/activate")
    async def activate_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeOverrideActionRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id,
                "runtime-overrides/activate",
                body.model_dump(mode="json"),
            )
        )

    @router.post("/api/program/runs/{program_run_id}/runtime-overrides/clear")
    async def clear_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeOverrideActionRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id,
                "runtime-overrides/clear",
                body.model_dump(mode="json"),
            )
        )

    @router.post("/api/program/runs/{program_run_id}/runtime-overrides/promote")
    async def promote_runtime_override(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeOverridePromotionRequest,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        try:
            state.draft_session.require_current(
                draft_id=body.expected_draft_id,
                expected_revision=body.expected_draft_revision,
            )
        except DraftSessionError as error:
            raise HTTPException(
                status_code=error.status_code,
                detail={
                    "code": error.code,
                    "message": str(error),
                    "details": error.detail,
                },
            ) from error
        return await control_call(
            lambda: client.program_runtime_control_action(
                program_run_id,
                "runtime-overrides/promote",
                body.model_dump(mode="json"),
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
        return await control_call(
            lambda: client.cycle_recovery_action(body.model_dump(mode="json"))
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
        return await control_call(
            lambda: client.program_run_motion_resume(
                program_run_id,
                body.model_dump(mode="json"),
            )
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
        return await control_call(
            lambda: client.program_run_resume_full_stop(
                program_run_id,
                body.model_dump(mode="json"),
            )
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

    @router.post("/api/manager/deployment-candidates/preview")
    def manager_deployment_candidate_preview(
        body: ManagerDeploymentCandidatePreviewRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.manager_deployment_candidate_preview(body.model_dump()))

    @router.post("/api/manager/apply")
    def manager_apply(body: ManagerApplyRequest) -> dict[str, Any]:
        return _call(lambda: client.manager_apply(body.model_dump()))

    @router.get("/api/manager/apply/status")
    def manager_apply_status(
        apply_id: Annotated[str, Query(min_length=1, max_length=256)],
        apply_generation: Annotated[int, Query(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
    ) -> dict[str, Any]:
        return _call(lambda: client.manager_apply_status(apply_id, apply_generation))

    @router.get("/api/execution/diagnostics")
    def execution_diagnostics() -> dict[str, Any]:
        return _call(client.execution_diagnostics)

    @router.get("/api/execution/status", deprecated=True)
    def execution_status() -> dict[str, Any]:
        return _call(client.execution_status)

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
        return await control_call(
            lambda: client.submit_control_command(body.model_dump(mode="json")),
            stop=is_stop_command(body.command_envelope.command_kind),
        )

    @router.post("/api/execution/operations")
    async def execution_submit(body: OperationIntentRequest) -> dict[str, Any]:
        return await control_call(lambda: client.execute(body.model_dump(mode="json")))

    @router.post("/api/execution/motion-previews")
    def execution_preview_motion(
        body: OperationIntentRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.preview_motion(body.model_dump(mode="json")))

    @router.post("/api/execution/capability-command-previews")
    def execution_preview_capability_command(
        body: OperationIntentRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.preview_capability_command(body.model_dump(mode="json")))

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

    @router.post("/api/execution/pick-place/sequences/pick")
    async def execution_start_pick(
        body: PickSequencePlanningRequest,
    ) -> dict[str, Any]:
        return await control_call(lambda: client.start_pick(body.model_dump(mode="json")))

    @router.post("/api/execution/pick-place/sequences/place")
    async def execution_start_place(
        body: PlaceSequencePlanningRequest,
    ) -> dict[str, Any]:
        return await control_call(lambda: client.start_place(body.model_dump(mode="json")))

    @router.get("/api/execution/pick-place/sequences/{sequence_id}")
    def execution_pick_place_sequence(
        sequence_id: Annotated[str, Path(min_length=1, max_length=96)],
    ) -> dict[str, Any]:
        return _call(lambda: client.pick_place_sequence(sequence_id))

    def require_matching_operation(
        path_operation_id: str,
        body_operation_id: str,
    ) -> None:
        if path_operation_id != body_operation_id:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "execution_operation_path_invalid",
                    "message": ("operation identity in the path and request body must match"),
                },
            )

    @router.post("/api/execution/operations/{operation_id}/pause")
    async def execution_pause(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(lambda: client.pause_operation(operation_id, body.model_dump()))

    @router.post("/api/execution/operations/{operation_id}/resume")
    async def execution_resume(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(lambda: client.resume_operation(operation_id, body.model_dump()))

    @router.post("/api/execution/operations/{operation_id}/reset")
    async def execution_reset(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(lambda: client.reset_operation(operation_id, body.model_dump()))

    @router.post("/api/execution/operations/{operation_id}/acknowledge-completed")
    async def execution_acknowledge_completed(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return await control_call(
            lambda: client.acknowledge_completed_operation(operation_id, body.model_dump())
        )

    @router.post("/api/execution/stops")
    async def execution_stop(body: StopIntentRequest) -> dict[str, Any]:
        return await control_call(
            lambda: client.stop_operations(body.model_dump(mode="json", exclude_unset=True)),
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
            lambda: client.recover_stop_fault(body.model_dump(mode="json", exclude_unset=True))
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
        program_binding_id: Annotated[str, Query(min_length=1, max_length=256)],
        component_role_id: Annotated[str, Query(min_length=1, max_length=256)],
        target_role_id: Annotated[str, Query(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        selector = RuntimeScaleControlSelectorRequest(
            program_binding_id=program_binding_id,
            component_role_id=component_role_id,
            target_role_id=target_role_id,
        )
        return _call(lambda: client.runtime_scale_control(selector.model_dump(mode="json")))

    @router.get("/api/scene")
    def scene() -> dict[str, Any]:
        return _call(client.scene)

    @router.post("/api/scene/transactions")
    def scene_commit(body: ComposerSceneTransactionRequest):
        owner_request = SceneTransactionRequest.model_validate(
            body.model_dump(
                mode="json",
                exclude={"expected_draft_id", "expected_draft_revision"},
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
                    "message": ("Scene edit does not identify the current authoritative Draft"),
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
            content=_call(lambda: client.history_payload(params=query.query_parameters())),
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
            headers={"Content-Disposition": 'attachment; filename="manyforge-events.json"'},
        )

    @router.get("/api/execution/history/discovery")
    def history_discovery(query: Annotated[HistorianDiscoveryRequest, Query()]) -> Response:
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
            headers={"Content-Disposition": 'attachment; filename="manyforge-debug.json"'},
        )

    @router.get("/api/debug/export/ros2-medkit")
    def normalized_trace_export(query: Annotated[HistorianDebugRequest, Query()]) -> Response:
        return Response(
            content=_call(
                lambda: client.get_json_payload(
                    "/api/debug/export/ros2-medkit",
                    params=query.history_query().query_parameters(),
                )
            ),
            media_type="application/json",
            headers={"Content-Disposition": 'attachment; filename="manyforge-trace.json"'},
        )

    @router.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return _call(client.historian_health)

    @router.get("/api/skills")
    def skills() -> dict[str, Any]:
        return _call(lambda: client.request("GET", "/api/skills"))

    @router.post("/api/skills/expand")
    def expand_skill(body: SkillExpansionRequest) -> dict[str, Any]:
        return _call(lambda: client.request("POST", "/api/skills/expand", body=body.model_dump()))

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
