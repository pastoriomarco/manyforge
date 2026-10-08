"""Bounded same-user client for the sole CCLC owner."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
import socket
import unicodedata
from typing import Any, Protocol

import rfc8785

from .native import HeadlessApiError, strict_json_object
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


CCLC_SOCKET_FILE = "cclc-command.sock"
CCLC_OPEN_REQUEST_VERSION = "manyforge.cclc_open_request.v1"
CCLC_OPEN_RESPONSE_VERSION = "manyforge.cclc_open_response.v1"
MAX_CCLC_REQUEST_BYTES = 4 * 1024 * 1024
MAX_CCLC_RESPONSE_BYTES = 8 * 1024 * 1024
_RESPONSE_FIELDS = frozenset({"api_version", "command", "error", "ok", "result"})


class CCLCOpenPort(Protocol):
    """Caller-neutral lifecycle/route surface; implementations own no truth."""

    def owner_identity(self) -> dict[str, Any]: ...

    def component_snapshot(self, component_id: str) -> dict[str, Any]: ...

    def route_templates(self, component_id: str) -> dict[str, Any]: ...

    def submit_lifecycle(self, intent: Mapping[str, Any]) -> dict[str, Any]: ...

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None: ...

    def lifecycle_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None: ...

    def submit_route(self, intent: Mapping[str, Any]) -> dict[str, Any]: ...

    def route_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None: ...

    def route_status(self, transaction_id: str) -> dict[str, Any] | None: ...

    def incident_events(self, after_sequence: int, limit: int) -> dict[str, Any]: ...


def _canonical_json(value: Mapping[str, Any], *, error_code: str, label: str) -> bytes:
    try:
        return rfc8785.dumps(dict(value))
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            error_code,
            f"{label} is not canonically encodable: {exc}",
        ) from exc


_TARGET_STATES = frozenset({"active", "prepared", "disabled"})
_TARGET_ORIGINS = frozenset({"startup", "operator", "program"})
_PROGRESS_PHASES = frozenset(
    {
        "satisfied",
        "waiting_for_prerequisites",
        "transition_in_progress",
        "outcome_uncertain",
        "blocked",
        "cancelled",
    }
)
_PROGRESS_INTENTS = frozenset({"prepare", "enable", "disable", "recover"})
_BLOCKING_CAUSE_CLASSES = frozenset(
    {
        "none",
        "availability",
        "emergency_stop",
        "protective_stop",
        "safeguard_stop",
        "fault",
        "configuration",
        "unknown",
        "collision",
        "external_control",
        "provider_lost",
    }
)
_INCIDENT_CAUSE_CLASSES = _BLOCKING_CAUSE_CLASSES - {"none", "availability"}
_MAX_BLOCKING_REASON_BYTES = 1024
_MAX_INCIDENT_ID_BYTES = 128
_MAX_INCIDENT_SUMMARY_BYTES = 1024
_MAX_VENDOR_CODES = 16
_MAX_VENDOR_CODE_BYTES = 64
_MAX_INT64 = 2**63 - 1
_MAX_SAFE_INTEGER = 2**53 - 1
INCIDENT_EVENT_KINDS = frozenset(
    {"component_incident", "component_incident_acknowledged"}
)
MAX_INCIDENT_EVENT_PAGE = 64


def _is_int(value: Any) -> bool:
    return type(value) is int


def _member(value: Any, allowed: frozenset[str]) -> bool:
    return isinstance(value, str) and value in allowed


def _bounded_text(value: Any, maximum: int) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and len(value.encode("utf-8")) <= maximum
    )


def _valid_incident(incident: Any) -> bool:
    """One open component incident (addendum 2 contract item 2), or null."""

    if incident is None:
        return True
    if not isinstance(incident, dict) or set(incident) != {
        "acknowledged",
        "cause_class",
        "detected_at_ns",
        "incident_id",
        "summary",
        "vendor_codes",
    }:
        return False
    codes = incident["vendor_codes"]
    summary = incident["summary"]
    return (
        _bounded_text(incident["incident_id"], _MAX_INCIDENT_ID_BYTES)
        and _member(incident["cause_class"], _INCIDENT_CAUSE_CLASSES)
        and isinstance(codes, list)
        and len(codes) <= _MAX_VENDOR_CODES
        and all(_bounded_text(code, _MAX_VENDOR_CODE_BYTES) for code in codes)
        and _is_int(incident["detected_at_ns"])
        and 0 <= incident["detected_at_ns"] <= _MAX_INT64
        and _bounded_text(summary, _MAX_INCIDENT_SUMMARY_BYTES)
        and unicodedata.is_normalized("NFC", summary)
        and incident["acknowledged"] is False
    )


def _invalid_lifecycle_fact(field: str) -> HeadlessApiError:
    return HeadlessApiError(
        "cclc_open_response_invalid",
        f"CCLC component snapshot {field} is invalid",
    )


def _validate_lifecycle_target_facts(snapshot: Mapping[str, Any]) -> None:
    """Fail closed on a malformed requested target or target progress.

    The snapshot is otherwise forwarded unchanged; these two owner-issued
    facts carry closed enums that Composer presents, so Headless rejects an
    unknown key or spelling instead of forwarding an unpresentable state.
    """

    if "lifecycle_target" in snapshot:
        target = snapshot["lifecycle_target"]
        if target is not None and (
            not isinstance(target, dict)
            or set(target) != {"origin", "requested_state", "target_generation"}
            or not _member(target["requested_state"], _TARGET_STATES)
            or not _member(target["origin"], _TARGET_ORIGINS)
            or not _is_int(target["target_generation"])
            or not 1 <= target["target_generation"] <= _MAX_INT64
        ):
            raise _invalid_lifecycle_fact("lifecycle_target")
    if "lifecycle_progress" in snapshot:
        progress = snapshot["lifecycle_progress"]
        if progress is None:
            return
        if (
            not isinstance(progress, dict)
            or set(progress)
            != {
                "blocking_cause_class",
                "blocking_reason",
                "delayed",
                "incident",
                "next_intent",
                "phase",
                "waiting_since_ns",
            }
            or not _member(progress["phase"], _PROGRESS_PHASES)
            or (
                progress["next_intent"] is not None
                and not _member(progress["next_intent"], _PROGRESS_INTENTS)
            )
            or not _member(
                progress["blocking_cause_class"], _BLOCKING_CAUSE_CLASSES
            )
            or (
                progress["blocking_reason"] is not None
                and not _bounded_text(
                    progress["blocking_reason"], _MAX_BLOCKING_REASON_BYTES
                )
            )
            or (
                progress["waiting_since_ns"] is not None
                and (
                    not _is_int(progress["waiting_since_ns"])
                    or not 0 <= progress["waiting_since_ns"] <= _MAX_INT64
                )
            )
            or type(progress["delayed"]) is not bool
            or not _valid_incident(progress["incident"])
        ):
            raise _invalid_lifecycle_fact("lifecycle_progress")


def _validate_incident_recovery(
    result: Mapping[str, Any], intent: Mapping[str, Any]
) -> None:
    """Fail closed on a malformed authorization-only incident Recover.

    CCLC answers the operator's Recover of an open incident without a
    lifecycle transaction: `acceptance` and `status` are null and
    `incident_recovery` names the closed incident. The key is optional and
    absent for every other request.
    """

    if "incident_recovery" not in result:
        return
    recovery = result["incident_recovery"]
    component_id = intent.get("controllable_component_id")
    if (
        not isinstance(recovery, dict)
        or set(recovery)
        != {
            "acknowledged_at_ns",
            "acknowledged_by",
            "cause_class",
            "controllable_component_id",
            "incident_id",
        }
        or not _bounded_text(recovery["incident_id"], _MAX_INCIDENT_ID_BYTES)
        or not _member(recovery["cause_class"], _INCIDENT_CAUSE_CLASSES)
        or not _bounded_text(recovery["acknowledged_by"], 1024)
        or not _is_int(recovery["acknowledged_at_ns"])
        or not 0 <= recovery["acknowledged_at_ns"] <= _MAX_SAFE_INTEGER
        or recovery["controllable_component_id"] != component_id
        or result.get("acceptance") is not None
        or result.get("status") is not None
        or result.get("replay_status") not in {"accepted_new", "replayed_exact"}
        or "rejection" in result
    ):
        raise HeadlessApiError(
            "cclc_open_response_invalid",
            "CCLC incident recovery result is invalid",
        )


def _validate_incident_events(
    result: Mapping[str, Any], after_sequence: int, limit: int
) -> None:
    """Strict ordered page of incident history events (contract item 5)."""

    def invalid(reason: str) -> HeadlessApiError:
        return HeadlessApiError(
            "cclc_open_response_invalid",
            f"CCLC incident events {reason}",
        )

    events = result.get("events")
    through = result.get("through_sequence")
    if (
        set(result) != {"events", "through_sequence"}
        or not isinstance(events, list)
        or len(events) > limit
        or not _is_int(through)
    ):
        raise invalid("page is malformed")
    previous = after_sequence
    for event in events:
        if not isinstance(event, dict) or set(event) != {
            "event_kind",
            "event_sequence",
            "incident_id",
            "occurred_at_ns",
            "payload",
        }:
            raise invalid("row is malformed")
        sequence = event["event_sequence"]
        payload = event["payload"]
        if (
            not _is_int(sequence)
            or not previous < sequence <= _MAX_SAFE_INTEGER
            or not _member(event["event_kind"], INCIDENT_EVENT_KINDS)
            or not _bounded_text(event["incident_id"], _MAX_INCIDENT_ID_BYTES)
            or not _is_int(event["occurred_at_ns"])
            or not 0 <= event["occurred_at_ns"] <= _MAX_SAFE_INTEGER
            or not isinstance(payload, dict)
            or payload.get("incident_id") != event["incident_id"]
            or payload.get("event_kind") != event["event_kind"]
        ):
            raise invalid("row is invalid or out of order")
        previous = sequence
    if through != previous:
        raise invalid("cursor does not match the page")


class CCLCOpenRequestClient:
    """Forward exact requests without caching, reducing, or replaying CCLC state."""

    def __init__(self, run_root: Path, *, timeout_seconds: float = 5.0) -> None:
        if not 0 < timeout_seconds <= 30:
            raise ValueError("CCLC timeout must be in (0, 30] seconds")
        try:
            self._run_root = PinnedRunRoot(run_root)
        except PrivateTransportCapabilityError as error:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC run capability is invalid: {error}",
            ) from error
        self._timeout_seconds = timeout_seconds

    def _exchange(
        self, command: str, payload: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        request = {
            "api_version": CCLC_OPEN_REQUEST_VERSION,
            "command": command,
            "payload": dict(payload),
        }
        encoded = _canonical_json(
            request,
            error_code="cclc_open_request_invalid",
            label="CCLC request",
        )
        if len(encoded) > MAX_CCLC_REQUEST_BYTES:
            raise HeadlessApiError(
                "cclc_open_request_oversized",
                "CCLC request exceeds its byte bound",
            )
        try:
            with self._run_root.socket_address(CCLC_SOCKET_FILE) as socket_address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAX_CCLC_RESPONSE_BYTES,
                    )
                    client.settimeout(self._timeout_seconds)
                    client.connect(socket_address)
                    sent = client.send(encoded)
                    if sent != len(encoded):
                        raise HeadlessApiError(
                            "cclc_open_transport_failure",
                            "CCLC request packet was truncated",
                        )
                    response_bytes, _ancillary, flags, _address = client.recvmsg(
                        MAX_CCLC_RESPONSE_BYTES
                    )
        except HeadlessApiError:
            raise
        except PrivateTransportCapabilityError as exc:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC command socket is unavailable: {exc}",
            ) from exc
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                f"CCLC transport failed: {exc}",
            ) from exc
        if flags & socket.MSG_TRUNC or not response_bytes:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is empty or oversized",
            )
        try:
            response_text = response_bytes.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is not UTF-8",
            ) from exc
        response = strict_json_object(response_text, label="CCLC response")
        if (
            _canonical_json(
                response,
                error_code="cclc_open_response_invalid",
                label="CCLC response",
            )
            != response_bytes
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response is not canonical JSON",
            )
        if (
            set(response) != _RESPONSE_FIELDS
            or response.get("api_version") != CCLC_OPEN_RESPONSE_VERSION
            or response.get("command") != command
            or type(response.get("ok")) is not bool
        ):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response envelope is invalid",
            )
        if response["ok"] is False:
            error = response.get("error")
            if (
                response.get("result") is not None
                or not isinstance(error, dict)
                or set(error) != {"code", "details", "message"}
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not error["message"]
                or not isinstance(error.get("details"), dict)
                or not all(
                    isinstance(key, str) and isinstance(value, str)
                    for key, value in error["details"].items()
                )
            ):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC error response is invalid",
                )
            raise HeadlessApiError(
                error["code"], error["message"], details=error["details"]
            )
        if response.get("error") is not None:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "successful CCLC response carries an error",
            )
        result = response.get("result")
        if result is not None and not isinstance(result, dict):
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                "CCLC response result is not an object or null",
            )
        return result

    @staticmethod
    def _require_object(
        result: dict[str, Any] | None, *, command: str
    ) -> dict[str, Any]:
        if result is None:
            raise HeadlessApiError(
                "cclc_open_response_invalid",
                f"{command} returned a null result",
            )
        return result

    def owner_identity(self) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("owner_identity", {}), command="owner_identity"
        )
        if (
            set(result) != {"request_owner_epoch"}
            or not isinstance(result.get("request_owner_epoch"), str)
            or not result["request_owner_epoch"]
            or len(result["request_owner_epoch"].encode("utf-8")) > 256
        ):
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC owner identity is invalid",
            )
        return result

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        result = self._require_object(
            self._exchange(
                "component_snapshot",
                {"controllable_component_id": component_id},
            ),
            command="component_snapshot",
        )
        if result.get("controllable_component_id") != component_id:
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC component snapshot identity does not match its request",
            )
        _validate_lifecycle_target_facts(result)
        return result

    def route_templates(self, component_id: str) -> dict[str, Any]:
        result = self._require_object(
            self._exchange(
                "route_templates",
                {"controllable_component_id": component_id},
            ),
            command="route_templates",
        )
        if (
            set(result)
            != {
                "controllable_component_id",
                "graph_revision",
                "retained_bundle_id",
                "templates",
            }
            or result.get("controllable_component_id") != component_id
            or not isinstance(result.get("graph_revision"), str)
            or not result["graph_revision"]
            or not isinstance(result.get("retained_bundle_id"), str)
            or not result["retained_bundle_id"]
            or not isinstance(result.get("templates"), list)
        ):
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC route templates do not match their component request",
            )
        return result

    def submit_lifecycle(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("lifecycle_submit", intent),
            command="lifecycle_submit",
        )
        acceptance = result.get("acceptance")
        context = intent.get("request_context")
        request_id = context.get("request_id") if isinstance(context, dict) else None
        if acceptance is not None and (
            not isinstance(acceptance, dict)
            or acceptance.get("request_id") != request_id
        ):
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC lifecycle acceptance does not match its request",
            )
        _validate_incident_recovery(result, intent)
        return result

    def lifecycle_request_result(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        result = self._request_result("lifecycle_request_result", intent)
        if result is not None:
            _validate_incident_recovery(result, intent)
        return result

    def incident_events(self, after_sequence: int, limit: int) -> dict[str, Any]:
        """Read incident history events after `after_sequence`, in order."""

        if (
            not _is_int(after_sequence)
            or not 0 <= after_sequence <= _MAX_SAFE_INTEGER
            or not _is_int(limit)
            or not 1 <= limit <= MAX_INCIDENT_EVENT_PAGE
        ):
            raise ValueError("incident event cursor or limit is outside its bound")
        result = self._require_object(
            self._exchange(
                "incident_events",
                {"after_sequence": after_sequence, "limit": limit},
            ),
            command="incident_events",
        )
        _validate_incident_events(result, after_sequence, limit)
        return result

    def route_request_result(self, intent: Mapping[str, Any]) -> dict[str, Any] | None:
        return self._request_result("route_request_result", intent)

    def _request_result(
        self, command: str, intent: Mapping[str, Any]
    ) -> dict[str, Any] | None:
        result = self._exchange(command, intent)
        if result is not None:
            acceptance = result.get("acceptance")
            if acceptance is not None and (
                not isinstance(acceptance, dict)
                or acceptance.get("request_id")
                != intent["request_context"]["request_id"]
            ):
                raise HeadlessApiError(
                    "cclc_open_response_identity_mismatch",
                    "CCLC result names another request",
                )
        return result

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None:
        result = self._exchange(
            "lifecycle_status",
            {"lifecycle_transaction_id": transaction_id},
        )
        if result is not None:
            acceptance = result.get("acceptance")
            if (
                not isinstance(acceptance, dict)
                or acceptance.get("lifecycle_transaction_id") != transaction_id
            ):
                raise HeadlessApiError(
                    "cclc_open_response_identity_mismatch",
                    "CCLC lifecycle status does not match its request",
                )
        return result

    def submit_route(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("route_submit", intent), command="route_submit"
        )
        acceptance = result.get("acceptance")
        context = intent.get("request_context")
        request_id = context.get("request_id") if isinstance(context, dict) else None
        if acceptance is not None and (
            not isinstance(acceptance, dict)
            or acceptance.get("request_id") != request_id
        ):
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC route acceptance does not match its request",
            )
        return result

    def route_status(self, transaction_id: str) -> dict[str, Any] | None:
        result = self._exchange(
            "route_status",
            {"route_selection_transaction_id": transaction_id},
        )
        if result is not None:
            acceptance = result.get("acceptance")
            if (
                not isinstance(acceptance, dict)
                or acceptance.get("route_selection_transaction_id") != transaction_id
            ):
                raise HeadlessApiError(
                    "cclc_open_response_identity_mismatch",
                    "CCLC route status does not match its request",
                )
        return result
