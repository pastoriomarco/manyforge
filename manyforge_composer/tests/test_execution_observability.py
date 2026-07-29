"""Tests for execution observability routes and trace export."""

from __future__ import annotations

from manyforge_composer.tests.node_environment_support import node_environment_for_catalog

import time
import os
import tempfile
from pathlib import Path

import pytest
import py_trees
import yaml

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.capabilities import V0_0_REQUIRED_KEYS
from manyforge_behavior.kernel_client import (
    ExecutionState,
    MockKernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
from manyforge_behavior.cycle_manager import CycleState
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_TEST_SECURITY = SecurityConfig(allowed_fs_roots=[str(_REPO_ROOT), "/tmp", "/workspace"])


def _write_yaml_file(data: dict, *, suffix: str = ".yaml") -> str:
    handle = tempfile.NamedTemporaryFile(mode="w", suffix=suffix, delete=False)
    yaml.safe_dump(data, handle, sort_keys=False)
    handle.close()
    return handle.name

def test_execution_blackboard_snapshot_after_reset():
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        assert response.status_code == 200

        reset_response = client.post("/api/cycle/reset")
        assert reset_response.status_code == 200

        snapshot_response = client.get("/api/execution/blackboard")
        assert snapshot_response.status_code == 200
        data = snapshot_response.json()

        assert "velocity_scale" in data["keys"]
        assert data["keys"]["velocity_scale"]["valueJson"] == 1.0
        assert "stop_requested" in data["keys"]
        assert data["keys"]["stop_requested"]["valueJson"] is False


def test_execution_digest_reports_running_state_and_object_summary():
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert client.post("/api/cycle/run").status_code == 200

        time.sleep(0.15)
        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

        assert data["state"] == "running"
        assert data["activeMoveId"] == "pick_approach"
        assert data["currentNode"] is not None
        assert any(obj["id"] == "table" for obj in data["objectSummary"])


def test_execution_digest_reports_held_motion_stop_as_resume_required():
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert client.post("/api/cycle/run").status_code == 200

        deadline = time.time() + 1.0
        while kernel.execute_count == 0 and time.time() < deadline:
            time.sleep(0.01)

        stop_response = client.post("/api/cycle/stop")
        assert stop_response.status_code == 200
        assert stop_response.json()["motionStopState"] == "held"

        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

        assert data["state"] == "running"
        assert data["motionStopState"] == "held"
        assert data["requiredOperatorAction"] == "resume"
        assert data["resumeAllowed"] is True
        assert "Operator action required: resume" in data["warnings"]


def test_execution_digest_reports_acknowledged_stopped_cycle_as_resume_ready():
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert (
            client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code
            == 200
        )
        assert state.cycle_manager is not None
        state.cycle_manager._state = CycleState.STOPPED  # noqa: SLF001
        state.cycle_manager._motion_stop_state = "held"  # noqa: SLF001
        state.cycle_manager._error = None  # noqa: SLF001
        state.cycle_manager._error_code = None  # noqa: SLF001
        kernel._state = ExecutionState.IDLE  # noqa: SLF001

        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

        assert data["state"] == "stopped"
        assert data["motionStopState"] == "held"
        assert data["recoveryOffer"] is None
        assert data["requiredOperatorAction"] == "resume"
        assert data["resumeAllowed"] is True
        assert data["ackRequired"] is False

        # Gate-only acknowledgement lands the kernel in Idle. A completed
        # execution must not acquire resume authority merely because a stale
        # cycle projection still reports held motion.
        kernel._state = ExecutionState.COMPLETED  # noqa: SLF001
        completed_response = client.get("/api/execution/digest")
        assert completed_response.status_code == 200
        completed_data = completed_response.json()
        assert completed_data["requiredOperatorAction"] == "none"
        assert completed_data["resumeAllowed"] is False


def test_execution_digest_reports_held_intervention_as_explicit_recovery_required():
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert (
            client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code
            == 200
        )
        assert state.cycle_manager is not None
        state.cycle_manager._state = CycleState.RUNNING  # noqa: SLF001
        state.cycle_manager._motion_stop_state = "held"  # noqa: SLF001
        kernel._state = ExecutionState.INTERVENTION_REQUIRED  # noqa: SLF001
        now_ns = time.monotonic_ns()
        kernel.script_recovery_assessment(
            {
                "schema_version": "manyforge.kernel_recovery_assessment.v1",
                "assessment_revision": 7,
                "assessed_at_ns": now_ns,
                "control_command_epoch": kernel.control_command_epoch(),
                "execution_id": kernel.get_status().execution_id,
                "execution_state": "intervention_required",
                "recovery_class": "stop_fault_intervention",
                "status": "action_required",
                "latch_id": "digest-intervention-latch",
                "source_id": "digest-test",
                "policy_id": "digest-test",
                "preemption_result": "succeeded",
                "stop_owner_active": False,
                "stop_owner_generation": 3,
                "backend_stop_confirmed": True,
                "stop_certificate_id": "digest-stop-certificate",
                "observed_state_available": True,
                "observed_state_sequence": 11,
                "observed_state_source_id": "mock",
                "observed_state_authority_profile": "mock_measured",
                "observed_state_measured": True,
                "observed_state_valid_until_ns": now_ns + 5_000_000_000,
                "observed_state_fresh": True,
                "observed_state_at_rest": True,
                "stronger_authority_active": False,
                "queued_latch_present": False,
                "retry_admissible": False,
                "allowed_actions": ["acknowledge_and_continue", "reset_cycle"],
                "reason_code": "intervention_requires_operator_choice",
                "evidence_refs": ["digest-stop-certificate"],
            }
        )
        expected_offer = state.cycle_manager.refresh_recovery_offer()
        assert expected_offer is not None

        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

        assert data["state"] == "running"
        assert data["motionStopState"] == "held"
        assert data["recoveryOffer"] == expected_offer
        assert data["requiredOperatorAction"] == "choose_recovery_action"
        assert data["resumeAllowed"] is False
        assert data["ackRequired"] is True
        assert data["resetRequired"] is False


def test_execution_digest_reports_structured_backend_fault_details():
    class TrackingFaultKernel(MockKernelClient):
        def get_execution_snapshot(self):
            snapshot = super().get_execution_snapshot()
            snapshot.error = (
                "Controller tracking error exceeded tolerance: 0.125 rad"
            )
            snapshot.error_code = "execution_aborted"
            snapshot.error_details = {
                "backend": "ros2_control",
                "reason_code": "tracking_error_exceeded",
                "joint_name": "shoulder_pan_joint",
                "peak_tracking_error_rad": "0.125",
                "tracking_error_tolerance_rad": "0.05",
            }
            return snapshot

    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=TrackingFaultKernel())
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200

        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

        assert data["error"] == (
            "Controller tracking error exceeded tolerance: 0.125 rad"
        )
        assert data["errorCode"] == "tracking_error_exceeded"
        assert data["errorDetails"]["reason_code"] == "tracking_error_exceeded"
        assert data["errorDetails"]["joint_name"] == "shoulder_pan_joint"


