"""Focused NRT ports consumed by executable v2 orchestration adapters."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
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


class OwnerTaskObservationUnavailableError(RuntimeError):
    """The exact accepted owner task is temporarily unavailable to observe."""


class ProgramRunScopeState(Enum):
    ACTIVE = "active"
    SETTLED = "settled"
    UNKNOWN = "unknown"


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


@dataclass(frozen=True, slots=True)
class ProgramRunPortContext:
    """Immutable Applied Program/Binding coordinates used by client ports."""

    program_run_id: str
    scenario_id: str
    scenario_catalog_revision: str
    program_id: str
    program_revision: str
    program_binding_id: str
    retained_bundle_id: str
    graph_revision: str
    node_environment_digest: str
    program_binding: Mapping[str, Any]
    program: Mapping[str, Any] = field(
        default_factory=lambda: MappingProxyType({})
    )
    assistant_projection: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        for field_name in ("program_run_id", "scenario_id", "program_id"):
            value = getattr(self, field_name)
            if not isinstance(value, str) or not value:
                raise ValueError(f"{field_name} must be non-empty text")
            if len(value.encode("utf-8")) > 512:
                raise ValueError(f"{field_name} exceeds the client-port bound")
        for field_name in (
            "scenario_catalog_revision",
            "program_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
            "node_environment_digest",
        ):
            value = getattr(self, field_name)
            if (
                not isinstance(value, str)
                or len(value) != 71
                or not value.startswith("sha256:")
                or any(character not in "0123456789abcdef" for character in value[7:])
            ):
                raise ValueError(f"{field_name} must be an exact SHA-256 identity")
        if not isinstance(self.program_binding, Mapping):
            raise TypeError("program_binding must be a mapping")
        binding = dict(self.program_binding)
        if (
            binding.get("program_id") != self.program_id
            or binding.get("program_revision") != self.program_revision
            or binding.get("program_binding_id") != self.program_binding_id
            or binding.get("retained_bundle_id") != self.retained_bundle_id
            or binding.get("graph_revision") != self.graph_revision
        ):
            raise ValueError("Program-run context and Program Binding disagree")
        object.__setattr__(self, "program_binding", _freeze(binding))
        if not isinstance(self.program, Mapping):
            raise TypeError("program must be a mapping")
        program = dict(self.program)
        if program and (
            program.get("program_id") != self.program_id
            or program.get("program_revision") != self.program_revision
        ):
            raise ValueError("Program-run context and Program disagree")
        object.__setattr__(self, "program", _freeze(program))
        if self.assistant_projection is not None and not isinstance(
            self.assistant_projection, Mapping
        ):
            raise TypeError("assistant_projection must be absent or a mapping")
        object.__setattr__(
            self,
            "assistant_projection",
            (
                None
                if self.assistant_projection is None
                else _freeze(dict(self.assistant_projection))
            ),
        )


class ProgramRunContextPort(Protocol):
    """Bind exact client-only coordinates before constructing a Program tree."""

    def bind(self, context: ProgramRunPortContext) -> None: ...

    def unbind(self, program_run_id: str) -> None: ...


class ProgramRunContextError(RuntimeError):
    """One atomic Program-run context transition could not be completed."""


class CompositeProgramRunContextPort:
    """Bind focused context ports as one ordered, rollback-safe unit.

    The member ports keep their separate responsibilities.  This coordinator
    only guarantees that a Program tree never observes a partly bound set.
    """

    def __init__(self, *ports: ProgramRunContextPort) -> None:
        if not ports:
            raise ValueError("composite context requires at least one port")
        if any(
            not callable(getattr(port, "bind", None))
            or not callable(getattr(port, "unbind", None))
            for port in ports
        ):
            raise TypeError("every composite member must be a context port")
        if len({id(port) for port in ports}) != len(ports):
            raise ValueError("composite context ports must be unique")
        self._ports = tuple(ports)

    def bind(self, context: ProgramRunPortContext) -> None:
        bound: list[ProgramRunContextPort] = []
        try:
            for port in self._ports:
                bound.append(port)
                port.bind(context)
        except Exception as bind_error:
            rollback_errors: list[Exception] = []
            for port in reversed(bound):
                try:
                    port.unbind(context.program_run_id)
                except Exception as rollback_error:
                    rollback_errors.append(rollback_error)
            if rollback_errors:
                detail = "; ".join(
                    str(error) or type(error).__name__
                    for error in rollback_errors
                )
                raise ProgramRunContextError(
                    "Program-run context bind failed and rollback was not "
                    f"complete: {detail}"
                ) from bind_error
            raise

    def unbind(self, program_run_id: str) -> None:
        errors: list[Exception] = []
        for port in reversed(self._ports):
            try:
                port.unbind(program_run_id)
            except Exception as error:
                errors.append(error)
        if errors:
            detail = "; ".join(
                str(error) or type(error).__name__ for error in errors
            )
            raise ProgramRunContextError(
                f"Program-run context cleanup was incomplete: {detail}"
            ) from errors[0]


class ProgramRunInitializationPort(Protocol):
    """Apply captured pre-dispatch Program state before tree construction."""

    def initialize(
        self,
        context: ProgramRunPortContext,
        initial_scene: Sequence[Mapping[str, Any]],
    ) -> None: ...

    def release(self, program_run_id: str) -> None: ...


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
class ProgramRunScopeSnapshot:
    """Kernel-operation quiescence for one exact Program run."""

    program_run_id: str
    state: ProgramRunScopeState
    active_operation_count: int | None
    reason_code: str | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.program_run_id, str) or not self.program_run_id:
            raise ValueError("program_run_id must be non-empty text")
        if not isinstance(self.state, ProgramRunScopeState):
            raise TypeError("Program-run scope state is invalid")
        if self.active_operation_count is not None and (
            type(self.active_operation_count) is not int
            or not 0 <= self.active_operation_count <= 4_096
        ):
            raise ValueError("active_operation_count is outside its bound")
        if self.state is ProgramRunScopeState.SETTLED and (
            self.active_operation_count != 0 or self.reason_code is not None
        ):
            raise ValueError("a settled Program-run scope must be empty")
        if self.state is ProgramRunScopeState.ACTIVE and (
            self.active_operation_count is None
            or self.active_operation_count < 1
            or self.reason_code is not None
        ):
            raise ValueError("an active Program-run scope must name active work")
        if self.state is ProgramRunScopeState.UNKNOWN and (
            self.active_operation_count is not None
            or not isinstance(self.reason_code, str)
            or not self.reason_code
        ):
            raise ValueError("an unknown Program-run scope requires one reason")


class ProgramRunScopePort(Protocol):
    """Stop and observe all Kernel operations attributed to one Program run."""

    def request_stop(
        self,
        program_run_id: str,
        reason: str | None,
    ) -> ProgramRunScopeSnapshot: ...

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot: ...


@dataclass(frozen=True, slots=True)
class ProgramInterventionSnapshot:
    """One authored Program policy that requires an operator decision."""

    identity: NodeAttemptIdentity
    reason_code: str
    message: str
    policy_parameter: str

    def __post_init__(self) -> None:
        if not isinstance(self.identity, NodeAttemptIdentity):
            raise TypeError("intervention identity is invalid")
        for field_name, maximum_bytes in (
            ("reason_code", 256),
            ("policy_parameter", 256),
        ):
            value = getattr(self, field_name)
            if (
                not isinstance(value, str)
                or not value
                or len(value.encode("utf-8")) > maximum_bytes
            ):
                raise ValueError(f"{field_name} must be bounded non-empty text")
        if (
            not isinstance(self.message, str)
            or len(self.message.encode("utf-8")) > 4_096
        ):
            raise ValueError("intervention message must be bounded text")


class ProgramInterventionPort(Protocol):
    """Record and read authored intervention without owning Kernel Pause."""

    def require(
        self,
        identity: NodeAttemptIdentity,
        *,
        reason_code: str,
        message: str,
        policy_parameter: str,
    ) -> ProgramInterventionSnapshot: ...

    def read(self, program_run_id: str) -> ProgramInterventionSnapshot | None: ...

    def release(self, program_run_id: str) -> None: ...


@dataclass(frozen=True, slots=True)
class NodeAdapterPorts:
    """Explicit focused ports; co-location never merges semantic ownership."""

    kernel_operations: OwnerTaskPort | None = None
    scene_transactions: OwnerInvocationPort | None = None
    gripper_status: OwnerInvocationPort | None = None
    model_queries: OwnerInvocationPort | None = None
    assistant_requests: OwnerTaskPort | None = None
    pick_place: OwnerTaskPort | None = None
    simulator_control: OwnerTaskPort | None = None
    cclc_lifecycle: OwnerTaskPort | None = None
    cclc_routes: OwnerTaskPort | None = None
    kernel_controls: OwnerInvocationPort | None = None
    program_run_scope: ProgramRunScopePort | None = None
    program_run_context: ProgramRunContextPort | None = None
    program_run_initialization: ProgramRunInitializationPort | None = None
    program_interventions: ProgramInterventionPort | None = None


__all__ = [
    "NodeAdapterPorts",
    "NodeAttemptIdentity",
    "NodeAttemptSource",
    "CompositeProgramRunContextPort",
    "ProgramRunContextError",
    "ProgramRunContextPort",
    "ProgramRunInitializationPort",
    "ProgramInterventionPort",
    "ProgramInterventionSnapshot",
    "ProgramRunPortContext",
    "OwnerNodeInputs",
    "OwnerInvocationPort",
    "OwnerInvocationResult",
    "OwnerTaskPort",
    "OwnerTaskObservationUnavailableError",
    "OwnerTaskSnapshot",
    "OwnerTaskState",
    "ProgramRunScopePort",
    "ProgramRunScopeSnapshot",
    "ProgramRunScopeState",
]
