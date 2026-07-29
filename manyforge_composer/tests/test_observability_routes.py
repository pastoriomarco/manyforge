"""Tests for kernel/scene observability routes."""

from __future__ import annotations

from manyforge_composer.tests.node_environment_support import node_environment_for_catalog

from concurrent.futures import ThreadPoolExecutor
import importlib.util
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock
import math
import sys
import threading
import time

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import (
    ExecutionSnapshot,
    GripperCommandState,
    GripperCommandStatus,
    MockKernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend import routes_infra, routes_runtime, routes_scene, routes_urdf
from manyforge_composer.backend.models import (
    ViewerFrameV1,
    ViewerManifestV1,
    VisualSceneSnapshotResponse,
)
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_DEPLOYMENT_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
_PINOCCHIO_AVAILABLE = importlib.util.find_spec("pinocchio") is not None
_REQUIRES_PINOCCHIO_VISUAL_FK = pytest.mark.skipif(
    not _PINOCCHIO_AVAILABLE,
    reason="independent visual-snapshot FK requires manyforge_composer[visual]",
)


def make_client(*, joint_names: list[str] | None = None) -> tuple[TestClient, ComposerState, MockKernelClient]:
    kernel = MockKernelClient(joint_names=joint_names)
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    app = create_app(state)
    return TestClient(app), state, kernel


def _publish_visual_snapshot(state: ComposerState) -> dict:
    return state.publish_visual_state_snapshot_sync(
        lambda: routes_scene._build_visual_state_snapshot_for_publisher(state),
        reason="test",
    )


def _make_hermetic_visual_builder_state(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> ComposerState:
    """Build a real viewer-worker input without an installed RoboPlan model."""
    urdf_path = _write_toy_visual_urdf(tmp_path)
    entry = SimpleNamespace(
        urdf_path=str(urdf_path),
        links=tuple(
            SimpleNamespace(name=name)
            for name in ("base_link", "link_1", "finger_link")
        ),
    )
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(
        routes_urdf,
        "active_urdf_model_key",
        lambda _state: "toy-wire-contract-key",
    )
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1", "finger_link"),
    )

    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(joint_names=["joint_a"]),
    )
    state.program = SimpleNamespace(
        robot=SimpleNamespace(
            base_frame="base_link",
            tcp_frame="link_1",
            base_pose={},
        ),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    return state


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_real_viewer_builder_matches_the_generated_strict_wire_contract(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    state = _make_hermetic_visual_builder_state(monkeypatch, tmp_path)

    result = _publish_visual_snapshot(state)
    assert result["success"] is True, result
    publication = state.last_viewer_publication()
    assert publication is not None

    manifest = ViewerManifestV1.model_validate(publication.manifest)
    ViewerFrameV1.model_validate(publication.frame)
    # The v1 manifest and the low-rate /scene/frames projection share the same
    # authority: semantic frames are visible by default; structural URDF link
    # frames are opt-in. The Step-15 migration briefly defaulted the manifest's
    # incomplete visibility map to True and exposed every gripper link axis.
    assert any(frame.roles for frame in manifest.frames)
    assert any(not frame.roles for frame in manifest.frames)
    assert all(frame.viewer_visible == bool(frame.roles) for frame in manifest.frames)

    structural_frame = next(frame for frame in manifest.frames if not frame.roles)
    semantic_frame = next(frame for frame in manifest.frames if frame.roles)
    state.set_frame_visibility(structural_frame.name, True)
    state.set_frame_visibility(semantic_frame.name, False)
    override_result = _publish_visual_snapshot(state)
    assert override_result["success"] is True, override_result
    override_publication = state.last_viewer_publication()
    assert override_publication is not None
    overridden = ViewerManifestV1.model_validate(override_publication.manifest)
    overridden_by_name = {frame.name: frame for frame in overridden.frames}
    assert overridden_by_name[structural_frame.name].viewer_visible is True
    assert overridden_by_name[semantic_frame.name].viewer_visible is False

    # Both frontend control surfaces consume /scene/frames while the renderer
    # consumes the v1 manifest. They must remain two projections of one
    # ComposerState authority, including explicit overrides and gizmo size.
    frame_projection = routes_scene.build_scene_frames_payload(state)
    projected_by_name = {
        frame["name"]: frame for frame in frame_projection["frames"]
    }
    assert set(projected_by_name) == set(overridden_by_name)
    assert all(
        projected_by_name[name]["viewerVisible"]
        == overridden_by_name[name].viewer_visible
        for name in projected_by_name
    )
    assert frame_projection["frameGizmoSizeM"] == overridden.frame_axes_gizmo_size_m

    assert state.viewer_data_plane_status()["render_revision"] == override_publication.frame[
        "render_revision"
    ]


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_viewer_builder_resolves_static_gripper_metadata_only_for_changed_inputs(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    state = _make_hermetic_visual_builder_state(monkeypatch, tmp_path)

    real_states = routes_scene._collect_gripper_states
    synthetic_revision = 0
    pose_source_revision = 0
    state_builds = 0

    def statuses_with_test_revision(_state_arg: ComposerState):
        return {}, (("test_revision", synthetic_revision),)

    def counted_states(*args, **kwargs):
        nonlocal state_builds
        state_builds += 1
        return real_states(*args, **kwargs)

    monkeypatch.setattr(
        routes_scene,
        "_collect_gripper_statuses",
        statuses_with_test_revision,
    )
    monkeypatch.setattr(routes_scene, "_collect_gripper_states", counted_states)
    monkeypatch.setattr(state, "viewer_source_revisions", lambda: (1, 1, 0, 0, 0))
    monkeypatch.setattr(
        state,
        "pose_source_snapshots_with_revision",
        lambda: ({}, pose_source_revision),
    )
    monkeypatch.setattr(
        state,
        "visual_scene_objects_input_snapshot",
        lambda: ([], {}, {"revision": 0, "timestamp_ns": 1}),
    )
    monkeypatch.setattr(state, "scene_viewer_revision", lambda: 0)

    first_build = routes_scene._build_visual_state_snapshot_for_publisher(state)
    state.commit_visual_input_signature(first_build.input_signature)
    stable_state_builds = state_builds

    with pytest.raises(routes_scene.ViewerInputUnchanged):
        routes_scene._build_visual_state_snapshot_for_publisher(state)
    assert state_builds == stable_state_builds

    # A transport-only source revision cannot force FK/encoding when its
    # render-significant content is unchanged.
    pose_source_revision = 1
    with pytest.raises(routes_scene.ViewerInputUnchanged):
        routes_scene._build_visual_state_snapshot_for_publisher(state)
    assert state_builds == stable_state_builds

    synthetic_revision = 1
    routes_scene._build_visual_state_snapshot_for_publisher(state)
    assert state_builds == stable_state_builds + 1


def test_visual_publisher_performs_no_build_without_demand_and_one_shot_is_exact(
) -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    built = threading.Event()
    build_count = 0

    def builder() -> VisualSceneSnapshotResponse:
        nonlocal build_count
        build_count += 1
        built.set()
        return VisualSceneSnapshotResponse(
            sequence_number=build_count,
            timestamp_ns=time.monotonic_ns(),
        )

    try:
        state.ensure_visual_state_publisher(builder, period_s=0.02)
        state.request_visual_state_publish(reason="no_demand")
        assert built.wait(timeout=0.05) is False

        generation = state.request_visual_state_one_shot(reason="test_one_shot")
        assert built.wait(timeout=1.0) is True
        result = state.wait_for_visual_state_one_shot(
            generation=generation,
            timeout_s=1.0,
        )
        assert result is not None
        assert result["success"] is True
        assert result["sequence_number"] == 1
        assert build_count == 1
    finally:
        state.stop_visual_state_publisher(timeout_s=1.0)


def test_unchanged_observer_sample_clears_transient_producer_error() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    state.record_visual_state_publish_error(
        duration_ms=1.0,
        error="execution_snapshot_unavailable",
        reason="test",
    )

    def unchanged_sample() -> None:
        raise routes_scene.ViewerInputUnchanged()

    result = state.publish_visual_state_snapshot_sync(
        unchanged_sample,
        reason="unchanged_after_recovery",
    )

    assert result["unchanged"] is True
    _snapshot, metadata = state.visual_state_snapshot()
    assert metadata["last_error"] is None


def test_visual_publisher_coalesces_dirty_wakes_under_granted_rate() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    built = threading.Event()
    build_count = 0

    def builder() -> VisualSceneSnapshotResponse:
        nonlocal build_count
        build_count += 1
        built.set()
        return VisualSceneSnapshotResponse(
            sequence_number=build_count,
            timestamp_ns=time.monotonic_ns(),
        )

    try:
        state.ensure_visual_state_publisher(builder, period_s=0.02)
        state.attach_viewer_demand(
            "rate-limited-client",
            visibility="focused",
            requested_rate_hz=1.0,
            layers=("canonical",),
        )
        assert built.wait(timeout=1.0) is True
        assert build_count == 1

        built.clear()
        for index in range(10):
            state.request_visual_state_publish(reason=f"dirty_{index}")
            time.sleep(0.01)

        assert built.wait(timeout=0.2) is False
        assert build_count == 1
        assert built.wait(timeout=1.2) is True
        assert build_count == 2
    finally:
        state.stop_visual_state_publisher(timeout_s=1.0)


def test_visual_publisher_cannot_lose_demand_attached_during_summary(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    sampled_inactive = threading.Event()
    release_inactive_sample = threading.Event()
    built = threading.Event()
    original_summary = state._viewer_demand.summary

    def gated_summary():
        result = original_summary()
        if not result.active and not sampled_inactive.is_set():
            sampled_inactive.set()
            assert release_inactive_sample.wait(timeout=1.0)
        return result

    monkeypatch.setattr(state._viewer_demand, "summary", gated_summary)

    def build_visual_snapshot() -> VisualSceneSnapshotResponse:
        built.set()
        return VisualSceneSnapshotResponse(
            sequence_number=1,
            timestamp_ns=time.monotonic_ns(),
        )

    try:
        state.ensure_visual_state_publisher(
            build_visual_snapshot,
            period_s=0.02,
        )
        assert sampled_inactive.wait(timeout=0.5) is True

        state.attach_viewer_demand(
            "lost-wake-client",
            visibility="focused",
            requested_rate_hz=1.0,
            layers=("canonical",),
        )
        release_inactive_sample.set()

        assert built.wait(timeout=0.5) is True
    finally:
        release_inactive_sample.set()
        state.stop_visual_state_publisher(timeout_s=1.0)


def test_viewer_model_key_cache_is_generation_fenced_and_bounded() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    calls = 0

    def resolve() -> str:
        nonlocal calls
        calls += 1
        return f"model-{calls}"

    assert state.resolve_viewer_model_key(
        generation=(1, 1),
        resolver=resolve,
    ) == "model-1"
    assert state.resolve_viewer_model_key(
        generation=(1, 1),
        resolver=resolve,
    ) == "model-1"
    assert calls == 1

    assert state.resolve_viewer_model_key(
        generation=(1, 2),
        resolver=resolve,
    ) == "model-2"
    assert state.resolve_viewer_model_key(
        generation=(1, 2),
        resolver=resolve,
        revalidate_after_s=0.0,
    ) == "model-3"


def test_viewer_static_input_cache_reuses_only_the_exact_revision_key() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    calls = 0

    def resolve() -> dict[str, int]:
        nonlocal calls
        calls += 1
        return {"generation": calls}

    first = state.resolve_viewer_static_input(key=(1, "model"), resolver=resolve)
    repeated = state.resolve_viewer_static_input(
        key=(1, "model"),
        resolver=resolve,
    )
    changed = state.resolve_viewer_static_input(key=(2, "model"), resolver=resolve)

    assert repeated is first
    assert first == {"generation": 1}
    assert changed == {"generation": 2}
    assert calls == 2


def test_visual_worker_failure_is_contained_and_last_good_snapshot_survives() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    previous = VisualSceneSnapshotResponse(sequence_number=1, timestamp_ns=1)
    replacement = VisualSceneSnapshotResponse(sequence_number=2, timestamp_ns=2)
    state.update_visual_state_snapshot(previous, reason="seed")

    def fail_worker():
        raise routes_scene.VisualSnapshotUnavailable(
            "viewer_worker_build_failed",
            "synthetic worker failure",
        )

    result = state.publish_visual_state_snapshot_sync(
        fail_worker,
        reason="worker_failure",
    )

    assert result["success"] is False
    assert result["error"] == "viewer_worker_build_failed"
    snapshot, metadata = state.visual_state_snapshot()
    assert snapshot is previous
    assert metadata["last_error"] == "viewer_worker_build_failed"

    recovered = state.publish_visual_state_snapshot_sync(
        lambda: replacement,
        reason="worker_recovered",
    )
    assert recovered["success"] is True
    assert state.visual_state_snapshot()[0] is replacement


def test_native_viewer_heartbeat_degrades_dead_worker_only_while_demanded(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    monkeypatch.setattr(
        state._viewer_worker,
        "status",
        lambda: {
            "alive": False,
            "worker_epoch": "failed-epoch",
            "pid": None,
            "restarts": 1,
            "last_error": "viewer_worker_exited",
            "worker_policy": {"policy_ok": False},
        },
    )

    undemanded = routes_infra.build_infra_status_payload(state)
    assert undemanded["components"]["native_viewer"]["status"] == "waiting"

    state.attach_viewer_demand(
        "heartbeat-test",
        visibility="focused",
        requested_rate_hz=15.0,
        layers=["canonical"],
    )
    demanded = routes_infra.build_infra_status_payload(state)
    component = demanded["components"]["native_viewer"]
    assert component["status"] == "degraded"
    assert component["worker_last_error"] == "viewer_worker_exited"
    assert component["worker_policy_ok"] is False


def test_runtime_and_infra_projections_share_one_material_heartbeat_sample() -> None:
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )

    first = routes_infra.build_infra_heartbeat_payload(state)
    routes_runtime.build_runtime_status_payload(state)
    initial_cache = state.heartbeat_cache_status()

    assert initial_cache["misses"] == 1
    assert initial_cache["hits"] >= 1
    assert routes_infra.build_infra_heartbeat_payload(state) is first

    state.update_external_runtime_status("test-provider", {"status": "ready"})
    refreshed = routes_infra.build_infra_heartbeat_payload(state)
    refreshed_cache = state.heartbeat_cache_status()
    assert refreshed is not first
    assert refreshed_cache["misses"] == 2


def _load_program_for_scene_sync() -> tuple[TestClient, ComposerState, MockKernelClient]:
    client, state, kernel = make_client()
    state.load_program(_PROGRAM_PATH)
    return client, state, kernel


def _write_toy_visual_urdf(tmp_path: Path) -> Path:
    urdf_path = tmp_path / "toy.urdf"
    urdf_path.write_text(
        """<?xml version="1.0"?>
<robot name="toy">
  <link name="base_link">
    <visual><geometry><box size="0.1 0.1 0.1"/></geometry></visual>
  </link>
  <joint name="joint_a" type="revolute">
    <parent link="base_link"/>
    <child link="link_1"/>
    <origin xyz="1 0 0" rpy="0 0 0"/>
    <axis xyz="0 0 1"/>
    <limit lower="-3.14" upper="3.14" effort="1" velocity="1"/>
  </joint>
  <link name="link_1">
    <visual><geometry><box size="0.1 0.1 0.1"/></geometry></visual>
  </link>
  <joint name="finger_joint" type="prismatic">
    <parent link="link_1"/>
    <child link="finger_link"/>
    <origin xyz="0 0 0" rpy="0 0 0"/>
    <axis xyz="1 0 0"/>
    <limit lower="0" upper="1" effort="1" velocity="1"/>
  </joint>
  <link name="finger_link">
    <visual><geometry><box size="0.05 0.05 0.05"/></geometry></visual>
  </link>
</robot>
""",
        encoding="utf-8",
    )
    return urdf_path


def _write_sibling_mimic_visual_urdfs(tmp_path: Path) -> tuple[Path, Path]:
    canonical_path = tmp_path / "sibling_mimic_canonical.urdf"
    view_path = tmp_path / "sibling_mimic_view.urdf"
    canonical_path.write_text(
        """<?xml version="1.0"?>
<robot name="sibling_mimic_canonical">
  <link name="base_link"/>
  <joint name="right_finger_joint" type="prismatic">
    <parent link="base_link"/>
    <child link="right_link"/>
    <axis xyz="1 0 0"/>
    <limit lower="0" upper="1" effort="1" velocity="1"/>
  </joint>
  <link name="right_link"/>
  <joint name="left_finger_joint" type="prismatic">
    <parent link="base_link"/>
    <child link="left_link"/>
    <origin xyz="0 1 0" rpy="0 0 0"/>
    <axis xyz="1 0 0"/>
    <limit lower="0" upper="1" effort="1" velocity="1"/>
    <mimic joint="right_finger_joint" multiplier="1" offset="0"/>
  </joint>
  <link name="left_link"/>
</robot>
""",
        encoding="utf-8",
    )
    view_path.write_text(
        """<?xml version="1.0"?>
<robot name="sibling_mimic_view">
  <link name="base_link"/>
  <joint name="__rma_mimic_0000_right_finger_joint" type="prismatic">
    <parent link="base_link"/>
    <child link="right_link"/>
    <axis xyz="1 0 0"/>
    <limit lower="0" upper="1" effort="1" velocity="1"/>
  </joint>
  <link name="right_link"/>
  <joint name="left_finger_joint" type="prismatic">
    <parent link="base_link"/>
    <child link="left_link"/>
    <origin xyz="0 1 0" rpy="0 0 0"/>
    <axis xyz="1 0 0"/>
    <limit lower="0" upper="1" effort="1" velocity="1"/>
    <mimic joint="__rma_mimic_0000_right_finger_joint" multiplier="1" offset="0"/>
  </joint>
  <link name="left_link"/>
</robot>
""",
        encoding="utf-8",
    )
    return canonical_path, view_path


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


def test_scene_state_reports_visual_snapshot_pending_without_live_fallback():
    client, _state, _kernel = make_client()

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    data = response.json()

    assert data["visual_state"] is None
    diagnostics = data["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["served_from_visual_snapshot"] is False
    assert diagnostics["fallback_reason"] in {
        "visual_model_unavailable",
        "visual_snapshot_publisher_unavailable",
        "visual_snapshot_pending",
    }
    if diagnostics["fallback_reason"] == "visual_model_unavailable":
        assert diagnostics["snapshot_publisher_error"] == "visual_model_unavailable"
    assert "snapshot_read" in diagnostics["timings_ms"]


def test_scene_state_pending_robot_includes_authority_metadata():
    class AuthorityKernel(MockKernelClient):
        def get_execution_snapshot(self):
            return ExecutionSnapshot(
                joint_positions_rad=[0.25],
                state_source="commanded_sim_predicted",
                source_timestamp_ns=123_456,
                source_age_ns=789,
            )

    kernel = AuthorityKernel(joint_names=["joint_a"])
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.ensure_visual_state_publisher = lambda builder, period_s: None

    payload = routes_scene.build_scene_state_payload(state)

    assert payload["visual_state_diagnostics"]["mode"] == "visual_snapshot_pending"
    assert payload["robot"]["joint_positions"] == {"joint_a": 0.25}
    assert payload["robot"]["state_source"] == "commanded_sim_predicted"
    assert payload["robot"]["source_timestamp_ns"] == 123_456
    assert payload["robot"]["source_age_ns"] == 789


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_scene_state_visual_snapshot_fk_uses_independent_model_when_enabled(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Snapshot FK must not call the kernel's legacy planning-scene FK path."""

    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(
                joint_positions_rad=[math.pi / 2.0],
                state_source="commanded_sim_predicted",
                source_timestamp_ns=123_000_000,
                source_age_ns=4_000_000,
            )

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("visual snapshot must not use legacy kernel FK")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("visual snapshot must not use single-link FK")

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-key")
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1"),
    )
    kernel = VisualKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="link_1", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    result = _publish_visual_snapshot(state)
    assert result["success"] is True, result

    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot"
    assert diagnostics["served_from_visual_snapshot"] is True
    assert diagnostics["fallback_reason"] is None
    assert payload["visual_state"] is not None
    assert payload["visual_state"]["sequence_number"] > 0
    assert payload["visual_state"]["model_key"] == "toy-key"
    assert payload["robot"]["tcp_pose_in_base"]["position_m"] == pytest.approx(
        [1.0, 0.0, 0.0],
        abs=1e-6,
    )
    assert payload["robot"]["state_source"] == "commanded_sim_predicted"
    assert payload["robot"]["source_timestamp_ns"] == 123_000_000
    assert payload["robot"]["source_age_ns"] == 4_000_000
    link_pose = payload["link_poses"]["link_1"]
    assert link_pose["position_m"] == pytest.approx([1.0, 0.0, 0.0])
    assert link_pose["orientation_xyzw"][2] == pytest.approx(math.sqrt(0.5), abs=1e-6)
    assert link_pose["orientation_xyzw"][3] == pytest.approx(math.sqrt(0.5), abs=1e-6)


def test_scene_state_visual_snapshot_reports_missing_pinocchio(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Missing visual FK dependencies must fail closed with stable diagnostics."""

    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[0.0])

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("visual snapshot must not fall back to legacy FK")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("visual snapshot must not fall back to single-link FK")

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(
        routes_urdf,
        "active_urdf_model_key",
        lambda _state: "toy-missing-pinocchio-key",
    )
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1"),
    )
    kernel = VisualKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="link_1", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )

    def unavailable_worker(*_args, **_kwargs):
        raise routes_scene.VisualSnapshotUnavailable(
            "pinocchio_unavailable",
            "No module named 'pinocchio' in visual worker",
        )

    monkeypatch.setattr(state, "build_visual_state_in_worker", unavailable_worker)

    result = _publish_visual_snapshot(state)
    assert result["success"] is False
    assert result["error"] == "pinocchio_unavailable"
    assert "pinocchio" in result["error_detail"]

    state.ensure_visual_state_publisher = lambda builder, period_s: None
    state.request_visual_state_publish = lambda *, reason="": None
    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["served_from_visual_snapshot"] is False
    assert diagnostics["fallback_reason"] == "pinocchio_unavailable"
    assert diagnostics["snapshot_publisher_error"] == "pinocchio_unavailable"
    assert "pinocchio" in diagnostics["snapshot_publisher_error_detail"]
    assert payload["visual_state"] is None
    assert payload["link_poses"] == {}


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_visual_state_publisher_uses_cached_scene_objects_without_backend_scene_read(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualObjectKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[0.0])

        def get_scene_objects(self):
            raise AssertionError("visual publisher must not poll mutable scene objects")

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-objects-key")
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1"),
    )
    kernel = VisualObjectKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="link_1", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="cached_box",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.2, 0.3]),
                pose_in_universe={
                    "position_m": [0.4, 0.5, 0.6],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=False,
                frame_id="universe",
            )
        ]
    )

    result = _publish_visual_snapshot(state)

    assert result["success"] is True, result
    snapshot, _meta = state.visual_state_snapshot()
    assert snapshot is not None
    assert [obj.object_id for obj in snapshot.objects] == ["cached_box"]


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_visual_state_publisher_uses_cached_joint_names(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Visual publishing must not poll RoboPlan group metadata every frame.

    The native get_joint_names() path reads the planner scene and can queue
    behind trajectory collision validation. Runtime setup caches the stable
    drivable joint order; the background visual publisher should consume that
    cache plus execution snapshots.
    """

    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[0.0])

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-joints-key")
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1"),
    )
    kernel = VisualKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="link_1", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )

    def blocked_get_joint_names():
        raise AssertionError("visual publisher must use cached joint names")

    monkeypatch.setattr(kernel, "get_joint_names", blocked_get_joint_names)

    result = _publish_visual_snapshot(state)

    assert result["success"] is True, result
    snapshot, _meta = state.visual_state_snapshot()
    assert snapshot is not None
    assert snapshot.robot is not None
    assert snapshot.robot.joint_names == ["joint_a"]


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_scene_state_visual_snapshot_uses_rma_private_view_for_sibling_mimic(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Visual FK uses the RMA view while public joint names stay canonical."""

    canonical_path, view_path = _write_sibling_mimic_visual_urdfs(tmp_path)

    class VisualMimicKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["right_finger_joint"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[0.25])

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("visual snapshot must not use legacy kernel FK")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("visual snapshot must not use single-link FK")

    entry = SimpleNamespace(urdf_path=str(canonical_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "rma-view-key")
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "right_link", "left_link"),
    )
    monkeypatch.setattr(
        routes_urdf,
        "robot_model_authority_metadata",
        lambda _state: {
            "view_changed": True,
            "view_urdf_path": str(view_path),
            "view_to_canonical_joint_names": {
                "__rma_mimic_0000_right_finger_joint": "right_finger_joint",
            },
            "canonical_to_view_joint_names": {
                "right_finger_joint": "__rma_mimic_0000_right_finger_joint",
            },
        },
    )
    kernel = VisualMimicKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="right_link", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="sibling_mimic", group_id="lite6"),
        adapters={},
        grippers={},
    )

    result = _publish_visual_snapshot(state)
    assert result["success"] is True, result

    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot"
    assert diagnostics["served_from_visual_snapshot"] is True
    assert payload["visual_state"]["model_key"] == "rma-view-key"
    assert payload["robot"]["joint_names"] == ["right_finger_joint"]
    assert payload["link_poses"]["right_link"]["position_m"] == pytest.approx(
        [0.25, 0.0, 0.0],
        abs=1e-6,
    )
    assert payload["link_poses"]["left_link"]["position_m"] == pytest.approx(
        [0.25, 1.0, 0.0],
        abs=1e-6,
    )


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_scene_state_visual_snapshot_includes_gripper_feedback_in_fk(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Actuated gripper joints are observer FK inputs, not arm execution joints."""

    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualGripperKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[0.0])

        def get_gripper_status(self, gripper_id: str):
            assert gripper_id == "primary"
            return GripperCommandStatus(
                state=GripperCommandState.COMPLETED,
                gripper_id=gripper_id,
                current_position=0.25,
                target_position=0.25,
            )

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("visual snapshot must not use legacy kernel FK")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("visual snapshot must not use single-link FK")

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-gripper-key")
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1", "finger_link"),
    )
    kernel = VisualGripperKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="finger_link", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={
            "primary": SimpleNamespace(
                joint_name="finger_joint",
                mount_frame="link_1",
                tcp_frame="finger_link",
                motion_time_s=0.8,
                srdf_presets={},
            )
        },
    )
    result = _publish_visual_snapshot(state)
    assert result["success"] is True, result

    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot"
    assert diagnostics["served_from_visual_snapshot"] is True
    assert payload["robot"]["joint_names"] == ["joint_a"]
    assert payload["robot"]["joint_positions"] == {"joint_a": 0.0}
    assert payload["grippers"][0]["joint_name"] == "finger_joint"
    assert payload["grippers"][0]["current_position"] == pytest.approx(0.25)
    assert payload["link_poses"]["finger_link"]["position_m"] == pytest.approx(
        [1.25, 0.0, 0.0],
        abs=1e-6,
    )
    assert payload["robot"]["tcp_pose_in_base"]["position_m"] == pytest.approx(
        [1.25, 0.0, 0.0],
        abs=1e-6,
    )


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_preview_link_poses_joint_falls_back_to_visual_fk_for_gripper_links(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    """Gripper ghosts request visual links outside the arm planning group.

    Native planner FK may report a reachable joint vector but no poses for
    those visual-only gripper links. The route must fall back to the full URDF
    visual FK model instead of returning an empty `link_poses` map.
    """
    urdf_path = _write_toy_visual_urdf(tmp_path)

    class EmptyNativeFkKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def compute_target_link_poses(self, request):
            return {
                "reachable": True,
                "joint_names": list(request["joint_names"]),
                "joint_positions_rad": list(request["joint_positions_rad"]),
                "link_poses": {},
            }

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-gripper-key")
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=EmptyNativeFkKernel())
    client = TestClient(create_app(state))
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="finger_link", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )

    response = client.post(
        "/api/scene/preview-link-poses",
        json={
            "kind": "joint",
            "joint_names": ["joint_a", "finger_joint"],
            "joint_positions_rad": [0.0, 0.3],
            "link_names": ["finger_link"],
        },
    )

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["reachable"] is True
    assert body["link_poses"]["finger_link"]["position_m"] == pytest.approx(
        [1.3, 0.0, 0.0],
        abs=1e-6,
    )


