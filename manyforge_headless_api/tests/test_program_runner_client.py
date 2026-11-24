"""Bounded transport tests for the isolated Program-runner client."""

from __future__ import annotations

import json

import httpx
import pytest

from manyforge_headless_api.program_runner_client import (
    ProgramRunnerClientError,
    ProgramRunnerHttpClient,
)

# The runner's commands whose reply is its row at acceptance (OD-214).
_COMMAND_PATHS = (
    "/motion-start",
    "/motion-resume",
    "/manual-handover",
    "/resume-halt",
    "/blackboard-values",
    "/manual-result",
    "/api/cycle/recovery-actions",
    "/api/program/cycle/reset",
    "/api/program/persistent-values/reset",
    "/api/program/persistent-values/clear",
)


def _row(request_key: str, kind: str = "start") -> dict:
    return {
        "kind": kind,
        "request_key": request_key,
        "step": "admitting",
        "accepted_at_ns": "1763112345678901234",
        "end": None,
    }


def _reply(request: httpx.Request) -> httpx.Response:
    """The runner's `program_command_reply.v1` for a command, else ``{"ok": true}``."""

    path = request.url.path
    command = request.method == "POST" and (
        path.endswith(_COMMAND_PATHS)
        or path in {"/api/program/runs", "/api/program/persistent-values"}
    )
    if not command:
        return httpx.Response(200, json={"ok": True})
    key = json.loads(request.content)["request_key"]
    return httpx.Response(
        200,
        json={"schema_version": "manyforge.program_command_reply.v1", "command": _row(key)},
    )


def test_program_runner_client_preserves_paths_queries_and_bodies() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return _reply(request)

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)

    def reply(key: str) -> dict:
        return {"schema_version": "manyforge.program_command_reply.v1", "command": _row(key)}

    try:
        assert client.runs(active_only=True, limit=7) == {"ok": True}
        assert client.start_run({"request_key": "1" * 32}) == reply("1" * 32)
        assert client.read_run(1) == {"ok": True}
        assert client.start_motion(1, {"request_key": "2" * 32}) == reply("2" * 32)
        assert client.prepare_manual_handover(1, {"request_key": "3" * 32}) == reply("3" * 32)
    finally:
        raw.close()

    assert [request.url.path for request in requests] == [
        "/api/program/runs",
        "/api/program/runs",
        "/api/program/runs/1",
        "/api/program/runs/1/motion-start",
        "/api/program/runs/1/manual-handover",
    ]
    assert dict(requests[0].url.params) == {
        "active_only": "true",
        "limit": "7",
    }
    assert json.loads(requests[1].content) == {"request_key": "11111111111111111111111111111111"}
    assert json.loads(requests[3].content) == {"request_key": "22222222222222222222222222222222"}
    assert json.loads(requests[4].content) == {"request_key": "33333333333333333333333333333333"}


