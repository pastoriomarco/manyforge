"""End-to-end tests for the native C++ kernel (no trampolines).

Tests: NativeKernelClient through the full stack — execute, scene edits,
error recovery, caching, and benchmarks.
"""

import time
import math
from pathlib import Path
import pytest

from manyforge_behavior.kernel_client import (
    ExecuteRequest, ExecutionState, GripperCommandRequest,
    GripperCommandState, MotionInterruptScope, MotionInterruptStopProfile,
    SceneEditRequest,
)

try:
    from native_kernel_client import (
        NativeKernelClient,
        _serialize_motion_interrupt_scope,
        _gripper_state_from_native,
        _state_from_native,
    )
except ImportError:
    NativeKernelClient = None
    _serialize_motion_interrupt_scope = None
    _gripper_state_from_native = None
    _state_from_native = None
    HAS_NATIVE_CLIENT = False
else:
    HAS_NATIVE_CLIENT = True

try:
    import manyforge_roboplan_py
except ImportError:
    HAS_NATIVE = False
else:
    HAS_NATIVE = HAS_NATIVE_CLIENT


UR5_JOINTS = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]

GOAL_A = [0.1, -0.5, 0.5, -0.5, -0.5, 0.0]
GOAL_B = [0.3, -0.8, 0.8, -0.3, -0.8, 0.1]
DEMO_REST = [0.0, -1.57, 1.57, -1.57, -1.57, 0.0]
UR10E_ARM_JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]
UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q = [
    0.09528976894858,
    -1.351485248665011,
    2.11062921572848,
    -2.3299263952809675,
    -1.5707963829586316,
    0.09608604784889696,
]
ROBOTIQ_GRASP_TOUCH_LINKS = [
    "left_inner_finger",
    "right_inner_finger",
    "left_inner_finger_pad",
    "right_inner_finger_pad",
]
_REPO_ROOT = Path(__file__).resolve().parents[2]
_UR10E_URDF = _REPO_ROOT / "manyforge_example_models" / "models" / "ur10e_robotiq_2f_140" / "ur10e_robotiq_2f_140.urdf"
_UR10E_SRDF = _REPO_ROOT / "manyforge_example_models" / "models" / "ur10e_robotiq_2f_140" / "ur10e_robotiq_2f_140.srdf"


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def native_kernel():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")
    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    return NativeKernelClient(bundle)


@pytest.fixture
def native_kernel_ur10e_robotiq():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")
    bundle = manyforge_roboplan_py.create_native_kernel_from_paths(
        "ur10e_robotiq_2f_140",
        "arm",
        str(_UR10E_URDF),
        str(_UR10E_SRDF),
        [str(_REPO_ROOT)],
    )
    return NativeKernelClient(bundle)


def test_create_native_kernel_accepts_collision_monitor_config():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    bundle = manyforge_roboplan_py.create_native_kernel(
        "ur5_gripper",
        "arm",
        {"collision_monitor_period_ns": 25_000_000},
    )
    kernel = NativeKernelClient(bundle)
    status = kernel.get_status()

    assert status.state == ExecutionState.IDLE


def test_motion_interrupt_scope_serializes_settle_velocity_threshold():
    if not HAS_NATIVE_CLIENT:
        pytest.skip("native_kernel_client not available")

    scope = MotionInterruptScope(
        scope_id="scope_a",
        policy_id="probe_trip",
        activation_mode="trip_if_already_active",
        stop_profile=MotionInterruptStopProfile(
            profile_id="operator_soft",
            stop_mode="soft_on_path",
            deceleration_time_s=0.5,
            max_stop_time_s=0.7,
            settle_velocity_threshold_rad_s=0.02,
            per_joint_max_deceleration_rad_s2={"j1": 8.0},
        ),
    )

    serialized = _serialize_motion_interrupt_scope(scope)
    assert serialized["stop_profile"]["settle_velocity_threshold_rad_s"] == 0.02


def test_native_gripper_grasp_loss_marks_cached_status_without_extension():
    if not HAS_NATIVE_CLIENT:
        pytest.skip("native_kernel_client not available")

    class FakeBundle:
        def __init__(self):
            self.positions = [0.0081]

        def get_group_joint_names(self, group_id):
            assert group_id == "lite_gripper"
            return ["right_finger_joint"]

        def get_group_joint_positions(self, group_id):
            assert group_id == "lite_gripper"
            return list(self.positions)

        def set_group_joint_positions(self, group_id, positions):
            assert group_id == "lite_gripper"
            self.positions = list(positions)

    kernel = NativeKernelClient(FakeBundle())
    close = kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="primary",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0,
            duration_s=0.0,
            actuation_kind="pneumatic",
            state_authority="native_backend",
            completion_policy="dwell_elapsed",
            grasp_confirmation_policy="perception",
            transport_kind="native_kinematic",
            open_state_ref="open",
            closed_state_ref="closed",
            srdf_presets={"open": 0.0081, "closed": 0.0},
        )
    )
    assert close.accepted is True, close.error

    lost = kernel.mark_gripper_grasp_lost(
        gripper_id="primary",
        evidence_label="attached_drift_monitor",
        message="measured object drift exceeded threshold",
    )
    assert lost.accepted is True, lost.error
    status = kernel.get_gripper_status("primary")
    assert status.state == GripperCommandState.FAILED
    assert status.evidence_state == "pressure_or_part_lost"
    assert status.reason_code == "grasp_confirmation_lost"
    assert "attached_drift_monitor" in status.evidence

    reopen = kernel.command_gripper(
        GripperCommandRequest(
            gripper_id="primary",
            group_id="lite_gripper",
            joint_name="right_finger_joint",
            target_position=0.0081,
            duration_s=0.0,
            actuation_kind="pneumatic",
            state_authority="native_backend",
            completion_policy="dwell_elapsed",
            grasp_confirmation_policy="perception",
            transport_kind="native_kinematic",
            open_state_ref="open",
            closed_state_ref="closed",
            srdf_presets={"open": 0.0081, "closed": 0.0},
        )
    )
    assert reopen.accepted is True, reopen.error
    reopened = kernel.get_gripper_status("primary")
    assert reopened.state == GripperCommandState.COMPLETED
    assert reopened.commanded_state == "open"
    assert reopened.reason_code is None


