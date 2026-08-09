"""Explicit thin Composer routes over the independent headless HTTP API."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from fastapi import APIRouter, HTTPException, Path, Query
from fastapi.responses import JSONResponse

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    SceneTransactionRequest,
    StopIntentRequest,
)


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


def _translate(error: HeadlessClientError) -> HTTPException:
    return HTTPException(
        status_code=error.status_code or 503,
        detail={"code": error.code, "message": str(error)},
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
            and response.get("api_version")
            == "manyforge.manager_command_response.v1"
            and response.get("ok") is False
            and response.get("request_id") == context.request_id
            and response.get("request_owner_epoch")
            == context.request_owner_epoch
            and isinstance(response.get("error"), dict)
            and response["error"].get("code") == error.code
        ):
            return JSONResponse(status_code=error.status_code, content=response)
        raise _translate(error) from error


def create_headless_router(client: HeadlessHttpClient) -> APIRouter:
    """Expose only bounded reads and attributed intents; own no reducer."""

    router = APIRouter()

    @router.get("/api/runtime")
    def runtime() -> dict[str, Any]:
        return _call(client.runtime)

    @router.get("/api/manager/status")
    def manager_status() -> dict[str, Any]:
        return _call(client.manager_status)

    @router.get("/api/manager/graph")
    def manager_graph() -> dict[str, Any]:
        return _call(client.manager_graph)

    @router.get("/api/manager/diff")
    def manager_diff() -> dict[str, Any]:
        return _call(client.manager_diff)

    @router.get("/api/node-environment")
    def node_environment() -> dict[str, Any]:
        return _call(client.node_environment)

    @router.get("/api/source-model")
    def source_model() -> dict[str, Any]:
        return _call(client.source_model)

    @router.get("/api/assistant/modes/{mode_id}")
    def assistant_mode(
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        return _call(lambda: client.assistant_mode(mode_id))

    @router.post("/api/manager/apply")
    def manager_apply(body: ManagerApplyRequest) -> dict[str, Any]:
        return _call(lambda: client.manager_apply(body.model_dump()))

    @router.get("/api/execution/status")
    def execution_status() -> dict[str, Any]:
        return _call(client.execution_status)

    @router.post("/api/execution/operations")
    def execution_submit(body: OperationIntentRequest) -> dict[str, Any]:
        return _call(lambda: client.execute(body.model_dump(mode="json")))

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
    def execution_start_pick(
        body: PickSequencePlanningRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.start_pick(body.model_dump(mode="json")))

    @router.post("/api/execution/pick-place/sequences/place")
    def execution_start_place(
        body: PlaceSequencePlanningRequest,
    ) -> dict[str, Any]:
        return _call(lambda: client.start_place(body.model_dump(mode="json")))

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
                    "message": (
                        "operation identity in the path and request body must match"
                    ),
                },
            )

    @router.post("/api/execution/operations/{operation_id}/pause")
    def execution_pause(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return _call(
            lambda: client.pause_operation(operation_id, body.model_dump())
        )

    @router.post("/api/execution/operations/{operation_id}/resume")
    def execution_resume(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return _call(
            lambda: client.resume_operation(operation_id, body.model_dump())
        )

    @router.post("/api/execution/operations/{operation_id}/reset")
    def execution_reset(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return _call(
            lambda: client.reset_operation(operation_id, body.model_dump())
        )

    @router.post(
        "/api/execution/operations/{operation_id}/acknowledge-completed"
    )
    def execution_acknowledge_completed(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return _call(
            lambda: client.acknowledge_completed_operation(
                operation_id, body.model_dump()
            )
        )

    @router.post("/api/execution/stops")
    def execution_stop(body: StopIntentRequest) -> dict[str, Any]:
        return _call(lambda: client.stop_operations(body.model_dump()))

    @router.post("/api/execution/operations/{operation_id}/runtime-scale")
    def execution_set_runtime_scale(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeScaleRequest,
    ) -> dict[str, Any]:
        require_matching_operation(operation_id, body.operation_id)
        return _call(
            lambda: client.set_operation_runtime_scale(
                operation_id, body.model_dump()
            )
        )

    @router.get("/api/execution/operations/{operation_id}/runtime-scale")
    def execution_get_runtime_scale(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _call(lambda: client.operation_runtime_scale(operation_id))

    @router.get("/api/scene")
    def scene() -> dict[str, Any]:
        return _call(client.scene)

    @router.post("/api/scene/transactions")
    def scene_commit(body: SceneTransactionRequest):
        return _call_scene(
            lambda: client.commit_scene_transaction(body.model_dump(mode="json")),
            body,
        )

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
        return _call(
            lambda: client.acknowledge_alarm(condition_id, body.model_dump())
        )

    @router.post("/api/execution/alarms/acknowledge-all")
    def acknowledge_all(body: AcknowledgeAllAlarmsRequest) -> dict[str, Any]:
        return _call(lambda: client.acknowledge_all(body.model_dump()))

    @router.get("/api/execution/history")
    def history(
        start_ns: Annotated[
            int | None,
            Query(ge=0, le=_MAXIMUM_SAFE_INTEGER),
        ] = None,
        end_ns: Annotated[
            int | None,
            Query(ge=0, le=_MAXIMUM_SAFE_INTEGER),
        ] = None,
        components: Annotated[
            list[str] | None,
            Query(max_length=256),
        ] = None,
        event_types: Annotated[
            list[str] | None,
            Query(max_length=256),
        ] = None,
        before_sequence: Annotated[
            int | None,
            Query(ge=1, le=_MAXIMUM_SAFE_INTEGER),
        ] = None,
        limit: Annotated[int, Query(ge=1, le=_MAXIMUM_HISTORY_PAGE)] = 32,
    ) -> dict[str, Any]:
        params: list[tuple[str, str | int]] = [("limit", limit)]
        for name, value in (
            ("start_ns", start_ns),
            ("end_ns", end_ns),
            ("before_sequence", before_sequence),
        ):
            if value is not None:
                params.append((name, value))
        params.extend(("components", value) for value in components or ())
        params.extend(("event_types", value) for value in event_types or ())
        return _call(lambda: client.history(params=params))

    @router.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return _call(client.historian_health)

    @router.post("/api/execution/history/reconcile")
    def historian_reconcile(body: HistorianReconcileRequest) -> dict[str, Any]:
        return _call(lambda: client.reconcile_historian(body.model_dump()))

    return router


__all__ = ["create_headless_router"]
