from __future__ import annotations

import os
import time
from pathlib import Path

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.routes_scene_meshes import get_scene_mesh_asset_service
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState

_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


def _write_obj(path: Path) -> None:
    path.write_text(
        "\n".join(
            [
                "v 0 0 0",
                "v 1 0 0",
                "v 0 1 0",
                "v 0 0 1",
                "f 1 2 3",
                "f 1 2 4",
            ]
        ),
        encoding="utf-8",
    )


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState]:
    kernel = MockKernelClient()
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=kernel,
        allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)],
    )
    state.program_source_path = str(tmp_path / "program.yaml")
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    app = create_app(
        state,
        security=SecurityConfig(api_token=None, allowed_fs_roots=[str(tmp_path), str(_REPO_ROOT)]),
    )
    return TestClient(app), state


def test_scene_mesh_object_gets_glb_asset_url(tmp_path: Path) -> None:
    mesh_path = tmp_path / "object.obj"
    _write_obj(mesh_path)
    client, _state = _client(tmp_path)

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "mesh_object",
            "shape_type": "mesh",
            "mesh_resource_uri": str(mesh_path),
            "mesh_scale": [1.0, 1.0, 1.0],
            "pose_position_m": [0.0, 0.0, 0.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 200
    assert response.json()["success"] is True

    shape = None
    for _ in range(30):
        state_response = client.get("/api/scene/state")
        assert state_response.status_code == 200
        objects = state_response.json()["objects"]
        shape = objects[0]["shape"]
        if shape.get("mesh_asset_status") == "ok":
            break
        time.sleep(0.1)
    assert shape is not None
    assert shape["mesh_asset_status"] == "ok"
    assert shape["mesh_asset_url"].startswith("/api/scene/object-meshes/")

    mesh_response = client.get(shape["mesh_asset_url"])
    assert mesh_response.status_code == 200
    assert mesh_response.headers["content-type"] == "model/gltf-binary"
    assert mesh_response.content[:4] == b"glTF"


def test_scene_mesh_asset_hash_ignores_mtime_only_touch(tmp_path: Path) -> None:
    mesh_path = tmp_path / "object.obj"
    _write_obj(mesh_path)
    _client_obj, state = _client(tmp_path)
    service = get_scene_mesh_asset_service(state)

    first = service._descriptor_for_resource(  # noqa: SLF001 - asset-key regression
        state,
        str(mesh_path),
        [1.0, 1.0, 1.0],
        register=True,
    )
    old_stat = mesh_path.stat()
    os.utime(mesh_path, ns=(old_stat.st_atime_ns + 1_000_000_000, old_stat.st_mtime_ns + 1_000_000_000))
    second = service._descriptor_for_resource(  # noqa: SLF001 - asset-key regression
        state,
        str(mesh_path),
        [1.0, 1.0, 1.0],
        register=True,
    )

    assert first.asset_hash == second.asset_hash


def test_scene_mesh_resolver_accepts_workspace_relative_isaac_example_uri(tmp_path: Path) -> None:
    """Mirror RoboPlan's relative path fallback for committed example scenes.

    The Isaac example stores repo-root relative mesh_resource_uri values while
    the program file itself lives under examples/. The viewer resolver must not
    reinterpret those paths as examples/examples/... and mark every mesh missing.
    """
    _client_obj, state = _client(tmp_path)
    state.program_source_path = str(
        _REPO_ROOT / "examples" / "isaac_manipulator_scene_light.program.yaml"
    )
    service = get_scene_mesh_asset_service(state)

    descriptor = service._descriptor_for_resource(  # noqa: SLF001 - resolver regression
        state,
        (
            "examples/isaac_manipulator_scene_light/meshes/"
            "mac_n_cheese_centered_collider_cooked.obj"
        ),
        [1.0, 1.0, 1.0],
        register=True,
    )

    assert descriptor.status == "pending"
    assert descriptor.resolved_realpath == str(
        (
            _REPO_ROOT
            / "examples"
            / "isaac_manipulator_scene_light"
            / "meshes"
            / "mac_n_cheese_centered_collider_cooked.obj"
        ).resolve()
    )


def test_scene_state_renders_workspace_relative_isaac_example_mesh(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    state.program_source_path = str(
        _REPO_ROOT / "examples" / "isaac_manipulator_scene_light.program.yaml"
    )

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "isaac_cheese_box",
            "shape_type": "mesh",
            "mesh_resource_uri": (
                "examples/isaac_manipulator_scene_light/meshes/"
                "mac_n_cheese_centered_collider_cooked.obj"
            ),
            "mesh_scale": [1.0, 1.0, 1.0],
            "pose_position_m": [0.0, 0.0, 0.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 200

    shape = None
    for _ in range(30):
        state_response = client.get("/api/scene/state")
        assert state_response.status_code == 200
        shape = state_response.json()["objects"][0]["shape"]
        if shape.get("mesh_asset_status") == "ok":
            break
        time.sleep(0.1)

    assert shape is not None
    assert shape["mesh_asset_status"] == "ok"
    assert shape["mesh_asset_url"].startswith("/api/scene/object-meshes/")


def test_scene_mesh_edit_rejects_paths_outside_allowed_roots(tmp_path: Path) -> None:
    mesh_path = tmp_path.parent / "outside.obj"
    _write_obj(mesh_path)
    client, _state = _client(tmp_path)

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "mesh_object",
            "shape_type": "mesh",
            "mesh_resource_uri": str(mesh_path),
            "pose_position_m": [0.0, 0.0, 0.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 409
    assert response.json()["detail"]["mesh_asset_status"] == "outside_allowed_roots"


def test_scene_mesh_edit_rejects_oversized_mesh(tmp_path: Path, monkeypatch) -> None:
    mesh_path = tmp_path / "large.obj"
    _write_obj(mesh_path)
    monkeypatch.setenv("MANYFORGE_MESH_MAX_FILE_BYTES", "8")
    client, _state = _client(tmp_path)

    response = client.post(
        "/api/scene/edit",
        json={
            "operation": "add",
            "object_id": "mesh_object",
            "shape_type": "mesh",
            "mesh_resource_uri": str(mesh_path),
            "pose_position_m": [0.0, 0.0, 0.0],
            "pose_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
    )
    assert response.status_code == 409
    assert response.json()["detail"]["mesh_asset_status"] == "too_large"
