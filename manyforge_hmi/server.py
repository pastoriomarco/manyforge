"""ViserHmiServer - Main entry point for the Viser-based HMI."""

import logging
import viser
import threading
import time
from typing import Optional

logger = logging.getLogger(__name__)

from .state_bridge import StateBridge
from .control_panel import ControlPanel
from .robot_visualizer import RobotVisualizer
from .scene_renderer import SceneRenderer
from .program_panel import ProgramPanel

from manyforge_behavior.cycle_manager import CycleManager


class ViserHmiServer:
    """Main Viser HMI server — wires all components together.

    This class creates and manages the Viser server and all HMI components,
    providing a unified interface for the ManyForge Human-Machine Interface.
    """

    def __init__(
        self,
        cycle_manager: CycleManager,
        host: str = "0.0.0.0",
        port: int = 8081,
        urdf_path: Optional[str] = None,
        joint_names: Optional[list[str]] = None,
        package_dirs: Optional[list[str]] = None,
    ) -> None:
        """Initialize the ViserHmiServer.

        Args:
            cycle_manager: The cycle manager for lifecycle control.
            host: Host address to bind to.
            port: Port to bind to (default 8081; use 0 for random port).
            urdf_path: Optional path to robot URDF file.
            joint_names: Optional list of joint names for the robot.
            package_dirs: Directories to search for package:// URI resolution.
        """
        # Create the Viser server
        self._viser_server = viser.ViserServer(host=host, port=port)

        # Create the StateBridge
        self._bridge = StateBridge(cycle_manager)

        # Create the ControlPanel
        self._control_panel = ControlPanel(self._viser_server, self._bridge)

        # Create the SceneRenderer
        self._scene_renderer = SceneRenderer(self._viser_server)

        # Create the ProgramPanel
        self._program_panel = ProgramPanel(self._viser_server)

        # Create RobotVisualizer if URDF path provided
        self._robot_visualizer: Optional[RobotVisualizer] = None
        if urdf_path:
            self._robot_visualizer = RobotVisualizer(
                self._viser_server, urdf_path, joint_names,
                package_dirs=package_dirs,
            )

        # Add grid floor
        self._viser_server.scene.add_grid(
            name="/grid",
            width=2.0,
            height=2.0,
            cell_size=0.1,
            position=(0.0, 0.0, 0.0),
        )

        # Show world axes
        self._viser_server.scene.world_axes.visible = True

        # Update loop state
        self._running = False
        self._update_thread: Optional[threading.Thread] = None
        self._last_snapshot_sequence = 0
        self._stale_count: int = 0

    def load_program(self, program) -> None:
        """Load a program: display in panel + render scene objects.

        Args:
            program: The Program object to load.
        """
        # Display in program panel
        self._program_panel.load_program(program)

        # Render scene objects
        if program.scene:
            self._scene_renderer.load_from_program(program.scene)

    def start(self) -> None:
        """Start the update loop (poll status, update UI)."""
        if self._running:
            return

        self._running = True
        self._update_thread = threading.Thread(target=self._update_loop, daemon=True)
        self._update_thread.start()

    @property
    def is_stale(self) -> bool:
        """True if recent updates have been failing."""
        return self._stale_count > 3

    def _update_loop(self) -> None:
        """Background update loop that polls status and updates UI."""
        while self._running:
            try:
                self._update_once()
                self._stale_count = 0
            except Exception:
                logger.warning("HMI update error", exc_info=True)
                self._stale_count += 1
            time.sleep(0.1)  # 100ms update rate

    def _update_once(self) -> None:
        """Poll runtime state once and update the UI."""
        if self._control_panel:
            state = self._bridge.state
            velocity_scale = self._bridge.velocity_scale
            self._control_panel.update(state, velocity_scale)

        if not self._robot_visualizer:
            return

        snapshot = self._bridge.execution_snapshot
        if snapshot.sequence_number <= self._last_snapshot_sequence:
            return
        self._last_snapshot_sequence = snapshot.sequence_number

        joint_names = self._robot_visualizer.joint_names
        if not joint_names or len(snapshot.joint_positions_rad) != len(joint_names):
            return

        self._robot_visualizer.update_joints(
            dict(zip(joint_names, snapshot.joint_positions_rad))
        )

    def stop(self) -> None:
        """Stop the server."""
        self._running = False
        if self._update_thread:
            self._update_thread.join(timeout=1.0)

    @property
    def viser_server(self) -> viser.ViserServer:
        """Access the underlying Viser server (for testing)."""
        return self._viser_server

    @property
    def bridge(self) -> StateBridge:
        """Access the StateBridge."""
        return self._bridge

    @property
    def control_panel(self) -> ControlPanel:
        """Access the ControlPanel."""
        return self._control_panel

    @property
    def scene_renderer(self) -> SceneRenderer:
        """Access the SceneRenderer."""
        return self._scene_renderer

    @property
    def program_panel(self) -> ProgramPanel:
        """Access the ProgramPanel."""
        return self._program_panel

    @property
    def robot_visualizer(self) -> Optional[RobotVisualizer]:
        """Access the RobotVisualizer if created."""
        return self._robot_visualizer
