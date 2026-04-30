"""HTTP fixture tests for the live assistant provider adapter."""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

pytest.importorskip("httpx")
pytest.importorskip("pydantic")

import httpx

from manyforge_composer.backend.assistant_provider import (
    AssistantProviderContext,
    AssistantProviderError,
    NemoClawAssistantProvider,
)
from manyforge_composer.backend.models import AssistantChatRequest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_FIXTURE_SERVER = _REPO_ROOT / "scripts" / "fake-assistant-provider.py"
_SMOKE_SCRIPT = _REPO_ROOT / "scripts" / "smoke-assistant-provider-bridge.py"


@pytest.fixture
def fake_provider_base_url() -> Iterator[str]:
    port = _free_tcp_port()
    process = subprocess.Popen(
        [
            sys.executable,
            str(_FIXTURE_SERVER),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--quiet",
        ],
        cwd=str(_REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    base_url = f"http://127.0.0.1:{port}"
    try:
        _wait_until_ready(base_url, process)
        yield base_url
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def test_nemoclaw_provider_round_trips_through_http_fixture(
    fake_provider_base_url: str,
) -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url=f"{fake_provider_base_url}/v1/manyforge/assistant",
        provider_id="openclaw",
        timeout_s=2.0,
    )

    result = provider.run(
        _request(),
        _context(conversation_id="conv-http-fixture"),
    )

    assert result.mode == "provider"
    assert result.message == "Fixture provider prepared a reviewable scene-resource proposal."
    assert result.mutated is False
    assert result.requires_review is True
    assert "Provider attempted to mark proposals as not requiring review" in result.warnings[-1]
    assert [call["name"] for call in result.tool_calls] == [
        "inspect_isaac_scene",
        "propose_scene_objects",
    ]
    assert result.tool_calls[0]["result"]["providerId"] == "openclaw"
    assert result.tool_calls[0]["result"]["conversationId"] == "conv-http-fixture"
    assert result.tool_calls[0]["result"]["requestedTools"] == [
        "inspect_isaac_scene",
        "propose_scene_objects",
    ]
    proposal = result.proposals[0]
    assert proposal["id"] == "proposal-fixture-soup-can"
    assert proposal["status"] == "draft"
    assert proposal["reviewRequired"] is True
    assert proposal["payload"]["sceneResource"]["objectId"] == "fixture_soup_can"
    assert proposal["source"]["kind"] == "fake_assistant_provider"


def test_nemoclaw_provider_surfaces_bridge_error_envelope() -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url="http://unused.example/v1/manyforge/assistant",
        request_fn=lambda _payload: {
            "version": "manyforge.assistant.provider_request.v0",
            "schemaVersion": "0.1.0",
            "error": {
                "code": "response_rejected",
                "detail": "upstream model emitted tool calls outside the active catalog",
            },
            "message": None,
            "toolCalls": [],
            "proposals": [],
            "warnings": [
                "upstream model emitted tool calls outside the active catalog"
            ],
            "mutated": False,
            "requiresReview": True,
        },
    )

    with pytest.raises(
        AssistantProviderError,
        match=(
            "response_rejected: upstream model emitted tool calls outside "
            "the active catalog"
        ),
    ):
        provider.run(_request(), _context())


def test_provider_bridge_smoke_script_round_trips_through_http_fixture(
    fake_provider_base_url: str,
) -> None:
    completed = subprocess.run(
        [
            sys.executable,
            str(_SMOKE_SCRIPT),
            "--endpoint",
            f"{fake_provider_base_url}/v1/manyforge/assistant",
            "--provider-id",
            "openclaw",
            "--assistant-mode",
            "composer-assistant",
            "--requested-tool",
            "inspect_isaac_scene",
            "--requested-tool",
            "propose_scene_objects",
            "--timeout-s",
            "2",
        ],
        cwd=str(_REPO_ROOT),
        check=True,
        capture_output=True,
        text=True,
    )

    payload = json.loads(completed.stdout)
    assert payload["ok"] is True
    assert payload["assistantMode"] == "composer-assistant"
    assert payload["toolCallCount"] == 2
    assert payload["proposalCount"] == 1
    assert "fixture provider only" in payload["warnings"]


@pytest.mark.parametrize(
    ("mode", "expected"),
    [
        ("mutating", "must not report state mutation"),
        ("malformed-proposal", "type must be a non-empty string"),
        ("malformed-tool-call", "status must be one of"),
    ],
)
def test_nemoclaw_provider_rejects_bad_http_fixture_responses(
    fake_provider_base_url: str,
    mode: str,
    expected: str,
) -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url=f"{fake_provider_base_url}/assistant?mode={mode}",
        timeout_s=2.0,
    )

    with pytest.raises(AssistantProviderError, match=expected):
        provider.run(_request(), _context())


def test_nemoclaw_provider_maps_http_errors_from_fixture(
    fake_provider_base_url: str,
) -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url=f"{fake_provider_base_url}/assistant?mode=http-error",
        timeout_s=2.0,
    )

    with pytest.raises(AssistantProviderError, match="returned HTTP 503"):
        provider.run(_request(), _context())


def test_nemoclaw_provider_times_out_against_slow_fixture(
    fake_provider_base_url: str,
) -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url=f"{fake_provider_base_url}/assistant?delay_s=0.25",
        timeout_s=0.05,
    )

    with pytest.raises(AssistantProviderError, match="timed out after 0.050s"):
        provider.run(_request(), _context())


def _request() -> AssistantChatRequest:
    return AssistantChatRequest(
        message="Create a planning object from the current scene",
        mode="provider",
        providerId="nemoclaw",
        requestedTools=["inspect_isaac_scene", "propose_scene_objects"],
        context={"sourceRoots": ["/World/soup_can"]},
    )


def _context(conversation_id: str = "conv-http") -> AssistantProviderContext:
    return AssistantProviderContext(
        conversation_id=conversation_id,
        program_loaded=False,
        cycle_state="idle",
        tool_catalog=[
            {
                "name": "inspect_isaac_scene",
                "mutatesState": False,
                "requiresReview": True,
            }
        ],
    )


def _free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _wait_until_ready(base_url: str, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, stderr = process.communicate(timeout=1)
            raise RuntimeError(
                "fake assistant provider exited early: "
                f"stdout={stdout!r} stderr={stderr!r}"
            )
        try:
            response = httpx.get(f"{base_url}/health", timeout=0.2)
            if response.status_code == 200:
                return
        except httpx.HTTPError:
            pass
        time.sleep(0.02)
    raise RuntimeError("fake assistant provider did not become ready")
