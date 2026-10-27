#!/usr/bin/env python3
"""Qualify the I9 owner-fenced Composer-to-model snapshot transport.

The observer in this probe is deliberately a transport fixture, not a browser
render claim.  Composer still prepares the exact current Headless owner frame,
admits one capacity-one capture, validates the uploaded owner/generation, and
serves it through the production Program snapshot source.  The production
OpenAI-compatible transport must then send those same image bytes as a
standard multimodal content part to a bounded local model fixture.
"""

from __future__ import annotations

import argparse
import base64
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import re
import sys
from threading import Event, Lock, Thread
import time
from typing import Any, Final
from urllib.parse import urlsplit, urlunsplit

import httpx
from websockets.sync.client import connect

from manyforge_behavior.assistant_program_port import ProgramModelRequest
from manyforge_headless_api.program_model_transport import OpenAICompatibleProgramModelTransport
from manyforge_program_runner.snapshot_source import ComposerProgramSnapshotSource


RESULT_SCHEMA: Final = "manyforge.i9_assistant_snapshot_probe.v1"
_MAXIMUM_JSON_BYTES: Final = 8 * 1024 * 1024
_OWNER_EPOCH: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$")
_REQUEST_ID: Final = re.compile(r"^[0-9a-f]{32}$")
_DIGEST: Final = "sha256:" + "a" * 64

# A real, decodable 1x1 JPEG.  The fixture makes no claim that these pixels
# represent the structural viewer; it proves only the owner-fenced transport.
_JPEG: Final = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////"
    "////////////////////////////////////////////////////////////2wBDAf//"
    "////////////////////////////////////////////////////////////"
    "////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAA"
    "AAAAAAAAAAAAAAAFBgf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAA"
    "AACfAP/EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEAAQUCX//EABQRAQAAAAAAAA"
    "AAAAAAAAAAABD/2gAIAQMBAT8BX//EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIB"
    "AT8BX//EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEABj8CX//EABQQAQAAAAAAAA"
    "AAAAAAAAAAABD/2gAIAQEAAT8hX//aAAwDAQACAAMAAAAQH//EABQRAQAAAAAAAAAA"
    "AAAAAAAAABD/2gAIAQMBAT8QX//EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIB"
    "AT8QX//EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEAAT8QX//Z",
    validate=True,
)


class SnapshotProbeError(RuntimeError):
    """One bounded transport or evidence failure."""


@dataclass(frozen=True, slots=True)
class SnapshotRequest:
    request_id: str
    width: int
    height: int
    media_type: str
    timeout_ms: int
    owner_epoch: str
    generation: int


@dataclass(frozen=True, slots=True)
class SnapshotProbeResult:
    schema_version: str
    passed: bool
    fixture_kind: str
    viewer_owner_epoch: str
    viewer_generation: int
    image_sha256: str
    image_bytes: int
    snapshot_completed_delta: int
    snapshot_rejected_delta: int
    model_requests: int
    model_payload_sha256: str
    elapsed_s: float


def parse_snapshot_request(value: Any) -> SnapshotRequest:
    """Parse the exact browser observer request without coercion."""

    if not isinstance(value, dict) or set(value) != {
        "height",
        "mime",
        "requestId",
        "requiredFrame",
        "timeoutMs",
        "type",
        "width",
    }:
        raise SnapshotProbeError("snapshot request fields are invalid")
    if value["type"] != "snapshot_request":
        raise SnapshotProbeError("snapshot request type is invalid")
    frame = value["requiredFrame"]
    if not isinstance(frame, dict) or set(frame) != {"generation", "ownerEpoch"}:
        raise SnapshotProbeError("snapshot required-frame fields are invalid")
    request_id = value["requestId"]
    owner_epoch = frame["ownerEpoch"]
    if (
        not isinstance(request_id, str)
        or _REQUEST_ID.fullmatch(request_id) is None
        or not isinstance(owner_epoch, str)
        or _OWNER_EPOCH.fullmatch(owner_epoch) is None
        or type(value["width"]) is not int
        or not 64 <= value["width"] <= 1920
        or type(value["height"]) is not int
        or not 64 <= value["height"] <= 1080
        or value["mime"] not in {"image/jpeg", "image/png"}
        or type(value["timeoutMs"]) is not int
        or not 1 <= value["timeoutMs"] <= 2000
        or type(frame["generation"]) is not int
        or frame["generation"] < 0
    ):
        raise SnapshotProbeError("snapshot request values are invalid")
    return SnapshotRequest(
        request_id=request_id,
        width=value["width"],
        height=value["height"],
        media_type=value["mime"],
        timeout_ms=value["timeoutMs"],
        owner_epoch=owner_epoch,
        generation=frame["generation"],
    )


