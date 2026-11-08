"""Fault forwarding decisions for the ros2_medkit adapter.

Pure data-in/data-out: consumes ManyForge execution diagnostic snapshots and
yields AUTOSAR-DEM-style fault reports
(FAILED on fault latch, PASSED on clear) keyed by the externally published
``MANYFORGE_*`` fault code. The ROS service transport is not available yet; this
module never imports rclpy or medkit types so its
contract tests run hermetically in any lane.

ManyForge core stays medkit-free: this package depends one-way on
``manyforge_ros`` (shared fault-code/severity mapping from the backend
fault taxonomy).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any, Final, Optional

from manyforge_ros.diagnostics_bridge import (
    MAX_DETAIL_CHARS,
    MAX_FAULT_CODE_CHARS,
    MAX_ID_CHARS,
    _validated_digest_fault,
    external_fault_code,
    fault_severity,
)

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

# Retained reducer-state bounds, not a future N transport declaration.
MAX_TRACKED_FAULTS: Final = 64
MAX_REPORT_ROWS: Final = 64
MAX_HEALING_PASSES: Final = 64
_FAULT_CODE_PATTERN: Final = re.compile(r"MANYFORGE_[A-Z0-9_]+")


def _bounded_text(
    value: object,
    *,
    label: str,
    limit: int,
    allow_empty: bool = False,
    identifier: bool = False,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise ValueError(f"{label} must be {qualifier}")
    if len(value) > limit:
        raise ValueError(f"{label} exceeds the {limit}-character bound")
    if identifier and value and value != value.strip():
        raise ValueError(f"{label} must not contain surrounding whitespace")
    return value


def _healing_pass_count(value: object) -> int:
    if type(value) is not int:
        raise ValueError("healing_passes must be an integer")
    if value < 1 or value > MAX_HEALING_PASSES:
        raise ValueError(
            f"healing_passes must be between 1 and {MAX_HEALING_PASSES}"
        )
    return value


@dataclass(frozen=True)
class FaultReport:
    fault_code: str
    event_type: int
    severity: int
    description: str
    source_id: str

    def __post_init__(self) -> None:
        fault_code = _bounded_text(
            self.fault_code,
            label="fault_code",
            limit=MAX_FAULT_CODE_CHARS,
            identifier=True,
        )
        if _FAULT_CODE_PATTERN.fullmatch(fault_code) is None:
            raise ValueError(
                "fault_code must use the MANYFORGE_[A-Z0-9_]+ namespace"
            )
        if type(self.event_type) is not int or self.event_type not in (
            EVENT_FAILED,
            EVENT_PASSED,
        ):
            raise ValueError("event_type must be an exact FAILED/PASSED integer")
        if type(self.severity) is not int or not 0 <= self.severity <= 3:
            raise ValueError("severity must be an exact integer from 0 to 3")
        _bounded_text(
            self.description,
            label="description",
            limit=MAX_DETAIL_CHARS,
            allow_empty=True,
        )
        _bounded_text(
            self.source_id,
            label="source_id",
            limit=MAX_ID_CHARS,
            identifier=True,
        )


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

    source_id: str = "/manyforge/runtime"
    healing_passes: int = 5
    _active: dict[str, str] = field(default_factory=dict, init=False, repr=False)
    _healing: dict[str, int] = field(default_factory=dict, init=False, repr=False)

    def __post_init__(self) -> None:
        self._validate_configuration()

    def _validate_configuration(self) -> None:
        _bounded_text(
            self.source_id,
            label="source_id",
            limit=MAX_ID_CHARS,
            identifier=True,
        )
        _healing_pass_count(self.healing_passes)

    @staticmethod
    def _validate_state_bound(
        active: dict[str, str], healing: dict[str, int]
    ) -> None:
        distinct_codes = set(active).union(healing)
        if len(distinct_codes) > MAX_TRACKED_FAULTS:
            raise ValueError(
                f"tracked faults exceeds the {MAX_TRACKED_FAULTS}-row bound"
            )

    def observe(
        self,
        digest: Optional[dict[str, Any]],
        *,
        archive_ref: Optional[str] = None,
    ) -> list[FaultReport]:
        self._validate_configuration()
        if archive_ref is not None:
            archive_ref = _bounded_text(
                archive_ref,
                label="archive_ref",
                limit=MAX_DETAIL_CHARS,
            )
        if digest is None:
            # Source unavailable: no state change. Reachability itself is
            # surfaced by the diagnostics bridge, not invented here.
            return []

        fault = _validated_digest_fault(digest)
        error_message = fault.message
        reason_code = fault.reason_code

        # Work on copies so every malformed/over-bound observation fails
        # before changing latch or healing state.
        next_active = dict(self._active)
        next_healing = dict(self._healing)
        self._validate_state_bound(next_active, next_healing)
        reports: list[FaultReport] = []
        if fault.active:
            reason_code = _bounded_text(
                reason_code,
                label="reason_code",
                limit=MAX_ID_CHARS,
                identifier=True,
            )
            error_message = _bounded_text(
                error_message,
                label="fault message",
                limit=MAX_DETAIL_CHARS,
            )
            fault_code = external_fault_code(reason_code)
            # A re-latching fault cancels its healing stream.
            next_healing.pop(fault_code, None)
            if fault_code not in next_active:
                severity_name = fault_severity(reason_code)
                description = error_message
                if archive_ref:
                    # Freeze-frame parity for core faults: medkit snapshots
                    # ROS topics only, so the ManyForge failure-archive
                    # reference rides along for post-mortem reachability.
                    description = f"{description} [archive: {archive_ref}]"
                description = _bounded_text(
                    description,
                    label="fault description",
                    limit=MAX_DETAIL_CHARS,
                )
                next_active[fault_code] = description
                reports.append(
                    FaultReport(
                        fault_code=fault_code,
                        event_type=EVENT_FAILED,
                        severity=SEVERITY_TO_MEDKIT.get(severity_name, 2),
                        description=description,
                        source_id=self.source_id,
                    )
                )
            cleared = [code for code in next_active if code != fault_code]
        else:
            cleared = list(next_active)

        for fault_code in cleared:
            next_active.pop(fault_code, None)
            next_healing[fault_code] = self.healing_passes

        self._validate_state_bound(next_active, next_healing)

        # Emit the healing stream: one PASSED per poll per clearing fault
        # until the configured pass count is consumed.
        for fault_code in tuple(next_healing):
            remaining = next_healing[fault_code]
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
                next_healing.pop(fault_code, None)
            else:
                next_healing[fault_code] = remaining - 1
        if len(reports) > MAX_REPORT_ROWS:
            raise ValueError(f"reports exceeds the {MAX_REPORT_ROWS}-row bound")

        self._active = next_active
        self._healing = next_healing
        return reports

    @property
    def active_fault_codes(self) -> set[str]:
        return set(self._active)
