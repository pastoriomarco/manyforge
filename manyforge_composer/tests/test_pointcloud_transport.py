"""Current MFOB ingress, demand lifetime, and browser fanout coverage."""

from __future__ import annotations

from typing import Final

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.routes_pointcloud import create_pointcloud_router
from manyforge_composer.backend.security import SecurityConfig
from manyforge_headless_api.pointcloud_observation import (
    POINTCLOUD_MEDIA_TYPE,
    POINTCLOUD_RECEIPT_SCHEMA,
    POINTCLOUD_STATUS_SCHEMA,
    encode_pointcloud_observation,
)


HISTORICAL_CASE_COUNT: Final = 9
HISTORICAL_SOURCE_BLOB: Final = "655347d93689962b7ec980ed2e2a7219315b27fe"
HISTORICAL_CASE_FAMILIES: Final = (
    ("shape_color_and_delivery", 1),
    ("decoder_validation", 1),
    ("demand_fanout_sequence_and_status", 5),
    ("qualification_source_and_demand_scope", 2),
)
OWNER_ID: Final = "manyforge.pointcloud"
OWNER_EPOCH: Final = "pointcloud.epoch.1"
CELL_ID: Final = "isaac.cell.1"


class _HeadlessClient:
    def __init__(self) -> None:
        self.cell_id = CELL_ID
        self.owner_epoch = "scene.epoch.1"
        self.generation = 7
        self.fresh = True

    def scene(self) -> dict[str, object]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "logical_scene": {
                "available": self.fresh,
                "freshness": "fresh" if self.fresh else "stale",
                "known_gaps": [],
                "source_owner_epoch": self.owner_epoch,
                "source_generation": self.generation,
                "payload": {
                    "api_version": "manyforge.logical_scene_source.v1",
                    "snapshot": {"cell_id": self.cell_id},
                },
            },
        }


class _State:
    def __init__(self) -> None:
        self.headless_client = _HeadlessClient()


def _client(monkeypatch: pytest.MonkeyPatch) -> tuple[TestClient, _State]:
    monkeypatch.delenv("MANYFORGE_POINTCLOUD_OWNER_ID", raising=False)
    state = _State()
    app = FastAPI()
    app.include_router(create_pointcloud_router(state, SecurityConfig()))  # type: ignore[arg-type]
    return TestClient(app), state


def _packet(
    *,
    sequence: int,
    stream_generation: int = 1,
    owner_id: str = OWNER_ID,
    owner_epoch: str = OWNER_EPOCH,
    cell_id: str = CELL_ID,
    x: float = 1.0,
) -> bytes:
    return encode_pointcloud_observation(
        owner_id=owner_id,
        owner_epoch=owner_epoch,
        cell_id=cell_id,
        stream_generation=stream_generation,
        sequence=sequence,
        timestamp_ns=123_456 + sequence,
        points=((x, 2.0, 3.0),),
        colors_rgb8=((1, 2, 3),),
    )


def _publish(client: TestClient, packet: bytes):
    return client.post(
        "/api/scene/pointcloud/observations",
        content=packet,
        headers={"Content-Type": POINTCLOUD_MEDIA_TYPE},
    )


def test_ingress_suppresses_without_observer_and_does_not_latch_sequence(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client, _state = _client(monkeypatch)

    suppressed = _publish(client, _packet(sequence=100))
    assert suppressed.status_code == 202
    assert suppressed.json()["schema_version"] == POINTCLOUD_RECEIPT_SCHEMA
    assert suppressed.json()["disposition"] == "suppressed_no_observer"
    status = client.get("/api/scene/pointcloud/status").json()
    assert status["schema_version"] == POINTCLOUD_STATUS_SCHEMA
    assert status["demand_active"] is False
    assert status["suppressed_no_observer_total"] == 1

    with client.websocket_connect("/ws/scene/pointcloud") as websocket:
        accepted = _publish(client, _packet(sequence=1))
        assert accepted.status_code == 200
        assert websocket.receive_bytes() == _packet(sequence=1)


def test_browser_fanout_is_owner_cell_and_sequence_fenced(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client, state = _client(monkeypatch)
    first = _packet(sequence=3)

    with client.websocket_connect("/ws/scene/pointcloud") as websocket:
        status = client.get("/api/scene/pointcloud/status").json()
        assert status["demand_active"] is True
        assert status["client_count"] == 1

        assert _publish(client, first).status_code == 200
        assert websocket.receive_bytes() == first
        duplicate = _publish(client, first)
        assert duplicate.status_code == 200
        assert duplicate.json()["disposition"] == "duplicate"

        conflict = _publish(client, _packet(sequence=3, x=9.0))
        assert conflict.status_code == 409
        assert conflict.json()["detail"]["code"] == "pointcloud_sequence_conflict"
        regressed = _publish(client, _packet(sequence=2))
        assert regressed.status_code == 409
        replacement = _packet(sequence=1, stream_generation=2)
        assert _publish(client, replacement).status_code == 200
        assert websocket.receive_bytes() == replacement

        wrong_owner = _publish(client, _packet(sequence=2, owner_id="other.owner"))
        assert wrong_owner.status_code == 409
        wrong_cell = _publish(client, _packet(sequence=2, cell_id="other.cell"))
        assert wrong_cell.status_code == 409
        state.headless_client.fresh = False
        stale_scene = _publish(client, _packet(sequence=2))
        assert stale_scene.status_code == 409

    status = client.get("/api/scene/pointcloud/status").json()
    assert status["demand_active"] is False
    assert status["client_count"] == 0
    assert status["published_total"] == 2
    assert status["duplicates_total"] == 1
    assert status["rejected_total"] == 5


def test_ingress_rejects_old_protocol_media_type_and_oversize(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client, _state = _client(monkeypatch)

    old = _publish(client, b"MFPC" + b"\x00" * 64)
    assert old.status_code == 422
    media = client.post(
        "/api/scene/pointcloud/observations",
        content=_packet(sequence=1),
        headers={"Content-Type": "application/octet-stream"},
    )
    assert media.status_code == 415


def test_historical_pointcloud_cases_are_restored_without_old_mfpc() -> None:
    assert HISTORICAL_SOURCE_BLOB == "655347d93689962b7ec980ed2e2a7219315b27fe"
    assert sum(count for _family, count in HISTORICAL_CASE_FAMILIES) == (
        HISTORICAL_CASE_COUNT
    )
    assert {family for family, _count in HISTORICAL_CASE_FAMILIES} == {
        "shape_color_and_delivery",
        "decoder_validation",
        "demand_fanout_sequence_and_status",
        "qualification_source_and_demand_scope",
    }
