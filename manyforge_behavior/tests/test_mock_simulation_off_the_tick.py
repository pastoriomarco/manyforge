"""The run's Mock ENDs and Halt states and its operator Stop scope reads
leave the tick on the run's sender (OD-380 D2, OD-213).

The real native port over the gateway fake; the run's sender is emptied
only when a test says so, on a thread of its own. Every owner call records
the thread it was made on: none may be the tick's (the test's) thread.
"""

from __future__ import annotations

import threading
from types import SimpleNamespace
from typing import Any, Callable

import py_trees

from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter
from manyforge_behavior.orchestration_ports import NodeAttemptIdentity, OwnerInvocationHeldError
from manyforge_behavior.program_runtime import ExecutableProgramRun
from manyforge_behavior.run_sender import RunSender

from .gateway_fakes import transport_gateway
from .test_mock_simulation_runner import (
    _END,
    _LOST,
    _ROWS,
    _Host,
    _ReadPort,
    _Transport,
    _common,
    _native,
)

_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS


class _Sender:
    """The run's sender, emptied on a thread of its own when the test says."""

    def __init__(self) -> None:
        self.pending: list[Callable[[], None]] = []
        self.sender = RunSender(8, spawn=lambda _name, work: self.pending.append(work))

    def drain(self) -> None:
        while self.pending:
            worker = threading.Thread(target=self.pending.pop(0))
            worker.start()
            worker.join()


class _Calls:
    """The thread of every owner call, by command."""

    def __init__(self) -> None:
        self.tick_thread = threading.get_ident()
        self.calls: list[tuple[str, int]] = []

    def record(self, name: str) -> None:
        self.calls.append((name, threading.get_ident()))

    def on_tick(self) -> list[str]:
        return [name for name, thread in self.calls if thread == self.tick_thread]


def _guarded_native(monkeypatch, tmp_path):
    port, host = _native(monkeypatch, tmp_path)
    calls = _Calls()
    answer = host._answer

    def recorded(command):
        calls.record(command["command"])
        return answer(command)

    class _Scope(_Transport):
        def read_program_execution_scope(self, program_run_id: int) -> dict[str, Any]:
            calls.record("read_program_execution_scope")
            return super().read_program_execution_scope(program_run_id)

    host._answer = recorded
    host._other = transport_gateway(_Scope())
    return port, host, calls


def _ends(host: _Host) -> list[dict[str, Any]]:
    return [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]


def _pauses(host: _Host) -> list[tuple[str, bool]]:
    return [(envelope["request"]["run_revision"], envelope["request"]["paused"])
            for envelope in host.envelopes
            if envelope["command"] == "set_mock_io_run_paused"]


def _run(mock, sender: _Sender, root=None):
    """The run's own tick, Stop and Mock settlement over ``root``."""

    events: list[dict[str, Any]] = []
    run = ExecutableProgramRun.__new__(ExecutableProgramRun)
    run._ports = SimpleNamespace(mock_simulation=mock)
    run._capture = SimpleNamespace(program_run_id=1)
    run._mock_ends_reported = set()
    run._record_node_history_event = events.append
    run._sender = sender.sender
    run._sender_open = True
    run._mock_settlements = []
    run._mock_ending = False
    run._mock_final_settled = False
    run._stop_settlement_read = None
    run._stopped = False
    run._task_adapters = {}
    run._sender_adapters = []
    run._attempt_history = None
    run._end_settling = False
    run._shown_node_statuses = None
    run._shown_wait_observations = None
    run._node_history_events = []
    run._wait_adapters = {}
    run._authored_nodes = {} if root is None else {root.name: root}
    if root is not None:
        run._tree = py_trees.trees.BehaviourTree(root=root)
    return run, events


def _read_node(mock, sender: _Sender, reads: list):
    common = _common("io.get_input", None, [], _ROWS[:1])
    common.update(
        node_instance_id="search_trip",
        attempt_source=SimpleNamespace(begin=lambda instance: NodeAttemptIdentity(1, instance, 1)),
        mock_simulation=mock,
    )
    return OwnerInvocationAdapter(**common, port=_ReadPort(reads), run_sender=sender.sender)


