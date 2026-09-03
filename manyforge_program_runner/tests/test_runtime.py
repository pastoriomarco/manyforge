"""Production Program-runner admission, replay, and lifecycle glue."""

from __future__ import annotations

from copy import deepcopy
from threading import Barrier, Event, Thread
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_behavior.node_availability import AvailabilityFact
from manyforge_behavior.program_run_owner import ProgramRunOwnerError
from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
    RuntimeBlackboardWriteRequest,
    RuntimeOverrideActionRequest,
    RuntimeOverridePromotionRequest,
    RuntimeOverrideStageRequest,
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
from manyforge_program_runner.models import (
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_ACTIVE_STATES = {
    "running",
    "intervention_required",
    "full_stopping",
    "full_stopped",
    "stopping",
    "finalizing",
}


def test_recovery_fences_join_program_and_execution_owner_contracts() -> None:
    applied = {
        "program_binding_id": "sha256:" + "1" * 64,
        "retained_bundle_id": _DIGEST_A,
        "graph_revision": _DIGEST_B,
        "source": {"source_generation": 7},
    }
    execution = {
        "ok": True,
        "identity": {
            # This identifies the execution owner's direct-control surface,
            # not the scenario's Program Binding above.
            "program_binding_id": "sha256:" + "2" * 64,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "active_publication_generation": 11,
            "scene_revision": {"revision_id": 3},
        },
    }

    assert ProgramRuntime._run_recovery_fences(
        applied,
        execution,
        scene_admission_epoch=13,
    ) == {
        "program_generation": 7,
        "deployment_generation": 11,
        "scene_admission_epoch": 13,
        "pose_source_apply_generation": 0,
    }

    execution["identity"]["graph_revision"] = _DIGEST_A
    with pytest.raises(
        ProgramRunnerError,
        match="current owners did not provide exact recovery generations",
    ):
        ProgramRuntime._run_recovery_fences(
            applied,
            execution,
            scene_admission_epoch=13,
        )


class _Headless:
    """Construction-complete transport whose unused effects fail loudly."""

    _PORT_METHODS = {
        "acknowledge_completed",
        "authoring_admission",
        "commit_scene_transaction",
        "cclc_component",
        "cclc_owner_identity",
        "cclc_route_templates",
        "component_lifecycle_status",
        "commit_program_model_attempt",
        "submit",
        "manager_graph",
        "read",
        "pause",
        "reset",
        "resume",
        "scene",
        "stop",
        "stop_program_run",
        "stop_status",
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
            "source": {
                "record_digest": _DIGEST_B,
                "source_generation": 1,
            },
        }

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        self.selection_reads += 1
        assert scenario_id == "cell-pick"
        return {"scenario_id": scenario_id, "program_revision": _DIGEST_A}

    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "program_binding_id": None,
                "retained_bundle_id": None,
                "graph_revision": None,
                "active_publication_generation": 1,
                "scene_revision": {"revision_id": 1},
            },
        }

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
        capture = {
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
        }
        self.capture = SimpleNamespace(
            program_run_id=program_run_id,
            to_dict=lambda: deepcopy(capture),
        )
        self._value = {
            "schema_version": "manyforge.program_run_status.v2",
            "owner_id": owner_id,
            "owner_epoch": owner_epoch,
            "run_revision": revision,
            "state": state,
            "capture": capture,
            "tree_status": "running",
            "node_status_revision": revision,
            "node_statuses": {"root": "running"},
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

    @property
    def run_revision(self) -> int:
        return self._value["run_revision"]

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

    def intervention(self, _program_run_id: str):
        return None


class _ObservationOwner(_Owner):
    def read(self, program_run_id: str) -> _Snapshot:
        try:
            return super().read(program_run_id)
        except KeyError as error:
            raise ProgramRunOwnerError(
                f"Program run {program_run_id} is not resident"
            ) from error


class _RuntimeControlOwner(_Owner):
    def __init__(self) -> None:
        super().__init__()
        self.effects: list[tuple[str, dict[str, Any]]] = []

    def _advance(self, program_run_id: str) -> _Snapshot:
        current = self.rows[program_run_id]
        advanced = _Snapshot(
            program_run_id,
            owner_id=current.to_dict()["owner_id"],
            owner_epoch=current.owner_epoch,
            state=current.state,
            revision=current.run_revision + 1,
        )
        self.rows[program_run_id] = advanced
        return advanced

    def validate_runtime_override(
        self,
        _program_run_id: str,
        **values: Any,
    ) -> Any:
        return deepcopy(values["value"])

    def note_runtime_control_change(self, program_run_id: str) -> _Snapshot:
        return self._advance(program_run_id)

    def activate_runtime_override(
        self,
        program_run_id: str,
        **values: Any,
    ) -> _Snapshot:
        self.effects.append(("activate", deepcopy(values)))
        return self._advance(program_run_id)

    def clear_runtime_override(
        self,
        program_run_id: str,
        **values: Any,
    ) -> _Snapshot:
        self.effects.append(("clear", deepcopy(values)))
        return self._advance(program_run_id)

    def validate_runtime_override_promotion(
        self,
        _program_run_id: str,
        **_values: Any,
    ) -> str:
        return "motion.move_to_pose"

    def write_runtime_blackboard(
        self,
        program_run_id: str,
        **values: Any,
    ) -> _Snapshot:
        self.effects.append(("write", deepcopy(values)))
        return self._advance(program_run_id)


class _Host:
    def __init__(self, owner: _Owner, *, owner_id: str, owner_epoch: str) -> None:
        self.owner = owner
        self.owner_id = owner_id
        self.owner_epoch = owner_epoch
        self.started = 0
        self.start_admissions: list[object] = []
        self.stop_calls: list[tuple[str, str | None]] = []
        self.motion_stop_calls: list[tuple[str, str | None]] = []
        self.full_stop_calls: list[tuple[str, str | None]] = []
        self.full_stop_resume_calls: list[tuple[str, str | None]] = []
        self.reset_calls: list[str] = []
        self.retained_reset_calls: list[str] = []
        self.shutdowns = 0

    def start(self) -> None:
        self.started += 1

    def start_run(
        self,
        admission: object,
        *,
        program_run_id: str | None = None,
    ) -> _Snapshot:
        self.start_admissions.append(admission)
        admitted_program_run_id = (
            f"program-run.{len(self.start_admissions)}"
            if program_run_id is None
            else program_run_id
        )
        snapshot = _Snapshot(
            admitted_program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=1,
        )
        self.owner.rows[admitted_program_run_id] = snapshot
        return snapshot

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        self.stop_calls.append((program_run_id, reason))
        current = self.owner.rows.get(program_run_id)
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="stopped",
            revision=1 if current is None else current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def stop_motion(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        self.motion_stop_calls.append((program_run_id, reason))
        current = self.owner.rows.get(program_run_id)
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=1 if current is None else current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def full_stop(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        self.full_stop_calls.append((program_run_id, reason))
        current = self.owner.rows.get(program_run_id)
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="full_stopped",
            revision=1 if current is None else current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def resume_full_stop(
        self,
        program_run_id: str,
        *,
        request_id: str,
        reason: str | None,
    ) -> _Snapshot:
        self.full_stop_resume_calls.append((program_run_id, reason))
        current = self.owner.rows[program_run_id]
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def reset_run(self, program_run_id: str) -> _Snapshot:
        self.reset_calls.append(program_run_id)
        return self.owner.rows.get(
            program_run_id,
            _Snapshot(
                program_run_id,
                owner_id=self.owner_id,
                owner_epoch=self.owner_epoch,
                state="stopped",
                revision=2,
            ),
        )

    def reset_retained_run(self, program_run_id: str) -> None:
        self.retained_reset_calls.append(program_run_id)

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
        self.reconcile_error: Exception | None = None
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
        if self.reconcile_error is not None:
            raise self.reconcile_error
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
    execution_run_root = tmp_path / "execution-run"
    execution_run_root.mkdir(exist_ok=True)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch=runtime_owner_epoch,
            execution_run_root=execution_run_root,
            cycle_journal_path=tmp_path / "cycle.sqlite3",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    return runtime, journal, admission


def _program_cycle_claim(owner_epoch: str) -> dict[str, Any]:
    intent = {
        "claim_kind": "program_cycle",
        "owner_role": "program_runner",
        "owner_epoch": owner_epoch,
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
    return {
        "intent": intent,
        "claim_generation": 1,
        "acquired_admission_generation": 1,
        "accepted_at_ns": 1,
    }


def _retain_terminal_cycle(
    journal: ProgramCycleJournal, claim: dict[str, Any]
) -> str:
    settlement = {
        "schema_version": "manyforge.program_cycle_start_settlement.v1",
        "kind": "proven_no_effect",
        "claim": claim,
        "program_run_id": "program-cycle.1",
        "prepared_scene": {
            "schema_version": "manyforge.prepared_program_scene.v1"
        },
    }
    reference = ProgramRuntime._digest(settlement)  # noqa: SLF001
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id="program-cycle.1",
        claim=claim,
        terminal_reference=reference,
        start_settlement=settlement,
    )
    return reference


def _program_reconcile_settlement(
    claim: dict[str, Any], successor_owner_epoch: str
) -> dict[str, Any]:
    return {
        "predecessor_claim": claim,
        "successor_owner_role": "program_runner",
        "successor_owner_epoch": successor_owner_epoch,
        "settlement_predicate": "reconciled",
        "reconciliation_evidence_digest": (
            cell_admission_owner_epoch_reconciliation_evidence_digest(
                "program_runner", successor_owner_epoch
            )
        ),
    }


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


def test_manager_recovery_repairs_a_prior_local_first_partial_rotation(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.orphaned-successor",
        runtime_owner_epoch="runner.current",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.old"}
            ],
        },
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    assert admission.reconciliations[0]["current_owner_epochs"] == [
        {"owner_role": "program_runner", "owner_epoch": "runner.current"}
    ]
    assert journal.read().owner_epoch == "runner.current"
    runtime.shutdown()


def test_manager_recovery_finishes_a_prior_kernel_first_partial_rotation(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.current",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.current"}
            ],
        },
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert admission.reconciliations == []
    assert journal.read().owner_epoch == "runner.current"
    runtime.shutdown()


