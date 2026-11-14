"""Exact Program-run admission, lifecycle, and Stop-priority contracts."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from copy import deepcopy
from dataclasses import replace
import itertools
from pathlib import Path
import threading
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
from manyforge_behavior.request_keys import (
    KeyClass,
    Minter,
    RUNNER_KEYS,
    format_request_key,
    key_class,
    key_origin,
    request_key,
)
from manyforge_behavior import program_scheduling
from manyforge_behavior.program_scheduling import (
    PROGRAM_TICKS_LATE_CODE,
    ProgramRunHoldFacts,
    ProgramInterventionRegistry,
    ProgramRunTickHost,
    ProgramRunTickOverruns,
    ProgramSchedulingError,
)
from manyforge_behavior.program import Program, program_node_policies
from manyforge_behavior.program_run_owner import (
    PROGRAM_RESET_STOP_REASON,
    ProgramRunAdmission,
    ProgramRunOwner,
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
    ProgramRunSnapshot,
    ProgramRunState,
    build_applied_program_run_admission,
    build_program_run_admission,
    program_run_end,
)
from manyforge_behavior.production_ports import NativeKernelProgramPort
from manyforge_behavior import program_runtime
from manyforge_behavior.program_runtime import ProgramRunOwnerIdentity
from manyforge_behavior.run_sender import RunSender
from manyforge_program_runner.runtime_controls import ProgramRuntimeControls


_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_APPLICATION_START_ID = "0123456789abcdef"
_CLIENT_KEYS = itertools.count(1)


@pytest.fixture(autouse=True)
def _sender_in_place(monkeypatch: pytest.MonkeyPatch) -> None:
    """Each run's sender (OD-213) makes its requests in place, in order, at
    submission instead of on a thread of its own: a node's request is then
    answered before the next tick, which reads it. A run ends on the tick
    after the one that sent its last request, never by thread timing."""

    def sender(capacity: int, *, name: str = "manyforge-program-sender") -> RunSender:
        return RunSender(capacity, name=name, spawn=lambda _name, work: work())

    monkeypatch.setattr(program_runtime, "RunSender", sender)


def _scene_step_sent(owner: ProgramRunOwner, program_run_id: int) -> ProgramRunSnapshot:
    """The example Program's tick that ends its motion leaf: its Scene leaf
    (remove_table) then sends its request on the run's sender and is RUNNING
    until the answer (OD-213 (4)), so the run is still RUNNING. The next tick
    reads the answer and ends the tree."""

    snapshot = owner.advance_once(program_run_id)
    assert snapshot.state is ProgramRunState.RUNNING
    assert snapshot.node_statuses["move_to_pick"] == "success"
    assert snapshot.node_statuses["remove_table"] == "running"
    return snapshot


def _host_scene_step_sent(host: ProgramRunTickHost) -> None:
    """`_scene_step_sent` for each run a manual host tick advances."""

    snapshots = host.advance_ready_once()
    assert snapshots
    for snapshot in snapshots:
        assert snapshot.state is ProgramRunState.RUNNING
        assert snapshot.node_statuses["remove_table"] == "running"


def _stop(owner: ProgramRunOwner, program_run_id: int, **kwargs: Any):
    """The runner's own Forced Program End as the run's tick applies it: the
    Stop is marked and sent, then its tree part runs."""

    return owner.finish_stop(program_run_id, owner.send_stop(program_run_id, **kwargs))


def _on_tick(host: ProgramRunTickHost, command: Callable[[], Any]) -> Any:
    """A host command handed to the run's tick (OD-146), in manual tick mode:
    its request waits on a thread; the test ticks the host once the command
    is posted. Returns the command's result or raises its error."""

    owner = host._owner  # noqa: SLF001
    posted = Event()
    post = owner.post_to_tick

    def post_and_note(*args):
        intent = post(*args)
        posted.set()
        return intent

    owner.post_to_tick = post_and_note
    outcome: dict[str, Any] = {}

    def request() -> None:
        try:
            outcome["result"] = command()
        except Exception as error:
            outcome["error"] = error

    thread = Thread(target=request)
    try:
        thread.start()
        while not posted.wait(0.01):
            if not thread.is_alive():
                break
        if posted.is_set():
            host.advance_ready_once()
        thread.join(5)
    finally:
        del owner.post_to_tick
    assert not thread.is_alive()
    if "error" in outcome:
        raise outcome["error"]
    return outcome["result"]


def _tick_end(owner: ProgramRunOwner, program_run_id: int) -> None:
    """The run's tick publishes its selection facts at its end (OD-146): a
    test that calls a tick-side owner method directly ends the tick here."""

    owner._publish_tick_facts(program_run_id)  # noqa: SLF001


def _key() -> bytes:
    """A fresh client request key (class 3), as Headless hands it on."""
    return request_key(KeyClass.HTTP_CLIENT, 1, next(_CLIENT_KEYS))


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
        api_version="manyforge.program.v3",
    )
    admission = _admission_for_program(
        admission, Program.from_dict(document, admission.environment.catalog), []
    )
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    run = owner._runs[run_id].run
    # The existing declared store is the producer, not a later History query.
    owner.write_runtime_blackboard(
        run_id, key="recorded_value", value=True, request_key=_key()
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
        "active_tcp/ur5_robot/arm": {
            "target": {"component_id": "ur5_robot", "command_target_id": "arm"},
            "frame": {"component_id": "ur5_robot", "frame_id": "tcp"}},
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
        self.unbound: list[int] = []

    def bind(self, context: ProgramRunPortContext) -> None:
        self.events.append("context_bind")
        self.contexts.append(context)

    def unbind(self, program_run_id: int) -> None:
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
        self.recoveries: list[tuple[int | str, bytes]] = []

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        self.observations += 1
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.INTERRUPTED,
            metadata={
                "stop_event_id": 1,
                "authored_failure_policy": "pause_for_operator",
                "authored_failure_policy_parameter": "operator_stop_policy",
                "recovery_assessment": {
                    "allowed_actions": ["replan_from_stopped_state"],
                    "recovery_class": self.recovery_class,
                    "status": "action_required",
                    "execution_state": "Stopped",
                    "stop_event_id": 1,
                },
            },
            reason_code="operation_stopped",
            message="native operation was stopped",
        )

    def recover(
        self,
        task_id: int | str,
        _identity: NodeAttemptIdentity,
        *,
        action: str,
        request_key: bytes,
        reason: str | None,
    ) -> dict[str, Any]:
        assert action == "replan_from_stopped_state"
        assert reason is None or isinstance(reason, str)
        self.recoveries.append((task_id, request_key))
        return {
            "request_key": format_request_key(request_key),
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
        if identity.program_run_id == 1:
            self._first_entered.set()
            assert self._release_first.wait(timeout=2.0)
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=(
                OwnerTaskState.SUCCEEDED
                if identity.program_run_id == 1
                else OwnerTaskState.ACTIVE
            ),
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        if task_id != "task-2":
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
        self.hold_revision = 0
        self.held = False
        self.ready = False
        self.selection_release_block_reason: str | None = None
        # The rule whose `hold` holds the run (Kernel scope field).
        self.stopped_by_rule_id: str | None = None
        self.attention_refusal: ProgramSelectionReleaseRejectedError | None = None
        # Kernel stand-in for the per-node resource test: a node is refused
        # only while an alarm gates one of its own target resources.
        self.alarmed_nodes: set[str] = set()
        self.attention_checks: list[dict] = []
        # The Kernel's frozen level of the run and the Halts it accepted.
        self.frozen = "none"
        self.halts = 0
        self.halt_releases: list[int] = []
        self.halt_refusal: ProgramSelectionReleaseRejectedError | None = None

    def bind(self, context: ProgramRunPortContext) -> None:
        pending = [context.program["tree"]]
        self.node_count = 0
        while pending:
            node = pending.pop()
            self.node_count += 1
            pending.extend(node.get("children", ()))

    def unbind(self, _program_run_id: str) -> None:
        pass

    def execution_scope(self, program_run_id: int) -> Mapping[str, Any]:
        return {
            "schema_version": "manyforge.program_execution_scope.v3",
            "claim_generation": 1,
            "program_run_id": program_run_id,
            "hold_revision": self.hold_revision,
            "frozen": self.frozen,
            "frozen_by": self.halts if self.frozen != "none" else None,
            "all_groups_held": False,
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
            "faulted_node_indices": (
                list(range(self.node_count))
                if self.held and self.state is ProgramRunScopeState.UNKNOWN
                else []
            ),
            "selection_release_block_reason": self.selection_release_block_reason,
            "stopped_by": "rule" if self.stopped_by_rule_id else None,
            "rule_id": self.stopped_by_rule_id,
        }

    def stop_selection(self, request):
        self.last_selection_stop = request
        self.request_stop(
            request["program_run_id"],
            bytes.fromhex(request["request_context"]["request_key"]),
            None,
        )
        if self.state is ProgramRunScopeState.UNKNOWN:
            raise RuntimeError("kernel_scope_unknown")
        return {
            "schema_version": "manyforge.program_execution_selection.v3",
            "state": _ScopePort.execution_scope(self, request["program_run_id"]),
            "selected_node_indices": [],
            "widened_node_indices": [],
            "stop": {"event_id": self.stop_requests, "replay_status": "accepted_new"},
        }

    def check_node_motion_attention(self, request):
        self.attention_checks.append(request)
        if (
            self.attention_refusal is not None
            and request["node_instance_id"] in self.alarmed_nodes
        ):
            raise self.attention_refusal

    def retire_held_preparations(
        self, program_run_id, selected_node_ids, expected_hold_revision,
    ):
        # This stand-in retains no accepted native preparations. Production
        # cleanup is exercised separately through NativeKernelProgramPort.
        return None

    def release_selection(self, request, expected_hold_revision):
        self.last_selection_release = request
        if self.attention_refusal is not None:
            raise self.attention_refusal
        if self.hold_revision != expected_hold_revision:
            raise ProgramSelectionReleaseRejectedError("selection revision changed")
        if self.state is not ProgramRunScopeState.SETTLED:
            raise ProgramSelectionReleaseRejectedError("selected scope has not settled")
        # Run/Resume releases the selection's Ready and Held work alike.
        self.release_requests += 1
        self.held = False
        self.ready = False
        self.hold_revision += 1
        self._events.append("scope_stop_release")
        return self.execution_scope(request["program_run_id"])

    def start_selection(self, request, expected_hold_revision):
        self.last_selection_start = request
        if self.hold_revision != expected_hold_revision:
            raise ProgramSelectionReleaseRejectedError("selection revision changed")
        if not self.ready or self.state is not ProgramRunScopeState.SETTLED:
            raise ProgramSelectionReleaseRejectedError("selected scope is not Ready")
        self.ready = False
        self.hold_revision += 1
        self._events.append("scope_motion_start")
        return self.execution_scope(request["program_run_id"])

    def _snapshot(self, program_run_id: int) -> ProgramRunScopeSnapshot:
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
        program_run_id: int,
        _stop_request_key: bytes,
        _reason: str | None,
    ) -> ProgramRunScopeSnapshot:
        self.stop_requests += 1
        self.last_stop_request = (program_run_id, _stop_request_key, _reason)
        # Like the Kernel, a Stop contains started work; never-started
        # (Ready) work keeps its Ready hold.
        if not self.ready:
            self.held = True
        self.hold_revision += 1
        self._events.append("scope_stop")
        return self._snapshot(program_run_id)

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return self._snapshot(program_run_id)

    def kernel_halt(self, frozen: str = "halt") -> None:
        """The Kernel accepts a Halt (or runner loss) for the run: it freezes
        the run and holds started work (Ready work keeps its Ready hold)."""

        if not self.ready:
            self.held = True
        self.frozen = frozen
        self.halts += 1
        self.hold_revision += 1
        self._events.append("kernel_halt")

    def release_halt(
        self, program_run_id: int, *, expected_hold_revision: int
    ) -> Mapping[str, Any]:
        # Releasing the freeze resumes execution only; like the Kernel, it
        # keeps every motion hold (held and Ready stay).
        if self.halt_refusal is not None:
            raise self.halt_refusal
        if expected_hold_revision != self.hold_revision:
            raise ProgramSelectionReleaseRejectedError(
                "hold revision changed", code="program_halt_release_stale"
            )
        self.release_requests += 1
        self.halt_releases.append(expected_hold_revision)
        self.frozen = "none"
        self.hold_revision += 1
        self._events.append("scope_stop_release")
        return self.execution_scope(program_run_id)

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


class _FixtureProgramRunOwner(ProgramRunOwner):
    """The runner always names the run number; the fixture stands in."""

    def __init__(self, *args, run_id_factory: Callable[[], int], **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._fixture_run_ids = run_id_factory

    def start(self, admission, *, program_run_id=None):
        snapshot = super().start(
            admission,
            program_run_id=(
                self._fixture_run_ids() if program_run_id is None else program_run_id
            ),
        )
        # The Runner normally registers the store and seeds it from the
        # Kernel's ordered Active TCP baseline before ticking the run.
        for row in admission.program_binding.get("target_bindings", ()):
            if row.get("robot_tcps"):
                target = row["target"]
                self.value_store(snapshot.capture.program_run_id).write_active_tcp(
                    target["component_id"], target["command_target_id"],
                    row["robot_tcps"][0]["entry"],
                )
        return snapshot


def _fixture(
    *,
    initial_task_state: OwnerTaskState,
    task_factory: Callable[[list[str]], _TaskPort] | None = None,
    context_port: _ContextPort | None = None,
    interventions: ProgramInterventionRegistry | None = None,
    run_id_factory: Callable[[], int] | None = None,
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
            "target_bindings": [{
                "target": {"component_id": "ur5_robot", "command_target_id": "arm"},
                "robot_tcps": [{"entry": {
                    "target": {"component_id": "ur5_robot", "command_target_id": "arm"},
                    "frame": {"component_id": "ur5_robot", "frame_id": "tcp"}},
                    "end_effector": True}],
            }],
        },
        readiness=_readiness(_DIGEST_A),
        owner_identities=(
            ProgramRunOwnerIdentity(
                role="kernel_operations",
                application_start_id=_APPLICATION_START_ID,
            ),
        ),
    )
    owner = _FixtureProgramRunOwner(
        owner_id="program-runner.cell-a",
        application_start_id=_APPLICATION_START_ID,
        ports=ports,
        run_id_factory=run_id_factory or (lambda: 1),
        maximum_retained_runs=maximum_retained_runs,
        program_clock_source=program_clock_source,
    )
    return owner, admission, task, scope, events


_UNSET_KIND = object()


def _facts(
    run_id: int,
    *,
    hold_revision: int,
    frozen: str = "none",
    frozen_by: int | None = None,
    all_groups_held: bool = False,
    held_by_rule: str | None = None,
    last_stop_event_id: int | None = None,
    stop_progress_sequence: int = 0,
    frozen_kind: str | None | object = _UNSET_KIND,
    frozen_source_label: str | None = "operator.halt",
) -> ProgramRunHoldFacts:
    """Kernel hold facts as the event stream's ``state`` carries them. A
    frozen run's kind defaults to its frozen level (Halt or runner loss)."""

    if frozen_kind is _UNSET_KIND:
        frozen_kind = None if frozen == "none" else frozen
    return ProgramRunHoldFacts.from_wire(
        {
            "program_run_id": run_id,
            "hold_revision": hold_revision,
            "frozen": frozen,
            "frozen_by": frozen_by,
            "all_groups_held": all_groups_held,
            "held_by_rule": held_by_rule,
            "frozen_kind": frozen_kind,
            "frozen_source_label": None if frozen_kind is None else frozen_source_label,
        },
        last_stop_event_id=last_stop_event_id,
        stop_progress_sequence=stop_progress_sequence,
    )


def _kernel_facts(scope: _ScopePort, run_id: int, **overrides: Any) -> ProgramRunHoldFacts:
    """The fake Kernel's current hold facts for the run."""

    values: dict[str, Any] = {
        "hold_revision": scope.hold_revision,
        "frozen": scope.frozen,
        "frozen_by": scope.halts if scope.frozen != "none" else None,
    }
    values.update(overrides)
    return _facts(run_id, **values)


def _halt(owner: ProgramRunOwner, run_id: int, *, frozen: str = "halt"):
    """The Kernel accepts a Halt for the run and the run mirrors it (memory
    only, no owner Stop call): the run is Halting."""

    scope = owner._scope  # noqa: SLF001
    scope.kernel_halt(frozen)
    owner._reconcile_hold_facts(run_id, _kernel_facts(scope, run_id))  # noqa: SLF001
    return owner.read(run_id)


def _host_halt(host: ProgramRunTickHost, scope: _ScopePort, run_id: int):
    """The Kernel accepts a Halt; its facts reach the host (stream thread)."""

    scope.kernel_halt()
    host.apply_hold_facts(_kernel_facts(scope, run_id))


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
        api_version="manyforge.program.v3",
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
    assert [identity.role for identity in started.capture.owner_identities] == [
        "kernel_operations",
        "program_runner",
    ]
    assert started.capture.node_policies == ()
    assert dict(started.capture.node_policy_by_instance_id) == {}

    sent = _scene_step_sent(owner, started.capture.program_run_id)
    assert sent.node_status_revision == 2
    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert completed.node_status_revision == 3
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
@pytest.mark.slow
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
    identifiers = iter((1, 2))
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

    _scene_step_sent(owner, first.capture.program_run_id)
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

    started = owner.start(admission, program_run_id=7)

    assert fixture_events[0] == "context_bind"
    assert len(context_port.contexts) == 1
    context = context_port.contexts[0]
    assert started.capture.program_run_id == 7
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
    _scene_step_sent(owner, started.capture.program_run_id)
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
            if context.program_run_id == 2:
                entered.set()
                assert release.wait(timeout=2.0)

    identifiers = iter((1, 2))
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

    _scene_step_sent(owner, started.capture.program_run_id)
    first = owner.advance_once(started.capture.program_run_id)
    assert first.state is ProgramRunState.FINALIZING
    assert first.reason_code == "program_run_cleanup_failed"
    assert context_port.unbound == [1]

    observed = owner.read(started.capture.program_run_id)
    assert observed.state is ProgramRunState.FINALIZING
    assert context_port.unbound == [1]

    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED
    assert completed.reason_code is None
    assert context_port.unbound == [1, 1]


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
    _scene_step_sent(owner, started.capture.program_run_id)
    completed = owner.advance_once(started.capture.program_run_id)
    assert completed.state is ProgramRunState.SUCCEEDED

    reset = owner.reset_effects(started.capture.program_run_id)

    assert reset.state is ProgramRunState.SUCCEEDED
    assert scope.reset_requests == 1
    assert events[-1] == "scope_reset"


