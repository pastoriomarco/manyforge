"""Thin Behavior client for the resource-addressed native execution owner."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Mapping, Protocol


class HeadlessExecutionTransport(Protocol):
    """The three bounded HTTP operations used by Behavior."""

    def execution_status(self) -> dict[str, Any]: ...

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def operation(self, operation_id: str) -> dict[str, Any]: ...


class KernelClientResponseError(RuntimeError):
    """Raised when a transport returns a successful but malformed projection."""


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
        for method in ("execution_status", "execute", "operation"):
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

    def operation(self, operation_id: str) -> dict[str, Any]:
        return self._successful(
            self._transport.operation(operation_id), label="observation"
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
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
