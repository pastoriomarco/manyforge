"""Unit tests for EventRecorder — run lifecycle, semantic events, JSONL sink."""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import pytest

from manyforge_behavior.kernel_client import ExecutionState, ExecutionSnapshot, ExecutionStatus
from manyforge_composer.backend import event_recorder as event_recorder_module
from manyforge_composer.backend.event_recorder import EventRecorder


# ---------------------------------------------------------------------------
# Minimal stubs that quack like ComposerState / CycleManager / Intervention
# ---------------------------------------------------------------------------

@dataclass
class _StubStatus:
    state: ExecutionState = ExecutionState.IDLE
    execution_id: int = 0
    progress: float = 0.0
    active_move_id: str = ""


@dataclass
class _StubSnapshot:
    error: Optional[str] = None
    joint_positions_rad: list[float] = field(default_factory=list)


@dataclass
class _StubIntervention:
    session_id: str = "sess-001"
    session_kind: str = "manual_intervention"
    authority_source: str = "human"
    principal_id: str = "human:test-operator"
    grant_type: str = "human_approved"
    parent_run_id: Optional[str] = None
    entry_state: str = "idle"
    resume_target: str = "stay_idle"
    trigger_reason: str = "manual_request"
    trigger_node_instance: Optional[str] = None
    approval_mode: str = "human_approved"
    artifact_origin: str = "saved_program"
    events: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class _StubDelegatedRecoveryRequest:
    request_id: str = "req-001"
    parent_run_id: Optional[str] = "run-parent-001"
    requested_from_state: str = "running"
    trigger_node_instance: str = "move_to_pick"
    trigger_node_id: str = "move_manipulator_action"
    trigger_reason: str = "execute_failed"
    delegate_policy: str = "llm_allowed"
    allowed_authority_sources: tuple[str, ...] = ("human", "llm")
    allowed_actions: tuple[str, ...] = ("intervention", "runtime_override", "draft_patch")
    allowed_resume_targets: tuple[str, ...] = ("resume", "stop", "reset")
    default_resume_target: str = "resume"
    runtime_override_policy: str = "llm_allowed"
    persistent_edit_policy: str = "human_approval"
    requested_at_ns: int = field(default_factory=time.monotonic_ns)


class _StubCycleManager:
    def __init__(
        self,
        state_value: str = "idle",
        error: Optional[str] = None,
        intervention: Optional[_StubIntervention] = None,
        kernel_status: Optional[_StubStatus] = None,
        kernel_snapshot: Optional[_StubSnapshot] = None,
    ):
        self.state = type("S", (), {"value": state_value})()
        self.error = error
        self.intervention = intervention
        self._status = kernel_status or _StubStatus()
        self._snapshot = kernel_snapshot or _StubSnapshot()

    def get_status(self):
        return self._status

    def get_execution_snapshot(self):
        return self._snapshot

    def get_node_statuses(self) -> dict[str, str]:
        return {}


class _StubKernel:
    def __init__(self, status=None, snapshot=None):
        self._status = status or _StubStatus()
        self._snapshot = snapshot or _StubSnapshot()

    def get_status(self):
        return self._status

    def get_execution_snapshot(self):
        return self._snapshot


class _StubState:
    def __init__(self, cycle_manager=None, kernel=None):
        self.cycle_manager = cycle_manager
        self._kernel = kernel


