"""Strict private wire client for the Kernel cell-admission owner."""

from __future__ import annotations

from collections.abc import Mapping
import json
import logging
import os
from pathlib import Path
import select
import socket
import struct
from typing import Any, Final

import rfc8785

from manyforge_behavior.json_scalars import nonnegative_int64

from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


EXECUTION_SOCKET_FILE: Final = "execution-command.sock"
# The operator-alarm attention heartbeat is served on the independent
# component socket; it never waits behind ordinary traffic.
EXECUTION_COMPONENT_SOCKET_FILE: Final = "execution-component.sock"
EXECUTION_COMMAND_VERSION: Final = "manyforge.execution_command.v4"
EXECUTION_RESPONSE_VERSION: Final = "manyforge.execution_command_response.v5"
MAXIMUM_PACKET_BYTES: Final = 1024 * 1024
MAXIMUM_RESPONSE_BYTES: Final = 4 * 1024 * 1024
_COMMANDS: Final = frozenset(
    {
        "cell_admission_get",
        "cell_admission_acquire",
        "cell_admission_release",
        "cell_admission_recovery_receipt",
        "cell_admission_publish_operator_alarm_attention",
    }
)
_ATTENTION_ACCEPTED_KEYS: Final = frozenset({"generation"})
_ERROR_RESPONSE_KEYS: Final = frozenset(
    {"api_version", "command", "error", "ok"}
)
_ERROR_KEYS: Final = frozenset({"code", "details", "message"})
_AUTHORING_KEYS: Final = frozenset(
    {"admission_generation", "authoring_allowed"}
)
_CLAIM_KEYS: Final = frozenset(
    {
        "accepted_at_ns",
        "acquired_admission_generation",
        "claim_generation",
        "intent",
    }
)
_INTENT_KEYS: Final = frozenset(
    {
        "claim_kind",
        "expected_admission_generation",
        "intervention_evidence",
        "owner_role",
        "owner_transaction_id",
        "program_cycle_evidence",
        "request_key",
        "scope",
    }
)
_RELEASE_KEYS: Final = frozenset(
    {
        "acquired_admission_generation",
        "claim_generation",
        "owner_role",
        "owner_transaction_id",
        "request_key",
        "settlement",
    }
)
_RECOVERY_QUERY_KEYS: Final = frozenset(
    {"expected_claim", "expected_release", "intent"}
)
_RECOVERY_RECEIPT_KEYS: Final = frozenset(
    {
        "admission_generation",
        "held_claims",
        "matched_claim",
        "matched_release",
        "status",
    }
)


class CellAdmissionError(RuntimeError):
    """Stable native admission failure preserving the native diagnostic."""

    def __init__(
        self,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
        *,
        outcome_unknown: bool = False,
        producer: str | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})
        self.outcome_unknown = outcome_unknown
        self.producer = producer


