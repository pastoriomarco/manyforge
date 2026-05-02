"""Tests for kernel/scene observability routes."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock
import sys

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend import routes_scene
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_DEPLOYMENT_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")


def make_client(*, joint_names: list[str] | None = None) -> tuple[TestClient, ComposerState, MockKernelClient]:
    kernel = MockKernelClient(joint_names=joint_names)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)
    return TestClient(app), state, kernel


def test_scene_state_no_program_returns_empty_world():
    client, state, kernel = make_client()

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    data = response.json()

    assert data["robot"] is not None
    assert data["robot"]["base_frame"] == "base_link"
    assert data["robot"]["tcp_frame"] == "tool0"
    assert data["objects"] == []
    assert data["timestamp_ns"] > 0


def test_scene_state_uses_backend_authoritative_objects_for_runtime_additions():
    client, state, kernel = make_client()
    result = kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="runtime_box",
            shape_type="box",
            box_dimensions_m=[0.2, 0.1, 0.3],
            pose_position_m=[0.5, -0.2, 0.4],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )
    assert result.success is True

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    data = response.json()

    assert [obj["object_id"] for obj in data["objects"]] == ["runtime_box"]
    obj = data["objects"][0]
    assert obj["shape"] == {
        "type": "box",
        "box_dimensions_m": [0.2, 0.1, 0.3],
        "sphere_radius_m": None,
        "cylinder_radius_m": None,
        "cylinder_height_m": None,
        "mesh_resource_uri": None,
        "mesh_scale": None,
    }
    assert obj["pose_in_universe"]["position_m"] == [0.5, -0.2, 0.4]
    assert obj["attached"] is False
    assert obj["attached_to"] is None
    assert obj["source"] == "runtime"
    assert obj["persistable"] is False


def test_scene_state_includes_robot_metadata_after_program_load():
    client, state, kernel = make_client(joint_names=["joint_a", "joint_b", "joint_c"])
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    data = response.json()

    assert data["robot"]["robot_id"] == "ur5"
    assert data["robot"]["group_id"] == "arm"
    assert data["robot"]["base_frame"] == "base_link"
    assert data["robot"]["tcp_frame"] == "tool0"
    assert data["robot"]["joint_names"] == ["joint_a", "joint_b", "joint_c"]
    assert isinstance(data["robot"]["joint_positions"], dict)


def test_scene_frames_expose_model_inventory_and_visibility_control():
    client, state, kernel = make_client()
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)

    class DummyHmi:
        def __init__(self):
            self.visible: dict[str, bool] = {}

        def get_frame_visibility(self, frame_name: str) -> bool:
            return self.visible.get(frame_name, frame_name == "tool0")

        def set_frame_visibility(self, frame_name: str, visible: bool) -> None:
            self.visible[frame_name] = visible

    state.hmi_server = DummyHmi()

    response = client.get("/api/scene/frames")
    assert response.status_code == 200
    data = response.json()

    assert data["hmiAvailable"] is True
    frame_names = {frame["name"] for frame in data["frames"]}
    assert "universe" in frame_names
    assert "base_link" in frame_names
    assert "tool0" in frame_names
    tool0 = next(frame for frame in data["frames"] if frame["name"] == "tool0")
    assert "program_tcp" in tool0["roles"]
    assert tool0["viewerVisible"] is True
    assert tool0["poseAvailable"] is True

    toggle = client.post(
        "/api/scene/frames/visibility",
        json={"frameName": "base_link", "visible": True},
    )
    assert toggle.status_code == 200
    assert state.hmi_server.visible["base_link"] is True


def test_scene_frames_do_not_append_default_package_paths_when_deployment_declares_package_paths(monkeypatch):
    client, state, kernel = make_client()
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)
    state.deployment.robot_config.package_paths = ["/declared/share"]

    calls: dict[str, object] = {}

    def fail_default_package_paths() -> list[str]:
        raise AssertionError("default package paths must not be used when deployment declares package_paths")

    def fake_resolve_robot_model(**kwargs):
        calls["extra_package_paths"] = kwargs["extra_package_paths"]
        calls["package_paths"] = kwargs["package_paths"]
        return SimpleNamespace(
            urdf_path="/declared/share/test.urdf",
            package_paths=list(kwargs["package_paths"]),
        )

    class FakeUrdf:
        @staticmethod
        def load(*args, **kwargs):
            calls["filename_handler_present"] = "filename_handler" in kwargs
            return SimpleNamespace(link_map={"base_link": object(), "tool0": object()})

    monkeypatch.setattr(routes_scene, "default_package_paths", fail_default_package_paths)
    monkeypatch.setattr(routes_scene, "resolve_robot_model", fake_resolve_robot_model)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=FakeUrdf))

    response = client.get("/api/scene/frames")

    assert response.status_code == 200
    assert calls["extra_package_paths"] == []
    assert calls["package_paths"] == ["/declared/share"]
    assert calls["filename_handler_present"] is True
    frame_names = {frame["name"] for frame in response.json()["frames"]}
    assert {"base_link", "tool0"}.issubset(frame_names)


def test_scene_edit_rejects_invalid_operation_with_422():
    client, state, kernel = make_client()

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "teleport",
            "object_id": "box1",
        },
    )

    assert response.status_code == 422
    data = response.json()
    assert any("operation" in str(err.get("loc", [])) for err in data["detail"])


def test_scene_edit_rejects_invalid_add_payload_with_422():
    client, state, kernel = make_client()

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "box1",
            "shape_type": "box",
        },
    )

    assert response.status_code == 422


def test_scene_edit_accepts_upsert_operation():
    client, state, kernel = make_client()

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "upsert",
            "object_id": "box1",
            "shape_type": "box",
            "box_dimensions_m": [0.05, 0.05, 0.05],
            "pose_position_m": [0.3, 0.0, 0.15],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )

    assert response.status_code == 200
    assert response.json()["success"] is True


def test_scene_edit_returns_domain_failure_for_missing_object():
    client, state, kernel = make_client()

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "remove",
            "object_id": "missing",
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["success"] is False
    assert data["error"] == "Object not found"


def test_scene_edit_rejected_during_running():
    client, state, kernel = make_client()
    state.cycle_manager = MagicMock()
    state.cycle_manager.state = CycleState.RUNNING

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "remove",
            "object_id": "missing",
        },
    )

    assert response.status_code == 409
    data = response.json()
    assert data["success"] is False
    assert data["cycleState"] == "running"
    assert "RUNNING" in data["error"]


def test_scene_edit_rejected_during_interrupt_handler():
    client, state, kernel = make_client()
    state.cycle_manager = MagicMock()
    state.cycle_manager.state = CycleState.INTERRUPT_HANDLER

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "remove",
            "object_id": "missing",
        },
    )

    assert response.status_code == 409
    data = response.json()
    assert data["success"] is False
    assert data["cycleState"] == "interrupt_handler"
    assert "INTERRUPT_HANDLER" in data["error"]


def test_scene_edit_rejected_during_direct_control():
    client, state, kernel = make_client()
    state.cycle_manager = MagicMock()
    state.cycle_manager.state = CycleState.DIRECT_CONTROL

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "remove",
            "object_id": "missing",
        },
    )

    assert response.status_code == 409
    data = response.json()
    assert data["success"] is False
    assert data["cycleState"] == "direct_control"
    assert "DIRECT_CONTROL" in data["error"]


def test_kernel_snapshot_reports_idle_when_no_program_loaded():
    client, state, kernel = make_client()

    response = client.get("/api/kernel/snapshot")
    assert response.status_code == 200
    data = response.json()

    assert data["state"] == "idle"
    assert data["execution_id"] == 0
    assert data["active_move_id"] is None
    assert data["timestamp_ns"] > 0


def test_kernel_metrics_returns_counter_payload():
    client, state, kernel = make_client()

    response = client.get("/api/kernel/metrics")
    assert response.status_code == 200
    data = response.json()

    expected_keys = {
        "cache_hits",
        "cache_misses",
        "cache_invalidations",
        "plans_succeeded",
        "plans_failed",
        "validations_passed",
        "validations_failed",
        "executions_completed",
        "executions_failed",
        "stops_issued",
        "collision_detections",
        "snapshots_published",
    }
    assert expected_keys.issubset(data.keys())