def _memory_events(rec: EventRecorder) -> list[dict[str, Any]]:
    return [rec._decode_stored_event(event) for event in rec._events]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestRunLifecycle:
    """Run start / finish and run_id tracking."""

    def test_no_run_initially(self):
        rec = EventRecorder()
        assert rec.current_run_id is None
        assert rec.last_run_id is None

    def test_idle_to_running_starts_run(self):
        rec = EventRecorder()
        # First observe idle
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        assert rec.current_run_id is None

        # Transition to running
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        assert rec.current_run_id is not None
        assert rec.current_run_id.startswith("run-")

    def test_running_to_idle_finishes_run(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        assert rec.current_run_id is None
        assert rec.last_run_id == run_id

    def test_multiple_runs_increment_counter(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run1 = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run2 = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        assert run1 != run2
        assert rec.last_run_id == run2


class TestSemanticEvents:
    """Kernel state transitions produce the correct semantic event types."""

    def _observe_kernel_transition(
        self,
        rec: EventRecorder,
        from_state: ExecutionState,
        to_state: ExecutionState,
        execution_id: int = 1,
        move_id: str = "m1",
    ) -> list[dict[str, Any]]:
        """Helper: observe two kernel states and return new events."""
        idx_before = rec._event_index

        status_from = _StubStatus(state=from_state, execution_id=execution_id, active_move_id=move_id)
        snap = _StubSnapshot()
        kernel = _StubKernel(status=status_from, snapshot=snap)
        cm = _StubCycleManager("running", kernel_status=status_from, kernel_snapshot=snap)
        rec.observe(_StubState(cycle_manager=cm, kernel=kernel))

        status_to = _StubStatus(state=to_state, execution_id=execution_id, active_move_id=move_id)
        kernel_to = _StubKernel(status=status_to, snapshot=snap)
        cm_to = _StubCycleManager("running", kernel_status=status_to, kernel_snapshot=snap)
        rec.observe(_StubState(cycle_manager=cm_to, kernel=kernel_to))

        return [e for e in _memory_events(rec) if e["event_index"] > idx_before]

    def test_planning_to_validating_emits_plan_completed(self):
        rec = EventRecorder()
        # Bootstrap cycle state
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.PLANNING, ExecutionState.VALIDATING,
        )
        semantic_types = [e["type"] for e in events]
        assert "plan_completed" in semantic_types

    def test_validating_to_executing_emits_execute_started(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.VALIDATING, ExecutionState.EXECUTING,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_started" in semantic_types

    def test_executing_to_completed(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.EXECUTING, ExecutionState.COMPLETED,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_completed" in semantic_types

    def test_executing_to_failed(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.EXECUTING, ExecutionState.FAILED,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_failed" in semantic_types

    def test_stopping_to_intervention_required(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.STOPPING, ExecutionState.INTERVENTION_REQUIRED,
        )
        intervention_events = [e for e in events if e["type"] == "intervention_required"]
        assert intervention_events
        assert intervention_events[-1]["severity"] == "ERROR"

    def test_executing_to_paused(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.EXECUTING, ExecutionState.PAUSED,
        )
        semantic_types = [e["type"] for e in events]
        assert "pause_completed" in semantic_types


class TestJSONLSink:
    """JSONL file persistence."""

    def test_events_written_to_jsonl(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

        jsonl_path = tmp_path / f"{run_id}.events.jsonl"
        assert jsonl_path.exists()

        lines = jsonl_path.read_text().strip().splitlines()
        assert len(lines) >= 2  # at least cycle_state_changed events
        for line in lines:
            event = json.loads(line)
            assert "event_index" in event
            assert "timestamp_ns" in event
            assert "type" in event
            assert event["run_id"] == run_id

    def test_no_trace_dir_no_file(self):
        rec = EventRecorder(trace_dir=None)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        # Should not raise
        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

    def test_history_loads_from_jsonl(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        # Create a fresh recorder pointing at the same trace dir
        rec2 = EventRecorder(trace_dir=tmp_path)
        result = rec2.history(run_id=run_id)
        assert result["run_id"] == run_id
        assert len(result["events"]) > 0

    def test_events_include_operational_envelope_fields(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        event = _memory_events(rec)[-1]
        assert event["sequence"] == event["event_index"]
        assert isinstance(event["timestamp"], str)
        assert isinstance(event["timestamp_wall"], str)
        assert event["schema_version"] == "manyforge.execution_event.v1"
        assert event["trace_source"] == "event_recorder"
        assert event["severity"] == "INFO"
        assert isinstance(event["component"], str)
        assert isinstance(event["message"], str)
        assert event["event"] == event["type"]
        assert event["program_run_id"] == event["run_id"]

    def test_export_ros2_medkit_trace_normalizes_records(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec.record_interrupt_event(
            action="latched",
            interrupt_id="int-001",
            source="operator_pause",
            policy_id="operator_pause_hold",
            preemption_mode="hold_cycle",
            disposition="resumable_hold",
            active_node_id="timer",
            run_id=run_id,
            details={"active_node_name": "wait_here"},
        )

        exported = rec.export_ros2_medkit_trace(run_id=run_id, exported_at_ns=123456789)
        assert exported["schema_version"] == "manyforge.ros2_medkit_trace.v1"
        assert exported["trace_source"] == "manyforge_composer"
        assert exported["exported_at_ns"] == 123456789
        assert exported["run_id"] == run_id
        assert exported["program_run_id"] == run_id
        assert exported["records"]
        interrupt_records = [
            record for record in exported["records"]
            if record["event_type"] == "interrupt_latched"
        ]
        assert interrupt_records
        record = interrupt_records[-1]
        assert record["timeline_id"] == run_id
        assert record["attributes"]["interrupt_id"] == "int-001"
        assert record["attributes"]["preemption_mode"] == "hold_cycle"

    def test_failure_archive_written_for_execution_failure(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        executing = _StubStatus(
            state=ExecutionState.EXECUTING,
            execution_id=7,
            active_move_id="move_pick",
            progress=0.25,
        )
        rec.observe(
            _StubState(
                cycle_manager=_StubCycleManager(
                    "running",
                    kernel_status=executing,
                    kernel_snapshot=_StubSnapshot(),
                ),
                kernel=_StubKernel(status=executing, snapshot=_StubSnapshot()),
            )
        )

        failed = _StubStatus(
            state=ExecutionState.FAILED,
            execution_id=7,
            active_move_id="move_pick",
            progress=0.25,
        )
        failed_snapshot = _StubSnapshot(error="tracking tolerance violation")
        rec.observe(
            _StubState(
                cycle_manager=_StubCycleManager(
                    "running",
                    kernel_status=failed,
                    kernel_snapshot=failed_snapshot,
                ),
                kernel=_StubKernel(status=failed, snapshot=failed_snapshot),
            )
        )
        assert rec.wait_for_failure_archives_idle()

        archive_root = tmp_path / "failure-archives"
        archives = sorted(path for path in archive_root.iterdir() if path.is_dir())
        assert archives
        manifest = json.loads((archives[0] / "manifest.json").read_text())
        assert manifest["schema_version"] == "manyforge.failure_archive.v1"
        assert manifest["trigger"]["type"] == "execute_failed"
        assert manifest["trigger"]["run_id"] == run_id
        assert manifest["trigger"]["execution_id"] == 7
        trigger = json.loads((archives[0] / "trigger_event.json").read_text())
        assert trigger["details"]["error"] == "tracking tolerance violation"
        assert (archives[0] / "recent_events.jsonl").read_text().strip()

        status = rec.failure_archive_status()
        assert status["enabled"] is True
        assert status["archive_count"] == 1
        assert status["last_archive_id"] == archives[0].name

    def test_failure_archive_cleanup_uses_age_and_fifo_budget(self, tmp_path):
        archive_root = tmp_path / "archives"
        archive_root.mkdir()
        old_by_age = archive_root / "incident-old-by-age"
        old_by_age.mkdir()
        (old_by_age / "payload.bin").write_bytes(b"x" * 128)
        fifo_oldest = archive_root / "incident-fifo-oldest"
        fifo_oldest.mkdir()
        (fifo_oldest / "payload.bin").write_bytes(b"x" * 15000)
        fifo_newer = archive_root / "incident-fifo-newer"
        fifo_newer.mkdir()
        (fifo_newer / "payload.bin").write_bytes(b"x" * 4000)

        now = time.time()
        old_time = now - 3600.0
        os.utime(old_by_age, (old_time, old_time))
        os.utime(fifo_oldest, (now - 300.0, now - 300.0))
        os.utime(fifo_newer, (now - 60.0, now - 60.0))

        rec = EventRecorder(
            trace_dir=tmp_path / "traces",
            failure_archive_dir=archive_root,
            failure_archive_max_age_s=600.0,
            failure_archive_max_bytes=20000,
        )
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        rec._record_event(
            event_type="cycle_error",
            run_id=rec.current_run_id,
            details={"error": "simulated incident"},
        )
        assert rec.wait_for_failure_archives_idle()

        assert not old_by_age.exists()
        assert not fifo_oldest.exists()
        assert fifo_newer.exists()
        assert any(path.name.startswith("incident-") for path in archive_root.iterdir())
        cleanup = rec.failure_archive_status()["last_cleanup"]
        assert cleanup["deleted_count"] >= 2
        assert "max_age_s" in cleanup["reason"]
        assert "max_bytes" in cleanup["reason"]

    def test_failure_archive_queue_pressure_is_reported(self, tmp_path, monkeypatch):
        monkeypatch.setattr(event_recorder_module, "_FAILURE_ARCHIVE_QUEUE_SIZE", 1)

        def slow_archive(self, job):
            time.sleep(0.2)
            archive_path = self._failure_archive_dir / f"incident-test-{job.event['event_index']}"
            archive_path.mkdir(parents=True, exist_ok=True)
            return archive_path

        monkeypatch.setattr(EventRecorder, "_write_failure_archive", slow_archive)
        rec = EventRecorder(trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        for i in range(8):
            rec._record_event(
                event_type="cycle_error",
                run_id=run_id,
                details={"error": f"simulated incident {i}"},
            )

        status = rec.failure_archive_status()
        assert status["queue_capacity"] == 1
        assert status["dropped_archives"] > 0
        assert status["last_archive_error"] == "failure archive queue full"
        assert rec.wait_for_failure_archives_idle(timeout_s=2.0)

    def test_native_streaming_events_project_into_history_once(self):
        rec = EventRecorder()
        native_events = [
            {
                "event_index": 1,
                "type": "streaming_session_started",
                "timestamp_ns": 101,
                "execution_id": 42,
                "details": {"session_id": 7, "input_mode": "cartesian"},
            },
            {
                "event_index": 2,
                "type": "streaming_target_dispatched",
                "timestamp_ns": 102,
                "execution_id": 42,
                "details": {"session_id": 7},
            },
            {
                "event_index": 3,
                "type": "streaming_target_rejected",
                "timestamp_ns": 103,
                "execution_id": 42,
                "error": {"code": "ik_failed", "message": "IK failed"},
                "details": {"session_id": 7, "source_sequence": 12},
            },
        ]

        rec.record_native_kernel_events(native_events)
        rec.record_native_kernel_events(native_events)

        events = _memory_events(rec)
        event_types = [event["type"] for event in events]
        assert event_types == ["streaming_session_started", "streaming_target_rejected"]
        assert events[0]["execution_mode"] == "direct_control"
        assert events[0]["component"] == "manyforge.direct_control"
        assert events[0]["details"]["native_kernel_event_index"] == 1
        assert events[1]["severity"] == "WARN"
        assert events[1]["details"]["error_code"] == "ik_failed"
        assert events[1]["details"]["error"] == "IK failed"
        assert events[1]["details"]["native_kernel_event_index"] == 3

    def test_history_filters_by_severity_component_and_type(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec.record_interrupt_event(
            action="latched",
            interrupt_id="int-002",
            source="operator_pause",
            policy_id="operator_pause_hold",
            preemption_mode="hold_cycle",
            disposition="resumable_hold",
            run_id=run_id,
            details={},
        )
        rec._record_event(
            event_type="cycle_error",
            run_id=run_id,
            details={"error": "boom"},
        )

        result = rec.history(
            run_id=run_id,
            min_severity="ERROR",
            components=["manyforge.cycle"],
            event_types=["cycle_error"],
        )
        assert len(result["events"]) == 1
        assert result["events"][0]["type"] == "cycle_error"


class TestBoundedRetention:
    """Ring buffer eviction."""

    def test_max_events_respected(self):
        rec = EventRecorder(max_events=10)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        # Generate many transitions
        for i in range(20):
            state_val = "running" if i % 2 == 0 else "stopped"
            rec.observe(_StubState(cycle_manager=_StubCycleManager(state_val)))

        assert len(rec._events) <= 10

    def test_event_index_monotonically_increases(self):
        rec = EventRecorder(max_events=5)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        for i in range(10):
            state_val = "running" if i % 2 == 0 else "stopped"
            rec.observe(_StubState(cycle_manager=_StubCycleManager(state_val)))

        indices = [e["event_index"] for e in _memory_events(rec)]
        assert indices == sorted(indices)
        assert len(set(indices)) == len(indices)  # all unique


class TestInterventionProjection:
    """Intervention session events projected into the unified stream."""

    def test_intervention_events_appear_in_stream(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        intervention = _StubIntervention(
            session_id="sess-abc",
            parent_run_id="run-parent-001",
        )
        intervention.events.append({
            "event_index": 1,
            "timestamp_ns": time.monotonic_ns(),
            "type": "session_started",
            "details": {},
        })

        cm = _StubCycleManager(
            "direct_control",
            intervention=intervention,
        )
        rec.observe(_StubState(cycle_manager=cm))

        matching = [
            e for e in _memory_events(rec)
            if e["type"] == "intervention_session_started"
        ]
        assert len(matching) == 1
        assert matching[0]["session_id"] == "sess-abc"
        assert matching[0]["session_kind"] == "manual_intervention"
        assert matching[0]["authority_source"] == "human"
        assert matching[0]["execution_mode"] == "intervention"

    def test_intervention_events_include_principal_id_and_grant_type(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        intervention = _StubIntervention(
            session_id="sess-pid",
            principal_id="human:operator-1",
            grant_type="human_approved",
            parent_run_id="run-parent-002",
        )
        intervention.events.append({
            "event_index": 1,
            "timestamp_ns": time.monotonic_ns(),
            "type": "session_started",
            "details": {},
        })

        cm = _StubCycleManager(
            "direct_control",
            intervention=intervention,
        )
        rec.observe(_StubState(cycle_manager=cm))

        matching = [
            e for e in _memory_events(rec)
            if e["type"] == "intervention_session_started"
        ]
        assert len(matching) == 1
        assert matching[0]["principal_id"] == "human:operator-1"
        assert matching[0]["grant_type"] == "human_approved"

    def test_duplicate_intervention_events_not_re_projected(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        intervention = _StubIntervention(session_id="sess-dup")
        intervention.events.append({
            "event_index": 1,
            "timestamp_ns": time.monotonic_ns(),
            "type": "session_started",
            "details": {},
        })

        cm = _StubCycleManager("direct_control", intervention=intervention)
        rec.observe(_StubState(cycle_manager=cm))
        count_before = len(rec._events)

        # Observe again with same events — should not duplicate
        rec.observe(_StubState(cycle_manager=cm))
        count_after = len(rec._events)
        assert count_after == count_before

    def test_delegated_recovery_request_event_is_recorded(self):
        rec = EventRecorder()
        request = _StubDelegatedRecoveryRequest()

        rec.record_delegated_recovery_requested(request)

        matching = [
            event for event in _memory_events(rec)
            if event["type"] == "delegated_recovery_requested"
        ]
        assert len(matching) == 1
        assert matching[0]["run_id"] == "run-parent-001"
        assert matching[0]["session_kind"] == "delegated_recovery"
        assert matching[0]["authority_source"] == "program_node"
        assert matching[0]["trigger_node_instance"] == "move_to_pick"

    def test_interrupt_event_is_recorded_with_stable_fields(self):
        rec = EventRecorder()

        rec.record_interrupt_event(
            action="detected",
            interrupt_id="int-001",
            source="operator_pause",
            policy_id="pause_hold",
            preemption_mode="hold_cycle",
            disposition="resumable_hold",
            active_node_id="move_to_pick",
            active_move_id="pick_approach",
            run_id="run-123",
            details={"latched": True},
        )

        matching = [
            event for event in _memory_events(rec)
            if event["type"] == "interrupt_detected"
        ]
        assert len(matching) == 1
        event = matching[0]
        assert event["component"] == "manyforge.interrupt"
        assert event["details"]["interrupt_id"] == "int-001"
        assert event["details"]["source"] == "operator_pause"
        assert event["details"]["policy_id"] == "pause_hold"
        assert event["details"]["preemption_mode"] == "hold_cycle"
        assert event["details"]["disposition"] == "resumable_hold"
        assert event["details"]["active_node_id"] == "move_to_pick"


class TestGetEventsSince:
    """Incremental event polling."""

    def test_returns_events_after_index(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        idx = rec._event_index
        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

        new_events = rec.get_events_since(idx)
        assert len(new_events) > 0
        assert all(e["event_index"] > idx for e in new_events)

    def test_returns_empty_when_caught_up(self):
        rec = EventRecorder()
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        idx = rec._event_index
        assert rec.get_events_since(idx) == []
