"""ROS-free Composer scene-initialization handshake helpers."""

from __future__ import annotations

from typing import Any, Mapping


def deferred_program_load_payload(
    *,
    program_path: str,
    deployment_path: str,
) -> dict[str, Any]:
    return {
        "path": program_path,
        "deploymentPath": deployment_path,
        "forceDiscardOverrides": True,
        "deferInitialRuntimeReset": True,
    }


def scene_initialization_token_from_program_response(
    payload: Mapping[str, Any],
) -> str:
    initialization = payload.get("sceneInitialization")
    if not isinstance(initialization, Mapping):
        raise ValueError("Composer response has no sceneInitialization object")
    if initialization.get("state") != "closed":
        raise ValueError("Composer scene initialization barrier is not closed")
    token = str(initialization.get("token") or "").strip()
    if not token:
        raise ValueError("Composer response has no scene initialization token")
    if len(token) > 128:
        raise ValueError("Composer scene initialization token is too long")
    return token


def scene_initialization_reset_payload(token: str) -> dict[str, str]:
    normalized = str(token or "").strip()
    if not normalized:
        raise ValueError("scene initialization token is required")
    if len(normalized) > 128:
        raise ValueError("scene initialization token is too long")
    return {"sceneInitializationToken": normalized}