def test_manager_recovery_does_not_advance_local_owner_before_kernel(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.current",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.old"}
            ],
        },
    )
    admission.reconcile_error = RuntimeError("Kernel reconciliation unavailable")

    with pytest.raises(RuntimeError, match="Kernel reconciliation unavailable"):
        runtime._reconcile_manager_liveness(  # noqa: SLF001
            _DIGEST_A, "manager.1", "manager-owner.1"
        )

    prepared = journal.read()
    assert prepared.owner_epoch == "runner.old"
    assert prepared.reconciliation_required is True
    assert prepared.reconciliation_evidence_digest == (
        cell_admission_owner_epoch_reconciliation_evidence_digest(
            "program_runner", "runner.current"
        )
    )
    admission.reconcile_error = None
    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )
    assert journal.read().owner_epoch == "runner.current"
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
    terminal_reference = _retain_terminal_cycle(journal, claim)
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
            "evidence_digest": terminal_reference,
        },
    }
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


def test_manager_recovery_reconciles_terminal_held_claim_then_reset_is_explicit(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "held_exact",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.old"}
            ],
        },
    )
    claim = _program_cycle_claim("runner.old")
    reference = _retain_terminal_cycle(journal, claim)
    admission.inventory.update(
        {
            "held_claims": [claim],
            "matched_claim": claim,
            "matched_release": None,
            "matched_reconcile_settlement": None,
        }
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    assert admission.reconciliations[0]["retained_claims"] == [claim]
    reconciled = journal.read()
    assert reconciled.state == "terminal_awaiting_reset"
    assert reconciled.owner_epoch == "runner.new"
    assert reconciled.claim == claim
    assert reconciled.release_intent is None
    assert reconciled.reconciliation_required is False

    admission.inventory = {
        "status": "reconciled_exact",
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [],
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.new"}
        ],
        "matched_claim": claim,
        "matched_release": None,
        "matched_reconcile_settlement": _program_reconcile_settlement(
            claim, "runner.new"
        ),
    }
    runtime._started = True  # noqa: SLF001
    result = runtime.reset_cycle(
        ProgramCycleResetRequest.model_validate(
            {
                "request_id": "request.reset.after-reconcile",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
                "program_run_id": "program-cycle.1",
                "expected_owner_epoch": "runner.new",
                "expected_cycle_generation": 1,
                "expected_claim_generation": 1,
                "terminal_reference": reference,
                "settlement_evidence_digest": reference,
            }
        )
    )

    assert admission.releases == []
    assert result["admission"] == {"admission_generation": 1}
    assert journal.read().state == "idle_after_reset"
    runtime._started = False  # noqa: SLF001
    runtime.shutdown()


