"""Independent FastAPI projection over the C++ owner process."""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager, suppress
from dataclasses import dataclass
from typing import Annotated, Any, Callable, Literal, Protocol
import hashlib
import json
import re
import threading
import time
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
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, Response
from starlette.types import ASGIApp, Message, Receive, Scope, Send

from .component_model import RUN_REFUSAL_CODES, RunReadinessRequest
from .execution import ExecutionCommandClient, ExecutionEventPump
from .manager_status_stream import ManagerStatusChanges, ManagerStatusFollower
from .control_dispatch import HeadlessExecutionControlDispatch
from .identity import (
    RequestKeyInvalid,
    RequestKeyRequired,
    StartIdentityMismatch,
    causal_display,
    identified,
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
from .request_validation import request_validation_detail
from .history_query import (
    HistorianCaptureRequest,
    HistorianQueryRequest,
    HistorianDiscoveryRequest,
    HistorianDebugRequest,
)
from .models import (
    AcknowledgeAlarmRequest,
    ApplyOutcomeRequest,
    ProgramResetOutcomeRequest,
    ComponentActionRequest,
    ComponentGroupActionRequest,
    RecoveryPreflightConfirmation,
    RecoveryGroupRequest,
    SetOutputRequest,
    AcknowledgeAllAlarmsRequest,
    ActivationPreparationRequest,
    ProgramReferenceValidationRequest,
    ComponentFrameLookupRequest,
    FullStopIntentRequest,
    RequestContext,
    IoSignalReadRequest,
    ManagerApplyRequest,
    ManagerApplyAcknowledgementRequest,
    MockIoInputRequest,
    ManagerDeploymentCandidatePreviewRequest,
    AuthoringMutationContext,
    AuthoringMutationSettlementRequest,
    ProgramModelAttemptRequest,
    ProgramDiagnosticRequest,
    SkillExpansionRequest,
    ProgramRunStartRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    ProgramNodeCorrectionPreviewRequest,
    ProgramHaltDiscardRequest,
    ProgramHaltResumeRequest,
    ProgramScenePreparationRequest,
    ProgramCycleResetRequest,
    CycleRecoveryActionRequest,
    ManualResultStageRequest,
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
    RuntimeBlackboardWriteRequest,
    OperationControlRequest,
    OperationResetRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES,
    decode_runtime_scale_selector_query,
    SceneResourcePoseQuery,
    SceneRuntimeLatchRequest,
    SceneTransactionRequest,
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
from .service import HeadlessService, TrustedAttribution, warm_builtin_node_environment
from .program_runner_client import ProgramRunnerHttpClient
from .cell_admission import CellAdmissionClient
from .alarm_attention import OperatorAlarmAttentionPublisher
from .activation_preparer import ActivationPreparer
from .disk_check import (
    EVENT_HISTORY,
    HALT_SOURCE_LABEL as DISK_HALT_SOURCE_LABEL,
    OPERATION_JOURNAL,
    DiskRecordsCheck,
    RecordFolder,
)


_REQUEST_ID_PATTERN = re.compile(r"[^A-Za-z0-9._-]")
# The execution Stop routes: an invalid body on any of them is still a Stop,
# widened by the Kernel from its route (PD-1: Halt stays Halt, the others
# become Stop all).
_EXECUTION_STOP_ROUTES = frozenset(
    {"/api/execution/stops", "/api/execution/stop-all", "/api/execution/halt"}
)
# A Program group Stop whose body is invalid is recognized on the execution
# Stop route of its kind.
_PROGRAM_STOP_ROUTES = {
    "/api/program/runs/{program_run_id}/motion-stop": "/api/execution/stops",
}
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_MAXIMUM_SIGNED_64 = 9_223_372_036_854_775_807
_MAXIMUM_ALARM_PAGE = 128
_MAXIMUM_HISTORY_PAGE = 32
_OBSERVATION_CREDIT_SCHEMA = "manyforge.execution_observation_credit.v2"
SOURCE_MODEL_REFERENCE_NOTE = (
    "The applied cell's source-local model facts, schema "
    "`manyforge.source_model_projection.v4`: `occurrences` (each model "
    "occurrence with its owning `component_id`, coordinates, links and SRDF "
    "facts), `targets` (each component command target with its frames, model "
    "groups and collision bodies) and `components`, one row per component of "
    "the applied Deployment, sorted by `component_id`. A row's `mounted_on` "
    "names the parent `{component_id, interface_id}` the component is mounted "
    "on, or is null. `source.source_content_digest` covers every field except "
    "`source`, so it changes with any of them. A reader that expects an older "
    "schema version refuses this one: reload an open Composer tab after an "
    "upgrade, and retry an assistant draft that was refused as stale. "
    "`GET /api/source-model-context` carries the same projection."
)
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
        or set(cursor) != {"application_start_id", "source_epoch", "source_sequence"}
        or not isinstance(cursor.get("application_start_id"), str)
        or not cursor["application_start_id"]
        or len(cursor["application_start_id"].encode("utf-8")) > 2_048
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
    if code == "draft_candidate_response_too_large":
        return 413
    if code == "component_frame_lookup_invalid":
        return 400
    if code in {"component_frame_lookup_stale", "component_reference_missing", "frame_missing"}:
        return 409
    if code == "program_run_started_then_failed":
        return 422
    if code == "full_stack_restart_required":
        return 409
    if code in {
        "headless_projection_response_invalid",
        "headless_projection_response_oversized",
        "manager_command_response_invalid",
        "execution_command_response_invalid",
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
        "planning_job_unknown",
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
            "program_run_admission_failed",
            "program_run_already_active",
            "program_applied_non_executable",
            "program_not_selected",
            # Preparation runs before any claim or effect; it refuses the input.
            "activation_preparation_failed",
            "program_reference_validation_failed",
            "prepared_activation_unverified",
            "authoring_not_ready",
            "program_run_draft_applied_mismatch",
            "activation_preparer_busy",
            "model_sources_unknown",
            "program_node_preview_busy",
            "deployment_candidate_cell_mismatch",
            "persistent_key_request_replayed",
            # An operator-alarm condition interlock refuses motion before any effect.
            "operator_alarm_interlock_active",
            "request_result_expired",
            # Component model v2: refusals before dispatch.
            "component_action_in_progress",
            "component_action_unsupported",
            "owner_not_prepared",
            "io_output_controller_owned",
            # A position reset while the simulation isn't playing.
            "simulation_not_playing",
            # An assistant mode read while the Deployment has no assistant.
            "assistant_component_absent",
            # Pick/place planning jobs: a held name, a release before the end.
            "planning_job_exists",
            "planning_job_unfinished",
        }
        # RunReadiness reasons and run refusals.
        or code in RUN_REFUSAL_CODES
    ):
        return 409
    if "unavailable" in code or code in {
        "assistant_ai_model_absent",
        "headless_api_closed",
        "kernel_owner_unavailable",
        "execution_owner_not_ready",
        "operator_alarm_state_unavailable",
        "program_applied_selection_not_active",
        "program_runner_unavailable",
        "program_run_scheduling_failed",
        "program_cycle_reconciliation_required",
        "request_ledger_full",
        "scene_projection_commit_uncertain",
        "execution_control_ingress_busy",
        "execution_planning_jobs_busy",
        # A runtime unit launching or preparing (contract 1.3).
        "runtime_unit_launch_in_progress",
    }:
        return 503
    if (
        code == "invalid_request"
        or code.endswith("_invalid")
        or code.endswith("_rejected")
    ):
        return 400
    if code in {
        "confirmation_required",
        "request_key_required",
        "start_identity_required",
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


def _conditional_projection(
    request: Request,
    payload: dict[str, Any],
    sources: tuple[dict[str, Any], ...],
    identity: list[Any],
) -> dict[str, Any] | Response:
    """Answer a projection read with a strong ETag and honour If-None-Match.

    ``identity`` holds content digests only (no owner epoch or generation), so
    a restarted Headless serving the same content keeps the same tag and a 304
    stays correct. A source that is not fresh and complete gets no tag: the
    reader then sees the full body and fails closed as before.
    """

    if not all(
        source.get("available") is True
        and source.get("freshness") == "fresh"
        and source.get("known_gaps") == []
        for source in sources
    ):
        return payload
    etag = (
        '"'
        + hashlib.sha256(
            json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()
        + '"'
    )
    if request.headers.get("if-none-match") == etag:
        return Response(status_code=304, headers={"ETag": etag})
    return JSONResponse(payload, headers={"ETag": etag})


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
    manager_status_follower: ManagerStatusFollower | None = None
    manager_status_changes = ManagerStatusChanges()
    viewer_pose_sources: Callable[[], tuple[str, str] | None] | None = None
    alarm_attention: OperatorAlarmAttentionPublisher | None = None
    disk_check: DiskRecordsCheck | None = None
    program_runner_client: ProgramRunnerHttpClient | None = None
    if service is None:
        assert config is not None
        # Before serving: never on request threads while machines prepare.
        warm_builtin_node_environment()
        core = NativeHeadlessCore(config)
        manager = ManagerCommandClient(
            config.manager_run_root,
            timeout_seconds=config.manager_timeout_seconds,
        )
        execution = ExecutionCommandClient(
            config.execution_run_root,
            timeout_seconds=config.execution_timeout_seconds,
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
            activation_preparer=activation_preparer,
            application_start_id=config.application_start_id,
        )
        event_execution = ExecutionCommandClient(
            config.execution_run_root,
            timeout_seconds=min(config.execution_timeout_seconds, 1.0),
        )
        # OD-216: the Manager's state from its status stream, followed for the
        # application's lifetime; each change or liveness change wakes the
        # control state, the scene commits waiting for their publication
        # (OD-293) and the viewer pose streams.
        scene_commits_wake = service.manager_status_changed

        def manager_control_changed() -> None:
            control_state_source.notify_cold()
            scene_commits_wake()

        manager_status_follower = ManagerStatusFollower(
            core,
            config.manager_run_root,
            silence_seconds=config.freshness_milliseconds / 1_000.0,
            on_control_change=manager_control_changed,
            on_viewer_change=manager_status_changes.notify,
            on_disconnect=service.manager_status_disconnected,
        )
        viewer_pose_sources = core.viewer_pose_sources
        event_pump = ExecutionEventPump(
            core,
            event_execution,
            application_start_id=config.application_start_id,
            poll_seconds=config.execution_event_poll_milliseconds / 1_000.0,
        )
        # Its own thread, reading the owner's published projection without the
        # core lock: neither event-pump work nor any core call can delay the
        # Kernel heartbeat. Each publication waits for the Kernel's answer;
        # the Kernel's freshness bound alone judges staleness (OD-307).
        alarm_attention = OperatorAlarmAttentionPublisher(
            core,
            CellAdmissionClient(
                config.execution_run_root,
                timeout_seconds=config.execution_timeout_seconds,
            ),
        )
        if config.disk_free_floor_bytes is not None:
            assert config.disk_free_margin_bytes is not None
            # OD-215: its own thread. The Halt goes on the Kernel's Stop
            # socket with the internal transport, as the Manager's Halt for a
            # late execution host; the journal's level comes from the pump.
            disk_halt = ExecutionCommandClient(
                config.execution_run_root,
                timeout_seconds=min(config.execution_timeout_seconds, 1.0),
                attribution_transport="internal",
            )
            pump = event_pump
            disk_admission = service.admission
            disk_check = DiskRecordsCheck(
                folders=(
                    RecordFolder(OPERATION_JOURNAL, config.execution_run_root.parent),
                    RecordFolder(EVENT_HISTORY, config.historian_directory),
                ),
                floor_bytes=config.disk_free_floor_bytes,
                margin_bytes=config.disk_free_margin_bytes,
                history_signals=core.history_disk_signals,
                journal_signals=lambda: pump.journal_disk_signals,
                halt=lambda: disk_halt.halt(
                    FullStopIntentRequest(
                        request_context=RequestContext(
                            request_key=disk_admission.mint_key(),
                            source_label=DISK_HALT_SOURCE_LABEL,
                        )
                    ),
                    trusted_principal_id=None,
                ),
                observe_condition=core.observe_disk_records_condition,
            )
    assert service is not None
    # One ``A`` check and one Headless key minter, shared with the service.
    admission = service.admission
    execution_controls = ExecutionControlClient(
        thread_name_prefix="manyforge_headless_control",
    )
    control_dispatch = HeadlessExecutionControlDispatch(service, admission)
    control_ingress = ExecutionControlIngress(
        projection=control_dispatch.control_revision,
        dispatch=control_dispatch,
        application_start_id=admission.application_start_id,
        lanes=execution_controls,
    )
    runner_control_url = getattr(
        getattr(service, "program_runner", None),
        "control_state_websocket_url",
        None,
    )
    if not isinstance(runner_control_url, str):
        runner_control_url = None
    control_state_source = HeadlessControlStateSource(
        service,
        control_ingress,
        program_runner_websocket_url=runner_control_url,
        incident_recorder=getattr(service, "record_client_incident", None),
        # The owned native core raises the ordinary Program-failure alarm from
        # the accepted Run; an injected service keeps the demand-only reader.
        program_failure_observer=(
            service.core.observe_program_failure_condition if owns_core else None
        ),
        # A Reset's end (its row) clears the initial-scene failure (OD-214).
        runner_command_observer=(
            service.observe_runner_commands
            if isinstance(service, HeadlessService)
            else None
        ),
    )
    if isinstance(service, HeadlessService):
        service.initial_scene_observer = control_state_source.notify_cold
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
        event_pump.set_global_rules_observer(control_state_source.notify_global_rules)
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

    def start_identity(value: str | None) -> None:
        """The one ``A`` check of a request naming a start-minted value."""

        try:
            admission.start_identity(value)
        except StartIdentityMismatch as mismatch:
            raise HeadlessApiError(
                mismatch.code,
                "the request names a value of another application start",
            ) from mismatch

    def read_start_identity(value: str | None, unknown: Exception) -> None:
        """A read naming a value of another start answers as an unknown id."""

        if value is None:
            start_identity(value)
        if value != admission.application_start_id:
            raise unknown

    def history_start_identity(query: Any) -> None:
        """A History query naming no start means the current one for its
        numbers, so a display form's 4-hex prefix must be the current start's;
        an earlier start is named by ``application_start_id`` or a full form.
        """

        full, prefixes = query.start_identities()
        current = admission.application_start_id
        if not full and any(prefix != current[:4] for prefix in prefixes):
            raise HeadlessApiError(
                "historian_query_invalid",
                "a display form names another application start; name that start "
                "with application_start_id or use the full form",
            )

    def shown(reply: dict[str, Any]) -> dict[str, Any]:
        """A reply naming an id carries ``A`` and its display (1.5)."""

        return identified(reply, admission.application_start_id)

    def http_key(raw: str | None) -> str:
        try:
            return admission.http_key(raw, stop=False)
        except RequestKeyRequired as error:
            raise HeadlessApiError(
                "request_key_required",
                "an effectful request must carry its request_key",
            ) from error
        except RequestKeyInvalid as error:
            raise HeadlessApiError(
                "request_key_invalid",
                "request_key must be exactly 32 hex characters",
            ) from error

    def admitted(body: Any, *, start_value: bool = True) -> Any:
        """Check ``A`` and form the key of one effectful HTTP request.

        The returned copy carries the formed key; ``A`` is never
        serialized downstream: nothing there compares it (contract 1.6).
        """

        if start_value:
            start_identity(body.application_start_id)
        context = getattr(body, "request_context", None)
        if context is not None:
            return body.model_copy(
                update={
                    "request_context": context.model_copy(
                        update={"request_key": http_key(context.request_key)}
                    ),
                }
            )
        return body.model_copy(
            update={
                "request_key": http_key(body.request_key),
            }
        )

    def command_payload(body: Any) -> dict[str, Any]:
        """The command payload of a REST route, with the ``A`` it carries.

        ``application_start_id`` is never serialized by the models; the
        dispatch checks it once and drops it.
        """

        return {
            **body.model_dump(mode="json"),
            "application_start_id": body.application_start_id,
        }

    def preview_admitted(body: Any) -> Any:
        """Check ``A`` of an effect-free request; a client key is used when
        given, otherwise Headless gives the request its own key."""

        start_identity(body.application_start_id)
        raw = body.request_context.request_key
        key = admission.mint_key() if raw is None else http_key(raw)
        return body.model_copy(
            update={
                "request_context": body.request_context.model_copy(
                    update={"request_key": key}
                ),
            }
        )

    def plan_admitted(body: Any) -> Any:
        """Each phase of a pick/place plan is checked and keyed as its own request."""

        return body.model_copy(
            update={
                name: admitted(getattr(body, name))
                for name in (
                    "approach_operation",
                    "descend_operation",
                    "tool_operation",
                    "retreat_operation",
                )
            }
        )

    async def rest_execution_command(
        *,
        command_kind: str,
        payload: dict[str, Any],
        request_context: Any,
        transport_request: Request,
        rejection_as_result: bool = False,
    ) -> dict[str, Any]:
        """Adapt a named REST route to the one semantic command ingress.

        ``rejection_as_result`` returns an ingress rejection (refused before
        the owner was called) as an ``ok: false`` reply instead of raising.
        """

        if is_stop_command(command_kind):
            # Stop must reach its reserved lane without first waiting on an
            # owner projection. Its freshness is never validated; these
            # envelope fields grant no authority.
            control_revision = 1
        else:
            # This synchronous Program Runner read can itself make a REST
            # request. Keep it off the ASGI event loop and within the existing
            # bounded ordinary-control lane. Recognized Stop retains its
            # projection-free reserved path above.
            control_revision = await control_call(control_dispatch.control_revision)
        freshness = control_ingress.freshness_for(control_revision)
        # The payload keeps the client's key and ``A``: the dispatch checks
        # ``A`` and forms the key once, for every session-less command.
        command = ExecutionCommandRequestDTO.model_validate(
            {
                "command_envelope": {
                    "schema_version": "manyforge.execution_command.v2",
                    "command_kind": command_kind,
                    "source_label": request_context.source_label,
                    "control_session_number": None,
                    "client_sequence": None,
                    "freshness": freshness.model_dump(mode="json"),
                    "expected_execution_control_revision": control_revision,
                },
                "payload": dict(payload),
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
            if rejection_as_result:
                return {
                    "ok": False,
                    "error": {"code": submission.receipt.code},
                    "ingress_rejected": True,
                }
            raise HeadlessApiError(
                submission.receipt.code,
                "execution command was rejected by its current owner",
            )
        return {} if submission.result is None else shown(dict(submission.result))

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        if manager_status_follower is not None:
            manager_status_follower.start()
        if event_pump is not None:
            event_pump.start()
        if alarm_attention is not None:
            alarm_attention.start()
        if disk_check is not None:
            disk_check.start()
        try:
            if owns_core:
                # Application-lifetime demand on the one Program reader, so the
                # Program-failure condition is followed with no browser connected.
                await control_state_source.start_program_demand()
            yield
        finally:
            try:
                # Stop following Applies before their owners' clients close.
                service.close()
                await control_state_source.close()
                if owns_core:
                    try:
                        try:
                            if disk_check is not None:
                                disk_check.shutdown()
                            if alarm_attention is not None:
                                alarm_attention.shutdown()
                        finally:
                            try:
                                if event_pump is not None:
                                    event_pump.shutdown()
                            finally:
                                if manager_status_follower is not None:
                                    manager_status_follower.shutdown()
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
        version="0.3.1",
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
            # The same candidate inputs (Deployment, model sources, Program);
            # the preparer applies this bound to them again.
            "/api/program/reference-validation": (
                MAX_ACTIVATION_PREPARATION_REQUEST_BYTES
            ),
            "/api/program/model-attempts": MAX_PROGRAM_MODEL_ATTEMPT_BYTES,
            "/api/program/diagnostics": 64 * 1024,
        },
        recognized_stop_paths=_EXECUTION_STOP_ROUTES,
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
                    "producer": error.producer,
                    "request_id": getattr(request.state, "request_id", None),
                }
            },
        )

    @app.exception_handler(Exception)
    async def internal_error_handler(
        request: Request,
        error: Exception,
    ) -> JSONResponse:
        # An uncaught failure still answers with the typed carrier, never an
        # anonymous 500 body.
        return JSONResponse(
            status_code=500,
            content={
                "detail": {
                    "code": "headless_api_internal_failure",
                    "details": {},
                    "message": str(error) or type(error).__name__,
                    "producer": "headless_api",
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
                    "producer": "headless_api",
                    "request_id": getattr(request.state, "request_id", None),
                }
            },
        )

    async def recognized_malformed_stop(
        request: Request,
        recognized_route: str,
        source_label: str | None = None,
    ) -> Response:
        """An invalid Stop request is still a Stop, recognized on its route.

        ``source_label`` is the caller's label where the body was validated.
        """

        reason = getattr(
            request.state,
            "recognized_stop_malformed_reason",
            "recognized_stop_payload_invalid",
        )
        request_key = admission.mint_key()
        attribution = _trusted_attribution(
            request,
            trusted_principal_adapter,
        )
        try:
            response = execution_response(
                await control_call(
                    lambda: service.execution_recognized_stop(
                        request_key=request_key,
                        recognized_route=recognized_route,
                        reason=reason,
                        source_label=source_label,
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

    @app.exception_handler(RequestValidationError)
    async def request_validation_handler(
        request: Request,
        error: RequestValidationError,
    ) -> Response:
        route = getattr(request.scope.get("route"), "path", None)
        recognized_route = (
            request.url.path
            if request.url.path in _EXECUTION_STOP_ROUTES
            else _PROGRAM_STOP_ROUTES.get(route)
        )
        if request.method == "POST" and recognized_route is not None:
            return await recognized_malformed_stop(request, recognized_route)
        return JSONResponse(
            status_code=422,
            content={
                "detail": request_validation_detail(
                    error.errors(),
                    producer="headless_api",
                    request_id=getattr(request.state, "request_id", None),
                )
            },
        )

    @app.get("/api/health")
    def health() -> dict[str, Any]:
        return service.health()

    @app.get("/api/runtime")
    def runtime() -> dict[str, Any]:
        return service.runtime()

    # Component model v2. ------------------

    @app.get("/api/components")
    def components() -> list[Any]:
        """Every ComponentState, in cell order."""
        return service.components()

    @app.post("/api/components/actions", status_code=202)
    def component_group_action(body: ComponentGroupActionRequest) -> dict[str, Any]:
        """One action on several components, one at a time in cell order."""
        return service.component_actions(body.action, body.component_ids)

    @app.post("/api/components/{component_id}/actions", status_code=202)
    def component_action(
        component_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: ComponentActionRequest,
    ) -> dict[str, Any]:
        """Accepted action; its result arrives in `last_action`."""
        return service.component_action(component_id, body.action)

    @app.get("/api/components/{component_id}/recover-enable/preflight")
    def component_recovery_preflight(component_id: Annotated[str, Path(min_length=1, max_length=256)]) -> dict[str, Any]:
        return service.component_recovery_preflight(component_id)

    @app.post("/api/components/{component_id}/recover-enable", status_code=202)
    def component_recover_enable(
        component_id: Annotated[str, Path(min_length=1, max_length=256)], body: RecoveryPreflightConfirmation,
    ) -> dict[str, Any]:
        start_identity(body.application_start_id)
        return service.component_recover_enable(
            component_id,
            body.model_dump(mode="json"),
        )

    @app.post("/api/components/recover-enable", status_code=202)
    def component_recover_enable_group(body: RecoveryGroupRequest) -> dict[str, Any]:
        start_identity(body.application_start_id)
        return service.component_recover_enable_group(body.model_dump(mode="json")["targets"])

    @app.get("/api/cell/latch")
    def cell_latch() -> dict[str, Any]:
        return service.cell_latch()

    @app.get("/api/io")
    def io_signals() -> list[Any]:
        """Every SignalSample."""
        return service.io_signals()

    @app.post("/api/io/{signal_id:path}/set")
    def set_io_output(
        signal_id: Annotated[str, Path(min_length=1, max_length=256)],
        body: SetOutputRequest,
    ) -> dict[str, Any]:
        """One output write; returns SetOutputResult."""
        return service.set_io_output(signal_id, body.value)

    @app.get("/api/program/readiness")
    def program_readiness() -> dict[str, Any]:
        """RunReadiness: can_execute, can_resume, blocked_reasons."""
        return service.run_readiness(control_state_source.held_cycle_facts())

    @app.post("/api/program/readiness")
    def program_runner_readiness(body: RunReadinessRequest) -> dict[str, Any]:
        """RunReadiness for the Program runner, from its own cycle facts."""
        return service.run_readiness(body.program_cycle.model_dump())

    @app.get("/api/cell/authoring-admission")
    def authoring_admission() -> dict[str, Any]:
        return service.authoring_admission()

    @app.post("/api/program/authoring-transactions")
    def acquire_authoring_transaction(
        body: AuthoringMutationContext,
    ) -> dict[str, Any]:
        claim = service.acquire_authoring_mutation(admitted(body))
        control_state_source.notify_cold()
        return {"claim": claim}

    @app.post("/api/program/authoring-transactions/settle")
    def settle_authoring_transaction(
        body: AuthoringMutationSettlementRequest,
    ) -> dict[str, Any]:
        admission = service.settle_authoring_mutation(admitted(body))
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

    @app.get("/api/node-environment")
    def node_environment(request: Request) -> dict[str, Any]:
        projection = service.node_environment()
        # The Runner join rewrites ``availability`` outside the core digest.
        return _conditional_projection(
            request,
            projection,
            (projection["source"],),
            [projection["source"]["source_content_digest"], projection["availability"]],
        )

    @app.get("/api/skills")
    def skills() -> dict[str, Any]:
        return service.skills()

    @app.post("/api/skills/expand")
    def expand_skill(body: SkillExpansionRequest) -> dict[str, Any]:
        return service.expand_skill(body)

    @app.get(
        "/api/source-model",
        openapi_extra={"x-manyforge-reference-note": SOURCE_MODEL_REFERENCE_NOTE},
    )
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
        start_identity(request.application_start_id)
        return service.program_model_attempt(request)

    @app.post("/api/program/diagnostics")
    def program_diagnostic(request: ProgramDiagnosticRequest) -> dict[str, Any]:
        start_identity(request.application_start_id)
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
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/scene-preparations")
    async def program_scene_prepare(
        request: ProgramScenePreparationRequest,
    ) -> dict[str, Any]:
        # The client's key is required (Q8) but each forwarded attempt gets
        # its own Headless key (BS 11.6), so the formed key is not passed on.
        request = admitted(request)
        return await json_control_call(lambda: service.program_scene_prepare(request))

    @app.get("/api/program/runs/{program_run_id}")
    def program_run_read(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        application_start_id: Annotated[str | None, Query(max_length=16)] = None,
    ) -> dict[str, Any]:
        read_start_identity(
            application_start_id,
            HeadlessApiError("program_run_not_found", "Program run is not known"),
        )
        return shown(service.program_run_read(program_run_id))

    @app.post("/api/program/runs/{program_run_id}/motion-stop")
    async def program_run_motion_stop(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: ProgramSelectionStopRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            # Path and body name different runs: an invalid Stop, so Stop all.
            return await recognized_malformed_stop(  # type: ignore[return-value]
                request,
                _PROGRAM_STOP_ROUTES["/api/program/runs/{program_run_id}/motion-stop"],
                body.source_label,
            )
        return await rest_execution_command(
            command_kind="program_motion_stop",
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/resume-motion")
    async def program_run_motion_resume(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
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
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/motion-start")
    async def program_run_motion_start(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
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
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/manual-handover")
    async def program_run_manual_handover(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
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
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/resume-halt")
    async def program_run_resume_halt(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: ProgramHaltResumeRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_resume_halt",
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.post("/api/program/runs/{program_run_id}/reset")
    async def program_run_discard_halt(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: ProgramHaltDiscardRequest,
        request: Request,
    ) -> dict[str, Any]:
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        return await rest_execution_command(
            command_kind="program_discard_halt",
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.get("/api/cycle/status")
    def program_cycle_status() -> dict[str, Any]:
        status = service.program_cycle_status(include_scene_clear=True)
        # Headless adds the display of the causal key (1.16.2).
        return {
            **status,
            "causal_display": causal_display(
                admission.application_start_id, status.get("causal_request_key")
            ),
        }

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
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    @app.get("/api/program/runs/{program_run_id}/runtime-controls")
    def program_runtime_controls(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        application_start_id: Annotated[str | None, Query(max_length=16)] = None,
    ) -> dict[str, Any]:
        read_start_identity(
            application_start_id,
            HeadlessApiError("program_run_not_found", "Program run is not known"),
        )
        return shown(service.program_runtime_controls(program_run_id))

    @app.post("/api/program/runs/{program_run_id}/node-correction-previews")
    def program_node_correction_preview(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: ProgramNodeCorrectionPreviewRequest,
        request: Request,
    ) -> dict[str, Any]:
        """Advisory native preview of one held motion node; not a command."""
        if body.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )
        start_identity(body.application_start_id)
        # A preview has no effect: a client key is used when given, else
        # Headless gives the preview its own key.
        key = admission.mint_key() if body.request_key is None else http_key(body.request_key)
        return shown(
            service.program_node_correction_preview(
                program_run_id,
                body.model_copy(update={"request_key": key}),
                _trusted_attribution(request, trusted_principal_adapter),
            )
        )

    def require_control_path(program_run_id: int, request: Any) -> None:
        if request.program_run_id != program_run_id:
            raise HeadlessApiError(
                "program_run_path_invalid",
                "Program-run identity in path and body must match",
            )

    @app.post("/api/program/runs/{program_run_id}/blackboard-values")
    async def write_runtime_blackboard(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: RuntimeBlackboardWriteRequest,
        request: Request,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await rest_execution_command(
            command_kind="program_runtime_blackboard_write",
            payload=command_payload(body),
            request_context=body,
            transport_request=request,
        )

    # Persistent blackboard values are state of the Applied Scenario project,
    # kept by the Program runner; editing them is a value edit, not an
    # execution command. An edit names the project folder the operator saw.
    @app.get("/api/program/persistent-values")
    def program_persistent_values() -> dict[str, Any]:
        return service.program_persistent_values()

    @app.post("/api/program/persistent-values")
    def set_program_persistent_value(
        body: PersistentKeyValueSetRequest,
    ) -> dict[str, Any]:
        return service.program_persistent_value_action(
            "set", admitted(body, start_value=body.program_run_id is not None)
        )

    @app.post("/api/program/persistent-values/reset")
    def reset_program_persistent_value(
        body: PersistentKeyValueResetRequest,
    ) -> dict[str, Any]:
        return service.program_persistent_value_action(
            "reset", admitted(body, start_value=body.program_run_id is not None)
        )

    @app.post("/api/program/persistent-values/clear")
    def clear_program_persistent_values(
        body: PersistentKeyValuesClearRequest,
    ) -> dict[str, Any]:
        return service.program_persistent_value_action(
            "clear", admitted(body, start_value=False)
        )

    @app.post("/api/program/runs/{program_run_id}/manual-result")
    async def stage_manual_result(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: ManualResultStageRequest,
        request: Request,
    ) -> dict[str, Any]:
        require_control_path(program_run_id, body)
        return await rest_execution_command(
            command_kind="program_manual_result_stage",
            payload=command_payload(body),
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
            payload=command_payload(body),
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
        request: Request,
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        projection = service.assistant_mode(mode_id)
        source = projection["source"]
        return _conditional_projection(
            request, projection, (source,), [source["source_content_digest"]]
        )

    @app.get("/api/assistant/modes/{mode_id}/context")
    def assistant_mode_context(
        request: Request,
        mode_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9.-]*$"),
        ],
    ) -> dict[str, Any]:
        context = service.assistant_mode_context(mode_id)
        sources = tuple(
            context[part]["source"]
            for part in ("assistant_mode", "node_environment", "source_model")
        )
        return _conditional_projection(
            request,
            context,
            sources,
            [source["source_content_digest"] for source in sources],
        )

    @app.post("/api/manager/deployment-candidates/preview")
    def manager_deployment_candidate_preview(
        request: ManagerDeploymentCandidatePreviewRequest,
    ) -> JSONResponse:
        start_identity(request.application_start_id)
        response = service.manager_deployment_candidate_preview(
            request
        )
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

    @app.post("/api/program/reference-validation")
    def validate_program_references(request: ProgramReferenceValidationRequest) -> dict[str, Any]:
        return service.validate_program_references(request)

    @app.post("/api/program/component-frame-lookup")
    def lookup_component_frame(request: ComponentFrameLookupRequest) -> dict[str, Any]:
        payload = request.model_dump(mode="json")
        result = service.core.lookup_component_frame(payload)
        expected = {**payload, "schema_version": "manyforge.component_frame_lookup_result.v1", "exists": True}
        if result != expected:
            raise HeadlessApiError("headless_projection_response_invalid", "Component frame lookup returned an invalid result")
        return result

    @app.post("/api/activation-preparations")
    def prepare_activation(request: ActivationPreparationRequest) -> dict[str, Any]:
        """Answered at acceptance (OD-214); the preparation's row is published
        in `GET /api/cycle/status` (`activation_preparation`)."""

        return service.prepare_activation(
            admitted(request),
            # The preparer's definite refusals (no effect) end the row
            # `refused`, as their HTTP status said before.
            refused=lambda code: 400 <= _error_status(code) < 500,
        )

    @app.get("/api/activation-preparations/{captured_input_digest}")
    def prepared_activation(
        captured_input_digest: Annotated[str, Path(pattern=r"^sha256:[0-9a-f]{64}$")],
    ) -> dict[str, Any]:
        """The prepared activation once its preparation ended ``succeeded``."""

        return service.prepared_activation(captured_input_digest)

    @app.post("/api/manager/apply")
    def manager_apply(request: ManagerApplyRequest) -> JSONResponse:
        response = service.manager_apply(admitted(request))
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
        apply_id: Annotated[int, Query(ge=1, le=9_007_199_254_740_991)],
        apply_generation: Annotated[int, Query(ge=1, le=9_007_199_254_740_991)],
        background: BackgroundTasks,
        application_start_id: Annotated[str | None, Query(max_length=16)] = None,
    ) -> JSONResponse:
        read_start_identity(
            application_start_id,
            HeadlessApiError("apply_status_not_found", "Apply status is not known"),
        )
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

    @app.post("/api/manager/apply/acknowledge")
    def manager_apply_acknowledge(
        apply_id: Annotated[int, Query(ge=1, le=9_007_199_254_740_991)],
        apply_generation: Annotated[int, Query(ge=1, le=9_007_199_254_740_991)],
        request: ManagerApplyAcknowledgementRequest,
    ) -> JSONResponse:
        response = service.manager_apply_acknowledge(
            apply_id, apply_generation, admitted(request).request_context
        )
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager Apply acknowledgement rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    @app.post("/api/manager/apply/release-for-new")
    def manager_apply_release_for_new(
        request: ManagerApplyAcknowledgementRequest,
    ) -> JSONResponse:
        """New: release the fence and claim a failed Apply still holds,
        whatever it changed; New's Apply then empties the cell."""

        response = service.manager_apply_release_for_new(
            admitted(request, start_value=False).request_context
        )
        if response.get("ok") is True:
            return JSONResponse(status_code=200, content=response)
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else None
        if not isinstance(code, str) or not code:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager Apply release for New rejection has no stable error code",
            )
        return JSONResponse(status_code=_error_status(code), content=response)

    def execution_response(response: dict[str, Any]) -> JSONResponse:
        response = shown(response)
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

    @app.post("/api/execution/io-signals/read")
    def execution_read_io_signal(body: IoSignalReadRequest) -> JSONResponse:
        """Read one bound boolean signal; no claim, no effect."""
        return execution_response(service.execution_read_io_signal(body))

    @app.post("/api/execution/io-signals/mock-input")
    def execution_set_mock_io_input(body: MockIoInputRequest) -> JSONResponse:
        """Set one Mock input (simulation; never a hardware command)."""
        return execution_response(service.execution_set_mock_io_input(body))

    @app.get("/api/execution/control-state")
    def execution_control_state() -> JSONResponse:
        return execution_response(service.execution_control_state())

    @app.websocket("/ws/scene/viewer-poses")
    async def viewer_pose_stream(websocket: WebSocket) -> None:
        from .viewer_pose_stream import serve_viewer_pose_stream

        await serve_viewer_pose_stream(
            websocket,
            viewer_pose_sources,
            None if viewer_pose_sources is None else manager_status_changes,
            None if config is None else config.cell_id,
            None if config is None else service.manager.scene_runtime_owner,
        )

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
                payload=command_payload(body),
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
                preview_admitted(body), _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/motion-previews")
    def execution_preview_motion(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_preview_motion(
                preview_admitted(body), _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/capability-command-previews")
    def execution_preview_capability_command(
        body: OperationIntentRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_preview_capability_command(
                preview_admitted(body), _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.get("/api/execution/operations/{operation_id}")
    def execution_read(
        operation_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        application_start_id: Annotated[str | None, Query(max_length=16)] = None,
    ) -> JSONResponse:
        if application_start_id is None:
            start_identity(application_start_id)
        if application_start_id != admission.application_start_id:
            # The reply of an unknown operation (contract 1.6).
            return execution_response(
                {
                    "ok": False,
                    "error": {
                        "code": "operation_not_resident",
                        "message": "operation is not resident or retained",
                    },
                }
            )
        return execution_response(service.execution_read(operation_id))

    @app.post("/api/execution/pick-place/plans/pick")
    def execution_plan_pick(
        body: PickSequencePlanningRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_plan_pick(
                plan_admitted(body), _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    @app.post("/api/execution/pick-place/plans/place")
    def execution_plan_place(
        body: PlaceSequencePlanningRequest,
        request: Request,
    ) -> JSONResponse:
        return execution_response(
            service.execution_plan_place(
                plan_admitted(body), _trusted_attribution(request, trusted_principal_adapter)
            )
        )

    # A pick/place plan is answered at acceptance with its job (OD-213 (1));
    # its state, and its result once done, are read here. Memory only on the
    # execution host: no wait and no time limit in any of these.
    @app.get("/api/execution/pick-place/plans/{job_id}")
    def execution_read_planning_job(
        job_id: Annotated[str, Path(min_length=1, max_length=96)],
    ) -> JSONResponse:
        return execution_response(service.execution_read_planning_job(job_id))

    @app.post("/api/execution/pick-place/plans/{job_id}/cancel")
    def execution_cancel_planning_job(
        job_id: Annotated[str, Path(min_length=1, max_length=96)],
    ) -> JSONResponse:
        return execution_response(service.execution_cancel_planning_job(job_id))

    @app.post("/api/execution/pick-place/plans/{job_id}/release")
    def execution_release_planning_job(
        job_id: Annotated[str, Path(min_length=1, max_length=96)],
    ) -> JSONResponse:
        return execution_response(service.execution_release_planning_job(job_id))

    def require_matching_operation(
        path_operation_id: int,
        body_operation_id: int,
    ) -> None:
        if path_operation_id != body_operation_id:
            raise HeadlessApiError(
                "execution_operation_path_invalid",
                "operation identity in the path and request body must match",
            )

    @app.post("/api/execution/operations/{operation_id}/reset")
    async def execution_reset(
        operation_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: OperationResetRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            await rest_execution_command(
                command_kind="operation_reset",
                payload=command_payload(body),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/operations/{operation_id}/acknowledge-completed")
    async def execution_acknowledge_completed(
        operation_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        body: OperationControlRequest,
        request: Request,
    ) -> JSONResponse:
        require_matching_operation(operation_id, body.operation_id)
        return execution_response(
            await rest_execution_command(
                command_kind="operation_acknowledge_completed",
                payload=command_payload(body),
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
                payload=command_payload(body),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/stop-all")
    async def execution_stop_all(
        body: FullStopIntentRequest,
        request: Request,
    ) -> JSONResponse:
        """Stop all: every operation in the cell; every group held."""
        return execution_response(
            await rest_execution_command(
                command_kind="stop_all",
                payload=body.model_dump(mode="json"),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.post("/api/execution/halt")
    async def execution_halt(
        body: FullStopIntentRequest,
        request: Request,
    ) -> JSONResponse:
        """Halt: Stop all and the Program frozen; works with no Program."""
        return execution_response(
            await rest_execution_command(
                command_kind="halt",
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
                payload=command_payload(body),
                request_context=body.request_context,
                transport_request=request,
            )
        )

    @app.get("/api/execution/runtime-scale-control")
    def execution_get_runtime_scale_control(
        request: Request,
        program_binding_id: Annotated[str, Query(min_length=1, max_length=256)],
        component_id: Annotated[
            str | None, Query(min_length=1, max_length=256)
        ] = None,
        command_target_id: Annotated[
            str | None, Query(min_length=1, max_length=256)
        ] = None,
        group_targets: Annotated[
            str | None, Query(max_length=MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES)
        ] = None,
    ) -> JSONResponse:
        selector_keys = (
            "program_binding_id",
            "component_id",
            "command_target_id",
            "group_targets",
        )
        if any(len(request.query_params.getlist(key)) > 1 for key in selector_keys):
            raise HTTPException(
                422, "runtime-scale selector query keys must be singular"
            )
        if group_targets is None:
            payload = {
                "program_binding_id": program_binding_id,
                "component_id": component_id,
                "command_target_id": command_target_id,
            }
        else:
            if component_id is not None or command_target_id is not None:
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

    @app.get("/api/scene/runtime-owner")
    def scene_runtime_owner() -> dict[str, Any]:
        return service.scene_runtime_owner()

    @app.post("/api/scene/runtime-latch")
    def scene_runtime_latch(body: SceneRuntimeLatchRequest) -> dict[str, Any]:
        """The Program Runner's between-nodes latch (OD-294); answered once
        the logical scene shows what it committed, like a Scene commit."""

        return service.scene_runtime_latch_between_nodes(body)

    @app.post("/api/scene/transactions")
    def scene_commit(
        body: SceneTransactionRequest,
        request: Request,
    ) -> JSONResponse:
        response = service.scene_commit(
            admitted(body), _trusted_attribution(request, trusted_principal_adapter)
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
        return service.scene_transaction_status(admitted(body))

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
            admitted(body),
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
            admitted(body),
            _trusted_attribution(request, trusted_principal_adapter),
        )
        control_state_source.notify_cold()
        return result

    @app.get("/api/execution/history")
    def history(
        query: Annotated[HistorianQueryRequest, Query()],
    ) -> JSONResponse:
        history_start_identity(query)
        # Encode this potentially large bounded page in the synchronous route
        # worker instead of blocking the shared ASGI event loop.
        return JSONResponse(content=service.history(query))

    @app.get("/api/execution/history/export")
    def history_export(
        query: Annotated[HistorianQueryRequest, Query()]
    ) -> JSONResponse:
        history_start_identity(query)
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
        history_start_identity(query.history_query())
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
        history_start_identity(query.history_query())
        return JSONResponse(
            content=service.debug_export(
                query.history_query(), include_volatile_motion_trace=False
            )["normalized_trace"],
            headers={
                "Content-Disposition": 'attachment; filename="manyforge-trace.json"'
            },
        )

    @app.get("/api/execution/program-runs/{program_run_id}/admitted-speeds")
    def program_run_admitted_speeds(
        program_run_id: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
        application_start_id: Annotated[str | None, Query(max_length=16)] = None,
    ) -> JSONResponse:
        read_start_identity(
            application_start_id,
            HeadlessApiError("program_run_not_found", "Program run is not known"),
        )
        return JSONResponse(
            content=shown(service.program_run_admitted_speeds(program_run_id))
        )

    @app.get("/api/execution/history/health")
    def historian_health() -> dict[str, Any]:
        return service.core.historian_health()

    @app.get("/api/execution/history/{sequence}")
    def history_detail(
        sequence: Annotated[int, Path(ge=1, le=_MAXIMUM_SAFE_INTEGER)],
    ) -> dict[str, Any]:
        return service.history_detail(sequence)

    @app.post("/api/execution/history/apply-outcomes")
    def history_apply_outcome(body: ApplyOutcomeRequest) -> dict[str, Any]:
        """One Scenario Load/New, Deployment or Program Apply/Revert terminal
        (outcome, typed error, component rows), recorded as `apply_finished`."""
        return service.record_apply_outcome(body)

    @app.post("/api/execution/history/program-reset-outcomes")
    def history_program_reset_outcome(
        body: ProgramResetOutcomeRequest,
    ) -> dict[str, Any]:
        """One Program Reset terminal (outcome, typed error, options, run,
        Applied Program and reloaded scene revision), recorded by the Program
        Runner as `program_reset_finished` (OD-305)."""
        return service.record_program_reset_outcome(body)

    return app


__all__ = ["TrustedPrincipal", "TrustedPrincipalAdapter", "create_app"]
