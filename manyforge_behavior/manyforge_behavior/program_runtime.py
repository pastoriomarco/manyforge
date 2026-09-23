"""One bounded py_trees Program run over exact retained identities."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
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

from manyforge_behavior.executable_node_resolver import (
    NodeAdapterBuildContext,
    build_executable_node_resolver,
)
from manyforge_behavior.node_availability import (
    NodeExecutionReadiness,
    project_node_execution_availability,
)
from manyforge_behavior.node_environment import NodeEnvironment
from manyforge_behavior.orchestration_ports import NodeAdapterPorts, NodeAttemptIdentity
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramRuntimeOverrideStore,
    ProgramValueStore,
)
from manyforge_behavior.program import (
    Program,
    ProgramNodePolicy,
    ProgramTreeNode,
)
from manyforge_behavior.recovery import (
    RecoveryAwareNode,
    recovery_policy_is_actionable,
)
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)

PROGRAM_RUN_CAPTURE_API: Final = "manyforge.program_run_capture.v1"

_MOTION_TARGET_NODE_IDS: Final = frozenset(
    {
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "motion.follow_cartesian_path",
    }
)


class ProgramRuntimeError(RuntimeError):
    """A Program run cannot be built or advanced truthfully."""


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


def _program_node_ids(node: ProgramTreeNode) -> tuple[str, ...]:
    result = {node.node_id}
    for child in node.children:
        result.update(_program_node_ids(child))
    return tuple(sorted(result))


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
        program_parameter_values: Mapping[str, Any] | None = None,
    ) -> None:
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
        pending_nodes = [program.tree]
        while pending_nodes:
            node = pending_nodes.pop()
            if node.node_id in _MOTION_TARGET_NODE_IDS:
                target_role = node.parameters.get("target_role")
                if isinstance(target_role, str) and target_role:
                    motion_target_by_node[node.instance_id] = target_role
            pending_nodes.extend(node.children)
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

        self._value_store = ProgramValueStore.from_program(program)
        self._runtime_overrides = ProgramRuntimeOverrideStore(
            program=program,
            catalog=environment.catalog,
        )
        self._input_resolver = NodeAttemptInputResolver(
            program=program,
            catalog=environment.catalog,
            value_store=self._value_store,
            program_parameter_values=program_parameter_values,
            runtime_overrides=self._runtime_overrides,
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
        self._operator_stop_state = OperatorMotionStopState(
            node_children, read_scope=self._read_operator_stop_scope
        )
        self._authored_nodes: dict[str, py_trees.behaviour.Behaviour] = {}
        root = self._build_node(
            program.tree,
            inherited_stop_policy="continue",
            root=True,
        )
        self._tree = py_trees.trees.BehaviourTree(root=root)
        self._tree.setup()
        self._lock = RLock()
        self._stopped = False

    def _build_node(
        self,
        node: ProgramTreeNode,
        *,
        inherited_stop_policy: str,
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
        children = tuple(
            self._build_node(
                child,
                inherited_stop_policy=effective_stop_policy,
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
            ),
        )
        policy = self._capture.node_policy_by_instance_id.get(node.instance_id)
        recovery_port = self._ports.program_interventions
        if (
            policy is not None
            and recovery_policy_is_actionable(policy)
            and recovery_port is not None
        ):
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
    def availability(self) -> dict[str, Any]:
        return deepcopy(self._availability)

    @property
    def value_store(self) -> ProgramValueStore:
        return self._value_store

    @property
    def runtime_overrides(self) -> ProgramRuntimeOverrideStore:
        return self._runtime_overrides

    @property
    def status(self) -> py_trees.common.Status:
        return self._tree.root.status

    @property
    def node_statuses(self) -> Mapping[str, str]:
        """Return exact authored-node phases from the current tree tick."""

        with self._lock:
            return MappingProxyType(
                {
                    instance_id: (
                        "idle"
                        if getattr(behaviour, "presented_status", behaviour.status)
                        is py_trees.common.Status.INVALID
                        else getattr(
                            behaviour, "presented_status", behaviour.status
                        ).name.lower()
                    )
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
            return self._tree.root.status

    def diagnostic_snapshot(self) -> dict[str, Any]:
        """Capture local values at the owner transition; no remote reads or I/O."""
        with self._lock:
            blackboard = self._value_store.diagnostic_snapshot()
            overrides = self._runtime_overrides.diagnostic_snapshot()
            return {
                "occurred_at_ns": str(monotonic_ns()),
                "occurred_at_wall": datetime.now(timezone.utc).isoformat(),
                "blackboard": blackboard["values"],
                "runtime_overrides": overrides["values"],
                "omitted_blackboard_count": blackboard["omitted_count"],
                "omitted_override_count": overrides["omitted_count"],
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

    def execution_selection_request(
        self, group_id: str | list[str], request_id: str
    ) -> dict[str, Any]:
        if self._scope_identity is None:
            raise ProgramRuntimeError("Program execution scope is not captured")
        kernel_epoch, runner_epoch, claim = self._scope_identity
        # Do not resolve Stop membership here: the Kernel owns conservative
        # containment of an unknown selection as well as valid selections.
        return {
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

    def terminal_failure(self) -> tuple[str, str]:
        """Return the exact focused-owner failure at a failed tree tip."""

        with self._lock:
            if self._tree.root.status is not py_trees.common.Status.FAILURE:
                raise ProgramRuntimeError(
                    "terminal failure is available only for a failed Program tree"
                )
            tip = self._tree.tip()
            reason_code = getattr(tip, "failure_reason_code", None)
            message = getattr(tip, "failure_message", "")
            if (
                not isinstance(reason_code, str)
                or not reason_code
                or len(reason_code.encode("utf-8")) > 256
                or not isinstance(message, str)
                or len(message.encode("utf-8")) > 4_096
            ):
                return "program_tree_failed", "Program tree failed"
            return reason_code, message

    def stop(self) -> None:
        """Invalidate the tree once, causing active leaves to request cancel."""

        with self._lock:
            if self._stopped:
                return
            self._stopped = True
            self._tree.root.stop(py_trees.common.Status.INVALID)


__all__ = [
    "ExecutableProgramRun",
    "PROGRAM_RUN_CAPTURE_API",
    "ProgramRunAttemptSource",
    "ProgramRunCapture",
    "ProgramRunOwnerIdentity",
    "ProgramRuntimeError",
]
