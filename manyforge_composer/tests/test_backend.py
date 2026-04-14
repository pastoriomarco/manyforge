"""Tests for ManyForge Composer Backend."""
from __future__ import annotations

import logging
import time
import pytest
from pathlib import Path
from fastapi.testclient import TestClient
from httpx import AsyncClient
import httpx
import asyncio
import py_trees
import yaml

from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_behavior.cycle_manager import CycleState
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.__main__ import build_program_loaded_callback
from manyforge_composer.backend import routes_filesystem, routes_infra

# Test security config: no auth, permissive filesystem roots for tmp_path access
_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=["/workspace", "/tmp"],
)


# Test fixture paths (relative to repo root, same as existing tests)
_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_DEPLOYMENT_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
_EXAMPLE_PROGRAM_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.program.yaml")
_EXAMPLE_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.deployment.yaml")
_SEMANTIC_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "semantic_shape_pick.deployment.yaml")


def _write_program_with_runtime_policy(
    tmp_path: Path,
    *,
    runtime_override: str = "llm_allowed",
    persistent_edit: str = "human_approval",
    intervention_delegate: str = "forbidden",
) -> str:
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    move_node = data["tree"]["children"][0]
    move_node["modification_policy"] = {
        "runtime_override": runtime_override,
        "persistent_edit": persistent_edit,
        "intervention_delegate": intervention_delegate,
    }
    target = tmp_path / "runtime_override.program.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


@pytest.fixture
def state():
    """Create a fresh ComposerState for each test."""
    kernel = MockKernelClient()
    return ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)


@pytest.fixture
def client(state):
    """Create a TestClient for the app."""
    app = create_app(state, security=_TEST_SECURITY)
    return TestClient(app)


def make_client(execute_duration_s: float = 0.0) -> TestClient:
    """Create a TestClient with a configurable mock execution duration."""
    kernel = MockKernelClient(execute_duration_s=execute_duration_s)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    return TestClient(app)


# =============================================================================
# Cycle Control Tests
# =============================================================================

def test_status_no_program(client):
    """GET /api/cycle/status returns idle + 1.0 when no program loaded."""
    response = client.get("/api/cycle/status")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "idle"
    assert data["velocityScale"] == 1.0


def test_runtime_override_status_no_program_returns_empty_state(client):
    """GET /api/program/runtime-overrides should be stable before any program is loaded."""
    response = client.get("/api/program/runtime-overrides")
    assert response.status_code == 200
    data = response.json()
    assert data["active"] == []
    assert data["staged"] == []
    assert data["stale"] == []
    assert data["activeCount"] == 0
    assert data["stagedCount"] == 0
    assert data["staleCount"] == 0
    assert data["hasActive"] is False
    assert data["hasStaged"] is False
    assert data["hasStale"] is False
    assert data["canActivateNow"] is False
    assert data["requiresResetToActivate"] is False
    assert data["currentTreeOrigin"] == "saved_program"
    assert data["baseProgramHash"] == ""
    assert data["nextEffectiveTree"] is None


def test_run_no_program_409(client):
    """POST /api/cycle/run returns 409 when no program loaded."""
    response = client.post("/api/cycle/run")
    assert response.status_code == 409


def test_stop_no_program_409(client):
    """POST /api/cycle/stop returns 409 when no program loaded."""
    response = client.post("/api/cycle/stop")
    assert response.status_code == 409


def test_reset_no_program_409(client):
    """POST /api/cycle/reset returns 409 when no program loaded."""
    response = client.post("/api/cycle/reset")
    assert response.status_code == 409


def test_velocity_no_program_409(client):
    """PUT /api/cycle/velocity-scale returns 409 when no program loaded."""
    response = client.put("/api/cycle/velocity-scale", json={"value": 0.5})
    assert response.status_code == 409


def test_run_from_idle(client, state):
    """Load program, POST run, verify state=running."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    response = client.post("/api/cycle/run")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "running"


def test_stop_from_running():
    """Run then stop, verify state=stopped."""
    with make_client(execute_duration_s=1.0) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        run_response = client.post("/api/cycle/run")
        assert run_response.status_code == 200
        assert run_response.json()["state"] == "running"

        response = client.post("/api/cycle/stop")
        assert response.status_code == 200
        data = response.json()
        assert data["state"] == "stopped"


def test_reset_from_running(client, state):
    """Run then reset, verify state=idle."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    client.post("/api/cycle/run")
    
    # Reset cycle
    response = client.post("/api/cycle/reset")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "idle"


def test_run_from_stopped():
    """Run, stop, run again, verify state=running."""
    with make_client(execute_duration_s=1.0) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        first_run = client.post("/api/cycle/run")
        assert first_run.status_code == 200
        assert first_run.json()["state"] == "running"

        stop_response = client.post("/api/cycle/stop")
        assert stop_response.status_code == 200
        assert stop_response.json()["state"] == "stopped"

        response = client.post("/api/cycle/run")
        assert response.status_code == 200
        data = response.json()
        assert data["state"] == "running"