def test_scene_routes_read_cached_visual_snapshot_without_backend_calls(
    monkeypatch: pytest.MonkeyPatch,
):
    class BlockingKernel(MockKernelClient):
        def get_execution_snapshot(self):
            raise AssertionError("scene routes must not read execution snapshots")

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("scene routes must not call FK")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("scene routes must not call FK")

        def get_scene_objects(self):
            raise AssertionError("scene routes must not read mutable scene objects")

    monkeypatch.setattr(routes_scene, "_load_model_frame_names", lambda _state: {"base_link"})
    kernel = BlockingKernel(joint_names=["joint_a"])
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=42,
            timestamp_ns=1,
            robot=routes_scene.SceneRobotResponse(
                robot_id="toy",
                group_id="arm",
                base_frame="base_link",
                tcp_frame="tool0",
                joint_names=["joint_a"],
                joint_positions={"joint_a": 0.0},
                state_source="commanded_sim_predicted",
                source_timestamp_ns=456,
                source_age_ns=7,
            ),
            link_poses={
                "base_link": {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            },
        ),
        reason="test",
    )
    publish_reasons: list[str] = []
    state.ensure_visual_state_publisher = lambda builder, period_s: None
    state.request_visual_state_one_shot = lambda *, reason="": publish_reasons.append(reason)

    state_payload = routes_scene.build_scene_state_payload(state)
    frames_payload = routes_scene.build_scene_frames_payload(state)

    assert state_payload["visual_state_diagnostics"]["served_from_visual_snapshot"] is True
    assert state_payload["visual_state_diagnostics"]["snapshot_sequence_number"] == 42
    assert state_payload["robot"]["robot_id"] == "toy"
    assert state_payload["robot"]["state_source"] == "commanded_sim_predicted"
    assert state_payload["robot"]["source_timestamp_ns"] == 456
    assert state_payload["robot"]["source_age_ns"] == 7
    frames = {frame["name"]: frame for frame in frames_payload["frames"]}
    assert frames["base_link"]["poseAvailable"] is True
    assert (
        frames_payload["visualStateDiagnostics"]["served_from_visual_snapshot"] is True
    )
    assert publish_reasons == []


