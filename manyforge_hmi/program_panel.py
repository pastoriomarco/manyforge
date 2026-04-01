"""ProgramPanel - Displays program info and tree structure in Viser GUI."""

import viser
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from manyforge_behavior.program import Program


class ProgramPanel:
    """Displays program info and tree structure in Viser GUI.

    This class creates and manages the program viewer widgets in the
    Viser sidebar, showing program metadata and the behavior tree structure.
    """

    def __init__(self, server: viser.ViserServer) -> None:
        """Initialize the ProgramPanel.

        Args:
            server: The Viser server instance.
        """
        self._server = server

        # Create the "Program" folder
        self._folder = server.gui.add_folder("Program", expand_by_default=True)

        # Create markdown displays
        with self._folder:
            self._program_info = server.gui.add_markdown("No program loaded")

    def _format_tree(self, tree_node, indent: int = 0) -> str:
        """Format a tree node as indented markdown text.

        Args:
            tree_node: The tree node to format.
            indent: Current indentation level.

        Returns:
            Formatted markdown string.
        """
        prefix = "  " * indent
        node_id = getattr(tree_node, "id", "unknown")
        node_name = getattr(tree_node, "name", "unnamed")

        lines = [f"{prefix}- {node_id}: {node_name}"]

        children = getattr(tree_node, "children", [])
        if children:
            for child in children:
                lines.extend(self._format_tree(child, indent + 1))

        return lines

    def load_program(self, program: "Program") -> None:
        """Display program metadata and tree structure.

        Args:
            program: The Program object to display.
        """
        # Build program info
        lines = []

        # Program name
        lines.append(f"**Program:** {program.name}")

        # Robot profile
        robot = program.robot
        robot_id = getattr(robot, "robot_id", "unknown")
        group_id = getattr(robot, "group_id", "unknown")
        lines.append(f"**Robot:** {robot_id} / {group_id}")

        # Scene objects count
        scene_count = len(program.scene) if program.scene else 0
        lines.append(f"**Scene objects:** {scene_count}")

        # Tree structure
        lines.append("")
        lines.append("**Tree:**")

        tree_lines = self._format_tree(program.tree)
        lines.extend(tree_lines)

        # Update the markdown display
        self._program_info.content = "\n".join(lines)

    def clear(self) -> None:
        """Clear program display."""
        self._program_info.content = "No program loaded"

    @property
    def program_info(self) -> viser.GuiMarkdownHandle:
        """Access the program info markdown widget."""
        return self._program_info
