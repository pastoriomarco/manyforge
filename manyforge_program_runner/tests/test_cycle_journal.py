"""Durable Program-cycle state and replay invariants."""

from __future__ import annotations

import sqlite3
from threading import Barrier, Thread

import pytest

from manyforge_program_runner.cycle_journal import ProgramCycleJournal


def test_terminal_blocks_new_cycle_and_reset_replays_after_restart(tmp_path) -> None:
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
    replay = restarted.record_request(
        owner_epoch="caller.epoch.1",
        request_id="reset.1",
        kind="reset",
        intent=reset_intent,
        cycle_generation=1,
    )
    assert replay.disposition == "succeeded"
    assert replay.result == result
    restarted.close()

    database = sqlite3.connect(path)
    encoded = database.execute(
        "SELECT intent_json FROM program_cycle_requests WHERE request_id='start.1'"
    ).fetchone()[0]
    assert "tree" not in encoded
    assert "identity-only" in encoded


def test_known_rejection_replays_and_unresolved_request_never_reexecutes(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_requests=2,
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
    rejected = journal.record_request(
        owner_epoch="caller.1",
        request_id="start.rejected",
        kind="start",
        intent={"revision": 1},
        cycle_generation=2,
    )
    assert rejected.disposition == "rejected"
    assert rejected.error == {
        "code": "program_run_draft_applied_mismatch",
        "message": "Draft and Applied Program revisions differ",
    }

    assert journal.record_request(
        owner_epoch="caller.1",
        request_id="reset.unknown",
        kind="reset",
        intent={"revision": 2},
        cycle_generation=2,
    ).disposition == "new"
    assert journal.record_request(
        owner_epoch="caller.1",
        request_id="reset.unknown",
        kind="reset",
        intent={"revision": 2},
        cycle_generation=3,
    ).disposition == "in_progress"
    with pytest.raises(OverflowError, match="full"):
        journal.record_request(
            owner_epoch="caller.1",
            request_id="start.over-capacity",
            kind="start",
            intent={"revision": 3},
            cycle_generation=3,
        )
    journal.close()


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
    assert record.state == "idle_after_reset"
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
    if checkpoint != "claim_journaled":
        journal.transition(
            expected_generation=1,
            expected_states=("idle_after_reset",),
            state=checkpoint,
        )
    journal.close()

    recovered = ProgramCycleJournal(path, owner_epoch="runner.stable")
    record = recovered.read()
    assert record.state == (
        "idle_after_reset" if checkpoint == "claim_journaled" else checkpoint
    )
    assert record.claim == claim
    assert record.owner_epoch == "runner.stable"
    assert record.terminal_reference is None
    assert record.reconciliation_required is True
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
