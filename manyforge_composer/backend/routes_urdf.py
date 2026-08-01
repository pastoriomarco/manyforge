"""Authored-deployment structural URDF routes.

The cutover shell serves a read-only JSON manifest and glTF/GLB assets from
the authored deployment. It publishes no live poses, commandability,
runtime-authority metadata, or semantic-scene state.

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
import math
import threading
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

_Matrix4 = tuple[
    tuple[float, float, float, float],
    tuple[float, float, float, float],
    tuple[float, float, float, float],
    tuple[float, float, float, float],
]
_IDENTITY_MATRIX: _Matrix4 = (
    (1.0, 0.0, 0.0, 0.0),
    (0.0, 1.0, 0.0, 0.0),
    (0.0, 0.0, 1.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
)


def _coerce_origin_matrix(origin: Any) -> _Matrix4 | None:
    """Return one finite rigid transform, or ``None`` on uncertainty."""
    if origin is None:
        return _IDENTITY_MATRIX
    try:
        matrix: _Matrix4 = tuple(  # type: ignore[assignment]
            tuple(float(origin[row, column]) for column in range(4))
            for row in range(4)
        )
    except (IndexError, KeyError, TypeError, ValueError):
        try:
            matrix = tuple(  # type: ignore[assignment]
                tuple(float(origin[row][column]) for column in range(4))
                for row in range(4)
            )
        except (IndexError, KeyError, TypeError, ValueError):
            return None
    if not all(math.isfinite(value) for row in matrix for value in row):
        return None
    if any(abs(matrix[3][column]) > 1e-8 for column in range(3)):
        return None
    if abs(matrix[3][3] - 1.0) > 1e-8:
        return None

    rotation = tuple(tuple(matrix[row][column] for column in range(3)) for row in range(3))
    for row in rotation:
        if abs(sum(value * value for value in row) - 1.0) > 1e-5:
            return None
    for left, right in ((0, 1), (0, 2), (1, 2)):
        if abs(sum(rotation[left][i] * rotation[right][i] for i in range(3))) > 1e-5:
            return None
    determinant = (
        rotation[0][0]
        * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1])
        - rotation[0][1]
        * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0])
        + rotation[0][2]
        * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0])
    )
    if abs(determinant - 1.0) > 1e-5:
        return None
    return matrix


def _multiply_transforms(left: _Matrix4, right: _Matrix4) -> _Matrix4:
    return tuple(  # type: ignore[return-value]
        tuple(
            sum(left[row][inner] * right[inner][column] for inner in range(4))
            for column in range(4)
        )
        for row in range(4)
    )


def _joint_motion_transform(
    joint_type: str,
    axis: tuple[float, float, float],
    position: float,
) -> _Matrix4 | None:
    """Return the URDF joint motion transform at one scalar position."""
    if not math.isfinite(position):
        return None
    if joint_type in {"fixed", "floating", "planar"}:
        return _IDENTITY_MATRIX
    if joint_type not in {"revolute", "continuous", "prismatic"}:
        return None
    axis_norm = math.sqrt(sum(value * value for value in axis))
    if not math.isfinite(axis_norm) or axis_norm <= 1e-12:
        return None
    x, y, z = (value / axis_norm for value in axis)
    if joint_type == "prismatic":
        return (
            (1.0, 0.0, 0.0, x * position),
            (0.0, 1.0, 0.0, y * position),
            (0.0, 0.0, 1.0, z * position),
            (0.0, 0.0, 0.0, 1.0),
        )

    cosine = math.cos(position)
    sine = math.sin(position)
    complement = 1.0 - cosine
    return (
        (
            cosine + x * x * complement,
            x * y * complement - z * sine,
            x * z * complement + y * sine,
            0.0,
        ),
        (
            y * x * complement + z * sine,
            cosine + y * y * complement,
            y * z * complement - x * sine,
            0.0,
        ),
        (
            z * x * complement - y * sine,
            z * y * complement + x * sine,
            cosine + z * z * complement,
            0.0,
        ),
        (0.0, 0.0, 0.0, 1.0),
    )


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
    semantics. Collision elements remain authored structural metadata only;
    this route makes no planning or collision-validation claim.
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
    neutral_position_m: tuple[float, float, float]
    neutral_orientation_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True)
class _JointEntry:
    """Read-only joint structure declared by the authored URDF."""

    name: str
    type: str
    axis: tuple[float, float, float]
    parent_link: str
    child_link: str
    limit_lower: float | None
    limit_upper: float | None
    effort_limit: float | None
    velocity_limit: float | None
    origin_position_m: tuple[float, float, float]
    origin_orientation_rpy_rad: tuple[float, float, float]
    origin_orientation_xyzw: tuple[float, float, float, float]
    coordinate_unit: str | None
    neutral_position: float | None
    mimic: tuple[str, float, float] | None = None
    is_mimic_master: bool = False


@dataclass
class _UrdfCacheEntry:
    urdf_path: str
    package_paths: tuple[str, ...]
    mtime_ns: int
    root_link: str
    links: tuple[_LinkEntry, ...]
    joints: tuple[_JointEntry, ...]
    mesh_glb_cache: dict[tuple[str, str, int, str, str], bytes]


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
    if deployment is None:
        return None
    if not state.deployment_source_path:
        return None
    authored_model = deployment.model
    if not authored_model.urdf_path:
        return None
    extra_package_paths = (
        [] if authored_model.package_paths else default_package_paths()
    )
    try:
        resolved = resolve_robot_model(
            urdf_path=authored_model.urdf_path,
            srdf_path=authored_model.srdf_path,
            package_paths=list(authored_model.package_paths),
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
        origin_matrix = _coerce_origin_matrix(origin)
        if origin_matrix is None:
            return zero_xyz, zero_rpy, identity_quat
        origin = origin_matrix
        try:
            xyz = (
                float(origin[0][3]),
                float(origin[1][3]),
                float(origin[2][3]),
            )
            r00 = float(origin[0][0])
            r01 = float(origin[0][1])
            r02 = float(origin[0][2])
            r10 = float(origin[1][0])
            r11 = float(origin[1][1])
            r12 = float(origin[1][2])
            r20 = float(origin[2][0])
            r21 = float(origin[2][1])
            r22 = float(origin[2][2])
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
        origin_matrix = _coerce_origin_matrix(origin)
        if origin_matrix is None:
            raise ValueError("visual origin is not a rigid transform")
        origin_xyz, origin_rpy, origin_xyzw = _origin_decompose(origin_matrix)

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
        origin_matrix = _coerce_origin_matrix(origin)
        if origin_matrix is None:
            raise ValueError("collision origin is not a rigid transform")
        origin_xyz, origin_rpy, origin_xyzw = _origin_decompose(origin_matrix)

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

    link_names = tuple(str(link_name) for link_name in link_map)
    if (
        not link_names
        or len(set(link_names)) != len(link_names)
        or not root_link
        or root_link not in set(link_names)
    ):
        logger.warning("URDF %s has no unambiguous authored root tree", urdf_path)
        return None

    geometry_by_link: dict[
        str,
        tuple[tuple[_LinkVisual, ...], tuple[_LinkCollision, ...]],
    ] = {}
    try:
        for raw_link_name, link in link_map.items():
            link_name = str(raw_link_name)
            link_visuals = tuple(
                built
                for index, visual in enumerate(getattr(link, "visuals", None) or [])
                if (built := _build_visual(link_name, index, visual)) is not None
            )
            link_collisions = tuple(
                built
                for index, collision in enumerate(
                    getattr(link, "collisions", None) or []
                )
                if (built := _build_collision(link_name, index, collision)) is not None
            )
            geometry_by_link[link_name] = (link_visuals, link_collisions)
    except (IndexError, TypeError, ValueError):
        logger.warning("URDF %s has invalid authored geometry metadata", urdf_path)
        return None

    # Preserve authored joint structure and derive one deterministic neutral
    # projection without consulting observed state. Independent scalar joints
    # are set to zero; authored mimic chains then apply their multiplier and
    # offset. This is a structural rendering pose, not an admissibility claim.
    joint_map = getattr(urdf_model, "joint_map", {}) or {}
    valid_joint_types = {
        "continuous",
        "fixed",
        "floating",
        "planar",
        "prismatic",
        "revolute",
    }
    scalar_joint_types = {"continuous", "prismatic", "revolute"}

    def _extract_mimic(joint: Any) -> tuple[str, float, float] | None:
        mimic = getattr(joint, "mimic", None)
        if mimic is None:
            return None
        master = getattr(mimic, "joint", None)
        if not isinstance(master, str) or not master:
            raise ValueError("mimic joint has no master")
        multiplier_raw = getattr(mimic, "multiplier", 1.0)
        offset_raw = getattr(mimic, "offset", 0.0)
        multiplier = float(multiplier_raw) if multiplier_raw is not None else 1.0
        offset = float(offset_raw) if offset_raw is not None else 0.0
        if not math.isfinite(multiplier) or not math.isfinite(offset):
            raise ValueError("mimic law is non-finite")
        return (master, multiplier, offset)

    def _optional_finite_float(value: Any) -> float | None:
        if value is None:
            return None
        converted = float(value)
        if not math.isfinite(converted):
            raise ValueError("joint limit is non-finite")
        return converted

    joint_rows: list[dict[str, Any]] = []
    joint_by_name: dict[str, dict[str, Any]] = {}
    try:
        for raw_joint_name, joint in joint_map.items():
            name = str(raw_joint_name)
            if not name or name in joint_by_name:
                raise ValueError("duplicate or empty joint name")
            joint_type = str(getattr(joint, "type", "fixed") or "fixed").lower()
            if joint_type not in valid_joint_types:
                raise ValueError(f"unsupported joint type {joint_type}")
            axis_value = getattr(joint, "axis", None)
            if axis_value is not None:
                if len(axis_value) != 3:
                    raise ValueError("joint axis is not a three-vector")
                axis = tuple(float(axis_value[index]) for index in range(3))
            else:
                axis = (1.0, 0.0, 0.0)
            if not all(math.isfinite(value) for value in axis):
                raise ValueError("joint axis is non-finite")
            parent_link = str(getattr(joint, "parent", "") or "")
            child_link = str(getattr(joint, "child", "") or "")
            origin_matrix = _coerce_origin_matrix(getattr(joint, "origin", None))
            if origin_matrix is None:
                raise ValueError("joint origin is not a rigid transform")
            limit = getattr(joint, "limit", None)
            row = {
                "name": name,
                "type": joint_type,
                "axis": axis,
                "parent_link": parent_link,
                "child_link": child_link,
                "origin_matrix": origin_matrix,
                "limit_lower": _optional_finite_float(
                    getattr(limit, "lower", None) if limit is not None else None
                ),
                "limit_upper": _optional_finite_float(
                    getattr(limit, "upper", None) if limit is not None else None
                ),
                "effort_limit": _optional_finite_float(
                    getattr(limit, "effort", None) if limit is not None else None
                ),
                "velocity_limit": _optional_finite_float(
                    getattr(limit, "velocity", None) if limit is not None else None
                ),
                "mimic": _extract_mimic(joint),
                "coordinate_unit": (
                    "m"
                    if joint_type == "prismatic"
                    else "rad"
                    if joint_type in {"continuous", "revolute"}
                    else None
                ),
            }
            joint_rows.append(row)
            joint_by_name[name] = row
    except (IndexError, KeyError, TypeError, ValueError):
        logger.warning("URDF %s has invalid authored joint metadata", urdf_path)
        return None

    neutral_positions: dict[str, float] = {}

    def _neutral_position(joint_name: str, pending: set[str]) -> float:
        cached_position = neutral_positions.get(joint_name)
        if cached_position is not None:
            return cached_position
        if joint_name in pending:
            raise ValueError("cyclic mimic graph")
        row = joint_by_name[joint_name]
        if row["type"] not in scalar_joint_types:
            if row["mimic"] is not None:
                raise ValueError("non-scalar joint has mimic metadata")
            neutral_positions[joint_name] = 0.0
            return 0.0
        mimic = row["mimic"]
        if mimic is None:
            neutral_positions[joint_name] = 0.0
            return 0.0
        master, multiplier, offset = mimic
        master_row = joint_by_name.get(master)
        if (
            master_row is None
            or master_row["type"] not in scalar_joint_types
            or master_row["coordinate_unit"] != row["coordinate_unit"]
        ):
            raise ValueError("mimic master is missing or unit-incompatible")
        position = multiplier * _neutral_position(master, pending | {joint_name}) + offset
        if not math.isfinite(position):
            raise ValueError("neutral mimic position is non-finite")
        neutral_positions[joint_name] = position
        return position

    try:
        for joint_name in joint_by_name:
            _neutral_position(joint_name, set())
    except (KeyError, ValueError):
        logger.warning("URDF %s has an invalid authored mimic graph", urdf_path)
        return None

    link_name_set = set(link_names)
    children_by_parent: dict[str, list[dict[str, Any]]] = {
        link_name: [] for link_name in link_names
    }
    incoming_joint_by_child: dict[str, str] = {}
    try:
        for row in joint_rows:
            parent_link = row["parent_link"]
            child_link = row["child_link"]
            if (
                parent_link not in link_name_set
                or child_link not in link_name_set
                or parent_link == child_link
                or child_link == root_link
                or child_link in incoming_joint_by_child
            ):
                raise ValueError("joint graph is not a rooted tree")
            incoming_joint_by_child[child_link] = row["name"]
            children_by_parent[parent_link].append(row)

        link_transforms: dict[str, _Matrix4] = {root_link: _IDENTITY_MATRIX}
        queue = [root_link]
        while queue:
            parent_link = queue.pop(0)
            parent_transform = link_transforms[parent_link]
            for row in children_by_parent[parent_link]:
                child_link = row["child_link"]
                if child_link in link_transforms:
                    raise ValueError("joint graph contains a cycle")
                motion = _joint_motion_transform(
                    row["type"],
                    row["axis"],
                    neutral_positions[row["name"]],
                )
                if motion is None:
                    raise ValueError("joint motion transform is undefined")
                child_transform = _multiply_transforms(
                    parent_transform,
                    _multiply_transforms(row["origin_matrix"], motion),
                )
                if _coerce_origin_matrix(child_transform) is None:
                    raise ValueError("derived link transform is not rigid")
                link_transforms[child_link] = child_transform
                queue.append(child_link)
        if set(link_transforms) != link_name_set:
            raise ValueError("joint graph is disconnected")
    except (KeyError, ValueError):
        logger.warning("URDF %s does not define one complete rooted link tree", urdf_path)
        return None

    masters = {
        mimic[0]
        for row in joint_rows
        if (mimic := row["mimic"]) is not None
    }
    joints: list[_JointEntry] = []
    for row in joint_rows:
        origin_position, origin_rpy, origin_xyzw = _origin_decompose(
            row["origin_matrix"]
        )
        joints.append(
            _JointEntry(
                name=row["name"],
                type=row["type"],
                axis=row["axis"],
                parent_link=row["parent_link"],
                child_link=row["child_link"],
                limit_lower=row["limit_lower"],
                limit_upper=row["limit_upper"],
                effort_limit=row["effort_limit"],
                velocity_limit=row["velocity_limit"],
                origin_position_m=origin_position,
                origin_orientation_rpy_rad=origin_rpy,
                origin_orientation_xyzw=origin_xyzw,
                coordinate_unit=row["coordinate_unit"],
                neutral_position=(
                    neutral_positions[row["name"]]
                    if row["type"] in scalar_joint_types
                    else None
                ),
                mimic=row["mimic"],
                is_mimic_master=row["name"] in masters,
            )
        )

    links: list[_LinkEntry] = []
    for link_name in link_names:
        neutral_position, _, neutral_xyzw = _origin_decompose(
            link_transforms[link_name]
        )
        visuals, collisions = geometry_by_link[link_name]
        links.append(
            _LinkEntry(
                name=link_name,
                visuals=visuals,
                collisions=collisions,
                neutral_position_m=neutral_position,
                neutral_orientation_xyzw=neutral_xyzw,
            )
        )

    entry = _UrdfCacheEntry(
        urdf_path=urdf_path,
        package_paths=package_paths,
        mtime_ns=mtime_ns,
        root_link=root_link,
        links=tuple(links),
        joints=tuple(joints),
        mesh_glb_cache={},
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




def _mesh_stat_key(mesh_path: str | None) -> str:
    if mesh_path is None:
        return "primitive"
    path = Path(mesh_path)
    try:
        stat = path.stat()
    except OSError:
        return f"{path.resolve()}|missing"
    return f"{path.resolve()}|{stat.st_mtime_ns}|{stat.st_size}"




def _entry_asset_revision_key(
    entry: _UrdfCacheEntry,
    robot_id: str | None,
) -> str:
    """Return a local stale-asset key, never a retained semantic identity.

    The structural shell uses this path/mtime-derived key only to prevent a
    browser cache from showing geometry from a previously authored file.  It
    must not be compared with compiler-owned model or bundle identities.
    """
    digest = hashlib.sha256()
    digest.update(_VISUAL_ASSET_SCHEMA_VERSION.encode("utf-8"))
    digest.update(b"\0")
    digest.update((robot_id or "").encode("utf-8"))
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
        if deployment is not None:
            robot_id = deployment.model.authored_model_id
        # Drei's GLB cache is keyed by URL string, while industrial robot
        # models often reuse link names across arms. Key mesh URLs by the
        # local authored-asset revision, not just robot_id, so switching
        # UR5 <-> UR10e or editing a same-id deployment cannot reuse stale
        # link geometry. This is deliberately not a compiler/retained identity.
        model_key = _entry_asset_revision_key(entry, robot_id)
        return {
            "robot_id": robot_id,
            "model_key": model_key,
            "root_link": entry.root_link,
            "projection": {
                "kind": "authored_neutral",
                "source": "authored_urdf",
                "root_frame": entry.root_link,
                "position_unit": "m",
                "angle_unit": "rad",
                "quaternion_order": "xyzw",
                "joint_coordinate_policy": (
                    "zero_independent_with_authored_mimic"
                ),
                "operational_validity": "not_asserted",
                "runtime_state_used": False,
            },
            "links": [
                {
                    "name": link.name,
                    "neutral_transform": {
                        "relative_to": entry.root_link,
                        "position_m": list(link.neutral_position_m),
                        "orientation_xyzw": list(
                            link.neutral_orientation_xyzw
                        ),
                    },
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
                    "origin": {
                        "relative_to": joint.parent_link,
                        "position_m": list(joint.origin_position_m),
                        "orientation_rpy_rad": list(
                            joint.origin_orientation_rpy_rad
                        ),
                        "orientation_xyzw": list(
                            joint.origin_orientation_xyzw
                        ),
                    },
                    "coordinate_unit": joint.coordinate_unit,
                    "neutral_position": joint.neutral_position,
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
        if deployment is not None:
            robot_id = deployment.model.authored_model_id
        active_model_key = _entry_asset_revision_key(entry, robot_id)
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
        if deployment is not None:
            robot_id = deployment.model.authored_model_id
        active_model_key = _entry_asset_revision_key(entry, robot_id)
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

    return router
