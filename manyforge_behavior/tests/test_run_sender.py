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


def test_a_program_stop_withdraws_only_the_queued_starts_it_covers() -> None:
    sender, spawn = _sender()
    sent: list[str] = []
    write = sender.submit(lambda: sent.append("write"))
    value = sender.submit(lambda: sent.append("value"), withdrawable=False)
    move_a = sender.submit(lambda: sent.append("a"), start_target={"id": "a"})
    move_b = sender.submit(lambda: sent.append("b"), start_target={"id": "b"})
    assert sender.stop_starts({"id": "a"}) is False  # `kernel.stop_operation` on a.
    assert move_a.withdrawn and move_a.withdrawn_by_program_stop
    assert not move_b.withdrawn
    assert sender.stop_starts() is False  # `kernel.cell_stop`: every target.
    assert move_b.withdrawn_by_program_stop
    spawn.run()
    assert sent == ["write", "value"]  # Nothing else is withdrawn.
    assert not write.withdrawn_by_program_stop and not value.withdrawn
    halted = sender.submit(lambda: None, start_target={"id": "a"})
    sender.withdraw_queued()  # A Halt's withdrawal is no Program Stop's.
    assert halted.withdrawn and not halted.withdrawn_by_program_stop
    with pytest.raises(ValueError):
        sender.submit(lambda: None, withdrawable=False, start_target={"id": "a"})


def test_a_program_stop_follows_a_start_in_flight_until_its_answer() -> None:
    sender, spawn = _sender()
    entered, release = Event(), Event()
    write_entered, write_release = Event(), Event()

    def start() -> str:
        entered.set()
        assert release.wait(5)
        return "admitted"

    def write() -> None:
        write_entered.set()
        assert write_release.wait(5)

    move = sender.submit(start, start_target={"id": "a"})
    spawn.start()
    assert entered.wait(5)
    assert sender.stop_starts({"id": "b"}) is False  # Another target's.
    assert sender.stop_starts() is True and move.sent and not move.withdrawn
    release.set()
    _eventually(lambda: move.answered)
    assert sender.stop_starts() is False and move.result() == "admitted"
    sender.submit(write)  # An I/O write in flight is never waited for.
    spawn.start()
    assert write_entered.wait(5)
    assert sender.stop_starts() is False
    write_release.set()


def test_a_command_applied_in_place_makes_its_requests_inside_it_first() -> None:
    # Base: a Resume, Run, Retry or Execute made its owner calls inside the
    # tick, ahead of the run's queued requests (also a not-withdrawable frame).
    sender, spawn = _sender()
    sent: list[str] = []
    frame = sender.submit(lambda: sent.append("frame"), withdrawable=False)

    def resume() -> str:
        read = sender.submit(lambda: sent.append("read") or "scope")
        assert read.answered  # Made in place, before submit returned.
        return read.result()

    assert sender.apply_in_place(resume) == "scope"
    assert sent == ["read"] and not frame.answered
    spawn.run()
    assert sent == ["read", "frame"]
    sender.close()
    assert sender.apply_in_place(lambda: sender.submit(lambda: "after").result()) == "after"
    with pytest.raises(RunSenderError):
        sender.submit(lambda: None)  # Outside the command: refused after close.


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
    # Its acknowledgement leaves the tick too (OD-380 D2).
    assert node.status is RUNNING and port.acks == [] and port.starts == 1
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
    node.tick_once()  # The answer; its acknowledgement is sent.
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
    node.tick_once()  # The answer; its acknowledgement is sent.
    spawn.run()
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
    # The answer, then its acknowledgement on the sender, both while held.
    _eventually(
        lambda: (node.observe_while_operator_stop_held(), spawn.run(), node.status)[2] is SUCCESS
    )
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
    sender.close()  # The Forced End closes the run's sender.
    assert node.interrupted_reason_code is None
    assert node.settle_abandoned_request() is False
    gate.set()
    spawn.threads[0].join(5)
    # The answer; its acknowledgement leaves the tick too, after the close
    # (OD-380 D2), and the node ends once it is answered.
    assert node.settle_abandoned_request() is False
    assert port.acks == [] and node.status is not SUCCESS
    spawn.run()
    assert node.settle_abandoned_request() is True
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


