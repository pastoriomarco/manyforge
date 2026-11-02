"""Typed I6/N + I8/P5 gate for motion transaction ownership."""

from __future__ import annotations

from typing import Final


MOTION_TRANSACTION_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_motion_transaction_owner_unavailable"
)
MOTION_TRANSACTION_RESTORATION_GATES: Final = ("I6", "N", "I8", "P5")


class MotionTransactionOwnerUnavailableError(RuntimeError):
    """Raised before Kernel status, acknowledgement, or snapshot access."""


class MotionExecutionCoordinator:
    """Unavailable old one-motion coordinator; the Kernel owns transactions."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise MotionTransactionOwnerUnavailableError(
            f"{MOTION_TRANSACTION_UNAVAILABLE_REASON_CODE}: resource-scoped "
            "motion transactions return at I6/N and I8/P5"
        )
