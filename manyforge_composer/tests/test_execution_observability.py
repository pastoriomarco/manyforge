"""Tests for execution observability routes and trace export."""

from __future__ import annotations

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
    MockKernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
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
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
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
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
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

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=TrackingFaultKernel())
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

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=PlanningTimeoutKernel())
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
        catalog_path=_CATALOG_PATH,
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


def test_execution_channels_and_debug_export_surface_incident_snapshot(tmp_path):
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
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

        export_response = client.get("/api/debug/export")
        assert export_response.status_code == 200
        exported = export_response.json()
        assert exported["eventChannels"]["fault_safety_stop"]
        assert exported["incidentSnapshots"][-1]["triggerType"] == "error_raised"


def test_infra_heartbeat_reports_kernel_and_archive_status(tmp_path):
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
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
        catalog_path=_CATALOG_PATH,
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
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
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
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
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
