"""Owner identity, bounds, and failures for Program snapshot capture."""

from __future__ import annotations

import base64
from hashlib import sha256
from threading import Event

import httpx
import pytest

from manyforge_behavior.assistant_program_port import ProgramSnapshotSourceError
from manyforge_program_runner.snapshot_source import ComposerProgramSnapshotSource


_URL = "http://composer.test/api/scene/snapshot/render"
_JPEG = b"\xff\xd8\xff\xe0manyforge-program-snapshot"


def _capture(source: ComposerProgramSnapshotSource, cancel: Event | None = None):
    return source.capture_snapshot(
        width=640,
        height=480,
        image_format="jpeg",
        transport="inline_data_url",
        timeout_s=30.0,
        cancel_event=cancel or Event(),
    )


def test_capture_returns_bounded_owner_attributed_inline_image() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            content=_JPEG,
            headers={
                "Cache-Control": "no-store",
                "Content-Type": "image/jpeg",
                "X-ManyForge-Viewer-Generation": "19",
                "X-ManyForge-Viewer-Owner-Epoch": "headless.epoch.1",
            },
        )

    source = ComposerProgramSnapshotSource(
        _URL,
        client=httpx.Client(transport=httpx.MockTransport(handler)),
    )

    attachment = _capture(source)

    assert base64.b64decode(attachment["data_url"].split(",", 1)[1]) == _JPEG
    assert attachment["source"] == {
        "height": 480,
        "image_sha256": "sha256:" + sha256(_JPEG).hexdigest(),
        "schema_version": "manyforge.program_snapshot_source.v1",
        "viewer_generation": 19,
        "viewer_owner_epoch": "headless.epoch.1",
        "width": 640,
    }
    query = requests[0].url.params
    assert query["format"] == "jpeg"
    assert query["height"] == "480"
    assert query["timeout_ms"] == "2000"
    assert query["width"] == "640"


def test_capture_rejects_missing_observer_and_invalid_owner_metadata() -> None:
    unavailable = ComposerProgramSnapshotSource(
        _URL,
        client=httpx.Client(
            transport=httpx.MockTransport(
                lambda _request: httpx.Response(503, json={"detail": "absent"})
            )
        ),
    )
    with pytest.raises(ProgramSnapshotSourceError) as absent:
        _capture(unavailable)
    assert absent.value.reason_code == "model_snapshot_observer_unavailable"
    assert "composer.test" not in str(absent.value)

    invalid = ComposerProgramSnapshotSource(
        _URL,
        client=httpx.Client(
            transport=httpx.MockTransport(
                lambda _request: httpx.Response(
                    200,
                    content=_JPEG,
                    headers={
                        "Cache-Control": "no-store",
                        "Content-Type": "image/jpeg",
                        "X-ManyForge-Viewer-Generation": "19",
                    },
                )
            )
        ),
    )
    with pytest.raises(ProgramSnapshotSourceError) as malformed:
        _capture(invalid)
    assert malformed.value.reason_code == "model_snapshot_capture_invalid"


def test_capture_is_cancelled_before_dispatch_and_rejects_bad_configuration() -> None:
    called = False

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal called
        called = True
        return httpx.Response(500)

    source = ComposerProgramSnapshotSource(
        _URL,
        client=httpx.Client(transport=httpx.MockTransport(handler)),
    )
    cancelled = Event()
    cancelled.set()
    with pytest.raises(ProgramSnapshotSourceError) as raised:
        _capture(source, cancelled)
    assert raised.value.reason_code == "model_request_cancelled"
    assert called is False

    with pytest.raises(ValueError, match="credential-free"):
        ComposerProgramSnapshotSource("http://secret@composer.test/snapshot")
    with pytest.raises(ProgramSnapshotSourceError) as transport:
        source.capture_snapshot(
            width=640,
            height=480,
            image_format="jpeg",
            transport="http_url",
            timeout_s=1.0,
            cancel_event=Event(),
        )
    assert transport.value.reason_code == "model_snapshot_request_invalid"


def test_capture_rejects_oversized_image_before_encoding() -> None:
    source = ComposerProgramSnapshotSource(
        _URL,
        client=httpx.Client(
            transport=httpx.MockTransport(
                lambda _request: httpx.Response(
                    200,
                    content=_JPEG + b"x" * (2 * 1024 * 1024),
                    headers={
                        "Cache-Control": "no-store",
                        "Content-Type": "image/jpeg",
                        "X-ManyForge-Viewer-Generation": "19",
                        "X-ManyForge-Viewer-Owner-Epoch": "headless.epoch.1",
                    },
                )
            )
        ),
    )

    with pytest.raises(ProgramSnapshotSourceError) as raised:
        _capture(source)
    assert raised.value.reason_code == "model_snapshot_capture_invalid"