def test_execution_digest_reports_latest_incident_headline_cause():
    class PlanningTimeoutKernel(MockKernelClient):
        def get_execution_snapshot(self):
            snapshot = super().get_execution_snapshot()
            snapshot.error = "Motion execution ended in stopped"
            snapshot.error_code = "planning_failed"
            snapshot.error_details = {"reason_code": "planning_timeout"}
            return snapshot

    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=PlanningTimeoutKernel())
    app = create_app(state, security=_TEST_SECURITY)
    state.event_recorder._record_event(
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
    state.event_recorder._record_event(
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

    with TestClient(app) as client:
        response = client.get("/api/execution/digest")
        assert response.status_code == 200
        data = response.json()

    assert data["activeActionSpan"]["actionSpanId"] == "span-drop"
    assert data["headlineCause"]["eventType"] == "plan_failed"
    assert data["headlineCause"]["summary"] == (
        "Planning timed out while planning `move_to_drop_approach` "
        "(`lite6_drop_approach`, execution 6). Cycle stopped and can be resumed."
    )
    assert data["recovery"] == "resume"


def test_execution_history_and_export_use_trace_sink(tmp_path):
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=kernel,
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert client.post("/api/cycle/run").status_code == 200
        time.sleep(0.25)

        history_response = client.get("/api/execution/history")
        assert history_response.status_code == 200
        history = history_response.json()

        assert history["runId"] is not None
        assert history["events"]
        assert all("event_index" in event for event in history["events"])
        assert all("schema_version" in event for event in history["events"])
        assert all("trace_source" in event for event in history["events"])
        assert any(event["type"] in {"execute_started", "kernel_state_transition"} for event in history["events"])
        trace_file = tmp_path / f"{history['runId']}.events.jsonl"
        # JSONL appends are asynchronous since audit 6B; bound the wait
        # instead of asserting instantaneous existence.
        deadline = time.monotonic() + 2.0
        while not trace_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert trace_file.exists()

        export_response = client.get("/api/debug/export")
        assert export_response.status_code == 200
        exported = export_response.json()
        assert exported["runId"] == history["runId"]
        assert "executionHistory" in exported
        assert "executionDigest" in exported
        assert "sceneState" in exported
        assert "blackboard" in exported
        assert "medkitTrace" in exported
        assert exported["medkitTrace"]["schemaVersion"] == "manyforge.ros2_medkit_trace.v1"
        assert exported["medkitTrace"]["runId"] == history["runId"]

        medkit_response = client.get("/api/debug/export/ros2-medkit")
        assert medkit_response.status_code == 200
        medkit = medkit_response.json()
        assert medkit["schemaVersion"] == "manyforge.ros2_medkit_trace.v1"
        assert medkit["runId"] == history["runId"]
        assert medkit["records"]
        assert all("timelineId" in record for record in medkit["records"])


def test_debug_export_preserves_string_valued_kernel_metric():
    class SourceIdentityMetricsKernel(MockKernelClient):
        def get_metrics(self) -> dict[str, int | str]:
            metrics = super().get_metrics()
            metrics["runtime_collision_freshest_observed_source_id"] = (
                "predicted_state"
            )
            return metrics

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=SourceIdentityMetricsKernel(),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.get("/api/debug/export")

    assert response.status_code == 200
    assert response.json()["kernelMetrics"][
        "runtime_collision_freshest_observed_source_id"
    ] == "predicted_state"


def test_debug_export_preserves_structured_runtime_collision_metrics():
    class StructuredMetricsKernel(MockKernelClient):
        def get_metrics(self) -> dict[str, object]:
            metrics: dict[str, object] = dict(super().get_metrics())
            metrics["runtime_collision_lookahead_last_sample_times_ns"] = [
                100_000_000,
                175_000_000,
                250_000_000,
            ]
            metrics["controller_observed_state_sources"] = [
                {
                    "source_id": "joint_states",
                    "authority": "measured",
                    "fresh": True,
                }
            ]
            return metrics

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=StructuredMetricsKernel(),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.get("/api/debug/export")

    assert response.status_code == 200
    metrics = response.json()["kernelMetrics"]
    assert metrics["runtime_collision_lookahead_last_sample_times_ns"] == [
        100_000_000,
        175_000_000,
        250_000_000,
    ]
    assert metrics["controller_observed_state_sources"] == [
        {
            "source_id": "joint_states",
            "authority": "measured",
            "fresh": True,
        }
    ]


def test_execution_channels_and_debug_export_surface_incident_snapshot(tmp_path):
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)
    state.event_recorder._record_event(
        event_type="pose_source_applied",
        details={"object_id": "box", "status": "applied"},
    )
    state.event_recorder._record_event(
        event_type="error_raised",
        details={
            "requested_stop_mode": "spring_back_hold",
            "fallback_reason": "controller_cancel_timeout",
            "reason_code": "controller_cancel_timeout",
        },
    )
    state.event_recorder.record_robot_lifecycle_event(
        "observation_baseline",
        details={"provider_id": "lite6-a:provider:test"},
    )

    with TestClient(app) as client:
        channels_response = client.get("/api/execution/channels")
        assert channels_response.status_code == 200
        channels = channels_response.json()
        assert channels["channels"]["fault_safety_stop"]
        assert channels["channels"]["pose_source"]
        assert channels["incidentSnapshots"]
        snapshot = channels["incidentSnapshots"][-1]
        assert snapshot["triggerType"] == "error_raised"
        assert "allEvents" in snapshot
        assert "eventsByChannel" in snapshot

        lifecycle_response = client.get(
            "/api/execution/channels",
            params={"limit": 1, "channels": "robot_lifecycle"},
        )
        assert lifecycle_response.status_code == 200
        lifecycle_channels = lifecycle_response.json()["channels"]
        assert list(lifecycle_channels) == ["robot_lifecycle"]
        assert lifecycle_channels["robot_lifecycle"][0]["type"] == (
            "robot_lifecycle_observation_baseline"
        )

        export_response = client.get("/api/debug/export")
        assert export_response.status_code == 200
        exported = export_response.json()
        assert exported["eventChannels"]["fault_safety_stop"]
        assert exported["incidentSnapshots"][-1]["triggerType"] == "error_raised"


def test_infra_heartbeat_reports_kernel_and_archive_status(tmp_path):
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/infra/heartbeat")
        assert response.status_code == 200
        heartbeat = response.json()

        assert heartbeat["schema_version"] == "manyforge.infra_heartbeat.v1"
        assert heartbeat["status"] == "ok"
        assert heartbeat["uptime_s"] >= 0.0
        assert heartbeat["components"]["kernel"]["available"] is True
        assert heartbeat["components"]["kernel"]["state"] == "idle"
        archive = heartbeat["components"]["failure_archive"]
        assert archive["enabled"] is True
        assert archive["status"] == "healthy"
        assert archive["path"].endswith("failure-archives")


def test_infra_heartbeat_degrades_when_failure_archives_drop(tmp_path):
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    state.event_recorder._failure_archive_dropped = 1
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/infra/heartbeat")
        assert response.status_code == 200
        heartbeat = response.json()

        assert heartbeat["status"] == "degraded"
        archive = heartbeat["components"]["failure_archive"]
        assert archive["status"] == "degraded"
        assert archive["dropped_archives"] == 1

def test_scene_diagnostics_reports_native_visual_mismatch():
    kernel = MockKernelClient()
    kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="diag_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="other_box",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
                pose_in_universe={
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=False,
                frame_id="universe",
            )
        ]
    )
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/scene/diagnostics")
        assert response.status_code == 200
        data = response.json()

        assert data["summary"]["total"] == 1
        assert data["summary"]["matched"] == 0
        assert data["summary"]["mismatched"] == 1
        assert data["summary"]["viewerAvailable"] is True
        assert data["objects"][0]["object_id"] == "diag_box"
        assert data["objects"][0]["match"] is False
        assert "not rendered in native visual cache" in data["objects"][0]["mismatch_reason"]


