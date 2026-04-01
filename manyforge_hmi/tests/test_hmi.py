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
from manyforge_behavior.program import Program
from manyforge_behavior.catalog import NodeCatalog

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
    """Tests for StateBridge using MockKernelClient."""

    def test_bridge_get_state_idle(self):
        """Test that initial state is IDLE."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)
        status = bridge.get_state()
        assert status.state == ExecutionState.IDLE

    def test_bridge_execute_stop(self):
        """Test stop from executing state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Manually set state to EXECUTING for testing
        kernel._state = ExecutionState.EXECUTING

        # Now stop
        result = bridge.execute_stop()
        assert result.accepted is True

    def test_bridge_execute_pause(self):
        """Test pause from executing state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Manually set state to EXECUTING for testing
        kernel._state = ExecutionState.EXECUTING

        # Pause
        result = bridge.execute_pause()
        assert result.accepted is True

        # Verify state is PAUSED
        status = bridge.get_state()
        assert status.state == ExecutionState.PAUSED

    def test_bridge_execute_resume(self):
        """Test resume from paused state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Manually set state to PAUSED for testing
        kernel._state = ExecutionState.PAUSED

        # Resume
        result = bridge.execute_resume()
        assert result.accepted is True

    def test_bridge_execute_reset(self):
        """Test reset from completed state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Start and complete
        request = ExecuteRequest(
            robot_id="test",
            group_id="arm",
            move_id="test_move",
            base_frame="base",
            tcp_frame="tool0",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.0, 0.0],
        )
        kernel.execute(request)

        # Reset
        result = bridge.execute_reset()
        assert result.accepted is True

        # Verify state is IDLE
        status = bridge.get_state()
        assert status.state == ExecutionState.IDLE

    def test_bridge_is_idle(self):
        """Test is_idle property."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Initially idle
        assert bridge.is_idle is True

        # Start execution
        request = ExecuteRequest(
            robot_id="test",
            group_id="arm",
            move_id="test_move",
            base_frame="base",
            tcp_frame="tool0",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.0, 0.0],
        )
        kernel.execute(request)

        # No longer idle
        assert bridge.is_idle is False

    def test_bridge_can_pause(self):
        """Test can_pause reflects state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Initially cannot pause (idle)
        assert bridge.can_pause is False

        # Manually set state to EXECUTING for testing
        kernel._state = ExecutionState.EXECUTING

        # Now can pause
        assert bridge.can_pause is True

    def test_bridge_can_stop(self):
        """Test can_stop reflects state."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        # Initially cannot stop (idle)
        assert bridge.can_stop is False

        # Manually set state to EXECUTING for testing
        kernel._state = ExecutionState.EXECUTING

        # Now can stop
        assert bridge.can_stop is True

    def test_bridge_execute_motion(self):
        """Test motion request forwarding."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        request = ExecuteRequest(
            robot_id="test",
            group_id="arm",
            move_id="test_move",
            base_frame="base",
            tcp_frame="tool0",
            motion_type="joint_goal",
            joint_goal_names=["j1", "j2"],
            joint_goal_positions_rad=[0.0, 0.0],
        )

        result = bridge.execute_motion(request)
        assert result.ok is True

    def test_bridge_scene_edit(self):
        """Test scene edit forwarding."""
        kernel = MockKernelClient()
        bridge = StateBridge(kernel)

        request = SceneEditRequest(
            operation="add",
            object_id="test_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )

        result = bridge.scene_edit(request)
        assert result.success is True


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
    def kernel(self):
        """Create a MockKernelClient."""
        return MockKernelClient()

    def test_server_creates(self, kernel):
        """Test that server creates with MockKernelClient."""
        server = ViserHmiServer(kernel, host="127.0.0.1", port=0)

        # Verify components exist
        assert server.viser_server is not None
        assert server.bridge is not None
        assert server.control_panel is not None
        assert server.scene_renderer is not None
        assert server.program_panel is not None

        server.stop()

    def test_server_load_program(self, kernel):
        """Test that server loads program."""
        server = ViserHmiServer(kernel, host="127.0.0.1", port=0)

        catalog_path = _CATALOG_PATH
        program_path = _PROGRAM_PATH

        catalog = NodeCatalog(catalog_path)
        program = Program(program_path, catalog)

        server.load_program(program)

        # Verify program was loaded
        assert server.scene_renderer.object_count > 0

        server.stop()

    def test_control_panel_creates(self, kernel):
        """Test that control panel has buttons."""
        server = ViserHmiServer(kernel, host="127.0.0.1", port=0)

        panel = server.control_panel

        # Verify buttons exist
        assert panel.stop_button is not None
        assert panel.pause_button is not None
        assert panel.resume_button is not None
        assert panel.reset_button is not None

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
