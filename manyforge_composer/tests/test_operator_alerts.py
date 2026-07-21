"""Operator-alert inbox identity, retention, and acknowledgement tests."""
from __future__ import annotations

import pytest

from manyforge_composer.backend.operator_alerts import (
    OperatorAlertError,
    OperatorAlertInbox,
)


def _incident(
    index: int,
    *,
    recovery_context_id: str | None = None,
    severity: str = "ERROR",
) -> dict:
    return {
        "incidentId": f"incident-{index}",
        "recoveryContextId": recovery_context_id,
        "timestampNs": index,
        "timestamp": f"2026-07-20T00:00:{index:02d}Z",
        "triggerType": "cycle_error",
        "triggerChannel": "fault_safety_stop",
        "triggerSeverity": severity,
        "triggerEvent": {
            "run_id": "run-alert-test",
            "execution_id": index,
            "severity": severity,
            "message": f"failure {index}",
            "details": {
                "error": f"failure {index}",
                "error_code": f"failure_{index}",
            },
        },
        "headlineCause": {
            "summary": f"Failure {index}",
            "message": f"failure {index}",
            "errorCode": f"failure_{index}",
        },
    }


def test_latest_three_does_not_evict_older_unacknowledged_alert() -> None:
    inbox = OperatorAlertInbox()
    for index in range(1, 5):
        assert inbox.ingest_incident(_incident(index)) is not None

    page = inbox.snapshot(limit=3)
    assert page["blockingCount"] == 4
    assert page["truncated"] is True
    assert [alert["incidentId"] for alert in page["alerts"]] == [
        "incident-4",
        "incident-3",
        "incident-2",
    ]

    inbox.acknowledge(page["alerts"][0]["alertId"])
    revealed = inbox.snapshot(limit=3)
    assert revealed["blockingCount"] == 3
    assert [alert["incidentId"] for alert in revealed["alerts"]] == [
        "incident-3",
        "incident-2",
        "incident-1",
    ]


def test_recovery_context_deduplicates_and_acknowledgement_does_not_reopen() -> None:
    inbox = OperatorAlertInbox()
    first = inbox.ingest_incident(
        _incident(1, recovery_context_id="recovery-context-shared")
    )
    assert first is not None
    assert inbox.ingest_incident(
        _incident(2, recovery_context_id="recovery-context-shared")
    ) is None

    page = inbox.snapshot()
    assert page["generation"] == 1
    assert page["blockingCount"] == 1
    assert page["alerts"][0]["occurrenceCount"] == 2
    assert page["alerts"][0]["incidentIds"] == ["incident-1", "incident-2"]

    alert_id = page["alerts"][0]["alertId"]
    _, changed = inbox.acknowledge(alert_id)
    assert changed is True
    assert inbox.snapshot()["blockingCount"] == 0
    assert inbox.ingest_incident(
        _incident(3, recovery_context_id="recovery-context-shared")
    ) is None
    assert inbox.snapshot()["blockingCount"] == 0

    replay, changed = inbox.acknowledge(alert_id)
    assert replay["alertId"] == alert_id
    assert changed is False


def test_recovery_context_keeps_true_timestamp_bounds_for_late_arrivals() -> None:
    inbox = OperatorAlertInbox()
    middle = _incident(2, recovery_context_id="recovery-context-out-of-order")
    earliest = _incident(1, recovery_context_id="recovery-context-out-of-order")
    latest = _incident(3, recovery_context_id="recovery-context-out-of-order")

    inbox.ingest_incident(middle)
    inbox.ingest_incident(latest)
    inbox.ingest_incident(earliest)

    alert = inbox.snapshot()["alerts"][0]
    assert alert["firstSeenAtNs"] == 1
    assert alert["firstSeenAt"] == earliest["timestamp"]
    assert alert["lastSeenAtNs"] == 3
    assert alert["lastSeenAt"] == latest["timestamp"]
    assert alert["occurrenceCount"] == 3


def test_warn_incident_is_history_only() -> None:
    inbox = OperatorAlertInbox()
    assert inbox.ingest_incident(_incident(1, severity="WARN")) is None
    assert inbox.snapshot()["blockingCount"] == 0


def test_nested_structured_cause_replaces_generic_wrapper_code() -> None:
    inbox = OperatorAlertInbox()
    incident = _incident(1)
    incident["headlineCause"]["errorCode"] = "cycle_error"
    incident["triggerEvent"]["details"] = {
        "error": "Motion execution ended in interrupted",
        "error_details": {
            "reason_code": "runtime_collision_lookahead",
            "colliding_pair": "wall <-> wrist_3_link",
        },
    }
    inbox.ingest_incident(incident)
    alert = inbox.snapshot()["alerts"][0]
    assert alert["errorCode"] == "runtime_collision_lookahead"
    assert alert["errorDetails"]["colliding_pair"] == "wall <-> wrist_3_link"


def test_acknowledge_all_is_confirmed_and_generation_fenced() -> None:
    inbox = OperatorAlertInbox()
    inbox.ingest_incident(_incident(1))
    inbox.ingest_incident(_incident(2))
    original = inbox.snapshot()

    with pytest.raises(OperatorAlertError) as confirmation:
        inbox.acknowledge_all(
            alert_ids=original["unacknowledgedAlertIds"],
            expected_generation=original["generation"],
            confirmed=False,
        )
    assert confirmation.value.code == (
        "operator_alert_acknowledge_all_confirmation_required"
    )

    inbox.ingest_incident(_incident(3))
    with pytest.raises(OperatorAlertError) as stale:
        inbox.acknowledge_all(
            alert_ids=original["unacknowledgedAlertIds"],
            expected_generation=original["generation"],
            confirmed=True,
        )
    assert stale.value.code == "operator_alert_inbox_stale"
    assert inbox.snapshot()["blockingCount"] == 3

    current = inbox.snapshot()
    acknowledged = inbox.acknowledge_all(
        alert_ids=current["unacknowledgedAlertIds"],
        expected_generation=current["generation"],
        confirmed=True,
    )
    assert {alert["incidentId"] for alert in acknowledged} == {
        "incident-1",
        "incident-2",
        "incident-3",
    }
    assert inbox.snapshot()["blockingCount"] == 0
