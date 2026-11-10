"""Gate for recovery-authoring behavior nodes."""

from __future__ import annotations

from typing import Final


RECOVERY_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_recovery_node_unavailable"
)


class RecoveryNodeUnavailableError(RuntimeError):
    """Raised before child, offer, Kernel, or recovery-policy state is accessed."""


class _UnavailableRecoveryNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise RecoveryNodeUnavailableError(
            f"{RECOVERY_NODE_UNAVAILABLE_REASON_CODE}: Program-bound recovery "
            "nodes are not available"
        )


class FallbackOverrideNode(_UnavailableRecoveryNode):
    pass


class RecoverySkillNode(_UnavailableRecoveryNode):
    pass
