"""Contract and boundedness tests for the binary point-cloud channel."""
from __future__ import annotations

import asyncio
import struct
from typing import Any

import pytest

pytest.importorskip("fastapi")

from manyforge_composer.backend.routes_pointcloud import (
    _PointCloudQualificationFixture,
    _qualification_fixture_cloud,
    PointCloudPublisher,
    decode_pointcloud_packet,
)


class _RecordingSocket:
    def __init__(self, *, fail: bool = False, block: bool = False) -> None:
        self.accepted = False
        self.fail = fail
        self.block = block
        self.release = asyncio.Event()
        self.send_entered = asyncio.Event()
        self.packets: list[bytes] = []

    async def accept(self) -> None:
        self.accepted = True

    async def send_bytes(self, packet: bytes) -> None:
        self.send_entered.set()
        if self.fail:
            raise RuntimeError("disconnected")
        if self.block:
            await self.release.wait()
        self.packets.append(packet)


async def _wait_until(predicate: Any, *, timeout_s: float = 1.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout_s
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            raise AssertionError("condition did not become true")
        await asyncio.sleep(0.001)


def test_binary_packet_round_trip_has_exact_shape_and_color_quantization() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher(max_points=4)
        socket = _RecordingSocket()
        client_id = await publisher.attach(socket)  # type: ignore[arg-type]
        sender = asyncio.create_task(publisher.send_loop(client_id))
        try:
            assert publisher.publish(
                [[1.25, -2.5, 3.75], [0.0, 0.5, 1.0]],
                [[1.0, 0.5, 0.0], [-1.0, 2.0, 0.25]],
            )
            await _wait_until(lambda: len(socket.packets) == 1)
            decoded = decode_pointcloud_packet(socket.packets[0])
            assert decoded["sequence"] == 1
            assert decoded["point_count"] == 2
            assert decoded["has_colors"] is True
            payload = decoded["payload"]
            assert struct.unpack_from("<6f", payload, 0) == pytest.approx(
                (1.25, -2.5, 3.75, 0.0, 0.5, 1.0)
            )
            assert bytes(payload[24:]) == bytes((255, 128, 0, 0, 255, 64))
        finally:
            await publisher.detach(client_id)
            await sender

    asyncio.run(scenario())


def test_no_observer_suppresses_source_iteration_and_encoding() -> None:
    class _ExplodingPoints:
        def __len__(self) -> int:
            raise AssertionError("source payload must not be inspected")

    publisher = PointCloudPublisher()
    assert publisher.publish(_ExplodingPoints()) is False  # type: ignore[arg-type]
    status = asyncio.run(publisher.status())
    assert status["suppressed_no_clients"] == 1
    assert status["published"] == 0
    assert status["sequence"] == 0


def test_qualification_cloud_is_deterministic_and_bounded() -> None:
    first = _qualification_fixture_cloud(7)
    second = _qualification_fixture_cloud(7)

    assert first == second
    assert len(first[0]) == len(first[1]) == 7
    assert all(len(point) == 3 for point in first[0])


def test_qualification_fixture_is_demand_scoped_and_uses_real_publisher() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher(max_points=8)
        socket = _RecordingSocket()
        client_id = await publisher.attach(socket)  # type: ignore[arg-type]
        sender = asyncio.create_task(publisher.send_loop(client_id))
        fixture = _PointCloudQualificationFixture(
            publisher,
            point_count=8,
            rate_hz=30.0,
        )
        fixture.start_if_needed()
        try:
            await _wait_until(lambda: len(socket.packets) >= 1)
            status = fixture.status()
            assert status["running"] is True
            assert status["frames_published"] >= 1
            decoded = decode_pointcloud_packet(socket.packets[-1])
            assert decoded["point_count"] == 8
        finally:
            await publisher.detach(client_id)
            await sender
        await fixture.wait_stopped()
        assert fixture.status()["running"] is False

    asyncio.run(scenario())


def test_rejected_frame_does_not_advance_sequence_or_replace_last_good_frame() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher(max_points=1)
        socket = _RecordingSocket()
        client_id = await publisher.attach(socket)  # type: ignore[arg-type]
        sender = asyncio.create_task(publisher.send_loop(client_id))
        try:
            assert publisher.publish([[0.0, 0.0, 0.0]])
            await _wait_until(lambda: len(socket.packets) == 1)
            with pytest.raises(ValueError, match="limit is 1"):
                publisher.publish([[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]])
            assert publisher.publish([[2.0, 2.0, 2.0]])
            await _wait_until(lambda: len(socket.packets) == 2)
            assert [
                decode_pointcloud_packet(packet)["sequence"]
                for packet in socket.packets
            ] == [1, 2]
            status = await publisher.status()
            assert status["rejected"] == 1
        finally:
            await publisher.detach(client_id)
            await sender

    asyncio.run(scenario())


def test_capacity_one_client_slot_replaces_intermediate_frames() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher()
        socket = _RecordingSocket(block=True)
        client_id = await publisher.attach(socket)  # type: ignore[arg-type]
        sender = asyncio.create_task(publisher.send_loop(client_id))
        try:
            assert publisher.publish([[1.0, 0.0, 0.0]])
            await asyncio.wait_for(socket.send_entered.wait(), timeout=1.0)
            assert publisher.publish([[2.0, 0.0, 0.0]])
            await asyncio.sleep(0)
            assert publisher.publish([[3.0, 0.0, 0.0]])
            await _wait_until(
                lambda: publisher._ingress_replacements >= 1  # noqa: SLF001
            )
            socket.release.set()
            await _wait_until(lambda: len(socket.packets) == 2)
            assert [
                struct.unpack_from(
                    "<f", decode_pointcloud_packet(packet)["payload"], 0
                )[0]
                for packet in socket.packets
            ] == [1.0, 3.0]
            status = await publisher.status()
            assert (
                status["ingress_replacements"]
                + status["client_replacements_total"]
            ) >= 1
        finally:
            await publisher.detach(client_id)
            await sender

    asyncio.run(scenario())


def test_last_detach_drops_stale_starter_frame_for_next_demand_lifetime() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher()
        first = _RecordingSocket()
        first_id = await publisher.attach(first)  # type: ignore[arg-type]
        assert publisher.publish([[1.0, 0.0, 0.0]])
        await asyncio.sleep(0)
        await publisher.detach(first_id)

        second = _RecordingSocket()
        second_id = await publisher.attach(second)  # type: ignore[arg-type]
        try:
            status = await publisher.status()
            assert status["clients"][second_id]["pending"] is False
            assert second.packets == []
        finally:
            await publisher.detach(second_id)

    asyncio.run(scenario())


def test_slow_client_times_out_without_delaying_fast_client() -> None:
    async def scenario() -> None:
        publisher = PointCloudPublisher()
        slow = _RecordingSocket(block=True)
        fast = _RecordingSocket()
        slow_id = await publisher.attach(slow)  # type: ignore[arg-type]
        fast_id = await publisher.attach(fast)  # type: ignore[arg-type]
        slow_sender = asyncio.create_task(publisher.send_loop(slow_id))
        fast_sender = asyncio.create_task(publisher.send_loop(fast_id))
        try:
            assert publisher.publish([[1.0, 2.0, 3.0]])
            await _wait_until(lambda: len(fast.packets) == 1)
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(slow_sender, timeout=1.0)
            assert publisher.has_clients()
            status = await publisher.status()
            assert status["client_count"] == 1
            assert status["send_timeouts_total"] == 1
        finally:
            slow.release.set()
            await publisher.detach(fast_id)
            await fast_sender

    asyncio.run(scenario())


def test_decoder_rejects_truncation_and_inconsistent_payload() -> None:
    with pytest.raises(ValueError, match="shorter"):
        decode_pointcloud_packet(b"MFPC")

    packet = bytearray(40)
    struct.pack_into("<4sBBHQQQII", packet, 0, b"MFPC", 1, 0, 40, 1, 1, 1, 1, 12)
    with pytest.raises(ValueError, match="length"):
        decode_pointcloud_packet(bytes(packet))
