"""Tests for the ManyForge HMI package."""

import os
from pathlib import Path

import pytest
import viser
import numpy as np

# Resolve fixture paths relative to repo root
_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")

from manyforge_behavior.kernel_client import (
    MockKernelClient,
    ExecutionState,
    ExecutionStatus,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    SceneEditRequest,
)
from manyforge_behavior.cycle_manager import CycleManager, CycleState
from manyforge_behavior.program import Program
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.runner import OrchestratorRunner

from manyforge_hmi import (
    StateBridge,
    ControlPanel,
    RobotVisualizer,
    SceneRenderer,
    ProgramPanel,
    ViserHmiServer,
)


# ============================================================================
# StateBridge Tests (no Viser dependency)
# ============================================================================

class TestStateBridge:
    """Tests for StateBridge using CycleManager."""

    def setup_method(self):
        """Set up test fixtures."""
        self.kernel = MockKernelClient()
        self.runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=self.kernel,
        )
        self.cycle_manager = CycleManager(self.runner, self.kernel)

    def test_initial_state_idle(self):
        """Test that initial state is IDLE."""
        bridge = StateBridge(self.cycle_manager)
        assert bridge.state == CycleState.IDLE

    def test_bridge_run(self):
        """Test run command."""
        bridge = StateBridge(self.cycle_manager)

        # Run from IDLE
        bridge.run()
        assert bridge.state == CycleState.RUNNING

    def test_bridge_stop(self):
        """Test stop from running state."""
        bridge = StateBridge(self.cycle_manager)

        # Run first
        bridge.run()
        assert bridge.state == CycleState.RUNNING

        # Now stop
        bridge.stop()
        assert bridge.state == CycleState.STOPPED

    def test_bridge_reset(self):
        """Test reset command."""
        bridge = StateBridge(self.cycle_manager)

        # Run first
        bridge.run()
        assert bridge.state == CycleState.RUNNING

        # Reset
        bridge.reset()
        assert bridge.state == CycleState.IDLE

    def test_bridge_velocity_scale(self):
        """Test velocity scale property and setter."""
        bridge = StateBridge(self.cycle_manager)

        # Initial velocity scale is 1.0
        assert bridge.velocity_scale == 1.0

        # Set new velocity scale
        bridge.set_velocity_scale(0.5)
        assert bridge.velocity_scale == 0.5

    def test_bridge_run_from_stopped(self):
        """Test run from STOPPED state resumes execution."""
        bridge = StateBridge(self.cycle_manager)

        # Run then stop
        bridge.run()
        bridge.stop()
        assert bridge.state == CycleState.STOPPED

        # Run again (resume)
        bridge.run()
        assert bridge.state == CycleState.RUNNING


# ============================================================================
# ControlPanel Tests (uses Viser with port=0)
# ============================================================================

