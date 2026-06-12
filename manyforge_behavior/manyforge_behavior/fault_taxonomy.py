"""Backend fault taxonomy (audit Phase 6A) — dependency-light module.

Extracted from ``cycle_manager`` so external-surface consumers (the 6H
diagnostics bridge in the ros-jazzy lane image, the 6I medkit adapter)
can import the taxonomy without pulling the orchestrator stack
(``cycle_manager`` imports ``py_trees``, which the slim ROS lane image
deliberately does not ship). ``cycle_manager`` re-exports these names, so
existing imports keep working. This module must stay stdlib-only.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BackendFaultClass:
    """One stable backend fault class (audit Phase 6A taxonomy).

    `severity` uses the spec/520 operational levels and feeds external
    diagnostics surfaces (Phase 6H `diagnostic_msgs`, Phase 6I ros2_medkit
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


# Keyed by `details.reason_code` as emitted by the ros2_control backend
# (manyforge_roboplan/include/manyforge_roboplan/ros2_control_capability.hpp,
# namespace ros2_control_fault_reason). A parity test in
# tests/test_cycle_manager.py parses that header and fails on drift.
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
        _fault_class("controller_result_failed", "ERROR"),
        _fault_class("empty_trajectory", "ERROR"),
        # Reset/resync blocked behind a monitor thread that will not quiesce:
        # lifecycle stuck until operator intervention.
        _fault_class("monitor_thread_still_shutting_down", "CRITICAL"),
        _fault_class("stale_controller_state", "ERROR"),
        _fault_class("start_state_mismatch", "ERROR"),
        _fault_class("tracking_error_exceeded", "ERROR"),
    )
}
