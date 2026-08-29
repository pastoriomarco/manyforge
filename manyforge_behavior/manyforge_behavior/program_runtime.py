"""One bounded py_trees Program run over exact retained identities."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from hashlib import sha256
from threading import Lock, RLock
from types import MappingProxyType
from typing import Any, Final

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
    OperatorMotionStopState,
)

PROGRAM_RUN_CAPTURE_API: Final = "manyforge.program_run_capture.v1"
_MAX_ATTEMPTS_PER_NODE: Final = 1_000_000


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
    scenario_catalog_revision: str
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
            "scenario_catalog_revision",
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
            "scenario_catalog_revision": self.scenario_catalog_revision,
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
    """Issue bounded collision-resistant attempt IDs within one exact run."""

    def __init__(self, program_run_id: str) -> None:
        self._program_run_id = _require_text(
            program_run_id,
            field="program_run_id",
        )
        self._counts: dict[str, int] = {}
        self._lock = Lock()

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        node_id = _require_text(node_instance_id, field="node_instance_id")
        with self._lock:
            count = self._counts.get(node_id, 0) + 1
            if count > _MAX_ATTEMPTS_PER_NODE:
                raise ProgramRuntimeError(
                    f"node {node_id!r} exceeds the bounded attempt count"
                )
            self._counts[node_id] = count
        preimage = (
            self._program_run_id.encode("utf-8")
            + b"\x00"
            + node_id.encode("utf-8")
            + b"\x00"
            + str(count).encode("ascii")
        )
        return NodeAttemptIdentity(
            program_run_id=self._program_run_id,
            node_instance_id=node_id,
            node_attempt_id=f"attempt:{sha256(preimage).hexdigest()}",
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
        self._operator_stop_state = OperatorMotionStopState()
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
        ancestor_holds: bool = False,
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
                ancestor_holds=(
                    ancestor_holds or effective_stop_policy == "hold"
                ),
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
        if effective_stop_policy == "hold" and not ancestor_holds:
            built = OperatorMotionStopPolicy(
                child=built,
                node_instance_id=node.instance_id,
                stop_state=self._operator_stop_state,
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
                        if behaviour.status is py_trees.common.Status.INVALID
                        else behaviour.status.name.lower()
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
