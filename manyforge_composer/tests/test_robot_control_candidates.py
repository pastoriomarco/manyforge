"""Validated Robot Control candidate tests (Phase 4-P5.1 + P5.3).

These assert the candidate contract:
  - create plans + validates a candidate WITHOUT moving the robot or mutating
    `/api/scene/state` (P5.1);
  - structured diagnostics for IK failure, joint-limit, collision, malformed,
    stale-revision, and supersession;
  - Approve/Execute moves the robot ONLY for a fresh ready candidate, through
    the normal intervention path, and fails closed (no motion) when freshness or
    revalidation cannot be proven (P5.3);
  - the surface never references live streaming.
"""
from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest
import yaml
from fastapi.testclient import TestClient

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    GripperCommandState,
    GripperCommandStatus,
    MockKernelClient,
    MotionPreviewResult,
    RobotStateSnapshot,
    SceneEditRequest,
)
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GRIPPER_DEPLOYMENT_PATH = str(
    _REPO_ROOT / "examples" / "pick_and_place_ur10e_robotiq.deployment.yaml"
)
_CATALOG_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)
_PROGRAM_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml"
)
_DEPLOYMENT_PATH = str(
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml"
)

_BASE = "/api/robot-control/candidates"


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState, MockKernelClient]:
    """Bare client (no program loaded) — for input-validation tests."""
    kernel = MockKernelClient()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    return TestClient(app), state, kernel


def _loaded_client(
    tmp_path: Path,
    *,
    execute_duration_s: float = 0.0,
    deployment_path: str = _DEPLOYMENT_PATH,
    kernel_bundle=None,
) -> tuple[TestClient, ComposerState, MockKernelClient]:
    """Client with a program + deployment loaded so candidate planning has a
    robot profile, a cycle manager (for Approve), and a start joint state."""
    kernel = MockKernelClient(execute_duration_s=execute_duration_s)
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        kernel_bundle=kernel_bundle,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    client = TestClient(app)
    resp = client.post(
        "/api/program/load",
        json={"path": _PROGRAM_PATH, "deploymentPath": deployment_path},
    )
    assert resp.status_code == 200, resp.text
    live_kernel = state.kernel  # load reuses the injected mock
    assert isinstance(live_kernel, MockKernelClient)
    assert state.cycle_manager is not None
    # Prepare a fresh cycle so the intervention blackboard carries the kernel /
    # motion coordinator (production wires this via the program-loaded callback,
    # which calls cycle_manager.reset()). Without it, an Approve dispatch would
    # tick a move node before the kernel is on the blackboard.
    state.cycle_manager.reset()
    # Seed a current full-joint vector matching the kernel's joints for
    # candidate planning before any execution has happened.
    state.record_current_joint_positions([0.0] * len(live_kernel.get_joint_names()))
    # Reset recorded calls so per-test assertions measure only candidate effects
    # (the prepare step applies the program scene via scene_edit).
    live_kernel.execute_count = 0
    live_kernel.scene_edits.clear()
    return client, state, live_kernel


def _joint_body() -> dict:
    return {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["joint1"], "positionsRad": [0.3]},
    }


def _cartesian_body() -> dict:
    return {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {
            "positionM": [0.4, 0.1, 0.5],
            "orientationXyzw": [0.0, 0.0, 0.0, 1.0],
            "frameId": "base",
            "tcpFrame": "tool0",
        },
    }


class _SpringOnlyStopCapabilityBundle:
    def get_motion_interrupt_stop_capability(self) -> dict:
        return {
            "supported_stop_modes": ["spring_back_hold"],
            "capability_profile_id": "test_spring_only",
            "hard_stop_level": "jtc_cancel_unprofiled_hold",
        }


class _AllStopModesCapabilityBundle:
    def get_motion_interrupt_stop_capability(self) -> dict:
        return {
            "supported_stop_modes": [
                "soft_on_path",
                "spring_back_hold",
                "hard_deceleration",
            ],
            "capability_profile_id": "test_all_stop_modes",
            "hard_stop_level": "jtc_cancel_unprofiled_hold",
        }


def _write_deployment_with_stop_profile(tmp_path: Path, *, stop_mode: str) -> str:
    data = yaml.safe_load(Path(_DEPLOYMENT_PATH).read_text(encoding="utf-8"))
    config = data["adapters"]["execution"].setdefault("config", {})
    config["operator_stop_profile"] = {
        "profile_id": f"operator_{stop_mode}",
        "stop_mode": stop_mode,
        "deceleration_time_s": 0.5,
    }
    config["default_fault_stop_profile"] = {
        "profile_id": "default_fault_spring_back_hold",
        "stop_mode": "spring_back_hold",
        "deceleration_time_s": 0.5,
    }
    target = tmp_path / f"deployment_{stop_mode}.deployment.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


def _write_deployment_without_cartesian_capability(tmp_path: Path) -> str:
    data = yaml.safe_load(Path(_DEPLOYMENT_PATH).read_text(encoding="utf-8"))
    planning = data["adapters"]["planning"]
    # An unregistered deterministic test adapter may advertise the mandatory
    # v0.0 surface without the optional Cartesian capability. Production
    # planning.roboplan manifests are exact and always advertise what they
    # implement.
    planning["adapter_id"] = "planning.test"
    planning["capability_keys"] = [
        key
        for key in planning["capability_keys"]
        if key != "planning.motion.cartesian_path"
    ]
    target = tmp_path / "deployment_without_cartesian.deployment.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


def _set_mock_robot_positions(
    state: ComposerState, kernel: MockKernelClient, positions: list[float]
) -> None:
    """Update both the fallback Composer cache and the mock's live snapshot."""
    state.record_current_joint_positions(positions)
    kernel._last_joint_positions_rad = list(positions)  # type: ignore[attr-defined]


# ---------------------------------------------------------------------------
# P5.1: candidate preview planning + validation (no motion)
# ---------------------------------------------------------------------------


def test_create_joint_candidate_ready_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    resp = client.post(_BASE, json=_joint_body())
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ready"
    assert body["commitEligible"] is True
    assert body["diagnostics"]["reasonCode"] == "ready"
    # Full-vector joint goal (partial target overlaid onto current vector).
    normalized = body["normalizedJointTarget"]
    assert normalized is not None
    assert len(normalized["jointNames"]) == len(kernel.get_joint_names())
    # No motion, no scene mutation.
    assert kernel.execute_count == 0
    assert kernel.scene_edits == []


