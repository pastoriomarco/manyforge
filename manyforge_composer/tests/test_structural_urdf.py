"""Structural URDF route tests without a runtime kernel."""

from __future__ import annotations

from pathlib import Path
import sys
from types import SimpleNamespace
from typing import Any

from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.deployment import (
    AuthoredRobotModelProjection,
    AuthoredStructuralDeployment,
)
from manyforge_composer.backend import routes_urdf
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState


def _transform(
    x: float = 0.0,
    y: float = 0.0,
    z: float = 0.0,
) -> list[list[float]]:
    return [
        [1.0, 0.0, 0.0, x],
        [0.0, 1.0, 0.0, y],
        [0.0, 0.0, 1.0, z],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _z_quarter_turn(
    x: float = 0.0,
    y: float = 0.0,
    z: float = 0.0,
) -> list[list[float]]:
    return [
        [0.0, -1.0, 0.0, x],
        [1.0, 0.0, 0.0, y],
        [0.0, 0.0, 1.0, z],
        [0.0, 0.0, 0.0, 1.0],
    ]


class _FakeGeometry:
    def __init__(self, filename: str) -> None:
        self.mesh = SimpleNamespace(filename=filename, scale=[1.0, 1.0, 1.0])
        self.box = None
        self.cylinder = None
        self.sphere = None


class _FakeVisual:
    def __init__(
        self,
        filename: str,
        *,
        origin: list[list[float]] | None = None,
    ) -> None:
        self.geometry = _FakeGeometry(filename)
        self.origin = origin


class _FakeCollision(_FakeVisual):
    pass


class _FakeLink:
    def __init__(
        self,
        *,
        visuals: list[_FakeVisual] | None = None,
        collisions: list[_FakeCollision] | None = None,
    ) -> None:
        self.visuals = visuals or []
        self.collisions = collisions or []


class _FakeMimic:
    def __init__(
        self,
        joint: str,
        *,
        multiplier: float = 1.0,
        offset: float = 0.0,
    ) -> None:
        self.joint = joint
        self.multiplier = multiplier
        self.offset = offset


class _FakeJoint:
    def __init__(
        self,
        name: str,
        parent: str,
        child: str,
        *,
        joint_type: str = "fixed",
        axis: tuple[float, float, float] = (1.0, 0.0, 0.0),
        origin: list[list[float]] | None = None,
        mimic: _FakeMimic | None = None,
    ) -> None:
        self.name = name
        self.parent = parent
        self.child = child
        self.type = joint_type
        self.axis = axis
        self.origin = origin
        self.limit = (
            SimpleNamespace(lower=-1.0, upper=1.0, effort=2.0, velocity=3.0)
            if joint_type in {"continuous", "prismatic", "revolute"}
            else None
        )
        self.mimic = mimic


@pytest.fixture(autouse=True)
def _clear_urdf_cache() -> None:
    routes_urdf._urdf_cache.clear()


def _patch_model_loading(
    monkeypatch: pytest.MonkeyPatch,
    fake_urdf_type: type[Any],
) -> None:
    monkeypatch.setattr(routes_urdf, "default_package_paths", lambda: [])
    monkeypatch.setattr(
        routes_urdf,
        "resolve_robot_model",
        lambda **kwargs: SimpleNamespace(
            urdf_path=str(kwargs["urdf_path"]),
            package_paths=[],
        ),
    )
    monkeypatch.setitem(
        sys.modules,
        "yourdfpy",
        SimpleNamespace(URDF=fake_urdf_type),
    )


def _structural_deployment(
    tmp_path: Path,
    urdf_path: Path,
) -> AuthoredStructuralDeployment:
    return AuthoredStructuralDeployment(
        source_path=tmp_path / "cell.deployment.yaml",
        model=AuthoredRobotModelProjection(
            authored_model_id="structural-robot",
            urdf_path=str(urdf_path),
        ),
    )


def _state(tmp_path: Path, urdf_path: Path) -> ComposerState:
    return ComposerState(
        deployment=_structural_deployment(tmp_path, urdf_path),
        deployment_source_path=str(tmp_path / "cell.deployment.yaml"),
    )


def test_state_loads_only_the_bounded_structural_model_projection(
    tmp_path: Path,
) -> None:
    urdf = tmp_path / "robot.urdf"
    urdf.write_text("<robot name='preview'/>", encoding="utf-8")
    artifact = tmp_path / "cell.deployment.yaml"
    artifact.write_text(
        "api_version: manyforge.deployment.v0\n"
        "robot_config:\n"
        "  robot_id: structural-robot\n"
        "  group_id: ignored-runtime-group\n"
        f"  urdf_path: {urdf}\n"
        "runtime_resources:\n"
        "  ignored: true\n",
        encoding="utf-8",
    )

    state = ComposerState.from_deployment_path(str(artifact))

    assert state.deployment is not None
    assert state.deployment.model.authored_model_id == "structural-robot"
    assert state.deployment.model.urdf_path == str(urdf)
    assert not hasattr(state.deployment.model, "group_id")


def test_manifest_projects_complete_authored_neutral_tree_without_authority(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf = tmp_path / "robot.urdf"
    visual_mesh = tmp_path / "visual.stl"
    collision_mesh = tmp_path / "collision.stl"
    for path in (urdf, visual_mesh, collision_mesh):
        path.write_text("fixture", encoding="utf-8")

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "carrier": _FakeLink(),
                "guide": _FakeLink(),
                "tool_link": _FakeLink(
                    visuals=[_FakeVisual(str(visual_mesh))],
                    collisions=[_FakeCollision(str(collision_mesh))],
                ),
            }
            self.joint_map = {
                "mount": _FakeJoint(
                    "mount",
                    "base_link",
                    "carrier",
                    origin=_transform(1.0, 0.0, 0.0),
                ),
                "slide": _FakeJoint(
                    "slide",
                    "carrier",
                    "guide",
                    joint_type="prismatic",
                    axis=(0.0, 1.0, 0.0),
                    origin=_transform(0.0, 2.0, 0.0),
                ),
                "mimic_slide": _FakeJoint(
                    "mimic_slide",
                    "guide",
                    "tool_link",
                    joint_type="prismatic",
                    axis=(0.0, 0.0, 1.0),
                    origin=_transform(0.0, 0.0, 3.0),
                    mimic=_FakeMimic("slide", multiplier=-2.0, offset=0.25),
                ),
            }

        @staticmethod
        def load(_urdf_path: str, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(tmp_path, urdf))).get(
        "/api/scene/urdf/manifest"
    )

    assert response.status_code == 200
    manifest = response.json()
    assert manifest["robot_id"] == "structural-robot"
    assert "authority" not in manifest
    assert manifest["projection"] == {
        "kind": "authored_neutral",
        "source": "authored_urdf",
        "root_frame": "base_link",
        "position_unit": "m",
        "angle_unit": "rad",
        "quaternion_order": "xyzw",
        "joint_coordinate_policy": "zero_independent_with_authored_mimic",
        "operational_validity": "not_asserted",
        "runtime_state_used": False,
    }

    links = {link["name"]: link for link in manifest["links"]}
    assert set(links) == {"base_link", "carrier", "guide", "tool_link"}
    assert links["base_link"]["visuals"] == []
    assert links["base_link"]["neutral_transform"] == {
        "relative_to": "base_link",
        "position_m": [0.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    assert links["tool_link"]["neutral_transform"] == {
        "relative_to": "base_link",
        "position_m": [1.0, 2.0, 3.25],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    visual = links["tool_link"]["visuals"][0]
    collision = links["tool_link"]["collisions"][0]
    assert f"/models/{manifest['model_key']}/meshes/tool_link/0" in visual[
        "mesh_url"
    ]
    assert f"/models/{manifest['model_key']}/collision-meshes/tool_link/0" in (
        collision["mesh_url"]
    )

    joints = {joint["name"]: joint for joint in manifest["joints"]}
    assert joints["mount"]["origin"] == {
        "relative_to": "base_link",
        "position_m": [1.0, 0.0, 0.0],
        "orientation_rpy_rad": [0.0, -0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    assert joints["mount"]["coordinate_unit"] is None
    assert joints["mount"]["neutral_position"] is None
    assert joints["slide"]["coordinate_unit"] == "m"
    assert joints["slide"]["neutral_position"] == 0.0
    assert joints["mimic_slide"]["neutral_position"] == 0.25
    assert joints["mimic_slide"]["mimic"] == {
        "master": "slide",
        "multiplier": -2.0,
        "offset": 0.25,
    }


def test_manifest_model_keys_asset_urls_and_rejects_stale_model(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf_a = tmp_path / "robot_a.urdf"
    urdf_b = tmp_path / "robot_b.urdf"
    mesh_a = tmp_path / "mesh_a.stl"
    mesh_b = tmp_path / "mesh_b.stl"
    for path in (urdf_a, urdf_b, mesh_a, mesh_b):
        path.write_text("fixture", encoding="utf-8")
    mesh_by_urdf = {str(urdf_a): str(mesh_a), str(urdf_b): str(mesh_b)}

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self, urdf_path: str) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "tool_link": _FakeLink(
                    visuals=[_FakeVisual(mesh_by_urdf[urdf_path])]
                ),
            }
            self.joint_map = {
                "mount": _FakeJoint("mount", "base_link", "tool_link")
            }

        @staticmethod
        def load(urdf_path: str, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf(urdf_path)

    _patch_model_loading(monkeypatch, FakeUrdf)
    monkeypatch.setattr(routes_urdf, "_render_visual_glb", lambda *_args: b"glb")
    state = _state(tmp_path, urdf_a)
    client = TestClient(create_app(state))

    first = client.get("/api/scene/urdf/manifest")
    repeated = client.get("/api/scene/urdf/manifest")
    assert first.status_code == repeated.status_code == 200
    assert first.json()["model_key"] == repeated.json()["model_key"]
    first_url = next(
        link for link in first.json()["links"] if link["name"] == "tool_link"
    )["visuals"][0]["mesh_url"]

    state.deployment = _structural_deployment(tmp_path, urdf_b)
    second = client.get("/api/scene/urdf/manifest")
    assert second.status_code == 200
    second_url = next(
        link for link in second.json()["links"] if link["name"] == "tool_link"
    )["visuals"][0]["mesh_url"]
    assert first.json()["model_key"] != second.json()["model_key"]
    assert first_url != second_url

    stale = client.get(first_url)
    assert stale.status_code == 409
    assert stale.json()["active_model_key"] == second.json()["model_key"]
    active = client.get(second_url)
    assert active.status_code == 200
    assert active.headers["cache-control"] == (
        "public, max-age=31536000, immutable"
    )


def test_manifest_composes_authored_rotations_and_publishes_xyzw(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf = tmp_path / "robot.urdf"
    urdf.write_text("fixture", encoding="utf-8")

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "elbow": _FakeLink(),
                "tool_link": _FakeLink(),
            }
            self.joint_map = {
                "base_to_elbow": _FakeJoint(
                    "base_to_elbow",
                    "base_link",
                    "elbow",
                    origin=_z_quarter_turn(1.0, 0.0, 0.0),
                ),
                "elbow_to_tool": _FakeJoint(
                    "elbow_to_tool",
                    "elbow",
                    "tool_link",
                    origin=_transform(1.0, 0.0, 0.0),
                ),
            }

        @staticmethod
        def load(_urdf_path: str, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(tmp_path, urdf))).get(
        "/api/scene/urdf/manifest"
    )
    assert response.status_code == 200
    links = {link["name"]: link for link in response.json()["links"]}
    tool_transform = links["tool_link"]["neutral_transform"]
    assert tool_transform["position_m"] == pytest.approx([1.0, 1.0, 0.0])
    assert tool_transform["orientation_xyzw"] == pytest.approx(
        [0.0, 0.0, 2.0**-0.5, 2.0**-0.5]
    )


@pytest.mark.parametrize("invalid_kind", ["disconnected", "non_rigid_origin"])
def test_manifest_fails_closed_for_incomplete_or_invalid_authored_tree(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    invalid_kind: str,
) -> None:
    urdf = tmp_path / "robot.urdf"
    urdf.write_text("fixture", encoding="utf-8")

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "tool_link": _FakeLink(),
            }
            invalid_origin = _transform()
            if invalid_kind == "non_rigid_origin":
                invalid_origin[0][0] = 2.0
                self.joint_map = {
                    "mount": _FakeJoint(
                        "mount",
                        "base_link",
                        "tool_link",
                        origin=invalid_origin,
                    )
                }
            else:
                self.joint_map = {}

        @staticmethod
        def load(_urdf_path: str, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(tmp_path, urdf))).get(
        "/api/scene/urdf/manifest"
    )
    assert response.status_code == 404
    assert response.json() == {
        "detail": "No URDF available for the active deployment"
    }


def test_manifest_is_absent_without_authored_deployment() -> None:
    response = TestClient(create_app(ComposerState())).get(
        "/api/scene/urdf/manifest"
    )
    assert response.status_code == 404
