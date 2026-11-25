"""Automatic motion-trace capture: cause, hand-off, worker and History row."""

from __future__ import annotations

import copy
import hashlib
import logging
import threading
from typing import Any

import rfc8785

from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.motion_trace_history import (
    MAXIMUM_PENDING_CAPTURES,
    MOTION_TRACE_ALLOWANCE_BYTES,
    MotionTraceCapture,
    motion_trace_cause,
    motion_trace_record,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.paged_event_stream import FollowedEventSource

_START = "3f9a0000000000a1"
_FILE = "trace-" + "ab" * 32 + ".json"
_METADATA = {"schema_version": "manyforge.motion_trace_file.v2", "file": _FILE,
             "bytes": 1234, "sha256": "sha256:" + "ab" * 32, "status": "complete"}


def _participant(component_id="arm_1", *, state="failed", kind="motion", stop=None):
    return {"controllable_component_id": component_id, "payload_kind": kind,
            "state": state, "stop": stop, "terminal_error": None}


def _event(sequence=1, *, state="failed", participants=None, record_kind="state_observed",
           operation_id=7, stop_event_id=None, program_run_id=3):
    payload = {
        "api_version": "manyforge.operation_journal_record.v10",
        "intent": {},
        "operation_state": {
            "operation_id": operation_id,
            "operation_revision": 4,
            "participants": [_participant()] if participants is None else participants,
            "program_binding_id": "binding.1",
            "program_run_id": program_run_id,
            "state": state,
            "stop_event_id": stop_event_id,
        },
        "record_kind": record_kind,
        "result": None,
        "runtime_scale_status": None,
    }
    canonical = rfc8785.dumps(payload)
    return {"occurred_at_ns": 100 + sequence,
            "occurred_at_wall": f"2026-11-24T10:00:00.00000000{sequence}Z",
            "operation_id": operation_id, "operation_revision": 4, "payload": payload,
            "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
            "record_kind": record_kind, "request_key": None, "sequence": sequence}


class _Core:
    def __init__(self) -> None:
        self.records: list[dict[str, Any]] = []
        self.recorded = threading.Event()
        self.completed = threading.Event()

    def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
        self.records.append(record)
        self.recorded.set()
        return {"recorded": True}

    # Event-pump surface.
    def refresh_alarm_sources(self):
        return {"refreshed": True}

    def ingest_operation_event(self, **_kwargs):
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **_kwargs):
        self.completed.set()
        return {"status": "complete"}

    def observe_runtime_fault_conditions(self, **_kwargs):
        return {"status": "observed", "changed": False}

    def mark_operation_event_source_unknown(self, **_kwargs):
        return {"status": "marked_unknown"}

    def observe_operation_table(self, **_kwargs):
        return {"changed": False}

    def observe_component_faults(self, **_kwargs):
        return {"status": "observed", "changed": False}


class _Execution:
    """Answers the export; `gate` holds it, as a slow host write would."""

    def __init__(self, *, error: HeadlessApiError | None = None) -> None:
        self.error = error
        self.calls: list[int] = []
        self.gate = threading.Event()
        self.gate.set()
        self.entered = threading.Event()

    def capture_motion_trace_file(self, allowance_bytes: int) -> dict[str, Any]:
        self.calls.append(allowance_bytes)
        self.entered.set()
        assert self.gate.wait(timeout=5)
        if self.error is not None:
            raise self.error
        return dict(_METADATA)


def _wait_records(core: _Core, count: int) -> None:
    for _ in range(500):
        if len(core.records) >= count:
            return
        core.recorded.wait(timeout=0.01)
        core.recorded.clear()
    raise AssertionError(f"expected {count} records, got {len(core.records)}")


