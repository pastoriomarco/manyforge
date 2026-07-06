"""Unit tests for EventRecorder — run lifecycle, semantic events, JSONL sink."""

from __future__ import annotations

import json
import os
import threading
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


class _RaceWideningEventRecorder(EventRecorder):
    def _handle_cycle_state_transition(self, *args, **kwargs) -> None:
        time.sleep(0.0001)
        super()._handle_cycle_state_transition(*args, **kwargs)

    def _handle_kernel_transition(self, *args, **kwargs) -> None:
        time.sleep(0.0001)
        super()._handle_kernel_transition(*args, **kwargs)

    def _handle_node_status_changes(self, *args, **kwargs) -> None:
        time.sleep(0.0001)
        super()._handle_node_status_changes(*args, **kwargs)


def _memory_events(rec: EventRecorder) -> list[dict[str, Any]]:
    return [rec._decode_stored_event(event) for event in rec._events]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestRunLifecycle:
    """Run start / finish and run_id tracking."""

    def test_no_run_initially(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        assert rec.current_run_id is None
        assert rec.last_run_id is None

    def test_idle_to_running_starts_run(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        # First observe idle
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        assert rec.current_run_id is None

        # Transition to running
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        assert rec.current_run_id is not None
        assert rec.current_run_id.startswith("run-")

    def test_running_to_idle_finishes_run(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        assert rec.current_run_id is None
        assert rec.last_run_id == run_id

    def test_multiple_runs_increment_counter(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run1 = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run2 = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        assert run1 != run2
        assert rec.last_run_id == run2


class TestObserveConcurrency:
    def test_concurrent_observe_records_each_transition_once(self):
        for _ in range(500):
            rec = _RaceWideningEventRecorder(observe_min_interval_s=0.0)
            status = _StubStatus(
                state=ExecutionState.PLANNING,
                execution_id=7,
                active_move_id="move-race",
            )
            snapshot = _StubSnapshot()
            cycle_manager = _StubCycleManager(
                "running",
                kernel_status=status,
                kernel_snapshot=snapshot,
            )
            cycle_manager.get_node_statuses = lambda: {"root": "RUNNING"}
            state = _StubState(
                cycle_manager=cycle_manager,
                kernel=_StubKernel(status=status, snapshot=snapshot),
            )
            barrier = threading.Barrier(3)
            errors: list[BaseException] = []

            def observe_once() -> None:
                try:
                    barrier.wait(timeout=1.0)
                    rec.observe(state, force=True)
                except BaseException as exc:  # pragma: no cover - test aid
                    errors.append(exc)

            threads = [
                threading.Thread(target=observe_once, name=f"observe-race-{i}")
                for i in range(2)
            ]
            for thread in threads:
                thread.start()
            barrier.wait(timeout=1.0)
            for thread in threads:
                thread.join(timeout=1.0)

            assert all(not thread.is_alive() for thread in threads)
            assert not errors
            events = _memory_events(rec)
            cycle_events = [
                event for event in events if event["type"] == "cycle_state_changed"
            ]
            kernel_events = [
                event for event in events if event["type"] == "kernel_state_transition"
            ]
            node_events = [
                event for event in events if event["type"] == "node_status_transition"
            ]
            assert [(event["details"]["from"], event["details"]["to"]) for event in cycle_events] == [
                (None, "running")
            ]
            assert [
                (event["details"]["from"], event["details"]["to"])
                for event in kernel_events
            ] == [(None, "planning")]
            assert [
                (event["from"], event["to"])
                for event in node_events
            ] == [(None, "RUNNING")]


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
        rec = EventRecorder(observe_min_interval_s=0.0)
        # Bootstrap cycle state
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.PLANNING, ExecutionState.VALIDATING,
        )
        semantic_types = [e["type"] for e in events]
        assert "plan_completed" in semantic_types

    def test_validating_to_executing_emits_execute_started(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.VALIDATING, ExecutionState.EXECUTING,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_started" in semantic_types

    def test_record_event_lifts_action_span_and_contact_policy_fields(self):
        rec = EventRecorder(observe_min_interval_s=0.0)

        rec._record_event(
            event_type="execute_started",
            run_id="run-001",
            execution_id=7,
            details={
                "action_source": "program_node",
                "action_span_id": "attempt-001",
                "node_instance_id": "sequence/move_to_pick",
                "node_attempt_id": "attempt-001",
                "parent_node_attempt_id": "plan-attempt-001",
                "contact_policy_id": "contact_grasp/lite6_box",
            },
        )

        event = _memory_events(rec)[-1]
        assert event["action_source"] == "program_node"
        assert event["action_span_id"] == "attempt-001"
        assert event["node_instance_id"] == "sequence/move_to_pick"
        assert event["node_attempt_id"] == "attempt-001"
        assert event["parent_node_attempt_id"] == "plan-attempt-001"
        assert event["contact_policy_id"] == "contact_grasp/lite6_box"
        assert event["details"]["contact_policy_id"] == "contact_grasp/lite6_box"

    def test_executing_to_completed(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.EXECUTING, ExecutionState.COMPLETED,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_completed" in semantic_types

    def test_executing_to_failed(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.EXECUTING, ExecutionState.FAILED,
        )
        semantic_types = [e["type"] for e in events]
        assert "execute_failed" in semantic_types

    def test_stopping_to_intervention_required(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        events = self._observe_kernel_transition(
            rec, ExecutionState.STOPPING, ExecutionState.INTERVENTION_REQUIRED,
        )
        intervention_events = [e for e in events if e["type"] == "intervention_required"]
        assert intervention_events
        assert intervention_events[-1]["severity"] == "ERROR"

    def test_executing_to_paused(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
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
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

        assert rec.wait_for_jsonl_idle()
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
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=None)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        # Should not raise
        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

    def test_history_loads_from_jsonl(self, tmp_path):
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        assert rec.wait_for_jsonl_idle()

        # Create a fresh recorder pointing at the same trace dir
        rec2 = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
        result = rec2.history(run_id=run_id)
        assert result["run_id"] == run_id
        assert len(result["events"]) > 0

    def test_events_include_operational_envelope_fields(self, tmp_path):
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
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
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id

        exported = rec.export_ros2_medkit_trace(run_id=run_id, exported_at_ns=123456789)
        assert exported["schema_version"] == "manyforge.ros2_medkit_trace.v1"
        assert exported["trace_source"] == "manyforge_composer"
        assert exported["exported_at_ns"] == 123456789
        assert exported["run_id"] == run_id
        assert exported["program_run_id"] == run_id
        assert exported["records"]
        # The Composer event stream now has internal diagnostic channels, but
        # ros2_medkit gets the stable v1 normalized record schema. Keep channel
        # routing out of the medkit contract unless that schema is versioned.
        first_record = exported["records"][0]
        assert "channel" not in first_record
        assert "channel" not in first_record["attributes"]

    def test_failure_archive_written_for_execution_failure(self, tmp_path):
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
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
        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
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

    def test_native_error_event_preserves_structured_error_details(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        native_events = [
            {
                "event_index": 4,
                "type": "error_raised",
                "timestamp_ns": 104,
                "execution_id": 43,
                "error": {
                    "code": "validation_collision",
                    "message": "Collision validation rejected trajectory",
                    "details": {
                        "collision_result_first_failure_pair": (
                            "object/box_a/shape/0 <-> "
                            "robot/robot1/link/link1/collision/0"
                        ),
                        "collision_result_first_failure_classification": (
                            "blocking_collision"
                        ),
                    },
                },
            },
        ]

        rec.record_native_kernel_events(native_events)

        events = _memory_events(rec)
        assert len(events) == 1
        details = events[0]["details"]
        assert events[0]["type"] == "error_raised"
        assert details["error_code"] == "validation_collision"
        assert details["error"] == "Collision validation rejected trajectory"
        assert (
            details["collision_result_first_failure_pair"]
            == "object/box_a/shape/0 <-> robot/robot1/link/link1/collision/0"
        )
        assert (
            details["collision_result_first_failure_classification"]
            == "blocking_collision"
        )
        assert details["error_details"][
            "collision_result_first_failure_pair"
        ] == details["collision_result_first_failure_pair"]

    def test_native_controlled_stop_success_records_single_summary(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        native_events = [
            {
                "event_index": 20,
                "type": "stop_issued",
                "timestamp_ns": 120,
                "execution_id": 9,
                "details": {
                    "controlled_stop_success": "true",
                    "phase": "operator_stop",
                    "requested_stop_mode": "soft_on_path",
                    "executed_stop_mode": "soft_on_path",
                    "stop_command_source": "fjt_preempt_path_ramp",
                    "deceleration_time_s": "1.000000",
                    "confirmation_latency_ns": "910000000",
                    "hard_stop_floor": "hard_deceleration",
                },
            },
            {
                "event_index": 21,
                "type": "state_transition",
                "timestamp_ns": 121,
                "execution_id": 9,
                "details": {
                    "from": "stopping",
                    "to": "stopped",
                },
            },
        ]

        rec.record_native_kernel_events(native_events)

        events = _memory_events(rec)
        assert len(events) == 1
        assert events[0]["type"] == "controlled_stop_completed"
        assert events[0]["execution_id"] == 9
        assert events[0]["channel"] == "controlled_stop"
        details = events[0]["details"]
        assert details["requested_stop_mode"] == "soft_on_path"
        assert details["executed_stop_mode"] == "soft_on_path"
        assert details["stop_command_source"] == "fjt_preempt_path_ramp"
        assert details["native_kernel_event_type"] == "stop_issued"
        assert details["native_kernel_event_index"] == 20

    def test_native_hard_deceleration_success_keeps_settle_evidence(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "event_index": 30,
                    "type": "stop_issued",
                    "timestamp_ns": 220,
                    "execution_id": 12,
                    "details": {
                        "controlled_stop_success": "true",
                        "phase": "operator_stop",
                        "requested_stop_mode": "hard_deceleration",
                        "executed_stop_mode": "hard_deceleration",
                        "stop_command_source": "jtc_cancel_decelerate_on_cancel",
                        "confirmation_latency_ns": "850000000",
                        "controller_cancel_confirmation_latency_ns": "31000000",
                        "post_stop_sample_source": "controller_state",
                        "post_stop_velocity_available": "true",
                        "post_stop_at_rest": "true",
                        "post_stop_max_abs_velocity_rad_s": "0.002",
                        "settle_velocity_threshold_rad_s": "0.01",
                        "post_stop_settle_wait_ns": "819000000",
                    },
                }
            ]
        )

        events = _memory_events(rec)
        assert len(events) == 1
        assert events[0]["type"] == "controlled_stop_completed"
        assert events[0]["channel"] == "controlled_stop"
        details = events[0]["details"]
        assert details["executed_stop_mode"] == "hard_deceleration"
        assert details["post_stop_sample_source"] == "controller_state"
        assert details["post_stop_at_rest"] == "true"
        assert details["controller_cancel_confirmation_latency_ns"] == "31000000"
        assert details["confirmation_latency_ns"] == "850000000"

    def test_native_resume_replan_lifecycle_is_recorded(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        native_events = [
            {
                "event_index": 10,
                "type": "plan_started",
                "timestamp_ns": 100,
                "execution_id": 7,
                "details": {
                    "reason": "resume_replan",
                    "resume_phase": "planning",
                },
            },
            {
                "event_index": 11,
                "type": "plan_completed",
                "timestamp_ns": 101,
                "execution_id": 7,
                "details": {
                    "reason": "resume_replan",
                    "resume_phase": "planning",
                },
            },
            {
                "event_index": 12,
                "type": "validation_started",
                "timestamp_ns": 102,
                "execution_id": 7,
                "details": {
                    "reason": "resume_replan",
                    "resume_phase": "validation",
                },
            },
            {
                "event_index": 13,
                "type": "validation_passed",
                "timestamp_ns": 103,
                "execution_id": 7,
                "details": {
                    "reason": "resume_replan",
                    "resume_phase": "validation",
                },
            },
            {
                "event_index": 14,
                "type": "execute_started",
                "timestamp_ns": 104,
                "execution_id": 7,
                "details": {
                    "reason": "resume_replan",
                },
            },
        ]

        rec.record_native_kernel_events(native_events)

        events = _memory_events(rec)
        assert [event["type"] for event in events] == [
            "plan_started",
            "plan_completed",
            "validation_started",
            "validation_passed",
        ]
        assert all(event["execution_id"] == 7 for event in events)
        assert all(event["details"]["reason"] == "resume_replan" for event in events)
        assert events[0]["details"]["native_kernel_event_index"] == 10
        assert events[-1]["details"]["native_kernel_event_index"] == 13

    def test_regular_native_lifecycle_events_are_not_recorded(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "event_index": 1,
                    "type": "plan_started",
                    "timestamp_ns": 100,
                    "execution_id": 1,
                },
                {
                    "event_index": 2,
                    "type": "validation_passed",
                    "timestamp_ns": 101,
                    "execution_id": 1,
                    "details": {
                        "reason": "normal_execute",
                    },
                },
                {
                    "event_index": 3,
                    "type": "stop_issued",
                    "timestamp_ns": 102,
                    "execution_id": 1,
                    "details": {
                        "reason": "legacy_stop_without_controlled_summary",
                    },
                },
            ]
        )

        assert _memory_events(rec) == []

    def test_history_filters_by_severity_component_and_type(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec._record_event(
            event_type="cycle_started",
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

    def test_history_filters_by_span_and_move_identity(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._record_event(
            event_type="plan_started",
            execution_id=7,
            move_id="move_to_pick",
            details={
                "action_span_id": "span-pick",
                "action_source": "program_node",
                "node_instance_id": "node-pick",
                "node_attempt_id": "attempt-pick-1",
                "contact_policy_id": "contact-pick",
            },
        )
        rec._record_event(
            event_type="plan_completed",
            execution_id=8,
            move_id="move_to_drop",
            details={
                "action_span_id": "span-drop",
                "node_instance_id": "node-drop",
                "node_attempt_id": "attempt-drop-1",
                "contact_policy_id": "contact-drop",
            },
        )

        by_span = rec.history(action_span_id="span-pick")["events"]
        assert [event["move_id"] for event in by_span] == ["move_to_pick"]
        assert by_span[0]["node_attempt_id"] == "attempt-pick-1"

        by_node_attempt = rec.history(node_attempt_id="attempt-drop-1")["events"]
        assert [event["action_span_id"] for event in by_node_attempt] == ["span-drop"]

        by_move = rec.export_events(move_id="move_to_pick")
        assert [event["action_span_id"] for event in by_move] == ["span-pick"]

        by_kernel_execution = rec.history(kernel_execution_id=8)["events"]
        assert [event["move_id"] for event in by_kernel_execution] == ["move_to_drop"]


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
        rec = EventRecorder(observe_min_interval_s=0.0)
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
        rec = EventRecorder(observe_min_interval_s=0.0)
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
        rec = EventRecorder(observe_min_interval_s=0.0)
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
        rec = EventRecorder(observe_min_interval_s=0.0)
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


class TestEventChannelsAndIncidents:
    """Protected channel retention and incident snapshots."""

    def test_pose_source_spam_does_not_evict_fault_channel(self):
        rec = EventRecorder(max_events=5, observe_min_interval_s=0.0)
        rec._record_event(
            event_type="kernel_error",
            details={
                "error": "Timed out waiting for FollowJointTrajectory cancel confirmation",
                "reason_code": "controller_cancel_timeout",
            },
        )
        for index in range(20):
            rec.record_pose_source_event(
                "applied",
                details={"object_id": f"obj-{index}", "status": "applied"},
            )

        all_recent = rec.history()["events"]
        assert all(event["type"] != "kernel_error" for event in all_recent)

        channels = rec.channel_histories()
        fault_events = channels["fault_safety_stop"]
        assert [event["type"] for event in fault_events] == ["kernel_error"]
        assert fault_events[0]["details"]["reason_code"] == "controller_cancel_timeout"

    def test_incident_snapshot_freezes_all_events_and_channels(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_pose_source_event(
            "applied",
            details={"object_id": "box", "status": "applied"},
        )
        rec._record_event(
            event_type="error_raised",
            details={
                "error": "Backend stop failed",
                "requested_stop_mode": "spring_back_hold",
                "reason_code": "controller_cancel_timeout",
            },
        )

        snapshots = rec.incident_snapshots()
        assert len(snapshots) == 1
        snapshot = snapshots[0]
        assert snapshot["triggerType"] == "error_raised"
        assert snapshot["triggerChannel"] == "fault_safety_stop"
        assert [event["type"] for event in snapshot["allEvents"]] == [
            "pose_source_applied",
            "error_raised",
        ]
        assert (
            snapshot["eventsByChannel"]["fault_safety_stop"][0]["type"]
            == "error_raised"
        )
        assert (
            snapshot["eventsByChannel"]["pose_source"][0]["type"]
            == "pose_source_applied"
        )

    def test_incident_snapshot_separates_span_chain_from_context(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._record_event(
            event_type="plan_started",
            move_id="foundationpose_pick",
            details={
                "action_span_id": "span-foundationpose",
                "node_instance_id": "node-foundationpose",
                "node_attempt_id": "attempt-foundationpose-1",
            },
        )
        rec._record_event(
            event_type="plan_started",
            move_id="drop_approach",
            details={
                "action_span_id": "span-drop",
                "node_instance_id": "node-drop",
                "node_attempt_id": "attempt-drop-1",
                "parent_node_attempt_id": "attempt-pick-sequence-1",
            },
        )
        rec._record_event(
            event_type="error_raised",
            move_id="drop_approach",
            details={
                "error": "Planning timed out",
                "action_span_id": "span-drop",
                "node_instance_id": "node-drop",
                "node_attempt_id": "attempt-drop-1",
                "parent_node_attempt_id": "attempt-pick-sequence-1",
            },
        )

        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["actionSpan"] == {
            "actionSpanId": "span-drop",
            "nodeInstanceId": "node-drop",
            "nodeAttemptId": "attempt-drop-1",
            "parentNodeAttemptId": "attempt-pick-sequence-1",
            "moveId": "drop_approach",
        }
        assert snapshot["spanScope"] == {
            "field": "action_span_id",
            "kind": "action_span",
            "value": "span-drop",
            "label": "Span-local chain scoped by action span",
            "attemptExact": True,
        }
        assert [event["move_id"] for event in snapshot["allEvents"]] == [
            "foundationpose_pick",
            "drop_approach",
            "drop_approach",
        ]
        assert [event["move_id"] for event in snapshot["spanEvents"]] == [
            "drop_approach",
            "drop_approach",
        ]
        assert {
            event["action_span_id"] for event in snapshot["spanEvents"]
        } == {"span-drop"}
        assert snapshot["contextWindows"]["spanEvents"]["count"] == 2

    def test_incident_headline_prefers_span_local_cause_over_generic_wrapper(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._record_event(
            event_type="plan_failed",
            execution_id=6,
            move_id="lite6_drop_approach",
            details={
                "error": "Planning timed out",
                "reason_code": "planning_timeout",
                "action_span_id": "span-drop",
                "node_instance_id": "move_to_drop_approach",
                "node_attempt_id": "attempt-drop-1",
            },
        )
        rec._record_event(
            event_type="cycle_error",
            execution_id=6,
            move_id="lite6_drop_approach",
            details={
                "error": "Motion execution ended in stopped",
                "action_span_id": "span-drop",
                "node_instance_id": "move_to_drop_approach",
                "node_attempt_id": "attempt-drop-1",
            },
        )

        snapshot = rec.incident_snapshots()[-1]
        headline = snapshot["headlineCause"]
        assert headline["eventType"] == "plan_failed"
        assert headline["errorCode"] == "planning_timeout"
        assert headline["genericWrapperSuppressed"] is True
        assert headline["summary"] == (
            "Planning timed out while planning `move_to_drop_approach` "
            "(`lite6_drop_approach`, execution 6). Cycle stopped and can be resumed."
        )

    def test_stale_foundationpose_previous_run_is_context_not_headline(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._record_event(
            event_type="pose_source_apply_failed",
            run_id="run-foundationpose-stale",
            move_id="foundationpose_pick",
            details={
                "error": "no fresh pose-source update for: graspable",
                "reason": "pick_sequence:plan_foundationpose_trocar_pick",
                "action_span_id": "span-foundationpose",
                "node_instance_id": "plan_foundationpose_trocar_pick",
                "node_attempt_id": "attempt-foundationpose-1",
            },
        )
        rec._record_event(
            event_type="plan_failed",
            run_id="run-drop-timeout",
            execution_id=6,
            move_id="lite6_drop_approach",
            details={
                "error": "Planning timed out",
                "reason_code": "planning_timeout",
                "action_span_id": "span-drop",
                "node_instance_id": "move_to_drop_approach",
                "node_attempt_id": "attempt-drop-1",
            },
        )
        rec._record_event(
            event_type="cycle_error",
            run_id="run-drop-timeout",
            execution_id=6,
            move_id="lite6_drop_approach",
            details={
                "error": "Motion execution ended in stopped",
                "action_span_id": "span-drop",
                "node_instance_id": "move_to_drop_approach",
                "node_attempt_id": "attempt-drop-1",
            },
        )

        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["headlineCause"]["eventType"] == "plan_failed"
        assert snapshot["headlineCause"]["errorCode"] == "planning_timeout"
        assert snapshot["actionSpan"]["actionSpanId"] == "span-drop"
        assert {
            event["action_span_id"] for event in snapshot["spanEvents"]
        } == {"span-drop"}
        assert "span-foundationpose" not in {
            event.get("action_span_id") for event in snapshot["spanEvents"]
        }
        assert "span-foundationpose" in {
            event.get("action_span_id") for event in snapshot["allEvents"]
        }
        assert {
            event.get("run_id") for event in snapshot["spanEvents"]
        } == {"run-drop-timeout"}

    def test_parallel_sibling_events_are_context_not_causal_chain(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._record_event(
            event_type="plan_completed",
            run_id="run-parallel",
            execution_id=10,
            move_id="parallel_benign_probe",
            details={
                "action_span_id": "span-sibling",
                "node_instance_id": "root/parallel/benign_probe",
                "node_attempt_id": "attempt-sibling-1",
            },
        )
        rec._record_event(
            event_type="validation_failed",
            run_id="run-parallel",
            execution_id=11,
            move_id="drop_approach",
            details={
                "error": "Collision validation rejected trajectory",
                "reason_code": "validation_collision",
                "action_span_id": "span-drop",
                "node_instance_id": "root/parallel/drop_branch/move",
                "node_attempt_id": "attempt-drop-1",
            },
        )

        snapshot = rec.incident_snapshots()[-1]
        assert [event["action_span_id"] for event in snapshot["spanEvents"]] == [
            "span-drop"
        ]
        assert {
            event.get("action_span_id") for event in snapshot["allEvents"]
        } == {"span-sibling", "span-drop"}
        assert snapshot["spanScope"]["attemptExact"] is True

    def test_duplicate_display_names_disambiguate_by_node_instance_id(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        for branch, span_id, attempt_id, event_type in (
            ("left", "span-left-move", "attempt-left-1", "plan_completed"),
            ("right", "span-right-move", "attempt-right-1", "plan_failed"),
        ):
            rec._record_event(
                event_type=event_type,
                run_id="run-duplicate-display-name",
                execution_id=20 if branch == "left" else 21,
                move_id="move",
                details={
                    "error": "Planning timed out" if event_type == "plan_failed" else "",
                    "reason_code": "planning_timeout"
                    if event_type == "plan_failed"
                    else "",
                    "display_name": "Move",
                    "action_span_id": span_id,
                    "node_instance_id": f"root/parallel/{branch}/move",
                    "node_attempt_id": attempt_id,
                },
            )

        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["actionSpan"]["nodeInstanceId"] == "root/parallel/right/move"
        assert [event["node_instance_id"] for event in snapshot["spanEvents"]] == [
            "root/parallel/right/move"
        ]
        assert rec.history(node_instance_id="root/parallel/left/move")["events"][
            0
        ]["action_span_id"] == "span-left-move"
        assert rec.history(node_instance_id="root/parallel/right/move")["events"][
            0
        ]["action_span_id"] == "span-right-move"


class TestGetEventsSince:
    """Incremental event polling."""

    def test_returns_events_after_index(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        idx = rec._event_index
        rec.observe(_StubState(cycle_manager=_StubCycleManager("stopped")))

        new_events = rec.get_events_since(idx)
        assert len(new_events) > 0
        assert all(e["event_index"] > idx for e in new_events)

    def test_returns_empty_when_caught_up(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        idx = rec._event_index
        assert rec.get_events_since(idx) == []


# --- Audit 6G: synthesized semantic events on poll-skipped edges ---


class TestSynthesizedSemanticEvents:
    """A fast move can flash through Executing between polls (found by the
    6G soak gate). Completed proves an execution window happened."""

    def _observe_pair(self, rec, from_state, to_state, *,
                      from_id=1, to_id=1, move_id="m1"):
        idx_before = rec._event_index
        snap = _StubSnapshot()
        status_from = _StubStatus(
            state=from_state, execution_id=from_id, active_move_id=move_id)
        rec.observe(_StubState(
            cycle_manager=_StubCycleManager(
                "running", kernel_status=status_from, kernel_snapshot=snap),
            kernel=_StubKernel(status=status_from, snapshot=snap)))
        status_to = _StubStatus(
            state=to_state, execution_id=to_id, active_move_id=move_id)
        rec.observe(_StubState(
            cycle_manager=_StubCycleManager(
                "running", kernel_status=status_to, kernel_snapshot=snap),
            kernel=_StubKernel(status=status_to, snapshot=snap)))
        return [e for e in _memory_events(rec) if e["event_index"] > idx_before]

    def test_skipped_executing_window_synthesizes_execute_events(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        events = self._observe_pair(
            rec, ExecutionState.VALIDATING, ExecutionState.COMPLETED)
        types = [e["type"] for e in events]
        assert "execute_started" in types
        assert "execute_completed" in types
        started = next(e for e in events if e["type"] == "execute_started")
        assert started["details"]["synthesized_from_edge"] == (
            "validating->completed")
        rec.close()

    def test_back_to_back_moves_synthesize_boundary_events(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        events = self._observe_pair(
            rec, ExecutionState.EXECUTING, ExecutionState.EXECUTING,
            from_id=1, to_id=2)
        # First observe legitimately emits execute_started for id 1
        # (None->executing); the boundary synthesis adds completed(1) +
        # started(2).
        started_ids = [
            e["execution_id"] for e in events if e["type"] == "execute_started"
        ]
        completed_ids = [
            e["execution_id"] for e in events if e["type"] == "execute_completed"
        ]
        assert started_ids == [1, 2]
        assert completed_ids == [1]
        rec.close()

    def test_planning_to_executing_jump_synthesizes_plan_completed(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        events = self._observe_pair(
            rec, ExecutionState.PLANNING, ExecutionState.EXECUTING)
        types = [e["type"] for e in events]
        assert "plan_completed" in types
        assert "execute_started" in types
        rec.close()

    def test_failure_edges_are_not_interpolated(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        events = self._observe_pair(
            rec, ExecutionState.VALIDATING, ExecutionState.FAILED)
        types = [e["type"] for e in events]
        assert "execute_started" not in types
        assert "validation_failed" in types
        rec.close()


class TestDurableFaultWrites:
    """WARN+ events are fsync'd so a fault survives an immediate power loss."""

    def test_fault_events_fsync_but_steady_state_events_do_not(
        self, tmp_path, monkeypatch
    ):
        import os as os_module

        import manyforge_composer.backend.event_recorder as er

        fsynced: list[int] = []
        real_fsync = os_module.fsync

        def recording_fsync(fd):
            fsynced.append(fd)
            return real_fsync(fd)

        monkeypatch.setattr(er.os, "fsync", recording_fsync)

        rec = EventRecorder(observe_min_interval_s=0.0, trace_dir=tmp_path)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        assert rec.wait_for_jsonl_idle()
        assert fsynced == []  # steady-state INFO events stay in page cache

        rec.observe(
            _StubState(
                cycle_manager=_StubCycleManager(
                    "stopped", error="collision detected"
                )
            )
        )
        assert rec.wait_for_jsonl_idle()
        assert len(fsynced) >= 1  # the fault-grade event hit the platter
        rec.close()


class TestProgramRunIdAdoption:
    """spec/500 §2.2: the orchestrator runner is the authoritative minter —
    the recorder adopts its id so durable history, kernel-stamped events,
    and runner records share one id space."""

    def test_recorder_adopts_runner_minted_run_id(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        manager = _StubCycleManager("idle")
        rec.observe(_StubState(cycle_manager=manager))

        manager_running = _StubCycleManager("running")
        manager_running.program_run_id = "run-20260703T000000Z-cafe0123"
        rec.observe(_StubState(cycle_manager=manager_running))

        assert rec.current_run_id == "run-20260703T000000Z-cafe0123"

    def test_recorder_mints_when_runner_offers_no_id(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        assert rec.current_run_id and rec.current_run_id.startswith("run-")

    def test_native_kernel_event_routes_to_its_stamped_run(self):
        """A kernel-stamped program_run_id is canonical: the durable event
        carries it top-level and history filters find it — even when the
        event is drained after a different run became current."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "error_raised",
                    "execution_id": 7,
                    "event_index": 1,
                    "timestamp_ns": 123,
                    "program_run_id": "run-kernel-stamped-01",
                    "error": {"code": -1, "message": "boom"},
                    "details": {"program_run_id": "run-kernel-stamped-01"},
                }
            ]
        )
        result = rec.history(
            run_id="run-kernel-stamped-01",
            program_run_id="run-kernel-stamped-01",
        )
        events = result["events"]
        assert events, "programRunId filter missed the kernel-stamped id"
        assert events[-1]["program_run_id"] == "run-kernel-stamped-01"

    def test_native_kernel_event_preserves_top_level_action_span_fields(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "error_raised",
                    "execution_id": 8,
                    "event_index": 2,
                    "timestamp_ns": 124,
                    "program_run_id": "run-kernel-stamped-02",
                    "action_source": "program_node",
                    "action_span_id": "span-move-002",
                    "action_span_program_run_id": "run-kernel-stamped-02",
                    "node_instance_id": "root/sequence/move_to_place",
                    "node_attempt_id": "attempt-move-002",
                    "parent_node_attempt_id": "attempt-plan-002",
                    "contact_policy_id": "contact_grasp/lite6_box",
                    "error": {"code": -1, "message": "boom"},
                }
            ]
        )

        events = rec.history(
            run_id="run-kernel-stamped-02",
            program_run_id="run-kernel-stamped-02",
        )["events"]
        assert len(events) == 1
        event = events[0]
        assert event["action_source"] == "program_node"
        assert event["action_span_id"] == "span-move-002"
        assert event["node_instance_id"] == "root/sequence/move_to_place"
        assert event["node_attempt_id"] == "attempt-move-002"
        assert event["parent_node_attempt_id"] == "attempt-plan-002"
        assert event["contact_policy_id"] == "contact_grasp/lite6_box"
        assert event["details"]["action_span_program_run_id"] == (
            "run-kernel-stamped-02"
        )
