"""Tests for native URDF visualizer routes."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

pytest.importorskip("fastapi")

from fastapi.testclient import TestClient

from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend import routes_urdf
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState


_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")


class _FakeMesh:
    def __init__(self, filename: str) -> None:
        self.filename = filename
        self.scale = [1.0, 1.0, 1.0]


class _FakeGeometry:
    def __init__(self, filename: str) -> None:
        self.mesh = _FakeMesh(filename)
        self.box = None
        self.cylinder = None
        self.sphere = None


class _FakeVisual:
    def __init__(self, filename: str) -> None:
        self.geometry = _FakeGeometry(filename)
        self.origin = None


class _FakeLink:
    def __init__(self, visuals: list[_FakeVisual] | None = None) -> None:
        self.visuals = visuals or []


def test_urdf_manifest_uses_content_model_key_for_mesh_cache(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf_a = tmp_path / "robot_a.urdf"
    urdf_b = tmp_path / "robot_b.urdf"
    mesh_a = tmp_path / "mesh_a.stl"
    mesh_b = tmp_path / "mesh_b.stl"
    for path in (urdf_a, urdf_b, mesh_a, mesh_b):
        path.write_text("fixture")

    mesh_by_urdf = {
        str(urdf_a): str(mesh_a),
        str(urdf_b): str(mesh_b),
    }

    class FakeUrdf:
        base_link = "base_link"
        joint_map: dict[str, object] = {}

        def __init__(self, urdf_path: str) -> None:
            mesh_path = mesh_by_urdf[urdf_path]
            self.link_map = {
                "base_link": _FakeLink(),
                "upper_arm_link": _FakeLink([_FakeVisual(mesh_path)]),
            }

        @staticmethod
        def load(urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf(urdf_path)

    def fake_resolve_robot_model(**kwargs: object) -> SimpleNamespace:
        return SimpleNamespace(
            urdf_path=str(kwargs["urdf_path"]),
            package_paths=[],
        )

    monkeypatch.setattr(routes_urdf, "default_package_paths", lambda: [])
    monkeypatch.setattr(routes_urdf, "resolve_robot_model", fake_resolve_robot_model)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=FakeUrdf))

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    robot_config = SimpleNamespace(
        robot_id="ur",
        group_id="arm",
        urdf_path=str(urdf_a),
        srdf_path=None,
        package_paths=[],
    )
    state.deployment = SimpleNamespace(robot_config=robot_config)
    client = TestClient(create_app(state))

    first = client.get("/api/scene/urdf/manifest")
    assert first.status_code == 200
    first_manifest = first.json()
    first_model_key = first_manifest["model_key"]
    first_mesh_url = first_manifest["links"][0]["visuals"][0]["mesh_url"]

    robot_config.urdf_path = str(urdf_b)
    second = client.get("/api/scene/urdf/manifest")
    assert second.status_code == 200
    second_manifest = second.json()
    second_model_key = second_manifest["model_key"]
    second_mesh_url = second_manifest["links"][0]["visuals"][0]["mesh_url"]

    assert first_manifest["robot_id"] == second_manifest["robot_id"] == "ur"
    assert first_model_key != second_model_key
    assert f"/models/{first_model_key}/meshes/" in first_mesh_url
    assert f"/models/{second_model_key}/meshes/" in second_mesh_url
    assert first_mesh_url != second_mesh_url

    stale = client.get(first_mesh_url)
    assert stale.status_code == 409
    assert stale.json()["active_model_key"] == second_model_key

    active_mesh = client.get(second_mesh_url)
    assert active_mesh.status_code in (200, 404)
    if active_mesh.status_code == 200:
        assert active_mesh.headers["cache-control"] == "public, max-age=31536000, immutable"

    legacy_current = client.get(first_mesh_url.replace(f"/models/{first_model_key}", ""))
    assert legacy_current.status_code in (200, 404)
    if legacy_current.status_code == 200:
        assert legacy_current.headers["cache-control"] == "no-store"
