"""URDF routes - /api/scene/urdf/*

Native R3F visualizer Day 2: serves the deployment's robot URDF as a
small JSON manifest plus one glTF/GLB per visual link. The frontend
fetches the manifest once, then `useGLTF`s each per-link mesh and
parents each one under a group whose transform comes from the live
`link_poses` field on `/api/scene/state`. Joint articulation thus
falls out of FK on the backend kernel (existing) without any new
WebSocket protocol.

Caching:
- The parsed URDF (with meshes resolved) is cached at module scope
  keyed by `(urdf_path, frozen package_paths)` and invalidated by the
  URDF file's mtime. Loading + mesh resolution is expensive (~10s+
  for a UR10e with full collision meshes); the per-link mesh route
  loads only one resource per call after the first manifest hit.
- Per-link glTF bytes are cached after the first conversion. The cache
  is small (one entry per visual link) and lifetimes match the URDF
  cache entry.
"""
from __future__ import annotations

import io
import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_package_uri,
    resolve_robot_model,
)

from .state import ComposerState

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class _LinkVisual:
    """One visual element of one URDF link.

    A URDF link can declare multiple `<visual>` blocks (e.g. UR5's
    ``tool0`` carries six boxes that together draw the rudimentary
    gripper shape). Each ``_LinkVisual`` corresponds to exactly one
    such block, identified by ``(link_name, visual_index)``. ``visuals``
    on the parent ``_LinkEntry`` preserves the URDF declaration order
    so the client can render them deterministically.

    Exactly one of ``mesh_path`` / ``primitive`` is non-None.
    """

    link_name: str
    visual_index: int
    # Set when the visual references an external mesh file (.dae/.stl/.obj/.glb).
    mesh_resource_uri: str | None
    mesh_path: str | None
    mesh_scale: tuple[float, float, float]
    # Set when the visual is a `<box>`, `<cylinder>` or `<sphere>` primitive.
    # The frontend turns these into native Three.js geometries to avoid a
    # trimesh round-trip for tiny shapes (which is what made the UR5
    # gripper render miss in the first cut).
    primitive: dict[str, Any] | None
    visual_origin_xyz: tuple[float, float, float]
    visual_origin_rpy: tuple[float, float, float]


@dataclass(frozen=True)
class _LinkEntry:
    name: str
    visuals: tuple[_LinkVisual, ...]


@dataclass(frozen=True)
class _JointEntry:
    """URDF joint metadata for the Day-5 robot-control overlay panel.

    Limits are ``None`` when the URDF doesn't declare them (e.g.
    continuous joints often omit lower/upper; the operator-facing
    panel renders those with a ``[-π, π]`` slider range as a
    sensible default).
    """

    name: str
    type: str
    axis: tuple[float, float, float]
    parent_link: str
    child_link: str
    limit_lower: Optional[float]
    limit_upper: Optional[float]
    effort_limit: Optional[float]
    velocity_limit: Optional[float]


@dataclass
class _UrdfCacheEntry:
    urdf_path: str
    package_paths: tuple[str, ...]
    mtime_ns: int
    root_link: str
    links: tuple[_LinkEntry, ...]
    joints: tuple[_JointEntry, ...]
    link_names_for_fk: tuple[str, ...]
    mesh_glb_cache: dict[tuple[str, int], bytes]


_cache_lock = threading.Lock()
_urdf_cache: dict[tuple[str, tuple[str, ...]], _UrdfCacheEntry] = {}


def _make_package_handler(package_dirs: list[str]):
    def handler(fname: str) -> str:
        if not fname.startswith("package://"):
            return fname
        resolved = resolve_package_uri(fname, package_dirs)
        if resolved is not None:
            return resolved
        return fname

    return handler


def _resolve_deployment_urdf(state: ComposerState) -> tuple[str, tuple[str, ...]] | None:
    deployment = state.deployment
    if deployment is None or deployment.robot_config is None:
        return None
    if not state.deployment_source_path:
        return None
    robot_config = deployment.robot_config
    if not robot_config.urdf_path:
        return None
    extra_package_paths = (
        [] if robot_config.package_paths else default_package_paths()
    )
    try:
        resolved = resolve_robot_model(
            urdf_path=robot_config.urdf_path,
            srdf_path=robot_config.srdf_path,
            package_paths=robot_config.package_paths,
            deployment_path=state.deployment_source_path,
            extra_package_paths=extra_package_paths,
        )
    except Exception:
        logger.debug("Could not resolve robot model for URDF cache", exc_info=True)
        return None
    return resolved.urdf_path, tuple(resolved.package_paths)


