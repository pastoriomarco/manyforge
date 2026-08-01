"""Owner-neutral, finite three-dimensional geometry algorithms.

This module deliberately defines no pose document, frame/identity contract, or
legacy parser.  Those wire semantics belong to the future headless scene and
Program owners.  The retained functions operate only on explicit position,
quaternion, and roll/pitch/yaw components so they can be reused by thin clients
without becoming another source of runtime truth.

Convention: RPY is intrinsic ZYX (roll about X, pitch about Y, yaw about Z) in
degrees; quaternions are ``[x, y, z, w]``.
"""

from __future__ import annotations

import math
from numbers import Real
from typing import Any


# Rotation-agreement tolerance on the quaternion dot product.  Two
# orientations agree when |dot| >= 1 - tolerance; 1e-3 admits approximately
# 5.1 degrees and is intended to catch representation mistakes while allowing
# ordinary hand-authored rounding.  This is a math helper default, not a wire
# compatibility rule.
QUAT_AGREEMENT_DOT_TOL = 1e-3


class GeometryError(ValueError):
    """Raised when an algorithm input is not finite or has the wrong shape."""


def rpy_deg_to_xyzw(rpy_deg: Any) -> list[float]:
    """Convert intrinsic-ZYX RPY degrees to a unit quaternion."""
    r, p, y = (
        math.radians(float(value))
        for value in _finite_list(rpy_deg, 3, "rpy_deg")
    )
    cr, sr = math.cos(r / 2), math.sin(r / 2)
    cp, sp = math.cos(p / 2), math.sin(p / 2)
    cy, sy = math.cos(y / 2), math.sin(y / 2)
    return normalize_quat(
        [
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy,
        ]
    )


def xyzw_to_rpy_deg(orientation_xyzw: Any) -> list[float]:
    """Convert a unit quaternion to intrinsic-ZYX RPY degrees."""
    x, y, z, w = normalize_quat(orientation_xyzw)
    sinr_cosp = 2.0 * (w * x + y * z)
    cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
    roll = math.atan2(sinr_cosp, cosr_cosp)
    sinp = 2.0 * (w * y - z * x)
    pitch = (
        math.copysign(math.pi / 2.0, sinp)
        if abs(sinp) >= 1.0
        else math.asin(sinp)
    )
    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw = math.atan2(siny_cosp, cosy_cosp)
    return [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)]


def normalize_quat(q: Any) -> list[float]:
    """Return a normalized finite quaternion, rejecting the zero quaternion."""
    values = _finite_list(q, 4, "quaternion")
    scale = max(abs(component) for component in values)
    if scale == 0.0:
        raise GeometryError("quaternion must be non-zero")
    scaled = [component / scale for component in values]
    scaled_norm = math.sqrt(sum(component * component for component in scaled))
    return [component / scaled_norm for component in scaled]


def quats_close(
    a: Any,
    b: Any,
    tol: float = QUAT_AGREEMENT_DOT_TOL,
) -> bool:
    """Return whether two quaternions represent approximately the same rotation."""
    if isinstance(tol, bool) or not isinstance(tol, Real):
        raise GeometryError("tol must be a finite number strictly between 0 and 1")
    try:
        normalized_tol = float(tol)
    except (OverflowError, TypeError, ValueError) as error:
        raise GeometryError(
            "tol must be a finite number strictly between 0 and 1"
        ) from error
    if not math.isfinite(normalized_tol) or not 0.0 < normalized_tol < 1.0:
        raise GeometryError("tol must be a finite number strictly between 0 and 1")
    qa, qb = normalize_quat(a), normalize_quat(b)
    return abs(sum(x * y for x, y in zip(qa, qb))) >= 1.0 - normalized_tol


def quat_mul(a: Any, b: Any) -> list[float]:
    """Return the normalized Hamilton product ``a ⊗ b``."""
    ax, ay, az, aw = normalize_quat(a)
    bx, by, bz, bw = normalize_quat(b)
    return normalize_quat(
        [
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz,
        ]
    )


def compose_local(current_xyzw: Any, delta_xyzw: Any) -> list[float]:
    """Apply ``delta_xyzw`` in the body/local frame."""
    return quat_mul(current_xyzw, delta_xyzw)