def _acknowledged(monkeypatch, tmp_path):
    """A run whose one read node's Mock START is acknowledged."""

    port, host, calls = _guarded_native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.record_run_paused(1, 1, False)  # Run start: recorded only.
    sender, reads = _Sender(), []
    node = _read_node(mock, sender, reads)
    run, events = _run(mock, sender, node)
    assert run.tick_once() is _RUNNING  # The START request is built off the tick.
    sender.drain()
    assert run.tick_once() is _RUNNING  # The START is queued.
    sender.drain()
    assert host.commands() == ["set_mock_io_run_paused", "start_mock_io_node_simulation"]
    assert run.tick_once() is _RUNNING  # Acknowledged: the read is queued.
    return host, calls, sender, run, events, reads


def test_the_end_of_a_stopped_node_is_sent_once_off_the_tick(monkeypatch, tmp_path) -> None:
    host, calls, sender, run, events, reads = _acknowledged(monkeypatch, tmp_path)
    run.stop()  # Program Stop: the tree is invalidated, the sender closed.
    assert _ends(host) == []  # Recorded only: nothing is sent on the tick.
    assert run.settle_sent_requests() is False  # Its final settlement is queued.
    sender.drain()  # Sent after the close: never withdrawn.
    assert run.settle_sent_requests() is True
    assert _ends(host) == [_END]  # Exactly once.
    assert reads == []  # The queued read was withdrawn by the Stop.
    assert events == []
    assert calls.on_tick() == []


def test_a_run_ends_once_its_pending_end_was_sent_and_reports_it_unanswered(
    monkeypatch, tmp_path
) -> None:
    host, calls, sender, run, events, reads = _acknowledged(monkeypatch, tmp_path)
    host.script["end_mock_io_node_simulation"] = [_LOST, _LOST]
    sender.drain()  # The read.
    assert run.tick_once() is _RUNNING  # The tree ended; its END is queued.
    assert run.status is _SUCCESS and _ends(host) == []
    sender.drain()  # The node's END (lost), then the run's final resend (lost).
    assert run.tick_once() is _SUCCESS
    assert events == [{
        "kind": "program_node_mock_simulation_end_unconfirmed",
        "node_instance_id": "search_trip",
        "node_attempt_id": 1,
        "node_start_index": "1",
    }]
    assert run.tick_once() is _SUCCESS and sender.pending == []  # Nothing more.
    assert _ends(host) == [_END, _END]
    assert reads == [("act", 1)] and calls.on_tick() == []


def test_an_answered_end_ends_the_run_on_the_next_tick(monkeypatch, tmp_path) -> None:
    host, calls, sender, run, events, _reads = _acknowledged(monkeypatch, tmp_path)
    sender.drain()  # The read.
    assert run.tick_once() is _RUNNING  # The END is queued.
    sender.drain()
    assert run.tick_once() is _SUCCESS
    assert _ends(host) == [_END] and events == [] and calls.on_tick() == []


def test_an_ended_tree_shows_nothing_until_its_end_is_answered(monkeypatch, tmp_path) -> None:
    """Base sent the END inside the tick that ended the tree: the ended
    tree's node statuses and history are shown with the run's end."""

    host, calls, sender, run, events, _reads = _acknowledged(monkeypatch, tmp_path)
    shown = run.node_statuses
    assert dict(shown) == {"node": "running"}
    sender.drain()  # The read.
    assert run.tick_once() is _RUNNING  # The tree ended; its END is queued.
    run._node_history_events.append({"kind": "program_node_preempted"})
    assert run.node_statuses is shown and run.take_node_history_events() == ()
    sender.drain()
    assert run.tick_once() is _SUCCESS
    assert dict(run.node_statuses) == {"node": "success"}
    assert run.take_node_history_events() == ({"kind": "program_node_preempted"},)
    assert _ends(host) == [_END] and calls.on_tick() == []


