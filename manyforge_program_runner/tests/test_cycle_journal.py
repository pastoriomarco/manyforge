"""Durable Program-cycle state and bounded in-flight command invariants."""

from __future__ import annotations

import os
import hashlib
from unittest.mock import patch
from pathlib import Path
import sqlite3
from threading import Barrier, Thread

import pytest
import rfc8785

from manyforge_program_runner.cycle_journal import (
    SCHEMA_VERSION,
    ProgramCycleJournal,
    ProgramCycleJournalVersionError,
    ProgramCycleStopWon,
)


def _history_snapshot(run: str, state: str, payload: str = "") -> dict:
    return {"capture": {"program_run_id": run}, "run_revision": 1,
            "state": state, "payload": payload}


def test_finished_history_byte_budget_prunes_oldest_and_keeps_current_and_inflight(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.test",
                                  maximum_finished_history_bytes=230)
    journal.retain_run_snapshot(_history_snapshot("old.1", "succeeded", "é" * 12))
    journal.retain_run_snapshot(_history_snapshot("old.2", "stopped", "é" * 12))
    journal.retain_run_snapshot(_history_snapshot("inflight", "running", "x" * 250))
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                       state="terminal_awaiting_reset", program_run_id="current",
                       run_snapshot=_history_snapshot("current", "failed", "z" * 20),
                       terminal_reference="sha256:" + "b" * 64)
    assert journal.read_run_snapshot("old.1") is None
    assert journal.read_run_snapshot("old.2") is not None
    assert journal.read_run_snapshot("current") is not None
    assert journal.read_run_snapshot("inflight") is not None
    assert journal._database.execute("PRAGMA journal_size_limit").fetchone()[0] == 230
    journal.close()


def test_oversize_current_is_retained_and_logged_once(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.test",
                                  maximum_finished_history_bytes=40)
    with patch("manyforge_program_runner.cycle_journal._LOGGER.warning") as warning:
        journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                           state="terminal_awaiting_reset", program_run_id="current",
                           run_snapshot=_history_snapshot("current", "failed", "x" * 100),
                           terminal_reference="sha256:" + "b" * 64)
        journal.retain_run_snapshot(_history_snapshot("current", "failed", "x" * 100))
    assert journal.read_run_snapshot("current") is not None
    warning.assert_called_once()
    journal.close()


def test_finished_history_budget_is_applied_when_existing_journal_opens(tmp_path: Path) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.test", maximum_finished_history_bytes=1000)
    journal.retain_run_snapshot(_history_snapshot("old.1", "succeeded", "x" * 30))
    journal.retain_run_snapshot(_history_snapshot("old.2", "succeeded", "x" * 30))
    journal.close()
    reopened = ProgramCycleJournal(path, owner_epoch="runner.test", maximum_finished_history_bytes=130)
    assert reopened.read_run_snapshot("old.1") is None
    assert reopened.read_run_snapshot("old.2") is not None
    reopened.close()


def test_history_budget_applies_when_inflight_snapshot_becomes_terminal(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.test",
                                  maximum_finished_history_bytes=170)
    journal.retain_run_snapshot(_history_snapshot("old", "succeeded", "x" * 20))
    journal.retain_run_snapshot(_history_snapshot("run", "running", "y" * 40))
    finished = _history_snapshot("run", "succeeded", "y" * 40)
    finished["run_revision"] = 2
    journal.retain_run_snapshot(finished)
    assert journal.read_run_snapshot("old") is None
    assert journal.read_run_snapshot("run") == finished
    journal.close()


def test_oversize_terminal_is_pruned_only_after_successful_reset(tmp_path: Path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.test",
                                  maximum_finished_history_bytes=40)
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                       state="terminal_awaiting_reset", program_run_id="current",
                       run_snapshot=_history_snapshot("current", "failed", "x" * 100),
                       terminal_reference="sha256:" + "b" * 64)
    journal.record_request(owner_epoch="caller.epoch", request_id="reset.1", kind="reset",
                           intent={"expected_cycle_generation": 1}, cycle_generation=1)
    with pytest.raises(RuntimeError, match="Reset in-flight command is absent"):
        journal.publish_reset(expected_generation=1, owner_epoch="runner.test",
                              request_owner_epoch="caller.epoch", request_id="missing",
                              result={"idle_cycle_generation": 2})
    assert journal.read_run_snapshot("current") is not None
    journal.publish_reset(expected_generation=1, owner_epoch="runner.test",
                          request_owner_epoch="caller.epoch", request_id="reset.1",
                          result={"idle_cycle_generation": 2})
    assert journal.read_run_snapshot("current") is None
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


