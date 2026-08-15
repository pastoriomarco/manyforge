"""Exact bounded binary contract for cell-frame point-cloud observations."""

from __future__ import annotations

from array import array
from dataclasses import dataclass
import math
import os
import re
import struct
from typing import Final, Sequence


POINTCLOUD_OBSERVATION_MAGIC: Final = b"MFOB"
POINTCLOUD_OBSERVATION_VERSION: Final = 1
POINTCLOUD_PROTOCOL_ID: Final = "manyforge.pointcloud_observation.v1"
POINTCLOUD_STATUS_SCHEMA: Final = "manyforge.viewer_pointcloud_status.v2"
POINTCLOUD_RECEIPT_SCHEMA: Final = "manyforge.pointcloud_observation_receipt.v1"
POINTCLOUD_MAX_POINTS: Final = 200_000
POINTCLOUD_MAX_PACKET_BYTES: Final = 3_002_048
POINTCLOUD_MEDIA_TYPE: Final = (
    "application/vnd.manyforge.pointcloud-observation"
)

_FLAG_COLORS: Final = 1 << 0
_FLAG_CLEAR: Final = 1 << 1
_HEADER: Final = struct.Struct("<4sBBHHHHHQQQII")
_IDENTITY = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:/-]*$")
_U64_MAX: Final = (1 << 64) - 1


@dataclass(frozen=True, slots=True)
class PointCloudObservation:
    """Decoded packet whose position/color views retain the packet bytes."""

    owner_id: str
    owner_epoch: str
    cell_id: str
    frame_id: str
    stream_generation: int
    sequence: int
    timestamp_ns: int
    point_count: int
    clear: bool
    positions_f32le: memoryview
    colors_rgb8: memoryview | None


def _u64(value: object, label: str) -> int:
    if type(value) is not int or value < 0 or value > _U64_MAX:
        raise ValueError(f"{label} must be an unsigned 64-bit integer")
    return value


def _identity(value: object, label: str, *, maximum_bytes: int) -> bytes:
    if type(value) is not str or _IDENTITY.fullmatch(value) is None:
        raise ValueError(f"{label} is not a bounded observation identity")
    encoded = value.encode("utf-8", errors="strict")
    if len(encoded) > maximum_bytes:
        raise ValueError(f"{label} exceeds its {maximum_bytes}-byte bound")
    return encoded


def _identity_block(
    owner_id: str,
    owner_epoch: str,
    cell_id: str,
) -> tuple[bytes, tuple[int, int, int, int]]:
    frame_id = f"cell:{cell_id}"
    fields = (
        _identity(owner_id, "owner_id", maximum_bytes=128),
        _identity(owner_epoch, "owner_epoch", maximum_bytes=256),
        _identity(cell_id, "cell_id", maximum_bytes=256),
        _identity(frame_id, "frame_id", maximum_bytes=261),
    )
    block = b"".join(fields)
    padding = b"\x00" * (-(_HEADER.size + len(block)) % 4)
    return block + padding, tuple(len(field) for field in fields)  # type: ignore[return-value]


def _header_and_payload(
    *,
    owner_id: str,
    owner_epoch: str,
    cell_id: str,
    stream_generation: int,
    sequence: int,
    timestamp_ns: int,
    point_count: int,
    positions_f32le: bytes,
    colors_rgb8: bytes | None,
    clear: bool,
) -> bytes:
    if type(point_count) is not int or point_count < 0:
        raise ValueError("point_count must be a non-negative integer")
    if point_count > POINTCLOUD_MAX_POINTS:
        raise ValueError("point_count exceeds the protocol limit")
    if type(positions_f32le) is not bytes:
        raise ValueError("positions_f32le must be immutable bytes")
    if colors_rgb8 is not None and type(colors_rgb8) is not bytes:
        raise ValueError("colors_rgb8 must be immutable bytes")
    if clear:
        if point_count != 0 or positions_f32le or colors_rgb8 is not None:
            raise ValueError("clear observation cannot carry point data")
    else:
        if point_count == 0:
            raise ValueError("an empty observation must use the clear flag")
        if len(positions_f32le) != point_count * 12:
            raise ValueError("position payload shape is inconsistent")
        if colors_rgb8 is not None and len(colors_rgb8) != point_count * 3:
            raise ValueError("color payload shape is inconsistent")
        for (coordinate,) in struct.iter_unpack("<f", positions_f32le):
            if not math.isfinite(coordinate):
                raise ValueError("position payload contains a non-finite coordinate")

    identity, lengths = _identity_block(owner_id, owner_epoch, cell_id)
    header_bytes = _HEADER.size + len(identity)
    payload = positions_f32le + (colors_rgb8 or b"")
    flags = (_FLAG_CLEAR if clear else 0) | (
        _FLAG_COLORS if colors_rgb8 is not None else 0
    )
    packet = _HEADER.pack(
        POINTCLOUD_OBSERVATION_MAGIC,
        POINTCLOUD_OBSERVATION_VERSION,
        flags,
        header_bytes,
        *lengths,
        _u64(stream_generation, "stream_generation"),
        _u64(sequence, "sequence"),
        _u64(timestamp_ns, "timestamp_ns"),
        point_count,
        len(payload),
    ) + identity + payload
    if len(packet) > POINTCLOUD_MAX_PACKET_BYTES:
        raise ValueError("point-cloud observation exceeds the packet byte bound")
    return packet


