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
        },
        summary=_summary(colliders, errors),
        probed_prims=probed_prims,
        colliders=colliders,
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

    try:
        prim_id = modules["PhysicsSchemaTools"].sdfPathToInt(str(prim.GetPath()))
    except Exception as exc:
        return CookedConvexSummary(
            attempted=True,
            available=False,
            error=f"could not encode prim path for PhysX: {exc}",
        )

    callback_payload: dict[str, Any] = {}

    def on_result(result: Any, convexes: list[Any]) -> None:
        callback_payload["result"] = _enum_name(result)
        callback_payload["convexes"] = [
            _convex_summary(convex, world_matrix, include_samples, max_sample_items)
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
        return CookedConvexSummary(
            attempted=True,
            available=False,
            error=f"request_convex_collision_representation failed: {exc}",
        )

    if "result" not in callback_payload:
        return CookedConvexSummary(
            attempted=True,
            available=False,
            error="PhysX cooking callback was not invoked",
        )

    convexes = callback_payload.get("convexes", [])
    result = callback_payload["result"]
    return CookedConvexSummary(
        attempted=True,
        available=bool(convexes),
        result=result,
        convex_count=len(convexes),
        convexes=convexes,
    )


def _convex_summary(
    convex: Any,
    world_matrix: Any,
    include_samples: bool,
    max_sample_items: int,
) -> dict[str, Any]:
    vertices = list(getattr(convex, "vertices", []) or [])
    indices = list(getattr(convex, "indices", []) or [])
    polygons = list(getattr(convex, "polygons", []) or [])
    summary: dict[str, Any] = {
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "polygon_count": len(polygons),
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
        },
        summary={"collider_count": 0, "error_count": len(errors)},
        probed_prims=[],
        colliders=[],
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

    objects = [
        _llm_object_summary(root, colliders)
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
        "warnings": _llm_report_warnings(report, objects),
        "errors": [asdict(error) for error in report.errors],
    }


def _llm_object_summary(root: str, colliders: list[ColliderRecord]) -> dict[str, Any]:
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
        union_aabb=union_aabb,
        warnings=warnings,
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
    union_aabb: dict[str, Any] | None,
    warnings: list[str],
) -> dict[str, Any] | None:
    if union_aabb is None:
        return None
    size = union_aabb["size"]
    if any(float(component) <= 0.0 for component in size):
        return None

    confidence = "medium"
    reason = "ManyForge currently supports box/sphere scene resources; using world AABB as a conservative box."
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
        warnings.append(
            "Object has many collider fragments; a single world-AABB box may be overly conservative."
        )

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
            "Source uses complex mesh approximation; box/sphere export is an approximation."
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
            return result
        except Exception:
            continue
    return {"schema": prim.GetTypeName() or "unknown"}


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
