"""Gate for legacy singleton motion nodes."""

from __future__ import annotations

from typing import Final


MOTION_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_motion_node_unavailable"
)


class MotionNodeUnavailableError(RuntimeError):
    """Raised before blackboard, Kernel, model, scene, or motion state access."""


class _UnavailableMotionNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise MotionNodeUnavailableError(
            f"{MOTION_NODE_UNAVAILABLE_REASON_CODE}: resource-addressed "
            "motion nodes are not available"
        )


class MoveManipulatorAction(_UnavailableMotionNode):
    pass


class ResetTrajectories(_UnavailableMotionNode):
    pass