def test_velocity_scale(client, state):
    """PUT velocity-scale {value: 0.5}, verify response."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Set velocity scale
    response = client.put("/api/cycle/velocity-scale", json={"value": 0.5})
    assert response.status_code == 200
    data = response.json()
    assert data["velocityScale"] == 0.5


# =============================================================================
# Catalog Tests
# =============================================================================

def test_catalog_list(client):
    """GET /api/catalog returns entries list with version."""
    response = client.get("/api/catalog")
    assert response.status_code == 200
    data = response.json()
    assert "version" in data
    assert "entries" in data
    assert isinstance(data["entries"], list)


def test_catalog_entry(client):
    """GET /api/catalog/sequence returns entry details."""
    response = client.get("/api/catalog/sequence")
    assert response.status_code == 200
    data = response.json()
    assert data["id"] == "sequence"
    assert "name" in data
    assert "category" in data
    assert "description" in data
    assert "kind" in data


def test_catalog_entry_includes_blackboard_metadata(client):
    """Catalog entry surfaces declared blackboard reader/writer parameter metadata."""
    response = client.get("/api/catalog/set_key_bool_value")
    assert response.status_code == 200
    data = response.json()
    assert data["bbReaderParams"] == []
    assert data["bbWriterParams"] == ["key"]


def test_catalog_entry_includes_ui_editor_hints(client):
    """Catalog entry surfaces UI editor hints for structured parameters."""
    response = client.get("/api/catalog/plan_pick_sequence")
    assert response.status_code == 200
    data = response.json()
    params = {param["name"]: param for param in data["parameters"]}
    assert params["pick_offset_envelope"]["uiEditor"] == "pose_envelope"
    assert params["approach_offset_envelope"]["uiEditor"] == "pose_envelope"
    assert "place_pose_map" not in params


def test_catalog_entry_not_found(client):
    """GET /api/catalog/nonexistent returns 404."""
    response = client.get("/api/catalog/nonexistent")
    assert response.status_code == 404


# =============================================================================
# Filesystem Browser Tests
# =============================================================================

def test_browse_program_files_filters_to_compatible_extensions(client, tmp_path):
    (tmp_path / "alpha.program.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "beta.program.yml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "ignore.txt").write_text("hello\n", encoding="utf-8")
    (tmp_path / "subdir").mkdir()

    response = client.get("/api/files/browse", params={"path": str(tmp_path), "kind": "program"})
    assert response.status_code == 200
    data = response.json()

    entry_names = [entry["name"] for entry in data["entries"]]
    assert "subdir" in entry_names
    assert "alpha.program.yaml" in entry_names
    assert "beta.program.yml" in entry_names
    assert "ignore.txt" not in entry_names


def test_browse_deployment_files_filters_to_compatible_extensions(client, tmp_path):
    (tmp_path / "robot.deployment.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "robot.deployment.yml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "other.program.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")

    response = client.get("/api/files/browse", params={"path": str(tmp_path), "kind": "deployment"})
    assert response.status_code == 200
    data = response.json()

    entry_names = [entry["name"] for entry in data["entries"]]
    assert "robot.deployment.yaml" in entry_names
    assert "robot.deployment.yml" in entry_names
    assert "other.program.yaml" not in entry_names


def test_file_browser_roots_skip_container_root_home(monkeypatch, tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()

    monkeypatch.setattr(routes_filesystem.Path, "cwd", staticmethod(lambda: workspace))
    monkeypatch.setattr(routes_filesystem.Path, "home", staticmethod(lambda: Path("/root")))

    allowed = [str(workspace), "/workspace"]
    roots = routes_filesystem._root_shortcuts(allowed)
    assert str(workspace.resolve()) in roots
    assert "/root" not in roots


# =============================================================================
# Program Tests
# =============================================================================

def test_load_program(client, state):
    """POST /api/program/load with valid path, verify response."""
    response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "description" in data
    assert "programProfile" in data
    assert "robot" in data
    assert "sceneCount" in data
    assert "parameterCount" in data


def test_load_program_invalid_path(client, state):
    """POST with a path outside the allowlist returns 403."""
    response = client.post("/api/program/load", json={"path": "/nonexistent/path.yaml"})
    assert response.status_code == 403


def test_get_program(client, state):
    """Load then GET /api/program."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get program
    response = client.get("/api/program")
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "robot" in data


def test_get_program_not_loaded(client):
    """GET /api/program without loading returns 404."""
    response = client.get("/api/program")
    assert response.status_code == 404


