"""Composer atomic node-environment reload and diagnostics routes."""

from __future__ import annotations

import shutil
import threading
from pathlib import Path
from types import SimpleNamespace

import py_trees
import pytest
from fastapi.testclient import TestClient
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.tests.node_environment_support import (
    write_external_test_node,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOG_PATH = REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"


def _write_node(root: Path, node_id: str) -> Path:
    return write_external_test_node(root, node_id)


def _state(*roots: Path) -> ComposerState:
    def factory(generation: int):
        return build_node_environment(
            CATALOG_PATH,
            external_roots=roots,
            generation=generation,
        )

    return ComposerState(
        node_environment=factory(1),
        node_environment_factory=factory,
        kernel=MockKernelClient(),
    )


def _program(environment, node_id: str) -> Program:
    return Program.from_dict(
        {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "reload_program",
            "catalog_version": environment.catalog.version,
            "robot": {
                "robot_id": "test",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": {"id": node_id, "name": "generated", "params": {}},
        },
        environment.catalog,
    )


def test_reload_adds_node_and_reports_catalog_delta(tmp_path: Path) -> None:
    state = _state(tmp_path)
    previous_hash = state.node_environment.effective_catalog_hash
    _write_node(tmp_path, "vendor_added")
    client = TestClient(create_app(state))

    response = client.post("/api/nodes/reload")

    assert response.status_code == 200
    report = response.json()
    assert report["status"] == "ok"
    assert report["previousCatalogHash"] == previous_hash
    assert report["summary"]["added"] == 1
    assert report["activeEffectiveCatalogHash"] != previous_hash
    catalog = client.get("/api/catalog").json()
    assert any(entry["id"] == "vendor_added" for entry in catalog["entries"])
    source = next(item for item in catalog["sources"] if item["nodeId"] == "vendor_added")
    assert source["kind"] == "external_python"
    assert source["contentHash"].startswith("sha256:")


def test_failed_duplicate_reload_preserves_active_environment(tmp_path: Path) -> None:
    root_a = tmp_path / "a"
    root_b = tmp_path / "b"
    _write_node(root_a, "vendor_duplicate")
    state = _state(root_a, root_b)
    active_hash = state.node_environment.effective_catalog_hash
    _write_node(root_b, "vendor_duplicate")
    client = TestClient(create_app(state))

    response = client.post("/api/nodes/reload")

    assert response.status_code == 422
    assert state.node_environment.effective_catalog_hash == active_hash
    diagnostics = client.get("/api/nodes/diagnostics").json()
    assert diagnostics["status"] == "rejected"
    assert diagnostics["activeEffectiveCatalogHash"] == active_hash
    assert any(
        result["errorCode"] == "duplicate_node_id"
        for result in diagnostics["results"]
    )


def test_source_route_serves_active_environment_snapshot_until_reload(
    tmp_path: Path,
) -> None:
    folder = _write_node(tmp_path, "vendor_source_snapshot")
    state = _state(tmp_path)
    client = TestClient(create_app(state))
    before = client.get(
        "/api/nodes/vendor_source_snapshot/source"
    ).json()

    (folder / "node.py").write_text(
        "raise RuntimeError('not active until reload')\n",
        encoding="utf-8",
    )
    (folder / "node.yaml").write_text("not: the active descriptor\n", encoding="utf-8")
    after = client.get(
        "/api/nodes/vendor_source_snapshot/source"
    ).json()

    assert after == before
    assert "not active until reload" not in after["source"]


def test_reload_conflict_while_execution_active_keeps_previous_catalog(
    tmp_path: Path,
) -> None:
    build_generations: list[int] = []

    def factory(generation: int):
        build_generations.append(generation)
        return build_node_environment(
            CATALOG_PATH,
            external_roots=[tmp_path],
            generation=generation,
        )

    state = ComposerState(
        node_environment=factory(1),
        node_environment_factory=factory,
        kernel=MockKernelClient(),
    )
    active_hash = state.node_environment.effective_catalog_hash
    _write_node(tmp_path, "vendor_busy")
    state.cycle_manager = SimpleNamespace(state=SimpleNamespace(value="running"))
    client = TestClient(create_app(state))

    response = client.post("/api/nodes/reload")

    assert response.status_code == 409
    assert state.node_environment.effective_catalog_hash == active_hash
    diagnostics = client.get("/api/nodes/diagnostics").json()
    assert diagnostics["status"] == "conflict"
    assert diagnostics["conflict"] == "execution_not_idle:running"
    assert build_generations == [1], "busy reload must not scan or import candidates"


def test_in_flight_run_admission_rejects_reload_before_candidate_build(
    tmp_path: Path,
) -> None:
    build_generations: list[int] = []

    def factory(generation: int):
        build_generations.append(generation)
        return build_node_environment(
            CATALOG_PATH,
            external_roots=[tmp_path],
            generation=generation,
        )

    state = ComposerState(
        node_environment=factory(1),
        node_environment_factory=factory,
        kernel=MockKernelClient(),
    )

    with state.runtime_admission_guard():
        with pytest.raises(RuntimeError, match="run_admission_in_progress"):
            state.reload_node_environment()

    assert build_generations == [1]
    assert state.latest_node_discovery_report.conflict == "run_admission_in_progress"


def test_reload_transaction_rejects_run_without_blocking_or_partial_swap(
    tmp_path: Path,
) -> None:
    build_started = threading.Event()
    release_build = threading.Event()

    def factory(generation: int):
        if generation > 1:
            build_started.set()
            assert release_build.wait(timeout=2.0)
        return build_node_environment(
            CATALOG_PATH,
            external_roots=[tmp_path],
            generation=generation,
        )

    state = ComposerState(
        node_environment=factory(1),
        node_environment_factory=factory,
        kernel=MockKernelClient(),
    )
    previous_hash = state.node_environment.effective_catalog_hash
    client = TestClient(create_app(state))
    errors: list[BaseException] = []

    def reload() -> None:
        try:
            state.reload_node_environment()
        except BaseException as exc:  # pragma: no cover - asserted below
            errors.append(exc)

    thread = threading.Thread(target=reload, daemon=True)
    thread.start()
    try:
        assert build_started.wait(timeout=1.0)
        response = client.post("/api/cycle/run")
        assert response.status_code == 409
        detail = response.json()["detail"]
        assert detail["errorCode"] == "node_environment_reload_in_progress"
        assert state.node_environment.effective_catalog_hash == previous_hash
    finally:
        release_build.set()
        thread.join(timeout=2.0)

    assert not thread.is_alive()
    assert errors == []
    assert state.node_environment.generation == 2


def test_reload_cannot_remove_node_used_by_loaded_program(tmp_path: Path) -> None:
    folder = _write_node(tmp_path, "vendor_in_use")
    state = _state(tmp_path)
    state.program = _program(state.node_environment, "vendor_in_use")
    state.program_source_path = "<memory>"
    active_hash = state.node_environment.effective_catalog_hash
    shutil.rmtree(folder)
    client = TestClient(create_app(state))

    response = client.post("/api/nodes/reload")

    assert response.status_code == 422
    assert state.node_environment.effective_catalog_hash == active_hash
    diagnostics = client.get("/api/nodes/diagnostics").json()
    assert diagnostics["conflict"] == "active_artifact_validation_failed"
    artifact_error = next(
        result
        for result in diagnostics["results"]
        if result["errorCode"] == "active_artifact_validation_failed"
    )
    assert "vendor_in_use" in artifact_error["messages"][0]
    assert state.node_environment.registry.contains("vendor_in_use")


def test_scenario_route_exposes_artifact_owned_inventory() -> None:
    state = _state()
    state.scenario_roots = [REPO_ROOT / "examples"]
    client = TestClient(create_app(state))

    response = client.get("/api/scenarios")

    assert response.status_code == 200
    scenarios = response.json()["scenarios"]
    assert len(scenarios) == 13
    assert sum(len(item["aliases"]) for item in scenarios) == 7
    isaac = next(
        item for item in scenarios if item["name"] == "isaac-ur10e-light-scene"
    )
    assert isaac["lane"] == "isaac-sim"
    assert isaac["aliases"] == ["isaac-light"]
    real_robot = next(
        item for item in scenarios if item["name"] == "real-lite6-commissioning"
    )
    assert real_robot["lane"] == "real-robot"
    assert real_robot["deploymentPath"].endswith(
        "lite6_fixed_pneumatic_commissioning.deployment.template.yaml"
    )
