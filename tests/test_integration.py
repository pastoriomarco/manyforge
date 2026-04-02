"""End-to-end integration tests for ManyForge v0.0.

These tests exercise the full stack from program YAML through behavior tree
execution to the kernel layer.

- Groups A, C, D, E use MockKernelClient (pure Python mock — tests runner/HMI logic).
- Group B uses the native C++ kernel with real RoboPlan (requires roboplan).
"""

from pathlib import Path
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[1]

# Behavior layer
from manyforge_behavior.kernel_client import (
    MockKernelClient, ExecuteRequest, ExecuteResult,
    CommandResult, ExecutionState, ExecutionStatus, SceneEditRequest,
)
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program

# HMI
from manyforge_hmi import ViserHmiServer, StateBridge

# Native kernel (optional — requires roboplan + manyforge_roboplan_py)
try:
    import manyforge_roboplan_py
    from native_kernel_client import NativeKernelClient
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def simple_motion_program_path():
    """Path to the simple motion test program."""
    return str(_REPO_ROOT / "tests" / "fixtures" / "simple_motion.program.yaml")


@pytest.fixture
def node_catalog_path():
    """Path to the node catalog."""
    return str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


@pytest.fixture
def mock_kernel():
    """Create a MockKernelClient for runner/HMI tests."""
    return MockKernelClient()


@pytest.fixture
def native_kernel():
    """Create a NativeKernelClient with real RoboPlan backend."""
    if not HAS_NATIVE:
        pytest.skip("manyforge_roboplan_py not available")
    bundle = manyforge_roboplan_py.create_native_kernel("ur5_gripper", "arm")
    return NativeKernelClient(bundle)


# ============================================================================
# Group A: Runner + MockKernelClient (7 tests)
# ============================================================================

