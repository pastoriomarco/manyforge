"""Tests for the ManyForge assistant-provider bridge package."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_assistant_bridge import bridge


def test_bridge_returns_out_of_catalog_tool_calls_as_review_proposals(
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
        tool_choice: Any = "auto",
    ) -> dict[str, Any]:
        captured["model"] = model
        captured["messages"] = messages
        captured["tools"] = tools
        captured["timeout_s"] = timeout_s
        captured["tool_choice"] = tool_choice
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
                        "description": "Descriptor-only tool that must not execute.",
                    },
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert "error" not in payload
    assert payload["toolCalls"] == [
        {
            "name": "delete_world",
            "arguments": {"force": True},
            "status": "proposed",
        }
    ]
    assert payload["proposals"] == [
        {
            "id": "call-hidden",
            "type": "tool_call_proposal",
            "title": "call delete_world",
            "summary": "LLM proposes calling delete_world with the supplied arguments.",
            "payload": {
                "toolCallId": "call-hidden",
                "toolName": "delete_world",
                "arguments": {"force": True},
            },
        }
    ]
    assert payload["mutated"] is False
    assert payload["requiresReview"] is True
    assert payload["draftMutated"] is False
    assert payload["warnings"] == [
        "model returned callback-safe calls together with proposal/runtime "
        "descriptors; descriptor response ended the loop"
    ]

    assert captured["model"] == "test-model"
    assert [tool["function"]["name"] for tool in captured["tools"]] == [
        "inspect_isaac_scene",
        "delete_world",
    ]
    assert captured["tools"][0]["function"] == {
        "name": "inspect_isaac_scene",
        "description": "Inspect the simulator scene.",
        "parameters": {
            "type": "object",
            "properties": {},
            "additionalProperties": True,
        },
    }
    assert captured["tools"][1]["function"] == {
        "name": "delete_world",
        "description": "Descriptor-only tool that must not execute.",
        "parameters": {
            "type": "object",
            "properties": {},
            "additionalProperties": True,
        },
    }

    audit_record = json.loads(audit_log.read_text(encoding="utf-8").splitlines()[-1])
    assert audit_record["exitReason"] == "proposal_returned"
    assert audit_record["assistantMode"] == "composer-assistant"
    assert audit_record["responseShape"] == {
        "toolCallNames": ["delete_world"],
        "proposalCount": 1,
        "warnings": 1,
        "draftMutated": False,
    }


def test_bridge_forwards_tool_input_schema_to_upstream(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    captured: dict[str, Any] = {}
    input_schema = {
        "type": "object",
        "required": ["sceneResource"],
        "properties": {
            "sceneResource": {
                "type": "object",
                "required": ["objectId"],
                "properties": {"objectId": {"type": "string"}},
                "additionalProperties": True,
            }
        },
        "additionalProperties": False,
    }

    async def resolve_model() -> str:
        return "test-model"

    async def call_upstream(
        *,
        tools: list[dict[str, Any]],
        **_: Any,
    ) -> dict[str, Any]:
        captured["tools"] = tools
        return {"choices": [{"message": {"content": "No change needed."}}]}

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", tmp_path / "audit.jsonl")
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            catalog_tools=["scene_draft_add_object"],
            tools=[
                {
                    "id": "scene_draft_add_object",
                    "effect": "composer_draft_mutating",
                    "description": "Add an object to the Composer draft scene.",
                    "inputSchema": input_schema,
                }
            ],
        ),
    )

    assert response.status_code == 200
    assert captured["tools"][0]["function"]["name"] == "scene_draft_add_object"
    assert captured["tools"][0]["function"]["parameters"] == input_schema


def test_bridge_success_envelope_always_includes_message(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    async def resolve_model() -> str:
        return "test-model"

    async def call_upstream(**_: Any) -> dict[str, Any]:
        return {"choices": [{"message": {"content": None, "tool_calls": []}}]}

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", tmp_path / "audit.jsonl")
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(),
    )

    assert response.status_code == 200
    assert response.json()["message"] == "Assistant completed without a text response."


def test_bridge_parses_well_formed_content_tool_call_in_loop(
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
        **_: Any,
    ) -> dict[str, Any]:
        upstream_messages.append(messages)
        if len(upstream_messages) == 1:
            return {
                "choices": [
                    {
                        "message": {
                            "content": (
                                '<tool_call>{"name":"program_read",'
                                '"arguments":{}}</tool_call>'
                            ),
                        }
                    }
                ]
            }
        assert upstream_messages[-1][-2]["role"] == "assistant"
        assert upstream_messages[-1][-2]["content"] == ""
        assert upstream_messages[-1][-2]["tool_calls"][0]["function"]["name"] == "program_read"
        assert upstream_messages[-1][-1]["role"] == "tool"
        return {"choices": [{"message": {"content": "The program is loaded."}}]}

    async def execute_callback_tool(**kwargs: Any) -> dict[str, Any]:
        callbacks.append(kwargs)
        return {"success": True, "result": {"programLoaded": True}, "mutated": False}

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", audit_log)
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)
    monkeypatch.setattr(bridge, "_execute_callback_tool", execute_callback_tool)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            request_id="req-content-tool-call",
            assistant_mode="query",
            catalog_tools=["program_read"],
            tools=[
                {
                    "id": "program_read",
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
    assert payload["toolCalls"][0]["name"] == "program_read"
    assert callbacks[0]["tool_id"] == "program_read"
    assert callbacks[0]["arguments"] == {}

    audit_records = [
        json.loads(line) for line in audit_log.read_text(encoding="utf-8").splitlines()
    ]
    parsed_records = [r for r in audit_records if r.get("contentToolCallsParsed")]
    assert parsed_records
    assert parsed_records[0]["contentToolCallFormats"] == ["tool_call_json"]


def test_bridge_parses_qwen_arg_pair_content_tool_call() -> None:
    content = """
