"""Demand-scoped MFOB point-cloud ingress and capacity-one browser fanout."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import hashlib
import os
import threading
from typing import Any, TYPE_CHECKING
import uuid

from fastapi import APIRouter, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.pointcloud_observation import (
    POINTCLOUD_DEFAULT_OWNER_ID,
    POINTCLOUD_MAX_PACKET_BYTES,
    POINTCLOUD_MEDIA_TYPE,
    POINTCLOUD_PROTOCOL_ID,
    POINTCLOUD_RECEIPT_SCHEMA,
    POINTCLOUD_STATUS_SCHEMA,
    PointCloudObservation,
    decode_pointcloud_observation,
)

from .security import SecurityConfig
from .viewer_data_plane import BoundedViewerFanout, ViewerTransportItem

if TYPE_CHECKING:
    from .state import ComposerState


@dataclass(frozen=True, slots=True)
class _AcceptedSourceFrame:
    owner_epoch: str
    stream_generation: int
    sequence: int
    packet_sha256: str


def _configured_owner_id() -> str:
    value = os.environ.get("MANYFORGE_POINTCLOUD_OWNER_ID", POINTCLOUD_DEFAULT_OWNER_ID)
    if (
        not value
        or len(value.encode("utf-8", errors="strict")) > 128
        or value[0] not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        or any(
            character
            not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-"
            for character in value
        )
    ):
        raise ValueError("MANYFORGE_POINTCLOUD_OWNER_ID is invalid")
    return value


def _fresh_scene_cell(state: "ComposerState") -> tuple[str, str, int]:
    client = getattr(state, "headless_client", None)
    if client is None or not callable(getattr(client, "scene", None)):
        raise RuntimeError("pointcloud_scene_owner_unavailable")
    try:
        projection = client.scene()
    except HeadlessClientError as error:
        raise RuntimeError(error.code) from error
    except Exception as error:
        raise RuntimeError("pointcloud_scene_owner_unavailable") from error
    if type(projection) is not dict:
        raise RuntimeError("pointcloud_scene_owner_stale")
    section = (
        projection.get("logical_scene")
    )
    payload = section.get("payload") if type(section) is dict else None
    snapshot = payload.get("snapshot") if type(payload) is dict else None
    if (
        projection.get("schema_version") != "manyforge.scene_projection.v1"
        or type(section) is not dict
        or section.get("available") is not True
        or section.get("freshness") != "fresh"
        or section.get("known_gaps") != []
        or type(section.get("source_owner_epoch")) is not str
        or not section["source_owner_epoch"]
        or type(section.get("source_generation")) is not int
        or isinstance(section.get("source_generation"), bool)
        or section["source_generation"] < 0
        or type(payload) is not dict
        or payload.get("api_version") != "manyforge.logical_scene_source.v1"
        or type(snapshot) is not dict
        or type(snapshot.get("cell_id")) is not str
        or not snapshot["cell_id"]
    ):
        raise RuntimeError("pointcloud_scene_owner_stale")
    return (
        snapshot["cell_id"],
        section["source_owner_epoch"],
        section["source_generation"],
    )


class PointCloudObservationService:
    """One explicit source owner and one capacity-one slot per browser."""

    def __init__(self, *, expected_owner_id: str | None = None) -> None:
        self._expected_owner_id = expected_owner_id or _configured_owner_id()
        self._fanout = BoundedViewerFanout(send_timeout_s=2.0)
        self._lock = threading.Lock()
        self._client_count = 0
        self._demand_revision = 0
        self._accepted: _AcceptedSourceFrame | None = None
        self._scene_owner_epoch: str | None = None
        self._scene_generation: int | None = None
        self._received_total = 0
        self._published_total = 0
        self._duplicates_total = 0
        self._suppressed_total = 0
        self._rejected_total = 0
        self._clear_total = 0
        self._bytes_total = 0

    async def attach(self, websocket: WebSocket, client_id: str) -> None:
        await self._fanout.attach(websocket, client_id=client_id)
        with self._lock:
            self._client_count += 1
            self._demand_revision += 1

    async def detach(self, client_id: str) -> None:
        await self._fanout.detach(client_id)
        with self._lock:
            if self._client_count > 0:
                self._client_count -= 1
                self._demand_revision += 1
            if self._client_count == 0:
                self._accepted = None

    async def send_loop(self, client_id: str) -> None:
        await self._fanout.send_loop(client_id)

    def _admit_sequence(
        self,
        observation: PointCloudObservation,
        digest: str,
    ) -> str:
        current = self._accepted
        if current is None or observation.owner_epoch != current.owner_epoch:
            self._accepted = _AcceptedSourceFrame(
                observation.owner_epoch,
                observation.stream_generation,
                observation.sequence,
                digest,
            )
            return "accepted"
        if observation.stream_generation < current.stream_generation:
            return "stream_generation_regressed"
        if observation.stream_generation > current.stream_generation:
            self._accepted = _AcceptedSourceFrame(
                observation.owner_epoch,
                observation.stream_generation,
                observation.sequence,
                digest,
            )
            return "accepted"
        if observation.sequence < current.sequence:
            return "sequence_regressed"
        if observation.sequence == current.sequence:
            return "duplicate" if digest == current.packet_sha256 else "sequence_conflict"
        self._accepted = _AcceptedSourceFrame(
            observation.owner_epoch,
            observation.stream_generation,
            observation.sequence,
            digest,
        )
        return "accepted"

    async def ingest(
        self,
        state: "ComposerState",
        packet: bytes,
    ) -> tuple[str, PointCloudObservation]:
        observation = decode_pointcloud_observation(packet)
        with self._lock:
            self._received_total += 1
        if observation.owner_id != self._expected_owner_id:
            with self._lock:
                self._rejected_total += 1
            return "source_owner_mismatch", observation
        try:
            cell_id, scene_epoch, scene_generation = _fresh_scene_cell(state)
        except RuntimeError:
            with self._lock:
                self._rejected_total += 1
            return "scene_owner_unavailable", observation
        if observation.cell_id != cell_id:
            with self._lock:
                self._rejected_total += 1
            return "cell_identity_mismatch", observation
        digest = hashlib.sha256(packet).hexdigest()
        with self._lock:
            self._scene_owner_epoch = scene_epoch
            self._scene_generation = scene_generation
            if self._client_count == 0:
                self._suppressed_total += 1
                return "suppressed_no_observer", observation
            admission = self._admit_sequence(observation, digest)
            if admission == "duplicate":
                self._duplicates_total += 1
                return admission, observation
            if admission != "accepted":
                self._rejected_total += 1
                return admission, observation
            self._published_total += 1
            self._bytes_total += len(packet)
            if observation.clear:
                self._clear_total += 1
        await self._fanout.publish(
            ViewerTransportItem(sequence=observation.sequence, payload=packet)
        )
        return "accepted", observation

    async def status(self) -> dict[str, Any]:
        fanout = await self._fanout.status()
        with self._lock:
            accepted = self._accepted
            status = {
                "schema_version": POINTCLOUD_STATUS_SCHEMA,
                "protocol": POINTCLOUD_PROTOCOL_ID,
                "expected_owner_id": self._expected_owner_id,
                "demand_active": self._client_count > 0,
                "demand_revision": self._demand_revision,
                "client_count": self._client_count,
                "source_owner_epoch": None if accepted is None else accepted.owner_epoch,
                "stream_generation": None if accepted is None else accepted.stream_generation,
                "sequence": None if accepted is None else accepted.sequence,
                "scene_owner_epoch": self._scene_owner_epoch,
                "scene_generation": self._scene_generation,
                "received_total": self._received_total,
                "published_total": self._published_total,
                "duplicates_total": self._duplicates_total,
                "suppressed_no_observer_total": self._suppressed_total,
                "rejected_total": self._rejected_total,
                "clear_total": self._clear_total,
                "bytes_total": self._bytes_total,
            }
        status["fanout"] = fanout
        return status


def get_pointcloud_observation_service(
    state: "ComposerState",
) -> PointCloudObservationService:
    service = getattr(state, "_pointcloud_observation_service", None)
    if isinstance(service, PointCloudObservationService):
        return service
    service = PointCloudObservationService()
    setattr(state, "_pointcloud_observation_service", service)
    return service


def create_pointcloud_router(
    state: "ComposerState",
    security: SecurityConfig,
) -> APIRouter:
    del security
    service = get_pointcloud_observation_service(state)
    router = APIRouter()

    @router.get("/api/scene/pointcloud/status")
    async def pointcloud_status() -> dict[str, Any]:
        return await service.status()

    @router.post("/api/scene/pointcloud/observations")
    async def publish_pointcloud(request: Request) -> JSONResponse:
        content_type = request.headers.get("content-type", "").split(";", 1)[0]
        if content_type.strip().lower() != POINTCLOUD_MEDIA_TYPE:
            return JSONResponse(
                status_code=415,
                content={"detail": {"code": "pointcloud_media_type_invalid"}},
            )
        body = bytearray()
        async for chunk in request.stream():
            if len(body) + len(chunk) > POINTCLOUD_MAX_PACKET_BYTES:
                return JSONResponse(
                    status_code=413,
                    content={"detail": {"code": "pointcloud_packet_oversized"}},
                )
            body.extend(chunk)
        try:
            disposition, observation = await service.ingest(state, bytes(body))
        except ValueError as error:
            return JSONResponse(
                status_code=422,
                content={
                    "detail": {
                        "code": "pointcloud_observation_invalid",
                        "message": str(error),
                    }
                },
            )
        if disposition in {
            "source_owner_mismatch",
            "scene_owner_unavailable",
            "cell_identity_mismatch",
            "stream_generation_regressed",
            "sequence_regressed",
            "sequence_conflict",
        }:
            return JSONResponse(
                status_code=409,
                content={"detail": {"code": f"pointcloud_{disposition}"}},
            )
        return JSONResponse(
            status_code=202 if disposition == "suppressed_no_observer" else 200,
            content={
                "schema_version": POINTCLOUD_RECEIPT_SCHEMA,
                "disposition": disposition,
                "owner_epoch": observation.owner_epoch,
                "stream_generation": observation.stream_generation,
                "sequence": observation.sequence,
            },
        )

    @router.websocket("/ws/scene/pointcloud")
    async def pointcloud_observer(websocket: WebSocket) -> None:
        await websocket.accept()
        client_id = uuid.uuid4().hex
        await service.attach(websocket, client_id)
        send_task = asyncio.create_task(service.send_loop(client_id))

        async def receive_until_disconnect() -> None:
            while True:
                message = await websocket.receive_text()
                if message != "ping":
                    await websocket.close(code=1003, reason="unsupported message")
                    return
                await websocket.send_text("pong")

        receive_task = asyncio.create_task(receive_until_disconnect())
        try:
            await asyncio.wait(
                {send_task, receive_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
        except WebSocketDisconnect:
            pass
        finally:
            await service.detach(client_id)
            for task in (send_task, receive_task):
                task.cancel()
            await asyncio.gather(send_task, receive_task, return_exceptions=True)

    return router


__all__ = [
    "PointCloudObservationService",
    "create_pointcloud_router",
    "get_pointcloud_observation_service",
]
