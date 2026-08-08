"""Focused NRT ports consumed by executable v2 orchestration adapters."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from types import MappingProxyType
from typing import Any, Protocol


class OwnerTaskState(Enum):
    ACTIVE = "active"
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    INTERRUPTED = "interrupted"


@dataclass(frozen=True, slots=True)
class NodeAttemptIdentity:
    program_run_id: str
    node_instance_id: str
    node_attempt_id: str
    parent_node_attempt_id: str | None = None

    def __post_init__(self) -> None:
        for field_name in ("program_run_id", "node_instance_id", "node_attempt_id"):
            value = getattr(self, field_name)
            if not isinstance(value, str) or not value:
                raise ValueError(f"{field_name} must be non-empty text")
            if len(value.encode("utf-8")) > 512:
                raise ValueError(f"{field_name} exceeds the Kernel field bound")
        if self.parent_node_attempt_id is not None and (
            not isinstance(self.parent_node_attempt_id, str)
            or not self.parent_node_attempt_id
            or len(self.parent_node_attempt_id.encode("utf-8")) > 512
        ):
            raise ValueError(
                "parent_node_attempt_id must be absent or bounded non-empty text"
            )


class NodeAttemptSource(Protocol):
    """Issue one fresh identity whenever a node attempt starts."""

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity: ...


def _freeze(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType({key: _freeze(nested) for key, nested in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(nested) for nested in value)
    return deepcopy(value)


@dataclass(frozen=True, slots=True)
class OwnerNodeInputs:
    """One immutable node-attempt snapshot sent to a semantic owner."""

    parameters: Mapping[str, Any]
    blackboard: Mapping[str, Any] = field(default_factory=lambda: MappingProxyType({}))
    embedded_values: Mapping[str, Any] = field(
        default_factory=lambda: MappingProxyType({})
    )

    def __post_init__(self) -> None:
        if not isinstance(self.parameters, Mapping):
            raise TypeError("owner parameters must be a mapping")
        if not isinstance(self.blackboard, Mapping):
            raise TypeError("owner blackboard inputs must be a mapping")
        if not isinstance(self.embedded_values, Mapping):
            raise TypeError("owner embedded values must be a mapping")
        if any(not isinstance(key, str) or not key for key in self.parameters):
            raise ValueError("owner parameter keys must be non-empty text")
        if any(not isinstance(key, str) or not key for key in self.blackboard):
            raise ValueError("owner blackboard keys must be non-empty text")
        if any(not isinstance(key, str) or not key for key in self.embedded_values):
            raise ValueError("owner embedded-value paths must be non-empty text")
        object.__setattr__(self, "parameters", _freeze(self.parameters))
        object.__setattr__(self, "blackboard", _freeze(self.blackboard))
        object.__setattr__(
            self,
            "embedded_values",
            _freeze(self.embedded_values),
        )


@dataclass(frozen=True, slots=True)
class OwnerTaskSnapshot:
    task_id: str
    state: OwnerTaskState
    output: Any = None
    metadata: Mapping[str, Any] = field(default_factory=lambda: MappingProxyType({}))
    reason_code: str | None = None
    message: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.task_id, str) or not self.task_id:
            raise ValueError("owner task id must be non-empty text")
        if not isinstance(self.state, OwnerTaskState):
            raise TypeError("owner task state must be an OwnerTaskState")
        if not isinstance(self.message, str):
            raise TypeError("owner task message must be text")
        if not isinstance(self.metadata, Mapping):
            raise TypeError("owner task metadata must be a mapping")
        if any(not isinstance(key, str) or not key for key in self.metadata):
            raise ValueError("owner task metadata keys must be non-empty text")
        if self.state is OwnerTaskState.SUCCEEDED and self.reason_code is not None:
            raise ValueError("a successful owner task cannot carry a failure reason")
        if self.state in {OwnerTaskState.FAILED, OwnerTaskState.INTERRUPTED} and (
            not isinstance(self.reason_code, str) or not self.reason_code
        ):
            raise ValueError("a failed owner task must carry a reason code")
        object.__setattr__(self, "output", deepcopy(self.output))
        object.__setattr__(
            self,
            "metadata",
            MappingProxyType(deepcopy(dict(self.metadata))),
        )


class OwnerTaskPort(Protocol):
    """One focused owner family with normalized asynchronous observation."""

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot: ...

    def observe(self, task_id: str) -> OwnerTaskSnapshot: ...

    def cancel(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
    ) -> None: ...

    def acknowledge(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
    ) -> None: ...


@dataclass(frozen=True, slots=True)
class OwnerInvocationResult:
    succeeded: bool
    output: Any = None
    metadata: Mapping[str, Any] = field(default_factory=lambda: MappingProxyType({}))
    reason_code: str | None = None
    message: str = ""

    def __post_init__(self) -> None:
        if type(self.succeeded) is not bool:
            raise TypeError("owner invocation succeeded must be boolean")
        if not isinstance(self.message, str):
            raise TypeError("owner invocation message must be text")
        if not isinstance(self.metadata, Mapping):
            raise TypeError("owner invocation metadata must be a mapping")
        if any(not isinstance(key, str) or not key for key in self.metadata):
            raise ValueError("owner invocation metadata keys must be non-empty text")
        if self.succeeded and self.reason_code is not None:
            raise ValueError("a successful invocation cannot carry a failure reason")
        if not self.succeeded and (
            not isinstance(self.reason_code, str) or not self.reason_code
        ):
            raise ValueError("a failed invocation must carry a reason code")
        object.__setattr__(self, "output", deepcopy(self.output))
        object.__setattr__(
            self,
            "metadata",
            MappingProxyType(deepcopy(dict(self.metadata))),
        )


class OwnerInvocationPort(Protocol):
    """One focused owner family whose public request settles synchronously."""

    def invoke(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerInvocationResult: ...


@dataclass(frozen=True, slots=True)
class NodeAdapterPorts:
    """Explicit focused ports; co-location never merges semantic ownership."""

    kernel_operations: OwnerTaskPort | None = None
    scene_transactions: OwnerInvocationPort | None = None
    gripper_status: OwnerInvocationPort | None = None
    model_queries: OwnerInvocationPort | None = None
    assistant_requests: OwnerTaskPort | None = None
    pick_place: OwnerTaskPort | None = None
    kernel_controls: OwnerInvocationPort | None = None


__all__ = [
    "NodeAdapterPorts",
    "NodeAttemptIdentity",
    "NodeAttemptSource",
    "OwnerNodeInputs",
    "OwnerInvocationPort",
    "OwnerInvocationResult",
    "OwnerTaskPort",
    "OwnerTaskSnapshot",
    "OwnerTaskState",
]
