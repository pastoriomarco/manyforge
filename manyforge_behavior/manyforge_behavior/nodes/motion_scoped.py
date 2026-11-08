"""Gate for motion-scoped interrupt nodes."""

from __future__ import annotations

from typing import Final


MOTION_SCOPED_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_motion_scoped_node_unavailable"
)


class MotionScopedNodeUnavailableError(RuntimeError):
    """Raised before signal, Kernel, Stop-profile, or recovery state access."""


class _UnavailableMotionScopedNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise MotionScopedNodeUnavailableError(
            f"{MOTION_SCOPED_NODE_UNAVAILABLE_REASON_CODE}: bound interrupt "
            "nodes are not available"
        )


class MoveUntilSignal(_UnavailableMotionScopedNode):
    pass


class ProbeSearchMove(_UnavailableMotionScopedNode):
    pass