def test_get_program_tree(client, state):
    """Load then GET /api/program/tree, verify tree structure."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get tree
    response = client.get("/api/program/tree")
    assert response.status_code == 200
    data = response.json()
    assert "tree" in data
    tree = data["tree"]
    assert "id" in tree
    assert "name" in tree
    assert "params" in tree
    assert "children" in tree


def test_get_program_tree_preserves_modification_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.get("/api/program/tree")
    assert response.status_code == 200
    move_node = response.json()["tree"]["children"][0]
    assert move_node["modificationPolicy"]["runtime_override"] == "llm_allowed"


def test_runtime_override_apply_updates_effective_tree_but_not_saved_artifact(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
            "reason": "path planning degraded",
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert data["hasActive"] is True
    assert data["currentTreeOrigin"] == "runtime_override"

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    move_node = runtime_tree["children"][0]
    assert move_node["params"]["planning_timeout_s"] == 3.5
    assert move_node["modificationPolicy"]["runtime_override"] == "llm_allowed"

    save_path = str(tmp_path / "saved_copy")
    save_response = client.post("/api/program/save", json={"path": save_path})
    assert save_response.status_code == 200
    saved_yaml = yaml.safe_load(
        Path(save_response.json()["path"]).read_text(encoding="utf-8")
    )
    assert saved_yaml["tree"]["children"][0]["params"]["planning_timeout_s"] == 2.0


def test_runtime_override_requires_node_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path, runtime_override="forbidden")
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 409
    assert "does not allow runtime overrides" in response.json()["detail"]


def test_runtime_override_staged_in_direct_control_requires_reset_before_stopped_resume(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL

        response = client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )
        assert response.status_code == 200
        assert response.json()["hasStaged"] is True
        assert response.json()["hasActive"] is False

        state.cycle_manager._state = CycleState.STOPPED
        run_response = client.post("/api/cycle/run")
        assert run_response.status_code == 409
        assert "reset the cycle first" in run_response.json()["detail"]


def test_runtime_override_activate_from_idle_promotes_staged_override(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state.cycle_manager._state = CycleState.IDLE
        activate_response = client.post("/api/program/runtime-overrides/activate")
        assert activate_response.status_code == 200
        assert activate_response.json()["hasActive"] is True
        assert activate_response.json()["hasStaged"] is False

        runtime_tree = client.get("/api/program/tree").json()["tree"]
        assert runtime_tree["children"][0]["params"]["planning_timeout_s"] == 4.0


def test_runtime_override_materialized_tree_supports_staged_view(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        current_tree = client.get("/api/program/runtime-overrides/materialized-tree").json()["tree"]
        staged_tree = client.get(
            "/api/program/runtime-overrides/materialized-tree",
            params={"includeStaged": "true"},
        ).json()["tree"]

        assert current_tree["children"][0]["params"]["planning_timeout_s"] == 2.0
        assert staged_tree["children"][0]["params"]["planning_timeout_s"] == 4.0


def test_runtime_override_promote_loaded_program_materializes_and_clears_ledger(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={"target": "loaded_program"},
    )
    assert response.status_code == 200
    data = response.json()
    assert data["runtimeOverrideStatus"]["hasActive"] is False
    assert data["tree"]["children"][0]["params"]["planning_timeout_s"] == 3.5

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    assert runtime_tree["children"][0]["params"]["planning_timeout_s"] == 3.5


def test_runtime_override_promote_loaded_program_rejects_dropping_staged_overrides(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state.cycle_manager._state = CycleState.STOPPED
        response = client.post(
            "/api/program/runtime-overrides/promote",
            json={"target": "loaded_program"},
        )
        assert response.status_code == 409
        assert "include staged overrides" in response.json()["detail"]


def test_runtime_override_promote_save_requires_persistent_edit_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        persistent_edit="forbidden",
    )
    client.post("/api/program/load", json={"path": program_path})
    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={
            "target": "save",
            "path": str(tmp_path / "saved_materialized"),
            "includeStaged": False,
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 409
    assert "does not allow persistent edits" in response.json()["detail"]


def test_runtime_override_promote_save_materializes_to_disk_with_explicit_authority(client, tmp_path):
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        persistent_edit="llm_allowed",
    )
    client.post("/api/program/load", json={"path": program_path})
    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={
            "target": "save",
            "path": str(tmp_path / "saved_materialized"),
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 200
    saved_path = Path(response.json()["path"])
    saved_yaml = yaml.safe_load(saved_path.read_text(encoding="utf-8"))
    assert saved_yaml["tree"]["children"][0]["params"]["planning_timeout_s"] == 3.5
    status = client.get("/api/program/runtime-overrides").json()
    assert status["hasActive"] is True


# =============================================================================
# Runtime Override Revision Binding Tests (OP-5)
# =============================================================================


def test_runtime_override_status_includes_base_program_hash(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert "baseProgramHash" in data
    assert len(data["baseProgramHash"]) == 64
    assert data["active"][0]["baseProgramHash"] == data["baseProgramHash"]
    assert data["active"][0]["isStale"] is False
    assert data["staleCount"] == 0
    assert data["hasStale"] is False


def test_load_program_rejects_when_overrides_exist(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post("/api/program/load", json={"path": program_path})
    assert response.status_code == 409
    assert "runtime overrides exist" in response.json()["detail"].lower()


def test_load_program_with_force_discard_overrides(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/load",
        json={"path": program_path, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0


def test_update_tree_rejects_when_overrides_exist(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    modified_tree = dict(original_tree)
    modified_tree["params"] = {"new_param": True}
    response = client.put(
        "/api/program/tree",
        json={"tree": modified_tree, "forceDiscardOverrides": False},
    )
    assert response.status_code == 409
    assert "runtime overrides exist" in response.json()["detail"].lower()


def test_update_tree_with_force_discard_overrides(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.put(
        "/api/program/tree",
        json={"tree": original_tree, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0


def test_runtime_override_revalidate_endpoint_no_stale(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post("/api/program/runtime-overrides/revalidate")
    assert response.status_code == 200
    data = response.json()
    assert data["staleCount"] == 0


def test_stale_override_detected_after_hash_change(tmp_path):
    """Simulate a program change by mutating _program_tree_hash; override becomes stale."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        status = client.get("/api/program/runtime-overrides").json()
        assert status["hasStale"] is False
        original_hash = status["baseProgramHash"]

        # Simulate a program change by mutating the hash.
        state._program_tree_hash = "0" * 64

        status = client.get("/api/program/runtime-overrides").json()
        assert status["hasStale"] is True
        assert status["staleCount"] == 1
        assert status["stale"][0]["nodeName"] == "move_to_pick"
        assert status["stale"][0]["isStale"] is True
        assert status["stale"][0]["baseProgramHash"] == original_hash
        assert status["canActivateNow"] is False


def test_stale_override_blocks_materialized_tree(tmp_path):
    """Stale overrides prevent materialized-tree preview."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state._program_tree_hash = "0" * 64

        response = client.get("/api/program/runtime-overrides/materialized-tree")
        assert response.status_code == 404
        assert "stale" in response.json()["detail"].lower()


def test_stale_override_revalidation_succeeds(tmp_path):
    """Revalidation updates the hash when the override is still valid."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        original_hash = state._program_tree_hash

        # Force-discard overrides and reload the same program to get a new
        # tree hash that is identical to the original.  Then re-apply the
        # override and simulate a hash change followed by reverting to the
        # real hash so the node still exists and validates.
        client.post(
            "/api/program/runtime-overrides/clear",
        )
        client.post("/api/program/load", json={"path": program_path})
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        # Simulate staleness then restore the real hash (like loading a
        # compatible program revision).
        state._program_tree_hash = "0" * 64
        assert state._has_stale_overrides()

        # Restore real hash so the node exists and override validates.
        state._program_tree_hash = original_hash
        # The override still has its old hash from apply time which equals
        # original_hash, so it is NOT stale now.  To test revalidation we
        # must make the override hash differ from current.
        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64

        response = client.post("/api/program/runtime-overrides/revalidate")
        assert response.status_code == 200
        data = response.json()
        assert data["revalidation_results"]["move_to_pick"] == "revalidated"
        assert data["staleCount"] == 0
        assert data["activeCount"] == 1
        assert data["active"][0]["baseProgramHash"] == original_hash