def test_execution_digest_and_debug_export_report_signal_provider_health():
    class FailingSignalProvider:
        adapter_id = "roxsie"

        def poll_signal_updates(self, now_ns: int):
            raise RuntimeError("connector offline")

    kernel = MockKernelClient()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert state.cycle_manager is not None
        state.cycle_manager.register_signal_provider(FailingSignalProvider())
        state.cycle_manager._refresh_signal_registry()

        digest_response = client.get("/api/execution/digest")
        assert digest_response.status_code == 200
        digest = digest_response.json()
        assert any("Signal provider 'roxsie' is disconnected" in warning for warning in digest["warnings"])

        export_response = client.get("/api/debug/export")
        assert export_response.status_code == 200
        exported = export_response.json()
        assert exported["signalSnapshot"]["providers"]["roxsie"]["connected"] is False
        assert "connector offline" in exported["signalSnapshot"]["providers"]["roxsie"]["lastError"]


# --- Audit 6B: event-recorder hot path (findings 2.3, 2.11, 2.16) ---


def test_record_event_does_not_block_on_slow_disk(tmp_path):
    """Finding 2.3: a slow JSONL sink must not stall observers; appends run
    on the writer thread while `_record_event` returns promptly."""
    import threading

    from manyforge_composer.backend.event_recorder import EventRecorder

    recorder = EventRecorder(trace_dir=tmp_path, failure_archive_enabled=False)
    release_disk = threading.Event()
    writes = []
    original_write = EventRecorder._write_jsonl_line

    def slow_write(file_path, json_text, *, durable=False):
        release_disk.wait(timeout=5.0)
        writes.append(json_text)
        original_write(file_path, json_text, durable=durable)

    recorder._write_jsonl_line = staticmethod(slow_write)
    try:
        t0 = time.monotonic()
        for index in range(20):
            recorder._record_event(
                event_type="cycle_state_changed",
                details={"index": index},
                run_id="run-slow-disk",
            )
        hot_path_s = time.monotonic() - t0
        # The disk is fully blocked, yet recording 20 events must be quick.
        assert hot_path_s < 0.5, f"hot path stalled on disk: {hot_path_s:.3f}s"
        release_disk.set()
        assert recorder.wait_for_jsonl_idle(timeout_s=5.0)
        assert len(writes) >= 20
    finally:
        release_disk.set()
        recorder.close()


