"""Bounded same-user client for the Manager daemon's private protocol."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import stat
from typing import Any, Protocol

from .models import ManagerApplyRequest
from .native import HeadlessApiError, strict_json_object


MANAGER_SOCKET_FILE = "manager-command.sock"
MANAGER_COMMAND_VERSION = "manyforge.manager_command.v1"
MANAGER_RESPONSE_VERSION = "manyforge.manager_command_response.v1"
MAX_MANAGER_PACKET_BYTES = 16 * 1024
MAX_MANAGER_RESPONSE_BYTES = 64 * 1024


class ManagerPort(Protocol):
    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]: ...


def _canonical_json(value: dict[str, Any]) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


class ManagerCommandClient:
    """Forward one packet at a time; never cache or reduce Manager truth."""

    def __init__(self, state_root: Path, *, timeout_seconds: float = 0.25) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 5:
            raise ValueError("Manager timeout must be in (0, 5] seconds")
        self._socket_path = state_root / MANAGER_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    def _validate_socket(self) -> None:
        try:
            metadata = self._socket_path.lstat()
        except FileNotFoundError as exc:
            raise HeadlessApiError(
                "manager_command_unavailable",
                "Manager command socket is absent",
            ) from exc
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HeadlessApiError(
                "manager_command_unavailable",
                "Manager command path is not the expected same-user socket",
            )

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_MANAGER_PACKET_BYTES:
            raise HeadlessApiError(
                "manager_command_oversized",
                "Manager command exceeds its byte bound",
            )
        self._validate_socket()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(self._timeout_seconds)
                client.connect(str(self._socket_path))
                sent = client.send(encoded)
                if sent != len(encoded):
                    raise HeadlessApiError(
                        "manager_command_transport_failure",
                        "Manager command packet was truncated",
                    )
                payload, _ancillary, flags, _address = client.recvmsg(
                    MAX_MANAGER_RESPONSE_BYTES
                )
        except HeadlessApiError:
            raise
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "manager_command_unavailable",
                f"Manager command transport failed: {exc}",
            ) from exc
        if flags & socket.MSG_TRUNC or not payload:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response is empty or oversized",
            )
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response is not UTF-8",
            ) from exc
        response = strict_json_object(text, label="Manager command response")
        if response.get("api_version") != MANAGER_RESPONSE_VERSION:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response version is unsupported",
            )
        if type(response.get("ok")) is not bool:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response has no boolean result",
            )
        return response

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "apply",
                "correlation_id": request.correlation_id,
                "deployment_artifact_id": request.deployment_artifact_id,
                "deployment_revision": request.deployment_revision,
                "expected_current": request.expected_current.model_dump(),
                "reject_if_affected_busy": request.reject_if_affected_busy,
                "request_id": request.request_id,
                "request_owner_epoch": request.request_owner_epoch,
                "source_label": request.source_label,
            }
        )
        if (
            response.get("request_id") != request.request_id
            or response.get("request_owner_epoch") != request.request_owner_epoch
        ):
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "Manager Apply response does not match the submitted request",
            )
        return response
