from __future__ import annotations

from pathlib import Path
import threading
import time
from types import SimpleNamespace
from typing import Any

from fastapi.testclient import TestClient

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.deployment import Deployment, GripperConfig
from manyforge_behavior.kernel_client import (
    GripperCommandRequest,
    GripperCommandState,
    MockKernelClient,
    SceneEditRequest,
)
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


def _minimal_deployment(*, pose_sources: list[dict] | None = None) -> Deployment:
    data = {
        "api_version": "manyforge.deployment.v0",
        "schema_version": "0.1.0",
        "name": "pose-source-test",
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
                "config": {},
            },
            "context": {
                "adapter_id": "context.test",
                "adapter_version": "0.1.0",
                "config": {},
            },
            "execution": {
                "adapter_id": "execution.test",
                "adapter_version": "0.1.0",
                "config": {},
            },
        },
        "orchestrator": {"engine": "py_trees", "config": {}},
        "launch": {
            "runner": {
                "module": "manyforge_behavior.runner",
                "args": [],
            },
        },
    }
    if pose_sources is not None:
        data["pose_sources"] = pose_sources
    return Deployment.from_dict(data)


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState]:
    kernel = MockKernelClient()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    state.program_source_path = str(tmp_path / "program.yaml")
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    return TestClient(app), state


_POSE_SOURCE_ADMISSION_DEFAULT = object()
_OBSERVED_TARGET_SCENE_ADMISSION = {
    "role": "observed_target",
    "lease_max_age_s": 2.0,
    "target_drift_tolerance_m": 0.02,
    "target_angular_tolerance_rad": 0.174533,
}


def _scene_admission_for_policy(
    policy: str,
    configured: object,
) -> dict[str, Any] | None:
    if configured is _POSE_SOURCE_ADMISSION_DEFAULT:
        return None
    if configured is None:
        return None
    return dict(configured)  # type: ignore[arg-type]


def _pose_sources(
    *,
    enabled: bool = True,
    box_policy: str = "before_planning",
    can_policy: str = "observe_only",
    box_scene_admission: object = _POSE_SOURCE_ADMISSION_DEFAULT,
    can_scene_admission: object = _POSE_SOURCE_ADMISSION_DEFAULT,
    measured_grasp: bool = False,
    measured_robot: bool = False,
    drift_action: str = "interrupt",
    debounce_s: float = 0.001,
) -> list[dict]:
    box_binding: dict[str, Any] = {
        "entity": "/World/box",
        "apply_policy": box_policy,
    }
    resolved_box_admission = _scene_admission_for_policy(
        box_policy, box_scene_admission
    )
    if resolved_box_admission is not None:
        box_binding["scene_admission"] = resolved_box_admission
    can_binding: dict[str, Any] = {
        "entity": "/World/can",
        "apply_policy": can_policy,
    }
    resolved_can_admission = _scene_admission_for_policy(
        can_policy, can_scene_admission
    )
    if resolved_can_admission is not None:
        can_binding["scene_admission"] = resolved_can_admission
    if measured_grasp:
        box_binding.update(
            {
                "measured_state_ghost": {
                    "enabled": True,
                    "robot_tcp": True,
                    "object": True,
                    "opacity": 0.5,
                },
                "measured_attachment_baseline": {
                    "capture": "on_attach_or_first_paired_sample",
                    "require_same_time_robot_tcp": True,
                },
                "attached_drift_monitor": {
                    "enabled": True,
                    "translation_threshold_m": 0.02,
                    "rotation_threshold_rad": 0.2,
                    "debounce_s": debounce_s,
                    "stale_behavior": "fault",
                    "action": drift_action,
                    "signal_id": "attached_object_drift/box",
                    "confirmation_signal_id": (
                        "attached_object_grasp_confirmed/box"
                    ),
                    "sample_time_tolerance_s": 0.05,
                },
            }
        )
    source: dict[str, Any] = {
        "id": "isaac_scene_objects",
        "kind": "simulation_interfaces",
        "enabled": enabled,
        "service_prefix": "auto",
        "max_update_hz": 4.0,
        "stale_after_s": 1.0,
        "frame_id": "universe",
        "objects": {
            "box": box_binding,
            "can": can_binding,
        },
    }
    if measured_robot:
        source["measured_robot"] = {"joint_state_topic": "/joint_states"}
    return [source]


