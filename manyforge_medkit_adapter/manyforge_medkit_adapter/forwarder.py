"""Fault forwarding decisions for the ros2_medkit adapter.

Pure data-in/data-out: consumes ManyForge execution digests (dicts from
``GET /api/execution/digest``) and yields AUTOSAR-DEM-style fault reports
(FAILED on fault latch, PASSED on clear) keyed by the externally published
``MANYFORGE_*`` fault code. The ROS service transport lives in
``adapter.py``; this module never imports rclpy or medkit types so the
contract tests run hermetically in any lane.

ManyForge core stays medkit-free: this package depends one-way on
``manyforge_ros`` (shared fault-code/severity mapping from the backend
fault taxonomy) and on the Composer REST surface.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Optional

from manyforge_behavior.fault_taxonomy import classify_digest_fault
from manyforge_ros.diagnostics_bridge import external_fault_code, fault_severity

# ros2_medkit_msgs/srv/ReportFault event types and Fault severities (pinned
# constants; the adapter asserts the service is reachable at startup and the
# integration test exercises the real types when medkit is installed).
EVENT_FAILED = 0
EVENT_PASSED = 1

SEVERITY_TO_MEDKIT = {
    "INFO": 0,
    "WARN": 1,
    "ERROR": 2,
    "CRITICAL": 3,
}


@dataclass(frozen=True)
class FaultReport:
    fault_code: str
    event_type: int
    severity: int
    description: str
    source_id: str


@dataclass
class FaultForwarder:
    """Tracks active ManyForge faults and emits FAILED/PASSED transitions.

    One FAILED per fault occurrence (not per poll). Clearing emits a PASSED
    *stream* (`healing_passes` consecutive polls): medkit's AUTOSAR-DEM
    debounce counts PASSED events toward healing, so a single PASSED leaves
    a CONFIRMED fault confirmed forever — verified against a real
    fault_manager. Faults are keyed by external fault code; occurrence
    counting happens medkit-side.
    """

    source_id: str = "/manyforge/composer"
    healing_passes: int = 5
    _active: dict[str, str] = field(default_factory=dict)
    _healing: dict[str, int] = field(default_factory=dict)

    def observe(
        self,
        digest: Optional[dict[str, Any]],
        *,
        archive_ref: Optional[str] = None,
    ) -> list[FaultReport]:
        if digest is None:
            # Composer unreachable: no state change. Reachability itself is
            # surfaced by the diagnostics bridge, not invented here.
            return []

        fault = classify_digest_fault(digest)
        error_message = fault.message
        reason_code = fault.reason_code

        reports: list[FaultReport] = []
        if fault.active:
            fault_code = external_fault_code(str(reason_code))
            # A re-latching fault cancels its healing stream.
            self._healing.pop(fault_code, None)
            if fault_code not in self._active:
                severity_name = fault_severity(str(reason_code))
                description = str(error_message)
                if archive_ref:
                    # Freeze-frame parity for core faults: medkit snapshots
                    # ROS topics only, so the ManyForge failure-archive
                    # reference rides along for post-mortem reachability.
                    description = f"{description} [archive: {archive_ref}]"
                self._active[fault_code] = description
                reports.append(
                    FaultReport(
                        fault_code=fault_code,
                        event_type=EVENT_FAILED,
                        severity=SEVERITY_TO_MEDKIT.get(severity_name, 2),
                        description=description,
                        source_id=self.source_id,
                    )
                )
            cleared = [code for code in self._active if code != fault_code]
        else:
            cleared = list(self._active)

        for fault_code in cleared:
            self._active.pop(fault_code, None)
            self._healing[fault_code] = self.healing_passes

        # Emit the healing stream: one PASSED per poll per clearing fault
        # until the configured pass count is consumed.
        for fault_code in list(self._healing):
            remaining = self._healing[fault_code]
            reports.append(
                FaultReport(
                    fault_code=fault_code,
                    event_type=EVENT_PASSED,
                    # Severity is ignored for PASSED events medkit-side.
                    severity=0,
                    description="",
                    source_id=self.source_id,
                )
            )
            if remaining <= 1:
                self._healing.pop(fault_code, None)
            else:
                self._healing[fault_code] = remaining - 1
        return reports

    @property
    def active_fault_codes(self) -> set[str]:
        return set(self._active)
