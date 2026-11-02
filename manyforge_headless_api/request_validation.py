"""The one request-validation refusal shared by every HTTP surface.

The same shape the Manager RPC returns for a command that does not decode:
code `invalid_request`, and `details` mapping each refused field path to that
field's own code (pydantic's error type over HTTP, for example `missing` or
`string_type`).
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from typing import Any

REQUEST_INVALID_CODE = "invalid_request"
# A refusal names at most this many fields; the rest are counted in the message.
MAX_REFUSED_FIELDS = 32


def request_validation_detail(
    errors: Iterable[Mapping[str, Any]],
    *,
    producer: str,
    request_id: str | None,
) -> dict[str, Any]:
    """The `detail` of one request-validation refusal."""

    details: dict[str, str] = {}
    messages: list[str] = []
    total = 0
    for error in errors:
        total += 1
        path = ".".join(str(part) for part in error.get("loc", ())) or "request"
        if path not in details and len(details) < MAX_REFUSED_FIELDS:
            details[path] = str(error.get("type") or "invalid")
            messages.append(f"{path}: {error.get('msg') or 'invalid'}")
    message = "; ".join(messages) or "request is invalid"
    if total > len(messages):
        message += f" (and {total - len(messages)} more)"
    return {
        "code": REQUEST_INVALID_CODE,
        "details": details,
        "message": message,
        "producer": producer,
        "request_id": request_id,
    }