def test_ros_gripper_grasp_loss_delegates_to_backend_status_sink():
    if not HAS_NATIVE_CLIENT:
        pytest.skip("native_kernel_client not available")

    class FakeRosBundle:
        def __init__(self):
            self.mark_calls = []
            self.status = {
                "state": "completed",
                "progress": 1.0,
                "current_position": 0.0,
                "target_position": 0.0,
                "evidence": ["driver_result"],
                "evidence_state": "confirmed",
            }

        def command_ros_gripper(
            self,
            gripper_id,
            target_position,
            duration_s,
            max_effort,
        ):
            assert gripper_id == "primary"
            self.status = {
                "state": "completed",
                "progress": 1.0,
                "current_position": target_position,
                "target_position": target_position,
                "evidence": ["driver_result"],
                "evidence_state": "confirmed",
            }
            return {"accepted": True}

        def get_ros_gripper_status(self, gripper_id):
            assert gripper_id == "primary"
            return dict(self.status)

        def mark_ros_gripper_grasp_lost(
            self,
            gripper_id,
            evidence_label,
            message,
            reason_code,
        ):
            self.mark_calls.append(
                (gripper_id, evidence_label, message, reason_code)
            )
            self.status = {
                **self.status,
                "state": "failed",
                "evidence": ["driver_result", evidence_label],
                "evidence_state": "pressure_or_part_lost",
                "error": message,
                "reason_code": reason_code,
            }
            return {"accepted": True}

    bundle = FakeRosBundle()
    kernel = NativeKernelClient(bundle)
    close = kernel.command_gripper(
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
            srdf_presets={"open": 0.0081, "closed": 0.0},
        )
    )
    assert close.accepted is True, close.error

    lost = kernel.mark_gripper_grasp_lost(
        gripper_id="primary",
        evidence_label="attached_drift_monitor",
        message="measured object drift exceeded threshold",
    )
    assert lost.accepted is True, lost.error
    assert bundle.mark_calls == [
        (
            "primary",
            "attached_drift_monitor",
            "measured object drift exceeded threshold",
            "grasp_confirmation_lost",
        )
    ]
    status = kernel.get_gripper_status("primary")
    assert status.state == GripperCommandState.FAILED
    assert status.evidence_state == "pressure_or_part_lost"
    assert status.reason_code == "grasp_confirmation_lost"
    assert "attached_drift_monitor" in status.evidence


def test_create_native_kernel_rejects_stop_profile_deceleration_time_over_cap():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    with pytest.raises(ValueError, match="deceleration_time_s must be <= 2.0"):
        manyforge_roboplan_py.create_native_kernel(
            "ur5_gripper",
            "arm",
            {
                "operator_stop_profile": {
                    "profile_id": "operator_soft_too_slow",
                    "stop_mode": "soft_on_path",
                    "deceleration_time_s": 2.001,
                    "max_stop_time_s": 2.0,
                    "per_joint_max_deceleration_rad_s2": {
                        "shoulder_pan_joint": 8.0,
                    },
                }
            },
        )


def test_create_native_kernel_accepts_stop_profile_deceleration_time_at_cap():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    bundle = manyforge_roboplan_py.create_native_kernel(
        "ur5_gripper",
        "arm",
        {
            "operator_stop_profile": {
                "profile_id": "operator_spring_cap",
                "stop_mode": "spring_back_hold",
                "deceleration_time_s": 2.0,
                "per_joint_max_deceleration_rad_s2": {
                    "shoulder_pan_joint": 8.0,
                },
            }
        },
    )
    capability = bundle.get_motion_interrupt_stop_capability()
    assert "spring_back_hold" in capability["supported_stop_modes"]


def test_create_native_kernel_rejects_spring_back_deceleration_time_over_cap():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    with pytest.raises(ValueError, match="deceleration_time_s must be <= 2.0"):
        manyforge_roboplan_py.create_native_kernel(
            "ur5_gripper",
            "arm",
            {
                "operator_stop_profile": {
                    "profile_id": "operator_spring_too_slow",
                    "stop_mode": "spring_back_hold",
                    "deceleration_time_s": 2.001,
                }
            },
        )


def test_create_native_kernel_rejects_non_finite_per_joint_deceleration_limit():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    with pytest.raises(ValueError, match="finite positive values"):
        manyforge_roboplan_py.create_native_kernel(
            "ur5_gripper",
            "arm",
            {
                "operator_stop_profile": {
                    "profile_id": "operator_soft_nan_joint_limit",
                    "stop_mode": "soft_on_path",
                    "deceleration_time_s": 0.5,
                    "max_stop_time_s": 1.0,
                    "per_joint_max_deceleration_rad_s2": {
                        "shoulder_pan_joint": math.nan,
                    },
                }
            },
        )


def test_create_native_kernel_rejects_unknown_stop_profile_fallback_mode():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    with pytest.raises(ValueError, match="allowed_fallback_modes"):
        manyforge_roboplan_py.create_native_kernel(
            "ur5_gripper",
            "arm",
            {
                "operator_stop_profile": {
                    "profile_id": "operator_soft_bad_fallback",
                    "stop_mode": "soft_on_path",
                    "deceleration_time_s": 0.5,
                    "max_stop_time_s": 1.0,
                    "per_joint_max_deceleration_rad_s2": {
                        "shoulder_pan_joint": 8.0,
                    },
                    "allowed_fallback_modes": ["not_a_stop_mode"],
                }
            },
        )


def test_native_kernel_stop_capability_binding_reports_audit_fields():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    capability = bundle.get_motion_interrupt_stop_capability()

    assert capability["supported_stop_modes"] == [
        "spring_back_hold",
        "soft_on_path",
    ]
    assert capability["capability_profile_id"] == "roboplan_native_v2_soft_on_path"
    assert capability["hard_stop_level"] == "native_immediate_hold"
    assert capability["derivative_proof_level"] == "manyforge_core_native_path_ramp"
    assert capability["scaling_source_visibility"] == "native_runtime_scale"
    assert capability["scaling_source_probe_status"] == "not_applicable"
    assert (
        capability["unsupported_reasons"]["hard_deceleration"]
        == "ros2_control_jtc_only_in_current_native_backend"
    )


def test_native_kernel_client_forwards_stop_capability_for_composer_gate():
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")

    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    kernel = NativeKernelClient(bundle)

    capability = kernel.get_motion_interrupt_stop_capability()

    assert capability["supported_stop_modes"] == [
        "spring_back_hold",
        "soft_on_path",
    ]
    assert capability["capability_profile_id"] == "roboplan_native_v2_soft_on_path"
    assert capability["hard_stop_level"] == "native_immediate_hold"


def test_native_state_mapping_uses_stable_state_name_only():
    if not HAS_NATIVE_CLIENT:
        pytest.skip("native_kernel_client not available")

    assert _state_from_native({"state": 12345, "state_name": "Completed"}) == (
        ExecutionState.COMPLETED
    )
    assert _state_from_native({"state": 12345, "state_name": "Failed"}) == (
        ExecutionState.FAILED
    )
    assert _state_from_native({"state": 12345, "state_name": "Interrupted"}) == (
        ExecutionState.INTERRUPTED
    )
    with pytest.raises(ValueError, match="missing required stable state_name"):
        _state_from_native({"state": 9})
    with pytest.raises(ValueError, match="unknown native kernel state_name"):
        _state_from_native({"state": 9, "state_name": "Complete-ish"})


def test_native_gripper_state_mapping_rejects_unknown_or_missing_state():
    if not HAS_NATIVE_CLIENT:
        pytest.skip("native_kernel_client not available")

    assert _gripper_state_from_native({"state": "idle"}) == GripperCommandState.IDLE
    assert (
        _gripper_state_from_native({"state": "executing"})
        == GripperCommandState.EXECUTING
    )
    assert (
        _gripper_state_from_native({"state": "completed"})
        == GripperCommandState.COMPLETED
    )
    assert _gripper_state_from_native({"state": "failed"}) == GripperCommandState.FAILED
    with pytest.raises(ValueError, match="missing required state"):
        _gripper_state_from_native({})
    with pytest.raises(ValueError, match="unknown native gripper state"):
        _gripper_state_from_native({"state": "interrupted"})