<arg_code>
</arg_code>
<arg_name>
scene_inspect
</arg_name>
<arg_value>
{"includeSubtrees":["/World"]}
</arg_value>
"""

    cleaned, raw_tool_calls, assistant_msg, meta = bridge._extract_upstream_message(
        {"choices": [{"message": {"content": content}}]}
    )

    assert cleaned == "<arg_code>\n</arg_code>"
    assert meta["contentToolCallsParsed"] is True
    assert meta["contentToolCallFormats"] == ["qwen_arg_pair"]
    assert raw_tool_calls[0]["function"]["name"] == "scene_inspect"
    assert json.loads(raw_tool_calls[0]["function"]["arguments"]) == {
        "includeSubtrees": ["/World"]
    }
    assert assistant_msg["tool_calls"] == raw_tool_calls


def test_bridge_does_not_repair_malformed_content_tool_call() -> None:
    malformed = '<tool_call>{"name": "scene_inspect" />'

    content, raw_tool_calls, assistant_msg, meta = bridge._extract_upstream_message(
        {"choices": [{"message": {"content": malformed}}]}
    )

    assert content == malformed
    assert raw_tool_calls == []
    assert assistant_msg == {"role": "assistant", "content": malformed}
    assert meta == {}


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

    async def execute_callback_tool(**kwargs: Any) -> dict[str, Any]:
        callbacks.append(kwargs)
        return {"success": True, "result": {"programLoaded": True}, "mutated": False}

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", audit_log)
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)
    monkeypatch.setattr(bridge, "_execute_callback_tool", execute_callback_tool)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            request_id="req-read-loop",
            assistant_mode="query",
            catalog_tools=["program_read"],
            tools=[
                {
                    "id": "program_read",
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
    assert payload["toolCalls"][0]["name"] == "program_read"
    assert payload["toolCalls"][0]["status"] == "completed"
    assert payload["toolCalls"][0]["result"] == {"programLoaded": True}
    assert len(upstream_messages) == 2
    assert callbacks[0]["tool_id"] == "program_read"
    assert callbacks[0]["effect"] == "read_only"

    audit_record = json.loads(audit_log.read_text(encoding="utf-8").splitlines()[-1])
    assert audit_record["exitReason"] == "ok"
    assert audit_record["turnCount"] == 2


def test_bridge_executes_composer_draft_mutating_tools_in_composer_mode(
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
        **_: Any,
    ) -> dict[str, Any]:
        upstream_messages.append(messages)
        if len(upstream_messages) == 1:
            return {
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": "call-draft-add",
                                    "type": "function",
                                    "function": {
                                        "name": "scene_draft_add_object",
                                        "arguments": json.dumps(
                                            {"sceneResource": {"objectId": "box_1"}}
                                        ),
                                    },
                                }
                            ],
                        }
                    }
                ]
            }
        assert upstream_messages[-1][-1]["role"] == "tool"
        return {"choices": [{"message": {"content": "I added box_1 to the draft."}}]}

    async def execute_callback_tool(**kwargs: Any) -> dict[str, Any]:
        callbacks.append(kwargs)
        return {
            "success": True,
            "result": {"draftMutation": "scene_resource_add", "objectId": "box_1"},
            "mutated": True,
            "draftMutated": True,
        }

    monkeypatch.setattr(bridge, "AUDIT_LOG_PATH", audit_log)
    monkeypatch.setattr(bridge, "_resolve_model", resolve_model)
    monkeypatch.setattr(bridge, "_call_upstream", call_upstream)
    monkeypatch.setattr(bridge, "_execute_callback_tool", execute_callback_tool)

    response = TestClient(bridge.app).post(
        "/v1/manyforge/assistant",
        json=_bridge_payload(
            request_id="req-draft-loop",
            assistant_mode="composer-assistant",
            catalog_tools=["scene_draft_add_object"],
            tools=[
                {
                    "id": "scene_draft_add_object",
                    "effect": "composer_draft_mutating",
                    "description": "Add an object to the Composer draft scene.",
                }
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert "error" not in payload
    assert payload["message"] == "I added box_1 to the draft."
    assert payload["draftMutated"] is True
    assert payload["mutated"] is False
    assert payload["toolCalls"][0]["name"] == "scene_draft_add_object"
    assert payload["toolCalls"][0]["status"] == "completed"
    assert callbacks[0]["effect"] == "composer_draft_mutating"
    assert len(upstream_messages) == 2


def test_bridge_rejects_composer_draft_mutating_tool_in_query_mode(
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
                        "content": "",
                        "tool_calls": [
                            {
                                "id": "call-draft-add",
                                "type": "function",
                                "function": {
                                    "name": "scene_draft_add_object",
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
            request_id="req-draft-query",
            assistant_mode="query",
            catalog_tools=["scene_draft_add_object"],
            tools=[
                {
                    "id": "scene_draft_add_object",
                    "effect": "composer_draft_mutating",
                    "description": "Add an object to the Composer draft scene.",
                }
            ],
        ),
    )

    assert response.status_code == 200
    payload = response.json()
    assert payload["error"]["code"] == "response_rejected"
    assert "composer_draft_mutating tools may only execute" in payload["error"]["detail"]


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
