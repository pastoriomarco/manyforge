"""Exact Program-run admission, lifecycle, and Stop-priority contracts."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from threading import Event, Thread
from types import MappingProxyType, SimpleNamespace
import time
from typing import Any

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.kernel_client import (
    KernelClient,
    KernelOperationAdmissionRejectedError,
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
from manyforge_behavior.production_ports import NativeKernelProgramPort
from manyforge_behavior.program_runtime import ProgramRunOwnerIdentity
from manyforge_program_runner.runtime_controls import ProgramRuntimeControls


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
    # The motion node's system-owned outcome key is captured like any key.
    assert record["blackboard"] == {
        "node_outcome.move_to_pick": "completed",
        "recorded_value": True,
    }
    assert record["program_binding_id"] == admission.program_binding_id
    assert not {"runtime_overrides", "omitted_override_count"} & set(record)
    assert "blackboard" not in snapshot.to_dict()
    assert owner.take_diagnostic(run_id) is None


def _plain(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain(nested) for key, nested in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        return [_plain(nested) for nested in value]
    return value


# A settled, not-eligible assessment for the failed motion attempts these fakes
# model. Without one the owner adapter waits for an assessment to settle.
_SETTLED_FAILURE_METADATA: Mapping[str, Any] = MappingProxyType(
    {
        "recovery_assessment": {
            "status": "action_required",
            "execution_state": "Failed",
            "allowed_actions": [],
            "recovery_class": "failed_validation",
        }
    }
)


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
            metadata=(
                _SETTLED_FAILURE_METADATA
                if self._initial_state is OwnerTaskState.FAILED
                else {}
            ),
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
                **_SETTLED_FAILURE_METADATA,
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
                    "status": "action_required",
                    "execution_state": "Stopped",
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


class _FirstTaskStoppedPort(_OperatorStopTaskPort):
    """Only the first task is stopped; the resumed task keeps running."""

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        if task_id == "task-1":
            return super().observe(task_id)
        self.observations += 1
        return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.ACTIVE)


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
        self.unknown_reason_code = "kernel_scope_unknown"
        self.stop_requests = 0
        self.release_requests = 0
        self.reset_requests = 0
        self.node_count = 0
        self.selection_revision = 0
        self.held = False
        self.ready = False
        self.selection_release_block_reason: str | None = None
        self.attention_refusal: ProgramSelectionReleaseRejectedError | None = None
        # Kernel stand-in for the per-node resource test: a node is refused
        # only while an alarm gates one of its own target resources.
        self.alarmed_nodes: set[str] = set()
        self.attention_checks: list[dict] = []

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
            "schema_version": "manyforge.program_execution_scope.v2",
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
            "ready_node_indices": (
                list(range(self.node_count))
                if self.ready and self.state is ProgramRunScopeState.SETTLED
                else []
            ),
            "stopping_node_indices": (
                list(range(self.node_count))
                if self.held and self.state is ProgramRunScopeState.ACTIVE
                else []
            ),
            "faulted_node_indices": [],
            "selection_release_block_reason": self.selection_release_block_reason,
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

    def check_node_motion_attention(self, request):
        self.attention_checks.append(request)
        if (
            self.attention_refusal is not None
            and request["node_instance_id"] in self.alarmed_nodes
        ):
            raise self.attention_refusal

    def retire_held_preparations(
        self, program_run_id, selected_node_ids, expected_selection_revision,
    ):
        # This stand-in retains no accepted native preparations. Production
        # cleanup is exercised separately through NativeKernelProgramPort.
        return None

    def release_selection(self, request, expected_selection_revision):
        self.last_selection_release = request
        if self.attention_refusal is not None:
            raise self.attention_refusal
        if self.selection_revision != expected_selection_revision:
            raise ProgramSelectionReleaseRejectedError("selection revision changed")
        if self.state is not ProgramRunScopeState.SETTLED:
            raise ProgramSelectionReleaseRejectedError("selected scope has not settled")
        # Run/Resume releases the selection's Ready and Held work alike.
        self.release_requests += 1
        self.held = False
        self.ready = False
        self.selection_revision += 1
        self._events.append("scope_stop_release")
        return self.execution_scope(request["program_run_id"])

    def start_selection(self, request, expected_selection_revision):
        self.last_selection_start = request
        if self.selection_revision != expected_selection_revision:
            raise ProgramSelectionReleaseRejectedError("selection revision changed")
        if not self.ready or self.state is not ProgramRunScopeState.SETTLED:
            raise ProgramSelectionReleaseRejectedError("selected scope is not Ready")
        self.ready = False
        self.selection_revision += 1
        self._events.append("scope_motion_start")
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
                self.unknown_reason_code,
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
        # Like the Kernel, a Stop contains started work; never-started
        # (Ready) work keeps its Ready hold.
        if not self.ready:
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
        # Releasing the whole-run Stop resumes execution only; like the
        # Kernel, it keeps every motion hold (held and Ready stay).
        self.release_requests += 1
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
    program_clock_source: Callable[[], float] = time.monotonic,
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
        project_path="/projects/ur5-pick-place.scenario",
        applied_record_digest="sha256:" + "9" * 64,
        source_document_digest=_DIGEST_A,
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
    )
    owner = ProgramRunOwner(
        owner_id="program-runner.cell-a",
        owner_epoch="program-runner.epoch.1",
        ports=ports,
        run_id_factory=run_id_factory or (lambda: "program-run-1"),
        maximum_retained_runs=maximum_retained_runs,
        program_clock_source=program_clock_source,
    )
    return owner, admission, task, scope, events


def _program_with_policies(
    admission: ProgramRunAdmission,
    *,
    second_row: bool = False,
) -> Program:
    document = admission.program.to_dict()
    document["tree"]["operator_stop_policy"] = "hold"
    if second_row:
        document["tree"]["children"][0]["recovery_policy"] = {
            "allow_intervention": True,
            "max_retries": 0,
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
    assert started.active_operation_count is None
    assert started.to_dict()["active_operation_count"] is None
    assert started.tree_status == "invalid"
    assert started.node_status_revision == 1
    assert set(started.node_statuses.values()) == {"idle"}
    assert task.starts == 0
    assert started.capture.scenario_id == "ur5-pick-place"
    assert started.capture.source_document_digest == _DIGEST_A
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
        "recovery_policy": {"allow_intervention": False, "max_retries": 0},
        "operator_stop_policy": "hold",
    }
    assert started.capture.to_dict()["node_policies"] == [policy.to_dict()]
    with pytest.raises(TypeError):
        started.capture.node_policy_by_instance_id[program.tree.instance_id] = policy
    with pytest.raises(TypeError):
        policy.recovery_policy["allow_intervention"] = True
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
                "recovery_policy": {"allow_intervention": False, "max_retries": 0},
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


def test_program_run_admission_rejects_removed_policy_rows_and_wrong_identity() -> (
    None
):
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    program = _program_with_policies(admission, second_row=True)
    rows = [policy.to_dict() for policy in program_node_policies(program)]
    binding = deepcopy(dict(admission.program_binding))
    binding["program_revision"] = program.program_revision
    binding["node_policies"] = deepcopy(rows)
    for row in binding["node_policies"]:
        row["modification_policy"] = {
            "runtime_override": "forbidden",
            "persistent_edit": "forbidden",
            "intervention_delegate": "forbidden",
        }

    with pytest.raises(ValueError, match="node_policies"):
        replace(admission, program=program, program_binding=binding)

    binding["node_policies"] = deepcopy(rows)

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
    assert paused.active_operation_count is None
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


def test_terminal_reset_partial_stop_accepts_only_current_zero_sample():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    run_id = owner.start(admission).capture.program_run_id
    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.FAILED
    assert terminal.active_operation_count == 0
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_partial"

    owner.begin_reset_containment(run_id, stop_request_id="reset.partial")
    assert (
        owner._entry(run_id).reset_containment_pre_stop_active_operation_count == 0
    )
    assert owner.reset_containment_settled(run_id)
    owner.begin_reset_containment(run_id, stop_request_id="reset.retry")
    assert (
        owner._entry(run_id).reset_containment_pre_stop_active_operation_count == 0
    )
    assert owner.reset_containment_settled(run_id)
    assert scope.stop_requests == 2


def test_program_run_owner_and_native_port_reset_partial_stop_with_intervention_operation():
    class ResetTransport:
        def __init__(self) -> None:
            self.owner_epoch = "kernel.epoch.7"
            self.runner_epoch = "program-runner.epoch.1"
            self.operations: dict[str, dict[str, Any]] = {}
            self.resident: dict[str, bool] = {}
            self.stop_states: dict[str, str] = {}
            self.stop_generations: dict[str, int] = {}
            self.controls: list[tuple[str, str]] = []
            self.released_fences: list[tuple[str, str, int]] = []

        def identity(self):
            return {
                "ok": True,
                "identity": {
                    "owner_epoch": self.owner_epoch,
                    "retained_bundle_id": _DIGEST_B,
                    "graph_revision": _DIGEST_C,
                    "scene_revision": {
                        "scene_id": "scene.1",
                        "revision_id": 1,
                        "content_hash": _DIGEST_A,
                    },
                },
            }

        def submit(self, body):
            operation_id = f"operation-{len(self.operations) + 1}"
            self.operations[operation_id] = {
                "operation_id": operation_id,
                "operation_revision": 1,
                "program_run_id": body["program_run_id"],
                "state": "queued",
                "active_stop_generation": None,
                "participants": [],
            }
            self.resident[operation_id] = True
            return {
                "ok": True,
                "accepted": {
                    "operation_id": operation_id,
                    "operation_revision": 1,
                },
            }

        def read(self, operation_id):
            return {
                "ok": True,
                "operation": deepcopy(self.operations[operation_id]),
                "resident": self.resident[operation_id],
                "owner_epoch": self.owner_epoch,
                "control_pending": False,
                "last_control": None,
            }

        def reset(self, operation_id, _body):
            self.controls.append(("reset", operation_id))
            self.operations[operation_id]["operation_revision"] += 1
            self.resident[operation_id] = False
            return {"ok": True, "accepted": {"operation_id": operation_id}}

        def acknowledge_completed(self, _operation_id, _body):
            return {"ok": True}

        def stop(self, _body):
            return {"ok": True}

        def stop_program_run(self, body):
            stop_id = f"scope-stop-{len(self.stop_states) + 1}"
            generation = len(self.stop_generations) + 1
            self.stop_states[stop_id] = "stopping"
            self.stop_generations[stop_id] = generation
            return {
                "ok": True,
                "accepted": {
                    "request_id": body["request_context"]["request_id"],
                    "stop_id": stop_id,
                    "stop_generation": generation,
                    "replay_status": "accepted_new",
                },
                "owner_epoch": self.owner_epoch,
            }

        def stop_status(self, stop_id):
            return {
                "ok": True,
                "stop_status": {
                    "stop_id": stop_id,
                    "state": self.stop_states[stop_id],
                    "stop_generation": self.stop_generations[stop_id],
                },
            }

        def stop_fault(self):
            return {
                "ok": True,
                "command": "get_stop_fault",
                "stop_fault": None,
            }

        def read_program_execution_scope(self, program_run_id):
            return {
                "ok": True,
                "scope": {
                    "schema_version": "manyforge.program_execution_scope.v2",
                    "program_run_id": program_run_id,
                    "request_owner_epoch": self.owner_epoch,
                    "program_runner_owner_epoch": self.runner_epoch,
                    "claim_generation": 1,
                    "selection_revision": 0,
                    "held_node_indices": [],
                    "ready_node_indices": [],
                    "stopping_node_indices": [],
                    "faulted_node_indices": [],
                },
            }

        def release_program_run_stop_fence(self, program_run_id, stop_id, generation):
            self.released_fences.append((program_run_id, stop_id, generation))
            return {"ok": True}

    transport = ResetTransport()
    native = NativeKernelProgramPort(KernelClient(transport))
    fixture_owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    binding = deepcopy(dict(admission.program_binding))
    binding["target_role_bindings"] = [
        {
            "target_role_id": "arm",
            "component_role_id": "robot",
            "controllable_component_id": "robot.1",
            "command_target_id": "arm.command",
            "capability_bindings": [
                {"capability_id": "execution.trajectory.execute", "command_schema": None},
                {"capability_id": "model.kinematics.fk", "command_schema": None},
                {"capability_id": "state.observed.current", "command_schema": None},
            ],
        }
    ]
    admission = replace(
        admission,
        program_binding=binding,
        owner_identities=(
            ProgramRunOwnerIdentity(
                owner_role="kernel_operations",
                owner_id="kernel.cell-a",
                owner_epoch=transport.owner_epoch,
            ),
        ),
    )
    owner = ProgramRunOwner(
        owner_id="program-runner.cell-a",
        owner_epoch=transport.runner_epoch,
        ports=replace(
            fixture_owner._ports,
            program_run_scope=native.scope,
            program_run_context=native,
        ),
        run_id_factory=lambda: "program-run.1",
    )

    run_id = owner.start(admission).capture.program_run_id
    native_task = native.operations.start(
        "motion.move_to_pose",
        NodeAttemptIdentity(run_id, "held-motion", "attempt.1"),
        OwnerNodeInputs(
            {
                "target_role": "arm",
                "base_frame_role": "base",
                "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"},
                "goal": {
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation": {"rpy_deg": [180.0, 0.0, 0.0]},
                },
                "target_mode": "relative",
                "target_reference": {
                    "type": "scene_resource",
                    "scene_resource_id": "graspable",
                },
                "offset_reference": {"type": "frame_role", "frame_role_id": "base"},
                "planning_profile": "balanced",
                "velocity_scale": 0.4,
                "acceleration_scale": 0.4,
                "max_tcp_linear_speed_mps": None,
                "per_joint_dynamics_limits": {},
                "joint_position_tol_rad": 0.05,
                "translation_tol_m": 0.005,
                "rotation_tol_rad": 0.05,
                "planning_timeout_s": 2.0,
                "execution_timeout_s": 10.0,
                "cache_mode": "default",
                "contact_policy_id": None,
            }
        ),
    )
    operation_id = native_task.metadata["operation_id"]
    transport.operations[operation_id]["state"] = "intervention_required"
    assert native.operations.observe(native_task.task_id).state is OwnerTaskState.FAILED

    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.FAILED
    assert terminal.active_operation_count == 0

    owner.begin_reset_containment(run_id, stop_request_id="reset.partial")
    transport.stop_states["scope-stop-1"] = "partial"
    owner._observe_reset_containment(run_id)
    assert owner._entry(run_id).reset_containment.reason_code == "program_run_stop_partial"
    assert owner.reset_containment_settled(run_id)

    reset = owner.reset_effects(run_id)
    assert reset.state is ProgramRunState.FAILED
    assert transport.operations[operation_id]["state"] == "intervention_required"
    assert transport.resident[operation_id] is False
    assert transport.controls == [("reset", operation_id)]
    assert native.scope.capture_stop(run_id) is None
    assert transport.released_fences == []
    owner.forget_reset_run(run_id)


@pytest.mark.parametrize("pre_stop_count", [None, 1])
def test_terminal_reset_partial_stop_rejects_missing_or_nonzero_sample(pre_stop_count):
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.SUCCEEDED
    assert terminal.active_operation_count == 0
    with owner._lock:
        owner._entry(run_id).terminal_settlement_active_operation_count = pre_stop_count
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_partial"

    owner.begin_reset_containment(run_id, stop_request_id="reset.partial")
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)


@pytest.mark.parametrize(
    ("scope_state", "reason_code"),
    [
        (ProgramRunScopeState.ACTIVE, None),
        (ProgramRunScopeState.UNKNOWN, "program_run_stop_failed"),
        (ProgramRunScopeState.UNKNOWN, "program_run_stop_uncontained"),
        (ProgramRunScopeState.UNKNOWN, "program_run_stop_generation_mismatch"),
    ],
)
def test_terminal_reset_does_not_treat_pending_or_other_unknown_as_partial(
    scope_state, reason_code
):
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = scope_state
    if reason_code is not None:
        scope.unknown_reason_code = reason_code

    owner.begin_reset_containment(run_id, stop_request_id="reset.unproven")
    if scope_state is ProgramRunScopeState.ACTIVE:
        assert owner.reset_containment_pending(run_id)
        assert not owner.reset_containment_settled(run_id)
    else:
        with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
            owner.reset_containment_settled(run_id)


def test_terminal_reset_partial_lost_reply_cannot_reuse_partial_proof():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_partial"
    owner.begin_reset_containment(run_id, stop_request_id="reset.partial")
    assert owner.reset_containment_settled(run_id)

    def lost_reply(*_args, **_kwargs):
        raise RuntimeError("response lost")

    scope.request_stop = lost_reply
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.begin_reset_containment(run_id, stop_request_id="reset.retry")
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)


def test_reset_containment_does_not_change_a_running_program():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)

    with pytest.raises(ProgramRunOwnerError, match="requires a terminal Run"):
        owner.begin_reset_containment(
            started.capture.program_run_id, stop_request_id="reset.running"
        )

    assert scope.stop_requests == 0


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


def test_failure_pause_stops_all_program_motion_once() -> None:
    """A node failure that pauses for the operator stops the Program's other
    motion, like Halt, exactly once."""
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(events, OwnerTaskState.FAILED),
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)

    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    assert scope.stop_requests == 1
    assert scope.last_selection_stop["group_ids"] == ["all"]
    assert scope.last_selection_stop["request_context"]["request_id"].startswith(
        "failure-pause-stop:"
    )
    assert not owner.failure_pause_stop_pending(run_id)
    owner.advance_once(run_id)
    assert scope.stop_requests == 1
    owner.stop(run_id)


class _RetryablePolicyTaskPort(_PolicyTaskPort):
    """Fails with a failure pause; records owner recovery of the failed step."""

    def __init__(self, events: list[str], scope_probe: Callable[[], bool]) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self.recoveries: list[tuple[str, str, bool]] = []
        self.accept_recovery = True
        self._held = scope_probe
        self.on_recover: Callable[[], None] | None = None

    def recover(self, task_id, _identity, *, action, request_id, reason):
        # Records whether motion was still held when recovery was prepared.
        self.recoveries.append((task_id, action, self._held()))
        if self.on_recover is not None:
            self.on_recover()
        return {
            "request_id": request_id,
            "action": action,
            "accepted": self.accept_recovery,
            "error": None if self.accept_recovery else {"message": "owner refused"},
        }


def _failure_paused_run():
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    holder: dict[str, Any] = {}
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _RetryablePolicyTaskPort(
            events, lambda: holder["scope"].held
        ),
        interventions=interventions,
    )
    holder["scope"] = scope
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)
    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    failure = interventions.read(run_id)
    assert failure is not None
    return owner, task, scope, interventions, run_id, failure


class _ObservationBoundFailureTaskPort(_PolicyTaskPort):
    """A failure whose owner Retry readiness carries an observation deadline."""

    def __init__(self, events: list[str], *, deadline_ns: int) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self.deadline_ns = deadline_ns
        self.observations = 0

    def _assessment(self) -> dict[str, Any]:
        return {
            "allowed_actions": ["retry_after_scene_repair", "reset_cycle"],
            "observed_state_valid_until_ns": self.deadline_ns,
            "recovery_class": "failed_validation",
            "execution_state": "Failed",
            "retry_admissible": True,
            "status": "retry_ready",
        }

    def start(self, node_id, identity, inputs) -> OwnerTaskSnapshot:
        started = super().start(node_id, identity, inputs)
        return replace(
            started,
            metadata={**started.metadata, "recovery_assessment": self._assessment()},
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations += 1
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.FAILED,
            metadata={"recovery_assessment": self._assessment()},
            reason_code="no_feasible_candidate",
        )


@pytest.mark.parametrize("deadline_ns", [0, 1_000])
def test_failure_pause_keeps_owner_observation_deadline_current(deadline_ns) -> None:
    """A dormant failure pause re-reads its owner assessment only while that
    assessment is bounded by an observation deadline, so a Retry offer follows
    the owner's freshness instead of a deadline captured at failure time."""
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _ObservationBoundFailureTaskPort(
            events, deadline_ns=deadline_ns
        ),
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.INTERVENTION_REQUIRED
    failure = interventions.read(run_id)
    assert failure.owner_assessment["observed_state_valid_until_ns"] == deadline_ns
    if deadline_ns == 0:
        # No owner deadline: the pause stays dormant without owner reads.
        assert not owner.failure_assessment_refresh_required(run_id)
        owner.advance_once(run_id)
        assert task.observations == 0
        owner.stop(run_id)
        return

    assert owner.failure_assessment_refresh_required(run_id)
    task.deadline_ns = 2_000
    held = owner.advance_once(run_id)
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert task.observations == 1
    assert task.starts == 1  # the tree stayed dormant
    refreshed = interventions.read(run_id)
    assert refreshed.identity == failure.identity
    assert refreshed.owner_assessment["observed_state_valid_until_ns"] == 2_000
    owner.stop(run_id)


