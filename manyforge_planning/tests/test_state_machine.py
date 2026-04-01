"""Tests for state machine transitions."""

import pytest

from manyforge_behavior.kernel_client import ExecuteRequest, ExecutionState


def test_stop(client):
    """Test stop transitions."""
    # Stop from IDLE should fail
    result = client.stop()
    assert result.accepted is False
    assert "Cannot stop" in result.error
    
    # Put client in PLANNING state by starting an execute
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
    # We can't easily stop during planning since it completes synchronously
    # But we can test stop from COMPLETED via reset then stop from other states
    
    # Test stop from STOPPED should fail
    client._state = ExecutionState.STOPPED
    result = client.stop()
    assert result.accepted is False
    
    # Test stop from PAUSED should work
    client._state = ExecutionState.PAUSED
    result = client.stop()
    assert result.accepted is True
    assert client.get_status().state == ExecutionState.STOPPED


def test_pause(client):
    """Test pause transitions."""
    # Pause from IDLE should fail
    result = client.pause()
    assert result.accepted is False
    assert "EXECUTING" in result.error
    
    # Pause from EXECUTING should work
    client._state = ExecutionState.EXECUTING
    result = client.pause()
    assert result.accepted is True
    assert client.get_status().state == ExecutionState.PAUSED


def test_resume(client):
    """Test resume transitions."""
    # Resume from IDLE should fail
    result = client.resume()
    assert result.accepted is False
    assert "PAUSED" in result.error
    
    # Resume from PAUSED should work
    client._state = ExecutionState.PAUSED
    result = client.resume()
    assert result.accepted is True
    assert client.get_status().state == ExecutionState.EXECUTING


def test_reset(client):
    """Test reset transitions."""
    # Reset from IDLE should fail
    result = client.reset()
    assert result.accepted is False
    assert "IDLE" in result.error
    
    # Reset from other states should work
    for state in [ExecutionState.PLANNING, ExecutionState.COMPLETED, ExecutionState.FAILED, 
                  ExecutionState.STOPPED, ExecutionState.PAUSED, ExecutionState.EXECUTING]:
        client._state = state
        result = client.reset()
        assert result.accepted is True, f"Reset failed from {state}"
        assert client.get_status().state == ExecutionState.IDLE
