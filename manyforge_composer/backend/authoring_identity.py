"""Small shared identities for Composer authoring operations and workspace state."""

from __future__ import annotations

import hashlib
import json
from typing import Any, Mapping


def canonical_digest(value: Mapping[str, Any]) -> str:
    """Return the established authoring digest without assigning it an owner."""

    encoded = json.dumps(
        dict(value),
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    return f"sha256:{hashlib.sha256(encoded).hexdigest()}"


__all__ = ["canonical_digest"]