def test_a_not_withdrawable_request_survives_halt_and_close_and_the_bound() -> None:
    # A value publication is state: a Halt or the run's end never withdraws
    # it, and it does not count against the node bound.
    sender, spawn = _sender(capacity=1)
    sent: list[str] = []
    node = sender.submit(lambda: sent.append("node"))
    publication = sender.submit(lambda: sent.append("values"), withdrawable=False)
    assert publication.withdraw() is False and not publication.withdrawn
    sender.withdraw_queued()
    assert node.withdrawn and not publication.withdrawn
    later = sender.submit(lambda: sent.append("values 2"), withdrawable=False)
    sender.close()
    with pytest.raises(RunSenderError):
        sender.submit(lambda: None)
    spawn.run()
    assert sent == ["values", "values 2"]
    assert publication.answered and later.answered


def test_after_close_only_a_not_withdrawable_request_is_accepted_and_sent() -> None:
    # The run's end still settles off the tick (its Stop's reads, a stopped
    # node's acknowledgement): sent in order, answered, never withdrawn.
    sender, spawn = _sender(capacity=1)
    sent: list[str] = []
    sender.close()
    with pytest.raises(RunSenderError):
        sender.submit(lambda: sent.append("node"))
    read = sender.submit(lambda: sent.append("read") or "scope", withdrawable=False)
    sender.withdraw_queued()
    assert read.withdraw() is False and read.state is RunRequestState.QUEUED
    ack = sender.submit(lambda: sent.append("ack"), withdrawable=False)
    spawn.run()
    assert sent == ["read", "ack"]
    assert read.result() == "scope" and ack.answered
    # Nothing is kept once the queue is empty: a later one starts a thread.
    again = sender.submit(lambda: sent.append("read 2"), withdrawable=False)
    assert len(spawn.pending) == 1
    spawn.run()
    assert again.answered and sent[-1] == "read 2"


# --- S2: answers wake the run; operation and planning-job watches ----------


class _Wakes:
    """A wake set that records what the sender asks of it (in order)."""

    def __init__(self) -> None:
        self.calls: list[tuple] = []
        self.answered_at_wake: list[bool] = []
        self.request = None

    def wake(self, run_id):
        self.calls.append(("wake", run_id))
        if self.request is not None:
            self.answered_at_wake.append(self.request.answered)

    def watch_operation(self, run_id, operation_id):
        self.calls.append(("watch_operation", run_id, operation_id))

    def watch_planning_job(self, run_id, job_id):
        self.calls.append(("watch_planning_job", run_id, job_id))

    def unwatch_operation(self, run_id, operation_id):
        self.calls.append(("unwatch_operation", run_id, operation_id))

    def unwatch_planning_job(self, run_id, job_id):
        self.calls.append(("unwatch_planning_job", run_id, job_id))

    def unwatch_run(self, run_id):
        self.calls.append(("unwatch_run", run_id))


def _waking_sender(capacity: int = 4) -> tuple[RunSender, _Spawn, _Wakes]:
    spawn, wakes = _Spawn(), _Wakes()
    return RunSender(capacity, program_run_id=7, wake_set=wakes, spawn=spawn), spawn, wakes


def test_each_answer_wakes_its_run_after_it_is_recorded() -> None:
    sender, spawn, wakes = _waking_sender()
    wakes.request = sender.submit(lambda: "A")
    spawn.run()
    failed = sender.submit(lambda: (_ for _ in ()).throw(_Typed("io_output_write_failed")))
    wakes.request = failed
    spawn.run()
    assert wakes.calls == [("wake", 7), ("wake", 7)]
    assert wakes.answered_at_wake == [True, True]  # The tick it wakes reads it.
    assert failed.failure_code == "io_output_write_failed"


def test_a_waking_sender_needs_its_run_number() -> None:
    with pytest.raises(ValueError):
        RunSender(1, wake_set=_Wakes())