def _database_file_identities(path: Path) -> dict[str, tuple[bytes, int, int, int, int]]:
    result: dict[str, tuple[bytes, int, int, int, int]] = {}
    for candidate in (path, Path(f"{path}-wal"), Path(f"{path}-shm")):
        try:
            metadata = candidate.lstat()
        except FileNotFoundError:
            continue
        result[candidate.name] = (
            candidate.read_bytes(),
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_mode,
            metadata.st_mtime_ns,
        )
    return result


@pytest.mark.parametrize(
    "kind",
    [
        "malformed",
        "old_schema",
        "future_schema",
        "extra_table",
        "weakened_constraint",
        "malformed_row",
    ],
)
def test_existing_unsupported_database_is_rejected_without_mutation(
    tmp_path: Path, kind: str
) -> None:
    path = tmp_path / "cycle.sqlite3"
    if kind == "malformed":
        path.write_bytes(b"not-a-sqlite-database\n")
    else:
        journal = ProgramCycleJournal(path, owner_epoch="runner.old")
        journal.close()
        database = sqlite3.connect(path)
        if kind in {"old_schema", "future_schema"}:
            database.execute("PRAGMA ignore_check_constraints=ON")
            database.execute(
                "UPDATE program_cycle_metadata SET schema_version=? WHERE singleton=1",
                (SCHEMA_VERSION - 1 if kind == "old_schema" else SCHEMA_VERSION + 1,),
            )
        elif kind == "extra_table":
            database.execute("CREATE TABLE unsupported_old_state(value TEXT)")
        elif kind == "weakened_constraint":
            database.execute("PRAGMA writable_schema=ON")
            database.execute(
                "UPDATE sqlite_schema SET sql=replace(sql,"
                f"' CHECK(schema_version={SCHEMA_VERSION})','') "
                "WHERE name='program_cycle_metadata'"
            )
            database.execute("PRAGMA writable_schema=OFF")
        else:
            database.execute(
                "INSERT INTO program_cycle_inflight_commands VALUES(?,?,?,?,?)",
                ("caller", "broken", "start", "{", 1),
            )
        database.commit()
        database.close()
    os.utime(path, ns=(path.stat().st_atime_ns, 1_700_000_000_000_000_000))
    before = _file_identity(path)

    with pytest.raises(RuntimeError):
        ProgramCycleJournal(path, owner_epoch="runner.new")

    assert _file_identity(path) == before


def test_absent_database_creates_only_current_schema_under_existing_parent(
    tmp_path: Path,
) -> None:
    path = tmp_path / "new" / "nested" / "cycle.sqlite3"
    path.parent.mkdir(parents=True)

    journal = ProgramCycleJournal(path, owner_epoch="runner.current")
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
            "program_runner_liveness",
            "program_run_history",
            "program_persistent_value_requests",
        }
    finally:
        database.close()


@pytest.mark.parametrize("found", [3, 4, 5, 6, SCHEMA_VERSION - 1, SCHEMA_VERSION + 1])
def test_other_journal_versions_are_refused_unchanged_with_fresh_start_guidance(
    tmp_path: Path, found: int
) -> None:
    """Development-only cutover: no migration, reinterpretation or deletion."""

    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="run.1",
        run_snapshot={
            "capture": {"program_run_id": "run.1"},
            "run_revision": 1,
            "state": "stopped",
        },
        terminal_reference="sha256:" + "b" * 64,
    )
    journal.close()
    database = sqlite3.connect(path)
    database.execute("PRAGMA ignore_check_constraints=ON")
    database.execute(
        "UPDATE program_cycle_metadata SET schema_version=? WHERE singleton=1",
        (found,),
    )
    database.commit()
    database.close()
    before = _database_file_identities(path)

    with pytest.raises(ProgramCycleJournalVersionError) as refused:
        ProgramCycleJournal(path, owner_epoch="runner.new")

    assert refused.value.found_version == found
    message = str(refused.value)
    assert f"version {found} is unsupported" in message
    assert f"only version {SCHEMA_VERSION}" in message
    if found < SCHEMA_VERSION:
        assert "Settle and Reset the run using the old version, or clean-start" in message
    else:
        assert "Settle and Reset the run using the version that created it, or clean-start" in message
    assert "compose-dev.sh stop" in message and "--keep-evidence" in message
    assert _database_file_identities(path) == before
    assert {entry.name for entry in tmp_path.iterdir()} == {path.name}


