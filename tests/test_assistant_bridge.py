"""Tests for the ManyForge assistant-provider bridge package."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_assistant_bridge import bridge


def test_bridge_rejects_out_of_catalog_tool_calls(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    audit_log = tmp_path / "audit.jsonl"
    captured: dict[str, Any] = {}

    async def resolve_model() -> str:
        return "test-model"

    async def call_upstream(
        *,
        model: str,
        messages: list[dict[str, Any]],
        tools: list[dict[str, Any]],
        timeout_s: float,
    ) -> dict[str, Any]:
        captured["model"] = model
        captured["messages"] = messages
        captured["tools"] = tools
        captured["timeout_s"] = timeout_s
        return {
            "choices": [
                {
                    "message": {
                        "content": "I will call two tools.",
                        "tool_calls": [
                            {
                                "id": "call-allowed",
                                "type": "function",
                                "function": {
                                    "name": "inspect_isaac_scene",
                                    "arguments": json.dumps({"includeSubtrees": ["/World"]}),
                                },
                            },
                            {
                                "id": "call-hidden",
                                "type": "function",
                                "function": {
                                    "name": "delete_world",
                                    "arguments": json.dumps({"force": True}),
                                },
                            },
                        ],
                    }
                }
            ]
        }

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", audit_log)
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            catalog_tools=["inspect_isaac_scene"],
            tools=[
                {
                    "id": "inspect_isaac_scene",
                    "effect": "read_only",
                    "description": "Inspect the simulator scene.",
                },
                {
                    "id": "delete_world",
                    "effect": "runtime_mutating",
                    "description": "Out-of-mode tool that must not be visible.",
                },
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert payload["error"]["code"] == "response_rejected"
    assert "delete_world" in payload["error"]["detail"]
    assert payload["toolCalls"] == []
    assert payload["proposals"] == []
    assert payload["mutated"] is False
    assert payload["requiresReview"] is True

    assert captured["model"] == "test-model"
    assert captured["tools"] == [
        {
            "type": "function",
            "function": {
                "name": "inspect_isaac_scene",
                "description": "Inspect the simulator scene.",
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "additionalProperties": True,
                },
            },
        }
    ]

    audit_record = json.loads(audit_log.read_text(encoding="utf-8").splitlines()[-1])
    assert audit_record["exitReason"] == "response_rejected"
    assert audit_record["assistantMode"] == "composer-assistant"
    assert audit_record["outOfMode"][0]["name"] == "delete_world"
    assert audit_record["outOfMode"][0]["argumentsHash"]


def test_bridge_returns_descriptor_tool_calls_as_proposals(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    async def resolve_model() -> str:
        return "test-model"

    async def call_upstream(**_: Any) -> dict[str, Any]:
        return {
            "choices": [
                {
                    "message": {
                        "content": "I can update the tree.",
                        "tool_calls": [
                            {
                                "id": "call-allowed",
                                "type": "function",
                                "function": {
                                    "name": "tree_insert_node",
                                    "arguments": "{}",
                                },
                            }
                        ],
                    }
                }
            ]
        }

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", tmp_path / "audit.jsonl")
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            catalog_tools=["tree.insert_node"],
            tools=[
                {
                    "id": "tree.insert_node",
                    "effect": "proposal",
                    "description": "Propose a tree insertion.",
                }
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert "error" not in payload
    assert payload["message"] == "I can update the tree."
    assert [call["name"] for call in payload["toolCalls"]] == ["tree.insert_node"]
    assert payload["proposals"][0]["payload"]["toolName"] == "tree.insert_node"


def test_bridge_executes_read_only_tools_in_loop(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    audit_log = tmp_path / "audit.jsonl"
    upstream_messages: list[list[dict[str, Any]]] = []
    callbacks: list[dict[str, Any]] = []

    async def resolve_model() -> str:
        return "test-model"

    async def call_upstream(
        *,
        messages: list[dict[str, Any]],
        tools: list[dict[str, Any]],
        **_: Any,
    ) -> dict[str, Any]:
        upstream_messages.append(messages)
        assert tools[0]["function"]["name"] == "program_read"
        if len(upstream_messages) == 1:
            return {
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": "call-program",
                                    "type": "function",
                                    "function": {
                                        "name": "program_read",
                                        "arguments": "{}",
                                    },
                                }
                            ],
                        }
                    }
                ]
            }
        assert upstream_messages[-1][-1]["role"] == "tool"
        assert upstream_messages[-1][-1]["tool_call_id"] == "call-program"
        return {"choices": [{"message": {"content": "The program is loaded."}}]}

    async def execute_read_only_tool(**kwargs: Any) -> dict[str, Any]:
        callbacks.append(kwargs)
        return {"success": True, "result": {"programLoaded": True}, "mutated": False}

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", audit_log)
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)
    monkeypatch.setattr(bridge, "_execute_read_only_tool", execute_read_only_tool)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            request_id="req-read-loop",
            assistant_mode="query",
            catalog_tools=["program.read"],
            tools=[
                {
                    "id": "program.read",
                    "effect": "read_only",
                    "description": "Read the loaded program.",
                }
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert "error" not in payload
    assert payload["message"] == "The program is loaded."
    assert payload["proposals"] == []
    assert payload["toolCalls"][0]["name"] == "program.read"
    assert payload["toolCalls"][0]["status"] == "completed"
    assert payload["toolCalls"][0]["result"] == {"programLoaded": True}
    assert len(upstream_messages) == 2
    assert callbacks[0]["tool_id"] == "program.read"

    audit_record = json.loads(audit_log.read_text(encoding="utf-8").splitlines()[-1])
    assert audit_record["exitReason"] == "ok"
    assert audit_record["turnCount"] == 2


def test_bridge_cancel_endpoint_rejects_matching_request(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    async def resolve_model() -> str:
        return "test-model"

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", tmp_path / "audit.jsonl")
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    bridge._CANCELLED_REQUESTS.discard("req-cancel")
    client = TestClient(bridge.app)

    cancel_response = client.post("/v1/manyforge/assistant/req-cancel/cancel")
    assert cancel_response.status_code == 200
    assert cancel_response.json()["cancelled"] is True

    response = client.post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(request_id="req-cancel"),
    )

    assert response.status_code == 200
    payload = response.json()
    assert payload["error"]["code"] == "cancelled"


def _bridge_payload(
    *,
    request_id: str = "req-test",
    assistant_mode: str = "composer-assistant",
    catalog_tools: list[str] | None = None,
    tools: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    catalog_tools = catalog_tools or ["inspect_isaac_scene"]
    return {
        "version": "manyforge.assistant.provider_request.v0",
        "schemaVersion": "0.1.0",
        "requestId": request_id,
        "providerId": "nemoclaw",
        "conversationId": "conv-test",
        "assistantMode": assistant_mode,
        "message": "Inspect the scene.",
        "requestedTools": list(catalog_tools),
        "context": {},
        "runtime": {"programLoaded": False, "cycleState": "idle"},
        "skills": [],
        "nodes": [],
        "catalog": {
            "skills": [],
            "tools": list(catalog_tools),
            "nodes": [],
        },
        "tools": tools
        or [
            {
                "id": "inspect_isaac_scene",
                "effect": "read_only",
                "description": "Inspect the simulator scene.",
            }
        ],
        "constraints": {
            "mutatesState": False,
            "requiresReview": True,
            "proposalStatus": "draft",
            "allowedToolCallStatuses": ["proposed", "skipped", "completed", "failed"],
        },
    }
