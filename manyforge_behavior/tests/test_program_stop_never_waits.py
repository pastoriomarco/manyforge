"""The Program's own Stop nodes never wait behind the run's other requests.

`kernel.stop_operation` and `kernel.cell_stop` have no run sender: the tick
sends their Stop and reads its status at once on the host's Stop control
socket (OD-119), as before the run sender carried owner calls (base
b71aa446). A request of the run already sent completes and its node returns
its real result (OD-213 (3)). Every host call is recorded in order with the
thread that made it: the tick (the test's thread) or the run's sender.

Base made a motion or gripper start inside the tick, so a Program Stop ticked
after it stopped what it had started. Now such a start still queued when the
Stop is sent is withdrawn and never reaches the host; its node ends as base's
start that the Stop cancelled before dispatch (`operation_cancelled_pre_dispatch`).
One already sent is answered first: the Stop follows its admission, as in
base. The Kernel's Program Stops close no admission (no latch), so this order
is what keeps such a start from running after the Stop. Nothing else of the
run is withdrawn or waited for.
"""

from __future__ import annotations

import threading
from dataclasses import replace
from types import SimpleNamespace
from typing import Any

import py_trees
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.nodes.v2_owner import OwnerTaskAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.production_ports import NativeKernelProgramPort

from .test_executable_node_runtime import _ManualSender, _PlanningPort, _planning_adapter
from .test_owner_calls_leave_the_tick import (
    _control,
    _guarded,
    _invocation,
    _motion_on,
    _owner_task,
    _Sender,
    _StopScope,
    _tick,
    _TickGuard,
    _Values,
)
from .test_production_ports import _CONTROL_STOP, _CONTROLLED, _ControlTransport, _context
from .v3_fixtures import target
from .gateway_fakes import transport_gateway

_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_FAILURE = py_trees.common.Status.FAILURE


class _OrderGuard(_TickGuard):
    """Every guarded call in order, with where it was made; a held call
    waits in its thread until the test releases it."""

    def __init__(self) -> None:
        super().__init__()
        self.log: list[tuple[str, str]] = []
        self.held: dict[str, threading.Event] = {}
        self.entered: dict[str, threading.Event] = {}

    def hold(self, name: str) -> None:
        self.held[name] = threading.Event()
        self.entered[name] = threading.Event()

    def release(self, name: str) -> None:
        self.held[name].set()

    def check(self, name: str) -> None:
        super().check(name)
        if not self.armed:
            return
        where = "tick" if threading.get_ident() == self.tick_thread else "sender"
        self.log.append((name, where))
        gate = self.held.get(name)
        if gate is not None:
            self.entered[name].set()
            assert gate.wait(10)


class _BackgroundSender(_Sender):
    """The run's sender whose thread runs in the background (a request can
    be in flight while the test ticks)."""

    def start(self) -> threading.Thread:
        worker = threading.Thread(target=self.pending.pop(0))
        worker.start()
        return worker


def _native_motion(guard: _TickGuard, transport: _ControlTransport) -> NativeKernelProgramPort:
    """The real native port over ``transport``, every host call guarded; its
    targets may also be stopped (`kernel.stop_operation`)."""

    guard.armed = False  # Binding the run is not a tick.
    port = NativeKernelProgramPort(
        KernelClient(transport_gateway(_guarded(transport, guard))),
        active_tcp_result=lambda *_args: True,
    )
    context = _context()
    binding = {
        **context.program_binding,
        "target_bindings": [
            {
                **row,
                "capability_bindings": [
                    *row["capability_bindings"],
                    {"capability_id": "execution.stop.soft", "command_schema": None},
                ],
            }
            for row in context.program_binding["target_bindings"]
        ],
    }
    port.bind(replace(context, program_binding=binding))
    guard.armed = True
    return port


def _effects(guard: _OrderGuard) -> list[tuple[str, str]]:
    return [
        entry for entry in guard.log
        if entry[0] not in {"identity", "query_operation", "targets", "read", "control_state"}
    ]


def _program_stop(
    port: Any, node_id: str = "kernel.cell_stop", sender: _Sender | None = None
) -> OwnerTaskAdapter:
    """The Program's Stop node as the resolver builds it: its Stop is not
    sent on the run's sender; it withdraws the run's queued starts there."""

    numbers = iter(range(1, 100))
    parameters = (
        {} if node_id == "kernel.cell_stop" else {"target": target("robot.1", "arm.command")}
    )
    node = OwnerTaskAdapter(
        name="stop", node_id=node_id, node_instance_id="stop",
        attempt_source=SimpleNamespace(
            begin=lambda instance: NodeAttemptIdentity(1, instance, next(numbers))),
        parameters=parameters, value_sources={},
        input_resolver=SimpleNamespace(
            value_store=_Values(),
            snapshot=lambda **_: SimpleNamespace(parameters=parameters, blackboard={}),
        ),
        port=port.controls, owner_port_name="kernel_controls",
        program_stop_starts=None if sender is None else sender.sender,
    )
    node.setup_with_descendants()
    return node


