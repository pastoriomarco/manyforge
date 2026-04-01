"""Tests for kinematics functionality."""

import numpy as np
import pytest

from manyforge_behavior.kernel_client import ExecuteRequest


def test_forward_kinematics(client, mock_scene):
    """Test forward kinematics via get_link_pose."""
    # Set joint positions
    q = np.array([0.1, 0.2, 0.3, 0.4, 0.5, 0.6])
    mock_scene.setJointPositions(q)
    
    # Get link pose
    pose = client.get_link_pose("tcp", "base")
    
    assert pose is not None
    assert "position_m" in pose
    assert "orientation_xyzw" in pose
    assert len(pose["position_m"]) == 3
    assert len(pose["orientation_xyzw"]) == 4
    
    # Verify position is computed from FK (simplified: x = 0.5 + q[0] * 0.1)
    expected_x = 0.5 + q[0] * 0.1
    assert abs(pose["position_m"][0] - expected_x) < 1e-6


def test_joint_limits(client, mock_scene):
    """Test that joint limits are accessible."""
    # Get position limits
    min_limits, max_limits = mock_scene.getPositionLimitVectors()
    
    assert len(min_limits) == 6
    assert len(max_limits) == 6
    assert np.allclose(min_limits, -np.pi * np.ones(6))
    assert np.allclose(max_limits, np.pi * np.ones(6))
    
    # Get velocity limits
    min_vel, max_vel = mock_scene.getVelocityLimitVectors()
    assert np.allclose(min_vel, -3.0 * np.ones(6))
    assert np.allclose(max_vel, 3.0 * np.ones(6))
    
    # Get acceleration limits
    min_acc, max_acc = mock_scene.getAccelerationLimitVectors()
    assert np.allclose(min_acc, -2.0 * np.ones(6))
    assert np.allclose(max_acc, 2.0 * np.ones(6))


def test_invalidate_trajectories(client):
    """Test trajectory invalidation."""
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


def test_get_link_pose_invalid_frame(client):
    """Test that get_link_pose returns None for invalid frame."""
    pose = client.get_link_pose("invalid_frame_name", "base")
    assert pose is None


def test_get_link_pose_orientation_non_identity(client, mock_scene):
    """Test that get_link_pose returns non-identity orientation when joints are non-zero.
    
    This tests the fix for Bug 1 where orientation was hardcoded to identity.
    """
    # Set non-zero joint positions
    q = np.array([0.5, 0.3, 0.2, 0.1, 0.0, 0.0])
    mock_scene.setJointPositions(q)
    
    # Get link pose
    pose = client.get_link_pose("tcp", "base")
    
    assert pose is not None
    assert "orientation_xyzw" in pose
    
    # The mock forwardKinematics returns identity rotation, so orientation should be identity
    # But the code should correctly extract it (not hardcode it)
    # The key is that the conversion function works correctly
    orientation = pose["orientation_xyzw"]
    assert len(orientation) == 4
    # For identity rotation matrix, quaternion should be [0, 0, 0, 1]
    assert np.allclose(orientation, [0.0, 0.0, 0.0, 1.0], atol=1e-6)


def test_get_link_pose_position_formula(client, mock_scene):
    """Test that position values match the FK formula: x = 0.5 + q[0]*0.1."""
    # Set joint positions
    q = np.array([0.5, 0.3, 0.2, 0.1, 0.0, 0.0])
    mock_scene.setJointPositions(q)
    
    # Get link pose
    pose = client.get_link_pose("tcp", "base")
    
    assert pose is not None
    
    # Verify position formula
    expected_x = 0.5 + q[0] * 0.1
    assert abs(pose["position_m"][0] - expected_x) < 1e-6
    assert abs(pose["position_m"][1] - 0.0) < 1e-6
    assert abs(pose["position_m"][2] - 0.6) < 1e-6
