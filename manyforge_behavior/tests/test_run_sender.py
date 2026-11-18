"""OD-213: the per-run sender, and the nodes whose slow request leaves the tick.

Scene nodes and `io.set_output` (an owner invocation), Set TCP (an owner
task start) and the pneumatic gripper's valve writes: the tick hands the
request to the run's sender and the node is RUNNING until its answer, read
on a later tick. A Halt or an operator Stop that holds the node withdraws a
request that is not sent yet (it is never sent; the node stays where it is
and sends it when ticked again); a sent request completes and the node
returns its real result, never `interrupted`.
"""

from __future__ import annotations

from threading import Event, Thread
import time
from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.kernel_client import KernelOperationAdmissionRejectedError
from manyforge_behavior.nodes.v2_owner import (
    GripperCommandAdapter,
    OwnerInvocationAdapter,
    OwnerTaskAdapter,
)
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationPendingError,
    OwnerInvocationResult,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.run_sender import (
    RunRequestState,
    RunSender,
    RunSenderError,
)

RUNNING = py_trees.common.Status.RUNNING
SUCCESS = py_trees.common.Status.SUCCESS
FAILURE = py_trees.common.Status.FAILURE
INVALID = py_trees.common.Status.INVALID


class _Spawn:
    """The sender's thread start, held until the test lets it run."""

    def __init__(self) -> None:
        self.pending: list = []
        self.threads: list[Thread] = []

    def __call__(self, name, work) -> None:
        self.pending.append(work)

    def run(self) -> None:
        """Empty the queue on this thread (calls that do not block)."""
        while self.pending:
            self.pending.pop(0)()

    def start(self) -> None:
        """Empty the queue on a thread of its own (a call that blocks)."""
        thread = Thread(target=self.pending.pop(0), daemon=True)
        thread.start()
        self.threads.append(thread)


def _sender(capacity: int = 4) -> tuple[RunSender, _Spawn]:
    spawn = _Spawn()
    return RunSender(capacity, spawn=spawn), spawn


def _eventually(condition) -> None:
    deadline = time.monotonic() + 5.0
    while not condition():
        if time.monotonic() > deadline:
            pytest.fail("condition not reached")
        time.sleep(0.001)


class _Typed(RuntimeError):
    def __init__(self, code: str) -> None:
        super().__init__(f"refused: {code}")
        self.code = code


# --- The sender ------------------------------------------------------------


def test_requests_are_sent_one_at_a_time_in_submission_order() -> None:
    sender, spawn = _sender()
    calls: list[str] = []
    first = sender.submit(lambda: calls.append("a") or "A")
    second = sender.submit(lambda: calls.append("b") or "B")
    assert calls == [] and first.state is RunRequestState.QUEUED and not first.sent
    assert len(spawn.pending) == 1  # One thread for the queue.
    spawn.run()
    assert calls == ["a", "b"]
    assert first.answered and first.result() == "A" and second.result() == "B"
    # No thread is kept while idle: the next request starts a new one.
    sender.submit(lambda: None)
    assert len(spawn.pending) == 1


def test_a_queued_request_is_withdrawn_and_never_sent() -> None:
    sender, spawn = _sender()
    calls: list[str] = []
    request = sender.submit(lambda: calls.append("write"))
    assert request.withdraw() is True and request.withdrawn
    assert request.withdraw() is True  # Already withdrawn.
    spawn.run()
    assert calls == []
    with pytest.raises(RunSenderError):
        request.result()


def test_a_sent_request_cannot_be_withdrawn_and_completes() -> None:
    sender, spawn = _sender()
    entered, release = Event(), Event()

    def write() -> str:
        entered.set()
        assert release.wait(5)
        return "written"

    request = sender.submit(write)
    spawn.start()
    assert entered.wait(5)
    assert request.sent and not request.answered
    assert request.withdraw() is False
    release.set()
    _eventually(lambda: request.answered)
    assert request.result() == "written"


def test_a_failed_call_is_the_answer_with_its_typed_code() -> None:
    sender, spawn = _sender()
    request = sender.submit(lambda: (_ for _ in ()).throw(_Typed("io_output_write_failed")))
    spawn.run()
    assert request.state is RunRequestState.FAILED
    assert request.failure_code == "io_output_write_failed"
    with pytest.raises(_Typed):
        request.result()