def make_request(move_id, goal, cache_mode="default"):
    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=UR5_JOINTS,
        joint_goal_positions_rad=goal,
        velocity_scale=1.0,
        acceleration_scale=1.0,
        planning_timeout_s=10.0,
        execution_timeout_s=30.0,
        cache_mode=cache_mode,
    )


def make_request_for_joints(
    move_id,
    joint_names,
    goal,
    cache_mode="default",
    max_tcp_linear_speed_mps=None,
):
    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=joint_names,
        joint_goal_positions_rad=goal,
        velocity_scale=1.0,
        acceleration_scale=1.0,
        max_tcp_linear_speed_mps=max_tcp_linear_speed_mps,
        planning_timeout_s=10.0,
        execution_timeout_s=30.0,
        cache_mode=cache_mode,
    )


def make_ur10e_joint_preview_request(move_id, goal):
    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="grasp_frame",
        motion_type="joint_goal",
        joint_goal_names=UR10E_ARM_JOINTS,
        joint_goal_positions_rad=goal,
        velocity_scale=1.0,
        acceleration_scale=1.0,
        planning_timeout_s=5.0,
        execution_timeout_s=10.0,
        cache_mode="disable",
    )


def make_relative_request(
    move_id,
    object_id,
    offset_position_m,
    offset_orientation_rpy_deg,
    cache_mode="disable",
):
    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="relative_move",
        relative_move={
            "target_reference": {"type": "object", "id": object_id},
            "offset": {
                "position_m": list(offset_position_m),
                "orientation_rpy_deg": list(offset_orientation_rpy_deg),
            },
        },
        velocity_scale=1.0,
        acceleration_scale=1.0,
        planning_timeout_s=10.0,
        execution_timeout_s=30.0,
        cache_mode=cache_mode,
    )


def make_relative_reference_request(
    move_id,
    reference,
    offset_position_m,
    offset_orientation_rpy_deg,
    offset_reference=None,
    cache_mode="disable",
):
    relative_move = {
        "target_reference": reference,
        "offset": {
            "position_m": list(offset_position_m),
            "orientation_rpy_deg": list(offset_orientation_rpy_deg),
        },
    }
    if offset_reference is not None:
        relative_move["offset_reference"] = offset_reference

    return ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="relative_move",
        relative_move=relative_move,
        velocity_scale=1.0,
        acceleration_scale=1.0,
        planning_timeout_s=10.0,
        execution_timeout_s=30.0,
        cache_mode=cache_mode,
    )


def rotate_vector_by_quaternion(vector, quaternion_xyzw):
    x, y, z, w = quaternion_xyzw
    vx, vy, vz = vector
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return [
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    ]


def wait_for_terminal(native_kernel, timeout_s=15.0):
    deadline = time.monotonic() + timeout_s
    last_status = native_kernel.get_status()
    while time.monotonic() < deadline:
        last_status = native_kernel.get_status()
        if last_status.state in {
            ExecutionState.COMPLETED,
            ExecutionState.FAILED,
            ExecutionState.STOPPED,
        }:
            return last_status
        time.sleep(0.01)
    pytest.fail(
        f"Timed out waiting for terminal state; last status was {last_status.state}"
    )


