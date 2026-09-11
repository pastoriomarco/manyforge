"""Exact Program-run admission, lifecycle, and Stop-priority contracts."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from threading import Event, Thread
import time
from typing import Any

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    NodeExecutionReadiness,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAdapterPorts,
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
    ProgramSelectionReleaseRejectedError,
)
from manyforge_behavior.program_scheduling import (
    ProgramInterventionRegistry,
    ProgramRunTickHost,
    ProgramSchedulingError,
)
from manyforge_behavior.program import Program, program_node_policies
from manyforge_behavior.program_run_owner import (
    ProgramRunAdmission,
    ProgramRunOwner,
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
    ProgramRunState,
    build_applied_program_run_admission,
    build_program_run_admission,
)
from manyforge_behavior.program_runtime import ProgramRunOwnerIdentity


_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def test_owner_captures_values_before_later_writes_without_changing_status_wire():
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    document = admission.program.to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "recorded_value",
                "type": "bool",
                "access": "read_write",
                "persistence": "run",
            }
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    admission = _admission_for_program(
        admission, Program.from_dict(document, admission.environment.catalog), []
    )
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    run = owner._runs[run_id].run
    # The existing declared store is the producer, not a later History query.
    owner.write_runtime_blackboard(
        run_id, key="recorded_value", value=True, request_id="write.1"
    )
    for _ in range(4):
        snapshot = owner.advance_once(run_id)
        if snapshot.state is ProgramRunState.SUCCEEDED:
            break
    assert snapshot.state is ProgramRunState.SUCCEEDED
    run.value_store.write("recorded_value", False)
    record = owner.take_diagnostic(run_id)
    assert record["blackboard"] == {"recorded_value": True}
    assert record["program_binding_id"] == admission.program_binding_id
    assert "blackboard" not in snapshot.to_dict()
    assert owner.take_diagnostic(run_id) is None


def _plain(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain(nested) for key, nested in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        return [_plain(nested) for nested in value]
    return value


class _TaskPort:
    def __init__(
        self,
        events: list[str],
        initial_state: OwnerTaskState,
    ) -> None:
        self._events = events
        self._initial_state = initial_state
        self.starts = 0
        self.cancels = 0

    def start(
        self,
        _node_id: str,
        _identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        self._events.append("leaf_start")
        return OwnerTaskSnapshot(
            task_id=f"task-{self.starts}",
            state=self._initial_state,
            reason_code=(
                "owner_failed" if self._initial_state is OwnerTaskState.FAILED else None
            ),
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.ACTIVE)

    def cancel(
        self,
        _task_id: str,
        _identity: NodeAttemptIdentity,
    ) -> None:
        self.cancels += 1
        self._events.append("leaf_cancel")

    def acknowledge(
        self,
        _task_id: str,
        _identity: NodeAttemptIdentity,
    ) -> None:
        self._events.append("leaf_acknowledge")


class _ContextPort:
    def __init__(self, events: list[str]) -> None:
        self.events = events
        self.contexts: list[ProgramRunPortContext] = []
        self.unbound: list[str] = []

    def bind(self, context: ProgramRunPortContext) -> None:
        self.events.append("context_bind")
        self.contexts.append(context)

    def unbind(self, program_run_id: str) -> None:
        self.events.append("context_unbind")
        self.unbound.append(program_run_id)


class _PolicyTaskPort(_TaskPort):
    def start(
        self,
        _node_id: str,
        _identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        self._events.append("leaf_start")
        return OwnerTaskSnapshot(
            task_id=f"task-{self.starts}",
            state=OwnerTaskState.FAILED,
            metadata={
                "authored_failure_policy": "pause_for_operator",
                "authored_failure_policy_parameter": ("no_feasible_plan_policy"),
            },
            reason_code="no_feasible_candidate",
            message="No pick candidate is feasible",
        )


class _OperatorStopTaskPort(_TaskPort):
    recovery_class = "ordinary_operator_stop"

    def __init__(self, events: list[str]) -> None:
        super().__init__(events, OwnerTaskState.ACTIVE)
        self.observations = 0
        self.recoveries: list[tuple[str, str]] = []

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations += 1
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.INTERRUPTED,
            metadata={
                "active_stop_id": "stop.1",
                "active_stop_generation": 1,
                "authored_failure_policy": "pause_for_operator",
                "authored_failure_policy_parameter": "operator_stop_policy",
                "recovery_assessment": {
                    "allowed_actions": ["replan_from_stopped_state"],
                    "recovery_class": self.recovery_class,
                    "stop_id": "stop.1",
                    "stop_generation": 1,
                },
            },
            reason_code="operation_stopped",
            message="native operation was stopped",
        )

    def recover(
        self,
        task_id: str,
        _identity: NodeAttemptIdentity,
        *,
        action: str,
        request_id: str,
        reason: str | None,
    ) -> dict[str, Any]:
        assert action == "replan_from_stopped_state"
        assert reason is None or isinstance(reason, str)
        self.recoveries.append((task_id, request_id))
        return {
            "request_id": request_id,
            "action": action,
            "accepted": True,
            "resulting_assessment": {"status": "resume_ready"},
            "error": None,
        }


class _MalformedOperatorStopTaskPort(_OperatorStopTaskPort):
    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations += 1
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.INTERRUPTED,
            metadata={
                "authored_failure_policy": "pause_for_operator",
                "authored_failure_policy_parameter": "operator_stop_policy",
                "recovery_assessment": {
                    "allowed_actions": ["replan_from_stopped_state"],
                    "recovery_class": "ordinary_operator_stop",
                },
            },
            reason_code="operation_stopped",
            message="native operation omitted its exact Stop identity",
        )


class _StopFaultTaskPort(_OperatorStopTaskPort):
    recovery_class = "stop_fault_intervention"


class _BlockingTaskPort(_TaskPort):
    def __init__(
        self,
        events: list[str],
        entered: Event,
        release: Event,
    ) -> None:
        super().__init__(events, OwnerTaskState.ACTIVE)
        self._entered = entered
        self._release = release

    def start(
        self,
        _node_id: str,
        _identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        self._events.append("leaf_start_enter")
        self._entered.set()
        if not self._release.wait(timeout=2.0):
            raise RuntimeError("test did not release the blocked dispatch")
        self._events.append("leaf_start")
        return OwnerTaskSnapshot(
            task_id=f"task-{self.starts}",
            state=OwnerTaskState.ACTIVE,
        )


class _ParallelTaskPort(_TaskPort):
    def __init__(
        self,
        events: list[str],
        first_entered: Event,
        release_first: Event,
        second_completed: Event,
    ) -> None:
        super().__init__(events, OwnerTaskState.SUCCEEDED)
        self._first_entered = first_entered
        self._release_first = release_first
        self._second_completed = second_completed

    def start(
        self,
        _node_id: str,
        identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        task_id = f"task-{identity.program_run_id}"
        if identity.program_run_id == "program-run-1":
            self._first_entered.set()
            assert self._release_first.wait(timeout=2.0)
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=(
                OwnerTaskState.SUCCEEDED
                if identity.program_run_id == "program-run-1"
                else OwnerTaskState.ACTIVE
            ),
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        if task_id != "task-program-run-2":
            raise AssertionError("unexpected parallel-test task")
        self._second_completed.set()
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.SUCCEEDED,
        )


class _InvocationPort:
    def invoke(
        self,
        _node_id: str,
        _identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerInvocationResult:
        return OwnerInvocationResult(True)


class _ScopePort:
    def __init__(self, events: list[str]) -> None:
        self._events = events
        self.state = ProgramRunScopeState.SETTLED
        self.stop_requests = 0
        self.release_requests = 0
        self.reset_requests = 0
        self.node_count = 0
        self.selection_revision = 0
        self.held = False

    def bind(self, context: ProgramRunPortContext) -> None:
        pending = [context.program["tree"]]
        self.node_count = 0
        while pending:
            node = pending.pop()
            self.node_count += 1
            pending.extend(node.get("children", ()))

    def unbind(self, _program_run_id: str) -> None:
        pass

    def execution_scope(self, program_run_id: str) -> Mapping[str, Any]:
        return {
            "schema_version": "manyforge.program_execution_scope.v1",
            "request_owner_epoch": "kernel.epoch.1",
            "program_runner_owner_epoch": "program-runner.epoch.1",
            "claim_generation": 1,
            "program_run_id": program_run_id,
            "selection_revision": self.selection_revision,
            "held_node_indices": (
                list(range(self.node_count))
                if self.held and self.state is ProgramRunScopeState.SETTLED
                else []
            ),
            "stopping_node_indices": (
                list(range(self.node_count))
                if self.held and self.state is ProgramRunScopeState.ACTIVE
                else []
            ),
            "faulted_node_indices": [],
        }

    def stop_selection(self, request):
        self.last_selection_stop = request
        self.request_stop(
            request["program_run_id"], request["request_context"]["request_id"], None
        )
        if self.state is ProgramRunScopeState.UNKNOWN:
            raise RuntimeError("kernel_scope_unknown")
        return {
            "state": _ScopePort.execution_scope(self, request["program_run_id"]),
            "selection_error": None,
        }

    def release_selection(self, request, expected_selection_revision):
        self.last_selection_release = request
        if self.selection_revision != expected_selection_revision:
            raise ProgramSelectionReleaseRejectedError("selection revision changed")
        if self.state is not ProgramRunScopeState.SETTLED:
            raise ProgramSelectionReleaseRejectedError("selected scope has not settled")
        self.release_stop(request["program_run_id"])
        return self.execution_scope(request["program_run_id"])

    def _snapshot(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        if self.state is ProgramRunScopeState.SETTLED:
            return ProgramRunScopeSnapshot(
                program_run_id,
                self.state,
                0,
            )
        if self.state is ProgramRunScopeState.UNKNOWN:
            return ProgramRunScopeSnapshot(
                program_run_id,
                self.state,
                None,
                "kernel_scope_unknown",
            )
        return ProgramRunScopeSnapshot(program_run_id, self.state, 1)

    def request_stop(
        self,
        program_run_id: str,
        _stop_request_id: str,
        _reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        self.stop_requests += 1
        self.last_stop_request = (program_run_id, _stop_request_id, _reason)
        self.held = True
        self.selection_revision += 1
        self._events.append("scope_stop")
        return self._snapshot(program_run_id)

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return self._snapshot(program_run_id)

    def capture_stop(self, program_run_id):
        return ("stop.1", 1) if self.stop_requests else None

    def release_stop(
        self, program_run_id: str, *, expected_stop=None
    ) -> ProgramRunScopeSnapshot:
        self.release_requests += 1
        self.held = False
        self.selection_revision += 1
        self.last_expected_stop = expected_stop
        self._events.append("scope_stop_release")
        return self._snapshot(program_run_id)

    def reset(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        self.reset_requests += 1
        self._events.append("scope_reset")
        return self._snapshot(program_run_id)


def _readiness(binding_id: str) -> NodeExecutionReadiness:
    resolver = build_executable_node_resolver(build_node_environment())
    return NodeExecutionReadiness(
        program_binding_id=binding_id,
        binding_by_node={
            disposition.row.node_id: AvailabilityFact(True)
            for disposition in resolver.dispositions
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


def _fixture(
    *,
    initial_task_state: OwnerTaskState,
    task_factory: Callable[[list[str]], _TaskPort] | None = None,
    context_port: _ContextPort | None = None,
    interventions: ProgramInterventionRegistry | None = None,
    run_id_factory: Callable[[], str] | None = None,
    maximum_retained_runs: int = 8,
) -> tuple[ProgramRunOwner, ProgramRunAdmission, _TaskPort, _ScopePort, list[str]]:
    events: list[str] = []
    if context_port is not None:
        context_port.events = events
    task = (
        _TaskPort(events, initial_task_state)
        if task_factory is None
        else task_factory(events)
    )
    invocation = _InvocationPort()
    scope = _ScopePort(events)
    ports = NodeAdapterPorts(
        kernel_operations=task,
        scene_transactions=invocation,
        gripper_status=invocation,
        model_queries=invocation,
        assistant_requests=task,
        pick_place=task,
        simulator_control=task,
        kernel_controls=invocation,
        program_run_scope=scope,
        program_run_context=(
            scope
            if context_port is None
            else CompositeProgramRunContextPort(scope, context_port)
        ),
        program_interventions=interventions,
    )
    environment = build_node_environment()
    program = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml"),
    )
    admission = ProgramRunAdmission(
        scenario_id="ur5-pick-place",
        applied_record_digest="sha256:" + "9" * 64,
        scenario_catalog_revision=_DIGEST_A,
        program=program,
        environment=environment,
        program_binding_id=_DIGEST_A,
        retained_bundle_id=_DIGEST_B,
        graph_revision=_DIGEST_C,
        program_binding={
            "program_binding_id": _DIGEST_A,
            "program_id": program.program_id,
            "program_revision": program.program_revision,
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "node_policies": [],
        },
        readiness=_readiness(_DIGEST_A),
        owner_identities=(
            ProgramRunOwnerIdentity(
                owner_role="kernel_operations",
                owner_id="kernel.cell-a",
                owner_epoch="kernel.epoch.1",
            ),
        ),
        program_parameter_values={},
    )
    owner = ProgramRunOwner(
        owner_id="program-runner.cell-a",
        owner_epoch="program-runner.epoch.1",
        ports=ports,
        run_id_factory=run_id_factory or (lambda: "program-run-1"),
        maximum_retained_runs=maximum_retained_runs,
    )
    return owner, admission, task, scope, events


def _program_with_policies(
    admission: ProgramRunAdmission,
    *,
    second_row: bool = False,
    allowed_skill: bool = False,
) -> Program:
    document = admission.program.to_dict()
    document["tree"]["operator_stop_policy"] = "hold"
    if second_row:
        document["tree"]["children"][0]["modification_policy"] = {
            "runtime_override": "human_approval",
            "persistent_edit": "forbidden",
            "intervention_delegate": "human_only",
        }
    if allowed_skill:
        document["tree"]["recovery_policy"] = {
            "allow_intervention": True,
            "allowed_recovery_actions": ["intervention"],
            "max_retries": 1,
            "allowed_skills": ["recover-pick"],
            "persistence_ceiling": "session_only",
        }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, admission.environment.catalog)


def _admission_for_program(
    admission: ProgramRunAdmission,
    program: Program,
    rows: list[dict[str, Any]],
) -> ProgramRunAdmission:
    binding = deepcopy(dict(admission.program_binding))
    binding["program_revision"] = program.program_revision
    binding["node_policies"] = rows
    return replace(admission, program=program, program_binding=binding)


def test_start_captures_the_exact_scenario_closure_before_the_first_tick() -> None:
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )

    started = owner.start(admission)
    assert started.state is ProgramRunState.RUNNING
    assert started.tree_status == "invalid"
    assert started.node_status_revision == 1
    assert set(started.node_statuses.values()) == {"idle"}
    assert task.starts == 0
    assert started.capture.scenario_id == "ur5-pick-place"
    assert started.capture.scenario_catalog_revision == _DIGEST_A
    assert started.capture.program_id == admission.program.program_id
    assert started.capture.program_revision == admission.program.program_revision
    assert started.capture.program_binding_id == _DIGEST_A
    assert started.capture.retained_bundle_id == _DIGEST_B
    assert started.capture.graph_revision == _DIGEST_C
    assert started.capture.node_environment_digest == (
        admission.environment.environment_digest
    )
    assert [identity.owner_role for identity in started.capture.owner_identities] == [
        "kernel_operations",
        "program_runner",
    ]
    assert started.capture.node_policies == ()
    assert dict(started.capture.node_policy_by_instance_id) == {}

    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert completed.node_status_revision == 2
    assert set(completed.node_statuses.values()) == {"success"}
    assert completed.active_operation_count == 0


def test_start_captures_exact_nondefault_policy_and_immutable_lookup() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    program = _program_with_policies(admission)
    source_rows = [policy.to_dict() for policy in program_node_policies(program)]
    admitted = _admission_for_program(admission, program, source_rows)

    source_rows[0]["operator_stop_policy"] = "continue"
    started = owner.start(admitted)
    policy = started.capture.node_policy_by_instance_id[program.tree.instance_id]

    assert policy.to_dict() == {
        "instance_id": program.tree.instance_id,
        "modification_policy": {
            "runtime_override": "forbidden",
            "persistent_edit": "forbidden",
            "intervention_delegate": "forbidden",
        },
        "recovery_policy": {
            "allow_intervention": False,
            "allowed_recovery_actions": [],
            "max_retries": 0,
            "allowed_skills": [],
            "persistence_ceiling": "session_only",
        },
        "operator_stop_policy": "hold",
    }
    assert started.capture.to_dict()["node_policies"] == [policy.to_dict()]
    with pytest.raises(TypeError):
        started.capture.node_policy_by_instance_id[program.tree.instance_id] = policy
    with pytest.raises(TypeError):
        policy.modification_policy["runtime_override"] = "human_approval"
    with pytest.raises(TypeError):
        policy.recovery_policy["max_retries"] = 1


@pytest.mark.parametrize(
    "mutation",
    [
        "missing",
        "stale",
        "default",
        "duplicate",
        "unsorted",
        "reordered",
        "nested",
    ],
)
def test_program_run_admission_rejects_nonexact_policy_vectors(
    mutation: str,
) -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    program = _program_with_policies(admission, second_row=True)
    rows = [policy.to_dict() for policy in program_node_policies(program)]
    binding = deepcopy(dict(admission.program_binding))
    binding["program_revision"] = program.program_revision
    binding["node_policies"] = deepcopy(rows)
    if mutation == "missing":
        del binding["node_policies"]
    elif mutation == "stale":
        binding["node_policies"] = binding["node_policies"][:-1]
    elif mutation == "default":
        binding["node_policies"] = [
            {
                "instance_id": "unused",
                "modification_policy": {
                    "runtime_override": "forbidden",
                    "persistent_edit": "forbidden",
                    "intervention_delegate": "forbidden",
                },
                "recovery_policy": {
                    "allow_intervention": False,
                    "allowed_recovery_actions": [],
                    "max_retries": 0,
                    "allowed_skills": [],
                    "persistence_ceiling": "session_only",
                },
                "operator_stop_policy": "inherit",
            },
            *rows,
        ]
    elif mutation == "duplicate":
        binding["node_policies"].append(deepcopy(rows[-1]))
    elif mutation in {"unsorted", "reordered"}:
        binding["node_policies"].reverse()
    else:
        binding["node_policies"][0]["recovery_policy"]["max_retries"] = 1

    with pytest.raises(ValueError, match="node_policies"):
        replace(admission, program=program, program_binding=binding)


def test_program_run_admission_rejects_policy_skills_and_wrong_program_identity() -> (
    None
):
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    program = _program_with_policies(admission, allowed_skill=True)
    binding = deepcopy(dict(admission.program_binding))
    binding["program_revision"] = program.program_revision
    binding["node_policies"] = []

    with pytest.raises(ValueError, match="revision-qualified retained skill"):
        replace(admission, program=program, program_binding=binding)

    binding["program_revision"] = admission.program.program_revision
    with pytest.raises(ValueError, match="disagree"):
        replace(admission, program=program, program_binding=binding)


def test_owner_exposes_one_bounded_local_run_index_without_downstream_reads() -> None:
    identifiers = iter(("program-run-1", "program-run-2"))
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        run_id_factory=lambda: next(identifiers),
    )
    first = owner.start(admission)
    second = owner.start(admission)

    bounded, truncated = owner.list_snapshots(limit=1)
    active, active_truncated = owner.list_snapshots(active_only=True, limit=8)

    assert [row.capture.program_run_id for row in bounded] == [
        first.capture.program_run_id
    ]
    assert truncated is True
    assert [row.capture.program_run_id for row in active] == [
        first.capture.program_run_id,
        second.capture.program_run_id,
    ]
    assert active_truncated is False

    owner.advance_once(first.capture.program_run_id)
    still_active, _ = owner.list_snapshots(active_only=True, limit=8)
    assert [row.capture.program_run_id for row in still_active] == [
        second.capture.program_run_id
    ]


def test_start_binds_exact_client_context_before_tree_construction() -> None:
    events: list[str] = []
    context_port = _ContextPort(events)
    owner, admission, _task, _scope, fixture_events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context_port,
    )

    started = owner.start(admission, program_run_id="program-cycle.7")

    assert fixture_events[0] == "context_bind"
    assert len(context_port.contexts) == 1
    context = context_port.contexts[0]
    assert started.capture.program_run_id == "program-cycle.7"
    assert context.program_run_id == started.capture.program_run_id
    assert context.program_binding_id == admission.program_binding_id
    assert context.program_binding["program_id"] == admission.program.program_id
    assert _plain(context.program) == admission.program.to_dict()
    assert context.assistant_projection is None


def test_start_never_mutates_scene_after_program_cycle_admission() -> None:
    events: list[str] = []
    context_port = _ContextPort(events)
    owner, admission, task, _scope, fixture_events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context_port,
    )

    started = owner.start(admission)

    assert fixture_events == ["context_bind"]
    assert task.starts == 0
    owner.advance_once(started.capture.program_run_id)
    assert fixture_events == [
        "context_bind",
        "leaf_start",
        "leaf_acknowledge",
        "context_unbind",
    ]


def test_composite_context_rolls_back_partial_bind_in_reverse_order() -> None:
    class _FailingContext(_ContextPort):
        def bind(self, context: ProgramRunPortContext) -> None:
            self.events.append("second_bind")
            raise RuntimeError("second context rejected")

        def unbind(self, program_run_id: str) -> None:
            self.events.append("second_unbind")
            self.unbound.append(program_run_id)

    first = _ContextPort([])
    second = _FailingContext([])
    composite = CompositeProgramRunContextPort(first, second)
    owner, admission, task, _scope, fixture_events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=composite,  # type: ignore[arg-type]
    )
    first.events = fixture_events
    second.events = fixture_events

    with pytest.raises(RuntimeError, match="second context rejected"):
        owner.start(admission)

    assert fixture_events == [
        "context_bind",
        "second_bind",
        "second_unbind",
        "context_unbind",
    ]
    assert task.starts == 0


def test_slow_admission_does_not_block_existing_program_run_reads() -> None:
    entered = Event()
    release = Event()

    class _BlockingSecondContext(_ContextPort):
        def bind(self, context: ProgramRunPortContext) -> None:
            super().bind(context)
            if context.program_run_id == "program-run-2":
                entered.set()
                assert release.wait(timeout=2.0)

    identifiers = iter(("program-run-1", "program-run-2"))
    context_port = _BlockingSecondContext([])
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context_port,
        run_id_factory=lambda: next(identifiers),
    )
    first = owner.start(admission)
    second_results: list[object] = []
    second = Thread(target=lambda: second_results.append(owner.start(admission)))
    second.start()
    assert entered.wait(timeout=1.0)

    read_results: list[object] = []
    reader = Thread(
        target=lambda: read_results.append(owner.read(first.capture.program_run_id))
    )
    reader.start()
    reader.join(timeout=0.2)
    assert not reader.is_alive()
    assert len(read_results) == 1

    release.set()
    second.join(timeout=1.0)
    assert not second.is_alive()
    assert len(second_results) == 1


def test_terminal_cleanup_is_retryable_before_the_run_claims_completion() -> None:
    class _RetryingContext(_ContextPort):
        failures = 1

        def unbind(self, program_run_id: str) -> None:
            super().unbind(program_run_id)
            if self.failures:
                self.failures -= 1
                raise RuntimeError("temporary context release failure")

    events: list[str] = []
    context_port = _RetryingContext(events)
    owner, admission, _task, _scope, _fixture_events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context_port,
    )
    started = owner.start(admission)

    first = owner.advance_once(started.capture.program_run_id)
    assert first.state is ProgramRunState.FINALIZING
    assert first.reason_code == "program_run_cleanup_failed"
    assert context_port.unbound == ["program-run-1"]

    observed = owner.read(started.capture.program_run_id)
    assert observed.state is ProgramRunState.FINALIZING
    assert context_port.unbound == ["program-run-1"]

    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert completed.reason_code is None
    assert context_port.unbound == ["program-run-1", "program-run-1"]


def test_failed_tree_projects_the_exact_focused_owner_failure() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    started = owner.start(admission)

    failed = owner.advance_once(started.capture.program_run_id)

    assert failed.state is ProgramRunState.FAILED
    assert failed.tree_status == "failure"
    assert failed.reason_code == "owner_failed"
    assert failed.message == ""


def test_terminal_run_reset_releases_exact_kernel_scope() -> None:
    owner, admission, _task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    started = owner.start(admission)
    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED

    reset = owner.reset_effects(started.capture.program_run_id)

    assert reset.state is ProgramRunState.SUCCEEDED
    assert scope.reset_requests == 1
    assert events[-1] == "scope_reset"


def test_durable_reset_forgets_resident_tree_and_reuses_concurrent_capacity() -> None:
    sequence = iter(f"program-run-{index}" for index in range(1, 9))
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        run_id_factory=lambda: next(sequence),
        maximum_retained_runs=1,
    )

    for _ in range(8):
        started = owner.start(admission)
        completed = owner.advance_once(started.capture.program_run_id)
        assert completed.state is ProgramRunState.SUCCEEDED
        owner.reset_effects(started.capture.program_run_id)
        owner.forget_reset_run(started.capture.program_run_id)

    assert scope.reset_requests == 8
    assert owner.list_snapshots(limit=8) == ((), False)


def test_active_run_cannot_reset_kernel_scope() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)

    with pytest.raises(ProgramRunOwnerError, match="only after settlement"):
        owner.reset_effects(started.capture.program_run_id)

    assert scope.reset_requests == 0


def test_authored_pause_requires_intervention_without_redispatch() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(
            events,
            OwnerTaskState.FAILED,
        ),
        interventions=interventions,
    )
    started = owner.start(admission)

    paused = owner.advance_once(started.capture.program_run_id)
    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    assert paused.reason_code == "no_feasible_candidate"
    assert task.starts == 1
    assert owner.advance_once(started.capture.program_run_id).state is (
        ProgramRunState.INTERVENTION_REQUIRED
    )
    assert task.starts == 1

    stopped = owner.stop(started.capture.program_run_id)
    assert stopped.state is ProgramRunState.STOPPED
    assert interventions.read(started.capture.program_run_id) is None


def test_tick_host_advances_each_scheduled_run_and_removes_terminal_work() -> None:
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)

    started = host.start_run(admission)
    completed = host.advance_ready_once()

    assert [snapshot.state for snapshot in completed] == [ProgramRunState.SUCCEEDED]
    assert task.starts == 1
    assert host.advance_ready_once() == ()
    assert host.read(started.capture.program_run_id).state is (
        ProgramRunState.SUCCEEDED
    )
    host.shutdown()


@pytest.mark.parametrize(
    "outcome", [ProgramRunScopeState.SETTLED, ProgramRunScopeState.UNKNOWN]
)
def test_terminal_reset_observes_scope_without_reopening_or_ticking_tree(outcome):
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner)
    started = host.start_run(admission)
    run_id = started.capture.program_run_id
    terminal = host.advance_ready_once()[0]
    scope.state = ProgramRunScopeState.ACTIVE
    try:
        assert (
            owner.begin_reset_containment(run_id, stop_request_id="reset.1") == terminal
        )
        assert scope.last_stop_request == (
            run_id,
            "reset.1",
            "operator requested Program cycle Reset",
        )
        host.schedule(run_id)
        assert host.advance_ready_once() == (terminal,)
        assert owner.reset_containment_pending(run_id)
        scope.state = outcome
        assert host.advance_ready_once() == (terminal,)
        assert not owner.reset_containment_pending(run_id)
        assert host.advance_ready_once() == ()
        if outcome is ProgramRunScopeState.SETTLED:
            assert (
                host.wait_terminal(run_id, require_reset_containment=True) == terminal
            )
        else:
            with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
                host.wait_terminal(run_id, require_reset_containment=True)
        assert host.read(run_id) == terminal
        assert task.starts == 1
        assert scope.stop_requests == 1
        assert scope.release_requests == scope.reset_requests == 0
    finally:
        host.shutdown()


def test_terminal_reset_lost_reply_does_not_reuse_previous_settlement():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    terminal = owner.advance_once(run_id)
    owner.begin_reset_containment(run_id, stop_request_id="reset.first")
    assert owner.reset_containment_settled(run_id)

    def lost_reply(*_args, **_kwargs):
        raise RuntimeError("response lost")

    scope.request_stop = lost_reply
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.begin_reset_containment(run_id, stop_request_id="reset.second")
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)
    assert owner.read(run_id) == terminal and task.starts == 1


def test_terminal_reset_waiter_uses_worker_notifications():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    run_id = host.start_run(admission).capture.program_run_id
    terminal = host.advance_ready_once()[0]
    entered, release = Event(), Event()
    read = scope.observe

    def delayed_scope(program_run_id):
        entered.set()
        assert release.wait(2.0)
        scope.state = ProgramRunScopeState.SETTLED
        return read(program_run_id)

    scope.state = ProgramRunScopeState.ACTIVE
    scope.observe = delayed_scope
    results = []
    errors = []

    def reset_wait():
        try:
            results.append(
                host.contain_reset_run(run_id, stop_request_id="reset.1", timeout_s=2)
            )
        except Exception as error:
            errors.append(error)

    host.start()
    waiter = Thread(target=reset_wait)
    waiter.start()
    try:
        assert entered.wait(2.0)
        assert waiter.is_alive() and host.read(run_id) == terminal
        release.set()
        waiter.join(2.0)
        assert not waiter.is_alive()
        assert not errors and results == [terminal]
        assert task.starts == 1 and scope.stop_requests == 1
        host.reset_run(run_id)
        host.forget_reset_run(run_id)
    finally:
        release.set()
        waiter.join(2.0)
        host.shutdown()


def test_first_tree_tick_can_wait_for_running_publication_without_another_worker() -> (
    None
):
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission, defer_ticks=True)
    assert host.advance_ready_once() == ()
    assert task.starts == 0
    host.schedule(started.capture.program_run_id)
    assert host.advance_ready_once()[0].state is ProgramRunState.SUCCEEDED
    assert task.starts == 1
    host.shutdown()


def test_settled_scope_read_failure_retries_on_the_existing_tree_worker() -> None:
    from manyforge_behavior.nodes.stop_policy import (
        OperatorMotionStopSnapshot,
        OperatorMotionStopState,
    )

    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)
    run = owner._entry(started.capture.program_run_id).run
    calls = 0

    def read_scope():
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("temporary projection read failure")
        return OperatorMotionStopSnapshot(1)

    state = OperatorMotionStopState({"scene": ()}, read_scope=read_scope)
    run._operator_stop_state = state
    with pytest.raises(RuntimeError, match="temporary"):
        state.refresh()
    assert not state.snapshot().stopping_nodes
    assert run.pending_selected_observation()
    run.refresh_pending_stop_settlement()
    assert state.observation_error() is None
    assert not run.pending_selected_observation()
    assert calls == 2


def test_tick_host_does_not_schedule_an_ordinary_held_intervention() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(
            events,
            OwnerTaskState.FAILED,
        ),
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)

    assert host.advance_ready_once()[0].state is (ProgramRunState.INTERVENTION_REQUIRED)
    assert host.advance_ready_once() == ()
    assert task.starts == 1
    assert host.read(started.capture.program_run_id).state is (
        ProgramRunState.INTERVENTION_REQUIRED
    )

    host.stop_run(started.capture.program_run_id)
    host.shutdown()


def test_tick_host_keeps_only_operator_stop_intervention_observable() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)

    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    held = host.advance_ready_once()[0]
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert owner.operator_stop_observation_required(started.capture.program_run_id)
    owner.refresh_operator_stop_intervention(started.capture.program_run_id)
    assert task.observations == 2
    assert host.advance_ready_once()[0].state is (ProgramRunState.INTERVENTION_REQUIRED)
    assert task.observations == 2

    intervention = interventions.read(started.capture.program_run_id)
    assert intervention is not None
    host.resume_recovery(
        started.capture.program_run_id,
        node_attempt_id=intervention.identity.node_attempt_id,
        action="retry",
    )
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 1
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 2

    host.stop_run(started.capture.program_run_id)
    host.shutdown()


def test_tick_host_retains_resume_after_a_stale_intervention_worker(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    stale_worker_entered = Event()
    release_stale_worker = Event()
    block_stale_worker = Event()
    advance_once = owner.advance_once

    def controlled_advance(program_run_id: str):
        snapshot = advance_once(program_run_id)
        if (
            block_stale_worker.is_set()
            and snapshot.state is ProgramRunState.INTERVENTION_REQUIRED
        ):
            stale_worker_entered.set()
            assert release_stale_worker.wait(timeout=2.0)
        return snapshot

    monkeypatch.setattr(owner, "advance_once", controlled_advance)
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    # Exercise the wake revision independently from the owner's defensive
    # current-snapshot join: the old worker must not erase Resume even if its
    # completed snapshot remains stale at scheduler ingress.
    monkeypatch.setattr(host, "_current_after_worker", lambda snapshot: snapshot)
    host.start()
    started = host.start_run(admission)
    program_run_id = started.capture.program_run_id
    try:
        deadline = time.monotonic() + 2.0
        while owner.read(program_run_id).state is not (
            ProgramRunState.INTERVENTION_REQUIRED
        ):
            assert time.monotonic() < deadline
            time.sleep(0.001)

        block_stale_worker.set()
        assert stale_worker_entered.wait(timeout=2.0)
        intervention = interventions.read(program_run_id)
        assert intervention is not None
        resumed = host.resume_recovery(
            program_run_id,
            node_attempt_id=intervention.identity.node_attempt_id,
            action="retry",
        )
        assert resumed.state is ProgramRunState.RUNNING

        release_stale_worker.set()
        deadline = time.monotonic() + 2.0
        while task.starts < 2:
            assert time.monotonic() < deadline
            time.sleep(0.001)
        assert owner.read(program_run_id).state is ProgramRunState.RUNNING
    finally:
        release_stale_worker.set()
        host.stop_run(program_run_id)
        host.shutdown()


def test_tick_host_never_loses_repeated_operator_stop_recovery() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    host.start()
    started = host.start_run(admission)
    program_run_id = started.capture.program_run_id

    try:
        for recovery_index in range(64):
            deadline = time.monotonic() + 2.0
            while owner.read(program_run_id).state is not (
                ProgramRunState.INTERVENTION_REQUIRED
            ):
                assert time.monotonic() < deadline
                time.sleep(0.001)

            intervention = interventions.read(program_run_id)
            assert intervention is not None
            owner_result = owner.apply_owner_recovery(
                program_run_id,
                node_attempt_id=intervention.identity.node_attempt_id,
                action="replan_from_stopped_state",
                request_id=f"repeated-recovery.{recovery_index}",
                reason="repeated scheduler qualification",
            )
            assert owner_result["accepted"] is True
            resumed = host.resume_recovery(
                program_run_id,
                node_attempt_id=intervention.identity.node_attempt_id,
                action="retry",
            )
            assert resumed.state is ProgramRunState.RUNNING

            expected_starts = recovery_index + 2
            deadline = time.monotonic() + 2.0
            while task.starts < expected_starts:
                assert time.monotonic() < deadline
                time.sleep(0.001)
    finally:
        host.stop_run(program_run_id)
        host.shutdown()


@pytest.mark.parametrize("groups", ["all", ["arm", "tools", "arm"]])
def test_motion_stop_holds_the_current_node_and_resume_hands_off_its_exact_fence(
    groups,
) -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    if isinstance(groups, list):
        document = admission.program.to_dict()
        document["tree"]["execution_groups"] = ["arm", "tools"]
        document["program_revision"] = compute_content_identity(
            document,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version="manyforge.program.v2",
        )
        admission = _admission_for_program(
            admission, Program.from_dict(document, admission.environment.catalog), []
        )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)

    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    scope.state = ProgramRunScopeState.ACTIVE
    stopped = host.stop_motion(
        started.capture.program_run_id,
        reason="ordinary operator motion Stop",
        group_id=groups,
    )
    assert stopped.state is ProgramRunState.RUNNING
    assert stopped.tree_status == "running"
    assert scope.stop_requests == 1
    expected_groups = [groups] if isinstance(groups, str) else groups
    assert scope.last_selection_stop["group_ids"] == expected_groups

    held = host.advance_ready_once()[0]
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(started.capture.program_run_id)
    assert intervention is not None
    assert intervention.policy_parameter == "operator_stop_policy"

    scope.state = ProgramRunScopeState.SETTLED
    resumed = host.resume_motion(
        started.capture.program_run_id,
        expected_run_revision=owner.read(started.capture.program_run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="resume-motion.1",
        group_id=groups,
        reason=None,
    )
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.release_requests == 1
    assert scope.last_selection_release["group_ids"] == expected_groups
    assert len(task.recoveries) == 1
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 2

    host.stop_run(started.capture.program_run_id)
    host.shutdown()


@pytest.mark.parametrize("new_stop", [False, True])
def test_manual_handover_reuses_cleanup_without_releasing_or_repeating_work(new_stop):
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id)
    owner.advance_once(run_id)
    intervention = interventions.read(run_id)
    assert intervention is not None
    node_index = sorted(admission.program.execution_selections["all"]).index(
        intervention.identity.node_instance_id
    )
    scope.state = ProgramRunScopeState.SETTLED
    recover = task.recover

    def cleanup(*args, **kwargs):
        result = recover(*args, **kwargs)
        if new_stop:
            scope.selection_revision += 1
            scope.state = ProgramRunScopeState.ACTIVE
        return result

    task.recover = cleanup
    request = dict(
        node_indices=(node_index,),
        expected_run_revision=owner.read(run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="manual.handover.1",
    )
    if new_stop:
        with pytest.raises(ProgramRunOwnerError, match="hold changed"):
            owner.prepare_manual_handover(run_id, **request)
    else:
        prepared = owner.prepare_manual_handover(run_id, **request)
        assert prepared.state is ProgramRunState.RUNNING
    assert len(task.recoveries) == 1
    assert scope.release_requests == 0 and scope.held
    owner.advance_once(run_id)
    assert task.starts == 1
    scope.state = ProgramRunScopeState.SETTLED
    owner.advance_once(run_id)
    owner.resume_motion(
        run_id,
        expected_run_revision=owner.read(run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="manual.explicit.resume",
    )
    owner.advance_once(run_id)
    assert scope.release_requests == 1 and task.starts == 2
    assert len(task.recoveries) == 1


@pytest.mark.parametrize(
    "invalid",
    [
        "unknown_node",
        "not_held",
        "stopping",
        "stale_revision",
        "negative",
        "boolean",
        "duplicate",
        "empty",
    ],
)
def test_manual_handover_rejects_inexact_scope_before_cleanup(invalid):
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id)
    owner.advance_once(run_id)
    intervention = interventions.read(run_id)
    assert intervention is not None
    scope.state = ProgramRunScopeState.SETTLED
    node_index = sorted(admission.program.execution_selections["all"]).index(
        intervention.identity.node_instance_id
    )
    indices = (node_index,)
    if invalid == "unknown_node":
        indices = (len(admission.program.execution_selections["all"]),)
    elif invalid == "negative":
        indices = (-1,)
    elif invalid == "boolean":
        indices = (True,)
    elif invalid == "duplicate":
        indices = (node_index, node_index)
    elif invalid == "empty":
        indices = ()
    elif invalid == "not_held":
        scope.held = False
    elif invalid == "stopping":
        scope.state = ProgramRunScopeState.ACTIVE
    with pytest.raises(ProgramRunOwnerError):
        owner.prepare_manual_handover(
            run_id,
            node_indices=indices,
            expected_run_revision=owner.read(run_id).run_revision,
            expected_selection_revision=scope.selection_revision
            - (invalid == "stale_revision"),
            request_id="invalid.manual.handover",
        )
    assert not task.recoveries and scope.release_requests == 0 and task.starts == 1


@pytest.mark.parametrize(
    "failure", ["new_program_stop", "changed_intervention", "release_unavailable"]
)
def test_motion_recovery_cannot_resume_across_a_new_stop_or_failed_handoff(
    failure,
) -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)
    run_id = started.capture.program_run_id
    host.advance_ready_once()
    scope.state = ProgramRunScopeState.ACTIVE
    host.stop_motion(run_id)
    assert host.advance_ready_once()[0].state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(run_id)
    assert intervention is not None
    scope.state = ProgramRunScopeState.SETTLED
    recover = task.recover

    def accept_then_stop(*args, **kwargs):
        result = recover(*args, **kwargs)
        if failure == "new_program_stop":
            scope.state = ProgramRunScopeState.ACTIVE
            owner.stop(run_id, stop_request_id="new-independent-stop")
        elif failure == "changed_intervention":
            interventions.release(run_id)
            interventions.register_run(run_id, [intervention.identity.node_instance_id])
            interventions.require(
                NodeAttemptIdentity(
                    run_id, intervention.identity.node_instance_id, "different-attempt"
                ),
                reason_code=intervention.reason_code,
                message=intervention.message,
                policy_parameter=intervention.policy_parameter,
                owner_port_name=intervention.owner_port_name,
                owner_task_id="different-task",
                owner_assessment=intervention.owner_assessment,
            )
        return result

    task.recover = accept_then_stop

    def unavailable(_request, _revision):
        raise RuntimeError("Stop handoff unavailable")

    if failure == "release_unavailable":
        scope.release_selection = unavailable
    try:
        with pytest.raises(ProgramRunOwnerError):
            host.resume_motion(
                run_id,
                expected_run_revision=owner.read(run_id).run_revision,
                expected_selection_revision=scope.selection_revision,
                request_id="resume.1",
                reason=None,
            )
        assert len(task.recoveries) == 1
        assert scope.release_requests == 0
        if failure == "new_program_stop":
            assert owner.read(run_id).state is not ProgramRunState.RUNNING
        assert owner.operator_stop_scope(run_id).held_nodes
        assert task.starts == 1
    finally:
        scope.state = ProgramRunScopeState.SETTLED
        host.stop_run(run_id)
        host.shutdown()


@pytest.mark.parametrize("program_scope", [True, False])
def test_recovery_preflights_only_its_exact_program_stop(program_scope):
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    scope.state = ProgramRunScopeState.ACTIVE
    if program_scope:
        host.stop_motion(run_id)
    # The task reports a current offer, but the aggregate still has work.
    assert host.advance_ready_once()[0].state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(run_id)
    assert intervention is not None
    if program_scope:
        with pytest.raises(ProgramRunOwnerError, match="still stopping"):
            host.resume_motion(
                run_id,
                expected_run_revision=owner.read(run_id).run_revision,
                expected_selection_revision=scope.selection_revision,
                request_id="too-early",
                reason=None,
            )
        assert not task.recoveries and scope.release_requests == 0
        scope.state = ProgramRunScopeState.SETTLED
    if program_scope:
        assert (
            host.resume_motion(
                run_id,
                expected_run_revision=owner.read(run_id).run_revision,
                expected_selection_revision=scope.selection_revision,
                request_id="settled",
            ).state
            is ProgramRunState.RUNNING
        )
        assert scope.release_requests == 1
    else:
        assert owner.apply_owner_recovery(
            run_id,
            node_attempt_id=intervention.identity.node_attempt_id,
            action="replan_from_stopped_state",
            request_id="settled",
            reason=None,
        )["accepted"]
        assert scope.state is ProgramRunScopeState.ACTIVE
        assert scope.release_requests == 0
    assert len(task.recoveries) == 1
    scope.state = ProgramRunScopeState.SETTLED
    host.stop_run(run_id)
    host.shutdown()


@pytest.mark.parametrize("full", [False, True])
def test_explicit_stop_recaptures_a_lost_reply_through_the_native_scope(full):
    from .test_production_ports import _Transport, _context
    from manyforge_behavior.kernel_client import KernelClient
    from manyforge_behavior.production_ports import NativeKernelProgramPort

    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=ProgramInterventionRegistry(maximum_retained_runs=8),
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    transport = _Transport()
    native = NativeKernelProgramPort(KernelClient(transport))
    native.bind(replace(_context(), program_run_id=run_id))
    owner._scope = native.scope
    original = transport.stop_program_run

    def lost(body):
        original(body)
        raise OSError("native Stop accepted; response lost")

    transport.stop_program_run = lost
    request = owner.full_stop if full else owner.stop
    first = request(run_id, stop_request_id="lost")
    assert first.state is (
        ProgramRunState.FULL_STOPPING if full else ProgramRunState.STOPPING
    )
    assert native.scope.observe(run_id).state is ProgramRunScopeState.UNKNOWN
    owner.advance_once(run_id)  # passive reconciliation must not retry an effect
    assert len(transport.controls) == 1
    transport.stop_program_run = original
    request(run_id, stop_request_id="fresh-recapture")
    assert len(transport.controls) == 2
    transport.stop_states["scope-stop-2"] = "succeeded"
    settled = owner.advance_once(run_id)
    assert settled.state is (
        ProgramRunState.FULL_STOPPED if full else ProgramRunState.STOPPED
    )
    if full:
        owner.stop(run_id, stop_request_id="finish-preserved-run")
        transport.stop_states["scope-stop-3"] = "succeeded"
        assert owner.advance_once(run_id).state is ProgramRunState.STOPPED
    assert native.scope.reset(run_id).state is ProgramRunScopeState.SETTLED
    assert not native._scope_stops


def test_full_stop_overtakes_a_held_full_stop_resume_without_releasing_its_fence():
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    assert (
        host.full_stop(run_id, stop_request_id="first").state
        is ProgramRunState.FULL_STOPPING
    )
    assert host.advance_ready_once()[0].state is ProgramRunState.FULL_STOPPED
    entered, finish, stop_done = Event(), Event(), Event()
    recover, errors, stopped = task.recover, [], []

    def held_recovery(*args, **kwargs):
        entered.set()
        assert finish.wait(timeout=2)
        return recover(*args, **kwargs)

    task.recover = held_recovery

    def resume():
        try:
            host.resume_full_stop(run_id, request_id="older-resume")
        except Exception as error:
            errors.append(error)

    def stop():
        stopped.append(host.full_stop(run_id, stop_request_id="newer-stop"))
        stop_done.set()

    resuming = Thread(target=resume)
    resuming.start()
    assert entered.wait(timeout=2)
    scope.state = ProgramRunScopeState.ACTIVE
    stopping = Thread(target=stop)
    stopping.start()
    try:
        assert stop_done.wait(timeout=0.5)
        assert stopped[0].state is ProgramRunState.FULL_STOPPING
        assert scope.stop_requests == 2
        assert scope.release_requests == 0
    finally:
        finish.set()
        resuming.join(timeout=2)
        stopping.join(timeout=2)
    assert not resuming.is_alive() and not stopping.is_alive()
    assert len(errors) == 1 and isinstance(
        errors[0], ProgramRunOwnerOutcomeUnknownError
    )
    assert scope.release_requests == 0 and task.starts == 1
    scope.state = ProgramRunScopeState.SETTLED
    host.stop_run(run_id)
    host.shutdown()


def test_full_stop_never_waits_for_a_scheduling_scope_read():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=ProgramInterventionRegistry(maximum_retained_runs=8),
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    entered, finish, first_done, second_done = Event(), Event(), Event(), Event()
    read_scope = scope.execution_scope
    results = []

    def held_read(run):
        entered.set()
        assert finish.wait(timeout=2)
        return read_scope(run)

    def stop(request_id, done):
        results.append(owner.full_stop(run_id, stop_request_id=request_id))
        done.set()

    scope.execution_scope = held_read
    first = Thread(target=stop, args=("first", first_done))
    reconcile = Thread(target=owner.reconcile_full_stop, args=(run_id,))
    second = Thread(target=stop, args=("second", second_done))
    first.start()
    try:
        assert first_done.wait(timeout=0.5)
        assert not entered.is_set()  # no inline optional read in the command
        reconcile.start()
        assert entered.wait(timeout=0.5)
        second.start()
        assert second_done.wait(timeout=0.5)
        assert scope.stop_requests == 2
        assert all(row.state is ProgramRunState.FULL_STOPPING for row in results)
    finally:
        finish.set()
        for thread in (first, reconcile, second):
            if thread.ident is not None:
                thread.join(timeout=2)
                assert not thread.is_alive()
    # The older settlement pass cannot complete the newer Stop occurrence.
    assert owner.read(run_id).state is ProgramRunState.FULL_STOPPING
    owner.stop(run_id)


def test_motion_stop_retains_escalated_stop_for_backend_recovery() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_StopFaultTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)

    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    scope.state = ProgramRunScopeState.ACTIVE
    assert host.stop_motion(started.capture.program_run_id).state is (
        ProgramRunState.RUNNING
    )

    held = host.advance_ready_once()[0]
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(started.capture.program_run_id)
    assert intervention is not None
    assert intervention.policy_parameter == "operator_stop_policy"
    assert intervention.owner_assessment is not None
    assert intervention.owner_assessment["recovery_class"] == "stop_fault_intervention"

    host.stop_run(started.capture.program_run_id)
    host.shutdown()


def test_motion_stop_does_not_wait_for_blocked_tree_dispatch() -> None:
    dispatch_entered = Event()
    release_dispatch = Event()
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _BlockingTaskPort(
            events,
            dispatch_entered,
            release_dispatch,
        ),
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert dispatch_entered.wait(timeout=1.0)

    scope.state = ProgramRunScopeState.ACTIVE
    completed = Event()
    result: list[ProgramRunState] = []

    def stop() -> None:
        result.append(
            host.stop_motion(
                started.capture.program_run_id,
                reason="priority Stop while a tree dispatch is blocked",
            ).state
        )
        completed.set()

    stopping = Thread(target=stop)
    stopping.start()
    assert completed.wait(timeout=0.2)
    assert result == [ProgramRunState.RUNNING]
    assert scope.stop_requests == 1

    release_dispatch.set()
    advancing.join(timeout=1.0)
    stopping.join(timeout=1.0)
    assert not advancing.is_alive()
    assert not stopping.is_alive()
    scope.state = ProgramRunScopeState.SETTLED
    host.stop_run(started.capture.program_run_id)
    host.shutdown()


def test_selection_status_never_settles_an_observed_completed_task():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.stop_motion(run_id)
    before = owner.read(run_id)
    task.observe = lambda task_id: OwnerTaskSnapshot(
        task_id=task_id,
        state=OwnerTaskState.SUCCEEDED,
    )
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["resume_allowed"]
    assert "leaf_acknowledge" not in events
    assert owner.read(run_id) == before
    assert task.starts == 1 and task.cancels == 0


def test_late_stop_reply_preserves_a_completed_run_projection():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    stop = scope.stop_selection
    terminal = []

    def complete_before_reply(request):
        accepted = stop(request)
        task.observe = lambda task_id: OwnerTaskSnapshot(
            task_id, OwnerTaskState.SUCCEEDED
        )
        terminal.append(owner.advance_once(run_id))
        return accepted

    scope.stop_selection = complete_before_reply
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError, match="state changed"):
        owner.stop_motion(run_id)
    assert terminal[0].state is ProgramRunState.SUCCEEDED
    assert terminal[0].active_operation_count == 0
    assert owner.read(run_id) == terminal[0]


def test_selection_projection_captures_run_after_waiting_for_tree_transition():
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    entered, release = Event(), Event()
    original_lock = owner._runs[run_id].transition_lock

    class ProjectionBarrier:
        def __enter__(self):
            entered.set()
            assert release.wait(timeout=1)
            original_lock.acquire()

        def __exit__(self, *_args):
            original_lock.release()

    owner._runs[run_id].transition_lock = ProjectionBarrier()
    result = []
    thread = Thread(target=lambda: result.append(owner.read_selection_controls(run_id)))
    thread.start()
    try:
        assert entered.wait(timeout=1)
        owner._runs[run_id].transition_lock = original_lock
        advanced = owner.advance_once(run_id)
    finally:
        release.set()
        thread.join(timeout=2)
    assert not thread.is_alive()
    snapshot, controls = result[0]
    assert snapshot == advanced
    assert controls["groups"][0]["state"] == "running"
    assert task.starts == 1


def test_stop_command_publication_does_not_advance_revision_again_on_unchanged_tick():
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    stopped = owner.stop_motion(run_id)
    assert owner.advance_once(run_id).run_revision == stopped.run_revision
    assert task.starts == 1


@pytest.mark.parametrize("settles_during_preflight", [False, True])
def test_logical_resume_publication_does_not_advance_revision_again_on_tick(
    settles_during_preflight,
):
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    if settles_during_preflight:
        scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id)
    held = owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.SETTLED
    resumed = owner.resume_motion(
        run_id,
        expected_run_revision=held.run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="logical.resume",
    )
    assert owner.advance_once(run_id).run_revision == resumed.run_revision
    assert task.starts == 1


def test_rejected_resume_does_not_publish_unrevisioned_settlement():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id)
    stopping = owner.advance_once(run_id)
    before = owner.operator_stop_scope(run_id)
    assert before.stopping_nodes
    scope.state = ProgramRunScopeState.SETTLED
    with pytest.raises(ProgramRunOwnerError, match="stale selection"):
        owner.resume_motion(
            run_id,
            expected_run_revision=stopping.run_revision,
            expected_selection_revision=scope.selection_revision - 1,
            request_id="stale.selection",
        )
    assert owner.operator_stop_scope(run_id) == before
    assert owner.read(run_id) == stopping
    settled = owner.advance_once(run_id)
    assert settled.run_revision == stopping.run_revision + 1
    assert not owner.operator_stop_scope(run_id).stopping_nodes
    assert owner.advance_once(run_id) == settled
    assert task.starts == 0 and scope.release_requests == 0


def test_resume_rechecks_run_revision_after_waiting_without_owner_effects():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    stopped = owner.stop_motion(run_id)
    entered, release = Event(), Event()
    original_lock = owner._runs[run_id].transition_lock

    class ResumeBarrier:
        def __enter__(self):
            entered.set()
            assert release.wait(timeout=1)
            original_lock.acquire()

        def __exit__(self, *_args):
            original_lock.release()

    owner._runs[run_id].transition_lock = ResumeBarrier()
    errors = []

    def resume():
        try:
            owner.resume_motion(
                run_id,
                expected_run_revision=stopped.run_revision,
                expected_selection_revision=scope.selection_revision,
                request_id="resume.crossed",
            )
        except ProgramRunOwnerError as error:
            errors.append(error)

    thread = Thread(target=resume)
    thread.start()
    try:
        assert entered.wait(timeout=1)
        owner._runs[run_id].transition_lock = original_lock
        changed = owner.note_runtime_control_change(run_id)
    finally:
        release.set()
        thread.join(timeout=2)
    assert not thread.is_alive()
    assert len(errors) == 1
    assert errors[0].code == "program_runtime_control_stale"
    assert errors[0].details == {
        "expected_run_revision": stopped.run_revision,
        "current_run_revision": changed.run_revision,
    }
    assert scope.release_requests == 0 and scope.held
    assert task.starts == 0
    assert owner.read(run_id) == changed


def test_pending_selection_read_failure_does_not_fail_the_tree_or_poll_after_settlement():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id)
    read = scope.execution_scope
    reads = 0

    def observe(run_id):
        nonlocal reads
        reads += 1
        if reads == 1:
            raise RuntimeError("temporary source outage")
        return read(run_id)

    scope.execution_scope = observe
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert (
        owner.execution_selection_controls(run_id)["groups"][0]["state"]
        == "unavailable"
    )
    scope.state = ProgramRunScopeState.SETTLED
    settled = owner.advance_once(run_id)
    assert owner.execution_selection_controls(run_id)["groups"][0]["state"] == "held"
    assert owner.advance_once(run_id).run_revision == settled.run_revision
    assert reads == 2 and scope.stop_requests == 1 and task.starts == 0
    assert "leaf_cancel" not in events


def test_motion_stop_with_zero_local_tasks_waits_for_scope_certificate():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["state"] == "running"
    assert not controls["groups"][0]["resume_allowed"]
    scope.state = ProgramRunScopeState.ACTIVE
    request = scope.request_stop

    def accepted(*args):
        request(*args)
        return ProgramRunScopeSnapshot(run_id, ProgramRunScopeState.ACTIVE, 0)

    scope.request_stop = accepted
    assert owner.stop_motion(run_id).state is ProgramRunState.RUNNING
    assert task.starts == 0
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["state"] == "stopping"
    assert not controls["groups"][0]["resume_allowed"]
    with pytest.raises(ProgramRunOwnerError, match="still stopping"):
        owner.resume_motion(
            run_id,
            request_id="still-settling",
            expected_run_revision=owner.read(run_id).run_revision,
            expected_selection_revision=scope.selection_revision,
        )
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert task.starts == 0
    scope.state = ProgramRunScopeState.SETTLED
    owner.advance_once(run_id)
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["state"] == "held"
    assert controls["groups"][0]["resume_allowed"]
    assert (
        owner.resume_motion(
            run_id,
            request_id="settled",
            expected_run_revision=owner.read(run_id).run_revision,
            expected_selection_revision=scope.selection_revision,
        ).state
        is ProgramRunState.RUNNING
    )
    assert owner.execution_selection_controls(run_id)["groups"][0]["state"] == "running"
    assert task.starts == 0  # Status and logical Resume never create a task.


@pytest.mark.parametrize("failure", ["lost_reply", "unchanged_reply", "new_stop"])
def test_selected_resume_keeps_cleanup_once_across_release_failure(failure):
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    host.stop_motion(run_id)
    host.advance_ready_once()
    revision = scope.selection_revision
    release = scope.release_selection

    def fail_release(request, expected_revision):
        if failure == "lost_reply":
            release(request, expected_revision)
            raise TimeoutError("release response lost")
        if failure == "new_stop":
            scope.request_stop(run_id, "new-stop-before-release", None)
            return release(request, expected_revision)
        return scope.execution_scope(run_id)

    scope.release_selection = fail_release
    try:
        expected_error = (
            ProgramRunOwnerRecoveryIncompleteError
            if failure == "new_stop"
            else ProgramRunOwnerOutcomeUnknownError
        )
        with pytest.raises(expected_error):
            host.resume_motion(
                run_id,
                expected_run_revision=owner.read(run_id).run_revision,
                expected_selection_revision=revision,
                request_id="resume.1",
            )
        assert len(task.recoveries) == 1
        assert interventions.read_all(run_id) == ()
        assert owner.read(run_id).state is ProgramRunState.RUNNING
        host.advance_ready_once()
        assert task.starts == 1  # Unknown release never opens the local view.
        scope.release_selection = release
        if failure != "unchanged_reply":
            with pytest.raises(ProgramRunOwnerError, match="stale selection"):
                host.resume_motion(
                    run_id,
                    expected_run_revision=owner.read(run_id).run_revision,
                    expected_selection_revision=revision,
                    request_id="reconcile",
                )
        if scope.held:
            host.resume_motion(
                run_id,
                expected_run_revision=owner.read(run_id).run_revision,
                expected_selection_revision=scope.selection_revision,
                request_id="resume.current",
            )
        host.advance_ready_once()
        # A stale preflight cannot open scheduling after an unconfirmed
        # release. The Runner requires explicit reconciliation/Reset for that
        # outcome; only a confirmed current release resumes ordinary work.
        assert task.starts == (1 if failure == "lost_reply" else 2)
        if failure == "lost_reply":
            assert owner.operator_stop_scope(run_id).held_nodes
        assert len(task.recoveries) == 1
        assert scope.release_requests == 1
    finally:
        host.stop_run(run_id)
        host.shutdown()


def test_old_scope_settlement_cannot_complete_a_newer_full_stop():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    owner.full_stop(run_id, stop_request_id="first")
    observe = scope.observe

    def crossed(run):
        scope.state = ProgramRunScopeState.SETTLED
        result = observe(run)
        scope.state = ProgramRunScopeState.ACTIVE
        owner.full_stop(run, stop_request_id="newer")
        return result

    scope.observe = crossed
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPING
    scope.observe = observe
    scope.state = ProgramRunScopeState.SETTLED
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED


def test_motion_stop_between_operations_preserves_the_tree_boundary() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)
    program_run_id = started.capture.program_run_id

    stopped = host.stop_motion(
        program_run_id,
        reason="ordinary operator motion Stop before the first operation",
    )

    assert stopped.state is ProgramRunState.RUNNING
    assert scope.stop_requests == 1
    assert task.starts == 0

    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 0

    resumed = host.resume_motion(
        program_run_id,
        expected_run_revision=owner.read(program_run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="run-after-boundary-stop.1",
    )
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.release_requests == 1
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 1

    host.stop_run(program_run_id)
    host.shutdown()


def test_intervention_registry_keeps_only_current_captured_node_attempts() -> None:
    registry = ProgramInterventionRegistry(maximum_retained_runs=1)
    registry.register_run("run", ["arm", "gripper"])
    arm = NodeAttemptIdentity("run", "arm", "arm.1")
    tool = NodeAttemptIdentity("run", "gripper", "tool.1")

    def require(identity):
        return registry.require(
            identity,
            reason_code="operation_stopped",
            message="",
            policy_parameter="operator_stop_policy",
        )

    first, second = require(arm), require(tool)
    assert registry.read_all("run") == (first, second)
    assert require(arm) == first
    with pytest.raises(ProgramSchedulingError, match="another intervention"):
        require(replace(arm, node_attempt_id="arm.2"))
    with pytest.raises(ProgramSchedulingError, match="uncaptured"):
        require(replace(arm, node_instance_id="unknown"))
    registry.resolve(tool, action="retry")
    assert registry.consume_resolution(arm) is None
    with pytest.raises(ProgramSchedulingError, match="another attempt"):
        registry.consume_resolution(replace(tool, node_attempt_id="old.tool"))
    assert registry.consume_resolution(tool) == ("retry", None)
    assert registry.read_all("run") == (first,)
    registry.resolve(arm, action="retry")
    assert registry.consume_resolution(arm) == ("retry", None)
    assert registry.read_all("run") == ()
    # A successor attempt reuses its captured-node slot, not an ever-growing ledger.
    require(replace(arm, node_attempt_id="arm.2"))
    registry.release("run")
    assert registry.read_all("run") == ()
    registry.register_run("next.run", ["arm"])
    with pytest.raises(ProgramSchedulingError, match="uncaptured"):
        require(arm)


def test_intervention_focus_does_not_hide_a_sibling_stop() -> None:
    registry = ProgramInterventionRegistry(maximum_retained_runs=1)
    registry.register_run("run", ["arm", "supervision"])
    arm = NodeAttemptIdentity("run", "arm", "arm.1")
    stop = registry.require(
        arm,
        reason_code="operation_stopped",
        message="",
        policy_parameter="operator_stop_policy",
    )
    ordinary = registry.require(
        NodeAttemptIdentity("run", "supervision", "supervision.1"),
        reason_code="program_node_failed",
        message="",
        policy_parameter="recovery_policy",
    )
    assert registry.read("run") == ordinary
    assert registry.has_operator_stop("run")
    assert registry.read_all("run") == (stop, ordinary)
    registry.resolve(ordinary.identity, action="manual_success")
    assert registry.read("run") == stop
    assert registry.read_all("run") == (stop, ordinary)
    registry.resolve(arm, action="retry")
    assert registry.consume_resolution(arm) == ("retry", None)
    assert not registry.has_operator_stop("run")
    assert registry.read("run") is None
    assert registry.consume_resolution(ordinary.identity) == ("manual_success", None)


def test_parallel_stop_records_both_attempts_before_full_stop_recovery() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    document = admission.program.to_dict()
    child = document["tree"]["children"][0]
    children = [deepcopy(child), deepcopy(child)]
    for index, row in enumerate(children):
        row["instance_id"] = f"parallel.{index}"
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "parallel",
        "parameters": {"success_policy": "all"},
        "children": children,
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    admission = _admission_for_program(admission, program, [])
    run_id = owner.start(admission).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert task.starts == 2
    assert owner.full_stop(run_id).state is ProgramRunState.FULL_STOPPING
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    assert {
        row.identity.node_instance_id for row in interventions.read_all(run_id)
    } == {
        "parallel.0",
        "parallel.1",
    }
    assert (
        owner.resume_full_stop(run_id, request_id="resume-both").state
        is ProgramRunState.RUNNING
    )
    assert len(task.recoveries) == 2
    assert interventions.read_all(run_id) == ()
    owner.advance_once(run_id)
    assert task.starts == 4
    owner.stop(run_id)


@pytest.mark.parametrize("policy", ["inherit", "continue"])
def test_selected_resume_refreshes_siblings_and_retains_partial_cleanup(policy) -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    document = admission.program.to_dict()
    children = [deepcopy(document["tree"]["children"][0]) for _ in range(2)]
    for index, node in enumerate(children):
        node["instance_id"] = f"parallel.{index}"
        if policy != "inherit":
            node["operator_stop_policy"] = policy
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "parallel",
        "parameters": {"success_policy": "all"},
        "children": children,
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    admission = _admission_for_program(
        admission, program, [row.to_dict() for row in program_node_policies(program)]
    )
    observe, recover = task.observe, task.recover
    ready = False
    second_settled = False

    def current_assessment(task_id):
        snapshot = observe(task_id)
        if task_id == "task-2" and not second_settled:
            return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.ACTIVE)
        assessment = {**snapshot.metadata["recovery_assessment"]}
        if not ready:
            assessment["allowed_actions"] = []
        return replace(
            snapshot, metadata={**snapshot.metadata, "recovery_assessment": assessment}
        )

    task.observe = current_assessment
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.stop_motion(run_id)
    first = owner.advance_once(run_id)
    assert len(interventions.read_all(run_id)) == 1
    assert owner.advance_once(run_id).run_revision == first.run_revision
    second_settled = True
    second = owner.advance_once(run_id)
    assert second.run_revision > first.run_revision
    assert second.node_status_revision == first.node_status_revision
    assert len(interventions.read_all(run_id)) == 2
    assert all(
        not item.owner_assessment["allowed_actions"]
        for item in interventions.read_all(run_id)
    )
    assert not owner.execution_selection_controls(run_id)["groups"][0]["resume_allowed"]
    ready = True
    assert owner.execution_selection_controls(run_id)["groups"][0]["resume_allowed"]
    assert not task.recoveries  # A fresh display assessment is not recovery.
    rejected_once = False

    def recover_current(task_id, *args, **kwargs):
        nonlocal rejected_once
        if task_id == "task-2" and not rejected_once:
            rejected_once = True
            return {
                "accepted": False,
                "error": {"message": "temporary second-owner rejection"},
            }
        return recover(task_id, *args, **kwargs)

    task.recover = recover_current
    with pytest.raises(
        ProgramRunOwnerRecoveryIncompleteError, match="second-owner rejection"
    ):
        owner.resume_motion(
            run_id,
            expected_run_revision=owner.read(run_id).run_revision,
            expected_selection_revision=scope.selection_revision,
            request_id="first",
        )
    assert [item[0] for item in task.recoveries] == ["task-1"]
    assert scope.release_requests == 0 and scope.held
    assert (
        owner.resume_motion(
            run_id,
            expected_run_revision=owner.read(run_id).run_revision,
            expected_selection_revision=scope.selection_revision,
            request_id="second",
        ).state
        is ProgramRunState.RUNNING
    )
    assert [item[0] for item in task.recoveries] == ["task-1", "task-2"]
    assert scope.release_requests == 1
    owner.advance_once(run_id)
    assert task.starts == 4
    owner.stop(run_id)


def test_full_stop_preserves_tree_and_run_recovers_a_fresh_attempt() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    context = _ContextPort([])
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        context_port=context,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)
    program_run_id = started.capture.program_run_id

    running = host.advance_ready_once()[0]
    assert running.state is ProgramRunState.RUNNING
    assert running.tree_status == "running"
    assert task.starts == 1

    stopped = host.full_stop(
        program_run_id,
        reason="ordinary operator Full Stop",
        stop_request_id="full-stop.1",
    )

    assert stopped.state is ProgramRunState.FULL_STOPPING
    stopped = host.advance_ready_once()[0]
    assert stopped.state is ProgramRunState.FULL_STOPPED
    assert stopped.tree_status == "running"
    assert scope.stop_requests == 1
    assert task.cancels == 0
    assert context.unbound == []
    assert interventions.read(program_run_id) is not None

    resumed = host.resume_full_stop(
        program_run_id,
        request_id="run-after-full-stop.1",
    )

    assert resumed.state is ProgramRunState.RUNNING
    assert task.starts == 1
    assert len(task.recoveries) == 1
    assert scope.release_requests == 1
    assert interventions.read(program_run_id) is None
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    assert task.starts == 2

    host.stop_run(program_run_id)
    host.shutdown()


def test_full_stop_waits_for_scope_settlement_before_freezing_the_tree() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)
    program_run_id = started.capture.program_run_id
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    scope.state = ProgramRunScopeState.ACTIVE

    stopping = host.full_stop(program_run_id)

    assert stopping.state is ProgramRunState.FULL_STOPPING
    assert task.cancels == 0
    assert host.advance_ready_once()[0].state is ProgramRunState.FULL_STOPPING

    scope.state = ProgramRunScopeState.SETTLED
    stopped = host.advance_ready_once()[0]
    assert stopped.state is ProgramRunState.FULL_STOPPED
    assert task.cancels == 0

    terminal = host.stop_run(program_run_id)
    assert terminal.state is ProgramRunState.STOPPED
    # Physical settlement already belongs to the adopted Full Stop. Program
    # Stop only invalidates/releases the preserved tree; it does not issue a
    # redundant task cancellation.
    assert task.cancels == 0
    host.shutdown()


def test_operator_stop_without_exact_identity_fails_before_held_tree_progress() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_MalformedOperatorStopTaskPort,
        interventions=interventions,
    )
    started = owner.start(admission)

    assert owner.advance_once(started.capture.program_run_id).state is (
        ProgramRunState.RUNNING
    )
    failed = owner.advance_once(started.capture.program_run_id)

    assert failed.state is ProgramRunState.FAILED
    assert failed.reason_code == "owner_stop_identity_missing"
    assert task.starts == 1
    assert interventions.read(started.capture.program_run_id) is None


def test_tick_host_delivers_terminal_without_polling_or_blocking_stop() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    observer_entered = Event()
    release_observer = Event()
    observed: list[ProgramRunState] = []

    def observe(snapshot) -> None:
        observed.append(snapshot.state)
        observer_entered.set()
        assert release_observer.wait(timeout=2.0)

    host = ProgramRunTickHost(owner, terminal_observer=observe)
    started = host.start_run(admission)
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert observer_entered.wait(timeout=1.0)

    stop_returned = Event()

    def stop() -> None:
        host.stop_run(started.capture.program_run_id)
        stop_returned.set()

    stopping = Thread(target=stop)
    stopping.start()
    assert stop_returned.wait(timeout=1.0)
    release_observer.set()
    advancing.join(timeout=1.0)
    stopping.join(timeout=1.0)
    assert observed == [ProgramRunState.SUCCEEDED]
    host.shutdown()


def test_first_intervention_is_published_before_host_becomes_idle():
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(events, OwnerTaskState.FAILED),
        interventions=ProgramInterventionRegistry(maximum_retained_runs=8),
    )
    delivered = Event()
    records = []

    def observe(snapshot):
        if snapshot.state is ProgramRunState.INTERVENTION_REQUIRED:
            records.append(owner.take_diagnostic(snapshot.capture.program_run_id))
            delivered.set()

    host = ProgramRunTickHost(owner, state_observer=observe)
    host.start()
    try:
        host.start_run(admission)
        # No second command, polling read, or unrelated wake can deliver it.
        assert delivered.wait(timeout=2)
        assert len(records) == 1 and records[0]["state"] == "intervention_required"
    finally:
        host.shutdown()


@pytest.mark.parametrize("background", [False, True])
def test_tick_host_latches_terminal_observer_failure(background) -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )

    def reject(_snapshot) -> None:
        raise RuntimeError("journal unavailable")

    delivered, records = Event(), []

    def observe(snapshot):
        record = owner.take_diagnostic(snapshot.capture.program_run_id)
        if record is not None:
            records.append(record)
            delivered.set()

    host = ProgramRunTickHost(owner, terminal_observer=reject, state_observer=observe)
    if background:
        host.start()
    try:
        host.start_run(admission)
        if not background:
            with pytest.raises(ProgramSchedulingError, match="observer failed"):
                host.advance_ready_once()
        assert delivered.wait(timeout=2)
        assert len(records) == 1 and records[0]["state"] == "succeeded"
        with pytest.raises(ProgramSchedulingError, match="worker failed"):
            host.schedule("another-run")
    finally:
        host.shutdown()


def test_tick_host_cleans_an_admitted_run_if_bounded_scheduling_rejects_it() -> None:
    identifiers = iter(("program-run-1", "program-run-2"))
    context = _ContextPort([])
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context,
        run_id_factory=lambda: next(identifiers),
    )
    host = ProgramRunTickHost(owner, maximum_scheduled_runs=1)
    first = host.start_run(admission)

    with pytest.raises(ProgramSchedulingError, match="set is full"):
        host.start_run(admission)

    assert owner.read("program-run-2").state is ProgramRunState.STOPPED
    assert context.unbound == ["program-run-2"]
    assert host.stop_run(first.capture.program_run_id).state is (
        ProgramRunState.STOPPED
    )
    host.shutdown()


def test_tick_host_rejects_recovery_before_changing_owner_when_queue_is_full() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(
            events,
            OwnerTaskState.FAILED,
        ),
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, maximum_scheduled_runs=1)
    started = host.start_run(admission)
    paused = host.advance_ready_once()[0]
    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(started.capture.program_run_id)
    assert intervention is not None
    host.schedule("occupied-run")

    with pytest.raises(ProgramSchedulingError, match="set is full"):
        host.resume_recovery(
            started.capture.program_run_id,
            node_attempt_id=intervention.identity.node_attempt_id,
            action="retry",
        )

    assert owner.read(started.capture.program_run_id).state is (
        ProgramRunState.INTERVENTION_REQUIRED
    )
    assert interventions.read(started.capture.program_run_id) == intervention
    host.shutdown()


def test_tick_host_does_not_let_one_slow_run_starve_another_run() -> None:
    first_entered = Event()
    release_first = Event()
    second_completed = Event()
    identifiers = iter(("program-run-1", "program-run-2"))
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        task_factory=lambda events: _ParallelTaskPort(
            events,
            first_entered,
            release_first,
            second_completed,
        ),
        run_id_factory=lambda: next(identifiers),
    )
    host = ProgramRunTickHost(
        owner,
        tick_interval_s=0.01,
        maximum_workers=2,
    )
    first = host.start_run(admission)
    second = host.start_run(admission)
    host.start()
    try:
        assert first_entered.wait(timeout=1.0)
        assert second_completed.wait(timeout=1.0)
        deadline = time.monotonic() + 1.0
        while (
            host.read(second.capture.program_run_id).state
            is not ProgramRunState.SUCCEEDED
            and time.monotonic() < deadline
        ):
            time.sleep(0.001)
        assert host.read(second.capture.program_run_id).state is (
            ProgramRunState.SUCCEEDED
        )

        release_first.set()
        deadline = time.monotonic() + 1.0
        while (
            host.read(first.capture.program_run_id).state
            is not ProgramRunState.SUCCEEDED
            and time.monotonic() < deadline
        ):
            time.sleep(0.001)
        assert host.read(first.capture.program_run_id).state is (
            ProgramRunState.SUCCEEDED
        )
    finally:
        release_first.set()
        host.shutdown()


def test_stop_reaches_kernel_scope_before_tree_invalidation_and_waits_for_proof() -> (
    None
):
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)
    running = owner.advance_once(started.capture.program_run_id)
    assert running.state is ProgramRunState.RUNNING
    assert events == ["leaf_start"]

    scope.state = ProgramRunScopeState.ACTIVE
    stopping = owner.stop(started.capture.program_run_id, reason="operator stop")
    assert stopping.state is ProgramRunState.STOPPING
    assert stopping.tree_status == "invalid"
    assert set(stopping.node_statuses.values()) == {"idle"}
    assert stopping.active_operation_count == 1
    assert events == ["leaf_start", "scope_stop", "leaf_cancel"]
    assert scope.stop_requests == 1
    assert task.cancels == 1

    still_stopping = owner.read(started.capture.program_run_id)
    assert still_stopping.state is ProgramRunState.STOPPING
    scope.state = ProgramRunScopeState.SETTLED
    still_observed = owner.read(started.capture.program_run_id)
    assert still_observed.state is ProgramRunState.STOPPING
    stopped = owner.advance_once(started.capture.program_run_id)
    assert stopped.state is ProgramRunState.STOPPED
    assert stopped.tree_status == "invalid"
    assert set(stopped.node_statuses.values()) == {"idle"}
    assert stopped.active_operation_count == 0

    repeated = owner.stop(started.capture.program_run_id)
    assert repeated.state is ProgramRunState.STOPPED
    assert scope.stop_requests == 1
    assert task.cancels == 1


def test_stop_cannot_race_a_tick_and_dispatch_after_the_kernel_stop_request() -> None:
    tick_entered = Event()
    release_tick = Event()
    owner, admission, task, _scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda target: _BlockingTaskPort(
            target,
            tick_entered,
            release_tick,
        ),
    )
    started = owner.start(admission)
    program_run_id = started.capture.program_run_id
    tick_results: list[object] = []
    stop_results: list[object] = []
    tick_thread = Thread(
        target=lambda: tick_results.append(owner.advance_once(program_run_id)),
    )
    stop_thread = Thread(
        target=lambda: stop_results.append(owner.stop(program_run_id)),
    )

    tick_thread.start()
    assert tick_entered.wait(timeout=1.0)
    stop_thread.start()
    stop_thread.join(timeout=0.05)
    assert stop_thread.is_alive()
    assert events == ["leaf_start_enter"]

    release_tick.set()
    tick_thread.join(timeout=1.0)
    stop_thread.join(timeout=1.0)
    assert not tick_thread.is_alive()
    assert not stop_thread.is_alive()
    assert len(tick_results) == 1
    assert len(stop_results) == 1
    assert events == ["leaf_start_enter", "leaf_start", "scope_stop", "leaf_cancel"]
    assert task.starts == 1


def test_failure_is_not_relabelled_as_stopped_while_kernel_work_settles() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    started = owner.start(admission)
    scope.state = ProgramRunScopeState.ACTIVE

    stopping = owner.advance_once(started.capture.program_run_id)
    assert stopping.state is ProgramRunState.STOPPING
    assert stopping.reason_code == "program_terminal_scope_not_settled"
    repeated = owner.stop(started.capture.program_run_id)
    assert repeated.state is ProgramRunState.STOPPING

    scope.state = ProgramRunScopeState.SETTLED
    assert owner.read(started.capture.program_run_id).state is (
        ProgramRunState.STOPPING
    )
    failed = owner.advance_once(started.capture.program_run_id)
    assert failed.state is ProgramRunState.FAILED
    assert failed.reason_code == "program_terminal_scope_not_settled"


def test_unknown_kernel_scope_never_reports_stopped_and_recovers_cleanly() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)
    scope.state = ProgramRunScopeState.UNKNOWN

    unknown = owner.stop(started.capture.program_run_id)
    assert unknown.state is ProgramRunState.STOPPING
    assert unknown.active_operation_count is None
    assert unknown.reason_code == "kernel_scope_unknown"

    scope.state = ProgramRunScopeState.SETTLED
    assert owner.read(started.capture.program_run_id).state is (
        ProgramRunState.STOPPING
    )
    stopped = owner.advance_once(started.capture.program_run_id)
    assert stopped.state is ProgramRunState.STOPPED
    assert stopped.active_operation_count == 0
    assert stopped.reason_code is None
    assert stopped.message == ""


def test_retained_terminal_scope_can_reset_without_rebuilding_its_tree() -> None:
    owner, _admission, _task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )

    owner.reset_retained_effects("program-run.retained")

    assert scope.reset_requests == 1
    assert events == ["scope_reset"]


def test_program_run_snapshot_is_a_closed_serializable_owner_projection() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    value: dict[str, Any] = owner.start(admission).to_dict()

    assert set(value) == {
        "schema_version",
        "owner_id",
        "owner_epoch",
        "run_revision",
        "state",
        "capture",
        "tree_status",
        "node_status_revision",
        "node_statuses",
        "active_operation_count",
        "reason_code",
        "message",
    }
    assert value["schema_version"] == "manyforge.program_run_status.v2"
    assert value["node_status_revision"] == 1
    assert value["node_statuses"]
    assert value["capture"]["schema_version"] == ("manyforge.program_run_capture.v1")


def _scenario_selection(admission: ProgramRunAdmission) -> dict[str, Any]:
    program = admission.program
    environment = admission.environment
    return {
        "schema_version": "manyforge.scenario_program_selection.v2",
        "applied_record_digest": admission.applied_record_digest,
        "scenario_id": admission.scenario_id,
        "scenario_catalog_revision": admission.scenario_catalog_revision,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "d" * 64,
        "retained_bundle_id": admission.retained_bundle_id,
        "graph_revision": admission.graph_revision,
        "program_binding_id": admission.program_binding_id,
        "program_id": program.program_id,
        "program_revision": program.program_revision,
        "node_environment_digest": environment.environment_digest,
        "assistant_mode": "forbidden",
        "assistant_projection": None,
        "typed_parameter_overrides": {},
        "owner_identities": [
            {
                "owner_role": "headless_projection",
                "owner_id": "headless-api.cell-a",
                "owner_epoch": "headless.epoch.1",
            }
        ],
        "program": program.to_dict(),
        "program_binding": {
            "program_binding_id": admission.program_binding_id,
            "program_id": program.program_id,
            "program_revision": program.program_revision,
            "retained_bundle_id": admission.retained_bundle_id,
            "graph_revision": admission.graph_revision,
            "node_policies": [policy.to_dict() for policy in admission.node_policies],
            "node_environment": {
                "catalog_id": environment.manifest.catalog_id,
                "content_version": environment.manifest.catalog_content_version,
                "catalog_digest": environment.manifest.catalog_digest,
                "environment_digest": environment.environment_digest,
            },
        },
    }


def test_native_selection_join_rejects_a_catalog_change_before_construction() -> None:
    _owner, fixture_admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    program = fixture_admission.program
    environment = fixture_admission.environment
    selection = _scenario_selection(fixture_admission)

    admitted = build_program_run_admission(
        selection,
        expected_scenario_id=fixture_admission.scenario_id,
        expected_scenario_catalog_revision=(
            fixture_admission.scenario_catalog_revision
        ),
        environment=environment,
        owner_availability=fixture_admission.readiness.owner_by_port,
    )
    assert admitted.program.program_revision == program.program_revision
    assert admitted.program_binding_id == fixture_admission.program_binding_id

    try:
        build_program_run_admission(
            selection,
            expected_scenario_id=fixture_admission.scenario_id,
            expected_scenario_catalog_revision="sha256:" + "e" * 64,
            environment=environment,
            owner_availability=fixture_admission.readiness.owner_by_port,
        )
    except ProgramRunOwnerError as error:
        assert "catalog changed" in str(error)
    else:
        raise AssertionError("stale scenario-catalog revision was admitted")


def test_native_selection_join_accepts_compatible_producer_environment() -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    selection = _scenario_selection(admission)
    producer_environment = "sha256:" + "8" * 64
    selection["node_environment_digest"] = producer_environment
    selection["program_binding"]["node_environment"].update(
        {
            "catalog_digest": "sha256:" + "9" * 64,
            "environment_digest": producer_environment,
        }
    )

    joined = build_program_run_admission(
        selection,
        expected_scenario_id=admission.scenario_id,
        expected_scenario_catalog_revision=admission.scenario_catalog_revision,
        environment=admission.environment,
        owner_availability=admission.readiness.owner_by_port,
    )

    assert joined.environment.environment_digest == (
        admission.environment.environment_digest
    )
    assert joined.program_binding["node_environment"]["environment_digest"] == (
        producer_environment
    )


def test_native_selection_join_rejects_unsupported_catalog_contract() -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    selection = _scenario_selection(admission)
    selection["program_binding"]["node_environment"]["content_version"] = "0.9.0"

    with pytest.raises(ProgramRunOwnerError, match="catalog contract is unsupported"):
        build_program_run_admission(
            selection,
            expected_scenario_id=admission.scenario_id,
            expected_scenario_catalog_revision=(admission.scenario_catalog_revision),
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )


def test_applied_program_join_uses_applied_content_and_fresh_owner_facts() -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    live = _scenario_selection(admission)
    applied = {
        key: value
        for key, value in live.items()
        if key
        not in {
            "applied_record_digest",
            "assistant_projection",
            "owner_identities",
            "schema_version",
        }
    }
    applied.update(
        {
            "schema_version": "manyforge.applied_program_selection.v1",
            "baseline_program_binding_id": admission.program_binding_id,
            "preparation_digest": _DIGEST_C,
            "source": {"record_digest": _DIGEST_B},
        }
    )

    joined = build_applied_program_run_admission(
        applied,
        live,
        expected_scenario_id=admission.scenario_id,
        expected_scenario_catalog_revision=admission.scenario_catalog_revision,
        environment=admission.environment,
        owner_availability=admission.readiness.owner_by_port,
    )

    assert joined.program_binding_id == admission.program_binding_id
    assert joined.owner_identities[0].owner_role == "headless_projection"
    live["graph_revision"] = _DIGEST_A
    try:
        build_applied_program_run_admission(
            applied,
            live,
            expected_scenario_id=admission.scenario_id,
            expected_scenario_catalog_revision=admission.scenario_catalog_revision,
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )
    except ProgramRunOwnerError as error:
        assert "disagree" in str(error)
    else:
        raise AssertionError("stale live owner facts were joined to Applied")
