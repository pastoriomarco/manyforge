"""Bounded binary point-cloud transport for the native scene viewer."""
from __future__ import annotations

import asyncio
import logging
import math
import os
import struct
import threading
import time
import uuid
from array import array
from dataclasses import dataclass, field
from typing import Any, Final, Optional, Sequence, TYPE_CHECKING

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .security import SecurityConfig, check_ws_bearer_token

if TYPE_CHECKING:
    from .state import ComposerState

_POINTCLOUD_MAGIC: Final = b"MFPC"
_POINTCLOUD_VERSION: Final = 1
_POINTCLOUD_FLAG_COLORS: Final = 1 << 0
_POINTCLOUD_FLAG_CLEAR: Final = 1 << 1
_POINTCLOUD_HEADER: Final = struct.Struct("<4sBBHQQQII")
_POINTCLOUD_SEND_TIMEOUT_S: Final = 0.25
_DEFAULT_MAX_POINTS: Final = 200_000
_PROTOCOL_MAX_POINTS: Final = 200_000

logger = logging.getLogger(__name__)


def _validated_max_points() -> int:
    raw = os.getenv("MANYFORGE_POINTCLOUD_MAX_POINTS", str(_DEFAULT_MAX_POINTS))
    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError("MANYFORGE_POINTCLOUD_MAX_POINTS must be an integer") from exc
    if value <= 0 or value > _PROTOCOL_MAX_POINTS:
        raise ValueError(
            f"MANYFORGE_POINTCLOUD_MAX_POINTS must be in [1, {_PROTOCOL_MAX_POINTS}]"
        )
    return value


def _qualification_fixture_enabled() -> bool:
    return os.getenv("MANYFORGE_POINTCLOUD_QUALIFICATION_FIXTURE", "").strip().lower() in {
        "1",
        "on",
        "true",
        "yes",
    }


def _qualification_fixture_shape() -> tuple[int, float]:
    try:
        point_count = int(
            os.getenv("MANYFORGE_POINTCLOUD_QUALIFICATION_POINTS", "20000")
        )
        rate_hz = float(
            os.getenv("MANYFORGE_POINTCLOUD_QUALIFICATION_RATE_HZ", "10")
        )
    except ValueError as exc:
        raise ValueError("point-cloud qualification fixture settings are invalid") from exc
    if point_count <= 0 or point_count > _PROTOCOL_MAX_POINTS:
        raise ValueError(
            "MANYFORGE_POINTCLOUD_QUALIFICATION_POINTS must be in "
            f"[1, {_PROTOCOL_MAX_POINTS}]"
        )
    if not math.isfinite(rate_hz) or rate_hz <= 0.0 or rate_hz > 30.0:
        raise ValueError(
            "MANYFORGE_POINTCLOUD_QUALIFICATION_RATE_HZ must be in (0, 30]"
        )
    return point_count, rate_hz


def _qualification_fixture_cloud(
    point_count: int,
) -> tuple[tuple[tuple[float, float, float], ...], tuple[tuple[float, float, float], ...]]:
    """Build one immutable, deterministic bounded cloud outside the hot loop."""
    width = max(1, math.ceil(math.sqrt(point_count)))
    denominator = max(1, width - 1)
    points: list[tuple[float, float, float]] = []
    colors: list[tuple[float, float, float]] = []
    for index in range(point_count):
        column = index % width
        row = index // width
        x = -0.5 + column / denominator
        y = -0.5 + row / denominator
        z = 0.45 + 0.02 * ((column + row) % 5)
        points.append((x, y, z))
        colors.append((column / denominator, row / denominator, 0.75))
    return tuple(points), tuple(colors)


def _pack_pointcloud_frame(
    *,
    stream_epoch: int,
    sequence: int,
    timestamp_ns: int,
    points: Sequence[Sequence[float]],
    colors: Optional[Sequence[Sequence[float]]],
    max_points: int,
) -> bytes:
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
    """Strict decoder used by tests and non-browser diagnostics."""
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


@dataclass
class _PointCloudClient:
    client_id: str
    websocket: WebSocket
    event: asyncio.Event = field(default_factory=asyncio.Event)
    pending: bytes | None = None
    sent: int = 0
    replaced: int = 0
    send_timeouts: int = 0
    send_errors: int = 0
    closed: bool = False


