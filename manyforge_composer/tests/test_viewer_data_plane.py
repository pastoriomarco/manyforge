from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest

from manyforge_composer.backend.models import ViewerFrameV1, ViewerManifestV1
from manyforge_composer.backend.viewer_data_plane import (
    BoundedViewerFanout,
    ViewerDemandRegistry,
    ViewerProtocolEncoder,
    freeze_viewer_render_signature,
    viewer_effective_period_s,
)


_REPO_ROOT = Path(__file__).resolve().parents[2]
_PRESERVATION_FIXTURE = (
    _REPO_ROOT / "docs" / "reference" / "VIEWER_DATA_PLANE_V1.fixture.json"
)


def test_generated_preservation_fixture_is_strict_and_order_coherent() -> None:
    payload = json.loads(_PRESERVATION_FIXTURE.read_text(encoding="utf-8"))
    manifest = ViewerManifestV1.model_validate(payload["manifest"])
    frame = ViewerFrameV1.model_validate(payload["frame"])

    manifest_payload = {
        key: value
        for key, value in manifest.model_dump(mode="json").items()
        if key
        not in {
            "type",
            "schema_version",
            "producer_epoch",
            "manifest_revision",
            "content_hash",
        }
    }
    encoded = ViewerProtocolEncoder(producer_epoch=manifest.producer_epoch).encode(
        manifest_payload=manifest_payload,
        frame_payload={"fixture": True},
    )

    assert encoded is not None
    assert encoded.manifest["content_hash"] == manifest.content_hash
    assert frame.producer_epoch == manifest.producer_epoch
    assert frame.manifest_revision == manifest.manifest_revision
    assert len(frame.entity_transforms) == len(manifest.entities)
    assert len(frame.frame_transforms) == len(manifest.frames)
    assert [item.object_id for item in manifest.entities] == manifest.orders.entities
    assert [item.name for item in manifest.frames] == manifest.orders.frames
    assert [item.gripper_id for item in manifest.grippers] == manifest.orders.grippers
    assert [item.pose_source_id for item in manifest.pose_sources] == manifest.orders.pose_sources
    assert manifest.robots[0].link_order == manifest.orders.links
    assert len(frame.robot_blocks[0].link_transforms) == len(manifest.orders.links)


def test_render_signature_ignores_evidence_time_but_retains_pose_and_status() -> None:
    class ModelRecord:
        def __init__(self, payload: dict) -> None:
            self.payload = payload

        def model_dump(self, *, mode: str) -> dict:
            assert mode == "python"
            return self.payload

    first = {
        "status": "streaming",
        "received_at_ns": 100,
        "objects": [{
            "object_id": "target",
            "age_s": 0.01,
            "update": {
                "source_sequence": 1,
                "source_timestamp_ns": 90,
                "position_m": [0.1, 0.2, 0.3],
            },
        }],
    }
    later_same_pose = {
        "status": "streaming",
        "received_at_ns": 200,
        "objects": [{
            "object_id": "target",
            "age_s": 0.02,
            "update": {
                "source_sequence": 2,
                "source_timestamp_ns": 190,
                "position_m": [0.1, 0.2, 0.3],
            },
        }],
    }

    signature = freeze_viewer_render_signature([ModelRecord(first)])
    assert signature == freeze_viewer_render_signature([ModelRecord(later_same_pose)])

    moved = {
        **later_same_pose,
        "objects": [{
            **later_same_pose["objects"][0],
            "update": {
                **later_same_pose["objects"][0]["update"],
                "position_m": [0.2, 0.2, 0.3],
            },
        }],
    }
    assert signature != freeze_viewer_render_signature([ModelRecord(moved)])
    assert signature != freeze_viewer_render_signature([
        ModelRecord({**later_same_pose, "status": "stale"}),
    ])


class _FakeClock:
    def __init__(self) -> None:
        self.now_ns = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, seconds: float) -> None:
        self.now_ns += int(seconds * 1e9)