def test_jsonl_order_preserved_and_close_joins_workers(tmp_path):
    """Findings 2.3 + 2.11: per-run JSONL ordering is fixed at enqueue time,
    and close() terminates both workers via sentinel."""
    from manyforge_composer.backend.event_recorder import EventRecorder

    recorder = EventRecorder(trace_dir=tmp_path)
    for index in range(50):
        recorder._record_event(
            event_type="cycle_state_changed",
            details={"index": index},
            run_id="run-order",
        )
    assert recorder.wait_for_jsonl_idle(timeout_s=5.0)
    assert recorder.close(timeout_s=5.0)

    jsonl_files = list(tmp_path.glob("*.events.jsonl"))
    assert jsonl_files
    import json as _json

    indices = []
    for line in jsonl_files[0].read_text(encoding="utf-8").splitlines():
        event = _json.loads(line)
        if event.get("type") == "cycle_state_changed" and "details" in event:
            if "index" in event["details"]:
                indices.append(event["details"]["index"])
    assert indices == sorted(indices), "JSONL event order must be FIFO"
    # Workers are gone after close().
    assert recorder._jsonl_worker is not None
    assert not recorder._jsonl_worker.is_alive()
    if recorder._failure_archive_worker is not None:
        assert not recorder._failure_archive_worker.is_alive()
    # close() is idempotent.
    assert recorder.close(timeout_s=1.0)


