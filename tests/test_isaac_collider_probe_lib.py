from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import pytest


HISTORICAL_SOURCE_BLOB = "380c7dc4ff873aa510290b5f6df5a080e9698ab0"
HISTORICAL_SOURCE_SHA256 = (
    "aa8f909cce01521e466ba7f0390d78bca2bf5d4d1629b8dc31377e8f8ae8ad94"
)
HISTORICAL_WRAPPER_BLOB = "f1b78ac419838a403ae74753749a55bbb30b629a"
HISTORICAL_WRAPPER_SHA256 = (
    "2962b68d0da79f3716d72064819dd553bfac545cadc53b19f9fe10dd7bbb6ae3"
)
HISTORICAL_TEST_BLOB = "bdbbbcd839a90161bc3425626093863e24b3a0b8"
HISTORICAL_TEST_SHA256 = (
    "57b5d3c4daa67b90a397eb6f219373844c232b3139299c3375e93fcb07f56abc"
)
HISTORICAL_TEST_NAME_DIGEST = (
    "0792fdbd59a9e9bf54a0b91a91bf40c1e20dce7d2474723baf15401300503226"
)
RESTORATION_GATE = "I9/I"

# Every test definition in HISTORICAL_TEST_BLOB remains executable against the
# retained owner-neutral helpers.  The line ranges are locations in that exact
# blob, not in this evolving test file.  I9 must reuse these outcomes when its
# future adapter supplies owner-issued collider evidence; they do not qualify
# the deleted live probe or its v0.1 wire.
HISTORICAL_ACCEPTANCE_COHORTS = (
    (
        "test_manyforge_recommendation_uses_mesh_when_resource_uri_is_available",
        "62-88",
        "recommend a supplied readable collider mesh with its supplied pose",
    ),
    (
        "test_manyforge_recommendation_preserves_mesh_scale_from_transform",
        "89-114",
        "preserve non-unit scale from supplied transform evidence",
    ),
    (
        "test_manyforge_recommendation_falls_back_to_box_without_mesh_resource_uri",
        "115-133",
        "fall back conservatively to a supplied world AABB",
    ),
    (
        "test_manyforge_recommendation_prefers_exported_original_mesh",
        "134-164",
        "honor an explicitly supplied original-mesh candidate",
    ),
    (
        "test_manyforge_recommendation_can_prefer_collider_mesh_over_original_mesh",
        "165-198",
        "honor explicit collider-source preference",
    ),
    (
        "test_manyforge_recommendation_can_use_exported_collider_mesh",
        "199-237",
        "select an already exported collider mesh",
    ),
    (
        "test_manyforge_recommendation_prefers_exported_cooked_collider_mesh",
        "238-269",
        "prefer supplied cooked-convex mesh evidence",
    ),
    (
        "test_export_primitive_colliders_as_single_obj",
        "270-311",
        "build one deterministic OBJ from supplied primitive collider records",
    ),
    (
        "test_convex_summary_reconstructs_triangle_faces",
        "325-346",
        "triangulate PhysX-style polygon metadata",
    ),
    (
        "test_convex_summary_reconstructs_isaac_num_vertices_polygons",
        "347-371",
        "accept Isaac polygon field spellings without stage access",
    ),
    (
        "test_convex_data_summary_reconstructs_direct_physx_dicts",
        "372-393",
        "normalize an already supplied direct-PhysX dictionary",
    ),
    (
        "test_convex_summary_accepts_triangle_flat_index_buffers",
        "394-414",
        "retain the bounded triangle-flat fallback",
    ),
    (
        "test_convex_summary_can_store_vertices_in_target_frame",
        "415-446",
        "transform supplied convex vertices into a supplied target frame",
    ),
)