def compose_world(current_xyzw: Any, delta_xyzw: Any) -> list[float]:
    """Apply ``delta_xyzw`` in the world frame."""
    return quat_mul(delta_xyzw, current_xyzw)


def add_rpy_deg(current_xyzw: Any, delta_rpy_deg: Any) -> list[float]:
    """Add an RPY delta component-wise and return the recomposed quaternion.

    This is field-add semantics, not rigid-body rotation composition.  It is an
    explicit pure authoring calculation and does not select a runtime frame or
    resolve a resource reference.
    """
    current_rpy = xyzw_to_rpy_deg(current_xyzw)
    delta = _finite_list(delta_rpy_deg, 3, "delta_rpy_deg")
    return rpy_deg_to_xyzw(
        [
            current_rpy[0] + delta[0],
            current_rpy[1] + delta[1],
            current_rpy[2] + delta[2],
        ]
    )


def quat_conjugate(q: Any) -> list[float]:
    """Return the inverse rotation of a unit quaternion."""
    x, y, z, w = normalize_quat(q)
    return [-x, -y, -z, w]


def quat_rotate_vector(q: Any, vector: Any) -> list[float]:
    """Rotate one explicit three-vector by ``q``."""
    x, y, z, w = normalize_quat(q)
    vx, vy, vz = _finite_list(vector, 3, "vector")
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    result = [
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    ]
    if any(not math.isfinite(component) for component in result):
        raise GeometryError("rotated vector exceeds finite numeric range")
    return result


def distance_between(a_position_m: Any, b_position_m: Any) -> float:
    """Return Euclidean distance between two explicit position components."""
    a = _finite_list(a_position_m, 3, "a_position_m")
    b = _finite_list(b_position_m, 3, "b_position_m")
    deltas = [left - right for left, right in zip(a, b)]
    if any(not math.isfinite(delta) for delta in deltas):
        raise GeometryError("position difference exceeds finite numeric range")
    distance = math.hypot(*deltas)
    if not math.isfinite(distance):
        raise GeometryError("distance exceeds finite numeric range")
    return distance


def angle_between(a_xyzw: Any, b_xyzw: Any) -> float:
    """Return the smallest rotation angle in degrees."""
    qa, qb = normalize_quat(a_xyzw), normalize_quat(b_xyzw)
    dot = min(1.0, abs(sum(left * right for left, right in zip(qa, qb))))
    return math.degrees(2.0 * math.acos(dot))


def relative_pose_components(
    *,
    target_position_m: Any,
    target_orientation_xyzw: Any,
    reference_position_m: Any,
    reference_orientation_xyzw: Any,
) -> tuple[list[float], list[float]]:
    """Express target components in the reference components' local basis.

    Frame compatibility and identity resolution are intentionally absent: the
    future semantic owner must establish them before calling this pure helper.
    The return value is ``(relative_position_m, relative_orientation_xyzw)``.
    """
    target_position = _finite_list(target_position_m, 3, "target_position_m")
    reference_position = _finite_list(
        reference_position_m,
        3,
        "reference_position_m",
    )
    inverse_reference = quat_conjugate(reference_orientation_xyzw)
    relative_position = quat_rotate_vector(
        inverse_reference,
        [
            target - reference
            for target, reference in zip(target_position, reference_position)
        ],
    )
    relative_orientation = quat_mul(
        inverse_reference,
        target_orientation_xyzw,
    )
    return relative_position, relative_orientation


def _finite_list(value: Any, length: int, label: str) -> list[float]:
    if not isinstance(value, (list, tuple)) or len(value) != length:
        raise GeometryError(f"{label} must contain exactly {length} numbers")
    result: list[float] = []
    for index, item in enumerate(value):
        if isinstance(item, bool) or not isinstance(item, Real):
            raise GeometryError(f"{label}[{index}] must be a finite number")
        try:
            converted = float(item)
        except (OverflowError, TypeError, ValueError) as error:
            raise GeometryError(
                f"{label}[{index}] must be a finite number"
            ) from error
        if not math.isfinite(converted):
            raise GeometryError(f"{label}[{index}] must be a finite number")
        result.append(converted)
    return result
