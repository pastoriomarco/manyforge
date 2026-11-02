from __future__ import annotations

import json
from types import SimpleNamespace

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.workbench_session import previous_session_notice
from manyforge_headless_api.client import HeadlessClientError


def report(shutdown="unknown", effects="unknown"):
    identity = "a" * 32
    return {
        "schema_version": "manyforge.workbench_previous_session.v1",
        "session_id": identity,
        "archive_relative_path": "previous-sessions/" + identity,
        "shutdown": shutdown,
        "effects": effects,
        "history_coverage": "not_imported",
        "journal_families": ["program_cycle", "scene", "manager", "kernel_operation"],
        "message": "Previous session preserved; Scene and Manager facts unknown. No old state was adopted. Previous journal events may be absent from History.",
    }


@pytest.mark.parametrize("shutdown,effects,visible", [
    ("clean", "closed", False), ("clean", "unknown", False), ("clean", "unresolved", True),
    ("unknown", "closed", True), ("unknown", "unknown", True), ("unknown", "unresolved", True),
])
def test_notice_uses_closed_shutdown_and_positive_unresolved_facts(tmp_path, shutdown, effects, visible):
    path = tmp_path / "report.json"
    value = report(shutdown, effects)
    path.write_text(json.dumps(value))
    before = path.read_bytes()
    assert previous_session_notice(str(path)) == (value if visible else None)
    assert path.read_bytes() == before


@pytest.mark.parametrize("key,value", [
    ("schema_version", "another.schema"), ("session_id", "../outside"),
    ("archive_relative_path", "previous-sessions/another-session"),
    ("shutdown", "assumed_clean"), ("effects", "assumed_closed"),
    ("history_coverage", "imported"), ("journal_families", ["program_cycle"]),
    ("message", ""), ("message", "x" * 1025),
])
def test_invalid_report_is_refused_even_if_it_claims_clean_shutdown(tmp_path, key, value):
    path = tmp_path / "report.json"
    invalid = {**report("clean", "closed"), key: value}
    path.write_text(json.dumps(invalid))
    with pytest.raises(ValueError):
        previous_session_notice(str(path))


def test_missing_report_is_not_silently_treated_as_an_empty_previous_session(tmp_path):
    assert previous_session_notice(None) is None
    with pytest.raises(FileNotFoundError):
        previous_session_notice(str(tmp_path / "missing.json"))


def test_report_symlink_and_oversized_input_are_refused_without_writing(tmp_path):
    original = tmp_path / "original.json"
    original.write_text(json.dumps(report()))
    link = tmp_path / "report.json"
    link.symlink_to(original)
    before = original.read_bytes()
    with pytest.raises(OSError):
        previous_session_notice(str(link))
    assert original.read_bytes() == before
    oversized = tmp_path / "oversized.json"
    oversized.write_bytes(b" " * (64 * 1024 + 1))
    with pytest.raises(ValueError, match="oversized"):
        previous_session_notice(str(oversized))


@pytest.mark.parametrize("mode", ["structural", "headless_ready", "headless_failed"])
@pytest.mark.parametrize("shutdown,effects,visible", [
    ("clean", "unknown", False), ("clean", "unresolved", True), ("unknown", "unknown", True),
])
def test_existing_health_path_preserves_notice_without_changing_readiness(tmp_path, monkeypatch, mode, shutdown, effects, visible):
    path = tmp_path / "report.json"
    value = report(shutdown, effects)
    path.write_text(json.dumps(value))
    monkeypatch.setenv("MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT", str(path))

    def health():
        if mode == "headless_failed":
            raise HeadlessClientError("projection_unavailable", "Actual current Headless fault")
        return {"status": "ready", "headless_projection_ready": True}

    state = ComposerState(headless_client=None if mode == "structural" else SimpleNamespace(health=health))
    client = TestClient(create_app(state))
    response = client.get("/api/health")
    assert response.status_code == 200
    actual = response.json()
    assert actual["workbench_session_notice"] == (value if visible else None)
    assert actual["status"] == ("ready" if mode == "headless_ready" else "degraded")
    if mode == "headless_failed":
        assert actual["runtimeAvailability"]["code"] == "projection_unavailable"