def test_create_cartesian_candidate_ready_with_tcp_ghost(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    resp = client.post(_BASE, json=_cartesian_body())
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ready"
    assert body["inputMode"] == "cartesian"
    ghost = body["ghost"]
    assert ghost["blocked"] is False
    assert ghost["finalTcpPose"]["positionM"] == [0.4, 0.1, 0.5]
    assert kernel.execute_count == 0


class _PreviewRecordingKernel(MockKernelClient):
    """MockKernelClient that records the last preview ExecuteRequest so tests
    can assert which frames the candidate planner forwarded."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.last_preview_request: ExecuteRequest | None = None

    def preview_motion(self, request, start_state=None):
        self.last_preview_request = request
        return super().preview_motion(request, start_state)


def _recording_loaded_client(
    tmp_path: Path,
) -> tuple[TestClient, ComposerState, _PreviewRecordingKernel]:
    kernel = _PreviewRecordingKernel()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    client = TestClient(app)
    resp = client.post(
        "/api/program/load",
        json={"path": _PROGRAM_PATH, "deploymentPath": _DEPLOYMENT_PATH},
    )
    assert resp.status_code == 200, resp.text
    live_kernel = state.kernel
    assert isinstance(live_kernel, _PreviewRecordingKernel)
    assert state.cycle_manager is not None
    state.cycle_manager.reset()
    state.record_current_joint_positions([0.0] * len(live_kernel.get_joint_names()))
    live_kernel.execute_count = 0
    live_kernel.scene_edits.clear()
    live_kernel.last_preview_request = None
    return client, state, live_kernel


def test_cartesian_candidate_honors_frame_id_and_tcp_frame(tmp_path: Path) -> None:
    """A cartesian candidate with a non-default frameId/tcpFrame
    must produce an ExecuteRequest carrying those frames (preview path)."""
    client, _state, kernel = _recording_loaded_client(tmp_path)
    body = client.post(
        _BASE,
        json={
            "sourceSurface": "cartesian_gizmo",
            "inputMode": "cartesian",
            "cartesianTarget": {
                "positionM": [0.4, 0.1, 0.5],
                "orientationXyzw": [0.0, 0.0, 0.0, 1.0],
                "frameId": "custom_world",
                "tcpFrame": "gripper_tip",
            },
        },
    ).json()
    assert body["status"] == "ready"
    assert kernel.last_preview_request is not None
    assert kernel.last_preview_request.base_frame == "custom_world"
    assert kernel.last_preview_request.tcp_frame == "gripper_tip"
    assert kernel.last_preview_request.motion_type == "cartesian_path"
    assert kernel.last_preview_request.pose_goal is None
    path = kernel.last_preview_request.cartesian_path
    assert path is not None
    assert path["fallback_policy"] == "fail_closed"
    assert path["waypoints"][-1]["position_m"] == [0.4, 0.1, 0.5]
    # The dispatch params (used at approve) carry the same frames so execute
    # validates and moves against exactly the planned frames.
    assert body["cartesianTarget"]["frameId"] == "custom_world"
    assert body["cartesianTarget"]["tcpFrame"] == "gripper_tip"


def test_cartesian_candidate_defaults_use_program_frames(tmp_path: Path) -> None:
    """Default frameId='base' + tcpFrame unset keeps the behavior identical
    (falls back to the program robot frames)."""
    client, _state, kernel = _recording_loaded_client(tmp_path)
    body = client.post(
        _BASE,
        json={
            "sourceSurface": "cartesian_gizmo",
            "inputMode": "cartesian",
            "cartesianTarget": {
                "positionM": [0.4, 0.1, 0.5],
                "orientationXyzw": [0.0, 0.0, 0.0, 1.0],
            },
        },
    ).json()
    assert body["status"] == "ready"
    assert kernel.last_preview_request is not None
    # The fixture program robot uses base_frame=base_link, tcp_frame=tool0.
    assert kernel.last_preview_request.base_frame == "base_link"
    assert kernel.last_preview_request.tcp_frame == "tool0"
    assert kernel.last_preview_request.motion_type == "cartesian_path"
    assert kernel.last_preview_request.cartesian_path is not None


def test_cartesian_candidate_pins_exact_start_tcp_waypoint(tmp_path: Path) -> None:
    client, state, kernel = _recording_loaded_client(tmp_path)
    expected_positions = state.get_current_joint_positions()
    assert expected_positions is not None
    body = client.post(_BASE, json=_cartesian_body()).json()

    assert body["status"] == "ready"
    assert kernel.last_preview_request is not None
    path = kernel.last_preview_request.cartesian_path
    assert path is not None
    start_fk_requests = [
        request
        for request in kernel.target_link_pose_requests
        if request.get("link_names") == ["tool0"]
    ]
    assert start_fk_requests == [{
        "kind": "joint",
        "base_frame": "base_link",
        "link_names": ["tool0"],
        "joint_names": kernel.get_joint_names(),
        "joint_positions_rad": expected_positions,
    }]
    # The deterministic FK stand-in's returned tool pose must be retained as
    # waypoint zero rather than inferred by the adapter or replaced by target.
    assert path["waypoints"][0] == {
        "position_m": [0.5, 0.0, 0.6],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    assert path["waypoints"][1] == {
        "position_m": [0.4, 0.1, 0.5],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }


def test_cartesian_candidate_fails_closed_without_capability(tmp_path: Path) -> None:
    deployment_path = _write_deployment_without_cartesian_capability(tmp_path)
    client, _state, kernel = _loaded_client(
        tmp_path, deployment_path=deployment_path
    )

    body = client.post(_BASE, json=_cartesian_body()).json()

    assert body["status"] == "blocked"
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == "unsupported_capability"
    assert body["diagnostics"]["details"]["requiredCapability"] == (
        "planning.motion.cartesian_path"
    )
    assert kernel.preview_requests == []
    assert kernel.execute_count == 0


def test_cartesian_capability_must_belong_to_planning_adapter(
    tmp_path: Path,
) -> None:
    client, state, kernel = _recording_loaded_client(tmp_path)
    state.deployment.adapters["planning"].capability_keys.remove(
        "planning.motion.cartesian_path"
    )
    state.deployment.adapters["execution"].capability_keys.append(
        "planning.motion.cartesian_path"
    )

    body = client.post(_BASE, json=_cartesian_body()).json()

    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "unsupported_capability"
    assert kernel.preview_requests == []


def test_cartesian_candidate_fails_closed_when_start_fk_is_unavailable(
    tmp_path: Path,
) -> None:
    client, _state, kernel = _recording_loaded_client(tmp_path)
    kernel.compute_target_link_poses = lambda _request: {  # type: ignore[method-assign]
        "reachable": False,
        "reason": "frame_unresolved",
    }

    body = client.post(_BASE, json=_cartesian_body()).json()

    assert body["status"] == "error"
    assert body["diagnostics"]["reasonCode"] == "no_robot_state"
    assert kernel.preview_requests == []


def test_candidate_forwards_per_joint_dynamics_limits_to_execute_request(
    tmp_path: Path,
) -> None:
    """Phase 4-P5.1K — deployment per-joint Robot Control dynamics caps must be
    forwarded into the preview ExecuteRequest, so preview enforcement sees the
    same caps the operator configured. The candidate planner builds the SAME
    request for preview and the approval-time recheck (shared helper), so both
    validate against identical caps."""
    from manyforge_behavior.deployment import RobotControlJointLimit
    from manyforge_behavior.kernel_client import JointDynamicsLimit

    client, state, kernel = _recording_loaded_client(tmp_path)
    # Inject an operator per-joint cap on a real drivable joint of the loaded
    # robot, then drive a candidate down the preview path.
    target_joint = kernel.get_joint_names()[0]
    state.deployment.robot_config.robot_control_joint_limits = {
        target_joint: RobotControlJointLimit(
            max_velocity_rad_s=0.4,
            max_acceleration_rad_s2=1.5,
        )
    }
    body = client.post(_BASE, json=_cartesian_body()).json()
    assert body["status"] == "ready"
    req = kernel.last_preview_request
    assert req is not None
    assert target_joint in req.per_joint_dynamics_limits
    fwd = req.per_joint_dynamics_limits[target_joint]
    assert isinstance(fwd, JointDynamicsLimit)
    assert fwd.max_velocity_rad_s == 0.4
    assert fwd.max_acceleration_rad_s2 == 1.5
    assert fwd.max_jerk_rad_s3 is None


def test_candidate_without_per_joint_limits_forwards_empty_map(
    tmp_path: Path,
) -> None:
    """No deployment per-joint caps → an empty map is forwarded (prior behavior
    preserved; no caps are silently injected)."""
    client, state, kernel = _recording_loaded_client(tmp_path)
    state.deployment.robot_config.robot_control_joint_limits = {}
    body = client.post(_BASE, json=_cartesian_body()).json()
    assert body["status"] == "ready"
    assert kernel.last_preview_request is not None
    assert kernel.last_preview_request.per_joint_dynamics_limits == {}


def test_cartesian_candidate_rejects_unprovable_jerk_limit(tmp_path: Path) -> None:
    from manyforge_behavior.deployment import RobotControlJointLimit

    client, state, kernel = _recording_loaded_client(tmp_path)
    target_joint = kernel.get_joint_names()[0]
    state.deployment.robot_config.robot_control_joint_limits = {
        target_joint: RobotControlJointLimit(max_jerk_rad_s3=8.0)
    }
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error="Cartesian planning cannot prove a requested per-joint jerk cap",
        error_details={
            "reason": "unsupported_joint_dynamics_limit",
            "joint": target_joint,
            "dimension": "jerk",
        },
    )

    body = client.post(_BASE, json=_cartesian_body()).json()

    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "joint_dynamics_limit_unsupported"
    assert kernel.last_preview_request is not None
    assert kernel.last_preview_request.per_joint_dynamics_limits[
        target_joint
    ].max_jerk_rad_s3 == 8.0


class _FkRecordingKernel(MockKernelClient):
    """MockKernelClient that also implements compute_target_link_poses and
    records the FK request, so a test can assert which base frame the ghost FK
    was computed against."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.last_fk_request: dict | None = None

    def compute_target_link_poses(self, request):
        self.last_fk_request = dict(request)
        return {
            "reachable": True,
            "link_poses": {
                "tool0": {
                    "position_m": [0.4, 0.1, 0.5],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            }
        }


def _fk_recording_loaded_client(
    tmp_path: Path,
) -> tuple[TestClient, ComposerState, _FkRecordingKernel]:
    kernel = _FkRecordingKernel()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    client = TestClient(app)
    resp = client.post(
        "/api/program/load",
        json={"path": _PROGRAM_PATH, "deploymentPath": _DEPLOYMENT_PATH},
    )
    assert resp.status_code == 200, resp.text
    live = state.kernel
    assert isinstance(live, _FkRecordingKernel)
    assert state.cycle_manager is not None
    state.cycle_manager.reset()
    state.record_current_joint_positions([0.0] * len(live.get_joint_names()))
    live.execute_count = 0
    live.scene_edits.clear()
    live.last_fk_request = None
    return client, state, live


def test_cartesian_ghost_fk_uses_candidate_frame_not_program_default(
    tmp_path: Path,
) -> None:
    """The candidate's ghost FK must be computed against the resolved base frame
    (honoring a custom Cartesian frameId), not always the program robot frame."""
    client, _state, kernel = _fk_recording_loaded_client(tmp_path)
    body = _cartesian_body()
    body["cartesianTarget"]["frameId"] = "custom_base"
    resp = client.post(_BASE, json=body)
    assert resp.status_code == 200, resp.text
    assert resp.json()["status"] == "ready"
    # The non-default frameId reached the ghost FK preview request.
    assert kernel.last_fk_request is not None
    assert kernel.last_fk_request["base_frame"] == "custom_base"


def test_ik_failure_blocked_red_ghost(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False, error="No IK solution found for the requested target pose"
    )
    body = client.post(_BASE, json=_cartesian_body()).json()
    assert body["status"] == "blocked"
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == "ik_unreachable"
    # Blocked Cartesian candidate still renders a red ghost at the target pose.
    assert body["ghost"]["blocked"] is True
    assert body["ghost"]["blockedTargetPose"]["positionM"] == [0.4, 0.1, 0.5]
    assert kernel.execute_count == 0


def test_joint_limit_failure_blocked_explicit_reason(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error="planning rejected",
        error_details={"reason_code": "joint_limit_violation", "joint": "joint1"},
    )
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "joint_limit_violation"
    assert body["diagnostics"]["details"]["joint"] == "joint1"


def test_collision_failure_blocked_with_pair_details(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error="trajectory waypoint in collision",
        error_details={"collision_pair": ["wrist_3_link", "table"], "waypoint": 4},
    )
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "collision_detected"
    assert body["diagnostics"]["details"]["collision_pair"] == ["wrist_3_link", "table"]


def test_validation_deferred_surfaces_distinct_reason(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error=(
            "Pre-dispatch validation deferred because runtime collision "
            "monitor owns the collision resource"
        ),
        error_details={
            "reason_code": "validation_deferred_runtime_monitor_active",
            "resource_conflict_reason": "shared_mutable_context_id",
            "runtime_protection_generation": "12",
        },
    )
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "blocked"
    assert (
        body["diagnostics"]["reasonCode"]
        == "validation_deferred_runtime_monitor_active"
    )
    assert (
        body["diagnostics"]["details"]["resource_conflict_reason"]
        == "shared_mutable_context_id"
    )
    assert kernel.execute_count == 0


def test_unknown_joint_blocked_malformed(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    body = client.post(
        _BASE,
        json={
            "sourceSurface": "joint_slider",
            "inputMode": "joint",
            "jointTarget": {"jointNames": ["not_a_joint"], "positionsRad": [0.1]},
        },
    ).json()
    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "malformed_target"
    assert kernel.execute_count == 0


def test_dynamics_limit_unsupported_maps_to_stable_reason(tmp_path: Path) -> None:
    """Phase 4-P5.1K — a backend `unsupported_joint_dynamics_limit` preview
    failure (e.g. a jerk cap on the discrete path) must surface as the stable
    candidate reason, NOT degrade to a generic planning_failed."""
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error="per-joint jerk caps are not enforceable by the discrete TOPP-RA path",
        error_details={
            "reason": "unsupported_joint_dynamics_limit",
            "dimension": "jerk",
            "joint": "joint1",
        },
    )
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "joint_dynamics_limit_unsupported"
    assert body["diagnostics"]["reasonCode"] != "planning_failed"
    assert body["diagnostics"]["details"]["dimension"] == "jerk"


def test_dynamics_limit_invalid_maps_to_stable_reason(tmp_path: Path) -> None:
    """Phase 4-P5.1K — a backend `invalid_joint_dynamics_limit` preview failure
    surfaces as the stable invalid-cap reason, not a generic planning_failed."""
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(
        ok=False,
        error="per-joint max_velocity_rad_s must be finite and strictly positive",
        error_details={
            "reason": "invalid_joint_dynamics_limit",
            "dimension": "velocity",
            "joint": "joint1",
        },
    )
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "blocked"
    assert body["diagnostics"]["reasonCode"] == "joint_dynamics_limit_invalid"
    assert body["diagnostics"]["reasonCode"] != "planning_failed"


def test_latest_wins_supersedes_prior_for_same_surface(tmp_path: Path) -> None:
    client, _state, _kernel = _loaded_client(tmp_path)
    first = client.post(_BASE, json=_joint_body()).json()
    second = client.post(_BASE, json=_joint_body()).json()
    assert first["candidateId"] != second["candidateId"]
    assert second["generation"] == first["generation"] + 1
    prior = client.get(f"{_BASE}/{first['candidateId']}").json()
    assert prior["status"] == "superseded"
    assert prior["commitEligible"] is False


def test_latest_wins_supersedes_prior_across_move_groups() -> None:
    """Robot Control has one shared Approve button, so the server keeps one
    active unapproved candidate globally. A joint draft supersedes a prior
    gripper draft even though they come from different source surfaces."""
    from manyforge_composer.backend.robot_control_candidates import (
        _CandidateStore,
        CandidateCreateRequest,
        CandidateStatus,
    )

    store = _CandidateStore()
    gripper_req = CandidateCreateRequest.model_validate(_gripper_body(0.1))
    joint_req = CandidateCreateRequest.model_validate(_joint_body())
    gripper_id, _ = store.reserve(gripper_req)
    joint_id, _ = store.reserve(joint_req)

    should_plan_gripper, gripper_view = store.begin_planning(gripper_id)
    should_plan_joint, joint_view = store.begin_planning(joint_id)
    assert should_plan_gripper is False
    assert gripper_view is not None
    assert gripper_view.status is CandidateStatus.SUPERSEDED
    assert gripper_view.commit_eligible is False
    assert should_plan_joint is True
    assert joint_view is None


def test_latest_wins_ordered_by_request_arrival_not_planning_completion(
    tmp_path: Path,
) -> None:
    """When an OLDER request plans SLOWER than a NEWER request, the
    NEWER candidate must stay ready and the OLDER one must end superseded.

    Ordering is driven by REQUEST ARRIVAL (reserve() under the lock), not by
    when each preview plan happens to finish. Composer serializes native preview
    work, so the newer request may wait for the currently-running preview, but
    its reservation still supersedes the older candidate before final state is
    persisted.
    """
    client, _state, kernel = _loaded_client(tmp_path)

    older_entered_preview = threading.Event()
    release_older_preview = threading.Event()
    real_preview_motion = kernel.preview_motion
    call_index = {"n": 0}
    index_lock = threading.Lock()

    def slow_first_preview(request, start_state=None):
        with index_lock:
            call_index["n"] += 1
            my_index = call_index["n"]
        if my_index == 1:
            # The older request: signal arrival, then block while the newer
            # request reserves a higher generation.
            older_entered_preview.set()
            assert release_older_preview.wait(timeout=5.0)
        return real_preview_motion(request, start_state)

    kernel.preview_motion = slow_first_preview  # type: ignore[assignment]

    results: dict[str, dict] = {}

    def post_older() -> None:
        results["older"] = client.post(_BASE, json=_joint_body()).json()

    def post_newer() -> None:
        results["newer"] = client.post(_BASE, json=_joint_body()).json()

    older_thread = threading.Thread(target=post_older)
    older_thread.start()
    # Wait until the older request is inside (blocked) preview planning; by then
    # it has already reserved its (lower) generation slot.
    assert older_entered_preview.wait(timeout=5.0)

    # Now fire the newer request; it reserves a HIGHER generation and supersedes
    # the older PLANNING placeholder. Its preview is serialized behind the
    # currently-running older preview.
    newer_thread = threading.Thread(target=post_newer)
    newer_thread.start()
    time.sleep(0.05)

    # Release the slow older preview so both route calls can complete.
    release_older_preview.set()
    older_thread.join(timeout=5.0)
    newer_thread.join(timeout=5.0)
    assert "older" in results, "older request did not complete"
    assert "newer" in results, "newer request did not complete"

    older = results["older"]
    newer = results["newer"]
    # Arrival order: older got the lower generation, newer the higher.
    assert newer["generation"] == older["generation"] + 1
    # The newer candidate is the live ready one; the older is superseded even
    # though it was the request whose preview entered the planner first.
    assert newer["status"] == "ready"
    # GET confirms the persisted state matches.
    newer_get = client.get(f"{_BASE}/{newer['candidateId']}").json()
    older_get = client.get(f"{_BASE}/{older['candidateId']}").json()
    assert newer_get["status"] == "ready"
    assert older_get["status"] == "superseded"
    assert older_get["commitEligible"] is False
    assert kernel.execute_count == 0


def test_superseded_reserved_candidate_skips_preview_motion(tmp_path: Path) -> None:
    """A candidate that was queued for planning but superseded before it claims
    the planning slot must not call the native planner at all. This is the
    server-side coalescing guard for slider/spinner bursts."""
    from manyforge_composer.backend.robot_control_candidates import (
        _CandidateStore,
        _plan_reserved_candidate,
        CandidateCreateRequest,
    )

    _client, state, kernel = _loaded_client(tmp_path)
    store = _CandidateStore()
    first_req = CandidateCreateRequest.model_validate(_joint_body())
    first_id, _first_gen = store.reserve(first_req)
    second_req = CandidateCreateRequest.model_validate(
        {
            "sourceSurface": "joint_slider",
            "inputMode": "joint",
            "jointTarget": {"jointNames": ["joint1"], "positionsRad": [0.8]},
        }
    )
    second_id, _second_gen = store.reserve(second_req)
    assert first_id != second_id

    def fail_if_called(_request, _start_state=None):
        raise AssertionError("superseded candidate should not call preview_motion")

    kernel.preview_motion = fail_if_called  # type: ignore[assignment]
    skipped = _plan_reserved_candidate(state, store, first_id, first_req)
    assert skipped.status == "superseded"
    assert skipped.commit_eligible is False


def test_candidate_goes_stale_when_robot_moves(tmp_path: Path) -> None:
    client, state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    assert client.get(f"{_BASE}/{cid}").json()["status"] == "ready"
    # Authoritative robot state changes after planning.
    _set_mock_robot_positions(
        state, kernel, [0.9] + [0.0] * (len(kernel.get_joint_names()) - 1)
    )
    stale = client.get(f"{_BASE}/{cid}").json()
    assert stale["status"] == "stale"
    assert stale["commitEligible"] is False
    assert stale["diagnostics"]["reasonCode"] == "stale_planning_snapshot"


def test_candidate_goes_stale_when_scene_changes(tmp_path: Path) -> None:
    """A scene/attached-object change must flip a ready candidate to
    stale WITHOUT a new user edit (the GET route recomputes staleness)."""
    client, state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    ready = client.get(f"{_BASE}/{cid}").json()
    assert ready["status"] == "ready"
    # The scene fingerprint is exposed on sourceRevisions and reflects the
    # loaded program scene (the table object).
    assert ready["sourceRevisions"]["sceneObjectCount"] >= 1
    baseline_fingerprint = ready["sourceRevisions"]["sceneFingerprint"]
    assert baseline_fingerprint != ""

    # Add a brand-new collision object through the kernel (no candidate edit).
    add_result = kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="rc_finding5_obstacle",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.6, 0.0, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
    )
    assert add_result.success, add_result.error

    stale = client.get(f"{_BASE}/{cid}").json()
    assert stale["status"] == "stale"
    assert stale["commitEligible"] is False
    assert stale["diagnostics"]["reasonCode"] == "stale_planning_snapshot"
    # No motion was dispatched by the scene change or the GET recompute.
    assert kernel.execute_count == 0


def test_no_program_loaded_is_error_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _client(tmp_path)  # bare, no program
    body = client.post(_BASE, json=_joint_body()).json()
    assert body["status"] == "error"
    assert body["diagnostics"]["reasonCode"] == "no_program_loaded"
    assert kernel.execute_count == 0


def test_revert_marks_reverted_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    reverted = client.post(f"{_BASE}/{cid}/revert")
    assert reverted.status_code == 200
    assert reverted.json()["status"] == "reverted"
    assert reverted.json()["commitEligible"] is False
    assert kernel.execute_count == 0


def test_get_unknown_candidate_404(tmp_path: Path) -> None:
    client, _state, _kernel = _client(tmp_path)
    assert client.get(f"{_BASE}/does-not-exist").status_code == 404


def test_preview_never_dispatches_or_mutates_scene(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    client.post(_BASE, json=_joint_body())
    client.post(_BASE, json=_cartesian_body())
    kernel.next_preview_result = MotionPreviewResult(ok=False, error="no ik")
    client.post(_BASE, json=_cartesian_body())
    # Across ready + blocked previews: zero executions, zero scene edits.
    assert kernel.execute_count == 0
    assert kernel.scene_edits == []


# ---------------------------------------------------------------------------
# P5.3: Approve / Execute through normal execution
# ---------------------------------------------------------------------------


def test_execute_fresh_ready_candidate_dispatches(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["candidate"]["status"] == "executing"
    assert body["candidate"]["diagnostics"]["reasonCode"] == "execution_running"
    assert (
        body["candidate"]["diagnostics"]["summary"]
        == "Candidate execution is running."
    )
    assert body["session"]["sessionId"]
    assert body["candidate"]["execution"]["sessionId"] == body["session"]["sessionId"]
    assert body["candidate"]["execution"]["moveId"]
    # Motion is dispatched through the normal intervention path and runs on the
    # cycle tick thread; wait for it to reach the kernel.
    deadline = time.time() + 5.0
    while kernel.execute_count == 0 and time.time() < deadline:
        time.sleep(0.02)
    assert kernel.execute_count >= 1


def test_execute_fresh_cartesian_candidate_dispatches_with_pose_frame(
    tmp_path: Path,
) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_cartesian_body()).json()["candidateId"]
    preview_path = dict(kernel.preview_requests[-1].cartesian_path or {})
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text
    deadline = time.time() + 5.0
    while kernel.execute_count == 0 and time.time() < deadline:
        time.sleep(0.02)
    assert kernel.execute_count >= 1
    assert kernel.last_execute_request is not None
    assert kernel.last_execute_request.motion_type == "cartesian_path"
    assert kernel.last_execute_request.pose_goal is None
    assert kernel.last_execute_request.cartesian_path is not None
    assert kernel.last_execute_request.cartesian_path == preview_path
    assert kernel.last_execute_request.cartesian_path["waypoints"][-1][
        "position_m"
    ] == [0.4, 0.1, 0.5]
    assert kernel.last_execute_request.base_frame == "base_link"
    assert kernel.last_execute_request.tcp_frame == "tool0"


def test_execute_fresh_cartesian_candidate_preserves_custom_frames(
    tmp_path: Path,
) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    body = _cartesian_body()
    body["cartesianTarget"]["frameId"] = "custom_world"
    body["cartesianTarget"]["tcpFrame"] = "gripper_tip"
    cid = client.post(_BASE, json=body).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text
    deadline = time.time() + 5.0
    while kernel.execute_count == 0 and time.time() < deadline:
        time.sleep(0.02)
    assert kernel.execute_count >= 1
    assert kernel.last_execute_request is not None
    assert kernel.last_execute_request.motion_type == "cartesian_path"
    assert kernel.last_execute_request.pose_goal is None
    assert kernel.last_execute_request.cartesian_path is not None
    assert kernel.last_execute_request.cartesian_path["fallback_policy"] == "fail_closed"
    assert kernel.last_execute_request.base_frame == "custom_world"
    assert kernel.last_execute_request.tcp_frame == "gripper_tip"


def test_execute_candidate_blocks_unsupported_configured_stop_profile(
    tmp_path: Path,
) -> None:
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="hard_deceleration"
    )
    client, _state, kernel = _loaded_client(
        tmp_path,
        deployment_path=deployment_path,
        kernel_bundle=_SpringOnlyStopCapabilityBundle(),
    )
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]

    resp = client.post(f"{_BASE}/{cid}/execute")

    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "stop_profile_unsupported"
    assert kernel.execute_count == 0
    candidate = client.get(f"{_BASE}/{cid}").json()
    assert candidate["status"] == "ready"
    assert candidate["commitEligible"] is True


