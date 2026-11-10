"""Single transport-neutral execution-control semantic adapter.

This module translates the generated control envelope into the existing strict
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
    OperationResetRequest,
    OperationIntentRequest,
    ProgramCycleResetRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
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


_EXECUTION_HOST_COMMANDS = frozenset(
    {
        "operation_start",
        "operation_stop",
        "operation_reset",
        "operation_acknowledge_completed",
        "cell_full_stop",
        "runtime_scale",
    }
)
_EXPECTED_EXECUTION_OWNER_FIELD = "expected_execution_owner_epoch"
_MANUAL_OPERATION_CONTROLS = frozenset(
    {
        "operation_reset",
        "operation_acknowledge_completed",
    }
)


def requires_execution_owner_fence(command_kind: str) -> bool:
    """Return whether a command targets the independently owned execution host."""

    return command_kind in _EXECUTION_HOST_COMMANDS


class HeadlessExecutionControlDispatch:
    """Map all public control transports onto one strict owner call table."""

    def __init__(self, service: HeadlessService) -> None:
        self._service = service

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
        identity = projection.get("identity", {})
        state = projection.get("control_state", {})
        if (
            not isinstance(identity, Mapping)
            or identity.get("owner_epoch")
            != request.payload.get(_EXPECTED_EXECUTION_OWNER_FIELD)
            or not isinstance(state, Mapping)
            # One component-based version, no legacy fallback.
            or state.get("schema_version") != "manyforge.execution_control_state.v7"
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

    def owner_projection(self) -> tuple[str, int]:
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
                envelope.control_session_id or envelope.freshness.receiver_incarnation
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
                    **request.payload, **self._caller_context(request, include_session=True)
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
        # Recognized Stop must not wait for a Program projection read.  Its
        # semantic owner validates the exact target and current state, then
        # the state stream publishes the authoritative outcome.
        if not is_stop_command(envelope.command_kind):
            cycle_before = self._service.program_cycle_status()
            allowed, reason = self._availability(cycle_before, envelope.command_kind)
            if not allowed:
                return self._reduce_unavailable(request, cycle_before, reason)
            conflict = self._manual_scope_conflict(request, cycle_before)
            if conflict is not None:
                return self._reduce_unavailable(request, cycle_before, conflict)

        context = self._caller_context(
            request,
            include_session=(
                envelope.command_kind.startswith("program_")
                or envelope.command_kind == "execution_recovery"
            ),
        )
        origin_epoch = context["request_owner_epoch"]
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
                model = ProgramRunStartRequest.model_validate({**payload, **context})
                result = self._service.program_run_start(model)
            elif kind in {
                "program_motion_stop",
                "program_full_stop",
                "program_stop",
            }:
                request_type = (
                    ProgramSelectionStopRequest
                    if kind == "program_motion_stop"
                    else ProgramRunStopRequest
                )
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

        def owner_result(disposition: str, code: str) -> ExecutionIngressResult:
            return self._addressed_result(
                request,
                disposition=disposition,
                code=code,
            )

        if isinstance(result, Mapping) and result.get("ok") is False:
            error = result.get("error")
            code = error.get("code") if isinstance(error, Mapping) else None
            return (
                owner_result(
                    "rejected",
                    (
                        code
                        if isinstance(code, str) and code
                        else "execution_command_rejected"
                    ),
                ),
                result,
            )
        return (
            owner_result(
                "received",
                f"{envelope.command_kind}_received",
            ),
            result,
        )


__all__ = [
    "HeadlessExecutionControlDispatch",
    "requires_execution_owner_fence",
]