class TestControlPanel:
    """Tests for ControlPanel using Viser."""

    @pytest.fixture
    def viser_server(self):
        """Create a Viser server with random port."""
        server = viser.ViserServer(host="127.0.0.1", port=0)
        yield server
        server.stop()

    @pytest.fixture
    def cycle_manager(self):
        """Create a CycleManager for testing."""
        kernel = MockKernelClient()
        runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=kernel,
        )
        return CycleManager(runner, kernel)

    def test_3_buttons_only(self, viser_server, cycle_manager):
        """Test that only Run, Stop, Reset buttons exist (no Pause/Resume/Run Program)."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Verify the three buttons exist
        assert panel.run_button is not None
        assert panel.stop_button is not None
        assert panel.reset_button is not None

        # Verify no pause/resume/run_program buttons exist
        assert not hasattr(panel, 'pause_button')
        assert not hasattr(panel, 'resume_button')
        assert not hasattr(panel, 'run_program_button')

    def test_run_button_enabled_when_idle_or_stopped(self, viser_server, cycle_manager):
        """Test Run button enabled state."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Initially IDLE - Run should be enabled
        panel.update(CycleState.IDLE, 1.0)
        assert panel.run_button.enabled is True

        # STOPPED - Run should be enabled
        panel.update(CycleState.STOPPED, 1.0)
        assert panel.run_button.enabled is True

        # RUNNING - Run should be disabled
        panel.update(CycleState.RUNNING, 1.0)
        assert panel.run_button.enabled is False

    def test_stop_button_enabled_when_running(self, viser_server, cycle_manager):
        """Test Stop button enabled state."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # IDLE - Stop should be disabled
        panel.update(CycleState.IDLE, 1.0)
        assert panel.stop_button.enabled is False

        # RUNNING - Stop should be enabled
        panel.update(CycleState.RUNNING, 1.0)
        assert panel.stop_button.enabled is True

        # STOPPED - Stop should be disabled
        panel.update(CycleState.STOPPED, 1.0)
        assert panel.stop_button.enabled is False

    def test_reset_button_enabled_when_running_or_stopped(self, viser_server, cycle_manager):
        """Test Reset button enabled state."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # IDLE - Reset should be disabled
        panel.update(CycleState.IDLE, 1.0)
        assert panel.reset_button.enabled is False

        # RUNNING - Reset should be enabled
        panel.update(CycleState.RUNNING, 1.0)
        assert panel.reset_button.enabled is True

        # STOPPED - Reset should be enabled
        panel.update(CycleState.STOPPED, 1.0)
        assert panel.reset_button.enabled is True

    def test_velocity_slider_exists(self, viser_server, cycle_manager):
        """Test that velocity slider exists."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        assert panel.velocity_slider is not None

    def test_velocity_slider_sets_scale(self, viser_server, cycle_manager):
        """Test that velocity slider sets velocity scale."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Initial scale is 1.0
        assert cycle_manager.velocity_scale == 1.0

        # Simulate slider input at 50%
        class MockTarget:
            value = 50
        class MockEvent:
            target = MockTarget()

        panel._on_velocity_slider_input(MockEvent())
        assert cycle_manager.velocity_scale == 0.5

    def test_run_delegates_to_cycle_manager(self, viser_server, cycle_manager):
        """Test that Run button delegates to CycleManager.run()."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Initially IDLE
        assert cycle_manager.state == CycleState.IDLE

        # Click Run button
        class MockEvent:
            pass

        panel._on_run_click(MockEvent())
        assert cycle_manager.state == CycleState.RUNNING

    def test_stop_delegates_to_cycle_manager(self, viser_server, cycle_manager):
        """Test that Stop button delegates to CycleManager.stop()."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Run first
        cycle_manager.run()
        assert cycle_manager.state == CycleState.RUNNING

        # Click Stop button
        class MockEvent:
            pass

        panel._on_stop_click(MockEvent())
        assert cycle_manager.state == CycleState.STOPPED

    def test_reset_delegates_to_cycle_manager(self, viser_server, cycle_manager):
        """Test that Reset button delegates to CycleManager.reset()."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Run first
        cycle_manager.run()
        assert cycle_manager.state == CycleState.RUNNING

        # Click Reset button
        class MockEvent:
            pass

        panel._on_reset_click(MockEvent())
        assert cycle_manager.state == CycleState.IDLE


# ============================================================================
# SceneRenderer Tests (uses Viser with port=0)
# ============================================================================

class TestSceneRenderer:
    """Tests for SceneRenderer using Viser."""

    @pytest.fixture
    def viser_server(self):
        """Create a Viser server with random port."""
        server = viser.ViserServer(host="127.0.0.1", port=0)
        yield server
        server.stop()

    def test_scene_add_box(self, viser_server):
        """Test adding a box to the scene."""
        renderer = SceneRenderer(viser_server)

        renderer.add_box(
            object_id="test_box",
            dimensions_m=(0.1, 0.2, 0.3),
            position_m=(0.5, 0.0, 0.4),
            orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
        )

        # Verify object was added
        assert renderer.object_count == 1
        assert "test_box" in renderer._objects

    def test_scene_add_sphere(self, viser_server):
        """Test adding a sphere to the scene."""
        renderer = SceneRenderer(viser_server)

        renderer.add_sphere(
            object_id="test_sphere",
            radius_m=0.05,
            position_m=(0.3, 0.1, 0.5),
        )

        # Verify object was added
        assert renderer.object_count == 1
        assert "test_sphere" in renderer._objects

    def test_scene_remove_object(self, viser_server):
        """Test removing an object from the scene."""
        renderer = SceneRenderer(viser_server)

        # Add and then remove
        renderer.add_box(
            object_id="test_box",
            dimensions_m=(0.1, 0.2, 0.3),
            position_m=(0.5, 0.0, 0.4),
            orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
        assert renderer.object_count == 1

        renderer.remove_object("test_box")
        assert renderer.object_count == 0
        assert "test_box" not in renderer._objects

    def test_scene_load_from_program(self, viser_server):
        """Test loading program scene resources."""
        catalog_path = _CATALOG_PATH
        program_path = _PROGRAM_PATH

        catalog = NodeCatalog(catalog_path)
        program = Program(program_path, catalog)

        renderer = SceneRenderer(viser_server)
        renderer.load_from_program(program.scene)

        # Verify objects were loaded
        assert renderer.object_count > 0


# ============================================================================
# Server Tests (uses Viser with port=0)
# ============================================================================

class TestViserHmiServer:
    """Tests for ViserHmiServer."""

    @pytest.fixture
    def cycle_manager(self):
        """Create a CycleManager."""
        kernel = MockKernelClient()
        runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=kernel,
        )
        return CycleManager(runner, kernel)

    def test_server_creates(self, cycle_manager):
        """Test that server creates with CycleManager."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        # Verify components exist
        assert server.viser_server is not None
        assert server.bridge is not None
        assert server.control_panel is not None
        assert server.scene_renderer is not None
        assert server.program_panel is not None

        server.stop()

    def test_server_load_program(self, cycle_manager):
        """Test that server loads program."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        catalog_path = _CATALOG_PATH
        program_path = _PROGRAM_PATH

        catalog = NodeCatalog(catalog_path)
        program = Program(program_path, catalog)

        server.load_program(program)

        # Verify program was loaded
        assert server.scene_renderer.object_count > 0

        server.stop()

    def test_control_panel_has_3_buttons(self, cycle_manager):
        """Test that control panel has exactly 3 buttons."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        panel = server.control_panel

        # Verify the three buttons exist
        assert panel.run_button is not None
        assert panel.stop_button is not None
        assert panel.reset_button is not None

        # Verify velocity slider exists
        assert panel.velocity_slider is not None

        server.stop()


