"""Explicit direct references for Program v3 test fixtures.

These builders create fresh authored identities; they do not convert legacy
Programs or stand in for a native-sealed Program Binding artifact.
"""

from __future__ import annotations

from typing import Any


def target(component_id: str, command_target_id: str) -> dict[str, str]:
    return {"component_id": component_id, "command_target_id": command_target_id}


def frame(component_id: str, frame_id: str) -> dict[str, str]:
    return {"component_id": component_id, "frame_id": frame_id}


def signal(component_id: str, signal_name: str) -> dict[str, str]:
    return {"component_id": component_id, "signal_name": signal_name}


def target_binding(
    component_id: str, command_target_id: str, capabilities: list[dict[str, Any]],
    **extra: Any,
) -> dict[str, Any]:
    return {
        "target": target(component_id, command_target_id),
        "capability_bindings": capabilities,
        **extra,
    }


def program_document(catalog: Any, tree: dict[str, Any], *, program_id: str, name: str) -> dict[str, Any]:
    return {
        "api_version": "manyforge.program.v3",
        "program_id": program_id,
        "program_profile": "composable-cell-v1",
        "name": name,
        "catalog": {"api_version": catalog.api_version, "content_version": catalog.content_version},
        "tree": tree,
    }
