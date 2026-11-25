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
        identity = NodeAttemptIdentity(1, node, len(attempts) + 1)
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


# --- With the run's sender (OD-380 D2): every owner call leaves the tick ------


def _sent_node(*, required, check=None, dwell=0.0):
    from manyforge_behavior.orchestration_ports import OwnerNodeInputs

    from .test_owner_calls_leave_the_tick import _Sender, _TickGuard, _guarded

    node, events, levels, now = _node(required=required, dwell=dwell)
    if check is not None:
        node = _confirming(node, events, check)
    guard, sender = _TickGuard(), _Sender()
    route = node._port.route
    adapter = GripperCommandAdapter(
        name="close", node_id="gripper.command_position", node_instance_id="close",
        attempt_source=node._attempt_source,
        parameters={"dwell_while_held": "pause"}, value_sources={},
        input_resolver=node._input_resolver,
        port=_guarded(node._port, guard), program_clock=ProgramClock(lambda: now[0]),
        io_watches=_guarded(node._io_watches, guard),
        io_signals=_guarded(node._io_signals, guard),
        run_sender=sender.sender, valve_sender=sender.sender,
    )
    assert route["logical"] == OwnerNodeInputs({"position": {"kind": "linear_m", "value_m": 0.0}})
    return adapter, events, levels, guard, sender, now


def _sent_ticks_until(node, sender, predicate, limit=40):
    for _ in range(limit):
        node.tick_once()
        sender.drain()  # The answers arrive between ticks.
        if predicate():
            return
    pytest.fail(f"gripper did not reach the expected stage: {node.status} {node.feedback_message!r}")


def test_with_the_run_sender_the_input_watches_and_reads_leave_the_tick():
    node, events, _levels, guard, sender, _now = _sent_node(required=("closed", "part"))
    _sent_ticks_until(node, sender, lambda: node.status is not py_trees.common.Status.RUNNING)
    assert node.status is py_trees.common.Status.SUCCESS
    # The same owner calls in the same order as inside the tick, none on it.
    assert [event[:2] for event in events] == [
        ("output", "valve-a"), ("output", "valve-b"),
        ("watch", "closed"), ("watch-release", "closed"),
        ("watch", "part"), ("watch-release", "part"),
        ("read", "closed"), ("read", "part"), ("logical", {"position": {"kind": "linear_m", "value_m": 0.0}}),
        ("ack", "gripper"),
    ]
    assert {"pneumatic_plan", "start", "acknowledge", "invoke"} <= set(guard.calls)
    assert guard.on_tick == []


def test_with_the_run_sender_a_watch_that_waits_is_read_off_the_tick_until_it_matches():
    node, events, levels, guard, sender, _now = _sent_node(required=("part",))
    levels["part"] = False
    _sent_ticks_until(node, sender, lambda: guard.calls.count("observe") >= 2)
    assert node.status is py_trees.common.Status.RUNNING
    levels["part"] = True
    _sent_ticks_until(node, sender, lambda: node.status is not py_trees.common.Status.RUNNING)
    assert node.status is py_trees.common.Status.SUCCESS
    assert events.count(("watch", "part")) == 1 and guard.on_tick == []


def test_with_the_run_sender_a_queued_input_read_at_the_deadline_is_withdrawn():
    node, events, levels, guard, sender, now = _sent_node(required=("part",), dwell=0.5)
    levels["part"] = False
    _sent_ticks_until(node, sender, lambda: ("watch", "part") in events)
    node.tick_once()  # Takes the registration: the input does not match yet.
    node.tick_once()  # Queues the watch's next read.
    queued = node._settle_request[2]
    assert not queued.sent
    now[0] = 1.0  # The deadline passed.
    node.tick_once()
    assert queued.withdrawn
    # Its watch is released first, off the tick, as base released it right
    # before the node failed.
    assert node.status is py_trees.common.Status.RUNNING
    assert ("watch-release", "part") not in events
    sender.drain()
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_wait_timeout"
    assert events.count(("watch-release", "part")) == 1 and guard.on_tick == []


@pytest.mark.parametrize("grasped", [True, False])
def test_with_the_run_sender_the_grasp_check_leaves_the_tick(grasped):
    node, events, levels, guard, sender, _now = _sent_node(required=(), check=_PART_CHECK)
    levels["part"] = grasped
    _sent_ticks_until(node, sender, lambda: ("ack", "gripper") in events)
    node.tick_once()  # The completed Close sends its grasp read.
    assert node.status is py_trees.common.Status.RUNNING
    assert ("read", "part") not in events
    sender.drain()
    node.tick_once()
    assert events[-1] == ("read", "part") and events.count(("read", "part")) == 1
    assert node.status is (
        py_trees.common.Status.SUCCESS if grasped else py_trees.common.Status.FAILURE
    )
    if not grasped:
        assert node.failure_reason_code == "grasp_not_confirmed"
    assert guard.on_tick == []


class _RefusedRelease:
    """The input watch port, whose every release is refused."""

    def __init__(self, inner, tries):
        self._inner = inner
        self._tries = tries

    def __getattr__(self, name):
        return getattr(self._inner, name)

    def acknowledge(self, task_id, identity):
        self._tries.append(("release", identity.node_attempt_id))
        raise RuntimeError("release refused")

    cancel = acknowledge


@pytest.mark.parametrize("sent", [False, True])
def test_a_refused_watch_release_is_tried_at_bases_points(sent):
    """Base tried the release when the watch ended, again before the node
    failed, and once more at the node's end: three tries, the same failure,
    with or without the run's sender."""

    tries = []
    if sent:
        node, events, _levels, _guard, sender, _now = _sent_node(required=("part",))
        node._io_watches = _RefusedRelease(node._io_watches, tries)
        _sent_ticks_until(node, sender, lambda: node.status is not py_trees.common.Status.RUNNING)
    else:
        node, events, _levels, _now = _node(required=("part",))
        node._io_watches = _RefusedRelease(node._io_watches, tries)
        _tick_until(node, lambda: node.status is not py_trees.common.Status.RUNNING)
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "owner_request_failed"
    assert "release was not confirmed" in node.failure_message
    assert tries == [("release", 1)] * 3
    assert not any(event[0] == "logical" for event in events)


@pytest.mark.parametrize("grasped", [True, False])
def test_a_stop_while_the_grasp_read_is_sent_ends_with_its_result(grasped):
    node, events, levels, guard, sender, _now = _sent_node(required=(), check=_PART_CHECK)
    levels["part"] = grasped
    _sent_ticks_until(node, sender, lambda: ("ack", "gripper") in events)
    node.tick_once()  # The completed Close's grasp read is on the sender.
    assert node.status is py_trees.common.Status.RUNNING
    node.stop(py_trees.common.Status.INVALID)  # The Program Stop.
    sender.sender.close()
    assert node.outcome is None and node.settle_abandoned_request() is False
    sender.drain()
    assert node.settle_abandoned_request() is True
    # As base, whose tick read it right after the acknowledgement.
    assert events.count(("read", "part")) == 1
    if grasped:
        assert node.status is py_trees.common.Status.SUCCESS and node.outcome == "success"
    else:
        assert node.status is py_trees.common.Status.FAILURE
        assert node.failure_reason_code == "grasp_not_confirmed"
