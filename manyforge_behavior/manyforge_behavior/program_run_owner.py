"""Bounded owner for exact Program-run orchestration lifecycles."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from hashlib import sha256
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
    PROGRAM_MOTION_STOP_RECOVERY_CLASSES,
    NodeAdapterPorts,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.program import (
    Program,
    ProgramNodePolicy,
    ProgramValidationError,
    require_exact_program_binding_node_policies,
)
from manyforge_behavior.program_runtime import (
    ExecutableProgramRun,
    ProgramRunCapture,
    ProgramRunOwnerIdentity,
)
from manyforge_behavior.orchestration_values import ProgramValueError


PROGRAM_RUN_STATUS_API: Final = "manyforge.program_run_status.v2"
SCENARIO_PROGRAM_SELECTION_API: Final = "manyforge.scenario_program_selection.v2"
APPLIED_PROGRAM_SELECTION_API: Final = "manyforge.applied_program_selection.v1"
_MAX_RETAINED_RUNS: Final = 4_096
_MAX_NODE_STATUSES: Final = 100_000
_NODE_STATUSES: Final = frozenset({"idle", "running", "success", "failure"})


class ProgramRunOwnerError(RuntimeError):
    """A Program-run owner request is invalid, stale, or unavailable."""


class ProgramRunOwnerOutcomeUnknownError(ProgramRunOwnerError):
    """An addressed owner effect returned without a trustworthy result."""


class ProgramRunState(Enum):
    RUNNING = "running"
    INTERVENTION_REQUIRED = "intervention_required"
    FULL_STOPPING = "full_stopping"
    FULL_STOPPED = "full_stopped"
    STOPPING = "stopping"
    FINALIZING = "finalizing"
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
    applied_record_digest: str
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
    node_policies: tuple[ProgramNodePolicy, ...] = field(init=False)

    def __post_init__(self) -> None:
        _text(self.scenario_id, field="scenario_id", maximum_bytes=128)
        for field_name in (
            "applied_record_digest",
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
        try:
            node_policies = require_exact_program_binding_node_policies(
                self.program,
                binding,
            )
        except ProgramValidationError as exc:
            raise ValueError(str(exc)) from exc
        object.__setattr__(self, "node_policies", node_policies)
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

    def port_context(self, program_run_id: str) -> ProgramRunPortContext:
        """Project the same immutable owner context for preparation or Run."""

        return ProgramRunPortContext(
            program_run_id=program_run_id,
            applied_record_digest=self.applied_record_digest,
            scenario_id=self.scenario_id,
            scenario_catalog_revision=self.scenario_catalog_revision,
            program_id=self.program.program_id,
            program_revision=self.program.program_revision,
            program_binding_id=self.program_binding_id,
            retained_bundle_id=self.retained_bundle_id,
            graph_revision=self.graph_revision,
            node_environment_digest=self.environment.environment_digest,
            program_binding=self.program_binding,
            program=self.program.to_dict(),
            assistant_projection=self.assistant_projection,
        )


@dataclass(frozen=True, slots=True)
class ProgramRunSnapshot:
    owner_id: str
    owner_epoch: str
    run_revision: int
    state: ProgramRunState
    capture: ProgramRunCapture
    tree_status: str
    node_status_revision: int
    node_statuses: Mapping[str, str]
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
        if (
            type(self.node_status_revision) is not int
            or not 1 <= self.node_status_revision
        ):
            raise ValueError("node_status_revision must be positive")
        if (
            not isinstance(self.node_statuses, Mapping)
            or len(self.node_statuses) > _MAX_NODE_STATUSES
        ):
            raise ValueError("node_statuses is outside its bound")
        statuses: dict[str, str] = {}
        for instance_id, status in self.node_statuses.items():
            statuses[_text(instance_id, field="node status instance ID")] = status
            if status not in _NODE_STATUSES:
                raise ValueError("node_statuses contains an invalid status")
        object.__setattr__(self, "node_statuses", MappingProxyType(statuses))
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
            "node_status_revision": self.node_status_revision,
            "node_statuses": dict(self.node_statuses),
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
    node_status_revision: int = 1
    node_statuses: Mapping[str, str] = field(default_factory=dict)
    active_operation_count: int | None = 0
    reason_code: str | None = None
    message: str = ""
    stop_requested: bool = False
    state_after_stop: ProgramRunState = ProgramRunState.STOPPED
    state_after_stop_reason_code: str | None = None
    state_after_stop_message: str = ""
    state_after_finalize: ProgramRunState = ProgramRunState.STOPPED
    state_after_finalize_reason_code: str | None = None
    state_after_finalize_message: str = ""
    resources_released: bool = False
    transition_lock: RLock = field(default_factory=RLock, repr=False)
    motion_stop_lock: RLock = field(default_factory=RLock, repr=False)


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
        self._starting_run_ids: set[str] = set()
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
            node_status_revision=entry.node_status_revision,
            node_statuses=entry.node_statuses,
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

    @staticmethod
    def _mark_finalizing(
        entry: _RunEntry,
        *,
        state_after_finalize: ProgramRunState,
        reason_code: str | None,
        message: str,
    ) -> None:
        entry.state = ProgramRunState.FINALIZING
        entry.state_after_finalize = state_after_finalize
        entry.state_after_finalize_reason_code = reason_code
        entry.state_after_finalize_message = message
        entry.active_operation_count = 0
        entry.reason_code = reason_code
        entry.message = message
        entry.revision += 1

    def _release_resources(self, program_run_id: str) -> tuple[Exception, ...]:
        errors: list[Exception] = []
        context = self._ports.program_run_context
        interventions = self._ports.program_interventions
        if context is not None:
            try:
                context.unbind(program_run_id)
            except Exception as error:
                errors.append(error)
        if interventions is not None:
            try:
                interventions.release(program_run_id)
            except Exception as error:
                errors.append(error)
        return tuple(errors)

    def _reconcile_finalizing(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FINALIZING:
                return self._snapshot(entry)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.FINALIZING:
                    return self._snapshot(entry)
                already_released = entry.resources_released
            errors = () if already_released else self._release_resources(program_run_id)
            with self._lock:
                entry = self._entry(program_run_id)
                if errors:
                    message = "; ".join(
                        str(error) or type(error).__name__ for error in errors
                    )
                    if (
                        entry.reason_code != "program_run_cleanup_failed"
                        or entry.message != message
                    ):
                        entry.reason_code = "program_run_cleanup_failed"
                        entry.message = message
                        entry.revision += 1
                    return self._snapshot(entry)
                entry.resources_released = True
                entry.state = entry.state_after_finalize
                entry.reason_code = entry.state_after_finalize_reason_code
                entry.message = entry.state_after_finalize_message
                entry.active_operation_count = 0
                entry.revision += 1
                return self._snapshot(entry)

    def start(
        self,
        admission: ProgramRunAdmission,
        *,
        program_run_id: str | None = None,
    ) -> ProgramRunSnapshot:
        if not isinstance(admission, ProgramRunAdmission):
            raise TypeError("start requires one exact Program-run admission")
        with self._lock:
            if (
                len(self._runs) + len(self._starting_run_ids)
                >= self._maximum_retained_runs
            ):
                raise ProgramRunOwnerError("Program-run retention is full")
            admitted_program_run_id = _text(
                (
                    self._run_id_factory()
                    if program_run_id is None
                    else program_run_id
                ),
                field="program_run_id",
            )
            if (
                admitted_program_run_id in self._runs
                or admitted_program_run_id in self._starting_run_ids
            ):
                raise ProgramRunOwnerError("Program-run identity was reused")
            self._starting_run_ids.add(admitted_program_run_id)
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
            program_run_id=admitted_program_run_id,
            applied_record_digest=admission.applied_record_digest,
            scenario_id=admission.scenario_id,
            scenario_catalog_revision=admission.scenario_catalog_revision,
            program_id=admission.program.program_id,
            program_revision=admission.program.program_revision,
            program_binding_id=admission.program_binding_id,
            retained_bundle_id=admission.retained_bundle_id,
            graph_revision=admission.graph_revision,
            node_environment_digest=admission.environment.environment_digest,
            owner_identities=owner_identities,
            node_policies=admission.node_policies,
        )
        context_port = self._ports.program_run_context
        context_bound = False
        port_context = admission.port_context(capture.program_run_id)
        try:
            try:
                if context_port is not None:
                    context_port.bind(port_context)
                    context_bound = True
                run = ExecutableProgramRun(
                    program=admission.program,
                    environment=admission.environment,
                    capture=capture,
                    readiness=admission.readiness,
                    ports=self._ports,
                    program_parameter_values=admission.program_parameter_values,
                )
                entry = _RunEntry(run=run, node_statuses=run.node_statuses)
                with self._lock:
                    if admitted_program_run_id in self._runs:
                        raise ProgramRunOwnerError(
                            "Program-run identity was reused"
                        )
                    self._runs[admitted_program_run_id] = entry
                    return self._snapshot(entry)
            except Exception:
                if context_bound and context_port is not None:
                    context_port.unbind(admitted_program_run_id)
                raise
        finally:
            with self._lock:
                self._starting_run_ids.discard(admitted_program_run_id)

    def read(self, program_run_id: str) -> ProgramRunSnapshot:
        """Return the current owner projection without advancing lifecycle."""

        with self._lock:
            return self._snapshot(self._entry(program_run_id))

    def intervention(self, program_run_id: str):
        """Return the exact held node attempt, if any, without deriving it."""

        port = self._ports.program_interventions
        if port is None:
            return None
        return port.read(program_run_id)

    def operator_stop_observation_required(self, program_run_id: str) -> bool:
        """Whether a held run must keep observing owner-issued Stop state.

        Ordinary recovery interventions are dormant until an explicit action.
        Operator Stop is different: continue branches and already-accepted
        owner tasks must remain observable while motion is contained.
        """

        with self._lock:
            if (
                self._entry(program_run_id).state
                is not ProgramRunState.INTERVENTION_REQUIRED
            ):
                return False
        intervention = self.intervention(program_run_id)
        return (
            intervention is not None
            and intervention.policy_parameter == "operator_stop_policy"
        )

    def refresh_operator_stop_intervention(
        self, program_run_id: str
    ) -> None:
        """Refresh one held Stop assessment only when its offer is read."""

        intervention = self.intervention(program_run_id)
        if (
            intervention is None
            or intervention.policy_parameter != "operator_stop_policy"
            or intervention.owner_port_name is None
            or intervention.owner_task_id is None
        ):
            return
        owner_port = getattr(self._ports, intervention.owner_port_name, None)
        observe = getattr(owner_port, "observe", None)
        if not callable(observe):
            return
        try:
            task = observe(intervention.owner_task_id)
        except Exception:
            return
        assessment = task.metadata.get("recovery_assessment")
        if (
            task.task_id != intervention.owner_task_id
            or task.state is not OwnerTaskState.INTERRUPTED
            or not isinstance(assessment, Mapping)
            or assessment.get("recovery_class")
            not in PROGRAM_MOTION_STOP_RECOVERY_CLASSES
        ):
            return
        port = self._ports.program_interventions
        if port is None:
            return
        try:
            port.refresh_owner_assessment(
                intervention.identity,
                assessment,
            )
        except Exception:
            return

    def note_runtime_control_change(
        self, program_run_id: str
    ) -> ProgramRunSnapshot:
        """Advance the run projection after one non-tree C2 state change."""

        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
            return self._snapshot(entry)

    def validate_runtime_override(
        self,
        program_run_id: str,
        *,
        node_instance_id: str,
        node_attempt_id: str,
        parameter_id: str,
        value: Any,
    ) -> Any:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.INTERVENTION_REQUIRED:
                raise ProgramRunOwnerError(
                    "runtime overrides require the exact held Program attempt"
                )
            policy = entry.run.capture.node_policy_by_instance_id.get(
                node_instance_id
            )
            if (
                policy is None
                or policy.modification_policy["runtime_override"] == "forbidden"
                or "runtime_override"
                not in policy.recovery_policy["allowed_recovery_actions"]
            ):
                raise ProgramRunOwnerError(
                    "the captured node policy forbids runtime override"
                )
            intervention = self.intervention(program_run_id)
            if (
                intervention is None
                or intervention.identity.node_instance_id != node_instance_id
                or intervention.identity.node_attempt_id != node_attempt_id
            ):
                raise ProgramRunOwnerError(
                    "runtime override identifies a stale node attempt"
                )
            store = entry.run.runtime_overrides
        try:
            return store.validate(node_instance_id, parameter_id, value)
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error

    def activate_runtime_override(
        self,
        program_run_id: str,
        *,
        node_instance_id: str,
        node_attempt_id: str,
        parameter_id: str,
        value: Any,
    ) -> ProgramRunSnapshot:
        validated = self.validate_runtime_override(
            program_run_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            parameter_id=parameter_id,
            value=value,
        )
        with self._lock:
            entry = self._entry(program_run_id)
            entry.run.runtime_overrides.activate(
                node_instance_id, parameter_id, validated
            )
            entry.revision += 1
            return self._snapshot(entry)

    def validate_runtime_override_promotion(
        self,
        program_run_id: str,
        *,
        node_instance_id: str,
    ) -> str:
        with self._lock:
            entry = self._entry(program_run_id)
            policy = entry.run.capture.node_policy_by_instance_id.get(
                node_instance_id
            )
            if (
                policy is None
                or policy.recovery_policy["persistence_ceiling"] != "draft_patch"
                or policy.modification_policy["persistent_edit"] == "forbidden"
            ):
                raise ProgramRunOwnerError(
                    "captured node policy forbids Draft promotion"
                )
            store = entry.run.runtime_overrides
        try:
            return store.node_id(node_instance_id)
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error

    def runtime_override_source_context(
        self,
        program_run_id: str,
        *,
        node_instance_id: str,
    ) -> dict[str, Any]:
        """Return immutable captured node facts for owner-derived staging."""

        with self._lock:
            entry = self._entry(program_run_id)
            store = entry.run.runtime_overrides
        try:
            return store.node_context(node_instance_id)
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error

    def clear_runtime_override(
        self,
        program_run_id: str,
        *,
        node_instance_id: str,
        parameter_id: str,
    ) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            entry.run.runtime_overrides.clear(node_instance_id, parameter_id)
            entry.revision += 1
            return self._snapshot(entry)

    def write_runtime_blackboard(
        self,
        program_run_id: str,
        *,
        key: str,
        value: Any,
        request_id: str,
    ) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                raise ProgramRunOwnerError(
                    "runtime blackboard writes require a current Program run"
                )
            store = entry.run.value_store
        try:
            store.write(
                key,
                value,
                metadata={
                    "program_run_id": program_run_id,
                    "request_id": request_id,
                    "value_source": "runtime_control",
                },
            )
        except (KeyError, ProgramValueError) as error:
            raise ProgramRunOwnerError(str(error)) from error
        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
            return self._snapshot(entry)

    def resolve_intervention(
        self,
        program_run_id: str,
        *,
        node_attempt_id: str,
        action: str,
        result: Any = None,
    ) -> ProgramRunSnapshot:
        port = self._ports.program_interventions
        if port is None:
            raise ProgramRunOwnerError("Program recovery owner is unavailable")
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.INTERVENTION_REQUIRED:
                raise ProgramRunOwnerError("Program recovery is not current")
            intervention = port.read(program_run_id)
            if (
                intervention is None
                or intervention.identity.node_attempt_id != node_attempt_id
            ):
                raise ProgramRunOwnerError("Program recovery attempt is stale")
            port.resolve(intervention.identity, action=action, result=result)
            entry.state = ProgramRunState.RUNNING
            entry.reason_code = None
            entry.message = ""
            entry.revision += 1
            return self._snapshot(entry)

    def apply_owner_recovery(
        self,
        program_run_id: str,
        *,
        node_attempt_id: str,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> Any:
        """Delegate one exact action to the port that owns the held task."""

        with self._lock:
            entry = self._entry(program_run_id)
            intervention = self.intervention(program_run_id)
            if (
                entry.state is not ProgramRunState.INTERVENTION_REQUIRED
                or intervention is None
                or intervention.identity.node_attempt_id != node_attempt_id
                or intervention.owner_task_id is None
                or intervention.owner_port_name is None
            ):
                raise ProgramRunOwnerError("owner recovery attempt is stale")
            port = getattr(self._ports, intervention.owner_port_name, None)
            recover = getattr(port, "recover", None)
            if not callable(recover):
                raise ProgramRunOwnerError(
                    "held task owner does not expose addressed recovery"
                )
            task_id = intervention.owner_task_id
            identity = intervention.identity
        result = recover(
            task_id,
            identity,
            action=action,
            request_id=request_id,
            reason=reason,
        )
        if not isinstance(result, Mapping) or type(result.get("accepted")) is not bool:
            raise ProgramRunOwnerOutcomeUnknownError(
                "owner recovery returned an invalid result"
            )
        assessment = result.get("resulting_assessment")
        if isinstance(assessment, Mapping):
            port = self._ports.program_interventions
            if port is not None:
                port.refresh_owner_assessment(identity, assessment)
        return result

    def list_snapshots(
        self,
        *,
        active_only: bool = False,
        limit: int = 128,
    ) -> tuple[tuple[ProgramRunSnapshot, ...], bool]:
        """Return one bounded owner-issued index without downstream reads.

        The explicit tick/advance path reconciles a stopping or finalizing
        run. Enumeration and exact ``read`` intentionally take only the local
        owner lock, so a slow Kernel observation cannot delay discovery of
        other runs or the independent Stop path.
        """

        if type(active_only) is not bool:
            raise TypeError("active_only must be boolean")
        if type(limit) is not int or not 1 <= limit <= _MAX_RETAINED_RUNS:
            raise ValueError("Program-run index limit is outside its bound")
        active_states = {
            ProgramRunState.RUNNING,
            ProgramRunState.INTERVENTION_REQUIRED,
            ProgramRunState.FULL_STOPPING,
            ProgramRunState.FULL_STOPPED,
            ProgramRunState.STOPPING,
            ProgramRunState.FINALIZING,
        }
        with self._lock:
            snapshots = tuple(
                self._snapshot(entry)
                for program_run_id, entry in sorted(self._runs.items())
                if not active_only or entry.state in active_states
            )
        return snapshots[:limit], len(snapshots) > limit

    def _begin_stop(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        state_after_stop: ProgramRunState,
        reason_code: str | None,
        message: str,
        stop_request_id: str | None = None,
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
                if entry.state is ProgramRunState.FINALIZING:
                    reconcile_finalize = True
                    issue_stop = False
                    observe_existing_stop = False
                    invalidate_tree = False
                    run = entry.run
                else:
                    reconcile_finalize = False
                    transitioning_from_full_stop = entry.state in {
                        ProgramRunState.FULL_STOPPING,
                        ProgramRunState.FULL_STOPPED,
                    }
                    issue_stop = not entry.stop_requested
                    observe_existing_stop = transitioning_from_full_stop
                    invalidate_tree = issue_stop or transitioning_from_full_stop
                    if issue_stop or transitioning_from_full_stop:
                        entry.stop_requested = True
                        entry.state = ProgramRunState.STOPPING
                        entry.state_after_stop = state_after_stop
                        entry.state_after_stop_reason_code = reason_code
                        entry.state_after_stop_message = message
                        entry.reason_code = reason_code
                        entry.message = message
                        entry.revision += 1
                    run = entry.run

            if reconcile_finalize:
                return self._reconcile_finalizing(program_run_id)

            scope_snapshot: ProgramRunScopeSnapshot | None = None
            scope_errors: list[Exception] = []
            if issue_stop:
                try:
                    scope_snapshot = self._scope.request_stop(
                        program_run_id,
                        stop_request_id or f"owner-transition:{uuid4().hex}",
                        reason,
                    )
                except Exception as exc:  # fail closed; status remains stopping
                    scope_errors.append(exc)
            elif observe_existing_stop:
                try:
                    scope_snapshot = self._scope.observe(program_run_id)
                except Exception as exc:
                    scope_errors.append(exc)
            if invalidate_tree:
                try:
                    # Kernel Stop has already had its independent priority path.
                    # Tree invalidation only prevents later orchestration effects.
                    run.stop()
                    stopped_node_statuses = run.node_statuses
                except Exception as exc:
                    scope_errors.append(exc)
                    stopped_node_statuses = None
            else:
                stopped_node_statuses = None
            scope_fields: tuple[int | None, bool, str | None] | None = None
            if scope_snapshot is not None and not scope_errors:
                try:
                    scope_fields = self._scope_fields(
                        scope_snapshot,
                        program_run_id,
                    )
                except Exception as exc:
                    scope_errors.append(exc)

            finalize = False
            with self._lock:
                entry = self._entry(program_run_id)
                if (
                    state_after_stop is ProgramRunState.STOPPED
                    and stopped_node_statuses is not None
                ):
                    if entry.tree_status != "invalid":
                        entry.tree_status = "invalid"
                        entry.revision += 1
                    if entry.node_statuses != stopped_node_statuses:
                        entry.node_statuses = stopped_node_statuses
                        entry.node_status_revision += 1
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
                        self._mark_finalizing(
                            entry,
                            state_after_finalize=entry.state_after_stop,
                            reason_code=entry.state_after_stop_reason_code,
                            message=entry.state_after_stop_message,
                        )
                        finalize = True
                snapshot = self._snapshot(entry)
            if finalize:
                return self._reconcile_finalizing(program_run_id)
            return snapshot

    def stop(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        return self._begin_stop(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id or f"owner-stop:{uuid4().hex}",
            state_after_stop=ProgramRunState.STOPPED,
            reason_code=None,
            message="",
        )

    def stop_motion(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        """Stop current addressed work without terminating its Program run.

        The Kernel scope already owns the exact operation identities.  Keeping
        this command here avoids exposing those identities to the browser and
        lets the normal tree tick turn the interrupted node into the authored
        operator-Stop intervention. If Stop lands between operations, preserve
        the existing tree at that boundary instead; otherwise a successful
        empty-scope Stop could be followed by a newly submitted operation.
        """

        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        with self._lock:
            entry = self._entry(program_run_id)
            transition_lock = entry.transition_lock
            motion_stop_lock = entry.motion_stop_lock
        with motion_stop_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.RUNNING:
                    raise ProgramRunOwnerError(
                        "motion Stop requires a running Program"
                    )
            stop_identity = (
                stop_request_id or f"owner-motion-stop:{uuid4().hex}"
            )
            try:
                scope_snapshot = self._scope.request_stop(
                    program_run_id,
                    stop_identity,
                    reason,
                )
                active_count, _settled, scope_reason = self._scope_fields(
                    scope_snapshot,
                    program_run_id,
                )
            except Exception as exc:
                raise ProgramRunOwnerOutcomeUnknownError(
                    str(exc) or "Kernel motion Stop outcome is unknown"
                ) from exc
            if scope_reason is not None:
                raise ProgramRunOwnerOutcomeUnknownError(
                    "Kernel motion Stop outcome is unknown: " + scope_reason
                )
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.RUNNING:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Program state changed while motion Stop was applied"
                    )
                entry.active_operation_count = active_count
                if active_count == 0:
                    # The Kernel fence now owns the boundary. A first Execute
                    # still crossing the ordinary lane will be rejected there;
                    # the Program thread never needs to join that lane before
                    # acknowledging Stop.
                    entry.state = ProgramRunState.FULL_STOPPING
                    entry.stop_requested = True
                    entry.reason_code = None
                    entry.message = ""
                entry.revision += 1
                preserve_boundary = entry.state is ProgramRunState.FULL_STOPPING
                snapshot = self._snapshot(entry)
            if not preserve_boundary or not transition_lock.acquire(blocking=False):
                return snapshot
            try:
                return self._complete_full_stop(program_run_id)
            finally:
                transition_lock.release()

    def _complete_full_stop(self, program_run_id: str) -> ProgramRunSnapshot:
        """Freeze a physically settled run without invalidating its tree."""

        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FULL_STOPPING:
                return self._snapshot(entry)
            run = entry.run
        try:
            active, _held, unrecoverable = run.reconcile_full_stop_tasks()
        except Exception as error:
            with self._lock:
                entry = self._entry(program_run_id)
                entry.active_operation_count = None
                entry.reason_code = "program_full_stop_task_state_unknown"
                entry.message = str(error) or type(error).__name__
                return self._snapshot(entry)
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FULL_STOPPING:
                return self._snapshot(entry)
            if active:
                entry.active_operation_count = active
                entry.reason_code = "program_full_stop_task_observation_pending"
                entry.message = (
                    "Full Stop settled physically while accepted Program work "
                    "is still becoming observable"
                )
                return self._snapshot(entry)
            entry.state = ProgramRunState.FULL_STOPPED
            entry.active_operation_count = 0
            if unrecoverable:
                entry.reason_code = "program_full_stop_resume_unavailable"
                entry.message = (
                    "Full Stop settled, but one or more preserved tasks do not "
                    "offer fresh current-state recovery; Reset remains available"
                )
            else:
                entry.reason_code = None
                entry.message = ""
            entry.revision += 1
            return self._snapshot(entry)

    def full_stop(
        self,
        program_run_id: str,
        *,
        reason: str | None = None,
        stop_request_id: str | None = None,
    ) -> ProgramRunSnapshot:
        """Contain the exact run, then preserve its quiesced tree for Run."""

        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        with self._lock:
            transition_lock = self._entry(program_run_id).transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                    ProgramRunState.FINALIZING,
                    ProgramRunState.STOPPING,
                }:
                    return self._snapshot(entry)
                if entry.state is ProgramRunState.FULL_STOPPED:
                    return self._snapshot(entry)
                if entry.state is ProgramRunState.FULL_STOPPING:
                    reconcile = True
                elif entry.state in {
                    ProgramRunState.RUNNING,
                    ProgramRunState.INTERVENTION_REQUIRED,
                }:
                    reconcile = False
                    entry.state = ProgramRunState.FULL_STOPPING
                    entry.stop_requested = True
                    entry.reason_code = None
                    entry.message = ""
                    entry.revision += 1
                else:
                    raise ProgramRunOwnerError(
                        "Full Stop requires a current Program run"
                    )
            if reconcile:
                return self.reconcile_full_stop(program_run_id)
            try:
                scope = self._scope.request_stop(
                    program_run_id,
                    stop_request_id or f"owner-full-stop:{uuid4().hex}",
                    reason,
                )
                active_count, settled, scope_reason = self._scope_fields(
                    scope, program_run_id
                )
            except Exception as error:
                with self._lock:
                    entry = self._entry(program_run_id)
                    entry.active_operation_count = None
                    entry.reason_code = "program_run_scope_unknown"
                    entry.message = str(error) or type(error).__name__
                    return self._snapshot(entry)
            with self._lock:
                entry = self._entry(program_run_id)
                entry.active_operation_count = active_count
                if scope_reason is not None:
                    entry.reason_code = scope_reason
                    entry.message = "Kernel Program-run scope is unknown"
                    settled = False
            return (
                self._complete_full_stop(program_run_id)
                if settled
                else self.read(program_run_id)
            )

    def reconcile_full_stop(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FULL_STOPPING:
                return self._snapshot(entry)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.FULL_STOPPING:
                    return self._snapshot(entry)
            try:
                scope = self._scope.observe(program_run_id)
                active_count, settled, scope_reason = self._scope_fields(
                    scope, program_run_id
                )
            except Exception as error:
                with self._lock:
                    entry = self._entry(program_run_id)
                    entry.active_operation_count = None
                    entry.reason_code = "program_run_scope_unknown"
                    entry.message = str(error) or type(error).__name__
                    return self._snapshot(entry)
            with self._lock:
                entry = self._entry(program_run_id)
                entry.active_operation_count = active_count
                if scope_reason is not None:
                    entry.reason_code = scope_reason
                    entry.message = "Kernel Program-run scope is unknown"
                    settled = False
            return (
                self._complete_full_stop(program_run_id)
                if settled
                else self.read(program_run_id)
            )

    def resume_full_stop(
        self,
        program_run_id: str,
        *,
        request_id: str,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Recover every held occurrence, then schedule only fresh attempts."""

        _text(request_id, field="request_id", maximum_bytes=512)
        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        with self._lock:
            transition_lock = self._entry(program_run_id).transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is ProgramRunState.FULL_STOPPING:
                    raise ProgramRunOwnerError("Full Stop is still settling")
                if entry.state is not ProgramRunState.FULL_STOPPED:
                    raise ProgramRunOwnerError(
                        "Run requires the exact preserved Full Stop occurrence"
                    )
                run = entry.run
                maximum_recoveries = len(entry.node_statuses) + 1
            interventions = self._ports.program_interventions
            for _index in range(maximum_recoveries):
                active, held, unrecoverable = run.reconcile_full_stop_tasks()
                if active:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Full Stop task settlement is not yet observable"
                    )
                if unrecoverable:
                    raise ProgramRunOwnerError(
                        "preserved Full Stop work has no fresh-state recovery"
                    )
                intervention = (
                    None
                    if interventions is None
                    else interventions.read(program_run_id)
                )
                if held == 0:
                    if intervention is not None:
                        raise ProgramRunOwnerOutcomeUnknownError(
                            "Full Stop recovery registry and held tasks disagree"
                        )
                    break
                if (
                    intervention is None
                    or intervention.policy_parameter != "operator_stop_policy"
                    or intervention.owner_port_name is None
                    or intervention.owner_task_id is None
                ):
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Full Stop lost an exact stopped task identity"
                    )
                owner_port = getattr(
                    self._ports, intervention.owner_port_name, None
                )
                recover = getattr(owner_port, "recover", None)
                if not callable(recover):
                    raise ProgramRunOwnerError(
                        "preserved Full Stop task does not support fresh-state recovery"
                    )
                recovery_request_id = "full-stop-resume:" + sha256(
                    (
                        request_id
                        + ":"
                        + intervention.owner_task_id
                    ).encode("utf-8")
                ).hexdigest()
                result = recover(
                    intervention.owner_task_id,
                    intervention.identity,
                    action="replan_from_stopped_state",
                    request_id=recovery_request_id,
                    reason=reason,
                )
                if not isinstance(result, Mapping) or type(
                    result.get("accepted")
                ) is not bool:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Full Stop owner recovery returned an invalid result"
                    )
                if result["accepted"] is not True:
                    error = result.get("error")
                    message = (
                        error.get("message")
                        if isinstance(error, Mapping)
                        and isinstance(error.get("message"), str)
                        else "fresh current-state recovery was rejected"
                    )
                    raise ProgramRunOwnerError(message)
                try:
                    interventions.resolve(
                        intervention.identity,
                        action="retry",
                    )
                    run.reconcile_full_stop_tasks()
                except Exception as error:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "accepted Full Stop recovery was not joined to its tree"
                    ) from error
            else:
                raise ProgramRunOwnerOutcomeUnknownError(
                    "Full Stop recovery exceeded the captured Program-node bound"
                )
            if run.operator_stop_held:
                raise ProgramRunOwnerOutcomeUnknownError(
                    "Full Stop recovery left an unresolved held task"
                )
            released = self._scope.release_stop(program_run_id)
            active_count, settled, scope_reason = self._scope_fields(
                released, program_run_id
            )
            if not settled or scope_reason is not None:
                raise ProgramRunOwnerOutcomeUnknownError(
                    scope_reason or "Full Stop handoff did not release"
                )
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is not ProgramRunState.FULL_STOPPED:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "Program state changed during Full Stop recovery"
                    )
                entry.state = ProgramRunState.RUNNING
                entry.stop_requested = False
                entry.active_operation_count = active_count
                entry.reason_code = None
                entry.message = ""
                entry.revision += 1
                return self._snapshot(entry)

    def _reset_scope_effects(self, program_run_id: str) -> None:
        snapshot = self._scope.reset(program_run_id)
        _active_count, settled, reason = self._scope_fields(
            snapshot, program_run_id
        )
        if not settled:
            raise ProgramRunOwnerError(
                reason or "Kernel Program-run scope did not reset"
            )

    def reset_retained_effects(self, program_run_id: str) -> None:
        """Reset a durably terminal run after its resident tree owner rotated."""

        _text(program_run_id, field="program_run_id")
        self._reset_scope_effects(program_run_id)

    def reset_effects(self, program_run_id: str) -> ProgramRunSnapshot:
        """Release terminal Kernel operations before Program-cycle Reset."""

        with self._lock:
            entry = self._entry(program_run_id)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state not in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                }:
                    raise ProgramRunOwnerError(
                        "Program-run effects can be reset only after settlement"
                    )
            self._reset_scope_effects(program_run_id)
            with self._lock:
                return self._snapshot(self._entry(program_run_id))

    def forget_reset_run(self, program_run_id: str) -> None:
        """Drop one quiescent resident tree after durable Program Reset.

        Durable history belongs to Program Runner's cycle journal.  Keeping the
        executable tree here after its Kernel scope has been reset would turn
        a concurrent-run bound into a process-lifetime command-count limit.
        """

        _text(program_run_id, field="program_run_id")
        with self._lock:
            entry = self._runs.get(program_run_id)
            if entry is None:
                return
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._runs.get(program_run_id)
                if entry is None:
                    return
                if entry.state not in {
                    ProgramRunState.STOPPED,
                    ProgramRunState.SUCCEEDED,
                    ProgramRunState.FAILED,
                }:
                    raise ProgramRunOwnerError(
                        "only a terminal Program run can be forgotten"
                    )
                if not entry.resources_released:
                    raise ProgramRunOwnerError(
                        "Program-run resources are not released"
                    )
                self._runs.pop(program_run_id)

    def reconcile_stop(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.STOPPING:
                return self._snapshot(entry)
            transition_lock = entry.transition_lock
        with transition_lock:
            finalize = False
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
                    self._mark_finalizing(
                        entry,
                        state_after_finalize=entry.state_after_stop,
                        reason_code=entry.state_after_stop_reason_code,
                        message=entry.state_after_stop_message,
                    )
                    finalize = True
                snapshot = self._snapshot(entry)
            if finalize:
                return self._reconcile_finalizing(program_run_id)
            return snapshot

    def advance_once(self, program_run_id: str) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
            transition_lock = entry.transition_lock
        with transition_lock:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is ProgramRunState.STOPPING:
                    reconcile_stop = True
                    reconcile_full_stop = False
                    reconcile_finalize = False
                elif entry.state is ProgramRunState.FULL_STOPPING:
                    reconcile_stop = False
                    reconcile_full_stop = True
                    reconcile_finalize = False
                elif entry.state is ProgramRunState.FINALIZING:
                    reconcile_stop = False
                    reconcile_full_stop = False
                    reconcile_finalize = True
                elif entry.state is ProgramRunState.INTERVENTION_REQUIRED:
                    intervention_port = self._ports.program_interventions
                    intervention = (
                        None
                        if intervention_port is None
                        else intervention_port.read(program_run_id)
                    )
                    if (
                        intervention is None
                        or intervention.policy_parameter
                        != "operator_stop_policy"
                    ):
                        return self._snapshot(entry)
                    reconcile_stop = False
                    reconcile_full_stop = False
                    reconcile_finalize = False
                    run = entry.run
                elif entry.state is not ProgramRunState.RUNNING:
                    return self._snapshot(entry)
                else:
                    reconcile_stop = False
                    reconcile_full_stop = False
                    reconcile_finalize = False
                    run = entry.run
            if reconcile_stop:
                return self.reconcile_stop(program_run_id)
            if reconcile_full_stop:
                return self.reconcile_full_stop(program_run_id)
            if reconcile_finalize:
                return self._reconcile_finalizing(program_run_id)

            try:
                tree_status = run.tick_once()
                node_statuses = run.node_statuses
            except Exception as exc:
                return self._begin_stop(
                    program_run_id,
                    reason="Program runner failure",
                    state_after_stop=ProgramRunState.FAILED,
                    reason_code="program_runtime_failed",
                    message=str(exc) or type(exc).__name__,
                )

            status_name = tree_status.name.lower()
            intervention = None
            intervention_port = self._ports.program_interventions
            if intervention_port is not None:
                try:
                    intervention = intervention_port.read(program_run_id)
                except Exception as exc:
                    return self._begin_stop(
                        program_run_id,
                        reason="Program intervention state unavailable",
                        state_after_stop=ProgramRunState.FAILED,
                        reason_code="program_intervention_state_unknown",
                        message=str(exc) or type(exc).__name__,
                    )
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state not in {
                    ProgramRunState.RUNNING,
                    ProgramRunState.INTERVENTION_REQUIRED,
                }:
                    return self._snapshot(entry)
                if entry.tree_status != status_name:
                    entry.tree_status = status_name
                    entry.revision += 1
                if entry.node_statuses != node_statuses:
                    entry.node_statuses = node_statuses
                    entry.node_status_revision += 1
                if (
                    intervention is not None
                    and entry.state is ProgramRunState.RUNNING
                ):
                    entry.state = ProgramRunState.INTERVENTION_REQUIRED
                    entry.reason_code = intervention.reason_code
                    entry.message = intervention.message
                    entry.revision += 1
                    return self._snapshot(entry)
                if tree_status is py_trees.common.Status.RUNNING:
                    return self._snapshot(entry)

            terminal_state = (
                ProgramRunState.SUCCEEDED
                if tree_status is py_trees.common.Status.SUCCESS
                else ProgramRunState.FAILED
            )
            terminal_reason_code: str | None = None
            terminal_message = ""
            if terminal_state is ProgramRunState.FAILED:
                terminal_reason_code, terminal_message = run.terminal_failure()
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
                    self._mark_finalizing(
                        entry,
                        state_after_finalize=terminal_state,
                        reason_code=terminal_reason_code,
                        message=terminal_message,
                    )
            return self._reconcile_finalizing(program_run_id)


def build_program_run_admission(
    selection: Mapping[str, Any],
    *,
    expected_scenario_id: str,
    expected_scenario_catalog_revision: str,
    environment: NodeEnvironment,
    owner_availability: Mapping[str, AvailabilityFact],
    additional_owner_identities: tuple[ProgramRunOwnerIdentity, ...] = (),
) -> ProgramRunAdmission:
    """Join one exact package selection to a compatible Python runner."""

    if not isinstance(selection, Mapping):
        raise TypeError("scenario Program selection must be a mapping")
    expected_fields = {
        "applied_record_digest",
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
    if not isinstance(binding_environment, dict) or set(binding_environment) != {
        "catalog_id",
        "content_version",
        "catalog_digest",
        "environment_digest",
    }:
        raise ProgramRunOwnerError(
            "scenario Program Binding has an invalid node environment"
        )
    if (
        binding_environment.get("catalog_id")
        != environment.manifest.catalog_id
        or binding_environment.get("content_version")
        != environment.manifest.catalog_content_version
    ):
        raise ProgramRunOwnerError(
            "scenario Program catalog contract is unsupported by this runner"
        )
    if (
        program.program_id != program_id
        or program.program_revision != program_revision
        or binding_value.get("program_binding_id") != program_binding_id
        or binding_value.get("program_id") != program_id
        or binding_value.get("program_revision") != program_revision
        or binding_value.get("retained_bundle_id") != retained_bundle_id
        or binding_value.get("graph_revision") != graph_revision
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
        applied_record_digest=_digest(
            selection.get("applied_record_digest"),
            field="applied_record_digest",
        ),
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
        "applied_record_digest": source["record_digest"],
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
    "ProgramRunOwnerOutcomeUnknownError",
    "ProgramRunSnapshot",
    "ProgramRunState",
    "build_applied_program_run_admission",
    "build_program_run_admission",
]
