"""Independent FastAPI projection over the C++ H0 owner process."""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from dataclasses import dataclass
from typing import Annotated, Any, Literal, Protocol
import json
import re
import threading
import uuid

from fastapi import (
    BackgroundTasks,
    FastAPI,
    HTTPException,
    Path,
    Query,
    Request,
    WebSocket,
    WebSocketDisconnect,
)
from fastapi.exception_handlers import request_validation_exception_handler
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, Response
from starlette.types import ASGIApp, Message, Receive, Scope, Send

from .cclc import CCLCOpenPort, CCLCOpenRequestClient
from .execution import ExecutionCommandClient, ExecutionEventPump
from .control_dispatch import (
    HeadlessExecutionControlDispatch,
    requires_execution_owner_fence,
)
from .control_protocol import (
    ControlProtocolError,
    ExecutionCommandRequestDTO,
    is_stop_command,
)
from .control_session import serve_execution_control
from .control_state import (
    ControlStateUnavailableError,
    HeadlessControlStateSource,
    build_control_projection,
)
from .execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
    ExecutionControlIngress,
)
from .manager import ManagerCommandClient
from .history_query import (
    HistorianCaptureRequest,
    HistorianQueryRequest,
    HistorianDiscoveryRequest,
    HistorianDebugRequest,
)
from .models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ComponentLifecycleRequest,
    ActivationPreparationRequest,
    FullStopIntentRequest,
    HistorianReconcileRequest,
    ManagerApplyRequest,
    ManagerDeploymentCandidatePreviewRequest,
    AuthoringMutationContext,
    AuthoringMutationRecoveryRequest,
    AuthoringMutationSettlementRequest,
    AuthoringOwnerReconciliationRequest,
    ProgramModelAttemptRequest,
    ProgramDiagnosticRequest,
    SkillExpansionRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    ProgramNodeCorrectionPreviewRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramScenePreparationRequest,
    ProgramCycleResetRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
    RuntimeBlackboardWriteRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES,
    decode_runtime_scale_selector_query,
    RouteSelectionRequest,
    RouteSelectionRequestResultQuery,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
)
from .native import (
    HeadlessApiConfig,
    HeadlessApiError,
    MAX_ACTIVATION_PREPARATION_REQUEST_BYTES,
    MAX_PROGRAM_MODEL_ATTEMPT_BYTES,
    NativeHeadlessCore,
    ViewerModelAsset,
    strict_json_object,
)
from .service import HeadlessService, TrustedAttribution
from .program_runner_client import ProgramRunnerHttpClient
from .cell_admission import CellAdmissionClient
from .alarm_attention import (
    PUBLICATION_DEADLINE_SECONDS,
    OperatorAlarmAttentionPublisher,
)
from .activation_preparer import ActivationPreparer


_REQUEST_ID_PATTERN = re.compile(r"[^A-Za-z0-9._-]")
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
ScenarioPath = Annotated[
    str, Path(min_length=1, max_length=128, pattern=r"^[a-z][a-z0-9_.-]*$")
]
_MAXIMUM_SIGNED_64 = 9_223_372_036_854_775_807
_MAXIMUM_ALARM_PAGE = 128
_MAXIMUM_HISTORY_PAGE = 32
_OBSERVATION_CREDIT_SCHEMA = "manyforge.execution_observation_credit.v1"
_MAXIMUM_OBSERVATION_CREDIT_BYTES = 4_096


def _execution_observation_cursor(message: str) -> dict[str, Any] | None:
    if len(message.encode("utf-8")) > _MAXIMUM_OBSERVATION_CREDIT_BYTES:
        raise ValueError("execution observation credit is oversized")
    value = strict_json_object(
        message,
        label="execution observation credit",
        maximum_bytes=_MAXIMUM_OBSERVATION_CREDIT_BYTES,
    )
    if set(value) != {"last_applied", "schema_version"} or (
        value.get("schema_version") != _OBSERVATION_CREDIT_SCHEMA
    ):
        raise ValueError("execution observation credit has an invalid shape")
    cursor = value.get("last_applied")
    if cursor is None:
        return None
    if (
        not isinstance(cursor, dict)
        or set(cursor) != {"owner_epoch", "source_epoch", "source_sequence"}
        or not isinstance(cursor.get("owner_epoch"), str)
        or not cursor["owner_epoch"]
        or len(cursor["owner_epoch"].encode("utf-8")) > 2_048
        or not isinstance(cursor.get("source_epoch"), str)
        or not cursor["source_epoch"]
        or len(cursor["source_epoch"].encode("utf-8")) > 2_048
        or type(cursor.get("source_sequence")) is not int
        or not 0 <= cursor["source_sequence"] <= _MAXIMUM_SAFE_INTEGER
    ):
        raise ValueError("execution observation credit cursor is invalid")
    return dict(cursor)


@dataclass(frozen=True, slots=True)
class TrustedPrincipal:
    principal_id: str
    assurance: str


class TrustedPrincipalAdapter(Protocol):
    """Configured external authentication boundary, never a body-field parser."""

    def resolve(self, request: Request) -> TrustedPrincipal | None: ...