def test_durable_reset_forgets_resident_tree_and_reuses_concurrent_capacity() -> None:
    sequence = iter(range(1, 9))
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        run_id_factory=lambda: next(sequence),
        maximum_retained_runs=1,
    )

    for _ in range(8):
        started = owner.start(admission)
        _scene_step_sent(owner, started.capture.program_run_id)
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

    stopped = _stop(owner, started.capture.program_run_id)
    assert stopped.state is ProgramRunState.STOPPED
    assert interventions.read(started.capture.program_run_id) is None


def test_tick_host_advances_each_scheduled_run_and_removes_terminal_work() -> None:
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)

    started = host.start_run(admission)
    _host_scene_step_sent(host)
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
    _host_scene_step_sent(host)
    terminal = host.advance_ready_once()[0]
    scope.state = ProgramRunScopeState.ACTIVE
    try:
        reset_key = _key()
        assert (
            owner.begin_reset_containment(run_id, stop_request_key=reset_key) == terminal
        )
        assert scope.last_stop_request == (
            run_id,
            reset_key,
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
    _scene_step_sent(owner, run_id)
    terminal = owner.advance_once(run_id)
    owner.begin_reset_containment(run_id, stop_request_key=_key())
    assert owner.reset_containment_settled(run_id)

    def lost_reply(*_args, **_kwargs):
        raise RuntimeError("response lost")

    scope.request_stop = lost_reply
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.begin_reset_containment(run_id, stop_request_key=_key())
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)
    assert owner.read(run_id) == terminal and task.starts == 1


@pytest.mark.parametrize("stop_state", ["partial", "failed"])
def test_terminal_reset_uncontained_stop_accepts_only_current_zero_sample(stop_state):
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    run_id = owner.start(admission).capture.program_run_id
    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.FAILED
    assert terminal.active_operation_count == 0
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = f"program_run_stop_{stop_state}"

    owner.begin_reset_containment(run_id, stop_request_key=_key())
    assert (
        owner._entry(run_id).reset_containment_pre_stop_active_operation_count == 0
    )
    assert owner.reset_containment_settled(run_id)
    owner.begin_reset_containment(run_id, stop_request_key=_key())
    assert (
        owner._entry(run_id).reset_containment_pre_stop_active_operation_count == 0
    )
    assert owner.reset_containment_settled(run_id)
    assert scope.stop_requests == 2


