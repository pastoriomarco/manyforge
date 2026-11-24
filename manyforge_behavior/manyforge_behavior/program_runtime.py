"""One bounded py_trees Program run over exact retained identities."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass, field
from hashlib import sha256
from types import MappingProxyType
from typing import Any, Final
from datetime import datetime, timezone
import itertools
import re
from time import monotonic_ns

import py_trees
import rfc8785

from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.executable_node_resolver import (
    NodeAdapterBuildContext,
    _derived_active_tcp_key,
    build_executable_node_resolver,
)
from manyforge_behavior.node_availability import (
    NodeExecutionReadiness,
    project_node_execution_availability,
)
from manyforge_behavior.failure_details import (
    FailureRecord,
    failure_origin,
    failure_wire_fields,
)
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.orchestration_ports import (
    NodeAdapterPorts,
    NodeAttemptIdentity,
    require_run_number,
)
from manyforge_behavior.request_keys import format_request_key
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueStore,
)
from manyforge_behavior.program import (
    INTERRUPT_MOTION_NODE_IDS,
    MOTION_NODE_IDS,
    Program,
    ProgramNodePolicy,
    ProgramTreeNode,
)
from manyforge_behavior.recovery import (
    RecoveryAwareNode,
    recovery_policy_is_actionable,
)
from manyforge_behavior.nodes.operator import OperatorRequestChoice
from manyforge_behavior.nodes.v2_owner import MotionTaskAdapter, OwnerTaskAdapter
from manyforge_behavior.run_sender import RunSender
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.value_path import (
    ValuePathError,
    parse_value_path,
    path_covers,
    replace_at_path,
)

PROGRAM_RUN_CAPTURE_API: Final = "manyforge.program_run_capture.v3"

class ProgramRuntimeError(RuntimeError):
    """A Program run cannot be built or advanced truthfully."""


class NodeInputProposalError(ValueError):
    """A proposed node input is foreign, selects captured meaning, or is invalid."""


# Parameters that name a captured Program Binding row by value. Required
# capability parameters and declared blackboard-key parameters are read from
# the catalog entry itself.
_BINDING_SELECTION_PARAMETERS: Final = frozenset({"tcp_endpoint"})


def _require_text(value: object, *, field: str, maximum_bytes: int = 512) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be non-empty text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{field} exceeds {maximum_bytes} bytes")
    return value


def _require_digest(value: object, *, field: str) -> str:
    text = _require_text(value, field=field, maximum_bytes=71)
    if (
        not text.startswith("sha256:")
        or len(text) != 71
        or any(character not in "0123456789abcdef" for character in text[7:])
    ):
        raise ValueError(f"{field} must be an exact SHA-256 identity")
    return text


@dataclass(frozen=True, slots=True)
class ProgramRunOwnerIdentity:
    """One owner captured for a run: its role in this application start."""

    role: str
    application_start_id: str

    def __post_init__(self) -> None:
        _require_text(self.role, field="role", maximum_bytes=32)
        if (
            not isinstance(self.application_start_id, str)
            or re.fullmatch(r"[0-9a-f]{16}", self.application_start_id) is None
        ):
            raise ValueError("application_start_id must be 16 lowercase hex characters")

    def to_dict(self) -> dict[str, str]:
        return {"role": self.role, "application_start_id": self.application_start_id}


@dataclass(frozen=True, slots=True)
class ProgramRunCapture:
    """Verified identities captured before one Program run is constructed."""

    program_run_id: int
    applied_record_digest: str
    scenario_id: str
    source_document_digest: str
    program_revision: str
    program_binding_id: str
    retained_bundle_id: str
    graph_revision: str
    node_environment_digest: str
    owner_identities: tuple[ProgramRunOwnerIdentity, ...]
    node_policies: tuple[ProgramNodePolicy, ...] = ()
    node_policy_digest: str = field(init=False)
    _node_policy_lookup: Mapping[str, ProgramNodePolicy] = field(
        init=False,
        repr=False,
        compare=False,
    )

    def __post_init__(self) -> None:
        require_run_number(self.program_run_id)
        _require_text(self.scenario_id, field="scenario_id", maximum_bytes=128)
        for field_name in (
            "applied_record_digest",
            "source_document_digest",
            "program_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
            "node_environment_digest",
        ):
            _require_digest(getattr(self, field_name), field=field_name)
        if (
            not isinstance(self.owner_identities, tuple)
            or not self.owner_identities
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
            or "program_runner"
            not in {identity.role for identity in self.owner_identities}
        ):
            raise ValueError(
                "owner_identities must be unique, role-sorted, and include "
                "program_runner"
            )
        if (
            not isinstance(self.node_policies, tuple)
            or len(self.node_policies) > 100_000
            or any(
                not isinstance(policy, ProgramNodePolicy)
                for policy in self.node_policies
            )
            or tuple(
                sorted(
                    self.node_policies,
                    key=lambda policy: policy.instance_id,
                )
            )
            != self.node_policies
            or len({policy.instance_id for policy in self.node_policies})
            != len(self.node_policies)
        ):
            raise ValueError(
                "node_policies must be one bounded unique instance-sorted tuple"
            )
        object.__setattr__(
            self,
            "_node_policy_lookup",
            MappingProxyType(
                {policy.instance_id: policy for policy in self.node_policies}
            ),
        )
        policy_bytes = rfc8785.dumps(
            [policy.to_dict() for policy in self.node_policies]
        )
        object.__setattr__(
            self,
            "node_policy_digest",
            "sha256:" + sha256(policy_bytes).hexdigest(),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": PROGRAM_RUN_CAPTURE_API,
            "program_run_id": self.program_run_id,
            "applied_record_digest": self.applied_record_digest,
            "scenario_id": self.scenario_id,
            "source_document_digest": self.source_document_digest,
            "program_revision": self.program_revision,
            "program_binding_id": self.program_binding_id,
            "retained_bundle_id": self.retained_bundle_id,
            "graph_revision": self.graph_revision,
            "node_environment_digest": self.node_environment_digest,
            "node_policies": [policy.to_dict() for policy in self.node_policies],
            "node_policy_digest": self.node_policy_digest,
            "owner_identities": [
                identity.to_dict() for identity in self.owner_identities
            ],
        }

    @property
    def node_policy_by_instance_id(self) -> Mapping[str, ProgramNodePolicy]:
        """Return the runner-owned immutable per-run policy lookup."""

        return self._node_policy_lookup


class ProgramRunAttemptSource:
    """Issue attempt numbers: one counter per run, from 1.

    Attempt numbers correlate owner work and derive the attempt's request
    keys; they are not ordering or freshness authority. Exact run/operation
    generations continue to fence every physical effect.
    """

    def __init__(self, program_run_id: int) -> None:
        self._program_run_id = require_run_number(program_run_id)
        # ``next`` on ``itertools.count`` is atomic in CPython: no lock.
        self._attempts = itertools.count(1)

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        node_id = _require_text(node_instance_id, field="node_instance_id")
        return NodeAttemptIdentity(
            program_run_id=self._program_run_id,
            node_instance_id=node_id,
            node_attempt_id=next(self._attempts),
        )


# The scene-change nodes and the scene operation each one makes (OD-375).
_SCENE_OPERATIONS: Final = MappingProxyType({
    "scene.add_object": "add_object",
    "scene.upsert_object": "upsert_object",
    "scene.remove_object": "remove_object",
    "scene.update_object_pose": "update_object_pose",
    "scene.attach_carried_object": "attach_carried_object",
    "scene.detach_carried_object": "detach_carried_object_at_current_pose",
})
_ATTEMPT_MESSAGE_BYTES: Final = 2048


@dataclass(slots=True)
class _OpenAttempt:
    identity: NodeAttemptIdentity
    node_id: str
    leaf: py_trees.behaviour.Behaviour
    started_ns: int
    started_at_wall: str
    # The failure an operator hold keeps while the leaf runs: its next
    # attempt (Retry) ends this one as failed.
    held_failure: "_AttemptError | None" = None


# A finished attempt's typed error: code, message and bounded details.
_AttemptError = tuple[str, str, Mapping[str, Any] | None]


def _leaf_failure(leaf: py_trees.behaviour.Behaviour) -> _AttemptError:
    return (
        leaf.failure_reason_code or "program_node_failed",
        leaf.failure_message,
        leaf.failure_details,
    )


def _attempt_result(
    leaf: py_trees.behaviour.Behaviour,
) -> tuple[str, _AttemptError | None]:
    """The outcome and error of a leaf's attempt that no longer runs."""

    status = leaf.status
    if status is not py_trees.common.Status.INVALID and getattr(leaf, "preempted", False):
        # Its literal `preempted_result` went to the tree, success or failure.
        return "preempted", None
    if status is py_trees.common.Status.SUCCESS:
        return "succeeded", None
    if status is py_trees.common.Status.FAILURE:
        return "failed", _leaf_failure(leaf)
    outcome = getattr(leaf, "outcome", None)
    if outcome == "interrupted":
        return "interrupted", (leaf.interrupted_reason_code, leaf.interrupted_message, None)
    if outcome == "failure":  # Its stop was not confirmed.
        return "failed", _leaf_failure(leaf)
    return "stopped", None


