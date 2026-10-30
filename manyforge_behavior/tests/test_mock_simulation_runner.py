"""Node Mock simulation (decision 133, slice S2): the Runner's node-start hook,
its port and its private wire command.

Fakes only: the action ports, the Mock simulation port and the native
command socket.
"""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Mapping

import py_trees
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.nodes.v2_owner import (
    InterruptMotionTaskAdapter,
    IoInputWaitAdapter,
    OwnerInvocationAdapter,
    OwnerTaskAdapter,
)
from manyforge_behavior.orchestration_ports import (
    MockSimulationRefusedError,
    NodeAttemptIdentity,
    OwnerInvocationHeldError,
    OwnerInvocationResult,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_behavior.production_ports import NativeKernelProgramPort
from manyforge_behavior.program_clock import ProgramClock

_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_FAILURE = py_trees.common.Status.FAILURE
_INVALID = py_trees.common.Status.INVALID
_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64

_ROWS = [
    {"signal": {"target_role": "arm", "resource_role": "ready"}, "start_value": False,
     "change": {"after_ms": 100, "value": True}},
    {"signal": {"target_role": "arm", "resource_role": "door"}, "start_value": True},
]


class _Lost(RuntimeError):
    """An unknown reply: transport loss (the existing failure code)."""

    code = "execution_owner_unavailable"


class _MockPort:
    """Scripted MockSimulationPort: replies are 'ack', an exception or a
    callable; every send and the shared action log are recorded."""

    def __init__(self, log: list[tuple[str, ...]], replies: list[Any] | None = None) -> None:
        self.log = log
        self.replies = list(replies or [])
        self.built: list[tuple[str, int]] = []
        self.sent: list[dict[str, Any]] = []

    def request(self, identity, node_start_index, signals):
        self.built.append((identity.node_attempt_id, node_start_index))
        return {
            "node_attempt_id": identity.node_attempt_id,
            "node_start_index": str(node_start_index),
            "signals": [dict(row) for row in signals],
        }

    def end(self, start_request) -> None:
        self.log.append(("end", start_request["node_attempt_id"], start_request["node_start_index"]))

    def start(self, request):
        self.sent.append(deepcopy(dict(request)))
        self.log.append(("mock", request["node_attempt_id"], request["node_start_index"]))
        reply = self.replies.pop(0) if self.replies else "ack"
        if isinstance(reply, BaseException):
            raise reply
        return {
            "status": "accepted",
            "node_start_index": request["node_start_index"],
            "started_at_ns": "1000000",
            "signals": [
                {"signal": {"target_role": "arm", "resource_role": "door"}, "status": "inactive"},
                {"signal": {"target_role": "arm", "resource_role": "ready"}, "status": "applied"},
            ],
        }


class _Attempts:
    def __init__(self) -> None:
        self.count = 0

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        self.count += 1
        return NodeAttemptIdentity("run.1", node_instance_id, f"attempt.{self.count}")


class _Store:
    def write_node_outcome(self, *_args, **_kwargs) -> None:
        pass


def _resolver() -> SimpleNamespace:
    return SimpleNamespace(
        snapshot=lambda *, parameters, **_: SimpleNamespace(parameters=parameters, blackboard={}),
        value_store=_Store(),
    )


def _common(node_id: str, mock: _MockPort | None, events: list, rows=_ROWS) -> dict[str, Any]:
    parameters: dict[str, Any] = {"target_role": "arm", "resource_role": "ready"}
    if rows is not None:
        parameters["mock_simulation"] = rows
    return dict(
        name="node",
        node_id=node_id,
        node_instance_id="node",
        attempt_source=_Attempts(),
        parameters=parameters,
        value_sources={},
        input_resolver=_resolver(),
        mock_simulation=mock,
        audit_sink=events.append,
    )


class _TaskPort:
    """Kernel-operations or watch port: one ACTIVE task per start."""

    def __init__(self, log: list[tuple[str, ...]]) -> None:
        self.log = log
        self.state = OwnerTaskState.ACTIVE
        self.start_errors: list[BaseException] = []
        self.metadata: dict[str, Any] = {}
        self.starts = 0

    def _snapshot(self, task_id: str) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(
            task_id, self.state, metadata=dict(self.metadata), reason_code=(
                "planning_failed" if self.state is OwnerTaskState.FAILED else None
            ),
        )

    def start(self, node_id, identity, inputs):
        self.log.append(("act", identity.node_attempt_id))
        if self.start_errors:
            raise self.start_errors.pop(0)
        self.starts += 1
        return self._snapshot(f"task.{self.starts}")

    def observe(self, task_id):
        return self._snapshot(task_id)

    def cancel(self, task_id, identity) -> None:
        pass

    def acknowledge(self, task_id, identity) -> None:
        pass

    def close(self, task_id, identity, *, request_id) -> bool:
        return True

    def release_preempted(self, task_id, identity) -> None:
        pass


class _ReadPort:
    def __init__(self, log: list[tuple[str, ...]]) -> None:
        self.log = log

    def invoke(self, node_id, identity, inputs):
        self.log.append(("act", identity.node_attempt_id))
        return OwnerInvocationResult(True, output=True)


def _clock() -> ProgramClock:
    return ProgramClock(lambda: 0.0)


def _motion(mock, events, log, rows=_ROWS):
    port = _TaskPort(log)
    node = InterruptMotionTaskAdapter(
        **_common("motion.move_until_signal", mock, events, rows),
        port=port,
        owner_port_name="kernel_operations",
        program_clock=_clock(),
        motion_attention=lambda *_: None,
        owner_api="kernel.operation",
        cancellable=True,
    )
    return node, port


def _wait(mock, events, log, rows=_ROWS):
    port = _TaskPort(log)
    node = IoInputWaitAdapter(
        **_common("io.wait_for_input", mock, events, rows), port=port, program_clock=_clock()
    )
    return node, port


def _read(mock, events, log, rows=_ROWS):
    port = _ReadPort(log)
    node = OwnerInvocationAdapter(**_common("io.get_input", mock, events, rows), port=port)
    return node, port


_ADAPTERS = {"interrupt_motion": _motion, "io_wait": _wait, "io_read": _read}


def _tick(node) -> py_trees.common.Status:
    node.tick_once()
    return node.status


def _run(node, ticks: int = 4) -> py_trees.common.Status:
    """Tick within one entry: stop at the node's first result."""

    for _ in range(ticks):
        if _tick(node) is not _RUNNING:
            break
    return node.status


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_one_send_per_attempt_before_the_action(kind: str) -> None:
    log: list[tuple[str, ...]] = []
    events: list[dict[str, Any]] = []
    mock = _MockPort(log)
    node, _port = _ADAPTERS[kind](mock, events, log)
    _run(node)
    assert log[0] == ("mock", "attempt.1", "1")
    assert [entry for entry in log if entry[0] == "mock"] == [("mock", "attempt.1", "1")]
    assert ("act", "attempt.1") in log
    assert mock.built == [("attempt.1", 1)]
    assert [event for event in events if event["kind"] == "program_node_mock_simulation"] == [{
        "kind": "program_node_mock_simulation",
        "node_instance_id": "node",
        "node_attempt_id": "attempt.1",
        "node_start_index": "1",
        "started_at_ns": "1000000",
        "applied": [{"target_role": "arm", "resource_role": "ready"}],
        "inactive": [{"target_role": "arm", "resource_role": "door"}],
    }]


def test_an_instant_read_invokes_on_the_same_tick_after_the_ack() -> None:
    log: list[tuple[str, ...]] = []
    node, _port = _read(_MockPort(log), [], log)
    assert _tick(node) is _SUCCESS
    assert log == [("mock", "attempt.1", "1"), ("act", "attempt.1"), ("end", "attempt.1", "1")]


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_an_empty_or_absent_mock_simulation_sends_nothing(kind: str) -> None:
    for rows in (None, []):
        log: list[tuple[str, ...]] = []
        mock = _MockPort(log)
        node, _port = _ADAPTERS[kind](mock, [], log, rows)
        _tick(node)
        node.stop(_INVALID)
        assert mock.sent == [] and mock.built == []
        assert log == [("act", "attempt.1")]  # No start and no end.


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_a_lost_ack_resends_the_identical_request_without_acting(kind: str) -> None:
    log: list[tuple[str, ...]] = []
    events: list[dict[str, Any]] = []
    mock = _MockPort(log, [_Lost("socket closed"), TimeoutError("late"), "ack"])
    node, _port = _ADAPTERS[kind](mock, events, log)
    assert _tick(node) is _RUNNING
    assert node.feedback_message == "Waiting for Mock simulation acknowledgement"
    assert _tick(node) is _RUNNING
    assert not any(entry[0] == "act" for entry in log)
    _tick(node)
    # Same request three times: no new index, no new attempt.
    assert mock.sent == [mock.sent[0]] * 3
    assert mock.built == [("attempt.1", 1)]
    assert log[:4] == [("mock", "attempt.1", "1")] * 3 + [("act", "attempt.1")]
    assert log.count(("end", "attempt.1", "1")) <= 1
    assert node.attempt_identity.node_attempt_id == "attempt.1"
    assert len([event for event in events if event["kind"] == "program_node_mock_simulation"]) == 1


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_a_closed_refusal_fails_the_node_without_acting(kind: str) -> None:
    log: list[tuple[str, ...]] = []
    refusal = MockSimulationRefusedError(
        "mock_simulation_stale",
        "Node Mock simulation start conflicts with its current attempt",
        {"reason": "mock_simulation_stale"},
    )
    mock = _MockPort(log, [refusal])
    node, _port = _ADAPTERS[kind](mock, [], log)
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "mock_simulation_refused"
    assert node.failure_details == {"reason": "mock_simulation_stale"}
    assert "mock_simulation_stale" in node.failure_message
    assert [entry for entry in log if entry[0] != "end"] == [("mock", "attempt.1", "1")]


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_re_entry_takes_a_new_index(kind: str) -> None:
    log: list[tuple[str, ...]] = []
    mock = _MockPort(log)
    node, _port = _ADAPTERS[kind](mock, [], log)
    _tick(node)
    node.stop(_INVALID)  # The parent ends the node (or retry_v1 re-enters it).
    _run(node)
    assert mock.built == [("attempt.1", 1), ("attempt.2", 2)]
    assert [entry for entry in log if entry[0] != "act"][:3] == [
        ("mock", "attempt.1", "1"), ("end", "attempt.1", "1"), ("mock", "attempt.2", "2")
    ]


def test_parent_ending_a_waiting_node_stops_resends() -> None:
    log: list[tuple[str, ...]] = []
    mock = _MockPort(log, [_Lost("lost")] * 5)
    node, _port = _motion(mock, [], log)
    assert _tick(node) is _RUNNING
    node.stop(_INVALID)
    # A halted or stopped tree does not tick the node: nothing more is sent.
    assert len(mock.sent) == 1
    assert log[-1] == ("end", "attempt.1", "1")


@pytest.mark.parametrize("kind", sorted(_ADAPTERS))
def test_end_is_sent_once_when_the_attempt_ends(kind: str) -> None:
    log: list[tuple[str, ...]] = []
    node, _port = _ADAPTERS[kind](_MockPort(log), [], log)
    _run(node, ticks=2)
    # Running (or halted: not ticked) is no end.
    if node.status is _RUNNING:
        assert not any(entry[0] == "end" for entry in log)
    node.stop(_INVALID)  # Parent end, preemption or Program Stop/Reset.
    node.stop(_INVALID)
    assert log.count(("end", "attempt.1", "1")) == 1
    assert log[-1] == ("end", "attempt.1", "1")


def test_a_failed_attempt_ends_once() -> None:
    log: list[tuple[str, ...]] = []
    node, port = _wait(_MockPort(log), [], log)
    port.state = OwnerTaskState.FAILED
    assert _tick(node) is _FAILURE
    node.stop(_INVALID)
    assert log == [("mock", "attempt.1", "1"), ("act", "attempt.1"), ("end", "attempt.1", "1")]


class _Interventions:
    def __init__(self) -> None:
        self.resolution = None

    def require(self, identity, **_kwargs) -> None:
        pass

    def consume_resolution(self, identity):
        resolution, self.resolution = self.resolution, None
        return resolution


def test_operator_retry_takes_a_new_index() -> None:
    log: list[tuple[str, ...]] = []
    mock = _MockPort(log)
    port = _TaskPort(log)
    port.state = OwnerTaskState.FAILED
    port.metadata = {
        "authored_failure_policy": "pause_for_operator",
        "authored_failure_policy_parameter": "no_feasible_plan_policy",
    }
    interventions = _Interventions()
    node = OwnerTaskAdapter(
        **_common("motion.move_until_signal", mock, [], _ROWS),
        port=port,
        owner_port_name="kernel_operations",
        intervention_port=interventions,
    )
    assert _tick(node) is _RUNNING  # Held for the operator.
    assert _tick(node) is _RUNNING
    interventions.resolution = ("retry", None)
    assert _tick(node) is _RUNNING  # Retry: a fresh attempt.
    port.state = OwnerTaskState.ACTIVE
    _tick(node)
    assert mock.built == [("attempt.1", 1), ("attempt.2", 2)]
    assert log == [
        ("mock", "attempt.1", "1"), ("act", "attempt.1"), ("end", "attempt.1", "1"),
        ("mock", "attempt.2", "2"), ("act", "attempt.2"),
    ]


class _StopState:
    def __init__(self) -> None:
        self.revision = 1

    def snapshot(self):
        return SimpleNamespace(selection_revision=self.revision, held_nodes=frozenset())

    def observation_error(self):
        return None

    def refresh(self) -> None:
        pass


def test_hold_before_admission_release_takes_a_new_index() -> None:
    log: list[tuple[str, ...]] = []
    mock = _MockPort(log)
    port = _TaskPort(log)
    port.start_errors = [OwnerInvocationHeldError(1)]
    stop = _StopState()
    node = OwnerTaskAdapter(
        **_common("motion.move_until_signal", mock, [], _ROWS),
        port=port,
        owner_port_name="kernel_operations",
        operator_stop_state=stop,
    )
    assert _tick(node) is _RUNNING
    assert node.presented_held
    assert _tick(node) is _RUNNING  # Still held: nothing is sent.
    assert len(mock.sent) == 1
    stop.revision = 2  # The owner released the hold.
    _tick(node)
    assert mock.built == [("attempt.1", 1), ("attempt.2", 2)]
    assert log == [
        ("mock", "attempt.1", "1"), ("act", "attempt.1"), ("end", "attempt.1", "1"),
        ("mock", "attempt.2", "2"), ("act", "attempt.2"),
    ]


def test_interrupt_motion_has_no_automatic_retry_and_no_double_send() -> None:
    log: list[tuple[str, ...]] = []
    mock = _MockPort(log)
    node, port = _motion(mock, [], log)
    port.state = OwnerTaskState.FAILED
    port.metadata = {
        "operation_id": "op",
        "recovery_assessment": {
            "recovery_class": "planning_failed",
            "status": "retry_ready",
            "execution_state": "Failed",
            "allowed_actions": [],
            "eligible_close": True,
            "retry_eligibility": {
                "eligible": True, "cause": "planning_no_effect",
                "effect": "known_no_effect", "reason": None,
            },
        },
    }
    assert _run(node) is _FAILURE
    assert port.starts == 1
    assert len(mock.sent) == 1


def test_a_missing_port_fails_a_node_with_authored_rows() -> None:
    log: list[tuple[str, ...]] = []
    node, _port = _read(None, [], log)
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "mock_simulation_refused"
    assert log == []


# --- Native port and wire ---------------------------------------------------

_FIXTURES = Path(__file__).resolve().parent / "fixtures" / "mock_simulation"


def _fixture(name: str) -> dict[str, Any]:
    """Track 2's frozen examples, unchanged (simulation133 interface01,
    node-end135, run-pause135 interface01; digest-corrected)."""

    return json.loads((_FIXTURES / name).read_text(encoding="utf-8"))


_START = _fixture("start.request.example.json")
_ACK = _fixture("start.ack.example.json")
_ERROR = _fixture("error.example.json")
_PAUSE = _fixture("run_pause.request.example.json")
_PAUSE_ACK = _fixture("run_pause.ack.example.json")
_END_EXAMPLE = _fixture("end.request.example.json")
_END_ACK = _fixture("end.ack.example.json")
_BINDING = _START["request"]["program_binding_id"]
_OCCURRENCE = (
    "owner_epoch", "program_binding_id", "program_runner_owner_epoch", "claim_generation",
    "program_run_id", "node_instance_id", "node_attempt_id", "node_start_index",
)
_END = _END_EXAMPLE["request"]
_RESPONSE = "manyforge.execution_command_response.v1"
_LOST = "lost"


class _Host:
    """The native command socket: scripted replies per command (a dict, or
    _LOST for an unknown reply); otherwise the exact success echo."""

    def __init__(self) -> None:
        self.script: dict[str, list[Any]] = {}
        self.envelopes: list[dict[str, Any]] = []

    def __call__(self, command, _socket_path):
        self.envelopes.append(deepcopy(command))
        scripted = self.script.get(command["command"])
        reply = scripted.pop(0) if scripted else None
        if callable(reply):  # Something happens while this command is in flight.
            reply = reply()
        if reply == _LOST:
            raise ConnectionError("reply lost")
        if reply is not None:
            return deepcopy(reply)
        request = command["request"]
        echo = {key: value for key, value in request.items()
                if key not in {"schema_version", "signals"}}
        ack = {"api_version": _RESPONSE, "command": command["command"], "ok": True, **echo}
        if command["command"] == "start_mock_io_node_simulation":
            ack.update(status="accepted", started_at_ns="1000000", signals=[
                {"signal": row["signal"], "status": "applied"} for row in request["signals"]
            ])
        return ack

    def commands(self) -> list[str]:
        return [envelope["command"] for envelope in self.envelopes]


def _context() -> ProgramRunPortContext:
    target = {
        "target_role_id": "arm",
        "component_role_id": "io_module",
        "controllable_component_id": "io.1",
        "command_target_id": "io.command",
        "capability_bindings": [],
        "resource_role_bindings": [],
    }
    return ProgramRunPortContext(
        program_run_id="run.1", applied_record_digest=_D, scenario_id="s",
        source_document_digest=_A, program_id="p", program_revision=_A,
        program_binding_id=_BINDING, retained_bundle_id=_B, graph_revision=_C,
        node_environment_digest=_D,
        program_binding={
            "program_binding_id": _BINDING, "program_id": "p", "program_revision": _A,
            "retained_bundle_id": _B, "graph_revision": _C,
            "target_role_bindings": [target],
        },
        program={},
    )


class _Transport:
    """The Runner transport's Mock commands over the real wire models."""

    def __init__(self, wire) -> None:
        self.start_mock_io_node_simulation = wire.start_mock_io_node_simulation
        self.end_mock_io_node_simulation = wire.end_mock_io_node_simulation
        self.set_mock_io_run_paused = wire.set_mock_io_run_paused

    def submit(self, body):  # pragma: no cover - never used here
        raise AssertionError("Mock simulation never submits an operation")

    read = reset = acknowledge_completed = stop = submit

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {
            "owner_epoch": "kernel.owner", "graph_revision": _C, "retained_bundle_id": _B,
            "scene_revision": {"scene_id": _D, "revision_id": 1, "content_hash": _D},
        }}

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        return {"ok": True, "scope": {
            "schema_version": "manyforge.program_execution_scope.v2",
            "program_run_id": program_run_id, "request_owner_epoch": "kernel.owner",
            "program_runner_owner_epoch": "runner.owner", "claim_generation": 1,
            "selection_revision": 0,
        }}


