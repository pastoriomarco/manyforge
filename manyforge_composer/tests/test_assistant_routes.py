"""Tests for Composer assistant routes."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.assistant_provider import (
    AssistantProviderError,
    AssistantProviderResult,
    DeterministicSceneAuthoringProvider,
    NemoClawAssistantProvider,
    build_assistant_provider,
)
from manyforge_composer.backend import routes_assistant
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_ISAAC_SCENE_FIXTURE = (
    Path(__file__).resolve().parent / "fixtures" / "isaac_scene_authoring_soup_can.json"
)
_DEPLOYMENT_FIXTURE = (
    _REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml"
)
_SEMANTIC_DEPLOYMENT_FIXTURE = (
    _REPO_ROOT / "examples" / "semantic_shape_pick.deployment.yaml"
)
_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=["/workspace", "/tmp"],
)


@pytest.fixture
def state() -> ComposerState:
    return ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())


@pytest.fixture
def client(state: ComposerState) -> TestClient:
    return TestClient(create_app(state, security=_TEST_SECURITY))


def test_assistant_chat_returns_reviewable_mock_scene_proposal(
    client: TestClient,
    state: ComposerState,
) -> None:
    before_program = state.program
    before_generation = state.program_session_generation
    before_cycle_status = client.get("/api/cycle/status").json()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Inspect the Isaac scene and propose planning objects",
            "requestedTools": ["inspect_isaac_scene"],
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["requestId"].startswith("mf-")
    assert data["mode"] == "mock"
    assert data["mutated"] is False
    assert data["requiresReview"] is True
    assert data["programLoaded"] is False
    assert data["cycleState"] == "idle"
    assert data["toolCalls"] == []
    assert data["proposals"]
    proposal = data["proposals"][0]
    assert proposal["type"] == "scene_authoring_review"
    assert proposal["status"] == "draft"
    assert proposal["reviewRequired"] is True
    assert proposal["payload"]["recommendedTool"] == "inspect_isaac_scene"

    assert state.program is before_program
    assert state.program_session_generation == before_generation
    assert client.get("/api/cycle/status").json() == before_cycle_status


def test_assistant_chat_generic_request_does_not_call_tools(
    client: TestClient,
) -> None:
    response = client.post(
        "/api/assistant/chat",
        json={"message": "What can you do?"},
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "mock"
    assert data["toolCalls"] == []
    assert data["mutated"] is False
    assert data["proposals"][0]["type"] == "assistant_scaffold"


def test_assistant_chat_rejects_blank_message(client: TestClient) -> None:
    response = client.post(
        "/api/assistant/chat",
        json={"message": "   "},
    )

    assert response.status_code == 422


def test_assistant_chat_rejects_unsupported_mode(client: TestClient) -> None:
    response = client.post(
        "/api/assistant/chat",
        json={"message": "hello", "mode": "model"},
    )

    assert response.status_code == 422


def test_assistant_chat_provider_mode_requires_configured_provider(client: TestClient) -> None:
    response = client.post(
        "/api/assistant/chat",
        json={"message": "hello", "mode": "provider"},
    )

    assert response.status_code == 503
    assert response.json()["detail"] == "No assistant provider is configured"


def test_assistant_chat_provider_mode_uses_configured_provider(
    client: TestClient,
    state: ComposerState,
) -> None:
    class FakeProvider:
        provider_id = "fake-nemoclaw"

        def run(self, request, context):
            assert context.tool_catalog
            assert context.program_loaded is False
            assert context.cycle_state == "idle"
            return AssistantProviderResult(
                mode="provider",
                message="fake provider response",
                proposals=[
                    {
                        "id": "proposal-provider-scene-review",
                        "type": "scene_authoring_review",
                        "title": "Review simulator scene",
                        "summary": "Provider-produced review proposal.",
                        "status": "draft",
                        "reviewRequired": True,
                        "payload": {"requestedMessage": request.message},
                        "warnings": [],
                        "source": {"kind": "fake_provider"},
                    }
                ],
                tool_calls=[
                    {
                        "name": "inspect_isaac_scene",
                        "status": "proposed",
                        "arguments": {"includeSubtrees": ["/World"]},
                    }
                ],
                warnings=["fake provider only"],
                mutated=False,
                requires_review=True,
            )

    state.assistant_provider = FakeProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "inspect scene",
            "mode": "provider",
            "providerId": "fake-nemoclaw",
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "provider"
    assert data["providerId"] == "fake-nemoclaw"
    assert data["message"] == "fake provider response"
    assert data["toolCalls"][0]["name"] == "inspect_isaac_scene"
    assert data["toolCalls"][0]["status"] == "proposed"
    assert data["proposals"][0]["source"]["kind"] == "fake_provider"
    assert data["mutated"] is False


def test_assistant_chat_rejects_unconfigured_provider_id(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "inspect scene",
            "mode": "provider",
            "providerId": "nemoclaw",
        },
    )

    assert response.status_code == 404
    assert response.json()["detail"] == "Requested assistant provider is not configured"


def test_assistant_chat_provider_mode_uses_deployment_assistant_config(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = Deployment(_SEMANTIC_DEPLOYMENT_FIXTURE)
    state.deployment_source_path = str(_SEMANTIC_DEPLOYMENT_FIXTURE)

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "inspect scene",
            "mode": "provider",
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "provider"
    assert data["providerId"] == "deterministic-scene-authoring"
    assert data["mutated"] is False
    assert data["requiresReview"] is True


def test_assistant_chat_provider_mode_requires_assistant_mode_for_mode_deployment(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "inspect scene",
            "mode": "provider",
        },
    )

    assert response.status_code == 400
    assert "assistantMode is required" in response.json()["detail"]


def test_nemoclaw_provider_sends_v01_assistant_mode_catalog(
    client: TestClient,
    state: ComposerState,
) -> None:
    payloads: list[dict] = []

    def fake_request(payload: dict) -> dict:
        payloads.append(payload)
        return {
            "message": "Program status is available.",
            "toolCalls": [
                {
                    "name": "program.read",
                    "status": "completed",
                    "arguments": {},
                    "result": {"ok": True},
                }
            ],
            "proposals": [],
            "warnings": [],
            "mutated": False,
            "requiresReview": True,
        }

    state.deployment = _assistant_mode_deployment()
    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=fake_request,
    )

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "read the program",
            "mode": "provider",
            "providerId": "nemoclaw",
            "assistantMode": "query",
            "requestId": "req-v01-catalog",
            "requestedTools": ["program.read"],
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["requestId"] == "req-v01-catalog"
    assert data["assistantMode"] == "query"
    assert data["toolCalls"][0]["name"] == "program.read"
    payload = payloads[0]
    assert payload["version"] == "manyforge.assistant.provider_request.v0"
    assert payload["requestId"] == "req-v01-catalog"
    assert payload["schemaVersion"] == "0.1.0"
    assert payload["assistantMode"] == "query"
    assert payload["catalog"] == {
        "skills": [],
        "tools": ["program.read", "program.validate", "status.read", "scene.inspect"],
        "nodes": [],
    }
    assert [tool["id"] for tool in payload["tools"]] == [
        "program.read",
        "program.validate",
        "status.read",
        "scene.inspect",
    ]
    assert payload["tools"][0]["effect"] == "read_only"
    assert payload["tools"][0]["description"] == "Read the loaded program."
    assert payload["tools"][0]["inputSchema"] == {
        "type": "object",
        "properties": {},
        "additionalProperties": False,
    }


def test_assistant_mode_rejects_requested_tools_outside_catalog(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "read the program",
            "mode": "provider",
            "assistantMode": "query",
            "requestedTools": ["tree.insert_node"],
        },
    )

    assert response.status_code == 400
    assert "outside assistantMode 'query'" in response.json()["detail"]


def test_assistant_bridge_read_only_callback_returns_program_state(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "query")

    response = client.post(
        "/api/assistant/bridge/tools/program.read",
        json={
            "requestId": "req-bridge",
            "conversationId": "conv-bridge",
            "assistantMode": "query",
            "catalogHash": catalog_hash,
            "arguments": {},
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolId"] == "program.read"
    assert data["success"] is True
    assert data["mutated"] is False
    assert data["result"]["programLoaded"] is False
    assert data["result"]["program"] is None


def test_assistant_bridge_read_only_callback_rejects_proposal_tools(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")

    response = client.post(
        "/api/assistant/bridge/tools/tree.insert_node",
        json={
            "requestId": "req-bridge",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {},
        },
    )

    assert response.status_code == 403
    assert "bridge callbacks may execute only" in response.json()["detail"]


def test_assistant_bridge_composer_draft_callback_applies_scene_resource(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    before_generation = state.program_session_generation
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")

    response = client.post(
        "/api/assistant/bridge/tools/scene.draft.add_object",
        json={
            "requestId": "req-bridge-draft",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {
                "sceneResource": _scene_resource_proposal()["payload"]["sceneResource"],
            },
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolId"] == "scene.draft.add_object"
    assert data["success"] is True
    assert data["mutated"] is True
    assert data["draftMutated"] is True
    assert data["result"]["draftMutation"] == "scene_resource_add"
    assert data["result"]["objectId"] == "sim_soup_can"
    assert state.program is not None
    assert [resource.object_id for resource in state.program.scene] == ["sim_soup_can"]
    assert state.program_session_generation == before_generation + 1


def test_assistant_bridge_composer_draft_callback_updates_scene_resource(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")
    resource = _scene_resource_proposal()["payload"]["sceneResource"]
    add_response = client.post(
        "/api/assistant/bridge/tools/scene.draft.add_object",
        json={
            "requestId": "req-bridge-add-before-update",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResource": resource},
        },
    )
    assert add_response.status_code == 200

    updated_resource = {
        **resource,
        "shape": {"type": "box", "box_dimensions_m": [0.08, 0.06, 0.12]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.2, -0.1, 0.06],
            "orientation_rpy_deg": [0.0, 0.0, 15.0],
        },
    }
    response = client.post(
        "/api/assistant/bridge/tools/scene.draft.update_object",
        json={
            "requestId": "req-bridge-update",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResource": updated_resource},
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolId"] == "scene.draft.update_object"
    assert data["success"] is True
    assert data["result"]["draftMutation"] == "scene_resource_update"
    assert data["result"]["updated"] == ["sim_soup_can"]
    assert state.program is not None
    assert state.program.scene[0].shape["box_dimensions_m"] == [0.08, 0.06, 0.12]


def test_assistant_bridge_composer_draft_callback_upserts_scene_resources(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")
    resource = _scene_resource_proposal()["payload"]["sceneResource"]
    assert client.post(
        "/api/assistant/bridge/tools/scene.draft.add_object",
        json={
            "requestId": "req-bridge-add-before-upsert",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResource": resource},
        },
    ).status_code == 200

    replacement = {
        **resource,
        "shape": {"type": "sphere", "sphere_radius_m": 0.04},
    }
    inserted = {
        "objectId": "sim_marker",
        "shape": {"type": "box", "box_dimensions_m": [0.03, 0.04, 0.05]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.3, 0.1, 0.025],
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
    }
    response = client.post(
        "/api/assistant/bridge/tools/scene.draft.upsert_objects",
        json={
            "requestId": "req-bridge-upsert",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResources": [replacement, inserted]},
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolId"] == "scene.draft.upsert_objects"
    assert data["success"] is True
    assert data["result"]["draftMutation"] == "scene_resources_upsert"
    assert data["result"]["updated"] == ["sim_soup_can"]
    assert data["result"]["inserted"] == ["sim_marker"]
    assert state.program is not None
    scene_by_id = {resource.object_id: resource for resource in state.program.scene}
    assert scene_by_id["sim_soup_can"].shape["type"] == "sphere"
    assert scene_by_id["sim_marker"].shape["box_dimensions_m"] == [0.03, 0.04, 0.05]


def test_assistant_bridge_composer_draft_callback_rejects_duplicate_upsert_ids(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")
    resource = _scene_resource_proposal()["payload"]["sceneResource"]

    response = client.post(
        "/api/assistant/bridge/tools/scene.draft.upsert_objects",
        json={
            "requestId": "req-bridge-upsert-duplicates",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResources": [resource, resource]},
        },
    )

    assert response.status_code == 400
    assert "duplicate objectId" in response.json()["detail"]


def test_assistant_bridge_discovery_callbacks_return_catalog_skills_and_capabilities(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")

    catalog_response = client.post(
        "/api/assistant/bridge/tools/catalog.read",
        json={
            "requestId": "req-catalog-read",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"entryIds": ["reset_trajectories"]},
        },
    )
    assert catalog_response.status_code == 200
    catalog_payload = catalog_response.json()["result"]
    assert catalog_payload["entryCount"] == 1
    assert catalog_payload["entries"][0]["id"] == "reset_trajectories"

    skills_response = client.post(
        "/api/assistant/bridge/tools/skills.read",
        json={
            "requestId": "req-skills-read",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"includeBodies": False},
        },
    )
    assert skills_response.status_code == 200
    assert skills_response.json()["result"]["skillCount"] > 0

    capabilities_response = client.post(
        "/api/assistant/bridge/tools/deployment.capabilities.read",
        json={
            "requestId": "req-capabilities-read",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {},
        },
    )
    assert capabilities_response.status_code == 200
    result = capabilities_response.json()["result"]
    assert "scene.edit.remove" in result["scene"]
    assert result["adapterIds"]["context"] == "context.test"


def test_assistant_bridge_composer_draft_callback_removes_scene_resources(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")
    resource = _scene_resource_proposal()["payload"]["sceneResource"]
    assert client.post(
        "/api/assistant/bridge/tools/scene.draft.add_object",
        json={
            "requestId": "req-add-before-remove",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"sceneResource": resource},
        },
    ).status_code == 200

    response = client.post(
        "/api/assistant/bridge/tools/scene.draft.remove_objects",
        json={
            "requestId": "req-remove-scene",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {"objectIds": ["sim_soup_can"]},
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolId"] == "scene.draft.remove_objects"
    assert data["result"]["draftMutation"] == "scene_resources_remove"
    assert data["result"]["removed"] == ["sim_soup_can"]
    assert state.program is not None
    assert state.program.scene == []


def test_assistant_bridge_composer_draft_tree_callbacks_mutate_validated_tree(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")

    insert_response = client.post(
        "/api/assistant/bridge/tools/tree.draft.insert_node",
        json={
            "requestId": "req-tree-insert",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {
                "parentName": "root",
                "index": 1,
                "node": {
                    "id": "remove_collision_object",
                    "name": "remove_temp_object",
                    "params": {"object_id": "temp_object"},
                },
            },
        },
    )
    assert insert_response.status_code == 200
    assert insert_response.json()["result"]["draftMutation"] == "tree_node_insert"

    update_response = client.post(
        "/api/assistant/bridge/tools/tree.draft.update_node_params",
        json={
            "requestId": "req-tree-update",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {
                "nodeName": "remove_temp_object",
                "params": {"object_id": "renamed_temp_object"},
            },
        },
    )
    assert update_response.status_code == 200
    assert state.program is not None
    assert state.program.tree.children[1].params["object_id"] == "renamed_temp_object"

    replace_response = client.post(
        "/api/assistant/bridge/tools/tree.draft.replace_subtree",
        json={
            "requestId": "req-tree-replace",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {
                "nodeName": "remove_temp_object",
                "subtree": {"id": "reset_trajectories", "name": "reset_replacement"},
            },
        },
    )
    assert replace_response.status_code == 200
    assert state.program.tree.children[1].name == "reset_replacement"

    move_response = client.post(
        "/api/assistant/bridge/tools/tree.draft.move_node",
        json={
            "requestId": "req-tree-move",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {
                "nodeName": "reset_replacement",
                "newParentName": "root",
                "index": 0,
            },
        },
    )
    assert move_response.status_code == 200
    assert state.program.tree.children[0].name == "reset_replacement"

    delete_response = client.post(
        "/api/assistant/bridge/tools/tree.draft.delete_node",
        json={
            "requestId": "req-tree-delete",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {"nodeName": "reset_replacement"},
        },
    )
    assert delete_response.status_code == 200
    assert [child.name for child in state.program.tree.children] == ["reset_trajectories"]


def test_assistant_bridge_composer_draft_parameter_and_blackboard_callbacks(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    catalog_hash = _assistant_catalog_hash(state, "composer-assistant")

    parameter_response = client.post(
        "/api/assistant/bridge/tools/program.draft.upsert_parameters",
        json={
            "requestId": "req-param-upsert",
            "assistantMode": "composer-assistant",
            "catalogHash": catalog_hash,
            "arguments": {
                "parameters": [
                    {
                        "name": "target_object",
                        "type": "string",
                        "default": "gear",
                        "description": "Object to manipulate.",
                    }
                ]
            },
        },
    )
    assert parameter_response.status_code == 200
    assert "target_object" in state.program.parameters

    namespace_response = client.post(
        "/api/assistant/bridge/tools/blackboard.draft.upsert_namespaces",
        json={
            "requestId": "req-bb-ns-upsert",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {
                "namespaces": [
                    {
                        "id": "selection",
                        "contract": "selection_context_v1",
                        "prefix": "/selection",
                        "fields": [{"name": "object_id", "type": "string"}],
                    }
                ]
            },
        },
    )
    assert namespace_response.status_code == 200
    assert "selection" in state.program.blackboard_namespaces

    key_response = client.post(
        "/api/assistant/bridge/tools/blackboard.draft.upsert_keys",
        json={
            "requestId": "req-bb-key-upsert",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {
                "keys": [{"id": "ready_flag", "type": "bool", "key": "/ready"}]
            },
        },
    )
    assert key_response.status_code == 200
    assert "ready_flag" in state.program.blackboard_keys

    remove_key_response = client.post(
        "/api/assistant/bridge/tools/blackboard.draft.remove_keys",
        json={
            "requestId": "req-bb-key-remove",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {"ids": ["ready_flag"]},
        },
    )
    assert remove_key_response.status_code == 200
    assert "ready_flag" not in state.program.blackboard_keys

    remove_namespace_response = client.post(
        "/api/assistant/bridge/tools/blackboard.draft.remove_namespaces",
        json={
            "requestId": "req-bb-ns-remove",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {"ids": ["selection"]},
        },
    )
    assert remove_namespace_response.status_code == 200
    assert "selection" not in state.program.blackboard_namespaces

    remove_parameter_response = client.post(
        "/api/assistant/bridge/tools/program.draft.remove_parameters",
        json={
            "requestId": "req-param-remove",
            "assistantMode": "composer-assistant",
            "catalogHash": _assistant_catalog_hash(state, "composer-assistant"),
            "arguments": {"names": ["target_object"]},
        },
    )
    assert remove_parameter_response.status_code == 200
    assert "target_object" not in state.program.parameters


def test_assistant_bridge_read_only_callback_rejects_stale_catalog_hash(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()

    response = client.post(
        "/api/assistant/bridge/tools/program.read",
        json={
            "requestId": "req-bridge",
            "assistantMode": "query",
            "catalogHash": "not-the-current-hash",
            "arguments": {},
        },
    )

    assert response.status_code == 409
    assert "catalogHash" in response.json()["detail"]


def test_nemoclaw_provider_rejects_out_of_catalog_tool_response(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.deployment = _assistant_mode_deployment()
    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=lambda payload: {
            "message": "I tried an unavailable tool.",
            "toolCalls": [
                {
                    "name": "tree.insert_node",
                    "status": "proposed",
                    "arguments": {},
                }
            ],
            "mutated": False,
        },
    )

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "read the program",
            "mode": "provider",
            "assistantMode": "query",
        },
    )

    assert response.status_code == 502
    data = response.json()
    assert data["error"]["code"] == "provider_error"
    assert "outside assistantMode 'query'" in data["error"]["detail"]


def test_assistant_tools_catalog_is_review_only(client: TestClient) -> None:
    response = client.get("/api/assistant/tools")

    assert response.status_code == 200
    data = response.json()
    names = {tool["name"] for tool in data["tools"]}
    assert {
        "inspect_isaac_scene",
        "propose_scene_objects",
        "propose_scene_object_nodes",
    } <= names
    for tool in data["tools"]:
        assert tool["mutatesState"] is False
        assert tool["requiresReview"] is True


def test_assistant_providers_catalog_marks_configured_provider(
    client: TestClient,
    state: ComposerState,
) -> None:
    response = client.get("/api/assistant/providers")

    assert response.status_code == 200
    data = response.json()
    assert data["configuredProviderId"] == "mock"
    assert any(provider["id"] == "mock" and provider["configured"] for provider in data["providers"])

    state.deployment = Deployment(_SEMANTIC_DEPLOYMENT_FIXTURE)
    response = client.get("/api/assistant/providers")
    assert response.status_code == 200
    data = response.json()
    assert data["configuredProviderId"] == "deterministic-scene-authoring"
    state.deployment = None

    state.assistant_provider = DeterministicSceneAuthoringProvider()
    response = client.get("/api/assistant/providers")
    assert response.status_code == 200
    data = response.json()
    assert data["configuredProviderId"] == "deterministic-scene-authoring"
    deterministic = next(
        provider for provider in data["providers"]
        if provider["id"] == "deterministic-scene-authoring"
    )
    assert deterministic["configured"] is True
    assert deterministic["requiresLiveModel"] is False
    assert deterministic["requiresLiveIsaacSim"] is False

    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=lambda payload: {"message": "unused"},
    )
    response = client.get("/api/assistant/providers")
    assert response.status_code == 200
    data = response.json()
    assert data["configuredProviderId"] == "nemoclaw"
    nemoclaw = next(provider for provider in data["providers"] if provider["id"] == "nemoclaw")
    assert nemoclaw["configured"] is True
    assert nemoclaw["requiresLiveModel"] is True
    assert nemoclaw["requiresLiveIsaacSim"] is False


def test_build_nemoclaw_provider_requires_endpoint(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("MANYFORGE_ASSISTANT_ENDPOINT_URL", raising=False)
    monkeypatch.delenv("MANYFORGE_NEMOCLAW_ASSISTANT_URL", raising=False)
    monkeypatch.delenv("MANYFORGE_OPENCLAW_ASSISTANT_URL", raising=False)

    with pytest.raises(AssistantProviderError, match="MANYFORGE_ASSISTANT_ENDPOINT_URL"):
        build_assistant_provider("nemoclaw")

    monkeypatch.setenv("MANYFORGE_ASSISTANT_ENDPOINT_URL", "http://nemoclaw.local/assistant")
    monkeypatch.setenv("MANYFORGE_ASSISTANT_TIMEOUT_S", "12.5")
    provider = build_assistant_provider("nemoclaw")
    assert isinstance(provider, NemoClawAssistantProvider)
    assert provider.endpoint_url == "http://nemoclaw.local/assistant"


def test_nemoclaw_provider_sends_contract_and_normalizes_response(
    client: TestClient,
    state: ComposerState,
) -> None:
    payloads: list[dict] = []
    resource = _scene_resource_proposal()["payload"]["sceneResource"]

    def fake_request(payload: dict) -> dict:
        payloads.append(payload)
        return {
            "message": "Prepared a reviewed planning object proposal.",
            "toolCalls": [
                {
                    "name": "inspect_isaac_scene",
                    "status": "completed",
                    "arguments": {"includeSubtrees": ["/World/soup_can"]},
                    "result": {"objectCount": 1},
                }
            ],
            "proposals": [
                {
                    "id": "proposal-nemoclaw-soup-can",
                    "type": "scene_resource_add",
                    "title": "Add soup can planning object",
                    "summary": "Provider-produced scene resource proposal.",
                    "status": "applied",
                    "reviewRequired": False,
                    "payload": {
                        "action": "add_scene_resource",
                        "sceneResource": resource,
                        "sourceRoot": "/World/soup_can",
                    },
                    "warnings": ["provider approximation"],
                    "source": {"kind": "nemoclaw", "model": "fake"},
                }
            ],
            "warnings": ["provider warning"],
            "mutated": False,
            "requiresReview": False,
        }

    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=fake_request,
    )

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Create a planning object from the current scene",
            "mode": "provider",
            "providerId": "nemoclaw",
            "requestedTools": ["inspect_isaac_scene", "propose_scene_objects"],
            "context": {"sourceRoots": ["/World/soup_can"]},
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "provider"
    assert data["providerId"] == "nemoclaw"
    assert data["mutated"] is False
    assert data["requiresReview"] is True
    assert data["toolCalls"][0]["name"] == "inspect_isaac_scene"
    assert data["toolCalls"][0]["status"] == "completed"
    proposal = data["proposals"][0]
    assert proposal["id"] == "proposal-nemoclaw-soup-can"
    assert proposal["status"] == "draft"
    assert proposal["reviewRequired"] is True
    assert proposal["source"] == {"kind": "nemoclaw", "model": "fake"}
    assert proposal["payload"]["sceneResource"] == resource
    assert "Provider attempted to mark proposals as not requiring review" in data["warnings"][-1]

    assert len(payloads) == 1
    payload = payloads[0]
    assert payload["version"] == "manyforge.assistant.provider_request.v0"
    assert payload["conversationId"] == data["conversationId"]
    assert payload["message"] == "Create a planning object from the current scene"
    assert payload["requestedTools"] == ["inspect_isaac_scene", "propose_scene_objects"]
    assert payload["context"] == {"sourceRoots": ["/World/soup_can"]}
    assert payload["runtime"] == {"programLoaded": False, "cycleState": "idle"}
    assert payload["constraints"]["mutatesState"] is False
    assert payload["constraints"]["requiresReview"] is True
    assert payload["tools"]


def test_nemoclaw_provider_rejects_mutating_response(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=lambda payload: {
            "message": "I changed the program.",
            "mutated": True,
        },
    )

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Create a planning object",
            "mode": "provider",
            "providerId": "nemoclaw",
        },
    )

    assert response.status_code == 502
    data = response.json()
    assert data["error"]["code"] == "provider_error"
    assert "must not report state mutation" in data["error"]["detail"]


def test_assistant_chat_records_success_status(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "requestId": "req-status-ok",
            "message": "Inspect Isaac and propose planning objects",
            "mode": "provider",
        },
    )

    assert response.status_code == 200
    status_response = client.get("/api/assistant/requests/req-status-ok")
    assert status_response.status_code == 200
    status = status_response.json()
    assert status["requestId"] == "req-status-ok"
    assert status["status"] == "completed"
    assert status["providerId"] == "deterministic-scene-authoring"
    assert status["toolCallCount"] == 1
    assert status["error"] is None


def test_assistant_chat_records_draft_mutation_status(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    state.deployment = _assistant_mode_deployment()
    before_generation = state.program_session_generation

    class DraftMutatingProvider:
        provider_id = "nemoclaw"

        def run(self, request, context):
            resource = {
                "objectId": "ui_demo_box",
                "shape": {"type": "box", "box_dimensions_m": [0.04, 0.05, 0.06]},
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.42, -0.12, 0.03],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            }
            result = routes_assistant._apply_scene_draft_resources_upsert(
                state,
                arguments={"sceneResources": [resource]},
            )
            return AssistantProviderResult(
                mode="provider",
                message="Added ui_demo_box to the Composer draft.",
                tool_calls=[
                    {
                        "name": "scene.draft.upsert_objects",
                        "status": "completed",
                        "arguments": {"sceneResources": [resource]},
                        "result": result,
                    }
                ],
                proposals=[],
                draft_mutated=True,
                mutated=False,
                requires_review=True,
            )

    state.assistant_provider = DraftMutatingProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "requestId": "req-draft-mutated",
            "message": "add a UI demo box",
            "mode": "provider",
            "providerId": "nemoclaw",
            "assistantMode": "composer-assistant",
            "requestedTools": ["scene.draft.upsert_objects"],
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["draftMutated"] is True
    assert data["toolCalls"][0]["name"] == "scene.draft.upsert_objects"
    assert data["toolCalls"][0]["status"] == "completed"
    assert data["toolCalls"][0]["result"]["draftMutation"] == "scene_resources_upsert"
    assert state.program is not None
    assert [resource.object_id for resource in state.program.scene] == ["ui_demo_box"]
    assert state.program_session_generation == before_generation + 1

    status_response = client.get("/api/assistant/requests/req-draft-mutated")
    assert status_response.status_code == 200
    status = status_response.json()
    assert status["status"] == "completed"
    assert status["providerId"] == "nemoclaw"
    assert status["assistantMode"] == "composer-assistant"
    assert status["draftMutated"] is True
    assert status["toolCallCount"] == 1
    assert status["error"] is None


def test_assistant_chat_returns_structured_provider_error_status(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        request_fn=lambda payload: {
            "message": None,
            "error": {
                "code": "response_rejected",
                "detail": "out-of-catalog action requested",
            },
            "toolCalls": [],
            "proposals": [],
            "mutated": False,
        },
    )

    response = client.post(
        "/api/assistant/chat",
        json={
            "requestId": "req-status-fail",
            "message": "read the program",
            "mode": "provider",
            "providerId": "nemoclaw",
        },
    )

    assert response.status_code == 502
    data = response.json()
    assert data["requestId"] == "req-status-fail"
    assert data["error"] == {
        "code": "response_rejected",
        "detail": "out-of-catalog action requested",
    }
    status_response = client.get("/api/assistant/requests/req-status-fail")
    assert status_response.status_code == 200
    status = status_response.json()
    assert status["status"] == "failed"
    assert status["error"] == data["error"]


def test_assistant_cancel_route_proxies_to_configured_provider(
    client: TestClient,
    state: ComposerState,
) -> None:
    class CancellableProvider:
        provider_id = "fake-cancellable"

        def __init__(self) -> None:
            self.cancelled_request_id = ""

        def run(self, request, context):
            return AssistantProviderResult(
                mode="provider",
                message="queued",
                tool_calls=[],
                proposals=[],
            )

        def cancel(self, request_id: str) -> dict:
            self.cancelled_request_id = request_id
            return {"requestId": request_id, "cancelled": True, "detail": "queued"}

    provider = CancellableProvider()
    state.assistant_provider = provider
    state.start_assistant_request(
        request_id="req-cancel-proxy",
        conversation_id="conv-cancel",
        provider_id=provider.provider_id,
        assistant_mode="query",
    )

    response = client.post("/api/assistant/requests/req-cancel-proxy/cancel")

    assert response.status_code == 200
    data = response.json()
    assert data["requestId"] == "req-cancel-proxy"
    assert data["cancelled"] is True
    assert data["status"] == "cancel_requested"
    assert provider.cancelled_request_id == "req-cancel-proxy"
    status = client.get("/api/assistant/requests/req-cancel-proxy").json()
    assert status["status"] == "cancel_requested"


def test_assistant_cancel_route_does_not_mark_when_provider_lacks_cancel(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()
    state.start_assistant_request(
        request_id="req-cancel-unsupported",
        conversation_id="conv-cancel",
        provider_id="deterministic-scene-authoring",
    )

    response = client.post("/api/assistant/requests/req-cancel-unsupported/cancel")

    assert response.status_code == 200
    data = response.json()
    assert data["cancelled"] is False
    assert data["status"] == "running"
    assert "does not support cancellation" in data["detail"]
    status = client.get("/api/assistant/requests/req-cancel-unsupported").json()
    assert status["status"] == "running"


def test_deterministic_provider_calls_proposal_tool_without_live_services(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Create planning objects from this Isaac scene report",
            "mode": "provider",
            "providerId": "deterministic-scene-authoring",
            "context": {
                "isaacSceneReport": _sample_isaac_report(),
                "sourceRoots": ["/World/soup_can"],
                "objectIdPrefix": "sim_",
                "maxProposals": 1,
            },
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "provider"
    assert data["providerId"] == "deterministic-scene-authoring"
    assert data["mutated"] is False
    assert [call["name"] for call in data["toolCalls"]] == [
        "inspect_isaac_scene",
        "propose_scene_objects",
    ]
    assert [call["status"] for call in data["toolCalls"]] == ["completed", "completed"]
    assert data["toolCalls"][0]["arguments"]["source"] == "request.context"
    assert data["toolCalls"][1]["result"]["proposalCount"] == 1
    assert len(data["proposals"]) == 1
    proposal = data["proposals"][0]
    assert proposal["type"] == "scene_resource_add"
    assert proposal["payload"]["sceneResource"]["objectId"] == "sim_World__soup_can"


def test_deterministic_provider_can_include_tree_node_tool_proposals(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Create planning objects and object operation nodes",
            "mode": "provider",
            "context": {
                "isaacSceneReport": _sample_isaac_report(),
                "sourceRoots": ["/World/soup_can"],
                "objectIdPrefix": "sim_",
                "proposeTreeNodes": True,
                "nodeOperations": ["add_collision_object", "attach_object_to_link"],
                "linkFrame": "tool0",
                "gripperId": "robotiq",
            },
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert [call["name"] for call in data["toolCalls"]] == [
        "inspect_isaac_scene",
        "propose_scene_objects",
        "propose_scene_object_nodes",
    ]
    proposal_types = [proposal["type"] for proposal in data["proposals"]]
    assert proposal_types == ["scene_resource_add", "tree_node_add", "tree_node_add"]
    attach_node = data["proposals"][2]["payload"]["treeNode"]
    assert attach_node["id"] == "attach_object_to_link"
    assert attach_node["params"]["object_id"] == "sim_World__soup_can"
    assert attach_node["params"]["link_frame"] == "tool0"
    assert attach_node["params"]["gripper_id"] == "robotiq"


def test_deterministic_provider_skips_live_inspection_without_context_report(
    client: TestClient,
    state: ComposerState,
) -> None:
    state.assistant_provider = DeterministicSceneAuthoringProvider()

    response = client.post(
        "/api/assistant/chat",
        json={
            "message": "Inspect Isaac and propose planning objects",
            "mode": "provider",
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["mode"] == "provider"
    assert data["toolCalls"][0]["name"] == "inspect_isaac_scene"
    assert data["toolCalls"][0]["status"] == "skipped"
    assert "Live inspection is intentionally not performed" in data["toolCalls"][0]["error"]
    assert data["mutated"] is False
    assert data["proposals"][0]["type"] == "scene_authoring_review"


def test_inspect_isaac_scene_tool_uses_injected_client_without_mutation(
    client: TestClient,
    state: ComposerState,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls = []

    def fake_inspect(request):
        calls.append(request)
        return _sample_isaac_report(source_roots=["/World/soup_can"])

    monkeypatch.setattr(routes_assistant, "inspect_isaac_scene_report", fake_inspect)
    before_generation = state.program_session_generation

    response = client.post(
        "/api/assistant/tools/inspect-isaac-scene",
        json={
            "includeSubtrees": ["/World/soup_can"],
            "excludeSubtrees": ["/World/robot"],
            "timeoutS": 1.5,
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolName"] == "inspect_isaac_scene"
    assert data["success"] is True
    assert data["mutated"] is False
    assert data["summary"]["object_count"] == 1
    assert data["report"]["objects"][0]["source_root"] == "/World/soup_can"
    assert calls[0].include_subtrees == ["/World/soup_can"]
    assert calls[0].exclude_subtrees == ["/World/robot"]
    assert state.program_session_generation == before_generation


def test_propose_scene_objects_returns_scene_resource_drafts(
    client: TestClient,
    state: ComposerState,
) -> None:
    before_program = state.program
    response = client.post(
        "/api/assistant/tools/propose-scene-objects",
        json={
            "inspectionReport": _sample_isaac_report(),
            "sourceRoots": ["/World/soup_can"],
            "objectIdPrefix": "sim_",
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolName"] == "propose_scene_objects"
    assert data["mutated"] is False
    assert data["warnings"] == []
    assert len(data["sceneResources"]) == 1
    resource = data["sceneResources"][0]
    assert resource == {
        "objectId": "sim_World__soup_can",
        "shape": {"type": "box", "box_dimensions_m": [0.074928, 0.07488, 0.101855]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.085469, -0.004781, 0.060786],
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
    }
    proposal = data["proposals"][0]
    assert proposal["type"] == "scene_resource_add"
    assert proposal["status"] == "draft"
    assert proposal["reviewRequired"] is True
    assert proposal["payload"]["sceneResource"] == resource
    assert proposal["payload"]["sourceRoot"] == "/World/soup_can"
    assert state.program is before_program


def test_propose_scene_objects_rejects_missing_objects(client: TestClient) -> None:
    response = client.post(
        "/api/assistant/tools/propose-scene-objects",
        json={"inspectionReport": {"summary": {}}},
    )

    assert response.status_code == 400
    assert "inspectionReport.objects must be a list" in response.json()["detail"]


def test_propose_scene_objects_skips_invalid_shape_lengths(client: TestClient) -> None:
    report = _sample_isaac_report(source_roots=["/World/soup_can"])
    report["objects"][0]["recommended_manyforge_shape"]["shape"]["box_dimensions_m"] = [1.0, 2.0]

    response = client.post(
        "/api/assistant/tools/propose-scene-objects",
        json={"inspectionReport": report},
    )

    assert response.status_code == 200
    data = response.json()
    assert data["sceneResources"] == []
    assert "box_dimensions_m must contain exactly 3 values" in data["warnings"][0]


def test_propose_scene_objects_skips_invalid_source_root(client: TestClient) -> None:
    report = _sample_isaac_report(source_roots=["/World/soup_can"])
    report["objects"][0]["source_root"] = ""

    response = client.post(
        "/api/assistant/tools/propose-scene-objects",
        json={"inspectionReport": report},
    )

    assert response.status_code == 200
    data = response.json()
    assert data["sceneResources"] == []
    assert "source_root must be a non-empty string" in data["warnings"][0]


def test_propose_scene_object_nodes_returns_tree_node_drafts(client: TestClient) -> None:
    resource = _scene_resource_proposal()["payload"]["sceneResource"]

    response = client.post(
        "/api/assistant/tools/propose-scene-object-nodes",
        json={
            "sceneResources": [resource],
            "operations": [
                "add_collision_object",
                "upsert_collision_object",
                "remove_collision_object",
            ],
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["toolName"] == "propose_scene_object_nodes"
    assert data["mutated"] is False
    assert data["warnings"] == []
    assert len(data["treeNodes"]) == 3
    add_node = data["treeNodes"][0]
    assert add_node["id"] == "add_collision_object"
    assert add_node["params"]["object_id"] == "sim_soup_can"
    assert add_node["params"]["shape_type"] == "box"
    assert add_node["params"]["box_dimensions_m"] == [0.075, 0.075, 0.102]
    assert add_node["params"]["frame_id"] == "universe"
    assert data["proposals"][0]["type"] == "tree_node_add"
    assert data["proposals"][0]["payload"]["treeNode"]["id"] == add_node["id"]
    assert data["proposals"][0]["payload"]["treeNode"]["params"] == add_node["params"]
    upsert_node = data["treeNodes"][1]
    assert upsert_node["id"] == "upsert_collision_object"
    assert upsert_node["params"]["object_id"] == "sim_soup_can"
    assert upsert_node["params"]["shape_type"] == "box"
    assert upsert_node["params"]["box_dimensions_m"] == [0.075, 0.075, 0.102]


def test_stage_accepts_valid_tree_node_proposal(client: TestClient) -> None:
    resource = _scene_resource_proposal()["payload"]["sceneResource"]
    node_response = client.post(
        "/api/assistant/tools/propose-scene-object-nodes",
        json={"sceneResources": [resource]},
    )
    assert node_response.status_code == 200
    proposal = node_response.json()["proposals"][0]

    response = client.post(
        "/api/assistant/proposals/stage",
        json={"conversationId": "conv-tree", "proposals": [proposal]},
    )

    assert response.status_code == 200
    data = response.json()
    assert data["proposals"][0]["proposal"]["type"] == "tree_node_add"
    assert data["proposals"][0]["proposal"]["payload"]["treeNode"]["id"] == "add_collision_object"


def test_stage_and_discard_scene_resource_proposal(client: TestClient) -> None:
    proposal = _scene_resource_proposal()

    stage_response = client.post(
        "/api/assistant/proposals/stage",
        json={
            "conversationId": "conv-1",
            "proposals": [proposal],
            "source": {"kind": "test"},
        },
    )

    assert stage_response.status_code == 200
    data = stage_response.json()
    assert data["mutated"] is False
    assert data["programMutated"] is False
    assert data["conversationId"] == "conv-1"
    assert data["proposals"][0]["proposalId"] == proposal["id"]
    assert data["proposals"][0]["lifecycleStatus"] == "staged"
    assert data["proposals"][0]["proposal"]["payload"]["sceneResource"]["objectId"] == "sim_soup_can"

    list_response = client.get("/api/assistant/proposals")
    assert list_response.status_code == 200
    assert list_response.json()["proposals"][0]["proposalId"] == proposal["id"]

    discard_response = client.post(
        f"/api/assistant/proposals/{proposal['id']}/discard",
        json={"reason": "operator rejected"},
    )
    assert discard_response.status_code == 200
    assert discard_response.json()["lifecycleStatus"] == "discarded"
    assert discard_response.json()["source"]["discardReason"] == "operator rejected"


def test_stage_rejects_invalid_scene_resource_proposal(client: TestClient) -> None:
    proposal = _scene_resource_proposal()
    proposal["payload"]["sceneResource"].pop("objectId")

    response = client.post(
        "/api/assistant/proposals/stage",
        json={"conversationId": "conv-1", "proposals": [proposal]},
    )

    assert response.status_code == 400
    assert "payload.sceneResource is invalid" in response.json()["detail"]


def test_materialize_scene_resource_add_returns_session_patch_without_mutation(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path))
    before_generation = state.program_session_generation

    proposal = _scene_resource_proposal()
    stage_response = client.post(
        "/api/assistant/proposals/stage",
        json={"conversationId": "conv-1", "proposals": [proposal]},
    )
    assert stage_response.status_code == 200

    response = client.post(f"/api/assistant/proposals/{proposal['id']}/materialize")

    assert response.status_code == 200
    data = response.json()
    assert data["proposalId"] == proposal["id"]
    assert data["mutated"] is False
    assert data["programMutated"] is False
    assert data["sessionPatch"]["scene"] == [
        proposal["payload"]["sceneResource"],
    ]
    assert state.program_session_generation == before_generation
    assert state.program is not None
    assert state.program.scene == []


def test_materialize_scene_resource_add_with_deployment_uses_capability_keys(
    client: TestClient,
    state: ComposerState,
    tmp_path: Path,
) -> None:
    program_path = _write_minimal_program(tmp_path)
    state.load_program(str(program_path), deployment_path=str(_DEPLOYMENT_FIXTURE))

    proposal = _scene_resource_proposal()
    stage_response = client.post(
        "/api/assistant/proposals/stage",
        json={"conversationId": "conv-1", "proposals": [proposal]},
    )
    assert stage_response.status_code == 200

    response = client.post(f"/api/assistant/proposals/{proposal['id']}/materialize")

    assert response.status_code == 200
    data = response.json()
    assert data["mutated"] is False
    assert data["sessionPatch"]["scene"] == [
        proposal["payload"]["sceneResource"],
    ]


def test_assistant_scene_authoring_smoke_fixture_path() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_REPO_ROOT / "scripts" / "smoke-assistant-scene-authoring.py"),
            "--fixture",
            str(_ISAAC_SCENE_FIXTURE),
            "--include-subtree",
            "/World/soup_can",
            "--source-root",
            "/World/soup_can",
            "--object-id-prefix",
            "sim_",
        ],
        cwd=str(_REPO_ROOT),
        check=True,
        capture_output=True,
        text=True,
    )
    data = json.loads(result.stdout)
    assert data["success"] is True
    assert data["mode"] == "fixture"
    assert data["transport"] == "direct"
    assert data["inspect"]["summary"]["object_count"] == 1
    assert data["inspect"]["summary"]["collider_count"] == 2
    assert data["inspect"]["mutated"] is False
    assert data["propose"]["mutated"] is False
    assert data["propose"]["proposalCount"] == 1
    assert data["propose"]["sceneResources"] == [
        {
            "objectId": "sim_World__soup_can",
            "shape": {
                "type": "box",
                "box_dimensions_m": [0.074928, 0.07488, 0.101855],
            },
            "pose": {
                "frame_id": "universe",
                "position_m": [0.085469, -0.004781, 0.060786],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        }
    ]


def test_assistant_scene_authoring_smoke_api_proposal_lifecycle() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_REPO_ROOT / "scripts" / "smoke-assistant-scene-authoring.py"),
            "--transport",
            "api",
            "--workflow",
            "proposal-lifecycle",
            "--fixture",
            str(_ISAAC_SCENE_FIXTURE),
            "--include-subtree",
            "/World/soup_can",
            "--source-root",
            "/World/soup_can",
            "--object-id-prefix",
            "sim_",
        ],
        cwd=str(_REPO_ROOT),
        check=True,
        capture_output=True,
        text=True,
    )
    data = json.loads(result.stdout)
    assert data["success"] is True
    assert data["workflow"] == "proposal-lifecycle"
    assert data["transport"] == "api"
    assert data["inspect"]["mutated"] is False
    assert data["propose"]["proposalCount"] == 1
    assert data["proposalLifecycle"]["stage"]["proposalCount"] == 1
    assert data["proposalLifecycle"]["stage"]["mutated"] is False
    assert data["proposalLifecycle"]["stage"]["programMutated"] is False
    assert data["proposalLifecycle"]["materialize"]["mutated"] is False
    assert data["proposalLifecycle"]["materialize"]["programMutated"] is False
    assert data["proposalLifecycle"]["materialize"]["sessionPatch"]["scene"] == [
        data["propose"]["sceneResources"][0],
    ]
    assert data["proposalLifecycle"]["loadedProgram"] == {
        "sceneCountBefore": 0,
        "sceneCountAfter": 0,
        "sessionGenerationUnchanged": True,
    }


def _sample_isaac_report(source_roots: list[str] | None = None) -> dict:
    report = json.loads(_ISAAC_SCENE_FIXTURE.read_text(encoding="utf-8"))
    if not source_roots:
        return report
    selected = set(source_roots)
    report["objects"] = [
        obj
        for obj in report["objects"]
        if isinstance(obj, dict) and obj.get("source_root") in selected
    ]
    report["summary"]["object_count"] = len(report["objects"])
    report["summary"]["collider_count"] = sum(
        int(obj.get("collider_count") or 0)
        for obj in report["objects"]
        if isinstance(obj, dict)
    )
    return report


def _scene_resource_proposal() -> dict:
    resource = {
        "objectId": "sim_soup_can",
        "shape": {"type": "box", "box_dimensions_m": [0.075, 0.075, 0.102]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.1, -0.2, 0.05],
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
    }
    return {
        "id": "proposal-scene-object-sim_soup_can",
        "type": "scene_resource_add",
        "title": "Add soup can planning object",
        "summary": "Draft planning object from simulator collider data.",
        "status": "draft",
        "reviewRequired": True,
        "payload": {
            "action": "add_scene_resource",
            "sceneResource": resource,
            "sourceRoot": "/World/soup_can",
        },
        "warnings": ["review required"],
        "source": {"kind": "test"},
    }


def _assistant_catalog_hash(state: ComposerState, assistant_mode: str) -> str:
    assert state.deployment is not None
    resolved = state.deployment.resolve_assistant_mode_catalog(assistant_mode)
    return routes_assistant._compute_assistant_catalog_hash(
        {
            "skills": list(resolved["skills"]),
            "tools": [
                tool.get("id")
                for tool in resolved["tools"]
                if isinstance(tool.get("id"), str)
            ],
            "nodes": list(resolved["nodes"]),
        },
        assistant_mode,
    )


def _assistant_mode_deployment() -> Deployment:
    return Deployment.from_dict(
        {
            "api_version": "manyforge.deployment.v0",
            "schema_version": "0.1.0",
            "name": "assistant-mode-test",
            "program_profile": "v0.0",
            "robot_config": {
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://test.urdf",
                "global_max_tcp_linear_speed_mps": 0.5,
            },
            "adapters": {
                "planning": {
                    "adapter_id": "planning.test",
                    "adapter_version": "0.1.0",
                    "capability_keys": [
                        "planning.motion.joint_goal",
                        "planning.motion.pose_goal",
                        "planning.trajectory.time_parameterized",
                    ],
                    "config": {},
                },
                "context": {
                    "adapter_id": "context.test",
                    "adapter_version": "0.1.0",
                    "capability_keys": [
                        "context.collision.check",
                        "context.kinematics.fk",
                        "context.limits.joint",
                        "context.state.current",
                        "context.validation.trajectory",
                        "scene.edit.add",
                        "scene.edit.remove",
                        "scene.edit.update_pose",
                        "scene.shape.box",
                        "scene.shape.sphere",
                    ],
                    "config": {},
                },
                "execution": {
                    "adapter_id": "execution.test",
                    "adapter_version": "0.1.0",
                    "capability_keys": [
                        "execution.feedback.phase_events",
                        "execution.feedback.state_snapshots",
                        "execution.pause",
                        "execution.reset",
                        "execution.resume",
                        "execution.stop.soft",
                        "execution.trajectory.execute",
                    ],
                    "config": {},
                },
            },
            "orchestrator": {"engine": "py_trees", "config": {}},
            "launch": {
                "runner": {
                    "module": "manyforge_behavior.runner",
                    "args": [],
                },
            },
            "assistant_modes": {
                "query": {
                    "enabled": True,
                    "description": "Read-only inspection.",
                    "authority": {
                        "session_kind": "not_applicable",
                        "approval_mode": "human_approved",
                        "review": "not_required",
                    },
                    "catalog": {
                        "skills": [],
                        "tools": [
                            "program.read",
                            "program.validate",
                            "status.read",
                            "scene.inspect",
                        ],
                        "nodes": [],
                    },
                },
                "composer-assistant": {
                    "enabled": True,
                    "description": "Program composition.",
                    "authority": {
                        "session_kind": "not_applicable",
                        "approval_mode": "human_approved",
                        "review": "required",
                    },
                    "catalog": {
                        "skills": [],
                        "tools": [
                            "program.read",
                            "catalog.read",
                            "skills.read",
                            "deployment.capabilities.read",
                            "tree.insert_node",
                            "scene.draft.add_object",
                            "scene.draft.update_object",
                            "scene.draft.upsert_objects",
                            "scene.draft.remove_objects",
                            "tree.draft.insert_node",
                            "tree.draft.update_node_params",
                            "tree.draft.delete_node",
                            "tree.draft.move_node",
                            "tree.draft.replace_subtree",
                            "program.draft.upsert_parameters",
                            "program.draft.remove_parameters",
                            "blackboard.draft.upsert_namespaces",
                            "blackboard.draft.remove_namespaces",
                            "blackboard.draft.upsert_keys",
                            "blackboard.draft.remove_keys",
                        ],
                        "nodes": [],
                    },
                },
            },
            "tool_catalog": [
                {
                    "id": "program.read",
                    "effect": "read_only",
                    "description": "Read the loaded program.",
                },
                {
                    "id": "program.validate",
                    "effect": "read_only",
                    "description": "Validate the loaded program.",
                },
                {
                    "id": "status.read",
                    "effect": "read_only",
                    "description": "Read runtime status.",
                },
                {
                    "id": "scene.inspect",
                    "effect": "read_only",
                    "description": "Inspect backend scene state.",
                },
                {
                    "id": "catalog.read",
                    "effect": "read_only",
                    "description": "Read node catalog entries.",
                },
                {
                    "id": "skills.read",
                    "effect": "read_only",
                    "description": "Read skill catalog entries.",
                },
                {
                    "id": "deployment.capabilities.read",
                    "effect": "read_only",
                    "description": "Read deployment capabilities.",
                },
                {
                    "id": "tree.insert_node",
                    "effect": "proposal",
                    "description": "Propose a tree insertion.",
                },
                {
                    "id": "scene.draft.add_object",
                    "effect": "composer_draft_mutating",
                    "description": "Apply a scene-object addition to the Composer draft.",
                },
                {
                    "id": "scene.draft.update_object",
                    "effect": "composer_draft_mutating",
                    "description": "Apply a scene-object replacement to the Composer draft.",
                },
                {
                    "id": "scene.draft.upsert_objects",
                    "effect": "composer_draft_mutating",
                    "description": "Insert or replace scene objects in the Composer draft.",
                },
                {
                    "id": "scene.draft.remove_objects",
                    "effect": "composer_draft_mutating",
                    "description": "Remove scene objects from the Composer draft.",
                },
                {
                    "id": "tree.draft.insert_node",
                    "effect": "composer_draft_mutating",
                    "description": "Insert a node into the Composer draft tree.",
                },
                {
                    "id": "tree.draft.update_node_params",
                    "effect": "composer_draft_mutating",
                    "description": "Update node parameters in the Composer draft tree.",
                },
                {
                    "id": "tree.draft.delete_node",
                    "effect": "composer_draft_mutating",
                    "description": "Delete a node from the Composer draft tree.",
                },
                {
                    "id": "tree.draft.move_node",
                    "effect": "composer_draft_mutating",
                    "description": "Move a node in the Composer draft tree.",
                },
                {
                    "id": "tree.draft.replace_subtree",
                    "effect": "composer_draft_mutating",
                    "description": "Replace a subtree in the Composer draft tree.",
                },
                {
                    "id": "program.draft.upsert_parameters",
                    "effect": "composer_draft_mutating",
                    "description": "Insert or replace program parameters.",
                },
                {
                    "id": "program.draft.remove_parameters",
                    "effect": "composer_draft_mutating",
                    "description": "Remove program parameters.",
                },
                {
                    "id": "blackboard.draft.upsert_namespaces",
                    "effect": "composer_draft_mutating",
                    "description": "Insert or replace blackboard namespaces.",
                },
                {
                    "id": "blackboard.draft.remove_namespaces",
                    "effect": "composer_draft_mutating",
                    "description": "Remove blackboard namespaces.",
                },
                {
                    "id": "blackboard.draft.upsert_keys",
                    "effect": "composer_draft_mutating",
                    "description": "Insert or replace blackboard keys.",
                },
                {
                    "id": "blackboard.draft.remove_keys",
                    "effect": "composer_draft_mutating",
                    "description": "Remove blackboard keys.",
                },
            ],
        }
    )


def _write_minimal_program(tmp_path: Path) -> Path:
    program_path = tmp_path / "minimal.program.yaml"
    program_path.write_text(
        yaml.safe_dump(
            {
                "api_version": "manyforge.program.v0",
                "program_profile": "v0.0",
                "name": "minimal",
                "catalog_version": "0.1.0",
                "description": "minimal program",
                "robot": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                },
                "tree": {
                    "id": "sequence",
                    "name": "root",
                    "children": [
                        {
                            "id": "reset_trajectories",
                            "name": "reset_trajectories",
                        }
                    ],
                },
            },
            sort_keys=False,
        ),
        encoding="utf-8",
    )
    return program_path
