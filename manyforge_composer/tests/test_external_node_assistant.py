"""External nodes preserve assistant default-deny authoring boundaries."""

from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.node_environment import build_node_environment
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.tests.node_environment_support import (
    write_external_test_node,
)
from manyforge_composer.tests.test_assistant_routes import _assistant_catalog_hash


REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOG_PATH = REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
PROGRAM_PATH = REPO_ROOT / "examples" / "pick_and_place_ur10e_robotiq.program.yaml"
DEPLOYMENT_PATH = REPO_ROOT / "examples" / "assistant_modes_scene_authoring.deployment.yaml"
SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=[str(REPO_ROOT), "/workspace", "/tmp"],
)


def _request(node_name: str, catalog_hash: str) -> dict:
    return {
        "requestId": f"req-{node_name}",
        "assistantMode": "composer-assistant",
        "catalogHash": catalog_hash,
        "arguments": {
            "parentName": "pick_and_place",
            "nodeName": node_name,
            "node": {"id": "vendor_assistant_leaf", "params": {}},
        },
    }


def test_external_node_is_readable_but_not_authorable_until_mode_allowlisted(
    tmp_path: Path,
) -> None:
    write_external_test_node(
        tmp_path,
        "vendor_assistant_leaf",
        parameters=[
            {
                "name": "retry_count",
                "required": False,
                "default": 2,
                "schema": {
                    "type": "int",
                    "constraints": {"min_inclusive": 0, "max_inclusive": 5},
                },
                "description": "Bounded retry count.",
            }
        ],
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    state = ComposerState(node_environment=environment, kernel=MockKernelClient())
    state.load_program(str(PROGRAM_PATH))
    state.deployment = Deployment(DEPLOYMENT_PATH)
    client = TestClient(create_app(state, security=SECURITY))

    initial_hash = _assistant_catalog_hash(state, "composer-assistant")
    catalog_response = client.post(
        "/api/assistant/bridge/tools/catalog_read",
        json={
            "requestId": "req-external-catalog",
            "assistantMode": "composer-assistant",
            "catalogHash": initial_hash,
            "arguments": {"entryIds": ["vendor_assistant_leaf"]},
        },
    )
    assert catalog_response.status_code == 200
    entry = catalog_response.json()["result"]["entries"][0]
    assert entry["id"] == "vendor_assistant_leaf"
    assistant_parameter = entry["parameters"][0]
    rest_entry = next(
        item
        for item in client.get("/api/catalog").json()["entries"]
        if item["id"] == "vendor_assistant_leaf"
    )
    rest_parameter = rest_entry["parameters"][0]
    assert assistant_parameter["schema"] == rest_parameter["schema"] == {
        "type": "integer",
        "minimum": 0,
        "maximum": 5,
    }
    assert assistant_parameter["schemaHash"] == rest_parameter["schemaHash"]
    assert assistant_parameter["default"] == rest_parameter["default"] == 2

    rejected = client.post(
        "/api/assistant/bridge/tools/tree_draft_insert_node",
        json=_request("external_rejected", initial_hash),
    ).json()
    assert rejected["success"] is False
    assert rejected["result"]["httpStatus"] == 403
    assert "not allowed" in rejected["error"]

    mode = state.deployment.assistant_modes["composer-assistant"]
    mode.catalog.nodes.append("vendor_assistant_leaf")
    allowed_hash = _assistant_catalog_hash(state, "composer-assistant")
    accepted = client.post(
        "/api/assistant/bridge/tools/tree_draft_insert_node",
        json=_request("external_accepted", allowed_hash),
    )
    assert accepted.status_code == 200
    payload = accepted.json()
    assert payload["success"] is True
    assert payload["result"]["draftMutation"] == "tree_node_insert"
    assert state.program is not None
    assert any(
        child.name == "external_accepted"
        for child in state.program.tree.children
    )