def test_manager_recovery_validates_kernel_first_terminal_settlement(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.old",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "reconciled_exact",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.new"}
            ],
        },
    )
    claim = _program_cycle_claim("runner.old")
    _retain_terminal_cycle(journal, claim)
    reconciliation_digest = (
        cell_admission_owner_epoch_reconciliation_evidence_digest(
            "program_runner", "runner.new"
        )
    )
    journal.prepare_liveness_reconciliation(
        predecessor_owner_epoch="runner.old",
        evidence_digest=reconciliation_digest,
        require_terminal_release=False,
    )
    admission.inventory.update(
        {
            "matched_claim": claim,
            "matched_release": None,
            "matched_reconcile_settlement": _program_reconcile_settlement(
                claim, "runner.wrong"
            ),
        }
    )

    with pytest.raises(RuntimeError, match="settlement is not exact"):
        runtime._reconcile_manager_liveness(  # noqa: SLF001
            _DIGEST_A, "manager.1", "manager-owner.1"
        )

    admission.inventory["matched_reconcile_settlement"] = (
        _program_reconcile_settlement(claim, "runner.new")
    )
    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )
    assert journal.read().owner_epoch == "runner.new"
    assert admission.reconciliations == []
    runtime.shutdown()


def test_manager_recovery_rotates_terminal_after_prior_settlement_compaction(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.original",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.mid"}
            ],
            "matched_claim": None,
            "matched_release": None,
            "matched_reconcile_settlement": None,
        },
    )
    claim = _program_cycle_claim("runner.original")
    _retain_terminal_cycle(journal, claim)
    journal.reconcile_liveness(
        owner_epoch="runner.mid",
        evidence_digest=(
            cell_admission_owner_epoch_reconciliation_evidence_digest(
                "program_runner", "runner.mid"
            )
        ),
        require_terminal_release=False,
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    assert admission.reconciliations[0]["retained_claims"] == []
    cycle = journal.read()
    assert cycle.state == "terminal_awaiting_reset"
    assert cycle.owner_epoch == "runner.new"
    assert cycle.claim == claim
    assert cycle.release_intent is None
    runtime.shutdown()


def test_manager_recovery_supersedes_stale_terminal_rotation_intent(
    tmp_path,
) -> None:
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.original",
        runtime_owner_epoch="runner.new",
        inventory={
            "status": "reconciled_exact",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [
                {"owner_role": "program_runner", "owner_epoch": "runner.mid"}
            ],
        },
    )
    claim = _program_cycle_claim("runner.original")
    _retain_terminal_cycle(journal, claim)
    journal.reconcile_liveness(
        owner_epoch="runner.mid",
        evidence_digest=(
            cell_admission_owner_epoch_reconciliation_evidence_digest(
                "program_runner", "runner.mid"
            )
        ),
        require_terminal_release=False,
    )
    journal.prepare_liveness_reconciliation(
        predecessor_owner_epoch="runner.mid",
        evidence_digest=(
            cell_admission_owner_epoch_reconciliation_evidence_digest(
                "program_runner", "runner.stale"
            )
        ),
        require_terminal_release=False,
    )
    admission.inventory.update(
        {
            "matched_claim": claim,
            "matched_release": None,
            "matched_reconcile_settlement": _program_reconcile_settlement(
                claim, "runner.mid"
            ),
        }
    )

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A, "manager.1", "manager-owner.1"
    )

    assert len(admission.reconciliations) == 1
    assert admission.reconciliations[0]["retained_claims"] == []
    cycle = journal.read()
    assert cycle.state == "terminal_awaiting_reset"
    assert cycle.owner_epoch == "runner.new"
    assert cycle.claim == claim
    assert cycle.release_intent is None
    assert cycle.reconciliation_required is False
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


def _retain_no_effect_terminal(
    journal: ProgramCycleJournal, *, program_run_id: str
) -> str:
    retained = journal.read()
    assert retained.claim is not None
    settlement = {
        "schema_version": "manyforge.program_cycle_start_settlement.v1",
        "kind": "proven_no_effect",
        "claim": retained.claim,
        "program_run_id": program_run_id,
        "prepared_scene": {
            "schema_version": "manyforge.prepared_program_scene.v1"
        },
    }
    reference = ProgramRuntime._digest(settlement)  # noqa: SLF001
    journal.transition(
        expected_generation=retained.cycle_generation,
        expected_states=("starting",),
        state="terminal_awaiting_reset",
        terminal_reference=reference,
        start_settlement=settlement,
    )
    return reference


def _bypass_scene_preparation(monkeypatch: pytest.MonkeyPatch) -> None:
    proof = {
        "schema_version": "manyforge.prepared_program_scene.v1",
        "desired_scene_digest": _DIGEST_A,
    }
    monkeypatch.setattr(
        ProgramRuntime,
        "_prepare_run_scene",
        lambda _self, _admission, _run_id: (object(), (), deepcopy(proof)),
    )
    monkeypatch.setattr(
        ProgramRuntime,
        "_verify_run_scene",
        lambda _self, _context, _scene, _proof: None,
    )


