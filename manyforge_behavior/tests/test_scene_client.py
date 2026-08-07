"""Thin Behavior logical-scene client contract."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.scene_client import SceneClient, SceneClientResponseError


class FakeTransport:
    def __init__(self) -> None:
        self.request: Mapping[str, Any] | None = None

    def scene(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "logical_scene": {},
        }

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.request = body
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": True,
            "projection_status": "committed",
            "committed_projection_revision": {"scene_revision": {}},
        }


def test_scene_client_forwards_without_owning_scene_state() -> None:
    transport = FakeTransport()
    client = SceneClient(transport)
    assert client.snapshot()["logical_scene"] == {}
    request = {"request_context": {"request_id": "request.1"}}
    assert client.commit(request)["projection_status"] == "committed"
    assert transport.request is request


def test_scene_client_rejects_malformed_owner_responses() -> None:
    class BadTransport(FakeTransport):
        def scene(self) -> dict[str, Any]:
            return {"schema_version": "legacy.scene"}

    with pytest.raises(SceneClientResponseError):
        SceneClient(BadTransport()).snapshot()

    class FailedTransport(FakeTransport):
        def commit_scene_transaction(
            self, body: Mapping[str, Any]
        ) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(SceneClientResponseError):
        SceneClient(FailedTransport()).commit({})