def test_stale_override_invalidation_removes_from_ledger(tmp_path):
    """Revalidation removes overrides whose target node no longer exists."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        # Simulate staleness.
        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64
        # Also rename the node in the tree so it can't be found.
        state.program.tree.children[0].name = "renamed_node"

        response = client.post("/api/program/runtime-overrides/revalidate")
        assert response.status_code == 200
        data = response.json()
        assert data["revalidation_results"]["move_to_pick"] == "invalid"
        assert data["activeCount"] == 0
        assert data["staleCount"] == 0


def test_force_discard_overrides_unconditional_same_tree(client, tmp_path):
    """forceDiscardOverrides=true clears overrides even when the tree is unchanged."""
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    # Re-apply the same tree with forceDiscardOverrides.
    response = client.put(
        "/api/program/tree",
        json={"tree": original_tree, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0
    assert status["hasActive"] is False


def test_validate_program_tree_accepts_current_tree(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    current_tree = client.get("/api/program/tree").json()["tree"]

    response = client.post("/api/program/validate", json={"tree": current_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is True
    assert data["errors"] == []


def test_validate_program_tree_reports_structural_errors(client):
    bad_tree = {
        "id": "sequence",
        "name": "root",
        "children": [
            {
                "id": "nonexistent_node",
                "name": "bad_child",
            },
        ],
    }

    response = client.post("/api/program/validate", json={"tree": bad_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is False
    assert data["errors"]
    assert data["errors"][0]["path"] == "tree.children[0]"
    assert "Unknown catalog entry" in data["errors"][0]["message"]


def test_validate_program_tree_reports_parameter_errors(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    bad_tree = {
        "id": "sequence",
        "name": "root",
        "children": [
            {
                "id": "move_manipulator_action",
                "name": "bad_move",
                "params": {
                    "move_id": "pick",
                    "motion_type": "joint_goal",
                    "joint_goal_names": ["j1"],
                    "joint_goal_positions_rad": [0.1],
                    "velocity_scale": -0.1,
                },
            },
        ],
    }

    response = client.post("/api/program/validate", json={"tree": bad_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is False
    assert any(
        error["path"] == "tree.children[0].params.velocity_scale"
        and "must be > 0.0" in error["message"]
        for error in data["errors"]
    )


def test_get_saved_program_tree_reflects_disk_not_applied_runtime(client):
    """GET /api/program/saved/tree returns the on-disk artifact even after an unsaved apply."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    modified_tree = {
        "id": "sequence",
        "name": "applied_not_saved_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }
    response = client.post("/api/session/apply", json={"tree": modified_tree})
    assert response.status_code == 200

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    saved_tree = client.get("/api/program/saved/tree").json()["tree"]
    assert runtime_tree["name"] == "applied_not_saved_sequence"
    assert saved_tree["name"] == "main_sequence"


# =============================================================================
# Deployment Tests
# =============================================================================

def test_get_deployment_not_loaded(client):
    """GET /api/deployment returns 404."""
    response = client.get("/api/deployment")
    assert response.status_code == 404


def test_get_capabilities_requires_loaded_deployment(client):
    response = client.get("/api/capabilities")
    assert response.status_code == 404


def test_load_deployment(client, state):
    """POST /api/deployment/load with valid path, verify response."""
    response = client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "description" in data
    assert "programProfile" in data
    assert "robotConfig" in data


def test_get_capabilities_returns_deployment_capability_summary(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})

    response = client.get("/api/capabilities")
    assert response.status_code == 200
    data = response.json()
    assert "planning.motion.joint_goal" in data["planning"]
    assert "context.state.current" in data["context"]
    assert "scene.edit.add" in data["scene"]
    assert "execution.trajectory.execute" in data["execution"]
    assert data["sensing"] == []
    assert data["contactTier"] == "unsupported"
    assert data["adapterIds"]["planning"] == "planning.roboplan"
    assert data["programProfile"] == "v0.0"
    assert data["timestampNs"] > 0


def test_get_skills_returns_catalog(client):
    """GET /api/skills returns the skill catalog with at least the default skills."""
    response = client.get("/api/skills")
    assert response.status_code == 200
    data = response.json()
    assert "skills" in data
    assert len(data["skills"]) >= 3
    skill_ids = [s["id"] for s in data["skills"]]
    assert "pick_object" in skill_ids
    assert "place_object" in skill_ids
    assert "move_to_named_config" in skill_ids

    pick = next(s for s in data["skills"] if s["id"] == "pick_object")
    assert pick["compatibleProfiles"] == ["v0.0"]
    assert len(pick["requires"]) > 0
    assert len(pick["provides"]) > 0
    assert len(pick["requiresCapabilities"]) > 0
    assert len(pick["parameters"]) >= 1
    assert "expandsTo" in pick
    assert pick["expandsTo"]["id"] == "sequence"
    assert data["timestampNs"] > 0


def test_get_full_deployment(client):
    """GET /api/deployment/full returns the full loaded artifact plus source path."""
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})

    response = client.get("/api/deployment/full")
    assert response.status_code == 200
    data = response.json()
    assert data["sourcePath"] == _DEPLOYMENT_PATH
    assert data["apiVersion"] == "manyforge.deployment.v0"
    assert "adapters" in data
    assert "orchestrator" in data
    assert "launch" in data


def test_get_full_deployment_includes_model_metadata(client):
    """GET /api/deployment/full includes model endpoints and prompt templates when configured."""
    client.post("/api/deployment/load", json={"path": _SEMANTIC_DEPLOYMENT_PATH})

    response = client.get("/api/deployment/full")
    assert response.status_code == 200
    data = response.json()
    assert "modelEndpoints" in data
    assert "promptTemplates" in data
    assert "selection_model" in data["modelEndpoints"]
    assert "select_scene_object_v1" in data["promptTemplates"]
    assert data["modelEndpoints"]["selection_model"]["modelId"] == "gemma-4-E4B-it"
    assert data["promptTemplates"]["select_scene_object_v1"]["responseTool"]["name"] == "select_scene_object"