def validate_probe_result(value: Any) -> dict[str, Any]:
    """Validate the compact result before it is admitted into an I9 row."""

    expected_fields = {
        "elapsed_s",
        "fixture_kind",
        "image_bytes",
        "image_sha256",
        "model_payload_sha256",
        "model_requests",
        "passed",
        "schema_version",
        "snapshot_completed_delta",
        "snapshot_rejected_delta",
        "viewer_generation",
        "viewer_owner_epoch",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        raise SnapshotProbeError("snapshot probe result fields are invalid")
    if (
        value["schema_version"] != RESULT_SCHEMA
        or value["fixture_kind"] != "owner_fenced_transport_jpeg"
        or type(value["passed"]) is not bool
        or not isinstance(value["viewer_owner_epoch"], str)
        or _OWNER_EPOCH.fullmatch(value["viewer_owner_epoch"]) is None
        or type(value["viewer_generation"]) is not int
        or value["viewer_generation"] < 0
        or not isinstance(value["image_sha256"], str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", value["image_sha256"])
        is None
        or type(value["image_bytes"]) is not int
        or not 1 <= value["image_bytes"] <= 2 * 1024 * 1024
        or type(value["snapshot_completed_delta"]) is not int
        or type(value["snapshot_rejected_delta"]) is not int
        or type(value["model_requests"]) is not int
        or value["snapshot_completed_delta"] < 0
        or value["snapshot_rejected_delta"] < 0
        or value["model_requests"] < 0
        or not isinstance(value["model_payload_sha256"], str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", value["model_payload_sha256"])
        is None
        or isinstance(value["elapsed_s"], bool)
        or not isinstance(value["elapsed_s"], (int, float))
        or not math.isfinite(float(value["elapsed_s"]))
        or not 0.0 <= float(value["elapsed_s"]) <= 30.0
    ):
        raise SnapshotProbeError("snapshot probe result values are invalid")
    expected_pass = (
        value["snapshot_completed_delta"] == 1
        and value["snapshot_rejected_delta"] == 0
        and value["model_requests"] == 1
        and value["image_bytes"] == len(_JPEG)
        and value["image_sha256"] == "sha256:" + sha256(_JPEG).hexdigest()
    )
    if value["passed"] is not expected_pass:
        raise SnapshotProbeError("snapshot probe pass verdict is inconsistent")
    return dict(value)


def _composer_urls(base_url: str) -> tuple[str, str, str, str]:
    parsed = urlsplit(base_url)
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in {"", "/"}
        or parsed.query
        or parsed.fragment
        or len(base_url.encode("utf-8")) > 4096
    ):
        raise SnapshotProbeError("Composer URL must be one credential-free origin")
    origin = urlunsplit((parsed.scheme, parsed.netloc, "", "", "")).rstrip("/")
    websocket_scheme = "wss" if parsed.scheme == "https" else "ws"
    websocket = urlunsplit(
        (websocket_scheme, parsed.netloc, "/ws/scene/snapshot", "", "")
    )
    return (
        origin + "/api/scene/snapshot/render",
        origin + "/api/scene/snapshot/status",
        origin + "/api/scene/snapshot/upload",
        websocket,
    )


class _SnapshotObserver:
    def __init__(self, websocket_url: str, upload_root: str) -> None:
        self.websocket_url = websocket_url
        self.upload_root = upload_root
        self.ready = Event()
        self.stop = Event()
        self.requests: list[SnapshotRequest] = []
        self.error: Exception | None = None
        self.thread = Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()
        if not self.ready.wait(5.0):
            raise SnapshotProbeError("snapshot observer did not bind")
        if self.error is not None:
            raise SnapshotProbeError(f"snapshot observer failed: {self.error}")

    def close(self) -> None:
        self.stop.set()
        self.thread.join(timeout=3.0)
        if self.thread.is_alive():
            raise SnapshotProbeError("snapshot observer did not stop")
        if self.error is not None:
            raise SnapshotProbeError(f"snapshot observer failed: {self.error}")

    def _run(self) -> None:
        try:
            with connect(
                self.websocket_url,
                open_timeout=3.0,
                close_timeout=1.0,
                max_size=64 * 1024,
            ) as websocket:
                self.ready.set()
                while not self.stop.is_set():
                    try:
                        raw = websocket.recv(timeout=0.1)
                    except TimeoutError:
                        continue
                    if not isinstance(raw, str) or len(raw.encode("utf-8")) > 64 * 1024:
                        raise SnapshotProbeError("snapshot observer message is invalid")
                    request = parse_snapshot_request(json.loads(raw))
                    if request.media_type != "image/jpeg":
                        raise SnapshotProbeError("probe expected one JPEG request")
                    self.requests.append(request)
                    response = httpx.post(
                        f"{self.upload_root}/{request.request_id}",
                        content=_JPEG,
                        headers={
                            "Content-Type": "image/jpeg",
                            "X-ManyForge-Viewer-Owner-Epoch": request.owner_epoch,
                            "X-ManyForge-Viewer-Generation": str(request.generation),
                        },
                        timeout=1.0,
                    )
                    if response.status_code != 200 or response.json() != {
                        "bytes": len(_JPEG),
                        "ok": True,
                    }:
                        raise SnapshotProbeError(
                            f"snapshot upload was rejected: HTTP {response.status_code}"
                        )
        except Exception as error:
            self.error = error
            self.ready.set()


class _ModelRecorder:
    def __init__(self) -> None:
        self.lock = Lock()
        self.payloads: list[dict[str, Any]] = []

    def record(self, payload: Any) -> None:
        if not isinstance(payload, dict):
            raise SnapshotProbeError("model fixture request must be an object")
        encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
        if len(encoded) > _MAXIMUM_JSON_BYTES:
            raise SnapshotProbeError("model fixture request exceeds its bound")
        with self.lock:
            self.payloads.append(payload)


def _model_server(recorder: _ModelRecorder) -> tuple[ThreadingHTTPServer, Thread]:
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:  # noqa: N802
            try:
                length = int(self.headers.get("Content-Length", "0"), 10)
                if not 0 < length <= _MAXIMUM_JSON_BYTES:
                    raise SnapshotProbeError("model request length is invalid")
                recorder.record(json.loads(self.rfile.read(length)))
                body = json.dumps(
                    {
                        "choices": [
                            {"message": {"content": '{"visible":true}'}}
                        ],
                        "model": "i9-snapshot-fixture",
                        "usage": {"total_tokens": 1},
                    },
                    separators=(",", ":"),
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()

        def log_message(self, _format: str, *_args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def _status(url: str) -> dict[str, Any]:
    response = httpx.get(url, timeout=2.0)
    if response.status_code != 200:
        raise SnapshotProbeError(
            f"snapshot status returned HTTP {response.status_code}"
        )
    value = response.json()
    if (
        not isinstance(value, dict)
        or value.get("schema_version")
        != "manyforge.viewer_snapshot_status.v1"
    ):
        raise SnapshotProbeError("snapshot status has an invalid schema")
    return value


def _wait_observer_bound(status_url: str) -> dict[str, Any]:
    deadline = time.monotonic() + 5.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            last = _status(status_url)
            if last.get("observer_bound") is True:
                return last
        except (httpx.HTTPError, SnapshotProbeError):
            pass
        time.sleep(0.02)
    raise SnapshotProbeError(f"snapshot observer did not become visible: {last}")


def run_probe(composer_url: str) -> dict[str, Any]:
    started = time.monotonic()
    render_url, status_url, upload_root, websocket_url = _composer_urls(
        composer_url
    )
    recorder = _ModelRecorder()
    server, server_thread = _model_server(recorder)
    observer = _SnapshotObserver(websocket_url, upload_root)
    snapshot_source: ComposerProgramSnapshotSource | None = None
    model_transport: OpenAICompatibleProgramModelTransport | None = None
    try:
        observer.start()
        before = _wait_observer_bound(status_url)
        snapshot_source = ComposerProgramSnapshotSource(render_url)
        attachment = snapshot_source.capture_snapshot(
            width=640,
            height=480,
            image_format="jpeg",
            transport="inline_data_url",
            timeout_s=2.0,
            cancel_event=Event(),
        )
        source = attachment["source"]
        model_transport = OpenAICompatibleProgramModelTransport()
        response = model_transport.complete(
            ProgramModelRequest(
                model_endpoint_alias="i9_snapshot_model",
                external_resource_alias="i9_snapshot_fixture",
                model_id="i9-snapshot-fixture",
                base_url=f"http://127.0.0.1:{server.server_port}/v1",
                api_key_env=None,
                prompt_template_id="i9.snapshot.probe",
                prompt_template_version="1",
                prompt_template_digest=_DIGEST,
                prompt="Report whether the owner-fenced cell frame is visible.",
                timeout_s=2.0,
                max_output_tokens=32,
                response_mode="json_object",
                tool_call_family="openai_chat_tools",
                response_tool=None,
                attachment=attachment,
            ),
            Event(),
        )
        after = _status(status_url)
        if response.text != '{"visible":true}':
            raise SnapshotProbeError("model fixture returned another response")
        if len(observer.requests) != 1:
            raise SnapshotProbeError("snapshot broker dispatched another request count")
        if len(recorder.payloads) != 1:
            raise SnapshotProbeError("model transport sent another request count")
        request = observer.requests[0]
        if (
            request.owner_epoch != source["viewer_owner_epoch"]
            or request.generation != source["viewer_generation"]
        ):
            raise SnapshotProbeError("snapshot attachment lost its owner fence")
        content = recorder.payloads[0].get("messages")
        expected_data_url = attachment["data_url"]
        if (
            not isinstance(content, list)
            or len(content) != 1
            or not isinstance(content[0], dict)
            or content[0].get("role") != "user"
            or not isinstance(content[0].get("content"), list)
            or len(content[0]["content"]) != 2
            or content[0]["content"][1]
            != {
                "image_url": {"url": expected_data_url},
                "type": "image_url",
            }
        ):
            raise SnapshotProbeError("model request omitted the exact snapshot image")
        model_payload = json.dumps(
            recorder.payloads[0], separators=(",", ":"), sort_keys=True
        ).encode()
        result = SnapshotProbeResult(
            schema_version=RESULT_SCHEMA,
            passed=True,
            fixture_kind="owner_fenced_transport_jpeg",
            viewer_owner_epoch=source["viewer_owner_epoch"],
            viewer_generation=source["viewer_generation"],
            image_sha256=source["image_sha256"],
            image_bytes=len(_JPEG),
            snapshot_completed_delta=(
                int(after["completed_total"]) - int(before["completed_total"])
            ),
            snapshot_rejected_delta=(
                int(after["deliveries_rejected_total"])
                - int(before["deliveries_rejected_total"])
            ),
            model_requests=len(recorder.payloads),
            model_payload_sha256="sha256:" + sha256(model_payload).hexdigest(),
            elapsed_s=round(time.monotonic() - started, 3),
        )
        return validate_probe_result(asdict(result))
    finally:
        if model_transport is not None:
            model_transport.close()
        if snapshot_source is not None:
            snapshot_source.close()
        try:
            observer.close()
        finally:
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=3.0)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer-url", required=True)
    args = parser.parse_args(argv)
    try:
        result = run_probe(args.composer_url)
    except Exception as error:
        print(f"I9 assistant snapshot probe failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
