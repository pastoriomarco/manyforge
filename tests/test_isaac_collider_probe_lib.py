from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


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