def _bypass_reset_scene_preparation(monkeypatch: pytest.MonkeyPatch) -> None:
    """Keep lifecycle-only tests independent from Applied-scene fixtures."""

    monkeypatch.setattr(
        ProgramRuntime,
        "_prepare_scene_after_reset",
        lambda _self, _request: None,
    )


class _PreparedRunAdmission:
    def __init__(self) -> None:
        self.program = SimpleNamespace(to_dict=lambda: {"scene": []})

    def port_context(self, program_run_id: str) -> object:
        return SimpleNamespace(program_run_id=program_run_id)


def test_applied_activation_prepares_scene_while_idle_without_run_claim(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )

    class IdleAdmission(_Admission):
        def get(self) -> dict[str, Any]:
            return {
                "admission_generation": 1,
                "authoring_allowed": True,
            }

    admission = IdleAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.activation-scene.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.activation-scene.1"
        ),
    )
    prepared: list[str] = []
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "prepare_initial_scene",
        lambda context, _scene: (
            prepared.append(context.program_run_id)
            or {"schema_version": "manyforge.prepared_program_scene.v1"}
        ),
    )
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "verify_initial_scene",
        lambda *_args: None,
    )
    runtime.start()

    result = runtime.prepare_applied_scene(
        ProgramScenePreparationRequest.model_validate(
            {
                "request_id": "activation.1",
                "request_owner_epoch": "composer.1",
                "source_label": "manyforge_composer_activation",
                "correlation_id": None,
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 1,
            }
        )
    )

    assert result["status"] == "prepared"
    assert result["admission_generation"] == 1
    assert prepared and prepared[0].startswith("program-scene.")
    assert admission.claims == []
    assert runtime.cycle_status()["state"] == "idle_after_reset"
    runtime.shutdown()


def test_run_prepares_while_idle_then_only_verifies_after_claim(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    events: list[str] = []

    class OrderedAdmission(_Admission):
        generation = 1

        def get(self) -> dict[str, Any]:
            events.append(f"admission:{self.generation}")
            return {"admission_generation": self.generation}

        def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
            events.append("claim")
            assert intent["expected_admission_generation"] == 3
            claim = super().acquire(intent)
            claim["acquired_admission_generation"] = 3
            return claim

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    admission = OrderedAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.scene-order.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.scene-order.1"
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001

    def prepare(_context: object, _scene: object) -> dict[str, Any]:
        events.append("prepare")
        admission.generation = 3
        return {"schema_version": "manyforge.prepared_program_scene.v1"}

    monkeypatch.setattr(runtime._scene, "prepare_initial_scene", prepare)  # noqa: SLF001
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "verify_initial_scene",
        lambda *_args: events.append("verify"),
    )
    runtime.start()

    runtime.start_run(_start_request())

    assert events == ["admission:1", "prepare", "admission:3", "claim", "verify"]
    status = runtime.cycle_status()
    run = runtime.read_run(status["program_run_id"])
    assert status["state"] == "running"
    assert status["run_revision"] == run["run_revision"]
    assert status["node_status_revision"] == run["node_status_revision"]
    assert status["run"] == run
    assert status["runtime_controls"]["program_run_id"] == run["capture"][
        "program_run_id"
    ]
    assert (
        status["runtime_controls"]["run_revision"]
        == status["run_revision"]
    )
    assert (
        status["runtime_controls"]["runtime_controls"][
            "runtime_control_revision"
        ]
        == status["runtime_control_revision"]
    )
    runtime.shutdown()


def test_scene_preparation_failure_leaves_cycle_idle_without_claim(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.prepare-fail.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.prepare-fail.1"
        ),
    )
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "prepare_initial_scene",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("scene unavailable")),
    )
    runtime.start()

    with pytest.raises(ProgramRunnerError, match="scene unavailable"):
        runtime.start_run(_start_request())

    assert admission.claims == []
    assert runtime.cycle_status()["state"] == "idle_after_reset"
    runtime.shutdown()


def test_post_claim_scene_race_records_resettable_no_effect_settlement(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.scene-race.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.scene-race.1"
        ),
    )
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "prepare_initial_scene",
        lambda *_args: {"schema_version": "manyforge.prepared_program_scene.v1"},
    )
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "verify_initial_scene",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("scene raced")),
    )
    runtime.start()

    with pytest.raises(ProgramRunnerError, match="scene raced"):
        runtime.start_run(_start_request())

    status = runtime.cycle_status()
    retained = runtime._journal.read()  # noqa: SLF001
    assert status["state"] == "terminal_awaiting_reset"
    assert status["program_run_id"] == "program-cycle.1"
    assert status["terminal_or_settlement_reference"].startswith("sha256:")
    assert retained.start_settlement is not None
    assert retained.start_settlement["kind"] == "proven_no_effect"
    assert runtime._digest(retained.start_settlement) == (  # noqa: SLF001
        status["terminal_or_settlement_reference"]
    )
    _bypass_reset_scene_preparation(monkeypatch)
    reset = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "request.reset.scene-race",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": status["program_run_id"],
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": status["cycle_generation"],
            "expected_claim_generation": status["cycle_claim_generation"],
            "terminal_reference": status["terminal_or_settlement_reference"],
            "settlement_evidence_digest": status[
                "terminal_or_settlement_reference"
            ],
        }
    )
    runtime.reset_cycle(reset)
    assert len(admission.releases) == 1
    assert runtime.cycle_status()["state"] == "idle_after_reset"
    runtime.shutdown()


def test_host_start_without_a_result_retains_claim_for_reconciliation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    admission = _Admission()
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.start-unknown.1"
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.start-unknown.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    monkeypatch.setattr(
        runtime._host,  # noqa: SLF001
        "start_run",
        lambda _admission, **_kwargs: (_ for _ in ()).throw(
            RuntimeError("owner response was lost")
        ),
    )

    with pytest.raises(ProgramRunnerError, match="owner response was lost"):
        runtime.start_run(_start_request())

    retained = journal.read()
    assert retained.reconciliation_required is True
    assert retained.claim is not None
    assert retained.start_settlement is None
    assert admission.releases == []
    runtime.shutdown()


