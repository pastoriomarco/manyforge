"""Closed v2 resolution, attempt snapshots, and thin owner adapters."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import json
import re
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from typing import Any

import py_trees
import pytest

import manyforge_behavior.executable_node_resolver as resolver_module
from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.executable_node_resolver import (
    ExecutableNodeResolverError,
    NodeAdapterBuildContext,
    build_executable_node_resolver,
)
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    NodeAvailabilityError,
    NodeExecutionReadiness,
    project_node_execution_availability,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    NodeAdapterPorts,
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerInvocationHeldError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramRuntimeOverrideStore,
    ProgramValueError,
    ProgramValueStore,
)
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState, OperatorMotionStopSnapshot
from manyforge_behavior.program import Program, ProgramTreeNode
from manyforge_behavior.program_runtime import (
    ExecutableProgramRun,
    ProgramRunAttemptSource,
    ProgramRunCapture,
    ProgramRunOwnerIdentity,
    ProgramRuntimeError,
)

_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_MAINTAINED_PROGRAM_INDEX = (
    _ROOT / "manyforge_deployment/resources/maintained/"
    "maintained_program_role_assignments.v1.json"
)


def _catalog() -> NodeCatalog:
    return NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def _identified(document: dict[str, Any]) -> dict[str, Any]:
    result = deepcopy(document)
    result["program_revision"] = compute_content_identity(
        result,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return result


def _minimal_document() -> dict[str, Any]:
    return Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        _catalog(),
    ).to_dict()


def _walk(node: ProgramTreeNode) -> tuple[ProgramTreeNode, ...]:
    return (node, *(nested for child in node.children for nested in _walk(child)))


def _readiness(binding_id: str) -> NodeExecutionReadiness:
    resolver = build_executable_node_resolver(build_node_environment())
    return NodeExecutionReadiness(
        program_binding_id=binding_id,
        binding_by_node={
            disposition.row.node_id: AvailabilityFact(True) for disposition in resolver.dispositions
        },
        owner_by_port={
            owner_port: AvailabilityFact(True)
            for owner_port in {
                disposition.owner_port_name
                for disposition in resolver.dispositions
                if disposition.owner_port_name is not None
            }
        },
    )


class _TaskPort:
    def __init__(self, states: tuple[OwnerTaskState, ...] = ()) -> None:
        self._states = list(states)
        self.starts: list[tuple[str, NodeAttemptIdentity, OwnerNodeInputs]] = []
        self.observations: list[str] = []
        self.cancellations: list[tuple[str, NodeAttemptIdentity]] = []
        self.acknowledgements: list[tuple[str, NodeAttemptIdentity]] = []
        self.output: Any = None
        self.metadata: dict[str, Any] = {}

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts.append((node_id, identity, inputs))
        state = self._states.pop(0) if self._states else OwnerTaskState.SUCCEEDED
        return OwnerTaskSnapshot(
            task_id=f"task-{len(self.starts)}",
            state=state,
            output=self.output,
            metadata=self.metadata,
            reason_code="owner_failed" if state is OwnerTaskState.FAILED else None,
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations.append(task_id)
        state = self._states.pop(0) if self._states else OwnerTaskState.SUCCEEDED
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=state,
            output=self.output,
            metadata=self.metadata,
            reason_code="owner_failed" if state is OwnerTaskState.FAILED else None,
        )

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self.cancellations.append((task_id, identity))

    def acknowledge(
        self,
        task_id: str,
        identity: NodeAttemptIdentity,
    ) -> None:
        self.acknowledgements.append((task_id, identity))


class _InvocationPort:
    def __init__(self) -> None:
        self.invocations: list[tuple[str, NodeAttemptIdentity, OwnerNodeInputs]] = []
        self.result = OwnerInvocationResult(True)

    def invoke(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerInvocationResult:
        self.invocations.append((node_id, identity, inputs))
        return self.result


def _portable_pose() -> dict[str, Any]:
    return {
        "frame_ref": {
            "kind": "target_frame",
            "target_role": "arm",
            "frame_role": "base",
        },
        "position_m": [0.4, 0.0, 0.2],
        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }


def _plain(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain(nested) for key, nested in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        return [_plain(nested) for nested in value]
    return value


class _MaintainedProgramTaskPort(_TaskPort):
    @staticmethod
    def _output(node_id: str, inputs: OwnerNodeInputs) -> Any:
        if node_id == "assistant.build_scene_selection":
            return {
                "candidate_object_ids": sorted(inputs.parameters["candidate_object_ids"]),
                "scene_context_json": '{"objects":[]}',
                "selection_request_text": inputs.parameters["selection_request_text"],
            }
        if node_id == "assistant.validate_scene_selection":
            return _plain(inputs.blackboard["selection_response_key"])
        if node_id == "assistant.query_model":
            output_key = inputs.parameters["output_key"]
            if output_key == "selection.response":
                return {
                    "confidence": 0.95,
                    "matched_attributes": ["shape:sphere"],
                    "outcome": "selected",
                    "reason": "one retained candidate",
                    "selected_object_id": "sphere_part",
                }
            if output_key == "perception.pick_target":
                return {
                    "confidence": 0.95,
                    "pick_left_bin": True,
                    "reason": "left bin selected",
                    "selected_object_id": "left_part",
                }
            if output_key == "perception.grasp_plan":
                return {
                    "approach_pose": _portable_pose(),
                    "confidence": 0.95,
                    "grasp_pose": _portable_pose(),
                    "reason": "fixture grasp",
                    "selected_object_id": "graspable",
                }
        if node_id == "planning.plan_pick_sequence":
            return {
                "approach_pose": _portable_pose(),
                "pick_pose": _portable_pose(),
                "retreat_pose": _portable_pose(),
                "selected_object_id": inputs.parameters["selected_object_id"],
            }
        return None

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts.append((node_id, identity, inputs))
        return OwnerTaskSnapshot(
            task_id=f"task-{len(self.starts)}",
            state=OwnerTaskState.SUCCEEDED,
            output=self._output(node_id, inputs),
        )


def _ports(
    task: _TaskPort | None = None,
    invocation: _InvocationPort | None = None,
) -> NodeAdapterPorts:
    task_port = task or _TaskPort()
    invocation_port = invocation or _InvocationPort()
    return NodeAdapterPorts(
        kernel_operations=task_port,
        scene_transactions=invocation_port,
        gripper_status=invocation_port,
        model_queries=invocation_port,
        assistant_requests=task_port,
        pick_place=task_port,
        simulator_control=task_port,
        kernel_controls=task_port,
    )


def _capture(program: Program, *, binding_id: str = _DIGEST_A) -> ProgramRunCapture:
    environment = build_node_environment()
    return ProgramRunCapture(
        program_run_id="run.test",
        applied_record_digest="sha256:" + "9" * 64,
        scenario_id="scenario.test",
        scenario_catalog_revision=_DIGEST_A,
        program_id=program.program_id,
        program_revision=program.program_revision,
        program_binding_id=binding_id,
        retained_bundle_id=_DIGEST_B,
        graph_revision=_DIGEST_C,
        node_environment_digest=environment.environment_digest,
        owner_identities=(
            ProgramRunOwnerIdentity(
                owner_role="program_runner",
                owner_id="program-runner.cell-a",
                owner_epoch="program-runner.epoch.1",
            ),
        ),
    )


def test_closed_resolver_exactly_disposes_all_effective_rows() -> None:
    environment = build_node_environment()
    resolver = build_executable_node_resolver(environment)
    dispositions = resolver.dispositions

    assert len(dispositions) == len(environment.registry.rows) == 52
    assert sum(item.adapter_build_qualified for item in dispositions) == 47
    assert sum(not item.adapter_build_qualified for item in dispositions) == 5
    assert all(
        (
            (item.unavailable_reason_code is None and not item.restoration_gates)
            if item.adapter_build_qualified
            else (item.unavailable_reason_code and item.restoration_gates)
        )
        for item in dispositions
    )
    assert "kernel.full_stop" not in {
        item.row.node_id: item.restoration_gates
        for item in dispositions
        if not item.adapter_build_qualified
    }


def test_headless_static_projection_matches_the_closed_deferred_set() -> None:
    resolver = build_executable_node_resolver(build_node_environment())
    expected = {
        item.row.node_id: (
            item.unavailable_reason_code,
            *item.restoration_gates,
        )
        for item in resolver.dispositions
        if not item.adapter_build_qualified
    }
    source = (_ROOT / "manyforge_headless/src/node_environment_projection.cpp").read_text(
        encoding="utf-8"
    )
    block = source.split("kDeferredAdapters{{", maxsplit=1)[1].split("}};", maxsplit=1)[0]
    fields = re.findall(r'"([^"]+)"', block)
    assert len(fields) == 4 * len(expected)
    observed = {
        fields[index]: tuple(fields[index + 1 : index + 4]) for index in range(0, len(fields), 4)
    }

    assert observed == expected


def test_closed_resolver_rejects_unknown_and_missing_algorithm_bindings(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    environment = build_node_environment()
    resolver = build_executable_node_resolver(environment)
    unknown = replace(
        environment.registry.rows[0],
        implementation_id="stale_implementation",
    )
    with pytest.raises(ExecutableNodeResolverError, match="not part"):
        resolver.disposition(unknown)

    algorithms = dict(resolver_module._PURE_BUILDERS)  # noqa: SLF001
    del algorithms["sequence_v1"]
    monkeypatch.setattr(resolver_module, "_PURE_BUILDERS", algorithms)
    with pytest.raises(
        ExecutableNodeResolverError,
        match="has no build-qualified algorithm",
    ):
        build_executable_node_resolver(environment)


def test_simulation_row_uses_the_focused_cancellable_task_owner() -> None:
    document = _minimal_document()
    document["tree"] = {
        "node_id": "simulation.reset",
        "instance_id": "reset_sim",
        "parameters": {},
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    task = _TaskPort((OwnerTaskState.ACTIVE, OwnerTaskState.SUCCEEDED))
    resolver = build_executable_node_resolver(build_node_environment())
    adapter = resolver.instantiate(
        build_node_environment().registry.record("simulation.reset"),
        NodeAdapterBuildContext(
            name="reset_sim",
            node_instance_id="reset_sim",
            parameters={},
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.simulation"),
            ports=NodeAdapterPorts(simulator_control=task),
        ),
    )

    adapter.initialise()
    assert adapter.update() is py_trees.common.Status.RUNNING
    assert task.starts[0][0] == "simulation.reset"
    assert task.starts[0][2].parameters == {}
    assert adapter.update() is py_trees.common.Status.SUCCESS
    assert task.acknowledgements == [("task-1", task.starts[0][1])]


@pytest.mark.parametrize("outcome", [OwnerTaskState.SUCCEEDED, OwnerTaskState.FAILED])
def test_recovery_row_uses_the_exact_control_port_and_does_not_retry(outcome) -> None:
    document = _minimal_document()
    parameters = {"target_role": "arm", "recovery_action": "acknowledge_and_continue"}
    document["tree"] = {
        "node_id": "recovery.apply_offered_action",
        "instance_id": "recover_arm",
        "parameters": parameters,
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    task = _TaskPort((outcome,))
    environment = build_node_environment()
    adapter = build_executable_node_resolver(environment).instantiate(
        environment.registry.record("recovery.apply_offered_action"),
        NodeAdapterBuildContext(
            name="recover_arm",
            node_instance_id="recover_arm",
            parameters=program.tree.parameters,
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.recovery"),
            ports=NodeAdapterPorts(kernel_controls=task),
        ),
    )
    adapter.initialise()
    expected = (
        py_trees.common.Status.SUCCESS
        if outcome is OwnerTaskState.SUCCEEDED
        else py_trees.common.Status.FAILURE
    )
    assert adapter.update() is expected
    assert adapter.update() is expected
    assert len(task.starts) == 1
    assert task.starts[0][0] == "recovery.apply_offered_action"
    assert dict(task.starts[0][2].parameters) == parameters
    assert not task.observations


def test_accepted_task_retries_only_typed_observation_unavailability() -> None:
    class _OneOutageTaskPort(_TaskPort):
        def __init__(self) -> None:
            super().__init__((OwnerTaskState.ACTIVE, OwnerTaskState.SUCCEEDED))
            self.outage_pending = True

        def observe(self, task_id: str) -> OwnerTaskSnapshot:
            self.observations.append(task_id)
            if self.outage_pending:
                self.outage_pending = False
                raise OwnerTaskObservationUnavailableError("temporarily unavailable")
            state = self._states.pop(0)
            return OwnerTaskSnapshot(task_id=task_id, state=state)

    document = _minimal_document()
    document["tree"] = {
        "node_id": "simulation.reset",
        "instance_id": "reset_sim",
        "parameters": {},
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    task = _OneOutageTaskPort()
    adapter = build_executable_node_resolver(build_node_environment()).instantiate(
        build_node_environment().registry.record("simulation.reset"),
        NodeAdapterBuildContext(
            name="reset_sim",
            node_instance_id="reset_sim",
            parameters={},
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.simulation"),
            ports=NodeAdapterPorts(simulator_control=task),
        ),
    )

    adapter.initialise()
    assert adapter.update() is py_trees.common.Status.RUNNING
    assert adapter.update() is py_trees.common.Status.RUNNING
    assert adapter.update() is py_trees.common.Status.SUCCESS
    assert len(task.starts) == 1
    assert task.observations == ["task-1", "task-1"]
    assert task.acknowledgements == [("task-1", task.starts[0][1])]


@pytest.mark.parametrize("proof", [1, "true", None])
def test_task_settlement_evidence_requires_an_explicit_boolean(proof):
    with pytest.raises(TypeError, match="settlement proof must be boolean"):
        OwnerTaskSnapshot("task", OwnerTaskState.SUCCEEDED, settlement_proven=proof)


def test_active_task_cannot_claim_terminal_settlement():
    with pytest.raises(ValueError, match="terminal owner result"):
        OwnerTaskSnapshot("task", OwnerTaskState.ACTIVE, settlement_proven=True)


@pytest.mark.parametrize("unstarted,settlement_proven", [(True, False), (False, False), (False, True)])
def test_held_task_observation_distinguishes_unstarted_from_settled_failure(unstarted, settlement_proven):
    class Port(_TaskPort):
        def observe(self, task_id):
            if unstarted:
                raise OwnerInvocationHeldError(4)
            return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.FAILED,
                                     settlement_proven=settlement_proven, reason_code="refused")

    document = _minimal_document()
    document["tree"] = {"node_id": "simulation.reset", "instance_id": "action",
                        "parameters": {}, "children": []}
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    task = Port((OwnerTaskState.ACTIVE,))
    state = OperatorMotionStopState({"action": ()})
    environment = build_node_environment()
    adapter = build_executable_node_resolver(environment).instantiate(
        environment.registry.record("simulation.reset"), NodeAdapterBuildContext(
            name="action", node_instance_id="action", parameters={},
            input_resolver=NodeAttemptInputResolver(program=program, catalog=catalog,
                value_store=ProgramValueStore.from_program(program)),
            attempt_source=ProgramRunAttemptSource("run.held"),
            ports=NodeAdapterPorts(simulator_control=task), operator_stop_state=state))
    list(adapter.tick())
    assert len(task.starts) == 1
    state.publish(OperatorMotionStopSnapshot(4, frozenset({"action"})))
    adapter.observe_while_operator_stop_held()
    if unstarted:
        assert adapter.presented_status is py_trees.common.Status.INVALID
        assert adapter.observe_control_state() == "inactive"
        list(adapter.tick())
        assert len(task.starts) == 1
        state.publish(OperatorMotionStopSnapshot(5))
        list(adapter.tick())
        assert len(task.starts) == 2  # Fresh attempt only after authoritative release.
        assert task.starts[0][1] != task.starts[1][1]
    else:
        assert adapter.status is py_trees.common.Status.FAILURE
        assert adapter.observe_control_state() == ("settled" if settlement_proven else "unrecoverable")
        assert len(task.acknowledgements) == int(settlement_proven)
        assert not task.cancellations


@pytest.mark.parametrize("selected_resume", [False, True])
@pytest.mark.parametrize("reject_settlement", [False, True])
def test_terminal_task_remains_running_while_owner_settlement_is_pending(
    selected_resume: bool,
    reject_settlement: bool,
) -> None:
    class _SettlingTaskPort(_TaskPort):
        def __init__(self) -> None:
            super().__init__((OwnerTaskState.SUCCEEDED,))
            self.acknowledgement_attempts = 0

        def acknowledge(
            self,
            task_id: str,
            identity: NodeAttemptIdentity,
        ) -> None:
            self.acknowledgement_attempts += 1
            if self.acknowledgement_attempts == 1:
                if reject_settlement:
                    raise ValueError("settlement refused")
                raise OwnerTaskSettlementPendingError("still settling")
            super().acknowledge(task_id, identity)

    document = _minimal_document()
    document["tree"] = {
        "node_id": "simulation.reset",
        "instance_id": "reset_sim",
        "parameters": {},
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    task = _SettlingTaskPort()
    adapter = build_executable_node_resolver(build_node_environment()).instantiate(
        build_node_environment().registry.record("simulation.reset"),
        NodeAdapterBuildContext(
            name="reset_sim",
            node_instance_id="reset_sim",
            parameters={},
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.simulation"),
            ports=NodeAdapterPorts(simulator_control=task),
        ),
    )

    adapter.initialise()
    if reject_settlement:
        assert adapter.update() is py_trees.common.Status.FAILURE
        assert adapter.observe_control_state() == "unrecoverable"
        assert not adapter.has_active_task
        assert adapter.failure_reason_code == "owner_acknowledgement_failed"
        assert task.acknowledgement_attempts == 1
        return
    assert adapter.update() is py_trees.common.Status.RUNNING
    assert adapter.observe_control_state() == "active"
    assert adapter.has_active_task
    if selected_resume:
        assert adapter.reconcile_full_stop_task(selected_resume=True) == "settled"
        assert adapter.status is py_trees.common.Status.SUCCESS
    else:
        assert adapter.update() is py_trees.common.Status.SUCCESS
    assert adapter.observe_control_state() == "settled"
    assert not adapter.has_active_task
    assert task.acknowledgement_attempts == 2
    assert task.acknowledgements == [("task-1", task.starts[0][1])]


def test_non_transient_and_identity_changing_observations_still_fail() -> None:
    class _ObservationFailureTaskPort(_TaskPort):
        def __init__(self, failure: Exception | None) -> None:
            super().__init__((OwnerTaskState.ACTIVE,))
            self.failure = failure

        def observe(self, task_id: str) -> OwnerTaskSnapshot:
            if self.failure is not None:
                raise self.failure
            return OwnerTaskSnapshot(
                task_id="another-task",
                state=OwnerTaskState.ACTIVE,
            )

    document = _minimal_document()
    document["tree"] = {
        "node_id": "simulation.reset",
        "instance_id": "reset_sim",
        "parameters": {},
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    for task, expected_reason in (
        (
            _ObservationFailureTaskPort(RuntimeError("malformed")),
            "owner_request_failed",
        ),
        (_ObservationFailureTaskPort(None), "owner_task_identity_changed"),
    ):
        adapter = build_executable_node_resolver(build_node_environment()).instantiate(
            build_node_environment().registry.record("simulation.reset"),
            NodeAdapterBuildContext(
                name="reset_sim",
                node_instance_id="reset_sim",
                parameters={},
                input_resolver=NodeAttemptInputResolver(
                    program=program,
                    catalog=catalog,
                    value_store=ProgramValueStore.from_program(program),
                ),
                attempt_source=ProgramRunAttemptSource("run.simulation"),
                ports=NodeAdapterPorts(simulator_control=task),
            ),
        )
        adapter.initialise()
        assert adapter.update() is py_trees.common.Status.RUNNING
        assert adapter.update() is py_trees.common.Status.FAILURE
        assert adapter.failure_reason_code == expected_reason
        assert len(task.starts) == 1
        assert task.acknowledgements == []


def test_availability_keeps_adapter_binding_owner_and_execution_separate() -> None:
    resolver = build_executable_node_resolver(build_node_environment())
    unselected = project_node_execution_availability(
        resolver,
        NodeExecutionReadiness.unselected(resolver),
        required_node_ids=("motion.move_to_joint_target", "sequence"),
    )
    by_node = {item["node_id"]: item for item in unselected["implementations"]}

    assert unselected["portable_authoring_available"] is True
    assert unselected["execution_available"] is False
    assert by_node["motion.move_to_joint_target"] == {
        **resolver.disposition(
            build_node_environment().registry.record("motion.move_to_joint_target")
        ).row.to_dict(),
        "authoring_available": True,
        "adapter_build_qualified": True,
        "binding_available": False,
        "owner_port_name": "kernel_operations",
        "owner_health_available": False,
        "execution_available": False,
        "implementation_identity_resolved": True,
        "reason_codes": [
            "program_binding_unselected",
            "program_runtime_owner_unavailable",
        ],
        "restoration_gates": [],
    }
    assert by_node["simulation.reset"]["authoring_available"] is True
    assert by_node["simulation.reset"]["adapter_build_qualified"] is True
    assert by_node["simulation.reset"]["owner_health_available"] is False
    assert by_node["simulation.reset"]["reason_codes"] == [
        "program_binding_unselected",
        "program_runtime_owner_unavailable",
    ]

    selected = project_node_execution_availability(
        resolver,
        _readiness(_DIGEST_A),
        required_node_ids=("motion.move_to_joint_target", "sequence"),
    )
    assert selected["execution_available"] is True
    assert selected["reason_codes"] == []
    assert {item["node_id"]: item for item in selected["implementations"]}["simulation.reset"][
        "execution_available"
    ] is True

    incomplete = dict(_readiness(_DIGEST_A).binding_by_node)
    incomplete.pop("sequence")
    with pytest.raises(NodeAvailabilityError, match="binding facts is not exact"):
        project_node_execution_availability(
            resolver,
            NodeExecutionReadiness(
                program_binding_id=_DIGEST_A,
                binding_by_node=incomplete,
                owner_by_port=_readiness(_DIGEST_A).owner_by_port,
            ),
            required_node_ids=("sequence",),
        )


def test_top_level_value_source_is_one_attempt_snapshot() -> None:
    document = _minimal_document()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "flag",
                "type": "bool",
                "access": "read_write",
                "persistence": "run",
            }
        ],
    }
    document["tree"] = {
        "node_id": "logic.check_bool",
        "instance_id": "check_flag",
        "parameters": {"expected_value": True},
        "value_sources": {
            "actual_value": {
                "kind": "blackboard_key",
                "reference": "flag",
            }
        },
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    store = ProgramValueStore.from_program(program)
    store.write("flag", True)
    resolver = build_executable_node_resolver(build_node_environment())
    node = resolver.instantiate(
        build_node_environment().registry.record("logic.check_bool"),
        NodeAdapterBuildContext(
            name="check_flag",
            node_instance_id="check_flag",
            parameters=program.tree.parameters,
            value_sources=program.tree.value_sources,
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=store,
            ),
        ),
    )

    node.initialise()
    store.write("flag", False)
    assert node.update() is py_trees.common.Status.SUCCESS
    node.initialise()
    assert node.update() is py_trees.common.Status.FAILURE


@pytest.mark.parametrize("expected", [False, True])
def test_logical_gripper_flag_wait_observes_updates_without_physical_owner(
    expected,
) -> None:
    """Old WaitForGripperState was a logical boolean wait, not grasp proof."""
    document = _minimal_document()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "gripper_flag",
                "type": "bool",
                "access": "read_write",
                "persistence": "run",
            }
        ],
    }
    document["tree"] = {
        "node_id": "logic.wait_for_bool",
        "instance_id": "wait_for_gripper_flag",
        "parameters": {"key": "gripper_flag", "expected_value": expected},
        "children": [],
    }
    program = Program.from_dict(_identified(document), _catalog())
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=NodeAdapterPorts(),
    )
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("gripper_flag", not expected)
    for _ in range(100):
        assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("gripper_flag", expected)
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def test_runtime_override_changes_only_later_attempt_inputs() -> None:
    catalog = _catalog()
    program = Program(
        _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
        catalog,
    )
    move = next(
        node for node in _walk(program.tree) if node.node_id == "motion.move_to_joint_target"
    )
    store = ProgramValueStore.from_program(program)
    overrides = ProgramRuntimeOverrideStore(program=program, catalog=catalog)
    resolver = NodeAttemptInputResolver(
        program=program,
        catalog=catalog,
        value_store=store,
        runtime_overrides=overrides,
    )
    before = resolver.snapshot(
        node_id=move.node_id,
        node_instance_id=move.instance_id,
        parameters=move.parameters,
        value_sources=move.value_sources,
    )
    overrides.activate(move.instance_id, "velocity_scale", 0.25)
    after = resolver.snapshot(
        node_id=move.node_id,
        node_instance_id=move.instance_id,
        parameters=move.parameters,
        value_sources=move.value_sources,
    )

    assert before.parameters["velocity_scale"] != 0.25
    assert after.parameters["velocity_scale"] == 0.25
    assert before.parameters["velocity_scale"] != after.parameters["velocity_scale"]


def test_runtime_override_cannot_replace_captured_binding_selectors() -> None:
    catalog = _catalog()
    program = Program(
        _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
        catalog,
    )
    move = next(
        node for node in _walk(program.tree) if node.node_id == "motion.move_to_joint_target"
    )
    overrides = ProgramRuntimeOverrideStore(program=program, catalog=catalog)

    with pytest.raises(ProgramValueError, match="captured binding meaning"):
        overrides.validate(move.instance_id, "target_role", "other_arm")
    with pytest.raises(ProgramValueError, match="captured binding meaning"):
        overrides.validate(move.instance_id, "planning_profile", "first_feasible")


def test_runtime_override_cannot_add_or_change_nested_semantic_selectors() -> None:
    catalog = _catalog()
    program = Program(
        _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
        catalog,
    )
    move = next(node for node in _walk(program.tree) if node.instance_id == "move_to_pick_approach")
    overrides = ProgramRuntimeOverrideStore(program=program, catalog=catalog)
    goal = deepcopy(dict(move.parameters["goal"]))
    goal["frame_ref"] = {"kind": "cell_frame"}

    with pytest.raises(ProgramValueError, match="captured semantic selection"):
        overrides.validate(move.instance_id, "goal", goal)

    changed_position = deepcopy(dict(move.parameters["goal"]))
    changed_position["position_m"] = [0.0, 0.0, 0.2]
    assert overrides.validate(move.instance_id, "goal", changed_position) == (changed_position)


def test_nested_model_sources_are_captured_atomically_by_path() -> None:
    catalog = _catalog()
    program = Program(_ROOT / "examples/semantic_shape_pick.program.yaml", catalog)
    query = next(node for node in _walk(program.tree) if node.instance_id == "choose_scene_object")
    store = ProgramValueStore.from_program(program)
    first_context = {
        "candidate_object_ids": ["sphere_part", "cube_part"],
        "scene_context_json": '{"revision":1}',
        "selection_request_text": "Select the sphere object.",
    }
    store.write("selection.context", first_context)
    snapshot = NodeAttemptInputResolver(
        program=program,
        catalog=catalog,
        value_store=store,
    ).snapshot(
        node_id=query.node_id,
        node_instance_id=query.instance_id,
        parameters=query.parameters,
        value_sources=query.value_sources,
    )
    store.write(
        "selection.context",
        {**first_context, "selection_request_text": "changed after admission"},
    )

    assert snapshot.embedded_values == {
        "/template_variables/0/source": ["sphere_part", "cube_part"],
        "/template_variables/1/source": '{"revision":1}',
        "/template_variables/2/source": "Select the sphere object.",
    }


def test_model_request_preserves_nonblocking_and_atomic_bundle_semantics() -> None:
    catalog = _catalog()
    program = Program(_ROOT / "examples/semantic_shape_pick.program.yaml", catalog)
    query = next(node for node in _walk(program.tree) if node.instance_id == "choose_scene_object")
    store = ProgramValueStore.from_program(program)
    store.write(
        "selection.context",
        {
            "candidate_object_ids": ["sphere_part", "cube_part"],
            "scene_context_json": '{"revision":1}',
            "selection_request_text": "Select the sphere object.",
        },
    )
    response = {
        "confidence": 0.92,
        "matched_attributes": ["shape:sphere"],
        "outcome": "selected",
        "reason": "single matching object",
        "selected_object_id": "sphere_part",
    }
    task = _TaskPort()
    task.output = response
    task.metadata = {
        "model_endpoint_alias": "local_pick_vlm",
        "model_version": "test-v1",
    }
    resolver = build_executable_node_resolver(build_node_environment())
    adapter = resolver.instantiate(
        build_node_environment().registry.record("assistant.query_model"),
        NodeAdapterBuildContext(
            name=query.instance_id,
            node_instance_id=query.instance_id,
            parameters=query.parameters,
            value_sources=query.value_sources,
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=catalog,
                value_store=store,
            ),
            attempt_source=ProgramRunAttemptSource("run.model"),
            ports=_ports(task),
        ),
    )
    tree = py_trees.trees.BehaviourTree(adapter)
    tree.setup()

    tree.tick()
    assert tree.root.status is py_trees.common.Status.RUNNING
    assert len(task.starts) == 1
    assert task.starts[0][2].embedded_values == {
        "/template_variables/0/source": ("sphere_part", "cube_part"),
        "/template_variables/1/source": '{"revision":1}',
        "/template_variables/2/source": "Select the sphere object.",
    }
    tree.tick()
    assert tree.root.status is py_trees.common.Status.SUCCESS
    assert store.read("selection.response") == response
    bundle = store.read_bundle("selection.response")
    assert bundle["value"] == response
    assert bundle["meta"] == {
        "status": "succeeded",
        "node_id": "assistant.query_model",
        "node_instance_id": "choose_scene_object",
        "model_endpoint_alias": "local_pick_vlm",
        "model_version": "test-v1",
        "program_run_id": "run.model",
        "node_attempt_id": task.starts[0][1].node_attempt_id,
        "owner_task_id": "task-1",
    }


def test_program_run_reuses_thin_ports_and_preserves_portable_inputs() -> None:
    program = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        _catalog(),
    )
    environment = build_node_environment()
    task = _TaskPort()
    invocation = _InvocationPort()
    run = ExecutableProgramRun(
        program=program,
        environment=environment,
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(task, invocation),
    )

    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert len(task.starts) == len(task.acknowledgements) == 1
    assert task.starts[0][0] == "motion.move_to_joint_target"
    assert task.starts[0][2].parameters["target_role"] == "arm"
    assert task.starts[0][2].parameters["velocity_scale"] == 0.5
    assert invocation.invocations[0][0] == "scene.remove_object"
    assert run.availability["execution_available"] is True
    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert len(task.starts) == 1


def test_all_maintained_program_node_forms_execute_through_the_closed_runner(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        py_trees.timers,
        "Timer",
        lambda name, duration: py_trees.behaviours.Success(name=name),
    )
    index = json.loads(_MAINTAINED_PROGRAM_INDEX.read_text(encoding="utf-8"))
    sources = tuple(sorted({assignment["program_source"] for assignment in index["assignments"]}))
    assert len(sources) == 11
    environment = build_node_environment()
    exercised: set[str] = set()

    for source in sources:
        program = Program(_ROOT / source, _catalog())
        task = _MaintainedProgramTaskPort()
        invocation = _InvocationPort()
        run = ExecutableProgramRun(
            program=program,
            environment=environment,
            capture=_capture(program),
            readiness=_readiness(_DIGEST_A),
            ports=_ports(task, invocation),
        )
        for _tick in range(128):
            status = run.tick_once()
            if status is not py_trees.common.Status.RUNNING:
                break
        assert status is py_trees.common.Status.SUCCESS, source
        exercised.update(node.node_id for node in _walk(program.tree))

    assert exercised == {
        "assistant.build_scene_selection",
        "assistant.query_model",
        "assistant.validate_scene_selection",
        "fallback",
        "gripper.command_position",
        "logic.check_bool",
        "logic.set_bool",
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "motion.pick_object",
        "motion.place_object",
        "parallel",
        "planning.plan_pick_sequence",
        "scene.add_object",
        "scene.attach_carried_object",
        "scene.detach_carried_object",
        "scene.remove_object",
        "sequence",
        "simulation.reset",
        "simulation.set_entity_pose",
        "simulation.set_state",
        "timer",
    }


def test_stopped_program_run_cannot_restart_effects() -> None:
    program = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        _catalog(),
    )
    task = _TaskPort((OwnerTaskState.ACTIVE,))
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(task),
    )

    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.stop()
    run.stop()
    assert len(task.cancellations) == 1
    with pytest.raises(ProgramRuntimeError, match="stopped Program run"):
        run.tick_once()


def test_retry_issues_a_fresh_attempt_identity_and_stop_cancels_active_work() -> None:
    document = _minimal_document()
    motion = deepcopy(document["tree"]["children"][0])
    document["tree"] = {
        "node_id": "retry",
        "instance_id": "retry_move",
        "parameters": {"num_attempts": 2},
        "children": [motion],
    }
    program = Program.from_dict(_identified(document), _catalog())
    task = _TaskPort((OwnerTaskState.FAILED, OwnerTaskState.SUCCEEDED))
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(task),
    )

    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert [item[1].node_attempt_id for item in task.starts] == list(
        dict.fromkeys(item[1].node_attempt_id for item in task.starts)
    )
    assert len(task.starts) == 2

    active_task = _TaskPort((OwnerTaskState.ACTIVE,))
    active_run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(active_task),
    )
    assert active_run.tick_once() is py_trees.common.Status.RUNNING
    active_run.stop()
    assert len(active_task.cancellations) == 1


def test_program_run_rejects_cross_artifact_capture_before_tree_construction() -> None:
    program = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        _catalog(),
    )
    with pytest.raises(ProgramRuntimeError, match="Program revision"):
        ExecutableProgramRun(
            program=program,
            environment=build_node_environment(),
            capture=replace(_capture(program), program_revision=_DIGEST_C),
            readiness=_readiness(_DIGEST_A),
            ports=_ports(),
        )


def test_closed_resolver_source_contains_no_dynamic_loader_surface() -> None:
    source = (
        _ROOT / "manyforge_behavior/manyforge_behavior/executable_node_resolver.py"
    ).read_text(encoding="utf-8")

    assert "importlib" not in source
    assert "entry_point" not in source
    assert "module_path" not in source
    assert "factory_path" not in source
