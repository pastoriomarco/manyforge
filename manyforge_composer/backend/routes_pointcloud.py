"""Owner-neutral MFPC codec and typed I9 point-cloud client gate.

The last full Composer implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``.  Its source blob
``655347d93689962b7ec980ed2e2a7219315b27fe`` records the exact interaction
baseline: environment/qualification helpers at lines 35-96, the retained
codec at lines 98-225, the WebSocket client/publisher and demand/status owner
at lines 228-523, the qualification producer at lines 526-594, and the old
route at lines 597-663.  The removed cohorts are I9 restoration and acceptance
inputs, not dormant fallback code.

This module intentionally creates no socket, task, thread, demand registry,
fanout queue, source fixture, or runtime status owner.  It retains only the
bounded binary codec and a route factory that fails before reading state or
security configuration.
"""

from __future__ import annotations

import math
import os
import struct
from array import array
from typing import Any, Final, Sequence, TYPE_CHECKING

from fastapi import APIRouter

from .security import SecurityConfig

if TYPE_CHECKING:
    from .state import ComposerState


_POINTCLOUD_MAGIC: Final = b"MFPC"
_POINTCLOUD_VERSION: Final = 1
_POINTCLOUD_FLAG_COLORS: Final = 1 << 0
_POINTCLOUD_FLAG_CLEAR: Final = 1 << 1
_POINTCLOUD_HEADER: Final = struct.Struct("<4sBBHQQQII")
_PROTOCOL_MAX_POINTS: Final = 200_000

POINTCLOUD_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_pointcloud_client_unavailable"
)
POINTCLOUD_RESTORATION_GATE: Final = "I9"


def _pack_pointcloud_frame(
    *,
    stream_epoch: int,
    sequence: int,
    timestamp_ns: int,
    points: Sequence[Sequence[float]],
    colors: Sequence[Sequence[float]] | None,
    max_points: int,
) -> bytes:
    """Encode one bounded MFPC frame without transport or runtime ownership."""
    if max_points <= 0 or max_points > _PROTOCOL_MAX_POINTS:
        raise ValueError("max_points is outside the supported range")
    count = len(points)
    if count > max_points:
        raise ValueError(f"point cloud has {count} points; limit is {max_points}")
    if colors is not None and len(colors) != count:
        raise ValueError("point cloud colors must match point count")

    positions = array("f")
    color_bytes = bytearray(count * 3) if colors is not None else None
    for index, point in enumerate(points):
        try:
            x, y, z = float(point[0]), float(point[1]), float(point[2])
        except (IndexError, TypeError, ValueError) as exc:
            raise ValueError(f"point {index} is not an xyz triple") from exc
        if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)):
            raise ValueError(f"point {index} is not finite")
        positions.extend((x, y, z))
        if color_bytes is not None and colors is not None:
            color = colors[index]
            try:
                red, green, blue = (
                    float(color[0]),
                    float(color[1]),
                    float(color[2]),
                )
            except (IndexError, TypeError, ValueError) as exc:
                raise ValueError(f"color {index} is not an rgb triple") from exc
            if not (
                math.isfinite(red)
                and math.isfinite(green)
                and math.isfinite(blue)
            ):
                raise ValueError(f"color {index} is not finite")
            offset = index * 3
            color_bytes[offset] = round(max(0.0, min(1.0, red)) * 255.0)
            color_bytes[offset + 1] = round(max(0.0, min(1.0, green)) * 255.0)
            color_bytes[offset + 2] = round(max(0.0, min(1.0, blue)) * 255.0)

    if positions.itemsize != 4:
        raise RuntimeError("platform float array is not IEEE-754 float32")
    if os.sys.byteorder != "little":
        positions.byteswap()
    payload = positions.tobytes()
    flags = 0
    if color_bytes is not None:
        flags |= _POINTCLOUD_FLAG_COLORS
        payload += color_bytes
    header = _POINTCLOUD_HEADER.pack(
        _POINTCLOUD_MAGIC,
        _POINTCLOUD_VERSION,
        flags,
        _POINTCLOUD_HEADER.size,
        int(stream_epoch),
        int(sequence),
        int(timestamp_ns),
        count,
        len(payload),
    )
    return header + payload


def _pack_pointcloud_clear(
    *,
    stream_epoch: int,
    sequence: int,
    timestamp_ns: int,
) -> bytes:
    """Encode one empty MFPC clear generation without transport effects."""
    return _POINTCLOUD_HEADER.pack(
        _POINTCLOUD_MAGIC,
        _POINTCLOUD_VERSION,
        _POINTCLOUD_FLAG_CLEAR,
        _POINTCLOUD_HEADER.size,
        int(stream_epoch),
        int(sequence),
        int(timestamp_ns),
        0,
        0,
    )


def decode_pointcloud_packet(packet: bytes) -> dict[str, Any]:
    """Validate and describe one bounded MFPC packet without copying payload."""
    if len(packet) < _POINTCLOUD_HEADER.size:
        raise ValueError("point-cloud packet is shorter than its header")
    (
        magic,
        version,
        flags,
        header_size,
        stream_epoch,
        sequence,
        timestamp_ns,
        point_count,
        payload_bytes,
    ) = _POINTCLOUD_HEADER.unpack_from(packet)
    if magic != _POINTCLOUD_MAGIC or version != _POINTCLOUD_VERSION:
        raise ValueError("unsupported point-cloud protocol")
    if header_size != _POINTCLOUD_HEADER.size:
        raise ValueError("invalid point-cloud header size")
    if flags & ~(_POINTCLOUD_FLAG_COLORS | _POINTCLOUD_FLAG_CLEAR):
        raise ValueError("unsupported point-cloud flags")
    if point_count > _PROTOCOL_MAX_POINTS:
        raise ValueError("point-cloud count exceeds protocol limit")
    expected_payload = point_count * 12
    if flags & _POINTCLOUD_FLAG_COLORS:
        expected_payload += point_count * 3
    if flags & _POINTCLOUD_FLAG_CLEAR:
        if point_count != 0 or payload_bytes != 0:
            raise ValueError("clear packet carries point data")
    elif payload_bytes != expected_payload:
        raise ValueError("point-cloud payload shape is inconsistent")
    if len(packet) != header_size + payload_bytes:
        raise ValueError("point-cloud packet length is inconsistent")
    return {
        "stream_epoch": stream_epoch,
        "sequence": sequence,
        "timestamp_ns": timestamp_ns,
        "point_count": point_count,
        "has_colors": bool(flags & _POINTCLOUD_FLAG_COLORS),
        "clear": bool(flags & _POINTCLOUD_FLAG_CLEAR),
        "payload": memoryview(packet)[header_size:],
    }


class PointCloudClientUnavailableError(RuntimeError):
    """Raised before state access, transport creation, or route registration."""


def create_pointcloud_router(
    _state: "ComposerState",
    _security: SecurityConfig,
) -> APIRouter:
    raise PointCloudClientUnavailableError(
        f"{POINTCLOUD_CLIENT_UNAVAILABLE_REASON_CODE}: point-cloud WebSocket "
        "routing returns at I9 after binding to the new observer owner"
    )


__all__ = [
    "PointCloudClientUnavailableError",
    "create_pointcloud_router",
    "decode_pointcloud_packet",
]