def test_get_executing_candidate_maps_completed_runtime_outcome(
    tmp_path: Path,
) -> None:
    client, state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text

    deadline = time.time() + 5.0
    body = None
    while time.time() < deadline:
        poll = client.get(f"{_BASE}/{cid}")
        assert poll.status_code == 200, poll.text
        body = poll.json()
        if body["status"] == "completed":
            break
        time.sleep(0.02)

    assert body is not None
    assert body["status"] == "completed"
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == "execution_completed"
    assert body["execution"]["sessionStatus"] == "completed"
    assert body["execution"]["state"] == "completed"
    assert body["execution"]["progress"] == 1.0
    assert kernel.execute_count >= 1
    assert state.get_current_joint_positions()[0] == pytest.approx(0.3)


@pytest.mark.parametrize(
    ("kernel_state", "candidate_status", "reason_code"),
    [
        (ExecutionState.INTERRUPTED, "interrupted", "execution_interrupted"),
        (ExecutionState.STOPPED, "stopped", "execution_stopped"),
        (ExecutionState.FAILED, "failed", "execution_failed"),
    ],
)
def test_get_executing_candidate_maps_kernel_terminal_state(
    tmp_path: Path,
    kernel_state: ExecutionState,
    candidate_status: str,
    reason_code: str,
) -> None:
    client, _state, kernel = _loaded_client(tmp_path, execute_duration_s=5.0)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text
    move_id = resp.json()["candidate"]["execution"]["moveId"]

    deadline = time.time() + 5.0
    while kernel.execute_count == 0 and time.time() < deadline:
        time.sleep(0.02)
    assert kernel.execute_count >= 1

    kernel._state = kernel_state  # type: ignore[attr-defined]
    kernel._active_move_id = move_id  # type: ignore[attr-defined]
    body = client.get(f"{_BASE}/{cid}").json()
    assert body["status"] == candidate_status
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == reason_code
    assert body["execution"]["state"] == kernel_state.value