def test_a_real_wake_set_returns_the_run_the_answer_woke() -> None:
    core = pytest.importorskip("manyforge_headless_py")
    wake = core.WakeSet()
    sender = RunSender(1, program_run_id=7, wake_set=wake, spawn=lambda _n, work: work())
    sender.submit(lambda: None)
    assert wake.wait(time.monotonic()) == [7]


def test_the_run_keeps_its_latest_two_watches_per_node_and_ends_them_at_close() -> None:
    sender, _spawn, wakes = _waking_sender(capacity=1)
    sender.watch_operation(1)
    sender.watch_planning_job("job")
    sender.watch_operation(1)  # Already watched: no second registration.
    sender.watch_operation(2)  # The third watch of one node: the oldest ends.
    assert wakes.calls == [
        ("watch_operation", 7, 1),
        ("watch_planning_job", 7, "job"),
        ("watch_operation", 7, 2),
        ("unwatch_planning_job", 7, "job"),
    ]
    sender.close()
    sender.watch_operation(3)  # The run ended: nothing is watched any more.
    assert wakes.calls[-1] == ("unwatch_run", 7)


# --- OD-380 D2: the motion and gripper starts leave the tick ----------------


class _MotionPort:
    """A kernel-operations port: each start is one accepted operation."""

    def __init__(self, *, gate: Event | None = None, log: list | None = None) -> None:
        self.gate = gate
        self.entered = Event()
        self.log = [] if log is None else log
        self.next_operation = 40
        self.observed: dict[int, OwnerTaskSnapshot] = {}
        self.cancelled: list[int] = []
        self.acknowledged: list[int] = []

    def start(self, node_id, identity, inputs):
        self.log.append(("start", identity.node_instance_id))
        self.entered.set()
        if self.gate is not None:
            assert self.gate.wait(5)
        self.next_operation += 1
        operation_id = self.next_operation
        return OwnerTaskSnapshot(operation_id, OwnerTaskState.ACTIVE,
                                 metadata={"operation_id": operation_id})

    def observe(self, task_id):
        return self.observed.get(task_id, OwnerTaskSnapshot(
            task_id, OwnerTaskState.ACTIVE, metadata={"operation_id": task_id}))

    def finish(self, task_id, state=OwnerTaskState.SUCCEEDED):
        self.observed[task_id] = OwnerTaskSnapshot(
            task_id, state, metadata={"operation_id": task_id}, settlement_proven=True,
            reason_code=None if state is OwnerTaskState.SUCCEEDED else "operation_stopped")

    def cancel(self, task_id, identity):
        self.log.append(("cancel", task_id))
        self.cancelled.append(task_id)

    def acknowledge(self, task_id, identity):
        self.acknowledged.append(task_id)

    def close(self, task_id, identity, *, request_key):
        return True


class _Values:
    def reset_node_outcome(self, node_instance_id, *, metadata=None):
        pass

    def write_node_outcome(self, node_instance_id, outcome, *, metadata=None):
        pass


class _Counter:
    def __init__(self) -> None:
        self.count = 0

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        self.count += 1
        return NodeAttemptIdentity(1, node_instance_id, self.count)


def _motion(port, sender, name: str = "move"):
    from manyforge_behavior.nodes.v2_owner import MotionTaskAdapter

    adapter = MotionTaskAdapter(
        name=name, node_id="motion.move_to_pose", node_instance_id=name,
        port=port, owner_port_name="kernel_operations",
        parameters={}, value_sources={},
        attempt_source=_Counter(),
        input_resolver=SimpleNamespace(
            value_store=_Values(),
            snapshot=lambda **kwargs: SimpleNamespace(
                parameters=kwargs["parameters"], blackboard={})),
        program_clock=ProgramClock(lambda: 0.0),
        cancellable=True, run_sender=sender,
    )
    adapter.setup_with_descendants()
    return adapter


