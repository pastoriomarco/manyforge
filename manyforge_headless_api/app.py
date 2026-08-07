"""Independent FastAPI projection over the C++ H0 owner process."""

from __future__ import annotations

from contextlib import asynccontextmanager
from dataclasses import dataclass
from typing import Annotated, Any, Literal, Protocol
import re
import uuid

from fastapi import FastAPI, Path, Query, Request
from fastapi.responses import JSONResponse
from starlette.types import ASGIApp, Message, Receive, Scope, Send

from .execution import ExecutionCommandClient, ExecutionEventPump
from .manager import ManagerCommandClient
from .models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
    OperationControlRequest,
    OperationIntentRequest,
    RuntimeScaleRequest,
    SceneTransactionRequest,
    StopIntentRequest,
)
from .native import HeadlessApiConfig, HeadlessApiError, NativeHeadlessCore
from .service import HeadlessService, TrustedAttribution


_REQUEST_ID_PATTERN = re.compile(r"[^A-Za-z0-9._-]")
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_MAXIMUM_ALARM_PAGE = 128
_MAXIMUM_HISTORY_PAGE = 32


@dataclass(frozen=True, slots=True)
class TrustedPrincipal:
    principal_id: str
    assurance: str


class TrustedPrincipalAdapter(Protocol):
    """Configured external authentication boundary, never a body-field parser."""

    def resolve(self, request: Request) -> TrustedPrincipal | None: ...


class RequestBodyLimitMiddleware:
    """Bound and replay one request body before downstream JSON decoding."""

    def __init__(self, app: ASGIApp, *, maximum_bytes: int) -> None:
        self.app = app
        self.maximum_bytes = maximum_bytes

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        headers = dict(scope.get("headers", ()))
        raw_length = headers.get(b"content-length")
        if raw_length is not None:
            try:
                declared = int(raw_length)
            except ValueError:
                await self._reject(scope, receive, send)
                return
            if declared < 0 or declared > self.maximum_bytes:
                await self._reject(scope, receive, send)
                return

        body = bytearray()
        disconnected = False
        while True:
            message = await receive()
            if message["type"] == "http.disconnect":
                disconnected = True
                break
            if message["type"] != "http.request":
                continue
            body.extend(message.get("body", b""))
            if len(body) > self.maximum_bytes:
                await self._reject(scope, receive, send)
                return
            if not message.get("more_body", False):
                break

        delivered = False

        async def replay_receive() -> Message:
            nonlocal delivered
            if not delivered:
                delivered = True
                if disconnected:
                    return {"type": "http.disconnect"}
                return {
                    "type": "http.request",
                    "body": bytes(body),
                    "more_body": False,
                }
            return await receive()

        await self.app(scope, replay_receive, send)

    @staticmethod
    async def _reject(scope: Scope, receive: Receive, send: Send) -> None:
        response = JSONResponse(
            status_code=413,
            content={
                "detail": {
                    "code": "headless_request_body_oversized",
                    "message": "request body exceeds the configured byte bound",
                }
            },
        )
        await response(scope, receive, send)


def _error_status(code: str) -> int:
    if code in {
        "headless_projection_response_invalid",
        "headless_projection_response_oversized",
        "manager_command_response_invalid",
        "manager_command_response_identity_mismatch",
        "execution_command_response_invalid",
        "execution_command_response_identity_mismatch",
        "node_environment_projection_invalid",
    }:
        return 502
    if code == "headless_api_internal_failure":
        return 500
    if code.endswith("_not_found") or code in {
        "missing_reference",
        "operator_alarm_not_found",
    }:
        return 404
    if "conflict" in code or "stale" in code or "fence" in code or code in {
        "object_already_exists",
        "scene_projection_transition_in_progress",
    }:
        return 409
    if (
        "unavailable" in code
        or code in {
            "headless_api_closed",
            "headless_request_ledger_full",
            "kernel_owner_unavailable",
            "operator_alarm_state_unavailable",
            "request_ledger_full",
            "scene_projection_commit_uncertain",
        }
    ):
        return 503
    if (
        code == "invalid_request"
        or code.endswith("_invalid")
        or code.endswith("_rejected")
    ):
        return 400
    if code in {
        "operator_alarm_acknowledgement_confirmation_required",
        "operator_alarm_acknowledgement_reason_required",
        "operator_alarm_acknowledge_all_confirmation_required",
        "operator_alarm_acknowledge_all_duplicate_member",
        "operator_alarm_limit_invalid",
        "operator_alarm_state_filter_invalid",
    }:
        return 400
    if code == "operator_alarm_membership_mismatch":
        return 409
    return 500


