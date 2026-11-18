"""Single transport-neutral execution-control semantic adapter.

This module translates the generated control envelope into the existing strict
owner requests. It contains no lifecycle state machine: Program-scoped
eligibility comes from Program Runner's current ``allowed_commands``
projection, standalone operation eligibility remains with the native owner,
and every effect is decided by one of those semantic owners.
"""

from __future__ import annotations

from dataclasses import replace

from collections.abc import Mapping
from typing import Any

from pydantic import ValidationError


from .control_protocol import (
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
    is_stop_command,
)
from .identity import (
    RequestAdmission,
    RequestKeyInvalid,
    RequestKeyRequired,
    StartIdentityMismatch,
    format_request_key,
    websocket_request_key,
)
from .models import (
    CycleRecoveryActionRequest,
    FullStopIntentRequest,
    ManualResultStageRequest,
    OperationControlRequest,
    OperationResetRequest,
    OperationIntentRequest,
    ProgramCycleResetRequest,
    ProgramHaltDiscardRequest,
    ProgramHaltResumeRequest,
    ProgramRunStartRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionStartRequest,
    ProgramSelectionResumeRequest,
    ProgramManualHandoverRequest,
    RuntimeScaleControlRequest,
    RuntimeBlackboardWriteRequest,
    StopIntentRequest,
)
from .native import HeadlessApiError
from .service import HeadlessService, TrustedAttribution


# Session-less (HTTP) commands that name no value minted inside one start,
# so they carry no ``A`` (contract 1.6; BS 1.3 route list).
_KINDS_WITHOUT_START_VALUE = frozenset({"program_run", "stop_all", "halt"})
# Kinds whose payload nests its request context (the Kernel DTO shape).
_NESTED_CONTEXT_KINDS = frozenset(
    {
        "operation_start",
        "operation_stop",
        "operation_reset",
        "operation_acknowledge_completed",
        "stop_all",
        "halt",
        "runtime_scale",
    }
)
_MANUAL_OPERATION_CONTROLS = frozenset(
    {
        "operation_reset",
        "operation_acknowledge_completed",
    }
)