class _NodeAttemptHistory:
    """Each node attempt of one run, closed exactly once into the history
    sink (OD-375). Tick thread only.

    It wraps the run's attempt source: ``begin`` opens an attempt. The run
    closes the open attempts whose leaf no longer runs after each tick, Halt
    settling and a Stop's settled sent requests (`close_finished`); a node's
    next attempt closes its previous one; an automatic retry closes the
    failed one (`close_retried`).
    """

    def __init__(
        self,
        source: ProgramRunAttemptSource,
        sink: Callable[[Mapping[str, Any]], None],
    ) -> None:
        self._source = source
        self._sink = sink
        self._leaves: dict[str, tuple[str, py_trees.behaviour.Behaviour]] = {}
        self._open: dict[str, _OpenAttempt] = {}

    def add_node(
        self, node_instance_id: str, node_id: str, leaf: py_trees.behaviour.Behaviour
    ) -> None:
        self._leaves[node_instance_id] = (node_id, leaf)

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        previous = self._open.pop(node_instance_id, None)
        if previous is not None:
            # Still open (hold release, operator Retry, re-entry): stopped,
            # unless an operator hold kept its failure.
            held = previous.held_failure
            self._close(previous, "stopped" if held is None else "failed", held)
        identity = self._source.begin(node_instance_id)
        node_id, leaf = self._leaves[node_instance_id]
        self._open[node_instance_id] = _OpenAttempt(
            identity,
            node_id,
            leaf,
            monotonic_ns(),
            datetime.now(timezone.utc).isoformat(),
        )
        return identity

    def close_finished(self) -> None:
        for node_instance_id, attempt in tuple(self._open.items()):
            leaf = attempt.leaf
            if leaf.status is not py_trees.common.Status.RUNNING:
                del self._open[node_instance_id]
                self._close(attempt, *_attempt_result(leaf))
            elif leaf.failure_reason_code and not getattr(leaf, "presented_held", False):
                attempt.held_failure = _leaf_failure(leaf)

    def close_retried(self, node_instance_id: str, code: str | None, message: str | None) -> None:
        attempt = self._open.pop(node_instance_id, None)
        if attempt is not None:
            self._close(attempt, "failed", (code or "program_node_failed", message or "", None))

    def _close(
        self, attempt: _OpenAttempt, outcome: str, error: _AttemptError | None
    ) -> None:
        identity = attempt.identity
        operation = _SCENE_OPERATIONS.get(attempt.node_id)
        inputs = (
            attempt.leaf.attempt_inputs
            if operation is not None and attempt.leaf.attempt_identity is identity
            else None
        )
        # Stamped here, at the attempt's end: delivery to History is later.
        finished_ns = monotonic_ns()
        self._sink({
            "program_run_id": identity.program_run_id,
            "node_instance_id": identity.node_instance_id,
            "node_id": attempt.node_id,
            "node_attempt_id": identity.node_attempt_id,
            "started_at_wall": attempt.started_at_wall,
            "finished_at_ns": str(finished_ns),
            "finished_at_wall": datetime.now(timezone.utc).isoformat(),
            "duration_ms": round((finished_ns - attempt.started_ns) / 1e6, 3),
            "outcome": outcome,
            "error": None if error is None else {
                "code": error[0],
                "message": error[1].encode("utf-8")[:_ATTEMPT_MESSAGE_BYTES].decode(
                    "utf-8", errors="ignore"
                ),
                "details": failure_wire_fields(error[2], None)["details"],
            },
            "scene_operation": operation,
            "scene_resource_id": None if inputs is None else inputs.get("scene_resource_id"),
        })


