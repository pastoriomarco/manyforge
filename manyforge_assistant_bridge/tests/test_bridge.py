from __future__ import annotations

import asyncio
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import time
from typing import Any

from fastapi.testclient import TestClient
import httpx

from manyforge_assistant_bridge.bridge import BridgeConfig, BridgeService, create_app
from manyforge_assistant_bridge.catalog import (
    canonical_json,
    framed_digest,
    raw_digest,
    resolve_catalog,
)
from manyforge_headless_api.client import HeadlessHttpClient


def _projection() -> dict[str, Any]:
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    tool = {
        "description": "Read the current Composer Draft Program.",
        "effect": "read_only",
        "input_schema": schema,
        "input_schema_hash": raw_digest(schema),
        "tool_id": "program_read",
    }
    policy = {
        "approval_mode": None,
        "review_policy": "required",
        "session_kind": None,
    }
    catalog = {
        "grant_policy": policy,
        "mode": "composer-assistant",
        "nodes": [],
        "skills": [],
        "tools": [tool],
    }
    semantic = {
        "assistant_projection_digest": "sha256:" + "1" * 64,
        "catalog_hash": framed_digest(
            "assistant-mode-catalog", "manyforge.assistant_mode_catalog.v1", catalog
        ),
        "description": "Draft authoring",
        "grant_policy": policy,
        "graph_revision": "graph.1",
        "instrument_registry_digest": "sha256:" + "2" * 64,
        "mode": "composer-assistant",
        "model_endpoint": {
            "ai_model_component_id": "assistant_model",
            "api_key_env": None,
            "base_url": "http://upstream.test/v1",
            "model_endpoint_alias": "assistant_model",
            "model_id": "model.1",
            "request_contract": {
                "default_timeout_s": 30,
                "max_output_tokens": 512,
                "supported_image_transports": [],
                "supports_image_input": False,
                "supports_json_object": True,
                "supports_response_tools": True,
                "tool_call_family": "openai_chat_tools",
            },
        },
        "node_environment": {
            "catalog_digest": "sha256:" + "3" * 64,
            "catalog_id": "manyforge",
            "content_version": "2",
            "environment_digest": "sha256:" + "4" * 64,
        },
        "nodes": [],
        "provider": {
            "provider_alias": "assistant_provider",
            "provider_kind": "direct",
        },
        "retained_bundle_id": "bundle.1",
        "schema_version": "manyforge.assistant_mode_projection.v1",
        "skills": [],
        "tools": [tool],
    }
    semantic["source"] = {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "projection_generation": 1,
        "projection_owner_epoch": "headless.epoch.1",
        "receiver_age_ns": 10,
        "selection_projection_generation": 1,
        "selection_source_content_digest": "sha256:" + "5" * 64,
        "selection_source_generation": 1,
        "selection_source_owner_epoch": "manager.epoch.1",
        "selection_source_owner_id": "manager",
        "source_content_digest": framed_digest(
            "manyforge-assistant-mode-projection-semantic",
            "manyforge.assistant_mode_projection.v1",
            semantic,
        ),
        "source_generation": 1,
        "source_owner_epoch": "headless.epoch.1",
        "source_owner_id": "headless.assistant-mode.composer-assistant",
        "source_owner_kind": "assistant_mode_projection",
    }
    return semantic


def _scene_projection() -> dict[str, Any]:
    def simulation_source(entity: str, sequence: int) -> dict[str, Any]:
        return {
            "payload": {
                "api_version": "manyforge.ros_pose_observation.v1",
                "kind": "simulation_entity_pose",
                "pose": {},
                "source": {"entity": entity},
            },
            "source_sequence": sequence,
        }

    sources = [
        {
            "payload": {
                "api_version": "manyforge.ros_pose_observation.v1",
                "joint_names": ["joint-1"],
                "kind": "joint_state",
                "positions": [0.1],
                "source_stamp_s": 1.0,
                "topic": "/isaac_joint_states",
            },
            "source_sequence": 1,
        },
        simulation_source("/World/keep/item", 2),
        simulation_source("/World/keep/drop/item", 3),
        simulation_source("/World/other/item", 4),
        simulation_source("/World/probe", 5),
    ]
    return {
        "schema_version": "manyforge.scene_projection.v1",
        "projection_generation": 7,
        "logical_scene": {
            "available": True,
            "payload": {"api_version": "manyforge.logical_scene_source.v1"},
        },
        "scene_runtime": {
            "available": True,
            "payload": {
                "api_version": "manyforge.scene_runtime_source.v1",
                "state": {"pose_sources": sources},
            },
        },
    }