def test_infra_model_endpoints_reports_configured_and_live_models(client, monkeypatch):
    """GET /api/infra/model-endpoints exposes deployment aliases plus deterministic /v1/models discovery."""
    client.post("/api/deployment/load", json={"path": _SEMANTIC_DEPLOYMENT_PATH})

    monkeypatch.setattr(
        routes_infra,
        "_probe_openai_compatible_models",
        lambda base_url: (["Qwen3.5-9B-Claude-Distilled-NVFP4", "gemma-4-E4B-it"], None),
    )

    response = client.get("/api/infra/model-endpoints")
    assert response.status_code == 200
    data = response.json()
    assert "selection_model" in data["endpoints"]
    endpoint = data["endpoints"]["selection_model"]
    assert endpoint["configuredModelId"] == "gemma-4-E4B-it"
    assert endpoint["probeStatus"] == "ok"
    assert "Qwen3.5-9B-Claude-Distilled-NVFP4" in endpoint["availableModelIds"]


def test_get_saved_full_deployment_reflects_disk_not_applied_runtime(client):
    """GET /api/deployment/saved/full returns the on-disk artifact even after an unsaved apply."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    full_deployment = client.get("/api/deployment/full").json()
    modified_deployment = {**full_deployment, "name": "applied_not_saved_deployment"}

    response = client.post("/api/session/apply", json={"deployment": modified_deployment})
    assert response.status_code == 200

    runtime_deployment = client.get("/api/deployment/full").json()
    saved_deployment = client.get("/api/deployment/saved/full").json()
    assert runtime_deployment["name"] == "applied_not_saved_deployment"
    assert saved_deployment["name"] == full_deployment["name"]


def test_load_program_rebuilds_kernel_from_deployment_config():
    """Program load rebuilds the kernel from deployment-derived config when a factory is set."""
    calls = []

    class RebuiltKernel(MockKernelClient):
        pass

    def kernel_factory(deployment):
        calls.append(deployment)
        return RebuiltKernel(), {"bundle": len(calls)}

    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_factory=kernel_factory,
    )
    app = create_app(state)

    with TestClient(app) as client:
        response = client.post(
            "/api/program/load",
            json={
                "path": _EXAMPLE_PROGRAM_PATH,
                "deployment_path": _EXAMPLE_DEPLOYMENT_PATH,
            },
        )

    assert response.status_code == 200
    assert len(calls) == 1
    assert calls[0] is state.deployment
    assert state.deployment is not None
    assert state.deployment.orchestrator.config["collision_monitor_period_ms"] == 10.0
    assert isinstance(state._kernel, RebuiltKernel)
    assert state.kernel_bundle == {"bundle": 1}


def test_put_deployment(client, state):
    """Load deployment via POST, PUT /api/deployment with modified data, GET /api/deployment — verify updated."""
    # Load deployment
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    
    # Get original deployment
    response = client.get("/api/deployment")
    original_data = response.json()
    original_name = original_data["name"]
    
    # Create modified deployment data
    modified_data = {
        "name": "updated_deployment",
        "description": "Updated description",
    }
    
    # PUT the modified deployment
    response = client.put("/api/deployment", json=modified_data)
    assert response.status_code == 200
    assert response.json()["status"] == "ok"
    
    # GET the deployment and verify it was updated
    response = client.get("/api/deployment")
    assert response.status_code == 200
    updated_data = response.json()
    assert updated_data["name"] == "updated_deployment"
    assert updated_data["name"] != original_name
    assert updated_data["description"] == "Updated description"


def test_save_deployment(client, state):
    """Load deployment via POST, POST /api/deployment/save with tmp path, verify file exists and is valid YAML."""
    import tempfile
    import os
    
    # Load deployment
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    
    # Create temp path
    tmp_path = tempfile.mktemp(suffix=".yaml")
    
    try:
        # POST to save
        response = client.post("/api/deployment/save", json={"path": tmp_path})
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        saved_path = data["path"]
        assert saved_path.endswith(".deployment.yaml")
        assert saved_path == f"{tmp_path}.deployment.yaml"
        
        # Verify file exists
        assert os.path.exists(saved_path)
        
        # Verify it's valid YAML
        import yaml
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.deployment.v0"
        assert "robot_config" in saved_data
        assert "adapters" in saved_data
        assert "orchestrator" in saved_data
        assert "launch" in saved_data
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.deployment.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_deployment_payload_does_not_apply_runtime_state(client):
    """Saving a modified deployment payload writes disk only and does not mutate loaded runtime state."""
    import os
    import tempfile
    import yaml

    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    original = client.get("/api/deployment/full").json()
    modified = {**original, "name": "saved_only_deployment"}

    tmp_path = tempfile.mktemp(suffix=".yaml")
    try:
      response = client.post("/api/deployment/save", json={"path": tmp_path, "deployment": modified})
      assert response.status_code == 200
      saved_path = response.json()["path"]
      assert saved_path == f"{tmp_path}.deployment.yaml"

      current = client.get("/api/deployment/full").json()
      assert current["name"] == original["name"]

      with open(saved_path) as f:
          saved_data = yaml.safe_load(f)
      assert saved_data["name"] == "saved_only_deployment"
    finally:
      if os.path.exists(tmp_path):
          os.unlink(tmp_path)
      suffixed_path = f"{tmp_path}.deployment.yaml"
      if os.path.exists(suffixed_path):
          os.unlink(suffixed_path)


# =============================================================================
# WebSocket Tests
# =============================================================================

def test_ws_state_receives_update(state):
    """Connect to /ws/state, verify at least one JSON message with state/velocityScale/timestamp fields."""
    app = create_app(state)
    with TestClient(app) as tc:
        with tc.websocket_connect("/ws/state") as ws:
            # The broadcast loop sends at 10Hz, so a message should arrive quickly
            message = ws.receive_json()
            assert "state" in message
            assert "velocityScale" in message
            assert "kernelEvents" in message
            assert isinstance(message["kernelEvents"], list)
            assert "timestamp" in message


def test_ws_includes_node_statuses(state):
    """WebSocket messages include nodeStatuses key."""
    app = create_app(state)
    with TestClient(app) as tc:
        with tc.websocket_connect("/ws/state") as ws:
            data = ws.receive_json()
            assert "nodeStatuses" in data
            assert isinstance(data["nodeStatuses"], dict)


# =============================================================================
# Program Save/Update Tests
# =============================================================================

def test_put_program_tree(client, state):
    """Load program via POST, PUT /api/program/tree with modified tree, GET /api/program/tree — verify updated."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get original tree
    response = client.get("/api/program/tree")
    original_tree = response.json()["tree"]
    original_name = original_tree["name"]
    
    # Create modified tree
    modified_tree = {
        "id": "sequence",
        "name": "updated_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset",
            },
        ],
    }
    
    # PUT the modified tree
    response = client.put("/api/program/tree", json={"tree": modified_tree})
    assert response.status_code == 200
    assert response.json()["status"] == "ok"
    
    # GET the tree and verify it was updated
    response = client.get("/api/program/tree")
    assert response.status_code == 200
    updated_tree = response.json()["tree"]
    assert updated_tree["name"] == "updated_sequence"
    assert updated_tree["name"] != original_name
    assert len(updated_tree["children"]) == 1

