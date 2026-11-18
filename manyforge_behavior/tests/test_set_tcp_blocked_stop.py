"""Set TCP Stop while the real owner port is blocked in synchronous submit."""

from copy import deepcopy
from dataclasses import replace
from threading import Event
import time

import py_trees
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot
from manyforge_behavior.production_ports import NativeKernelProgramPort

from .test_executable_node_runtime import _leaf_node, _set_tcp_stop_run
from .test_production_ports import _context, _Transport
from .v3_fixtures import frame, target, target_binding


def _ur5_context():
    """The shared port context, rebound to the target the Set TCP run authors."""
    context = _context()
    binding = dict(context.program_binding)
    binding["target_bindings"] = [target_binding("ur5_robot", "arm", [
        {"capability_id": "execution.trajectory.execute", "command_schema": None},
    ])]
    binding["frame_bindings"] = [{"reference": frame("ur5_robot", "tcp")}]
    return replace(context, program_run_id=1, program_binding=binding)


def _eventually(condition):
    deadline = time.monotonic() + 5.0
    while not condition():
        assert time.monotonic() < deadline, "condition not reached"
        time.sleep(0.001)


def _tick_until(run, condition):
    deadline = time.monotonic() + 5.0
    while not condition():
        assert time.monotonic() < deadline, "condition not reached"
        run.tick_once()
        time.sleep(0.001)


@pytest.mark.parametrize("policy", ["hold", "continue"])
def test_stop_while_a_sent_set_tcp_step_is_blocked_completes_it_then_obeys_policy(policy):
    """OD-213: the tick sends the Set TCP step on the run's sender and never
    waits for it. A Stop that arrives after the step was sent does not
    withdraw it: the step completes, the node returns its real result, and
    the Stop policy then decides whether the next node starts."""
    entered = Event()
    release = Event()
    confirmed = []
    rows = [{"component_id": "ur5_robot", "entry": {
        "target": target("ur5_robot", "arm"), "frame": frame("ur5_robot", "tcp")},
        "registration_generation": 1, "sequence": 1}]

    class BlockingSubmit(_Transport):
        def submit(self, body):
            self.submissions.append(deepcopy(body))
            if len(self.submissions) == 1:
                entered.set()
                if not release.wait(5):
                    raise TimeoutError("test did not release Set TCP")
            return {"ok": True, "accepted": {
                "request_key": body["request_context"]["request_key"],
                "operation_id": len(self.submissions),
            }, "active_tcp": rows}

    transport = BlockingSubmit()
    port = NativeKernelProgramPort(
        KernelClient(transport),
        active_tcp_result=lambda run, robot, value: confirmed.append((run, robot, deepcopy(value))),
    )
    port.bind(_ur5_context())
    run = _set_tcp_stop_run(policy=policy, task=port.operations)
    try:
        # The tick returns at once; the step is sent off the tick.
        assert run.tick_once() is py_trees.common.Status.RUNNING
        assert entered.wait(5), "Set TCP did not reach its submit"
        assert len(transport.submissions) == 1
        assert confirmed == []
        # The selected-scope Stop arrives while the sent step is blocked.
        run._operator_stop_state.publish(  # noqa: SLF001
            OperatorMotionStopSnapshot(1, frozenset({"root", "first", "second"}))
        )
        assert run.tick_once() is py_trees.common.Status.RUNNING
        assert _leaf_node(run, "first").status is py_trees.common.Status.RUNNING
    finally:
        release.set()
    _eventually(lambda: len(confirmed) == 1)
    assert confirmed[0] == (1, "ur5_robot", rows)
    _tick_until(run, lambda: _leaf_node(run, "first").status is py_trees.common.Status.SUCCESS)
    assert transport.controls == []
    if policy == "hold":
        for _ in range(5):
            assert run.tick_once() is py_trees.common.Status.RUNNING
        assert len(transport.submissions) == 1  # The next node waits for Run/Resume.
        run._operator_stop_state.publish(OperatorMotionStopSnapshot(2))  # noqa: SLF001
    _tick_until(run, lambda: run.status is py_trees.common.Status.SUCCESS)
    assert len(transport.submissions) == 2
    assert len(confirmed) == 2
    assert transport.operation_reads == 0


def test_forced_end_while_a_set_tcp_step_is_sent_waits_for_its_real_result():
    """OD-213: the Forced Program End invalidates the tree at once; the sent
    step completes and ends its node with the real result, never
    `interrupted`, and the run settles only after that answer."""
    entered = Event()
    release = Event()

    class BlockingSubmit(_Transport):
        def submit(self, body):
            self.submissions.append(deepcopy(body))
            entered.set()
            if not release.wait(5):
                raise TimeoutError("test did not release Set TCP")
            return {"ok": True, "accepted": {
                "request_key": body["request_context"]["request_key"],
                "operation_id": len(self.submissions),
            }, "active_tcp": [{"component_id": "ur5_robot", "entry": {
                "target": target("ur5_robot", "arm"), "frame": frame("ur5_robot", "tcp")},
                "registration_generation": 1, "sequence": 1}]}

    transport = BlockingSubmit()
    port = NativeKernelProgramPort(KernelClient(transport), active_tcp_result=lambda *args: True)
    port.bind(_ur5_context())
    run = _set_tcp_stop_run(policy="hold", task=port.operations)
    try:
        assert run.tick_once() is py_trees.common.Status.RUNNING
        assert entered.wait(5)
        run.stop()
        assert run.settle_sent_requests() is False
        assert _leaf_node(run, "first").interrupted_reason_code is None
    finally:
        release.set()
    _eventually(run.settle_sent_requests)
    assert run.node_statuses["first"] == "success"
    assert run.node_statuses["second"] == "idle"
    assert len(transport.submissions) == 1
