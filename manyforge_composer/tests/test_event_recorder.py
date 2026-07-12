"""Unit tests for EventRecorder — run lifecycle, semantic events, JSONL sink."""

from __future__ import annotations

from collections import deque
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

def _curated_events(rec: EventRecorder) -> list[dict[str, Any]]:
    """Events excluding the OW1 full-fidelity kernel_native lane.

    Curated-projection tests assert the UI-facing contract; the additive
    kernel_native channel records everything by design and is asserted by
    TestKernelEventPipelineLossVisibility.
    """
    return [
        e
        for e in _memory_events(rec)
        if e.get("channel") != "kernel_native" and e.get("type") != "kernel_event_gap"
    ]




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

        events = _curated_events(rec)
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

        events = _curated_events(rec)
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

        events = _curated_events(rec)
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

        events = _curated_events(rec)
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

        # OW1: these now land on the kernel_native full-fidelity channel,
        # but the curated UI lanes stay empty.
        assert _curated_events(rec) == []
        assert all(
            e["channel"] == "kernel_native" for e in _memory_events(rec)
        )

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

    def test_intervention_projection_slices_bounded_event_ring_after_cursor(self):
        rec = EventRecorder(max_events=5000, observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))

        intervention = _StubIntervention(session_id="sess-ring")
        intervention.events = deque(maxlen=16)
        for index in range(1, 25):
            intervention.events.append(
                {
                    "event_index": index,
                    "timestamp_ns": time.monotonic_ns(),
                    "type": "session_progress",
                    "details": {"value": index},
                }
            )

        cm = _StubCycleManager("direct_control", intervention=intervention)
        rec.observe(_StubState(cycle_manager=cm))
        projected = [
            event
            for event in _memory_events(rec)
            if event["type"] == "intervention_session_progress"
        ]
        assert [event["details"]["value"] for event in projected] == list(
            range(9, 25)
        )

        for index in range(25, 31):
            intervention.events.append(
                {
                    "event_index": index,
                    "timestamp_ns": time.monotonic_ns(),
                    "type": "session_progress",
                    "details": {"value": index},
                }
            )

        rec.observe(_StubState(cycle_manager=cm))
        projected = [
            event
            for event in _memory_events(rec)
            if event["type"] == "intervention_session_progress"
        ]
        assert [event["details"]["value"] for event in projected] == list(
            range(9, 31)
        )

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

    def test_scene_snapshot_lease_status_counts_deduped_native_decisions(self):
        rec = EventRecorder(observe_min_interval_s=0.0)

        rec.record_native_kernel_events(
            [
                {
                    "type": "validation_passed",
                    "event_index": 10,
                    "execution_id": 1,
                    "details": {
                        "reason": "scene_snapshot_lease_live_divergence_accepted",
                    },
                },
                {
                    "type": "error_raised",
                    "event_index": 11,
                    "execution_id": 1,
                    "error": {
                        "code": "scene_changed_during_validation",
                        "message": "Observed target moved",
                        "details": {
                            "reason_code": "target_moved_after_validation",
                            "object_id": "box",
                            "source_id": "foundationpose",
                        },
                    },
                },
            ]
        )
        rec.record_native_kernel_events(
            [
                {
                    "type": "error_raised",
                    "event_index": 11,
                    "execution_id": 1,
                    "error": {
                        "code": "scene_changed_during_validation",
                        "message": "Observed target moved",
                        "details": {
                            "reason_code": "target_moved_after_validation",
                        },
                    },
                }
            ]
        )

        status = rec.scene_snapshot_lease_status()
        counters = status["counters"]
        assert counters["accepted_bounded_divergence_total"] == 1
        assert counters["target_drift_rejected_total"] == 1
        assert counters["obstacle_drift_rejected_total"] == 0
        assert status["last_reason"] == "target_moved_after_validation"

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

    def test_incident_snapshot_uses_predecoded_rings_without_redecoding(
        self,
        monkeypatch,
    ):
        rec = EventRecorder(observe_min_interval_s=0.0)
        for index in range(260):
            rec._record_event(
                event_type="plan_started",
                execution_id=index + 1,
                move_id=f"move-{index}",
            )

        def fail_decode(_stored):
            raise AssertionError("incident snapshot decoded stored JSON under lock")

        monkeypatch.setattr(EventRecorder, "_decode_stored_event", fail_decode)
        rec._record_event(
            event_type="error_raised",
            details={"error": "Backend stop failed"},
        )

        snapshot = rec.incident_snapshots()[-1]
        expected_all = [
            json.loads(stored.json_text)
            for stored in list(rec._events)[
                -event_recorder_module._INCIDENT_ALL_EVENT_LIMIT:
            ]
        ]
        expected_channels = {
            channel: [
                json.loads(stored.json_text)
                for stored in list(events)[
                    -event_recorder_module._INCIDENT_CHANNEL_EVENT_LIMIT:
                ]
            ]
            for channel, events in rec._events_by_channel.items()
        }

        assert snapshot["allEvents"] == expected_all
        assert snapshot["eventsByChannel"] == expected_channels

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
        # Exactly one durable write is expected here: the run-index
        # run_started line (load-bearing for restart rehydration, plan OW5).
        # Steady-state INFO *events* stay in page cache.
        index_fsyncs = len(fsynced)
        assert index_fsyncs <= 1
        fsynced.clear()
        rec._record_event(event_type="cache_hit", execution_id=1)
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

        events = [
            e
            for e in rec.history(
                run_id="run-kernel-stamped-02",
                program_run_id="run-kernel-stamped-02",
            )["events"]
            # OW1 additive full-fidelity lane; this test asserts the curated
            # projection's correlation contract.
            if e.get("channel") != "kernel_native"
        ]
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


class TestClockAnchor:
    """OW2: kernel-stamped event wall times derive from the clock anchor."""

    def test_anchor_event_recorded_and_native_wall_time_derived(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        anchor_mono = 1_000_000_000_000
        anchor_wall = 1_700_000_000_000_000_000  # 2023-11-14T22:13:20Z
        rec.set_clock_anchor(monotonic_ns=anchor_mono, wall_time_ns=anchor_wall)

        anchor_events = [
            e for e in _memory_events(rec) if e["type"] == "clock_anchor"
        ]
        assert len(anchor_events) == 1
        assert anchor_events[0]["details"]["monotonic_ns"] == str(anchor_mono)

        # A native kernel event stamped 5 s after the anchor on the kernel
        # clock must carry anchor_wall + 5 s, not the receipt time.
        rec.record_native_kernel_events(
            [
                {
                    "type": "error_raised",
                    "event_index": 1,
                    "execution_id": 3,
                    "timestamp_ns": anchor_mono + 5_000_000_000,
                    "details": {"error_code": "planning_failed"},
                }
            ]
        )
        native = [e for e in _memory_events(rec) if e["type"] == "error_raised"]
        assert len(native) == 1
        assert native[0]["timestamp"] == "2023-11-14T22:13:25+00:00".replace(
            "+00:00", "Z"
        )

    def test_new_run_file_starts_with_its_own_anchor(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.set_clock_anchor(
            monotonic_ns=1_000_000_000_000,
            wall_time_ns=1_700_000_000_000_000_000,
        )
        # idle -> running starts a new run; the run must be self-contained
        # for wall-time derivation: its first event is the anchor.
        rec._handle_cycle_state_transition(None, "running", "run-anchor-01")
        run_events = [
            e for e in _memory_events(rec) if e.get("run_id") == "run-anchor-01"
        ]
        assert run_events
        assert run_events[0]["type"] == "clock_anchor"
        assert run_events[0]["details"]["monotonic_ns"] == "1000000000000"

    def test_without_anchor_falls_back_to_receipt_wall_time(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "error_raised",
                    "event_index": 1,
                    "execution_id": 3,
                    "timestamp_ns": 42,
                    "details": {},
                }
            ]
        )
        native = [e for e in _memory_events(rec) if e["type"] == "error_raised"]
        assert len(native) == 1
        # Receipt-time fallback: plausible current wall time, not 1970.
        assert native[0]["timestamp"].startswith("20")
        assert not native[0]["timestamp"].startswith("1970")


class TestKernelFlightDumpIngest:
    """OW3: fail-closed kernel flight dumps become durable incidents."""

    def _write_dump(self, flight_dir: Path, name: str, header: dict,
                    records: int) -> Path:
        path = flight_dir / name
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(json.dumps(header) + "\n")
            for i in range(records):
                handle.write(
                    json.dumps({"seq": i + 1, "type": "state_transition"}) + "\n"
                )
        return path

    def test_ingests_renames_and_records_error_event(self, tmp_path):
        flight_dir = tmp_path / "flight"
        flight_dir.mkdir()
        self._write_dump(
            flight_dir,
            "flight-1700000000000.jsonl",
            {
                "schema_version": "manyforge.kernel_flight.v1",
                "reason": "scheduler_drain_timeout",
                "dumped_wall_ns": 1_700_000_000_000_000_000,
                "torn": False,
            },
            records=7,
        )
        rec = EventRecorder(observe_min_interval_s=0.0)
        ingested = rec.ingest_kernel_flight_dumps(str(flight_dir))
        assert ingested == 1

        events = [
            e for e in _memory_events(rec) if e["type"] == "kernel_flight_dump"
        ]
        assert len(events) == 1
        assert events[0]["severity"] == "ERROR"
        assert events[0]["details"]["reason"] == "scheduler_drain_timeout"
        assert events[0]["details"]["records"] == "7"
        # Idempotence: the file is renamed and a second scan ingests nothing.
        assert not list(flight_dir.glob("flight-*.jsonl"))
        assert len(list(flight_dir.glob("*.ingested"))) == 1
        assert rec.ingest_kernel_flight_dumps(str(flight_dir)) == 0

    def test_malformed_dump_is_still_ingested_as_evidence(self, tmp_path):
        flight_dir = tmp_path / "flight"
        flight_dir.mkdir()
        (flight_dir / "flight-99.jsonl").write_text("{torn-not-json\n")
        rec = EventRecorder(observe_min_interval_s=0.0)
        assert rec.ingest_kernel_flight_dumps(str(flight_dir)) == 1
        events = [
            e for e in _memory_events(rec) if e["type"] == "kernel_flight_dump"
        ]
        assert len(events) == 1
        assert "parse_error" in events[0]["details"]

    def test_missing_directory_is_harmless(self, tmp_path):
        rec = EventRecorder(observe_min_interval_s=0.0)
        assert rec.ingest_kernel_flight_dumps(str(tmp_path / "absent")) == 0


