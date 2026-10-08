"""Smoke tests for manyforge_core_py bindings."""

import pytest

manyforge_core_py = pytest.importorskip("manyforge_core_py")


def test_import_module():
    """Test that the module can be imported."""
    assert manyforge_core_py is not None


def test_execution_state_enum():
    """Test ExecutionState enum values."""
    assert manyforge_core_py.ExecutionState.kIdle == manyforge_core_py.kIdle
    assert manyforge_core_py.ExecutionState.kPlanning == manyforge_core_py.kPlanning
    assert manyforge_core_py.ExecutionState.kValidating == manyforge_core_py.kValidating
    assert manyforge_core_py.ExecutionState.kExecuting == manyforge_core_py.kExecuting
    assert manyforge_core_py.ExecutionState.kInterrupted == manyforge_core_py.kInterrupted
    assert manyforge_core_py.ExecutionState.kStopping == manyforge_core_py.kStopping
    assert manyforge_core_py.ExecutionState.kStopped == manyforge_core_py.kStopped
    assert manyforge_core_py.ExecutionState.kCompleted == manyforge_core_py.kCompleted
    assert manyforge_core_py.ExecutionState.kFailed == manyforge_core_py.kFailed
    assert (
        manyforge_core_py.ExecutionState.kInterventionRequired
        == manyforge_core_py.kInterventionRequired
    )


def test_cache_mode_enum():
    """Test CacheMode enum values."""
    assert manyforge_core_py.CacheMode.kDefault == manyforge_core_py.kDefault
    assert manyforge_core_py.CacheMode.kDisable == manyforge_core_py.kDisable


def test_error_code_enum():
    """Test ErrorCode enum values."""
    assert manyforge_core_py.ErrorCode.kSuccess == manyforge_core_py.kSuccess
    assert manyforge_core_py.ErrorCode.kInvalidRequest == manyforge_core_py.kInvalidRequest
    assert manyforge_core_py.ErrorCode.kPlanningFailed == manyforge_core_py.kPlanningFailed
    assert (
        manyforge_core_py.ErrorCode.kInvalidContactPolicy
        == manyforge_core_py.kInvalidContactPolicy
    )
    assert (
        manyforge_core_py.ErrorCode.kContactPolicyViolation
        == manyforge_core_py.kContactPolicyViolation
    )


def test_vector3_dto():
    """Test Vector3DTO creation and attributes."""
    v = manyforge_core_py.Vector3DTO(1.0, 2.0, 3.0)
    assert v.x_m == 1.0
    assert v.y_m == 2.0
    assert v.z_m == 3.0
    assert "Vector3DTO" in repr(v)


def test_quaternion_dto():
    """Test QuaternionDTO creation and attributes."""
    q = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    assert q.x == 0.0
    assert q.y == 0.0
    assert q.z == 0.0
    assert q.w == 1.0


def test_pose3_dto():
    """Test Pose3DTO creation and attributes."""
    pos = manyforge_core_py.Vector3DTO(1.0, 2.0, 3.0)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    p = manyforge_core_py.Pose3DTO(pos, orient)
    
    assert p.position_m.x_m == 1.0
    assert p.orientation.w == 1.0


def test_tolerances_dto():
    """Test TolerancesDTO creation and attributes."""
    import manyforge_core_py
    
    t = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    assert t.joint_position_tol_rad == 0.05
    assert t.translation_tol_m == 0.005
    assert t.rotation_tol_rad == 0.05


def test_speed_profile_dto():
    """Test SpeedProfileDTO creation and attributes."""
    import manyforge_core_py
    
    s = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)
    assert s.velocity_scale == 1.0
    assert s.acceleration_scale == 1.0
    assert s.max_tcp_linear_speed_mps is None


def test_timeouts_dto():
    """Test TimeoutsDTO creation and attributes."""
    import manyforge_core_py

    t = manyforge_core_py.TimeoutsDTO(2.0, 10.0)
    assert t.planning_timeout_s == 2.0
    assert t.execution_timeout_s == 10.0


def test_joint_dynamics_limit_dto():
    """Phase 4-P5.1K — JointDynamicsLimitDTO construction + optional fields."""
    import manyforge_core_py

    empty = manyforge_core_py.JointDynamicsLimitDTO()
    assert empty.max_velocity_rad_s is None
    assert empty.max_acceleration_rad_s2 is None
    assert empty.max_jerk_rad_s3 is None

    lim = manyforge_core_py.JointDynamicsLimitDTO(
        max_velocity_rad_s=0.5, max_acceleration_rad_s2=1.2
    )
    assert lim.max_velocity_rad_s == 0.5
    assert lim.max_acceleration_rad_s2 == 1.2
    assert lim.max_jerk_rad_s3 is None


def test_internal_execution_requests_are_not_python_constructible():
    """Resolved execution/planning requests are minted by the C++ Kernel."""
    import manyforge_core_py

    assert not hasattr(manyforge_core_py, "ExecuteRequestDTO")
    assert not hasattr(manyforge_core_py, "PlanRequestDTO")


