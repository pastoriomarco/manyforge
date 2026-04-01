"""Tests for the mock kernel client."""

import pytest

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    MockKernelClient,
    SceneEditRequest,
)


@pytest.fixture
def kernel():
    return MockKernelClient()


def make_request(move_id: str = "move_1") -> ExecuteRequest:
    return ExecuteRequest(
        robot_id="ur5",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2"],
        joint_goal_positions_rad=[0.5, 1.0],
    )


class TestMockKernelExecute:
    def test_execute_from_idle(self, kernel):
        result = kernel.execute(make_request())
        assert result.ok
        assert result.execution_id == 1
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_execute_increments_id(self, kernel):
        kernel.execute(make_request())
        kernel.reset()
        result = kernel.execute(make_request("move_2"))
        assert result.execution_id == 2

    def test_execute_from_non_idle_fails(self, kernel):
        kernel.execute(make_request())
        result = kernel.execute(make_request("move_2"))
        assert not result.ok

    def test_execute_failure_injection(self, kernel):
        kernel.fail_next_execute = True
        result = kernel.execute(make_request())
        assert not result.ok
        assert kernel.get_status().state == ExecutionState.FAILED


class TestMockKernelStop:
    def test_stop_from_idle_rejected(self, kernel):
        result = kernel.stop()
        assert not result.accepted

    def test_stop_from_completed_rejected(self, kernel):
        kernel.execute(make_request())
        result = kernel.stop()
        assert not result.accepted

    def test_stop_count(self, kernel):
        kernel.stop()
        assert kernel.stop_count == 1


class TestMockKernelPauseResume:
    def test_pause_from_idle_rejected(self, kernel):
        result = kernel.pause()
        assert not result.accepted

    def test_resume_from_idle_rejected(self, kernel):
        result = kernel.resume()
        assert not result.accepted


class TestMockKernelReset:
    def test_reset_from_idle_rejected(self, kernel):
        result = kernel.reset()
        assert not result.accepted

    def test_reset_from_completed(self, kernel):
        kernel.execute(make_request())
        result = kernel.reset()
        assert result.accepted
        assert kernel.get_status().state == ExecutionState.IDLE

    def test_reset_from_failed(self, kernel):
        kernel.fail_next_execute = True
        kernel.execute(make_request())
        result = kernel.reset()
        assert result.accepted
        assert kernel.get_status().state == ExecutionState.IDLE


class TestMockKernelSceneEdit:
    def test_scene_edit_records(self, kernel):
        req = SceneEditRequest(operation="add", object_id="box1")
        result = kernel.scene_edit(req)
        assert result.success
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].object_id == "box1"

    def test_scene_edit_failure_injection(self, kernel):
        kernel.fail_next_scene_edit = True
        req = SceneEditRequest(operation="add", object_id="box1")
        result = kernel.scene_edit(req)
        assert not result.success


class TestMockKernelMisc:
    def test_invalidate_trajectories(self, kernel):
        kernel.invalidate_trajectories()
        kernel.invalidate_trajectories()
        assert kernel.invalidate_count == 2

    def test_get_link_pose(self, kernel):
        pose = kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert "position_m" in pose
        assert "orientation_xyzw" in pose
        assert len(kernel.link_pose_calls) == 1

    def test_initial_status(self, kernel):
        status = kernel.get_status()
        assert status.state == ExecutionState.IDLE
        assert status.execution_id == 0
        assert status.progress == 0.0