class _Headless(HeadlessHttpClient):
    def __init__(self, projection: dict[str, Any]) -> None:
        self.projection = projection
        self.assistant_mode_reads = 0
        self.scene_reads = 0

    def assistant_mode(self, mode: str) -> dict[str, Any]:
        assert mode == "composer-assistant"
        self.assistant_mode_reads += 1
        return deepcopy(self.projection)

    def node_environment(self) -> dict[str, Any]:
        mode = self.projection
        return {
            "schema_version": "manyforge.node_environment_projection.v2",
            "retained_bundle_id": mode["retained_bundle_id"],
            "graph_revision": mode["graph_revision"],
            **mode["node_environment"],
            "package_environment": deepcopy(mode["node_environment"]),
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
            },
        }

    def scene(self) -> dict[str, Any]:
        self.scene_reads += 1
        return deepcopy(_scene_projection())

    def close(self) -> None:
        pass


def _request(projection: dict[str, Any]) -> dict[str, Any]:
    resolved = resolve_catalog(projection, requested_tools=[], field_policy={})
    return {
        "schemaVersion": "manyforge.assistant.provider_request.v1",
        "requestId": "request.1",
        "requestOwnerEpoch": "composer.page.1",
        "sourceLabel": "composer_ui",
        "correlationId": None,
        "conversationId": "conversation.1",
        "assistantMode": "composer-assistant",
        "message": "Read my Program",
        "requestedTools": [],
        "fieldPolicy": {},
        "modeProjection": projection,
        "resolvedCatalog": resolved.to_wire(),
        "composerDraft": {
            "draftId": "draft.1",
            "expectedDraftRevision": 1,
            "expectedWorkspaceEpoch": "workspace.1",
            "expectedWorkspaceRevision": 3,
            "expectedAdmissionGeneration": 7,
            "programLoaded": True,
            "authoring": {"schema_version": "manyforge.composer_authoring_forest.v1"},
            "candidateAvailable": True,
            "programRevision": "sha256:" + "8" * 64,
            "program": {
                "api_version": "manyforge.program.v2",
                "program_revision": "sha256:" + "8" * 64,
            },
            "diagnostics": [],
            "nodeEnvironmentDigest": projection["node_environment"][
                "environment_digest"
            ],
            "nodeEnvironmentProjectionDigest": "sha256:" + "6" * 64,
            "sourceModelProjectionDigest": "sha256:" + "7" * 64,
        },
    }


def _append_projection_tool(
    projection: dict[str, Any],
    tool: dict[str, Any],
) -> None:
    projection["tools"].append(tool)
    projection["tools"].sort(key=lambda row: row["tool_id"])
    catalog = {
        "grant_policy": projection["grant_policy"],
        "mode": projection["mode"],
        "nodes": projection["nodes"],
        "skills": projection["skills"],
        "tools": projection["tools"],
    }
    projection["catalog_hash"] = framed_digest(
        "assistant-mode-catalog", "manyforge.assistant_mode_catalog.v1", catalog
    )
    projection["source"]["source_content_digest"] = framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v1",
        {key: value for key, value in projection.items() if key != "source"},
    )


def _service(
    tmp_path: Path,
    projection: dict[str, Any],
    *,
    upstream_handler,
    composer_handler,
) -> BridgeService:
    config = BridgeConfig(
        headless_api_url="http://headless.test",
        composer_base_url="http://composer.test",
        audit_log_path=tmp_path / "audit.jsonl",
        maximum_wall_seconds=5,
    )
    service = BridgeService(
        config,
        headless_client=_Headless(projection),  # type: ignore[arg-type]
        upstream_client=httpx.AsyncClient(
            transport=httpx.MockTransport(upstream_handler),
        ),
        composer_client=httpx.AsyncClient(
            base_url="http://composer.test",
            transport=httpx.MockTransport(composer_handler),
        ),
    )
    service._owns_upstream = True
    service._owns_composer = True
    return service