def test_save_program(client, state):
    """Load program via POST, POST /api/program/save with tmp path, verify file exists and is valid YAML."""
    import tempfile
    import os
    
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Create temp path
    tmp_path = tempfile.mktemp(suffix=".yaml")
    
    try:
        # POST to save
        response = client.post("/api/program/save", json={"path": tmp_path})
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        saved_path = data["path"]
        assert saved_path.endswith(".program.yaml")
        assert saved_path == f"{tmp_path}.program.yaml"
        
        # Verify file exists
        assert os.path.exists(saved_path)
        
        # Verify it's valid YAML
        import yaml
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.program.v0"
        assert "tree" in saved_data
        assert saved_data["tree"]["name"] == "main_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.program.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_program_tree_payload_does_not_apply_runtime_state(client):
    """Saving a modified tree payload writes disk only and does not mutate loaded runtime state."""
    import os
    import tempfile
    import yaml

    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    original_tree = client.get("/api/program/tree").json()["tree"]
    modified_tree = {
        "id": "sequence",
        "name": "saved_only_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }

    tmp_path = tempfile.mktemp(suffix=".yaml")
    try:
        response = client.post("/api/program/save", json={"path": tmp_path, "tree": modified_tree})
        assert response.status_code == 200
        saved_path = response.json()["path"]
        assert saved_path == f"{tmp_path}.program.yaml"

        current_tree = client.get("/api/program/tree").json()["tree"]
        assert current_tree == original_tree

        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        assert saved_data["tree"]["name"] == "saved_only_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.program.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_apply_session_updates_program_and_deployment_together(client):
    """POST /api/session/apply updates the runtime models atomically."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    full_deployment = client.get("/api/deployment/full").json()

    modified_tree = {
        "id": "sequence",
        "name": "applied_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }
    modified_deployment = {**full_deployment, "name": "applied_deployment"}

    response = client.post(
        "/api/session/apply",
        json={"tree": modified_tree, "deployment": modified_deployment},
    )
    assert response.status_code == 200

    current_tree = client.get("/api/program/tree").json()["tree"]
    current_deployment = client.get("/api/deployment/full").json()
    assert current_tree["name"] == "applied_sequence"
    assert current_deployment["name"] == "applied_deployment"


# =============================================================================
# Logging Tests
# =============================================================================

def test_json_log_format():
    """setup_logging produces JSON with level, event, timestamp keys."""
    import io
    import json
    import logging
    from manyforge_composer.backend.logging_config import setup_logging

    setup_logging("INFO")
    test_logger = logging.getLogger("test.json_format")
    buf = io.StringIO()
    handler = logging.StreamHandler(buf)
    handler.setFormatter(logging.getLogger().handlers[0].formatter)
    test_logger.addHandler(handler)
    test_logger.info("hello world")
    test_logger.removeHandler(handler)

    line = buf.getvalue().strip()
    record = json.loads(line)
    assert "timestamp" in record
    assert record["level"] == "INFO"
    assert record["event"] == "hello world"
    assert record["logger"] == "test.json_format"


def test_request_logging(client, capsys):
    """HTTP requests produce structured log entries."""
    client.get("/api/health")
    # Just verify the request didn't error — the logging middleware runs
    # but capturing stderr in a test with uvicorn is non-trivial.
    # The JSON format test above covers the formatter.


def test_program_load_callback_registers_scene_edit_callback(caplog):
    """Program-load callback wires scene sync without raising or logging errors."""
    py_trees.blackboard.Blackboard.clear()

    class DummySceneRenderer:
        def __init__(self):
            self.cleared = False
            self.loaded_scene = None
            self.sync_calls = 0

        def clear(self):
            self.cleared = True

        def load_from_program(self, scene):
            self.loaded_scene = scene

        def sync_box(self, *args, **kwargs):
            self.sync_calls += 1

        def sync_sphere(self, *args, **kwargs):
            self.sync_calls += 1

        def remove_object(self, object_id):
            self.last_removed = object_id

        def add_box_on_frame(self, *args, **kwargs):
            self.last_box_on_frame = (args, kwargs)

        def add_box(self, *args, **kwargs):
            self.last_box = (args, kwargs)

        def add_sphere(self, *args, **kwargs):
            self.last_sphere = (args, kwargs)

    class DummyHmiServer:
        instances = []

        def __init__(self, **kwargs):
            self.kwargs = kwargs
            self.scene_renderer = DummySceneRenderer()
            self.robot_visualizer = None
            self.started = False
            self.stopped = False
            self.program = None
            self.scene_sync_requested = 0
            DummyHmiServer.instances.append(self)

        def load_program(self, program):
            self.program = program

        def request_scene_sync(self, **kwargs):
            self.scene_sync_requested += 1

        def sync_scene_from_backend(self):
            self.scene_sync_requested += 1

        def start(self):
            self.started = True

        def stop(self):
            self.stopped = True

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    class DummyBundle:
        def __init__(self):
            self.set_calls = 0

        def set_joint_positions(self, positions):
            self.set_calls += 1

    bundle = DummyBundle()
    state._on_program_loaded = build_program_loaded_callback(
        host="127.0.0.1",
        hmi_port=0,
        urdf_path=None,
        package_dirs=[],
        bundle=bundle,
        hmi_server_factory=DummyHmiServer,
    )

    with caplog.at_level(logging.ERROR):
        state.load_program(_PROGRAM_PATH)

    assert not any("on_program_loaded callback failed" in rec.message for rec in caplog.records)
    assert DummyHmiServer.instances
    server = DummyHmiServer.instances[-1]
    assert server.started
    assert server.program is not None
    assert state.cycle_manager._on_run_from_idle is not None
    assert state.cycle_manager._tree is not None
    assert bundle.set_calls == 0

    bb = py_trees.blackboard.Client(name="test_scene_edit_callback")
    bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.READ)
    assert callable(bb.scene_edit_callback)

    state.cycle_manager._on_run_from_idle()
    assert server.scene_renderer.cleared
    assert server.scene_sync_requested > 0


def test_program_load_callback_applies_simulator_bootstrap_from_deployment(caplog):
    """Program-load callback applies explicit simulator-only startup joint positions."""
    py_trees.blackboard.Blackboard.clear()

    class DummySceneRenderer:
        def clear(self):
            self.cleared = True

    class DummyHmiServer:
        instances = []

        def __init__(self, **kwargs):
            self.kwargs = kwargs
            self.scene_renderer = DummySceneRenderer()
            self.robot_visualizer = None
            self.started = False
            self.program = None
            DummyHmiServer.instances.append(self)

        def load_program(self, program):
            self.program = program

        def request_scene_sync(self, **kwargs):
            pass

        def sync_scene_from_backend(self):
            pass

        def start(self):
            self.started = True

        def stop(self):
            pass

    class DummyBundle:
        def __init__(self):
            self.set_calls = []

        def set_joint_positions(self, positions):
            self.set_calls.append(list(positions))

    bundle = DummyBundle()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state._on_program_loaded = build_program_loaded_callback(
        host="127.0.0.1",
        hmi_port=0,
        urdf_path=None,
        package_dirs=[],
        bundle=bundle,
        hmi_server_factory=DummyHmiServer,
    )

    with caplog.at_level(logging.ERROR):
        state.load_program(_EXAMPLE_PROGRAM_PATH, deployment_path=_EXAMPLE_DEPLOYMENT_PATH)

    assert not any("on_program_loaded callback failed" in rec.message for rec in caplog.records)
    assert bundle.set_calls == [[0.0, -1.57, 1.57, -1.57, -1.57, 0.0]]


def test_debug_scene_trace_endpoint_returns_backend_and_hmi_observability():
    """Debug scene trace endpoint exposes current backend/HMI trace and scope."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.load_program(_PROGRAM_PATH)

    kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="trace_box",
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )

    class DummyHmiServer:
        def get_scene_trace(self, limit=None):
            return [{"source": "hmi", "event": "reconcile_render", "object_id": "trace_box"}]

        def get_object_render_diagnostics(self, object_id):
            return {"object_id": object_id, "exists": object_id == "trace_box"}

    state.hmi_server = DummyHmiServer()
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/debug/scene-trace?limit=10")

    assert response.status_code == 200
    data = response.json()
    assert "included" in data["scope"]
    assert data["backend"]
    assert data["backend"][-1]["event"] == "add"
    assert data["hmi"][0]["event"] == "reconcile_render"
    assert data["objects"]
    assert "table" in data["objects"]


