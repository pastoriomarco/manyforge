"""Production Program-runner admission, replay, and lifecycle glue."""

from __future__ import annotations

from copy import deepcopy
from threading import Event, Thread
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_headless_api.models import (
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.cell_admission import CellAdmissionError
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.models import ProgramCycleResetRequest


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_ACTIVE_STATES = {"running", "intervention_required", "stopping", "finalizing"}


class _Headless:
    """Construction-complete transport whose unused effects fail loudly."""

    _PORT_METHODS = {
        "acknowledge_completed_operation",
        "commit_scene_transaction",
        "cclc_component",
        "cclc_route_templates",
        "component_lifecycle_status",
        "commit_program_model_attempt",
        "execute",
        "execution_status",
        "manager_graph",
        "operation",
        "operation_runtime_scale",
        "pause_operation",
        "reset_operation",
        "resume_operation",
        "scene",
        "set_operation_runtime_scale",
        "stop_operations",
        "submit_component_lifecycle",
        "submit_route_selection",
        "route_selection_status",
    }

    def __init__(self) -> None:
        self.applied_reads = 0
        self.selection_reads = 0

    def program_applied(self) -> dict[str, Any]:
        self.applied_reads += 1
        return {
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": _DIGEST_A,
            "program_revision": _DIGEST_A,
            "source": {"record_digest": _DIGEST_B},
        }

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        self.selection_reads += 1
        assert scenario_id == "cell-pick"
        return {"scenario_id": scenario_id, "program_revision": _DIGEST_A}

    def __getattr__(self, name: str):
        if name not in self._PORT_METHODS:
            raise AttributeError(name)

        def unexpected(*_args: object, **_kwargs: object) -> None:
            raise AssertionError(f"unexpected effectful headless call: {name}")

        return unexpected


class _Snapshot:
    def __init__(
        self,
        program_run_id: str,
        *,
        owner_id: str,
        owner_epoch: str,
        state: str,
        revision: int,
    ) -> None:
        self.capture = SimpleNamespace(program_run_id=program_run_id)
        self._value = {
            "schema_version": "manyforge.program_run_status.v1",
            "owner_id": owner_id,
            "owner_epoch": owner_epoch,
            "run_revision": revision,
            "state": state,
            "capture": {
                "schema_version": "manyforge.program_run_capture.v1",
                "program_run_id": program_run_id,
            },
            "tree_status": "running",
            "active_operation_count": 1 if state in _ACTIVE_STATES else 0,
            "reason_code": None,
            "message": "",
        }

    @property
    def state(self) -> str:
        return self._value["state"]

    @property
    def owner_epoch(self) -> str:
        return self._value["owner_epoch"]

    def to_dict(self) -> dict[str, Any]:
        return deepcopy(self._value)


class _Owner:
    def __init__(self) -> None:
        self.rows: dict[str, _Snapshot] = {}
        self.reads = 0

    def list_snapshots(
        self,
        *,
        active_only: bool,
        limit: int,
    ) -> tuple[tuple[_Snapshot, ...], bool]:
        rows = tuple(
            row
            for _identity, row in sorted(self.rows.items())
            if not active_only or row.state in _ACTIVE_STATES
        )
        return rows[:limit], len(rows) > limit

    def read(self, program_run_id: str) -> _Snapshot:
        self.reads += 1
        return self.rows[program_run_id]


class _Host:
    def __init__(self, owner: _Owner, *, owner_id: str, owner_epoch: str) -> None:
        self.owner = owner
        self.owner_id = owner_id
        self.owner_epoch = owner_epoch
        self.started = 0
        self.start_admissions: list[object] = []
        self.stop_calls: list[tuple[str, str | None]] = []
        self.shutdowns = 0

    def start(self) -> None:
        self.started += 1

    def start_run(self, admission: object) -> _Snapshot:
        self.start_admissions.append(admission)
        program_run_id = f"program-run.{len(self.start_admissions)}"
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None,
    ) -> _Snapshot:
        self.stop_calls.append((program_run_id, reason))
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="stopped",
            revision=2,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def shutdown(self) -> None:
        self.shutdowns += 1


class _SimulationControl:
    def __init__(self) -> None:
        self.closed = 0

    def bind(self, _context: object) -> None:
        pass

    def unbind(self, _program_run_id: str) -> None:
        pass

    def start(self, *_args: object, **_kwargs: object) -> None:
        raise AssertionError("unexpected simulation task")

    def observe(self, _task_id: str) -> None:
        raise AssertionError("unexpected simulation task")

    def cancel(self, *_args: object) -> None:
        raise AssertionError("unexpected simulation task")

    def acknowledge(self, *_args: object) -> None:
        raise AssertionError("unexpected simulation task")

    def close(self) -> None:
        self.closed += 1