def test_viewer_demand_registry_clamps_rate_and_expires_crashed_client() -> None:
    clock = _FakeClock()
    changes: list[int] = []
    registry = ViewerDemandRegistry(
        lease_timeout_s=5.0,
        disconnect_grace_s=0.5,
        max_rate_hz=20.0,
        clock_ns=clock,
        on_change=lambda: changes.append(clock()),
    )

    summary = registry.attach(
        "client-a",
        visibility="focused",
        requested_rate_hz=100.0,
        layers=("measured_robot",),
    )
    assert summary.active is True
    assert summary.requested_rate_hz == 20.0
    assert summary.layers == frozenset({"canonical", "measured_robot"})

    clock.advance(4.9)
    assert registry.summary().active is True
    clock.advance(0.2)
    summary = registry.summary()
    assert summary.active is False
    assert summary.client_count == 0
    assert len(changes) == 2


def test_viewer_demand_hidden_and_collapsed_clients_do_not_request_frames() -> None:
    clock = _FakeClock()
    registry = ViewerDemandRegistry(clock_ns=clock)
    registry.attach(
        "hidden",
        visibility="hidden",
        requested_rate_hz=30.0,
        layers=("canonical", "diagnostics"),
    )
    registry.attach(
        "collapsed",
        visibility="collapsed",
        requested_rate_hz=30.0,
        layers=("canonical",),
    )
    summary = registry.summary()
    assert summary.client_count == 2
    assert summary.active_client_count == 0
    assert summary.active is False
    assert summary.requested_rate_hz == 0.0


def test_viewer_demand_disconnect_has_bounded_grace() -> None:
    clock = _FakeClock()
    registry = ViewerDemandRegistry(
        disconnect_grace_s=0.25,
        clock_ns=clock,
    )
    registry.attach("client", visibility="visible", requested_rate_hz=10.0)
    summary = registry.detach("client")
    assert summary.active is True
    clock.advance(0.3)
    assert registry.summary().active is False


def test_viewer_demand_rejects_unknown_layers_and_invalid_rate() -> None:
    registry = ViewerDemandRegistry()
    with pytest.raises(ValueError, match="unsupported viewer layer"):
        registry.attach("client", layers=("kernel_authority",))
    with pytest.raises(ValueError, match="finite"):
        registry.attach("client", requested_rate_hz=float("nan"))


def test_requested_rate_can_reduce_but_not_exceed_server_sampling_rate() -> None:
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=5.0,
    ) == pytest.approx(0.2)
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=30.0,
    ) == pytest.approx(1.0 / 15.0)
    assert viewer_effective_period_s(
        fastest_period_s=1.0 / 15.0,
        requested_rate_hz=0.0,
    ) is None


def test_protocol_encoder_reuses_manifest_revision_and_self_contains_frames() -> None:
    encoder = ViewerProtocolEncoder(producer_epoch="epoch-a")
    first = encoder.encode(
        manifest_payload={"world_frame": "universe", "objects": ["box"]},
        frame_payload={"timestamp_ns": 1, "object_transforms": [[0.0, 0.0, 0.0]]},
    )
    second = encoder.encode(
        manifest_payload={"objects": ["box"], "world_frame": "universe"},
        frame_payload={"timestamp_ns": 2, "object_transforms": [[1.0, 0.0, 0.0]]},
    )
    third = encoder.encode(
        manifest_payload={"world_frame": "universe", "objects": ["box", "wall"]},
        frame_payload={"timestamp_ns": 3, "object_transforms": []},
    )

    assert first is not None
    assert second is not None
    assert third is not None
    assert first.manifest_revision == second.manifest_revision == 1
    assert third.manifest_revision == 2
    assert [first.frame_sequence, second.frame_sequence, third.frame_sequence] == [1, 2, 3]
    assert [
        first.frame["render_revision"],
        second.frame["render_revision"],
        third.frame["render_revision"],
    ] == [1, 2, 3]
    assert second.frame["manifest_revision"] == 1
    assert second.frame["producer_epoch"] == "epoch-a"
    assert "NaN" not in second.frame_json


