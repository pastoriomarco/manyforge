"""I6/N + I8/P5 gate for recovery-authoring behavior nodes."""

from __future__ import annotations

from typing import Final


RECOVERY_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_recovery_node_unavailable"
)
RECOVERY_NODE_RESTORATION_GATES: Final = ("I6", "N", "I8", "P5")


class RecoveryNodeUnavailableError(RuntimeError):
    """Raised before child, offer, Kernel, or recovery-policy state is accessed."""


class _UnavailableRecoveryNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise RecoveryNodeUnavailableError(
            f"{RECOVERY_NODE_UNAVAILABLE_REASON_CODE}: Program-bound recovery "
            "nodes return at I6/N against the unified owner at I8/P5"
        )


class FallbackOverrideNode(_UnavailableRecoveryNode):
    pass


class RecoverySkillNode(_UnavailableRecoveryNode):
    pass
