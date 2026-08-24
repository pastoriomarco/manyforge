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
from manyforge_headless_api.cell_admission import (
    CellAdmissionError,
    cell_admission_owner_epoch_reconciliation_evidence_digest,
)
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
        "cclc_owner_identity",
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
                "applied_record_digest": _DIGEST_B,
                "scenario_id": "scenario.1",
                "scenario_catalog_revision": _DIGEST_A,
                "program_id": "program.1",
                "program_revision": _DIGEST_A,
                "program_binding_id": _DIGEST_B,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "node_environment_digest": _DIGEST_A,
                "node_policy_digest": _DIGEST_B,
                "node_policies": [],
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
        self.inventory: dict[str, Any] = {
            "status": "unknown",
            "kernel_owner_epoch": "kernel.epoch.1",
            "held_claims": [],
            "current_owner_epochs": [],
        }

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

    def recovery_inventory(self, *, owner_epoch: str) -> dict[str, Any]:
        assert owner_epoch
        return deepcopy(self.inventory)

    def recovery_receipt(self, _query: dict[str, Any]) -> dict[str, Any]:
        return deepcopy(self.inventory)


def _reconcile_runtime(
    tmp_path,
    *,
    journal_owner_epoch: str,
    runtime_owner_epoch: str,
    inventory: dict[str, Any],
) -> tuple[ProgramRuntime, ProgramCycleJournal, _Admission]:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch=journal_owner_epoch
    )
    admission = _Admission()
    admission.inventory = deepcopy(inventory)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch=runtime_owner_epoch,
            execution_state_root=tmp_path,
            cycle_journal_path=tmp_path / "cycle.sqlite3",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    return runtime, journal, admission


def test_manager_recovery_bootstraps_absent_program_runner_without_reconcile(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.new",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [],
        },
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert admission.reconciliations == []
    assert journal.read().owner_epoch == "runner.new"
    assert journal.liveness().manager_runtime_epoch == "manager.1"
    runtime.shutdown()


def test_manager_recovery_rotates_only_clean_idle_program_runner_owner(
    tmp_path,
) -> None:
    inventory = {
        "status": "unknown",
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [],
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.old"},
            {"owner_role": "manager", "owner_epoch": "manager-owner.1"},
        ],
    }
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.new",
        inventory=inventory,
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    request = admission.reconciliations[0]
    assert request["kernel_owner_epoch"] == "kernel.1"
    assert request["retained_claims"] == []
    assert request["current_owner_epochs"] == [
        {"owner_role": "program_runner", "owner_epoch": "runner.new"},
        {"owner_role": "manager", "owner_epoch": "manager-owner.1"},
    ]
    assert request["reconciliation_evidence_digest"] == (
        cell_admission_owner_epoch_reconciliation_evidence_digest(
            "program_runner", "runner.new"
        )
    )
    assert journal.read().owner_epoch == "runner.new"
    runtime.shutdown()


def test_manager_recovery_same_program_runner_epoch_is_transport_noop(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.same",
        runtime_owner_epoch="runner.same",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.same"}
            ],
        },
    )
    journal.retain_liveness_loss(
        manager_runtime_epoch="manager.old",
        manager_owner_epoch="manager-owner.old",
        fault_reference=_DIGEST_B,
        expected_generation=1,
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert admission.reconciliations == []
    assert journal.read().owner_epoch == "runner.same"
    assert journal.read().reconciliation_required is False
    runtime.shutdown()


def test_manager_recovery_rotates_exactly_released_terminal_predecessor(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "released_exact",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.old"}
            ],
        },
    )
    intent = {
        "claim_kind": "program_cycle",
        "owner_role": "program_runner",
        "owner_epoch": "runner.old",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": _DIGEST_A,
        "expected_admission_generation": 1,
        "scope": {"resources": [], "configurations": []},
        "program_cycle_evidence": {
            "draft_program_revision": _DIGEST_A,
            "applied_program_revision": _DIGEST_A,
        },
        "intervention_evidence": None,
    }
    claim = {
        "intent": intent,
        "claim_generation": 1,
        "acquired_admission_generation": 1,
        "accepted_at_ns": 1,
    }
    release = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.old",
        "owner_transaction_id": "program-cycle.1",
        "request_id": "start.1",
        "request_payload_digest": _DIGEST_A,
        "acquired_admission_generation": 1,
        "claim_generation": 1,
        "settlement": {
            "settlement_kind": "explicit_reset",
            "evidence_digest": _DIGEST_B,
        },
    }
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="program-cycle.1",
        claim=claim,
        terminal_reference=_DIGEST_B,
    )
    journal.remember_release_intent(
        expected_generation=1,
        claim_generation=1,
        release=release,
    )
    admission.inventory["matched_claim"] = claim
    admission.inventory["matched_release"] = release

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    assert journal.read().owner_epoch == "runner.new"
    assert journal.read().release_intent == release
    runtime.shutdown()