def test_protocol_encoder_does_not_publish_time_only_evidence() -> None:
    encoder = ViewerProtocolEncoder(producer_epoch="epoch")
    first = encoder.encode(
        manifest_payload={"objects": ["box"]},
        frame_payload={
            "timestamp_ns": 1,
            "source_sequence": 1,
            "pose_source_states": [
                {"status": "fresh", "received_at_ns": 10, "age_s": 0.1}
            ],
            "object_transforms": [[1.0, 2.0, 3.0]],
        },
    )
    second = encoder.encode(
        manifest_payload={"objects": ["box"]},
        frame_payload={
            "timestamp_ns": 2,
            "source_sequence": 2,
            "pose_source_states": [
                {"status": "fresh", "received_at_ns": 20, "age_s": 0.2}
            ],
            "object_transforms": [[1.0, 2.0, 3.0]],
        },
    )

    assert first is not None
    assert first.frame_sequence == 1
    assert first.frame["render_revision"] == 1
    assert second is None

    third = encoder.encode(
        manifest_payload={"objects": ["box"]},
        frame_payload={
            "timestamp_ns": 3,
            "source_sequence": 3,
            "pose_source_states": [
                {"status": "stale", "received_at_ns": 30, "age_s": 0.3}
            ],
            "object_transforms": [[1.0, 2.0, 3.0]],
        },
    )
    assert third is not None
    assert third.frame_sequence == 2
    assert third.frame["render_revision"] == 2


class _FakeWebSocket:
    def __init__(self) -> None:
        self.messages: list[str] = []
        self.allow_send = asyncio.Event()

    async def send_text(self, message: str) -> None:
        await self.allow_send.wait()
        self.messages.append(message)


def test_fanout_slow_client_keeps_only_latest_generation_and_resyncs_manifest() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout()
        websocket = _FakeWebSocket()
        await fanout.attach(websocket, client_id="slow")  # type: ignore[arg-type]
        encoder = ViewerProtocolEncoder(producer_epoch="epoch")
        first = encoder.encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 1},
        )
        second = encoder.encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 2},
        )
        third = encoder.encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 3},
        )
        await fanout.publish(first)
        await fanout.publish(second)
        await fanout.publish(third)

        sender = asyncio.create_task(fanout.send_loop("slow"))
        websocket.allow_send.set()
        for _ in range(20):
            if len(websocket.messages) >= 2:
                break
            await asyncio.sleep(0)
        status = await fanout.status()
        assert status["clients"]["slow"]["replaced_frames"] == 2
        assert status["replaced_frames_total"] == 2
        assert status["send_errors_total"] == 0
        assert status["send_timeouts_total"] == 0
        assert len(websocket.messages) == 2
        assert '"type":"manifest"' in websocket.messages[0]
        assert '"frame_sequence":3' in websocket.messages[1]

        await fanout.detach("slow")
        await sender

    asyncio.run(scenario())


def test_fanout_keeps_server_diagnostics_in_an_independent_capacity_one_slot() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout()
        websocket = _FakeWebSocket()
        await fanout.attach(websocket, client_id="client")  # type: ignore[arg-type]
        await fanout.publish_control("client", {"type": "lease_ack", "revision": 1})
        await fanout.publish_diagnostics("client", {"type": "diagnostics", "value": 1})
        await fanout.publish_diagnostics("client", {"type": "diagnostics", "value": 2})
        status = await fanout.status()
        assert status["clients"]["client"]["replaced_diagnostics"] == 1
        assert status["replaced_diagnostics_total"] == 1

        sender = asyncio.create_task(fanout.send_loop("client"))
        websocket.allow_send.set()
        for _ in range(20):
            if len(websocket.messages) >= 2:
                break
            await asyncio.sleep(0)
        assert [json.loads(item)["type"] for item in websocket.messages] == [
            "lease_ack",
            "diagnostics",
        ]
        assert json.loads(websocket.messages[1])["value"] == 2
        await fanout.detach("client")
        await sender

    asyncio.run(scenario())


