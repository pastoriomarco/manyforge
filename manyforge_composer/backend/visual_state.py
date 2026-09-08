"""Transport-neutral rigid-pose helpers; no visual observation owner/client.

The current native viewer consumes execution observations. The historical
dynamic-snapshot client was replaced, not deferred to a future factory here.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class VisualPose:
    """One finite rigid pose independent of an HTTP or WebSocket wire."""

    position_m: tuple[float, float, float]
    orientation_xyzw: tuple[float, float, float, float]

    def __post_init__(self) -> None:
        if any(not math.isfinite(value) for value in self.position_m):
            raise ValueError("visual pose position must be finite")
        normalized = _normalized_quaternion(self.orientation_xyzw)
        object.__setattr__(self, "orientation_xyzw", normalized)


def _normalized_quaternion(
    value: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    if len(value) != 4 or any(not math.isfinite(item) for item in value):
        raise ValueError("visual pose quaternion must contain four finite values")
    norm = math.sqrt(sum(item * item for item in value))
    if norm <= 0.0:
        raise ValueError("visual pose quaternion must have non-zero norm")
    return tuple(item / norm for item in value)  # type: ignore[return-value]


def _quaternion_product(
    left: tuple[float, float, float, float],
    right: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    lx, ly, lz, lw = left
    rx, ry, rz, rw = right
    return (
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
        lw * rw - lx * rx - ly * ry - lz * rz,
    )


def _rotate_vector(
    quaternion: tuple[float, float, float, float],
    vector: tuple[float, float, float],
) -> tuple[float, float, float]:
    conjugate = (-quaternion[0], -quaternion[1], -quaternion[2], quaternion[3])
    rotated = _quaternion_product(
        _quaternion_product(quaternion, (*vector, 0.0)),
        conjugate,
    )
    return rotated[:3]


def compose_pose_in_world(*, parent_pose: VisualPose, child_pose: VisualPose) -> VisualPose:
    """Compose a parent world pose and child pose in the parent frame."""
    rotated = _rotate_vector(parent_pose.orientation_xyzw, child_pose.position_m)
    return VisualPose(
        position_m=tuple(
            parent_pose.position_m[index] + rotated[index] for index in range(3)
        ),  # type: ignore[arg-type]
        orientation_xyzw=_quaternion_product(
            parent_pose.orientation_xyzw,
            child_pose.orientation_xyzw,
        ),
    )


def relative_pose_from_world(*, child_pose: VisualPose, parent_pose: VisualPose) -> VisualPose:
    """Express one world pose relative to another world pose."""
    inverse_orientation = (
        -parent_pose.orientation_xyzw[0],
        -parent_pose.orientation_xyzw[1],
        -parent_pose.orientation_xyzw[2],
        parent_pose.orientation_xyzw[3],
    )
    delta = tuple(
        child_pose.position_m[index] - parent_pose.position_m[index] for index in range(3)
    )
    return VisualPose(
        position_m=_rotate_vector(inverse_orientation, delta),
        orientation_xyzw=_quaternion_product(
            inverse_orientation,
            child_pose.orientation_xyzw,
        ),
    )


__all__ = [
    "VisualPose",
    "compose_pose_in_world",
    "relative_pose_from_world",
]