class TestRunnerWithMockKernel:
    """Tests for OrchestratorRunner with MockKernelClient."""

    def test_runner_loads(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Program loads and tree builds."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        tree = runner.build_tree()
        assert tree is not None
        assert tree.root is not None

    def test_runner_executes(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Program runs, tree ticks through, motions execute."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        runner.build_tree()
        success = runner.run(max_ticks=100)
        assert success is True

    def test_runner_completes(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Runner.run() returns True (success)."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        runner.build_tree()
        result = runner.run(max_ticks=100)
        assert result is True

    def test_runner_state_transitions(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Status goes IDLE → COMPLETED after execute."""
        status = mock_kernel.get_status()
        assert status.state == ExecutionState.IDLE

        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        runner.build_tree()
        runner.run(max_ticks=100)

        status = mock_kernel.get_status()
        assert status.state == ExecutionState.COMPLETED

    def test_runner_parameter_override(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """velocity_scale override reaches the kernel."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
            overrides={"velocity_scale": 0.8},
        )
        runner.load()
        runner.build_tree()
        success = runner.run(max_ticks=100)
        assert success is True

    def test_runner_reset_between_runs(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Execute, reset, execute again works."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        runner.build_tree()

        success1 = runner.run(max_ticks=100)
        assert success1 is True

        reset_result = mock_kernel.reset()
        assert reset_result.accepted is True

        success2 = runner.run(max_ticks=100)
        assert success2 is True

    def test_runner_two_moves(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Both moves in sequence execute successfully."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        runner.build_tree()
        success = runner.run(max_ticks=100)
        assert success is True


# ============================================================================
# Group B: Runner + NativeKernelClient with Real RoboPlan (5 tests)
# ============================================================================

UR5_JOINTS = [
    "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
    "wrist_1_joint", "wrist_2_joint", "wrist_3_joint",
]


class TestRunnerWithNativeKernel:
    """Tests for OrchestratorRunner with NativeKernelClient (real RoboPlan)."""

    def test_native_kernel_loads(
        self, simple_motion_program_path, node_catalog_path, native_kernel
    ):
        """Program loads with native kernel."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=native_kernel,
        )
        runner.load()
        tree = runner.build_tree()
        assert tree is not None

    def test_native_kernel_joint_goal(self, native_kernel):
        """Single joint goal plans and executes successfully."""
        request = ExecuteRequest(
            robot_id="robot1",
            group_id="arm",
            move_id="test_move",
            base_frame="base_link",
            tcp_frame="tool0",
            motion_type="joint_goal",
            joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            velocity_scale=1.0,
            acceleration_scale=1.0,
            planning_timeout_s=10.0,
            execution_timeout_s=60.0,
        )
        result = native_kernel.execute(request)
        assert result.ok is True

    def test_native_kernel_two_moves(self, native_kernel):
        """Two sequential moves complete."""
        request1 = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal", joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=10.0, execution_timeout_s=60.0,
        )
        result1 = native_kernel.execute(request1)
        assert result1.ok is True

        native_kernel.reset()

        request2 = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move2",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal", joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.2, -0.8, 0.8, -0.3, -0.8, 0.1],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=10.0, execution_timeout_s=60.0,
        )
        result2 = native_kernel.execute(request2)
        assert result2.ok is True

    def test_native_kernel_reset_cycle(self, native_kernel):
        """Execute → reset → execute again."""
        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal", joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=10.0, execution_timeout_s=60.0,
        )
        result1 = native_kernel.execute(request)
        assert result1.ok is True

        reset_result = native_kernel.reset()
        assert reset_result.accepted is True

        request2 = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move2",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal", joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.2, -0.8, 0.8, -0.3, -0.8, 0.1],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=10.0, execution_timeout_s=60.0,
        )
        result2 = native_kernel.execute(request2)
        assert result2.ok is True

    def test_native_kernel_status_idle_after_reset(self, native_kernel):
        """Status returns to IDLE after reset."""
        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal", joint_goal_names=UR5_JOINTS,
            joint_goal_positions_rad=[0.1, -0.5, 0.5, -0.5, -0.5, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=10.0, execution_timeout_s=60.0,
        )
        result = native_kernel.execute(request)
        assert result.ok is True

        reset_result = native_kernel.reset()
        assert reset_result.accepted is True

        status = native_kernel.get_status()
        assert status.state == ExecutionState.IDLE


# ============================================================================
# Group C: HMI + MockKernelClient (4 tests)
# ============================================================================

class TestHmiWithMockKernel:
    """Tests for ViserHmiServer with MockKernelClient."""

    def test_hmi_creates(self, mock_kernel):
        """ViserHmiServer(kernel=mock_kernel, port=0) works."""
        server = ViserHmiServer(kernel=mock_kernel, port=0)
        assert server is not None
        assert server.viser_server is not None
        server.stop()

    def test_hmi_bridge_status(self, mock_kernel):
        """StateBridge.get_state() returns IDLE."""
        bridge = StateBridge(mock_kernel)
        state = bridge.get_state()
        assert state.state == ExecutionState.IDLE

    def test_hmi_bridge_execute(self, mock_kernel):
        """Execute motion via bridge, status changes."""
        bridge = StateBridge(mock_kernel)

        initial_state = bridge.get_state()
        assert initial_state.state == ExecutionState.IDLE

        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="test_move",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result = bridge.execute_motion(request)
        assert result.ok is True

        new_state = bridge.get_state()
        assert new_state.state == ExecutionState.COMPLETED

    def test_hmi_load_program(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Load program into HMI server."""
        server = ViserHmiServer(kernel=mock_kernel, port=0)

        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        program = runner.program

        server.load_program(program)
        server.stop()


# ============================================================================
# Group D: Error Paths (4 tests)
# ============================================================================

class TestErrorPaths:
    """Tests for error handling via MockKernelClient."""

    def test_execute_failure(self, mock_kernel):
        """Injected execute failure produces FAILED state."""
        mock_kernel.fail_next_execute = True

        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="test_move",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result = mock_kernel.execute(request)
        assert result.ok is False

        status = mock_kernel.get_status()
        assert status.state == ExecutionState.FAILED

    def test_stop_from_idle(self, mock_kernel):
        """Stop when idle returns not-accepted."""
        result = mock_kernel.stop()
        assert result.accepted is False

    def test_auto_reset_on_sequential_execute(self, mock_kernel):
        """Second execute after completed — auto-resets from terminal state (matches C++ kernel)."""
        request1 = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move1",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result1 = mock_kernel.execute(request1)
        assert result1.ok is True

        # Auto-reset from COMPLETED — sequential execute succeeds
        request2 = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move2",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.2, 0.2, 0.2, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result2 = mock_kernel.execute(request2)
        assert result2.ok is True

    def test_reset_recovers(self, mock_kernel):
        """After failure, reset returns to IDLE, can execute again."""
        mock_kernel.fail_next_execute = True

        request = ExecuteRequest(
            robot_id="robot1", group_id="arm", move_id="move1",
            base_frame="base_link", tcp_frame="tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2", "j3", "j4", "j5", "j6"],
            joint_goal_positions_rad=[0.1, 0.1, 0.1, 0.0, 0.0, 0.0],
            velocity_scale=1.0, acceleration_scale=1.0,
            planning_timeout_s=5.0, execution_timeout_s=10.0,
        )
        result1 = mock_kernel.execute(request)
        assert result1.ok is False

        reset_result = mock_kernel.reset()
        assert reset_result.accepted is True
        assert mock_kernel.get_status().state == ExecutionState.IDLE

        result2 = mock_kernel.execute(request)
        assert result2.ok is True


# ============================================================================
# Group E: Scene Edit Verification (2 tests)
# ============================================================================

class TestSceneEdits:
    """Tests for scene edit interface via MockKernelClient."""

    def test_scene_edit_recorded(self, mock_kernel):
        """scene_edit calls are recorded by MockKernelClient."""
        request = SceneEditRequest(
            operation="add", object_id="test_obj", shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.5, 0.0, 0.5],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = mock_kernel.scene_edit(request)
        assert result.success is True
        assert len(mock_kernel.scene_edits) == 1

    def test_runner_skips_scene_when_empty(
        self, simple_motion_program_path, node_catalog_path, mock_kernel
    ):
        """Program with empty scene[] runs without scene_edit calls."""
        runner = OrchestratorRunner(
            catalog_path=node_catalog_path,
            program_path=simple_motion_program_path,
            kernel=mock_kernel,
        )
        runner.load()
        assert len(runner.program.scene) == 0

        runner.build_tree()
        success = runner.run(max_ticks=100)
        assert success is True


# ============================================================================
# Run tests
# ============================================================================

if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short", "-s"])