def test_fanout_resends_manifest_when_worker_epoch_restarts_at_same_revision() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout()
        websocket = _FakeWebSocket()
        websocket.allow_send.set()
        await fanout.attach(websocket, client_id="restart")  # type: ignore[arg-type]
        sender = asyncio.create_task(fanout.send_loop("restart"))
        first = ViewerProtocolEncoder(producer_epoch="before").encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 1},
        )
        restarted = ViewerProtocolEncoder(producer_epoch="after").encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 2},
        )
        try:
            await fanout.publish(first)
            for _ in range(20):
                if len(websocket.messages) == 2:
                    break
                await asyncio.sleep(0)
            await fanout.publish(restarted)
            for _ in range(20):
                if len(websocket.messages) == 4:
                    break
                await asyncio.sleep(0)
            assert len(websocket.messages) == 4
            assert '"producer_epoch":"before"' in websocket.messages[0]
            assert '"producer_epoch":"after"' in websocket.messages[2]
            assert '"producer_epoch":"after"' in websocket.messages[3]
        finally:
            await fanout.detach("restart")
            await sender

    asyncio.run(scenario())


def test_threadsafe_ingress_is_capacity_one_before_event_loop_fanout() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout()
        websocket = _FakeWebSocket()
        await fanout.attach(websocket, client_id="client")  # type: ignore[arg-type]
        encoder = ViewerProtocolEncoder(producer_epoch="epoch")
        publications = [
            encoder.encode(
                manifest_payload={"objects": ["a"]},
                frame_payload={"value": value},
            )
            for value in range(3)
        ]

        # No await between offers: the loop cannot drain the crossing, so all
        # three calls exercise the capacity-one producer-to-loop slot.
        assert all(fanout.publish_threadsafe(item) for item in publications)
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        status = await fanout.status()
        assert status["ingress_replacements"] == 2
        assert status["published_frames"] == 1

        sender = asyncio.create_task(fanout.send_loop("client"))
        websocket.allow_send.set()
        for _ in range(20):
            if len(websocket.messages) >= 2:
                break
            await asyncio.sleep(0)
        assert '"frame_sequence":3' in websocket.messages[-1]
        await fanout.detach("client")
        await sender

    asyncio.run(scenario())


def test_cached_generation_offer_targets_only_the_new_client() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout()
        first_socket = _FakeWebSocket()
        second_socket = _FakeWebSocket()
        await fanout.attach(first_socket, client_id="first")  # type: ignore[arg-type]
        await fanout.attach(second_socket, client_id="second")  # type: ignore[arg-type]
        publication = ViewerProtocolEncoder(producer_epoch="epoch").encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 1},
        )

        await fanout.offer_to_client("second", publication)

        status = await fanout.status()
        assert status["clients"]["first"]["pending"] is False
        assert status["clients"]["second"]["pending"] is True

    asyncio.run(scenario())


def test_fanout_bounds_a_socket_send_that_never_completes() -> None:
    async def scenario() -> None:
        fanout = BoundedViewerFanout(send_timeout_s=0.01)
        websocket = _FakeWebSocket()
        await fanout.attach(websocket, client_id="blocked")  # type: ignore[arg-type]
        publication = ViewerProtocolEncoder(producer_epoch="epoch").encode(
            manifest_payload={"objects": ["a"]},
            frame_payload={"value": 1},
        )
        await fanout.offer_to_client("blocked", publication)

        with pytest.raises(asyncio.TimeoutError):
            await fanout.send_loop("blocked")
        status = await fanout.status()
        assert status["clients"]["blocked"]["send_timeouts"] == 1
        assert status["clients"]["blocked"]["sent_frames"] == 0

    asyncio.run(scenario())
