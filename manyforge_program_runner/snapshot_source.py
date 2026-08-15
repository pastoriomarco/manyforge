"""Bounded Composer snapshot client for Program model attachments."""

from __future__ import annotations

import base64
from collections.abc import Mapping
from hashlib import sha256
import math
import re
from threading import Event, RLock
from typing import Final
from urllib.parse import urlsplit

import httpx

from manyforge_behavior.assistant_program_port import (
    ProgramAssistantPortError,
    ProgramSnapshotSourceError,
    validate_program_snapshot_attachment,
)


_MAX_IMAGE_BYTES: Final = 2 * 1024 * 1024
_MAX_CAPTURE_TIMEOUT_S: Final = 2.0
_STREAM_CHUNK_BYTES: Final = 64 * 1024
_OWNER_EPOCH: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$")


class ComposerProgramSnapshotSource:
    """Capture one owner-fenced viewer frame as an inline data URL."""

    def __init__(
        self,
        render_url: str,
        *,
        client: httpx.Client | None = None,
    ) -> None:
        if not isinstance(render_url, str) or not render_url:
            raise ValueError("snapshot render URL must be bounded non-empty text")
        try:
            parsed = urlsplit(render_url)
            parsed.port
        except ValueError as exc:
            raise ValueError("snapshot render URL is invalid") from exc
        if (
            len(render_url.encode("utf-8")) > 4_096
            or parsed.scheme not in {"http", "https"}
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
            or parsed.path != "/api/scene/snapshot/render"
            or bool(parsed.query)
            or bool(parsed.fragment)
        ):
            raise ValueError(
                "snapshot render URL must be the credential-free Composer "
                "/api/scene/snapshot/render endpoint"
            )
        self._render_url = render_url
        self._client = client or httpx.Client()
        self._owns_client = client is None
        self._lock = RLock()
        self._closed = False

    @staticmethod
    def _parameters(
        *,
        width: int,
        height: int,
        image_format: str,
        transport: str,
        timeout_s: float,
        cancel_event: Event,
    ) -> tuple[dict[str, int | str], str, float]:
        if (
            type(width) is not int
            or not 64 <= width <= 1_920
            or type(height) is not int
            or not 64 <= height <= 1_080
            or image_format not in {"jpeg", "png"}
            or transport != "inline_data_url"
            or not isinstance(cancel_event, Event)
            or isinstance(timeout_s, bool)
            or not isinstance(timeout_s, (int, float))
            or not math.isfinite(float(timeout_s))
            or float(timeout_s) <= 0.0
        ):
            raise ProgramSnapshotSourceError(
                "model_snapshot_request_invalid",
                "Program snapshot request is outside its fixed contract",
            )
        bounded_timeout_s = min(float(timeout_s), _MAX_CAPTURE_TIMEOUT_S)
        timeout_ms = max(1, min(2_000, int(bounded_timeout_s * 1_000.0)))
        return (
            {
                "format": image_format,
                "height": height,
                "timeout_ms": timeout_ms,
                "width": width,
            },
            "image/jpeg" if image_format == "jpeg" else "image/png",
            bounded_timeout_s,
        )

    def capture_snapshot(
        self,
        *,
        width: int,
        height: int,
        image_format: str,
        transport: str,
        timeout_s: float,
        cancel_event: Event,
    ) -> Mapping[str, object]:
        params, expected_media_type, bounded_timeout_s = self._parameters(
            width=width,
            height=height,
            image_format=image_format,
            transport=transport,
            timeout_s=timeout_s,
            cancel_event=cancel_event,
        )
        if cancel_event.is_set():
            raise ProgramSnapshotSourceError(
                "model_request_cancelled",
                "model request was cancelled",
            )
        with self._lock:
            if self._closed:
                raise ProgramSnapshotSourceError(
                    "model_snapshot_image_source_unavailable",
                    "Program snapshot source is closed",
                )
            try:
                with self._client.stream(
                    "GET",
                    self._render_url,
                    params=params,
                    timeout=httpx.Timeout(bounded_timeout_s),
                ) as response:
                    if response.status_code == 503:
                        raise ProgramSnapshotSourceError(
                            "model_snapshot_observer_unavailable",
                            "no generation-matched viewer snapshot is available",
                        )
                    if response.status_code < 200 or response.status_code >= 300:
                        raise ProgramSnapshotSourceError(
                            "model_snapshot_capture_failed",
                            "snapshot source rejected the capture request",
                        )
                    media_type = response.headers.get("content-type", "").split(
                        ";", 1
                    )[0].strip().lower()
                    owner_epoch = response.headers.get(
                        "x-manyforge-viewer-owner-epoch"
                    )
                    raw_generation = response.headers.get(
                        "x-manyforge-viewer-generation"
                    )
                    cache_control = response.headers.get("cache-control", "")
                    try:
                        generation = int(raw_generation, 10)
                    except (TypeError, ValueError):
                        generation = -1
                    if (
                        media_type != expected_media_type
                        or owner_epoch is None
                        or _OWNER_EPOCH.fullmatch(owner_epoch) is None
                        or generation < 0
                        or "no-store" not in {
                            token.strip().lower()
                            for token in cache_control.split(",")
                        }
                    ):
                        raise ProgramSnapshotSourceError(
                            "model_snapshot_capture_invalid",
                            "snapshot source returned invalid owner metadata",
                        )
                    raw_length = response.headers.get("content-length")
                    if raw_length is not None:
                        try:
                            content_length = int(raw_length, 10)
                        except ValueError as exc:
                            raise ProgramSnapshotSourceError(
                                "model_snapshot_capture_invalid",
                                "snapshot source returned an invalid byte length",
                            ) from exc
                        if content_length < 1 or content_length > _MAX_IMAGE_BYTES:
                            raise ProgramSnapshotSourceError(
                                "model_snapshot_capture_invalid",
                                "snapshot image exceeds its fixed byte bound",
                            )
                    image = bytearray()
                    for chunk in response.iter_bytes(
                        chunk_size=_STREAM_CHUNK_BYTES
                    ):
                        if cancel_event.is_set():
                            raise ProgramSnapshotSourceError(
                                "model_request_cancelled",
                                "model request was cancelled",
                            )
                        if len(image) + len(chunk) > _MAX_IMAGE_BYTES:
                            raise ProgramSnapshotSourceError(
                                "model_snapshot_capture_invalid",
                                "snapshot image exceeds its fixed byte bound",
                            )
                        image.extend(chunk)
            except ProgramSnapshotSourceError:
                raise
            except httpx.TimeoutException as exc:
                raise ProgramSnapshotSourceError(
                    "model_snapshot_capture_timeout",
                    "snapshot capture timed out",
                ) from exc
            except httpx.HTTPError as exc:
                raise ProgramSnapshotSourceError(
                    "model_snapshot_image_source_unavailable",
                    "Program snapshot source transport failed",
                ) from exc
        if cancel_event.is_set():
            raise ProgramSnapshotSourceError(
                "model_request_cancelled",
                "model request was cancelled",
            )
        image_bytes = bytes(image)
        attachment = {
            "data_url": (
                f"data:{expected_media_type};base64,"
                + base64.b64encode(image_bytes).decode("ascii")
            ),
            "kind": "snapshot_image",
            "media_type": expected_media_type,
            "source": {
                "height": height,
                "image_sha256": "sha256:" + sha256(image_bytes).hexdigest(),
                "schema_version": "manyforge.program_snapshot_source.v1",
                "viewer_generation": generation,
                "viewer_owner_epoch": owner_epoch,
                "width": width,
            },
            "transport": "inline_data_url",
        }
        try:
            return validate_program_snapshot_attachment(attachment)
        except ProgramAssistantPortError as exc:
            raise ProgramSnapshotSourceError(
                "model_snapshot_capture_invalid",
                str(exc),
            ) from exc

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            if self._owns_client:
                self._client.close()


__all__ = ["ComposerProgramSnapshotSource"]
