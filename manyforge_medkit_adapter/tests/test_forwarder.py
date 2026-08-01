"""Audit 6I: hermetic fault-forwarding contract tests."""

from __future__ import annotations

from pathlib import Path

import pytest

from manyforge_medkit_adapter import adapter
from manyforge_medkit_adapter.forwarder import (
    EVENT_FAILED,
    EVENT_PASSED,
    MAX_HEALING_PASSES,
    MAX_TRACKED_FAULTS,
    FaultReport,
    FaultForwarder,
)
from manyforge_ros.diagnostics_bridge import (
    MAX_DETAIL_CHARS,
    MAX_FAULT_CODE_CHARS,
    MAX_ID_CHARS,
)


class _StringTrap:
    def __str__(self) -> str:
        raise AssertionError("an arbitrary object's __str__ must not be called")


def test_effectful_adapter_entrypoint_is_fenced_without_dormant_loop(
    capsys: pytest.CaptureFixture[str],
):
    assert adapter.main() == 78
    assert capsys.readouterr().err.strip() == adapter.UNAVAILABLE_MESSAGE

    source = Path(adapter.__file__).read_text(encoding="utf-8")
    assert "def _run_adapter_port_target" not in source
    assert "urllib" not in source
    assert "rclpy" not in source
    assert "ReportFault" not in source


def _tracking_fault_digest():
    return {
        "state": "stopped",
        "error": "Controller tracking error exceeded tolerance: 0.078 rad",
        "errorCode": "tracking_error_exceeded",
        "errorDetails": {
            "reason_code": "tracking_error_exceeded",
            "backend": "ros2_control",
            "joint_name": "wrist_3_joint",
        },
    }


def _healthy_digest():
    return {"state": "idle", "error": None}


def _fault_digest(index: int) -> dict:
    reason_code = f"bounded_fault_{index:03d}"
    return {
        "state": "stopped",
        "error": "bounded fault",
        "errorCode": reason_code,
        "errorDetails": {"reason_code": reason_code},
    }


@pytest.mark.parametrize(
    "healing_passes",
    [True, False, 0, -1, MAX_HEALING_PASSES + 1, 1.0, "5"],
)
def test_healing_configuration_requires_a_bounded_exact_integer(
    healing_passes,
) -> None:
    with pytest.raises(ValueError):
        FaultForwarder(healing_passes=healing_passes)


@pytest.mark.parametrize(
    "source_id",
    [
        None,
        7,
        "",
        " /manyforge/runtime",
        "/manyforge/runtime ",
        "x" * (MAX_ID_CHARS + 1),
    ],
)
def test_source_id_requires_an_exact_bounded_identifier(source_id) -> None:
    with pytest.raises(ValueError):
        FaultForwarder(source_id=source_id)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("fault_code", "bad-code"),
        ("fault_code", "MANYFORGE_" + "X" * MAX_FAULT_CODE_CHARS),
        ("event_type", True),
        ("event_type", 2),
        ("severity", False),
        ("severity", 4),
        ("description", "x" * (MAX_DETAIL_CHARS + 1)),
        ("source_id", "x" * (MAX_ID_CHARS + 1)),
    ],
)
def test_fault_report_rejects_malformed_or_unbounded_fields(field, value) -> None:
    fields = {
        "fault_code": "MANYFORGE_TEST_FAULT",
        "event_type": EVENT_FAILED,
        "severity": 2,
        "description": "fault",
        "source_id": "/manyforge/runtime",
    }
    fields[field] = value
    with pytest.raises(ValueError):
        FaultReport(**fields)


def test_fault_latch_reports_failed_once_then_passed_on_clear():
    forwarder = FaultForwarder(source_id="/manyforge/test/runtime")

    reports = forwarder.observe(_tracking_fault_digest())
    assert len(reports) == 1
    report = reports[0]
    assert report.fault_code == "MANYFORGE_TRACKING_ERROR_EXCEEDED"
    assert report.event_type == EVENT_FAILED
    assert report.severity == 2  # taxonomy ERROR
    assert report.source_id == "/manyforge/test/runtime"

    # Same fault on subsequent polls: no re-report (medkit aggregates).
    assert forwarder.observe(_tracking_fault_digest()) == []
    assert forwarder.active_fault_codes == {"MANYFORGE_TRACKING_ERROR_EXCEEDED"}

    # Clear -> a PASSED healing stream (medkit DEM debounce counts PASSED
    # events toward healing; a single PASSED would never heal a CONFIRMED
    # fault — verified against a real fault_manager).
    total_passed = 0
    for _ in range(forwarder.healing_passes):
        reports = forwarder.observe(_healthy_digest())
        assert all(r.event_type == EVENT_PASSED for r in reports)
        assert all(
            r.fault_code == "MANYFORGE_TRACKING_ERROR_EXCEEDED" for r in reports
        )
        total_passed += len(reports)
    assert total_passed == forwarder.healing_passes
    assert forwarder.observe(_healthy_digest()) == []
    assert forwarder.active_fault_codes == set()


def test_critical_class_maps_to_medkit_severity_3():
    forwarder = FaultForwarder()
    digest = {
        "state": "stopped",
        "error": "Timed out waiting for cancel confirmation",
        "errorCode": "stop_failed",
        "errorDetails": {"reason_code": "controller_cancel_timeout"},
    }
    reports = forwarder.observe(digest)
    assert len(reports) == 1
    assert reports[0].severity == 3  # CRITICAL bypasses medkit debounce
    assert reports[0].fault_code == "MANYFORGE_CONTROLLER_CANCEL_TIMEOUT"


