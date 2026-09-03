"""Program-owner direct execution transport boundary."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.execution_transport import (
    ProgramRunnerExecutionTransport,
)


def _stop_body() -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "program-stop.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "manyforge_behavior",
            "correlation_id": "program-run.1",
        },
        "program_run_id": "program-run.1",
        "reason": "operator Stop",
    }


def test_transport_reuses_the_owner_client_with_internal_attribution(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    commands: list[dict[str, Any]] = []

    def exchange(
        _client: ExecutionCommandClient,
        command: dict[str, Any],
        _socket_path: Path,
    ) -> dict[str, Any]:
        commands.append(command)
        kind = command["command"]
        if kind == "identity":
            return {"api_version": "test", "command": kind, "ok": True}
        if kind == "stop_program_run":
            return {
                "api_version": "test",
                "accepted": {"request_id": "program-stop.1"},
                "command": kind,
                "ok": True,
            }
        if kind == "release_program_run_stop_fence":
            return {
                "api_version": "test",
                "command": kind,
                "ok": True,
            }
        assert kind == "read_stop"
        return {
            "api_version": "test",
            "command": kind,
            "ok": True,
            "stop_status": {
                "request_id": "program-stop.1",
                "schema_version": "manyforge.execution_stop_status.v1",
                "state": "succeeded",
                "stop_generation": 1,
                "stop_id": "stop.1",
            },
        }

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange)
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=3.5)

    assert transport.identity()["ok"] is True
    assert transport.stop_program_run(_stop_body())["ok"] is True
    assert transport.stop_status("stop.1")["stop_status"]["state"] == "succeeded"
    assert transport.release_program_run_stop_fence(
        "program-run.1", "stop.1", 1
    )["ok"] is True
    assert commands[1]["attribution"] == {
        "transport": "internal",
        "assurance": "unverified",
        "principal_id": None,
    }
    assert commands[1]["request"] == _stop_body()
    assert commands[3] == {
        "api_version": "manyforge.execution_command.v1",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "release_program_run_stop_fence",
        "program_run_id": "program-run.1",
        "stop_generation": 1,
        "stop_id": "stop.1",
    }


def test_transport_preserves_validation_and_owner_error_codes(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    def unavailable(
        _client: ExecutionCommandClient,
        command: dict[str, Any],
        _socket_path: Path,
    ) -> dict[str, Any]:
        return {
            "api_version": "test",
            "command": command["command"],
            "error": {
                "code": "execution_owner_unavailable",
                "message": "owner is restarting",
            },
            "ok": False,
        }

    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=3.5)
    invalid = _stop_body()
    invalid["unexpected"] = True
    with pytest.raises(HeadlessApiError) as validation:
        transport.stop_program_run(invalid)
    assert validation.value.code == "execution_command_invalid"

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", unavailable)
    with pytest.raises(HeadlessApiError) as owner:
        transport.stop_program_run(_stop_body())
    assert owner.value.code == "execution_owner_unavailable"