class HeadlessExecutionControlDispatch:
    """Map all public control transports onto one strict owner call table."""

    def __init__(self, service: HeadlessService, admission: RequestAdmission) -> None:
        self._service = service
        self._admission = admission

    def _manual_scope_conflict(
        self, request: ExecutionCommandRequestDTO, cycle: Mapping[str, Any]
    ) -> str | None:
        """Select native manual authority, never infer rest or lifecycle permission."""
        kind = request.command_envelope.command_kind
        if cycle.get("state") == "idle_after_reset":
            return None
        if kind == "operation_start":
            # The strict intent model and native product resolver validate the
            # context and intrinsic direct-control binding before any effect.
            return (
                None
                if isinstance(request.payload.get("manual_handover"), Mapping)
                else "manual_handover_required"
            )
        if kind not in _MANUAL_OPERATION_CONTROLS:
            return None
        projection = self._service.execution_control_state()
        state = projection.get("control_state", {})
        if (
            not isinstance(state, Mapping)
            # One component-based version, no legacy fallback.
            or state.get("schema_version") != "manyforge.execution_control_state.v8"
            or request.payload.get("execution_scope") is not None
        ):
            return "manual_operation_scope_stale"
        operations = state.get("manual_operations")
        if not isinstance(operations, list):
            return "manual_operation_scope_stale"
        matches = [
            row
            for row in operations
            if isinstance(row, Mapping)
            and row.get("resident") is True
            and isinstance(row.get("operation"), Mapping)
            and row["operation"].get("operation_id")
            == request.payload.get("operation_id")
        ]
        # The projection is native-owned. Like Stop, the control resolves to
        # the operation's current revision; a concurrent retirement still
        # rejects in the command's Kernel check.
        if len(matches) == 1:
            return None
        return "manual_operation_scope_stale"

    def control_revision(self) -> int:
        cycle = self._service.program_cycle_status()
        revision = cycle.get("execution_control_revision")
        if type(revision) is not int or revision < 1:
            raise HeadlessApiError(
                "execution_control_projection_invalid",
                "Program owner omitted its execution-control revision",
            )
        return revision

    def _caller_context(
        self,
        request: ExecutionCommandRequestDTO,
    ) -> dict[str, Any]:
        """Form the request context: the key is formed here, once.

        A WebSocket command's key is class 0 from its session and client
        sequence. A session-less (HTTP) command carries the client's 32-hex
        key in its payload (top level, or in its nested request context); a
        Stop without a valid one gets a fresh Headless key.
        """

        envelope = request.command_envelope
        if envelope.control_session_number is not None:
            assert envelope.client_sequence is not None
            key = format_request_key(
                websocket_request_key(
                    envelope.control_session_number, int(envelope.client_sequence)
                )
            )
        else:
            raw = request.payload.get("request_key")
            nested = request.payload.get("request_context")
            if raw is None and isinstance(nested, Mapping):
                raw = nested.get("request_key")
            stop = is_stop_command(envelope.command_kind)
            try:
                key = self._admission.http_key(raw, stop=stop)
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
        return {
            "request_key": key,
            "source_label": envelope.source_label,
        }

    @staticmethod
    def _owner_result(
        cycle: Mapping[str, Any],
        *,
        disposition: str,
        code: str,
    ) -> ExecutionIngressResult:
        revision = cycle.get("execution_control_revision")
        if type(revision) is not int:
            raise HeadlessApiError(
                "execution_control_projection_invalid",
                "Program owner returned an invalid current projection",
            )
        current_identity = {
            "cycle_generation": cycle.get("cycle_generation"),
            "program_run_id": cycle.get("program_run_id"),
            "state": cycle.get("state"),
        }
        return ExecutionIngressResult(
            disposition=disposition,  # type: ignore[arg-type]
            code=code,
            execution_control_revision=revision,
            current_identity=current_identity,
        )

    @staticmethod
    def _addressed_result(
        request: ExecutionCommandRequestDTO,
        *,
        disposition: str,
        code: str,
    ) -> ExecutionIngressResult:
        """Describe owner disposition without resampling owner state.

        The command envelope already names the exact semantic context admitted
        at this ingress.  Current owner state travels independently on the
        change-driven control stream, so synchronously rereading Program Runner
        after an effect only delays the receipt and can accidentally describe a
        concurrent, unrelated transition.
        """

        envelope = request.command_envelope
        return ExecutionIngressResult(
            disposition=disposition,  # type: ignore[arg-type]
            code=code,
            execution_control_revision=(envelope.expected_execution_control_revision),
            current_identity=None,
        )

    @staticmethod
    def _availability(
        cycle: Mapping[str, Any], command_kind: str
    ) -> tuple[bool, str | None]:
        commands = cycle.get("allowed_commands")
        if not isinstance(commands, list):
            raise HeadlessApiError(
                "execution_control_projection_invalid",
                "Program owner omitted allowed commands",
            )
        for row in commands:
            if isinstance(row, Mapping) and row.get("command_kind") == command_kind:
                allowed = row.get("allowed")
                reason = row.get("disabled_reason")
                if type(allowed) is not bool or (
                    reason is not None and not isinstance(reason, str)
                ):
                    break
                return allowed, reason
        raise HeadlessApiError(
            "execution_control_projection_invalid",
            "Program owner omitted the addressed command availability",
        )

    def _reduce_unavailable(
        self,
        request: ExecutionCommandRequestDTO,
        cycle: Mapping[str, Any],
        reason: str | None,
    ) -> tuple[ExecutionIngressResult, Mapping[str, Any]]:
        kind = request.command_envelope.command_kind
        state = cycle.get("state")
        if kind == "program_reset" and state == "resetting":
            try:
                model = ProgramCycleResetRequest.model_validate({
                    **request.payload, **self._caller_context(request)
                })
            except ValidationError as error:
                raise HeadlessApiError(
                    "execution_control_payload_invalid",
                    "command payload does not match its declared command kind",
                    details={"validation": str(error)},
                ) from error
            selected = {
                "reload_initial_scene": model.reload_initial_scene,
                "reset_robot_position": model.reset_robot_position,
            }
            accepted = cycle.get("reset_options")
            if not isinstance(accepted, dict) or accepted != selected:
                disposition, code = "rejected", "program_run_request_conflict"
            else:
                disposition, code = "joined", "program_reset_already_current"
        elif is_stop_command(kind) and state in {
            "idle_after_reset",
            "resetting",
            "terminal_awaiting_reset",
        }:
            disposition, code = "no_effect", "program_already_contained"
        else:
            disposition, code = "rejected", reason or "command_not_allowed"
        return self._owner_result(
            cycle,
            disposition=disposition,
            code=code,
        ), {"program_cycle": dict(cycle)}

    def __call__(
        self,
        request: ExecutionCommandRequestDTO,
        stamp: ExecutionIngressStamp,
    ) -> tuple[ExecutionIngressResult, Mapping[str, Any] | None]:
        envelope = request.command_envelope
        stop = is_stop_command(envelope.command_kind)
        attribution = TrustedAttribution(
            principal_id=stamp.trusted_principal_id,
            assurance=stamp.assurance or "unverified",
        )
        payload = dict(request.payload)
        # Only a session-less (HTTP) command carries the client's key and
        # ``A``; a WebSocket session belongs to one start (contract 1.6).
        payload.pop("request_key", None)
        start_identity = payload.pop("application_start_id", None)
        if (
            envelope.control_session_number is None
            and envelope.command_kind not in _KINDS_WITHOUT_START_VALUE
        ):
            try:
                self._admission.start_identity(start_identity)
            except StartIdentityMismatch as mismatch:
                if not stop:
                    raise HeadlessApiError(
                        mismatch.code,
                        "the request names a value of another application start",
                    ) from mismatch
                # A Stop is never refused: a Stop of another or an unnamed
                # start acts as Stop all (BS 1.3, B12; PD-1). It goes as a
                # recognized Stop on the operation-Stop route, so the Kernel
                # records why it was widened (`stale_start`).
                context = self._caller_context(request)
                result = self._service.execution_recognized_stop(
                    request_key=context["request_key"],
                    recognized_route="/api/execution/stops",
                    reason="recognized_stop_start_not_current",
                    source_label=context["source_label"],
                    attribution=attribution,
                )
                return self._keyed_result(request, result, context["request_key"])
        # Recognized Stop must not wait for a Program projection read.  Its
        # semantic owner validates the exact target and current state, then
        # the state stream publishes the authoritative outcome.
        if not stop:
            cycle_before = self._service.program_cycle_status(before_command=True)
            allowed, reason = self._availability(cycle_before, envelope.command_kind)
            if not allowed:
                return self._reduce_unavailable(request, cycle_before, reason)
            conflict = self._manual_scope_conflict(request, cycle_before)
            if conflict is not None:
                return self._reduce_unavailable(request, cycle_before, conflict)

        context = self._caller_context(request)
        formed_key = context["request_key"]
        if envelope.command_kind in _NESTED_CONTEXT_KINDS:
            payload.pop("request_context", None)
        try:
            kind = envelope.command_kind
            if kind == "program_run":
                model = ProgramRunStartRequest.model_validate({**payload, **context})
                result = self._service.program_run_start(model)
            elif kind == "program_motion_stop":
                # Kernel-first: the selection Stop goes to the host, never
                # through the Program runner (contract 1.8).
                model = ProgramSelectionStopRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_motion_stop(model, attribution)
            elif kind in {"program_motion_start", "program_motion_resume"}:
                model_type = (
                    ProgramSelectionStartRequest
                    if kind == "program_motion_start"
                    else ProgramSelectionResumeRequest
                )
                model = model_type.model_validate(
                    {**payload, **context}
                )
                result = (
                    self._service.program_run_motion_start(model.program_run_id, model)
                    if kind == "program_motion_start"
                    else self._service.program_run_motion_resume(model.program_run_id, model)
                )
            elif kind == "program_manual_handover":
                model = ProgramManualHandoverRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_manual_handover(
                    model.program_run_id, model
                )
            elif kind == "program_reset":
                result = self._service.program_cycle_reset(
                    ProgramCycleResetRequest.model_validate({**payload, **context})
                )
            elif kind == "program_resume_halt":
                model = ProgramHaltResumeRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_resume_halt(
                    model.program_run_id, model
                )
            elif kind == "program_discard_halt":
                model = ProgramHaltDiscardRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_discard_halt(
                    model.program_run_id, model
                )
            elif kind == "program_runtime_blackboard_write":
                model = RuntimeBlackboardWriteRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "blackboard-values", model
                )
            elif kind == "program_manual_result_stage":
                model = ManualResultStageRequest.model_validate({**payload, **context})
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "manual-result", model
                )
            elif kind == "operation_start":
                model = OperationIntentRequest.model_validate(
                    {**payload, "request_context": context}
                )
                result = self._service.execution_submit(model, attribution)
            elif kind in {
                "operation_reset",
                "operation_acknowledge_completed",
            }:
                model = (
                    OperationResetRequest
                    if kind == "operation_reset"
                    else OperationControlRequest
                ).model_validate({**payload, "request_context": context})
                if kind == "operation_reset":
                    result = self._service.execution_reset(model, attribution)
                else:
                    result = self._service.execution_acknowledge_completed(
                        model, attribution
                    )
            elif kind == "operation_stop":
                result = self._service.execution_stop(
                    StopIntentRequest.model_validate(
                        {**payload, "request_context": context}
                    ),
                    attribution,
                )
            elif kind in {"stop_all", "halt"}:
                model = FullStopIntentRequest.model_validate(
                    {**payload, "request_context": context}
                )
                result = (
                    self._service.execution_stop_all(model, attribution)
                    if kind == "stop_all"
                    else self._service.execution_halt(model, attribution)
                )
            elif kind == "execution_recovery":
                result = self._service.cycle_recovery_action(
                    CycleRecoveryActionRequest.model_validate({**payload, **context})
                )
            elif kind == "runtime_scale":
                result = self._service.execution_set_runtime_scale_control(
                    RuntimeScaleControlRequest.model_validate(
                        {**payload, "request_context": context}
                    ),
                    attribution,
                )
            else:
                raise HeadlessApiError(
                    "execution_control_command_unsupported",
                    f"unsupported execution command kind {kind!r}",
                )
        except ValidationError as error:
            if not (
                is_stop_command(envelope.command_kind)
                and error.title in _STOP_REQUEST_MODELS
            ):
                raise HeadlessApiError(
                    "execution_control_payload_invalid",
                    "command payload does not match its declared command kind",
                    details={"validation": str(error)},
                ) from error
            # A recognized Stop whose payload is invalid is widened by the
            # Kernel from the route it names, as on the Stop HTTP routes
            # (PD-1): Halt stays Halt, every other Stop becomes Stop all.
            formed_key = self._admission.mint_key()
            result = self._service.execution_recognized_stop(
                request_key=formed_key,
                recognized_route=_MALFORMED_STOP_ROUTES[envelope.command_kind],
                reason="recognized_stop_payload_invalid",
                # The envelope's label is validated with the envelope.
                source_label=envelope.source_label,
                attribution=attribution,
            )

        return self._keyed_result(request, result, formed_key)

    def _keyed_result(
        self,
        request: ExecutionCommandRequestDTO,
        result: Any,
        request_key: str,
    ) -> tuple[ExecutionIngressResult, Mapping[str, Any] | None]:
        """The dispatch result; the receipt names the key the command was
        forwarded with, and a runner command's row (OD-214)."""

        ingress, reply = self._dispatch_result(request, result)
        command = None
        if request.command_envelope.command_kind in _RUNNER_COMMAND_KINDS:
            # The runner's `program_command_reply.v1`, validated by the
            # runner client.
            command = result["command"]
        return replace(ingress, request_key=request_key, command=command), reply

    def _dispatch_result(
        self,
        request: ExecutionCommandRequestDTO,
        result: Any,
    ) -> tuple[ExecutionIngressResult, Mapping[str, Any] | None]:
        envelope = request.command_envelope
        if isinstance(result, Mapping) and result.get("ok") is False:
            error = result.get("error")
            code = error.get("code") if isinstance(error, Mapping) else None
            return (
                self._addressed_result(
                    request,
                    disposition="rejected",
                    code=(
                        code
                        if isinstance(code, str) and code
                        else "execution_command_rejected"
                    ),
                ),
                result,
            )
        return (
            self._addressed_result(
                request,
                disposition="received",
                code=f"{envelope.command_kind}_received",
            ),
            result,
        )


# The command kinds Headless forwards to the Program runner: their reply is
# the runner's `program_command_reply.v1`, answered at acceptance (OD-214).
_RUNNER_COMMAND_KINDS = frozenset(
    {
        "program_run",
        "program_motion_start",
        "program_motion_resume",
        "program_manual_handover",
        "program_reset",
        "program_resume_halt",
        "program_discard_halt",
        "program_runtime_blackboard_write",
        "program_manual_result_stage",
        "execution_recovery",
    }
)
# The payload models of the Stop command kinds. Only their validation
# failure promotes; any other model's failure keeps its refusal.
_STOP_REQUEST_MODELS = frozenset(
    model.__name__
    for model in (
        StopIntentRequest,
        FullStopIntentRequest,
        ProgramSelectionStopRequest,
    )
)
# The Stop route each Stop kind names when its payload is invalid (PD-1).
_MALFORMED_STOP_ROUTES = {
    "program_motion_stop": "/api/execution/stops",
    "operation_stop": "/api/execution/stops",
    "stop_all": "/api/execution/stop-all",
    "halt": "/api/execution/halt",
}


__all__ = [
    "HeadlessExecutionControlDispatch",
]
