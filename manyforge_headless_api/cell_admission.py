"""Strict private wire client for the Kernel cell-admission owner."""

from __future__ import annotations

from collections.abc import Mapping
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import time
from typing import Any, Final

import rfc8785

from manyforge_behavior.json_scalars import nonnegative_int64

from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


EXECUTION_SOCKET_FILE: Final = "execution-command.sock"
# The operator-alarm attention heartbeat is served beside Stop on the
# independent control socket; it never waits behind ordinary traffic.
EXECUTION_CONTROL_SOCKET_FILE: Final = "execution-control.sock"
EXECUTION_COMMAND_VERSION: Final = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION: Final = "manyforge.execution_command_response.v1"
MAXIMUM_PACKET_BYTES: Final = 1024 * 1024
MAXIMUM_RESPONSE_BYTES: Final = 4 * 1024 * 1024
_CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1"
_COMMANDS: Final = frozenset(
    {
        "cell_admission_get",
        "cell_admission_acquire",
        "cell_admission_release",
        "cell_admission_reconcile",
        "cell_admission_recovery_receipt",
        "cell_admission_publish_operator_alarm_attention",
    }
)
_ATTENTION_ACCEPTED_KEYS: Final = frozenset({"generation", "owner_epoch"})
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
        "owner_epoch",
        "owner_role",
        "owner_transaction_id",
        "program_cycle_evidence",
        "request_id",
        "request_payload_digest",
        "scope",
    }
)
_RELEASE_KEYS: Final = frozenset(
    {
        "acquired_admission_generation",
        "claim_generation",
        "owner_epoch",
        "owner_role",
        "owner_transaction_id",
        "request_id",
        "request_payload_digest",
        "settlement",
    }
)
_RECONCILE_KEYS: Final = frozenset(
    {
        "current_owner_epochs",
        "kernel_owner_epoch",
        "reconciliation_evidence_digest",
        "retained_claims",
    }
)
_RECOVERY_QUERY_KEYS: Final = frozenset(
    {"expected_claim", "expected_release", "intent"}
)
_RECOVERY_RECEIPT_KEYS: Final = frozenset(
    {
        "admission_generation",
        "current_owner_epochs",
        "held_claims",
        "kernel_owner_epoch",
        "matched_claim",
        "matched_reconcile_settlement",
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
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})
        self.outcome_unknown = outcome_unknown


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

    def _socket_timeout(self, deadline: float | None) -> float:
        """Per-operation timeout, never past an overall monotonic deadline."""

        if deadline is None:
            return self._timeout_seconds
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise TimeoutError("cell-admission command deadline elapsed")
        return min(remaining, self._timeout_seconds)

    def _exchange(
        self,
        packet: dict[str, Any],
        *,
        deadline: float | None = None,
        socket_file: str = EXECUTION_SOCKET_FILE,
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
        try:
            with self._run_root.socket_address(socket_file) as socket_address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAXIMUM_RESPONSE_BYTES,
                    )
                    # With a deadline every blocking step (connect, send,
                    # receive) gets only the time that remains of it.
                    client.settimeout(self._socket_timeout(deadline))
                    client.connect(socket_address)
                    if hasattr(socket, "SO_PEERCRED"):
                        credentials = client.getsockopt(
                            socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                        )
                        _pid, peer_uid, _gid = struct.unpack("3i", credentials)
                        if peer_uid != os.geteuid():
                            raise OSError("peer is not owned by the current user")
                    client.settimeout(self._socket_timeout(deadline))
                    if client.send(encoded) != len(encoded):
                        raise OSError("command packet was truncated")
                    client.settimeout(self._socket_timeout(deadline))
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAXIMUM_RESPONSE_BYTES
                    )
        except (
            PrivateTransportCapabilityError,
            FileNotFoundError,
            OSError,
            TimeoutError,
        ) as error:
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
        deadline: float | None = None,
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
        response = self._exchange(
            packet,
            deadline=deadline,
            socket_file=(
                EXECUTION_CONTROL_SOCKET_FILE
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
            if (
                not isinstance(error, dict)
                or set(error) != _ERROR_KEYS
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not isinstance(error.get("details"), dict)
            ):
                raise CellAdmissionError(
                    "execution_command_response_invalid",
                    "native cell-admission error is malformed",
                    outcome_unknown=True,
                )
            raise CellAdmissionError(
                error["code"], error["message"], error["details"]
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

    def acquire(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        if set(intent) not in (_INTENT_KEYS, _INTENT_KEYS | {"program_node_evidence"}):
            raise ValueError("claim intent is not the exact A1 projection")
        result = self._call("cell_admission_acquire", intent)
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

    def reconcile(self, reconciliation: Mapping[str, Any]) -> dict[str, Any]:
        if set(reconciliation) != _RECONCILE_KEYS:
            raise ValueError("reconciliation is not the exact A1 projection")
        return self._authoring(
            self._call("cell_admission_reconcile", reconciliation)
        )

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
                "reconciled_exact",
            }
            or not isinstance(result.get("kernel_owner_epoch"), str)
            or not isinstance(result.get("held_claims"), list)
            or not isinstance(result.get("current_owner_epochs"), list)
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native recovery receipt is malformed",
                outcome_unknown=True,
            )
        return result

    def publish_operator_alarm_attention(
        self, projection: Mapping[str, Any], *, deadline: float
    ) -> dict[str, Any]:
        """Publish the Alarm Lifecycle Owner's fenced motion-attention view.

        The Kernel retains the newest generation of one owner epoch and alone
        evaluates it at Run/Resume/recovery admission (spec 450 §5.2). The
        whole exchange (connect, send, receive) ends by the monotonic
        ``deadline``; a later Kernel acceptance is then unobserved here.
        """
        result = self._call(
            "cell_admission_publish_operator_alarm_attention",
            projection,
            deadline=deadline,
        )
        if (
            set(result) != _ATTENTION_ACCEPTED_KEYS
            or result.get("owner_epoch") != projection.get("owner_epoch")
            or result.get("generation") != projection.get("generation")
        ):
            raise CellAdmissionError(
                "execution_command_response_invalid",
                "native operator-alarm attention acceptance is malformed",
            )
        return result

    def recovery_inventory(
        self, *, owner_epoch: str
    ) -> dict[str, Any]:
        """Obtain one exact Kernel inventory through an unknown Program probe."""
        probe_digest = "sha256:" + hashlib.sha256(
            rfc8785.dumps(
                {
                    "owner_epoch": owner_epoch,
                    "probe": "program-runner-recovery-inventory",
                }
            )
        ).hexdigest()
        return self.recovery_receipt(
            {
                "intent": {
                    "claim_kind": "program_cycle",
                    "owner_role": "program_runner",
                    "owner_epoch": owner_epoch,
                    "owner_transaction_id": "program-runner-recovery-inventory",
                    "request_id": "program-runner-recovery-inventory",
                    "request_payload_digest": probe_digest,
                    "expected_admission_generation": 1,
                    "scope": {"resources": [], "configurations": []},
                    "program_cycle_evidence": {
                        "draft_program_revision": probe_digest,
                        "applied_program_revision": probe_digest,
                    },
                    "intervention_evidence": None,
                },
                "expected_claim": None,
                "expected_release": None,
            }
        )


def cell_admission_owner_epoch_reconciliation_evidence_digest(
    owner_role: str, owner_epoch: str
) -> str:
    """Mirror CellAdmissionOwnerEpochReconciliationEvidenceDigest framing."""
    if not owner_role or not owner_epoch:
        raise ValueError("owner reconciliation identity must be non-empty")
    payload = rfc8785.dumps(
        {"owner_epoch": owner_epoch, "owner_role": owner_role}
    )
    preimage = b"\0".join(
        (
            _CONTENT_IDENTITY_PREFIX,
            b"cell-admission-owner-epoch-reconciliation-evidence",
            b"1.0",
            payload,
        )
    )
    return "sha256:" + hashlib.sha256(preimage).hexdigest()


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
    "cell_admission_owner_epoch_reconciliation_evidence_digest",
]
