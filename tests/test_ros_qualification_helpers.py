"""ROS-free contracts retained from unavailable Isaac qualification clients."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType

import pytest


_ROOT = Path(__file__).resolve().parents[1]
_LITE6_SMOKE_ARTIFACT_VALIDATOR = (
    _ROOT / "scripts" / "ros" / "validate-isaac-lite6-smoke-artifact.py"
)

# Exact restoration source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# manyforge_behavior/tests/test_deployment.py blob
# 36ff9b99a5a51f03de84cf4692333cddc246214b.  The fixture is lines 217-281;
# the two ROS-free CLI cases are lines 4127-4215.
def _lite6_live_smoke_artifact_payload() -> dict:
    joints = ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
    start = {joint: 0.0 for joint in joints}
    probe = dict(start)
    probe["joint1"] = 0.03

    def arm_result(target):
        return {
            "target_positions_rad": target,
            "final_arm_state_rad": target,
            "final_isaac_state_rad": target,
            "max_abs_target_error_rad": 0.0,
        }

    return {
        "passed": True,
        "details": {
            "schema": "manyforge.isaac_lite6_live_smoke.v1",
            "started_at_utc": "2026-06-25T10:00:00+00:00",
            "ended_at_utc": "2026-06-25T10:00:03+00:00",
            "arm_action": "/isaac_arm_controller/follow_joint_trajectory",
            "gripper_action": "/lite_gripper_controller/gripper_cmd",
            "arm_joint_state_topic": "/joint_states",
            "isaac_joint_state_topic": "/isaac_joint_states",
            "gripper_command_topic": "/isaac_joint_commands_gripper",
            "gripper_transport_kind": "isaac_joint_state_effort",
            "arm_joints": joints,
            "gripper_joint": "right_finger_joint",
            "start_positions_rad": start,
            "probe_positions_rad": probe,
            "arm_delta_rad": 0.03,
            "arm_motion_duration_s": 0.8,
            "gripper_open_position": 0.0081,
            "gripper_close_position": 0.0,
            "gripper_open_effort": 5.0,
            "gripper_close_effort": -5.0,
            "gripper_dwell_s": 0.8,
            "arm_probe_result": arm_result(probe),
            "arm_restore_result": arm_result(start),
            "gripper_close_result": {
                "transport_kind": "isaac_joint_state_effort",
                "actuation_kind": "pneumatic",
                "state_authority": "sim_observed",
                "completion_policy": "dwell_elapsed",
                "target_position": 0.0,
                "raw_command_position": 0.0,
                "raw_command_effort": -5.0,
                "dwell_s": 0.8,
                "dwell_elapsed_s": 0.8,
                "isaac_state_position": 0.0,
            },
            "gripper_open_result": {
                "transport_kind": "isaac_joint_state_effort",
                "actuation_kind": "pneumatic",
                "state_authority": "sim_observed",
                "completion_policy": "dwell_elapsed",
                "target_position": 0.0081,
                "raw_command_position": 0.0081,
                "raw_command_effort": 5.0,
                "dwell_s": 0.8,
                "dwell_elapsed_s": 0.8,
                "isaac_state_position": 0.0081,
            },
        },
    }


def _load_script(name: str, relative_path: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, _ROOT / relative_path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def test_lite6_runtime_resolver_shell_is_a_pre_effect_i10_gate() -> None:
    source = (
        _ROOT / "scripts/ros/resolve-lite6-supervised-runtime.sh"
    ).read_text(encoding="utf-8")

    assert 'REASON_CODE="composable_cell_runtime_unavailable"' in source
    assert 'RESTORATION_GATE="I10/R"' in source
    assert "exit 78" in source
    for forbidden in (
        "generate-lite6-fixed-pneumatic-model.sh",
        "source ",
        "ros2 run",
        "ROBOT_IP",
        "OUTPUT_ROOT",
    ):
        assert forbidden not in source


def test_controlled_stop_seam_reducer_is_transport_neutral() -> None:
    module = _load_script(
        "manyforge_controlled_stop_evidence_test",
        "scripts/ros/validate-isaac-controlled-stop.py",
    )
    sample = module.StateSample
    samples = [
        sample(t, (0.0,), (velocity,), (0.0,), (velocity,))
        for t, velocity in (
            (-0.20, 0.20),
            (-0.10, 0.19),
            (-0.02, 0.18),
            (0.01, 0.17),
            (0.05, 0.10),
            (0.10, 0.00),
        )
    ]

    details = module.analyze_reference_seam(
        samples,
        seam_time_s=0.0,
        velocity_at_seam=(0.18,),
        seam_window_s=0.03,
        stopped_velocity_tolerance_rad_s=0.01,
    )

    assert details["seam_step_ratio"] == pytest.approx(1.0)
    assert details["backward_reversal_rad_s"] == 0.0
    assert details["settle_time_s"] == pytest.approx(0.1)
    assert module.controlled_stop_evidence_passes(
        accepted=True,
        max_speed_at_stop_rad_s=0.18,
        details=details,
    )


def test_controlled_stop_verdict_rejects_missing_or_malformed_metrics() -> None:
    module = _load_script(
        "manyforge_controlled_stop_evidence_validation_test",
        "scripts/ros/validate-isaac-controlled-stop.py",
    )
    complete = {
        "seam_step_ratio": 1.0,
        "seam_ref_vel_step_rad_s": 0.01,
        "backward_reversal_rad_s": 0.0,
    }

    for missing in complete:
        malformed = dict(complete)
        del malformed[missing]
        with pytest.raises(ValueError, match="missing required"):
            module.controlled_stop_evidence_passes(
                accepted=True,
                max_speed_at_stop_rad_s=0.18,
                details=malformed,
            )

    for key, value in (
        ("seam_step_ratio", True),
        ("seam_ref_vel_step_rad_s", float("nan")),
        ("backward_reversal_rad_s", -0.01),
    ):
        malformed = dict(complete)
        malformed[key] = value
        with pytest.raises(ValueError):
            module.controlled_stop_evidence_passes(
                accepted=True,
                max_speed_at_stop_rad_s=0.18,
                details=malformed,
            )


def test_controlled_stop_reducer_rejects_unbounded_or_nonfinite_samples() -> None:
    module = _load_script(
        "manyforge_controlled_stop_evidence_bounds_test",
        "scripts/ros/validate-isaac-controlled-stop.py",
    )
    sample = module.StateSample

    with pytest.raises(ValueError, match="4096-item bound"):
        module.analyze_reference_seam(
            (
                sample(float(index), (0.0,), (0.0,), (0.0,), (0.0,))
                for index in range(module.MAX_EVIDENCE_SAMPLES + 1)
            ),
            seam_time_s=0.0,
            velocity_at_seam=(0.1,),
            seam_window_s=0.1,
            stopped_velocity_tolerance_rad_s=0.01,
        )

    with pytest.raises(ValueError, match="finite"):
        module.analyze_reference_seam(
            [
                sample(0.0, (0.0,), (0.1,), (0.0,), (0.1,)),
                sample(0.1, (0.0,), (float("nan"),), (0.0,), (0.1,)),
                sample(0.2, (0.0,), (0.0,), (0.0,), (0.0,)),
            ],
            seam_time_s=0.1,
            velocity_at_seam=(0.1,),
            seam_window_s=0.05,
            stopped_velocity_tolerance_rad_s=0.01,
        )


def test_lite6_smoke_envelope_matches_retained_validator_schema() -> None:
    module = _load_script(
        "manyforge_lite6_smoke_envelope_test",
        "scripts/ros/validate-isaac-lite6-smoke.py",
    )

    payload = module.build_smoke_artifact(
        module.SmokeResult(passed=True, details={"arm_probe_result": {}}),
        started_at_utc="2026-07-31T10:00:00+00:00",
        ended_at_utc="2026-07-31T10:00:01+00:00",
    )

    assert payload["passed"] is True
    assert payload["details"]["schema"] == "manyforge.isaac_lite6_live_smoke.v1"
    assert payload["details"]["arm_probe_result"] == {}


def test_lite6_live_smoke_artifact_validator_accepts_complete_evidence(
    tmp_path: Path,
) -> None:
    """The recorded live-smoke artifact has a deterministic acceptance gate."""
    artifact = tmp_path / "isaac-lite6-pick-place-live-smoke.json"
    artifact.write_text(
        json.dumps(_lite6_live_smoke_artifact_payload()),
        encoding="utf-8",
    )

    completed = subprocess.run(
        [
            sys.executable,
            str(_LITE6_SMOKE_ARTIFACT_VALIDATOR),
            str(artifact),
            "--json",
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    report = json.loads(completed.stdout)
    assert report["passed"] is True


def test_lite6_live_smoke_artifact_validator_rejects_incomplete_evidence(
    tmp_path: Path,
) -> None:
    """The recorded live-smoke artifact gate fails closed on weak evidence."""
    cases = {
        "failed_run": lambda payload: payload.update({"passed": False}),
        "backwards_timestamps": lambda payload: payload["details"].update(
            {
                "started_at_utc": "2026-06-25T10:00:03+00:00",
                "ended_at_utc": "2026-06-25T10:00:00+00:00",
            }
        ),
        "missing_lite6_joint": lambda payload: payload["details"][
            "arm_joints"
        ].pop(),
        "missing_raw_gripper_command": lambda payload: payload["details"][
            "gripper_open_result"
        ].pop("raw_command_position"),
        "missing_direct_observed_state": lambda payload: payload["details"][
            "gripper_open_result"
        ].pop("isaac_state_position"),
        "wrong_direct_completion_policy": lambda payload: payload["details"][
            "gripper_close_result"
        ].update({"completion_policy": "position_reached"}),
        "short_direct_dwell": lambda payload: payload["details"][
            "gripper_close_result"
        ].update({"dwell_elapsed_s": 0.1}),
        "excessive_arm_target_error": lambda payload: payload["details"][
            "arm_restore_result"
        ].update(
            {
                "final_arm_state_rad": {
                    **payload["details"]["arm_restore_result"][
                        "final_arm_state_rad"
                    ],
                    "joint1": 0.5,
                },
                "max_abs_target_error_rad": 0.5,
            }
        ),
        "forged_arm_target_error": lambda payload: payload["details"][
            "arm_restore_result"
        ]["final_arm_state_rad"].update({"joint1": 0.5}),
    }
    for name, mutate in cases.items():
        payload = json.loads(json.dumps(_lite6_live_smoke_artifact_payload()))
        mutate(payload)
        artifact = tmp_path / f"{name}.json"
        artifact.write_text(json.dumps(payload), encoding="utf-8")

        completed = subprocess.run(
            [
                sys.executable,
                str(_LITE6_SMOKE_ARTIFACT_VALIDATOR),
                str(artifact),
                "--json",
            ],
            text=True,
            capture_output=True,
        )

        assert completed.returncode != 0, name
        report = json.loads(completed.stdout)
        assert report["passed"] is False
        assert report["error"]


def test_measured_grasp_baseline_round_trips_object_pose() -> None:
    module = _load_script(
        "manyforge_measured_grasp_math_test",
        "scripts/ros/validate-isaac-measured-grasp.py",
    )
    tcp = module.pose_to_matrix((1.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
    measured_object = module.pose_to_matrix(
        (1.0, 2.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )

    baseline = module.tcp_to_object_baseline(tcp, measured_object)
    reconstructed = module.object_from_tcp_baseline(tcp, baseline)

    for actual_row, expected_row in zip(reconstructed, measured_object, strict=True):
        assert actual_row == pytest.approx(expected_row)
    assert module.matrix_to_pose(reconstructed, frame_id="cell")[
        "position_m"
    ] == pytest.approx([1.0, 2.0, 0.0])


@pytest.mark.parametrize(
    "position",
    [
        (True, 0.0, 0.0),
        ("0", 0.0, 0.0),
        (float("nan"), 0.0, 0.0),
    ],
)
def test_measured_grasp_pose_math_rejects_coercible_or_nonfinite_values(
    position,
) -> None:
    module = _load_script(
        "manyforge_measured_grasp_numeric_validation_test",
        "scripts/ros/validate-isaac-measured-grasp.py",
    )

    with pytest.raises(ValueError):
        module.pose_to_matrix(position, (0.0, 0.0, 0.0, 1.0))


@pytest.mark.parametrize(
    ("matrix", "message"),
    [
        (
            [
                [2.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
            "orthonormal",
        ),
        (
            [
                [-1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
            "determinant must be \\+1",
        ),
        (
            [
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 1.0, 1.0],
            ],
            "homogeneous bottom row",
        ),
    ],
)
def test_measured_grasp_pose_math_rejects_non_rigid_matrices(
    matrix, message: str
) -> None:
    module = _load_script(
        "manyforge_measured_grasp_rigid_validation_test",
        "scripts/ros/validate-isaac-measured-grasp.py",
    )

    with pytest.raises(ValueError, match=message):
        module.invert_rigid_transform(matrix)


def test_measured_grasp_result_wire_is_absent_and_frame_is_bounded() -> None:
    module = _load_script(
        "manyforge_measured_grasp_result_cutover_test",
        "scripts/ros/validate-isaac-measured-grasp.py",
    )
    identity = module.pose_to_matrix(
        (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
    )

    assert not hasattr(module, "EvidenceRow")
    assert not hasattr(module, "QualificationResult")
    with pytest.raises(ValueError, match="frame_id must be a non-empty string"):
        module.matrix_to_pose(identity, frame_id=True)
    with pytest.raises(ValueError, match="1024-character bound"):
        module.matrix_to_pose(identity, frame_id="x" * 1025)
