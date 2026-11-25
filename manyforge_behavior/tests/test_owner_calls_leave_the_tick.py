"""Every owner call of the quick-read, watch, rule, Mock and control nodes
leaves the tick on the run's sender (OD-380 D2, OD-213).

The run's sender here is emptied only when a test says so, each request on a
thread of its own; the fakes fail any call made from the tick thread. A Halt
withdraws a request not sent yet; a sent one completes and the node returns
its real result.
"""

from __future__ import annotations

import threading
from types import SimpleNamespace
from typing import Any, Callable

import py_trees
import pytest

from manyforge_behavior.executable_node_resolver import (
    NodeAdapterBuildContext,
    NodeAdapterPorts,
    build_executable_node_resolver,
)
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.nodes.v2_owner import (
    GripperStatusWaitAdapter,
    IoInputWaitAdapter,
    MotionTaskAdapter,
    OwnerInvocationAdapter,
    OwnerTaskAdapter,
)
from manyforge_behavior.nodes.v2_pure import GeometryCheckPoseBounds
from manyforge_behavior.orchestration_ports import (
    FrameTransformResult,
    MockSimulationRefusedError,
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.orchestration_values import NodeAttemptInputResolver, ProgramValueStore
from manyforge_behavior.production_ports import NativeKernelProgramPort
from manyforge_behavior.program import Program
from manyforge_behavior.program_clock import ProgramClock
from manyforge_behavior.program_runtime import ProgramRunAttemptSource
from manyforge_behavior.run_sender import RunSender

from .test_executable_node_runtime import _TaskPort, _catalog, _identified, _minimal_document
from .test_io_signal_nodes import _OwnerError, _Transport, _WatchPort, _context, _signal_row
from .test_production_ports import _motion_inputs
from .test_production_ports import _Transport as _ProductionTransport
from .test_production_ports import _context as _production_context
from .v3_fixtures import signal, target
from .gateway_fakes import transport_gateway

_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_FAILURE = py_trees.common.Status.FAILURE


class _Sender:
    """The run's sender, emptied on a thread of its own when the test says."""

    def __init__(self, capacity: int = 8) -> None:
        self.pending: list[Callable[[], None]] = []
        self.sender = RunSender(capacity, spawn=lambda _name, work: self.pending.append(work))

    def drain(self) -> None:
        while self.pending:
            worker = threading.Thread(target=self.pending.pop(0))
            worker.start()
            worker.join()


class _TickGuard:
    """Records any owner call made on the tick thread (the test's thread)."""

    def __init__(self) -> None:
        self.tick_thread = threading.get_ident()
        self.armed = True
        self.on_tick: list[str] = []
        self.calls: list[str] = []

    def check(self, name: str) -> None:
        if not self.armed:
            return
        self.calls.append(name)
        if threading.get_ident() == self.tick_thread:
            self.on_tick.append(name)


def _guarded(inner: Any, guard: _TickGuard) -> Any:
    """``inner`` whose every method call goes through ``guard``."""

    class Guarded:
        def __getattr__(self, name: str) -> Any:
            value = getattr(inner, name)
            if not callable(value):
                return value

            def call(*args: Any, **kwargs: Any) -> Any:
                guard.check(name)
                return value(*args, **kwargs)

            return call

    return Guarded()


def _tick(node: py_trees.behaviour.Behaviour) -> py_trees.common.Status:
    node.tick_once()
    return node.status


def _attempts(instance: str):
    numbers = iter(range(1, 100))
    return SimpleNamespace(begin=lambda _: NodeAttemptIdentity(1, instance, next(numbers)))


def _resolver(parameters: dict[str, Any], writes: list[tuple[str, Any]] | None = None):
    store = SimpleNamespace(
        write=lambda key, value, metadata: (writes if writes is not None else []).append(
            (key, value)
        )
    )
    return SimpleNamespace(
        snapshot=lambda **_: SimpleNamespace(parameters=parameters, blackboard={}),
        value_store=store,
    )


# --- Quick reads and rule activation over the real native port -------------


class _RuleTransport(_Transport):
    def __init__(self) -> None:
        super().__init__()
        self.rule_calls: list[tuple[str, bool]] = []
        self.rule_error: Exception | None = None
        self.during_rule: Callable[[], None] | None = None

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]:
        self.rule_calls.append((rule_id, active))
        if self.during_rule is not None:
            self.during_rule()
        if self.rule_error is not None:
            raise self.rule_error
        return {"ok": True}


