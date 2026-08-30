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

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def read(self, operation_id: str) -> dict[str, Any]: ...

    def operation_recovery(self, operation_id: str) -> dict[str, Any]: ...

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def pause(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def resume(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def reset(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_program_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_status(self, stop_id: str) -> dict[str, Any]: ...

    def set_runtime_scale(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def get_runtime_scale(self, operation_id: str) -> dict[str, Any]: ...


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

    def __init__(self, transport: ExecutionTransport) -> None:
        for method in (
            "identity",
            "submit",
            "read",
            "pause",
            "resume",
            "reset",
            "acknowledge_completed",
            "stop",
            "set_runtime_scale",
            "get_runtime_scale",
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

    def targets(self) -> dict[str, Any]:
        return self._projection("targets")

    def control_state(self) -> dict[str, Any]:
        return self._projection("control_state")

    def observation(self) -> dict[str, Any]:
        return self._projection("observation")

    def _projection(self, method_name: str) -> dict[str, Any]:
        method = getattr(self._transport, method_name, None)
        if not callable(method):
            raise TypeError(f"execution transport has no callable {method_name}")
        return self._successful(method(), label=method_name.replace("_", " "))

    def execute(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.submit(intent), label="submission"
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
        method = getattr(self._transport, "read_pick_place_sequence", None)
        if not callable(method):
            raise TypeError(
                "execution transport has no callable read_pick_place_sequence"
            )
        return self._successful(
            method(sequence_id), label="pick/place sequence observation"
        )

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
            raise TypeError(
                "execution transport has no callable operation_recovery"
            )
        response = self._successful(
            method(operation_id), label="recovery assessment"
        )
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
        if not isinstance(result, dict):
            raise KernelClientResponseError(
                "headless recovery action has no owner result"
            )
        return result

    def pause(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.pause(operation_id, request),
            label="Pause",
        )

    def resume(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.resume(operation_id, request),
            label="Resume",
        )

    def reset(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.reset(operation_id, request),
            label="Reset",
        )

    def acknowledge_completed(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.acknowledge_completed(operation_id, request),
            label="completion acknowledgement",
        )

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

    def set_runtime_scale(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.set_runtime_scale(operation_id, request),
            label="runtime-scale update",
        )

    def runtime_scale(self, operation_id: str) -> dict[str, Any]:
        return self._successful(
            self._transport.get_runtime_scale(operation_id),
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
    "ExecutionTransport",
    "KernelClient",
    "KernelOperationObservationUnavailableError",
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
