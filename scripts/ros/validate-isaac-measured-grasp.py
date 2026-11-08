#!/usr/bin/env python3
"""Measured-grasp pose math plus an unavailable live runner.

The bounded rigid-transform math is independent of Composer, ROS, and the
simulator and remains as same-role qualification logic.  The old live harness
and its unused ``EvidenceRow``/``QualificationResult`` wire are pinned at blob
``58f32e3a926f0c3d54183b985bd16ecea6805469``.  They may return only as a
client and evidence contract of the headless scene and Kernel Stop owners.
"""

from __future__ import annotations

import math
import sys
from itertools import islice
from numbers import Real
from typing import Any, Iterable, Sequence


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live measured-grasp qualification is not "
    "available yet"
)
MAX_FRAME_ID_CHARS = 1024
RIGID_TRANSFORM_TOLERANCE = 1e-6


def normalize_quaternion(
    quaternion_xyzw: Sequence[float],
) -> tuple[float, float, float, float]:
    """Return a finite unit quaternion or reject malformed evidence."""

    x, y, z, w = _finite_vector(
        quaternion_xyzw,
        length=4,
        label="orientation_xyzw",
    )
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm <= 0.0 or not math.isfinite(norm):
        raise ValueError("orientation_xyzw must be finite and non-zero")
    return x / norm, y / norm, z / norm, w / norm