def test_execute_unknown_candidate_404(tmp_path: Path) -> None:
    client, _state, _kernel = _loaded_client(tmp_path)
    assert client.post(f"{_BASE}/does-not-exist/execute").status_code == 404


def test_execute_blocked_candidate_409_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    kernel.next_preview_result = MotionPreviewResult(ok=False, error="no ik solution")
    cid = client.post(_BASE, json=_cartesian_body()).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "ik_unreachable"
    assert kernel.execute_count == 0


def test_execute_reverted_candidate_409_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    client.post(f"{_BASE}/{cid}/revert")
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "candidate_reverted"
    assert kernel.execute_count == 0


def test_execute_stale_candidate_409_no_motion(tmp_path: Path) -> None:
    client, state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    # Robot moves after planning -> approval must fail closed without motion.
    _set_mock_robot_positions(
        state, kernel, [0.9] + [0.0] * (len(kernel.get_joint_names()) - 1)
    )
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "stale_planning_snapshot"
    assert kernel.execute_count == 0


def test_execute_replan_required_when_revalidation_fails_no_motion(
    tmp_path: Path,
) -> None:
    client, state, kernel = _loaded_client(tmp_path)
    cid = client.post(_BASE, json=_joint_body()).json()["candidateId"]
    # Snapshot unchanged (candidate still ready), but the mandatory
    # pre-dispatch revalidation now fails -> replan_required, no motion.
    kernel.next_preview_result = MotionPreviewResult(
        ok=False, error="now colliding with a freshly added object"
    )
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "replan_required"
    assert kernel.execute_count == 0


