"""Durable Program-cycle state and bounded in-flight command invariants."""

from __future__ import annotations

import os
import hashlib
import json
from unittest.mock import patch
from pathlib import Path
import sqlite3
from threading import Barrier, Thread

import pytest
import rfc8785

from manyforge_program_runner.cycle_journal import (
    SCHEMA_VERSION,
    ProgramCycleJournal,
)


def _k(*parts: str) -> bytes:
    """A deterministic 16-byte request key named by text, for readable tests."""

    return hashlib.md5("/".join(parts).encode()).digest()


def _history_snapshot(run: int, state: str, payload: str = "") -> dict:
    return {"capture": {"program_run_id": run}, "run_revision": 1,
            "state": state, "payload": payload}


def test_finished_history_byte_budget_prunes_oldest_and_keeps_current_and_inflight(tmp_path: Path) -> None:
    # Run numbers below keep the encoded size of the earlier text ids, so the
    # byte budgets are unchanged.
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3",
                                  maximum_finished_history_bytes=230)
    journal.retain_run_snapshot(_history_snapshot(1000001, "succeeded", "é" * 12))
    journal.retain_run_snapshot(_history_snapshot(1000002, "stopped", "é" * 12))
    journal.retain_run_snapshot(_history_snapshot(1000000003, "running", "x" * 250))
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                       state="terminal_awaiting_reset", program_run_id=100000001,
                       run_snapshot=_history_snapshot(100000001, "failed", "z" * 20),
                       terminal_reference="sha256:" + "b" * 64)
    assert journal.read_run_snapshot(1000001) is None
    assert journal.read_run_snapshot(1000002) is not None
    assert journal.read_run_snapshot(100000001) is not None
    assert journal.read_run_snapshot(1000000003) is not None
    # The WAL bound is a setting of the writer's connection.
    assert journal._writer.wait_idle(5.0)
    assert journal._writer._database.execute("PRAGMA journal_size_limit").fetchone()[0] == 230
    journal.close()


def test_oversize_current_is_retained_and_logged_once(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3",
                                  maximum_finished_history_bytes=40)
    with patch("manyforge_program_runner.cycle_journal._LOGGER.warning") as warning:
        journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                           state="terminal_awaiting_reset", program_run_id=100000001,
                           run_snapshot=_history_snapshot(100000001, "failed", "x" * 100),
                           terminal_reference="sha256:" + "b" * 64)
        journal.retain_run_snapshot(_history_snapshot(100000001, "failed", "x" * 100))
    assert journal.read_run_snapshot(100000001) is not None
    warning.assert_called_once()
    journal.close()


def test_history_budget_applies_when_inflight_snapshot_becomes_terminal(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3",
                                  maximum_finished_history_bytes=170)
    journal.retain_run_snapshot(_history_snapshot(10004, "succeeded", "x" * 20))
    journal.retain_run_snapshot(_history_snapshot(10005, "running", "y" * 40))
    finished = _history_snapshot(10005, "succeeded", "y" * 40)
    finished["run_revision"] = 2
    journal.retain_run_snapshot(finished)
    assert journal.read_run_snapshot(10004) is None
    assert journal.read_run_snapshot(10005) == finished
    journal.close()


def test_oversize_terminal_is_pruned_only_after_successful_reset(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3",
                                  maximum_finished_history_bytes=40)
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                       state="terminal_awaiting_reset", program_run_id=100000001,
                       run_snapshot=_history_snapshot(100000001, "failed", "x" * 100),
                       terminal_reference="sha256:" + "b" * 64)
    journal.record_request(request_key=_k("caller.epoch", "reset.1"), kind="reset",
                           intent={"expected_cycle_generation": 1}, cycle_generation=1)
    with pytest.raises(RuntimeError, match="Reset in-flight command is absent"):
        journal.publish_reset(expected_generation=1, request_key=_k("caller.epoch", "missing"),
                              result={"idle_cycle_generation": 2})
    assert journal.read_run_snapshot(100000001) is not None
    journal.publish_reset(expected_generation=1, request_key=_k("caller.epoch", "reset.1"),
                          result={"idle_cycle_generation": 2})
    assert journal.read_run_snapshot(100000001) is None
    journal.close()


