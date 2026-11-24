"""Bounded owner for exact Program-run orchestration lifecycles."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
import logging
from threading import Event, RLock
import time
from types import MappingProxyType
from typing import Any, Final

import py_trees

from manyforge_behavior.failure_details import (
    failure_origin,
    failure_wire_fields,
    require_failure_details,
    require_failure_origin,
)
from manyforge_behavior.node_availability import NodeExecutionReadiness
from manyforge_behavior.node_availability import AvailabilityFact
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.orchestration_ports import (
    PROGRAM_MOTION_STOP_RECOVERY_CLASS,
    recovery_outcome,
    NodeAdapterPorts,
    OwnerTaskSettlementPendingError,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
    ProgramSelectionReleaseRejectedError,
    require_run_number,
)
from manyforge_behavior.request_keys import RUNNER_KEYS, format_request_key
from manyforge_behavior.program import (
    Program,
    ProgramNodePolicy,
    ProgramValidationError,
    require_exact_program_binding_node_policies,
)
from manyforge_behavior.program_frame_facts import ProgramFrameFacts
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.program_runtime import (
    ExecutableProgramRun,
    ProgramRunCapture,
    ProgramRunOwnerIdentity,
    ProgramRuntimeError,
)
from manyforge_behavior.orchestration_values import ProgramValueError, ProgramValueStore


PROGRAM_RUN_STATUS_API: Final = "manyforge.program_run_status.v5"
PROGRAM_DIAGNOSTIC_API: Final = "manyforge.program_diagnostic.v5"
PROGRAM_RUN_INPUT_API: Final = "manyforge.program_run_input.v3"
APPLIED_PROGRAM_SELECTION_API: Final = "manyforge.applied_program_selection.v5"
_MAX_RETAINED_RUNS: Final = 4_096
_MAX_NODE_STATUSES: Final = 100_000
_NODE_STATUSES: Final = frozenset(
    {"idle", "running", "held", "success", "failure", "interrupted"}
)
_FAILURE_PAUSE_UNCONFIRMED: Final = (
    "Stopping the Program's other motion is not confirmed yet."
)
# The runner itself ended or paused the run (scope, settlement, runtime).
_RUNNER_FAILURE: Final = failure_origin("program_runner")
_LOGGER = logging.getLogger(__name__)


class ProgramRunOwnerError(RuntimeError):
    """A Program-run owner request is invalid, stale, or unavailable."""


class ProgramRunOwnerStaleRevisionError(ProgramRunOwnerError):
    """A revision-fenced request was rejected before any owner effect."""

    code = "program_runtime_control_stale"

    def __init__(self, expected: int, current: int) -> None:
        super().__init__("Program run changed before manual handover admission")
        self.details = {
            "expected_run_revision": expected,
            "current_run_revision": current,
        }


def _release_refused(error: ProgramSelectionReleaseRejectedError) -> ProgramRunOwnerError:
    """Keep the Kernel's stable refusal code and details for the caller."""
    refused = ProgramRunOwnerError(str(error))
    if error.code is not None:
        refused.code = error.code  # type: ignore[attr-defined]
        refused.details = dict(error.details)  # type: ignore[attr-defined]
    return refused


class ProgramRunOwnerOutcomeUnknownError(ProgramRunOwnerError):
    """An addressed owner effect returned without a trustworthy result."""


class ProgramRunOwnerRecoveryIncompleteError(ProgramRunOwnerError):
    """Some cleanup succeeded; the selected hold was not released."""


PROGRAM_RESET_STOP_REASON = "operator requested Program cycle Reset"
# A partial or failed Reset Stop leaves activity unknown; the cell latch
# blocks execution until every component reports idle, so Reset completes.
PROGRAM_RUN_STOP_UNCONTAINED_REASON_CODES: Final = frozenset(
    {"program_run_stop_partial", "program_run_stop_failed"}
)


RUN_FROZEN_LEVELS: Final = frozenset({"none", "halt", "runner_loss", "program_end"})
# Applied kind tokens of a Stop that freezes a run (the Halt's cause).
RUN_FROZEN_KINDS: Final = frozenset(
    {"halt", "internal_fault", "stop_failed_halt", "runner_loss", "planned_stop", "program_end"}
)
_MAX_SAFE: Final = 9_007_199_254_740_991


def _optional_counter(value: object, *, field: str) -> int | None:
    if value is not None and (type(value) is not int or not 1 <= value <= _MAX_SAFE):
        raise ValueError(f"{field} is invalid")
    return value


@dataclass(frozen=True, slots=True)
class ProgramRunHoldFacts:
    """The Kernel's hold state of the claimed run (``program_run_hold`` of the
    event stream's ``state``) with the cell-wide Stop markers of the same
    message (``last_stop_event_id``, ``stop_progress_sequence``).

    Level-triggered: the runner keeps only the latest facts per run and
    reconciles its own state against them on the run's tick.
    """

    program_run_id: int
    hold_revision: int
    frozen: str
    frozen_by: int | None
    all_groups_held: bool
    held_by_rule: str | None
    last_stop_event_id: int | None
    stop_progress_sequence: int
    # The freezing Stop's applied kind and source label (the Halt's cause).
    frozen_kind: str | None = None
    frozen_source_label: str | None = None

    @classmethod
    def from_wire(
        cls,
        hold: object,
        *,
        last_stop_event_id: object,
        stop_progress_sequence: object,
    ) -> ProgramRunHoldFacts:
        if not isinstance(hold, Mapping) or set(hold) != {
            "program_run_id",
            "hold_revision",
            "frozen",
            "frozen_by",
            "all_groups_held",
            "held_by_rule",
            "frozen_kind",
            "frozen_source_label",
        }:
            raise ValueError("program_run_hold facts are malformed")
        kind, label = hold["frozen_kind"], hold["frozen_source_label"]
        if kind is not None and kind not in RUN_FROZEN_KINDS:
            raise ValueError("program_run_hold frozen_kind is invalid")
        if label is not None and type(label) is not str:
            raise ValueError("program_run_hold frozen_source_label is invalid")
        revision = hold["hold_revision"]
        if type(revision) is not int or not 0 <= revision <= _MAX_SAFE:
            raise ValueError("program_run_hold hold_revision is invalid")
        if hold["frozen"] not in RUN_FROZEN_LEVELS:
            raise ValueError("program_run_hold frozen level is invalid")
        if type(hold["all_groups_held"]) is not bool:
            raise ValueError("program_run_hold all_groups_held is invalid")
        rule = hold["held_by_rule"]
        if rule is not None and (type(rule) is not str or not rule):
            raise ValueError("program_run_hold held_by_rule is invalid")
        if (
            type(stop_progress_sequence) is not int
            or not 0 <= stop_progress_sequence <= _MAX_SAFE
        ):
            raise ValueError("stop_progress_sequence is invalid")
        return cls(
            program_run_id=require_run_number(hold["program_run_id"]),
            hold_revision=revision,
            frozen=hold["frozen"],
            frozen_by=_optional_counter(hold["frozen_by"], field="frozen_by"),
            all_groups_held=hold["all_groups_held"],
            held_by_rule=rule,
            last_stop_event_id=_optional_counter(
                last_stop_event_id, field="last_stop_event_id"
            ),
            stop_progress_sequence=stop_progress_sequence,
            frozen_kind=kind,
            frozen_source_label=label,
        )

    def halt_cause(self) -> dict[str, Any] | None:
        """The run status member ``halt`` these facts give a halting or
        halted run: null without a freezing kind."""

        if self.frozen_kind is None:
            return None
        return {"kind": self.frozen_kind, "source_label": self.frozen_source_label}

    def reconciliation_key(self) -> tuple[object, ...]:
        """The fields whose change makes the run re-read its scope (§6.3.2):
        a participant moving from stopping to faulted raises no hold
        revision but raises ``stop_progress_sequence``."""

        return (
            self.hold_revision,
            self.frozen,
            self.frozen_by,
            self.all_groups_held,
            self.held_by_rule,
            self.stop_progress_sequence,
        )


class ProgramRunState(Enum):
    RUNNING = "running"
    INTERVENTION_REQUIRED = "intervention_required"
    HALTING = "halting"
    HALTED = "halted"
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


def _runtime_scale_selector_for_group(
    run: ExecutableProgramRun,
    program_binding: Mapping[str, Any],
    group_id: str,
) -> tuple[dict[str, Any] | None, str | None]:
    """Project one captured group to the exact runtime-scale selector."""

    selected_targets = run.execution_selection_motion_targets(group_id)
    bindings = program_binding.get("target_bindings", ())
    targets: set[tuple[str, str]] = set()
    for component, command_target in selected_targets:
        matches = [
            row for row in bindings if isinstance(row, Mapping)
            and row.get("target") == {
                "component_id": component, "command_target_id": command_target
            }
        ]
        if len(matches) != 1:
            return None, "captured_motion_target_binding_unavailable"
        targets.add((component, command_target))
    if not targets:
        return None, "selection_has_no_motion_targets"
    ordered = [
        {"component_id": component, "command_target_id": target}
        for component, target in sorted(targets)
    ]
    selector: dict[str, Any] = {
        "program_binding_id": run.capture.program_binding_id,
    }
    if len(ordered) == 1:
        selector.update(ordered[0])
    else:
        selector["group_targets"] = ordered
    return selector, None


def _halt_phase(state: ProgramRunState) -> str | None:
    """``halting`` or ``halted`` while a Halt freezes the run, else None."""

    if state in {ProgramRunState.HALTING, ProgramRunState.HALTED}:
        return state.value
    return None


def _selection_group_state(
    nodes: Sequence[str],
    facts: _TickFacts,
    unavailable: str | None,
    blocked: Mapping[str, str],
    halt: str | None = None,
) -> tuple[str, str | None, list[str], list[str]]:
    """One motion group's state and release reason from its run's tick
    facts: the runtime controls' ``groups[].state`` and the
    ``motion_group_state.<group>`` key (OD-339) both come from here.
    Also returns the group's held and ordinary-held (stopped) nodes.

    ``halt`` (``_halt_phase``): a Halt freezes every group, whatever its
    node holds: ``stopping`` while it settles, ``held`` once halted (a
    group with a faulted Stop stays ``faulted``). Run and Resume stay
    unavailable (``unavailable`` names why); Execute continues."""

    snapshot = facts.scope
    held, ready = snapshot.held_nodes, snapshot.ready_nodes
    selected_held = [node for node in nodes if node in held]
    selected_ready = [node for node in nodes if node in ready]
    selected_effectful = [node for node in nodes if node in facts.initial_ready_nodes]
    selected_ordinary_held = [node for node in selected_held if node not in ready]
    reason = unavailable
    if any(node in snapshot.faulted_nodes for node in selected_held) and (
        halt is not None or reason is None
    ):
        state, reason = "faulted", "selected_stop_fault_requires_recovery"
    elif halt is not None:
        state = "stopping" if halt == "halting" else "held"
    elif reason is not None:
        state = "unavailable"
    elif any(node in snapshot.stopping_nodes for node in selected_held):
        state, reason = "stopping", "selected_motion_is_stopping"
    elif selected_ready:
        state = (
            "ready"
            if len(selected_ready) == len(selected_effectful)
            and not selected_ordinary_held
            else "mixed"
        )
        # Run and Resume are one release; stopped work in a mixed
        # group still needs its recovery to be available.
        reason = next(
            (blocked[node] for node in selected_held if node in blocked),
            None,
        )
    else:
        state = (
            "running"
            if not selected_held
            else "held"
            if len(selected_ordinary_held) == len(nodes)
            else "mixed"
        )
        reason = next(
            (blocked[node] for node in selected_held if node in blocked),
            None,
        )
        if not selected_held:
            reason = "selection_not_held"
    return state, reason, selected_held, selected_ordinary_held


def _program_stop_fields(nodes: Sequence[str], facts: _TickFacts) -> dict[str, str | None]:
    """A group row's ``stopped_by`` and ``stopped_by_node_instance_id``
    (OD-342), from the Kernel's attribution of held nodes: `program` when
    the whole group is held and every stopped (not Ready) node of it is held
    by a Program node's own Stop; it names the first such node's requester
    (node order)."""

    held, ready = facts.scope.held_nodes, facts.scope.ready_nodes
    holds = facts.program_holds
    stopped = [node for node in sorted(nodes) if node not in ready]
    node = (
        holds[stopped[0]]
        if stopped
        and all(member in held for member in nodes)
        and all(member in holds for member in stopped)
        else None
    )
    return {
        "stopped_by": None if node is None else "program",
        "stopped_by_node_instance_id": node,
    }


def _selection_resource_ids(
    run: ExecutableProgramRun,
    program_binding: Mapping[str, Any],
    group_id: str,
) -> list[str]:
    """The exact resources the Kernel reserves for one selection's release.

    The Kernel's rule: every effectful target effect of the
    selection's nodes, resolved through the sealed Program Binding. Composer
    tests operator-alarm motion attention against this set; the Kernel stays
    the enforcement point.
    """

    selected_targets = set(run.execution_selection_effect_targets(group_id))
    resources: set[str] = set()
    for target in program_binding.get("target_bindings", ()):
        if not isinstance(target, Mapping) or not isinstance(target.get("target"), Mapping):
            continue
        identity = target["target"]
        if (identity.get("component_id"), identity.get("command_target_id")) not in selected_targets:
            continue
        for resource_role in target.get("resource_role_bindings", ()):
            if isinstance(resource_role, Mapping):
                resources.update(
                    resource
                    for resource in resource_role.get("resource_ids", ())
                    if isinstance(resource, str)
                )
    return sorted(resources)


@dataclass(frozen=True, slots=True)
class ProgramRunAdmission:
    """One exact verified scenario closure admitted before tree construction.

    ``project_path`` is the Applied Scenario project folder; it scopes the
    run's persistent blackboard values.
    """

    scenario_id: str
    project_path: str
    applied_record_digest: str
    source_document_digest: str
    program: Program
    environment: NodeEnvironment
    program_binding_id: str
    retained_bundle_id: str
    graph_revision: str
    program_binding: Mapping[str, Any]
    readiness: NodeExecutionReadiness
    owner_identities: tuple[ProgramRunOwnerIdentity, ...]
    assistant_projection: Mapping[str, Any] | None = None
    simulation_scene: Mapping[str, Any] | None = None
    node_policies: tuple[ProgramNodePolicy, ...] = field(init=False)

    def __post_init__(self) -> None:
        _text(self.scenario_id, field="scenario_id", maximum_bytes=128)
        _text(self.project_path, field="project_path", maximum_bytes=4096)
        for field_name in (
            "applied_record_digest",
            "source_document_digest",
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
            binding.get("program_revision") != self.program.program_revision
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
                    key=lambda identity: identity.role,
                )
            )
            != self.owner_identities
            or len({identity.role for identity in self.owner_identities})
            != len(self.owner_identities)
            or any(
                identity.role == "program_runner"
                for identity in self.owner_identities
            )
        ):
            raise ValueError(
                "admitted owner identities must be unique, role-sorted, and "
                "exclude the local Program runner"
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
                else MappingProxyType(deepcopy(dict(self.assistant_projection)))
            ),
        )
        if self.simulation_scene is not None and not isinstance(
            self.simulation_scene, Mapping
        ):
            raise TypeError("simulation_scene must be absent or one mapping")
        object.__setattr__(
            self,
            "simulation_scene",
            (
                None
                if self.simulation_scene is None
                else MappingProxyType(deepcopy(dict(self.simulation_scene)))
            ),
        )

    def port_context(
        self, program_run_id: int, *, context_kind: str = "run"
    ) -> ProgramRunPortContext:
        """Project the same immutable owner context for preparation or Run.

        A scene preparation that belongs to no run passes its Apply number
        with ``context_kind="apply"``."""

        return ProgramRunPortContext(
            program_run_id=program_run_id,
            context_kind=context_kind,
            applied_record_digest=self.applied_record_digest,
            scenario_id=self.scenario_id,
            source_document_digest=self.source_document_digest,
            program_revision=self.program.program_revision,
            program_binding_id=self.program_binding_id,
            retained_bundle_id=self.retained_bundle_id,
            graph_revision=self.graph_revision,
            node_environment_digest=self.environment.environment_digest,
            program_binding=self.program_binding,
            program=self.program.to_dict(),
            assistant_projection=self.assistant_projection,
            simulation_scene=self.simulation_scene,
        )


