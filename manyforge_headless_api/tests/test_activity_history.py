"""History records of component actions, Apply and Program Reset terminals,
Program node attempts and scene changes (activity_history)."""

from __future__ import annotations

import threading
import time
import json
from typing import Any

from fastapi.testclient import TestClient
import pytest

from manyforge_headless_api.activity_history import (
    SCHEMA_VERSION,
    apply_record,
    component_action_record,
    program_node_attempt_record,
    program_reset_record,
    scene_change_record,
)
from manyforge_headless_api.app import create_app
from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.models import SceneTransactionRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import TrustedAttribution
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeManager,
    FakeProgramRunner,
    FakeTrustedAdapter,
    _scene_body,
    _service,
)
from manyforge_headless_api.tests.test_component_model import (
    _ModelExecution,
    _reply,
    _state,
)
from manyforge_headless_api.tests.test_execution_event_stream import (
    _SubscribedExecution,
    _host_snapshot,
    _host_state,
    _message,
)
from manyforge_headless_api.tests.test_execution_events import _RecordingCore

_APPLICATION_START_ID = "3f9a0000000000a1"


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
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, _SubscribedExecution(), application_start_id=_APPLICATION_START_ID
    )
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
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, _SubscribedExecution(), application_start_id=_APPLICATION_START_ID
    )
    pump._record_finished_actions(_model())  # noqa: SLF001
    pump._record_finished_actions(_model(  # noqa: SLF001
        _state("lite6", last_action=_result("prepare", "succeeded", 1))))