def _add_object(
    client: TestClient,
    object_id: str = "box",
    *,
    position_m: list[float] | None = None,
) -> None:
    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": object_id,
            "shape_type": "box",
            "box_dimensions_m": [0.1, 0.1, 0.1],
            "pose_position_m": position_m or [0.0, 0.0, 0.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 200


def _add_box(client: TestClient) -> None:
    _add_object(client, "box")


def _pose_source_observation(
    object_id: str,
    *,
    observed_frame_id: str = "universe",
    age_s: float | None = 0.05,
    position_xyz: list[float] | None = None,
    monotonic_s: float = 10.0,
) -> dict[str, Any]:
    observed: dict[str, Any] = {
        "objectId": object_id,
        "status": "fresh",
        "update": {
            "poseSourceId": "isaac_scene_objects",
            "objectId": object_id,
            "entity": "/wrong",
            "monotonicS": monotonic_s,
            "applyPolicy": "observe_only",
            "pose": {
                "frameId": observed_frame_id,
                "stampSec": 100.0,
                "positionXYZ": position_xyz or [1.0, 2.0, 3.0],
                "orientationXYZW": [0.0, 0.0, 0.0, 1.0],
            },
        },
    }
    if age_s is not None:
        observed["ageS"] = age_s
    return observed


def _post_fresh_snapshot(
    client: TestClient,
    observations: list[dict[str, Any]],
    *,
    measured_tcp: dict[str, Any] | None = None,
    measured_robot: dict[str, Any] | None = None,
) -> Any:
    payload: dict[str, Any] = {
        "poseSourceId": "isaac_scene_objects",
        "status": "fresh",
        "monotonicS": 10.0,
        "objects": observations,
    }
    if measured_tcp is not None:
        payload["measuredTcp"] = measured_tcp
    if measured_robot is not None:
        payload["measuredRobot"] = measured_robot
    response = client.post(
        "/api/scene/pose-sources/isaac_scene_objects/snapshot",
        json=payload,
    )
    assert response.status_code == 200
    return response


def _post_fresh_box_snapshot(
    client: TestClient,
    *,
    observed_frame_id: str = "universe",
    age_s: float | None = 0.05,
) -> Any:
    return _post_fresh_snapshot(
        client,
        [
            _pose_source_observation(
                "box",
                observed_frame_id=observed_frame_id,
                age_s=age_s,
            )
        ],
    )


def _wait_for_pose_source_apply(state: ComposerState) -> dict[str, Any]:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        with state._pose_source_apply_lock:
            running = state._pose_source_apply_running
            pending = state._pose_source_apply_pending
            result = state._pose_source_apply_last_result
        if not running and not pending and result is not None:
            return result
        time.sleep(0.005)
    raise AssertionError("timed out waiting for pose-source apply worker")


def test_scene_state_exposes_configured_pose_sources_as_stale(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    sources = response.json()["pose_sources"]

    assert sources[0]["pose_source_id"] == "isaac_scene_objects"
    assert sources[0]["status"] == "stale"
    assert [obj["object_id"] for obj in sources[0]["objects"]] == ["box", "can"]
    assert sources[0]["objects"][0]["apply_policy"] == "before_planning"
    assert sources[0]["objects"][0]["status"] == "stale"


def test_full_deployment_exposes_pose_sources_for_gui_editing(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="between_nodes",
            box_scene_admission={
                "role": "observed_target",
                "lease_max_age_s": 2.0,
                "target_drift_tolerance_m": 0.02,
                "target_angular_tolerance_rad": 0.1,
            },
        )
    )

    response = client.get("/api/deployment/full")

    assert response.status_code == 200
    pose_sources = response.json()["poseSources"]
    assert pose_sources[0]["id"] == "isaac_scene_objects"
    assert pose_sources[0]["maxUpdateHz"] == 4.0
    assert pose_sources[0]["staleAfterS"] == 1.0
    assert pose_sources[0]["objects"]["box"]["applyPolicy"] == "between_nodes"
    assert pose_sources[0]["objects"]["box"]["entity"] == "/World/box"
    assert pose_sources[0]["objects"]["box"]["sceneAdmission"] == {
        "role": "observed_target",
        "leaseMaxAgeS": 2.0,
        "targetDriftToleranceM": 0.02,
        "targetAngularToleranceRad": 0.1,
        "obstacleDriftToleranceM": None,
        "obstacleAngularToleranceRad": None,
        "allowRuntimeDynamicObstacle": False,
    }


def test_pose_source_snapshot_is_observe_only_and_normalizes_policy(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())

    _add_box(client)
    _post_fresh_box_snapshot(client)

    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [0.0, 0.0, 0.0]

    pose_source = scene["pose_sources"][0]
    observed = pose_source["objects"][0]
    assert pose_source["status"] == "fresh"
    assert observed["status"] == "fresh"
    assert observed["entity"] == "/World/box"
    assert observed["apply_policy"] == "before_planning"
    assert observed["scene_admission"] == {
        "role": "snapshot_obstacle",
        "leaseMaxAgeS": None,
        "targetDriftToleranceM": None,
        "targetAngularToleranceRad": None,
        "obstacleDriftToleranceM": None,
        "obstacleAngularToleranceRad": None,
        "allowRuntimeDynamicObstacle": False,
    }
    assert observed["update"]["entity"] == "/World/box"
    assert observed["update"]["apply_policy"] == "before_planning"
    assert observed["update"].get("scene_admission") is None
    assert observed["update"]["pose"]["position_xyz"] == [1.0, 2.0, 3.0]


def test_pose_source_snapshot_exposes_measured_robot_state(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(measured_robot=True)
    )

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box")],
        measured_robot={
            "jointStateTopic": "/joint_states",
            "jointNames": ["shoulder_pan_joint", "elbow_joint"],
            "jointPositionsRad": [0.25, -0.5],
            "monotonicS": 10.0,
            "ageS": 0.0,
            "stampSec": 17.25,
        },
    )

    scene = client.get("/api/scene/state").json()
    source = scene["pose_sources"][0]
    assert source["measured_robot"]["joint_state_topic"] == "/joint_states"
    assert source["measured_robot"]["joint_names"] == [
        "shoulder_pan_joint",
        "elbow_joint",
    ]
    assert source["measured_robot"]["joint_positions_rad"] == [0.25, -0.5]
    assert state.kernel.scene_edits == []


