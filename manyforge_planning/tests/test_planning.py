"""Tests for motion planning functionality."""

import numpy as np
import pytest

from manyforge_behavior.kernel_client import ExecuteRequest, ExecutionState
from manyforge_planning.roboplan_mock import MockRoboPlan


def test_plan_joint_goal_success(client):
    """Test that planning a joint goal succeeds."""
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
    )
    
    result = client.execute(request)
    
    assert result.ok is True
    assert result.execution_id == 1
    assert result.error is None
    assert client.get_status().state == ExecutionState.COMPLETED


def test_plan_pose_goal_success(client):
    """Test that planning a pose goal succeeds (via IK)."""
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="pose_goal",
        pose_goal={
            "position_m": [0.5, 0.0, 0.6],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    
    result = client.execute(request)
    
    assert result.ok is True
    assert result.execution_id == 1
    assert result.error is None
    assert client.get_status().state == ExecutionState.COMPLETED


def test_plan_from_non_idle_fails(client):
    """Test that planning from non-IDLE state fails."""
    # First, put client in COMPLETED state
    request1 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
    )
    client.execute(request1)
    
    # Now try to plan again without resetting
    request2 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move2",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.2, 0.3, 0.4, 0.5, 0.6, 0.7],
    )
    result = client.execute(request2)
    
    assert result.ok is False
    assert "IDLE" in result.error


def test_plan_with_collision_fails(client, mock_scene):
    """Test that planning with a collision fails."""
    # Add a collision at the goal position
    goal_q = np.array([0.1, 0.2, 0.3, 0.4, 0.5, 0.6])
    mock_scene.add_collision_position(goal_q)
    
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=goal_q.tolist(),
    )
    
    result = client.execute(request)
    
    assert result.ok is False
    assert client.get_status().state == ExecutionState.FAILED


def test_execute_twice_fresh_trajectory(client):
    """Test that executing twice (with reset) produces fresh trajectories."""
    # First execution
    request1 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
    )
    client.execute(request1)
    
    # Get first trajectory
    trajectory1 = client._cached_trajectory
    assert trajectory1 is not None
    
    # Reset and execute again
    client.reset()
    request2 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move2",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.2, 0.3, 0.4, 0.5, 0.6, 0.7],
    )
    client.execute(request2)
    
    # Get second trajectory
    trajectory2 = client._cached_trajectory
    assert trajectory2 is not None
    
    # Verify they are different objects (fresh trajectories)
    assert trajectory1 is not trajectory2


def test_invalidate_clears_cache(client):
    """Test that invalidate_trajectories sets _cached_trajectory to None."""
    # First, plan something to cache a trajectory
    request = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
    )
    client.execute(request)
    
    # Trajectory should be cached
    assert client._cached_trajectory is not None
    
    # Invalidate
    client.invalidate_trajectories()
    
    # Trajectory should be cleared
    assert client._cached_trajectory is None


def test_execution_id_increments(client):
    """Test that execution_id increments across executions."""
    # First execution
    request1 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move1",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
    )
    result1 = client.execute(request1)
    assert result1.execution_id == 1
    
    # Reset and second execution
    client.reset()
    request2 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move2",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.2, 0.3, 0.4, 0.5, 0.6, 0.7],
    )
    result2 = client.execute(request2)
    assert result2.execution_id == 2
    
    # Reset and third execution
    client.reset()
    request3 = ExecuteRequest(
        robot_id="robot1",
        group_id="group1",
        move_id="move3",
        base_frame="base",
        tcp_frame="tcp",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
    )
    result3 = client.execute(request3)
    assert result3.execution_id == 3
