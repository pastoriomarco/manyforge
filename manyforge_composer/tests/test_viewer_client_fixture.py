from __future__ import annotations

import asyncio
import importlib.util
from pathlib import Path
import sys
from typing import Any


_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts" / "qualification" / "viewer_client_fixture.py"
_SPEC = importlib.util.spec_from_file_location("viewer_client_fixture", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
fixture = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = fixture
_SPEC.loader.exec_module(fixture)


class _Clock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        self.value += 0.05
        return self.value


class _Socket:
    def __init__(self) -> None:
        self.sent: list[str] = []
        self.receive_calls = 0

    async def send(self, value: str) -> None:
        self.sent.append(value)

    async def recv(self) -> str:
        self.receive_calls += 1
        return '{"type":"frame"}'


def _config(mode: str) -> Any:
    return fixture.FixtureConfig(
        url="ws://example.test/ws/scene/viewer",
        mode=mode,
        duration_s=0.35,
        renew_interval_s=0.1,
        requested_rate_hz=15.0,
        layers=("canonical", "diagnostics"),
        qualification_send_delay_ms=250.0,
    )


def test_slow_fixture_renews_lease_and_remains_a_normal_protocol_consumer() -> None:
    socket = _Socket()
    result = asyncio.run(
        fixture._run_connected(socket, _config("slow"), monotonic=_Clock())
    )

    assert result["leases_sent"] >= 1
    assert result["messages_received"] >= 1
    assert socket.receive_calls >= 1


def test_normal_fixture_consumes_frames_and_renews_lease() -> None:
    socket = _Socket()
    result = asyncio.run(
        fixture._run_connected(socket, _config("normal"), monotonic=_Clock())
    )

    assert result["leases_sent"] >= 1
    assert result["messages_received"] >= 1
    assert socket.receive_calls >= 1


def test_token_is_added_without_discarding_existing_query() -> None:
    assert (
        fixture._authenticated_url("ws://host/ws?existing=1", "secret")
        == "ws://host/ws?existing=1&token=secret"
    )


def test_slow_fixture_requests_the_gated_writer_delay() -> None:
    assert (
        fixture._fixture_url(_config("slow"))
        == "ws://example.test/ws/scene/viewer?qualification_send_delay_ms=250"
    )
