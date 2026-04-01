"""RobotVisualizer - URDF rendering and joint state updates."""

import viser
from viser.extras import ViserUrdf
from pathlib import Path
from typing import Optional


class RobotVisualizer:
    """Renders robot URDF in Viser 3D scene and updates joint positions.

    This class handles loading and displaying robot URDF models in the
    Viser 3D scene, and updating joint positions based on forward kinematics.
    """

    def __init__(
        self,
        server: viser.ViserServer,
        urdf_path: str,
        joint_names: Optional[list[str]] = None,
    ) -> None:
        """Initialize the RobotVisualizer.

        Args:
            server: The Viser server instance.
            urdf_path: Path to the URDF file.
            joint_names: List of joint names for updates (optional).
        """
        self._server = server
        self._urdf_path = Path(urdf_path)
        self._joint_names = joint_names or []

        # Load URDF using ViserUrdf
        self._viser_urdf = ViserUrdf(
            server,
            self._urdf_path,
            root_node_name="/robot",
            mesh_color_override=None,  # Use mesh colors from URDF
        )

        # Track visibility
        self._visible = True

    def update_joints(self, positions: dict[str, float]) -> None:
        """Update joint positions in the visualization.

        Args:
            positions: Dictionary mapping joint name to angle in radians.
        """
        self._viser_urdf.update_cfg(positions)

    def set_visible(self, visible: bool) -> None:
        """Show/hide the robot.

        Args:
            visible: True to show, False to hide.
        """
        self._visible = visible
        # Note: ViserUrdf doesn't have a direct visibility toggle,
        # so we would need to remove and re-add the robot
        if not visible:
            # Remove the robot from the scene
            self._server.scene.remove_by_name("/robot")
        elif not self._viser_urdf:
            # Re-add if needed (simplified - in practice you'd want to cache)
            pass

    @property
    def viser_urdf(self) -> ViserUrdf:
        """Access the underlying ViserUrdf instance."""
        return self._viser_urdf

    @property
    def joint_names(self) -> list[str]:
        """Get the list of joint names."""
        return self._joint_names
