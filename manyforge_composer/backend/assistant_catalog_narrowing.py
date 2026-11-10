"""Narrow one verified assistant mode without becoming a catalog owner."""

from __future__ import annotations

from copy import deepcopy
import re
from typing import Any, Final, Mapping, Sequence
import unicodedata

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    raw_sha256_digest,
)


_CATALOG_VERSION: Final = "manyforge.assistant_mode_catalog.v1"
ORIENTATION_FIELDS: Final = frozenset(
    {
        "xyzw",
        "rpy_deg",
        "rotate_local_rpy_deg",
        "rotate_world_rpy_deg",
        "rpy_delta_deg",
        "orientation_from",
    }
)
_TOOL_ID_PATTERN: Final = re.compile(r"^[a-z][a-z0-9_]*$")


class AssistantCatalogNarrowingError(ValueError):
    pass


def normalize_field_policy(value: Any) -> dict[str, Any]:
    if value in (None, {}):
        return {}
    if type(value) is not dict or set(value) != {"allowedOrientationFields"}:
        raise AssistantCatalogNarrowingError("fieldPolicy has an unsupported shape")
    raw = value["allowedOrientationFields"]
    if type(raw) is not list or not raw or len(raw) > len(ORIENTATION_FIELDS):
        raise AssistantCatalogNarrowingError(
            "allowedOrientationFields must be a bounded non-empty array"
        )
    if (
        any(type(item) is not str for item in raw)
        or raw != sorted(set(raw))
        or not set(raw) <= ORIENTATION_FIELDS
    ):
        raise AssistantCatalogNarrowingError(
            "allowedOrientationFields is not canonical or supported"
        )
    return {"allowedOrientationFields": list(raw)}


def normalize_requested_tools(value: Any) -> list[str]:
    if value is None:
        return []
    if type(value) is not list or len(value) > 4_096:
        raise AssistantCatalogNarrowingError("requestedTools must be a bounded array")
    if (
        any(
            type(item) is not str
            or not item
            or item != item.strip()
            or unicodedata.normalize("NFC", item) != item
            or len(item.encode("utf-8")) > 256
            or _TOOL_ID_PATTERN.fullmatch(item) is None
            for item in value
        )
        or value != sorted(set(value))
    ):
        raise AssistantCatalogNarrowingError(
            "requestedTools must contain sorted unique canonical IDs"
        )
    return list(value)


def _framed_digest(domain: str, version: str, value: Any) -> str:
    preimage = b"\0".join(
        (
            CONTENT_IDENTITY_PREFIX,
            domain.encode("ascii"),
            version.encode("ascii"),
            canonical_json_bytes(value),
        )
    )
    return raw_sha256_digest(preimage)


def narrow_schema(schema: dict[str, Any], allowed: set[str]) -> dict[str, Any]:
    disallowed = set(ORIENTATION_FIELDS) - allowed
    if not ({"xyzw", "rpy_deg"} & allowed):
        disallowed.add("orientation")

    def impossible(value: Any) -> bool:
        required = value.get("required") if type(value) is dict else None
        return type(required) is list and any(item in disallowed for item in required)

    def narrow(value: Any) -> Any:
        if type(value) is list:
            return [narrow(item) for item in value if not impossible(item)]
        if type(value) is not dict:
            return deepcopy(value)
        result: dict[str, Any] = {}
        for key, item in value.items():
            if key == "properties" and type(item) is dict:
                result[key] = {
                    name: narrow(child)
                    for name, child in item.items()
                    if name not in disallowed
                }
            elif key == "required" and type(item) is list:
                result[key] = [name for name in item if name not in disallowed]
            else:
                result[key] = narrow(item)
        return result

    return narrow(schema)


def resolve_assistant_catalog(
    mode_projection: Mapping[str, Any],
    *,
    requested_tools: Sequence[str] | None,
    field_policy: Mapping[str, Any] | None,
) -> dict[str, Any]:
    requested = normalize_requested_tools(
        list(requested_tools) if requested_tools is not None else []
    )
    policy = normalize_field_policy(dict(field_policy) if field_policy is not None else {})
    source_tools = mode_projection.get("tools")
    if type(source_tools) is not list:
        raise AssistantCatalogNarrowingError("verified mode has no tool array")
    available = {row["tool_id"] for row in source_tools}
    selected = set(requested) if requested else available
    if selected - available:
        raise AssistantCatalogNarrowingError(
            "requestedTools contains tools outside the selected mode"
        )
    allowed = set(policy.get("allowedOrientationFields", ORIENTATION_FIELDS))
    tools: list[dict[str, Any]] = []
    for source in source_tools:
        if source["tool_id"] not in selected:
            continue
        tool = deepcopy(source)
        if policy:
            tool["input_schema"] = narrow_schema(tool["input_schema"], allowed)
            tool["input_schema_hash"] = raw_sha256_digest(
                canonical_json_bytes(tool["input_schema"])
            )
        tools.append(tool)
    catalog = {
        "grant_policy": deepcopy(mode_projection["grant_policy"]),
        "mode": mode_projection["mode"],
        "nodes": deepcopy(mode_projection["nodes"]),
        "skills": deepcopy(mode_projection["skills"]),
        "tools": tools,
    }
    return {
        "catalogHash": _framed_digest(
            "assistant-mode-catalog", _CATALOG_VERSION, catalog
        ),
        "grantPolicy": catalog["grant_policy"],
        "nodes": catalog["nodes"],
        "skills": catalog["skills"],
        "tools": tools,
    }


__all__ = [
    "AssistantCatalogNarrowingError",
    "ORIENTATION_FIELDS",
    "narrow_schema",
    "normalize_field_policy",
    "normalize_requested_tools",
    "resolve_assistant_catalog",
]