class _ModelTransport:
    def __init__(self) -> None:
        self.closed = 0

    def validate_endpoint(self, **_kwargs: object) -> None:
        pass

    def complete(self, *_args: object, **_kwargs: object) -> None:
        raise AssertionError("unexpected model request")

    def close(self) -> None:
        self.closed += 1


class _SnapshotSource:
    def __init__(self) -> None:
        self.closed = 0

    def capture_snapshot(self, **_kwargs: object) -> None:
        raise AssertionError("unexpected snapshot request")

    def close(self) -> None:
        self.closed += 1


class _Admission:
    def __init__(self) -> None:
        self.claims: list[dict[str, Any]] = []
        self.releases: list[dict[str, Any]] = []
        self.reconciliations: list[dict[str, Any]] = []
        self.release_error: CellAdmissionError | None = None

    def get(self) -> dict[str, Any]:
        return {"admission_generation": 1}

    def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
        self.claims.append(deepcopy(intent))
        return {
            "intent": deepcopy(intent),
            "claim_generation": 1,
            "acquired_admission_generation": 1,
            "accepted_at_ns": 1,
        }

    def release(self, release: dict[str, Any]) -> dict[str, Any]:
        self.releases.append(deepcopy(release))
        if self.release_error is not None:
            raise self.release_error
        return {"admission_generation": 2}

    def reconcile(self, reconciliation: dict[str, Any]) -> dict[str, Any]:
        self.reconciliations.append(deepcopy(reconciliation))
        return {"admission_generation": 2}


def _start_request(*, request_id: str = "request.start.1") -> ProgramRunStartRequest:
    return ProgramRunStartRequest.model_validate(
        {
            "request_id": request_id,
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B,
        }
    )


def test_runtime_replays_exact_requests_and_keeps_one_active_product_run(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    headless = _Headless()
    admitted = object()
    joins: list[tuple[dict[str, Any], dict[str, Any]]] = []

    def join(applied: dict[str, Any], live: dict[str, Any], **_kwargs: object):
        joins.append((deepcopy(applied), deepcopy(live)))
        return admitted

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        join,
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="program-runner.epoch.1",
        ),
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]

    runtime.start()
    request = _start_request()
    started = runtime.start_run(request)
    replayed = runtime.start_run(request)

    assert started == replayed
    assert started["capture"]["program_run_id"] == "program-run.1"
    assert host.started == 1
    assert host.start_admissions == [admitted]
    assert headless.applied_reads == 2
    assert headless.selection_reads == 1
    assert len(joins) == 1

    with pytest.raises(ProgramRunnerError) as active:
        runtime.start_run(_start_request(request_id="request.start.2"))
    assert active.value.code == "program_cycle_reset_required"
    assert headless.applied_reads == 2
    assert runtime.cycle_status()["state"] == "running"

    with pytest.raises(ProgramRunnerError) as conflict:
        runtime.start_run(
            ProgramRunStartRequest.model_validate(
                {
                    **request.model_dump(mode="json"),
                    "scenario_catalog_revision": "sha256:" + "c" * 64,
                }
            )
        )
    assert conflict.value.code == "program_run_request_conflict"

    stop = ProgramRunStopRequest.model_validate(
        {
            "request_id": "request.stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "reason": "operator stop",
        }
    )
    owner_stop = host.stop_run

    def stop_with_terminal_observer(
        program_run_id: str, *, reason: str | None
    ) -> _Snapshot:
        snapshot = owner_stop(program_run_id, reason=reason)
        runtime._record_terminal_snapshot(snapshot)  # noqa: SLF001
        return snapshot

    host.stop_run = stop_with_terminal_observer  # type: ignore[method-assign]
    stopped = runtime.stop_run("program-run.1", stop)
    stop_replay = runtime.stop_run("program-run.1", stop)
    assert stopped == stop_replay
    assert host.stop_calls == [("program-run.1", "operator stop")]
    assert runtime.cycle_status()["cycle_claim_generation"] == 1
    owner.rows.clear()
    with pytest.raises(ProgramRunnerError) as terminal:
        runtime.start_run(_start_request(request_id="request.start.terminal"))
    assert terminal.value.code == "program_cycle_reset_required"

    status = runtime.cycle_status()
    reset = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "request.reset.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": 1,
            "expected_claim_generation": 1,
            "terminal_reference": status[
                "terminal_or_settlement_reference"
            ],
            "settlement_evidence_digest": _DIGEST_A,
        }
    )
    reset_result = runtime.reset_cycle(reset)
    assert runtime.reset_cycle(reset) == reset_result
    assert len(admission.releases) == 1
    assert runtime.cycle_status()["state"] == "idle_after_reset"

    runtime.shutdown()
    assert host.shutdowns == 1
    assert host.stop_calls == [("program-run.1", "operator stop")]


