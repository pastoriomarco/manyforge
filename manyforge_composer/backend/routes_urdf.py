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

import hashlib
import io
import logging
import threading
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_package_uri,
    resolve_robot_model,
)

from .state import ComposerState

logger = logging.getLogger(__name__)

# Public visual-asset protocol revision. Bump this whenever the
# server-side URDF-to-GLB conversion semantics change. The value is
# deliberately folded into the model key so browsers and drei cannot
# reuse immutable GLBs produced by an older converter.
_VISUAL_ASSET_SCHEMA_VERSION = "r3f-urdf-glb-v3-xyzw"


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
    # Quaternion (x, y, z, w) derived directly from yourdfpy's rotation
    # matrix. The frontend MUST prefer this over rpy, because Three.js's
    # default Euler order is intrinsic XYZ while URDF RPY is fixed-axis
    # ZYX — these disagree for any compound rotation (e.g. UR10E's
    # upperarm origin rpy="π/2 0 -π/2").
    visual_origin_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True)
class _LinkCollision:
    """One collision element of one URDF link.

    Kept separate from `_LinkVisual` so the public manifest can expose
    collision geometry without conflating it with operator-facing visual mesh
    semantics. The viewer renders collisions only as an optional diagnostic
    overlay; planning/collision validation continues to use the backend model.
    """

    link_name: str
    collision_index: int
    mesh_resource_uri: str | None
    mesh_path: str | None
    mesh_scale: tuple[float, float, float]
    primitive: dict[str, Any] | None
    collision_origin_xyz: tuple[float, float, float]
    collision_origin_rpy: tuple[float, float, float]
    collision_origin_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True)
class _LinkEntry:
    name: str
    visuals: tuple[_LinkVisual, ...]
    collisions: tuple[_LinkCollision, ...]


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
    # Non-None when this joint's value is computed from another joint via
    # a URDF `<mimic>` declaration. Such joints cannot be commanded
    # independently and the jog UI must hide them — they follow the
    # master through FK.
    mimic: Optional[tuple[str, float, float]] = None  # (master, multiplier, offset)
    # True when at least one other joint mimics this one. RMA metadata now
    # decides commandability for Robot Control; this remains manifest metadata
    # and a fallback for runtimes without an authority block.
    is_mimic_master: bool = False


@dataclass
class _UrdfCacheEntry:
    urdf_path: str
    package_paths: tuple[str, ...]
    mtime_ns: int
    root_link: str
    links: tuple[_LinkEntry, ...]
    joints: tuple[_JointEntry, ...]
    link_names_for_fk: tuple[str, ...]
    mesh_glb_cache: dict[tuple[str, str, int, str, str], bytes]
    # yourdfpy.URDF model retained for on-demand FK queries
    # (e.g. gripper-root resolution + tcp→root transform for the
    # Move-To unreachable ghost). Loaded with `load_meshes=False` so
    # the per-entry memory cost is the parsed XML + a few numpy
    # arrays. None when the URDF could not be parsed.
    urdf_model: Any = None


