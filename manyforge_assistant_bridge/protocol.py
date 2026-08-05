"""Pure catalog-identity helper retained for the H0 assistant-client port."""
from __future__ import annotations

import hashlib
import json
from typing import Any, Final


MAX_CATALOG_FAMILY_ITEMS: Final = 512
MAX_CATALOG_IDENTIFIER_CHARS: Final = 256
MAX_CATALOG_JSON_BYTES: Final = 4 * 1024 * 1024
CATALOG_FAMILIES: Final = ("skills", "tools", "nodes")


def compute_catalog_hash(catalog: dict[str, list[str]], mode: str) -> str:
    """Deterministic identity of a resolved catalog and active mode.

    This is not an authenticity or tamper-detection primitive by itself. A
    future bridge must compare it with an independently trusted expected hash;
    recomputing it only from an untrusted envelope proves nothing about which
    tools were authorized.

    The input is an already-resolved set of visible identifiers. The H0 read
    client obtains owner-issued state independently; a future N provider must
    bind this hash to separately trusted catalog state. Callers must never
    derive both the catalog and the value used to authenticate that catalog
    from the same untrusted request.
    """
    normalized_mode = _bounded_identifier(mode, label="mode")
    if type(catalog) is not dict:
        raise TypeError("catalog must be an object")
    if len(catalog) > len(CATALOG_FAMILIES):
        raise ValueError("catalog contains too many families")
    unknown_families: list[str] = []
    for family in catalog:
        if type(family) is not str:
            raise TypeError("catalog family names must be strings")
        if family not in CATALOG_FAMILIES:
            unknown_families.append(family)
    if unknown_families:
        raise ValueError(
            "catalog contains unsupported families: "
            + ", ".join(sorted(unknown_families))
        )
    payload: dict[str, Any] = {"mode": normalized_mode}
    for family in CATALOG_FAMILIES:
        raw_values = catalog.get(family, [])
        if type(raw_values) is not list:
            raise TypeError(f"catalog.{family} must be a list")
        if len(raw_values) > MAX_CATALOG_FAMILY_ITEMS:
            raise ValueError(
                f"catalog.{family} exceeds the "
                f"{MAX_CATALOG_FAMILY_ITEMS}-item bound"
            )
        values = [
            _bounded_identifier(value, label=f"catalog.{family}[{index}]")
            for index, value in enumerate(raw_values)
        ]
        if len(set(values)) != len(values):
            raise ValueError(f"catalog.{family} contains duplicate identifiers")
        payload[family] = sorted(values)
    blob = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    if len(blob) > MAX_CATALOG_JSON_BYTES:
        raise ValueError(
            f"catalog exceeds the {MAX_CATALOG_JSON_BYTES}-byte bound"
        )
    return hashlib.sha256(blob).hexdigest()


def _bounded_identifier(value: Any, *, label: str) -> str:
    if type(value) is not str or not value:
        raise ValueError(f"{label} must be a non-empty string")
    if value != value.strip():
        raise ValueError(f"{label} must not contain surrounding whitespace")
    if len(value) > MAX_CATALOG_IDENTIFIER_CHARS:
        raise ValueError(
            f"{label} exceeds the {MAX_CATALOG_IDENTIFIER_CHARS}-character bound"
        )
    return value