_CANCELLED = ("operation_cancelled_pre_dispatch", "native operation was interrupted")


def _stopped_before_sent(node: Any) -> tuple[Any, ...]:
    return (node.status, node.failure_reason_code, node._failure_message, node.outcome)


def _stop_completes(transport: _ControlTransport, stop: OwnerTaskAdapter) -> None:
    transport.stop_states[_CONTROL_STOP] = "succeeded"
    assert _tick(stop) is _SUCCESS


@pytest.mark.parametrize("node_id", ["kernel.stop_operation", "kernel.cell_stop"])
def test_the_resolver_builds_program_stops_without_the_run_sender(node_id: str) -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port = _control(node_id, guard, sender)
    assert _tick(node) is _SUCCESS  # Sent, read and consumed inside the tick.
    assert sender.pending == [] and len(port.starts) == 1
    assert guard.on_tick == [node_id, "acknowledge"]


@pytest.mark.parametrize("node_id", ["kernel.stop_operation", "kernel.cell_stop"])
def test_a_program_stop_is_sent_and_read_inside_the_tick(node_id: str) -> None:
    guard, transport = _OrderGuard(), _ControlTransport()
    port = _native_motion(guard, transport)
    stop = _program_stop(port, node_id)
    assert _tick(stop) is _RUNNING
    sent = "cell_stop" if node_id == "kernel.cell_stop" else "stop"
    assert _effects(guard) == [(sent, "tick")]
    assert _tick(stop) is _RUNNING  # Pending: read again on the next tick.
    _stop_completes(transport, stop)
    assert _effects(guard) == [(sent, "tick"), ("read_stop", "tick"), ("read_stop", "tick")]
    assert all(where == "tick" for _name, where in guard.log)


def test_parallel_move_and_stop_the_stop_is_sent_at_once() -> None:
    # Parallel[move, cell_stop] ticked together. Base: the move's start and
    # then the Stop, both inside the tick. Now the move's start is queued on
    # the run's sender, the Stop withdraws it and is sent at once inside the tick.
    guard, sender, transport = _OrderGuard(), _Sender(), _ControlTransport()
    port = _native_motion(guard, transport)
    move, stop = _motion_on(port, sender), _program_stop(port, sender=sender)
    root = py_trees.composites.Parallel(
        "both", policy=py_trees.common.ParallelPolicy.SuccessOnAll(), children=[move, stop])
    root.setup_with_descendants()
    root.tick_once()
    assert _effects(guard) == [("cell_stop", "tick")]  # The start is withdrawn.
    assert stop.status is _RUNNING and move.status is _RUNNING
    transport.stop_states[_CONTROL_STOP] = "succeeded"
    root.tick_once()
    assert stop.status is _SUCCESS


@pytest.mark.parametrize("node_id", ["kernel.cell_stop", "kernel.stop_operation"])
def test_no_queued_motion_start_reaches_the_host_after_the_program_stop(node_id: str) -> None:
    guard, sender, transport = _OrderGuard(), _Sender(), _ControlTransport()
    port = _native_motion(guard, transport)
    move, stop = _motion_on(port, sender), _program_stop(port, node_id, sender)
    root = py_trees.composites.Parallel(
        "both", policy=py_trees.common.ParallelPolicy.SuccessOnAll(), children=[move, stop])
    root.setup_with_descendants()
    root.tick_once()
    sender.drain()
    root.tick_once()
    sender.drain()
    names = [name for name, _where in _effects(guard)]
    assert "submit" not in names
    # Base: the start the Stop cancelled before dispatch fails to the tree.
    assert _stopped_before_sent(move) == (_FAILURE, *_CANCELLED, "failure")
    assert move._failure_details is None and move._operation_failure is None
    assert transport.operations.keys() == {_CONTROLLED}  # Nothing was admitted.


def test_a_program_stop_on_another_target_leaves_the_queued_start() -> None:
    # `kernel.stop_operation` covers its own target's operation only.
    guard, sender, transport = _OrderGuard(), _Sender(), _ControlTransport()
    move = _motion_on(_native_motion(guard, transport), sender)
    assert _tick(move) is _RUNNING
    other = target("robot.2", "arm.command")
    assert sender.sender.stop_starts(other) is False
    sender.drain()
    assert [name for name, _ in _effects(guard)].count("submit") == 1
    assert _tick(move) is _RUNNING and move.failure_reason_code is None


