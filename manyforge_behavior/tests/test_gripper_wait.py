"""The read-only waiter keeps one scope and uses the existing tree ticks."""

from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.nodes.v2_owner import GripperStatusWaitAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
)
from manyforge_behavior.program_clock import ProgramClock


def _waiter(clock=None, program_clock=None, timeout_while_held="pause", **adapter_options):
    clock = clock or [10.0]
    program_clock = program_clock or ProgramClock(lambda: clock[0])
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
        # The resolver supplies the catalog default `pause` when omitted.
        "timeout_while_held": timeout_while_held,
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
            )
        ),
        port=port,
        program_clock=program_clock,
        cancellable=True,
        **adapter_options,
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


def test_stopping_a_running_wait_reports_interrupted_never_failure():
    node, _, _, calls, _ = _waiter()
    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING

    node.stop(py_trees.common.Status.INVALID)  # Program Stop, Reset or cancel.

    assert node.outcome == "interrupted"
    assert node.presented_interrupted
    assert node.interrupted_reason_code == "program_node_interrupted"
    assert node.failure_reason_code is None
    assert len(calls) == 1  # Read-only: nothing to cancel at the owner.
    node.tick_once()  # A fresh attempt starts clean.
    assert node.outcome is None and node.interrupted_reason_code is None


def test_a_finished_wait_is_not_reinterpreted_as_interrupted():
    node, _, port, _, scope = _waiter()
    port.result = OwnerInvocationResult(True, metadata={"command_scope": scope})
    node.tick_once()
    node.stop(py_trees.common.Status.INVALID)
    assert node.outcome is None
    assert node.interrupted_reason_code is None


def _held_waiter(timeout_while_held):
    clock = [10.0]
    program_clock = ProgramClock(lambda: clock[0])
    state = OperatorMotionStopState({"wait": ()}, clock=program_clock.now)
    node, clock, _, _, _ = _waiter(
        clock,
        program_clock,
        timeout_while_held,
        operator_stop_state=state,
        operator_stop_hold_scope="wait",
    )
    return node, clock, state, program_clock


def test_operator_stop_hold_does_not_turn_into_a_timeout_failure():
    node, clock, state, _ = _held_waiter("pause")
    node.tick_once()
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    clock[0] += 5.0  # Held well past the 1 s timeout; the hold stops ticks.
    state.publish(OperatorMotionStopSnapshot(2, frozenset()))
    clock[0] += 0.5

    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert node.failure_reason_code is None
    clock[0] += 0.5  # 1 s of unheld waiting: the timeout still bounds it.
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "gripper_wait_timeout"


def test_held_time_needs_both_the_stop_state_and_its_scope():
    state = OperatorMotionStopState({"wait": ()})
    with pytest.raises(ValueError, match="held-time scope"):
        _waiter(operator_stop_state=state)


def test_count_mode_counts_operator_stop_held_time():
    node, clock, state, _ = _held_waiter("count")
    node.tick_once()
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"wait"})))
    clock[0] += 5.0
    state.publish(OperatorMotionStopSnapshot(2, frozenset()))
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "gripper_wait_timeout"


@pytest.mark.parametrize("timeout_while_held", ["pause", "count"])
def test_halted_program_time_never_counts_and_the_same_attempt_continues(
    timeout_while_held,
):
    node, clock, _, program_clock = _held_waiter(timeout_while_held)
    node.tick_once()
    attempt = node.attempt_identity
    program_clock.freeze()  # Halt
    clock[0] += 60.0
    program_clock.resume()  # Execute
    clock[0] += 0.9
    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert node.attempt_identity is attempt
    clock[0] += 0.1
    node.tick_once()
    assert node.failure_reason_code == "gripper_wait_timeout"