def test_the_running_pump_records_an_action_the_host_finished() -> None:
    core = _PumpCore()
    finished = _result("prepare", "failed", 3, {"code": "component_launch_not_settled",
                                                "message": "lite6 didn't start"})
    execution = _SubscribedExecution(_host_snapshot())
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01
    )
    pump.start()
    try:
        assert core.completed.wait(3.0)  # The first model is the baseline.
        execution.script.append(_message("state", state={
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


# Program Reset terminals from the Program Runner (OD-305) -------------------------


def _reset_facts(**changes: Any) -> dict[str, Any]:
    return {
        "reload_initial_scene": True, "reset_robot_position": False,
        "reset_stored_values": False, "program_run_id": 7, "program_revision": "sha256:" + "1" * 64,
        "applied_record_digest": "sha256:" + "2" * 64,
        "scene_revision_id": 10, "scene_content_hash": "sha256:" + "3" * 64,
        "cause_code": None, **changes,
    }


def test_a_program_reset_record_names_its_options_and_scene_revision() -> None:
    record = program_reset_record("succeeded", None, _reset_facts())
    assert record["kind"] == "program_reset" and record["subject"] == "program_reset"
    assert record["action"] is None and record["components"] == []
    assert record["error"] is None and record["reset"] == _reset_facts()
    assert record["message"] == (
        "Program reset succeeded · run 7 · reloaded initial scene (scene revision 10)")
    refused = program_reset_record(
        "refused", {"code": "authoring_not_ready", "message": "not ready"},
        _reset_facts(program_run_id=None, scene_revision_id=None, scene_content_hash=None,
                     reset_robot_position=True, reset_stored_values=True))
    assert refused["message"] == (
        "Program reset refused: authoring_not_ready: not ready"
        " · reload initial scene · reset robot position · reset stored values")


def test_a_program_reset_outcome_is_recorded_through_the_route() -> None:
    core = _ActivityCore()
    service = _service(core, FakeManager(), _ModelExecution(_reply()),
                       program_runner=FakeProgramRunner(active=False))
    with TestClient(create_app(service=service)) as client:
        response = client.post("/api/execution/history/program-reset-outcomes", json={
            "outcome": "failed",
            "error": {"code": "program_cycle_recovery_required", "message": "scene owner lost"},
            "reset": _reset_facts(scene_revision_id=None, scene_content_hash=None,
                                  cause_code="program_cycle_reset_scene_preparation_failed"),
        })
        bad_outcome = client.post("/api/execution/history/program-reset-outcomes",
                                  json={"outcome": "accepted", "reset": _reset_facts()})
        bad_revision = client.post("/api/execution/history/program-reset-outcomes",
                                   json={"outcome": "succeeded",
                                         "reset": _reset_facts(scene_revision_id=0)})
        no_facts = client.post("/api/execution/history/program-reset-outcomes",
                               json={"outcome": "succeeded"})
        facts = _reset_facts()
        del facts["reset_stored_values"]
        no_option = client.post("/api/execution/history/program-reset-outcomes",
                                json={"outcome": "succeeded", "reset": facts})
    assert response.status_code == 200 and response.json() == {"recorded": True}
    assert {bad_outcome.status_code, bad_revision.status_code, no_facts.status_code,
            no_option.status_code} == {422}
    [record] = core.activity
    assert record["kind"] == "program_reset" and record["outcome"] == "failed"
    assert record["error"] == {"code": "program_cycle_recovery_required",
                               "message": "scene owner lost"}
    assert record["reset"]["cause_code"] == "program_cycle_reset_scene_preparation_failed"
    assert record["reset"]["scene_revision_id"] is None
    assert record["message"].startswith(
        "Program reset failed: program_cycle_recovery_required: scene owner lost · run 7")


# Program node attempts from the Program Runner (OD-375) ---------------------------


def _attempt(**changes: Any) -> dict[str, Any]:
    return {
        "program_run_id": 3, "node_instance_id": "pick_1",
        "node_id": "motion.move_to_pose", "node_attempt_id": 12,
        "started_at_wall": "2026-11-24T10:00:00.125+00:00",
        "finished_at_ns": "5000000000", "finished_at_wall": "2026-11-24T10:00:01.365+00:00",
        "duration_ms": 1240.0, "outcome": "succeeded", "error": None,
        "scene_operation": None, "scene_resource_id": None, **changes,
    }


def test_a_node_attempt_record_carries_its_attempt_facts_and_a_display_line() -> None:
    failed = program_node_attempt_record(_attempt(
        outcome="failed", error={"code": "motion_plan_failed", "message": "No path",
                                 "details": {"input_problem": "invalid"}}))
    assert failed == {
        "schema_version": SCHEMA_VERSION,
        "kind": "program_node_attempt",
        "subject": "motion.move_to_pose",
        "action": None,
        "outcome": "failed",
        "error": {"code": "motion_plan_failed", "message": "No path",
                  "details": {"input_problem": "invalid"}},
        "components": [],
        "message": "Node pick_1 (motion.move_to_pose) failed: motion_plan_failed: No path"
                   " · run 3 attempt 12 · 1.24 s",
        "attempt": {
            "program_run_id": 3, "node_instance_id": "pick_1", "node_attempt_id": 12,
            "started_at_wall": "2026-11-24T10:00:00.125+00:00",
            "finished_at_ns": "5000000000",
            "finished_at_wall": "2026-11-24T10:00:01.365+00:00", "duration_ms": 1240.0,
            "scene_operation": None, "scene_resource_id": None,
        },
    }
    scene = program_node_attempt_record(_attempt(
        node_instance_id="add_box", node_id="scene.add_object", duration_ms=8.4,
        scene_operation="add_object", scene_resource_id="box_1"))
    assert scene["error"] is None
    assert scene["message"] == (
        "Node add_box (scene.add_object) succeeded · run 3 attempt 12 · 8 ms"
        " · add object box_1")
    # Only a failed or interrupted attempt carries an error.
    stopped = program_node_attempt_record(_attempt(
        outcome="stopped", error={"code": "operation_stopped", "message": "Stop"}))
    assert stopped["error"] is None


def test_node_attempts_are_recorded_one_event_each_through_the_route() -> None:
    core = _ActivityCore()
    service = _service(core, FakeManager(), _ModelExecution(_reply()),
                       program_runner=FakeProgramRunner(active=False))
    batch = {
        "schema_version": "manyforge.program_node_attempt_batch.v2",
        "application_start_id": _APPLICATION_START_ID,
        "attempts": [
            _attempt(duration_ms=0),
            _attempt(node_attempt_id=13, outcome="interrupted",
                     error={"code": "component_action_interrupted_by_stop"},
                     scene_operation="detach_carried_object"),
        ],
    }
    invalid = [
        {**batch, "attempts": []},
        {**batch, "attempts": [_attempt()] * 65},
        {**batch, "attempts": [_attempt(outcome="failed")]},  # No error.
        {**batch, "attempts": [_attempt(error={"code": "x", "message": ""})]},
        {**batch, "attempts": [_attempt(outcome="cancelled")]},
        {**batch, "attempts": [_attempt(duration_ms=-1.0)]},
        {**batch, "attempts": [_attempt(started_at_wall="yesterday")]},
        {**batch, "attempts": [_attempt(finished_at_ns="-1")]},
        {**batch, "attempts": [_attempt(finished_at_wall="today")]},
        {**batch, "attempts": [_attempt(outcome="failed", error={
            "code": "x", "details": {"unregistered": 1}})]},
        {**batch, "attempts": [_attempt(node_attempt_id=0)]},
        {**batch, "attempts": [_attempt(node_id="n" * 257)]},
        {**batch, "attempts": [{**_attempt(), "extra": 1}]},
    ]
    with TestClient(create_app(service=service)) as client:
        response = client.post("/api/program/node-attempts", json=batch)
        refused = [client.post("/api/program/node-attempts", json=body) for body in invalid]
        no_start = client.post("/api/program/node-attempts",
                               json={**batch, "application_start_id": None})
        other_start = client.post("/api/program/node-attempts",
                                  json={**batch, "application_start_id": "0bad0000000000b2"})
    assert response.status_code == 200, response.text
    assert response.json() == {
        "schema_version": "manyforge.program_node_attempt_record.v1", "recorded": 2}
    assert {r.status_code for r in refused} == {422}
    assert no_start.json()["detail"]["code"] == "start_identity_required"
    assert other_start.json()["detail"]["code"] == "stale_application_start_id"
    assert [(r["kind"], r["outcome"], r["attempt"]["node_attempt_id"]) for r in core.activity] == [
        ("program_node_attempt", "succeeded", 12), ("program_node_attempt", "interrupted", 13)]
    assert core.activity[0]["attempt"]["duration_ms"] == 0
    assert core.activity[1]["error"] == {"code": "component_action_interrupted_by_stop"}
    assert core.activity[1]["attempt"]["scene_operation"] == "detach_carried_object"


def test_a_lost_node_attempt_line_is_not_counted_and_never_fails_the_route() -> None:
    core = _ActivityCore()

    def fail(_record: dict[str, Any]) -> dict[str, Any]:
        raise HeadlessApiError("activity_record_invalid", "refused")

    core.record_activity = fail  # type: ignore[method-assign]
    service = _service(core, FakeManager(), _ModelExecution(_reply()),
                       program_runner=FakeProgramRunner(active=False))
    with TestClient(create_app(service=service)) as client:
        response = client.post("/api/program/node-attempts", json={
            "schema_version": "manyforge.program_node_attempt_batch.v2",
            "application_start_id": _APPLICATION_START_ID, "attempts": [_attempt()]})
    assert response.status_code == 200 and response.json()["recorded"] == 0


# Scene changes by non-Program callers (OD-375, OD-378) -----------------------------


def test_a_scene_change_record_names_object_caller_and_time() -> None:
    record = scene_change_record("add_object", "succeeded", None, {
        "scene_resource_id": "box_1", "source_label": "composer_ui",
        "principal_id": None, "duration_ms": 12.3, "scene_revision_id": 4,
        "manager_start_id": "0123456789abcdef", "sequence": 9})
    assert record == {
        "schema_version": SCHEMA_VERSION,
        "kind": "scene_change",
        "subject": "add_object",
        "action": None,
        "outcome": "succeeded",
        "error": None,
        "components": [],
        "message": "Scene add object box_1 succeeded · composer_ui · 12 ms",
        "scene_change": {
            "scene_resource_id": "box_1", "source_label": "composer_ui",
            "principal_id": None, "duration_ms": 12.3, "scene_revision_id": 4,
            "manager_start_id": "0123456789abcdef", "sequence": 9},
    }
    refused = scene_change_record(
        "set_collision_policy", "refused",
        {"code": "stale_revision", "message": "Scene changed"},
        {"scene_resource_id": None, "source_label": "mcp", "principal_id": "operator.7",
         "duration_ms": 3.0, "scene_revision_id": None})
    assert refused["message"] == (
        "Scene set collision policy refused: stale_revision: Scene changed · mcp · 3 ms")


class _SceneManager(FakeManager):
    def __init__(self, **response: Any) -> None:
        super().__init__()
        self.response = response
        self.raised: HeadlessApiError | None = None

    def commit_scene_transaction(self, request, *, trusted_principal_id, mesh_content_digest):
        reply = super().commit_scene_transaction(
            request, trusted_principal_id=trusted_principal_id,
            mesh_content_digest=mesh_content_digest)
        if self.raised is not None:
            raise self.raised
        return {**reply, **self.response}


def _scene_request(**changes: Any) -> SceneTransactionRequest:
    body = _scene_body()
    body.pop("application_start_id")
    body["request_context"]["source_label"] = "composer_ui"
    for name, value in changes.items():
        if value is None:
            body.pop(name, None)
        else:
            body[name] = value
    return SceneTransactionRequest.model_validate_json(json.dumps(body))


def _commit(manager: _SceneManager, request: SceneTransactionRequest) -> list[dict[str, Any]]:
    core = _ActivityCore()
    service = _service(core, manager)
    try:
        service.scene_commit(request, TrustedAttribution("operator.7"))
    except HeadlessApiError:
        pass
    return core.activity


@pytest.mark.parametrize("response", [
    {},
    {"replay_status": "replayed_exact"},
    {"ok": False, "projection_status": "not_attempted", "committed_projection_revision": None,
     "error": {"code": "stale_revision", "message": "Scene changed"}},
    {"ok": False, "projection_status": "failed_projection_uncertain",
     "committed_projection_revision": None,
     "error": {"code": "stale_revision", "message": "Scene changed"}},
])
def test_a_scene_commit_the_manager_answered_reaches_history_from_its_stream_only(
    response: dict[str, Any],
) -> None:
    """OD-400: whatever the Manager decided is recorded from its status
    stream (``test_scene_change_history``), never by the caller too."""

    core = _ActivityCore()
    manager = _SceneManager(**response)
    service = _service(core, manager)
    app = create_app(service=service, trusted_principal_adapter=FakeTrustedAdapter())
    body = _scene_body()
    body["request_context"]["source_label"] = "composer_ui"
    with TestClient(app) as client:
        client.post("/api/scene/transactions", json=body)
    assert core.activity == []
    assert len(manager.scene_requests) == 1


def test_a_command_the_manager_could_not_read_is_recorded_refused_by_headless() -> None:
    """No outcome of it reaches the Manager's stream, so Headless records the
    refusal it got back, as before OD-400."""

    refusal = {"code": "invalid_request", "message": "scene transaction request is invalid",
               "details": {"command": "invalid"}, "producer": "component_manager"}

    class Unread(_SceneManager):
        def commit_scene_transaction(self, request, *, trusted_principal_id, mesh_content_digest):
            super().commit_scene_transaction(
                request, trusted_principal_id=trusted_principal_id,
                mesh_content_digest=mesh_content_digest)
            return {"api_version": "manyforge.manager_command_response.v3",
                    "error": refusal, "ok": False}

    [record] = _commit(Unread(), _scene_request())
    assert (record["kind"], record["subject"], record["outcome"]) == (
        "scene_change", "remove_object", "refused")
    assert record["error"] == {"code": "invalid_request",
                               "message": "scene transaction request is invalid",
                               "details": {"command": "invalid"}}
    facts = record["scene_change"]
    assert {**facts, "duration_ms": 0} == {
        "scene_resource_id": "part.1", "source_label": "composer_ui",
        "principal_id": "operator.7", "duration_ms": 0, "scene_revision_id": None,
        "manager_start_id": None, "sequence": None}
    # Any other Manager refusal without an owner outcome came with its record
    # on the stream (the Manager records it): nothing here.
    other = {**refusal, "code": "manager_command_rejected", "details": {}}

    class Rejected(Unread):
        def commit_scene_transaction(self, request, **kwargs):
            return {**super().commit_scene_transaction(request, **kwargs), "error": other}

    assert _commit(Rejected(), _scene_request()) == []


def test_a_program_node_scene_commit_that_failed_is_left_to_its_node_attempt_event() -> None:
    manager = _SceneManager()
    manager.raised = HeadlessApiError("manager_unavailable", "Manager is not answering")
    request = _scene_request(program_node_evidence={
        "program_run_id": 7, "parent_program_cycle_claim_generation": 2,
        "node_instance_id": "remove_part", "node_attempt_id": 5,
        "owner_api": "scene.transaction"})
    assert _commit(manager, request) == []
    assert len(manager.scene_requests) == 1


def test_a_scene_commit_that_raised_is_recorded_failed_and_still_raises() -> None:
    core = _ActivityCore()
    manager = _SceneManager()
    manager.raised = HeadlessApiError("manager_unavailable", "Manager is not answering")
    service = _service(core, manager)
    with pytest.raises(HeadlessApiError) as raised:
        service.scene_commit(_scene_request(), TrustedAttribution())
    assert raised.value.code == "manager_unavailable"
    [record] = core.activity
    assert (record["outcome"], record["error"]) == (
        "failed", {"code": "manager_unavailable", "message": "Manager is not answering"})
    facts = record["scene_change"]
    assert facts["duration_ms"] >= 0
    # No Manager outcome: no Manager start, no sequence.
    assert {**facts, "duration_ms": 0} == {
        "scene_resource_id": "part.1", "source_label": "composer_ui", "principal_id": None,
        "duration_ms": 0, "scene_revision_id": None, "manager_start_id": None,
        "sequence": None}


def test_a_lost_scene_change_line_never_fails_the_commit() -> None:
    core = _ActivityCore()

    def fail(_record: dict[str, Any]) -> dict[str, Any]:
        raise RuntimeError("historian queue closed")

    core.record_activity = fail  # type: ignore[method-assign]
    manager = _SceneManager()
    manager.raised = HeadlessApiError("manager_unavailable", "Manager is not answering")
    service = _service(core, manager)
    with pytest.raises(HeadlessApiError) as raised:
        service.scene_commit(_scene_request(), TrustedAttribution())
    assert raised.value.code == "manager_unavailable"
