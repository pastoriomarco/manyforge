"""Probe Isaac Sim/USD collider data for ManyForge scene export planning.

Run this script inside Isaac Sim's Python/Kit environment with a scene already
loaded, for example:

    ./python.sh /path/to/manyforge/scripts/isaac-collider-probe.py \
        --output /tmp/isaac_colliders.json \
        --pretty

The script is intentionally read-only.  It inventories collider prims, authored
USD physics metadata, transforms, bounding boxes, and optionally attempts to
request PhysX cooked convex collision representations.
"""

from __future__ import annotations

import argparse
import json
import math
import platform
import sys
import traceback
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


_SUPPORTED_MANYFORGE_MESH_EXTENSIONS = {
    ".obj",
    ".stl",
    ".dae",
    ".ply",
    ".off",
    ".glb",
    ".gltf",
}

_MESH_SOURCE_PREFERENCES = {"auto", "original", "collider", "primitive"}


@dataclass
class ProbeError:
    scope: str
    message: str
    traceback: str | None = None


@dataclass
class CookedConvexSummary:
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


@dataclass
class ProbeReport:
    probe_version: str
    environment: dict[str, Any]
    stage: dict[str, Any]
    filters: dict[str, Any]
    summary: dict[str, Any]
    probed_prims: list["PrimRecord"]
    colliders: list[ColliderRecord]
    object_meshes: dict[str, list[dict[str, Any]]]
    errors: list[ProbeError]


@dataclass
class PrimRecord:
    path: str
    name: str
    type_name: str
    active: bool
    visibility: str | None
    computed_visibility: str | None
    purpose: str | None
    applied_schemas: list[str]
    parent_path: str | None
    xformable: bool
    local_transform: list[list[float]] | None
    world_transform: list[list[float]] | None
    local_aabb: dict[str, Any] | None
    world_aabb: dict[str, Any] | None
    errors: list[str] = field(default_factory=list)


@dataclass
class ProbeOptions:
    stage: str | None = None
    include_subtree: list[str] = field(default_factory=list)
    exclude_subtree: list[str] = field(default_factory=list)
    max_colliders: int | None = None
    cooked_convex: bool = True
    include_cooked_samples: bool = False
    max_sample_items: int = 8
    include_tracebacks: bool = False
    include_hidden: bool = False
    purpose: list[str] = field(default_factory=list)
    probe_prim: list[str] = field(default_factory=list)
    mesh_export_dir: str | None = None
    mesh_source_preference: str = "auto"


def capture_probe_report(options: ProbeOptions) -> ProbeReport:
    """Capture a collider report from the current Isaac Sim stage or a USD file."""
    errors: list[ProbeError] = []

    try:
        modules = _load_isaac_modules()
    except Exception as exc:
        raise RuntimeError(
            "Failed to import Isaac/Omniverse Python modules. Run this inside "
            "Isaac Sim's Python/Kit environment."
        ) from exc

    try:
        stage = _get_stage(modules, options.stage)
    except Exception as exc:
        errors.append(_probe_error("stage", exc, options.include_tracebacks))
        return _empty_report(options, modules, errors)

    colliders: list[ColliderRecord] = []
    stage_id = _stage_id(modules, stage)
    cooking = _acquire_cooking_interface(modules) if options.cooked_convex else None
    bbox_cache = _make_bbox_cache(modules)

    probed_prims = _inspect_requested_prims(
        modules=modules,
        stage=stage,
        bbox_cache=bbox_cache,
        prim_paths=options.probe_prim,
        include_tracebacks=options.include_tracebacks,
        errors=errors,
    )

    for prim in stage.Traverse():
        try:
            path = str(prim.GetPath())
            if not _path_included(path, options.include_subtree, options.exclude_subtree):
                continue
            if not options.include_hidden and not _prim_is_visible(modules, prim):
                continue
            if not _purpose_allowed(modules, prim, options.purpose):
                continue
            if not _has_collision_api(modules, prim):
                continue
            record = _inspect_collider(
                modules=modules,
                stage=stage,
                stage_id=stage_id,
                prim=prim,
                bbox_cache=bbox_cache,
                cooking=cooking,
                cooked_enabled=options.cooked_convex,
                include_cooked_samples=options.include_cooked_samples,
                max_sample_items=options.max_sample_items,
            )
            colliders.append(record)
            if options.max_colliders is not None and len(colliders) >= options.max_colliders:
                break
        except Exception as exc:
            errors.append(_probe_error(str(prim.GetPath()), exc, options.include_tracebacks))

    object_meshes = _inspect_object_meshes(
        modules=modules,
        stage=stage,
        colliders=colliders,
        mesh_export_dir=options.mesh_export_dir,
        errors=errors,
        include_tracebacks=options.include_tracebacks,
    )

    return ProbeReport(
        probe_version="0.1",
        environment=_environment_info(modules),
        stage=_stage_info(modules, stage, stage_id),
        filters={
            "include_subtree": options.include_subtree,
            "exclude_subtree": options.exclude_subtree,
            "max_colliders": options.max_colliders,
            "cooked_convex": options.cooked_convex,
            "include_cooked_samples": options.include_cooked_samples,
            "include_hidden": options.include_hidden,
            "purpose": options.purpose,
            "probe_prim": options.probe_prim,
            "mesh_export_dir": options.mesh_export_dir,
            "mesh_source_preference": options.mesh_source_preference,
        },
        summary=_summary(colliders, errors),
        probed_prims=probed_prims,
        colliders=colliders,
        object_meshes=object_meshes,
        errors=errors,
    )


def main() -> int:
    args = _parse_args()
    options = _options_from_args(args)
    try:
        report = capture_probe_report(options)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 2
    _write_payload(_profile_payload(report, args.output_profile), args)
    return 0 if not report.errors else 1


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stage",
        default=None,
        help=(
            "Optional USD stage file to open. If omitted, use the currently "
            "loaded Isaac Sim stage."
        ),
    )
    parser.add_argument(
        "--output",
        default=None,
        help="JSON output path. If omitted, write JSON to stdout.",
    )
    parser.add_argument(
        "--include-subtree",
        action="append",
        default=[],
        help="Only include collider prims under this path. May be repeated.",
    )
    parser.add_argument(
        "--exclude-subtree",
        action="append",
        default=[],
        help="Exclude collider prims under this path. May be repeated.",
    )
    parser.add_argument(
        "--max-colliders",
        type=int,
        default=None,
        help="Optional maximum number of collider prims to inspect.",
    )
    parser.add_argument(
        "--no-cooked-convex",
        action="store_false",
        dest="cooked_convex",
        help="Disable PhysX cooked convex collision representation requests.",
    )
    parser.set_defaults(cooked_convex=True)
    parser.add_argument(
        "--include-cooked-samples",
        action="store_true",
        help="Include small vertex/index/polygon samples from cooked convex data.",
    )
    parser.add_argument(
        "--max-sample-items",
        type=int,
        default=8,
        help="Maximum sampled items per cooked convex array when samples are enabled.",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Pretty-print JSON output.",
    )
    parser.add_argument(
        "--include-hidden",
        action="store_true",
        help="Include collider prims whose computed USD visibility is invisible.",
    )
    parser.add_argument(
        "--purpose",
        action="append",
        default=[],
        choices=["default", "render", "proxy", "guide", "all"],
        help=(
            "Only include collider prims with this computed USD purpose. "
            "May be repeated. Defaults to all purposes."
        ),
    )
    parser.add_argument(
        "--probe-prim",
        action="append",
        default=[],
        help=(
            "Also report transform/AABB metadata for this prim path even if it "
            "is not a collider. May be repeated."
        ),
    )
    parser.add_argument(
        "--mesh-export-dir",
        default=None,
        help=(
            "Optional directory where each logical object's original USD mesh "
            "subtree is exported as an OBJ for ManyForge mesh scene resources."
        ),
    )
    parser.add_argument(
        "--mesh-source-preference",
        choices=sorted(_MESH_SOURCE_PREFERENCES),
        default="auto",
        help=(
            "Select which mesh source should drive LLM scene-resource "
            "recommendations. auto prefers original visual/object meshes when "
            "available; collider ignores original meshes and prefers directly "
            "usable collider meshes; primitive forces primitive/AABB fallback."
        ),
    )
    parser.add_argument(
        "--output-profile",
        choices=["raw_inventory", "llm_scene_authoring"],
        default="raw_inventory",
        help="Select raw probe JSON or compact LLM scene-authoring JSON.",
    )
    parser.add_argument(
        "--include-tracebacks",
        action="store_true",
        help="Include Python tracebacks in error records.",
    )
    return parser.parse_args()


def _options_from_args(args: argparse.Namespace) -> ProbeOptions:
    return ProbeOptions(
        stage=args.stage,
        include_subtree=args.include_subtree,
        exclude_subtree=args.exclude_subtree,
        max_colliders=args.max_colliders,
        cooked_convex=args.cooked_convex,
        include_cooked_samples=args.include_cooked_samples,
        max_sample_items=args.max_sample_items,
        include_tracebacks=args.include_tracebacks,
        include_hidden=args.include_hidden,
        purpose=args.purpose,
        probe_prim=args.probe_prim,
        mesh_export_dir=args.mesh_export_dir,
        mesh_source_preference=args.mesh_source_preference,
    )


def _load_isaac_modules() -> dict[str, Any]:
    from pxr import Gf, PhysicsSchemaTools, PhysxSchema, Sdf, Usd, UsdGeom, UsdPhysics, UsdUtils

    modules: dict[str, Any] = {
        "Gf": Gf,
        "PhysicsSchemaTools": PhysicsSchemaTools,
        "PhysxSchema": PhysxSchema,
        "Sdf": Sdf,
        "Usd": Usd,
        "UsdGeom": UsdGeom,
        "UsdPhysics": UsdPhysics,
        "UsdUtils": UsdUtils,
    }

    try:
        import omni.usd

        modules["omni.usd"] = omni.usd
    except Exception:
        modules["omni.usd"] = None

    try:
        import omni.kit.app

        modules["omni.kit.app"] = omni.kit.app
    except Exception:
        modules["omni.kit.app"] = None

    try:
        import omni.physx.bindings._physx as physx_bindings

        modules["omni.physx.bindings._physx"] = physx_bindings
    except Exception:
        modules["omni.physx.bindings._physx"] = None

    return modules


def _get_stage(modules: dict[str, Any], stage_path: str | None) -> Any:
    Usd = modules["Usd"]
    if stage_path:
        stage = Usd.Stage.Open(stage_path)
        if stage is None:
            raise RuntimeError(f"Could not open USD stage: {stage_path}")
        return stage

    omni_usd = modules.get("omni.usd")
    if omni_usd is None:
        raise RuntimeError("omni.usd is not available and --stage was not provided")
    stage = omni_usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No Isaac Sim stage is currently loaded")
    return stage