def _load_probe_module():
    repo_root = Path(__file__).resolve().parents[1]
    module_path = repo_root / "scripts" / "isaac_collider_probe_lib.py"
    spec = importlib.util.spec_from_file_location("isaac_collider_probe_lib_under_test", module_path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


probe = _load_probe_module()


def _collider(**overrides):
    data = {
        "path": "/World/can/mesh",
        "name": "mesh",
        "type_name": "Mesh",
        "active": True,
        "visibility": "inherited",
        "computed_visibility": "inherited",
        "purpose": "default",
        "applied_schemas": [],
        "parent_path": "/World/can",
        "rigid_body_path": None,
        "articulation_root_path": None,
        "xformable": True,
        "local_transform": None,
        "world_transform": [
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.1, -0.2, 0.3, 1.0],
        ],
        "local_aabb": None,
        "world_aabb": {
            "min": [0.0, 0.0, 0.0],
            "max": [1.0, 1.0, 1.0],
            "size": [1.0, 1.0, 1.0],
            "center": [0.5, 0.5, 0.5],
        },
        "collision_enabled": True,
        "collision_api": {},
        "mesh_collision_api": {"approximation": "convexHull"},
        "physx_collision_schemas": {},
        "geometry_schema": {"schema": "mesh"},
        "cooked_convex": probe.CookedConvexSummary(attempted=False, available=False),
        "errors": [],
    }
    data.update(overrides)
    return probe.ColliderRecord(**data)


def _write_minimal_obj(
    output_directory: Path,
    *,
    source_root: str = "/World/object",
    filename_suffix: str = "collider",
    extra=None,
):
    return probe._write_obj_mesh_record(
        source="exported_collider_mesh",
        source_root=source_root,
        prim_path=source_root,
        root_world=None,
        vertices=[[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
        faces=[[1, 2, 3]],
        mesh_export_dir=str(output_directory),
        fallback_aabb={"center": [0.0, 0.0, 0.0]},
        filename_suffix=filename_suffix,
        extra=extra,
    )


def test_manyforge_recommendation_uses_mesh_when_resource_uri_is_available() -> None:
    warnings: list[str] = []
    collider = _collider(
        geometry_schema={
            "schema": "mesh",
            "mesh_resource_uri": "file:///workspace/meshes/can.obj",
        }
    )

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[],
        union_aabb=collider.world_aabb,
        warnings=warnings,
    )

    assert recommendation["shape"] == {
        "type": "mesh",
        "mesh_resource_uri": "file:///workspace/meshes/can.obj",
        "mesh_scale": [1.0, 1.0, 1.0],
    }
    assert recommendation["pose"]["position_m"] == [0.1, -0.2, 0.3]
    assert recommendation["pose"]["orientation_rpy_deg"] == [0.0, -0.0, 0.0]
    assert any("readable by the ManyForge runtime" in warning for warning in warnings)


def test_manyforge_recommendation_preserves_mesh_scale_from_transform() -> None:
    warnings: list[str] = []
    collider = _collider(
        geometry_schema={
            "schema": "mesh",
            "mesh_resource_uri": "file:///workspace/meshes/table.obj",
        },
        world_transform=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.5, 0.0, 0.0],
            [0.0, 0.0, 1.15, 0.0],
            [0.0, 0.0, -0.7696, 1.0],
        ],
    )

    recommendation = probe._manyforge_shape_recommendation(
        root="/background/table_low_327",
        colliders=[collider],
        object_meshes=[],
        union_aabb=collider.world_aabb,
        warnings=warnings,
    )

    assert recommendation["shape"]["mesh_scale"] == [1.0, 1.5, 1.15]


def test_manyforge_recommendation_falls_back_to_box_without_mesh_resource_uri() -> None:
    warnings: list[str] = []
    collider = _collider()

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[],
        union_aabb=collider.world_aabb,
        warnings=warnings,
    )

    assert recommendation["shape"] == {
        "type": "box",
        "box_dimensions_m": [1.0, 1.0, 1.0],
    }
    assert recommendation["pose"]["position_m"] == [0.5, 0.5, 0.5]