def test_config_pairs_execution_root_exactly_with_cycle_journal(tmp_path) -> None:
    execution_run_root = tmp_path / "execution-run"
    execution_run_root.mkdir()
    journal = tmp_path / "state" / "cycle.sqlite3"
    config = ProgramRunnerConfig(
        headless_url="http://headless.test",
        execution_run_root=execution_run_root,
        cycle_journal_path=journal,
    )
    assert config.execution_run_root == execution_run_root
    assert config.cycle_journal_path == journal

    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            execution_run_root=execution_run_root,
        )
    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            cycle_journal_path=journal,
        )
    with pytest.raises(ValueError, match="outside disposable"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            execution_run_root=execution_run_root,
            cycle_journal_path=execution_run_root / "cycle.sqlite3",
        )


def test_repeated_concurrent_run_observation_is_strictly_read_only(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.observation"
    )
    history = _Snapshot(
        "run.history",
        owner_id="program-runner.test",
        owner_epoch="runner.observation",
        state="succeeded",
        revision=3,
    )
    journal.retain_run_snapshot(history.to_dict())
    owner = _ObservationOwner()
    owner.rows["run.live"] = _Snapshot(
        "run.live",
        owner_id="program-runner.test",
        owner_epoch="runner.observation",
        state="running",
        revision=2,
    )
    owner.rows["run.resident-terminal"] = _Snapshot(
        "run.resident-terminal",
        owner_id="program-runner.test",
        owner_epoch="runner.observation",
        state="stopped",
        revision=4,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.test",
            owner_epoch="runner.observation",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    retain_calls: list[dict[str, Any]] = []

    def forbidden_retention(snapshot: dict[str, Any]) -> None:
        retain_calls.append(snapshot)
        raise AssertionError("a Program-run GET attempted durable retention")

    journal.retain_run_snapshot = forbidden_retention  # type: ignore[method-assign]
    changes_before = journal._database.total_changes  # noqa: SLF001
    barrier = Barrier(5)
    errors: list[BaseException] = []

    def observe() -> None:
        barrier.wait()
        try:
            for _ in range(20):
                all_runs = runtime.runs(active_only=False, limit=10)
                assert {
                    value["capture"]["program_run_id"]
                    for value in all_runs["runs"]
                } == {
                    "run.history",
                    "run.live",
                    "run.resident-terminal",
                }
                assert runtime.runs(active_only=True, limit=10)["runs"][0][
                    "capture"
                ]["program_run_id"] == "run.live"
                assert runtime.read_run("run.live")["state"] == "running"
                assert runtime.read_run("run.history")["state"] == "succeeded"
        except BaseException as error:
            errors.append(error)

    threads = [Thread(target=observe) for _ in range(4)]
    for thread in threads:
        thread.start()
    barrier.wait()
    for thread in threads:
        thread.join()

    assert errors == []
    assert retain_calls == []
    assert journal._database.total_changes == changes_before  # noqa: SLF001
    runtime.shutdown()


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
    _bypass_scene_preparation(monkeypatch)
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
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
    assert started["capture"]["program_run_id"] == "program-cycle.1"
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

    motion_stop = ProgramRunStopRequest.model_validate(
        {
            "request_id": "request.motion-stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "reason": "ordinary operator motion Stop",
        }
    )
    cycle_journal = runtime._journal  # noqa: SLF001
    assert cycle_journal is not None
    original_retain = cycle_journal.retain_run_snapshot
    stop_path_retention: list[dict[str, Any]] = []

    def retain_on_stop_path(snapshot: dict[str, Any]) -> None:
        stop_path_retention.append(snapshot)

    cycle_journal.retain_run_snapshot = retain_on_stop_path  # type: ignore[method-assign]
    motion_stopped = runtime.stop_motion("program-cycle.1", motion_stop)
    assert runtime.stop_motion("program-cycle.1", motion_stop) == motion_stopped
    cycle_journal.retain_run_snapshot = original_retain  # type: ignore[method-assign]
    assert motion_stopped["state"] == "running"
    assert stop_path_retention == []
    assert host.motion_stop_calls == [
        ("program-cycle.1", "ordinary operator motion Stop")
    ]
    assert runtime.cycle_status()["state"] == "running"

    full_stop = ProgramRunStopRequest.model_validate(
        {
            **motion_stop.model_dump(mode="json"),
            "request_id": "request.full-stop.1",
            "reason": "ordinary operator Full Stop",
        }
    )
    full_stopped = runtime.full_stop("program-cycle.1", full_stop)
    assert runtime.full_stop("program-cycle.1", full_stop) == full_stopped
    assert full_stopped["state"] == "full_stopped"
    assert host.full_stop_calls == [
        ("program-cycle.1", "ordinary operator Full Stop")
    ]
    assert runtime.cycle_status()["state"] == "running"

    resume_full_stop = ProgramFullStopResumeRequest.model_validate(
        {
            "request_id": "request.resume-full-stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_run_revision": full_stopped["run_revision"],
            "reason": "operator Run after Full Stop",
        }
    )
    resumed = runtime.resume_full_stop(
        "program-cycle.1", resume_full_stop
    )
    assert runtime.resume_full_stop(
        "program-cycle.1", resume_full_stop
    ) == resumed
    assert resumed["state"] == "running"
    assert host.full_stop_resume_calls == [
        ("program-cycle.1", "operator Run after Full Stop")
    ]
    assert runtime.cycle_status()["state"] == "running"

    stop = ProgramRunStopRequest.model_validate(
        {
            "request_id": "request.stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "reason": "operator stop",
        }
    )
    owner_stop = host.stop_run

    def stop_with_terminal_observer(
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        snapshot = owner_stop(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )
        runtime._record_terminal_snapshot(snapshot)  # noqa: SLF001
        return snapshot

    host.stop_run = stop_with_terminal_observer  # type: ignore[method-assign]
    stopped = runtime.stop_run("program-cycle.1", stop)
    stop_replay = runtime.stop_run("program-cycle.1", stop)
    assert stopped == stop_replay
    assert host.stop_calls == [("program-cycle.1", "operator stop")]
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
            "program_run_id": "program-cycle.1",
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
    assert host.reset_calls == ["program-cycle.1"]
    assert runtime.cycle_status()["state"] == "idle_after_reset"

    second = runtime.start_run(_start_request(request_id="request.start.after-reset"))
    assert second["capture"]["program_run_id"] == "program-cycle.2"
    assert runtime.cycle_status()["state"] == "running"
    assert len(host.start_admissions) == 2
    second_full_stop = ProgramRunStopRequest.model_validate(
        {
            **full_stop.model_dump(mode="json"),
            "request_id": "request.full-stop.2",
            "program_run_id": "program-cycle.2",
        }
    )
    second_full_stopped = runtime.full_stop(
        "program-cycle.2", second_full_stop
    )
    current_cycle = runtime.cycle_status()
    discard = ProgramFullStopResetRequest.model_validate(
        {
            "request_id": "request.discard-full-stop.2",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.2",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_run_revision": second_full_stopped["run_revision"],
            "expected_cycle_generation": current_cycle["cycle_generation"],
            "expected_claim_generation": current_cycle[
                "cycle_claim_generation"
            ],
            "reason": "operator Reset discarded Full Stop",
        }
    )
    discarded = runtime.discard_full_stop("program-cycle.2", discard)
    assert runtime.discard_full_stop("program-cycle.2", discard) == discarded
    assert discarded["state"] == "stopped"
    terminal_cycle = runtime.cycle_status()
    assert terminal_cycle["state"] == "terminal_awaiting_reset"
    second_reset = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "request.reset.2",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": discard.request_id,
            "program_run_id": "program-cycle.2",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": terminal_cycle["cycle_generation"],
            "expected_claim_generation": terminal_cycle[
                "cycle_claim_generation"
            ],
            "terminal_reference": terminal_cycle[
                "terminal_or_settlement_reference"
            ],
            "settlement_evidence_digest": terminal_cycle[
                "terminal_or_settlement_reference"
            ],
        }
    )
    runtime.reset_cycle(second_reset)
    assert runtime.cycle_status()["state"] == "idle_after_reset"

    third = runtime.start_run(
        _start_request(request_id="request.start.after-full-stop-reset")
    )
    third_stop = ProgramRunStopRequest.model_validate(
        {
            **stop.model_dump(mode="json"),
            "request_id": "request.stop.3",
            "program_run_id": third["capture"]["program_run_id"],
        }
    )
    runtime.stop_run(third["capture"]["program_run_id"], third_stop)

    runtime.shutdown()
    assert host.shutdowns == 1
    assert host.stop_calls == [
        ("program-cycle.1", "operator stop"),
        ("program-cycle.2", "operator Reset discarded Full Stop"),
        ("program-cycle.3", "operator stop"),
    ]


