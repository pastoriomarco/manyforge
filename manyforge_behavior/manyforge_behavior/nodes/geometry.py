"""Portable pose-distance math and I6/N geometry-node gates."""

from __future__ import annotations

import math
from collections.abc import Mapping
from numbers import Real
from typing import Any, Final, Sequence

from manyforge_behavior.geometry import (
    compose_local,
    compose_world,
    normalize_quat,
    quat_conjugate,
    quat_mul,
    quat_rotate_vector,
    rpy_deg_to_xyzw,
    xyzw_to_rpy_deg,
)


GEOMETRY_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_geometry_node_unavailable"
)
GEOMETRY_NODE_RESTORATION_GATES: Final = ("I6", "N")


class GeometryNodeUnavailableError(RuntimeError):
    """Raised before blackboard or model/FK state is accessed."""


def _finite_vector(values: Sequence[float], *, length: int, field: str) -> tuple[float, ...]:
    if type(values) not in (list, tuple) or len(values) != length:
        raise ValueError(f"{field} must contain exactly {length} values")
    result_list: list[float] = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, Real):
            raise ValueError(f"{field} must contain only finite numbers")
        try:
            converted = float(value)
        except (OverflowError, TypeError, ValueError) as error:
            raise ValueError(
                f"{field} must contain only finite numbers"
            ) from error
        if not math.isfinite(converted):
            raise ValueError(f"{field} must contain only finite numbers")
        result_list.append(converted)
    result = tuple(result_list)
    return result


def _normalized_quaternion(
    values: tuple[float, ...], *, field: str
) -> tuple[float, ...]:
    scale = max(abs(value) for value in values)
    if scale == 0.0:
        raise ValueError(f"{field} must have non-zero norm")
    scaled = tuple(value / scale for value in values)
    norm = math.sqrt(sum(value * value for value in scaled))
    return tuple(value / norm for value in scaled)


def translation_distance_m(
    first: Sequence[float],
    second: Sequence[float],
) -> float:
    """Return Euclidean distance between two finite 3D positions."""
    a = _finite_vector(first, length=3, field="first position")
    b = _finite_vector(second, length=3, field="second position")
    deltas = tuple(left - right for left, right in zip(a, b))
    if any(not math.isfinite(value) for value in deltas):
        raise ValueError("position difference exceeds finite numeric range")
    distance = math.hypot(*deltas)
    if not math.isfinite(distance):
        raise ValueError("translation distance exceeds finite numeric range")
    return distance


def quaternion_angular_distance_rad(
    first_xyzw: Sequence[float],
    second_xyzw: Sequence[float],
) -> float:
    """Return shortest angular distance between two finite quaternions."""
    first = _finite_vector(first_xyzw, length=4, field="first quaternion")
    second = _finite_vector(second_xyzw, length=4, field="second quaternion")
    first_unit = _normalized_quaternion(first, field="first quaternion")
    second_unit = _normalized_quaternion(second, field="second quaternion")
    dot = sum(a * b for a, b in zip(first_unit, second_unit))
    return 2.0 * math.acos(max(-1.0, min(1.0, abs(dot))))


def offset_pose(
    pose: Mapping[str, Any],
    offset: Mapping[str, Any],
    *,
    axes_xyzw: Sequence[float] | None = None,
) -> dict[str, Any]:
    """Apply one translation + intrinsic-ZYX RPY-degree offset to a pose.

    With ``axes_xyzw`` None the offset is measured along the pose's own axes
    (position += R*t, q = q*dq). Otherwise ``axes_xyzw`` is the orientation,
    in the pose's frame, of the axes the offset is measured along (identity
    for the pose's frame itself): position += A*t, q = (A*dq*A^-1)*q. The
    result keeps the input frame and orientation representation.
    """
    frame_ref = pose["frame_ref"]
    position = _finite_vector(pose["position_m"], length=3, field="pose position_m")
    orientation = pose["orientation"]
    representation = "xyzw" if "xyzw" in orientation else "rpy_deg"
    current = (
        normalize_quat(orientation["xyzw"])
        if representation == "xyzw"
        else rpy_deg_to_xyzw(orientation["rpy_deg"])
    )
    translation = _finite_vector(offset["position_m"], length=3, field="offset position_m")
    delta = rpy_deg_to_xyzw(offset["rpy_deg"])
    if axes_xyzw is None:
        moved = quat_rotate_vector(current, translation)
        rotated = compose_local(current, delta)
    else:
        axes = normalize_quat(
            _finite_vector(axes_xyzw, length=4, field="offset axes orientation")
        )
        moved = quat_rotate_vector(axes, translation)
        rotated = compose_world(
            current, quat_mul(quat_mul(axes, delta), quat_conjugate(axes))
        )
    result_position = [origin + step for origin, step in zip(position, moved)]
    if any(not math.isfinite(value) for value in result_position):
        raise ValueError("offset pose exceeds finite numeric range")
    return {
        "frame_ref": dict(frame_ref),
        "position_m": result_position,
        "orientation": {
            representation: (
                rotated if representation == "xyzw" else xyzw_to_rpy_deg(rotated)
            )
        },
    }


def position_within_bounds(
    position_m: Sequence[float],
    minimum_m: Sequence[float],
    maximum_m: Sequence[float],
    *,
    inclusive: bool,
) -> bool:
    """Return whether a position lies inside an axis-aligned box."""
    position = _finite_vector(position_m, length=3, field="position_m")
    low = _finite_vector(minimum_m, length=3, field="min_position_m")
    high = _finite_vector(maximum_m, length=3, field="max_position_m")
    if any(lower > upper for lower, upper in zip(low, high)):
        raise ValueError("min_position_m exceeds max_position_m")
    return all(
        (lower <= value <= upper) if inclusive else (lower < value < upper)
        for value, lower, upper in zip(position, low, high)
    )


class _UnavailableGeometryNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise GeometryNodeUnavailableError(
            f"{GEOMETRY_NODE_UNAVAILABLE_REASON_CODE}: addressed model/FK "
            "geometry nodes return at I6/N"
        )


class GetLinkPose(_UnavailableGeometryNode):
    pass


class CheckPoseDistance(_UnavailableGeometryNode):
    pass
