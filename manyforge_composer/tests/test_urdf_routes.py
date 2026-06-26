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


class _FakeCollision:
    def __init__(self, filename: str) -> None:
        self.geometry = _FakeGeometry(filename)
        self.origin = None


class _FakeLink:
    def __init__(
        self,
        visuals: list[_FakeVisual] | None = None,
        collisions: list[_FakeCollision] | None = None,
    ) -> None:
        self.visuals = visuals or []
        self.collisions = collisions or []


class _FakeManifestMimic:
    def __init__(
        self,
        joint: str,
        multiplier: float = 1.0,
        offset: float = 0.0,
    ) -> None:
        self.joint = joint
        self.multiplier = multiplier
        self.offset = offset


class _FakeManifestJoint:
    def __init__(
        self,
        name: str,
        *,
        joint_type: str = "revolute",
        parent: str = "base_link",
        child: str | None = None,
        mimic: _FakeManifestMimic | None = None,
    ) -> None:
        self.name = name
        self.type = joint_type
        self.axis = [1.0, 0.0, 0.0]
        self.parent = parent
        self.child = child or f"{name}_link"
        self.limit = SimpleNamespace(lower=-1.0, upper=1.0, effort=1.0, velocity=1.0)
        self.mimic = mimic


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


def test_urdf_manifest_exposes_collision_geometry_separately(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf = tmp_path / "robot.urdf"
    visual_mesh = tmp_path / "visual.stl"
    collision_mesh = tmp_path / "collision.stl"
    for path in (urdf, visual_mesh, collision_mesh):
        path.write_text("fixture")

    class FakeUrdf:
        base_link = "base_link"
        joint_map: dict[str, object] = {}

        def __init__(self) -> None:
            self.link_map = {
                "tool_link": _FakeLink(
                    visuals=[_FakeVisual(str(visual_mesh))],
                    collisions=[_FakeCollision(str(collision_mesh))],
                ),
            }

        @staticmethod
        def load(_urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

    monkeypatch.setattr(routes_urdf, "default_package_paths", lambda: [])
    monkeypatch.setattr(
        routes_urdf,
        "resolve_robot_model",
        lambda **kwargs: SimpleNamespace(
            urdf_path=str(kwargs["urdf_path"]),
            package_paths=[],
        ),
    )
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=FakeUrdf))
    routes_urdf._urdf_cache.clear()  # type: ignore[attr-defined]

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="ur",
            group_id="arm",
            urdf_path=str(urdf),
            srdf_path=None,
            package_paths=[],
        )
    )
    client = TestClient(create_app(state))

    response = client.get("/api/scene/urdf/manifest")
    assert response.status_code == 200
    manifest = response.json()
    link = manifest["links"][0]
    assert link["visuals"][0]["mesh_url"] != link["collisions"][0]["mesh_url"]
    assert "/meshes/tool_link/0" in link["visuals"][0]["mesh_url"]
    assert "/collision-meshes/tool_link/0" in link["collisions"][0]["mesh_url"]
    assert link["collisions"][0]["collision_origin"]["orientation_xyzw"] == [0.0, 0.0, 0.0, 1.0]


