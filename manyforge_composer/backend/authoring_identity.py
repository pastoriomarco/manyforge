"""Small shared identities for Composer authoring operations and workspace state."""

from __future__ import annotations

import hashlib
import json
from types import MappingProxyType
from typing import Any, Mapping


def freeze_json(value: Any) -> Any:
    """Return an immutable copy of one already-validated JSON value."""

    if type(value) is dict:
        return MappingProxyType(
            {key: freeze_json(item) for key, item in value.items()}
        )
    if type(value) is list:
        return tuple(freeze_json(item) for item in value)
    if type(value) in {str, int, float, bool, type(None)}:
        return value
    raise TypeError("authoring value is outside the exact JSON subset")


def thaw_json(value: Any) -> Any:
    """Return a mutable JSON copy of a value produced by :func:`freeze_json`."""

    if isinstance(value, Mapping):
        return {key: thaw_json(item) for key, item in value.items()}
    if type(value) is tuple:
        return [thaw_json(item) for item in value]
    if type(value) in {str, int, float, bool, type(None)}:
        return value
    raise TypeError("frozen authoring value is outside the exact JSON subset")


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


__all__ = ["canonical_digest", "freeze_json", "thaw_json"]