def test_fault_replacing_fault_passes_old_and_fails_new():
    forwarder = FaultForwarder()
    assert len(forwarder.observe(_tracking_fault_digest())) == 1
    digest2 = {
        "state": "stopped",
        "error": "goal rejected",
        "errorCode": "execution_rejected",
        "errorDetails": {"reason_code": "controller_goal_rejected"},
    }
    reports = forwarder.observe(digest2)
    kinds = {(r.fault_code, r.event_type) for r in reports}
    assert ("MANYFORGE_CONTROLLER_GOAL_REJECTED", EVENT_FAILED) in kinds
    assert ("MANYFORGE_TRACKING_ERROR_EXCEEDED", EVENT_PASSED) in kinds


def test_archive_ref_rides_in_description_for_freeze_frame_parity():
    forwarder = FaultForwarder()
    reports = forwarder.observe(
        _tracking_fault_digest(), archive_ref="run-42/archive-7"
    )
    assert "[archive: run-42/archive-7]" in reports[0].description


def test_overlong_archive_or_message_fails_without_changing_state() -> None:
    forwarder = FaultForwarder()
    forwarder.observe(_tracking_fault_digest())
    active_before = dict(forwarder._active)
    healing_before = dict(forwarder._healing)

    with pytest.raises(ValueError, match="archive_ref exceeds"):
        forwarder.observe(
            _healthy_digest(), archive_ref="x" * (MAX_DETAIL_CHARS + 1)
        )
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before

    with pytest.raises(ValueError, match="archive_ref must"):
        forwarder.observe(_healthy_digest(), archive_ref=_StringTrap())
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before

    digest = _tracking_fault_digest()
    digest["error"] = "x" * (MAX_DETAIL_CHARS + 1)
    with pytest.raises(ValueError, match="digest.error exceeds"):
        forwarder.observe(digest)
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before


def test_mutated_configuration_fails_before_healing_state_changes() -> None:
    forwarder = FaultForwarder()
    forwarder.observe(_tracking_fault_digest())
    active_before = dict(forwarder._active)
    healing_before = dict(forwarder._healing)
    forwarder.healing_passes = True

    with pytest.raises(ValueError, match="healing_passes must be an integer"):
        forwarder.observe(_healthy_digest())
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before


def test_archive_composition_must_fit_the_description_bound() -> None:
    forwarder = FaultForwarder()
    digest = _tracking_fault_digest()
    digest["error"] = "x" * MAX_DETAIL_CHARS
    with pytest.raises(ValueError, match="fault description exceeds"):
        forwarder.observe(digest, archive_ref="archive")
    assert forwarder.active_fault_codes == set()


@pytest.mark.parametrize(
    "digest",
    [
        [],
        {},
        {"state": "idle"},
        {"state": "idle", "error": "fault"},
        {"state": "idle", "error": None, "errorCode": "fault"},
        {"state": "stopped", "error": _StringTrap(), "errorCode": "fault"},
        {
            "state": "stopped",
            "error": "fault",
            "errorDetails": {"reason_code": _StringTrap()},
        },
        {
            "state": "stopped",
            "error": "fault",
            "errorCode": "fault",
            "errorDetails": {"detail": _StringTrap()},
        },
    ],
)
def test_malformed_observations_fail_before_latch_or_healing_mutation(digest) -> None:
    forwarder = FaultForwarder()
    forwarder.observe(_tracking_fault_digest())
    active_before = dict(forwarder._active)
    healing_before = dict(forwarder._healing)
    with pytest.raises(ValueError):
        forwarder.observe(digest)
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before


def test_unavailable_diagnostic_source_changes_nothing():
    forwarder = FaultForwarder()
    forwarder.observe(_tracking_fault_digest())
    assert forwarder.observe(None) == []
    assert forwarder.active_fault_codes == {"MANYFORGE_TRACKING_ERROR_EXCEEDED"}


def test_relatching_a_healing_fault_cancels_its_passed_stream() -> None:
    forwarder = FaultForwarder(healing_passes=MAX_HEALING_PASSES)
    assert len(forwarder.observe(_tracking_fault_digest())) == 1
    passed = forwarder.observe(_healthy_digest())
    assert [report.event_type for report in passed] == [EVENT_PASSED]

    relatched = forwarder.observe(_tracking_fault_digest())
    assert [report.event_type for report in relatched] == [EVENT_FAILED]
    assert forwarder._healing == {}
    assert forwarder.observe(_tracking_fault_digest()) == []


def test_tracked_fault_and_report_rows_are_bounded_atomically() -> None:
    forwarder = FaultForwarder(healing_passes=MAX_HEALING_PASSES)
    for index in range(MAX_TRACKED_FAULTS):
        reports = forwarder.observe(_fault_digest(index))
        assert len(reports) <= MAX_TRACKED_FAULTS

    active_before = dict(forwarder._active)
    healing_before = dict(forwarder._healing)
    with pytest.raises(ValueError, match="tracked faults exceeds"):
        forwarder.observe(_fault_digest(MAX_TRACKED_FAULTS))
    assert forwarder._active == active_before
    assert forwarder._healing == healing_before


def test_running_state_with_stale_error_text_does_not_latch():
    # The digest can carry the previous run's prose while a new run is
    # active; only non-running states with a structured reason latch.
    forwarder = FaultForwarder()
    digest = {
        "state": "running",
        "error": "Controller tracking error exceeded tolerance: 0.078 rad",
        "errorCode": "tracking_error_exceeded",
        "errorDetails": {"reason_code": "tracking_error_exceeded"},
    }
    assert forwarder.observe(digest) == []