def pose_to_matrix(
    position_m: Sequence[float],
    orientation_xyzw: Sequence[float],
) -> list[list[float]]:
    """Convert one normalized pose value to a homogeneous transform."""

    position = _finite_vector(position_m, length=3, label="position_m")
    x, y, z, w = normalize_quaternion(orientation_xyzw)
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return [
        [1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy), position[0]],
        [2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx), position[1]],
        [2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy), position[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def multiply_transforms(
    left: Sequence[Sequence[float]],
    right: Sequence[Sequence[float]],
) -> list[list[float]]:
    """Multiply two 4x4 homogeneous transforms."""

    left_matrix = _rigid_transform(left, label="left transform")
    right_matrix = _rigid_transform(right, label="right transform")
    return [
        [
            sum(
                left_matrix[row][index] * right_matrix[index][column]
                for index in range(4)
            )
            for column in range(4)
        ]
        for row in range(4)
    ]


def invert_rigid_transform(transform: Sequence[Sequence[float]]) -> list[list[float]]:
    """Invert a 4x4 rigid transform without a numeric-library dependency."""

    matrix = _rigid_transform(transform, label="transform")
    rotation_t = [
        [matrix[column][row] for column in range(3)] for row in range(3)
    ]
    translation = [matrix[row][3] for row in range(3)]
    inverse_translation = [
        -sum(rotation_t[row][column] * translation[column] for column in range(3))
        for row in range(3)
    ]
    return [
        [*rotation_t[0], inverse_translation[0]],
        [*rotation_t[1], inverse_translation[1]],
        [*rotation_t[2], inverse_translation[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def matrix_to_quaternion(transform: Sequence[Sequence[float]]) -> list[float]:
    """Extract a normalized XYZW quaternion from a 4x4 transform."""

    matrix = _rigid_transform(transform, label="transform")
    m00, m01, m02 = matrix[0][0], matrix[0][1], matrix[0][2]
    m10, m11, m12 = matrix[1][0], matrix[1][1], matrix[1][2]
    m20, m21, m22 = matrix[2][0], matrix[2][1], matrix[2][2]
    trace = m00 + m11 + m22
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * scale
        x = (m21 - m12) / scale
        y = (m02 - m20) / scale
        z = (m10 - m01) / scale
    elif m00 > m11 and m00 > m22:
        scale = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        w = (m21 - m12) / scale
        x = 0.25 * scale
        y = (m01 + m10) / scale
        z = (m02 + m20) / scale
    elif m11 > m22:
        scale = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        w = (m02 - m20) / scale
        x = (m01 + m10) / scale
        y = 0.25 * scale
        z = (m12 + m21) / scale
    else:
        scale = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        w = (m10 - m01) / scale
        x = (m02 + m20) / scale
        y = (m12 + m21) / scale
        z = 0.25 * scale
    return list(normalize_quaternion((x, y, z, w)))


def matrix_to_pose(
    transform: Sequence[Sequence[float]], *, frame_id: str
) -> dict[str, Any]:
    """Serialize one transform using the retained transport-neutral pose keys."""

    if not isinstance(frame_id, str) or not frame_id:
        raise ValueError("frame_id must be a non-empty string")
    if len(frame_id) > MAX_FRAME_ID_CHARS:
        raise ValueError(
            f"frame_id exceeds the {MAX_FRAME_ID_CHARS}-character bound"
        )
    matrix = _rigid_transform(transform, label="transform")
    return {
        "frame_id": frame_id,
        "position_m": [matrix[row][3] for row in range(3)],
        "orientation_xyzw": matrix_to_quaternion(matrix),
    }


def _finite_vector(
    values: Iterable[Any], *, length: int, label: str
) -> tuple[float, ...]:
    if isinstance(values, (str, bytes, bytearray)):
        raise ValueError(f"{label} must contain {length} real values")
    try:
        bounded = tuple(islice(iter(values), length + 1))
    except TypeError as exc:
        raise ValueError(f"{label} must contain {length} real values") from exc
    if len(bounded) != length:
        raise ValueError(f"{label} must contain {length} values")
    normalized: list[float] = []
    for index, value in enumerate(bounded):
        if isinstance(value, bool) or not isinstance(value, Real):
            raise ValueError(f"{label}[{index}] must be a real number")
        numeric = float(value)
        if not math.isfinite(numeric):
            raise ValueError(f"{label}[{index}] must be finite")
        normalized.append(numeric)
    return tuple(normalized)


def _rigid_transform(
    transform: Iterable[Iterable[Any]], *, label: str
) -> tuple[tuple[float, ...], ...]:
    if isinstance(transform, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be a 4x4 rigid transform")
    try:
        rows = tuple(islice(iter(transform), 5))
    except TypeError as exc:
        raise ValueError(f"{label} must be a 4x4 rigid transform") from exc
    if len(rows) != 4:
        raise ValueError(f"{label} must be a 4x4 rigid transform")
    matrix = tuple(
        _finite_vector(row, length=4, label=f"{label}[{index}]")
        for index, row in enumerate(rows)
    )
    expected_bottom = (0.0, 0.0, 0.0, 1.0)
    if any(
        abs(actual - expected) > RIGID_TRANSFORM_TOLERANCE
        for actual, expected in zip(matrix[3], expected_bottom, strict=True)
    ):
        raise ValueError(f"{label} must have the homogeneous bottom row")
    rotation = tuple(row[:3] for row in matrix[:3])
    for row_index, row in enumerate(rotation):
        for other_index, other in enumerate(rotation):
            dot = sum(
                left * right for left, right in zip(row, other, strict=True)
            )
            expected = 1.0 if row_index == other_index else 0.0
            if abs(dot - expected) > RIGID_TRANSFORM_TOLERANCE:
                raise ValueError(f"{label} rotation must be orthonormal")
    determinant = (
        rotation[0][0]
        * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1])
        - rotation[0][1]
        * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0])
        + rotation[0][2]
        * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0])
    )
    if abs(determinant - 1.0) > RIGID_TRANSFORM_TOLERANCE:
        raise ValueError(f"{label} rotation determinant must be +1")
    return matrix


def tcp_to_object_baseline(
    tcp_transform: Sequence[Sequence[float]],
    object_transform: Sequence[Sequence[float]],
) -> list[list[float]]:
    """Compute the measured TCP-to-object baseline used by the qualifier."""

    return multiply_transforms(invert_rigid_transform(tcp_transform), object_transform)


def object_from_tcp_baseline(
    tcp_transform: Sequence[Sequence[float]],
    baseline_transform: Sequence[Sequence[float]],
) -> list[list[float]]:
    """Project the measured object pose from TCP and captured baseline."""

    return multiply_transforms(tcp_transform, baseline_transform)


def main(argv: Sequence[str] | None = None) -> int:
    """Fail before HTTP, ROS, simulator, cycle, or scene effects."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