def test_record_names_the_file_and_its_cause() -> None:
    record = motion_trace_record("arm_1", "succeeded", None, {
        "cause": "operation_failed", "operation_id": 7, "operation_state": "failed",
        "stop_event_id": None, "file": _FILE, "bytes": 1234, "status": "complete",
        "request_count": 1,
    })
    assert record == {
        "schema_version": "manyforge.activity_event.v2",
        "kind": "motion_trace_captured",
        "subject": "arm_1",
        "action": None,
        "outcome": "succeeded",
        "error": None,
        "components": [],
        "message": f"Motion trace of arm_1 saved: {_FILE} · operation 7 failed",
        "motion_trace": {"cause": "operation_failed", "operation_id": 7,
                         "operation_state": "failed", "file": _FILE, "bytes": 1234,
                         "status": "complete", "request_count": 1},
    }


def test_failed_record_carries_the_typed_error() -> None:
    record = motion_trace_record(
        "arm_1", "failed", {"code": "motion_trace_store_full", "message": "store is full"},
        {"cause": "stop_escalated", "stop_event_id": 9, "request_count": 3},
    )
    assert record["outcome"] == "failed"
    assert record["error"] == {"code": "motion_trace_store_full", "message": "store is full"}
    assert record["message"] == (
        "Motion trace of arm_1 failed: motion_trace_store_full: store is full"
        " · Stop 9 escalated · 3 requests"
    )
    assert 1 <= len(record["motion_trace"]) <= 16


def test_cause_is_a_failed_motion_participant_or_an_escalated_stop() -> None:
    subject, facts = motion_trace_cause(_event())
    assert subject == "arm_1"
    assert facts == {"cause": "operation_failed", "operation_id": 7, "operation_revision": 4,
                     "operation_state": "failed", "stop_event_id": None, "program_run_id": 3}
    escalated = _event(state="stopped", stop_event_id=9, participants=[
        _participant("gripper", state="stopped", kind="capability_command"),
        _participant("arm_2", state="stopped",
                     stop={"escalated": True, "stop_event_id": 9}),
    ])
    subject, facts = motion_trace_cause(escalated)
    assert (subject, facts["cause"], facts["stop_event_id"]) == ("arm_2", "stop_escalated", 9)
    # Interrupted and intervention-required operations are failures too.
    assert motion_trace_cause(_event(state="interrupted"))[1]["cause"] == "operation_failed"


def test_no_cause_for_success_plain_stop_capability_only_or_other_rows() -> None:
    assert motion_trace_cause(_event(state="completed")) is None
    assert motion_trace_cause(_event(state="stopped", participants=[
        _participant(state="stopped", stop={"escalated": False, "stop_event_id": 2})])) is None
    assert motion_trace_cause(_event(participants=[
        _participant("gripper", kind="capability_command")])) is None
    # A failure observed before its terminal row (still stopping) waits for it.
    assert motion_trace_cause(_event(state="stopping")) is None
    assert motion_trace_cause(_event(record_kind="control_accepted")) is None


def test_capture_writes_once_and_records_the_file() -> None:
    core, execution = _Core(), _Execution()
    capture = MotionTraceCapture(core, execution)
    capture.start()
    try:
        capture.observe(_event())
        _wait_records(core, 1)
        # The same operation's repeated terminal row is not captured again.
        capture.observe(_event(sequence=2))
    finally:
        capture.shutdown()
    assert execution.calls == [MOTION_TRACE_ALLOWANCE_BYTES]
    (record,) = core.records
    assert record["outcome"] == "succeeded"
    assert record["motion_trace"]["file"] == _FILE
    assert record["motion_trace"]["cause"] == "operation_failed"