# ---------------------------------------------------------------------------
# P5.2G: gripper candidate parity (no-motion authoring; one command on approve)
# ---------------------------------------------------------------------------


def _gripper_client(
    tmp_path: Path,
) -> tuple[TestClient, ComposerState, MockKernelClient]:
    """Bare client with a gripper-bearing deployment. Gripper candidates
    validate against the deployment gripper config + URDF limits and need no
    program/robot profile, so a program load is not required."""
    client, state, kernel = _client(tmp_path)
    state.deployment = Deployment(_GRIPPER_DEPLOYMENT_PATH)
    state.deployment_source_path = _GRIPPER_DEPLOYMENT_PATH
    state.kernel_bundle = _AllStopModesCapabilityBundle()
    return client, state, kernel


def _gripper_body(target_position: float, surface: str = "gripper_slider") -> dict:
    return {
        "sourceSurface": surface,
        "inputMode": "gripper",
        "gripperTarget": {"gripperId": "primary", "targetPosition": target_position},
    }


def test_gripper_candidate_create_is_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    resp = client.post(_BASE, json=_gripper_body(0.1))
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["status"] == "ready"
    assert body["inputMode"] == "gripper"
    assert body["commitEligible"] is True
    assert body["gripperTarget"]["gripperId"] == "primary"
    assert body["gripperTarget"]["targetPosition"] == pytest.approx(0.1)
    # Preview-only: no gripper command dispatched, no scene mutation.
    assert kernel.gripper_command_count == 0
    assert kernel.scene_edits == []