def test_program_run_owner_and_native_port_reset_partial_stop_with_intervention_operation():
    from .test_production_ports import _motion_test_port, _motion_inputs, _context, _thawed

    class ResetTransport:
        def __init__(self) -> None:
            self.operations: dict[int, dict[str, Any]] = {}
            self.resident: dict[int, bool] = {}
            self.stop_states: dict[int, str] = {}
            self.faulted_node_indices: list[int] = []
            self.controls: list[tuple[str, int]] = []
            self.released_halts: list[tuple[int, int]] = []

        def identity(self):
            return {
                "ok": True,
                "identity": {
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
            operation_id = len(self.operations) + 1
            self.operations[operation_id] = {
                "operation_id": operation_id,
                "operation_revision": 1,
                "program_run_id": body["program_run_id"],
                "state": "queued",
                "stop_event_id": None,
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
            event_id = len(self.stop_states) + 1
            self.stop_states[event_id] = "pending"
            return {
                "ok": True,
                "accepted": {
                    "request_key": body["request_context"]["request_key"],
                    "event_id": event_id,
                    "requested": "program_end",
                    "applied": "program_end",
                    "widen_reason": "none",
                    "hold_revision": event_id,
                    "program_run_id": body.get("program_run_id"),
                    "covered": [],
                    "stale_diagnostics": [],
                    "stale_total": 0,
                    "accepted_at_ns": 1,
                    "replay_status": "accepted_new",
                },
            }

        def read_program_execution_scope(self, program_run_id):
            return {
                "ok": True,
                "scope": {
                    "schema_version": "manyforge.program_execution_scope.v3",
                    "program_run_id": program_run_id,
                    "claim_generation": 1,
                    "hold_revision": len(self.stop_states),
                    "held_node_indices": [],
                    "ready_node_indices": [],
                    "stopping_node_indices": [],
                    "faulted_node_indices": list(self.faulted_node_indices),
                    "selection_release_block_reason": None,
                    "stopped_by": None,
                    "rule_id": None,
                    "frozen": "program_end" if self.stop_states else "none",
                    "frozen_by": len(self.stop_states) or None,
                    "all_groups_held": False,
                },
            }

        def release_halt(self, program_run_id, expected_hold_revision):
            self.released_halts.append((program_run_id, expected_hold_revision))
            return {"ok": True, "command": "release_halt", "scope": None}

    transport = ResetTransport()
    native = _motion_test_port(transport)
    fixture_owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    binding = deepcopy(dict(admission.program_binding))
    motion_binding = _context().program_binding
    for key in ("target_bindings", "frame_bindings", "signal_bindings", "collision_body_bindings"):
        binding[key] = _thawed(motion_binding[key])
    admission = replace(
        admission,
        program_binding=binding,
        owner_identities=(
            ProgramRunOwnerIdentity(
                role="kernel_operations",
                application_start_id=_APPLICATION_START_ID,
            ),
        ),
    )
    owner = ProgramRunOwner(
        owner_id="program-runner.cell-a",
        application_start_id=_APPLICATION_START_ID,
        ports=replace(
            fixture_owner._ports,
            program_run_scope=native.scope,
            program_run_context=native,
        ),
    )

    run_id = owner.start(admission, program_run_id=1).capture.program_run_id
    native_task = native.operations.start(
        "motion.move_to_pose",
        NodeAttemptIdentity(run_id, "held-motion", 1),
        _motion_inputs(),
    )
    operation_id = native_task.metadata["operation_id"]
    transport.operations[operation_id]["state"] = "intervention_required"
    assert native.operations.observe(native_task.task_id).state is OwnerTaskState.FAILED

    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.FAILED
    assert terminal.active_operation_count == 0

    owner.begin_reset_containment(run_id, stop_request_key=_key())
    # The Reset Stop ended with a participant uncontained: its node faulted
    # (one Kernel scope read; no read_stop in this path).
    transport.stop_states[1] = "complete"
    transport.faulted_node_indices = [0]
    owner._observe_reset_containment(run_id)
    assert owner._entry(run_id).reset_containment.reason_code == "program_run_stop_failed"
    assert owner.reset_containment_settled(run_id)

    reset = owner.reset_effects(run_id)
    assert reset.state is ProgramRunState.FAILED
    assert transport.operations[operation_id]["state"] == "intervention_required"
    assert transport.resident[operation_id] is False
    assert transport.controls == [("reset", operation_id)]
    # No Program-run Stop stays captured after Reset.
    assert run_id not in native._scope_stops  # noqa: SLF001
    retained = native._scope_stops.get(run_id)  # noqa: SLF001
    assert retained is None or retained.active is None
    assert transport.released_halts == []
    owner.forget_reset_run(run_id)


@pytest.mark.parametrize("pre_stop_count", [None, 1])
def test_terminal_reset_partial_stop_rejects_missing_or_nonzero_sample(pre_stop_count):
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    _scene_step_sent(owner, run_id)
    terminal = owner.advance_once(run_id)
    assert terminal.state is ProgramRunState.SUCCEEDED
    assert terminal.active_operation_count == 0
    with owner._lock:
        owner._entry(run_id).terminal_settlement_active_operation_count = pre_stop_count
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_partial"

    owner.begin_reset_containment(run_id, stop_request_key=_key())
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)


@pytest.mark.parametrize(
    ("scope_state", "reason_code"),
    [
        (ProgramRunScopeState.ACTIVE, None),
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
    _scene_step_sent(owner, run_id)
    owner.advance_once(run_id)
    scope.state = scope_state
    if reason_code is not None:
        scope.unknown_reason_code = reason_code

    owner.begin_reset_containment(run_id, stop_request_key=_key())
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
    owner.begin_reset_containment(run_id, stop_request_key=_key())
    assert owner.reset_containment_settled(run_id)

    def lost_reply(*_args, **_kwargs):
        raise RuntimeError("response lost")

    scope.request_stop = lost_reply
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.begin_reset_containment(run_id, stop_request_key=_key())
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.reset_containment_settled(run_id)


def test_reset_containment_does_not_change_a_running_program():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    started = owner.start(admission)

    with pytest.raises(ProgramRunOwnerError, match="requires a terminal Run"):
        owner.begin_reset_containment(
            started.capture.program_run_id, stop_request_key=_key()
        )

    assert scope.stop_requests == 0


def test_terminal_reset_waiter_uses_worker_notifications():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    run_id = host.start_run(admission).capture.program_run_id
    _host_scene_step_sent(host)
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
                host.contain_reset_run(run_id, stop_request_key=_key())
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


def test_terminal_reset_waiter_has_no_deadline_and_ends_at_shutdown():
    # OD-214: Reset waits for containment to settle with no time limit; an
    # unsettled run keeps it waiting until the application ends.
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    run_id = host.start_run(admission).capture.program_run_id
    _host_scene_step_sent(host)
    host.advance_ready_once()
    observed = Event()
    read = scope.observe

    def unsettled_scope(program_run_id):
        observed.set()
        return read(program_run_id)

    scope.state = ProgramRunScopeState.ACTIVE
    scope.observe = unsettled_scope
    errors = []

    def reset_wait():
        try:
            host.contain_reset_run(run_id, stop_request_key=_key())
        except Exception as error:
            errors.append(error)

    host.start()
    waiter = Thread(target=reset_wait)
    waiter.start()
    try:
        assert observed.wait(2.0)
        assert waiter.is_alive()
        host.shutdown()
        waiter.join(2.0)
        assert not waiter.is_alive()
        assert len(errors) == 1
        assert isinstance(errors[0], ProgramRunOwnerOutcomeUnknownError)
    finally:
        host.shutdown()
        waiter.join(2.0)


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
    _host_scene_step_sent(host)
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
    pause_key = bytes.fromhex(scope.last_selection_stop["request_context"]["request_key"])
    assert key_class(pause_key) is KeyClass.INTERNAL
    assert key_origin(pause_key) == Minter.RUNNER
    assert not owner.failure_pause_stop_pending(run_id)
    owner.advance_once(run_id)
    assert scope.stop_requests == 1
    _stop(owner, run_id)


class _SimulationPausedTaskPort(_TaskPort):
    """The Kernel refuses the first `refusals` starts: the simulation isn't
    playing; later starts succeed."""

    def __init__(self, events: list[str], refusals: int) -> None:
        super().__init__(events, OwnerTaskState.SUCCEEDED)
        self.refusals = refusals
        self.refused = 0

    def start(self, node_id, identity, inputs):
        if self.refused < self.refusals:
            self.refused += 1
            raise KernelOperationAdmissionRejectedError(
                "simulation_not_playing",
                "the simulation is not playing",
                {"reason": "simulation_not_playing"},
            )
        return super().start(node_id, identity, inputs)


def test_a_simulation_not_playing_refusal_is_the_operator_stop_until_run() -> None:
    """The refused node never fails. The owner sends the
    operator's Stop on its groups once; the node waits for Run; a Run while
    the simulation still isn't playing is refused and held again; a Run after
    it plays proceeds."""
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED,
        task_factory=lambda events: _SimulationPausedTaskPort(events, refusals=2),
    )
    run_id = owner.start(admission).capture.program_run_id

    def run_after_play(request_key: bytes) -> None:
        controls = owner.execution_selection_controls(run_id)
        assert controls["groups"][0]["resume_allowed"]
        owner.resume_motion(
            run_id,
            expected_hold_revision=controls["hold_revision"],
            request_key=request_key,
        )

    held = owner.advance_once(run_id)
    assert held.state is ProgramRunState.RUNNING
    assert held.reason_code is None
    assert task.refused == 1 and scope.stop_requests == 1
    simulation_key = bytes.fromhex(
        scope.last_selection_stop["request_context"]["request_key"]
    )
    assert key_class(simulation_key) is KeyClass.INTERNAL
    assert key_origin(simulation_key) == Minter.RUNNER
    held = owner.advance_once(run_id)
    assert held.node_statuses["move_to_pick"] == "held"
    # No polling and no Stop of its own again while held.
    owner.advance_once(run_id)
    assert task.refused == 1 and scope.stop_requests == 1 and task.starts == 0

    run_after_play(_key())
    again = owner.advance_once(run_id)
    assert again.state is ProgramRunState.RUNNING
    assert task.refused == 2 and scope.stop_requests == 2
    assert owner.advance_once(run_id).node_statuses["move_to_pick"] == "held"

    run_after_play(_key())
    for _ in range(20):
        snapshot = owner.advance_once(run_id)
        if snapshot.state is not ProgramRunState.RUNNING:
            break
    assert snapshot.state is ProgramRunState.SUCCEEDED
    assert task.starts >= 1 and scope.stop_requests == 2


class _RetryablePolicyTaskPort(_PolicyTaskPort):
    """Fails with a failure pause; records owner recovery of the failed step."""

    def __init__(self, events: list[str], scope_probe: Callable[[], bool]) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self.recoveries: list[tuple[str, str, bool]] = []
        self.accept_recovery = True
        self._held = scope_probe
        self.on_recover: Callable[[], None] | None = None

    def recover(self, task_id, _identity, *, action, request_key, reason):
        # Records whether motion was still held when recovery was prepared.
        self.recoveries.append((task_id, action, self._held()))
        if self.on_recover is not None:
            self.on_recover()
        return {
            "request_key": format_request_key(request_key),
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
        self.fresh_reads = 0

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

    def current_recovery_assessment(self, task_id: str, identity) -> dict[str, Any]:
        self.fresh_reads += 1
        return self._assessment()


@pytest.mark.parametrize("deadline_ns", [0, 1_000])
def test_failure_pause_stays_dormant_whatever_the_owner_deadline(deadline_ns) -> None:
    """Recovery offers do not expire, so a dormant failure pause never re-reads
    its owner assessment to keep a deadline current: freshness is checked when
    an action is pressed."""
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
    held = owner.advance_once(run_id)
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert task.observations == 0
    assert task.starts == 1  # the tree stayed dormant
    assert interventions.read(run_id).identity == failure.identity
    _stop(owner, run_id)


def test_press_time_failure_refresh_reads_the_owner_and_keeps_the_acknowledgement() -> None:
    """A manual-result press freshly re-reads the held task's
    owner assessment (offers do not expire), not its cached observation; the
    Program's non-moving acknowledgement stays."""
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _ObservationBoundFailureTaskPort(
            events, deadline_ns=1_000
        ),
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.INTERVENTION_REQUIRED
    failure = interventions.read(run_id)
    interventions.refresh_owner_assessment(
        failure.identity,
        {**failure.owner_assessment, "acknowledged_non_moving": True},
    )
    task.deadline_ns = 2_000
    refreshed = owner.refresh_failure_assessment(
        run_id, node_attempt_id=failure.identity.node_attempt_id
    )
    assert task.fresh_reads == 1 and task.observations == 0
    assert refreshed.identity == failure.identity
    assert refreshed.owner_assessment["observed_state_valid_until_ns"] == 2_000
    assert refreshed.owner_assessment["acknowledged_non_moving"] is True
    assert interventions.read(run_id).owner_assessment == refreshed.owner_assessment
    _stop(owner, run_id)


def test_failure_retry_recovers_while_held_then_releases_and_restarts() -> None:
    owner, task, scope, interventions, run_id, failure = _failure_paused_run()
    assert scope.held  # the failure pause stopped all Program motion
    releases_before = scope.release_requests

    owner.retry_failure_pause(
        run_id,
        node_attempt_id=failure.identity.node_attempt_id,
        action="retry_motion_from_current_state",
        request_key=_key(),
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
    _stop(owner, run_id)


def test_stop_during_failure_retry_wins_and_keeps_motion_held() -> None:
    owner, task, scope, _interventions, run_id, failure = _failure_paused_run()
    # A newer Stop lands while Retry prepares recovery.
    task.on_recover = lambda: setattr(scope, "hold_revision", scope.hold_revision + 1)

    with pytest.raises(ProgramRunOwnerRecoveryIncompleteError):
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id,
            action="retry_motion_from_current_state",
            request_key=_key(),
            reason=None,
        )
    assert scope.held
    assert owner.read(run_id).state is ProgramRunState.INTERVENTION_REQUIRED
    _stop(owner, run_id)


def test_rejected_failure_recovery_releases_nothing() -> None:
    owner, task, scope, _interventions, run_id, failure = _failure_paused_run()
    task.accept_recovery = False
    releases_before = scope.release_requests

    with pytest.raises(ProgramRunOwnerError, match="owner refused"):
        owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id,
            action="retry_motion_from_current_state",
            request_key=_key(),
            reason=None,
        )
    assert scope.release_requests == releases_before
    assert scope.held
    _stop(owner, run_id)


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
            request_key=_key(),
            reason=None,
        )

    scope.state = ProgramRunScopeState.SETTLED  # the retried Stop succeeds
    confirmed = owner.advance_once(run_id)
    assert "not confirmed" not in confirmed.message
    assert not owner.failure_pause_stop_pending(run_id)
    _stop(owner, run_id)


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
    _stop(owner, run_id)


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

    _on_tick(host, lambda: host.stop_run(started.capture.program_run_id))
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

    _on_tick(host, lambda: host.stop_run(started.capture.program_run_id))
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

    def controlled_advance(program_run_id: int, **kwargs):
        snapshot = advance_once(program_run_id, **kwargs)
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
                request_key=_key(),
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
            api_version="manyforge.program.v3",
        )
        admission = _admission_for_program(
            admission, Program.from_dict(document, admission.environment.catalog), []
        )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    started = host.start_run(admission)

    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    scope.state = ProgramRunScopeState.ACTIVE
    stopped = owner.stop_motion(
        started.capture.program_run_id,
        reason="ordinary operator motion Stop",
        group_id=groups,
        stop_request_key=_key(),
    )
    host.schedule(started.capture.program_run_id)
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
    resumed = _on_tick(host, lambda: host.resume_motion(
        started.capture.program_run_id,
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
        group_id=groups,
        reason=None,
    ))
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.release_requests == 1
    assert scope.last_selection_release["group_ids"] == expected_groups
    assert len(task.recoveries) == 1
    # The tick that applied the Resume ticked the tree right after it.
    progressed = owner.read(started.capture.program_run_id)
    assert progressed.state is ProgramRunState.RUNNING
    assert "held" not in progressed.node_statuses.values()
    assert task.starts == 2

    _on_tick(host, lambda: host.stop_run(started.capture.program_run_id))
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
    owner.stop_motion(run_id, stop_request_key=_key())
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
            scope.hold_revision += 1
            scope.state = ProgramRunScopeState.ACTIVE
        return result

    task.recover = cleanup
    request = dict(
        node_indices=(node_index,),
        expected_run_revision=owner.read(run_id).run_revision,
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
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
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
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
@pytest.mark.slow
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
    owner.stop_motion(run_id, stop_request_key=_key())
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
            expected_hold_revision=scope.hold_revision
            - (invalid == "stale_revision"),
            request_key=_key(),
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
    owner.stop_motion(run_id, stop_request_key=_key())
    host.schedule(run_id)
    assert host.advance_ready_once()[0].state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(run_id)
    assert intervention is not None
    scope.state = ProgramRunScopeState.SETTLED
    recover = task.recover

    def accept_then_stop(*args, **kwargs):
        result = recover(*args, **kwargs)
        if failure == "new_program_stop":
            scope.state = ProgramRunScopeState.ACTIVE
            _stop(owner, run_id, stop_request_key=_key())
        elif failure == "changed_intervention":
            interventions.release(run_id)
            interventions.register_run(run_id, [intervention.identity.node_instance_id])
            interventions.require(
                NodeAttemptIdentity(
                    run_id, intervention.identity.node_instance_id, 999
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
            _on_tick(host, lambda: host.resume_motion(
                run_id,
                expected_hold_revision=scope.hold_revision,
                request_key=_key(),
                reason=None,
            ))
        assert len(task.recoveries) == 1
        assert scope.release_requests == 0
        if failure == "new_program_stop":
            assert owner.read(run_id).state is not ProgramRunState.RUNNING
        assert owner.operator_stop_scope(run_id).held_nodes
        assert task.starts == 1
    finally:
        scope.state = ProgramRunScopeState.SETTLED
        _on_tick(host, lambda: host.stop_run(run_id))
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
        owner.stop_motion(run_id, stop_request_key=_key())
        host.schedule(run_id)
    # The task reports a current offer, but the aggregate still has work.
    assert host.advance_ready_once()[0].state is ProgramRunState.INTERVENTION_REQUIRED
    intervention = interventions.read(run_id)
    assert intervention is not None
    if program_scope:
        with pytest.raises(ProgramRunOwnerError, match="still stopping"):
            _on_tick(host, lambda: host.resume_motion(
                run_id,
                expected_hold_revision=scope.hold_revision,
                request_key=_key(),
                reason=None,
            ))
        assert not task.recoveries and scope.release_requests == 0
        scope.state = ProgramRunScopeState.SETTLED
    if program_scope:
        assert (
            _on_tick(host, lambda: host.resume_motion(
                run_id,
                expected_hold_revision=scope.hold_revision,
                request_key=_key(),
            )).state
            is ProgramRunState.RUNNING
        )
        assert scope.release_requests == 1
    else:
        assert owner.apply_owner_recovery(
            run_id,
            node_attempt_id=intervention.identity.node_attempt_id,
            action="replan_from_stopped_state",
            request_key=_key(),
            reason=None,
        )["accepted"]
        assert scope.state is ProgramRunScopeState.ACTIVE
        assert scope.release_requests == 0
    assert len(task.recoveries) == 1
    scope.state = ProgramRunScopeState.SETTLED
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_explicit_stop_recaptures_a_lost_reply_through_the_native_scope():
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
    # This test's own Kernel stand-in for the Forced Program End and the one
    # scope read that settles it (host body exactly {request_context,
    # program_run_id}; the accepted reply names the hold revision).
    sent: list[dict] = []
    kernel = {"hold_revision": 0, "stopping": [0]}

    def stop_program_run(body):
        assert set(body) == {"request_context", "program_run_id"}
        sent.append(body)
        kernel["hold_revision"] += 1
        return {"ok": True, "command": "stop_program_run", "accepted": {
            "request_key": body["request_context"]["request_key"],
            "event_id": len(sent), "hold_revision": kernel["hold_revision"],
            "replay_status": "accepted_new", "program_run_id": run_id,
            "requested": "program_end", "applied": "program_end",
            "widen_reason": "none", "covered": [], "stale_diagnostics": [],
            "stale_total": 0, "accepted_at_ns": 1}}

    def read_program_execution_scope(program_run_id):
        return {"ok": True, "scope": {
            "schema_version": "manyforge.program_execution_scope.v3",
            "claim_generation": 1, "program_run_id": program_run_id,
            "hold_revision": kernel["hold_revision"], "ready_node_indices": [],
            "held_node_indices": [], "stopping_node_indices": list(kernel["stopping"]),
            "faulted_node_indices": [], "selection_release_block_reason": None,
            "stopped_by": None, "rule_id": None, "frozen": "program_end",
            "frozen_by": len(sent) or None, "all_groups_held": False}}

    def lost(body):
        stop_program_run(body)
        raise OSError("native Stop accepted; response lost")

    transport.read_program_execution_scope = read_program_execution_scope
    transport.stop_program_run = lost
    first = _stop(owner, run_id, stop_request_key=RUNNER_KEYS.next_key())
    assert first.state is ProgramRunState.STOPPING
    assert native.scope.observe(run_id).state is ProgramRunScopeState.UNKNOWN
    owner.advance_once(run_id)  # passive reconciliation must not retry an effect
    assert len(sent) == 1
    transport.stop_program_run = stop_program_run
    _stop(owner, run_id, stop_request_key=RUNNER_KEYS.next_key())
    assert len(sent) == 2
    assert native.scope.observe(run_id).state is ProgramRunScopeState.ACTIVE
    kernel["stopping"] = []  # the Kernel contained the run
    settled = owner.advance_once(run_id)
    assert settled.state is ProgramRunState.STOPPED
    assert native.scope.reset(run_id).state is ProgramRunScopeState.SETTLED
    assert not native._scope_stops


def test_halt_overtakes_a_held_execute_without_releasing_the_newer_halt():
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    _host_halt(host, scope, run_id)
    assert host.advance_ready_once()[0].state is ProgramRunState.HALTED
    # A second Halt lands in the Kernel before its facts reach the run:
    # Execute releases at the revision of the facts the run applied, so the
    # Kernel refuses it as stale and never releases the newer Halt unseen.
    scope.kernel_halt()
    with pytest.raises(ProgramRunOwnerError) as refused:
        _on_tick(host, lambda: host.resume_halt(run_id))
    assert getattr(refused.value, "code", None) == "program_halt_release_stale"
    assert scope.halts == 2 and scope.stop_requests == 0
    assert scope.release_requests == 0 and scope.frozen == "halt"
    assert task.starts == 1
    assert owner.read(run_id).state is ProgramRunState.HALTED
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_halt_never_waits_for_a_scheduling_scope_read():
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

    def halt(done):
        results.append(_halt(owner, run_id))
        done.set()

    scope.execution_scope = held_read
    first = Thread(target=halt, args=(first_done,))
    reconcile = Thread(target=owner.reconcile_halting, args=(run_id,))
    second = Thread(target=halt, args=(second_done,))
    first.start()
    try:
        assert first_done.wait(timeout=0.5)
        assert not entered.is_set()  # entering Halting reads nothing
        reconcile.start()
        assert entered.wait(timeout=0.5)
        # A later Halt is mirrored while the Halting tick waits on its read.
        second.start()
        assert second_done.wait(timeout=0.5)
        assert scope.halts == 2 and scope.stop_requests == 0
        assert all(row.state is ProgramRunState.HALTING for row in results)
    finally:
        finish.set()
        for thread in (first, reconcile, second):
            if thread.ident is not None:
                thread.join(timeout=2)
                assert not thread.is_alive()
    # Halt on a Halting run changes nothing; the settled read completes it.
    assert owner.read(run_id).state is ProgramRunState.HALTED
    _stop(owner, run_id)


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
            owner.stop_motion(
                started.capture.program_run_id,
                reason="priority Stop while a tree dispatch is blocked",
                stop_request_key=_key(),
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
    _on_tick(host, lambda: host.stop_run(started.capture.program_run_id))
    host.shutdown()


def test_selection_status_never_settles_an_observed_completed_task():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.stop_motion(run_id, stop_request_key=_key())
    _tick_end(owner, run_id)
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
        expected_hold_revision=controls["hold_revision"],
        request_key=_key(),
    )
    _scene_step_sent(owner, run_id)
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

    def bound_target(component: str, resources: list[str]) -> dict[str, Any]:
        return {
            "target": {"component_id": component, "command_target_id": "arm"},
            "resource_role_bindings": [{"resource_role_id": "joints",
                                        "resource_ids": resources,
                                        "compiled_resource_names": resources}],
        }

    binding["target_bindings"] = [
        bound_target("ur5_robot", ["robot.joint_2", "robot.joint_1"]),
        bound_target("spare", ["spare.joint_1"]),
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
        expected_hold_revision=controls["hold_revision"],
        request_key=_key(),
    )
    assert released.state is ProgramRunState.RUNNING
    assert task.starts == 0
    assert events[-1] == "scope_stop_release"
    _tick_end(owner, run_id)
    running = owner.execution_selection_controls(run_id)["groups"][0]
    assert running["state"] == "running"
    assert not running["start_allowed"]
    assert running["stop_allowed"]


@pytest.mark.parametrize(
    "blocker",
    [
        "cell_motion_admission_closed",
        "program_run_admission_pending",
        "program_halt_active",
    ],
)
def test_selection_controls_project_the_release_blocker_the_run_last_read(blocker):
    owner, admission, _task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    started = owner.start(admission)
    run_id = started.capture.program_run_id
    hold_revision = scope.hold_revision
    initially_available = owner.execution_selection_controls(run_id)["groups"][0]
    assert initially_available["start_allowed"] is True
    assert initially_available["resume_allowed"] is True
    scope.selection_release_block_reason = blocker
    # Shown once the run reads its scope again (a hold change wakes it).
    assert owner.execution_selection_controls(run_id)["groups"][0]["start_allowed"]
    owner.refresh_selection_scope(run_id)
    _tick_end(owner, run_id)

    group = owner.execution_selection_controls(run_id)["groups"][0]

    assert group["state"] == "ready"
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == blocker
    assert group["resume_unavailable_reason"] == blocker
    assert group["stop_allowed"] is False
    assert scope.hold_revision == hold_revision
    assert scope.release_requests == 0
    assert "scope_stop_release" not in events

    # A cleared blocker is shown after the run's next scope read.
    scope.selection_release_block_reason = None
    owner.refresh_selection_scope(run_id)
    _tick_end(owner, run_id)
    recovered = owner.execution_selection_controls(run_id)["groups"][0]
    assert recovered["state"] == "ready"
    assert recovered["start_allowed"] is True
    assert recovered["resume_allowed"] is True
    assert scope.hold_revision == hold_revision
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
    revision = scope.hold_revision
    scope.selection_release_block_reason = blocker
    # The run's own scope read refuses the malformed blocker.
    with pytest.raises(Exception):
        owner.refresh_selection_scope(run_id)
    _tick_end(owner, run_id)

    snapshot, controls = owner.read_selection_controls(run_id)
    group = controls["groups"][0]

    assert snapshot == baseline_snapshot
    assert baseline_group["state"] == "ready"
    assert group["state"] == "unavailable"
    assert group["start_allowed"] is False
    assert group["resume_allowed"] is False
    assert group["start_unavailable_reason"] == "program_selection_observation_unavailable"
    assert group["resume_unavailable_reason"] == "program_selection_observation_unavailable"
    assert controls["motion_held"] is False
    assert scope.hold_revision == revision
    assert scope.release_requests == 0


def test_status_read_keeps_the_last_facts_while_the_kernel_is_unreadable():
    """OD-152 (a): the status read asks the Kernel nothing."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    run_id = owner.start(admission).capture.program_run_id
    baseline = owner.read_selection_controls(run_id)
    assert baseline[1]["groups"][0]["start_allowed"] is True

    def unreadable(_program_run_id):
        raise RuntimeError("temporary Kernel scope read failure")

    scope.execution_scope = unreadable
    assert owner.read_selection_controls(run_id) == baseline
    assert scope.release_requests == 0


def test_selection_release_blocker_does_not_disable_stop_for_running_group():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.selection_release_block_reason = "cell_motion_admission_closed"
    owner.refresh_selection_scope(run_id)
    _tick_end(owner, run_id)

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
        scope.hold_revision += 1
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
    assert group["ordinary_held_node_count"] == 0
    assert controls["motion_held"] is True

    def published(current: dict[str, list[int]]) -> dict[str, Any]:
        phases.clear()
        phases.update(current)
        owner.stop_motion(run_id, stop_request_key=_key())
        _tick_end(owner, run_id)
        return owner.execution_selection_controls(run_id)

    # Run, then a whole-Program Stop that settled: every effectful node is
    # Held while control flow keeps running, so `all` is only `mixed`.
    stopped = published({"held": [0, 1]})
    assert stopped["groups"][0]["state"] == "mixed"
    assert stopped["groups"][0]["ordinary_held_node_count"] == 2
    assert stopped["motion_held"] is True
    # Never-started and stopped work alike stay held.
    mixed = published({"ready": [0], "held": [1]})
    assert mixed["motion_held"] is True
    assert mixed["groups"][0]["ordinary_held_node_count"] == 1
    # Still stopping, faulted, or one effectful node released: not held.
    stopping = published({"held": [0], "stopping": [1]})
    assert stopping["motion_held"] is False
    assert stopping["groups"][0]["ordinary_held_node_count"] is None
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
    }
    scope.attention_refusal = ProgramSelectionReleaseRejectedError(
        "an active operator-alarm condition interlock gates this motion",
        code="operator_alarm_interlock_active",
        details=details,
    )
    scope.alarmed_nodes = {"move_to_pick"}
    check_key = _key()
    with pytest.raises(ProgramRunOwnerError) as checked:
        owner.check_motion_attention(run_id, "move_to_pick", check_key)
    assert checked.value.code == "operator_alarm_interlock_active"
    assert checked.value.details == details
    check = scope.attention_checks[-1]
    assert check["schema_version"] == "manyforge.program_node_motion_attention_check.v2"
    assert check["node_instance_id"] == "move_to_pick"
    assert check["program_run_id"] == run_id
    assert check["request_context"] == {
        "request_key": format_request_key(check_key),
        "source_label": "program_recovery_control",
    }

    controls = owner.execution_selection_controls(run_id)
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.start_motion(
            run_id,
            expected_hold_revision=controls["hold_revision"],
            request_key=_key(),
        )
    assert refused.value.code == "operator_alarm_interlock_active"
    assert scope.ready
    assert "scope_stop_release" not in events

    scope.attention_refusal = None
    owner.check_motion_attention(run_id, "move_to_pick", _key())
    released = owner.start_motion(
        run_id,
        expected_hold_revision=controls["hold_revision"],
        request_key=_key(),
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
        "an active operator-alarm condition interlock gates this motion",
        code="operator_alarm_interlock_active",
        details={"reason": "operator_alarm_interlock_active"},
    )
    # Robot B's alarm (another node's resources): the held node continues.
    scope.alarmed_nodes = {"remove_table"}
    owner.check_motion_attention(run_id, "move_to_pick", _key())
    assert [row["node_instance_id"] for row in scope.attention_checks] == [
        "move_to_pick"
    ]
    # The held node's own alarm refuses with the Kernel's code.
    scope.alarmed_nodes = {"move_to_pick"}
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.check_motion_attention(run_id, "move_to_pick", _key())
    assert refused.value.code == "operator_alarm_interlock_active"
    # A node outside the capture is refused before any Kernel call.
    with pytest.raises(ProgramRunOwnerError):
        owner.check_motion_attention(run_id, "not_a_node", _key())
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
    assert _halt(owner, run_id).state is ProgramRunState.HALTING
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED

    resumed = owner.resume_halt(run_id)
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
        expected_hold_revision=controls["hold_revision"],
        request_key=_key(),
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

    released = _on_tick(host, lambda: host.start_motion(
        started.capture.program_run_id,
        expected_hold_revision=controls["hold_revision"],
        request_key=_key(),
        group_id="all",
        reason="operator initial motion Run",
    ))

    assert released.run_revision == started.run_revision + 1
    # The tick that applied the release ticked the tree right after it.
    assert events.index("scope_stop_release") < events.index("leaf_start")
    assert task.starts == 1
    assert observed[-1].run_revision >= released.run_revision
    assert started.capture.program_run_id in host._scheduled  # noqa: SLF001
    _on_tick(host, lambda: host.stop_run(started.capture.program_run_id))
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
        _scene_step_sent(owner, run_id)
        terminal.append(owner.advance_once(run_id))
        return accepted

    scope.stop_selection = complete_before_reply
    # The Kernel accepted the Stop; the run ended meanwhile: the reply is its
    # current state, not an error.
    replied = owner.stop_motion(run_id, stop_request_key=_key())
    assert replied == terminal[0]
    assert terminal[0].state is ProgramRunState.SUCCEEDED
    assert terminal[0].active_operation_count == 0
    assert owner.read(run_id) == terminal[0]


def test_selection_projection_uses_retained_owner_assessment_without_owner_io():
    interventions = ProgramInterventionRegistry(maximum_retained_runs=1)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    owner.stop_motion(run_id, stop_request_key=_key())
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
    stopped = owner.stop_motion(run_id, stop_request_key=_key())
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
    owner.stop_motion(run_id, stop_request_key=_key())
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.SETTLED
    resumed = owner.resume_motion(
        run_id,
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
    )
    assert owner.advance_once(run_id).run_revision == resumed.run_revision
    assert task.starts == 1


def test_rejected_resume_does_not_publish_unrevisioned_settlement():
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id, stop_request_key=_key())
    stopping = owner.advance_once(run_id)
    before = owner.operator_stop_scope(run_id)
    assert before.stopping_nodes
    scope.state = ProgramRunScopeState.SETTLED
    with pytest.raises(ProgramRunOwnerError, match="stale selection"):
        owner.resume_motion(
            run_id,
            expected_hold_revision=scope.hold_revision - 1,
            request_key=_key(),
        )
    assert owner.operator_stop_scope(run_id) == before
    assert owner.read(run_id) == stopping
    settled = owner.advance_once(run_id)
    assert settled.run_revision == stopping.run_revision + 1
    assert not owner.operator_stop_scope(run_id).stopping_nodes
    assert owner.advance_once(run_id) == settled
    assert task.starts == 0 and scope.release_requests == 0


def test_resume_crossed_by_a_run_revision_change_is_still_admitted():
    """Run/Resume is fenced on the selection, not on the run revision."""
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission, defer_ticks=True).capture.program_run_id
    owner.stop_motion(run_id, stop_request_key=_key())
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.SETTLED
    thread, outcome = _post(
        host,
        lambda: host.resume_motion(
            run_id, expected_hold_revision=scope.hold_revision, request_key=_key()
        ),
    )
    # The run revision moves after the Resume was posted, before its tick.
    owner.note_runtime_control_change(run_id)
    host.advance_ready_once()
    _answer(thread, outcome)
    assert scope.release_requests == 1
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_pending_selection_read_failure_does_not_fail_the_tree_or_poll_after_settlement():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id, stop_request_key=_key())
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
    assert reads == 1  # The status read asks the Kernel nothing (OD-152 (a)).
    scope.state = ProgramRunScopeState.SETTLED
    settled = owner.advance_once(run_id)
    assert reads == 2
    assert owner.execution_selection_controls(run_id)["groups"][0]["state"] == "held"
    assert reads == 2
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
    assert owner.stop_motion(run_id, stop_request_key=_key()).state is ProgramRunState.RUNNING
    _tick_end(owner, run_id)
    assert task.starts == 0
    controls = owner.execution_selection_controls(run_id)
    assert controls["groups"][0]["state"] == "stopping"
    assert not controls["groups"][0]["resume_allowed"]
    with pytest.raises(ProgramRunOwnerError, match="still stopping"):
        owner.resume_motion(
            run_id,
            request_key=_key(),
            expected_hold_revision=scope.hold_revision,
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
            request_key=_key(),
            expected_hold_revision=scope.hold_revision,
        ).state
        is ProgramRunState.RUNNING
    )
    _tick_end(owner, run_id)
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
    owner.stop_motion(run_id, stop_request_key=_key())
    host.schedule(run_id)
    host.advance_ready_once()
    revision = scope.hold_revision
    release = scope.release_selection

    def fail_release(request, expected_revision):
        if failure == "lost_reply":
            release(request, expected_revision)
            raise TimeoutError("release response lost")
        if failure == "new_stop":
            scope.request_stop(run_id, _key(), None)
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
            _on_tick(host, lambda: host.resume_motion(
                run_id,
                expected_hold_revision=revision,
                request_key=_key(),
            ))
        assert len(task.recoveries) == 1
        assert interventions.read_all(run_id) == ()
        assert owner.read(run_id).state is ProgramRunState.RUNNING
        host.advance_ready_once()
        assert task.starts == 1  # Unknown release never opens the local view.
        scope.release_selection = release
        if failure != "unchanged_reply":
            with pytest.raises(ProgramRunOwnerError, match="stale selection"):
                _on_tick(host, lambda: host.resume_motion(
                    run_id,
                    expected_hold_revision=revision,
                    request_key=_key(),
                ))
        if scope.held:
            _on_tick(host, lambda: host.resume_motion(
                run_id,
                expected_hold_revision=scope.hold_revision,
                request_key=_key(),
            ))
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
        _on_tick(host, lambda: host.stop_run(run_id))
        host.shutdown()


def test_halting_read_crossed_by_a_release_cannot_complete_the_halt():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    scope.state = ProgramRunScopeState.ACTIVE
    _halt(owner, run_id)
    read_scope = scope.execution_scope

    def crossed(run):
        scope.state = ProgramRunScopeState.SETTLED
        result = read_scope(run)
        # The Kernel's freeze is released while the read is in flight.
        scope.frozen = "none"
        scope.hold_revision += 1
        owner._reconcile_hold_facts(run, _kernel_facts(scope, run))  # noqa: SLF001
        return result

    scope.execution_scope = crossed
    assert owner.reconcile_halting(run_id).state is ProgramRunState.RUNNING
    scope.execution_scope = read_scope
    assert owner.reconcile_halting(run_id).state is ProgramRunState.RUNNING
    _stop(owner, run_id)


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

    stopped = owner.stop_motion(
        program_run_id,
        reason="ordinary operator motion Stop before the first operation",
        stop_request_key=_key(),
    )
    host.schedule(program_run_id)

    assert stopped.state is ProgramRunState.RUNNING
    assert scope.stop_requests == 1
    assert task.starts == 0

    waiting = host.advance_ready_once()[0]
    assert waiting.state is ProgramRunState.RUNNING
    assert task.starts == 0
    # Reached but not started: presented as held (waiting for Run), not idle.
    assert "held" in waiting.node_statuses.values()

    resumed = _on_tick(host, lambda: host.resume_motion(
        program_run_id,
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
    ))
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.release_requests == 1
    # The tick that applied the Resume ticked the tree right after it.
    running = owner.read(program_run_id)
    assert running.state is ProgramRunState.RUNNING
    assert "held" not in running.node_statuses.values()
    assert task.starts == 1

    _on_tick(host, lambda: host.stop_run(program_run_id))
    host.shutdown()


def test_intervention_registry_keeps_only_current_captured_node_attempts() -> None:
    registry = ProgramInterventionRegistry(maximum_retained_runs=1)
    registry.register_run(1, ["arm", "gripper"])
    arm = NodeAttemptIdentity(1, "arm", 1)
    tool = NodeAttemptIdentity(1, "gripper", 2)

    def require(identity):
        return registry.require(
            identity,
            reason_code="operation_stopped",
            message="",
            policy_parameter="operator_stop_policy",
        )

    first, second = require(arm), require(tool)
    assert registry.read_all(1) == (first, second)
    assert require(arm) == first
    with pytest.raises(ProgramSchedulingError, match="another intervention"):
        require(replace(arm, node_attempt_id=3))
    with pytest.raises(ProgramSchedulingError, match="uncaptured"):
        require(replace(arm, node_instance_id="unknown"))
    registry.resolve(tool, action="retry")
    assert registry.consume_resolution(arm) is None
    with pytest.raises(ProgramSchedulingError, match="another attempt"):
        registry.consume_resolution(replace(tool, node_attempt_id=4))
    assert registry.consume_resolution(tool) == ("retry", None)
    assert registry.read_all(1) == (first,)
    registry.resolve(arm, action="retry")
    assert registry.consume_resolution(arm) == ("retry", None)
    assert registry.read_all(1) == ()
    # A successor attempt reuses its captured-node slot, not an ever-growing ledger.
    require(replace(arm, node_attempt_id=3))
    registry.release(1)
    assert registry.read_all(1) == ()
    registry.register_run(2, ["arm"])
    with pytest.raises(ProgramSchedulingError, match="uncaptured"):
        require(arm)


def test_intervention_focus_does_not_hide_a_sibling_stop() -> None:
    registry = ProgramInterventionRegistry(maximum_retained_runs=1)
    registry.register_run(1, ["arm", "supervision"])
    arm = NodeAttemptIdentity(1, "arm", 1)
    stop = registry.require(
        arm,
        reason_code="operation_stopped",
        message="",
        policy_parameter="operator_stop_policy",
    )
    ordinary = registry.require(
        NodeAttemptIdentity(1, "supervision", 2),
        reason_code="program_node_failed",
        message="",
        policy_parameter="recovery_policy",
    )
    assert registry.read(1) == ordinary
    assert registry.has_operator_stop(1)
    assert registry.read_all(1) == (stop, ordinary)
    registry.resolve(ordinary.identity, action="manual_success")
    assert registry.read(1) == stop
    assert registry.read_all(1) == (stop, ordinary)
    registry.resolve(arm, action="retry")
    assert registry.consume_resolution(arm) == ("retry", None)
    assert not registry.has_operator_stop(1)
    assert registry.read(1) is None
    assert registry.consume_resolution(ordinary.identity) == ("manual_success", None)


def test_parallel_stop_records_both_attempts_before_halt_recovery() -> None:
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
        api_version="manyforge.program.v3",
    )
    program = Program.from_dict(document, admission.environment.catalog)
    admission = _admission_for_program(admission, program, [])
    run_id = owner.start(admission).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert task.starts == 2
    assert _halt(owner, run_id).state is ProgramRunState.HALTING
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    assert {
        row.identity.node_instance_id for row in interventions.read_all(run_id)
    } == {
        "parallel.0",
        "parallel.1",
    }
    assert (
        owner.resume_halt(run_id).state
        is ProgramRunState.RUNNING
    )
    # Execute keeps both interrupted attempts stopped.
    assert task.recoveries == []
    assert len(interventions.read_all(run_id)) == 2
    owner.advance_once(run_id)
    assert task.starts == 2
    owner.resume_motion(
        run_id,
        expected_hold_revision=_scope.hold_revision,
        request_key=_key(),
    )
    assert len(task.recoveries) == 2
    assert interventions.read_all(run_id) == ()
    owner.advance_once(run_id)
    assert task.starts == 4
    _stop(owner, run_id)


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
        api_version="manyforge.program.v3",
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
    owner.stop_motion(run_id, stop_request_key=_key())
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
            expected_hold_revision=scope.hold_revision,
            request_key=_key(),
        )
    assert [item[0] for item in task.recoveries] == ["task-1"]
    assert scope.release_requests == 0 and scope.held
    assert (
        owner.resume_motion(
            run_id,
            expected_hold_revision=scope.hold_revision,
            request_key=_key(),
        ).state
        is ProgramRunState.RUNNING
    )
    assert [item[0] for item in task.recoveries] == ["task-1", "task-2"]
    assert scope.release_requests == 1
    owner.advance_once(run_id)
    assert task.starts == 4
    _stop(owner, run_id)


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
        api_version="manyforge.program.v3",
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

    assert _halt(owner, run_id).state is ProgramRunState.HALTING
    now[0] = 150.0
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    now[0] = 400.0  # Halted for 299 s: none of it counts, held or not.
    resumed = owner.resume_halt(run_id)
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
    _halt(owner, run_id)
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED

    stopped = _stop(owner, run_id)

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
    _stop(owner, run_id)


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
    _stop(owner, run_id)


def test_halt_then_execute_keeps_motion_stopped_until_resume() -> None:
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

    _host_halt(host, scope, program_run_id)

    stopped = host.advance_ready_once()[0]
    assert stopped.state is ProgramRunState.HALTED
    assert stopped.active_operation_count == 0
    assert stopped.tree_status == "running"
    # Mirrored from the Kernel's hold facts: no Stop call of the owner's own.
    assert scope.stop_requests == 0
    assert scope.halts == 1
    assert task.cancels == 0
    assert context.unbound == []
    assert interventions.read(program_run_id) is not None

    resumed = _on_tick(host, lambda: host.resume_halt(program_run_id))

    # Execute after Halt resumes execution only: no recovery, no new attempt,
    # and the interrupted work keeps its Stop intervention.
    assert resumed.state is ProgramRunState.RUNNING
    assert resumed.active_operation_count is None
    assert task.recoveries == []
    assert scope.release_requests == 1
    assert scope.halt_releases == [scope.hold_revision - 1]
    assert interventions.read(program_run_id) is not None
    # The stored Halt facts are older than the runner's own release.
    waiting = host.advance_ready_once()[0]
    assert waiting.state in {
        ProgramRunState.RUNNING,
        ProgramRunState.INTERVENTION_REQUIRED,
    }
    assert "held" in waiting.node_statuses.values()
    assert task.starts == 1

    # Only the explicit group Resume recovers and restarts the motion.
    _on_tick(host, lambda: host.resume_motion(
        program_run_id,
        expected_hold_revision=scope.hold_revision,
        request_key=_key(),
    ))
    assert len(task.recoveries) == 1
    assert interventions.read(program_run_id) is None
    # The tick that applied the Resume ticked the tree right after it.
    assert owner.read(program_run_id).state is ProgramRunState.RUNNING
    assert task.starts == 2

    _on_tick(host, lambda: host.stop_run(program_run_id))
    host.shutdown()


def test_halt_waits_for_scope_settlement_before_freezing_the_tree() -> None:
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

    _host_halt(host, scope, program_run_id)

    assert host.advance_ready_once()[0].state is ProgramRunState.HALTING
    assert task.cancels == 0
    assert host.advance_ready_once()[0].state is ProgramRunState.HALTING

    scope.state = ProgramRunScopeState.SETTLED
    stopped = host.advance_ready_once()[0]
    assert stopped.state is ProgramRunState.HALTED
    assert task.cancels == 0

    terminal = _on_tick(host, lambda: host.stop_run(program_run_id))
    assert terminal.state is ProgramRunState.STOPPED
    # Physical settlement already belongs to the Kernel's Halt. Program
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
    _host_scene_step_sent(host)
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert observer_entered.wait(timeout=1.0)

    # The ended run's Stop sends nothing; its answer comes from the run's
    # next tick, after the observer (OD-146).
    thread, outcome = _post(
        host, lambda: host.stop_run(started.capture.program_run_id)
    )
    assert thread.is_alive()
    release_observer.set()
    advancing.join(timeout=1.0)
    host.advance_ready_once()
    assert _answer(thread, outcome).state is ProgramRunState.SUCCEEDED
    assert set(observed) == {ProgramRunState.SUCCEEDED}
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
            _host_scene_step_sent(host)
            with pytest.raises(ProgramSchedulingError, match="observer failed"):
                host.advance_ready_once()
        assert delivered.wait(timeout=2)
        assert len(records) == 1 and records[0]["state"] == "succeeded"
        with pytest.raises(ProgramSchedulingError, match="worker failed"):
            host.schedule(99)
    finally:
        host.shutdown()


class _TickClock:
    """A monotonic clock for manual ticks: each tick's start and end are two
    reads; the end read advances by the tick's scripted duration."""

    def __init__(self, durations: Sequence[float]) -> None:
        self._durations = list(durations)
        self._now = 100.0
        self._in_tick = False

    def __call__(self) -> float:
        if self._in_tick:
            self._now += self._durations.pop(0) if self._durations else 0.0
        self._in_tick = not self._in_tick
        return self._now


def _late_tick_lines(caplog) -> list[dict[str, str]]:
    lines = []
    for record in caplog.records:
        message = record.getMessage()
        if message.startswith(f"manyforge_fault code={PROGRAM_TICKS_LATE_CODE} "):
            lines.append(dict(field.split("=", 1) for field in message.split()[1:]))
    return lines


def test_late_ticks_are_counted_and_logged_once_per_episode(caplog) -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    # Period 10 ms: two late ticks, one on time, then one more late tick.
    clock = _TickClock((0.020, 0.030, 0.001, 0.025))
    host = ProgramRunTickHost(owner, tick_interval_s=0.01, monotonic=clock)
    run_id = host.start_run(admission).capture.program_run_id
    caplog.set_level("WARNING", logger=program_scheduling.__name__)

    host.advance_ready_once()
    host.advance_ready_once()
    # Late ticks are counted; nothing is stopped and no line is written yet.
    assert host.read(run_id).state is ProgramRunState.RUNNING
    assert host.read_tick_overruns(run_id) == ProgramRunTickOverruns(
        late_ticks=2, longest_tick_ns=30_000_000, episodes=1
    )
    assert _late_tick_lines(caplog) == []

    host.advance_ready_once()  # On time: the episode ends with one line.
    assert _late_tick_lines(caplog) == [
        {
            "code": PROGRAM_TICKS_LATE_CODE,
            "program_run_id": str(run_id),
            "period_ns": "10000000",
            "late_ticks": "2",
            "longest_tick_ns": "30000000",
            "episode_ended": "1",
            "late_ticks_total": "2",
            "longest_tick_ns_total": "30000000",
            "episodes_total": "1",
            "line": "1",
            "max_lines": "64",
        }
    ]

    host.advance_ready_once()  # Late again: a second episode opens.
    assert host.read_tick_overruns(run_id) == ProgramRunTickOverruns(
        late_ticks=3, longest_tick_ns=30_000_000, episodes=2
    )
    assert len(_late_tick_lines(caplog)) == 1

    host.advance_ready_once()  # On time: the second episode's line.
    lines = _late_tick_lines(caplog)
    assert len(lines) == 2
    assert lines[1]["late_ticks"] == "1"
    assert lines[1]["longest_tick_ns"] == "25000000"
    assert lines[1]["episode_ended"] == "1"
    assert lines[1]["episodes_total"] == "2"
    host.shutdown()


def test_a_late_tick_episode_ends_when_its_run_leaves_the_schedule(caplog) -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    clock = _TickClock((0.02,) * 50)
    host = ProgramRunTickHost(owner, tick_interval_s=0.01, monotonic=clock)
    run_id = host.start_run(admission).capture.program_run_id
    caplog.set_level("WARNING", logger=program_scheduling.__name__)

    ticks = 0
    while host.advance_ready_once():
        ticks += 1
    # Every tick was late and the run ended: one line, written as it left.
    assert host.read(run_id).state is ProgramRunState.SUCCEEDED
    lines = _late_tick_lines(caplog)
    assert len(lines) == 1
    assert lines[0]["late_ticks"] == str(ticks)
    assert lines[0]["episode_ended"] == "1"
    host.shutdown()


def test_a_long_late_tick_episode_writes_bounded_lines(caplog, monkeypatch) -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    monkeypatch.setattr(program_scheduling, "_LATE_TICK_LINE_EVERY", 2)
    monkeypatch.setattr(program_scheduling, "_MAX_LATE_TICK_LINES_PER_RUN", 3)
    clock = _TickClock((0.02,) * 20)
    host = ProgramRunTickHost(owner, tick_interval_s=0.01, monotonic=clock)
    run_id = host.start_run(admission).capture.program_run_id
    caplog.set_level("WARNING", logger=program_scheduling.__name__)

    for _ in range(20):
        host.advance_ready_once()

    # One episode of 20 late ticks: a line every 2 late ticks while it lasts,
    # at most 3 per run; every late tick is still counted.
    lines = _late_tick_lines(caplog)
    assert [line["late_ticks"] for line in lines] == ["2", "4", "6"]
    assert {line["episode_ended"] for line in lines} == {"0"}
    assert [line["line"] for line in lines] == ["1", "2", "3"]
    assert host.read_tick_overruns(run_id) == ProgramRunTickOverruns(
        late_ticks=20, longest_tick_ns=20_000_000, episodes=1
    )
    host.shutdown()


def test_end_of_run_work_runs_on_the_pool_while_other_runs_tick() -> None:
    identifiers = iter((1, 2))
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        run_id_factory=lambda: next(identifiers),
    )
    entered, release = Event(), Event()
    observer_threads: list[str] = []
    terminal: list[int] = []

    def observe_terminal(snapshot) -> None:
        observer_threads.append(threading.current_thread().name)
        entered.set()
        assert release.wait(timeout=5.0)
        terminal.append(snapshot.capture.program_run_id)

    host = ProgramRunTickHost(
        owner,
        tick_interval_s=0.005,
        terminal_observer=observe_terminal,
    )
    # The first run starts with deferred ticks: a tick of it already in
    # flight when the stop is posted could end the run before the stop's own
    # scheduling, which then delivers its end a second time (the consumers
    # accept that). Here the stop's tick is the run's first.
    first = host.start_run(admission, defer_ticks=True).capture.program_run_id
    second = host.start_run(admission).capture.program_run_id
    observations = []
    observe = task.observe

    def counted(task_id):
        observations.append(task_id)
        return observe(task_id)

    task.observe = counted
    host.start()
    try:
        host.stop_run(first)
        assert entered.wait(timeout=5.0)
        # The end-of-run work runs on a tick worker; the scheduler thread
        # keeps ticking the other run meanwhile.
        assert observer_threads == observer_threads[:1]
        assert observer_threads[0].startswith("manyforge-program-tick-worker")
        seen = len(observations)
        deadline = time.monotonic() + 5.0
        while len(observations) < seen + 3 and time.monotonic() < deadline:
            time.sleep(0.005)
        assert len(observations) >= seen + 3
        assert host.read(second).state is ProgramRunState.RUNNING

        # Forgetting the ended run waits for its end-of-run work.
        host.reset_run(first)
        forgotten = Event()
        forget = Thread(
            target=lambda: (host.forget_reset_run(first), forgotten.set())
        )
        forget.start()
        assert not forgotten.wait(timeout=0.1)
        release.set()
        assert forgotten.wait(timeout=5.0)
        forget.join(timeout=5.0)
        # Once per hand-off, then the state wake.
        assert terminal == [first]
    finally:
        release.set()
        host.stop_run(second)
        host.wait_terminal(second)
        host.shutdown()


def test_tick_host_cleans_an_admitted_run_if_bounded_scheduling_rejects_it() -> None:
    identifiers = iter((1, 2))
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

    assert owner.read(2).state is ProgramRunState.STOPPED
    assert context.unbound == [2]
    assert _on_tick(host, lambda: host.stop_run(first.capture.program_run_id)).state is (
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
    host.schedule(99)

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
    identifiers = iter((1, 2))
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
    stopping = _stop(owner, started.capture.program_run_id, reason="operator stop")
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

    repeated = _stop(owner, started.capture.program_run_id)
    assert repeated.state is ProgramRunState.STOPPED
    assert scope.stop_requests == 1
    assert task.cancels == 1


def test_stop_is_sent_during_a_blocked_dispatch_and_the_tree_ends_after_it() -> None:
    """OD-146: the runner's own Stop no longer waits for the tick in progress;
    the tree's invalidation (the leaf's cancel) follows on the next tick."""
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
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    program_run_id = host.start_run(admission).capture.program_run_id
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert tick_entered.wait(timeout=1.0)
    try:
        thread, outcome = _post(host, lambda: host.stop_run(program_run_id))
        assert events == ["leaf_start_enter", "scope_stop"]
    finally:
        release_tick.set()
        advancing.join(timeout=1.0)
    assert not advancing.is_alive()
    host.advance_ready_once()
    stopped = _answer(thread, outcome)
    assert stopped.state is not ProgramRunState.RUNNING
    assert events == ["leaf_start_enter", "scope_stop", "leaf_start", "leaf_cancel"]
    assert task.starts == 1
    host.shutdown()


def test_program_stop_refuses_new_work_before_the_runner_records_it() -> None:
    """The Program runner records `stopping` only after this Stop returns."""
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    starts = task.starts
    stopped = _on_tick(host, lambda: host.stop_run(run_id, reason="operator stop"))
    assert stopped.state is not ProgramRunState.RUNNING
    for command in (host.resume_motion, host.start_motion):
        with pytest.raises(ProgramRunOwnerError):
            _on_tick(host, lambda: command(
                run_id,
                expected_hold_revision=scope.hold_revision,
                request_key=_key(),
                reason=None,
            ))
    host.advance_ready_once()
    assert task.starts == starts
    host.shutdown()


def test_failure_is_not_relabelled_as_stopped_while_kernel_work_settles() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED
    )
    started = owner.start(admission)
    scope.state = ProgramRunScopeState.ACTIVE

    stopping = owner.advance_once(started.capture.program_run_id)
    assert stopping.state is ProgramRunState.STOPPING
    assert stopping.reason_code == "program_terminal_scope_not_settled"
    repeated = _stop(owner, started.capture.program_run_id)
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

    unknown = _stop(owner, started.capture.program_run_id)
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

    owner.reset_retained_effects(9)

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
        "run_end",
        "halt",
    }
    assert value["operation_failures"] == []
    assert value["halt"] is None
    assert value["schema_version"] == "manyforge.program_run_status.v5"
    assert value["details"] is None and value["origin"] is None
    assert value["node_status_revision"] == 1
    assert value["node_statuses"]
    assert value["capture"]["schema_version"] == ("manyforge.program_run_capture.v3")


def _scenario_selection(admission: ProgramRunAdmission) -> dict[str, Any]:
    program = admission.program
    environment = admission.environment
    return {
        "schema_version": "manyforge.program_run_input.v3",
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
                "role": "headless_projection",
                "application_start_id": "fedcba9876543210",
            }
        ],
        "program": program.to_dict(),
        "program_frame_facts": {
            "applied_record_digest": admission.applied_record_digest,
            "program_binding_id": admission.program_binding_id,
            "program_revision": program.program_revision,
            "retained_bundle_id": admission.retained_bundle_id,
            "graph_revision": admission.graph_revision,
            "components": ["ur5_robot"],
            "frames": [{"reference": {"component_id": "ur5_robot", "frame_id": "base"},
                        "match_count": 1}],
        },
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


@pytest.mark.parametrize("field", [
    "applied_record_digest", "program_binding_id", "program_revision",
    "retained_bundle_id", "graph_revision",
])
def test_run_admission_refuses_stale_frame_facts(field: str) -> None:
    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    selection = _scenario_selection(admission)
    selection["program_frame_facts"][field] = "sha256:" + "f" * 64
    with pytest.raises(ProgramRunOwnerError, match="scenario frame facts are invalid"):
        build_program_run_admission(
            selection,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=admission.source_document_digest,
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )


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


def test_applied_selection_keeps_strict_nested_program_identity() -> None:
    """A stale nested revision in an otherwise consistent Applied selection is
    refused at the Runner boundary; the same document opened for authoring
    only warns (`edited_outside_manyforge`)."""

    from manyforge_behavior.program import ProgramValidationError

    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    selection = _scenario_selection(admission)
    selection["program"]["program_revision"] = "sha256:" + "e" * 64

    with pytest.raises(ProgramValidationError, match="content identity mismatch"):
        build_program_run_admission(
            selection,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=(admission.source_document_digest),
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )

    authored = Program.from_dict(
        selection["program"],
        admission.environment.catalog,
        authoring_load=True,
    )
    assert [row["code"] for row in authored.load_diagnostics] == [
        "edited_outside_manyforge"
    ]
    assert authored.program_revision == admission.program.program_revision


def test_applied_selection_refuses_an_older_catalog_selector(monkeypatch) -> None:
    """An internally consistent Program under an older catalog minor is refused
    at the Runner boundary; opened for authoring it is normalized."""

    from manyforge_behavior.canonical_artifact import compute_content_identity
    from manyforge_behavior.program import ProgramValidationError

    _owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    selection = _scenario_selection(admission)
    catalog = admission.environment.catalog
    # The shipped catalog is 3.0.0; emulate a later compatible minor so this
    # still exercises an older same-major selector instead of inventing 3.-1.0.
    monkeypatch.setattr(type(catalog), "content_version", property(lambda _: "3.1.0"))
    older = selection["program"]
    older["catalog"]["content_version"] = "3.0.0"
    older["program_revision"] = compute_content_identity(
        older,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    selection["program_revision"] = older["program_revision"]
    selection["program_binding"]["program_revision"] = older["program_revision"]

    with pytest.raises(ProgramValidationError, match="exact active v3 catalog"):
        build_program_run_admission(
            selection,
            expected_scenario_id=admission.scenario_id,
            expected_source_document_digest=(admission.source_document_digest),
            environment=admission.environment,
            owner_availability=admission.readiness.owner_by_port,
        )

    authored = Program.from_dict(older, catalog, authoring_load=True)
    assert authored.load_diagnostics == ()
    assert authored.to_dict()["catalog"]["content_version"] == catalog.content_version
    assert authored.program_revision != older["program_revision"]


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
            "schema_version": "manyforge.applied_program_selection.v5",
            "baseline_program_binding_id": admission.program_binding_id,
            "preparation_digest": _DIGEST_C,
            "source": {"record_digest": admission.applied_record_digest},
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
    assert joined.owner_identities[0].role == "headless_projection"
    # The Applied Scenario project folder scopes the run's persistent values.
    assert joined.project_path == admission.project_path
    # The simulator entity mapping reaches the client ports frozen.
    scene = joined.port_context(1).simulation_scene
    assert scene["cell_frame_id"] == "universe"
    assert scene["scene_entities"][0]["entity"] == "/World/box"
    applied["graph_revision"] = _DIGEST_A
    applied["program_frame_facts"]["graph_revision"] = _DIGEST_A
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
    applied["schema_version"] = "manyforge.applied_program_selection.v5"
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
        api_version="manyforge.program.v3",
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
    # The catalog declares the timer's input failure (OD-77); this run pauses
    # on it instead of failing.
    assert set(admission.environment.catalog.get("timer").outcomes) == {
        "success",
        "failure",
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
        request_key=_key(),
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
    "reference_type": "component_frame",
    "reference_id": "tool0",
    "component_id": "arm.left",
    "retryable": False,
    # Bounded away: an over-long value, a nested value, an unregistered key.
    "path": "p" * 257,
    "rule_id": {"nested": "rule.1"},
    "trace": "owner internals",
}
_OWNER_REFUSAL_PROJECTED = {
    "reason": "observed_state_unavailable",
    "reference_type": "component_frame",
    "reference_id": "tool0",
    "component_id": "arm.left",
    "retryable": False,
}
_OWNER_REFUSAL_MESSAGE = (
    "addressed observed state is unavailable (observed_state_unavailable; "
    "reference component_frame 'tool0' of component 'arm.left')"
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
        application_start_id=_APPLICATION_START_ID,
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
        api_version="manyforge.program.v3",
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
    assert status["schema_version"] == "manyforge.program_run_status.v5"
    assert status["reason_code"] == "node_input_unavailable"
    assert (status["details"], status["origin"]) == (details, origin)
    diagnostic = owner.take_diagnostic(run_id)
    assert diagnostic["schema_version"] == "manyforge.program_diagnostic.v5"
    assert diagnostic["reason_code"] == "node_input_unavailable"
    assert diagnostic["message"] == status["message"]
    assert (diagnostic["details"], diagnostic["origin"]) == (details, origin)
    offer = _recovery_offer(paused, interventions, run_id, "wait")
    assert offer["schema_version"] == "manyforge.cycle_recovery_offer.v6"
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
    run_id = owner.start(admission).capture.program_run_id
    _scene_step_sent(owner, run_id)
    succeeded = owner.advance_once(run_id)
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
    stopped = _stop(owner, run_id, reason="operator Stop")
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

    if task_state is OwnerTaskState.SUCCEEDED:
        _scene_step_sent(owner, run_id)
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
    document["tree"] = {"node_id":"gripper.command_position","instance_id":"command",
        "parameters":{"target":{"component_id":"tool.1","command_target_id":"gripper.command"},"position":{"kind":"linear_m","value_m":0.008}},"operator_stop_policy":policy,"children":[]}
    document["program_revision"] = compute_content_identity(document,own_field="program_revision",
        identity_domain="program-revision",api_version="manyforge.program.v3")
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
    assert _halt(owner, run_id).state is ProgramRunState.HALTING
    now[0]=150.0
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    now[0]=400.0
    assert owner.resume_halt(run_id).state is ProgramRunState.RUNNING
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
            raise OwnerInvocationHeldError(scope.hold_revision)
    owner,admission,task,scope,events = _fixture(initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events:UnsentPort(events,OwnerTaskState.ACTIVE))
    run_id=owner.start(_program_dwell_admission(admission)).capture.program_run_id
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    scope.state=ProgramRunScopeState.SETTLED
    owner.stop_motion(run_id,group_id="all",stop_request_key=_key())
    assert owner.advance_once(run_id).state is ProgramRunState.RUNNING
    assert task.pending_preparation and task.starts == 1
    def retire(actual_run, selected, revision):
        assert actual_run == run_id and selected == frozenset({"command"})
        assert revision == scope.hold_revision
        assert task.pending_preparation
        events.append("preparation_cleanup")
        if cleanup == "pending": raise RuntimeError("readback still resident")
        task.pending_preparation=False
        if cleanup == "new_hold": scope.hold_revision+=1
    scope.retire_held_preparations=retire
    args=dict(expected_hold_revision=scope.hold_revision,
              request_key=_key(),group_id="all",reason=None)
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
        # An admitted Kernel operation: its task id is the operation number.
        return self.observe(self.starts)

    def observe(self, task_id: int) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.FAILED,
            metadata={"recovery_assessment": self._assessment, "operation_id": task_id},
            reason_code="planning_failed",
            message="no feasible path",
        )

    def close(self, task_id, _identity, *, request_key) -> bool:
        self.closes.append(task_id)
        return True

    def recover(self, _task_id, _identity, *, action, request_key, reason):
        self.recoveries.append(action)
        return {"request_key": format_request_key(request_key), "action": action,
                "accepted": True, "error": None}


class _ProgramStopTaskPort(_TaskPort):
    """F-6a (D+W): a carry motion stopped while moving, as the production
    port projects it. `recovery_class` is the Kernel's requester class: the
    Program's own Stop (gate trip, carried object lost) or an operator Stop
    (which keeps the pause_for_operator policy). The engine reports waiting
    (`terminal_cleanup_pending`) for the first `cleanup_reads` observations,
    then retry_ready with close permission."""

    def __init__(self, events: list[str], recovery_class: str, cleanup_reads: int) -> None:
        super().__init__(events, OwnerTaskState.ACTIVE)
        self.recovery_class = recovery_class
        self.cleanup_reads = cleanup_reads
        self.observes = 0
        self.closes: list[int] = []
        self.recoveries: list[tuple[int | str, bytes]] = []

    def start(self, _node_id, _identity, _inputs) -> OwnerTaskSnapshot:
        self.starts += 1
        self._events.append("leaf_start")
        # An admitted Kernel operation: its task id is the operation number.
        return OwnerTaskSnapshot(task_id=self.starts, state=OwnerTaskState.ACTIVE)

    def observe(self, task_id: int) -> OwnerTaskSnapshot:
        if task_id != 1:
            # The failure branch's own motion (the safe pose) completes.
            return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.SUCCEEDED)
        self.observes += 1
        pending = self.observes <= self.cleanup_reads
        metadata: dict[str, Any] = {
            "operation_id": task_id,
            "operation_state": "stopped",
            "stop_event_id": 1,
            "recovery_assessment": {
                "allowed_actions": (
                    ["reset_cycle"] if pending else ["replan_from_stopped_state", "reset_cycle"]
                ),
                "recovery_class": self.recovery_class,
                "status": "waiting" if pending else "retry_ready",
                "reason_code": "terminal_cleanup_pending" if pending else "stop_certified",
                "execution_state": "Stopped",
                "eligible_close": not pending,
                "retry_eligibility": {
                    "eligible": False,
                    "cause": "not_eligible",
                    "effect": "effect_observed",
                    "reason": "not_retryable_state",
                },
            },
        }
        if self.recovery_class == "ordinary_operator_stop":
            metadata["authored_failure_policy"] = "pause_for_operator"
            metadata["authored_failure_policy_parameter"] = "operator_stop_policy"
        return OwnerTaskSnapshot(
            task_id=task_id,
            state=OwnerTaskState.INTERRUPTED,
            metadata=metadata,
            reason_code="interrupt_fault",
            message="Global interrupt rule matched",
        )

    def close(self, task_id, _identity, *, request_key) -> bool:
        self.closes.append(task_id)
        return True

    def recover(self, task_id, _identity, *, action, request_key, reason):
        self.recoveries.append((task_id, request_key))
        return {"request_key": format_request_key(request_key), "action": action,
                "accepted": True, "error": None}


def _advance_until_settled(owner: ProgramRunOwner, run_id: str, limit: int = 20):
    snapshot = owner.advance_once(run_id)
    for _ in range(limit):
        if snapshot.state is not ProgramRunState.RUNNING:
            break
        snapshot = owner.advance_once(run_id)
    return snapshot


@pytest.mark.parametrize("cleanup_reads", [0, 1, 3])
def test_f6a_program_requested_stop_fails_into_the_tree_and_closes_after_cleanup(
    cleanup_reads: int,
) -> None:
    """D: the Program's own Stop is an ordinary node failure, not an operator
    hold: the operation closes (the robot's engine is free for the failure
    branch's motion) and no intervention is recorded. W: while the engine's
    terminal cleanup is pending the node rereads instead of retaining it."""

    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _ProgramStopTaskPort(
            events, "ordinary_program_stop", cleanup_reads
        ),
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    settled = _advance_until_settled(owner, run_id)
    assert task.closes == [1]
    assert task.observes >= cleanup_reads + 1
    assert interventions.read_all(run_id) == ()
    # This minimal Program has no failure branch: the node's failure is the
    # Run's, with the Stop's typed cause (in a Fallback the next child runs).
    assert settled.state is ProgramRunState.FAILED
    assert settled.reason_code == "interrupt_fault"


def test_f6a_program_requested_stop_runs_the_fallback_recovery_motion() -> None:
    """The mock_lite6_part_lost_recovery shape: Fallback(carry, safe pose). The carry is
    stopped by the Program's own Stop (gate on `part_lost`, or
    on_grasp_lost {stop_motion: true}); after its terminal cleanup the
    operation closes, so the recovery branch's motion on the same robot is
    started (not refused `participant_engine_busy`) and the Run succeeds with
    no operator hold."""

    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _ProgramStopTaskPort(events, "ordinary_program_stop", 2),
        interventions=interventions,
    )
    document = admission.program.to_dict()
    carry = deepcopy(document["tree"]["children"][0])
    carry["instance_id"] = "pick_retreat"
    safe_pose = deepcopy(carry)
    safe_pose["instance_id"] = "safe_pose_after_loss"
    document["tree"] = {
        "node_id": "fallback",
        "instance_id": "carry_or_recover",
        "parameters": {},
        "children": [carry, safe_pose],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    admission = _admission_for_program(
        admission, Program.from_dict(document, admission.environment.catalog), []
    )
    run_id = owner.start(admission).capture.program_run_id
    settled = _advance_until_settled(owner, run_id)
    assert task.closes == [1]
    assert task.starts == 2  # The recovery motion was submitted after the close.
    assert interventions.read_all(run_id) == ()
    assert settled.state is ProgramRunState.SUCCEEDED


def test_f6a_program_requested_stop_with_intervention_policy_holds_for_the_operator() -> None:
    """(d) An intervention recovery_policy on the node: the Program's own Stop
    holds the Run (INTERVENTION_REQUIRED) as a failure hold, not an operator
    Stop. The operation stays live for Retry / Continue / Reset, recorded on
    the decided assessment after the terminal cleanup."""

    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _ProgramStopTaskPort(events, "ordinary_program_stop", 2),
        interventions=interventions,
    )
    program = _program_with_policies(admission, second_row=True)
    rows = [policy.to_dict() for policy in program_node_policies(program)]
    run_id = owner.start(_admission_for_program(admission, program, rows)).capture.program_run_id
    held = _advance_until_settled(owner, run_id)
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert task.closes == []
    rows = interventions.read_all(run_id)
    assert len(rows) == 1
    assert rows[0].policy_parameter != "operator_stop_policy"
    assert rows[0].reason_code == "interrupt_fault"
    assert rows[0].owner_assessment["status"] == "retry_ready"
    assert rows[0].owner_assessment["eligible_close"] is True
    assert rows[0].owner_assessment["recovery_class"] == "ordinary_program_stop"
    _stop(owner, run_id)