_cache_lock = threading.Lock()
_urdf_cache: dict[tuple[str, tuple[str, ...]], _UrdfCacheEntry] = {}
_MAX_URDF_CACHE_ENTRIES = 8
_MAX_GLB_CACHE_ENTRIES_PER_MODEL = 256


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

    def _origin_decompose(
        origin: Any,
    ) -> tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float, float],
    ]:
        """Return (xyz, rpy, quat_xyzw) from a 4x4 origin matrix.

        The quaternion is derived directly from the rotation matrix
        (Shepperd's method), bypassing Euler conventions entirely. The
        frontend MUST consume the quaternion: Three.js's default Euler
        order is intrinsic XYZ while URDF RPY is fixed-axis ZYX, and the
        two disagree for any compound rotation (e.g. UR10E's
        ``upperarm`` visual origin ``rpy="π/2 0 -π/2"`` ends up 90° off
        if rpy is fed to ``<group rotation={...}>`` directly).
        """
        zero_xyz = (0.0, 0.0, 0.0)
        zero_rpy = (0.0, 0.0, 0.0)
        identity_quat = (0.0, 0.0, 0.0, 1.0)
        if origin is None:
            return zero_xyz, zero_rpy, identity_quat
        try:
            xyz = (
                float(origin[0, 3]),
                float(origin[1, 3]),
                float(origin[2, 3]),
            )
            r00 = float(origin[0, 0])
            r01 = float(origin[0, 1])
            r02 = float(origin[0, 2])
            r10 = float(origin[1, 0])
            r11 = float(origin[1, 1])
            r12 = float(origin[1, 2])
            r20 = float(origin[2, 0])
            r21 = float(origin[2, 1])
            r22 = float(origin[2, 2])
            sy = math.sqrt(r00 * r00 + r10 * r10)
            if sy < 1e-6:
                roll = math.atan2(-r12, r11)
                pitch = math.atan2(-r20, sy)
                yaw = 0.0
            else:
                roll = math.atan2(r21, r22)
                pitch = math.atan2(-r20, sy)
                yaw = math.atan2(r10, r00)
            # Shepperd's method: numerically robust quaternion extraction.
            trace = r00 + r11 + r22
            if trace > 0.0:
                s = math.sqrt(trace + 1.0) * 2.0
                qw = 0.25 * s
                qx = (r21 - r12) / s
                qy = (r02 - r20) / s
                qz = (r10 - r01) / s
            elif r00 > r11 and r00 > r22:
                s = math.sqrt(1.0 + r00 - r11 - r22) * 2.0
                qw = (r21 - r12) / s
                qx = 0.25 * s
                qy = (r01 + r10) / s
                qz = (r02 + r20) / s
            elif r11 > r22:
                s = math.sqrt(1.0 + r11 - r00 - r22) * 2.0
                qw = (r02 - r20) / s
                qx = (r01 + r10) / s
                qy = 0.25 * s
                qz = (r12 + r21) / s
            else:
                s = math.sqrt(1.0 + r22 - r00 - r11) * 2.0
                qw = (r10 - r01) / s
                qx = (r02 + r20) / s
                qy = (r12 + r21) / s
                qz = 0.25 * s
            norm = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
            if norm > 1e-12:
                qx /= norm
                qy /= norm
                qz /= norm
                qw /= norm
            else:
                qx, qy, qz, qw = 0.0, 0.0, 0.0, 1.0
            return xyz, (roll, pitch, yaw), (qx, qy, qz, qw)
        except Exception:
            return zero_xyz, zero_rpy, identity_quat

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
        origin_xyz, origin_rpy, origin_xyzw = _origin_decompose(origin)

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
                visual_origin_xyzw=origin_xyzw,
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
                visual_origin_xyzw=origin_xyzw,
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
                visual_origin_xyzw=origin_xyzw,
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
                visual_origin_xyzw=origin_xyzw,
            )
        return None

    def _build_collision(
        link_name: str,
        collision_index: int,
        collision: Any,
    ) -> _LinkCollision | None:
        geometry = getattr(collision, "geometry", None)
        if geometry is None:
            return None
        origin = getattr(collision, "origin", None)
        origin_xyz, origin_rpy, origin_xyzw = _origin_decompose(origin)

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
            return _LinkCollision(
                link_name=link_name,
                collision_index=collision_index,
                mesh_resource_uri=filename,
                mesh_path=_resolve_mesh_path(filename),
                mesh_scale=mesh_scale,
                primitive=None,
                collision_origin_xyz=origin_xyz,
                collision_origin_rpy=origin_rpy,
                collision_origin_xyzw=origin_xyzw,
            )

        box = getattr(geometry, "box", None)
        if box is not None:
            size = getattr(box, "size", None)
            if size is None or len(size) != 3:
                return None
            return _LinkCollision(
                link_name=link_name,
                collision_index=collision_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={
                    "type": "box",
                    "size_m": [float(size[0]), float(size[1]), float(size[2])],
                },
                collision_origin_xyz=origin_xyz,
                collision_origin_rpy=origin_rpy,
                collision_origin_xyzw=origin_xyzw,
            )

        cylinder = getattr(geometry, "cylinder", None)
        if cylinder is not None:
            radius = getattr(cylinder, "radius", None)
            length = getattr(cylinder, "length", None)
            if radius is None or length is None:
                return None
            return _LinkCollision(
                link_name=link_name,
                collision_index=collision_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={
                    "type": "cylinder",
                    "radius_m": float(radius),
                    "length_m": float(length),
                },
                collision_origin_xyz=origin_xyz,
                collision_origin_rpy=origin_rpy,
                collision_origin_xyzw=origin_xyzw,
            )

        sphere = getattr(geometry, "sphere", None)
        if sphere is not None:
            radius = getattr(sphere, "radius", None)
            if radius is None:
                return None
            return _LinkCollision(
                link_name=link_name,
                collision_index=collision_index,
                mesh_resource_uri=None,
                mesh_path=None,
                mesh_scale=(1.0, 1.0, 1.0),
                primitive={"type": "sphere", "radius_m": float(radius)},
                collision_origin_xyz=origin_xyz,
                collision_origin_rpy=origin_rpy,
                collision_origin_xyzw=origin_xyzw,
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
        raw_collisions = getattr(link, "collisions", None) or []
        link_collisions: list[_LinkCollision] = []
        for index, collision in enumerate(raw_collisions):
            built = _build_collision(str(link_name), index, collision)
            if built is not None:
                link_collisions.append(built)
        if link_visuals or link_collisions:
            links.append(
                _LinkEntry(
                    name=str(link_name),
                    visuals=tuple(link_visuals),
                    collisions=tuple(link_collisions),
                )
            )

    # Joint metadata for the Day-5 robot-control overlay. We capture
    # everything we need from `yourdfpy.Joint` here so the manifest
    # response doesn't have to re-parse the URDF on every request.
    #
    # URDF `<mimic>` joints are pinned to a master joint by a linear
    # relation (multiplier·q_master + offset). Followers cannot be commanded
    # independently. RMA metadata provides the authoritative commandable
    # group lists; mimic flags remain useful manifest metadata and a fallback
    # for runtimes without an authority block. We collect mimic data in a
    # first pass and then back-fill `is_mimic_master` on the corresponding
    # entries.
    joint_map = getattr(urdf_model, "joint_map", {}) or {}

    def _extract_mimic(joint: Any) -> Optional[tuple[str, float, float]]:
        m = getattr(joint, "mimic", None)
        if m is None:
            return None
        master = getattr(m, "joint", None)
        if not isinstance(master, str) or not master:
            return None
        multiplier_raw = getattr(m, "multiplier", 1.0)
        offset_raw = getattr(m, "offset", 0.0)
        try:
            multiplier = float(multiplier_raw) if multiplier_raw is not None else 1.0
        except (TypeError, ValueError):
            multiplier = 1.0
        try:
            offset = float(offset_raw) if offset_raw is not None else 0.0
        except (TypeError, ValueError):
            offset = 0.0
        return (master, multiplier, offset)

    mimic_by_name: dict[str, tuple[str, float, float]] = {}
    for joint_name, joint in joint_map.items():
        mimic = _extract_mimic(joint)
        if mimic is not None:
            mimic_by_name[str(joint_name)] = mimic
    masters: set[str] = {master for (master, _, _) in mimic_by_name.values()}

    joints: list[_JointEntry] = []
    for joint_name, joint in joint_map.items():
        name = str(joint_name)
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
                name=name,
                type=joint_type,
                axis=axis,
                parent_link=parent_link,
                child_link=child_link,
                limit_lower=None if limit_lower is None else float(limit_lower),
                limit_upper=None if limit_upper is None else float(limit_upper),
                effort_limit=None if effort_limit is None else float(effort_limit),
                velocity_limit=None if velocity_limit is None else float(velocity_limit),
                mimic=mimic_by_name.get(name),
                is_mimic_master=name in masters,
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
        urdf_model=urdf_model,
    )
    _urdf_cache[cache_key] = entry
    while len(_urdf_cache) > _MAX_URDF_CACHE_ENTRIES:
        _urdf_cache.pop(next(iter(_urdf_cache)))
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


