"""Tests for the mock kernel client."""

import pytest

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    MockKernelClient,
    ObjectState,
    SceneObjectSnapshot,
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

    def test_execute_from_terminal_state_requires_acknowledgement(self, kernel):
        first = kernel.execute(make_request())
        assert kernel.get_status().state == ExecutionState.COMPLETED
        result = kernel.execute(make_request("move_2"))
        assert not result.ok

        stale_ack = kernel.acknowledge_completed(first.execution_id + 1)
        assert not stale_ack.accepted
        assert kernel.get_status().state == ExecutionState.COMPLETED

        ack = kernel.acknowledge_completed(first.execution_id)
        assert ack.accepted
        assert kernel.get_status().state == ExecutionState.IDLE

        result = kernel.execute(make_request("move_2"))
        assert result.ok

    def test_execute_from_non_terminal_non_idle_fails(self, kernel):
        kernel._state = ExecutionState.EXECUTING
        result = kernel.execute(make_request())
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
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
        )
        result = kernel.scene_edit(req)
        assert result.success
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].object_id == "box1"

    def test_scene_edit_failure_injection(self, kernel):
        kernel.fail_next_scene_edit = True
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
        )
        result = kernel.scene_edit(req)
        assert not result.success

    def test_scene_edit_invalid_operation_fails(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(operation="teleport", object_id="box1"))
        assert result.success is False
        assert "Unsupported scene edit operation" in (result.error or "")

    def test_scene_edit_remove_missing_fails(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(operation="remove", object_id="missing"))
        assert result.success is False
        assert result.error == "Object not found"

    def test_scene_trace_records_scene_mutations(self, kernel):
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        kernel.scene_edit(SceneEditRequest(
            operation="attach",
            object_id="box1",
            frame_id="tool0",
        ))
        trace = kernel.get_scene_trace()
        assert [event["event"] for event in trace][-2:] == ["add", "attach"]
        assert trace[-1]["success"] is True
        assert trace[-1]["frame_id"] == "tool0"
        assert trace[-1]["attached"] is True
        assert trace[-1]["pose_in_frame"] is not None
        assert trace[-1]["pose_in_universe"] is not None


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

    def test_get_object_pose_and_state_follow_backend_scene_edits(self, kernel):
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))

        pose = kernel.get_object_pose("box1", "base_link")
        assert pose is not None
        assert pose["position_m"] == [0.4, 0.1, 0.2]

        state = kernel.get_object_state("box1")
        assert isinstance(state, ObjectState)
        assert state.exists is True
        assert state.attached is False
        assert state.frame_id == "universe"

    def test_get_object_state_reports_missing_object(self, kernel):
        state = kernel.get_object_state("missing_box")
        assert state.object_id == "missing_box"
        assert state.exists is False
        assert kernel.get_object_pose("missing_box", "base_link") is None

    def test_get_scene_objects_returns_authoritative_shape_and_pose(self, kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="sphere1",
                shape_type="sphere",
                sphere_radius_m=0.05,
                pose_position_m=[0.1, 0.2, 0.3],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        )

        objects = kernel.get_scene_objects()
        assert len(objects) == 1
        assert isinstance(objects[0], SceneObjectSnapshot)
        assert objects[0].object_id == "sphere1"
        assert objects[0].shape is not None
        assert objects[0].shape.type == "sphere"
        assert objects[0].shape.sphere_radius_m == pytest.approx(0.05)
        assert objects[0].pose_in_universe is not None
        assert objects[0].pose_in_universe["position_m"] == pytest.approx([0.1, 0.2, 0.3])

    def test_attach_and_detach_preserve_world_pose_with_stable_object_id(self, kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.05, 0.05, 0.05],
                pose_position_m=[0.4, 0.1, 0.2],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        )
        pose_before = kernel.get_object_pose("box1", "universe")
        assert pose_before is not None

        attach_result = kernel.scene_edit(
            SceneEditRequest(operation="attach", object_id="box1", frame_id="tool0")
        )
        assert attach_result.success is True
        attached_state = kernel.get_object_state("box1")
        assert attached_state.attached is True
        assert attached_state.frame_id == "tool0"
        pose_attached = kernel.get_object_pose("box1", "universe")
        assert pose_attached is not None
        assert pose_attached["position_m"] == pytest.approx(pose_before["position_m"])

        detach_result = kernel.scene_edit(
            SceneEditRequest(operation="detach", object_id="box1", frame_id="universe")
        )
        assert detach_result.success is True
        detached_state = kernel.get_object_state("box1")
        assert detached_state.attached is False
        assert detached_state.frame_id == "universe"
        pose_detached = kernel.get_object_pose("box1", "universe")
        assert pose_detached is not None
        assert pose_detached["position_m"] == pytest.approx(pose_attached["position_m"])

    def test_initial_status(self, kernel):
        status = kernel.get_status()
        assert status.state == ExecutionState.IDLE
        assert status.execution_id == 0
        assert status.progress == 0.0