def test_observe_never_waits_and_coalesces_per_robot() -> None:
    """The pump's hand-off returns while an export is held by the host; one
    request at most waits per robot, later ones are counted into it."""
    core, execution = _Core(), _Execution()
    execution.gate.clear()
    capture = MotionTraceCapture(core, execution)
    capture.start()
    try:
        capture.observe(_event(operation_id=1))
        assert execution.entered.wait(timeout=5)
        returned = threading.Event()

        def hand_off():
            for operation_id in (2, 3, 4):
                capture.observe(_event(operation_id=operation_id))
            capture.observe(_event(operation_id=5, participants=[_participant("arm_2")]))
            returned.set()

        threading.Thread(target=hand_off).start()
        assert returned.wait(timeout=1)
        execution.gate.set()
        _wait_records(core, 3)
    finally:
        capture.shutdown()
    assert len(execution.calls) == 3
    by_subject = [(r["subject"], r["motion_trace"]["operation_id"],
                   r["motion_trace"]["request_count"]) for r in core.records]
    assert by_subject == [("arm_1", 1, 1), ("arm_1", 2, 3), ("arm_2", 5, 1)]


def test_pending_robots_are_bounded() -> None:
    core, execution = _Core(), _Execution()
    capture = MotionTraceCapture(core, execution)  # not started: nothing runs
    for index in range(MAXIMUM_PENDING_CAPTURES + 2):
        capture.request(f"arm_{index}", {"cause": "operation_failed", "operation_id": index})
    assert capture.dropped == 2
    assert execution.calls == []


def test_failed_capture_is_recorded_and_logged_once(caplog) -> None:
    core = _Core()
    execution = _Execution(error=HeadlessApiError("motion_trace_store_full", "store is full"))
    capture = MotionTraceCapture(core, execution)
    capture.start()
    caplog.set_level(logging.WARNING, logger="manyforge_headless_api.motion_trace_history")
    try:
        capture.observe(_event(operation_id=1))
        _wait_records(core, 1)
        capture.observe(_event(operation_id=2))
        _wait_records(core, 2)
    finally:
        capture.shutdown()
    assert [r["outcome"] for r in core.records] == ["failed", "failed"]
    assert core.records[0]["error"]["code"] == "motion_trace_store_full"
    assert capture.failures == 2
    warnings = [r for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warnings) == 1


class _PumpExecution(FollowedEventSource, _Execution):
    """The pump's event source (its paged stream) and the capture's host."""

    def __init__(self, event: dict[str, Any]) -> None:
        super().__init__()
        self.event = event

    def identity(self) -> dict[str, Any]:
        return {"api_version": "manyforge.execution_command_response.v6",
                "command": "identity", "ok": True,
                "identity": {"application_start_id": _START}}

    def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        events = [] if after_sequence else [copy.deepcopy(self.event)]
        return {
            "execution_owner_ready": True,
            "runtime_faults": {"operation_journal_write_fault": None,
                               "program_runner_loss": None, "kernel_internal_fault": None},
            "operation_progress": [], "admission_generation": 1, "events": events,
            "has_more": False, "next_after_sequence": after_sequence or 1,
            "schema_version": "manyforge.execution_event_page.v3",
        }


def test_event_pump_hands_a_failed_operation_to_the_capture(monkeypatch, tmp_path) -> None:
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    core = _Core()
    execution = _PumpExecution(_event())
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_START, poll_seconds=0.01,
        page_limit=1, motion_trace_source=execution,
    )
    pump.start()
    try:
        assert core.completed.wait(timeout=2)
        _wait_records(core, 1)
    finally:
        pump.shutdown()
    assert pump.last_error is None
    assert core.records[0]["kind"] == "motion_trace_captured"
    assert core.records[0]["subject"] == "arm_1"


def test_event_pump_without_a_trace_store_never_captures(monkeypatch) -> None:
    monkeypatch.delenv("MANYFORGE_MOTION_TRACE_DIR", raising=False)
    core = _Core()
    execution = _PumpExecution(_event())
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_START, poll_seconds=0.01, page_limit=1,
        motion_trace_source=execution,
    )
    pump.start()
    try:
        assert core.completed.wait(timeout=2)
    finally:
        pump.shutdown()
    assert execution.calls == []
    assert core.records == []
