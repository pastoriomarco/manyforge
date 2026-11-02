"""Capacity-one browser snapshot client bound to Headless frame identity."""

from __future__ import annotations

import asyncio
import logging
import math
import re
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Callable, Final, Optional, TYPE_CHECKING

from fastapi import APIRouter, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse, Response
from starlette.concurrency import run_in_threadpool

from .security import SecurityConfig

if TYPE_CHECKING:
    from .state import ComposerState


SNAPSHOT_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_snapshot_client_unavailable"
)
_MAXIMUM_SNAPSHOT_BYTES: Final = 4 * 1024 * 1024
_MAXIMUM_SNAPSHOT_OBSERVERS: Final = 16
_REQUEST_ID: Final = re.compile(r"^[0-9a-f]{32}$")
_OWNER_EPOCH: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$")

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class SnapshotRequest:
    """Transport-neutral request delivered to one already-bound observer."""

    request_id: str
    width: int
    height: int
    media_type: str
    deadline_ns: int
    required_owner_epoch: str
    required_generation: int


@dataclass(frozen=True)
class SnapshotCapture:
    """One image and the exact browser-observed owner frame it depicts."""

    image_bytes: bytes
    media_type: str
    observed_owner_epoch: str
    observed_generation: int


SnapshotSender = Callable[[SnapshotRequest], None]
SnapshotPrepare = Callable[[float], tuple[str, int] | None]


