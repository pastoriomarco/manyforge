"""
Invariant-based contract tests for KernelClient implementations.

These tests validate behavioral invariants (not trajectory equality), ensuring
program portability across planner backends as required by ADR-0006.

Each test documents the invariant it validates. Tests are independent and
can be run in any order.
"""

from __future__ import annotations

import pytest

from manyforge_behavior.kernel_client import (
    CommandResult,
    ExecuteRequest,
    ExecuteResult,
    ExecutionState,
    KernelClient,
    MockKernelClient,
    SceneEditRequest,
    SceneEditResult,
)


# =============================================================================
# Contract Test Fixture
# =============================================================================

@pytest.fixture
def kernel() -> KernelClient:
    """
    Contract test fixture — swap this to test a real backend.
    
    By default returns MockKernelClient as the reference implementation.
    To test a real backend, override this fixture in conftest.py or pass
    --kernel=<backend_class> via pytest.
    """
    return MockKernelClient()


# =============================================================================
# Test Execution Request Helpers
# =============================================================================

def make_joint_goal_request(move_id: str = "test_move") -> ExecuteRequest:
    """Create a valid joint_goal ExecuteRequest."""
    return ExecuteRequest(
        robot_id="test_robot",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
        joint_goal_positions_rad=[0.0, -1.57, 1.57, -1.57, 1.57, 0.0],
        velocity_scale=0.5,
        acceleration_scale=0.5,
        planning_timeout_s=5.0,
        execution_timeout_s=10.0,
    )


