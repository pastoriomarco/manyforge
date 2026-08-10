"""Bounded owner for exact Program-run orchestration lifecycles."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from threading import RLock
from types import MappingProxyType
from typing import Any, Final
from uuid import uuid4

import py_trees

from manyforge_behavior.node_availability import NodeExecutionReadiness
from manyforge_behavior.node_availability import AvailabilityFact
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.orchestration_ports import (
    NodeAdapterPorts,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.program import Program
from manyforge_behavior.program_runtime import (
    ExecutableProgramRun,
    ProgramRunCapture,
    ProgramRunOwnerIdentity,
)


PROGRAM_RUN_STATUS_API: Final = "manyforge.program_run_status.v1"
SCENARIO_PROGRAM_SELECTION_API: Final = "manyforge.scenario_program_selection.v2"
APPLIED_PROGRAM_SELECTION_API: Final = "manyforge.applied_program_selection.v1"
_MAX_RETAINED_RUNS: Final = 4_096


class ProgramRunOwnerError(RuntimeError):
    """A Program-run owner request is invalid, stale, or unavailable."""


class ProgramRunState(Enum):
    RUNNING = "running"
    STOPPING = "stopping"
    STOPPED = "stopped"
    SUCCEEDED = "succeeded"
    FAILED = "failed"


def _text(value: object, *, field: str, maximum_bytes: int = 512) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be non-empty text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{field} exceeds {maximum_bytes} bytes")
    return value


def _digest(value: object, *, field: str) -> str:
    text = _text(value, field=field, maximum_bytes=71)
    if (
        len(text) != 71
        or not text.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in text[7:])
    ):
        raise ValueError(f"{field} must be an exact SHA-256 identity")
    return text


@dataclass(frozen=True, slots=True)
class ProgramRunAdmission:
    """One exact verified scenario closure admitted before tree construction."""

    scenario_id: str
    scenario_catalog_revision: str
    program: Program
    environment: NodeEnvironment
    program_binding_id: str
    retained_bundle_id: str
    graph_revision: str
    program_binding: Mapping[str, Any]
    readiness: NodeExecutionReadiness
    owner_identities: tuple[ProgramRunOwnerIdentity, ...]
    program_parameter_values: Mapping[str, Any]
    assistant_projection: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        _text(self.scenario_id, field="scenario_id", maximum_bytes=128)
        for field_name in (
            "scenario_catalog_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
        ):
            _digest(getattr(self, field_name), field=field_name)
        if not isinstance(self.program, Program):
            raise TypeError("program must be one validated Program")
        if not isinstance(self.environment, NodeEnvironment):
            raise TypeError("environment must be one retained NodeEnvironment")
        if not isinstance(self.readiness, NodeExecutionReadiness):
            raise TypeError("readiness must contain exact owner facts")
        if not isinstance(self.program_binding, Mapping):
            raise TypeError("program_binding must be one verified mapping")
        binding = dict(self.program_binding)
        if (
            binding.get("program_id") != self.program.program_id
            or binding.get("program_revision") != self.program.program_revision
            or binding.get("program_binding_id") != self.program_binding_id
            or binding.get("retained_bundle_id") != self.retained_bundle_id
            or binding.get("graph_revision") != self.graph_revision
        ):
            raise ValueError("Program-run admission and Program Binding disagree")
        object.__setattr__(
            self,
            "program_binding",
            MappingProxyType(deepcopy(binding)),
        )
        if self.readiness.program_binding_id != self.program_binding_id:
            raise ValueError("readiness selects another Program Binding")
        if (
            not isinstance(self.owner_identities, tuple)
            or any(
                not isinstance(identity, ProgramRunOwnerIdentity)
                for identity in self.owner_identities
            )
            or tuple(
                sorted(
                    self.owner_identities,
                    key=lambda identity: identity.owner_role,
                )
            )
            != self.owner_identities
            or len({identity.owner_role for identity in self.owner_identities})
            != len(self.owner_identities)
            or any(
                identity.owner_role == "program_runner"
                for identity in self.owner_identities
            )
        ):
            raise ValueError(
                "admitted owner identities must be unique, role-sorted, and "
                "exclude the local Program runner"
            )
        if not isinstance(self.program_parameter_values, Mapping):
            raise TypeError("program_parameter_values must be a mapping")
        if any(
            not isinstance(key, str) or not key for key in self.program_parameter_values
        ):
            raise ValueError("Program parameter IDs must be non-empty text")
        object.__setattr__(
            self,
            "program_parameter_values",
            MappingProxyType(deepcopy(dict(self.program_parameter_values))),
        )
        if self.assistant_projection is not None and not isinstance(
            self.assistant_projection, Mapping
        ):
            raise TypeError("assistant_projection must be absent or one mapping")
        object.__setattr__(
            self,
            "assistant_projection",
            (
                None
                if self.assistant_projection is None
                else MappingProxyType(
                    deepcopy(dict(self.assistant_projection))
                )
            ),
        )


@dataclass(frozen=True, slots=True)
class ProgramRunSnapshot:
    owner_id: str
    owner_epoch: str
    run_revision: int
    state: ProgramRunState
    capture: ProgramRunCapture
    tree_status: str
    active_operation_count: int | None
    reason_code: str | None
    message: str

    def __post_init__(self) -> None:
        _text(self.owner_id, field="owner_id")
        _text(self.owner_epoch, field="owner_epoch")
        if type(self.run_revision) is not int or not 1 <= self.run_revision:
            raise ValueError("run_revision must be positive")
        if not isinstance(self.state, ProgramRunState):
            raise TypeError("Program-run state is invalid")
        if not isinstance(self.capture, ProgramRunCapture):
            raise TypeError("Program-run capture is invalid")
        if self.tree_status not in {
            "invalid",
            "running",
            "success",
            "failure",
        }:
            raise ValueError("tree_status is invalid")
        if self.active_operation_count is not None and (
            type(self.active_operation_count) is not int
            or not 0 <= self.active_operation_count <= 4_096
        ):
            raise ValueError("active_operation_count is outside its bound")
        if self.reason_code is not None:
            _text(self.reason_code, field="reason_code", maximum_bytes=256)
        if (
            not isinstance(self.message, str)
            or len(self.message.encode("utf-8")) > 4_096
        ):
            raise ValueError("message must be bounded text")

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": PROGRAM_RUN_STATUS_API,
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
            "run_revision": self.run_revision,
            "state": self.state.value,
            "capture": self.capture.to_dict(),
            "tree_status": self.tree_status,
            "active_operation_count": self.active_operation_count,
            "reason_code": self.reason_code,
            "message": self.message,
        }


@dataclass(slots=True)
class _RunEntry:
    run: ExecutableProgramRun
    state: ProgramRunState = ProgramRunState.RUNNING
    revision: int = 1
    tree_status: str = "invalid"
    active_operation_count: int | None = 0
    reason_code: str | None = None
    message: str = ""
    stop_requested: bool = False
    state_after_stop: ProgramRunState = ProgramRunState.STOPPED
    state_after_stop_reason_code: str | None = None
    state_after_stop_message: str = ""
    transition_lock: RLock = field(default_factory=RLock, repr=False)


class ProgramRunOwner:
    """Own orchestration state while Kernel and focused ports own effects."""

    def __init__(
        self,
        *,
        owner_id: str,
        owner_epoch: str,
        ports: NodeAdapterPorts,
        run_id_factory: Callable[[], str] | None = None,
        maximum_retained_runs: int = _MAX_RETAINED_RUNS,
    ) -> None:
        self._owner_id = _text(owner_id, field="owner_id")
        self._owner_epoch = _text(owner_epoch, field="owner_epoch")
        if not isinstance(ports, NodeAdapterPorts):
            raise TypeError("ports must be one focused owner-port set")
        if ports.program_run_scope is None:
            raise ValueError("Program-run owner requires one Kernel scope port")
        if (
            type(maximum_retained_runs) is not int
            or not 1 <= maximum_retained_runs <= _MAX_RETAINED_RUNS
        ):
            raise ValueError("maximum_retained_runs is outside its bound")
        self._ports = ports
        self._scope = ports.program_run_scope
        self._run_id_factory = run_id_factory or (lambda: f"program-run-{uuid4().hex}")
        self._maximum_retained_runs = maximum_retained_runs
        self._runs: dict[str, _RunEntry] = {}
        self._lock = RLock()

    def _entry(self, program_run_id: str) -> _RunEntry:
        _text(program_run_id, field="program_run_id")
        try:
            return self._runs[program_run_id]
        except KeyError as exc:
            raise ProgramRunOwnerError("Program run does not exist") from exc

    def _snapshot(self, entry: _RunEntry) -> ProgramRunSnapshot:
        return ProgramRunSnapshot(
            owner_id=self._owner_id,
            owner_epoch=self._owner_epoch,
            run_revision=entry.revision,
            state=entry.state,
            capture=entry.run.capture,
            tree_status=entry.tree_status,
            active_operation_count=entry.active_operation_count,
            reason_code=entry.reason_code,
            message=entry.message,
        )

    @staticmethod
    def _scope_fields(
        snapshot: ProgramRunScopeSnapshot,
        program_run_id: str,
    ) -> tuple[int | None, bool, str | None]:
        if not isinstance(snapshot, ProgramRunScopeSnapshot):
            raise ProgramRunOwnerError("Kernel scope returned an invalid snapshot")
        if snapshot.program_run_id != program_run_id:
            raise ProgramRunOwnerError("Kernel scope returned another Program run")
        return (
            snapshot.active_operation_count,
            snapshot.state is ProgramRunScopeState.SETTLED,
            snapshot.reason_code,
        )

    def start(self, admission: ProgramRunAdmission) -> ProgramRunSnapshot:
        if not isinstance(admission, ProgramRunAdmission):
            raise TypeError("start requires one exact Program-run admission")
        with self._lock:
            if len(self._runs) >= self._maximum_retained_runs:
                raise ProgramRunOwnerError("Program-run retention is full")
            program_run_id = _text(
                self._run_id_factory(),
                field="program_run_id",
            )
            if program_run_id in self._runs:
                raise ProgramRunOwnerError("Program-run identity was reused")
            owner_identities = tuple(
                sorted(
                    (
                        *admission.owner_identities,
                        ProgramRunOwnerIdentity(
                            owner_role="program_runner",
                            owner_id=self._owner_id,
                            owner_epoch=self._owner_epoch,
                        ),
                    ),
                    key=lambda identity: identity.owner_role,
                )
            )
            capture = ProgramRunCapture(
                program_run_id=program_run_id,
                scenario_id=admission.scenario_id,
                scenario_catalog_revision=admission.scenario_catalog_revision,
                program_id=admission.program.program_id,
                program_revision=admission.program.program_revision,
                program_binding_id=admission.program_binding_id,
                retained_bundle_id=admission.retained_bundle_id,
                graph_revision=admission.graph_revision,
                node_environment_digest=admission.environment.environment_digest,
                owner_identities=owner_identities,
            )
            context_port = self._ports.program_run_context
            initialization_port = self._ports.program_run_initialization
            context_bound = False
            initialization_started = False
            port_context = ProgramRunPortContext(
                program_run_id=capture.program_run_id,
                scenario_id=capture.scenario_id,
                scenario_catalog_revision=capture.scenario_catalog_revision,
                program_id=capture.program_id,
                program_revision=capture.program_revision,
                program_binding_id=capture.program_binding_id,
                retained_bundle_id=capture.retained_bundle_id,
                graph_revision=capture.graph_revision,
                node_environment_digest=capture.node_environment_digest,
                program_binding=admission.program_binding,
                program=admission.program.to_dict(),
                assistant_projection=admission.assistant_projection,
            )
            try:
                if context_port is not None:
                    context_port.bind(port_context)
                    context_bound = True
                if initialization_port is not None:
                    initialization_started = True
                    initial_scene = admission.program.to_dict().get("scene", [])
                    initialization_port.initialize(port_context, initial_scene)
                run = ExecutableProgramRun(
                    program=admission.program,
                    environment=admission.environment,
                    capture=capture,
                    readiness=admission.readiness,
                    ports=self._ports,
                    program_parameter_values=admission.program_parameter_values,
                )
            except Exception:
                try:
                    if initialization_started and initialization_port is not None:
                        initialization_port.release(program_run_id)
                finally:
                    if context_bound and context_port is not None:
                        context_port.unbind(program_run_id)
                raise
            entry = _RunEntry(run=run)
            self._runs[program_run_id] = entry
            return self._snapshot(entry)

    def read(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            stopping = entry.state is ProgramRunState.STOPPING
        if stopping:
            return self.reconcile_stop(program_run_id)
        with self._lock:
            return self._snapshot(self._entry(program_run_id))

    def _begin_stop(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        state_after_stop: ProgramRunState,
        reason_code: str | None,
        message: str,
    ) -> ProgramRunSnapshot:
        with self._lock:
            transition_lock = self._entry(program_run_id).transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                }:
                    return self._snapshot(entry)
                issue_stop = not entry.stop_requested
                if issue_stop:
                    entry.stop_requested = True
                    entry.state = ProgramRunState.STOPPING
                    entry.state_after_stop = state_after_stop
                    entry.state_after_stop_reason_code = reason_code
                    entry.state_after_stop_message = message
                    entry.reason_code = reason_code
                    entry.message = message
                    entry.revision += 1
                run = entry.run

            scope_snapshot: ProgramRunScopeSnapshot | None = None
            scope_errors: list[Exception] = []
            if issue_stop:
                try:
                    scope_snapshot = self._scope.request_stop(program_run_id, reason)
                except Exception as exc:  # fail closed; status remains stopping
                    scope_errors.append(exc)
                try:
                    # Kernel Stop has already had its independent priority path.
                    # Tree invalidation only prevents later orchestration effects.
                    run.stop()
                except Exception as exc:
                    scope_errors.append(exc)
            scope_fields: tuple[int | None, bool, str | None] | None = None
            if scope_snapshot is not None and not scope_errors:
                try:
                    scope_fields = self._scope_fields(
                        scope_snapshot,
                        program_run_id,
                    )
                except Exception as exc:
                    scope_errors.append(exc)

            with self._lock:
                entry = self._entry(program_run_id)
                if scope_errors:
                    entry.active_operation_count = None
                    entry.reason_code = "program_run_scope_unknown"
                    entry.message = "; ".join(
                        str(error) or type(error).__name__ for error in scope_errors
                    )
                    return self._snapshot(entry)
                if scope_fields is not None:
                    active_count, settled, scope_reason = scope_fields
                    entry.active_operation_count = active_count
                    if scope_reason is not None:
                        entry.reason_code = scope_reason
                        entry.message = "Kernel Program-run scope is unknown"
                    else:
                        entry.reason_code = entry.state_after_stop_reason_code
                        entry.message = entry.state_after_stop_message
                    if settled:
                        entry.state = entry.state_after_stop
                        entry.revision += 1
                return self._snapshot(entry)

    def stop(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        return self._begin_stop(
            program_run_id,
            reason=reason,
            state_after_stop=ProgramRunState.STOPPED,
            reason_code=None,
            message="",
        )

    def reconcile_stop(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.STOPPING:
                return self._snapshot(entry)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.STOPPING:
                    return self._snapshot(entry)
            try:
                scope_snapshot = self._scope.observe(program_run_id)
                scope_fields = self._scope_fields(scope_snapshot, program_run_id)
            except Exception as exc:
                with self._lock:
                    entry = self._entry(program_run_id)
                    entry.active_operation_count = None
                    entry.reason_code = "program_run_scope_unknown"
                    entry.message = str(exc) or type(exc).__name__
                    return self._snapshot(entry)
            with self._lock:
                entry = self._entry(program_run_id)
                active_count, settled, scope_reason = scope_fields
                entry.active_operation_count = active_count
                if scope_reason is not None:
                    entry.reason_code = scope_reason
                    entry.message = "Kernel Program-run scope is unknown"
                else:
                    entry.reason_code = entry.state_after_stop_reason_code
                    entry.message = entry.state_after_stop_message
                if entry.state is ProgramRunState.STOPPING and settled:
                    entry.state = entry.state_after_stop
                    entry.revision += 1
                return self._snapshot(entry)

    def advance_once(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is ProgramRunState.STOPPING:
                    reconcile = True
                elif entry.state is not ProgramRunState.RUNNING:
                    return self._snapshot(entry)
                else:
                    reconcile = False
                    run = entry.run
            if reconcile:
                return self.reconcile_stop(program_run_id)

            try:
                tree_status = run.tick_once()
            except Exception as exc:
                return self._begin_stop(
                    program_run_id,
                    reason="Program runner failure",
                    state_after_stop=ProgramRunState.FAILED,
                    reason_code="program_runtime_failed",
                    message=str(exc) or type(exc).__name__,
                )

            status_name = tree_status.name.lower()
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.RUNNING:
                    return self._snapshot(entry)
                if entry.tree_status != status_name:
                    entry.tree_status = status_name
                    entry.revision += 1
                if tree_status is py_trees.common.Status.RUNNING:
                    return self._snapshot(entry)

            terminal_state = (
                ProgramRunState.SUCCEEDED
                if tree_status is py_trees.common.Status.SUCCESS
                else ProgramRunState.FAILED
            )
            try:
                scope_snapshot = self._scope.observe(program_run_id)
                _active_count, scope_settled, _scope_reason = self._scope_fields(
                    scope_snapshot,
                    program_run_id,
                )
            except Exception as exc:
                return self._begin_stop(
                    program_run_id,
                    reason="Program terminal scope unknown",
                    state_after_stop=terminal_state,
                    reason_code="program_run_scope_unknown",
                    message=str(exc) or type(exc).__name__,
                )
            if not scope_settled:
                return self._begin_stop(
                    program_run_id,
                    reason="Program tree terminated with active operations",
                    state_after_stop=terminal_state,
                    reason_code="program_terminal_scope_not_settled",
                    message=("Program tree terminated before its Kernel scope settled"),
                )

            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is ProgramRunState.RUNNING:
                    entry.active_operation_count = 0
                    entry.state = terminal_state
                    entry.reason_code = (
                        None
                        if terminal_state is ProgramRunState.SUCCEEDED
                        else "program_tree_failed"
                    )
                    entry.message = (
                        ""
                        if terminal_state is ProgramRunState.SUCCEEDED
                        else "Program tree failed"
                    )
                    entry.revision += 1
                return self._snapshot(entry)


def build_program_run_admission(
    selection: Mapping[str, Any],
    *,
    expected_scenario_id: str,
    expected_scenario_catalog_revision: str,
    environment: NodeEnvironment,
    owner_availability: Mapping[str, AvailabilityFact],
    additional_owner_identities: tuple[ProgramRunOwnerIdentity, ...] = (),
) -> ProgramRunAdmission:
    """Join one native exact selection to the packaged Python runner."""

    if not isinstance(selection, Mapping):
        raise TypeError("scenario Program selection must be a mapping")
    expected_fields = {
        "assistant_projection",
        "assistant_mode",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "owner_identities",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "schema_version",
        "typed_parameter_overrides",
    }
    if set(selection) != expected_fields:
        raise ProgramRunOwnerError("scenario Program selection has wrong fields")
    if selection.get("schema_version") != SCENARIO_PROGRAM_SELECTION_API:
        raise ProgramRunOwnerError("scenario Program selection version is unsupported")
    _text(selection.get("deployment_artifact_id"), field="deployment_artifact_id")
    _digest(selection.get("deployment_revision"), field="deployment_revision")
    if selection.get("assistant_mode") not in {
        "required",
        "optional",
        "forbidden",
    }:
        raise ProgramRunOwnerError("scenario assistant mode is invalid")
    assistant_mode = selection["assistant_mode"]
    assistant_projection = selection.get("assistant_projection")
    if assistant_projection is not None and not isinstance(
        assistant_projection, dict
    ):
        raise ProgramRunOwnerError(
            "scenario assistant projection must be null or an object"
        )
    if assistant_mode == "required" and assistant_projection is None:
        raise ProgramRunOwnerError(
            "scenario requires an absent assistant projection"
        )
    if assistant_mode == "forbidden" and assistant_projection is not None:
        raise ProgramRunOwnerError(
            "scenario forbids a present assistant projection"
        )
    scenario_id = _text(selection.get("scenario_id"), field="scenario_id")
    scenario_catalog_revision = _digest(
        selection.get("scenario_catalog_revision"),
        field="scenario_catalog_revision",
    )
    if scenario_id != expected_scenario_id:
        raise ProgramRunOwnerError("scenario Program selection changed scenario ID")
    if scenario_catalog_revision != expected_scenario_catalog_revision:
        raise ProgramRunOwnerError("scenario catalog changed during Run admission")
    if not isinstance(environment, NodeEnvironment):
        raise TypeError("environment must be one retained NodeEnvironment")
    node_environment_digest = _digest(
        selection.get("node_environment_digest"),
        field="node_environment_digest",
    )
    if node_environment_digest != environment.environment_digest:
        raise ProgramRunOwnerError(
            "selected node environment differs from the packaged runner"
        )

    program_value = selection.get("program")
    binding_value = selection.get("program_binding")
    overrides = selection.get("typed_parameter_overrides")
    if not isinstance(program_value, dict):
        raise ProgramRunOwnerError("scenario selection has no Program object")
    if not isinstance(binding_value, dict):
        raise ProgramRunOwnerError("scenario selection has no Program Binding object")
    if not isinstance(overrides, dict):
        raise ProgramRunOwnerError("scenario selection overrides are not an object")
    program = Program.from_dict(deepcopy(program_value), environment.catalog)
    program_binding_id = _digest(
        selection.get("program_binding_id"),
        field="program_binding_id",
    )
    retained_bundle_id = _digest(
        selection.get("retained_bundle_id"),
        field="retained_bundle_id",
    )
    graph_revision = _digest(
        selection.get("graph_revision"),
        field="graph_revision",
    )
    program_revision = _digest(
        selection.get("program_revision"),
        field="program_revision",
    )
    program_id = _text(selection.get("program_id"), field="program_id")
    binding_environment = binding_value.get("node_environment")
    if (
        program.program_id != program_id
        or program.program_revision != program_revision
        or binding_value.get("program_binding_id") != program_binding_id
        or binding_value.get("program_id") != program_id
        or binding_value.get("program_revision") != program_revision
        or binding_value.get("retained_bundle_id") != retained_bundle_id
        or binding_value.get("graph_revision") != graph_revision
        or not isinstance(binding_environment, dict)
        or binding_environment.get("environment_digest") != node_environment_digest
    ):
        raise ProgramRunOwnerError(
            "scenario Program, binding, and retained identities disagree"
        )

    raw_owner_identities = selection.get("owner_identities")
    if not isinstance(raw_owner_identities, list):
        raise ProgramRunOwnerError("scenario selection owner identities are invalid")
    parsed_owner_identities: list[ProgramRunOwnerIdentity] = []
    for raw_identity in raw_owner_identities:
        if not isinstance(raw_identity, dict) or set(raw_identity) != {
            "owner_role",
            "owner_id",
            "owner_epoch",
        }:
            raise ProgramRunOwnerError(
                "scenario selection has a malformed owner identity"
            )
        parsed_owner_identities.append(
            ProgramRunOwnerIdentity(
                owner_role=raw_identity["owner_role"],
                owner_id=raw_identity["owner_id"],
                owner_epoch=raw_identity["owner_epoch"],
            )
        )
    for identity in additional_owner_identities:
        if not isinstance(identity, ProgramRunOwnerIdentity):
            raise TypeError("additional owner identity is invalid")
        parsed_owner_identities.append(identity)
    owner_identities = tuple(
        sorted(parsed_owner_identities, key=lambda identity: identity.owner_role)
    )
    if len({identity.owner_role for identity in owner_identities}) != len(
        owner_identities
    ):
        raise ProgramRunOwnerError("scenario selection repeats an owner role")

    resolver = build_executable_node_resolver(environment)
    expected_ports = {
        disposition.owner_port_name
        for disposition in resolver.dispositions
        if disposition.owner_port_name is not None
    }
    if (
        not isinstance(owner_availability, Mapping)
        or set(owner_availability) != expected_ports
        or any(
            not isinstance(fact, AvailabilityFact)
            for fact in owner_availability.values()
        )
    ):
        raise ProgramRunOwnerError("Program owner availability facts are not exact")
    readiness = NodeExecutionReadiness(
        program_binding_id=program_binding_id,
        binding_by_node={
            disposition.row.node_id: AvailabilityFact(True)
            for disposition in resolver.dispositions
        },
        owner_by_port=owner_availability,
    )
    return ProgramRunAdmission(
        scenario_id=scenario_id,
        scenario_catalog_revision=scenario_catalog_revision,
        program=program,
        environment=environment,
        program_binding_id=program_binding_id,
        retained_bundle_id=retained_bundle_id,
        graph_revision=graph_revision,
        program_binding=binding_value,
        readiness=readiness,
        owner_identities=owner_identities,
        program_parameter_values=overrides,
        assistant_projection=assistant_projection,
    )


def build_applied_program_run_admission(
    applied: Mapping[str, Any],
    live_scenario_selection: Mapping[str, Any],
    *,
    expected_scenario_id: str,
    expected_scenario_catalog_revision: str,
    environment: NodeEnvironment,
    owner_availability: Mapping[str, AvailabilityFact],
    additional_owner_identities: tuple[ProgramRunOwnerIdentity, ...] = (),
) -> ProgramRunAdmission:
    """Join authoritative Applied content to fresh scenario-owner identities."""

    if not isinstance(applied, Mapping):
        raise TypeError("Applied Program selection must be a mapping")
    expected_applied_fields = {
        "assistant_mode",
        "baseline_program_binding_id",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "preparation_digest",
        "program",
        "program_binding",
        "program_binding_id",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "schema_version",
        "source",
        "typed_parameter_overrides",
    }
    if set(applied) != expected_applied_fields:
        raise ProgramRunOwnerError("Applied Program selection has wrong fields")
    if applied.get("schema_version") != APPLIED_PROGRAM_SELECTION_API:
        raise ProgramRunOwnerError("Applied Program selection version is unsupported")
    _digest(applied.get("baseline_program_binding_id"), field="baseline_program_binding_id")
    _digest(applied.get("preparation_digest"), field="preparation_digest")
    source = applied.get("source")
    if not isinstance(source, Mapping):
        raise ProgramRunOwnerError("Applied Program source identity is invalid")
    _digest(source.get("record_digest"), field="source.record_digest")

    if not isinstance(live_scenario_selection, Mapping):
        raise TypeError("live scenario Program selection must be a mapping")
    for field_name in (
        "assistant_mode",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "retained_bundle_id",
        "scenario_catalog_revision",
        "scenario_id",
        "typed_parameter_overrides",
    ):
        if live_scenario_selection.get(field_name) != applied.get(field_name):
            raise ProgramRunOwnerError(
                "Applied Program and live scenario occurrence disagree"
            )
    owner_identities = live_scenario_selection.get("owner_identities")
    if not isinstance(owner_identities, list):
        raise ProgramRunOwnerError(
            "live scenario selection has no exact owner identities"
        )
    normalized_selection = {
        "assistant_projection": deepcopy(
            live_scenario_selection.get("assistant_projection")
        ),
        "assistant_mode": applied["assistant_mode"],
        "deployment_artifact_id": applied["deployment_artifact_id"],
        "deployment_revision": applied["deployment_revision"],
        "graph_revision": applied["graph_revision"],
        "node_environment_digest": applied["node_environment_digest"],
        "owner_identities": deepcopy(owner_identities),
        "program": deepcopy(applied["program"]),
        "program_binding": deepcopy(applied["program_binding"]),
        "program_binding_id": applied["program_binding_id"],
        "program_id": applied["program_id"],
        "program_revision": applied["program_revision"],
        "retained_bundle_id": applied["retained_bundle_id"],
        "scenario_catalog_revision": applied["scenario_catalog_revision"],
        "scenario_id": applied["scenario_id"],
        "schema_version": SCENARIO_PROGRAM_SELECTION_API,
        "typed_parameter_overrides": deepcopy(
            applied["typed_parameter_overrides"]
        ),
    }
    return build_program_run_admission(
        normalized_selection,
        expected_scenario_id=expected_scenario_id,
        expected_scenario_catalog_revision=expected_scenario_catalog_revision,
        environment=environment,
        owner_availability=owner_availability,
        additional_owner_identities=additional_owner_identities,
    )


__all__ = [
    "APPLIED_PROGRAM_SELECTION_API",
    "PROGRAM_RUN_STATUS_API",
    "SCENARIO_PROGRAM_SELECTION_API",
    "ProgramRunAdmission",
    "ProgramRunOwner",
    "ProgramRunOwnerError",
    "ProgramRunSnapshot",
    "ProgramRunState",
    "build_applied_program_run_admission",
    "build_program_run_admission",
]