def test_error_dto():
    """Test ErrorDTO creation and attributes."""
    import manyforge_core_py
    
    e = manyforge_core_py.ErrorDTO("test_code", "test message", {})
    assert e.code == "test_code"
    assert e.message == "test message"


def test_joint_goal_dto():
    """Test JointGoalDTO creation and attributes."""
    import manyforge_core_py
    
    j = manyforge_core_py.JointGoalDTO(["joint1", "joint2"], [0.5, 1.0])
    assert j.joint_names == ["joint1", "joint2"]
    assert j.positions_rad == [0.5, 1.0]


def test_pose_goal_dto():
    """Test PoseGoalDTO creation and attributes."""
    import manyforge_core_py
    
    pos = manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    goal_pose = manyforge_core_py.Pose3DTO(pos, orient)
    
    p = manyforge_core_py.PoseGoalDTO(goal_pose)
    assert p.goal.position_m.x_m == 1.0


def test_robot_state_dto():
    """Test RobotStateDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.RobotStateDTO(["j1", "j2"], [0.0, 0.5], [0.0, 0.0])
    assert r.joint_names == ["j1", "j2"]
    assert r.positions_rad == [0.0, 0.5]


def test_trajectory_dto():
    """Test TrajectoryDTO creation and attributes."""
    import manyforge_core_py
    
    wp1 = manyforge_core_py.TrajectoryWaypointDTO(0.0, [0.0, 0.0], [], [])
    wp2 = manyforge_core_py.TrajectoryWaypointDTO(1.0, [0.5, 0.5], [], [])
    
    resources = [
        manyforge_core_py.ResourceId("resource.j1"),
        manyforge_core_py.ResourceId("resource.j2"),
    ]
    t = manyforge_core_py.TrajectoryDTO(resources, ["j1", "j2"], [wp1, wp2])
    assert [resource.value for resource in t.resources] == [
        "resource.j1",
        "resource.j2",
    ]
    assert t.joint_names == ["j1", "j2"]
    assert len(t.waypoints) == 2
    assert t.empty() is False
    assert t.runtime_scale_bounds is None
    t.runtime_scale_bounds = manyforge_core_py.TrajectoryRuntimeScaleBoundsDTO(2.0, 8.0)
    assert t.runtime_scale_bounds.max_rate_per_s == 2.0
    assert t.runtime_scale_bounds.max_rate_change_per_s2 == 8.0
    assert t.runtime_scale_bounds.max_stop_path_rate == 1.0
    assert t.runtime_scale_bounds.nominal_time_multiplier is None
    t.runtime_scale_bounds = manyforge_core_py.TrajectoryRuntimeScaleBoundsDTO(2.0, 8.0, 1.5)
    assert t.runtime_scale_bounds.max_stop_path_rate == 1.5
    t.runtime_scale_bounds = manyforge_core_py.TrajectoryRuntimeScaleBoundsDTO(
        2.0, 8.0, 1.5, 2.5
    )
    assert t.runtime_scale_bounds.nominal_time_multiplier == 2.5
    assert t.cartesian_waypoint_times_s == []
    t.cartesian_waypoint_times_s = [0.0, 1.0]
    assert t.cartesian_waypoint_times_s == [0.0, 1.0]


def test_execution_status():
    """Test ExecutionStatus creation and attributes."""
    import manyforge_core_py
    
    s = manyforge_core_py.ExecutionStatus()
    assert s.state == manyforge_core_py.ExecutionState.kIdle
    assert s.progress == 0.0
    assert s.execution_id == 0


def test_stop_result_dto():
    """Test StopResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.StopResultDTO()
    assert r.accepted is False


def test_reset_result_dto():
    """Test ResetResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.ResetResultDTO()
    assert r.accepted is False


def test_plan_result_dto():
    """Test PlanResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.PlanResultDTO()
    assert r.trajectory is None
    assert r.error is None


def test_validation_result_dto():
    """Test ValidationResultDTO creation and attributes."""
    import manyforge_core_py

    r = manyforge_core_py.ValidationResultDTO()
    assert r.valid is False
    assert r.trajectory_metrics is None

    metrics = manyforge_core_py.TrajectoryValidationMetricsDTO()
    metrics.trajectory_duration_s = 1.25
    metrics.peak_abs_joint_velocity_rad_s = [0.4, 0.5]
    metrics.peak_abs_joint_acceleration_rad_s2 = [1.0, 1.5]
    metrics.peak_tcp_linear_speed_mps = 0.15
    r.trajectory_metrics = metrics
    assert r.trajectory_metrics.trajectory_duration_s == 1.25
    assert r.trajectory_metrics.peak_abs_joint_velocity_rad_s == [0.4, 0.5]
    assert r.trajectory_metrics.peak_abs_joint_acceleration_rad_s2 == [1.0, 1.5]
    assert r.trajectory_metrics.peak_tcp_linear_speed_mps == 0.15


