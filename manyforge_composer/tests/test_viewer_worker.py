from __future__ import annotations

import builtins
import math
import os
import threading
import time

import pytest

from manyforge_composer.backend.viewer_worker import (
    ViewerWorkerError,
    ViewerWorkerSupervisor,
    _decode_envelope,
    _encode_envelope,
)
from manyforge_composer.backend.visual_state import (
    VisualFkModelDescriptor,
    VisualSnapshotUnavailable,
    compute_robot_visual_link_poses_from_descriptor,
)


def _sleeping_worker(_requests, _responses, _max_input, _max_output) -> None:
    time.sleep(10.0)


def _crashing_worker(_requests, _responses, _max_input, _max_output) -> None:
    os._exit(17)


def test_worker_envelope_rejects_checksum_and_length_tampering() -> None:
    envelope = _encode_envelope(
        schema="test.v1",
        request_id=3,
        payload={"value": 1},
        max_bytes=1024,
    )
    request_id, payload = _decode_envelope(
        envelope,
        expected_schema="test.v1",
        max_bytes=1024,
    )
    assert request_id == 3
    assert payload == {"value": 1}

    tampered = dict(envelope, payload=envelope["payload"] + b" ")
    with pytest.raises(ViewerWorkerError, match="payload length"):
        _decode_envelope(
            tampered,
            expected_schema="test.v1",
            max_bytes=1024,
        )