def test_failure_retry_recovers_while_held_then_releases_and_restarts() -> None:
    owner, task, scope, interventions, run_id, failure = _failure_paused_run()
    assert scope.held  # the failure pause stopped all Program motion
    releases_before = scope.release_requests

    owner.retry_failure_pause(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action="retry_motion_from_current_state",
        request_id="retry.1",
        reason=None,
    )
    # Recovery was prepared while held; the release came after it.
    assert task.recoveries == [("task-1", "retry_motion_from_current_state", True)]
    assert scope.release_requests == releases_before + 1
    assert not scope.held

    resumed = owner.resolve_intervention(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action="retry",
        result=None,
    )
    assert resumed.state is ProgramRunState.RUNNING
    owner.advance_once(run_id)  # consumes the Retry decision
    owner.advance_once(run_id)  # starts the fresh attempt
    assert task.starts == 2
    owner.stop(run_id)


def test_stop_during_failure_retry_wins_and_keeps_motion_held() -> None:
    owner, task, scope, _interventions, run_id, failure = _failure_paused_run()
    # A newer Stop lands while Retry prepares recovery.
    task.on_recover = lambda: setattr(scope, "selection_revision", scope.selection_revision + 1)

    with pytest.raises(ProgramRunOwnerRecoveryIncompleteError):
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id,
            action="retry_motion_from_current_state",
            request_id="retry.stale",
            reason=None,
        )
    assert scope.held
    assert owner.read(run_id).state is ProgramRunState.INTERVENTION_REQUIRED
    owner.stop(run_id)