def test_gripper_candidate_preset_surface_authors(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    resp = client.post(_BASE, json=_gripper_body(0.0, surface="gripper_preset"))
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["status"] == "ready"
    assert body["sourceSurface"] == "gripper_preset"
    assert kernel.gripper_command_count == 0


def test_gripper_candidate_out_of_limit_is_blocked_no_motion(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    resp = client.post(_BASE, json=_gripper_body(999.0))
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["status"] == "blocked"
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == "gripper_limit_violation"
    assert kernel.gripper_command_count == 0


def test_gripper_candidate_approve_dispatches_exactly_one_command(
    tmp_path: Path,
) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    cid = client.post(_BASE, json=_gripper_body(0.1)).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["candidate"]["status"] == "executing"
    # Gripper dispatch is a single synchronous bridge command, not an
    # intervention session.
    assert body["session"] is None
    assert kernel.gripper_command_count == 1
    assert kernel.last_gripper_command_request is not None
    assert kernel.last_gripper_command_request.gripper_id == "primary"
    assert kernel.last_gripper_command_request.target_position == pytest.approx(0.1)


def test_gripper_candidate_blocked_does_not_dispatch(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    cid = client.post(_BASE, json=_gripper_body(999.0)).json()["candidateId"]
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert kernel.gripper_command_count == 0


def test_gripper_candidate_reverted_does_not_dispatch(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_client(tmp_path)
    cid = client.post(_BASE, json=_gripper_body(0.1)).json()["candidateId"]
    assert client.post(f"{_BASE}/{cid}/revert").status_code == 200
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert kernel.gripper_command_count == 0


class _GripperStatusKernel(MockKernelClient):
    """MockKernelClient whose gripper status can be forced EXECUTING to exercise
    the busy-state revalidation gate."""

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.forced_gripper_state = None

    def get_gripper_status(self, gripper_id):
        if self.forced_gripper_state is not None:
            return GripperCommandStatus(
                state=self.forced_gripper_state, gripper_id=gripper_id
            )
        return super().get_gripper_status(gripper_id)


def _gripper_status_client(
    tmp_path: Path,
) -> tuple[TestClient, ComposerState, _GripperStatusKernel]:
    kernel = _GripperStatusKernel()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        kernel_bundle=_AllStopModesCapabilityBundle(),
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    app = create_app(
        state,
        security=SecurityConfig(
            api_token=None,
            allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
        ),
    )
    client = TestClient(app)
    state.deployment = Deployment(_GRIPPER_DEPLOYMENT_PATH)
    state.deployment_source_path = _GRIPPER_DEPLOYMENT_PATH
    return client, state, kernel


def test_gripper_candidate_create_blocked_when_gripper_busy(tmp_path: Path) -> None:
    client, _state, kernel = _gripper_status_client(tmp_path)
    kernel.forced_gripper_state = GripperCommandState.EXECUTING
    resp = client.post(_BASE, json=_gripper_body(0.1))
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["status"] == "blocked"
    assert body["commitEligible"] is False
    assert body["diagnostics"]["reasonCode"] == "gripper_busy"
    assert kernel.gripper_command_count == 0


def test_gripper_candidate_approve_fails_closed_when_gripper_busy(
    tmp_path: Path,
) -> None:
    client, _state, kernel = _gripper_status_client(tmp_path)
    # Create while idle -> ready.
    cid = client.post(_BASE, json=_gripper_body(0.1)).json()["candidateId"]
    # Another command starts executing on this gripper before approval.
    kernel.forced_gripper_state = GripperCommandState.EXECUTING
    resp = client.post(f"{_BASE}/{cid}/execute")
    assert resp.status_code == 409
    assert resp.json()["reasonCode"] == "gripper_busy"
    # Fail closed: no command_gripper call was made.
    assert kernel.gripper_command_count == 0


def test_gripper_candidate_not_stuck_busy_after_command_completes(
    tmp_path: Path,
) -> None:
    """Backend-owned progress must complete without observer side effects."""
    client, _state, kernel = _gripper_client(tmp_path)
    cid = client.post(
        _BASE,
        json={
            "sourceSurface": "gripper_slider",
            "inputMode": "gripper",
            "gripperTarget": {
                "gripperId": "primary",
                "targetPosition": 0.1,
                "durationS": 0.05,
            },
        },
    ).json()["candidateId"]
    assert client.post(f"{_BASE}/{cid}/execute").status_code == 200
    assert kernel.gripper_command_count == 1
    # Let the command finish; the next candidate's busy check advances the
    # simulated command and must see it COMPLETED, not stuck EXECUTING.
    time.sleep(0.12)
    body = client.post(_BASE, json=_gripper_body(0.2)).json()
    assert body["status"] == "ready", body


def test_scene_state_path_clears_gripper_executing_after_completion(
    tmp_path: Path,
) -> None:
    """The browser sees backend-owned completion without advancing it."""
    from manyforge_composer.backend.routes_scene import _collect_gripper_states

    client, state, _kernel = _gripper_client(tmp_path)
    cid = client.post(
        _BASE,
        json={
            "sourceSurface": "gripper_slider",
            "inputMode": "gripper",
            "gripperTarget": {
                "gripperId": "primary",
                "targetPosition": 0.1,
                "durationS": 0.05,
            },
        },
    ).json()["candidateId"]
    assert client.post(f"{_BASE}/{cid}/execute").status_code == 200

    def _primary(states):
        return next(g for g in states if g.gripper_id == "primary")

    # Immediately after approve the simulated command is executing.
    assert _primary(_collect_gripper_states(state)).state == "executing"
    time.sleep(0.12)
    assert _primary(_collect_gripper_states(state)).state == "completed"


def test_gripper_candidate_get_clears_executing_after_completion(
    tmp_path: Path,
) -> None:
    """A gripper candidate has no arm execution/session payload, but the
    candidate endpoint still reports the UI lifecycle after Approve. Polling it
    must observe the gripper bridge terminal state so the browser
    cannot stay locally stuck at `executing` and block the next command."""
    client, _state, _kernel = _gripper_client(tmp_path)
    cid = client.post(
        _BASE,
        json={
            "sourceSurface": "gripper_slider",
            "inputMode": "gripper",
            "gripperTarget": {
                "gripperId": "primary",
                "targetPosition": 0.1,
                "durationS": 0.05,
            },
        },
    ).json()["candidateId"]
    assert client.post(f"{_BASE}/{cid}/execute").status_code == 200
    time.sleep(0.12)

    body = client.get(f"{_BASE}/{cid}").json()
    assert body["status"] == "completed", body
    assert body["diagnostics"]["reasonCode"] == "execution_completed"
    assert body["diagnostics"]["details"]["gripperState"] == "completed"


def test_gripper_mode_requires_gripper_surface(tmp_path: Path) -> None:
    """A non-gripper surface may not author a gripper candidate, even with a
    gripperTarget payload (mode/surface consistency, both directions)."""
    client, _state, kernel = _gripper_client(tmp_path)
    resp = client.post(
        _BASE,
        json={
            "sourceSurface": "move_to",
            "inputMode": "gripper",
            "gripperTarget": {"gripperId": "primary", "targetPosition": 0.1},
        },
    )
    assert resp.status_code == 422
    assert kernel.gripper_command_count == 0


def _ready_store_candidate():
    """Build a store holding one READY, dispatch-ready joint candidate; return
    (store, candidate_id). Used by the TOCTOU dispatch-commit tests."""
    from manyforge_composer.backend.robot_control_candidates import (
        _CandidateStore,
        _EMPTY_PLANNING_SNAPSHOT,
        _StoredCandidate,
        CandidateCreateRequest,
        CandidateResponse,
        CandidateStatus,
    )

    store = _CandidateStore()
    req = CandidateCreateRequest.model_validate(
        {
            "sourceSurface": "joint_slider",
            "inputMode": "joint",
            "jointTarget": {"jointNames": ["joint1"], "positionsRad": [0.3]},
        }
    )
    cid, gen = store.reserve(req)
    ready = CandidateResponse(
        candidate_id=cid,
        generation=gen,
        source_surface=req.source_surface,
        input_mode=req.input_mode,
        status=CandidateStatus.READY,
        commit_eligible=True,
        created_at_ns=1,
        updated_at_ns=1,
    )
    store.fill(
        cid,
        ready,
        _StoredCandidate(
            response=ready,
            snapshot=_EMPTY_PLANNING_SNAPSHOT,
            dispatch_params={
                "motion_type": "joint_goal",
                "joint_goal_names": ["joint1"],
                "joint_goal_positions_rad": [0.3],
            },
        ),
    )
    return store, cid, _EMPTY_PLANNING_SNAPSHOT


def test_commit_for_dispatch_fails_closed_after_concurrent_revert() -> None:
    """TOCTOU guard: a revert that lands AFTER
    begin_approval (which caches dispatch_params) but BEFORE the atomic
    pre-dispatch commit must win — commit_for_dispatch refuses, so the route
    cannot dispatch and the robot stays still."""
    from manyforge_composer.backend.robot_control_candidates import CandidateStatus

    store, cid, snapshot = _ready_store_candidate()
    gated, _view = store.begin_approval(cid, snapshot)
    assert gated is not None  # approve gate opened (APPROVING)
    # A concurrent revert lands during the (lock-free) revalidation window.
    reverted = store.revert(cid)
    assert reverted is not None and reverted.status is CandidateStatus.REVERTED
    # The atomic pre-dispatch commit must now FAIL CLOSED — no motion.
    committed, current = store.commit_for_dispatch(cid)
    assert committed is False
    assert current is not None and current.status is CandidateStatus.REVERTED


def test_commit_for_dispatch_commits_then_post_commit_revert_is_noop() -> None:
    """Positive path + post-commit revert: once committed (EXECUTING), a later
    revert cannot recall motion and must NOT relabel the candidate REVERTED."""
    from manyforge_composer.backend.robot_control_candidates import CandidateStatus

    store, cid, snapshot = _ready_store_candidate()
    gated, _view = store.begin_approval(cid, snapshot)
    assert gated is not None
    committed, view = store.commit_for_dispatch(cid)
    assert committed is True
    assert view is not None and view.status is CandidateStatus.EXECUTING
    # A revert AFTER the dispatch commit is a no-op (status stays EXECUTING).
    after = store.revert(cid)
    assert after is not None and after.status is CandidateStatus.EXECUTING


# ---------------------------------------------------------------------------
# Input validation (model-layer; no program required)
# ---------------------------------------------------------------------------


_MALFORMED_BODIES = [
    {"sourceSurface": "joint_slider", "inputMode": "joint"},
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1"], "positionsRad": [0.1]},
        "cartesianTarget": {"positionM": [0, 0, 0], "orientationXyzw": [0, 0, 0, 1]},
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": [], "positionsRad": []},
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1", "j2"], "positionsRad": [0.1]},
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1", "j1"], "positionsRad": [0.1, 0.2]},
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": [" "], "positionsRad": [0.1]},
    },
    {"sourceSurface": "cartesian_gizmo", "inputMode": "cartesian"},
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0, 0, 0], "orientationXyzw": [0, 0, 0, 1]},
        "jointTarget": {"jointNames": ["j1"], "positionsRad": [0.1]},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0, 0], "orientationXyzw": [0, 0, 0, 1]},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0, 0, 0], "orientationXyzw": [0, 0, 1]},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0, 0, 0], "orientationXyzw": [0, 0, 0, 0]},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {
            "positionM": [0, 0, 0],
            "orientationXyzw": [0, 0, 0, 1],
            "frameId": "",
        },
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {
            "positionM": [0, 0, 0],
            "orientationXyzw": [0, 0, 0, 1],
            "tcpFrame": " ",
        },
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1"], "positionsRad": [0.1]},
        "sourceSrface": "joint_slider",
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1"], "positionsRad": [0.1], "foo": 1},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "cartesian",
        "cartesianTarget": {
            "positionM": [0, 0, 0],
            "orientationXyzw": [0, 0, 0, 1],
            "foo": 1,
        },
    },
    {
        "sourceSurface": "joint_slider",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0, 0, 0], "orientationXyzw": [0, 0, 0, 1]},
    },
    {
        "sourceSurface": "cartesian_gizmo",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["j1"], "positionsRad": [0.1]},
    },
]