def test_scene_state_deployment_only_uses_cached_visual_snapshot(
    monkeypatch: pytest.MonkeyPatch,
):
    class BlockingKernel(MockKernelClient):
        def get_execution_snapshot(self):
            raise AssertionError("loaded deployment route must not read execution snapshots")

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("loaded deployment route must not call FK")

        def get_scene_objects(self):
            raise AssertionError("loaded deployment route must not read mutable scene objects")

    kernel = BlockingKernel(joint_names=["joint_a"])
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=8,
            timestamp_ns=1,
            robot=routes_scene.SceneRobotResponse(
                robot_id="toy",
                group_id="arm",
                base_frame="base_link",
                tcp_frame="tool0",
                joint_names=["joint_a"],
                joint_positions={"joint_a": 0.0},
            ),
        ),
        reason="test",
    )
    state.ensure_visual_state_publisher = lambda builder, period_s: None

    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["served_from_visual_snapshot"] is True
    assert diagnostics["snapshot_sequence_number"] == 8
    assert payload["robot"]["robot_id"] == "toy"


def test_scene_state_marks_stale_visual_snapshot_without_blocking(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setenv("MANYFORGE_VIEWER_DEBUG_SNAPSHOT_STALE_AFTER_MS", "50")
    kernel = MockKernelClient(joint_names=["joint_a"])
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=7,
            timestamp_ns=1,
            robot=routes_scene.SceneRobotResponse(
                robot_id="toy",
                group_id="arm",
                base_frame="base_link",
                tcp_frame="tool0",
                joint_names=["joint_a"],
                joint_positions={"joint_a": 0.0},
            ),
        ),
        reason="test",
    )
    with state._visual_state_lock:
        state._visual_state_timestamp_ns = time.monotonic_ns() - 200_000_000
    state.ensure_visual_state_publisher = lambda builder, period_s: None

    payload = routes_scene.build_scene_state_payload(state)

    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["served_from_visual_snapshot"] is True
    assert diagnostics["snapshot_stale"] is True
    assert diagnostics["fallback_reason"] == "visual_snapshot_stale"
    assert payload["visual_state"]["stale"] is True