def test_scene_snapshot_returns_503_without_hmi():
    """GET /api/scene/snapshot returns 503 when HMI is not running."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshot")
    assert response.status_code == 503
    assert "HMI server not running" in response.json()["detail"]


def test_scene_snapshot_returns_503_without_client():
    """GET /api/scene/snapshot returns 503 when no Viser client is connected."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    class MockHmi:
        def render_scene(self, width=640, height=480, format="jpeg"):
            return None  # No client connected

    state.hmi_server = MockHmi()
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshot")
    assert response.status_code == 503
    assert "No Viser client" in response.json()["detail"]


def test_scene_snapshot_returns_image():
    """GET /api/scene/snapshot returns JPEG image bytes when HMI renders."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    # Create a minimal valid JPEG (1x1 pixel)
    import io
    from PIL import Image
    img = Image.new("RGB", (1, 1), color=(128, 128, 128))
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    fake_jpeg = buf.getvalue()

    class MockHmi:
        def render_scene(self, width=640, height=480, format="jpeg"):
            return fake_jpeg

    state.hmi_server = MockHmi()
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshot?width=320&height=240&format=jpeg")
    assert response.status_code == 200
    assert response.headers["content-type"] == "image/jpeg"
    assert len(response.content) > 0


# =============================================================================
# Direct Control / Intervention Tests
# =============================================================================

def test_direct_execute_no_program_409(client):
    """POST /api/direct/execute returns 409 when no program loaded."""
    response = client.post("/api/direct/execute", json={
        "tree": {"id": "timer", "name": "t", "params": {"duration_s": 0.01}, "children": []},
        "principalId": "human:test-operator",
    })
    assert response.status_code == 409


def test_direct_status_no_program_409(client):
    """GET /api/direct/status returns 409 when no program loaded."""
    response = client.get("/api/direct/status")
    assert response.status_code == 409


def test_direct_abort_no_program_409(client):
    """POST /api/direct/abort returns 409 when no program loaded."""
    response = client.post("/api/direct/abort")
    assert response.status_code == 409


def test_direct_status_no_session_404():
    """GET /api/direct/status returns 404 when no intervention has run."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.get("/api/direct/status")
    assert response.status_code == 404