def test_a_current_journal_reopens_with_its_records_unchanged(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    snapshot = {
        "capture": {"program_run_id": "run.1"},
        "run_revision": 1,
        "state": "stopped",
    }
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="run.1",
        run_snapshot=snapshot,
        terminal_reference="sha256:" + "b" * 64,
    )
    journal.close()

    reopened = ProgramCycleJournal(path, owner_epoch="runner.new")
    try:
        assert reopened.read().state == "terminal_awaiting_reset"
        assert reopened.read_run_snapshot("run.1") == snapshot
    finally:
        reopened.close()


def _complete_persistent(
    journal: ProgramCycleJournal,
    request_id: str,
    kind: str = "persistent_value_set",
    outcome: str = "applied",
) -> str:
    record = journal.record_request(
        owner_epoch="composer.epoch.1",
        request_id=request_id,
        kind=kind,
        intent={"request_id": request_id},
        cycle_generation=1,
    )
    if record.disposition != "new":
        return record.disposition
    journal.complete_persistent_value_request(
        owner_epoch="composer.epoch.1", request_id=request_id, outcome=outcome
    )
    return "new"


def test_completed_persistent_value_requests_are_bounded_and_survive_reopen(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(
        path,
        owner_epoch="runner.epoch.1",
        maximum_completed_persistent_value_requests=3,
    )
    assert _complete_persistent(journal, "edit.1") == "new"
    assert _complete_persistent(journal, "edit.2", "persistent_value_reset") == "new"
    assert (
        _complete_persistent(
            journal, "edit.3", "persistent_values_clear", "storage_uncertain"
        )
        == "new"
    )
    # A resend of a completed request is reported with its action and outcome.
    resent = journal.record_request(
        owner_epoch="composer.epoch.1",
        request_id="edit.3",
        kind="persistent_values_clear",
        intent={"request_id": "edit.3"},
        cycle_generation=1,
    )
    assert (resent.disposition, resent.result) == (
        "completed",
        {"action": "clear", "outcome": "storage_uncertain"},
    )
    # The identity is the journal's request identity: another caller epoch
    # with the same request id is a different request.
    assert journal.record_request(
        owner_epoch="composer.epoch.2",
        request_id="edit.1",
        kind="persistent_value_set",
        intent={"request_id": "edit.1"},
        cycle_generation=1,
    ).disposition == "new"
    journal.cancel_request(owner_epoch="composer.epoch.2", request_id="edit.1")
    # Only persistent-value requests complete into the table.
    journal.record_request(
        owner_epoch="composer.epoch.1",
        request_id="start.1",
        kind="start",
        intent={"request_id": "start.1"},
        cycle_generation=1,
    )
    with pytest.raises(RuntimeError, match="in-flight command is absent"):
        journal.complete_persistent_value_request(
            owner_epoch="composer.epoch.1", request_id="start.1", outcome="applied"
        )
    journal.cancel_request(owner_epoch="composer.epoch.1", request_id="start.1")

    # A fourth completion trims the oldest identity, by insertion order.
    assert _complete_persistent(journal, "edit.4") == "new"
    assert _complete_persistent(journal, "edit.2") == "completed"
    journal.close()

    reopened = ProgramCycleJournal(path, owner_epoch="runner.epoch.2")
    try:
        for request_id in ("edit.2", "edit.3", "edit.4"):
            assert _complete_persistent(reopened, request_id) == "completed"
        # The trimmed identity is no longer known; it is a new request.
        assert _complete_persistent(reopened, "edit.1") == "new"
    finally:
        reopened.close()

    database = sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)
    try:
        assert database.execute(
            "SELECT request_id,action,outcome FROM program_persistent_value_requests "
            "ORDER BY position"
        ).fetchall() == [
            ("edit.2", "reset", "applied"),
            ("edit.3", "clear", "storage_uncertain"),
            ("edit.4", "set", "applied"),
            ("edit.1", "set", "applied"),
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
        ProgramCycleJournal(path, owner_epoch="runner.current")

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
        ProgramCycleJournal(path, owner_epoch="runner.current")

    assert not path.exists()
    assert _file_identity(sidecar) == before


def test_existing_hard_linked_database_is_rejected_without_mutation(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    journal.close()
    os.link(path, tmp_path / "cycle-alias.sqlite3")
    before = _file_identity(path)

    with pytest.raises(RuntimeError, match="owner-held regular file"):
        ProgramCycleJournal(path, owner_epoch="runner.new")

    assert _file_identity(path) == before


def test_replaced_parent_capability_is_rejected_before_next_operation(
    tmp_path: Path,
) -> None:
    state_root = tmp_path / "state"
    state_root.mkdir(mode=0o700)
    path = state_root / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    retained = tmp_path / "retained-state"
    state_root.rename(retained)
    state_root.mkdir(mode=0o700)

    with pytest.raises(RuntimeError, match="replaced or changed"):
        journal.read()

    assert list(state_root.iterdir()) == []
    journal.close()


def test_runtime_rejects_an_unexpected_rollback_sidecar(tmp_path: Path) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    rollback = Path(f"{path}-journal")
    rollback.write_bytes(b"unexpected rollback state")
    rollback.chmod(0o600)

    with pytest.raises(RuntimeError, match="unsupported rollback sidecar"):
        journal.read()

    journal.close()


def test_read_only_preflight_sees_crash_left_wal_without_mutating_it(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    journal.close()

    reader = sqlite3.connect(path, isolation_level=None)
    try:
        reader.execute("BEGIN")
        assert reader.execute(
            "SELECT schema_version FROM program_cycle_metadata"
        ).fetchone() == (SCHEMA_VERSION,)
        writer = sqlite3.connect(path, isolation_level=None)
        try:
            writer.execute("PRAGMA wal_autocheckpoint=0")
            writer.execute("PRAGMA ignore_check_constraints=ON")
            writer.execute("BEGIN IMMEDIATE")
            writer.execute(
                "UPDATE program_cycle_metadata SET schema_version=1 "
                "WHERE singleton=1"
            )
            writer.execute("COMMIT")
        finally:
            writer.close()

        assert Path(f"{path}-wal").is_file()
        before = _database_file_identities(path)

        with pytest.raises(
            ProgramCycleJournalVersionError, match="schema version 1 is unsupported"
        ):
            ProgramCycleJournal(path, owner_epoch="runner.new")

        assert _database_file_identities(path) == before
    finally:
        reader.close()


def test_terminal_blocks_new_cycle_and_resolved_commands_do_not_accumulate(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.epoch.1")
    intent = {"request_id": "start.1", "program": "identity-only"}
    assert (
        journal.record_request(
            owner_epoch="caller.epoch.1",
            request_id="start.1",
            kind="start",
            intent=intent,
            cycle_generation=1,
        )
        .disposition
        == "new"
    )
    claim = {
        "intent": {
            "owner_epoch": "runner.epoch.1",
            "owner_transaction_id": "program-cycle.1",
            "request_id": "start.1",
            "request_payload_digest": "sha256:" + "a" * 64,
        },
        "claim_generation": 7,
        "acquired_admission_generation": 6,
    }
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="starting",
        program_run_id="run.1",
        claim=claim,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("starting",),
        state="terminal_awaiting_reset",
        capture_identities={"program_run_id": "run.1"},
        run_snapshot={
            "run_revision": 3,
            "state": "succeeded",
            "capture": {"program_run_id": "run.1"},
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
        owner_epoch="caller.epoch.1",
        request_id="reset.1",
        kind="reset",
        intent=reset_intent,
        cycle_generation=1,
    )
    release = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.epoch.1",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": "sha256:" + "a" * 64,
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
        owner_epoch="runner.epoch.1",
        request_owner_epoch="caller.epoch.1",
        request_id="reset.1",
        result=result,
    )
    journal.close()

    restarted = ProgramCycleJournal(path, owner_epoch="runner.epoch.2")
    assert restarted.read().state == "idle_after_reset"
    assert restarted.read().cycle_generation == 2
    assert restarted.read().release_intent is None
    assert restarted.read_run_snapshot("run.1") == {
        "run_revision": 3,
        "state": "succeeded",
        "capture": {"program_run_id": "run.1"},
    }
    reused_identity_is_a_new_stale_intent = restarted.record_request(
        owner_epoch="caller.epoch.1",
        request_id="reset.1",
        kind="reset",
        intent=reset_intent,
        cycle_generation=1,
    )
    assert reused_identity_is_a_new_stale_intent.disposition == "new"
    restarted.cancel_request(
        owner_epoch="caller.epoch.1", request_id="reset.1"
    )
    restarted.close()

    database = sqlite3.connect(path)
    assert database.execute(
        "SELECT COUNT(*) FROM program_cycle_inflight_commands"
    ).fetchone() == (0,)
    database.close()


def test_known_rejection_is_discarded_but_unresolved_request_never_reexecutes(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_inflight_commands=1,
    )
    first = journal.record_request(
        owner_epoch="caller.1",
        request_id="start.rejected",
        kind="start",
        intent={"revision": 1},
        cycle_generation=1,
    )
    assert first.disposition == "new"
    journal.reject_request(
        owner_epoch="caller.1",
        request_id="start.rejected",
        code="program_run_draft_applied_mismatch",
        message="Draft and Applied Program revisions differ",
    )
    rejected_identity_is_reusable = journal.record_request(
        owner_epoch="caller.1",
        request_id="start.rejected",
        kind="start",
        intent={"revision": 1},
        cycle_generation=2,
    )
    assert rejected_identity_is_reusable.disposition == "new"
    journal.cancel_request(owner_epoch="caller.1", request_id="start.rejected")

    assert (
        journal.record_request(
            owner_epoch="caller.1",
            request_id="reset.unknown",
            kind="reset",
            intent={"revision": 2},
            cycle_generation=2,
        ).disposition
        == "new"
    )
    assert (
        journal.record_request(
            owner_epoch="caller.1",
            request_id="reset.unknown",
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
            owner_epoch="caller.1",
            request_id="ordinary.1",
            kind="start",
            intent={"revision": 3},
            cycle_generation=3,
        ).disposition
        == "new"
    )
    with pytest.raises(OverflowError, match="full"):
        journal.record_request(
            owner_epoch="caller.1",
            request_id="start.over-capacity",
            kind="start",
            intent={"revision": 3},
            cycle_generation=3,
        )
    journal.close()


def test_every_runtime_request_kind_survives_owner_restart(tmp_path) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.1")
    kinds = (
        "start",
        "reset",
        "runtime_blackboard_write",
        "manual_result_stage",
        "recovery_action",
        "resume_full_stop",
        "discard_full_stop",
    )
    for index, kind in enumerate(kinds):
        assert journal.record_request(
            owner_epoch="operator.1",
            request_id=f"request.{index}",
            kind=kind,
            intent={"kind": kind},
            cycle_generation=1,
        ).disposition == "new"
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="starting",
        program_run_id="program-cycle.1",
        claim={"claim_generation": 1},
    )
    with pytest.raises(ValueError, match="kind is unsupported"):
        journal.record_request(
            owner_epoch="operator.1",
            request_id="request.invalid",
            kind="unknown_control",
            intent={"kind": "unknown_control"},
            cycle_generation=1,
        )
    journal.close()

    restarted = ProgramCycleJournal(path, owner_epoch="runner.2")
    for index, kind in enumerate(kinds):
        assert restarted.record_request(
            owner_epoch="operator.1",
            request_id=f"request.{index}",
            kind=kind,
            intent={"kind": kind},
            cycle_generation=1,
        ).disposition == "in_progress"
    restarted.close()


def test_acquire_response_loss_is_durably_owner_unknown(tmp_path) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    acquire_intent = {
        "claim_kind": "program_cycle",
        "owner_role": "program_runner",
        "owner_epoch": "runner.old",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.unknown",
        "request_payload_digest": "sha256:" + "a" * 64,
        "expected_admission_generation": 4,
        "scope": {"resources": [], "configurations": []},
        "program_cycle_evidence": {
            "draft_program_revision": "sha256:" + "b" * 64,
            "applied_program_revision": "sha256:" + "b" * 64,
        },
        "intervention_evidence": None,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        evidence=acquire_intent,
    )
    journal.close()

    restarted = ProgramCycleJournal(path, owner_epoch="runner.new")
    record = restarted.read()
    assert record.state == "starting"
    assert record.owner_epoch == "runner.old"
    assert record.claim is None
    assert record.acquisition_pending == acquire_intent
    assert record.terminal_reference is None
    assert record.reconciliation_required is True
    restarted.close()


def test_terminal_record_cannot_be_rewritten(tmp_path) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.epoch.1"
    )
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="run.1",
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


def test_liveness_reconciliation_intent_is_durable_and_replaceable(tmp_path) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.local")
    first_digest = "sha256:" + "a" * 64
    second_digest = "sha256:" + "b" * 64

    journal.prepare_liveness_reconciliation(
        predecessor_owner_epoch="runner.kernel-old",
        evidence_digest=first_digest,
    )
    prepared = journal.read()
    assert prepared.owner_epoch == "runner.kernel-old"
    assert prepared.reconciliation_required is True
    assert prepared.reconciliation_evidence_digest == first_digest

    journal.prepare_liveness_reconciliation(
        predecessor_owner_epoch="runner.kernel-current",
        evidence_digest=second_digest,
    )
    replaced = journal.read()
    assert replaced.owner_epoch == "runner.kernel-current"
    assert replaced.reconciliation_required is True
    assert replaced.reconciliation_evidence_digest == second_digest

    journal.reconcile_liveness(
        owner_epoch="runner.successor",
        evidence_digest=second_digest,
    )
    finalized = journal.read()
    assert finalized.owner_epoch == "runner.successor"
    assert finalized.reconciliation_required is False
    assert finalized.reconciliation_evidence_digest == second_digest
    journal.close()


@pytest.mark.parametrize("checkpoint", ["claim_journaled", "starting", "running"])
def test_each_exact_post_acquire_crash_checkpoint_recovers_terminal(
    tmp_path, checkpoint: str
) -> None:
    path = tmp_path / f"{checkpoint}.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.stable")
    claim = {
        "intent": {"owner_epoch": "runner.stable"},
        "claim_generation": 3,
        "acquired_admission_generation": 2,
        "accepted_at_ns": 1,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        evidence={"request_id": "start.1"},
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        claim=claim,
    )
    if checkpoint == "running":
        journal.transition(
            expected_generation=1,
            expected_states=("starting",),
            state=checkpoint,
        )
    journal.close()

    recovered = ProgramCycleJournal(path, owner_epoch="runner.stable")
    record = recovered.read()
    assert record.state == ("running" if checkpoint == "running" else "starting")
    assert record.claim == claim
    assert record.owner_epoch == "runner.stable"
    assert record.terminal_reference is None
    assert record.reconciliation_required is True
    recovered.close()


