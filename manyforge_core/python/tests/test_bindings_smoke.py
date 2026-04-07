"""Smoke tests for manyforge_core_py bindings."""

import pytest


def test_import_module():
    """Test that the module can be imported."""
    import manyforge_core_py
    assert manyforge_core_py is not None


def test_execution_state_enum():
    """Test ExecutionState enum values."""
    import manyforge_core_py
    
    assert manyforge_core_py.ExecutionState.kIdle == manyforge_core_py.kIdle
    assert manyforge_core_py.ExecutionState.kPlanning == manyforge_core_py.kPlanning
    assert manyforge_core_py.ExecutionState.kValidating == manyforge_core_py.kValidating
    assert manyforge_core_py.ExecutionState.kExecuting == manyforge_core_py.kExecuting
    assert manyforge_core_py.ExecutionState.kPaused == manyforge_core_py.kPaused
    assert manyforge_core_py.ExecutionState.kStopping == manyforge_core_py.kStopping
    assert manyforge_core_py.ExecutionState.kStopped == manyforge_core_py.kStopped
    assert manyforge_core_py.ExecutionState.kCompleted == manyforge_core_py.kCompleted
    assert manyforge_core_py.ExecutionState.kFailed == manyforge_core_py.kFailed


def test_cache_mode_enum():
    """Test CacheMode enum values."""
    import manyforge_core_py
    
    assert manyforge_core_py.CacheMode.kDefault == manyforge_core_py.kDefault
    assert manyforge_core_py.CacheMode.kDisable == manyforge_core_py.kDisable


def test_error_code_enum():
    """Test ErrorCode enum values."""
    import manyforge_core_py
    
    assert manyforge_core_py.ErrorCode.kSuccess == manyforge_core_py.kSuccess
    assert manyforge_core_py.ErrorCode.kInvalidRequest == manyforge_core_py.kInvalidRequest
    assert manyforge_core_py.ErrorCode.kPlanningFailed == manyforge_core_py.kPlanningFailed


def test_vector3_dto():
    """Test Vector3DTO creation and attributes."""
    import manyforge_core_py
    
    v = manyforge_core_py.Vector3DTO(1.0, 2.0, 3.0)
    assert v.x_m == 1.0
    assert v.y_m == 2.0
    assert v.z_m == 3.0
    assert "Vector3DTO" in repr(v)


def test_quaternion_dto():
    """Test QuaternionDTO creation and attributes."""
    import manyforge_core_py
    
    q = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    assert q.x == 0.0
    assert q.y == 0.0
    assert q.z == 0.0
    assert q.w == 1.0


def test_pose3_dto():
    """Test Pose3DTO creation and attributes."""
    import manyforge_core_py
    
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
    
    t = manyforge_core_py.TrajectoryDTO(["j1", "j2"], [wp1, wp2])
    assert t.joint_names == ["j1", "j2"]
    assert len(t.waypoints) == 2
    assert t.empty() is False


def test_execution_status():
    """Test ExecutionStatus creation and attributes."""
    import manyforge_core_py
    
    s = manyforge_core_py.ExecutionStatus()
    assert s.state == manyforge_core_py.ExecutionState.kIdle
    assert s.progress == 0.0
    assert s.execution_id == 0


def test_execute_request_dto():
    """Test ExecuteRequestDTO creation and attributes."""
    import manyforge_core_py
    
    tolerances = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    speed_profile = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)
    timeouts = manyforge_core_py.TimeoutsDTO(2.0, 10.0)
    motion = manyforge_core_py.JointGoalDTO(["j1"], [0.5])
    
    req = manyforge_core_py.ExecuteRequestDTO(
        "robot1", "group1", "move1",
        "base_link", "tcp",
        tolerances, speed_profile, timeouts,
        manyforge_core_py.CacheMode.kDefault,
        motion
    )
    
    assert req.robot_id == "robot1"
    assert req.group_id == "group1"
    assert req.move_id == "move1"


