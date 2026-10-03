"""Production Program-runner admission and lifecycle glue."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from threading import Barrier, Event, Thread
from types import SimpleNamespace
from typing import Any, Mapping

import pytest

from manyforge_behavior.node_availability import AvailabilityFact
from manyforge_behavior.orchestration_ports import (
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.program_run_owner import (
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
    ProgramRunOwnerStaleRevisionError,
)
from manyforge_behavior.program_scheduling import ProgramSchedulingError
from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramManualHandoverRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
    ProgramSelectionStartRequest,
    RuntimeBlackboardWriteRequest,
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
from manyforge_program_runner.runtime_controls import ProgramRuntimeControls
from manyforge_program_runner.models import (
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
    ProgramSelectionStopRequest,
)
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot


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
        "scene_transaction_status",
        "cclc_component",
        "cclc_owner_identity",
        "cclc_route_templates",
        "component_lifecycle_status",
        "component_lifecycle_request_result",
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
        "route_selection_request_result",
    }

    def __init__(self) -> None:
        self.applied_reads = 0
        self.registered_captures: list[dict[str, Any]] = []
        self.cclc_reads: list[str] = []

    def program_applied(self) -> dict[str, Any]:
        self.applied_reads += 1
        return {
            "scenario_id": "cell-pick",
            "source_document_digest": _DIGEST_A,
            "program_revision": _DIGEST_A,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "program_binding": {
                "component_role_bindings": [
                    {"controllable_component_id": "robot.1"}
                ]
            },
            "source": {
                "record_digest": _DIGEST_B,
                "source_generation": 1,
            },
        }

    def cclc_component(self, component_id: str) -> dict[str, Any]:
        assert component_id == "robot.1"
        self.cclc_reads.append(component_id)
        return {
            "controllable_component_id": component_id,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "component_lifecycle_state": "active",
        }

    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "owner_epoch": "execution.epoch.1",
                "program_binding_id": None,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "active_publication_generation": 1,
                "scene_revision": {"revision_id": 1},
            },
        }

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.registered_captures.append(deepcopy(dict(body)))
        return {
            "ok": True,
            "command": "register_program_run_capture",
            "program_run_id": body["program_run_id"],
            "claim_generation": body["claim_generation"],
        }

    def stop_fault(self) -> dict[str, Any]:
        """Report the normal no-latched-fault state for Reset tests."""

        return {"ok": True, "stop_fault": None}

    def __getattr__(self, name: str):
        if name not in self._PORT_METHODS:
            raise AttributeError(name)

        def unexpected(*_args: object, **_kwargs: object) -> None:
            raise AssertionError(f"unexpected effectful headless call: {name}")

        return unexpected


def test_program_component_readiness_uses_exact_cclc_state() -> None:
    class ComponentHeadless:
        def __init__(self) -> None:
            self.state = "disabled"
            self.graph = _DIGEST_B
            self.reads: list[str] = []

        def cclc_component(self, component_id: str) -> dict[str, Any]:
            self.reads.append(component_id)
            return {
                "controllable_component_id": component_id,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": self.graph,
                "component_lifecycle_state": self.state,
            }

    headless = ComponentHeadless()
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]
    applied = {
        "retained_bundle_id": _DIGEST_A,
        "graph_revision": _DIGEST_B,
        "program_binding": {
            "component_role_bindings": [
                {"controllable_component_id": "robot.1"},
                {"controllable_component_id": "robot.1"},
            ]
        },
    }
    with pytest.raises(ProgramRunnerError) as disabled:
        runtime._verify_required_components_active(applied)
    assert disabled.value.code == "program_required_component_not_active"
    assert "robot.1" in str(disabled.value)
    assert headless.reads == ["robot.1"]

    headless.state = "active"
    headless.graph = _DIGEST_A
    with pytest.raises(ProgramRunnerError) as stale:
        runtime._verify_required_components_active(applied)
    assert stale.value.code == "program_required_component_stale"

    headless.graph = _DIGEST_B
    runtime._verify_required_components_active(applied)
    assert headless.reads == ["robot.1", "robot.1", "robot.1"]


def test_program_component_readiness_does_not_block_an_unbound_peer() -> None:
    class Components:
        def cclc_component(self, component_id: str) -> dict[str, Any]:
            assert component_id == "robot.enabled"
            return {
                "controllable_component_id": component_id,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "component_lifecycle_state": "active",
            }

    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = Components()  # type: ignore[assignment]
    runtime._verify_required_components_active(
        {
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "program_binding": {
                "component_role_bindings": [
                    {"controllable_component_id": "robot.enabled"}
                ]
            },
        }
    )


def test_disabled_required_component_rejects_program_before_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    class DisabledHeadless(_Headless):
        def cclc_component(self, component_id: str) -> dict[str, Any]:
            return {
                **super().cclc_component(component_id),
                "component_lifecycle_state": "disabled",
            }

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    headless = DisabledHeadless()
    admission = _Admission()
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.1"),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    with pytest.raises(ProgramRunnerError) as refused:
        runtime.start_run(_start_request())
    assert refused.value.code == "program_required_component_not_active"
    assert admission.claims == []
    assert journal.read().state == "idle_after_reset"
    runtime.shutdown()


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
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": program_run_id,
            "applied_record_digest": _DIGEST_B,
            "scenario_id": "scenario.1",
            "source_document_digest": _DIGEST_A,
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
            node_policy_by_instance_id={},
            to_dict=lambda: deepcopy(capture),
        )
        self._value = {
            "schema_version": "manyforge.program_run_status.v3",
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
            "details": None,
            "origin": None,
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

    def failure_pause_stop_pending(self, _program_run_id: str) -> bool:
        return False

    def read(self, program_run_id: str) -> _Snapshot:
        self.reads += 1
        return self.rows[program_run_id]

    def intervention(self, _program_run_id: str):
        return None

    def take_diagnostic(self, _program_run_id: str):
        return None

    def operator_stop_scope(self, _program_run_id: str):
        return OperatorMotionStopSnapshot()

    def read_selection_controls(self, program_run_id: str):
        return self.read(program_run_id), self.execution_selection_controls(
            program_run_id
        )

    def execution_selection_controls(self, program_run_id: str) -> dict[str, Any]:
        running = self.rows[program_run_id].state == "running"
        return {
            "selection_revision": 0,
            "groups": [
                {
                    "group_id": "all",
                    "node_count": 1,
                    "held_node_count": 0,
                    "state": "running" if running else "unavailable",
                    "stop_allowed": running,
                    "resume_allowed": False,
                    "resume_unavailable_reason": (
                        "selection_not_held" if running else "program_run_not_running"
                    ),
                }
            ],
        }


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

    def note_runtime_control_change(self, program_run_id: str) -> _Snapshot:
        return self._advance(program_run_id)

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
        self.scheduled: list[str] = []
        self.stop_calls: list[tuple[str, str | None]] = []
        self.motion_stop_calls: list[tuple[str, str | None]] = []
        self.motion_start_calls: list[tuple[str, object, int, int, str]] = []
        self.full_stop_calls: list[tuple[str, str | None]] = []
        self.full_stop_resume_calls: list[tuple[str, str | None]] = []
        self.reset_calls: list[str] = []
        self.reset_containment_calls: list[str] = []
        self.retained_reset_calls: list[str] = []
        self.forgotten_reset_runs: list[str] = []
        self.shutdowns = 0

    def start(self) -> None:
        self.started += 1

    def start_run(
        self,
        admission: object,
        *,
        program_run_id: str | None = None,
        defer_ticks: bool = False,
    ) -> _Snapshot:
        assert defer_ticks
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

    def schedule(self, program_run_id: str) -> None:
        self.scheduled.append(program_run_id)

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        self.stop_calls.append((program_run_id, reason))
        current = self.owner.rows.get(program_run_id)
        if current is not None and current.state in {"stopped", "succeeded", "failed"}:
            return current
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="stopped",
            revision=1 if current is None else current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def wait_terminal(
        self,
        program_run_id: str,
        *,
        timeout_s: float = 5.0,
    ) -> _Snapshot:
        del timeout_s
        return self.owner.rows[program_run_id]

    def contain_reset_run(self, program_run_id, *, stop_request_id, timeout_s):
        assert stop_request_id and timeout_s > 0
        self.reset_containment_calls.append(program_run_id)
        return self.owner.rows[program_run_id]

    def stop_motion(
        self,
        program_run_id: str,
        *,
        group_id: str = "all",
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

    def start_motion(
        self,
        program_run_id: str,
        *,
        expected_run_revision: int,
        expected_selection_revision: int,
        request_id: str,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> object:
        del reason
        self.motion_start_calls.append(
            (
                program_run_id,
                group_id,
                expected_run_revision,
                expected_selection_revision,
                request_id,
            )
        )
        current = self.owner.rows[program_run_id]
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return SimpleNamespace(
            state=SimpleNamespace(value="running"),
            run_revision=snapshot.run_revision,
        )

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

    def forget_reset_run(self, program_run_id: str) -> None:
        self.forgotten_reset_runs.append(program_run_id)
        self.owner.rows.pop(program_run_id, None)

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


def test_diagnostic_join_failure_still_closes_transport_and_releases_worker(
    monkeypatch, tmp_path
):
    from manyforge_program_runner.diagnostics import ProgramDiagnosticPublisher

    runtime, _journal, _admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    entered, closed = Event(), Event()

    def send(_record):
        entered.set()
        assert closed.wait(timeout=2)
        raise OSError("transport closed during pending delivery")

    publisher = ProgramDiagnosticPublisher(send)
    runtime._diagnostics = publisher
    runtime._diagnostic_client = SimpleNamespace(close=closed.set)
    publisher.submit({"state": "succeeded"})
    assert entered.wait(timeout=2)
    thread = publisher._thread
    join = thread.join
    # Explicitly hit an expired join while delivery is held; no wall-clock
    # two-second wait is needed to qualify the cleanup failure branch.
    monkeypatch.setattr(thread, "join", lambda **_kwargs: None)
    try:
        with pytest.raises(ProgramRunnerError, match="shutdown bound"):
            runtime.shutdown()
        assert closed.is_set()
    finally:
        closed.set()
        join(timeout=2)
    assert not thread.is_alive()
    assert publisher.health()["delivery_failed"]


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


def _retain_terminal_cycle(journal: ProgramCycleJournal, claim: dict[str, Any]) -> str:
    settlement = {
        "schema_version": "manyforge.program_cycle_start_settlement.v1",
        "kind": "proven_no_effect",
        "claim": claim,
        "program_run_id": "program-cycle.1",
        "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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
    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001
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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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


def test_manager_recovery_resumes_contained_reset_after_runner_restart(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
    path = tmp_path / "cycle.sqlite3"
    old = ProgramCycleJournal(path, owner_epoch="runner.old")
    claim = _program_cycle_claim("runner.old")
    reference = _retain_terminal_cycle(old, claim)
    old.begin_resetting(
        expected_generation=1,
        program_run_id="program-cycle.1",
        correlation_id="reset.before-restart",
    )
    old.update_reset_phase(
        expected_generation=1,
        program_run_id="program-cycle.1",
        expected_phase="stopping",
        phase="settling",
        terminal_reference=reference,
    )
    old.update_reset_phase(
        expected_generation=1,
        program_run_id="program-cycle.1",
        expected_phase="settling",
        phase="restoring_scene",
        terminal_reference=reference,
    )
    old.close()

    journal = ProgramCycleJournal(path, owner_epoch="runner.new")
    admission = _Admission()
    admission.inventory = {
        "status": "held_exact",
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [claim],
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.old"}
        ],
        "matched_claim": claim,
        "matched_release": None,
        "matched_reconcile_settlement": None,
    }
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.new",
            execution_run_root=tmp_path / "execution-run",
            cycle_journal_path=path,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001

    runtime._reconcile_manager_liveness(  # noqa: SLF001
        _DIGEST_A,
        "manager.1",
        "manager-owner.1",
    )

    recovered = journal.read()
    assert recovered.state == "recovery_required"
    assert recovered.owner_epoch == "runner.new"
    assert recovered.reset_phase == "restoring_scene"
    assert recovered.reconciliation_required is False
    assert len(admission.reconciliations) == 1

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
            claim,
            "runner.new",
        ),
    }
    runtime._started = True  # noqa: SLF001
    result = runtime.reset_cycle(
        ProgramCycleResetRequest.model_validate(
            {
                "request_id": "reset.after-restart",
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

    assert result["idle_cycle_generation"] == 2
    assert admission.releases == []
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
    reconciliation_digest = cell_admission_owner_epoch_reconciliation_evidence_digest(
        "program_runner", "runner.new"
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

    admission.inventory["matched_reconcile_settlement"] = _program_reconcile_settlement(
        claim, "runner.new"
    )
    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001
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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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

    runtime._reconcile_manager_liveness(
        _DIGEST_A, "manager.1", "manager-owner.1"
    )  # noqa: SLF001

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
            "source_document_digest": _DIGEST_A,
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
        "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
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
        lambda _self, _admission, _run_id, **_kwargs: (
            SimpleNamespace(
                program_id="cell-pick",
                program_revision=_DIGEST_A,
                program_binding_id=_DIGEST_B,
                retained_bundle_id=_DIGEST_A,
                graph_revision=_DIGEST_B,
            ),
            (),
            deepcopy(proof),
        ),
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
        lambda _self, _request, _authorization: None,
    )
    monkeypatch.setattr(
        ProgramRuntime,
        "_clear_reset_stop_fault",
        lambda _self, _request: None,
    )


class _PreparedRunAdmission:
    def __init__(self) -> None:
        self.program = SimpleNamespace(to_dict=lambda: {"scene": []})

    def port_context(self, program_run_id: str) -> object:
        return SimpleNamespace(
            program_run_id=program_run_id,
            program_id="cell-pick",
            program_revision=_DIGEST_A,
            program_binding_id=_DIGEST_B,
            retained_bundle_id=_DIGEST_A,
            graph_revision=_DIGEST_B,
        )


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


@pytest.mark.parametrize("delayed_stop", [False, True])
def test_starting_stop_reaches_native_while_capture_registration_is_held(
    monkeypatch: pytest.MonkeyPatch, tmp_path, delayed_stop: bool
) -> None:
    entered, release = Event(), Event()
    stop_entered, release_stop = Event(), Event()
    stops: list[dict[str, Any]] = []

    class HeldRegistration(_Headless):
        def register_program_run_capture(self, body):
            entered.set()
            assert release.wait(5), "registration gate was not released"
            return super().register_program_run_capture(body)

        def stop_program_run(self, body):
            stops.append(deepcopy(body))
            assert body["request_context"]["request_owner_epoch"] == "execution.epoch.1"
            return {
                "ok": True,
                "owner_epoch": "execution.epoch.1",
                "accepted": {
                    "request_id": body["request_context"]["request_id"],
                    "stop_id": "stop.1",
                    "stop_generation": len(stops),
                    "replay_status": "accepted_new",
                },
            }

    def ordinary_read_forbidden(**_kwargs):
        raise AssertionError("Stop attempted an ordinary owner read")

    headless, admission = HeldRegistration(), _Admission()
    monkeypatch.setattr(admission, "recovery_inventory", ordinary_read_forbidden)
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch="runner.1"
        ),
        headless_client=headless,
        execution_transport=headless,
        admission_client=admission,
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="runner.1"
        ),
    )
    runtime._host.shutdown()
    host = _Host(_Owner(), owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._host = host
    runtime.start()
    failures = []
    stop_results, stop_errors = [], []
    request_stop = runtime._native.scope.request_stop

    def held_stop(program_run_id, stop_request_id, reason):
        if delayed_stop and stop_request_id.startswith("execution:"):
            stop_entered.set()
            assert release_stop.wait(5), "Stop gate was not released"
        return request_stop(program_run_id, stop_request_id, reason)

    monkeypatch.setattr(runtime._native.scope, "request_stop", held_stop)

    def start():
        try:
            runtime.start_run(_start_request())
        except Exception as error:
            failures.append(error)

    starter = Thread(target=start)
    stopper = None
    starter.start()
    try:
        assert entered.wait(5), "Start did not reach the registration gate"

        request = ProgramRunStopRequest.model_validate(
            {
                "request_id": "stop.during-registration",
                "request_owner_epoch": "composer.1",
                "source_label": "operator",
                "correlation_id": None,
                "program_run_id": "program-cycle.1",
                "expected_owner_epoch": "runner.1",
                "reason": "stop during capture",
            }
        )

        def stop():
            try:
                stop_results.append(runtime.stop_run("program-cycle.1", request))
            except Exception as error:
                stop_errors.append(error)

        if delayed_stop:
            stopper = Thread(target=stop)
            stopper.start()
            assert stop_entered.wait(5), "Stop did not commit Stopping"
            release.set()
            starter.join(5)
            assert not starter.is_alive()
            assert len(stops) == 1
            assert runtime._starting_kernel_owner_epoch is None
            release_stop.set()
            stopper.join(5)
            assert not stopper.is_alive()
            assert len(stops) == 2
        else:
            stop()
            assert len(stops) == 1 and not release.is_set()
        assert not stop_errors
        assert stop_results[0]["state"] == "stopping"
        assert host.start_admissions == []
    finally:
        release.set()
        release_stop.set()
        starter.join(5)
        if stopper is not None:
            stopper.join(5)
        runtime.shutdown()
    assert not starter.is_alive()
    assert len(failures) == 1
    assert isinstance(failures[0], ProgramRunnerError)
    assert failures[0].code == "program_run_superseded_by_stop"
    assert runtime._starting_kernel_owner_epoch is None


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
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001

    def prepare(_context: object, _scene: object) -> dict[str, Any]:
        events.append("prepare")
        admission.generation = 3
        return {"schema_version": "manyforge.prepared_program_scene.v1"}

    monkeypatch.setattr(
        runtime._scene, "prepare_initial_scene", prepare
    )  # noqa: SLF001
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
    assert (
        status["runtime_controls"]["program_run_id"] == run["capture"]["program_run_id"]
    )
    assert status["runtime_controls"]["run_revision"] == status["run_revision"]
    assert (
        status["runtime_controls"]["runtime_controls"]["runtime_control_revision"]
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


def test_stop_overtakes_start_before_claim_without_launching_tree(
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
            headless_url="http://headless.test",
            owner_epoch="runner.start-stop-race.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="runner.start-stop-race.1",
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    preparation_entered = Event()
    continue_preparation = Event()

    def prepare_scene(*_args: object) -> tuple[str, dict[str, Any], dict[str, Any]]:
        preparation_entered.set()
        assert continue_preparation.wait(timeout=1.0)
        return (
            "program-cycle.1",
            {},
            {"schema_version": "manyforge.prepared_program_scene.v1"},
        )

    monkeypatch.setattr(runtime, "_prepare_run_scene", prepare_scene)
    monkeypatch.setattr(
        runtime._scene, "release", lambda _context_id: None
    )  # noqa: SLF001
    runtime.start()
    start_outcome: list[Exception] = []

    def start() -> None:
        try:
            runtime.start_run(_start_request())
        except Exception as error:  # test captures the exact terminal result
            start_outcome.append(error)

    thread = Thread(target=start)
    thread.start()
    assert preparation_entered.wait(timeout=1.0)
    starting = runtime.cycle_status()
    assert starting["state"] == "starting"
    availability = {
        command["command_kind"]: command for command in starting["allowed_commands"]
    }
    assert availability["program_stop"]["allowed"] is True
    assert availability["program_motion_stop"]["allowed"] is False
    assert availability["program_full_stop"]["allowed"] is False
    stop = ProgramRunStopRequest.model_validate(
        {
            "request_id": "request.stop.start-race",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": "request.stop.start-race",
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "reason": "operator stop overtook start",
        }
    )
    acceptance = runtime.stop_run("program-cycle.1", stop)
    assert acceptance["state"] == "stopping"
    assert runtime.cycle_status()["state"] == "stopping"
    continue_preparation.set()
    thread.join(timeout=1.0)

    assert not thread.is_alive()
    assert len(start_outcome) == 1
    assert isinstance(start_outcome[0], ProgramRunnerError)
    assert start_outcome[0].code == "program_run_superseded_by_stop"
    assert host.start_admissions == []
    assert admission.claims == []
    terminal = runtime.cycle_status()
    assert terminal["state"] == "terminal_awaiting_reset"
    assert terminal["cycle_claim_generation"] is None

    _bypass_reset_scene_preparation(monkeypatch)
    reset = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "request.reset.start-race",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": "request.reset.start-race",
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": 1,
            "expected_claim_generation": None,
            "terminal_reference": terminal["terminal_or_settlement_reference"],
            "settlement_evidence_digest": terminal["terminal_or_settlement_reference"],
        }
    )
    assert runtime.reset_cycle(reset)["idle_cycle_generation"] == 2
    assert admission.releases == []
    assert runtime.cycle_status()["state"] == "idle_after_reset"
    runtime.shutdown()


@pytest.mark.parametrize("completion_wins_before_stop", [False, True])
def test_reset_publishes_resetting_until_motion_is_terminal(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    completion_wins_before_stop,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
    admission = _Admission()
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", owner_epoch="runner.resetting.1"
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.resetting.1",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime._clear_reset_stop_fault = lambda _request: None  # type: ignore[method-assign]  # noqa: SLF001
    runtime.start()
    runtime.start_run(_start_request())

    settlement_wait_entered = Event()
    permit_settlement = Event()

    def begin_stop(
        program_run_id: str,
        *,
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        from manyforge_behavior.program_run_owner import PROGRAM_RESET_STOP_REASON

        assert reason == PROGRAM_RESET_STOP_REASON
        assert stop_request_id == "program-reset:composer.epoch.1:request.reset.running"
        current = owner.rows[program_run_id]
        stopping = _Snapshot(
            program_run_id,
            owner_id=runtime.owner_id,
            owner_epoch=runtime.owner_epoch,
            state="succeeded" if completion_wins_before_stop else "stopping",
            revision=current.run_revision + 1,
        )
        owner.rows[program_run_id] = stopping
        return stopping

    def wait_terminal(
        program_run_id: str,
        *,
        timeout_s: float = 5.0,
    ) -> _Snapshot:
        del timeout_s
        settlement_wait_entered.set()
        assert permit_settlement.wait(timeout=1.0)
        stopped = _Snapshot(
            program_run_id,
            owner_id=runtime.owner_id,
            owner_epoch=runtime.owner_epoch,
            state="stopped",
            revision=owner.rows[program_run_id].run_revision + 1,
        )
        owner.rows[program_run_id] = stopped
        return stopped

    host.stop_run = begin_stop  # type: ignore[method-assign]
    host.wait_terminal = wait_terminal  # type: ignore[method-assign]

    def contain_terminal(program_run_id, *, stop_request_id, timeout_s):
        assert stop_request_id and timeout_s > 0
        host.reset_containment_calls.append(program_run_id)
        assert journal.read().reset_phase == "stopping"
        if completion_wins_before_stop:
            settlement_wait_entered.set()
            assert permit_settlement.wait(timeout=1.0)
        return owner.rows[program_run_id]

    host.contain_reset_run = contain_terminal
    cycle = runtime.cycle_status()
    request = ProgramCycleResetRequest.model_validate(
        {
            "request_id": "request.reset.running",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": "request.reset.running",
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_cycle_generation": cycle["cycle_generation"],
            "expected_claim_generation": cycle["cycle_claim_generation"],
            "terminal_reference": None,
            "settlement_evidence_digest": None,
        }
    )
    outcomes: list[dict[str, Any] | Exception] = []

    def reset() -> None:
        try:
            outcomes.append(runtime.reset_cycle(request))
        except Exception as error:
            outcomes.append(error)

    thread = Thread(target=reset)
    thread.start()
    assert settlement_wait_entered.wait(timeout=1.0)
    resetting = runtime.cycle_status()
    assert resetting["state"] == "resetting"
    assert resetting["reset_phase"] == "stopping"
    availability = {row["command_kind"]: row for row in resetting["allowed_commands"]}
    assert availability["program_run"]["allowed"] is False
    assert availability["program_stop"]["allowed"] is False
    assert availability["program_reset"]["allowed"] is False
    assert host.reset_calls == [] and admission.releases == []

    permit_settlement.set()
    thread.join(timeout=1.0)
    assert not thread.is_alive()
    assert len(outcomes) == 1 and isinstance(outcomes[0], dict)
    assert outcomes[0]["idle_cycle_generation"] == 2
    assert host.reset_containment_calls == ["program-cycle.1"]
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
        lambda *_args, **_kwargs: {
            "schema_version": "manyforge.prepared_program_scene.v1"
        },
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
            "settlement_evidence_digest": status["terminal_or_settlement_reference"],
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
                    value["capture"]["program_run_id"] for value in all_runs["runs"]
                } == {
                    "run.history",
                    "run.live",
                    "run.resident-terminal",
                }
                assert (
                    runtime.runs(active_only=True, limit=10)["runs"][0]["capture"][
                        "program_run_id"
                    ]
                    == "run.live"
                )
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


def test_repeated_run_is_state_rejected_and_keeps_one_active_product_run(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    headless = _Headless()
    admitted = object()
    joins: list[dict[str, Any]] = []

    def join(applied: dict[str, Any], **_kwargs: object):
        joins.append(deepcopy(applied))
        return admitted

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        join,
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
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
    register = headless.register_program_run_capture

    def register_after_claim(body: Mapping[str, Any]) -> dict[str, Any]:
        assert host.start_admissions == []
        cycle = runtime._journal.read()
        assert cycle.state == "starting" and cycle.claim is not None
        assert body["claim_generation"] == cycle.claim["claim_generation"]
        assert body["program_runner_owner_epoch"] == runtime.owner_epoch
        assert body["program_run_id"] == cycle.program_run_id
        return register(body)

    monkeypatch.setattr(headless, "register_program_run_capture", register_after_claim)
    schedule = host.schedule

    def schedule_after_running(program_run_id: str) -> None:
        cycle = runtime._journal.read()
        assert cycle.state == "running" and cycle.program_run_id == program_run_id
        schedule(program_run_id)

    monkeypatch.setattr(host, "schedule", schedule_after_running)
    started = runtime.start_run(request)
    with pytest.raises(ProgramRunnerError) as repeated:
        runtime.start_run(request)

    assert repeated.value.code == "program_cycle_reset_required"
    assert started["capture"]["program_run_id"] == "program-cycle.1"
    assert host.started == 1
    assert host.start_admissions == [admitted]
    assert host.scheduled == [started["capture"]["program_run_id"]]
    assert headless.applied_reads == 2
    assert headless.cclc_reads == ["robot.1", "robot.1"]
    assert len(joins) == 1
    assert len(headless.registered_captures) == 1
    assert "table" not in headless.registered_captures[0]
    monkeypatch.setattr(headless, "register_program_run_capture", register)

    with pytest.raises(ProgramRunnerError) as active:
        runtime.start_run(_start_request(request_id="request.start.2"))
    assert active.value.code == "program_cycle_reset_required"
    assert headless.applied_reads == 2
    assert runtime.cycle_status()["state"] == "running"

    with pytest.raises(ProgramRunnerError) as changed_repeated_intent:
        runtime.start_run(
            ProgramRunStartRequest.model_validate(
                {
                    **request.model_dump(mode="json"),
                    "source_document_digest": "sha256:" + "c" * 64,
                }
            )
        )
    assert changed_repeated_intent.value.code == "program_cycle_reset_required"

    motion_start = ProgramSelectionStartRequest.model_validate(
        {
            "request_id": "request.motion-start.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "expected_run_revision": started["run_revision"],
            "expected_selection_revision": 0,
            "group_ids": ["robot_group"],
            "reason": "initial operator motion Run",
        }
    )
    motion_started = runtime.start_motion("program-cycle.1", motion_start)
    assert motion_started["state"] == "running"
    assert host.motion_start_calls == [
        ("program-cycle.1", ["robot_group"], 1, 0, "request.motion-start.1")
    ]

    motion_stop = ProgramSelectionStopRequest.model_validate(
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

    original_motion_stop = host.stop_motion
    stopping_statuses: list[dict[str, Any]] = []

    def observe_motion_stopping(
        program_run_id: str,
        *,
        group_id: str = "all",
        reason: str | None,
        stop_request_id: str | None = None,
    ) -> _Snapshot:
        stopping_statuses.append(runtime.cycle_status())
        return original_motion_stop(
            program_run_id,
            group_id=group_id,
            reason=reason,
            stop_request_id=stop_request_id,
        )

    host.stop_motion = observe_motion_stopping  # type: ignore[method-assign]
    cycle_journal.retain_run_snapshot = retain_on_stop_path  # type: ignore[method-assign]
    motion_stopped = runtime.stop_motion("program-cycle.1", motion_stop)
    cycle_journal.retain_run_snapshot = original_retain  # type: ignore[method-assign]
    host.stop_motion = original_motion_stop  # type: ignore[method-assign]
    assert motion_stopped["state"] == "running"
    assert stop_path_retention == []
    assert len(stopping_statuses) == 1
    assert stopping_statuses[0]["state"] == "running"
    stopping_commands = {
        row["command_kind"]: row for row in stopping_statuses[0]["allowed_commands"]
    }
    assert stopping_commands["program_motion_stop"]["allowed"] is True
    assert stopping_commands["program_reset"]["allowed"] is True
    assert host.motion_stop_calls == [
        ("program-cycle.1", "ordinary operator motion Stop")
    ]
    motion_stopped_cycle = runtime.cycle_status()
    assert motion_stopped_cycle["state"] == "running"

    settled_motion_stop = _Snapshot(
        "program-cycle.1",
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="intervention_required",
        revision=motion_stopped["run_revision"] + 1,
    )
    owner.rows["program-cycle.1"] = settled_motion_stop
    runtime._observe_run_state(settled_motion_stop)  # noqa: SLF001
    motion_stopped_cycle = runtime.cycle_status()
    assert motion_stopped_cycle["state"] == "running"
    assert (
        next(
            row
            for row in motion_stopped_cycle["allowed_commands"]
            if row["command_kind"] == "program_manual_handover"
        )["allowed"]
        is True
    )

    full_stop = ProgramRunStopRequest.model_validate(
        {
            **motion_stop.model_dump(mode="json", exclude={"group_ids"}),
            "request_id": "request.full-stop.1",
            "reason": "ordinary operator Full Stop",
        }
    )
    full_stopped = runtime.full_stop("program-cycle.1", full_stop)
    assert full_stopped["state"] == "full_stopped"
    assert host.full_stop_calls == [("program-cycle.1", "ordinary operator Full Stop")]
    full_stopped_status = runtime.cycle_status()
    assert full_stopped_status["state"] == "running"
    full_stopped_commands = {
        row["command_kind"]: row for row in full_stopped_status["allowed_commands"]
    }
    assert full_stopped_commands["program_resume_full_stop"]["allowed"] is True
    assert full_stopped_commands["program_discard_full_stop"]["allowed"] is True
    assert full_stopped_commands["program_motion_stop"]["allowed"] is False
    assert full_stopped_commands["program_manual_handover"]["allowed"] is False

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
    resumed = runtime.resume_full_stop("program-cycle.1", resume_full_stop)
    assert resumed["state"] == "running"
    assert host.full_stop_resume_calls == [
        ("program-cycle.1", "operator Run after Full Stop")
    ]
    resumed_status = runtime.cycle_status()
    assert resumed_status["state"] == "running"
    resumed_commands = {
        row["command_kind"]: row for row in resumed_status["allowed_commands"]
    }
    assert resumed_commands["program_resume_full_stop"]["allowed"] is False
    assert resumed_commands["program_discard_full_stop"]["allowed"] is False
    assert resumed_commands["program_motion_stop"]["allowed"] is True

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
    stopped_again = runtime.stop_run("program-cycle.1", stop)
    assert stopped["state"] == "stopped"
    assert stopped_again["state"] == "terminal_awaiting_reset"
    assert host.stop_calls == [("program-cycle.1", "operator stop")]
    assert runtime.cycle_status()["cycle_claim_generation"] == 1
    resident_terminal = owner.rows["program-cycle.1"]
    owner.rows.clear()
    with pytest.raises(ProgramRunnerError) as terminal:
        runtime.start_run(_start_request(request_id="request.start.terminal"))
    assert terminal.value.code == "program_cycle_reset_required"
    # Historical status alone still blocks Run. Actual Reset containment uses
    # the terminal resident Run; its absence is covered by the recovery tests.
    owner.rows["program-cycle.1"] = resident_terminal

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
            "terminal_reference": status["terminal_or_settlement_reference"],
            "settlement_evidence_digest": status["terminal_or_settlement_reference"],
        }
    )
    reset_result = runtime.reset_cycle(reset)
    with pytest.raises(ProgramRunnerError) as completed_reset:
        runtime.reset_cycle(reset)
    assert completed_reset.value.code == "program_cycle_reset_stale"
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
    second_full_stopped = runtime.full_stop("program-cycle.2", second_full_stop)
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
            "expected_claim_generation": current_cycle["cycle_claim_generation"],
            "reason": "operator Reset discarded Full Stop",
        }
    )
    discarded = runtime.discard_full_stop("program-cycle.2", discard)
    with pytest.raises(ProgramRunnerError) as repeated_discard:
        runtime.discard_full_stop("program-cycle.2", discard)
    assert repeated_discard.value.code == "program_full_stop_reset_stale"
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
            "expected_claim_generation": terminal_cycle["cycle_claim_generation"],
            "terminal_reference": terminal_cycle["terminal_or_settlement_reference"],
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


@pytest.mark.parametrize(
    "case", ["held", "unused", "stale", "lost_query", "incomplete", "unknown"]
)
def test_manual_handover_requeries_kernel_and_retains_cleanup_outcome(
    monkeypatch, tmp_path, case
):
    from manyforge_behavior.program_run_owner import ProgramRunState

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, journal, _admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    run_id = "program-cycle.1"

    class ManualSnapshot(_Snapshot):
        @property
        def state(self):
            return ProgramRunState(super().state)

    owner.rows[run_id] = ManualSnapshot(
        run_id,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="running",
        revision=owner.read(run_id).run_revision,
    )
    context = {
        "program_runner_owner_epoch": runtime.owner_epoch,
        "program_run_id": run_id,
        "claim_generation": journal.read().claim["claim_generation"],
        "selection_revision": 0 if case == "unused" else 3,
    }
    request = ProgramManualHandoverRequest.model_validate(
        {
            "request_id": "manual.cleanup",
            "request_owner_epoch": "caller.1",
            "source_label": "test",
            "expected_owner_epoch": runtime.owner_epoch,
            "program_run_id": run_id,
            "expected_run_revision": owner.read(run_id).run_revision,
            "operation": {
                "request_context": {
                    "request_id": "manual.1",
                    "request_owner_epoch": "kernel.1",
                    "source_label": "test",
                    "correlation_id": None,
                },
                "program_id": "direct",
                "program_revision": _DIGEST_A,
                "program_binding_id": _DIGEST_B,
                "program_run_id": "manual.1",
                "expected_graph_revision": _DIGEST_A,
                "expected_retained_bundle_id": _DIGEST_B,
                "expected_scene_revision": None,
                "coordination_policy": "independent",
                "manual_handover": context,
                "participants": [
                    {
                        "component_role_id": "gripper",
                        "target_role_id": "gripper",
                        "required_capability_id": "tool.gripper.command",
                        "action_span": None,
                        "payload": {
                            "kind": "capability_command",
                            "command_schema_id": "gripper.command_position",
                            "completion_timeout_s": 2.0,
                            "caller_options": [],
                            "command_schema_revision": _DIGEST_A,
                            "command_value": {
                                "kind": "gripper_position",
                                "position": {"kind": "linear_m", "value_m": 0.0},
                                "max_effort": None,
                            },
                        },
                    }
                ],
            },
        }
    )
    calls = []

    def query(operation):
        calls.append("query")
        assert operation == request.operation.model_dump(mode="json")
        if case == "lost_query":
            raise OSError("query response lost")
        return {
            "context": (
                {**context, "selection_revision": 4} if case == "stale" else context
            ),
            "producer_node_indices": [] if case == "unused" else [0],
        }

    def cleanup(actual_run, **kwargs):
        calls.append("cleanup")
        assert actual_run == run_id and kwargs["node_indices"] == (0,)
        assert kwargs["expected_selection_revision"] == 3
        if case == "incomplete":
            raise ProgramRunOwnerRecoveryIncompleteError(
                "first task retired; second rejected"
            )
        if case == "unknown":
            raise ProgramRunOwnerOutcomeUnknownError("cleanup outcome unknown")
        return owner.read(run_id)

    runtime._kernel.read_manual_handover = query
    host.prepare_manual_handover = cleanup
    try:
        if case in {"held", "unused"}:
            result = runtime.prepare_manual_handover(run_id, request)
            assert result["state"] == "running"
        else:
            with pytest.raises(ProgramRunnerError) as rejected:
                runtime.prepare_manual_handover(run_id, request)
            assert (
                rejected.value.code
                == {
                    "stale": "manual_handover_scope_stale",
                    "lost_query": "manual_handover_query_unavailable",
                    "incomplete": "manual_handover_cleanup_incomplete",
                    "unknown": "program_cycle_reconciliation_required",
                }[case]
            )
        assert calls == (
            ["query"]
            if case in {"unused", "stale", "lost_query"}
            else ["query", "cleanup"]
        )
        assert journal.read().reconciliation_required == (case == "unknown")
        assert bool(journal.pending_commands()) == (case == "unknown")
        if case == "unknown":
            cycle = journal.read()
            # A later explicit containment can use the existing recovery Reset;
            # it must not replay the uncertain cleanup or release Program holds.
            recovery = replace(cycle, state="recovery_required")
            assert runtime._current_cycle_recovery_reset(recovery)
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("case", ["unselected", "incomplete", "unknown", "stale"])
def test_default_resume_routes_only_selected_nodes_and_preserves_partial_outcomes(
    monkeypatch,
    tmp_path,
    case,
):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, journal, _admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
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
    owner.intervention = lambda _run: intervention
    owner.operator_stop_scope = lambda _run: OperatorMotionStopSnapshot(
        4, frozenset({"other" if case == "unselected" else "move.1"})
    )
    offer = {
        "offer_id": _DIGEST_A,
        "offer_revision": 1,
        "actions": [{"action_id": "replan_from_stopped_state", "enabled": True}],
    }
    runtime._recovery_offer = lambda *_args, **_kwargs: deepcopy(offer)
    calls = []

    def focused(*_args, **_kwargs):
        calls.append("focused")
        raise ProgramRunOwnerError("focused owner rejected without effect")

    def selected(*_args, **_kwargs):
        calls.append("selected")
        assert (
            _kwargs["expected_run_revision"]
            == owner.read("program-cycle.1").run_revision
        )
        if case == "stale":
            raise ProgramRunOwnerStaleRevisionError(_kwargs["expected_run_revision"], 2)
        error = (
            ProgramRunOwnerRecoveryIncompleteError
            if case == "incomplete"
            else ProgramRunOwnerOutcomeUnknownError
        )
        raise error("first cleanup accepted; second result " + case)

    owner.apply_owner_recovery = focused
    host.resume_motion = selected
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_id": "resume.1",
            "request_owner_epoch": "client.1",
            "source_label": "test",
            "expected_owner_epoch": runtime.owner_epoch,
            "offer_id": offer["offer_id"],
            "offer_revision": offer["offer_revision"],
            "action_id": "replan_from_stopped_state",
        }
    )
    try:
        with pytest.raises(ProgramRunnerError) as rejected:
            runtime.apply_recovery_action(request)
        assert calls == ["focused" if case == "unselected" else "selected"]
        if case == "unknown":
            assert rejected.value.code == "program_cycle_reconciliation_required"
            assert journal.read().reconciliation_required
            # Explicit containment must remain available without retiring or
            # reclassifying the uncertain command that caused the fence.
            pending = journal._database.execute(
                "SELECT * FROM program_cycle_inflight_commands"
            ).fetchall()
            assert len(pending) == 1
            claim = journal.read().claim
            stopped = runtime.stop_run(
                "program-cycle.1",
                ProgramRunStopRequest.model_validate(
                    {
                        "request_id": "contain.unknown.resume",
                        "request_owner_epoch": "client.1",
                        "source_label": "test",
                        "program_run_id": "program-cycle.1",
                        "expected_owner_epoch": runtime.owner_epoch,
                    }
                ),
            )
            assert stopped["state"] == "stopped"
            assert journal.read().state == "terminal_awaiting_reset"
            assert journal.read().claim == claim
            assert journal.read().reconciliation_required
            assert (
                journal._database.execute(
                    "SELECT * FROM program_cycle_inflight_commands"
                ).fetchall()
                == pending
            )
        else:
            assert journal.read().state == "running"
            assert not journal.read().reconciliation_required
            if case == "incomplete":
                assert rejected.value.code == "program_motion_resume_incomplete"
                assert rejected.value.details["cleanup_accepted"] is True
                assert rejected.value.details["recovery_offer"] == offer
            if case == "stale":
                assert rejected.value.code == "program_runtime_control_stale"
                assert rejected.value.details["expected_run_revision"] == 1
                assert rejected.value.details["current_run_revision"] == 2
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(
    ("near_revision_limit", "pending_stop", "failure"),
    [
        (False, False, None),
        (True, False, None),
        (False, True, None),
        (True, True, None),
        (False, True, "restore"),
        (False, True, "release"),
    ],
)
def test_operator_motion_stop_reset_stops_tree_and_resets_cycle_once(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    near_revision_limit: bool,
    pending_stop: bool,
    failure: str | None,
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
    cleared_stop_faults: list[str] = []
    runtime._clear_reset_stop_fault = (  # type: ignore[method-assign]  # noqa: SLF001
        lambda reset: cleared_stop_faults.append(reset.program_run_id)
    )
    runtime.start()
    runtime.start_run(_start_request())

    if pending_stop:
        stop_run = host.stop_run
        terminal = None

        def stop_pending(program_run_id: str, **kwargs: Any) -> _Snapshot:
            nonlocal terminal
            terminal = stop_run(program_run_id, **kwargs)
            stopping = _Snapshot(
                program_run_id,
                owner_id=runtime.owner_id,
                owner_epoch=runtime.owner_epoch,
                state="stopping",
                revision=terminal.run_revision,
            )
            owner.rows[program_run_id] = stopping
            return stopping

        def wait_settlement(program_run_id: str, **_kwargs: Any) -> _Snapshot:
            assert journal.read().state == "resetting"
            assert journal.read().reset_phase == "stopping"
            assert not admission.releases and not host.reset_calls
            assert terminal is not None
            owner.rows[program_run_id] = terminal
            return terminal

        host.stop_run = stop_pending
        host.wait_terminal = wait_settlement

    if near_revision_limit:
        controls = runtime._controls("program-cycle.1")
        controls.audit_value("prior_control")
        snapshot = controls.snapshot()
        maximum = 9_007_199_254_740_991
        snapshot["runtime_control_revision"] = maximum - 1
        snapshot["audit"] = [
            {"event_revision": index, "kind": "prior_control"}
            for index in range(maximum - 257, maximum - 1)
        ]
        capture = runtime.read_run("program-cycle.1")["capture"]
        runtime._runtime_controls["program-cycle.1"] = ProgramRuntimeControls.restore(
            capture, snapshot
        )

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
        lambda _run_id, *, cycle_generation, selection_scope=None: (
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

    if failure is not None:
        if failure == "restore":

            def failed_restore(*_args: Any) -> None:
                raise ProgramRunnerError("program_scene_unavailable", "restore failed")

            runtime._prepare_scene_after_reset = failed_restore
        else:
            admission.release_error = RuntimeError("release reply lost")

        try:
            with pytest.raises(ProgramRunnerError) as rejected:
                runtime.apply_recovery_action(request)
            cause = (
                "program_scene_unavailable"
                if failure == "restore"
                else "program_cycle_reconciliation_required"
            )
            assert rejected.value.code == "program_cycle_recovery_required"
            assert rejected.value.details == {"cause_code": cause}
            failed_cycle = journal.read()
            assert failed_cycle.state == "recovery_required"
            assert failed_cycle.cycle_generation == 1
            assert failed_cycle.claim is not None
            assert failed_cycle.reset_phase == (
                "restoring_scene" if failure == "restore" else "releasing"
            )
            assert failed_cycle.reconciliation_required is (failure == "release")
            assert failed_cycle.failure["code"] == cause
            if failure == "restore":
                assert failed_cycle.failure["failed_phase"] == "restoring_scene"
                assert not admission.releases
            pending = journal._database.execute(  # noqa: SLF001
                "SELECT request_id,request_kind FROM program_cycle_inflight_commands"
            ).fetchall()
            # The wrapper retires only itself. One canonical Reset survives
            # both known and unknown phase failures until successful recovery.
            assert all(row[0] != request.request_id for row in pending)
            assert [row[1] for row in pending] == ["reset"]
        finally:
            runtime.shutdown()
        return

    result = runtime.apply_recovery_action(request)

    with pytest.raises(ProgramRunnerError) as completed_recovery:
        runtime.apply_recovery_action(request)
    assert completed_recovery.value.code == "cycle_recovery_offer_stale"
    assert result["schema_version"] == "manyforge.cycle_recovery_result.v2"
    assert result["action_id"] == "reset_cycle"
    assert result["cycle_resumed"] is False
    assert result["run"]["state"] == "stopped"
    assert result["recovery_offer"] is None
    assert result["runtime_controls"] is None
    assert host.stop_calls == [
        ("program-cycle.1", "operator requested Program cycle Reset")
    ]
    assert host.reset_calls == ["program-cycle.1"]
    assert cleared_stop_faults == ["program-cycle.1"]
    assert len(admission.releases) == 1
    assert journal.read().state == "idle_after_reset"
    if near_revision_limit:
        controls_result = result["owner_result"]["cycle_reset"]["runtime_controls"]
        assert controls_result["runtime_control_revision"] == maximum
        assert controls_result["audit"][-1]["kind"] == "program_runtime_controls_reset"
        assert controls_result["audit"][-1]["correlation_id"] == "operator-stop-reset.1"
    runtime.shutdown()


@pytest.mark.parametrize("endpoint", ["cycle_status", "read_runtime_controls"])
def test_control_projection_reuses_one_paired_run_and_selection_snapshot(
    monkeypatch, tmp_path, endpoint
):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, _journal, _admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    owner = _RuntimeControlOwner()
    runtime._owner = owner
    runtime._host = _Host(
        owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch
    )
    runtime.start()
    run_id = runtime.start_run(_start_request())["capture"]["program_run_id"]

    def crossed_assessment(*_args, **_kwargs):
        owner._advance(run_id)
        return None

    pairs = []

    def paired(current_id):
        live = owner.read(current_id)
        selections = owner.execution_selection_controls(current_id)
        pairs.append((live, selections))
        return live, selections

    runtime._recovery_offer = crossed_assessment
    owner.read_selection_controls = paired
    try:
        if endpoint == "cycle_status":
            status = runtime.cycle_status()
            controls = status["runtime_controls"]
            assert (
                status["run_revision"]
                == status["run"]["run_revision"]
                == controls["run_revision"]
            )
        else:
            controls = runtime.read_runtime_controls(run_id)
        assert len(pairs) == 1
        assert controls["run_revision"] == pairs[0][0].run_revision
        assert controls["execution_selections"] == pairs[0][1]

        def retired(_run_id):
            raise ProgramRunOwnerError("Program run does not exist")

        owner.read_selection_controls = retired
        with pytest.raises(ProgramRunnerError) as unavailable:
            runtime.read_runtime_controls(run_id)
        assert unavailable.value.code == "program_run_not_found"
    finally:
        runtime.shutdown()


def test_cycle_status_projects_pending_acquire_as_starting(tmp_path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
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


def test_runtime_controls_are_state_conditioned_and_restore_current_projection(
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

    commands = {
        row["command_kind"]: row for row in runtime.cycle_status()["allowed_commands"]
    }
    assert commands["gripper_confirm"]["allowed"] is True
    # Native target/context checks, not this coarse cycle projection, decide
    # whether held-cycle manual motion can be admitted.
    assert commands["operation_start"]["allowed"] is True
    assert commands["program_manual_handover"]["allowed"] is True

    written = runtime.write_runtime_blackboard(
        program_run_id,
        RuntimeBlackboardWriteRequest.model_validate(
            {
                **request_base("controls.write.1", 1),
                "key": "operator.confirmed",
                "value": True,
            }
        ),
    )
    assert written["result"] == {
        "key": "operator.confirmed",
        "status": "written",
    }
    with pytest.raises(ProgramRunnerError) as repeated_write:
        runtime.write_runtime_blackboard(
            program_run_id,
            RuntimeBlackboardWriteRequest.model_validate(
                {
                    **request_base("controls.write.2", 1),
                    "key": "operator.confirmed",
                    "value": False,
                }
            ),
        )
    assert repeated_write.value.code == "program_runtime_control_stale"
    assert repeated_write.value.details == {
        "expected_run_revision": 1,
        "current_run_revision": 2,
    }
    assert written["runtime_controls"]["runtime_controls"][
        "runtime_control_revision"
    ] == 2

    # A failed attempt held for intervention keeps permitted keys writable
    # through the same value-store path; the write never resumes the run and
    # manual-result staging stays unavailable.
    current = owner.rows[program_run_id]
    owner.rows[program_run_id] = _Snapshot(
        program_run_id,
        owner_id=current.to_dict()["owner_id"],
        owner_epoch=current.owner_epoch,
        state="intervention_required",
        revision=current.run_revision,
    )
    commands = {
        row["command_kind"]: row for row in runtime.cycle_status()["allowed_commands"]
    }
    assert commands["program_runtime_blackboard_write"]["allowed"] is True
    assert commands["program_manual_result_stage"]["allowed"] is False
    paused = runtime.write_runtime_blackboard(
        program_run_id,
        RuntimeBlackboardWriteRequest.model_validate(
            {
                **request_base("controls.write.paused", current.run_revision),
                "key": "operator.confirmed",
                "value": False,
            }
        ),
    )
    assert paused["run"]["state"] == "intervention_required"
    expected_controls = paused["runtime_controls"]["runtime_controls"]
    assert expected_controls["runtime_control_revision"] == 3
    assert [kind for kind, _values in owner.effects] == ["write", "write"]

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
        "execution_control_revision",
        "causal_correlation_id",
        "causal_session_id",
        "causal_client_sequence",
        "current_command_kind",
        "failure",
        "reset_phase",
        "allowed_commands",
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
    assert host.stop_calls == [("program-run.1", "manager_presence_expired")]
    assert replacement.read().state == "terminal_awaiting_reset"
    assert replacement.read().reconciliation_required is True
    assert runtime.liveness_status()["command_fenced"] is True
    runtime.shutdown()


def test_restart_contains_a_nonresident_run_from_exact_kernel_scope(
    tmp_path,
) -> None:
    path = tmp_path / "cycle.sqlite3"
    old = ProgramCycleJournal(path, owner_epoch="runner.old")
    claim = _program_cycle_claim("runner.old")
    old.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-run.1",
        evidence=claim["intent"],
    )
    old.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-run.1",
        claim=claim,
    )
    running = _Snapshot(
        "program-run.1",
        owner_id="program-runner",
        owner_epoch="runner.old",
        state="running",
        revision=4,
    ).to_dict()
    old.transition(
        expected_generation=1,
        expected_states=("starting",),
        state="running",
        program_run_id="program-run.1",
        run_snapshot=running,
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

    class _SettledScope:
        def __init__(self) -> None:
            self.requests: list[tuple[str, str, str | None]] = []
            self.observations = 0

        def request_stop(
            self,
            program_run_id: str,
            stop_request_id: str,
            reason: str | None,
        ) -> ProgramRunScopeSnapshot:
            self.requests.append((program_run_id, stop_request_id, reason))
            return ProgramRunScopeSnapshot(
                program_run_id,
                (
                    ProgramRunScopeState.ACTIVE
                    if len(self.requests) == 1
                    else ProgramRunScopeState.SETTLED
                ),
                1 if len(self.requests) == 1 else 0,
            )

        def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
            self.observations += 1
            return ProgramRunScopeSnapshot(
                program_run_id,
                ProgramRunScopeState.SETTLED,
                0,
            )

    replacement = ProgramCycleJournal(path, owner_epoch="runner.new")
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
    scope = _SettledScope()
    runtime._native = SimpleNamespace(scope=scope)  # type: ignore[assignment]  # noqa: SLF001

    runtime.start()

    # Startup submits containment once without polling or delaying liveness.
    # The next ordinary Manager renewal rejoins the same exact Kernel Stop and
    # observes its settlement.
    assert replacement.read().state == "running"
    assert runtime._contain_retained_run(  # noqa: SLF001
        "program-run.1",
        fault_reference=_DIGEST_B,
    )

    cycle = replacement.read()
    terminal = replacement.read_run_snapshot("program-run.1")
    assert cycle.state == "terminal_awaiting_reset"
    assert cycle.reconciliation_required is True
    assert terminal is not None
    assert terminal["run_revision"] == 5
    assert terminal["state"] == "stopped"
    assert terminal["tree_status"] == "invalid"
    assert terminal["node_statuses"] == {"root": "idle"}
    assert terminal["node_status_revision"] == 5
    assert terminal["active_operation_count"] == 0
    assert len(scope.requests) == 2
    assert scope.requests[0][0] == "program-run.1"
    assert scope.requests[0][2] == "manager_presence_expired"
    assert scope.requests[0][1] == scope.requests[1][1]
    assert scope.observations == 0
    assert runtime.liveness_status()["command_fenced"] is True
    runtime.shutdown()


def test_tree_refinement_never_widens_reconciliation_permissions(tmp_path) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.current",
    )
    claim = _program_cycle_claim("runner.current")
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id="program-run.1",
        evidence=claim["intent"],
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id="program-run.1",
        claim=claim,
    )
    running = _Snapshot(
        "program-run.1",
        owner_id="program-runner",
        owner_epoch="runner.current",
        state="running",
        revision=2,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("starting",),
        state="running",
        program_run_id="program-run.1",
        run_snapshot=running.to_dict(),
    )
    journal.record_request(
        owner_epoch="operator.1",
        request_id="resume.unknown",
        kind="resume_motion",
        intent={"program_run_id": "program-run.1"},
        cycle_generation=1,
    )
    journal.require_reconciliation(expected_generation=1)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="runner.current",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    owner.rows["program-run.1"] = running

    def unexpected_intervention_lookup(_run_id: str) -> None:
        raise AssertionError("reconciliation must suppress node recovery lookup")

    owner.intervention = unexpected_intervention_lookup  # type: ignore[method-assign]
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    assert (
        runtime._recovery_offer(  # noqa: SLF001
            "program-run.1", cycle_generation=1
        )
        is None
    )
    owner.intervention = lambda _run_id: None  # type: ignore[method-assign]

    status = runtime.cycle_status()
    assert status["recovery_offer"] is None
    assert status["failure"]["command_kind"] == "resume_motion"
    assert "blocked until the Program is safely reconciled" in status["failure"][
        "message"
    ]
    permissions = {
        command["command_kind"]: command["allowed"]
        for command in status["allowed_commands"]
    }

    assert {kind for kind, allowed in permissions.items() if allowed} == {
        "cell_full_stop",
        "execution_recovery",
        "stop_recovery",
        # Exact native manual retirement cannot deadlock Program recovery.
        "operation_reset",
        "operation_acknowledge_completed",
    }
    runtime.shutdown()


@pytest.mark.parametrize(
    "outcome_unknown",
    [False, True],
    ids=["explicit-rejection", "unknown-outcome"],
)
def test_release_failure_is_visible_and_only_known_rejection_can_resume(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    outcome_unknown: bool,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(
        path,
        owner_epoch="runner.current",
        maximum_inflight_commands=1,
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
    admission.release_error = CellAdmissionError(
        "settlement_validator_unavailable",
        "settlement validator is unavailable",
        outcome_unknown=outcome_unknown,
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
    assert rejected.value.code == "program_cycle_recovery_required"
    assert rejected.value.details == {"cause_code": "settlement_validator_unavailable"}
    failed_cycle = journal.read()
    assert failed_cycle.state == "recovery_required"
    assert failed_cycle.reset_phase == "releasing"
    if outcome_unknown:
        assert failed_cycle.reconciliation_required is True
        assert failed_cycle.failure == {"code": "program_cycle_reconciliation_required"}
        reset_availability = {
            command["command_kind"]: command
            for command in runtime.cycle_status()["allowed_commands"]
        }
        assert reset_availability["program_reset"]["allowed"] is False
        assert journal._database.execute(  # noqa: SLF001
            "SELECT COUNT(*) FROM program_cycle_inflight_commands"
        ).fetchone() == (1,)
        runtime.shutdown()
        return

    assert failed_cycle.reconciliation_required is False
    assert failed_cycle.failure == {
        "code": "settlement_validator_unavailable",
        "failed_phase": "releasing",
        "message": "settlement validator is unavailable",
    }
    assert journal.read().release_intent == admission.releases[0]
    reset_availability = {
        command["command_kind"]: command
        for command in runtime.cycle_status()["allowed_commands"]
    }
    assert reset_availability["program_reset"]["allowed"] is True

    admission.release_error = None
    retry = request.model_copy(update={"request_id": "reset.2"})
    assert runtime.reset_cycle(retry)["idle_cycle_generation"] == 2
    assert journal.read().state == "idle_after_reset"
    assert admission.releases == [admission.releases[0], admission.releases[0]]
    runtime.shutdown()


@pytest.mark.parametrize("phase_write_failure", [False, True])
def test_reset_keeps_backend_fenced_when_scene_convergence_fails(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    phase_write_failure: bool,
) -> None:
    events: list[str] = []

    class OrderedAdmission(_Admission):
        def release(self, release: dict[str, Any]) -> dict[str, Any]:
            result = super().release(release)
            events.append("release")
            return result

    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.reset-scene.1",
        maximum_inflight_commands=1,
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
        *,
        expected_record_digest: str | None,
        context_id: str,
        program_reset_scene_authorization: Mapping[str, Any] | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        assert expected_record_digest is None
        assert journal.read().state == "resetting"
        assert journal.read().reset_phase == "restoring_scene"
        assert program_reset_scene_authorization == {
            "program_runner_owner_epoch": "runner.reset-scene.1",
            "program_run_id": "program-cycle.1",
            "cycle_claim_generation": 1,
            "terminal_or_settlement_reference": terminal_reference,
        }
        contexts.append(context_id)
        events.append(f"scene:{len(contexts)}")
        if len(contexts) == 1 and not phase_write_failure:
            raise RuntimeError("logical scene owner temporarily unavailable")
        return ({"source": {"record_digest": _DIGEST_A}}, {"proof": True})

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

    if phase_write_failure:
        capture = {
            "program_run_id": "program-cycle.1",
            "program_id": "program.1",
            "program_revision": _DIGEST_A,
            "program_binding_id": _DIGEST_B,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "node_environment_digest": _DIGEST_A,
        }
        snapshot = ProgramRuntimeControls(capture).snapshot()
        maximum = 9_007_199_254_740_991
        snapshot["runtime_control_revision"] = maximum - 1
        snapshot["audit"] = [
            {"event_revision": index, "kind": "prior_control"}
            for index in range(maximum - 257, maximum - 1)
        ]
        controls = ProgramRuntimeControls.restore(capture, snapshot)
        runtime._runtime_controls["program-cycle.1"] = controls
        update_phase = journal.update_reset_phase
        failed_phase_write = False

        def fail_phase_once(**kwargs):
            nonlocal failed_phase_write
            if kwargs["phase"] == "releasing" and not failed_phase_write:
                failed_phase_write = True
                assert controls.revision == maximum
                raise RuntimeError("Reset phase write temporarily unavailable")
            return update_phase(**kwargs)

        monkeypatch.setattr(journal, "update_reset_phase", fail_phase_once)

    with pytest.raises(ProgramRunnerError) as failed:
        runtime.reset_cycle(request)
    assert failed.value.code == "program_cycle_recovery_required"
    assert failed.value.details == {
        "cause_code": "program_cycle_reset_scene_preparation_failed"
    }
    assert events == ["scene:1"]
    failed_cycle = journal.read()
    assert failed_cycle.state == "recovery_required"
    assert failed_cycle.reconciliation_required is False
    assert failed_cycle.reset_phase == "restoring_scene"
    assert failed_cycle.failure == {
        "code": "program_cycle_reset_scene_preparation_failed",
        "failed_phase": "restoring_scene",
        "message": (
            "Reset phase write temporarily unavailable"
            if phase_write_failure
            else "logical scene owner temporarily unavailable"
        ),
    }
    assert admission.releases == []

    retry = request.model_copy(update={"request_id": "reset.scene.2"})
    assert runtime.reset_cycle(retry)["idle_cycle_generation"] == 2
    assert events == ["scene:1", "scene:2", "release", "idle"]
    assert len(admission.releases) == 1
    assert journal.read().state == "idle_after_reset"
    if phase_write_failure:
        assert controls.revision == maximum
        assert controls.snapshot()["audit"][-1]["request_id"] == "reset.scene.1"
    runtime.shutdown()


def test_rejected_and_completed_resets_do_not_consume_inflight_capacity(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.current",
        maximum_inflight_commands=2,
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
            maximum_inflight_commands=2,
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
    with pytest.raises(ProgramRunnerError) as stale_again:
        runtime.reset_cycle(stale)
    assert stale_again.value.code == "program_cycle_reset_stale"
    assert len(admission.releases) == 1

    another_stale = ProgramCycleResetRequest.model_validate(
        {**stale.model_dump(mode="json"), "request_id": "reset.capacity"}
    )
    with pytest.raises(ProgramRunnerError) as rejected_again:
        runtime.reset_cycle(another_stale)
    assert rejected_again.value.code == "program_cycle_reset_stale"
    assert journal._database.execute(  # noqa: SLF001
        "SELECT COUNT(*) FROM program_cycle_inflight_commands"
    ).fetchone() == (0,)
    runtime.shutdown()


@pytest.mark.parametrize(
    "field,value",
    [
        ("state", "resetting"),
        ("owner_epoch", "predecessor"),
        ("program_run_id", "different-run"),
        ("program_run_id", None),
        ("claim", None),
        ("claim", {"intent": {"owner_epoch": "runner.1", "owner_role": "other"}}),
        ("acquisition_pending", {}),
        ("reset_phase", "settling"),
        ("release_intent", {}),
    ],
)
def test_recovery_stop_does_not_admit_other_ownership_or_pending_reset(field, value):
    cycle = SimpleNamespace(
        state="recovery_required",
        owner_epoch="runner.1",
        program_run_id="program-cycle.1",
        claim={
            "claim_generation": 1,
            "intent": {
                "owner_epoch": "runner.1",
                "owner_role": "program_runner",
                "claim_kind": "program_cycle",
                "owner_transaction_id": "program-cycle.1",
            },
        },
        acquisition_pending=None,
        reset_phase=None,
        release_intent=None,
    )
    runner = SimpleNamespace(owner_epoch="runner.1")
    assert ProgramRuntime._current_cycle_recovery_stop(runner, cycle)
    setattr(cycle, field, value)
    assert not ProgramRuntime._current_cycle_recovery_stop(runner, cycle)


@pytest.mark.parametrize("generation", [None, True, 0, -1, 9_007_199_254_740_992])
def test_recovery_stop_requires_valid_claim_generation(generation):
    cycle = SimpleNamespace(
        state="recovery_required",
        owner_epoch="runner.1",
        program_run_id="program-cycle.1",
        claim={
            "claim_generation": generation,
            "intent": {
                "owner_epoch": "runner.1",
                "owner_role": "program_runner",
                "claim_kind": "program_cycle",
                "owner_transaction_id": "program-cycle.1",
            },
        },
        acquisition_pending=None,
        reset_phase=None,
        release_intent=None,
    )
    runner = SimpleNamespace(owner_epoch="runner.1")
    assert not ProgramRuntime._current_cycle_recovery_stop(runner, cycle)


@pytest.mark.parametrize(
    "uncertain_resume,reset_outcome_unknown",
    [(False, False), (True, False), (False, True)],
)
def test_stop_retains_claim_until_exact_terminal_snapshot_is_observed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    uncertain_resume,
    reset_outcome_unknown,
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

    if uncertain_resume:
        runtime._journal.require_reconciliation(expected_generation=1)
    commands = {
        row["command_kind"]: row["allowed"]
        for row in runtime.cycle_status()["allowed_commands"]
    }
    assert commands["program_stop"]
    if uncertain_resume:
        assert not commands["program_run"] and not commands["program_motion_resume"]

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
    assert runtime._journal.read().reconciliation_required is uncertain_resume

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
    assert runtime._journal.read().reconciliation_required is uncertain_resume
    if not uncertain_resume:
        _bypass_reset_scene_preparation(monkeypatch)
        if reset_outcome_unknown:

            def uncertain_containment(*_args, **_kwargs):
                raise ProgramRunOwnerOutcomeUnknownError("Reset Stop response was lost")

            host.contain_reset_run = uncertain_containment
        reset = ProgramCycleResetRequest.model_validate(
            {
                "request_id": "reset.terminal",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator_ui",
                "program_run_id": "program-cycle.1",
                "expected_owner_epoch": runtime.owner_epoch,
                "expected_cycle_generation": 1,
                "expected_claim_generation": 1,
                "terminal_reference": status["terminal_or_settlement_reference"],
                "settlement_evidence_digest": status[
                    "terminal_or_settlement_reference"
                ],
            }
        )
        if reset_outcome_unknown:
            with pytest.raises(ProgramRunnerError) as rejected:
                runtime.reset_cycle(reset)
            assert (
                rejected.value.details["cause_code"]
                == "program_cycle_reconciliation_required"
            )
            current = runtime._journal.read()
            assert (
                current.state == "recovery_required"
                and current.reset_phase == "stopping"
            )
            assert current.reconciliation_required and current.claim is not None
            assert admission.releases == [] and host.reset_calls == []
            assert runtime._journal._database.execute(
                "SELECT request_id FROM program_cycle_inflight_commands"
            ).fetchall() == [("reset.terminal",)]
        else:
            assert runtime.reset_cycle(reset)["idle_cycle_generation"] == 2
            assert host.reset_containment_calls == ["program-cycle.1"]
            assert host.reset_calls == ["program-cycle.1"]
    runtime.shutdown()


@pytest.fixture
def uncertain_reset_runtime(monkeypatch, tmp_path):
    """One lost Resume through the public owner path, with ordinary capacity full."""
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    monkeypatch.setattr(
        ProgramRuntime, "_prepare_scene_after_reset", lambda *_args: None
    )
    runtime, journal, admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    journal._maximum_inflight_commands = 1
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    claim = journal.read().claim
    admission.inventory = {
        "status": "held_exact",
        "kernel_owner_epoch": "kernel.1",
        "held_claims": [claim],
        "matched_claim": claim,
        "matched_release": None,
        "matched_reconcile_settlement": None,
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.1"}
        ],
    }

    def unknown_resume(*_args, **_kwargs):
        raise ProgramRunOwnerOutcomeUnknownError("Resume response lost")

    host.resume_full_stop = unknown_resume
    request = ProgramFullStopResumeRequest.model_validate(
        {
            "request_id": "resume.unknown",
            "request_owner_epoch": "operator.1",
            "source_label": "operator_ui",
            "expected_owner_epoch": runtime.owner_epoch,
            "program_run_id": "program-cycle.1",
            "expected_run_revision": 1,
        }
    )
    with pytest.raises(ProgramRunnerError) as unknown:
        runtime.resume_full_stop("program-cycle.1", request)
    assert unknown.value.code == "program_cycle_reconciliation_required"
    retained = journal.read()
    assert retained.reconciliation_required
    assert retained.failure == {
        "cause_code": "ProgramRunOwnerOutcomeUnknownError",
        "code": "program_runtime_control_outcome_unknown",
        "command_kind": "resume_full_stop",
        "message": (
            "The Resume Full Stop outcome could not be confirmed. Further execution "
            "is blocked until the Program is safely reconciled."
        ),
    }
    assert runtime.cycle_status()["recovery_offer"] is None
    assert len(journal.pending_commands()) == 1
    try:
        yield runtime, journal, admission, owner, host
    finally:
        runtime.shutdown()


def _current_reset_request(runtime, journal, request_id):
    cycle = journal.read()
    return ProgramCycleResetRequest.model_validate(
        {
            "request_id": request_id,
            "request_owner_epoch": "operator.1",
            "source_label": "operator_ui",
            "expected_owner_epoch": runtime.owner_epoch,
            "program_run_id": cycle.program_run_id,
            "expected_cycle_generation": cycle.cycle_generation,
            "expected_claim_generation": cycle.claim["claim_generation"],
            "terminal_reference": cycle.terminal_reference,
            "settlement_evidence_digest": cycle.terminal_reference,
        }
    )


@pytest.mark.parametrize("lost_confirmations", [0, 2])
def test_reset_recovers_unknown_resume_without_replay_or_capacity_growth(
    uncertain_reset_runtime, lost_confirmations
):
    runtime, journal, admission, owner, host = uncertain_reset_runtime
    original = journal.pending_commands()
    stop_ids, restored_ids = [], []
    contain = host.contain_reset_run

    def contain_or_lose(run_id, *, stop_request_id, timeout_s):
        stop_ids.append(stop_request_id)
        assert journal.read().reconciliation_required
        assert original[0] in journal.pending_commands()
        assert not admission.releases
        if len(stop_ids) <= lost_confirmations:
            raise ProgramRunOwnerOutcomeUnknownError("Stop response lost")
        return contain(run_id, stop_request_id=stop_request_id, timeout_s=timeout_s)

    def restore(request, _authorization):
        assert not journal.read().reconciliation_required
        assert journal.read().reset_phase == "restoring_scene"
        assert len(journal.pending_commands()) == 2
        assert original[0] in journal.pending_commands()
        restored_ids.append(request.request_id)

    host.contain_reset_run = contain_or_lose
    runtime._prepare_scene_after_reset = restore
    for index in range(lost_confirmations + 1):
        request = _current_reset_request(runtime, journal, f"reset.{index}")
        allowed = {
            row["command_kind"]: row["allowed"]
            for row in runtime.cycle_status()["allowed_commands"]
        }
        assert allowed["program_reset"] and not allowed["program_run"]
        assert not allowed["program_motion_resume"]
        if index < lost_confirmations:
            with pytest.raises(ProgramRunnerError) as unknown:
                runtime.reset_cycle(request)
            assert (
                unknown.value.details["cause_code"]
                == "program_cycle_reconciliation_required"
            )
            assert len(journal.pending_commands()) == 2
            assert journal.read().reconciliation_required
            if index == 0:
                with pytest.raises(ProgramRunnerError) as duplicate:
                    runtime.reset_cycle(request)
                assert duplicate.value.code == "execution_control_command_in_progress"
                assert len(stop_ids) == 1
        else:
            assert runtime.reset_cycle(request)["idle_cycle_generation"] == 2
    assert len(set(stop_ids)) == lost_confirmations + 1
    assert restored_ids == ["reset.0"]
    assert len(admission.releases) == 1
    assert journal.pending_commands() == ()
    assert journal.read().state == "idle_after_reset"
    assert not owner.rows


@pytest.mark.parametrize(
    "crossing",
    [
        "kernel",
        "owner",
        "claim",
        "program_node_evidence",
        "intervention_evidence",
        "active_run",
        "liveness",
        "revision",
    ],
)
def test_unknown_reset_never_restores_without_exact_containment(
    uncertain_reset_runtime, crossing
):
    runtime, journal, admission, owner, host = uncertain_reset_runtime
    reset = host.reset_run

    def cross_after_cleanup(run_id):
        result = reset(run_id)
        receipt = admission.inventory
        if crossing == "kernel":
            receipt["kernel_owner_epoch"] = "kernel.replaced"
        elif crossing == "owner":
            receipt["current_owner_epochs"][0]["owner_epoch"] = "runner.replaced"
        elif crossing == "claim":
            receipt["matched_claim"] = {
                **receipt["matched_claim"],
                "claim_generation": 2,
            }
        elif crossing in {"program_node_evidence", "intervention_evidence"}:
            receipt["held_claims"].append(
                {"intent": {crossing: {"parent_program_cycle_claim_generation": 1}}}
            )
        elif crossing == "active_run":
            owner.rows[run_id]._value["active_operation_count"] = 1
        elif crossing == "liveness":
            journal.retain_liveness_loss(
                expected_generation=1,
                manager_runtime_epoch="manager.1",
                manager_owner_epoch="manager.1",
                fault_reference="manager-lost.1",
            )
        else:
            journal.update_reset_phase(
                expected_generation=1,
                program_run_id=run_id,
                expected_phase="settling",
                phase="settling",
            )
        return result

    def forbidden_restore(*_args):
        pytest.fail("uncertain Reset crossed into scene restoration")

    host.reset_run = cross_after_cleanup
    runtime._prepare_scene_after_reset = forbidden_restore
    with pytest.raises(ProgramRunnerError) as rejected:
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
    assert (
        rejected.value.details["cause_code"] == "program_cycle_reconciliation_required"
    )
    assert not admission.releases
    assert journal.read().reconciliation_required
    assert journal.read().state == "recovery_required"
    assert len(journal.pending_commands()) == 2


def test_unknown_reset_requires_resident_run_and_preserves_conflicting_request(
    uncertain_reset_runtime,
):
    runtime, journal, admission, owner, host = uncertain_reset_runtime
    before = journal.pending_commands()
    with pytest.raises(ProgramRunnerError) as conflict:
        runtime.reset_cycle(_current_reset_request(runtime, journal, "resume.unknown"))
    assert conflict.value.code == "program_run_request_conflict"
    owner.rows.clear()
    assert not next(
        row["allowed"]
        for row in runtime.cycle_status()["allowed_commands"]
        if row["command_kind"] == "program_reset"
    )
    with pytest.raises(ProgramRunnerError) as missing:
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
    assert missing.value.code == "program_cycle_reconciliation_required"
    assert journal.pending_commands() == before
    assert not host.stop_calls and not host.reset_calls and not admission.releases


def test_reset_confirmation_uses_fresh_fault_recovery_identity(uncertain_reset_runtime):
    runtime, journal, admission, _owner, _host = uncertain_reset_runtime
    recoveries = []
    offer = {
        "latch": {
            "latch_id": "latch.1",
            "latch_generation": 1,
            "recovery_assessment_revision": 1,
            "stop_certificate": {
                "stop_certificate_id": "stop.1",
                "stop_certificate_revision": 1,
            },
        },
        "configuration_expectations": [],
    }

    def recover(intent):
        recoveries.append(intent)
        if len(recoveries) == 1:
            offer["latch"]["recovery_assessment_revision"] = 2
            raise ProgramRunOwnerOutcomeUnknownError("fault recovery response lost")
        offer.clear()
        return {"result": {"accepted": True}}

    runtime._kernel = SimpleNamespace(
        stop_fault=lambda: {"stop_fault": deepcopy(offer) if offer else None},
        identity=lambda: {"identity": {"owner_epoch": "kernel.1"}},
        recover_stop_fault=recover,
    )
    with pytest.raises(ProgramRunnerError) as unknown:
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
    assert (
        unknown.value.details["cause_code"] == "program_cycle_reconciliation_required"
    )
    assert journal.read().reconciliation_required and not admission.releases
    assert (
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.1"))[
            "idle_cycle_generation"
        ]
        == 2
    )
    assert [row["expected_recovery_assessment_revision"] for row in recoveries] == [
        1,
        2,
    ]
    assert len({row["request_context"]["request_id"] for row in recoveries}) == 2
    assert [row["request_context"]["correlation_id"] for row in recoveries] == [
        "reset.0",
        "reset.1",
    ]


def test_recovery_reset_classifies_retained_intents_not_failure_text(
    uncertain_reset_runtime,
):
    runtime, journal, _admission, _owner, _host = uncertain_reset_runtime
    cycle = journal.read()
    row = journal.pending_commands()[0]
    assert runtime._current_cycle_recovery_reset(cycle, [row])
    for kind in ("resume_motion", "resume_full_stop", "prepare_manual_handover"):
        assert runtime._current_cycle_recovery_reset(cycle, [{**row, "kind": kind}])
    for field, value in (
        ("cycle_generation", 2),
        ("kind", "start"),
        ("kind", "discard_full_stop"),
    ):
        assert not runtime._current_cycle_recovery_reset(cycle, [{**row, field: value}])
    for field, value in (("expected_owner_epoch", "old"), ("program_run_id", "other")):
        assert not runtime._current_cycle_recovery_reset(
            cycle,
            [
                {
                    **row,
                    "intent": {**row["intent"], field: value},
                }
            ],
        )
    reset = {
        **row,
        "kind": "reset",
        "request_id": "reset.0",
        "intent": _current_reset_request(runtime, journal, "reset.0").model_dump(
            mode="json"
        ),
    }
    assert runtime._current_cycle_recovery_reset(cycle, [reset])
    assert not runtime._current_cycle_recovery_reset(
        cycle, [reset, {**reset, "request_id": "reset.1"}]
    )
    action = {
        **row,
        "kind": "recovery_action",
        "intent": {
            "expected_owner_epoch": runtime.owner_epoch,
            "action_id": "replan_from_stopped_state",
        },
    }
    assert runtime._current_cycle_recovery_reset(cycle, [action])
    action["intent"]["action_id"] = "reset_cycle"
    assert not runtime._current_cycle_recovery_reset(cycle, [action])
    assert not runtime._current_cycle_recovery_reset(cycle, [action, reset])
    reset["request_id"] = runtime._digest(
        {
            "kind": "operator_stop_reset_cycle",
            "request_owner_epoch": row["request_owner_epoch"],
            "request_id": row["request_id"],
            "program_run_id": cycle.program_run_id,
        }
    )
    assert runtime._current_cycle_recovery_reset(cycle, [action, reset])


def test_draft_applied_mismatch_is_rechecked_without_retained_receipts(
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
        def program_applied(self) -> dict[str, Any]:
            events.append("applied")
            admission.generation = 2
            return super().program_applied()

    headless = InterleavedHeadless()
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_inflight_commands=1,
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
    with pytest.raises(ProgramRunnerError) as rechecked:
        runtime.start_run(request)
    assert rechecked.value.code == "stale_generation"
    with pytest.raises(ProgramRunnerError) as still_stale:
        runtime.start_run(_start_request(request_id="request.start.capacity"))
    assert still_stale.value.code == "stale_generation"
    assert events == ["generation", "applied", "generation", "generation"]
    assert admission.claims == []
    assert journal._database.execute(  # noqa: SLF001
        "SELECT COUNT(*) FROM program_cycle_inflight_commands"
    ).fetchone() == (0,)
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
    journal = FailingJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
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
    assert journal.read().state == "recovery_required"
    assert journal.read().claim is None
    assert journal.read().acquisition_pending is not None
    runtime.shutdown()


def test_native_acquire_rejection_is_safe_to_recheck_without_retained_receipt(
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
    assert admission.calls == 2
    runtime.shutdown()


def test_stop_bypasses_transition_lock_and_full_ordinary_inflight_bound(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        owner_epoch="runner.1",
        maximum_inflight_commands=1,
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
            maximum_inflight_commands=1,
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
    assert (
        runtime.stop_run("program-run.1", request)["state"] == "terminal_awaiting_reset"
    )
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
            "source_document_digest": _DIGEST_A,
            "source": {"record_digest": _DIGEST_B},
        },
    )

    runtime.start()
    with pytest.raises(ProgramRunnerError) as stale:
        runtime.start_run(_start_request())
    assert stale.value.code == "program_run_applied_selection_stale"
    assert host.start_admissions == []
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

    def admit(applied: object, **kwargs: object) -> object:
        captured.update(
            applied=applied,
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
                "source_document_digest": _DIGEST_A,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_A,
                "source": {
                    "record_digest": _DIGEST_B,
                    "source_generation": 7,
                },
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
    assert captured["kwargs"] == {
        "expected_scenario_id": "cell-pick",
        "expected_source_document_digest": _DIGEST_A,
        "environment": runtime._environment,  # noqa: SLF001
        "owner_availability": runtime._owner_availability,  # noqa: SLF001
    }
    assert result["availability"] == availability
    assert result["source"]["program_revision"] == _DIGEST_A
    assert headless.applied_reads == 0
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
    ):
        assert availability[port_name] == AvailabilityFact(
            False,
            ("program_execution_owner_unavailable",),
        )
    assert availability["scene_transactions"] == AvailabilityFact(True)
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
    assert runtime._owner_availability["simulator_control"].available  # noqa: SLF001
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


@pytest.mark.parametrize(
    ("action_id", "case"),
    [
        ("retry_runtime_step", "accepted"),
        ("retry_motion_from_current_state", "accepted"),
        ("retry_motion_from_current_state", "rejected"),
        ("retry_motion_from_current_state", "incomplete"),
        ("retry_motion_from_current_state", "unknown"),
        ("retry_motion_from_current_state", "resolve_rejected"),
        ("retry_runtime_step", "forbidden"),
        ("retry_motion_from_current_state", "exhausted"),
    ],
)
def test_failure_retry_prepares_and_releases_through_owner_before_ticking(
    monkeypatch, tmp_path, action_id, case
):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: object(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, journal, _admission = _reconcile_runtime(
        tmp_path,
        journal_owner_epoch="runner.1",
        runtime_owner_epoch="runner.1",
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    # The run-start captured policy, not the caller, permits the Retry.
    owner.rows["program-cycle.1"].capture.node_policy_by_instance_id = {
        "move.1": SimpleNamespace(
            recovery_policy={
                "allow_intervention": case != "forbidden",
                "max_retries": 1 if case == "exhausted" else 0,
            }
        )
    }
    if case == "exhausted":
        runtime._controls("program-cycle.1").note_retry("move.1")
    intervention = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id="program-cycle.1",
            node_instance_id="move.1",
            node_attempt_id="attempt.1",
        ),
        owner_task_id="operation.1",
        policy_parameter="stop_policy",
    )
    resolved = False
    owner.intervention = lambda _run: None if resolved else intervention
    owner.operator_stop_scope = lambda _run: OperatorMotionStopSnapshot(
        4, frozenset({"move.1", "grip.1"})
    )
    offer = {
        "offer_id": _DIGEST_A,
        "offer_revision": 1,
        "actions": [{"action_id": action_id, "enabled": True}],
    }
    runtime._recovery_offer = lambda *_args, **_kwargs: deepcopy(offer)
    calls: list[Any] = []

    def retry(run_id, **kwargs):
        calls.append(("retry_failure_pause", kwargs["action"]))
        if case == "rejected":
            raise ProgramRunOwnerError("a newer Stop holds the motion")
        if case == "incomplete":
            raise ProgramRunOwnerRecoveryIncompleteError("cleanup accepted")
        if case == "unknown":
            raise ProgramRunOwnerOutcomeUnknownError("release reply lost")
        return owner.read(run_id)

    def resume_recovery(run_id, **kwargs):
        nonlocal resolved
        calls.append(("resume_recovery", kwargs["action"]))
        if case == "resolve_rejected":
            raise ProgramSchedulingError("tick host is closed")
        resolved = True
        return owner.read(run_id)

    def forbidden(*_args, **_kwargs):
        raise AssertionError("failure Retry must not use per-node owner recovery")

    owner.retry_failure_pause = retry
    owner.apply_owner_recovery = forbidden
    host.resume_motion = forbidden
    host.resume_recovery = resume_recovery
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_id": "retry.1",
            "request_owner_epoch": "client.1",
            "source_label": "test",
            "expected_owner_epoch": runtime.owner_epoch,
            "offer_id": offer["offer_id"],
            "offer_revision": offer["offer_revision"],
            "action_id": action_id,
        }
    )
    owner_action = None if action_id == "retry_runtime_step" else action_id
    try:
        if case in {"forbidden", "exhausted"}:
            # The command revalidates the same captured-policy predicate as
            # the offer, before any effect, whatever the caller's label.
            for index, label in enumerate(("operator_ui", "assistant")):
                with pytest.raises(ProgramRunnerError) as refused:
                    runtime.apply_recovery_action(
                        request.model_copy(
                            update={
                                "request_id": f"retry.label.{index}",
                                "source_label": label,
                            }
                        )
                    )
                assert refused.value.code == (
                    "program_runtime_retry_forbidden_by_policy"
                    if case == "forbidden"
                    else "program_runtime_retry_limit_reached"
                )
            assert calls == []
            assert not journal.read().reconciliation_required
            return
        if case == "accepted":
            result = runtime.apply_recovery_action(request)
            # Owner prepares and releases all motion first; only then does
            # the tree tick again.
            assert calls == [
                ("retry_failure_pause", owner_action),
                ("resume_recovery", "retry"),
            ]
            assert result["cycle_resumed"] is True
            assert result["owner_result"] == {"accepted": True, "action": action_id}
            assert runtime._controls("program-cycle.1").retry_count("move.1") == 1
            return
        with pytest.raises(ProgramRunnerError) as rejected:
            runtime.apply_recovery_action(request)
        if case in {"rejected", "incomplete"}:
            # Nothing released: the tree stays paused, the cycle coherent.
            assert calls == [("retry_failure_pause", owner_action)]
            assert journal.read().state == "running"
            assert not journal.read().reconciliation_required
            assert rejected.value.code == (
                "program_motion_resume_incomplete"
                if case == "incomplete"
                else "cycle_recovery_action_rejected"
            )
        else:
            assert rejected.value.code == "program_cycle_reconciliation_required"
            assert journal.read().reconciliation_required
    finally:
        runtime.shutdown()