def test_manyforge_recommendation_prefers_exported_original_mesh() -> None:
    warnings: list[str] = []
    collider = _collider(
        geometry_schema={
            "schema": "mesh",
            "mesh_resource_uri": "file:///workspace/collider.obj",
        }
    )

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[
            {
                "source": "exported_original_mesh",
                "mesh_resource_uri": "file:///workspace/program_meshes/can.obj",
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.1, -0.2, 0.3],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            }
        ],
        union_aabb=collider.world_aabb,
        warnings=warnings,
    )

    assert recommendation["shape"]["mesh_resource_uri"] == "file:///workspace/program_meshes/can.obj"
    assert "original object mesh exported from Isaac" in recommendation["reason"]


def test_manyforge_recommendation_can_prefer_collider_mesh_over_original_mesh() -> None:
    warnings: list[str] = []
    collider = _collider(
        geometry_schema={
            "schema": "mesh",
            "mesh_resource_uri": "file:///workspace/collider.obj",
        }
    )

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[
            {
                "source": "exported_original_mesh",
                "mesh_resource_uri": "file:///workspace/program_meshes/can.obj",
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.1, -0.2, 0.3],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            }
        ],
        union_aabb=collider.world_aabb,
        warnings=warnings,
        mesh_source_preference="collider",
    )

    assert recommendation["shape"]["mesh_resource_uri"] == "file:///workspace/collider.obj"
    assert recommendation["mesh_source"] == "referenced_collider_mesh"
    assert recommendation["mesh_source_prim_path"] == "/World/can/mesh"
    assert "mesh_source_preference='collider'" in recommendation["reason"]


def test_manyforge_recommendation_can_use_exported_collider_mesh() -> None:
    warnings: list[str] = []
    collider = _collider()

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[
            {
                "source": "exported_collider_mesh",
                "prim_path": "/World/can/collider",
                "mesh_resource_uri": "file:///workspace/program_meshes/can_collider.obj",
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.1, -0.2, 0.3],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            },
            {
                "source": "exported_original_mesh",
                "mesh_resource_uri": "file:///workspace/program_meshes/can_visual.obj",
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.1, -0.2, 0.3],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            },
        ],
        union_aabb=collider.world_aabb,
        warnings=warnings,
        mesh_source_preference="collider",
    )

    assert recommendation["shape"]["mesh_resource_uri"] == (
        "file:///workspace/program_meshes/can_collider.obj"
    )
    assert recommendation["mesh_source"] == "exported_collider_mesh"


def test_manyforge_recommendation_prefers_exported_cooked_collider_mesh() -> None:
    warnings: list[str] = []
    collider = _collider()

    recommendation = probe._manyforge_shape_recommendation(
        root="/World/can",
        colliders=[collider],
        object_meshes=[
            {
                "source": "exported_cooked_convex_collider_mesh",
                "prim_path": "/World/can",
                "mesh_resource_uri": "file:///workspace/program_meshes/can_cooked.obj",
                "pose": {
                    "frame_id": "universe",
                    "position_m": [0.1, -0.2, 0.3],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
                "convex_hull_count": 3,
            }
        ],
        union_aabb=collider.world_aabb,
        warnings=warnings,
        mesh_source_preference="collider",
    )

    assert recommendation["shape"]["mesh_resource_uri"] == (
        "file:///workspace/program_meshes/can_cooked.obj"
    )
    assert recommendation["mesh_source"] == "exported_cooked_convex_collider_mesh"
    assert recommendation["confidence"] == "high"


def test_export_primitive_colliders_as_single_obj(tmp_path: Path) -> None:
    collider_a = _collider(
        path="/World/small_KLT/Collision/Cube",
        geometry_schema={"schema": "cube", "size": 1.0},
        mesh_collision_api={"approximation": "none"},
        world_transform=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ],
    )
    collider_b = _collider(
        path="/World/small_KLT/Collision/Cube_01",
        geometry_schema={"schema": "cube", "size": 1.0},
        mesh_collision_api={"approximation": "none"},
        world_transform=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [2.0, 0.0, 0.0, 1.0],
        ],
    )

    record = probe._export_primitive_collider_obj(
        source_root="/World/small_KLT",
        root_world=None,
        colliders=[collider_a, collider_b],
        mesh_export_dir=str(tmp_path),
        fallback_aabb=collider_a.world_aabb,
    )

    assert record is not None
    assert record["source"] == "exported_primitive_collider_mesh"
    assert record["primitive_collider_count"] == 2
    assert record["point_count"] == 16
    assert record["face_count"] == 24
    mesh_path = Path(record["mesh_resource_uri"].removeprefix("file://"))
    text = mesh_path.read_text(encoding="utf-8")
    assert text.count("\nv ") == 16
    assert text.count("\nf ") == 24


