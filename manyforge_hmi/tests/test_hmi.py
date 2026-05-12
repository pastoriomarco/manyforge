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
_DEFAULT_URDF_PATH = "/usr/local/share/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf"

from manyforge_behavior.kernel_client import (
    MockKernelClient,
    ExecutionSnapshot,
    ExecutionState,
    ExecutionStatus,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    GripperCommandState,
    GripperCommandStatus,
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
        kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=kernel,
        )
        cycle_manager = CycleManager(runner, kernel)
        bridge = StateBridge(cycle_manager)

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
        kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=kernel,
        )
        cycle_manager = CycleManager(runner, kernel)
        bridge = StateBridge(cycle_manager)

        # Run then stop
        bridge.run()
        bridge.stop()
        assert bridge.state == CycleState.STOPPED

        # Run again (resume)
        bridge.run()
        assert bridge.state == CycleState.RUNNING

    def test_bridge_execution_snapshot(self):
        """Execution snapshot is exposed through the bridge."""
        bridge = StateBridge(self.cycle_manager)
        snapshot = bridge.execution_snapshot
        assert isinstance(snapshot, ExecutionSnapshot)

    def test_bridge_exposes_backend_object_queries(self):
        """Object query surface is reachable through the bridge."""
        result = self.kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.3, 0.0, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        assert result.success is True
        bridge = StateBridge(self.cycle_manager)
        pose = bridge.get_object_pose("box1", "base_link")
        state = bridge.get_object_state("box1")

        assert pose is not None
        assert pose["position_m"] == [0.3, 0.0, 0.2]
        assert state.exists is True
        assert state.frame_id == "universe"
        assert bridge.get_link_pose("tool0", "universe") is not None


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

    def test_status_text_prefers_backend_snapshot(self, viser_server, cycle_manager):
        """Visible status text is backend-first."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)
        snapshot = ExecutionSnapshot(
            timestamp_ns=1,
            sequence_number=2,
            execution_id=3,
            move_id="pick_move",
            state=ExecutionState.EXECUTING,
            progress=0.5,
            joint_positions_rad=[],
        )

        panel.update(CycleState.RUNNING, 1.0, snapshot)

        assert "Backend: EXECUTING" in panel.status_text.content
        assert "Move: pick_move" in panel.status_text.content
        assert "Progress: 50%" in panel.status_text.content

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

    def test_reset_button_enabled_in_all_cycle_states(self, viser_server, cycle_manager):
        """Test Reset button enabled state."""
        bridge = StateBridge(cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # IDLE - Reset should remain enabled so operators can prepare a fresh cycle
        panel.update(CycleState.IDLE, 1.0)
        assert panel.reset_button.enabled is True

        # RUNNING - Reset should be enabled
        panel.update(CycleState.RUNNING, 1.0)
        assert panel.reset_button.enabled is True

        # STOPPED - Reset should still be enabled
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
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        slow_runner = OrchestratorRunner(
            catalog_path=_CATALOG_PATH,
            program_path=_PROGRAM_PATH,
            kernel=slow_kernel,
        )
        slow_cycle_manager = CycleManager(slow_runner, slow_kernel)
        bridge = StateBridge(slow_cycle_manager)
        panel = ControlPanel(viser_server, bridge)

        # Run first
        slow_cycle_manager.run()
        assert slow_cycle_manager.state == CycleState.RUNNING

        # Click Stop button
        class MockEvent:
            pass

        panel._on_stop_click(MockEvent())
        assert slow_cycle_manager.state == CycleState.STOPPED

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

    def test_sync_box_updates_pose_in_place_when_parent_is_unchanged(self, viser_server):
        """Pose updates under the same parent must not tear down and recreate the node."""
        renderer = SceneRenderer(viser_server)

        first = renderer.sync_box(
            "box1",
            (0.1, 0.2, 0.3),
            (0.1, 0.2, 0.3),
            (0.0, 0.0, 0.0, 1.0),
            frame_path="/robot/visual/tool0",
        )
        second = renderer.sync_box(
            "box1",
            (0.1, 0.2, 0.3),
            (0.2, 0.25, 0.35),
            (0.0, 0.0, 0.0, 1.0),
            frame_path="/robot/visual/tool0",
        )
        third = renderer.sync_box(
            "box1",
            (0.1, 0.2, 0.3),
            (0.2, 0.25, 0.35),
            (0.0, 0.0, 0.0, 1.0),
            frame_path="/robot/visual/tool0",
        )

        assert first["action"] == "rendered"
        assert second["action"] == "updated_pose"
        assert third["action"] == "unchanged"
        assert second["scene_name"] == "/robot/visual/tool0/box1"
        assert renderer.get_render_info("box1")["scene_node_present"] is True

    def test_sync_box_recreates_when_parent_frame_changes(self, viser_server):
        """World-to-attached transitions should recreate once under the new parent."""
        renderer = SceneRenderer(viser_server)

        first = renderer.sync_box(
            "box1",
            (0.1, 0.2, 0.3),
            (0.1, 0.2, 0.3),
            (0.0, 0.0, 0.0, 1.0),
            frame_path=None,
        )
        second = renderer.sync_box(
            "box1",
            (0.1, 0.2, 0.3),
            (0.0, 0.0, 0.05),
            (0.0, 0.0, 0.0, 1.0),
            frame_path="/robot/visual/tool0",
        )

        assert first["action"] == "rendered"
        assert second["action"] == "rendered"
        assert second["previous_scene_name"] == "/scene/box1"
        assert second["scene_name"] == "/robot/visual/tool0/box1"


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

        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))

        server.load_program(program)

        # Verify backend-authored scene object was rendered
        assert server.scene_renderer.object_count == 1

        server.stop()

    def test_rebuild_link_viser_paths_uses_visual_root(self, cycle_manager, monkeypatch):
        """Attached-object frame paths must use the URDF visual joint-frame root."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeUrdfWrapper:
            def __init__(self):
                self._urdf = type("FakeUrdf", (), {"scene": object()})()

        class FakeVisualizer:
            def __init__(self):
                self.viser_urdf = FakeUrdfWrapper()

        calls = []

        def fake_name_from_frame(scene, frame_name, root_node_name="/"):
            calls.append((frame_name, root_node_name))
            return f"{root_node_name}/{frame_name}"

        import viser.extras._urdf as _urdf_module

        monkeypatch.setattr(_urdf_module, "_viser_name_from_frame", fake_name_from_frame)
        server._robot_visualizer = FakeVisualizer()
        server._bridge.get_gripper_configs = lambda: {
            "primary": {
                "mount_frame": "tool0",
                "attach_frame": "grasp_frame",
                "tcp_frame": "grasp_frame",
                "auxiliary_frames": ["insertion_frame"],
            }
        }
        server._rebuild_link_viser_paths()

        assert calls
        assert all(root_node_name == "/robot/visual" for _, root_node_name in calls)
        assert server._link_viser_paths["tool0"] == "/robot/visual/tool0"
        assert server._link_viser_paths["grasp_frame"] == "/robot/visual/grasp_frame"
        assert server._link_viser_paths["insertion_frame"] == "/robot/visual/insertion_frame"

        server.stop()

    def test_load_program_syncs_robot_from_backend_snapshot(self, cycle_manager):
        """Program load bootstraps robot visualization from backend state, not a hardcoded pose."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "joint_b"]

            def __init__(self):
                self.updated = None

            def update_joints(self, positions):
                self.updated = positions

        server._robot_visualizer = FakeVisualizer()
        cycle_manager.get_execution_snapshot = lambda: ExecutionSnapshot(
            timestamp_ns=11,
            sequence_number=5,
            execution_id=7,
            move_id="",
            state=ExecutionState.IDLE,
            progress=0.0,
            joint_positions_rad=[0.25, -0.5],
        )

        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)

        assert server._robot_visualizer.updated == {"joint_a": 0.25, "joint_b": -0.5}
        assert server._last_snapshot_sequence == 5

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

    def test_update_once_drives_robot_from_execution_snapshot(self, cycle_manager):
        """Robot visualization is driven from execution snapshots, not replay callbacks."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "joint_b"]

            def __init__(self):
                self.updated = None

            def update_joints(self, positions):
                self.updated = positions

        fake_visualizer = FakeVisualizer()
        server._robot_visualizer = fake_visualizer

        snapshot = ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=7,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.EXECUTING,
            progress=0.5,
            joint_positions_rad=[0.1, -0.2],
        )
        cycle_manager.get_execution_snapshot = lambda: snapshot

        server._update_once()

        assert fake_visualizer.updated == {"joint_a": 0.1, "joint_b": -0.2}
        assert server._last_snapshot_sequence == 7

        server.stop()

    def test_update_once_maps_arm_only_snapshots_around_configured_gripper_joint(self, cycle_manager):
        """Shorter arm-only snapshots exclude configured gripper joints instead of assuming a prefix."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "finger_joint", "joint_b"]

            def __init__(self):
                self.updates = []

            def update_joints(self, positions):
                self.updates.append(dict(positions))

        fake_visualizer = FakeVisualizer()
        server._robot_visualizer = fake_visualizer
        server._bridge.get_gripper_configs = lambda: {
            "primary": {"joint_name": "finger_joint"}
        }

        snapshot = ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=8,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.EXECUTING,
            progress=0.5,
            joint_positions_rad=[0.1, -0.2],
        )
        cycle_manager.get_execution_snapshot = lambda: snapshot

        server._update_once()

        assert fake_visualizer.updates[0] == {"joint_a": 0.1, "joint_b": -0.2}
        assert server._last_snapshot_sequence == 8

        server.stop()

    def test_update_once_ignores_partial_snapshot_when_joint_mapping_is_ambiguous(self, cycle_manager):
        """Short snapshots without a resolvable arm-only mapping should not update the visualizer."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "joint_b", "joint_c"]

            def __init__(self):
                self.updates = []

            def update_joints(self, positions):
                self.updates.append(dict(positions))

        fake_visualizer = FakeVisualizer()
        server._robot_visualizer = fake_visualizer
        server._bridge.get_gripper_configs = lambda: {"primary": {"joint_name": "finger_joint"}}
        cycle_manager.get_execution_snapshot = lambda: ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=9,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.EXECUTING,
            progress=0.5,
            joint_positions_rad=[0.1, -0.2],
        )

        server._update_once()

        assert fake_visualizer.updates == []
        assert server._last_snapshot_sequence == 9

        server.stop()

    def test_update_once_applies_actuated_gripper_position(self, cycle_manager):
        """Actuated gripper positions are rendered from kernel gripper status."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "joint_b", "finger_joint"]

            def __init__(self):
                self.updates = []

            def update_joints(self, positions):
                self.updates.append(dict(positions))

        fake_visualizer = FakeVisualizer()
        server._robot_visualizer = fake_visualizer
        cycle_manager.get_execution_snapshot = lambda: ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=9,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.IDLE,
            progress=0.0,
            joint_positions_rad=[0.0, 0.0],
        )
        server._bridge.get_gripper_configs = lambda: {
            "primary": {"joint_name": "finger_joint"}
        }
        server._bridge.get_gripper_status = lambda _gripper_id: GripperCommandStatus(
            state=GripperCommandState.EXECUTING,
            gripper_id="primary",
            progress=0.5,
            current_position=0.35,
            target_position=0.7,
        )

        server._update_once()

        assert fake_visualizer.updates[-1] == {"finger_joint": 0.35}

        server.stop()

    def test_update_once_skips_gripper_update_when_status_fetch_fails(self, cycle_manager):
        """Backend gripper status errors should not break the HMI update loop."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)

        class FakeVisualizer:
            joint_names = ["joint_a", "joint_b", "finger_joint"]

            def __init__(self):
                self.updates = []

            def update_joints(self, positions):
                self.updates.append(dict(positions))

        fake_visualizer = FakeVisualizer()
        server._robot_visualizer = fake_visualizer
        cycle_manager.get_execution_snapshot = lambda: ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=10,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.IDLE,
            progress=0.0,
            joint_positions_rad=[0.0, 0.0],
        )
        server._bridge.get_gripper_configs = lambda: {
            "primary": {"joint_name": "finger_joint"}
        }
        server._bridge.get_gripper_status = lambda _gripper_id: (_ for _ in ()).throw(
            RuntimeError("backend unavailable")
        )

        server._update_once()

        assert fake_visualizer.updates[0] == {"joint_a": 0.0, "joint_b": 0.0}

        server.stop()

    def test_update_once_syncs_configured_tool_frames_from_backend(self, cycle_manager):
        """Configured tool frames are rendered as standalone axes from backend poses."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        cycle_manager.get_execution_snapshot = lambda: ExecutionSnapshot(
            timestamp_ns=123,
            sequence_number=10,
            execution_id=9,
            move_id="move_1",
            state=ExecutionState.IDLE,
            progress=0.0,
            joint_positions_rad=[],
        )
        server._bridge.get_gripper_configs = lambda: {
            "primary": {
                "joint_name": "",
                "mount_frame": "tool0",
                "attach_frame": "grasp_frame",
                "tcp_frame": "grasp_frame",
                "auxiliary_frames": ["insertion_frame"],
            }
        }

        server._update_once()

        assert "grasp_frame" in server._frame_handles
        assert tuple(server._frame_handles["grasp_frame"].position) == pytest.approx(
            (0.5, 0.0, 0.6)
        )
        assert tuple(server._frame_handles["grasp_frame"].wxyz) == pytest.approx(
            (1.0, 0.0, 0.0, 0.0)
        )
        assert server._link_viser_paths["grasp_frame"] == "/frames/grasp_frame"

        server.stop()

    def test_update_once_syncs_objects_from_backend_queries(self, cycle_manager):
        """Object rendering is reconciled from backend object queries."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)

        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))

        server._update_once()
        assert server.scene_renderer.object_count == 1

        cycle_manager._kernel.scene_edit(
            SceneEditRequest(operation="remove", object_id=first.object_id)
        )
        server._update_once()
        assert server.scene_renderer.object_count == 0

        server.stop()

    def test_object_render_diagnostics_report_backend_and_scene_presence(self, cycle_manager):
        """Runtime diagnostics expose backend state, frame path, and scene-node presence."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)

        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))
        server._update_once()

        diagnostics = server.get_object_render_diagnostics(first.object_id)
        assert diagnostics["exists"] is True
        assert diagnostics["state"]["frame_id"] == first.pose.get("frame_id", "universe")
        assert diagnostics["backend_pose_in_universe"] is not None
        assert diagnostics["render"]["scene_name"] == f"/scene/{first.object_id}"
        assert diagnostics["render"]["scene_node_present"] is True

        cycle_manager._kernel.scene_edit(
            SceneEditRequest(operation="attach", object_id=first.object_id, frame_id="tool0")
        )
        server._update_once()

        diagnostics = server.get_object_render_diagnostics(first.object_id)
        assert diagnostics["state"]["attached"] is True
        assert diagnostics["state"]["frame_id"] == "tool0"
        assert diagnostics["backend_pose_in_frame"] is not None
        assert diagnostics["render"]["scene_name"] is not None
        assert diagnostics["render"]["scene_node_present"] is True

        server.stop()

    def test_scene_trace_records_invalidation_and_reconcile_events(self, cycle_manager):
        """HMI scene trace captures invalidation hints and backend reconciliation."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)

        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))
        server.request_scene_sync(
            reason="scene_edit_callback",
            operation="add",
            object_id=first.object_id,
        )
        server._update_once()

        trace = server.get_scene_trace()
        assert any(
            event["event"] == "invalidation_hint"
            and event["object_id"] == first.object_id
            and event["operation"] == "add"
            for event in trace
        )
        reconcile = next(
            event for event in reversed(trace)
            if event["event"] == "reconcile_render" and event["object_id"] == first.object_id
        )
        assert reconcile["backend_state"]["exists"] is True
        assert reconcile["render"]["scene_node_present"] is True

        server.stop()

    def test_attached_reconcile_is_stable_after_first_render(self, cycle_manager):
        """Once attached, repeated reconciles should not emit no-op trace churn."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        server._link_viser_paths["tool0"] = "/robot/visual/tool0"
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)

        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))
        cycle_manager._kernel.scene_edit(
            SceneEditRequest(operation="attach", object_id=first.object_id, frame_id="tool0")
        )

        server._update_once()
        server.clear_scene_trace()
        server._update_once()

        assert not any(
            event["event"] == "reconcile_render" and event["object_id"] == first.object_id
            for event in server.get_scene_trace()
        )
        diagnostics = server.get_object_render_diagnostics(first.object_id)
        assert diagnostics["state"]["attached"] is True
        assert diagnostics["frame_path"] == "/frames/tool0"
        assert diagnostics["render"]["scene_name"] == f"/frames/tool0/{first.object_id}"
        assert diagnostics["render"]["scene_node_present"] is True

        server.stop()

    def test_frame_visibility_supports_semantic_hide_and_extra_frame_show(self, cycle_manager):
        """Viewer visibility can hide semantic frames and enable arbitrary model frames."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        server.load_program(program)
        server._update_once()

        assert server.get_frame_visibility("tool0") is True
        server.set_frame_visibility("tool0", False)
        assert server.get_frame_visibility("tool0") is False
        assert server._frame_handles["tool0"].visible is False

        assert "base_link" not in server._frame_handles
        server.set_frame_visibility("base_link", True)
        assert server.get_frame_visibility("base_link") is True
        assert "base_link" in server._frame_handles
        base_link_handle = server._frame_handles["base_link"]
        assert server.get_frame_gizmo_size() == pytest.approx(0.1)
        assert server._frame_axes_dimensions() == pytest.approx((0.1, 0.005, 0.0125))

        server.set_frame_gizmo_size(0.12)
        assert server.get_frame_gizmo_size() == pytest.approx(0.12)
        assert server._frame_axes_dimensions() == pytest.approx((0.12, 0.006, 0.015))
        assert "base_link" in server._frame_handles
        assert server._frame_handles["base_link"] is not base_link_handle

        server.stop()

    def test_object_frame_visibility_toggles_backend_object_pose_axis(self, cycle_manager):
        """Object gizmo frames are viewer-only and follow backend object poses."""
        server = ViserHmiServer(cycle_manager, host="127.0.0.1", port=0)
        catalog = NodeCatalog(_CATALOG_PATH)
        program = Program(_PROGRAM_PATH, catalog)
        first = program.scene[0]
        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id=first.object_id,
            shape_type=first.shape.get("type"),
            box_dimensions_m=first.shape.get("box_dimensions_m"),
            sphere_radius_m=first.shape.get("sphere_radius_m"),
            pose_position_m=first.pose.get("position_m"),
            pose_orientation_xyzw=first.pose.get("orientation_xyzw", [0.0, 0.0, 0.0, 1.0]),
            frame_id=first.pose.get("frame_id", "universe"),
        ))
        server.load_program(program)
        server._update_once()

        assert server.get_object_frame_visibility(first.object_id) is False
        assert first.object_id not in server._object_frame_handles

        server.set_object_frame_visibility(first.object_id, True)
        assert server.get_object_frame_visibility(first.object_id) is True
        assert first.object_id in server._object_frame_handles
        handle = server._object_frame_handles[first.object_id]
        assert handle.visible is True
        assert np.allclose(handle.position, first.pose.get("position_m"))
        assert np.allclose(
            server._object_frame_render_state[first.object_id],
            (0.75, 0.01, 0.03),
        )

        cycle_manager._kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id=first.object_id,
            pose_position_m=[0.42, -0.13, 0.27],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        server._update_once()
        assert np.allclose(handle.position, [0.42, -0.13, 0.27])

        server.set_object_frame_visibility(first.object_id, False)
        assert server.get_object_frame_visibility(first.object_id) is False
        assert first.object_id not in server._object_frame_handles

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
        urdf_path = _DEFAULT_URDF_PATH
        if not os.path.exists(urdf_path):
            pytest.skip(f"URDF fixture not available on host: {urdf_path}")

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
        urdf_path = _DEFAULT_URDF_PATH
        if not os.path.exists(urdf_path):
            pytest.skip(f"URDF fixture not available on host: {urdf_path}")

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