def test_motion_start_leaves_the_tick_and_its_answer_watches_the_operation() -> None:
    sender, spawn, wakes = _waking_sender()
    port = _MotionPort()
    node = _motion(port, sender)
    node.tick_once()
    assert node.status is RUNNING and port.log == []  # Nothing inside the tick.
    spawn.run()
    # The operation's watch is registered before the answer wakes the run.
    assert wakes.calls == [("watch_operation", 7, 41), ("wake", 7)]
    node.tick_once()  # The answer: the operation is observed from now on.
    assert node.status is RUNNING and node.has_active_task
    port.finish(41)
    node.tick_once()
    # Its acknowledgement leaves the tick too (OD-380 D2).
    assert node.status is RUNNING and port.acknowledged == []
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS and port.acknowledged == [41]
    assert port.log == [("start", "move")]


def test_parallel_motion_starts_reach_the_host_in_tick_order_one_at_a_time() -> None:
    sender, spawn = _sender()
    gate = Event()
    log: list = []
    port = _MotionPort(gate=gate, log=log)
    first, second = _motion(port, sender, "a"), _motion(port, sender, "b")
    root = py_trees.composites.Parallel(
        "both", policy=py_trees.common.ParallelPolicy.SuccessOnAll(), children=[first, second])
    root.setup_with_descendants()
    root.tick_once()
    assert first.status is RUNNING and second.status is RUNNING and log == []
    spawn.start()
    assert port.entered.wait(5)
    assert log == [("start", "a")]  # b waits behind a's answer.
    root.tick_once()  # Neither answer yet: the tick does not wait.
    assert first.status is RUNNING and second.status is RUNNING
    gate.set()
    spawn.threads[0].join(5)
    assert log == [("start", "a"), ("start", "b")]
    root.tick_once()
    port.finish(41)
    port.finish(42)
    root.tick_once()  # Both acknowledgements are sent.
    spawn.run()
    root.tick_once()
    assert root.status is SUCCESS and sorted(port.acknowledged) == [41, 42]


def test_a_branch_stopped_before_its_motion_start_was_sent_sends_nothing() -> None:
    sender, spawn = _sender()
    port = _MotionPort()
    node = _motion(port, sender)
    node.tick_once()
    node.stop(INVALID)
    spawn.run()
    assert port.log == [] and node.outcome == "interrupted"
    assert not node.has_pending_drain