def test_a_halt_state_is_sent_off_the_tick_across_the_halts_withdrawal(
    monkeypatch, tmp_path
) -> None:
    host, calls, sender, run, events, _reads = _acknowledged(monkeypatch, tmp_path)
    mock = run._ports.mock_simulation
    # Entering Halting: queued requests are withdrawn, the state recorded only.
    run.withdraw_unsent_requests()
    assert mock.record_run_paused(1, 2, True) is True
    run.settle_mock_pause()  # The Halting tick: queued on the run's sender.
    run.withdraw_unsent_requests()  # Not withdrawable.
    assert _pauses(host) == [("1", False)]
    sender.drain()
    assert _pauses(host) == [("1", False), ("2", True)]
    run.settle_mock_pause()
    assert sender.pending == []  # Answered: nothing more to send.
    # Execute: the resume's reply is lost; a newer Halt supersedes it.
    host.script["set_mock_io_run_paused"] = [_LOST]
    mock.record_run_paused(1, 3, False)
    run.settle_mock_pause()
    sender.drain()
    mock.record_run_paused(1, 4, True)
    run.settle_mock_pause()
    sender.drain()
    run.settle_mock_pause()
    assert _pauses(host) == [("1", False), ("2", True), ("3", False), ("4", True)]
    assert events == [{"kind": "program_mock_simulation_pause_unconfirmed",
                       "paused": False, "run_revision": "3", "held_node_instance_ids": []}]
    assert calls.on_tick() == []


# --- Operator Stop scope reads -----------------------------------------------


class _ScopeReads:
    def __init__(self, answers: list[Any]) -> None:
        self.answers = answers
        self.threads: list[int] = []

    def __call__(self) -> OperatorMotionStopSnapshot:
        self.threads.append(threading.get_ident())
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer


def test_a_pending_stop_settlement_reads_the_scope_off_the_tick() -> None:
    tick = threading.get_ident()
    reads = _ScopeReads([
        RuntimeError("scope unavailable"),
        OperatorMotionStopSnapshot(1, frozenset({"a"})),
    ])
    state = OperatorMotionStopState({"a": ()}, read_scope=reads)
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a"}), frozenset({"a"})))
    sender = _Sender()
    run, _events = _run(None, sender)
    run._operator_stop_state = state
    run.refresh_pending_stop_settlement()
    run.refresh_pending_stop_settlement()  # One read at a time.
    assert reads.threads == [] and len(sender.pending) == 1
    sender.drain()  # Failed: an observation outage, the holds stay.
    assert state.observation_error() is not None
    assert state.snapshot().stopping_nodes == {"a"}
    run.refresh_pending_stop_settlement()  # A later tick reads again.
    sender.drain()
    assert state.observation_error() is None
    assert state.snapshot().stopping_nodes == frozenset()
    run.refresh_pending_stop_settlement()  # Settled: no more reads.
    assert sender.pending == []
    assert len(reads.threads) == 2 and tick not in reads.threads


def test_a_hold_before_admission_reads_the_scope_off_the_tick_and_keeps_waiting() -> None:
    tick = threading.get_ident()
    reads = _ScopeReads([OperatorMotionStopSnapshot(2, frozenset({"node"}))])
    state = OperatorMotionStopState({"node": ()}, read_scope=reads)
    state.publish(OperatorMotionStopSnapshot(1))
    sender = _Sender()
    common = _common("io.get_input", None, [], None)
    node = OwnerInvocationAdapter(
        **common, port=_ReadPort([]), operator_stop_state=state, run_sender=sender.sender
    )
    node._hold_before_admission(OwnerInvocationHeldError(2))
    node._hold_before_admission(OwnerInvocationHeldError(2))  # One read queued.
    assert reads.threads == [] and len(sender.pending) == 1
    assert node._wait_for_admission_release() is True  # The exact fence holds.
    sender.drain()
    assert state.snapshot().held_nodes == {"node"}
    assert node._wait_for_admission_release() is True  # Held: still waiting.
    assert len(reads.threads) == 1 and tick not in reads.threads