def test_operator_motion_stop_reset_stops_tree_and_resets_cycle_once(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    admission = _Admission()
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="program-runner.operator-stop-reset.1",
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            owner_epoch="program-runner.operator-stop-reset.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    runtime.start_run(_start_request())

    intervention = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id="program-cycle.1",
            node_instance_id="move.1",
            node_attempt_id="attempt.1",
        ),
        owner_task_id="operation.1",
        policy_parameter="operator_stop_policy",
    )
    owner.intervention = lambda _run_id: (  # type: ignore[method-assign]
        intervention if journal.read().state == "running" else None
    )
    offer = {
        "offer_id": _DIGEST_A,
        "offer_revision": 1,
        "actions": [{"action_id": "reset_cycle", "enabled": True}],
    }
    runtime._recovery_offer = (  # type: ignore[method-assign]  # noqa: SLF001
        lambda _run_id, *, cycle_generation: (
            deepcopy(offer)
            if cycle_generation == 1 and journal.read().state == "running"
            else None
        )
    )
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_id": "request.operator-stop-reset.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": "operator-stop-reset.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "offer_id": offer["offer_id"],
            "offer_revision": offer["offer_revision"],
            "action_id": "reset_cycle",
            "reason": "operator Reset after motion Stop",
        }
    )

    result = runtime.apply_recovery_action(request)

    assert runtime.apply_recovery_action(request) == result
    assert result["schema_version"] == "manyforge.cycle_recovery_result.v2"
    assert result["action_id"] == "reset_cycle"
    assert result["cycle_resumed"] is False
    assert result["run"]["state"] == "stopped"
    assert result["recovery_offer"] is None
    assert host.stop_calls == [
        ("program-cycle.1", "operator Reset after motion Stop")
    ]
    assert host.reset_calls == ["program-cycle.1"]
    assert len(admission.releases) == 1
    assert journal.read().state == "idle_after_reset"
    runtime.shutdown()


def test_cycle_status_projects_pending_acquire_as_starting(tmp_path) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
    )
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-cycle.1",
        evidence={"request_id": "request.start.1"},
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )

    status = runtime._cycle_status_core()  # noqa: SLF001

    assert status["state"] == "starting"
    assert status["program_run_id"] == "program-cycle.1"
    assert status["cycle_claim_generation"] is None
    runtime.shutdown()


