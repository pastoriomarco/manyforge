"""OD-375: the run records one history event per finished node attempt."""

from __future__ import annotations

from dataclasses import replace
from datetime import datetime
from threading import Event
import time
from typing import Any

import py_trees

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import OwnerTaskState
from manyforge_behavior.program import Program
from manyforge_behavior.program_clock import ProgramClock

from .test_executable_node_runtime import (
    _DIGEST_A,
    _InvocationPort,
    _RefusingCancelPort,
    _TaskPort,
    _capture,
    _catalog,
    _fake_port_run,
    _fast_source,
    _identified,
    _leaf,
    _minimal_document,
    _ports,
    _readiness,
    _tick_answered,
)
from .test_motion_automatic_retry import _MotionPort, _assessment, _failed, _succeeded

_FIELDS = {
    "program_run_id", "node_instance_id", "node_id", "node_attempt_id",
    "started_at_wall", "finished_at_ns", "finished_at_wall", "duration_ms",
    "outcome", "error", "scene_operation", "scene_resource_id",
}
_TIMES = {"started_at_wall", "finished_at_ns", "finished_at_wall", "duration_ms"}


def _run(program: Program, ports, events: list[dict[str, Any]] | None, **kwargs: Any):
    if events is not None:
        ports = replace(ports, node_attempt_history=events.append)
    return _fake_port_run(
        program=program,
        environment=build_node_environment(),
        capture=_capture(program),
        readiness=_readiness(_DIGEST_A),
        ports=ports,
        **kwargs,
    )


def _summary(events: list[dict[str, Any]]) -> list[tuple[Any, ...]]:
    return [
        (
            event["node_instance_id"],
            event["node_attempt_id"],
            event["outcome"],
            None if event["error"] is None else {
                key: event["error"][key] for key in ("code", "message")
            },
        )
        for event in events
    ]


def test_each_finished_attempt_is_one_event_and_a_scene_change_names_its_object() -> None:
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    events: list[dict[str, Any]] = []
    run = _run(program, _ports(), events)

    assert _tick_answered(run) is py_trees.common.Status.SUCCESS
    assert run.tick_once() is py_trees.common.Status.SUCCESS

    assert [set(event) for event in events] == [_FIELDS, _FIELDS]
    move, remove = events
    assert {key: move[key] for key in _FIELDS - _TIMES} == {
        "program_run_id": 1,
        "node_instance_id": "move_to_pick",
        "node_id": "motion.move_to_joint_target",
        "node_attempt_id": 1,
        "outcome": "succeeded",
        "error": None,
        "scene_operation": None,
        "scene_resource_id": None,
    }
    assert (remove["node_id"], remove["node_attempt_id"], remove["outcome"]) == (
        "scene.remove_object", 2, "succeeded",
    )
    assert (remove["scene_operation"], remove["scene_resource_id"]) == ("remove_object", "table")
    for event in events:
        started = datetime.fromisoformat(event["started_at_wall"])
        assert started.tzinfo is not None
        assert datetime.fromisoformat(event["finished_at_wall"]) >= started
        assert int(event["finished_at_ns"]) > 0
        assert event["duration_ms"] >= 0


def test_a_failed_attempt_carries_its_code_and_the_retry_is_a_new_attempt() -> None:
    document = _minimal_document()
    document["tree"] = {
        "node_id": "retry",
        "instance_id": "retry_move",
        "parameters": {"num_attempts": 2},
        "children": [document["tree"]["children"][0]],
    }
    program = Program.from_dict(_identified(document), _catalog())
    task = _TaskPort((OwnerTaskState.FAILED, OwnerTaskState.SUCCEEDED))
    task.metadata = {
        "recovery_assessment": {
            "status": "action_required", "execution_state": "Failed", "allowed_actions": [],
        }
    }
    events: list[dict[str, Any]] = []
    run = _run(program, _ports(task), events)

    assert run.tick_once() is py_trees.common.Status.RUNNING
    assert run.tick_once() is py_trees.common.Status.SUCCESS

    assert _summary(events) == [
        ("move_to_pick", 1, "failed", {"code": "owner_failed", "message": ""}),
        ("move_to_pick", 2, "succeeded", None),
    ]
    # The typed error keeps the leaf's bounded details (None when it has none).
    assert "details" in events[0]["error"]