def _file_identity(
    path: Path,
) -> tuple[bytes, int, int, int, int, int, set[str]]:
    metadata = path.stat()
    return (
        path.read_bytes(),
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_size,
        metadata.st_mtime_ns,
        {entry.name for entry in path.parent.iterdir()},
    )


@pytest.mark.parametrize("kind", ["journal", "malformed"])
def test_an_existing_file_at_the_path_is_refused_without_mutation(
    tmp_path: Path, kind: str
) -> None:
    path = tmp_path / "cycle.sqlite3"
    if kind == "malformed":
        path.write_bytes(b"not-a-sqlite-database\n")
        path.chmod(0o600)
    else:
        ProgramCycleJournal(path).close()
    os.utime(path, ns=(path.stat().st_atime_ns, 1_700_000_000_000_000_000))
    before = _file_identity(path)

    with pytest.raises(RuntimeError, match="cannot be reserved"):
        ProgramCycleJournal(path)

    assert _file_identity(path) == before


def test_absent_database_creates_only_current_schema_under_existing_parent(
    tmp_path: Path,
) -> None:
    path = tmp_path / "new" / "nested" / "cycle.sqlite3"
    path.parent.mkdir(parents=True)

    journal = ProgramCycleJournal(path)
    journal.close()

    assert path.is_file()
    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        assert database.execute(
            "SELECT schema_version FROM program_cycle_metadata WHERE singleton=1"
        ).fetchone() == (SCHEMA_VERSION,)
        assert {
            row[0]
            for row in database.execute(
                "SELECT name FROM sqlite_schema WHERE type='table'"
            )
        } == {
            "program_cycle_metadata",
            "program_cycle",
            "program_cycle_inflight_commands",
            "program_run_history",
            "program_persistent_value_requests",
        }
    finally:
        database.close()


def test_a_closed_journal_file_holds_its_records_unchanged(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path)
    snapshot = {
        "capture": {"program_run_id": 1},
        "run_revision": 1,
        "state": "stopped",
    }
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id=1,
        run_snapshot=snapshot,
        terminal_reference="sha256:" + "b" * 64,
    )
    stored = journal.read_run_snapshot(1)
    journal.close()

    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        assert database.execute(
            "SELECT state, terminal_reference FROM program_cycle"
        ).fetchone() == ("terminal_awaiting_reset", "sha256:" + "b" * 64)
        rows = database.execute(
            "SELECT program_run_id, run_revision, snapshot_json, terminal "
            "FROM program_run_history"
        ).fetchall()
    finally:
        database.close()
    assert stored == snapshot
    assert [(row[0], row[1], json.loads(row[2]), row[3]) for row in rows] == [
        (1, 1, snapshot, 1)
    ]


def _complete_persistent(
    journal: ProgramCycleJournal,
    request_id: str,
    kind: str = "persistent_value_set",
    outcome: str = "applied",
) -> str:
    record = journal.record_request(
        request_key=_k("composer.epoch.1", request_id),
        kind=kind,
        intent={"request_id": request_id},
        cycle_generation=1,
    )
    if record.disposition != "new":
        return record.disposition
    journal.complete_persistent_value_request(
        request_key=_k("composer.epoch.1", request_id), outcome=outcome
    )
    return "new"