def _stage_id(modules: dict[str, Any], stage: Any) -> int | None:
    try:
        return int(modules["UsdUtils"].StageCache.Get().GetId(stage).ToLongInt())
    except Exception:
        return None


def _make_bbox_cache(modules: dict[str, Any]) -> Any:
    Usd = modules["Usd"]
    UsdGeom = modules["UsdGeom"]
    included_purposes = [
        UsdGeom.Tokens.default_,
        UsdGeom.Tokens.render,
        UsdGeom.Tokens.proxy,
        UsdGeom.Tokens.guide,
    ]
    return UsdGeom.BBoxCache(Usd.TimeCode.Default(), included_purposes, useExtentsHint=True)


def _inspect_requested_prims(
    *,
    modules: dict[str, Any],
    stage: Any,
    bbox_cache: Any,
    prim_paths: list[str],
    include_tracebacks: bool,
    errors: list[ProbeError],
) -> list[PrimRecord]:
    records: list[PrimRecord] = []
    for prim_path in prim_paths:
        try:
            prim = stage.GetPrimAtPath(prim_path)
            if prim is None or not prim.IsValid():
                raise RuntimeError(f"Prim not found: {prim_path}")
            records.append(_inspect_prim(modules, prim, bbox_cache))
        except Exception as exc:
            errors.append(_probe_error(prim_path, exc, include_tracebacks))
    return records


def _inspect_prim(modules: dict[str, Any], prim: Any, bbox_cache: Any) -> PrimRecord:
    errors: list[str] = []
    world_matrix = _world_matrix(modules, prim, errors)
    return PrimRecord(
        path=str(prim.GetPath()),
        name=prim.GetName(),
        type_name=prim.GetTypeName(),
        active=bool(prim.IsActive()),
        visibility=_visibility_attr(modules, prim),
        computed_visibility=_computed_visibility(modules, prim),
        purpose=_computed_purpose(modules, prim),
        applied_schemas=list(prim.GetAppliedSchemas()),
        parent_path=_parent_path(prim),
        xformable=_is_xformable(modules, prim),
        local_transform=_local_transform(modules, prim, errors),
        world_transform=_matrix_to_rows(world_matrix) if world_matrix is not None else None,
        local_aabb=_aabb(modules, bbox_cache, prim, local=True, errors=errors),
        world_aabb=_aabb(modules, bbox_cache, prim, local=False, errors=errors),
        errors=errors,
    )


def _inspect_collider(
    *,
    modules: dict[str, Any],
    stage: Any,
    stage_id: int | None,
    prim: Any,
    bbox_cache: Any,
    cooking: Any,
    cooked_enabled: bool,
    include_cooked_samples: bool,
    max_sample_items: int,
) -> ColliderRecord:
    UsdPhysics = modules["UsdPhysics"]
    PhysxSchema = modules["PhysxSchema"]
    path = str(prim.GetPath())
    errors: list[str] = []

    collision_api = _make_api(UsdPhysics.CollisionAPI, prim)
    collision_enabled = _attr_value(_api_attr(collision_api, "GetCollisionEnabledAttr"))

    mesh_api = None
    if _has_api(prim, UsdPhysics.MeshCollisionAPI):
        mesh_api = _inspect_api_attrs(
            _make_api(UsdPhysics.MeshCollisionAPI, prim),
            [
                "GetApproximationAttr",
            ],
        )

    world_matrix = _world_matrix(modules, prim, errors)
    cooked = _inspect_cooked_convex(
        modules=modules,
        stage=stage,
        stage_id=stage_id,
        prim=prim,
        world_matrix=world_matrix,
        cooking=cooking,
        enabled=cooked_enabled,
        include_samples=include_cooked_samples,
        max_sample_items=max_sample_items,
    )

    return ColliderRecord(
        path=path,
        name=prim.GetName(),
        type_name=prim.GetTypeName(),
        active=bool(prim.IsActive()),
        visibility=_visibility_attr(modules, prim),
        computed_visibility=_computed_visibility(modules, prim),
        purpose=_computed_purpose(modules, prim),
        applied_schemas=list(prim.GetAppliedSchemas()),
        parent_path=_parent_path(prim),
        rigid_body_path=_ancestor_with_api(prim, UsdPhysics.RigidBodyAPI),
        articulation_root_path=_ancestor_with_api(prim, UsdPhysics.ArticulationRootAPI),
        xformable=_is_xformable(modules, prim),
        local_transform=_local_transform(modules, prim, errors),
        world_transform=_matrix_to_rows(world_matrix) if world_matrix is not None else None,
        local_aabb=_aabb(modules, bbox_cache, prim, local=True, errors=errors),
        world_aabb=_aabb(modules, bbox_cache, prim, local=False, errors=errors),
        collision_enabled=bool(collision_enabled) if collision_enabled is not None else None,
        collision_api=_inspect_api_attrs(
            collision_api,
            [
                "GetCollisionEnabledAttr",
                "GetFilteredGroupsRel",
                "GetSimulationOwnerRel",
            ],
        ),
        mesh_collision_api=mesh_api,
        physx_collision_schemas=_physx_collision_schemas(PhysxSchema, prim),
        geometry_schema=_geometry_schema(modules, prim),
        cooked_convex=cooked,
        errors=errors,
    )


def _inspect_cooked_convex(
    *,
    modules: dict[str, Any],
    stage: Any,
    stage_id: int | None,
    prim: Any,
    world_matrix: Any,
    cooking: Any,
    enabled: bool,
    include_samples: bool,
    max_sample_items: int,
) -> CookedConvexSummary:
    if not enabled:
        return CookedConvexSummary(attempted=False, available=False, error="disabled")
    if cooking is None:
        return CookedConvexSummary(
            attempted=True,
            available=False,
            error="omni.physx cooking interface is not available",
        )
    if stage_id is None:
        return CookedConvexSummary(
            attempted=True,
            available=False,
            error="stage id is not available",
        )

    target_inverse = _inverse_matrix(world_matrix)
    attempted_paths: list[str] = []
    last_result: str | None = None
    last_error: str | None = None
    for cooking_prim in _cooking_prim_candidates(modules, prim):
        source_path = str(cooking_prim.GetPath())
        attempted_paths.append(source_path)
        try:
            prim_id = modules["PhysicsSchemaTools"].sdfPathToInt(source_path)
        except Exception as exc:
            last_error = f"could not encode prim path for PhysX: {exc}"
            continue

        source_world = _world_matrix(modules, cooking_prim, [])
        callback_payload: dict[str, Any] = {}

        def on_result(result: Any, convexes: list[Any]) -> None:
            callback_payload["result"] = _enum_name(result)
            callback_payload["convexes"] = [
                _convex_summary(
                    convex,
                    world_matrix,
                    include_samples,
                    max_sample_items,
                    source_world_matrix=source_world,
                    target_inverse_matrix=target_inverse,
                )
                for convex in list(convexes)
            ]

        try:
            cooking.request_convex_collision_representation(
                int(stage_id),
                int(prim_id),
                False,
                on_result,
            )
        except Exception as exc:
            last_error = f"request_convex_collision_representation failed: {exc}"
            continue

        if "result" not in callback_payload:
            last_error = "PhysX cooking callback was not invoked"
            continue

        convexes = callback_payload.get("convexes", [])
        result = callback_payload["result"]
        last_result = result
        if not convexes:
            continue
        return CookedConvexSummary(
            attempted=True,
            available=True,
            result=result,
            source_path=source_path,
            attempted_paths=attempted_paths,
            convex_count=len(convexes),
            convexes=convexes,
        )

    for cooking_prim in _cooking_prim_candidates(modules, prim):
        source_path = str(cooking_prim.GetPath())
        if source_path not in attempted_paths:
            attempted_paths.append(source_path)
        source_world = _world_matrix(modules, cooking_prim, [])
        try:
            convexes = _direct_convex_mesh_data(
                cooking,
                source_path,
                world_matrix,
                include_samples,
                max_sample_items,
                source_world_matrix=source_world,
                target_inverse_matrix=target_inverse,
            )
        except Exception as exc:
            last_error = f"get_convex_mesh_data failed: {exc}"
            continue
        if not convexes:
            continue
        return CookedConvexSummary(
            attempted=True,
            available=True,
            result="DIRECT_CONVEX_MESH_DATA",
            source_path=source_path,
            attempted_paths=attempted_paths,
            convex_count=len(convexes),
            convexes=convexes,
        )

    try:
        recook_path, convexes = _temporary_recook_descendant_mesh(
            modules=modules,
            stage=stage,
            stage_id=stage_id,
            prim=prim,
            cooking=cooking,
            target_world_matrix=world_matrix,
            target_inverse_matrix=target_inverse,
            include_samples=include_samples,
            max_sample_items=max_sample_items,
        )
    except Exception as exc:
        last_error = f"temporary descendant-mesh recook failed: {exc}"
    else:
        if recook_path:
            attempted_paths.append(recook_path)
        if convexes:
            return CookedConvexSummary(
                attempted=True,
                available=True,
                result="TEMPORARY_DESCENDANT_MESH_RECOOK",
                source_path=recook_path,
                attempted_paths=attempted_paths,
                convex_count=len(convexes),
                convexes=convexes,
            )

    return CookedConvexSummary(
        attempted=True,
        available=False,
        result=last_result,
        attempted_paths=attempted_paths,
        convex_count=0,
        convexes=[],
        error=last_error,
    )


def _direct_convex_mesh_data(
    cooking: Any,
    source_path: str,
    world_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
    *,
    source_world_matrix: Any | None,
    target_inverse_matrix: Any | None,
) -> list[dict[str, Any]]:
    get_count = getattr(cooking, "get_nb_convex_mesh_data", None)
    get_data = getattr(cooking, "get_convex_mesh_data", None)
    if get_count is None or get_data is None:
        return []
    count = int(get_count(source_path))
    convexes = []
    for index in range(count):
        data = get_data(source_path, index)
        if not isinstance(data, dict):
            continue
        convexes.append(
            _convex_data_summary(
                data,
                world_matrix,
                include_samples,
                max_sample_items,
                source_world_matrix=source_world_matrix,
                target_inverse_matrix=target_inverse_matrix,
            )
        )
    return convexes


