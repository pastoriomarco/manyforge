from __future__ import annotations

import asyncio
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
from typing import Any

from fastapi.testclient import TestClient
import httpx
import pytest

from manyforge_assistant_bridge.bridge import BridgeConfig, BridgeService, create_app
from manyforge_assistant_bridge.node_catalog import expected_shape
from manyforge_assistant_bridge.catalog import (
    AssistantCatalogError,
    canonical_json,
    framed_digest,
    raw_digest,
    resolve_catalog,
    verify_active_mode_projection,
)
from manyforge_headless_api.client import HeadlessHttpClient


def _projection() -> dict[str, Any]:
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    tool = {
        "argument_types": {},
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
        "assistant_component_id": "assistant",
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
        "retained_bundle_id": "bundle.1",
        "runs_in": ["no_program_active"],
        "schema_version": "manyforge.assistant_mode_projection.v2",
        "skills": [],
        "compact_history": False,
        "constrained_regeneration": False,
        "stuck_loop_recovery": True,
        "tools": [tool],
        "types": {},
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
            "manyforge.assistant_mode_projection.v2",
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
    def __init__(
        self, projection: dict[str, Any], node_catalog: dict[str, Any] | None = None
    ) -> None:
        self.projection = projection
        self.node_catalog = node_catalog or {"entries": [], "schema_types": {}}
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
            "catalog": deepcopy(self.node_catalog),
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
            },
        }

    def source_model(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.source_model_projection.v3",
            "components": [{"component_id": "robot", "mounted_on": None}],
            "targets": [{"component_id": "robot", "target_id": "arm",
                         "frames": [{"frame_role_id": "tcp"}]}],
        }

    # Headless conditional GETs: a strong ETag over the content digests, 304
    # while unchanged, and no tag while a source is not fresh (as app.py).
    @staticmethod
    def _conditional(
        payload: dict[str, Any], identity: Any, etag: str | None
    ) -> tuple[dict[str, Any] | None, str | None]:
        source = payload["source"]
        if (
            source.get("available") is not True
            or source.get("freshness") != "fresh"
            or source.get("known_gaps") != []
        ):
            return payload, None
        tag = '"' + hashlib.sha256(canonical_json(identity)).hexdigest() + '"'
        return (None, etag) if etag == tag else (payload, tag)

    def assistant_mode_if_changed(
        self, mode: str, etag: str | None
    ) -> tuple[dict[str, Any] | None, str | None]:
        projection = self.assistant_mode(mode)
        return self._conditional(
            projection, [projection["source"]["source_content_digest"]], etag
        )

    def node_environment_if_changed(
        self, etag: str | None
    ) -> tuple[dict[str, Any] | None, str | None]:
        environment = self.node_environment()
        return self._conditional(
            environment,
            {key: value for key, value in environment.items() if key != "source"},
            etag,
        )

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
                "api_version": "manyforge.program.v3",
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


def _rehash(projection: dict[str, Any]) -> dict[str, Any]:
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
        "manyforge.assistant_mode_projection.v2",
        {key: value for key, value in projection.items() if key != "source"},
    )
    return projection


def _append_projection_tool(
    projection: dict[str, Any],
    tool: dict[str, Any],
) -> None:
    tool.setdefault("argument_types", {})
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
        "manyforge.assistant_mode_projection.v2",
        {key: value for key, value in projection.items() if key != "source"},
    )


def _service(
    tmp_path: Path,
    projection: dict[str, Any],
    *,
    upstream_handler,
    composer_handler,
    node_catalog: dict[str, Any] | None = None,
) -> BridgeService:
    config = BridgeConfig(
        headless_api_url="http://headless.test",
        composer_base_url="http://composer.test",
        audit_log_path=tmp_path / "audit.jsonl",
        # Generous: a slower test host runs many verified turns per request.
        maximum_wall_seconds=60,
    )
    service = BridgeService(
        config,
        headless_client=_Headless(projection, node_catalog),  # type: ignore[arg-type]
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
    for _ in range(6_000):
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
    assert audit[-1]["assistantComponentId"] == "assistant"
    assert {"providerAlias", "providerKind"}.isdisjoint(audit[-1])


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
                # A claimless Draft edit echoes the admission generation.
                "nextAdmissionGeneration": body["expectedAdmissionGeneration"],
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
    ] == [7, 7]
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
    # Read results keep their content; the model sees it inline with `ok`.
    details = json.loads(tool_message["content"])
    assert details["ok"] is True
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
    selected = details["simulation_pose_sources"]
    assert [row["source_sequence"] for row in selected] == [1, 2, 5]
    # The scene part is in the shape the scene tools take, no projection
    # bookkeeping.
    assert set(details["scene"]) == {
        "note", "scene_resources", "other_objects", "carried_object_attachments"}


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
        "manyforge.assistant_mode_projection.v2",
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


def test_bridge_accepts_a_projection_whose_source_observation_moved_on(
    tmp_path: Path,
) -> None:
    # Composer's read and the bridge's read are separate Headless reads; the
    # Manager section's age, generations and digest move between them.
    projection = _projection()

    async def upstream(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200, json={"choices": [{"message": {"content": "ok"}}]}, request=request
        )

    async def composer(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, request=request)

    service = _service(
        tmp_path, projection, upstream_handler=upstream, composer_handler=composer
    )
    forwarded = deepcopy(projection)
    forwarded["source"]["receiver_age_ns"] = 250_000_000
    forwarded["source"]["projection_generation"] = 0
    forwarded["source"]["selection_source_generation"] = 0
    forwarded["source"]["selection_source_content_digest"] = "sha256:" + "6" * 64
    with TestClient(create_app(service.config, service=service)) as client:
        body = _request(forwarded)
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        status = _terminal(client, "request.1")

    assert status["state"] == "completed"
    assert status["response"]["message"] == "ok"


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


