"""Thin Behavior client for the resource-addressed native execution owner."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Mapping, Protocol


class HeadlessExecutionTransport(Protocol):
    """The bounded operation-addressed HTTP calls used by Behavior."""

    def execution_status(self) -> dict[str, Any]: ...

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def operation(self, operation_id: str) -> dict[str, Any]: ...

    def pause_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def resume_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def reset_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def acknowledge_completed_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def stop_operations(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def set_operation_runtime_scale(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def operation_runtime_scale(self, operation_id: str) -> dict[str, Any]: ...


class HeadlessPickPlacePlanningTransport(Protocol):
    """Optional stateless planning extension over the base execution client."""

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def start_pick(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def start_place(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def pick_place_sequence(self, sequence_id: str) -> dict[str, Any]: ...


class KernelClientResponseError(RuntimeError):
    """Raised when a transport returns a successful but malformed projection."""


class KernelOperationObservationUnavailableError(RuntimeError):
    """An exact operation read could not reach the native owner temporarily."""


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
    PAUSED = "paused"
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

    def __init__(self, transport: HeadlessExecutionTransport) -> None:
        for method in (
            "execution_status",
            "execute",
            "operation",
            "pause_operation",
            "resume_operation",
            "reset_operation",
            "acknowledge_completed_operation",
            "stop_operations",
            "set_operation_runtime_scale",
            "operation_runtime_scale",
        ):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"execution transport has no callable {method}")
        self._transport = transport

    @staticmethod
    def _successful(value: object, *, label: str) -> dict[str, Any]:
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise KernelClientResponseError(
                f"headless execution {label} did not return a successful envelope"
            )
        return value

    def status(self) -> dict[str, Any]:
        return self._successful(
            self._transport.execution_status(), label="status"
        )

    def execute(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.execute(intent), label="submission"
        )

    def plan_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_pick", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_pick")
        return self._successful(
            method(intent), label="pick planning"
        )

    def plan_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_place", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_place")
        return self._successful(
            method(intent), label="place planning"
        )

    def start_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "start_pick", None)
        if not callable(method):
            raise TypeError("execution transport has no callable start_pick")
        return self._successful(method(intent), label="pick sequence start")

    def start_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "start_place", None)
        if not callable(method):
            raise TypeError("execution transport has no callable start_place")
        return self._successful(method(intent), label="place sequence start")

    def pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        method = getattr(self._transport, "pick_place_sequence", None)
        if not callable(method):
            raise TypeError(
                "execution transport has no callable pick_place_sequence"
            )
        return self._successful(
            method(sequence_id), label="pick/place sequence observation"
        )

    def operation(self, operation_id: str) -> dict[str, Any]:
        try:
            response = self._transport.operation(operation_id)
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

    def pause(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.pause_operation(operation_id, request),
            label="Pause",
        )

    def resume(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.resume_operation(operation_id, request),
            label="Resume",
        )

    def reset(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.reset_operation(operation_id, request),
            label="Reset",
        )

    def acknowledge_completed(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.acknowledge_completed_operation(
                operation_id, request
            ),
            label="completion acknowledgement",
        )

    def stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.stop_operations(request),
            label="Stop",
        )

    def set_runtime_scale(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.set_operation_runtime_scale(operation_id, request),
            label="runtime-scale update",
        )

    def runtime_scale(self, operation_id: str) -> dict[str, Any]:
        return self._successful(
            self._transport.operation_runtime_scale(operation_id),
            label="runtime-scale observation",
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
    "HeadlessExecutionTransport",
    "KernelClient",
    "KernelOperationObservationUnavailableError",
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