class TestKernelEventPipelineLossVisibility:
    """OW1: full-fidelity durable lane + loud loss (gaps and drop counters)."""

    @staticmethod
    def _native(seq, idx, type_="state_transition", **kw):
        e = {
            "type": type_,
            "sequence": seq,
            "event_index": idx,
            "execution_id": 1,
            "timestamp_ns": 1000 + seq,
        }
        e.update(kw)
        return e

    def test_every_native_event_is_durable_full_fidelity(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._native(1, 1),
                self._native(2, 2, type_="cache_hit"),
                self._native(3, 3, type_="interrupt_latched"),
            ]
        )
        events = _memory_events(rec)
        types = [e["type"] for e in events]
        assert "kernel_native_state_transition" in types
        assert "kernel_native_cache_hit" in types
        assert "kernel_native_interrupt_latched" in types
        latched = next(
            e for e in events if e["type"] == "kernel_native_interrupt_latched"
        )
        assert latched["severity"] == "WARN"
        assert latched["channel"] == "kernel_native"
        assert latched["details"]["kernel_sequence"] == "3"
        # No gap: contiguous sequences must not synthesize gap records.
        assert not [e for e in events if e["type"] == "kernel_event_gap"]

    def test_sequence_gap_produces_durable_warn_record(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events([self._native(1, 1)])
        rec.record_native_kernel_events([self._native(5, 2)])
        gaps = [
            e for e in _memory_events(rec) if e["type"] == "kernel_event_gap"
        ]
        assert len(gaps) == 1
        assert gaps[0]["severity"] == "WARN"
        assert gaps[0]["details"]["missed_min"] == "2"
        assert gaps[0]["details"]["missed_max"] == "4"

    def test_pipeline_drop_counter_advance_produces_gap_record(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [self._native(1, 1)],
            pipeline_status={
                "collector_dropped_total": 0,
                "manager_dropped_total": 0,
            },
        )
        rec.record_native_kernel_events(
            [self._native(2, 2)],
            pipeline_status={
                "collector_dropped_total": 7,
                "manager_dropped_total": 0,
            },
        )
        gaps = [
            e for e in _memory_events(rec) if e["type"] == "kernel_event_gap"
        ]
        assert len(gaps) == 1
        assert gaps[0]["details"]["collector_dropped_delta"] == "7"

    def test_full_fidelity_lane_dedupes_by_event_index(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events([self._native(1, 1)])
        rec.record_native_kernel_events([self._native(1, 1)])
        events = [
            e
            for e in _memory_events(rec)
            if e["type"] == "kernel_native_state_transition"
        ]
        assert len(events) == 1


class TestTraceRetentionAndRehydration:
    """OW5: run index, restart rehydration, retention pruning."""

    def _start_and_finish_run(self, rec, run_id, severity_event=None):
        rec._handle_cycle_state_transition(None, "running", run_id)
        if severity_event:
            rec._record_event(event_type=severity_event, execution_id=1)
        rec._handle_cycle_state_transition("running", "idle")

    def test_run_index_lines_and_rehydration(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        self._start_and_finish_run(rec, "run-a")
        self._start_and_finish_run(rec, "run-b", severity_event="cycle_error")
        assert rec.close(timeout_s=10.0)

        index_path = tmp_path / "runs.index.jsonl"
        assert index_path.exists()
        lines = [json.loads(l) for l in index_path.read_text().splitlines()]
        kinds = [(l["kind"], l["run_id"]) for l in lines]
        assert ("run_started", "run-a") in kinds
        assert ("run_ended", "run-b") in kinds
        ended_b = next(
            l for l in lines if l["kind"] == "run_ended" and l["run_id"] == "run-b"
        )
        assert ended_b["max_severity"] == "ERROR"
        assert int(ended_b["event_count"]) >= 1

        # Restart: last_run_id and the run counter survive, so post-restart
        # forensics needs no out-of-band ids and synthesized ids cannot
        # collide with pre-restart ones.
        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        assert rec2.last_run_id == "run-b"
        assert rec2._run_counter >= 2
        assert rec2.close(timeout_s=10.0)

    def test_runs_listing_is_most_recent_first(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        self._start_and_finish_run(rec, "run-1")
        self._start_and_finish_run(rec, "run-2")
        assert rec.close(timeout_s=10.0)
        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        runs = rec2.list_runs()
        assert [r["run_id"] for r in runs][:2] == ["run-2", "run-1"]
        assert runs[0].get("ended_wall")
        assert rec2.close(timeout_s=10.0)

    def test_size_pruning_prefers_info_runs_and_records_evidence(self, tmp_path):
        rec = EventRecorder(
            trace_dir=tmp_path,
            observe_min_interval_s=0.0,
            trace_max_bytes=1,  # everything is over budget
        )
        self._start_and_finish_run(rec, "run-info")
        self._start_and_finish_run(rec, "run-error", severity_event="cycle_error")
        # Third run stays current/open: never pruned.
        rec._handle_cycle_state_transition(None, "running", "run-open")
        rec._record_event(event_type="cycle_started", execution_id=0)
        rec._jsonl_queue.join()  # writes are async; flush before pruning

        summary = rec._prune_trace_files()
        assert summary["error"] is None
        assert not (tmp_path / "run-info.events.jsonl").exists()
        # ERROR run pruned only because even alone it exceeds the 1-byte
        # budget — but INFO must have gone first (both gone here); the open
        # run must survive.
        assert (tmp_path / "run-open.events.jsonl").exists()

        pruned_events = [
            e for e in _memory_events(rec) if e["type"] == "trace_pruned"
        ]
        assert pruned_events
        reasons = {e["details"]["reason"] for e in pruned_events}
        assert "size" in reasons
        assert rec.close(timeout_s=10.0)

    def test_error_runs_survive_when_budget_allows(self, tmp_path):
        rec = EventRecorder(
            trace_dir=tmp_path,
            observe_min_interval_s=0.0,
            trace_max_bytes=10_000_000,
        )
        self._start_and_finish_run(rec, "run-info")
        self._start_and_finish_run(rec, "run-error", severity_event="cycle_error")
        assert rec.close(timeout_s=10.0)

        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        # Drain the async startup prune (default budget: no-op) so it cannot
        # race the deterministic prune below.
        rec2._failure_archive_queue.join()
        # Budget large enough for one file: INFO-only run goes first.
        info_size = (tmp_path / "run-info.events.jsonl").stat().st_size
        error_size = (tmp_path / "run-error.events.jsonl").stat().st_size
        rec2._trace_max_bytes = error_size + info_size - 1
        summary = rec2._prune_trace_files()
        assert summary["pruned_files"] == 1
        assert not (tmp_path / "run-info.events.jsonl").exists()
        assert (tmp_path / "run-error.events.jsonl").exists()
        assert rec2.close(timeout_s=10.0)

    def test_trace_storage_status_shape(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        self._start_and_finish_run(rec, "run-x")
        rec._jsonl_queue.join()  # writes are async; flush before inspecting
        status = rec.trace_storage_status()
        assert status["enabled"] is True
        assert status["run_file_count"] >= 1
        assert status["total_bytes"] > 0
        assert isinstance(status["volatile"], bool)
        assert status["max_bytes"] == 2 * 1024 * 1024 * 1024
        assert rec.close(timeout_s=10.0)


class TestOW5DurabilityAndShutdown:
    """Review fixes: prune evidence durability, shutdown ordering, torn index."""

    def test_trace_pruned_and_clock_anchor_are_durable_despite_info(
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

        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        rec._jsonl_queue.join()
        fsynced.clear()

        rec.set_clock_anchor(monotonic_ns=1, wall_time_ns=2)
        rec._jsonl_queue.join()
        assert fsynced, "clock_anchor is INFO but must be durable"

        fsynced.clear()
        rec._record_event(
            event_type="trace_pruned",
            execution_id=0,
            details={"pruned_run_id": "run-old", "bytes": "1", "reason": "age"},
        )
        rec._jsonl_queue.join()
        assert fsynced, "trace_pruned is INFO but must be durable (520 §4.3)"

        # Control: plain INFO still batches (no fsync).
        fsynced.clear()
        rec._record_event(event_type="cache_hit", execution_id=1)
        rec._jsonl_queue.join()
        assert fsynced == []
        assert rec.close(timeout_s=10.0)

    def test_queued_prune_during_close_still_writes_evidence(self, tmp_path):
        rec = EventRecorder(
            trace_dir=tmp_path,
            observe_min_interval_s=0.0,
            trace_max_bytes=1,
        )
        # A finished old run eligible for pruning, then an open run to
        # receive the evidence.
        rec._handle_cycle_state_transition(None, "running", "run-old")
        rec._handle_cycle_state_transition("running", "idle")
        rec._handle_cycle_state_transition(None, "running", "run-live")
        rec._record_event(event_type="cycle_started", execution_id=0)
        rec._jsonl_queue.join()
        # Queue a prune and immediately close: the prune must run before the
        # JSONL writer exits, so the deletion's evidence reaches disk.
        rec._enqueue_trace_prune()
        assert rec.close(timeout_s=10.0)

        assert not (tmp_path / "run-old.events.jsonl").exists()
        live = (tmp_path / "run-live.events.jsonl").read_text()
        assert "trace_pruned" in live, (
            "prune ran during shutdown: its evidence must be on disk, "
            "not lost in a stopped writer queue"
        )

    def test_torn_index_line_does_not_break_rehydration(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-good")
        rec._handle_cycle_state_transition("running", "idle")
        assert rec.close(timeout_s=10.0)
        # Simulate a crash mid-append: torn, non-JSON final line.
        with open(tmp_path / "runs.index.jsonl", "a", encoding="utf-8") as fh:
            fh.write('{"kind": "run_started", "run_id": "run-to')
        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        assert rec2.last_run_id == "run-good"
        assert rec2.close(timeout_s=10.0)

    def test_startup_prune_evidence_cannot_resurrect_pruned_run(self, tmp_path):
        """Review round 2: after restart, `_last_run` is rehydrated, and the
        run-fallback in `_record_event` would write the startup prune's
        `trace_pruned` evidence back into the run file it just deleted —
        recreating it. System events must go to `_norun.events.jsonl` when
        no run is live."""
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-old")
        rec._record_event(event_type="cycle_started", execution_id=0)
        rec._handle_cycle_state_transition("running", "idle")
        assert rec.close(timeout_s=10.0)
        assert (tmp_path / "run-old.events.jsonl").exists()

        # Restart with a budget that forces the startup prune to evict
        # run-old. The prune runs on the worker; close() drains it (in the
        # fixed order) before stopping the writer.
        rec2 = EventRecorder(
            trace_dir=tmp_path,
            observe_min_interval_s=0.0,
            trace_max_bytes=1,
        )
        assert rec2.last_run_id == "run-old"  # rehydrated: the trap is armed
        assert rec2.close(timeout_s=10.0)

        assert not (tmp_path / "run-old.events.jsonl").exists(), (
            "startup prune evidence must not recreate the pruned run file"
        )
        norun = tmp_path / "_norun.events.jsonl"
        assert norun.exists()
        assert "trace_pruned" in norun.read_text()

    def test_startup_clock_anchor_does_not_attach_to_rehydrated_run(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-old")
        rec._record_event(event_type="cycle_started", execution_id=0)
        rec._handle_cycle_state_transition("running", "idle")
        assert rec.close(timeout_s=10.0)
        old_path = tmp_path / "run-old.events.jsonl"
        old_before = old_path.read_text()

        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        assert rec2.last_run_id == "run-old"  # rehydrated: fallback trap armed
        rec2.set_clock_anchor(monotonic_ns=11, wall_time_ns=22)
        assert rec2.close(timeout_s=10.0)

        assert old_path.read_text() == old_before
        norun = tmp_path / "_norun.events.jsonl"
        assert norun.exists()
        assert "clock_anchor" in norun.read_text()


class TestStopDecisionChains:
    """OW4: stop-decision provenance assembled from the native event stream."""

    _seq = 0

    def _ev(self, type_, ts, execution_id=7, details=None, from_state=None,
            to_state=None):
        TestStopDecisionChains._seq += 1
        e = {
            "type": type_,
            "sequence": TestStopDecisionChains._seq,
            "event_index": TestStopDecisionChains._seq,
            "execution_id": execution_id,
            "timestamp_ns": ts,
        }
        if details:
            e["details"] = details
        if from_state:
            e["from_state"] = from_state
        if to_state:
            e["to_state"] = to_state
        return e

    def _chains(self, rec):
        return [
            e for e in _memory_events(rec) if e["type"] == "stop_decision_chain"
        ]

    def test_operator_stop_chain_golden(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                # kernel stop() main path: stop_requested with EMPTY details
                self._ev("stop_requested", 1_000),
                self._ev(
                    "stop_issued", 2_000,
                    details={
                        "controlled_stop_success": "true",
                        "requested_stop_mode": "soft_on_path",
                        "executed_stop_mode": "soft_on_path",
                        "emergency_fallback": "none",
                        "hard_stop_transitioned": "false",
                    },
                ),
                self._ev(
                    "state_transition", 5_000,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 1
        d = chains[0]["details"]
        assert d["trigger"] == "operator"
        assert d["terminal_state"] == "Stopped"
        assert d["controlled_stop_success"] == "true"
        assert d["requested_stop_mode"] == "soft_on_path"
        assert d["request_to_issued_ns"] == "1000"
        assert d["issued_to_terminal_ns"] == "3000"
        assert d["request_to_terminal_ns"] == "4000"
        assert d["kernel_execution_id"] == "7"
        assert chains[0]["channel"] == "stop_decision"
        assert rec.last_stop_decision()["trigger"] == "operator"

    def test_interrupt_preemption_chain_golden(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                # Kernel act-before-emit ordering commits Interrupted before
                # publishing the latch.  Interrupted is a phase boundary only.
                self._ev(
                    "state_transition", 500,
                    from_state="Executing", to_state="Interrupted",
                    details={"reason": "interrupt_latched"},
                ),
                self._ev(
                    "interrupt_latched", 1_000,
                    details={
                        "latch_id": "interrupt-3",
                        "source_id": "plc.guard_door",
                        "policy_id": "door_policy",
                        "preemption_action": "preempt_trajectory",
                        "preemption_result": "pending",
                    },
                ),
                self._ev(
                    "interrupt_preempt_applied", 2_500,
                    details={
                        "latch_id": "interrupt-3",
                        "preemption_action": "preempt_trajectory",
                        "preemption_result": "succeeded",
                    },
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 1
        d = chains[0]["details"]
        assert d["trigger"] == "interrupt"
        assert d["latch_id"] == "interrupt-3"
        assert d["policy_id"] == "door_policy"
        assert d["preemption_action"] == "preempt_trajectory"
        assert d["preemption_result"] == "succeeded"
        assert d["terminal_state"] == "Interrupted"
        assert d["terminal_reason"] == "interrupt_latched"
        assert d["latch_to_terminal_ns"] == "1500"
        assert d["chain_complete"] == "true"

    def test_fault_escalation_with_override_chain_golden(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev(
                    "interrupt_latched", 1_000,
                    details={
                        "latch_id": "interrupt-9",
                        "source_id": "manyforge.kernel.runtime_collision_fault",
                        "policy_id": (
                            "manyforge.kernel.runtime_collision_fault"
                            ".preempt_trajectory"
                        ),
                        "preemption_action": "preempt_trajectory",
                    },
                ),
                self._ev("stop_requested", 1_500),
                self._ev(
                    "state_transition", 6_000,
                    from_state="Idle", to_state="InterventionRequired",
                    details={
                        "reason": "fault_preempt_pending_timeout",
                        "terminal_authority_override": "true",
                        "proposed_terminal": "Stopped",
                    },
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 1
        d = chains[0]["details"]
        assert d["trigger"] == "fault"
        assert d["terminal_state"] == "InterventionRequired"
        assert d["override"] == "true"
        assert d["proposed_terminal"] == "Stopped"
        assert d["terminal_reason"] == "fault_preempt_pending_timeout"

    def test_reset_trigger_and_unobserved_terminal(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev(
                    "stop_requested", 1_000,
                    details={"reason": "reset_stop_before_reset"},
                ),
                self._ev(
                    "state_transition", 2_000,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        assert self._chains(rec)[-1]["details"]["trigger"] == "reset"

        # Interrupted alone is not a stop terminal.  It remains pending until a
        # definitive outcome or an explicit incomplete boundary is observed.
        rec.record_native_kernel_events(
            [
                self._ev(
                    "state_transition", 9_000, execution_id=8,
                    from_state="Executing", to_state="Interrupted",
                ),
            ]
        )
        assert len(self._chains(rec)) == 1
        assert rec.close(timeout_s=10.0)
        last = self._chains(rec)[-1]["details"]
        assert last["trigger"] == "unobserved"
        assert last["kernel_execution_id"] == "8"
        assert last["chain_complete"] == "false"
        assert last["incomplete_reason"] == "process_exit_before_stop_outcome"

    def test_resume_abandons_episode_and_duplicate_terminal_suppressed(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                # pause-like flow: stop requested, but execution resumes
                self._ev("stop_requested", 1_000),
                self._ev(
                    "state_transition", 2_000,
                    from_state="Paused", to_state="Executing",
                ),
                # then a real stop later
                self._ev("stop_requested", 3_000),
                self._ev(
                    "state_transition", 4_000,
                    from_state="Stopping", to_state="Stopped",
                ),
                # commit-override duplicate terminal for the same execution
                self._ev(
                    "state_transition", 4_100,
                    from_state="Idle", to_state="Stopped",
                    details={"terminal_authority_override": "true"},
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 1  # abandoned episode: 0; real stop: 1; dup: 0
        assert chains[0]["details"]["trigger"] == "operator"
        assert chains[0]["details"]["request_to_terminal_ns"] == "1000"

    def test_chain_is_durable_and_in_digest_shape(self, tmp_path):
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-stop")
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000),
                self._ev(
                    "state_transition", 2_000,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._jsonl_queue.join()
        run_file = (tmp_path / "run-stop.events.jsonl").read_text()
        run_events = [json.loads(line) for line in run_file.splitlines()]
        chain_event = next(
            event for event in run_events if event["type"] == "stop_decision_chain"
        )
        assert chain_event["details"]["recorded_run_id"] == "run-stop"
        assert chain_event["details"]["recorded_event_index"] == str(
            chain_event["event_index"]
        )
        assert rec.last_stop_decision()["trigger"] == "operator"
        assert rec.last_stop_decision()["recorded_run_id"] == "run-stop"
        assert rec.close(timeout_s=10.0)

    def test_incident_snapshot_embeds_matching_stop_decision(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=42),
                self._ev(
                    "state_transition", 2_000, execution_id=42,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        # An ERROR event for the same execution triggers an incident.
        rec._record_event(
            event_type="error_raised",
            execution_id=42,
            details={"error": "post-stop fault"},
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["stopDecision"] is not None
        assert snapshot["stopDecision"]["kernel_execution_id"] == "42"

    def test_later_escalation_of_stopped_execution_gets_its_own_chain(self):
        """Adversarial finding 1: Stopped -> (reset fails) ->
        InterventionRequired for the SAME execution is a NEW decision, not a
        duplicate of the commit-override pair."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=7),
                self._ev(
                    "state_transition", 2_000, execution_id=7,
                    from_state="Stopping", to_state="Stopped",
                ),
                # commit-override duplicate: same execution, same to_state
                self._ev(
                    "state_transition", 2_100, execution_id=7,
                    from_state="Idle", to_state="Stopped",
                    details={"terminal_authority_override": "true"},
                ),
                # reset fails while Stopped: DIFFERENT terminal state
                self._ev(
                    "state_transition", 9_000, execution_id=7,
                    from_state="Stopped", to_state="InterventionRequired",
                    details={"reason": "backend_reset"},
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 2
        assert chains[0]["details"]["terminal_state"] == "Stopped"
        assert chains[1]["details"]["terminal_state"] == "InterventionRequired"
        assert chains[1]["details"]["trigger"] == "reset"
        assert chains[1]["details"]["chain_complete"] == "false"
        # last_stop_decision reflects the escalation, not the stale stop
        assert rec.last_stop_decision()["terminal_state"] == "InterventionRequired"

    def test_pending_stop_admission_race_still_classifies_operator(self):
        """Adversarial finding 2: stop_requested(lifecycle_busy) ->
        ->Planning (abandons) -> stop_issued -> ->Stopped must classify as
        operator via the issued evidence, not degrade to unobserved."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev(
                    "stop_requested", 1_000, execution_id=8,
                    details={"reason": "lifecycle_busy"},
                ),
                self._ev(
                    "state_transition", 1_100, execution_id=8,
                    from_state="Idle", to_state="Planning",
                ),
                self._ev("stop_issued", 1_200, execution_id=8),
                self._ev(
                    "state_transition", 1_300, execution_id=8,
                    from_state="Planning", to_state="Stopped",
                    details={"reason": "pending_stop_during_execute_admission"},
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 1
        d = chains[0]["details"]
        assert d["trigger"] == "operator"
        assert d["issued_to_terminal_ns"] == "100"

    def test_failed_stop_attempt_closes_episode_as_chain(self):
        """Adversarial finding 3: Stopping -> Failed with an open episode is
        the most valuable chain (a stop that FAILED) and must not leak or
        conflate with a later retry."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=9),
                self._ev("stop_issued", 1_500, execution_id=9),
                self._ev(
                    "state_transition", 3_000, execution_id=9,
                    from_state="Stopping", to_state="Failed",
                ),
                # later retry: its own episode, no conflated timings
                self._ev(
                    "stop_requested", 60_000, execution_id=9,
                    details={"reason": "operator_retry"},
                ),
                self._ev(
                    "state_transition", 61_000, execution_id=9,
                    from_state="Failed", to_state="InterventionRequired",
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 2
        first = chains[0]["details"]
        assert first["terminal_state"] == "Failed"
        assert first["trigger"] == "operator"
        assert first["request_to_terminal_ns"] == "2000"
        second = chains[1]["details"]
        assert second["terminal_state"] == "InterventionRequired"
        assert second["request_to_terminal_ns"] == "1000"
        assert second["stop_reason"] == "operator_retry"
        # plain Failed without any episode still yields no chain
        rec.record_native_kernel_events(
            [
                self._ev(
                    "state_transition", 70_000, execution_id=10,
                    from_state="Planning", to_state="Failed",
                ),
            ]
        )
        assert len(self._chains(rec)) == 2

    def test_cross_execution_terminal_does_not_merge_episodes(self):
        """Adversarial finding 4: a terminal from execution B must not close
        execution A's episode into a hybrid chain."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=7),
                self._ev(
                    "state_transition", 2_000, execution_id=8,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        chains = self._chains(rec)
        assert len(chains) == 2
        stale, terminal = (chain["details"] for chain in chains)
        assert stale["kernel_execution_id"] == "7"
        assert stale["chain_complete"] == "false"
        assert stale["incomplete_reason"] == (
            "superseded_by_other_execution_terminal"
        )
        assert terminal["kernel_execution_id"] == "8"
        assert terminal["trigger"] == "unobserved"
        assert terminal["chain_complete"] == "false"
        assert "request_to_terminal_ns" not in terminal

    def test_kernel_event_gap_invalidates_open_episode(self):
        """Hardening: loss between episode events poisons the assembly."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [self._ev("stop_requested", 1_000, execution_id=11)]
        )
        # Sequence jump => gap record => episode invalidated.
        gap_ev = self._ev(
            "state_transition", 5_000, execution_id=11,
            from_state="Stopping", to_state="Stopped",
        )
        gap_ev["sequence"] = gap_ev["sequence"] + 50
        TestStopDecisionChains._seq += 50
        rec.record_native_kernel_events([gap_ev])
        chains = self._chains(rec)
        assert len(chains) == 2
        before_gap, terminal = (chain["details"] for chain in chains)
        assert before_gap["trigger"] == "operator"
        assert before_gap["chain_complete"] == "false"
        assert before_gap["incomplete_reason"] == "kernel_sequence_gap"
        assert before_gap["missing_sequence_start"]
        assert before_gap["missing_sequence_end"]
        assert terminal["trigger"] == "unobserved"
        assert terminal["chain_complete"] == "false"

    def test_runtime_collision_success_is_sequence_authoritative_across_drains(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        collision = self._ev(
            "collision_detected",
            1_000,
            execution_id=81,
            details={"reason": "runtime_collision_monitor", "contact_a": "arm"},
        )
        interrupted = self._ev(
            "state_transition",
            1_100,
            execution_id=81,
            from_state="Executing",
            to_state="Interrupted",
            details={"reason": "interrupt_latched"},
        )
        latched = self._ev(
            "interrupt_latched",
            1_200,
            execution_id=81,
            details={
                "latch_id": "collision-latch-81",
                "source_id": "manyforge.kernel.runtime_collision_fault",
                "policy_id": "runtime-collision-policy",
            },
        )
        coverage = self._ev(
            "runtime_collision_coverage",
            1_300,
            execution_id=81,
            details={
                "coverage_complete": "true",
                "ticks_attempted": "8",
                "collision_queries_executed": "8",
            },
        )
        certificate = self._ev(
            "stop_certificate_published",
            1_400,
            execution_id=81,
            details={
                "latch_id": "collision-latch-81",
                "certificate_id": "stopcert-81",
                "completion_result": "succeeded",
                "stop_owner": "fault_preempt_worker",
                "backend_stop_confirmed": "true",
            },
        )
        preempted = self._ev(
            "interrupt_preempt_applied",
            1_500,
            execution_id=81,
            details={
                "latch_id": "collision-latch-81",
                "preemption_action": "preempt_trajectory",
                "preemption_result": "succeeded",
            },
        )

        rec.record_native_kernel_events([collision, interrupted])
        assert self._chains(rec) == []
        # Arrival order within the second drain is deliberately reversed.  The
        # assembler must use kernel sequence, not list/recorder order.
        reversed_arrival = [preempted, certificate, coverage, latched]
        next_arrival_index = interrupted["event_index"] + 1
        for offset, event in enumerate(reversed_arrival):
            event["event_index"] = next_arrival_index + offset
        rec.record_native_kernel_events(reversed_arrival)

        chains = self._chains(rec)
        assert len(chains) == 1
        details = chains[0]["details"]
        assert details["chain_complete"] == "true"
        assert details["trigger"] == "fault"
        assert details["latch_id"] == "collision-latch-81"
        assert details["stop_certificate_id"] == "stopcert-81"
        assert details["physical_stop_result"] == "succeeded"
        assert details["stop_monitor_coverage_complete"] == "true"
        assert details["native_sequence_start"] == str(collision["sequence"])
        assert details["native_sequence_end"] == str(preempted["sequence"])
        assert "stop_decision_id" in details

    def test_runtime_collision_failure_waits_for_intervention_terminal(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        first_batch = [
            self._ev(
                "collision_detected", 1_000, execution_id=82,
                details={"reason": "runtime_collision_monitor"},
            ),
            self._ev(
                "state_transition", 1_050, execution_id=82,
                from_state="Executing", to_state="Interrupted",
            ),
            self._ev(
                "interrupt_latched", 1_100, execution_id=82,
                details={
                    "latch_id": "collision-latch-82",
                    "source_id": "manyforge.kernel.runtime_collision_fault",
                },
            ),
            self._ev(
                "stop_certificate_published", 1_200, execution_id=82,
                details={
                    "latch_id": "collision-latch-82",
                    "certificate_id": "stopcert-82",
                    "completion_result": "backend_preempt_failed",
                    "stop_owner": "fault_preempt_worker",
                    "backend_stop_confirmed": "false",
                },
            ),
            self._ev(
                "interrupt_preempt_applied", 1_300, execution_id=82,
                details={
                    "latch_id": "collision-latch-82",
                    "preemption_action": "preempt_trajectory",
                    "preemption_result": "backend_preempt_failed",
                },
            ),
        ]
        rec.record_native_kernel_events(first_batch)
        assert self._chains(rec) == []

        rec.record_native_kernel_events(
            [
                self._ev(
                    "state_transition", 1_400, execution_id=82,
                    from_state="Interrupted", to_state="InterventionRequired",
                    details={"reason": "fault_stop_total_failure"},
                )
            ]
        )
        details = self._chains(rec)[0]["details"]
        assert details["chain_complete"] == "true"
        assert details["trigger"] == "fault"
        assert details["terminal_state"] == "InterventionRequired"
        assert details["terminal_reason"] == "fault_stop_total_failure"
        assert details["physical_stop_result"] == "backend_preempt_failed"
        assert details["stop_certificate_id"] == "stopcert-82"

    def test_production_reorder_buffer_spans_drain_boundary(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        terminal = {
            "type": "state_transition",
            "sequence": 2,
            "event_index": 1,
            "execution_id": 91,
            "timestamp_ns": 2_000,
            "from_state": "Stopping",
            "to_state": "Stopped",
        }
        requested = {
            "type": "stop_requested",
            "sequence": 1,
            "event_index": 2,
            "execution_id": 91,
            "timestamp_ns": 1_000,
        }
        status = {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 2,
        }

        # Sequence 2 reaches the collector first. It must remain buffered and
        # cannot create an unobserved terminal chain.
        rec.record_native_kernel_events([terminal], pipeline_status=status)
        assert self._chains(rec) == []
        assert not [
            event
            for event in _memory_events(rec)
            if event["type"].startswith("kernel_native_")
        ]

        rec.record_native_kernel_events([requested], pipeline_status=status)
        chains = self._chains(rec)
        assert len(chains) == 1
        details = chains[0]["details"]
        assert details["chain_complete"] == "true"
        assert details["trigger"] == "operator"
        assert details["native_sequence_start"] == "1"
        assert details["native_sequence_end"] == "2"
        raw_sequences = [
            int(event["details"]["kernel_sequence"])
            for event in _memory_events(rec)
            if event["type"].startswith("kernel_native_")
        ]
        assert raw_sequences == [1, 2]

    def test_concurrent_drains_cannot_overtake_staged_native_sequence(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        first_staged = threading.Event()
        release_first = threading.Event()
        original = rec._record_sequence_ready_native_events

        def gated(events):
            if events and int(events[0].get("sequence") or 0) == 1:
                first_staged.set()
                assert release_first.wait(timeout=2.0)
            original(events)

        rec._record_sequence_ready_native_events = gated
        status = {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 2,
        }
        requested = {
            "type": "stop_requested",
            "sequence": 1,
            "event_index": 1,
            "execution_id": 92,
            "timestamp_ns": 1_000,
        }
        terminal = {
            "type": "state_transition",
            "sequence": 2,
            "event_index": 2,
            "execution_id": 92,
            "timestamp_ns": 2_000,
            "from_state": "Stopping",
            "to_state": "Stopped",
        }
        first = threading.Thread(
            target=rec.record_native_kernel_events,
            args=([requested],),
            kwargs={"pipeline_status": status},
        )
        second = threading.Thread(
            target=rec.record_native_kernel_events,
            args=([terminal],),
            kwargs={"pipeline_status": status},
        )
        first.start()
        assert first_staged.wait(timeout=2.0)
        second.start()
        time.sleep(0.02)
        assert second.is_alive()
        release_first.set()
        first.join(timeout=2.0)
        second.join(timeout=2.0)
        assert not first.is_alive()
        assert not second.is_alive()

        chains = self._chains(rec)
        assert len(chains) == 1
        assert chains[0]["details"]["chain_complete"] == "true"
        assert not [
            event
            for event in _memory_events(rec)
            if event["type"] == "kernel_event_gap"
        ]

    def test_empty_drop_observation_advances_later_reorder_cursor(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 93,
                    "timestamp_ns": 1_000,
                }
            ],
            pipeline_status={
                "collector_dropped_total": 0,
                "manager_dropped_total": 0,
                "last_sequence_seen": 1,
            },
        )
        # Sequence 2 was observed and dropped before this empty drain.
        rec.record_native_kernel_events(
            [],
            pipeline_status={
                "collector_dropped_total": 1,
                "manager_dropped_total": 0,
                "last_sequence_seen": 2,
            },
        )
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 3,
                    "event_index": 2,
                    "execution_id": 94,
                    "timestamp_ns": 3_000,
                },
                {
                    "type": "state_transition",
                    "sequence": 4,
                    "event_index": 3,
                    "execution_id": 94,
                    "timestamp_ns": 4_000,
                    "from_state": "Stopping",
                    "to_state": "Stopped",
                },
            ],
            pipeline_status={
                "collector_dropped_total": 1,
                "manager_dropped_total": 0,
                "last_sequence_seen": 4,
            },
        )

        chains = self._chains(rec)
        assert len(chains) == 2
        assert chains[0]["details"]["chain_complete"] == "false"
        assert chains[0]["details"]["incomplete_reason"] == (
            "pipeline_drop_counter_advanced"
        )
        assert chains[1]["details"]["kernel_execution_id"] == "94"
        assert chains[1]["details"]["chain_complete"] == "true"
        assert rec._pending_native_events_by_sequence == {}

    def test_normal_completion_is_not_misreported_as_stop_decision(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "state_transition",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 95,
                    "timestamp_ns": 1_000,
                    "from_state": "Executing",
                    "to_state": "Completed",
                }
            ]
        )
        assert self._chains(rec) == []

    def test_stop_intent_followed_by_completion_is_explicitly_incomplete(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 96,
                    "timestamp_ns": 1_000,
                },
                {
                    "type": "state_transition",
                    "sequence": 2,
                    "event_index": 2,
                    "execution_id": 96,
                    "timestamp_ns": 2_000,
                    "from_state": "Stopping",
                    "to_state": "Completed",
                },
            ]
        )
        details = self._chains(rec)[0]["details"]
        assert details["chain_complete"] == "false"
        assert details["completion_boundary"] == (
            "execution_progressed_without_stop_terminal"
        )
        assert details["incomplete_reason"] == (
            "execution_completed_without_stop_terminal"
        )

    def test_kernel_source_rotation_fences_sequences_and_open_episode(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        status = {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 1,
        }
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 101,
                    "timestamp_ns": 1_000,
                }
            ],
            pipeline_status=status,
        )
        rec.rotate_kernel_event_source(
            kernel_replaced=True,
            reason="test_kernel_replacement",
        )

        first = self._chains(rec)[0]["details"]
        assert first["chain_complete"] == "false"
        assert first["incomplete_reason"] == "test_kernel_replacement"
        assert first["kernel_event_source_generation"] == "1"

        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 102,
                    "timestamp_ns": 2_000,
                },
                {
                    "type": "state_transition",
                    "sequence": 2,
                    "event_index": 2,
                    "execution_id": 102,
                    "timestamp_ns": 3_000,
                    "from_state": "Stopping",
                    "to_state": "Stopped",
                },
            ],
            pipeline_status={**status, "last_sequence_seen": 2},
        )
        second = self._chains(rec)[1]["details"]
        assert second["chain_complete"] == "true"
        assert second["kernel_execution_id"] == "102"
        assert second["kernel_event_source_generation"] == "2"

    def test_late_old_source_batch_is_rejected_after_rotation(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        status = {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 1,
        }
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 103,
                    "timestamp_ns": 1_000,
                }
            ],
            pipeline_status=status,
            source_identity=1001,
        )
        rec.rotate_kernel_event_source(
            kernel_replaced=True,
            reason="test_source_identity_rotation",
            next_source_identity=1002,
        )
        rec.record_native_kernel_events(
            [
                {
                    "type": "state_transition",
                    "sequence": 2,
                    "event_index": 2,
                    "execution_id": 103,
                    "timestamp_ns": 2_000,
                    "from_state": "Stopping",
                    "to_state": "Stopped",
                }
            ],
            pipeline_status={**status, "last_sequence_seen": 2},
            source_identity=1001,
        )

        gaps = [
            event
            for event in _memory_events(rec)
            if event["type"] == "kernel_event_gap"
        ]
        assert gaps[-1]["details"]["reason"] == (
            "stale_kernel_event_source_rejected"
        )
        assert not [
            event
            for event in _memory_events(rec)
            if event["type"] == "kernel_native_state_transition"
            and event["execution_id"] == 103
        ]

    def test_empty_old_source_callback_after_rotation_is_not_false_loss(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                {
                    "type": "stop_requested",
                    "sequence": 1,
                    "event_index": 1,
                    "execution_id": 104,
                    "timestamp_ns": 1_000,
                }
            ],
            pipeline_status={
                "collector_dropped_total": 0,
                "manager_dropped_total": 0,
                "last_sequence_seen": 1,
            },
            source_identity=2001,
        )
        rec.rotate_kernel_event_source(
            kernel_replaced=True,
            reason="test_empty_source_identity_rotation",
            next_source_identity=2002,
        )
        gaps_before = rec.kernel_event_gap_status()["gap_records_total"]

        rec.record_native_kernel_events(
            [],
            pipeline_status={
                "collector_dropped_total": 7,
                "manager_dropped_total": 3,
                "last_sequence_seen": 11,
            },
            source_identity=2001,
        )

        assert rec.kernel_event_gap_status()["gap_records_total"] == gaps_before

    def test_stale_chain_not_inherited_by_unrelated_stop_channel_incident(self):
        """Embed-selector hardening: a stop/fault-channel incident in a LATER
        run must not inherit the previous run's chain (the old channel-only
        fallback did exactly that)."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        # Run A: a stop with a chain.
        rec._handle_cycle_state_transition(None, "running", "run-a")
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=21),
                self._ev(
                    "state_transition", 2_000, execution_id=21,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._handle_cycle_state_transition("running", "idle")
        # Run B: an unrelated controlled-stop-channel ERROR with no exec id.
        rec._handle_cycle_state_transition(None, "running", "run-b")
        rec._record_event(
            event_type="error_raised",
            execution_id=0,
            details={
                "error": "unrelated failure",
                "requested_stop_mode": "soft_on_path",  # routes to controlled_stop channel
            },
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["stopDecision"] is None
        assert snapshot["stopDecisionCorrelation"] is None

    def test_execid_zero_stop_incident_same_run_recent_gets_tagged_embed(self):
        """The fallback that motivated the old heuristic still works — but
        same-run + recent, and labeled as a proximity match."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-c")
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=33),
                self._ev(
                    "state_transition", 2_000, execution_id=33,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._record_event(
            event_type="error_raised",
            execution_id=0,
            details={
                "error": "stop aftermath",
                "requested_stop_mode": "soft_on_path",
            },
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["stopDecision"] is not None
        assert snapshot["stopDecision"]["kernel_execution_id"] == "33"
        assert (
            snapshot["stopDecisionCorrelation"] == "same_run_recent_stop_channel"
        )

    def test_exact_execution_match_still_tagged_exact(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=44),
                self._ev(
                    "state_transition", 2_000, execution_id=44,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._record_event(
            event_type="error_raised",
            execution_id=44,
            details={"error": "post-stop fault"},
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["stopDecision"] is not None
        assert snapshot["stopDecisionCorrelation"] == "execution_id_match"

    def test_nonzero_mismatched_execution_id_does_not_use_proximity_fallback(self):
        """Only execution_id=0/missing triggers may use same-run proximity.
        A different nonzero id is contrary evidence, not a weak match."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-mismatch")
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=55),
                self._ev(
                    "state_transition", 2_000, execution_id=55,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._record_event(
            event_type="error_raised",
            execution_id=56,
            details={
                "error": "different execution",
                "requested_stop_mode": "soft_on_path",
            },
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["stopDecision"] is None
        assert snapshot["stopDecisionCorrelation"] is None

    def test_execid_zero_post_run_same_run_uses_trigger_run_anchor(self):
        """Same-run means the trigger event's recorded run_id, not whatever
        run is currently live while the incident snapshot is built."""
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._handle_cycle_state_transition(None, "running", "run-finished")
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000, execution_id=66),
                self._ev(
                    "state_transition", 2_000, execution_id=66,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        rec._handle_cycle_state_transition("running", "idle")
        rec._record_event(
            event_type="error_raised",
            execution_id=0,
            details={
                "error": "late stop aftermath",
                "requested_stop_mode": "soft_on_path",
            },
        )
        snapshot = rec.incident_snapshots()[-1]
        assert snapshot["triggerEvent"]["run_id"] == "run-finished"
        assert snapshot["stopDecision"] is not None
        assert snapshot["stopDecision"]["kernel_execution_id"] == "66"
        assert (
            snapshot["stopDecisionCorrelation"] == "same_run_recent_stop_channel"
        )


class TestRecoveryEvidenceCorrelation:
    def test_runtime_fault_incident_offer_and_action_share_exact_context(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        manager = _StubCycleManager(
            "running",
            error="Model output could not be committed",
        )
        manager.get_error_snapshot = lambda: (
            manager.error,
            "model_output_commit_failed",
            {
                "reason_code": "model_output_commit_failed",
                "runtime_fault_id": "fault-opaque-23",
                "runtime_fault_generation": 23,
                "runtime_fault_domain": "model",
            },
        )
        rec.observe(_StubState(cycle_manager=manager))
        rec.record_recovery_offer_published(
            {
                "offerId": "offer-runtime-fault-incident",
                "offerRevision": 1,
                "executionId": 0,
                "evidenceRefs": ["runtime_fault:fault-opaque-23"],
            }
        )
        rec.record_recovery_action_event(
            "accepted",
            {
                "request_id": "request-runtime-fault-incident",
                "offer_id": "offer-runtime-fault-incident",
                "offer_revision": 1,
                "execution_id": 0,
                "evidence_refs": ["runtime_fault:fault-opaque-23"],
                "action_id": "retry_runtime_step",
                "principal_id": "operator:test",
            },
        )

        events = _memory_events(rec)
        cycle_error = next(e for e in events if e["type"] == "cycle_error")
        offer = next(e for e in events if e["type"] == "recovery_offer_published")
        action = next(e for e in events if e["type"] == "recovery_action_accepted")
        incident = rec.incident_snapshots()[-1]
        context_id = incident["recoveryContextId"]
        assert cycle_error["details"]["error_details"]["runtime_fault_id"] == (
            "fault-opaque-23"
        )
        assert offer["details"]["recovery_context_id"] == context_id
        assert offer["details"]["incident_id"] == incident["incidentId"]
        assert action["details"]["recovery_context_id"] == context_id
        assert action["details"]["incident_id"] == incident["incidentId"]

    def test_cycle_only_offer_and_action_share_runtime_fault_context(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._start_run("run-cycle-only-recovery")
        rec.record_recovery_offer_published(
            {
                "offerId": "offer-runtime-fault",
                "offerRevision": 1,
                "executionId": 0,
                "evidenceRefs": ["runtime_fault:fault-opaque-17"],
            }
        )
        rec.record_recovery_action_event(
            "accepted",
            {
                "request_id": "request-runtime-fault",
                "offer_id": "offer-runtime-fault",
                "offer_revision": 1,
                "execution_id": 0,
                "evidence_refs": ["runtime_fault:fault-opaque-17"],
                "action_id": "retry_runtime_step",
                "principal_id": "operator:test",
            },
        )

        events = _memory_events(rec)
        offer = next(e for e in events if e["type"] == "recovery_offer_published")
        action = next(e for e in events if e["type"] == "recovery_action_accepted")
        context_id = offer["details"]["recovery_context_id"]
        assert context_id.startswith("recovery-context-")
        assert action["details"]["recovery_context_id"] == context_id

    def test_offer_chain_action_and_incident_share_stable_ids(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec._start_run("run-recovery-correlation")
        rec._record_event(
            event_type="error_raised",
            execution_id=42,
            details={"latch_id": "latch-42", "reason_code": "runtime_collision"},
        )
        rec.record_recovery_offer_published(
            {
                "offerId": "offer-42",
                "offerRevision": 1,
                "executionId": 42,
                "latchId": "latch-42",
                "stopCertificateId": "stopcert-42",
            }
        )
        rec._record_event(
            event_type="stop_decision_chain",
            execution_id=42,
            details={
                "chain_complete": "true",
                "kernel_execution_id": "42",
                "latch_id": "latch-42",
                "terminal_state": "Stopped",
                "terminal_kernel_sequence": "91",
            },
            system_event=True,
        )
        rec.record_recovery_action_event(
            "accepted",
            {
                "request_id": "request-42",
                "offer_id": "offer-42",
                "offer_revision": 1,
                "execution_id": 42,
                "latch_id": "latch-42",
                "action_id": "retry_motion_from_current_state",
                "principal_id": "operator:test",
            },
        )

        events = _memory_events(rec)
        offer = next(e for e in events if e["type"] == "recovery_offer_published")
        chain = next(e for e in events if e["type"] == "stop_decision_chain")
        action = next(e for e in events if e["type"] == "recovery_action_accepted")
        snapshot = rec.incident_snapshots()[-1]

        context_id = chain["details"]["recovery_context_id"]
        decision_id = chain["details"]["stop_decision_id"]
        incident_id = snapshot["incidentId"]
        assert offer["details"]["recovery_context_id"] == context_id
        assert offer["details"]["incident_id"] == incident_id
        assert chain["details"]["incident_id"] == incident_id
        assert action["details"]["recovery_context_id"] == context_id
        assert action["details"]["stop_decision_id"] == decision_id
        assert action["details"]["incident_id"] == incident_id
        assert snapshot["recoveryContextId"] == context_id
        assert snapshot["stopDecisionId"] == decision_id
        assert snapshot["stopDecision"]["stop_decision_id"] == decision_id

class TestLatencyHistograms:
    """OW6: per-run latency histograms derived on the recorder thread from
    event timestamps the pipeline already carries."""

    _seq = 0

    def _ev(self, type_, ts, execution_id=7, details=None, from_state=None,
            to_state=None):
        TestLatencyHistograms._seq += 1
        e = {
            "type": type_,
            "sequence": TestLatencyHistograms._seq,
            "event_index": TestLatencyHistograms._seq,
            "execution_id": execution_id,
            "timestamp_ns": ts,
        }
        if details:
            e["details"] = details
        if from_state:
            e["from_state"] = from_state
        if to_state:
            e["to_state"] = to_state
        return e

    def _metrics(self, rec):
        return rec.latency_histograms()["current_run"]["metrics"]

    # -- histogram math ----------------------------------------------------

    def test_bucket_assignment_and_stats_golden(self):
        h = event_recorder_module._LatencyHistogram()
        h.observe(500_000)        # <= 1ms -> bucket 0
        h.observe(1_000_000)      # == bound -> bucket 0
        h.observe(1_500_000)      # bucket 1 (2ms)
        h.observe(9_000_000)      # bucket 3 (10ms)
        payload = h.to_payload()
        assert payload["count"] == 4
        assert payload["sum_ns"] == 12_000_000
        assert payload["min_ns"] == 500_000
        assert payload["max_ns"] == 9_000_000
        assert payload["bucket_counts"][0] == 2
        assert payload["bucket_counts"][1] == 1
        assert payload["bucket_counts"][3] == 1
        assert sum(payload["bucket_counts"]) == 4

    def test_percentile_interpolation_golden(self):
        h = event_recorder_module._LatencyHistogram()
        # 100 samples all in the 1ms..2ms bucket: p50 interpolates to the
        # bucket midpoint, p99 to 99% of the way through it.
        for _ in range(100):
            h.observe(1_500_000)
        assert h.percentile_ns(0.50) == 1_500_000
        assert h.percentile_ns(0.99) == 1_990_000

    def test_overflow_negative_and_empty_edges(self):
        h = event_recorder_module._LatencyHistogram()
        assert h.percentile_ns(0.50) is None       # empty
        h.observe(-5)                              # ignored
        assert h.count == 0
        h.observe(60_000_000_000)                  # > 10s -> overflow bucket
        payload = h.to_payload()
        assert payload["bucket_counts"][-1] == 1
        # Overflow percentile is honest about resolution: last finite bound.
        assert payload["p99_ns"] == 10_000_000_000
        assert payload["max_ns"] == 60_000_000_000

    # -- duration assembly from the native stream ---------------------------

    def test_plan_validate_execute_durations_assembled(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("plan_started", 1_000_000),
                self._ev("plan_completed", 4_000_000),
                self._ev("validation_started", 5_000_000),
                self._ev("validation_passed", 7_000_000),
                self._ev("execute_started", 8_000_000),
                self._ev("execute_completed", 20_000_000),
            ]
        )
        metrics = self._metrics(rec)
        assert metrics["plan"]["count"] == 1
        assert metrics["plan"]["sum_ns"] == 3_000_000
        assert metrics["validate"]["count"] == 1
        assert metrics["validate"]["sum_ns"] == 2_000_000
        assert metrics["execute"]["count"] == 1
        assert metrics["execute"]["sum_ns"] == 12_000_000
        assert metrics["stop"]["count"] == 0

    def test_validation_failure_still_observes_duration(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("validation_started", 1_000_000),
                self._ev("validation_failed", 3_500_000),
            ]
        )
        validate = self._metrics(rec)["validate"]
        assert validate["count"] == 1
        assert validate["sum_ns"] == 2_500_000

    def test_interleaved_executions_do_not_cross_contaminate(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("plan_started", 1_000_000, execution_id=1),
                self._ev("plan_started", 2_000_000, execution_id=2),
                self._ev("plan_completed", 10_000_000, execution_id=1),
                self._ev("plan_completed", 3_000_000, execution_id=2),
            ]
        )
        plan = self._metrics(rec)["plan"]
        assert plan["count"] == 2
        # 9ms (exec 1) + 1ms (exec 2); a keying bug would yield 8ms + 2ms
        # or drop one of the pair.
        assert plan["sum_ns"] == 10_000_000
        assert plan["min_ns"] == 1_000_000
        assert plan["max_ns"] == 9_000_000

    def test_unmatched_or_unstamped_completions_ignored(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                # completion with no start
                self._ev("plan_completed", 5_000_000, execution_id=3),
                # start without kernel timestamp is never trusted
                self._ev("execute_started", 0, execution_id=4),
                self._ev("execute_completed", 9_000_000, execution_id=4),
                # completion stamped BEFORE its start (clock regression)
                self._ev("plan_started", 8_000_000, execution_id=5),
                self._ev("plan_completed", 7_000_000, execution_id=5),
            ]
        )
        metrics = self._metrics(rec)
        assert metrics["plan"]["count"] == 0
        assert metrics["execute"]["count"] == 0

    def test_pending_cap_evicts_oldest_start(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        cap = event_recorder_module._LATENCY_PENDING_CAP
        rec.record_native_kernel_events(
            [
                self._ev("plan_started", 1_000_000 + i, execution_id=100 + i)
                for i in range(cap + 1)
            ]
        )
        rec.record_native_kernel_events(
            [
                # oldest (100) was evicted; newest survives
                self._ev("plan_completed", 9_000_000, execution_id=100),
                self._ev(
                    "plan_completed", 9_000_000, execution_id=100 + cap
                ),
            ]
        )
        plan = self._metrics(rec)["plan"]
        assert plan["count"] == 1

    def test_stop_latency_fed_from_decision_chain(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000),
                self._ev("stop_issued", 2_000),
                self._ev(
                    "state_transition", 5_000,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        stop = self._metrics(rec)["stop"]
        assert stop["count"] == 1
        assert stop["sum_ns"] == 4_000  # request -> terminal

    # -- run lifecycle -------------------------------------------------------

    def test_per_run_reset_and_last_run_stash(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        first_run_id = rec.current_run_id
        rec.record_native_kernel_events(
            [
                self._ev("plan_started", 1_000_000),
                self._ev("plan_completed", 2_000_000),
            ]
        )
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        payload = rec.latency_histograms()
        assert payload["schema_version"] == "manyforge.latency_histograms.v1"
        assert payload["current_run"]["run_id"] == rec.current_run_id
        assert payload["current_run"]["run_id"] != first_run_id
        assert payload["current_run"]["metrics"]["plan"]["count"] == 0
        assert payload["last_run"]["run_id"] == first_run_id
        assert payload["last_run"]["metrics"]["plan"]["count"] == 1

    def test_stop_latency_captures_issued_only_operator_stop(self):
        # The pending-stop execute-admission path opens a stop episode on
        # stop_issued alone (no stop_requested, no interrupt_latched); the
        # resulting chain carries only issued_to_terminal_ns. That operator
        # stop must still be observed, not silently dropped from the histogram.
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.record_native_kernel_events(
            [
                self._ev("stop_issued", 1_000, execution_id=5),
                self._ev(
                    "state_transition", 1_500, execution_id=5,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        stop = self._metrics(rec)["stop"]
        assert stop["count"] == 1
        assert stop["sum_ns"] == 500  # issued -> terminal

    def test_trailing_completion_after_finish_attributed_to_that_run(self):
        # A completion observed in the window between run finish and the next
        # run's start must land in the finishing run's histogram — the stash is
        # taken at the next start, after this sample has been paired.
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        first_run_id = rec.current_run_id
        rec.record_native_kernel_events(
            [self._ev("execute_started", 1_000_000, execution_id=9)]
        )
        # Run goes idle (finish) BEFORE the execute_completed is observed.
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.record_native_kernel_events(
            [self._ev("execute_completed", 4_000_000, execution_id=9)]
        )
        # Next run starts: the finishing run's histogram (now holding the
        # trailing sample) is stashed as last_run.
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))

        payload = rec.latency_histograms()
        assert payload["last_run"]["run_id"] == first_run_id
        assert payload["last_run"]["metrics"]["execute"]["count"] == 1
        assert payload["last_run"]["metrics"]["execute"]["sum_ns"] == 3_000_000
        # The new run starts clean.
        assert payload["current_run"]["run_id"] != first_run_id
        assert payload["current_run"]["metrics"]["execute"]["count"] == 0

    def test_first_run_has_no_bogus_last_run(self):
        # last_run stays None until a real run has been superseded — the first
        # start must not stash an empty predecessor.
        rec = EventRecorder(observe_min_interval_s=0.0)
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        assert rec.latency_histograms()["last_run"] is None

    def test_stop_latency_summary_falls_back_to_last_run(self):
        rec = EventRecorder(observe_min_interval_s=0.0)
        assert rec.stop_latency_summary()["count"] == 0

        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        run_id = rec.current_run_id
        rec.record_native_kernel_events(
            [
                self._ev("stop_requested", 1_000),
                self._ev("stop_issued", 2_000),
                self._ev(
                    "state_transition", 5_000,
                    from_state="Stopping", to_state="Stopped",
                ),
            ]
        )
        summary = rec.stop_latency_summary()
        assert summary["source_run"] == run_id
        assert summary["count"] == 1
        assert summary["p50_ns"] is not None
        assert summary["p99_ns"] is not None

        # After the run ends the summary falls back to the stashed run.
        rec.observe(_StubState(cycle_manager=_StubCycleManager("idle")))
        rec.observe(_StubState(cycle_manager=_StubCycleManager("running")))
        fallback = rec.stop_latency_summary()
        assert fallback["source_run"] == run_id
        assert fallback["count"] == 1

class TestCrossRunHistory:
    """OW7: wall-time-window queries resolved across runs via the run index."""

    T1 = "2026-07-07T01:00:00Z"
    T2 = "2026-07-07T02:00:00Z"
    T3 = "2026-07-07T03:00:00Z"

    def _ns(self, iso):
        return event_recorder_module._wall_iso_to_ns(iso)

    def _recorder(self, tmp_path, monkeypatch):
        wall = {"v": self.T1}
        monkeypatch.setattr(
            event_recorder_module, "_wall_timestamp_iso", lambda: wall["v"]
        )
        rec = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        return rec, wall

    def _seed(self, rec, wall, run_id, start, events, end):
        # events: list of (event_type, wall_iso)
        wall["v"] = start
        rec._handle_cycle_state_transition(None, "running", run_id)
        for etype, ewall in events:
            wall["v"] = ewall
            rec._record_event(event_type=etype, execution_id=1)
        wall["v"] = end
        rec._handle_cycle_state_transition("running", "idle")

    def _flush(self, rec):
        rec._jsonl_queue.join()

    def test_window_selects_only_run_in_range(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._seed(rec, wall, "run-3", self.T3, [("pose_source_applied", self.T3)], self.T3)
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T01:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T02:30:00Z"),
        )
        assert result["run_ids"] == ["run-2"]
        assert result["events"]
        assert all(e["run_id"] == "run-2" for e in result["events"])
        assert rec.close(timeout_s=10.0)

    def test_window_spanning_multiple_runs_in_run_order(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._seed(rec, wall, "run-3", self.T3, [("pose_source_applied", self.T3)], self.T3)
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T01:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T03:30:00Z"),
        )
        # Oldest run first; run-1 (before the window) excluded.
        assert result["run_ids"] == ["run-2", "run-3"]
        # Wall-ordered page: every run-2 event (T2) precedes every run-3 (T3).
        run_sequence = [e["run_id"] for e in result["events"]]
        assert set(run_sequence) == {"run-2", "run-3"}
        assert run_sequence == sorted(run_sequence, key=["run-2", "run-3"].index)
        assert rec.close(timeout_s=10.0)

    def test_per_event_wall_filter_within_a_run(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-x", "2026-07-07T10:00:00Z",
            [
                ("pose_source_applied", "2026-07-07T10:00:00Z"),
                ("pose_source_applied", "2026-07-07T10:05:00Z"),
                ("pose_source_applied", "2026-07-07T10:10:00Z"),
            ],
            "2026-07-07T10:10:00Z",
        )
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T10:03:00Z"),
            to_wall_ns=self._ns("2026-07-07T10:07:00Z"),
        )
        walls = [e["timestamp_wall"] for e in result["events"]]
        assert walls == ["2026-07-07T10:05:00Z"]
        assert result["run_ids"] == ["run-x"]
        assert rec.close(timeout_s=10.0)

    def test_severity_floor_across_runs(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-1", self.T1,
            [("pose_source_applied", self.T1), ("cycle_error", self.T1)], self.T1,
        )
        self._seed(
            rec, wall, "run-2", self.T2,
            [("pose_source_applied", self.T2), ("cycle_error", self.T2)], self.T2,
        )
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T00:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T02:30:00Z"),
            min_severity="WARN",
        )
        assert result["events"]
        assert all(
            event_recorder_module._SEVERITY_ORDER[e["severity"]]
            >= event_recorder_module._SEVERITY_ORDER["WARN"]
            for e in result["events"]
        )
        assert {e["run_id"] for e in result["events"]} == {"run-1", "run-2"}
        assert rec.close(timeout_s=10.0)

    def test_pagination_returns_most_recent_tail(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-1", self.T1,
            [("pose_source_applied", self.T1) for _ in range(6)], self.T1,
        )
        self._seed(
            rec, wall, "run-2", self.T2,
            [("pose_source_applied", self.T2) for _ in range(6)], self.T2,
        )
        self._flush(rec)

        window = dict(
            from_wall_ns=self._ns("2026-07-07T00:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T02:30:00Z"),
        )
        full = rec.history(limit=10000, **window)["events"]
        limited = rec.history(limit=5, **window)["events"]
        assert len(limited) == 5
        assert [e["event_index"] for e in limited] == [
            e["event_index"] for e in full[-5:]
        ]
        assert rec.close(timeout_s=10.0)

    def test_window_before_all_runs_is_empty(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T00:00:00Z"),
            to_wall_ns=self._ns("2026-07-07T00:30:00Z"),
        )
        assert result["events"] == []
        assert result["run_ids"] == []
        assert rec.close(timeout_s=10.0)

    def test_open_ended_lower_bound_only(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._seed(rec, wall, "run-3", self.T3, [("pose_source_applied", self.T3)], self.T3)
        self._flush(rec)

        # Only from_wall: everything at/after T2.
        result = rec.history(from_wall_ns=self._ns("2026-07-07T01:30:00Z"))
        assert result["run_ids"] == ["run-2", "run-3"]
        assert rec.close(timeout_s=10.0)

    def test_trailing_event_after_finish_is_not_pruned(self, tmp_path, monkeypatch):
        # A trailing event recorded during the idle gap is attributed to the
        # just-finished run and written to its JSONL with a wall AFTER the
        # recorded ended_wall. A window past ended_wall must still find it:
        # pruning on ended_wall would silently drop in-window evidence.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        wall["v"] = "2026-07-07T01:00:00Z"
        rec._handle_cycle_state_transition(None, "running", "run-1")
        rec._record_event(event_type="pose_source_applied", execution_id=1)
        wall["v"] = "2026-07-07T01:00:10Z"
        rec._handle_cycle_state_transition("running", "idle")
        # trailing event in the idle gap, attributed to run-1, wall > ended_wall
        wall["v"] = "2026-07-07T01:00:30Z"
        rec._record_event(
            event_type="pose_source_applied",
            execution_id=1,
            details={"marker": "trailing"},
        )
        # a later run gives run-1 a finite territory ending at run-2's start.
        wall["v"] = "2026-07-07T02:00:00Z"
        rec._handle_cycle_state_transition(None, "running", "run-2")
        rec._record_event(event_type="pose_source_applied", execution_id=2)
        wall["v"] = "2026-07-07T02:00:10Z"
        rec._handle_cycle_state_transition("running", "idle")
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T01:00:20Z"),
            to_wall_ns=self._ns("2026-07-07T01:00:40Z"),
        )
        assert result["run_ids"] == ["run-1"]
        assert any(
            e.get("details", {}).get("marker") == "trailing"
            for e in result["events"]
        )
        assert rec.close(timeout_s=10.0)

    def test_cross_run_query_tolerates_torn_run_file(self, tmp_path, monkeypatch):
        # A crashed run leaves a torn final line — exactly what forensic
        # cross-run queries target. The torn line is skipped, the valid events
        # survive, and the query does not raise.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        wall["v"] = self.T2
        rec._handle_cycle_state_transition(None, "running", "run-1")
        rec._record_event(
            event_type="pose_source_applied",
            execution_id=1,
            details={"marker": "survivor"},
        )
        rec._handle_cycle_state_transition("running", "idle")
        self._flush(rec)
        assert rec.close(timeout_s=10.0)
        run_file = tmp_path / "run-1.events.jsonl"
        clean_count = len(
            [ln for ln in run_file.read_text().splitlines() if ln.strip()]
        )
        with open(run_file, "a", encoding="utf-8") as fh:
            fh.write('{"type": "pose_source_applied", "timestamp')  # torn

        rec2 = EventRecorder(trace_dir=tmp_path, observe_min_interval_s=0.0)
        result = rec2.history(
            from_wall_ns=self._ns("2026-07-07T01:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T02:30:00Z"),
        )
        # The query does not raise; the torn line is skipped and every valid
        # event (all in-window at T2) survives.
        assert len(result["events"]) == clean_count
        assert any(
            e.get("details", {}).get("marker") == "survivor"
            for e in result["events"]
        )
        assert result["run_ids"] == ["run-1"]
        assert rec2.close(timeout_s=10.0)

    def test_page_is_wall_ordered_under_backward_clock_step(self, tmp_path, monkeypatch):
        # run-A is index-older but WALL-newer than run-B (a backward wall-clock
        # step between runs). A limit-1 page must return the wall-most-recent
        # event (run-A), which the removed newest-index-first early-stop would
        # have dropped in favor of the wall-older run-B.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-A", "2026-07-07T10:00:00Z",
            [("pose_source_applied", "2026-07-07T10:00:00Z")],
            "2026-07-07T10:00:05Z",
        )
        self._seed(
            rec, wall, "run-B", "2026-07-07T09:00:00Z",
            [("pose_source_applied", "2026-07-07T09:00:00Z")],
            "2026-07-07T09:00:05Z",
        )
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T08:00:00Z"),
            to_wall_ns=self._ns("2026-07-07T11:00:00Z"),
            limit=1,
        )
        assert len(result["events"]) == 1
        assert result["events"][0]["run_id"] == "run-A"
        assert rec.close(timeout_s=10.0)

    def test_backward_clock_step_does_not_prune_older_index_run(self, tmp_path, monkeypatch):
        # The next-newer run's started_wall is not a safe upper bound when the
        # wall clock steps backward. A window after the newer run's start still
        # must find the index-older, wall-newer run.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-A", "2026-07-07T10:00:00Z",
            [("pose_source_applied", "2026-07-07T10:00:00Z")],
            "2026-07-07T10:00:05Z",
        )
        self._seed(
            rec, wall, "run-B", "2026-07-07T09:00:00Z",
            [("pose_source_applied", "2026-07-07T09:00:00Z")],
            "2026-07-07T09:00:05Z",
        )
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T09:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T10:30:00Z"),
        )
        assert result["run_ids"] == ["run-A"]
        assert all(e["run_id"] == "run-A" for e in result["events"])
        assert rec.close(timeout_s=10.0)

    def test_late_explicit_run_event_after_newer_run_start_is_not_pruned(self, tmp_path, monkeypatch):
        # Kernel/native evidence carries its original run id. If it drains after
        # another run starts, the next-run start must not prune the older run.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(
            rec, wall, "run-1", "2026-07-07T01:00:00Z",
            [("pose_source_applied", "2026-07-07T01:00:00Z")],
            "2026-07-07T01:00:05Z",
        )
        wall["v"] = "2026-07-07T02:00:00Z"
        rec._handle_cycle_state_transition(None, "running", "run-2")
        rec._record_event(event_type="pose_source_applied", execution_id=2)
        wall["v"] = "2026-07-07T02:00:30Z"
        rec._record_event(
            event_type="pose_source_applied",
            execution_id=1,
            run_id="run-1",
            details={"marker": "late-native"},
        )
        wall["v"] = "2026-07-07T02:01:00Z"
        rec._handle_cycle_state_transition("running", "idle")
        self._flush(rec)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T02:00:20Z"),
            to_wall_ns=self._ns("2026-07-07T02:00:40Z"),
        )
        assert result["run_ids"] == ["run-1"]
        assert [e.get("details", {}).get("marker") for e in result["events"]] == [
            "late-native"
        ]
        assert rec.close(timeout_s=10.0)

    def test_unparseable_wall_event_is_not_evicted_first(self, tmp_path, monkeypatch):
        # The run index placed the run in the wall window; a malformed event
        # wall stamp is accepted evidence and should survive a tight page limit,
        # not sort as wall=0 and get evicted first.
        rec, wall = self._recorder(tmp_path, monkeypatch)
        wall["v"] = "2026-07-07T01:00:00Z"
        rec._handle_cycle_state_transition(None, "running", "run-1")
        rec._record_event(event_type="pose_source_applied", execution_id=1)
        wall["v"] = "2026-07-07T01:00:10Z"
        rec._record_event(
            event_type="pose_source_applied",
            execution_id=1,
            details={"marker": "unparseable-wall"},
        )
        rec._handle_cycle_state_transition("running", "idle")
        self._flush(rec)

        run_file = tmp_path / "run-1.events.jsonl"
        rewritten: list[str] = []
        for line in run_file.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event.get("details", {}).get("marker") == "unparseable-wall":
                event["timestamp_wall"] = "not-a-wall"
                event["timestamp"] = "not-a-wall"
            rewritten.append(json.dumps(event))
        run_file.write_text("\n".join(rewritten) + "\n", encoding="utf-8")

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T00:59:00Z"),
            to_wall_ns=self._ns("2026-07-07T01:01:00Z"),
            limit=1,
        )
        assert result["events"][0].get("details", {}).get("marker") == "unparseable-wall"
        assert rec.close(timeout_s=10.0)

    def test_cross_run_scan_cap_is_visible(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._seed(rec, wall, "run-3", self.T3, [("pose_source_applied", self.T3)], self.T3)
        self._flush(rec)
        monkeypatch.setattr(event_recorder_module, "_MAX_CROSS_RUN_RUNS", 2)

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T00:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T03:30:00Z"),
        )
        assert result["runs_truncated"] is True
        assert result["cross_run_scan_limit"] == 2
        assert result["scanned_run_count"] == 2
        assert result["run_ids"] == ["run-2", "run-3"]
        assert rec.close(timeout_s=10.0)

    def test_wall_query_with_run_id_streams_selected_run_directly(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._seed(rec, wall, "run-2", self.T2, [("pose_source_applied", self.T2)], self.T2)
        self._flush(rec)

        result = rec.history(
            run_id="run-1",
            from_wall_ns=self._ns("2026-07-07T00:30:00Z"),
            to_wall_ns=self._ns("2026-07-07T02:30:00Z"),
        )
        assert result["run_ids"] == ["run-1"]
        assert result["cross_run_scan_limit"] == 1
        assert result["scanned_run_count"] == 1
        assert all(e["run_id"] == "run-1" for e in result["events"])
        assert rec.close(timeout_s=10.0)

    def test_current_run_wall_query_merges_unflushed_memory_tail(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        wall["v"] = "2026-07-07T01:00:00Z"
        rec._handle_cycle_state_transition(None, "running", "run-live")
        rec._record_event(event_type="pose_source_applied", execution_id=1)
        self._flush(rec)

        def _drop_jsonl_enqueue(**_kwargs):
            return None

        monkeypatch.setattr(rec, "_enqueue_jsonl_locked", _drop_jsonl_enqueue)
        wall["v"] = "2026-07-07T01:00:30Z"
        rec._record_event(
            event_type="pose_source_applied",
            execution_id=1,
            details={"marker": "memory-tail"},
        )

        result = rec.history(
            from_wall_ns=self._ns("2026-07-07T00:59:00Z"),
            to_wall_ns=self._ns("2026-07-07T01:01:00Z"),
        )
        assert any(
            e.get("details", {}).get("marker") == "memory-tail"
            for e in result["events"]
        )
        assert rec.close(timeout_s=10.0)

    def test_single_run_query_has_empty_run_ids(self, tmp_path, monkeypatch):
        rec, wall = self._recorder(tmp_path, monkeypatch)
        self._seed(rec, wall, "run-1", self.T1, [("pose_source_applied", self.T1)], self.T1)
        self._flush(rec)
        # No wall window -> single-run path -> run_ids stays empty.
        result = rec.history()
        assert result["run_ids"] == []
        assert result["run_id"] == "run-1"
        assert rec.close(timeout_s=10.0)
