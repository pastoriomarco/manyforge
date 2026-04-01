"""Full pick-and-place pipeline tests exercising the ManyForge behavior stack.

Tests exercise: program YAML → OrchestratorRunner → KernelClient →
scene edits + motions + blackboard logic + cache invalidation.

Uses MockKernelClient for runner/pipeline tests.
"""

from pathlib import Path
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[1]

from manyforge_behavior.kernel_client import (
    MockKernelClient, ExecuteRequest, ExecutionState, SceneEditRequest,
)
from manyforge_behavior.runner import OrchestratorRunner


# ============================================================================
# Fixtures
# ============================================================================

CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
PICK_PLACE_PROGRAM = str(_REPO_ROOT / "tests" / "fixtures" / "pick_and_place.program.yaml")
SIMPLE_PROGRAM = str(_REPO_ROOT / "tests" / "fixtures" / "simple_motion.program.yaml")


@pytest.fixture
def kernel():
    """Create a MockKernelClient."""
    return MockKernelClient()


# ============================================================================
# Full pipeline: pick and place
# ============================================================================

class TestPickAndPlacePipeline:
    """Full pick-and-place pipeline through MockKernelClient."""

    def test_pick_place_program_loads(self, kernel):
        """Program with scene edits loads and validates."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
        )
        runner.load()
        tree = runner.build_tree()
        assert tree is not None

    def test_pick_place_apply_scene(self, kernel):
        """apply_scene() adds the table from the scene section."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        # MockKernelClient records scene edits
        assert len(kernel.scene_edits) > 0

    def test_pick_place_full_run(self, kernel):
        """Full pick-and-place program runs to completion."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        success = runner.run(max_ticks=200, tick_period_s=0.0)
        assert success is True

    def test_pick_place_state_after_completion(self, kernel):
        """Kernel is in COMPLETED state after full run."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        runner.run(max_ticks=200, tick_period_s=0.0)

        status = kernel.get_status()
        assert status.state == ExecutionState.COMPLETED

    def test_pick_place_reset_and_rerun(self, kernel):
        """Program can be reset and run again."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()

        success1 = runner.run(max_ticks=200, tick_period_s=0.0)
        assert success1 is True

        kernel.reset()
        assert kernel.get_status().state == ExecutionState.IDLE

        runner.build_tree()
        success2 = runner.run(max_ticks=200, tick_period_s=0.0)
        assert success2 is True

    def test_pick_place_with_parameter_override(self, kernel):
        """velocity_scale override propagates through pick-and-place."""
        runner = OrchestratorRunner(
            catalog_path=CATALOG_PATH,
            program_path=PICK_PLACE_PROGRAM,
            kernel=kernel,
            overrides={"velocity_scale": 0.3},
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        success = runner.run(max_ticks=200, tick_period_s=0.0)
        assert success is True


# ============================================================================
# Scene edit operations
# ============================================================================

class TestSceneEditPipeline:
    """Scene edit operations through MockKernelClient."""

    def test_add_box_object(self, kernel):
        """Add a box collision object."""
        request = SceneEditRequest(
            operation="add", object_id="box1", shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        )
        result = kernel.scene_edit(request)
        assert result.success is True

    def test_add_sphere_object(self, kernel):
        """Add a sphere collision object."""
        request = SceneEditRequest(
            operation="add", object_id="sphere1", shape_type="sphere",
            sphere_radius_m=0.05,
            pose_position_m=[0.3, 0.1, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        )
        result = kernel.scene_edit(request)
        assert result.success is True

    def test_remove_object(self, kernel):
        """Add then remove a collision object."""
        kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="temp_obj", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ))
        result = kernel.scene_edit(SceneEditRequest(
            operation="remove", object_id="temp_obj",
        ))
        assert result.success is True

    def test_update_object_pose(self, kernel):
        """Add then update pose of a collision object."""
        kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="movable", shape_type="sphere",
            sphere_radius_m=0.04,
            pose_position_m=[0.3, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ))
        result = kernel.scene_edit(SceneEditRequest(
            operation="update_pose", object_id="movable",
            pose_position_m=[0.6, 0.2, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ))
        assert result.success is True

    def test_scene_edit_then_motion(self, kernel):
        """Add scene objects, then plan and execute motion."""
        kernel.scene_edit(SceneEditRequest(
            operation="add", object_id="obstacle", shape_type="box",
            box_dimensions_m=[0.2, 0.2, 0.4],
            pose_position_m=[0.4, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ))

        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="avoid_obstacle",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result = kernel.execute(request)
        assert result.ok is True

    def test_get_link_pose(self, kernel):
        """Forward kinematics query returns pose dict."""
        pose = kernel.get_link_pose("tcp", "base_link")
        assert pose is not None
        assert "position_m" in pose
        assert "orientation_xyzw" in pose
        assert len(pose["position_m"]) == 3
        assert len(pose["orientation_xyzw"]) == 4


# ============================================================================
# Error recovery pipeline
# ============================================================================

class TestErrorRecoveryPipeline:
    """Error paths and recovery through MockKernelClient."""

    def test_planning_failure_then_recovery(self, kernel):
        """Execute failure → kernel enters FAILED → reset → re-execute succeeds."""
        kernel.fail_next_execute = True

        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="failing_move",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )

        result = kernel.execute(request)
        assert result.ok is False
        assert kernel.get_status().state == ExecutionState.FAILED

        reset_result = kernel.reset()
        assert reset_result.accepted is True
        assert kernel.get_status().state == ExecutionState.IDLE

        result2 = kernel.execute(request)
        assert result2.ok is True
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_multi_failure_recovery_cycle(self, kernel):
        """Multiple failure→reset→success cycles work correctly."""
        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="cycle_move",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )

        for i in range(3):
            # Fail
            kernel.fail_next_execute = True
            result = kernel.execute(request)
            assert result.ok is False, f"Cycle {i}: should fail"

            # Reset
            kernel.reset()

            # Succeed
            result = kernel.execute(request)
            assert result.ok is True, f"Cycle {i}: should succeed"

            # Reset for next cycle
            kernel.reset()


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])