def _effectful_targets(
    entry: Any, parameters: Mapping[str, Any], program_binding: Mapping[str, Any]
) -> frozenset[tuple[str, str]]:
    """Exact targets named by a node's effectful requirements.

    The same rule the Kernel's captured node table uses for a node's
    effectful target effects: a ``when`` condition and the target parameter
    take the node's literal value, else the catalog default.
    """

    defaults = {
        parameter.id: parameter.default
        for parameter in entry.parameters
        if parameter.has_default
    }

    def effective(parameter_id: str) -> Any:
        return parameters[parameter_id] if parameter_id in parameters else defaults.get(parameter_id)

    targets: set[tuple[str, str]] = set()
    for requirement in entry.required_capabilities:
        if (
            requirement.scope != "target_binding"
            or not requirement.effectful
            or requirement.target_parameter is None
        ):
            continue
        if (
            requirement.when_parameter is not None
            and effective(requirement.when_parameter) != requirement.when_equals
        ):
            continue
        reference = effective(requirement.target_parameter)
        if not isinstance(reference, Mapping):
            raise ProgramRuntimeError("effectful node has no component reference")
        if "signal_name" in reference:
            rows = program_binding.get("signal_bindings", ())
            if not isinstance(rows, (list, tuple)):
                raise ProgramRuntimeError("captured signal bindings are invalid")
            matches = [row for row in rows if isinstance(row, Mapping)
                       and row.get("reference") == reference]
            if len(matches) != 1 or not isinstance(matches[0].get("target"), Mapping):
                raise ProgramRuntimeError("effectful signal has no unique captured target")
            target = matches[0]["target"]
            if target.get("component_id") != reference.get("component_id"):
                raise ProgramRuntimeError("effectful signal target has a different component")
        else:
            target = reference
        component, command = target.get("component_id"), target.get("command_target_id")
        if not isinstance(component, str) or not component or not isinstance(command, str) or not command:
            raise ProgramRuntimeError("effectful node target is invalid")
        targets.add((component, command))
    return frozenset(targets)


def _program_node_ids(node: ProgramTreeNode) -> tuple[str, ...]:
    result = {node.node_id}
    for child in node.children:
        result.update(_program_node_ids(child))
    return tuple(sorted(result))


def _presented_node_status(behaviour: py_trees.behaviour.Behaviour) -> str:
    if getattr(behaviour, "presented_held", False):
        return "held"
    status = getattr(behaviour, "presented_status", behaviour.status)
    if status is py_trees.common.Status.INVALID:
        # A cancellable leaf stopped while running.
        if getattr(behaviour, "presented_interrupted", False):
            return "interrupted"
        # A stop whose cancellation the owner refused is a failure.
        if getattr(behaviour, "presented_stop_failed", False):
            return "failure"
        return "idle"
    return status.name.lower()