class SnapshotBroker:
    """Capacity-one request/result rendezvous with one absolute deadline.

    The broker defines no runtime state. One injected preparation callback
    reads the current Headless frame identity, and one non-blocking sender
    translates the admitted request to the browser observer wire.
    """

    def __init__(self, *, prepare_capture: SnapshotPrepare | None = None) -> None:
        self._lock = threading.Lock()
        self._senders: list[SnapshotSender] = []
        self._active_sender: SnapshotSender | None = None
        self._prepare_capture = prepare_capture
        self._capture_admitted = False
        self._pending: dict[str, threading.Event] = {}
        self._requirements: dict[str, tuple[str, int, str]] = {}
        self._results: dict[str, Optional[SnapshotCapture]] = {}
        self._requests_total = 0
        self._completed_total = 0
        self._failed_total = 0
        self._timeouts_total = 0
        self._unavailable_total = 0
        self._busy_rejected_total = 0
        self._dispatch_failed_total = 0
        self._preparation_failed_total = 0
        self._deliveries_rejected_total = 0
        self._bytes_total = 0
        self._last_completion_ns: int | None = None

    def bind_sender(self, sender: SnapshotSender) -> None:
        if not callable(sender):
            raise TypeError("snapshot sender must be callable")
        with self._lock:
            if sender in self._senders:
                return
            if len(self._senders) >= _MAXIMUM_SNAPSHOT_OBSERVERS:
                raise RuntimeError("snapshot observer limit reached")
            self._senders.append(sender)

    def configure_prepare_capture(self, callback: SnapshotPrepare) -> None:
        if not callable(callback):
            raise TypeError("snapshot prepare callback must be callable")
        with self._lock:
            if self._capture_admitted or self._pending:
                raise RuntimeError("cannot replace snapshot preparation while active")
            self._prepare_capture = callback

    def unbind_sender(self, sender: SnapshotSender) -> None:
        with self._lock:
            if sender in self._senders:
                self._senders.remove(sender)
            if self._active_sender is sender:
                self._active_sender = None
                for request_id, event in self._pending.items():
                    self._results[request_id] = None
                    event.set()

    def has_observer(self) -> bool:
        with self._lock:
            return bool(self._senders)

    def capture_sync(
        self,
        width: int,
        height: int,
        format: str,
        *,
        timeout_s: float = 2.0,
    ) -> Optional[SnapshotCapture]:
        """Wait boundedly for one result; no intermediate request is queued."""
        if (
            isinstance(width, bool)
            or isinstance(height, bool)
            or not isinstance(width, int)
            or not isinstance(height, int)
            or width < 1
            or height < 1
            or width > 8192
            or height > 8192
        ):
            raise ValueError("snapshot dimensions must be in [1, 8192]")
        requested_format = str(format).strip().lower()
        if requested_format not in {"jpeg", "png", "image/jpeg", "image/png"}:
            raise ValueError("snapshot format must be jpeg or png")
        media_type = (
            "image/jpeg" if requested_format in {"jpeg", "image/jpeg"} else "image/png"
        )
        try:
            timeout = float(timeout_s)
        except (TypeError, ValueError) as exc:
            raise ValueError("snapshot timeout must be numeric") from exc
        if not math.isfinite(timeout) or timeout <= 0.0:
            raise ValueError("snapshot timeout must be finite and positive")
        deadline_ns = time.monotonic_ns() + int(timeout * 1_000_000_000)

        with self._lock:
            self._requests_total += 1
            if self._capture_admitted:
                self._busy_rejected_total += 1
                self._failed_total += 1
                return None
            sender = self._senders[-1] if self._senders else None
            if sender is None:
                self._unavailable_total += 1
                self._failed_total += 1
                return None
            self._capture_admitted = True
            self._active_sender = sender
            prepare_capture = self._prepare_capture

        if prepare_capture is None:
            with self._lock:
                self._capture_admitted = False
                self._active_sender = None
                self._preparation_failed_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None
        remaining_s = max(0.0, (deadline_ns - time.monotonic_ns()) / 1e9)
        try:
            required_frame = prepare_capture(remaining_s)
        except Exception:
            logger.exception("snapshot frame preparation failed")
            required_frame = None
        if (
            required_frame is None
            or type(required_frame) is not tuple
            or len(required_frame) != 2
            or type(required_frame[0]) is not str
            or not required_frame[0]
            or type(required_frame[1]) is not int
            or isinstance(required_frame[1], bool)
            or required_frame[1] < 0
        ):
            with self._lock:
                self._capture_admitted = False
                self._active_sender = None
                self._preparation_failed_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        with self._lock:
            if sender not in self._senders:
                self._capture_admitted = False
                self._active_sender = None
                self._unavailable_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
                return None
            request_id = uuid.uuid4().hex
            event = threading.Event()
            self._pending[request_id] = event
            self._requirements[request_id] = (
                required_frame[0],
                required_frame[1],
                media_type,
            )

        request = SnapshotRequest(
            request_id=request_id,
            width=width,
            height=height,
            media_type=media_type,
            deadline_ns=deadline_ns,
            required_owner_epoch=required_frame[0],
            required_generation=required_frame[1],
        )
        try:
            sender(request)
        except Exception:
            with self._lock:
                self._pending.pop(request_id, None)
                self._requirements.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._active_sender = None
                self._dispatch_failed_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        remaining_s = max(0.0, (deadline_ns - time.monotonic_ns()) / 1e9)
        if not event.wait(remaining_s):
            with self._lock:
                # Delivery and the deadline may become ready concurrently. A
                # result already settled under the broker lock wins.
                if request_id in self._results:
                    result = self._results.pop(request_id)
                    self._pending.pop(request_id, None)
                    self._requirements.pop(request_id, None)
                    self._capture_admitted = False
                    self._active_sender = None
                    self._last_completion_ns = time.monotonic_ns()
                    if result is None:
                        self._failed_total += 1
                    else:
                        self._completed_total += 1
                        self._bytes_total += len(result.image_bytes)
                    return result
                self._pending.pop(request_id, None)
                self._requirements.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._active_sender = None
                self._timeouts_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        with self._lock:
            result = self._results.pop(request_id, None)
            self._pending.pop(request_id, None)
            self._requirements.pop(request_id, None)
            self._capture_admitted = False
            self._active_sender = None
            self._last_completion_ns = time.monotonic_ns()
            if result is None:
                self._failed_total += 1
            else:
                self._completed_total += 1
                self._bytes_total += len(result.image_bytes)
            return result

    def render_sync(
        self,
        width: int,
        height: int,
        format: str,
        *,
        timeout_s: float = 2.0,
    ) -> Optional[bytes]:
        """Compatibility surface for transport-neutral non-HTTP callers."""
        capture = self.capture_sync(
            width,
            height,
            format,
            timeout_s=timeout_s,
        )
        return None if capture is None else capture.image_bytes

    def deliver(
        self,
        request_id: str,
        image_bytes: Optional[bytes],
        *,
        observed_owner_epoch: str | None = None,
        observed_generation: int | None = None,
    ) -> bool:
        with self._lock:
            event = self._pending.get(request_id)
            requirement = self._requirements.get(request_id)
            if event is None or requirement is None or request_id in self._results:
                self._deliveries_rejected_total += 1
                return False
            if image_bytes is None:
                self._results[request_id] = None
                event.set()
                return True
            required_epoch, required_generation, media_type = requirement
            if (
                type(image_bytes) is not bytes
                or not image_bytes
                or len(image_bytes) > _MAXIMUM_SNAPSHOT_BYTES
                or type(observed_owner_epoch) is not str
                or _OWNER_EPOCH.fullmatch(observed_owner_epoch) is None
                or type(observed_generation) is not int
                or isinstance(observed_generation, bool)
                or observed_owner_epoch != required_epoch
                or observed_generation < required_generation
                or (
                    media_type == "image/png"
                    and not image_bytes.startswith(b"\x89PNG\r\n\x1a\n")
                )
                or (
                    media_type == "image/jpeg"
                    and not image_bytes.startswith(b"\xff\xd8\xff")
                )
            ):
                self._deliveries_rejected_total += 1
                return False
            self._results[request_id] = SnapshotCapture(
                image_bytes=image_bytes,
                media_type=media_type,
                observed_owner_epoch=observed_owner_epoch,
                observed_generation=observed_generation,
            )
            event.set()
            return True

    def status(self) -> dict[str, int | bool | None]:
        with self._lock:
            return {
                "observer_bound": bool(self._senders),
                "observer_count": len(self._senders),
                "pending_count": len(self._pending),
                "requests_total": self._requests_total,
                "completed_total": self._completed_total,
                "failed_total": self._failed_total,
                "timeouts_total": self._timeouts_total,
                "unavailable_total": self._unavailable_total,
                "busy_rejected_total": self._busy_rejected_total,
                "dispatch_failed_total": self._dispatch_failed_total,
                "preparation_failed_total": self._preparation_failed_total,
                "deliveries_rejected_total": self._deliveries_rejected_total,
                "bytes_total": self._bytes_total,
                "last_completion_ns": self._last_completion_ns,
            }