def test_completed_persistent_value_requests_are_bounded_and_reach_the_file(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(
        path,
        maximum_completed_persistent_value_requests=3,
    )
    assert _complete_persistent(journal, "edit.1") == "new"
    assert _complete_persistent(journal, "edit.2", "persistent_value_reset") == "new"
    assert _complete_persistent(journal, "edit.3", "persistent_values_clear") == "new"
    # A resend of a completed request is reported with its action and outcome.
    resent = journal.record_request(
        request_key=_k("composer.epoch.1", "edit.3"),
        kind="persistent_values_clear",
        intent={"request_id": "edit.3"},
        cycle_generation=1,
    )
    assert (resent.disposition, resent.result) == (
        "completed",
        {"action": "clear", "outcome": "applied"},
    )
    # The identity is the journal's request identity: another caller epoch
    # with the same request id is a different request.
    assert journal.record_request(
        request_key=_k("composer.epoch.2", "edit.1"),
        kind="persistent_value_set",
        intent={"request_id": "edit.1"},
        cycle_generation=1,
    ).disposition == "new"
    journal.reject_request(request_key=_k("composer.epoch.2", "edit.1"), code="x", message="x")
    # Only persistent-value requests complete into the table.
    journal.record_request(
        request_key=_k("composer.epoch.1", "start.1"),
        kind="start",
        intent={"request_id": "start.1"},
        cycle_generation=1,
    )
    with pytest.raises(RuntimeError, match="in-flight command is absent"):
        journal.complete_persistent_value_request(
            request_key=_k("composer.epoch.1", "start.1"), outcome="applied"
        )
    journal.reject_request(request_key=_k("composer.epoch.1", "start.1"), code="x", message="x")

    # A fourth completion trims the oldest identity, by insertion order.
    assert _complete_persistent(journal, "edit.4") == "new"
    for request_id in ("edit.2", "edit.3", "edit.4"):
        assert _complete_persistent(journal, request_id) == "completed"
    # The trimmed identity is no longer known; it is a new request.
    assert journal.record_request(
        request_key=_k("composer.epoch.1", "edit.1"),
        kind="persistent_value_set",
        intent={"request_id": "edit.1"},
        cycle_generation=1,
    ).disposition == "new"
    journal.reject_request(request_key=_k("composer.epoch.1", "edit.1"), code="x", message="x")
    in_memory = [
        (key, action, outcome, position)
        for key, (action, outcome, position) in journal._persistent.items()
    ]
    journal.close()

    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        rows = database.execute(
            "SELECT request_key,action,outcome,position FROM program_persistent_value_requests "
            "ORDER BY position"
        ).fetchall()
        assert rows == in_memory
        assert [row[:3] for row in rows] == [
            (_k("composer.epoch.1", "edit.2"), "reset", "applied"),
            (_k("composer.epoch.1", "edit.3"), "clear", "applied"),
            (_k("composer.epoch.1", "edit.4"), "set", "applied"),
        ]
        # The trim is an indexed delete, never a scan of the table.
        plan = " ".join(
            str(row[-1])
            for row in database.execute(
                "EXPLAIN QUERY PLAN DELETE FROM program_persistent_value_requests "
                "WHERE position<=?",
                (1,),
            )
        )
        assert "USING" in plan and "INDEX" in plan, plan
        assert database.execute(
            "SELECT COUNT(*) FROM program_cycle_inflight_commands"
        ).fetchone() == (0,)
    finally:
        database.close()


def test_absent_database_never_repairs_a_missing_parent(tmp_path: Path) -> None:
    path = tmp_path / "missing" / "nested" / "cycle.sqlite3"
    assert not path.parent.exists()

    with pytest.raises(RuntimeError, match="parent cannot be inspected"):
        ProgramCycleJournal(path)

    assert not (tmp_path / "missing").exists()


@pytest.mark.parametrize("suffix", ["-journal", "-wal", "-shm"])
def test_absent_database_rejects_orphan_sidecar_without_mutation(
    tmp_path: Path,
    suffix: str,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    sidecar = Path(f"{path}{suffix}")
    sidecar.write_bytes(b"orphaned durable state\n")
    sidecar.chmod(0o600)
    before = _file_identity(sidecar)

    with pytest.raises(RuntimeError, match="orphan sidecars remain"):
        ProgramCycleJournal(path)

    assert not path.exists()
    assert _file_identity(sidecar) == before


def test_terminal_blocks_new_cycle_and_resolved_commands_do_not_accumulate(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path)
    intent = {"request_id": "start.1", "program": "identity-only"}
    assert (
        journal.record_request(
            request_key=_k("caller.epoch.1", "start.1"),
            kind="start",
            intent=intent,
            cycle_generation=1,
        )
        .disposition
        == "new"
    )
    claim = {
        "intent": {
            "owner_transaction_id": 1,
            "request_key": _k("caller.epoch.1", "start.1").hex(),
        },
        "claim_generation": 7,
        "acquired_admission_generation": 6,
    }
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="starting",
        program_run_id=1,
        claim=claim,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("starting",),
        state="terminal_awaiting_reset",
        capture_identities={"program_run_id": 1},
        run_snapshot={
            "run_revision": 3,
            "state": "succeeded",
            "capture": {"program_run_id": 1},
        },
        terminal_reference="sha256:" + "b" * 64,
    )
    assert journal.read().state == "terminal_awaiting_reset"
    with pytest.raises(RuntimeError, match="stale"):
        journal.transition(
            expected_generation=1,
            expected_states=("idle_after_reset",),
            state="starting",
        )

    reset_intent = {"expected_cycle_generation": 1}
    journal.record_request(
        request_key=_k("caller.epoch.1", "reset.1"),
        kind="reset",
        intent=reset_intent,
        cycle_generation=1,
    )
    release = {
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "request_key": _k("caller.epoch.1", "reset.1").hex(),
        "acquired_admission_generation": 6,
        "claim_generation": 7,
        "settlement": {
            "settlement_kind": "explicit_reset",
            "evidence_digest": "sha256:" + "b" * 64,
        },
    }
    journal.remember_release_intent(
        expected_generation=1, claim_generation=7, release=release
    )
    assert journal.read().release_intent == release
    result = {"idle_cycle_generation": 2}
    journal.publish_reset(
        expected_generation=1,
        request_key=_k("caller.epoch.1", "reset.1"),
        result=result,
    )

    assert journal.read().state == "idle_after_reset"
    assert journal.read().cycle_generation == 2
    assert journal.read().release_intent is None
    finished = {
        "run_revision": 3,
        "state": "succeeded",
        "capture": {"program_run_id": 1},
    }
    assert journal.read_run_snapshot(1) == finished
    reused_identity_is_a_new_stale_intent = journal.record_request(
        request_key=_k("caller.epoch.1", "reset.1"),
        kind="reset",
        intent=reset_intent,
        cycle_generation=1,
    )
    assert reused_identity_is_a_new_stale_intent.disposition == "new"
    journal.reject_request(
        request_key=_k("caller.epoch.1", "reset.1"), code="x", message="x"
    )
    journal.close()

    database = sqlite3.connect(path)
    try:
        assert database.execute(
            "SELECT state, cycle_generation, release_intent_json FROM program_cycle"
        ).fetchone() == ("idle_after_reset", 2, None)
        assert json.loads(database.execute(
            "SELECT snapshot_json FROM program_run_history WHERE program_run_id=1"
        ).fetchone()[0]) == finished
        assert database.execute(
            "SELECT COUNT(*) FROM program_cycle_inflight_commands"
        ).fetchone() == (0,)
    finally:
        database.close()


def test_known_rejection_is_discarded_but_unresolved_request_never_reexecutes(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        maximum_inflight_commands=1,
    )
    first = journal.record_request(
        request_key=_k("caller.1", "start.rejected"),
        kind="start",
        intent={"revision": 1},
        cycle_generation=1,
    )
    assert first.disposition == "new"
    journal.reject_request(
        request_key=_k("caller.1", "start.rejected"),
        code="program_run_draft_applied_mismatch",
        message="Draft and Applied Program revisions differ",
    )
    rejected_identity_is_reusable = journal.record_request(
        request_key=_k("caller.1", "start.rejected"),
        kind="start",
        intent={"revision": 1},
        cycle_generation=2,
    )
    assert rejected_identity_is_reusable.disposition == "new"
    journal.reject_request(
        request_key=_k("caller.1", "start.rejected"),
        code="program_run_draft_applied_mismatch",
        message="Draft and Applied Program revisions differ",
    )

    assert (
        journal.record_request(
            request_key=_k("caller.1", "reset.unknown"),
            kind="reset",
            intent={"revision": 2},
            cycle_generation=2,
        ).disposition
        == "new"
    )
    assert (
        journal.record_request(
            request_key=_k("caller.1", "reset.unknown"),
            kind="reset",
            intent={"revision": 2},
            cycle_generation=3,
        ).disposition
        == "in_progress"
    )
    # Reset has its own single reserve; it does not consume or enlarge the
    # ordinary slot. Runtime state eligibility still rejects Start during Reset.
    assert (
        journal.record_request(
            request_key=_k("caller.1", "ordinary.1"),
            kind="start",
            intent={"revision": 3},
            cycle_generation=3,
        ).disposition
        == "new"
    )
    with pytest.raises(OverflowError, match="full"):
        journal.record_request(
            request_key=_k("caller.1", "start.over-capacity"),
            kind="start",
            intent={"revision": 3},
            cycle_generation=3,
        )
    journal.close()


def test_terminal_record_cannot_be_rewritten(tmp_path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id=1,
        claim={"claim_generation": 1},
        terminal_reference="sha256:" + "a" * 64,
    )
    before = journal.read()
    with pytest.raises(ValueError, match="immutable"):
        journal.transition(
            expected_generation=1,
            expected_states=("terminal_awaiting_reset",),
            state="terminal_awaiting_reset",
            terminal_reference="sha256:" + "b" * 64,
        )
    assert journal.read() == before
    journal.close()


@pytest.mark.parametrize("prior_uncertain", [False, True])
@pytest.mark.parametrize("new_uncertain", [False, True])
def test_reset_failure_never_clears_prior_uncertainty(tmp_path, prior_uncertain, new_uncertain):
    journal = ProgramCycleJournal(tmp_path / "reset.sqlite3")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id=1,
        claim={"claim_generation": 1},
        reconciliation_required=prior_uncertain,
    )
    journal.record_request(
        request_key=_k("operator.1", "uncertain.1"),
        kind="resume_motion",
        intent={"kind": "resume_motion"},
        cycle_generation=1,
    )
    journal.begin_resetting(expected_generation=1, program_run_id=1, causal_request_key=_k("reset.1"))
    journal.fail_reset(
        expected_generation=1,
        program_run_id=1,
        code="reset_failed",
        message="Reset failed",
        reconciliation_required=new_uncertain,
    )
    record = journal.read()
    assert record.state == "recovery_required"
    assert record.reconciliation_required is (prior_uncertain or new_uncertain)
    assert record.claim == {"claim_generation": 1}
    assert record.reset_phase == "stopping"
    assert (
        journal.record_request(
            request_key=_k("operator.1", "uncertain.1"),
            kind="resume_motion",
            intent={"kind": "resume_motion"},
            cycle_generation=1,
        ).disposition
        == "in_progress"
    )
    journal.close()


