"""Client of the Kernel cell-admission owner.

``get``, ``acquire``, ``release`` and ``recovery_receipt`` are one gateway
core call each (``manyforge_headless_py.Gateway``, GIL released, only the
connect limited; OD-214). The operator-alarm attention publication keeps its
own exchange here: its wait for the Kernel's acceptance also ends at the
publisher's shutdown (the caller's stop pipe, OD-307), which a core call
cannot.
"""

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

from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


_LOGGER = logging.getLogger(__name__)

# The operator-alarm attention heartbeat is served on the independent
# component socket; it never waits behind ordinary traffic.
EXECUTION_COMPONENT_SOCKET_FILE: Final = "execution-component.sock"
EXECUTION_COMMAND_VERSION: Final = "manyforge.execution_command.v4"
EXECUTION_RESPONSE_VERSION: Final = "manyforge.execution_command_response.v5"
MAXIMUM_PACKET_BYTES: Final = 1024 * 1024
MAXIMUM_RESPONSE_BYTES: Final = 4 * 1024 * 1024
_ATTENTION_COMMAND: Final = "cell_admission_publish_operator_alarm_attention"
# Core failures raised before anything was sent: their outcome is known.
_NOT_SENT_CODES: Final = frozenset(
    {"execution_command_invalid", "execution_command_oversized"}
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
    """Owner-specific admission integration over the gateway core."""

    def __init__(self, run_root: Path, *, timeout_seconds: float = 10.0) -> None:
        # ``timeout_seconds`` limits the connect only.
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        import manyforge_headless_py as core  # type: ignore[import-not-found]

        self._gateway_error: type[Exception] = core.GatewayError
        try:
            # Cell-admission commands carry no attribution.
            self._gateway = core.Gateway(
                str(run_root), None, attribution="internal",
                connect_limit_s=timeout_seconds,
            )
            self._run_root = PinnedRunRoot(run_root)
        except (core.GatewayError, PrivateTransportCapabilityError) as error:
            raise CellAdmissionError(
                "execution_owner_unavailable",
                f"native execution run capability is invalid: {error}",
            ) from error
        self._timeout_seconds = timeout_seconds

    def _call(
        self,
        command: str,
        members: dict[str, Any],
    ) -> dict[str, Any]:
        """One gateway call; the result member of an accepted reply."""

        try:
            response = self._gateway.host(command, members)
        except self._gateway_error as error:
            _LOGGER.warning(
                "Cell-admission call failed: command=%s code=%s message=%s",
                command, error.code, error.message,
            )
            raise CellAdmissionError(
                error.code, error.message, error.details,
                outcome_unknown=error.code not in _NOT_SENT_CODES,
                producer=error.producer,
            ) from error
        return _result(command, response)

    def get(self) -> dict[str, Any]:
        return self._call("cell_admission_get", {})

    def acquire(
        self,
        intent: Mapping[str, Any],
        *,
        program_cycle_status: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        """A Program Reset claim carries the runner's current cycle status
        beside its intent, and only it does (OD-139)."""
        members: dict[str, Any] = {"request": dict(intent)}
        if program_cycle_status is not None:
            members["program_cycle_status"] = dict(program_cycle_status)
        return self._call("cell_admission_acquire", members)

    def release(self, release: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("cell_admission_release", {"request": dict(release)})

    def recovery_receipt(self, query: Mapping[str, Any]) -> dict[str, Any]:
        return self._call("cell_admission_recovery_receipt", {"request": dict(query)})

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
        packet = {
            "api_version": EXECUTION_COMMAND_VERSION,
            "command": _ATTENTION_COMMAND,
            "request": dict(projection),
        }
        response = self._exchange(packet, stop_fd=stop_fd)
        if (
            response.get("api_version") != EXECUTION_RESPONSE_VERSION
            or response.get("command") != _ATTENTION_COMMAND
            or type(response.get("ok")) is not bool
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native cell-admission response has the wrong envelope",
                outcome_unknown=True,
            )
        result = _result(_ATTENTION_COMMAND, response)
        if result.get("generation") != projection.get("generation"):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native operator-alarm attention acceptance names another generation",
            )
        return result

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
        socket_file = EXECUTION_COMPONENT_SOCKET_FILE
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
            _LOGGER.warning(
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


_RESULT_MEMBERS: Final = {
    "cell_admission_acquire": "claim",
    "cell_admission_recovery_receipt": "recovery_receipt",
    _ATTENTION_COMMAND: "operator_alarm_attention",
}


def _result(command: str, response: dict[str, Any]) -> dict[str, Any]:
    """An accepted reply's result member; a refusal as its typed error."""

    if response["ok"] is not True:
        error = response["error"]
        raise CellAdmissionError(
            error["code"], error["message"], error.get("details"),
            producer=error.get("producer", "kernel"),
        )
    return response[_RESULT_MEMBERS.get(command, "authoring_admission")]


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
