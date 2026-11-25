"""Program run events in History: the batch model and the record builder."""

from __future__ import annotations

from types import SimpleNamespace
from typing import Any

from pydantic import ValidationError
import pytest

from manyforge_headless_api.activity_history import SCHEMA_VERSION
from manyforge_headless_api.models import ProgramNodeAttemptBatchRequest
from manyforge_headless_api.program_run_history import (
    program_run_event_record,
    record_program_run_events,
    runner_records_lost_record,
)
from manyforge_headless_api.service import HeadlessService

_DIGEST = "sha256:" + "a" * 64
_START_KEY = "0123456789abcdef0123456789abcdef"


def _event(**changes: Any) -> dict[str, Any]:
    return {
        "program_run_id": 12, "event": "program_run_started", "outcome": "started",
        "occurred_at_ns": "5000000000", "occurred_at_wall": "2026-11-24T10:00:00.125+00:00",
        "error": None,
        "facts": {
            "scenario_id": "cell-pick", "program_revision": _DIGEST,
            "applied_record_digest": _DIGEST, "program_binding_id": _DIGEST,
            "request_key": _START_KEY, "source_label": "composer_ui",
        },
        **changes,
    }


def _attempt() -> dict[str, Any]:
    return {
        "program_run_id": 12, "node_instance_id": "pick_1",
        "node_id": "motion.move_to_pose", "node_attempt_id": 1,
        "started_at_wall": "2026-11-24T10:00:00.125+00:00",
        "finished_at_ns": "5000000000", "finished_at_wall": "2026-11-24T10:00:01.365+00:00",
        "duration_ms": 1240.0, "outcome": "succeeded", "error": None,
        "scene_operation": None, "scene_resource_id": None,
    }


def _batch(**changes: Any) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_node_attempt_batch.v2",
        "application_start_id": "0bad0000000000b1",
        "attempts": [_attempt()], "run_events": [_event()], **changes,
    }


def test_the_v2_batch_carries_attempts_and_run_events_within_one_bound() -> None:
    batch = ProgramNodeAttemptBatchRequest.model_validate(_batch())
    body = batch.model_dump(mode="json")
    assert "application_start_id" not in body
    assert body["run_events"] == [_event()]
    assert ProgramNodeAttemptBatchRequest.model_validate(_batch(attempts=[])).run_events
    assert ProgramNodeAttemptBatchRequest.model_validate(_batch(run_events=[])).attempts
    failed = _event(event="program_run_finished", outcome="failed", facts={},
                    error={"code": "program_tree_failed", "message": "No path"})
    assert ProgramNodeAttemptBatchRequest.model_validate(_batch(run_events=[failed]))


@pytest.mark.parametrize("changes", [
    {"schema_version": "manyforge.program_node_attempt_batch.v1"},
    {"attempts": [], "run_events": []},
    {"attempts": [_attempt()] * 32, "run_events": [_event()] * 33},
    {"run_events": [_event(outcome="failed")]},  # No error.
    {"run_events": [_event(error={"code": "x"})]},  # Error without failure.
    {"run_events": [_event(event="Program-Run")]},
    {"run_events": [_event(outcome="started2")]},
    {"run_events": [_event(program_run_id=0)]},
    {"run_events": [_event(occurred_at_ns="-1")]},
    {"run_events": [_event(occurred_at_ns=5)]},
    {"run_events": [_event(occurred_at_wall="today")]},
    {"run_events": [_event(facts={f"f{index}": index for index in range(30)})]},
    {"run_events": [_event(facts={"": 1})]},
    {"run_events": [{**_event(), "extra": 1}]},
])
def test_the_v2_batch_refuses_malformed_run_events(changes: dict[str, Any]) -> None:
    with pytest.raises(ValidationError):
        ProgramNodeAttemptBatchRequest.model_validate(_batch(**changes))


def test_a_run_start_names_the_program_and_its_caller() -> None:
    assert program_run_event_record(_event()) == {
        "schema_version": SCHEMA_VERSION,
        "kind": "program_run_event",
        "subject": "program_run_started",
        "action": None,
        "outcome": "started",
        "error": None,
        "components": [],
        "message": "Program run 12 started · scenario cell-pick · Program sha256:aaaaaaaaaaaa"
                   " · composer_ui",
        "run_event": {
            "scenario_id": "cell-pick", "program_revision": _DIGEST,
            "applied_record_digest": _DIGEST, "program_binding_id": _DIGEST,
            "program_run_id": 12, "occurred_at_ns": 5_000_000_000,
            "occurred_at_wall": "2026-11-24T10:00:00.125+00:00",
        },
        "caller": {"request_key": _START_KEY, "source_label": "composer_ui"},
    }