def _rule_stop(value: Any) -> str | None:
    """The ``rule_id`` of a Kernel fact ``stopped_by: rule`` inside ``value``
    (a held or stopped operation's facts), or None."""

    pending = [value]
    while pending:
        item = pending.pop()
        if isinstance(item, Mapping):
            if item.get("stopped_by") == "rule" and isinstance(item.get("rule_id"), str):
                return item["rule_id"]
            pending.extend(item.values())
        elif isinstance(item, (list, tuple)):
            pending.extend(item)
    return None


def program_run_end(snapshot: "ProgramRunSnapshot") -> dict[str, Any] | None:
    """``ProgramRunEnd`` of a terminal run, else None.

    ``completed`` when the root succeeded; ``failed`` names the node and its
    error; ``stopped`` names the operator or the rule whose Stop held the run.
    ``cell_fault`` needs the cell latch, which the Runner reads at the end.
    """

    if snapshot.state is ProgramRunState.SUCCEEDED:
        return {"end_reason": "completed", "details": {}}
    if snapshot.state is ProgramRunState.FAILED:
        details: dict[str, Any] = {
            "error": {"code": snapshot.reason_code or "program_tree_failed",
                      "message": snapshot.message},
        }
        node = None if snapshot.origin is None else snapshot.origin.get("node_instance_id")
        if node is not None:
            details["node_instance_id"] = node
        return {"end_reason": "failed", "details": details}
    if snapshot.state is ProgramRunState.STOPPED:
        rule_id = snapshot.stopped_by_rule_id or _rule_stop(
            [snapshot.details, list(snapshot.operation_failures)]
        )
        return {
            "end_reason": "stopped",
            "details": (
                {"stopped_by": "operator"}
                if rule_id is None
                else {"stopped_by": "rule", "rule_id": rule_id}
            ),
        }
    return None


@dataclass(frozen=True, slots=True)
class ProgramRunSnapshot:
    owner_id: str
    run_revision: int
    state: ProgramRunState
    capture: ProgramRunCapture
    tree_status: str
    node_status_revision: int
    node_statuses: Mapping[str, str]
    active_operation_count: int | None
    reason_code: str | None
    message: str
    # Bounded details and origin of a failure; None for any other reason
    # (a Stop, a success) and when the failure carries none.
    details: Mapping[str, Any] | None
    origin: Mapping[str, Any] | None
    # Operator-facing operation failures (held, or of a failed/stopped run
    # until Reset): node attempt, operation and the Kernel's own facts.
    operation_failures: tuple[Mapping[str, Any], ...] = ()
    # The global rule whose `hold` holds the run (the Kernel scope's
    # `stopped_by: rule`); a stopped run's end names it.
    stopped_by_rule_id: str | None = None
    # The Halt's cause while the run is halting or halted:
    # {"kind": <frozen kind token>, "source_label": <text or None>}.
    halt: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        _text(self.owner_id, field="owner_id")
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
        object.__setattr__(self, "details", require_failure_details(self.details))
        object.__setattr__(self, "origin", require_failure_origin(self.origin))
        if self.origin is not None and self.reason_code is None:
            raise ValueError("a failure origin requires its reason code")
        if self.details is not None and self.origin is None:
            raise ValueError("failure details require their origin")
        if self.stopped_by_rule_id is not None:
            _text(self.stopped_by_rule_id, field="stopped_by_rule_id")

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": PROGRAM_RUN_STATUS_API,
            "owner_id": self.owner_id,
            "run_revision": self.run_revision,
            "state": self.state.value,
            "capture": self.capture.to_dict(),
            "tree_status": self.tree_status,
            "node_status_revision": self.node_status_revision,
            "node_statuses": dict(self.node_statuses),
            "active_operation_count": self.active_operation_count,
            "reason_code": self.reason_code,
            "message": self.message,
            **failure_wire_fields(self.details, self.origin),
            # Always present (empty when none): a missing key is unknown.
            "operation_failures": deepcopy([dict(row) for row in self.operation_failures]),
            "run_end": program_run_end(self),
            "halt": None if self.halt is None else dict(self.halt),
        }


@dataclass(frozen=True, slots=True)
class SentStop:
    """A Forced Program End marked and sent (``ProgramRunOwner.send_stop``);
    what its tree part on the run's tick still has to apply."""

    state_after_stop: ProgramRunState
    # The run had already ended: nothing to apply.
    ended: bool = False
    # The run was already finalizing: the tick finishes that.
    reconcile_finalize: bool = False
    invalidate_tree: bool = False
    scope_snapshot: ProgramRunScopeSnapshot | None = None
    scope_errors: tuple[Exception, ...] = ()


class RunIntent:
    """One command handed to its run's tick (OD-146).

    The tick takes the run's intents at its start, in posting order, applies
    each one and hands back its result or its error. The request that posted
    it waits for that (``ProgramRunTickHost``).
    """

    __slots__ = ("_apply", "done", "result", "error")

    def __init__(self, apply: Callable[[], Any]) -> None:
        self._apply = apply
        self.done = Event()
        self.result: Any = None
        self.error: Exception | None = None


@dataclass(frozen=True, slots=True)
class _TickFacts:
    """The selection facts of a run as its tick last left them: published
    under the owner lock for the threads that do not tick the run (the
    status read behind the panel, the scheduler)."""

    scope: Any
    # What blocked a selection release in the run's latest Kernel scope read
    # (OD-152 (a)).
    selection_release_block_reason: str | None
    stop_observation_error: str | None
    initial_ready_nodes: frozenset[str]
    # The retained owner-task states of the held nodes; None when they could
    # not be read.
    held_task_states: Mapping[str, str] | None
    # ``ExecutableProgramRun.pending_selected_observation``.
    stop_observation_pending: bool
    # The open operator questions (``ExecutableProgramRun.operator_questions``).
    operator_questions: tuple[Mapping[str, Any], ...] = ()
    # ``ExecutableProgramRun.program_holds``: the Kernel's `stopped_by:
    # program` attribution of held nodes.
    program_holds: Mapping[str, str] = field(default_factory=dict)


@dataclass(slots=True)
class _RunEntry:
    run: ExecutableProgramRun
    program_binding: Mapping[str, Any]
    state: ProgramRunState = ProgramRunState.RUNNING
    revision: int = 1
    # The held-node mask last carried by a run_revision.
    mock_held: tuple[str, ...] = ()
    tree_status: str = "invalid"
    node_status_revision: int = 1
    node_statuses: Mapping[str, str] = field(default_factory=dict)
    # Audit facts of automatic motion retries decided since the runner's last take.
    pending_node_history_events: list[dict[str, Any]] = field(default_factory=list)
    active_operation_count: int | None = 0
    reason_code: str | None = None
    message: str = ""
    details: Mapping[str, Any] | None = None
    origin: Mapping[str, Any] | None = None
    pending_diagnostic: dict[str, Any] | None = None
    stop_requested: bool = False
    # A failure pause stops the Program's other motion, like Halt. Set when
    # the pause is entered; sent by the tick after its tree work.
    failure_pause_stop_pending: bool = False
    # Class 1 runner keys stored on the run, so a resend reuses its key:
    # the Forced Program End the owner starts itself, and the internal
    # motion Stops of one run revision ((revision, key) per kind).
    forced_end_key: bytes | None = None
    simulation_stop_key: tuple[int, bytes] | None = None
    failure_pause_stop_key: tuple[int, bytes] | None = None
    # Each held node is its own failure hold. The tree is dormant from a new
    # hold until the operator releases one; the others stay held meanwhile.
    failure_paused: bool = False
    # The hold revision the Kernel returned for the runner's own last
    # accepted release (Run, Resume, Execute after Halt): hold facts below it
    # describe the state before that release and are ignored.
    own_release_revision: int = 0
    # The reconciliation key of the last hold facts applied on a tick.
    applied_hold_facts: tuple[object, ...] | None = None
    # The cause of the freeze in the last hold facts applied (``halt``).
    halt_cause: Mapping[str, Any] | None = None
    # Stop-interrupted task replans: (Resume request key, node attempt) ->
    # the replan's class 1 key, reused when that request is sent again.
    replan_keys: dict[tuple[bytes, int], bytes] = field(default_factory=dict)
    known_holds: frozenset[str] = frozenset()
    # Operator-facing operation failures by hold attempt ("" for the failed
    # tree); a row leaves with its released hold, the rest at Reset.
    operation_failures: dict[str, dict[str, Any]] = field(default_factory=dict)
    state_after_stop: ProgramRunState = ProgramRunState.STOPPED
    state_after_stop_reason_code: str | None = None
    state_after_stop_message: str = ""
    state_after_stop_details: Mapping[str, Any] | None = None
    state_after_stop_origin: Mapping[str, Any] | None = None
    state_after_finalize: ProgramRunState = ProgramRunState.STOPPED
    state_after_finalize_reason_code: str | None = None
    state_after_finalize_message: str = ""
    state_after_finalize_details: Mapping[str, Any] | None = None
    state_after_finalize_origin: Mapping[str, Any] | None = None
    resources_released: bool = False
    # The last cleanup attempt failed (OD-279): the tick does not retry it;
    # only a Reset does (``retry_cleanup``), once per Reset.
    cleanup_failed: bool = False
    reset_containment: ProgramRunScopeSnapshot | None = None
    # Preserve the exact terminal scope sample; the public terminal snapshot
    # normalizes its count to zero after settlement.
    terminal_settlement_active_operation_count: int | None = None
    # Bound to each Reset-owned Stop occurrence before its Stop is requested.
    reset_containment_pre_stop_active_operation_count: int | None = None
    # Commands handed to the run's tick, in posting order (OD-146).
    inbox: deque[RunIntent] = field(default_factory=deque, repr=False)
    tick_facts: _TickFacts | None = None
    # The (eligible, tick facts) the motion group state keys were last
    # written from.
    group_state_inputs: tuple[ProgramRunState, _TickFacts] | None = None


