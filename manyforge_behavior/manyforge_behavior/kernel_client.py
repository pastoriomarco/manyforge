"""Thin Behavior client for the resource-addressed native execution owner."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Mapping, Protocol


class ExecutionTransport(Protocol):
    """Bounded operation-addressed calls to the execution owner."""

    def identity(self) -> dict[str, Any]: ...

    def targets(self) -> dict[str, Any]: ...

    def control_state(self) -> dict[str, Any]: ...

    def observation(self) -> dict[str, Any]: ...

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def query_operation(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def full_stop(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def set_operation_runtime_scale(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def register_program_run_capture(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]: ...

    def read_manual_handover(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_program_selection(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def release_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]: ...

    def start_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]: ...

    def check_program_node_motion_attention(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def read(self, operation_id: str) -> dict[str, Any]: ...

    def operation_recovery(self, operation_id: str) -> dict[str, Any]: ...

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_program_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def release_program_run_stop_fence(
        self, program_run_id: str, stop_id: str, stop_generation: int
    ) -> dict[str, Any]: ...

    def stop_status(self, stop_id: str) -> dict[str, Any]: ...

    def stop_fault(self) -> dict[str, Any]: ...

    def recover_stop_fault(self, body: Mapping[str, Any]) -> dict[str, Any]: ...


class KernelClientResponseError(RuntimeError):
    """Raised when a transport returns a successful but malformed projection."""


class KernelOperationAdmissionRejectedError(RuntimeError):
    """The execution owner explicitly rejected an operation before acceptance."""

    def __init__(
        self,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})


class KernelOperationObservationUnavailableError(RuntimeError):
    """An exact operation read could not reach the native owner temporarily."""


class KernelOperationSettlementPendingError(RuntimeError):
    """A terminal operation still has one accepted subordinate effect in flight."""


class LegacyKernelMockUnavailableError(RuntimeError):
    """The removed stateful Python mock cannot become a second Kernel owner."""


class ExecutionState(Enum):
    """Closed vocabulary projected by the C++ addressed execution owner."""

    QUEUED = "queued"
    RESOLVING = "resolving"
    PLANNING = "planning"
    VALIDATING = "validating"
    PREPARED = "prepared"
    DISPATCHING = "dispatching"
    EXECUTING = "executing"
    STOPPING = "stopping"
    COMPLETED = "completed"
    STOPPED = "stopped"
    FAILED = "failed"
    INTERRUPTED = "interrupted"
    INTERVENTION_REQUIRED = "intervention_required"
    CANCELLED_PRE_DISPATCH = "cancelled_pre_dispatch"


_ACTION_SOURCES = frozenset(
    {"program_node", "direct_control", "robot_control", "mcp", "system"}
)


@dataclass(frozen=True, slots=True)
class ActionSpan:
    """Transport-neutral behavior-owned action correlation."""

    action_source: str = "system"
    action_span_id: str = ""
    program_run_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None
    parent_node_attempt_id: str | None = None

    def __post_init__(self) -> None:
        if self.action_source not in _ACTION_SOURCES:
            raise ValueError("action_source is unsupported")
        if not self.action_span_id or len(self.action_span_id.encode("utf-8")) > 256:
            raise ValueError("action_span_id is invalid")
        if self.action_source == "program_node":
            if not self.node_instance_id or not self.node_attempt_id:
                raise ValueError(
                    "program_node spans require node_instance_id and node_attempt_id"
                )
            if self.action_span_id != self.node_attempt_id:
                raise ValueError(
                    "program_node action_span_id must equal node_attempt_id"
                )

    def as_request(self) -> dict[str, str | None]:
        return {
            "action_source": self.action_source,
            "action_span_id": self.action_span_id,
            "program_run_id": self.program_run_id,
            "node_instance_id": self.node_instance_id,
            "node_attempt_id": self.node_attempt_id,
            "parent_node_attempt_id": self.parent_node_attempt_id,
        }


class KernelClient:
    """Stateless adapter; the C++ host remains the only execution owner."""

    def __init__(self, transport: ExecutionTransport) -> None:
        for method in (
            "identity",
            "submit",
            "read",
            "reset",
            "acknowledge_completed",
            "stop",
        ):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"execution transport has no callable {method}")
        self._transport = transport

    @staticmethod
    def _successful(value: object, *, label: str) -> dict[str, Any]:
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise KernelClientResponseError(
                f"execution owner {label} did not return a successful envelope"
            )
        return value

    def identity(self) -> dict[str, Any]:
        return self._successful(self._transport.identity(), label="identity")

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Install a claim-bound capture through the private Program transport."""

        return self._successful(
            self._transport.register_program_run_capture(body),
            label="Program execution capture",
        )

    def targets(self) -> dict[str, Any]:
        return self._projection("targets")

    def control_state(self) -> dict[str, Any]:
        return self._projection("control_state")

    def observation(self) -> dict[str, Any]:
        return self._projection("observation")

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.query_target(body), label="target observation"
        )

    def _projection(self, method_name: str) -> dict[str, Any]:
        method = getattr(self._transport, method_name, None)
        if not callable(method):
            raise TypeError(f"execution transport has no callable {method_name}")
        return self._successful(method(), label=method_name.replace("_", " "))

    def execute(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        try:
            response = self._transport.submit(intent)
        except Exception as exc:
            code = getattr(exc, "code", None)
            # Transport loss and malformed or mismatched replies cannot prove
            # whether the owner accepted the request. All other typed owner
            # errors are explicit pre-acceptance rejections.
            if isinstance(code, str) and code not in {
                "execution_owner_unavailable",
                "execution_command_transport_failure",
                "execution_command_response_invalid",
                "execution_command_response_identity_mismatch",
            }:
                details = getattr(exc, "details", None)
                raise KernelOperationAdmissionRejectedError(
                    code,
                    str(exc) or "execution owner rejected the operation",
                    details if isinstance(details, Mapping) else None,
                ) from exc
            raise
        if isinstance(response, dict) and response.get("ok") is False:
            error = response.get("error")
            if (
                isinstance(error, Mapping)
                and isinstance(error.get("code"), str)
                and error["code"]
                and isinstance(error.get("message"), str)
                and isinstance(error.get("details", {}), Mapping)
            ):
                raise KernelOperationAdmissionRejectedError(
                    error["code"], error["message"], error.get("details")
                )
        return self._successful(response, label="submission")

    def plan_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_pick", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_pick")
        return self._successful(method(intent), label="pick planning")

    def plan_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_place", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_place")
        return self._successful(method(intent), label="place planning")

    def operation(self, operation_id: str) -> dict[str, Any]:
        try:
            response = self._transport.read(operation_id)
        except Exception as exc:
            # Headless transports expose stable machine-readable error codes.
            # Map only owner availability on this exact read; every other
            # transport, protocol, and owner error retains its original type.
            if getattr(exc, "code", None) == "execution_owner_unavailable":
                raise KernelOperationObservationUnavailableError(
                    "native operation observation is temporarily unavailable"
                ) from exc
            raise
        return self._successful(response, label="observation")

    def recovery_assessment(self, operation_id: str) -> dict[str, Any]:
        method = getattr(self._transport, "operation_recovery", None)
        if not callable(method):
            raise TypeError("execution transport has no callable operation_recovery")
        response = self._successful(method(operation_id), label="recovery assessment")
        assessment = response.get("recovery_assessment")
        if not isinstance(assessment, dict):
            raise KernelClientResponseError(
                "headless recovery assessment has no owner projection"
            )
        revision = response.get("operation_revision")
        participants = response.get("participants")
        if (
            type(revision) is not int
            or revision < 1
            or not isinstance(participants, list)
            or not participants
        ):
            raise KernelClientResponseError(
                "headless recovery assessment has no exact operation scope"
            )
        return {
            **assessment,
            "operation_revision": revision,
            "participants": participants,
            "stop_id": response.get("stop_id"),
            "stop_generation": response.get("stop_generation"),
        }

    def recover(
        self,
        operation_id: str,
        expected_operation_revision: int,
        request: Mapping[str, Any],
    ) -> dict[str, Any]:
        method = getattr(self._transport, "apply_operation_recovery", None)
        if not callable(method):
            raise TypeError(
                "execution transport has no callable apply_operation_recovery"
            )
        response = self._successful(
            method(operation_id, expected_operation_revision, request),
            label="recovery action",
        )
        result = response.get("result")
        if (
            not isinstance(result, dict)
            or result.get("request_id") != request.get("request_id")
            or result.get("action") != request.get("action")
            or type(result.get("accepted")) is not bool
            or not isinstance(result.get("resulting_assessment"), dict)
        ):
            raise KernelClientResponseError(
                "headless recovery action has no exact owner result"
            )
        return result

    def query_operation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.query_operation(request), label="Program operation context"
        )

    def full_stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(self._transport.full_stop(request), label="Full Stop")

    def set_operation_runtime_scale(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.set_operation_runtime_scale(request),
            label="operation runtime scale",
        )

    def reset(self, operation_id: str, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.reset(operation_id, request),
            label="Reset",
        )

    def acknowledge_completed(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        try:
            response = self._transport.acknowledge_completed(operation_id, request)
        except Exception as error:
            details = getattr(error, "details", None)
            if getattr(error, "code", None) == "operation_effect_in_flight" or (
                isinstance(details, Mapping)
                and details.get("reason") == "operation_effect_in_flight"
            ):
                raise KernelOperationSettlementPendingError(
                    "operation completion is waiting for a subordinate effect"
                ) from error
            raise
        return self._successful(response, label="completion acknowledgement")

    def stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.stop(request),
            label="Stop",
        )

    def stop_program_run(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.stop_program_run(request),
            label="Program-run Stop",
        )

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        response = self._successful(
            self._transport.read_program_execution_scope(program_run_id),
            label="Program execution scope",
        )
        scope = response.get("scope")
        if not isinstance(scope, dict) or scope.get("program_run_id") != program_run_id:
            raise KernelClientResponseError("execution scope changed its Program run")
        return scope

    def read_manual_handover(self, request: Mapping[str, Any]) -> dict[str, Any] | None:
        response = self._successful(
            self._transport.read_manual_handover(request), label="manual handover"
        )
        handover = response.get("handover")
        if handover is not None and not isinstance(handover, dict):
            raise KernelClientResponseError("manual handover has an invalid shape")
        return handover

    def stop_program_selection(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.stop_program_selection(request),
            label="Program selection Stop",
        )
        accepted = response.get("accepted")
        if (
            response.get("command") != "stop_program_selection"
            or not isinstance(accepted, Mapping)
            or not isinstance(accepted.get("group_ids"), list)
            or not accepted["group_ids"]
            or accepted.get("group_ids") != request.get("group_ids")
            or accepted.get("replay_status") not in {"accepted_new", "replayed_exact"}
            or not isinstance(accepted.get("state"), Mapping)
        ):
            raise KernelClientResponseError("Program selection Stop omitted its scope")
        return accepted

    def release_program_selection(
        self, request: Mapping[str, Any], expected_selection_revision: int
    ) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.release_program_selection(
                request, expected_selection_revision
            ),
            label="Program selection Resume",
        )
        scope = response.get("scope")
        if (
            response.get("command") != "release_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("selection_revision")) is not int
            or scope["selection_revision"] != expected_selection_revision + 1
        ):
            raise KernelClientResponseError(
                "Program selection Resume omitted its scope"
            )
        return scope

    def start_program_selection(
        self, request: Mapping[str, Any], expected_selection_revision: int
    ) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.start_program_selection(
                request, expected_selection_revision
            ),
            label="Program selection initial Run",
        )
        scope = response.get("scope")
        if (
            response.get("command") != "start_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("selection_revision")) is not int
            or scope["selection_revision"] != expected_selection_revision + 1
        ):
            raise KernelClientResponseError(
                "Program selection initial Run omitted its scope"
            )
        return scope

    def check_program_node_motion_attention(
        self, request: Mapping[str, Any]
    ) -> None:
        response = self._successful(
            self._transport.check_program_node_motion_attention(request),
            label="Program node motion attention",
        )
        if response.get("command") != "check_program_node_motion_attention":
            raise KernelClientResponseError(
                "Program node motion attention omitted its command"
            )

    def release_program_run_stop_fence(
        self, program_run_id: str, stop_id: str, stop_generation: int
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.release_program_run_stop_fence(
                program_run_id, stop_id, stop_generation
            ),
            label="Program-run Stop release",
        )

    def stop_status(self, stop_id: str) -> dict[str, Any]:
        method = getattr(self._transport, "stop_status", None)
        if not callable(method):
            raise TypeError("execution transport has no callable stop_status")
        response = self._successful(method(stop_id), label="Stop status")
        status = response.get("stop_status")
        if (
            not isinstance(status, dict)
            or status.get("stop_id") != stop_id
            or status.get("state")
            not in {
                "accepted",
                "stopping",
                "certifying",
                "succeeded",
                "partial",
                "failed",
            }
        ):
            raise KernelClientResponseError(
                "execution Stop status changed its aggregate identity"
            )
        return response

    def stop_fault(self) -> dict[str, Any]:
        method = getattr(self._transport, "stop_fault", None)
        if not callable(method):
            raise TypeError("execution transport has no callable stop_fault")
        return self._successful(method(), label="Stop-fault observation")

    def recover_stop_fault(self, request: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "recover_stop_fault", None)
        if not callable(method):
            raise TypeError("execution transport has no callable recover_stop_fault")
        return self._successful(
            method(request),
            label="Stop-fault recovery",
        )


class MockKernelClient:
    """Removed stateful semantic mock; tests fake the thin HTTP transport."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise LegacyKernelMockUnavailableError(
            "stateful Python MockKernelClient was superseded by the C++ "
            "resource-addressed owner; inject a bounded transport fake"
        )


__all__ = [
    "ActionSpan",
    "ExecutionState",
    "ExecutionTransport",
    "KernelClient",
    "KernelOperationAdmissionRejectedError",
    "KernelOperationObservationUnavailableError",
    "KernelOperationSettlementPendingError",
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
