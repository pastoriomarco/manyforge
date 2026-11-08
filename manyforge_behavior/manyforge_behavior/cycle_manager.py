"""Thin recovery/presentation vocabulary and the native-cycle cutover gate.

The former module owned the Python behavior-tree tick loop, lifecycle state,
Stop deadlines and settlement, intervention sessions, recovery admission,
scene reset, signal mirroring, event harvesting, and Kernel reconciliation.
Those are runtime semantics, not client helpers, and return under the native
Program Binding and the single C++ Kernel Stop owner.

The old singleton cycle-state enum is also absent: its exact historical source
is blob ``34b644322e6a0a6a8fdd51857a787c4afceb561d`` and its display/test input
is blob ``4ceccc78f0aaf37347ea72ff583915b8acfbcf59``.  Future state must be an
owner-issued, resource-addressed projection, not a Python compatibility enum.
Only recovery authoring vocabulary, bounded fault presentation, and
the immediate typed gate remain here.
"""

from __future__ import annotations

from enum import Enum
from typing import Any, Final, Optional

from manyforge_behavior.fault_taxonomy import (
    BackendFaultClass,
    ROS2_CONTROL_BACKEND_FAULT_CLASSES,
)


CYCLE_MANAGER_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_cycle_manager_unavailable"
)


class CycleManagerUnavailableError(RuntimeError):
    """Raised before runner, Kernel, tree, clock, or thread state is accessed."""


class RecoveryTrigger(Enum):
    """Canonical recovery-trigger authoring vocabulary."""

    PLANNING_FAILED = "planning_failed"
    VALIDATION_FAILED = "validation_failed"
    EXECUTE_FAILED = "execute_failed"
    EXECUTE_TIMEOUT = "execute_timeout"
    SCENE_MISMATCH = "scene_mismatch"
    PRECONDITION_FAILED = "precondition_failed"
    POSTCONDITION_FAILED = "postcondition_failed"


RECOVERY_TRIGGER_CANONICAL_VALUES: Final = frozenset(
    trigger.value for trigger in RecoveryTrigger
)


def canonical_recovery_trigger_values() -> set[str]:
    """Return a defensive set of values accepted by live authoring."""
    return set(RECOVERY_TRIGGER_CANONICAL_VALUES)


_ROS2_CONTROL_BACKEND_REASON_CODES: Final = frozenset(
    ROS2_CONTROL_BACKEND_FAULT_CLASSES
)
_GENERIC_BACKEND_ERROR_CODES: Final = frozenset(
    {
        None,
        "execution_aborted",
        "execution_rejected",
        "invalid_request",
        "stop_failed",
    }
)


def preferred_structured_error_code(
    error_code: Optional[str],
    error_details: Optional[dict[str, Any]],
) -> Optional[str]:
    """Prefer one known backend reason over a generic transport code.

    This is presentation only.  It does not classify a fault, decide Stop,
    mutate recovery state, or accept arbitrary detail strings as authority.
    """
    if isinstance(error_details, dict):
        reason_code = error_details.get("reason_code")
        if (
            isinstance(reason_code, str)
            and reason_code in _ROS2_CONTROL_BACKEND_REASON_CODES
            and error_code in _GENERIC_BACKEND_ERROR_CODES
        ):
            return reason_code
    return error_code


class CycleManager:
    """Unavailable compatibility name; Python owns no cycle runtime state."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise CycleManagerUnavailableError(
            f"{CYCLE_MANAGER_UNAVAILABLE_REASON_CODE}: native cycle requests "
            "are not available"
        )


__all__ = [
    "BackendFaultClass",
    "CYCLE_MANAGER_UNAVAILABLE_REASON_CODE",
    "CycleManager",
    "CycleManagerUnavailableError",
    "RECOVERY_TRIGGER_CANONICAL_VALUES",
    "ROS2_CONTROL_BACKEND_FAULT_CLASSES",
    "RecoveryTrigger",
    "canonical_recovery_trigger_values",
    "preferred_structured_error_code",
]
