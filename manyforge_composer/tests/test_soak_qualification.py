"""Session-8 soak/load qualification for bounded performance surfaces.

These tests are intentionally opt-in. They exercise longer loops than the
normal per-PR regression suite and should be run through
``scripts/ci/run-soak-qualification.sh`` or with
``MANYFORGE_RUN_SOAK_TESTS=1`` inside the dev Docker image.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

if os.getenv("MANYFORGE_RUN_SOAK_TESTS") != "1":
    pytest.skip(
        "set MANYFORGE_RUN_SOAK_TESTS=1 to run soak/load qualification",
        allow_module_level=True,
    )

pytest.importorskip("fastapi")

from manyforge_behavior.kernel_client import (  # noqa: E402
    GripperCommandState,
    GripperCommandStatus,
    ExecutionSnapshot,
)
from manyforge_composer.backend import routes_scene, routes_urdf  # noqa: E402
from manyforge_composer.backend.event_recorder import EventRecorder  # noqa: E402


_POSE = {
    "position_m": [0.0, 0.0, 0.0],
    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
}


class _SoakKernel:
    def __init__(self) -> None:
        self.single_link_fk_calls = 0
        self.batch_fk_calls = 0
        self.batch_link_count = 0
        self.gripper_status_calls = 0
        self.gripper_update_calls = 0

    def get_joint_names(self) -> list[str]:
        return ["j0", "j1", "j2"]

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        return ExecutionSnapshot(joint_positions_rad=[0.1, 0.2, 0.3])

    def get_link_pose(self, link_name: str, reference_frame: str) -> dict[str, Any] | None:
        self.single_link_fk_calls += 1
        assert reference_frame in {"base", "world"}
        assert link_name == "tcp"
        return dict(_POSE)

    def get_link_poses(
        self,
        link_names: list[str],
        reference_frame: str,
    ) -> dict[str, dict[str, Any]]:
        self.batch_fk_calls += 1
        self.batch_link_count += len(link_names)
        assert reference_frame == "world"
        return {name: dict(_POSE) for name in link_names}

    def get_scene_objects(self) -> list[Any]:
        return []

    def update_gripper_commands(self) -> None:
        self.gripper_update_calls += 1

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        self.gripper_status_calls += 1
        return GripperCommandStatus(
            state=GripperCommandState.IDLE,
            gripper_id=gripper_id,
            current_position=0.0,
            target_position=0.0,
        )


class _SoakState:
    def __init__(self, kernel: _SoakKernel, *, gripper_count: int = 0) -> None:
        self.kernel = kernel
        self._kernel = kernel
        self.cycle_manager = None
        self._last_known_joint_positions_rad: list[float] | None = None
        self.program = SimpleNamespace(
            robot=SimpleNamespace(
                base_frame="base",
                tcp_frame="tcp",
                base_pose={},
            ),
            scene=[],
        )
        grippers = {
            f"gripper_{idx:02d}": SimpleNamespace(
                joint_name=f"finger_joint_{idx:02d}",
                mount_frame="tool0",
                attach_frame="tool0",
                tcp_frame="tcp",
                auxiliary_frames=[],
                motion_time_s=0.25,
                srdf_presets={},
            )
            for idx in range(gripper_count)
        }
        self.deployment = SimpleNamespace(
            robot_config=SimpleNamespace(robot_id="soak_bot", group_id="arm"),
            adapters={
                "context": SimpleNamespace(
                    config={"scene_world_frame": "world"},
                    adapter_id="planning.roboplan_context",
                )
            },
            grippers=grippers,
        )

    def get_object_axes_gizmo_size(self) -> float:
        return 0.1

    def get_object_frame_visibility(self, _object_id: str) -> bool:
        return True


def test_scene_state_soak_uses_one_batch_fk_per_poll(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    link_names = tuple(f"link_{idx:03d}" for idx in range(80))
    monkeypatch.setattr(routes_urdf, "visual_link_names_for_fk", lambda _state: link_names)
    monkeypatch.setattr(routes_urdf, "active_urdf_model_key", lambda _state: "soak-model")
    monkeypatch.setattr(routes_scene, "_collect_gripper_states", lambda _state: [])
    kernel = _SoakKernel()
    state = _SoakState(kernel)

    for _ in range(150):
        payload = routes_scene.build_scene_state_payload(state)
        assert len(payload["link_poses"]) == len(link_names)

    assert kernel.batch_fk_calls == 150
    assert kernel.batch_link_count == 150 * len(link_names)
    # One single-link FK for the TCP pose per response is expected; there
    # must be no fallback per-link FK chatter for visual links.
    assert kernel.single_link_fk_calls == 150


def test_gripper_state_soak_resolves_urdf_limits_once_per_response(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    get_entry_calls = 0

    def fake_get_urdf_entry(_state: Any) -> Any:
        nonlocal get_entry_calls
        get_entry_calls += 1
        return SimpleNamespace(
            joints=tuple(
                SimpleNamespace(
                    name=f"finger_joint_{idx:02d}",
                    limit_lower=0.0,
                    limit_upper=0.8,
                )
                for idx in range(24)
            )
        )

    monkeypatch.setattr(routes_urdf, "get_urdf_entry", fake_get_urdf_entry)
    monkeypatch.setattr(routes_urdf, "resolve_gripper_visualization", lambda *args, **kwargs: None)
    kernel = _SoakKernel()
    state = _SoakState(kernel, gripper_count=24)

    for _ in range(75):
        grippers = routes_scene._collect_gripper_states(state)
        assert len(grippers) == 24
        assert all(gripper.limit_lower == 0.0 for gripper in grippers)
        assert all(gripper.limit_upper == 0.8 for gripper in grippers)

    assert get_entry_calls == 75
    assert kernel.gripper_status_calls == 75 * 24
    assert kernel.gripper_update_calls == 75


class _FakeUrdf:
    base_link = "base_link"
    joint_map: dict[str, object] = {}

    def __init__(self) -> None:
        self.link_map = {"base_link": SimpleNamespace(visuals=[])}

    @staticmethod
    def load(_urdf_path: str, **_kwargs: object) -> "_FakeUrdf":
        return _FakeUrdf()


def test_urdf_cache_soak_enforces_entry_cap(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_URDF_CACHE_ENTRIES", 3)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=_FakeUrdf))
    routes_urdf._urdf_cache.clear()  # type: ignore[attr-defined]
    try:
        paths = []
        for idx in range(9):
            path = tmp_path / f"robot_{idx}.urdf"
            path.write_text(f"<robot name='r{idx}'/>", encoding="utf-8")
            paths.append(str(path))
            assert routes_urdf._load_urdf_entry(str(path), ()) is not None
            assert len(routes_urdf._urdf_cache) <= 3  # type: ignore[attr-defined]

        retained_paths = {key[0] for key in routes_urdf._urdf_cache.keys()}  # type: ignore[attr-defined]
        assert retained_paths == set(paths[-3:])
    finally:
        routes_urdf._urdf_cache.clear()  # type: ignore[attr-defined]


class _FakeMesh:
    def export(self, *, file_type: str) -> bytes:
        assert file_type == "glb"
        return b"glb"


def test_glb_cache_soak_enforces_per_model_cap(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_GLB_CACHE_ENTRIES_PER_MODEL", 4)
    monkeypatch.setitem(
        sys.modules,
        "trimesh",
        SimpleNamespace(load=lambda *_args, **_kwargs: _FakeMesh()),
    )
    entry = routes_urdf._UrdfCacheEntry(
        urdf_path=str(tmp_path / "robot.urdf"),
        package_paths=(),
        mtime_ns=1,
        root_link="base_link",
        links=(),
        joints=(),
        link_names_for_fk=(),
        mesh_glb_cache={},
        urdf_model=None,
    )
    for idx in range(12):
        mesh_path = tmp_path / f"mesh_{idx}.stl"
        mesh_path.write_text("solid fixture", encoding="utf-8")
        visual = routes_urdf._LinkVisual(
            link_name=f"link_{idx}",
            visual_index=idx,
            mesh_resource_uri=str(mesh_path),
            mesh_path=str(mesh_path),
            mesh_scale=(1.0, 1.0, 1.0),
            primitive=None,
            visual_origin_xyz=(0.0, 0.0, 0.0),
            visual_origin_rpy=(0.0, 0.0, 0.0),
            visual_origin_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
        assert routes_urdf._render_visual_glb(entry, visual) == b"glb"
        assert len(entry.mesh_glb_cache) <= 4

    retained_indices = {key[1] for key in entry.mesh_glb_cache.keys()}
    assert retained_indices == {8, 9, 10, 11}


def test_execution_history_soak_keeps_default_and_max_reads_bounded(
    tmp_path: Path,
) -> None:
    run_id = "run-soak-history"
    trace_file = tmp_path / f"{run_id}.events.jsonl"
    with trace_file.open("w", encoding="utf-8") as handle:
        for idx in range(12_500):
            handle.write(
                json.dumps(
                    {
                        "event_index": idx,
                        "timestamp_ns": idx,
                        "type": "cycle_state_changed",
                        "severity": "INFO",
                        "component": "manyforge.cycle",
                        "message": "cycle state changed",
                    },
                    sort_keys=True,
                )
            )
            handle.write("\n")

    recorder = EventRecorder(trace_dir=tmp_path)
    default_history = recorder.history(run_id=run_id)
    assert len(default_history["events"]) == 1_000
    assert default_history["events"][0]["event_index"] == 11_500

    max_history = recorder.history(run_id=run_id, limit=50_000)
    assert len(max_history["events"]) == 10_000
    assert max_history["events"][0]["event_index"] == 2_500

    zero_history = recorder.history(run_id=run_id, limit=0)
    assert zero_history["events"] == []


def test_failure_archive_soak_retention_stays_under_byte_budget(
    tmp_path: Path,
) -> None:
    max_bytes = 20_000
    archive_root = tmp_path / "archives"
    recorder = EventRecorder(
        trace_dir=tmp_path / "traces",
        failure_archive_dir=archive_root,
        failure_archive_max_age_s=3600.0,
        failure_archive_max_bytes=max_bytes,
    )
    run_id = "run-soak-archive"

    for idx in range(12):
        recorder._record_event(
            event_type="cycle_error",
            run_id=run_id,
            details={
                "error": f"simulated pressure incident {idx}",
                "payload": "x" * 512,
            },
        )
        if idx % 2 == 1:
            assert recorder.wait_for_failure_archives_idle(timeout_s=5.0)

    assert recorder.wait_for_failure_archives_idle(timeout_s=5.0)
    status = recorder.failure_archive_status()
    assert status["total_bytes"] <= max_bytes
    assert status["last_cleanup"]["deleted_count"] > 0
    assert status["dropped_archives"] == 0
    assert status["last_archive_error"] is None
