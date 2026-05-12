"""Validated JSON query contract for the Isaac collider service."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any


QUERY_VERSION = "manyforge.isaac.collider_query.v0.1"
MESH_TRANSFER_REQUEST_VERSION = "manyforge.isaac.mesh_transfer_request.v0.1"


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
    mesh_export_dir: str | None
    mesh_source_preference: str


@dataclass(frozen=True)
class MeshTransferRequest:
    request_id: str | None
    mesh_resource_uri: str | None
    source_root: str | None
    mesh_export_dir: str | None
    mesh_source_preference: str
    include_hidden: bool
    purpose: list[str]
    chunk_size: int


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
        request_id=_optional_string(raw.get("request_id"), "request_id"),
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
        mesh_export_dir=_optional_string(
            raw.get("mesh_export_dir", raw.get("meshExportDir")),
            "mesh_export_dir",
        ),
        mesh_source_preference=_mesh_source_preference(
            raw.get("mesh_source_preference", raw.get("meshSourcePreference", "auto"))
        ),
    )


def request_id_from_text(text: str) -> str | None:
    try:
        raw = json.loads(text or "{}")
    except Exception:
        return None
    return _optional_string(raw.get("request_id"), "request_id") if isinstance(raw, dict) else None


def parse_mesh_transfer_request(text: str) -> MeshTransferRequest:
    try:
        raw = json.loads(text or "{}")
    except json.JSONDecodeError as exc:
        raise ValueError(f"Mesh transfer request must be JSON: {exc}") from exc
    if not isinstance(raw, dict):
        raise ValueError("Mesh transfer request JSON must be an object")

    version = raw.get("version", MESH_TRANSFER_REQUEST_VERSION)
    if version != MESH_TRANSFER_REQUEST_VERSION:
        raise ValueError(f"Unsupported mesh transfer request version: {version!r}")

    mesh_resource_uri = _optional_string(
        raw.get("mesh_resource_uri", raw.get("meshResourceUri")),
        "mesh_resource_uri",
    )
    source_root = _optional_string(
        raw.get("source_root", raw.get("sourceRoot")),
        "source_root",
    )
    if bool(mesh_resource_uri) == bool(source_root):
        raise ValueError(
            "Mesh transfer request must provide exactly one of "
            "mesh_resource_uri or source_root"
        )

    return MeshTransferRequest(
        request_id=_optional_string(raw.get("request_id"), "request_id"),
        mesh_resource_uri=mesh_resource_uri,
        source_root=source_root,
        mesh_export_dir=_optional_string(
            raw.get("mesh_export_dir", raw.get("meshExportDir")),
            "mesh_export_dir",
        ),
        mesh_source_preference=_mesh_source_preference(
            raw.get("mesh_source_preference", raw.get("meshSourcePreference", "auto"))
        ),
        include_hidden=_bool_value(raw.get("include_hidden", True), "include_hidden"),
        purpose=_purpose_list(raw.get("purpose", ["all"])),
        chunk_size=_chunk_size(raw.get("chunk_size", raw.get("chunkSize", 262144))),
    )


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


def _optional_string(value: Any, field_name: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError(f"{field_name} must be a string")
    return value


def _mesh_source_preference(value: Any) -> str:
    if not isinstance(value, str):
        raise ValueError(
            f"mesh_source_preference must be a string, got {type(value).__name__}"
        )
    if value not in {"auto", "original", "collider", "primitive"}:
        raise ValueError(
            "mesh_source_preference must be one of: auto, original, collider, primitive"
        )
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


def _chunk_size(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("chunk_size must be an integer")
    if value < 4096:
        raise ValueError("chunk_size must be >= 4096")
    if value > 4 * 1024 * 1024:
        raise ValueError("chunk_size must be <= 4194304")
    return value
