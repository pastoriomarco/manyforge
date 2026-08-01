"""Focused bounded snapshot-broker coverage for the I9 port target."""

import time

import pytest

from manyforge_composer.backend.routes_snapshot import (
    SnapshotBroker,
    create_snapshot_router,
)
from manyforge_composer.backend.security import SecurityConfig


def test_snapshot_without_observer_fails_immediately_and_boundedly() -> None:
    broker = SnapshotBroker()
    started = time.monotonic()

    assert broker.render_sync(320, 240, "jpeg", timeout_s=10.0) is None
    assert time.monotonic() - started < 0.1
    assert broker.status() == {
        "observer_bound": False,
        "pending_count": 0,
        "requests_total": 1,
        "completed_total": 0,
        "failed_total": 1,
        "timeouts_total": 0,
        "unavailable_total": 1,
        "busy_rejected_total": 0,
        "dispatch_failed_total": 0,
        "deliveries_rejected_total": 0,
        "bytes_total": 0,
        "last_completion_ns": None,
    }


def test_snapshot_rejects_unknown_or_late_delivery() -> None:
    broker = SnapshotBroker()

    assert broker.deliver("unknown", b"image") is False
    assert broker.status()["deliveries_rejected_total"] == 1


def test_snapshot_sender_receives_transport_neutral_request() -> None:
    broker = SnapshotBroker()

    def sender(request) -> None:
        assert request.width == 64
        assert request.height == 32
        assert request.media_type == "jpeg"
        assert request.deadline_ns > time.monotonic_ns()
        assert broker.deliver(request.request_id, b"image") is True

    broker.bind_sender(sender)

    assert broker.render_sync(64, 32, "jpeg", timeout_s=0.1) == b"image"
    assert broker.status()["completed_total"] == 1


def test_snapshot_route_is_fenced_before_state_access() -> None:
    class PoisonState:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"state was accessed: {name}")

    with pytest.raises(
        RuntimeError,
        match="composable_cell_snapshot_client_unavailable",
    ):
        create_snapshot_router(PoisonState(), SecurityConfig())  # type: ignore[arg-type]