def _wire(monkeypatch, tmp_path):
    from manyforge_headless_api.execution import ExecutionCommandClient
    from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport

    host = _Host()
    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", host)
    return ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0), host


def _native(monkeypatch, tmp_path):
    wire, host = _wire(monkeypatch, tmp_path)
    port = NativeKernelProgramPort(KernelClient(_Transport(wire)))
    port.bind(_context())
    return port, host


_IDENTITY = NodeAttemptIdentity("run.1", "search_trip", "attempt:opaque")


def test_start_wire_round_trips_the_frozen_examples(monkeypatch, tmp_path) -> None:
    wire, host = _wire(monkeypatch, tmp_path)
    host.script["start_mock_io_node_simulation"] = [_ACK]
    ack = wire.start_mock_io_node_simulation(deepcopy(_START["request"]))
    assert host.envelopes == [_START]
    assert ack == _ACK and len(ack) == 14


def test_run_pause_wire_round_trips_the_frozen_examples(monkeypatch, tmp_path) -> None:
    wire, host = _wire(monkeypatch, tmp_path)
    host.script["set_mock_io_run_paused"] = [_PAUSE_ACK]
    ack = wire.set_mock_io_run_paused(deepcopy(_PAUSE["request"]))
    assert host.envelopes == [_PAUSE]
    assert len(_PAUSE["request"]) == 9
    assert ack == _PAUSE_ACK and len(ack) == 11


