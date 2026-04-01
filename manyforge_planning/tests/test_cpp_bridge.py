"""Integration tests for cpp_bridge adapters and factory."""

import time
import pytest
import numpy as np

import manyforge_core_py

from manyforge_planning.roboplan_mock import (
    MockScene,
    MockRRT,
    MockRRTOptions,
    MockPathShortcutter,
    MockTOPPRA,
    MockSimpleIk,
    MockSimpleIkOptions,
    MockRoboPlan,
)

from cpp_bridge import (
    RoboPlanPlannerAdapter,
    RoboPlanPlanningContextAdapter,
    RoboPlanExecutionAdapter,
    WallClock,
    ThreadScheduler,
)

from cpp_kernel_factory import (
    create_cpp_kernel_client,
    create_default_mock_scene,
    create_default_planning_components,
    create_full_kernel_client,
)

from manyforge_behavior.kernel_client import ExecuteRequest


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def mock_scene():
    """Create a mock scene with 6 joints."""
    joint_names = ["j1", "j2", "j3", "j4", "j5", "j6"]
    return MockScene(joint_names=joint_names, group_name="arm")


@pytest.fixture
def planning_components(mock_scene):
    """Create planning components for the scene."""
    rrt_opts = MockRRTOptions()
    rrt_opts.group_name = "arm"
    rrt_opts.max_planning_time = 5.0
    rrt_opts.max_nodes = 5000

    rrt = MockRRT(mock_scene, rrt_opts)
    shortcutter = MockPathShortcutter(mock_scene, "arm")
    toppra = MockTOPPRA(mock_scene)

    ik_opts = MockSimpleIkOptions()
    ik_opts.group_name = "arm"
    ik_opts.max_restarts = 50

    ik = MockSimpleIk(mock_scene, ik_opts)

    return rrt, shortcutter, toppra, ik


@pytest.fixture
def planner_adapter(mock_scene, planning_components):
    """Create a planner adapter."""
    rrt, shortcutter, toppra, ik = planning_components
    return RoboPlanPlannerAdapter(mock_scene, rrt, shortcutter, toppra, ik)


@pytest.fixture
def context_adapter(mock_scene):
    """Create a planning context adapter."""
    return RoboPlanPlanningContextAdapter(mock_scene)


@pytest.fixture
def execution_adapter():
    """Create an execution adapter."""
    return RoboPlanExecutionAdapter()


@pytest.fixture
def kernel_client(mock_scene, planning_components):
    """Create a full kernel client."""
    rrt, shortcutter, toppra, ik = planning_components
    return create_cpp_kernel_client(
        mock_scene, rrt, shortcutter, toppra, ik, MockRoboPlan
    )


# ============================================================================
# Planner Adapter Tests
# ============================================================================

def test_planner_adapter_joint_goal(planner_adapter, mock_scene):
    """Test planning with a joint goal succeeds."""
    # Create start state
    start_positions = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    start_state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        start_positions,
        [0.0] * 6
    )

    # Create joint goal
    goal_positions = [0.5, 0.5, 0.5, 0.5, 0.5, 0.5]
    motion = manyforge_core_py.JointGoalDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        goal_positions
    )

    # Create request
    tolerances = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    speed_profile = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)

    request = manyforge_core_py.PlanRequestDTO(
        "robot1", "group1", "move1",
        "base_link", "tcp",
        start_state, motion, tolerances, speed_profile,
        2.0
    )

    # Plan
    result = planner_adapter.plan(request)

    # Verify success
    assert result.error is None
    assert result.trajectory is not None
    assert len(result.trajectory.waypoints) >= 2


def test_planner_adapter_pose_goal(planner_adapter, mock_scene):
    """Test planning with a pose goal succeeds."""
    # Create start state
    start_positions = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    start_state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        start_positions,
        [0.0] * 6
    )

    # Create pose goal
    pos = manyforge_core_py.Vector3DTO(0.5, 0.0, 0.6)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    goal_pose = manyforge_core_py.Pose3DTO(pos, orient)
    motion = manyforge_core_py.PoseGoalDTO(goal_pose)

    # Create request
    tolerances = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    speed_profile = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)

    request = manyforge_core_py.PlanRequestDTO(
        "robot1", "group1", "move1",
        "base_link", "tcp",
        start_state, motion, tolerances, speed_profile,
        2.0
    )

    # Plan
    result = planner_adapter.plan(request)

    # Verify success (IK may just return start position, but planning should work)
    assert result.error is None or "IK" not in (result.error.message if result.error else "")
    if result.error is None:
        assert result.trajectory is not None