@pytest.mark.parametrize("prior_uncertain", [False, True])
@pytest.mark.parametrize("new_uncertain", [False, True])
def test_reset_failure_never_clears_prior_uncertainty(tmp_path, prior_uncertain, new_uncertain):
    journal = ProgramCycleJournal(tmp_path / "reset.sqlite3", owner_epoch="runner.1")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="run.1",
        claim={"claim_generation": 1},
        reconciliation_required=prior_uncertain,
    )
    journal.record_request(
        owner_epoch="operator.1",
        request_id="uncertain.1",
        kind="resume_motion",
        intent={"kind": "resume_motion"},
        cycle_generation=1,
    )
    journal.begin_resetting(expected_generation=1, program_run_id="run.1", correlation_id="reset.1")
    journal.fail_reset(
        expected_generation=1,
        program_run_id="run.1",
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
            owner_epoch="operator.1",
            request_id="uncertain.1",
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
        tmp_path / "capacity.sqlite3", owner_epoch="runner.1", maximum_inflight_commands=capacity
    )
    # One bounded setup transaction avoids thousands of filesystem syncs.
    with journal._transaction():
        journal._database.executemany(
            "INSERT INTO program_cycle_inflight_commands VALUES(?,?,?,?,?)",
            (("operator.1", f"move.{i}", "resume_motion", "{}", 1) for i in range(capacity)),
        )
    with pytest.raises(OverflowError):
        journal.record_request(
            owner_epoch="operator.1",
            request_id="extra",
            kind="resume_motion",
            intent={},
            cycle_generation=1,
        )
    assert (
        journal.record_request(
            owner_epoch="operator.1",
            request_id="reset.1",
            kind="reset",
            intent={},
            cycle_generation=1,
        ).disposition
        == "new"
    )
    with pytest.raises(OverflowError):
        journal.record_request(
            owner_epoch="operator.1",
            request_id="reset.2",
            kind="reset",
            intent={},
            cycle_generation=1,
        )
    assert len(journal.pending_commands()) == capacity + 1
    assert (
        journal.record_request(
            owner_epoch="operator.1",
            request_id="reset.1",
            kind="reset",
            intent={},
            cycle_generation=1,
        ).disposition
        == "in_progress"
    )
    journal.close()