@pytest.mark.parametrize("capacity", [1, 4096])
def test_one_reset_reserve_does_not_widen_ordinary_capacity(tmp_path, capacity):
    journal = ProgramCycleJournal(
        tmp_path / "capacity.sqlite3", maximum_inflight_commands=capacity
    )
    # Seeds the in-flight rows directly in memory.
    with journal._lock:
        for i in range(capacity):
            journal._inflight[_k("operator.1", f"move.{i}")] = ("resume_motion", "{}", 1)
    with pytest.raises(OverflowError):
        journal.record_request(
            request_key=_k("operator.1", "extra"),
            kind="resume_motion",
            intent={},
            cycle_generation=1,
        )
    assert (
        journal.record_request(
            request_key=_k("operator.1", "reset.1"),
            kind="reset",
            intent={},
            cycle_generation=1,
        ).disposition
        == "new"
    )
    with pytest.raises(OverflowError):
        journal.record_request(
            request_key=_k("operator.1", "reset.2"),
            kind="reset",
            intent={},
            cycle_generation=1,
        )
    assert len(journal.pending_commands()) == capacity + 1
    assert (
        journal.record_request(
            request_key=_k("operator.1", "reset.1"),
            kind="reset",
            intent={},
            cycle_generation=1,
        ).disposition
        == "in_progress"
    )
    journal.close()


