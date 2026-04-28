"""Tests for Composer assistant routes."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend import routes_assistant
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_ISAAC_SCENE_FIXTURE = (
    Path(__file__).resolve().parent / "fixtures" / "isaac_scene_authoring_soup_can.json"
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


def test_assistant_tools_catalog_is_review_only(client: TestClient) -> None:
    response = client.get("/api/assistant/tools")

    assert response.status_code == 200
    data = response.json()
    names = {tool["name"] for tool in data["tools"]}
    assert {"inspect_isaac_scene", "propose_scene_objects"} <= names
    for tool in data["tools"]:
        assert tool["mutatesState"] is False
        assert tool["requiresReview"] is True


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
