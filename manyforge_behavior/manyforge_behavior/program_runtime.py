"""One bounded py_trees Program run over exact retained identities."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass, field
from hashlib import sha256
from threading import RLock
from types import MappingProxyType
from typing import Any, Final
from uuid import uuid4
from datetime import datetime, timezone
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
)
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.orchestration_ports import NodeAdapterPorts, NodeAttemptIdentity
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
from manyforge_behavior.nodes.v2_owner import MotionTaskAdapter, OwnerTaskAdapter
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

PROGRAM_RUN_CAPTURE_API: Final = "manyforge.program_run_capture.v2"

class ProgramRuntimeError(RuntimeError):
    """A Program run cannot be built or advanced truthfully."""


class NodeInputProposalError(ValueError):
    """A proposed node input is foreign, selects captured meaning, or is invalid."""


# Parameters that name a captured Program Binding row by value. Required
# capability parameters and declared blackboard-key parameters are read from
# the catalog entry itself.
_BINDING_SELECTION_PARAMETERS: Final = frozenset({"contact_policy_id", "tcp_endpoint"})


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
    """One exact downstream or orchestration owner captured for a run."""

    owner_role: str
    owner_id: str
    owner_epoch: str

    def __post_init__(self) -> None:
        _require_text(self.owner_role, field="owner_role", maximum_bytes=128)
        _require_text(self.owner_id, field="owner_id")
        _require_text(self.owner_epoch, field="owner_epoch")

    def to_dict(self) -> dict[str, str]:
        return {
            "owner_role": self.owner_role,
            "owner_id": self.owner_id,
            "owner_epoch": self.owner_epoch,
        }


@dataclass(frozen=True, slots=True)
class ProgramRunCapture:
    """Verified identities captured before one Program run is constructed."""

    program_run_id: str
    applied_record_digest: str
    scenario_id: str
    source_document_digest: str
    program_id: str
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
        _require_text(self.program_run_id, field="program_run_id")
        _require_text(self.scenario_id, field="scenario_id", maximum_bytes=128)
        _require_text(self.program_id, field="program_id")
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
                    key=lambda identity: identity.owner_role,
                )
            )
            != self.owner_identities
            or len({identity.owner_role for identity in self.owner_identities})
            != len(self.owner_identities)
            or "program_runner"
            not in {identity.owner_role for identity in self.owner_identities}
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
            "program_id": self.program_id,
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
    """Issue opaque attempt identities without retaining completed attempts.

    Attempt IDs correlate owner work; they are not ordering or freshness
    authority. UUIDs remove the old per-node lifetime counter and its limit.
    Exact run/operation generations continue to fence every physical effect.
    """

    def __init__(self, program_run_id: str) -> None:
        self._program_run_id = _require_text(
            program_run_id,
            field="program_run_id",
        )

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        node_id = _require_text(node_instance_id, field="node_instance_id")
        return NodeAttemptIdentity(
            program_run_id=self._program_run_id,
            node_instance_id=node_id,
            node_attempt_id=f"attempt:{uuid4().hex}",
        )


def _effectful_target_roles(entry: Any, parameters: Mapping[str, Any]) -> frozenset[str]:
    """Target roles a node's effectful target-binding requirements name.

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

    roles: set[str] = set()
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
        role = effective(requirement.target_parameter)
        if isinstance(role, str) and role:
            roles.add(role)
    return frozenset(roles)


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
        # A cancellable leaf stopped while running (spec 440 §4.6).
        if getattr(behaviour, "presented_interrupted", False):
            return "interrupted"
        # A stop whose cancellation the owner refused is a failure.
        if getattr(behaviour, "presented_stop_failed", False):
            return "failure"
        return "idle"
    return status.name.lower()