def test_rejected_failure_recovery_releases_nothing() -> None:
    owner, task, scope, _interventions, run_id, failure = _failure_paused_run()
    task.accept_recovery = False
    releases_before = scope.release_requests

    with pytest.raises(ProgramRunOwnerError, match="owner refused"):
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id,
            action="retry_motion_from_current_state",
            request_id="retry.rejected",
            reason=None,
        )
    assert scope.release_requests == releases_before
    assert scope.held
    owner.stop(run_id)


def test_unconfirmed_failure_containment_is_visible_and_blocks_retry() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _PolicyTaskPort(events, OwnerTaskState.FAILED),
        interventions=interventions,
    )
    scope.state = ProgramRunScopeState.UNKNOWN  # the containment Stop fails
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)

    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    assert "No pick candidate is feasible" in paused.message
    assert "Stopping the Program's other motion is not confirmed yet." in paused.message
    assert owner.failure_pause_stop_pending(run_id)
    failure = interventions.read(run_id)
    with pytest.raises(ProgramRunOwnerError, match="not confirmed"):
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id,
            action=None,
            request_id="retry.unconfirmed",
            reason=None,
        )

    scope.state = ProgramRunScopeState.SETTLED  # the retried Stop succeeds
    confirmed = owner.advance_once(run_id)
    assert "not confirmed" not in confirmed.message
    assert not owner.failure_pause_stop_pending(run_id)
    owner.stop(run_id)