def test_urdf_manifest_surfaces_robot_model_authority_and_keys_by_fingerprint(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    urdf = tmp_path / "lite6.urdf"
    urdf.write_text("fixture")

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "link6": _FakeLink([_FakeVisual(str(tmp_path / "mesh.stl"))]),
                "left_finger_link": _FakeLink(),
                "right_finger_link": _FakeLink(),
            }
            self.joint_map = {
                **{
                    f"joint{i}": _FakeManifestJoint(
                        f"joint{i}",
                        parent="base_link",
                        child=f"link{i}",
                    )
                    for i in range(1, 7)
                },
                "right_finger_joint": _FakeManifestJoint(
                    "right_finger_joint",
                    joint_type="prismatic",
                    parent="link6",
                    child="right_finger_link",
                ),
                "left_finger_joint": _FakeManifestJoint(
                    "left_finger_joint",
                    joint_type="prismatic",
                    parent="link6",
                    child="left_finger_link",
                    mimic=_FakeManifestMimic("right_finger_joint"),
                ),
            }

        @staticmethod
        def load(_urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

    def fake_resolve_robot_model(**kwargs: object) -> SimpleNamespace:
        return SimpleNamespace(
            urdf_path=str(kwargs["urdf_path"]),
            package_paths=[],
        )

    monkeypatch.setattr(routes_urdf, "default_package_paths", lambda: [])
    monkeypatch.setattr(routes_urdf, "resolve_robot_model", fake_resolve_robot_model)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=FakeUrdf))
    routes_urdf._urdf_cache.clear()  # type: ignore[attr-defined]

    kernel = MockKernelClient()
    kernel.robot_model_authority_metadata = {
        "canonical_fingerprint": "fingerprint-a",
        "view_fingerprint": "view-a",
        "view_changed": True,
        "commandable_joint_names": [
            "joint1",
            "joint2",
            "joint3",
            "joint4",
            "joint5",
            "joint6",
            "right_finger_joint",
        ],
        "group_commandable_joint_names": {
            "lite6": ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"],
            "lite_gripper": ["right_finger_joint"],
        },
        "grippers": {
            "primary": {
                "group_id": "lite_gripper",
                "command_joint": "right_finger_joint",
                "derived_joints": ["left_finger_joint"],
                "presets": {"open": 0.0081, "close": 0.0},
            }
        },
    }

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="lite6",
            group_id="lite6",
            urdf_path=str(urdf),
            srdf_path=None,
            package_paths=[],
        )
    )
    client = TestClient(create_app(state))

    first = client.get("/api/scene/urdf/manifest")
    assert first.status_code == 200
    manifest = first.json()
    assert manifest["authority"]["canonical_fingerprint"] == "fingerprint-a"
    assert manifest["authority"]["group_commandable_joint_names"]["lite6"] == [
        "joint1",
        "joint2",
        "joint3",
        "joint4",
        "joint5",
        "joint6",
    ]
    assert manifest["authority"]["grippers"]["primary"]["command_joint"] == (
        "right_finger_joint"
    )
    assert manifest["authority"]["grippers"]["primary"]["derived_joints"] == [
        "left_finger_joint"
    ]
    assert "models/" + manifest["model_key"] in manifest["links"][0]["visuals"][0]["mesh_url"]

    kernel.robot_model_authority_metadata = {
        **kernel.robot_model_authority_metadata,
        "canonical_fingerprint": "fingerprint-b",
    }
    second = client.get("/api/scene/urdf/manifest")
    assert second.status_code == 200
    assert second.json()["model_key"] != manifest["model_key"]


# ----------------------------------------------------------------------
# Gripper visualization resolution (ADR-0018 unreachable-ghost fallback).
# The Move-To unreachable branch needs the first downstream link with
# visuals reachable from `mount_frame`, plus the static SE(3) from
# `tcp_frame` to that link. This is the canonical UR10e+Robotiq case:
# tcp_frame is a virtual grasp_frame (no visuals); mount_frame is
# tool0 (also no visuals); the gripper root is robotiq_base_link.
# ----------------------------------------------------------------------


def _make_state(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, fake_urdf_cls) -> ComposerState:
    urdf_path = tmp_path / "robot.urdf"
    urdf_path.write_text("fixture")
    monkeypatch.setattr(routes_urdf, "default_package_paths", lambda: [])
    monkeypatch.setattr(
        routes_urdf,
        "resolve_robot_model",
        lambda **kwargs: SimpleNamespace(urdf_path=str(kwargs["urdf_path"]), package_paths=[]),
    )
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=fake_urdf_cls))
    routes_urdf._urdf_cache.clear()  # type: ignore[attr-defined]
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.deployment_source_path = str(tmp_path / "deployment.yaml")
    state.deployment = SimpleNamespace(
        robot_config=SimpleNamespace(
            robot_id="ur10e",
            group_id="arm",
            urdf_path=str(urdf_path),
            srdf_path=None,
            package_paths=[],
        )
    )
    return state


class _FakeJoint:
    def __init__(self, name: str, parent: str, child: str, joint_type: str = "fixed") -> None:
        self.name = name
        self.parent = parent
        self.child = child
        self.type = joint_type
        self.axis = [1.0, 0.0, 0.0]
        self.origin = None
        self.limit = None
        self.mimic = None