def test_f6a_operator_stop_still_holds_on_its_decided_assessment() -> None:
    """An operator Stop is still an operator hold (Resume / Reset), and with W
    the hold is recorded on the decided assessment, never on the brief
    terminal-cleanup wait. Nothing is closed."""

    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _ProgramStopTaskPort(events, "ordinary_operator_stop", 2),
        interventions=interventions,
    )
    run_id = owner.start(admission).capture.program_run_id
    held = _advance_until_settled(owner, run_id)
    assert held.state is ProgramRunState.INTERVENTION_REQUIRED
    assert task.closes == []
    rows = interventions.read_all(run_id)
    assert len(rows) == 1
    assert rows[0].policy_parameter == "operator_stop_policy"
    assert rows[0].owner_assessment["status"] == "retry_ready"
    assert rows[0].owner_assessment["recovery_class"] == "ordinary_operator_stop"
    _stop(owner, run_id)


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
    assert task.closes == [1]  # Closed before the wait.
    retried = owner.take_node_history_events(run_id)
    assert len(retried) == 1
    assert retried[0]["attempt"] == 2
    assert retried[0]["max_attempts"] == 4
    assert retried[0]["cause"] == "planning_no_effect"
    assert owner.take_node_history_events(run_id) == ()

    stopped = _stop(owner, run_id)

    # No run-end backstop: nothing was resident, nothing is reported.
    assert task.closes == [1] and task.recoveries == []
    assert task.cancels == 0
    assert stopped.state is ProgramRunState.STOPPED


