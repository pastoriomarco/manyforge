"""Thin Kernel-client values during the I6/N and I8/P5 binding cutover.

The former module duplicated C++ DTOs in mutable Python dataclasses, addressed
one ``robot_id/group_id``, supplied permissive abstract-method fallbacks, and
implemented a stateful Python mock Kernel including Stop and recovery truth.
Those contracts are replaced atomically by the resource-addressed C++ DTOs and
Python bindings.  Exact presentation vocabulary and behavior-owned action-span
correlation remain; construction of either legacy client fails before effects.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Final


KERNEL_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_kernel_client_binding_unavailable"
)
KERNEL_CLIENT_RESTORATION_GATES: Final = ("I6", "N", "I8", "P5")
_ACTION_SOURCES: Final = frozenset(
    {"program_node", "direct_control", "component_control", "mcp", "system"}
)


class KernelClientBindingUnavailableError(RuntimeError):
    """Raised before native bindings, clocks, threads, or Kernel state access."""


class ExecutionState(Enum):
    """Dependency-light mirror of the stable C++ execution-state vocabulary."""

    IDLE = "idle"
    PLANNING = "planning"
    VALIDATING = "validating"
    EXECUTING = "executing"
    INTERRUPTED = "interrupted"
    PAUSED = "paused"
    STOPPING = "stopping"
    STOPPED = "stopped"
    COMPLETED = "completed"
    FAILED = "failed"
    INTERVENTION_REQUIRED = "intervention_required"


@dataclass(frozen=True, slots=True)
class ActionSpan:
    """Transport-neutral spec/210 correlation value owned by behavior nodes."""

    action_source: str = "system"
    action_span_id: str = ""
    program_run_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None
    parent_node_attempt_id: str | None = None

    def __post_init__(self) -> None:
        if self.action_source not in _ACTION_SOURCES:
            raise ValueError("action_source is unsupported")
        if not self.action_span_id or len(self.action_span_id) > 512:
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


def _raise_kernel_client_unavailable() -> None:
    raise KernelClientBindingUnavailableError(
        f"{KERNEL_CLIENT_UNAVAILABLE_REASON_CODE}: resource-addressed C++ "
        "Kernel bindings return at I6/N and multi-domain Stop at I8/P5"
    )


class KernelClient:
    """Unavailable compatibility name; no Python Kernel authority remains."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_kernel_client_unavailable()


class MockKernelClient(KernelClient):
    """Unavailable old semantic mock; future tests use the C++ fake owner."""


__all__ = [
    "ActionSpan",
    "ExecutionState",
    "KERNEL_CLIENT_RESTORATION_GATES",
    "KERNEL_CLIENT_UNAVAILABLE_REASON_CODE",
    "KernelClient",
    "KernelClientBindingUnavailableError",
    "MockKernelClient",
]
