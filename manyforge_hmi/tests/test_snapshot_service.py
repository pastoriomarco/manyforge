from __future__ import annotations

import time

import pytest

from manyforge_hmi.snapshot_service import SceneSnapshotService


def test_snapshot_service_returns_cached_frame_after_background_capture():
    render_calls: list[tuple[int, int, str]] = []

    def render(width: int, height: int, format: str) -> bytes:
        render_calls.append((width, height, format))
        time.sleep(0.01)
        return b"frame-1"

    service = SceneSnapshotService(
        render,
        max_age_s=5.0,
        request_cooldown_s=0.0,
        capture_timeout_s=0.5,
    )
    try:
        assert service.get_snapshot(320, 240, "jpeg") is None

        deadline = time.monotonic() + 0.5
        cached = None
        while time.monotonic() < deadline:
            cached = service.get_snapshot(320, 240, "jpeg")
            if cached is not None:
                break
            time.sleep(0.005)

        assert cached == b"frame-1"
        assert render_calls == [(320, 240, "jpeg")]
    finally:
        service.shutdown()


def test_snapshot_service_disables_after_capture_timeout():
    def render(width: int, height: int, format: str) -> bytes:
        time.sleep(0.2)
        return b"late-frame"

    service = SceneSnapshotService(
        render,
        max_age_s=5.0,
        request_cooldown_s=0.0,
        capture_timeout_s=0.01,
    )
    try:
        assert service.get_snapshot(320, 240, "jpeg") is None
        time.sleep(0.05)
        with pytest.raises(RuntimeError, match="scene snapshot capture exceeded"):
            service.get_snapshot(320, 240, "jpeg")
        assert service.disabled_reason is not None
    finally:
        service.shutdown()
