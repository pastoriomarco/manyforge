"""Clients of Composer drop an idle connection before Composer closes it."""

from __future__ import annotations

import asyncio
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

import manyforge_composer.__main__ as composer_main
from manyforge_assistant_bridge.bridge import BridgeConfig, BridgeService
from manyforge_headless_api.tests.test_internal_http_keepalive import (
    client_keep_alive_expiry,
    server_keep_alive,
)
from manyforge_program_runner.snapshot_source import ComposerProgramSnapshotSource


def _composer_server_keep_alive(monkeypatch: pytest.MonkeyPatch) -> int:
    monkeypatch.delenv("MANYFORGE_STATE_DIR", raising=False)
    monkeypatch.setattr(composer_main, "setup_logging", lambda _level: None)
    monkeypatch.setattr(
        composer_main,
        "ComposerState",
        SimpleNamespace(from_paths=lambda **_kwargs: SimpleNamespace(assistant_client=None)),
    )
    monkeypatch.setattr(composer_main, "create_app", lambda *_args, **_kwargs: object())
    monkeypatch.setattr(
        sys, "argv", ["manyforge_composer", "--application-start-id", "3f9a00000000002a"]
    )
    return server_keep_alive(monkeypatch, composer_main, composer_main.main)


def test_clients_of_composer_expire_idle_connections_first(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    server = _composer_server_keep_alive(monkeypatch)
    # The Program runner's snapshot capture.
    snapshot = ComposerProgramSnapshotSource(
        "http://127.0.0.1:9000/api/scene/snapshot/render"
    )
    # The assistant bridge's Draft callbacks.
    bridge = BridgeService(
        BridgeConfig(
            headless_api_url="http://127.0.0.1:9010",
            composer_base_url="http://127.0.0.1:9000",
            audit_log_path=tmp_path / "audit.jsonl",
        )
    )
    try:
        expiries = [
            client_keep_alive_expiry(snapshot._client),
            client_keep_alive_expiry(bridge._composer),
        ]
    finally:
        snapshot.close()
        asyncio.run(bridge.close())
    assert all(expiry < server for expiry in expiries)
