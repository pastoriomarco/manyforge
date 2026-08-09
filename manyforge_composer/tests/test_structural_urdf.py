"""Structural URDF route tests over the active retained viewer product."""

from __future__ import annotations

import io
import sys
from types import SimpleNamespace
from typing import Any

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend import routes_urdf
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.native import ViewerModelAsset


_BUNDLE_A = f"sha256:{'a' * 64}"
_BUNDLE_B = f"sha256:{'b' * 64}"
_VIEWER_A = f"sha256:{'c' * 64}"
_VIEWER_B = f"sha256:{'d' * 64}"


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


def _urdf_asset(
    marker: bytes = b"fixture-a",
    *,
    retained_bundle_id: str = _BUNDLE_A,
    viewer_model_revision: str = _VIEWER_A,
) -> ViewerModelAsset:
    return ViewerModelAsset(
        asset_kind="urdf",
        cell_id="cell-fixture",
        graph_revision="graph-7",
        retained_bundle_id=retained_bundle_id,
        product_id="cell_viewer_model",
        product_contract_id="manyforge.viewer_model",
        product_contract_revision=f"sha256:{'e' * 64}",
        viewer_model_revision=viewer_model_revision,
        logical_uri=None,
        media_type="application/xml",
        byte_size=len(marker),
        content_digest=f"sha256:{'f' * 64}",
        bytes=marker,
    )