def wait_for_non_idle(native_kernel, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    last_status = native_kernel.get_status()
    while time.monotonic() < deadline:
        last_status = native_kernel.get_status()
        if last_status.state != ExecutionState.IDLE:
            return last_status
        time.sleep(0.005)
    pytest.fail("Timed out waiting for execution to leave IDLE")


def get_cached_duration_s(native_kernel, move_id):
    traj = native_kernel.get_cached_trajectory(move_id)
    assert traj is not None, f"No cached trajectory for move_id={move_id}"
    assert traj.waypoints, f"Cached trajectory for move_id={move_id} has no waypoints"
    return traj.waypoints[-1].time_from_start_s


def reset_to_home(native_kernel):
    native_kernel.reset()
    native_kernel._bundle.set_joint_positions([0.0] * len(UR5_JOINTS))


def reset_to_demo_rest(native_kernel):
    native_kernel.reset()
    native_kernel._bundle.set_joint_positions(DEMO_REST)


# ============================================================================
# Native kernel: basic operations
# ============================================================================

class TestNativeKernelBasics:

    def test_initial_state_idle(self, native_kernel):
        status = native_kernel.get_status()
        assert status.state == ExecutionState.IDLE

    def test_execute_joint_goal(self, native_kernel):
        result = native_kernel.execute(make_request("native_basic", GOAL_A))
        assert result.ok is True
        assert result.execution_id > 0
        wait_for_terminal(native_kernel)

    def test_execute_accepts_explicit_null_optional_correlation_fields(
        self, native_kernel
    ):
        result = native_kernel._bundle.execute({
            "robot_id": "robot1",
            "group_id": "arm",
            "move_id": "native_null_optional_correlation",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
            "motion_type": "joint_goal",
            "joint_goal_names": UR5_JOINTS,
            "joint_goal_positions_rad": GOAL_A,
            "velocity_scale": 1.0,
            "acceleration_scale": 1.0,
            "planning_timeout_s": 10.0,
            "execution_timeout_s": 30.0,
            "cache_mode": "default",
            "program_run_id": None,
            "action_span": {
                "action_source": None,
                "action_span_id": None,
                "program_run_id": None,
                "node_instance_id": None,
                "node_attempt_id": None,
                "parent_node_attempt_id": None,
            },
            "contact_policy_id": None,
        })
        assert result["ok"] is True
        wait_for_terminal(native_kernel)

    def test_state_after_execute(self, native_kernel):
        native_kernel.execute(make_request("native_state", GOAL_A))
        status = wait_for_non_idle(native_kernel)
        assert status.active_move_id == "native_state"
        assert status.execution_id > 0

    def test_execution_snapshot_exposes_ids_and_progress(self, native_kernel):
        result = native_kernel.execute(make_request("native_snapshot", GOAL_A))
        assert result.ok is True
        snapshot = native_kernel.get_execution_snapshot()
        assert snapshot.execution_id == result.execution_id
        assert snapshot.move_id == "native_snapshot"
        assert snapshot.sequence_number > 0
        terminal = wait_for_terminal(native_kernel)
        final_snapshot = native_kernel.get_execution_snapshot()
        assert final_snapshot.execution_id == result.execution_id
        assert final_snapshot.move_id == "native_snapshot"
        assert final_snapshot.progress >= 0.0
        assert terminal.state in {
            ExecutionState.COMPLETED,
            ExecutionState.FAILED,
            ExecutionState.STOPPED,
        }

    def test_metrics_track_snapshot_publication_and_observer_lag(self, native_kernel):
        result = native_kernel.execute(make_request("native_metrics_snapshot", GOAL_A))
        assert result.ok is True
        wait_for_non_idle(native_kernel)
        time.sleep(0.05)
        native_kernel.get_execution_snapshot()
        wait_for_terminal(native_kernel)
        metrics = native_kernel.get_metrics()
        assert metrics["events_emitted"] > 0
        assert metrics["flight_recorder_records"] > 0
        assert metrics["snapshots_published"] > 0
        assert metrics["observer_lag_max_ns"] > 0
        assert metrics["scene_projection_successful_updates_total"] > 0
        assert metrics["scene_projection_misses_total"] >= 0
        assert metrics["scene_projection_lag_max_ns"] >= 0
        assert metrics["scene_projection_last_success_timestamp_ns"] > 0
        assert metrics["scene_projection_last_success_age_ns"] >= 0
        assert metrics["scene_projection_terminal_retries_scheduled_total"] >= 0
        assert metrics["scene_projection_terminal_retry_successes_total"] >= 0
        assert metrics["scene_projection_terminal_retry_exhausted_total"] >= 0

    def test_set_joint_positions_refreshes_idle_execution_snapshot(self, native_kernel):
        native_kernel._bundle.set_joint_positions(DEMO_REST)
        snapshot = native_kernel.get_execution_snapshot()
        assert snapshot.state == ExecutionState.IDLE
        assert snapshot.joint_positions_rad == pytest.approx(DEMO_REST)

    def test_custom_ur10e_bundle_reports_arm_group_state(self, native_kernel_ur10e_robotiq):
        assert native_kernel_ur10e_robotiq.get_joint_names() == UR10E_ARM_JOINTS

        native_kernel_ur10e_robotiq._bundle.set_joint_positions(DEMO_REST)
        snapshot = native_kernel_ur10e_robotiq.get_execution_snapshot()

        assert snapshot.state == ExecutionState.IDLE
        assert snapshot.joint_positions_rad == pytest.approx(DEMO_REST)

    def test_custom_ur10e_bundle_reports_gripper_group_state(self, native_kernel_ur10e_robotiq):
        joint_names = native_kernel_ur10e_robotiq._bundle.get_group_joint_names("gripper")
        positions = native_kernel_ur10e_robotiq._bundle.get_group_joint_positions("gripper")

        assert joint_names == ["finger_joint"]
        assert positions == pytest.approx([0.0])

    def test_custom_ur10e_bundle_commands_gripper(self, native_kernel_ur10e_robotiq):
        result = native_kernel_ur10e_robotiq.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.7,
                duration_s=0.02,
            )
        )
        assert result.accepted is True, result.error

        deadline = time.monotonic() + 1.0
        native_kernel_ur10e_robotiq.update_gripper_commands()
        status = native_kernel_ur10e_robotiq.get_gripper_status("primary")
        while status.state == GripperCommandState.EXECUTING and time.monotonic() < deadline:
            time.sleep(0.01)
            native_kernel_ur10e_robotiq.update_gripper_commands()
            status = native_kernel_ur10e_robotiq.get_gripper_status("primary")

        assert status.state == GripperCommandState.COMPLETED
        assert status.current_position == pytest.approx(0.7, abs=1e-3)
        assert native_kernel_ur10e_robotiq._bundle.get_group_joint_positions("gripper") == pytest.approx([0.7], abs=1e-3)

    def test_custom_ur10e_gripper_status_read_is_side_effect_free(
        self, native_kernel_ur10e_robotiq
    ):
        result = native_kernel_ur10e_robotiq.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.7,
                duration_s=0.02,
            )
        )
        assert result.accepted is True, result.error

        time.sleep(0.03)
        status = native_kernel_ur10e_robotiq.get_gripper_status("primary")

        assert status.state == GripperCommandState.EXECUTING
        assert native_kernel_ur10e_robotiq._bundle.get_group_joint_positions("gripper") == pytest.approx([0.0])

        native_kernel_ur10e_robotiq.update_gripper_commands()
        completed = native_kernel_ur10e_robotiq.get_gripper_status("primary")

        assert completed.state == GripperCommandState.COMPLETED
        assert native_kernel_ur10e_robotiq._bundle.get_group_joint_positions("gripper") == pytest.approx([0.7], abs=1e-3)

    def test_custom_ur10e_bundle_executes_speed_limited_joint_goal(
        self, native_kernel_ur10e_robotiq
    ):
        native_kernel_ur10e_robotiq._bundle.set_joint_positions(DEMO_REST)

        result = native_kernel_ur10e_robotiq.execute(
            make_request_for_joints(
                "ur10e_speed_limited",
                UR10E_ARM_JOINTS,
                GOAL_A,
                cache_mode="disable",
                max_tcp_linear_speed_mps=0.25,
            )
        )

        assert result.ok is True, result.error
        terminal = wait_for_terminal(native_kernel_ur10e_robotiq)
        assert terminal.state == ExecutionState.COMPLETED

    def test_reset_to_idle(self, native_kernel):
        native_kernel.execute(make_request("native_reset", GOAL_A))
        wait_for_terminal(native_kernel)
        result = native_kernel.reset()
        assert result.accepted is True
        assert native_kernel.get_status().state == ExecutionState.IDLE

    def test_execute_reset_cycle(self, native_kernel):
        for i in range(5):
            result = native_kernel.execute(
                make_request(f"native_cycle_{i}", GOAL_A, cache_mode="disable"))
            assert result.ok is True, f"Cycle {i} failed: {result.error}"
            wait_for_terminal(native_kernel)
            native_kernel.reset()

    def test_bound_acknowledgement_required_for_sequential_execute(self, native_kernel):
        """Second execute requires exact acknowledgement of Completed execution."""
        r1 = native_kernel.execute(make_request("native_auto_1", GOAL_A))
        assert r1.ok is True
        wait_for_terminal(native_kernel)

        r2 = native_kernel.execute(make_request("native_auto_2", GOAL_B))
        assert r2.ok is False

        stale_ack = native_kernel.acknowledge_completed(r1.execution_id + 1)
        assert stale_ack.accepted is False
        assert native_kernel.get_status().state == ExecutionState.COMPLETED

        ack = native_kernel.acknowledge_completed(r1.execution_id)
        assert ack.accepted is True
        assert native_kernel.get_status().state == ExecutionState.IDLE

        r2 = native_kernel.execute(make_request("native_auto_2", GOAL_B))
        assert r2.ok is True
        wait_for_terminal(native_kernel)


# ============================================================================
# Native kernel: scene edits
# ============================================================================

