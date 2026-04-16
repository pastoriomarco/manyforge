"""Non-blocking cached scene snapshot service for HMI-backed image consumers."""

from __future__ import annotations

import logging
import threading
import time
from concurrent.futures import Future
from dataclasses import dataclass
from typing import Callable, Optional

logger = logging.getLogger(__name__)


class SceneSnapshotUnavailableError(RuntimeError):
    """Raised when async scene snapshot capture has been disabled."""


@dataclass(frozen=True)
class SnapshotSpec:
    width: int
    height: int
    format: str


@dataclass
class CachedSnapshot:
    image_bytes: bytes
    captured_ns: int


class SceneSnapshotService:
    """Serve cached scene snapshots without blocking the caller.

    The underlying render function may block indefinitely on browser/HMI state.
    This service never waits for it on the caller thread. Instead, it submits at
    most one background capture at a time and returns the latest fresh cached
    frame when available.

    If a capture exceeds ``capture_timeout_s``, the service disables further
    capture attempts until restart. This prevents repeated submissions into a
    worker that is already hung.
    """

    def __init__(
        self,
        render_fn: Callable[[int, int, str], Optional[bytes]],
        *,
        max_age_s: float = 2.0,
        request_cooldown_s: float = 0.25,
        capture_timeout_s: float = 1.0,
    ) -> None:
        self._render_fn = render_fn
        self._max_age_ns = int(max_age_s * 1_000_000_000)
        self._request_cooldown_ns = int(request_cooldown_s * 1_000_000_000)
        self._capture_timeout_ns = int(capture_timeout_s * 1_000_000_000)
        self._lock = threading.Lock()
        self._cache: dict[SnapshotSpec, CachedSnapshot] = {}
        self._inflight: dict[SnapshotSpec, tuple[Future[Optional[bytes]], int]] = {}
        self._last_request_ns: dict[SnapshotSpec, int] = {}
        self._disabled_reason: Optional[str] = None

    def shutdown(self) -> None:
        return

    @property
    def disabled_reason(self) -> Optional[str]:
        with self._lock:
            return self._disabled_reason

    def get_snapshot(self, width: int, height: int, format: str) -> Optional[bytes]:
        """Return a fresh cached snapshot, scheduling a background capture if needed."""
        spec = SnapshotSpec(width=int(width), height=int(height), format=str(format))
        now_ns = time.monotonic_ns()
        with self._lock:
            self._harvest_locked(now_ns)
            cached = self._cache.get(spec)
            if cached is not None and now_ns - cached.captured_ns <= self._max_age_ns:
                return bytes(cached.image_bytes)

            if self._disabled_reason is not None:
                raise SceneSnapshotUnavailableError(self._disabled_reason)

            inflight = self._inflight.get(spec)
            if inflight is None:
                last_request_ns = self._last_request_ns.get(spec, 0)
                if now_ns - last_request_ns >= self._request_cooldown_ns:
                    self._last_request_ns[spec] = now_ns
                    future: Future[Optional[bytes]] = Future()

                    def _capture() -> None:
                        try:
                            image_bytes = self._render_fn(
                                spec.width,
                                spec.height,
                                spec.format,
                            )
                        except Exception as exc:
                            future.set_exception(exc)
                            return
                        future.set_result(image_bytes)

                    thread = threading.Thread(
                        target=_capture,
                        name="manyforge_snapshot_capture",
                        daemon=True,
                    )
                    thread.start()
                    self._inflight[spec] = (future, now_ns)
            return None

    def _harvest_locked(self, now_ns: int) -> None:
        for spec, (future, started_ns) in list(self._inflight.items()):
            if future.done():
                self._inflight.pop(spec, None)
                try:
                    image_bytes = future.result()
                except Exception:
                    logger.warning(
                        "Scene snapshot capture failed for %sx%s %s",
                        spec.width,
                        spec.height,
                        spec.format,
                        exc_info=True,
                    )
                    continue
                if image_bytes:
                    self._cache[spec] = CachedSnapshot(
                        image_bytes=bytes(image_bytes),
                        captured_ns=now_ns,
                    )
                continue

            if now_ns - started_ns > self._capture_timeout_ns:
                self._inflight.pop(spec, None)
                if self._disabled_reason is None:
                    self._disabled_reason = (
                        f"scene snapshot capture exceeded {self._capture_timeout_ns / 1_000_000_000:.2f}s"
                    )
                    logger.error(
                        "Scene snapshot capture timed out after %.2fs; disabling async snapshot service",
                        self._capture_timeout_ns / 1_000_000_000,
                    )
                return
