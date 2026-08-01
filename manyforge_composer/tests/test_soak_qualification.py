"""Current cache-bound checks plus the historical soak disposition.

Historical blob ``d574829042bf0be7e6f624754a0f1ab1049db728`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains six cases.
The two structural URDF/GLB cache cases remain valid and are ported below.
Dynamic viewer/gripper and Composer-owned historian/archive soak return only
at their named gates.
"""

from __future__ import annotations

from pathlib import Path
import sys
from types import SimpleNamespace
from typing import Final

import pytest

from manyforge_composer.backend import routes_urdf


HISTORICAL_CASE_COUNT: Final = 6
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "dynamic_scene_batch_fk",
        "historical line 238",
        1,
        ("I9", "I"),
        "one batched visual FK operation per source observation",
    ),
    (
        "addressed_gripper_visual_state",
        "historical line 259",
        1,
        ("I6", "N", "I9", "I"),
        "model limits resolve once per response and no command mutation occurs",
    ),
    (
        "structural_asset_cache_bounds",
        "historical lines 310-338",
        2,
        (),
        "retained below against the current structural URDF shell",
    ),
    (
        "historian_page_and_archive_retention",
        "historical lines 380-415",
        2,
        ("I5", "H0"),
        "bounded history pages and byte-bounded failure evidence retention",
    ),
)


class _FakeUrdf:
    base_link = "base_link"
    joint_map: dict[str, object] = {}
    link_map = {
        "base_link": SimpleNamespace(visuals=(), collisions=()),
    }

    @staticmethod
    def load(_urdf_path: str, **_kwargs: object) -> "_FakeUrdf":
        return _FakeUrdf()


class _FakeMesh:
    def export(self, *, file_type: str) -> bytes:
        assert file_type == "glb"
        return b"glb"


def test_urdf_cache_remains_bounded_in_the_structural_shell(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_URDF_CACHE_ENTRIES", 3)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=_FakeUrdf))
    routes_urdf._urdf_cache.clear()
    try:
        paths: list[str] = []
        for index in range(9):
            path = tmp_path / f"robot_{index}.urdf"
            path.write_text(f"<robot name='r{index}'/>", encoding="utf-8")
            paths.append(str(path))
            assert routes_urdf._load_urdf_entry(str(path), ()) is not None
            assert len(routes_urdf._urdf_cache) <= 3

        retained_paths = {key[0] for key in routes_urdf._urdf_cache}
        assert retained_paths == set(paths[-3:])
    finally:
        routes_urdf._urdf_cache.clear()


def test_glb_cache_remains_bounded_per_structural_model(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_GLB_CACHE_ENTRIES_PER_MODEL", 4)
    monkeypatch.setitem(
        sys.modules,
        "trimesh",
        SimpleNamespace(load=lambda *_args, **_kwargs: _FakeMesh()),
    )
    entry = routes_urdf._UrdfCacheEntry(
        urdf_path=str(tmp_path / "robot.urdf"),
        package_paths=(),
        mtime_ns=1,
        root_link="base_link",
        links=(),
        joints=(),
        mesh_glb_cache={},
    )

    for index in range(12):
        mesh_path = tmp_path / f"mesh_{index}.stl"
        mesh_path.write_text("solid fixture", encoding="utf-8")
        visual = routes_urdf._LinkVisual(
            link_name=f"link_{index}",
            visual_index=index,
            mesh_resource_uri=str(mesh_path),
            mesh_path=str(mesh_path),
            mesh_scale=(1.0, 1.0, 1.0),
            primitive=None,
            visual_origin_xyz=(0.0, 0.0, 0.0),
            visual_origin_rpy=(0.0, 0.0, 0.0),
            visual_origin_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
        assert routes_urdf._render_visual_glb(entry, visual) == b"glb"
        assert len(entry.mesh_glb_cache) <= 4

    assert {key[2] for key in entry.mesh_glb_cache} == {8, 9, 10, 11}


def test_historical_soak_cases_are_all_retained_or_gate_assigned() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 6
    retained = [family for family in HISTORICAL_CASE_FAMILIES if not family[3]]
    assert [family[0] for family in retained] == ["structural_asset_cache_bounds"]
