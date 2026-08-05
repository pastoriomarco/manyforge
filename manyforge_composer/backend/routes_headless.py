"""Explicit thin Composer routes over the independent headless HTTP API."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from fastapi import APIRouter, HTTPException, Path, Query

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
)


_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_MAXIMUM_ALARM_PAGE = 128
_MAXIMUM_HISTORY_PAGE = 32


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

    @router.post("/api/manager/apply")
    def manager_apply(body: ManagerApplyRequest) -> dict[str, Any]:
        return _call(lambda: client.manager_apply(body.model_dump()))

    @router.get("/api/scene")
    def scene() -> dict[str, Any]:
        return _call(client.scene)

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