# --- Per-node failure holds, Stop scope, Continue, operation failures ---

_OPERATION_FACT = {
    "operation": {"operation_id": 1, "operation_revision": 2},
    "resource_ids": ["arm.joints"],
    "failure": {"code": "no_feasible_candidate", "message": "No pick candidate is feasible"},
    "terminal_facts": [{"kind": "local_failure", "effect": "no_effect"}],
}


class _HeldFailureTaskPort(_PolicyTaskPort):
    """Every attempt fails into an authored pause, with its operation facts;
    the operation can be closed."""

    def __init__(self, events: list[str]) -> None:
        super().__init__(events, OwnerTaskState.FAILED)
        self.closes: list[tuple[int | str, bytes]] = []

    def start(self, node_id, identity, inputs) -> OwnerTaskSnapshot:
        started = super().start(node_id, identity, inputs)
        return replace(
            started, metadata={**started.metadata, "operation_failure": _OPERATION_FACT}
        )

    def close(self, task_id, _identity, *, request_key) -> bool:
        self.closes.append((task_id, request_key))
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
        api_version="manyforge.program.v3",
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
    _stop(owner, run_id)


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
        request_key=_key(),
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
    _stop(owner, run_id)


def test_own_groups_scope_stops_and_releases_only_the_held_nodes_groups() -> None:
    owner, _task, scope, interventions, run_id, _paused = _two_group_holds("own_groups")

    assert scope.last_selection_stop["group_ids"] == ["arm", "tools"]
    assert owner.hold_motion_groups(run_id, "tool_move") == (("tools",), "own_groups")
    tool = _hold(interventions, run_id, "tool_move")
    owner.retry_failure_pause(
        run_id,
        node_attempt_id=tool.identity.node_attempt_id,
        action=None,
        request_key=_key(),
        reason=None,
    )
    assert scope.last_selection_release["group_ids"] == ["tools"]
    _stop(owner, run_id)