@pytest.mark.parametrize("crossing", [None, "revision"])
def test_reset_discharge_is_exact_and_keeps_original_commands(tmp_path, crossing):
    journal = ProgramCycleJournal(tmp_path / "discharge.sqlite3")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="recovery_required",
        program_run_id=1,
        claim={"claim_generation": 1},
        reconciliation_required=True,
    )
    journal.record_request(
        request_key=_k("operator.1", "resume.1"),
        kind="resume_motion",
        intent={},
        cycle_generation=1,
    )
    pending = journal.pending_commands()
    journal.begin_resetting(
        expected_generation=1,
        program_run_id=1,
        causal_request_key=_k("reset.1"),
        expected_recovery=journal.read(),
    )
    journal.update_reset_phase(
        expected_generation=1,
        program_run_id=1,
        expected_phase="stopping",
        phase="settling",
        terminal_reference="sha256:" + "a" * 64,
    )
    expected = journal.read()
    if crossing == "revision":
        journal.update_reset_phase(
            expected_generation=1,
            program_run_id=1,
            expected_phase="settling",
            phase="settling",
        )
    if crossing:
        before = journal.read()
        with pytest.raises(RuntimeError, match="discharge is stale"):
            journal.settle_reset_recovery(
                expected=expected
            )
        assert journal.read() == before
    else:
        journal.settle_reset_recovery(
            expected=expected
        )
        assert journal.read().reset_phase == "restoring_scene"
        assert not journal.read().reconciliation_required
    assert journal.pending_commands() == pending
    journal.close()