class PointCloudPublisher:
    """One-encode, capacity-one ingress and capacity-one per-client fanout."""

    def __init__(self, *, max_points: int | None = None) -> None:
        self._max_points = _validated_max_points() if max_points is None else int(max_points)
        if self._max_points <= 0 or self._max_points > _PROTOCOL_MAX_POINTS:
            raise ValueError("max_points is outside the supported range")
        self._stream_epoch = uuid.uuid4().int & ((1 << 64) - 1)
        self._encode_lock = threading.Lock()
        self._ingress_lock = threading.Lock()
        self._clients_lock = asyncio.Lock()
        self._clients: dict[str, _PointCloudClient] = {}
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._client_count = 0
        self._demand_epoch = 0
        self._sequence = 0
        self._latest_packet: bytes | None = None
        self._ingress_pending: tuple[int, bytes] | None = None
        self._ingress_scheduled = False
        self._published = 0
        self._clears = 0
        self._suppressed_no_clients = 0
        self._ingress_replacements = 0
        self._schedule_failures = 0
        self._superseded_demand = 0
        self._rejected = 0
        self._sent_total = 0
        self._client_replacements_total = 0
        self._send_timeouts_total = 0
        self._send_errors_total = 0

    async def attach(self, websocket: WebSocket, *, client_id: str | None = None) -> str:
        await websocket.accept()
        identifier = client_id or uuid.uuid4().hex
        loop = asyncio.get_running_loop()
        with self._ingress_lock:
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("point-cloud publisher cannot span event loops")
            self._loop = loop
        async with self._clients_lock:
            if identifier in self._clients:
                raise ValueError(f"point-cloud client already attached: {identifier}")
            client = _PointCloudClient(identifier, websocket)
            first_client = not self._clients
            self._clients[identifier] = client
            with self._ingress_lock:
                if first_client:
                    self._demand_epoch += 1
                    starter = None
                else:
                    starter = self._latest_packet
                self._client_count = len(self._clients)
            if starter is not None:
                client.pending = starter
                client.event.set()
        return identifier

    async def detach(self, client_id: str) -> None:
        async with self._clients_lock:
            client = self._clients.pop(client_id, None)
            if client is not None:
                client.closed = True
                client.event.set()
            with self._ingress_lock:
                self._client_count = len(self._clients)
                if self._client_count == 0:
                    # A future observer must not receive a frame produced for a
                    # prior demand lifetime. It will receive the next source
                    # publication after attaching.
                    self._demand_epoch += 1
                    self._latest_packet = None
                    self._ingress_pending = None

    def has_clients(self) -> bool:
        with self._ingress_lock:
            return self._client_count > 0

    def publish(
        self,
        points: Sequence[Sequence[float]],
        colors: Optional[Sequence[Sequence[float]]] = None,
    ) -> bool:
        """Encode and offer one frame, or suppress it when nobody observes."""
        with self._encode_lock:
            with self._ingress_lock:
                if self._client_count == 0 or self._loop is None:
                    self._suppressed_no_clients += 1
                    return False
                demand_epoch = self._demand_epoch
                sequence = self._sequence + 1
            try:
                packet = _pack_pointcloud_frame(
                    stream_epoch=self._stream_epoch,
                    sequence=sequence,
                    timestamp_ns=time.monotonic_ns(),
                    points=points,
                    colors=colors,
                    max_points=self._max_points,
                )
            except Exception:
                with self._ingress_lock:
                    self._rejected += 1
                raise
            with self._ingress_lock:
                if (
                    demand_epoch != self._demand_epoch
                    or self._client_count == 0
                    or self._loop is None
                ):
                    self._superseded_demand += 1
                    return False
                self._sequence = sequence
                self._latest_packet = packet
                self._published += 1
            return self._offer_threadsafe(packet, demand_epoch=demand_epoch)

    def clear(self) -> bool:
        with self._encode_lock:
            with self._ingress_lock:
                has_clients = self._client_count > 0 and self._loop is not None
                demand_epoch = self._demand_epoch
                sequence = self._sequence + 1
                self._clears += 1
                self._latest_packet = None
            if not has_clients:
                return False
            packet = _pack_pointcloud_clear(
                stream_epoch=self._stream_epoch,
                sequence=sequence,
                timestamp_ns=time.monotonic_ns(),
            )
            with self._ingress_lock:
                if (
                    demand_epoch != self._demand_epoch
                    or self._client_count == 0
                    or self._loop is None
                ):
                    self._superseded_demand += 1
                    return False
                self._sequence = sequence
            return self._offer_threadsafe(packet, demand_epoch=demand_epoch)

    def _offer_threadsafe(self, packet: bytes, *, demand_epoch: int) -> bool:
        schedule = False
        with self._ingress_lock:
            loop = self._loop
            if (
                loop is None
                or loop.is_closed()
                or self._client_count == 0
                or demand_epoch != self._demand_epoch
            ):
                self._superseded_demand += 1
                return False
            if self._ingress_pending is not None:
                self._ingress_replacements += 1
            self._ingress_pending = (demand_epoch, packet)
            if not self._ingress_scheduled:
                self._ingress_scheduled = True
                schedule = True
        if not schedule:
            return True
        try:
            loop.call_soon_threadsafe(self._start_ingress_drain)
        except RuntimeError:
            with self._ingress_lock:
                self._ingress_pending = None
                self._ingress_scheduled = False
                self._schedule_failures += 1
            return False
        return True

    def _start_ingress_drain(self) -> None:
        asyncio.create_task(self._drain_ingress())

    async def _drain_ingress(self) -> None:
        while True:
            with self._ingress_lock:
                pending = self._ingress_pending
                self._ingress_pending = None
                if pending is None:
                    self._ingress_scheduled = False
                    return
            demand_epoch, packet = pending
            async with self._clients_lock:
                with self._ingress_lock:
                    if demand_epoch != self._demand_epoch:
                        self._superseded_demand += 1
                        continue
                replacements = 0
                for client in self._clients.values():
                    if client.closed:
                        continue
                    if client.pending is not None:
                        client.replaced += 1
                        replacements += 1
                    client.pending = packet
                    client.event.set()
            if replacements:
                with self._ingress_lock:
                    self._client_replacements_total += replacements

    async def send_loop(self, client_id: str) -> None:
        while True:
            async with self._clients_lock:
                client = self._clients.get(client_id)
                if client is None or client.closed:
                    return
                event = client.event
            await event.wait()
            async with self._clients_lock:
                client = self._clients.get(client_id)
                if client is None or client.closed:
                    return
                packet = client.pending
                client.pending = None
                client.event.clear()
            if packet is None:
                continue
            try:
                await asyncio.wait_for(
                    client.websocket.send_bytes(packet),
                    timeout=_POINTCLOUD_SEND_TIMEOUT_S,
                )
                async with self._clients_lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.sent += 1
                with self._ingress_lock:
                    self._sent_total += 1
            except asyncio.TimeoutError:
                async with self._clients_lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.send_timeouts += 1
                with self._ingress_lock:
                    self._send_timeouts_total += 1
                await self.detach(client_id)
                raise
            except Exception:
                async with self._clients_lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.send_errors += 1
                with self._ingress_lock:
                    self._send_errors_total += 1
                await self.detach(client_id)
                raise

    async def status(self) -> dict[str, Any]:
        async with self._clients_lock:
            clients = {
                client_id: {
                    "pending": client.pending is not None,
                    "sent": client.sent,
                    "replaced": client.replaced,
                    "send_timeouts": client.send_timeouts,
                    "send_errors": client.send_errors,
                }
                for client_id, client in self._clients.items()
            }
        with self._ingress_lock:
            return {
                "schema_version": "manyforge.viewer.pointcloud_status.v1",
                "stream_epoch": self._stream_epoch,
                "demand_epoch": self._demand_epoch,
                "sequence": self._sequence,
                "max_points": self._max_points,
                "client_count": self._client_count,
                "published": self._published,
                "clears": self._clears,
                "suppressed_no_clients": self._suppressed_no_clients,
                "rejected": self._rejected,
                "sent_total": self._sent_total,
                "client_replacements_total": self._client_replacements_total,
                "send_timeouts_total": self._send_timeouts_total,
                "send_errors_total": self._send_errors_total,
                "ingress_pending": self._ingress_pending is not None,
                "ingress_replacements": self._ingress_replacements,
                "schedule_failures": self._schedule_failures,
                "superseded_demand": self._superseded_demand,
                "clients": clients,
            }