def test_runtime_controls_replay_and_restore_one_run_scoped_ledger(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    journal_path = tmp_path / "cycle.sqlite3"
    owner_epoch = "program-runner.controls.1"
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch=owner_epoch,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            journal_path,
            owner_epoch=owner_epoch,
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _RuntimeControlOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    started = runtime.start_run(_start_request())
    program_run_id = started["capture"]["program_run_id"]

    def request_base(request_id: str, expected_run_revision: int) -> dict[str, Any]:
        return {
            "request_id": request_id,
            "request_owner_epoch": "composer.controls.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": program_run_id,
            "expected_owner_epoch": owner_epoch,
            "expected_run_revision": expected_run_revision,
        }

    stage_request = RuntimeOverrideStageRequest.model_validate(
        {
            **request_base("controls.stage.1", 1),
            "node_instance_id": "node.motion.1",
            "node_attempt_id": "attempt.motion.1",
            "parameter_id": "goal",
            "value": {"position_m": [0.1, 0.2, 0.3]},
            "source_observation_id": None,
            "source_owner_generation": None,
        }
    )
    staged = runtime.stage_runtime_override(program_run_id, stage_request)
    assert runtime.stage_runtime_override(program_run_id, stage_request) == staged
    override = staged["result"]
    assert override["status"] == "staged"
    assert owner.rows[program_run_id].run_revision == 2

    activated = runtime.activate_runtime_override(
        program_run_id,
        RuntimeOverrideActionRequest.model_validate(
            {
                **request_base("controls.activate.1", 2),
                "override_id": override["override_id"],
                "expected_override_revision": 1,
            }
        ),
    )
    assert activated["result"]["status"] == "active"

    promoted = runtime.promote_runtime_override(
        program_run_id,
        RuntimeOverridePromotionRequest.model_validate(
            {
                **request_base("controls.promote.1", 3),
                "override_id": override["override_id"],
                "expected_override_revision": 2,
                "expected_draft_id": "draft.controls.1",
                "expected_draft_revision": 1,
            }
        ),
    )
    assert promoted["result"]["status"] == "awaiting_reset"

    cleared = runtime.clear_runtime_override(
        program_run_id,
        RuntimeOverrideActionRequest.model_validate(
            {
                **request_base("controls.clear.1", 4),
                "override_id": override["override_id"],
                "expected_override_revision": 2,
            }
        ),
    )
    assert cleared["result"]["status"] == "cleared"

    written = runtime.write_runtime_blackboard(
        program_run_id,
        RuntimeBlackboardWriteRequest.model_validate(
            {
                **request_base("controls.write.1", 5),
                "key": "operator.confirmed",
                "value": True,
            }
        ),
    )
    assert written["result"] == {
        "key": "operator.confirmed",
        "status": "written",
    }
    expected_controls = written["runtime_controls"]["runtime_controls"]
    assert expected_controls["runtime_control_revision"] == 6
    assert [kind for kind, _values in owner.effects] == [
        "activate",
        "clear",
        "write",
    ]

    runtime.shutdown()
    restored = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch=owner_epoch,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            journal_path,
            owner_epoch=owner_epoch,
        ),
    )
    restored._host.shutdown()  # noqa: SLF001
    restored._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    restored._host = _Host(  # type: ignore[assignment]  # noqa: SLF001
        owner,
        owner_id=restored.owner_id,
        owner_epoch=owner_epoch,
    )
    restored.start()

    assert (
        restored.read_runtime_controls(program_run_id)["runtime_controls"]
        == expected_controls
    )
    restored.shutdown()


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
        expected_states=("starting",),
        state="terminal_awaiting_reset",
        terminal_reference=_DIGEST_B,
    )
    old.close()

    replacement = ProgramCycleJournal(path, owner_epoch="runner.new")
    assert replacement.read().owner_epoch == "runner.old"
    assert replacement.read().reconciliation_required is True
    admission = _Admission()
    execution_run_root = tmp_path / "execution-run"
    execution_run_root.mkdir()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.new",
            execution_run_root=execution_run_root,
            cycle_journal_path=path,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=replacement,
    )
    runtime.start()
    assert runtime.health()["started"] is True
    status = runtime.cycle_status()
    assert status["state"] == "terminal_awaiting_reset"
    assert status["program_runner_owner_epoch"] == "runner.old"
    projected = runtime._current_cycle_status_projection()  # noqa: SLF001
    assert set(projected) == {
        "program_runner_owner_epoch",
        "cycle_generation",
        "state",
        "program_run_id",
        "cycle_claim_generation",
        "terminal_or_settlement_reference",
    }
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
        expected_states=("starting",),
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
    execution_run_root = tmp_path / "execution-run"
    execution_run_root.mkdir()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.new",
            execution_run_root=execution_run_root,
            cycle_journal_path=path,
            manager_liveness_socket=tmp_path / "runner.sock",
            cell_id="cell",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
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
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
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
    terminal_reference = _retain_no_effect_terminal(
        journal, program_run_id="program-run.1"
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
        execution_transport=_Headless(),  # type: ignore[arg-type]
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
            "terminal_reference": terminal_reference,
            "settlement_evidence_digest": terminal_reference,
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


def test_reset_opens_idle_then_converges_scene_and_exact_replay_finishes_it(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    events: list[str] = []

    class OrderedAdmission(_Admission):
        def release(self, release: dict[str, Any]) -> dict[str, Any]:
            result = super().release(release)
            events.append("release")
            return result

    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.reset-scene.1"
    )
    claim = _program_cycle_claim("runner.reset-scene.1")
    terminal_reference = _retain_terminal_cycle(journal, claim)
    admission = OrderedAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.reset-scene.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    published = journal.publish_reset

    def publish_reset(**kwargs: Any) -> None:
        published(**kwargs)
        assert journal.read().state == "idle_after_reset"
        events.append("idle")

    monkeypatch.setattr(journal, "publish_reset", publish_reset)
    contexts: list[str] = []

    def prepare_scene(
        *, expected_record_digest: str | None, context_id: str
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        assert expected_record_digest is None
        assert journal.read().state == "idle_after_reset"
        contexts.append(context_id)
        events.append(f"scene:{len(contexts)}")
        if len(contexts) == 1:
            raise RuntimeError("logical scene owner temporarily unavailable")
        return ({}, {})

    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", prepare_scene)
    request = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "reset.scene.1",
            "request_owner_epoch": "composer.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": 1,
            "expected_claim_generation": 1,
            "terminal_reference": terminal_reference,
            "settlement_evidence_digest": terminal_reference,
        }
    )
    runtime.start()

    with pytest.raises(ProgramRunnerError) as failed:
        runtime.reset_cycle(request)
    assert failed.value.code == "program_cycle_reset_scene_preparation_failed"
    assert events == ["release", "idle", "scene:1"]
    assert journal.read().state == "idle_after_reset"

    replay = runtime.reset_cycle(request)

    assert replay["idle_cycle_generation"] == 2
    assert events == ["release", "idle", "scene:1", "scene:2"]
    assert len(admission.releases) == 1
    assert contexts[0] == contexts[1]
    runtime.shutdown()