@pytest.mark.parametrize("checkpoint", ["held", "running", "acquire_unknown"])
def test_manager_recovery_keeps_uncontained_predecessor_fail_closed(
    tmp_path, checkpoint: str
) -> None:
    inventory = {
        "status": "unknown",
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [],
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.old"}
        ],
    }
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.new",
        inventory=inventory,
    )
    if checkpoint == "held":
        admission.inventory["held_claims"] = [
            {"intent": {"owner_role": "program_runner", "owner_epoch": "runner.old"}}
        ]
    elif checkpoint == "running":
        journal.transition(
            expected_generation=1,
            expected_states=("idle_after_reset",),
            state="running",
        )
    else:
        journal.begin_acquire(
            expected_generation=1,
            provisional_run_id="program-cycle.1",
            evidence={"request_id": "start.unknown"},
        )

    with pytest.raises(RuntimeError):
        runtime._reconcile_manager_liveness(  # noqa: SLF001
            _DIGEST_A, "manager.1", "manager-owner.1"
        )

    assert admission.reconciliations == []
    assert journal.read().owner_epoch == "runner.old"
    runtime.shutdown()


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
            "draft_program_revision": _DIGEST_A,
            "expected_admission_generation": 1,
        }
    )


def test_config_pairs_execution_root_exactly_with_cycle_journal(tmp_path) -> None:
    journal = tmp_path / "cycle.sqlite3"
    config = ProgramRunnerConfig(
        headless_url="http://headless.test",
        execution_state_root=tmp_path,
        cycle_journal_path=journal,
    )
    assert config.execution_state_root == tmp_path
    assert config.cycle_journal_path == journal

    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            execution_state_root=tmp_path,
        )
    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            cycle_journal_path=journal,
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
            "settlement_evidence_digest": status[
                "terminal_or_settlement_reference"
            ],
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
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=replacement,
    )
    runtime.start()
    assert runtime.health()["started"] is True
    status = runtime.cycle_status()
    assert status["state"] == "terminal_awaiting_reset"
    assert status["program_runner_owner_epoch"] == "runner.old"
    projected = runtime._current_cycle_status_projection()  # noqa: SLF001
    assert projected["program_runner_owner_epoch"] == "runner.old"
    assert projected["state"] == "terminal_awaiting_reset"
    assert admission.reconciliations == []
    assert admission.releases == []
    assert replacement.read().owner_epoch == "runner.old"
    assert replacement.read().state == "terminal_awaiting_reset"
    assert replacement.read().claim == claim
    assert replacement.read().reconciliation_required is True
    runtime.shutdown()


def test_restart_retries_containment_after_durable_loss_crash_window(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    old = ProgramCycleJournal(path, owner_epoch="runner.old")
    intent = {
        "owner_role": "program_runner",
        "owner_epoch": "runner.old",
        "owner_transaction_id": "program-run.1",
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
        provisional_run_id="program-run.1",
        evidence=intent,
    )
    old.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-run.1",
        claim=claim,
    )
    old.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="running",
        program_run_id="program-run.1",
    )
    old.retain_liveness_manager(
        manager_runtime_epoch="manager.old",
        manager_owner_epoch="manager-owner.old",
    )
    old.retain_liveness_loss(
        manager_runtime_epoch="manager.old",
        manager_owner_epoch="manager-owner.old",
        fault_reference=_DIGEST_B,
        expected_generation=1,
    )
    old.close()

    replacement = ProgramCycleJournal(path, owner_epoch="runner.new")
    with pytest.raises(RuntimeError, match="not contained"):
        replacement.reconcile_liveness(
            owner_epoch="runner.new",
            evidence_digest=_DIGEST_A,
        )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.new",
            execution_state_root=tmp_path,
            cycle_journal_path=path,
            manager_liveness_socket=tmp_path / "runner.sock",
            cell_id="cell",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=replacement,
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
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]

    runtime.start()

    assert host.start_admissions == []
    assert host.stop_calls == [
        ("program-run.1", "manager_presence_expired")
    ]
    assert replacement.read().state == "terminal_awaiting_reset"
    assert replacement.read().reconciliation_required is True
    assert runtime.liveness_status()["command_fenced"] is True
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
            "settlement_evidence_digest": _DIGEST_B,
        }
    )

    runtime.start()
    with pytest.raises(ProgramRunnerError) as rejected:
        runtime.reset_cycle(request)
    assert rejected.value.code == "settlement_validator_unavailable"
    assert journal.read().reconciliation_required is False
    assert journal.read().state == "terminal_awaiting_reset"
    assert journal.read().release_intent == admission.releases[0]

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
            "settlement_evidence_digest": _DIGEST_B,
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
    request = ProgramRunStartRequest.model_validate(
        {
            **_start_request().model_dump(mode="json"),
            "draft_program_revision": _DIGEST_B,
        }
    )
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
