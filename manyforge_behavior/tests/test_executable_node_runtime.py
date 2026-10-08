"""Closed v2 resolution, attempt snapshots, and thin owner adapters."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
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
    FrameTransformError,
    FrameTransformResult,
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
    NodeInputError,
    ProgramValueError,
    ProgramValueStore,
)
from manyforge_behavior.nodes.interruption import InterruptedOutcome
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.program import Program, ProgramTreeNode, ProgramValidationError
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.program_runtime import (
    ExecutableProgramRun,
    ProgramRunAttemptSource,
    ProgramRunCapture,
    ProgramRunOwnerIdentity,
    ProgramRuntimeError,
    _presented_node_status,
)

_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
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
        if node_id in {
            "planning.plan_pick_sequence",
            "planning.plan_place_sequence",
        }:
            pose = _portable_pose()
            return {
                "approach_pose": pose,
                "descend_path": [pose, pose],
                "pick_pose": pose,
                "retreat_path": [pose, pose],
                "retreat_pose": pose,
                "selected_object_id": inputs.parameters["selected_object_id"],
                "tool_position": _plain(inputs.parameters["tool_position"]),
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
        simulator_control=task_port,
        kernel_controls=task_port,
    )


def _capture(program: Program, *, binding_id: str = _DIGEST_A) -> ProgramRunCapture:
    environment = build_node_environment()
    return ProgramRunCapture(
        program_run_id="run.test",
        applied_record_digest="sha256:" + "9" * 64,
        scenario_id="scenario.test",
        source_document_digest=_DIGEST_A,
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

    assert len(dispositions) == len(environment.registry.rows) == 49
    assert sum(item.adapter_build_qualified for item in dispositions) == 43
    assert sum(not item.adapter_build_qualified for item in dispositions) == 6
    assert all(
        (
            (item.unavailable_reason_code is None and not item.restoration_gates)
            if item.adapter_build_qualified
            else (item.unavailable_reason_code and item.restoration_gates)
        )
        for item in dispositions
    )
    deferred = {
        item.row.node_id: (item.unavailable_reason_code, item.restoration_gates)
        for item in dispositions
        if not item.adapter_build_qualified
    }
    assert "kernel.full_stop" not in deferred
    # Check input reads through the same semantic I/O owner as Get input.
    assert deferred["io.check_input"] == deferred["io.get_input"] == (
        "semantic_io_node_adapter_unavailable_until_i11",
        ("I11", "T"),
    )


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
        # Ticking but held until motion starts: presented as "held", not idle.
        assert adapter.status is py_trees.common.Status.RUNNING
        assert adapter.presented_held is True
        assert adapter.observe_control_state() == "inactive"
        list(adapter.tick())
        assert len(task.starts) == 1
        state.publish(OperatorMotionStopSnapshot(5))
        list(adapter.tick())
        assert len(task.starts) == 2  # Fresh attempt only after authoritative release.
        assert adapter.presented_held is False
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
        "node_id": "logic.check_condition",
        "instance_id": "check_flag",
        "parameters": {},
        "value_sources": {
            "condition": {
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
        build_node_environment().registry.record("logic.check_condition"),
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
            attempt_source=ProgramRunAttemptSource("run.test"),
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
        "node_id": "logic.wait_for_value",
        "instance_id": "wait_for_gripper_flag",
        "parameters": {"key": "gripper_flag", "value": expected},
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


def _keyed_document(*keys: dict[str, Any]) -> dict[str, Any]:
    document = _minimal_document()
    document["blackboard"] = {"namespaces": [], "keys": list(keys)}
    # A port-free tree: these tests exercise only the run's value store.
    document["tree"] = {
        "node_id": "logic.check_condition",
        "instance_id": "check",
        "parameters": {"condition": True},
        "children": [],
    }
    return document


def _key(key: str, value_type: str, **extra: Any) -> dict[str, Any]:
    return {
        "key": key,
        "type": value_type,
        "access": "read_write",
        "persistence": "run",
        **extra,
    }


class _Register:
    def __init__(self, stored: Mapping[str, Any] | None = None) -> None:
        self.stored = dict(stored or {})
        self.commits: list[tuple[str, str, Any]] = []
        self.edits: list[Mapping[str, Any]] = []
        self.drops: list[tuple[str, str, str, str | None]] = []
        self.fail = False

    def restore(self, scenario_id: str, declarations: Mapping[str, Any]) -> Mapping[str, Any]:
        assert scenario_id == "scenario.test"
        return {key: value for key, value in self.stored.items() if key in declarations}

    def drop(
        self,
        scenario_id: str,
        key: str,
        *,
        reason: str,
        detail: str | None = None,
        edit: Mapping[str, Any] | None = None,
    ) -> None:
        self.drops.append((scenario_id, key, reason, detail))
        if self.fail:
            raise OSError("disk full")
        self.stored.pop(key, None)

    def commit(
        self,
        scenario_id: str,
        declaration: Any,
        value: Any,
        *,
        edit: Mapping[str, Any] | None = None,
    ) -> None:
        if self.fail:
            raise OSError("disk full")
        self.commits.append((scenario_id, declaration.key, value))
        self.edits.append(dict(edit or {}))
        self.stored[declaration.key] = value


def _run(
    program: Program,
    register: _Register | None = None,
    *,
    program_clock: ProgramClock | None = None,
) -> ExecutableProgramRun:
    return ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=NodeAdapterPorts(persistent_keys=register),
        program_clock=program_clock,
    )


@pytest.mark.parametrize("mutability", ["immutable", "before_run", "between_operations"])
def test_removed_mutability_is_refused_by_name(mutability: str) -> None:
    document = _minimal_document()
    document["parameters"] = [
        {
            "parameter_id": "speed",
            "type": "float64",
            "default": 0.5,
            "runtime_mutability": mutability,
        }
    ]
    with pytest.raises(ProgramValidationError, match="runtime_mutability: was removed"):
        Program.from_dict(_identified(document), _catalog())


def test_removed_persistence_is_refused_by_name() -> None:
    document = _keyed_document(_key("flag", "bool", persistence="node"))
    with pytest.raises(ProgramValidationError, match="node was removed"):
        Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize(
    ("value_type", "initial", "message"),
    [
        ("int64", 1.5, "signed 64-bit integer"),
        ("bool", 1, "must be a boolean"),
        ("int64", None, "omitted instead of null"),
        ("model_response", {}, "cannot have an authored default"),
    ],
)
def test_key_initial_value_is_typed(value_type, initial, message) -> None:
    document = _keyed_document(_key("value", value_type, initial=initial))
    with pytest.raises(ProgramValidationError, match=message):
        Program.from_dict(_identified(document), _catalog())


def test_initial_values_load_before_any_node_even_for_read_only_keys() -> None:
    program = Program.from_dict(
        _identified(
            _keyed_document(
                _key("count", "int64", initial=3),
                _key("empty", "string"),
                _key("limit", "float64", access="read", initial=0.25),
            )
        ),
        _catalog(),
    )
    run = _run(program)

    assert run.value_store.read("count") == 3
    assert run.value_store.read("limit") == 0.25
    assert run.value_store.read_bundle("limit")["meta"]["value_source"] == "initial"
    assert not run.value_store.contains("empty")
    with pytest.raises(ProgramValueError, match="not writable"):
        run.value_store.write("limit", 1.0)
    run.value_store.write("count", 4)
    # Each run starts again from the declared initial value.
    assert _run(program).value_store.read("count") == 3


def test_persistent_key_restores_then_commits_only_changes_before_visibility() -> None:
    program = Program.from_dict(
        _identified(
            _keyed_document(
                _key("fresh", "int64", persistence="persistent", initial=7),
                _key("index", "int64", persistence="persistent", initial=0),
            )
        ),
        _catalog(),
    )
    register = _Register({"index": 5})
    store = _run(program, register).value_store

    assert store.read("index") == 5
    assert store.read_bundle("index")["meta"]["value_source"] == "persistent_register"
    assert store.read("fresh") == 7
    store.write("index", 5)
    assert register.commits == []
    store.write("index", 6)
    assert register.commits == [("scenario.test", "index", 6)]
    register.fail = True
    with pytest.raises(ProgramValueError, match="could not be stored: disk full"):
        store.write("index", 8)
    assert store.read("index") == 6


def test_set_value_and_offset_pose_name_run_node_and_attempt_in_the_persistent_edit() -> None:
    offset = {"position_m": [0.0, 0.0, 0.1], "rpy_deg": [0.0, 0.0, 0.0]}
    program = _typed_program(
        [
            _key("count", "int64", persistence="persistent", initial=0),
            _key("place", "stored_pose", persistence="persistent"),
        ],
        _leaf("logic.set_value", "set_count", key="count", value=3),
        _leaf(
            "geometry.offset_pose",
            "lift",
            pose=_cell_pose(),
            offset=offset,
            output_key="place",
        ),
    )
    register = _Register()
    run = _run(program, register)
    assert _tick_to_end(run) is py_trees.common.Status.SUCCESS
    assert [key for _scenario, key, _value in register.commits] == ["count", "place"]
    for edit, node_id, instance_id in zip(
        register.edits,
        ("logic.set_value", "geometry.offset_pose"),
        ("set_count", "lift"),
        strict=True,
    ):
        assert edit == {
            "program_run_id": "run.test",
            "node_id": node_id,
            "node_instance_id": instance_id,
            "node_attempt_id": edit["node_attempt_id"],
        }
        assert re.fullmatch(r"attempt:[0-9a-f]{32}", edit["node_attempt_id"])
    assert register.edits[0]["node_attempt_id"] != register.edits[1]["node_attempt_id"]


_TCP = {"kind": "target_frame", "target_role": "arm", "frame_role": "tcp"}
_TABLE = {"kind": "scene_resource", "scene_resource_id": "table"}


def _two_robot_program(
    keys: Sequence[dict[str, Any]], *children: dict[str, Any]
) -> Program:
    """The minimal Program plus a second robot `peer` with base and flange."""

    document = _minimal_document()
    arm = document["target_roles"]["arm"]
    declared = {row["capability_id"] for row in arm["required_capabilities"]}
    arm["required_capabilities"] = sorted(
        [
            *arm["required_capabilities"],
            *(
                {"capability_id": capability, "parameters": {}, "disposition": "required"}
                for capability in ("planning.motion.cartesian_path", "planning.motion.pose_goal")
                if capability not in declared
            ),
        ],
        key=lambda row: row["capability_id"],
    )
    document["target_roles"]["peer"] = {
        **deepcopy(arm),
        "required_frame_roles": ["base", "flange"],
    }
    document["blackboard"] = {"namespaces": [], "keys": sorted(keys, key=lambda row: row["key"])}
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": list(children) or [_idle()],
    }
    return Program.from_dict(_identified(document), _catalog())


@pytest.mark.parametrize(
    ("key", "message"),
    [
        (_key("n", "int64", allowed_frames=[{"kind": "cell_frame"}]), "only for a stored_pose key"),
        (_key("p", "stored_pose", allowed_frames=[]), "must not be empty"),
        (_key("p", "stored_pose", allowed_frames=[_TCP, {"kind": "cell_frame"}]), "canonical-sorted"),
        (_key("p", "stored_pose", allowed_frames=[_TCP, _TCP]), "canonical-sorted"),
        (
            _key("p", "stored_pose", allowed_frames=[{"kind": "scene_resource", "scene_resource_id": "moon"}]),
            "does not name a declared scene resource",
        ),
        (
            _key(
                "p",
                "stored_pose",
                allowed_frames=[{"kind": "target_frame", "target_role": "peer", "frame_role": "tcp"}],
            ),
            "is not required by the selected target role",
        ),
        (
            _key(
                "p",
                "stored_pose",
                allowed_frames=[_TABLE],
                initial={
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
            ),
            "is not one of the key's allowed_frames",
        ),
    ],
)
def test_allowed_frames_is_a_closed_list_of_declared_pose_frames(key, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _two_robot_program([key])


def test_allowed_frames_restricts_every_value_of_a_pose_key() -> None:
    allowed = [{"kind": "cell_frame"}, _TABLE, _TCP]
    program = _two_robot_program(
        [_key("place", "stored_pose", allowed_frames=allowed, initial=_cell_pose())]
    )
    assert [dict(frame) for frame in program.blackboard_keys["place"].allowed_frames] == allowed
    store = ProgramValueStore.from_program(program)
    store.write("place", {**_cell_pose(), "frame_ref": _TCP})
    with pytest.raises(ProgramValueError, match="allowed_frames"):
        store.write("place", _portable_pose())
    assert store.read("place")["frame_ref"] == _TCP
    # Restored values take the same check (the store drops what it refuses).
    with pytest.raises(ProgramValidationError, match="allowed_frames"):
        program.validate_key_value("place", _portable_pose(), path="restored")
    # Without allowed_frames any declared frame is accepted, another robot's included.
    open_program = _two_robot_program([_key("place", "stored_pose")])
    open_program.validate_key_value(
        "place",
        {**_cell_pose(), "frame_ref": {"kind": "target_frame", "target_role": "peer", "frame_role": "flange"}},
        path="node",
    )


def test_poses_inside_structured_values_name_declared_frames() -> None:
    # Values a Program cannot author (paths, bundle fields) are written by
    # nodes; their pose frames take the same declared-frame check.
    plan_fields = [
        {"field": "approach_pose", "type": "stored_pose"},
        {"field": "descend_path", "type": "cartesian_path"},
    ]
    program = _two_robot_program(
        [
            _key("path", "cartesian_path"),
            _key("plan", "pick_planning_result", fields=plan_fields),
        ]
    )
    store = ProgramValueStore.from_program(program)
    flange = {"kind": "target_frame", "target_role": "peer", "frame_role": "flange"}
    undeclared = {"kind": "target_frame", "target_role": "peer", "frame_role": "tcp"}
    moon = {"kind": "scene_resource", "scene_resource_id": "moon"}
    framed = {**_cell_pose(), "frame_ref": flange}
    unframed = {"position_m": [0.0, 0.0, 0.1], "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}

    def plan(approach: dict[str, Any], *path: dict[str, Any]) -> dict[str, Any]:
        return {"approach_pose": approach, "descend_path": list(path)}

    store.write("path", [framed, unframed])
    store.write("plan", plan(framed, _cell_pose(), unframed))
    for key, value, message in (
        ("path", [unframed, {**framed, "frame_ref": undeclared}], r"'path'\[1\]\.frame_ref"),
        ("path", [{**framed, "frame_ref": moon}, unframed], "declared scene resource"),
        ("plan", plan({**framed, "frame_ref": undeclared}, framed, unframed), "approach_pose"),
        ("plan", plan(unframed, framed, unframed), "approach_pose must contain frame_ref"),
        ("plan", plan(framed, unframed, {**framed, "frame_ref": {"kind": "world"}}), "cell_frame"),
    ):
        with pytest.raises(ProgramValueError, match=message):
            store.write(key, value)
    assert store.read("path") == [framed, unframed]
    assert store.read("plan")["approach_pose"] == framed


def _relative_move(reference: dict[str, Any], **goal_frame: Any) -> dict[str, Any]:
    return _leaf(
        "motion.move_to_pose",
        "move",
        target_role="arm",
        base_frame_role="base",
        tcp_frame_role="tcp",
        goal={"position_m": [0.0, 0.0, 0.1], "orientation": {"rpy_deg": [0.0, 0.0, 0.0]}, **goal_frame},
        target_mode="relative",
        target_reference=reference,
    )


def test_reference_frames_pair_their_robot_and_frame_role() -> None:
    # Another robot's frame, named robot plus frame.
    _two_robot_program(
        [], _relative_move({"type": "frame_role", "frame_role_id": "flange", "target_role": "peer"})
    )
    _two_robot_program([], _relative_move({"type": "cell_frame"}))
    with pytest.raises(ProgramValidationError, match="required by target role 'peer'"):
        _two_robot_program(
            [], _relative_move({"type": "frame_role", "frame_role_id": "tcp", "target_role": "peer"})
        )
    # The unqualified form stays the moving robot's own frame.
    with pytest.raises(ProgramValidationError, match="required by target role 'arm'"):
        _two_robot_program([], _relative_move({"type": "frame_role", "frame_role_id": "flange"}))
    # A pose frame pairs its target and frame role too.
    with pytest.raises(ProgramValidationError, match="required by target role 'peer'"):
        _two_robot_program(
            [],
            _leaf(
                "geometry.check_pose_bounds",
                "bounds",
                pose={**_cell_pose(), "frame_ref": {"kind": "target_frame", "target_role": "peer", "frame_role": "tcp"}},
                frame_ref={"kind": "cell_frame"},
                min_position_m=[0.0, 0.0, 0.0],
                max_position_m=[1.0, 1.0, 1.0],
            ),
        )


def test_relative_goal_and_cartesian_waypoints_keep_one_owner_resolvable_frame() -> None:
    with pytest.raises(ProgramValidationError, match="relative goal is stated in the moving robot"):
        _two_robot_program([], _relative_move({"type": "cell_frame"}, frame_ref={"kind": "cell_frame"}))
    base = {"kind": "target_frame", "target_role": "arm", "frame_role": "base"}
    _two_robot_program([], _relative_move({"type": "cell_frame"}, frame_ref=base))
    pose = {"position_m": [0.0, 0.0, 0.1], "orientation": {"rpy_deg": [0.0, 0.0, 0.0]}}
    path = _leaf(
        "motion.follow_cartesian_path",
        "path",
        target_role="arm",
        base_frame_role="base",
        tcp_frame_role="tcp",
        path=[{**pose, "frame_ref": {"kind": "cell_frame"}}, {**pose, "frame_ref": base}],
    )
    with pytest.raises(ProgramValidationError, match="waypoints must all use one frame"):
        _two_robot_program([], path)


def test_relative_target_reference_presence_counts_a_whole_value_source() -> None:
    from manyforge_behavior import program as program_module

    check = program_module._validate_node_cross_fields  # noqa: SLF001
    with pytest.raises(ProgramValidationError, match="required for relative pose motion"):
        check("motion.move_to_pose", {"target_mode": "relative"}, sourced=frozenset(), path="n", context=None)
    check(
        "motion.move_to_pose",
        {"target_mode": "relative"},
        sourced=frozenset({"target_reference"}),
        path="n",
        context=None,
    )


def test_persistent_keys_require_the_register() -> None:
    program = Program.from_dict(
        _identified(
            _keyed_document(_key("index", "int64", persistence="persistent"))
        ),
        _catalog(),
    )
    with pytest.raises(ProgramValueError, match="persistent-key register"):
        _run(program)


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

    assert snapshot.parameters["template_variables"] == {
        "candidate_object_ids": ["sphere_part", "cube_part"],
        "scene_context_json": '{"revision":1}',
        "selection_request_text": "Select the sphere object.",
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
    assert task.starts[0][2].parameters["template_variables"] == {
        "candidate_object_ids": ("sphere_part", "cube_part"),
        "scene_context_json": '{"revision":1}',
        "selection_request_text": "Select the sphere object.",
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


def test_all_maintained_program_node_forms_execute_through_the_closed_runner() -> None:
    sources = tuple(
        sorted(
            path.relative_to(_ROOT).as_posix()
            for path in (_ROOT / "examples").glob("*.program.yaml")
            if not path.name.startswith("ur5e_lite6_wall_")
            and path.read_text(encoding="utf-8").startswith(
                "api_version: manyforge.program.v2\n"
            )
        )
    )
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
            # A day of Program time passes per reading: every timer elapses
            # on its first tick after it starts.
            program_clock=ProgramClock(_fast_source()),
        )
        for _tick in range(128):
            status = run.tick_once()
            if status is not py_trees.common.Status.RUNNING:
                break
        assert status is py_trees.common.Status.SUCCESS, (
            source,
            run.terminal_failure(),
        )
        exercised.update(node.node_id for node in _walk(program.tree))

    assert exercised == {
        "assistant.build_scene_selection",
        "assistant.query_model",
        "assistant.validate_scene_selection",
        "fallback",
        "gripper.command_position",
        "logic.check_condition",
        "logic.set_value",
        "motion.follow_cartesian_path",
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "parallel",
        "planning.plan_pick_sequence",
        "planning.plan_place_sequence",
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


def _fast_source(step_s: float = 86_400.0):
    now = [0.0]

    def read() -> float:
        now[0] += step_s
        return now[0]

    return read


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


def test_node_status_presentation_distinguishes_held_from_idle() -> None:
    from manyforge_behavior.program_runtime import _presented_node_status

    class Leaf(py_trees.behaviour.Behaviour):
        def __init__(self, status, held=False):
            super().__init__("leaf")
            self.status = status
            self.presented_held = held

        def update(self):
            return self.status

    running = py_trees.common.Status.RUNNING
    assert _presented_node_status(Leaf(running, held=True)) == "held"
    assert _presented_node_status(Leaf(running)) == "running"
    assert _presented_node_status(Leaf(py_trees.common.Status.INVALID)) == "idle"
    assert _presented_node_status(Leaf(py_trees.common.Status.SUCCESS)) == "success"


@pytest.mark.parametrize(
    "node_id", ["timer", "logic.wait_for_value", "gripper.wait_for_status"]
)
def test_timed_nodes_require_the_runs_program_clock(node_id: str) -> None:
    environment = build_node_environment()
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    with pytest.raises(ExecutableNodeResolverError, match="Program clock"):
        build_executable_node_resolver(environment).instantiate(
            environment.registry.record(node_id),
            NodeAdapterBuildContext(
                name="leaf",
                node_instance_id="leaf",
                parameters={},
                input_resolver=NodeAttemptInputResolver(
                    program=program,
                    catalog=_catalog(),
                    value_store=ProgramValueStore.from_program(program),
                ),
                attempt_source=ProgramRunAttemptSource("run.clock"),
                ports=_ports(),
            ),
        )


def test_every_cancellable_leaf_adapter_reports_interrupted_and_no_other_does() -> None:
    environment = build_node_environment()
    resolver = build_executable_node_resolver(environment)
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    input_resolver = NodeAttemptInputResolver(
        program=program,
        catalog=_catalog(),
        value_store=ProgramValueStore.from_program(program),
    )
    checked: dict[str, bool] = {}
    for disposition in resolver.dispositions:
        entry = environment.catalog.get(disposition.row.node_id)
        if entry.kind != "leaf" or not disposition.adapter_build_qualified:
            continue
        adapter = resolver.instantiate(
            disposition.row,
            NodeAdapterBuildContext(
                name="leaf",
                node_instance_id="leaf",
                parameters={"duration_s": 1.0},
                input_resolver=input_resolver,
                attempt_source=ProgramRunAttemptSource("run.flags"),
                ports=_ports(),
                program_clock=ProgramClock(),
            ),
        )
        reports = isinstance(adapter, InterruptedOutcome) and adapter.cancellable
        assert reports is entry.execution.cancellable, entry.id
        checked[entry.id] = reports

    assert {node_id for node_id, reports in checked.items() if reports} == {
        "assistant.build_scene_selection",
        "assistant.query_model",
        "assistant.validate_scene_selection",
        "gripper.command_position",
        "gripper.wait_for_status",
        "logic.wait_for_value",
        "motion.follow_cartesian_path",
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "planning.plan_pick_sequence",
        "planning.plan_place_sequence",
        "simulation.reset",
        "simulation.set_entity_pose",
        "simulation.set_state",
        "simulation.step",
        "timer",
    }


def _owner_task_adapter(node_id: str, task: _TaskPort, **context: Any):
    environment = build_node_environment()
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    return build_executable_node_resolver(environment).instantiate(
        environment.registry.record(node_id),
        NodeAdapterBuildContext(
            name="leaf",
            node_instance_id="leaf",
            parameters={},
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=_catalog(),
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.interrupt"),
            ports=_ports(task),
            **context,
        ),
    )


def test_stopping_a_running_owner_task_reports_interrupted_never_failure() -> None:
    task = _TaskPort((OwnerTaskState.ACTIVE,) * 3)
    adapter = _owner_task_adapter("simulation.reset", task)
    adapter.tick_once()
    assert adapter.status is py_trees.common.Status.RUNNING

    adapter.stop(py_trees.common.Status.INVALID)

    assert task.cancellations == [("task-1", task.starts[0][1])]
    assert adapter.outcome == "interrupted"
    assert adapter.presented_interrupted
    assert adapter.interrupted_reason_code == "program_node_interrupted"
    assert adapter.failure_reason_code is None
    assert not adapter.presented_stop_failed
    assert _presented_node_status(adapter) == "interrupted"
    adapter.tick_once()  # A fresh attempt starts clean.
    assert adapter.interrupted_reason_code is None


class _RefusingCancelPort(_TaskPort):
    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self.cancellations.append((task_id, identity))
        raise RuntimeError("owner unreachable")


@pytest.mark.parametrize("node_id", ["simulation.reset", "kernel.full_stop"])
def test_refused_cancellation_reports_failure_never_interrupted(node_id: str) -> None:
    task = _RefusingCancelPort((OwnerTaskState.ACTIVE,) * 3)
    adapter = _owner_task_adapter(node_id, task)
    adapter.tick_once()
    assert adapter.status is py_trees.common.Status.RUNNING

    adapter.stop(py_trees.common.Status.INVALID)

    # An unconfirmed stop pauses for the operator: it is a failure.
    assert len(task.cancellations) == 1
    assert adapter.outcome == "failure"
    assert adapter.presented_stop_failed
    assert not adapter.presented_interrupted
    assert adapter.interrupted_reason_code is None
    assert adapter.failure_reason_code == "owner_cancellation_failed"
    assert adapter.failure_message == "owner unreachable"
    assert _presented_node_status(adapter) == "failure"
    adapter.tick_once()  # A fresh attempt starts clean.
    assert adapter.outcome is None
    assert not adapter.presented_stop_failed
    assert adapter.failure_reason_code is None


def test_operator_stop_held_task_stopped_by_program_stop_reports_interrupted() -> None:
    class _StoppedPort(_TaskPort):
        def observe(self, task_id: str) -> OwnerTaskSnapshot:
            self.observations.append(task_id)
            return OwnerTaskSnapshot(
                task_id=task_id,
                state=OwnerTaskState.INTERRUPTED,
                metadata={
                    "authored_failure_policy": "pause_for_operator",
                    "authored_failure_policy_parameter": "operator_stop_policy",
                },
                reason_code="operation_stopped",
                message="native operation was stopped",
            )

    class _Interventions:
        def __init__(self) -> None:
            self.required: list[str] = []

        def require(self, _identity: NodeAttemptIdentity, **kwargs: Any) -> None:
            self.required.append(kwargs["reason_code"])

        def consume_resolution(self, _identity: NodeAttemptIdentity) -> None:
            return None

    interventions = _Interventions()
    task = _StoppedPort((OwnerTaskState.ACTIVE,))
    adapter = _owner_task_adapter(
        "simulation.reset",
        task,
    )
    adapter._intervention_port = interventions  # noqa: SLF001
    adapter.tick_once()
    adapter.tick_once()  # The Stop holds the interrupted task for Resume.
    assert adapter.status is py_trees.common.Status.RUNNING
    assert interventions.required == ["operation_stopped"]
    assert adapter.outcome is None

    # A Program Stop or cancel ends the hold (a Halt freezes it instead).
    adapter.stop(py_trees.common.Status.INVALID)

    assert adapter.outcome == "interrupted"
    assert adapter.interrupted_reason_code == "program_node_interrupted"


def test_non_cancellable_running_leaf_never_reports_interrupted() -> None:
    task = _TaskPort((OwnerTaskState.ACTIVE,) * 3)
    adapter = _owner_task_adapter("kernel.full_stop", task)
    assert not adapter.cancellable
    adapter.tick_once()
    assert adapter.status is py_trees.common.Status.RUNNING

    adapter.stop(py_trees.common.Status.INVALID)

    assert adapter.outcome is None
    assert not adapter.presented_interrupted
    assert adapter.interrupted_reason_code is None


def test_stopped_program_run_presents_running_cancellable_leaves_as_interrupted() -> None:
    document = _minimal_document()
    motion = deepcopy(document["tree"]["children"][0])
    document["blackboard"] = {"namespaces": [], "keys": [_key("count", "int64", initial=0)]}
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "together",
        "parameters": {"success_policy": "all"},
        "children": [
            motion,
            _leaf("timer", "pause", duration_s=60.0),
            _leaf("logic.wait_for_value", "wait", key="count", operator="==", value=1),
            _leaf("logic.check_condition", "check", condition=True),
        ],
    }
    program = Program.from_dict(_identified(document), _catalog())
    task = _TaskPort((OwnerTaskState.ACTIVE,) * 4)
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(task),
    )
    assert run.tick_once() is py_trees.common.Status.RUNNING
    motion_id = motion["instance_id"]
    assert run.node_statuses[motion_id] == "running"

    run.stop()

    assert dict(run.node_statuses) == {
        "together": "idle",
        motion_id: "interrupted",
        "pause": "interrupted",
        "wait": "interrupted",
        "check": "idle",
    }
    assert len(task.cancellations) == 1


def test_stopped_program_run_presents_a_refused_cancellation_as_failure() -> None:
    document = _minimal_document()
    motion = deepcopy(document["tree"]["children"][0])
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "together",
        "parameters": {"success_policy": "all"},
        "children": [motion, _leaf("timer", "pause", duration_s=60.0)],
    }
    program = Program.from_dict(_identified(document), _catalog())
    task = _RefusingCancelPort((OwnerTaskState.ACTIVE,) * 4)
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(task),
    )
    assert run.tick_once() is py_trees.common.Status.RUNNING
    motion_id = motion["instance_id"]

    run.stop()

    # The owner refused to cancel the motion: failure. The confirmed timer
    # stop stays interrupted.
    assert dict(run.node_statuses) == {
        "together": "idle",
        motion_id: "failure",
        "pause": "interrupted",
    }
    assert len(task.cancellations) == 1


# --- V13.3 typed value nodes (Logic) and pose nodes (Geometry, Scene) -------


def _cell_pose(position=(0.1, 0.2, 0.3), orientation=None) -> dict[str, Any]:
    return {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": list(position),
        "orientation": orientation or {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }


def _typed_program(
    keys: Sequence[dict[str, Any]],
    *children: dict[str, Any],
    parameters: Sequence[dict[str, Any]] = (),
) -> Program:
    document = _minimal_document()
    document["blackboard"] = {
        "namespaces": [],
        "keys": sorted(keys, key=lambda row: row["key"]),
    }
    document["parameters"] = sorted(
        [*document["parameters"], *parameters],
        key=lambda row: row["parameter_id"],
    )
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": list(children),
    }
    return Program.from_dict(_identified(document), _catalog())


def _leaf(
    node_id: str,
    instance_id: str,
    value_sources: Mapping[str, Any] | None = None,
    **parameters: Any,
) -> dict[str, Any]:
    node = {
        "node_id": node_id,
        "instance_id": instance_id,
        "parameters": parameters,
        "children": [],
    }
    if value_sources:
        node["value_sources"] = dict(value_sources)
    return node


def _source(kind: str, reference: str) -> dict[str, str]:
    return {"kind": kind, "reference": reference}


def _parameter(parameter_id: str, value_type: str, default: Any) -> dict[str, Any]:
    return {
        "parameter_id": parameter_id,
        "type": value_type,
        "default": default,
    }


def _tick_to_end(run: ExecutableProgramRun, ticks: int = 16) -> py_trees.common.Status:
    status = run.tick_once()
    for _ in range(ticks):
        if status is not py_trees.common.Status.RUNNING:
            break
        status = run.tick_once()
    return status


def _feedback(run: ExecutableProgramRun) -> str:
    return run._tree.tip().feedback_message  # noqa: SLF001


def test_set_value_writes_values_typed_by_the_key_from_every_source() -> None:
    program = _typed_program(
        [
            _key("count", "int64", initial=1),
            _key("label", "string"),
            _key("limit", "int64", initial=7),
            _key("place", "stored_pose"),
            _key("ready", "bool"),
            _key("speed", "float64"),
        ],
        _leaf("logic.set_value", "set_ready", key="ready", value=True),
        _leaf("logic.set_value", "set_speed", key="speed", value=2),
        _leaf("logic.set_value", "set_label", key="label", value="slot a"),
        _leaf("logic.set_value", "set_place", key="place", value=_portable_pose()),
        _leaf(
            "logic.set_value",
            "count_from_key",
            {"value": _source("blackboard_key", "limit")},
            key="count",
        ),
        _leaf(
            "logic.set_value",
            "speed_from_parameter",
            {"value": _source("program_parameter", "pick_velocity_scale")},
            key="speed",
        ),
    )
    run = _run(program)

    assert _tick_to_end(run) is py_trees.common.Status.SUCCESS
    assert run.value_store.read("ready") is True
    assert run.value_store.read("label") == "slot a"
    assert run.value_store.read("place") == _portable_pose()
    assert run.value_store.read("count") == 7
    assert run.value_store.read("speed") == 0.5


@pytest.mark.parametrize(
    ("key", "node", "message"),
    [
        (
            _key("count", "int64"),
            _leaf("logic.set_value", "set", key="count", value=2.5),
            "signed 64-bit integer",
        ),
        (
            _key("count", "int64"),
            _leaf("logic.set_value", "set", key="count", value=True),
            "signed 64-bit integer",
        ),
        (
            _key("place", "stored_pose"),
            _leaf(
                "logic.set_value",
                "set",
                key="place",
                value={
                    **_portable_pose(),
                    "frame_ref": {
                        "kind": "target_frame",
                        "target_role": "arm",
                        "frame_role": "camera",
                    },
                },
            ),
            "is not required by the selected target role",
        ),
        (
            _key("count", "int64"),
            _leaf(
                "logic.set_value",
                "set",
                {"value": _source("program_parameter", "pick_velocity_scale")},
                key="count",
            ),
            "source type 'float64' does not match the 'int64' type of key 'count'",
        ),
        (
            _key("count", "int64", access="read"),
            _leaf("logic.set_value", "set", key="count", value=1),
            "does not permit write access",
        ),
        (
            _key("plan", "model_response", fields=[{"field": "reason", "type": "string"}]),
            _leaf("logic.set_value", "set", key="plan", value="x"),
            "declares type 'model_response', expected",
        ),
        (
            _key("place", "stored_pose"),
            _leaf("logic.check_value", "check", key="place", value=1.0),
            "declares type 'stored_pose', expected",
        ),
        (
            _key("ready", "bool"),
            _leaf("logic.check_value", "check", key="ready", operator="<", value=True),
            "bool keys support only == and !=",
        ),
        (
            _key("label", "string"),
            _leaf("logic.check_value", "check", key="label", value="a", tolerance=0.1),
            "tolerance applies only to == and != on numeric keys",
        ),
        (
            _key("speed", "float64"),
            _leaf(
                "logic.wait_for_value",
                "wait",
                key="speed",
                operator=">=",
                value=1.0,
                tolerance=0.1,
                timeout_s=1.0,
            ),
            "tolerance applies only to == and != on numeric keys",
        ),
    ],
)
def test_key_typed_parameters_are_refused_by_name_at_validation(
    key, node, message
) -> None:
    with pytest.raises(ProgramValidationError, match=re.escape(message)):
        _typed_program([key], node)


def test_key_typed_value_is_checked_again_at_node_attempt_time() -> None:
    """An attempt's input value or a sourced pose must still match the key's type."""
    program = _typed_program(
        [_key("count", "int64"), _key("copy", "stored_pose"), _key("place", "stored_pose")],
        _leaf("logic.set_value", "set_count", key="count", value=1),
        _leaf(
            "logic.set_value",
            "copy_place",
            {"value": _source("blackboard_key", "place")},
            key="copy",
        ),
    )
    catalog = _catalog()
    store = ProgramValueStore.from_program(program)
    resolver = NodeAttemptInputResolver(
        program=program, catalog=catalog, value_store=store
    )
    set_count, copy_place = program.tree.children
    with pytest.raises(ProgramValueError, match="signed 64-bit integer") as invalid:
        resolver.snapshot(
            node_id="logic.set_value",
            node_instance_id="set_count",
            # closed union: a number, but not the key's int64
            parameters={**set_count.parameters, "value": 2.5},
            value_sources=set_count.value_sources,
        )
    assert dict(invalid.value.details) == {
        "input_problem": "invalid",
        "path": "value",
        "source_kind": "parameter",
        "source_reference": None,
    }
    # Every write is checked with the Program's declared frames, so a key
    # never holds a pose in an undeclared frame.
    with pytest.raises(ProgramValueError, match="does not name a declared scene resource"):
        store.write(
            "place",
            {
                **_portable_pose(),
                "frame_ref": {"kind": "scene_resource", "scene_resource_id": "nope"},
            },
        )
    with pytest.raises(
        NodeInputError,
        match=r"^missing input value from blackboard_key 'place': "
        r"blackboard key 'place' has no value$",
    ) as missing:
        resolver.snapshot(
            node_id="logic.set_value",
            node_instance_id="copy_place",
            parameters=copy_place.parameters,
            value_sources=copy_place.value_sources,
        )
    assert dict(missing.value.details) == {
        "input_problem": "missing",
        "path": "value",
        "source_kind": "blackboard_key",
        "source_reference": "place",
    }


@pytest.mark.parametrize(
    ("key", "actual", "operator", "value", "tolerance", "matched"),
    [
        (_key("n", "int64"), 3, "==", 3, None, True),
        (_key("n", "int64"), 3, "!=", 3, None, False),
        (_key("n", "int64"), 3, "<", 4, None, True),
        (_key("n", "int64"), 3, "<=", 3, None, True),
        (_key("n", "int64"), 3, ">", 3, None, False),
        (_key("n", "int64"), 3, ">=", 4, None, False),
        (_key("n", "float64"), 0.105, "==", 0.1, 0.01, True),
        (_key("n", "float64"), 0.105, "==", 0.1, None, False),
        (_key("n", "float64"), 0.2, "!=", 0.1, 0.05, True),
        (_key("n", "bool"), True, "!=", False, None, True),
        (_key("n", "string"), "slot a", "==", "slot a", None, True),
        (_key("n", "string"), "slot a", "!=", "slot a", None, False),
    ],
)
def test_check_value_compares_by_key_type(
    key, actual, operator, value, tolerance, matched
) -> None:
    extra = {} if tolerance is None else {"tolerance": tolerance}
    program = _typed_program(
        [{**key, "initial": actual}],
        _leaf("logic.check_value", "check", key="n", operator=operator, value=value, **extra),
    )
    expected = py_trees.common.Status.SUCCESS if matched else py_trees.common.Status.FAILURE
    assert _run(program).tick_once() is expected


def test_check_value_fails_on_an_empty_key() -> None:
    program = _typed_program(
        [_key("n", "int64")], _leaf("logic.check_value", "check", key="n", value=1)
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    assert "node_input_unavailable" in _feedback(run)


def test_wait_for_value_reads_the_watched_key_fresh_on_every_tick() -> None:
    program = _typed_program(
        [_key("count", "int64"), _key("target", "int64", initial=3)],
        _leaf(
            "logic.wait_for_value",
            "wait",
            {"value": _source("blackboard_key", "target")},
            key="count",
            operator=">=",
            timeout_s=60.0,
        ),
    )
    run = _run(program)

    assert run.tick_once() is py_trees.common.Status.RUNNING  # still empty
    run.value_store.write("count", 2)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    # The threshold is a fixed input of this attempt, resolved once.
    run.value_store.write("target", 10)
    run.value_store.write("count", 3)
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def test_wait_for_value_times_out_on_the_program_clock_and_retry_is_fresh() -> None:
    now = [100.0]
    program = _typed_program(
        [_key("count", "int64", initial=0), _key("target", "int64", initial=5)],
        _leaf(
            "logic.wait_for_value",
            "wait",
            {"value": _source("blackboard_key", "target")},
            key="count",
            operator="==",
            timeout_s=2.0,
        ),
    )
    wait = program.tree.children[0]
    store = ProgramValueStore.from_program(program)
    node = build_executable_node_resolver(build_node_environment()).instantiate(
        build_node_environment().registry.record("logic.wait_for_value"),
        NodeAdapterBuildContext(
            name="wait",
            node_instance_id="wait",
            parameters=wait.parameters,
            value_sources=wait.value_sources,
            input_resolver=NodeAttemptInputResolver(
                program=program, catalog=_catalog(), value_store=store
            ),
            attempt_source=ProgramRunAttemptSource("run.test"),
            program_clock=ProgramClock(lambda: now[0]),
        ),
    )

    node.initialise()
    now[0] = 101.9
    assert node.update() is py_trees.common.Status.RUNNING
    now[0] = 102.0
    assert node.update() is py_trees.common.Status.FAILURE
    assert node.feedback_message.startswith("wait_timed_out: 'count'")
    # Retry: a fresh attempt resolves the threshold again and restarts the clock.
    store.write("target", 0)
    node.initialise()
    assert node.update() is py_trees.common.Status.SUCCESS


def test_wait_for_value_without_timeout_waits_until_the_value_matches() -> None:
    now = [100.0]
    program = _typed_program(
        [_key("count", "int64", initial=0)],
        _leaf("logic.wait_for_value", "wait", key="count", operator="==", value=5),
    )
    run = _run(program, program_clock=ProgramClock(lambda: now[0]))

    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 1.0e9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("count", 5)
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def _held_wait(
    now: list[float], node_id: str = "logic.wait_for_value", **parameters: Any
) -> tuple[OperatorMotionStopPolicy, OperatorMotionStopState]:
    """One timed node held as a run holds it: the Stop state and the node
    share one fake Program clock and the hold policy skips ticks while held."""

    leaf = (
        _leaf(node_id, "wait", key="count", value=5, **parameters)
        if node_id == "logic.wait_for_value"
        else _leaf(node_id, "wait", **parameters)
    )
    program = _typed_program([_key("count", "int64", initial=0)], leaf)
    wait = program.tree.children[0]
    clock = ProgramClock(lambda: now[0])
    state = OperatorMotionStopState({"wait": ()}, clock=clock.now)
    node = build_executable_node_resolver(build_node_environment()).instantiate(
        build_node_environment().registry.record(node_id),
        NodeAdapterBuildContext(
            name="wait",
            node_instance_id="wait",
            parameters=wait.parameters,
            value_sources=wait.value_sources,
            input_resolver=NodeAttemptInputResolver(
                program=program,
                catalog=_catalog(),
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource("run.test"),
            operator_stop_state=state,
            operator_stop_hold_scope="wait",
            program_clock=clock,
        ),
    )
    held = OperatorMotionStopPolicy(child=node, node_instance_id="wait", stop_state=state)
    return held, state


def _tick(node: py_trees.behaviour.Behaviour) -> py_trees.common.Status:
    list(node.tick())
    return node.status


def test_wait_for_value_count_mode_counts_held_time() -> None:
    now = [100.0]
    node, state = _held_wait(now, timeout_s=2.0, timeout_while_held="count")
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    now[0] = 105.0
    assert _tick(node) is py_trees.common.Status.RUNNING  # held: not ticked
    state.publish(OperatorMotionStopSnapshot(2))
    assert _tick(node) is py_trees.common.Status.FAILURE
    assert node.decorated.feedback_message.startswith("wait_timed_out: 'count'")


@pytest.mark.parametrize("mode", [None, "pause"])
def test_wait_for_value_pause_excludes_held_time_and_resumes_the_remainder(
    mode: str | None,
) -> None:
    now = [100.0]
    extra = {} if mode is None else {"timeout_while_held": mode}
    node, state = _held_wait(now, timeout_s=2.0, **extra)
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    now[0] = 105.0
    assert _tick(node) is py_trees.common.Status.RUNNING
    state.publish(OperatorMotionStopSnapshot(2))
    # 0.5 s ran before the hold; 1.5 s remain after Resume.
    now[0] = 106.4
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 106.5
    assert _tick(node) is py_trees.common.Status.FAILURE
    assert node.decorated.feedback_message.startswith("wait_timed_out: 'count'")


def test_wait_for_value_held_time_policy_without_a_timeout_has_no_effect() -> None:
    now = [100.0]
    node, state = _held_wait(now, timeout_while_held="pause")
    assert _tick(node) is py_trees.common.Status.RUNNING
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    state.publish(OperatorMotionStopSnapshot(2))
    now[0] = 1.0e9
    assert _tick(node) is py_trees.common.Status.RUNNING


def _stopped_run_wait(
    now: list[float],
    policy: str,
    parent_policy: str | None = None,
    node_id: str = "logic.wait_for_value",
    **parameters: Any,
) -> ExecutableProgramRun:
    """A run whose timed node `wait` has `policy`, optionally inside a
    `branch` sequence with `parent_policy`; the run's Stop state and the node
    read one fake Program clock."""

    wait = (
        _leaf(node_id, "wait", key="count", value=5, **parameters)
        if node_id == "logic.wait_for_value"
        else _leaf(node_id, "wait", **parameters)
    )
    wait["operator_stop_policy"] = policy
    if parent_policy is not None:
        wait = {
            "node_id": "sequence",
            "instance_id": "branch",
            "parameters": {},
            "children": [wait],
            "operator_stop_policy": parent_policy,
        }
    return _run(
        _typed_program([_key("count", "int64", initial=0)], wait),
        program_clock=ProgramClock(lambda: now[0]),
    )


def test_wait_for_value_held_by_its_own_policy_pauses_by_default_in_a_run() -> None:
    now = [100.0]
    run = _stopped_run_wait(now, "hold", timeout_s=2.0)
    state = run._operator_stop_state  # noqa: SLF001
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"root", "wait"})))
    now[0] = 105.0
    assert run.tick_once() is py_trees.common.Status.RUNNING  # held: not ticked
    state.publish(OperatorMotionStopSnapshot(2))
    now[0] = 106.4
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 106.5
    assert run.tick_once() is py_trees.common.Status.FAILURE


def test_wait_for_value_continue_under_a_held_hold_parent_pauses_by_default() -> None:
    now = [100.0]
    run = _stopped_run_wait(now, "continue", "hold", timeout_s=2.0)
    state = run._operator_stop_state  # noqa: SLF001
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 100.5
    # The parent's hold stops ticking the wait: it is held, so it pauses.
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"root", "branch", "wait"})))
    now[0] = 105.0
    assert run.tick_once() is py_trees.common.Status.RUNNING
    state.publish(OperatorMotionStopSnapshot(2))
    now[0] = 106.4
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 106.5
    assert run.tick_once() is py_trees.common.Status.FAILURE


@pytest.mark.parametrize("mode", [None, "pause", "count"])
def test_wait_for_value_kept_running_by_continue_policy_keeps_counting(
    mode: str | None,
) -> None:
    now = [100.0]
    extra = {} if mode is None else {"timeout_while_held": mode}
    run = _stopped_run_wait(now, "continue", timeout_s=2.0, **extra)
    state = run._operator_stop_state  # noqa: SLF001
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 100.5
    # The Stop selection covers the wait, but its policy keeps it running.
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"root", "wait"})))
    now[0] = 101.9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 102.0
    assert run.tick_once() is py_trees.common.Status.FAILURE
    assert run._tree.tip().feedback_message.startswith(  # noqa: SLF001
        "wait_timed_out: 'count'"
    )


def _leaf_node(run: ExecutableProgramRun, instance_id: str) -> Any:
    node = run._authored_nodes[instance_id]  # noqa: SLF001
    return node.decorated if isinstance(node, OperatorMotionStopPolicy) else node


def test_timer_delays_for_its_duration_of_program_time_and_succeeds() -> None:
    now = [100.0]
    run = _run(
        _typed_program([], _leaf("timer", "delay", duration_s=2.0)),
        program_clock=ProgramClock(lambda: now[0]),
    )
    timer = _leaf_node(run, "delay")
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 101.9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 102.0
    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert timer.outcome == "success"


@pytest.mark.parametrize("mode", [None, "pause"])
def test_timer_pause_excludes_held_time_and_resumes_the_remainder(
    mode: str | None,
) -> None:
    now = [100.0]
    extra = {} if mode is None else {"duration_while_held": mode}
    node, state = _held_wait(now, "timer", duration_s=2.0, **extra)
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    now[0] = 105.0
    assert _tick(node) is py_trees.common.Status.RUNNING  # held: not ticked
    state.publish(OperatorMotionStopSnapshot(2))
    # 0.5 s ran before the hold; 1.5 s remain after Resume.
    now[0] = 106.4
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 106.5
    assert _tick(node) is py_trees.common.Status.SUCCESS


def test_timer_count_mode_counts_held_time() -> None:
    now = [100.0]
    node, state = _held_wait(now, "timer", duration_s=2.0, duration_while_held="count")
    assert _tick(node) is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    now[0] = 105.0
    assert _tick(node) is py_trees.common.Status.RUNNING  # held: not ticked
    state.publish(OperatorMotionStopSnapshot(2))
    assert _tick(node) is py_trees.common.Status.SUCCESS


@pytest.mark.parametrize("mode", [None, "pause", "count"])
def test_timer_kept_running_by_continue_policy_keeps_counting(mode: str | None) -> None:
    now = [100.0]
    extra = {} if mode is None else {"duration_while_held": mode}
    run = _stopped_run_wait(now, "continue", node_id="timer", duration_s=2.0, **extra)
    state = run._operator_stop_state  # noqa: SLF001
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 100.5
    # The Stop selection covers the timer, but its policy keeps it running.
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"root", "wait"})))
    now[0] = 101.9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 102.0
    assert run.tick_once() is py_trees.common.Status.SUCCESS


@pytest.mark.parametrize("node_id", ["timer", "logic.wait_for_value"])
def test_frozen_program_time_never_counts_and_the_attempt_continues(
    node_id: str,
) -> None:
    """A Halt freezes the run's Program clock; the attempt keeps its identity
    and only its unfrozen time counts, whatever the held-time choice."""
    now = [100.0]
    parameters = (
        {"duration_s": 2.0, "duration_while_held": "count"}
        if node_id == "timer"
        else {"timeout_s": 2.0, "timeout_while_held": "count"}
    )
    run = _stopped_run_wait(now, "continue", node_id=node_id, **parameters)
    clock = run.program_clock
    node = _leaf_node(run, "wait")
    assert run.tick_once() is py_trees.common.Status.RUNNING
    attempt = node._identity.node_attempt_id  # noqa: SLF001
    now[0] = 101.0
    clock.freeze()
    now[0] = 500.0
    clock.resume()
    now[0] = 500.9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert node._identity.node_attempt_id == attempt  # noqa: SLF001
    now[0] = 501.0
    expected = (
        py_trees.common.Status.SUCCESS
        if node_id == "timer"
        else py_trees.common.Status.FAILURE
    )
    assert run.tick_once() is expected


def test_a_hold_spanning_frozen_program_time_is_not_subtracted_twice() -> None:
    now = [100.0]
    run = _stopped_run_wait(now, "hold", node_id="timer", duration_s=2.0)
    clock = run.program_clock
    state = run._operator_stop_state  # noqa: SLF001
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 100.5
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"root", "wait"})))
    now[0] = 101.0
    clock.freeze()  # Halt while held
    now[0] = 110.0
    clock.resume()  # Execute
    now[0] = 111.0
    state.publish(OperatorMotionStopSnapshot(2))  # Resume Motion
    # Program time ran 0.5 s before the hold; 1.5 s remain from 111.0.
    now[0] = 112.4
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 112.5
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def test_timer_duration_comes_from_its_value_source_once_per_attempt() -> None:
    now = [100.0]
    run = _run(
        _typed_program(
            [_key("delay", "float64", initial=2.0)],
            _leaf("timer", "delay", {"duration_s": _source("blackboard_key", "delay")}),
        ),
        program_clock=ProgramClock(lambda: now[0]),
    )
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("delay", 100.0)  # The running attempt keeps 2 s.
    now[0] = 101.9
    assert run.tick_once() is py_trees.common.Status.RUNNING
    now[0] = 102.0
    assert run.tick_once() is py_trees.common.Status.SUCCESS


@pytest.mark.parametrize("initial", [None, 0.0, 86_401.0])
def test_timer_with_a_missing_or_invalid_sourced_duration_fails_by_name(
    initial: float | None,
) -> None:
    extra = {} if initial is None else {"initial": initial}
    run = _run(
        _typed_program(
            [_key("delay", "float64", **extra)],
            _leaf("timer", "delay", {"duration_s": _source("blackboard_key", "delay")}),
        ),
        program_clock=ProgramClock(lambda: 100.0),
    )
    assert run.tick_once() is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    assert failure.reason_code == "node_input_unavailable"
    timer = _leaf_node(run, "delay")
    # The timer's catalog declares only success and interrupted; its inputs
    # still fail, before the wait starts, and the run presents that failure.
    assert set(run._environment.catalog.get("timer").outcomes) == {  # noqa: SLF001
        "success",
        "interrupted",
    }
    assert timer.outcome == "failure"
    assert run.node_statuses["delay"] == "failure"
    problem = "missing" if initial is None else "invalid"
    expected_details = {
        "input_problem": problem,
        "path": "duration_s",
        "source_kind": "blackboard_key",
        "source_reference": "delay",
    }
    assert dict(timer.failure_details) == expected_details
    assert dict(failure.details) == expected_details
    assert dict(failure.origin) == {
        "kind": "program_node",
        "node_instance_id": "delay",
        "owner_api": None,
    }
    assert failure.message.startswith(
        f"{problem} input duration_s from blackboard_key 'delay': "
    )


def test_timer_input_failure_counts_as_a_failure_for_an_enclosing_retry() -> None:
    """Retry counts an input failure like any child failure, and each new
    child attempt resolves its inputs again."""
    now = [100.0]
    program = _typed_program(
        [_key("delay", "float64"), _key("tries", "int64", initial=0)],
        {
            "node_id": "retry",
            "instance_id": "again",
            "parameters": {"num_attempts": 3},
            "children": [
                _leaf(
                    "timer",
                    "delay",
                    {"duration_s": _source("blackboard_key", "delay")},
                )
            ],
        },
    )
    run = _run(program, program_clock=ProgramClock(lambda: now[0]))
    retry = run._authored_nodes["again"]  # noqa: SLF001
    timer = _leaf_node(run, "delay")
    # First attempt: `delay` has no value, so the timer fails before waiting.
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert timer.failure_reason_code == "node_input_unavailable"
    assert timer.failure_details["input_problem"] == "missing"
    assert retry._attempt_count == 1  # noqa: SLF001
    # Second attempt: an out-of-schema value is invalid, counted again.
    run.value_store.write("delay", 0.0)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert timer.failure_details["input_problem"] == "invalid"
    assert retry._attempt_count == 2  # noqa: SLF001
    # Third attempt resolves the fixed value and waits for it.
    run.value_store.write("delay", 1.0)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert timer.failure_reason_code is None and timer.failure_details is None
    now[0] = 101.0
    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert timer.outcome == "success"


def test_timer_input_failure_exhausts_an_enclosing_retry_by_name() -> None:
    program = _typed_program(
        [_key("delay", "float64")],
        {
            "node_id": "retry",
            "instance_id": "again",
            "parameters": {"num_attempts": 2},
            "children": [
                _leaf(
                    "timer",
                    "delay",
                    {"duration_s": _source("blackboard_key", "delay")},
                )
            ],
        },
    )
    run = _run(program, program_clock=ProgramClock(lambda: 100.0))
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    assert failure.reason_code == "node_input_unavailable"
    assert failure.message.startswith(
        "missing input duration_s from blackboard_key 'delay'"
    )
    # The exhausted retry presents the timer's own input failure.
    assert dict(failure.origin) == {
        "kind": "program_node",
        "node_instance_id": "delay",
        "owner_api": None,
    }
    assert run.node_statuses["again"] == "failure"
    assert run.node_statuses["delay"] == "failure"


@pytest.mark.parametrize(
    ("keys", "value_sources", "reason", "details", "cause"),
    [
        (
            [_key("count", "float64")],
            {"value": {"kind": "blackboard_key", "reference": "count"}},
            "node_input_unavailable",
            ("missing", "value", "blackboard_key", "count"),
            "blackboard key 'count' has no value",
        ),
        (
            [_key("count", "float64")],
            {"value": {"kind": "expression", "expression": "key.count + 1.0"}},
            "expression_evaluation_failed",
            ("missing", "value", "expression", "key.count + 1.0"),
            "key.count has no value",
        ),
        (
            [_key("count", "float64", initial=0.0)],
            {"value": {"kind": "expression", "expression": "10.0 / key.count"}},
            "expression_evaluation_failed",
            ("invalid", "value", "expression", "10.0 / key.count"),
            "division_by_zero",
        ),
        (
            [
                _key("divisor", "float64", initial=0.0),
                _key("ratio", "float64", access="read", expression="10.0 / key.divisor"),
            ],
            {"value": {"kind": "blackboard_key", "reference": "ratio"}},
            "expression_evaluation_failed",
            ("invalid", "value", "blackboard_key", "ratio"),
            "computed key 'ratio' = '10.0 / key.divisor'",
        ),
    ],
)
def test_input_failures_name_the_problem_path_and_source(
    keys: list[dict[str, Any]],
    value_sources: dict[str, Any],
    reason: str,
    details: tuple[str, str, str, str],
    cause: str,
) -> None:
    program = _typed_program(
        [*keys, _key("out", "float64", initial=0.0)],
        _leaf("logic.set_value", "set_out", value_sources, key="out", value=0.0),
    )
    run = _run(program)
    assert _tick_to_end(run) is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    failed_reason, message = failure.reason_code, failure.message
    node = _leaf_node(run, "set_out")
    problem, path, kind, reference = details
    assert failed_reason == reason
    expected_details = {
        "input_problem": problem,
        "path": path,
        "source_kind": kind,
        "source_reference": reference,
    }
    assert dict(node.failure_details) == expected_details
    # The run carries the node's own details, attributed to the node.
    assert dict(failure.details) == expected_details
    assert dict(failure.origin) == {
        "kind": "program_node",
        "node_instance_id": "set_out",
        "owner_api": None,
    }
    assert message.startswith(f"{problem} input {path} from {kind} {reference!r}: ")
    assert cause in message
    # The node's own work never started: nothing was written.
    assert run.value_store.read("out") == 0.0


def test_a_missing_authored_or_read_key_names_the_reading_parameter() -> None:
    program = _typed_program(
        [_key("count", "int64")],
        _leaf("logic.check_value", "check", key="count", operator="==", value=1),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    node = _leaf_node(run, "check")
    assert node.failure_reason_code == "node_input_unavailable"
    assert dict(node.failure_details) == {
        "input_problem": "missing",
        "path": "key",
        "source_kind": "blackboard_key",
        "source_reference": "count",
    }


def test_control_flow_input_failure_names_its_parameter_and_source() -> None:
    program = _typed_program(
        [_key("count", "int64", initial=0), _key("done", "int64", initial=0)],
        _counted("repeat", _leaf("logic.set_value", "mark", key="done", value=1), "count"),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    loop = run._authored_nodes["loop"]  # noqa: SLF001
    assert loop.failure_reason_code == "node_input_unavailable"
    assert dict(loop.failure_details) == {
        "input_problem": "invalid",
        "path": "num_cycles",
        "source_kind": "blackboard_key",
        "source_reference": "count",
    }
    assert run.node_statuses["loop"] == "failure"


def _counted(node_id: str, child: dict[str, Any], source_key: str) -> dict[str, Any]:
    parameter = "num_cycles" if node_id == "repeat" else "num_attempts"
    return {
        "node_id": node_id,
        "instance_id": "loop",
        "parameters": {},
        "value_sources": {parameter: _source("blackboard_key", source_key)},
        "children": [child],
    }


def test_repeat_count_comes_from_its_value_source_once_per_attempt() -> None:
    program = _typed_program(
        [_key("cycles", "int64", initial=3), _key("done", "int64", initial=0)],
        _counted(
            "repeat",
            _leaf("logic.set_value", "mark", {"value": _source("blackboard_key", "cycles")},
                  key="done"),
            "cycles",
        ),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("cycles", 1_000_000)  # The running attempt keeps 3.
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def test_retry_count_comes_from_its_value_source() -> None:
    program = _typed_program(
        [_key("attempts", "int64", initial=3)],
        _counted("retry", _leaf("logic.check_condition", "check", condition=False),
                 "attempts"),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.FAILURE


@pytest.mark.parametrize("node_id", ["repeat", "retry"])
@pytest.mark.parametrize("initial", [None, 0, -2, 1_000_001])
def test_missing_or_invalid_sourced_count_fails_by_name_never_unlimited(
    node_id: str, initial: int | None
) -> None:
    extra = {} if initial is None else {"initial": initial}
    program = _typed_program(
        [_key("count", "int64", **extra), _key("done", "int64", initial=0)],
        _counted(node_id, _leaf("logic.set_value", "mark", key="done", value=1), "count"),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    assert failure.reason_code == "node_input_unavailable"
    assert "count" in failure.message or "num_" in failure.message
    assert failure.origin["kind"] == "program_node"
    assert failure.details["source_kind"] == "blackboard_key"
    # The child never ran: the failed attempt had no count at all.
    assert run.value_store.read("done") == 0


@pytest.mark.parametrize(
    ("policy", "expected"),
    [("one", py_trees.common.Status.SUCCESS), ("all", py_trees.common.Status.RUNNING)],
)
def test_parallel_success_policy_comes_from_its_value_source(
    policy: str, expected: py_trees.common.Status
) -> None:
    program = _typed_program(
        [_key("policy", "string", initial=policy), _key("count", "int64", initial=0)],
        {
            "node_id": "parallel",
            "instance_id": "together",
            "parameters": {},
            "value_sources": {"success_policy": _source("blackboard_key", "policy")},
            "children": [
                _leaf("logic.check_condition", "check", condition=True),
                _leaf("logic.wait_for_value", "wait", key="count", value=1),
            ],
        },
    )
    run = _run(program, program_clock=ProgramClock(lambda: 100.0))
    assert run.tick_once() is expected


@pytest.mark.parametrize("initial", [None, "some"])
def test_parallel_missing_or_invalid_sourced_policy_fails_by_name(
    initial: str | None,
) -> None:
    extra = {} if initial is None else {"initial": initial}
    program = _typed_program(
        [_key("policy", "string", **extra), _key("done", "int64", initial=0)],
        {
            "node_id": "parallel",
            "instance_id": "together",
            "parameters": {},
            "value_sources": {"success_policy": _source("blackboard_key", "policy")},
            "children": [_leaf("logic.set_value", "mark", key="done", value=1)],
        },
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    assert run.terminal_failure().reason_code == "node_input_unavailable"
    assert run.value_store.read("done") == 0


def test_wait_for_value_held_time_policy_is_a_closed_enum() -> None:
    with pytest.raises(ProgramValidationError, match="timeout_while_held"):
        _typed_program(
            [_key("count", "int64")],
            _leaf(
                "logic.wait_for_value",
                "wait",
                key="count",
                value=5,
                timeout_s=1.0,
                timeout_while_held="freeze",
            ),
        )


class _FramePort(_InvocationPort):
    """Stands in for the native cell frame resolver: the arm base sits at
    (1, 0, 0) in the cell, yawed +90 degrees; the table at (0, 0, 0.5)."""

    _IN_CELL = {
        "cell_frame": ([0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 1.0]),
        "arm.base": ([1.0, 0.0, 0.0], [0.0, 0.0, 0.5**0.5, 0.5**0.5]),
        "table": ([0.0, 0.0, 0.5], [0.0, 0.0, 0.0, 1.0]),
    }

    def __init__(self, error: Exception | None = None) -> None:
        super().__init__()
        self.transforms: list[tuple[NodeAttemptIdentity, dict[str, Any], dict[str, Any]]] = []
        self.error = error

    @classmethod
    def _frame(cls, frame: Mapping[str, Any]) -> tuple[list[float], list[float]]:
        if frame["kind"] == "cell_frame":
            return cls._IN_CELL["cell_frame"]
        if frame["kind"] == "scene_resource":
            return cls._IN_CELL[frame["scene_resource_id"]]
        return cls._IN_CELL[f"{frame['target_role']}.{frame['frame_role']}"]

    def express_pose(self, identity, pose, target_frame):
        from manyforge_behavior.geometry import (
            quat_conjugate,
            quat_mul,
            quat_rotate_vector,
            rpy_deg_to_xyzw,
        )

        self.transforms.append((identity, _plain(pose), _plain(target_frame)))
        if self.error is not None:
            raise self.error
        orientation = pose["orientation"]
        quaternion = (
            list(orientation["xyzw"])
            if "xyzw" in orientation
            else rpy_deg_to_xyzw(orientation["rpy_deg"])
        )
        source_t, source_q = self._frame(pose["frame_ref"])
        target_t, target_q = self._frame(target_frame)
        in_cell = [
            a + b
            for a, b in zip(source_t, quat_rotate_vector(source_q, pose["position_m"]))
        ]
        inverse = quat_conjugate(target_q)
        position = quat_rotate_vector(
            inverse, [a - b for a, b in zip(in_cell, target_t)]
        )
        return FrameTransformResult(
            pose={
                "frame_ref": dict(target_frame),
                "position_m": position,
                "orientation": {
                    "xyzw": quat_mul(inverse, quat_mul(source_q, quaternion))
                },
            },
            metadata={"evaluated_at_ns": 7},
        )


def _framed_run(program: Program, frames: _FramePort | None) -> ExecutableProgramRun:
    return ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=NodeAdapterPorts(model_queries=frames),
    )


def _base_pose(position=(0.4, 0.0, 0.2), orientation=None) -> dict[str, Any]:
    return {**_cell_pose(position, orientation), "frame_ref": _portable_pose()["frame_ref"]}


def test_offset_pose_measures_the_offset_along_the_chosen_axes_and_keeps_the_frame() -> None:
    yawed = _cell_pose(orientation={"rpy_deg": [0.0, 0.0, 90.0]})
    offset = {"position_m": [0.1, 0.0, 0.0], "rpy_deg": [0.0, 0.0, 90.0]}
    program = _typed_program(
        [
            _key("along_base", "stored_pose"),
            _key("in_cell", "stored_pose"),
            _key("in_pose", "stored_pose"),
            _key("start", "stored_pose", initial=yawed),
        ],
        _leaf(
            "geometry.offset_pose",
            "local",
            {"pose": _source("blackboard_key", "start")},
            offset=offset,
            output_key="in_pose",
        ),
        _leaf(
            "geometry.offset_pose",
            "world",
            pose=yawed,
            offset=offset,
            offset_frame={"kind": "cell_frame"},
            output_key="in_cell",
        ),
        _leaf(
            "geometry.offset_pose",
            "base",
            pose=yawed,
            offset=offset,
            offset_frame=_portable_pose()["frame_ref"],
            output_key="along_base",
        ),
    )
    frames = _FramePort()
    run = _framed_run(program, frames)
    assert _tick_to_end(run) is py_trees.common.Status.SUCCESS

    local = run.value_store.read("in_pose")
    world = run.value_store.read("in_cell")
    base = run.value_store.read("along_base")
    assert local["frame_ref"] == world["frame_ref"] == base["frame_ref"] == {"kind": "cell_frame"}
    assert local["position_m"] == pytest.approx([0.1, 0.3, 0.3])
    assert world["position_m"] == pytest.approx([0.2, 0.2, 0.3])
    # The base x axis is the cell y axis.
    assert base["position_m"] == pytest.approx([0.1, 0.3, 0.3])
    # The input representation (rpy_deg) is kept; both yaws compose to 180.
    for pose in (local, world, base):
        assert abs(pose["orientation"]["rpy_deg"][2]) == pytest.approx(180.0)
    # Only the foreign axes needed the owner: one transform, of the base axes.
    [(identity, pose, target)] = frames.transforms
    assert identity.node_instance_id == "base"
    assert pose == {
        "frame_ref": _portable_pose()["frame_ref"],
        "position_m": [0.0, 0.0, 0.0],
        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }
    assert target == {"kind": "cell_frame"}


def test_offset_pose_across_frames_fails_by_name_without_a_transform_owner() -> None:
    offset = {"position_m": [0.0, 0.0, 0.1], "rpy_deg": [0.0, 0.0, 0.0]}
    program = _typed_program(
        [_key("out", "stored_pose"), _key("start", "stored_pose", initial=_portable_pose())],
        _leaf(
            "geometry.offset_pose",
            "offset",
            {"pose": _source("blackboard_key", "start")},
            offset=offset,
            offset_frame={"kind": "cell_frame"},
            output_key="out",
        ),
    )
    run = _framed_run(program, None)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    assert _feedback(run).startswith("frame_transform_unavailable:")
    assert not run.value_store.contains("out")
    refused = _FramePort(
        FrameTransformError("program_reference_robot_not_local", "robot is hosted elsewhere")
    )
    run = _framed_run(program, refused)
    assert run.tick_once() is py_trees.common.Status.FAILURE
    assert run._tree.tip().failure_reason_code == "program_reference_robot_not_local"  # noqa: SLF001
    # The pose's own axes need no transform, whatever its frame.
    local = _typed_program(
        [_key("out", "stored_pose")],
        _leaf("geometry.offset_pose", "offset", pose=_portable_pose(), offset=offset, output_key="out"),
    )
    run = _framed_run(local, None)
    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert run.value_store.read("out")["frame_ref"] == _portable_pose()["frame_ref"]
    assert run.value_store.read("out")["position_m"] == pytest.approx([0.4, 0.0, 0.3])


@pytest.mark.parametrize(
    ("position", "inclusive", "inside"),
    [
        ((0.5, 0.5, 0.5), True, True),
        ((1.0, 0.0, 0.5), True, True),
        ((1.0, 0.0, 0.5), False, False),
        ((1.01, 0.5, 0.5), True, False),
    ],
)
def test_check_pose_bounds_is_an_axis_aligned_box_in_one_frame(
    position, inclusive, inside
) -> None:
    program = _typed_program(
        [_key("part", "stored_pose", initial=_cell_pose(position))],
        _leaf(
            "geometry.check_pose_bounds",
            "bounds",
            {"pose": _source("blackboard_key", "part")},
            frame_ref={"kind": "cell_frame"},
            min_position_m=[0.0, 0.0, 0.0],
            max_position_m=[1.0, 1.0, 1.0],
            inclusive=inclusive,
        ),
    )
    expected = py_trees.common.Status.SUCCESS if inside else py_trees.common.Status.FAILURE
    assert _framed_run(program, None).tick_once() is expected


@pytest.mark.parametrize(("base_x", "inside"), [(0.25, True), (0.75, False)])
def test_check_pose_bounds_expresses_the_pose_in_the_box_frame(base_x, inside) -> None:
    # A base-framed point (x, 0, 0.2) is (1, x, 0.2) in the cell.
    program = _typed_program(
        [_key("part", "stored_pose", initial=_base_pose((base_x, 0.0, 0.2)))],
        _leaf(
            "geometry.check_pose_bounds",
            "bounds",
            {"pose": _source("blackboard_key", "part")},
            frame_ref={"kind": "cell_frame"},
            min_position_m=[0.9, 0.0, 0.0],
            max_position_m=[1.1, 0.5, 1.0],
        ),
    )
    frames = _FramePort()
    expected = py_trees.common.Status.SUCCESS if inside else py_trees.common.Status.FAILURE
    assert _framed_run(program, frames).tick_once() is expected
    [(_identity, pose, target)] = frames.transforms
    assert pose["frame_ref"] == _portable_pose()["frame_ref"]
    assert target == {"kind": "cell_frame"}


def test_check_pose_bounds_refuses_an_empty_box_and_an_undeclared_frame_pair() -> None:
    bounds = {
        "frame_ref": {"kind": "cell_frame"},
        "min_position_m": [0.0, 0.0, 0.0],
        "max_position_m": [1.0, 1.0, 1.0],
    }
    with pytest.raises(ProgramValidationError, match="exceeds max_position_m"):
        _typed_program(
            [],
            _leaf(
                "geometry.check_pose_bounds",
                "bounds",
                pose=_cell_pose(),
                **{**bounds, "min_position_m": [0.0, 2.0, 0.0]},
            ),
        )
    with pytest.raises(ProgramValidationError, match="does not name a frame_role"):
        _typed_program(
            [],
            _leaf(
                "geometry.check_pose_bounds",
                "bounds",
                pose=_cell_pose(),
                **{
                    **bounds,
                    "frame_ref": {
                        "kind": "target_frame",
                        "target_role": "arm",
                        "frame_role": "camera",
                    },
                },
            ),
        )


def test_check_pose_distance_expresses_pose_b_in_pose_a_frame() -> None:
    def distance(
        first: dict[str, Any], second: dict[str, Any], frames: _FramePort | None = None
    ) -> ExecutableProgramRun:
        return _framed_run(
            _typed_program(
                [
                    _key("a", "stored_pose", initial=first),
                    _key("b", "stored_pose", initial=second),
                ],
                _leaf(
                    "geometry.check_pose_distance",
                    "near",
                    pose_key_a="a",
                    pose_key_b="b",
                    translation_threshold_m=0.001,
                    rotation_threshold_rad=0.001,
                ),
            ),
            frames,
        )

    yawed = _cell_pose(orientation={"rpy_deg": [0.0, 0.0, 90.0]})
    half = 0.5**0.5
    same = _cell_pose(orientation={"xyzw": [0.0, 0.0, half, half]})
    assert distance(yawed, same).tick_once() is py_trees.common.Status.SUCCESS
    assert distance(yawed, _cell_pose()).tick_once() is py_trees.common.Status.FAILURE
    # The base origin, unrotated in the base, is (1, 0, 0) yawed 90 deg in the cell.
    frames = _FramePort()
    at_base = _cell_pose(position=(1.0, 0.0, 0.0), orientation={"rpy_deg": [0.0, 0.0, 90.0]})
    origin = _base_pose(position=(0.0, 0.0, 0.0), orientation={"rpy_deg": [0.0, 0.0, 0.0]})
    assert distance(at_base, origin, frames).tick_once() is py_trees.common.Status.SUCCESS
    [(_identity, pose, target)] = frames.transforms
    assert pose == origin and target == {"kind": "cell_frame"}
    # Equal coordinates in different frames are different places.
    far = distance(_cell_pose(position=(0.4, 0.0, 0.2)), _base_pose(), _FramePort())
    assert far.tick_once() is py_trees.common.Status.FAILURE


def test_get_object_pose_in_another_frame_is_one_owner_transform() -> None:
    program = _typed_program(
        [_key("part_pose", "stored_pose")],
        _leaf(
            "scene.get_object_pose",
            "read_table",
            scene_resource_id="table",
            frame=_portable_pose()["frame_ref"],
            output_key="part_pose",
        ),
    )
    frames = _FramePort()
    scene = _InvocationPort()
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=NodeAdapterPorts(scene_transactions=scene, model_queries=frames),
    )

    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert scene.invocations == []
    [(_identity, pose, target)] = frames.transforms
    assert pose["frame_ref"] == {"kind": "scene_resource", "scene_resource_id": "table"}
    assert target == _portable_pose()["frame_ref"]
    written = run.value_store.read("part_pose")
    assert written["frame_ref"] == _portable_pose()["frame_ref"]
    # The table (0, 0, 0.5) is (0, 1, 0.5) in the base at (1, 0, 0) yawed +90.
    assert written["position_m"] == pytest.approx([0.0, 1.0, 0.5])
    assert run.value_store.read_bundle("part_pose")["meta"]["evaluated_at_ns"] == 7


def test_get_object_pose_writes_the_scene_port_pose_to_a_pose_key() -> None:
    program = _typed_program(
        [_key("part_pose", "stored_pose")],
        _leaf(
            "scene.get_object_pose",
            "read_table",
            scene_resource_id="table",
            output_key="part_pose",
        ),
    )
    invocation = _InvocationPort()
    invocation.result = OwnerInvocationResult(True, output=_cell_pose())
    run = ExecutableProgramRun(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=_ports(invocation=invocation),
    )

    assert run.tick_once() is py_trees.common.Status.SUCCESS
    assert invocation.invocations[0][0] == "scene.get_object_pose"
    assert invocation.invocations[0][2].parameters["scene_resource_id"] == "table"
    assert run.value_store.read("part_pose") == _cell_pose()


# --- V13.5 expressions, computed keys and value sources by path ------------


def _expression(text: str) -> dict[str, str]:
    return {"kind": "expression", "expression": text}


def _computed(key: str, value_type: str, expression: str) -> dict[str, Any]:
    return _key(key, value_type, access="read", expression=expression)


def _idle() -> dict[str, Any]:
    return _leaf("logic.check_condition", "idle", condition=True)


def test_field_expressions_and_computed_keys_resolve_from_one_snapshot() -> None:
    offset = {"position_m": [0.0, 0.0, 0.1], "rpy_deg": [0.0, 0.0, 0.0]}
    program = _typed_program(
        [
            _key("index", "int64", initial=2),
            _key("out", "stored_pose"),
            _computed("lift", "float64", "key.index * param.pitch"),
            _computed("lift_mm", "float64", "key.lift * 1000"),
        ],
        _leaf(
            "geometry.offset_pose",
            "offset",
            {
                "offset.position_m[2]": _expression("key.lift + 0.01"),
                "pose.position_m[0]": _source("blackboard_key", "lift"),
            },
            pose=_cell_pose(),
            offset=offset,
            output_key="out",
        ),
        parameters=[_parameter("pitch", "float64", 0.05)],
    )
    run = _run(program)
    assert run.value_store.read("lift") == pytest.approx(0.1)
    assert run.value_store.read("lift_mm") == pytest.approx(100.0)
    assert run.value_store.read_bundle("lift")["meta"]["value_source"] == "expression"
    assert not run.value_store.contains("lift")  # computed keys are never stored
    assert _tick_to_end(run) is py_trees.common.Status.SUCCESS
    result = run.value_store.read("out")
    assert result["position_m"] == pytest.approx([0.1, 0.2, 0.3 + 0.11])
    # The literal is untouched in the Program.
    assert program.tree.children[0].parameters["offset"] == offset


@pytest.mark.parametrize(
    ("condition", "status"),
    [
        ({"condition": True}, py_trees.common.Status.SUCCESS),
        ({"condition": False}, py_trees.common.Status.FAILURE),
        ({"value_sources": {"condition": _expression("key.index > 1")}}, py_trees.common.Status.SUCCESS),
        ({"value_sources": {"condition": _expression("!(key.index > 1)")}}, py_trees.common.Status.FAILURE),
    ],
)
def test_check_condition_succeeds_only_while_its_condition_is_true(condition, status) -> None:
    program = _typed_program(
        [_key("index", "int64", initial=2)],
        _leaf("logic.check_condition", "check", **condition),
    )
    assert _tick_to_end(_run(program)) is status


def test_program_parameter_defaults_feed_expressions() -> None:
    program = _typed_program(
        [_computed("lift", "float64", "param.pitch * 2")],
        _idle(),
        parameters=[_parameter("pitch", "float64", 0.05)],
    )
    store = ProgramValueStore.from_program(program)
    assert store.read("lift") == 0.1
    assert dict(store.program_parameters) == {
        parameter_id: declaration.default
        for parameter_id, declaration in program.parameters.items()
    }
    assert store.program_parameters["pitch"] == 0.05
    # There is no override input: the Program default is the only source.
    with pytest.raises(TypeError):
        ProgramValueStore.from_program(program, parameter_values={"pitch": 0.5})


def test_expression_failure_fails_the_node_naming_the_expression_and_cause() -> None:
    program = _typed_program(
        [_key("count", "int64"), _key("out", "int64")],
        _leaf(
            "logic.set_value",
            "set_out",
            {"value": _expression("key.count + 1")},
            key="out",
            value=0,
        ),
    )
    run = _run(program)
    assert _tick_to_end(run) is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    reason, message = failure.reason_code, failure.message
    assert reason == "expression_evaluation_failed"
    assert "'key.count + 1'" in message
    assert "missing_value" in message and "key.count has no value" in message

    # After the input is fixed, a fresh attempt evaluates again.
    retry = _run(program)
    retry.value_store.write("count", 4)
    assert _tick_to_end(retry) is py_trees.common.Status.SUCCESS
    assert retry.value_store.read("out") == 5


def test_a_computed_key_that_cannot_be_evaluated_fails_its_reader() -> None:
    program = _typed_program(
        [
            _key("divisor", "float64", initial=0.0),
            _computed("ratio", "float64", "1 / key.divisor"),
        ],
        _leaf(
            "logic.check_value",
            "check",
            key="ratio",
            operator=">",
            value=0.0,
        ),
    )
    run = _run(program)
    assert _tick_to_end(run) is py_trees.common.Status.FAILURE
    failure = run.terminal_failure()
    reason, message = failure.reason_code, failure.message
    assert reason == "expression_evaluation_failed"
    assert "computed key 'ratio' = '1 / key.divisor'" in message
    assert "division_by_zero" in message


def test_a_failing_computed_dependency_matters_only_when_it_is_read() -> None:
    program = _typed_program(
        [
            _key("divisor", "float64", initial=0.0),
            _computed("bad", "float64", "1.0 / key.divisor"),
            _computed("guarded", "float64", "key.divisor == 0.0 ? 0.0 : key.bad"),
            _computed("eager", "float64", "key.bad + 1.0"),
            _computed("twice", "float64", "key.eager * 2.0"),
        ],
        _idle(),
    )
    store = _run(program).value_store
    # The untaken branch never reads `bad`, so its failure does not matter.
    assert store.read("guarded") == 0.0
    # A reader of `bad`, directly or through another computed key, fails
    # with `bad`'s own cause, not merely "has no value".
    for key in ("eager", "twice"):
        with pytest.raises(ProgramValueError, match="division_by_zero"):
            store.read(key)
    store.write("divisor", 4.0)
    assert store.read("guarded") == 0.25
    assert store.read("twice") == 2.5


@pytest.mark.parametrize(
    ("formula", "status"),
    [
        ("key.divisor == 0.0 ? 0.0 : key.bad", py_trees.common.Status.SUCCESS),
        ("key.bad + 1.0", py_trees.common.Status.FAILURE),
    ],
)
def test_a_node_value_formula_fails_on_a_computed_key_only_when_it_reads_it(
    formula: str, status: py_trees.common.Status
) -> None:
    program = _typed_program(
        [
            _key("divisor", "float64", initial=0.0),
            _computed("bad", "float64", "1.0 / key.divisor"),
            _key("out", "float64"),
        ],
        _leaf(
            "logic.set_value",
            "set_out",
            {"value": _expression(formula)},
            key="out",
            value=1.0,
        ),
    )
    run = _run(program)
    assert _tick_to_end(run) is status
    if status is py_trees.common.Status.SUCCESS:
        # The untaken branch never reads `bad`, so its failure does not matter.
        assert run.value_store.read("out") == 0.0
        return
    failure = run.terminal_failure()
    reason, message = failure.reason_code, failure.message
    assert reason == "expression_evaluation_failed"
    # The reader reports `bad`'s own cause, not merely "has no value".
    assert "key.bad could not be computed" in message
    assert "division_by_zero" in message


def test_wait_for_value_recomputes_a_watched_computed_key_every_tick() -> None:
    program = _typed_program(
        [_key("count", "int64", initial=0), _computed("double", "int64", "key.count * 2")],
        _leaf(
            "logic.wait_for_value",
            "wait",
            key="double",
            operator=">=",
            value=6,
            timeout_s=60.0,
        ),
    )
    run = _run(program)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("count", 2)
    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.value_store.write("count", 3)
    assert run.tick_once() is py_trees.common.Status.SUCCESS


def test_computed_keys_are_never_written() -> None:
    program = _typed_program([_computed("lift", "float64", "1.5")], _idle())
    store = _run(program).value_store
    with pytest.raises(ProgramValueError, match="computed from its expression"):
        store.write("lift", 2.0)


def test_key_writes_and_loads_use_the_full_declared_type_check() -> None:
    program = _typed_program(
        [
            _key("object", "scene_resource"),
            _key("place", "stored_pose"),
            _key("stored", "stored_pose", persistence="persistent"),
        ],
        _idle(),
    )
    store = ProgramValueStore.from_program(
        program, scenario_id="scenario.test", register=_Register()
    )
    with pytest.raises(ProgramValueError, match="does not name a declared scene resource"):
        store.write("object", "not_in_scene")
    with pytest.raises(ProgramValueError, match="does not name a declared target role"):
        store.write(
            "place",
            {
                **_cell_pose(),
                "frame_ref": {"kind": "target_frame", "target_role": "ghost", "frame_role": "tcp"},
            },
        )
    with pytest.raises(ProgramValueError, match="missing required fields"):
        store.write("place", {"position_m": [0, 0, 0], "orientation": {"xyzw": [0, 0, 0, 1]}})
    store.write("place", _portable_pose())
    # A restored value is checked the same way. One the committed Program
    # refuses is dropped with a notice and the run starts without it.
    for fail in (False, True):
        register = _Register({"stored": {**_cell_pose(), "frame_ref": {"kind": "moon"}}})
        register.fail = fail
        restored = ProgramValueStore.from_program(
            program, scenario_id="scenario.test", register=register
        )
        assert not restored.contains("stored")
        ((scenario, key, reason, detail),) = register.drops
        assert (scenario, key, reason) == ("scenario.test", "stored", "value_invalid")
        assert detail is not None and "(persistent_register)" in detail
        # A drop the register could not store still never loads the value.
        assert ("stored" in register.stored) is fail