def _load_urdf_entry(urdf_path: str, package_paths: tuple[str, ...]) -> _UrdfCacheEntry | None:
    try:
        mtime_ns = Path(urdf_path).stat().st_mtime_ns
    except OSError:
        return None
    cache_key = (urdf_path, package_paths)
    cached = _urdf_cache.get(cache_key)
    if cached is not None and cached.mtime_ns == mtime_ns:
        return cached

    try:
        import yourdfpy
    except ImportError:
        logger.warning("yourdfpy not installed; URDF visualizer is disabled")
        return None

    try:
        if package_paths:
            urdf_model = yourdfpy.URDF.load(
                urdf_path,
                filename_handler=_make_package_handler(list(package_paths)),
                load_meshes=False,
            )
        else:
            urdf_model = yourdfpy.URDF.load(urdf_path, load_meshes=False)
    except Exception:
        logger.warning("Failed to load URDF %s", urdf_path, exc_info=True)
        return None

    link_map = getattr(urdf_model, "link_map", {}) or {}
    root_link = ""
    try:
        root_link = str(urdf_model.base_link)
    except Exception:
        if link_map:
            root_link = next(iter(link_map.keys()))

    import math

    def _origin_to_xyz_rpy(
        origin: Any,
    ) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
        if origin is None:
            return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
        try:
            xyz = (
                float(origin[0, 3]),
                float(origin[1, 3]),
                float(origin[2, 3]),
            )
            r00, r10 = float(origin[0, 0]), float(origin[1, 0])
            r20, r21, r22 = float(origin[2, 0]), float(origin[2, 1]), float(origin[2, 2])
            r12 = float(origin[1, 2])
            r11 = float(origin[1, 1])
            sy = math.sqrt(r00 * r00 + r10 * r10)
            if sy < 1e-6:
                roll = math.atan2(-r12, r11)
                pitch = math.atan2(-r20, sy)
                yaw = 0.0
            else:
                roll = math.atan2(r21, r22)
                pitch = math.atan2(-r20, sy)
                yaw = math.atan2(r10, r00)
            return xyz, (roll, pitch, yaw)
        except Exception:
            return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)

    def _resolve_mesh_path(mesh_uri: str) -> str | None:
        if mesh_uri.startswith("package://"):
            return resolve_package_uri(mesh_uri, list(package_paths))
        if mesh_uri.startswith("file://"):
            return mesh_uri[len("file://") :]
        return mesh_uri

    def _build_visual(
        link_name: str,
        visual_index: int,
        visual: Any,
    ) -> _LinkVisual | None:
        geometry = getattr(visual, "geometry", None)
        if geometry is None:
            return None
        origin = getattr(visual, "origin", None)
        origin_xyz, origin_rpy = _origin_to_xyz_rpy(origin)

        mesh = getattr(geometry, "mesh", None)
        if mesh is not None:
            filename = getattr(mesh, "filename", None)
            if not isinstance(filename, str) or not filename:
                return None
            scale = getattr(mesh, "scale", None)
            if scale is not None and len(scale) == 3:
                mesh_scale = (float(scale[0]), float(scale[1]), float(scale[2]))
            else:
                mesh_scale = (1.0, 1.0, 1.0)
            return _LinkVisual(
                link_name=link_name,
                visual_index=visual_index,
                mesh_resource_uri=filename,
                mesh_path=_resolve_mesh_path(filename),
                mesh_scale=mesh_scale,
                primitive=None,
                visual_origin_xyz=origin_xyz,
                visual_origin_rpy=origin_rpy,
            )

        box = getattr(geometry, "box", None)
        if box is not None:
            size = getattr(box, "size", None)
            if size is None or len(size) != 3:
                return None
            return _LinkVisual(
                link_name=link_name,
                visual_index=visual_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={
                    "type": "box",
                    "size_m": [float(size[0]), float(size[1]), float(size[2])],
                },
                visual_origin_xyz=origin_xyz,
                visual_origin_rpy=origin_rpy,
            )

        cylinder = getattr(geometry, "cylinder", None)
        if cylinder is not None:
            radius = getattr(cylinder, "radius", None)
            length = getattr(cylinder, "length", None)
            if radius is None or length is None:
                return None
            return _LinkVisual(
                link_name=link_name,
                visual_index=visual_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={
                    "type": "cylinder",
                    "radius_m": float(radius),
                    "length_m": float(length),
                },
                visual_origin_xyz=origin_xyz,
                visual_origin_rpy=origin_rpy,
            )

        sphere = getattr(geometry, "sphere", None)
        if sphere is not None:
            radius = getattr(sphere, "radius", None)
            if radius is None:
                return None
            return _LinkVisual(
                link_name=link_name,
                visual_index=visual_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={"type": "sphere", "radius_m": float(radius)},
                visual_origin_xyz=origin_xyz,
                visual_origin_rpy=origin_rpy,
            )
        return None

    links: list[_LinkEntry] = []
    fk_link_names: list[str] = []
    for link_name, link in link_map.items():
        fk_link_names.append(str(link_name))
        raw_visuals = getattr(link, "visuals", None) or []
        link_visuals: list[_LinkVisual] = []
        for index, visual in enumerate(raw_visuals):
            built = _build_visual(str(link_name), index, visual)
            if built is not None:
                link_visuals.append(built)
        if link_visuals:
            links.append(_LinkEntry(name=str(link_name), visuals=tuple(link_visuals)))

    # Joint metadata for the Day-5 robot-control overlay. We capture
    # everything we need from `yourdfpy.Joint` here so the manifest
    # response doesn't have to re-parse the URDF on every request.
    joints: list[_JointEntry] = []
    joint_map = getattr(urdf_model, "joint_map", {}) or {}
    for joint_name, joint in joint_map.items():
        joint_type = str(getattr(joint, "type", "fixed") or "fixed")
        axis_value = getattr(joint, "axis", None)
        if axis_value is not None and len(axis_value) == 3:
            axis = (float(axis_value[0]), float(axis_value[1]), float(axis_value[2]))
        else:
            axis = (1.0, 0.0, 0.0)
        parent_link = str(getattr(joint, "parent", "") or "")
        child_link = str(getattr(joint, "child", "") or "")
        limit = getattr(joint, "limit", None)
        limit_lower = getattr(limit, "lower", None) if limit is not None else None
        limit_upper = getattr(limit, "upper", None) if limit is not None else None
        effort_limit = getattr(limit, "effort", None) if limit is not None else None
        velocity_limit = getattr(limit, "velocity", None) if limit is not None else None
        joints.append(
            _JointEntry(
                name=str(joint_name),
                type=joint_type,
                axis=axis,
                parent_link=parent_link,
                child_link=child_link,
                limit_lower=None if limit_lower is None else float(limit_lower),
                limit_upper=None if limit_upper is None else float(limit_upper),
                effort_limit=None if effort_limit is None else float(effort_limit),
                velocity_limit=None if velocity_limit is None else float(velocity_limit),
            )
        )

    entry = _UrdfCacheEntry(
        urdf_path=urdf_path,
        package_paths=package_paths,
        mtime_ns=mtime_ns,
        root_link=root_link,
        links=tuple(links),
        joints=tuple(joints),
        link_names_for_fk=tuple(fk_link_names),
        mesh_glb_cache={},
    )
    _urdf_cache[cache_key] = entry
    return entry


