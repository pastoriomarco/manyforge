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


def _manager_error_snapshot(
    manager: Any,
) -> tuple[Optional[str], Optional[str], Optional[dict[str, Any]]]:
    reader = getattr(manager, "get_error_snapshot", None)
    if callable(reader):
        try:
            error, code, details = reader()
            return error, code, _dict_or_none(details)
        except Exception:  # noqa: BLE001 - status projection stays available.
            pass
    return (
        getattr(manager, "error", None),
        getattr(manager, "error_code", None),
        _dict_or_none(getattr(manager, "error_details", None)),
    )


def manager_cycle_error_fields(
    manager: Any,
) -> tuple[Optional[str], Optional[str], Optional[dict[str, Any]]]:
    """Return only the cycle-manager-owned error tuple.

    Successful command responses use this helper so reserved control routes
    such as /stop do not perform extra kernel snapshot/status probes while
    forming a 200 response. Polling/status surfaces can still call
    active_cycle_error_fields() when the snapshot fallback is worth the extra
    visibility.
    """
    error, raw_code, details = _manager_error_snapshot(manager)
    error_code = preferred_structured_error_code(raw_code, details)
    if error is None:
        error = _message_from_details(error_code, details)
    return error, error_code, details


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
    error, raw_code, manager_details = _manager_error_snapshot(manager)
    details = manager_details

    kernel_state_token: Optional[str]
    try:
        status = manager.get_status()
        kernel_state_token = _state_token(status).upper()
        snapshot_is_relevant = kernel_state_token in _FAULTISH_KERNEL_STATES
    except Exception:  # noqa: BLE001 - keep status polling non-fatal
        kernel_state_token = None
        snapshot_is_relevant = True

    if (
        error is None
        or raw_code is None
        or details is None
        or kernel_state_token == "INTERVENTION_REQUIRED"
    ) and snapshot_is_relevant:
        try:
            snapshot = manager.get_execution_snapshot()
        except Exception:  # noqa: BLE001 - keep status polling non-fatal
            snapshot = None
        if snapshot is not None:
            snapshot_details = _dict_or_none(getattr(snapshot, "error_details", None))
            if kernel_state_token == "INTERVENTION_REQUIRED":
                merged_details: dict[str, Any] = {}
                if snapshot_details:
                    merged_details.update(snapshot_details)
                if details:
                    # Preserve the richer original cause (for example the
                    # runtime-collision pair) while adding the kernel recovery
                    # boundary. Without this merge, the HMI keeps advertising a
                    # plain held-motion Resume while the kernel requires an
                    # explicit acknowledge/reset decision.
                    if snapshot_details:
                        for key in ("phase", "reason", "reason_code"):
                            if key in snapshot_details and key in details:
                                merged_details.setdefault(
                                    f"kernel_intervention_{key}",
                                    snapshot_details[key],
                                )
                    merged_details.update(details)
                merged_details.setdefault("kernel_state", "intervention_required")
                if getattr(manager, "motion_stop_state", None) == "held":
                    merged_details.setdefault(
                        "motion_soft_stop_reason",
                        "motion_soft_fail_requires_intervention",
                    )
                details = merged_details or details
            else:
                details = details or snapshot_details
            error = error or getattr(snapshot, "error", None) or None
            raw_code = raw_code or getattr(snapshot, "error_code", None) or None

    error_code = preferred_structured_error_code(raw_code, details)
    if error is None:
        error = _message_from_details(error_code, details)
    return error, error_code, details