def test_replacement_owner_remains_fail_closed_for_later_reconciliation_owner(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    old = ProgramCycleJournal(path, owner_epoch="runner.old")
    intent = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.old",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": _DIGEST_A,
    }
    claim = {
        "intent": intent,
        "claim_generation": 7,
        "acquired_admission_generation": 6,
        "accepted_at_ns": 1,
    }
    old.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        evidence=intent,
    )
    old.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        claim=claim,
    )
    old.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        terminal_reference=_DIGEST_B,
    )
    old.close()

    replacement = ProgramCycleJournal(path, owner_epoch="runner.new")
    assert replacement.read().owner_epoch == "runner.old"
    assert replacement.read().reconciliation_required is True
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.new",
            execution_state_root=tmp_path,
            cycle_journal_path=path,
            kernel_owner_epoch="kernel.epoch.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=replacement,
    )
    runtime.start()
    assert runtime.health()["started"] is True
    with pytest.raises(ProgramRunnerError) as status:
        runtime.cycle_status()
    assert status.value.code == "program_cycle_reconciliation_required"
    assert admission.reconciliations == []
    assert admission.releases == []
    assert replacement.read().owner_epoch == "runner.old"
    assert replacement.read().state == "terminal_awaiting_reset"
    assert replacement.read().claim == claim
    assert replacement.read().reconciliation_required is True
    runtime.shutdown()


def test_explicit_release_rejection_remains_retryable_without_unknown_outcome(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.current")
    intent = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.current",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": _DIGEST_A,
    }
    claim = {
        "intent": intent,
        "claim_generation": 7,
        "acquired_admission_generation": 6,
        "accepted_at_ns": 1,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-run.1",
        evidence=intent,
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-run.1",
        claim=claim,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        terminal_reference=_DIGEST_B,
    )
    admission = _Admission()
    admission.release_error = CellAdmissionError(
        "settlement_validator_unavailable",
        "settlement validator is unavailable",
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.current",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    request = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "reset.1",
            "request_owner_epoch": "composer.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "runner.current",
            "expected_cycle_generation": 1,
            "expected_claim_generation": 7,
            "terminal_reference": _DIGEST_B,
            "settlement_evidence_digest": _DIGEST_A,
        }
    )

    runtime.start()
    with pytest.raises(ProgramRunnerError) as rejected:
        runtime.reset_cycle(request)
    assert rejected.value.code == "settlement_validator_unavailable"
    assert journal.read().reconciliation_required is False
    assert journal.read().state == "terminal_awaiting_reset"

    admission.release_error = None
    assert runtime.reset_cycle(request)["idle_cycle_generation"] == 2
    assert journal.read().state == "idle_after_reset"
    runtime.shutdown()


def test_rejected_reset_replays_after_state_change_and_retains_capacity(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.current",
        maximum_requests=2,
    )
    intent = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.current",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": _DIGEST_A,
    }
    claim = {
        "intent": intent,
        "claim_generation": 7,
        "acquired_admission_generation": 6,
        "accepted_at_ns": 1,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-run.1",
        evidence=intent,
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-run.1",
        claim=claim,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        terminal_reference=_DIGEST_B,
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.current",
            maximum_replay_records=2,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()

    stale = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "reset.stale",
            "request_owner_epoch": "composer.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "runner.current",
            "expected_cycle_generation": 1,
            "expected_claim_generation": 7,
            "terminal_reference": _DIGEST_A,
            "settlement_evidence_digest": _DIGEST_A,
        }
    )
    with pytest.raises(ProgramRunnerError) as first:
        runtime.reset_cycle(stale)
    assert first.value.code == "program_cycle_reset_stale"

    valid = ProgramCycleResetRequest.model_validate(
        {
            **stale.model_dump(mode="json"),
            "request_id": "reset.valid",
            "terminal_reference": _DIGEST_B,
        }
    )
    assert runtime.reset_cycle(valid)["idle_cycle_generation"] == 2
    with pytest.raises(ProgramRunnerError) as replay:
        runtime.reset_cycle(stale)
    assert replay.value.code == "program_cycle_reset_stale"
    assert len(admission.releases) == 1

    over_capacity = ProgramCycleResetRequest.model_validate(
        {**stale.model_dump(mode="json"), "request_id": "reset.capacity"}
    )
    with pytest.raises(ProgramRunnerError) as full:
        runtime.reset_cycle(over_capacity)
    assert full.value.code == "program_run_request_ledger_full"
    runtime.shutdown()