def test_scene_state_does_not_wait_for_blocked_visual_publisher(
    monkeypatch: pytest.MonkeyPatch,
):
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    entered = threading.Event()
    release = threading.Event()

    def blocked_builder(_state):
        entered.set()
        assert release.wait(timeout=5.0)
        raise RuntimeError("synthetic publisher block")

    monkeypatch.setattr(routes_scene, "_build_visual_state_snapshot_for_publisher", blocked_builder)

    try:
        first_payload = routes_scene.build_scene_state_payload(state)
        assert first_payload["visual_state_diagnostics"]["mode"] == "visual_snapshot_pending"
        assert entered.wait(timeout=2.0)

        started = time.monotonic()
        second_payload = routes_scene.build_scene_state_payload(state)
        elapsed_ms = (time.monotonic() - started) * 1000.0
    finally:
        release.set()
        state.stop_visual_state_publisher(timeout_s=2.0)

    assert elapsed_ms < 50.0
    diagnostics = second_payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["served_from_visual_snapshot"] is False
    assert second_payload["robot"]["robot_id"] == "toy"


def test_scene_state_visual_snapshot_uses_cached_objects_without_scene_read(
    monkeypatch: pytest.MonkeyPatch,
):
    class ObjectCacheKernel(MockKernelClient):
        def get_scene_objects(self):
            raise AssertionError("scene-state observer must use cached visual objects")

    monkeypatch.setattr(routes_urdf, "visual_link_names_for_fk", lambda _state: ())
    kernel = ObjectCacheKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(scene=[])
    scene_object = SceneObjectSnapshot(
        object_id="cached_box",
        shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.2, 0.3]),
        pose_in_universe={
            "position_m": [0.4, 0.5, 0.6],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
        attached=False,
        frame_id="universe",
    )
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=1,
            timestamp_ns=1,
            objects=routes_scene._scene_object_responses(state, [scene_object]),
        ),
        reason="test",
    )

    payload = routes_scene.build_scene_state_payload(state)

    assert payload["objects"][0]["object_id"] == "cached_box"
    assert payload["objects"][0]["shape"]["type"] == "box"
    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["objects_source"] == "visual_snapshot"
    assert diagnostics["objects_age_ms"] is not None