class _PointCloudQualificationFixture:
    """Opt-in NRT source for repeatable browser qualification only."""

    def __init__(
        self,
        publisher: PointCloudPublisher,
        *,
        point_count: int,
        rate_hz: float,
    ) -> None:
        self._publisher = publisher
        self._point_count = point_count
        self._rate_hz = rate_hz
        self._points, self._colors = _qualification_fixture_cloud(point_count)
        self._task: asyncio.Task[None] | None = None
        self._frames_attempted = 0
        self._frames_published = 0
        self._failures = 0

    def start_if_needed(self) -> None:
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(
                self._run(),
                name="manyforge-pointcloud-qualification-fixture",
            )

    async def _run(self) -> None:
        period_s = 1.0 / self._rate_hz
        current = asyncio.current_task()
        try:
            while self._publisher.has_clients():
                started = time.monotonic()
                self._frames_attempted += 1
                try:
                    published = await asyncio.to_thread(
                        self._publisher.publish,
                        self._points,
                        self._colors,
                    )
                    if published:
                        self._frames_published += 1
                except Exception:
                    self._failures += 1
                    logger.exception("Point-cloud qualification fixture publish failed")
                remaining_s = period_s - (time.monotonic() - started)
                if remaining_s > 0.0:
                    await asyncio.sleep(remaining_s)
        finally:
            # No await separates the final demand check from this assignment,
            # so an attach either keeps this task alive or observes None and
            # starts the next demand lifetime. Cancellation never respawns work.
            if self._task is current:
                self._task = None

    def status(self) -> dict[str, Any]:
        return {
            "enabled": True,
            "running": self._task is not None and not self._task.done(),
            "point_count": self._point_count,
            "rate_hz": self._rate_hz,
            "frames_attempted": self._frames_attempted,
            "frames_published": self._frames_published,
            "failures": self._failures,
        }

    async def wait_stopped(self, *, timeout_s: float = 1.0) -> None:
        task = self._task
        if task is not None:
            await asyncio.wait_for(asyncio.shield(task), timeout=timeout_s)


