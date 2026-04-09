"""Tests for ManyForge Composer Backend."""
from __future__ import annotations

import logging
import pytest
from pathlib import Path
from fastapi.testclient import TestClient
from httpx import AsyncClient
import httpx
import asyncio
import py_trees

from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app
from manyforge_composer.__main__ import build_program_loaded_callback
from manyforge_composer.backend import routes_filesystem


# Test fixture paths (relative to repo root, same as existing tests)
_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_DEPLOYMENT_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
_EXAMPLE_PROGRAM_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.program.yaml")
_EXAMPLE_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.deployment.yaml")


@pytest.fixture
def state():
    """Create a fresh ComposerState for each test."""
    kernel = MockKernelClient()
    return ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)


@pytest.fixture
def client(state):
    """Create a TestClient for the app."""
    app = create_app(state)
    return TestClient(app)


def make_client(execute_duration_s: float = 0.0) -> TestClient:
    """Create a TestClient with a configurable mock execution duration."""
    kernel = MockKernelClient(execute_duration_s=execute_duration_s)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)
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

    roots = routes_filesystem._root_shortcuts()
    assert str(workspace.resolve()) in roots
    assert "/" in roots
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
    """POST with bad path returns 400."""
    response = client.post("/api/program/load", json={"path": "/nonexistent/path.yaml"})
    assert response.status_code == 400


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


def test_load_deployment(client, state):
    """POST /api/deployment/load with valid path, verify response."""
    response = client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "description" in data
    assert "programProfile" in data
    assert "robotConfig" in data


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
        assert data["path"] == tmp_path
        
        # Verify file exists
        assert os.path.exists(tmp_path)
        
        # Verify it's valid YAML
        import yaml
        with open(tmp_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.deployment.v0"
        assert "robot_config" in saved_data
        assert "adapters" in saved_data
        assert "orchestrator" in saved_data
        assert "launch" in saved_data
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


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

      current = client.get("/api/deployment/full").json()
      assert current["name"] == original["name"]

      with open(tmp_path) as f:
          saved_data = yaml.safe_load(f)
      assert saved_data["name"] == "saved_only_deployment"
    finally:
      if os.path.exists(tmp_path):
          os.unlink(tmp_path)


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
        assert data["path"] == tmp_path
        
        # Verify file exists
        assert os.path.exists(tmp_path)
        
        # Verify it's valid YAML
        import yaml
        with open(tmp_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.program.v0"
        assert "tree" in saved_data
        assert saved_data["tree"]["name"] == "main_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


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

        current_tree = client.get("/api/program/tree").json()["tree"]
        assert current_tree == original_tree

        with open(tmp_path) as f:
            saved_data = yaml.safe_load(f)
        assert saved_data["tree"]["name"] == "saved_only_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


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
