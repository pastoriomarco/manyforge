"""I8/P5 gate for the removed blackboard-local Stop policy decorator."""

from __future__ import annotations

from typing import Final


STOP_POLICY_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_stop_policy_node_unavailable"
)
STOP_POLICY_NODE_RESTORATION_GATES: Final = ("I8", "P5")


class StopPolicyNodeUnavailableError(RuntimeError):
    """Raised before child, blackboard, or Stop state is touched."""


class OperatorMotionStopPolicy:
    """Unavailable old local decorator; one Kernel Stop envelope replaces it."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise StopPolicyNodeUnavailableError(
            f"{STOP_POLICY_NODE_UNAVAILABLE_REASON_CODE}: Stop-policy behavior "
            "returns only against the Kernel owner at I8/P5"
        )