def test_resolve_gripper_visualization_for_virtual_tcp_chain(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """UR10e+Robotiq canonical case: mount=tool0 (no visuals),
    tcp=grasp_frame (no visuals), gripper-root=robotiq_base_link (has visuals)."""
    import numpy as np  # local — pytest will skip the test if missing

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "tool0": _FakeLink(),  # mount, no visuals
                "robotiq_base_link": _FakeLink([_FakeVisual("dummy_mesh")]),
                "grasp_frame": _FakeLink(),  # tcp, no visuals
                "left_outer_knuckle": _FakeLink([_FakeVisual("knuckle_mesh")]),
            }
            self.joint_map = {
                "tool0_to_robotiq": _FakeJoint("tool0_to_robotiq", "tool0", "robotiq_base_link"),
                "robotiq_to_grasp": _FakeJoint("robotiq_to_grasp", "robotiq_base_link", "grasp_frame"),
                "finger_joint": _FakeJoint(
                    "finger_joint", "robotiq_base_link", "left_outer_knuckle", "revolute"
                ),
            }

        @staticmethod
        def load(urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

        def get_transform(self, frame_to: str, frame_from: str):
            # grasp_frame is a fixed child of robotiq_base_link with
            # identity origin in this fixture; transforms between
            # the two are identity.
            return np.eye(4)

    state = _make_state(tmp_path, monkeypatch, FakeUrdf)
    result = routes_urdf.resolve_gripper_visualization(
        state, mount_frame="tool0", tcp_frame="grasp_frame",
        srdf_gripper_joint_names=("finger_joint",),
    )
    assert result is not None
    assert result.gripper_root_link == "robotiq_base_link"
    # Identity transform within tolerance — Shepperd's-method extraction
    # of an identity rotation returns (0, 0, 0, 1).
    assert result.tcp_to_root_position_m == (0.0, 0.0, 0.0)
    qx, qy, qz, qw = result.tcp_to_root_orientation_xyzw
    assert abs(qx) < 1e-9 and abs(qy) < 1e-9 and abs(qz) < 1e-9
    assert abs(qw - 1.0) < 1e-9
    # The full subtree must include the root + descendants reachable
    # via the URDF joint graph (grasp_frame, left_outer_knuckle here).
    subtree_names = {link.name for link in result.subtree_links}
    assert "robotiq_base_link" in subtree_names
    assert "left_outer_knuckle" in subtree_names
    # The root itself appears with identity transform.
    root_entry = next(
        link for link in result.subtree_links if link.name == "robotiq_base_link"
    )
    assert root_entry.position_m == (0.0, 0.0, 0.0)
    assert root_entry.orientation_xyzw == (0.0, 0.0, 0.0, 1.0)


def test_resolve_gripper_visualization_returns_none_when_no_visual_descendant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """No downstream link from mount_frame has visuals — should
    return None so the frontend falls through to the floor marker."""

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "base_link": _FakeLink(),
                "tool0": _FakeLink(),
                "phantom_a": _FakeLink(),  # no visuals
                "phantom_b": _FakeLink(),  # no visuals
            }
            self.joint_map = {
                "tool0_to_phantom_a": _FakeJoint("tool0_to_phantom_a", "tool0", "phantom_a"),
                "phantom_a_to_phantom_b": _FakeJoint("phantom_a_to_phantom_b", "phantom_a", "phantom_b"),
            }

        @staticmethod
        def load(urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

        def get_transform(self, *args, **kwargs):  # pragma: no cover - never reached
            return None

    state = _make_state(tmp_path, monkeypatch, FakeUrdf)
    result = routes_urdf.resolve_gripper_visualization(
        state, mount_frame="tool0", tcp_frame="phantom_b",
    )
    assert result is None


def test_resolve_gripper_visualization_returns_none_when_mount_frame_unknown(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Mount frame is not in the URDF — fail-closed."""

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {"base_link": _FakeLink()}
            self.joint_map = {}

        @staticmethod
        def load(urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

        def get_transform(self, *args, **kwargs):  # pragma: no cover - never reached
            return None

    state = _make_state(tmp_path, monkeypatch, FakeUrdf)
    result = routes_urdf.resolve_gripper_visualization(
        state, mount_frame="not_a_link", tcp_frame="base_link",
    )
    assert result is None


def test_resolve_gripper_visualization_srdf_tiebreak_prefers_gripper_subtree(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """mount_frame has two children A and B; only B's subtree contains
    the SRDF gripper joint. Resolver MUST prefer B's subtree even if
    A's first-child happens to have visuals."""
    import numpy as np

    class FakeUrdf:
        base_link = "base_link"

        def __init__(self) -> None:
            self.link_map = {
                "tool0": _FakeLink(),
                "decoy_with_visuals": _FakeLink([_FakeVisual("decoy")]),
                "gripper_root": _FakeLink([_FakeVisual("gripper_mesh")]),
                "gripper_finger": _FakeLink([_FakeVisual("finger_mesh")]),
            }
            self.joint_map = {
                "tool0_to_decoy": _FakeJoint("tool0_to_decoy", "tool0", "decoy_with_visuals"),
                "tool0_to_gripper": _FakeJoint("tool0_to_gripper", "tool0", "gripper_root"),
                "finger_joint": _FakeJoint(
                    "finger_joint", "gripper_root", "gripper_finger", "revolute"
                ),
            }

        @staticmethod
        def load(urdf_path: str, **_: object) -> "FakeUrdf":
            return FakeUrdf()

        def get_transform(self, frame_to: str, frame_from: str):
            return np.eye(4)

    state = _make_state(tmp_path, monkeypatch, FakeUrdf)
    result = routes_urdf.resolve_gripper_visualization(
        state, mount_frame="tool0", tcp_frame="gripper_root",
        srdf_gripper_joint_names=("finger_joint",),
    )
    assert result is not None
    assert result.gripper_root_link == "gripper_root"
