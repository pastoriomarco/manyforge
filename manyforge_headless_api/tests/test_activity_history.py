"""History records of component actions and Apply terminals (activity_history)."""

from __future__ import annotations

import threading
import time
from typing import Any

from fastapi.testclient import TestClient

from manyforge_headless_api.activity_history import (
    SCHEMA_VERSION,
    apply_record,
    component_action_record,
)
from manyforge_headless_api.app import create_app
from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeManager,
    FakeProgramRunner,
    _service,
)
from manyforge_headless_api.tests.test_component_model import (
    _ModelExecution,
    _reply,
    _state,
)
from manyforge_headless_api.tests.test_execution_event_stream import (
    _EPOCH,
    _SubscribedExecution,
    _host_snapshot,
    _host_state,
    _message,
)
from manyforge_headless_api.tests.test_execution_events import _RecordingCore


def _result(action: str, outcome: str, started: int, error: dict[str, str] | None = None):
    result: dict[str, Any] = {
        "action": action, "outcome": outcome, "started_at_ns": started, "finished_at_ns": started + 1,
    }
    if error is not None:
        result["error"] = error
    return result


class _ActivityCore(FakeCore):
    def __init__(self) -> None:
        super().__init__()
        self.activity: list[dict[str, Any]] = []

    def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
        self.activity.append(record)
        return {"recorded": True}


class _PumpCore(_RecordingCore):
    def __init__(self) -> None:
        super().__init__()
        self.activity: list[dict[str, Any]] = []

    def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
        self.activity.append(record)
        return {"recorded": True}


# Records ---------------------------------------------------------------------


def test_a_failed_action_records_its_typed_error_and_a_display_line() -> None:
    record = component_action_record("lite6_arm", "enable", "failed", {
        "code": "xarm_joint_angles_unavailable", "message": "Joint angles unavailable",
        "vendor_code": "C2",
    })
    assert record == {
        "schema_version": SCHEMA_VERSION,
        "kind": "component_action",
        "subject": "lite6_arm",
        "action": "enable",
        "outcome": "failed",
        "error": {"code": "xarm_joint_angles_unavailable",
                  "message": "Joint angles unavailable", "vendor_code": "C2"},
        "components": [],
        "message": "Enable lite6_arm failed: xarm_joint_angles_unavailable: "
                   "Joint angles unavailable (vendor C2)",
    }


def test_a_success_has_no_error_and_a_stop_interruption_reads_interrupted() -> None:
    success = component_action_record("lite6", "reset_position", "succeeded")
    assert success["error"] is None and success["message"] == "Reset position lite6 succeeded"
    stopped = component_action_record("lite6_arm", "enable", "failed", {
        "code": "component_action_interrupted_by_stop", "message": "Stopped"})
    assert stopped["outcome"] == "interrupted"


def test_an_apply_record_keeps_each_component_row_and_its_error() -> None:
    record = apply_record(
        "scenario_load", "failed",
        {"code": "component_launch_not_settled", "message": "lite6 didn't start"},
        [{"component_id": "lite6", "disposition": "add", "lifecycle_after": "not_prepared",
          "error": {"code": "component_launch_not_settled", "message": "lite6 didn't start"}},
         {"component_id": "gripper", "disposition": "add", "lifecycle_after": "prepared"}],
    )
    assert record["kind"] == "apply" and record["subject"] == "scenario_load"
    assert record["action"] is None
    assert record["components"][1] == {
        "component_id": "gripper", "disposition": "add", "lifecycle_after": "prepared"}
    assert record["message"] == (
        "Scenario load failed: component_launch_not_settled: lite6 didn't start; "
        "lite6: component_launch_not_settled")


# One event per finished host action ---------------------------------------------


def _model(*states: dict[str, Any]) -> dict[str, Any]:
    return {"component_states": list(states)}


def test_the_pump_records_each_new_last_action_once_after_its_baseline() -> None:
    core = _PumpCore()
    pump = ExecutionEventPump(core, _SubscribedExecution())  # type: ignore[arg-type]
    old = _result("prepare", "succeeded", 1)
    pump._record_finished_actions(_model(_state("lite6", last_action=old)))  # noqa: SLF001
    assert core.activity == []  # Finished before this Headless: not recorded again.
    failed = _result("enable", "failed", 5, {"code": "xarm_joint_angles_unavailable",
                                             "message": "Joint angles unavailable"})
    for _ in range(3):  # Repeated models carry the same result.
        pump._record_finished_actions(_model(  # noqa: SLF001
            _state("lite6", last_action=old), _state("lite6_arm", last_action=failed)))
    assert [(r["subject"], r["action"], r["outcome"]) for r in core.activity] == [
        ("lite6_arm", "enable", "failed")]
    assert core.activity[0]["error"]["code"] == "xarm_joint_angles_unavailable"
    again = _result("enable", "failed", 9, failed["error"])
    pump._record_finished_actions(_model(_state("lite6_arm", last_action=again)))  # noqa: SLF001
    assert len(core.activity) == 2  # The same action again is a new result.