def test_multiple_retained_resets_remain_readable_but_cannot_grow(tmp_path):
    path = tmp_path / "legacy-resets.sqlite3"
    journal = ProgramCycleJournal(path)
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="recovery_required",
        program_run_id=1,
        claim={"claim_generation": 1},
        reconciliation_required=True,
    )
    # Seeds two Reset rows directly in memory.
    with journal._lock:
        for index in range(2):
            journal._inflight[_k("operator.1", f"reset.{index}")] = ("reset", "{}", 1)
    before = journal.pending_commands()
    assert len(before) == 2
    for kind in ("reset", "resume_motion"):
        with pytest.raises(OverflowError):
            journal.record_request(
                request_key=_k("operator.1", "new.1"),
                kind=kind,
                intent={},
                cycle_generation=1,
            )
        assert journal.pending_commands() == before
    journal.close()


def test_terminal_observation_is_exact_idempotent_and_race_safe(tmp_path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="running",
        program_run_id=1,
        claim={"claim_generation": 1},
    )
    reference = "sha256:" + "c" * 64
    snapshot = {
        "run_revision": 2,
        "state": "succeeded",
        "capture": {"program_run_id": 1},
    }
    barrier = Barrier(3)
    errors: list[BaseException] = []

    def observe() -> None:
        barrier.wait()
        try:
            journal.observe_terminal(
                expected_generation=1,
                program_run_id=1,
                terminal_reference=reference,
                snapshot=snapshot,
            )
        except BaseException as error:
            errors.append(error)

    threads = [Thread(target=observe), Thread(target=observe)]
    for thread in threads:
        thread.start()
    barrier.wait()
    for thread in threads:
        thread.join()

    assert errors == []
    terminal = journal.read()
    assert terminal.state == "terminal_awaiting_reset"
    assert terminal.terminal_reference == reference
    with pytest.raises(RuntimeError, match="conflicts"):
        journal.observe_terminal(
            expected_generation=1,
            program_run_id=1,
            terminal_reference="sha256:" + "d" * 64,
            snapshot=snapshot,
        )
    assert journal.read() == terminal
    journal.close()


def test_reset_option_defaults_preserve_exact_request_identity(tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    original ={"request_id": "reset.1", "unrelated": "must stay exact"}
    try:
        assert journal.record_request(request_key=_k("caller.1", "reset.1"), kind="reset",
                                      intent=original, cycle_generation=1).disposition == "new"
        defaults = {**original, "reload_initial_scene": False, "reset_robot_position": False}
        assert journal.record_request(request_key=_k("caller.1", "reset.1"), kind="reset",
                                      intent=defaults, cycle_generation=1).disposition == "in_progress"
        for changed in ({**defaults, "reload_initial_scene": True},
                        {**defaults, "reset_robot_position": True},
                        {**defaults, "unrelated": "changed"}):
            with pytest.raises(ValueError, match="different intent"):
                journal.record_request(request_key=_k("caller.1", "reset.1"), kind="reset",
                                       intent=changed, cycle_generation=1)
        assert journal.pending_commands()[0]["intent"] == original
    finally:
        journal.close()



def test_the_journal_keys_by_request_key(
    tmp_path: Path,
) -> None:
    """Schema 11: no owner epoch per row, integer run numbers and 16-byte request keys."""

    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path)
    key = _k("operator", "start.1")
    journal.record_request(
        request_key=key, kind="start", intent={}, cycle_generation=1
    )
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id=1,
        evidence={"claim_kind": "program_cycle"},
        causal_request_key=key,
    )
    record = journal.read()
    assert record.program_run_id == 1
    assert record.causal_request_key == key
    assert journal.pending_commands()[0]["request_key"] == key
    journal.close()

    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        assert database.execute(
            "SELECT causal_request_key FROM program_cycle"
        ).fetchone() == (key,)
        columns = {row[1] for row in database.execute("PRAGMA table_info(program_cycle)")}
        assert "owner_epoch" not in columns
        assert "causal_request_key" in columns
        assert not {"causal_correlation_id", "causal_session_id"} & columns
    finally:
        database.close()
