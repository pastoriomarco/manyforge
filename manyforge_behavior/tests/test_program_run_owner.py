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
    ProgramRunState,
    build_applied_program_run_admission,
    build_program_run_admission,
)
from manyforge_behavior.program_runtime import ProgramRunOwnerIdentity


_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


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
                "owner_failed"
                if self._initial_state is OwnerTaskState.FAILED
                else None
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
                "authored_failure_policy_parameter": (
                    "no_feasible_plan_policy"
                ),
            },
            reason_code="no_feasible_candidate",
            message="No pick candidate is feasible",
        )


class _OperatorStopTaskPort(_TaskPort):
    def __init__(self, events: list[str]) -> None:
        super().__init__(events, OwnerTaskState.ACTIVE)
        self.observations = 0

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
                    "recovery_class": "ordinary_operator_stop",
                },
            },
            reason_code="operation_stopped",
            message="native operation was stopped",
        )


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
        _reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        self.stop_requests += 1
        self._events.append("scope_stop")
        return self._snapshot(program_run_id)

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
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
        program_run_context=context_port,
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
        maximum_retained_runs=8,
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
        api_version="manyforge.program.v1",
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
    policy = started.capture.node_policy_by_instance_id[
        program.tree.instance_id
    ]

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


def test_program_run_admission_rejects_policy_skills_and_wrong_program_identity() -> None:
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

    started = owner.start(admission)

    assert fixture_events[0] == "context_bind"
    assert len(context_port.contexts) == 1
    context = context_port.contexts[0]
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
        target=lambda: read_results.append(
            owner.read(first.capture.program_run_id)
        )
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

    assert [snapshot.state for snapshot in completed] == [
        ProgramRunState.SUCCEEDED
    ]
    assert task.starts == 1
    assert host.advance_ready_once() == ()
    assert host.read(started.capture.program_run_id).state is (
        ProgramRunState.SUCCEEDED
    )
    host.shutdown()


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

    assert host.advance_ready_once()[0].state is (
        ProgramRunState.INTERVENTION_REQUIRED
    )
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
    assert owner.operator_stop_observation_required(
        started.capture.program_run_id
    )
    owner.refresh_operator_stop_intervention(started.capture.program_run_id)
    assert task.observations == 2
    assert host.advance_ready_once()[0].state is (
        ProgramRunState.INTERVENTION_REQUIRED
    )
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


def test_tick_host_latches_terminal_observer_failure() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )

    def reject(_snapshot) -> None:
        raise RuntimeError("journal unavailable")

    host = ProgramRunTickHost(owner, terminal_observer=reject)
    host.start_run(admission)
    with pytest.raises(ProgramSchedulingError, match="observer failed"):
        host.advance_ready_once()
    with pytest.raises(ProgramSchedulingError, match="worker failed"):
        host.schedule("another-run")
    host.shutdown()


def test_tick_host_cleans_an_admitted_run_if_bounded_scheduling_rejects_it() -> (
    None
):
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


def test_tick_host_rejects_recovery_before_changing_owner_when_queue_is_full() -> (
    None
):
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
        "active_operation_count",
        "reason_code",
        "message",
    }
    assert value["schema_version"] == "manyforge.program_run_status.v1"
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
            "node_policies": [
                policy.to_dict() for policy in admission.node_policies
            ],
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
            expected_scenario_catalog_revision=(
                admission.scenario_catalog_revision
            ),
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
