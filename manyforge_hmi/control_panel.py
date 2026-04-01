"""ControlPanel - Viser GUI widgets for execution control."""

import viser
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .state_bridge import StateBridge


class ControlPanel:
    """Stop/Pause/Resume/Reset buttons + status display in Viser GUI.

    This class creates and manages the execution control widgets in the
    Viser sidebar, including buttons for Stop, Pause, Resume, and Reset,
    as well as status display and progress bar.
    """

    def __init__(self, server: viser.ViserServer, bridge: "StateBridge") -> None:
        """Initialize the ControlPanel.

        Args:
            server: The Viser server instance.
            bridge: The StateBridge for state queries and command execution.
        """
        self._server = server
        self._bridge = bridge

        # Create the "Execution Control" folder
        self._folder = server.gui.add_folder("Execution Control", expand_by_default=True)

        # Create status markdown display
        with self._folder:
            self._status_header = server.gui.add_markdown("## Execution Status")
            self._status_text = server.gui.add_markdown("State: IDLE")
            self._progress_bar = server.gui.add_progress_bar(
                value=0, animated=False
            )

        # Create buttons folder
        self._buttons_folder = server.gui.add_folder("Controls", expand_by_default=True)

        # Stop button (red)
        with self._buttons_folder:
            self._stop_button = server.gui.add_button(
                "Stop", color="red"
            )

        # Pause button (yellow)
        with self._buttons_folder:
            self._pause_button = server.gui.add_button(
                "Pause", color="yellow"
            )

        # Resume button (green)
        with self._buttons_folder:
            self._resume_button = server.gui.add_button(
                "Resume", color="green"
            )

        # Reset button (blue)
        with self._buttons_folder:
            self._reset_button = server.gui.add_button(
                "Reset", color="blue"
            )

        # Wire up button click handlers
        self._stop_button.on_click(self._on_stop_click)
        self._pause_button.on_click(self._on_pause_click)
        self._resume_button.on_click(self._on_resume_click)
        self._reset_button.on_click(self._on_reset_click)

        # Initial update
        self.update()

    def _on_stop_click(self, event: viser.GuiEvent) -> None:
        """Handle Stop button click."""
        result = self._bridge.execute_stop()
        if not result.accepted:
            self._status_text.content = f"Stop failed: {result.error}"

    def _on_pause_click(self, event: viser.GuiEvent) -> None:
        """Handle Pause button click."""
        result = self._bridge.execute_pause()
        if not result.accepted:
            self._status_text.content = f"Pause failed: {result.error}"

    def _on_resume_click(self, event: viser.GuiEvent) -> None:
        """Handle Resume button click."""
        result = self._bridge.execute_resume()
        if not result.accepted:
            self._status_text.content = f"Resume failed: {result.error}"

    def _on_reset_click(self, event: viser.GuiEvent) -> None:
        """Handle Reset button click."""
        result = self._bridge.execute_reset()
        if not result.accepted:
            self._status_text.content = f"Reset failed: {result.error}"

    def update(self) -> None:
        """Update status display and button states from bridge."""
        status = self._bridge.get_state()

        # Update status text
        state_name = status.state.value.upper() if status.state else "UNKNOWN"
        self._status_text.content = f"State: {state_name}"

        # Update progress bar
        self._progress_bar.value = int(status.progress * 100)

        # Update button enabled states
        self._stop_button.enabled = self._bridge.can_stop
        self._pause_button.enabled = self._bridge.can_pause
        self._resume_button.enabled = self._bridge.can_resume
        self._reset_button.enabled = status.state != self._bridge.get_state().state or True  # Always enable reset

    @property
    def stop_button(self) -> viser.GuiButtonHandle:
        """Access the Stop button."""
        return self._stop_button

    @property
    def pause_button(self) -> viser.GuiButtonHandle:
        """Access the Pause button."""
        return self._pause_button

    @property
    def resume_button(self) -> viser.GuiButtonHandle:
        """Access the Resume button."""
        return self._resume_button

    @property
    def reset_button(self) -> viser.GuiButtonHandle:
        """Access the Reset button."""
        return self._reset_button

    @property
    def status_text(self) -> viser.GuiMarkdownHandle:
        """Access the status text widget."""
        return self._status_text

    @property
    def progress_bar(self) -> viser.GuiSliderHandle:
        """Access the progress bar widget."""
        return self._progress_bar
