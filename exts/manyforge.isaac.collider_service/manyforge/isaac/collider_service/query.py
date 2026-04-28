"""Validated JSON query contract for the Isaac collider service."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any


QUERY_VERSION = "manyforge.isaac.collider_query.v0.1"


@dataclass(frozen=True)
class ColliderQuery:
    request_id: str | None
    include_subtrees: list[str]
    exclude_subtrees: list[str]
    probe_prims: list[str]
    include_hidden: bool
    include_cooked_convex: bool
    include_cooked_samples: bool
    output_profile: str
    max_colliders: int | None
    purpose: list[str]


def parse_query(text: str) -> ColliderQuery:
    try:
        raw = json.loads(text or "{}")
    except json.JSONDecodeError as exc:
        raise ValueError(f"Query must be JSON: {exc}") from exc
    if not isinstance(raw, dict):
        raise ValueError("Query JSON must be an object")

    version = raw.get("version", QUERY_VERSION)
    if version != QUERY_VERSION:
        raise ValueError(f"Unsupported query version: {version!r}")

    return ColliderQuery(
        request_id=_optional_string(raw.get("request_id")),
        include_subtrees=_string_list(
            raw.get("include_subtrees", raw.get("include_subtree", []))
        ),
        exclude_subtrees=_string_list(
            raw.get("exclude_subtrees", raw.get("exclude_subtree", []))
        ),
        probe_prims=_string_list(raw.get("probe_prims", raw.get("probe_prim", []))),
        include_hidden=_bool_value(raw.get("include_hidden", False), "include_hidden"),
        include_cooked_convex=_bool_value(
            raw.get("include_cooked_convex", True),
            "include_cooked_convex",
        ),
        include_cooked_samples=_bool_value(
            raw.get("include_cooked_samples", False),
            "include_cooked_samples",
        ),
        output_profile=_output_profile(raw.get("output_profile", "llm_scene_authoring")),
        max_colliders=_optional_int(raw.get("max_colliders")),
        purpose=_purpose_list(raw.get("purpose", [])),
    )


def request_id_from_text(text: str) -> str | None:
    try:
        raw = json.loads(text or "{}")
    except Exception:
        return None
    return _optional_string(raw.get("request_id")) if isinstance(raw, dict) else None


def _string_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        invalid = [item for item in value if not isinstance(item, str)]
        if invalid:
            raise ValueError(
                "Expected list of strings, got invalid item type(s): "
                f"{[type(item).__name__ for item in invalid]}"
            )
        return list(value)
    raise ValueError(f"Expected string or list of strings, got {type(value).__name__}")


def _purpose_list(value: Any) -> list[str]:
    valid = {"default", "render", "proxy", "guide", "all"}
    purposes = _string_list(value)
    invalid = [item for item in purposes if item not in valid]
    if invalid:
        raise ValueError(f"Invalid purpose value(s): {invalid}")
    return purposes


def _output_profile(value: Any) -> str:
    if value not in {"raw_inventory", "llm_scene_authoring"}:
        raise ValueError(f"Unsupported output_profile: {value!r}")
    return str(value)


def _optional_string(value: Any) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError("request_id must be a string")
    return value


def _bool_value(value: Any, field_name: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{field_name} must be a boolean")
    return value


def _optional_int(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("max_colliders must be an integer")
    if value <= 0:
        raise ValueError("max_colliders must be > 0")
    return value