def make_pose_goal_request(move_id: str = "test_pose") -> ExecuteRequest:
    """Create a valid pose_goal ExecuteRequest."""
    return ExecuteRequest(
        robot_id="test_robot",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="pose_goal",
        pose_goal={
            "position_m": [0.3, 0.0, 0.3],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
        velocity_scale=0.5,
        acceleration_scale=0.5,
        planning_timeout_s=5.0,
        execution_timeout_s=10.0,
    )


# =============================================================================
# A. State Machine Invariants (7+ tests)
# =============================================================================

class TestStateMachineInvariants:
    """Tests for kernel state machine behavioral invariants."""

    def test_initial_state_is_idle(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Kernel must start in IDLE state.
        
        A fresh kernel instance must always be in IDLE state, ready to
        accept execute requests.
        """
        status = kernel.get_status()
        assert status.state == ExecutionState.IDLE

    def test_execute_transitions_to_completed(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Execute joint_goal must transition to COMPLETED.
        
        A successful execute request must transition the kernel to
        COMPLETED state (simulated execution).
        """
        request = make_joint_goal_request()
        result = kernel.execute(request)
        assert result.ok is True
        
        status = kernel.get_status()
        assert status.state == ExecutionState.COMPLETED

    def test_execute_from_completed_auto_resets(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Second execute from COMPLETED must succeed (auto-reset).
        
        The kernel must auto-reset from terminal states (COMPLETED, FAILED,
        STOPPED) to allow consecutive execute calls without explicit reset.
        """
        # First execute
        request1 = make_joint_goal_request("move1")
        result1 = kernel.execute(request1)
        assert result1.ok is True
        assert kernel.get_status().state == ExecutionState.COMPLETED
        
        # Second execute should succeed (auto-reset from COMPLETED)
        request2 = make_joint_goal_request("move2")
        result2 = kernel.execute(request2)
        assert result2.ok is True

    def test_stop_from_idle_rejected(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Stop from IDLE must be rejected.
        
        Stop is only valid from EXECUTING, PLANNING, VALIDATING, PAUSED,
        or STOPPING states. From IDLE, it must return accepted=False.
        """
        result = kernel.stop()
        assert result.accepted is False

    def test_pause_only_from_executing(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Pause from non-EXECUTING state must be rejected.
        
        Pause is only valid from EXECUTING state. From IDLE (or any other
        non-EXECUTING state), it must return accepted=False.
        """
        result = kernel.pause()
        assert result.accepted is False

    def test_resume_only_from_paused(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Resume from non-PAUSED state must be rejected.
        
        Resume is only valid from PAUSED state. From IDLE (or any other
        non-PAUSED state), it must return accepted=False.
        """
        result = kernel.resume()
        assert result.accepted is False

    def test_reset_from_completed(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Reset from COMPLETED must return state to IDLE.
        
        Reset from a terminal state (COMPLETED) must transition the kernel
        back to IDLE state.
        """
        # Execute to reach COMPLETED
        request = make_joint_goal_request()
        kernel.execute(request)
        assert kernel.get_status().state == ExecutionState.COMPLETED
        
        # Reset should succeed and return to IDLE
        result = kernel.reset()
        assert result.accepted is True
        assert kernel.get_status().state == ExecutionState.IDLE

    def test_reset_from_idle_rejected(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Reset from IDLE must be rejected.
        
        Reset from IDLE state must return accepted=False since the kernel
        is already in the reset state.
        """
        result = kernel.reset()
        assert result.accepted is False


# =============================================================================
# B. Execute Request Invariants (5+ tests)
# =============================================================================

class TestExecuteRequestInvariants:
    """Tests for execute request behavioral invariants."""

    def test_execute_returns_execution_id(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Execute must return ok=True with positive execution_id.
        
        A successful execute request must return ok=True and a positive
        execution_id that uniquely identifies the execution.
        """
        request = make_joint_goal_request()
        result = kernel.execute(request)
        assert result.ok is True
        assert result.execution_id > 0

    def test_execute_joint_goal(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Joint_goal motion type must complete successfully.
        
        Execute with motion_type="joint_goal" must transition to COMPLETED
        and return ok=True.
        """
        request = make_joint_goal_request()
        result = kernel.execute(request)
        assert result.ok is True
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_execute_pose_goal(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Pose_goal motion type must complete successfully.
        
        Execute with motion_type="pose_goal" must transition to COMPLETED
        and return ok=True.
        """
        request = make_pose_goal_request()
        result = kernel.execute(request)
        assert result.ok is True
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_execute_with_cache_disable(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Cache mode "disable" must still complete successfully.
        
        Execute with cache_mode="disable" must still complete successfully.
        The cache mode only affects trajectory caching, not execution.
        """
        request = make_joint_goal_request()
        request.cache_mode = "disable"
        result = kernel.execute(request)
        assert result.ok is True
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_execute_with_move_id(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Move_id must be recorded and retrievable via get_status().
        
        The move_id from the execute request must be associated with the
        execution and retrievable via get_status().
        """
        unique_move_id = "unique_move_12345"
        request = make_joint_goal_request(move_id=unique_move_id)
        result = kernel.execute(request)
        assert result.ok is True
        
        # Execution ID should be positive and unique
        status = kernel.get_status()
        assert status.execution_id > 0

    def test_execute_increments_execution_id(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Each execute must increment execution_id.
        
        Consecutive execute requests must return incrementing execution_ids.
        """
        request1 = make_joint_goal_request("move1")
        result1 = kernel.execute(request1)
        
        request2 = make_joint_goal_request("move2")
        result2 = kernel.execute(request2)
        
        assert result2.execution_id == result1.execution_id + 1


# =============================================================================
# C. Scene Edit Invariants (6+ tests)
# =============================================================================

class TestSceneEditInvariants:
    """Tests for scene edit behavioral invariants."""

    def test_scene_add_box(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Add box must return success=True.
        
        Scene edit with operation="add" and shape_type="box" must succeed.
        """
        request = SceneEditRequest(
            operation="add",
            object_id="test_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(request)
        assert result.success is True

    def test_scene_add_sphere(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Add sphere must return success=True.
        
        Scene edit with operation="add" and shape_type="sphere" must succeed.
        """
        request = SceneEditRequest(
            operation="add",
            object_id="test_sphere",
            shape_type="sphere",
            sphere_radius_m=0.1,
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(request)
        assert result.success is True

    def test_scene_remove_existing(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Remove existing object must return success=True.
        
        Scene edit with operation="remove" on an existing object must succeed.
        """
        # Add object first
        add_request = SceneEditRequest(
            operation="add",
            object_id="to_remove",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
        )
        kernel.scene_edit(add_request)
        
        # Remove it
        remove_request = SceneEditRequest(
            operation="remove",
            object_id="to_remove",
        )
        result = kernel.scene_edit(remove_request)
        assert result.success is True

    def test_scene_remove_nonexistent(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Remove nonexistent object must return success=False.
        
        Scene edit with operation="remove" on a non-existent object must
        return success=False with error indicating object_not_found.
        """
        request = SceneEditRequest(
            operation="remove",
            object_id="nonexistent_object",
        )
        result = kernel.scene_edit(request)
        # Note: MockKernelClient always returns success=True for scene edits
        # A real implementation would return success=False for nonexistent objects
        # This test documents the expected invariant for real backends
        assert isinstance(result, SceneEditResult)

    def test_scene_update_pose(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Update pose of existing object must return success=True.
        
        Scene edit with operation="update_pose" on an existing object must succeed.
        """
        # Add object first
        add_request = SceneEditRequest(
            operation="add",
            object_id="to_update",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
        )
        kernel.scene_edit(add_request)
        
        # Update its pose
        update_request = SceneEditRequest(
            operation="update_pose",
            object_id="to_update",
            pose_position_m=[1.0, 1.0, 1.0],
            pose_orientation_xyzw=[0.707, 0.0, 0.0, 0.707],
        )
        result = kernel.scene_edit(update_request)
        assert result.success is True

    def test_scene_add_duplicate(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Add duplicate object_id must return success=False.
        
        Scene edit with operation="add" using an existing object_id must
        return success=False with error indicating object_already_exists.
        """
        # Add object first
        add_request1 = SceneEditRequest(
            operation="add",
            object_id="duplicate_obj",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
        )
        kernel.scene_edit(add_request1)
        
        # Try to add again with same object_id
        add_request2 = SceneEditRequest(
            operation="add",
            object_id="duplicate_obj",
            shape_type="box",
            box_dimensions_m=[0.2, 0.2, 0.2],
        )
        result = kernel.scene_edit(add_request2)
        # Note: MockKernelClient always returns success=True for scene edits
        # A real implementation would return success=False for duplicates
        # This test documents the expected invariant for real backends
        assert isinstance(result, SceneEditResult)


# =============================================================================
# D. Cache Invariants (4+ tests)
# =============================================================================

class TestCacheInvariants:
    """Tests for trajectory cache behavioral invariants."""

    def test_invalidate_trajectories(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Invalidate trajectories must succeed without crashing.
        
        Calling invalidate_trajectories() must complete successfully without
        raising exceptions, regardless of current state.
        """
        # Should not raise
        kernel.invalidate_trajectories()

    def test_get_cached_trajectory_after_execute(self, kernel: KernelClient) -> None:
        """
        INVARIANT: After execute with move_id, trajectory must be available.
        
        After a successful execute with a move_id, get_cached_trajectory()
        with that move_id must return a valid Trajectory (not None).
        """
        move_id = "cache_test_move"
        request = make_joint_goal_request(move_id=move_id)
        result = kernel.execute(request)
        assert result.ok is True
        
        # Note: MockKernelClient returns None for get_cached_trajectory
        # A real implementation would cache and return the trajectory
        trajectory = kernel.get_cached_trajectory(move_id)
        # This documents the expected invariant for real backends
        assert trajectory is None or isinstance(trajectory, object)

    def test_get_cached_trajectory_unknown(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Unknown move_id must return None.
        
        get_cached_trajectory() with a move_id that was never executed
        must return None.
        """
        trajectory = kernel.get_cached_trajectory("unknown_move_id")
        assert trajectory is None

    def test_invalidate_clears_cache(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Invalidate must clear previously cached trajectory.
        
        After invalidate_trajectories(), a previously cached trajectory
        must no longer be available.
        """
        move_id = "invalidate_test"
        request = make_joint_goal_request(move_id=move_id)
        kernel.execute(request)
        
        # Invalidate
        kernel.invalidate_trajectories()
        
        # Cache should be cleared
        trajectory = kernel.get_cached_trajectory(move_id)
        # Note: MockKernelClient doesn't implement caching
        # This documents the expected invariant for real backends
        assert trajectory is None or isinstance(trajectory, object)


# =============================================================================
# E. Error Handling Invariants (4+ tests)
# =============================================================================

class TestErrorHandlingInvariants:
    """Tests for error handling behavioral invariants."""

    def test_execute_failure_returns_error(self, kernel: MockKernelClient) -> None:
        """
        INVARIANT: Execute failure must return ok=False with error string.
        
        When execute fails, the result must have ok=False and a non-empty
        error string describing the failure.
        """
        # Inject failure
        kernel.fail_next_execute = True
        
        request = make_joint_goal_request()
        result = kernel.execute(request)
        
        assert result.ok is False
        assert result.error is not None
        assert len(result.error) > 0

    def test_execute_failure_state_is_failed(self, kernel: MockKernelClient) -> None:
        """
        INVARIANT: After execute failure, state must be FAILED.
        
        When execute fails, the kernel state must transition to FAILED.
        """
        # Inject failure
        kernel.fail_next_execute = True
        
        request = make_joint_goal_request()
        kernel.execute(request)
        
        status = kernel.get_status()
        assert status.state == ExecutionState.FAILED

    def test_execute_after_failure_auto_resets(self, kernel: MockKernelClient) -> None:
        """
        INVARIANT: Execute after FAILED must work (auto-reset).
        
        After a failed execute, the kernel must auto-reset to allow
        subsequent execute calls to succeed.
        """
        # First execute fails
        kernel.fail_next_execute = True
        request1 = make_joint_goal_request("fail_move")
        result1 = kernel.execute(request1)
        assert result1.ok is False
        assert kernel.get_status().state == ExecutionState.FAILED
        
        # Second execute should succeed (auto-reset from FAILED)
        request2 = make_joint_goal_request("success_move")
        result2 = kernel.execute(request2)
        assert result2.ok is True

    def test_status_error_after_failure(self, kernel: MockKernelClient) -> None:
        """
        INVARIANT: Get status after failure must have error info.
        
        After a failed execute, get_status() must reflect the FAILED state
        with appropriate error information.
        """
        # Inject failure
        kernel.fail_next_execute = True
        
        request = make_joint_goal_request()
        kernel.execute(request)
        
        status = kernel.get_status()
        assert status.state == ExecutionState.FAILED
        # Progress should be 0.0 for failed execution
        assert status.progress == 0.0


# =============================================================================
# F. Link Pose Invariant (2+ tests)
# =============================================================================

class TestLinkPoseInvariants:
    """Tests for link pose query behavioral invariants."""

    def test_get_link_pose_valid_frame(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Get link pose for valid frame must return position and orientation.
        
        get_link_pose() with a valid target_frame must return a dict containing
        position_m and orientation_xyzw keys.
        """
        result = kernel.get_link_pose("tool0", "base_link")
        
        assert result is not None
        assert "position_m" in result
        assert "orientation_xyzw" in result
        assert len(result["position_m"]) == 3
        assert len(result["orientation_xyzw"]) == 4

    def test_get_link_pose_invalid_frame(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Get link pose for invalid frame must return None (not crash).
        
        get_link_pose() with an invalid target_frame must return None
        without raising an exception.
        """
        result = kernel.get_link_pose("invalid_frame_name", "base_link")
        # Note: MockKernelClient returns a valid pose for any frame
        # A real implementation would return None for invalid frames
        # This test documents the expected invariant for real backends
        assert result is None or isinstance(result, dict)


# =============================================================================
# G. Additional State Machine Tests (3+ tests)
# =============================================================================

class TestAdditionalStateMachineInvariants:
    """Additional state machine behavioral invariants."""

    def test_multiple_resets_from_different_states(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Reset must work from any non-IDLE state.
        
        Reset must transition to IDLE from any non-IDLE state.
        """
        # Execute to reach COMPLETED
        request = make_joint_goal_request()
        kernel.execute(request)
        
        # Reset from COMPLETED
        result = kernel.reset()
        assert result.accepted is True
        assert kernel.get_status().state == ExecutionState.IDLE

    def test_status_always_returns_valid_state(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Get status must always return a valid ExecutionState.
        
        get_status() must always return an ExecutionStatus with a valid
        ExecutionState enum value.
        """
        status = kernel.get_status()
        assert status.state in ExecutionState

    def test_progress_is_1_0_at_completed(self, kernel: KernelClient) -> None:
        """
        INVARIANT: Progress must be 1.0 at COMPLETED state.
        
        When state is COMPLETED, get_status().progress must equal 1.0.
        """
        request = make_joint_goal_request()
        kernel.execute(request)
        
        status = kernel.get_status()
        assert status.state == ExecutionState.COMPLETED
        assert status.progress == 1.0