def _native(guard: _TickGuard) -> tuple[_RuleTransport, NativeKernelProgramPort]:
    transport = _RuleTransport()
    guard.armed = False  # Binding the run is not a tick.
    port = NativeKernelProgramPort(KernelClient(transport_gateway(_guarded(transport, guard))))
    port.bind(_context([_signal_row()]))
    guard.armed = True
    return transport, port


def _invocation(port: Any, sender: _Sender, node_id: str, parameters: dict[str, Any],
                *, writes: list | None = None, **kwargs: Any) -> OwnerInvocationAdapter:
    return OwnerInvocationAdapter(
        name="leaf",
        node_id=node_id,
        node_instance_id="leaf",
        attempt_source=_attempts("leaf"),
        parameters=parameters,
        value_sources={},
        input_resolver=_resolver(parameters, writes),
        output_key_parameter="output_key" if "output_key" in parameters else None,
        port=port,
        run_sender=sender.sender,
        **kwargs,
    )


def test_an_input_read_leaves_the_tick_and_its_answer_ends_the_node() -> None:
    guard, sender, writes = _TickGuard(), _Sender(), []
    transport, port = _native(guard)
    node = _invocation(
        port.queries, sender, "io.get_input",
        {"signal": signal("io.1", "di_0"), "output_key": "part"}, writes=writes,
    )
    assert _tick(node) is _RUNNING
    assert transport.queries == []  # Queued on the sender, not made in the tick.
    sender.drain()
    assert len(transport.queries) == 1
    assert _tick(node) is _SUCCESS
    assert writes == [("part", True)]
    assert guard.calls == ["query_target"] and guard.on_tick == []


def test_a_halt_withdraws_a_queued_rule_activation_and_it_is_sent_after_execute() -> None:
    guard, sender = _TickGuard(), _Sender()
    transport, port = _native(guard)
    node = _invocation(port.queries, sender, "rules.set_active",
                       {"rule_id": "guard", "active": True})
    assert _tick(node) is _RUNNING
    sender.sender.withdraw_queued()  # The Halt.
    assert node.reconcile_halt_task() == "unaffected"
    sender.drain()
    assert transport.rule_calls == []  # Never sent.
    assert _tick(node) is _RUNNING  # Execute: the node sends it again.
    sender.drain()
    assert transport.rule_calls == [("guard", True)]
    assert _tick(node) is _SUCCESS
    assert guard.on_tick == []


def test_a_sent_rule_activation_completes_through_a_halt_with_its_real_result() -> None:
    guard, sender = _TickGuard(), _Sender()
    transport, port = _native(guard)
    node = _invocation(port.queries, sender, "rules.set_active",
                       {"rule_id": "unknown", "active": False})
    assert _tick(node) is _RUNNING
    halted: list[str] = []
    # The Halt arrives while the request is with the host.
    transport.during_rule = lambda: halted.append(node.reconcile_halt_task())
    transport.rule_error = _OwnerError("rule_unknown")
    sender.drain()
    assert halted == ["active"]  # The run stays Halting until the answer.
    assert node.reconcile_halt_task() == "unaffected"  # Answered: read after Execute.
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "rule_unknown"
    assert node.outcome != "interrupted"
    assert transport.rule_calls == [("unknown", False)]
    assert guard.on_tick == []


def test_a_sent_read_of_a_stopped_tree_ends_its_attempt_with_the_real_result() -> None:
    guard, sender = _TickGuard(), _Sender()
    transport, port = _native(guard)
    node = _invocation(port.queries, sender, "io.check_input",
                       {"signal": signal("io.1", "di_0"), "expected_value": False})
    assert _tick(node) is _RUNNING
    stopped: list[bool] = []

    def stop_during_read() -> None:
        node.stop(py_trees.common.Status.INVALID)  # Program Stop: Forced End.
        stopped.append(node.settle_abandoned_request())

    original = transport.query_target

    def query_target(query):
        stop_during_read()
        return original(query)

    transport.query_target = query_target
    sender.drain()
    assert stopped == [False]  # Sent: the Forced End waits for its answer.
    assert node.settle_abandoned_request() is True
    assert node.status is _FAILURE  # The input is True: the real result.
    assert len(transport.queries) == 1 and guard.on_tick == []


