"""Tests for validation and failure scenarios."""

import numpy as np
import pytest

from manyforge_behavior.kernel_client import ExecuteRequest, ExecutionState, SceneEditRequest


class TestPlanningFailures:
    """Test planning failure scenarios."""
    
    def test_ik_solver_fails(self, client, mock_ik):
        """Test that pose_goal returns ok=False when IK solver fails."""
        mock_ik.set_fail(True)
        
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
        
        assert result.ok is False
        assert client.get_status().state == ExecutionState.FAILED
    
    def test_rrt_planner_fails(self, client, mock_rrt):
        """Test that joint_goal returns ok=False when RRT planner fails."""
        mock_rrt.set_fail(True)
        
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
        
        assert result.ok is False
        assert client.get_status().state == ExecutionState.FAILED
    
    def test_toppra_fails(self, client, mock_topptra):
        """Test that TOPPRA failure returns ok=False."""
        mock_topptra.set_fail(True)
        
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
        
        assert result.ok is False
        assert client.get_status().state == ExecutionState.FAILED
    
    def test_invalid_motion_type(self, client):
        """Test that invalid motion_type returns error."""
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move1",
            base_frame="base",
            tcp_frame="tcp",
            motion_type="cartesian",  # Invalid motion type
        )
        
        result = client.execute(request)
        
        assert result.ok is False
        assert "Unknown motion_type" in result.error
        assert client.get_status().state == ExecutionState.FAILED
    
    def test_pose_goal_none(self, client):
        """Test that pose_goal with None pose returns error."""
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move1",
            base_frame="base",
            tcp_frame="tcp",
            motion_type="pose_goal",
            pose_goal=None,
        )
        
        result = client.execute(request)
        
        assert result.ok is False
        assert "pose_goal is None" in result.error
        assert client.get_status().state == ExecutionState.FAILED


class TestStateMachineCompleteness:
    """Test state machine completeness."""
    
    def test_execute_complete_reset_execute_cycle(self, client):
        """Test full cycle: execute -> complete -> reset -> execute again."""
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
        assert result1.ok is True
        assert client.get_status().state == ExecutionState.COMPLETED
        
        # Reset
        reset_result = client.reset()
        assert reset_result.accepted is True
        assert client.get_status().state == ExecutionState.IDLE
        
        # Second execution
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
        assert result2.ok is True
        assert client.get_status().state == ExecutionState.COMPLETED
    
    def test_stop_from_planning(self, client):
        """Test stop from PLANNING state."""
        client._state = ExecutionState.PLANNING
        result = client.stop()
        assert result.accepted is True
        assert client.get_status().state == ExecutionState.STOPPED
    
    def test_stop_from_executing(self, client):
        """Test stop from EXECUTING state."""
        client._state = ExecutionState.EXECUTING
        result = client.stop()
        assert result.accepted is True
        assert client.get_status().state == ExecutionState.STOPPED
    
    def test_stop_from_paused(self, client):
        """Test stop from PAUSED state."""
        client._state = ExecutionState.PAUSED
        result = client.stop()
        assert result.accepted is True
        assert client.get_status().state == ExecutionState.STOPPED
    
    def test_stop_from_idle_rejected(self, client):
        """Test stop from IDLE is rejected."""
        client._state = ExecutionState.IDLE
        result = client.stop()
        assert result.accepted is False
    
    def test_stop_from_completed_rejected(self, client):
        """Test stop from COMPLETED is rejected."""
        client._state = ExecutionState.COMPLETED
        result = client.stop()
        assert result.accepted is False
    
    def test_stop_from_failed_rejected(self, client):
        """Test stop from FAILED is rejected."""
        client._state = ExecutionState.FAILED
        result = client.stop()
        assert result.accepted is False
    
    def test_pause_from_non_executing_rejected(self, client):
        """Test pause from non-EXECUTING states is rejected."""
        for state in [ExecutionState.IDLE, ExecutionState.PLANNING, 
                      ExecutionState.COMPLETED, ExecutionState.FAILED]:
            client._state = state
            result = client.pause()
            assert result.accepted is False, f"Pause should be rejected from {state}"
    
    def test_resume_from_non_paused_rejected(self, client):
        """Test resume from non-PAUSED states is rejected."""
        for state in [ExecutionState.IDLE, ExecutionState.PLANNING,
                      ExecutionState.EXECUTING, ExecutionState.COMPLETED, 
                      ExecutionState.FAILED]:
            client._state = state
            result = client.resume()
            assert result.accepted is False, f"Resume should be rejected from {state}"


class TestSceneEditValidation:
    """Test scene edit validation."""
    
    def test_remove_nonexistent_geometry(self, client):
        """Test removing non-existent geometry returns error."""
        request = SceneEditRequest(
            operation="remove",
            object_id="nonexistent",
        )
        
        result = client.scene_edit(request)
        
        assert result.success is False
        assert "Unknown geometry" in result.error
    
    def test_update_nonexistent_geometry(self, client):
        """Test updating pose of non-existent geometry returns error."""
        request = SceneEditRequest(
            operation="update_pose",
            object_id="nonexistent",
            pose_position_m=[0.5, 0.0, 0.6],
        )
        
        result = client.scene_edit(request)
        
        assert result.success is False
        assert "Unknown geometry" in result.error
    
    def test_add_box_without_dimensions(self, client):
        """Test adding box without dimensions returns error."""
        request = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=None,
            pose_position_m=[0.5, 0.0, 0.6],
        )
        
        result = client.scene_edit(request)
        
        assert result.success is False
        assert "box_dimensions_m required" in result.error
    
    def test_add_sphere_without_radius(self, client):
        """Test adding sphere without radius returns error."""
        request = SceneEditRequest(
            operation="add",
            object_id="sphere1",
            shape_type="sphere",
            sphere_radius_m=None,
            pose_position_m=[0.5, 0.0, 0.6],
        )
        
        result = client.scene_edit(request)
        
        assert result.success is False
        assert "sphere_radius_m required" in result.error