@pytest.mark.parametrize("crossing", [None, "revision", "liveness", "reconciliation_digest"])
def test_reset_discharge_is_exact_and_keeps_original_commands(tmp_path, crossing):
    journal = ProgramCycleJournal(tmp_path / "discharge.sqlite3", owner_epoch="runner.1")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="recovery_required",
        program_run_id="run.1",
        claim={"claim_generation": 1},
        reconciliation_required=True,
    )
    journal.record_request(
        owner_epoch="operator.1",
        request_id="resume.1",
        kind="resume_motion",
        intent={},
        cycle_generation=1,
    )
    pending = journal.pending_commands()
    journal.begin_resetting(
        expected_generation=1,
        program_run_id="run.1",
        correlation_id="reset.1",
        expected_recovery=journal.read(),
    )
    journal.update_reset_phase(
        expected_generation=1,
        program_run_id="run.1",
        expected_phase="stopping",
        phase="settling",
        terminal_reference="sha256:" + "a" * 64,
    )
    expected = journal.read()
    if crossing == "revision":
        journal.update_reset_phase(
            expected_generation=1,
            program_run_id="run.1",
            expected_phase="settling",
            phase="settling",
        )
    elif crossing == "liveness":
        journal.retain_liveness_loss(
            expected_generation=1,
            manager_runtime_epoch="manager.1",
            manager_owner_epoch="manager.1",
            fault_reference="manager-lost.1",
        )
    elif crossing == "reconciliation_digest":
        # Liveness/reconciliation changes need not advance the control
        # revision. Compare the complete retained cycle, not that counter alone.
        with journal._transaction():
            journal._database.execute(
                "UPDATE program_cycle SET reconciliation_evidence_digest=?",
                ("sha256:" + "b" * 64,),
            )
    if crossing:
        before = journal.read()
        with pytest.raises(RuntimeError, match="discharge is stale"):
            journal.settle_reset_recovery(
                expected=expected, manager_runtime_epoch=None, manager_owner_epoch=None
            )
        assert journal.read() == before
    else:
        journal.settle_reset_recovery(
            expected=expected, manager_runtime_epoch=None, manager_owner_epoch=None
        )
        assert journal.read().reset_phase == "restoring_scene"
        assert not journal.read().reconciliation_required
    assert journal.pending_commands() == pending
    journal.close()