def test_ordinary_stop_intervention_sends_no_extra_failure_pause_stop() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    held = owner.advance_once(run_id)
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert scope.stop_requests == 0
    assert not owner.failure_pause_stop_pending(run_id)
    owner.stop(run_id)


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
    assert task.observations == 1
    assert not owner.operator_stop_observation_required(
        started.capture.program_run_id
    )
    assert host.advance_ready_once() == ()

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
        # The resumed task must stay active: with every task reporting the
        # Stop, the retried attempt can legitimately pause again before the
        # final RUNNING read.
        task_factory=_FirstTaskStoppedPort,
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
        # A settled ordinary Stop is intentionally dormant.  Model a crossing
        # command-side wake explicitly instead of relying on idle polling to
        # manufacture the stale worker.
        host.schedule(program_run_id)
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
        assert interventions.read(program_run_id) is None
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
        document["execution_groups"] = {
            "arm": {"execution_policy": "independent"},
            "tools": {"execution_policy": "independent"},
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
    diagnostic = owner.take_diagnostic(started.capture.program_run_id)
    assert diagnostic is not None
    assert diagnostic["state"] == "held"
    assert diagnostic["reason_code"] == "operation_stopped"
    intervention = interventions.read(started.capture.program_run_id)
    assert intervention is not None
    assert intervention.policy_parameter == "operator_stop_policy"
    # The interrupted node keeps running without progress: presented as held.
    assert "held" in held.node_statuses.values()

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
    progressed = host.advance_ready_once()[0]
    assert progressed.state is ProgramRunState.RUNNING
    assert "held" not in progressed.node_statuses.values()
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
    capture, errors, stopped = scope.capture_stop, [], []

    def held_capture(*args, **kwargs):
        entered.set()
        assert finish.wait(timeout=2)
        return capture(*args, **kwargs)

    scope.capture_stop = held_capture

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
    diagnostic = owner.take_diagnostic(started.capture.program_run_id)
    assert diagnostic is not None
    assert diagnostic["state"] == "intervention_required"
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
    assert not controls["groups"][0]["resume_allowed"]
    assert (
        controls["groups"][0]["resume_unavailable_reason"]
        == "selected_owner_work_pending"
    )
    assert "leaf_acknowledge" not in events
    assert owner.read(run_id) == before
    assert task.starts == 1 and task.cancels == 0
    held = owner.advance_once(run_id)
    assert held.state is ProgramRunState.RUNNING
    assert held.node_statuses["move_to_pick"] == "success"
    # Reached after the Stop but not started: waits for Resume.
    assert held.node_statuses["remove_table"] == "held"
    assert events.count("leaf_acknowledge") == 1
    assert task.starts == 1 and task.cancels == 0
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["resume_allowed"]
    owner.resume_motion(
        run_id,
        expected_run_revision=held.run_revision,
        expected_selection_revision=controls["selection_revision"],
        request_id="resume.after-completed-task",
    )
    completed = owner.advance_once(run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert completed.active_operation_count == 0
    assert events.count("leaf_acknowledge") == 1
    assert task.starts == 1 and task.cancels == 0


def test_execution_selection_names_the_exact_resources_its_release_reserves():
    """Group resources follow the Kernel's rule: effectful target effects of
    the selected nodes through the sealed binding, nothing else."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    binding = deepcopy(dict(admission.program_binding))

    def target(role: str, resources: list[str]) -> dict[str, Any]:
        return {
            "target_role_id": role,
            "resource_role_bindings": [
                {
                    "resource_role_id": f"{role}_joints",
                    "resource_ids": resources,
                    "compiled_resource_names": resources,
                }
            ],
        }

    binding["target_role_bindings"] = [
        target("arm", ["robot.joint_2", "robot.joint_1"]),
        # A bound role no selected node addresses effectfully is not reserved.
        target("spare", ["spare.joint_1"]),
    ]
    scope.ready = True
    scope.node_count = 3
    started = owner.start(replace(admission, program_binding=binding))
    groups = owner.execution_selection_controls(started.capture.program_run_id)["groups"]
    assert groups[0]["group_id"] == "all"
    assert groups[0]["resource_ids"] == ["robot.joint_1", "robot.joint_2"]


def test_initial_motion_run_releases_ready_scope_without_recovery():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    # The production Kernel capture already knows the sealed table when the
    # run first observes revision zero. Seed the test double equivalently.
    scope.node_count = 3
    started = owner.start(admission)
    run_id = started.capture.program_run_id

    controls = owner.execution_selection_controls(run_id)
    group = controls["groups"][0]
    assert group["state"] == "ready"
    assert group["start_allowed"]
    assert not group["stop_allowed"]
    # Run and Resume are the same release.
    assert group["resume_allowed"]

    released = owner.start_motion(
        run_id,
        expected_run_revision=started.run_revision,
        expected_selection_revision=controls["selection_revision"],
        request_id="initial.motion.run",
    )
    assert released.state is ProgramRunState.RUNNING
    assert task.starts == 0
    assert events[-1] == "scope_stop_release"
    running = owner.execution_selection_controls(run_id)["groups"][0]
    assert running["state"] == "running"
    assert not running["start_allowed"]
    assert running["stop_allowed"]


@pytest.mark.parametrize(
    "blocker",
    [
        "cell_motion_admission_closed",
        "cell_motion_admission_unreconciled",
        "program_run_admission_pending",
        "program_halt_active",
    ],
)
def test_selection_controls_project_current_kernel_release_blocker_without_releasing(blocker):
    owner, admission, _task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    selection_revision = scope.selection_revision
    initially_available = owner.execution_selection_controls(run_id)["groups"][0]
    assert initially_available["start_allowed"] is True
    assert initially_available["resume_allowed"] is True
    scope.selection_release_block_reason = blocker

    group = owner.execution_selection_controls(run_id)["groups"][0]

    assert group["state"] == "ready"
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == blocker
    assert group["resume_unavailable_reason"] == blocker
    assert group["stop_allowed"] is False
    assert scope.selection_revision == selection_revision
    assert scope.release_requests == 0
    assert "scope_stop_release" not in events

    # Admission can clear independently of selection revision. The existing
    # owner projection must observe that change instead of caching the refusal.
    scope.selection_release_block_reason = None
    recovered = owner.execution_selection_controls(run_id)["groups"][0]
    assert recovered["state"] == "ready"
    assert recovered["start_allowed"] is True
    assert recovered["resume_allowed"] is True
    assert scope.selection_revision == selection_revision
    assert scope.release_requests == 0
    assert "scope_stop_release" not in events


@pytest.mark.parametrize("blocker", ["unknown_release_blocker", 1, False, {}])
def test_selection_controls_fail_closed_on_malformed_kernel_release_blocker(blocker):
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    baseline_snapshot, baseline = owner.read_selection_controls(run_id)
    baseline_group = baseline["groups"][0]
    revision = scope.selection_revision
    scope.selection_release_block_reason = blocker

    snapshot, controls = owner.read_selection_controls(run_id)
    group = controls["groups"][0]

    assert snapshot == baseline_snapshot
    assert group["state"] == baseline_group["state"] == "ready"
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == "program_selection_observation_unavailable"
    assert group["resume_unavailable_reason"] == "program_selection_observation_unavailable"
    assert group["stop_allowed"] == baseline_group["stop_allowed"]
    assert controls["motion_held"] == baseline["motion_held"]
    assert scope.selection_revision == revision
    assert scope.release_requests == 0


def test_unreadable_advisory_scope_blocks_release_only_for_retained_projection():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    run_id = owner.start(admission).capture.program_run_id
    baseline_snapshot, baseline = owner.read_selection_controls(run_id)
    baseline_group = baseline["groups"][0]
    assert baseline_group["state"] == "ready"
    assert baseline_group["start_allowed"] is True
    assert baseline["motion_held"] is True

    def unreadable(_program_run_id):
        raise RuntimeError("temporary Kernel scope read failure")

    scope.execution_scope = unreadable
    snapshot, controls = owner.read_selection_controls(run_id)
    group = controls["groups"][0]

    assert snapshot == baseline_snapshot
    assert group["state"] == baseline_group["state"] == "ready"
    assert group["stop_allowed"] == baseline_group["stop_allowed"]
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == "program_selection_observation_unavailable"
    assert group["resume_unavailable_reason"] == "program_selection_observation_unavailable"
    assert controls["motion_held"] is True
    assert scope.release_requests == 0


def test_selection_scope_ipc_does_not_hold_program_transition_lock():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    transition_lock = owner._runs[run_id].transition_lock
    read_scope = scope.execution_scope
    entered, release = Event(), Event()

    def blocked_scope_read(program_run_id):
        entered.set()
        if not release.wait(timeout=2):
            raise RuntimeError("test scope read was not released")
        return read_scope(program_run_id)

    scope.execution_scope = blocked_scope_read
    results = []
    thread = Thread(
        target=lambda: results.append(owner.read_selection_controls(run_id))
    )
    thread.start()
    acquired = False
    try:
        assert entered.wait(timeout=1)
        acquired = transition_lock.acquire(timeout=0.1)
        if acquired:
            transition_lock.release()
    finally:
        release.set()
        thread.join(timeout=2)

    assert not thread.is_alive()
    assert acquired
    assert len(results) == 1


def test_selection_release_blocker_does_not_disable_stop_for_running_group():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.selection_release_block_reason = "cell_motion_admission_closed"

    group = owner.execution_selection_controls(run_id)["groups"][0]

    assert group["state"] == "running"
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == "selection_not_held"
    assert group["stop_allowed"] is True
    assert scope.release_requests == 0


def test_motion_held_counts_only_effectful_nodes_and_settled_holds():
    """The owner fact a held-run Draft edit relies on: every effectful node is
    held by the Kernel (never released, or stopped and certified), whatever
    the non-effectful control-flow nodes are doing."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    phases: dict[str, list[int]] = {}
    read = scope.execution_scope

    def execution_scope(program_run_id: str) -> dict[str, Any]:
        return {
            **read(program_run_id),
            **{
                f"{phase}_node_indices": phases.get(phase, [])
                for phase in ("ready", "held", "stopping", "faulted")
            },
        }

    def stop_selection(request: Mapping[str, Any]) -> dict[str, Any]:
        scope.selection_revision += 1
        return {
            "state": execution_scope(request["program_run_id"]),
            "selection_error": None,
        }

    scope.execution_scope = execution_scope
    scope.stop_selection = stop_selection
    # Executed, never Run: the Kernel holds the effectful nodes Ready; the
    # control-flow node is not held (the live case: `all` is `ready` with
    # fewer held than captured nodes).
    phases.update(ready=[0, 1])
    run_id = owner.start(admission).capture.program_run_id
    controls = owner.execution_selection_controls(run_id)
    group = controls["groups"][0]
    assert scope.node_count == 3
    assert (group["group_id"], group["state"]) == ("all", "ready")
    assert (group["node_count"], group["held_node_count"]) == (3, 2)
    assert controls["motion_held"] is True

    def published(current: dict[str, list[int]]) -> dict[str, Any]:
        phases.clear()
        phases.update(current)
        owner.stop_motion(run_id)
        return owner.execution_selection_controls(run_id)

    # Run, then a whole-Program Stop that settled: every effectful node is
    # Held while control flow keeps running, so `all` is only `mixed`.
    stopped = published({"held": [0, 1]})
    assert stopped["groups"][0]["state"] == "mixed"
    assert stopped["motion_held"] is True
    # Never-started and stopped work alike stay held.
    assert published({"ready": [0], "held": [1]})["motion_held"] is True
    # Still stopping, faulted, or one effectful node released: not held.
    assert published({"held": [0], "stopping": [1]})["motion_held"] is False
    assert published({"held": [0], "faulted": [1]})["motion_held"] is False
    assert published({"held": [0]})["motion_held"] is False


def test_operator_alarm_refusal_keeps_the_kernel_code_and_releases_nothing():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    details = {
        "reason": "operator_alarm_interlock_active",
        "interlock_alarms": '[{"activation_id":"a","condition_id":"c","record_revision":1}]',
        "unacknowledged_alarms": "[]",
    }
    scope.attention_refusal = ProgramSelectionReleaseRejectedError(
        "an active operator-alarm condition interlock gates this motion",
        code="operator_alarm_interlock_active",
        details=details,
    )
    scope.alarmed_nodes = {"move_to_pick"}
    with pytest.raises(ProgramRunOwnerError) as checked:
        owner.check_motion_attention(run_id, "move_to_pick", "retry.check")
    assert checked.value.code == "operator_alarm_interlock_active"
    assert checked.value.details == details
    check = scope.attention_checks[-1]
    assert check["schema_version"] == "manyforge.program_node_motion_attention_check.v1"
    assert check["node_instance_id"] == "move_to_pick"
    assert check["program_run_id"] == run_id
    assert check["request_context"]["request_id"] == "retry.check"

    controls = owner.execution_selection_controls(run_id)
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.start_motion(
            run_id,
            expected_run_revision=started.run_revision,
            expected_selection_revision=controls["selection_revision"],
            request_id="initial.motion.refused",
        )
    assert refused.value.code == "operator_alarm_interlock_active"
    assert scope.ready
    assert "scope_stop_release" not in events

    scope.attention_refusal = None
    owner.check_motion_attention(run_id, "move_to_pick", "retry.check.cleared")
    released = owner.start_motion(
        run_id,
        expected_run_revision=owner.read(run_id).run_revision,
        expected_selection_revision=controls["selection_revision"],
        request_id="initial.motion.after.clear",
    )
    assert released.state is ProgramRunState.RUNNING
    assert events[-1] == "scope_stop_release"


def test_recovery_attention_check_names_only_the_held_node():
    """An alarm on another node's resources does not block this recovery."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    scope.attention_refusal = ProgramSelectionReleaseRejectedError(
        "an unacknowledged operator alarm gates this motion",
        code="unacknowledged_operator_alarms",
        details={"reason": "unacknowledged_operator_alarms"},
    )
    # Robot B's alarm (another node's resources): the held node continues.
    scope.alarmed_nodes = {"remove_table"}
    owner.check_motion_attention(run_id, "move_to_pick", "recover.a")
    assert [row["node_instance_id"] for row in scope.attention_checks] == [
        "move_to_pick"
    ]
    # The held node's own alarm refuses with the Kernel's code.
    scope.alarmed_nodes = {"move_to_pick"}
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.check_motion_attention(run_id, "move_to_pick", "recover.a.alarmed")
    assert refused.value.code == "unacknowledged_operator_alarms"
    # A node outside the capture is refused before any Kernel call.
    with pytest.raises(ProgramRunOwnerError):
        owner.check_motion_attention(run_id, "not_a_node", "recover.unknown")
    assert len(scope.attention_checks) == 2


def test_halt_before_run_then_execute_keeps_motion_ready_for_run():
    """Execute, Halt, Execute: the tree resumes but motion still needs Run."""
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    assert owner.full_stop(run_id).state is ProgramRunState.FULL_STOPPING
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED

    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    assert resumed.state is ProgramRunState.RUNNING
    owner.advance_once(run_id)
    assert task.starts == 0
    assert scope.ready  # never-started work still waits for Run
    group = owner.execution_selection_controls(run_id)["groups"][0]
    assert group["state"] == "ready"
    assert group["start_allowed"]

    controls = owner.execution_selection_controls(run_id)
    owner.start_motion(
        run_id,
        expected_run_revision=owner.read(run_id).run_revision,
        expected_selection_revision=controls["selection_revision"],
        request_id="run-after-execute",
    )
    assert events[-1] == "scope_stop_release"


def test_tick_host_initial_motion_run_releases_and_wakes_selected_scope():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    observed = []
    host = ProgramRunTickHost(
        owner,
        tick_interval_s=0.01,
        state_observer=observed.append,
    )
    started = host.start_run(admission, defer_ticks=True)
    controls = owner.execution_selection_controls(started.capture.program_run_id)

    released = host.start_motion(
        started.capture.program_run_id,
        expected_run_revision=started.run_revision,
        expected_selection_revision=controls["selection_revision"],
        request_id="initial.motion.run.host",
        group_id="all",
        reason="operator initial motion Run",
    )

    assert released.run_revision == started.run_revision + 1
    assert events[-1] == "scope_stop_release"
    assert task.starts == 0
    assert observed[-1] == released
    assert started.capture.program_run_id in host._scheduled  # noqa: SLF001
    host.stop_run(started.capture.program_run_id)
    host.shutdown()


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


def test_selection_projection_uses_retained_owner_assessment_without_owner_io():
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.stop_motion(run_id)
    owner.advance_once(run_id)

    observe = task.observe
    entered = Event()

    def slow_observe(task_id):
        entered.set()
        return observe(task_id)

    task.observe = slow_observe
    _snapshot, controls = owner.read_selection_controls(run_id)
    assert not entered.is_set()
    assert controls["groups"][0]["resume_allowed"]


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
    assert reads == 1
    assert (
        owner.execution_selection_controls(run_id)["groups"][0]["state"]
        == "unavailable"
    )
    assert reads == 2  # The explicit controls read refreshes advisory eligibility.
    scope.state = ProgramRunScopeState.SETTLED
    settled = owner.advance_once(run_id)
    assert reads == 3
    assert owner.execution_selection_controls(run_id)["groups"][0]["state"] == "held"
    assert reads == 4
    assert owner.advance_once(run_id).run_revision == settled.run_revision
    assert reads == 4 and scope.stop_requests == 1 and task.starts == 0
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

    waiting = host.advance_ready_once()[0]
    assert waiting.state is ProgramRunState.RUNNING
    assert task.starts == 0
    # Reached but not started: presented as held (waiting for Run), not idle.
    assert "held" in waiting.node_statuses.values()

    resumed = host.resume_motion(
        program_run_id,
        expected_run_revision=owner.read(program_run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="run-after-boundary-stop.1",
    )
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.release_requests == 1
    running = host.advance_ready_once()[0]
    assert running.state is ProgramRunState.RUNNING
    assert "held" not in running.node_statuses.values()
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
    # Execute keeps both interrupted attempts stopped.
    assert task.recoveries == []
    assert len(interventions.read_all(run_id)) == 2
    owner.advance_once(run_id)
    assert task.starts == 2
    owner.resume_motion(
        run_id,
        expected_run_revision=owner.read(run_id).run_revision,
        expected_selection_revision=_scope.selection_revision,
        request_id="resume-both-motion",
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
    second_settled = False

    def current_assessment(task_id):
        snapshot = observe(task_id)
        if task_id == "task-2" and not second_settled:
            return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.ACTIVE)
        return snapshot

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
    # The sibling stops settling and now waits for Resume: running -> held.
    assert first.node_statuses["parallel.1"] == "running"
    assert second.node_statuses["parallel.1"] == "held"
    assert second.node_status_revision == first.node_status_revision + 1
    assert len(interventions.read_all(run_id)) == 2
    assert owner.execution_selection_controls(run_id)["groups"][0]["resume_allowed"]
    assert not task.recoveries
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


def _timed_admission(
    admission: ProgramRunAdmission, node_id: str, **parameters: Any
) -> ProgramRunAdmission:
    """One timed leaf that keeps running through an operator Stop, so only
    the Program clock decides how its time counts."""
    document = admission.program.to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "count",
                "type": "int64",
                "access": "read_write",
                "persistence": "run",
                "initial": 0,
            }
        ],
    }
    leaf_parameters = (
        parameters
        if node_id == "timer"
        else {"key": "count", "value": 5, **parameters}
    )
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": [
            {
                "node_id": node_id,
                "instance_id": "wait",
                "parameters": leaf_parameters,
                "children": [],
                "operator_stop_policy": "continue",
            }
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    return _admission_for_program(
        admission, program, [row.to_dict() for row in program_node_policies(program)]
    )


def _timed_run(node_id: str, **parameters: Any):
    now = [100.0]
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        program_clock_source=lambda: now[0],
    )
    run_id = owner.start(
        _timed_admission(admission, node_id, **parameters)
    ).capture.program_run_id
    node = owner._runs[run_id].run._authored_nodes["wait"]  # noqa: SLF001
    return owner, scope, run_id, node, now


@pytest.mark.parametrize(
    ("node_id", "parameters", "terminal"),
    [
        ("timer", {"duration_s": 2.0}, ProgramRunState.SUCCEEDED),
        ("timer", {"duration_s": 2.0, "duration_while_held": "count"},
         ProgramRunState.SUCCEEDED),
        ("logic.wait_for_value", {"timeout_s": 2.0}, ProgramRunState.FAILED),
        ("logic.wait_for_value", {"timeout_s": 2.0, "timeout_while_held": "count"},
         ProgramRunState.FAILED),
    ],
)
def test_halt_freezes_program_time_and_execute_continues_the_same_attempt(
    node_id: str, parameters: dict[str, Any], terminal: ProgramRunState
) -> None:
    owner, _scope, run_id, node, now = _timed_run(node_id, **parameters)
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    attempt = node._identity  # noqa: SLF001
    now[0] = 101.0

    assert owner.full_stop(run_id).state is ProgramRunState.FULL_STOPPING
    now[0] = 150.0
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    now[0] = 400.0  # Halted for 299 s: none of it counts, held or not.
    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    assert resumed.state is ProgramRunState.RUNNING
    assert resumed.node_statuses["wait"] == "running"  # never interrupted

    now[0] = 400.9
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert node._identity is attempt  # noqa: SLF001
    now[0] = 401.0
    finished = owner.advance_once(run_id)
    assert finished.state is terminal
    if terminal is ProgramRunState.FAILED:
        assert node.feedback_message.startswith("wait_timed_out")


@pytest.mark.parametrize("node_id", ["timer", "logic.wait_for_value"])
def test_program_stop_of_a_halted_run_interrupts_its_wait(node_id: str) -> None:
    parameters = {"duration_s": 2.0} if node_id == "timer" else {"timeout_s": 2.0}
    owner, _scope, run_id, node, _now = _timed_run(node_id, **parameters)
    owner.advance_once(run_id)
    owner.full_stop(run_id)
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED

    stopped = owner.stop(run_id)

    assert stopped.state is ProgramRunState.STOPPED
    assert node.outcome == "interrupted"
    assert stopped.node_statuses["wait"] == "interrupted"


def test_a_failure_pause_freezes_program_time_until_it_is_resolved() -> None:
    owner, _task, _scope, _interventions, run_id, failure = _failure_paused_run()
    clock = owner._runs[run_id].run.program_clock  # noqa: SLF001
    assert clock.frozen  # The tree is dormant, like a Halt.
    owner.resolve_intervention(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action="retry",
        result=None,
    )
    assert not clock.frozen
    owner.stop(run_id)


def test_an_operator_stop_intervention_keeps_program_time_running() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    assert owner.advance_once(run_id).state is ProgramRunState.INTERVENTION_REQUIRED
    # The tree keeps ticking; each held node decides how its held time counts.
    assert not owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    owner.stop(run_id)


def test_full_stop_then_execute_keeps_motion_stopped_until_resume() -> None:
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
    assert stopped.active_operation_count == 0
    assert stopped.tree_status == "running"
    assert scope.stop_requests == 1
    assert task.cancels == 0
    assert context.unbound == []
    assert interventions.read(program_run_id) is not None

    resumed = host.resume_full_stop(
        program_run_id,
        request_id="run-after-full-stop.1",
    )

    # Execute after Halt resumes execution only: no recovery, no new attempt,
    # and the interrupted work keeps its Stop intervention.
    assert resumed.state is ProgramRunState.RUNNING
    assert resumed.active_operation_count is None
    assert task.recoveries == []
    assert scope.release_requests == 1
    assert interventions.read(program_run_id) is not None
    waiting = host.advance_ready_once()[0]
    assert waiting.state in {
        ProgramRunState.RUNNING,
        ProgramRunState.INTERVENTION_REQUIRED,
    }
    assert "held" in waiting.node_statuses.values()
    assert task.starts == 1

    # Only the explicit group Resume recovers and restarts the motion.
    host.resume_motion(
        program_run_id,
        expected_run_revision=owner.read(program_run_id).run_revision,
        expected_selection_revision=scope.selection_revision,
        request_id="resume-after-execute.1",
    )
    assert len(task.recoveries) == 1
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
    assert running.active_operation_count is None
    assert events == ["leaf_start"]

    scope.state = ProgramRunScopeState.ACTIVE
    stopping = owner.stop(started.capture.program_run_id, reason="operator stop")
    assert stopping.state is ProgramRunState.STOPPING
    assert stopping.tree_status == "invalid"
    # The cancelled running leaf is cancellable: it reports interrupted.
    assert list(stopping.node_statuses.values()).count("interrupted") == 1
    assert set(stopping.node_statuses.values()) == {"idle", "interrupted"}
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
    assert stopped.node_statuses == stopping.node_statuses
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
        "details",
        "origin",
        "operation_failures",
    }
    assert value["operation_failures"] == []
    assert value["schema_version"] == "manyforge.program_run_status.v3"
    assert value["details"] is None and value["origin"] is None
    assert value["node_status_revision"] == 1
    assert value["node_statuses"]
    assert value["capture"]["schema_version"] == ("manyforge.program_run_capture.v2")


def _scenario_selection(admission: ProgramRunAdmission) -> dict[str, Any]:
    program = admission.program
    environment = admission.environment
    return {
        "schema_version": "manyforge.program_run_input.v2",
        "applied_record_digest": admission.applied_record_digest,
        "scenario_id": admission.scenario_id,
        "project_path": admission.project_path,
        "source_document_digest": admission.source_document_digest,
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
        "simulation_scene": None,
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
        expected_source_document_digest=(
            fixture_admission.source_document_digest
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
            expected_source_document_digest="sha256:" + "e" * 64,
            environment=environment,
            owner_availability=fixture_admission.readiness.owner_by_port,
        )
    except ProgramRunOwnerError as error:
        assert "Scenario source document changed" in str(error)
    else:
        raise AssertionError("stale applied configuration revision was admitted")


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
        expected_source_document_digest=admission.source_document_digest,
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
            expected_source_document_digest=(admission.source_document_digest),
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )


def test_applied_program_publication_is_complete_run_input() -> None:
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
            "schema_version",
        }
    }
    applied.update(
        {
            "schema_version": "manyforge.applied_program_selection.v3",
            "baseline_program_binding_id": admission.program_binding_id,
            "preparation_digest": _DIGEST_C,
            "source": {"record_digest": _DIGEST_B},
            "simulation_scene": {
                "cell_frame_id": "universe",
                "scene_entities": [
                    {"entity": "/World/box", "scene_resource_id": "box"}
                ],
            },
        }
    )

    joined = build_applied_program_run_admission(
        applied,
        expected_scenario_id=admission.scenario_id,
        expected_source_document_digest=admission.source_document_digest,
        environment=admission.environment,
        owner_availability=admission.readiness.owner_by_port,
    )

    assert joined.program_binding_id == admission.program_binding_id
    assert joined.owner_identities[0].owner_role == "headless_projection"
    # The Applied Scenario project folder scopes the run's persistent values.
    assert joined.project_path == admission.project_path
    # The simulator entity mapping reaches the client ports frozen.
    scene = joined.port_context("program-run.1").simulation_scene
    assert scene["cell_frame_id"] == "universe"
    assert scene["scene_entities"][0]["entity"] == "/World/box"
    applied["graph_revision"] = _DIGEST_A
    try:
        build_applied_program_run_admission(
            applied,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=admission.source_document_digest,
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )
    except ProgramRunOwnerError as error:
        assert "scenario Program, binding, and retained identities disagree" in str(error)
    else:
        raise AssertionError("incoherent Applied publication was admitted")


def test_applied_publication_with_removed_parameter_overrides_is_refused() -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    live = _scenario_selection(admission)
    applied = {
        key: value
        for key, value in live.items()
        if key not in {"applied_record_digest", "schema_version"}
    }
    # The v2 publication carried launch-time typed_parameter_overrides. It is
    # refused by version, never read as an override-free selection.
    applied.update(
        {
            "schema_version": "manyforge.applied_program_selection.v2",
            "baseline_program_binding_id": admission.program_binding_id,
            "preparation_digest": _DIGEST_C,
            "source": {"record_digest": _DIGEST_B},
            "typed_parameter_overrides": {},
        }
    )
    with pytest.raises(ProgramRunOwnerError, match="overrides were removed"):
        build_applied_program_run_admission(
            applied,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=admission.source_document_digest,
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )
    applied["schema_version"] = "manyforge.applied_program_selection.v3"
    with pytest.raises(ProgramRunOwnerError, match="wrong fields"):
        build_applied_program_run_admission(
            applied,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=admission.source_document_digest,
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )


def _timer_with_sourced_duration_admission(
    admission: ProgramRunAdmission,
) -> ProgramRunAdmission:
    """A timer whose `duration_s` comes from an empty key; its failures
    pause for the operator (recovery policy with intervention)."""
    document = admission.program.to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "delay",
                "type": "float64",
                "access": "read_write",
                "persistence": "run",
            }
        ],
    }
    document["tree"] = {
        "node_id": "sequence",
        "instance_id": "root",
        "parameters": {},
        "children": [
            {
                "node_id": "timer",
                "instance_id": "wait",
                "parameters": {},
                "value_sources": {
                    "duration_s": {"kind": "blackboard_key", "reference": "delay"}
                },
                "recovery_policy": {"allow_intervention": True, "max_retries": 1},
                "children": [],
            }
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    return _admission_for_program(
        admission, program, [row.to_dict() for row in program_node_policies(program)]
    )


def test_timer_input_failure_pauses_and_operator_retry_re_resolves_inputs() -> None:
    """The timer declares no `failure` outcome, yet a missing sourced
    duration is an ordinary node failure: it raises the failure pause, and
    the operator's Retry starts a fresh attempt that resolves the fixed key."""
    now = [100.0]
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        interventions=interventions,
        program_clock_source=lambda: now[0],
    )
    run_id = owner.start(
        _timer_with_sourced_duration_admission(admission)
    ).capture.program_run_id
    assert set(admission.environment.catalog.get("timer").outcomes) == {
        "success",
        "interrupted",
    }

    paused = owner.advance_once(run_id)

    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    assert paused.node_statuses["wait"] == "failure"
    assert paused.reason_code == "node_input_unavailable"
    expected = "missing input duration_s from blackboard_key 'delay': "
    assert paused.message.startswith(expected)
    failure = interventions.read(run_id)
    assert failure is not None
    assert failure.reason_code == "node_input_unavailable"
    assert failure.policy_parameter == "recovery_policy"
    assert failure.message.startswith(expected)
    diagnostic = owner.take_diagnostic(run_id)
    assert diagnostic is not None
    assert diagnostic["state"] == "intervention_required"
    assert diagnostic["reason_code"] == "node_input_unavailable"
    assert diagnostic["message"].startswith(expected)
    # The failure pause stopped the Program's other motion, like any failure.
    assert scope.stop_requests == 1
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001

    # The operator fixes the key, then retries the failed step.
    owner._runs[run_id].run.value_store.write("delay", 1.0)  # noqa: SLF001
    owner.retry_failure_pause(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action=None,
        request_id="retry.1",
        reason=None,
    )
    resumed = owner.resolve_intervention(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action="retry",
        result=None,
    )
    assert resumed.state is ProgramRunState.RUNNING
    owner.advance_once(run_id)  # consumes the Retry decision
    started = owner.advance_once(run_id)  # the fresh attempt resolves 1.0 s
    assert started.state is ProgramRunState.RUNNING
    assert started.node_statuses["wait"] == "running"
    now[0] += 1.0
    finished = owner.advance_once(run_id)
    assert finished.state is ProgramRunState.SUCCEEDED
    assert finished.node_statuses["wait"] == "success"


# One failure, produced once where it happens, reaches every run record
# (run status, recorded diagnostic, recovery offer) with exact details and
# origin; a Stop is not a failure and carries neither.

_OWNER_REFUSAL_DETAILS = {
    "reason": "observed_state_unavailable",
    "reference_type": "frame",
    "reference_id": "tool0",
    "target_role_id": "arm.left",
    "retryable": False,
    # Bounded away: an over-long value, a nested value, an unregistered key.
    "path": "p" * 257,
    "latch_id": {"nested": "latch.1"},
    "trace": "owner internals",
}
_OWNER_REFUSAL_PROJECTED = {
    "reason": "observed_state_unavailable",
    "reference_type": "frame",
    "reference_id": "tool0",
    "target_role_id": "arm.left",
    "retryable": False,
}
_OWNER_REFUSAL_MESSAGE = (
    "addressed observed state is unavailable (observed_state_unavailable; "
    "reference frame 'tool0' of target role 'arm.left')"
)


class _RefusingTaskPort(_TaskPort):
    """The motion owner refuses the request before accepting it."""

    def start(
        self,
        _node_id: str,
        _identity: NodeAttemptIdentity,
        _inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        self._events.append("leaf_start")
        raise KernelOperationAdmissionRejectedError(
            "operation_rejected",
            "addressed observed state is unavailable",
            _OWNER_REFUSAL_DETAILS,
        )


def _recovery_offer(snapshot, interventions, run_id: str, node: str):
    intervention = interventions.read(run_id)
    assert intervention is not None
    controls = ProgramRuntimeControls(snapshot.capture.to_dict())
    return controls.recovery_offer(
        cycle_generation=1,
        owner_epoch="program-runner.epoch.1",
        intervention=intervention,
        policy=snapshot.capture.node_policy_by_instance_id[node].recovery_policy,
    )


def _pausing_move(admission: ProgramRunAdmission) -> ProgramRunAdmission:
    """The example Program whose motion pauses for the operator on failure."""
    document = admission.program.to_dict()
    document["tree"]["children"][0]["recovery_policy"] = {
        "allow_intervention": True,
        "max_retries": 1,
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    return _admission_for_program(
        admission, program, [row.to_dict() for row in program_node_policies(program)]
    )


def test_input_failure_reaches_status_diagnostic_and_offer_exactly() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        interventions=interventions,
        program_clock_source=lambda: 100.0,
    )
    run_id = owner.start(
        _timer_with_sourced_duration_admission(admission)
    ).capture.program_run_id

    paused = owner.advance_once(run_id)

    details = {
        "input_problem": "missing",
        "path": "duration_s",
        "source_kind": "blackboard_key",
        "source_reference": "delay",
    }
    origin = {"kind": "program_node", "node_instance_id": "wait", "owner_api": None}
    status = paused.to_dict()
    assert status["schema_version"] == "manyforge.program_run_status.v3"
    assert status["reason_code"] == "node_input_unavailable"
    assert (status["details"], status["origin"]) == (details, origin)
    diagnostic = owner.take_diagnostic(run_id)
    assert diagnostic["schema_version"] == "manyforge.program_diagnostic.v3"
    assert diagnostic["reason_code"] == "node_input_unavailable"
    assert diagnostic["message"] == status["message"]
    assert (diagnostic["details"], diagnostic["origin"]) == (details, origin)
    offer = _recovery_offer(paused, interventions, run_id, "wait")
    assert offer["schema_version"] == "manyforge.cycle_recovery_offer.v4"
    assert offer["reason_code"] == "node_input_unavailable"
    assert offer["message"] == status["message"]
    assert offer["message"].startswith(
        "missing input duration_s from blackboard_key 'delay': "
    )
    assert (offer["details"], offer["origin"]) == (details, origin)


def test_owner_refusal_reaches_status_diagnostic_and_offer_exactly() -> None:
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _RefusingTaskPort(events, OwnerTaskState.ACTIVE),
        interventions=interventions,
    )
    run_id = owner.start(_pausing_move(admission)).capture.program_run_id

    paused = owner.advance_once(run_id)

    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    origin = {
        "kind": "owner",
        "node_instance_id": "move_to_pick",
        "owner_api": "kernel.operation",
    }
    status = paused.to_dict()
    assert status["reason_code"] == "owner_request_failed"
    # The message names the owner's cause once; the details travel apart.
    assert status["message"] == _OWNER_REFUSAL_MESSAGE
    assert (status["details"], status["origin"]) == (_OWNER_REFUSAL_PROJECTED, origin)
    diagnostic = owner.take_diagnostic(run_id)
    assert diagnostic["state"] == "intervention_required"
    assert diagnostic["message"] == _OWNER_REFUSAL_MESSAGE
    assert (diagnostic["details"], diagnostic["origin"]) == (
        _OWNER_REFUSAL_PROJECTED,
        origin,
    )
    offer = _recovery_offer(paused, interventions, run_id, "move_to_pick")
    assert offer["reason_code"] == "owner_request_failed"
    assert offer["message"] == _OWNER_REFUSAL_MESSAGE
    assert (offer["details"], offer["origin"]) == (_OWNER_REFUSAL_PROJECTED, origin)


def test_terminal_owner_refusal_keeps_its_details_and_origin() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _RefusingTaskPort(events, OwnerTaskState.ACTIVE),
    )
    run_id = owner.start(admission).capture.program_run_id

    failed = owner.advance_once(run_id)

    assert failed.state is ProgramRunState.FAILED
    origin = {
        "kind": "owner",
        "node_instance_id": "move_to_pick",
        "owner_api": "kernel.operation",
    }
    status = failed.to_dict()
    assert (status["reason_code"], status["message"]) == (
        "owner_request_failed",
        _OWNER_REFUSAL_MESSAGE,
    )
    assert (status["details"], status["origin"]) == (_OWNER_REFUSAL_PROJECTED, origin)
    diagnostic = owner.take_diagnostic(run_id)
    assert diagnostic["state"] == "failed"
    assert (diagnostic["reason_code"], diagnostic["message"]) == (
        "owner_request_failed",
        _OWNER_REFUSAL_MESSAGE,
    )
    assert (diagnostic["details"], diagnostic["origin"]) == (
        _OWNER_REFUSAL_PROJECTED,
        origin,
    )