class _FakePolygon:
    def __init__(self, base: int, count: int) -> None:
        self.mIndexBase = base
        self.mNbVerts = count


class _FakeIsaacPolygon:
    def __init__(self, base: int, count: int) -> None:
        self.index_base = base
        self.num_vertices = count


def test_convex_summary_reconstructs_triangle_faces() -> None:
    convex = type("FakeConvex", (), {})()
    convex.vertices = [
        [0.0, 0.0, 0.0],
        [1.0, 0.0, 0.0],
        [1.0, 1.0, 0.0],
        [0.0, 1.0, 0.0],
    ]
    convex.indices = [0, 1, 2, 3]
    convex.polygons = [_FakePolygon(0, 4)]

    summary = probe._convex_summary(
        convex,
        world_matrix=None,
        include_samples=False,
        max_sample_items=8,
    )

    assert summary["vertices"] == convex.vertices
    assert summary["faces"] == [[0, 1, 2], [0, 2, 3]]


def test_convex_summary_reconstructs_isaac_num_vertices_polygons() -> None:
    convex = type("FakeConvex", (), {})()
    convex.vertices = [
        [0.0, 0.0, 0.0],
        [1.0, 0.0, 0.0],
        [1.0, 1.0, 0.0],
        [0.0, 1.0, 0.0],
        [0.0, 0.0, 1.0],
    ]
    convex.indices = [0, 1, 2, 3, 0, 2, 4]
    convex.polygons = [
        _FakeIsaacPolygon(0, 4),
        {"index_base": 4, "num_vertices": 3},
    ]

    summary = probe._convex_summary(
        convex,
        world_matrix=None,
        include_samples=False,
        max_sample_items=8,
    )

    assert summary["faces"] == [[0, 1, 2], [0, 2, 3], [0, 2, 4]]


def test_convex_data_summary_reconstructs_direct_physx_dicts() -> None:
    summary = probe._convex_data_summary(
        {
            "vertices": [
                [0.0, 0.0, 0.0],
                [1.0, 0.0, 0.0],
                [1.0, 1.0, 0.0],
                [0.0, 1.0, 0.0],
            ],
            "indices": [0, 1, 2, 3],
            "polygons": [{"index_base": 0, "num_vertices": 4}],
        },
        world_matrix=None,
        include_samples=False,
        max_sample_items=8,
        source_world_matrix=None,
        target_inverse_matrix=None,
    )

    assert summary["faces"] == [[0, 1, 2], [0, 2, 3]]


def test_convex_summary_accepts_triangle_flat_index_buffers() -> None:
    convex = type("FakeConvex", (), {})()
    convex.vertices = [
        [0.0, 0.0, 0.0],
        [1.0, 0.0, 0.0],
        [0.0, 1.0, 0.0],
        [0.0, 0.0, 1.0],
    ]
    convex.indices = [0, 1, 2, 0, 2, 3]
    convex.polygons = [object(), object()]

    summary = probe._convex_summary(
        convex,
        world_matrix=None,
        include_samples=False,
        max_sample_items=8,
    )

    assert summary["faces"] == [[0, 1, 2], [0, 2, 3]]


