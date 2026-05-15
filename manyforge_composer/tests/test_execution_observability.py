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
from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
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


def _interrupt_program() -> dict:
    return {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "interrupt_program",
        "catalog_version": "0.1.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        "interrupt_handlers": {
            "pause_announce": {
                "root": {
                    "id": "sequence",
                    "name": "pause_announce",
                    "children": [{"id": "reset_trajectories", "name": "noop"}],
                },
            },
        },
        "interrupt_policies": [
            {
                "id": "operator_pause_hold",
                "source": "operator_pause",
                "applies_to": {"node_names": ["wait_here"], "interruptible_only": True},
                "preemption_mode": "hold_cycle",
                "handler": "pause_announce",
                "after_handler": "resumable_hold",
            }
        ],
        "tree": {
            "id": "sequence",
            "name": "root",
            "children": [
                {"id": "timer", "name": "wait_here", "params": {"duration_s": 5.0}},
            ],
        },
    }


def _interrupt_program_with_timeout() -> dict:
    data = _interrupt_program()
    data["interrupt_handlers"]["pause_announce"]["timeout_s"] = 0.05
    data["interrupt_handlers"]["pause_announce"]["root"] = {
        "id": "timer",
        "name": "pause_delay",
        "params": {"duration_s": 5.0},
    }
    return data


def _interrupt_deployment(
    *,
    adapter: str = "composer_ui",
    signal: str = "pause_request",
    maskable: bool = True,
) -> dict:
    return {
        "api_version": "manyforge.deployment.v0",
        "schema_version": "0.1.0",
        "name": "interrupt_deployment",
        "program_profile": "v0.0",
        "robot_config": {
            "robot_id": "ur5",
            "group_id": "arm",
            "urdf_path": "package://test.urdf",
            "global_max_tcp_linear_speed_mps": 0.5,
        },
        "adapters": {
            "planning": {
                "adapter_id": "planning.test",
                "adapter_version": "0.1.0",
                "supported_profiles": ["v0.0"],
                "capability_keys": sorted(V0_0_REQUIRED_KEYS),
                "config": {},
            },
            "context": {
                "adapter_id": "context.test",
                "adapter_version": "0.1.0",
                "supported_profiles": ["v0.0"],
                "capability_keys": [],
                "config": {},
            },
            "execution": {
                "adapter_id": "execution.test",
                "adapter_version": "0.1.0",
                "supported_profiles": ["v0.0"],
                "capability_keys": [],
                "config": {},
            },
        },
        "orchestrator": {"engine": "py_trees", "config": {}},
        "launch": {"runner": {"module": "manyforge_behavior.runner", "args": []}},
        "interrupt_sources": {
            "operator_pause": {
                "adapter": adapter,
                "signal": signal,
                "kind": "edge",
                "priority": 30,
                "maskable": maskable,
            }
        },
    }


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
        assert archive["path"].endswith("failure-archives")


