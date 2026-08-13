"""Bounded same-user client for the sole CCLC owner."""

from __future__ import annotations

from collections.abc import Mapping
import os
from pathlib import Path
import socket
import stat
from typing import Any, Protocol

import rfc8785

from .native import HeadlessApiError, strict_json_object


CCLC_SOCKET_FILE = "cclc-command.sock"
CCLC_OPEN_REQUEST_VERSION = "manyforge.cclc_open_request.v1"
CCLC_OPEN_RESPONSE_VERSION = "manyforge.cclc_open_response.v1"
MAX_CCLC_REQUEST_BYTES = 4 * 1024 * 1024
MAX_CCLC_RESPONSE_BYTES = 8 * 1024 * 1024
_RESPONSE_FIELDS = frozenset(
    {"api_version", "command", "error", "ok", "result"}
)


class CCLCOpenPort(Protocol):
    """Caller-neutral lifecycle/route surface; implementations own no truth."""

    def component_snapshot(self, component_id: str) -> dict[str, Any]: ...

    def route_templates(self, component_id: str) -> dict[str, Any]: ...

    def submit_lifecycle(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any] | None: ...

    def submit_route(self, intent: Mapping[str, Any]) -> dict[str, Any]: ...

    def route_status(self, transaction_id: str) -> dict[str, Any] | None: ...


def _canonical_json(
    value: Mapping[str, Any], *, error_code: str, label: str
) -> bytes:
    try:
        return rfc8785.dumps(dict(value))
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            error_code,
            f"{label} is not canonically encodable: {exc}",
        ) from exc


class CCLCOpenRequestClient:
    """Forward exact requests without caching, reducing, or replaying CCLC state."""

    def __init__(self, state_root: Path, *, timeout_seconds: float = 5.0) -> None:
        if not 0 < timeout_seconds <= 30:
            raise ValueError("CCLC timeout must be in (0, 30] seconds")
        self._socket_path = state_root / CCLC_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    def _validate_socket(self) -> None:
        try:
            metadata = self._socket_path.lstat()
        except FileNotFoundError as exc:
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                "CCLC command socket is absent",
            ) from exc
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HeadlessApiError(
                "cclc_owner_unavailable",
                "CCLC command path is not the expected same-user socket",
            )

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
        self._validate_socket()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_RCVBUF,
                    MAX_CCLC_RESPONSE_BYTES,
                )
                client.settimeout(self._timeout_seconds)
                client.connect(str(self._socket_path))
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
                    isinstance(key, str)
                    and isinstance(value, str)
                    for key, value in error["details"].items()
                )
            ):
                raise HeadlessApiError(
                    "cclc_open_response_invalid",
                    "CCLC error response is invalid",
                )
            raise HeadlessApiError(error["code"], error["message"])
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

    def submit_lifecycle(
        self, intent: Mapping[str, Any]
    ) -> dict[str, Any]:
        result = self._require_object(
            self._exchange("lifecycle_submit", intent),
            command="lifecycle_submit",
        )
        acceptance = result.get("acceptance")
        context = intent.get("request_context")
        request_id = context.get("request_id") if isinstance(context, dict) else None
        if (
            acceptance is not None
            and (
                not isinstance(acceptance, dict)
                or acceptance.get("request_id") != request_id
            )
        ):
            raise HeadlessApiError(
                "cclc_open_response_identity_mismatch",
                "CCLC lifecycle acceptance does not match its request",
            )
        return result

    def lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any] | None:
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
        if (
            acceptance is not None
            and (
                not isinstance(acceptance, dict)
                or acceptance.get("request_id") != request_id
            )
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
                or acceptance.get("route_selection_transaction_id")
                != transaction_id
            ):
                raise HeadlessApiError(
                    "cclc_open_response_identity_mismatch",
                    "CCLC route status does not match its request",
                )
        return result
