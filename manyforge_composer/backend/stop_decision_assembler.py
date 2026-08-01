"""Typed cutover gate for Kernel-owned Stop-decision assembly.

The historical module at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` correlated native
events, opened and merged Stop episodes, inferred triggers, selected terminal
authority, classified physical results, minted derived details, and closed or
invalidated chains.  Those are production Stop causal/terminal semantics, not
Composer presentation behavior.

I8/P5 restores this work once in the C++ Kernel/Stop owner, using immutable
participant plans, native sequence, certificates, evidence policy, explicit
loss records, and one terminal outcome.  Python may later display an
owner-issued Stop-decision record; it must not assemble, repair, order, or
complete one.  The old enums, record DTO, ordering helper, and mutable episode
state are intentionally absent.
"""

from __future__ import annotations

from typing import Final, NoReturn


STOP_DECISION_OWNER_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_stop_decision_owner_unavailable"
)
STOP_DECISION_RESTORATION_GATES: Final = ("I8", "P5")


class StopDecisionOwnerUnavailableError(RuntimeError):
    """Raised before native evidence, runtime state, or storage is touched."""


def _raise_stop_decision_owner_unavailable() -> NoReturn:
    raise StopDecisionOwnerUnavailableError(
        f"{STOP_DECISION_OWNER_UNAVAILABLE_REASON_CODE}: the single C++ "
        "Kernel Stop-decision owner returns at I8/P5"
    )


class StopDecisionAssembler:
    """Unavailable compatibility name with effect-free construction failure."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_stop_decision_owner_unavailable()


def require_stop_decision_owner(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Fail closed before inspecting former events or assembler state."""

    _raise_stop_decision_owner_unavailable()


__all__ = [
    "STOP_DECISION_OWNER_UNAVAILABLE_REASON_CODE",
    "STOP_DECISION_RESTORATION_GATES",
    "StopDecisionAssembler",
    "StopDecisionOwnerUnavailableError",
    "require_stop_decision_owner",
]
