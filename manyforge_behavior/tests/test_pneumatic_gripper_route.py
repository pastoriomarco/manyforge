"""Program pneumatic sequence uses ordinary operation, watch and read ports."""

from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.nodes.v2_owner import GripperCommandAdapter
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationHeldError,
    OwnerInvocationResult,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.program_clock import ProgramClock


class _Operations:
    def __init__(self, route, events, *, output_evidence="source_accepted"):
        self.route = route
        self.events = events
        self.output_evidence = output_evidence

    def pneumatic_plan(self, identity, inputs):
        return self.route

    def start_pneumatic_output(self, identity, inputs, step):
        self.events.append(("output", step, dict(inputs.parameters)))
        return OwnerTaskSnapshot(f"valve.{step}", OwnerTaskState.SUCCEEDED,
                                 metadata={"output_evidence": self.output_evidence})

    def start(self, node_id, identity, inputs):
        self.events.append(("logical", dict(inputs.parameters)))
        return OwnerTaskSnapshot("gripper", OwnerTaskState.SUCCEEDED)

    def acknowledge(self, task_id, identity):
        self.events.append(("ack", task_id))


class _Watches:
    def __init__(self, levels, events):
        self.levels = levels
        self.events = events
        self.rows = {}

    def start(self, node_id, identity, inputs):
        row = dict(inputs.parameters)
        self.events.append(("watch", row["resource_role"]))
        self.rows["watch"] = row
        return self._snapshot()

    def observe(self, task_id):
        return self._snapshot()

    def _snapshot(self):
        row = self.rows["watch"]
        matched = self.levels[row["resource_role"]] is row["expected_value"]
        return OwnerTaskSnapshot("watch", OwnerTaskState.SUCCEEDED if matched else OwnerTaskState.ACTIVE,
                                 metadata={"quality": "fresh"})

    def acknowledge(self, task_id, identity):
        self.events.append(("watch-release", self.rows[task_id]["resource_role"]))

    cancel = acknowledge


class _Reads:
    def __init__(self, levels, events):
        self.levels = levels
        self.events = events

    def invoke(self, node_id, identity, inputs):
        row = inputs.parameters
        self.events.append(("read", row["resource_role"]))
        if self.levels[row["resource_role"]] is row["expected_value"]:
            return OwnerInvocationResult(True)
        return OwnerInvocationResult(False, reason_code="io_input_not_matched")


def _node(*, required, dwell=0.0, evidence="source_accepted"):
    from manyforge_behavior.orchestration_ports import OwnerNodeInputs

    events, levels, now = [], {"closed": True, "part": True}, [0.0]
    route = {
        "outputs": [
            {"target_role": "io", "resource_role": "valve-a", "value": False},
            {"target_role": "io", "resource_role": "valve-b", "value": True},
        ],
        "inputs": [
            {"target_role": "io", "resource_role": name, "expected_value": True}
            for name in required
        ],
        "logical": OwnerNodeInputs({"position": {"kind": "linear_m", "value_m": 0.0}}),
        "dwell_s": dwell,
    }
    operations = _Operations(route, events, output_evidence=evidence)
    attempts = []
    def begin(node):
        identity = NodeAttemptIdentity("run", node, f"attempt.{len(attempts)}")
        attempts.append(identity)
        return identity
    adapter = GripperCommandAdapter(
        name="close", node_id="gripper.command_position", node_instance_id="close",
        attempt_source=SimpleNamespace(begin=begin),
        parameters={"dwell_while_held": "pause"}, value_sources={},
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={})),
        port=operations, program_clock=ProgramClock(lambda: now[0]),
        io_watches=_Watches(levels, events), io_signals=_Reads(levels, events),
    )
    return adapter, events, levels, now


def _tick_until(node, predicate, limit=20):
    for _ in range(limit):
        node.tick_once()
        if predicate():
            return
    pytest.fail("gripper did not reach the expected stage")


