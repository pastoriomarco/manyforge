"""Current cache-bound checks plus the historical soak disposition.

Historical blob ``d574829042bf0be7e6f624754a0f1ab1049db728`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains six cases.
The two structural URDF/GLB cache cases remain valid and are ported below.
Dynamic viewer/gripper and Composer-owned historian/archive soak return only
at their named gates.
"""

from __future__ import annotations

import sys
from dataclasses import replace
from types import SimpleNamespace
from typing import Final

import pytest

from manyforge_composer.backend import routes_urdf
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.native import ViewerModelAsset


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


def _viewer_asset(index: int) -> ViewerModelAsset:
    payload = f"<robot name='r{index}'/>".encode()
    return ViewerModelAsset(
        asset_kind="urdf",
        cell_id="cell-fixture",
        graph_revision=f"graph-{index}",
        retained_bundle_id=f"sha256:{index:064x}",
        product_id="cell_viewer_model",
        product_contract_id="manyforge.viewer_model",
        product_contract_revision=f"sha256:{'a' * 64}",
        viewer_model_revision=f"sha256:{index + 16:064x}",
        logical_uri=None,
        media_type="application/xml",
        byte_size=len(payload),
        content_digest=f"sha256:{index + 32:064x}",
        bytes=payload,
    )


def test_structural_model_key_is_stable_across_runtime_bundle_replacement() -> None:
    viewer_revision = "sha256:" + "a" * 64
    assert routes_urdf._viewer_model_key(
        "sha256:" + "b" * 64,
        viewer_revision,
    ) == routes_urdf._viewer_model_key(
        "sha256:" + "c" * 64,
        viewer_revision,
    )


def test_urdf_cache_remains_bounded_in_the_structural_shell(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_URDF_CACHE_ENTRIES", 3)
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=_FakeUrdf))
    routes_urdf._urdf_cache.clear()
    try:
        expected_keys: list[str] = []
        for index in range(9):
            asset = _viewer_asset(index)
            expected_keys.append(
                routes_urdf._viewer_model_key(
                    asset.retained_bundle_id,
                    asset.viewer_model_revision,
                )
            )
            assert routes_urdf._load_urdf_entry(asset) is not None
            assert len(routes_urdf._urdf_cache) <= 3

        assert set(routes_urdf._urdf_cache) == set(expected_keys[-3:])
    finally:
        routes_urdf._urdf_cache.clear()


def test_urdf_cache_rebinds_resources_after_runtime_bundle_replacement(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setitem(sys.modules, "yourdfpy", SimpleNamespace(URDF=_FakeUrdf))
    routes_urdf._urdf_cache.clear()
    try:
        first_asset = _viewer_asset(1)
        replacement = replace(
            first_asset,
            retained_bundle_id="sha256:" + "f" * 64,
            graph_revision="graph-replacement",
        )
        first = routes_urdf._load_urdf_entry(first_asset)
        current = routes_urdf._load_urdf_entry(replacement)
        assert first is not None
        assert current is first
        assert current.retained_bundle_id == replacement.retained_bundle_id
        assert current.graph_revision == replacement.graph_revision
    finally:
        routes_urdf._urdf_cache.clear()


def test_glb_cache_remains_bounded_per_structural_model(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(routes_urdf, "_MAX_GLB_CACHE_ENTRIES_PER_MODEL", 4)
    monkeypatch.setitem(
        sys.modules,
        "trimesh",
        SimpleNamespace(load=lambda *_args, **_kwargs: _FakeMesh()),
    )
    entry = routes_urdf._UrdfCacheEntry(
        cell_id="cell-fixture",
        graph_revision="graph-1",
        retained_bundle_id=f"sha256:{'b' * 64}",
        viewer_model_revision=f"sha256:{'c' * 64}",
        urdf_content_digest=f"sha256:{'d' * 64}",
        model_key="model-key",
        root_link="base_link",
        links=(),
        joints=(),
        mesh_glb_cache={},
    )

    class FakeClient:
        def viewer_model_resource(
            self,
            retained_bundle_id: str,
            logical_uri: str,
        ) -> ViewerModelAsset:
            assert retained_bundle_id == entry.retained_bundle_id
            return ViewerModelAsset(
                asset_kind="resource",
                cell_id=entry.cell_id,
                graph_revision=entry.graph_revision,
                retained_bundle_id=retained_bundle_id,
                product_id="cell_viewer_model",
                product_contract_id="manyforge.viewer_model",
                product_contract_revision=f"sha256:{'a' * 64}",
                viewer_model_revision=entry.viewer_model_revision,
                logical_uri=logical_uri,
                media_type="model/stl",
                byte_size=13,
                content_digest=f"sha256:{'e' * 64}",
                bytes=b"solid fixture",
            )

    state = ComposerState(headless_client=FakeClient())
    for index in range(12):
        logical_uri = f"package://fixture/mesh_{index}.stl"
        visual = routes_urdf._LinkVisual(
            link_name=f"link_{index}",
            visual_index=index,
            mesh_resource_uri=logical_uri,
            mesh_scale=(1.0, 1.0, 1.0),
            primitive=None,
            visual_origin_xyz=(0.0, 0.0, 0.0),
            visual_origin_rpy=(0.0, 0.0, 0.0),
            visual_origin_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
        assert routes_urdf._render_visual_glb(state, entry, visual) == b"glb"
        assert len(entry.mesh_glb_cache) <= 4

    assert {key[2] for key in entry.mesh_glb_cache} == {8, 9, 10, 11}


def test_historical_soak_cases_are_all_retained_or_gate_assigned() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 6
    retained = [family for family in HISTORICAL_CASE_FAMILIES if not family[3]]
    assert [family[0] for family in retained] == ["structural_asset_cache_bounds"]