def test_each_run_end_has_its_outcome_and_only_a_failed_end_its_error() -> None:
    succeeded = program_run_event_record(_event(
        event="program_run_finished", outcome="succeeded",
        facts={"run_revision": 9, "end_reason": "completed"}))
    assert (succeeded["outcome"], succeeded["error"], "caller" in succeeded) == (
        "succeeded", None, False)
    assert succeeded["message"] == "Program run 12 succeeded"
    assert succeeded["run_event"] == {
        "run_revision": 9, "end_reason": "completed", "program_run_id": 12,
        "occurred_at_ns": 5_000_000_000, "occurred_at_wall": "2026-11-24T10:00:00.125+00:00"}
    failed = program_run_event_record(_event(
        event="program_run_finished", outcome="failed",
        error={"code": "motion_plan_failed", "message": "No path",
               "details": {"input_problem": "invalid"}},
        facts={"run_revision": 9, "end_reason": "cell_fault", "component_id": "robot.1"}))
    assert failed["error"] == {"code": "motion_plan_failed", "message": "No path",
                               "details": {"input_problem": "invalid"}}
    assert failed["message"] == (
        "Program run 12 failed: motion_plan_failed: No path · cell fault"
        " · component id robot.1")
    stopped = program_run_event_record(_event(
        event="program_run_finished", outcome="stopped",
        facts={"end_reason": "stopped", "stopped_by": "rule", "rule_id": "door_open"}))
    assert stopped["error"] is None
    assert stopped["message"] == "Program run 12 stopped · stopped by rule · rule id door_open"


def test_an_audit_event_keeps_its_facts_as_bounded_scalars() -> None:
    record = program_run_event_record(_event(
        event="program_operator_choice_answered", outcome="recorded",
        facts={"event_revision": 4, "node_instance_id": "ask_1", "node_attempt_id": 2,
               "option_id": "retry", "label": "Retry", "result": None,
               "group_ids": ["arm", "gripper"], "large": 2**60, "text": "x" * 5000,
               "request_key": _START_KEY, "source_label": None}))
    assert (record["subject"], record["outcome"], record["error"]) == (
        "program_operator_choice_answered", "recorded", None)
    assert record["message"] == (
        "Program run 12: program operator choice answered · node ask_1 · attempt 2"
        " · option retry")
    assert record["caller"] == {"request_key": _START_KEY}
    facts = record["run_event"]
    assert facts["group_ids"] == '["arm","gripper"]'
    assert facts["large"] == str(2**60)
    assert len(facts["text"]) == 1024
    assert facts["result"] is None and facts["source_label"] is None
    assert "request_key" not in facts
    assert all(value is None or isinstance(value, (bool, int, float, str))
               for value in facts.values())


def test_run_events_are_handed_to_the_historian_one_record_each() -> None:
    records: list[dict[str, Any]] = []

    class _Core:
        def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
            records.append(record)
            return {"recorded": len(records) == 1}

    assert record_program_run_events(_Core(), [_event(), _event(program_run_id=13)]) == 1
    assert [record["run_event"]["program_run_id"] for record in records] == [12, 13]


_LOSS = {"count": 3, "first_lost_wall": "2026-11-24T10:00:00.125000+00:00",
         "last_lost_wall": "2026-11-24T10:00:02.500000+00:00"}


@pytest.mark.parametrize("loss", [
    {**_LOSS, "count": 0}, {**_LOSS, "count": "3"}, {**_LOSS, "first_lost_wall": "today"},
    {**_LOSS, "extra": 1}, {"count": 3},
])
def test_the_v2_batch_refuses_a_malformed_loss_count(loss: dict[str, Any]) -> None:
    assert ProgramNodeAttemptBatchRequest.model_validate(
        _batch(lost_since_last_batch=None)).lost_since_last_batch is None
    with pytest.raises(ValidationError):
        ProgramNodeAttemptBatchRequest.model_validate(_batch(lost_since_last_batch=loss))


def test_the_runners_losses_are_one_records_lost_record() -> None:
    assert runner_records_lost_record(_LOSS) == {
        "schema_version": SCHEMA_VERSION,
        "kind": "records_lost",
        "subject": "program_runner_history_publisher",
        "action": None,
        "outcome": "lost",
        "error": None,
        "components": [],
        "message": "Program Runner history lost 3 records between"
                   " 2026-11-24T10:00:00.125000+00:00 and 2026-11-24T10:00:02.500000+00:00",
        "records_lost": {
            "lost_count": 3,
            "first_lost_wall": "2026-11-24T10:00:00.125000+00:00",
            "last_lost_wall": "2026-11-24T10:00:02.500000+00:00",
        },
    }
    single = runner_records_lost_record(
        {**_LOSS, "count": 1, "last_lost_wall": _LOSS["first_lost_wall"]})
    assert single["message"] == (
        "Program Runner history lost 1 record at 2026-11-24T10:00:00.125000+00:00")


def test_a_batch_records_its_losses_once_and_does_not_count_them() -> None:
    records: list[dict[str, Any]] = []

    class _Core:
        def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
            records.append(record)
            return {"recorded": True}

    service = SimpleNamespace(core=_Core())
    with_loss = ProgramNodeAttemptBatchRequest.model_validate(
        _batch(lost_since_last_batch=_LOSS))
    without = ProgramNodeAttemptBatchRequest.model_validate(_batch())
    assert HeadlessService.record_program_node_attempts(service, with_loss)["recorded"] == 2
    assert [record["kind"] for record in records] == [
        "program_node_attempt", "program_run_event", "records_lost"]
    assert records[2]["records_lost"]["lost_count"] == 3
    records.clear()
    assert HeadlessService.record_program_node_attempts(service, without)["recorded"] == 2
    assert [record["kind"] for record in records] == ["program_node_attempt", "program_run_event"]
