"""Tests for the CycleManager."""

from pathlib import Path
import time
import pytest
import py_trees

from manyforge_behavior.kernel_client import ExecutionState, MockKernelClient
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.cycle_manager import CycleManager, CycleState

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


class TestCycleManagerInitialState:
    """Test initial state of CycleManager."""
    
    def test_initial_state_idle(self, kernel):
        """CycleManager starts in IDLE state."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.state == CycleState.IDLE
    
    def test_velocity_scale_default_1_0(self, kernel):
        """Velocity scale defaults to 1.0."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.velocity_scale == 1.0


class TestCycleManagerRun:
    """Test run() method."""
    
    def test_run_from_idle_starts_ticking(self, kernel):
        """run() from IDLE builds tree and starts tick thread."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.run()
        
        assert manager.state == CycleState.RUNNING
        # Give tick thread time to run
        time.sleep(0.2)
        # Tree should complete quickly, so state may change
        # But we know it started running
    
    def test_run_from_running_is_noop(self, kernel):
        """run() from RUNNING state is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        # Call run again — should be no-op
        manager.run()
        assert manager.state == CycleState.RUNNING
    
    def test_run_from_stopped_resumes(self, kernel):
        """run() from STOPPED resumes execution."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        # Run then stop
        manager.run()
        time.sleep(0.2)
        manager.stop()
        assert manager.state == CycleState.STOPPED
        
        # Run again — should resume
        manager.run()
        assert manager.state == CycleState.RUNNING


class TestCycleManagerStop:
    """Test stop() method."""
    
    def test_stop_from_running(self, kernel):
        """stop() from RUNNING sets state to STOPPED."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        manager.stop()
        assert manager.state == CycleState.STOPPED
    
    def test_stop_from_stopped_is_noop(self, kernel):
        """stop() from STOPPED is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.stop()
        assert manager.state == CycleState.STOPPED
        
        # Stop again — should be no-op
        manager.stop()
        assert manager.state == CycleState.STOPPED
    
    def test_stop_from_idle_is_noop(self, kernel):
        """stop() from IDLE is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.stop()
        assert manager.state == CycleState.IDLE
    
    def test_stop_calls_kernel_pause_if_executing(self, kernel):
        """stop() calls kernel.pause() if kernel is EXECUTING."""
        # Create a kernel that's in EXECUTING state
        kernel._state = ExecutionState.EXECUTING
        
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        time.sleep(0.1)
        manager.stop()
        
        # Kernel should have been paused
        assert kernel.pause_count == 1


class TestCycleManagerReset:
    """Test reset() method."""
    
    def test_reset_from_running(self, kernel):
        """reset() from RUNNING stops then resets."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        manager.reset()
        assert manager.state == CycleState.IDLE
    
    def test_reset_from_stopped(self, kernel):
        """reset() from STOPPED resets to IDLE."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.stop()
        assert manager.state == CycleState.STOPPED
        
        manager.reset()
        assert manager.state == CycleState.IDLE
    
    def test_reset_from_idle_is_noop(self, kernel):
        """reset() from IDLE is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        initial_reset_count = kernel.reset_count
        
        manager.reset()
        assert manager.state == CycleState.IDLE
        # Reset count should not have increased
        assert kernel.reset_count == initial_reset_count


class TestCycleManagerVelocityScale:
    """Test velocity scale functionality."""
    
    def test_velocity_scale_clamped(self, kernel):
        """velocity_scale is clamped to [0.0, 1.0]."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        # Test clamping below 0
        manager.set_velocity_scale(-0.5)
        assert manager.velocity_scale == 0.0
        
        # Test clamping above 1
        manager.set_velocity_scale(2.0)
        assert manager.velocity_scale == 1.0
        
        # Test valid value
        manager.set_velocity_scale(0.5)
        assert manager.velocity_scale == 0.5
    
    def test_velocity_scale_applied_to_motion(self, kernel):
        """velocity_scale is written to blackboard and accessible."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.set_velocity_scale(0.75)
        
        # Check blackboard has the value
        bb = py_trees.blackboard.Client(name="test_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.READ)
        assert bb.velocity_scale == 0.75
    
    def test_stop_flag_blocks_new_motion(self, kernel):
        """stop_requested flag is written to blackboard."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        time.sleep(0.1)
        
        # Check stop flag is False when running
        bb = py_trees.blackboard.Client(name="test_stop")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.READ)
        assert bb.stop_requested == False
        
        manager.stop()
        
        # Check stop flag is True when stopped
        assert bb.stop_requested == True


class TestCycleManagerProgramCompletion:
    """Test program completion through CycleManager."""
    
    def test_program_completes_through_cycle_manager(self, kernel):
        """A simple program completes when run through CycleManager."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        
        # Wait for program to complete
        time.sleep(1.0)
        
        # Program should have completed (tree ticked to SUCCESS)
        # The tick thread exits when tree completes
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()
        
        # Kernel should have been called
        assert kernel.execute_count >= 1
