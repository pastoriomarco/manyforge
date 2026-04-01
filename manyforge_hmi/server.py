"""ViserHmiServer - Main entry point for the Viser-based HMI."""

import viser
import threading
import time
from typing import Optional

from .state_bridge import StateBridge
from .control_panel import ControlPanel
from .robot_visualizer import RobotVisualizer
from .scene_renderer import SceneRenderer
from .program_panel import ProgramPanel

from manyforge_behavior.kernel_client import KernelClient


class ViserHmiServer:
    """Main Viser HMI server — wires all components together.

    This class creates and manages the Viser server and all HMI components,
    providing a unified interface for the ManyForge Human-Machine Interface.
    """

    def __init__(
        self,
        kernel: KernelClient,
        host: str = "0.0.0.0",
        port: int = 8080,
        urdf_path: Optional[str] = None,
        joint_names: Optional[list[str]] = None,
    ) -> None:
        """Initialize the ViserHmiServer.

        Args:
            kernel: The kernel client for execution control.
            host: Host address to bind to.
            port: Port to bind to (use 0 for random port).
            urdf_path: Optional path to robot URDF file.
            joint_names: Optional list of joint names for the robot.
        """
        # Create the Viser server
        self._viser_server = viser.ViserServer(host=host, port=port)

        # Create the StateBridge
        self._bridge = StateBridge(kernel)

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
                self._viser_server, urdf_path, joint_names
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

    def _update_loop(self) -> None:
        """Background update loop that polls status and updates UI."""
        while self._running:
            try:
                if self._control_panel:
                    self._control_panel.update()
            except Exception:
                pass
            time.sleep(0.1)  # 100ms update rate

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