def test_close_withdraws_what_is_queued_and_refuses_more_a_sent_one_completes() -> None:
    sender, spawn = _sender()
    entered, release = Event(), Event()
    calls: list[str] = []

    def first() -> None:
        entered.set()
        assert release.wait(5)
        calls.append("first")

    sent = sender.submit(first)
    queued = sender.submit(lambda: calls.append("second"))
    spawn.start()
    assert entered.wait(5)
    sender.close()
    assert queued.withdrawn
    with pytest.raises(RunSenderError):
        sender.submit(lambda: None)
    release.set()
    _eventually(lambda: sent.answered)
    spawn.threads[0].join(5)
    assert calls == ["first"]


def test_withdraw_queued_leaves_the_sender_open() -> None:
    sender, spawn = _sender()
    queued = sender.submit(lambda: None)
    sender.withdraw_queued()
    assert queued.withdrawn
    again = sender.submit(lambda: "again")
    spawn.run()
    assert again.result() == "again"


def test_the_sender_is_bounded_by_its_node_count() -> None:
    sender, _spawn = _sender(capacity=1)
    sender.submit(lambda: None)
    with pytest.raises(RunSenderError):
        sender.submit(lambda: None)


# --- io.set_output and the Scene nodes (owner invocations) -----------------


class _Port:
    """An owner invocation port; ``gate`` holds the call once entered."""

    def __init__(self, result=None, *, gate: Event | None = None) -> None:
        self.result = OwnerInvocationResult(True) if result is None else result
        self.gate = gate
        self.entered = Event()
        self.calls: list[str] = []

    def invoke(self, node_id, identity, inputs):
        self.calls.append(node_id)
        self.entered.set()
        if self.gate is not None:
            assert self.gate.wait(5)
        if isinstance(self.result, Exception):
            raise self.result
        return self.result


def _identity(instance: str = "write", attempt: int = 1) -> NodeAttemptIdentity:
    return NodeAttemptIdentity(1, instance, attempt)


def _invocation(port, sender, node_id: str = "io.set_output") -> OwnerInvocationAdapter:
    adapter = OwnerInvocationAdapter(
        name="write", node_id=node_id, node_instance_id="write",
        port=port, parameters={"value": True}, value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={},
        )),
        cancellable=node_id == "io.set_output",
        run_sender=sender,
    )
    adapter.setup_with_descendants()
    return adapter


@pytest.mark.parametrize("node_id", ["io.set_output", "scene.add_object"])
def test_the_node_is_running_until_its_answer(node_id) -> None:
    sender, spawn = _sender()
    port = _Port()
    node = _invocation(port, sender, node_id)
    node.tick_once()
    assert node.status is RUNNING and port.calls == []  # Nothing inside the tick.
    node.tick_once()
    assert node.status is RUNNING
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.calls == [node_id]


def test_a_failed_write_returns_the_device_code_never_resent() -> None:
    sender, spawn = _sender()
    port = _Port(OwnerInvocationResult(False, reason_code="io_output_write_failed", message="no"))
    node = _invocation(port, sender)
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is FAILURE
    assert node.failure_reason_code == "io_output_write_failed"
    assert port.calls == ["io.set_output"]


def test_a_raised_request_fails_as_before() -> None:
    sender, spawn = _sender()
    node = _invocation(_Port(RuntimeError("socket closed")), sender)
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is FAILURE and node.failure_reason_code == "owner_request_failed"


def test_an_unconfirmed_scene_outcome_is_read_on_the_sender_never_resent() -> None:
    sender, spawn = _sender()
    reads: list[object] = [None, OwnerInvocationResult(True)]

    def observe():
        return reads.pop(0)

    port = _Port(OwnerInvocationPendingError("Scene transaction outcome is unconfirmed", observe))
    node = _invocation(port, sender, "scene.add_object")
    node.tick_once()
    spawn.run()
    node.tick_once()  # The commit's answer: its outcome is unconfirmed.
    assert node.status is RUNNING and len(reads) == 2
    node.tick_once()  # The read of that outcome is queued, not made in the tick.
    assert len(reads) == 2
    spawn.run()
    assert len(reads) == 1
    node.tick_once()  # Still unconfirmed: the next read is queued at once.
    assert node.status is RUNNING
    spawn.run()
    assert reads == []
    node.tick_once()
    assert node.status is SUCCESS
    assert port.calls == ["scene.add_object"]


