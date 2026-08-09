from __future__ import annotations

from copy import deepcopy
from types import SimpleNamespace
from typing import Any

from fastapi.testclient import TestClient

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState


class RecordingProvider:
    def __init__(self) -> None:
        self.submissions: list[dict[str, Any]] = []
        self.status_reads: list[str] = []
        self.cancellations: list[str] = []

    @staticmethod
    def _status(request_id: str, state: str = "running") -> dict[str, Any]:
        return {
            "schemaVersion": "manyforge.assistant.provider_status.v1",
            "requestId": request_id,
            "requestOwnerEpoch": "page.1",
            "sourceLabel": "composer_ui",
            "correlationId": None,
            "conversationId": "conversation.1",
            "assistantMode": "composer-assistant",
            "state": state,
            "response": None,
            "error": None,
        }

    def submit(self, body) -> dict[str, Any]:
        self.submissions.append(deepcopy(dict(body)))
        return self._status(body["requestId"])

    def status(self, request_id: str) -> dict[str, Any]:
        self.status_reads.append(request_id)
        return self._status(request_id)

    def cancel(self, request_id: str) -> dict[str, Any]:
        self.cancellations.append(request_id)
        return self._status(request_id, "cancelled")


def _mode() -> dict[str, Any]:
    return {
        "mode": "composer-assistant",
        "grant_policy": {
            "approval_mode": None,
            "review_policy": "required",
            "session_kind": None,
        },
        "nodes": [],
        "skills": [],
        "tools": [
            {
                "tool_id": "program_read",
                "description": "Read Draft",
                "effect": "read_only",
                "input_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {},
                },
                "input_schema_hash": "sha256:" + "1" * 64,
            }
        ],
        "assistant_projection_digest": "sha256:" + "2" * 64,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "source": {"source_content_digest": "sha256:" + "3" * 64},
    }


def test_composer_forwards_exact_owner_context_without_owning_request_state(
    monkeypatch,
) -> None:
    from manyforge_composer.backend import routes_assistant

    provider = RecordingProvider()
    state = ComposerState(
        headless_client=object(),  # type: ignore[arg-type]
        assistant_client=provider,  # type: ignore[arg-type]
    )
    node = SimpleNamespace(
        payload={
            "environment_digest": "sha256:" + "4" * 64,
            "source": {"source_content_digest": "sha256:" + "5" * 64},
        }
    )
    source = SimpleNamespace(
        payload={"source": {"source_content_digest": "sha256:" + "6" * 64}}
    )
    mode = SimpleNamespace(payload=_mode())
    monkeypatch.setattr(routes_assistant, "read_active_node_environment", lambda _state: node)
    monkeypatch.setattr(
        routes_assistant,
        "read_active_source_model",
        lambda _state, *, node_environment: source,
    )
    monkeypatch.setattr(
        routes_assistant,
        "read_active_assistant_mode",
        lambda _state, *, mode, node_environment: SimpleNamespace(payload=_mode()),
    )

    with TestClient(create_app(state)) as client:
        accepted = client.post(
            "/api/assistant/requests",
            json={
                "requestId": "request.1",
                "requestOwnerEpoch": "page.1",
                "sourceLabel": "composer_ui",
                "correlationId": None,
                "conversationId": "conversation.1",
                "assistantMode": "composer-assistant",
                "message": "Read the Draft",
                "requestedTools": [],
                "fieldPolicy": {},
            },
        )
        status = client.get("/api/assistant/requests/request.1")
        cancelled = client.post("/api/assistant/requests/request.1/cancel")

    assert accepted.status_code == 202
    assert status.status_code == 200
    assert cancelled.json()["state"] == "cancelled"
    assert provider.status_reads == ["request.1"]
    assert provider.cancellations == ["request.1"]
    forwarded = provider.submissions[0]
    assert forwarded["schemaVersion"] == "manyforge.assistant.provider_request.v1"
    assert forwarded["modeProjection"] == mode.payload
    assert forwarded["composerDraft"]["draftId"] == state.draft_session.draft_id
    assert forwarded["composerDraft"]["programLoaded"] is False
    assert set(forwarded["resolvedCatalog"]) == {
        "catalogHash",
        "grantPolicy",
        "nodes",
        "skills",
        "tools",
    }