# ============================================================================
# ProgramPanel Tests
# ============================================================================

class TestProgramPanel:
    """Tests for ProgramPanel."""

    @pytest.fixture
    def viser_server(self):
        """Create a Viser server with random port."""
        server = viser.ViserServer(host="127.0.0.1", port=0)
        yield server
        server.stop()

    def test_program_panel_load(self, viser_server):
        """Test that program panel displays program info."""
        panel = ProgramPanel(viser_server)

        catalog_path = _CATALOG_PATH
        program_path = _PROGRAM_PATH

        catalog = NodeCatalog(catalog_path)
        program = Program(program_path, catalog)

        panel.load_program(program)

        # Verify program info was set
        content = panel.program_info.content
        assert "test_pick_and_place" in content
        assert "ur5" in content


# ============================================================================
# RobotVisualizer Tests
# ============================================================================

class TestRobotVisualizer:
    """Tests for RobotVisualizer."""

    @pytest.fixture
    def viser_server(self):
        """Create a Viser server with random port."""
        server = viser.ViserServer(host="127.0.0.1", port=0)
        yield server
        server.stop()

    def test_robot_visualizer_creates(self, viser_server):
        """Test that robot visualizer creates with URDF."""
        urdf_path = "/usr/local/share/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf"

        joint_names = [
            "shoulder_pan_joint",
            "shoulder_lift_joint",
            "elbow_joint",
            "wrist_1_joint",
            "wrist_2_joint",
            "wrist_3_joint",
        ]

        visualizer = RobotVisualizer(viser_server, urdf_path, joint_names)

        # Verify visualizer was created
        assert visualizer.viser_urdf is not None
        assert visualizer.joint_names == joint_names

    def test_robot_visualizer_update_joints(self, viser_server):
        """Test updating joint positions."""
        urdf_path = "/usr/local/share/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf"

        joint_names = [
            "shoulder_pan_joint",
            "shoulder_lift_joint",
            "elbow_joint",
            "wrist_1_joint",
            "wrist_2_joint",
            "wrist_3_joint",
        ]

        visualizer = RobotVisualizer(viser_server, urdf_path, joint_names)

        positions = {
            "shoulder_pan_joint": 0.0,
            "shoulder_lift_joint": -1.57,
            "elbow_joint": 1.57,
            "wrist_1_joint": -1.57,
            "wrist_2_joint": -1.57,
            "wrist_3_joint": 0.0,
        }

        visualizer.update_joints(positions)

        # No exception means success


# ============================================================================
# Quaternion Conversion Test
# ============================================================================

class TestQuaternionConversion:
    """Tests for quaternion conversion (xyzw to wxyz)."""

    def test_xyzw_to_wxyz_conversion(self):
        """Test quaternion conversion in SceneRenderer."""
        server = viser.ViserServer(host="127.0.0.1", port=0)
        renderer = SceneRenderer(server)

        # Test the internal conversion method
        xyzw = (0.707, 0.0, 0.0, 0.707)  # 45 degree rotation around x
        wxyz = renderer._xyzw_to_wxyz(xyzw)

        assert wxyz == (0.707, 0.707, 0.0, 0.0)

        server.stop()