def _release_for(held: str, others: Sequence[str], *, scope: str = "all"):
    """`_failure_hold_release` over named groups arm/tools/spare; `free` is
    the node in no named group."""
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
            request_key=_key(),
            reason=None,
        )
    else:
        owner.continue_failure_pause(
            run_id,
            node_attempt_id=free.identity.node_attempt_id,
            request_key=_key(),
            reason=None,
        )
    # arm_move's hold owns arm (the only named group): free_move releases
    # itself by node_instance_id, never another hold's group.
    assert scope.release_requests == releases + 1
    assert scope.last_selection_release["group_ids"] == []
    assert scope.last_selection_release["node_instance_id"] == "free_move"
    _stop(owner, run_id)


def test_a_named_group_hold_never_sends_a_node_release() -> None:
    owner, _task, scope, interventions, run_id, _paused = _two_group_holds(
        None, layout=(("arm_move", ["arm"]), ("free_move", None))
    )
    arm = _hold(interventions, run_id, "arm_move")
    owner.retry_failure_pause(
        run_id,
        node_attempt_id=arm.identity.node_attempt_id,
        action=None,
        request_key=_key(),
        reason=None,
    )
    assert scope.last_selection_release["group_ids"] == ["arm"]
    assert "node_instance_id" not in scope.last_selection_release
    _stop(owner, run_id)


def test_continue_closes_the_held_operation_and_the_node_fails_to_the_tree() -> None:
    owner, task, scope, interventions, run_id, _paused = _two_group_holds(
        None, success_policy="one"
    )
    arm = _hold(interventions, run_id, "arm_move")

    continue_key = _key()
    owner.continue_failure_pause(
        run_id,
        node_attempt_id=arm.identity.node_attempt_id,
        request_key=continue_key,
        reason=None,
    )
    assert task.closes == [(arm.owner_task_id, continue_key)]
    assert scope.last_selection_release["group_ids"] == ["arm"]
    owner.resolve_intervention(
        run_id, node_attempt_id=arm.identity.node_attempt_id, action="manual_failure"
    )
    starts = task.starts
    ticked = owner.advance_once(run_id)
    assert ticked.node_statuses["arm_move"] == "failure"
    assert task.starts == starts  # FAILURE to the tree, no new attempt.
    assert ticked.state is ProgramRunState.INTERVENTION_REQUIRED  # tool_move held
    _stop(owner, run_id)


def test_continue_refused_when_the_held_operation_cannot_close() -> None:
    owner, task, scope, interventions, run_id, _paused = _two_group_holds(None)
    arm = _hold(interventions, run_id, "arm_move")
    task.close = lambda *_args, **_kwargs: False
    releases = scope.release_requests

    with pytest.raises(ProgramRunOwnerError, match="not eligible to close"):
        owner.continue_failure_pause(
            run_id,
            node_attempt_id=arm.identity.node_attempt_id,
            request_key=_key(),
            reason=None,
        )
    assert scope.release_requests == releases
    _stop(owner, run_id)


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


def test_the_run_end_says_why_the_run_ended() -> None:
    """`ProgramRunEnd`: a failure that reaches the root ends the
    run `failed` (never "finished"), naming the node and its error; a
    succeeded root is `completed`; a running run has no end yet."""

    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.FAILED,
        task_factory=lambda events: _FailedFactTaskPort(events, OwnerTaskState.FAILED),
    )
    run_id = owner.start(admission).capture.program_run_id
    failed = owner.advance_once(run_id).to_dict()
    assert failed["state"] == "failed"
    assert failed["run_end"]["end_reason"] == "failed"
    assert failed["run_end"]["details"]["node_instance_id"] == "move_to_pick"
    assert failed["run_end"]["details"]["error"]["code"] == failed["reason_code"]

    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    run_id = owner.start(admission).capture.program_run_id
    assert owner.read(run_id).to_dict()["run_end"] is None
    for _ in range(8):
        completed = owner.advance_once(run_id)
        if completed.state is ProgramRunState.SUCCEEDED:
            break
    assert completed.to_dict()["run_end"] == {"end_reason": "completed", "details": {}}


def test_a_rule_hold_in_the_kernel_scope_reaches_the_run_snapshot() -> None:
    """The run reads the scope's stopped_by: rule; a run the operator
    then ends names the rule. Resume (scope without it) clears it."""

    owner, admission, _task, scope, _events = _fixture(initial_task_state=OwnerTaskState.ACTIVE)
    scope.stopped_by_rule_id = "door"
    started = owner.start(admission)
    assert started.stopped_by_rule_id == "door"
    scope.stopped_by_rule_id = None
    owner._entry(started.capture.program_run_id).run.refresh_operator_stop_scope()
    assert owner.read(started.capture.program_run_id).stopped_by_rule_id is None


@pytest.mark.parametrize(
    ("facts", "held_by_rule", "expected"),
    [
        ((), None, {"stopped_by": "operator"}),
        (
            ({"failure": {"terminal_error": {"code": "interrupt_fault", "details": {
                "reason": "io_global_rule_hold", "stopped_by": "rule", "rule_id": "door"}}}},),
            None,
            {"stopped_by": "rule", "rule_id": "door"},
        ),
        # A rule's `hold` held the run with nothing moving (the Kernel
        # scope's stopped_by: rule), then the operator ended it.
        ((), "door", {"stopped_by": "rule", "rule_id": "door"}),
    ],
)
def test_a_stopped_run_names_who_stopped_it(facts, held_by_rule, expected) -> None:
    snapshot = SimpleNamespace(
        state=ProgramRunState.STOPPED, details=None, origin=None,
        reason_code=None, message="", operation_failures=facts,
        stopped_by_rule_id=held_by_rule,
    )
    assert program_run_end(snapshot) == {"end_reason": "stopped", "details": expected}


class _MockRunRecorder:
    """The Run-wide Mock pause notifications the owner sends."""

    def __init__(self, events: list[str] | None = None) -> None:
        self.calls: list[tuple[Any, ...]] = []
        self.events = events
        # Which path each pause state took: sent at once, or recorded only
        # (entering Halt) and sent by the run's tick (resend_run_pause).
        self.sent: list[tuple[Any, ...]] = []
        self.recorded: list[tuple[Any, ...]] = []
        self.resends: list[int] = []

    def set_run_paused(self, program_run_id, run_revision, paused, held=()) -> None:
        self.calls.append(("pause", program_run_id, run_revision, paused, tuple(held)))
        self.sent.append(self.calls[-1])
        if self.events is not None:
            self.events.append(f"mock_snapshot:{run_revision}:{paused}:{','.join(held)}")

    def record_run_paused(self, program_run_id, run_revision, paused, held=()) -> bool:
        # Memory only, like the native port's record.
        self.calls.append(("pause", program_run_id, run_revision, paused, tuple(held)))
        self.recorded.append(self.calls[-1])
        return True

    def end(self, start_request) -> None:  # pragma: no cover - no Mock nodes here
        self.calls.append(("end", start_request))

    def resend_unknown_ends(self, program_run_id):
        return ()

    def resend_run_pause(self, program_run_id):
        self.resends.append(program_run_id)
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
    _halt(owner, run_id)
    assert mock.calls[-1][2:] and mock.calls[-1][3] is True
    halt_revision = mock.calls[-1][2]
    assert halt_revision > started.run_revision
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    resumed = owner.resume_halt(run_id)
    assert mock.calls[-1][:4] == ("pause", run_id, resumed.run_revision, False)
    assert resumed.run_revision > halt_revision
    count = len(mock.calls)
    _stop(owner, run_id)  # Teardown ends nodes (END), never a pause state.
    assert len(mock.calls) == count


def test_a_run_without_a_mock_port_runs_through_halt_resume_and_stop():
    """No Mock nodes, no Mock port, no I/O service: no Mock call."""
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    assert owner._ports.mock_simulation is None
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    _halt(owner, run_id)
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    resumed = owner.resume_halt(run_id)
    assert resumed.state is ProgramRunState.RUNNING
    owner.advance_once(run_id)
    _stop(owner, run_id)


