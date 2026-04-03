"""Tests for ManyForge Composer Backend."""
from __future__ import annotations

import pytest
from pathlib import Path
from fastapi.testclient import TestClient
from httpx import AsyncClient
import httpx
import asyncio

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app


# Test fixture paths (relative to repo root, same as existing tests)
_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")


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


def test_stop_from_running(client, state):
    """Run then stop, verify state=stopped."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    client.post("/api/cycle/run")
    
    # Stop cycle
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


def test_run_from_stopped(client, state):
    """Run, stop, run again, verify state=running."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    client.post("/api/cycle/run")
    
    # Stop cycle
    client.post("/api/cycle/stop")
    
    # Run again
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


# =============================================================================
# Deployment Tests
# =============================================================================

def test_get_deployment_not_loaded(client):
    """GET /api/deployment returns 404."""
    response = client.get("/api/deployment")
    assert response.status_code == 404


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
