"""Typed same-host execution transport for the Program owner."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any, TypeVar

from pydantic import BaseModel, ValidationError

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    OperationControlRequest,
    OperationRuntimeScaleRequest,
    OperationIntentRequest,
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
        return self.require_success(super()._exchange_on(command, socket_path))

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().submit(
            self._request(OperationIntentRequest, body),
            trusted_principal_id=None,
        )

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return super().query_target(self._request(TargetObservationQuery, body))

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
        request = self._request(OperationControlRequest, body)
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
        return self._pick_place("plan_pick", PickSequencePlanningRequest, body)

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("plan_place", PlaceSequencePlanningRequest, body)

    def start_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("start_pick", PickSequencePlanningRequest, body)

    def start_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._pick_place("start_place", PlaceSequencePlanningRequest, body)


__all__ = ["ProgramRunnerExecutionTransport"]