def test_rejected_reset_replays_after_state_change_and_retains_capacity(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
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
    terminal_reference = _retain_no_effect_terminal(
        journal, program_run_id="program-run.1"
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.current",
            maximum_replay_records=2,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
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
            "terminal_reference": terminal_reference,
            "settlement_evidence_digest": terminal_reference,
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
    _bypass_scene_preparation(monkeypatch)
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
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

    def delayed_stop(
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
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
        "program-cycle.1",
        ProgramRunStopRequest.model_validate(
            {
                "request_id": "request.stop.delayed",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
                "program_run_id": "program-cycle.1",
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
        "program-cycle.1",
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="stopped",
        revision=3,
    )
    owner.rows["program-cycle.1"] = terminal_snapshot
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
    _bypass_scene_preparation(monkeypatch)
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
        execution_transport=headless,  # type: ignore[arg-type]
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
    _bypass_scene_preparation(monkeypatch)
    admission = _Admission()
    journal = FailingJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    with pytest.raises(ProgramRunnerError) as failed:
        runtime.start_run(_start_request())
    assert failed.value.code == "program_cycle_reconciliation_required"
    assert len(admission.claims) == 1
    assert journal.read().reconciliation_required is True
    assert journal.read().state == "starting"
    assert journal.read().claim is None
    assert journal.read().acquisition_pending is not None
    runtime.shutdown()


def test_native_acquire_rejection_replays_exact_error_without_second_call(
    monkeypatch: pytest.MonkeyPatch,
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

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    admission = RejectingAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
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
        execution_transport=_Headless(),  # type: ignore[arg-type]
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

    def observed_stop(
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        reached.set()
        return original(
            program_run_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )

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
        execution_transport=headless,  # type: ignore[arg-type]
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


def test_node_availability_uses_supplied_exact_facts_without_headless_callback(
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
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="program-runner.test",
        ),
    )
    captured: dict[str, Any] = {}
    root = SimpleNamespace(node_id="root", children=())
    admitted = SimpleNamespace(
        applied_record_digest=_DIGEST_B,
        graph_revision=_DIGEST_A,
        program_binding_id=_DIGEST_B,
        retained_bundle_id=_DIGEST_A,
        scenario_id="cell-pick",
        readiness=object(),
        program=SimpleNamespace(
            program_id="program.1",
            program_revision=_DIGEST_A,
            tree=root,
        ),
    )

    def admit(applied: object, selection: object, **kwargs: object) -> object:
        captured.update(
            applied=applied,
            selection=selection,
            kwargs=kwargs,
        )
        return admitted

    availability = {
        "schema_version": "manyforge.node_execution_availability.v1",
        "environment_digest": runtime._environment.environment_digest,  # noqa: SLF001
        "program_binding_id": _DIGEST_B,
        "required_node_ids": ["root"],
        "portable_authoring_available": True,
        "execution_available": True,
        "reason_codes": [],
        "implementations": [],
    }
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        admit,
    )
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.project_node_execution_availability",
        lambda *_args, **_kwargs: availability,
    )
    request = ProgramNodeAvailabilityRequest.model_validate(
        {
            "applied": {
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": _DIGEST_A,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_A,
                "source": {
                    "record_digest": _DIGEST_B,
                    "source_generation": 7,
                },
            },
            "selection": {
                "scenario_id": "cell-pick",
                "program_revision": _DIGEST_A,
            },
        }
    )
    monkeypatch.setattr(
        runtime._kernel,  # noqa: SLF001
        "identity",
        lambda: {
            "ok": True,
            "identity": {
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_A,
                "active_publication_generation": 3,
                "scene_revision": {"revision_id": 2},
            },
        },
    )

    runtime.start()
    result = runtime.node_availability(request)

    assert captured["applied"] is request.applied
    assert captured["selection"] is request.selection
    assert captured["kwargs"] == {
        "expected_scenario_id": "cell-pick",
        "expected_scenario_catalog_revision": _DIGEST_A,
        "environment": runtime._environment,  # noqa: SLF001
        "owner_availability": runtime._owner_availability,  # noqa: SLF001
    }
    assert result["availability"] == availability
    assert result["source"]["program_revision"] == _DIGEST_A
    assert headless.applied_reads == 0
    assert headless.selection_reads == 0
    runtime.shutdown()


def test_node_availability_marks_only_native_ports_unavailable_without_owner(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
    )
    monkeypatch.setattr(
        runtime._kernel,  # noqa: SLF001
        "identity",
        lambda: (_ for _ in ()).throw(ConnectionError("owner absent")),
    )

    availability = runtime._current_node_owner_availability(  # noqa: SLF001
        {
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_A,
            "source": {"source_generation": 1},
        },
        admission_generation=1,
    )

    for port_name in (
        "gripper_status",
        "kernel_controls",
        "kernel_operations",
        "model_queries",
        "pick_place",
    ):
        assert availability[port_name] == AvailabilityFact(
            False,
            ("program_execution_owner_unavailable",),
        )
    assert availability["scene_transactions"] == AvailabilityFact(True)
    assert availability["cclc_lifecycle"] == AvailabilityFact(True)
    runtime.shutdown()


def test_runtime_injects_and_closes_the_focused_simulation_owner() -> None:
    simulation = _SimulationControl()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
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
        execution_transport=headless,  # type: ignore[arg-type]
        model_transport=model,  # type: ignore[arg-type]
        snapshot_source=snapshot,  # type: ignore[arg-type]
    )

    assert runtime._assistant is not None  # noqa: SLF001
    assert runtime.health()["assistant_snapshot_source_available"] is True
    runtime.shutdown()
    runtime.shutdown()
    assert snapshot.closed == 1
    assert model.closed == 1