def get_urdf_entry(state: ComposerState) -> _UrdfCacheEntry | None:
    resolution = _resolve_deployment_urdf(state)
    if resolution is None:
        return None
    urdf_path, package_paths = resolution
    with _cache_lock:
        return _load_urdf_entry(urdf_path, package_paths)


def visual_link_names_for_fk(state: ComposerState) -> tuple[str, ...]:
    """Return the link names the scene-state route should fetch poses for."""
    entry = get_urdf_entry(state)
    if entry is None:
        return ()
    return tuple(link.name for link in entry.links)


def _find_visual(
    entry: _UrdfCacheEntry,
    link_name: str,
    visual_index: int,
) -> _LinkVisual | None:
    for link in entry.links:
        if link.name != link_name:
            continue
        for visual in link.visuals:
            if visual.visual_index == visual_index:
                return visual
        return None
    return None


def _render_visual_glb(
    entry: _UrdfCacheEntry,
    visual: _LinkVisual,
) -> bytes | None:
    cache_key = (visual.link_name, visual.visual_index)
    cached = entry.mesh_glb_cache.get(cache_key)
    if cached is not None:
        return cached
    # Primitive-only visuals are rendered client-side via Three.js native
    # geometries — no glTF needed. The mesh route returns 404 for these
    # and the frontend uses the manifest's `primitive` block instead.
    if visual.mesh_path is None:
        return None
    mesh_file = Path(visual.mesh_path)
    if not mesh_file.exists():
        logger.warning(
            "Mesh file missing for link %s visual %d at %s",
            visual.link_name,
            visual.visual_index,
            mesh_file,
        )
        return None
    try:
        import trimesh
    except ImportError:
        logger.warning("trimesh not installed; URDF mesh route is disabled")
        return None
    try:
        loaded = trimesh.load(mesh_file, force="mesh")
    except Exception:
        logger.warning("trimesh.load failed for %s", mesh_file, exc_info=True)
        return None
    mesh: Any = loaded
    if hasattr(loaded, "dump"):
        try:
            mesh = loaded.dump(concatenate=True)
        except Exception:
            mesh = loaded
    sx, sy, sz = visual.mesh_scale
    if (abs(sx - 1.0) + abs(sy - 1.0) + abs(sz - 1.0)) > 1e-9:
        try:
            mesh.apply_scale([sx, sy, sz])
        except Exception:
            pass
    buffer = io.BytesIO()
    try:
        data = mesh.export(file_type="glb")
        if isinstance(data, bytes):
            buffer.write(data)
        elif isinstance(data, str):
            buffer.write(data.encode("utf-8"))
        else:
            return None
    except Exception:
        logger.warning("mesh.export to glb failed for %s", mesh_file, exc_info=True)
        return None
    glb_bytes = buffer.getvalue()
    entry.mesh_glb_cache[cache_key] = glb_bytes
    return glb_bytes