def test_an_out_of_mode_tool_is_never_run_and_gets_a_typed_error(
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

    # The call is never executed (Composer is never reached); the model gets
    # a typed error naming the tool, and the request ends by the stuck-loop
    # rule instead of failing.
    assert status["state"] == "completed"
    calls = status["response"]["toolCalls"]
    assert calls and all(call["status"] == "failed" for call in calls)
    assert calls[0]["result"]["detail"]["path"] == "tool"
    assert calls[0]["result"]["detail"]["reason"] == "unknown_tool"
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
    assert upstream_calls == 5
    assert service._headless.assistant_mode_reads == 5
    calls = status["response"]["toolCalls"]
    assert all(call["status"] == "failed" for call in calls)


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


def _held_upstream(release: threading.Event, seen: threading.Event):
    async def upstream(request: httpx.Request) -> httpx.Response:
        seen.set()
        while not release.is_set():
            await asyncio.sleep(0.01)
        return httpx.Response(
            200, json={"choices": [{"message": {"content": "Done."}}]}, request=request
        )

    return upstream


async def _unused_composer(request: httpx.Request) -> httpx.Response:
    return httpx.Response(500, request=request)


def _health(client: TestClient) -> dict[str, Any]:
    response = client.get("/health")
    assert response.status_code == 200
    return response.json()


def test_health_is_busy_exactly_while_a_record_is_running(tmp_path: Path) -> None:
    projection = _projection()
    release, seen = threading.Event(), threading.Event()
    service = _service(
        tmp_path,
        projection,
        upstream_handler=_held_upstream(release, seen),
        composer_handler=_unused_composer,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert _health(client) == {
            "schemaVersion": "manyforge.assistant.bridge_health.v2",
            "status": "ready",
            "activity": "idle",
        }
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        assert _health(client)["activity"] == "busy"
        assert seen.wait(10)
        assert _health(client)["activity"] == "busy"
        release.set()
        assert _terminal(client, "request.1")["state"] == "completed"
        assert _health(client)["activity"] == "idle"

        release.clear()
        body = _request(projection)
        body["requestId"] = "request.2"
        assert client.post("/v1/manyforge/assistant", json=body).status_code == 202
        assert _health(client)["activity"] == "busy"
        assert client.post("/v1/manyforge/assistant/request.2/cancel").json()["state"] == "cancelled"
        assert _health(client)["activity"] == "idle"
        release.set()


def test_shutdown_cancels_running_records_and_audits_cancelled(tmp_path: Path) -> None:
    projection = _projection()
    release, seen = threading.Event(), threading.Event()
    service = _service(
        tmp_path,
        projection,
        upstream_handler=_held_upstream(release, seen),
        composer_handler=_unused_composer,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        assert seen.wait(10)
    # Leaving the client ran the lifespan shutdown: BridgeService.close().

    assert service.ledger.busy() is False
    assert asyncio.run(service.ledger.status("request.1"))["state"] == "cancelled"
    audit = [json.loads(line) for line in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert audit[-1]["requestId"] == "request.1"
    assert audit[-1]["exitReason"] == "cancelled"
    assert audit[-1]["assistantComponentId"] == "assistant"


def test_bridge_serves_on_an_inherited_socket(tmp_path: Path) -> None:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.set_inheritable(True)
    port = listener.getsockname()[1]
    environment = {
        **os.environ,
        "PYTHONPATH": os.pathsep.join(path for path in sys.path if path),
        "MANYFORGE_ASSISTANT_AUDIT_LOG": str(tmp_path / "audit.jsonl"),
    }
    process = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "manyforge_assistant_bridge.bridge",
            "--fd",
            str(listener.fileno()),
        ],
        pass_fds=(listener.fileno(),),
        env=environment,
    )
    listener.close()
    try:
        deadline = time.monotonic() + 30
        while True:
            assert process.poll() is None
            try:
                response = httpx.get(f"http://127.0.0.1:{port}/health", timeout=1)
                break
            except httpx.TransportError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        assert response.json() == {
            "schemaVersion": "manyforge.assistant.bridge_health.v2",
            "status": "ready",
            "activity": "idle",
        }
    finally:
        process.terminate()
        process.wait(10)


def test_mode_projection_v2_is_accepted_and_v1_with_provider_refused() -> None:
    projection = _projection()
    assert verify_active_mode_projection(projection, expected_mode="composer-assistant") == projection

    for runs_in in ([], [""], "no_program_active"):
        broken = deepcopy(projection)
        broken["runs_in"] = runs_in
        with pytest.raises(AssistantCatalogError) as error:
            verify_active_mode_projection(_rehash(broken), expected_mode="composer-assistant")
        assert error.value.code == "assistant_projection_invalid"

    v1 = deepcopy(projection)
    v1["schema_version"] = "manyforge.assistant_mode_projection.v1"
    with pytest.raises(AssistantCatalogError) as error:
        verify_active_mode_projection(v1, expected_mode="composer-assistant")
    assert error.value.code == "assistant_projection_invalid"

    with_provider = deepcopy(projection)
    del with_provider["assistant_component_id"]
    del with_provider["runs_in"]
    with_provider["provider"] = {"provider_alias": "assistant_provider", "provider_kind": "direct"}
    with pytest.raises(AssistantCatalogError) as error:
        verify_active_mode_projection(
            _rehash(with_provider), expected_mode="composer-assistant"
        )
    assert error.value.code == "assistant_projection_invalid"


def test_conversation_history_is_partitioned_by_exact_narrowed_catalog(
    tmp_path: Path,
) -> None:
    projection = _projection()
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    projection["tools"].append(
        {
            "argument_types": {},
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
        "manyforge.assistant_mode_projection.v2",
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
        "manyforge.assistant_mode_projection.v2",
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


# --- catalog-generated node tools (OD-75) and stuck-loop recovery (OD-76) ---

from manyforge_behavior.catalog import NodeCatalog  # noqa: E402
from manyforge_behavior.node_environment import BUILTIN_CATALOG_PATH  # noqa: E402
from manyforge_headless_api.assistant_instruments import AssistantInstruments  # noqa: E402

_CATALOG = NodeCatalog(BUILTIN_CATALOG_PATH)
_INSTRUMENTS = AssistantInstruments(_CATALOG)


def _node_projection(
    *, stuck_loop_recovery: bool = True, compact_history: bool = False
) -> dict[str, Any]:
    """The test projection with real catalog-generated node tools and rows."""

    native = _projection()
    native["stuck_loop_recovery"] = stuck_loop_recovery
    native["compact_history"] = compact_history
    for field in ("catalog_hash", "instrument_registry_digest", "nodes"):
        native.pop(field)
    native["source"].pop("source_content_digest")
    native["tools"] = [
        {"effect": "read_only", "tool_id": "catalog_read"},
        {"effect": "composer_draft_mutating", "tool_id": "tree_draft_insert_node"},
    ]
    return _INSTRUMENTS.compose_mode(native, [entry.id for entry in _INSTRUMENTS.entries()])


def _scripted_upstream(calls: list[tuple[str, dict[str, Any]]], seen: list[dict[str, Any]]):
    async def upstream(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        seen.append(body)
        index = len(seen) - 1
        if index >= len(calls):
            message: dict[str, Any] = {"content": "done"}
        else:
            name, arguments = calls[index]
            message = {
                "content": "",
                "tool_calls": [
                    {
                        "id": f"call.{index}",
                        "type": "function",
                        "function": {"name": name, "arguments": json.dumps(arguments)},
                    }
                ],
            }
        return httpx.Response(200, json={"choices": [{"message": message}]}, request=request)

    return upstream


async def _no_composer(request: httpx.Request) -> httpx.Response:
    raise AssertionError("an invalid node call must not reach Composer")


def _run(tmp_path: Path, projection: dict[str, Any], upstream) -> dict[str, Any]:
    service = _service(
        tmp_path,
        projection,
        upstream_handler=upstream,
        composer_handler=_no_composer,
        node_catalog=_CATALOG.raw_data,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        return _terminal(client, "request.1")


def _gripper_insert(position: Any) -> dict[str, Any]:
    return {
        "parent_instance_id": "@root",
        "node": {
            "node_id": "gripper.command_position",
            "instance_id": "close_gripper",
            "parameters": {
                "target": {"component_id": "gripper", "command_target_id": "gripper"},
                "position": position,
            },
            "children": [],
        },
    }


def test_node_tools_are_small_and_the_prompt_lists_node_summaries(tmp_path: Path) -> None:
    projection = _node_projection()
    insert = next(row for row in projection["tools"] if row["tool_id"] == "tree_draft_insert_node")
    assert len(json.dumps(insert["input_schema"])) < 8_000
    assert all("type" in value for value in insert["input_schema"]["properties"].values())
    seen: list[dict[str, Any]] = []
    status = _run(tmp_path, projection, _scripted_upstream([], seen))
    assert status["state"] == "completed"
    prompt = seen[0]["messages"][0]["content"]
    gripper = next(
        line for line in prompt.splitlines()
        if line.strip().startswith("- gripper.command_position (target, position, ")
    )
    assert gripper.endswith(".")
    assert "example.check_frame_height" not in prompt
    assert "Rules:" in prompt and "Cell (frame cell_frame" in prompt
    assert "- robot: target arm (frames tcp)" in prompt
    assert len(prompt) < 12_000


def test_wrong_gripper_position_names_the_field_and_its_expected_shape(tmp_path: Path) -> None:
    seen: list[dict[str, Any]] = []
    calls = [
        ("tree_draft_insert_node", _gripper_insert("open")),  # type: ignore[arg-type]
        ("tree_draft_insert_node", _gripper_insert({"kind": "named_preset"})),
    ]
    status = _run(tmp_path, _node_projection(), _scripted_upstream(calls, seen))
    result = status["response"]["toolCalls"][0]["result"]
    assert result["kind"] == "validation_error"
    detail = result["detail"]
    assert detail["code"] == "assistant_tool_arguments_invalid"
    assert detail["path"] == "node.parameters.position"
    assert detail["expected"].startswith("coordinate_position: kind angular_rad + value_rad")
    assert result["message"].startswith("node.parameters.position: expected coordinate_position")
    missing = status["response"]["toolCalls"][1]["result"]["detail"]
    assert missing["path"] == "node.parameters.position.name"
    assert missing["expected"] == "open|closed"


def test_valid_nested_children_are_checked_per_node(tmp_path: Path) -> None:
    node = {
        "node_id": "sequence",
        "instance_id": "main",
        "parameters": {},
        "children": [_gripper_insert({"kind": "named_preset", "name": "open"})["node"]],
    }
    checked = []

    async def composer(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        checked.append(body["arguments"])
        return httpx.Response(
            200,
            json={
                "success": True,
                "mutated": False,
                "draftMutated": False,
                "requestId": body["requestId"],
                "requestOwnerEpoch": body["requestOwnerEpoch"],
                "sourceLabel": body["sourceLabel"],
                "correlationId": body["correlationId"],
                "nextAdmissionGeneration": body["expectedAdmissionGeneration"],
                "nextWorkspaceRevision": body["expectedWorkspaceRevision"],
                "result": {"kind": "composer_draft_read"},
            },
            request=request,
        )

    projection = _node_projection()
    seen: list[dict[str, Any]] = []
    service = _service(
        tmp_path,
        projection,
        upstream_handler=_scripted_upstream(
            [("tree_draft_insert_node", {"parent_instance_id": "@root", "node": node})], seen
        ),
        composer_handler=composer,
        node_catalog=_CATALOG.raw_data,
    )
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["response"]["toolCalls"][0]["status"] == "completed"
    assert checked[0]["node"]["children"][0]["node_id"] == "gripper.command_position"

    bad = deepcopy(node)
    bad["children"][0]["parameters"]["speed"] = 1.0
    seen = []
    status = _run(tmp_path, projection, _scripted_upstream(
        [("tree_draft_insert_node", {"parent_instance_id": "@root", "node": bad})], seen
    ))
    detail = status["response"]["toolCalls"][0]["result"]["detail"]
    assert detail["path"] == "node.children[0].parameters.speed"
    assert detail["expected"].startswith("one of the node's parameters: ")


def test_catalog_read_returns_descriptions_and_every_referenced_type(tmp_path: Path) -> None:
    seen: list[dict[str, Any]] = []
    calls = [("catalog_read", {"entry_ids": ["motion.move_to_pose"], "include_parameters": True})]
    status = _run(tmp_path, _node_projection(), _scripted_upstream(calls, seen))
    details = status["response"]["toolCalls"][0]["result"]["details"]
    entry = details["entries"][0]
    assert entry["description"] == _CATALOG.get("motion.move_to_pose").description
    assert all(parameter["description"] for parameter in entry["parameters"])
    types = details["schemaTypes"]
    references = set()

    def collect(value: Any) -> None:
        if isinstance(value, dict):
            if isinstance(value.get("schema_ref"), str):
                references.add(value["schema_ref"].rsplit("/", 1)[-1])
            for item in value.values():
                collect(item)
        elif isinstance(value, list):
            for item in value:
                collect(item)

    collect([entry["parameters"], types])
    assert references and references <= set(types)
    assert all(types[name]["description"] for name in types)
    assert "required_when" in json.dumps(types)


def _repeat_failures(tmp_path: Path, *, stuck_loop_recovery: bool) -> tuple[dict[str, Any], list]:
    seen: list[dict[str, Any]] = []
    calls = [("tree_draft_insert_node", _gripper_insert({"kind": "named_preset"}))] * 8
    status = _run(
        tmp_path,
        _node_projection(stuck_loop_recovery=stuck_loop_recovery),
        _scripted_upstream(calls, seen),
    )
    return status, seen


def _tool_messages(seen: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        json.loads(message["content"])
        for message in seen[-1]["messages"]
        if message["role"] == "tool"
    ]


def test_repeated_same_class_failures_get_a_hint_then_a_clarification(tmp_path: Path) -> None:
    status, seen = _repeat_failures(tmp_path, stuck_loop_recovery=True)
    response = status["response"]
    assert response["exitReason"] == "needs_clarification"
    assert response["needsClarification"] is True
    assert len(response["toolCalls"]) == 5
    tool_results = _tool_messages(seen)
    assert "recovery" not in tool_results[0]
    recovery = tool_results[1]["recovery"]
    assert recovery["code"] == "assistant_repeated_tool_failure"
    assert recovery["errorClass"] == "assistant_tool_arguments_invalid"
    assert recovery["repeats"] == 2
    assert recovery["clarificationRequired"] is False
    assert tool_results[2]["recovery"]["hint"] != recovery["hint"]


def test_stuck_loop_recovery_off_gives_no_hint_and_no_forced_clarification(
    tmp_path: Path,
) -> None:
    status, seen = _repeat_failures(tmp_path, stuck_loop_recovery=False)
    response = status["response"]
    assert response["exitReason"] == "ok"
    assert len(response["toolCalls"]) == 8
    assert all("recovery" not in result for result in _tool_messages(seen))


def test_a_refused_draft_edit_keeps_its_typed_code_for_the_model_and_recovery() -> None:
    from types import SimpleNamespace

    from manyforge_assistant_bridge.bridge import _DiagnosticsBaseline, _model_result
    from manyforge_assistant_bridge.recovery import StuckLoopTracker

    def refused(code: str) -> dict[str, Any]:
        # The Composer callback's refused-edit shape (routes_assistant_draft).
        return {"success": False, "mutated": False, "draftMutated": False, "result": {
            "kind": "validation_error", "httpStatus": 409, "message": f"refused: {code}",
            "detail": {"code": code, "message": f"refused: {code}"}}}

    call = SimpleNamespace(tool_id="tree_draft_move_node", arguments={})
    view = _model_result(call, refused("draft_arguments_invalid"),
                         _DiagnosticsBaseline(frozenset(), []))
    assert view["errors"] == [{"code": "draft_arguments_invalid",
                               "message": "refused: draft_arguments_invalid"}]

    tracker = StuckLoopTracker(enabled=True)
    assert tracker.record(call.tool_id, refused("draft_arguments_invalid")) is None
    # A different typed refusal of the same tool is a different class.
    assert tracker.record(call.tool_id, refused("draft_tree_cycle_invalid")) is None
    recovery = tracker.record(call.tool_id, refused("draft_arguments_invalid"))
    assert recovery["errorClass"] == "draft_arguments_invalid"
    assert recovery["repeats"] == 2


def test_experimental_reasoning_switch_maps_to_the_chat_template_kwarg(tmp_path: Path) -> None:
    for reasoning, expected in ((None, None), ("off", False), ("on", True)):
        projection = _projection()
        if reasoning is not None:
            projection["model_endpoint"]["request_contract"]["reasoning"] = reasoning
            _rehash(projection)
        seen: list[dict[str, Any]] = []
        status = _run(tmp_path, projection, _scripted_upstream([], seen))
        assert status["state"] == "completed"
        kwargs = seen[0].get("chat_template_kwargs")
        assert kwargs == (None if expected is None else {"enable_thinking": expected})


def test_sampling_is_sent_only_as_the_profile_sets_it(tmp_path: Path) -> None:
    sampling_fields = {"temperature", "top_p", "top_k", "min_p"}
    for sampling in (None, {"temperature": 0.7, "top_p": 0.8}, {"top_k": 20}):
        projection = _projection()
        if sampling is not None:
            projection["model_endpoint"]["request_contract"]["sampling"] = sampling
            _rehash(projection)
        seen: list[dict[str, Any]] = []
        status = _run(tmp_path, projection, _scripted_upstream([], seen))
        assert status["state"] == "completed"
        sent = {key: seen[0][key] for key in sampling_fields & set(seen[0])}
        assert sent == (sampling or {})



def _first_result(tmp_path: Path, arguments: dict[str, Any]) -> dict[str, Any]:
    seen: list[dict[str, Any]] = []
    status = _run(tmp_path, _node_projection(), _scripted_upstream(
        [("tree_draft_insert_node", arguments)], seen))
    return status["response"]["toolCalls"][0]["result"]


def test_placement_rule_is_named_instead_of_an_opaque_alternative(tmp_path: Path) -> None:
    arguments = _gripper_insert({"kind": "named_preset", "name": "open"})
    arguments["after_instance_id"] = "close_gripper"
    detail = _first_result(tmp_path, arguments)["detail"]
    assert detail["path"] == "placement"
    assert "exactly one of parent_instance_id" in detail["expected"]
    assert detail["errors"][0]["message"] == "got parent_instance_id and after_instance_id"


def test_every_error_of_the_closest_alternative_is_reported(tmp_path: Path) -> None:
    arguments = {
        "parent_instance_id": "@root",
        "node": {"node_id": "parallel", "parameters": {},
                 "children": [{"node_id": "timer", "parameters": {}},
                              {"node_id": "timer", "parameters": {"duration_s": 1.0}}]},
    }
    detail = _first_result(tmp_path, arguments)["detail"]
    paths = {row["path"] for row in detail["errors"]}
    assert {"node.instance_id", "node.children[0].instance_id",
            "node.children[1].instance_id"} <= paths
    assert all(row["expected"] for row in detail["errors"])
    assert all("$" != row["path"] for row in detail["errors"])


def test_leaf_children_and_empty_value_sources_may_be_omitted(tmp_path: Path) -> None:
    forwarded = []

    async def composer(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        forwarded.append(body["arguments"])
        return httpx.Response(200, json={
            "success": True, "mutated": False, "draftMutated": False,
            "requestId": body["requestId"], "requestOwnerEpoch": body["requestOwnerEpoch"],
            "sourceLabel": body["sourceLabel"], "correlationId": body["correlationId"],
            "nextAdmissionGeneration": body["expectedAdmissionGeneration"],
            "nextWorkspaceRevision": body["expectedWorkspaceRevision"],
            "result": {"kind": "composer_draft_read"}}, request=request)

    node = {"node_id": "timer", "instance_id": "wait", "parameters": {"duration_s": 1.0},
            "value_sources": {}}
    projection = _node_projection()
    service = _service(tmp_path, projection,
                       upstream_handler=_scripted_upstream(
                           [("tree_draft_insert_node",
                             {"parent_instance_id": "@root", "node": json.dumps(node)})], []),
                       composer_handler=composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["response"]["toolCalls"][0]["status"] == "completed"
    # The string-encoded object was decoded; Composer gets the accepted form.
    assert forwarded[0]["node"] == {"node_id": "timer", "instance_id": "wait",
                                    "parameters": {"duration_s": 1.0}, "children": []}


@pytest.mark.parametrize(("tool", "arguments", "field", "expected"), [
    ("tree_draft_insert_node",
     {"parent_instance_id": "@root", "node": {
         "node_id": "fallback", "instance_id": "try", "children": [
             {"node_id": "fallback", "instance_id": "inner"}]}},
     "node",
     {"node_id": "fallback", "instance_id": "try", "parameters": {}, "children": [
         {"node_id": "fallback", "instance_id": "inner", "parameters": {}, "children": []}]}),
    ("tree_draft_wrap_node",
     {"target_instance_id": "@root", "wrapper": {"node_id": "fallback", "instance_id": "w"}},
     "wrapper",
     {"node_id": "fallback", "instance_id": "w", "parameters": {}}),
    ("tree_draft_wrap_node",
     {"target_instance_id": "@root", "wrapper": {
         "node_id": "fallback", "instance_id": "w", "parameters": {}, "children": []}},
     "wrapper",
     {"node_id": "fallback", "instance_id": "w", "parameters": {}}),
    ("tree_draft_wrap_node",
     {"target_instance_id": "@root", "wrapper": {
         "node_id": "fallback", "instance_id": "w", "children": [{"node_id": "timer"}]}},
     "wrapper",
     {"node_id": "fallback", "instance_id": "w", "parameters": {},
      "children": [{"node_id": "timer"}]}),
])
def test_omitted_parameters_are_empty(
    tmp_path: Path, tool: str, arguments: dict, field: str, expected: dict
) -> None:
    from manyforge_assistant_bridge.node_catalog import normalize_tree_arguments

    normalize_tree_arguments(tool, arguments)
    assert arguments[field] == expected


def test_an_object_argument_that_is_not_json_is_a_typed_failure(tmp_path: Path) -> None:
    detail = _first_result(tmp_path, {"parent_instance_id": "@root",
                                      "node": '{"node_id": "timer" "x": 1}'})["detail"]
    assert detail["path"] == "node"
    assert detail["reason"] == "json_parse"


def test_unknown_value_source_names_the_bindable_parameters(tmp_path: Path) -> None:
    arguments = _gripper_insert({"kind": "named_preset", "name": "open"})
    arguments["node"]["value_sources"] = {"dummy": {"kind": "blackboard_key", "reference": "k"}}
    detail = _first_result(tmp_path, arguments)["detail"]
    assert detail["path"] == "node.value_sources.dummy"
    assert detail["expected"].startswith("value_sources binds a parameter")


def _forwarding_composer(forwarded: list, *, result: dict | None = None):
    async def composer(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        forwarded.append(body["arguments"])
        return httpx.Response(200, json={
            "success": True, "mutated": True, "draftMutated": True,
            "requestId": body["requestId"], "requestOwnerEpoch": body["requestOwnerEpoch"],
            "sourceLabel": body["sourceLabel"], "correlationId": body["correlationId"],
            "nextAdmissionGeneration": body["expectedAdmissionGeneration"],
            "nextWorkspaceRevision": body["expectedWorkspaceRevision"] + 1,
            "result": result or {"kind": "composer_draft_mutation", "mutation": "x",
                                 "details": {"inserted": ["par"]}, "draftId": "draft.1",
                                 "draftRevision": 2, "candidateAvailable": False,
                                 "programRevision": None,
                                 "diagnostics": [{"code": "authoring_node_children_incomplete",
                                                  "path": "forest[0].children[0].children",
                                                  "message": "Requires at least 1 children; found 0.",
                                                  "severity": "error"}]}}, request=request)
    return composer


def test_incomplete_nodes_go_to_the_draft_and_come_back_as_diagnostics(tmp_path: Path) -> None:
    forwarded: list = []
    seen: list = []
    projection = _node_projection()
    calls = [("tree_draft_insert_node", {"parent_instance_id": "@root", "node": {
        "node_id": "parallel", "instance_id": "par", "parameters": {}}}),
        ("tree_draft_insert_node", {"parent_instance_id": "par", "node": {
            "node_id": "gripper.command_position", "instance_id": "g", "parameters": {}}})]
    service = _service(tmp_path, projection, upstream_handler=_scripted_upstream(calls, seen),
                       composer_handler=_forwarding_composer(forwarded),
                       node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert [call["status"] for call in status["response"]["toolCalls"]] == ["completed"] * 2
    assert forwarded[0]["node"]["children"] == []
    view = json.loads(next(m for m in seen[1]["messages"] if m["role"] == "tool")["content"])
    assert view == {"ok": True, "changed": {"inserted": ["par"]},
                    "diagnostics_new": [
                        "forest[0].children[0].children: Requires at least 1 children; found 0."],
                    "diagnostics_other": 0}
    # The request introduced it, so it stays new after the next edit.
    second = json.loads([m for m in seen[2]["messages"] if m["role"] == "tool"][-1]["content"])
    assert second["diagnostics_new"] == view["diagnostics_new"]
    assert second["diagnostics_other"] == 0


def test_typed_arguments_are_checked_against_their_full_type(tmp_path: Path) -> None:
    projection = _node_projection()
    native_tools = [{"effect": "composer_draft_mutating", "tool_id": "scene_draft_add_object"}]
    projection = _INSTRUMENTS.compose_mode(
        {**{k: v for k, v in projection.items() if k not in (
            "catalog_hash", "instrument_registry_digest", "nodes", "types")},
         "tools": native_tools,
         "source": {k: v for k, v in projection["source"].items() if k != "source_content_digest"}},
        [row["node_id"] for row in projection["nodes"]])
    seen: list = []
    calls = [("scene_draft_add_object", {"scene_resource": {"scene_resource_id": "box",
              "shape": {"type": "box"}, "pose": {"frame_ref": {"kind": "cell_frame"},
              "position_m": [0, 0, 0], "orientation": {"xyzw": [0, 0, 0, 1]}}}})]
    service = _service(tmp_path, projection, upstream_handler=_scripted_upstream(calls, seen),
                       composer_handler=_no_composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    detail = status["response"]["toolCalls"][0]["result"]["detail"]
    assert detail["path"].startswith("scene_resource.shape")


def test_catalog_read_resolves_named_types(tmp_path: Path) -> None:
    seen: list = []
    calls = [("catalog_read", {"types": ["scene_resource", "coordinate_position"]})]
    status = _run(tmp_path, _node_projection(), _scripted_upstream(calls, seen))
    details = status["response"]["toolCalls"][0]["result"]["details"]
    assert details["entries"] == []
    assert "scene_resource_id" in details["types"]["scene_resource"]["shape"]
    assert details["types"]["coordinate_position"]["properties"]["kind"]["values"]


def test_a_read_tool_argument_error_is_a_tool_result_not_a_request_failure(tmp_path: Path) -> None:
    seen: list = []
    calls = [("catalog_read", {"entry_ids": ["program_draft_upsert_parameters"]})]
    status = _run(tmp_path, _node_projection(), _scripted_upstream(calls, seen))
    assert status["state"] == "completed"
    call = status["response"]["toolCalls"][0]
    assert call["status"] == "failed"
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is False
    assert view["errors"][0]["path"] == "entry_ids"
    assert "system prompt" in view["errors"][0]["expected"]



@pytest.mark.parametrize(
    ("tool", "arguments", "path_or_code"),
    [
        ("catalog_read", {"types": ["no_such_type"]}, "types"),
        ("catalog_read", {"entry_ids": ["no.such.node"]}, "entry_ids"),
        ("skills_read", {"skill_ids": ["no_such_skill"]}, "assistant_tool_arguments_invalid"),
        ("skills_read", {"include_bodies": True}, "assistant_skill_body_unavailable"),
    ],
)
def test_read_tool_argument_failures_are_tool_results(
    tmp_path: Path, tool: str, arguments: dict, path_or_code: str
) -> None:
    projection = _node_projection()
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": "read_only", "tool_id": tool}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    projection = _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])
    seen: list = []
    status = _run(tmp_path, projection, _scripted_upstream([(tool, arguments)], seen))
    assert status["state"] == "completed"
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is False
    error = view["errors"][0]
    assert path_or_code in (error.get("path"), error.get("code"))


def test_a_composer_argument_refusal_is_a_tool_result(tmp_path: Path) -> None:
    async def composer(request: httpx.Request) -> httpx.Response:
        return httpx.Response(400, json={"detail": {
            "code": "assistant_callback_arguments_invalid", "message": "$.node: bad"}},
            request=request)

    projection = _node_projection()
    seen: list = []
    calls = [("tree_draft_insert_node", {"parent_instance_id": "@root", "node": {
        "node_id": "timer", "instance_id": "t", "parameters": {"duration_s": 1.0}}})]
    service = _service(tmp_path, projection, upstream_handler=_scripted_upstream(calls, seen),
                       composer_handler=composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    assert status["state"] == "completed"
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view == {"ok": False, "errors": [{"code": "assistant_callback_arguments_invalid",
                                             "message": "$.node: bad"}]}


def test_unknown_tool_suggests_its_family_then_the_closest_name() -> None:
    from manyforge_assistant_bridge.bridge import _tool_suggestion

    tools = ["catalog_read", "tree_draft_delete_node", "tree_draft_move_node", "program_read"]
    assert _tool_suggestion("tree_draft_remove_node", tools) == (
        "listed tree_draft_* tools: tree_draft_delete_node, tree_draft_move_node"
    )
    assert _tool_suggestion("catelog_read", tools) == "closest: catalog_read"



def test_scene_inspect_presents_objects_in_the_scene_tool_shape() -> None:
    from manyforge_assistant_bridge.bridge import present_scene

    scene = {"logical_scene": {"payload": {"snapshot": {"objects": [{
        "scene_resource_id": "graspable",
        "collision_shapes": [{"type": "box", "dimensions_m": {"x": 0.1, "y": 0.2, "z": 0.3}}],
        "placement": {"cell_id": "c1", "kind": "cell_fixed", "pose_in_cell": {
            "orientation_xyzw": [0, 0, 0, 1], "position_m": {"x": 1, "y": 2, "z": 3}}},
    }], "carried_object_attachments": []}}}}
    view = present_scene(scene)
    assert view["scene_resources"] == [{
        "scene_resource_id": "graspable",
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.2, 0.3]},
        "pose": {"frame_ref": {"kind": "cell_frame"}, "position_m": [1, 2, 3],
                 "orientation": {"xyzw": [0, 0, 0, 1]}},
    }]


def test_catalog_read_gives_the_insertable_node_form(tmp_path: Path) -> None:
    seen: list = []
    projection = _node_projection()
    node_id = projection["nodes"][0]["node_id"]
    calls = [("catalog_read", {"entry_ids": [node_id]})]
    status = _run(tmp_path, projection, _scripted_upstream(calls, seen))
    entry = status["response"]["toolCalls"][0]["result"]["details"]["entries"][0]
    assert entry["insert_as"].startswith("{")
    assert "instance_id: identifier" in entry["insert_as"]
    assert f"node_id: '{node_id}'" in entry["insert_as"]


def test_catalog_read_answers_a_type_name_in_entry_ids_with_a_note(tmp_path: Path) -> None:
    seen: list = []
    calls = [("catalog_read", {"entry_ids": ["scene_resource", "timer"]})]
    _run(tmp_path, _node_projection(), _scripted_upstream(calls, seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is True
    assert [row["id"] for row in view["entries"]] == ["timer"]
    assert "scene_resource_id" in view["types"]["scene_resource"]["shape"]
    assert view["note"] == "scene_resource is a type; next time use types: [scene_resource]"


def _two_requests(tmp_path: Path, *, compact_history: bool) -> list[dict[str, Any]]:
    projection = _node_projection(compact_history=compact_history)
    seen: list[dict[str, Any]] = []
    calls = [("catalog_read", {"entry_ids": ["no.such.node"]})]
    service = _service(tmp_path, projection, upstream_handler=_scripted_upstream(calls, seen),
                       composer_handler=_no_composer, node_catalog=_CATALOG.raw_data)
    second = _request(projection)
    second["requestId"] = "request.2"
    second["message"] = "and now the second request"
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        assert _terminal(client, "request.1")["state"] == "completed"
        assert client.post("/v1/manyforge/assistant", json=second).status_code == 202
        assert _terminal(client, "request.2")["state"] == "completed"
    return seen[-1]["messages"]


def test_compact_history_carries_request_answer_and_edits_only(tmp_path: Path) -> None:
    messages = _two_requests(tmp_path, compact_history=True)
    assert [m["role"] for m in messages] == ["system", "user", "assistant", "user"]
    assert messages[2]["content"] == "done\n\nDraft changes: none."
    assert messages[3]["content"] == "and now the second request"


def test_full_history_is_the_default_and_keeps_tool_results(tmp_path: Path) -> None:
    messages = _two_requests(tmp_path, compact_history=False)
    assert [m["role"] for m in messages] == [
        "system", "user", "assistant", "tool", "assistant", "user"]


def test_change_summary_names_what_each_committed_edit_touched() -> None:
    from manyforge_assistant_bridge.history import change_summary

    calls = [
        {"toolId": "tree_draft_insert_node", "draftMutated": True, "arguments": {
            "parent_instance_id": "@root",
            "node": {"node_id": "logic.sequence", "instance_id": "main", "children": [
                {"node_id": "logic.wait_for_value", "instance_id": "wait",
                 "parameters": {"key": "not.an.edit"}}]}}},
        {"toolId": "tree_draft_update_node_params", "draftMutated": True,
         "arguments": {"instance_id": "wait", "parameters": {"key": "x"}}},
        {"toolId": "scene_draft_remove_objects", "draftMutated": True,
         "arguments": {"scene_resource_ids": ["box_a", "box_b"]}},
        {"toolId": "program_draft_upsert_parameters", "draftMutated": True,
         "arguments": {"parameters": [{"parameter_id": "speed", "type": "number"}]}},
        {"toolId": "program_draft_upsert_contact_policies", "draftMutated": True,
         "arguments": {"contact_policies": [{"policy_id": "grasp_contact", "zones": [
             {"zone_id": "fingertips", "body_pairs": [{"body_b": {
                 "kind": "runtime_object", "runtime_object_id": "pick_target"}}]}]}]}},
        {"toolId": "program_draft_remove_contact_policies", "draftMutated": True,
         "arguments": {"contact_policy_ids": ["old_grasp"]}},
        {"toolId": "tree_draft_delete_node", "draftMutated": False,
         "arguments": {"instance_id": "failed"}},
    ]
    assert change_summary(calls) == (
        "Draft changes:\n"
        "- added main, wait (tree_draft_insert_node)\n"
        "- updated wait (tree_draft_update_node_params)\n"
        "- removed box_a, box_b (scene_draft_remove_objects)\n"
        "- added or updated speed (program_draft_upsert_parameters)\n"
        "- added or updated grasp_contact (program_draft_upsert_contact_policies)\n"
        "- removed old_grasp (program_draft_remove_contact_policies)"
    )


def test_json_parse_errors_say_where_the_text_breaks() -> None:
    from manyforge_assistant_bridge.bridge import _decode_typed_arguments

    schema = {"properties": {"node": {"type": "object"}}}
    extra = {"node": '{"node_id": "a", "instance_id": "b"}}'}
    (error,) = _decode_typed_arguments(extra, schema)
    assert error.message == (
        "the node object is complete at char 36; remove the trailing text '}'")
    broken = {"node": '{"node_id": "a" "instance_id": "b"}'}
    (error,) = _decode_typed_arguments(broken, schema)
    assert error.message == (
        "the node object is not valid JSON: Expecting ',' delimiter at char 16: "
        "'_id\": \"a\" <HERE>\"instance_'")


@pytest.mark.parametrize("placeholder", ["", "None", "null"])
def test_a_placeholder_for_an_optional_id_says_to_omit_it(
    tmp_path: Path, placeholder: str
) -> None:
    projection = _node_projection()
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": "read_only", "tool_id": "program_read"}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    projection = _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])
    seen: list = []
    calls = [("program_read", {"instance_id": placeholder})]
    status = _run(tmp_path, projection, _scripted_upstream(calls, seen))
    assert status["state"] == "completed"
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["errors"][0]["path"] == "instance_id"
    assert view["errors"][0]["message"] == (
        f"{placeholder!r} is not a value: omit instance_id to read the whole Program")


def test_a_placeholder_for_a_required_id_or_an_enum_is_not_reported_as_omittable() -> None:
    from manyforge_assistant_bridge.node_catalog import placeholder_errors

    schema = {"type": "object", "required": ["instance_id"], "properties": {
        "instance_id": {"type": "string", "pattern": "^[a-z]+$"},
        "source": {"enum": ["none", "camera"]}}}
    assert placeholder_errors(schema, {"instance_id": "null", "source": "none"}) == []


def test_prompt_scene_lines_give_size_pose_and_roles() -> None:
    from manyforge_assistant_bridge.prompt import _scene_line

    row = {
        "scene_resource_id": "graspable",
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.01, 0.01]},
        "pose": {"frame_ref": {"kind": "cell_frame"}, "position_m": [0.3, -0.3, 0.15],
                 "orientation": {"xyzw": [0, 0, -0.3826834323650898, 0.9238795325112867]}},
        "resource_roles": ["pick_object"],
    }
    assert _scene_line(row) == (
        "- graspable: box 0.1x0.01x0.01 at (0.3, -0.3, 0.15) rpy (0, 0, -45) in cell, "
        "roles pick_object")
    sphere = {"scene_resource_id": "ball", "shape": {"type": "sphere", "sphere_radius_m": 0.04},
              "pose": {"frame_ref": {"kind": "cell_frame"}, "position_m": [0, 0, 0.00004],
                       "orientation": {"rpy_deg": [0, -0.0001, 90]}}}
    assert _scene_line(sphere) == "- ball: sphere radius_m=0.04 at (0, 0, 0) rpy (0, 0, 90) in cell"


def test_prompt_outline_lists_contact_policies_compactly() -> None:
    from manyforge_assistant_bridge.prompt import MAXIMUM_SECTION_LINES, _outline

    grasp = {
        "policy_id": "grasp_part", "mode": "contact_grasp", "version": "sha256:" + "0" * 64,
        "zones": [{"zone_id": "tool_contact", "max_penetration_m": 0.002,
                   "terminal_window_start_fraction": 0.8, "violation_response": "hard_stop",
                   "body_pairs": [{"body_a": {"kind": "component_model_body",
                                              "body": {"component_id": "gripper",
                                                       "collision_body_id": "fingers"}},
                                   "body_b": {"kind": "runtime_object",
                                              "runtime_object_id": "held_part"}}]}],
    }
    release = {
        "policy_id": "release_part", "mode": "contact_release",
        "zones": [{"zone_id": "tip", "release_evidence_mode": "binary_contact",
                   "body_pairs": [{"body_a": {"kind": "motion_endpoint"},
                                   "body_b": {"kind": "runtime_object",
                                              "runtime_object_id": "held_part"}}]}],
    }
    out = _outline({"forest": [], "contact_policies": [grasp, release]}, [])
    start = out.index("Contact policies:")
    assert out[start + 1:start + 3] == [
        "- grasp_part: contact_grasp; zone tool_contact: gripper/fingers~held_part, "
        "max penetration 0.002 m",
        "- release_part: contact_release; zone tip: motion endpoint~held_part, "
        "evidence binary_contact",
    ]
    assert "Contact policies:" not in _outline({"forest": []}, [])
    many = _outline({"forest": [], "contact_policies": [
        {**grasp, "policy_id": f"p{i}"} for i in range(MAXIMUM_SECTION_LINES + 5)]}, [])
    assert sum(line.startswith("- p") for line in many) == MAXIMUM_SECTION_LINES



@pytest.mark.parametrize(("arguments", "message"), [
    ({"entry_ids": ["fallback"], "kind": "decorator"},
     "fallback is a composite, excluded by kind: decorator"),
    ({"entry_ids": ["timer"], "category": "motion"},
     "timer is a {category}, excluded by category: motion"),
])
def test_catalog_read_says_when_a_filter_excludes_a_named_entry(
    tmp_path: Path, arguments: dict, message: str
) -> None:
    category = next(row["category"] for row in _CATALOG.raw_data["entries"]
                    if row["id"] == "timer")
    seen: list = []
    _run(tmp_path, _node_projection(), _scripted_upstream([("catalog_read", arguments)], seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["errors"][0]["message"] == message.format(category=category)



def test_a_wrapper_with_children_is_still_refused(tmp_path: Path) -> None:
    projection = _node_projection()
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": "composer_draft_mutating", "tool_id": "tree_draft_wrap_node"}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    projection = _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])
    seen: list = []
    calls = [("tree_draft_wrap_node", {"target_instance_id": "@root", "wrapper": {
        "node_id": "fallback", "instance_id": "w",
        "children": [{"node_id": "timer", "instance_id": "t"}]}})]
    _run(tmp_path, projection, _scripted_upstream(calls, seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is False
    assert view["errors"][0]["path"] == "wrapper.children"



def test_catalog_read_answers_a_tool_name_in_entry_ids_with_its_argument_types(
    tmp_path: Path,
) -> None:
    projection = _node_projection()
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": "read_only", "tool_id": "catalog_read"},
                       {"effect": "composer_draft_mutating", "tool_id": "scene_draft_add_object"},
                       {"effect": "composer_draft_mutating", "tool_id": "tree_draft_delete_node"}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    projection = _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])
    seen: list = []
    calls = [("catalog_read", {"entry_ids": ["scene_draft_add_object", "tree_draft_delete_node"]})]
    _run(tmp_path, projection, _scripted_upstream(calls, seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is True
    assert list(view["types"]) == ["scene_resource"]
    assert view["note"] == (
        "scene_draft_add_object is a tool; its arguments: scene_resource (type scene_resource); "
        "their types are under types (next time use types: [scene_resource]); "
        "tree_draft_delete_node is a tool; its arguments: {instance_id: identifier}")


def test_catalog_read_unknown_names_stay_errors(tmp_path: Path) -> None:
    seen: list = []
    _run(tmp_path, _node_projection(), _scripted_upstream(
        [("catalog_read", {"entry_ids": ["scene_resource", "no_such_thing"]})], seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is False
    assert [row["message"] for row in view["errors"]] == [
        "not a node id: no_such_thing; node ids are listed in the system prompt"]


def test_every_type_name_shown_to_the_model_resolves_through_catalog_read() -> None:
    """Expected shapes (compact_shape, expected_shape) and catalog_read parameter
    rows name schema_ref types and kinds; enclosing shapes name the projection's
    argument types. Every such name over the real catalog is readable."""

    projection = _node_projection()
    catalog = _CATALOG.raw_data
    names: set[str] = set(projection["types"])

    def walk(schema: Any) -> None:
        if not isinstance(schema, dict):
            return
        if isinstance(schema.get("schema_ref"), str):
            names.add(schema["schema_ref"].rsplit("/", 1)[-1])
        if isinstance(schema.get("type"), str) and schema["type"] not in ("object", "array", "enum"):
            names.add(schema["type"])
        for child in (schema.get("properties") or {}).values():
            walk(child)
        walk(schema.get("items"))
        walk(schema.get("additional_properties"))

    for entry in catalog["entries"]:
        for parameter in entry.get("parameters", []):
            walk(parameter.get("schema") or {"schema_ref": parameter.get("schema_ref")})
    for schema in catalog["schema_types"].values():
        walk(schema)
    environment = {"catalog": catalog, "retained_bundle_id": "bundle",
                   "environment_digest": "digest"}

    def read(name: str) -> dict[str, Any]:
        return BridgeService._catalog_read(
            environment, {"types": [name]}, projection["nodes"], projection["types"],
            projection["tools"])["types"][name]

    unresolved = []
    for name in sorted(names):
        try:
            read(name)
        except Exception:  # noqa: BLE001 - collected for one assertion
            unresolved.append(name)
    assert unresolved == []
    # The live trace: an insert error named the condition kind.
    assert expected_shape(catalog, "motion.move_until_signal",
                          ["parameters", "interrupt_condition", "signal"]) == (
        "component_interrupt_condition")
    assert "signal_name" in read("component_interrupt_condition")["shape"]


def test_deployment_capabilities_read_shows_a_concise_view(tmp_path: Path) -> None:
    projection = _single_tool_projection("deployment_capabilities_read", "read_only")

    def implementation(node_id: str, reasons: list[dict[str, Any]]) -> dict[str, Any]:
        codes = sorted({row["code"] for row in reasons} | (
            {"program_runtime_owner_unavailable"} if reasons else set()))
        return {"node_id": node_id, "engine_id": "py_trees", "implementation_id": node_id,
                "executable_package_id": "manyforge_behavior", "adapter_build_qualified": True,
                "authoring_available": True, "binding_available": not reasons,
                "execution_available": not reasons, "implementation_identity_resolved": True,
                "owner_health_available": True, "reason_codes": codes, "reasons": reasons,
                "restoration_gates": []}

    availability = {
        "binding_context": "applied", "execution_available": True,
        "manager_runtime_converged": True, "portable_authoring_available": True,
        "reason_codes": [], "restoration_gates": [],
        "implementations": [
            implementation("io.set_output", []),
            implementation("simulation.step", [{
                "code": "capability_not_offered",
                "message": "No component offers simulator.control.step",
                "details": {"capability": "simulator.control.step"}}]),
        ],
    }
    seen: list = []
    service = _service(tmp_path, projection,
                       upstream_handler=_scripted_upstream([("deployment_capabilities_read", {})], seen),
                       composer_handler=_no_composer, node_catalog=_CATALOG.raw_data)
    full = service._headless.node_environment

    def node_environment() -> dict[str, Any]:
        return {**full(), "availability": deepcopy(availability)}

    service._headless.node_environment = node_environment  # type: ignore[method-assign]
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    # The record keeps the full read; the model sees the concise view.
    assert "catalog" in status["response"]["toolCalls"][0]["result"]["details"]
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    entry = next(e for e in _CATALOG.raw_data["entries"] if e["id"] == "io.set_output")
    required = {row["capability_id"] for row in entry["required_capabilities"]}
    assert view == {
        "ok": True,
        "execution_available": True,
        "reason_codes": [],
        "nodes_available": ["io.set_output"],
        "nodes_unavailable": {"simulation.step": [
            "capability_not_offered (simulator.control.step)",
            "program_runtime_owner_unavailable"]},
        "capabilities_offered": sorted(required),
        "capabilities_not_offered": ["simulator.control.step"],
        "more": ("catalog_read for node parameters; the system prompt's Cell section "
                 "for targets, frames and signals"),
    }


def _single_tool_projection(tool_id: str, effect: str) -> dict[str, Any]:
    projection = _node_projection()
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": effect, "tool_id": tool_id}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    return _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])


def test_every_missing_required_field_is_reported_in_one_result(tmp_path: Path) -> None:
    projection = _single_tool_projection("blackboard_draft_upsert_keys", "composer_draft_mutating")
    seen: list = []
    calls = [("blackboard_draft_upsert_keys", {"keys": [{"key": "scratch.flag", "type": "bool"}]})]
    _run(tmp_path, projection, _scripted_upstream(calls, seen))
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert {row["path"] for row in view["errors"]} == {"keys[0].access", "keys[0].persistence"}


def test_scene_inspect_reads_the_draft_scene_from_composer(tmp_path: Path) -> None:
    projection = _single_tool_projection("scene_inspect", "read_only")
    resource = {"scene_resource_id": "draft_only", "shape": {"type": "box",
                "box_dimensions_m": [0.1, 0.1, 0.1]}, "pose": {
                "frame_ref": {"kind": "cell_frame"}, "position_m": [0, 0, 0],
                "orientation": {"xyzw": [0, 0, 0, 1]}}}
    forwarded: list = []
    seen: list = []
    composer = _forwarding_composer(forwarded, result={
        "kind": "composer_draft_read", "mutation": None, "details": {"scene": [resource]}})
    service = _service(tmp_path, projection,
                       upstream_handler=_scripted_upstream([("scene_inspect", {})], seen),
                       composer_handler=composer, node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        _terminal(client, "request.1")
    assert forwarded == [{}]
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["scene_resources"] == [resource]


def test_prompt_rules_scope_diagnostic_fixes_and_name_the_wrap_for_a_new_top() -> None:
    from manyforge_assistant_bridge.prompt import RULES

    assert any(rule.startswith("Fix mistakes in your own edits") and rule.endswith(
        "Never delete or rewrite the operator's nodes to clear diagnostics unless asked.")
        for rule in RULES)
    assert "To make a node the new top of the Program, wrap @root with tree_draft_wrap_node." in RULES


def _selector_facts() -> dict[str, Any]:
    def target(component: str, target_id: str) -> dict[str, Any]:
        return {"target": {"component_id": component, "command_target_id": target_id}}

    def frame(component: str, frame_id: str, ambiguous: bool = False) -> dict[str, Any]:
        return {"reference": {"component_id": component, "frame_id": frame_id},
                "ambiguous": ambiguous, "moving_targets": []}

    def signal(name: str, direction: str) -> dict[str, Any]:
        return {"reference": {"component_id": "cell_io", "signal_name": name},
                "ambiguous": False, "direction": direction, "value_type": "bool"}

    return {
        "components": [{"component_id": "ur10e_robot"}, {"component_id": "cell_io"},
                       {"component_id": "gripper"}],
        "targets": [target("ur10e_robot", "arm"), target("cell_io", "io"),
                    target("gripper", "gripper")],
        "signals": [signal("part_present", "input"), signal("zone_occupied", "input"),
                    signal("clamp_valve", "output")]
                   + [signal(f"spare_{index:02}", "input") for index in range(12)],
        "frames": [frame("ur10e_robot", "base"), frame("ur10e_robot", "flange"),
                   frame("ur10e_robot", "tool0", ambiguous=True)],
        "bodies": [],
        "rules": [
            {"rule_id": "zone_intrusion", "scope": "all_motion", "on_trip": "stop",
             "active_at_start": True,
             "condition": {"compare": {"op": "==", "value": True, "operand": {
                 "signal": {"controllable_component_id": "cell_io", "signal_name": "zone_occupied"}}}}},
            {"rule_id": "watch", "scope": {"execution_groups": ["left", "right"]},
             "on_trip": "notify", "active_at_start": False,
             "condition": {"any": [
                 {"not": {"compare": {"op": "==", "value": False, "operand": {"key": "flag"}}}},
                 {"compare": {"op": ">", "value": 2, "operand": {"key": "count"}}}]}},
        ],
    }


def test_prompt_cell_lines_name_the_candidate_ids_and_rules() -> None:
    from manyforge_assistant_bridge.prompt import _cell

    source_model = {"components": [
        {"component_id": "gripper", "mounted_on": {"component_id": "ur10e_robot", "interface_id": "tool_flange"}},
    ]}
    lines = _cell(source_model, _selector_facts())
    assert lines[1:] == [
        "- cell_io: targets io; inputs part_present, zone_occupied, spare_00, spare_01, spare_02, "
        "spare_03, spare_04, spare_05, spare_06, spare_07, spare_08, spare_09 +2 more; outputs clamp_valve",
        "- gripper: targets gripper; mounted on ur10e_robot.tool_flange",
        "- ur10e_robot: targets arm; frames base, flange",
        "Rules: zone_intrusion (stops all_motion while cell_io.zone_occupied; active at start), "
        "watch (notifies execution groups left+right while (not (not key flag) or key count > 2))",
    ]


def test_prompt_cell_without_selector_facts_keeps_the_source_model_line() -> None:
    from manyforge_assistant_bridge.prompt import _cell

    source_model = {"components": [{"component_id": "arm"}],
                    "targets": [{"component_id": "arm", "target_id": "motion",
                                 "frames": [{"frame_role_id": "tcp"}]}]}
    assert _cell(source_model)[1:] == ["- arm: target motion (frames tcp)"]


def test_diagnostics_split_against_the_draft_at_request_start() -> None:
    from types import SimpleNamespace

    from manyforge_assistant_bridge.bridge import (
        _DiagnosticsBaseline, _diagnostic_key, _model_result, _reference_row)

    missing = {"code": "component_reference_missing",
               "location": {"document": "program", "path": "/forest/0/parameters/target",
                            "node_instance_id": "attach", "parameter": "target"},
               "payload": {"component_id": "ghost"}}
    started = {"code": "command_target_missing",
               "location": {"document": "program", "path": "/forest/1/parameters/target",
                            "node_instance_id": "move", "parameter": "target"},
               "payload": {"component_id": "arm", "command_target_id": "flange"}}
    unit = {"code": "authoring_parameter_invalid", "path": "forest[0].parameters.max_effort",
            "message": "unit kind does not match the commanded position"}
    call = SimpleNamespace(tool_id="tree_draft_update_node_params", arguments={})

    def edit(authoring: list, reference: list | None) -> dict[str, Any]:
        payload: dict[str, Any] = {"kind": "composer_draft_mutation", "details": {},
                                   "diagnostics": authoring}
        if reference is not None:
            payload["referenceDiagnostics"] = reference
        return {"success": True, "result": payload}

    start_reference = [_reference_row(started)]
    state = _DiagnosticsBaseline(
        start=frozenset(_diagnostic_key(row) for row in start_reference),
        reference=start_reference)
    # First edit introduces two diagnostics; the request-start one is "other".
    first = _model_result(call, edit([unit], [started, missing]), state)
    assert first["diagnostics_new"] == [
        "forest[0].parameters.max_effort: unit kind does not match the commanded position",
        "/forest/0/parameters/target: component_reference_missing (component_id=ghost)"]
    assert first["diagnostics_other"] == 1
    # A clean second edit (or a no-op without reference rows) keeps them new.
    second = _model_result(call, edit([unit], None), state)
    assert second["diagnostics_new"] == first["diagnostics_new"]
    assert second["diagnostics_other"] == 1
    third = _model_result(call, edit([], [started]), state)
    assert third["diagnostics_new"] == [] and third["diagnostics_other"] == 1


def test_an_insert_under_root_alias_names_the_root_and_the_wrap() -> None:
    from manyforge_assistant_bridge.bridge import _model_result

    class Call:
        tool_id = "tree_draft_insert_node"
        arguments = {"parent_instance_id": "@root", "node": {}}

    view = _model_result(Call(), {"success": True, "result": {
        "kind": "composer_draft_mutation", "details": {
            "inserted_instance_id": "rep", "index": 12, "parent_instance_id": "pick_and_place"}}},
        set())
    assert view["hint"] == ("inserted as last child of pick_and_place; to put it elsewhere "
                            "use before_instance_id, after_instance_id or index")



def test_node_list_lines_name_the_parameters() -> None:
    from manyforge_assistant_bridge.prompt import _nodes, _parameter_names

    assert _parameter_names([{"id": "num_attempts", "default": 3}]) == "num_attempts?"
    many = [{"id": "target", "required": True}, *({"id": f"p{i}"} for i in range(7))]
    assert _parameter_names(many) == "target, p0?, p1?, p2?, p3?, p4?, …"
    catalog = {"entries": [{"id": "retry", "category": "control_flow",
                            "parameters": [{"id": "num_attempts", "default": 3}]}]}
    assert _nodes([{"node_id": "retry", "summary": "Runs its single child."}], catalog) == [
        "- control_flow:", "  - retry (num_attempts?): Runs its single child."]


def test_reference_diagnostics_name_the_valid_alternatives() -> None:
    from manyforge_assistant_bridge.bridge import _reference_row

    facts = json.loads(json.dumps({
        "components": [{"component_id": "ur10e_robot"}, {"component_id": "robotiq_2f_140"},
                       {"component_id": "cell_io"}],
        "targets": [{"target": {"component_id": "ur10e_robot", "command_target_id": "arm"}},
                    {"target": {"component_id": "robotiq_2f_140", "command_target_id": "gripper"}}],
        "frames": [{"reference": {"component_id": "ur10e_robot", "frame_id": "base"}},
                   {"reference": {"component_id": "ur10e_robot", "frame_id": "flange"}}],
        "signals": [{"reference": {"component_id": "cell_io", "signal_name": "part_present"},
                     "direction": "input"},
                    {"reference": {"component_id": "cell_io", "signal_name": "clamp_valve"},
                     "direction": "output"}],
        "bodies": [], "rules": []}))

    def message(code: str, payload: dict) -> str:
        return _reference_row({"code": code, "location": {"path": "/x"}, "payload": payload},
                              facts)["message"]

    assert message("command_target_missing", {"component_id": "ur10e_robot",
                                              "command_target_id": "flange"}).endswith(
        "; ur10e_robot has command targets: arm")
    assert message("component_reference_missing", {"component_id": "cell_frame"}).endswith(
        "; components: ur10e_robot, robotiq_2f_140, cell_io; "
        "for the cell frame use frame_ref kind cell_frame")
    assert message("frame_not_mounted", {"component_id": "ur10e_robot", "frame_id": "tool0"}
                   ).endswith("; frames of ur10e_robot: base, flange")
    assert message("signal_direction_mismatch", {"component_id": "cell_io",
                                                 "signal_name": "clamp_valve"}).endswith(
        "; signals of cell_io: part_present (input), clamp_valve (output)")
    many = {**facts, "components": [{"component_id": f"c{i}"} for i in range(13)]}
    text = _reference_row({"code": "component_reference_missing", "location": {"path": "/x"},
                           "payload": {"component_id": "ghost"}}, many)["message"]
    assert text.endswith("c9 +3 more")
    # Without facts the message is the code and payload only.
    assert _reference_row({"code": "component_reference_missing", "location": {"path": "/x"},
                           "payload": {"component_id": "ghost"}})["message"] == (
        "component_reference_missing (component_id=ghost)")



def test_a_missing_capability_names_the_targets_that_provide_it() -> None:
    from manyforge_assistant_bridge.bridge import _reference_row

    facts = {"targets": [
        {"target": {"component_id": "ur10e_robot", "command_target_id": "arm"},
         "capabilities": [{"capability_id": "state.observed.current", "available": True}]},
        {"target": {"component_id": "robotiq_2f_140", "command_target_id": "gripper"},
         "capabilities": [{"capability_id": "tool.gripper.command", "available": True}]}]}
    row = {"code": "capability_missing", "location": {"path": "/x"}, "payload": {
        "component_id": "robotiq_2f_140", "command_target_id": "gripper",
        "capability_id": "state.observed.current", "required_parameters": {}}}
    assert _reference_row(row, facts)["message"].endswith(
        "; state.observed.current is provided by: ur10e_robot/arm")
    row["payload"]["capability_id"] = "planning.motion.pose_goal"
    assert _reference_row(row, facts)["message"].endswith(
        "; no command target provides planning.motion.pose_goal")



def test_an_insert_beside_the_root_explains_the_second_top_level_node() -> None:
    from manyforge_assistant_bridge.bridge import _model_result

    class Call:
        tool_id = "tree_draft_insert_node"
        arguments = {"before_instance_id": "pick_and_place", "node": {}}

    view = _model_result(Call(), {"success": True, "result": {
        "kind": "composer_draft_mutation", "details": {
            "inserted_instance_id": "clear_valve", "index": 0, "parent_instance_id": None,
            "now_between": [None, "pick_and_place"]}}}, set())
    assert view["hint"] == (
        "clear_valve is now a second top-level node beside the root pick_and_place; "
        "to run it first in pick_and_place use parent_instance_id pick_and_place with index 0, "
        "or to put it above the root, wrap @root")
    # An anchor inside the tree gives no hint.
    Call.arguments = {"after_instance_id": "move_to_pick", "node": {}}
    inside = _model_result(Call(), {"success": True, "result": {
        "kind": "composer_draft_mutation", "details": {
            "inserted_instance_id": "x", "index": 2, "parent_instance_id": "pick_and_place"}}},
        set())
    assert "hint" not in inside


def test_forbidden_forms_get_generated_messages_naming_the_field() -> None:
    from jsonschema import Draft202012Validator
    from manyforge_assistant_bridge.node_catalog import collect_errors

    projection = _node_projection()
    schema = next(row["node_schema"] for row in projection["nodes"]
                  if row["node_id"] == "logic.check_condition")
    node = {"node_id": "logic.check_condition", "instance_id": "gate",
            "parameters": {"condition": True},
            "value_sources": {"condition": {"kind": "program_parameter",
                                            "reference": "enable_cycle"}}}
    rows = collect_errors(Draft202012Validator(schema), node, prefix="node")
    assert [(row.path, row.message) for row in rows if row.reason == "not"] == [
        ("node.value_sources.condition",
         "condition is set in both parameters and value_sources; set it in only one")]

    exclusive = {"allOf": [{"not": {"required": ["min_inclusive", "min_exclusive"]}}]}
    (row,) = collect_errors(Draft202012Validator(exclusive),
                            {"min_inclusive": 1, "min_exclusive": 0}, prefix="c")
    assert (row.path, row.message) == ("c.min_exclusive",
                                       "min_inclusive and min_exclusive cannot both be set")

    conditional = {"allOf": [{
        "if": {"not": {"required": ["kind"], "properties": {"kind": {"const": "blackboard_field"}}}},
        "then": {"not": {"required": ["field"]}}}]}
    (row,) = collect_errors(Draft202012Validator(conditional),
                            {"kind": "blackboard_key", "field": "x"}, prefix="source")
    assert (row.path, row.message) == ("source.field",
                                       "field is allowed only when kind is blackboard_field")

    alternatives = {"not": {"anyOf": [{"required": ["pose"]}, {"required": ["delta"]}]}}
    (row,) = collect_errors(Draft202012Validator(alternatives), {"delta": 1}, prefix="edit")
    assert (row.path, row.message) == ("edit.delta", "none of pose, delta may be set here")


def test_an_oversized_read_result_is_a_tool_result_the_model_can_narrow(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    import manyforge_assistant_bridge.bridge as bridge_module

    monkeypatch.setattr(bridge_module, "MAXIMUM_CALLBACK_RESPONSE_BYTES", 200)
    seen: list = []
    status = _run(tmp_path, _node_projection(), _scripted_upstream(
        [("catalog_read", {"include_parameters": True})], seen))
    assert status["state"] == "completed"
    view = json.loads([m for m in seen[1]["messages"] if m["role"] == "tool"][0]["content"])
    assert view["ok"] is False
    assert view["errors"][0]["code"] == "assistant_owner_response_oversized"


def test_diagnostics_name_the_node_their_path_points_into() -> None:
    from manyforge_assistant_bridge.bridge import _diagnostic_text, _reference_row

    authoring = {"code": "x", "path": "forest[0].children[2].children[0].value_sources.velocity_scale",
                 "message": "must default to the Program velocity_scale parameter",
                 "node": {"instance_id": "move_to_pick", "node_id": "motion.move_to_pose",
                          "path": "value_sources.velocity_scale"}}
    assert _diagnostic_text(authoring) == (
        "move_to_pick (motion.move_to_pose): value_sources.velocity_scale: must default to "
        "the Program velocity_scale parameter "
        "[forest[0].children[2].children[0].value_sources.velocity_scale]")
    reference = {"code": "component_reference_missing",
                 "location": {"path": "/forest/0/children/1/parameters/target"},
                 "payload": {"component_id": "ghost"},
                 "node": {"instance_id": "b", "node_id": "timer", "path": "parameters.target"}}
    assert _diagnostic_text(_reference_row(reference)) == (
        "b (timer): parameters.target: component_reference_missing (component_id=ghost) "
        "[/forest/0/children/1/parameters/target]")
    assert _diagnostic_text({"path": "forest", "message": "two roots"}) == "forest: two roots"


def test_building_and_one_child_hints() -> None:
    from types import SimpleNamespace

    from manyforge_assistant_bridge.bridge import _DiagnosticsBaseline, _model_result

    insert = SimpleNamespace(tool_id="tree_draft_insert_node", arguments={"parent_instance_id": "p"})
    empty_retry = {"success": True, "result": {
        "kind": "composer_draft_mutation",
        "details": {"inserted_instance_id": "retry_clamp", "index": 0, "parent_instance_id": "p"},
        "diagnostics": [{"code": "authoring_node_children_incomplete",
                         "path": "forest[0].children[0].children",
                         "message": "Requires at least 1 children; found 0.",
                         "node": {"instance_id": "retry_clamp", "node_id": "retry",
                                  "path": "children"}}]}}
    view = _model_result(insert, empty_retry, _DiagnosticsBaseline(frozenset(), []))
    assert view["diagnostics_new"][0].endswith(
        "; expected while building: insert its children next with parent_instance_id retry_clamp")

    move = SimpleNamespace(tool_id="tree_draft_move_node", arguments={})
    refused = {"success": False, "result": {
        "kind": "validation_error", "message": "retry_clamp accepts at most 1 children",
        "detail": {"code": "composer_authoring_child_capacity_exceeded",
                   "path": "forest[0].children[0].children"}}}
    view = _model_result(move, refused, _DiagnosticsBaseline(frozenset(), []))
    assert view["errors"][0]["message"] == (
        "retry_clamp accepts at most 1 children; to run several steps under retry_clamp, "
        "put them in a sequence and make that its child")



def test_a_component_id_that_names_a_member_says_whose_it_is() -> None:
    from manyforge_assistant_bridge.bridge import _reference_row

    facts = {"components": [{"component_id": "cell_io"}, {"component_id": "ur10e_robot"}],
             "signals": [{"reference": {"component_id": "cell_io", "signal_name": "clamp_valve"},
                          "direction": "output"}],
             "targets": [{"target": {"component_id": "ur10e_robot", "command_target_id": "arm"}}],
             "frames": [{"reference": {"component_id": "ur10e_robot", "frame_id": "flange"}}],
             "bodies": []}

    def message(component: str) -> str:
        return _reference_row({"code": "component_reference_missing", "location": {"path": "/x"},
                               "payload": {"component_id": component}}, facts)["message"]

    assert message("clamp_valve").endswith(
        "; clamp_valve is a signal of cell_io (output): use component_id cell_io, "
        "signal_name clamp_valve; components: cell_io, ur10e_robot")
    assert "flange is a frame of ur10e_robot: use component_id ur10e_robot, frame_id flange" in (
        message("flange"))
    assert message("ghost").endswith("; components: cell_io, ur10e_robot")


# OD-93: the last verified projection pair is kept by its Headless identity.


def _verify_counts(monkeypatch) -> dict[str, int]:
    from manyforge_assistant_bridge import bridge as bridge_module

    counts = {"verify": 0, "resolve": 0}

    def counted(name: str, function):
        def wrapper(*args, **kwargs):
            counts[name] += 1
            return function(*args, **kwargs)

        return wrapper

    monkeypatch.setattr(
        bridge_module,
        "verify_active_mode_projection",
        counted("verify", bridge_module.verify_active_mode_projection),
    )
    monkeypatch.setattr(
        bridge_module, "resolve_catalog", counted("resolve", bridge_module.resolve_catalog)
    )
    return counts


def _bridge(tmp_path: Path, projection: dict[str, Any]) -> BridgeService:
    async def unused(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no upstream or Composer call is expected")

    return _service(
        tmp_path, projection, upstream_handler=unused, composer_handler=unused
    )


def _provider_request(projection: dict[str, Any]):
    from manyforge_assistant_bridge.contracts import ProviderRequest

    return ProviderRequest.model_validate(_request(projection))


def test_unchanged_identity_reuses_the_verified_projection(
    tmp_path: Path, monkeypatch
) -> None:
    counts = _verify_counts(monkeypatch)
    projection = _projection()
    service = _bridge(tmp_path, projection)
    request = _provider_request(projection)

    async def run() -> list[Any]:
        return [await service._verify_request(request) for _ in range(3)]

    results = asyncio.run(run())
    assert service._headless.assistant_mode_reads == 3
    assert counts == {"verify": 1, "resolve": 1}
    assert all(result.catalog is results[0].catalog for result in results)
    assert results[-1].active_projection["source"]["source_content_digest"] == (
        projection["source"]["source_content_digest"]
    )


def test_changed_identity_reverifies_and_refuses_the_old_forwarded_projection(
    tmp_path: Path, monkeypatch
) -> None:
    from manyforge_assistant_bridge.bridge import BridgeRunError

    counts = _verify_counts(monkeypatch)
    projection = _projection()
    service = _bridge(tmp_path, projection)
    old_request = _provider_request(projection)
    asyncio.run(service._verify_request(old_request))

    # An Apply: Headless now serves different content (a new digest).
    applied = deepcopy(projection)
    applied["description"] = "Draft authoring after Apply"
    _rehash(applied)
    service._headless.projection = applied
    with pytest.raises(BridgeRunError) as stale:
        asyncio.run(service._verify_request(old_request))
    assert stale.value.code == "assistant_projection_stale"
    assert counts["verify"] == 2

    verified = asyncio.run(service._verify_request(_provider_request(applied)))
    assert verified.active_projection["description"] == "Draft authoring after Apply"
    assert counts == {"verify": 2, "resolve": 2}


def test_cached_identity_still_refuses_a_stale_forwarded_request(
    tmp_path: Path, monkeypatch
) -> None:
    from manyforge_assistant_bridge.bridge import BridgeRunError

    counts = _verify_counts(monkeypatch)
    projection = _projection()
    service = _bridge(tmp_path, projection)
    asyncio.run(service._verify_request(_provider_request(projection)))

    forwarded = deepcopy(projection)
    forwarded["description"] = "an older Composer view"
    _rehash(forwarded)
    with pytest.raises(BridgeRunError) as stale:
        asyncio.run(service._verify_request(_provider_request(forwarded)))
    assert stale.value.code == "assistant_projection_stale"

    mismatched = _request(projection)
    mismatched["resolvedCatalog"]["catalogHash"] = "sha256:" + "0" * 64
    from manyforge_assistant_bridge.contracts import ProviderRequest

    with pytest.raises(BridgeRunError) as mismatch:
        asyncio.run(
            service._verify_request(ProviderRequest.model_validate(mismatched))
        )
    assert mismatch.value.code == "assistant_catalog_mismatch"
    # Both refusals came from the cached verification (304 reads).
    assert counts == {"verify": 1, "resolve": 1}


def test_headless_restart_with_the_same_content_keeps_the_verification(
    tmp_path: Path, monkeypatch
) -> None:
    counts = _verify_counts(monkeypatch)
    projection = _projection()
    service = _bridge(tmp_path, projection)
    request = _provider_request(projection)
    asyncio.run(service._verify_request(request))

    # A fresh Headless: new owner epoch and generations, same content digest.
    restarted = deepcopy(projection)
    restarted["source"]["projection_owner_epoch"] = "headless.epoch.2"
    restarted["source"]["source_owner_epoch"] = "headless.epoch.2"
    restarted["source"]["projection_generation"] = 1
    restarted["source"]["receiver_age_ns"] = 99
    service._headless = _Headless(restarted)
    verified = asyncio.run(service._verify_request(request))
    assert counts == {"verify": 1, "resolve": 1}
    assert verified.active_projection["description"] == projection["description"]

    # Content changed across the restart: a different tag, a full verify.
    changed = deepcopy(restarted)
    changed["description"] = "changed while down"
    _rehash(changed)
    service._headless = _Headless(changed)
    asyncio.run(service._verify_request(_provider_request(changed)))
    assert counts["verify"] == 2


def test_stale_source_is_never_served_from_the_verified_slot(
    tmp_path: Path, monkeypatch
) -> None:
    from manyforge_assistant_bridge.bridge import BridgeRunError

    _verify_counts(monkeypatch)
    projection = _projection()
    service = _bridge(tmp_path, projection)
    request = _provider_request(projection)
    asyncio.run(service._verify_request(request))

    service._headless.projection["source"]["freshness"] = "stale"
    with pytest.raises(BridgeRunError) as unavailable:
        asyncio.run(service._verify_request(request))
    assert unavailable.value.code == "assistant_projection_unavailable"


def _repeat_edits(tmp_path: Path, *, stuck_loop_recovery: bool):
    seen: list[dict[str, Any]] = []
    forwarded: list = []
    edit = {"instance_id": "wait", "expected_node_id": "timer",
            "parameters": {"duration_s": 2.0}}
    projection = _node_projection(stuck_loop_recovery=stuck_loop_recovery)
    native = {k: v for k, v in projection.items()
              if k not in ("catalog_hash", "instrument_registry_digest", "nodes", "types")}
    native["tools"] = [{"effect": "composer_draft_mutating",
                        "tool_id": "tree_draft_update_node_params"}]
    native["source"] = {k: v for k, v in projection["source"].items()
                        if k != "source_content_digest"}
    native["stuck_loop_recovery"] = stuck_loop_recovery
    projection = _INSTRUMENTS.compose_mode(native, [r["node_id"] for r in projection["nodes"]])
    calls = [("tree_draft_update_node_params", edit)] * 6
    service = _service(tmp_path, projection, upstream_handler=_scripted_upstream(calls, seen),
                       composer_handler=_forwarding_composer(forwarded),
                       node_catalog=_CATALOG.raw_data)
    with TestClient(create_app(service.config, service=service)) as client:
        assert client.post("/v1/manyforge/assistant", json=_request(projection)).status_code == 202
        status = _terminal(client, "request.1")
    return status, seen


def test_repeated_identical_successful_edits_get_a_hint_then_a_clarification(
    tmp_path: Path,
) -> None:
    status, seen = _repeat_edits(tmp_path, stuck_loop_recovery=True)
    response = status["response"]
    assert response["exitReason"] == "needs_clarification"
    assert len(response["toolCalls"]) == 5
    assert all(call["status"] == "completed" for call in response["toolCalls"])
    results = _tool_messages(seen)
    assert "recovery" not in results[0]
    assert results[1]["recovery"]["errorClass"] == "repeated_identical_edit"
    assert "already succeeded" in results[1]["recovery"]["action"]
    assert "repeated the same edit 5 times" in response["message"]


def test_repeated_identical_edits_are_not_counted_with_recovery_off(tmp_path: Path) -> None:
    status, seen = _repeat_edits(tmp_path, stuck_loop_recovery=False)
    assert status["response"]["exitReason"] == "ok"
    assert all("recovery" not in row for row in _tool_messages(seen))


def _nested_condition(value: Any = True) -> dict[str, Any]:
    signal = {"signal": {"component_id": "gripper", "signal_name": "part_present"}}
    compare = {"compare": {"operand": signal, "op": "==", "value": value}}
    return {"any": [compare, {"all": [compare, {"not": compare}]}]}


def test_a_nested_interrupt_condition_validates_inside_the_node_schema() -> None:
    from jsonschema import Draft202012Validator
    from manyforge_assistant_bridge.node_catalog import NodeCallChecker

    projection = _node_projection()
    checker = NodeCallChecker(projection["nodes"], _CATALOG.raw_data, Draft202012Validator)

    def insert(condition: Any) -> list[Any]:
        return checker.check("tree_draft_insert_node", {"node": {
            "node_id": "motion.move_until_signal", "instance_id": "seek",
            "parameters": {"interrupt_condition": condition}, "children": []}})

    assert insert(_nested_condition()) == []
    # An invalid nested condition is refused with its field path.
    bad = _nested_condition()
    bad["any"][1]["all"][1]["not"]["compare"]["op"] = "~"
    errors = insert(bad)
    assert errors and all(row.path.startswith(
        "node.parameters.interrupt_condition") for row in errors)
    assert any("any[1]" in row.path for row in errors)



def test_forbidden_forms_inside_a_referenced_value_source_schema() -> None:
    from jsonschema import Draft202012Validator
    from manyforge_assistant_bridge.node_catalog import collect_errors

    projection = _node_projection()
    schema = next(row["update_schema"] for row in projection["nodes"]
                  if row["node_id"] == "geometry.check_pose_bounds")
    arguments = {
        "expected_node_id": "geometry.check_pose_bounds", "instance_id": "check_box_in_table",
        "merge": True,
        "parameters": {"pose": {"frame_ref": {"kind": "cell_frame"},
                                "orientation": {"rpy_deg": [0, 0, 0]},
                                "position_m": [0, 0, 0]}},
        "value_sources": {"pose": {"field": "box_pose", "kind": "blackboard_key",
                                   "reference": "box_pose"}},
    }
    rows = {(row.path, row.message) for row in
            collect_errors(Draft202012Validator(schema), arguments) if row.reason == "not"}
    assert ("value_sources.pose", "pose is set in both parameters and value_sources; "
            "set it in only one") in rows
    assert ("value_sources.pose.field", "field is allowed only when kind is blackboard_field") in rows
