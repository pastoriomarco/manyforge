"""Composer does not read the previous-session report (OD-255).

Headless files the report from ``session-reports/pending/`` and raises the
"previous session unclean" alarm; Composer starts and answers health without
it, whatever the report's state when both start together.
"""
from __future__ import annotations

import json
from types import SimpleNamespace

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError


@pytest.mark.parametrize("mode", ["structural", "headless_ready", "headless_failed"])
def test_composer_starts_while_the_report_is_still_pending(tmp_path, monkeypatch, mode):
    identity = "a" * 32
    pending = tmp_path / "session-reports" / "pending"
    pending.mkdir(parents=True)
    (pending / f"{identity}.json").write_text(json.dumps({"session_id": identity}))
    # The final path an earlier launcher named: not there until Headless moves it.
    monkeypatch.setenv(
        "MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT",
        str(tmp_path / "session-reports" / f"{identity}.json"),
    )

    def health():
        if mode == "headless_failed":
            raise HeadlessClientError("projection_unavailable", "Actual current Headless fault")
        return {"status": "ready", "headless_projection_ready": True}

    state = ComposerState(
        application_start_id=0x3F9A0000000000A1,
        headless_client=None if mode == "structural" else SimpleNamespace(health=health),
    )
    client = TestClient(create_app(state))
    response = client.get("/api/health")
    assert response.status_code == 200
    actual = response.json()
    assert "workbench_session_notice" not in actual
    assert actual["status"] == ("ready" if mode == "headless_ready" else "degraded")
    # The assistant is a Deployment component; health no longer reports a bridge.
    assert "assistant_bridge" not in actual["components"]
    if mode == "headless_failed":
        assert actual["runtimeAvailability"]["code"] == "projection_unavailable"
    assert (pending / f"{identity}.json").is_file()