def test_an_unconfirmed_mock_end_holds_the_run_short_of_terminal_until_unbind_succeeds():
    """The native unbind fails while a Mock END is unanswered.
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
    _scene_step_sent(owner, run_id)
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
    selection = run.operator_stop_scope().hold_revision
    mask["value"] = ("move_to_pick",)  # E.g. a Faulted settlement: no new
    starts_before = sum(event == "leaf_start" for event in events)  # selection revision.
    events.append("tick")
    after = owner.advance_once(run_id)
    snapshot_revision = mock.calls[-1][2]
    assert mock.calls[-1] == ("pause", run_id, snapshot_revision, False, ("move_to_pick",))
    assert before < snapshot_revision <= after.run_revision
    assert run.operator_stop_scope().hold_revision == selection
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
    _halt(owner, run_id)
    assert mock.calls[-1][3:] == (True, ("move_to_pick",))  # Both causes.
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    resumed = owner.resume_halt(run_id)
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
            entry.state = ProgramRunState.HALTING  # A concurrent Halt.
        return mask["value"]

    run.mock_held_nodes = halt_meanwhile
    mask["value"] = ("move_to_pick",)  # E.g. a failed Stop's Faulted settlement.
    owner.advance_once(run_id)
    assert mock.calls[-1][3:] == (True, ("move_to_pick",))
    # The selection Stop and release paths re-check RUNNING under the same
    # lock, so they never publish during a Halt; the helper covers them too.
    entry = owner._entry(run_id)
    for state, paused in ((ProgramRunState.HALTED, True),
                          (ProgramRunState.HALTING, True),
                          (ProgramRunState.RUNNING, False)):
        entry.state = state
        assert owner._mock_paused(entry) is paused
    # A run is halted only through applied Kernel hold facts (§6.3.2); the
    # concurrent Halt above is the Kernel's, so its facts are the applied ones.
    scope = owner._scope  # noqa: SLF001
    scope.kernel_halt()
    entry.applied_hold_facts = _kernel_facts(scope, run_id).reconciliation_key()
    entry.state = ProgramRunState.HALTING
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    resumed = owner.resume_halt(run_id)
    assert mock.calls[-1] == ("pause", run_id, resumed.run_revision, False, ("move_to_pick",))


def test_motion_stop_has_no_effect_while_a_stop_or_halt_contains_the_run():
    """A repeated Stop is never refused: once the run is stopping or
    halting, a motion Stop answers the unchanged snapshot, like Halt."""
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    halting = _halt(owner, run_id)
    assert halting.state is ProgramRunState.HALTING
    scope.last_selection_stop = None
    repeated = owner.stop_motion(
        run_id, reason="repeated Stop during Halt", stop_request_key=_key()
    )
    assert repeated.state is ProgramRunState.HALTING
    assert repeated.run_revision == halting.run_revision
    assert scope.last_selection_stop is None

    # A halted run (Halt finished) is contained: no effect either.
    halted = owner.reconcile_halting(run_id)
    assert halted.state is ProgramRunState.HALTED
    scope.last_selection_stop = None
    assert owner.stop_motion(run_id, stop_request_key=_key()) == halted
    assert scope.last_selection_stop is None


def test_motion_stop_on_an_ended_run_has_no_effect():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    ended = _stop(owner, run_id)
    assert ended.state is ProgramRunState.STOPPED
    scope.last_selection_stop = None
    assert owner.stop_motion(run_id, stop_request_key=_key()) == ended
    assert scope.last_selection_stop is None


def test_motion_stop_answers_the_current_run_after_the_kernel_accepted_it():
    """A Kernel selection error (it contained more: Stop all) or a failed
    hold projection after acceptance answers the run, not an error."""
    for failure in ("selection_error", "projection"):
        owner, admission, task, scope, events = _fixture(
            initial_task_state=OwnerTaskState.ACTIVE
        )
        run_id = owner.start(admission).capture.program_run_id
        owner.advance_once(run_id)
        stop = scope.stop_selection

        def accepted_with_failure(request, stop=stop, failure=failure):
            accepted = dict(stop(request))
            if failure == "selection_error":
                accepted["selection_error"] = {
                    "code": "request_id_conflict",
                    "message": "Program Stop identity was reused with a different selection",
                }
            else:
                accepted["state"] = None
            return accepted

        scope.stop_selection = accepted_with_failure
        replied = owner.stop_motion(run_id, stop_request_key=_key())
        assert replied.state is ProgramRunState.RUNNING, failure
        assert replied == owner.read(run_id), failure

    # Only a failed Kernel call itself leaves the outcome unknown.
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)

    def unreachable(request):
        raise TimeoutError("Kernel did not answer")

    scope.stop_selection = unreachable
    with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
        owner.stop_motion(run_id, stop_request_key=_key())


def test_motion_stop_has_no_effect_while_a_program_stop_ends_the_run():
    owner, admission, task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    stopping = _stop(owner, run_id)
    assert stopping.state is ProgramRunState.STOPPING
    scope.last_selection_stop = None
    repeated = owner.stop_motion(run_id, group_id=["all"], stop_request_key=_key())
    assert repeated.state is ProgramRunState.STOPPING
    assert repeated.run_revision == stopping.run_revision
    assert scope.last_selection_stop is None


def test_owner_forced_end_reuses_its_class_1_key_on_resend() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()

    _stop(owner, run_id)
    first = scope.last_stop_request[1]
    _stop(owner, run_id)
    assert scope.last_stop_request[1] == first
    assert key_class(first) is KeyClass.INTERNAL
    assert key_origin(first) == Minter.RUNNER
    host.shutdown()


def _count_scope_reads(scope: _ScopePort) -> list[str]:
    """Record every Kernel scope call the owner makes from now on."""
    calls: list[str] = []
    for name in ("request_stop", "observe", "execution_scope", "stop_selection"):
        original = getattr(scope, name)

        def counted(*args, _name=name, _original=original, **kwargs):
            calls.append(_name)
            return _original(*args, **kwargs)

        setattr(scope, name, counted)
    return calls


def test_halt_is_mirrored_from_kernel_hold_facts_without_an_owner_stop_call() -> None:
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING

    _host_halt(host, scope, run_id)
    # The stream thread only stores the facts and wakes the run.
    assert owner.read(run_id).state is ProgramRunState.RUNNING
    assert host.advance_ready_once()[0].state is ProgramRunState.HALTED
    assert scope.stop_requests == 0  # Kernel-first: no Stop call of its own
    # Halt freezes the Program clock and the tree is not ticked.
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    # The same facts again change nothing.
    host.apply_hold_facts(_kernel_facts(scope, run_id))
    assert host.advance_ready_once()[0].state is ProgramRunState.HALTED

    resumed = _on_tick(host, lambda: host.resume_halt(run_id))
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.halt_releases == [scope.hold_revision - 1]
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_halt_facts_are_memory_only_and_the_mock_pause_is_sent_on_the_tick() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    mock = _MockRunRecorder()
    owner._ports = replace(owner._ports, mock_simulation=mock)
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    assert host.advance_ready_once()[0].state is ProgramRunState.RUNNING
    sent, recorded, mock.resends[:] = len(mock.sent), len(mock.recorded), []
    scope.kernel_halt()
    calls = _count_scope_reads(scope)

    # The event-stream follower's call: memory only, no owner transition.
    host.apply_hold_facts(_kernel_facts(scope, run_id))
    assert calls == []
    assert len(mock.sent) == sent and len(mock.recorded) == recorded
    assert owner.read(run_id).state is ProgramRunState.RUNNING

    # Entering Halting on the tick records the pause only ...
    assert owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _kernel_facts(scope, run_id)
    )
    assert calls == []
    assert owner.read(run_id).state is ProgramRunState.HALTING
    assert [row[3] for row in mock.recorded[recorded:]] == [True]
    assert len(mock.sent) == sent and mock.resends == []
    # ... and the Halting tick sends it (reconcile_halting).
    owner.advance_once(run_id)
    assert mock.resends and set(mock.resends) == {run_id}
    assert "execution_scope" in calls and "observe" not in calls
    assert len(mock.sent) == sent
    host.shutdown()


@pytest.mark.parametrize("frozen", ["halt", "runner_loss"])
@pytest.mark.parametrize("intervention", [False, True])
def test_a_freeze_moves_an_active_run_to_halting_without_a_kernel_call(
    frozen, intervention
) -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=ProgramInterventionRegistry(maximum_retained_runs=8),
    )
    mock = _MockRunRecorder()
    owner._ports = replace(owner._ports, mock_simulation=mock)
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    if intervention:
        scope.state = ProgramRunScopeState.ACTIVE
        owner.stop_motion(run_id, stop_request_key=_key())
        owner.advance_once(run_id)
        scope.state = ProgramRunScopeState.SETTLED
    assert owner.read(run_id).state in {
        ProgramRunState.RUNNING,
        ProgramRunState.INTERVENTION_REQUIRED,
    }
    before = owner.read(run_id).run_revision
    calls = _count_scope_reads(scope)
    sent = len(mock.sent)

    halting = _halt(owner, run_id, frozen=frozen)

    assert halting.state is ProgramRunState.HALTING
    assert halting.run_revision > before
    assert calls == [] and scope.stop_requests == (1 if intervention else 0)
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    assert mock.recorded[-1][3] is True and len(mock.sent) == sent
    _stop(owner, run_id)


def test_a_halting_run_is_halted_once_its_scope_settles() -> None:
    # The Halt's Stop interrupts the started task, as the Kernel reports it.
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    _halt(owner, run_id)
    facts = _kernel_facts(scope, run_id)
    assert owner.advance_once(run_id, hold_facts=facts).state is ProgramRunState.HALTING
    scope.state = ProgramRunScopeState.SETTLED
    halted = owner.advance_once(run_id, hold_facts=facts)
    assert halted.state is ProgramRunState.HALTED
    assert halted.active_operation_count == 0
    # Halt on a halted run changes nothing.
    scope.kernel_halt()
    again = owner.advance_once(run_id, hold_facts=_kernel_facts(scope, run_id))
    assert again.state is ProgramRunState.HALTED
    assert scope.stop_requests == 0 and scope.release_requests == 0
    _stop(owner, run_id)


def _halted_with_a_faulted_node(owner, scope, run_id):
    """The Halt's Stop left a node faulted: Halting while another node is
    still stopping, Halted once none is, the failed Stop kept as reason."""

    _halt(owner, run_id)
    kernel_scope = scope.execution_scope

    def stopping_and_faulted(program_run_id):
        read = dict(kernel_scope(program_run_id))
        read["stopping_node_indices"], read["faulted_node_indices"] = [0], [1]
        return read

    scope.execution_scope = stopping_and_faulted
    halting = owner.reconcile_halting(run_id)
    assert halting.state is ProgramRunState.HALTING
    assert halting.reason_code == "program_run_stop_failed"
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001

    del scope.execution_scope
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_failed"
    assert scope.execution_scope(run_id)["faulted_node_indices"]
    assert not scope.execution_scope(run_id)["stopping_node_indices"]
    halted = owner.reconcile_halting(run_id)
    assert halted.state is ProgramRunState.HALTED
    assert halted.reason_code == "program_run_stop_failed"
    assert halted.active_operation_count == 0
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    assert scope.stop_requests == 0 and scope.release_requests == 0
    return halted


def test_a_halting_run_with_a_faulted_node_is_halted_once_none_is_stopping() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    halted = _halted_with_a_faulted_node(owner, scope, run_id)
    # Further Halting reads change nothing on the halted run.
    assert owner.reconcile_halting(run_id).run_revision == halted.run_revision
    _stop(owner, run_id)


def test_execute_after_a_halt_with_a_faulted_node_keeps_the_kernel_refusal() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    halted = _halted_with_a_faulted_node(owner, scope, run_id)
    # C3: the Kernel refuses the Halt release over a faulted node.
    scope.halt_refusal = ProgramSelectionReleaseRejectedError(
        "node is faulted", code="program_selection_recovery_required"
    )
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.resume_halt(run_id)
    assert not isinstance(refused.value, ProgramRunOwnerOutcomeUnknownError)
    assert getattr(refused.value, "code", None) == "program_selection_recovery_required"
    after = owner.read(run_id)
    assert after.state is ProgramRunState.HALTED
    assert after.run_revision == halted.run_revision
    assert after.reason_code == "program_run_stop_failed"
    assert owner._entry(run_id).own_release_revision == 0  # noqa: SLF001
    assert owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    assert scope.release_requests == 0
    _stop(owner, run_id)


def _run_in_state(start_state: ProgramRunState):
    """A run in the given state, its Kernel scope settled (Halted: the Halt's
    Stop left a node faulted)."""

    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    if start_state is ProgramRunState.INTERVENTION_REQUIRED:
        fixture = _fixture(
            initial_task_state=OwnerTaskState.FAILED,
            task_factory=lambda events: _ObservationBoundFailureTaskPort(
                events, deadline_ns=0
            ),
            interventions=interventions,
        )
    else:
        fixture = _fixture(
            initial_task_state=OwnerTaskState.ACTIVE,
            task_factory=_OperatorStopTaskPort,
            interventions=interventions,
        )
    owner, admission, _task, scope, _events = fixture
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    if start_state is ProgramRunState.HALTED:
        _halted_with_a_faulted_node(owner, scope, run_id)
    assert owner.read(run_id).state is start_state
    return owner, scope, run_id


def _reset_rest_completes(owner, scope, run_id) -> None:
    """The rest of a Program Reset after its Forced Program End: containment
    accepts the failed Stop, the operation Reset retires the failed operation
    (its fault goes with it) and the run's effects reset."""

    owner.begin_reset_containment(run_id, stop_request_key=_key())
    assert owner.reset_containment_settled(run_id)
    scope.state = ProgramRunScopeState.SETTLED
    reset = owner.reset_effects(run_id)
    assert reset.state is ProgramRunState.STOPPED
    assert scope.reset_requests == 1
    owner.forget_reset_run(run_id)


@pytest.mark.parametrize(
    "start_state",
    [
        ProgramRunState.RUNNING,
        ProgramRunState.INTERVENTION_REQUIRED,
        ProgramRunState.HALTED,
    ],
)
def test_reset_ends_a_run_whose_forced_end_left_a_node_faulted(start_state) -> None:
    owner, scope, run_id = _run_in_state(start_state)
    # Reset's Forced Program End: a participant's Stop fails, none stops.
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_failed"
    ended = _stop(
        owner, run_id, reason=PROGRAM_RESET_STOP_REASON, stop_request_key=_key()
    )
    assert scope.execution_scope(run_id)["faulted_node_indices"]
    assert ended.state is ProgramRunState.STOPPED
    assert ended.reason_code == "program_run_stop_failed"
    assert ended.active_operation_count == 0
    _reset_rest_completes(owner, scope, run_id)


def test_a_forced_end_with_a_faulted_node_waits_while_a_node_is_stopping() -> None:
    owner, scope, run_id = _run_in_state(ProgramRunState.RUNNING)
    scope.state = ProgramRunScopeState.ACTIVE
    stopping = _stop(
        owner, run_id, reason=PROGRAM_RESET_STOP_REASON, stop_request_key=_key()
    )
    assert stopping.state is ProgramRunState.STOPPING

    # One node's Stop failed while another is still stopping: no end yet.
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_failed"
    kernel_scope = scope.execution_scope

    def stopping_and_faulted(program_run_id):
        read = dict(kernel_scope(program_run_id))
        read["stopping_node_indices"], read["faulted_node_indices"] = [0], [1]
        return read

    scope.execution_scope = stopping_and_faulted
    still = owner.advance_once(run_id)
    assert still.state is ProgramRunState.STOPPING
    assert still.reason_code == "program_run_stop_failed"
    assert owner.advance_once(run_id).state is ProgramRunState.STOPPING

    # The other node stopped; the faulted one stays faulted: the run ends.
    del scope.execution_scope
    ended = owner.advance_once(run_id)
    assert ended.state is ProgramRunState.STOPPED
    assert ended.reason_code == "program_run_stop_failed"
    _reset_rest_completes(owner, scope, run_id)


def test_a_forced_end_keeps_the_runs_own_failure_over_the_failed_stop() -> None:
    owner, scope, run_id = _run_in_state(ProgramRunState.RUNNING)
    scope.state = ProgramRunScopeState.UNKNOWN
    scope.unknown_reason_code = "program_run_stop_failed"
    # The tree fails: the runner ends the run with its own failure.
    with owner._lock:  # noqa: SLF001
        run = owner._entry(run_id).run  # noqa: SLF001

    def failing_tick():
        raise RuntimeError("tree failed")

    run.tick_once = failing_tick
    ended = owner.advance_once(run_id)
    assert ended.state is ProgramRunState.FAILED
    assert ended.reason_code == "program_runtime_failed"


@pytest.mark.parametrize("settled", [False, True])
def test_a_released_freeze_returns_a_halting_or_halted_run_to_running(settled) -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    mock = _MockRunRecorder()
    owner._ports = replace(owner._ports, mock_simulation=mock)
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    if not settled:
        scope.state = ProgramRunScopeState.ACTIVE
    _halt(owner, run_id)
    expected = ProgramRunState.HALTED if settled else ProgramRunState.HALTING
    assert owner.reconcile_halting(run_id).state is expected
    halted = owner.read(run_id)

    # The Kernel released the freeze (facts not older than any own release).
    scope.frozen = "none"
    scope.hold_revision += 1
    scope.state = ProgramRunScopeState.SETTLED
    assert owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _kernel_facts(scope, run_id)
    )
    running = owner.read(run_id)
    assert running.state is ProgramRunState.RUNNING
    assert running.run_revision > halted.run_revision
    assert not owner._runs[run_id].run.program_clock.frozen  # noqa: SLF001
    assert mock.sent[-1][:4] == ("pause", run_id, running.run_revision, False)
    assert scope.release_requests == 0
    _stop(owner, run_id)


@pytest.mark.parametrize(
    ("frozen", "kind", "label"),
    [
        ("halt", "halt", "operator.halt"),
        ("halt", "internal_fault", "kernel.internal_fault"),
        ("halt", "stop_failed_halt", "kernel.stop_failed"),
        ("halt", "planned_stop", None),
        ("runner_loss", "runner_loss", None),
    ],
)
def test_a_halting_or_halted_run_publishes_the_cause_of_its_halt(frozen, kind, label) -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    assert owner.read(run_id).halt is None
    assert owner.read(run_id).to_dict()["halt"] is None
    scope.kernel_halt(frozen)
    facts = _kernel_facts(scope, run_id, frozen_kind=kind, frozen_source_label=label)
    owner._reconcile_hold_facts(run_id, facts)  # noqa: SLF001
    halting = owner.read(run_id)
    assert halting.state is ProgramRunState.HALTING
    assert halting.to_dict()["halt"] == {"kind": kind, "source_label": label}
    halted = owner.reconcile_halting(run_id)
    assert halted.state is ProgramRunState.HALTED
    assert halted.to_dict()["halt"] == {"kind": kind, "source_label": label}

    # Released: the run runs again and names no Halt.
    scope.frozen = "none"
    scope.hold_revision += 1
    owner._reconcile_hold_facts(run_id, _kernel_facts(scope, run_id))  # noqa: SLF001
    running = owner.read(run_id)
    assert running.state is ProgramRunState.RUNNING
    assert running.halt is None and running.to_dict()["halt"] is None
    _stop(owner, run_id)


def test_a_halt_without_a_freezing_kind_publishes_no_cause() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.kernel_halt()
    owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _kernel_facts(scope, run_id, frozen_kind=None)
    )
    halting = owner.read(run_id)
    assert halting.state is ProgramRunState.HALTING
    assert halting.to_dict()["halt"] is None
    # The kind arrives with the same freeze: the run republishes its cause.
    owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _kernel_facts(scope, run_id, frozen_source_label="operator.halt")
    )
    named = owner.read(run_id)
    assert named.run_revision == halting.run_revision + 1
    assert named.to_dict()["halt"] == {"kind": "halt", "source_label": "operator.halt"}
    _stop(owner, run_id)


def test_program_end_facts_change_nothing() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    before = owner.read(run_id)
    scope.hold_revision += 1
    owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _kernel_facts(scope, run_id, frozen="program_end", frozen_by=3)
    )
    after = owner.read(run_id)
    assert after.state is ProgramRunState.RUNNING
    assert after.run_revision == before.run_revision
    _stop(owner, run_id)


@pytest.mark.parametrize("frozen", ["halt", "runner_loss", "none", "program_end"])
def test_facts_change_nothing_while_the_run_is_ending(frozen) -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    stopping = _stop(owner, run_id)
    assert stopping.state is ProgramRunState.STOPPING
    scope.hold_revision += 1
    owner._reconcile_hold_facts(  # noqa: SLF001
        run_id,
        _kernel_facts(
            scope, run_id, frozen=frozen,
            frozen_by=None if frozen == "none" else 1,
        ),
    )
    after = owner.read(run_id)
    assert after.state is ProgramRunState.STOPPING
    assert after.run_revision == stopping.run_revision


def test_facts_older_than_an_accepted_execute_are_ignored() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    _halt(owner, run_id)
    stale = _kernel_facts(scope, run_id)
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    resumed = owner.resume_halt(run_id)
    assert resumed.state is ProgramRunState.RUNNING
    assert owner._entry(run_id).own_release_revision == scope.hold_revision  # noqa: SLF001

    # The Halt's facts arrive after Execute's own reply: ignored entirely.
    assert not owner._reconcile_hold_facts(run_id, stale)  # noqa: SLF001
    assert owner.read(run_id) == resumed
    # A newer Halt still applies.
    assert _halt(owner, run_id).state is ProgramRunState.HALTING
    _stop(owner, run_id)


def test_facts_older_than_an_accepted_selection_release_are_ignored() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    held = owner.execution_selection_controls(run_id)["hold_revision"]
    stale = _facts(run_id, hold_revision=held, frozen="halt", frozen_by=1)
    released = owner.start_motion(
        run_id, expected_hold_revision=held, request_key=_key()
    )
    assert released.state is ProgramRunState.RUNNING
    assert owner._entry(run_id).own_release_revision == held + 1  # noqa: SLF001

    assert not owner._reconcile_hold_facts(run_id, stale)  # noqa: SLF001
    assert owner.read(run_id).state is ProgramRunState.RUNNING
    newer = _facts(run_id, hold_revision=held + 1, frozen="halt", frozen_by=1)
    assert owner._reconcile_hold_facts(run_id, newer)  # noqa: SLF001
    assert owner.read(run_id).state is ProgramRunState.HALTING
    _stop(owner, run_id)


def test_refused_release_does_not_record_revision() -> None:
    # A refused selection release (Run/Resume) records nothing.
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    scope.ready = True
    scope.node_count = 3
    run_id = owner.start(admission).capture.program_run_id
    held = owner.execution_selection_controls(run_id)["hold_revision"]
    scope.attention_refusal = ProgramSelectionReleaseRejectedError(
        "operator alarm", code="program_selection_operator_attention_required"
    )
    with pytest.raises(ProgramRunOwnerError):
        owner.start_motion(run_id, expected_hold_revision=held, request_key=_key())
    assert owner._entry(run_id).own_release_revision == 0  # noqa: SLF001
    assert scope.hold_revision == held
    # Facts at the unchanged revision still apply.
    assert owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _facts(run_id, hold_revision=held, frozen="halt", frozen_by=1)
    )
    assert owner.read(run_id).state is ProgramRunState.HALTING
    _stop(owner, run_id)

    # A refused Halt release (Execute after Halt) records nothing either.
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    _halt(owner, run_id)
    halt_revision = scope.hold_revision
    assert owner.reconcile_halting(run_id).state is ProgramRunState.HALTED
    scope.halt_refusal = ProgramSelectionReleaseRejectedError(
        "hold revision changed", code="program_halt_release_stale"
    )
    with pytest.raises(ProgramRunOwnerError) as refused:
        owner.resume_halt(run_id)
    assert getattr(refused.value, "code", None) == "program_halt_release_stale"
    assert owner._entry(run_id).own_release_revision == 0  # noqa: SLF001
    assert owner.read(run_id).state is ProgramRunState.HALTED
    # Facts at the Halt's revision are still followed (not filtered).
    assert owner._reconcile_hold_facts(  # noqa: SLF001
        run_id, _facts(run_id, hold_revision=halt_revision)
    )
    assert owner.read(run_id).state is ProgramRunState.RUNNING
    _stop(owner, run_id)


def test_a_newer_freeze_of_a_halted_run_raises_its_run_revision() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    _halt(owner, run_id)
    halted = owner.reconcile_halting(run_id)
    assert halted.state is ProgramRunState.HALTED
    # The same facts again change nothing.
    owner._reconcile_hold_facts(run_id, _kernel_facts(scope, run_id))  # noqa: SLF001
    assert owner.read(run_id).run_revision == halted.run_revision
    # A newer Halt: a pending Execute (exact run revision) is now stale.
    after = _halt(owner, run_id)
    assert after.state is ProgramRunState.HALTED
    assert after.run_revision == halted.run_revision + 1
    resumed = owner.resume_halt(run_id)
    assert resumed.state is ProgramRunState.RUNNING
    assert scope.halt_releases == [scope.hold_revision - 1]
    _stop(owner, run_id)