class TestNativeSceneEdits:

    def test_add_box(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="box1", shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

    def test_update_pose_reports_geometry_freshness_and_noop_status(self, native_kernel):
        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="fresh_box", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert add_result.success is True

        first = native_kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id="fresh_box",
            pose_position_m=[0.6, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
            source_id="vision",
            source_timestamp_ns=123_000_000,
            source_freshness_budget_ns=50_000_000,
        ))
        assert first.success is True
        assert first.update_status == "geometry_changed"
        assert first.collision_world_revision_changed is True

        heartbeat = native_kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id="fresh_box",
            pose_position_m=[0.6, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
            source_id="vision",
            source_timestamp_ns=124_000_000,
            source_freshness_budget_ns=50_000_000,
        ))
        assert heartbeat.success is True
        assert heartbeat.update_status == "freshness_only"
        assert heartbeat.collision_world_revision_changed is False
        assert heartbeat.collision_world_freshness_changed is True

        duplicate = native_kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id="fresh_box",
            pose_position_m=[0.6, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
            source_id="vision",
            source_timestamp_ns=124_000_000,
            source_freshness_budget_ns=50_000_000,
        ))
        assert duplicate.success is True
        assert duplicate.update_status == "no_op"
        assert duplicate.collision_world_revision_changed is False
        assert duplicate.collision_world_freshness_changed is False
        counters = native_kernel.scene_update_status_counters()
        assert counters["geometry_changed"] == 1
        assert counters["freshness_only"] == 1
        assert counters["no_op"] == 1
        assert counters["runtime_bundle_published"] >= 3

    def test_add_sphere(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="sphere1", shape_type="sphere",
            sphere_radius_m=0.05,
            pose_position_m=[0.3, 0.1, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

    def test_add_cylinder(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="cylinder1", shape_type="cylinder",
            cylinder_radius_m=0.05,
            cylinder_height_m=0.2,
            pose_position_m=[0.3, -0.2, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

    def test_upsert_replaces_existing_object(self, native_kernel):
        insert = native_kernel.scene_edit(SceneEditRequest(
            operation="upsert", object_id="upsert_box", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert insert.success is True
        replace = native_kernel.scene_edit(SceneEditRequest(
            operation="upsert", object_id="upsert_box", shape_type="sphere",
            sphere_radius_m=0.04,
            pose_position_m=[0.4, 0.1, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert replace.success is True
        state = native_kernel.get_object_state("upsert_box")
        assert state.exists is True

    def test_add_remove(self, native_kernel):
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="temp", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="remove", object_id="temp",
        ))
        assert result.success is True

    def test_scene_edit_then_motion(self, native_kernel):
        """Add a small obstacle far from the goal — planner should still succeed."""
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="obstacle", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.8, 0.8, 0.8],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        result = native_kernel.execute(make_request("native_obstacle", GOAL_A))
        assert result.ok is True
        wait_for_terminal(native_kernel)

    def test_get_link_pose(self, native_kernel):
        pose = native_kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert "position_m" in pose
        assert "orientation_xyzw" in pose
        assert len(pose["position_m"]) == 3
        assert len(pose["orientation_xyzw"]) == 4

    def test_get_link_poses_matches_single_fk(self, native_kernel):
        target_frames = ["base_link", "tool0"]
        batch = native_kernel.get_link_poses(target_frames, "universe")

        assert set(batch.keys()) == set(target_frames)
        for frame_name in target_frames:
            single = native_kernel.get_link_pose(frame_name, "universe")
            assert single is not None
            assert batch[frame_name]["position_m"] == pytest.approx(
                single["position_m"], abs=1e-9
            )
            assert batch[frame_name]["orientation_xyzw"] == pytest.approx(
                single["orientation_xyzw"], abs=1e-9
            )

    def test_get_object_pose_and_state(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="query_box", shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.45, -0.1, 0.55],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

        pose = native_kernel.get_object_pose("query_box", "universe")
        assert pose is not None
        assert pose["position_m"] == [0.45, -0.1, 0.55]
        assert pose["orientation_xyzw"] == [0.0, 0.0, 0.0, 1.0]

        state = native_kernel.get_object_state("query_box")
        assert state.object_id == "query_box"
        assert state.exists is True
        assert state.attached is False
        assert state.frame_id == "universe"

    def test_scene_object_enumeration_exposes_shape_and_joint_names(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="enumerated_box", shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.45, -0.1, 0.55],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True

        object_ids = native_kernel.get_scene_object_ids()
        assert "enumerated_box" in object_ids

        joint_names = native_kernel.get_joint_names()
        assert joint_names == UR5_JOINTS

        objects = native_kernel.get_scene_objects()
        enumerated = next(obj for obj in objects if obj.object_id == "enumerated_box")
        assert enumerated.shape is not None
        assert enumerated.shape.type == "box"
        assert enumerated.shape.box_dimensions_m == pytest.approx([0.1, 0.2, 0.3])
        assert enumerated.pose_in_universe is not None
        assert enumerated.pose_in_universe["position_m"] == pytest.approx([0.45, -0.1, 0.55])

    def test_invalid_scene_edit_operation_returns_error(self, native_kernel):
        result = native_kernel.scene_edit(SceneEditRequest(
            operation="teleport",
            object_id="query_box",
        ))
        assert result.success is False
        assert "Unsupported scene edit operation" in (result.error or "")

    def test_attached_frame_object_pose_follows_backend_link_frame(self, native_kernel):
        link_pose = native_kernel.get_link_pose("tool0", "universe")
        assert link_pose is not None

        result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="query_box_attached", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="tool0",
        ))
        assert result.success is True

        pose = native_kernel.get_object_pose("query_box_attached", "universe")
        assert pose is not None
        for actual, expected in zip(pose["position_m"], link_pose["position_m"]):
            assert actual == pytest.approx(expected, abs=1e-6)

        state = native_kernel.get_object_state("query_box_attached")
        assert state.exists is True
        assert state.attached is True
        assert state.frame_id == "tool0"

    def test_scene_edit_attach_preserves_world_pose_with_stable_object_id(self, native_kernel):
        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="attach_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.42, -0.08, 0.52],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert add_result.success is True
        pose_before = native_kernel.get_object_pose("attach_box", "universe")
        assert pose_before is not None

        attach_result = native_kernel.scene_edit(SceneEditRequest(
            operation="attach", object_id="attach_box", frame_id="tool0"
        ))
        assert attach_result.success is True

        state = native_kernel.get_object_state("attach_box")
        assert state.exists is True
        assert state.attached is True
        assert state.frame_id == "tool0"
        pose_after = native_kernel.get_object_pose("attach_box", "universe")
        assert pose_after is not None
        for actual, expected in zip(pose_after["position_m"], pose_before["position_m"]):
            assert actual == pytest.approx(expected, abs=1e-6)

    def test_scene_edit_detach_preserves_world_pose_with_stable_object_id(self, native_kernel):
        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="detach_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.01, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="tool0",
        ))
        assert add_result.success is True
        pose_before = native_kernel.get_object_pose("detach_box", "universe")
        assert pose_before is not None

        detach_result = native_kernel.scene_edit(SceneEditRequest(
            operation="detach", object_id="detach_box", frame_id="universe"
        ))
        assert detach_result.success is True

        state = native_kernel.get_object_state("detach_box")
        assert state.exists is True
        assert state.attached is False
        assert state.frame_id == "universe"
        pose_after = native_kernel.get_object_pose("detach_box", "universe")
        assert pose_after is not None
        for actual, expected in zip(pose_after["position_m"], pose_before["position_m"]):
            assert actual == pytest.approx(expected, abs=1e-6)

    def test_scene_trace_reports_backend_scene_mutations(self, native_kernel):
        native_kernel.clear_scene_trace()
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="trace_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.42, -0.08, 0.52],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        native_kernel.scene_edit(SceneEditRequest(
            operation="attach", object_id="trace_box", frame_id="tool0"
        ))
        trace = native_kernel.get_scene_trace()
        assert [event["event"] for event in trace][-2:] == ["add", "attach"]
        assert trace[-1]["source"] == "backend"
        assert trace[-1]["success"] is True
        assert trace[-1]["frame_id"] == "tool0"
        assert trace[-1]["pose_in_frame"] is not None
        assert trace[-1]["pose_in_universe"] is not None

    def test_relative_move_resolves_object_reference_at_execute_time(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="ref_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.3, -0.3, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))

        result = native_kernel.execute(
            make_relative_request(
                "native_relative_object",
                "ref_box",
                [0.0, 0.0, 0.10],
                [180.0, 0.0, -90.0],
            )
        )
        assert result.ok is True
        terminal = wait_for_terminal(native_kernel)
        assert terminal.state == ExecutionState.COMPLETED

        pose = native_kernel.get_link_pose("tool0", "universe")
        assert pose is not None
        assert pose["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert pose["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert pose["position_m"][2] == pytest.approx(0.25, abs=0.03)

    def test_relative_move_resolves_object_reference_for_preview_motion(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="preview_ref_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.3, -0.3, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))
        assert add_result.success is True, add_result.error

        preview = native_kernel.preview_motion(
            make_relative_request(
                "native_relative_preview_object",
                "preview_ref_box",
                [0.0, 0.0, 0.10],
                [180.0, 0.0, -90.0],
            )
        )

        assert preview.ok is True, preview.error
        assert preview.final_tcp_pose is not None
        assert preview.final_tcp_pose["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert preview.final_tcp_pose["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert preview.final_tcp_pose["position_m"][2] == pytest.approx(0.25, abs=0.03)

    def test_relative_move_resolves_object_reference_for_plan_motion(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="plan_ref_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.3, -0.3, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))
        assert add_result.success is True, add_result.error

        preview, trajectory = native_kernel.plan_motion(
            make_relative_request(
                "native_relative_plan_object",
                "plan_ref_box",
                [0.0, 0.0, 0.10],
                [180.0, 0.0, -90.0],
            )
        )

        assert preview.ok is True, preview.error
        assert trajectory is not None
        assert trajectory.waypoints
        assert preview.final_tcp_pose is not None
        assert preview.final_tcp_pose["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert preview.final_tcp_pose["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert preview.final_tcp_pose["position_m"][2] == pytest.approx(0.25, abs=0.03)

    def test_relative_move_recomputes_goal_after_object_pose_changes(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="dynamic_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.3, -0.3, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))

        req = make_relative_request(
            "native_relative_dynamic",
            "dynamic_box",
            [0.0, 0.0, 0.10],
            [180.0, 0.0, -90.0],
            cache_mode="default",
        )
        result = native_kernel.execute(req)
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED
        pose_a = native_kernel.get_link_pose("tool0", "universe")
        assert pose_a is not None

        reset_to_demo_rest(native_kernel)
        native_kernel.scene_edit(SceneEditRequest(
            operation="update_pose", object_id="dynamic_box",
            pose_position_m=[0.38, -0.18, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))
        result = native_kernel.execute(req)
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED
        pose_b = native_kernel.get_link_pose("tool0", "universe")
        assert pose_b is not None

        assert pose_a["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert pose_a["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert pose_b["position_m"][0] == pytest.approx(0.38, abs=0.03)
        assert pose_b["position_m"][1] == pytest.approx(-0.18, abs=0.03)

    def test_relative_move_resolves_robot_link_reference_at_execute_time(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        start_pose = native_kernel.get_link_pose("tool0", "base_link")
        assert start_pose is not None
        expected_delta = rotate_vector_by_quaternion(
            [0.0, 0.0, 0.05], start_pose["orientation_xyzw"]
        )

        result = native_kernel.execute(
            make_relative_reference_request(
                "native_relative_tcp",
                {"type": "robot_link", "id": "tool0"},
                [0.0, 0.0, 0.05],
                [0.0, 0.0, 0.0],
            )
        )
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED

        final_pose = native_kernel.get_link_pose("tool0", "base_link")
        assert final_pose is not None
        for axis in range(3):
            assert final_pose["position_m"][axis] == pytest.approx(
                start_pose["position_m"][axis] + expected_delta[axis], abs=0.03
            )

    def test_relative_move_resolves_frame_reference_at_execute_time(self, native_kernel):
        reset_to_demo_rest(native_kernel)

        result = native_kernel.execute(
            make_relative_reference_request(
                "native_relative_frame",
                {"type": "frame", "id": "base_link"},
                [0.3, -0.3, 0.25],
                [180.0, 0.0, -90.0],
            )
        )
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED

        pose = native_kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert pose["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert pose["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert pose["position_m"][2] == pytest.approx(0.25, abs=0.03)

    def test_relative_move_resolves_pose_reference_at_execute_time(self, native_kernel):
        reset_to_demo_rest(native_kernel)

        result = native_kernel.execute(
            make_relative_reference_request(
                "native_relative_pose",
                {
                    "type": "pose",
                    "pose": {
                        "position_m": [0.3, -0.3, 0.15],
                        "orientation_rpy_deg": [0.0, 0.0, -45.0],
                    },
                },
                [0.0, 0.0, 0.10],
                [180.0, 0.0, -90.0],
            )
        )
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED

        pose = native_kernel.get_link_pose("tool0", "universe")
        assert pose is not None
        assert pose["position_m"][0] == pytest.approx(0.3, abs=0.03)
        assert pose["position_m"][1] == pytest.approx(-0.3, abs=0.03)
        assert pose["position_m"][2] == pytest.approx(0.25, abs=0.03)

    def test_relative_move_uses_independent_offset_reference_at_execute_time(self, native_kernel):
        reset_to_demo_rest(native_kernel)
        native_kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="dynamic_box", shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.30, -0.30, 0.15],
            pose_orientation_xyzw=[0.0, 0.0, -0.38268343, 0.92387953],
            frame_id="universe",
        ))

        result = native_kernel.execute(
            make_relative_reference_request(
                "native_relative_offset_basis",
                {"type": "object", "id": "dynamic_box"},
                [0.10, 0.0, 0.10],
                [180.0, 0.0, -90.0],
                offset_reference={"type": "frame", "id": "base_link"},
            )
        )
        assert result.ok is True
        assert wait_for_terminal(native_kernel).state == ExecutionState.COMPLETED

        pose = native_kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert pose["position_m"][0] == pytest.approx(0.40, abs=0.03)
        assert pose["position_m"][1] == pytest.approx(-0.30, abs=0.03)
        assert pose["position_m"][2] == pytest.approx(0.25, abs=0.03)


class TestNativeUr10eRobotiqAttachedObjectCollision:

    def _set_repro_robot_state(self, native_kernel):
        native_kernel._bundle.set_group_joint_positions(
            "arm",
            UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
        )
        native_kernel._bundle.set_group_joint_positions("gripper", [0.33])

    def _add_clear_grasped_box(
        self,
        native_kernel,
        object_id="attached_clear_box",
        support_contacts=None,
    ):
        self._set_repro_robot_state(native_kernel)
        grasp_pose = native_kernel.get_link_pose("grasp_frame", "universe")
        assert grasp_pose is not None

        object_offset_in_grasp = [0.0, 0.0, 0.1109473993]
        object_pos_world = [
            p + d for p, d in zip(
                grasp_pose["position_m"],
                rotate_vector_by_quaternion(
                    object_offset_in_grasp,
                    grasp_pose["orientation_xyzw"],
                ),
            )
        ]

        add_result = native_kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=object_id,
            shape_type="box",
            box_dimensions_m=[0.092770782, 0.184926749, 0.040091786],
            pose_position_m=object_pos_world,
            pose_orientation_xyzw=grasp_pose["orientation_xyzw"],
            frame_id="universe",
            support_contacts=support_contacts,
        ))
        assert add_result.success is True, add_result.error

        attach_result = native_kernel.scene_edit(SceneEditRequest(
            operation="attach",
            object_id=object_id,
            frame_id="grasp_frame",
            touch_links=ROBOTIQ_GRASP_TOUCH_LINKS,
        ))
        assert attach_result.success is True, attach_result.error
        return object_pos_world, grasp_pose["orientation_xyzw"]

    def test_attached_object_collision_pose_uses_named_frame_not_parent_joint(
        self,
        native_kernel_ur10e_robotiq,
    ):
        self._add_clear_grasped_box(native_kernel_ur10e_robotiq)

        comparison = native_kernel_ur10e_robotiq.collision_compare_positions(
            UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            waypoint_index=0,
        )
        assert comparison["ok"] is True
        assert comparison["manyforge_collision"]["decision"] == "pass", comparison
        assert comparison["manyforge_collision"]["in_collision"] is False, comparison
        assert comparison["decision_match"] is True, comparison

        preview = native_kernel_ur10e_robotiq.preview_motion(
            make_ur10e_joint_preview_request(
                "attached_object_named_frame_pose",
                UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            )
        )

        assert preview.ok is True, preview.error

    def test_attached_object_still_collides_with_world_obstacles_at_carried_pose(
        self,
        native_kernel_ur10e_robotiq,
    ):
        object_pos_world, object_orientation_world = self._add_clear_grasped_box(
            native_kernel_ur10e_robotiq,
            object_id="attached_overlap_box",
        )
        obstacle_result = native_kernel_ur10e_robotiq.scene_edit(SceneEditRequest(
            operation="add",
            object_id="attached_pose_obstacle",
            shape_type="box",
            box_dimensions_m=[0.04, 0.04, 0.04],
            pose_position_m=object_pos_world,
            pose_orientation_xyzw=object_orientation_world,
            frame_id="universe",
        ))
        assert obstacle_result.success is True, obstacle_result.error

        preview = native_kernel_ur10e_robotiq.preview_motion(
            make_ur10e_joint_preview_request(
                "attached_object_obstacle_pose",
                UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            )
        )

        assert preview.ok is False
        assert preview.error is not None
        assert "attached_pose_obstacle" in (
            preview.error + " " + str(preview.error_details)
        )

    def test_declared_support_contact_reaches_validation_and_requires_release(
        self,
        native_kernel_ur10e_robotiq,
    ):
        self._set_repro_robot_state(native_kernel_ur10e_robotiq)
        grasp_pose = native_kernel_ur10e_robotiq.get_link_pose("grasp_frame", "universe")
        assert grasp_pose is not None
        object_offset_in_grasp = [0.0, 0.0, 0.1109473993]
        support_pos_world = [
            p + d for p, d in zip(
                grasp_pose["position_m"],
                rotate_vector_by_quaternion(
                    object_offset_in_grasp,
                    grasp_pose["orientation_xyzw"],
                ),
            )
        ]
        support_pos_world[2] -= 0.035

        support_result = native_kernel_ur10e_robotiq.scene_edit(SceneEditRequest(
            operation="add",
            object_id="support_surface",
            shape_type="box",
            box_dimensions_m=[0.12, 0.20, 0.04],
            pose_position_m=support_pos_world,
            pose_orientation_xyzw=grasp_pose["orientation_xyzw"],
            frame_id="universe",
        ))
        assert support_result.success is True, support_result.error

        self._add_clear_grasped_box(
            native_kernel_ur10e_robotiq,
            object_id="supported_attached_box",
            support_contacts=[
                {
                    "support_object_id": "support_surface",
                    "max_start_penetration_m": 0.02,
                    "require_separation_by_end": True,
                }
            ],
        )

        preview = native_kernel_ur10e_robotiq.preview_motion(
            make_ur10e_joint_preview_request(
                "support_contact_requires_release",
                UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            )
        )

        assert preview.ok is False
        assert preview.error is not None
        assert "start state is already in collision" not in preview.error
        assert "Support-contact pair did not separate" in preview.error

    def test_dynamic_attached_support_contact_requires_release(
        self,
        native_kernel_ur10e_robotiq,
    ):
        self._set_repro_robot_state(native_kernel_ur10e_robotiq)
        grasp_pose = native_kernel_ur10e_robotiq.get_link_pose("grasp_frame", "universe")
        assert grasp_pose is not None
        object_offset_in_grasp = [0.0, 0.0, 0.1109473993]
        support_pos_world = [
            p + d for p, d in zip(
                grasp_pose["position_m"],
                rotate_vector_by_quaternion(
                    object_offset_in_grasp,
                    grasp_pose["orientation_xyzw"],
                ),
            )
        ]
        support_pos_world[2] -= 0.035

        support_result = native_kernel_ur10e_robotiq.scene_edit(SceneEditRequest(
            operation="add",
            object_id="dynamic_support_surface",
            shape_type="box",
            box_dimensions_m=[0.12, 0.20, 0.04],
            pose_position_m=support_pos_world,
            pose_orientation_xyzw=grasp_pose["orientation_xyzw"],
            frame_id="universe",
        ))
        assert support_result.success is True, support_result.error

        self._add_clear_grasped_box(
            native_kernel_ur10e_robotiq,
            object_id="dynamic_supported_attached_box",
        )

        preview = native_kernel_ur10e_robotiq.preview_motion(
            make_ur10e_joint_preview_request(
                "dynamic_support_contact_requires_release",
                UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            )
        )

        assert preview.ok is False
        assert preview.error is not None
        assert "start state is already in collision" not in preview.error
        assert "Support-contact pair did not separate" in preview.error
        assert "dynamic_support_surface" in (
            preview.error + " " + str(preview.error_details)
        )

    def test_detached_declared_support_contact_does_not_require_release(
        self,
        native_kernel_ur10e_robotiq,
    ):
        self._set_repro_robot_state(native_kernel_ur10e_robotiq)

        support_result = native_kernel_ur10e_robotiq.scene_edit(SceneEditRequest(
            operation="add",
            object_id="detached_support_surface",
            shape_type="box",
            box_dimensions_m=[0.30, 0.30, 0.05],
            pose_position_m=[0.65, 0.65, 0.025],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert support_result.success is True, support_result.error

        object_result = native_kernel_ur10e_robotiq.scene_edit(SceneEditRequest(
            operation="add",
            object_id="detached_supported_box",
            shape_type="box",
            box_dimensions_m=[0.10, 0.10, 0.08],
            pose_position_m=[0.65, 0.65, 0.04],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
            support_contacts=[
                {
                    "support_object_id": "detached_support_surface",
                    "max_start_penetration_m": 0.02,
                    "require_separation_by_end": True,
                }
            ],
        ))
        assert object_result.success is True, object_result.error

        preview = native_kernel_ur10e_robotiq.preview_motion(
            make_ur10e_joint_preview_request(
                "detached_support_contact_ignored_for_robot_move",
                UR10E_ROBOTIQ_ATTACHED_OBJECT_REPRO_Q,
            )
        )

        assert preview.ok is True, preview.error


# ============================================================================
# Native kernel: caching
# ============================================================================

class TestNativeCaching:

    def test_cache_hit(self, native_kernel):
        req = make_request("native_cache_hit", GOAL_A)
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        # Second execute should hit cache
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)

    def test_cache_disabled(self, native_kernel):
        req = make_request("native_nocache", GOAL_A, cache_mode="disable")
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        native_kernel.reset()
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)

    def test_invalidate_trajectories(self, native_kernel):
        native_kernel.execute(make_request("native_inval", GOAL_A))
        wait_for_terminal(native_kernel)
        native_kernel.reset()
        native_kernel.invalidate_trajectories()
        # Should still work after invalidation (plans fresh)
        r = native_kernel.execute(make_request("native_inval", GOAL_A))
        assert r.ok is True
        wait_for_terminal(native_kernel)


# ============================================================================
# Native kernel: benchmarks
# ============================================================================

class TestNativeBenchmark:

    def test_single_plan_timing(self, native_kernel):
        """Single uncached plan timing."""
        num_trials = 3

        # Warmup
        native_kernel.execute(make_request("warmup", GOAL_A))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        times = []
        for i in range(num_trials):
            req = make_request(f"native_{i}", GOAL_A, cache_mode="disable")
            start = time.perf_counter()
            r = native_kernel.execute(req)
            wait_for_terminal(native_kernel)
            times.append(time.perf_counter() - start)
            assert r.ok is True, f"Trial {i} failed: {r.error}"
            native_kernel.reset()

        avg = sum(times) / len(times) * 1000
        print(f"\n=== Single Plan ({num_trials} trials) ===")
        print(f"  Native C++: {avg:.1f}ms avg")

    def test_cached_replay_timing(self, native_kernel):
        """Cached replay timing."""
        native_kernel.execute(make_request("n_cache", GOAL_B))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        req = make_request("n_cache", GOAL_B)
        start = time.perf_counter()
        native_kernel.execute(req)
        wait_for_terminal(native_kernel)
        t = time.perf_counter() - start

        print(f"\n=== Cached Replay ===")
        print(f"  Native C++: {t*1000:.2f}ms")

    def test_five_moves_timing(self, native_kernel):
        """5 sequential moves timing."""
        goals = [
            [0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            [0.3, -0.8, 0.8, -0.3, -0.8, 0.1],
            [-0.2, -1.0, 1.0, -0.5, -1.0, 0.0],
            [0.5, -0.6, 0.6, -0.6, -0.6, 0.2],
            [0.0, -0.7, 0.7, -0.7, -0.7, 0.0],
        ]

        # Warmup
        native_kernel.execute(make_request("warmup5", goals[0]))
        wait_for_terminal(native_kernel)
        native_kernel.reset()

        start = time.perf_counter()
        for i, goal in enumerate(goals):
            r = native_kernel.execute(
                make_request(f"n5_{i}", goal, cache_mode="disable")
            )
            assert r.ok is True, f"Move {i} failed: {r.error}"
            wait_for_terminal(native_kernel)
            native_kernel.reset()
        elapsed = time.perf_counter() - start

        n = len(goals)
        print(f"\n=== {n} Moves (no cache) ===")
        print(f"  Native C++: {elapsed*1000:.0f}ms total, {elapsed/n*1000:.1f}ms/move")


# ============================================================================
# Native kernel: TCP speed enforcement
# ============================================================================

class TestTcpSpeedEnforcement:

    def _make_speed_limited_request(self, move_id, goal, tcp_speed_mps,
                                      cache_mode="default"):
        return ExecuteRequest(
            robot_id="robot1",
            group_id="arm",
            move_id=move_id,
            base_frame="base_link",
            tcp_frame="tool0",
            motion_type="joint_goal",
            joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=goal,
            velocity_scale=1.0,
            acceleration_scale=1.0,
            max_tcp_linear_speed_mps=tcp_speed_mps,
            planning_timeout_s=10.0,
            execution_timeout_s=30.0,
            cache_mode=cache_mode,
        )

    def test_speed_limited_succeeds(self, native_kernel):
        """Plan with TCP speed limit succeeds."""
        req = self._make_speed_limited_request("tcp_limit", GOAL_A, 0.25)
        result = native_kernel.execute(req)
        assert result.ok is True
        wait_for_terminal(native_kernel)

    def test_speed_limited_slower_than_unlimited(self, native_kernel):
        """Speed-limited trajectory takes longer than unlimited."""
        # Unlimited (use default cache so we can read trajectory duration)
        r1 = native_kernel.execute(make_request("tcp_fast", GOAL_A))
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        t_unlimited = get_cached_duration_s(native_kernel, "tcp_fast")
        assert t_unlimited > 0
        reset_to_home(native_kernel)

        # Speed-limited (0.10 m/s is very restrictive)
        req = self._make_speed_limited_request("tcp_slow", GOAL_A, 0.10)
        r2 = native_kernel.execute(req)
        assert r2.ok is True
        wait_for_terminal(native_kernel)
        t_limited = get_cached_duration_s(native_kernel, "tcp_slow")
        assert t_limited > 0

        print(f"\n=== TCP Speed Enforcement ===")
        print(f"  Unlimited traj duration: {t_unlimited:.3f}s")
        print(f"  Limited (0.10 m/s) traj duration: {t_limited:.3f}s")
        assert t_limited > t_unlimited, \
            f"Limited trajectory ({t_limited:.3f}s) should be slower than unlimited ({t_unlimited:.3f}s)"

    def test_no_limit_when_zero(self, native_kernel):
        """TCP speed = 0 means no limit (same as no field)."""
        req = self._make_speed_limited_request("tcp_zero_a", GOAL_A, 0.0)
        r1 = native_kernel.execute(req)
        assert r1.ok is True
        wait_for_terminal(native_kernel)
        t_zero = get_cached_duration_s(native_kernel, "tcp_zero_a")
        reset_to_home(native_kernel)

        r2 = native_kernel.execute(make_request("tcp_none_a", GOAL_A))
        assert r2.ok is True
        wait_for_terminal(native_kernel)
        t_none = get_cached_duration_s(native_kernel, "tcp_none_a")

        # Should be approximately the same (both unlimited)
        assert abs(t_zero - t_none) < 0.01, \
            f"Zero limit ({t_zero:.3f}s) should match no limit ({t_none:.3f}s)"


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])


def test_kernel_event_collector_counts_overflow_drops():
    """OW1: the native event collector counts ring-overflow drops and serves
    them through drain_kernel_events_with_stats; events carry the kernel
    dispatch sequence."""
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")
    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    # Each velocity-scale change dispatches one kernel event; overflow the
    # 4096-entry collector ring without draining.
    for i in range(4200):
        bundle.set_runtime_velocity_scale(0.5 + (i % 2) * 0.25)
    stats = bundle.drain_kernel_events_with_stats()
    assert stats["dropped_total"] == 4200 - 4096
    events = stats["events"]
    assert len(events) == 4096
    sequences = [e["sequence"] for e in events]
    assert all(s > 0 for s in sequences)
    assert sequences == sorted(sequences)
    # Second drain: empty, monotonic counter unchanged.
    stats2 = bundle.drain_kernel_events_with_stats()
    assert stats2["events"] == []
    assert stats2["dropped_total"] == 4200 - 4096