def test_execution_digest_and_history_include_pending_interrupt():
    kernel = MockKernelClient()
    program_path = _write_yaml_file(_interrupt_program(), suffix=".program.yaml")
    deployment_path = _write_yaml_file(_interrupt_deployment(), suffix=".deployment.yaml")
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[os.path.dirname(program_path)],
    )
    app = create_app(
        state,
        security=SecurityConfig(allowed_fs_roots=[os.path.dirname(program_path)]),
    )

    try:
        with TestClient(app) as client:
            response = client.post(
                "/api/program/load",
                json={"path": program_path, "deploymentPath": deployment_path},
            )
            assert response.status_code == 200
            assert client.post("/api/cycle/run").status_code == 200

            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.0
            digest = None
            while time.time() < deadline:
                digest_response = client.get("/api/execution/digest")
                assert digest_response.status_code == 200
                digest = digest_response.json()
                if digest.get("pendingInterrupt") is not None:
                    break
                time.sleep(0.01)

            assert digest is not None
            assert digest["state"] == "stopped"
            assert digest["pendingInterrupt"] is not None
            assert digest["pendingInterrupt"]["sourceId"] == "operator_pause"
            assert digest["pendingInterrupt"]["preemptionMode"] == "hold_cycle"
            assert digest["pendingInterrupt"]["requiredAction"] == "resume"
            assert digest["pendingInterrupt"]["resumeAllowed"] is True
            assert digest["pendingInterrupt"]["resetRequired"] is False
            assert digest["requiredOperatorAction"] == "resume"
            assert digest["resumeAllowed"] is True
            assert digest["resetRequired"] is False
            assert (
                digest["pendingInterrupt"]["timing"]["quiesceLatencyNs"] is not None
            )
            assert (
                digest["pendingInterrupt"]["timing"]["handlerCompletionLatencyNs"]
                is not None
            )
            assert any("Interrupt latched" in warning for warning in digest["warnings"])
            assert any("Operator action required: resume" in warning for warning in digest["warnings"])

            history_response = client.get("/api/execution/history")
            assert history_response.status_code == 200
            history = history_response.json()
            interrupt_events = [event for event in history["events"] if event["type"] == "interrupt_latched"]
            assert interrupt_events
            assert interrupt_events[-1]["details"]["source"] == "operator_pause"
            handler_started_events = [
                event for event in history["events"] if event["type"] == "interrupt_handler_started"
            ]
            assert handler_started_events
            handler_completed_events = [
                event for event in history["events"] if event["type"] == "interrupt_handler_completed"
            ]
            assert handler_completed_events

            filtered_history_response = client.get(
                "/api/execution/history",
                params={
                    "minSeverity": "INFO",
                    "components": "manyforge.interrupt",
                    "eventTypes": "interrupt_latched",
                },
            )
            assert filtered_history_response.status_code == 200
            filtered_history = filtered_history_response.json()
            assert filtered_history["events"]
            assert all(event["type"] == "interrupt_latched" for event in filtered_history["events"])
            assert all(event["component"] == "manyforge.interrupt" for event in filtered_history["events"])

            medkit_response = client.get("/api/debug/export/ros2-medkit")
            assert medkit_response.status_code == 200
            medkit = medkit_response.json()
            interrupt_records = [
                record for record in medkit["records"]
                if record["eventType"] == "interrupt_latched"
            ]
            assert interrupt_records
            assert interrupt_records[-1]["attributes"]["source"] == "operator_pause"
            handler_records = [
                record for record in medkit["records"]
                if record["eventType"] == "interrupt_handler_completed"
            ]
            assert handler_records
            assert handler_records[-1]["attributes"]["disposition"] == "resumable_hold"

            py_trees.blackboard.Blackboard.set("pause_request", False)
            assert client.post("/api/cycle/reset").status_code == 200
    finally:
        os.unlink(program_path)
        os.unlink(deployment_path)


def test_debug_signal_publish_route_can_trigger_interrupt():
    kernel = MockKernelClient()
    program_path = _write_yaml_file(_interrupt_program(), suffix=".program.yaml")
    deployment_path = _write_yaml_file(
        _interrupt_deployment(adapter="demo_ui", signal="pause_request"),
        suffix=".deployment.yaml",
    )
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[os.path.dirname(program_path)],
    )
    app = create_app(
        state,
        security=SecurityConfig(allowed_fs_roots=[os.path.dirname(program_path)]),
    )

    try:
        with TestClient(app) as client:
            response = client.post(
                "/api/program/load",
                json={"path": program_path, "deploymentPath": deployment_path},
            )
            assert response.status_code == 200
            assert client.post("/api/cycle/run").status_code == 200

            publish_response = client.post(
                "/api/debug/signals/publish",
                json={
                    "adapter": "demo_ui",
                    "signal": "pause_request",
                    "value": True,
                    "staleAfterMs": 1500,
                    "mirrorKey": "demo/pause_request",
                    "metadata": {"reason": "operator_pause_demo"},
                },
            )
            assert publish_response.status_code == 200
            snapshot = publish_response.json()
            assert "demo_ui:pause_request" in snapshot["signals"]
            assert snapshot["signals"]["demo_ui:pause_request"]["freshness"] == "fresh"

            deadline = time.time() + 1.0
            digest = None
            while time.time() < deadline:
                digest_response = client.get("/api/execution/digest")
                assert digest_response.status_code == 200
                digest = digest_response.json()
                if digest.get("pendingInterrupt") is not None:
                    break
                time.sleep(0.01)

            assert digest is not None
            assert digest["state"] == "stopped"
            assert digest["pendingInterrupt"] is not None
            assert digest["pendingInterrupt"]["sourceAdapter"] == "demo_ui"
            assert digest["pendingInterrupt"]["signal"] == "pause_request"
    finally:
        os.unlink(program_path)
        os.unlink(deployment_path)