def _temporary_recook_descendant_mesh(
    *,
    modules: dict[str, Any],
    stage: Any,
    stage_id: int,
    prim: Any,
    cooking: Any,
    target_world_matrix: Any,
    target_inverse_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
) -> tuple[str | None, list[dict[str, Any]]]:
    """Cook a transient mesh copy when CollisionAPI is authored on an Xform.

    PhysX's collision-representation API expects the prim carrying CollisionAPI
    to also be parseable as collision geometry. Some Isaac assets author
    MeshCollisionAPI on an Xform with the actual UsdGeom.Mesh below it; PhysX
    then returns RESULT_ERROR_INVALID_PARSING even though the runtime UI shows a
    convex-decomposition setting. This fallback creates a temporary cook-only
    mesh from the descendant visual mesh, copies the collision approximation
    settings, requests the cooked representation, and removes the temp prim.
    """
    try:
        if prim.IsA(modules["UsdGeom"].Mesh):
            return None, []
    except Exception:
        return None, []

    mesh_prims = [
        descendant
        for descendant in _iter_prim_subtree(prim)
        if _prim_is_active_mesh(modules, descendant)
    ]
    if not mesh_prims:
        return None, []

    temp_root = f"/__manyforge_probe_cooking_{_object_id_hint(str(prim.GetPath()))}"
    temp_path = f"{temp_root}/collision_mesh"
    Sdf = modules["Sdf"]
    UsdGeom = modules["UsdGeom"]
    UsdPhysics = modules["UsdPhysics"]
    PhysxSchema = modules["PhysxSchema"]

    stage.RemovePrim(Sdf.Path(temp_root))
    try:
        UsdGeom.Xform.Define(stage, Sdf.Path(temp_root))
        mesh = UsdGeom.Mesh.Define(stage, Sdf.Path(temp_path))
        temp_prim = mesh.GetPrim()
        points, face_counts, face_indices = _merged_mesh_geometry_in_target_frame(
            modules=modules,
            mesh_prims=mesh_prims,
            target_inverse_matrix=target_inverse_matrix,
        )
        if not points or not face_counts or not face_indices:
            return temp_path, []

        Gf = modules["Gf"]
        mesh.CreatePointsAttr([Gf.Vec3f(*point) for point in points])
        mesh.CreateFaceVertexCountsAttr(face_counts)
        mesh.CreateFaceVertexIndicesAttr(face_indices)
        UsdPhysics.CollisionAPI.Apply(temp_prim)
        mesh_collision = UsdPhysics.MeshCollisionAPI.Apply(temp_prim)
        approximation = None
        if _has_api(prim, UsdPhysics.MeshCollisionAPI):
            source_mesh_collision = _make_api(UsdPhysics.MeshCollisionAPI, prim)
            approximation = _attr_value(_api_attr(source_mesh_collision, "GetApproximationAttr"))
        if not approximation and _has_api(prim, getattr(PhysxSchema, "PhysxConvexDecompositionCollisionAPI", None)):
            approximation = "convexDecomposition"
        if approximation:
            _set_api_attr(mesh_collision, "CreateApproximationAttr", approximation)
        _copy_collision_schema_attrs(
            modules=modules,
            source_prim=prim,
            target_prim=temp_prim,
        )

        prim_id = modules["PhysicsSchemaTools"].sdfPathToInt(temp_path)
        source_world_matrix = target_world_matrix
        callback_payload: dict[str, Any] = {}

        def on_result(result: Any, convexes: list[Any]) -> None:
            callback_payload["result"] = _enum_name(result)
            callback_payload["convexes"] = [
                _convex_summary(
                    convex,
                    target_world_matrix,
                    include_samples,
                    max_sample_items,
                    source_world_matrix=source_world_matrix,
                    target_inverse_matrix=target_inverse_matrix,
                )
                for convex in list(convexes)
            ]

        cooking.request_convex_collision_representation(
            int(stage_id),
            int(prim_id),
            False,
            on_result,
        )
        convexes = callback_payload.get("convexes", [])
        if convexes:
            return temp_path, convexes
        direct_convexes = _direct_convex_mesh_data(
            cooking,
            temp_path,
            target_world_matrix,
            include_samples,
            max_sample_items,
            source_world_matrix=source_world_matrix,
            target_inverse_matrix=target_inverse_matrix,
        )
        return temp_path, direct_convexes
    finally:
        stage.RemovePrim(Sdf.Path(temp_root))


def _prim_is_active_mesh(modules: dict[str, Any], prim: Any) -> bool:
    try:
        return bool(prim.IsActive() and prim.IsA(modules["UsdGeom"].Mesh))
    except Exception:
        return False


def _merged_mesh_geometry_in_target_frame(
    *,
    modules: dict[str, Any],
    mesh_prims: list[Any],
    target_inverse_matrix: Any,
) -> tuple[list[list[float]], list[int], list[int]]:
    points: list[list[float]] = []
    face_counts: list[int] = []
    face_indices: list[int] = []
    for prim in mesh_prims:
        try:
            mesh = modules["UsdGeom"].Mesh(prim)
            prim_points = mesh.GetPointsAttr().Get() or []
            prim_face_counts = mesh.GetFaceVertexCountsAttr().Get() or []
            prim_face_indices = mesh.GetFaceVertexIndicesAttr().Get() or []
            prim_world = _world_matrix(modules, prim, [])
        except Exception:
            continue
        point_offset = len(points)
        for point in prim_points:
            world_point = _transform_point(prim_world, point)
            target_point = _transform_point(target_inverse_matrix, world_point)
            points.append(_rounded_list(target_point, digits=9))
        face_counts.extend(int(count) for count in prim_face_counts)
        face_indices.extend(point_offset + int(index) for index in prim_face_indices)
    return points, face_counts, face_indices


def _copy_collision_schema_attrs(
    *,
    modules: dict[str, Any],
    source_prim: Any,
    target_prim: Any,
) -> None:
    PhysxSchema = modules["PhysxSchema"]
    schema_methods = [
        (
            getattr(PhysxSchema, "PhysxConvexHullCollisionAPI", None),
            ["HullVertexLimit", "MinThickness"],
        ),
        (
            getattr(PhysxSchema, "PhysxConvexDecompositionCollisionAPI", None),
            ["HullVertexLimit", "MaxConvexHulls", "VoxelResolution"],
        ),
        (
            getattr(PhysxSchema, "PhysxSDFMeshCollisionAPI", None),
            ["SdfResolution", "SdfSubgridResolution", "SdfBitsPerSubgridPixel", "SdfMargin"],
        ),
    ]
    for schema_cls, names in schema_methods:
        if schema_cls is None or not _has_api(source_prim, schema_cls):
            continue
        source_api = _make_api(schema_cls, source_prim)
        target_api = schema_cls.Apply(target_prim)
        for name in names:
            value = _attr_value(_api_attr(source_api, f"Get{name}Attr"))
            if value is not None:
                _set_api_attr(target_api, f"Create{name}Attr", value)


def _set_api_attr(api: Any, create_method_name: str, value: Any) -> None:
    try:
        attr = getattr(api, create_method_name)()
    except Exception:
        getter = "Get" + create_method_name.removeprefix("Create")
        attr = _api_attr(api, getter)
    if attr is not None:
        try:
            attr.Set(value)
        except Exception:
            pass


def _convex_summary(
    convex: Any,
    world_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
    *,
    source_world_matrix: Any | None = None,
    target_inverse_matrix: Any | None = None,
) -> dict[str, Any]:
    raw_vertices = list(getattr(convex, "vertices", []) or [])
    vertices = _convex_vertices_in_target_frame(
        raw_vertices,
        source_world_matrix,
        target_inverse_matrix,
    )
    indices = list(getattr(convex, "indices", []) or [])
    polygons = list(getattr(convex, "polygons", []) or [])
    faces = _convex_faces(indices, polygons)
    summary: dict[str, Any] = {
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "polygon_count": len(polygons),
        "vertices": [_rounded_list(_vector_to_list(item), digits=9) for item in vertices],
        "faces": faces,
        "local_aabb": _points_aabb(vertices),
        "world_aabb": _points_aabb(
            [_transform_point(world_matrix, vertex) for vertex in vertices]
        ) if world_matrix is not None else None,
    }
    if include_samples:
        summary["vertices_sample"] = [_vector_to_list(item) for item in vertices[:max_sample_items]]
        summary["indices_sample"] = [int(item) for item in indices[:max_sample_items]]
        summary["polygons_sample"] = [
            _plain_value(item) for item in polygons[:max_sample_items]
        ]
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
    raw_vertices = list(data.get("vertices") or [])
    vertices = _convex_vertices_in_target_frame(
        raw_vertices,
        source_world_matrix,
        target_inverse_matrix,
    )
    indices = list(data.get("indices") or [])
    polygons = list(data.get("polygons") or [])
    faces = _convex_faces(indices, polygons)
    summary: dict[str, Any] = {
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "polygon_count": len(polygons),
        "vertices": [_rounded_list(_vector_to_list(item), digits=9) for item in vertices],
        "faces": faces,
        "local_aabb": _points_aabb(vertices),
        "world_aabb": _points_aabb(
            [_transform_point(world_matrix, vertex) for vertex in vertices]
        ) if world_matrix is not None else None,
    }
    if include_samples:
        summary["vertices_sample"] = [_vector_to_list(item) for item in vertices[:max_sample_items]]
        summary["indices_sample"] = [int(item) for item in indices[:max_sample_items]]
        summary["polygons_sample"] = [
            _plain_value(item) for item in polygons[:max_sample_items]
        ]
    return summary


def _cooking_prim_candidates(modules: dict[str, Any], prim: Any) -> list[Any]:
    candidates = [prim]
    try:
        if prim.IsA(modules["UsdGeom"].Mesh):
            return candidates
    except Exception:
        return candidates
    for descendant in _iter_prim_subtree(prim):
        try:
            if descendant.IsActive() and descendant.IsA(modules["UsdGeom"].Mesh):
                candidates.append(descendant)
        except Exception:
            continue
    return candidates


def _convex_vertices_in_target_frame(
    vertices: list[Any],
    source_world_matrix: Any | None,
    target_inverse_matrix: Any | None,
) -> list[list[float]]:
    if source_world_matrix is None or target_inverse_matrix is None:
        return [_vector_to_list(vertex) for vertex in vertices]
    transformed = []
    for vertex in vertices:
        world_point = _transform_point(source_world_matrix, vertex)
        transformed.append(_transform_point(target_inverse_matrix, world_point))
    return transformed


def _convex_faces(indices: list[Any], polygons: list[Any]) -> list[list[int]]:
    faces: list[list[int]] = []
    index_values = [int(item) for item in indices]
    for polygon in polygons:
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
        if base is None or count is None or count < 3:
            continue
        polygon_indices = index_values[base : base + count]
        if len(polygon_indices) < 3:
            continue
        for index in range(1, len(polygon_indices) - 1):
            faces.append([polygon_indices[0], polygon_indices[index], polygon_indices[index + 1]])
    if not faces and len(index_values) >= 3 and len(index_values) % 3 == 0:
        # Isaac/PhysX can expose polygon objects without Python-visible
        # base/count fields. In that case the cooked convex index buffer is
        # already triangle-flat, which is enough for an OBJ collider export.
        faces = [
            [index_values[index], index_values[index + 1], index_values[index + 2]]
            for index in range(0, len(index_values), 3)
        ]
    return faces


