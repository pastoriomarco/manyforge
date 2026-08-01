"""Portable pose-distance math and I6/N geometry-node gates."""

from __future__ import annotations

import math
from numbers import Real
from typing import Final, Sequence


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
