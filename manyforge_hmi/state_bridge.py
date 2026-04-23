"""StateBridge - Bridges CycleManager and the UI layer.

This is the testable core — no Viser dependency.
"""

from manyforge_behavior.cycle_manager import CycleManager, CycleState
from manyforge_behavior.kernel_client import (
    ExecutionSnapshot,
    GripperCommandStatus,
    ObjectState,
)


class StateBridge:
    """Wraps CycleManager and provides state to the UI layer.

    This class is the bridge between the CycleManager and the
    Viser-based UI. It provides a clean interface for querying state and
    executing commands.
    """

    def __init__(self, cycle_manager: CycleManager) -> None:
        """Initialize the StateBridge with a CycleManager.

        Args:
            cycle_manager: The cycle manager for lifecycle control.
        """
        self._cm = cycle_manager

    @property
    def state(self) -> CycleState:
        """Get the current lifecycle state.

        Returns:
            CycleState enum value (IDLE, RUNNING, or STOPPED).
        """
        return self._cm.state

    @property
    def velocity_scale(self) -> float:
        """Get the current velocity scale (0.0 to 1.0).

        Returns:
            Velocity scale value between 0.0 and 1.0.
        """
        return self._cm.velocity_scale

    @property
    def execution_snapshot(self) -> ExecutionSnapshot:
        """Get the latest execution snapshot for observer-side visualization."""
        return self._cm.get_execution_snapshot()

    def get_object_pose(self, object_id: str, base_frame: str):
        """Get the current backend-authoritative object pose."""
        return self._cm.get_object_pose(object_id, base_frame)

    def get_link_pose(self, target_frame: str, base_frame: str):
        """Get the current backend-authoritative pose for a named frame."""
        return self._cm.get_link_pose(target_frame, base_frame)

    def get_object_state(self, object_id: str) -> ObjectState:
        """Get the current backend-authoritative object state."""
        return self._cm.get_object_state(object_id)

    def get_gripper_status(self, gripper_id: str) -> GripperCommandStatus:
        """Get the latest actuated gripper status from the kernel."""
        return self._cm.get_gripper_status(gripper_id)

    def get_gripper_configs(self) -> dict[str, dict]:
        """Get deployment gripper configs for HMI-side rendering helpers."""
        return self._cm.get_gripper_configs()

    def get_scene_trace(self, limit: int | None = None) -> list[dict]:
        """Get recent backend scene trace events."""
        return self._cm.get_scene_trace(limit=limit)

    def clear_scene_trace(self) -> None:
        """Clear retained backend scene trace events."""
        self._cm.clear_scene_trace()

    def run(self) -> None:
        """Start or resume program execution.

        If IDLE: builds and starts everything.
        If STOPPED: resumes from stopped state.
        If RUNNING: no-op.
        """
        self._cm.run()

    def stop(self) -> None:
        """Stop program execution.

        If RUNNING: sets stop flag and suspends tick thread.
        If STOPPED or IDLE: no-op.
        """
        self._cm.stop()

    def reset(self) -> None:
        """Reset program to initial state.

        Rebuilds the software/runtime cycle state without teleporting the robot.
        """
        self._cm.reset()

    def set_velocity_scale(self, scale: float) -> None:
        """Set velocity scale on blackboard.

        Args:
            scale: Velocity scale between 0.0 and 1.0.
        """
        self._cm.set_velocity_scale(scale)