def test_success_and_stop_carry_no_failure_details_or_origin() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    succeeded = owner.advance_once(owner.start(admission).capture.program_run_id)
    assert succeeded.state is ProgramRunState.SUCCEEDED
    diagnostic = owner.take_diagnostic(succeeded.capture.program_run_id)
    for record in (succeeded.to_dict(), diagnostic):
        assert record["reason_code"] is None
        assert record["details"] is None and record["origin"] is None

    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    stopped = owner.stop(run_id, reason="operator Stop")
    while stopped.state is not ProgramRunState.STOPPED:
        stopped = owner.advance_once(run_id)
    status = stopped.to_dict()
    assert status["reason_code"] is None
    assert status["details"] is None and status["origin"] is None


@pytest.mark.parametrize(
    ("task_state", "final_state"),
    [
        (OwnerTaskState.FAILED, ProgramRunState.FAILED),
        (OwnerTaskState.SUCCEEDED, ProgramRunState.SUCCEEDED),
    ],
)
def test_runner_failure_names_the_program_runner_without_details(
    task_state: OwnerTaskState, final_state: ProgramRunState
) -> None:
    owner, admission, _task, scope, _events = _fixture(initial_task_state=task_state)
    run_id = owner.start(admission).capture.program_run_id

    def unknown_scope(_program_run_id: str) -> ProgramRunScopeSnapshot:
        raise RuntimeError("Kernel scope observation timed out")

    scope.observe = unknown_scope
    snapshot = owner.advance_once(run_id)

    assert snapshot.state is final_state
    status = snapshot.to_dict()
    assert status["reason_code"] == "program_run_scope_unknown"
    assert status["details"] is None
    # The runner ended the run; a run that ends succeeded did not fail.
    assert status["origin"] == (
        None
        if final_state is ProgramRunState.SUCCEEDED
        else {"kind": "program_runner", "node_instance_id": None, "owner_api": None}
    )