# --- Gripper status wait ----------------------------------------------------


class _GripperPort:
    def __init__(self, guard: _TickGuard) -> None:
        self.guard = guard
        self.state = "executing"
        self.reads = 0
        self.during_read: Callable[[], None] | None = None

    def invoke(self, node_id, identity, inputs) -> OwnerInvocationResult:
        self.guard.check("query_target")
        self.reads += 1
        if self.during_read is not None:
            self.during_read()
        matched = self.state == "completed"
        return OwnerInvocationResult(
            matched,
            metadata={"command_scope": ("c", 1, "s", 1, "1", "cfg", 1),
                      "command_state": self.state},
            reason_code=None if matched else "gripper_status_not_matched",
        )


def _gripper_wait(guard: _TickGuard, sender: _Sender, timeout_s: float = 5.0):
    clock = [10.0]
    parameters = {
        "target": target("gripper.1", "gripper"),
        "expected_state": "reached",
        "timeout_s": timeout_s,
        "timeout_while_held": "pause",
    }
    port = _GripperPort(guard)
    node = GripperStatusWaitAdapter(
        name="wait",
        node_id="gripper.wait_for_status",
        node_instance_id="wait",
        attempt_source=_attempts("wait"),
        parameters=parameters,
        value_sources={},
        input_resolver=_resolver(parameters),
        port=port,
        program_clock=ProgramClock(lambda: clock[0]),
        run_sender=sender.sender,
    )
    return node, port, clock


def test_gripper_status_reads_leave_the_tick_at_their_cadence() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, clock = _gripper_wait(guard, sender)
    assert _tick(node) is _RUNNING and port.reads == 0
    sender.drain()
    assert _tick(node) is _RUNNING and port.reads == 1  # Not matched yet.
    assert _tick(node) is _RUNNING and not sender.pending  # Next read in 0.1 s.
    clock[0] += 0.1
    port.state = "completed"
    assert _tick(node) is _RUNNING
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert port.reads == 2 and guard.on_tick == []


def test_a_gripper_read_queued_at_the_deadline_is_withdrawn_and_the_wait_fails() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, clock = _gripper_wait(guard, sender, timeout_s=1.0)
    assert _tick(node) is _RUNNING
    clock[0] += 1.0
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "gripper_wait_timeout"
    sender.drain()
    assert port.reads == 0


def test_a_sent_gripper_read_answered_after_the_deadline_fails_as_late() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, clock = _gripper_wait(guard, sender, timeout_s=1.0)
    port.state = "completed"
    assert _tick(node) is _RUNNING

    def slow() -> None:
        clock[0] += 1.5
    port.during_read = slow
    sender.drain()
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "gripper_wait_timeout"
    assert "after the deadline" in node.failure_message


# --- I/O watch wait ---------------------------------------------------------


class _GuardedWatchPort(_WatchPort):
    def __init__(self, guard: _TickGuard) -> None:
        super().__init__()
        self.guard = guard

    def start(self, node_id, identity, inputs):
        self.guard.check("register_io_watch")
        return super().start(node_id, identity, inputs)

    def observe(self, task_id):
        self.guard.check("read_io_watch")
        return super().observe(task_id)

    def cancel(self, task_id, identity):
        self.guard.check("release_io_watch")
        super().cancel(task_id, identity)

    def acknowledge(self, task_id, identity):
        self.guard.check("release_io_watch")
        super().acknowledge(task_id, identity)


def _io_wait(guard: _TickGuard, sender: _Sender, timeout_s: float | None = None):
    clock = [100.0]
    parameters: dict[str, Any] = {
        "signal": signal("io.1", "di_0"), "expected_value": True, "edge": "level",
    }
    if timeout_s is not None:
        parameters["timeout_s"] = timeout_s
    port = _GuardedWatchPort(guard)
    node = IoInputWaitAdapter(
        name="wait",
        node_id="io.wait_for_input",
        node_instance_id="wait",
        attempt_source=_attempts("wait"),
        parameters=parameters,
        value_sources={},
        input_resolver=_resolver(parameters),
        port=port,
        program_clock=ProgramClock(lambda: clock[0]),
        run_sender=sender.sender,
        cancellable=True,
    )
    return node, port, clock


