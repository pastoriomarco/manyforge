"""StateBridge - Bridges CycleManager and the UI layer.

This is the testable core — no Viser dependency.
"""

from manyforge_behavior.cycle_manager import CycleManager, CycleState


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

        If RUNNING: stops first, then resets.
        If STOPPED: stops tick thread, resets kernel, rebuilds tree.
        If IDLE: no-op.
        """
        self._cm.reset()

    def set_velocity_scale(self, scale: float) -> None:
        """Set velocity scale on blackboard.

        Args:
            scale: Velocity scale between 0.0 and 1.0.
        """
        self._cm.set_velocity_scale(scale)