def test_convex_summary_can_store_vertices_in_target_frame() -> None:
    convex = type("FakeConvex", (), {})()
    convex.vertices = [[2.0, 0.0, 0.0]]
    convex.indices = []
    convex.polygons = []

    summary = probe._convex_summary(
        convex,
        world_matrix=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [10.0, 0.0, 0.0, 1.0],
        ],
        include_samples=False,
        max_sample_items=8,
        source_world_matrix=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [1.0, 0.0, 0.0, 1.0],
        ],
        target_inverse_matrix=[
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [-10.0, 0.0, 0.0, 1.0],
        ],
    )

    assert summary["vertices"] == [[-7.0, 0.0, 0.0]]
    assert summary["world_aabb"]["center"] == [3.0, 0.0, 0.0]


@pytest.mark.parametrize(
    ("field", "bound_name", "message"),
    (
        ("vertices", "MAX_VERTICES", "convex vertices"),
        ("indices", "MAX_INDICES", "convex indices"),
        ("polygons", "MAX_POLYGONS", "convex polygons"),
    ),
)
def test_convex_collections_are_bounded_before_materialization(
    monkeypatch,
    field: str,
    bound_name: str,
    message: str,
) -> None:
    convex = type("FakeConvex", (), {})()
    convex.vertices = [[0.0, 0.0, 0.0]]
    convex.indices = []
    convex.polygons = []
    setattr(convex, field, [object(), object()])
    monkeypatch.setattr(probe, bound_name, 1)

    with pytest.raises(ValueError, match=message):
        probe._convex_summary(
            convex,
            world_matrix=None,
            include_samples=False,
            max_sample_items=0,
        )


def test_collider_and_generated_face_counts_are_bounded_before_growth(
    monkeypatch,
) -> None:
    monkeypatch.setattr(probe, "MAX_COLLIDERS", 1)
    with pytest.raises(ValueError, match="colliders exceeds"):
        probe._manyforge_shape_recommendation(
            root="/World/object",
            colliders=[_collider(), _collider(path="/World/object/second")],
            object_meshes=[],
            union_aabb=_collider().world_aabb,
            warnings=[],
        )

    monkeypatch.setattr(probe, "MAX_FACES", 1)
    with pytest.raises(ValueError, match="convex faces exceeds"):
        probe._convex_data_summary(
            {
                "vertices": [
                    [0.0, 0.0, 0.0],
                    [1.0, 0.0, 0.0],
                    [1.0, 1.0, 0.0],
                    [0.0, 1.0, 0.0],
                ],
                "indices": [0, 1, 2, 3],
                "polygons": [{"index_base": 0, "num_vertices": 4}],
            },
            world_matrix=None,
            include_samples=False,
            max_sample_items=0,
            source_world_matrix=None,
            target_inverse_matrix=None,
        )


def test_primitive_tessellation_dimensions_are_bounded_before_allocation(
    monkeypatch,
) -> None:
    with pytest.raises(ValueError, match="sphere segments"):
        probe._sphere_mesh(1.0, segments=probe.MAX_SEGMENTS + 1, rings=2)
    with pytest.raises(ValueError, match="sphere rings"):
        probe._sphere_mesh(1.0, segments=3, rings=probe.MAX_RINGS + 1)
    with pytest.raises(ValueError, match="cylinder segments"):
        probe._cylinder_mesh(
            1.0,
            1.0,
            "Z",
            segments=probe.MAX_SEGMENTS + 1,
        )

    monkeypatch.setattr(probe, "MAX_VERTICES", 7)
    with pytest.raises(ValueError, match="sphere vertices exceeds"):
        probe._sphere_mesh(1.0, segments=3, rings=3)