def test_valves_are_acknowledged_before_waits_and_logical_completion():
    node, events, _, _ = _node(required=("closed", "part"))
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)
    assert [event[:2] for event in events] == [
        ("output", 0), ("ack", "valve.0"), ("output", 1), ("ack", "valve.1"),
        ("watch", "closed"), ("watch-release", "closed"),
        ("watch", "part"), ("watch-release", "part"),
        ("read", "closed"), ("read", "part"), ("logical", {"position": {"kind": "linear_m", "value_m": 0.0}}),
        ("ack", "gripper"),
    ]


def test_confirmation_rewatches_changed_input_without_rewriting_valves():
    node, events, levels, _ = _node(required=("closed", "part"))
    _tick_until(node, lambda: ("read", "closed") in events)
    levels["part"] = False
    _tick_until(node, lambda: events.count(("watch", "part")) == 2)
    assert node.status is py_trees.common.Status.RUNNING
    levels["part"] = True
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)
    assert sum(event[0] == "output" for event in events) == 2
    assert events.count(("read", "closed")) == 2


@pytest.mark.parametrize("dwell", [0.0, 0.5])
def test_input_wait_zero_has_no_deadline_positive_expires(dwell):
    node, events, levels, now = _node(required=("part",), dwell=dwell)
    levels["part"] = False
    _tick_until(node, lambda: ("watch", "part") in events)
    now[0] = 1.0
    node.tick_once()
    assert node.status is (py_trees.common.Status.RUNNING if dwell == 0 else py_trees.common.Status.FAILURE)
    assert not any(event[0] == "logical" for event in events)


def test_unconfirmed_output_is_not_acknowledged_or_followed_by_more_writes():
    node, events, _, _ = _node(required=(), evidence="acceptance_unknown")
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_output_acceptance_unconfirmed"
    assert [event[0] for event in events] == ["output"]


def test_no_inputs_preserves_dwell_before_logical_completion():
    node, events, _, now = _node(required=(), dwell=0.5)
    _tick_until(node, lambda: len([event for event in events if event[0] == "output"]) == 2)
    node.tick_once()
    assert not any(event[0] == "logical" for event in events)
    now[0] = 0.5
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)


def test_stop_after_accepted_valve_settles_then_continues_same_attempt():
    node, events, _, _ = _node(required=())
    class ActiveFirst(_Operations):
        def start_pneumatic_output(self, identity, inputs, step):
            self.events.append(("output", step, identity.node_attempt_id))
            if step == 0:
                return OwnerTaskSnapshot("valve.0", OwnerTaskState.ACTIVE)
            return OwnerTaskSnapshot(f"valve.{step}", OwnerTaskState.SUCCEEDED,
                                     metadata={"output_evidence": "source_accepted"})
        def observe(self, task_id):
            return OwnerTaskSnapshot(task_id, OwnerTaskState.SUCCEEDED,
                                     metadata={"output_evidence": "source_accepted"})
    node._port = ActiveFirst(node._port.route, events)
    state = OperatorMotionStopState({"close": ()})
    node._operator_stop_state = state
    wrapper = OperatorMotionStopPolicy(child=node, node_instance_id="close", stop_state=state)
    wrapper.tick_once()  # valve 0 accepted, still active
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"close"})))
    wrapper.tick_once()  # observe and acknowledge valve 0 during the hold
    assert ("ack", "valve.0") in events
    state.publish(OperatorMotionStopSnapshot(2))
    _tick_until(wrapper, lambda: wrapper.status is py_trees.common.Status.SUCCESS)
    output_attempts = [(event[1], event[2]) for event in events if event[0] == "output"]
    assert output_attempts == [(0, "attempt.0"), (1, "attempt.0")]


