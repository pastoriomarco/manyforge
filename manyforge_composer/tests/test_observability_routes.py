"""Tests for kernel/scene observability routes."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import MagicMock

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_composer.backend.app import create_app
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
    assert "RUNNING" in data["error"]


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