def create_pointcloud_router(
    state: "ComposerState",
    security: SecurityConfig,
) -> APIRouter:
    router = APIRouter()
    publisher = PointCloudPublisher()
    qualification_fixture: _PointCloudQualificationFixture | None = None
    if _qualification_fixture_enabled():
        point_count, rate_hz = _qualification_fixture_shape()
        qualification_fixture = _PointCloudQualificationFixture(
            publisher,
            point_count=point_count,
            rate_hz=rate_hz,
        )
        logger.warning(
            "Point-cloud qualification fixture enabled: %d points at %.3f Hz",
            point_count,
            rate_hz,
        )
    state.point_cloud_publisher = publisher  # type: ignore[attr-defined]

    @router.get("/api/scene/pointcloud/status")
    async def pointcloud_status() -> dict[str, Any]:
        status = await publisher.status()
        status["qualification_fixture"] = (
            qualification_fixture.status()
            if qualification_fixture is not None
            else {"enabled": False, "running": False}
        )
        return status

    @router.websocket("/ws/scene/pointcloud")
    async def ws_pointcloud(websocket: WebSocket) -> None:
        if security.api_token and not check_ws_bearer_token(
            websocket,
            security.api_token,
        ):
            await websocket.close(code=4401, reason="Unauthorized")
            return
        client_id = await publisher.attach(websocket)
        if qualification_fixture is not None:
            qualification_fixture.start_if_needed()
        send_task = asyncio.create_task(publisher.send_loop(client_id))

        async def receive_until_disconnect() -> None:
            while True:
                await websocket.receive_text()

        receive_task = asyncio.create_task(receive_until_disconnect())
        try:
            await asyncio.wait(
                {send_task, receive_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
        except WebSocketDisconnect:
            pass
        finally:
            await publisher.detach(client_id)
            for task in (send_task, receive_task):
                task.cancel()
            await asyncio.gather(send_task, receive_task, return_exceptions=True)
            try:
                await websocket.close()
            except Exception:
                pass

    return router


__all__ = [
    "PointCloudPublisher",
    "create_pointcloud_router",
    "decode_pointcloud_packet",
]