class CellAdmissionClient:
    """Strict bounded client for owner-specific admission integration."""

    def __init__(self, run_root: Path, *, timeout_seconds: float = 10.0) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        try:
            self._run_root = PinnedRunRoot(run_root)
        except PrivateTransportCapabilityError as error:
            raise CellAdmissionError(
                "execution_owner_unavailable",
                f"native execution run capability is invalid: {error}",
            ) from error
        self._timeout_seconds = timeout_seconds

    @staticmethod
    def _strict_object(payload: bytes) -> dict[str, Any]:
        try:
            text = payload.decode("utf-8", errors="strict")
            value = json.loads(
                text,
                parse_constant=lambda token: (_ for _ in ()).throw(
                    ValueError(f"non-finite JSON value {token}")
                ),
                object_pairs_hook=_unique_object,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native response is not strict JSON",
            ) from error
        if not isinstance(value, dict):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native response is not a JSON object",
            )
        try:
            canonical = rfc8785.dumps(value)
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as error:
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native response is not canonically encodable",
            ) from error
        if canonical != payload:
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native response is not canonical JSON",
            )
        return value

    def _exchange(
        self,
        packet: dict[str, Any],
        *,
        socket_file: str = EXECUTION_SOCKET_FILE,
        stop_fd: int | None = None,
    ) -> dict[str, Any]:
        try:
            encoded = rfc8785.dumps(packet)
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as error:
            raise CellAdmissionError(
                "execution_command_invalid",
                "cell-admission command is not canonically encodable",
            ) from error
        if len(encoded) > MAXIMUM_PACKET_BYTES:
            raise CellAdmissionError(
                "execution_command_oversized",
                "cell-admission command exceeds its byte bound",
            )
        operation = "resolve_socket"
        peer_pid: int | None = None
        try:
            with self._run_root.socket_address(socket_file) as socket_address:
                operation = "socket"
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    operation = "setsockopt"
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAXIMUM_RESPONSE_BYTES,
                    )
                    # Only the connection is limited (OD-214): a connected
                    # request waits for its answer, ended by the host's
                    # answer, by the socket closing when the host exits
                    # (an empty read), or by the caller's stop pipe.
                    operation = "settimeout"
                    client.settimeout(self._timeout_seconds)
                    operation = "connect"
                    client.connect(socket_address)
                    if hasattr(socket, "SO_PEERCRED"):
                        operation = "getsockopt(SO_PEERCRED)"
                        credentials = client.getsockopt(
                            socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                        )
                        _pid, peer_uid, _gid = struct.unpack("3i", credentials)
                        peer_pid = _pid
                        if peer_uid != os.geteuid():
                            raise OSError("peer is not owned by the current user")
                    operation = "settimeout"
                    client.settimeout(None)
                    if stop_fd is not None:
                        # The host taking the packet or the caller's stop: a
                        # host whose queue is full never holds a shutdown.
                        operation = "select"
                        _, writable, _ = select.select([stop_fd], [client], [])
                        if not writable:
                            raise ConnectionAbortedError("the exchange was stopped")
                    operation = "send"
                    if client.send(encoded) != len(encoded):
                        raise OSError("command packet was truncated")
                    if stop_fd is not None:
                        # The answer, the host closing the connection or
                        # the caller's stop (OD-296 pattern).
                        operation = "select"
                        readable, _, _ = select.select([client, stop_fd], [], [])
                        if stop_fd in readable:
                            raise ConnectionAbortedError("the exchange was stopped")
                    operation = "recvmsg"
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAXIMUM_RESPONSE_BYTES
                    )
                    operation = "validate_socket"
        except (
            PrivateTransportCapabilityError,
            FileNotFoundError,
            OSError,
            TimeoutError,
        ) as error:
            logging.getLogger(__name__).warning(
                "Native cell-admission transport failed: syscall=%s errno=%s peer_socket=%s peer_pid=%s exception_type=%s",
                operation, getattr(error, "errno", None), socket_file, peer_pid, type(error).__name__,
            )
            raise CellAdmissionError(
                "execution_owner_unavailable",
                f"native execution transport failed: {error}",
                outcome_unknown=True,
            ) from error
        if not payload or flags & socket.MSG_TRUNC:
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native response is empty or oversized",
                outcome_unknown=True,
            )
        try:
            return self._strict_object(payload)
        except CellAdmissionError as error:
            error.outcome_unknown = True
            raise

    def _call(
        self,
        command: str,
        request: Mapping[str, Any] | None,
        *,
        stop_fd: int | None = None,
        program_cycle_status: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        if command not in _COMMANDS:
            raise ValueError("unsupported cell-admission command")
        packet = {
            "api_version": EXECUTION_COMMAND_VERSION,
            "command": command,
        }
        if command == "cell_admission_get":
            if request is not None:
                raise ValueError("cell-admission get has no request")
        else:
            if not isinstance(request, Mapping):
                raise TypeError("cell-admission request must be a mapping")
            packet["request"] = dict(request)
        if program_cycle_status is not None:
            packet["program_cycle_status"] = dict(program_cycle_status)
        response = self._exchange(
            packet,
            stop_fd=stop_fd,
            socket_file=(
                EXECUTION_COMPONENT_SOCKET_FILE
                if command == "cell_admission_publish_operator_alarm_attention"
                else EXECUTION_SOCKET_FILE
            ),
        )
        if (
            response.get("api_version") != EXECUTION_RESPONSE_VERSION
            or response.get("command") != command
            or type(response.get("ok")) is not bool
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission response has the wrong envelope",
                outcome_unknown=True,
            )
        if response["ok"] is not True:
            if set(response) != _ERROR_RESPONSE_KEYS:
                raise CellAdmissionError(
                    "execution_command_response_invalid",
                    "native cell-admission error has the wrong envelope",
                    outcome_unknown=True,
                )
            error = response.get("error")
            producer = (
                error.get("producer", "kernel")
                if isinstance(error, dict)
                else None
            )
            if (
                not isinstance(error, dict)
                or frozenset(error)
                not in {_ERROR_KEYS, _ERROR_KEYS | {"producer"}}
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not isinstance(error.get("details"), dict)
                or not isinstance(producer, str)
                or not producer
            ):
                raise CellAdmissionError(
                    "execution_command_response_invalid",
                    "native cell-admission error is malformed",
                    outcome_unknown=True,
                )
            raise CellAdmissionError(
                error["code"], error["message"], error["details"],
                producer=producer,
            )
        result_key = {
            "cell_admission_acquire": "claim",
            "cell_admission_recovery_receipt": "recovery_receipt",
            "cell_admission_publish_operator_alarm_attention": (
                "operator_alarm_attention"
            ),
        }.get(command, "authoring_admission")
        expected_keys = frozenset({"api_version", "command", "ok", result_key})
        if set(response) != expected_keys or not isinstance(
            response.get(result_key), dict
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission result is malformed",
                outcome_unknown=True,
            )
        return response[result_key]

    def get(self) -> dict[str, Any]:
        return self._authoring(self._call("cell_admission_get", None))

    @staticmethod
    def _authoring(value: dict[str, Any]) -> dict[str, Any]:
        if (
            set(value) != _AUTHORING_KEYS
            or type(value.get("authoring_allowed")) is not bool
            or type(value.get("admission_generation")) is not int
            or not 1 <= value["admission_generation"] <= 9_007_199_254_740_991
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native authoring-admission projection is malformed",
            )
        return value

    def acquire(
        self,
        intent: Mapping[str, Any],
        *,
        program_cycle_status: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        """A Program Reset claim carries the runner's current cycle status
        beside its intent, and only it does (OD-139)."""
        if set(intent) not in (_INTENT_KEYS, _INTENT_KEYS | {"program_node_evidence"}):
            raise ValueError("claim intent is not the exact A1 projection")
        result = self._call(
            "cell_admission_acquire", intent, program_cycle_status=program_cycle_status
        )
        try:
            nonnegative_int64(result.get("accepted_at_ns"))
        except ValueError as error:
            raise CellAdmissionError("execution_command_response_invalid",
                                     "native claim acceptance time is malformed") from error
        if (
            set(result) != _CLAIM_KEYS
            or result.get("intent") != dict(intent)
            or any(
                type(result.get(key)) is not int
                for key in (
                    "acquired_admission_generation",
                    "claim_generation",
                )
            )
            or not 1
            <= result["acquired_admission_generation"]
            <= 9_007_199_254_740_991
            or not 1 <= result["claim_generation"] <= 9_007_199_254_740_991
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission claim is malformed",
                outcome_unknown=True,
            )
        return result

    def release(self, release: Mapping[str, Any]) -> dict[str, Any]:
        if set(release) != _RELEASE_KEYS:
            raise ValueError("claim release is not the exact A1 projection")
        return self._authoring(self._call("cell_admission_release", release))

    def recovery_receipt(self, query: Mapping[str, Any]) -> dict[str, Any]:
        if set(query) != _RECOVERY_QUERY_KEYS:
            raise ValueError("recovery query is not the exact A1 projection")
        result = self._call("cell_admission_recovery_receipt", query)
        if (
            set(result) != _RECOVERY_RECEIPT_KEYS
            or result.get("status")
            not in {
                "unknown",
                "conflicting",
                "held_exact",
                "released_exact",
            }
            or not isinstance(result.get("held_claims"), list)
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native recovery receipt is malformed",
                outcome_unknown=True,
            )
        return result

    def publish_operator_alarm_attention(
        self, projection: Mapping[str, Any], *, stop_fd: int | None = None
    ) -> dict[str, Any]:
        """Publish the Alarm Lifecycle Owner's fenced motion-attention view.

        The Kernel retains the newest generation and alone
        evaluates it at Run/Resume/recovery admission, judging its age by
        its own freshness bound. The wait for the acceptance has no time
        limit (OD-307): it ends by the Kernel's answer, by the socket closing
        when the host exits, or by ``stop_fd`` (the read end of the caller's
        stop pipe) becoming readable.
        """
        result = self._call(
            "cell_admission_publish_operator_alarm_attention",
            projection,
            stop_fd=stop_fd,
        )
        if (
            set(result) != _ATTENTION_ACCEPTED_KEYS
            or result.get("generation") != projection.get("generation")
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native operator-alarm attention acceptance is malformed",
            )
        return result


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member {key!r}")
        result[key] = value
    return result


__all__ = [
    "CellAdmissionClient",
    "CellAdmissionError",
    "EXECUTION_COMMAND_VERSION",
    "EXECUTION_RESPONSE_VERSION",
]