def test_observe_is_rate_limited_and_force_bypasses(tmp_path):
    """Finding 2.16: polling observe() calls are budgeted; force=True is
    exact for export/recovery callers."""
    from manyforge_composer.backend.event_recorder import EventRecorder

    class _CountingState:
        def __init__(self):
            self.reads = 0

        @property
        def cycle_manager(self):
            self.reads += 1
            return None

        _kernel = None

    recorder = EventRecorder(trace_dir=tmp_path, failure_archive_enabled=False)
    state = _CountingState()
    try:
        recorder.observe(state)
        assert state.reads == 1
        # Immediately repolled: budgeted away.
        recorder.observe(state)
        assert state.reads == 1
        # Exactness path bypasses the budget.
        recorder.observe(state, force=True)
        assert state.reads == 2
        # After the budget elapses, polling observes scan again.
        recorder._last_observe_monotonic = 0.0
        recorder.observe(state)
        assert state.reads == 3
    finally:
        recorder.close()


def test_latency_histograms_endpoint_and_debug_export(tmp_path):
    """OW6: a real cycle populates the execute-duration histogram, and the
    per-run payload is reachable via its endpoint and the debug export."""
    kernel = MockKernelClient(execute_duration_s=0.2)
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=kernel,
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert client.post("/api/cycle/run").status_code == 200
        # Bound the wait on at least one execute start/complete pair landing.
        deadline = time.monotonic() + 3.0
        execute_count = 0
        while time.monotonic() < deadline:
            payload = client.get("/api/execution/latency-histograms").json()
            execute_count = payload["current_run"]["metrics"]["execute"]["count"]
            if execute_count >= 1:
                break
            time.sleep(0.05)

        payload = client.get("/api/execution/latency-histograms").json()
        assert payload["schema_version"] == "manyforge.latency_histograms.v1"
        metrics = payload["current_run"]["metrics"]
        assert set(metrics) == {"stop", "plan", "validate", "execute"}
        assert metrics["execute"]["count"] >= 1
        assert metrics["execute"]["sum_ns"] > 0
        assert metrics["execute"]["p50_ns"] is not None
        assert "bucket_bounds_ns" in metrics["execute"]
        assert payload["current_run"]["run_id"] is not None

        exported = client.get("/api/debug/export").json()
        assert "latencyHistograms" in exported
        assert (
            exported["latencyHistograms"]["schema_version"]
            == "manyforge.latency_histograms.v1"
        )
        assert (
            exported["latencyHistograms"]["current_run"]["metrics"]["execute"][
                "count"
            ]
            >= 1
        )