def test_pose_source_snapshot_rejects_unknown_objects(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())

    response = client.post(
        "/api/scene/pose-sources/isaac_scene_objects/snapshot",
        json={
            "poseSourceId": "isaac_scene_objects",
            "status": "fresh",
            "monotonicS": 10.0,
            "objects": [{"objectId": "unknown", "status": "fresh"}],
        },
    )

    assert response.status_code == 400
    assert "unknown object" in response.json()["detail"]


def test_pose_source_snapshot_rejects_disabled_sources(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources(enabled=False))

    response = client.post(
        "/api/scene/pose-sources/isaac_scene_objects/snapshot",
        json={
            "poseSourceId": "isaac_scene_objects",
            "status": "fresh",
            "monotonicS": 10.0,
            "objects": [],
        },
    )

    assert response.status_code == 409


def test_when_idle_apply_updates_planning_scene_from_fresh_snapshot(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="when_idle")
    )
    _add_box(client)
    _post_fresh_box_snapshot(client)

    response = client.post(
        "/api/scene/pose-sources/apply",
        json={"policy": "when_idle", "objectIds": ["box"]},
    )

    assert response.status_code == 200
    assert response.json()["success"] is True
    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [1.0, 2.0, 3.0]
    edit = state.kernel.scene_edits[-1]
    assert edit.operation == "update_pose"
    assert edit.source_freshness_budget_ns is None
    assert edit.source_timestamp_ns is None


def test_pose_source_apply_rejects_fresh_pose_without_age(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="when_idle")
    )
    _add_box(client)
    _post_fresh_box_snapshot(client, age_s=None)

    response = client.post(
        "/api/scene/pose-sources/apply",
        json={"policy": "when_idle", "objectIds": ["box"]},
    )

    assert response.status_code == 409
    payload = response.json()
    assert payload["success"] is False
    assert payload["rejected_count"] == 1
    assert payload["details"][0]["reject_reason"] == "missing_age"
    assert state.kernel.scene_edits[-1].operation == "add"