def test_a_branch_stopped_after_its_motion_start_was_sent_cancels_and_drains_it() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _MotionPort(gate=gate)
    node = _motion(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    # Its outcome waits for the start's answer (base read it in the tick).
    assert node.outcome is None
    assert node.has_pending_drain and node.cached_control_state() == "active"
    assert node.drain_invalidated() is False  # Its answer is not there yet.
    gate.set()
    spawn.threads[0].join(5)
    # The answer: the accepted operation is cancelled, then drained from the
    # next tick on, as if the stop had come after a start inside the tick.
    assert node.drain_invalidated() is False
    assert port.cancelled == [41] and node.outcome == "interrupted"
    port.finish(41, OwnerTaskState.FAILED)
    assert node.drain_invalidated() is False  # Its acknowledgement is sent.
    assert port.acknowledged == []
    spawn.run()
    assert node.drain_invalidated() is True
    assert port.acknowledged == [41] and not node.has_pending_drain


def test_a_new_attempt_waits_for_the_stopped_attempts_sent_start() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _MotionPort(gate=gate)
    node = _motion(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    node.tick_once()  # Re-entered: one operation per node, so it waits.
    assert node.status is RUNNING and port.log == [("start", "move")]
    gate.set()
    spawn.threads[0].join(5)
    port.gate = None
    node.tick_once()  # The old start answered: cancelled, drained next.
    assert port.cancelled == [41]
    port.finish(41, OwnerTaskState.FAILED)
    node.tick_once()  # Its acknowledgement is sent.
    spawn.run()
    node.tick_once()  # Drained: the new attempt sends its own start.
    spawn.run()
    assert port.log == [("start", "move"), ("cancel", 41), ("start", "move")]


def test_forced_end_after_a_motion_start_was_sent_cancels_what_it_started() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _MotionPort(gate=gate)
    node = _motion(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    assert node.settle_abandoned_request() is False
    gate.set()
    _eventually(node.settle_abandoned_request)
    assert port.cancelled == [41]
    assert node.status is INVALID and node.outcome == "interrupted"


def test_halt_before_a_motion_start_was_sent_withdraws_it_execute_sends_it() -> None:
    sender, spawn = _sender()
    port = _MotionPort()
    node = _motion(port, sender)
    node.tick_once()
    sender.withdraw_queued()  # The run entering Halting.
    assert node.reconcile_halt_task() == "inactive"
    spawn.run()
    assert port.log == []
    node.tick_once()  # Execute: the same attempt sends it.
    spawn.run()
    node.tick_once()
    assert node.has_active_task and port.log == [("start", "move")]


def test_halt_after_a_motion_start_was_sent_observes_the_accepted_operation() -> None:
    sender, spawn = _sender()
    gate = Event()
    port = _MotionPort(gate=gate)
    node = _motion(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    assert node.reconcile_halt_task() == "active"  # The run stays Halting.
    gate.set()
    spawn.threads[0].join(5)
    # Answered: the accepted operation is the node's task, observed by the
    # Halt like one started inside the tick (still active: still Halting).
    assert node.reconcile_halt_task() == "active"
    assert node._task is not None and node._task.task_id == 41
    port.finish(41)
    assert node.reconcile_halt_task() == "active"  # Its acknowledgement is sent.
    spawn.run()
    node.reconcile_halt_task()
    assert node.status is SUCCESS and port.acknowledged == [41]


class _GripperOperations(_Operations):
    """A gripper port whose plan says whether the target is pneumatic."""

    def __init__(self, log: list, *, pneumatic: bool) -> None:
        super().__init__()
        self.log = log
        self.pneumatic = pneumatic

    def pneumatic_plan(self, identity, inputs):
        self.log.append("plan")
        return super().pneumatic_plan(identity, inputs) if self.pneumatic else None

    def start(self, node_id, identity, inputs):
        self.log.append("start")
        return super().start(node_id, identity, inputs)


def _sent_gripper(valves, sender, operations) -> GripperCommandAdapter:
    adapter = GripperCommandAdapter(
        name="close", node_id="gripper.command_position", node_instance_id="close",
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        parameters={"dwell_while_held": "pause"}, value_sources={},
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={})),
        port=operations, owner_port_name="kernel_operations",
        program_clock=ProgramClock(lambda: 0.0),
        io_watches=SimpleNamespace(), io_signals=valves,
        cancellable=True, valve_sender=sender, run_sender=sender,
    )
    adapter.setup_with_descendants()
    return adapter


def test_gripper_route_read_and_start_are_one_request_off_the_tick() -> None:
    sender, spawn = _sender()
    log: list = []
    node = _sent_gripper(_Valves(), sender, _GripperOperations(log, pneumatic=False))
    node.tick_once()
    assert node.status is RUNNING and log == []
    spawn.run()
    assert log == ["plan", "start"]
    node.tick_once()  # The answer; its acknowledgement is sent.
    spawn.run()
    node.tick_once()
    assert node.status is SUCCESS


def test_pneumatic_gripper_route_then_valves_then_command_all_off_the_tick() -> None:
    sender, spawn = _sender()
    log: list = []
    valves = _Valves()
    operations = _GripperOperations(log, pneumatic=True)
    node = _sent_gripper(valves, sender, operations)
    node.tick_once()
    spawn.run()
    assert log == ["plan"] and valves.writes == []
    for _ in range(8):
        node.tick_once()
        assert log.count("start") <= 1
        spawn.run()
        if node.status is SUCCESS:
            break
    assert node.status is SUCCESS
    assert log == ["plan", "start"] and valves.writes == ["valve-a", "valve-b"]


def test_a_request_submitted_inside_a_sent_request_is_made_in_place() -> None:
    """A command that left the tick reads its tree (OD-380 D2): a request
    its tree submits on the sender's own thread is answered inside it."""

    sender = RunSender(4)
    inside: list[object] = []

    def command() -> str:
        read = sender.submit(lambda: "observed")
        inside.extend([read.answered, read.result()])
        return "done"

    request = sender.submit(command, withdrawable=False)
    _eventually(lambda: request.answered)
    assert request.result() == "done"
    assert inside == [True, "observed"]


def test_a_request_submitted_elsewhere_while_one_is_sent_still_queues() -> None:
    sender = RunSender(4)
    entered, opened = Event(), Event()
    order: list[str] = []

    def first() -> None:
        entered.set()
        assert opened.wait(2.0)
        order.append("first")

    sent = sender.submit(first)
    assert entered.wait(2.0)
    later = sender.submit(lambda: order.append("later"))
    assert later.state is RunRequestState.QUEUED
    opened.set()
    _eventually(lambda: later.answered)
    assert sent.answered and order == ["first", "later"]


# --- A Stop between a request and the tick reading its answer (base: one) ---


class _Outcomes(_Values):
    """The node's outcome key: what each attempt wrote."""

    def __init__(self) -> None:
        self.written: list[str] = []

    def write_node_outcome(self, node_instance_id, outcome, *, metadata=None):
        self.written.append(outcome)


class _SettlingPort(_MotionPort):
    """A motion port whose failed operation is close-eligible, whose close
    is logged, and whose start can be refused."""

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self.closed: list[int] = []
        self.refusal: Exception | None = None

    def start(self, node_id, identity, inputs):
        if self.refusal is not None:
            self.log.append(("start", identity.node_instance_id))
            self.entered.set()
            if self.gate is not None:
                assert self.gate.wait(5)
            raise self.refusal
        return super().start(node_id, identity, inputs)

    def fail_closable(self, task_id: int) -> None:
        self.observed[task_id] = OwnerTaskSnapshot(
            task_id, OwnerTaskState.FAILED, reason_code="planning_failed",
            message="no plan", metadata={
                "operation_id": task_id,
                "recovery_assessment": {
                    "execution_state": "Failed", "status": "action_required",
                    "eligible_close": True,
                },
            })

    def close(self, task_id, identity, *, request_key):
        self.closed.append(task_id)
        return True


def _settling_motion(port, sender):
    node = _motion(port, sender)
    outcomes = _Outcomes()
    node._input_resolver.value_store = outcomes
    return node, outcomes


def _accepted(node, port, spawn) -> None:
    node.tick_once()
    spawn.run()  # The start.
    node.tick_once()  # Its answer: the operation is observed.
    assert node.status is RUNNING and node.has_active_task


def test_a_stop_while_a_completed_motions_acknowledgement_is_sent_ends_it_with_success() -> None:
    sender, spawn = _sender()
    port = _SettlingPort()
    node, outcomes = _settling_motion(port, sender)
    _accepted(node, port, spawn)
    port.finish(41)
    node.tick_once()  # Completed: its acknowledgement is on the sender.
    assert node.status is RUNNING and port.acknowledged == []
    node.stop(INVALID)  # The Program Stop (the tree is invalidated).
    sender.close()
    assert node.outcome is None and port.cancelled == []
    assert node.settle_abandoned_request() is False  # Not answered yet.
    spawn.run()  # Sent also after the close: never withdrawn.
    assert node.settle_abandoned_request() is True
    # As base, whose tick acknowledged it: success, its outcome key written.
    assert node.status is SUCCESS and node.outcome == "success"
    assert port.acknowledged == [41] and outcomes.written == ["completed"]
    assert port.cancelled == [] and not node.has_pending_drain


def test_a_stop_while_a_failed_motions_close_is_sent_ends_it_with_its_failure() -> None:
    sender, spawn = _sender()
    port = _SettlingPort()
    node, outcomes = _settling_motion(port, sender)
    _accepted(node, port, spawn)
    port.fail_closable(41)
    node.tick_once()  # Failed, close-eligible: its close is on the sender.
    assert node.status is RUNNING and port.closed == []
    node.stop(INVALID)
    sender.close()
    assert node.settle_abandoned_request() is False
    spawn.run()
    assert node.settle_abandoned_request() is True
    assert node.status is FAILURE and node.outcome == "failure"
    assert node.failure_reason_code == "planning_failed"
    assert port.closed == [41] and outcomes.written == ["failed"]


def test_a_branch_stopped_while_an_acknowledgement_is_sent_ends_on_the_runs_tick() -> None:
    sender, spawn = _sender()
    port = _SettlingPort()
    node, outcomes = _settling_motion(port, sender)
    _accepted(node, port, spawn)
    port.finish(41)
    node.tick_once()
    node.stop(INVALID)  # Its parent ended it; the run goes on.
    assert node.has_pending_drain and node.drain_invalidated() is False
    spawn.run()
    assert node.drain_invalidated() is True
    assert node.status is SUCCESS and outcomes.written == ["completed"]


def test_a_stop_while_an_undecided_failures_read_is_sent_then_interrupts() -> None:
    """Base read the assessment inside the tick: still undecided, the node
    was RUNNING when the Stop came, so it is `interrupted` and drained."""

    sender, spawn = _sender()
    port = _SettlingPort()
    node, outcomes = _settling_motion(port, sender)
    _accepted(node, port, spawn)
    port.observed[41] = OwnerTaskSnapshot(
        41, OwnerTaskState.FAILED, reason_code="planning_failed",
        metadata={"operation_id": 41})  # No assessment yet: one read.
    reads: list[int] = []
    observe, pushed = port.observe, [True]

    def needs_read(task_id):
        from manyforge_behavior.orchestration_ports import OwnerTaskReadRequiredError

        if pushed.pop() if pushed else False:
            return observe(task_id)  # The pushed table: it failed.
        raise OwnerTaskReadRequiredError(lambda: reads.append(task_id) or observe(task_id))

    port.observe = needs_read
    node.tick_once()  # Failed: its assessment read is on the sender.
    assert reads == [] and node.status is RUNNING
    node.stop(INVALID)
    sender.close()
    spawn.run()
    assert node.settle_abandoned_request() is True
    assert reads == [41] and node.outcome == "interrupted"
    assert outcomes.written == [] and port.closed == []


@pytest.mark.parametrize("refusal", ["rule_holds", "priority"])
def test_a_stop_after_a_motion_start_was_sent_and_refused_ends_with_the_refusal(refusal) -> None:
    from manyforge_behavior.orchestration_ports import OwnerInvocationRefusedByPriorityError

    sender, spawn = _sender()
    gate = Event()
    port = _SettlingPort(gate=gate)
    port.refusal = (
        KernelOperationAdmissionRejectedError("rule_holds", "a rule holds", {"rule_id": "r1"})
        if refusal == "rule_holds"
        else OwnerInvocationRefusedByPriorityError(
            "refused", {"node_instance_id": "other", "operation_id": "7"})
    )
    node, outcomes = _settling_motion(port, sender)
    node.tick_once()
    spawn.start()
    assert port.entered.wait(5)
    node.stop(INVALID)
    sender.close()
    assert node.settle_abandoned_request() is False and node.outcome is None
    gate.set()
    _eventually(node.settle_abandoned_request)
    # As base, whose tick read the refusal before the Stop came.
    assert node.status is FAILURE and node.outcome == "failure"
    if refusal == "rule_holds":
        assert node.failure_reason_code == "rule_holds"
        assert outcomes.written == ["failed"]
    else:
        assert node.failure_reason_code == "refused_by_priority"
        assert outcomes.written == ["refused_by_priority"]
    assert port.cancelled == [] and not node.has_pending_drain


def test_a_stop_before_a_motion_start_was_sent_withdraws_it_and_interrupts() -> None:
    sender, spawn = _sender()
    port = _SettlingPort()
    node, outcomes = _settling_motion(port, sender)
    node.tick_once()  # The start is queued.
    node.stop(INVALID)
    sender.close()
    spawn.run()
    assert port.log == []  # Never sent (OD-213 part 3).
    assert node.settle_abandoned_request() is True and node.outcome == "interrupted"