def test_watch_registration_reads_and_release_leave_the_tick() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, _clock = _io_wait(guard, sender)
    assert _tick(node) is _RUNNING and port.calls == []
    sender.drain()
    assert _tick(node) is _RUNNING  # Registered: pending.
    assert _tick(node) is _RUNNING  # A read is sent; the next tick takes it.
    sender.drain()
    assert _tick(node) is _RUNNING
    port.state = OwnerTaskState.SUCCEEDED
    assert _tick(node) is _RUNNING
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert port.releases() == []  # The release is sent, not awaited.
    sender.drain()
    assert port.releases() == [("acknowledge", "watch.1")]
    assert [call[0] for call in port.calls] == [
        "start", "observe", "observe", "acknowledge",
    ]
    assert guard.on_tick == []


def test_a_halt_withdraws_a_queued_registration_and_execute_sends_it_again() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, _clock = _io_wait(guard, sender)
    assert _tick(node) is _RUNNING
    sender.sender.withdraw_queued()
    sender.drain()
    assert port.calls == []
    assert _tick(node) is _RUNNING
    sender.drain()
    assert port.calls == [("start", 1)]


def test_the_next_attempt_registers_only_after_the_last_release_is_sent() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, _clock = _io_wait(guard, sender)
    _tick(node)
    sender.drain()
    _tick(node)  # Registered.
    node.stop(py_trees.common.Status.INVALID)  # Released (queued).
    assert node.presented_interrupted is True
    assert _tick(node) is _RUNNING  # The new attempt waits for the release.
    assert len(sender.pending) == 1
    sender.drain()
    assert port.releases() == [("cancel", "watch.1")]
    assert _tick(node) is _RUNNING
    sender.drain()
    assert ("start", 2) in port.calls and guard.on_tick == []


def test_a_queued_watch_read_at_the_timeout_is_withdrawn_and_the_watch_released() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, clock = _io_wait(guard, sender, timeout_s=1.0)
    _tick(node)
    sender.drain()
    _tick(node)  # Registered.
    _tick(node)  # Read queued.
    clock[0] += 1.0
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "io_wait_timeout"
    sender.drain()
    assert [call[0] for call in port.calls] == ["start", "cancel"]


def test_a_sent_registration_answered_after_the_timeout_fails_and_releases() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, clock = _io_wait(guard, sender, timeout_s=1.0)
    port.state = OwnerTaskState.SUCCEEDED

    def slow() -> None:
        clock[0] += 1.5
    port.during_call = slow
    assert _tick(node) is _RUNNING
    sender.drain()
    assert _tick(node) is _FAILURE
    assert node.failure_message == "The input read completed after timeout_s."
    sender.drain()
    assert port.releases() == [("acknowledge", "watch.1")]


# --- Mock simulation START ---------------------------------------------------


class _MockPort:
    def __init__(self, guard: _TickGuard) -> None:
        self.guard = guard
        self.starts: list[dict[str, Any]] = []
        self.ends: list[dict[str, Any]] = []
        self.outcomes: list[Any] = []

    def request(self, identity, index, rows):
        self.guard.check("read_program_execution_scope")  # The claim generation.
        return {"node_start_index": str(index), "node_attempt_id": identity.node_attempt_id}

    def start(self, request):
        self.guard.check("start_mock_io_node_simulation")
        self.starts.append(dict(request))
        outcome = self.outcomes.pop(0) if self.outcomes else None
        if isinstance(outcome, Exception):
            raise outcome
        return {"signals": [], "started_at_ns": 1}

    def end(self, request):
        self.ends.append(dict(request))


def test_a_mock_start_leaves_the_tick_before_the_read_and_an_unknown_reply_is_resent() -> None:
    guard, sender = _TickGuard(), _Sender()
    reads: list[str] = []

    class _Reads:
        def invoke(self, node_id, identity, inputs):
            guard.check("query_target")
            reads.append(node_id)
            return OwnerInvocationResult(True)

    mock = _MockPort(guard)
    mock.outcomes = [RuntimeError("reply lost")]
    node = _invocation(
        _Reads(), sender, "io.check_input",
        {"signal": signal("io.1", "di_0"), "expected_value": True,
         "mock_simulation": [{"signal": signal("io.1", "di_0"), "start_value": True}]},
        mock_simulation=mock,
    )
    assert _tick(node) is _RUNNING  # The START request is built off the tick.
    sender.drain()
    assert _tick(node) is _RUNNING and mock.starts == []  # The START is queued.
    sender.drain()
    assert _tick(node) is _RUNNING  # Unknown: the same START is sent again.
    sender.drain()
    assert [start["node_start_index"] for start in mock.starts] == ["1", "1"]
    assert _tick(node) is _RUNNING and reads == []  # Acknowledged: the read is queued.
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert reads == ["io.check_input"] and guard.on_tick == []