@pytest.mark.parametrize("held", [True, False])
def test_a_withdrawn_start_holds_or_fails_as_base_on_the_stop_scope(held: bool) -> None:
    # Base (`_hold_stopped_unstarted`): a start a Stop ended with nothing
    # started holds when a Stop holds the node (fresh scope read on the
    # run's sender), else its failure returns to the tree.
    guard, sender, transport = _OrderGuard(), _Sender(), _ControlTransport()
    port = _native_motion(guard, transport)
    scope = _StopScope(guard, frozenset({"move"} if held else ()))
    move = _motion_on(port, sender, operator_stop_state=scope)
    stop = _program_stop(port, sender=sender)
    assert _tick(move) is _RUNNING
    assert _tick(stop) is _RUNNING  # Withdraws the queued start.
    assert _tick(move) is _RUNNING and scope.refreshes == 0  # Its scope read is sent.
    sender.drain()
    assert scope.refreshes == 1
    if held:
        assert _tick(move) is _RUNNING and move.presented_held
        assert move._held_before_admission == 1
        assert move.failure_reason_code is None
    else:
        assert _stopped_before_sent(move) == (_RUNNING, None, "", None)
        assert _tick(move) is _FAILURE
        assert _stopped_before_sent(move) == (_FAILURE, *_CANCELLED, "failure")
    assert "submit" not in [name for name, _ in _effects(guard)]
    assert all(
        where == "sender" for name, where in guard.log if name == "read_program_execution_scope"
    )


def test_a_program_stop_follows_a_motion_start_in_flight() -> None:
    # Base: the start was admitted inside the tick before the Stop was sent.
    # Now the Stop waits only for that sent start's admission answer.
    guard, sender, transport = _OrderGuard(), _BackgroundSender(), _ControlTransport()
    port = _native_motion(guard, transport)
    move, stop = _motion_on(port, sender), _program_stop(port, sender=sender)
    guard.hold("submit")
    assert _tick(move) is _RUNNING
    worker = sender.start()
    assert guard.entered["submit"].wait(10)  # In flight on the run's sender.
    assert _tick(stop) is _RUNNING
    assert _tick(stop) is _RUNNING  # Still awaiting the admission's answer.
    assert _effects(guard) == [("submit", "sender")]
    guard.release("submit")
    worker.join(10)
    assert _tick(stop) is _RUNNING
    assert _effects(guard) == [("submit", "sender"), ("cell_stop", "tick")]
    _stop_completes(transport, stop)
    # The admitted move observes its operation as a started one does.
    assert _tick(move) is _RUNNING and move._task is not None


class _BlockingInvoke:
    def __init__(self, guard: _OrderGuard, name: str) -> None:
        self.guard, self.name = guard, name

    def invoke(self, node_id, identity, inputs):
        self.guard.check(self.name)
        return OwnerInvocationResult(True)


class _BlockingTask:
    def __init__(self, guard: _OrderGuard, name: str) -> None:
        self.guard, self.name = guard, name

    def start(self, node_id, identity, inputs):
        self.guard.check(self.name)
        return OwnerTaskSnapshot("tcp", OwnerTaskState.SUCCEEDED)

    def observe(self, task_id):
        return OwnerTaskSnapshot(task_id, OwnerTaskState.SUCCEEDED)

    def acknowledge(self, task_id, identity):
        self.guard.check("acknowledge")

    def cancel(self, task_id, identity):
        raise AssertionError("an atomic step is never cancelled")


def _in_flight_node(kind: str, guard: _OrderGuard, sender: _Sender):
    if kind == "set_tcp":
        return _owner_task(
            OwnerTaskAdapter, _BlockingTask(guard, kind), sender, kind, "motion.set_tcp",
            {"target": target("robot.1", "arm.command")}, owner_port_name="kernel_operations",
        )
    node_id = "io.set_output" if kind == "io_write" else "scene.attach_object"
    return _invocation(_BlockingInvoke(guard, kind), sender, node_id, {})


@pytest.mark.parametrize("kind", ["scene_commit", "io_write", "set_tcp"])
def test_a_program_stop_does_not_wait_for_a_sent_atomic_request(kind: str) -> None:
    # Base: the same (these three already left the tick; a Program Stop
    # never waited for them). The sent request completes with its real result.
    guard, sender, transport = _OrderGuard(), _BackgroundSender(), _ControlTransport()
    port = _native_motion(guard, transport)
    node, stop = _in_flight_node(kind, guard, sender), _program_stop(port, sender=sender)
    guard.hold(kind)
    assert _tick(node) is _RUNNING
    worker = sender.start()
    assert guard.entered[kind].wait(10)  # In flight on the run's sender.
    assert _tick(stop) is _RUNNING  # Sent at once, not behind the request.
    assert _effects(guard) == [(kind, "sender"), ("cell_stop", "tick")]
    _stop_completes(transport, stop)  # Also while the request is in flight.
    guard.release(kind)
    worker.join(10)
    for _ in range(3):  # Its answer (and Set TCP's acknowledgement).
        if _tick(node) is not _RUNNING:
            break
        sender.drain()
    assert node.status is _SUCCESS and node.failure_reason_code is None