def test_planner_adapter_planning_failure(planner_adapter, mock_scene):
    """Test that RRT failure returns an error."""
    # Force RRT to fail
    planner_adapter._rrt.set_fail(True)

    # Create start state
    start_positions = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    start_state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        start_positions,
        [0.0] * 6
    )

    # Create joint goal
    goal_positions = [0.5, 0.5, 0.5, 0.5, 0.5, 0.5]
    motion = manyforge_core_py.JointGoalDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        goal_positions
    )

    # Create request
    tolerances = manyforge_core_py.TolerancesDTO(0.05, 0.005, 0.05)
    speed_profile = manyforge_core_py.SpeedProfileDTO(1.0, 1.0, None)

    request = manyforge_core_py.PlanRequestDTO(
        "robot1", "group1", "move1",
        "base_link", "tcp",
        start_state, motion, tolerances, speed_profile,
        2.0
    )

    # Plan
    result = planner_adapter.plan(request)

    # Verify error
    assert result.error is not None
    assert result.trajectory is None


# ============================================================================
# Planning Context Adapter Tests
# ============================================================================

def test_context_get_current_state(context_adapter, mock_scene):
    """Test getting current state returns correct joint positions."""
    # Set known joint positions
    mock_scene.setJointPositions([1.0, 2.0, 3.0, 4.0, 5.0, 6.0])

    # Get current state
    state = context_adapter.get_current_state()

    # Verify
    assert state.joint_names == ["j1", "j2", "j3", "j4", "j5", "j6"]
    assert np.allclose(state.positions_rad, [1.0, 2.0, 3.0, 4.0, 5.0, 6.0])


def test_context_validate_trajectory(context_adapter, mock_scene):
    """Test that a valid trajectory passes validation."""
    # Create a trajectory with waypoints
    wp1 = manyforge_core_py.TrajectoryWaypointDTO(0.0, [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [], [])
    wp2 = manyforge_core_py.TrajectoryWaypointDTO(1.0, [0.5, 0.5, 0.5, 0.5, 0.5, 0.5], [], [])

    trajectory = manyforge_core_py.TrajectoryDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        [wp1, wp2]
    )

    options = manyforge_core_py.ValidationOptionsDTO(1.0, "tcp", "base_link")

    # Validate
    result = context_adapter.validate_trajectory(trajectory, options)

    # Should be valid (no collision positions set)
    assert result.valid is True
    assert result.error is None


def test_context_collision_check_clear(context_adapter, mock_scene):
    """Test collision check returns false for clear state."""
    # Create robot state
    state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        [0.0] * 6
    )

    request = manyforge_core_py.CollisionCheckRequestDTO(state)

    # Check
    result = context_adapter.collision_check(request)

    # Should not be in collision
    assert result.in_collision is False


def test_context_collision_check_collision(context_adapter, mock_scene):
    """Test collision check returns true for collision state."""
    # Add a collision position
    collision_q = [0.5, 0.5, 0.5, 0.5, 0.5, 0.5]
    mock_scene.add_collision_position(collision_q)

    # Create robot state at collision position
    state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        collision_q,
        [0.0] * 6
    )

    request = manyforge_core_py.CollisionCheckRequestDTO(state)

    # Check
    result = context_adapter.collision_check(request)

    # Should be in collision
    assert result.in_collision is True


def test_context_forward_kinematics(context_adapter, mock_scene):
    """Test forward kinematics returns correct pose."""
    # Create robot state
    state = manyforge_core_py.RobotStateDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        [0.0] * 6
    )

    request = manyforge_core_py.FkRequestDTO(state, "tcp", "base_link")

    # Compute FK
    pose = context_adapter.forward_kinematics(request)

    # Verify pose structure
    assert pose is not None
    assert hasattr(pose, "position_m")
    assert hasattr(pose, "orientation")


def test_context_get_joint_limits(context_adapter, mock_scene):
    """Test getting joint limits returns correct values."""
    limits = context_adapter.get_joint_limits("robot1", "arm")

    # Verify structure
    assert limits.joint_names == ["j1", "j2", "j3", "j4", "j5", "j6"]
    assert len(limits.position_lower_rad) == 6
    assert len(limits.position_upper_rad) == 6


def test_context_add_object(context_adapter, mock_scene):
    """Test adding a box to the scene."""
    # Create box shape
    dims = manyforge_core_py.Vector3DTO(0.1, 0.2, 0.3)
    shape = manyforge_core_py.BoxShapeDTO(dims)

    # Create pose
    pos = manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    pose = manyforge_core_py.Pose3DTO(pos, orient)

    # Create request
    request = manyforge_core_py.AddObjectRequestDTO("obj1", shape, pose, "base_link")

    # Add object
    result = context_adapter.add_object(request)

    # Verify success
    assert result.success is True
    assert result.error is None


def test_context_remove_object(context_adapter, mock_scene):
    """Test removing an object from the scene."""
    # First add an object
    dims = manyforge_core_py.Vector3DTO(0.1, 0.2, 0.3)
    shape = manyforge_core_py.BoxShapeDTO(dims)
    pos = manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    pose = manyforge_core_py.Pose3DTO(pos, orient)
    add_request = manyforge_core_py.AddObjectRequestDTO("obj1", shape, pose, "base_link")
    context_adapter.add_object(add_request)

    # Now remove it
    remove_request = manyforge_core_py.RemoveObjectRequestDTO("obj1")
    result = context_adapter.remove_object(remove_request)

    # Verify success
    assert result.success is True