def test_continuous_snapshot_applies_planning_scene_without_operator_apply(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    refresh_reasons: list[str] = []
    publish_reasons: list[str] = []
    state.request_visual_scene_objects_refresh = (
        lambda *, reason="": refresh_reasons.append(reason)
        or {"success": True, "queued": True}
    )
    state.request_visual_state_publish = (
        lambda *, reason="": publish_reasons.append(reason)
    )

    response = _post_fresh_box_snapshot(client)

    payload = response.json()
    assert payload["continuous_apply"]["success"] is True
    assert payload["continuous_apply"]["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    assert apply_result["success"] is True
    assert apply_result["applied_count"] == 1
    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [1.0, 2.0, 3.0]
    assert state.kernel.scene_edits[-1].operation == "update_pose"
    assert state.kernel.scene_edits[-1].frame_id == "universe"
    snapshot = state.pose_source_snapshots()["isaac_scene_objects"]
    assert state.kernel.scene_edits[-1].source_freshness_budget_ns == 1_000_000_000
    assert (
        state.kernel.scene_edits[-1].source_timestamp_ns
        == snapshot["received_at_ns"] - 50_000_000
    )
    edit = state.kernel.scene_edits[-1]
    assert edit.source_id == "isaac_scene_objects"
    assert edit.scene_admission_role == "snapshot_obstacle"
    assert edit.scene_admission_lease_max_age_ns is None
    assert edit.scene_admission_target_drift_tolerance_m is None
    assert edit.scene_admission_target_angular_tolerance_rad is None
    visual_objects, _meta = state.visual_scene_objects_snapshot()
    assert visual_objects[0].pose_in_universe["position_m"] == [1.0, 2.0, 3.0]
    assert refresh_reasons == []
    # Per-object visual edits are committed with publish=False; one
    # generation-checked batch wake replaces the old duplicate wake.
    assert publish_reasons == [
        "pose_source_continuous",
        "scene_state_cache_empty",
    ]


def test_continuous_duplicate_apply_skips_kernel_scene_edit_and_records_metric(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    _post_fresh_box_snapshot(client)
    first_result = _wait_for_pose_source_apply(state)
    assert first_result["success"] is True
    scene_edit_count = len(state.kernel.scene_edits)
    event_count = len(state.event_recorder.get_events_since(0))

    duplicate = state.apply_pose_source_updates(
        policy="continuous",
        object_ids=["box"],
        expected_generation=state._pose_source_apply_generation_value(),
        reason="duplicate_probe",
    )

    assert duplicate["success"] is True
    assert duplicate["applied_count"] == 0
    assert duplicate["skipped_count"] == 1
    assert duplicate["details"][0]["skip_reason"] == "pose_source_duplicate_no_op"
    assert len(state.kernel.scene_edits) == scene_edit_count
    assert len(state.event_recorder.get_events_since(0)) == event_count
    metrics = state.pose_source_apply_status()["counters"]
    assert metrics["no_op_skipped"] >= 1
    heartbeat = client.get("/api/infra/heartbeat").json()
    assert (
        heartbeat["components"]["pose_source_apply"]["counters"]["no_op_skipped"]
        >= 1
    )


def test_scene_edit_invalidates_pose_source_duplicate_signature(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    _post_fresh_box_snapshot(client)
    first_result = _wait_for_pose_source_apply(state)
    assert first_result["success"] is True

    duplicate = state.apply_pose_source_updates(
        policy="continuous",
        object_ids=["box"],
        expected_generation=state._pose_source_apply_generation_value(),
        reason="duplicate_probe",
    )
    assert duplicate["details"][0]["skip_reason"] == "pose_source_duplicate_no_op"

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "update_pose",
            "object_id": "box",
            "pose_position_m": [9.0, 9.0, 9.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "frame_id": "universe",
        },
    )
    assert response.status_code == 200
    assert response.json()["success"] is True
    scene_edit_count = len(state.kernel.scene_edits)

    reapplied = state.apply_pose_source_updates(
        policy="continuous",
        object_ids=["box"],
        expected_generation=state._pose_source_apply_generation_value(),
        reason="after_scene_edit",
    )

    assert reapplied["success"] is True
    assert reapplied["applied_count"] == 1
    assert reapplied["details"][0].get("update_status") == "geometry_changed"
    assert len(state.kernel.scene_edits) == scene_edit_count + 1


def test_continuous_freshness_only_apply_calls_kernel_without_visual_publish(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    publish_reasons: list[str] = []
    state.request_visual_state_publish = (
        lambda *, reason="": publish_reasons.append(reason)
    )

    _post_fresh_box_snapshot(client)
    first_result = _wait_for_pose_source_apply(state)
    assert first_result["success"] is True
    assert first_result["details"][0].get("update_status") == "geometry_changed"
    scene_edit_count = len(state.kernel.scene_edits)
    publish_reasons.clear()
    event_count = len(state.event_recorder.get_events_since(0))

    _post_fresh_box_snapshot(client)
    second_result = _wait_for_pose_source_apply(state)

    assert second_result["success"] is True
    assert second_result["applied_count"] == 1
    assert second_result["details"][0].get("update_status") == "freshness_only"
    assert second_result["details"][0].get("visual_update_skipped") == "freshness_only"
    assert len(state.kernel.scene_edits) == scene_edit_count + 1
    assert publish_reasons == []
    assert len(state.event_recorder.get_events_since(0)) == event_count
    metrics = state.pose_source_apply_status()["counters"]
    assert metrics["freshness_only_applied"] >= 1


def test_pose_source_clear_invalidates_worker_waiting_for_scene_lock(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous_unattached")
    )
    _add_box(client)

    state.scene_edit_lock.acquire()
    try:
        response = _post_fresh_box_snapshot(client)
        assert response.json()["continuous_apply"]["queued"] is True
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            with state._pose_source_apply_lock:
                if state._pose_source_apply_running or state._pose_source_apply_pending:
                    break
            time.sleep(0.005)
        state.clear_pose_source_snapshots()
    finally:
        state.scene_edit_lock.release()

    result = _wait_for_pose_source_apply(state)
    assert result["success"] is True
    assert result["applied_count"] == 0
    assert result["rejected_count"] == 0
    assert (
        result["details"][0]["skip_reason"]
        == "pose_source_apply_generation_invalidated"
    )
    assert [edit.operation for edit in state.kernel.scene_edits] == ["add"]
    metrics = state.pose_source_apply_status()["counters"]
    assert metrics["stale_generation_dropped"] >= 1


def test_pose_source_invalidation_after_first_object_fences_rest_of_batch(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous",
            can_policy="continuous",
        )
    )
    _add_object(client, "box")
    _add_object(client, "can")
    generation_before = state._pose_source_apply_generation_value()

    first_edit_committed = threading.Event()
    invalidation_done = threading.Event()
    original_scene_edit = state.kernel.scene_edit

    def scene_edit_with_mid_batch_invalidation(request):
        result = original_scene_edit(request)
        if request.operation == "update_pose" and request.object_id == "box":
            first_edit_committed.set()
            # Invalidation intent is wait-free with respect to the per-object
            # commit lock. The invalidator itself remains blocked until this
            # object transaction finishes.
            assert state._pose_source_invalidation_requested.wait(timeout=1.0)
        return result

    state.kernel.scene_edit = scene_edit_with_mid_batch_invalidation
    publish_reasons: list[str] = []
    state.request_visual_state_publish = (
        lambda *, reason="": publish_reasons.append(reason)
    )

    def invalidate() -> None:
        assert first_edit_committed.wait(timeout=1.0)
        state.invalidate_pose_source_applies(reason="deterministic_mid_batch_reset")
        invalidation_done.set()

    invalidator = threading.Thread(target=invalidate, daemon=False)
    invalidator.start()
    response = _post_fresh_snapshot(
        client,
        [
            _pose_source_observation("box", position_xyz=[1.0, 0.0, 0.0]),
            _pose_source_observation("can", position_xyz=[2.0, 0.0, 0.0]),
        ],
    )
    assert response.json()["continuous_apply"]["queued"] is True
    result = _wait_for_pose_source_apply(state)
    invalidator.join(timeout=1.0)

    assert invalidation_done.is_set()
    update_ids = [
        edit.object_id
        for edit in state.kernel.scene_edits
        if edit.operation == "update_pose"
    ]
    assert update_ids == ["box"]
    assert not any(reason.startswith("pose_source_") for reason in publish_reasons)
    assert state._pose_source_apply_signatures == {}
    assert result["reason"] == "deterministic_mid_batch_reset"
    assert result["invalidated_generation"] == generation_before
    assert result["current_generation"] == generation_before + 1
    metrics = state.pose_source_apply_status()["counters"]
    assert metrics["stale_result_ignored"] >= 1


def test_initial_generation_in_flight_apply_is_counted_by_invalidation(
    tmp_path: Path,
) -> None:
    _, state = _client(tmp_path)
    with state._pose_source_apply_lock:
        assert state._pose_source_apply_generation == 0
        state._pose_source_apply_in_flight = {
            "policy": "continuous",
            "object_ids": ["box"],
            "expected_generation": 0,
        }

    result = state.invalidate_pose_source_applies(reason="generation_zero_probe")

    assert result["generation"] == 1
    assert result["dropped_count"] == 1
    assert state.pose_source_apply_status()["last_result"]["skipped_count"] == 1


def test_generation_aware_visual_refresh_cannot_publish_after_invalidation(
    tmp_path: Path,
) -> None:
    _, state = _client(tmp_path)
    read_started = threading.Event()
    release_read = threading.Event()
    original_get_scene_objects = state.kernel.get_scene_objects

    def blocked_get_scene_objects():
        read_started.set()
        assert release_read.wait(timeout=1.0)
        return original_get_scene_objects()

    state.kernel.get_scene_objects = blocked_get_scene_objects
    committed_snapshots: list[list[Any]] = []
    state.update_visual_scene_objects_snapshot = (
        lambda objects, *, error=None: committed_snapshots.append(list(objects))
    )
    generation = state._pose_source_apply_generation_value()

    queued = state.request_visual_scene_objects_refresh(
        reason="generation_fence_probe",
        expected_pose_source_generation=generation,
    )
    assert queued["queued"] is True
    assert read_started.wait(timeout=1.0)
    state.invalidate_pose_source_applies(reason="visual_refresh_invalidation")
    release_read.set()
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        with state._visual_scene_objects_lock:
            if not state._visual_scene_objects_refresh_running:
                break
        time.sleep(0.005)

    assert committed_snapshots == []
    with state._visual_scene_objects_lock:
        assert state._visual_scene_objects_refresh_running is False


def test_continuous_snapshot_skips_live_mutation_during_resumable_stop(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    state.cycle_manager = SimpleNamespace(
        state=CycleState.STOPPED,
        live_scene_mutation_block_reason=lambda: "resumable_stopped_context",
    )

    response = _post_fresh_box_snapshot(client)

    assert response.json()["continuous_apply"]["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    assert apply_result["success"] is True
    assert apply_result["applied_count"] == 0
    assert apply_result["skipped_count"] == 1
    assert apply_result["details"][0]["skip_reason"] == "resumable_stopped_context"
    assert state.kernel.get_scene_objects()[0].pose_in_universe["position_m"] == [
        0.0,
        0.0,
        0.0,
    ]
    assert state.kernel.scene_edits[-1].operation == "add"


def test_continuous_snapshot_fails_closed_when_scene_guard_unavailable(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    state.cycle_manager = SimpleNamespace(
        state=CycleState.STOPPED,
        live_scene_mutation_block_reason=lambda: "scene_mutation_guard_unavailable",
    )

    response = _post_fresh_box_snapshot(client)

    assert response.json()["continuous_apply"]["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    assert apply_result["success"] is False
    assert apply_result["applied_count"] == 0
    assert apply_result["rejected_count"] == 1
    assert (
        apply_result["details"][0]["reject_reason"]
        == "scene_mutation_guard_unavailable"
    )
    assert state.kernel.scene_edits[-1].operation == "add"


def test_continuous_snapshot_without_scene_admission_defaults_to_snapshot_obstacle(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous",
            box_scene_admission=None,
        )
    )
    _add_box(client)

    response = _post_fresh_box_snapshot(client)

    assert response.json()["continuous_apply"]["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    assert apply_result["success"] is True
    assert apply_result["applied_count"] == 1
    assert apply_result["rejected_count"] == 0
    detail = apply_result["details"][0]
    assert detail["scene_admission_role"] == "snapshot_obstacle"
    assert state.kernel.scene_edits[-1].operation == "update_pose"
    assert state.kernel.scene_edits[-1].scene_admission_role == "snapshot_obstacle"
    scene = client.get("/api/scene/state").json()
    assert scene["objects"][0]["pose_in_universe"]["position_m"] == [1.0, 2.0, 3.0]
    observed = scene["pose_sources"][0]["objects"][0]
    assert observed["status"] == "fresh"
    assert observed["update"]["pose"]["position_xyz"] == [1.0, 2.0, 3.0]


def test_scene_state_does_not_wait_for_blocked_pose_source_apply(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    original_scene_edit = state.kernel.scene_edit
    entered = threading.Event()
    release = threading.Event()

    def blocking_scene_edit(request: SceneEditRequest):
        if request.operation == "update_pose":
            entered.set()
            assert release.wait(timeout=5.0)
        return original_scene_edit(request)

    state.kernel.scene_edit = blocking_scene_edit  # type: ignore[method-assign]

    try:
        response = _post_fresh_box_snapshot(client)
        assert response.json()["continuous_apply"]["queued"] is True
        assert entered.wait(timeout=2.0)

        started = time.monotonic()
        scene = client.get("/api/scene/state").json()
        elapsed_ms = (time.monotonic() - started) * 1000.0
    finally:
        release.set()
        apply_result = _wait_for_pose_source_apply(state)

    assert elapsed_ms < 50.0
    assert scene["objects"][0]["pose_in_universe"]["position_m"] == [0.0, 0.0, 0.0]
    assert scene["pose_sources"][0]["objects"][0]["status"] == "fresh"
    assert apply_result["success"] is True
    assert apply_result["applied_count"] == 1


def test_continuous_snapshot_preserves_attached_object_chain(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )
    _add_box(client)
    attach_result = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach_result.success is True

    response = _post_fresh_box_snapshot(client)

    assert response.json()["continuous_apply"]["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    apply_detail = apply_result["details"][0]
    assert apply_detail["status"] == "applied"
    assert apply_detail["attached"] is True
    assert apply_detail["attached_frame_id"] == "tool0"
    assert apply_detail["attached_update_mode"] == "attachment_offset_from_observation"
    assert state.kernel.scene_edits[-1].operation == "update_pose"
    assert state.kernel.scene_edits[-1].frame_id == "tool0"
    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [1.0, 2.0, 3.0]


def test_continuous_unattached_snapshot_applies_only_while_detached(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous_unattached")
    )
    _add_box(client)

    detached_response = _post_fresh_box_snapshot(client)

    detached_apply = detached_response.json()["continuous_apply"]
    assert detached_apply["success"] is True
    assert detached_apply["queued"] is True
    detached_result = _wait_for_pose_source_apply(state)
    assert detached_result["success"] is True
    assert detached_result["applied_count"] == 1
    assert state.kernel.scene_edits[-1].operation == "update_pose"
    assert state.kernel.scene_edits[-1].frame_id == "universe"

    attach_result = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach_result.success is True

    attached_response = _post_fresh_box_snapshot(client)

    attached_apply = attached_response.json()["continuous_apply"]
    assert attached_apply["success"] is True
    assert attached_apply["queued"] is True
    attached_result = _wait_for_pose_source_apply(state)
    assert attached_result["success"] is True
    assert attached_result["applied_count"] == 0
    assert attached_result["skipped_count"] == 1
    detail = attached_result["details"][0]
    assert detail["status"] == "skipped"
    assert detail["skip_reason"] == "attached_object"
    assert detail["attached_frame_id"] == "tool0"
    assert state.kernel.scene_edits[-1].operation == "attach"


def test_continuous_unattached_snapshot_updates_canonical_scene_while_cycle_running(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous_unattached")
    )
    _add_box(client)
    state.cycle_manager = SimpleNamespace(state=CycleState.RUNNING)

    response = _post_fresh_box_snapshot(client)

    apply_request = response.json()["continuous_apply"]
    assert apply_request["success"] is True
    assert apply_request["queued"] is True
    apply_result = _wait_for_pose_source_apply(state)
    assert apply_result["success"] is True
    assert apply_result["applied_count"] == 1
    assert apply_result["skipped_count"] == 0
    detail = apply_result["details"][0]
    assert detail["status"] == "applied"
    assert detail["frame_id"] == "universe"
    visual_objects, _meta = state.visual_scene_objects_snapshot()
    assert visual_objects[0].pose_in_universe["position_m"] == [1.0, 2.0, 3.0]
    assert state.kernel.scene_edits[-1].operation == "update_pose"


def test_continuous_policy_is_not_operator_apply_route(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous")
    )

    response = client.post(
        "/api/scene/pose-sources/apply",
        json={"policy": "continuous", "objectIds": ["box"]},
    )

    assert response.status_code == 409
    assert "runtime latch" in response.json()["error"]


def test_continuous_unattached_policy_is_not_operator_apply_route(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="continuous_unattached")
    )

    response = client.post(
        "/api/scene/pose-sources/apply",
        json={"policy": "continuous_unattached", "objectIds": ["box"]},
    )

    assert response.status_code == 409
    assert "runtime latch" in response.json()["error"]



def test_when_idle_apply_refuses_active_cycle(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="when_idle")
    )
    state.cycle_manager = SimpleNamespace(state=CycleState.RUNNING)

    response = client.post(
        "/api/scene/pose-sources/apply",
        json={"policy": "when_idle", "objectIds": ["box"]},
    )

    assert response.status_code == 409
    assert "requires idle" in response.json()["error"]


def test_before_planning_latch_can_apply_during_running_cycle(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())
    _add_box(client)
    _post_fresh_box_snapshot(client)
    state.cycle_manager = SimpleNamespace(state=CycleState.RUNNING)

    result = state.apply_pose_sources_before_planning(object_ids=["box"])

    assert result["success"] is True
    state.cycle_manager = None
    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [1.0, 2.0, 3.0]
    edit = state.kernel.scene_edits[-1]
    assert edit.source_freshness_budget_ns is None
    assert edit.source_timestamp_ns is None


def test_before_planning_latch_refreshes_non_referenced_bound_objects(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(can_policy="before_planning")
    )
    _add_box(client)
    _add_object(client, "can")
    _post_fresh_snapshot(
        client,
        [
            _pose_source_observation("box", position_xyz=[1.0, 2.0, 3.0]),
            _pose_source_observation("can", position_xyz=[4.0, 5.0, 6.0]),
        ],
    )

    result = state.apply_pose_sources_before_planning(object_ids=["box"])

    assert result["success"] is True
    assert result["applied_count"] == 2
    assert {detail["object_id"] for detail in result["details"]} == {"box", "can"}
    assert [edit.object_id for edit in state.kernel.scene_edits[-2:]] == [
        "box",
        "can",
    ]
    scene = client.get("/api/scene/state").json()
    poses = {
        obj["object_id"]: obj["pose_in_universe"]["position_m"]
        for obj in scene["objects"]
    }
    assert poses["box"] == [1.0, 2.0, 3.0]
    assert poses["can"] == [4.0, 5.0, 6.0]


def test_pose_source_apply_uses_configured_frame_not_provider_frame(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())
    _add_box(client)
    _post_fresh_box_snapshot(client, observed_frame_id="box_provider_frame")

    result = state.apply_pose_sources_before_planning(object_ids=["box"])

    assert result["success"] is True
    assert state.kernel.scene_edits[-1].frame_id == "universe"
    assert result["details"][0]["frame_id"] == "universe"
    assert result["details"][0]["observed_frame_id"] == "box_provider_frame"


def test_pose_source_apply_failure_event_includes_object_reject_details(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())
    _add_box(client)
    _post_fresh_box_snapshot(client, observed_frame_id="box_provider_frame")
    state.kernel.fail_next_scene_edit = True

    result = state.apply_pose_sources_before_planning(object_ids=["box"])

    assert result["success"] is False
    events = state.event_recorder.get_events_since(0)
    event = events[-1]
    assert event["type"] == "pose_source_apply_failed"
    detail = event["details"]["details"][0]
    assert detail["object_id"] == "box"
    assert detail["reject_reason"] == "scene_edit_failed"
    assert detail["frame_id"] == "universe"
    assert detail["observed_frame_id"] == "box_provider_frame"


class _ConditionFaultRecorder:
    def __init__(self) -> None:
        self.published: list[dict[str, Any]] = []
        self.faults: list[dict[str, Any]] = []

    def publish_signal(self, **kwargs: Any) -> None:
        self.published.append(kwargs)

    def raise_runtime_condition_fault(self, **kwargs: Any) -> None:
        self.faults.append(kwargs)


def _measured_tcp_sample(*, monotonic_s: float = 10.0) -> dict[str, Any]:
    return {
        "frameId": "universe",
        "tcpFrameId": "tool0",
        "monotonicS": monotonic_s,
        "ageS": 0.05,
        "pose": {
            "frameId": "universe",
            "stampSec": 100.0,
            "positionXYZ": [0.0, 0.0, 0.0],
            "orientationXYZW": [0.0, 0.0, 0.0, 1.0],
        },
    }


def test_measured_grasp_drift_baseline_and_scene_state_status(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="diagnostic_only",
        )
    )
    _add_box(client)
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True

    _post_fresh_snapshot(
        client,
        [
            _pose_source_observation(
                "box",
                position_xyz=[0.10, 0.0, 0.0],
                monotonic_s=10.0,
            )
        ],
        measured_tcp=_measured_tcp_sample(monotonic_s=10.0),
    )

    scene = client.get("/api/scene/state").json()
    source = scene["pose_sources"][0]
    assert source["measured_tcp"]["tcp_frame_id"] == "tool0"
    drift = source["objects"][0]["attached_drift"]
    assert drift["baseline_captured"] is True
    assert drift["status"] == "ok"
    assert drift["translation_m"] == 0.0
    assert drift["sample_time_delta_s"] == 0.0


def test_measured_grasp_publishes_confirmation_signal(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    recorder = _ConditionFaultRecorder()
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="diagnostic_only",
            debounce_s=1.0,
        )
    )
    _add_box(client)
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True
    state.cycle_manager = recorder

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    confirmation_events = [
        item
        for item in recorder.published
        if item["signal"] == "attached_object_grasp_confirmed/box"
    ]
    assert confirmation_events[-1]["value"] is True
    assert confirmation_events[-1]["mirror_key"] == (
        "attached_object_grasp_confirmed/box"
    )
    assert confirmation_events[-1]["metadata"]["status"] == "ok"

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.15, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    confirmation_events = [
        item
        for item in recorder.published
        if item["signal"] == "attached_object_grasp_confirmed/box"
    ]
    assert confirmation_events[-1]["value"] is False
    assert confirmation_events[-1]["metadata"]["status"] == "near_limit"


def test_measured_grasp_drift_interrupts_after_debounce(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    recorder = _ConditionFaultRecorder()
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="interrupt",
            debounce_s=0.001,
        )
    )
    _add_box(client)
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True
    state.cycle_manager = recorder

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.15, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    assert recorder.faults == []
    time.sleep(0.005)
    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.15, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )

    assert recorder.published[-1]["signal"] == "attached_object_drift/box"
    assert recorder.published[-1]["value"] is True
    assert recorder.faults
    fault = recorder.faults[-1]
    assert fault["condition_id"] == "attached_object_drift/box"
    assert fault["details"]["reason_code"] == "attached_object_drift"
    assert fault["details"]["translation_m"] > 0.02


def test_measured_grasp_drift_marks_confirmed_gripper_lost(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    recorder = _ConditionFaultRecorder()
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="interrupt",
            debounce_s=0.001,
        )
    )
    state.deployment.grippers["primary"] = GripperConfig(
        command_mode="ros_gripper_command",
        group_id="lite_gripper",
        joint_name="right_finger_joint",
        motion_time_s=0.1,
        srdf_presets={"open": 0.04, "closed": 0.0},
        actuation_kind="pneumatic",
        state_authority="sim_observed",
        completion_policy="dwell_elapsed",
        grasp_confirmation_policy="perception",
        transport={"kind": "isaac_joint_state_effort"},
        open_state_ref="open",
        closed_state_ref="closed",
        semantic_configured=True,
        attach_frame="tool0",
        tcp_frame="tool0",
    )
    _add_box(client)
    command = state.kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="primary",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0,
            duration_s=0.0,
            actuation_kind="pneumatic",
            state_authority="sim_observed",
            completion_policy="dwell_elapsed",
            grasp_confirmation_policy="perception",
            transport_kind="isaac_joint_state_effort",
            open_state_ref="open",
            closed_state_ref="closed",
            srdf_presets={"open": 0.04, "closed": 0.0},
        )
    )
    assert command.accepted is True
    confirmed = state.kernel.confirm_gripper_grasp(
        gripper_id="primary",
        grasp_confirmation_policy="perception",
        evidence_label="perception_grasp_confirmed",
    )
    assert confirmed.accepted is True
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True
    state.cycle_manager = recorder

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.15, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )
    time.sleep(0.005)
    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.15, 0.0, 0.0])],
        measured_tcp=_measured_tcp_sample(),
    )

    status = state.kernel.get_gripper_status("primary")
    assert status.state == GripperCommandState.FAILED
    assert status.evidence_state == "pressure_or_part_lost"
    assert status.reason_code == "grasp_confirmation_lost"
    assert "attached_drift_monitor" in status.evidence
    fault = recorder.faults[-1]
    assert fault["details"]["reason_code"] == "attached_object_drift"
    assert fault["details"]["gripper_grasp_loss"]["status"] == "recorded"
    assert fault["details"]["gripper_grasp_loss"]["gripper_id"] == "primary"