def test_end_wire_is_the_start_occurrence_with_its_schema(monkeypatch, tmp_path) -> None:
    wire, host = _wire(monkeypatch, tmp_path)
    host.script["end_mock_io_node_simulation"] = [_END_ACK]
    ack = wire.end_mock_io_node_simulation(deepcopy(_END))
    assert host.envelopes == [_END_EXAMPLE]
    assert _END == {"schema_version": "manyforge.mock_io_node_end.v1",
                    **{name: _START["request"][name] for name in _OCCURRENCE}}
    assert ack == _END_ACK and len(ack) == 11


def test_wire_error_keeps_its_code_and_details(monkeypatch, tmp_path) -> None:
    from manyforge_headless_api.native import HeadlessApiError

    wire, host = _wire(monkeypatch, tmp_path)
    host.script["start_mock_io_node_simulation"] = [_ERROR]
    with pytest.raises(HeadlessApiError) as caught:
        wire.start_mock_io_node_simulation(deepcopy(_START["request"]))
    assert caught.value.code == "mock_simulation_stale"
    assert caught.value.details == {"reason": "mock_simulation_stale"}


@pytest.mark.parametrize(
    "mutation",
    [
        {"node_start_index": 1},  # Not decimal text.
        {"node_start_index": "0"},  # Not positive.
        {"node_start_index": "01"},  # Not canonical.
        {"node_start_index": str(2**64)},  # Beyond uint64.
        {"signals": [{"signal": {"target_role": "arm", "resource_role": "ready"},
                      "start_value": False}]},  # change must be explicit.
        {"signals": []},
    ],
)
def test_start_wire_request_is_strict(monkeypatch, tmp_path, mutation) -> None:
    from manyforge_headless_api.native import HeadlessApiError

    wire, host = _wire(monkeypatch, tmp_path)
    with pytest.raises(HeadlessApiError) as caught:
        wire.start_mock_io_node_simulation({**_START["request"], **mutation})
    assert caught.value.code == "execution_command_invalid"
    assert host.envelopes == []


