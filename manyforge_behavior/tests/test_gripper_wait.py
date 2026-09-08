"""The read-only waiter keeps one scope and uses the existing tree ticks."""

from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.nodes.v2_owner import GripperStatusWaitAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
)


def _waiter():
    clock = [10.0]
    scope = (7, 2, "source", "revision", "provider.1", "route.1", 3)
    calls = []
    port = SimpleNamespace(
        result=OwnerInvocationResult(
            False,
            reason_code="gripper_status_not_matched",
            metadata={"command_scope": scope, "command_state": "completed"},
        )
    )

    def invoke(*args):
        calls.append(args)
        return port.result

    port.invoke = invoke
    parameters = {
        "target_role": "gripper",
        "expected_state": "grasp_confirmed",
        "timeout_s": 1.0,
    }
    node = GripperStatusWaitAdapter(
        name="wait",
        node_id="gripper.wait_for_status",
        node_instance_id="wait",
        attempt_source=SimpleNamespace(
            begin=lambda _: NodeAttemptIdentity("run.1", "wait", "attempt.1")
        ),
        parameters=parameters,
        value_sources={},
        input_resolver=SimpleNamespace(
            snapshot=lambda **_: SimpleNamespace(
                parameters=parameters,
                blackboard={},
                embedded_values={},
            )
        ),
        port=port,
        clock=lambda: clock[0],
    )
    return node, clock, port, calls, scope


def test_wait_is_bounded_read_only_and_completes_without_reissuing_a_command():
    node, clock, port, calls, scope = _waiter()
    for _ in range(100):
        node.tick_once()
        assert node.status is py_trees.common.Status.RUNNING
    assert len(calls) == 1
    port.result = OwnerInvocationResult(
        True, metadata={"command_scope": scope, "command_state": "completed"}
    )
    clock[0] += 0.1
    node.tick_once()
    assert node.status is py_trees.common.Status.SUCCESS
    assert len(calls) == 2


@pytest.mark.parametrize("field", range(7))
def test_wait_never_follows_a_replacement_command_or_source(field):
    node, clock, port, _, scope = _waiter()
    node.tick_once()
    changed = list(scope)
    changed[field] = "replaced"
    port.result = OwnerInvocationResult(True, metadata={"command_scope": tuple(changed)})
    clock[0] += 0.1
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "gripper_wait_stale"


def test_timeout_does_not_do_another_read_and_cancelling_wait_is_only_local():
    node, clock, port, calls, _ = _waiter()
    node.tick_once()
    node.stop(py_trees.common.Status.INVALID)  # Port has no command/cancel API.
    assert len(calls) == 1
    node.tick_once()
    clock[0] += 1.0
    node.tick_once()
    assert node.failure_reason_code == "gripper_wait_timeout"
    assert len(calls) == 2


@pytest.mark.parametrize(
    "reason,state",
    [("grasp_not_confirmed", "completed"), ("gripper_status_not_matched", "failed")],
)
def test_denied_grasp_or_failed_command_is_not_hidden_as_waiting(reason, state):
    node, _, port, _, scope = _waiter()
    port.result = OwnerInvocationResult(
        False,
        reason_code=reason,
        metadata={"command_scope": scope, "command_state": state},
    )
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE


def test_slow_read_cannot_report_late_success():
    node, clock, port, _, scope = _waiter()

    def invoke(*_):
        clock[0] += 1.1
        return OwnerInvocationResult(True, metadata={"command_scope": scope})

    port.invoke = invoke
    node.tick_once()
    assert node.failure_reason_code == "gripper_wait_timeout"