class RequestBodyLimitMiddleware:
    """Bound and replay one request body before downstream JSON decoding."""

    def __init__(
        self,
        app: ASGIApp,
        *,
        maximum_bytes: int,
        path_maximum_bytes: dict[str, int] | None = None,
        error_code: str = "headless_request_body_oversized",
        error_message: str = "request body exceeds the configured byte bound",
        recognized_stop_paths: frozenset[str] = frozenset(),
    ) -> None:
        self.app = app
        self.maximum_bytes = maximum_bytes
        self.path_maximum_bytes = path_maximum_bytes or {}
        self.error_code = error_code
        self.error_message = error_message
        self.recognized_stop_paths = recognized_stop_paths

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        maximum_bytes = self.path_maximum_bytes.get(
            str(scope.get("path", "")), self.maximum_bytes
        )
        recognized_stop = (
            scope.get("method") == "POST"
            and scope.get("path") in self.recognized_stop_paths
        )

        def mark_malformed(reason: str, *, close_connection: bool) -> None:
            state = scope.setdefault("state", {})
            state["recognized_stop_malformed_reason"] = reason
            if close_connection:
                state["recognized_stop_close_connection"] = True

        headers = dict(scope.get("headers", ()))
        raw_length = headers.get(b"content-length")
        declared_oversized = False
        if raw_length is not None:
            try:
                declared = int(raw_length)
            except ValueError:
                await self._reject(scope, receive, send)
                return
            if declared < 0:
                await self._reject(scope, receive, send)
                return
            if declared > maximum_bytes:
                if not recognized_stop:
                    await self._reject(scope, receive, send)
                    return
                mark_malformed("recognized_stop_body_oversized", close_connection=True)
                declared_oversized = True

        body = bytearray()
        disconnected = False
        if declared_oversized:
            body.extend(b"{}")
        else:
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    disconnected = True
                    break
                if message["type"] != "http.request":
                    continue
                body.extend(message.get("body", b""))
                if len(body) > maximum_bytes:
                    if not recognized_stop:
                        await self._reject(scope, receive, send)
                        return
                    mark_malformed(
                        "recognized_stop_body_oversized", close_connection=True
                    )
                    body.clear()
                    body.extend(b"{}")
                    break
                if not message.get("more_body", False):
                    break

        if recognized_stop and not declared_oversized and not disconnected:
            try:
                strict_json_object(
                    bytes(body).decode("utf-8"), label="recognized Stop request"
                )
            except (HeadlessApiError, UnicodeError):
                mark_malformed(
                    "recognized_stop_payload_invalid", close_connection=False
                )
                body.clear()
                body.extend(b"{}")

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

    async def _reject(self, scope: Scope, receive: Receive, send: Send) -> None:
        response = JSONResponse(
            status_code=413,
            content={
                "detail": {
                    "code": self.error_code,
                    "message": self.error_message,
                }
            },
        )
        await response(scope, receive, send)


