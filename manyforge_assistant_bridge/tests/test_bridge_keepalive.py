"""The bridge's clients and its server keep the keep-alive ordering."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
import uvicorn

from manyforge_assistant_bridge import bridge
from manyforge_composer.backend.assistant_provider_client import (
    AssistantProviderHttpClient,
)
from manyforge_headless_api.tests.test_internal_http_keepalive import (
    client_keep_alive_expiry,
    server_keep_alive,
)


def test_composer_client_of_the_bridge_expires_idle_connections_first(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(bridge, "_config_from_environment", lambda: object())
    monkeypatch.setattr(bridge, "create_app", lambda _config: object())
    server = server_keep_alive(
        monkeypatch, bridge, lambda: bridge.main(["--fd", "3"])
    )
    client = AssistantProviderHttpClient()
    try:
        expiry = client_keep_alive_expiry(client._client)
    finally:
        client.close()
    assert expiry < server


def test_model_client_of_the_bridge_expires_idle_connections_first(
    tmp_path: Path,
) -> None:
    # The AI model server is not ManyForge's; compare with uvicorn's default
    # keep-alive, which OpenAI-compatible model servers commonly serve with.
    service = bridge.BridgeService(
        bridge.BridgeConfig(
            headless_api_url="http://127.0.0.1:9010",
            composer_base_url="http://127.0.0.1:9000",
            audit_log_path=tmp_path / "audit.jsonl",
        )
    )
    try:
        expiry = client_keep_alive_expiry(service._upstream)
    finally:
        asyncio.run(service.close())
    assert expiry < uvicorn.Config(object(), log_config=None).timeout_keep_alive