@pytest.mark.parametrize(
    "mutation", [{"run_revision": 1}, {"run_revision": "0"}, {"paused": 0},
                 {"run_revision": str(2**64)}, {"held_node_instance_ids": ["b", "a"]},
                 {"held_node_instance_ids": ["a", "a"]}]
)
def test_run_pause_wire_request_is_strict(monkeypatch, tmp_path, mutation) -> None:
    from manyforge_headless_api.native import HeadlessApiError

    wire, host = _wire(monkeypatch, tmp_path)
    with pytest.raises(HeadlessApiError):
        wire.set_mock_io_run_paused({**_PAUSE["request"], **mutation})
    assert host.envelopes == []


def test_wire_accepts_the_full_uint64_index(monkeypatch, tmp_path) -> None:
    top = str(2**64 - 1)
    wire, host = _wire(monkeypatch, tmp_path)
    wire.start_mock_io_node_simulation({**_START["request"], "node_start_index": top})
    assert host.envelopes[0]["request"]["node_start_index"] == top


@pytest.mark.parametrize(
    "mutation",
    [
        {"command": "end_mock_io_node_simulation"},
        {"started_at_ns": 1000000},
        {"status": "superseded"},
        {"result": {}},
    ],
)
def test_wire_refuses_a_malformed_start_ack(monkeypatch, tmp_path, mutation) -> None:
    from manyforge_headless_api.native import HeadlessApiError

    wire, host = _wire(monkeypatch, tmp_path)
    host.script["start_mock_io_node_simulation"] = [{**_ACK, **mutation}]
    with pytest.raises(HeadlessApiError) as caught:
        wire.start_mock_io_node_simulation(deepcopy(_START["request"]))
    assert caught.value.code == "execution_command_response_invalid"


