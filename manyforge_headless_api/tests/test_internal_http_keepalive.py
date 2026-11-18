"""Every internal client drops an idle connection before its server closes it."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import httpx
import pytest
import uvicorn

from manyforge_headless_api import cli
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.program_model_transport import (
    OpenAICompatibleProgramModelTransport,
)


def client_keep_alive_expiry(client: httpx.Client | httpx.AsyncClient) -> float:
    """The idle expiry of the client's own connection pool."""

    return client._transport._pool._keepalive_expiry  # type: ignore[attr-defined]


def server_keep_alive(monkeypatch: pytest.MonkeyPatch, module: Any, start) -> int:
    """Start a server through its real launcher and read uvicorn's Config."""

    started: list[uvicorn.Config] = []
    monkeypatch.setattr(
        module.uvicorn,
        "run",
        lambda app, **options: started.append(
            uvicorn.Config(app, log_config=None, **options)
        ),
    )
    start()
    assert len(started) == 1
    return started[0].timeout_keep_alive


def headless_server_keep_alive(monkeypatch: pytest.MonkeyPatch, root: Path) -> int:
    monkeypatch.setattr(cli, "create_app", lambda _config: object())
    args = ["serve", "--cell-id", "cell_a", "--application-start-id", "3f9a0000000000a1"]
    for flag in (
        "install-root", "artifact-root", "node-catalog-path",
        "node-environment-manifest-path", "alarm-catalog-path", "historian-directory",
        "manager-run-root", "execution-run-root",
    ):
        args += [f"--{flag}", str(root / flag)]
    return server_keep_alive(monkeypatch, cli, lambda: cli.main(args))


def test_clients_of_the_headless_api_expire_idle_connections_first(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # Composer, the Program runner, the assistant bridge and the CLI all
    # reach the headless API through this client.
    server = headless_server_keep_alive(monkeypatch, tmp_path)
    with HeadlessHttpClient("http://127.0.0.1:9010") as client:
        expiry = client_keep_alive_expiry(client._client)
    assert expiry < server


def test_the_program_model_client_expires_idle_connections_first() -> None:
    # The AI model server is not ManyForge's; compare with uvicorn's default
    # keep-alive, which OpenAI-compatible model servers commonly serve with.
    transport = OpenAICompatibleProgramModelTransport()
    try:
        expiry = client_keep_alive_expiry(transport._client)
    finally:
        transport.close()
    assert expiry < uvicorn.Config(object(), log_config=None).timeout_keep_alive