class ExecutableProgramRun:
    """Build and tick one exact Program without owning downstream semantics."""

    def __init__(
        self,
        *,
        program: Program,
        environment: NodeEnvironment,
        capture: ProgramRunCapture,
        readiness: NodeExecutionReadiness,
        ports: NodeAdapterPorts,
        program_clock: ProgramClock | None = None,
        motion_attention: Callable[[str, str], None] | None = None,
        transport_timeout_s: float | None = None,
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
        if program.program_id != capture.program_id:
            raise ProgramRuntimeError("captured Program ID does not match the Program")
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

        self._program = program
        self._environment = environment
        self._capture = capture
        motion_target_by_node: dict[str, str] = {}
        effect_targets_by_node: dict[str, frozenset[str]] = {}
        pending_nodes = [program.tree]
        while pending_nodes:
            node = pending_nodes.pop()
            if node.node_id in MOTION_NODE_IDS or node.node_id in INTERRUPT_MOTION_NODE_IDS:
                target_role = node.parameters.get("target_role")
                if isinstance(target_role, str) and target_role:
                    motion_target_by_node[node.instance_id] = target_role
            effect_targets_by_node[node.instance_id] = _effectful_target_roles(
                environment.catalog.get(node.node_id), node.parameters
            )
            pending_nodes.extend(node.children)
        self._selection_effect_target_roles = MappingProxyType({
            group_id: tuple(sorted({
                role
                for node_id in node_ids
                for role in effect_targets_by_node.get(node_id, ())
            }))
            for group_id, node_ids in program.execution_selections.items()
        })
        self._selection_motion_target_roles = MappingProxyType({
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
        self._scope_identity: tuple[str, str, int] | None = None
        # AUD-6: the global rule whose `hold` holds this run, from the latest
        # Kernel scope read (None once Resume releases it); the run end names it.
        self._stopped_by_rule_id: str | None = None
        # The run's one Program clock. Its owner freezes it while the run is
        # halted; waits, the timer and held-time measurement all read it, so
        # a hold that spans a Halt is never subtracted twice.
        self._program_clock = program_clock or ProgramClock()
        # The owner's motion-attention check, and this run's automatic-retry audit facts.
        if motion_attention is not None and not callable(motion_attention):
            raise TypeError("motion_attention must be callable")
        self._motion_attention = motion_attention
        # The Runner's execution transport timeout, for owner task nodes.
        self._transport_timeout_s = transport_timeout_s
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
        # Decision 138: each node's nearest Hold scope (None: Continue up to
        # the root), for the held-node mask of the Run's Mock snapshot.
        self._hold_scopes: dict[str, str | None] = {}
        root = self._build_node(
            program.tree,
            inherited_stop_policy="continue",
            root=True,
        )
        self._tree = py_trees.trees.BehaviourTree(root=root)
        self._tree.setup()
        self._lock = RLock()
        self._stopped = False
        # Decision 135: unconfirmed Mock ENDs/pause states already reported.
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
                attempt_source=self._attempt_source,
                ports=self._ports,
                operator_stop_state=self._operator_stop_state,
                operator_stop_hold_scope=hold_scope,
                program_clock=self._program_clock,
                motion_attention=self._motion_attention,
                audit_sink=self._record_node_history_event,
                transport_timeout_s=self._transport_timeout_s,
            ),
        )
        if isinstance(built, OwnerTaskAdapter):
            self._task_adapters[node.instance_id] = built
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
        capability, contact-grasp binding or blackboard key: those keep the
        captured meaning. Raises ``NodeInputProposalError`` for a foreign or
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

        with self._lock:
            self._node_history_events.append(dict(event))

    def take_node_history_events(self) -> tuple[dict[str, Any], ...]:
        """Node history events decided since the last take."""

        with self._lock:
            events, self._node_history_events = self._node_history_events, []
        return tuple(events)

    def note_operator_release(self, node_ids: frozenset[str]) -> None:
        """An accepted operator release (Resume, Run, Retry, Continue) freed
        these nodes: each motion node's automatic retry budget restarts."""

        with self._lock:
            for node_id in node_ids:
                adapter = self._task_adapters.get(node_id)
                if isinstance(adapter, MotionTaskAdapter):
                    adapter.restart_retry_series()

    def simulation_stop_nodes(self) -> frozenset[str]:
        """SIM-1: nodes refused with `simulation_not_playing` whose operator
        Stop the run owner hasn't sent yet."""

        with self._lock:
            return frozenset(
                node_id
                for node_id, adapter in self._task_adapters.items()
                if adapter.simulation_stop_pending
            )

    def note_simulation_stops(self, node_ids: frozenset[str]) -> None:
        """The operator Stop for these nodes' simulation holds was accepted."""

        with self._lock:
            for node_id in node_ids:
                adapter = self._task_adapters.get(node_id)
                if adapter is not None:
                    adapter.simulation_stop_sent()

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

        with self._lock:
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

        with self._lock:
            return MappingProxyType(
                {
                    instance_id: _presented_node_status(behaviour)
                    for instance_id, behaviour in self._authored_nodes.items()
                }
            )

    def tick_once(self) -> py_trees.common.Status:
        """Advance at most one tree tick; scheduling remains the host's job."""

        with self._lock:
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
            status = self._tree.root.status
            self._settle_mock_ends(
                final=status in {py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}
            )
            return status

    def _settle_mock_ends(self, *, final: bool) -> None:
        """Decision 135: resend this Run's unanswered node ENDs unchanged on
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
        # A Halt state never answered: the Mock may not have paused/resumed.
        for fact in (*superseded, *((current,) if final and current else ())):
            key = ("pause", fact["run_revision"])
            if key in self._mock_ends_reported:
                continue
            self._mock_ends_reported.add(key)
            self._record_node_history_event(
                {"kind": "program_mock_simulation_pause_unconfirmed", **fact}
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
        with self._lock:
            blackboard = self._value_store.diagnostic_snapshot()
            return {
                "occurred_at_ns": str(monotonic_ns()),
                "occurred_at_wall": datetime.now(timezone.utc).isoformat(),
                "blackboard": blackboard["values"],
                "omitted_blackboard_count": blackboard["omitted_count"],
            }

    def reconcile_full_stop_tasks(
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
        a Kernel hold alone cannot prove their work ended. Full Stop retains
        its existing physical task scope and distinct cancellation contract.
        """
        with self._lock:
            result = {}
            for behaviour in self._tree.root.iterate():
                if node_ids is not None and behaviour.name not in node_ids:
                    continue
                read = getattr(
                    behaviour,
                    (
                        "reconcile_full_stop_task"
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
        runner_epoch = next(
            (
                owner.owner_epoch
                for owner in self._capture.owner_identities
                if owner.owner_role == "program_runner"
            ),
            None,
        )
        if (
            not isinstance(scope, Mapping)
            or scope.get("schema_version") != "manyforge.program_execution_scope.v2"
            or scope.get("program_run_id") != self._capture.program_run_id
            or runner_epoch is None
            or scope.get("program_runner_owner_epoch") != runner_epoch
            or not isinstance(scope.get("request_owner_epoch"), str)
            or not scope["request_owner_epoch"]
            or type(scope.get("claim_generation")) is not int
            or not 0 < scope["claim_generation"] <= 2**53 - 1
            or type(scope.get("selection_revision")) is not int
            or not 0 <= scope["selection_revision"] <= 2**53 - 1
        ):
            raise ProgramRuntimeError("operator Stop projection changed its capture")
        rule_id = scope.get("rule_id")
        self._stopped_by_rule_id = (
            rule_id
            if scope.get("stopped_by") == "rule" and isinstance(rule_id, str) and rule_id
            else None
        )
        release_block_reason = scope.get("selection_release_block_reason")
        if release_block_reason is not None and (
            not isinstance(release_block_reason, str)
            or release_block_reason not in {
                "cell_motion_admission_closed",
                "cell_motion_admission_unreconciled",
                "program_run_admission_pending",
                "program_halt_active",
            }
        ):
            raise ProgramRuntimeError(
                "operator Stop projection has an invalid selection release blocker"
            )
        identity = (
            scope["request_owner_epoch"],
            runner_epoch,
            scope["claim_generation"],
        )
        if self._scope_identity is not None and identity != self._scope_identity:
            raise ProgramRuntimeError(
                "operator Stop projection changed its owner/claim"
            )
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
        self._scope_identity = identity
        return OperatorMotionStopSnapshot(
            scope["selection_revision"],
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
        """Cached scheduling query; never reads an owner or grants admission."""
        scope = self._operator_stop_state.snapshot()
        if (
            scope.stopping_nodes
            or self._operator_stop_state.observation_error() is not None
        ):
            return True
        if not scope.held_nodes:
            return False
        with self._lock:
            return any(
                node.name in scope.held_nodes
                and getattr(node, "has_active_task", False)
                for node in self._tree.root.iterate()
            )

    @property
    def stopped_by_rule_id(self) -> str | None:
        """The rule whose `hold` holds this run, from the latest scope read."""
        return self._stopped_by_rule_id

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
            and snapshot.selection_revision != expected_revision
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

    def execution_selection_motion_target_roles(
        self, group_id: str
    ) -> tuple[str, ...]:
        """Return captured motion target roles in one execution selection.

        The selection is resolved from the immutable Program captured for this
        run.  It is presentation metadata only: command admission still uses
        the original group id and the Kernel's own target resolution.
        """

        try:
            return self._selection_motion_target_roles[group_id]
        except KeyError as error:
            raise ProgramRuntimeError(
                "execution selection is not in this capture"
            ) from error

    def execution_selection_effect_target_roles(
        self, group_id: str
    ) -> tuple[str, ...]:
        """Return the target roles of every effectful node in one selection.

        With the sealed Program Binding these name the exact resources the
        Kernel reserves for the selection's Run/Resume; presentation uses
        them to anticipate operator-alarm motion attention (spec 450 §5.2).
        """

        try:
            return self._selection_effect_target_roles[group_id]
        except KeyError as error:
            raise ProgramRuntimeError(
                "execution selection is not in this capture"
            ) from error

    def execution_selection_request(
        self,
        group_id: str | list[str],
        request_id: str,
        *,
        node_instance_id: str | None = None,
    ) -> dict[str, Any]:
        """The selection DTO. ``node_instance_id`` (release only, contract 43
        / SX-2020) names one captured held node with ``group_ids: []``."""
        if self._scope_identity is None:
            raise ProgramRuntimeError("Program execution scope is not captured")
        if node_instance_id is not None and (
            group_id != [] or node_instance_id not in self._authored_nodes
        ):
            raise ProgramRuntimeError(
                "a node release names one captured node and no group"
            )
        kernel_epoch, runner_epoch, claim = self._scope_identity
        # Do not resolve Stop membership here: the Kernel owns conservative
        # containment of an unknown selection as well as valid selections.
        request = {
            "schema_version": "manyforge.program_execution_selection.v2",
            "program_runner_owner_epoch": runner_epoch,
            "claim_generation": claim,
            "program_run_id": self._capture.program_run_id,
            "group_ids": [group_id] if isinstance(group_id, str) else list(group_id),
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": kernel_epoch,
                "source_label": "program_selection_control",
                "correlation_id": None,
            },
        }
        if node_instance_id is not None:
            request["node_instance_id"] = node_instance_id
        return request

    def node_motion_attention_request(
        self, node_instance_id: str, request_id: str
    ) -> dict[str, Any]:
        """Kernel no-effect attention check for one captured node."""
        if self._scope_identity is None:
            raise ProgramRuntimeError("Program execution scope is not captured")
        if node_instance_id not in self._authored_nodes:
            raise ProgramRuntimeError("node is not in this capture")
        kernel_epoch, runner_epoch, claim = self._scope_identity
        return {
            "schema_version": "manyforge.program_node_motion_attention_check.v1",
            "program_runner_owner_epoch": runner_epoch,
            "claim_generation": claim,
            "program_run_id": self._capture.program_run_id,
            "node_instance_id": node_instance_id,
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": kernel_epoch,
                "source_label": "program_recovery_control",
                "correlation_id": None,
            },
        }

    def terminal_failure(self) -> FailureRecord:
        """Return the exact failure at a failed tree tip.

        The tip's own reason, message, details and origin, attributed to its
        authored node instance. A tip that is not one exact authored node, or
        names no bounded reason, is the runner's generic ``program_tree_failed``.
        """

        with self._lock:
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
        """Invalidate the tree once, causing active leaves to request cancel."""

        with self._lock:
            if self._stopped:
                return
            self._stopped = True
            self._tree.root.stop(py_trees.common.Status.INVALID)
            self._settle_mock_ends(final=True)


__all__ = [
    "ExecutableProgramRun",
    "NodeInputProposalError",
    "PROGRAM_RUN_CAPTURE_API",
    "ProgramRunAttemptSource",
    "ProgramRunCapture",
    "ProgramRunOwnerIdentity",
    "ProgramRuntimeError",
]