def test_native_port_builds_the_canonical_request_with_a_decimal_index(
    monkeypatch, tmp_path
) -> None:
    port, _ = _native(monkeypatch, tmp_path)
    request = port.mock_simulation.request(_IDENTITY, 2**64 - 1, _ROWS)
    assert request == {
        **{key: _START["request"][key] for key in _OCCURRENCE},
        "schema_version": "manyforge.mock_io_node_start.v1",
        "node_start_index": "18446744073709551615",
        # Canonical selector order; an omitted change is an explicit null.
        "signals": [
            {"signal": {"target_role": "arm", "resource_role": "door"},
             "start_value": True, "change": None},
            {"signal": {"target_role": "arm", "resource_role": "ready"},
             "start_value": False, "change": {"after_ms": 100, "value": True}},
        ],
    }
    with pytest.raises(Exception):
        port.mock_simulation.request(_IDENTITY, 0, _ROWS)


def test_a_start_without_the_runs_pause_state_is_refused_unsent(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    request = port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1])
    with pytest.raises(MockSimulationRefusedError) as caught:
        port.mock_simulation.start(request)
    assert caught.value.code == "mock_run_pause_unknown"
    assert host.envelopes == []


def test_the_initial_pause_state_precedes_the_first_start(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    request = port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1])
    assert port.mock_simulation.start(request)["status"] == "accepted"
    assert host.envelopes == [_PAUSE, _START]


def test_a_lost_pause_reply_is_resent_identically_before_the_start(
    monkeypatch, tmp_path
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    host.script["set_mock_io_run_paused"] = [_LOST, _LOST]
    port.mock_simulation.set_run_paused("run.1", 1, False)
    assert host.envelopes == []  # Recorded only: no START yet.
    request = port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1])
    for _ in range(2):
        with pytest.raises(Exception) as caught:  # Unknown: the node keeps waiting.
            port.mock_simulation.start(request)
        assert not isinstance(caught.value, MockSimulationRefusedError)
    port.mock_simulation.start(request)
    assert host.commands() == ["set_mock_io_run_paused"] * 3 + ["start_mock_io_node_simulation"]
    assert host.envelopes[:3] == [_PAUSE] * 3