def test_heartbeat_reports_stop_latency_component(tmp_path):
    """OW6: the heartbeat carries an informational stop-latency summary."""
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        heartbeat = client.get("/api/infra/heartbeat").json()
        assert "stop_latency" in heartbeat["components"]
        stop_latency = heartbeat["components"]["stop_latency"]
        assert "count" in stop_latency
        # No stops yet: informational summary reports zero, never degrades.
        assert stop_latency["count"] == 0
        assert heartbeat["status"] in {"ok", "degraded"}


def test_heartbeat_reports_realtime_components(tmp_path):
    """RT-6: heartbeat carries runtime-safety and route-executor evidence."""
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        heartbeat = client.get("/api/infra/heartbeat").json()

    runtime_safety = heartbeat["components"]["runtime_safety"]
    assert runtime_safety["status"] == "healthy"
    assert runtime_safety["runtime_collision_ticks_started"] == 0
    assert runtime_safety["validation_deferral_status"] == "clear"

    route_executors = heartbeat["components"]["route_executors"]
    assert route_executors["status"] == "healthy"
    assert set(route_executors["roles"]) == {"control", "lifecycle", "background"}
    assert route_executors["roles"]["control"]["max_admitted"] >= 2


def test_heartbeat_degrades_on_runtime_collision_tick_fault(tmp_path):
    class FaultedRuntimeSafetyKernel(MockKernelClient):
        def get_metrics(self) -> dict[str, int]:
            metrics = super().get_metrics()
            metrics["runtime_collision_ticks_started"] = 3
            metrics["runtime_collision_ticks_completed"] = 2
            metrics["runtime_collision_ticks_faulted"] = 1
            metrics["runtime_collision_tick_max_ns"] = 12_000_000
            return metrics

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=FaultedRuntimeSafetyKernel(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        heartbeat = client.get("/api/infra/heartbeat").json()

    assert heartbeat["status"] == "degraded"
    runtime_safety = heartbeat["components"]["runtime_safety"]
    assert runtime_safety["status"] == "degraded"
    assert runtime_safety["runtime_collision_ticks_faulted"] == 1
    assert runtime_safety["runtime_collision_tick_max_ns"] == 12_000_000


def test_heartbeat_degrades_on_runtime_collision_missed_period(tmp_path):
    class LateRuntimeSafetyKernel(MockKernelClient):
        def get_metrics(self) -> dict[str, int]:
            metrics = super().get_metrics()
            metrics["runtime_collision_tick_start_jitter_max_ns"] = 25_000_000
            metrics["runtime_collision_tick_late_periods"] = 1
            metrics["runtime_collision_tick_missed_periods"] = 2
            metrics["runtime_collision_tick_max_consecutive_late_periods"] = 1
            return metrics

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=LateRuntimeSafetyKernel(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        heartbeat = client.get("/api/infra/heartbeat").json()

    assert heartbeat["status"] == "degraded"
    runtime_safety = heartbeat["components"]["runtime_safety"]
    assert runtime_safety["status"] == "degraded"
    assert runtime_safety["runtime_collision_tick_missed_periods"] == 2
    assert runtime_safety["runtime_collision_tick_start_jitter_max_ns"] == 25_000_000


def test_heartbeat_degrades_when_runtime_safety_metrics_unavailable(tmp_path):
    class MetricsUnavailableKernel(MockKernelClient):
        def get_metrics(self) -> dict[str, int]:
            raise RuntimeError("metrics unavailable")

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MetricsUnavailableKernel(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        heartbeat = client.get("/api/infra/heartbeat").json()

    assert heartbeat["status"] == "degraded"
    runtime_safety = heartbeat["components"]["runtime_safety"]
    assert runtime_safety["status"] == "error"
    assert runtime_safety["error"] == "metrics unavailable"


def test_heartbeat_degrades_when_route_executor_status_unavailable(
    tmp_path,
    monkeypatch,
):
    from manyforge_composer.backend import routes_infra

    def raise_status_error():
        raise RuntimeError("executor status unavailable")

    monkeypatch.setattr(routes_infra, "route_executor_status", raise_status_error)
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        heartbeat = client.get("/api/infra/heartbeat").json()

    assert heartbeat["status"] == "degraded"
    route_executors = heartbeat["components"]["route_executors"]
    assert route_executors["status"] == "error"
    assert route_executors["error"] == "executor status unavailable"


def _seed_runs_at_walls(rec, monkeypatch, runs):
    """Seed (run_id, wall_iso) runs onto a recorder with controlled wall time."""
    from manyforge_composer.backend import event_recorder as er_mod

    wall = {"v": runs[0][1]}
    monkeypatch.setattr(er_mod, "_wall_timestamp_iso", lambda: wall["v"])
    for run_id, wall_iso in runs:
        wall["v"] = wall_iso
        rec._handle_cycle_state_transition(None, "running", run_id)
        rec._record_event(event_type="pose_source_applied", execution_id=1)
        rec._handle_cycle_state_transition("running", "idle")
    rec._jsonl_queue.join()


def test_cross_run_history_endpoint_windows_by_wall(tmp_path, monkeypatch):
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    _seed_runs_at_walls(
        state.event_recorder,
        monkeypatch,
        [
            ("run-1", "2026-07-07T01:00:00Z"),
            ("run-2", "2026-07-07T02:00:00Z"),
            ("run-3", "2026-07-07T03:00:00Z"),
        ],
    )
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        resp = client.get(
            "/api/execution/history",
            params={
                "fromWall": "2026-07-07T01:30:00Z",
                "toWall": "2026-07-07T02:30:00Z",
            },
        )
        assert resp.status_code == 200
        data = resp.json()
        assert data["runIds"] == ["run-2"]
        assert data["runsTruncated"] is False
        assert data["scannedRunCount"] >= 1
        assert data["crossRunScanLimit"] == 2000
        assert data["events"]
        assert all(e["run_id"] == "run-2" for e in data["events"])

        # A wall window spanning runs 2 and 3, oldest first.
        spanning = client.get(
            "/api/execution/history",
            params={"fromWall": "2026-07-07T01:30:00Z"},
        ).json()
        assert spanning["runIds"] == ["run-2", "run-3"]

        selected = client.get(
            "/api/execution/history",
            params={
                "run_id": "run-1",
                "fromWall": "2026-07-07T00:30:00Z",
                "toWall": "2026-07-07T03:30:00Z",
            },
        ).json()
        assert selected["runIds"] == ["run-1"]
        assert selected["crossRunScanLimit"] == 1

        # Malformed wall bound is a client error, not a 500.
        bad = client.get(
            "/api/execution/history", params={"fromWall": "not-a-timestamp"}
        )
        assert bad.status_code == 400

        mixed_clocks = client.get(
            "/api/execution/history",
            params={
                "fromWall": "2026-07-07T01:30:00Z",
                "startNs": 1,
            },
        )
        assert mixed_clocks.status_code == 400


def test_debug_export_includes_run_index(tmp_path, monkeypatch):
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
        trace_dir=str(tmp_path),
    )
    _seed_runs_at_walls(
        state.event_recorder,
        monkeypatch,
        [
            ("run-1", "2026-07-07T01:00:00Z"),
            ("run-2", "2026-07-07T02:00:00Z"),
        ],
    )
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        exported = client.get("/api/debug/export").json()
        assert "runIndex" in exported
        run_ids = {entry["run_id"] for entry in exported["runIndex"]}
        assert {"run-1", "run-2"} <= run_ids