def test_measured_grasp_drift_unverifiable_on_mixed_time_samples(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="interrupt",
        )
    )
    _add_box(client)
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0], monotonic_s=12.0)],
        measured_tcp=_measured_tcp_sample(monotonic_s=10.0),
    )

    drift = client.get("/api/scene/state").json()["pose_sources"][0]["objects"][0][
        "attached_drift"
    ]
    assert drift["status"] == "unverifiable"
    assert drift["unverifiable_reason"] == "sample_time_mismatch"


def test_measured_grasp_stale_fault_interrupts_when_tcp_missing(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    recorder = _ConditionFaultRecorder()
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(
            box_policy="continuous_unattached",
            measured_grasp=True,
            drift_action="interrupt",
        )
    )
    _add_box(client)
    attach = state.kernel.scene_edit(
        SceneEditRequest(operation="attach", object_id="box", frame_id="tool0")
    )
    assert attach.success is True
    state.cycle_manager = recorder

    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0])],
    )

    assert recorder.published[-1]["signal"] == "attached_object_drift/box"
    assert recorder.published[-1]["value"] is True
    fault = recorder.faults[-1]
    assert fault["condition_id"] == "attached_object_drift/box"
    assert fault["details"]["reason_code"] == "attached_object_drift_unverifiable"
    assert fault["details"]["reason"] == "measured_tcp_missing"
    drift = state.pose_source_drift_states()[("isaac_scene_objects", "box")]
    assert drift["status"] == "unverifiable"
    assert drift["unverifiable_reason"] == "measured_tcp_missing"
    event_count = sum(
        1
        for event in state.event_recorder.get_events_since(0)
        if event["type"] == "pose_source_drift_unverifiable"
    )
    _post_fresh_snapshot(
        client,
        [_pose_source_observation("box", position_xyz=[0.10, 0.0, 0.0])],
    )
    assert len(recorder.faults) == 1
    assert (
        sum(
            1
            for event in state.event_recorder.get_events_since(0)
            if event["type"] == "pose_source_drift_unverifiable"
        )
        == event_count
    )


def test_between_nodes_latch_can_apply_during_running_cycle(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.deployment = _minimal_deployment(
        pose_sources=_pose_sources(box_policy="between_nodes")
    )
    _add_box(client)
    _post_fresh_box_snapshot(client)
    state.cycle_manager = SimpleNamespace(state=CycleState.RUNNING)

    result = state.apply_pose_sources_between_nodes(object_ids=["box"])

    assert result["success"] is True
    state.cycle_manager = None
    scene = client.get("/api/scene/state").json()
    object_pose = scene["objects"][0]["pose_in_universe"]
    assert object_pose["position_m"] == [1.0, 2.0, 3.0]
    edit = state.kernel.scene_edits[-1]
    assert edit.source_freshness_budget_ns is None
    assert edit.source_timestamp_ns is None


def test_before_planning_latch_fails_closed_without_fresh_pose(tmp_path: Path) -> None:
    _client_obj, state = _client(tmp_path)
    state.deployment = _minimal_deployment(pose_sources=_pose_sources())

    result = state.apply_pose_sources_before_planning(object_ids=["box"])

    assert result["success"] is False
    assert "no fresh pose-source update" in result["error"]