def test_multiple_retained_resets_remain_readable_but_cannot_grow(tmp_path):
    path = tmp_path / "legacy-resets.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.1")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="recovery_required",
        program_run_id="run.1",
        claim={"claim_generation": 1},
        reconciliation_required=True,
    )
    with journal._transaction():
        journal._database.executemany(
            "INSERT INTO program_cycle_inflight_commands VALUES(?,?,?,?,?)",
            [("operator.1", f"reset.{index}", "reset", "{}", 1) for index in range(2)],
        )
    journal.close()
    recovered = ProgramCycleJournal(path, owner_epoch="runner.1")
    before = recovered.pending_commands()
    assert len(before) == 2
    for kind in ("reset", "resume_motion"):
        with pytest.raises(OverflowError):
            recovered.record_request(
                owner_epoch="operator.1",
                request_id="new.1",
                kind=kind,
                intent={},
                cycle_generation=1,
            )
        assert recovered.pending_commands() == before
    recovered.close()


def test_restart_turns_interrupted_reset_into_exact_recovery_phase(tmp_path) -> None:
    path = tmp_path / "reset-interrupted.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.old")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="program-cycle.1",
        claim={"claim_generation": 1},
        terminal_reference="sha256:" + "a" * 64,
    )
    journal.record_request(
        owner_epoch="operator.1",
        request_id="reset.1",
        kind="reset",
        intent={"kind": "reset"},
        cycle_generation=1,
    )
    journal.begin_resetting(
        expected_generation=1,
        program_run_id="program-cycle.1",
        correlation_id="reset.1",
    )
    journal.update_reset_phase(
        expected_generation=1,
        program_run_id="program-cycle.1",
        expected_phase="stopping",
        phase="restoring_scene",
    )
    journal.close()

    recovered = ProgramCycleJournal(path, owner_epoch="runner.new")
    record = recovered.read()
    assert record.state == "recovery_required"
    assert record.owner_epoch == "runner.old"
    assert record.reset_phase == "restoring_scene"
    assert record.reconciliation_required is True
    assert record.failure == {
        "code": "program_cycle_reset_interrupted",
        "failed_phase": "restoring_scene",
        "message": "Program Runner restarted during Reset",
    }
    assert recovered.record_request(
        owner_epoch="operator.1",
        request_id="reset.1",
        kind="reset",
        intent={"kind": "reset"},
        cycle_generation=1,
    ).disposition == "in_progress"
    recovered.close()