def test_stop_retains_claim_until_exact_terminal_snapshot_is_observed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    headless = _Headless()
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="program-runner.epoch.1",
        ),
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]
    runtime.start()
    runtime.start_run(_start_request())

    def delayed_stop(program_run_id: str, *, reason: str | None) -> _Snapshot:
        host.stop_calls.append((program_run_id, reason))
        snapshot = _Snapshot(
            program_run_id,
            owner_id=runtime.owner_id,
            owner_epoch=runtime.owner_epoch,
            state="stopping",
            revision=2,
        )
        owner.rows[program_run_id] = snapshot
        return snapshot

    host.stop_run = delayed_stop  # type: ignore[method-assign]
    stopped = runtime.stop_run(
        "program-run.1",
        ProgramRunStopRequest.model_validate(
            {
                "request_id": "request.stop.delayed",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
                "program_run_id": "program-run.1",
                "expected_owner_epoch": runtime.owner_epoch,
                "reason": "operator stop",
            }
        ),
    )
    assert stopped["state"] == "stopping"
    status = runtime.cycle_status()
    assert status["state"] == "stopping"
    assert status["cycle_claim_generation"] == 1
    assert admission.releases == []

    terminal_snapshot = _Snapshot(
        "program-run.1",
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="stopped",
        revision=3,
    )
    owner.rows["program-run.1"] = terminal_snapshot
    runtime._record_terminal_snapshot(terminal_snapshot)  # noqa: SLF001
    status = runtime.cycle_status()
    assert status["state"] == "terminal_awaiting_reset"
    assert status["terminal_or_settlement_reference"] is not None
    runtime.shutdown()


def test_draft_applied_mismatch_is_replayed_without_native_acquire(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    events: list[str] = []

    class InterleavedAdmission(_Admission):
        generation = 1

        def get(self) -> dict[str, Any]:
            events.append("generation")
            return {"admission_generation": self.generation}

        def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
            events.append("acquire")
            return super().acquire(intent)

    admission = InterleavedAdmission()

    class InterleavedHeadless(_Headless):
        draft_revision = _DIGEST_B

        def program_applied(self) -> dict[str, Any]:
            events.append("applied")
            return super().program_applied()

        def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
            events.append("draft")
            admission.generation = 2
            return {
                "scenario_id": scenario_id,
                "program_revision": self.draft_revision,
            }

    headless = InterleavedHeadless()
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_requests=1,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    request = _start_request()
    with pytest.raises(ProgramRunnerError) as stale:
        runtime.start_run(request)
    assert stale.value.code == "program_run_draft_applied_mismatch"
    admission.generation = 3
    with pytest.raises(ProgramRunnerError) as replay:
        runtime.start_run(request)
    assert replay.value.code == "program_run_draft_applied_mismatch"
    headless.draft_revision = _DIGEST_A
    with pytest.raises(ProgramRunnerError) as full:
        runtime.start_run(_start_request(request_id="request.start.capacity"))
    assert full.value.code == "program_run_request_ledger_full"
    assert events == ["generation", "applied", "draft"]
    assert admission.claims == []
    assert journal.read().state == "idle_after_reset"
    assert journal.read().claim is None
    assert journal.read().acquisition_pending is None
    runtime.shutdown()


def test_acquire_success_before_claim_journal_failure_latches_reconciliation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    class FailingJournal(ProgramCycleJournal):
        def remember_acquired_claim(self, **_kwargs: object) -> None:
            raise OSError("injected durable write failure")

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    admission = _Admission()
    journal = FailingJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    with pytest.raises(ProgramRunnerError) as failed:
        runtime.start_run(_start_request())
    assert failed.value.code == "program_cycle_reconciliation_required"
    assert len(admission.claims) == 1
    assert journal.read().reconciliation_required is True
    assert journal.read().state == "idle_after_reset"
    assert journal.read().claim is None
    assert journal.read().acquisition_pending is not None
    runtime.shutdown()


def test_native_acquire_rejection_replays_exact_error_without_second_call(
    tmp_path,
) -> None:
    class RejectingAdmission(_Admission):
        calls = 0

        def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
            self.calls += 1
            raise CellAdmissionError(
                "authoring_not_ready",
                "native authoring admission is closed",
                {"admission_generation": 1},
            )

    admission = RejectingAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
        ),
    )
    runtime.start()
    request = _start_request()
    for _attempt in range(2):
        with pytest.raises(ProgramRunnerError) as rejected:
            runtime.start_run(request)
        assert rejected.value.code == "authoring_not_ready"
        assert str(rejected.value) == "native authoring admission is closed"
    assert admission.calls == 1
    runtime.shutdown()


