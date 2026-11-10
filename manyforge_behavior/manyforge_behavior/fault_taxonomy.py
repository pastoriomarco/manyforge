"""Backend fault taxonomy — dependency-light module.

Extracted from ``cycle_manager`` so external-surface consumers (the
diagnostics bridge in the ros-jazzy lane image, the medkit adapter)
can import the taxonomy without pulling the orchestrator stack
(``cycle_manager`` imports ``py_trees``, which the slim ROS lane image
deliberately does not ship). ``cycle_manager`` re-exports these names, so
existing imports keep working. This module must stay stdlib-only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Optional


@dataclass(frozen=True)
class BackendFaultClass:
    """One stable backend fault class.

    `severity` uses the operational levels and feeds external
    diagnostics surfaces (the `diagnostic_msgs` bridge and the ros2_medkit
    adapter). Severity rule: CRITICAL means stop/quiescence was not confirmed
    or the lifecycle is stuck (immediate operator attention; medkit-style
    consumers bypass debounce); ERROR means execution aborted fail-closed
    with the system in a known state.

    `external_alias` is the externally published fault code (medkit
    `fault_code` convention: UPPER_SNAKE_CASE, <=64 chars, [A-Z0-9_]). The
    MANYFORGE_ prefix keeps the global fault namespace collision-free when
    other cell subsystems report into the same fault manager.
    """

    reason_code: str
    severity: str
    external_alias: str


def _fault_class(reason_code: str, severity: str) -> BackendFaultClass:
    return BackendFaultClass(
        reason_code=reason_code,
        severity=severity,
        external_alias=f"MANYFORGE_{reason_code.upper()}",
    )


# Keyed by `details.reason_code` as emitted by the ros2_control execution
# backend
# (manyforge_ros2_control_execution/include/manyforge_ros2_control_execution/
# ros2_control_capability.hpp, namespace ros2_control_fault_reason). A parity
# test in tests/test_cycle_manager.py parses that header and fails on drift.
ROS2_CONTROL_BACKEND_FAULT_CLASSES: dict[str, BackendFaultClass] = {
    fault.reason_code: fault
    for fault in (
        _fault_class("action_server_unavailable", "ERROR"),
        # Cancel was requested but never confirmed: motion state unknown
        # during a stop window.
        _fault_class("controller_cancel_timeout", "CRITICAL"),
        _fault_class("controller_feedback_timeout", "ERROR"),
        _fault_class("controller_goal_dispatch_failed", "ERROR"),
        _fault_class("controller_goal_rejected", "ERROR"),
        _fault_class("controller_goal_response_timeout", "ERROR"),
        # Rejected before dispatch: no controller goal was admitted.
        _fault_class("controller_binding_rejected", "ERROR"),
        _fault_class("execution_admission_rejected", "ERROR"),
        # Lost after dispatch: cancellation is requested, but this fault
        # itself does not prove that the controller confirmed the stop.
        _fault_class("controller_binding_lost", "CRITICAL"),
        _fault_class("execution_admission_lost", "CRITICAL"),
        _fault_class("controller_result_failed", "ERROR"),
        _fault_class("empty_trajectory", "ERROR"),
        # Reset/resync blocked behind a monitor thread that will not quiesce:
        # lifecycle stuck until operator intervention.
        _fault_class("monitor_thread_still_shutting_down", "CRITICAL"),
        _fault_class("stale_controller_state", "ERROR"),
        _fault_class("start_state_mismatch", "ERROR"),
        _fault_class("tracking_error_exceeded", "ERROR"),
        _fault_class("tracking_sample_invalid", "ERROR"),
        # Gripper classes. All ERROR: a gripper fault aborts the tree
        # fail-closed but does not leave arm motion unconfirmed (CRITICAL is
        # reserved for unconfirmed-stop / stuck-lifecycle classes).
        _fault_class("gripper_action_server_unavailable", "ERROR"),
        _fault_class("gripper_busy", "ERROR"),
        _fault_class("gripper_goal_aborted", "ERROR"),
        _fault_class("gripper_goal_dispatch_failed", "ERROR"),
        _fault_class("gripper_goal_rejected", "ERROR"),
        _fault_class("gripper_invalid_command", "ERROR"),
        _fault_class("gripper_not_configured", "ERROR"),
        _fault_class("invalid_gripper_semantics", "ERROR"),
        _fault_class("gripper_request_failed", "ERROR"),
        _fault_class("gripper_request_ack_timeout", "ERROR"),
        _fault_class("gripper_dwell_timeout", "ERROR"),
        _fault_class("stale_gripper_state", "ERROR"),
        _fault_class("command_estimate_attach_rejected", "ERROR"),
        _fault_class("grasp_confirmation_lost", "ERROR"),
        _fault_class("gripper_result_failed", "ERROR"),
        _fault_class("gripper_result_timeout", "ERROR"),
        _fault_class("gripper_state_not_finite", "ERROR"),
    )
}


@dataclass(frozen=True)
class DigestFault:
    """Classified fault view of one Composer execution digest."""

    message: Optional[str]
    reason_code: Optional[str]
    details: dict[str, Any]
    state: str
    active: bool


def classify_digest_fault(digest: dict[str, Any]) -> DigestFault:
    """Extract the fault view of an execution digest.

    Single authority for the stale-fault guard shared by the external
    diagnostics surfaces (the `/diagnostics` bridge and the medkit
    forwarder): the digest can carry the previous run's error prose while a
    new run is active, so a `running` digest never latches a fault — a stale
    fault must never mask the healthy heartbeat. Every other state latches
    when BOTH a message and a classified reason are present. `idle` is NOT
    excluded: tree-terminal failures rest in idle with the error latched
    (every transition into idle either clears the error or sets it fresh),
    and excluding idle made those faults invisible to external diagnostics.
    `reason_code` falls back to the top-level error code when the details
    map carries no reason token.
    """
    error_message = digest.get("error")
    error_details = digest.get("errorDetails") or digest.get("error_details") or {}
    if not isinstance(error_details, dict):
        error_details = {}
    error_code = digest.get("errorCode") or digest.get("error_code")
    reason_code = error_details.get("reason_code")
    if reason_code is None and isinstance(error_code, str) and error_code:
        reason_code = error_code
    state = str(digest.get("state") or "").lower()
    active = (
        bool(error_message)
        and bool(reason_code)
        and state != "running"
    )
    return DigestFault(
        message=error_message if isinstance(error_message, str) else None,
        reason_code=str(reason_code) if reason_code else None,
        details=error_details,
        state=state,
        active=active,
    )