def test_plan_request_dto():
    """Test PlanRequestDTO creation and attributes."""
    import manyforge_core_py
    
    start_state = manyforge_core_py.RobotStateDTO(["j1"], [0.0], [])
    motion = manyforge_core_py.JointGoalDTO(["j1"], [0.5])
    tolerances = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    speed_profile = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)
    
    req = manyforge_core_py.PlanRequestDTO(
        "robot1", "group1", "move1",
        "base_link", "tcp",
        start_state, motion, tolerances, speed_profile,
        2.0
    )
    
    assert req.robot_id == "robot1"
    assert req.planning_timeout_s == 2.0


def test_stop_result_dto():
    """Test StopResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.StopResultDTO()
    assert r.accepted is False


def test_pause_result_dto():
    """Test PauseResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.PauseResultDTO()
    assert r.accepted is False


def test_resume_result_dto():
    """Test ResumeResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.ResumeResultDTO()
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


def test_collision_check_result_dto():
    """Test CollisionCheckResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.CollisionCheckResultDTO()
    assert r.in_collision is False


def test_scene_edit_result_dto():
    """Test SceneEditResultDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.SceneEditResultDTO()
    assert r.success is False


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


def test_add_object_request_dto():
    """Test AddObjectRequestDTO creation and attributes."""
    import manyforge_core_py
    
    shape = manyforge_core_py.BoxShapeDTO(manyforge_core_py.Vector3DTO(0.1, 0.1, 0.1))
    pose = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    )
    
    req = manyforge_core_py.AddObjectRequestDTO("obj1", shape, pose, "base_link")
    assert req.object_id == "obj1"


def test_remove_object_request_dto():
    """Test RemoveObjectRequestDTO creation and attributes."""
    import manyforge_core_py
    
    req = manyforge_core_py.RemoveObjectRequestDTO("obj1")
    assert req.object_id == "obj1"


def test_update_object_pose_request_dto():
    """Test UpdateObjectPoseRequestDTO creation and attributes."""
    import manyforge_core_py
    
    pose = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    )
    
    req = manyforge_core_py.UpdateObjectPoseRequestDTO("obj1", pose, "base_link")
    assert req.object_id == "obj1"


def test_object_state_dto():
    """Test ObjectStateDTO creation and attributes."""
    import manyforge_core_py

    state = manyforge_core_py.ObjectStateDTO("obj1", True, False, "universe")
    assert state.object_id == "obj1"
    assert state.exists is True
    assert state.attached is False
    assert state.frame_id == "universe"


def test_reference_dto():
    """Test ReferenceDTO creation and attributes."""
    import manyforge_core_py
    
    r = manyforge_core_py.ReferenceDTO(manyforge_core_py.ReferenceType.kFrame, "base_link")
    assert r.type == manyforge_core_py.ReferenceType.kFrame
    assert r.id == "base_link"
    assert r.pose is None


def test_reference_dto_pose():
    """ReferenceDTO can carry an inline pose reference."""
    import manyforge_core_py

    r = manyforge_core_py.ReferenceDTO()
    r.type = manyforge_core_py.ReferenceType.kPose
    r.pose = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.3, -0.2, 0.1),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0),
    )
    assert r.type == manyforge_core_py.ReferenceType.kPose
    assert r.pose.position_m.x_m == 0.3


def test_relative_move_dto():
    """Test RelativeMoveDTO creation and attributes."""
    import manyforge_core_py
    
    ref = manyforge_core_py.ReferenceDTO(manyforge_core_py.ReferenceType.kFrame, "base_link")
    offset = manyforge_core_py.Pose3DTO(
        manyforge_core_py.Vector3DTO(0.1, 0.0, 0.0),
        manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    )
    
    r = manyforge_core_py.RelativeMoveDTO(ref, offset)
    assert r.target_reference.id == "base_link"
    assert r.offset_reference is None


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
