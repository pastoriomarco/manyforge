"""Program pneumatic sequence: valve writes through the one write path
then the ordinary watch, read and operation ports."""

from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior.nodes.v2_owner import GripperCommandAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerTaskSnapshot,
    OwnerTaskState,
)
from manyforge_behavior.program_clock import ProgramClock


class _Operations:
    def __init__(self, route, events):
        self.route = route
        self.events = events

    def pneumatic_plan(self, identity, inputs):
        return self.route

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
    """The io_signals port: valve writes (`io.set_output`) and input reads."""

    def __init__(self, levels, events, writes):
        self.levels = levels
        self.events = events
        # Scripted result per valve role; absent roles succeed.
        self.writes = writes

    def invoke(self, node_id, identity, inputs):
        row = inputs.parameters
        if node_id == "io.set_output":
            self.events.append(("output", row["resource_role"], row["value"]))
            result = self.writes.get(row["resource_role"], OwnerInvocationResult(True))
            if isinstance(result, Exception):
                raise result
            return result
        self.events.append(("read", row["resource_role"]))
        if self.levels[row["resource_role"]] is row["expected_value"]:
            return OwnerInvocationResult(True)
        return OwnerInvocationResult(False, reason_code="io_input_not_matched")


def _node(*, required, dwell=0.0, writes=None):
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
    operations = _Operations(route, events)
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
        io_watches=_Watches(levels, events), io_signals=_Reads(levels, events, writes or {}),
        transport_timeout_s=5.0,
    )
    return adapter, events, levels, now


def _tick_until(node, predicate, limit=20):
    for _ in range(limit):
        node.tick_once()
        if predicate():
            return
    pytest.fail("gripper did not reach the expected stage")


def test_valves_are_written_before_waits_and_logical_completion():
    node, events, _, _ = _node(required=("closed", "part"))
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)
    assert [event[:2] for event in events] == [
        ("output", "valve-a"), ("output", "valve-b"),
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


@pytest.mark.parametrize("result,code", [
    # failed and unknown write results, and an admission refusal, all carry
    # the host's code.
    (OwnerInvocationResult(False, reason_code="io_output_write_failed", message="refused"),
     "io_output_write_failed"),
    (OwnerInvocationResult(False, reason_code="io_output_acceptance_unknown", message="unknown"),
     "io_output_acceptance_unknown"),
    (OwnerInvocationResult(False, reason_code="cell_latched", message="latched"), "cell_latched"),
])
def test_failed_valve_write_fails_the_node_without_resend_or_later_writes(result, code):
    node, events, _, _ = _node(required=("part",), writes={"valve-a": result})
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == code
    assert events == [("output", "valve-a", False)]


def test_lost_write_reply_fails_once_and_is_never_resent():
    node, events, _, _ = _node(required=(), writes={"valve-b": RuntimeError("socket closed")})
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "owner_request_failed"
    assert [event[:2] for event in events] == [("output", "valve-a"), ("output", "valve-b")]


def test_no_inputs_preserves_dwell_before_logical_completion():
    node, events, _, now = _node(required=(), dwell=0.5)
    _tick_until(node, lambda: len([event for event in events if event[0] == "output"]) == 2)
    node.tick_once()
    assert not any(event[0] == "logical" for event in events)
    now[0] = 0.5
    _tick_until(node, lambda: node.status is py_trees.common.Status.SUCCESS)


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