class _FakeHeadlessClient:
    def __init__(self, asset: ViewerModelAsset) -> None:
        self.asset = asset
        self.resources: dict[str, bytes] = {}
        self.resource_calls: list[tuple[str, str]] = []

    def viewer_model_urdf(self) -> ViewerModelAsset:
        return self.asset

    def viewer_model_resource(
        self,
        retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset:
        self.resource_calls.append((retained_bundle_id, logical_uri))
        if retained_bundle_id != self.asset.retained_bundle_id:
            raise HeadlessClientError(
                "viewer_model_selection_stale",
                "viewer-model resource belongs to an inactive retained bundle",
                409,
            )
        try:
            payload = self.resources[logical_uri]
        except KeyError as exc:
            raise HeadlessClientError(
                "viewer_model_resource_not_found",
                "viewer-model resource is absent",
                404,
            ) from exc
        return ViewerModelAsset(
            asset_kind="resource",
            cell_id=self.asset.cell_id,
            graph_revision=self.asset.graph_revision,
            retained_bundle_id=retained_bundle_id,
            product_id=self.asset.product_id,
            product_contract_id=self.asset.product_contract_id,
            product_contract_revision=self.asset.product_contract_revision,
            viewer_model_revision=self.asset.viewer_model_revision,
            logical_uri=logical_uri,
            media_type="model/stl",
            byte_size=len(payload),
            content_digest=f"sha256:{'1' * 64}",
            bytes=payload,
        )


def _state(asset: ViewerModelAsset | None = None) -> ComposerState:
    return ComposerState(
        headless_client=(
            _FakeHeadlessClient(asset or _urdf_asset())
            if asset is not None
            else None
        )
    )


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
    monkeypatch.setitem(
        sys.modules,
        "yourdfpy",
        SimpleNamespace(URDF=fake_urdf_type),
    )


def test_state_has_no_v0_deployment_or_filesystem_model_authority() -> None:
    state = ComposerState(headless_client=_FakeHeadlessClient(_urdf_asset()))

    assert not hasattr(state, "deployment")
    assert not hasattr(state, "deployment_source_path")
    assert state.viewer_model_key is None


def test_manifest_projects_complete_retained_neutral_tree_without_authority(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    visual_uri = "package://fixture/visual.stl"
    collision_uri = "package://fixture/collision.stl"

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "carrier": _FakeLink(),
                "guide": _FakeLink(),
                "tool_link": _FakeLink(
                    visuals=[_FakeVisual(visual_uri)],
                    collisions=[_FakeCollision(collision_uri)],
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
        def load(stream: io.BytesIO, **_kwargs: object) -> "FakeUrdf":
            assert stream.read() == b"fixture-a"
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(_urdf_asset()))).get(
        "/api/scene/urdf/manifest"
    )

    assert response.status_code == 200
    manifest = response.json()
    assert manifest["cell_id"] == "cell-fixture"
    assert manifest["graph_revision"] == "graph-7"
    assert manifest["retained_bundle_id"] == _BUNDLE_A
    assert manifest["viewer_model_revision"] == _VIEWER_A
    assert "authority" not in manifest
    assert manifest["projection"] == {
        "kind": "retained_neutral",
        "source": "active_retained_viewer_model",
        "root_frame": "base_link",
        "position_unit": "m",
        "angle_unit": "rad",
        "quaternion_order": "xyzw",
        "joint_coordinate_policy": "zero_independent_with_retained_mimic",
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
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    mesh_by_marker = {
        b"fixture-a": "package://fixture/mesh_a.stl",
        b"fixture-b": "package://fixture/mesh_b.stl",
    }

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self, marker: bytes) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "tool_link": _FakeLink(
                    visuals=[_FakeVisual(mesh_by_marker[marker])]
                ),
            }
            self.joint_map = {
                "mount": _FakeJoint("mount", "base_link", "tool_link")
            }

        @staticmethod
        def load(stream: io.BytesIO, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf(stream.read())

    _patch_model_loading(monkeypatch, FakeUrdf)
    monkeypatch.setattr(routes_urdf, "_render_visual_glb", lambda *_args: b"glb")
    headless = _FakeHeadlessClient(_urdf_asset())
    state = ComposerState(headless_client=headless)
    client = TestClient(create_app(state))

    first = client.get("/api/scene/urdf/manifest")
    repeated = client.get("/api/scene/urdf/manifest")
    assert first.status_code == repeated.status_code == 200
    assert first.json()["model_key"] == repeated.json()["model_key"]
    first_url = next(
        link for link in first.json()["links"] if link["name"] == "tool_link"
    )["visuals"][0]["mesh_url"]

    headless.asset = _urdf_asset(
        b"fixture-b",
        retained_bundle_id=_BUNDLE_B,
        viewer_model_revision=_VIEWER_B,
    )
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


def test_mesh_route_reads_the_exact_retained_logical_resource_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    logical_uri = "package://fixture/tool.stl"

    class FakeUrdf:
        base_link = "base_link"
        link_map = {
            "base_link": _FakeLink(
                visuals=[_FakeVisual(logical_uri)],
            ),
        }
        joint_map: dict[str, object] = {}

        @staticmethod
        def load(_stream: io.BytesIO, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    class FakeMesh:
        def export(self, *, file_type: str) -> bytes:
            assert file_type == "glb"
            return b"glb"

    def load_mesh(stream: io.BytesIO, **kwargs: object) -> FakeMesh:
        assert stream.read() == b"solid retained"
        assert kwargs == {"file_type": "stl", "force": "mesh"}
        return FakeMesh()

    _patch_model_loading(monkeypatch, FakeUrdf)
    monkeypatch.setitem(
        sys.modules,
        "trimesh",
        SimpleNamespace(load=load_mesh),
    )
    headless = _FakeHeadlessClient(_urdf_asset())
    headless.resources[logical_uri] = b"solid retained"
    client = TestClient(create_app(ComposerState(headless_client=headless)))
    manifest = client.get("/api/scene/urdf/manifest").json()
    mesh_url = manifest["links"][0]["visuals"][0]["mesh_url"]

    first = client.get(mesh_url)
    second = client.get(mesh_url)

    assert first.status_code == second.status_code == 200
    assert first.content == second.content == b"glb"
    assert headless.resource_calls == [(_BUNDLE_A, logical_uri)]


def test_manifest_composes_retained_rotations_and_publishes_xyzw(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
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
        def load(_stream: io.BytesIO, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(_urdf_asset()))).get(
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
def test_manifest_fails_closed_for_incomplete_or_invalid_retained_tree(
    monkeypatch: pytest.MonkeyPatch,
    invalid_kind: str,
) -> None:
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
        def load(_stream: io.BytesIO, **_kwargs: object) -> "FakeUrdf":
            return FakeUrdf()

    _patch_model_loading(monkeypatch, FakeUrdf)
    response = TestClient(create_app(_state(_urdf_asset()))).get(
        "/api/scene/urdf/manifest"
    )
    assert response.status_code == 404
    assert response.json() == {
        "detail": "No URDF available for the active deployment"
    }


def test_manifest_is_absent_without_headless_viewer_source() -> None:
    response = TestClient(create_app(ComposerState())).get(
        "/api/scene/urdf/manifest"
    )
    assert response.status_code == 404