def _trusted_attribution(
    request: Request,
    adapter: TrustedPrincipalAdapter | None,
) -> TrustedAttribution:
    if adapter is None:
        return TrustedAttribution()
    principal = adapter.resolve(request)
    if principal is None:
        return TrustedAttribution()
    if not principal.principal_id or not principal.assurance:
        raise HeadlessApiError(
            "trusted_authentication_adapter_invalid",
            "trusted authentication adapter returned incomplete attribution",
        )
    return TrustedAttribution(principal.principal_id, principal.assurance)


def create_app(
    config: HeadlessApiConfig | None = None,
    *,
    service: HeadlessService | None = None,
    trusted_principal_adapter: TrustedPrincipalAdapter | None = None,
) -> FastAPI:
    """Create the headless API without importing or starting Composer."""

    if (config is None) == (service is None):
        raise ValueError("provide exactly one of config or service")
    owns_core = service is None
    event_pump: ExecutionEventPump | None = None
    if service is None:
        assert config is not None
        core = NativeHeadlessCore(config)
        manager = ManagerCommandClient(
            config.manager_state_root,
            timeout_seconds=config.manager_timeout_seconds,
        )
        execution = ExecutionCommandClient(
            config.execution_state_root,
            timeout_seconds=config.execution_timeout_seconds,
        )
        service = HeadlessService(core, manager, execution)
        event_execution = ExecutionCommandClient(
            config.execution_state_root,
            timeout_seconds=min(config.execution_timeout_seconds, 1.0),
        )
        event_pump = ExecutionEventPump(
            core,
            event_execution,
            poll_seconds=config.execution_event_poll_milliseconds / 1_000.0,
        )
    assert service is not None

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        if event_pump is not None:
            event_pump.start()
        try:
            yield
        finally:
            if owns_core:
                try:
                    if event_pump is not None:
                        event_pump.shutdown()
                finally:
                    service.core.shutdown()

    app = FastAPI(
        title="ManyForge Headless Runtime API",
        version="0.1.0",
        lifespan=lifespan,
    )
    maximum_body = (
        config.maximum_request_body_bytes if config is not None else 1024 * 1024
    )
    app.add_middleware(RequestBodyLimitMiddleware, maximum_bytes=maximum_body)

    @app.middleware("http")
    async def request_identity(request: Request, call_next):
        raw_id = request.headers.get("x-request-id", "")
        request_id = _REQUEST_ID_PATTERN.sub("", raw_id)[:64]
        request.state.request_id = request_id or uuid.uuid4().hex[:16]
        response = await call_next(request)
        response.headers["X-Request-ID"] = request.state.request_id
        return response

    @app.exception_handler(HeadlessApiError)
    async def headless_error_handler(
        request: Request,
        error: HeadlessApiError,
    ) -> JSONResponse:
        return JSONResponse(
            status_code=_error_status(error.code),
            content={
                "detail": {
                    "code": error.code,
                    "message": str(error),
                    "request_id": getattr(request.state, "request_id", None),
                }
            },
        )

    @app.get("/api/health")
    def health() -> dict[str, Any]:
        return service.health()

    @app.get("/api/runtime")
    def runtime() -> dict[str, Any]:
        return service.runtime()

    @app.get("/api/manager/status")
    def manager_status() -> dict[str, Any]:
        return service.manager_status()

    @app.get("/api/manager/graph")
    def manager_graph() -> dict[str, Any]:
        return service.manager_graph()

    @app.get("/api/manager/diff")
    def manager_diff() -> dict[str, Any]:
        return service.manager_diff()

    @app.get("/api/node-environment")
    def node_environment() -> dict[str, Any]:
        return service.node_environment()

    @app.post("/api/manager/apply")
    def manager_apply(request: ManagerApplyRequest) -> JSONResponse:
        response = service.manager_apply(request)
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    def execution_response(response: dict[str, Any]) -> JSONResponse:
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    @app.get("/api/execution/status")
    def execution_status() -> JSONResponse:
        return execution_response(service.execution_status())

    @app.post("/api/execution/operations")
    def execution_submit(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_submit(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.get("/api/execution/operations/{operation_id}")
    def execution_read(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> JSONResponse:
        return execution_response(service.execution_read(operation_id))

    def require_matching_operation(
        path_operation_id: str,
        body_operation_id: str,
    ) -> None:
        if path_operation_id != body_operation_id:
            raise HeadlessApiError(
                "execution_operation_path_invalid",
                "operation identity in the path and request body must match",
            )

    @app.post("/api/execution/operations/{operation_id}/pause")
    def execution_pause(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            service.execution_pause(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/operations/{operation_id}/resume")
    def execution_resume(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            service.execution_resume(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/operations/{operation_id}/reset")
    def execution_reset(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            service.execution_reset(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post(
        "/api/execution/operations/{operation_id}/acknowledge-completed"
    )
    def execution_acknowledge_completed(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            service.execution_acknowledge_completed(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/stops")
    def execution_stop(
        body: StopIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_stop(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/operations/{operation_id}/runtime-scale")
    def execution_set_runtime_scale(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeScaleRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            service.execution_set_runtime_scale(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.get("/api/execution/operations/{operation_id}/runtime-scale")
    def execution_get_runtime_scale(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> JSONResponse:
        return execution_response(
            service.execution_get_runtime_scale(operation_id)
        )

    @app.get("/api/scene")
    def scene() -> dict[str, Any]:
        return service.scene()

    @app.post("/api/scene/transactions")
    def scene_commit(
        body: SceneTransactionRequest,
        request: Request,
    ) -> JSONResponse:
        response = service.scene_commit(
            body, _trusted_attribution(request, trusted_principal_adapter)
        )
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    @app.get("/api/execution/alarms")
    def alarms(
        limit: Annotated[int, Query(ge=0, le=_MAXIMUM_ALARM_PAGE)] = 32,
        states: Annotated[list[str] | None, Query(max_length=4)] = None,
        ordering: Literal["default", "attention_newest"] = "default",
    ) -> dict[str, Any]:
        return service.alarms(
            limit=limit,
            states=states or (),
            ordering=ordering,
        )

    @app.get("/api/execution/alarms/metrics")
    def alarm_metrics() -> dict[str, Any]:
        return service.core.alarm_metrics()

    @app.post("/api/execution/alarms/{condition_id}/acknowledge")
    def acknowledge_alarm(
        condition_id: Annotated[str, Path(pattern=r"^condition-[0-9a-f]{24}$")],
        body: AcknowledgeAlarmRequest,
        request: Request,
    ) -> dict[str, Any]:
        return service.acknowledge_alarm(
            condition_id,
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )

    @app.post("/api/execution/alarms/acknowledge-all")
    def acknowledge_all(
        body: AcknowledgeAllAlarmsRequest,
        request: Request,
    ) -> dict[str, Any]:
        return service.acknowledge_all(
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )

    @app.get("/api/execution/history")
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
        if start_ns is not None and end_ns is not None and start_ns > end_ns:
            raise HeadlessApiError(
                "historian_query_invalid",
                "start_ns must not exceed end_ns",
            )
        return service.history(
            start_ns=start_ns,
            end_ns=end_ns,
            components=components or (),
            event_types=event_types or (),
            before_sequence=before_sequence,
            limit=limit,
        )

    @app.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return service.core.historian_health()

    @app.post("/api/execution/history/reconcile")
    def historian_reconcile(
        body: HistorianReconcileRequest,
        request: Request,
    ) -> dict[str, Any]:
        return service.historian_reconcile(
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )

    return app


__all__ = ["TrustedPrincipal", "TrustedPrincipalAdapter", "create_app"]
