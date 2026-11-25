"""Each Program Reset terminal is one History row (OD-305)."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.models import ProgramCycleResetRequest
from manyforge_program_runner.runtime import ProgramRunnerConfig, ProgramRunnerError, ProgramRuntime
from manyforge_program_runner.tests.test_runtime import (
    _A,
    _DIGEST_B,
    _Admission,
    _Headless,
    _key,
)

_CONTENT_HASH = "sha256:" + "c" * 64


class _RecordingHeadless(_Headless):
    def __init__(self, *, record_error: Exception | None = None) -> None:
        super().__init__()
        self.reset_outcomes: list[dict[str, Any]] = []
        self.record_error = record_error

    def record_program_reset_outcome(self, body: dict[str, Any]) -> dict[str, Any]:
        self.reset_outcomes.append(body)
        if self.record_error is not None:
            raise self.record_error
        return {"recorded": True}


class _AuthoringAdmission(_Admission):
    def __init__(self, authoring_allowed: bool = True) -> None:
        super().__init__()
        self.authoring_allowed = authoring_allowed

    def get(self) -> dict[str, Any]:
        return {"admission_generation": 1, "authoring_allowed": self.authoring_allowed}


def _runtime(tmp_path, headless: _RecordingHeadless, *, authoring_allowed: bool = True):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless, gateway=_Headless(),
        admission_client=_AuthoringAdmission(authoring_allowed), cycle_journal=journal,
    )
    return runtime, journal


def _reload(request_id: str) -> ProgramCycleResetRequest:
    return ProgramCycleResetRequest.model_validate({
        "request_key": _key(request_id), "source_label": "operator_ui",
        "program_run_id": None, "expected_cycle_generation": 1,
        "reload_initial_scene": True,
    })


def _scene_readback(runtime: ProgramRuntime, headless: _RecordingHeadless, monkeypatch) -> None:
    """The verified readback leaves scene revision 10."""

    def prepare(**_kwargs):
        return (headless.program_applied(), {"proof": True},
                {"scene_id": "scene.1", "revision_id": 10, "content_hash": _CONTENT_HASH})

    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", prepare)


def test_a_reset_that_reloads_the_scene_records_what_it_used_and_left(monkeypatch, tmp_path):
    headless = _RecordingHeadless()
    runtime, journal = _runtime(tmp_path, headless)
    _scene_readback(runtime, headless, monkeypatch)
    runtime.start()
    try:
        runtime.reset_cycle(_reload("reset.history.reload"))
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()
    assert headless.reset_outcomes == [{
        "outcome": "succeeded",
        "reset": {
            "reload_initial_scene": True,
            "reset_robot_position": False,
            "reset_stored_values": False,
            "program_run_id": None,
            "program_revision": headless.program_applied()["program_revision"],
            "applied_record_digest": _DIGEST_B,
            "scene_revision_id": 10,
            "scene_content_hash": _CONTENT_HASH,
            "cause_code": None,
        },
    }]


def test_a_refused_reset_records_its_typed_refusal_and_no_scene(monkeypatch, tmp_path):
    headless = _RecordingHeadless()
    runtime, journal = _runtime(tmp_path, headless, authoring_allowed=False)
    monkeypatch.setattr(runtime, "_prepare_current_applied_scene",
                        lambda **_kwargs: pytest.fail("a refused Reset touched the Scene"))
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.reset_cycle(_reload("reset.history.refused"))
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()
    assert refused.value.code == "authoring_not_ready"
    [row] = headless.reset_outcomes
    assert row["outcome"] == "refused"
    assert row["error"]["code"] == "authoring_not_ready"
    assert row["reset"]["reload_initial_scene"] is True
    assert row["reset"]["scene_revision_id"] is None
    assert row["reset"]["applied_record_digest"] is None


def test_a_failed_reset_records_its_typed_error_and_cause(monkeypatch, tmp_path):
    headless = _RecordingHeadless()
    runtime, journal = _runtime(tmp_path, headless)

    def unavailable(**_kwargs):
        raise ProgramRunnerError("scene_owner_unavailable", "logical scene owner unavailable")

    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", unavailable)
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as failed:
            runtime.reset_cycle(_reload("reset.history.failed"))
        assert journal.read().state == "recovery_required"
    finally:
        runtime.shutdown()
    assert failed.value.code == "program_cycle_recovery_required"
    [row] = headless.reset_outcomes
    assert row["outcome"] == "failed"
    assert row["error"]["code"] == "program_cycle_recovery_required"
    assert "logical scene owner unavailable" in row["error"]["message"]
    assert row["reset"]["cause_code"] == "program_cycle_reset_scene_preparation_failed"
    assert row["reset"]["scene_revision_id"] is None


def test_a_lost_history_row_never_changes_the_reset_result(monkeypatch, tmp_path, caplog):
    headless = _RecordingHeadless(record_error=RuntimeError("headless unavailable"))
    runtime, journal = _runtime(tmp_path, headless)
    _scene_readback(runtime, headless, monkeypatch)
    runtime.start()
    try:
        result = runtime.reset_cycle(_reload("reset.history.lost"))
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()
    assert result["idle_cycle_generation"] == 2
    assert len(headless.reset_outcomes) == 1
    assert "program_reset_outcome_record_failed" in caplog.text