def test_terminal_observation_is_exact_idempotent_and_race_safe(tmp_path) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.epoch.1"
    )
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="running",
        program_run_id="run.1",
        claim={"claim_generation": 1},
    )
    reference = "sha256:" + "c" * 64
    snapshot = {
        "run_revision": 2,
        "state": "succeeded",
        "capture": {"program_run_id": "run.1"},
    }
    barrier = Barrier(3)
    errors: list[BaseException] = []

    def observe() -> None:
        barrier.wait()
        try:
            journal.observe_terminal(
                expected_generation=1,
                program_run_id="run.1",
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
            program_run_id="run.1",
            terminal_reference="sha256:" + "d" * 64,
            snapshot=snapshot,
        )
    assert journal.read() == terminal
    journal.close()


def test_native_settlement_readers_accept_the_current_journal_schema() -> None:
    """The native host reads this journal directly: its only accepted schema
    must be this runner's, or every restart refuses a current journal."""

    source = (
        Path(__file__).resolve().parents[2]
        / "manyforge_roboplan/src/owner_settlement_readers.cpp"
    ).read_text(encoding="utf-8")
    assert (
        f"constexpr int kProgramCycleJournalSchema = {SCHEMA_VERSION};"
        in source
    )


@pytest.mark.parametrize("mismatch", ["none", "owner", "generation", "run", "claim", "failure", "terminal"])
def test_running_cas_classifies_only_exact_accepted_stop(tmp_path, mismatch):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    claim = {"claim_generation": 9, "intent": {"owner_epoch": "runner.1", "owner_transaction_id": "run.1"}}
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",),
                       state="starting", program_run_id="run.1", claim=claim)
    journal.mark_stopping(expected_generation=1, program_run_id="run.1")
    if mismatch == "failure":
        journal.transition(expected_generation=1, expected_states=("stopping",), state="stopping",
                           failure={"code": "actual_failure"})
    if mismatch == "terminal":
        snapshot = {"run_revision": 2, "state": "stopped", "capture": {"program_run_id": "run.1"}}
        reference = "sha256:" + hashlib.sha256(rfc8785.dumps({
            "program_run_id": "run.1", "run_revision": 2, "state": "stopped"})).hexdigest()
        journal.observe_terminal(expected_generation=1, program_run_id="run.1",
                                 terminal_reference=reference, snapshot=snapshot)
    before = journal.read()
    expected = ProgramCycleStopWon if mismatch in {"none", "terminal"} else RuntimeError
    with pytest.raises(expected) as caught:
        journal.transition(expected_generation=2 if mismatch == "generation" else 1,
                           expected_states=("starting",), state="running",
                           program_run_id="foreign.run" if mismatch == "run" else "run.1",
                           expected_running_owner_epoch="foreign.owner" if mismatch == "owner" else "runner.1",
                           expected_running_claim={**claim, "claim_generation": 10} if mismatch == "claim" else claim)
    assert isinstance(caught.value, ProgramCycleStopWon) == (mismatch in {"none", "terminal"})
    assert journal.read() == before
    journal.close()


