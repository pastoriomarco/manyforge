"""Focused payload-neutral viewer transport coverage during the I9 cutover."""

from __future__ import annotations

import asyncio

import pytest

from manyforge_composer.backend.routes_viewer import create_viewer_router
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.viewer_data_plane import (
    BoundedViewerFanout,
    ViewerDemandRegistry,
    ViewerTransportItem,
)
from manyforge_composer.backend.viewer_worker import (
    ViewerWorkerError,
    ViewerWorkerSupervisor,
    _decode_envelope,
    _encode_envelope,
)
from manyforge_composer.backend.visual_state import (
    VisualPose,
    compose_pose_in_world,
    relative_pose_from_world,
)
from manyforge_composer.backend.ws_state import StateHub


class _RecordingSocket:
    def __init__(self) -> None:
        self.text: list[str] = []
        self.binary: list[bytes] = []

    async def send_text(self, payload: str) -> None:
        self.text.append(payload)

    async def send_bytes(self, payload: bytes) -> None:
        self.binary.append(payload)


class _BlockingSocket:
    def __init__(self) -> None:
        self.release = asyncio.Event()

    async def send_text(self, _payload: str) -> None:
        await self.release.wait()

    async def send_bytes(self, _payload: bytes) -> None:
        await self.release.wait()


def test_demand_registry_clamps_rate_and_rejects_wire_specific_layer_shapes() -> None:
    registry = ViewerDemandRegistry(max_rate_hz=20.0)

    summary = registry.attach(
        "observer-a",
        requested_rate_hz=50.0,
        layers=("geometry", "telemetry"),
    )

    assert summary.active is True
    assert summary.requested_rate_hz == 20.0
    assert summary.layers == frozenset({"geometry", "telemetry"})


def test_fanout_keeps_only_latest_opaque_payload_per_client() -> None:
    async def scenario() -> None:
        socket = _RecordingSocket()
        fanout = BoundedViewerFanout()
        await fanout.attach(socket, client_id="observer-a")  # type: ignore[arg-type]
        await fanout.publish(ViewerTransportItem(sequence=1, payload="old"))
        await fanout.publish(ViewerTransportItem(sequence=2, payload=b"latest"))
        sender = asyncio.create_task(fanout.send_loop("observer-a"))
        try:
            for _ in range(100):
                if socket.binary:
                    break
                await asyncio.sleep(0)
            assert socket.text == []
            assert socket.binary == [b"latest"]
            status = await fanout.status()
            assert status["sent_total"] == 1
            assert status["replaced_total"] == 1
        finally:
            await fanout.detach("observer-a")
            await sender
        assert (await fanout.status())["sent_total"] == 1

    asyncio.run(scenario())


def test_fanout_detach_drops_pending_work_without_socket_io() -> None:
    async def scenario() -> None:
        socket = _RecordingSocket()
        fanout = BoundedViewerFanout()
        await fanout.attach(socket, client_id="observer-a")  # type: ignore[arg-type]
        await fanout.publish(ViewerTransportItem(sequence=1, payload="pending"))

        await fanout.detach("observer-a")
        await fanout.send_loop("observer-a")

        assert socket.text == []
        assert socket.binary == []
        assert (await fanout.status())["client_count"] == 0

    asyncio.run(scenario())


def test_fanout_threadsafe_ingress_is_capacity_one() -> None:
    async def scenario() -> None:
        socket = _RecordingSocket()
        fanout = BoundedViewerFanout()
        await fanout.attach(socket, client_id="observer-a")  # type: ignore[arg-type]
        publications = [
            ViewerTransportItem(sequence=index, payload=f"payload-{index}")
            for index in range(1, 4)
        ]

        assert all(fanout.publish_threadsafe(item) for item in publications)
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        status = await fanout.status()
        assert status["ingress_replacements"] == 2
        assert status["published"] == 1
        assert status["clients"]["observer-a"]["pending"] is True

        sender = asyncio.create_task(fanout.send_loop("observer-a"))
        try:
            for _ in range(100):
                if socket.text:
                    break
                await asyncio.sleep(0)
            assert socket.text == ["payload-3"]
        finally:
            await fanout.detach("observer-a")
            await sender

    asyncio.run(scenario())


def test_fanout_offer_targets_only_the_named_client() -> None:
    async def scenario() -> None:
        first = _RecordingSocket()
        second = _RecordingSocket()
        fanout = BoundedViewerFanout()
        await fanout.attach(first, client_id="first")  # type: ignore[arg-type]
        await fanout.attach(second, client_id="second")  # type: ignore[arg-type]

        await fanout.offer_to_client(
            "second",
            ViewerTransportItem(sequence=1, payload="cached"),
        )

        status = await fanout.status()
        assert status["clients"]["first"]["pending"] is False
        assert status["clients"]["second"]["pending"] is True
        await fanout.detach("first")
        await fanout.detach("second")

    asyncio.run(scenario())


def test_fanout_bounds_a_socket_send_that_never_completes() -> None:
    async def scenario() -> None:
        socket = _BlockingSocket()
        fanout = BoundedViewerFanout(send_timeout_s=0.001)
        await fanout.attach(socket, client_id="blocked")  # type: ignore[arg-type]
        await fanout.publish(ViewerTransportItem(sequence=1, payload="pending"))

        with pytest.raises(asyncio.TimeoutError):
            await fanout.send_loop("blocked")
        status = await fanout.status()
        assert status["clients"]["blocked"]["send_timeouts"] == 1
        assert status["clients"]["blocked"]["sent"] == 0
        assert status["send_timeouts_total"] == 1
        await fanout.detach("blocked")

    asyncio.run(scenario())


def test_worker_envelope_is_bounded_and_digest_checked_without_old_schema() -> None:
    envelope = _encode_envelope(
        schema="observer.job",
        request_id=7,
        payload={"value": 3},
        max_bytes=64,
    )
    assert _decode_envelope(
        envelope,
        expected_schema="observer.job",
        max_bytes=64,
    ) == (7, {"value": 3})

    length_tampered = dict(
        envelope,
        payload=envelope["payload"] + b" ",
    )
    with pytest.raises(ViewerWorkerError, match="payload length"):
        _decode_envelope(
            length_tampered,
            expected_schema="observer.job",
            max_bytes=64,
        )

    digest_tampered = dict(envelope, payload_sha256="0" * 64)
    with pytest.raises(ViewerWorkerError, match="checksum"):
        _decode_envelope(
            digest_tampered,
            expected_schema="observer.job",
            max_bytes=64,
        )


def test_visual_pose_composition_round_trips_without_runtime_or_fk_owner() -> None:
    parent = VisualPose(
        position_m=(1.0, 2.0, 3.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    child_in_parent = VisualPose(
        position_m=(0.5, -0.25, 0.75),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    child_world = compose_pose_in_world(
        parent_pose=parent,
        child_pose=child_in_parent,
    )

    assert relative_pose_from_world(
        child_pose=child_world,
        parent_pose=parent,
    ) == child_in_parent


def test_viewer_factories_fail_before_state_or_process_effects() -> None:
    class PoisonState:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"state was accessed: {name}")

    with pytest.raises(RuntimeError, match="composable_cell_viewer_client_unavailable"):
        create_viewer_router(PoisonState(), SecurityConfig())  # type: ignore[arg-type]
    with pytest.raises(RuntimeError, match="composable_cell_viewer_worker_unavailable"):
        ViewerWorkerSupervisor()
    with pytest.raises(
        RuntimeError,
        match="composable_cell_state_projection_unavailable",
    ):
        StateHub().start(PoisonState())  # type: ignore[arg-type]
