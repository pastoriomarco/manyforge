"""Audit 6I: hermetic fault-forwarding contract tests."""

from __future__ import annotations

from manyforge_medkit_adapter.forwarder import (
    EVENT_FAILED,
    EVENT_PASSED,
    FaultForwarder,
)


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


def test_fault_latch_reports_failed_once_then_passed_on_clear():
    forwarder = FaultForwarder(source_id="/manyforge/test/composer")

    reports = forwarder.observe(_tracking_fault_digest())
    assert len(reports) == 1
    report = reports[0]
    assert report.fault_code == "MANYFORGE_TRACKING_ERROR_EXCEEDED"
    assert report.event_type == EVENT_FAILED
    assert report.severity == 2  # taxonomy ERROR
    assert report.source_id == "/manyforge/test/composer"

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


def test_unreachable_composer_changes_nothing():
    forwarder = FaultForwarder()
    forwarder.observe(_tracking_fault_digest())
    assert forwarder.observe(None) == []
    assert forwarder.active_fault_codes == {"MANYFORGE_TRACKING_ERROR_EXCEEDED"}


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
