"""Tests for execution observability routes and trace export."""

from __future__ import annotations

import time
from pathlib import Path

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")


def test_execution_blackboard_snapshot_after_reset():
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    app = create_app(state)

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
    app = create_app(state)

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


def test_execution_history_and_export_use_trace_sink(tmp_path):
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        trace_dir=str(tmp_path),
    )
    app = create_app(state)

    with TestClient(app) as client:
        assert client.post("/api/program/load", json={"path": _PROGRAM_PATH}).status_code == 200
        assert client.post("/api/cycle/run").status_code == 200
        time.sleep(0.25)

        history_response = client.get("/api/execution/history")
        assert history_response.status_code == 200
        history = history_response.json()

        assert history["runId"] is not None
        assert history["events"]
        assert any(event["type"] in {"execute_started", "kernel_state_transition"} for event in history["events"])
        trace_file = tmp_path / f"{history['runId']}.events.jsonl"
        assert trace_file.exists()

        export_response = client.get("/api/debug/export")
        assert export_response.status_code == 200
        exported = export_response.json()
        assert exported["runId"] == history["runId"]
        assert "executionHistory" in exported
        assert "executionDigest" in exported
        assert "sceneState" in exported
        assert "blackboard" in exported


def test_scene_diagnostics_reports_hmi_mismatch():
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

    class DummyHmi:
        def get_object_render_diagnostics(self, object_id):
            return {
                "object_id": object_id,
                "frame_path": None,
                "render": {
                    "object_id": object_id,
                    "scene_name": None,
                    "render_state": None,
                    "scene_node_present": False,
                },
            }

    state.hmi_server = DummyHmi()
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/scene/diagnostics")
        assert response.status_code == 200
        data = response.json()

        assert data["summary"]["total"] == 1
        assert data["summary"]["matched"] == 0
        assert data["summary"]["mismatched"] == 1
        assert data["summary"]["hmiAvailable"] is True
        assert data["objects"][0]["object_id"] == "diag_box"
        assert data["objects"][0]["match"] is False
        assert "not rendered" in data["objects"][0]["mismatch_reason"]