def _terminal(client: TestClient, request_id: str) -> dict[str, Any]:
    for _ in range(100):
        status = client.get(f"/v1/manyforge/assistant/{request_id}").json()
        if status["state"] != "running":
            return status
        time.sleep(0.01)
    raise AssertionError("assistant request did not finish")


def test_bridge_verifies_projection_and_runs_only_draft_callback(tmp_path: Path) -> None:
    projection = _projection()
    upstream_calls = 0
    upstream_bodies: list[dict[str, Any]] = []
    callback_bodies: list[dict[str, Any]] = []

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        assert str(request.url) == "http://upstream.test/v1/chat/completions"
        assert "authorization" not in request.headers
        upstream_calls += 1
        upstream_bodies.append(json.loads(request.content))
        if upstream_calls == 1:
            message = {
                "content": "",
                "tool_calls": [
                    {
                        "id": "call.1",
                        "type": "function",
                        "function": {"name": "program_read", "arguments": "{}"},
                    }
                ],
            }
        else:
            message = {"content": "Your Program is loaded."}
        return httpx.Response(200, json={"choices": [{"message": message}]}, request=request)

    async def composer(request: httpx.Request) -> httpx.Response:
        callback_body = json.loads(request.content)
        callback_bodies.append(callback_body)
        return httpx.Response(
            200,
            json={
                "success": True,
                "mutated": False,
                "draftMutated": False,
                "requestId": callback_body["requestId"],
                "requestOwnerEpoch": callback_body["requestOwnerEpoch"],
                "sourceLabel": callback_body["sourceLabel"],
                "correlationId": callback_body["correlationId"],
                "nextAdmissionGeneration": callback_body[
                    "expectedAdmissionGeneration"
                ],
                "nextWorkspaceRevision": callback_body[
                    "expectedWorkspaceRevision"
                ],
                "result": {"kind": "composer_draft_read", "draftRevision": 1},
            },
            request=request,
        )

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=composer
    )
    app = create_app(service.config, service=service)
    with TestClient(app) as client:
        accepted = client.post("/v1/manyforge/assistant", json=_request(projection))
        assert accepted.status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "completed"
    assert status["response"]["message"] == "Your Program is loaded."
    assert status["response"]["mutated"] is False
    assert callback_bodies[0]["catalogHash"] == projection["catalog_hash"]
    assert callback_bodies[0]["requestedTools"] == []
    assert callback_bodies[0]["expectedAdmissionGeneration"] == 7
    assert {
        "clientId",
        "snapshotReceiverEpoch",
        "snapshotSequence",
    }.isdisjoint(callback_bodies[0])
    assert callback_bodies[0]["requestId"].startswith("assistant.callback.")
    assert callback_bodies[0]["requestId"] != "request.1"
    assert upstream_calls == 2
    assert upstream_bodies[0]["max_tokens"] == 512
    assert upstream_bodies[0]["tool_choice"] == "auto"
    assert service._headless.assistant_mode_reads == 2
    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert [row["kind"] for row in audit] == ["tool_call", "request"]
    assert all(type(row["timestampNs"]) is int for row in audit)
    assert all(row["timestampNs"] > 9_007_199_254_740_991 for row in audit)
    assert "message" not in audit[-1]