def test_scene_state_empty_visual_object_cache_queues_refresh_without_scene_read(
    monkeypatch: pytest.MonkeyPatch,
):
    class ObjectCacheKernel(MockKernelClient):
        def get_scene_objects(self):
            raise AssertionError("scene-state route must not read mutable scene objects")

    monkeypatch.setattr(routes_urdf, "visual_link_names_for_fk", lambda _state: ())
    kernel = ObjectCacheKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(scene=[])
    publish_reasons: list[str] = []
    state.ensure_visual_state_publisher = lambda builder, period_s: None
    state.request_visual_state_one_shot = lambda *, reason="": publish_reasons.append(reason)

    payload = routes_scene.build_scene_state_payload(state)

    assert payload["objects"] == []
    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["objects_source"] == "visual_snapshot_pending"
    assert publish_reasons == ["scene_state_cache_empty"]


def test_scene_state_does_not_wait_for_blocked_object_refresh(
    monkeypatch: pytest.MonkeyPatch,
):
    class BlockingRefreshKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__()
            self.entered = threading.Event()
            self.release = threading.Event()

        def get_scene_objects(self):
            self.entered.set()
            assert self.release.wait(timeout=5.0)
            return []

    kernel = BlockingRefreshKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(scene=[])
    publish_reasons: list[str] = []
    state.ensure_visual_state_publisher = lambda builder, period_s: None
    state.request_visual_state_one_shot = lambda *, reason="": publish_reasons.append(reason)

    try:
        refresh_result = state.request_visual_scene_objects_refresh(reason="test_blocked")
        assert refresh_result["queued"] is True
        assert kernel.entered.wait(timeout=2.0)

        started = time.monotonic()
        payload = routes_scene.build_scene_state_payload(state)
        elapsed_ms = (time.monotonic() - started) * 1000.0
    finally:
        kernel.release.set()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            _objects, meta = state.visual_scene_objects_snapshot()
            if not meta["refresh_running"]:
                break
            time.sleep(0.005)

    assert elapsed_ms < 50.0
    diagnostics = payload["visual_state_diagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["objects_refresh_running"] is True
    assert publish_reasons == ["scene_state_cache_empty"]


def test_scene_edit_updates_visual_cache_without_backend_refresh(
    monkeypatch: pytest.MonkeyPatch,
):
    client, state, _kernel = make_client()
    refresh_reasons: list[str] = []
    publish_reasons: list[str] = []
    state.request_visual_scene_objects_refresh = (
        lambda *, reason="": refresh_reasons.append(reason)
        or {"success": True, "queued": True}
    )
    state.request_visual_state_publish = (
        lambda *, reason="": publish_reasons.append(reason)
    )

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "operator_box",
            "shape_type": "box",
            "box_dimensions_m": [0.1, 0.2, 0.3],
            "pose_position_m": [0.4, 0.5, 0.6],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )

    assert response.status_code == 200
    assert response.json()["success"] is True
    objects, meta = state.visual_scene_objects_snapshot()
    assert meta["timestamp_ns"] > 0
    assert [obj.object_id for obj in objects] == ["operator_box"]
    assert objects[0].pose_in_universe["position_m"] == [0.4, 0.5, 0.6]
    assert objects[0].shape.type == "box"
    assert refresh_reasons == []
    assert publish_reasons == ["scene_edit_add"]