def test_stop_bypasses_transition_lock_and_full_ordinary_ledger(tmp_path) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_requests=1,
    )
    journal.record_request(
        owner_epoch="caller.1",
        request_id="ordinary.full",
        kind="start",
        intent={"ordinary": True},
        cycle_generation=1,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="running",
        program_run_id="program-run.1",
        claim={"claim_generation": 1},
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.1",
            maximum_replay_records=1,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()
    owner = _Owner()
    owner.rows["program-run.1"] = _Snapshot(
        "program-run.1",
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="running",
        revision=1,
    )
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]
    runtime.start()
    request = ProgramRunStopRequest.model_validate(
        {
            "request_id": "stop.independent",
            "request_owner_epoch": "caller.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "runner.1",
            "reason": "contain",
        }
    )
    reached = Event()
    completed = Event()
    original = host.stop_run

    def observed_stop(program_run_id: str, *, reason: str | None) -> _Snapshot:
        reached.set()
        return original(program_run_id, reason=reason)

    host.stop_run = observed_stop  # type: ignore[method-assign]

    def invoke() -> None:
        runtime.stop_run("program-run.1", request)
        completed.set()

    runtime._transition_lock.acquire()  # noqa: SLF001
    thread = Thread(target=invoke)
    thread.start()
    assert reached.wait(timeout=1)
    assert completed.wait(timeout=1)
    runtime._transition_lock.release()  # noqa: SLF001
    thread.join()
    assert host.stop_calls == [("program-run.1", "contain")]
    assert runtime.stop_run("program-run.1", request)["state"] == "stopped"
    assert host.stop_calls == [("program-run.1", "contain")]
    runtime.shutdown()


def test_runtime_rejects_a_stale_applied_selection_before_tree_admission(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    headless = _Headless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="program-runner.test",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="program-runner.test",
        ),
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]
    monkeypatch.setattr(
        headless,
        "program_applied",
        lambda: {
            "scenario_id": "another-scenario",
            "scenario_catalog_revision": _DIGEST_A,
            "source": {"record_digest": _DIGEST_B},
        },
    )

    runtime.start()
    with pytest.raises(ProgramRunnerError) as stale:
        runtime.start_run(_start_request())
    assert stale.value.code == "program_run_applied_selection_stale"
    assert host.start_admissions == []
    assert headless.selection_reads == 0
    runtime.shutdown()


def test_runtime_injects_and_closes_the_focused_simulation_owner() -> None:
    simulation = _SimulationControl()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=_Headless(),  # type: ignore[arg-type]
        simulation_control=simulation,  # type: ignore[arg-type]
    )

    assert runtime._ports.simulator_control is simulation  # noqa: SLF001
    assert runtime._owner_availability[  # noqa: SLF001
        "simulator_control"
    ].available
    assert runtime.health()["simulation_control_available"] is True
    runtime.shutdown()
    runtime.shutdown()
    assert simulation.closed == 1


def test_runtime_injects_and_closes_the_program_snapshot_source() -> None:
    headless = _Headless()
    model = _ModelTransport()
    snapshot = _SnapshotSource()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=headless,  # type: ignore[arg-type]
        model_transport=model,  # type: ignore[arg-type]
        snapshot_source=snapshot,  # type: ignore[arg-type]
    )

    assert runtime._assistant is not None  # noqa: SLF001
    assert runtime.health()["assistant_snapshot_source_available"] is True
    runtime.shutdown()
    runtime.shutdown()
    assert snapshot.closed == 1
    assert model.closed == 1