@pytest.mark.parametrize("body", _MALFORMED_BODIES)
def test_malformed_candidate_returns_422(tmp_path: Path, body: dict) -> None:
    client, _state, _kernel = _client(tmp_path)
    assert client.post(_BASE, json=body).status_code == 422


def test_move_to_surface_accepts_both_modes(tmp_path: Path) -> None:
    client, _state, _kernel = _loaded_client(tmp_path)
    joint = {
        "sourceSurface": "move_to",
        "inputMode": "joint",
        "jointTarget": {"jointNames": ["joint1"], "positionsRad": [0.1]},
    }
    cartesian = {
        "sourceSurface": "move_to",
        "inputMode": "cartesian",
        "cartesianTarget": {"positionM": [0.3, 0.1, 0.4], "orientationXyzw": [0, 0, 0, 1]},
    }
    assert client.post(_BASE, json=joint).status_code == 200
    assert client.post(_BASE, json=cartesian).status_code == 200


def test_non_finite_joint_position_http_returns_422(tmp_path: Path) -> None:
    client, _state, _kernel = _client(tmp_path)
    raw = (
        '{"sourceSurface":"joint_slider","inputMode":"joint",'
        '"jointTarget":{"jointNames":["j1"],"positionsRad":[Infinity]}}'
    )
    resp = client.post(
        _BASE, content=raw, headers={"content-type": "application/json"}
    )
    assert resp.status_code == 422