def test_attached_visual_object_pose_is_composed_from_link_pose(
    monkeypatch: pytest.MonkeyPatch,
):
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=1,
            timestamp_ns=1,
            link_poses={
                "tool0": {
                    "position_m": [1.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            },
        ),
        reason="test",
    )
    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="part",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
                pose_in_universe={
                    "position_m": [1.2, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=False,
                frame_id="universe",
            )
        ]
    )
    revision_before_attach = state.viewer_source_revisions()[3]

    visual_update = state.apply_visual_scene_edit_success(
        SceneEditRequest(operation="attach", object_id="part", frame_id="tool0")
    )
    revision_after_attach = state.viewer_source_revisions()[3]
    objects_at_attach, _meta = state.visual_scene_objects_for_link_poses(
        {
            "tool0": {
                "position_m": [1.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
    )
    objects_after_motion, _meta = state.visual_scene_objects_for_link_poses(
        {
            "tool0": {
                "position_m": [2.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
    )

    assert visual_update == {"updated": True, "needs_refresh": False}
    assert revision_after_attach == revision_before_attach + 1
    assert objects_at_attach[0].attached is True
    assert objects_at_attach[0].frame_id == "tool0"
    assert objects_at_attach[0].pose_in_universe["position_m"] == pytest.approx(
        [1.2, 0.0, 0.0]
    )
    assert objects_after_motion[0].pose_in_universe["position_m"] == pytest.approx(
        [2.2, 0.0, 0.0]
    )


def test_visual_object_pose_edit_invalidates_worker_static_input():
    state = ComposerState(
        node_environment=node_environment_for_catalog(_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="part",
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
    revision_before = state.viewer_source_revisions()[3]
    first = state.resolve_viewer_static_input(
        key=(revision_before,),
        resolver=lambda: "before",
    )

    result = state.apply_visual_scene_edit_success(
        SceneEditRequest(
            operation="update_pose",
            object_id="part",
            pose_position_m=[0.4, 0.5, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )
    revision_after = state.viewer_source_revisions()[3]
    second = state.resolve_viewer_static_input(
        key=(revision_after,),
        resolver=lambda: "after",
    )

    assert result == {"updated": True, "needs_refresh": False}
    assert revision_after == revision_before + 1
    assert first == "before"
    assert second == "after"


def test_visual_object_refresh_derives_attachment_offset_from_snapshot(
    monkeypatch: pytest.MonkeyPatch,
):
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=1,
            timestamp_ns=1,
            link_poses={
                "tool0": {
                    "position_m": [1.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            },
        ),
        reason="test",
    )

    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="part",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
                pose_in_universe={
                    "position_m": [1.2, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=True,
                frame_id="tool0",
            )
        ]
    )
    objects_after_motion, _meta = state.visual_scene_objects_for_link_poses(
        {
            "tool0": {
                "position_m": [2.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
    )

    assert objects_after_motion[0].attached is True
    assert objects_after_motion[0].pose_in_universe["position_m"] == pytest.approx(
        [2.2, 0.0, 0.0]
    )


def test_visual_object_reader_lazily_derives_missing_attachment_offset(
    monkeypatch: pytest.MonkeyPatch,
):
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    state.update_visual_scene_objects_snapshot(
        [
            SceneObjectSnapshot(
                object_id="part",
                shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
                pose_in_universe={
                    "position_m": [1.2, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                attached=True,
                frame_id="tool0",
            )
        ]
    )

    objects_at_first_fk, _meta = state.visual_scene_objects_for_link_poses(
        {
            "tool0": {
                "position_m": [1.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
    )
    objects_after_motion, _meta = state.visual_scene_objects_for_link_poses(
        {
            "tool0": {
                "position_m": [2.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
    )

    assert objects_at_first_fk[0].pose_in_universe["position_m"] == pytest.approx(
        [1.2, 0.0, 0.0]
    )
    assert objects_after_motion[0].pose_in_universe["position_m"] == pytest.approx(
        [2.2, 0.0, 0.0]
    )


def test_scene_frames_visual_snapshot_pending_flags_are_deterministic():
    client, state, _kernel = make_client()
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)

    response = client.get("/api/scene/frames")
    assert response.status_code == 200
    data = response.json()

    diagnostics = data["visualStateDiagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    assert diagnostics["served_from_visual_snapshot"] is False
    assert diagnostics["fallback_reason"] in {
        "visual_snapshot_pending",
        "visual_snapshot_publisher_unavailable",
    }
    assert "frame_inventory" in diagnostics["timings_ms"]


@_REQUIRES_PINOCCHIO_VISUAL_FK
def test_scene_frames_visual_snapshot_avoids_legacy_batch_fk(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    urdf_path = _write_toy_visual_urdf(tmp_path)

    class VisualFrameKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return ExecutionSnapshot(joint_positions_rad=[math.pi / 2.0])

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            raise AssertionError("frames route must not compute FK")

    entry = SimpleNamespace(urdf_path=str(urdf_path))
    monkeypatch.setattr(routes_urdf, "get_urdf_entry", lambda _state: entry)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "toy-frame-key")
    monkeypatch.setattr(
        routes_scene,
        "_load_model_frame_names",
        lambda _state: {"base_link", "link_1"},
    )
    monkeypatch.setattr(
        routes_urdf,
        "visual_link_names_for_fk",
        lambda _state: ("base_link", "link_1"),
    )
    kernel = VisualFrameKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    result = _publish_visual_snapshot(state)
    assert result["success"] is True, result

    payload = routes_scene.build_scene_frames_payload(state)

    diagnostics = payload["visualStateDiagnostics"]
    assert diagnostics["mode"] == "visual_snapshot"
    assert diagnostics["served_from_visual_snapshot"] is True
    frames = {frame["name"]: frame for frame in payload["frames"]}
    assert frames["link_1"]["poseAvailable"] is True
    assert frames["link_1"]["poseInWorld"]["orientation_xyzw"][2] == pytest.approx(
        math.sqrt(0.5),
        abs=1e-6,
    )


def test_scene_frames_does_not_wait_for_blocked_visual_publisher(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setattr(
        routes_scene,
        "_load_model_frame_names",
        lambda _state: {"base_link", "tool0"},
    )
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=MockKernelClient())
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="toy", group_id="arm"),
        adapters={},
        grippers={},
    )
    entered = threading.Event()
    release = threading.Event()

    def blocked_builder(_state):
        entered.set()
        assert release.wait(timeout=5.0)
        raise RuntimeError("synthetic publisher block")

    monkeypatch.setattr(routes_scene, "_build_visual_state_snapshot_for_publisher", blocked_builder)

    try:
        first_payload = routes_scene.build_scene_frames_payload(state)
        assert (
            first_payload["visualStateDiagnostics"]["mode"]
            == "visual_snapshot_pending"
        )
        assert entered.wait(timeout=2.0)

        started = time.monotonic()
        second_payload = routes_scene.build_scene_frames_payload(state)
        elapsed_ms = (time.monotonic() - started) * 1000.0
    finally:
        release.set()
        state.stop_visual_state_publisher(timeout_s=2.0)

    assert elapsed_ms < 50.0
    diagnostics = second_payload["visualStateDiagnostics"]
    assert diagnostics["mode"] == "visual_snapshot_pending"
    frames = {frame["name"]: frame for frame in second_payload["frames"]}
    assert frames["base_link"]["poseAvailable"] is False


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
    refresh = state.refresh_visual_scene_objects_sync(reason="test_runtime_addition")
    assert refresh["success"] is True

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
    assert obj["viewer_frame_visible"] is False


def test_scene_object_frame_visibility_updates_viewer_state():
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
    refresh = state.refresh_visual_scene_objects_sync(reason="test_object_frame_visibility")
    assert refresh["success"] is True

    toggle = client.post(
        "/api/scene/objects/frame-visibility",
        json={"objectId": "runtime_box", "visible": True},
    )
    assert toggle.status_code == 200
    assert toggle.json()["visible"] is True
    assert state.get_object_frame_visibility("runtime_box") is True

    response = client.get("/api/scene/state")
    assert response.status_code == 200
    data = response.json()
    assert data["objects"][0]["viewer_frame_visible"] is True


def test_scene_state_includes_robot_metadata_after_program_load(
    monkeypatch: pytest.MonkeyPatch,
):
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


def test_scene_state_does_not_advance_gripper_commands_on_read_path(
    monkeypatch: pytest.MonkeyPatch,
):
    """Scene-state polling is an observer path.

    Gripper command progression belongs to execution/tick paths. The native
    viewer must not mutate kernel state while reading FK because that couples
    rendering cadence to command state and can stall every observer route
    behind gripper or scene locks.
    """

    class OrderedKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__(joint_names=["joint_a"])

        def get_execution_snapshot(self):
            return SimpleNamespace(joint_positions_rad=[0.0])

        def get_link_pose(self, link_name: str, reference_frame: str):
            return {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            return {
                name: {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
                for name in link_names
            }

    monkeypatch.setattr(routes_urdf, "visual_link_names_for_fk", lambda _state: ("finger",))
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "ordered")
    kernel = OrderedKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base", tcp_frame="tcp", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="testbot", group_id="arm"),
        adapters={},
        grippers={},
    )
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=1,
            timestamp_ns=1,
            link_poses={
                "finger": {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            },
        ),
        reason="test",
    )

    payload = routes_scene.build_scene_state_payload(state)

    assert payload["link_poses"]["finger"]["position_m"] == [0.0, 0.0, 0.0]
    assert not hasattr(kernel, "update_gripper_commands")


def test_scene_frames_expose_model_inventory_and_visibility_control(
    monkeypatch: pytest.MonkeyPatch,
):
    client, state, kernel = make_client()
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)
    response = client.get("/api/scene/frames")
    assert response.status_code == 200
    data = response.json()

    assert data["viewerAvailable"] is True
    assert data["frameGizmoSizeM"] == 0.1
    frame_names = {frame["name"] for frame in data["frames"]}
    assert "universe" in frame_names
    assert "base_link" in frame_names
    assert "tool0" in frame_names
    tool0 = next(frame for frame in data["frames"] if frame["name"] == "tool0")
    assert "program_tcp" in tool0["roles"]
    assert tool0["viewerVisible"] is True
    assert tool0["poseAvailable"] is False

    toggle = client.post(
        "/api/scene/frames/visibility",
        json={"frameName": "base_link", "visible": True},
    )
    assert toggle.status_code == 200
    assert state.get_frame_visibility("base_link", default=False) is True

    resize = client.post(
        "/api/scene/frames/gizmo-size",
        json={"axesLengthM": 0.12},
    )
    assert resize.status_code == 200
    assert resize.json()["axesLengthM"] == 0.12
    assert state.get_frame_gizmo_size() == 0.12


def test_scene_frames_never_call_batch_fk_on_observer_route(monkeypatch):
    class BatchKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__()
            self.batch_calls: list[tuple[list[str], str]] = []

        def get_link_poses(self, link_names: list[str], reference_frame: str):
            self.batch_calls.append((list(link_names), reference_frame))
            return {
                name: {
                    "position_m": [float(index), 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
                for index, name in enumerate(link_names)
            }

        def get_link_pose(self, link_name: str, reference_frame: str):
            raise AssertionError("scene frames route must not compute FK")

    monkeypatch.setattr(
        routes_scene,
        "_load_model_frame_names",
        lambda _state: {"link_a", "link_b"},
    )
    kernel = BatchKernel()
    state = ComposerState(node_environment=node_environment_for_catalog(_CATALOG_PATH), kernel=kernel)
    state.program = SimpleNamespace(
        robot=SimpleNamespace(base_frame="base_link", tcp_frame="tool0", base_pose={}),
        scene=[],
    )
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(robot_id="testbot", group_id="arm"),
        adapters={},
        grippers={},
    )

    payload = routes_scene.build_scene_frames_payload(state)

    assert kernel.batch_calls == []
    poses_by_name = {frame["name"]: frame for frame in payload["frames"]}
    assert poses_by_name["link_a"]["poseAvailable"] is False
    assert poses_by_name["link_a"]["poseInWorld"] is None


def test_scene_frames_do_not_append_default_package_paths_when_deployment_declares_package_paths(monkeypatch, tmp_path):
    client, state, kernel = make_client()
    state.load_program(_PROGRAM_PATH, deployment_path=_DEPLOYMENT_PATH)
    state.deployment.robot_config.package_paths = ["/declared/share"]
    urdf_path = tmp_path / "test.urdf"
    urdf_path.write_text("<robot name='test'/>", encoding="utf-8")

    calls: dict[str, object] = {}

    def fail_default_package_paths() -> list[str]:
        raise AssertionError("default package paths must not be used when deployment declares package_paths")

    def fake_resolve_robot_model(**kwargs):
        calls["extra_package_paths"] = kwargs["extra_package_paths"]
        calls["package_paths"] = kwargs["package_paths"]
        return SimpleNamespace(
            urdf_path=str(urdf_path),
            package_paths=list(kwargs["package_paths"]),
        )

    class FakeUrdf:
        @staticmethod
        def load(*args, **kwargs):
            calls["filename_handler_present"] = "filename_handler" in kwargs
            return SimpleNamespace(link_map={"base_link": object(), "tool0": object()})

    monkeypatch.setattr(routes_urdf, "default_package_paths", fail_default_package_paths)
    monkeypatch.setattr(routes_urdf, "resolve_robot_model", fake_resolve_robot_model)
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


def test_scene_edit_accepts_cylinder_add():
    client, state, kernel = make_client()

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "cylinder1",
            "shape_type": "cylinder",
            "cylinder_radius_m": 0.04,
            "cylinder_height_m": 0.2,
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
        "runtime_collision_ticks_started",
        "runtime_collision_ticks_completed",
        "runtime_collision_ticks_faulted",
        "runtime_collision_tick_max_ns",
        "validation_deferred_runtime_monitor_active",
        "runtime_collision_snapshot_refresh_failures",
        "snapshots_published",
        "event_subscriber_exceptions",
        "observer_lag_max_ns",
        "events_emitted",
        "flight_recorder_records",
    }
    assert expected_keys.issubset(data.keys())


def test_kernel_metrics_preserves_structured_observed_source_telemetry():
    client, state, kernel = make_client()
    kernel.get_metrics = lambda: {  # type: ignore[method-assign]
        "validations_passed": 1,
        "controller_observed_state_sources": [
            {
                "source_kind": "controller_state",
                "source_id": "controller_state",
                "published_total": 42,
                "incomplete_total": 0,
                "writer_busy_total": 0,
                "latest_received_timestamp_ns": 123,
                "max_interarrival_ns": 456,
            }
        ],
    }

    response = client.get("/api/kernel/metrics")

    assert response.status_code == 200
    assert response.json()["controller_observed_state_sources"] == [
        {
            "source_kind": "controller_state",
            "source_id": "controller_state",
            "published_total": 42,
            "incomplete_total": 0,
            "writer_busy_total": 0,
            "latest_received_timestamp_ns": 123,
            "max_interarrival_ns": 456,
        }
    ]


def test_live_scene_edit_rejected_during_resumable_stopped():
    """A resumable STOPPED cycle owns the live scene: POST /api/scene/edit must
    be refused with a 409, because resuming against an externally-mutated scene
    is unsafe."""
    client, state, _kernel = _load_program_for_scene_sync()
    assert state.cycle_manager is not None
    state.cycle_manager._state = CycleState.STOPPED
    state.cycle_manager._tree = SimpleNamespace(root=SimpleNamespace(status="RUNNING"))

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "operator_box",
            "shape_type": "box",
            "box_dimensions_m": [0.1, 0.2, 0.3],
            "pose_position_m": [0.4, 0.5, 0.6],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 409
    body = response.json()
    assert body["success"] is False
    assert "resumable behavior-tree context" in (body["error"] or "")


def test_program_load_seeds_authored_scene_without_implicit_reset():
    """Loading a program seeds the authored scene into the live kernel via an
    explicit load-time seed (no implicit mirror). The seed is scene-only: the
    cycle stays IDLE with no behavior tree, so the first Run still prepares a
    fresh cycle."""
    _client, state, kernel = _load_program_for_scene_sync()
    assert state.cycle_manager is not None
    authored_ids = {r.object_id for r in (state.program.scene if state.program else [])}
    assert authored_ids, "fixture program must declare authored scene objects"
    live_ids = {obj.object_id for obj in kernel.get_scene_objects()}
    assert authored_ids.issubset(live_ids), (authored_ids, live_ids)
    assert state.cycle_manager.state == CycleState.IDLE
    assert state.cycle_manager._tree is None


def test_authored_scene_sync_endpoint_stays_removed():
    """The deleted authored-scene mirror endpoint must not come back.

    The former endpoint could reconcile the draft scene into the live planning
    scene during idle/stopped windows. That reset/detach path is safety
    relevant, so POST must remain non-callable and must not mutate live scene
    objects.
    """

    client, _state, kernel = _load_program_for_scene_sync()
    before_ids = {obj.object_id for obj in kernel.get_scene_objects()}

    response = client.post("/api/scene/sync-authored", json={})

    assert response.status_code in {404, 405}
    after_ids = {obj.object_id for obj in kernel.get_scene_objects()}
    assert after_ids == before_ids
