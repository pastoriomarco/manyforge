"""Unit coverage for the model-serving crash-monitor component.

Exercises _model_serving_component(): the fail-closed reader that turns the
out-of-band watcher status file into a runtime-status component. The watcher
itself (serving/vllm-status-watch.sh) is validated separately at the shell
level; here we pin the contract the Composer relies on.
"""

from __future__ import annotations

import json
import time

import pytest

pytest.importorskip("fastapi")

from manyforge_composer.backend import routes_runtime as rr


def _write(tmp_path, **overrides) -> str:
    payload = {
        "schema": "manyforge.serving.vllm_status.v1",
        "container": "manyforge-e2e-vllm",
        "state": "running",
        "phase": "running",
        "exitCode": None,
        "oomKilled": False,
        "reason": "",
        "updatedAt": time.time() * 1000.0,
        "pollIntervalMs": 2000,
    }
    payload.update(overrides)
    path = tmp_path / "vllm-status.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return str(path)


def test_missing_file_omits_component(tmp_path, monkeypatch):
    # No model launch observed (e.g. composer-only) -> no component, no fault.
    monkeypatch.setattr(rr, "_VLLM_STATUS_FILE", str(tmp_path / "absent.json"))
    component, is_fault = rr._model_serving_component()
    assert component is None
    assert is_fault is False


def test_absent_state_omits_component(tmp_path, monkeypatch):
    # Watcher up but no serving container ever observed (composer-only).
    monkeypatch.setattr(rr, "_VLLM_STATUS_FILE", _write(tmp_path, state="absent"))
    component, is_fault = rr._model_serving_component()
    assert component is None
    assert is_fault is False


def test_running_is_healthy(tmp_path, monkeypatch):
    monkeypatch.setattr(rr, "_VLLM_STATUS_FILE", _write(tmp_path, state="running"))
    component, is_fault = rr._model_serving_component()
    assert component["status"] == "healthy"
    assert is_fault is False


def test_failed_is_fault_with_exit_code(tmp_path, monkeypatch):
    monkeypatch.setattr(
        rr,
        "_VLLM_STATUS_FILE",
        _write(tmp_path, state="failed", reason="non-zero exit (code 1)", exitCode=1),
    )
    component, is_fault = rr._model_serving_component()
    assert component["status"] == "fault"
    assert is_fault is True
    assert "exit 1" in component["detail"]


def test_stale_file_fails_closed_to_waiting(tmp_path, monkeypatch):
    # A frozen "running" from a dead watcher must NOT read as ready.
    stale_ms = (time.time() - 3600) * 1000.0
    monkeypatch.setattr(
        rr, "_VLLM_STATUS_FILE", _write(tmp_path, state="running", updatedAt=stale_ms)
    )
    component, is_fault = rr._model_serving_component()
    assert component["status"] == "waiting"
    assert is_fault is False
    assert "stale" in component["detail"].lower()


def test_corrupt_file_fails_closed(tmp_path, monkeypatch):
    path = tmp_path / "vllm-status.json"
    path.write_text("{not json", encoding="utf-8")
    monkeypatch.setattr(rr, "_VLLM_STATUS_FILE", str(path))
    component, is_fault = rr._model_serving_component()
    assert component["status"] == "waiting"
    assert is_fault is False


def test_stopped_is_disabled(tmp_path, monkeypatch):
    monkeypatch.setattr(
        rr, "_VLLM_STATUS_FILE", _write(tmp_path, state="stopped", reason="graceful exit (code 0)")
    )
    component, is_fault = rr._model_serving_component()
    assert component["status"] == "disabled"
    assert is_fault is False