@pytest.mark.parametrize("invalid", (True, False, float("nan"), float("inf"), "1"))
def test_geometry_requires_finite_real_values_that_are_not_bool(invalid) -> None:
    with pytest.raises(ValueError):
        probe._cube_mesh(invalid)
    with pytest.raises(ValueError):
        probe._vector_to_list([0.0, invalid, 0.0])


@pytest.mark.parametrize(
    "indices",
    (
        [0, 1, True],
        [0, 1, 1.5],
        [0, 1, -1],
        [0, 1, 3],
    ),
)
def test_convex_and_obj_faces_require_exact_in_range_indices(
    indices,
    tmp_path: Path,
) -> None:
    with pytest.raises(ValueError):
        probe._convex_faces(indices, [], vertex_count=3)
    with pytest.raises(ValueError):
        probe._write_obj_mesh_record(
            source="exported_collider_mesh",
            source_root="/World/object",
            prim_path="/World/object",
            root_world=None,
            vertices=[
                [0.0, 0.0, 0.0],
                [1.0, 0.0, 0.0],
                [0.0, 1.0, 0.0],
            ],
            faces=[indices],
            mesh_export_dir=str(tmp_path),
            fallback_aabb={"center": [0.0, 0.0, 0.0]},
            filename_suffix="collider",
        )


def test_polygon_ranges_are_exact_and_cannot_slice_past_indices() -> None:
    with pytest.raises(ValueError, match="integer, not bool"):
        probe._convex_faces(
            [0, 1, 2],
            [{"index_base": True, "num_vertices": 3}],
            vertex_count=3,
        )
    with pytest.raises(ValueError, match="index range exceeds"):
        probe._convex_faces(
            [0, 1, 2],
            [{"index_base": 1, "num_vertices": 3}],
            vertex_count=3,
        )


def test_polygon_samples_are_cycle_depth_and_text_bounded(monkeypatch) -> None:
    cycle: list[object] = []
    cycle.append(cycle)
    with pytest.raises(ValueError, match="cycle"):
        probe._plain_value(cycle)

    nested: object = "leaf"
    for _index in range(probe.MAX_SAMPLE_DEPTH + 1):
        nested = [nested]
    with pytest.raises(ValueError, match="depth bound"):
        probe._plain_value(nested)

    monkeypatch.setattr(probe, "MAX_TEXT_BYTES", 4)
    with pytest.raises(ValueError, match="4-byte bound"):
        probe._plain_value("12345")


def test_polygon_samples_never_stringify_unsupported_objects() -> None:
    class PoisonString:
        def __str__(self) -> str:
            raise AssertionError("unsupported sample was stringified")

    with pytest.raises(ValueError, match="no supported bounded representation"):
        probe._plain_value(PoisonString())


def test_polygon_samples_preserve_bounded_json_values() -> None:
    assert probe._plain_value(
        [{"index_base": 0, "num_vertices": 3, "weights": (0.5, 1)}]
    ) == [
        {"index_base": 0, "num_vertices": 3, "weights": [0.5, 1]}
    ]