def _first_int_attr(value: Any, *names: str) -> int | None:
    for name in names:
        if isinstance(value, dict) and name in value:
            try:
                return int(value[name])
            except Exception:
                continue
        try:
            attr = getattr(value, name)
        except Exception:
            continue
        try:
            return int(attr() if callable(attr) else attr)
        except Exception:
            continue
    return None


def _acquire_cooking_interface(modules: dict[str, Any]) -> Any:
    physx_bindings = modules.get("omni.physx.bindings._physx")
    if physx_bindings is None:
        return None
    try:
        return physx_bindings.acquire_physx_cooking_interface()
    except Exception:
        return None


def _environment_info(modules: dict[str, Any]) -> dict[str, Any]:
    return {
        "python": sys.version,
        "executable": sys.executable,
        "platform": platform.platform(),
        "kit": _kit_info(modules),
        "extensions": _extension_info(
            modules,
            [
                "omni.physx",
                "omni.usd",
                "isaacsim.core.api",
                "omni.isaac.core",
            ],
        ),
    }


def _kit_info(modules: dict[str, Any]) -> dict[str, Any]:
    omni_kit_app = modules.get("omni.kit.app")
    if omni_kit_app is None:
        return {"available": False}
    try:
        app = omni_kit_app.get_app()
    except Exception as exc:
        return {"available": False, "error": str(exc)}
    info: dict[str, Any] = {"available": True}
    for key, method_name in [
        ("app_name", "get_app_name"),
        ("app_version", "get_app_version"),
        ("build_version", "get_build_version"),
    ]:
        try:
            method = getattr(app, method_name)
            info[key] = method()
        except Exception:
            pass
    return info


def _extension_info(modules: dict[str, Any], extension_names: list[str]) -> dict[str, Any]:
    omni_kit_app = modules.get("omni.kit.app")
    if omni_kit_app is None:
        return {}
    try:
        manager = omni_kit_app.get_app().get_extension_manager()
    except Exception:
        return {}

    results: dict[str, Any] = {}
    for extension_name in extension_names:
        try:
            extension_id = manager.get_enabled_extension_id(extension_name)
            data = manager.get_extension_dict(extension_id) if extension_id else None
            results[extension_name] = {
                "enabled": bool(extension_id),
                "id": extension_id,
                "version": _dict_get(data, ["package", "version"]),
                "path": manager.get_extension_path(extension_id) if extension_id else None,
            }
        except Exception as exc:
            results[extension_name] = {"enabled": False, "error": str(exc)}
    return results


def _stage_info(modules: dict[str, Any], stage: Any, stage_id: int | None) -> dict[str, Any]:
    UsdGeom = modules["UsdGeom"]
    root_layer = stage.GetRootLayer()
    session_layer = stage.GetSessionLayer()
    return {
        "stage_id": stage_id,
        "root_layer": root_layer.identifier if root_layer else None,
        "session_layer": session_layer.identifier if session_layer else None,
        "meters_per_unit": _safe_call(lambda: float(UsdGeom.GetStageMetersPerUnit(stage))),
        "up_axis": _safe_call(lambda: str(UsdGeom.GetStageUpAxis(stage))),
        "default_prim": _safe_call(
            lambda: str(stage.GetDefaultPrim().GetPath())
            if stage.GetDefaultPrim().IsValid()
            else None
        ),
    }


def _summary(colliders: list[ColliderRecord], errors: list[ProbeError]) -> dict[str, Any]:
    type_counts: dict[str, int] = {}
    approximation_counts: dict[str, int] = {}
    cooked_counts = {"attempted": 0, "available": 0, "failed": 0}
    for collider in colliders:
        type_counts[collider.type_name] = type_counts.get(collider.type_name, 0) + 1
        approximation = None
        if collider.mesh_collision_api:
            approximation = collider.mesh_collision_api.get("approximation")
        if approximation:
            approximation_counts[str(approximation)] = approximation_counts.get(str(approximation), 0) + 1
        if collider.cooked_convex.attempted:
            cooked_counts["attempted"] += 1
            if collider.cooked_convex.available:
                cooked_counts["available"] += 1
            elif collider.cooked_convex.error:
                cooked_counts["failed"] += 1

    return {
        "collider_count": len(colliders),
        "type_counts": type_counts,
        "mesh_approximation_counts": approximation_counts,
        "cooked_convex": cooked_counts,
        "error_count": len(errors),
    }


def _empty_report(
    options: ProbeOptions,
    modules: dict[str, Any] | None,
    errors: list[ProbeError],
) -> ProbeReport:
    return ProbeReport(
        probe_version="0.1",
        environment=_environment_info(modules or {}) if modules else {},
        stage={},
        filters={
            "include_subtree": options.include_subtree,
            "exclude_subtree": options.exclude_subtree,
            "max_colliders": options.max_colliders,
            "cooked_convex": options.cooked_convex,
            "include_hidden": options.include_hidden,
            "purpose": options.purpose,
            "probe_prim": options.probe_prim,
            "mesh_export_dir": options.mesh_export_dir,
            "mesh_source_preference": options.mesh_source_preference,
        },
        summary={"collider_count": 0, "error_count": len(errors)},
        probed_prims=[],
        colliders=[],
        object_meshes={},
        errors=errors,
    )


def _write_payload(payload: dict[str, Any], args: argparse.Namespace) -> None:
    text = json.dumps(
        _json_safe(payload),
        allow_nan=False,
        indent=2 if args.pretty else None,
        sort_keys=True,
    )
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
    else:
        print(text)


def _profile_payload(report: ProbeReport, output_profile: str) -> dict[str, Any]:
    if output_profile == "llm_scene_authoring":
        return _llm_scene_authoring_payload(report)
    return asdict(report)


def _llm_scene_authoring_payload(report: ProbeReport) -> dict[str, Any]:
    groups: dict[str, list[ColliderRecord]] = {}
    for collider in report.colliders:
        groups.setdefault(_logical_object_root(collider.path), []).append(collider)

    mesh_source_preference = _mesh_source_preference_from_filters(report.filters)
    objects = [
        _llm_object_summary(
            root,
            colliders,
            report.object_meshes.get(root, []),
            mesh_source_preference=mesh_source_preference,
        )
        for root, colliders in sorted(groups.items(), key=lambda item: item[0])
    ]
    return {
        "version": "manyforge.isaac.collider_report.v0.1",
        "output_profile": "llm_scene_authoring",
        "success": not report.errors,
        "stage": report.stage,
        "filters": report.filters,
        "summary": {
            "object_count": len(objects),
            "collider_count": len(report.colliders),
            "probed_prim_count": len(report.probed_prims),
            "error_count": len(report.errors),
            "recommended_review": True,
        },
        "probed_prims": [asdict(item) for item in report.probed_prims],
        "objects": objects,
        "object_meshes": report.object_meshes,
        "warnings": _llm_report_warnings(report, objects),
        "errors": [asdict(error) for error in report.errors],
    }


def _llm_object_summary(
    root: str,
    colliders: list[ColliderRecord],
    object_meshes: list[dict[str, Any]],
    *,
    mesh_source_preference: str = "auto",
) -> dict[str, Any]:
    union_aabb = _union_aabbs([collider.world_aabb for collider in colliders])
    type_counts = _count_values(collider.type_name for collider in colliders)
    schema_counts = _count_values(
        collider.geometry_schema.get("schema") for collider in colliders
    )
    approximation_counts = _count_values(
        (collider.mesh_collision_api or {}).get("approximation")
        for collider in colliders
    )
    cooked_valid = sum(1 for collider in colliders if collider.cooked_convex.available)
    warnings = _llm_object_warnings(colliders, union_aabb)
    recommendation = _manyforge_shape_recommendation(
        root=root,
        colliders=colliders,
        object_meshes=object_meshes,
        union_aabb=union_aabb,
        warnings=warnings,
        mesh_source_preference=mesh_source_preference,
    )
    return {
        "id_hint": _object_id_hint(root),
        "source_root": root,
        "runtime_pose_source_hint": root,
        "collider_count": len(colliders),
        "collider_paths": [collider.path for collider in colliders],
        "type_counts": type_counts,
        "geometry_schema_counts": schema_counts,
        "mesh_approximation_counts": approximation_counts,
        "world_aabb": union_aabb,
        "object_meshes": object_meshes,
        "collider_mesh_candidates": _collider_mesh_candidates(colliders, union_aabb),
        "cooked_convex_valid_count": cooked_valid,
        "visibility": sorted(
            {
                collider.computed_visibility
                for collider in colliders
                if collider.computed_visibility
            }
        ),
        "purpose": sorted(
            {collider.purpose for collider in colliders if collider.purpose}
        ),
        "recommended_manyforge_shape": recommendation,
        "warnings": warnings,
    }


