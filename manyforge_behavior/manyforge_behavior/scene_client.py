"""Strict stateless Behavior client for the logical-scene authority."""

from __future__ import annotations

from typing import Any, Mapping, Protocol


class HeadlessSceneTransport(Protocol):
    """The bounded logical-scene HTTP calls used by Behavior."""

    def scene(self) -> dict[str, Any]: ...

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...


class SceneClientResponseError(RuntimeError):
    """Raised when a nominally successful transport response is malformed."""


class SceneClient:
    """Forward scene reads/intents without caching or reducing owner truth."""

    def __init__(self, transport: HeadlessSceneTransport) -> None:
        for method in ("scene", "commit_scene_transaction"):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"scene transport has no callable {method}")
        self._transport = transport

    def snapshot(self) -> dict[str, Any]:
        value = self._transport.scene()
        if (
            not isinstance(value, dict)
            or value.get("schema_version") != "manyforge.scene_projection.v1"
        ):
            raise SceneClientResponseError(
                "headless scene read returned an unsupported projection"
            )
        return value

    def commit(self, request: Mapping[str, Any]) -> dict[str, Any]:
        value = self._transport.commit_scene_transaction(request)
        if (
            not isinstance(value, dict)
            or value.get("api_version")
            != "manyforge.manager_command_response.v1"
            or value.get("ok") is not True
            or value.get("projection_status") != "committed"
            or value.get("committed_projection_revision") is None
        ):
            raise SceneClientResponseError(
                "headless scene commit did not return a committed owner result"
            )
        return value


__all__ = [
    "HeadlessSceneTransport",
    "SceneClient",
    "SceneClientResponseError",
]
