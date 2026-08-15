"""Exact MFOB point-cloud observation contract coverage."""

import struct

import pytest

from manyforge_headless_api.pointcloud_observation import (
    POINTCLOUD_MAX_POINTS,
    decode_pointcloud_observation,
    encode_pointcloud_clear,
    encode_pointcloud_observation,
)


COMMON = {
    "owner_id": "pointcloud.owner.1",
    "owner_epoch": "pointcloud.epoch.1",
    "cell_id": "isaac.cell.1",
    "stream_generation": 4,
    "sequence": 7,
    "timestamp_ns": 123_456,
}


def test_mfob_round_trip_has_owner_frame_and_no_copy_payload_views() -> None:
    packet = encode_pointcloud_observation(
        **COMMON,
        points=((1.25, -2.5, 3.75), (0.0, 0.5, 1.0)),
        colors_rgb8=((255, 128, 0), (0, 255, 64)),
    )
    observation = decode_pointcloud_observation(packet)

    assert packet[:4] == b"MFOB"
    assert observation.owner_id == COMMON["owner_id"]
    assert observation.owner_epoch == COMMON["owner_epoch"]
    assert observation.cell_id == COMMON["cell_id"]
    assert observation.frame_id == "cell:isaac.cell.1"
    assert observation.stream_generation == 4
    assert observation.sequence == 7
    assert observation.timestamp_ns == 123_456
    assert observation.point_count == 2
    assert struct.unpack("<6f", observation.positions_f32le) == pytest.approx(
        (1.25, -2.5, 3.75, 0.0, 0.5, 1.0)
    )
    assert bytes(observation.colors_rgb8 or b"") == bytes(
        (255, 128, 0, 0, 255, 64)
    )
    assert observation.positions_f32le.obj is packet


def test_clear_is_an_owner_bound_empty_generation() -> None:
    packet = encode_pointcloud_clear(**COMMON)
    observation = decode_pointcloud_observation(packet)

    assert observation.clear is True
    assert observation.point_count == 0
    assert bytes(observation.positions_f32le) == b""
    assert observation.colors_rgb8 is None


def test_protocol_rejects_old_magic_nonfinite_shape_and_identity_tampering() -> None:
    packet = encode_pointcloud_observation(
        **COMMON,
        points=((0.0, 0.0, 0.0),),
    )
    old = b"MFPC" + packet[4:]
    with pytest.raises(ValueError, match="unsupported"):
        decode_pointcloud_observation(old)

    nonfinite = bytearray(packet)
    header_bytes = struct.unpack_from("<H", nonfinite, 6)[0]
    struct.pack_into("<f", nonfinite, header_bytes, float("nan"))
    with pytest.raises(ValueError, match="non-finite"):
        decode_pointcloud_observation(bytes(nonfinite))

    frame_tampered = bytearray(packet)
    frame_tampered[frame_tampered.index(b"cell:isaac.cell.1")] = ord("x")
    with pytest.raises(ValueError, match="canonical cell frame"):
        decode_pointcloud_observation(bytes(frame_tampered))

    with pytest.raises(ValueError, match="packet length"):
        decode_pointcloud_observation(packet + b"\x00")


def test_encoder_rejects_bounds_bad_colors_and_nonfinite_coordinates() -> None:
    with pytest.raises(ValueError, match="limit is 1"):
        encode_pointcloud_observation(
            **COMMON,
            points=((0.0, 0.0, 0.0), (1.0, 1.0, 1.0)),
            max_points=1,
        )
    with pytest.raises(ValueError, match="colors must match"):
        encode_pointcloud_observation(
            **COMMON,
            points=((0.0, 0.0, 0.0),),
            colors_rgb8=(),
        )
    with pytest.raises(ValueError, match="RGB8"):
        encode_pointcloud_observation(
            **COMMON,
            points=((0.0, 0.0, 0.0),),
            colors_rgb8=((256, 0, 0),),
        )
    with pytest.raises(ValueError, match="xyz triple"):
        encode_pointcloud_observation(
            **COMMON,
            points=((0.0, 0.0, 0.0, 1.0),),
        )
    with pytest.raises(ValueError, match="RGB8"):
        encode_pointcloud_observation(
            **COMMON,
            points=((0.0, 0.0, 0.0),),
            colors_rgb8=(1,),  # type: ignore[arg-type]
        )
    with pytest.raises(ValueError, match="not finite"):
        encode_pointcloud_observation(
            **COMMON,
            points=((float("inf"), 0.0, 0.0),),
        )
    assert POINTCLOUD_MAX_POINTS == 200_000