def test_transform_failures_never_fall_back_to_raw_or_identity_geometry() -> None:
    class BrokenTransform:
        def Transform(self, _point):
            raise RuntimeError("broken")

        def __getitem__(self, index):
            return (
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            )[index]

    with pytest.raises(ValueError, match="Transform operation failed"):
        probe._transform_point(BrokenTransform(), [1.0, 2.0, 3.0])
    with pytest.raises(ValueError, match="supplied together"):
        probe._convex_vertices_in_target_frame(
            [[1.0, 2.0, 3.0]],
            source_world_matrix=[
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
            target_inverse_matrix=None,
        )
    with pytest.raises(ValueError, match="exactly four rows"):
        probe._mesh_pose_from_transform(
            [[1.0, 0.0, 0.0, 0.0]],
            {"center": [9.0, 9.0, 9.0]},
        )
    with pytest.raises(ValueError, match="singular"):
        probe._inverse_matrix([[0.0] * 4 for _ in range(4)])


def test_primitive_export_requires_each_collider_world_transform(
    tmp_path: Path,
) -> None:
    collider = _collider(
        geometry_schema={"schema": "cube", "size": 1.0},
        world_transform=None,
    )
    with pytest.raises(ValueError, match="has no world transform"):
        probe._export_primitive_collider_obj(
            source_root="/World/object",
            root_world=None,
            colliders=[collider],
            mesh_export_dir=str(tmp_path),
            fallback_aabb=collider.world_aabb,
        )
    assert list(tmp_path.iterdir()) == []

    referenced_mesh = _collider(
        geometry_schema={
            "schema": "mesh",
            "mesh_resource_uri": "file:///workspace/object.obj",
        },
        world_transform=None,
    )
    with pytest.raises(ValueError, match="has no world transform"):
        probe._collider_mesh_candidates(
            [referenced_mesh],
            referenced_mesh.world_aabb,
        )


def test_obj_extra_fields_cannot_override_core_record_fields(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="must not override core field"):
        _write_minimal_obj(tmp_path, extra={"point_count": 999})
    assert list(tmp_path.iterdir()) == []


def test_obj_suffix_is_strict_and_source_root_is_contained(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="filename suffix"):
        _write_minimal_obj(tmp_path, filename_suffix="../../escape")
    assert not (tmp_path.parent / "escape.obj").exists()

    record = _write_minimal_obj(
        tmp_path,
        source_root="/../../outside\nobject?",
        filename_suffix="safe",
    )
    output = Path(record["mesh_resource_uri"].removeprefix("file://"))
    assert output.parent == tmp_path.resolve()
    assert output.is_file()


def test_obj_output_rejects_directory_and_destination_symlinks(
    tmp_path: Path,
) -> None:
    real_directory = tmp_path / "real"
    real_directory.mkdir()
    linked_directory = tmp_path / "linked"
    linked_directory.symlink_to(real_directory, target_is_directory=True)
    with pytest.raises(ValueError, match="must not be a symlink"):
        _write_minimal_obj(linked_directory)

    outside = tmp_path / "outside.obj"
    outside.write_text("sentinel\n", encoding="utf-8")
    destination = real_directory / (
        f"{probe._object_id_hint('/World/object')}_collider.obj"
    )
    destination.symlink_to(outside)
    with pytest.raises(FileExistsError, match="refusing to overwrite"):
        _write_minimal_obj(real_directory)
    assert destination.is_symlink()
    assert outside.read_text(encoding="utf-8") == "sentinel\n"


def test_obj_publication_is_exclusive_and_preserves_existing_bytes(
    tmp_path: Path,
) -> None:
    record = _write_minimal_obj(tmp_path)
    output = Path(record["mesh_resource_uri"].removeprefix("file://"))
    original = output.read_bytes()

    with pytest.raises(FileExistsError, match="refusing to overwrite"):
        _write_minimal_obj(tmp_path)
    assert output.read_bytes() == original
    assert output.is_file() and not output.is_symlink()


def test_obj_output_byte_cap_and_failed_publish_leave_no_partial_file(
    tmp_path: Path,
    monkeypatch,
) -> None:
    monkeypatch.setattr(probe, "MAX_OBJ_OUTPUT_BYTES", 16)
    with pytest.raises(ValueError, match="OBJ output exceeds"):
        _write_minimal_obj(tmp_path)
    assert list(tmp_path.iterdir()) == []

    monkeypatch.setattr(probe, "MAX_OBJ_OUTPUT_BYTES", 1024)

    def fail_link(*_args, **_kwargs):
        raise OSError("injected publication failure")

    monkeypatch.setattr(probe.os, "link", fail_link)
    with pytest.raises(OSError, match="injected publication failure"):
        _write_minimal_obj(tmp_path)
    assert list(tmp_path.iterdir()) == []


def test_historical_algorithm_tests_have_exact_i9_acceptance_mapping() -> None:
    names = tuple(cohort[0] for cohort in HISTORICAL_ACCEPTANCE_COHORTS)
    ordered_name_bytes = ("\n".join(names) + "\n").encode("utf-8")

    assert len(names) == 13
    assert len(set(names)) == len(names)
    assert hashlib.sha256(ordered_name_bytes).hexdigest() == HISTORICAL_TEST_NAME_DIGEST
    assert all(callable(globals().get(name)) for name in names)
    assert all(cohort[1] and cohort[2] for cohort in HISTORICAL_ACCEPTANCE_COHORTS)


def test_live_probe_is_an_early_typed_i9_gate_without_dormant_bodies() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    library_path = repo_root / "scripts" / "isaac_collider_probe_lib.py"
    wrapper_path = repo_root / "scripts" / "isaac-collider-probe.py"
    bundled_copy = (
        repo_root
        / "exts"
        / "manyforge.isaac.collider_service"
        / "manyforge"
        / "isaac"
        / "collider_service"
        / "isaac_collider_probe_lib.py"
    )
    library_source = library_path.read_text(encoding="utf-8")
    wrapper_source = wrapper_path.read_text(encoding="utf-8")

    assert not bundled_copy.exists()
    assert "from isaac_collider_probe_lib import main" in wrapper_source
    assert "argparse" not in wrapper_source
    assert "composable_cell_runtime_unavailable" in library_source
    assert "RESTORATION_GATE: Final = \"I9/I\"" in library_source
    for forbidden_body in (
        "def capture_probe_report(",
        "def _parse_args(",
        "def _load_isaac_modules(",
        "def _get_stage(",
        "def _inspect_collider(",
        "def _temporary_recook_descendant_mesh(",
        "def _inspect_object_meshes(",
        "def _write_payload(",
        "def _llm_scene_authoring_payload(",
        "class ProbeReport",
        "class ProbeOptions",
        "from pxr import",
        "import omni",
    ):
        assert forbidden_body not in library_source
    assert '"version": "manyforge.isaac.collider_report.v0.1"' not in library_source


def test_unavailability_payload_pins_removed_and_retained_responsibilities() -> None:
    payload = probe.unavailable_payload()

    assert payload["reason_code"] == "composable_cell_runtime_unavailable"
    assert payload["message"] == probe.UNAVAILABLE_MESSAGE
    assert payload["restoration_gate"] == RESTORATION_GATE
    assert payload["historical_source_blob"] == HISTORICAL_SOURCE_BLOB
    assert payload["historical_wrapper_blob"] == HISTORICAL_WRAPPER_BLOB
    assert payload["historical_test_blob"] == HISTORICAL_TEST_BLOB
    assert payload["future_owner"] == (
        "I9 retained-product Isaac adapter and its owner-issued collider "
        "inspection/evidence boundary"
    )
    assert payload["removed_contracts"] == [
        "manyforge.isaac.collider_report.v0.1"
    ]
    assert [cohort["lines"] for cohort in payload["removed_source_cohorts"]] == [
        "28-46",
        "87-950",
        "1027-1041,1116-1318",
        "1319-1372,1513-1517,1630-1989",
        "2200-2210,2219-2240,2244-2653,2744-2793",
    ]
    assert payload["exit_code"] == 78


def test_direct_probe_main_rejects_before_parsing_or_writing(
    tmp_path: Path,
    capsys,
) -> None:
    forbidden_output = tmp_path / "must-not-exist.json"

    result = probe.main(["--output", str(forbidden_output), "--stage", "unsafe.usd"])
    captured = capsys.readouterr()
    payload = json.loads(captured.err)

    assert result == 78
    assert captured.out == ""
    assert not forbidden_output.exists()
    assert payload["reason_code"] == "composable_cell_runtime_unavailable"
    assert payload["restoration_gate"] == RESTORATION_GATE