def test_a_lost_history_record_never_breaks_the_pump() -> None:
    core = _PumpCore()

    def fail(_record: dict[str, Any]) -> dict[str, Any]:
        raise RuntimeError("historian queue closed")

    core.record_activity = fail  # type: ignore[method-assign]
    pump = ExecutionEventPump(core, _SubscribedExecution())  # type: ignore[arg-type]
    pump._record_finished_actions(_model())  # noqa: SLF001
    pump._record_finished_actions(_model(  # noqa: SLF001
        _state("lite6", last_action=_result("prepare", "succeeded", 1))))


def test_the_running_pump_records_an_action_the_host_finished() -> None:
    core = _PumpCore()
    finished = _result("prepare", "failed", 3, {"code": "component_launch_not_settled",
                                                "message": "lite6 didn't start"})
    execution = _SubscribedExecution(_host_snapshot())
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01)  # type: ignore[arg-type]
    pump.start()
    try:
        assert core.completed.wait(3.0)  # The first model is the baseline.
        execution.script.append(_message("state", _EPOCH, state={
            **_host_state(), "component_states": [_state("lite6", "not_prepared",
                                                         last_action=finished)]}))
        deadline = time.monotonic() + 3.0
        while not core.activity and time.monotonic() < deadline:
            time.sleep(0.01)
    finally:
        pump.shutdown()
    assert [(r["subject"], r["outcome"]) for r in core.activity] == [("lite6", "failed")]


# Refusals: never started, so the host has no result -----------------------------


def test_a_refused_action_is_recorded_as_refused_with_its_code() -> None:
    core = _ActivityCore()
    execution = _ModelExecution(_reply())
    execution.refusal = "component_action_in_progress"
    service = _service(core, FakeManager(), execution, program_runner=FakeProgramRunner(active=False))
    with TestClient(create_app(service=service)) as client:
        response = client.post("/api/components/robot/actions", json={"action": "enable"})
    assert response.status_code == 409
    assert [(r["subject"], r["action"], r["outcome"], r["error"]["code"]) for r in core.activity] == [
        ("robot", "enable", "refused", "component_action_in_progress")]


def test_an_accepted_action_is_left_to_the_host_result() -> None:
    core = _ActivityCore()
    service = _service(core, FakeManager(), _ModelExecution(_reply()),
                       program_runner=FakeProgramRunner(active=False))
    service.component_action("robot", "disable")
    assert core.activity == []


def test_a_group_member_skipped_after_its_owner_failed_is_recorded_refused() -> None:
    core = _ActivityCore()
    execution = _ModelExecution(_reply(component_states=[
        _state("lite6", "not_prepared"),
        _state("lite6_arm", "not_prepared", owner_id="lite6", role="arm"),
    ]))
    execution.outcome = {"lite6": "failed"}
    service = _service(core, FakeManager(), execution, program_runner=FakeProgramRunner(active=False))
    service.component_actions("prepare", ["lite6", "lite6_arm"])
    for thread in threading.enumerate():
        if thread.name == "component-group-action":
            thread.join(5.0)
    assert [(r["subject"], r["outcome"], r["error"]["code"]) for r in core.activity] == [
        ("lite6_arm", "refused", "owner_not_prepared")]


# Apply terminals from Composer ---------------------------------------------------


def test_an_apply_outcome_is_recorded_with_its_rows() -> None:
    core = _ActivityCore()
    service = _service(core, FakeManager(), _ModelExecution(_reply()),
                       program_runner=FakeProgramRunner(active=False))
    with TestClient(create_app(service=service)) as client:
        response = client.post("/api/execution/history/apply-outcomes", json={
            "apply_kind": "scenario_load", "outcome": "failed",
            "error": {"code": "execution_owner_unavailable", "message": ""},
            "components": [{"component_id": "lite6", "disposition": "add",
                            "error": {"code": "execution_owner_unavailable", "message": "x"}}],
        })
        bad = client.post("/api/execution/history/apply-outcomes",
                          json={"apply_kind": "scenario_open", "outcome": "failed"})
    assert response.status_code == 200 and response.json() == {"recorded": True}
    assert bad.status_code == 422
    [record] = core.activity
    assert record["subject"] == "scenario_load" and record["outcome"] == "failed"
    assert record["error"] == {"code": "execution_owner_unavailable"}
    assert record["components"][0]["error"]["code"] == "execution_owner_unavailable"