def test_an_automatic_retry_closes_the_failed_attempt_once() -> None:
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    motion = _MotionPort([_failed(_assessment()), _succeeded()])
    events: list[dict[str, Any]] = []
    run = _run(
        program,
        replace(_ports(), kernel_operations=motion),
        events,
        program_clock=ProgramClock(_fast_source()),
        motion_attention=lambda _node, _key: None,
    )

    status = run.tick_once()
    assert _summary(events) == [
        ("move_to_pick", 1, "failed", {"code": "planning_failed", "message": "no feasible path"}),
    ]
    for _ in range(8):
        if status is not py_trees.common.Status.RUNNING:
            break
        status = _tick_answered(run)
    assert status is py_trees.common.Status.SUCCESS
    assert len(motion.starts) == 2
    assert _summary(events)[1:] == [
        ("move_to_pick", 2, "succeeded", None),
        ("remove_table", 3, "succeeded", None),
    ]


def test_run_stop_closes_each_running_attempt_with_its_stop_result() -> None:
    document = _minimal_document()
    motion = document["tree"]["children"][0]
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "together",
        "parameters": {"success_policy": "all"},
        "children": [
            motion,
            _leaf("timer", "pause", duration_s=60.0),
            _leaf("kernel.cell_stop", "halt_cell"),
        ],
    }
    program = Program.from_dict(_identified(document), _catalog())
    events: list[dict[str, Any]] = []
    ports = replace(
        _ports(_RefusingCancelPort((OwnerTaskState.ACTIVE,) * 4)),
        kernel_controls=_TaskPort((OwnerTaskState.ACTIVE,) * 4),
    )
    run = _run(program, ports, events)

    assert run.tick_once() is py_trees.common.Status.RUNNING
    run.stop()
    assert events == []  # Closed once the Stop's sent requests settled.
    assert run.settle_sent_requests()

    assert sorted(_summary(events)) == [
        ("halt_cell", 3, "stopped", None),  # Not cancellable.
        ("move_to_pick", 1, "failed", {  # Its cancellation was refused.
            "code": "owner_cancellation_failed", "message": "owner unreachable",
        }),
        ("pause", 2, "interrupted", {
            "code": "program_node_interrupted", "message": "the running attempt was stopped",
        }),
    ]


class _BlockingInvocationPort(_InvocationPort):
    def __init__(self) -> None:
        super().__init__()
        self.entered = Event()
        self.release = Event()

    def invoke(self, node_id, identity, inputs):
        self.entered.set()
        assert self.release.wait(5)
        return super().invoke(node_id, identity, inputs)


def test_a_scene_change_sent_before_the_stop_records_its_real_result() -> None:
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    invocation = _BlockingInvocationPort()
    events: list[dict[str, Any]] = []
    run = _run(program, _ports(invocation=invocation), events)
    try:
        for _ in range(8):
            assert run.tick_once() is py_trees.common.Status.RUNNING
            if invocation.entered.wait(0.05):
                break
        assert invocation.entered.is_set()
        run.stop()
        assert not run.settle_sent_requests()
        assert _summary(events) == [("move_to_pick", 1, "succeeded", None)]
    finally:
        invocation.release.set()
    for _ in range(500):
        if run.settle_sent_requests():
            break
        time.sleep(0.01)

    # OD-213: the sent request completes and ends its node.
    remove = events[-1]
    assert _summary(events) == [
        ("move_to_pick", 1, "succeeded", None),
        ("remove_table", 2, "succeeded", None),
    ]
    assert (remove["scene_operation"], remove["scene_resource_id"]) == ("remove_object", "table")


def test_without_a_sink_no_attempt_is_tracked() -> None:
    program = Program.from_dict(_identified(_minimal_document()), _catalog())
    run = _run(program, _ports(), None)

    assert _tick_answered(run) is py_trees.common.Status.SUCCESS
    assert run._attempt_history is None  # noqa: SLF001


def test_a_preempted_attempt_is_preempted_whatever_its_literal_result() -> None:
    from types import SimpleNamespace

    from manyforge_behavior.program_runtime import _attempt_result

    for status in (py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE):
        leaf = SimpleNamespace(
            status=status, preempted=True, failure_reason_code=None, failure_message=""
        )
        assert _attempt_result(leaf) == ("preempted", None)