def _mesh_stat_key(mesh_path: str | None) -> str:
    if mesh_path is None:
        return "primitive"
    path = Path(mesh_path)
    try:
        stat = path.stat()
    except OSError:
        return f"{path.resolve()}|missing"
    return f"{path.resolve()}|{stat.st_mtime_ns}|{stat.st_size}"


def robot_model_authority_metadata(state: ComposerState) -> dict[str, Any]:
    """Return canonical Robot Model Authority metadata for the active runtime.

    The C++/RoboPlan import path may consume a private joint-renamed view, but
    Composer-facing structure must remain canonical. Runtime factories attach
    this metadata to the kernel client; the URDF route only republishes it.
    """

    candidates: list[Any] = [getattr(state, "kernel", None), state.kernel_bundle]
    bundle = state.kernel_bundle
    native_bundle = getattr(bundle, "native_bundle", None) if bundle is not None else None
    if native_bundle is not None:
        candidates.append(native_bundle)
    for candidate in candidates:
        metadata = getattr(candidate, "robot_model_authority_metadata", None)
        if isinstance(metadata, Mapping):
            return dict(metadata)
    return {}


def _authority_fingerprint(metadata: Mapping[str, Any] | None) -> str | None:
    if not metadata:
        return None
    value = metadata.get("canonical_fingerprint")
    if isinstance(value, str) and value:
        return value
    return None