def test_collision_check_result_dto():
    """Test CollisionCheckResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.CollisionCheckResultDTO()
    assert r.in_collision is False
    assert r.contact_body_a is None
    assert r.contact_body_b is None
    assert r.penetration_depth_m is None
    assert r.estimated_force_n is None

    r.in_collision = True
    r.contact_body_a = "tool0"
    r.contact_body_b = "box"
    r.penetration_depth_m = 0.001
    r.estimated_force_n = 3.5
    assert r.in_collision is True
    assert r.contact_body_a == "tool0"
    assert r.contact_body_b == "box"
    assert r.penetration_depth_m == 0.001
    assert r.estimated_force_n == 3.5


def test_box_shape_dto():
    """Test BoxShapeDTO creation and attributes."""
    import manyforge_core_py
    
    dims = manyforge_core_py.Vector3DTO(0.1, 0.2, 0.3)
    b = manyforge_core_py.BoxShapeDTO(dims)
    assert b.dimensions_m.x_m == 0.1


def test_sphere_shape_dto():
    """Test SphereShapeDTO creation and attributes."""
    import manyforge_core_py
    
    s = manyforge_core_py.SphereShapeDTO(0.1)
    assert s.radius_m == 0.1


def test_cylinder_shape_dto():
    """Test CylinderShapeDTO creation and attributes."""
    import manyforge_core_py
    
    c = manyforge_core_py.CylinderShapeDTO(0.1, 0.5)
    assert c.radius_m == 0.1
    assert c.height_m == 0.5


def test_reference_dto():
    """Test ReferenceDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.ReferenceDTO(
        manyforge_core_py.ReferenceType.kFrameRole, "base"
    )
    assert r.type == manyforge_core_py.ReferenceType.kFrameRole
    assert r.id == "base"
    assert r.pose is None


def test_reference_dto_pose():
    """ReferenceDTO can carry an inline pose reference."""
    import manyforge_core_py

    r = manyforge_core_py.ReferenceDTO(manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.3, -0.2, 0.1),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0),
    ))
    assert r.type == manyforge_core_py.ReferenceType.kPose
    assert r.pose.position_m.x_m == 0.3


def test_relative_motion_authoring_types_remain_available():
    """Python authors intent, while C++ resolves and mints execution requests."""
    import manyforge_core_py

    ref = manyforge_core_py.ReferenceDTO(
        manyforge_core_py.ReferenceType.kFrameRole, "base"
    )
    motion = manyforge_core_py.PoseGoalDTO(manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.1, 0.0, 0.0),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    ))

    assert ref.id == "base"
    assert motion.goal.position_m.x_m == 0.1
    assert manyforge_core_py.MotionTargetMode.kRelative is not None
    assert manyforge_core_py.PlanningProfile.kBalanced is not None


def test_cartesian_path_dto():
    """Test CartesianPathDTO creation and attributes."""
    import manyforge_core_py
    
    pose1 = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.0, 0.0, 0.5),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    )
    pose2 = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.1, 0.0, 0.5),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    )
    
    c = manyforge_core_py.CartesianPathDTO([pose1, pose2], 0.01, 0.1)
    assert len(c.waypoints) == 2
    assert c.max_translation_step_m == 0.01


def test_named_target_dto():
    """Test NamedTargetDTO creation and attributes."""
    import manyforge_core_py
    
    n = manyforge_core_py.NamedTargetDTO("home")
    assert n.name == "home"


def test_validation_options_dto():
    """Test ValidationOptionsDTO creation and attributes."""
    import manyforge_core_py
    
    v = manyforge_core_py.ValidationOptionsDTO(1.0, "tcp", "base_link")
    assert v.max_tcp_linear_speed_mps == 1.0
    assert v.tcp_frame == "tcp"
    assert v.base_frame == "base_link"
    assert v.run_collision_checks is True

    v.run_collision_checks = False
    assert v.run_collision_checks is False

    default_speed = manyforge_core_py.ValidationOptionsDTO(
        tcp_frame="tcp", base_frame="base_link"
    )
    assert default_speed.max_tcp_linear_speed_mps == 0.0
    assert default_speed.run_collision_checks is True


def test_fk_request_dto():
    """Test FkRequestDTO creation and attributes."""
    import manyforge_core_py
    
    state = manyforge_core_py.RobotStateDTO(["j1"], [0.0], [])
    f = manyforge_core_py.FkRequestDTO(state, "tcp", "base_link")
    assert f.target_frame == "tcp"
    assert f.base_frame == "base_link"


def test_joint_limits_dto():
    """Test JointLimitsDTO creation and attributes."""
    import manyforge_core_py
    
    j = manyforge_core_py.JointLimitsDTO(
        ["j1"], [-3.14, 3.14], [-3.14, 3.14], [1.0], []
    )
    assert j.joint_names == ["j1"]
    assert j.position_lower_rad == [-3.14, 3.14]


def test_collision_check_request_dto():
    """Test CollisionCheckRequestDTO creation and attributes."""
    import manyforge_core_py
    
    state = manyforge_core_py.RobotStateDTO(["j1"], [0.0], [])
    c = manyforge_core_py.CollisionCheckRequestDTO(state)
    assert c.robot_state.joint_names == ["j1"]
