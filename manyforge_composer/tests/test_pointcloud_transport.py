"""MFPC codec coverage and exact disposition of the former live channel.

Historical test blob ``9c60fec84f8355c76627ca9515a07b7fd83bb01d``
(``sha256:a90d9e79220ce7e4d1f32c44fb8b5caed74c9f646a6bdcfb6113b453b5e33787``)
at ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains nine
cases.  Packet encoding/validation remains active below.  WebSocket delivery,
demand lifetime, sequence/status ownership, fanout, slow-client isolation, and
the qualification producer remain exact I9 acceptance inputs, not runnable
legacy fallbacks.
"""

from __future__ import annotations

import struct
from typing import Final

import pytest

from manyforge_composer.backend.routes_pointcloud import (
    POINTCLOUD_CLIENT_UNAVAILABLE_REASON_CODE,
    PointCloudClientUnavailableError,
    _pack_pointcloud_clear,
    _pack_pointcloud_frame,
    create_pointcloud_router,
    decode_pointcloud_packet,
)


HISTORICAL_CASE_COUNT: Final = 9
HISTORICAL_SOURCE_BLOB: Final = "655347d93689962b7ec980ed2e2a7219315b27fe"
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "mfpc_shape_color_and_delivery",
        "historical lines 49-74",
        1,
        True,
        ("I9",),
        "packet shape and color quantization are retained as a direct codec "
        "test; socket delivery returns with the I9 observer client",
    ),
    (
        "mfpc_decoder_validation",
        "historical lines 235-242",
        1,
        True,
        (),
        "retained below as owner-neutral malformed-packet validation",
    ),
    (
        "demand_fanout_sequence_and_status",
        "historical lines 77-87 and 127-232",
        5,
        False,
        ("I9",),
        "no-observer suppression, accepted-sequence settlement, capacity-one "
        "replacement, demand epochs, status, and slow-client isolation return "
        "against the new observer-owned transport",
    ),
    (
        "qualification_source_and_demand_scope",
        "historical lines 90-124",
        2,
        False,
        ("I9",),
        "the synthetic cloud and async producer return only as qualification "
        "fixtures for the restored I9 channel",
    ),
)


def test_binary_packet_round_trip_has_exact_shape_and_color_quantization() -> None:
    packet = _pack_pointcloud_frame(
        stream_epoch=17,
        sequence=3,
        timestamp_ns=123_456,
        points=[[1.25, -2.5, 3.75], [0.0, 0.5, 1.0]],
        colors=[[1.0, 0.5, 0.0], [-1.0, 2.0, 0.25]],
        max_points=4,
    )

    decoded = decode_pointcloud_packet(packet)

    assert decoded["stream_epoch"] == 17
    assert decoded["sequence"] == 3
    assert decoded["timestamp_ns"] == 123_456
    assert decoded["point_count"] == 2
    assert decoded["has_colors"] is True
    assert decoded["clear"] is False
    payload = decoded["payload"]
    assert struct.unpack_from("<6f", payload, 0) == pytest.approx(
        (1.25, -2.5, 3.75, 0.0, 0.5, 1.0)
    )
    assert bytes(payload[24:]) == bytes((255, 128, 0, 0, 255, 64))


def test_frame_encoder_enforces_protocol_bounds_and_finite_triples() -> None:
    common = {
        "stream_epoch": 1,
        "sequence": 1,
        "timestamp_ns": 1,
    }
    with pytest.raises(ValueError, match="outside the supported range"):
        _pack_pointcloud_frame(
            **common,
            points=[],
            colors=None,
            max_points=0,
        )
    with pytest.raises(ValueError, match="limit is 1"):
        _pack_pointcloud_frame(
            **common,
            points=[[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]],
            colors=None,
            max_points=1,
        )
    with pytest.raises(ValueError, match="colors must match"):
        _pack_pointcloud_frame(
            **common,
            points=[[0.0, 0.0, 0.0]],
            colors=[],
            max_points=1,
        )
    with pytest.raises(ValueError, match="point 0 is not finite"):
        _pack_pointcloud_frame(
            **common,
            points=[[float("nan"), 0.0, 0.0]],
            colors=None,
            max_points=1,
        )
    with pytest.raises(ValueError, match="color 0 is not finite"):
        _pack_pointcloud_frame(
            **common,
            points=[[0.0, 0.0, 0.0]],
            colors=[[0.0, float("inf"), 0.0]],
            max_points=1,
        )


def test_clear_packet_is_an_empty_self_describing_generation() -> None:
    decoded = decode_pointcloud_packet(
        _pack_pointcloud_clear(
            stream_epoch=9,
            sequence=12,
            timestamp_ns=34,
        )
    )

    assert decoded["stream_epoch"] == 9
    assert decoded["sequence"] == 12
    assert decoded["timestamp_ns"] == 34
    assert decoded["point_count"] == 0
    assert decoded["has_colors"] is False
    assert decoded["clear"] is True
    assert bytes(decoded["payload"]) == b""


def test_decoder_rejects_truncation_inconsistent_payload_and_oversize() -> None:
    with pytest.raises(ValueError, match="shorter"):
        decode_pointcloud_packet(b"MFPC")

    inconsistent = bytearray(40)
    struct.pack_into(
        "<4sBBHQQQII",
        inconsistent,
        0,
        b"MFPC",
        1,
        0,
        40,
        1,
        1,
        1,
        1,
        12,
    )
    with pytest.raises(ValueError, match="length"):
        decode_pointcloud_packet(bytes(inconsistent))

    oversized = bytearray(40)
    struct.pack_into(
        "<4sBBHQQQII",
        oversized,
        0,
        b"MFPC",
        1,
        0,
        40,
        1,
        1,
        1,
        200_001,
        0,
    )
    with pytest.raises(ValueError, match="protocol limit"):
        decode_pointcloud_packet(bytes(oversized))


def test_pointcloud_router_fails_before_reading_state_or_security() -> None:
    class Poison:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"unavailable gate read an argument: {name}")

    with pytest.raises(
        PointCloudClientUnavailableError,
        match=POINTCLOUD_CLIENT_UNAVAILABLE_REASON_CODE,
    ):
        create_pointcloud_router(Poison(), Poison())  # type: ignore[arg-type]


def test_historical_pointcloud_cases_are_split_without_loss() -> None:
    assert HISTORICAL_SOURCE_BLOB == "655347d93689962b7ec980ed2e2a7219315b27fe"
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == (
        HISTORICAL_CASE_COUNT
    )
    assert {family[0] for family in HISTORICAL_CASE_FAMILIES if family[3]} == {
        "mfpc_shape_color_and_delivery",
        "mfpc_decoder_validation",
    }
    assert all(
        family[4] == ("I9",)
        for family in HISTORICAL_CASE_FAMILIES
        if not family[3]
    )