def _authority_manifest(metadata: Mapping[str, Any] | None) -> dict[str, Any] | None:
    if not metadata:
        return None

    def string_map_list(raw: Any) -> dict[str, list[str]]:
        if not isinstance(raw, Mapping):
            return {}
        out: dict[str, list[str]] = {}
        for key, value in raw.items():
            if isinstance(value, (list, tuple)):
                out[str(key)] = [str(item) for item in value]
        return out

    raw_grippers = metadata.get("grippers")
    grippers: dict[str, dict[str, Any]] = {}
    if isinstance(raw_grippers, Mapping):
        for gripper_id, raw_profile in raw_grippers.items():
            if not isinstance(raw_profile, Mapping):
                continue
            derived = raw_profile.get("derived_joints", [])
            presets = raw_profile.get("presets", {})
            grippers[str(gripper_id)] = {
                "group_id": str(raw_profile.get("group_id", "") or ""),
                "command_joint": str(raw_profile.get("command_joint", "") or ""),
                "derived_joints": (
                    [str(item) for item in derived]
                    if isinstance(derived, (list, tuple))
                    else []
                ),
                "presets": dict(presets) if isinstance(presets, Mapping) else {},
            }

    return {
        "canonical_fingerprint": str(metadata.get("canonical_fingerprint", "") or ""),
        "view_fingerprint": str(metadata.get("view_fingerprint", "") or ""),
        "view_changed": bool(metadata.get("view_changed", False)),
        "commandable_joint_names": [
            str(item)
            for item in (metadata.get("commandable_joint_names", []) or [])
            if str(item)
        ],
        "group_commandable_joint_names": string_map_list(
            metadata.get("group_commandable_joint_names")
        ),
        "grippers": grippers,
    }


def _entry_model_key(
    entry: _UrdfCacheEntry,
    robot_id: str | None,
    *,
    authority_fingerprint: str | None = None,
) -> str:
    """Stable public key for the active robot visual model content."""
    digest = hashlib.sha256()
    digest.update(_VISUAL_ASSET_SCHEMA_VERSION.encode("utf-8"))
    digest.update(b"\0")
    digest.update((robot_id or "").encode("utf-8"))
    digest.update(b"\0")
    digest.update((authority_fingerprint or "").encode("utf-8"))
    digest.update(b"\0")
    digest.update(str(Path(entry.urdf_path).resolve()).encode("utf-8", errors="replace"))
    digest.update(b"\0")
    digest.update(str(entry.mtime_ns).encode("ascii"))
    digest.update(b"\0")
    for package_path in entry.package_paths:
        digest.update(str(Path(package_path).resolve()).encode("utf-8", errors="replace"))
        digest.update(b"\0")
    for link in entry.links:
        digest.update(link.name.encode("utf-8", errors="replace"))
        digest.update(b"\0")
        for visual in link.visuals:
            digest.update(str(visual.visual_index).encode("ascii"))
            digest.update(b"\0")
            digest.update((visual.mesh_resource_uri or "").encode("utf-8", errors="replace"))
            digest.update(b"\0")
            digest.update(_mesh_stat_key(visual.mesh_path).encode("utf-8", errors="replace"))
            digest.update(b"\0")
        for collision in link.collisions:
            digest.update(str(collision.collision_index).encode("ascii"))
            digest.update(b"\0")
            digest.update((collision.mesh_resource_uri or "").encode("utf-8", errors="replace"))
            digest.update(b"\0")
            digest.update(_mesh_stat_key(collision.mesh_path).encode("utf-8", errors="replace"))
            digest.update(b"\0")
    return digest.hexdigest()[:24]


def active_urdf_model_key(state: ComposerState) -> str | None:
    """Return the active URDF visual model key for scene-state clients."""
    entry = get_urdf_entry(state)
    if entry is None:
        return None
    deployment = state.deployment
    robot_id = None
    if deployment is not None and deployment.robot_config is not None:
        robot_id = deployment.robot_config.robot_id
    metadata = robot_model_authority_metadata(state)
    return _entry_model_key(
        entry,
        robot_id,
        authority_fingerprint=_authority_fingerprint(metadata),
    )


@dataclass(frozen=True)
class GripperSubtreeLink:
    """One link in the gripper subtree, posed relative to
    `gripper_root_link` at the URDF default joint configuration
    (open pose for SRDF group_state defaults; articulated mimic
    chains follow their master at the default joint value)."""

    name: str
    position_m: tuple[float, float, float]
    orientation_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True)
class GripperVisualizationResolution:
    """Resolved root link + tcp→root SE(3) + full subtree for the
    Move-To unreachable ghost.

    `gripper_root_link` is the first link reachable downstream of the
    deployment's `mount_frame` (in the URDF joint tree) that has at
    least one visual block. The frontend renders the FULL gripper
    subtree in red at the operator's commanded TCP pose, offset by
    `tcp_to_root_*` and then each link's `position_m`/
    `orientation_xyzw` (relative to root). Showing only the root link
    isn't legible to operators — Robotiq's base is just a cube; the
    fingers and knuckles are what make the gripper recognizable as
    such.

    `tcp_to_root_position_m` and `tcp_to_root_orientation_xyzw`
    describe the SE(3) transform from `tcp_frame` to
    `gripper_root_link` at zero-joint-state. For typical gripper
    attachments (fixed-joint chain from `tool0` to
    `robotiq_base_link`) the transform is configuration-invariant.

    `subtree_links` is the gripper_root_link + all its descendants
    reachable via the URDF joint graph, each carrying its pose
    relative to gripper_root_link. The root itself appears in the
    list with identity transform so the frontend can render uniformly.
    """

    gripper_root_link: str
    tcp_to_root_position_m: tuple[float, float, float]
    tcp_to_root_orientation_xyzw: tuple[float, float, float, float]
    subtree_links: tuple[GripperSubtreeLink, ...]


