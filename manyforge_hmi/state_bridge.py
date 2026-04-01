"""StateBridge - Bridges KernelClient and the UI layer.

This is the testable core — no Viser dependency.
"""

from typing import Optional
from manyforge_behavior.kernel_client import (
    KernelClient,
    ExecutionState,
    ExecutionStatus,
    ExecuteRequest,
    ExecuteResult,
    CommandResult,
    SceneEditRequest,
    SceneEditResult,
)


class StateBridge:
    """Polls kernel status and provides state to the UI layer.

    This class is the bridge between the ManyForge kernel client and the
    Viser-based UI. It provides a clean interface for querying state and
    executing commands.
    """

    def __init__(self, kernel: KernelClient) -> None:
        """Initialize the StateBridge with a KernelClient.

        Args:
            kernel: The kernel client to communicate with.
        """
        self._kernel = kernel

    def get_state(self) -> ExecutionStatus:
        """Get the current execution status from the kernel.

        Returns:
            ExecutionStatus containing state, execution_id, and progress.
        """
        return self._kernel.get_status()

    def execute_stop(self) -> CommandResult:
        """Execute a stop command.

        Returns:
            CommandResult indicating if the stop was accepted.
        """
        return self._kernel.stop()

    def execute_pause(self) -> CommandResult:
        """Execute a pause command.

        Returns:
            CommandResult indicating if the pause was accepted.
        """
        return self._kernel.pause()

    def execute_resume(self) -> CommandResult:
        """Execute a resume command.

        Returns:
            CommandResult indicating if the resume was accepted.
        """
        return self._kernel.resume()

    def execute_reset(self) -> CommandResult:
        """Execute a reset command.

        Returns:
            CommandResult indicating if the reset was accepted.
        """
        return self._kernel.reset()

    def execute_motion(self, request: ExecuteRequest) -> ExecuteResult:
        """Execute a motion request.

        Args:
            request: The execute request containing motion parameters.

        Returns:
            ExecuteResult indicating if the motion was accepted.
        """
        return self._kernel.execute(request)

    def get_link_pose(self, target_frame: str, base_frame: str) -> Optional[dict]:
        """Get the pose of a link relative to a base frame.

        Args:
            target_frame: The target link/frame name.
            base_frame: The base frame name.

        Returns:
            Dictionary with position_m and orientation_xyzw, or None if not found.
        """
        return self._kernel.get_link_pose(target_frame, base_frame)

    def scene_edit(self, request: SceneEditRequest) -> SceneEditResult:
        """Execute a scene edit request.

        Args:
            request: The scene edit request.

        Returns:
            SceneEditResult indicating if the edit was successful.
        """
        return self._kernel.scene_edit(request)

    @property
    def is_executing(self) -> bool:
        """Check if currently executing.

        Returns:
            True if state is EXECUTING.
        """
        status = self.get_state()
        return status.state == ExecutionState.EXECUTING

    @property
    def is_idle(self) -> bool:
        """Check if currently idle.

        Returns:
            True if state is IDLE.
        """
        status = self.get_state()
        return status.state == ExecutionState.IDLE

    @property
    def can_pause(self) -> bool:
        """Check if pause is possible.

        Returns:
            True if currently executing (can be paused).
        """
        return self.is_executing

    @property
    def can_resume(self) -> bool:
        """Check if resume is possible.

        Returns:
            True if currently paused.
        """
        status = self.get_state()
        return status.state == ExecutionState.PAUSED

    @property
    def can_stop(self) -> bool:
        """Check if stop is possible.

        Returns:
            True if executing or paused.
        """
        status = self.get_state()
        return status.state in (ExecutionState.EXECUTING, ExecutionState.PAUSED)

    @property
    def current_state(self) -> ExecutionState:
        """Get the current execution state.

        Returns:
            The current ExecutionState enum value.
        """
        return self.get_state().state

    @property
    def progress(self) -> float:
        """Get the current execution progress (0.0 to 1.0).

        Returns:
            Progress value between 0.0 and 1.0.
        """
        return self.get_state().progress
