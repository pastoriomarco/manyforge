"""I6/N + I8/P5 gate for motion-scoped interrupt nodes."""

from __future__ import annotations

from typing import Final


MOTION_SCOPED_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_motion_scoped_node_unavailable"
)
MOTION_SCOPED_NODE_RESTORATION_GATES: Final = ("I6", "N", "I8", "P5")


class MotionScopedNodeUnavailableError(RuntimeError):
    """Raised before signal, Kernel, Stop-profile, or recovery state access."""


class _UnavailableMotionScopedNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise MotionScopedNodeUnavailableError(
            f"{MOTION_SCOPED_NODE_UNAVAILABLE_REASON_CODE}: bound interrupt "
            "nodes return at I6/N and consume one Kernel Stop envelope at I8/P5"
        )


class MoveUntilSignal(_UnavailableMotionScopedNode):
    pass


class ProbeSearchMove(_UnavailableMotionScopedNode):
    pass