def _has_visuals(entry: _UrdfCacheEntry, link_name: str) -> bool:
    for link in entry.links:
        if link.name == link_name:
            return len(link.visuals) > 0
    return False


def _walk_gripper_root(
    entry: _UrdfCacheEntry,
    mount_frame: str,
    srdf_gripper_joint_names: tuple[str, ...] = (),
) -> str | None:
    """Walk the URDF joint tree forward from `mount_frame` to the
    first descendant link that has at least one visual.

    Tiebreak when `mount_frame` has multiple downstream children: if
    `srdf_gripper_joint_names` is non-empty, prefer the child whose
    subtree contains the most joints from that list (typically the
    gripper's SRDF group joints — `finger_joint` for Robotiq).
    Otherwise fall back to the first child link reached in BFS order.
    Returns None if no descendant has visuals (the floor-fallback
    case where the unreachable ghost shows a red marker only).
    """
    if not mount_frame:
        return None
    # Adjacency: parent_link -> list of (child_link, joint_name)
    children_of: dict[str, list[tuple[str, str]]] = {}
    for joint in entry.joints:
        children_of.setdefault(joint.parent_link, []).append(
            (joint.child_link, joint.name)
        )
    if mount_frame not in children_of:
        return None
    srdf_set = set(srdf_gripper_joint_names)

    def subtree_score(link: str) -> tuple[int, int]:
        # (number of SRDF gripper joints in subtree, total descendants).
        # First element drives the tiebreak; second breaks ties by size.
        n_srdf, n_total = 0, 0
        seen = {link}
        stack = [link]
        while stack:
            node = stack.pop()
            for child, joint_name in children_of.get(node, ()):
                if child in seen:
                    continue
                seen.add(child)
                n_total += 1
                if joint_name in srdf_set:
                    n_srdf += 1
                stack.append(child)
        return (n_srdf, n_total)

    # Pick the best child of mount_frame, then BFS down to first
    # link with visuals.
    candidates = children_of[mount_frame]
    if len(candidates) > 1:
        candidates = sorted(candidates, key=lambda c: subtree_score(c[0]), reverse=True)
    for child_link, _joint_name in candidates:
        # BFS the subtree of this candidate to find first link with
        # visuals.
        from collections import deque

        queue: deque[str] = deque([child_link])
        seen: set[str] = {child_link}
        while queue:
            node = queue.popleft()
            if _has_visuals(entry, node):
                return node
            for grandchild, _jn in children_of.get(node, ()):
                if grandchild not in seen:
                    seen.add(grandchild)
                    queue.append(grandchild)
    return None