def test_visual_fk_reports_missing_dependency_at_worker_owned_boundary(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    real_import = builtins.__import__

    def blocked_import(name, globals=None, locals=None, fromlist=(), level=0):
        if name == "pinocchio" or name.startswith("pinocchio."):
            raise ModuleNotFoundError("No module named 'pinocchio'")
        return real_import(name, globals, locals, fromlist, level)

    monkeypatch.setattr(builtins, "__import__", blocked_import)
    with pytest.raises(VisualSnapshotUnavailable) as raised:
        compute_robot_visual_link_poses_from_descriptor(
            VisualFkModelDescriptor(
                model_key="missing-pinocchio-worker-test",
                canonical_urdf_path="/does/not/matter.urdf",
                authority_metadata={},
            ),
            joint_names=["joint"],
            joint_positions_rad=[0.0],
            link_names=["base"],
            reference_frame="base",
            base_frame="base",
            base_pose={},
        )
    assert raised.value.reason == "pinocchio_unavailable"


def test_supervisor_retires_worker_on_bounded_timeout() -> None:
    supervisor = ViewerWorkerSupervisor(
        timeout_s=0.1,
        worker_target=_sleeping_worker,
    )
    try:
        with pytest.raises(ViewerWorkerError) as raised:
            supervisor.build({}, input_signature=(), sample_duration_ms=0.0)
        assert raised.value.reason == "viewer_worker_timeout"
        status = supervisor.status()
        assert status["alive"] is False
        assert status["timeouts"] == 1
    finally:
        supervisor.stop()


def test_supervisor_contains_worker_process_crash() -> None:
    supervisor = ViewerWorkerSupervisor(
        timeout_s=1.0,
        worker_target=_crashing_worker,
    )
    try:
        with pytest.raises(ViewerWorkerError) as raised:
            supervisor.build({}, input_signature=(), sample_duration_ms=0.0)
        assert raised.value.reason == "viewer_worker_exited"
        assert "17" in raised.value.detail
        assert supervisor.status()["alive"] is False
    finally:
        supervisor.stop()


def test_supervisor_stop_interrupts_in_flight_visual_wait() -> None:
    supervisor = ViewerWorkerSupervisor(
        timeout_s=5.0,
        worker_target=_sleeping_worker,
    )
    finished = threading.Event()
    errors: list[ViewerWorkerError] = []

    def request() -> None:
        try:
            supervisor.build({}, input_signature=(), sample_duration_ms=0.0)
        except ViewerWorkerError as exc:
            errors.append(exc)
        finally:
            finished.set()

    thread = threading.Thread(target=request)
    thread.start()
    deadline = time.monotonic() + 2.0
    while not supervisor.status()["alive"] and time.monotonic() < deadline:
        time.sleep(0.01)
    supervisor.stop()
    thread.join(timeout=1.0)
    assert finished.is_set()
    assert errors
    assert errors[0].reason == "viewer_worker_stopping"


def _worker_payload(
    urdf_path: str,
    *,
    joint_position: float,
    attachment_offset: dict | None,
) -> dict:
    return {
        "model_descriptor": {
            "model_key": f"worker-fixture:{urdf_path}",
            "canonical_urdf_path": urdf_path,
            "authority_metadata": {},
        },
        "robot": {
            "robot_id": "robot",
            "group_id": "arm",
            "urdf_model_key": "worker-fixture",
            "base_frame": "base",
            "tcp_frame": "tool",
            "joint_names": ["joint"],
            "joint_positions": {"joint": joint_position},
        },
        "visual_joint_names": ["joint"],
        "visual_joint_positions": [joint_position],
        "link_names": ["base", "tool"],
        "world_frame": "base",
        "base_frame": "base",
        "base_pose": {},
        "grippers": [],
        "pose_sources": [],
        "objects": [
            {
                "response": {
                    "object_id": "part",
                    "shape": None,
                    "pose_in_universe": {
                        "position_m": [1.2, 0.0, 0.0],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": True,
                    "attached_to": "tool",
                },
                "frame_id": "tool",
                "attachment_offset": attachment_offset,
            }
        ],
        "object_revision": 7,
        "demanded_layers": ["canonical"],
        "frame_roles": {},
        "frame_names": [],
        "frame_visibility": {},
        "object_axes_gizmo_size_m": 0.1,
        "frame_axes_gizmo_size_m": 0.1,
    }


def test_real_worker_owns_attachment_following_and_restarts_with_fresh_epoch(
    tmp_path,
) -> None:
    urdf = tmp_path / "viewer_worker.urdf"
    urdf.write_text(
        """<?xml version="1.0"?>
<robot name="viewer_worker">
  <link name="base"/>
  <link name="tool"/>
  <joint name="joint" type="revolute">
    <parent link="base"/><child link="tool"/>
    <origin xyz="1 0 0" rpy="0 0 0"/><axis xyz="0 0 1"/>
    <limit lower="-3.14" upper="3.14" effort="1" velocity="1"/>
  </joint>
</robot>
""",
        encoding="utf-8",
    )
    supervisor = ViewerWorkerSupervisor(timeout_s=3.0)
    try:
        first = supervisor.build(
            _worker_payload(
                str(urdf),
                joint_position=0.0,
                attachment_offset=None,
            ),
            input_signature=("first",),
            sample_duration_ms=0.0,
        )
        offset = first.derived_attachment_offsets["part"]
        assert offset["position_m"] == pytest.approx([0.2, 0.0, 0.0])
        first_epoch = first.publication.producer_epoch

        second = supervisor.build(
            _worker_payload(
                str(urdf),
                joint_position=math.pi / 2.0,
                attachment_offset=offset,
            ),
            input_signature=("second",),
            sample_duration_ms=0.0,
        )
        assert second.snapshot.objects[0].pose_in_universe["position_m"] == pytest.approx(
            [1.0, 0.2, 0.0], abs=1e-6
        )
        assert second.publication.producer_epoch == first_epoch

        supervisor._retire(force=True)  # exercise the supervised restart boundary
        restarted = supervisor.build(
            _worker_payload(
                str(urdf),
                joint_position=math.pi / 2.0,
                attachment_offset=offset,
            ),
            input_signature=("restarted",),
            sample_duration_ms=0.0,
        )
        assert restarted.publication.producer_epoch != first_epoch
        assert restarted.publication.manifest is not None
        status = supervisor.status()
        assert status["starts"] == 2
        assert status["alive"] is True
        assert status["worker_policy"]["scheduler"] == "SCHED_OTHER"
        assert status["worker_policy"]["policy_ok"] is True
    finally:
        supervisor.stop()
