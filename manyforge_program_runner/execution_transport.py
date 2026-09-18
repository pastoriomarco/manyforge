"""Typed same-host execution transport for the Program owner."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Annotated, Any, TypeVar

from pydantic import BaseModel, Field, ValidationError, field_validator

from manyforge_behavior.orchestration_ports import ProgramSelectionReleaseRejectedError
from manyforge_headless_api.execution import (
    EXECUTION_COMMAND_VERSION,
    ExecutionCommandClient,
)
from manyforge_headless_api.models import (
    MAX_OPERATION_PARTICIPANTS,
    StrictRequest,
    OperationControlRequest,
    OperationRuntimeScaleRequest,
    OperationIntentRequest,
    OperationParticipantRequest,
    OperationRecoveryActionRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    ProgramOperationQuery,
    FullStopIntentRequest,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError


_Request = TypeVar("_Request", bound=BaseModel)


class _ProgramNodeExecutionScope(StrictRequest):
    """Private Runner attribution, never accepted from a public operation DTO."""

    node_instance_id: str
    node_attempt_id: str

    @field_validator("node_instance_id", "node_attempt_id")
    @classmethod
    def validate_scope_identity(cls, value: str, info) -> str:
        limit = 128 if info.field_name == "node_instance_id" else 512
        if not value or "\0" in value or len(value.encode("utf-8")) > limit:
            raise ValueError("Program execution scope identity exceeds its bounds")
        return value


class _ProgramParticipant(OperationParticipantRequest):
    execution_scope: _ProgramNodeExecutionScope


class _ProgramControl(OperationControlRequest):
    execution_scope: _ProgramNodeExecutionScope


class _ProgramOperation(OperationIntentRequest):
    participants: Annotated[
        list[_ProgramParticipant],
        Field(min_length=1, max_length=MAX_OPERATION_PARTICIPANTS),
    ]


class _ProgramPick(PickSequencePlanningRequest):
    approach_operation: _ProgramOperation
    descend_operation: _ProgramOperation
    tool_operation: _ProgramOperation
    retreat_operation: _ProgramOperation


class _ProgramPlace(PlaceSequencePlanningRequest):
    approach_operation: _ProgramOperation
    descend_operation: _ProgramOperation
    tool_operation: _ProgramOperation
    retreat_operation: _ProgramOperation


class ProgramRunnerExecutionTransport(ExecutionCommandClient):
    """Validate Program mappings over the existing private owner client."""

    def __init__(self, run_root: Path, *, timeout_seconds: float) -> None:
        super().__init__(
            run_root,
            timeout_seconds=timeout_seconds,
            attribution_transport="internal",
        )

    @staticmethod
    def _request(model: type[_Request], body: Mapping[str, Any]) -> _Request:
        try:
            return model.model_validate(body)
        except ValidationError as error:
            raise HeadlessApiError(
                "execution_command_invalid",
                "Program Runner execution request is invalid",
            ) from error

    def _exchange_on(
        self, command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        response = super()._exchange_on(command, socket_path)
        try:
            return self.require_success(response)
        except HeadlessApiError as error:
            if command.get("command") in {
                "start_program_selection",
                "release_program_selection",
            } and error.details.get("reason") in {
                "program_selection_stale",
                "program_selection_unknown",
                "program_selection_revision_stale",
                "program_selection_not_ready",
                "program_selection_not_releasable",
                "program_selection_recovery_required",
                "program_children_unsettled",
                "program_run_admission_in_flight",
            }:
                # These closed Kernel rejections all precede release. A lost,
                # malformed or exceptional reply remains outcome-unknown.
                raise ProgramSelectionReleaseRejectedError(str(error)) from error
            raise

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().submit(
            self._request(_ProgramOperation, body),
            trusted_principal_id=None,
        )

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Send only identities; the native product boundary supplies the table.

        This is an internal Run-owner call, not an external REST operation or
        a preparation side effect. Native decoding and Kernel admission remain
        responsible for the closed shape and the exact current claim.
        """

        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "register_program_run_capture",
                "capture": dict(body),
            }
        )
        if (
            response.get("command") != "register_program_run_capture"
            or response.get("program_run_id") != body.get("program_run_id")
            or type(response.get("claim_generation")) is not int
            or response["claim_generation"] != body.get("claim_generation")
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Program capture response does not identify the registered claim",
            )
        return response

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().query_target(self._request(TargetObservationQuery, body))

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read_program_execution_scope",
                "program_run_id": program_run_id,
            }
        )

    def read_manual_handover(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().read_manual_handover(
            self._request(OperationIntentRequest, body), trusted_principal_id=None
        )

    def stop_program_selection(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "stop_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
            }
        )

    def release_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "release_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
                "expected_selection_revision": expected_selection_revision,
            }
        )

    def start_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]:
        return self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "start_program_selection",
                "attribution": self._request_attribution(None),
                "request": dict(body),
                "expected_selection_revision": expected_selection_revision,
            }
        )

    def query_operation(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().query_operation(self._request(ProgramOperationQuery, body))

    def full_stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().full_stop(
            self._request(FullStopIntentRequest, body), trusted_principal_id=None
        )

    def set_operation_runtime_scale(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().set_operation_runtime_scale(
            self._request(OperationRuntimeScaleRequest, body), trusted_principal_id=None
        )

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return super().apply_operation_recovery(
            operation_id,
            expected_operation_revision,
            self._request(OperationRecoveryActionRequest, body),
        )

    def _mapped_control(
        self,
        method: str,
        operation_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        request = self._request(
            _ProgramControl if "execution_scope" in body else OperationControlRequest,
            body,
        )
        if request.operation_id != operation_id:
            raise HeadlessApiError(
                "execution_operation_path_invalid",
                "operation identity and request body must match",
            )
        return getattr(super(), method)(request, trusted_principal_id=None)

    def pause(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._mapped_control("pause", operation_id, body)

    def resume(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._mapped_control("resume", operation_id, body)

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._mapped_control("reset", operation_id, body)

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._mapped_control("acknowledge_completed", operation_id, body)

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().stop(
            self._request(StopIntentRequest, body),
            trusted_principal_id=None,
        )

    def stop_program_run(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().stop_program_run(
            self._request(ProgramRunStopIntentRequest, body)
        )

    def recover_stop_fault(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().recover_stop_fault(
            self._request(StopFaultRecoveryIntentRequest, body)
        )

    def _pick_place(
        self,
        method: str,
        model: type[PickSequencePlanningRequest] | type[PlaceSequencePlanningRequest],
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return getattr(super(), method)(
            self._request(model, body),
            trusted_principal_id=None,
        )

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("plan_pick", _ProgramPick, body)

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("plan_place", _ProgramPlace, body)

__all__ = ["ProgramRunnerExecutionTransport"]
