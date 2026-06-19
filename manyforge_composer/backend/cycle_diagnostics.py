"""Shared cycle/kernel diagnostic projection for operator-facing status."""
from __future__ import annotations

from typing import Any, Optional

from manyforge_behavior.cycle_manager import preferred_structured_error_code


_FAULTISH_KERNEL_STATES = {
    "INTERRUPTED",
    "INTERVENTION_REQUIRED",
    "STOPPING",
}


def _state_token(value: Any) -> str:
    raw = getattr(value, "state", value)
    token = getattr(raw, "name", None) or getattr(raw, "value", None) or str(raw)
    return str(token).split(".")[-1]


def _snapshot_is_relevant(manager: Any) -> bool:
    try:
        status = manager.get_status()
    except Exception:  # noqa: BLE001 - status visibility is best-effort
        return True
    return _state_token(status) in _FAULTISH_KERNEL_STATES


def _dict_or_none(value: Any) -> Optional[dict[str, Any]]:
    if not value:
        return None
    if isinstance(value, dict):
        return value
    try:
        return dict(value)
    except Exception:  # noqa: BLE001 - preserve status path robustness
        return None


def _message_from_details(
    error_code: Optional[str],
    details: Optional[dict[str, Any]],
) -> Optional[str]:
    if not details:
        return error_code
    for key in (
        "message",
        "controlled_stop_error_reason",
        "error_reason",
        "reason",
        "reason_code",
    ):
        value = details.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return error_code


def active_cycle_error_fields(
    manager: Any,
) -> tuple[Optional[str], Optional[str], Optional[dict[str, Any]]]:
    """Return the best active error tuple for the cycle-status surfaces.

    CycleManager owns the operator lifecycle, but kernel/backend stop faults can
    leave the manager in STOPPED with no local `_error` while the kernel snapshot
    carries the actual intervention cause. The HMI should not need a debug
    export to explain that state, so this helper falls back to the snapshot when
    the kernel is in a fault/intervention state.
    """
    error = getattr(manager, "error", None)
    manager_details = _dict_or_none(getattr(manager, "error_details", None))
    raw_code = getattr(manager, "error_code", None)
    details = manager_details

    if (
        error is None or raw_code is None or details is None
    ) and _snapshot_is_relevant(manager):
        try:
            snapshot = manager.get_execution_snapshot()
        except Exception:  # noqa: BLE001 - keep status polling non-fatal
            snapshot = None
        if snapshot is not None:
            error = error or getattr(snapshot, "error", None) or None
            raw_code = raw_code or getattr(snapshot, "error_code", None) or None
            details = details or _dict_or_none(
                getattr(snapshot, "error_details", None)
            )

    error_code = preferred_structured_error_code(raw_code, details)
    if error is None:
        error = _message_from_details(error_code, details)
    return error, error_code, details
