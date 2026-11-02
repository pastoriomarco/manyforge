"""Set TCP Stop while the real owner port is blocked in synchronous submit."""

from copy import deepcopy
from dataclasses import replace
from threading import Event, Thread

import py_trees
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot
from manyforge_behavior.production_ports import NativeKernelProgramPort

from .test_executable_node_runtime import _leaf_node, _set_tcp_stop_run
from .test_production_ports import _context, _Transport


@pytest.mark.parametrize("policy", ["hold", "continue"])
def test_stop_during_blocked_set_tcp_submit_settles_then_obeys_policy(policy):
    entered = Event()
    release = Event()
    confirmed = []
    rows = [{"component_id": "robot.1", "entry": {
        "component_id": "robot.1", "target_id": "arm.command", "frame_role_id": "tcp"},
        "owner_epoch": "kernel.epoch.7", "registration_generation": 1, "sequence": 1}]

    class BlockingSubmit(_Transport):
        def submit(self, body):
            self.submissions.append(deepcopy(body))
            if len(self.submissions) == 1:
                entered.set()
                if not release.wait(5):
                    raise TimeoutError("test did not release Set TCP")
            return {"ok": True, "accepted": {
                "request_id": body["request_context"]["request_id"],
                "operation_id": f"operation-{len(self.submissions)}",
            }, "active_tcp": rows}

    transport = BlockingSubmit()
    port = NativeKernelProgramPort(
        KernelClient(transport),
        active_tcp_result=lambda run, robot, value: confirmed.append((run, robot, deepcopy(value))),
    )
    port.bind(replace(_context(), program_run_id="run.test"))
    run = _set_tcp_stop_run(policy=policy, task=port.operations)
    outcomes = []
    errors = []

    def tick():
        try:
            outcomes.append(run.tick_once())
        except BaseException as error:
            errors.append(error)

    worker = Thread(target=tick)
    worker.start()
    try:
        assert entered.wait(5), "Set TCP did not reach the synchronous submit"
        assert len(transport.submissions) == 1
        assert confirmed == []
        # Publish the selected-scope Stop while the accepted TCP step is still
        # blocked; no synthetic ACTIVE task or cancellation is introduced.
        run._operator_stop_state.publish(  # noqa: SLF001
            OperatorMotionStopSnapshot(1, frozenset({"root", "first", "second"}))
        )
    finally:
        release.set()
        worker.join(5)
    assert not worker.is_alive()
    assert errors == []
    assert confirmed[0] == ("run.test", "robot.1", rows)
    assert _leaf_node(run, "first").status is py_trees.common.Status.SUCCESS
    assert transport.controls == []
    if policy == "hold":
        assert outcomes == [py_trees.common.Status.RUNNING]
        assert len(transport.submissions) == 1
        assert len(confirmed) == 1
        run._operator_stop_state.publish(OperatorMotionStopSnapshot(2))  # noqa: SLF001
        assert run.tick_once() is py_trees.common.Status.SUCCESS
    else:
        assert outcomes == [py_trees.common.Status.SUCCESS]
    assert len(transport.submissions) == 2
    assert len(confirmed) == 2
    assert transport.operation_reads == 0