def create_urdf_router(state: ComposerState) -> APIRouter:
    """Create the URDF router. Mounted under /api/scene/urdf."""
    router = APIRouter()

    @router.get("/manifest")
    def get_manifest() -> dict[str, Any]:
        entry = get_urdf_entry(state)
        if entry is None:
            raise HTTPException(
                status_code=404,
                detail="No URDF available for the active deployment",
            )
        deployment = state.deployment
        robot_id = None
        if deployment is not None and deployment.robot_config is not None:
            robot_id = deployment.robot_config.robot_id
        return {
            "robot_id": robot_id,
            "root_link": entry.root_link,
            "links": [
                {
                    "name": link.name,
                    "visuals": [
                        {
                            "index": visual.visual_index,
                            "mesh_url": (
                                f"/api/scene/urdf/meshes/{link.name}/{visual.visual_index}"
                                if visual.mesh_path is not None
                                else None
                            ),
                            "primitive": visual.primitive,
                            "visual_origin": {
                                "position_m": list(visual.visual_origin_xyz),
                                "orientation_rpy_rad": list(visual.visual_origin_rpy),
                            },
                        }
                        for visual in link.visuals
                    ],
                }
                for link in entry.links
            ],
            "joints": [
                {
                    "name": joint.name,
                    "type": joint.type,
                    "axis": list(joint.axis),
                    "parent_link": joint.parent_link,
                    "child_link": joint.child_link,
                    "limit_lower": joint.limit_lower,
                    "limit_upper": joint.limit_upper,
                    "effort_limit": joint.effort_limit,
                    "velocity_limit": joint.velocity_limit,
                }
                for joint in entry.joints
            ],
        }

    @router.get("/meshes/{link_name}/{visual_index}")
    def get_mesh(link_name: str, visual_index: int) -> Response:
        entry = get_urdf_entry(state)
        if entry is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "No URDF available for the active deployment"},
            )
        visual = _find_visual(entry, link_name, visual_index)
        if visual is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": f"Unknown link/visual '{link_name}'/{visual_index}",
                },
            )
        glb_bytes = _render_visual_glb(entry, visual)
        if glb_bytes is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": (
                        f"No renderable mesh for link '{link_name}' visual {visual_index}"
                    ),
                },
            )
        return Response(
            content=glb_bytes,
            media_type="model/gltf-binary",
            headers={
                "Cache-Control": "public, max-age=300",
                "ETag": f'"{entry.mtime_ns}-{link_name}-{visual_index}"',
            },
        )

    return router