def _se3_transform_via_urdf(
    entry: _UrdfCacheEntry, source_frame: str, target_frame: str
) -> tuple[tuple[float, float, float], tuple[float, float, float, float]] | None:
    """Return SE(3) (xyz, xyzw) from `source_frame` to `target_frame`
    at the URDF's default joint configuration, or None if either
    frame is not in the URDF or the transform is undefined.
    """
    model = entry.urdf_model
    if model is None:
        return None
    link_map = getattr(model, "link_map", {}) or {}
    if source_frame not in link_map or target_frame not in link_map:
        return None
    try:
        # yourdfpy.URDF.get_transform(target, source) returns the 4x4
        # homogeneous transform from source frame to target frame at
        # the model's default configuration (zero joints unless the
        # SRDF/URDF provides a non-default cfg). For fixed-joint
        # chains (typical gripper attachments) this is
        # configuration-invariant.
        T = model.get_transform(frame_to=target_frame, frame_from=source_frame)
    except Exception:
        logger.debug(
            "yourdfpy get_transform(%s -> %s) failed",
            source_frame,
            target_frame,
            exc_info=True,
        )
        return None
    if T is None:
        return None
    try:
        import math

        xyz = (float(T[0, 3]), float(T[1, 3]), float(T[2, 3]))
        # Shepperd's quaternion extraction — same routine used by
        # _origin_decompose for visual_origin. Kept inline to avoid
        # depending on numpy here.
        r00 = float(T[0, 0]); r01 = float(T[0, 1]); r02 = float(T[0, 2])
        r10 = float(T[1, 0]); r11 = float(T[1, 1]); r12 = float(T[1, 2])
        r20 = float(T[2, 0]); r21 = float(T[2, 1]); r22 = float(T[2, 2])
        trace = r00 + r11 + r22
        if trace > 0.0:
            s = math.sqrt(trace + 1.0) * 2.0
            qw = 0.25 * s
            qx = (r21 - r12) / s
            qy = (r02 - r20) / s
            qz = (r10 - r01) / s
        elif r00 > r11 and r00 > r22:
            s = math.sqrt(1.0 + r00 - r11 - r22) * 2.0
            qw = (r21 - r12) / s
            qx = 0.25 * s
            qy = (r01 + r10) / s
            qz = (r02 + r20) / s
        elif r11 > r22:
            s = math.sqrt(1.0 + r11 - r00 - r22) * 2.0
            qw = (r02 - r20) / s
            qx = (r01 + r10) / s
            qy = 0.25 * s
            qz = (r12 + r21) / s
        else:
            s = math.sqrt(1.0 + r22 - r00 - r11) * 2.0
            qw = (r10 - r01) / s
            qx = (r02 + r20) / s
            qy = (r12 + r21) / s
            qz = 0.25 * s
        norm = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
        if norm > 1e-12:
            qx /= norm; qy /= norm; qz /= norm; qw /= norm
        else:
            qx, qy, qz, qw = 0.0, 0.0, 0.0, 1.0
        return xyz, (qx, qy, qz, qw)
    except Exception:
        logger.debug(
            "tcp→root quaternion extraction failed for %s -> %s",
            source_frame,
            target_frame,
            exc_info=True,
        )
        return None


def _enumerate_gripper_subtree(
    entry: _UrdfCacheEntry, gripper_root: str
) -> tuple[str, ...]:
    """Return gripper_root + all descendants reachable via the URDF
    joint graph, in BFS order. Includes structural links (no visuals)
    so the caller can decide whether to render or skip — the
    backend's job is to enumerate, the frontend's job is to look up
    visuals from the manifest."""
    children_of: dict[str, list[str]] = {}
    for joint in entry.joints:
        children_of.setdefault(joint.parent_link, []).append(joint.child_link)
    from collections import deque

    out: list[str] = [gripper_root]
    seen: set[str] = {gripper_root}
    queue: deque[str] = deque([gripper_root])
    while queue:
        node = queue.popleft()
        for child in children_of.get(node, ()):
            if child in seen:
                continue
            seen.add(child)
            out.append(child)
            queue.append(child)
    return tuple(out)


def _collect_subtree_relative_poses(
    entry: _UrdfCacheEntry, gripper_root: str, subtree: tuple[str, ...]
) -> tuple[GripperSubtreeLink, ...]:
    """For each link in `subtree`, compute its pose relative to
    `gripper_root` at the URDF default configuration. Links whose
    transform can't be computed are dropped (industrial fail-closed —
    one bad link doesn't take down the whole ghost)."""
    result: list[GripperSubtreeLink] = []
    for link_name in subtree:
        if link_name == gripper_root:
            # Identity — root relative to itself.
            result.append(
                GripperSubtreeLink(
                    name=link_name,
                    position_m=(0.0, 0.0, 0.0),
                    orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
                )
            )
            continue
        transform = _se3_transform_via_urdf(entry, gripper_root, link_name)
        if transform is None:
            logger.debug(
                "subtree FK miss for %s relative to %s",
                link_name,
                gripper_root,
            )
            continue
        position, orientation = transform
        result.append(
            GripperSubtreeLink(
                name=link_name,
                position_m=position,
                orientation_xyzw=orientation,
            )
        )
    return tuple(result)


def resolve_gripper_visualization(
    state: ComposerState,
    mount_frame: str,
    tcp_frame: str,
    *,
    srdf_gripper_joint_names: tuple[str, ...] = (),
) -> GripperVisualizationResolution | None:
    """Resolve the gripper visualization root + subtree for the
    Move-To unreachable ghost.

    Industrial-safety properties:
    - Side-effect-free (read-only URDF + FK).
    - Always returns None on uncertainty (caller falls back to a
      red-axes marker that ALWAYS renders).
    - Deterministic tiebreak when `mount_frame` has multiple
      downstream children: prefer the subtree containing the most
      SRDF gripper-group joints (when provided), else BFS-first.
    - Configuration-invariant for the typical fixed-joint
      attachment chain.
    - Subtree enumeration is BFS deterministic; one bad descendant
      FK does not invalidate the whole resolution (drop-on-error).
    """
    entry = get_urdf_entry(state)
    if entry is None:
        return None
    mount = (mount_frame or "").strip()
    tcp = (tcp_frame or "").strip()
    if not mount or not tcp:
        return None
    gripper_root = _walk_gripper_root(entry, mount, srdf_gripper_joint_names)
    if gripper_root is None:
        return None
    transform = _se3_transform_via_urdf(entry, tcp, gripper_root)
    if transform is None:
        return None
    position, orientation = transform
    subtree_names = _enumerate_gripper_subtree(entry, gripper_root)
    subtree_links = _collect_subtree_relative_poses(entry, gripper_root, subtree_names)
    return GripperVisualizationResolution(
        gripper_root_link=gripper_root,
        tcp_to_root_position_m=position,
        tcp_to_root_orientation_xyzw=orientation,
        subtree_links=subtree_links,
    )


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