def test_a_refused_resume_is_resent_identically_without_a_new_revision(
    monkeypatch, tmp_path
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    refusal = {"api_version": _RESPONSE, "ok": False, "error": {
        "code": "stale_generation", "message": "not current", "details": {}}}
    port.mock_simulation.set_run_paused("run.1", 1, False)
    port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    port.mock_simulation.set_run_paused("run.1", 2, True)  # Halt.
    host.script["set_mock_io_run_paused"] = [refusal]
    port.mock_simulation.set_run_paused("run.1", 3, False)  # Resume: refused.
    assert port.mock_simulation.resend_run_pause("run.1") == ((), None)  # Resent: ACKed.
    resumes = [envelope["request"] for envelope in host.envelopes][3:]
    assert resumes == [{**_PAUSE["request"], "run_revision": "3"}] * 2
    assert port.mock_simulation.resend_run_pause("run.1") == ((), None)
    assert len(host.envelopes) == 5  # Settled: no second revision, no resend.


def test_a_stale_pause_refusal_is_logged_and_not_retried(monkeypatch, tmp_path, caplog) -> None:
    port, host = _native(monkeypatch, tmp_path)
    host.script["set_mock_io_run_paused"] = [_ERROR]
    port.mock_simulation.set_run_paused("run.1", 1, False)
    port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    # No accepted receipt: reported unconfirmed once, never retried.
    assert port.mock_simulation.resend_run_pause("run.1") == (
        ({"paused": False, "run_revision": "1", "held_node_instance_ids": []},), None
    )
    assert port.mock_simulation.resend_run_pause("run.1") == ((), None)
    assert host.commands() == ["set_mock_io_run_paused", "start_mock_io_node_simulation"]
    assert "stale" in caplog.text


def test_a_lower_pause_revision_is_never_sent(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    port.mock_simulation.set_run_paused("run.1", 3, True)
    port.mock_simulation.set_run_paused("run.1", 2, False)
    port.mock_simulation.set_run_paused("run.1", 3, False)
    assert [envelope["request"]["run_revision"] for envelope in host.envelopes
            if envelope["command"] == "set_mock_io_run_paused"] == ["1", "3"]


def test_a_pause_superseded_unanswered_is_reported_unconfirmed(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    host.script["set_mock_io_run_paused"] = [_LOST]
    port.mock_simulation.set_run_paused("run.1", 2, True)  # Halt: reply lost.
    port.mock_simulation.set_run_paused("run.1", 3, False)
    assert port.mock_simulation.resend_run_pause("run.1") == (
        ({"paused": True, "run_revision": "2", "held_node_instance_ids": []},), None
    )


def test_native_port_maps_a_closed_refusal(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    host.script["start_mock_io_node_simulation"] = [_ERROR]
    with pytest.raises(MockSimulationRefusedError) as caught:
        port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    assert caught.value.code == "mock_simulation_stale"
    assert caught.value.details == {"reason": "mock_simulation_stale"}


def test_native_port_leaves_an_unknown_reply_unknown(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    host.script["start_mock_io_node_simulation"] = [{**_ACK, "status": "superseded"}]
    with pytest.raises(Exception) as caught:
        port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    assert not isinstance(caught.value, MockSimulationRefusedError)


def _run_cleanup(port) -> tuple[Any, list[dict[str, Any]]]:
    """The Run's real per-tick Mock cleanup over this port."""

    from manyforge_behavior.program_runtime import ExecutableProgramRun

    events: list[dict[str, Any]] = []
    run = SimpleNamespace(
        _ports=SimpleNamespace(mock_simulation=port),
        _capture=SimpleNamespace(program_run_id="run.1"),
        _mock_ends_reported=set(),
        _record_node_history_event=events.append,
    )
    return (lambda final: ExecutableProgramRun._settle_mock_ends(run, final=final)), events


def _read_node(port):
    log: list[tuple[str, ...]] = []
    attempts = SimpleNamespace(
        begin=lambda instance: NodeAttemptIdentity("run.1", instance, "attempt:opaque")
    )
    common = _common("io.get_input", None, [], _ROWS[:1])
    common.update(node_instance_id="search_trip", attempt_source=attempts,
                  mock_simulation=port)
    return OwnerInvocationAdapter(**common, port=_ReadPort(log))


def test_a_lost_end_is_resent_identically_on_the_next_tick_and_acked(
    monkeypatch, tmp_path
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    host.script["end_mock_io_node_simulation"] = [_LOST]
    node = _read_node(port.mock_simulation)
    assert _tick(node) is _SUCCESS  # START, read, END (reply lost).
    cleanup, events = _run_cleanup(port.mock_simulation)
    cleanup(False)  # The Run's next tick: the same END again, now ACKed.
    cleanup(True)
    ends = [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert ends == [_END, _END]
    assert host.commands() == [
        "set_mock_io_run_paused", "start_mock_io_node_simulation",
        "end_mock_io_node_simulation", "end_mock_io_node_simulation",
    ]
    assert events == []  # Cancelled: proven by its ACK.


def test_an_end_never_acked_is_reported_unconfirmed_once(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    port.mock_simulation.set_run_paused("run.1", 1, False)
    host.script["end_mock_io_node_simulation"] = [_LOST] * 4
    node = _read_node(port.mock_simulation)
    _tick(node)
    cleanup, events = _run_cleanup(port.mock_simulation)
    cleanup(True)  # The tree ended with its END unanswered.
    cleanup(True)
    assert events == [{
        "kind": "program_node_mock_simulation_end_unconfirmed",
        "node_instance_id": "search_trip",
        "node_attempt_id": "attempt:opaque",
        "node_start_index": "1",
    }]
    from manyforge_behavior.production_ports import ProductionPortError

    with pytest.raises(ProductionPortError, match="unconfirmed Mock simulation ends"):
        port.unbind("run.1")  # Lost again: unbind fails, context kept.
    port.unbind("run.1")  # The next unbind resends: OK, so it succeeds.
    ends = [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert ends == [_END] * 5


def test_an_unanswered_pause_is_reported_when_the_run_ends(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    host.script["set_mock_io_run_paused"] = [_LOST] * 3
    port.mock_simulation.set_run_paused("run.1", 1, False)
    with pytest.raises(Exception):  # The first START establishes it: lost.
        port.mock_simulation.start(port.mock_simulation.request(_IDENTITY, 1, _ROWS[:1]))
    cleanup, events = _run_cleanup(port.mock_simulation)
    cleanup(False)
    assert events == []
    cleanup(True)
    assert events == [{"kind": "program_mock_simulation_pause_unconfirmed",
                       "paused": False, "run_revision": "1", "held_node_instance_ids": []}]
    assert host.envelopes == [_PAUSE] * 3


def test_a_run_that_never_starts_a_mock_script_makes_no_mock_call(
    monkeypatch, tmp_path
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)  # Run start.
    mock.set_run_paused("run.1", 2, True)  # Halt.
    mock.set_run_paused("run.1", 3, False)  # Resume.
    cleanup, events = _run_cleanup(mock)
    cleanup(False)
    cleanup(True)  # Stop / tree end.
    mock.release_run("run.1")
    assert host.envelopes == [] and events == []


def test_the_first_start_sends_the_current_state_once(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)
    mock.set_run_paused("run.1", 2, True)
    mock.set_run_paused("run.1", 4, False)  # Resumed before any node start.
    mock.start(mock.request(_IDENTITY, 1, _ROWS[:1]))
    mock.start(mock.request(NodeAttemptIdentity("run.1", "search_trip", "attempt:2"), 2,
                            _ROWS[:1]))
    pauses = [envelope["request"] for envelope in host.envelopes
              if envelope["command"] == "set_mock_io_run_paused"]
    assert pauses == [{**_PAUSE["request"], "run_revision": "4"}]


def test_a_halt_during_the_first_starts_pause_send_is_sent(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)

    def halt_meanwhile():
        mock.set_run_paused("run.1", 2, True)  # Halt while paused=false is in flight.
        return None

    host.script["set_mock_io_run_paused"] = [halt_meanwhile]
    mock.start(mock.request(_IDENTITY, 1, _ROWS[:1]))
    pauses = [(envelope["request"]["run_revision"], envelope["request"]["paused"])
              for envelope in host.envelopes
              if envelope["command"] == "set_mock_io_run_paused"]
    assert pauses == [("1", False), ("2", True)]
    assert mock.resend_run_pause("run.1") == ((), None)  # Both answered, nothing pending.


def _refusal(reason: str, code: str = "stale_generation") -> dict[str, Any]:
    return {"api_version": _RESPONSE, "ok": False, "error": {
        "code": code, "message": "refused", "details": {"reason": reason}}}


def _started(port, host):
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)
    start = mock.request(_IDENTITY, 1, _ROWS[:1])
    mock.start(start)
    return mock, start


@pytest.mark.parametrize(
    "refusal",
    [_refusal("mock_simulation_stale"), _refusal("mock_simulation_scope_stale"),
     _refusal("io_signal_service_unavailable", "unsupported_capability")],
)
def test_a_refused_end_stays_pending_and_is_resent_until_acked(
    monkeypatch, tmp_path, caplog, refusal
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock, start = _started(port, host)
    host.script["end_mock_io_node_simulation"] = [refusal, refusal]
    mock.end(start)
    cleanup, events = _run_cleanup(mock)
    cleanup(False)  # Refused again: still pending, never "cancelled".
    assert mock.resend_unknown_ends("run.1") == ()  # Exact OK ACK.
    cleanup(True)
    assert events == []
    ends = [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert ends == [_END] * 3
    assert caplog.text.count("END refused") == 1  # Once per END, warning level.
    assert all(record.levelname == "WARNING" for record in caplog.records
               if "END refused" in record.getMessage())


def test_a_refused_end_never_acked_is_reported_when_the_run_ends(
    monkeypatch, tmp_path
) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock, start = _started(port, host)
    host.script["end_mock_io_node_simulation"] = [_refusal("mock_simulation_stale")] * 4
    mock.end(start)
    cleanup, events = _run_cleanup(mock)
    cleanup(True)
    assert [event["kind"] for event in events] == [
        "program_node_mock_simulation_end_unconfirmed"
    ]
    from manyforge_behavior.production_ports import ProductionPortError

    for _ in range(2):  # Refused: the unbind fails; nothing is "cancelled".
        with pytest.raises(ProductionPortError):
            port.unbind("run.1")
    port.unbind("run.1")  # Exact OK ACK: the Run unbinds with no END pending.
    ends = [envelope for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert [envelope["request"] for envelope in ends] == [_END] * 5
    assert len([envelope for envelope in host.envelopes
                if envelope["command"] == "end_mock_io_node_simulation"]) == 5


def test_a_new_start_waits_while_an_older_end_is_unanswered(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock, start = _started(port, host)
    host.script["end_mock_io_node_simulation"] = [_LOST, _LOST]
    mock.end(start)  # Lost.
    second = mock.request(NodeAttemptIdentity("run.1", "search_trip", "attempt:2"), 2,
                          _ROWS[:1])
    with pytest.raises(Exception) as caught:  # END resent, lost again: wait.
        mock.start(second)
    assert not isinstance(caught.value, MockSimulationRefusedError)
    mock.start(second)  # END resent and ACKed, then the START.
    assert host.commands()[2:] == [
        "end_mock_io_node_simulation", "end_mock_io_node_simulation",
        "end_mock_io_node_simulation", "start_mock_io_node_simulation",
    ]
    ends = [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert ends == [_END] * 3


def test_a_halt_before_the_first_start_sends_paused_true_first(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)
    mock.set_run_paused("run.1", 2, True)  # Halted before any node start.
    mock.start(mock.request(_IDENTITY, 1, _ROWS[:1]))
    assert host.commands() == ["set_mock_io_run_paused", "start_mock_io_node_simulation"]
    assert host.envelopes[0]["request"] == {
        **_PAUSE["request"], "run_revision": "2", "paused": True
    }


def test_a_lost_end_fails_unbind_until_the_next_unbind_gets_its_ack(
    monkeypatch, tmp_path
) -> None:
    from manyforge_behavior.production_ports import ProductionPortError

    port, host = _native(monkeypatch, tmp_path)
    mock, start = _started(port, host)
    host.script["end_mock_io_node_simulation"] = [_LOST, _LOST]
    mock.end(start)  # Lost.
    with pytest.raises(ProductionPortError, match="unconfirmed Mock simulation ends"):
        port.unbind("run.1")  # Resent, lost again: unbind fails.
    assert port._context("run.1") is not None  # Still bound.
    port.unbind("run.1")  # Resent, OK: unbound.
    with pytest.raises(ProductionPortError):
        port._context("run.1")
    ends = [envelope["request"] for envelope in host.envelopes
            if envelope["command"] == "end_mock_io_node_simulation"]
    assert ends == [_END] * 3


# --- S2b: held-node detection (wire pending Track 2) ------------------------


class _HeldState:
    def __init__(self) -> None:
        self.held: set[str] = set()
        self.revision = 1

    def holds_subtree(self, node_id: str) -> bool:
        return node_id in self.held

    def snapshot(self):
        return SimpleNamespace(selection_revision=self.revision)


def test_the_held_mask_is_every_node_under_a_wholly_held_hold_scope() -> None:
    from manyforge_behavior.program_runtime import ExecutableProgramRun

    state = _HeldState()
    run = SimpleNamespace(
        _hold_scopes={
            "wait": "wait",  # Its own Hold wrapper.
            "probe": "branch",  # A Continue leaf: the nearest Hold above it.
            "branch": "branch",
            "other": "other",  # Its wrapper is not held.
            "free": None,  # Continue all the way up: never held.
        },
        _operator_stop_state=state,
    )
    assert ExecutableProgramRun.mock_held_nodes(run) == ()
    state.held = {"wait", "branch", "free"}
    assert ExecutableProgramRun.mock_held_nodes(run) == ("branch", "probe", "wait")


def test_the_snapshot_carries_the_held_mask_and_halt_together(monkeypatch, tmp_path) -> None:
    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False, ("wait",))  # Held before the first START.
    mock.start(mock.request(_IDENTITY, 1, _ROWS[:1]))
    mock.set_run_paused("run.1", 2, True, ("wait",))  # Halt: both causes.
    mock.set_run_paused("run.1", 3, False, ("wait",))  # Resume: still held.
    mock.set_run_paused("run.1", 4, False, ())  # Released.
    bodies = [(envelope["request"]["run_revision"], envelope["request"]["paused"],
               envelope["request"]["held_node_instance_ids"])
              for envelope in host.envelopes if envelope["command"] == "set_mock_io_run_paused"]
    assert bodies == [("1", False, ["wait"]), ("2", True, ["wait"]),
                      ("3", False, ["wait"]), ("4", False, [])]
    assert host.commands().index("start_mock_io_node_simulation") == 1


@pytest.mark.parametrize(
    ("paused", "held"),
    [(True, ()), (False, ("search_trip",))],
    ids=["newer_halt", "newer_held_mask"],
)
def test_an_older_pause_ack_cannot_confirm_a_newer_unknown_snapshot(
    monkeypatch, tmp_path, paused, held
) -> None:
    from manyforge_behavior.production_ports import ProductionPortError

    port, host = _native(monkeypatch, tmp_path)
    mock = port.mock_simulation
    mock.set_run_paused("run.1", 1, False)
    request = mock.request(_IDENTITY, 1, _ROWS[:1])
    original = deepcopy(request)

    def newer_snapshot_meanwhile():
        # The original running/unheld snapshot is still in flight. A newer
        # committed Halt/held snapshot has no confirmed reply when it returns.
        mock.set_run_paused("run.1", 2, paused, held)
        return None  # The older snapshot gets its ordinary successful ACK.

    host.script["set_mock_io_run_paused"] = [newer_snapshot_meanwhile, _LOST]
    with pytest.raises(ProductionPortError, match="pause state is not yet answered"):
        mock.start(request)
    assert host.commands() == ["set_mock_io_run_paused", "set_mock_io_run_paused"]
    first, current = [envelope["request"] for envelope in host.envelopes]
    assert (first["run_revision"], first["paused"], first["held_node_instance_ids"]) == ("1", False, [])
    assert (current["run_revision"], current["paused"], current["held_node_instance_ids"]) == (
        "2", paused, list(held)
    )
    assert request == original
    # Ordinary retry resends the exact newer snapshot before the same START;
    # neither a new node index nor a replacement Run revision is invented.
    mock.start(request)
    assert host.commands() == ["set_mock_io_run_paused", "set_mock_io_run_paused",
                               "set_mock_io_run_paused", "start_mock_io_node_simulation"]
    assert host.envelopes[2]["request"] == current
    assert host.envelopes[3]["request"] == original
    assert mock.resend_run_pause("run.1") == ((), None)