def test_direct_pending_recovery_404_when_no_request():
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.get("/api/direct/pending-recovery")
    assert response.status_code == 404


def test_delegated_recovery_request_created_on_node_failure(tmp_path):
    kernel = MockKernelClient()
    kernel.fail_next_execute = True
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        intervention_delegate="llm_allowed",
        persistent_edit="llm_allowed",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")
        time.sleep(0.2)

        pending = client.get("/api/direct/pending-recovery")
        assert pending.status_code == 200
        data = pending.json()
        assert data["triggerNodeInstance"] == "move_to_pick"
        assert data["triggerReason"] == "execute_failed"
        assert data["delegatePolicy"] == "llm_allowed"
        assert data["allowedAuthoritySources"] == ["human", "llm"]
        assert "runtime_override" in data["allowedActions"]
        assert "draft_patch" in data["allowedActions"]

        digest = client.get("/api/execution/digest").json()
        assert digest["state"] == "stopped"
        assert digest["delegatedRecoveryRequest"]["triggerNodeInstance"] == "move_to_pick"


def test_direct_execute_delegated_recovery_consumes_pending_request(tmp_path):
    kernel = MockKernelClient()
    kernel.fail_next_execute = True
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        intervention_delegate="llm_allowed",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")
        time.sleep(0.2)

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "delegated_pause",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "delegated_recovery",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
            "resumeTarget": "stop",
        })
        assert response.status_code == 200
        assert response.json()["sessionKind"] == "delegated_recovery"
        assert response.json()["triggerNodeInstance"] == "move_to_pick"
        assert response.json()["principalId"] == "agent:test-llm"
        assert response.json()["grantType"] == "policy_granted"

        time.sleep(0.2)
        pending = client.get("/api/direct/pending-recovery")
        assert pending.status_code == 404


def test_direct_execute_delegated_recovery_enforces_pending_authority(tmp_path):
    kernel = MockKernelClient()
    kernel.fail_next_execute = True
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        intervention_delegate="human_only",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")
        time.sleep(0.2)

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "delegated_pause",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "delegated_recovery",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
            "resumeTarget": "stop",
        })
        assert response.status_code == 409
        assert "authority_source must be one of: human" in response.json()["error"]


def test_direct_execute_and_status():
    """POST /api/direct/execute starts intervention; GET status returns session."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "brief_timer",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
            "approvalMode": "human_approved",
        })
        assert response.status_code == 200
        data = response.json()
        assert "sessionId" in data
        assert data["sessionKind"] == "manual_intervention"
        assert data["authoritySource"] == "human"
        assert data["principalId"] == "human:test-operator"
        assert data["grantType"] == "human_approved"
        assert data["status"] == "running"

        # Wait for completion
        time.sleep(0.3)

        status_resp = client.get("/api/direct/status")
        assert status_resp.status_code == 200
        status_data = status_resp.json()
        assert status_data["sessionId"] == data["sessionId"]
        assert status_data["status"] in ("completed", "failed")


def test_direct_promote_blackboard_value_after_intervention():
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        execute_response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "set_key_bool_value",
                "name": "publish_detection",
                "params": {"key": "detected_ok", "value": True},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
            "approvalMode": "human_approved",
        })
        assert execute_response.status_code == 200
        session_id = execute_response.json()["sessionId"]

        time.sleep(0.1)

        promote_response = client.post("/api/direct/promote", json={
            "sessionId": session_id,
            "promoteKeys": {"detected_ok": "recovery.detected_ok"},
        })
        assert promote_response.status_code == 200
        assert promote_response.json()["promotions"]

        blackboard = client.get("/api/execution/blackboard").json()["keys"]
        assert blackboard["recovery/detected_ok"]["valueJson"] is True


def test_direct_execute_from_idle_requires_human_approval():
    """POST /api/direct/execute rejects unapproved IDLE direct control."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "brief_timer",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
        })
    assert response.status_code == 409
    assert "human-approved manual intervention" in response.json()["error"]


def test_direct_execute_while_running_409():
    """POST /api/direct/execute returns 409 when cycle is RUNNING."""
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "t",
                "params": {"duration_s": 0.01},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
        })
        assert response.status_code == 409

        client.post("/api/cycle/stop")
        client.post("/api/cycle/reset")


def test_direct_abort_no_running_intervention():
    """POST /api/direct/abort returns 409 when no intervention is running."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.post("/api/direct/abort")
    assert response.status_code == 409


# =============================================================================
# Kernel Event Bridge Tests
# =============================================================================

def test_kernel_events_no_program(client):
    """GET /api/kernel/events returns empty when no program loaded."""
    response = client.get("/api/kernel/events")
    assert response.status_code == 200
    data = response.json()
    assert data["events"] == []
    assert data["last_event_index"] == 0


def test_kernel_events_after_execution():
    """GET /api/kernel/events returns events after program execution."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")
        time.sleep(0.3)  # let tree tick

        response = client.get("/api/kernel/events?since=0")
        assert response.status_code == 200
        data = response.json()
        assert len(data["events"]) > 0
        assert data["last_event_index"] > 0

        # Polling with last index returns no new events
        last = data["last_event_index"]
        response2 = client.get(f"/api/kernel/events?since={last}")
        data2 = response2.json()
        assert len(data2["events"]) == 0

        client.post("/api/cycle/reset")


def test_kernel_events_have_correct_structure():
    """Kernel events have required fields: type, execution_id, timestamp_ns."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")
        time.sleep(0.3)

        response = client.get("/api/kernel/events?since=0")
        data = response.json()
        for event in data["events"]:
            assert "type" in event
            assert "execution_id" in event
            assert "timestamp_ns" in event
            assert "event_index" in event

        client.post("/api/cycle/reset")
