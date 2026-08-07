"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

import os
import socket
import stat
from pathlib import Path
from typing import Any, Protocol

import rfc8785

from .models import OperationIntentRequest
from .native import HeadlessApiError, strict_json_object


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v1"
MAX_EXECUTION_PACKET_BYTES = 1024 * 1024
MAX_EXECUTION_RESPONSE_BYTES = 1024 * 1024


class ExecutionPort(Protocol):
    def status(self) -> dict[str, Any]: ...

    def submit(self, request: OperationIntentRequest) -> dict[str, Any]: ...

    def read(self, operation_id: str) -> dict[str, Any]: ...


def _canonical_json(value: dict[str, Any]) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            "execution_command_invalid",
            f"execution command is not canonically encodable: {exc}",
        ) from exc


class ExecutionCommandClient:
    """Forward intents and observations without caching execution truth."""

    def __init__(self, state_root: Path, *, timeout_seconds: float = 10.0) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        self._socket_path = state_root / EXECUTION_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    def _validate_socket(self) -> None:
        try:
            metadata = self._socket_path.lstat()
        except FileNotFoundError as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution command socket is absent",
            ) from exc
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution path is not the expected same-user socket",
            )

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_EXECUTION_PACKET_BYTES:
            raise HeadlessApiError(
                "execution_command_oversized",
                "execution command exceeds its byte bound",
            )
        self._validate_socket()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(self._timeout_seconds)
                client.connect(str(self._socket_path))
                sent = client.send(encoded)
                if sent != len(encoded):
                    raise HeadlessApiError(
                        "execution_command_transport_failure",
                        "execution command packet was truncated",
                    )
                payload, _ancillary, flags, _address = client.recvmsg(
                    MAX_EXECUTION_RESPONSE_BYTES
                )
        except HeadlessApiError:
            raise
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution transport failed: {exc}",
            ) from exc
        if flags & socket.MSG_TRUNC or not payload:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution response is empty or oversized",
            )
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution response is not UTF-8",
            ) from exc
        response = strict_json_object(text, label="execution command response")
        if _canonical_json(response) != payload:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution response is not canonical JSON",
            )
        if response.get("api_version") != EXECUTION_RESPONSE_VERSION:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution response version is unsupported",
            )
        if type(response.get("ok")) is not bool:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution response has no boolean result",
            )
        return response

    def status(self) -> dict[str, Any]:
        response = self._exchange(
            {"api_version": EXECUTION_COMMAND_VERSION, "command": "status"}
        )
        if response.get("ok") is True and response.get("command") != "status":
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution status response has the wrong command kind",
            )
        return response

    def submit(self, request: OperationIntentRequest) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "submit",
                "operation": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "submit"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution acceptance does not match the submitted request",
            )
        return response

    def read(self, operation_id: str) -> dict[str, Any]:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read",
                "operation_id": operation_id,
            }
        )
        operation = response.get("operation")
        if response.get("ok") is True and (
            response.get("command") != "read"
            or not isinstance(operation, dict)
            or operation.get("operation_id") != operation_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution observation does not match the requested operation",
            )
        return response


__all__ = ["ExecutionCommandClient", "ExecutionPort"]
