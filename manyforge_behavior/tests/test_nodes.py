"""Tests for py_trees behavior node implementations."""

import pytest
import py_trees

from manyforge_behavior.kernel_client import (
    ExecutionState,
    MockKernelClient,
)
from manyforge_behavior.nodes.motion import MoveManipulatorAction, ResetTrajectories
from manyforge_behavior.nodes.scene import (
    AddCollisionObject,
    RemoveCollisionObject,
    UpdateCollisionObjectPose,
)
from manyforge_behavior.nodes.logic import (
    CheckKeyBoolValue,
    SetKeyBoolValue,
    WaitForKeyBool,
)
from manyforge_behavior.nodes.geometry import GetLinkPose, CheckPoseDistance


@pytest.fixture(autouse=True)
def reset_blackboard():
    """Reset the global blackboard between tests."""
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


@pytest.fixture
def bb_with_kernel(kernel):
    """Set up blackboard with kernel."""
    client = py_trees.blackboard.Client(name="test_setup")
    client.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    client.kernel = kernel
    return client


def _tick_node(node):
    """Setup and tick a node once."""
    node.setup()
    node.initialise()
    return node.update()


class TestMoveManipulatorAction:
    def test_success(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="test_move",
            move_id="m1",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
        )
        # MockKernelClient instantly completes
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.execute_count == 1

    def test_failure(self, kernel, bb_with_kernel):
        kernel.fail_next_execute = True
        node = MoveManipulatorAction(
            name="test_move_fail",
            move_id="m1",
            motion_type="joint_goal",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE

    def test_from_non_idle_non_terminal(self, kernel, bb_with_kernel):
        """Execute from non-terminal non-IDLE state (e.g. EXECUTING) should fail."""
        kernel._state = ExecutionState.EXECUTING
        node = MoveManipulatorAction(
            name="test_move_busy",
            move_id="m1",
            motion_type="joint_goal",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE

    def test_request_includes_tolerances_and_tcp_cap(self, kernel, bb_with_kernel):
        cap_bb = py_trees.blackboard.Client(name="tcp_cap_writer")
        cap_bb.register_key(
            key="global_max_tcp_linear_speed_mps",
            access=py_trees.common.Access.WRITE,
        )
        cap_bb.global_max_tcp_linear_speed_mps = 0.25

        vs_bb = py_trees.blackboard.Client(name="velocity_writer")
        vs_bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        vs_bb.velocity_scale = 0.8

        node = MoveManipulatorAction(
            name="test_move_params",
            move_id="m_tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
            velocity_scale=0.5,
            acceleration_scale=0.6,
            max_tcp_linear_speed_mps=0.5,
            joint_position_tol_rad=0.02,
            translation_tol_m=0.003,
            rotation_tol_rad=0.04,
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.4)
        assert kernel.last_execute_request.acceleration_scale == pytest.approx(0.6)
        assert kernel.last_execute_request.max_tcp_linear_speed_mps == pytest.approx(0.25)
        assert kernel.last_execute_request.joint_position_tol_rad == pytest.approx(0.02)
        assert kernel.last_execute_request.translation_tol_m == pytest.approx(0.003)
        assert kernel.last_execute_request.rotation_tol_rad == pytest.approx(0.04)

    def test_velocity_scale_does_not_compound_across_async_ticks(self, bb_with_kernel):
        kernel = MockKernelClient(execute_duration_s=0.05)
        bb_with_kernel.kernel = kernel

        vs_bb = py_trees.blackboard.Client(name="velocity_writer")
        vs_bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        vs_bb.velocity_scale = 0.8

        node = MoveManipulatorAction(
            name="test_move_async",
            move_id="m_async",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
            velocity_scale=0.5,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.RUNNING
        for _ in range(5):
            node.update()

        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.4)
        assert node._params["velocity_scale"] == pytest.approx(0.5)

        while node.update() == py_trees.common.Status.RUNNING:
            pass

        node.initialise()
        assert node.update() == py_trees.common.Status.RUNNING
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.4)


class TestResetTrajectories:
    def test_success(self, kernel, bb_with_kernel):
        node = ResetTrajectories(name="test_reset")
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.invalidate_count == 1


class TestAddCollisionObject:
    def test_success(self, kernel, bb_with_kernel):
        node = AddCollisionObject(
            name="add_box",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[1.0, 0.5, 0.1],
            pose={"position_m": [0.5, 0, 0.4], "orientation_xyzw": [0, 0, 0, 1]},
            frame_id="base_link",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].operation == "add"

    def test_failure(self, kernel, bb_with_kernel):
        kernel.fail_next_scene_edit = True
        node = AddCollisionObject(name="add_fail", object_id="fail")
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE


class TestRemoveCollisionObject:
    def test_success(self, kernel, bb_with_kernel):
        node = RemoveCollisionObject(name="remove_box", object_id="box1")
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[0].operation == "remove"


class TestUpdateCollisionObjectPose:
    def test_success(self, kernel, bb_with_kernel):
        node = UpdateCollisionObjectPose(
            name="update_box",
            object_id="box1",
            pose={"position_m": [1.0, 0, 0], "orientation_xyzw": [0, 0, 0, 1]},
            frame_id="base_link",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[0].operation == "update_pose"


class TestCheckKeyBoolValue:
    def test_matching_value(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="flag", access=py_trees.common.Access.WRITE)
        writer.flag = True

        node = CheckKeyBoolValue(name="check_flag", key="flag", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_mismatched_value(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="flag", access=py_trees.common.Access.WRITE)
        writer.flag = False

        node = CheckKeyBoolValue(name="check_flag", key="flag", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE

    def test_missing_key(self, bb_with_kernel):
        node = CheckKeyBoolValue(name="check_missing", key="missing_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE


class TestSetKeyBoolValue:
    def test_sets_value(self, bb_with_kernel):
        node = SetKeyBoolValue(name="set_flag", key="my_flag", value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="my_flag", access=py_trees.common.Access.READ)
        assert reader.my_flag is True


class TestWaitForKeyBool:
    def test_returns_running_when_missing(self, bb_with_kernel):
        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING

    def test_returns_success_when_matched(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="wait_key", access=py_trees.common.Access.WRITE)
        writer.wait_key = True

        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_returns_running_when_mismatched(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="wait_key", access=py_trees.common.Access.WRITE)
        writer.wait_key = False

        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING


class TestGetLinkPose:
    def test_success(self, kernel, bb_with_kernel):
        node = GetLinkPose(
            name="get_pose",
            target_frame="tool0",
            base_frame="base_link",
            output_key="tool_pose",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.link_pose_calls == [("tool0", "base_link")]

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="tool_pose", access=py_trees.common.Access.READ)
        assert "position_m" in reader.tool_pose


class TestCheckPoseDistance:
    def test_within_threshold(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="pose_a", access=py_trees.common.Access.WRITE)
        writer.register_key(key="pose_b", access=py_trees.common.Access.WRITE)
        writer.pose_a = {
            "position_m": [1.0, 0.0, 0.5],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        writer.pose_b = {
            "position_m": [1.001, 0.0, 0.5],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

        node = CheckPoseDistance(
            name="check_dist",
            pose_key_a="pose_a",
            pose_key_b="pose_b",
            translation_threshold_m=0.01,
            rotation_threshold_rad=0.1,
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_exceeds_threshold(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="pose_a", access=py_trees.common.Access.WRITE)
        writer.register_key(key="pose_b", access=py_trees.common.Access.WRITE)
        writer.pose_a = {
            "position_m": [0.0, 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        writer.pose_b = {
            "position_m": [1.0, 1.0, 1.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

        node = CheckPoseDistance(
            name="check_dist_fail",
            pose_key_a="pose_a",
            pose_key_b="pose_b",
            translation_threshold_m=0.01,
            rotation_threshold_rad=0.1,
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE
