"""Bounded worker-envelope primitives and the fenced I9 worker factory.

The old visual snapshot builder, versioned Composer worker protocol, and
process supervisor are absent. Generic envelope integrity and CPU-set parsing
remain as port inputs; creating a visual worker is explicitly unavailable.
"""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any, Final, NoReturn


_DEFAULT_MAX_INPUT_BYTES: Final = 8 * 1024 * 1024
_DEFAULT_MAX_OUTPUT_BYTES: Final = 16 * 1024 * 1024
_SCHEMA_TOKEN: Final = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")
VIEWER_WORKER_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_viewer_worker_unavailable"
)
VIEWER_WORKER_RESTORATION_GATE: Final = "I9"


class ViewerWorkerError(RuntimeError):
    """A bounded worker-envelope operation failed."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(detail or reason)
        self.reason = reason
        self.detail = detail or reason


class ViewerWorkerUnavailableError(ViewerWorkerError):
    """The I9 observer-owned visual worker has not been connected."""


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")


def _encode_envelope(
    *,
    schema: str,
    request_id: int,
    payload: dict[str, Any],
    max_bytes: int,
) -> dict[str, Any]:
    """Encode one caller-named, payload-neutral bounded envelope."""
    if _SCHEMA_TOKEN.fullmatch(schema) is None:
        raise ViewerWorkerError("worker_envelope_invalid", "invalid schema token")
    if isinstance(request_id, bool) or request_id < 0:
        raise ViewerWorkerError("worker_envelope_invalid", "invalid request id")
    if max_bytes <= 0:
        raise ViewerWorkerError("worker_envelope_invalid", "invalid byte limit")
    encoded = _canonical_json_bytes(payload)
    if len(encoded) > max_bytes:
        raise ViewerWorkerError(
            "worker_envelope_payload_too_large",
            f"payload is {len(encoded)} bytes; limit is {max_bytes}",
        )
    return {
        "schema": schema,
        "request_id": int(request_id),
        "payload_length": len(encoded),
        "payload_sha256": hashlib.sha256(encoded).hexdigest(),
        "payload": encoded,
    }


def _decode_envelope(
    envelope: Any,
    *,
    expected_schema: str,
    max_bytes: int,
) -> tuple[int, dict[str, Any]]:
    """Verify identity, bounds, digest, UTF-8, JSON, and object root."""
    if _SCHEMA_TOKEN.fullmatch(expected_schema) is None:
        raise ViewerWorkerError("worker_envelope_invalid", "invalid schema token")
    if not isinstance(envelope, dict) or envelope.get("schema") != expected_schema:
        raise ViewerWorkerError("worker_envelope_protocol_error", "unexpected schema")
    request_id = envelope.get("request_id")
    payload = envelope.get("payload")
    payload_length = envelope.get("payload_length")
    digest = envelope.get("payload_sha256")
    if isinstance(request_id, bool) or not isinstance(request_id, int) or request_id < 0:
        raise ViewerWorkerError("worker_envelope_protocol_error", "invalid request id")
    if not isinstance(payload, bytes):
        raise ViewerWorkerError("worker_envelope_protocol_error", "payload is not bytes")
    if max_bytes <= 0 or len(payload) > max_bytes or payload_length != len(payload):
        raise ViewerWorkerError("worker_envelope_protocol_error", "invalid payload length")
    if not isinstance(digest, str) or hashlib.sha256(payload).hexdigest() != digest:
        raise ViewerWorkerError("worker_envelope_protocol_error", "checksum mismatch")
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ViewerWorkerError("worker_envelope_protocol_error", str(exc)) from exc
    if not isinstance(decoded, dict):
        raise ViewerWorkerError("worker_envelope_protocol_error", "root is not an object")
    return request_id, decoded


def _parse_cpu_set(raw: str) -> set[int]:
    """Parse a bounded Linux-style CPU list without touching process policy."""
    cpus: set[int] = set()
    for token in raw.split(","):
        part = token.strip()
        if not part:
            continue
        if "-" in part:
            first, last = part.split("-", 1)
            start = int(first)
            end = int(last)
            if start < 0 or end < start or end - start > 4096:
                raise ValueError(part)
            cpus.update(range(start, end + 1))
        else:
            value = int(part)
            if value < 0:
                raise ValueError(part)
            cpus.add(value)
        if len(cpus) > 4096:
            raise ValueError("CPU set exceeds 4096 entries")
    return cpus


def require_viewer_worker() -> NoReturn:
    raise ViewerWorkerUnavailableError(
        VIEWER_WORKER_UNAVAILABLE_REASON_CODE,
        f"{VIEWER_WORKER_UNAVAILABLE_REASON_CODE}: visual worker returns at I9 "
        "after binding to the observer-owned snapshot contract",
    )


class ViewerWorkerSupervisor:
    """Stable import marker; construction performs no process or queue effect."""

    def __init__(
        self,
        *,
        timeout_s: float | None = None,
        max_input_bytes: int = _DEFAULT_MAX_INPUT_BYTES,
        max_output_bytes: int = _DEFAULT_MAX_OUTPUT_BYTES,
        worker_target: Any | None = None,
    ) -> None:
        del timeout_s, max_input_bytes, max_output_bytes, worker_target
        require_viewer_worker()


__all__ = [
    "ViewerWorkerError",
    "ViewerWorkerSupervisor",
    "ViewerWorkerUnavailableError",
    "require_viewer_worker",
]
