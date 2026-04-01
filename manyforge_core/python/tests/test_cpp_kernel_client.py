"""Tests for CppKernelClient."""

import pytest
import manyforge_core_py
from manyforge_behavior.kernel_client import (
    KernelClient,
    ExecuteRequest,
    ExecutionState,
)

from cpp_kernel_client import CppKernelClient


class MockPlannerBackend(manyforge_core_py.IPlannerBackend):
    """Mock planner backend."""

    def __init__(self):
        super().__init__()

    def plan(self, request: manyforge_core_py.PlanRequestDTO) -> manyforge_core_py.PlanResultDTO:
        result = manyforge_core_py.PlanResultDTO()
        wp = manyforge_core_py.TrajectoryWaypointDTO(0.0, [0.0, 0.0], [], [])
        wp2 = manyforge_core_py.TrajectoryWaypointDTO(1.0, [0.5, 0.5], [], [])
        result.trajectory = manyforge_core_py.TrajectoryDTO(["j1", "j2"], [wp, wp2])
        return result


class MockPlanningContext(manyforge_core_py.IPlanningContext):
    """Mock planning context."""

    def __init__(self):
        super().__init__()

    def get_current_state(self) -> manyforge_core_py.RobotStateDTO:
        return manyforge_core_py.RobotStateDTO(["j1", "j2"], [0.0, 0.0], [])

    def validate_trajectory(
        self, trajectory: manyforge_core_py.TrajectoryDTO,
        options: manyforge_core_py.ValidationOptionsDTO
    ) -> manyforge_core_py.ValidationResultDTO:
        result = manyforge_core_py.ValidationResultDTO()
        result.valid = True
        return result

    def collision_check(
        self, request: manyforge_core_py.CollisionCheckRequestDTO
    ) -> manyforge_core_py.CollisionCheckResultDTO:
        result = manyforge_core_py.CollisionCheckResultDTO()
        result.in_collision = False
        return result

    def forward_kinematics(
        self, request: manyforge_core_py.FkRequestDTO
    ) -> manyforge_core_py.Pose3DTO:
        return manyforge_core_py.Pose3DTO(
            manyforge_core_py.Vector3DTO(0.5, 0.0, 0.6),
            manyforge_core_py.QuaternionDTO(0.0, 0.0, 0.0, 1.0)
        )

    def get_joint_limits(self) -> manyforge_core_py.JointLimitsDTO:
        return manyforge_core_py.JointLimitsDTO(
            ["j1", "j2"], [-3.14, -3.14], [3.14, 3.14], [1.0, 1.0], []
        )

    def add_object(
        self, request: manyforge_core_py.AddObjectRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        result = manyforge_core_py.SceneEditResultDTO()
        result.success = True
        return result

    def remove_object(
        self, request: manyforge_core_py.RemoveObjectRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        result = manyforge_core_py.SceneEditResultDTO()
        result.success = True
        return result

    def update_object_pose(
        self, request: manyforge_core_py.UpdateObjectPoseRequestDTO
    ) -> manyforge_core_py.SceneEditResultDTO:
        result = manyforge_core_py.SceneEditResultDTO()
        result.success = True
        return result


class MockExecutionBackend(manyforge_core_py.IExecutionBackend):
    """Mock execution backend."""

    def __init__(self):
        super().__init__()

    def execute_trajectory(
        self, trajectory: manyforge_core_py.TrajectoryDTO
    ) -> manyforge_core_py.ExecutionHandle:
        return manyforge_core_py.ExecutionHandle()

    def stop(self) -> manyforge_core_py.StopResultDTO:
        result = manyforge_core_py.StopResultDTO()
        result.accepted = True
        return result

    def pause(self) -> manyforge_core_py.PauseResultDTO:
        result = manyforge_core_py.PauseResultDTO()
        result.accepted = True
        return result

    def resume(self, trajectory: manyforge_core_py.TrajectoryDTO) -> manyforge_core_py.ResumeResultDTO:
        result = manyforge_core_py.ResumeResultDTO()
        result.accepted = True
        return result

    def reset(self) -> manyforge_core_py.ResetResultDTO:
        result = manyforge_core_py.ResetResultDTO()
        result.accepted = True
        return result


class MockClock(manyforge_core_py.IClock):
    """Mock clock."""

    def __init__(self):
        super().__init__()
        self._time = 0

    def now(self) -> int:
        self._time += 1
        return self._time


class MockScheduler(manyforge_core_py.IScheduler):
    """Mock scheduler."""

    def __init__(self):
        super().__init__()
        self._callbacks = []

    def schedule(self, deadline: int, callback) -> int:
        handle = len(self._callbacks)
        self._callbacks.append((deadline, callback))
        return handle

    def cancel(self, handle: int) -> bool:
        if 0 <= handle < len(self._callbacks):
            self._callbacks[handle] = None
            return True
        return False


def create_client_with_mocks():
    """Create a CppKernelClient with mock backends."""
    planner = MockPlannerBackend()
    context = MockPlanningContext()
    execution = MockExecutionBackend()
    clock = MockClock()
    scheduler = MockScheduler()
    cache = manyforge_core_py.TrajectoryCache()

    return CppKernelClient(planner, context, execution, clock, scheduler, cache)


class TestCppKernelClient:
    """Tests for CppKernelClient."""

    def test_is_subclass_of_kernel_client(self):
        """Verify CppKernelClient is a proper subclass of KernelClient."""
        assert issubclass(CppKernelClient, KernelClient)

    def test_client_creation(self):
        """Test that client can be created with mock backends."""
        client = create_client_with_mocks()
        assert client is not None

    def test_initial_state_is_idle(self):
        """Test that client starts in IDLE state."""
        client = create_client_with_mocks()
        status = client.get_status()
        assert status.state == ExecutionState.IDLE

    def test_execute_joint_goal(self):
        """Test execute() with a joint goal request."""
        client = create_client_with_mocks()

        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move1",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )

        result = client.execute(request)
        assert result.ok is True
        assert result.execution_id > 0

    def test_execute_pose_goal(self):
        """Test execute() with a pose goal request."""
        client = create_client_with_mocks()

        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move2",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="pose_goal",
            pose_goal={
                "position_m": [0.5, 0.0, 0.6],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        )

        result = client.execute(request)
        assert result.ok is True
        assert result.execution_id > 0

    def test_stop_command(self):
        """Test stop() after synchronous execute (already completed)."""
        client = create_client_with_mocks()

        # Execute first — mock backend completes synchronously
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move3",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )
        client.execute(request)

        # Kernel is already in Completed state — stop is rejected (correct behavior)
        result = client.stop()
        assert result.accepted is False

    def test_get_status_after_execute(self):
        """Test that get_status() returns correct state after execute."""
        client = create_client_with_mocks()

        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move4",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )
        client.execute(request)

        status = client.get_status()
        # After execute, state should be COMPLETED (mock backend completes immediately)
        assert status.state == ExecutionState.COMPLETED
        assert status.execution_id > 0

    def test_pause_command(self):
        """Test pause() after synchronous execute (already completed)."""
        client = create_client_with_mocks()

        # Execute first — mock backend completes synchronously
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move5",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )
        client.execute(request)

        # Kernel is already in Completed state — pause is rejected (correct behavior)
        result = client.pause()
        assert result.accepted is False

    def test_resume_command(self):
        """Test resume() after synchronous execute (already completed)."""
        client = create_client_with_mocks()

        # Execute first — mock backend completes synchronously
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move6",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )
        client.execute(request)

        # Kernel is already in Completed state — resume is rejected (correct behavior)
        result = client.resume()
        assert result.accepted is False

    def test_reset_command(self):
        """Test reset() command."""
        client = create_client_with_mocks()

        # Execute first
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="group1",
            move_id="move7",
            base_frame="base_link",
            tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.5, 1.0],
        )
        client.execute(request)

        result = client.reset()
        assert result.accepted is True

        # After reset, state should be IDLE
        status = client.get_status()
        assert status.state == ExecutionState.IDLE

    def test_invalidate_trajectories(self):
        """Test invalidate_trajectories() method."""
        client = create_client_with_mocks()

        # This should not raise any exceptions
        client.invalidate_trajectories()

    def test_scene_edit_not_supported(self):
        """Test that scene_edit returns not supported error."""
        from manyforge_behavior.kernel_client import SceneEditRequest

        client = create_client_with_mocks()

        request = SceneEditRequest(
            operation="add",
            object_id="obj1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
        )

        result = client.scene_edit(request)
        assert result.success is False
        assert "not supported" in result.error.lower()

    def test_get_link_pose_not_supported(self):
        """Test that get_link_pose returns None."""
        client = create_client_with_mocks()

        result = client.get_link_pose("tcp", "base_link")
        assert result is None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
