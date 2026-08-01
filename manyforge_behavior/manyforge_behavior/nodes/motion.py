"""I6/N + I8/P5 gate for legacy singleton motion nodes."""

from __future__ import annotations

from typing import Final


MOTION_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_motion_node_unavailable"
)
MOTION_NODE_RESTORATION_GATES: Final = ("I6", "N", "I8", "P5")


class MotionNodeUnavailableError(RuntimeError):
    """Raised before blackboard, Kernel, model, scene, or motion state access."""


class _UnavailableMotionNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise MotionNodeUnavailableError(
            f"{MOTION_NODE_UNAVAILABLE_REASON_CODE}: resource-addressed "
            "motion nodes return at I6/N and Stop integration at I8/P5"
        )


class MoveManipulatorAction(_UnavailableMotionNode):
    pass


class ResetTrajectories(_UnavailableMotionNode):
    pass