def test_changed_hold_facts_are_read_on_the_next_tick_only() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    calls = _count_scope_reads(scope)
    refresh = owner.refresh_selection_scope
    refresh_reads: list[int] = []

    def spy(program_run_id):
        before = calls.count("execution_scope")
        snapshot = refresh(program_run_id)
        refresh_reads.append(calls.count("execution_scope") - before)
        return snapshot

    owner.refresh_selection_scope = spy

    facts = _facts(run_id, hold_revision=5, all_groups_held=True)
    host.apply_hold_facts(facts)
    assert calls == [] and refresh_reads == []

    owner.advance_once(run_id, hold_facts=facts)
    # The scope read happens inside the tick.
    assert len(refresh_reads) == 1 and refresh_reads[0] >= 1
    owner.advance_once(run_id, hold_facts=facts)
    assert len(refresh_reads) == 1
    # A new cell-wide event alone changes no hold: no read.
    owner.advance_once(run_id, hold_facts=replace(facts, last_stop_event_id=9))
    assert len(refresh_reads) == 1
    # A participant moving from stopping to faulted raises only the
    # progress sequence: read again.
    progressed = replace(facts, last_stop_event_id=9, stop_progress_sequence=1)
    owner.advance_once(run_id, hold_facts=progressed)
    assert len(refresh_reads) == 2
    for change in (
        {"hold_revision": 6},
        {"held_by_rule": "door"},
        {"all_groups_held": False},
    ):
        progressed = replace(progressed, **change)
        owner.advance_once(run_id, hold_facts=progressed)
    assert len(refresh_reads) == 5
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_hold_facts_for_a_run_not_yet_held_wait_for_its_first_tick() -> None:
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    scope.kernel_halt()
    host.apply_hold_facts(_kernel_facts(scope, 7))
    assert host._hold_facts[7].frozen == "halt"  # noqa: SLF001
    assert 7 not in host._scheduled  # noqa: SLF001

    started = host.start_run(admission, program_run_id=7)
    assert started.state is ProgramRunState.RUNNING
    assert host.advance_ready_once()[0].state in {
        ProgramRunState.HALTING,
        ProgramRunState.HALTED,
    }
    _on_tick(host, lambda: host.stop_run(7))
    host.shutdown()


def test_hold_facts_of_earlier_runs_the_owner_no_longer_holds_are_dropped() -> None:
    owner, admission, _task, _scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    host.apply_hold_facts(_facts(7, hold_revision=1))
    host.apply_hold_facts(_facts(8, hold_revision=2))
    assert set(host._hold_facts) == {8}  # noqa: SLF001
    host.shutdown()


def test_hold_facts_wake_a_held_run_and_leave_with_its_reset() -> None:
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission, defer_ticks=True).capture.program_run_id
    assert run_id not in host._scheduled  # noqa: SLF001
    # Before its explicit schedule a deferred run is not ticked: the facts
    # are kept for its first tick (the effectful tick follows Running).
    facts = _kernel_facts(scope, run_id)
    host.apply_hold_facts(facts)
    assert run_id not in host._scheduled  # noqa: SLF001
    assert host._hold_facts[run_id] == facts  # noqa: SLF001
    assert host.advance_ready_once() == ()
    assert task.starts == 0

    host.schedule(run_id)
    applied = []
    advance = owner.advance_once

    def spy(program_run_id, *, hold_facts=None):
        applied.append(hold_facts)
        return advance(program_run_id, hold_facts=hold_facts)

    owner.advance_once = spy
    snapshot = host.advance_ready_once()[0]
    while snapshot.state is ProgramRunState.RUNNING:
        snapshot = host.advance_ready_once()[0]
    assert snapshot.state is ProgramRunState.SUCCEEDED
    assert applied[0] == facts
    # Once scheduled, the run's facts wake it again.
    host.apply_hold_facts(facts)
    assert run_id in host._scheduled  # noqa: SLF001
    host.advance_ready_once()
    host.reset_run(run_id)
    host.forget_reset_run(run_id)
    assert run_id not in host._hold_facts  # noqa: SLF001
    host.shutdown()


def test_a_resent_resume_reuses_its_stopped_task_replan_key() -> None:
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
        interventions=ProgramInterventionRegistry(maximum_retained_runs=1),
    )
    run_id = owner.start(admission).capture.program_run_id
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.ACTIVE
    owner.stop_motion(run_id, stop_request_key=_key())
    owner.advance_once(run_id)
    scope.state = ProgramRunScopeState.SETTLED
    recover = task.recover
    lost = [2]

    def lossy(*args, **kwargs):
        result = recover(*args, **kwargs)
        if lost[0]:
            lost[0] -= 1
            raise ConnectionError("recover reply lost")
        return result

    task.recover = lossy
    first, second = _key(), _key()
    for key in (first, first):  # The same Resume request, sent again.
        with pytest.raises(ProgramRunOwnerOutcomeUnknownError):
            owner.resume_motion(
                run_id, expected_hold_revision=scope.hold_revision,
                request_key=key,
            )
    owner.resume_motion(
        run_id, expected_hold_revision=scope.hold_revision,
        request_key=second,
    )
    keys = [key for _task_id, key in task.recoveries]
    assert len(keys) == 3
    assert keys[0] == keys[1] != keys[2]
    assert first not in keys and second not in keys
    assert all(
        key_class(key) is KeyClass.INTERNAL and key_origin(key) == Minter.RUNNER
        for key in keys
    )


# OD-146: the Program tree is owned by its tick. Commands that waited for the
# tick in progress are handed to the run's tick; the request waits for the
# result. OD-152 (a): the status read uses the facts the run last learnt.


def _post(host: ProgramRunTickHost, command: Callable[[], Any]):
    """Start one queued host command on a request thread and return once it
    is posted to the run's tick. Returns (thread, outcome)."""

    owner = host._owner  # noqa: SLF001
    posted = Event()
    post = owner.post_to_tick

    def post_and_note(*args):
        intent = post(*args)
        posted.set()
        return intent

    owner.post_to_tick = post_and_note
    outcome: dict[str, Any] = {}

    def request() -> None:
        try:
            outcome["result"] = command()
        except Exception as error:
            outcome["error"] = error

    thread = Thread(target=request)
    try:
        thread.start()
        assert posted.wait(2.0)
    finally:
        del owner.post_to_tick
    return thread, outcome


def _answer(thread: Thread, outcome: dict[str, Any]) -> Any:
    thread.join(2.0)
    assert not thread.is_alive()
    if "error" in outcome:
        raise outcome["error"]
    return outcome["result"]


def _refusal(call: Callable[[], Any]) -> tuple[type, str, Any, Any]:
    with pytest.raises(ProgramRunOwnerError) as refused:
        call()
    error = refused.value
    return (
        type(error),
        str(error),
        getattr(error, "code", None),
        getattr(error, "details", None),
    )


@pytest.mark.parametrize(
    "command",
    [
        "resume_motion",
        "start_motion",
        "prepare_manual_handover",
        "retry_failure_pause",
        "continue_failure_pause",
        "resume_halt",
        "contain_reset_run",
    ],
)
def test_each_queued_command_is_applied_at_the_next_tick_with_its_former_answer(
    command,
):
    owner, admission, task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    stale = scope.hold_revision + 7
    owner_calls = {
        "resume_motion": lambda: owner.resume_motion(
            run_id, expected_hold_revision=stale, request_key=_key()
        ),
        "start_motion": lambda: owner.start_motion(
            run_id, expected_hold_revision=stale, request_key=_key()
        ),
        "prepare_manual_handover": lambda: owner.prepare_manual_handover(
            run_id,
            node_indices=(0,),
            expected_run_revision=owner.read(run_id).run_revision + 1,
            expected_hold_revision=scope.hold_revision,
            request_key=_key(),
        ),
        "retry_failure_pause": lambda: owner.retry_failure_pause(
            run_id, node_attempt_id=1, action=None, request_key=_key(), reason=None
        ),
        "continue_failure_pause": lambda: owner.continue_failure_pause(
            run_id, node_attempt_id=1, request_key=_key(), reason=None
        ),
        "resume_halt": lambda: owner.resume_halt(run_id),
        "contain_reset_run": lambda: owner.begin_reset_containment(
            run_id, stop_request_key=_key()
        ),
    }
    host_calls = {
        "resume_motion": lambda: host.resume_motion(
            run_id, expected_hold_revision=stale, request_key=_key()
        ),
        "start_motion": lambda: host.start_motion(
            run_id, expected_hold_revision=stale, request_key=_key()
        ),
        "prepare_manual_handover": lambda: host.prepare_manual_handover(
            run_id,
            node_indices=(0,),
            expected_run_revision=owner.read(run_id).run_revision + 1,
            expected_hold_revision=scope.hold_revision,
            request_key=_key(),
        ),
        "retry_failure_pause": lambda: host.retry_failure_pause(
            run_id, node_attempt_id=1, action=None, request_key=_key(), reason=None
        ),
        "continue_failure_pause": lambda: host.continue_failure_pause(
            run_id, node_attempt_id=1, request_key=_key(), reason=None
        ),
        "resume_halt": lambda: host.resume_halt(run_id),
        "contain_reset_run": lambda: host.contain_reset_run(
            run_id, stop_request_key=_key()
        ),
    }
    # The answer the command gave when it ran on the request thread.
    former = _refusal(owner_calls[command])
    revision = owner.read(run_id).run_revision

    thread, outcome = _post(host, host_calls[command])
    # Posted, not applied: nothing answers it before the run's next tick.
    assert thread.is_alive() and not outcome
    host.advance_ready_once()
    assert _refusal(lambda: _answer(thread, outcome)) == former
    assert owner.read(run_id).run_revision >= revision
    assert scope.release_requests == 0
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_a_queued_release_is_applied_at_the_next_tick_and_answers_its_result():
    owner, admission, _task, scope, events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission, defer_ticks=True).capture.program_run_id
    hold_revision = scope.hold_revision

    thread, outcome = _post(
        host,
        lambda: host.start_motion(
            run_id, expected_hold_revision=hold_revision, request_key=_key()
        ),
    )
    assert thread.is_alive() and scope.release_requests == 0
    host.advance_ready_once()
    released = _answer(thread, outcome)
    assert scope.release_requests == 1
    assert events.index("scope_stop_release") < events.index("leaf_start")
    assert released.state in {ProgramRunState.RUNNING, ProgramRunState.SUCCEEDED}
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_commands_for_runs_not_being_ticked_are_still_taken_and_answered():
    # Halted: the run is not ticked; Execute after the Halt wakes it.
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=_OperatorStopTaskPort,
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    run_id = host.start_run(admission).capture.program_run_id
    host.advance_ready_once()
    _host_halt(host, scope, run_id)
    assert host.advance_ready_once()[0].state is ProgramRunState.HALTED
    assert host.advance_ready_once() == ()
    host.start()
    try:
        assert host.resume_halt(run_id).state is ProgramRunState.RUNNING
        host.stop_run(run_id)
        terminal = host.wait_terminal(run_id)
        assert terminal.state in {ProgramRunState.STOPPED, ProgramRunState.FAILED}
        # Ended, awaiting Reset: a Forced Program End is still answered.
        assert host.stop_run(run_id) == host.read(run_id) == terminal
    finally:
        host.shutdown()


def test_a_retry_for_a_dormant_failure_hold_is_taken_and_answered():
    owner, _task, scope, interventions, run_id, failure = _failure_paused_run()
    host = ProgramRunTickHost(owner, tick_interval_s=0.001)
    expected = _refusal(
        lambda: owner.retry_failure_pause(
            run_id,
            node_attempt_id=failure.identity.node_attempt_id + 99,
            action=None,
            request_key=_key(),
            reason=None,
        )
    )
    host.start()
    try:
        assert _refusal(
            lambda: host.retry_failure_pause(
                run_id,
                node_attempt_id=failure.identity.node_attempt_id + 99,
                action=None,
                request_key=_key(),
                reason=None,
            )
        ) == expected
    finally:
        host.shutdown()


def test_runner_stop_is_sent_while_a_tick_is_blocked_and_marked_first():
    dispatch_entered, release_dispatch = Event(), Event()
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _BlockingTaskPort(
            events, dispatch_entered, release_dispatch
        ),
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert dispatch_entered.wait(1.0)

    sent = Event()
    state_at_send: list[ProgramRunState] = []
    request_stop = scope.request_stop

    def recorded(program_run_id, key, reason):
        state_at_send.append(owner.read(program_run_id).state)
        sent.set()
        return request_stop(program_run_id, key, reason)

    scope.request_stop = recorded
    results: list[Any] = []
    stopping = Thread(
        target=lambda: results.append(host.stop_run(run_id, reason="Reset"))
    )
    stopping.start()
    try:
        # The Stop reaches the Kernel while the tick is still blocked, and
        # the run was marked stopping before it was sent.
        assert sent.wait(1.0)
        assert advancing.is_alive() and not release_dispatch.is_set()
        assert state_at_send == [ProgramRunState.STOPPING]
        # Its tree part waits for the run's next tick.
        assert stopping.is_alive()
    finally:
        release_dispatch.set()
        advancing.join(2.0)
    assert not advancing.is_alive()
    host.advance_ready_once()
    stopping.join(2.0)
    assert not stopping.is_alive()
    assert results[0].state is ProgramRunState.STOPPED
    assert results[0].tree_status == "invalid"
    assert scope.stop_requests == 1
    host.shutdown()


def test_status_read_and_unqueued_commands_do_not_wait_for_a_blocked_tick():
    dispatch_entered, release_dispatch = Event(), Event()
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _BlockingTaskPort(
            events, dispatch_entered, release_dispatch
        ),
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert dispatch_entered.wait(1.0)

    def returns_promptly(call: Callable[[], Any]) -> None:
        done = Event()

        def run() -> None:
            try:
                call()
            except ProgramRunOwnerError:
                pass
            done.set()

        Thread(target=run, daemon=True).start()
        assert done.wait(0.5)

    try:
        # The status read behind the panel (reader R).
        returns_promptly(lambda: owner.read_selection_controls(run_id))
        # K6 owner recovery, K7 manual result, K8 value write: unchanged,
        # none waits for the tick.
        returns_promptly(
            lambda: owner.apply_owner_recovery(
                run_id,
                node_attempt_id=1,
                action="retry",
                request_key=_key(),
                reason=None,
            )
        )
        returns_promptly(
            lambda: owner.resolve_intervention(
                run_id, node_attempt_id=1, action="manual_success"
            )
        )
        returns_promptly(
            lambda: owner.write_runtime_blackboard(
                run_id, key="not_declared", value=1, request_key=_key()
            )
        )
        assert advancing.is_alive()
    finally:
        release_dispatch.set()
        advancing.join(2.0)
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_commands_posted_during_a_blocked_tick_follow_it_in_posting_order():
    dispatch_entered, release_dispatch = Event(), Event()
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.ACTIVE,
        task_factory=lambda events: _BlockingTaskPort(
            events, dispatch_entered, release_dispatch
        ),
    )
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission).capture.program_run_id
    advancing = Thread(target=host.advance_ready_once)
    advancing.start()
    assert dispatch_entered.wait(1.0)
    applied: list[str] = []
    for name in ("resume_motion", "retry_failure_pause"):
        original = getattr(owner, name)

        def recorded(*args, _name=name, _original=original, **kwargs):
            applied.append(_name)
            return _original(*args, **kwargs)

        setattr(owner, name, recorded)

    first = _post(
        host,
        lambda: host.resume_motion(
            run_id, expected_hold_revision=scope.hold_revision + 7, request_key=_key()
        ),
    )
    second = _post(
        host,
        lambda: host.retry_failure_pause(
            run_id, node_attempt_id=1, action=None, request_key=_key(), reason=None
        ),
    )
    try:
        assert applied == []
    finally:
        release_dispatch.set()
        advancing.join(2.0)
    # The blocked tick had passed its start: the next one applies both.
    assert applied == []
    host.advance_ready_once()
    assert applied == ["resume_motion", "retry_failure_pause"]
    with pytest.raises(ProgramRunOwnerError, match="stale selection"):
        _answer(*first)
    with pytest.raises(ProgramRunOwnerError, match="failure Retry attempt is stale"):
        _answer(*second)
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()


def test_status_read_makes_no_kernel_exchange():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    run_id = owner.start(admission).capture.program_run_id
    reads = _count_scope_reads(scope)
    _snapshot, controls = owner.read_selection_controls(run_id)
    assert reads == []
    assert controls["groups"][0]["start_allowed"] is True


def test_a_release_blocker_that_arrives_as_a_hold_change_is_shown():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission, defer_ticks=True).capture.program_run_id
    assert owner.execution_selection_controls(run_id)["groups"][0]["start_allowed"]

    # A Stop still settling elsewhere in the run: the Kernel raises the hold
    # revision and its stream wakes the run.
    scope.selection_release_block_reason = "stopping"
    scope.hold_revision += 1
    host.apply_hold_facts(_kernel_facts(scope, run_id))
    # Until the run's tick reads it, the status shows what the run last learnt.
    assert owner.execution_selection_controls(run_id)["groups"][0]["start_allowed"]
    # The deferred run's first tick follows its explicit schedule.
    host.schedule(run_id)
    host.advance_ready_once()
    group = owner.execution_selection_controls(run_id)["groups"][0]
    assert group["start_allowed"] is False
    assert group["start_unavailable_reason"] == "stopping"
    host.shutdown()


def test_the_kernels_refusal_in_the_admission_window_reaches_the_caller():
    owner, admission, _task, scope, _events = _fixture(
        initial_task_state=OwnerTaskState.SUCCEEDED
    )
    scope.ready = True
    host = ProgramRunTickHost(owner, tick_interval_s=0.01)
    run_id = host.start_run(admission, defer_ticks=True).capture.program_run_id
    # One of the run's own admissions is in progress: the Kernel's scope
    # would say so, but no hold change tells the run; the status shows the
    # release as possible (OD-152 (a)).
    scope.selection_release_block_reason = "program_run_admission_pending"
    assert owner.execution_selection_controls(run_id)["groups"][0]["start_allowed"]
    details = {
        "reason": "program_selection_not_releasable",
        "selection_release_block_reason": "program_run_admission_pending",
    }

    def refuse(_request, _expected_hold_revision):
        raise ProgramSelectionReleaseRejectedError(
            "A Program-run admission is pending; Program Run or Resume remains "
            "blocked",
            code="invalid_state_transition",
            details=details,
        )

    scope.release_selection = refuse
    with pytest.raises(ProgramRunOwnerError) as refused:
        _on_tick(
            host,
            lambda: host.start_motion(
                run_id, expected_hold_revision=scope.hold_revision, request_key=_key()
            ),
        )
    assert refused.value.code == "invalid_state_transition"
    assert refused.value.details == details
    _on_tick(host, lambda: host.stop_run(run_id))
    host.shutdown()