def test_a_refused_mock_start_fails_the_node_and_a_queued_one_is_never_sent() -> None:
    guard, sender = _TickGuard(), _Sender()
    mock = _MockPort(guard)
    mock.outcomes = [MockSimulationRefusedError("mock_simulation_invalid", "no", None)]
    parameters = {"signal": signal("io.1", "di_0"), "expected_value": True,
                  "mock_simulation": [{"signal": signal("io.1", "di_0"), "start_value": True}]}
    node = _invocation(_GripperPort(guard), sender, "io.check_input", parameters,
                       mock_simulation=mock)
    _tick(node)
    sender.drain()
    _tick(node)
    sender.drain()
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "mock_simulation_refused"
    stopped = _invocation(_GripperPort(guard), sender, "io.check_input", parameters,
                          mock_simulation=mock)
    _tick(stopped)
    sender.drain()
    assert _tick(stopped) is _RUNNING  # Its START is queued.
    stopped.stop(py_trees.common.Status.INVALID)
    sender.drain()
    assert len(mock.starts) == 1  # The stopped node's START was withdrawn.
    assert len(mock.ends) == 2  # Each attempt that built a START ends it.


# --- Pure frame transforms ---------------------------------------------------


class _Frames:
    def __init__(self, guard: _TickGuard) -> None:
        self.guard = guard
        self.calls = 0

    def express_pose(self, identity, pose, frame):
        self.guard.check("query_target")
        self.calls += 1
        return FrameTransformResult(
            pose={**pose, "frame_ref": dict(frame), "position_m": [0.1, 0.0, 0.0]},
            metadata={},
        )