def test_run_status_refuses_unbounded_or_orphan_failure_fields() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    snapshot = owner.start(admission)
    origin = {"kind": "owner", "node_instance_id": "n", "owner_api": None}
    with pytest.raises(ValueError, match="outside their bound"):
        replace(snapshot, reason_code="x", details={"path": "p" * 257}, origin=origin)
    with pytest.raises(ValueError, match="origin"):
        replace(snapshot, reason_code="x", details={"path": "duration_s"})
    with pytest.raises(ValueError, match="reason code"):
        replace(snapshot, origin=origin)


def _program_dwell_admission(admission, policy="hold"):
    document = admission.program.to_dict()
    document["target_roles"]["gripper"] = {
        "component_role": "robot",
        "required_capabilities": [
            {"capability_id": capability, "parameters": {}, "disposition": "required"}
            for capability in ("tool.gripper.command", "tool.gripper.feedback")
        ],
    }
    document["tree"] = {"node_id":"gripper.command_position","instance_id":"command",
        "parameters":{"target_role":"gripper","position":{"kind":"linear_m","value_m":0.008}},"operator_stop_policy":policy,"children":[]}
    document["program_revision"] = compute_content_identity(document,own_field="program_revision",
        identity_domain="program-revision",api_version="manyforge.program.v2")
    program = Program.from_dict(document,admission.environment.catalog)
    return _admission_for_program(admission,program,[row.to_dict() for row in program_node_policies(program)])


def test_program_owner_halt_preserves_delivery_settled_dwell_attempt():
    from manyforge_behavior.nodes.stop_policy import OperatorMotionStopPolicy
    class DwellPort(_TaskPort):
        def start(self, *args):
            result = super().start(*args)
            return replace(result,metadata={"gripper_actuation_kind":"pneumatic","gripper_dwell_s":2.0})
    now=[100.0]
    owner,admission,task,scope,events = _fixture(initial_task_state=OwnerTaskState.SUCCEEDED,
        task_factory=lambda events:DwellPort(events,OwnerTaskState.SUCCEEDED),program_clock_source=lambda:now[0])
    run_id=owner.start(_program_dwell_admission(admission, "continue")).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    run=owner._runs[run_id].run
    wrapped=run._authored_nodes["command"]
    node=wrapped.decorated if isinstance(wrapped,OperatorMotionStopPolicy) else wrapped
    attempt=node.attempt_identity
    assert node.cached_control_state() == "settled" and not node.has_active_task
    now[0]=101.0
    assert owner.full_stop(run_id).state is ProgramRunState.FULL_STOPPING
    now[0]=150.0
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    now[0]=400.0
    assert owner.resume_full_stop(run_id,request_id="execute-after-halt").state is ProgramRunState.RUNNING
    assert node.attempt_identity is attempt
    now[0]=400.9
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    now[0]=401.0
    assert owner.advance_once(run_id).state is ProgramRunState.SUCCEEDED
    assert task.starts == 1 and task.cancels == 0
    assert events.count("leaf_acknowledge") == 1


@pytest.mark.parametrize("cleanup", ["complete","pending","new_hold"])
def test_program_owner_retires_held_preparation_before_exact_selected_release(cleanup):
    from manyforge_behavior.orchestration_ports import OwnerInvocationHeldError
    class UnsentPort(_TaskPort):
        pending_preparation = False
        def observe(self, task_id):
            self.pending_preparation = True
            raise OwnerInvocationHeldError(scope.selection_revision)
    owner,admission,task,scope,events = _fixture(initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events:UnsentPort(events,OwnerTaskState.ACTIVE))
    run_id=owner.start(_program_dwell_admission(admission)).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    scope.state=ProgramRunScopeState.SETTLED
    owner.stop_motion(run_id,group_id="all")
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    before=owner.read(run_id)
    assert task.pending_preparation and task.starts == 1
    def retire(actual_run, selected, revision):
        assert actual_run == run_id and selected == frozenset({"command"})
        assert revision == scope.selection_revision
        assert task.pending_preparation
        events.append("preparation_cleanup")
        if cleanup == "pending": raise RuntimeError("readback still resident")
        task.pending_preparation=False
        if cleanup == "new_hold": scope.selection_revision+=1
    scope.retire_held_preparations=retire
    args=dict(expected_run_revision=before.run_revision,expected_selection_revision=scope.selection_revision,
              request_id="resume-unsent",group_id="all",reason=None)
    if cleanup == "complete":
        owner.resume_motion(run_id,**args)
        assert events.index("preparation_cleanup") < events.index("scope_stop_release")
        assert scope.release_requests == 1
        assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
        assert task.starts == 2
    else:
        with pytest.raises(ProgramRunOwnerError): owner.resume_motion(run_id,**args)
        assert scope.release_requests == 0 and scope.held
        assert task.starts == 1


