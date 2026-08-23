"""Strict private wire client for the Kernel cell-admission owner."""

from __future__ import annotations

from collections.abc import Mapping
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
from typing import Any, Final

import rfc8785


EXECUTION_SOCKET_FILE: Final = "execution-command.sock"
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
    }
)
_ERROR_RESPONSE_KEYS: Final = frozenset(
    {"api_version", "command", "error", "ok"}
)
_ADMISSION_RESPONSE_KEYS: Final = frozenset(
    {"api_version", "authoring_admission", "command", "ok"}
)
_CLAIM_RESPONSE_KEYS: Final = frozenset(
    {"api_version", "claim", "command", "ok"}
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

    def __init__(self, state_root: Path, *, timeout_seconds: float = 10.0) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        self._socket_path = state_root / EXECUTION_SOCKET_FILE
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

    def _exchange(self, packet: dict[str, Any]) -> dict[str, Any]:
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
            metadata = self._socket_path.lstat()
            if (
                not stat.S_ISSOCK(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
            ):
                raise OSError("path is not the expected same-user socket")
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(self._timeout_seconds)
                client.setsockopt(
                    socket.SOL_SOCKET, socket.SO_RCVBUF, MAXIMUM_RESPONSE_BYTES
                )
                client.connect(str(self._socket_path))
                if hasattr(socket, "SO_PEERCRED"):
                    credentials = client.getsockopt(
                        socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                    )
                    _pid, peer_uid, _gid = struct.unpack("3i", credentials)
                    if peer_uid != os.geteuid():
                        raise OSError("peer is not owned by the current user")
                if client.send(encoded) != len(encoded):
                    raise OSError("command packet was truncated")
                payload, _ancillary, flags, _address = client.recvmsg(
                    MAXIMUM_RESPONSE_BYTES
                )
        except (FileNotFoundError, OSError, TimeoutError) as error:
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
        self, command: str, request: Mapping[str, Any] | None
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
        response = self._exchange(packet)
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
        result_key = (
            "claim"
            if command == "cell_admission_acquire"
            else (
                "recovery_receipt"
                if command == "cell_admission_recovery_receipt"
                else "authoring_admission"
            )
        )
        expected_keys = (
            _CLAIM_RESPONSE_KEYS
            if command == "cell_admission_acquire"
            else (
                frozenset(
                    {
                        "api_version",
                        "command",
                        "ok",
                        "recovery_receipt",
                    }
                )
                if command == "cell_admission_recovery_receipt"
                else _ADMISSION_RESPONSE_KEYS
            )
        )
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
        if set(intent) != _INTENT_KEYS:
            raise ValueError("claim intent is not the exact A1 projection")
        result = self._call("cell_admission_acquire", intent)
        if (
            set(result) != _CLAIM_KEYS
            or result.get("intent") != dict(intent)
            or any(
                type(result.get(key)) is not int
                for key in (
                    "accepted_at_ns",
                    "acquired_admission_generation",
                    "claim_generation",
                )
            )
            or result["accepted_at_ns"] < 0
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
