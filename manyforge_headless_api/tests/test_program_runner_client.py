"""Bounded transport tests for the isolated Program-runner client."""

from __future__ import annotations

import json

import httpx
import pytest

from manyforge_headless_api.program_runner_client import (
    ProgramRunnerClientError,
    ProgramRunnerHttpClient,
)


def test_program_runner_client_preserves_paths_queries_and_bodies() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"ok": True})

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        assert client.runs(active_only=True, limit=7) == {"ok": True}
        assert client.start_run({"request_id": "request.1"}) == {"ok": True}
        assert client.read_run("program run:1") == {"ok": True}
        assert client.stop_run(
            "program run:1",
            {"request_id": "request.2"},
        ) == {"ok": True}
        assert client.stop_motion(
            "program run:1",
            {"request_id": "request.3"},
        ) == {"ok": True}
        assert client.prepare_manual_handover(
            "program run:1",
            {"request_id": "request.4"},
        ) == {"ok": True}
    finally:
        raw.close()

    assert [request.url.path for request in requests] == [
        "/api/program/runs",
        "/api/program/runs",
        "/api/program/runs/program run:1",
        "/api/program/runs/program run:1/stop",
        "/api/program/runs/program run:1/motion-stop",
        "/api/program/runs/program run:1/manual-handover",
    ]
    assert dict(requests[0].url.params) == {
        "active_only": "true",
        "limit": "7",
    }
    assert json.loads(requests[1].content) == {"request_id": "request.1"}
    assert json.loads(requests[3].content) == {"request_id": "request.2"}
    assert json.loads(requests[4].content) == {"request_id": "request.3"}
    assert json.loads(requests[5].content) == {"request_id": "request.4"}


def test_program_runner_client_preserves_stable_remote_errors() -> None:
    def handler(_request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            409,
            json={
                "detail": {
                    "code": "program_run_already_active",
                    "message": "one active Program run is already present",
                }
            },
        )

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        with pytest.raises(ProgramRunnerClientError) as captured:
            client.start_run({"request_id": "request.1"})
    finally:
        raw.close()

    assert captured.value.code == "program_run_already_active"
    assert captured.value.status_code == 409
    assert str(captured.value) == "one active Program run is already present"


@pytest.mark.parametrize(
    "base_url",
    [
        "runner.test",
        "file:///tmp/runner",
        "https://secret@runner.test",
        "https://runner.test/prefix",
    ],
)
def test_program_runner_client_rejects_ambiguous_urls(base_url: str) -> None:
    with pytest.raises(ValueError, match="credential-free HTTP"):
        ProgramRunnerHttpClient(base_url)


def test_program_runner_client_rejects_non_object_or_duplicate_json() -> None:
    payloads: list[bytes] = [b"[]", b'{"ok":true,"ok":false}']

    def handler(_request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, content=payloads.pop(0))

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        for _index in range(2):
            with pytest.raises(ProgramRunnerClientError) as captured:
                client.runs()
            assert captured.value.code == "program_runner_response_invalid"
    finally:
        raw.close()