def _manyforge_shape_recommendation(
    *,
    root: str,
    colliders: list[ColliderRecord],
    object_meshes: list[dict[str, Any]],
    union_aabb: dict[str, Any] | None,
    warnings: list[str],
    mesh_source_preference: str = "auto",
) -> dict[str, Any] | None:
    preference = (
        mesh_source_preference
        if mesh_source_preference in _MESH_SOURCE_PREFERENCES
        else "auto"
    )
    if preference in {"auto", "original"}:
        object_mesh = _preferred_original_mesh_record(object_meshes)
        if object_mesh is not None:
            warnings.append(
                "Mesh recommendation requires the mesh resource path to be readable by the ManyForge runtime."
            )
            return {
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": object_mesh["mesh_resource_uri"],
                    "mesh_scale": object_mesh.get("mesh_scale", [1.0, 1.0, 1.0]),
                },
                "pose": object_mesh["pose"],
                "confidence": "high" if object_mesh.get("source") == "exported_original_mesh" else "medium",
                "reason": (
                    "Using the original object mesh exported from Isaac."
                    if object_mesh.get("source") == "exported_original_mesh"
                    else "Using an existing original object mesh asset discovered in the USD composition."
                ),
                "source_root": root,
                "mesh_source": object_mesh.get("source"),
                "mesh_source_prim_path": object_mesh.get("prim_path"),
            }
        if preference == "original":
            warnings.append(
                "Original mesh source was requested, but no usable original mesh was found; falling back to collider/primitive geometry."
            )

    if preference in {"auto", "original", "collider"}:
        collider_mesh = _preferred_collider_mesh_record(
            object_meshes,
            colliders,
            union_aabb,
        )
        if collider_mesh is not None:
            if len(_collider_mesh_candidates(colliders, union_aabb)) > 1:
                warnings.append(
                    "Multiple collider mesh candidates were found; using the first candidate in collider traversal order."
                )
            warnings.append(
                "Mesh recommendation requires the mesh resource path to be readable by the ManyForge runtime."
            )
            return {
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": collider_mesh["mesh_resource_uri"],
                    "mesh_scale": collider_mesh.get("mesh_scale", [1.0, 1.0, 1.0]),
                },
                "pose": collider_mesh["pose"],
                "confidence": collider_mesh["confidence"],
                "reason": (
                    "Using a directly usable Isaac collider mesh because "
                    f"mesh_source_preference={preference!r}."
                ),
                "source_root": root,
                "mesh_source": collider_mesh["source"],
                "mesh_source_prim_path": collider_mesh["prim_path"],
            }
        if preference == "collider":
            warnings.append(
                "Collider mesh source was requested, but no directly usable collider mesh was found; falling back to collider primitive/AABB geometry."
            )

    if preference == "primitive":
        warnings.append(
            "Primitive mesh source preference requested; original and collider mesh assets were ignored."
        )

    return _primitive_shape_recommendation(
        root=root,
        colliders=colliders,
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
    if union_aabb is None:
        return None
    size = union_aabb["size"]
    if any(float(component) <= 0.0 for component in size):
        return None

    confidence = "medium"
    reason = "Using world AABB as a conservative box."
    if len(colliders) == 1:
        collider = colliders[0]
        schema = collider.geometry_schema.get("schema")
        approximation = (collider.mesh_collision_api or {}).get("approximation")
        if schema == "cube" or approximation == "boundingCube":
            confidence = "high"
            reason = "Source collider is an authored cube or boundingCube approximation."
        elif approximation in {"convexHull", "convexDecomposition", "sdf", "none"}:
            confidence = "medium"
    elif len(colliders) > 8:
        confidence = "low"
        reason = "Using world AABB as a conservative box over many collider fragments."
        warnings.append(
            "Object has many collider fragments; a single world-AABB box may be overly conservative."
        )
    else:
        confidence = "medium"
        reason = "Using world AABB as a conservative box over grouped collider fragments."

    return {
        "shape": {
            "type": "box",
            "box_dimensions_m": _rounded_list(size),
        },
        "pose": {
            "frame_id": "universe",
            "position_m": _rounded_list(union_aabb["center"]),
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
        "confidence": confidence,
        "reason": reason,
        "source_root": root,
    }


def _mesh_source_preference_from_filters(filters: dict[str, Any]) -> str:
    value = filters.get("mesh_source_preference", "auto")
    return value if isinstance(value, str) and value in _MESH_SOURCE_PREFERENCES else "auto"


def _preferred_original_mesh_record(records: list[dict[str, Any]]) -> dict[str, Any] | None:
    for source in ("exported_original_mesh", "referenced_original_mesh"):
        for record in records:
            if record.get("source") == source and record.get("mesh_resource_uri"):
                return record
    for record in records:
        if (
            record.get("mesh_resource_uri")
            and record.get("source") not in {"referenced_collider_mesh"}
        ):
            return record
    return None


def _preferred_collider_mesh_record(
    object_meshes: list[dict[str, Any]],
    colliders: list[ColliderRecord],
    fallback_aabb: dict[str, Any] | None,
) -> dict[str, Any] | None:
    for source in (
        "exported_cooked_convex_collider_mesh",
        "exported_primitive_collider_mesh",
        "exported_collider_mesh",
        "referenced_collider_mesh",
    ):
        for record in object_meshes:
            if record.get("source") == source and record.get("mesh_resource_uri"):
                return {
                    "source": source,
                    "prim_path": record.get("prim_path"),
                    "mesh_resource_uri": record["mesh_resource_uri"],
                    "mesh_resource_candidates": record.get("mesh_resource_candidates", []),
                    "pose": record["pose"],
                    "mesh_scale": record.get("mesh_scale", [1.0, 1.0, 1.0]),
                    "confidence": "high" if source != "referenced_collider_mesh" else "medium",
                }
    candidates = _collider_mesh_candidates(colliders, fallback_aabb)
    return candidates[0] if candidates else None


def _collider_mesh_candidates(
    colliders: list[ColliderRecord],
    fallback_aabb: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    candidates: list[dict[str, Any]] = []
    for collider in colliders:
        schema = collider.geometry_schema.get("schema")
        mesh_resource_uri = collider.geometry_schema.get("mesh_resource_uri")
        if schema != "mesh" or not isinstance(mesh_resource_uri, str) or not mesh_resource_uri:
            continue
        approximation = (collider.mesh_collision_api or {}).get("approximation")
        aabb = collider.world_aabb or fallback_aabb
        if aabb is None:
            continue
        candidates.append(
            {
                "source": "referenced_collider_mesh",
                "prim_path": collider.path,
                "mesh_resource_uri": mesh_resource_uri,
                "mesh_resource_candidates": list(
                    collider.geometry_schema.get("mesh_resource_candidates") or [mesh_resource_uri]
                ),
                "pose": _mesh_pose_from_collider(collider, aabb),
                "mesh_scale": _mesh_scale_from_transform(collider.world_transform),
                "confidence": "high" if approximation in {"none", "convexHull"} else "medium",
                "mesh_approximation": approximation,
            }
        )
    return candidates


def _mesh_pose_from_collider(
    collider: ColliderRecord,
    fallback_aabb: dict[str, Any],
) -> dict[str, Any]:
    matrix = collider.world_transform
    return _mesh_pose_from_transform(matrix, fallback_aabb)


def _mesh_pose_from_transform(
    matrix: list[list[float]] | None,
    fallback_aabb: dict[str, Any],
) -> dict[str, Any]:
    if matrix and len(matrix) >= 4 and all(isinstance(row, list) and len(row) >= 4 for row in matrix[:4]):
        return {
            "frame_id": "universe",
            "position_m": _rounded_list([matrix[3][0], matrix[3][1], matrix[3][2]]),
            "orientation_rpy_deg": _rounded_list(_rpy_deg_from_row_major_transform(matrix)),
        }
    return {
        "frame_id": "universe",
        "position_m": _rounded_list(fallback_aabb["center"]),
        "orientation_rpy_deg": [0.0, 0.0, 0.0],
    }


def _mesh_scale_from_transform(matrix: Any) -> list[float]:
    if matrix is None:
        return [1.0, 1.0, 1.0]
    rows = matrix if isinstance(matrix, list) else _matrix_to_rows(matrix)
    if len(rows) < 3 or any(
        not isinstance(row, list) or len(row) < 3
        for row in rows[:3]
    ):
        return [1.0, 1.0, 1.0]
    scale = []
    for row in rows[:3]:
        length = math.sqrt(sum(float(row[col]) * float(row[col]) for col in range(3)))
        scale.append(round(length if length > 1e-12 else 1.0, 9))
    return scale


def _llm_object_warnings(
    colliders: list[ColliderRecord],
    union_aabb: dict[str, Any] | None,
) -> list[str]:
    warnings: list[str] = []
    if union_aabb is None:
        warnings.append("No usable world AABB was available.")
    if any(collider.computed_visibility == "invisible" for collider in colliders):
        warnings.append("One or more collider prims are hidden.")
    if any(not collider.collision_enabled for collider in colliders):
        warnings.append("One or more collider prims have collision disabled or unset.")
    if any((collider.mesh_collision_api or {}).get("approximation") in {"convexDecomposition", "sdf"} for collider in colliders):
        warnings.append(
            "Source uses complex mesh approximation; primitive or mesh export must be reviewed."
        )
    if any(collider.cooked_convex.attempted and not collider.cooked_convex.available for collider in colliders):
        warnings.append("Cooked convex data was not available for one or more colliders.")
    return warnings


def _llm_report_warnings(
    report: ProbeReport,
    objects: list[dict[str, Any]],
) -> list[str]:
    warnings: list[str] = []
    if report.errors:
        warnings.append("Probe completed with errors; inspect the errors field.")
    if any(
        obj["source_root"].startswith("/World/ur10") or "robotiq" in obj["source_root"].lower()
        for obj in objects
    ):
        warnings.append(
            "Robot or gripper geometry appears in the report. Exclude URDF-owned subtrees before generating a ManyForge scene."
        )
    if len(objects) > 50:
        warnings.append(
            "Large object count; assistant tooling should preserve the full report but use grouped views, search, and explicit user selection before generating a ManyForge scene."
        )
    return warnings


def _inspect_object_meshes(
    *,
    modules: dict[str, Any],
    stage: Any,
    colliders: list[ColliderRecord],
    mesh_export_dir: str | None,
    errors: list[ProbeError],
    include_tracebacks: bool,
) -> dict[str, list[dict[str, Any]]]:
    roots = sorted({_logical_object_root(collider.path) for collider in colliders})
    object_meshes: dict[str, list[dict[str, Any]]] = {}
    for root in roots:
        try:
            root_colliders = [
                collider for collider in colliders if _logical_object_root(collider.path) == root
            ]
            records = _object_mesh_records(
                modules=modules,
                stage=stage,
                source_root=root,
                colliders=root_colliders,
                mesh_export_dir=mesh_export_dir,
            )
            if records:
                object_meshes[root] = records
        except Exception as exc:
            errors.append(_probe_error(f"{root}:object_mesh", exc, include_tracebacks))
    return object_meshes


def _object_mesh_records(
    *,
    modules: dict[str, Any],
    stage: Any,
    source_root: str,
    colliders: list[ColliderRecord],
    mesh_export_dir: str | None,
) -> list[dict[str, Any]]:
    root_prim = stage.GetPrimAtPath(source_root)
    if root_prim is None or not root_prim.IsValid():
        return []

    mesh_prims = [
        prim
        for prim in _iter_prim_subtree(root_prim)
        if _is_original_mesh_prim(modules, prim)
    ]
    collider_mesh_prims = [
        prim
        for prim in _iter_prim_subtree(root_prim)
        if _is_collider_mesh_prim(modules, prim)
    ]
    if not mesh_prims and not collider_mesh_prims and not colliders:
        return []

    records: list[dict[str, Any]] = []
    root_aabb = _aabb(modules, _make_bbox_cache(modules), root_prim, local=False, errors=[])
    root_world = _world_matrix(modules, root_prim, [])
    if mesh_export_dir:
        exported = _export_original_mesh_obj(
            modules=modules,
            source_root=source_root,
            root_world=root_world,
            mesh_prims=mesh_prims,
            mesh_export_dir=mesh_export_dir,
            fallback_aabb=root_aabb,
            source="exported_original_mesh",
            filename_suffix="visual",
        )
        if exported is not None:
            records.append(exported)
        exported_collider = _export_original_mesh_obj(
            modules=modules,
            source_root=source_root,
            root_world=root_world,
            mesh_prims=collider_mesh_prims,
            mesh_export_dir=mesh_export_dir,
            fallback_aabb=root_aabb,
            source="exported_collider_mesh",
            filename_suffix="collider",
        )
        if exported_collider is not None:
            records.append(exported_collider)
        cooked_collider = _export_cooked_convex_collider_obj(
            source_root=source_root,
            root_world=root_world,
            colliders=colliders,
            mesh_export_dir=mesh_export_dir,
            fallback_aabb=root_aabb,
        )
        if cooked_collider is not None:
            records.append(cooked_collider)
        primitive_collider = _export_primitive_collider_obj(
            source_root=source_root,
            root_world=root_world,
            colliders=colliders,
            mesh_export_dir=mesh_export_dir,
            fallback_aabb=root_aabb,
        )
        if primitive_collider is not None:
            records.append(primitive_collider)

    for prim in mesh_prims:
        schema = _geometry_schema(modules, prim)
        candidates = schema.get("mesh_resource_candidates") or []
        if not candidates:
            continue
        prim_aabb = (
            _aabb(modules, _make_bbox_cache(modules), prim, local=False, errors=[])
            or root_aabb
            or {"center": [0.0, 0.0, 0.0]}
        )
        prim_world = _world_transform(modules, prim, [])
        records.append(
            {
                "source": "referenced_original_mesh",
                "source_root": source_root,
                "prim_path": str(prim.GetPath()),
                "mesh_resource_uri": candidates[0],
                "mesh_resource_candidates": candidates,
                "pose": _mesh_pose_from_transform(prim_world, prim_aabb),
                "mesh_scale": _mesh_scale_from_transform(prim_world),
                "point_count": schema.get("points_count"),
                "face_count": schema.get("face_vertex_counts_count"),
            }
        )
    for prim in collider_mesh_prims:
        schema = _geometry_schema(modules, prim)
        candidates = schema.get("mesh_resource_candidates") or []
        if not candidates:
            continue
        prim_aabb = (
            _aabb(modules, _make_bbox_cache(modules), prim, local=False, errors=[])
            or root_aabb
            or {"center": [0.0, 0.0, 0.0]}
        )
        prim_world = _world_transform(modules, prim, [])
        records.append(
            {
                "source": "referenced_collider_mesh",
                "source_root": source_root,
                "prim_path": str(prim.GetPath()),
                "mesh_resource_uri": candidates[0],
                "mesh_resource_candidates": candidates,
                "pose": _mesh_pose_from_transform(prim_world, prim_aabb),
                "mesh_scale": _mesh_scale_from_transform(prim_world),
                "point_count": schema.get("points_count"),
                "face_count": schema.get("face_vertex_counts_count"),
            }
        )
    return records


def _iter_prim_subtree(prim: Any) -> Any:
    yield prim
    try:
        children = prim.GetChildren()
    except Exception:
        return
    for child in children:
        yield from _iter_prim_subtree(child)


def _is_original_mesh_prim(modules: dict[str, Any], prim: Any) -> bool:
    try:
        if not prim.IsA(modules["UsdGeom"].Mesh):
            return False
    except Exception:
        return False
    if not prim.IsActive():
        return False
    if _has_collision_api(modules, prim):
        return False
    purpose = _computed_purpose(modules, prim) or "default"
    return purpose != "guide"


def _is_collider_mesh_prim(modules: dict[str, Any], prim: Any) -> bool:
    try:
        return bool(prim.IsA(modules["UsdGeom"].Mesh) and _has_collision_api(modules, prim))
    except Exception:
        return False


def _export_original_mesh_obj(
    *,
    modules: dict[str, Any],
    source_root: str,
    root_world: Any,
    mesh_prims: list[Any],
    mesh_export_dir: str,
    fallback_aabb: dict[str, Any] | None,
    source: str,
    filename_suffix: str,
) -> dict[str, Any] | None:
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    root_inverse = None
    if root_world is not None:
        try:
            root_inverse = root_world.GetInverse()
        except Exception:
            root_inverse = None

    for prim in mesh_prims:
        try:
            mesh = modules["UsdGeom"].Mesh(prim)
            points = mesh.GetPointsAttr().Get() or []
            face_counts = mesh.GetFaceVertexCountsAttr().Get() or []
            face_indices = mesh.GetFaceVertexIndicesAttr().Get() or []
            world = _world_matrix(modules, prim, [])
        except Exception:
            continue
        index_base = len(vertices)
        for point in points:
            world_point = _transform_point(world, point)
            local_point = (
                _transform_point(root_inverse, world_point)
                if root_inverse is not None
                else world_point
            )
            vertices.append(_rounded_list(local_point, digits=9))
        cursor = 0
        for count_value in face_counts:
            count = int(count_value)
            polygon = [
                index_base + int(face_indices[cursor + offset]) + 1
                for offset in range(count)
            ]
            cursor += count
            if len(polygon) < 3:
                continue
            for index in range(1, len(polygon) - 1):
                faces.append([polygon[0], polygon[index], polygon[index + 1]])

    if not vertices or not faces:
        return None

    export_dir = Path(mesh_export_dir).expanduser()
    export_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_{filename_suffix}" if filename_suffix else ""
    mesh_path = export_dir / f"{_object_id_hint(source_root)}{suffix}.obj"
    lines = [f"# ManyForge {source} export from Isaac source root {source_root}"]
    lines.extend(f"v {vertex[0]:.9g} {vertex[1]:.9g} {vertex[2]:.9g}" for vertex in vertices)
    lines.extend(f"f {face[0]} {face[1]} {face[2]}" for face in faces)
    mesh_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    pose = _mesh_pose_from_transform(
        _matrix_to_rows(root_world) if root_world is not None else None,
        fallback_aabb or {"center": [0.0, 0.0, 0.0]},
    )
    scale = _mesh_scale_from_transform(
        _matrix_to_rows(root_world) if root_world is not None else None
    )
    uri = mesh_path.resolve().as_uri()
    return {
        "source": source,
        "source_root": source_root,
        "prim_path": source_root,
        "mesh_resource_uri": uri,
        "mesh_resource_candidates": [uri],
        "pose": pose,
        "mesh_scale": scale,
        "point_count": len(vertices),
        "face_count": len(faces),
    }


def _export_cooked_convex_collider_obj(
    *,
    source_root: str,
    root_world: Any,
    colliders: list[ColliderRecord],
    mesh_export_dir: str,
    fallback_aabb: dict[str, Any] | None,
) -> dict[str, Any] | None:
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    root_inverse = _inverse_matrix(root_world)
    hull_count = 0

    for collider in colliders:
        if not collider.cooked_convex.available:
            continue
        collider_world = collider.world_transform
        for convex in collider.cooked_convex.convexes:
            convex_vertices = convex.get("vertices") or []
            convex_faces = convex.get("faces") or []
            if not convex_vertices or not convex_faces:
                continue
            hull_count += 1
            index_base = len(vertices)
            for vertex in convex_vertices:
                world_point = _transform_point(collider_world, vertex)
                local_point = (
                    _transform_point(root_inverse, world_point)
                    if root_inverse is not None
                    else world_point
                )
                vertices.append(_rounded_list(local_point, digits=9))
            for face in convex_faces:
                if len(face) < 3:
                    continue
                faces.append([index_base + int(item) + 1 for item in face[:3]])

    return _write_obj_mesh_record(
        source="exported_cooked_convex_collider_mesh",
        source_root=source_root,
        prim_path=source_root,
        root_world=root_world,
        vertices=vertices,
        faces=faces,
        mesh_export_dir=mesh_export_dir,
        fallback_aabb=fallback_aabb,
        filename_suffix="collider_cooked",
        extra={"convex_hull_count": hull_count},
    )


def _export_primitive_collider_obj(
    *,
    source_root: str,
    root_world: Any,
    colliders: list[ColliderRecord],
    mesh_export_dir: str,
    fallback_aabb: dict[str, Any] | None,
) -> dict[str, Any] | None:
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    root_inverse = _inverse_matrix(root_world)
    primitive_count = 0

    for collider in colliders:
        primitive = _primitive_collider_mesh(collider)
        if primitive is None:
            continue
        primitive_count += 1
        primitive_vertices, primitive_faces = primitive
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
            faces.append([index_base + int(item) + 1 for item in face])

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
    if not vertices or not faces:
        return None

    export_dir = Path(mesh_export_dir).expanduser()
    export_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_{filename_suffix}" if filename_suffix else ""
    mesh_path = export_dir / f"{_object_id_hint(source_root)}{suffix}.obj"
    lines = [f"# ManyForge {source} export from Isaac source root {source_root}"]
    lines.extend(f"v {vertex[0]:.9g} {vertex[1]:.9g} {vertex[2]:.9g}" for vertex in vertices)
    lines.extend(f"f {face[0]} {face[1]} {face[2]}" for face in faces)
    mesh_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    root_rows = _matrix_to_rows(root_world) if root_world is not None else None
    uri = mesh_path.resolve().as_uri()
    record = {
        "source": source,
        "source_root": source_root,
        "prim_path": prim_path,
        "mesh_resource_uri": uri,
        "mesh_resource_candidates": [uri],
        "pose": _mesh_pose_from_transform(root_rows, fallback_aabb or {"center": [0.0, 0.0, 0.0]}),
        "mesh_scale": _mesh_scale_from_transform(root_rows),
        "point_count": len(vertices),
        "face_count": len(faces),
    }
    if extra:
        record.update(extra)
    return record


def _inverse_matrix(matrix: Any) -> Any:
    if matrix is None:
        return None
    try:
        return matrix.GetInverse()
    except Exception:
        return None


def _primitive_collider_mesh(collider: ColliderRecord) -> tuple[list[list[float]], list[list[int]]] | None:
    schema = collider.geometry_schema.get("schema")
    if schema == "cube":
        size = collider.geometry_schema.get("size")
        edge = float(size) if isinstance(size, (int, float)) and float(size) > 0.0 else 1.0
        return _cube_mesh(edge)
    if schema == "sphere":
        radius = collider.geometry_schema.get("radius")
        if isinstance(radius, (int, float)) and float(radius) > 0.0:
            return _sphere_mesh(float(radius))
    if schema == "cylinder":
        radius = collider.geometry_schema.get("radius")
        height = collider.geometry_schema.get("height")
        axis = str(collider.geometry_schema.get("axis") or "Z")
        if (
            isinstance(radius, (int, float))
            and isinstance(height, (int, float))
            and float(radius) > 0.0
            and float(height) > 0.0
        ):
            return _cylinder_mesh(float(radius), float(height), axis)
    return None


def _cube_mesh(edge: float) -> tuple[list[list[float]], list[list[int]]]:
    half = edge / 2.0
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
        [0, 1, 2], [0, 2, 3],
        [4, 6, 5], [4, 7, 6],
        [0, 4, 5], [0, 5, 1],
        [1, 5, 6], [1, 6, 2],
        [2, 6, 7], [2, 7, 3],
        [3, 7, 4], [3, 4, 0],
    ]
    return vertices, faces


def _sphere_mesh(radius: float, segments: int = 16, rings: int = 8) -> tuple[list[list[float]], list[list[int]]]:
    vertices: list[list[float]] = [[0.0, 0.0, radius]]
    for ring in range(1, rings):
        phi = math.pi * ring / rings
        z = radius * math.cos(phi)
        ring_radius = radius * math.sin(phi)
        for segment in range(segments):
            theta = 2.0 * math.pi * segment / segments
            vertices.append([ring_radius * math.cos(theta), ring_radius * math.sin(theta), z])
    vertices.append([0.0, 0.0, -radius])

    faces: list[list[int]] = []
    south = len(vertices) - 1
    for segment in range(segments):
        faces.append([0, 1 + segment, 1 + ((segment + 1) % segments)])
    for ring in range(rings - 2):
        base = 1 + ring * segments
        next_base = base + segments
        for segment in range(segments):
            a = base + segment
            b = base + ((segment + 1) % segments)
            c = next_base + segment
            d = next_base + ((segment + 1) % segments)
            faces.append([a, c, b])
            faces.append([b, c, d])
    last_ring = 1 + (rings - 2) * segments
    for segment in range(segments):
        faces.append([last_ring + segment, south, last_ring + ((segment + 1) % segments)])
    return vertices, faces


def _cylinder_mesh(radius: float, height: float, axis: str, segments: int = 24) -> tuple[list[list[float]], list[list[int]]]:
    half = height / 2.0
    vertices: list[list[float]] = []
    for z in (-half, half):
        for segment in range(segments):
            theta = 2.0 * math.pi * segment / segments
            vertices.append(_axis_point(radius * math.cos(theta), radius * math.sin(theta), z, axis))
    bottom_center = len(vertices)
    vertices.append(_axis_point(0.0, 0.0, -half, axis))
    top_center = len(vertices)
    vertices.append(_axis_point(0.0, 0.0, half, axis))

    faces: list[list[int]] = []
    for segment in range(segments):
        next_segment = (segment + 1) % segments
        bottom_a = segment
        bottom_b = next_segment
        top_a = segments + segment
        top_b = segments + next_segment
        faces.append([bottom_a, top_a, bottom_b])
        faces.append([bottom_b, top_a, top_b])
        faces.append([bottom_center, bottom_b, bottom_a])
        faces.append([top_center, top_a, top_b])
    return vertices, faces


def _axis_point(x: float, y: float, z: float, axis: str) -> list[float]:
    axis = axis.upper()
    if axis == "X":
        return [z, x, y]
    if axis == "Y":
        return [x, z, y]
    return [x, y, z]


def _logical_object_root(path: str) -> str:
    parts = [part for part in path.split("/") if part]
    if not parts:
        return path
    if parts[0] == "World" and len(parts) >= 2:
        return "/" + "/".join(parts[:2])
    if parts[0] == "background" and len(parts) >= 2:
        return "/" + "/".join(parts[:2])
    return "/" + parts[0]


def _object_id_hint(source_root: str) -> str:
    slug = source_root.strip("/").replace("/", "__")
    cleaned = []
    for char in slug:
        cleaned.append(char if char.isalnum() or char == "_" else "_")
    return "".join(cleaned).strip("_") or "scene_object"


def _union_aabbs(aabbs: list[dict[str, Any] | None]) -> dict[str, Any] | None:
    valid = [aabb for aabb in aabbs if aabb is not None]
    if not valid:
        return None
    minimum = [min(float(aabb["min"][index]) for aabb in valid) for index in range(3)]
    maximum = [max(float(aabb["max"][index]) for aabb in valid) for index in range(3)]
    return {
        "min": _rounded_list(minimum),
        "max": _rounded_list(maximum),
        "size": _rounded_list([maximum[index] - minimum[index] for index in range(3)]),
        "center": _rounded_list([(maximum[index] + minimum[index]) / 2.0 for index in range(3)]),
    }


def _count_values(values: Any) -> dict[str, int]:
    counts: dict[str, int] = {}
    for value in values:
        key = str(value) if value is not None else "unknown"
        counts[key] = counts.get(key, 0) + 1
    return counts


def _rounded_list(values: list[Any], digits: int = 6) -> list[float]:
    return [round(float(value), digits) for value in values]


def _path_included(
    path: str,
    include_subtrees: list[str],
    exclude_subtrees: list[str],
) -> bool:
    if include_subtrees and not any(_path_is_under(path, prefix) for prefix in include_subtrees):
        return False
    return not any(_path_is_under(path, prefix) for prefix in exclude_subtrees)


def _path_is_under(path: str, prefix: str) -> bool:
    normalized = prefix.rstrip("/")
    return path == normalized or path.startswith(normalized + "/")


def _has_collision_api(modules: dict[str, Any], prim: Any) -> bool:
    return _has_api(prim, modules["UsdPhysics"].CollisionAPI)


def _has_api(prim: Any, api_cls: Any) -> bool:
    try:
        return bool(prim.HasAPI(api_cls))
    except Exception:
        try:
            api = api_cls(prim)
            return bool(api and api.GetPrim().IsValid())
        except Exception:
            return False


def _make_api(api_cls: Any, prim: Any) -> Any:
    return api_cls(prim)


def _api_attr(api: Any, method_name: str) -> Any:
    try:
        method = getattr(api, method_name)
        return method()
    except Exception:
        return None


def _attr_value(attr_or_rel: Any) -> Any:
    if attr_or_rel is None:
        return None
    try:
        if hasattr(attr_or_rel, "GetTargets"):
            return [str(item) for item in attr_or_rel.GetTargets()]
        return _plain_value(attr_or_rel.Get())
    except Exception:
        return None


def _inspect_api_attrs(api: Any, getter_names: list[str]) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for getter_name in getter_names:
        key = _getter_to_key(getter_name)
        values[key] = _attr_value(_api_attr(api, getter_name))
    return values


def _getter_to_key(getter_name: str) -> str:
    if getter_name.startswith("Get"):
        getter_name = getter_name[3:]
    if getter_name.endswith("Attr"):
        getter_name = getter_name[:-4]
    if getter_name.endswith("Rel"):
        getter_name = getter_name[:-3]
    result = []
    for index, char in enumerate(getter_name):
        if char.isupper() and index > 0:
            result.append("_")
        result.append(char.lower())
    return "".join(result)


def _physx_collision_schemas(PhysxSchema: Any, prim: Any) -> dict[str, dict[str, Any]]:
    schema_methods = {
        "physx_collision": [
            PhysxSchema.PhysxCollisionAPI,
            [
                "GetContactOffsetAttr",
                "GetRestOffsetAttr",
                "GetTorsionalPatchRadiusAttr",
                "GetMinTorsionalPatchRadiusAttr",
            ],
        ],
        "physx_mesh_collision": [
            getattr(PhysxSchema, "PhysxTriangleMeshCollisionAPI", None),
            [
                "GetMeshSimplificationAttr",
                "GetWeldToleranceAttr",
            ],
        ],
        "physx_convex_hull_collision": [
            getattr(PhysxSchema, "PhysxConvexHullCollisionAPI", None),
            [
                "GetHullVertexLimitAttr",
                "GetMinThicknessAttr",
            ],
        ],
        "physx_convex_decomposition_collision": [
            getattr(PhysxSchema, "PhysxConvexDecompositionCollisionAPI", None),
            [
                "GetHullVertexLimitAttr",
                "GetMaxConvexHullsAttr",
                "GetVoxelResolutionAttr",
            ],
        ],
        "physx_sdf_mesh_collision": [
            getattr(PhysxSchema, "PhysxSDFMeshCollisionAPI", None),
            [
                "GetSdfResolutionAttr",
                "GetSdfSubgridResolutionAttr",
                "GetSdfBitsPerSubgridPixelAttr",
                "GetSdfMarginAttr",
            ],
        ],
    }

    result: dict[str, dict[str, Any]] = {}
    for schema_name, (schema_cls, getters) in schema_methods.items():
        if schema_cls is None or not _has_api(prim, schema_cls):
            continue
        result[schema_name] = _inspect_api_attrs(_make_api(schema_cls, prim), getters)
    return result


def _geometry_schema(modules: dict[str, Any], prim: Any) -> dict[str, Any]:
    UsdGeom = modules["UsdGeom"]
    schema_candidates = [
        ("cube", UsdGeom.Cube, ["GetSizeAttr"]),
        ("sphere", UsdGeom.Sphere, ["GetRadiusAttr"]),
        ("cylinder", UsdGeom.Cylinder, ["GetRadiusAttr", "GetHeightAttr", "GetAxisAttr"]),
        ("capsule", UsdGeom.Capsule, ["GetRadiusAttr", "GetHeightAttr", "GetAxisAttr"]),
        ("cone", UsdGeom.Cone, ["GetRadiusAttr", "GetHeightAttr", "GetAxisAttr"]),
        ("mesh", UsdGeom.Mesh, ["GetPointsAttr", "GetFaceVertexCountsAttr", "GetFaceVertexIndicesAttr"]),
    ]
    for schema_name, schema_cls, getters in schema_candidates:
        try:
            if not prim.IsA(schema_cls):
                continue
            schema = schema_cls(prim)
            result = {"schema": schema_name}
            for getter in getters:
                key = _getter_to_key(getter)
                attr = _api_attr(schema, getter)
                if schema_name == "mesh":
                    result[key + "_count"] = _attr_len(attr)
                else:
                    result[key] = _attr_value(attr)
            if schema_name == "mesh":
                mesh_candidates = _mesh_resource_candidates(prim)
                if mesh_candidates:
                    result["mesh_resource_uri"] = mesh_candidates[0]
                    result["mesh_resource_candidates"] = mesh_candidates
            return result
        except Exception:
            continue
    return {"schema": prim.GetTypeName() or "unknown"}


def _mesh_resource_candidates(prim: Any) -> list[str]:
    candidates: list[str] = []
    seen: set[str] = set()

    for asset_path, base_path in _prim_asset_paths(prim):
        uri = _manyforge_mesh_uri(asset_path, base_path)
        if uri and uri not in seen:
            candidates.append(uri)
            seen.add(uri)

    return candidates


def _prim_asset_paths(prim: Any) -> list[tuple[str, str | None]]:
    paths: list[tuple[str, str | None]] = []

    def add(value: Any, base_path: str | None = None) -> None:
        for asset_path in _extract_asset_paths(value):
            paths.append((asset_path, base_path))

    try:
        add(prim.GetMetadata("references"))
    except Exception:
        pass
    try:
        add(prim.GetMetadata("payload"))
    except Exception:
        pass
    try:
        add(prim.GetAssetInfo())
    except Exception:
        pass

    try:
        for spec in prim.GetPrimStack():
            layer = getattr(spec, "layer", None)
            real_path = getattr(layer, "realPath", None) if layer is not None else None
            base_path = str(real_path) if real_path else None
            add(getattr(spec, "referenceList", None), base_path)
            add(getattr(spec, "payloadList", None), base_path)
            add(getattr(layer, "identifier", None), base_path)
            add(real_path, base_path)
    except Exception:
        pass

    return paths


def _extract_asset_paths(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, (list, tuple, set)):
        result: list[str] = []
        for item in value:
            result.extend(_extract_asset_paths(item))
        return result
    if isinstance(value, dict):
        result = []
        for item in value.values():
            result.extend(_extract_asset_paths(item))
        return result
    result: list[str] = []
    for attr_name in ("assetPath", "path", "resolvedPath"):
        try:
            attr = getattr(value, attr_name)
        except Exception:
            continue
        if isinstance(attr, str) and attr:
            result.append(attr)
    for list_name in ("explicitItems", "prependedItems", "appendedItems", "addedItems"):
        try:
            items = getattr(value, list_name)
        except Exception:
            continue
        result.extend(_extract_asset_paths(items))
    return result


def _manyforge_mesh_uri(asset_path: str, base_path: str | None) -> str | None:
    if not asset_path or asset_path.startswith("<"):
        return None
    normalized = asset_path.strip("@")
    suffix = Path(normalized.split("?", 1)[0]).suffix.lower()
    if suffix not in _SUPPORTED_MANYFORGE_MESH_EXTENSIONS:
        return None
    if normalized.startswith(("package://", "file://")):
        return normalized
    if "://" in normalized:
        return None

    path = Path(normalized)
    if not path.is_absolute() and base_path:
        path = Path(base_path).parent / path
    return path.resolve().as_uri() if path.is_absolute() else normalized


def _attr_len(attr: Any) -> int | None:
    if attr is None:
        return None
    try:
        value = attr.Get()
        return len(value) if value is not None else None
    except Exception:
        return None


def _ancestor_with_api(prim: Any, api_cls: Any) -> str | None:
    current = prim
    while current and current.IsValid():
        if _has_api(current, api_cls):
            return str(current.GetPath())
        current = current.GetParent()
    return None


def _parent_path(prim: Any) -> str | None:
    parent = prim.GetParent()
    if parent and parent.IsValid():
        return str(parent.GetPath())
    return None


def _is_xformable(modules: dict[str, Any], prim: Any) -> bool:
    try:
        return prim.IsA(modules["UsdGeom"].Xformable)
    except Exception:
        return False


def _is_imageable(modules: dict[str, Any], prim: Any) -> bool:
    try:
        return prim.IsA(modules["UsdGeom"].Imageable)
    except Exception:
        return False


def _visibility_attr(modules: dict[str, Any], prim: Any) -> str | None:
    if not _is_imageable(modules, prim):
        return None
    try:
        return _plain_value(modules["UsdGeom"].Imageable(prim).GetVisibilityAttr().Get())
    except Exception:
        return None


def _computed_visibility(modules: dict[str, Any], prim: Any) -> str | None:
    if not _is_imageable(modules, prim):
        return None
    try:
        return str(
            modules["UsdGeom"].Imageable(prim).ComputeVisibility(
                modules["Usd"].TimeCode.Default()
            )
        )
    except Exception:
        return None


def _computed_purpose(modules: dict[str, Any], prim: Any) -> str | None:
    if not _is_imageable(modules, prim):
        return None
    try:
        purpose = modules["UsdGeom"].Imageable(prim).ComputePurpose()
        if purpose:
            return str(purpose)
    except Exception:
        pass
    try:
        return _plain_value(modules["UsdGeom"].Imageable(prim).GetPurposeAttr().Get())
    except Exception:
        return None


def _prim_is_visible(modules: dict[str, Any], prim: Any) -> bool:
    computed = _computed_visibility(modules, prim)
    return computed is None or computed != "invisible"


def _purpose_allowed(modules: dict[str, Any], prim: Any, purposes: list[str]) -> bool:
    if not purposes or "all" in purposes:
        return True
    purpose = _computed_purpose(modules, prim) or "default"
    return purpose in purposes


def _local_transform(modules: dict[str, Any], prim: Any, errors: list[str]) -> list[list[float]] | None:
    try:
        if not _is_xformable(modules, prim):
            return None
        xformable = modules["UsdGeom"].Xformable(prim)
        try:
            result = xformable.GetLocalTransformation()
        except TypeError:
            result = xformable.GetLocalTransformation(modules["Usd"].TimeCode.Default())
        matrix = result[0] if isinstance(result, tuple) else result
        return _matrix_to_rows(matrix)
    except Exception as exc:
        errors.append(f"local transform failed: {exc}")
        return None


def _world_transform(modules: dict[str, Any], prim: Any, errors: list[str]) -> list[list[float]] | None:
    matrix = _world_matrix(modules, prim, errors)
    return _matrix_to_rows(matrix) if matrix is not None else None


def _world_matrix(modules: dict[str, Any], prim: Any, errors: list[str]) -> Any:
    try:
        if not _is_xformable(modules, prim):
            return None
        return modules["UsdGeom"].Xformable(prim).ComputeLocalToWorldTransform(
            modules["Usd"].TimeCode.Default()
        )
    except Exception as exc:
        errors.append(f"world transform failed: {exc}")
        return None


def _aabb(
    modules: dict[str, Any],
    bbox_cache: Any,
    prim: Any,
    *,
    local: bool,
    errors: list[str],
) -> dict[str, Any] | None:
    try:
        bbox = bbox_cache.ComputeLocalBound(prim) if local else bbox_cache.ComputeWorldBound(prim)
        aligned = bbox.ComputeAlignedRange()
        minimum = _vector_to_list(aligned.GetMin())
        maximum = _vector_to_list(aligned.GetMax())
        if any(_is_inf(item) for item in minimum + maximum):
            return None
        return {
            "min": minimum,
            "max": maximum,
            "size": [maximum[index] - minimum[index] for index in range(3)],
            "center": [(maximum[index] + minimum[index]) / 2.0 for index in range(3)],
        }
    except Exception as exc:
        label = "local" if local else "world"
        errors.append(f"{label} aabb failed: {exc}")
        return None


def _matrix_to_rows(matrix: Any) -> list[list[float]]:
    return [[float(matrix[row][col]) for col in range(4)] for row in range(4)]


def _rpy_deg_from_row_major_transform(matrix: list[list[float]]) -> list[float]:
    # Gf.Matrix4d uses row-vector transforms. Transpose the 3x3 block into the
    # conventional column-vector rotation matrix before extracting XYZ RPY.
    rotation = [[float(matrix[col][row]) for col in range(3)] for row in range(3)]
    for col in range(3):
        norm = math.sqrt(sum(rotation[row][col] * rotation[row][col] for row in range(3)))
        if norm > 0.0:
            for row in range(3):
                rotation[row][col] /= norm

    sy = math.sqrt(rotation[0][0] * rotation[0][0] + rotation[1][0] * rotation[1][0])
    singular = sy < 1e-9
    if not singular:
        roll = math.atan2(rotation[2][1], rotation[2][2])
        pitch = math.atan2(-rotation[2][0], sy)
        yaw = math.atan2(rotation[1][0], rotation[0][0])
    else:
        roll = math.atan2(-rotation[1][2], rotation[1][1])
        pitch = math.atan2(-rotation[2][0], sy)
        yaw = 0.0
    return [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)]


def _vector_to_list(value: Any) -> list[float]:
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except Exception:
        return [float(item) for item in value]


def _points_aabb(points: list[Any]) -> dict[str, Any] | None:
    if not points:
        return None
    vectors = [_vector_to_list(point) for point in points]
    minimum = [min(point[index] for point in vectors) for index in range(3)]
    maximum = [max(point[index] for point in vectors) for index in range(3)]
    return {
        "min": minimum,
        "max": maximum,
        "size": [maximum[index] - minimum[index] for index in range(3)],
        "center": [(maximum[index] + minimum[index]) / 2.0 for index in range(3)],
    }


def _transform_point(matrix: Any, point: Any) -> list[float]:
    if matrix is None:
        return _vector_to_list(point)
    try:
        return _vector_to_list(matrix.Transform(point))
    except Exception:
        vector = _vector_to_list(point)
        return [
            vector[0] * float(matrix[0][0])
            + vector[1] * float(matrix[1][0])
            + vector[2] * float(matrix[2][0])
            + float(matrix[3][0]),
            vector[0] * float(matrix[0][1])
            + vector[1] * float(matrix[1][1])
            + vector[2] * float(matrix[2][1])
            + float(matrix[3][1]),
            vector[0] * float(matrix[0][2])
            + vector[1] * float(matrix[1][2])
            + vector[2] * float(matrix[2][2])
            + float(matrix[3][2]),
        ]


def _plain_value(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_plain_value(item) for item in value]
    if hasattr(value, "pathString"):
        return value.pathString
    if hasattr(value, "GetString"):
        try:
            return value.GetString()
        except Exception:
            pass
    try:
        return _vector_to_list(value)
    except Exception:
        return str(value)


def _enum_name(value: Any) -> str:
    name = getattr(value, "name", None)
    if name:
        return str(name)
    return str(value)


def _safe_call(fn: Any) -> Any:
    try:
        return fn()
    except Exception as exc:
        return {"error": str(exc)}


def _dict_get(data: dict[str, Any] | None, keys: list[str]) -> Any:
    current: Any = data
    for key in keys:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    return current


def _is_inf(value: float) -> bool:
    return value == float("inf") or value == float("-inf")


def _json_safe(value: Any) -> Any:
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, tuple):
        return [_json_safe(item) for item in value]
    return value


def _probe_error(scope: str, exc: Exception, include_traceback: bool) -> ProbeError:
    return ProbeError(
        scope=scope,
        message=str(exc),
        traceback=traceback.format_exc() if include_traceback else None,
    )


if __name__ == "__main__":
    raise SystemExit(main())
