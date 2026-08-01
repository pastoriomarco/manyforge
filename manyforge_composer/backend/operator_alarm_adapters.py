"""Typed I5/H0 gate for source-owned operator-alarm producers.

Scene, pose, collision, provider, lifecycle, and runtime-health conditions are
owned by their new headless producers.  The former Composer adapter aggregate
mixed those sources with alarm-manager and historian state, so none of its
producer-specific runtime body remains here.
"""

from __future__ import annotations

from typing import Final


OPERATOR_ALARM_ADAPTERS_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_operator_alarm_adapters_unavailable"
)
OPERATOR_ALARM_ADAPTERS_RESTORATION_GATES: Final = ("I5", "H0")


class OperatorAlarmAdaptersUnavailableError(RuntimeError):
    """Raised before manager, source, clock, or history state is accessed."""


class OperatorAlarmAdapters:
    """Unavailable compatibility name; no Composer alarm producer exists."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise OperatorAlarmAdaptersUnavailableError(
            f"{OPERATOR_ALARM_ADAPTERS_UNAVAILABLE_REASON_CODE}: source-owned "
            "condition producers return with the headless alarm owner at I5/H0"
        )