def test_halt_before_the_write_was_sent_withdraws_it_execute_sends_it() -> None:
    sender, spawn = _sender()
    port = _Port()
    node = _invocation(port, sender)
    node.tick_once()
    assert node.reconcile_halt_task() == "unaffected"
    spawn.run()
    assert port.calls == []  # Not sent.
    node.tick_once()  # Execute: the same attempt sends it.
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.calls == ["io.set_output"]


def test_halt_withdrawal_by_the_run_is_resent_after_execute() -> None:
    sender, spawn = _sender()
    port = _Port()
    node = _invocation(port, sender)
    node.tick_once()
    sender.withdraw_queued()  # The run entering Halting.
    spawn.run()
    assert port.calls == []
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.calls == ["io.set_output"]


def test_halt_after_the_write_was_sent_waits_for_its_real_result() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _Port(OwnerInvocationResult(False, reason_code="io_output_write_failed", message="no"),
                 gate=gate)
    node = _invocation(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    assert node.reconcile_halt_task() == "active"  # The run stays Halting.
    gate.set()
    _eventually(lambda: node.reconcile_halt_task() == "unaffected")
    node.tick_once()  # First tick after Execute: the real result.
    assert node.status is FAILURE
    assert node.failure_reason_code == "io_output_write_failed"
    assert node.outcome == "failure"
    assert port.calls == ["io.set_output"]


def test_held_before_the_write_was_sent_it_is_never_sent_while_held() -> None:
    sender, spawn = _sender()
    port = _Port()
    node = _invocation(port, sender)
    node.tick_once()
    node.observe_while_operator_stop_held()
    node.observe_while_operator_stop_held()
    spawn.run()
    assert port.calls == [] and node.status is RUNNING
    assert node.observe_control_state() == "inactive"
    node.tick_once()  # Run/Resume: the node is ticked again and sends it.
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.calls == ["io.set_output"]


def test_held_after_the_write_was_sent_it_ends_with_the_real_result() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _Port(gate=gate)
    node = _invocation(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.observe_while_operator_stop_held()
    assert node.status is RUNNING and node.has_active_task
    gate.set()
    _eventually(lambda: (node.observe_while_operator_stop_held(), node.status)[1] is SUCCESS)
    assert port.calls == ["io.set_output"]


def test_forced_end_before_the_write_was_sent_sends_nothing() -> None:
    sender, spawn = _sender()
    port = _Port()
    node = _invocation(port, sender)
    node.tick_once()
    node.stop(INVALID)
    sender.close()
    spawn.run()
    assert port.calls == []
    assert node.settle_abandoned_request() is True


def test_forced_end_after_the_write_was_sent_returns_the_real_result() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _Port(OwnerInvocationResult(False, reason_code="io_output_write_failed", message="no"),
                 gate=gate)
    node = _invocation(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    sender.close()
    assert node.settle_abandoned_request() is False  # The run stays Stopping.
    gate.set()
    _eventually(node.settle_abandoned_request)
    assert node.status is FAILURE and node.outcome == "failure"
    assert node.failure_reason_code == "io_output_write_failed"
    assert node.interrupted_reason_code is None


def test_without_a_sender_the_call_is_made_in_the_tick() -> None:
    port = _Port()
    node = _invocation(port, None)
    node.tick_once()
    assert node.status is SUCCESS and port.calls == ["io.set_output"]


# --- Set TCP (an owner task start) -----------------------------------------


class _TcpPort:
    def __init__(self, *, gate: Event | None = None, error: Exception | None = None) -> None:
        self.gate = gate
        self.error = error
        self.entered = Event()
        self.starts = 0
        self.acks: list[str] = []

    def start(self, node_id, identity, inputs):
        self.starts += 1
        self.entered.set()
        if self.gate is not None:
            assert self.gate.wait(5)
        if self.error is not None:
            raise self.error
        return OwnerTaskSnapshot("tcp", OwnerTaskState.SUCCEEDED, settlement_proven=True)

    def acknowledge(self, task_id, identity):
        self.acks.append(task_id)

    def observe(self, task_id):
        raise AssertionError("an immediate step is never observed")

    cancel = observe


def _set_tcp(port, sender) -> OwnerTaskAdapter:
    adapter = OwnerTaskAdapter(
        name="tcp", node_id="motion.set_tcp", node_instance_id="tcp",
        port=port, owner_port_name="kernel_operations", 
        parameters={}, value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={},
        )),
        cancellable=True,
        run_sender=sender,
    )
    adapter.setup_with_descendants()
    return adapter


def test_set_tcp_is_running_until_its_answer_one_tick_later() -> None:
    sender, spawn = _sender()
    port = _TcpPort()
    node = _set_tcp(port, sender)
    node.tick_once()
    assert node.status is RUNNING and port.starts == 0
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.acks == ["tcp"] and port.starts == 1


def test_set_tcp_refusal_keeps_its_kernel_code() -> None:
    sender, spawn = _sender()
    port = _TcpPort(error=KernelOperationAdmissionRejectedError(
        "rule_holds", "Rule rule.1 holds its scope", {"rule_id": "rule.1"}))
    node = _set_tcp(port, sender)
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is FAILURE and node.failure_reason_code == "rule_holds"


def test_set_tcp_halt_before_sent_withdraws_then_sends_after_execute() -> None:
    sender, spawn = _sender()
    port = _TcpPort()
    node = _set_tcp(port, sender)
    node.tick_once()
    assert node.reconcile_halt_task() == "inactive"
    spawn.run()
    assert port.starts == 0
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.starts == 1


def test_set_tcp_halt_after_sent_keeps_halting_until_the_answer() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _TcpPort(gate=gate)
    node = _set_tcp(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    assert node.reconcile_halt_task() == "active"
    gate.set()
    _eventually(lambda: node.reconcile_halt_task() == "unaffected")
    node.tick_once()
    assert node.status is SUCCESS and port.starts == 1


def test_set_tcp_held_before_sent_is_not_sent_held_after_sent_completes() -> None:
    sender, spawn = _sender()
    port = _TcpPort()
    node = _set_tcp(port, sender)
    node.tick_once()
    node.observe_while_operator_stop_held()
    spawn.run()
    assert port.starts == 0 and node.status is RUNNING
    gate = Event()
    port.gate = gate
    node.tick_once()  # Released: sent now.
    spawn.start()
    assert port.entered.wait(5)
    node.observe_while_operator_stop_held()
    assert node.status is RUNNING and node.has_active_task
    gate.set()
    _eventually(lambda: (node.observe_while_operator_stop_held(), node.status)[1] is SUCCESS)
    assert port.starts == 1 and port.acks == ["tcp"]


def test_set_tcp_forced_end_before_sent_is_interrupted_after_sent_real_result() -> None:
    sender, spawn = _sender()
    port = _TcpPort()
    node = _set_tcp(port, sender)
    node.tick_once()
    node.stop(INVALID)
    spawn.run()
    assert port.starts == 0 and node.outcome == "interrupted"

    sender, spawn = _sender()
    gate = Event()
    port = _TcpPort(gate=gate)
    node = _set_tcp(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    assert node.interrupted_reason_code is None
    assert node.settle_abandoned_request() is False
    gate.set()
    _eventually(node.settle_abandoned_request)
    assert node.status is SUCCESS and node.outcome == "success" and port.acks == ["tcp"]


# --- The pneumatic gripper's valve writes ----------------------------------


class _Valves:
    def __init__(self, results=None, *, gate: Event | None = None) -> None:
        self.results = results or {}
        self.gate = gate
        self.entered = Event()
        self.writes: list[str] = []

    def invoke(self, node_id, identity, inputs):
        assert node_id == "io.set_output"
        role = inputs.parameters["resource_role"]
        self.writes.append(role)
        self.entered.set()
        if self.gate is not None:
            assert self.gate.wait(5)
        return self.results.get(role, OwnerInvocationResult(True))


class _Operations:
    def __init__(self) -> None:
        self.starts = 0

    def pneumatic_plan(self, identity, inputs):
        return {
            "outputs": [
                {"target_role": "io", "resource_role": "valve-a", "value": False},
                {"target_role": "io", "resource_role": "valve-b", "value": True},
            ],
            "inputs": [],
            "logical": OwnerNodeInputs({"position": {"kind": "linear_m", "value_m": 0.0}}),
            "dwell_s": 0.0,
        }

    def start(self, node_id, identity, inputs):
        self.starts += 1
        return OwnerTaskSnapshot("gripper", OwnerTaskState.SUCCEEDED)

    def acknowledge(self, task_id, identity):
        pass


def _gripper(valves, sender) -> tuple[GripperCommandAdapter, _Operations]:
    operations = _Operations()
    adapter = GripperCommandAdapter(
        name="close", node_id="gripper.command_position", node_instance_id="close",
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        parameters={"dwell_while_held": "pause"}, value_sources={},
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={})),
        port=operations, owner_port_name="kernel_operations",
        program_clock=ProgramClock(lambda: 0.0),
        io_watches=SimpleNamespace(), io_signals=valves,
        cancellable=True, valve_sender=sender,
    )
    adapter.setup_with_descendants()
    return adapter, operations


def test_valve_writes_leave_the_tick_one_at_a_time() -> None:
    sender, spawn = _sender()
    valves = _Valves()
    node, operations = _gripper(valves, sender)
    node.tick_once()
    assert valves.writes == [] and node.status is RUNNING
    spawn.run()
    node.tick_once()
    assert valves.writes == ["valve-a"]
    spawn.run()
    for _ in range(5):
        node.tick_once()
        if node.status is SUCCESS:
            break
    assert valves.writes == ["valve-a", "valve-b"]
    assert node.status is SUCCESS and operations.starts == 1


def test_a_failed_valve_write_fails_with_its_code_without_later_writes() -> None:
    sender, spawn = _sender()
    valves = _Valves({"valve-a": OwnerInvocationResult(
        False, reason_code="io_output_write_failed", message="no")})
    node, _operations = _gripper(valves, sender)
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert node.status is FAILURE and node.failure_reason_code == "io_output_write_failed"
    assert valves.writes == ["valve-a"]


def test_held_after_a_valve_write_was_sent_it_completes_and_stops_there() -> None:
    sender, spawn = _sender()
    gate = Event()
    valves = _Valves(gate=gate)
    node, _operations = _gripper(valves, sender)
    node.tick_once()
    spawn.start()
    assert valves.entered.wait(5)
    node.observe_while_operator_stop_held()
    gate.set()
    _eventually(lambda: (node.observe_while_operator_stop_held(), node._request)[1] is None)
    node.observe_while_operator_stop_held()
    assert node.status is RUNNING and valves.writes == ["valve-a"]  # valve-b waits.
    node.tick_once()  # Released: the next write is sent.
    spawn.start()
    _eventually(lambda: valves.writes == ["valve-a", "valve-b"])


def test_halt_before_a_valve_write_was_sent_resends_the_same_valve() -> None:
    sender, spawn = _sender()
    valves = _Valves()
    node, _operations = _gripper(valves, sender)
    node.tick_once()
    assert node.reconcile_halt_task() == "inactive"
    spawn.run()
    assert valves.writes == []
    node.tick_once()
    spawn.run()
    node.tick_once()
    assert valves.writes == ["valve-a"]


def test_forced_end_after_a_valve_write_was_sent_returns_its_failure() -> None:
    sender, spawn = _sender()
    gate = Event()
    valves = _Valves({"valve-a": OwnerInvocationResult(
        False, reason_code="io_output_write_failed", message="no")}, gate=gate)
    node, _operations = _gripper(valves, sender)
    node.tick_once()
    spawn.start()
    assert valves.entered.wait(5)
    node.stop(INVALID)
    assert node.settle_abandoned_request() is False
    gate.set()
    _eventually(node.settle_abandoned_request)
    assert node.status is FAILURE and node.failure_reason_code == "io_output_write_failed"
