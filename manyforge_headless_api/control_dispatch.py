"""Single transport-neutral execution-control semantic adapter.

This module translates the generated UI6-F envelope into the existing strict
owner requests. It contains no lifecycle state machine: Program-scoped
eligibility comes from Program Runner's current ``allowed_commands``
projection, standalone operation eligibility remains with the native owner,
and every effect is decided by one of those semantic owners.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from pydantic import ValidationError

from .control_protocol import (
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
    is_stop_command,
)
from .models import (
    CycleRecoveryActionRequest,
    FullStopIntentRequest,
    ManualResultStageRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramCycleResetRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramSelectionStopRequest,
    ProgramSelectionResumeRequest,
    RuntimeScaleControlRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
)
from .native import HeadlessApiError
from .service import HeadlessService, TrustedAttribution


_NATIVE_EXECUTION_COMMANDS = frozenset(
    {
        "operation_start",
        "operation_stop",
        "operation_pause",
        "operation_resume",
        "operation_reset",
        "operation_acknowledge_completed",
        "pick_sequence_start",
        "place_sequence_start",
        "cell_full_stop",
        "stop_recovery",
        "runtime_scale",
    }
)
_EXPECTED_EXECUTION_OWNER_FIELD = "expected_execution_owner_epoch"
_STANDALONE_EXECUTION_CONTROL_REVISION = 1
_MISSING = object()


def requires_execution_owner_fence(command_kind: str) -> bool:
    """Return whether a command targets the independently owned native runtime."""

    return command_kind in _NATIVE_EXECUTION_COMMANDS


class HeadlessExecutionControlDispatch:
    """Map all public control transports onto one strict owner call table."""

    def __init__(self, service: HeadlessService) -> None:
        self._service = service

    def _uses_standalone_execution_owner(self) -> bool:
        """Return whether this Headless instance intentionally has no runner."""

        return getattr(self._service, "program_runner", _MISSING) is None

    def _standalone_execution_projection(self) -> tuple[str, int, dict[str, Any]]:
        response = self._service.execution_identity()
        identity = response.get("identity") if isinstance(response, Mapping) else None
        owner = identity.get("owner_epoch") if isinstance(identity, Mapping) else None
        if not isinstance(owner, str) or not owner:
            raise HeadlessApiError(
                "execution_control_projection_invalid",
                "native execution owner omitted its current identity",
            )
        # The native owner epoch changes on replacement. Operation, Stop, and
        # scale payloads carry their own finer-grained revisions, so a second
        # synthetic state machine or aggregate revision is neither needed nor
        # useful for standalone Headless operation control.
        return owner, _STANDALONE_EXECUTION_CONTROL_REVISION, {
            "execution_owner_epoch": owner,
        }

    def owner_projection(self) -> tuple[str, int]:
        if self._uses_standalone_execution_owner():
            owner, revision, _identity = self._standalone_execution_projection()
            return owner, revision
        cycle = self._service.program_cycle_status()
        owner = cycle.get("program_runner_owner_epoch")
        revision = cycle.get("execution_control_revision")
        if (
            not isinstance(owner, str)
            or not owner
            or type(revision) is not int
            or revision < 1
        ):
            raise HeadlessApiError(
                "execution_control_projection_invalid",
                "Program owner omitted its execution-control identity",
            )
        return owner, revision

    @staticmethod
    def _caller_context(
        request: ExecutionCommandRequestDTO,
        *,
        include_session: bool,
    ) -> dict[str, Any]:
        envelope = request.command_envelope
        payload_request_id = request.payload.get("request_id")
        nested_context = request.payload.get("request_context")
        if not isinstance(payload_request_id, str) and isinstance(
            nested_context, Mapping
        ):
            payload_request_id = nested_context.get("request_id")
        if not isinstance(payload_request_id, str):
            payload_request_id = envelope.correlation_id
        context = {
            # Correlation groups related activity; it is not the command's
            # replay/idempotency identity. Preserve the command-specific
            # request ID already validated by its strict payload model.
            "request_id": payload_request_id,
            "request_owner_epoch": (
                envelope.control_session_id
                or envelope.freshness.receiver_incarnation
            ),
            "source_label": envelope.source_label,
            "correlation_id": envelope.correlation_id,
        }
        if include_session:
            context.update(
                session_id=envelope.control_session_id,
                client_sequence=envelope.client_sequence,
            )
        return context

    @staticmethod
    def _pick_place_payload(
        payload: Mapping[str, Any], context: Mapping[str, Any]
    ) -> dict[str, Any]:
        """Apply the one admitted envelope identity to every sequence phase."""

        result = dict(payload)
        for field in (
            "approach_operation",
            "descend_operation",
            "tool_operation",
            "retreat_operation",
        ):
            operation = result.get(field)
            if not isinstance(operation, Mapping):
                continue
            retained_context = operation.get("request_context")
            phase_request_id = (
                retained_context.get("request_id")
                if isinstance(retained_context, Mapping)
                else None
            )
            phase_context = dict(context)
            if isinstance(phase_request_id, str):
                phase_context["request_id"] = phase_request_id
            result[field] = {**operation, "request_context": phase_context}
        return result

    @staticmethod
    def _owner_result(
        cycle: Mapping[str, Any],
        *,
        disposition: str,
        code: str,
    ) -> ExecutionIngressResult:
        owner = cycle.get("program_runner_owner_epoch")
        revision = cycle.get("execution_control_revision")
        if not isinstance(owner, str) or type(revision) is not int:
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
            semantic_owner_epoch=owner,
            execution_control_revision=revision,
            current_identity=current_identity,
        )

    @staticmethod
    def _addressed_result(
        request: ExecutionCommandRequestDTO,
        *,
        disposition: str,
        code: str,
        current_identity: Mapping[str, Any] | None = None,
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
            semantic_owner_epoch=envelope.expected_semantic_owner_epoch,
            execution_control_revision=(
                envelope.expected_execution_control_revision
            ),
            current_identity=current_identity,
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
            disposition, code = "joined", "program_reset_already_current"
        elif is_stop_command(kind) and state == "stopping":
            disposition, code = "joined", "program_stop_already_current"
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
        standalone_execution = self._uses_standalone_execution_owner()
        if standalone_execution:
            if envelope.command_kind not in _NATIVE_EXECUTION_COMMANDS:
                # Preserve the stable unavailable result for Program-owned
                # commands; only independently owned native controls can run
                # in an intentionally runner-less Headless process.
                raise HeadlessApiError(
                    "program_runner_unavailable",
                    "independent Program runner is not configured",
                )
            cycle_before = None
        elif is_stop_command(envelope.command_kind):
            # Recognized Stop must not wait for a Program projection read.  Its
            # semantic owner validates the exact target and current state, then
            # the state stream publishes the authoritative outcome.
            cycle_before = None
        else:
            cycle_before = self._service.program_cycle_status()
            allowed, reason = self._availability(
                cycle_before, envelope.command_kind
            )
            if not allowed:
                return self._reduce_unavailable(request, cycle_before, reason)

        context = self._caller_context(
            request,
            include_session=(
                envelope.command_kind.startswith("program_")
                or envelope.command_kind == "execution_recovery"
            ),
        )
        payload = dict(request.payload)
        if requires_execution_owner_fence(envelope.command_kind):
            execution_owner = payload.pop(_EXPECTED_EXECUTION_OWNER_FIELD, None)
            if (
                not isinstance(execution_owner, str)
                or not execution_owner
                or len(execution_owner.encode("utf-8")) > 256
            ):
                raise HeadlessApiError(
                    "execution_control_payload_invalid",
                    "native execution command omitted its current owner fence",
                )
            # This is a target-owner fence copied from the backend projection,
            # not caller authority.  The control session remains the caller
            # identity and the native owner rejects the command if this exact
            # epoch has already been replaced.
            context["request_owner_epoch"] = execution_owner
        attribution = TrustedAttribution(
            principal_id=stamp.trusted_principal_id,
            assurance=stamp.assurance or "unverified",
        )
        try:
            kind = envelope.command_kind
            if kind == "program_run":
                result = self._service.program_run_start(
                    ProgramRunStartRequest.model_validate({**payload, **context})
                )
            elif kind in {
                "program_motion_stop",
                "program_full_stop",
                "program_stop",
            }:
                request_type = ProgramSelectionStopRequest if kind == "program_motion_stop" else ProgramRunStopRequest
                model = request_type.model_validate({**payload, **context})
                if kind == "program_motion_stop":
                    result = self._service.program_run_motion_stop(
                        model.program_run_id, model
                    )
                elif kind == "program_full_stop":
                    result = self._service.program_run_full_stop(
                        model.program_run_id, model
                    )
                else:
                    result = self._service.program_run_stop(model.program_run_id, model)
            elif kind == "program_motion_resume":
                model = ProgramSelectionResumeRequest.model_validate({**payload, **context})
                result = self._service.program_run_motion_resume(model.program_run_id, model)
            elif kind == "program_reset":
                result = self._service.program_cycle_reset(
                    ProgramCycleResetRequest.model_validate({**payload, **context})
                )
            elif kind == "program_resume_full_stop":
                model = ProgramFullStopResumeRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_resume_full_stop(
                    model.program_run_id, model
                )
            elif kind == "program_discard_full_stop":
                model = ProgramFullStopResetRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_run_reset_full_stop(
                    model.program_run_id, model
                )
            elif kind == "program_runtime_override_stage":
                model = RuntimeOverrideStageRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "runtime-overrides", model
                )
            elif kind in {
                "program_runtime_override_activate",
                "program_runtime_override_clear",
            }:
                model = RuntimeOverrideActionRequest.model_validate(
                    {**payload, **context}
                )
                suffix = (
                    "runtime-overrides/activate"
                    if kind == "program_runtime_override_activate"
                    else "runtime-overrides/clear"
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, suffix, model
                )
            elif kind == "program_runtime_override_promote":
                model = RuntimeOverridePromotionRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "runtime-overrides/promote", model
                )
            elif kind == "program_runtime_blackboard_write":
                model = RuntimeBlackboardWriteRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "blackboard-values", model
                )
            elif kind == "program_manual_result_stage":
                model = ManualResultStageRequest.model_validate(
                    {**payload, **context}
                )
                result = self._service.program_runtime_control_action(
                    model.program_run_id, "manual-result", model
                )
            elif kind == "operation_start":
                result = self._service.execution_submit(
                    OperationIntentRequest.model_validate(
                        {**payload, "request_context": context}
                    ),
                    attribution,
                )
            elif kind in {
                "operation_pause",
                "operation_resume",
                "operation_reset",
                "operation_acknowledge_completed",
            }:
                model = OperationControlRequest.model_validate(
                    {**payload, "request_context": context}
                )
                if kind == "operation_pause":
                    result = self._service.execution_pause(model, attribution)
                elif kind == "operation_resume":
                    result = self._service.execution_resume(model, attribution)
                elif kind == "operation_reset":
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
            elif kind == "pick_sequence_start":
                result = self._service.execution_start_pick(
                    PickSequencePlanningRequest.model_validate(
                        self._pick_place_payload(payload, context)
                    ),
                    attribution,
                )
            elif kind == "place_sequence_start":
                result = self._service.execution_start_place(
                    PlaceSequencePlanningRequest.model_validate(
                        self._pick_place_payload(payload, context)
                    ),
                    attribution,
                )
            elif kind == "cell_full_stop":
                result = self._service.execution_full_stop(
                    FullStopIntentRequest.model_validate(
                        {**payload, "request_context": context}
                    ),
                    attribution,
                )
            elif kind == "execution_recovery":
                result = self._service.cycle_recovery_action(
                    CycleRecoveryActionRequest.model_validate({**payload, **context})
                )
            elif kind == "stop_recovery":
                result = self._service.execution_recover_stop_fault(
                    StopFaultRecoveryIntentRequest.model_validate(
                        {**payload, "request_context": context}
                    )
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
            raise HeadlessApiError(
                "execution_control_payload_invalid",
                "command payload does not match its declared command kind",
                details={"validation": str(error)},
            ) from error

        if standalone_execution:
            # Ordinary native commands were fenced against this projection by
            # the ingress immediately before dispatch. Recognized Stop skips
            # that read deliberately and the native owner resolves stale
            # targets conservatively. Do not add a post-command identity read:
            # it could place a Stop response behind unrelated native work.
            owner = envelope.expected_semantic_owner_epoch
            revision = envelope.expected_execution_control_revision
            current_identity = {"execution_owner_epoch": owner}

            def owner_result(disposition: str, code: str) -> ExecutionIngressResult:
                return self._addressed_result(
                    request,
                    disposition=disposition,
                    code=code,
                    current_identity=current_identity,
                )

        else:

            def owner_result(disposition: str, code: str) -> ExecutionIngressResult:
                return self._addressed_result(
                    request,
                    disposition=disposition,
                    code=code,
                )
        if isinstance(result, Mapping) and result.get("ok") is False:
            error = result.get("error")
            code = error.get("code") if isinstance(error, Mapping) else None
            return owner_result(
                "rejected",
                (
                    code
                    if isinstance(code, str) and code
                    else "execution_command_rejected"
                ),
            ), result
        return owner_result(
            "received",
            f"{envelope.command_kind}_received",
        ), result


__all__ = [
    "HeadlessExecutionControlDispatch",
    "requires_execution_owner_fence",
]
