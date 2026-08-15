"""Focused capacity, identity, and HTTP coverage for viewer snapshots."""

from concurrent.futures import ThreadPoolExecutor
import time

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.routes_snapshot import (
    SnapshotBroker,
    SnapshotClientUnavailableError,
    create_snapshot_router,
)
from manyforge_composer.backend.security import SecurityConfig


OWNER_EPOCH = "headless.epoch.7"
OWNER_GENERATION = 41
JPEG = b"\xff\xd8\xff\xd9"


def _broker() -> SnapshotBroker:
    return SnapshotBroker(
        prepare_capture=lambda _timeout_s: (OWNER_EPOCH, OWNER_GENERATION),
    )


def test_snapshot_without_observer_fails_immediately_and_boundedly() -> None:
    broker = _broker()
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
        "preparation_failed_total": 0,
        "deliveries_rejected_total": 0,
        "bytes_total": 0,
        "last_completion_ns": None,
    }


def test_snapshot_rejects_unknown_or_late_delivery() -> None:
    broker = _broker()

    assert broker.deliver("unknown", JPEG) is False
    assert broker.status()["deliveries_rejected_total"] == 1


def test_snapshot_sender_receives_owner_bound_request_and_capture_identity() -> None:
    broker = _broker()

    def sender(request) -> None:
        assert request.width == 64
        assert request.height == 32
        assert request.media_type == "image/jpeg"
        assert request.required_owner_epoch == OWNER_EPOCH
        assert request.required_generation == OWNER_GENERATION
        assert request.deadline_ns > time.monotonic_ns()
        assert broker.deliver(
            request.request_id,
            JPEG,
            observed_owner_epoch=OWNER_EPOCH,
            observed_generation=OWNER_GENERATION + 1,
        ) is True
        assert broker.deliver(request.request_id, None) is False

    broker.bind_sender(sender)

    capture = broker.capture_sync(64, 32, "jpeg", timeout_s=0.1)
    assert capture is not None
    assert capture.image_bytes == JPEG
    assert capture.observed_owner_epoch == OWNER_EPOCH
    assert capture.observed_generation == OWNER_GENERATION + 1
    assert broker.status()["completed_total"] == 1
    assert broker.status()["deliveries_rejected_total"] == 1


def test_snapshot_rejects_cross_owner_or_regressed_frame_without_settling() -> None:
    broker = _broker()

    def sender(request) -> None:
        assert broker.deliver(
            request.request_id,
            JPEG,
            observed_owner_epoch="replacement.epoch",
            observed_generation=OWNER_GENERATION,
        ) is False
        assert broker.deliver(
            request.request_id,
            JPEG,
            observed_owner_epoch=OWNER_EPOCH,
            observed_generation=OWNER_GENERATION - 1,
        ) is False
        assert broker.deliver(request.request_id, None) is True

    broker.bind_sender(sender)
    assert broker.render_sync(64, 64, "jpeg", timeout_s=0.1) is None
    assert broker.status()["deliveries_rejected_total"] == 2


class _HeadlessClient:
    def runtime(self) -> dict[str, object]:
        return {
            "schema_version": "manyforge.headless_runtime_snapshot.v1",
            "complete": True,
            "projection_owner_epoch": OWNER_EPOCH,
            "projection_generation": OWNER_GENERATION,
        }


class _State:
    def __init__(self) -> None:
        self.headless_client = _HeadlessClient()


def _client() -> tuple[TestClient, _State]:
    state = _State()
    app = FastAPI()
    app.include_router(create_snapshot_router(state, SecurityConfig()))  # type: ignore[arg-type]
    return TestClient(app), state


def test_snapshot_render_returns_the_observed_frame_headers() -> None:
    client, state = _client()
    broker = state.snapshot_broker  # type: ignore[attr-defined]

    def sender(request) -> None:
        assert broker.deliver(
            request.request_id,
            JPEG,
            observed_owner_epoch=OWNER_EPOCH,
            observed_generation=OWNER_GENERATION,
        )

    broker.bind_sender(sender)
    response = client.get(
        "/api/scene/snapshot/render",
        params={"width": 64, "height": 64, "format": "jpeg"},
    )
    assert response.status_code == 200
    assert response.content == JPEG
    assert response.headers["x-manyforge-viewer-owner-epoch"] == OWNER_EPOCH
    assert response.headers["x-manyforge-viewer-generation"] == str(
        OWNER_GENERATION
    )


def test_snapshot_upload_settles_only_matching_pending_frame() -> None:
    client, state = _client()
    broker = state.snapshot_broker  # type: ignore[attr-defined]
    requests = []
    broker.bind_sender(requests.append)

    with ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(
            client.get,
            "/api/scene/snapshot/render?width=64&height=64&format=jpeg",
        )
        deadline = time.monotonic() + 1.0
        while not requests and time.monotonic() < deadline:
            time.sleep(0.001)
        assert len(requests) == 1
        request_id = requests[0].request_id
        rejected = client.post(
            f"/api/scene/snapshot/upload/{request_id}",
            content=JPEG,
            headers={
                "X-ManyForge-Viewer-Owner-Epoch": "wrong.epoch",
                "X-ManyForge-Viewer-Generation": str(OWNER_GENERATION),
            },
        )
        assert rejected.status_code == 409
        accepted = client.post(
            f"/api/scene/snapshot/upload/{request_id}",
            content=JPEG,
            headers={
                "X-ManyForge-Viewer-Owner-Epoch": OWNER_EPOCH,
                "X-ManyForge-Viewer-Generation": str(OWNER_GENERATION),
            },
        )
        assert accepted.status_code == 200
        response = future.result(timeout=1.0)
    assert response.status_code == 200
    assert response.content == JPEG


def test_snapshot_route_requires_a_headless_runtime_client() -> None:
    class MissingState:
        headless_client = None

    with pytest.raises(
        SnapshotClientUnavailableError,
        match="composable_cell_snapshot_client_unavailable",
    ):
        create_snapshot_router(MissingState(), SecurityConfig())  # type: ignore[arg-type]
