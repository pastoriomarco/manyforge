from __future__ import annotations

from pathlib import Path

import pytest

from scripts.lib import workbench_session as sessions


@pytest.mark.parametrize("has_claim", [False, True])
def test_orderly_manager_loss_on_real_idle_cycle_is_unknown_while_active_claim_is_unresolved(tmp_path: Path, has_claim: bool):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    journal_path = tmp_path / "program-cycle.sqlite3"
    owner = ProgramCycleJournal(journal_path, owner_epoch="runner.old")
    if has_claim:
        owner.transition(expected_generation=1, expected_states=("idle_after_reset",),
            state="running", program_run_id="run.1", claim={"claim_generation": 1},
            reconciliation_required=False)
    owner.retain_liveness_loss(expected_generation=1,
        manager_runtime_epoch="manager.runtime.1", manager_owner_epoch="manager.owner.1",
        fault_reference="manager.loss.1")
    retained = owner.read()
    assert retained.reconciliation_required is True
    assert retained.state == ("running" if has_claim else "idle_after_reset")
    assert retained.program_run_id == ("run.1" if has_claim else None)
    owner.close()
    before = {path.name: (path.read_bytes(), path.stat().st_ino)
        for path in tmp_path.iterdir() if path.is_file()}

    effects, unknown, unresolved = sessions._effect_observations(tmp_path)

    if has_claim:
        assert effects == "unresolved"
        assert "program_cycle" in unresolved
    else:
        assert effects == "unknown"
        assert "program_cycle" in unknown
        assert "program_cycle" not in unresolved
    assert {path.name: (path.read_bytes(), path.stat().st_ino)
        for path in tmp_path.iterdir() if path.is_file()} == before


def test_idle_cycle_with_a_named_run_is_never_diagnosed_as_empty(tmp_path: Path):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    owner = ProgramCycleJournal(tmp_path / "program-cycle.sqlite3", owner_epoch="runner.old")
    owner.transition(expected_generation=1, expected_states=("idle_after_reset",),
        state="idle_after_reset", program_run_id="run.1", claim=None,
        reconciliation_required=False)
    owner.close()

    effects, _unknown, unresolved = sessions._effect_observations(tmp_path)

    assert effects == "unresolved"
    assert "program_cycle" in unresolved


@pytest.mark.parametrize("has_claim", [False, True])
def test_real_retained_manager_loss_reaches_the_approved_history_only_or_visible_notice(tmp_path: Path, has_claim: bool):
    from manyforge_composer.backend.workbench_session import previous_session_notice
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    root = tmp_path / "state"
    root.mkdir()
    initial = sessions.Session.direct(root)
    owner = ProgramCycleJournal(root / "current/program-cycle.sqlite3", owner_epoch="runner.old")
    if has_claim:
        owner.transition(expected_generation=1, expected_states=("idle_after_reset",),
            state="running", program_run_id="run.1", claim={"claim_generation": 1},
            reconciliation_required=False)
    owner.retain_liveness_loss(expected_generation=1, manager_runtime_epoch="manager.runtime.1",
        manager_owner_epoch="manager.owner.1", fault_reference="manager.loss.1")
    owner.close()
    initial.stopped(verified=True)
    initial.close()

    successor = sessions.Session.direct(root)
    try:
        report = successor.marker["previous_session"]
        assert report["shutdown"] == "clean"
        assert report["effects"] == ("unresolved" if has_claim else "unknown")
        pending = root / "session-reports/pending" / (report["session_id"] + ".json")
        notice = previous_session_notice(str(pending))
        assert (notice is not None) is has_claim
        assert (root / report["archive_relative_path"] / "program-cycle.sqlite3").is_file()
    finally:
        successor.close()