def test_a_program_stop_while_a_motion_start_is_queued() -> None:
    guard, sender, transport = _OrderGuard(), _BackgroundSender(), _ControlTransport()
    port = _native_motion(guard, transport)
    write = _in_flight_node("io_write", guard, sender)
    move, stop = _motion_on(port, sender), _program_stop(port, sender=sender)
    guard.hold("io_write")
    _tick(write)
    worker = sender.start()
    assert guard.entered["io_write"].wait(10)
    assert _tick(move) is _RUNNING  # Its start is queued behind the write.
    assert _tick(stop) is _RUNNING  # Sent at once, while the write is in flight.
    assert _effects(guard)[:2] == [("io_write", "sender"), ("cell_stop", "tick")]
    guard.release("io_write")
    worker.join(10)
    names = [name for name, _where in _effects(guard)]
    assert "submit" not in names
    assert _tick(write) is _SUCCESS  # The sent write's real result.
    assert _tick(move) is _FAILURE
    assert _stopped_before_sent(move) == (_FAILURE, *_CANCELLED, "failure")


def test_a_program_stop_while_a_motion_start_is_queued_is_not_delayed() -> None:
    guard, sender, transport = _OrderGuard(), _BackgroundSender(), _ControlTransport()
    port = _native_motion(guard, transport)
    write = _in_flight_node("io_write", guard, sender)
    move, stop = _motion_on(port, sender), _program_stop(port, sender=sender)
    guard.hold("io_write")
    _tick(write)
    worker = sender.start()
    assert guard.entered["io_write"].wait(10)
    assert _tick(move) is _RUNNING  # Its start is queued behind the write.
    assert _tick(stop) is _RUNNING
    assert _effects(guard) == [("io_write", "sender"), ("cell_stop", "tick")]
    _stop_completes(transport, stop)
    guard.release("io_write")
    worker.join(10)
    assert _tick(write) is _SUCCESS  # The sent write's real result.


def test_a_program_stop_during_planning_leaves_the_planning_job() -> None:
    # Base: the same (the planning job already left the tick, and a Program
    # Stop never cancelled it): the Stop is sent at once, the job plans on and
    # the node ends with the job's real result (here the fake's failure).
    guard, transport = _OrderGuard(), _ControlTransport()
    stop = _program_stop(_native_motion(guard, transport))
    task, sender = _PlanningPort(), _ManualSender()
    planning = _planning_adapter(task, sender)
    assert _tick(planning) is _RUNNING
    sender.drain()
    task.states["job-1"] = "planning"
    assert _tick(planning) is _RUNNING
    assert _tick(stop) is _RUNNING
    assert _effects(guard) == [("cell_stop", "tick")]
    _stop_completes(transport, stop)
    assert task.planning_cancels == []
    task.states["job-1"] = "done"
    for _ in range(6):
        if _tick(planning) is not _RUNNING:
            break
        sender.drain()
    assert planning.status is py_trees.common.Status.FAILURE
    assert planning.failure_reason_code == "no_feasible_candidate"
    assert task.finishes == ["job-1"]


def test_a_halt_leaves_a_sent_program_stop_and_execute_reads_it() -> None:
    # Base: the Stop node has nothing on the run's sender; a Halt does not
    # touch it and it reads its status again after Execute.
    guard, sender, transport = _OrderGuard(), _Sender(), _ControlTransport()
    stop = _program_stop(_native_motion(guard, transport))
    _tick(stop)
    sender.sender.withdraw_queued()  # The Halt.
    assert stop.reconcile_halt_task() == "unaffected"
    _stop_completes(transport, stop)
    assert [name for name, _ in _effects(guard)] == ["cell_stop", "read_stop"]


def test_a_forced_end_after_a_program_stop_was_sent_ends_its_observation() -> None:
    guard, transport = _OrderGuard(), _ControlTransport()
    port = _native_motion(guard, transport)
    stop = _program_stop(port)
    _tick(stop)  # Sent and accepted inside the tick.
    stop.stop(py_trees.common.Status.INVALID)  # Forced Program End.
    assert stop.settle_abandoned_request() is True
    # The Kernel keeps the accepted Stop; the attempt's observation ends.
    assert port.controls._pending == {}
    assert stop.status is py_trees.common.Status.INVALID and stop.outcome is None
    assert [kind for kind, *_ in transport.controls] == ["cell_stop"]
