"""Owner-neutral collider geometry and scene-authoring helpers.

The former 2,793-line Isaac/USD/PhysX probe is pinned at source blob
``380c7dc4ff873aa510290b5f6df5a080e9698ab0``.  Live stage discovery,
temporary-stage recooking, update-loop ownership, report publication, and the
``manyforge.isaac.collider_report.v0.1`` contract are unavailable until I9/I.
They are deliberately not kept behind this module's gate as dormant bodies.

The retained functions are bounded transforms, convex-buffer normalization,
shape recommendation, and primitive OBJ generation.  Their exact historical
source cohorts and all 13 focused test definitions are exposed by
``unavailable_payload`` so future I9 work can recover behavior without treating
the old wire as a compatibility obligation.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass, field
from numbers import Integral, Real
from pathlib import Path
from typing import Any, BinaryIO, Final, Iterable


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I9/I"
HISTORICAL_SOURCE_BLOB: Final = "380c7dc4ff873aa510290b5f6df5a080e9698ab0"
HISTORICAL_WRAPPER_BLOB: Final = "f1b78ac419838a403ae74753749a55bbb30b629a"
HISTORICAL_TEST_BLOB: Final = "bdbbbcd839a90161bc3425626093863e24b3a0b8"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac collider inspection requires the "
    f"{RESTORATION_GATE} restoration gate"
)

_MESH_SOURCE_PREFERENCES = {"auto", "original", "collider", "primitive"}

# These bounds apply before materializing caller-controlled iterables or
# generating tessellated geometry.  They are deliberately independent of the
# unavailable live probe's former CLI limits.
MAX_COLLIDERS: Final = 4_096
MAX_OBJECT_MESH_RECORDS: Final = 4_096
MAX_VERTICES: Final = 262_144
MAX_INDICES: Final = 786_432
MAX_POLYGONS: Final = 262_144
MAX_FACES: Final = 524_288
MAX_SEGMENTS: Final = 1_024
MAX_RINGS: Final = 1_024
MAX_SAMPLE_ITEMS: Final = 4_096
MAX_OBJ_OUTPUT_BYTES: Final = 16 * 1024 * 1024
MAX_PATH_BYTES: Final = 4_096
MAX_TEXT_BYTES: Final = 4_096
MAX_FILENAME_COMPONENT_BYTES: Final = 96
MAX_EXTRA_FIELDS: Final = 128
MAX_SAMPLE_DEPTH: Final = 16
MAX_SAMPLE_NODES: Final = 16_384
MAX_SAMPLE_TEXT_BYTES: Final = 1024 * 1024

_SAFE_FILENAME_COMPONENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*")
_OBJ_CORE_RECORD_FIELDS: Final = frozenset(
    {
        "source",
        "source_root",
        "prim_path",
        "mesh_resource_uri",
        "mesh_resource_candidates",
        "pose",
        "mesh_scale",
        "point_count",
        "face_count",
    }
)


@dataclass
class CookedConvexSummary:
    """Owner-neutral evidence extracted from an already supplied convex buffer."""

    attempted: bool
    available: bool
    result: str | None = None
    source_path: str | None = None
    attempted_paths: list[str] = field(default_factory=list)
    convex_count: int | None = None
    convexes: list[dict[str, Any]] = field(default_factory=list)
    error: str | None = None


@dataclass
class ColliderRecord:
    """Data-only input to the retained recommendation and export algorithms."""

    path: str
    name: str
    type_name: str
    active: bool
    visibility: str | None
    computed_visibility: str | None
    purpose: str | None
    applied_schemas: list[str]
    parent_path: str | None
    rigid_body_path: str | None
    articulation_root_path: str | None
    xformable: bool
    local_transform: list[list[float]] | None
    world_transform: list[list[float]] | None
    local_aabb: dict[str, Any] | None
    world_aabb: dict[str, Any] | None
    collision_enabled: bool | None
    collision_api: dict[str, Any]
    mesh_collision_api: dict[str, Any] | None
    physx_collision_schemas: dict[str, dict[str, Any]]
    geometry_schema: dict[str, Any]
    cooked_convex: CookedConvexSummary
    errors: list[str] = field(default_factory=list)


def unavailable_payload() -> dict[str, object]:
    """Describe the removed I9 owner and the exact retained source evidence."""

    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "message": UNAVAILABLE_MESSAGE,
        "restoration_gate": RESTORATION_GATE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_wrapper_blob": HISTORICAL_WRAPPER_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
        "future_owner": (
            "I9 retained-product Isaac adapter and its owner-issued collider "
            "inspection/evidence boundary"
        ),
        "removed_contracts": ["manyforge.isaac.collider_report.v0.1"],
        "removed_source_cohorts": [
            {
                "lines": "28-46",
                "responsibility": (
                    "live USD asset-extension discovery constants and the "
                    "report-only error envelope"
                ),
            },
            {
                "lines": "87-950",
                "responsibility": (
                    "report/options DTOs, CLI parsing, Isaac module and stage "
                    "discovery, collider inspection, PhysX lookup, and temporary "
                    "stage recooking"
                ),
            },
            {
                "lines": "1027-1041,1116-1318",
                "responsibility": (
                    "cooking-prim discovery, environment/stage capture, report "
                    "summary, output-profile serialization, and v0.1 publication"
                ),
            },
            {
                "lines": "1319-1372,1513-1517,1630-1989",
                "responsibility": (
                    "report-level grouping/warnings plus live USD object-mesh "
                    "discovery and export"
                ),
            },
            {
                "lines": "2200-2210,2219-2240,2244-2653,2744-2793",
                "responsibility": (
                    "stage-path grouping, live USD/API/schema inspection, report "
                    "sanitization, error capture, and executable dispatch"
                ),
            },
        ],
        "retained_source_cohorts": [
            "48-84: collider evidence DTOs",
            "951-1024,1043-1113: convex normalization and triangulation",
            "1373-1510,1518-1628: pure shape recommendation and transforms",
            "1990-2197,2211-2216: primitive OBJ construction and naming",
            "2241-2242,2654-2742: bounded numeric/transform helpers",
        ],
        "historical_acceptance": (
            "all 13 definitions in the historical test blob remain focused "
            "I9 acceptance inputs and executable tests of retained helpers; "
            "their exact line/name/outcome mapping is "
            "tests/test_isaac_collider_probe_lib.py::"
            "HISTORICAL_ACCEPTANCE_COHORTS"
        ),
        "exit_code": UNAVAILABLE_EXIT_CODE,
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before argument parsing, Isaac imports, stage access, or file writes."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


def _bounded_items(
    values: Iterable[Any] | None,
    *,
    maximum: int,
    label: str,
) -> list[Any]:
    """Materialize at most ``maximum`` items, including unsized iterables."""

    if values is None:
        return []
    if isinstance(values, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be an item collection")
    try:
        supplied_count = len(values)  # type: ignore[arg-type]
    except (AttributeError, TypeError):
        supplied_count = None
    if supplied_count is not None and supplied_count > maximum:
        raise ValueError(f"{label} exceeds the {maximum}-item bound")

    bounded: list[Any] = []
    try:
        iterator = iter(values)
    except TypeError as exc:
        raise ValueError(f"{label} must be an item collection") from exc
    for item in iterator:
        if len(bounded) >= maximum:
            raise ValueError(f"{label} exceeds the {maximum}-item bound")
        bounded.append(item)
    return bounded


def _exact_int(
    value: Any,
    *,
    label: str,
    minimum: int | None = None,
    maximum: int | None = None,
) -> int:
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise ValueError(f"{label} must be an integer, not bool")
    result = int(value)
    if minimum is not None and result < minimum:
        raise ValueError(f"{label} must be at least {minimum}")
    if maximum is not None and result > maximum:
        raise ValueError(f"{label} must be at most {maximum}")
    return result


def _finite_real(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a real number, not bool")
    try:
        result = float(value)
    except (OverflowError, TypeError, ValueError) as exc:
        raise ValueError(f"{label} must be finite") from exc
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _finite_vector3(value: Any, *, label: str) -> list[float]:
    items = _bounded_items(value, maximum=3, label=label)
    if len(items) != 3:
        raise ValueError(f"{label} must contain exactly three coordinates")
    return [
        _finite_real(item, label=f"{label}[{index}]")
        for index, item in enumerate(items)
    ]


def _bounded_text(
    value: Any,
    *,
    label: str,
    maximum_bytes: int = MAX_TEXT_BYTES,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if "\x00" in value:
        raise ValueError(f"{label} must not contain a NUL byte")
    if not allow_empty and not value:
        raise ValueError(f"{label} must not be empty")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{label} exceeds the {maximum_bytes}-byte bound")
    return value


def _safe_filename_component(
    value: Any,
    *,
    label: str,
    allow_empty: bool = False,
) -> str:
    component = _bounded_text(
        value,
        label=label,
        maximum_bytes=MAX_FILENAME_COMPONENT_BYTES,
        allow_empty=allow_empty,
    )
    if not component and allow_empty:
        return component
    if _SAFE_FILENAME_COMPONENT.fullmatch(component) is None:
        raise ValueError(
            f"{label} must contain only ASCII letters, digits, '_' or '-'"
        )
    return component


def _convex_summary(
    convex: Any,
    world_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
    *,
    source_world_matrix: Any | None = None,
    target_inverse_matrix: Any | None = None,
) -> dict[str, Any]:
    sample_limit = _exact_int(
        max_sample_items,
        label="max_sample_items",
        minimum=0,
        maximum=MAX_SAMPLE_ITEMS,
    )
    raw_vertices = _bounded_items(
        getattr(convex, "vertices", None),
        maximum=MAX_VERTICES,
        label="convex vertices",
    )
    vertices = _convex_vertices_in_target_frame(
        raw_vertices,
        source_world_matrix,
        target_inverse_matrix,
    )
    indices = _bounded_items(
        getattr(convex, "indices", None),
        maximum=MAX_INDICES,
        label="convex indices",
    )
    polygons = _bounded_items(
        getattr(convex, "polygons", None),
        maximum=MAX_POLYGONS,
        label="convex polygons",
    )
    faces = _convex_faces(indices, polygons, vertex_count=len(vertices))
    summary: dict[str, Any] = {
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "polygon_count": len(polygons),
        "vertices": [
            _rounded_list(_vector_to_list(item), digits=9) for item in vertices
        ],
        "faces": faces,
        "local_aabb": _points_aabb(vertices),
        "world_aabb": (
            _points_aabb(
                [_transform_point(world_matrix, vertex) for vertex in vertices]
            )
            if world_matrix is not None
            else None
        ),
    }
    if include_samples:
        summary["vertices_sample"] = [
            _vector_to_list(item) for item in vertices[:sample_limit]
        ]
        summary["indices_sample"] = [
            _exact_int(item, label="sampled convex index", minimum=0)
            for item in indices[:sample_limit]
        ]
        summary["polygons_sample"] = _plain_value(polygons[:sample_limit])
    return summary


def _convex_data_summary(
    data: dict[str, Any],
    world_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
    *,
    source_world_matrix: Any | None,
    target_inverse_matrix: Any | None,
) -> dict[str, Any]:
    if not isinstance(data, dict):
        raise ValueError("convex data must be an object")
    sample_limit = _exact_int(
        max_sample_items,
        label="max_sample_items",
        minimum=0,
        maximum=MAX_SAMPLE_ITEMS,
    )
    raw_vertices = _bounded_items(
        data.get("vertices"),
        maximum=MAX_VERTICES,
        label="convex vertices",
    )
    vertices = _convex_vertices_in_target_frame(
        raw_vertices,
        source_world_matrix,
        target_inverse_matrix,
    )
    indices = _bounded_items(
        data.get("indices"),
        maximum=MAX_INDICES,
        label="convex indices",
    )
    polygons = _bounded_items(
        data.get("polygons"),
        maximum=MAX_POLYGONS,
        label="convex polygons",
    )
    faces = _convex_faces(indices, polygons, vertex_count=len(vertices))
    summary: dict[str, Any] = {
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "polygon_count": len(polygons),
        "vertices": [
            _rounded_list(_vector_to_list(item), digits=9) for item in vertices
        ],
        "faces": faces,
        "local_aabb": _points_aabb(vertices),
        "world_aabb": (
            _points_aabb(
                [_transform_point(world_matrix, vertex) for vertex in vertices]
            )
            if world_matrix is not None
            else None
        ),
    }
    if include_samples:
        summary["vertices_sample"] = [
            _vector_to_list(item) for item in vertices[:sample_limit]
        ]
        summary["indices_sample"] = [
            _exact_int(item, label="sampled convex index", minimum=0)
            for item in indices[:sample_limit]
        ]
        summary["polygons_sample"] = _plain_value(polygons[:sample_limit])
    return summary


def _convex_vertices_in_target_frame(
    vertices: list[Any],
    source_world_matrix: Any | None,
    target_inverse_matrix: Any | None,
) -> list[list[float]]:
    bounded_vertices = _bounded_items(
        vertices,
        maximum=MAX_VERTICES,
        label="convex vertices",
    )
    if source_world_matrix is None and target_inverse_matrix is None:
        return [_vector_to_list(vertex) for vertex in bounded_vertices]
    if source_world_matrix is None or target_inverse_matrix is None:
        raise ValueError(
            "source_world_matrix and target_inverse_matrix must be supplied together"
        )
    transformed: list[list[float]] = []
    for vertex in bounded_vertices:
        world_point = _transform_point(source_world_matrix, vertex)
        transformed.append(_transform_point(target_inverse_matrix, world_point))
    return transformed


def _convex_faces(
    indices: list[Any],
    polygons: list[Any],
    *,
    vertex_count: int,
) -> list[list[int]]:
    bounded_indices = _bounded_items(
        indices,
        maximum=MAX_INDICES,
        label="convex indices",
    )
    bounded_polygons = _bounded_items(
        polygons,
        maximum=MAX_POLYGONS,
        label="convex polygons",
    )
    validated_vertex_count = _exact_int(
        vertex_count,
        label="convex vertex_count",
        minimum=0,
        maximum=MAX_VERTICES,
    )
    index_values = [
        _exact_int(
            item,
            label=f"convex indices[{index}]",
            minimum=0,
            maximum=validated_vertex_count - 1,
        )
        for index, item in enumerate(bounded_indices)
    ]
    faces: list[list[int]] = []
    for polygon_number, polygon in enumerate(bounded_polygons):
        base = _first_int_attr(
            polygon,
            "mIndexBase",
            "indexBase",
            "index_base",
            "baseIndex",
            "base_index",
        )
        count = _first_int_attr(
            polygon,
            "mNbVerts",
            "num_vertices",
            "nb_vertices",
            "nbVerts",
            "numVerts",
            "numVertices",
            "vertexCount",
            "vertex_count",
        )
        if base is None and count is None:
            continue
        if base is None or count is None:
            raise ValueError(
                f"convex polygons[{polygon_number}] must supply both index base "
                "and vertex count"
            )
        base = _exact_int(
            base,
            label=f"convex polygons[{polygon_number}].index_base",
            minimum=0,
            maximum=len(index_values),
        )
        count = _exact_int(
            count,
            label=f"convex polygons[{polygon_number}].vertex_count",
            minimum=3,
            maximum=MAX_INDICES,
        )
        if base + count > len(index_values):
            raise ValueError(
                f"convex polygons[{polygon_number}] index range exceeds the "
                "supplied index buffer"
            )
        new_faces = count - 2
        if len(faces) + new_faces > MAX_FACES:
            raise ValueError(f"convex faces exceeds the {MAX_FACES}-item bound")
        polygon_indices = index_values[base : base + count]
        for index in range(1, len(polygon_indices) - 1):
            faces.append(
                [
                    polygon_indices[0],
                    polygon_indices[index],
                    polygon_indices[index + 1],
                ]
            )
    if not faces and len(index_values) >= 3 and len(index_values) % 3 == 0:
        # Some already-extracted PhysX polygon objects expose no Python-visible
        # base/count fields while their supplied index buffer is triangle-flat.
        if len(index_values) // 3 > MAX_FACES:
            raise ValueError(f"convex faces exceeds the {MAX_FACES}-item bound")
        faces = [
            [index_values[index], index_values[index + 1], index_values[index + 2]]
            for index in range(0, len(index_values), 3)
        ]
    return faces


def _first_int_attr(value: Any, *names: str) -> int | None:
    for name in names:
        if isinstance(value, dict) and name in value:
            return _exact_int(value[name], label=name)
        try:
            attr = getattr(value, name)
        except AttributeError:
            continue
        try:
            candidate = attr() if callable(attr) else attr
        except Exception as exc:
            raise ValueError(f"could not read integer attribute {name}") from exc
        return _exact_int(candidate, label=name)
    return None


def _manyforge_shape_recommendation(
    *,
    root: str,
    colliders: list[ColliderRecord],
    object_meshes: list[dict[str, Any]],
    union_aabb: dict[str, Any] | None,
    warnings: list[str],
    mesh_source_preference: str = "auto",
) -> dict[str, Any] | None:
    bounded_colliders = _bounded_items(
        colliders,
        maximum=MAX_COLLIDERS,
        label="colliders",
    )
    bounded_object_meshes = _bounded_items(
        object_meshes,
        maximum=MAX_OBJECT_MESH_RECORDS,
        label="object mesh records",
    )
    bounded_root = _bounded_text(root, label="source root")
    preference = (
        mesh_source_preference
        if mesh_source_preference in _MESH_SOURCE_PREFERENCES
        else "auto"
    )
    if preference in {"auto", "original"}:
        object_mesh = _preferred_original_mesh_record(bounded_object_meshes)
        if object_mesh is not None:
            warnings.append(
                "Mesh recommendation requires the mesh resource path to be "
                "readable by the ManyForge runtime."
            )
            return {
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": object_mesh["mesh_resource_uri"],
                    "mesh_scale": object_mesh.get(
                        "mesh_scale", [1.0, 1.0, 1.0]
                    ),
                },
                "pose": object_mesh["pose"],
                "confidence": (
                    "high"
                    if object_mesh.get("source") == "exported_original_mesh"
                    else "medium"
                ),
                "reason": (
                    "Using the original object mesh exported from Isaac."
                    if object_mesh.get("source") == "exported_original_mesh"
                    else (
                        "Using an existing original object mesh asset discovered "
                        "in the USD composition."
                    )
                ),
                "source_root": bounded_root,
                "mesh_source": object_mesh.get("source"),
                "mesh_source_prim_path": object_mesh.get("prim_path"),
            }
        if preference == "original":
            warnings.append(
                "Original mesh source was requested, but no usable original mesh "
                "was found; falling back to collider/primitive geometry."
            )

    if preference in {"auto", "original", "collider"}:
        collider_mesh = _preferred_collider_mesh_record(
            bounded_object_meshes,
            bounded_colliders,
            union_aabb,
        )
        if collider_mesh is not None:
            if len(_collider_mesh_candidates(bounded_colliders, union_aabb)) > 1:
                warnings.append(
                    "Multiple collider mesh candidates were found; using the "
                    "first candidate in collider traversal order."
                )
            warnings.append(
                "Mesh recommendation requires the mesh resource path to be "
                "readable by the ManyForge runtime."
            )
            return {
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": collider_mesh["mesh_resource_uri"],
                    "mesh_scale": collider_mesh.get(
                        "mesh_scale", [1.0, 1.0, 1.0]
                    ),
                },
                "pose": collider_mesh["pose"],
                "confidence": collider_mesh["confidence"],
                "reason": (
                    "Using a directly usable Isaac collider mesh because "
                    f"mesh_source_preference={preference!r}."
                ),
                "source_root": bounded_root,
                "mesh_source": collider_mesh["source"],
                "mesh_source_prim_path": collider_mesh["prim_path"],
            }
        if preference == "collider":
            warnings.append(
                "Collider mesh source was requested, but no directly usable "
                "collider mesh was found; falling back to collider "
                "primitive/AABB geometry."
            )

    if preference == "primitive":
        warnings.append(
            "Primitive mesh source preference requested; original and collider "
            "mesh assets were ignored."
        )

    return _primitive_shape_recommendation(
        root=bounded_root,
        colliders=bounded_colliders,
        union_aabb=union_aabb,
        warnings=warnings,
    )


def _primitive_shape_recommendation(
    *,
    root: str,
    colliders: list[ColliderRecord],
    union_aabb: dict[str, Any] | None,
    warnings: list[str],
) -> dict[str, Any] | None:
    bounded_root = _bounded_text(root, label="source root")
    bounded_colliders = _bounded_items(
        colliders,
        maximum=MAX_COLLIDERS,
        label="colliders",
    )
    if union_aabb is None:
        return None
    if not isinstance(union_aabb, dict):
        raise ValueError("union_aabb must be an object")
    size = _finite_vector3(union_aabb.get("size"), label="union_aabb.size")
    center = _finite_vector3(
        union_aabb.get("center"),
        label="union_aabb.center",
    )
    if any(component <= 0.0 for component in size):
        return None

    confidence = "medium"
    reason = "Using world AABB as a conservative box."
    if len(bounded_colliders) == 1:
        collider = bounded_colliders[0]
        schema = collider.geometry_schema.get("schema")
        approximation = (collider.mesh_collision_api or {}).get("approximation")
        if schema == "cube" or approximation == "boundingCube":
            confidence = "high"
            reason = "Source collider is an authored cube or boundingCube approximation."
        elif approximation in {"convexHull", "convexDecomposition", "sdf", "none"}:
            confidence = "medium"
    elif len(bounded_colliders) > 8:
        confidence = "low"
        reason = "Using world AABB as a conservative box over many collider fragments."
        warnings.append(
            "Object has many collider fragments; a single world-AABB box may be "
            "overly conservative."
        )
    else:
        reason = "Using world AABB as a conservative box over grouped collider fragments."

    return {
        "shape": {
            "type": "box",
            "box_dimensions_m": _rounded_list(size),
        },
        "pose": {
            "frame_id": "universe",
            "position_m": _rounded_list(center),
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
        "confidence": confidence,
        "reason": reason,
        "source_root": bounded_root,
    }


def _preferred_original_mesh_record(
    records: list[dict[str, Any]],
) -> dict[str, Any] | None:
    bounded_records = _bounded_items(
        records,
        maximum=MAX_OBJECT_MESH_RECORDS,
        label="object mesh records",
    )
    for source in ("exported_original_mesh", "referenced_original_mesh"):
        for index, record in enumerate(bounded_records):
            if not isinstance(record, dict):
                raise ValueError(f"object mesh records[{index}] must be an object")
            if record.get("source") == source and record.get("mesh_resource_uri"):
                return _validated_supplied_mesh_record(
                    record,
                    label=f"object mesh records[{index}]",
                )
    for index, record in enumerate(bounded_records):
        if not isinstance(record, dict):
            raise ValueError(f"object mesh records[{index}] must be an object")
        if record.get("mesh_resource_uri") and record.get("source") not in {
            "referenced_collider_mesh"
        }:
            return _validated_supplied_mesh_record(
                record,
                label=f"object mesh records[{index}]",
            )
    return None


def _validated_supplied_mesh_record(
    record: Any,
    *,
    label: str,
) -> dict[str, Any]:
    if not isinstance(record, dict):
        raise ValueError(f"{label} must be an object")
    source = _bounded_text(record.get("source"), label=f"{label}.source")
    uri = _bounded_text(
        record.get("mesh_resource_uri"),
        label=f"{label}.mesh_resource_uri",
    )
    pose = _validated_pose(record.get("pose"), label=f"{label}.pose")
    scale = _finite_vector3(
        record.get("mesh_scale", [1.0, 1.0, 1.0]),
        label=f"{label}.mesh_scale",
    )
    if any(abs(component) <= 1e-12 for component in scale):
        raise ValueError(f"{label}.mesh_scale must not contain zero")
    candidates = _bounded_items(
        record.get("mesh_resource_candidates", [uri]),
        maximum=MAX_OBJECT_MESH_RECORDS,
        label=f"{label}.mesh_resource_candidates",
    )
    result = {
        "source": source,
        "prim_path": (
            _bounded_text(record["prim_path"], label=f"{label}.prim_path")
            if record.get("prim_path") is not None
            else None
        ),
        "mesh_resource_uri": uri,
        "mesh_resource_candidates": [
            _bounded_text(item, label=f"{label}.mesh_resource_candidates[{index}]")
            for index, item in enumerate(candidates)
        ],
        "pose": pose,
        "mesh_scale": scale,
    }
    for key in ("convex_hull_count",):
        if key in record:
            result[key] = record[key]
    return result


def _validated_pose(value: Any, *, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return {
        "frame_id": _bounded_text(
            value.get("frame_id"),
            label=f"{label}.frame_id",
        ),
        "position_m": _finite_vector3(
            value.get("position_m"),
            label=f"{label}.position_m",
        ),
        "orientation_rpy_deg": _finite_vector3(
            value.get("orientation_rpy_deg"),
            label=f"{label}.orientation_rpy_deg",
        ),
    }


def _preferred_collider_mesh_record(
    object_meshes: list[dict[str, Any]],
    colliders: list[ColliderRecord],
    fallback_aabb: dict[str, Any] | None,
) -> dict[str, Any] | None:
    bounded_object_meshes = _bounded_items(
        object_meshes,
        maximum=MAX_OBJECT_MESH_RECORDS,
        label="object mesh records",
    )
    bounded_colliders = _bounded_items(
        colliders,
        maximum=MAX_COLLIDERS,
        label="colliders",
    )
    for source in (
        "exported_cooked_convex_collider_mesh",
        "exported_primitive_collider_mesh",
        "exported_collider_mesh",
        "referenced_collider_mesh",
    ):
        for index, record in enumerate(bounded_object_meshes):
            if not isinstance(record, dict):
                raise ValueError(f"object mesh records[{index}] must be an object")
            if record.get("source") == source and record.get("mesh_resource_uri"):
                validated_record = _validated_supplied_mesh_record(
                    record,
                    label=f"object mesh records[{index}]",
                )
                return {
                    "source": source,
                    "prim_path": validated_record.get("prim_path"),
                    "mesh_resource_uri": validated_record["mesh_resource_uri"],
                    "mesh_resource_candidates": validated_record[
                        "mesh_resource_candidates"
                    ],
                    "pose": validated_record["pose"],
                    "mesh_scale": validated_record["mesh_scale"],
                    "confidence": (
                        "high" if source != "referenced_collider_mesh" else "medium"
                    ),
                }
    candidates = _collider_mesh_candidates(bounded_colliders, fallback_aabb)
    return candidates[0] if candidates else None


def _collider_mesh_candidates(
    colliders: list[ColliderRecord],
    fallback_aabb: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    bounded_colliders = _bounded_items(
        colliders,
        maximum=MAX_COLLIDERS,
        label="colliders",
    )
    candidates: list[dict[str, Any]] = []
    for collider_number, collider in enumerate(bounded_colliders):
        schema = collider.geometry_schema.get("schema")
        mesh_resource_uri = collider.geometry_schema.get("mesh_resource_uri")
        if (
            schema != "mesh"
            or not isinstance(mesh_resource_uri, str)
            or not mesh_resource_uri
        ):
            continue
        approximation = (collider.mesh_collision_api or {}).get("approximation")
        aabb = (
            collider.world_aabb
            if collider.world_aabb is not None
            else fallback_aabb
        )
        if aabb is None:
            continue
        candidates.append(
            {
                "source": "referenced_collider_mesh",
                "prim_path": _bounded_text(
                    collider.path,
                    label=f"colliders[{collider_number}].path",
                ),
                "mesh_resource_uri": _bounded_text(
                    mesh_resource_uri,
                    label=f"colliders[{collider_number}].mesh_resource_uri",
                ),
                "mesh_resource_candidates": [
                    _bounded_text(
                        item,
                        label=(
                            f"colliders[{collider_number}]."
                            f"mesh_resource_candidates[{index}]"
                        ),
                    )
                    for index, item in enumerate(
                        _bounded_items(
                            collider.geometry_schema.get(
                                "mesh_resource_candidates"
                            )
                            or [mesh_resource_uri],
                            maximum=MAX_OBJECT_MESH_RECORDS,
                            label="mesh resource candidates",
                        )
                    )
                ],
                "pose": _mesh_pose_from_collider(collider, aabb),
                "mesh_scale": _mesh_scale_from_transform(collider.world_transform),
                "confidence": (
                    "high" if approximation in {"none", "convexHull"} else "medium"
                ),
                "mesh_approximation": approximation,
            }
        )
    return candidates


def _mesh_pose_from_collider(
    collider: ColliderRecord,
    fallback_aabb: dict[str, Any],
) -> dict[str, Any]:
    if collider.world_transform is None:
        raise ValueError(
            f"mesh collider {collider.path!r} has no world transform"
        )
    return _mesh_pose_from_transform(collider.world_transform, fallback_aabb)


def _mesh_pose_from_transform(
    matrix: list[list[float]] | None,
    fallback_aabb: dict[str, Any],
) -> dict[str, Any]:
    if matrix is not None:
        rows = _matrix_to_rows(matrix)
        return {
            "frame_id": "universe",
            "position_m": _rounded_list(
                [rows[3][0], rows[3][1], rows[3][2]]
            ),
            "orientation_rpy_deg": _rounded_list(
                _rpy_deg_from_row_major_transform(rows)
            ),
        }
    if not isinstance(fallback_aabb, dict):
        raise ValueError("fallback_aabb must be an object")
    return {
        "frame_id": "universe",
        "position_m": _rounded_list(
            _finite_vector3(
                fallback_aabb.get("center"),
                label="fallback_aabb.center",
            )
        ),
        "orientation_rpy_deg": [0.0, 0.0, 0.0],
    }


def _mesh_scale_from_transform(matrix: Any) -> list[float]:
    if matrix is None:
        return [1.0, 1.0, 1.0]
    rows = _matrix_to_rows(matrix)
    scale = []
    for row_index, row in enumerate(rows[:3]):
        length = math.sqrt(
            sum(row[col] * row[col] for col in range(3))
        )
        if not math.isfinite(length) or length <= 1e-12:
            raise ValueError(
                f"transform row {row_index} has a non-finite or zero scale"
            )
        scale.append(round(length, 9))
    return scale


def _export_primitive_collider_obj(
    *,
    source_root: str,
    root_world: Any,
    colliders: list[ColliderRecord],
    mesh_export_dir: str,
    fallback_aabb: dict[str, Any] | None,
) -> dict[str, Any] | None:
    """Write supplied primitive collider data; never discovers an Isaac stage."""

    bounded_colliders = _bounded_items(
        colliders,
        maximum=MAX_COLLIDERS,
        label="colliders",
    )
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    root_inverse = _inverse_matrix(root_world) if root_world is not None else None
    primitive_count = 0

    for collider in bounded_colliders:
        primitive = _primitive_collider_mesh(collider)
        if primitive is None:
            continue
        if collider.world_transform is None:
            raise ValueError(
                f"collider {collider.path!r} has no world transform for OBJ export"
            )
        primitive_count += 1
        primitive_vertices, primitive_faces = primitive
        if len(vertices) + len(primitive_vertices) > MAX_VERTICES:
            raise ValueError(f"OBJ vertices exceeds the {MAX_VERTICES}-item bound")
        if len(faces) + len(primitive_faces) > MAX_FACES:
            raise ValueError(f"OBJ faces exceeds the {MAX_FACES}-item bound")
        index_base = len(vertices)
        for vertex in primitive_vertices:
            world_point = _transform_point(collider.world_transform, vertex)
            local_point = (
                _transform_point(root_inverse, world_point)
                if root_inverse is not None
                else world_point
            )
            vertices.append(_rounded_list(local_point, digits=9))
        for face in primitive_faces:
            validated_face = _validate_face(
                face,
                vertex_count=len(primitive_vertices),
                one_based=False,
                label=f"collider {collider.path!r} primitive face",
            )
            faces.append([index_base + item + 1 for item in validated_face])

    return _write_obj_mesh_record(
        source="exported_primitive_collider_mesh",
        source_root=source_root,
        prim_path=source_root,
        root_world=root_world,
        vertices=vertices,
        faces=faces,
        mesh_export_dir=mesh_export_dir,
        fallback_aabb=fallback_aabb,
        filename_suffix="collider_primitives",
        extra={"primitive_collider_count": primitive_count},
    )


def _write_obj_mesh_record(
    *,
    source: str,
    source_root: str,
    prim_path: str,
    root_world: Any,
    vertices: list[list[float]],
    faces: list[list[int]],
    mesh_export_dir: str,
    fallback_aabb: dict[str, Any] | None,
    filename_suffix: str,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    bounded_vertices = _bounded_items(
        vertices,
        maximum=MAX_VERTICES,
        label="OBJ vertices",
    )
    bounded_faces = _bounded_items(
        faces,
        maximum=MAX_FACES,
        label="OBJ faces",
    )
    if not bounded_vertices or not bounded_faces:
        return None

    validated_vertices = [
        _finite_vector3(vertex, label=f"OBJ vertices[{index}]")
        for index, vertex in enumerate(bounded_vertices)
    ]
    validated_faces = [
        _validate_face(
            face,
            vertex_count=len(validated_vertices),
            one_based=True,
            label=f"OBJ faces[{index}]",
        )
        for index, face in enumerate(bounded_faces)
    ]
    safe_source = _safe_filename_component(source, label="OBJ source")
    safe_suffix = _safe_filename_component(
        filename_suffix,
        label="OBJ filename suffix",
        allow_empty=True,
    )
    bounded_source_root = _bounded_text(source_root, label="source_root")
    bounded_prim_path = _bounded_text(prim_path, label="prim_path")
    root_rows = _matrix_to_rows(root_world) if root_world is not None else None
    pose = _mesh_pose_from_transform(
        root_rows,
        (
            fallback_aabb
            if fallback_aabb is not None
            else {"center": [0.0, 0.0, 0.0]}
        ),
    )
    mesh_scale = _mesh_scale_from_transform(root_rows)
    validated_extra = _validated_obj_extra(extra)

    export_dir = _resolve_export_directory(mesh_export_dir)
    suffix = f"_{safe_suffix}" if safe_suffix else ""
    mesh_name = f"{_object_id_hint(bounded_source_root)}{suffix}.obj"
    mesh_path = export_dir / mesh_name
    if len(os.fsencode(mesh_path)) > MAX_PATH_BYTES:
        raise ValueError("OBJ output path exceeds its byte bound")
    if mesh_path.parent != export_dir or not mesh_path.is_relative_to(export_dir):
        raise ValueError("OBJ output path escapes the export directory")
    if os.path.lexists(mesh_path):
        raise FileExistsError(f"refusing to overwrite OBJ output: {mesh_path}")

    _write_obj_exclusive(
        mesh_path,
        source=safe_source,
        source_hint=_object_id_hint(bounded_source_root),
        vertices=validated_vertices,
        faces=validated_faces,
    )
    uri = mesh_path.as_uri()
    record = {
        "source": safe_source,
        "source_root": bounded_source_root,
        "prim_path": bounded_prim_path,
        "mesh_resource_uri": uri,
        "mesh_resource_candidates": [uri],
        "pose": pose,
        "mesh_scale": mesh_scale,
        "point_count": len(validated_vertices),
        "face_count": len(validated_faces),
    }
    record.update(validated_extra)
    return record


def _validate_face(
    face: Any,
    *,
    vertex_count: int,
    one_based: bool,
    label: str,
) -> list[int]:
    items = _bounded_items(face, maximum=3, label=label)
    if len(items) != 3:
        raise ValueError(f"{label} must contain exactly three vertex indices")
    minimum = 1 if one_based else 0
    maximum = vertex_count if one_based else vertex_count - 1
    return [
        _exact_int(
            item,
            label=f"{label}[{index}]",
            minimum=minimum,
            maximum=maximum,
        )
        for index, item in enumerate(items)
    ]


def _validated_obj_extra(extra: dict[str, Any] | None) -> dict[str, Any]:
    if extra is None:
        return {}
    if not isinstance(extra, dict):
        raise ValueError("OBJ extra fields must be an object")
    if len(extra) > MAX_EXTRA_FIELDS:
        raise ValueError(
            f"OBJ extra fields exceeds the {MAX_EXTRA_FIELDS}-item bound"
        )
    validated: dict[str, Any] = {}
    for key, value in extra.items():
        bounded_key = _bounded_text(key, label="OBJ extra field name")
        if bounded_key in _OBJ_CORE_RECORD_FIELDS:
            raise ValueError(
                f"OBJ extra field must not override core field {bounded_key!r}"
            )
        validated[bounded_key] = value
    return validated


def _resolve_export_directory(mesh_export_dir: str | Path) -> Path:
    raw_value = os.fspath(mesh_export_dir)
    path_text = _bounded_text(
        raw_value,
        label="mesh_export_dir",
        maximum_bytes=MAX_PATH_BYTES,
    )
    raw_directory = Path(path_text).expanduser()
    if raw_directory.is_symlink():
        raise ValueError(
            f"mesh export directory must not be a symlink: {raw_directory}"
        )
    try:
        raw_directory.mkdir(parents=True, exist_ok=True)
        metadata = raw_directory.lstat()
    except OSError as exc:
        raise ValueError(
            f"cannot prepare mesh export directory {raw_directory}: {exc}"
        ) from exc
    if not stat.S_ISDIR(metadata.st_mode) or raw_directory.is_symlink():
        raise ValueError(
            f"mesh export path is not a non-symlink directory: {raw_directory}"
        )
    try:
        resolved = raw_directory.resolve(strict=True)
        resolved_metadata = resolved.lstat()
    except OSError as exc:
        raise ValueError(
            f"cannot resolve mesh export directory {raw_directory}: {exc}"
        ) from exc
    if not stat.S_ISDIR(resolved_metadata.st_mode) or resolved.is_symlink():
        raise ValueError(
            f"resolved mesh export path is not a directory: {resolved}"
        )
    return resolved


def _write_bounded_obj_line(
    stream: BinaryIO,
    line: str,
    written_bytes: int,
) -> int:
    encoded = (line + "\n").encode("ascii")
    if len(encoded) > MAX_OBJ_OUTPUT_BYTES - written_bytes:
        raise ValueError(
            f"OBJ output exceeds the {MAX_OBJ_OUTPUT_BYTES}-byte bound"
        )
    stream.write(encoded)
    return written_bytes + len(encoded)


def _write_obj_exclusive(
    mesh_path: Path,
    *,
    source: str,
    source_hint: str,
    vertices: list[list[float]],
    faces: list[list[int]],
) -> None:
    """Stream to a private regular file, then atomically publish without replace."""

    if mesh_path.parent.resolve(strict=True) != mesh_path.parent:
        raise ValueError("OBJ destination parent is not normalized")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{mesh_path.name}.",
        suffix=".tmp",
        dir=mesh_path.parent,
    )
    temporary = Path(temporary_name)
    temporary_metadata: os.stat_result | None = None
    published = False
    try:
        try:
            stream = os.fdopen(descriptor, "wb")
        except Exception:
            os.close(descriptor)
            raise
        with stream:
            temporary_metadata = os.fstat(stream.fileno())
            if not stat.S_ISREG(temporary_metadata.st_mode):
                raise ValueError("OBJ staging output is not a regular file")
            written_bytes = _write_bounded_obj_line(
                stream,
                f"# ManyForge {source} export for {source_hint}",
                0,
            )
            for vertex in vertices:
                written_bytes = _write_bounded_obj_line(
                    stream,
                    f"v {vertex[0]:.9g} {vertex[1]:.9g} {vertex[2]:.9g}",
                    written_bytes,
                )
            for face in faces:
                written_bytes = _write_bounded_obj_line(
                    stream,
                    f"f {face[0]} {face[1]} {face[2]}",
                    written_bytes,
                )
            stream.flush()
            os.fsync(stream.fileno())

        if os.path.lexists(mesh_path):
            raise FileExistsError(f"refusing to overwrite OBJ output: {mesh_path}")
        try:
            os.link(temporary, mesh_path, follow_symlinks=False)
        except FileExistsError as exc:
            raise FileExistsError(
                f"refusing to overwrite OBJ output: {mesh_path}"
            ) from exc
        published = True
        published_metadata = mesh_path.lstat()
        if (
            temporary_metadata is None
            or not stat.S_ISREG(published_metadata.st_mode)
            or mesh_path.is_symlink()
            or published_metadata.st_dev != temporary_metadata.st_dev
            or published_metadata.st_ino != temporary_metadata.st_ino
        ):
            raise ValueError("published OBJ output is not the staged regular file")
        directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        directory_descriptor = os.open(mesh_path.parent, directory_flags)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except Exception:
        if published and temporary_metadata is not None:
            try:
                published_metadata = mesh_path.lstat()
                if (
                    stat.S_ISREG(published_metadata.st_mode)
                    and published_metadata.st_dev == temporary_metadata.st_dev
                    and published_metadata.st_ino == temporary_metadata.st_ino
                ):
                    mesh_path.unlink()
            except OSError:
                pass
        raise
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _inverse_matrix(matrix: Any) -> list[list[float]]:
    """Return an exact finite 4x4 inverse or reject a singular transform."""

    rows = _matrix_to_rows(matrix)
    augmented = [
        row[:] + [1.0 if row_index == column else 0.0 for column in range(4)]
        for row_index, row in enumerate(rows)
    ]
    for column in range(4):
        pivot_row = max(
            range(column, 4),
            key=lambda candidate: abs(augmented[candidate][column]),
        )
        pivot = augmented[pivot_row][column]
        if not math.isfinite(pivot) or abs(pivot) <= 1e-15:
            raise ValueError("transform matrix is singular")
        if pivot_row != column:
            augmented[column], augmented[pivot_row] = (
                augmented[pivot_row],
                augmented[column],
            )
        divisor = augmented[column][column]
        augmented[column] = [item / divisor for item in augmented[column]]
        for row_index in range(4):
            if row_index == column:
                continue
            factor = augmented[row_index][column]
            augmented[row_index] = [
                augmented[row_index][item] - factor * augmented[column][item]
                for item in range(8)
            ]
    inverse = [row[4:] for row in augmented]
    return [
        [
            _finite_real(item, label=f"inverse[{row_index}][{column}]")
            for column, item in enumerate(row)
        ]
        for row_index, row in enumerate(inverse)
    ]


def _primitive_collider_mesh(
    collider: ColliderRecord,
) -> tuple[list[list[float]], list[list[int]]] | None:
    schema = collider.geometry_schema.get("schema")
    if schema == "cube":
        size = collider.geometry_schema.get("size")
        edge = _finite_real(size, label="cube size")
        if edge <= 0.0:
            raise ValueError("cube size must be positive")
        return _cube_mesh(edge)
    if schema == "sphere":
        radius = collider.geometry_schema.get("radius")
        validated_radius = _finite_real(radius, label="sphere radius")
        if validated_radius <= 0.0:
            raise ValueError("sphere radius must be positive")
        return _sphere_mesh(validated_radius)
    if schema == "cylinder":
        radius = collider.geometry_schema.get("radius")
        height = collider.geometry_schema.get("height")
        axis = collider.geometry_schema.get("axis", "Z")
        validated_radius = _finite_real(radius, label="cylinder radius")
        validated_height = _finite_real(height, label="cylinder height")
        if validated_radius <= 0.0 or validated_height <= 0.0:
            raise ValueError("cylinder radius and height must be positive")
        if not isinstance(axis, str) or axis.upper() not in {"X", "Y", "Z"}:
            raise ValueError("cylinder axis must be X, Y, or Z")
        return _cylinder_mesh(validated_radius, validated_height, axis)
    return None


def _cube_mesh(edge: float) -> tuple[list[list[float]], list[list[int]]]:
    validated_edge = _finite_real(edge, label="cube edge")
    if validated_edge <= 0.0:
        raise ValueError("cube edge must be positive")
    half = validated_edge / 2.0
    vertices = [
        [-half, -half, -half],
        [half, -half, -half],
        [half, half, -half],
        [-half, half, -half],
        [-half, -half, half],
        [half, -half, half],
        [half, half, half],
        [-half, half, half],
    ]
    faces = [
        [0, 1, 2],
        [0, 2, 3],
        [4, 6, 5],
        [4, 7, 6],
        [0, 4, 5],
        [0, 5, 1],
        [1, 5, 6],
        [1, 6, 2],
        [2, 6, 7],
        [2, 7, 3],
        [3, 7, 4],
        [3, 4, 0],
    ]
    return vertices, faces


def _sphere_mesh(
    radius: float,
    segments: int = 16,
    rings: int = 8,
) -> tuple[list[list[float]], list[list[int]]]:
    validated_radius = _finite_real(radius, label="sphere radius")
    if validated_radius <= 0.0:
        raise ValueError("sphere radius must be positive")
    validated_segments = _exact_int(
        segments,
        label="sphere segments",
        minimum=3,
        maximum=MAX_SEGMENTS,
    )
    validated_rings = _exact_int(
        rings,
        label="sphere rings",
        minimum=2,
        maximum=MAX_RINGS,
    )
    vertex_count = 2 + validated_segments * (validated_rings - 1)
    face_count = 2 * validated_segments * (validated_rings - 1)
    if vertex_count > MAX_VERTICES:
        raise ValueError(f"sphere vertices exceeds the {MAX_VERTICES}-item bound")
    if face_count > MAX_FACES:
        raise ValueError(f"sphere faces exceeds the {MAX_FACES}-item bound")

    vertices: list[list[float]] = [[0.0, 0.0, validated_radius]]
    for ring in range(1, validated_rings):
        phi = math.pi * ring / validated_rings
        z = validated_radius * math.cos(phi)
        ring_radius = validated_radius * math.sin(phi)
        for segment in range(validated_segments):
            theta = 2.0 * math.pi * segment / validated_segments
            vertices.append(
                [
                    ring_radius * math.cos(theta),
                    ring_radius * math.sin(theta),
                    z,
                ]
            )
    vertices.append([0.0, 0.0, -validated_radius])

    faces: list[list[int]] = []
    south = len(vertices) - 1
    for segment in range(validated_segments):
        faces.append(
            [
                0,
                1 + segment,
                1 + ((segment + 1) % validated_segments),
            ]
        )
    for ring in range(validated_rings - 2):
        base = 1 + ring * validated_segments
        next_base = base + validated_segments
        for segment in range(validated_segments):
            a = base + segment
            b = base + ((segment + 1) % validated_segments)
            c = next_base + segment
            d = next_base + ((segment + 1) % validated_segments)
            faces.append([a, c, b])
            faces.append([b, c, d])
    last_ring = 1 + (validated_rings - 2) * validated_segments
    for segment in range(validated_segments):
        faces.append(
            [
                last_ring + segment,
                south,
                last_ring + ((segment + 1) % validated_segments),
            ]
        )
    return vertices, faces


def _cylinder_mesh(
    radius: float,
    height: float,
    axis: str,
    segments: int = 24,
) -> tuple[list[list[float]], list[list[int]]]:
    validated_radius = _finite_real(radius, label="cylinder radius")
    validated_height = _finite_real(height, label="cylinder height")
    if validated_radius <= 0.0 or validated_height <= 0.0:
        raise ValueError("cylinder radius and height must be positive")
    if not isinstance(axis, str) or axis.upper() not in {"X", "Y", "Z"}:
        raise ValueError("cylinder axis must be X, Y, or Z")
    validated_segments = _exact_int(
        segments,
        label="cylinder segments",
        minimum=3,
        maximum=MAX_SEGMENTS,
    )
    vertex_count = 2 * validated_segments + 2
    face_count = 4 * validated_segments
    if vertex_count > MAX_VERTICES:
        raise ValueError(
            f"cylinder vertices exceeds the {MAX_VERTICES}-item bound"
        )
    if face_count > MAX_FACES:
        raise ValueError(f"cylinder faces exceeds the {MAX_FACES}-item bound")

    half = validated_height / 2.0
    vertices: list[list[float]] = []
    for z in (-half, half):
        for segment in range(validated_segments):
            theta = 2.0 * math.pi * segment / validated_segments
            vertices.append(
                _axis_point(
                    validated_radius * math.cos(theta),
                    validated_radius * math.sin(theta),
                    z,
                    axis,
                )
            )
    bottom_center = len(vertices)
    vertices.append(_axis_point(0.0, 0.0, -half, axis))
    top_center = len(vertices)
    vertices.append(_axis_point(0.0, 0.0, half, axis))

    faces: list[list[int]] = []
    for segment in range(validated_segments):
        next_segment = (segment + 1) % validated_segments
        bottom_a = segment
        bottom_b = next_segment
        top_a = validated_segments + segment
        top_b = validated_segments + next_segment
        faces.append([bottom_a, top_a, bottom_b])
        faces.append([bottom_b, top_a, top_b])
        faces.append([bottom_center, bottom_b, bottom_a])
        faces.append([top_center, top_a, top_b])
    return vertices, faces


def _axis_point(x: float, y: float, z: float, axis: str) -> list[float]:
    values = [
        _finite_real(x, label="axis point x"),
        _finite_real(y, label="axis point y"),
        _finite_real(z, label="axis point z"),
    ]
    if not isinstance(axis, str) or axis.upper() not in {"X", "Y", "Z"}:
        raise ValueError("axis must be X, Y, or Z")
    axis = axis.upper()
    if axis == "X":
        return [values[2], values[0], values[1]]
    if axis == "Y":
        return [values[0], values[2], values[1]]
    return values


def _object_id_hint(source_root: str) -> str:
    bounded_root = _bounded_text(source_root, label="source_root")
    slug = bounded_root.strip("/").replace("/", "__")
    cleaned: list[str] = []
    for char in slug:
        cleaned.append(
            char if char.isascii() and (char.isalnum() or char == "_") else "_"
        )
    stem = "".join(cleaned).strip("_") or "scene_object"
    stem = stem[:72].rstrip("_") or "scene_object"
    digest = hashlib.sha256(bounded_root.encode("utf-8")).hexdigest()[:12]
    return f"{stem}_{digest}"


def _rounded_list(values: list[Any], digits: int = 6) -> list[float]:
    validated_digits = _exact_int(
        digits,
        label="rounding digits",
        minimum=0,
        maximum=15,
    )
    bounded_values = _bounded_items(
        values,
        maximum=MAX_VERTICES,
        label="numeric values",
    )
    return [
        round(_finite_real(value, label=f"numeric values[{index}]"), validated_digits)
        for index, value in enumerate(bounded_values)
    ]


def _matrix_to_rows(matrix: Any) -> list[list[float]]:
    if matrix is None:
        raise ValueError("transform matrix must not be None")
    if isinstance(matrix, (list, tuple)):
        if len(matrix) != 4:
            raise ValueError("transform matrix must contain exactly four rows")
        for row_index, row in enumerate(matrix):
            if not isinstance(row, (list, tuple)) or len(row) != 4:
                raise ValueError(
                    f"transform matrix row {row_index} must contain exactly four values"
                )
    rows: list[list[float]] = []
    for row_index in range(4):
        row_values: list[float] = []
        for column in range(4):
            try:
                value = matrix[row_index][column]
            except Exception as exc:
                raise ValueError("transform matrix must be indexable as 4x4") from exc
            row_values.append(
                _finite_real(
                    value,
                    label=f"transform[{row_index}][{column}]",
                )
            )
        rows.append(row_values)
    return rows


def _rpy_deg_from_row_major_transform(
    matrix: list[list[float]],
) -> list[float]:
    rows = _matrix_to_rows(matrix)
    # Gf.Matrix4d uses row-vector transforms. Transpose the 3x3 block into the
    # conventional column-vector rotation matrix before extracting XYZ RPY.
    rotation = [[rows[col][row] for col in range(3)] for row in range(3)]
    for col in range(3):
        norm = math.sqrt(
            sum(rotation[row][col] * rotation[row][col] for row in range(3))
        )
        if not math.isfinite(norm) or norm <= 1e-12:
            raise ValueError(f"transform rotation column {col} is degenerate")
        for row in range(3):
            rotation[row][col] /= norm

    sy = math.sqrt(
        rotation[0][0] * rotation[0][0]
        + rotation[1][0] * rotation[1][0]
    )
    singular = sy < 1e-9
    if not singular:
        roll = math.atan2(rotation[2][1], rotation[2][2])
        pitch = math.atan2(-rotation[2][0], sy)
        yaw = math.atan2(rotation[1][0], rotation[0][0])
    else:
        roll = math.atan2(-rotation[1][2], rotation[1][1])
        pitch = math.atan2(-rotation[2][0], sy)
        yaw = 0.0
    return _finite_vector3(
        [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)],
        label="orientation_rpy_deg",
    )


def _vector_to_list(value: Any) -> list[float]:
    return _finite_vector3(value, label="vector")


def _points_aabb(points: list[Any]) -> dict[str, Any] | None:
    bounded_points = _bounded_items(
        points,
        maximum=MAX_VERTICES,
        label="AABB points",
    )
    if not bounded_points:
        return None
    vectors = [_vector_to_list(point) for point in bounded_points]
    minimum = [min(point[index] for point in vectors) for index in range(3)]
    maximum = [max(point[index] for point in vectors) for index in range(3)]
    return {
        "min": minimum,
        "max": maximum,
        "size": [maximum[index] - minimum[index] for index in range(3)],
        "center": [
            (maximum[index] + minimum[index]) / 2.0 for index in range(3)
        ],
    }


def _transform_point(matrix: Any, point: Any) -> list[float]:
    if matrix is None:
        raise ValueError("transform matrix must not be None")
    vector = _vector_to_list(point)
    transform = getattr(matrix, "Transform", None)
    if transform is not None:
        if not callable(transform):
            raise ValueError("matrix Transform attribute must be callable")
        try:
            transformed = transform(point)
        except Exception as exc:
            raise ValueError("matrix Transform operation failed") from exc
        return _finite_vector3(transformed, label="transformed point")

    rows = _matrix_to_rows(matrix)
    transformed = [
        vector[0] * rows[0][column]
        + vector[1] * rows[1][column]
        + vector[2] * rows[2][column]
        + rows[3][column]
        for column in range(3)
    ]
    return _finite_vector3(transformed, label="transformed point")


def _plain_value(value: Any) -> Any:
    remaining_nodes = MAX_SAMPLE_NODES
    remaining_text_bytes = MAX_SAMPLE_TEXT_BYTES

    def bounded_sample_text(raw: Any, *, label: str) -> str:
        nonlocal remaining_text_bytes
        text = _bounded_text(
            raw,
            label=label,
            maximum_bytes=MAX_TEXT_BYTES,
            allow_empty=True,
        )
        size = len(text.encode("utf-8"))
        if size > remaining_text_bytes:
            raise ValueError(
                f"polygon sample text exceeds the {MAX_SAMPLE_TEXT_BYTES}-byte bound"
            )
        remaining_text_bytes -= size
        return text

    def convert(item: Any, *, depth: int, ancestors: set[int]) -> Any:
        nonlocal remaining_nodes
        if depth > MAX_SAMPLE_DEPTH:
            raise ValueError(
                f"polygon sample exceeds the {MAX_SAMPLE_DEPTH}-level depth bound"
            )
        if remaining_nodes <= 0:
            raise ValueError(
                f"polygon sample exceeds the {MAX_SAMPLE_NODES}-node bound"
            )
        remaining_nodes -= 1

        if item is None or isinstance(item, bool):
            return item
        if isinstance(item, str):
            return bounded_sample_text(item, label="polygon sample text")
        if isinstance(item, Integral):
            return _exact_int(
                item,
                label="polygon sample integer",
                minimum=-(2**63),
                maximum=2**63 - 1,
            )
        if isinstance(item, Real):
            return _finite_real(item, label="polygon sample number")
        if type(item) in (list, tuple, dict):
            object_id = id(item)
            if object_id in ancestors:
                raise ValueError("polygon sample contains a cycle")
            ancestors.add(object_id)
            try:
                if type(item) in (list, tuple):
                    if len(item) > MAX_SAMPLE_ITEMS:
                        raise ValueError(
                            "polygon sample collection exceeds the "
                            f"{MAX_SAMPLE_ITEMS}-item bound"
                        )
                    return [
                        convert(child, depth=depth + 1, ancestors=ancestors)
                        for child in item
                    ]
                if len(item) > MAX_SAMPLE_ITEMS:
                    raise ValueError(
                        "polygon sample object exceeds the "
                        f"{MAX_SAMPLE_ITEMS}-field bound"
                    )
                converted: dict[str, Any] = {}
                for key, child in item.items():
                    name = bounded_sample_text(
                        key,
                        label="polygon sample field name",
                    )
                    converted[name] = convert(
                        child,
                        depth=depth + 1,
                        ancestors=ancestors,
                    )
                return converted
            finally:
                ancestors.remove(object_id)

        try:
            path_string = getattr(item, "pathString")
        except AttributeError:
            path_string = None
        except Exception as exc:
            raise ValueError("polygon sample pathString access failed") from exc
        if path_string is not None:
            return bounded_sample_text(
                path_string,
                label="polygon sample pathString",
            )
        try:
            get_string = getattr(item, "GetString")
        except AttributeError:
            get_string = None
        except Exception as exc:
            raise ValueError("polygon sample GetString access failed") from exc
        if get_string is not None:
            if not callable(get_string):
                raise ValueError("polygon sample GetString must be callable")
            try:
                rendered = get_string()
            except Exception as exc:
                raise ValueError("polygon sample GetString failed") from exc
            return bounded_sample_text(
                rendered,
                label="polygon sample GetString result",
            )
        try:
            return _vector_to_list(item)
        except (TypeError, ValueError) as exc:
            raise ValueError(
                "polygon sample value has no supported bounded representation"
            ) from exc

    return convert(value, depth=0, ancestors=set())


if __name__ == "__main__":
    raise SystemExit(main())