def encode_pointcloud_observation(
    *,
    owner_id: str,
    owner_epoch: str,
    cell_id: str,
    stream_generation: int,
    sequence: int,
    timestamp_ns: int,
    points: Sequence[Sequence[float]],
    colors_rgb8: Sequence[Sequence[int]] | None = None,
    max_points: int = POINTCLOUD_MAX_POINTS,
) -> bytes:
    """Encode one finite XYZ float32/RGB8 observation in cell coordinates."""
    if type(max_points) is not int or max_points < 1 or max_points > POINTCLOUD_MAX_POINTS:
        raise ValueError("max_points is outside the protocol range")
    count = len(points)
    if count > max_points:
        raise ValueError(f"point cloud has {count} points; limit is {max_points}")
    if colors_rgb8 is not None and len(colors_rgb8) != count:
        raise ValueError("point cloud colors must match point count")
    positions = array("f")
    colors = bytearray(count * 3) if colors_rgb8 is not None else None
    for index, point in enumerate(points):
        try:
            if len(point) != 3:
                raise ValueError
            xyz = (float(point[0]), float(point[1]), float(point[2]))
        except (IndexError, TypeError, ValueError) as error:
            raise ValueError(f"point {index} is not an xyz triple") from error
        if not all(math.isfinite(value) for value in xyz):
            raise ValueError(f"point {index} is not finite")
        positions.extend(xyz)
        if colors is not None and colors_rgb8 is not None:
            color = colors_rgb8[index]
            try:
                valid_color = len(color) == 3 and all(
                    type(channel) is int and 0 <= channel <= 255
                    for channel in color
                )
            except TypeError:
                valid_color = False
            if not valid_color:
                raise ValueError(f"color {index} is not an RGB8 triple")
            offset = index * 3
            colors[offset : offset + 3] = bytes(color)
    if positions.itemsize != 4:
        raise RuntimeError("platform float array is not IEEE-754 float32")
    if os.sys.byteorder != "little":
        positions.byteswap()
    return _header_and_payload(
        owner_id=owner_id,
        owner_epoch=owner_epoch,
        cell_id=cell_id,
        stream_generation=stream_generation,
        sequence=sequence,
        timestamp_ns=timestamp_ns,
        point_count=count,
        positions_f32le=positions.tobytes(),
        colors_rgb8=None if colors is None else bytes(colors),
        clear=False,
    )


def encode_pointcloud_clear(
    *,
    owner_id: str,
    owner_epoch: str,
    cell_id: str,
    stream_generation: int,
    sequence: int,
    timestamp_ns: int,
) -> bytes:
    return _header_and_payload(
        owner_id=owner_id,
        owner_epoch=owner_epoch,
        cell_id=cell_id,
        stream_generation=stream_generation,
        sequence=sequence,
        timestamp_ns=timestamp_ns,
        point_count=0,
        positions_f32le=b"",
        colors_rgb8=None,
        clear=True,
    )