def test_execution_history_reports_interrupt_handler_timeout_failure():
    kernel = MockKernelClient()
    program_path = _write_yaml_file(_interrupt_program_with_timeout(), suffix=".program.yaml")
    deployment_path = _write_yaml_file(_interrupt_deployment(), suffix=".deployment.yaml")
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[os.path.dirname(program_path)],
    )
    app = create_app(
        state,
        security=SecurityConfig(allowed_fs_roots=[os.path.dirname(program_path)]),
    )

    try:
        with TestClient(app) as client:
            response = client.post(
                "/api/program/load",
                json={"path": program_path, "deploymentPath": deployment_path},
            )
            assert response.status_code == 200
            assert client.post("/api/cycle/run").status_code == 200

            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.5
            digest = None
            while time.time() < deadline:
                digest_response = client.get("/api/execution/digest")
                assert digest_response.status_code == 200
                digest = digest_response.json()
                pending = digest.get("pendingInterrupt")
                if pending is not None and pending["details"].get("handler_state") == "failed":
                    break
                time.sleep(0.01)

            assert digest is not None
            assert digest["state"] == "stopped"
            pending = digest["pendingInterrupt"]
            assert pending is not None
            assert pending["details"]["handler_state"] == "failed"
            assert pending["details"]["handler_failure_reason"] == "timeout"
            assert pending["details"]["handler_timeout_s"] == 0.05
            assert pending["requiredAction"] == "reset"
            assert pending["resumeAllowed"] is False
            assert pending["resetRequired"] is True
            assert digest["requiredOperatorAction"] == "reset"
            assert digest["resetRequired"] is True
            assert pending["timing"]["handlerDurationNs"] is not None
            assert pending["timing"]["handlerCompletionLatencyNs"] is not None

            history_response = client.get("/api/execution/history")
            assert history_response.status_code == 200
            history = history_response.json()
            failed_events = [
                event for event in history["events"] if event["type"] == "interrupt_handler_failed"
            ]
            assert failed_events
            assert failed_events[-1]["details"]["disposition"] == "failed"
            assert failed_events[-1]["details"]["handler_failure_reason"] == "timeout"
            assert failed_events[-1]["details"]["handler_timeout_s"] == 0.05
            assert failed_events[-1]["details"]["handler_duration_ns"] is not None
    finally:
        os.unlink(program_path)
        os.unlink(deployment_path)


def test_unhandled_non_maskable_interrupt_requires_reset_in_digest():
    kernel = MockKernelClient()
    program = _interrupt_program()
    program.pop("interrupt_handlers", None)
    program.pop("interrupt_policies", None)
    program_path = _write_yaml_file(program, suffix=".program.yaml")
    deployment_path = _write_yaml_file(
        _interrupt_deployment(maskable=False),
        suffix=".deployment.yaml",
    )
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[os.path.dirname(program_path)],
    )
    app = create_app(
        state,
        security=SecurityConfig(allowed_fs_roots=[os.path.dirname(program_path)]),
    )

    try:
        with TestClient(app) as client:
            response = client.post(
                "/api/program/load",
                json={"path": program_path, "deploymentPath": deployment_path},
            )
            assert response.status_code == 200
            assert client.post("/api/cycle/run").status_code == 200

            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.0
            digest = None
            while time.time() < deadline:
                digest_response = client.get("/api/execution/digest")
                assert digest_response.status_code == 200
                digest = digest_response.json()
                pending = digest.get("pendingInterrupt")
                if pending is not None and pending["preemptionMode"] == "latched_fault":
                    break
                time.sleep(0.01)

            assert digest is not None
            pending = digest["pendingInterrupt"]
            assert pending is not None
            assert pending["sourceId"] == "operator_pause"
            assert pending["preemptionMode"] == "latched_fault"
            assert pending["afterHandler"] == "halted"
            assert pending["requiredAction"] == "reset"
            assert pending["resumeAllowed"] is False
            assert pending["resetRequired"] is True
            assert pending["ackRequired"] is False
            assert digest["requiredOperatorAction"] == "reset"
            assert digest["resumeAllowed"] is False
            assert digest["resetRequired"] is True
            assert digest["ackRequired"] is False
    finally:
        os.unlink(program_path)
        os.unlink(deployment_path)


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
