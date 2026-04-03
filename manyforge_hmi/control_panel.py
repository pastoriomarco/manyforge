"""ControlPanel - Viser GUI widgets for execution control."""

import viser
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .state_bridge import StateBridge
from manyforge_behavior.cycle_manager import CycleState


class ControlPanel:
    """Run/Stop/Reset buttons + velocity slider + status display in Viser GUI.

    This class creates and manages the execution control widgets in the
    Viser sidebar, including buttons for Run, Stop, and Reset,
    a velocity slider, as well as status display.
    """

    def __init__(self, server: viser.ViserServer, bridge: "StateBridge") -> None:
        """Initialize the ControlPanel.

        Args:
            server: The Viser server instance.
            bridge: The StateBridge for state queries and command execution.
        """
        self._server = server
        self._bridge = bridge
        self._updating = False

        # Create the "Execution Control" folder
        self._folder = server.gui.add_folder("Execution Control", expand_by_default=True)

        # Create status markdown display
        with self._folder:
            self._status_header = server.gui.add_markdown("## Execution Status")
            self._status_text = server.gui.add_markdown("State: IDLE")

        # Create velocity slider
        with self._folder:
            self._velocity_slider = server.gui.add_slider(
                label="Velocity Scale",
                min=0,
                max=100,
                step=1,
                initial_value=100,
            )

        # Create buttons folder
        self._buttons_folder = server.gui.add_folder("Controls", expand_by_default=True)

        # Run button (green)
        with self._buttons_folder:
            self._run_button = server.gui.add_button(
                "Run", color="green"
            )

        # Stop button (red)
        with self._buttons_folder:
            self._stop_button = server.gui.add_button(
                "Stop", color="red"
            )

        # Reset button (blue)
        with self._buttons_folder:
            self._reset_button = server.gui.add_button(
                "Reset", color="blue"
            )

        # Wire up button click handlers
        self._run_button.on_click(self._on_run_click)
        self._stop_button.on_click(self._on_stop_click)
        self._reset_button.on_click(self._on_reset_click)

        # Wire up velocity slider handler
        self._velocity_slider.on_update(self._on_velocity_slider_input)

        # Initial update
        self.update(CycleState.IDLE, 1.0)

    def _on_run_click(self, event: viser.GuiEvent) -> None:
        """Handle Run button click."""
        self._bridge.run()

    def _on_stop_click(self, event: viser.GuiEvent) -> None:
        """Handle Stop button click."""
        self._bridge.stop()

    def _on_reset_click(self, event: viser.GuiEvent) -> None:
        """Handle Reset button click."""
        self._bridge.reset()

    def _on_velocity_slider_input(self, event: viser.GuiEvent) -> None:
        """Handle velocity slider input."""
        if self._updating:
            return  # Ignore programmatic updates
        # Convert 0-100 integer to 0.0-1.0 float
        scale = event.target.value / 100.0
        self._bridge.set_velocity_scale(scale)

    def update(self, state: CycleState, velocity_scale: float) -> None:
        """Update status display and button states.

        Args:
            state: Current CycleState (IDLE, RUNNING, or STOPPED).
            velocity_scale: Current velocity scale (0.0 to 1.0).
        """
        # Update status text
        state_name = state.value.upper()
        self._status_text.content = f"State: {state_name}"

        # Update velocity slider value (guarded to avoid triggering on_update)
        self._updating = True
        self._velocity_slider.value = int(velocity_scale * 100)
        self._updating = False

        # Update button enabled states
        # Run: enabled when state is IDLE or STOPPED
        self._run_button.enabled = state in (CycleState.IDLE, CycleState.STOPPED)

        # Stop: enabled when state is RUNNING
        self._stop_button.enabled = state == CycleState.RUNNING

        # Reset: enabled when RUNNING or STOPPED
        self._reset_button.enabled = state in (CycleState.RUNNING, CycleState.STOPPED)

    @property
    def run_button(self) -> viser.GuiButtonHandle:
        """Access the Run button."""
        return self._run_button

    @property
    def stop_button(self) -> viser.GuiButtonHandle:
        """Access the Stop button."""
        return self._stop_button

    @property
    def reset_button(self) -> viser.GuiButtonHandle:
        """Access the Reset button."""
        return self._reset_button

    @property
    def velocity_slider(self) -> viser.GuiSliderHandle:
        """Access the velocity slider widget."""
        return self._velocity_slider

    @property
    def status_text(self) -> viser.GuiMarkdownHandle:
        """Access the status text widget."""
        return self._status_text