def test_bridge_assigns_one_composer_idempotency_key_per_mutating_tool_call(
    tmp_path: Path,
) -> None:
    projection = _projection()
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    _append_projection_tool(
        projection,
        {
            "description": "Add one authored scene object.",
            "effect": "composer_draft_mutating",
            "input_schema": schema,
            "input_schema_hash": raw_digest(schema),
            "tool_id": "scene_draft_add_object",
        },
    )
    upstream_calls = 0
    callback_bodies: list[dict[str, Any]] = []

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        if upstream_calls == 1:
            message = {
                "content": "",
                "tool_calls": [
                    {
                        "id": f"call.{index}",
                        "type": "function",
                        "function": {
                            "name": "scene_draft_add_object",
                            "arguments": "{}",
                        },
                    }
                    for index in range(2)
                ],
            }
        else:
            message = {"content": "Both Draft edits completed."}
        return httpx.Response(
            200,
            json={"choices": [{"message": message}]},
            request=request,
        )

    async def composer(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        callback_bodies.append(body)
        next_revision = body["expectedDraftRevision"] + 1
        return httpx.Response(
            200,
            json={
                "success": True,
                "mutated": True,
                "draftMutated": True,
                "requestId": body["requestId"],
                "requestOwnerEpoch": body["requestOwnerEpoch"],
                "sourceLabel": body["sourceLabel"],
                "correlationId": body["correlationId"],
                "nextAdmissionGeneration": (
                    body["expectedAdmissionGeneration"] + 2
                ),
                "nextWorkspaceRevision": body["expectedWorkspaceRevision"] + 1,
                "result": {
                    "kind": "composer_draft_mutation",
                    "draftRevision": next_revision,
                },
            },
            request=request,
        )

    service = _service(
        tmp_path,
        projection,
        upstream_handler=upstream,
        composer_handler=composer,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post(
            "/v1/manyforge/assistant", json=_request(projection)
        ).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "completed"
    assert status["response"]["requestId"] == "request.1"
    assert status["response"]["draftMutated"] is True
    assert [body["expectedDraftRevision"] for body in callback_bodies] == [1, 2]
    assert [
        body["expectedAdmissionGeneration"] for body in callback_bodies
    ] == [7, 9]
    assert {
        body["expectedWorkspaceEpoch"] for body in callback_bodies
    } == {"workspace.1"}
    assert [
        body["expectedWorkspaceRevision"] for body in callback_bodies
    ] == [3, 4]
    callback_ids = [body["requestId"] for body in callback_bodies]
    expected_ids = [
        "assistant.callback."
        + hashlib.sha256(
            canonical_json(
                {
                    "provider_request_id": "request.1",
                    "request_owner_epoch": "composer.page.1",
                    "turn": 1,
                    "upstream_tool_call_id": f"call.{index}",
                    "tool_position": index,
                }
            )
        ).hexdigest()
        for index in range(2)
    ]
    assert callback_ids == expected_ids


def test_callback_transport_uncertainty_stops_before_another_model_turn(
    tmp_path: Path,
) -> None:
    projection = _projection()
    upstream_calls = 0
    callback_calls = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        return httpx.Response(
            200,
            json={
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": "call.uncertain",
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
            },
            request=request,
        )

    async def composer(request: httpx.Request) -> httpx.Response:
        nonlocal callback_calls
        callback_calls += 1
        raise httpx.ReadTimeout("callback response was lost", request=request)

    service = _service(
        tmp_path,
        projection,
        upstream_handler=upstream,
        composer_handler=composer,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post(
            "/v1/manyforge/assistant", json=_request(projection)
        ).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "failed"
    assert status["error"]["code"] == "assistant_callback_timeout"
    assert upstream_calls == 1
    assert callback_calls == 1


def test_inspect_isaac_scene_uses_only_filtered_headless_scene_truth(
    tmp_path: Path,
) -> None:
    projection = _projection()
    string_array = {
        "items": {"maxLength": 4096, "minLength": 1, "type": "string"},
        "maxItems": 4096,
        "type": "array",
        "uniqueItems": True,
    }
    schema = {
        "additionalProperties": False,
        "properties": {
            "exclude_subtrees": string_array,
            "include_subtrees": string_array,
            "mesh_export_dir": {
                "maxLength": 4096,
                "minLength": 1,
                "type": "string",
            },
            "mesh_source_preference": {
                "enum": ["auto", "original", "collider", "primitive"],
                "type": "string",
            },
            "output_profile": {
                "enum": ["llm_scene_authoring", "raw_inventory"],
                "type": "string",
            },
            "probe_prims": string_array,
        },
        "type": "object",
    }
    _append_projection_tool(
        projection,
        {
            "description": "Read a normalized Isaac scene-inspection report.",
            "effect": "read_only",
            "input_schema": schema,
            "input_schema_hash": raw_digest(schema),
            "tool_id": "inspect_isaac_scene",
        },
    )
    upstream_bodies: list[dict[str, Any]] = []
    arguments = {
        "exclude_subtrees": ["/World/keep/drop"],
        "include_subtrees": ["/World/keep"],
        "mesh_export_dir": "/must/not/be/written",
        "mesh_source_preference": "collider",
        "output_profile": "llm_scene_authoring",
        "probe_prims": ["/World/probe"],
    }

    async def upstream(request: httpx.Request) -> httpx.Response:
        upstream_bodies.append(json.loads(request.content))
        if len(upstream_bodies) == 1:
            message = {
                "content": "",
                "tool_calls": [
                    {
                        "id": "call.inspect",
                        "type": "function",
                        "function": {
                            "name": "inspect_isaac_scene",
                            "arguments": json.dumps(arguments),
                        },
                    }
                ],
            }
        else:
            message = {"content": "The owner scene has been inspected."}
        return httpx.Response(
            200,
            json={"choices": [{"message": message}]},
            request=request,
        )

    async def composer_must_not_run(
        request: httpx.Request,
    ) -> httpx.Response:
        raise AssertionError(
            "Isaac inspection must not use a Composer or simulator RPC"
        )

    service = _service(
        tmp_path,
        projection,
        upstream_handler=upstream,
        composer_handler=composer_must_not_run,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post(
            "/v1/manyforge/assistant", json=_request(projection)
        ).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "completed"
    assert service._headless.scene_reads == 1
    tool_message = next(
        row for row in upstream_bodies[1]["messages"] if row["role"] == "tool"
    )
    details = json.loads(tool_message["content"])["result"]["details"]
    assert details["schema_version"] == (
        "manyforge.assistant.isaac_scene_inspection.v1"
    )
    assert details["presentation"] == {
        "exclude_subtrees": ["/World/keep/drop"],
        "include_subtrees": ["/World/keep"],
        "mesh_export": {
            "performed": False,
            "requested_directory": "/must/not/be/written",
            "reason": (
                "Headless inspection is read-only and performs no simulator or "
                "filesystem RPC"
            ),
        },
        "mesh_source_preference": "collider",
        "output_profile": "llm_scene_authoring",
        "pose_source_count": 5,
        "probe_prims": ["/World/probe"],
        "selected_pose_source_count": 3,
        "selected_simulation_entity_count": 2,
    }
    selected = details["scene"]["scene_runtime"]["payload"]["state"][
        "pose_sources"
    ]
    assert [row["source_sequence"] for row in selected] == [1, 2, 5]


def test_bridge_rejects_a_self_consistent_but_nonactive_projection_before_model(
    tmp_path: Path,
) -> None:
    projection = _projection()
    upstream_calls = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        return httpx.Response(500, request=request)

    async def composer(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=composer
    )
    forwarded = deepcopy(projection)
    forwarded["description"] = "forged but self-consistent"
    forwarded["source"]["source_content_digest"] = framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v1",
        {key: value for key, value in forwarded.items() if key != "source"},
    )
    body = _request(forwarded)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "failed"
    assert status["error"]["code"] == "assistant_projection_stale"
    assert upstream_calls == 0
    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert audit[-1]["exitReason"] == "assistant_projection_stale"


def test_bridge_rejects_removed_envelopes_at_the_http_boundary(tmp_path: Path) -> None:
    projection = _projection()

    async def unused(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=unused, composer_handler=unused
    )
    with TestClient(create_app(service.config, service=service)) as client:
        response = client.post(
            "/v1/manyforge/assistant",
            json={"version": "manyforge.assistant.provider_request.v0"},
        )
    assert response.status_code == 422


def test_bridge_rejects_out_of_mode_output_and_audits_only_names_and_hashes(
    tmp_path: Path,
) -> None:
    projection = _projection()

    async def upstream(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200,
            json={
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": "call.outside",
                                    "type": "function",
                                    "function": {
                                        "name": "runtime_stop",
                                        "arguments": '{"reason":"secret value"}',
                                    },
                                }
                            ],
                        }
                    }
                ]
            },
            request=request,
        )

    async def unused(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "failed"
    assert status["error"]["code"] == "response_rejected"
    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert audit[-1]["detail"]["offendingTools"] == [
        {
            "name": "runtime_stop",
            "argumentsHash": hashlib.sha256(
                b'{"reason":"secret value"}'
            ).hexdigest(),
        }
    ]
    assert "secret value" not in (tmp_path / "audit.jsonl").read_text()


def test_bridge_rejects_duplicate_upstream_tool_call_ids_before_callbacks(
    tmp_path: Path,
) -> None:
    projection = _projection()

    async def upstream(request: httpx.Request) -> httpx.Response:
        call = {
            "id": "call.duplicate",
            "type": "function",
            "function": {"name": "program_read", "arguments": "{}"},
        }
        return httpx.Response(
            200,
            json={"choices": [{"message": {"content": "", "tool_calls": [call, call]}}]},
            request=request,
        )

    callback_calls = 0

    async def composer(request: httpx.Request) -> httpx.Response:
        nonlocal callback_calls
        callback_calls += 1
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path,
        projection,
        upstream_handler=upstream,
        composer_handler=composer,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "failed"
    assert status["error"]["code"] == "upstream_response_invalid"
    assert callback_calls == 0


def test_bridge_bounds_repeated_tool_failures_as_clarification(tmp_path: Path) -> None:
    projection = _projection()
    upstream_calls = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        return httpx.Response(
            200,
            json={
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": f"call.{upstream_calls}",
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
            },
            request=request,
        )

    async def composer(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        return httpx.Response(
            200,
            json={
                "success": False,
                "mutated": False,
                "draftMutated": False,
                "requestId": body["requestId"],
                "requestOwnerEpoch": body["requestOwnerEpoch"],
                "sourceLabel": body["sourceLabel"],
                "correlationId": body["correlationId"],
                "nextAdmissionGeneration": body[
                    "expectedAdmissionGeneration"
                ],
                "nextWorkspaceRevision": body["expectedWorkspaceRevision"],
                "result": {
                    "kind": "validation_error",
                    "message": "Draft is unavailable",
                },
            },
            request=request,
        )

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=composer
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "completed"
    assert status["response"]["needsClarification"] is True
    assert status["response"]["exitReason"] == "needs_clarification"
    assert upstream_calls == 4
    assert service._headless.assistant_mode_reads == 4


def test_bridge_cancellation_is_terminal_and_audited(tmp_path: Path) -> None:
    projection = _projection()

    async def upstream(request: httpx.Request) -> httpx.Response:
        await asyncio.sleep(60)
        return httpx.Response(500, request=request)

    async def unused(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        cancelled = client.post("/v1/manyforge/assistant/request.1/cancel")
        assert cancelled.status_code == 200
        assert cancelled.json()["state"] == "cancelled"
        assert _terminal(client, "request.1")["state"] == "cancelled"

    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert audit[-1]["exitReason"] == "cancelled"


def test_conversation_history_is_partitioned_by_exact_narrowed_catalog(
    tmp_path: Path,
) -> None:
    projection = _projection()
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    projection["tools"].append(
        {
            "description": "Read runtime status.",
            "effect": "read_only",
            "input_schema": schema,
            "input_schema_hash": raw_digest(schema),
            "tool_id": "status_read",
        }
    )
    catalog = {
        "grant_policy": projection["grant_policy"],
        "mode": projection["mode"],
        "nodes": projection["nodes"],
        "skills": projection["skills"],
        "tools": projection["tools"],
    }
    projection["catalog_hash"] = framed_digest(
        "assistant-mode-catalog", "manyforge.assistant_mode_catalog.v1", catalog
    )
    projection["source"]["source_content_digest"] = framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v1",
        {key: value for key, value in projection.items() if key != "source"},
    )
    upstream_bodies: list[dict[str, Any]] = []

    async def upstream(request: httpx.Request) -> httpx.Response:
        upstream_bodies.append(json.loads(request.content))
        return httpx.Response(
            200,
            json={
                "choices": [
                    {"message": {"content": f"answer {len(upstream_bodies)}"}}
                ]
            },
            request=request,
        )

    async def unused(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    first = _request(projection)
    first["requestedTools"] = ["program_read"]
    first["resolvedCatalog"] = resolve_catalog(
        projection, requested_tools=first["requestedTools"], field_policy={}
    ).to_wire()
    second = _request(projection)
    second["requestId"] = "request.2"
    second["requestedTools"] = ["status_read"]
    second["resolvedCatalog"] = resolve_catalog(
        projection, requested_tools=second["requestedTools"], field_policy={}
    ).to_wire()

    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=first).status_code == 202
        assert _terminal(client, "request.1")["state"] == "completed"
        assert client.post("/v1/manyforge/assistant", json=second).status_code == 202
        assert _terminal(client, "request.2")["state"] == "completed"

    second_messages = upstream_bodies[1]["messages"]
    assert all(message.get("content") != "answer 1" for message in second_messages)


def test_request_id_is_idempotent_only_for_identical_content(tmp_path: Path) -> None:
    projection = _projection()
    upstream_calls = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        return httpx.Response(
            200,
            json={"choices": [{"message": {"content": "done"}}]},
            request=request,
        )

    async def unused(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    body = _request(projection)
    changed = deepcopy(body)
    changed["message"] = "Different content"
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        conflict = client.post("/v1/manyforge/assistant", json=changed)
        assert conflict.status_code == 409
        assert conflict.json()["detail"]["code"] == "assistant_request_id_conflict"
        assert _terminal(client, "request.1")["state"] == "completed"

    assert upstream_calls == 1


def test_lost_projection_and_malformed_model_response_fail_before_effects(
    tmp_path: Path,
) -> None:
    projection = _projection()
    body = _request(projection)
    upstream_calls = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal upstream_calls
        upstream_calls += 1
        return httpx.Response(200, json={"choices": "invalid"}, request=request)

    async def unused(request: httpx.Request) -> httpx.Response:
        raise AssertionError("malformed/lost inputs must not reach Composer")

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    unavailable = deepcopy(projection)
    unavailable["source"]["freshness"] = "stale"
    service._headless.projection = unavailable
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        lost = _terminal(client, "request.1")
    assert lost["state"] == "failed"
    assert lost["error"]["code"] == "assistant_projection_unavailable"
    assert upstream_calls == 0

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    malformed = _request(projection)
    malformed["requestId"] = "request.2"
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=malformed).status_code == 202
        result = _terminal(client, "request.2")
    assert result["state"] == "failed"
    assert result["error"]["code"] == "upstream_response_invalid"
    assert upstream_calls == 1


def _with_token_env(projection: dict[str, Any], name: str) -> dict[str, Any]:
    projection["model_endpoint"]["api_key_env"] = name
    projection["source"]["source_content_digest"] = framed_digest(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v1",
        {key: value for key, value in projection.items() if key != "source"},
    )
    return projection


def test_bridge_sends_the_component_token_from_its_named_env_var(
    tmp_path: Path, monkeypatch
) -> None:
    projection = _with_token_env(_projection(), "MANYFORGE_TEST_AI_TOKEN")
    monkeypatch.setenv("MANYFORGE_TEST_AI_TOKEN", "secret-value")
    headers: list[str | None] = []

    async def upstream(request: httpx.Request) -> httpx.Response:
        headers.append(request.headers.get("authorization"))
        return httpx.Response(
            200, json={"choices": [{"message": {"content": "ok"}}]}, request=request
        )

    async def unused(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no tool call was requested")

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=unused
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["state"] == "completed"
    assert headers == ["Bearer secret-value"]
    assert "secret-value" not in (tmp_path / "audit.jsonl").read_text()


def test_bridge_refuses_an_unset_token_env_var_before_any_model_request(
    tmp_path: Path, monkeypatch
) -> None:
    projection = _with_token_env(_projection(), "MANYFORGE_TEST_AI_TOKEN")
    monkeypatch.delenv("MANYFORGE_TEST_AI_TOKEN", raising=False)

    async def unused(request: httpx.Request) -> httpx.Response:
        raise AssertionError("an unset token env var must not reach the model or Composer")

    service = _service(
        tmp_path, projection, upstream_handler=unused, composer_handler=unused
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["state"] == "failed"
    assert status["error"]["code"] == "ai_model_token_unavailable"


def test_bridge_reports_a_missing_ai_model_component_as_a_typed_failure(
    tmp_path: Path,
) -> None:
    from manyforge_headless_api.client import HeadlessClientError

    projection = _projection()

    async def unused(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no model request without an AI model component")

    service = _service(
        tmp_path, projection, upstream_handler=unused, composer_handler=unused
    )

    def absent(mode: str) -> dict[str, Any]:
        raise HeadlessClientError(
            "assistant_ai_model_absent", "no AI model component", status_code=503
        )

    service._headless.assistant_mode = absent  # type: ignore[method-assign]
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["state"] == "failed"
    assert status["error"]["code"] == "assistant_ai_model_absent"