class SnapshotClientUnavailableError(RuntimeError):
    """Raised before snapshot routing is bound to a viewer owner."""


def create_snapshot_router(
    state: "ComposerState",
    security: SecurityConfig,
) -> APIRouter:
    del security
    client = getattr(state, "headless_client", None)
    if client is None:
        raise SnapshotClientUnavailableError(
            f"{SNAPSHOT_CLIENT_UNAVAILABLE_REASON_CODE}: snapshot routing "
            "requires the Headless owner client"
        )

    def prepare_capture(timeout_s: float) -> tuple[str, int] | None:
        if not math.isfinite(timeout_s) or timeout_s <= 0.0:
            return None
        runtime = client.runtime()
        if (
            type(runtime) is not dict
            or runtime.get("schema_version")
            != "manyforge.headless_runtime_snapshot.v1"
            or runtime.get("complete") is not True
            or type(runtime.get("projection_owner_epoch")) is not str
            or not runtime["projection_owner_epoch"]
            or type(runtime.get("projection_generation")) is not int
            or isinstance(runtime.get("projection_generation"), bool)
            or runtime["projection_generation"] < 0
        ):
            return None
        return (
            runtime["projection_owner_epoch"],
            runtime["projection_generation"],
        )

    broker = getattr(state, "snapshot_broker", None)
    if not isinstance(broker, SnapshotBroker):
        broker = SnapshotBroker(prepare_capture=prepare_capture)
        setattr(state, "snapshot_broker", broker)
    else:
        broker.configure_prepare_capture(prepare_capture)
    router = APIRouter()

    @router.websocket("/ws/scene/snapshot")
    async def snapshot_observer(websocket: WebSocket) -> None:
        await websocket.accept()
        loop = asyncio.get_running_loop()

        def sender(request: SnapshotRequest) -> None:
            remaining_ms = max(
                1,
                int((request.deadline_ns - time.monotonic_ns()) / 1_000_000),
            )
            future = asyncio.run_coroutine_threadsafe(
                websocket.send_json(
                    {
                        "type": "snapshot_request",
                        "requestId": request.request_id,
                        "width": request.width,
                        "height": request.height,
                        "mime": request.media_type,
                        "requiredFrame": {
                            "ownerEpoch": request.required_owner_epoch,
                            "generation": request.required_generation,
                        },
                        "timeoutMs": remaining_ms,
                    }
                ),
                loop,
            )

            def settle_send(done: object) -> None:
                try:
                    done.result()  # type: ignore[attr-defined]
                except Exception:
                    broker.deliver(request.request_id, None)

            future.add_done_callback(settle_send)

        try:
            broker.bind_sender(sender)
        except RuntimeError:
            await websocket.close(code=1013, reason="snapshot observer already bound")
            return
        try:
            while True:
                message = await websocket.receive_text()
                if message != "ping":
                    await websocket.close(code=1003, reason="unsupported snapshot message")
                    return
                await websocket.send_text("pong")
        except WebSocketDisconnect:
            return
        finally:
            broker.unbind_sender(sender)

    @router.post("/api/scene/snapshot/upload/{request_id}")
    async def upload_snapshot(request_id: str, request: Request) -> JSONResponse:
        if _REQUEST_ID.fullmatch(request_id) is None:
            return JSONResponse(
                status_code=404,
                content={"detail": {"code": "snapshot_request_unknown"}},
            )
        body = bytearray()
        async for chunk in request.stream():
            if len(body) + len(chunk) > _MAXIMUM_SNAPSHOT_BYTES:
                return JSONResponse(
                    status_code=413,
                    content={"detail": {"code": "snapshot_image_oversized"}},
                )
            body.extend(chunk)
        image = bytes(body) if body else None
        observed_epoch = request.headers.get("x-manyforge-viewer-owner-epoch")
        raw_generation = request.headers.get("x-manyforge-viewer-generation")
        try:
            observed_generation = (
                int(raw_generation, 10) if raw_generation is not None else None
            )
        except ValueError:
            observed_generation = None
        if not broker.deliver(
            request_id,
            image,
            observed_owner_epoch=observed_epoch,
            observed_generation=observed_generation,
        ):
            return JSONResponse(
                status_code=409,
                content={"detail": {"code": "snapshot_delivery_rejected"}},
            )
        return JSONResponse(content={"ok": image is not None, "bytes": len(image or b"")})

    @router.get("/api/scene/snapshot/render")
    async def render_snapshot(
        width: int = Query(default=640, ge=64, le=1920),
        height: int = Query(default=480, ge=64, le=1080),
        format: str = Query(default="jpeg", pattern="^(jpeg|png)$"),
        timeout_ms: int = Query(default=2_000, ge=1, le=2_000),
    ) -> Response:
        capture = await run_in_threadpool(
            broker.capture_sync,
            width,
            height,
            format,
            timeout_s=timeout_ms / 1_000.0,
        )
        if capture is None:
            return JSONResponse(
                status_code=503,
                content={
                    "detail": {
                        "code": "snapshot_observer_unavailable",
                        "message": "no generation-matched browser snapshot was captured",
                    }
                },
            )
        return Response(
            content=capture.image_bytes,
            media_type=capture.media_type,
            headers={
                "Cache-Control": "no-store",
                "X-ManyForge-Viewer-Owner-Epoch": capture.observed_owner_epoch,
                "X-ManyForge-Viewer-Generation": str(capture.observed_generation),
            },
        )

    @router.get("/api/scene/snapshot/status")
    def snapshot_status() -> dict[str, object]:
        return {
            "schema_version": "manyforge.viewer_snapshot_status.v1",
            **broker.status(),
        }

    return router


__all__ = [
    "SnapshotBroker",
    "SnapshotCapture",
    "SnapshotClientUnavailableError",
    "SnapshotRequest",
    "create_snapshot_router",
]