def decode_pointcloud_observation(packet: bytes) -> PointCloudObservation:
    """Strictly validate one MFOB packet and return no-copy payload views."""
    if type(packet) is not bytes:
        raise ValueError("point-cloud observation packet must be immutable bytes")
    if len(packet) < _HEADER.size:
        raise ValueError("point-cloud observation is shorter than its header")
    if len(packet) > POINTCLOUD_MAX_PACKET_BYTES:
        raise ValueError("point-cloud observation exceeds the packet byte bound")
    (
        magic,
        version,
        flags,
        header_bytes,
        owner_id_bytes,
        owner_epoch_bytes,
        cell_id_bytes,
        frame_id_bytes,
        stream_generation,
        sequence,
        timestamp_ns,
        point_count,
        payload_bytes,
    ) = _HEADER.unpack_from(packet)
    if magic != POINTCLOUD_OBSERVATION_MAGIC or version != POINTCLOUD_OBSERVATION_VERSION:
        raise ValueError("unsupported point-cloud observation protocol")
    if flags & ~(_FLAG_COLORS | _FLAG_CLEAR):
        raise ValueError("unsupported point-cloud observation flags")
    lengths = (owner_id_bytes, owner_epoch_bytes, cell_id_bytes, frame_id_bytes)
    if any(length == 0 for length in lengths):
        raise ValueError("point-cloud observation identity is incomplete")
    identity_end = _HEADER.size + sum(lengths)
    expected_header_bytes = identity_end + (-(identity_end) % 4)
    if header_bytes != expected_header_bytes or header_bytes > len(packet):
        raise ValueError("point-cloud observation header size is inconsistent")
    if any(packet[index] != 0 for index in range(identity_end, header_bytes)):
        raise ValueError("point-cloud observation header padding is not zero")
    cursor = _HEADER.size
    decoded: list[str] = []
    for length in lengths:
        try:
            value = packet[cursor : cursor + length].decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            raise ValueError("point-cloud observation identity is not UTF-8") from error
        decoded.append(value)
        cursor += length
    owner_id, owner_epoch, cell_id, frame_id = decoded
    _identity(owner_id, "owner_id", maximum_bytes=128)
    _identity(owner_epoch, "owner_epoch", maximum_bytes=256)
    _identity(cell_id, "cell_id", maximum_bytes=256)
    _identity(frame_id, "frame_id", maximum_bytes=261)
    if frame_id != f"cell:{cell_id}":
        raise ValueError("point-cloud observation is not in its canonical cell frame")
    if point_count > POINTCLOUD_MAX_POINTS:
        raise ValueError("point_count exceeds the protocol limit")
    has_colors = bool(flags & _FLAG_COLORS)
    clear = bool(flags & _FLAG_CLEAR)
    expected_payload = point_count * (15 if has_colors else 12)
    if clear:
        if point_count != 0 or payload_bytes != 0 or has_colors:
            raise ValueError("clear observation carries point data")
    elif point_count == 0 or payload_bytes != expected_payload:
        raise ValueError("point-cloud observation payload shape is inconsistent")
    if len(packet) != header_bytes + payload_bytes:
        raise ValueError("point-cloud observation packet length is inconsistent")
    positions_end = header_bytes + point_count * 12
    positions = memoryview(packet)[header_bytes:positions_end]
    for (coordinate,) in struct.iter_unpack("<f", positions):
        if not math.isfinite(coordinate):
            raise ValueError("position payload contains a non-finite coordinate")
    colors = memoryview(packet)[positions_end:] if has_colors else None
    return PointCloudObservation(
        owner_id=owner_id,
        owner_epoch=owner_epoch,
        cell_id=cell_id,
        frame_id=frame_id,
        stream_generation=stream_generation,
        sequence=sequence,
        timestamp_ns=timestamp_ns,
        point_count=point_count,
        clear=clear,
        positions_f32le=positions,
        colors_rgb8=colors,
    )


__all__ = [
    "POINTCLOUD_MAX_PACKET_BYTES",
    "POINTCLOUD_MAX_POINTS",
    "POINTCLOUD_MEDIA_TYPE",
    "POINTCLOUD_OBSERVATION_MAGIC",
    "POINTCLOUD_OBSERVATION_VERSION",
    "POINTCLOUD_PROTOCOL_ID",
    "POINTCLOUD_RECEIPT_SCHEMA",
    "POINTCLOUD_STATUS_SCHEMA",
    "PointCloudObservation",
    "decode_pointcloud_observation",
    "encode_pointcloud_clear",
    "encode_pointcloud_observation",
]