def _find_collision(
    entry: _UrdfCacheEntry,
    link_name: str,
    collision_index: int,
) -> _LinkCollision | None:
    for link in entry.links:
        if link.name != link_name:
            continue
        for collision in link.collisions:
            if collision.collision_index == collision_index:
                return collision
        return None
    return None


def _render_mesh_glb(
    entry: _UrdfCacheEntry,
    *,
    kind: str,
    link_name: str,
    index: int,
    mesh_path: str | None,
    mesh_scale: tuple[float, float, float],
) -> bytes | None:
    if mesh_path is None:
        return None
    mesh_file = Path(mesh_path)
    if not mesh_file.exists():
        logger.warning(
            "Mesh file missing for link %s %s %d at %s",
            link_name,
            kind,
            index,
            mesh_file,
        )
        return None
    cache_key = (
        kind,
        link_name,
        index,
        _VISUAL_ASSET_SCHEMA_VERSION,
        _mesh_stat_key(mesh_path),
    )
    with _cache_lock:
        cached = entry.mesh_glb_cache.get(cache_key)
    if cached is not None:
        return cached
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
    sx, sy, sz = mesh_scale
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
    with _cache_lock:
        entry.mesh_glb_cache[cache_key] = glb_bytes
        while len(entry.mesh_glb_cache) > _MAX_GLB_CACHE_ENTRIES_PER_MODEL:
            entry.mesh_glb_cache.pop(next(iter(entry.mesh_glb_cache)))
    return glb_bytes


def _render_visual_glb(
    entry: _UrdfCacheEntry,
    visual: _LinkVisual,
) -> bytes | None:
    # Primitive-only visuals are rendered client-side via Three.js native
    # geometries — no glTF needed. The mesh route returns 404 for these
    # and the frontend uses the manifest's `primitive` block instead.
    return _render_mesh_glb(
        entry,
        kind="visual",
        link_name=visual.link_name,
        index=visual.visual_index,
        mesh_path=visual.mesh_path,
        mesh_scale=visual.mesh_scale,
    )