class ExecutableProgramRun:
    """Build and tick one exact Program without owning downstream semantics.

    The tree belongs to the run's tick (OD-146): only the thread that ticks
    the run calls into it, so it has no lock. Other threads read the facts
    the tick publishes on the run's owner entry.
    """

    def __init__(
        self,
        *,
        program: Program,
        environment: NodeEnvironment,
        capture: ProgramRunCapture,
        program_binding: Mapping[str, Any],
        readiness: NodeExecutionReadiness,
        ports: NodeAdapterPorts,
        program_clock: ProgramClock | None = None,
        motion_attention: Callable[[str, str], None] | None = None,
        project_path: str | None = None,
    ) -> None:
        # ``project_path``: the Applied Scenario project folder that scopes
        # persistent blackboard values; required when the Program has any.
        if not isinstance(program, Program):
            raise TypeError("program must be one validated Program")
        if not isinstance(environment, NodeEnvironment):
            raise TypeError("environment must be one retained NodeEnvironment")
        if not isinstance(capture, ProgramRunCapture):
            raise TypeError("capture must contain verified Program-run identities")
        if not isinstance(readiness, NodeExecutionReadiness):
            raise TypeError("readiness must contain exact headless facts")
        if not isinstance(ports, NodeAdapterPorts):
            raise TypeError("ports must be one focused owner-port set")
        if program_clock is not None and not isinstance(program_clock, ProgramClock):
            raise TypeError("program_clock must be one ProgramClock")
        if program.program_revision != capture.program_revision:
            raise ProgramRuntimeError(
                "captured Program revision does not match the Program"
            )
        if environment.environment_digest != capture.node_environment_digest:
            raise ProgramRuntimeError(
                "captured node environment does not match the retained environment"
            )
        if program.validated_catalog_digest != environment.catalog.catalog_digest:
            raise ProgramRuntimeError(
                "Program was not validated against the retained environment catalog"
            )
        if readiness.program_binding_id != capture.program_binding_id:
            raise ProgramRuntimeError("readiness facts select another Program Binding")
        if not isinstance(program_binding, Mapping) or program_binding.get("program_binding_id") != capture.program_binding_id:
            raise ProgramRuntimeError("Program Binding does not match the captured identity")

        self._program = program
        self._environment = environment
        self._capture = capture
        motion_target_by_node: dict[str, tuple[str, str]] = {}
        effect_targets_by_node: dict[str, frozenset[tuple[str, str]]] = {}
        pending_nodes = [program.tree]
        while pending_nodes:
            node = pending_nodes.pop()
            if node.node_id in MOTION_NODE_IDS or node.node_id in INTERRUPT_MOTION_NODE_IDS:
                target = node.parameters.get("target")
                if isinstance(target, Mapping):
                    component, command = target.get("component_id"), target.get("command_target_id")
                    if isinstance(component, str) and component and isinstance(command, str) and command:
                        motion_target_by_node[node.instance_id] = (component, command)
            effect_targets_by_node[node.instance_id] = _effectful_targets(
                environment.catalog.get(node.node_id), node.parameters, program_binding
            )
            pending_nodes.extend(node.children)
        self._selection_effect_targets = MappingProxyType({
            group_id: tuple(sorted({
                role
                for node_id in node_ids
                for role in effect_targets_by_node.get(node_id, ())
            }))
            for group_id, node_ids in program.execution_selections.items()
        })
        self._selection_motion_targets = MappingProxyType({
            group_id: tuple(sorted({
                motion_target_by_node[node_id]
                for node_id in node_ids
                if node_id in motion_target_by_node
            }))
            for group_id, node_ids in program.execution_selections.items()
        })
        self._resolver = build_executable_node_resolver(environment)
        self._required_node_ids = _program_node_ids(program.tree)
        self._availability = project_node_execution_availability(
            self._resolver,
            readiness,
            required_node_ids=self._required_node_ids,
        )
        if not self._availability["execution_available"]:
            reasons = ",".join(self._availability["reason_codes"])
            raise ProgramRuntimeError(
                f"Program execution is unavailable for the selected binding: {reasons}"
            )

        self._value_store = ProgramValueStore.from_program(
            program,
            project_path=project_path,
            register=ports.persistent_keys,
            program_run_id=capture.program_run_id,
        )
        self._input_resolver = NodeAttemptInputResolver(
            program=program,
            catalog=environment.catalog,
            value_store=self._value_store,
        )
        self._attempt_source = ProgramRunAttemptSource(capture.program_run_id)
        # Leaves begin their attempts through the history when it records them.
        self._attempt_history = (
            None
            if ports.node_attempt_history is None
            else _NodeAttemptHistory(self._attempt_source, ports.node_attempt_history)
        )
        self._ports = ports
        node_children: dict[str, tuple[str, ...]] = {}

        def capture_topology(node: ProgramTreeNode) -> None:
            for child in node.children:
                capture_topology(child)
            node_children[node.instance_id] = tuple(
                child.instance_id for child in node.children
            )

        capture_topology(program.tree)
        self._scope_node_ids = tuple(sorted(node_children))
        self._scope_captured = False
        # The global rule whose `hold` holds this run, from the latest
        # Kernel scope read (None once Resume releases it); the run end names it.
        self._stopped_by_rule_id: str | None = None
        # Held node -> the Program node whose own Stop holds it (the Kernel
        # scope's `stopped_by: program`, OD-342), from the latest scope read.
        self._program_holds: Mapping[str, str] = MappingProxyType({})
        # What blocked a selection release in the latest Kernel scope read
        # (OD-152): the status read shows it without asking the Kernel again.
        self._selection_release_block_reason: str | None = None
        # The run's one Program clock. Its owner freezes it while the run is
        # halted; waits, the timer and held-time measurement all read it, so
        # a hold that spans a Halt is never subtracted twice.
        self._program_clock = program_clock or ProgramClock()
        # The owner's motion-attention check, and this run's automatic-retry audit facts.
        if motion_attention is not None and not callable(motion_attention):
            raise TypeError("motion_attention must be callable")
        self._motion_attention = motion_attention
        self._node_history_events: list[dict[str, Any]] = []
        self._operator_stop_state = OperatorMotionStopState(
            node_children,
            read_scope=self._read_operator_stop_scope,
            clock=self._program_clock.now,
        )
        self._authored_nodes: dict[str, py_trees.behaviour.Behaviour] = {}
        # Owner-task leaves by node: drained after each tick, and restarted
        # (motion retry budget) when the operator releases a hold over them.
        self._task_adapters: dict[str, OwnerTaskAdapter] = {}
        # Each node's nearest Hold scope (None: Continue up to
        # the root), for the held-node mask of the Run's Mock snapshot.
        self._hold_scopes: dict[str, str | None] = {}
        # The run's ordered sender (OD-213): slow node requests leave the
        # tick. Bounded by the node count (one queued request per node).
        self._sender = RunSender(
            len(node_children),
            name=f"manyforge-program-{capture.program_run_id}-sender",
        )
        # Leaves whose sent request a Forced Program End still awaits.
        self._sender_adapters: list[Any] = []
        # Leaves that ask for the Program's own motion-group Stop (Ask
        # operator, Stop motion groups) by node; the questions by node.
        self._program_stop_adapters: dict[str, Any] = {}
        self._question_adapters: dict[str, OperatorRequestChoice] = {}
        root = self._build_node(
            program.tree,
            inherited_stop_policy="continue",
            root=True,
        )
        self._tree = py_trees.trees.BehaviourTree(root=root)
        self._tree.setup()
        self._stopped = False
        # Unconfirmed Mock ENDs/pause states already reported.
        self._mock_ends_reported: set[tuple[str, str]] = set()

    def _build_node(
        self,
        node: ProgramTreeNode,
        *,
        inherited_stop_policy: str,
        hold_scope: str | None = None,
        root: bool = False,
    ) -> py_trees.behaviour.Behaviour:
        authored_stop_policy = node.operator_stop_policy or "inherit"
        entry = self._environment.catalog.get(node.node_id)
        if authored_stop_policy == "inherit":
            effective_stop_policy = (
                "continue"
                if root
                else (
                    inherited_stop_policy
                    if entry.kind in {"composite", "decorator"}
                    else "hold"
                )
            )
        else:
            effective_stop_policy = authored_stop_policy
        # The nearest hold decorator at or above this node: the one whose
        # held subtree stops ticking it.
        if effective_stop_policy == "hold":
            hold_scope = node.instance_id
        children = tuple(
            self._build_node(
                child,
                inherited_stop_policy=effective_stop_policy,
                hold_scope=hold_scope,
            )
            for child in node.children
        )
        row = self._environment.registry.record(node.node_id, "py_trees")
        built = self._resolver.instantiate(
            row,
            NodeAdapterBuildContext(
                name=node.instance_id,
                node_instance_id=node.instance_id,
                parameters=node.parameters,
                children=children,
                value_sources=node.value_sources,
                input_resolver=self._input_resolver,
                attempt_source=self._attempt_history or self._attempt_source,
                ports=self._ports,
                operator_stop_state=self._operator_stop_state,
                operator_stop_hold_scope=hold_scope,
                program_clock=self._program_clock,
                motion_attention=self._motion_attention,
                audit_sink=self._record_node_history_event,
                run_sender=self._sender,
                execution_selections=self._program.execution_selections,
            ),
        )
        if self._attempt_history is not None:
            self._attempt_history.add_node(node.instance_id, node.node_id, built)
        if callable(getattr(built, "program_stop_request", None)):
            self._program_stop_adapters[node.instance_id] = built
        if isinstance(built, OperatorRequestChoice):
            self._question_adapters[node.instance_id] = built
        if isinstance(built, OwnerTaskAdapter):
            self._task_adapters[node.instance_id] = built
        if callable(getattr(built, "settle_abandoned_request", None)):
            self._sender_adapters.append(built)
        self._hold_scopes[node.instance_id] = hold_scope
        policy = self._capture.node_policy_by_instance_id.get(node.instance_id)
        recovery_port = self._ports.program_interventions
        if (
            policy is not None
            and recovery_policy_is_actionable(policy)
            and recovery_port is not None
        ):
            if isinstance(built, OwnerTaskAdapter):
                # The leaf's own hold keeps its failed operation live for the
                # operator's owner actions (Retry, Continue) instead of closing.
                built.hold_failures_for_intervention()
            built = RecoveryAwareNode(
                child=built,
                node_instance_id=node.instance_id,
                policy=policy,
                # Its fallback identity keys the hold of the leaf's own
                # attempt: not another attempt of the history.
                attempt_source=self._attempt_source,
                recovery_port=recovery_port,
            )
        if effective_stop_policy == "hold":
            built = OperatorMotionStopPolicy(
                child=built,
                node_instance_id=node.instance_id,
                stop_state=self._operator_stop_state,
                drain_children=entry.kind in {"composite", "decorator"},
            )
        self._authored_nodes[node.instance_id] = built
        return built

    @property
    def capture(self) -> ProgramRunCapture:
        return self._capture

    @property
    def program_clock(self) -> ProgramClock:
        """The run-scoped clock; the run's owner freezes it during a Halt."""

        return self._program_clock

    @property
    def availability(self) -> dict[str, Any]:
        return deepcopy(self._availability)

    @property
    def value_store(self) -> ProgramValueStore:
        return self._value_store

    def preview_node_inputs(
        self,
        node_instance_id: str,
        proposed: Sequence[tuple[str, Any]],
    ) -> tuple[str, Mapping[str, Any], Mapping[str, Any]]:
        """Resolve one node's inputs as a fresh attempt would, with proposals.

        Effect-free: one atomic read through the run's own input resolver. It
        mints no attempt, ticks nothing and writes no key. A proposal replaces
        the value at its parameter value path and shadows any value source at
        or beneath that path. Proposals cannot select a target, component,
        capability or blackboard key, and of a collision rule they can change
        only a row's clearance: the rest keeps the captured meaning. Raises ``NodeInputProposalError`` for a foreign or
        invalid proposal; a sourced input that cannot be resolved now raises
        the resolver's own ``ProgramValueError``.
        """

        pending = [self._program.tree]
        node: ProgramTreeNode | None = None
        while pending:
            candidate = pending.pop()
            if candidate.instance_id == node_instance_id:
                node = candidate
                break
            pending.extend(candidate.children)
        if node is None:
            raise NodeInputProposalError(
                f"node instance {node_instance_id!r} is not in the captured Program"
            )
        catalog = self._environment.catalog
        entry = catalog.get(node.node_id)
        declarations = {parameter.id: parameter for parameter in entry.parameters}
        fixed = set(_BINDING_SELECTION_PARAMETERS)
        fixed.update(
            parameter
            for requirement in entry.required_capabilities
            for parameter in (
                requirement.target_parameter,
                requirement.component_parameter,
                requirement.when_parameter,
            )
            if parameter is not None
        )
        fixed.update(
            binding.key_parameter
            for binding in (*entry.blackboard_reads, *entry.blackboard_writes)
        )
        prefix = f"node[{node.node_id}].parameters."

        def validate(parameter_id: str, value: Any) -> None:
            issues = validate_value(
                declarations[parameter_id].schema,
                value,
                registry=catalog.schema_registry,
                path=f"{prefix}{parameter_id}",
            )
            if issues:
                raise NodeInputProposalError(f"{issues[0].path}: {issues[0].message}")

        proposals: list[tuple[str, tuple[Any, ...], Any]] = []
        for path, value in proposed:
            try:
                segments = parse_value_path(path)
            except ValuePathError as error:
                raise NodeInputProposalError(
                    f"proposed input path {path!r} is invalid: {error}"
                ) from error
            root = segments[0]
            if root not in declarations:
                raise NodeInputProposalError(
                    f"node {node.node_id!r} has no parameter {root!r}"
                )
            if root in fixed:
                raise NodeInputProposalError(
                    f"node parameter {root!r} selects captured binding meaning"
                )
            # A collision rule's selections name captured collision bodies;
            # only a row's clearance is a plain value of the move.
            if root == "collision_rules" and not (
                len(segments) == 3 and segments[2] == "clearance_m"
            ):
                raise NodeInputProposalError(
                    "only the clearance_m of a collision rule row can be proposed"
                )
            if any(
                path_covers(segments, other) or path_covers(other, segments)
                for _path, other, _value in proposals
            ):
                raise NodeInputProposalError(
                    f"proposed input path {path!r} overlaps another proposal"
                )
            if len(segments) == 1:
                validate(root, value)
            proposals.append((path, segments, deepcopy(value)))
        value_sources = {
            source_path: source
            for source_path, source in node.value_sources.items()
            if not any(
                path_covers(segments, parse_value_path(source_path))
                for _path, segments, _value in proposals
            )
        }
        parameters = deepcopy(dict(node.parameters))
        for _path, segments, value in proposals:
            if len(segments) == 1:
                parameters[segments[0]] = deepcopy(value)
        snapshot = self._input_resolver.snapshot(
            node_id=node.node_id,
            node_instance_id=node.instance_id,
            parameters=parameters,
            value_sources=value_sources,
            derived_active_tcp_key=_derived_active_tcp_key(node.node_id, node.parameters),
        )
        resolved = deepcopy(dict(snapshot.parameters))
        for path, segments, value in proposals:
            root = segments[0]
            try:
                resolved[root] = replace_at_path(resolved.get(root), segments, value)
            except KeyError as error:
                raise NodeInputProposalError(
                    f"proposed input {path!r} names no value of node {node.node_id!r}"
                ) from error
        for root in {segments[0] for _path, segments, _value in proposals}:
            validate(root, resolved[root])
            key_parameter = catalog.schema_registry.resolve(
                declarations[root].schema
            ).value_type_from_key
            if key_parameter is not None:
                try:
                    self._program.validate_key_value(
                        resolved[key_parameter], resolved[root], path=f"{prefix}{root}"
                    )
                except (KeyError, ValueError) as error:
                    raise NodeInputProposalError(str(error)) from error
        return node.node_id, MappingProxyType(resolved), snapshot.blackboard

    @property
    def status(self) -> py_trees.common.Status:
        return self._tree.root.status

    def _record_node_history_event(self, event: dict[str, Any]) -> None:
        """One node history event (`kind` plus its facts: an automatic retry,
        a logical preemption), drained with the run's status."""

        self._node_history_events.append(dict(event))
        if (
            event.get("kind") == "program_node_automatic_retry"
            and self._attempt_history is not None
        ):
            # Emitted before the retry's own attempt begins.
            self._attempt_history.close_retried(
                event["node_instance_id"], event["code"], event["message"]
            )

    def take_node_history_events(self) -> tuple[dict[str, Any], ...]:
        """Node history events decided since the last take."""

        events, self._node_history_events = self._node_history_events, []
        return tuple(events)

    def note_operator_release(self, node_ids: frozenset[str]) -> None:
        """An accepted operator release (Resume, Run, Retry, Continue) freed
        these nodes: each motion node's automatic retry budget restarts."""

        for node_id in node_ids:
            adapter = self._task_adapters.get(node_id)
            if isinstance(adapter, MotionTaskAdapter):
                adapter.restart_retry_series()

    def simulation_stop_nodes(self) -> frozenset[str]:
        """Nodes refused with `simulation_not_playing` whose operator
        Stop the run owner hasn't sent yet."""

        return frozenset(
            node_id
            for node_id, adapter in self._task_adapters.items()
            if adapter.simulation_stop_pending
        )

    def note_simulation_stops(self, node_ids: frozenset[str]) -> None:
        """The operator Stop for these nodes' simulation holds was accepted."""

        for node_id in node_ids:
            adapter = self._task_adapters.get(node_id)
            if adapter is not None:
                adapter.simulation_stop_sent()

    def program_stop_requests(
        self,
    ) -> tuple[tuple[str, int, tuple[str, ...], bytes], ...]:
        """The Program's own motion-group Stops its nodes asked for and the
        Kernel has not accepted yet: ``(node, attempt, groups, key)``. The
        run's tick sends them after the tree tick."""

        result = []
        for node_id, adapter in sorted(self._program_stop_adapters.items()):
            request = adapter.program_stop_request()
            if request is not None:
                result.append((node_id, *request))
        return tuple(result)

    def note_program_stop_accepted(
        self, node_instance_id: str, node_attempt_id: int, hold_revision: int
    ) -> None:
        """The Kernel accepted that attempt's Stop at ``hold_revision``."""

        adapter = self._program_stop_adapters.get(node_instance_id)
        if adapter is not None:
            adapter.note_program_stop_accepted(node_attempt_id, hold_revision)

    def operator_questions(self) -> tuple[Mapping[str, Any], ...]:
        """The open operator questions, by node. Tick only: the tick
        publishes them for the other threads."""

        return tuple(
            question
            for _node_id, adapter in sorted(self._question_adapters.items())
            if (question := adapter.question()) is not None
        )

    def answer_operator_question(
        self, node_instance_id: str, node_attempt_id: int, option_id: str
    ) -> Mapping[str, Any]:
        """Hand the operator's answer to the open question. Tick only.

        Returns the chosen option; raises ``ProgramRuntimeError`` when the
        question is no longer open or has no such option."""

        adapter = self._question_adapters.get(node_instance_id)
        if adapter is None:
            raise ProgramRuntimeError("the operator question is not in this capture")
        try:
            return adapter.answer(node_attempt_id, option_id)
        except ValueError as error:
            raise ProgramRuntimeError(str(error)) from error

    def node_motion_groups(self, node_instance_id: str) -> tuple[str, ...]:
        """The named execution groups containing this node, or ``("all",)``
        for a node in no named group (only "all" addresses it)."""

        groups = tuple(
            group
            for group, members in self._program.execution_selections.items()
            if group != "all" and node_instance_id in members
        )
        return groups or ("all",)

    def operation_failure(self, node_instance_id: str | None = None) -> dict[str, Any] | None:
        """The operation failure at the failed tip of a held node's subtree
        (or of the failed tree): its node attempt and the Kernel's facts."""

        behaviour = (
            self._tree.root
            if node_instance_id is None
            else self._authored_nodes.get(node_instance_id)
        )
        tip = None if behaviour is None else behaviour.tip()
        fact = getattr(tip, "operation_failure", None)
        identity = getattr(tip, "attempt_identity", None)
        if not isinstance(fact, Mapping) or not isinstance(identity, NodeAttemptIdentity):
            return None
        return {
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            **deepcopy(dict(fact)),
        }

    @property
    def node_statuses(self) -> Mapping[str, str]:
        """Return exact authored-node phases from the current tree tick.

        ``held`` is a ticking node whose owner work waits for motion to start
        (held before admission); it would otherwise present as idle.
        """

        return MappingProxyType(
            {
                instance_id: _presented_node_status(behaviour)
                for instance_id, behaviour in self._authored_nodes.items()
            }
        )

    def tick_once(self) -> py_trees.common.Status:
        """Advance at most one tree tick; scheduling remains the host's job."""

        if self._stopped:
            raise ProgramRuntimeError("a stopped Program run cannot be ticked")
        if self._tree.root.status in {
            py_trees.common.Status.SUCCESS,
            py_trees.common.Status.FAILURE,
        }:
            return self._tree.root.status
        self._tree.tick()
        for adapter in self._task_adapters.values():
            # Abandoned (INVALID) attempts close once their Stop settled.
            if adapter.has_pending_drain:
                adapter.drain_invalidated()
        if self._attempt_history is not None:
            self._attempt_history.close_finished()
        status = self._tree.root.status
        self._settle_mock_ends(
            final=status in {py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}
        )
        return status

    def _settle_mock_ends(self, *, final: bool) -> None:
        """Resend this Run's unanswered node ENDs unchanged on
        every tick; when the tree ends with an END still unanswered, its
        cancellation is unproven and the history says so (once)."""

        port = getattr(self._ports, "mock_simulation", None)
        resend = getattr(port, "resend_unknown_ends", None)
        if resend is None:
            return
        run_id = self._capture.program_run_id
        try:
            pending = resend(run_id)
            superseded, current = port.resend_run_pause(run_id)
        except Exception:
            return
        self._report_unconfirmed_pauses(
            (*superseded, *((current,) if final and current else ()))
        )
        if not final:
            return
        for request in pending:
            key = (request["node_attempt_id"], request["node_start_index"])
            if key in self._mock_ends_reported:
                continue
            self._mock_ends_reported.add(key)
            self._record_node_history_event(
                {
                    "kind": "program_node_mock_simulation_end_unconfirmed",
                    "node_instance_id": request["node_instance_id"],
                    "node_attempt_id": request["node_attempt_id"],
                    "node_start_index": request["node_start_index"],
                }
            )

    def mock_held_nodes(self) -> tuple[str, ...]:
        """S2b: the sorted node instance ids whose nearest Hold wrapper is
        wholly held: a pure function of the selection state."""

        return tuple(sorted(
            node_id
            for node_id, hold_scope in self._hold_scopes.items()
            if hold_scope is not None and self._operator_stop_state.holds_subtree(hold_scope)
        ))

    def diagnostic_snapshot(self) -> dict[str, Any]:
        """Capture local values at the owner transition; no remote reads or I/O."""
        blackboard = self._value_store.diagnostic_snapshot()
        return {
            "occurred_at_ns": str(monotonic_ns()),
            "occurred_at_wall": datetime.now(timezone.utc).isoformat(),
            "blackboard": blackboard["values"],
            "omitted_blackboard_count": blackboard["omitted_count"],
        }

    def reconcile_halt_tasks(
        self, *, node_ids: frozenset[str] | None = None
    ) -> tuple[int, int, int]:
        """Observe only accepted Program-scope tasks while the tree is frozen.

        The result is ``(active, held, unrecoverable)``. Repeating this after
        one exact retry resolution consumes that resolution and exposes the
        next parallel held task without ticking or submitting new work.
        """

        states = tuple(
            self.task_control_states(node_ids=node_ids, settle=True).values()
        )
        if self._attempt_history is not None:
            self._attempt_history.close_finished()
        return (
            states.count("active"),
            states.count("held"),
            states.count("unrecoverable"),
        )

    def task_control_states(
        self,
        *,
        node_ids: frozenset[str] | None = None,
        settle: bool = False,
        observe: bool = True,
    ) -> dict[str, str]:
        """Read current tasks; only explicit reconciliation may settle them.

        Selected Resume also joins other owners' existing task settlement;
        a Kernel hold alone cannot prove their work ended. Halt retains
        its existing physical task scope and distinct cancellation contract.
        """
        result = {}
        for behaviour in self._tree.root.iterate():
            if node_ids is not None and behaviour.name not in node_ids:
                continue
            read = getattr(
                behaviour,
                (
                    "reconcile_halt_task"
                    if settle
                    else "observe_control_state"
                    if observe
                    else "cached_control_state"
                ),
                None,
            )
            if callable(read):
                result[behaviour.name] = (
                    read(selected_resume=node_ids is not None) if settle else read()
                )
        return result

    def _read_operator_stop_scope(self) -> OperatorMotionStopSnapshot:
        scope_port = self._ports.program_run_scope
        if scope_port is None:
            raise ProgramRuntimeError("operator Stop requires a Kernel scope port")
        return self._decode_operator_stop_scope(
            scope_port.execution_scope(self._capture.program_run_id)
        )

    def _decode_operator_stop_scope(
        self, scope: Mapping[str, Any]
    ) -> OperatorMotionStopSnapshot:
        if (
            not isinstance(scope, Mapping)
            or scope.get("schema_version") != "manyforge.program_execution_scope.v4"
            or scope.get("program_run_id") != self._capture.program_run_id
            or type(scope.get("hold_revision")) is not int
            or not 0 <= scope["hold_revision"] <= 2**53 - 1
        ):
            raise ProgramRuntimeError("operator Stop projection changed its capture")
        rule_id = scope.get("rule_id")
        self._stopped_by_rule_id = (
            rule_id
            if scope.get("stopped_by") == "rule" and isinstance(rule_id, str) and rule_id
            else None
        )
        pairs = scope.get("program_held_nodes")
        if not isinstance(pairs, (list, tuple)) or len(pairs) > len(self._scope_node_ids):
            raise ProgramRuntimeError(
                "operator Stop projection has invalid Program node holds"
            )
        program_holds: dict[str, str] = {}
        for pair in pairs:
            if (
                not isinstance(pair, (list, tuple))
                or len(pair) != 2
                or any(
                    type(index) is not int or not 0 <= index < len(self._scope_node_ids)
                    for index in pair
                )
            ):
                raise ProgramRuntimeError(
                    "operator Stop projection has invalid Program node holds"
                )
            program_holds[self._scope_node_ids[pair[0]]] = self._scope_node_ids[pair[1]]
        self._program_holds = MappingProxyType(program_holds)
        release_block_reason = scope.get("selection_release_block_reason")
        if release_block_reason is not None and (
            not isinstance(release_block_reason, str)
            or release_block_reason not in {
                "cell_motion_admission_closed",
                "program_run_admission_pending",
                "program_halt_active",
                "stopping",
            }
        ):
            raise ProgramRuntimeError(
                "operator Stop projection has an invalid selection release blocker"
            )
        self._selection_release_block_reason = release_block_reason
        held: set[str] = set()
        phases: dict[str, frozenset[str]] = {}
        for field in (
            "ready_node_indices",
            "held_node_indices",
            "stopping_node_indices",
            "faulted_node_indices",
        ):
            indices = scope.get(field)
            if not isinstance(indices, (list, tuple)) or len(indices) > len(
                self._scope_node_ids
            ):
                raise ProgramRuntimeError(
                    "operator Stop projection has invalid node indices"
                )
            previous = -1
            for index in indices:
                if type(index) is not int or not previous < index < len(
                    self._scope_node_ids
                ):
                    raise ProgramRuntimeError(
                        "operator Stop projection has invalid node indices"
                    )
                node_id = self._scope_node_ids[index]
                if node_id in held:
                    raise ProgramRuntimeError("operator Stop projection repeats a node")
                held.add(node_id)
                previous = index
            phases[field] = self.scope_node_ids(indices)
        self._scope_captured = True
        return OperatorMotionStopSnapshot(
            scope["hold_revision"],
            frozenset(held),
            phases["stopping_node_indices"],
            phases["faulted_node_indices"],
            phases["ready_node_indices"],
        )

    def refresh_pending_stop_settlement(self) -> None:
        """Use the existing tree worker only until accepted containment settles."""
        if (
            self._operator_stop_state.snapshot().stopping_nodes
            or self._operator_stop_state.observation_error() is not None
        ):
            try:
                self._operator_stop_state.refresh()
            except Exception:
                # An observation outage is not a failed tree or proof of rest.
                # Keep the exact holds; the next settling tick retries the read.
                pass

    @property
    def stop_observation_error(self) -> str | None:
        return self._operator_stop_state.observation_error()

    def take_stop_observation_change(self) -> bool:
        return self._operator_stop_state.take_observation_change()

    def pending_selected_observation(self) -> bool:
        """Cached scheduling query; never reads an owner or grants admission.
        The run's tick publishes it for the scheduler."""
        scope = self._operator_stop_state.snapshot()
        if (
            scope.stopping_nodes
            or self._operator_stop_state.observation_error() is not None
        ):
            return True
        if not scope.held_nodes:
            return False
        return any(
            node.name in scope.held_nodes
            and getattr(node, "has_active_task", False)
            for node in self._tree.root.iterate()
        )

    @property
    def selection_release_block_reason(self) -> str | None:
        """The release blocker of the latest Kernel scope read."""
        return self._selection_release_block_reason

    @property
    def program_holds(self) -> Mapping[str, str]:
        """Held node -> the Program node whose own Stop holds it, from the
        latest Kernel scope read (`stopped_by: program`)."""
        return self._program_holds

    @property
    def stopped_by_rule_id(self) -> str | None:
        """The rule whose `hold` holds this run, from the latest scope read."""
        return self._stopped_by_rule_id

    def settle_mock_pause(self) -> None:
        """Send the Run's unanswered Mock pause state (entering Halt only
        records it); report superseded states never answered. Tick worker."""

        port = getattr(self._ports, "mock_simulation", None)
        resend = getattr(port, "resend_run_pause", None)
        if resend is None:
            return
        try:
            superseded, _current = resend(self._capture.program_run_id)
        except Exception:
            return
        self._report_unconfirmed_pauses(superseded)

    def _report_unconfirmed_pauses(self, facts: Sequence[Mapping[str, Any]]) -> None:
        # A Halt state never answered: the Mock may not have paused/resumed.
        for fact in facts:
            key = ("pause", fact["run_revision"])
            if key in self._mock_ends_reported:
                continue
            self._mock_ends_reported.add(key)
            self._record_node_history_event(
                {"kind": "program_mock_simulation_pause_unconfirmed", **fact}
            )

    def refresh_operator_stop_scope(self) -> None:
        """Refresh scheduling only, never issue or release physical containment."""

        self._operator_stop_state.refresh()

    def read_operator_stop_scope(self) -> Mapping[str, Any]:
        """Validate fresh Resume evidence without publishing a worker observation."""
        scope_port = self._ports.program_run_scope
        if scope_port is None:
            raise ProgramRuntimeError("operator Stop requires a Kernel scope port")
        scope = scope_port.execution_scope(self._capture.program_run_id)
        self._decode_operator_stop_scope(scope)
        return scope

    def operator_stop_scope(self) -> OperatorMotionStopSnapshot:
        return self._operator_stop_state.snapshot()

    def initial_ready_nodes(self) -> frozenset[str]:
        return self._operator_stop_state.initial_ready_nodes()

    def scope_node_ids(self, indices: Sequence[int]) -> frozenset[str]:
        """Translate indices from an already validated captured scope."""
        return frozenset(self._scope_node_ids[index] for index in indices)

    def publish_operator_stop_scope(
        self,
        scope: Mapping[str, Any],
        *,
        expected_revision: int | None = None,
        released_nodes: frozenset[str] = frozenset(),
        notify_observation: bool = True,
    ) -> None:
        """Apply the accepted owner result without a tree lock or another read."""
        snapshot = self._decode_operator_stop_scope(scope)
        if (
            expected_revision is not None
            and snapshot.hold_revision != expected_revision
        ) or released_nodes & snapshot.held_nodes:
            raise ProgramRuntimeError(
                "selection release did not release its exact scope"
            )
        self._operator_stop_state.publish(
            snapshot, notify_observation=notify_observation
        )

    def execution_selection(self, group_id: str | list[str]) -> frozenset[str]:
        try:
            groups = [group_id] if isinstance(group_id, str) else group_id
            return frozenset(
                node
                for group in groups
                for node in self._program.execution_selections[group]
            )
        except KeyError as error:
            raise ProgramRuntimeError(
                "execution selection is not in this capture"
            ) from error

    @property
    def execution_selections(self) -> Mapping[str, tuple[str, ...]]:
        return self._program.execution_selections

    def execution_selection_motion_targets(
        self, group_id: str
    ) -> tuple[tuple[str, str], ...]:
        """Return captured component/target identities in one selection.

        The selection is resolved from the immutable Program captured for this
        run.  It is presentation metadata only: command admission still uses
        the original group id and the Kernel's own target resolution.
        """

        try:
            return self._selection_motion_targets[group_id]
        except KeyError as error:
            raise ProgramRuntimeError(
                "execution selection is not in this capture"
            ) from error

    def execution_selection_effect_targets(
        self, group_id: str
    ) -> tuple[tuple[str, str], ...]:
        """Return the component/target identities of effectful nodes.

        With the sealed Program Binding these name the exact resources the
        Kernel reserves for the selection's Run/Resume; presentation uses
        them to anticipate operator-alarm motion attention.
        """

        try:
            return self._selection_effect_targets[group_id]
        except KeyError as error:
            raise ProgramRuntimeError(
                "execution selection is not in this capture"
            ) from error

    def execution_selection_request(
        self,
        group_id: str | list[str],
        request_key: bytes,
        *,
        node_instance_id: str | None = None,
        stopped_by_node_instance_id: str | None = None,
    ) -> dict[str, Any]:
        """The selection DTO. ``node_instance_id`` (release only)
        names one captured held node with ``group_ids: []``;
        ``stopped_by_node_instance_id`` (Stop only) names the Program node
        whose own Stop it is: the Kernel attributes the holds to it."""
        if not self._scope_captured:
            raise ProgramRuntimeError("Program execution scope is not captured")
        if node_instance_id is not None and (
            group_id != [] or node_instance_id not in self._authored_nodes
        ):
            raise ProgramRuntimeError(
                "a node release names one captured node and no group"
            )
        # Do not resolve Stop membership here: the Kernel owns conservative
        # containment of an unknown selection as well as valid selections.
        request = {
            "schema_version": "manyforge.program_execution_selection.v3",
            "program_run_id": self._capture.program_run_id,
            "group_ids": [group_id] if isinstance(group_id, str) else list(group_id),
            "request_context": {
                "request_key": format_request_key(request_key),
                "source_label": "program_selection_control",
            },
        }
        if node_instance_id is not None:
            request["node_instance_id"] = node_instance_id
        if stopped_by_node_instance_id is not None:
            if stopped_by_node_instance_id not in self._authored_nodes:
                raise ProgramRuntimeError("the stopping node is not in this capture")
            request["stopped_by_node_instance_id"] = stopped_by_node_instance_id
        return request

    def node_motion_attention_request(
        self, node_instance_id: str, request_key: bytes
    ) -> dict[str, Any]:
        """Kernel no-effect attention check for one captured node."""
        if not self._scope_captured:
            raise ProgramRuntimeError("Program execution scope is not captured")
        if node_instance_id not in self._authored_nodes:
            raise ProgramRuntimeError("node is not in this capture")
        return {
            "schema_version": "manyforge.program_node_motion_attention_check.v2",
            "program_run_id": self._capture.program_run_id,
            "node_instance_id": node_instance_id,
            "request_context": {
                "request_key": format_request_key(request_key),
                "source_label": "program_recovery_control",
            },
        }

    def terminal_failure(self) -> FailureRecord:
        """Return the exact failure at a failed tree tip.

        The tip's own reason, message, details and origin, attributed to its
        authored node instance. A tip that is not one exact authored node, or
        names no bounded reason, is the runner's generic ``program_tree_failed``.
        """

        if self._tree.root.status is not py_trees.common.Status.FAILURE:
            raise ProgramRuntimeError(
                "terminal failure is available only for a failed Program tree"
            )
        tip = self._tree.tip()
        node_instance_id = (
            tip.name
            if tip is not None and tip.name in self._authored_nodes
            else None
        )
        reason_code = getattr(tip, "failure_reason_code", None)
        message = getattr(tip, "failure_message", "")
        if (
            node_instance_id is None
            or not isinstance(reason_code, str)
            or not reason_code
            or len(reason_code.encode("utf-8")) > 256
            or not isinstance(message, str)
            or len(message.encode("utf-8")) > 4_096
        ):
            return FailureRecord(
                "program_tree_failed",
                "Program tree failed",
                None,
                failure_origin("program_runner", node_instance_id=node_instance_id),
            )
        return FailureRecord(
            reason_code,
            message,
            tip.failure_details,
            tip.failure_origin.for_node(node_instance_id),
        )

    def stop(self) -> None:
        """Invalidate the tree once, causing active leaves to request cancel.

        Requests not sent yet are withdrawn and the sender ends with the run;
        a sent one completes (``settle_sent_requests``)."""

        if self._stopped:
            return
        self._stopped = True
        self._tree.root.stop(py_trees.common.Status.INVALID)
        self._sender.close()
        self._settle_mock_ends(final=True)

    def withdraw_unsent_requests(self) -> None:
        """A Halt froze the run: nothing queued on its sender is sent. Each
        node sends its request again when it is ticked after Execute."""

        self._sender.withdraw_queued()

    def settle_sent_requests(self) -> bool:
        """Forced Program End (OD-213): each stopped node whose request was
        already sent ends with that request's real result. True once no sent
        request is outstanding; the run's tick asks again until then."""

        settled = all([adapter.settle_abandoned_request() for adapter in self._sender_adapters])
        if settled and self._attempt_history is not None:
            # The attempts the Stop ended close here, a sent request's
            # with its real result.
            self._attempt_history.close_finished()
        return settled

    def close_sender(self) -> None:
        """Runner shutdown: nothing queued is sent any more."""

        self._sender.close()


__all__ = [
    "ExecutableProgramRun",
    "NodeInputProposalError",
    "PROGRAM_RUN_CAPTURE_API",
    "ProgramRunAttemptSource",
    "ProgramRunCapture",
    "ProgramRunOwnerIdentity",
    "ProgramRuntimeError",
]