def test_reset_option_defaults_preserve_exact_request_identity(tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    original = {"request_id": "reset.1", "unrelated": "must stay exact"}
    try:
        assert journal.record_request(owner_epoch="caller.1", request_id="reset.1", kind="reset",
                                      intent=original, cycle_generation=1).disposition == "new"
        defaults = {**original, "reload_initial_scene": False, "reset_robot_position": False}
        assert journal.record_request(owner_epoch="caller.1", request_id="reset.1", kind="reset",
                                      intent=defaults, cycle_generation=1).disposition == "in_progress"
        for changed in ({**defaults, "reload_initial_scene": True},
                        {**defaults, "reset_robot_position": True},
                        {**defaults, "unrelated": "changed"}):
            with pytest.raises(ValueError, match="different intent"):
                journal.record_request(owner_epoch="caller.1", request_id="reset.1", kind="reset",
                                       intent=changed, cycle_generation=1)
        assert journal.pending_commands()[0]["intent"] == original
    finally:
        journal.close()


def test_idle_reset_claim_register_and_prior_scene_fault_survive_interruption(tmp_path):
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.1")
    original = {"code": "program_scene_preparation_failed", "message": "unrepaired Scene"}
    journal.record_scene_preparation_failure(expected_generation=1, owner_epoch="runner.1", failure=original)
    journal.record_request(owner_epoch="caller.1", request_id="reset.1", kind="reset",
                           intent={"reset_robot_position": True}, cycle_generation=1)
    journal.begin_idle_resetting(expected_generation=1, owner_epoch="runner.1", correlation_id="reset.1",
                                 session_id=None, client_sequence=None, phase="restoring_position")
    retained = {"intent": {"expected_admission_generation": 41}, "claim": None, "release_intent": None}
    journal.remember_reset_effect_claim(expected_generation=1, expected=None, retained=retained)
    journal.close()
    reopened = ProgramCycleJournal(path, owner_epoch="runner.1")
    try:
        cycle = reopened.read()
        assert cycle.state == "recovery_required" and cycle.reconciliation_required
        assert cycle.reset_effect_claim == retained
        assert cycle.failure["prior_scene_preparation_failure"] == original
        assert cycle.failure["failed_phase"] == "restoring_position"
    finally:
        reopened.close()