def _render_collision_glb(
    entry: _UrdfCacheEntry,
    collision: _LinkCollision,
) -> bytes | None:
    return _render_mesh_glb(
        entry,
        kind="collision",
        link_name=collision.link_name,
        index=collision.collision_index,
        mesh_path=collision.mesh_path,
        mesh_scale=collision.mesh_scale,
    )


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
        # Drei's GLB cache is keyed by URL string, while industrial robot
        # models often reuse link names across arms. Key mesh URLs by the
        # resolved URDF content identity, not just robot_id, so switching
        # UR5 <-> UR10e or editing a same-id deployment cannot reuse stale
        # link geometry at new FK poses.
        authority_metadata = robot_model_authority_metadata(state)
        authority = _authority_manifest(authority_metadata)
        model_key = _entry_model_key(
            entry,
            robot_id,
            authority_fingerprint=_authority_fingerprint(authority_metadata),
        )
        return {
            "robot_id": robot_id,
            "model_key": model_key,
            "authority": authority,
            "root_link": entry.root_link,
            "links": [
                {
                    "name": link.name,
                    "visuals": [
                        {
                            "index": visual.visual_index,
                            "mesh_url": (
                                f"/api/scene/urdf/models/{model_key}/meshes/"
                                f"{quote(link.name, safe='')}/{visual.visual_index}"
                                if visual.mesh_path is not None
                                else None
                            ),
                            "primitive": visual.primitive,
                            "visual_origin": {
                                "position_m": list(visual.visual_origin_xyz),
                                "orientation_rpy_rad": list(visual.visual_origin_rpy),
                                "orientation_xyzw": list(visual.visual_origin_xyzw),
                            },
                        }
                        for visual in link.visuals
                    ],
                    "collisions": [
                        {
                            "index": collision.collision_index,
                            "mesh_url": (
                                f"/api/scene/urdf/models/{model_key}/collision-meshes/"
                                f"{quote(link.name, safe='')}/{collision.collision_index}"
                                if collision.mesh_path is not None
                                else None
                            ),
                            "primitive": collision.primitive,
                            "collision_origin": {
                                "position_m": list(collision.collision_origin_xyz),
                                "orientation_rpy_rad": list(collision.collision_origin_rpy),
                                "orientation_xyzw": list(collision.collision_origin_xyzw),
                            },
                        }
                        for collision in link.collisions
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
                    "mimic": (
                        {
                            "master": joint.mimic[0],
                            "multiplier": joint.mimic[1],
                            "offset": joint.mimic[2],
                        }
                        if joint.mimic is not None
                        else None
                    ),
                    "is_mimic_master": joint.is_mimic_master,
                }
                for joint in entry.joints
            ],
        }

    def _get_mesh_for_model(
        link_name: str,
        visual_index: int,
        requested_model_key: str | None,
    ) -> Response:
        entry = get_urdf_entry(state)
        if entry is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "No URDF available for the active deployment"},
            )
        deployment = state.deployment
        robot_id = None
        if deployment is not None and deployment.robot_config is not None:
            robot_id = deployment.robot_config.robot_id
        authority_metadata = robot_model_authority_metadata(state)
        active_model_key = _entry_model_key(
            entry,
            robot_id,
            authority_fingerprint=_authority_fingerprint(authority_metadata),
        )
        if requested_model_key is not None and requested_model_key != active_model_key:
            return JSONResponse(
                status_code=409,
                content={
                    "detail": "Stale URDF mesh URL for inactive robot model",
                    "active_model_key": active_model_key,
                },
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
                "Cache-Control": (
                    "public, max-age=31536000, immutable"
                    if requested_model_key is not None
                    else "no-store"
                ),
                "ETag": f'"{active_model_key}-{link_name}-{visual_index}"',
            },
        )

    def _get_collision_mesh_for_model(
        link_name: str,
        collision_index: int,
        requested_model_key: str | None,
    ) -> Response:
        entry = get_urdf_entry(state)
        if entry is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "No URDF available for the active deployment"},
            )
        deployment = state.deployment
        robot_id = None
        if deployment is not None and deployment.robot_config is not None:
            robot_id = deployment.robot_config.robot_id
        authority_metadata = robot_model_authority_metadata(state)
        active_model_key = _entry_model_key(
            entry,
            robot_id,
            authority_fingerprint=_authority_fingerprint(authority_metadata),
        )
        if requested_model_key is not None and requested_model_key != active_model_key:
            return JSONResponse(
                status_code=409,
                content={
                    "detail": "Stale URDF collision mesh URL for inactive robot model",
                    "active_model_key": active_model_key,
                },
            )
        collision = _find_collision(entry, link_name, collision_index)
        if collision is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": f"Unknown link/collision '{link_name}'/{collision_index}",
                },
            )
        glb_bytes = _render_collision_glb(entry, collision)
        if glb_bytes is None:
            return JSONResponse(
                status_code=404,
                content={
                    "detail": (
                        f"No renderable collision mesh for link '{link_name}' collision "
                        f"{collision_index}"
                    ),
                },
            )
        return Response(
            content=glb_bytes,
            media_type="model/gltf-binary",
            headers={
                "Cache-Control": (
                    "public, max-age=31536000, immutable"
                    if requested_model_key is not None
                    else "no-store"
                ),
                "ETag": f'"{active_model_key}-{link_name}-collision-{collision_index}"',
            },
        )

    @router.get("/models/{model_key}/meshes/{link_name}/{visual_index}")
    def get_model_mesh(model_key: str, link_name: str, visual_index: int) -> Response:
        return _get_mesh_for_model(
            link_name=link_name,
            visual_index=visual_index,
            requested_model_key=model_key,
        )

    @router.get("/models/{model_key}/collision-meshes/{link_name}/{collision_index}")
    def get_model_collision_mesh(
        model_key: str, link_name: str, collision_index: int
    ) -> Response:
        return _get_collision_mesh_for_model(
            link_name=link_name,
            collision_index=collision_index,
            requested_model_key=model_key,
        )

    @router.get("/meshes/{link_name}/{visual_index}")
    def get_mesh(link_name: str, visual_index: int, model: str | None = None) -> Response:
        # Legacy route kept for older bundles during a rolling local reload.
        # New manifests always use the model-addressed route above.
        return _get_mesh_for_model(
            link_name=link_name,
            visual_index=visual_index,
            requested_model_key=model,
        )

    @router.get("/collision-meshes/{link_name}/{collision_index}")
    def get_collision_mesh(
        link_name: str, collision_index: int, model: str | None = None
    ) -> Response:
        return _get_collision_mesh_for_model(
            link_name=link_name,
            collision_index=collision_index,
            requested_model_key=model,
        )

    return router
