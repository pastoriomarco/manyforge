"""Payload-neutral capacity-one snapshot broker and fenced I9 route."""

from __future__ import annotations

import math
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Callable, Final, Optional, TYPE_CHECKING

from fastapi import APIRouter

from .security import SecurityConfig

if TYPE_CHECKING:
    from .state import ComposerState


SNAPSHOT_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_snapshot_client_unavailable"
)
SNAPSHOT_RESTORATION_GATE: Final = "I9"


@dataclass(frozen=True)
class SnapshotRequest:
    """Transport-neutral request delivered to one already-bound observer."""

    request_id: str
    width: int
    height: int
    media_type: str
    deadline_ns: int


SnapshotSender = Callable[[SnapshotRequest], None]


class SnapshotBroker:
    """Capacity-one request/result rendezvous with one absolute deadline.

    The broker defines no WebSocket message, viewer generation fields, runtime
    state callback, or behavior-tree provider. A future I9 adapter binds one
    non-blocking sender and translates ``SnapshotRequest`` to its frozen wire.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._sender: SnapshotSender | None = None
        self._capture_admitted = False
        self._pending: dict[str, threading.Event] = {}
        self._results: dict[str, Optional[bytes]] = {}
        self._requests_total = 0
        self._completed_total = 0
        self._failed_total = 0
        self._timeouts_total = 0
        self._unavailable_total = 0
        self._busy_rejected_total = 0
        self._dispatch_failed_total = 0
        self._deliveries_rejected_total = 0
        self._bytes_total = 0
        self._last_completion_ns: int | None = None

    def bind_sender(self, sender: SnapshotSender) -> None:
        if not callable(sender):
            raise TypeError("snapshot sender must be callable")
        with self._lock:
            if self._capture_admitted or self._pending:
                raise RuntimeError("cannot replace snapshot sender while active")
            self._sender = sender

    def unbind_sender(self, sender: SnapshotSender) -> None:
        with self._lock:
            if self._sender is sender:
                self._sender = None
                for request_id, event in self._pending.items():
                    self._results[request_id] = None
                    event.set()

    def has_observer(self) -> bool:
        with self._lock:
            return self._sender is not None

    def render_sync(
        self,
        width: int,
        height: int,
        format: str,
        *,
        timeout_s: float = 2.0,
    ) -> Optional[bytes]:
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
        media_type = str(format).strip().lower()
        if not media_type or len(media_type) > 64:
            raise ValueError("snapshot media type is invalid")
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
            sender = self._sender
            if sender is None:
                self._unavailable_total += 1
                self._failed_total += 1
                return None
            self._capture_admitted = True
            request_id = uuid.uuid4().hex
            event = threading.Event()
            self._pending[request_id] = event

        request = SnapshotRequest(
            request_id=request_id,
            width=width,
            height=height,
            media_type=media_type,
            deadline_ns=deadline_ns,
        )
        try:
            sender(request)
        except Exception:
            with self._lock:
                self._pending.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._dispatch_failed_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        remaining_s = max(0.0, (deadline_ns - time.monotonic_ns()) / 1e9)
        if not event.wait(remaining_s):
            with self._lock:
                self._pending.pop(request_id, None)
                self._results.pop(request_id, None)
                self._capture_admitted = False
                self._timeouts_total += 1
                self._failed_total += 1
                self._last_completion_ns = time.monotonic_ns()
            return None

        with self._lock:
            result = self._results.pop(request_id, None)
            self._pending.pop(request_id, None)
            self._capture_admitted = False
            self._last_completion_ns = time.monotonic_ns()
            if result is None:
                self._failed_total += 1
            else:
                self._completed_total += 1
                self._bytes_total += len(result)
            return result

    def deliver(self, request_id: str, image_bytes: Optional[bytes]) -> bool:
        with self._lock:
            event = self._pending.get(request_id)
            if event is None:
                self._deliveries_rejected_total += 1
                return False
            self._results[request_id] = image_bytes
            event.set()
            return True

    def status(self) -> dict[str, int | bool | None]:
        with self._lock:
            return {
                "observer_bound": self._sender is not None,
                "pending_count": len(self._pending),
                "requests_total": self._requests_total,
                "completed_total": self._completed_total,
                "failed_total": self._failed_total,
                "timeouts_total": self._timeouts_total,
                "unavailable_total": self._unavailable_total,
                "busy_rejected_total": self._busy_rejected_total,
                "dispatch_failed_total": self._dispatch_failed_total,
                "deliveries_rejected_total": self._deliveries_rejected_total,
                "bytes_total": self._bytes_total,
                "last_completion_ns": self._last_completion_ns,
            }


class SnapshotClientUnavailableError(RuntimeError):
    """Raised before snapshot routing is bound to a viewer owner."""


def create_snapshot_router(
    _state: "ComposerState",
    _security: SecurityConfig,
) -> APIRouter:
    raise SnapshotClientUnavailableError(
        f"{SNAPSHOT_CLIENT_UNAVAILABLE_REASON_CODE}: snapshot routing returns "
        "at I9 after binding to the new observer owner"
    )


__all__ = [
    "SnapshotBroker",
    "SnapshotClientUnavailableError",
    "SnapshotRequest",
    "create_snapshot_router",
]
