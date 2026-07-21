"""Server-owned operator-attention inbox for Composer execution incidents.

Alert acknowledgement is deliberately separate from cycle/kernel recovery.  This
module only remembers which error incidents still require operator attention; it
cannot clear a fault, consume a recovery offer, reset, or start motion.
"""
from __future__ import annotations

from collections import deque
from copy import deepcopy
import hashlib
import threading
import time
from typing import Any, Iterable


OPERATOR_ALERT_INBOX_SCHEMA_VERSION = "manyforge.operator_alert_inbox.v1"
OPERATOR_ALERT_SCHEMA_VERSION = "manyforge.operator_alert.v1"
_ACKNOWLEDGED_RETENTION = 4096
_SEVERITY_ORDER = {
    "DEBUG": 10,
    "INFO": 20,
    "WARN": 30,
    "WARNING": 30,
    "ERROR": 40,
    "CRITICAL": 50,
}
_GENERIC_ERROR_CODES = {
    "cycle_error",
    "kernel_error",
    "error_raised",
    "execute_failed",
}


class OperatorAlertError(RuntimeError):
    """Stable validation/conflict error raised by inbox mutations."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class OperatorAlertInbox:
    """Thread-safe, process-lifetime alert ledger.

    Unacknowledged records are never evicted.  Acknowledged records are reduced
    to bounded tombstones so late duplicate diagnostics cannot reopen the same
    recovery context while memory remains bounded.  Durable execution events are
    the longer-lived forensic record.
    """

    def __init__(self, *, acknowledged_retention: int = _ACKNOWLEDGED_RETENTION) -> None:
        self._lock = threading.RLock()
        self._active_by_id: dict[str, dict[str, Any]] = {}
        self._alert_id_by_key: dict[str, str] = {}
        self._acknowledged_keys: set[str] = set()
        self._acknowledged_key_order: deque[str] = deque()
        self._acknowledged_retention = max(1, int(acknowledged_retention))
        self._generation = 0

    @staticmethod
    def _identity(snapshot: dict[str, Any]) -> tuple[str, str, str | None]:
        incident_id = str(snapshot.get("incidentId") or "").strip()
        if not incident_id:
            raise OperatorAlertError(
                "operator_alert_incident_identity_missing",
                "Incident snapshot has no incidentId",
            )
        recovery_context_id = str(snapshot.get("recoveryContextId") or "").strip()
        key = (
            f"recovery:{recovery_context_id}"
            if recovery_context_id
            else f"incident:{incident_id}"
        )
        return key, incident_id, recovery_context_id or None

    @staticmethod
    def _alert_id(key: str) -> str:
        digest = hashlib.sha256(
            f"manyforge.operator-alert.v1\0{key}".encode("utf-8")
        ).hexdigest()[:24]
        return f"operator-alert-{digest}"

    @staticmethod
    def _trigger(snapshot: dict[str, Any]) -> dict[str, Any]:
        value = snapshot.get("triggerEvent")
        return value if isinstance(value, dict) else {}

    @staticmethod
    def _headline(snapshot: dict[str, Any]) -> dict[str, Any]:
        value = snapshot.get("headlineCause")
        return value if isinstance(value, dict) else {}

    @classmethod
    def _record_from_snapshot(
        cls,
        snapshot: dict[str, Any],
        *,
        alert_id: str,
        key: str,
        incident_id: str,
        recovery_context_id: str | None,
    ) -> dict[str, Any]:
        trigger = cls._trigger(snapshot)
        headline = cls._headline(snapshot)
        trigger_details = trigger.get("details")
        trigger_details = (
            deepcopy(trigger_details) if isinstance(trigger_details, dict) else {}
        )
        nested_error_details = trigger_details.get("error_details")
        error_details = (
            deepcopy(nested_error_details)
            if isinstance(nested_error_details, dict)
            else {}
        )
        for field, value in trigger_details.items():
            if field != "error_details":
                error_details.setdefault(field, deepcopy(value))
        timestamp_ns = int(
            snapshot.get("timestampNs")
            or trigger.get("timestamp_ns")
            or time.monotonic_ns()
        )
        severity = str(
            snapshot.get("triggerSeverity")
            or trigger.get("severity")
            or "ERROR"
        ).upper()
        message = str(
            headline.get("summary")
            or headline.get("message")
            or trigger.get("message")
            or snapshot.get("triggerType")
            or "Execution error"
        )
        headline_error_code = str(headline.get("errorCode") or "")
        structured_error_code = str(
            error_details.get("reason_code")
            or error_details.get("error_code")
            or ""
        )
        error_code = (
            structured_error_code
            if structured_error_code
            and headline_error_code in _GENERIC_ERROR_CODES
            else headline_error_code
            or structured_error_code
            or str(snapshot.get("triggerType") or "execution_error")
        )
        return {
            "schemaVersion": OPERATOR_ALERT_SCHEMA_VERSION,
            "alertId": alert_id,
            "deduplicationKey": key,
            "recoveryContextId": recovery_context_id,
            "incidentId": incident_id,
            "incidentIds": [incident_id],
            "runId": trigger.get("run_id"),
            "executionId": int(trigger.get("execution_id") or 0),
            "severity": severity,
            "errorCode": error_code,
            "message": message,
            "errorDetails": error_details,
            "triggerType": snapshot.get("triggerType") or trigger.get("type"),
            "triggerChannel": snapshot.get("triggerChannel") or trigger.get("channel"),
            "firstSeenAtNs": timestamp_ns,
            "firstSeenAt": snapshot.get("timestamp") or trigger.get("timestamp"),
            "lastSeenAtNs": timestamp_ns,
            "lastSeenAt": snapshot.get("timestamp") or trigger.get("timestamp"),
            "occurrenceCount": 1,
            "revision": 1,
        }

    def ingest_incident(self, snapshot: dict[str, Any]) -> dict[str, Any] | None:
        """Create/update one blocking alert from an ERROR/CRITICAL incident.

        Returns the newly-created alert for audit projection, otherwise None.
        WARN-only incident snapshots remain available in execution history but
        do not become motion-blocking alerts.
        """
        severity = str(snapshot.get("triggerSeverity") or "").upper()
        if _SEVERITY_ORDER.get(severity, 0) < _SEVERITY_ORDER["ERROR"]:
            return None
        key, incident_id, recovery_context_id = self._identity(snapshot)
        with self._lock:
            if key in self._acknowledged_keys:
                return None
            existing_id = self._alert_id_by_key.get(key)
            if existing_id is not None:
                existing = self._active_by_id.get(existing_id)
                if existing is not None:
                    update = self._record_from_snapshot(
                        snapshot,
                        alert_id=existing_id,
                        key=key,
                        incident_id=incident_id,
                        recovery_context_id=recovery_context_id,
                    )
                    incident_ids = list(existing["incidentIds"])
                    if incident_id not in incident_ids:
                        incident_ids = (incident_ids + [incident_id])[-32:]
                    existing_first_ns = int(existing["firstSeenAtNs"])
                    existing_last_ns = int(existing["lastSeenAtNs"])
                    update_ns = int(update["lastSeenAtNs"])
                    existing.update(
                        {
                            "incidentId": incident_id,
                            "incidentIds": incident_ids,
                            "occurrenceCount": int(existing["occurrenceCount"]) + 1,
                            "revision": int(existing["revision"]) + 1,
                        }
                    )
                    if update_ns < existing_first_ns:
                        existing["firstSeenAtNs"] = update_ns
                        existing["firstSeenAt"] = update["firstSeenAt"]
                    if update_ns > existing_last_ns:
                        existing["lastSeenAtNs"] = update_ns
                        existing["lastSeenAt"] = update["lastSeenAt"]
                    if _SEVERITY_ORDER.get(update["severity"], 0) >= _SEVERITY_ORDER.get(
                        str(existing["severity"]), 0
                    ):
                        for field in (
                            "severity",
                            "errorCode",
                            "message",
                            "errorDetails",
                            "triggerType",
                            "triggerChannel",
                            "runId",
                            "executionId",
                        ):
                            existing[field] = update[field]
                    return None

            alert_id = self._alert_id(key)
            alert = self._record_from_snapshot(
                snapshot,
                alert_id=alert_id,
                key=key,
                incident_id=incident_id,
                recovery_context_id=recovery_context_id,
            )
            self._active_by_id[alert_id] = alert
            self._alert_id_by_key[key] = alert_id
            self._generation += 1
            return deepcopy(alert)

    def snapshot(self, *, limit: int | None = None) -> dict[str, Any]:
        with self._lock:
            alerts = sorted(
                self._active_by_id.values(),
                key=lambda alert: (
                    int(alert["lastSeenAtNs"]),
                    int(alert["firstSeenAtNs"]),
                    str(alert["alertId"]),
                ),
                reverse=True,
            )
            all_ids = [str(alert["alertId"]) for alert in alerts]
            effective_limit = len(alerts) if limit is None else max(0, int(limit))
            selected = alerts[:effective_limit]
            return {
                "schemaVersion": OPERATOR_ALERT_INBOX_SCHEMA_VERSION,
                "generation": self._generation,
                "blockingCount": len(alerts),
                "unacknowledgedAlertIds": all_ids,
                "alerts": deepcopy(selected),
                "truncated": len(selected) < len(alerts),
                "timestampNs": time.monotonic_ns(),
            }

    def has_blocking_alerts(self) -> bool:
        with self._lock:
            return bool(self._active_by_id)

    def acknowledge(self, alert_id: str) -> tuple[dict[str, Any], bool]:
        """Acknowledge one exact alert, returning (record, changed)."""
        with self._lock:
            alert = self._active_by_id.pop(alert_id, None)
            if alert is None:
                key = next(
                    (
                        key
                        for key, candidate_id in self._alert_id_by_key.items()
                        if candidate_id == alert_id and key in self._acknowledged_keys
                    ),
                    None,
                )
                if key is not None:
                    return {"alertId": alert_id, "deduplicationKey": key}, False
                raise OperatorAlertError(
                    "operator_alert_not_found",
                    f"Unknown operator alert: {alert_id}",
                )
            key = str(alert["deduplicationKey"])
            self._remember_acknowledged_key_locked(key)
            self._generation += 1
            return deepcopy(alert), True

    def acknowledge_all(
        self,
        *,
        alert_ids: Iterable[str],
        expected_generation: int,
        confirmed: bool,
    ) -> list[dict[str, Any]]:
        if not confirmed:
            raise OperatorAlertError(
                "operator_alert_acknowledge_all_confirmation_required",
                "Acknowledge all requires explicit confirmation",
            )
        requested = [str(value) for value in alert_ids]
        if len(requested) != len(set(requested)):
            raise OperatorAlertError(
                "operator_alert_acknowledge_all_duplicate_id",
                "Acknowledge-all alertIds must be unique",
            )
        with self._lock:
            if int(expected_generation) != self._generation:
                raise OperatorAlertError(
                    "operator_alert_inbox_stale",
                    "The alert inbox changed while acknowledgement was pending",
                )
            current_ids = set(self._active_by_id)
            if set(requested) != current_ids:
                raise OperatorAlertError(
                    "operator_alert_inbox_membership_mismatch",
                    "Acknowledge-all must name the complete current alert inbox",
                )
            if not current_ids:
                return []
            acknowledged = [deepcopy(self._active_by_id[alert_id]) for alert_id in requested]
            for alert in acknowledged:
                key = str(alert["deduplicationKey"])
                self._remember_acknowledged_key_locked(key)
                self._active_by_id.pop(str(alert["alertId"]), None)
            self._generation += 1
            return acknowledged

    def _remember_acknowledged_key_locked(self, key: str) -> None:
        if key in self._acknowledged_keys:
            return
        self._acknowledged_keys.add(key)
        self._acknowledged_key_order.append(key)
        while len(self._acknowledged_key_order) > self._acknowledged_retention:
            expired = self._acknowledged_key_order.popleft()
            self._acknowledged_keys.discard(expired)
            self._alert_id_by_key.pop(expired, None)