def test_stop_refusing_second_valve_starts_fresh_attempt_and_rewrites_first():
    node, events, _, _ = _node(required=())
    snapshot = [OperatorMotionStopSnapshot()]
    state = OperatorMotionStopState({"close": ()}, read_scope=lambda: snapshot[0])
    state.refresh()
    class HeldSecond(_Operations):
        refused = False
        def start_pneumatic_output(self, identity, inputs, step):
            if step == 1 and not self.refused:
                self.refused = True
                snapshot[0] = OperatorMotionStopSnapshot(1, frozenset({"close"}))
                raise OwnerInvocationHeldError(1)
            self.events.append(("output", step, identity.node_attempt_id))
            return OwnerTaskSnapshot(f"valve.{step}", OwnerTaskState.SUCCEEDED,
                                     metadata={"output_evidence": "source_accepted"})
    node._port = HeldSecond(node._port.route, events)
    node._operator_stop_state = state
    wrapper = OperatorMotionStopPolicy(child=node, node_instance_id="close", stop_state=state)
    wrapper.tick_once()  # first valve confirmed
    wrapper.tick_once()  # second refused because Stop crossed admission
    assert node.presented_held
    snapshot[0] = OperatorMotionStopSnapshot(2)
    state.refresh()
    _tick_until(wrapper, lambda: wrapper.status is py_trees.common.Status.SUCCESS)
    output_attempts = [(event[1], event[2]) for event in events if event[0] == "output"]
    assert output_attempts == [(0, "attempt.0"), (0, "attempt.1"), (1, "attempt.1")]




_PART_CHECK = {"node_id": "io.check_input",
               "parameters": {"target_role": "io", "resource_role": "part", "expected_value": True}}


def _confirming(node, events, check):
    """The owner names the grasp read on the Close's own task metadata."""
    class Confirming(_Operations):
        def start(self, node_id, identity, inputs):
            self.events.append(("logical", dict(inputs.parameters)))
            return OwnerTaskSnapshot("gripper", OwnerTaskState.SUCCEEDED, metadata=(
                {} if check is None else {"grasp_confirmation_check": check}))
    node._port = Confirming(node._port.route, events)
    return node


def test_without_required_policy_close_reads_no_grasp_evidence():
    node, events, levels, _ = _node(required=())
    node = _confirming(node, events, None)
    levels["part"] = False
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)
    assert ("read", "part") not in events


@pytest.mark.parametrize("grasped", [True, False])
def test_required_pneumatic_close_reads_workpiece_input_at_completion(grasped):
    node, events, levels, _ = _node(required=())
    node = _confirming(node, events, _PART_CHECK)
    levels["part"] = grasped
    _tick_until(node, lambda: node.status is not py_trees.common.Status.RUNNING)
    # Read once, after the logical command completed and was acknowledged.
    assert events[-1] == ("read", "part") and ("ack", "gripper") in events
    if grasped:
        assert node.status is py_trees.common.Status.SUCCESS
    else:
        assert node.status is py_trees.common.Status.FAILURE
        assert node.failure_reason_code == "grasp_not_confirmed"
        assert "workpiece_grasped" in node.feedback_message


class _StatusReads:
    def __init__(self, confirmed, events):
        self.confirmed = confirmed
        self.events = events

    def invoke(self, node_id, identity, inputs):
        self.events.append(("status", node_id))
        return OwnerInvocationResult(True, metadata={"grasp_confirmed": self.confirmed})


@pytest.mark.parametrize("confirmed,status", [
    (True, py_trees.common.Status.SUCCESS),
    (False, py_trees.common.Status.FAILURE),
    (None, py_trees.common.Status.FAILURE),
])
def test_required_position_close_needs_stall_window_evidence(confirmed, status):
    node, events, _, _ = _node(required=())
    check = {"node_id": "gripper.check_status",
             "parameters": {"target_role": "gripper", "expected_state": "reached"}}

    class Position(_Operations):
        def pneumatic_plan(self, identity, inputs):
            return None

        def start(self, node_id, identity, inputs):
            return OwnerTaskSnapshot("gripper", OwnerTaskState.SUCCEEDED,
                                     metadata={"grasp_confirmation_check": check})

    node._port = Position(None, events)
    node._io_signals = _StatusReads(confirmed, events)
    _tick_until(node, lambda: node.status is not py_trees.common.Status.RUNNING)
    assert node.status is status
    assert events[-1] == ("status", "gripper.check_status")
    if status is py_trees.common.Status.FAILURE:
        assert node.failure_reason_code == "grasp_not_confirmed"