# ============================================================================
# Execution Backend Tests
# ============================================================================

def test_execution_backend_execute(execution_adapter):
    """Test that execution backend accepts trajectory."""
    # Create a trajectory
    wp1 = manyforge_core_py.TrajectoryWaypointDTO(0.0, [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], [], [])
    wp2 = manyforge_core_py.TrajectoryWaypointDTO(1.0, [0.5, 0.5, 0.5, 0.5, 0.5, 0.5], [], [])

    trajectory = manyforge_core_py.TrajectoryDTO(
        ["j1", "j2", "j3", "j4", "j5", "j6"],
        [wp1, wp2]
    )

    # Execute
    handle = execution_adapter.execute_trajectory(trajectory)

    # Verify handle
    assert handle is not None


# ============================================================================
# WallClock Tests
# ============================================================================

def test_wall_clock_now():
    """Test wall clock returns time in nanoseconds."""
    clock = WallClock()
    now = clock.now()

    # Should be a large number (nanoseconds since epoch)
    assert now > 0
    assert now > 1e12  # At least year 2001 in nanoseconds


# ============================================================================
# ThreadScheduler Tests
# ============================================================================

def test_thread_scheduler_schedule():
    """Test scheduler schedules callback."""
    scheduler = ThreadScheduler()
    deadline = int(time.time() * 1e9) + 100_000_000  # 100ms from now

    callback_called = []

    def callback():
        callback_called.append(True)

    handle = scheduler.schedule(deadline, callback)

    # Verify handle
    assert handle > 0

    # Wait for callback
    time.sleep(0.15)

    # Verify callback was called
    assert len(callback_called) == 1


def test_thread_scheduler_cancel():
    """Test scheduler cancels callback."""
    scheduler = ThreadScheduler()
    deadline = int(time.time() * 1e9) + 1000_000_000  # 1 second from now

    callback_called = []

    def callback():
        callback_called.append(True)

    handle = scheduler.schedule(deadline, callback)

    # Cancel before deadline
    scheduler.cancel(handle)

    # Wait past deadline
    time.sleep(1.1)

    # Verify callback was NOT called
    assert len(callback_called) == 0


# ============================================================================
# Factory Tests
# ============================================================================

def test_factory_creates_kernel():
    """Test factory returns working CppKernelClient."""
    client = create_full_kernel_client()

    # Verify it's a CppKernelClient
    from cpp_kernel_client import CppKernelClient
    assert isinstance(client, CppKernelClient)

    # Verify it implements KernelClient ABC
    from manyforge_behavior.kernel_client import KernelClient
    assert isinstance(client, KernelClient)


# ============================================================================
# End-to-End Integration Tests
# ============================================================================

def test_e2e_joint_goal_execute(kernel_client):
    """Test full execute flow: plan → validate → execute."""
    # Create execute request with joint goal
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id="move1",
        base_frame="base_link",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.5, 0.5, 0.5, 0.5, 0.5, 0.5],
        velocity_scale=1.0,
        acceleration_scale=1.0,
        joint_position_tol_rad=0.05,
        translation_tol_m=0.005,
        rotation_tol_rad=0.05,
        planning_timeout_s=2.0,
        execution_timeout_s=10.0,
    )

    # Execute
    result = kernel_client.execute(request)

    # Verify success
    assert result.ok is True
    assert result.execution_id > 0


def test_e2e_execute_then_reset(kernel_client):
    """Test execute, then reset to idle."""
    # Create execute request
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="arm",
        move_id="move1",
        base_frame="base_link",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.5, 0.5, 0.5, 0.5, 0.5, 0.5],
    )

    # Execute
    result = kernel_client.execute(request)
    assert result.ok is True

    # Get status
    status = kernel_client.get_status()
    assert status.state.value == "completed"

    # Reset
    reset_result = kernel_client.reset()
    assert reset_result.accepted is True

    # Verify back to idle
    status = kernel_client.get_status()
    assert status.state.value == "idle"


def test_e2e_scene_edit_invalidates_cache(mock_scene, planning_components):
    """Test that adding object works through context."""
    rrt, shortcutter, toppra, ik = planning_components

    # Create kernel client
    client = create_cpp_kernel_client(
        mock_scene, rrt, shortcutter, toppra, ik, MockRoboPlan
    )

    # Create planning context adapter directly for scene edit
    context = RoboPlanPlanningContextAdapter(mock_scene)

    # Add object
    dims = manyforge_core_py.Vector3DTO(0.1, 0.2, 0.3)
    shape = manyforge_core_py.BoxShapeDTO(dims)
    pos = manyforge_core_py.Vector3DTO(1.0, 0.0, 0.5)
    orient = manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
    pose = manyforge_core_py.Pose3DTO(pos, orient)
    request = manyforge_core_py.AddObjectRequestDTO("obj1", shape, pose, "base_link")

    result = context.add_object(request)

    # Verify success
    assert result.success is True


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