def _bounds(guard: _TickGuard, sender: _Sender):
    parameters = {
        "pose": {"frame_ref": {"kind": "cell_frame"}, "position_m": [1.0, 0.0, 0.0],
                 "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}},
        "frame_ref": {"component_id": "robot.1", "frame_id": "base"},
        "min_position_m": [0.0, -1.0, -1.0],
        "max_position_m": [0.5, 1.0, 1.0],
        "inclusive": True,
    }
    frames = _Frames(guard)
    node = GeometryCheckPoseBounds(
        name="bounds",
        parameters=parameters,
        value_sources={},
        input_resolver=_resolver(parameters),
        attempt_source=_attempts("bounds"),
        frame_transforms=frames,
        run_sender=sender.sender,
    )
    return node, frames


def test_a_frame_transform_leaves_the_tick_and_the_expression_waits_for_it() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, frames = _bounds(guard, sender)
    assert _tick(node) is _RUNNING and frames.calls == 0
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert frames.calls == 1 and guard.on_tick == []


def test_a_halt_withdraws_a_queued_frame_transform_and_it_is_sent_again() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, frames = _bounds(guard, sender)
    _tick(node)
    sender.sender.withdraw_queued()
    sender.drain()
    assert frames.calls == 0
    assert _tick(node) is _RUNNING
    sender.drain()
    assert _tick(node) is _SUCCESS and frames.calls == 1


# --- Control nodes (resolver wiring) ------------------------------------------


class _GuardedTaskPort(_TaskPort):
    def __init__(self, guard: _TickGuard) -> None:
        super().__init__()
        self.guard = guard

    def start(self, node_id, identity, inputs):
        self.guard.check(node_id)
        return super().start(node_id, identity, inputs)

    def acknowledge(self, task_id, identity):
        self.guard.check("acknowledge")
        super().acknowledge(task_id, identity)


def _control(node_id: str, guard: _TickGuard, sender: _Sender):
    document = _minimal_document()
    document["tree"] = {
        "node_id": node_id,
        "instance_id": "control",
        "parameters": (
            {} if node_id == "kernel.cell_stop" else {"target": target("ur5_robot", "arm")}
        ),
        "children": [],
    }
    catalog = _catalog()
    program = Program.from_dict(_identified(document), catalog)
    port = _GuardedTaskPort(guard)
    environment = build_node_environment()
    node = build_executable_node_resolver(environment).instantiate(
        environment.registry.record(node_id),
        NodeAdapterBuildContext(
            name="control",
            node_instance_id="control",
            parameters=program.tree.parameters,
            input_resolver=NodeAttemptInputResolver(
                program=program, catalog=catalog,
                value_store=ProgramValueStore.from_program(program),
            ),
            attempt_source=ProgramRunAttemptSource(3),
            ports=NodeAdapterPorts(kernel_controls=port),
            run_sender=sender.sender,
        ),
    )
    return node, port


@pytest.mark.parametrize("node_id", ["kernel.reset_operation"])
def test_program_control_nodes_leave_the_tick(node_id: str) -> None:
    # The Program's own Stop nodes do not: test_program_stop_never_waits.
    guard, sender = _TickGuard(), _Sender()
    node, port = _control(node_id, guard, sender)
    assert _tick(node) is _RUNNING and port.starts == []
    sender.drain()
    assert _tick(node) is _RUNNING  # The answer; its consumption is sent.
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert len(port.starts) == 1 and port.acknowledgements
    assert guard.calls == [node_id, "acknowledge"] and guard.on_tick == []


# --- Owner-task settling and motion retry ------------------------------------


class _Values:
    def reset_node_outcome(self, node_instance_id, *, metadata=None):
        pass

    def write_node_outcome(self, node_instance_id, outcome, *, metadata=None):
        pass


def _owner_task(node_type: type, port: Any, sender: _Sender, name: str,
                node_id: str, parameters: dict[str, Any], **kwargs: Any):
    numbers = iter(range(1, 100))
    resolver = SimpleNamespace(
        value_store=_Values(),
        snapshot=lambda **_: SimpleNamespace(parameters=parameters, blackboard={}),
    )
    if node_type is not OwnerTaskAdapter:
        kwargs.setdefault("program_clock", ProgramClock(lambda: 0.0))
    node = node_type(
        name=name, node_id=node_id, node_instance_id=name,
        attempt_source=SimpleNamespace(
            begin=lambda instance: NodeAttemptIdentity(1, instance, next(numbers))),
        parameters=parameters, value_sources={}, input_resolver=resolver,
        port=port, run_sender=sender.sender, **kwargs,
    )
    node.setup_with_descendants()
    return node


def _native_motion(guard: _TickGuard, transport: Any):
    """The real native port over ``transport``, every host call guarded."""

    guard.armed = False  # Binding the run is not a tick.
    port = NativeKernelProgramPort(
        KernelClient(transport_gateway(_guarded(transport, guard))),
        active_tcp_result=lambda *_args: True,
    )
    port.bind(_production_context())
    guard.armed = True
    return port


def _motion_on(port: Any, sender: _Sender, name: str = "move", **kwargs: Any):
    return _owner_task(
        MotionTaskAdapter, port.operations, sender, name, "motion.move_to_pose",
        dict(_motion_inputs().parameters), owner_port_name="kernel_operations",
        cancellable=True, **kwargs,
    )


def test_a_motion_reads_its_result_and_acknowledges_it_off_the_tick() -> None:
    guard, sender, transport = _TickGuard(), _Sender(), _ProductionTransport()
    port = _native_motion(guard, transport)
    node = _motion_on(port, sender)
    assert _tick(node) is _RUNNING
    sender.drain()  # The start.
    assert _tick(node) is _RUNNING
    operation = transport.operations[1]
    operation.update({"state": "completed", "operation_revision": 2})
    assert _tick(node) is _RUNNING  # No pushed table here: its read is sent.
    reads = transport.operation_reads
    sender.drain()
    assert transport.operation_reads == reads + 1
    assert _tick(node) is _RUNNING  # Completed: its acknowledgement is sent.
    assert [kind for kind, *_ in transport.controls] == []
    sender.drain()
    assert _tick(node) is _SUCCESS
    assert [kind for kind, *_ in transport.controls] == ["acknowledge"]
    assert transport.controls[0][2]["expected_operation_revision"] == 2
    assert {"submit", "read", "acknowledge_completed"} <= set(guard.calls)
    assert guard.on_tick == []


def test_a_failed_motion_reads_its_assessment_and_closes_off_the_tick() -> None:
    guard, sender, transport = _TickGuard(), _Sender(), _ProductionTransport()
    port = _native_motion(guard, transport)
    node = _motion_on(port, sender)
    assessments: list[int] = []

    def operation_recovery(operation_id):
        assessments.append(operation_id)
        return {
            "ok": True, "operation_revision": 2, "stop_event_id": None,
            "participants": [{"operation_participant_id": "participant.arm"}],
            "recovery_assessment": {
                "execution_state": "Failed", "status": "action_required",
                "eligible_close": True, "assessment_revision": 3,
                "control_command_epoch": "epoch.1", "execution_id": "execution.1",
            },
        }

    closes: list[str] = []

    def apply_operation_recovery(_operation_id, revision, body):
        closes.append(body["action"])
        return {"ok": True, "result": {
            "request_key": body["request_key"], "action": body["action"],
            "accepted": True, "resulting_assessment": {"status": "closed"},
        }}

    transport.operation_recovery = operation_recovery
    transport.apply_operation_recovery = apply_operation_recovery
    _tick(node)
    sender.drain()
    _tick(node)
    transport.operations[1].update({
        "state": "failed", "operation_revision": 2,
        "participants": [{"terminal_error": {"code": "planning_failed", "message": "no"}}],
    })
    statuses = []
    for _ in range(6):
        statuses.append(_tick(node))
        sender.drain()
        if node.status is _FAILURE:
            break
    assert node.status is _FAILURE and node.failure_reason_code == "planning_failed"
    assert assessments and closes == ["close_operation"]
    assert guard.on_tick == []


class _RetryPort:
    """Each start is accepted; its operation then fails, retry-eligible."""

    def __init__(self, guard: _TickGuard) -> None:
        self.guard = guard
        self.starts = 0
        self.failed: set[int] = set()

    def start(self, node_id, identity, inputs):
        self.guard.check("submit")
        self.starts += 1
        return OwnerTaskSnapshot(self.starts, OwnerTaskState.ACTIVE,
                                 metadata={"operation_id": self.starts})

    def observe(self, task_id):
        if task_id not in self.failed:
            return OwnerTaskSnapshot(task_id, OwnerTaskState.ACTIVE,
                                     metadata={"operation_id": task_id})
        return OwnerTaskSnapshot(task_id, OwnerTaskState.FAILED, reason_code="planning_failed",
                                 metadata={"operation_id": task_id, "recovery_assessment": {
                                     "execution_state": "Failed", "status": "retry_ready",
                                     "eligible_close": True,
                                     "retry_eligibility": {"eligible": True,
                                                           "cause": "planning_no_effect"},
                                 }})

    def close(self, task_id, identity, *, request_key):
        self.guard.check("apply_operation_recovery")
        return True

    def acknowledge(self, task_id, identity):
        self.guard.check("acknowledge_completed")

    def cancel(self, task_id, identity):
        pass


def _retrying(guard: _TickGuard, sender: _Sender):
    port, checks = _RetryPort(guard), []

    def attention(node_instance_id: str, request_key: bytes) -> None:
        guard.check("check_program_node_motion_attention")
        checks.append(node_instance_id)

    node = _owner_task(
        MotionTaskAdapter, port, sender, "move", "motion.move_to_pose",
        {"automatic_retry_delay_s": 0.0}, owner_port_name="kernel_operations",
        motion_attention=attention, cancellable=True,
    )
    _tick(node)
    sender.drain()
    _tick(node)  # The start's answer: the operation is observed.
    port.failed.add(1)
    assert _tick(node) is _RUNNING  # Failed, retry-eligible: its close is sent.
    sender.drain()
    assert _tick(node) is _RUNNING  # Closed: the retry waits (no delay).
    return node, port, checks


def test_the_automatic_retry_checks_attention_then_starts_off_the_tick() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, checks = _retrying(guard, sender)
    assert _tick(node) is _RUNNING and checks == []  # The check is sent.
    sender.drain()
    assert checks == ["move"] and port.starts == 1
    assert _tick(node) is _RUNNING and port.starts == 1  # Passed: the start is sent.
    sender.drain()
    assert port.starts == 2
    assert guard.calls == ["submit", "apply_operation_recovery",
                           "check_program_node_motion_attention", "submit"]
    assert guard.on_tick == []


def test_a_refused_attention_check_fails_the_retry_with_its_code() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, _checks = _retrying(guard, sender)

    class _Refused(RuntimeError):
        code = "operator_alarm_attention_required"

    def refused(node_instance_id: str, request_key: bytes) -> None:
        raise _Refused("an operator alarm needs attention")

    node._motion_attention = refused
    _tick(node)
    sender.drain()
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "operator_alarm_attention_required"
    assert port.starts == 1


def test_a_halt_drops_the_retry_check_and_execute_checks_again() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, checks = _retrying(guard, sender)
    assert _tick(node) is _RUNNING  # The check is queued.
    sender.sender.withdraw_queued()  # The Halt.
    assert node.reconcile_halt_task() == "inactive"
    sender.drain()
    assert checks == []
    assert _tick(node) is _RUNNING  # Execute: checked again before any start.
    sender.drain()
    assert checks == ["move"] and port.starts == 1
    _tick(node)
    sender.drain()
    assert port.starts == 2 and guard.on_tick == []


def test_a_sent_check_answered_across_a_stop_hold_is_made_again() -> None:
    guard, sender = _TickGuard(), _Sender()
    node, port, checks = _retrying(guard, sender)
    _tick(node)
    sender.drain()  # Answered, not read yet.
    node.observe_while_operator_stop_held()  # A Stop holds the node.
    assert _tick(node) is _RUNNING and port.starts == 1  # Run: a fresh check.
    sender.drain()
    assert checks == ["move", "move"]
    _tick(node)
    sender.drain()
    assert port.starts == 2


# --- A stopped unstarted task's operator-Stop scope read --------------------


class _StopScope:
    """The run's operator-Stop state: each refresh is a Kernel scope read."""

    def __init__(self, guard: _TickGuard, held: frozenset[str]) -> None:
        self.guard = guard
        self.held = held
        self.refreshes = 0

    def refresh(self) -> None:
        self.guard.check("read_program_execution_scope")
        self.refreshes += 1

    def snapshot(self) -> Any:
        return SimpleNamespace(held_nodes=self.held, hold_revision=self.refreshes)

    def observation_error(self) -> None:
        return None

    def note_task_observation_change(self) -> None:
        pass


class _RetiredByStopPort:
    """One accepted operation that a Stop ended before it started; the
    Kernel already retired it (OD-264)."""

    def __init__(self) -> None:
        self.stopped = False

    def start(self, node_id, identity, inputs):
        return OwnerTaskSnapshot(1, OwnerTaskState.ACTIVE, metadata={"operation_id": 1})

    def observe(self, task_id):
        if not self.stopped:
            return OwnerTaskSnapshot(1, OwnerTaskState.ACTIVE, metadata={"operation_id": 1})
        return OwnerTaskSnapshot(1, OwnerTaskState.INTERRUPTED, reason_code="operation_stopped",
                                 message="stopped before it started", metadata={
                                     "operation_id": 1, "resident": False, "stop_event_id": 3,
                                     "operation_failure": {
                                         "terminal_facts": [{"effect": "no_effect"}]},
                                 })

    def close(self, task_id, identity, *, request_key):
        raise AssertionError("a retired operation is never closed")

    def cancel(self, task_id, identity):
        pass


@pytest.mark.parametrize("held", [True, False])
def test_a_stopped_unstarted_task_reads_the_stop_scope_off_the_tick(held: bool) -> None:
    guard, sender, port = _TickGuard(), _Sender(), _RetiredByStopPort()
    scope = _StopScope(guard, frozenset({"move"} if held else ()))
    node = _owner_task(OwnerTaskAdapter, port, sender, "move", "motion.move_to_pose", {},
                       owner_port_name="kernel_operations", operator_stop_state=scope,
                       cancellable=True)
    _tick(node)
    sender.drain()  # The start.
    assert _tick(node) is _RUNNING
    port.stopped = True
    assert _tick(node) is _RUNNING and scope.refreshes == 0  # Its scope read is sent.
    sender.drain()
    assert scope.refreshes == 1
    # As base, which read the scope inside the tick and decided on it.
    if held:
        assert _tick(node) is _RUNNING and node.presented_held
        assert node._held_before_admission == 1
    else:
        assert _tick(node) is _FAILURE and node.failure_reason_code == "operation_stopped"
    assert scope.refreshes == 1 and guard.on_tick == []