class _MotionRetryTaskPort(_TaskPort):
    """A motion attempt fails with an eligible planning cause; it can be closed."""

    def __init__(self, events: list[str], holder: dict[str, Any]) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self._holder = holder
        self.closes: list[str] = []
        self.recoveries: list[str] = []
        self._assessment = {
            "allowed_actions": ["retry_after_scene_repair"],
            "recovery_class": "planning_failed",
            "status": "retry_ready",
            "execution_state": "Failed",
            "eligible_close": True,
            "retry_eligibility": {
                "eligible": True,
                "cause": "planning_no_effect",
                "effect": "known_no_effect",
                "reason": None,
            },
        }

    def start(self, _node_id, _identity, _inputs) -> OwnerTaskSnapshot:
        self.starts += 1
        return self.observe(f"task-{self.starts}")

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.FAILED,
            metadata={"recovery_assessment": self._assessment, "operation_id": task_id},
            reason_code="planning_failed",
            message="no feasible path",
        )

    def close(self, task_id, _identity, *, request_id) -> bool:
        self.closes.append(task_id)
        return True

    def recover(self, _task_id, _identity, *, action, request_id, reason):
        self.recoveries.append(action)
        return {"request_id": request_id, "action": action, "accepted": True, "error": None}


def test_run_stopped_during_an_automatic_retry_wait_leaves_nothing_resident() -> None:
    clock = [0.0]
    holder: dict[str, Any] = {}
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _MotionRetryTaskPort(events, holder),
        program_clock_source=lambda: clock[0],
    )
    run_id = owner.start(admission).capture.program_run_id

    waiting = owner.advance_once(run_id)

    assert waiting.state is ProgramRunState.RUNNING  # The wait keeps the node running.
    assert task.closes == ["task-1"]  # Closed before the wait.
    retried = owner.take_node_history_events(run_id)
    assert len(retried) == 1
    assert retried[0]["attempt"] == 2
    assert retried[0]["max_attempts"] == 4
    assert retried[0]["cause"] == "planning_no_effect"
    assert owner.take_node_history_events(run_id) == ()

    stopped = owner.stop(run_id)

    # No run-end backstop: nothing was resident, nothing is reported.
    assert task.closes == ["task-1"] and task.recoveries == []
    assert task.cancels == 0
    assert stopped.state is ProgramRunState.STOPPED


# --- R5: per-node failure holds, Stop scope, Continue, operation failures ---

_OPERATION_FACT = {
    "operation": {"source_owner_epoch": "kernel.epoch.1", "operation_id": "op", "operation_revision": 2},
    "resource_ids": ["arm.joints"],
    "failure": {"code": "no_feasible_candidate", "message": "No pick candidate is feasible"},
    "terminal_facts": [{"kind": "local_failure", "effect": "no_effect"}],
}


class _HeldFailureTaskPort(_PolicyTaskPort):
    """Every attempt fails into an authored pause, with its operation facts;
    the operation can be closed."""

    def __init__(self, events: list[str]) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self.closes: list[tuple[str, str]] = []

    def start(self, node_id, identity, inputs) -> OwnerTaskSnapshot:
        started = super().start(node_id, identity, inputs)
        return replace(
            started, metadata={**started.metadata, "operation_failure": _OPERATION_FACT}
        )

    def close(self, task_id, _identity, *, request_id) -> bool:
        self.closes.append((task_id, request_id))
        return True


def _two_group_holds(
    scope_value: str | None,
    *,
    success_policy: str = "all",
    layout: Sequence[tuple[str, list[str] | None]] = (
        ("arm_move", ["arm"]),
        ("tool_move", ["tools"]),
    ),
):
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=_HeldFailureTaskPort,
        interventions=interventions,
    )
    document = admission.program.to_dict()
    move = document["tree"]["children"][0]
    policy = {"allow_intervention": True, "max_retries": 0}
    if scope_value is not None:
        policy["intervention_stop_scope"] = scope_value
    children = []
    for instance_id, groups in layout:
        child = deepcopy(move)
        child["instance_id"] = instance_id
        if groups is not None:
            child["execution_groups"] = groups
        child["recovery_policy"] = dict(policy)
        children.append(child)
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "parallel",
        "parameters": {"success_policy": success_policy},
        "children": children,
    }
    document["execution_groups"] = {
        group: {"execution_policy": "independent"}
        for _node, groups in layout
        for group in groups or ()
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    rows = [row.to_dict() for row in program_node_policies(program)]
    admission = _admission_for_program(admission, program, rows)
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)
    return owner, task, scope, interventions, run_id, paused


def _hold(interventions, run_id, node_instance_id):
    return next(
        hold
        for hold in interventions.read_holds(run_id)
        if hold.identity.node_instance_id == node_instance_id
    )


def test_every_held_node_is_its_own_hold_with_its_operation_failure_row() -> None:
    owner, _task, scope, interventions, run_id, paused = _two_group_holds(None)

    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    assert [hold.identity.node_instance_id for hold in interventions.read_holds(run_id)] == [
        "arm_move",
        "tool_move",
    ]
    assert owner.failure_holds(run_id) == interventions.read_holds(run_id)
    assert owner.hold_motion_groups(run_id, "arm_move") == (("arm",), "all")
    # Default scope all: the pause stops every group, once.
    assert scope.last_selection_stop["group_ids"] == ["all"]
    rows = paused.to_dict()["operation_failures"]
    assert [(row["node_instance_id"], row["recovery_class"]) for row in rows] == [
        ("arm_move", "failed_validation"),
        ("tool_move", "failed_validation"),
    ]
    arm = _hold(interventions, run_id, "arm_move")
    assert rows[0]["node_attempt_id"] == arm.identity.node_attempt_id
    # The revision that published the hold (the pause Stop moves it on).
    assert 1 < rows[0]["operator_facing_run_revision"] <= paused.run_revision
    assert rows[1]["operator_facing_run_revision"] == rows[0]["operator_facing_run_revision"]
    assert {key: rows[0][key] for key in _OPERATION_FACT} == _OPERATION_FACT
    owner.stop(run_id)


def test_retry_of_one_hold_releases_every_group_not_owned_by_another_hold() -> None:
    owner, task, scope, interventions, run_id, _paused = _two_group_holds(None)
    arm = _hold(interventions, run_id, "arm_move")
    released: list[frozenset[str]] = []
    run = owner._entry(run_id).run
    original = run.note_operator_release
    run.note_operator_release = lambda nodes: (released.append(nodes), original(nodes))

    owner.retry_failure_pause(
        run_id,
        node_attempt_id=arm.identity.node_attempt_id,
        action=None,
        request_id="retry.arm",
        reason=None,
    )
    assert scope.last_selection_release["group_ids"] == ["arm"]  # tools stays held
    assert released and "tool_move" not in released[0]

    resumed = owner.resolve_intervention(
        run_id, node_attempt_id=arm.identity.node_attempt_id, action="retry"
    )
    # The other hold stays: still intervention_required, but the tree ticks.
    assert resumed.state is ProgramRunState.INTERVENTION_REQUIRED
    assert resumed.reason_code == "no_feasible_candidate"
    assert [row["node_instance_id"] for row in resumed.to_dict()["operation_failures"]] == [
        "tool_move"
    ]
    starts = task.starts
    owner.advance_once(run_id)  # Consumes arm's Retry decision.
    owner.advance_once(run_id)  # Its fresh attempt starts while tool_move stays held.
    assert task.starts == starts + 1
    owner.stop(run_id)


def test_own_groups_scope_stops_and_releases_only_the_held_nodes_groups() -> None:
    owner, _task, scope, interventions, run_id, _paused = _two_group_holds("own_groups")

    assert scope.last_selection_stop["group_ids"] == ["arm", "tools"]
    assert owner.hold_motion_groups(run_id, "tool_move") == (("tools",), "own_groups")
    tool = _hold(interventions, run_id, "tool_move")
    owner.retry_failure_pause(
        run_id,
        node_attempt_id=tool.identity.node_attempt_id,
        action=None,
        request_id="retry.tool",
        reason=None,
    )
    assert scope.last_selection_release["group_ids"] == ["tools"]
    owner.stop(run_id)


def _release_for(held: str, others: Sequence[str], *, scope: str = "all"):
    """`_failure_hold_release` over named groups arm/tools/spare; `free` is
    the node in no named group (contract item 43)."""
    groups = {"arm_move": ("arm",), "tool_move": ("tools",), "free_move": ("all",)}
    policy = SimpleNamespace(recovery_policy={"intervention_stop_scope": scope})
    run = SimpleNamespace(
        node_motion_groups=groups.__getitem__,
        execution_selections={"all": (), "arm": (), "spare": (), "tools": ()},
        capture=SimpleNamespace(node_policy_by_instance_id={held: policy}),
    )
    hold = lambda node: SimpleNamespace(  # noqa: E731
        identity=SimpleNamespace(node_instance_id=node)
    )
    return ProgramRunOwner._failure_hold_release(
        SimpleNamespace(_hold_stop_scope=ProgramRunOwner._hold_stop_scope),
        run,
        hold(held),
        [hold(held), *(hold(node) for node in others)],
    )


@pytest.mark.parametrize("scope", ["all", "own_groups"])
def test_a_hold_in_no_named_group_releases_every_group_no_other_hold_owns(scope) -> None:
    assert _release_for("free_move", [], scope=scope) == "all"
    assert _release_for("free_move", ["arm_move"], scope=scope) == ["spare", "tools"]
    assert _release_for("free_move", ["arm_move", "tool_move"], scope=scope) == ["spare"]
    # While it is active, another hold releases only its own groups.
    assert _release_for("arm_move", ["free_move"]) == ["arm"]
    assert _release_for("arm_move", ["tool_move"]) == ["arm", "spare"]
    assert _release_for("arm_move", []) == "all"


@pytest.mark.parametrize("operation", ["retry", "continue"])
def test_a_hold_in_no_named_group_releases_only_its_own_node(operation) -> None:
    owner, _task, scope, interventions, run_id, _paused = _two_group_holds(
        None, layout=(("arm_move", ["arm"]), ("free_move", None))
    )
    assert owner.hold_motion_groups(run_id, "free_move") == (("all",), "all")
    releases = scope.release_requests
    free = _hold(interventions, run_id, "free_move")
    if operation == "retry":
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=free.identity.node_attempt_id,
            action=None,
            request_id="retry.free",
            reason=None,
        )
    else:
        owner.continue_failure_pause(
            run_id,
            node_attempt_id=free.identity.node_attempt_id,
            request_id="continue.free",
            reason=None,
        )
    # arm_move's hold owns arm (the only named group): free_move releases
    # itself by node_instance_id (SX-2020), never another hold's group.
    assert scope.release_requests == releases + 1
    assert scope.last_selection_release["group_ids"] == []
    assert scope.last_selection_release["node_instance_id"] == "free_move"
    owner.stop(run_id)