class ProgramRunOwner:
    """Own orchestration state while Kernel and focused ports own effects.

    Each run's tree belongs to its tick (OD-146). The commands that change
    the tree are handed to the tick (``post_to_tick``); the tick-side
    methods (``advance_once``, ``resume_motion``, ``start_motion``,
    ``prepare_manual_handover``, ``retry_failure_pause``,
    ``continue_failure_pause``, ``resume_halt``, ``finish_stop``,
    ``begin_reset_containment``, ``stop_motion``) run only on it. The owner
    lock guards memory only.
    """

    def __init__(
        self,
        *,
        owner_id: str,
        application_start_id: str,
        ports: NodeAdapterPorts,
        maximum_retained_runs: int = _MAX_RETAINED_RUNS,
        program_clock_source: Callable[[], float] = time.monotonic,
    ) -> None:
        self._owner_id = _text(owner_id, field="owner_id")
        self._application_start_id = _text(
            application_start_id, field="application_start_id"
        )
        if not isinstance(ports, NodeAdapterPorts):
            raise TypeError("ports must be one focused owner-port set")
        if ports.program_run_scope is None:
            raise ValueError("Program-run owner requires one Kernel scope port")
        if (
            type(maximum_retained_runs) is not int
            or not 1 <= maximum_retained_runs <= _MAX_RETAINED_RUNS
        ):
            raise ValueError("maximum_retained_runs is outside its bound")
        if not callable(program_clock_source):
            raise TypeError("program_clock_source must be a monotonic clock")
        self._program_clock_source = program_clock_source
        self._ports = ports
        self._scope = ports.program_run_scope
        self._maximum_retained_runs = maximum_retained_runs
        self._runs: dict[str, _RunEntry] = {}
        self._starting_run_ids: set[str] = set()
        self._lock = RLock()

    def _entry(self, program_run_id: int) -> _RunEntry:
        require_run_number(program_run_id)
        try:
            return self._runs[program_run_id]
        except KeyError as exc:
            raise ProgramRunOwnerError("Program run does not exist") from exc

    def post_to_tick(
        self, program_run_id: int, apply: Callable[[], Any]
    ) -> RunIntent:
        """Hand one command to the run's tick (OD-146); memory only. The
        caller wakes the run."""

        intent = RunIntent(apply)
        with self._lock:
            self._entry(program_run_id).inbox.append(intent)
        return intent

    def withdraw_from_tick(self, program_run_id: int, intent: RunIntent) -> bool:
        """Take back an intent the tick has not taken yet (its run could not
        be woken). False when the tick already took it."""

        with self._lock:
            inbox = self._entry(program_run_id).inbox
            if intent not in inbox:
                return False
            inbox.remove(intent)
            return True

    def _apply_tick_intents(self, program_run_id: int) -> None:
        """On the run's tick, at its start: apply the intents posted since
        the last tick, in posting order. Each answer is handed back after
        the facts it changed are published."""

        with self._lock:
            entry = self._entry(program_run_id)
            if not entry.inbox:
                return
            intents, entry.inbox = entry.inbox, deque()
        while intents:
            intent = intents[0]
            try:
                intent.result = intent._apply()
            except Exception as error:
                intent.error = error
            try:
                self._publish_tick_facts(program_run_id)
            except Exception:
                # The tick fails (a worker failure). This intent and the ones
                # not applied yet go back to the inbox; the tick host answers
                # them with its typed failure once no tick runs.
                intent.result = None
                intent.error = None
                with self._lock:
                    entry.inbox.extendleft(reversed(intents))
                raise
            intents.popleft()
            intent.done.set()

    def fail_tick_intents(self, error: Callable[[], Exception | None]) -> None:
        """Once no tick runs any more: answer every intent still handed to a
        tick with the tick host's typed refusal (OD-146)."""

        with self._lock:
            intents = [
                intent for entry in self._runs.values() for intent in entry.inbox
            ]
            for entry in self._runs.values():
                entry.inbox = deque()
        for intent in intents:
            intent.error = error()
            intent.done.set()

    def _tick_facts(self, run: ExecutableProgramRun) -> _TickFacts:
        """On the run's tick: its selection facts for the other threads."""

        scope = run.operator_stop_scope()
        try:
            held_task_states: Mapping[str, str] | None = (
                {}
                if not scope.held_nodes
                else run.task_control_states(node_ids=scope.held_nodes, observe=False)
            )
        except Exception:
            held_task_states = None
        return _TickFacts(
            scope=scope,
            selection_release_block_reason=run.selection_release_block_reason,
            stop_observation_error=run.stop_observation_error,
            initial_ready_nodes=run.initial_ready_nodes(),
            held_task_states=held_task_states,
            stop_observation_pending=run.pending_selected_observation(),
            operator_questions=run.operator_questions(),
            program_holds=run.program_holds,
        )

    def _publish_tick_facts(self, program_run_id: int) -> None:
        with self._lock:
            run = self._entry(program_run_id).run
        facts = self._tick_facts(run)
        with self._lock:
            entry = self._entry(program_run_id)
            entry.tick_facts = facts
        self._publish_motion_group_states(program_run_id)

    def operator_questions(self, program_run_id: int) -> tuple[Mapping[str, Any], ...]:
        """The run's open operator questions as its tick last published them."""

        with self._lock:
            facts = self._entry(program_run_id).tick_facts
        return () if facts is None else facts.operator_questions

    def answer_operator_question(
        self,
        program_run_id: int,
        *,
        node_instance_id: str,
        node_attempt_id: int,
        option_id: str,
    ) -> Mapping[str, Any]:
        """On the run's tick: hand the operator's answer to its question.

        No Kernel effect; the node ends on its next tick. Returns the chosen
        option; a question no longer open is refused
        (``cycle_recovery_offer_stale``)."""

        with self._lock:
            entry = self._entry(program_run_id)
            run = entry.run
            ended = entry.state in {
                ProgramRunState.STOPPING,
                ProgramRunState.FINALIZING,
                ProgramRunState.STOPPED,
                ProgramRunState.SUCCEEDED,
                ProgramRunState.FAILED,
            }
        try:
            if ended:
                raise ProgramRuntimeError("the Program run is ending")
            option = run.answer_operator_question(
                node_instance_id, node_attempt_id, option_id
            )
        except ProgramRuntimeError as error:
            refused = ProgramRunOwnerError(str(error))
            refused.code = "cycle_recovery_offer_stale"  # type: ignore[attr-defined]
            raise refused from error
        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
        return option

    @staticmethod
    def _schedule_program_time(entry: _RunEntry, *, tree_ticks: bool) -> None:
        """Program time advances exactly while the run's tree is scheduled
        to tick: Running, or waiting on an operator-Stop intervention (only
        held subtrees are skipped, and each held node decides whether its
        held time counts). It stands still during a Halt (Halting and
        Halted), a node-failure pause that keeps the tree dormant, and
        once the run stops or finishes."""

        entry.run.program_clock.set_running(tree_ticks)

    @staticmethod
    def _mock_paused(entry: _RunEntry) -> bool:
        """Under the owner lock: the Run is Halted, so a
        mask-only snapshot keeps the Mock paused."""

        return entry.state in {ProgramRunState.HALTING, ProgramRunState.HALTED}

    @staticmethod
    def _note_mock_held(entry: _RunEntry) -> tuple[str, ...] | None:
        """Under the owner lock, with a revision just taken: the Run's new
        held-node mask if it changed, else None."""

        held = entry.run.mock_held_nodes()
        if held == entry.mock_held:
            return None
        entry.mock_held = held
        return held

    def _notify_mock_run_paused(
        self,
        program_run_id: int,
        run_revision: int,
        paused: bool,
        held: tuple[str, ...] = (),
        *,
        record_only: bool = False,
    ) -> None:
        """The Run's Halt state for its nodes' Mock timers,
        outside the owner lock; best effort. The port only records it until
        the Run's first Mock START: a Run without Mock nodes makes
        no Mock call and needs no I/O service. ``record_only``: memory only;
        the run's next tick sends it (``ExecutableProgramRun.settle_mock_pause``)."""

        notify = getattr(
            self._ports.mock_simulation,
            "record_run_paused" if record_only else "set_run_paused",
            None,
        )
        if notify is None:
            return
        try:
            notify(program_run_id, run_revision, paused, held)
        except Exception:
            pass

    def _snapshot(self, entry: _RunEntry) -> ProgramRunSnapshot:
        return ProgramRunSnapshot(
            owner_id=self._owner_id,
            run_revision=entry.revision,
            state=entry.state,
            capture=entry.run.capture,
            tree_status=entry.tree_status,
            node_status_revision=entry.node_status_revision,
            node_statuses=entry.node_statuses,
            # Scope counts are settlement samples, not a live motion feed.
            # Once scheduling is permitted, an earlier zero is no longer proof.
            active_operation_count=(
                None
                if entry.state
                in (
                    ProgramRunState.RUNNING,
                    ProgramRunState.INTERVENTION_REQUIRED,
                )
                else entry.active_operation_count
            ),
            reason_code=entry.reason_code,
            message=entry.message,
            details=entry.details,
            origin=entry.origin,
            operation_failures=tuple(entry.operation_failures.values()),
            stopped_by_rule_id=entry.run.stopped_by_rule_id,
            halt=(
                entry.halt_cause
                if entry.state in {ProgramRunState.HALTING, ProgramRunState.HALTED}
                else None
            ),
        )

    @staticmethod
    def _set_reason(
        entry: _RunEntry,
        reason_code: str | None,
        message: str,
        *,
        details: Mapping[str, Any] | None = None,
        origin: Mapping[str, Any] | None = None,
    ) -> None:
        """Replace the run's reason; details and origin only describe a failure."""

        entry.reason_code = reason_code
        entry.message = message
        entry.details = details
        entry.origin = origin

    @classmethod
    def _set_runner_failure(
        cls, entry: _RunEntry, reason_code: str, message: str
    ) -> None:
        cls._set_reason(entry, reason_code, message, origin=_RUNNER_FAILURE)

    @classmethod
    def _restore_after_stop(cls, entry: _RunEntry) -> None:
        cls._set_reason(
            entry,
            entry.state_after_stop_reason_code,
            entry.state_after_stop_message,
            details=entry.state_after_stop_details,
            origin=entry.state_after_stop_origin,
        )

    @staticmethod
    def _scope_fields(
        snapshot: ProgramRunScopeSnapshot,
        program_run_id: int,
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
        details: Mapping[str, Any] | None,
        origin: Mapping[str, Any] | None,
    ) -> None:
        entry.state = ProgramRunState.FINALIZING
        ProgramRunOwner._schedule_program_time(entry, tree_ticks=False)
        entry.state_after_finalize = state_after_finalize
        entry.state_after_finalize_reason_code = reason_code
        entry.state_after_finalize_message = message
        entry.state_after_finalize_details = details
        entry.state_after_finalize_origin = origin
        entry.active_operation_count = 0
        ProgramRunOwner._set_reason(
            entry, reason_code, message, details=details, origin=origin
        )
        entry.revision += 1

    @classmethod
    def _mark_finalizing_after_stop(cls, entry: _RunEntry) -> None:
        cls._mark_finalizing(
            entry,
            state_after_finalize=entry.state_after_stop,
            reason_code=entry.state_after_stop_reason_code,
            message=entry.state_after_stop_message,
            details=entry.state_after_stop_details,
            origin=entry.state_after_stop_origin,
        )

    @classmethod
    def _mark_finalizing_after_uncontained_stop(
        cls, entry: _RunEntry, reason_code: str | None
    ) -> None:
        """The run ends although its Forced Program End left a node
        uncontained: the failed Stop is its reason, unless the run ends
        with a reason of its own."""

        if entry.state_after_stop_reason_code is not None:
            cls._mark_finalizing_after_stop(entry)
            return
        cls._mark_finalizing(
            entry,
            state_after_finalize=entry.state_after_stop,
            reason_code=reason_code,
            message="Kernel Program-run scope is unknown",
            details=None,
            origin=_RUNNER_FAILURE,
        )

    def _uncontained_end(self, program_run_id: int, scope_reason: str | None) -> bool:
        """Forced Program End (design L4): it ends once nothing of the run is
        stopping, also when its Stop left a node uncontained. One Kernel
        scope read, made only for an uncontained reply."""

        if scope_reason not in PROGRAM_RUN_STOP_UNCONTAINED_REASON_CODES:
            return False
        scope = self._scope.execution_scope(program_run_id)
        stopping = scope.get("stopping_node_indices")
        if scope.get("program_run_id") != program_run_id or not isinstance(
            stopping, (list, tuple)
        ):
            raise ProgramRunOwnerError("Kernel Program-run scope is invalid")
        return not stopping

    def _release_resources(self, program_run_id: int) -> tuple[Exception, ...]:
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

    def _reconcile_finalizing(self, program_run_id: int) -> ProgramRunSnapshot:
        """On the run's tick. A failed cleanup is not attempted again on
        later ticks, only after a Reset's ``retry_cleanup`` (OD-279)."""

        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FINALIZING or entry.cleanup_failed:
                return self._snapshot(entry)
            already_released = entry.resources_released
        errors = () if already_released else self._release_resources(program_run_id)
        with self._lock:
            entry = self._entry(program_run_id)
            if errors:
                entry.cleanup_failed = True
                message = "; ".join(
                    str(error) or type(error).__name__ for error in errors
                )
                if (
                    entry.reason_code != "program_run_cleanup_failed"
                    or entry.message != message
                ):
                    self._set_runner_failure(
                        entry, "program_run_cleanup_failed", message
                    )
                    entry.revision += 1
                return self._snapshot(entry)
            entry.resources_released = True
            entry.state = entry.state_after_finalize
            self._set_reason(
                entry,
                entry.state_after_finalize_reason_code,
                entry.state_after_finalize_message,
                details=entry.state_after_finalize_details,
                origin=entry.state_after_finalize_origin,
            )
            entry.active_operation_count = 0
            entry.revision += 1
            return self._snapshot(entry)

    def start(
        self,
        admission: ProgramRunAdmission,
        *,
        program_run_id: int,
    ) -> ProgramRunSnapshot:
        if not isinstance(admission, ProgramRunAdmission):
            raise TypeError("start requires one exact Program-run admission")
        with self._lock:
            if (
                len(self._runs) + len(self._starting_run_ids)
                >= self._maximum_retained_runs
            ):
                raise ProgramRunOwnerError("Program-run retention is full")
            admitted_program_run_id = require_run_number(program_run_id)
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
                        role="program_runner",
                        application_start_id=self._application_start_id,
                    ),
                ),
                key=lambda identity: identity.role,
            )
        )
        capture = ProgramRunCapture(
            program_run_id=admitted_program_run_id,
            applied_record_digest=admission.applied_record_digest,
            scenario_id=admission.scenario_id,
            source_document_digest=admission.source_document_digest,
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
        interventions = self._ports.program_interventions
        interventions_bound = False
        port_context = admission.port_context(capture.program_run_id)
        try:
            try:
                if context_port is not None:
                    context_port.bind(port_context)
                    context_bound = True
                run = ExecutableProgramRun(
                    program=admission.program.with_active_tcp_binding(admission.program_binding),
                    environment=admission.environment,
                    capture=capture,
                    program_binding=admission.program_binding,
                    project_path=admission.project_path,
                    readiness=admission.readiness,
                    ports=self._ports,
                    program_clock=ProgramClock(self._program_clock_source),
                    # Automatic motion retry asks the owner's own attention check.
                    motion_attention=lambda node_instance_id, request_key: (
                        self.check_motion_attention(
                            admitted_program_run_id, node_instance_id, request_key
                        )
                    ),
                )
                run.refresh_operator_stop_scope()
                entry = _RunEntry(
                    run=run,
                    program_binding=admission.program_binding,
                    node_statuses=run.node_statuses,
                    tick_facts=self._tick_facts(run),
                )
                if interventions is not None:
                    interventions.register_run(
                        admitted_program_run_id, entry.node_statuses.keys()
                    )
                    interventions_bound = True
                with self._lock:
                    if admitted_program_run_id in self._runs:
                        raise ProgramRunOwnerError("Program-run identity was reused")
                    entry.mock_held = run.mock_held_nodes()
                    self._runs[admitted_program_run_id] = entry
                    snapshot = self._snapshot(entry)
                # The motion group state keys start from the run's first facts.
                self._publish_motion_group_states(admitted_program_run_id)
                # Recorded, not sent: the first Mock START sends it.
                self._notify_mock_run_paused(
                    admitted_program_run_id, snapshot.run_revision, False, entry.mock_held
                )
                return snapshot
            except Exception:
                try:
                    if context_bound and context_port is not None:
                        context_port.unbind(admitted_program_run_id)
                finally:
                    if interventions_bound:
                        interventions.release(admitted_program_run_id)
                raise
        finally:
            with self._lock:
                self._starting_run_ids.discard(admitted_program_run_id)

    def read(self, program_run_id: int) -> ProgramRunSnapshot:
        """Return the current owner projection without advancing lifecycle."""

        with self._lock:
            return self._snapshot(self._entry(program_run_id))

    def value_store(self, program_run_id: int) -> ProgramValueStore:
        """The run's value store, for its BB publication subscription."""

        with self._lock:
            return self._entry(program_run_id).run.value_store

    def take_node_history_events(self, program_run_id: int) -> tuple[dict[str, Any], ...]:
        """Node history events (automatic retries, preemptions) since the last take."""
        with self._lock:
            entry = self._entry(program_run_id)
            events, entry.pending_node_history_events = (
                tuple(entry.pending_node_history_events),
                [],
            )
            return events

    def take_diagnostic(self, program_run_id: int) -> dict[str, Any] | None:
        """Transfer one transition capture to the optional asynchronous recorder."""
        with self._lock:
            entry = self._entry(program_run_id)
            record, entry.pending_diagnostic = entry.pending_diagnostic, None
            return record

    def _capture_diagnostic(
        self,
        entry: _RunEntry,
        state: str,
        *,
        reason_code: str | None,
        message: str,
        details: Mapping[str, Any] | None,
        origin: Mapping[str, Any] | None,
    ) -> None:
        """Capture one incident/completion with its exact reason."""
        # Called only on a tree incident/completion transition, not progress.
        # The immutable capture contains references, never a later scene read.
        capture = entry.run.capture
        entry.pending_diagnostic = {
            "schema_version": PROGRAM_DIAGNOSTIC_API,
            "program_run_id": capture.program_run_id,
            "application_start_id": self._application_start_id,
            "program_revision": capture.program_revision,
            "program_binding_id": capture.program_binding_id,
            "retained_bundle_id": capture.retained_bundle_id,
            "graph_revision": capture.graph_revision,
            "state": state,
            "reason_code": reason_code,
            "message": message or f"Program {state}",
            **failure_wire_fields(details, origin),
            **entry.run.diagnostic_snapshot(),
        }

    def intervention(self, program_run_id: int, *, node_attempt_id: int | None = None):
        """Return the exact held node attempt, if any, without deriving it."""

        port = self._ports.program_interventions
        if port is None:
            return None
        if node_attempt_id is not None:
            return next(
                (
                    row
                    for row in port.read_all(program_run_id)
                    if row.identity.node_attempt_id == node_attempt_id
                ),
                None,
            )
        return port.read(program_run_id)

    def failure_holds(self, program_run_id: int) -> tuple[Any, ...]:
        """Every active failure hold of the run, one per held node."""

        self._entry(program_run_id)
        port = self._ports.program_interventions
        return () if port is None else port.read_holds(program_run_id)

    def hold_motion_groups(
        self, program_run_id: int, node_instance_id: str
    ) -> tuple[tuple[str, ...], str]:
        """A held node's own motion groups and its captured Stop scope."""

        with self._lock:
            run = self._entry(program_run_id).run
        return (
            run.node_motion_groups(node_instance_id),
            self._hold_stop_scope(run, node_instance_id),
        )

    def preview_node_inputs(
        self,
        program_run_id: int,
        *,
        node_instance_id: str,
        proposed: Sequence[tuple[str, Any]],
    ) -> tuple[str, Mapping[str, Any], Mapping[str, Any]]:
        """Resolve one captured node's inputs with proposals; no owner effect."""

        with self._lock:
            run = self._entry(program_run_id).run
        return run.preview_node_inputs(node_instance_id, proposed)

    def operator_stop_observation_required(self, program_run_id: int) -> bool:
        """Whether accepted Stop work still needs an observation tick.

        Ordinary recovery interventions are dormant until an explicit action.
        A retained intervention is therefore not itself pending work.  Keep
        ticking only while the captured Stop projection is unsettled, its
        observation is unavailable, or a selected owner task is still active.
        An explicit recovery command schedules the run again. Reads the fact
        the run's tick last published: it never waits for a tick.
        """

        with self._lock:
            entry = self._entry(program_run_id)
            return (
                entry.state is ProgramRunState.INTERVENTION_REQUIRED
                and entry.tick_facts.stop_observation_pending
            )

    def refresh_failure_assessment(self, program_run_id: int, *, node_attempt_id: int):
        """Re-read a held failure's owner assessment when the operator presses.

        Recovery offers do not expire, so a manual result checks freshness
        here, like the Resume and replan presses. The Program's own
        acknowledgement of the non-moving state is not an owner fact: it is
        kept. Raises ProgramRunOwnerError when the owner cannot be read.
        """

        intervention = self.intervention(program_run_id, node_attempt_id=node_attempt_id)
        if intervention is None or intervention.owner_task_id is None:
            raise ProgramRunOwnerError("held attempt has no owner task")
        owner_port = getattr(self._ports, intervention.owner_port_name or "", None)
        # A fresh owner read, never the task's cached terminal.
        read = getattr(owner_port, "current_recovery_assessment", None)
        port = self._ports.program_interventions
        if not callable(read) or port is None:
            raise ProgramRunOwnerError("held task owner cannot read its recovery assessment")
        assessment = read(intervention.owner_task_id, intervention.identity)
        if not isinstance(assessment, Mapping):
            raise ProgramRunOwnerError("held task has no current recovery assessment")
        previous = intervention.owner_assessment
        acknowledged = (
            isinstance(previous, Mapping)
            and previous.get("acknowledged_non_moving") is True
        )
        return port.refresh_owner_assessment(
            intervention.identity,
            {**assessment, "acknowledged_non_moving": acknowledged},
        )

    def _refresh_stop_assessment(self, intervention):
        if intervention.owner_port_name is None or intervention.owner_task_id is None:
            raise ProgramRunOwnerError("stopped attempt lost its owner task")
        owner_port = getattr(self._ports, intervention.owner_port_name, None)
        observe = getattr(owner_port, "observe", None)
        if not callable(observe):
            raise ProgramRunOwnerError("stopped task cannot be observed")
        task = observe(intervention.owner_task_id)
        assessment = task.metadata.get("recovery_assessment")
        if (
            task.task_id != intervention.owner_task_id
            or task.state is not OwnerTaskState.INTERRUPTED
            or not isinstance(assessment, Mapping)
            or assessment.get("recovery_class") != PROGRAM_MOTION_STOP_RECOVERY_CLASS
        ):
            raise ProgramRunOwnerError(
                "stopped task has no current exact recovery assessment"
            )
        port = self._ports.program_interventions
        if port is None:
            raise ProgramRunOwnerError("stopped task lost its intervention registry")
        return port.refresh_owner_assessment(intervention.identity, assessment)

    def check_motion_attention(
        self, program_run_id: int, node_instance_id: str, request_key: bytes
    ) -> None:
        """No-effect Kernel check of operator-alarm attention for the held node
        a motion-producing recovery would move, before any effect.

        Only that node's target resources are tested: an alarm
        on disjoint resources does not block this recovery, just as disjoint
        operations continue.
        """
        with self._lock:
            run = self._entry(program_run_id).run
        try:
            request = run.node_motion_attention_request(node_instance_id, request_key)
        except ProgramRuntimeError as error:
            raise ProgramRunOwnerError(str(error)) from error
        try:
            self._scope.check_node_motion_attention(request)
        except ProgramSelectionReleaseRejectedError as error:
            raise _release_refused(error) from error

    def has_run(self, program_run_id: int) -> bool:
        with self._lock:
            return program_run_id in self._runs

    def close_run_senders(self) -> None:
        """Runner shutdown, after the ticks stopped: no node request still
        queued is sent; a sent one completes on its owner (OD-213)."""

        with self._lock:
            runs = [entry.run for entry in self._runs.values()]
        for run in runs:
            run.close_sender()

    def refresh_selection_scope(self, program_run_id: int) -> ProgramRunSnapshot:
        """Refresh the operator Stop scope and the Mock held mask from the
        Kernel scope read (tick worker). Never issues or releases
        containment."""

        with self._lock:
            run = self._entry(program_run_id).run
        run.refresh_operator_stop_scope()
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.run is not run:
                return self._snapshot(entry)
            entry.active_operation_count = None
            entry.revision += 1
            held = self._note_mock_held(entry)
            held_revision, held_paused = entry.revision, self._mock_paused(entry)
            snapshot = self._snapshot(entry)
        if held is not None:
            self._notify_mock_run_paused(program_run_id, held_revision, held_paused, held)
        return snapshot

    def operator_stop_scope(self, program_run_id: int):
        """Return the current scheduling view, not a new owner read."""
        with self._lock:
            run = self._entry(program_run_id).run
        return run.operator_stop_scope()

    def note_runtime_control_change(self, program_run_id: int) -> ProgramRunSnapshot:
        """Advance the run projection after one non-tree runtime-control state change."""

        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
            return self._snapshot(entry)

    def write_runtime_blackboard(
        self,
        program_run_id: int,
        *,
        key: str,
        value: Any,
        request_key: bytes,
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
                    "request_key": format_request_key(request_key),
                    "value_source": "runtime_control",
                },
            )
        except (KeyError, ProgramValueError) as error:
            raise ProgramRunOwnerError(str(error)) from error
        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
            return self._snapshot(entry)

    def read_runtime_blackboard(
        self, program_run_id: int
    ) -> tuple[ProgramRunSnapshot, list[dict[str, Any]]]:
        """The run's projection and its declared keys' current values; a
        read of the run's value store, no Kernel exchange or tick wait."""

        with self._lock:
            entry = self._entry(program_run_id)
            live = self._snapshot(entry)
            store = entry.run.value_store
        return live, store.current_values()

    def reset_runtime_blackboard(
        self,
        program_run_id: int,
        *,
        key: str,
        request_key: bytes,
    ) -> ProgramRunSnapshot:
        """Forget a persistent key's stored value; the run sees `initial:` again."""

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
            store.reset_to_initial(
                key,
                metadata={
                    "program_run_id": program_run_id,
                    "request_key": format_request_key(request_key),
                    "value_source": "runtime_control",
                },
            )
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error
        with self._lock:
            entry = self._entry(program_run_id)
            entry.revision += 1
            return self._snapshot(entry)

    def resolve_intervention(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
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
            intervention = self.intervention(
                program_run_id, node_attempt_id=node_attempt_id
            )
            if (
                intervention is None
                or intervention.identity.node_attempt_id != node_attempt_id
            ):
                raise ProgramRunOwnerError("Program recovery attempt is stale")
            port.resolve(intervention.identity, action=action, result=result)
            # The operator released this hold: the tree ticks again while any
            # other failure hold stays held (its groups stay stopped).
            if intervention.policy_parameter != "operator_stop_policy":
                entry.failure_paused = False
            entry.operation_failures.pop(intervention.identity.node_attempt_id, None)
            remaining = port.read_holds(program_run_id)
            if remaining:
                entry.state = ProgramRunState.INTERVENTION_REQUIRED
                self._set_reason(
                    entry,
                    remaining[0].reason_code,
                    remaining[0].message,
                    details=remaining[0].details,
                    origin=remaining[0].origin,
                )
            else:
                entry.state = ProgramRunState.RUNNING
                self._set_reason(entry, None, "")
            self._schedule_program_time(entry, tree_ticks=not entry.failure_paused)
            entry.revision += 1
            return self._snapshot(entry)

    def apply_owner_recovery(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        action: str,
        request_key: bytes,
        reason: str | None,
    ) -> Any:
        """Delegate one exact action to the port that owns the held task."""

        with self._lock:
            entry = self._entry(program_run_id)
            intervention = self.intervention(
                program_run_id, node_attempt_id=node_attempt_id
            )
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
            if identity.node_instance_id in entry.run.operator_stop_scope().held_nodes:
                raise ProgramRunOwnerError(
                    "a selected Stop requires selected-batch Resume"
                )
        result = recover(
            task_id,
            identity,
            action=action,
            request_key=request_key,
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
            ProgramRunState.HALTING,
            ProgramRunState.HALTED,
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

    def _send_stop(
        self,
        program_run_id: int,
        *,
        reason: str | None,
        state_after_stop: ProgramRunState,
        reason_code: str | None,
        message: str,
        origin: Mapping[str, Any] | None,
        stop_request_key: bytes | None = None,
        explicit: bool = False,
    ) -> SentStop:
        """The first part of a Forced Program End, on any thread: mark the run
        under the owner lock, then send the Stop with no lock held. It never
        waits for the run's tick; the tree part is ``_finish_stop``."""

        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state in {
                ProgramRunState.STOPPED,
                ProgramRunState.SUCCEEDED,
                ProgramRunState.FAILED,
            }:
                return SentStop(state_after_stop, ended=True)
            if entry.state is ProgramRunState.FINALIZING:
                return SentStop(state_after_stop, reconcile_finalize=True)
            transitioning_from_halt = entry.state in {
                ProgramRunState.HALTING,
                ProgramRunState.HALTED,
            }
            first_stop = not entry.stop_requested
            issue_stop = first_stop or explicit
            observe_existing_stop = transitioning_from_halt
            invalidate_tree = first_stop or transitioning_from_halt
            if issue_stop and stop_request_key is None:
                # The owner's own Forced Program End: one class 1
                # key per run, reused when it is sent again.
                if entry.forced_end_key is None:
                    entry.forced_end_key = RUNNER_KEYS.next_key()
                stop_request_key = entry.forced_end_key
            if first_stop or transitioning_from_halt:
                entry.stop_requested = True
                entry.state = ProgramRunState.STOPPING
                self._schedule_program_time(entry, tree_ticks=False)
                entry.state_after_stop = state_after_stop
                entry.state_after_stop_reason_code = reason_code
                entry.state_after_stop_message = message
                entry.state_after_stop_details = None
                # A run that ends succeeded did not fail.
                entry.state_after_stop_origin = (
                    None
                    if state_after_stop is ProgramRunState.SUCCEEDED
                    else origin
                )
                self._restore_after_stop(entry)
                entry.revision += 1

        scope_snapshot: ProgramRunScopeSnapshot | None = None
        scope_errors: list[Exception] = []
        if issue_stop:
            try:
                assert stop_request_key is not None  # set with issue_stop
                scope_snapshot = self._scope.request_stop(
                    program_run_id, stop_request_key, reason
                )
            except Exception as exc:  # fail closed; status remains stopping
                scope_errors.append(exc)
        elif observe_existing_stop:
            try:
                scope_snapshot = self._scope.observe(program_run_id)
            except Exception as exc:
                scope_errors.append(exc)
        return SentStop(
            state_after_stop,
            invalidate_tree=invalidate_tree,
            scope_snapshot=scope_snapshot,
            scope_errors=tuple(scope_errors),
        )

    def _finish_stop(self, program_run_id: int, sent: SentStop) -> ProgramRunSnapshot:
        """The tree part of a Forced Program End, on the run's tick: invalidate
        the tree, then apply the Stop's scope reply."""

        if sent.ended:
            return self.read(program_run_id)
        if sent.reconcile_finalize:
            return self._reconcile_finalizing(program_run_id)
        with self._lock:
            run = self._entry(program_run_id).run
        scope_errors = list(sent.scope_errors)
        if sent.invalidate_tree:
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
        # A node request sent before the Stop completes and ends its node
        # with the real result before the run ends (OD-213).
        sent_settled = run.settle_sent_requests()
        if stopped_node_statuses is not None:
            stopped_node_statuses = run.node_statuses
        scope_fields: tuple[int | None, bool, str | None] | None = None
        uncontained_end = False
        if sent.scope_snapshot is not None and not scope_errors:
            try:
                scope_fields = self._scope_fields(
                    sent.scope_snapshot,
                    program_run_id,
                )
                uncontained_end = self._uncontained_end(
                    program_run_id, scope_fields[2]
                )
            except Exception as exc:
                scope_errors.append(exc)

        finalize = False
        with self._lock:
            entry = self._entry(program_run_id)
            if (
                sent.state_after_stop is ProgramRunState.STOPPED
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
                self._set_runner_failure(
                    entry,
                    "program_run_scope_unknown",
                    "; ".join(
                        str(error) or type(error).__name__
                        for error in scope_errors
                    ),
                )
                return self._snapshot(entry)
            if scope_fields is not None:
                active_count, settled, scope_reason = scope_fields
                if uncontained_end:
                    settled, active_count = True, 0
                entry.active_operation_count = active_count
                if scope_reason is not None:
                    self._set_runner_failure(
                        entry, scope_reason, "Kernel Program-run scope is unknown"
                    )
                else:
                    self._restore_after_stop(entry)
                if settled and sent_settled:
                    entry.terminal_settlement_active_operation_count = (
                        active_count
                        if active_count == 0
                        and (scope_reason is None or uncontained_end)
                        else None
                    )
                    if uncontained_end:
                        self._mark_finalizing_after_uncontained_stop(
                            entry, scope_reason
                        )
                    else:
                        self._mark_finalizing_after_stop(entry)
                    finalize = True
            snapshot = self._snapshot(entry)
        if finalize:
            return self._reconcile_finalizing(program_run_id)
        return snapshot

    def _begin_stop(
        self,
        program_run_id: int,
        *,
        reason: str | None,
        state_after_stop: ProgramRunState,
        reason_code: str | None,
        message: str,
        origin: Mapping[str, Any] | None,
    ) -> ProgramRunSnapshot:
        """The run's own tick ends the run: both parts at once."""

        return self._finish_stop(
            program_run_id,
            self._send_stop(
                program_run_id,
                reason=reason,
                state_after_stop=state_after_stop,
                reason_code=reason_code,
                message=message,
                origin=origin,
            ),
        )

    def send_stop(
        self,
        program_run_id: int,
        *,
        reason: str | None = None,
        stop_request_key: bytes | None = None,
    ) -> SentStop:
        """Forced Program End (the runner's own Stop of a run), from any
        thread: the run is marked stopping under the owner lock, then the
        Stop is sent at once with no lock held, never after the tick in
        progress. Its tree part, ``finish_stop``, belongs to the run's tick.
        ``stop_request_key`` is the runner's key for this action (a part of
        the request that caused it); without one the run's own class 1 key
        is used."""
        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        return self._send_stop(
            program_run_id,
            reason=reason,
            stop_request_key=stop_request_key,
            state_after_stop=ProgramRunState.STOPPED,
            reason_code=None,
            message="",
            origin=None,
            explicit=True,
        )

    def finish_stop(self, program_run_id: int, sent: SentStop) -> ProgramRunSnapshot:
        """On the run's tick: the tree part of a ``send_stop``."""

        return self._finish_stop(program_run_id, sent)

    def stop_motion(
        self,
        program_run_id: int,
        *,
        group_id: str | list[str] = "all",
        reason: str | None = None,
        stop_request_key: bytes,
        stopped_by_node_instance_id: str | None = None,
        on_accepted: Callable[[int], None] | None = None,
    ) -> ProgramRunSnapshot:
        """Hold a captured selection and contain its work through the Kernel.

        The runner's own selection Stops only (simulation not playing, a
        failure pause, a Program node's own Stop), sent by the run's tick; a
        client's group Stop goes to the Kernel directly.
        ``stopped_by_node_instance_id`` names the Program node whose own Stop
        it is (the Kernel's `stopped_by: program`). ``on_accepted`` gets
        the accepted hold revision, under the owner lock, once the Kernel
        accepted the Stop.
        """

        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                # A Stop already contains this run, it is halted, or it
                # has ended: a motion Stop has no effect.
                return self._snapshot(entry)
            run = entry.run
        try:
            accepted = self._scope.stop_selection(
                run.execution_selection_request(
                    group_id,
                    stop_request_key,
                    stopped_by_node_instance_id=stopped_by_node_instance_id,
                )
            )
        except Exception as exc:
            raise ProgramRunOwnerOutcomeUnknownError(
                str(exc) or "Kernel motion Stop outcome is unknown"
            ) from exc
        # The Kernel accepted the Stop: from here the reply is the run's
        # current state, never an error.
        with self._lock:
            # Publish the accepted holds and their Run revision together.
            # This short local lock never waits for tree work or owner I/O.
            try:
                run.publish_operator_stop_scope(
                    accepted["state"], notify_observation=False
                )
            except Exception:
                return self._snapshot(self._entry(program_run_id))
            entry = self._entry(program_run_id)
            if entry.run is not run or entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                # The run changed while the Stop was applied.
                return self._snapshot(entry)
            entry.active_operation_count = None
            entry.revision += 1
            if on_accepted is not None:
                on_accepted(accepted["state"]["hold_revision"])
            held = self._note_mock_held(entry)
            held_revision, held_paused = entry.revision, self._mock_paused(entry)
            snapshot = self._snapshot(entry)
        if held is not None:
            # The new mask under this command's revision.
            self._notify_mock_run_paused(
                program_run_id, held_revision, held_paused, held
            )
        # A Kernel selection error means it contained more than the
        # selection (Stop all); the state stream shows it.
        # The existing tick host schedules settlement. No tree/owner
        # read may extend this command or retain Stop serialization.
        return snapshot

    def _selection_recovery_candidates(
        self,
        run: ExecutableProgramRun,
        selected: frozenset[str],
        *,
        settle: bool = True,
        refresh_assessments: bool = True,
    ) -> tuple[tuple[Any, ...], dict[str, str]]:
        """Resolve held attempts; commands may require a fresh owner assessment."""
        states = run.task_control_states(
            node_ids=selected,
            settle=settle,
            observe=refresh_assessments,
        )
        return self._recovery_candidates(
            run, selected, states, refresh_assessments=refresh_assessments
        )

    def _recovery_candidates(
        self,
        run: ExecutableProgramRun,
        selected: frozenset[str],
        states: Mapping[str, str],
        *,
        refresh_assessments: bool,
    ) -> tuple[tuple[Any, ...], dict[str, str]]:
        blocked = {
            node_id: (
                "selected_owner_work_pending"
                if state == "active"
                else "selected_owner_recovery_unavailable"
            )
            for node_id, state in states.items()
            if state in {"active", "unrecoverable"}
        }
        interventions = self._ports.program_interventions
        batch = []
        current = (
            ()
            if interventions is None
            else interventions.read_all(run.capture.program_run_id)
        )
        for item in current:
            node_id = item.identity.node_instance_id
            if node_id not in selected:
                continue
            if refresh_assessments:
                try:
                    item = self._refresh_stop_assessment(item)
                except Exception:
                    blocked[node_id] = "selected_owner_assessment_unavailable"
                    continue
            if (
                states.get(node_id) == "held"
                and item.policy_parameter == "operator_stop_policy"
                and isinstance(item.owner_assessment, Mapping)
                and item.owner_assessment.get("reason_code") == "terminal_cleanup_pending"
            ):
                # Its Stop is still settling: not yet Resume-ready (GUI "Stopping").
                blocked[node_id] = "selected_motion_stop_settling"
                continue
            if (
                states.get(node_id) != "held"
                or item.policy_parameter != "operator_stop_policy"
                or item.owner_task_id is None
                or item.owner_port_name is None
                or not callable(
                    getattr(
                        getattr(self._ports, item.owner_port_name, None),
                        "recover",
                        None,
                    )
                )
                or "replan_from_stopped_state"
                not in (item.owner_assessment or {}).get("allowed_actions", ())
            ):
                blocked[node_id] = "selected_owner_recovery_unavailable"
            else:
                batch.append(item)
        candidates = {item.identity.node_instance_id for item in batch}
        recorded = {item.identity.node_instance_id for item in current}
        for node_id, state in states.items():
            if state == "held" and node_id not in candidates:
                # A stopped node whose operator-Stop hold the tree has not
                # recorded yet (its Stop's cleanup still settles) is stopping.
                blocked[node_id] = (
                    "selected_motion_stop_settling"
                    if node_id not in recorded
                    or blocked.get(node_id) == "selected_motion_stop_settling"
                    else "selected_owner_recovery_unavailable"
                )
        return tuple(batch), blocked

    def _selection_observation(
        self, run: ExecutableProgramRun, eligible: bool, facts: _TickFacts
    ) -> tuple[str | None, dict[str, str]]:
        """Why the run's selection state is unavailable (None when known),
        and the stopped nodes whose release is blocked, with the reason."""

        snapshot = facts.scope
        unavailable = (
            facts.stop_observation_error if eligible else "program_run_not_running"
        )
        blocked: dict[str, str] = {}
        ordinary_held = snapshot.held_nodes - snapshot.ready_nodes
        if eligible and ordinary_held:
            states = facts.held_task_states
            if states is None:
                unavailable = "program_selection_observation_unavailable"
            else:
                try:
                    _batch, blocked = self._recovery_candidates(
                        run,
                        ordinary_held,
                        {
                            node: state
                            for node, state in states.items()
                            if node in ordinary_held
                        },
                        refresh_assessments=False,
                    )
                except Exception:
                    unavailable = "program_selection_observation_unavailable"
        return unavailable, blocked

    def _publish_motion_group_states(self, program_run_id: int) -> None:
        """On the run's tick, after its facts: each motion group's state, as
        the runtime controls show it, into its ``motion_group_state.<group>``
        key (OD-339). Recomputed only when the facts or the run's state
        changed; the store publishes only changed values."""

        with self._lock:
            entry = self._entry(program_run_id)
            run, facts, run_state = entry.run, entry.tick_facts, entry.state
            if facts is None or entry.group_state_inputs == (run_state, facts):
                return
            entry.group_state_inputs = (run_state, facts)
        eligible = run_state in {
            ProgramRunState.RUNNING,
            ProgramRunState.INTERVENTION_REQUIRED,
        }
        halt = _halt_phase(run_state)
        unavailable, blocked = self._selection_observation(run, eligible, facts)
        store = run.value_store
        for group_id, nodes in run.execution_selections.items():
            store.write_motion_group_state(
                group_id,
                _selection_group_state(nodes, facts, unavailable, blocked, halt)[0],
            )

    def execution_selection_controls(self, program_run_id: int) -> dict[str, Any]:
        return self.read_selection_controls(program_run_id)[1]

    def read_selection_controls(
        self, program_run_id: int
    ) -> tuple[ProgramRunSnapshot, dict[str, Any]]:
        """Derive one Run/control projection from the selection facts the
        run's tick last published, under the owner lock: no Kernel exchange
        and no wait for a tick. What blocks a release is what the run last
        learnt from the Kernel (OD-152 (a)); the Kernel's own refusal of a
        release stays the authority."""
        with self._lock:
            entry = self._entry(program_run_id)
            run = entry.run
            live = self._snapshot(entry)
            eligible = entry.state in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }
            facts = entry.tick_facts
            program_binding = entry.program_binding
            halt = _halt_phase(entry.state)
        snapshot = facts.scope
        selection_release_block_reason = facts.selection_release_block_reason
        unavailable, blocked = self._selection_observation(run, eligible, facts)
        groups = []
        for group_id, nodes in run.execution_selections.items():
            scale_selector, scale_reason = _runtime_scale_selector_for_group(
                run, program_binding, group_id
            )
            state, reason, selected_held, selected_ordinary_held = (
                _selection_group_state(nodes, facts, unavailable, blocked, halt)
            )
            # Run and Resume are the same command.
            release_allowed = (
                eligible
                and reason is None
                and state in {"ready", "held", "mixed"}
            )
            if release_allowed and selection_release_block_reason is not None:
                release_allowed = False
                reason = selection_release_block_reason
            release_reason = (
                None if release_allowed else reason or "selection_not_releasable"
            )
            groups.append(
                {
                    "group_id": group_id,
                    "node_count": len(nodes),
                    # A halted run holds every node (the Halt's freeze).
                    "held_node_count": (
                        len(nodes) if halt == "halted" else len(selected_held)
                    ),
                    # During a Halt the stopped work per group is not known;
                    # the run's own Halt state covers it.
                    "ordinary_held_node_count": (
                        None
                        if halt is not None
                        or state in {"unavailable", "stopping", "faulted"}
                        else len(selected_ordinary_held)
                    ),
                    "state": state,
                    "stop_allowed": eligible and state not in {"ready", "held"},
                    "start_allowed": release_allowed,
                    "start_unavailable_reason": release_reason,
                    "resume_allowed": release_allowed,
                    "resume_unavailable_reason": release_reason,
                    "runtime_scale_selector": scale_selector,
                    "runtime_scale_unavailable_reason": scale_reason,
                    "resource_ids": _selection_resource_ids(
                        run, program_binding, group_id
                    ),
                    # A Program node's own Stop holds the group until Run:
                    # the Kernel's attribution of the group's held nodes.
                    **_program_stop_fields(nodes, facts),
                }
            )
        held = snapshot.held_nodes
        initially_effectful = facts.initial_ready_nodes
        stopping, faulted = snapshot.stopping_nodes, snapshot.faulted_nodes
        # Every effectful node is held by the Kernel in a settled phase:
        # never released (Ready) or stopped with its Stop certified
        # (Held). The Kernel admits a Program operation only for a
        # released node, so no Program motion is active and none can start
        # until a Run/Resume release. Non-effectful nodes never move. An
        # unknown or empty effectful set proves nothing.
        motion_held = (
            unavailable is None
            and bool(initially_effectful)
            and initially_effectful <= held - stopping - faulted
        )
        return live, {
            "hold_revision": snapshot.hold_revision,
            "motion_held": motion_held,
            "groups": groups,
        }

    def resume_motion(
        self,
        program_run_id: int,
        *,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Recover one frozen selected batch, then release its Kernel hold.

        Fenced on the hold revision, not the run revision: tree ticks
        raise the run revision without changing the holds.
        """
        return self._recover_motion_selection(
            program_run_id,
            expected_run_revision=None,
            expected_hold_revision=expected_hold_revision,
            request_key=request_key,
            group_id=group_id,
            reason=reason,
        )

    def start_motion(
        self,
        program_run_id: int,
        *,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Run is the same operation as Resume: release every never-started
        and stopped node in the selection, recovering stopped work first."""
        return self.resume_motion(
            program_run_id,
            expected_hold_revision=expected_hold_revision,
            request_key=request_key,
            group_id=group_id,
            reason=reason,
        )

    def prepare_manual_handover(
        self,
        program_run_id: int,
        *,
        node_indices: tuple[int, ...],
        expected_run_revision: int,
        expected_hold_revision: int,
        request_key: bytes,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Retire selected stopped attempts without releasing any Program hold.

        The caller supplies the Kernel-derived conflicting producer indices
        and the hold revision of that Kernel read; the cleanup is fenced on it
        (the operator's approve sends it, PD-8). This is cleanup only: Kernel
        manual admission must independently recheck the current hold, owners
        and resources. No manual or Program motion starts.
        """
        if (
            not isinstance(node_indices, tuple)
            or not node_indices
            or len(node_indices) > 100_000
            or any(type(index) is not int or index < 0 for index in node_indices)
            or any(left >= right for left, right in zip(node_indices, node_indices[1:]))
        ):
            raise ProgramRunOwnerError(
                "manual handover requires canonical Program node indices"
            )
        with self._lock:
            try:
                node_ids = self._entry(program_run_id).run.scope_node_ids(node_indices)
            except IndexError as error:
                raise ProgramRunOwnerError(
                    "manual handover index is outside its captured Program"
                ) from error
        return self._recover_motion_selection(
            program_run_id,
            expected_run_revision=expected_run_revision,
            expected_hold_revision=expected_hold_revision,
            request_key=request_key,
            group_id="all",
            reason=reason,
            held_nodes=node_ids,
        )

    def _recover_stopped_item(
        self,
        run: ExecutableProgramRun,
        item: Any,
        request_key: bytes,
        reason: str | None,
    ) -> None:
        """Replan one Stop-interrupted task while its node stays held.

        Cleanup cannot start motion: the Kernel still holds admission until
        the caller's revision-fenced release. The replan's key is one class 1
        key per (Resume request, node attempt), stored on the run, so a
        resend of the same request reuses it and a new request gets a new one.
        """
        identity = item.identity
        with self._lock:
            keys = self._entry(identity.program_run_id).replan_keys
            slot = (request_key, identity.node_attempt_id)
            replan_key = keys.get(slot)
            if replan_key is None:
                replan_key = keys[slot] = RUNNER_KEYS.next_key()
        owner_port = getattr(self._ports, item.owner_port_name)
        result = owner_port.recover(
            item.owner_task_id,
            identity,
            action="replan_from_stopped_state",
            request_key=replan_key,
            reason=reason,
        )
        outcome = recovery_outcome(result)
        if outcome is None:
            raise ProgramRunOwnerOutcomeUnknownError(
                "selected owner recovery outcome is unknown"
            )
        if not outcome:
            error = result.get("error")
            raise ProgramRunOwnerError(
                str(error.get("message"))
                if isinstance(error, Mapping)
                else "selected owner recovery was rejected"
            )
        # Consume successful cleanup once, even if a later item fails. The
        # Kernel still holds all future admissions.
        self._ports.program_interventions.resolve(item.identity, action="retry")
        run.reconcile_halt_tasks(
            node_ids=frozenset({item.identity.node_instance_id})
        )

    @staticmethod
    def _hold_stop_scope(run: ExecutableProgramRun, node_instance_id: str) -> str:
        """The held node's captured `intervention_stop_scope` (default all)."""
        policy = run.capture.node_policy_by_instance_id.get(node_instance_id)
        scope = None if policy is None else policy.recovery_policy.get(
            "intervention_stop_scope"
        )
        return "own_groups" if scope == "own_groups" else "all"

    def _failure_pause_stop_groups(
        self, run: ExecutableProgramRun, holds: Sequence[Any]
    ) -> str | list[str]:
        """What a failure pause stops: all groups, or only the held nodes'
        own motion groups when every hold's scope is `own_groups`."""
        groups: set[str] = set()
        for hold in holds:
            node = hold.identity.node_instance_id
            own = run.node_motion_groups(node)
            if self._hold_stop_scope(run, node) == "all" or own == ("all",):
                return "all"
            groups.update(own)
        return sorted(groups) if groups else "all"

    def _failure_hold_release(
        self, run: ExecutableProgramRun, failure: Any, holds: Sequence[Any]
    ) -> str | list[str]:
        """The groups released for one hold (contract items 39, 43): its own
        motion groups (`own_groups`), or every group not owned by another
        active hold (`all`, the default). Its own groups are always released.

        A hold whose node is in no named group owns every named group that no
        other active hold names ("all" when it is the only hold) and, while
        other holds are active, releases its own node by node_instance_id;
        while it is active, another hold releases only its own
        groups.
        """
        node = failure.identity.node_instance_id
        own = run.node_motion_groups(node)
        taken = {
            group
            for hold in holds
            if hold.identity != failure.identity
            for group in run.node_motion_groups(hold.identity.node_instance_id)
        }
        if own == ("all",):
            if not taken:
                return "all"
            return [
                group
                for group in run.execution_selections
                if group != "all" and group not in taken
            ]
        if self._hold_stop_scope(run, node) == "own_groups":
            return list(own)
        if not taken:
            return "all"
        if "all" in taken:
            return list(own)
        return [
            group
            for group in run.execution_selections
            if group != "all" and (group in own or group not in taken)
        ]

    def retry_failure_pause(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        action: str | None,
        request_key: bytes,
        reason: str | None,
    ) -> ProgramRunSnapshot:
        """Retry one failure hold and release its motion groups.

        ``action`` is the failed step's owner recovery, or ``None`` for a
        step without an owner task. The caller then resolves the hold so the
        tree ticks again.
        """
        return self._release_failure_hold(
            program_run_id,
            node_attempt_id=node_attempt_id,
            action=action,
            close_held=False,
            request_key=request_key,
            reason=reason,
        )

    def continue_failure_pause(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        request_key: bytes,
        reason: str | None,
    ) -> ProgramRunSnapshot:
        """Continue: close the held operation and release
        the hold's groups like Retry. The caller then resolves the hold with a
        failure, so the held node returns FAILURE to the tree."""
        return self._release_failure_hold(
            program_run_id,
            node_attempt_id=node_attempt_id,
            action=None,
            close_held=True,
            request_key=request_key,
            reason=reason,
        )

    def _release_failure_hold(
        self,
        program_run_id: int,
        *,
        node_attempt_id: int,
        action: str | None,
        close_held: bool,
        request_key: bytes,
        reason: str | None,
    ) -> ProgramRunSnapshot:
        """One operation, in order: verify that the pause's containment Stop
        is confirmed; capture the current selection revision; recover the
        Stop-interrupted tasks and the failed step (or close it) while every
        node stays held; then release the hold's groups (Ready and Held work)
        against that revision, so a Stop that lands meanwhile wins.
        """
        interventions = self._ports.program_interventions
        with self._lock:
            entry = self._entry(program_run_id)
            holds = (
                ()
                if interventions is None
                else interventions.read_holds(program_run_id)
            )
            failure = next(
                (
                    hold
                    for hold in holds
                    if hold.identity.node_attempt_id == node_attempt_id
                ),
                None,
            )
            if (
                entry.state is not ProgramRunState.INTERVENTION_REQUIRED
                or failure is None
            ):
                raise ProgramRunOwnerError("failure Retry attempt is stale")
            if entry.failure_pause_stop_pending:
                raise ProgramRunOwnerError(
                    "stopping the Program's other motion is not confirmed; "
                    "Retry waits for it"
                )
            run, run_revision = entry.run, entry.revision
        release = self._failure_hold_release(run, failure, holds)
        scope = run.read_operator_stop_scope()
        if run.scope_node_ids(
            (*scope["stopping_node_indices"], *scope["faulted_node_indices"])
        ):
            raise ProgramRunOwnerError(
                "Program motion is still stopping or faulted; Retry waits for it"
            )
        captured_revision = scope["hold_revision"]
        kept = (
            frozenset()
            if release == "all"
            else run.execution_selection(
                [
                    group
                    for group in run.execution_selections
                    if group != "all" and group not in release
                ]
            )
        )
        selected = (
            run.execution_selection(release)
            & (
                run.scope_node_ids(scope["held_node_indices"])
                | run.scope_node_ids(scope["ready_node_indices"])
            )
        ) - kept
        # A hold in no named group is only in "all": while other holds
        # are active it releases its own node by node_instance_id
        # next to the free groups.
        node = failure.identity.node_instance_id
        node_release = (
            release != "all"
            and run.node_motion_groups(node) == ("all",)
            and node in run.scope_node_ids(scope["held_node_indices"])
        )
        batch, blocked = self._selection_recovery_candidates(run, selected)
        # The held failed steps are resolved by their own holds.
        for hold in holds:
            blocked.pop(hold.identity.node_instance_id, None)
        if blocked:
            raise ProgramRunOwnerError(next(iter(blocked.values())))
        cleanup_accepted = False
        try:
            for item in batch:
                self._recover_stopped_item(run, item, request_key, reason)
                cleanup_accepted = True
            port = getattr(self._ports, failure.owner_port_name or "", None)
            if close_held and failure.owner_task_id is not None:
                close = getattr(port, "close", None)
                if not callable(close):
                    raise ProgramRunOwnerError(
                        "the held operation has no addressed closure"
                    )
                try:
                    closed = close(
                        failure.owner_task_id,
                        failure.identity,
                        request_key=request_key,
                    )
                except OwnerTaskSettlementPendingError as error:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "held operation closure outcome is unknown"
                    ) from error
                if closed is not True:
                    raise ProgramRunOwnerError(
                        "the held operation is not eligible to close"
                    )
                cleanup_accepted = True
            if action is not None:
                recover = getattr(port, "recover", None)
                if failure.owner_task_id is None or not callable(recover):
                    raise ProgramRunOwnerError(
                        "the failed step has no addressed owner recovery"
                    )
                # Allowed while held: this operation releases the hold
                # only after the recovery is prepared.
                result = recover(
                    failure.owner_task_id,
                    failure.identity,
                    action=action,
                    request_key=request_key,
                    reason=reason,
                )
                if (
                    not isinstance(result, Mapping)
                    or type(result.get("accepted")) is not bool
                ):
                    raise ProgramRunOwnerOutcomeUnknownError(
                        "failed-step recovery outcome is unknown"
                    )
                if not result["accepted"]:
                    error = result.get("error")
                    raise ProgramRunOwnerError(
                        str(error.get("message"))
                        if isinstance(error, Mapping)
                        else "failed-step recovery was rejected"
                    )
                cleanup_accepted = True
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.run is not run or entry.revision != run_revision:
                    raise ProgramRunOwnerError(
                        "Program changed during Retry; motion stays held"
                    )
            releases: list[tuple[dict[str, Any], frozenset[str]]] = []
            if selected:
                releases.append(
                    (run.execution_selection_request(release, request_key), selected)
                )
            if node_release:
                releases.append(
                    (
                        run.execution_selection_request(
                            [], request_key, node_instance_id=node
                        ),
                        frozenset({node}),
                    )
                )
            revision = captured_revision
            for request, nodes in releases:
                try:
                    released = self._scope.release_selection(request, revision)
                    self._note_own_release(program_run_id, released)
                    run.publish_operator_stop_scope(
                        released,
                        expected_revision=revision + 1,
                        released_nodes=nodes,
                        notify_observation=False,
                    )
                except ProgramSelectionReleaseRejectedError as error:
                    raise _release_refused(error) from error
                except Exception as error:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        f"Retry release outcome is unconfirmed: {error}"
                    ) from error
                run.note_operator_release(nodes)
                revision += 1
                cleanup_accepted = True
        except ProgramRunOwnerOutcomeUnknownError:
            raise
        except ProgramRunOwnerError as error:
            if cleanup_accepted:
                raise ProgramRunOwnerRecoveryIncompleteError(str(error)) from error
            raise
        with self._lock:
            return self._snapshot(self._entry(program_run_id))

    def _recover_motion_selection(
        self,
        program_run_id: int,
        *,
        expected_run_revision: int | None,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str],
        reason: str | None,
        held_nodes: frozenset[str] | None = None,
    ) -> ProgramRunSnapshot:
        """Use the same exact cleanup for Resume and held-cycle handover.

        Manual handover also fences on the run revision; Run/Resume passes
        None and is fenced on the hold revision only.

        Cleanup can retire old effects but cannot start new motion. Scheduling
        stays held until the single revision-fenced release succeeds. A newer
        Stop does not wait for this ordinary-lane work and wins that fence.
        """
        with self._lock:
            entry = self._entry(program_run_id)
            if (
                expected_run_revision is not None
                and entry.revision != expected_run_revision
            ):
                raise ProgramRunOwnerStaleRevisionError(
                    expected_run_revision, entry.revision
                )
            if entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                raise ProgramRunOwnerError(
                    "motion Resume requires a current running Program"
                )
            run, run_revision = entry.run, entry.revision
        selected = run.execution_selection(group_id)
        if held_nodes is not None:
            if not held_nodes <= selected:
                raise ProgramRunOwnerError(
                    "manual handover names unknown Program nodes"
                )
            selected = held_nodes
        scope = run.read_operator_stop_scope()
        if (
            type(expected_hold_revision) is not int
            or scope["hold_revision"] != expected_hold_revision
        ):
            raise ProgramRunOwnerError("motion Resume identifies a stale selection")
        if selected & run.scope_node_ids(
            (*scope["stopping_node_indices"], *scope["faulted_node_indices"])
        ):
            raise ProgramRunOwnerError("selected work is still stopping or faulted")
        held = run.scope_node_ids(scope["held_node_indices"])
        if held_nodes is not None and not selected <= held:
            raise ProgramRunOwnerError("manual handover producers are not all held")
        if held_nodes is None:
            # Run and Resume are one release: never-started (Ready) work
            # is released with stopped work; only stopped work needs the
            # recovery below.
            held = held | run.scope_node_ids(scope["ready_node_indices"])
        selected &= held
        if not selected:
            raise ProgramRunOwnerError("selected scope is not held")
        interventions = self._ports.program_interventions
        batch, blocked = self._selection_recovery_candidates(run, selected)
        if blocked:
            raise ProgramRunOwnerError(next(iter(blocked.values())))
        cleanup_accepted = False
        try:
            for item in batch:
                self._recover_stopped_item(run, item, request_key, reason)
                cleanup_accepted = True
            self._scope.retire_held_preparations(
                program_run_id, selected, expected_hold_revision,
            )
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.run is not run or (
                    expected_run_revision is not None
                    and entry.revision != run_revision
                ):
                    raise ProgramRunOwnerError(
                        "Program changed during selected recovery; holds remain"
                    )
            if held_nodes is not None:
                current_scope = run.read_operator_stop_scope()
                if (
                    current_scope["hold_revision"]
                    != expected_hold_revision
                    or not selected
                    <= run.scope_node_ids(current_scope["held_node_indices"])
                    or selected
                    & run.scope_node_ids(
                        (
                            *current_scope["stopping_node_indices"],
                            *current_scope["faulted_node_indices"],
                        )
                    )
                ):
                    raise ProgramRunOwnerError(
                        "Program hold changed during manual handover"
                    )
            else:
                try:
                    released = self._scope.release_selection(
                        run.execution_selection_request(group_id, request_key),
                        expected_hold_revision,
                    )
                    self._note_own_release(program_run_id, released)
                    run.publish_operator_stop_scope(
                        released,
                        expected_revision=expected_hold_revision + 1,
                        released_nodes=selected,
                        notify_observation=False,
                    )
                except ProgramSelectionReleaseRejectedError as error:
                    raise _release_refused(error) from error
                except Exception as error:
                    raise ProgramRunOwnerOutcomeUnknownError(
                        f"selected release outcome is unconfirmed: {error}"
                    ) from error
                run.note_operator_release(selected)
        except ProgramRunOwnerOutcomeUnknownError:
            raise
        except ProgramRunOwnerError as error:
            if cleanup_accepted:
                raise ProgramRunOwnerRecoveryIncompleteError(str(error)) from error
            raise
        except Exception as error:
            raise ProgramRunOwnerOutcomeUnknownError(
                "selected recovery outcome is unconfirmed"
            ) from error
        finally:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.run is run and entry.state in {
                    ProgramRunState.RUNNING,
                    ProgramRunState.INTERVENTION_REQUIRED,
                }:
                    # A lost release response cannot leave a nonexistent
                    # intervention latched. Holds remain in the last exact
                    # view until a current owner result proves release.
                    remaining = (
                        None
                        if interventions is None
                        else interventions.read(program_run_id)
                    )
                    entry.state = (
                        ProgramRunState.INTERVENTION_REQUIRED
                        if remaining is not None
                        else ProgramRunState.RUNNING
                    )
                    self._schedule_program_time(
                        entry,
                        tree_ticks=remaining is None or not entry.failure_paused,
                    )
                    if remaining is None:
                        self._set_reason(entry, None, "")
                    else:
                        self._set_reason(
                            entry,
                            remaining.reason_code,
                            remaining.message,
                            details=remaining.details,
                            origin=remaining.origin,
                        )
                    entry.revision += 1
                    held = self._note_mock_held(entry)
                    held_revision = entry.revision
                    held_paused = self._mock_paused(entry)
                else:
                    held = None
            if held is not None:
                # The released mask under this revision.
                self._notify_mock_run_paused(
                    program_run_id, held_revision, held_paused, held
                )
        return self.read(program_run_id)

    def _note_own_release(
        self, program_run_id: int, released: Mapping[str, Any]
    ) -> None:
        """Record the hold revision of the runner's own accepted release
        (Run, Resume, Execute after Halt). Older hold facts describe the
        state before it and are ignored from now on (§6.3.2). Only an
        accepted release reaches here: a refusal raises before it."""

        revision = released.get("hold_revision") if isinstance(released, Mapping) else None
        if type(revision) is not int or revision < 0:
            raise ProgramRunOwnerOutcomeUnknownError(
                "the release reply omitted its hold revision"
            )
        with self._lock:
            entry = self._entry(program_run_id)
            entry.own_release_revision = max(entry.own_release_revision, revision)

    def _reconcile_hold_facts(
        self, program_run_id: int, facts: ProgramRunHoldFacts | None
    ) -> bool:
        """Mirror the Kernel's hold state for this run (§6.3.2), memory only.

        Facts older than the runner's own last accepted release are ignored.
        A Halt or runner-loss freeze moves an active run to Halting (its
        tree stops ticking, its clock stops, the Mock pause is recorded for
        the next tick to send); a freeze the Kernel no longer holds returns
        a Halting or Halted run to Running. Returns True when the facts
        changed since the last ones applied, so the caller re-reads the
        scope. An ended run (stopped, succeeded, failed) re-reads nothing:
        its tree no longer ticks, its context was released at its end and
        its terminal snapshot stays as it ended.
        """

        if facts is None:
            return False
        if facts.program_run_id != program_run_id:
            raise ProgramRunOwnerError("hold facts name another Program run")
        notify: tuple[int, bool, tuple[str, ...]] | None = None
        with self._lock:
            entry = self._entry(program_run_id)
            if facts.hold_revision < entry.own_release_revision:
                return False
            key = facts.reconciliation_key()
            previous = entry.applied_hold_facts
            changed = key != previous
            entry.applied_hold_facts = key
            cause = facts.halt_cause()
            cause_changed = cause != entry.halt_cause
            entry.halt_cause = cause
            revision_before = entry.revision
            active = entry.state in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }
            halted = entry.state in {ProgramRunState.HALTING, ProgramRunState.HALTED}
            if facts.frozen in {"halt", "runner_loss"} and active:
                entry.state = ProgramRunState.HALTING
                # Halt freezes the Program: its waits and timers keep their
                # attempt and lose no time until Execute.
                self._schedule_program_time(entry, tree_ticks=False)
                entry.stop_requested = True
                entry.active_operation_count = None
                self._set_reason(entry, None, "")
                entry.revision += 1
                entry.mock_held = entry.run.mock_held_nodes()
                # A Halt before a node request was sent: it is not sent
                # (OD-213); the node sends it again after Execute.
                entry.run.withdraw_unsent_requests()
                # Recorded only: the Halting tick sends it (reconcile_halting).
                notify = (entry.revision, True, entry.mock_held)
                record_only = True
            elif (
                facts.frozen in {"halt", "runner_loss"}
                and entry.state is ProgramRunState.HALTED
                and (previous is None or previous[2] != facts.frozen_by)
            ):
                # A newer freeze of an already halted run: a pending Execute
                # names the older revision and is refused (C8). A halting run
                # has no Execute to make stale; its settling read goes on.
                entry.revision += 1
            elif facts.frozen == "none" and halted:
                entry.state = ProgramRunState.RUNNING
                self._schedule_program_time(entry, tree_ticks=True)
                entry.stop_requested = False
                entry.active_operation_count = None
                self._set_reason(entry, None, "")
                entry.revision += 1
                entry.mock_held = entry.run.mock_held_nodes()
                notify = (entry.revision, False, entry.mock_held)
                record_only = False
                _LOGGER.info(
                    "Program run %s follows the Kernel: Halt released at hold "
                    "revision %s",
                    program_run_id,
                    facts.hold_revision,
                )
            if (
                cause_changed
                and entry.revision == revision_before
                and entry.state in {ProgramRunState.HALTING, ProgramRunState.HALTED}
            ):
                # The published cause of the Halt changed.
                entry.revision += 1
            ended = entry.state in {
                ProgramRunState.STOPPED,
                ProgramRunState.SUCCEEDED,
                ProgramRunState.FAILED,
            }
        if notify is not None:
            self._notify_mock_run_paused(
                program_run_id, *notify, record_only=record_only
            )
        return changed and not ended

    def _complete_halting(
        self, program_run_id: int, *, expected_revision: int, stop_failed: bool
    ) -> ProgramRunSnapshot:
        """Freeze a physically settled run without invalidating its tree.
        ``stop_failed``: the Halt's Stop left a node faulted; the run keeps
        that runner failure as its reason."""

        with self._lock:
            entry = self._entry(program_run_id)
            if (
                entry.state is not ProgramRunState.HALTING
                or entry.revision != expected_revision
            ):
                return self._snapshot(entry)
            run = entry.run
            captured_revision = entry.revision
        try:
            active, _held, unrecoverable = run.reconcile_halt_tasks()
        except Exception as error:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.revision != captured_revision:
                    return self._snapshot(entry)
                entry.active_operation_count = None
                self._set_runner_failure(
                    entry,
                    "program_halt_task_state_unknown",
                    str(error) or type(error).__name__,
                )
                return self._snapshot(entry)
        with self._lock:
            entry = self._entry(program_run_id)
            if (
                entry.state is not ProgramRunState.HALTING
                or entry.revision != captured_revision
            ):
                return self._snapshot(entry)
            if active:
                entry.active_operation_count = active
                if not stop_failed:
                    # Halt progress, not a failure.
                    self._set_reason(
                        entry,
                        "program_halt_task_observation_pending",
                        "Halt settled physically while accepted Program work "
                        "is still becoming observable",
                    )
                return self._snapshot(entry)
            entry.state = ProgramRunState.HALTED
            self._schedule_program_time(entry, tree_ticks=False)
            entry.active_operation_count = 0
            if stop_failed:
                pass  # The failed-Stop reason set by the Halting tick stays.
            elif unrecoverable:
                # What the settled Halt allows, not a failure.
                self._set_reason(
                    entry,
                    "program_halt_resume_unavailable",
                    "Halt settled, but one or more preserved tasks do not "
                    "offer fresh current-state recovery; Reset remains available",
                )
            else:
                self._set_reason(entry, None, "")
            entry.revision += 1
            return self._snapshot(entry)

    def reconcile_halting(self, program_run_id: int) -> ProgramRunSnapshot:
        """Halting tick: send the recorded Mock pause, read the Kernel scope
        (stopping and faulted nodes) and, once no Program node is stopping,
        move to Halted, also with faulted nodes (their failed Stop stays the
        run's reason). The scope read is applied only if the run did not
        change meanwhile."""

        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.HALTING:
                return self._snapshot(entry)
            captured_revision = entry.revision
            run = entry.run
        # The Halt's Mock pause, recorded on entering Halting, is sent here.
        run.settle_mock_pause()
        try:
            # One Kernel scope read: halted once no Program node is stopping;
            # a faulted node means the Halt's Stop did not contain it.
            scope = self._scope.execution_scope(program_run_id)
            stopping = scope.get("stopping_node_indices")
            faulted = scope.get("faulted_node_indices")
            if (
                scope.get("program_run_id") != program_run_id
                or not isinstance(stopping, (list, tuple))
                or not isinstance(faulted, (list, tuple))
            ):
                raise ProgramRunOwnerError("Kernel Program-run scope is invalid")
            settled = not stopping
            active_count = 0 if settled else None
            scope_reason = "program_run_stop_failed" if faulted else None
        except Exception as error:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.revision != captured_revision:
                    return self._snapshot(entry)
                entry.active_operation_count = None
                self._set_runner_failure(
                    entry,
                    "program_run_scope_unknown",
                    str(error) or type(error).__name__,
                )
                return self._snapshot(entry)
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.revision != captured_revision:
                return self._snapshot(entry)
            entry.active_operation_count = active_count
            if scope_reason is not None:
                self._set_runner_failure(
                    entry, scope_reason, "Kernel Program-run scope is unknown"
                )
        return (
            self._complete_halting(
                program_run_id,
                expected_revision=captured_revision,
                stop_failed=scope_reason is not None,
            )
            if settled
            else self.read(program_run_id)
        )

    def resume_halt(
        self,
        program_run_id: int,
        *,
        reason: str | None = None,
    ) -> ProgramRunSnapshot:
        """Execute after Halt: resume tree execution without releasing motion.

        Execute after Halt governs execution only. Interrupted work stays
        held with its operator-stop intervention for a later group Resume,
        and never-started work stays Ready for Run: the Kernel keeps every
        node's motion hold when it releases the freeze (``release_halt`` at
        the hold revision of the last hold facts this run applied, so a newer
        Halt is refused as stale, never released unseen). Applied by the
        run's tick (OD-146); no Kernel call is made under the owner lock.
        """

        if reason is not None:
            _text(reason, field="reason", maximum_bytes=4_096)
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is ProgramRunState.HALTING:
                raise ProgramRunOwnerError("Halt is still settling")
            if entry.state is not ProgramRunState.HALTED:
                raise ProgramRunOwnerError(
                    "Execute requires the exact preserved halted run"
                )
            run = entry.run
            captured_revision = entry.revision
            applied = entry.applied_hold_facts
        active, _held, unrecoverable = run.reconcile_halt_tasks()
        if active:
            raise ProgramRunOwnerOutcomeUnknownError(
                "Halt task settlement is not yet observable"
            )
        if unrecoverable:
            raise ProgramRunOwnerError(
                "preserved halted work has no fresh-state recovery"
            )
        with self._lock:
            entry = self._entry(program_run_id)
            if (
                entry.state is not ProgramRunState.HALTED
                or entry.revision != captured_revision
            ):
                # A newer Halt landed meanwhile; never release it.
                raise ProgramRunOwnerOutcomeUnknownError(
                    "Program state changed before the Halt release"
                )
        # A run is halted only through applied hold facts with a freeze.
        assert applied is not None
        revision, frozen = applied[0], applied[1]
        if frozen != "none":
            try:
                released = self._scope.release_halt(
                    program_run_id, expected_hold_revision=revision
                )
            except ProgramSelectionReleaseRejectedError as error:
                raise _release_refused(error) from error
            except Exception as error:
                raise ProgramRunOwnerOutcomeUnknownError(
                    f"Halt release outcome is unconfirmed: {error}"
                ) from error
            if released is None:
                raise ProgramRunOwnerError(
                    "the run is no longer the Kernel's current run"
                )
            self._note_own_release(program_run_id, released)
        run.refresh_operator_stop_scope()
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is ProgramRunState.RUNNING and entry.run is run:
                # The run's tick already followed the released Kernel state.
                return self._snapshot(entry)
            if (
                entry.state is not ProgramRunState.HALTED
                or entry.revision != captured_revision
            ):
                raise ProgramRunOwnerOutcomeUnknownError(
                    "Program state changed during the Halt release"
                )
            entry.state = ProgramRunState.RUNNING
            # Execute after Halt continues the same attempts.
            self._schedule_program_time(entry, tree_ticks=True)
            entry.stop_requested = False
            entry.active_operation_count = None
            self._set_reason(entry, None, "")
            entry.revision += 1
            entry.mock_held = entry.run.mock_held_nodes()
            held = entry.mock_held
            snapshot = self._snapshot(entry)
        # Resume continues the pending Mock changes. A node START that
        # reaches the host before this arrives is applied paused by the
        # host; the Runner does not defer it.
        self._notify_mock_run_paused(program_run_id, snapshot.run_revision, False, held)
        return snapshot

    def begin_reset_containment(
        self, program_run_id: int, *, stop_request_key: bytes
    ) -> ProgramRunSnapshot:
        """Close a terminal Run's Kernel scope without changing its history.
        Applied by the run's tick (OD-146)."""
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state not in {
                ProgramRunState.STOPPED,
                ProgramRunState.SUCCEEDED,
                ProgramRunState.FAILED,
            }:
                raise ProgramRunOwnerError(
                    "Reset containment requires a terminal Run"
                )
            entry.reset_containment = None
            entry.reset_containment_pre_stop_active_operation_count = (
                entry.terminal_settlement_active_operation_count
            )
        try:
            scope = self._scope.request_stop(
                program_run_id, stop_request_key, PROGRAM_RESET_STOP_REASON
            )
            self._scope_fields(scope, program_run_id)
        except Exception as error:
            with self._lock:
                entry.reset_containment = ProgramRunScopeSnapshot(
                    program_run_id,
                    ProgramRunScopeState.UNKNOWN,
                    None,
                    "reset_containment_outcome_unknown",
                )
            detail = str(error) or type(error).__name__
            raise ProgramRunOwnerOutcomeUnknownError(
                f"Reset containment outcome is unknown: {detail}"
            ) from error
        with self._lock:
            entry.reset_containment = scope
            return self._snapshot(entry)

    def reset_containment_pending(self, program_run_id: int) -> bool:
        """Scheduling only: do not read an external owner here."""
        with self._lock:
            scope = self._entry(program_run_id).reset_containment
            return scope is not None and scope.state is ProgramRunScopeState.ACTIVE

    def reset_containment_settled(self, program_run_id: int) -> bool:
        with self._lock:
            entry = self._entry(program_run_id)
            scope = entry.reset_containment
            pre_stop_active_count = (
                entry.reset_containment_pre_stop_active_operation_count
            )
        if scope is None:
            raise ProgramRunOwnerError("Reset containment has not been requested")
        if scope.state is ProgramRunScopeState.UNKNOWN:
            if (
                scope.reason_code in PROGRAM_RUN_STOP_UNCONTAINED_REASON_CODES
                and pre_stop_active_count == 0
            ):
                return True
            raise ProgramRunOwnerOutcomeUnknownError(
                scope.reason_code or "Reset containment outcome is unknown"
            )
        return scope.state is ProgramRunScopeState.SETTLED

    def _observe_reset_containment(self, program_run_id: int) -> ProgramRunSnapshot:
        try:
            scope = self._scope.observe(program_run_id)
            self._scope_fields(scope, program_run_id)
        except Exception as error:
            scope = ProgramRunScopeSnapshot(
                program_run_id,
                ProgramRunScopeState.UNKNOWN,
                None,
                (str(error) or type(error).__name__)[:256],
            )
        with self._lock:
            entry = self._entry(program_run_id)
            entry.reset_containment = scope
            return self._snapshot(entry)

    def _reset_scope_effects(self, program_run_id: int) -> None:
        snapshot = self._scope.reset(program_run_id)
        _active_count, settled, reason = self._scope_fields(snapshot, program_run_id)
        if not settled:
            raise ProgramRunOwnerError(
                reason or "Kernel Program-run scope did not reset"
            )

    def reset_retained_effects(self, program_run_id: int) -> None:
        """Reset a durably terminal run after its resident tree owner rotated."""

        require_run_number(program_run_id)
        self._reset_scope_effects(program_run_id)

    def reset_effects(self, program_run_id: int) -> ProgramRunSnapshot:
        """Release terminal Kernel operations before Program-cycle Reset.
        It touches no tree; it takes the owner lock only."""

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

    def forget_reset_run(self, program_run_id: int) -> None:
        """Drop one quiescent resident tree after durable Program Reset.

        Durable history belongs to Program Runner's cycle journal.  Keeping the
        executable tree here after its Kernel scope has been reset would turn
        a concurrent-run bound into a process-lifetime command-count limit.
        """

        require_run_number(program_run_id)
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
                raise ProgramRunOwnerError("Program-run resources are not released")
            self._runs.pop(program_run_id)

    def reconcile_stop(self, program_run_id: int) -> ProgramRunSnapshot:
        finalize = False
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.STOPPING:
                return self._snapshot(entry)
            run = entry.run
        # A node request sent before the Stop still ends its node with the
        # real result before the run ends (OD-213).
        sent_settled = run.settle_sent_requests()
        node_statuses = run.node_statuses
        try:
            scope_snapshot = self._scope.observe(program_run_id)
            scope_fields = self._scope_fields(scope_snapshot, program_run_id)
            uncontained_end = self._uncontained_end(program_run_id, scope_fields[2])
        except Exception as exc:
            with self._lock:
                entry = self._entry(program_run_id)
                entry.active_operation_count = None
                self._set_runner_failure(
                    entry,
                    "program_run_scope_unknown",
                    str(exc) or type(exc).__name__,
                )
                return self._snapshot(entry)
        with self._lock:
            entry = self._entry(program_run_id)
            active_count, settled, scope_reason = scope_fields
            if uncontained_end:
                settled, active_count = True, 0
            entry.active_operation_count = active_count
            if scope_reason is not None:
                self._set_runner_failure(
                    entry, scope_reason, "Kernel Program-run scope is unknown"
                )
            else:
                self._restore_after_stop(entry)
            if (
                entry.state_after_stop is ProgramRunState.STOPPED
                and entry.tree_status == "invalid"
                and entry.node_statuses != node_statuses
            ):
                entry.node_statuses = node_statuses
                entry.node_status_revision += 1
            if entry.state is ProgramRunState.STOPPING and settled and sent_settled:
                entry.terminal_settlement_active_operation_count = (
                    active_count
                    if active_count == 0
                    and (scope_reason is None or uncontained_end)
                    else None
                )
                if uncontained_end:
                    self._mark_finalizing_after_uncontained_stop(entry, scope_reason)
                else:
                    self._mark_finalizing_after_stop(entry)
                finalize = True
            snapshot = self._snapshot(entry)
        if finalize:
            return self._reconcile_finalizing(program_run_id)
        return snapshot

    def advance_once(
        self,
        program_run_id: int,
        *,
        hold_facts: ProgramRunHoldFacts | None = None,
    ) -> ProgramRunSnapshot:
        """One tick; the tree belongs to it (OD-146). First the commands
        handed to the run since its last tick, in posting order, in every
        state (also a dormant, halted or ended run). Then the run mirrors the
        Kernel's latest hold facts (§6.3.2) and, when they changed, re-reads
        its scope (held, stopping and faulted nodes, the Mock held mask).
        Last, it publishes its selection facts for the other threads."""

        self._apply_tick_intents(program_run_id)
        snapshot = self._advance_run(program_run_id, hold_facts)
        self._publish_tick_facts(program_run_id)
        return snapshot

    def _advance_run(
        self, program_run_id: int, hold_facts: ProgramRunHoldFacts | None
    ) -> ProgramRunSnapshot:
        if self._reconcile_hold_facts(program_run_id, hold_facts):
            self.refresh_selection_scope(program_run_id)
        with self._lock:
            halting = self._entry(program_run_id).state is ProgramRunState.HALTING
        if halting:
            # The Halting tick reads the Kernel scope and applies it only if
            # the run did not change.
            return self.reconcile_halting(program_run_id)
        snapshot = self._advance_once(program_run_id)
        snapshot = self._stop_motion_for_program_nodes(program_run_id, snapshot)
        snapshot = self._stop_motion_for_simulation(program_run_id, snapshot)
        return self._stop_motion_for_failure_pause(program_run_id, snapshot)

    def _stop_motion_for_simulation(
        self, program_run_id: int, snapshot: ProgramRunSnapshot
    ) -> ProgramRunSnapshot:
        """A command refused because its simulation isn't
        playing is handled as the operator's Stop on the refused nodes' motion
        groups (all groups for a node in none): the same Kernel selection
        Stop, never a node failure. The run stays held until the operator's
        Run; nothing resumes by itself. Sent again on the next tick only if
        its outcome is not confirmed."""
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                return snapshot
            run = entry.run
            revision = entry.revision
        nodes = run.simulation_stop_nodes()
        if not nodes:
            return snapshot
        groups: set[str] = set()
        for node in nodes:
            groups.update(run.node_motion_groups(node))
        try:
            stopped = self.stop_motion(
                program_run_id,
                group_id="all" if "all" in groups else sorted(groups),
                reason="simulation_not_playing",
                stop_request_key=self._revision_stop_key(
                    program_run_id, revision, "simulation_stop_key"
                ),
            )
        except Exception:
            return snapshot
        run.note_simulation_stops(nodes)
        return stopped

    def _stop_motion_for_program_nodes(
        self, program_run_id: int, snapshot: ProgramRunSnapshot
    ) -> ProgramRunSnapshot:
        """A Program node's own motion-group Stop (Ask operator's
        `hold_motion`, Stop motion groups): the operator's group Stop, sent
        with the node attempt's own key, again on the next tick until the
        Kernel accepted it. The request names the node: the Kernel
        attributes the holds it raises to it (`stopped_by: program`) until
        their release (Run)."""
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state not in {
                ProgramRunState.RUNNING,
                ProgramRunState.INTERVENTION_REQUIRED,
            }:
                return snapshot
            run = entry.run
        for node_id, attempt_id, groups, key in run.program_stop_requests():

            def accepted(
                hold_revision: int,
                node_id: str = node_id,
                attempt_id: int = attempt_id,
            ) -> None:
                # Under the owner lock, memory only.
                run.note_program_stop_accepted(node_id, attempt_id, hold_revision)

            try:
                snapshot = self.stop_motion(
                    program_run_id,
                    group_id=list(groups),
                    reason=f"Program node {node_id} stops its motion groups",
                    stop_request_key=key,
                    stopped_by_node_instance_id=node_id,
                    on_accepted=accepted,
                )
            except ProgramRunOwnerOutcomeUnknownError:
                # Not confirmed: sent again with the same key next tick.
                continue
        # The accepted Stops' history events, with this tick's.
        events = run.take_node_history_events()
        if events:
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.run is run:
                    entry.pending_node_history_events.extend(events)
                    entry.revision += 1
                    snapshot = self._snapshot(entry)
        return snapshot

    def _revision_stop_key(
        self, program_run_id: int, revision: int, slot: str
    ) -> bytes:
        """The class 1 key of one internal motion Stop per run revision:
        a retry for the same revision resends the same key."""
        with self._lock:
            entry = self._entry(program_run_id)
            stored = getattr(entry, slot)
            if stored is None or stored[0] != revision:
                stored = (revision, RUNNER_KEYS.next_key())
                setattr(entry, slot, stored)
            return stored[1]

    def failure_pause_stop_pending(self, program_run_id: int) -> bool:
        with self._lock:
            return self._entry(program_run_id).failure_pause_stop_pending

    def cleanup_failed(self, program_run_id: int) -> bool:
        """The finalizing run's last cleanup attempt failed (OD-279)."""
        with self._lock:
            entry = self._entry(program_run_id)
            return entry.state is ProgramRunState.FINALIZING and entry.cleanup_failed

    def retry_cleanup(self, program_run_id: int) -> bool:
        """A Reset, on its own thread: allow one more cleanup attempt of a
        finalizing run whose cleanup failed. Memory only; the attempt runs on
        the run's next tick. True when an attempt was allowed."""
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is not ProgramRunState.FINALIZING or not entry.cleanup_failed:
                return False
            entry.cleanup_failed = False
            return True

    def _stop_motion_for_failure_pause(
        self, program_run_id: int, snapshot: ProgramRunSnapshot
    ) -> ProgramRunSnapshot:
        """A node failure that pauses for the operator also stops Program
        motion, like Halt: all groups, or only the held nodes' own groups
        (`intervention_stop_scope: own_groups`); the tree is already dormant.
        Retried on the next tick if the Stop outcome is not confirmed."""
        with self._lock:
            entry = self._entry(program_run_id)
            if not entry.failure_pause_stop_pending:
                return snapshot
            if entry.state is not ProgramRunState.INTERVENTION_REQUIRED:
                entry.failure_pause_stop_pending = False
                return snapshot
            entry.failure_pause_stop_pending = False
            revision = entry.revision
            run = entry.run
        port = self._ports.program_interventions
        try:
            stopped = self.stop_motion(
                program_run_id,
                group_id=self._failure_pause_stop_groups(
                    run, () if port is None else port.read_holds(program_run_id)
                ),
                reason="Program paused on a node failure",
                stop_request_key=self._revision_stop_key(
                    program_run_id, revision, "failure_pause_stop_key"
                ),
            )
        except Exception:
            # Keep the node's own failure, and say that containment is not
            # confirmed; it is retried and Retry waits for it.
            with self._lock:
                entry = self._entry(program_run_id)
                if entry.state is ProgramRunState.INTERVENTION_REQUIRED:
                    entry.failure_pause_stop_pending = True
                    if _FAILURE_PAUSE_UNCONFIRMED not in entry.message:
                        entry.message = (
                            f"{entry.message} {_FAILURE_PAUSE_UNCONFIRMED}".strip()
                        )
                        entry.revision += 1
                return self._snapshot(entry)
        with self._lock:
            entry = self._entry(program_run_id)
            if _FAILURE_PAUSE_UNCONFIRMED in entry.message:
                entry.message = entry.message.replace(
                    _FAILURE_PAUSE_UNCONFIRMED, ""
                ).strip()
                entry.revision += 1
                return self._snapshot(entry)
        return stopped

    def _advance_once(self, program_run_id: int) -> ProgramRunSnapshot:
        with self._lock:
            entry = self._entry(program_run_id)
        if (
            entry.reset_containment is not None
            and entry.reset_containment.state is ProgramRunScopeState.ACTIVE
        ):
            # A finished tree must not tick again merely because Reset
            # awaits an asynchronous Kernel certificate. Its immutable
            # terminal snapshot and revisions remain unchanged.
            return self._observe_reset_containment(program_run_id)
        observe_only = False
        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is ProgramRunState.STOPPING:
                reconcile_stop = True
                reconcile_finalize = False
            elif entry.state is ProgramRunState.FINALIZING:
                reconcile_stop = False
                reconcile_finalize = True
            elif entry.state is ProgramRunState.INTERVENTION_REQUIRED:
                intervention_port = self._ports.program_interventions
                intervention = (
                    None
                    if intervention_port is None
                    else intervention_port.read(program_run_id)
                )
                # Dormant from a new failure hold until the operator
                # releases one; other holds stay held while it ticks.
                dormant = intervention is None or entry.failure_paused
                self._schedule_program_time(entry, tree_ticks=not dormant)
                if dormant:
                    if not entry.run.pending_selected_observation():
                        return self._snapshot(entry)
                    # A separate intervention keeps ordinary tree work
                    # dormant, but cannot hide already-accepted Stop work.
                    observe_only = True
                reconcile_stop = False
                reconcile_finalize = False
                run = entry.run
            elif entry.state is not ProgramRunState.RUNNING:
                return self._snapshot(entry)
            else:
                reconcile_stop = False
                reconcile_finalize = False
                run = entry.run
        if reconcile_stop:
            return self.reconcile_stop(program_run_id)
        if reconcile_finalize:
            return self._reconcile_finalizing(program_run_id)

        try:
            run.refresh_pending_stop_settlement()
            # A changed held-node mask takes its own run
            # revision and reaches the Mock port before this tick can
            # START anything (the port settles it before a START).
            with self._lock:
                entry = self._entry(program_run_id)
                held = None
                if entry.run is run and run.mock_held_nodes() != entry.mock_held:
                    entry.revision += 1
                    held = self._note_mock_held(entry)
                    held_revision = entry.revision
                    held_paused = self._mock_paused(entry)
            if held is not None:
                self._notify_mock_run_paused(
                    program_run_id, held_revision, held_paused, held
                )
            if observe_only:
                held = run.operator_stop_scope().held_nodes
                run.reconcile_halt_tasks(node_ids=held or None)
                tree_status = run.status
            else:
                tree_status = run.tick_once()
            node_statuses = run.node_statuses
            node_history_events = run.take_node_history_events()
            stop_observation_changed = run.take_stop_observation_change()
        except Exception as exc:
            return self._begin_stop(
                program_run_id,
                reason="Program runner failure",
                state_after_stop=ProgramRunState.FAILED,
                reason_code="program_runtime_failed",
                message=str(exc) or type(exc).__name__,
                origin=_RUNNER_FAILURE,
            )

        status_name = tree_status.name.lower()
        intervention = None
        holds: tuple[Any, ...] = ()
        intervention_port = self._ports.program_interventions
        if intervention_port is not None:
            try:
                intervention = intervention_port.read(program_run_id)
                holds = intervention_port.read_holds(program_run_id)
            except Exception as exc:
                return self._begin_stop(
                    program_run_id,
                    reason="Program intervention state unavailable",
                    state_after_stop=ProgramRunState.FAILED,
                    reason_code="program_intervention_state_unknown",
                    message=str(exc) or type(exc).__name__,
                    origin=_RUNNER_FAILURE,
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
            if stop_observation_changed:
                entry.revision += 1
            if node_history_events:
                # The run revision moves, so the runner's observer drains them.
                entry.pending_node_history_events.extend(node_history_events)
                entry.revision += 1
            if entry.node_statuses != node_statuses:
                entry.node_statuses = node_statuses
                entry.node_status_revision += 1
            new_holds = [
                hold
                for hold in holds
                if hold.identity.node_attempt_id not in entry.known_holds
            ]
            entry.known_holds = frozenset(
                hold.identity.node_attempt_id for hold in holds
            )
            if new_holds:
                # A node failure reaches the operator: pause the whole
                # run (dormant tree, frozen clock) and stop its scope.
                first = new_holds[0]
                entry.state = ProgramRunState.INTERVENTION_REQUIRED
                entry.failure_paused = True
                entry.failure_pause_stop_pending = True
                self._schedule_program_time(entry, tree_ticks=False)
                self._set_reason(
                    entry,
                    first.reason_code,
                    first.message,
                    details=first.details,
                    origin=first.origin,
                )
                entry.revision += 1
                for hold in new_holds:
                    row = run.operation_failure(hold.identity.node_instance_id)
                    if row is not None:
                        entry.operation_failures[hold.identity.node_attempt_id] = {
                            **row,
                            "operator_facing_run_revision": entry.revision,
                        }
                self._capture_diagnostic(
                    entry,
                    "intervention_required",
                    reason_code=first.reason_code,
                    message=first.message,
                    details=first.details,
                    origin=first.origin,
                )
                return self._snapshot(entry)
            if intervention is not None and entry.state is ProgramRunState.RUNNING:
                entry.state = ProgramRunState.INTERVENTION_REQUIRED
                # A node-failure pause keeps the tree dormant, like Halt;
                # an operator-Stop intervention keeps it ticking.
                self._schedule_program_time(
                    entry,
                    tree_ticks=intervention.policy_parameter
                    == "operator_stop_policy",
                )
                self._set_reason(
                    entry,
                    intervention.reason_code,
                    intervention.message,
                    details=intervention.details,
                    origin=intervention.origin,
                )
                entry.revision += 1
                assessment = intervention.owner_assessment
                ordinary_stop = (
                    intervention.policy_parameter == "operator_stop_policy"
                    and isinstance(assessment, Mapping)
                    and assessment.get("recovery_class")
                    == "ordinary_operator_stop"
                )
                if intervention.policy_parameter != "operator_stop_policy":
                    entry.failure_pause_stop_pending = True
                self._capture_diagnostic(
                    entry,
                    "held" if ordinary_stop else "intervention_required",
                    reason_code=intervention.reason_code,
                    message=(
                        "Program motion stopped and held; Resume Motion replans "
                        "from current state"
                        if ordinary_stop
                        else intervention.message
                    ),
                    details=intervention.details,
                    origin=intervention.origin,
                )
                return self._snapshot(entry)
            if tree_status is py_trees.common.Status.RUNNING:
                return self._snapshot(entry)

        terminal_state = (
            ProgramRunState.SUCCEEDED
            if tree_status is py_trees.common.Status.SUCCESS
            else ProgramRunState.FAILED
        )
        terminal_failure = (
            run.terminal_failure()
            if terminal_state is ProgramRunState.FAILED
            else None
        )
        terminal_row = (
            run.operation_failure()
            if terminal_state is ProgramRunState.FAILED
            else None
        )
        try:
            scope_snapshot = self._scope.observe(program_run_id)
            active_count, scope_settled, scope_reason = self._scope_fields(
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
                origin=_RUNNER_FAILURE,
            )
        if not scope_settled:
            return self._begin_stop(
                program_run_id,
                reason="Program tree terminated with active operations",
                state_after_stop=terminal_state,
                reason_code="program_terminal_scope_not_settled",
                message=("Program tree terminated before its Kernel scope settled"),
                origin=_RUNNER_FAILURE,
            )

        with self._lock:
            entry = self._entry(program_run_id)
            if entry.state is ProgramRunState.RUNNING:
                entry.terminal_settlement_active_operation_count = (
                    active_count
                    if active_count == 0 and scope_reason is None
                    else None
                )
                terminal = (
                    {
                        "reason_code": None,
                        "message": "",
                        "details": None,
                        "origin": None,
                    }
                    if terminal_failure is None
                    else {
                        "reason_code": terminal_failure.reason_code,
                        "message": terminal_failure.message,
                        "details": terminal_failure.details,
                        "origin": terminal_failure.origin,
                    }
                )
                self._capture_diagnostic(entry, terminal_state.value, **terminal)
                self._mark_finalizing(
                    entry, state_after_finalize=terminal_state, **terminal
                )
                if terminal_row is not None:
                    # The failed run's operation failure, until Reset.
                    entry.operation_failures[""] = {
                        **terminal_row,
                        "operator_facing_run_revision": entry.revision,
                    }
        return self._reconcile_finalizing(program_run_id)


def build_program_run_admission(
    selection: Mapping[str, Any],
    *,
    expected_scenario_id: str,
    expected_source_document_digest: str,
    environment: NodeEnvironment,
    owner_availability: Mapping[str, AvailabilityFact],
    additional_owner_identities: tuple[ProgramRunOwnerIdentity, ...] = (),
) -> ProgramRunAdmission:
    """Validate one complete normalized Program-run input."""

    if not isinstance(selection, Mapping):
        raise TypeError("Program-run input must be a mapping")
    expected_fields = {
        "applied_record_digest",
        "assistant_projection",
        "assistant_mode",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "owner_identities",
        "program",
        "program_binding",
        "program_frame_facts",
        "program_binding_id",
        "program_revision",
        "project_path",
        "retained_bundle_id",
        "source_document_digest",
        "scenario_id",
        "schema_version",
        "simulation_scene",
    }
    if selection.get("schema_version") != PROGRAM_RUN_INPUT_API:
        raise ProgramRunOwnerError("Program-run input version is unsupported")
    if set(selection) != expected_fields:
        raise ProgramRunOwnerError("Program-run input has wrong fields")
    _digest(selection.get("deployment_revision"), field="deployment_revision")
    if selection.get("assistant_mode") not in {
        "required",
        "optional",
        "forbidden",
    }:
        raise ProgramRunOwnerError("scenario assistant mode is invalid")
    assistant_mode = selection["assistant_mode"]
    assistant_projection = selection.get("assistant_projection")
    if assistant_projection is not None and not isinstance(assistant_projection, dict):
        raise ProgramRunOwnerError(
            "scenario assistant projection must be null or an object"
        )
    if assistant_mode == "required" and assistant_projection is None:
        raise ProgramRunOwnerError("scenario requires an absent assistant projection")
    if assistant_mode == "forbidden" and assistant_projection is not None:
        raise ProgramRunOwnerError("scenario forbids a present assistant projection")
    simulation_scene = selection.get("simulation_scene")
    if simulation_scene is not None and not isinstance(simulation_scene, dict):
        raise ProgramRunOwnerError("simulation scene must be null or an object")
    scenario_id = _text(selection.get("scenario_id"), field="scenario_id")
    project_path = _text(
        selection.get("project_path"), field="project_path", maximum_bytes=4096
    )
    source_document_digest = _digest(
        selection.get("source_document_digest"),
        field="source_document_digest",
    )
    if scenario_id != expected_scenario_id:
        raise ProgramRunOwnerError("scenario Program selection changed scenario ID")
    if source_document_digest != expected_source_document_digest:
        raise ProgramRunOwnerError(
            "Scenario source document changed during Run admission"
        )
    if not isinstance(environment, NodeEnvironment):
        raise TypeError("environment must be one retained NodeEnvironment")
    node_environment_digest = _digest(
        selection.get("node_environment_digest"),
        field="node_environment_digest",
    )

    program_value = selection.get("program")
    binding_value = selection.get("program_binding")
    if not isinstance(program_value, dict):
        raise ProgramRunOwnerError("scenario selection has no Program object")
    if not isinstance(binding_value, dict):
        raise ProgramRunOwnerError("scenario selection has no Program Binding object")
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
    try:
        frame_facts = ProgramFrameFacts.from_capture(selection.get("program_frame_facts"), selection)
    except ValueError as error:
        raise ProgramRunOwnerError(f"scenario frame facts are invalid: {error}") from error
    program = program.with_program_frame_facts(frame_facts)

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
        binding_environment.get("catalog_id") != environment.manifest.catalog_id
        or binding_environment.get("content_version")
        != environment.manifest.catalog_content_version
    ):
        raise ProgramRunOwnerError(
            "scenario Program catalog contract is unsupported by this runner"
        )
    if (
        program.program_revision != program_revision
        or binding_value.get("program_binding_id") != program_binding_id
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
            "role",
            "application_start_id",
        }:
            raise ProgramRunOwnerError(
                "scenario selection has a malformed owner identity"
            )
        parsed_owner_identities.append(
            ProgramRunOwnerIdentity(
                role=raw_identity["role"],
                application_start_id=raw_identity["application_start_id"],
            )
        )
    for identity in additional_owner_identities:
        if not isinstance(identity, ProgramRunOwnerIdentity):
            raise TypeError("additional owner identity is invalid")
        parsed_owner_identities.append(identity)
    owner_identities = tuple(
        sorted(parsed_owner_identities, key=lambda identity: identity.role)
    )
    if len({identity.role for identity in owner_identities}) != len(
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
        project_path=project_path,
        applied_record_digest=_digest(
            selection.get("applied_record_digest"),
            field="applied_record_digest",
        ),
        source_document_digest=source_document_digest,
        program=program,
        environment=environment,
        program_binding_id=program_binding_id,
        retained_bundle_id=retained_bundle_id,
        graph_revision=graph_revision,
        program_binding=binding_value,
        readiness=readiness,
        owner_identities=owner_identities,
        assistant_projection=assistant_projection,
        simulation_scene=simulation_scene,
    )


def build_applied_program_run_admission(
    applied: Mapping[str, Any],
    *,
    expected_scenario_id: str,
    expected_source_document_digest: str,
    environment: NodeEnvironment,
    owner_availability: Mapping[str, AvailabilityFact],
    additional_owner_identities: tuple[ProgramRunOwnerIdentity, ...] = (),
) -> ProgramRunAdmission:
    """Build admission from one complete authoritative Applied publication."""

    if not isinstance(applied, Mapping):
        raise TypeError("Applied Program selection must be a mapping")
    expected_applied_fields = {
        "assistant_mode",
        "assistant_projection",
        "baseline_program_binding_id",
        "deployment_revision",
        "graph_revision",
        "node_environment_digest",
        "owner_identities",
        "preparation_digest",
        "program",
        "program_binding",
        "program_frame_facts",
        "program_binding_id",
        "program_revision",
        "project_path",
        "retained_bundle_id",
        "source_document_digest",
        "scenario_id",
        "schema_version",
        "simulation_scene",
        "source",
    }
    if applied.get("schema_version") != APPLIED_PROGRAM_SELECTION_API:
        raise ProgramRunOwnerError(
            "Applied Program selection version is unsupported (only "
            f"{APPLIED_PROGRAM_SELECTION_API}; launch-time Program parameter "
            "overrides were removed)"
        )
    if set(applied) != expected_applied_fields:
        raise ProgramRunOwnerError("Applied Program selection has wrong fields")
    _digest(
        applied.get("baseline_program_binding_id"), field="baseline_program_binding_id"
    )
    _digest(applied.get("preparation_digest"), field="preparation_digest")
    source = applied.get("source")
    if not isinstance(source, Mapping):
        raise ProgramRunOwnerError("Applied Program source identity is invalid")
    _digest(source.get("record_digest"), field="source.record_digest")

    owner_identities = applied.get("owner_identities")
    if not isinstance(owner_identities, list):
        raise ProgramRunOwnerError(
            "Applied Program publication has no exact owner identities"
        )
    normalized_selection = {
        "applied_record_digest": source["record_digest"],
        "assistant_projection": deepcopy(applied.get("assistant_projection")),
        "assistant_mode": applied["assistant_mode"],
        "deployment_revision": applied["deployment_revision"],
        "graph_revision": applied["graph_revision"],
        "node_environment_digest": applied["node_environment_digest"],
        "owner_identities": deepcopy(owner_identities),
        "program": deepcopy(applied["program"]),
        "program_binding": deepcopy(applied["program_binding"]),
        "program_frame_facts": deepcopy(applied["program_frame_facts"]),
        "program_binding_id": applied["program_binding_id"],
        "program_revision": applied["program_revision"],
        "retained_bundle_id": applied["retained_bundle_id"],
        "source_document_digest": applied["source_document_digest"],
        "scenario_id": applied["scenario_id"],
        "project_path": applied["project_path"],
        "schema_version": PROGRAM_RUN_INPUT_API,
        "simulation_scene": deepcopy(applied["simulation_scene"]),
    }
    return build_program_run_admission(
        normalized_selection,
        expected_scenario_id=expected_scenario_id,
        expected_source_document_digest=expected_source_document_digest,
        environment=environment,
        owner_availability=owner_availability,
        additional_owner_identities=additional_owner_identities,
    )


__all__ = [
    "APPLIED_PROGRAM_SELECTION_API",
    "PROGRAM_DIAGNOSTIC_API",
    "PROGRAM_RUN_STATUS_API",
    "program_run_end",
    "PROGRAM_RUN_INPUT_API",
    "ProgramRunAdmission",
    "ProgramRunOwner",
    "ProgramRunOwnerError",
    "ProgramRunOwnerOutcomeUnknownError",
    "ProgramRunOwnerRecoveryIncompleteError",
    "ProgramRunSnapshot",
    "ProgramRunState",
    "RunIntent",
    "SentStop",
    "build_applied_program_run_admission",
    "build_program_run_admission",
]