def test_program_runner_client_preserves_stable_remote_errors() -> None:
    def handler(_request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            409,
            json={
                "detail": {
                    "code": "program_run_already_active",
                    "details": {"active_run": 1},
                    "message": "one active Program run is already present",
                    "producer": "kernel",
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
            client.start_run({"request_key": "11111111111111111111111111111111"})
    finally:
        raw.close()

    assert captured.value.code == "program_run_already_active"
    assert captured.value.status_code == 409
    assert str(captured.value) == "one active Program run is already present"
    assert captured.value.details == {"active_run": 1}
    assert captured.value.producer == "kernel"


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


def test_program_runner_client_addresses_applied_persistent_values() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return _reply(request)

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        assert client.persistent_values() == {"ok": True}
        for action in ("set", "reset", "clear"):
            key = {"set": "1", "reset": "2", "clear": "3"}[action] * 32
            assert client.persistent_value_action(action, {"request_key": key})[
                "command"
            ]["request_key"] == key
        with pytest.raises(ValueError, match="unsupported"):
            client.persistent_value_action("delete", {})
    finally:
        raw.close()

    base = "/api/program/persistent-values"
    assert [(request.method, request.url.path) for request in requests] == [
        ("GET", base),
        ("POST", base),
        ("POST", f"{base}/reset"),
        ("POST", f"{base}/clear"),
    ]
    assert json.loads(requests[2].content) == {"request_key": "2" * 32}


def test_program_runner_client_names_the_transport_failure_it_reports() -> None:
    def disconnected(request: httpx.Request) -> httpx.Response:
        raise httpx.RemoteProtocolError(
            "Server disconnected without sending a response.", request=request
        )

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(disconnected),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        with pytest.raises(ProgramRunnerClientError) as captured:
            client.cycle_status()
    finally:
        raw.close()
    assert captured.value.code == "program_runner_unavailable"
    assert captured.value.details == {"transport_error": "RemoteProtocolError"}


def test_program_runner_commands_wait_for_acceptance_without_a_read_limit() -> None:
    """OD-214: a command's reply comes at acceptance; only the connection is
    limited. Status reads keep the whole limit."""

    timeouts: dict[str, dict] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        timeouts[f"{request.method} {request.url.path}"] = request.extensions["timeout"]
        return _reply(request)

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
        timeout=httpx.Timeout(7.0),
    )
    client = ProgramRunnerHttpClient("http://runner.test", timeout_seconds=7.0, client=raw)
    key = {"request_key": "4" * 32}
    try:
        client.start_run(key)
        client.start_motion(1, key)
        client.resume_motion(1, key)
        client.prepare_manual_handover(1, key)
        client.resume_halt(1, key)
        client.reset_cycle(key)
        client.runtime_control_action(1, "blackboard-values", key)
        client.runtime_control_action(1, "manual-result", key)
        client.apply_recovery_action(key)
        for action in ("set", "reset", "clear"):
            client.persistent_value_action(action, key)
        client.cycle_status()
        client.read_run(1)
        client.runtime_blackboard_values(1)
    finally:
        raw.close()

    reads = {
        "GET /api/cycle/status",
        "GET /api/program/runs/1",
        "GET /api/program/runs/1/blackboard-values",
    }
    assert len(timeouts) == 15
    for request, timeout in timeouts.items():
        if request in reads:
            assert timeout == {"connect": 7.0, "read": 7.0, "write": 7.0, "pool": 7.0}
        else:
            assert timeout == {"connect": 7.0, "read": None, "write": None, "pool": None}, request


def test_cycle_read_before_a_command_waits_for_the_answer_and_ends_at_runner_loss() -> None:
    """OD-214 (T10): the cycle status read that decides whether a command is
    forwarded has only its connection limited; a lost runner still ends it."""

    timeouts: list[dict] = []
    lost = False

    def handler(request: httpx.Request) -> httpx.Response:
        if lost:
            raise httpx.RemoteProtocolError(
                "Server disconnected without sending a response.", request=request
            )
        timeouts.append(request.extensions["timeout"])
        return _reply(request)

    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(handler),
        timeout=httpx.Timeout(7.0),
    )
    client = ProgramRunnerHttpClient("http://runner.test", timeout_seconds=7.0, client=raw)
    try:
        client.cycle_status(before_command=True)
        lost = True
        with pytest.raises(ProgramRunnerClientError) as captured:
            client.cycle_status(before_command=True)
    finally:
        raw.close()
    assert timeouts == [{"connect": 7.0, "read": None, "write": None, "pool": None}]
    assert captured.value.code == "program_runner_unavailable"


@pytest.mark.parametrize(
    "body",
    [
        {"ok": True},
        {"schema_version": "manyforge.program_command_reply.v1"},
        {"schema_version": "manyforge.program_command_reply.v1", "command": {"kind": "start"}},
        {
            "schema_version": "manyforge.program_command_reply.v1",
            "command": {**_row("5" * 32), "message": "text"},
        },
    ],
)
def test_program_runner_client_refuses_an_invalid_command_reply(body: dict) -> None:
    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(lambda _request: httpx.Response(200, json=body)),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        with pytest.raises(ProgramRunnerClientError) as captured:
            client.reset_cycle({"request_key": "5" * 32})
    finally:
        raw.close()
    assert captured.value.code == "program_runner_response_invalid"


def test_program_runner_client_returns_a_replayed_ended_row_as_answered() -> None:
    ended = {
        **_row("6" * 32, "reset"),
        "step": "releasing",
        "end": {
            "outcome": "refused",
            "code": "program_run_request_conflict",
            "details": {"program_run_id": 3},
            "finished_at_ns": "1763112345678909999",
        },
    }
    body = {"schema_version": "manyforge.program_command_reply.v1", "command": ended}
    raw = httpx.Client(
        base_url="http://runner.test",
        transport=httpx.MockTransport(lambda _request: httpx.Response(200, json=body)),
    )
    client = ProgramRunnerHttpClient("http://runner.test", client=raw)
    try:
        assert client.reset_cycle({"request_key": "6" * 32}) == body
    finally:
        raw.close()