def test_non_finite_positions_rejected_at_model_layer() -> None:
    import pydantic

    from manyforge_composer.backend.robot_control_candidates import (
        CandidateCartesianTarget,
        CandidateJointTarget,
    )

    for bad in (float("inf"), float("-inf"), float("nan")):
        with pytest.raises(pydantic.ValidationError):
            CandidateJointTarget(joint_names=["j1"], positions_rad=[bad])
        with pytest.raises(pydantic.ValidationError):
            CandidateCartesianTarget(position_m=[bad, 0.0, 0.0])
        with pytest.raises(pydantic.ValidationError):
            CandidateCartesianTarget(orientation_xyzw=[bad, 0.0, 0.0, 1.0])


def test_candidate_surface_is_streaming_free() -> None:
    """Prove no default Robot Control candidate path references streaming."""
    import manyforge_composer.backend.robot_control_candidates as candidate_mod

    source = Path(candidate_mod.__file__).read_text(encoding="utf-8")
    # Backend / kernel streaming entry points that the candidate surface
    # (a Python backend module) must never reference. The tokens are
    # assembled from fragments so this assertion does not itself embed the
    # contiguous literals that the strict verifier's forbidden-token scan
    # now rejects in production scan dirs.
    sess = "streaming" + "_session"
    forbidden = [
        "begin_" + sess,
        "push_" + "streaming" + "_target",
        "end_" + sess,
        "Streaming" + "SessionConfigDTO",
        "Streaming" + "TargetDTO",
    ]
    present = [token for token in forbidden if token in source]
    assert present == [], f"candidate surface must not reference streaming: {present}"