def test_a_named_group_hold_never_sends_a_node_release() -> None:
    owner, _task, scope, interventions, run_id, _paused = _two_group_holds(
        None, layout=(("arm_move", ["arm"]), ("free_move", None))
    )
    arm = _hold(interventions, run_id, "arm_move")
    owner.retry_failure_pause(
        run_id,
        node_attempt_id=arm.identity.node_attempt_id,
        action=None,
        request_id="retry.arm",
        reason=None,
    )
    assert scope.last_selection_release["group_ids"] == ["arm"]
    assert "node_instance_id" not in scope.last_selection_release
    owner.stop(run_id)


def test_continue_closes_the_held_operation_and_the_node_fails_to_the_tree() -> None:
    owner, task, scope, interventions, run_id, _paused = _two_group_holds(
        None, success_policy="one"
    )
    arm = _hold(interventions, run_id, "arm_move")

    owner.continue_failure_pause(
        run_id,
        node_attempt_id=arm.identity.node_attempt_id,
        request_id="continue.arm",
        reason=None,
    )
    assert task.closes == [(arm.owner_task_id, "continue.arm")]
    assert scope.last_selection_release["group_ids"] == ["arm"]
    owner.resolve_intervention(
        run_id, node_attempt_id=arm.identity.node_attempt_id, action="manual_failure"
    )
    starts = task.starts
    ticked = owner.advance_once(run_id)
    assert ticked.node_statuses["arm_move"] == "failure"
    assert task.starts == starts  # FAILURE to the tree, no new attempt.
    assert ticked.state is ProgramRunState.INTERVENTION_REQUIRED  # tool_move held
    owner.stop(run_id)


def test_continue_refused_when_the_held_operation_cannot_close() -> None:
    owner, task, scope, interventions, run_id, _paused = _two_group_holds(None)
    arm = _hold(interventions, run_id, "arm_move")
    task.close = lambda *_args, **_kwargs: False
    releases = scope.release_requests

    with pytest.raises(ProgramRunOwnerError, match="not eligible to close"):
        owner.continue_failure_pause(
            run_id,
            node_attempt_id=arm.identity.node_attempt_id,
            request_id="continue.refused",
            reason=None,
        )
    assert scope.release_requests == releases
    owner.stop(run_id)


class _FailedFactTaskPort(_TaskPort):
    def start(self, node_id, identity, inputs) -> OwnerTaskSnapshot:
        started = super().start(node_id, identity, inputs)
        return replace(
            started, metadata={**started.metadata, "operation_failure": _OPERATION_FACT}
        )


def test_a_failed_run_keeps_its_operation_failure_row_until_reset() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _FailedFactTaskPort(events, OwnerTaskState.FAILED),
    )
    run_id = owner.start(admission).capture.program_run_id
    failed = owner.advance_once(run_id)

    assert failed.state is ProgramRunState.FAILED
    (row,) = failed.to_dict()["operation_failures"]
    assert row["node_instance_id"] == "move_to_pick"
    assert row["failure"] == _OPERATION_FACT["failure"]
    assert row["operator_facing_run_revision"] <= failed.run_revision


def test_a_run_without_operation_failures_publishes_an_empty_list() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    # Always present: a missing key would be unknown provenance.
    assert owner.advance_once(run_id).to_dict()["operation_failures"] == []


class _MockRunRecorder:
    """Decision 135: the Run-wide Mock pause notifications the owner sends."""

    def __init__(self, events: list[str] | None = None) -> None:
        self.calls: list[tuple[Any, ...]] = []
        self.events = events

    def set_run_paused(self, program_run_id, run_revision, paused, held=()) -> None:
        self.calls.append(("pause", program_run_id, run_revision, paused, tuple(held)))
        if self.events is not None:
            self.events.append(f"mock_snapshot:{run_revision}:{paused}:{','.join(held)}")

    def end(self, start_request) -> None:  # pragma: no cover - no Mock nodes here
        self.calls.append(("end", start_request))

    def resend_unknown_ends(self, program_run_id):
        return ()

    def resend_run_pause(self, program_run_id):
        return (), None


def test_mock_run_pause_follows_run_start_halt_and_resume_but_not_teardown():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    mock = _MockRunRecorder()
    owner._ports = replace(owner._ports, mock_simulation=mock)
    scope.ready = True
    scope.node_count = 3
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    # Recorded (the port sends it only before the first Mock START), with the
    # actual mask: never-started (Ready) nodes are held.
    initial_mask = owner._entry(run_id).run.mock_held_nodes()
    assert mock.calls == [("pause", run_id, started.run_revision, False, initial_mask)]
    owner.advance_once(run_id)
    owner.full_stop(run_id)
    assert mock.calls[-1][2:] and mock.calls[-1][3] is True
    halt_revision = mock.calls[-1][2]
    assert halt_revision > started.run_revision
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    assert mock.calls[-1][:4] == ("pause", run_id, resumed.run_revision, False)
    assert resumed.run_revision > halt_revision
    count = len(mock.calls)
    owner.stop(run_id)  # Teardown ends nodes (END), never a pause state.
    assert len(mock.calls) == count


def test_a_run_without_a_mock_port_runs_through_halt_resume_and_stop():
    """SX-2416: no Mock nodes, no Mock port, no I/O service: no Mock call."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    assert owner._ports.mock_simulation is None
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.full_stop(run_id)
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    assert resumed.state is ProgramRunState.RUNNING
    owner.advance_once(run_id)
    owner.stop(run_id)


def test_an_unconfirmed_mock_end_holds_the_run_short_of_terminal_until_unbind_succeeds():
    """SX-2433/2434: the native unbind fails while a Mock END is unanswered.
    The Run stays FINALIZING (not terminal: no terminal record, readiness or
    claim release can follow), and the finalizing tick re-enters unbind."""
    from manyforge_behavior.production_ports import ProductionPortError

    class _PendingEndContext(_ContextPort):
        pending_ends = 2  # Unbind attempts that still find an unanswered END.

        def unbind(self, program_run_id: str) -> None:
            super().unbind(program_run_id)
            if self.pending_ends:
                self.pending_ends -= 1
                raise ProductionPortError(
                    "cannot unbind a Program run with unconfirmed Mock simulation ends"
                )

    events: list[str] = []
    context_port = _PendingEndContext(events)
    owner, admission, _task, _scope, _fixture_events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        context_port=context_port,
    )
    run_id = owner.start(admission).capture.program_run_id
    for _ in range(2):
        held = owner.advance_once(run_id)
        assert held.state is ProgramRunState.FINALIZING
        assert held.reason_code == "program_run_cleanup_failed"
        assert "unconfirmed Mock simulation ends" in held.message
    completed = owner.advance_once(run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert context_port.unbound == [run_id] * 3



def _mock_owner(initial_task_state=OwnerTaskState.ACTIVE):
    owner, admission, task, scope, events = _fixture(initial_task_state=initial_task_state)
    mock = _MockRunRecorder(events)
    owner._ports = replace(owner._ports, mock_simulation=mock)
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    run = owner._entry(run_id).run
    mask = {"value": ()}
    run.mock_held_nodes = lambda: mask["value"]
    return owner, run_id, run, mock, mask, events


def test_a_changed_held_mask_takes_a_new_revision_before_the_ticks_starts():
    owner, run_id, run, mock, mask, events = _mock_owner()
    before = owner.read(run_id).run_revision
    selection = run.operator_stop_scope().selection_revision
    mask["value"] = ("move_to_pick",)  # E.g. a Faulted settlement: no new
    starts_before = sum(event == "leaf_start" for event in events)  # selection revision.
    events.append("tick")
    after = owner.advance_once(run_id)
    snapshot_revision = mock.calls[-1][2]
    assert mock.calls[-1] == ("pause", run_id, snapshot_revision, False, ("move_to_pick",))
    assert before < snapshot_revision <= after.run_revision
    assert run.operator_stop_scope().selection_revision == selection
    tick = events[events.index("tick"):]
    snapshot_at = tick.index(f"mock_snapshot:{snapshot_revision}:False:move_to_pick")
    assert all(event != "leaf_start" for event in tick[:snapshot_at])
    assert sum(event == "leaf_start" for event in events) >= starts_before
    count = len(mock.calls)
    owner.advance_once(run_id)  # Same mask: no new revision, nothing sent.
    assert len(mock.calls) == count


def test_a_released_mask_shrinks_with_a_higher_revision():
    owner, run_id, _run, mock, mask, _events = _mock_owner()
    mask["value"] = ("move_to_pick", "remove_table")
    owner.advance_once(run_id)
    held_revision = mock.calls[-1][2]
    mask["value"] = ("remove_table",)  # A selected release / Retry published.
    owner.advance_once(run_id)
    assert mock.calls[-1][3:] == (False, ("remove_table",))
    assert mock.calls[-1][2] > held_revision


def test_halt_and_a_held_node_are_both_carried():
    owner, run_id, _run, mock, mask, _events = _mock_owner()
    mask["value"] = ("move_to_pick",)
    owner.advance_once(run_id)
    owner.full_stop(run_id)
    assert mock.calls[-1][3:] == (True, ("move_to_pick",))  # Both causes.
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    # Clearing the Halt keeps the hold.
    assert mock.calls[-1] == ("pause", run_id, resumed.run_revision, False, ("move_to_pick",))
    mask["value"] = ()
    owner.advance_once(run_id)
    assert mock.calls[-1][3:] == (False, ())
    revisions = [call[2] for call in mock.calls]
    assert revisions == sorted(set(revisions))  # One ordering: strictly increasing.


def test_a_mask_change_during_a_halt_keeps_the_mock_paused():
    """A Halt landing while a tick derives a changed mask: the mask-only
    snapshot stays paused; the resume then carries paused=false and it."""
    owner, run_id, run, mock, mask, _events = _mock_owner()

    def halt_meanwhile():
        entry = owner._entry(run_id)
        if entry.state is ProgramRunState.RUNNING and mask["value"]:
            entry.state = ProgramRunState.FULL_STOPPING  # A concurrent Halt.
        return mask["value"]

    run.mock_held_nodes = halt_meanwhile
    mask["value"] = ("move_to_pick",)  # E.g. a failed Stop's Faulted settlement.
    owner.advance_once(run_id)
    assert mock.calls[-1][3:] == (True, ("move_to_pick",))
    # The selection Stop and release paths re-check RUNNING under the same
    # lock, so they never publish during a Halt; the helper covers them too.
    entry = owner._entry(run_id)
    for state, paused in ((ProgramRunState.FULL_STOPPED, True),
                          (ProgramRunState.FULL_STOPPING, True),
                          (ProgramRunState.RUNNING, False)):
        entry.state = state
        assert owner._mock_paused(entry) is paused
    entry.state = ProgramRunState.FULL_STOPPING
    assert owner.reconcile_full_stop(run_id).state is ProgramRunState.FULL_STOPPED
    resumed = owner.resume_full_stop(run_id, request_id="execute-after-halt")
    assert mock.calls[-1] == ("pause", run_id, resumed.run_revision, False, ("move_to_pick",))