def _error_status(code: str) -> int:
    if code == "full_stack_restart_required":
        return 409
    if code in {
        "headless_projection_response_invalid",
        "headless_projection_response_oversized",
        "manager_command_response_invalid",
        "manager_command_response_identity_mismatch",
        "execution_command_response_invalid",
        "execution_command_response_identity_mismatch",
        "cclc_open_response_invalid",
        "cclc_open_response_identity_mismatch",
        "node_environment_projection_invalid",
        "assistant_mode_context_invalid",
        "source_model_context_invalid",
        "source_model_projection_invalid",
        "program_applied_selection_invalid",
        "program_runner_response_invalid",
        "viewer_model_asset_invalid",
        "assistant_mode_projection_invalid",
    }:
        return 502
    if code == "headless_api_internal_failure":
        return 500
    if code.endswith("_not_found") or code in {
        "missing_reference",
        "operator_alarm_not_found",
    }:
        return 404
    if (
        "conflict" in code
        or "stale" in code
        or "fence" in code
        or code.startswith(("program_cycle_state_", "program_run_state_"))
        or code
        in {
            "invalid_state_transition",
            "command_not_allowed",
            "manual_handover_required",
            "object_already_exists",
            "scene_projection_transition_in_progress",
            "program_run_admission_failed",
            "program_run_already_active",
            "program_required_component_not_active",
            "route_selection_component_not_ready",
            "program_applied_non_executable",
            "program_not_selected",
            # Preparation runs before any claim or effect; it refuses the input.
            "activation_preparation_failed",
            "prepared_activation_unverified",
            "authoring_not_ready",
            "program_run_draft_applied_mismatch",
            "deployment_candidate_compiler_busy",
            "program_node_preview_busy",
            "deployment_candidate_cell_mismatch",
            "cclc_request_namespace_refreshed",
            "cclc_request_admission_deferred",
            "persistent_key_request_replayed",
            "unacknowledged_operator_alarms",
        }
    ):
        return 409
    if "unavailable" in code or code in {
        "headless_api_closed",
        "kernel_owner_unavailable",
        "execution_owner_not_ready",
        "operator_alarm_state_unavailable",
        "program_applied_selection_not_active",
        "program_activation_history_incomplete",
        "program_runner_unavailable",
        "program_run_scheduling_failed",
        "request_ledger_full",
        "scene_projection_commit_uncertain",
        "persistent_key_storage_uncertain",
        "execution_control_ingress_busy",
    }:
        return 503
    if (
        code == "invalid_request"
        or code.endswith("_invalid")
        or code.endswith("_rejected")
    ):
        return 400
    if code in {
        "cclc_confirmation_required",
        "confirmation_required",
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
    cclc: CCLCOpenPort | None = None,
    trusted_principal_adapter: TrustedPrincipalAdapter | None = None,
) -> FastAPI:
    """Create the headless API without importing or starting Composer."""

    if (config is None) == (service is None):
        raise ValueError("provide exactly one of config or service")
    owns_core = service is None
    event_pump: ExecutionEventPump | None = None
    alarm_attention: OperatorAlarmAttentionPublisher | None = None
    program_runner_client: ProgramRunnerHttpClient | None = None
    if service is None:
        assert config is not None
        core = NativeHeadlessCore(config)
        manager = ManagerCommandClient(
            config.manager_run_root,
            timeout_seconds=config.manager_timeout_seconds,
        )
        execution = ExecutionCommandClient(
            config.execution_run_root,
            timeout_seconds=config.execution_timeout_seconds,
        )
        if cclc is None:
            cclc = CCLCOpenRequestClient(
                config.execution_run_root,
                timeout_seconds=config.cclc_timeout_seconds,
            )
        if config.program_runner_url is not None:
            program_runner_client = ProgramRunnerHttpClient(
                config.program_runner_url,
                timeout_seconds=config.program_runner_timeout_seconds,
            )
        activation_preparer = None
        if config.activation_preparer_path is not None:
            assert config.activation_preparer_capability_root is not None
            assert config.activation_preparer_activation_root is not None
            activation_preparer = ActivationPreparer(
                executable=config.activation_preparer_path,
                capability_root=config.activation_preparer_capability_root,
                activation_root=config.activation_preparer_activation_root,
                runtime_cell_id=config.cell_id,
                timeout_seconds=config.activation_preparer_timeout_seconds,
            )
        service = HeadlessService(
            core,
            manager,
            execution,
            program_runner=program_runner_client,
            cell_admission=CellAdmissionClient(
                config.execution_run_root,
                timeout_seconds=config.execution_timeout_seconds,
            ),
            cclc=cclc,
            activation_preparer=activation_preparer,
        )
        event_execution = ExecutionCommandClient(
            config.execution_run_root,
            timeout_seconds=min(config.execution_timeout_seconds, 1.0),
        )
        event_pump = ExecutionEventPump(
            core,
            event_execution,
            poll_seconds=config.execution_event_poll_milliseconds / 1_000.0,
            # Its own short-bounded client: an unreachable CCLC owner must not
            # hold up operation-event ingestion beyond one short exchange.
            cclc=CCLCOpenRequestClient(
                config.execution_run_root,
                timeout_seconds=min(config.cclc_timeout_seconds, 1.0),
            ),
        )
        # Its own thread, reading the owner's published projection without the
        # core lock: neither event-pump work nor any core call can delay the
        # Kernel heartbeat; each publication ends by its deadline D < P.
        alarm_attention = OperatorAlarmAttentionPublisher(
            core,
            CellAdmissionClient(
                config.execution_run_root,
                timeout_seconds=min(
                    config.execution_timeout_seconds, PUBLICATION_DEADLINE_SECONDS
                ),
            ),
        )
    assert service is not None
    execution_controls = ExecutionControlClient(
        thread_name_prefix="manyforge_headless_control",
    )
    control_dispatch = HeadlessExecutionControlDispatch(service)
    control_ingress = ExecutionControlIngress(
        projection=control_dispatch.owner_projection,
        dispatch=control_dispatch,
        lanes=execution_controls,
    )
    runner_control_url = getattr(
        getattr(service, "program_runner", None),
        "control_state_websocket_url",
        None,
    )
    if not isinstance(runner_control_url, str):
        runner_control_url = None
    cold_freshness_paths = (
        ()
        if config is None
        else (
            config.projection_root / "manager-source.json",
            config.projection_root / "logical-scene-source.json",
            config.projection_root / "scene-runtime-source.json",
        )
    )
    cold_watch_paths = cold_freshness_paths
    control_state_source = HeadlessControlStateSource(
        service,
        control_ingress,
        program_runner_websocket_url=runner_control_url,
        cold_watch_paths=cold_watch_paths,
        cold_freshness_paths=cold_freshness_paths,
        cold_freshness_budget_ns=(
            None if config is None else config.freshness_milliseconds * 1_000_000
        ),
        incident_recorder=getattr(service, "record_client_incident", None),
    )
    if alarm_attention is not None:
        # Alarm projections carry the Kernel-held attention copy; a change of
        # it republishes them (Composer enables Run only once the Kernel
        # holds a reconciled projection of the active configuration).
        service.kernel_attention_source = alarm_attention.kernel_held_attention
        alarm_attention.set_kernel_held_observer(control_state_source.notify_cold)
    if event_pump is not None:
        event_pump.set_change_observer(control_state_source.notify_cold)
        event_pump.set_settled_observer(control_state_source.notify_motion_settled)
        event_pump.set_progress_observer(control_state_source.notify_operation_progress)
    service_cclc = getattr(service, "cclc", None)
    if cclc is None:
        cclc = service_cclc
    elif isinstance(service, HeadlessService) and service_cclc is None:
        # Keep the service-owned lifecycle admission path and the thin route/
        # projection paths on the same explicitly injected CCLC adapter.
        service.cclc = cclc

    def cclc_port() -> CCLCOpenPort:
        if cclc is None:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                "CCLC owner is not configured",
            )
        return cclc

    async def control_call(callback, *, stop: bool = False):
        try:
            lane = execution_controls.stop if stop else execution_controls.ordinary
            return await lane(callback)
        except ExecutionControlBusyError as error:
            raise HeadlessApiError(
                "execution_control_ingress_busy",
                str(error),
                details={
                    "lane": error.lane.value,
                    "maximum_admitted": error.maximum_admitted,
                },
            ) from error

    async def json_control_call(callback, *, stop: bool = False) -> JSONResponse:
        """Run and encode one JSON-native result inside its bounded lane."""

        return await control_call(
            lambda: JSONResponse(content=callback()),
            stop=stop,
        )

    async def rest_execution_command(
        *,
        command_kind: str,
        payload: dict[str, Any],
        request_context: Any,
        transport_request: Request,
    ) -> dict[str, Any]:
        """Adapt a named REST route to the one semantic command ingress."""

        if is_stop_command(command_kind):
            # Stop must reach its reserved lane without first waiting on an
            # owner projection. The semantic owner resolves stale exact scope
            # conservatively; these envelope fields grant no authority.
            owner_epoch = request_context.request_owner_epoch
            control_revision = 1
        else:
            owner_epoch, control_revision = control_dispatch.owner_projection()
        freshness = control_ingress.freshness_for(owner_epoch, control_revision)
        correlation_id = request_context.correlation_id or request_context.request_id
        command_payload = dict(payload)
        if requires_execution_owner_fence(command_kind):
            command_payload["expected_execution_owner_epoch"] = (
                request_context.request_owner_epoch
            )
        command = ExecutionCommandRequestDTO.model_validate(
            {
                "command_envelope": {
                    "schema_version": "manyforge.execution_command.v1",
                    "command_kind": command_kind,
                    "correlation_id": correlation_id,
                    "source_label": request_context.source_label,
                    "control_session_id": None,
                    "client_sequence": None,
                    "freshness": freshness.model_dump(mode="json"),
                    "expected_semantic_owner_epoch": owner_epoch,
                    "expected_execution_control_revision": control_revision,
                },
                "payload": command_payload,
            }
        )
        attribution = _trusted_attribution(transport_request, trusted_principal_adapter)
        submission = await control_ingress.submit(
            command,
            transport_kind="rest",
            trusted_principal_id=attribution.principal_id,
            assurance=attribution.assurance,
        )
        control_state_source.notify_control()
        if submission.receipt.disposition == "rejected" and not (
            isinstance(submission.result, dict) and submission.result.get("ok") is False
        ):
            raise HeadlessApiError(
                submission.receipt.code,
                "execution command was rejected by its current owner",
            )
        return {} if submission.result is None else dict(submission.result)

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        if event_pump is not None:
            event_pump.start()
        if alarm_attention is not None:
            alarm_attention.start()
        try:
            yield
        finally:
            try:
                # Stop following Applies before their owners' clients close.
                service.close()
                await control_state_source.close()
                if owns_core:
                    try:
                        try:
                            if alarm_attention is not None:
                                alarm_attention.shutdown()
                        finally:
                            if event_pump is not None:
                                event_pump.shutdown()
                    finally:
                        try:
                            if program_runner_client is not None:
                                program_runner_client.close()
                        finally:
                            service.core.shutdown()
            finally:
                control_ingress.close()
                execution_controls.close()

    app = FastAPI(
        title="ManyForge Headless Runtime API",
        version="0.1.0",
        lifespan=lifespan,
    )
    maximum_body = (
        config.maximum_request_body_bytes if config is not None else 1024 * 1024
    )
    app.add_middleware(
        RequestBodyLimitMiddleware,
        maximum_bytes=maximum_body,
        path_maximum_bytes={
            "/api/activation-preparations": (
                MAX_ACTIVATION_PREPARATION_REQUEST_BYTES
            ),
            "/api/program/model-attempts": MAX_PROGRAM_MODEL_ATTEMPT_BYTES,
            "/api/program/diagnostics": 64 * 1024,
        },
        recognized_stop_paths=frozenset(
            {"/api/execution/stops", "/api/execution/full-stop"}
        ),
    )

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
                    "details": error.details,
                    "message": str(error),
                    "request_id": getattr(request.state, "request_id", None),
                }
            },
        )

    @app.exception_handler(ControlProtocolError)
    async def control_protocol_error_handler(
        request: Request,
        error: ControlProtocolError,
    ) -> JSONResponse:
        return JSONResponse(
            status_code=409,
            content={
                "detail": {
                    "code": error.code,
                    "details": {},
                    "message": str(error),
                    "request_id": getattr(request.state, "request_id", None),
                }
            },
        )

    @app.exception_handler(RequestValidationError)
    async def request_validation_handler(
        request: Request,
        error: RequestValidationError,
    ) -> Response:
        if request.method == "POST" and request.url.path in {
            "/api/execution/stops",
            "/api/execution/full-stop",
        }:
            reason = getattr(
                request.state,
                "recognized_stop_malformed_reason",
                "recognized_stop_payload_invalid",
            )
            adapter_request_id = "malformed-stop." + getattr(
                request.state, "request_id", uuid.uuid4().hex[:16]
            )
            attribution = _trusted_attribution(
                request,
                trusted_principal_adapter,
            )
            try:
                response = execution_response(
                    await control_call(
                        lambda: service.execution_recognized_malformed_stop(
                            request_id=adapter_request_id,
                            recognized_route=request.url.path,
                            reason=reason,
                            attribution=attribution,
                        ),
                        stop=True,
                    )
                )
            except HeadlessApiError as execution_error:
                response = await headless_error_handler(
                    request,
                    execution_error,
                )
            if getattr(request.state, "recognized_stop_close_connection", False):
                response.headers["Connection"] = "close"
            return response
        return await request_validation_exception_handler(request, error)

    @app.get("/api/health")
    def health() -> dict[str, Any]:
        return service.health()

    @app.get("/api/runtime")
    def runtime() -> dict[str, Any]:
        return service.runtime()

    @app.get("/api/component-overview")
    async def component_overview() -> dict[str, Any]:
        try:
            current = control_state_source.current_component_overview()
        except ControlStateUnavailableError as error:
            raise HeadlessApiError(
                "control_projection_unavailable",
                str(error),
            ) from error
        if current is not None:
            return current
        # With no control client there is deliberately no retained source work.
        # A standalone REST reader owns exactly one on-demand fact capture and
        # reduction in a read worker instead of starting a polling/subscription
        # lifetime.
        return await asyncio.to_thread(service.component_overview)

    @app.get("/api/cell/authoring-admission")
    def authoring_admission() -> dict[str, Any]:
        return service.authoring_admission()

    @app.post("/api/program/authoring-transactions")
    def acquire_authoring_transaction(
        body: AuthoringMutationContext,
    ) -> dict[str, Any]:
        claim = service.acquire_authoring_mutation(body)
        control_state_source.notify_cold()
        return {"claim": claim}

    @app.post("/api/program/authoring-transactions/settle")
    def settle_authoring_transaction(
        body: AuthoringMutationSettlementRequest,
    ) -> dict[str, Any]:
        admission = service.settle_authoring_mutation(body)
        control_state_source.notify_cold()
        return {"admission": admission}

    @app.post("/api/program/authoring-transactions/recovery-receipt")
    def recover_authoring_transaction(
        body: AuthoringMutationRecoveryRequest,
    ) -> dict[str, Any]:
        receipt = service.recover_authoring_mutation(body)
        control_state_source.notify_cold()
        return {"recovery_receipt": receipt}

    @app.post("/api/program/authoring-transactions/reconcile-owner")
    def reconcile_authoring_owner(
        body: AuthoringOwnerReconciliationRequest,
    ) -> dict[str, Any]:
        admission = service.reconcile_authoring_owner(body)
        control_state_source.notify_cold()
        return {"admission": admission}

    @app.get("/api/manager/status")
    def manager_status() -> dict[str, Any]:
        return service.manager_status()

    @app.get("/api/manager/graph")
    def manager_graph() -> dict[str, Any]:
        return service.manager_graph()

    @app.get("/api/manager/diff")
    def manager_diff() -> dict[str, Any]:
        return service.manager_diff()

    @app.get("/api/cclc/components/{component_id}")
    def cclc_component_snapshot(
        component_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return cclc_port().component_snapshot(component_id)

    @app.get("/api/cclc/owner")
    def cclc_owner_identity() -> dict[str, Any]:
        return cclc_port().owner_identity()

    @app.get("/api/cclc/components/{component_id}/route-templates")
    def cclc_route_templates(
        component_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return cclc_port().route_templates(component_id)

    @app.post("/api/cclc/lifecycle/requests")
    def cclc_lifecycle_submit(
        body: ComponentLifecycleRequest,
    ) -> dict[str, Any]:
        if not body.confirmed:
            raise HeadlessApiError(
                "cclc_confirmation_required",
                "component lifecycle request requires explicit confirmation",
            )
        return service.component_lifecycle_submit(body)

    @app.post("/api/cclc/lifecycle/requests/result")
    def cclc_lifecycle_request_result(
        body: ComponentLifecycleRequest,
    ) -> dict[str, Any]:
        return {"result": service.component_lifecycle_request_result(body)}

    @app.get("/api/cclc/lifecycle/transactions/{transaction_id}")
    def cclc_lifecycle_status(
        transaction_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return {"status": service.component_lifecycle_status(transaction_id)}

    @app.post("/api/cclc/routes/requests")
    def cclc_route_submit(body: RouteSelectionRequest) -> dict[str, Any]:
        if not body.confirmed:
            raise HeadlessApiError(
                "cclc_confirmation_required",
                "route-selection request requires explicit confirmation",
            )
        return service.route_selection_submit(body)

    @app.post("/api/cclc/routes/requests/result")
    def cclc_route_request_result(
        body: RouteSelectionRequestResultQuery,
    ) -> dict[str, Any]:
        return {"result": service.route_selection_request_result(body)}

    @app.get("/api/cclc/routes/transactions/{transaction_id}")
    def cclc_route_status(
        transaction_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return {"status": cclc_port().route_status(transaction_id)}

    @app.get("/api/node-environment")
    def node_environment() -> dict[str, Any]:
        return service.node_environment()

    @app.get("/api/skills")
    def skills() -> dict[str, Any]:
        return service.skills()

    @app.post("/api/skills/expand")
    def expand_skill(body: SkillExpansionRequest) -> dict[str, Any]:
        return service.expand_skill(body)

    @app.get("/api/source-model")
    def source_model() -> dict[str, Any]:
        return service.source_model()

    @app.get("/api/source-model-context")
    def source_model_context() -> dict[str, Any]:
        return service.source_model_context()

    @app.get("/api/program/applied")
    def program_applied() -> dict[str, Any]:
        return service.program_applied()

    @app.get("/api/program/applied/status")
    def program_applied_status() -> dict[str, Any]:
        return service.program_applied_status()

    @app.get("/api/program/committed-activation")
    def program_committed_activation() -> dict[str, Any]:
        return service.committed_scenario_activation()

    @app.post("/api/program/model-attempts")
    def program_model_attempt(
        request: ProgramModelAttemptRequest,
    ) -> dict[str, Any]:
        return service.program_model_attempt(request)

    @app.post("/api/program/diagnostics")
    def program_diagnostic(request: ProgramDiagnosticRequest) -> dict[str, Any]:
        return service.core.record_program_diagnostic(request.model_dump(mode="json"))

    @app.get("/api/program/diagnostics/health")
    def program_diagnostic_health() -> dict[str, Any]:
        return service.program_diagnostic_health()

    @app.get("/api/program/runs")
    def program_runs(
        active_only: bool = False,
        limit: Annotated[int, Query(ge=1, le=4_096)] = 128,
    ) -> dict[str, Any]:
        return service.program_runs(active_only=active_only, limit=limit)

    @app.post("/api/program/runs")
    async def program_run_start(
        body: ProgramRunStartRequest,
        request: Request,
    ) -> dict[str, Any]:
        return await rest_execution_command(
            command_kind="program_run",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/scene-preparations")
    async def program_scene_prepare(
        request: ProgramScenePreparationRequest,
    ) -> dict[str, Any]:
        return await json_control_call(lambda: service.program_scene_prepare(request))

    @app.get("/api/program/runs/{program_run_id}")
    def program_run_read(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return service.program_run_read(program_run_id)

    @app.post("/api/program/runs/{program_run_id}/stop")
    async def program_run_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramRunStopRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in the path and request body must match",
            )
        return await rest_execution_command(
            command_kind="program_stop",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/motion-stop")
    async def program_run_motion_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionStopRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_motion_stop",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/resume-motion")
    async def program_run_motion_resume(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionResumeRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_motion_resume",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/motion-start")
    async def program_run_motion_start(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramSelectionStartRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_motion_start",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/manual-handover")
    async def program_run_manual_handover(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramManualHandoverRequest,
        request: Request,
    ) -> dict[str, Any]:
        """Retire exact held tasks through the Program owner, without releasing holds."""
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_manual_handover",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/full-stop")
    async def program_run_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramRunStopRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_full_stop",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/resume-full-stop")
    async def program_run_resume_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramFullStopResumeRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_resume_full_stop",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/reset")
    async def program_run_reset_full_stop(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramFullStopResetRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_discard_full_stop",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.get("/api/cycle/status")
    def program_cycle_status() -> dict[str, Any]:
        return service.program_cycle_status()

    @app.get("/api/control/state")
    def execution_control_projection() -> dict[str, Any]:
        cycle = service.program_cycle_status()
        return build_control_projection(cycle, control_ingress).model_dump(mode="json")

    @app.post("/api/control/commands")
    async def execution_control_command(
        body: ExecutionCommandRequestDTO,
        request: Request,
    ) -> JSONResponse:
        attribution = _trusted_attribution(request, trusted_principal_adapter)
        submission = await control_ingress.submit(
            body,
            transport_kind="rest",
            trusted_principal_id=attribution.principal_id,
            assurance=attribution.assurance,
        )
        control_state_source.notify_control()
        return JSONResponse(
            content={
                "receipt": submission.receipt.model_dump(mode="json"),
                "result": submission.result,
            }
        )

    @app.websocket("/ws/control")
    async def execution_control_websocket(websocket: WebSocket) -> None:
        await serve_execution_control(
            websocket,
            control_state_source,
            control_ingress,
        )

    @app.get("/api/control/diagnostics")
    async def execution_control_diagnostics() -> dict[str, Any]:
        return control_state_source.diagnostics()

    @app.post("/api/program/cycle/reset")
    async def program_cycle_reset(
        body: ProgramCycleResetRequest,
        request: Request,
    ) -> dict[str, Any]:
        return await rest_execution_command(
            command_kind="program_reset",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.get("/api/program/runs/{program_run_id}/runtime-controls")
    def program_runtime_controls(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return service.program_runtime_controls(program_run_id)

    @app.post("/api/program/runs/{program_run_id}/node-correction-previews")
    def program_node_correction_preview(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ProgramNodeCorrectionPreviewRequest,
        request: Request,
    ) -> dict[str, Any]:
        """Advisory native preview of one held motion node; not a command."""
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return service.program_node_correction_preview(
            program_run_id,
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )

    def require_control_path(program_run_id: str, request: Any) -> None:
        if request.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )

    @app.post("/api/program/runs/{program_run_id}/blackboard-values")
    async def write_runtime_blackboard(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: RuntimeBlackboardWriteRequest,
        request: Request,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await rest_execution_command(
            command_kind="program_runtime_blackboard_write",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    def require_scenario_path(scenario_id: str, request: Any) -> None:
        if request.scenario_id != scenario_id:
            raise HeadlessApiError(
                "persistent_key_path_invalid",
                "Scenario identity in path and body must match",
            )

    # Persistent blackboard values are Scenario state kept by the Program
    # runner; editing them is a value edit, not an execution command.
    @app.get("/api/program/scenarios/{scenario_id}/persistent-values")
    def program_persistent_values(scenario_id: ScenarioPath) -> dict[str, Any]:
        return service.program_persistent_values(scenario_id)

    @app.post("/api/program/scenarios/{scenario_id}/persistent-values")
    def set_program_persistent_value(
        scenario_id: ScenarioPath,
        body: PersistentKeyValueSetRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return service.program_persistent_value_action(scenario_id, "set", body)

    @app.post("/api/program/scenarios/{scenario_id}/persistent-values/reset")
    def reset_program_persistent_value(
        scenario_id: ScenarioPath,
        body: PersistentKeyValueResetRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return service.program_persistent_value_action(scenario_id, "reset", body)

    @app.post("/api/program/scenarios/{scenario_id}/persistent-values/clear")
    def clear_program_persistent_values(
        scenario_id: ScenarioPath,
        body: PersistentKeyValuesClearRequest,
    ) -> dict[str, Any]:
        require_scenario_path(scenario_id, body)
        return service.program_persistent_value_action(scenario_id, "clear", body)

    @app.post("/api/program/runs/{program_run_id}/manual-result")
    async def stage_manual_result(
        program_run_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ManualResultStageRequest,
        request: Request,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await rest_execution_command(
            command_kind="program_manual_result_stage",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/cycle/recovery-actions")
    async def cycle_recovery_action(
        body: CycleRecoveryActionRequest,
        request: Request,
    ) -> dict[str, Any]:
        return await rest_execution_command(
            command_kind="execution_recovery",
            payload=body.model_dump(mode="json"),
            request_context=body,
            transport_request=request,
        )

    def viewer_asset_response(asset: ViewerModelAsset) -> Response:
        cache_control = (
            "public, max-age=31536000, immutable"
            if asset.asset_kind == "resource"
            else "no-store"
        )
        return Response(
            content=asset.bytes,
            media_type=asset.media_type,
            headers={
                # The resource route is addressed by bundle + logical URI.
                # The URDF route selects the current active bundle and must
                # never be frozen by an intermediary across a later Apply.
                "Cache-Control": cache_control,
                "ETag": f'"{asset.content_digest}"',
                "X-ManyForge-Asset-Kind": asset.asset_kind,
                "X-ManyForge-Cell-ID": asset.cell_id,
                "X-ManyForge-Content-Digest": asset.content_digest,
                "X-ManyForge-Graph-Revision": asset.graph_revision,
                "X-ManyForge-Product-Contract-ID": asset.product_contract_id,
                "X-ManyForge-Product-Contract-Revision": (
                    asset.product_contract_revision
                ),
                "X-ManyForge-Product-ID": asset.product_id,
                "X-ManyForge-Retained-Bundle-ID": asset.retained_bundle_id,
                "X-ManyForge-Viewer-Model-Revision": (asset.viewer_model_revision),
            },
        )

    @app.get("/api/viewer-model/urdf")
    def viewer_model_urdf() -> Response:
        return viewer_asset_response(service.viewer_model_urdf())

    @app.get("/api/viewer-model/resource")
    def viewer_model_resource(
        retained_bundle_id: Annotated[
            str,
            Query(pattern=r"^sha256:[0-9a-f]{64}$"),
        ],
        logical_uri: Annotated[
            str,
            Query(min_length=1, max_length=4096),
        ],
    ) -> Response:
        return viewer_asset_response(
            service.viewer_model_resource(retained_bundle_id, logical_uri)
        )

    @app.get("/api/assistant/modes")
    def assistant_modes() -> dict[str, Any]:
        return service.assistant_modes()

    @app.get("/api/assistant/modes/{mode_id}")
    def assistant_mode(
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        return service.assistant_mode(mode_id)

    @app.get("/api/assistant/modes/{mode_id}/context")
    def assistant_mode_context(
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        return service.assistant_mode_context(mode_id)

    @app.post("/api/manager/deployment-candidates/preview")
    def manager_deployment_candidate_preview(
        request: ManagerDeploymentCandidatePreviewRequest,
    ) -> JSONResponse:
        response = service.manager_deployment_candidate_preview(request)
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager candidate rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    @app.post("/api/activation-preparations")
    def prepare_activation(
        request: ActivationPreparationRequest,
    ) -> dict[str, Any]:
        return service.prepare_activation(request)

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

    @app.get("/api/manager/apply/status")
    def manager_apply_status(
        apply_id: Annotated[str, Query(min_length=1, max_length=256)],
        apply_generation: Annotated[int, Query(ge=1, le=9_007_199_254_740_991)],
        background: BackgroundTasks,
    ) -> JSONResponse:
        response = service.manager_apply_status(apply_id, apply_generation)
        if response.get("ok") is True:
            if response.get("state") == "succeeded":
                # Carry Manager's committed publication to the Program runner
                # after this response: the Apply outcome is already final and
                # nothing the runner does can change or delay it. Manager's
                # published source follows its journal, so the runner is woken
                # only once that source names this Apply (see apply_follower).
                background.add_task(
                    service.follow_committed_apply, apply_id, apply_generation
                )
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager Apply status rejection has no stable error code",
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

    @app.get("/api/execution/diagnostics")
    def execution_diagnostics() -> JSONResponse:
        return execution_response(service.execution_diagnostics())

    @app.get("/api/execution/identity")
    def execution_identity() -> JSONResponse:
        return execution_response(service.execution_identity())

    @app.get("/api/execution/targets")
    def execution_targets() -> JSONResponse:
        return execution_response(service.execution_targets())

    @app.get("/api/execution/control-state")
    def execution_control_state() -> JSONResponse:
        return execution_response(service.execution_control_state())

    @app.websocket("/ws/execution/observations")
    async def execution_observation_stream(websocket: WebSocket) -> None:
        """Proxy one credited, change-driven native observation session."""

        await websocket.accept()
        cancelled = threading.Event()
        credit_ready = asyncio.Event()
        pending_cursor: dict[str, Any] | None = None
        has_pending_credit = False
        request_in_flight = False
        session: Any | None = None

        async def receive_credit() -> None:
            nonlocal pending_cursor, has_pending_credit
            while True:
                text = await websocket.receive_text()
                if has_pending_credit or request_in_flight:
                    raise ValueError(
                        "only one execution observation credit may be outstanding"
                    )
                pending_cursor = _execution_observation_cursor(text)
                has_pending_credit = True
                credit_ready.set()

        async def send_observation() -> None:
            nonlocal pending_cursor, has_pending_credit, request_in_flight, session
            while True:
                await credit_ready.wait()
                credit_ready.clear()
                cursor = pending_cursor
                pending_cursor = None
                has_pending_credit = False
                request_in_flight = True
                if session is None:
                    context = service.open_execution_observation_session()
                    session = await asyncio.to_thread(context.__enter__)
                response = await asyncio.to_thread(
                    session.next,
                    cursor,
                    cancelled,
                )
                await websocket.send_text(
                    json.dumps(
                        response,
                        allow_nan=False,
                        ensure_ascii=False,
                        separators=(",", ":"),
                        sort_keys=True,
                    )
                )
                request_in_flight = False

        receiver = asyncio.create_task(receive_credit())
        sender = asyncio.create_task(send_observation())
        try:
            await asyncio.wait(
                {receiver, sender},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for task in (receiver, sender):
                if task.done():
                    task.result()
        except (WebSocketDisconnect, asyncio.CancelledError):
            pass
        except (HeadlessApiError, RuntimeError, ValueError):
            with suppress(RuntimeError, WebSocketDisconnect):
                await websocket.close(
                    code=1002,
                    reason="invalid execution observation stream",
                )
        finally:
            cancelled.set()
            for task in (receiver, sender):
                task.cancel()
            if session is not None:
                with suppress(Exception):
                    session.close()
            for task in (receiver, sender):
                with suppress(
                    asyncio.CancelledError,
                    HeadlessApiError,
                    RuntimeError,
                    ValueError,
                    WebSocketDisconnect,
                ):
                    await task

    @app.post("/api/execution/operations")
    async def execution_submit(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            await rest_execution_command(
                command_kind="operation_start",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/manual-handover")
    def execution_read_manual_handover(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        """Read compact target eligibility/context; no cleanup or motion admission."""
        return execution_response(
            service.execution_read_manual_handover(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/motion-previews")
    def execution_preview_motion(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_preview_motion(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/capability-command-previews")
    def execution_preview_capability_command(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_preview_capability_command(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.get("/api/execution/operations/{operation_id}")
    def execution_read(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> JSONResponse:
        return execution_response(service.execution_read(operation_id))

    @app.post("/api/execution/pick-place/plans/pick")
    def execution_plan_pick(
        body: PickSequencePlanningRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_plan_pick(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/pick-place/plans/place")
    def execution_plan_place(
        body: PlaceSequencePlanningRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_plan_place(
                body, _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    def require_matching_operation(
        path_operation_id: str,
        body_operation_id: str,
    ) -> None:
        if path_operation_id != body_operation_id:
            raise HeadlessApiError(
                "execution_operation_path_invalid",
                "operation identity in the path and request body must match",
            )

    @app.post("/api/execution/operations/{operation_id}/reset")
    async def execution_reset(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            await rest_execution_command(
                command_kind="operation_reset",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/operations/{operation_id}/acknowledge-completed")
    async def execution_acknowledge_completed(
        operation_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            await rest_execution_command(
                command_kind="operation_acknowledge_completed",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/stops")
    async def execution_stop(
        body: StopIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            await rest_execution_command(
                command_kind="operation_stop",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/full-stop")
    async def execution_full_stop(
        body: FullStopIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            await rest_execution_command(
                command_kind="cell_full_stop",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.get("/api/execution/stop-fault")
    def execution_stop_fault() -> JSONResponse:
        return execution_response(service.execution_stop_fault())

    @app.post("/api/execution/stop-fault/recover")
    async def execution_recover_stop_fault(
        body: StopFaultRecoveryIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            await rest_execution_command(
                command_kind="stop_recovery",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/runtime-scale-control")
    async def execution_set_runtime_scale_control(
        body: RuntimeScaleControlRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            await rest_execution_command(
                command_kind="runtime_scale",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.get("/api/execution/runtime-scale-control")
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
    ) -> JSONResponse:
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
        return execution_response(service.execution_get_runtime_scale_control(selector))

    @app.get("/api/scene")
    def scene() -> dict[str, Any]:
        return service.scene()

    @app.post("/api/scene/resource-poses/query")
    def scene_resource_poses(
        body: SceneResourcePoseQuery,
    ) -> dict[str, Any]:
        return service.scene_resource_poses(body)

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

    @app.post("/api/scene/transactions/status")
    def scene_transaction_status(body: SceneTransactionRequest) -> dict[str, Any]:
        return service.scene_transaction_status(body)

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
        result = service.acknowledge_alarm(
            condition_id,
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )
        control_state_source.notify_cold()
        return result

    @app.post("/api/execution/alarms/acknowledge-all")
    def acknowledge_all(
        body: AcknowledgeAllAlarmsRequest,
        request: Request,
    ) -> dict[str, Any]:
        result = service.acknowledge_all(
            body,
            _trusted_attribution(request, trusted_principal_adapter),
        )
        control_state_source.notify_cold()
        return result

    @app.get("/api/execution/history")
    def history(
        query: Annotated[HistorianQueryRequest, Query()],
    ) -> JSONResponse:
        # Encode this potentially large bounded page in the synchronous route
        # worker instead of blocking the shared ASGI event loop.
        return JSONResponse(content=service.history(query))

    @app.get("/api/execution/history/export")
    def history_export(
        query: Annotated[HistorianQueryRequest, Query()]
    ) -> JSONResponse:
        return JSONResponse(
            content=service.history_export(query),
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-events.json"'
            },
        )

    @app.get("/api/execution/history/captures")
    def history_captures(
        query: Annotated[HistorianCaptureRequest, Query()]
    ) -> JSONResponse:
        return JSONResponse(content=service.history_captures(query))

    @app.get("/api/execution/history/discovery")
    def history_discovery(
        query: Annotated[HistorianDiscoveryRequest, Query()]
    ) -> JSONResponse:
        return JSONResponse(
            content=service.core.history_discovery(**query.model_dump())
        )

    @app.get("/api/debug/export")
    def debug_export(query: Annotated[HistorianDebugRequest, Query()]) -> JSONResponse:
        return JSONResponse(
            content=service.debug_export(query.history_query()),
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-debug.json"'
            },
        )

    @app.get("/api/debug/export/ros2-medkit")
    def normalized_trace_export(
        query: Annotated[HistorianDebugRequest, Query()]
    ) -> JSONResponse:
        return JSONResponse(
            content=service.debug_export(query.history_query())["normalized_trace"],
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-trace.json"'
            },
        )

    @app.get("/api/execution/program-runs/{program_run_id}/admitted-speeds")
    def program_run_admitted_speeds(
        program_run_id: Annotated[str, Path(min_length=1, max_length=1024)],
    ) -> JSONResponse:
        return JSONResponse(
            content=service.program_run_admitted_speeds(program_run_id)
        )

    @app.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return service.core.historian_health()

    @app.get("/api/execution/history/{sequence}")
    def history_detail(
        sequence: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
    ) -> dict[str, Any]:
        return service.history_detail(sequence)

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
