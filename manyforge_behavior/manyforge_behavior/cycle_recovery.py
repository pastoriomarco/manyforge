"""Typed cutover gate for the unified cycle-recovery offer owner.

The removed coordinator authored ``manyforge.cycle_recovery_offer.v1`` from
singleton CycleManager and Kernel facts.  The normative v2 offer is bound to
operations, participants, resources, routes, active control configurations,
Program Bindings, scene generations, and the Kernel Stop certificate.  That
offer must be authored once by the headless owner; Python does not retain the
v1 classifier, request validator, Run chooser, or callback-driven state.
"""

from __future__ import annotations

from typing import Final


CYCLE_RECOVERY_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_cycle_recovery_owner_unavailable"
)


class CycleRecoveryOwnerUnavailableError(RuntimeError):
    """Raised before Kernel, cycle, clock, callback, or request state access."""


class CycleRecoveryCoordinator:
    """Unavailable compatibility name; no v1 recovery offer is authored."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise CycleRecoveryOwnerUnavailableError(
            f"{CYCLE_RECOVERY_UNAVAILABLE_REASON_CODE}: the unified v2 "
            "recovery owner is not available"
        )
