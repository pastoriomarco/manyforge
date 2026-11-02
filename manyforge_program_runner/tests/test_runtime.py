"""Production Program-runner admission and lifecycle glue."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from threading import Barrier, Event, Thread
from types import SimpleNamespace
from typing import Any, Callable, Mapping

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_availability import AvailabilityFact, NodeExecutionReadiness
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    PersistentValueUncertainError,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.orchestration_values import (
    ProgramValueStore,
)
from manyforge_behavior.program import Program
from manyforge_behavior.program_run_owner import (
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
)
from manyforge_behavior.orchestration_values import ProgramValueUncertainError
from manyforge_behavior.program_scheduling import ProgramSchedulingError
from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ProgramFullStopResetRequest,
    ProgramFullStopResumeRequest,
    ProgramRunStartRequest,
    ProgramManualHandoverRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
    ProgramSelectionResumeRequest,
    ProgramSelectionStartRequest,
    RuntimeBlackboardWriteRequest,
)
from manyforge_behavior.kernel_client import KernelClient
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.interrupt_state_mirror import (
    InterruptRuleStateMirror,
    RuleCatalog,
)
from manyforge_program_runner.persistent_keys import (
    CommittedActivation,
    PersistentKeyRegister,
)
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_cycle import ProgramCycleFrameDTO
from manyforge_headless_api.cell_admission import (
    CellAdmissionError,
    cell_admission_owner_epoch_reconciliation_evidence_digest,
)
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.runtime_controls import (
    ProgramRuntimeControls,
)
from manyforge_program_runner.models import (
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
    ProgramSelectionStopRequest,
)
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_ROOT = Path(__file__).resolve().parents[2]
_ACTIVE_STATES = {
    "running",
    "intervention_required",
    "full_stopping",
    "full_stopped",
    "stopping",
    "finalizing",
}


def test_runner_converts_headless_producer_without_relabeling_it() -> None:
    source = HeadlessClientError(
        "stale_generation",
        "owner refused",
        producer="kernel",
    )

    translated = ProgramRuntime._headless_error(source)

    assert translated.code == "stale_generation"
    assert translated.producer == "kernel"


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


def _timer_only_program() -> dict[str, Any]:
    return {"scene": [], "tree": {"node_id": "timer", "instance_id": "timer.1",
                                 "parameters": {"duration_s": 5.0}, "children": []}}


class _Headless:
    """Construction-complete transport whose unused effects fail loudly."""

    _PORT_METHODS = {
        "acknowledge_completed",
        "authoring_admission",
        "commit_scene_transaction",
        "scene_transaction_status",
        "commit_program_model_attempt",
        "submit",
        "manager_graph",
        "read",
        "reset",
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
        # CONTRACTS §10 RunReadiness and §5 CellLatch, as served by the API.
        self.readiness: dict[str, Any] = {
            "can_execute": True, "can_resume": True, "motion_clear": True, "blocked_reasons": []}
        self.latch: dict[str, Any] = {"latched": False, "causes": []}
        self.readiness_reads = 0
        self.rule_resets = 0
        self.rule_activations: list[tuple[str, bool]] = []
        self.outside_clears: list[tuple[str, str]] = []
        self.output_writes: list[tuple[str, Any]] = []

    def apply_tool_payload(self) -> None:
        pass

    def reset_active_tcp(self) -> dict[str, Any]:
        # A selected scene reload resets Active TCPs; no robot store is bound here.
        return {"ok": True, "robots": []}

    def program_applied(self) -> dict[str, Any]:
        self.applied_reads += 1
        return {
            "scenario_id": "cell-pick",
            "project_path": "/projects/cell-pick.scenario",
            "source_document_digest": _DIGEST_A,
            "program_revision": _DIGEST_A,
            "program": _timer_only_program(),
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

    def request(self, method: str, path: str) -> dict[str, Any]:
        assert method == "GET"
        if path == "/api/program/readiness":
            self.readiness_reads += 1
            return deepcopy(self.readiness)
        assert path == "/api/cell/latch"
        return deepcopy(self.latch)

    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "owner_epoch": "execution.epoch.1",
                "program_binding_id": None,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "active_publication_generation": 1,
            },
        }

    def targets(self) -> dict[str, Any]:
        """The execution owner's targets; tests set rows in ``target_components``."""
        return {
            "ok": True,
            "targets": {
                "components": deepcopy(getattr(self, "target_components", [])),
                "schema_version": "manyforge.execution_targets.v3",
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

    def reset_rule_activation(self) -> dict[str, Any]:
        self.rule_resets += 1
        return {"ok": True, "command": "reset_rule_activation"}

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]:
        self.rule_activations.append((rule_id, active))
        return {"ok": True, "command": "set_rule_active"}

    def clear_outside_change(self, component_id: str, signal_name: str) -> dict[str, Any]:
        self.outside_clears.append((component_id, signal_name))
        return {"ok": True, "command": "clear_outside_change"}

    def set_io_output(self, signal_id: str, value: Any, *, origin: str) -> dict[str, Any]:
        assert origin == "program"
        self.output_writes.append((signal_id, value))
        return {"ok": True, "command": "set_io_output", "result": {"outcome": "succeeded"}}

    def __getattr__(self, name: str):
        if name not in self._PORT_METHODS:
            raise AttributeError(name)

        def unexpected(*_args: object, **_kwargs: object) -> None:
            raise AssertionError(f"unexpected effectful headless call: {name}")

        return unexpected


def test_run_readiness_refuses_with_its_first_reason() -> None:
    """CONTRACTS §10: the Runner reads RunReadiness and computes nothing."""

    headless = _Headless()
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]
    runtime._require_run_readiness("can_execute")
    headless.readiness = {
        "can_execute": False,
        "can_resume": True,
        "blocked_reasons": [
            {"code": "component_not_enabled", "message": "robot.1 is prepared",
             "component_id": "robot.1"},
            {"code": "rule_holds", "message": "guard holds", "rule_id": "guard"},
        ],
    }
    with pytest.raises(ProgramRunnerError) as refused:
        runtime._require_run_readiness("can_execute")
    assert refused.value.code == "component_not_enabled"
    assert refused.value.details == {"component_id": "robot.1"}
    runtime._require_run_readiness("can_resume")
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "rule_holds", "message": "guard holds", "rule_id": "guard"}],
    }
    with pytest.raises(ProgramRunnerError) as held:
        runtime._require_run_readiness("can_resume")
    assert held.value.code == "rule_holds"
    assert held.value.details == {"rule_id": "guard"}
    # A running run with its motion group held: Execute and a stopped run's
    # Resume are refused (run_in_progress), the held group's Run/Resume is not.
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "motion_clear": True,
        "blocked_reasons": [{"code": "run_in_progress", "message": "A Program run is in progress"}],
    }
    runtime._require_run_readiness("motion_clear")
    # motion_clear is refused with its own reason, never a rule hold (the
    # kernel refuses a hold's scope itself).
    headless.readiness = {
        "can_execute": False, "can_resume": False, "motion_clear": False,
        "blocked_reasons": [
            {"code": "rule_holds", "message": "guard holds", "rule_id": "guard"},
            {"code": "input_not_fresh", "message": "door has no fresh value", "signal_id": "door"},
        ],
    }
    with pytest.raises(ProgramRunnerError) as stale:
        runtime._require_run_readiness("motion_clear")
    assert stale.value.code == "input_not_fresh"


def test_blocked_readiness_rejects_program_before_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    headless = _Headless()
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "cell_latched", "message": "robot.1 is in fault",
                             "component_id": "robot.1"}],
    }
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
    assert refused.value.code == "cell_latched"
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

    @property
    def operation_failures(self) -> tuple[dict[str, Any], ...]:
        # ProgramRunSnapshot's typed failure rows, as published by to_dict.
        return tuple(deepcopy(self._value.get("operation_failures", [])))

    def to_dict(self) -> dict[str, Any]:
        return deepcopy(self._value)


class _ValueStore:
    """The Run value store as the runtime uses it without global rules."""


class _Owner:
    def __init__(self) -> None:
        self.rows: dict[str, _Snapshot] = {}
        self.reads = 0
        self.attention_refusal: ProgramRunOwnerError | None = None
        self.attention_checks: list[tuple[str, str, str]] = []

    def check_motion_attention(
        self, program_run_id: str, node_instance_id: str, request_id: str
    ) -> None:
        self.attention_checks.append((program_run_id, node_instance_id, request_id))
        if self.attention_refusal is not None:
            raise self.attention_refusal

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

    def value_store(self, _program_run_id: str) -> "_ValueStore":
        return _ValueStore()

    def read(self, program_run_id: str) -> _Snapshot:
        self.reads += 1
        return self.rows[program_run_id]

    def intervention(self, _program_run_id: str, *, node_attempt_id=None):
        return None

    def failure_holds(self, _program_run_id: str):
        return ()

    def hold_motion_groups(self, _program_run_id: str, _node_instance_id: str):
        return ("all",), "all"

    def take_node_history_events(self, _program_run_id: str):
        return ()

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
                "request_id": "reset.after-restart", "reload_initial_scene": True,
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


class _PreparedRunAdmission:
    def __init__(self) -> None:
        self.program = SimpleNamespace(
            to_dict=_timer_only_program, interrupt_condition_keys=(), rule_activation_rule_ids=()
        )
        # The run reads it for blackboard-key publication; this fixture has none.
        self.program_binding = None
        self.project_path = "/projects/cell-pick.scenario"

    def port_context(self, program_run_id: str) -> object:
        return SimpleNamespace(
            program_run_id=program_run_id,
            program_id="cell-pick",
            program_revision=_DIGEST_A,
            program_binding_id=_DIGEST_B,
            retained_bundle_id=_DIGEST_A,
            graph_revision=_DIGEST_B,
        )


def test_empty_scenario_clear_uses_current_apply_and_not_program_binding(monkeypatch, tmp_path):
    class IdleAdmission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    class EmptyHeadless(_Headless):
        def program_applied_status(self):
            return {"program_id": None, "record_digest": _DIGEST_B,
                    "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B}

        def committed_scenario_activation(self):
            return {"apply_id": "apply.1", "apply_generation": 1,
                    "record_digest": _DIGEST_B, "scene_initialization": "initialize",
                    "origin": {"kind": "scenario"}}

    headless = EmptyHeadless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.clear.1"),
        headless_client=headless, execution_transport=_Headless(), admission_client=IdleAdmission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.clear.1"),
    )
    calls = []
    def clear(**kwargs):
        calls.append(kwargs)
        assert kwargs["is_current"]()
        return {"state": "succeeded", "last_observed_remaining": [], "refused": []}
    monkeypatch.setattr(runtime._scene, "clear_applied_scene", clear)
    runtime.start()
    result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
        "request_id": "apply-scene.1", "request_owner_epoch": "runner.clear.1",
        "source_label": "headless_apply_completion", "correlation_id": None,
        "expected_applied_record_digest": _DIGEST_B, "expected_admission_generation": 1,
        "clear_empty_scene": True,
    }))
    assert result["prepared_scene"]["state"] == "succeeded"
    assert calls[0]["retained_bundle_id"] == _DIGEST_A
    assert headless.applied_reads == 0
    runtime.shutdown()


def test_direct_empty_clear_accepts_bootstrap_and_checks_publication(monkeypatch, tmp_path):
    class IdleAdmission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    class BootstrapHeadless(_Headless):
        generation = 1
        def manager_status(self):
            return {"source": {"available": True, "freshness": "fresh"},
                    "status": {"manager_ready": True, "runtime": {"converged": True},
                               "active": {"canonical_empty": True, "retained_bundle_id": _DIGEST_A,
                                          "graph_revision": _DIGEST_B,
                                          "publication": {"publication_generation": self.generation}}}}

    headless = BootstrapHeadless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.clear.1"),
        headless_client=headless, execution_transport=_Headless(), admission_client=IdleAdmission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.clear.1"),
    )
    def clear(**kwargs):
        assert kwargs["is_current"]()
        headless.generation = 2
        assert not kwargs["is_current"]()
        return {"state": "superseded", "last_observed_remaining": ["part"]}
    monkeypatch.setattr(runtime._scene, "clear_applied_scene", clear)
    runtime.start()
    result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
        "request_id": "clear.1", "request_owner_epoch": "runner.clear.1",
        "source_label": "composer_ui", "correlation_id": None,
        "expected_applied_record_digest": None, "expected_admission_generation": 1,
        "clear_empty_scene": True, "expected_publication_generation": 1,
    }))
    assert result["prepared_scene"]["state"] == "superseded"
    assert headless.applied_reads == 0
    runtime.shutdown()


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
    status = runtime.cycle_status()
    assert status["state"] == "idle_after_reset"
    availability = {row["command_kind"]: row for row in status["allowed_commands"]}
    assert availability["execution_recovery"]["allowed"] is False
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
        cycle = runtime._journal.read()
        runtime.shutdown()
    assert not starter.is_alive()
    assert len(failures) == 1
    assert isinstance(failures[0], ProgramRunnerError)
    assert failures[0].code == "program_run_superseded_by_stop"
    assert failures[0].details["cause_code"] == "program_run_superseded_by_stop"
    assert failures[0].details["program_run_id"] == "program-cycle.1"
    assert failures[0].details["needs_reset"] is True
    assert failures[0].details["stopped_by_operator"] is True
    assert "stopped during start" in str(failures[0])
    assert cycle.state == "terminal_awaiting_reset"
    assert cycle.failure is None
    assert cycle.start_settlement["kind"] == "proven_no_effect"
    assert cycle.start_settlement["program_run_id"] == "program-cycle.1"
    assert runtime._starting_kernel_owner_epoch is None


def test_applied_scene_preparation_precedes_run_and_run_never_checks_scene(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    events: list[str] = []

    class OrderedAdmission(_Admission):
        generation = 1

        def get(self) -> dict[str, Any]:
            events.append(f"admission:{self.generation}")
            return {"admission_generation": self.generation, "authoring_allowed": True}

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

    runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
        "request_id": "applied.scene", "request_owner_epoch": "composer.1",
        "source_label": "composer", "correlation_id": None,
        "expected_applied_record_digest": _DIGEST_B,
        "expected_admission_generation": 1,
    }))
    assert events == ["admission:1", "prepare", "verify", "admission:3"]
    events.clear()
    for method in ("prepare_initial_scene", "verify_initial_scene"):
        monkeypatch.setattr(runtime._scene, method,
                            lambda *_args: pytest.fail("Run touched the Scene"))
    runtime.start_run(_start_request())

    assert events == ["admission:3", "admission:3", "claim"]
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



@pytest.mark.parametrize("changed_program", [False, True])
def test_immediate_run_after_idle_reset_uses_server_generation_and_exact_program(
    monkeypatch: pytest.MonkeyPatch, tmp_path, changed_program: bool,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    class Admission(_Admission):
        generation = 36
        starting = False
        reads = 0
        def get(self):
            if self.starting:
                self.reads += 1
                if self.reads == 2:
                    # The Manager observes the just-published Reset cycle
                    # between the initial and fresh server admission reads.
                    self.generation = 38
            return {"admission_generation": self.generation, "authoring_allowed": True}
        def acquire(self, intent):
            assert intent["expected_admission_generation"] == self.generation == 38
            claim = super().acquire(intent)
            claim["acquired_admission_generation"] = self.generation
            return claim
    admission = Admission()
    headless = _Headless()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.reset-run.1")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.reset-run.1"),
        headless_client=headless, execution_transport=_Headless(), admission_client=admission,
        cycle_journal=journal,
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    monkeypatch.setattr(runtime._scene, "prepare_initial_scene",
                        lambda *_args: {"schema_version": "manyforge.prepared_program_scene.v1"})
    monkeypatch.setattr(runtime._scene, "verify_initial_scene", lambda *_args: None)
    runtime.start()
    try:
        for generation in (1, 2):
            result = runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
                "request_id": "idle.reset." + str(generation), "request_owner_epoch": "operator.1",
                "source_label": "operator_ui", "program_run_id": None,
                "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": generation,
                "expected_claim_generation": None, "terminal_reference": None,
                "settlement_evidence_digest": None,
            }))
        assert result["admission"]["admission_generation"] == 36
        assert journal.read().cycle_generation == 3
        for method in ("prepare_initial_scene", "verify_initial_scene"):
            monkeypatch.setattr(runtime._scene, method,
                                lambda *_args: pytest.fail("Run touched the Scene"))
        admission.starting = True
        if changed_program:
            original = headless.program_applied
            def another_client_applied():
                applied = original()
                applied["source"]["record_digest"] = _DIGEST_A
                return applied
            monkeypatch.setattr(headless, "program_applied", another_client_applied)
            with pytest.raises(ProgramRunnerError) as refused:
                runtime.start_run(_start_request())
            assert refused.value.code == "program_run_applied_selection_stale"
            assert not admission.claims
            assert journal.read().state == "idle_after_reset"
        else:
            runtime.start_run(_start_request())
            assert journal.read().state == "running"
            assert len(admission.claims) == 1
            assert admission.claims[0]["expected_admission_generation"] == 38
    finally:
        runtime.shutdown()


class _CarriedObjectLossStore:
    def __init__(
        self,
        declarations: Mapping[str, Any],
        *,
        failure: Exception | None = None,
    ) -> None:
        self.program = SimpleNamespace(blackboard_keys=dict(declarations))
        self.writes: list[tuple[str, bool, Mapping[str, Any]]] = []
        self.failure = failure

    def write(self, key: str, value: bool, *, metadata: Mapping[str, Any]) -> None:
        if self.failure is not None:
            raise self.failure
        self.writes.append((key, value, dict(metadata)))


class _CarriedObjectLossMirrorFake:
    def __init__(
        self, snapshot: dict[str, object], catalog: RuleCatalog | None
    ) -> None:
        self._snapshot = snapshot
        self._catalog = catalog

    def carried_object_last_losses(self) -> object:
        return self._snapshot["last_losses"]

    def carried_object_lost_signals(self) -> object:
        return self._snapshot["rows"]

    def catalog(self) -> RuleCatalog | None:
        return self._catalog

    def poll(self) -> None:
        return None


def _carried_object_last_loss(
    event_id: str,
    *,
    key: str = "part_lost",
) -> dict[str, Any]:
    return {
        "command_target_id": "gripper.command",
        "controllable_component_id": "robot.1",
        "loss_event_id": event_id,
        "set_key": key,
    }


def _carried_object_loss_runtime(
    initial_losses: object,
    stores: Mapping[str, _CarriedObjectLossStore] | _CarriedObjectLossStore,
) -> tuple[ProgramRuntime, dict[str, object]]:
    from threading import Lock

    snapshot: dict[str, object] = {"last_losses": initial_losses, "rows": ()}
    runtime = object.__new__(ProgramRuntime)
    runtime._interrupt_state_run_id = None  # noqa: SLF001
    runtime._carried_object_lost_run_id = None  # noqa: SLF001
    runtime._carried_object_lost_seen_event_ids = set()  # noqa: SLF001
    runtime._grasp_lost_mirror_lock = Lock()  # noqa: SLF001
    runtime._interrupt_state_mirror = _CarriedObjectLossMirrorFake(  # noqa: SLF001
        snapshot, catalog=None
    )
    runtime._carried_object_lost_run_active = False  # noqa: SLF001
    run_ids = tuple(stores) if isinstance(stores, Mapping) else ("run.1",)
    runtime._runtime_controls_lock = Lock()  # noqa: SLF001
    runtime._runtime_controls = {  # noqa: SLF001
        run_id: ProgramRuntimeControls(
            {
                "program_run_id": run_id,
                "program_id": "program.1",
                "program_revision": _DIGEST_A,
                "program_binding_id": _DIGEST_B,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "node_environment_digest": _DIGEST_A,
            }
        )
        for run_id in run_ids
    }
    runtime._bbkey_publishers_lock = Lock()  # noqa: SLF001
    runtime._bbkey_publishers = {}  # noqa: SLF001
    runtime._owner = SimpleNamespace(  # noqa: SLF001
        value_store=lambda run_id: (
            stores[run_id]
            if isinstance(stores, Mapping)
            else stores
        )
    )
    return runtime, snapshot


def _install_startable_loss_mirror(
    runtime: ProgramRuntime,
    owner: Any,
    store: ProgramValueStore | _CarriedObjectLossStore,
) -> dict[str, object]:
    snapshot: dict[str, object] = {"last_losses": (), "rows": ()}
    owner.value_store = lambda _program_run_id: store
    runtime._interrupt_state_mirror = _CarriedObjectLossMirrorFake(  # noqa: SLF001
        snapshot,
        catalog=RuleCatalog(
            "execution.epoch.1", None, 1, _DIGEST_A, _DIGEST_B, (), ()
        ),
    )
    return snapshot


def _writable_bool_declaration() -> SimpleNamespace:
    return SimpleNamespace(type="bool", access="write", expression=None)


def _real_persistent_loss_program() -> Program:
    environment = build_node_environment()
    catalog = environment.catalog
    document = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml", catalog
    ).to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "part_lost",
                "type": "bool",
                "access": "read_write",
                "persistence": "persistent",
            }
        ],
    }
    document["program_revision"] = compute_content_identity(
        deepcopy(document),
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, catalog)


def _real_persistent_loss_store(
    persistent_commit: Callable[[Any, Any, Mapping[str, Any]], None],
) -> ProgramValueStore:
    return ProgramValueStore(
        _real_persistent_loss_program(), persistent_commit=persistent_commit
    )


def test_carried_object_loss_known_last_losses_at_run_start_are_already_seen() -> None:
    loss = _carried_object_last_loss("loss.before-run")
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, _snapshot = _carried_object_loss_runtime((loss,), store)

    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert runtime._carried_object_lost_seen_event_ids == {"loss.before-run"}  # noqa: SLF001
    assert store.writes == []


def test_carried_object_lost_unknown_at_run_start_reports_first_known_losses() -> None:
    loss = _carried_object_last_loss("loss.after-unknown-start")
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, snapshot = _carried_object_loss_runtime(None, store)

    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(None)  # noqa: SLF001
    assert runtime._carried_object_lost_seen_event_ids == set()  # noqa: SLF001

    snapshot["last_losses"] = (loss,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert [write[:2] for write in store.writes] == [("part_lost", True)]
    assert store.writes[0][2] == {
        "writer": "grasp_lost",
        "loss_event_id": "loss.after-unknown-start",
    }


def test_carried_object_lost_repeated_loss_and_new_event_for_key() -> None:
    first = _carried_object_last_loss("loss.first")
    second = _carried_object_last_loss("loss.second")
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, _snapshot = _carried_object_loss_runtime((), store)
    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001

    _snapshot["last_losses"] = (first,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(None)  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    assert [write[:2] for write in store.writes] == [("part_lost", True)]

    # A known empty snapshot of current rows does not erase the durable last
    # loss. A later event for this key replaces it with a new event ID.
    _snapshot["last_losses"] = (second,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert [write[:2] for write in store.writes] == [
        ("part_lost", True),
        ("part_lost", True),
    ]
    assert runtime._carried_object_lost_seen_event_ids == {"loss.second"}  # noqa: SLF001


def test_mirror_delivers_cleared_losses_per_key_and_unknown_stays_unknown() -> None:
    first = _carried_object_last_loss("loss.first-key", key="part_lost")
    second = _carried_object_last_loss("loss.second-key", key="other_lost")
    store = _CarriedObjectLossStore(
        {
            "part_lost": _writable_bool_declaration(),
            "other_lost": _writable_bool_declaration(),
        }
    )
    runtime, _snapshot = _carried_object_loss_runtime((), store)
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel.1",
        read_events=lambda _owner, _after: {},
        on_change=lambda _states: None,
        on_carried_object_lost=runtime._mirror_carried_object_lost_signals,  # noqa: SLF001
    )
    runtime._interrupt_state_mirror = mirror  # noqa: SLF001

    def apply_state(last_losses: tuple[Mapping[str, Any], ...]) -> None:
        mirror.apply_stream_message(
            {
                "type": "snapshot" if mirror.carried_object_last_losses() is None else "state",
                "owner_epoch": "kernel.1",
                "cursor": 0,
                "reason": "subscribed",
                "state": {
                    "interrupt_rule_catalog": None,
                    "interrupt_rule_state_baseline": None,
                    "carried_object_lost_signals": [],
                    "carried_object_last_losses": list(last_losses),
                },
            }
        )

    apply_state(())
    assert mirror.carried_object_lost_signals() == ()
    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001

    # The current rows remain clear, but the per-key loss state keeps both
    # independent losses until each key is lost again.
    apply_state((first, second))
    assert mirror.carried_object_lost_signals() == ()
    assert mirror.carried_object_last_losses() == (first, second)
    assert [write[:2] for write in store.writes] == [
        ("part_lost", True),
        ("other_lost", True),
    ]
    seen = set(runtime._carried_object_lost_seen_event_ids)  # noqa: SLF001
    assert seen == {"loss.first-key", "loss.second-key"}

    mirror.apply_stream_message(
        {"type": "gap", "owner_epoch": "kernel.2", "reason": "owner_epoch_changed"}
    )
    assert mirror.carried_object_last_losses() is None
    # A known-row notification during an unknown loss state cannot erase the
    # last-known IDs or cause another write.
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    assert runtime._carried_object_lost_seen_event_ids == seen  # noqa: SLF001
    assert len(store.writes) == 2


def test_start_stages_pre_cas_loss_until_admission_request_completes(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, _admission, owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.loss-start-success.1",
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    state = _install_startable_loss_mirror(runtime, owner, store)
    original_transition = journal.transition
    intercepted: list[bool] = []
    loss = _carried_object_last_loss("loss.before-running-cas")

    def transition(**kwargs: Any) -> None:
        if kwargs.get("state") == "running":
            assert journal.read().state == "starting"
            assert runtime._carried_object_lost_run_id == "program-cycle.1"  # noqa: SLF001
            assert runtime._interrupt_state_run_id is None  # noqa: SLF001  (SX-261: not exposed before admission)
            assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
            state["last_losses"] = (loss,)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert store.writes == []
            intercepted.append(True)
        original_transition(**kwargs)

    original_complete_request = journal.complete_request
    completed: list[bool] = []

    def complete_request(**kwargs: Any) -> Any:
        # SX-263: the staged loss is still unwritten when admission completes.
        assert store.writes == []
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
        completed.append(True)
        return original_complete_request(**kwargs)

    monkeypatch.setattr(journal, "transition", transition)
    monkeypatch.setattr(journal, "complete_request", complete_request)
    try:
        result = runtime.start_run(_start_request())

        assert intercepted == [True]
        assert completed == [True]
        assert result["state"] == "running"
        assert journal.read().state == "running"
        assert runtime._carried_object_lost_run_active is True  # noqa: SLF001
        assert [write[:2] for write in store.writes] == [("part_lost", True)]
        assert runtime._carried_object_lost_seen_event_ids == {  # noqa: SLF001
            "loss.before-running-cas"
        }
    finally:
        runtime.shutdown()


def test_start_complete_request_failure_never_writes_the_key_and_retires_binding(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """SX-263: a raising admission completion leaves the staged loss unwritten."""

    runtime, journal, _admission, owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.loss-start-complete-fails.1",
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    state = _install_startable_loss_mirror(runtime, owner, store)
    original_transition = journal.transition
    staged: list[bool] = []

    def transition(**kwargs: Any) -> None:
        if kwargs.get("state") == "running":
            state["last_losses"] = (_carried_object_last_loss("loss.before-failed-complete"),)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            staged.append(True)
        original_transition(**kwargs)

    original_complete_request = journal.complete_request
    bindings_at_complete: list[str | None] = []

    def fail_complete_request(**kwargs: Any) -> Any:
        # Only the admission completion fails; the failed-start
        # terminalization records its own result through the journal.
        bindings_at_complete.append(runtime._carried_object_lost_run_id)  # noqa: SLF001
        if len(bindings_at_complete) == 1:
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert store.writes == []
            raise RuntimeError("admission completion failed")
        return original_complete_request(**kwargs)

    monkeypatch.setattr(journal, "transition", transition)
    monkeypatch.setattr(journal, "complete_request", fail_complete_request)
    try:
        with pytest.raises(ProgramRunnerError) as raised:
            runtime.start_run(_start_request())

        assert raised.value.code == "program_run_started_then_failed"
        assert raised.value.details["cause_code"] == "program_run_admission_failed"
        assert raised.value.details["cause_message"] == "admission completion failed"
        assert staged == [True]
        assert bindings_at_complete[0] == "program-cycle.1"
        assert store.writes == []
        assert runtime._carried_object_lost_run_id is None  # noqa: SLF001
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
        # A later known loss state cannot reach the retired binding.
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert store.writes == []
    finally:
        runtime.shutdown()


def test_stop_won_running_cas_discards_inactive_loss_binding(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, _admission, owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.loss-start-stop-cas.1",
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    state = _install_startable_loss_mirror(runtime, owner, store)
    original_transition = journal.transition
    stop_acceptances: list[dict[str, Any]] = []
    loss = _carried_object_last_loss("loss.before-stop-won-cas")
    monkeypatch.setattr(runtime._native.scope, "request_stop", lambda *_args, **_kwargs: None)

    def transition(**kwargs: Any) -> None:
        if kwargs.get("state") == "running" and not stop_acceptances:
            assert runtime._carried_object_lost_run_id == "program-cycle.1"  # noqa: SLF001
            assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
            state["last_losses"] = (loss,)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert store.writes == []
            stop_acceptances.append(
                runtime.stop_run(
                    "program-cycle.1",
                    ProgramRunStopRequest.model_validate(
                        {
                            "request_id": "stop.before-running-cas",
                            "request_owner_epoch": "composer.epoch.1",
                            "source_label": "operator_ui",
                            "correlation_id": None,
                            "program_run_id": "program-cycle.1",
                            "expected_owner_epoch": runtime.owner_epoch,
                            "reason": "Stop wins the Running CAS",
                        }
                    ),
                )
            )
            assert journal.read().state == "stopping"
        original_transition(**kwargs)

    monkeypatch.setattr(journal, "transition", transition)
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.start_run(_start_request())

        assert stop_acceptances and stop_acceptances[0]["state"] == "stopping"
        assert journal.read().state == "terminal_awaiting_reset"
        assert store.writes == []
        assert runtime._carried_object_lost_run_id is None  # noqa: SLF001
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
    finally:
        runtime.shutdown()


def test_run_start_is_refused_before_claim_while_a_robot_has_no_end_effector_tcp(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """B-run-1: the execution owner reports a robot without an End-effector
    TCP (a null Robot Control TCP); Execute is refused, typed, before any
    claim, and the cycle stays idle."""
    runtime, journal, admission, _owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.no-end-effector-tcp.1",
        finish_cycle=False,
    )
    motion = {
        "command_target_id": "arm",
        "operation_capability_id": "execution.trajectory.execute",
        "presentation": "motion",
        "command": {"tcp_frame_role_id": None},
    }
    runtime._kernel._transport.target_components = [  # noqa: SLF001
        {"controllable_component_id": "lite6_robot", "targets": [motion]},
    ]
    try:
        with pytest.raises(ProgramRunnerError) as raised:
            runtime.start_run(_start_request())

        assert raised.value.code == "program_end_effector_tcp_unavailable"
        assert raised.value.details["controllable_component_id"] == "lite6_robot"
        assert admission.claims == []
        assert journal.read().state == "idle_after_reset"

        # With an End-effector TCP the same Run starts.
        motion["command"]["tcp_frame_role_id"] = "tcp"
        runtime._kernel._transport.target_components = [  # noqa: SLF001
            {"controllable_component_id": "lite6_robot", "targets": [motion]},
        ]
        result = runtime.start_run(_start_request(request_id="request.start.2"))
        assert result["state"] == "running"
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("terminalization", ["normal", "early_return", "raises"])
def test_post_cas_start_failure_discards_inactive_binding_on_all_exits(
    monkeypatch: pytest.MonkeyPatch, tmp_path, terminalization: str
) -> None:
    runtime, journal, _admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        f"runner.loss-start-failed-{terminalization}.1",
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    state = _install_startable_loss_mirror(runtime, owner, store)

    def fail_schedule(program_run_id: str) -> None:
        assert program_run_id == "program-cycle.1"
        assert journal.read().state == "running"
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
        state["last_losses"] = (
            _carried_object_last_loss(f"loss.after-cas-{terminalization}"),
        )
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert store.writes == []
        raise RuntimeError("host schedule failed after the Running CAS")

    monkeypatch.setattr(host, "schedule", fail_schedule)
    if terminalization == "early_return":
        monkeypatch.setattr(
            runtime,
            "_terminalize_failed_start",
            lambda *_args, **_kwargs: None,
        )
    elif terminalization == "raises":
        def fail_terminalization(*_args: Any, **_kwargs: Any) -> None:
            raise RuntimeError("terminalization failed")

        monkeypatch.setattr(runtime, "_terminalize_failed_start", fail_terminalization)

    try:
        with pytest.raises(RuntimeError if terminalization == "raises" else ProgramRunnerError):
            runtime.start_run(_start_request())

        assert store.writes == []
        assert runtime._carried_object_lost_run_id is None  # noqa: SLF001
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
        if terminalization == "normal":
            assert journal.read().state == "terminal_awaiting_reset"
        assert not runtime._carried_object_lost_seen_event_ids  # noqa: SLF001
    finally:
        runtime.shutdown()


def test_loss_writes_during_stopping_and_terminal_until_reset(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, _admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.loss-stop-terminal.1",
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore(
        {
            "part_lost": _writable_bool_declaration(),
            "other_lost": _writable_bool_declaration(),
            "third_lost": _writable_bool_declaration(),
        }
    )
    state = _install_startable_loss_mirror(runtime, owner, store)
    try:
        runtime.start_run(_start_request())
        assert runtime._carried_object_lost_run_active is True  # noqa: SLF001

        original_stop_run = host.stop_run
        during_stop = _carried_object_last_loss("loss.during-stopping", key="part_lost")

        def stop_while_loss_arrives(
            program_run_id: str, *, reason: str | None, stop_request_id: str | None = None
        ) -> _Snapshot:
            assert journal.read().state == "stopping"
            state["last_losses"] = (during_stop,)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert [write[:2] for write in store.writes] == [("part_lost", True)]
            return original_stop_run(
                program_run_id, reason=reason, stop_request_id=stop_request_id
            )

        monkeypatch.setattr(host, "stop_run", stop_while_loss_arrives)
        stopped = runtime.stop_run(
            "program-cycle.1",
            ProgramRunStopRequest.model_validate(
                {
                    "request_id": "stop.loss-terminal",
                    "request_owner_epoch": "composer.epoch.1",
                    "source_label": "operator_ui",
                    "correlation_id": None,
                    "program_run_id": "program-cycle.1",
                    "expected_owner_epoch": runtime.owner_epoch,
                    "reason": "test stop lifecycle delivery",
                }
            ),
        )
        assert stopped["state"] == "stopped"
        assert journal.read().state == "terminal_awaiting_reset"

        after_terminal = _carried_object_last_loss(
            "loss.after-terminal", key="other_lost"
        )
        state["last_losses"] = (during_stop, after_terminal)
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert [write[:2] for write in store.writes] == [
            ("part_lost", True),
            ("other_lost", True),
        ]
        assert runtime._carried_object_lost_run_id == "program-cycle.1"  # noqa: SLF001

        # The explicit Run Reset retires the consumer binding.
        runtime._reset_bbkey_publication("program-cycle.1")  # noqa: SLF001
        state["last_losses"] = (
            during_stop,
            after_terminal,
            _carried_object_last_loss("loss.after-reset", key="third_lost"),
        )
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert len(store.writes) == 2
        assert runtime._carried_object_lost_run_id is None  # noqa: SLF001
    finally:
        runtime.shutdown()


def test_terminal_carried_loss_restores_from_persistent_register_for_next_run(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, _admission, owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
        "runner.loss-terminal-persistent.1",
        finish_cycle=False,
    )
    program = _real_persistent_loss_program()
    project_path = "/projects/terminal-loss-persistence.scenario"
    register = PersistentKeyRegister(tmp_path / "persistent-keys.json")
    activation_digest = "sha256:" + "c" * 64
    register.reconcile(
        CommittedActivation(
            record_digest=activation_digest,
            project_path=project_path,
            declarations=program.blackboard_keys,
            committed_applies={activation_digest: project_path},
        )
    )
    current_store = ProgramValueStore.from_program(
        program,
        project_path=project_path,
        register=register,
        program_run_id="program-cycle.1",
    )
    state = _install_startable_loss_mirror(runtime, owner, current_store)
    try:
        runtime.start_run(_start_request())
        stopped = runtime.stop_run(
            "program-cycle.1",
            ProgramRunStopRequest.model_validate(
                {
                    "request_id": "stop.loss-terminal-persistent",
                    "request_owner_epoch": "composer.epoch.1",
                    "source_label": "operator_ui",
                    "correlation_id": None,
                    "program_run_id": "program-cycle.1",
                    "expected_owner_epoch": runtime.owner_epoch,
                    "reason": "test terminal persistent loss",
                }
            ),
        )
        assert stopped["state"] == "stopped"
        assert journal.read().state == "terminal_awaiting_reset"

        state["last_losses"] = (
            _carried_object_last_loss(
                "loss.after-terminal-persistent", key="part_lost"
            ),
        )
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert current_store.read("part_lost") is True

        next_run_store = ProgramValueStore.from_program(
            program,
            project_path=project_path,
            register=register,
            program_run_id="program-cycle.2",
        )
        assert next_run_store.read("part_lost") is True
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(
    ("key", "declarations"),
    [
        ("undeclared", {}),
        (
            "read_only",
            {
                "read_only": SimpleNamespace(
                    type="bool", access="read", expression=None
                )
            },
        ),
        (
            "wrong_type",
            {
                "wrong_type": SimpleNamespace(
                    type="int", access="write", expression=None
                )
            },
        ),
        (
            "computed",
            {
                "computed": SimpleNamespace(
                    type="bool", access="write", expression="true"
                )
            },
        ),
    ],
)
def test_carried_object_lost_invalid_key_is_logged_and_seen(
    caplog: pytest.LogCaptureFixture,
    key: str,
    declarations: Mapping[str, Any],
) -> None:
    row = _carried_object_last_loss("loss.invalid-key", key=key)
    store = _CarriedObjectLossStore(declarations)
    runtime, _snapshot = _carried_object_loss_runtime((), store)
    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001

    _snapshot["last_losses"] = (row,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert store.writes == []
    assert runtime._carried_object_lost_seen_event_ids == {"loss.invalid-key"}  # noqa: SLF001
    assert "not a writable bool key" in caplog.text


def test_old_attachment_loss_during_new_run_sets_current_key() -> None:
    prior_loss = _carried_object_last_loss("loss.first-run")
    loss_during_new_run = _carried_object_last_loss("loss.second-run")
    declarations = {"part_lost": _writable_bool_declaration()}
    old_run_store = _CarriedObjectLossStore(declarations)
    current_run_store = _CarriedObjectLossStore(declarations)
    runtime, snapshot = _carried_object_loss_runtime(
        (), {"run.1": old_run_store, "run.2": current_run_store}
    )

    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001
    snapshot["last_losses"] = (prior_loss,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    assert [write[2]["loss_event_id"] for write in old_run_store.writes] == [
        "loss.first-run"
    ]

    # At the next Run boundary, the retained last-loss entry is already
    # baseline state. A later event on the earlier-Run attachment updates the
    # same per-key entry and belongs to Run 2.
    runtime._begin_carried_object_lost_run("run.2")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.2")  # noqa: SLF001
    snapshot["last_losses"] = (loss_during_new_run,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert [write[2]["loss_event_id"] for write in old_run_store.writes] == [
        "loss.first-run"
    ]
    assert [write[2]["loss_event_id"] for write in current_run_store.writes] == [
        "loss.second-run"
    ]


def test_carried_object_lost_uncertain_write_is_seen_and_logged(
    caplog: pytest.LogCaptureFixture,
) -> None:
    attempts: list[str] = []

    def uncertain_commit(_declaration, _value, _metadata) -> None:
        attempts.append("write")
        raise PersistentValueUncertainError("directory sync failed")

    store = _real_persistent_loss_store(uncertain_commit)
    runtime, snapshot = _carried_object_loss_runtime((), store)
    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001

    snapshot["last_losses"] = (_carried_object_last_loss("loss.uncertain"),)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    controls = runtime._runtime_controls["run.1"]  # noqa: SLF001
    assert store.read("part_lost") is True
    assert attempts == ["write"]
    assert runtime._carried_object_lost_seen_event_ids == {  # noqa: SLF001
        "loss.uncertain"
    }
    assert controls.snapshot()["audit"] == []
    assert "grasp-lost key part_lost could not be written" in caplog.text


def test_carried_object_lost_failed_write_is_logged_seen_and_other_keys_continue(
    caplog: pytest.LogCaptureFixture,
) -> None:
    store = _CarriedObjectLossStore(
        {
            "part_lost": _writable_bool_declaration(),
            "other_lost": _writable_bool_declaration(),
        }
    )
    attempts: list[str] = []

    def write(key: str, value: bool, *, metadata: Mapping[str, Any]) -> None:
        attempts.append(key)
        if key == "part_lost":
            raise RuntimeError("persistent register rejected the write")
        store.writes.append((key, value, dict(metadata)))

    store.write = write  # type: ignore[method-assign]
    runtime, snapshot = _carried_object_loss_runtime((), store)
    runtime._begin_carried_object_lost_run("run.1")  # noqa: SLF001
    runtime._activate_carried_object_lost_run("run.1")  # noqa: SLF001

    losses = (
        _carried_object_last_loss("loss.failed", key="part_lost"),
        _carried_object_last_loss("loss.continues", key="other_lost"),
    )
    snapshot["last_losses"] = losses
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    assert "grasp-lost key part_lost could not be written" in caplog.text

    # The known snapshot remains seen after a failed write, and a second
    # valid key in that same snapshot is still handled.
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert runtime._carried_object_lost_seen_event_ids == {  # noqa: SLF001
        "loss.failed",
        "loss.continues",
    }
    assert attempts == ["part_lost", "other_lost"]
    assert [write[:2] for write in store.writes] == [("other_lost", True)]


def test_scene_preparation_failure_blocks_run_until_idle_reset_converges(
    monkeypatch: pytest.MonkeyPatch, tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    class IdleAdmission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    admission = IdleAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.prepare-fail.1"),
        headless_client=_Headless(), execution_transport=_Headless(),
        admission_client=admission,
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.prepare-fail.1"),
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    monkeypatch.setattr(runtime._scene, "prepare_initial_scene",
                        lambda *_args: (_ for _ in ()).throw(RuntimeError("scene unavailable")))
    runtime.start()
    preparation = ProgramScenePreparationRequest.model_validate({
        "request_id": "apply.scene.failure", "request_owner_epoch": "composer.1",
        "source_label": "composer", "correlation_id": None,
        "expected_applied_record_digest": _DIGEST_B,
        "expected_admission_generation": 1,
    })
    with pytest.raises(ProgramRunnerError, match="scene unavailable") as failed:
        runtime.prepare_applied_scene(preparation)
    assert failed.value.code == "program_scene_preparation_failed"
    status = runtime.cycle_status()
    assert status["state"] == "idle_after_reset"
    assert status["failure"]["code"] == "program_scene_preparation_failed"
    commands = {row["command_kind"]: row for row in status["allowed_commands"]}
    assert commands["program_run"]["allowed"] is False
    assert commands["program_run"]["disabled_reason"] == "program_scene_preparation_failed"
    assert commands["program_reset"]["allowed"] is True
    with pytest.raises(ProgramRunnerError) as rejected:
        runtime.start_run(_start_request())
    assert rejected.value.code == "program_scene_preparation_failed"
    assert admission.claims == []
    objects = {"changed": True}
    def restore(_context, _scene, **_kwargs):
        assert runtime.cycle_status()["state"] == "resetting"
        objects.clear()
        objects["program_object"] = True
        return {"schema_version": "manyforge.prepared_program_scene.v1"}
    monkeypatch.setattr(runtime._scene, "prepare_initial_scene", restore)
    monkeypatch.setattr(runtime._scene, "verify_initial_scene", lambda *_args: None)
    reset = ProgramCycleResetRequest.model_validate({
        "request_id": "idle.reset", "reload_initial_scene": True, "request_owner_epoch": "composer.1",
        "source_label": "operator_ui", "correlation_id": None,
        "program_run_id": None, "expected_owner_epoch": runtime.owner_epoch,
        "expected_cycle_generation": 1, "expected_claim_generation": None,
        "terminal_reference": None, "settlement_evidence_digest": None,
    })
    result = runtime.reset_cycle(reset)
    assert result["program_run_id"] is None
    assert result["idle_cycle_generation"] == 2
    assert objects == {"program_object": True}
    assert runtime.cycle_status()["failure"] is None
    runtime.start_run(_start_request(request_id="after.idle.reset"))
    assert runtime.cycle_status()["state"] == "running"
    assert [claim["claim_kind"] for claim in admission.claims] == ["program_reset", "program_cycle"]
    assert len(admission.releases) == 1
    runtime.shutdown()


def _terminal_cycle_reset_runtime(
    monkeypatch, tmp_path, owner_epoch, *, finish_cycle: bool = True
):
    """A public Run/Stop cycle with deterministic owner observations."""

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", owner_epoch=owner_epoch
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch=owner_epoch
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    if not finish_cycle:
        return runtime, runtime._journal, admission, owner, host  # noqa: SLF001
    runtime.start_run(_start_request())
    stopped = runtime.stop_run(
        "program-cycle.1",
        ProgramRunStopRequest.model_validate(
            {
                "request_id": "request.stop.before-reset",
                "request_owner_epoch": "composer.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
                "program_run_id": "program-cycle.1",
                "expected_owner_epoch": runtime.owner_epoch,
                "reason": "operator stop before Reset",
            }
        ),
    )
    assert stopped["state"] == "stopped"
    assert runtime.cycle_status()["state"] == "terminal_awaiting_reset"
    return runtime, runtime._journal, admission, owner, host  # noqa: SLF001


def test_program_runtime_reset_from_terminal_partial_stop_releases_cycle_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, "runner.partial-reset.terminal"
    )
    host.contain_reset_run = lambda program_run_id, **_kwargs: owner.rows[program_run_id]
    try:
        reset = _current_reset_request(runtime, journal, "reset.partial.terminal")
        result = runtime.reset_cycle(reset)

        assert result["idle_cycle_generation"] == 2
        assert runtime.cycle_status()["state"] == "idle_after_reset"
        assert journal.pending_commands() == ()
        assert len(admission.releases) == 1
        assert admission.releases[0]["settlement"]["settlement_kind"] == "explicit_reset"
        assert host.reset_calls == ["program-cycle.1"]
        assert host.forgotten_reset_runs == ["program-cycle.1"]
        assert "program-cycle.1" not in owner.rows
    finally:
        runtime.shutdown()


def test_program_runtime_reset_retry_from_recovery_required_retires_same_cycle(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, "runner.partial-reset.retry"
    )
    containment_calls = 0

    def contain_once_then_settle(program_run_id, **_kwargs):
        nonlocal containment_calls
        containment_calls += 1
        if containment_calls == 1:
            raise ProgramRunOwnerOutcomeUnknownError(
                "program_run_stop_partial"
            )
        return owner.rows[program_run_id]

    host.contain_reset_run = contain_once_then_settle
    try:
        first = _current_reset_request(runtime, journal, "reset.partial.first")
        with pytest.raises(ProgramRunnerError) as pending:
            runtime.reset_cycle(first)
        assert pending.value.code == "program_cycle_recovery_required"
        recovery = runtime.cycle_status()
        assert recovery["state"] == "recovery_required"
        assert recovery["reset_phase"] == "stopping"
        assert not admission.releases
        assert host.reset_calls == []
        claim = journal.read().claim
        assert claim is not None
        admission.inventory = {
            "status": "held_exact",
            "kernel_owner_epoch": "execution.epoch.1",
            "held_claims": [claim],
            "current_owner_epochs": [
                {
                    "owner_role": "program_runner",
                    "owner_epoch": runtime.owner_epoch,
                }
            ],
            "matched_claim": claim,
            "matched_release": None,
            "matched_reconcile_settlement": None,
        }

        retry = _current_reset_request(runtime, journal, "reset.partial.retry")
        result = runtime.reset_cycle(retry)

        assert containment_calls == 2
        assert result["idle_cycle_generation"] == 2
        assert runtime.cycle_status()["state"] == "idle_after_reset"
        assert journal.pending_commands() == ()
        assert len(admission.releases) == 1
        assert admission.releases[0]["settlement"]["settlement_kind"] == "explicit_reset"
        assert host.stop_calls[-1] == (
            "program-cycle.1",
            "operator requested Program cycle Reset",
        )
        assert host.reset_calls == ["program-cycle.1"]
        assert host.forgotten_reset_runs == ["program-cycle.1"]
        assert "program-cycle.1" not in owner.rows
    finally:
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

    original_get = admission.get
    reads = 0
    def read_admission():
        nonlocal reads
        reads += 1
        if reads == 2:
            preparation_entered.set()
            assert continue_preparation.wait(timeout=1.0)
        return original_get()
    monkeypatch.setattr(admission, "get", read_admission)
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
            "expected_owner_epoch": "program-runner.previous",
            "reason": "operator stop overtook start",
        }
    )
    with pytest.raises(ProgramRunnerError) as wrong_path:
        runtime.stop_run("program-cycle.other", stop)
    assert wrong_path.value.code == "program_run_path_invalid"
    acceptance = runtime.stop_run("program-cycle.1", stop)
    assert acceptance["state"] == "stopping"
    assert runtime.cycle_status()["state"] == "stopping"
    continue_preparation.set()
    thread.join(timeout=1.0)

    assert not thread.is_alive()
    assert len(start_outcome) == 1
    assert isinstance(start_outcome[0], ProgramRunnerError)
    assert start_outcome[0].code == "program_run_stopped_before_start"
    assert start_outcome[0].details == {
        "program_run_id": "program-cycle.1",
        "claim_generation": None,
        "stopped_by_operator": True,
        "no_effect": True,
        "needs_reset": True,
        "cause_code": "program_run_superseded_by_stop",
        # M6 ERR: the cause's typed producer travels with it.
        "cause_producer": "program_runner",
        "cause_message": "Program Stop was accepted before Run admission completed",
    }
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


def test_run_does_not_compare_changed_authored_scene(
    monkeypatch: pytest.MonkeyPatch, tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.scene-changed.1"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=admission,
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.scene-changed.1"),
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    for method in ("prepare_initial_scene", "verify_initial_scene"):
        monkeypatch.setattr(runtime._scene, method,
                            lambda *_args: pytest.fail("Run edited or compared the Scene"))
    runtime.start()
    runtime.start_run(_start_request())
    assert runtime.cycle_status()["state"] == "running"
    assert len(admission.claims) == 1
    runtime.shutdown()


def test_host_start_without_a_result_retains_claim_for_reconciliation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
    admitted = _PreparedRunAdmission()
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
    assert headless.readiness_reads == 1
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
            "expected_selection_revision": 0,
            "group_ids": ["robot_group"],
            "reason": "initial operator motion Run",
        }
    )
    ready = headless.readiness
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "rule_holds", "message": "guard holds",
                             "rule_id": "guard"}],
    }
    blocked_start = ProgramSelectionStartRequest.model_validate({
        **motion_start.model_dump(mode="json"),
        "request_id": "request.motion-start.blocked",
    })
    with pytest.raises(ProgramRunnerError) as blocked:
        runtime.start_motion("program-cycle.1", blocked_start)
    assert blocked.value.code == "rule_holds"
    assert host.motion_start_calls == []
    headless.readiness = ready

    # N8: Run is fenced on run id, owner epoch and selection revision only.
    for request_id, run_id, changes, code in (
        ("stale-epoch", "program-cycle.1", {"expected_owner_epoch": "old"},
         "program_run_owner_stale"),
        ("stale-run", "program-cycle.9", {"program_run_id": "program-cycle.9"},
         "program_runtime_control_stale"),
    ):
        with pytest.raises(ProgramRunnerError) as stale:
            runtime.start_motion(
                run_id,
                ProgramSelectionStartRequest.model_validate({
                    **motion_start.model_dump(mode="json"),
                    "request_id": f"request.motion-start.{request_id}",
                    **changes,
                }),
            )
        assert stale.value.code == code
    assert host.motion_start_calls == []
    # Tree ticks right after Execute raise the run revision; Run is admitted.
    ticked = owner.rows["program-cycle.1"]
    owner.rows["program-cycle.1"] = _Snapshot(
        "program-cycle.1",
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
        state="running",
        revision=ticked.run_revision + 3,
    )
    motion_started = runtime.start_motion("program-cycle.1", motion_start)
    assert motion_started["state"] == "running"
    assert host.motion_start_calls == [
        ("program-cycle.1", ["robot_group"], 0, "request.motion-start.1")
    ]

    motion_stop = ProgramSelectionStopRequest.model_validate(
        {
            "request_id": "request.motion-stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-cycle.1",
            "expected_owner_epoch": "program-runner.previous",
            "reason": "ordinary operator motion Stop",
        }
    )
    with pytest.raises(ProgramRunnerError) as wrong_path:
        runtime.stop_motion("program-cycle.other", motion_stop)
    assert wrong_path.value.code == "program_run_path_invalid"
    assert host.motion_stop_calls == []
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
    with pytest.raises(ProgramRunnerError) as wrong_path:
        runtime.full_stop("program-cycle.other", full_stop)
    assert wrong_path.value.code == "program_run_path_invalid"
    assert host.full_stop_calls == []
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
    # Resume after Halt checks readiness like Resume (CONTRACTS §10).
    ready = headless.readiness
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "cell_latched", "message": "Cell latched"}],
    }
    blocked_resume = ProgramFullStopResumeRequest.model_validate({
        **resume_full_stop.model_dump(mode="json"),
        "request_id": "request.resume-full-stop.blocked",
    })
    with pytest.raises(ProgramRunnerError) as blocked_full_resume:
        runtime.resume_full_stop("program-cycle.1", blocked_resume)
    assert blocked_full_resume.value.code == "cell_latched"
    assert host.full_stop_resume_calls == []
    headless.readiness = ready
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
    # Hold the cycle reader just before its controls snapshot while the real
    # Reset advances the controls revision, then inspect the emitted frame.
    snapshot_entered, allow_snapshot = Event(), Event()
    reset_marked, allow_reset = Event(), Event()
    original_projection = runtime._runtime_controls_projection  # noqa: SLF001
    controls = runtime._optional_controls("program-cycle.1")  # noqa: SLF001
    assert controls is not None
    original_mark_reset = controls.mark_reset

    def held_projection(*args, **kwargs):
        snapshot_entered.set()
        assert allow_snapshot.wait(timeout=2.0)
        return original_projection(*args, **kwargs)

    def held_mark_reset(**kwargs):
        original_mark_reset(**kwargs)
        reset_marked.set()
        assert allow_reset.wait(timeout=2.0)

    monkeypatch.setattr(runtime, "_runtime_controls_projection", held_projection)
    monkeypatch.setattr(controls, "mark_reset", held_mark_reset)
    frames, reset_results, errors = [], [], []

    def read_cycle():
        try:
            frames.append(runtime.cycle_status())
        except Exception as error:
            errors.append(error)

    def reset_cycle():
        try:
            reset_results.append(runtime.reset_cycle(reset))
        except Exception as error:
            errors.append(error)

    reader = Thread(target=read_cycle)
    resetter = Thread(target=reset_cycle)
    reader.start()
    try:
        assert snapshot_entered.wait(timeout=2.0)
        resetter.start()
        assert reset_marked.wait(timeout=2.0)
        allow_snapshot.set()
        reader.join(timeout=2.0)
        assert not reader.is_alive()
        assert len(frames) == 1
        ProgramCycleFrameDTO.model_validate(frames[0])
        assert frames[0]["runtime_control_revision"] == controls.revision
    finally:
        allow_snapshot.set()
        allow_reset.set()
        reader.join(timeout=2.0)
        if resetter.ident is not None:
            resetter.join(timeout=2.0)
    assert not resetter.is_alive()
    assert errors == []
    assert len(reset_results) == 1
    reset_result = reset_results[0]
    monkeypatch.setattr(runtime, "_runtime_controls_projection", original_projection)
    monkeypatch.setattr(controls, "mark_reset", original_mark_reset)
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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


@pytest.mark.parametrize("case", ["unselected", "incomplete", "unknown"])
def test_default_resume_routes_only_selected_nodes_and_preserves_partial_outcomes(
    monkeypatch,
    tmp_path,
    case,
):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        # N8: Resume is fenced on the selection revision, not the run revision.
        assert "expected_run_revision" not in _kwargs
        assert _kwargs["expected_selection_revision"] == 4
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
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(
    ("near_revision_limit", "pending_stop", "failure"),
    [
        (False, False, None),
        (True, False, None),
        (False, True, None),
        (True, True, None),
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        execution_transport=(transport := _Headless()),  # type: ignore[arg-type]
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
    # Reset returns every global rule to its `active_at_start` (CONTRACTS §8).
    assert transport.rule_resets >= 1
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        "reset_options",
        "reset_robot_position",
        "accepted_reset",
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
            "request_id": "reset.scene.1", "reload_initial_scene": True,
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        request = _current_reset_request(runtime, journal, f"reset.{index}").model_copy(
            update={"reload_initial_scene": True})
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
    assert rechecked.value.code == "program_run_draft_applied_mismatch"
    with pytest.raises(ProgramRunnerError) as still_stale:
        runtime.start_run(
            request.model_copy(update={"request_id": "request.start.capacity"})
        )
    assert still_stale.value.code == "program_run_draft_applied_mismatch"
    assert events == ["generation", "applied"] * 3
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
    root = SimpleNamespace(node_id="timer", parameters={"duration_s": 5.0}, children=())
    admitted = SimpleNamespace(
        applied_record_digest=_DIGEST_B,
        graph_revision=_DIGEST_A,
        program_binding_id=_DIGEST_B,
        retained_bundle_id=_DIGEST_A,
        scenario_id="cell-pick",
        readiness=NodeExecutionReadiness(program_binding_id=_DIGEST_B,
            binding_by_node={"timer": AvailabilityFact(True)}, owner_by_port=runtime._owner_availability),
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
        "required_node_ids": ["timer"],
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
                "program": _timer_only_program(),
                "program_binding": {},
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
            "program": _timer_only_program(),
            "program_binding": {},
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

    # The port is wrapped so a simulator reset also resets Active TCPs.
    assert runtime._ports.simulator_control._port is simulation  # noqa: SLF001
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
        ("retry_runtime_step", "alarm"),
        ("retry_motion_from_current_state", "alarm"),
    ],
)
def test_failure_retry_prepares_and_releases_through_owner_before_ticking(
    monkeypatch, tmp_path, action_id, case
):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
        if case == "alarm":
            # The Kernel's operator-alarm attention refuses the motion this
            # Retry would release, before any owner effect (spec 450 §5.2).
            refusal = ProgramRunOwnerError(
                "an active operator-alarm condition interlock gates this motion"
            )
            refusal.code = "operator_alarm_interlock_active"  # type: ignore[attr-defined]
            refusal.details = {  # type: ignore[attr-defined]
                "reason": "operator_alarm_interlock_active",
                "interlock_alarms": '[{"activation_id":"a","condition_id":"c",'
                '"record_revision":1}]',
            }
            owner.attention_refusal = refusal
            with pytest.raises(ProgramRunnerError) as refused:
                runtime.apply_recovery_action(request)
            assert refused.value.code == "operator_alarm_interlock_active"
            assert refused.value.details["interlock_alarms"].startswith("[{")
            # Only the held node's resources are tested, not the whole run.
            assert owner.attention_checks == [("program-cycle.1", "move.1", "retry.1")]
            assert calls == []
            assert journal.read().state == "running"
            assert not journal.read().reconciliation_required
            # Acknowledged: the same offer's Retry proceeds.
            owner.attention_refusal = None
            result = runtime.apply_recovery_action(
                request.model_copy(update={"request_id": "retry.after.ack"})
            )
            assert result["cycle_resumed"] is True
            return
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


@pytest.mark.parametrize("failure_at", [1, 2])
def test_lifecycle_failure_before_or_after_claim_is_honest_and_resettable(monkeypatch, tmp_path, failure_at):
    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission",
                        lambda *_args, **_kwargs: _PreparedRunAdmission())
    details = {"snapshot_changed": "false", "target_facts_changed": "false", "owner_changed": "false",
               "admission_changed": "true", "admission_generation_before": "36", "admission_generation_after": "37"}
    class PointHeadless(_Headless):
        reads = 0
        failing = True
        def program_applied(self):
            # The Applied read before the claim (1) and its re-read after it (2).
            self.reads += 1
            if self.failing and self.reads == failure_at:
                raise HeadlessClientError("component_lifecycle_facts_stale", "actual read failure", 409, details=details)
            return super().program_applied()
    headless = PointHeadless()
    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.failed-start.1")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.failed-start.1"),
                             headless_client=headless, execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    runtime._host.shutdown()
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner = owner
    runtime._host = host
    _bypass_reset_scene_preparation(monkeypatch)
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as caught:
            runtime.start_run(_start_request())
        error = caught.value
        assert all(error.details[key] == value for key, value in details.items())
        cycle = journal.read()
        assert host.start_admissions == [] and headless.registered_captures == []
        if failure_at == 1:
            assert error.code == "component_lifecycle_facts_stale"
            assert cycle.state == "idle_after_reset" and cycle.claim is None
            assert admission.claims == []
        else:
            assert error.code == "program_run_started_then_failed"
            assert error.producer == "program_runner"
            assert error.details["cause_producer"] == "headless_api"
            assert error.details["program_run_id"] == cycle.program_run_id == "program-cycle.1"
            assert error.details["needs_reset"] is True
            assert cycle.state == "terminal_awaiting_reset" and cycle.claim is not None
            assert cycle.start_settlement["prepared_scene"] == {}
            assert cycle.failure["code"] == "component_lifecycle_facts_stale"
            assert all(cycle.failure["details"][key] == value for key, value in details.items())
            assert runtime.cycle_status()["failure"] == cycle.failure
            assert runtime._cycle_terminal_evidence_is_exact(journal, cycle)
            assert journal.pending_commands() == ()
            with pytest.raises(ProgramRunnerError) as repeated:
                runtime.start_run(_start_request())
            assert repeated.value.code == "program_cycle_reset_required"
            assert len(admission.claims) == 1 and host.start_admissions == []
            assert journal.pending_commands() == ()
            headless.failing = False
            # A no-host-run failed start has no native tasks; the current own
            # claim and exact durable settlement still govern release.
            reset = runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.failed.start"))
            assert reset["idle_cycle_generation"] == 2
            assert journal.read().state == "idle_after_reset" and journal.read().claim is None
            assert admission.releases[0]["settlement"]["evidence_digest"] == cycle.terminal_reference
            runtime.start_run(_start_request().model_copy(update={"request_id": "run.after.failed-start.reset"}))
            assert journal.read().state == "running"
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("host_failed", [False, True])
def test_stop_wins_running_cas_after_last_abort_without_inventing_failure(monkeypatch, tmp_path, host_failed):
    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission",
                        lambda *_args, **_kwargs: _PreparedRunAdmission())
    class StopHeadless(_Headless):
        def stop_program_run(self, body):
            return {"ok": True, "owner_epoch": "execution.epoch.1", "accepted": {
                "request_id": body["request_context"]["request_id"], "stop_id": "stop.1",
                "stop_generation": 1, "replay_status": "accepted_new"}}
    headless, admission = StopHeadless(), _Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.1"),
                             headless_client=headless, execution_transport=headless,
                             admission_client=admission, cycle_journal=journal)
    runtime._host.shutdown()
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    project = runtime._project_run
    stopped = False
    def stop_after_last_abort(snapshot):
        nonlocal stopped
        if not stopped and journal.read().state == "starting":
            stopped = True
            stop = ProgramRunStopRequest.model_validate({
                "request_id": "stop.last-window", "request_owner_epoch": "composer.1",
                "source_label": "operator", "correlation_id": None,
                "program_run_id": "program-cycle.1", "expected_owner_epoch": "runner.1",
                "reason": "operator Stop before Running CAS"})
            assert runtime.stop_run("program-cycle.1", stop)["state"] == "stopping"
            if host_failed:
                terminal = _Snapshot("program-cycle.1", owner_id=runtime.owner_id,
                                     owner_epoch=runtime.owner_epoch, state="failed", revision=2)
                terminal._value.update(reason_code="controller_lost", message="Controller disappeared during Stop",
                                       details={"reason": "controller endpoint lost"},
                                       origin={"kind": "program_runner", "node_instance_id": None, "owner_api": None})
                owner.rows["program-cycle.1"] = terminal
                runtime._record_terminal_snapshot(terminal.to_dict())
        return project(snapshot)
    monkeypatch.setattr(runtime, "_project_run", stop_after_last_abort)
    try:
        with pytest.raises(ProgramRunnerError) as caught:
            runtime.start_run(_start_request())
        assert stopped
        assert caught.value.code == ("program_run_started_then_failed" if host_failed else "program_run_superseded_by_stop")
        assert caught.value.details["program_run_id"] == "program-cycle.1"
        cycle = journal.read()
        assert len(admission.claims) == 1
        assert caught.value.details["claim_generation"] == cycle.claim["claim_generation"]
        assert caught.value.details["needs_reset"] is True
        assert cycle.state == "terminal_awaiting_reset" and cycle.failure is None
        if host_failed:
            assert caught.value.details["cause_code"] == "controller_lost"
            assert caught.value.details["cause_message"] == "Controller disappeared during Stop"
            assert caught.value.details["reason"] == "controller endpoint lost"
            assert caught.value.details["host_state"] == "failed"
            assert caught.value.details["host_run_revision"] == 2
            assert "transition is stale" not in str(caught.value)
            assert journal.read_run_snapshot("program-cycle.1")["state"] == "failed"
            assert cycle.start_settlement is None
        else:
            assert caught.value.details["stopped_by_operator"] is True
            assert cycle.start_settlement["kind"] == "terminal_effects_settled"
            assert cycle.start_settlement["terminal_run"]["state"] == "stopped"
        assert len(host.start_admissions) == 1 and host.scheduled == []
        assert len(host.stop_calls) == (0 if host_failed else 1)
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("not_ready", [True, False])
def test_scene_preparation_refuses_not_ready_but_not_a_moved_generation(monkeypatch, tmp_path, not_ready):
    class ObservedAdmission(_Admission):
        def get(self):
            return {"admission_generation": 35 if not_ready else 37,
                    "authoring_allowed": not not_ready}
    admission = ObservedAdmission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.1"),
                             headless_client=_Headless(), execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    prepared: list[str] = []
    def scene_effect(**kwargs):
        if not_ready:
            raise AssertionError("Refused admission must not prepare the Scene")
        prepared.append(kwargs["context_id"])
        return {"program_revision": "program.rev"}, {"schema_version": "manyforge.prepared_program_scene.v1"}
    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", scene_effect)
    runtime.start()
    try:
        if not_ready:
            with pytest.raises(ProgramRunnerError) as caught:
                runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
                    "request_id": "idle.reset.not-ready", "reload_initial_scene": True, "request_owner_epoch": "composer.1",
                    "source_label": "operator", "correlation_id": None,
                    "program_run_id": None, "expected_owner_epoch": "runner.1",
                    "expected_cycle_generation": 1, "expected_claim_generation": None,
                    "terminal_reference": None, "settlement_evidence_digest": None}))
            assert caught.value.code == "authoring_not_ready"
            assert caught.value.details == {"admission_generation": 35,
                                           "authoring_allowed": False,
                                           "expected_admission_generation": 35}
        else:
            # The caller read generation 35; only the generation moved since.
            result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
                "request_id": "scene.older-generation", "request_owner_epoch": "composer.1",
                "source_label": "operator", "correlation_id": None,
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 35}))
            assert result["status"] == "prepared" and result["admission_generation"] == 37
            assert len(prepared) == 1
        cycle = journal.read()
        assert cycle.state == "idle_after_reset" and cycle.cycle_generation == 1
        assert cycle.claim is None and cycle.failure is None
        assert admission.claims == [] and admission.releases == []
    finally:
        runtime.shutdown()


def test_unknown_host_start_keeps_claim_and_unresolved_request_without_resending(monkeypatch, tmp_path):
    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission",
                        lambda *_args, **_kwargs: _PreparedRunAdmission())
    class UnknownHost(_Host):
        def start_run(self, admission, **_kwargs):
            self.start_admissions.append(admission)
            raise RuntimeError("Host start result was lost")
    headless, admission = _Headless(), _Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.1"),
                             headless_client=headless, execution_transport=headless,
                             admission_client=admission, cycle_journal=journal)
    runtime._host.shutdown()
    owner = _ObservationOwner()
    host = UnknownHost(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    request = _start_request()
    try:
        with pytest.raises(ProgramRunnerError) as caught:
            runtime.start_run(request)
        assert caught.value.code == "program_cycle_reconciliation_required"
        assert caught.value.details["program_run_id"] == "program-cycle.1"
        cycle = journal.read()
        assert cycle.state == "recovery_required" and cycle.reconciliation_required
        assert cycle.claim["claim_generation"] == caught.value.details["claim_generation"]
        assert cycle.terminal_reference is None and cycle.start_settlement is None
        assert any(row["request_id"] == request.request_id for row in journal.pending_commands())
        with pytest.raises(ProgramRunnerError) as repeated:
            runtime.start_run(request)
        assert repeated.value.code == "program_cycle_reconciliation_required"
        assert len(host.start_admissions) == 1 and host.scheduled == []
        assert admission.releases == []
    finally:
        runtime.shutdown()


# DX891: dependency follows used targets/nodes, not declared roles or site names.
class _SimulationLaneHeadless(_Headless):
    def __init__(self, graph: dict[str, Any]) -> None:
        super().__init__()
        self.graph = graph
        self.graph_reads = 0

    def manager_graph(self) -> dict[str, Any]:
        self.graph_reads += 1
        return deepcopy(self.graph)


_SIMULATION_LANE_PROFILES = {
    "mock": {"lane": "mock"},
    "isaac": {"lane": "isaac"},
    "real": {"lane": "real", "device_address_ref": "lite6.address"},
}


def _set_simulation_lane(graph, component, lane):
    """Mirror one compiled profile and its Manager-derived simulator row."""
    profile = next(row for row in graph["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"]
                   if row["controllable_component_id"] == component)
    for key in ("lane", "device_address_ref"):
        profile.pop(key, None)
    profile.update(_SIMULATION_LANE_PROFILES[lane])
    dependency = next(row for row in graph["component_simulator_dependencies"]
                      if row["controllable_component_id"] == component)
    dependency["simulator"] = "isaac" if lane == "isaac" else "none"


def _simulation_lane_fixture(*, lanes=("mock", "isaac"), simulation=None, admission=None, journal=None):
    """Use real retained binding row shapes and the actual runtime catalog.

    The second component is an explicit unit-test clone for mixed-lane cases;
    this fixture does not claim to publish or reverify a compiled CAS artifact.
    """
    import json
    from pathlib import Path

    root = Path(__file__).resolve().parents[2]
    binding = json.loads((root / "manyforge_deployment/resources/"
                         "program_binding_lite6_native.fixture.json").read_text())
    binding["retained_bundle_id"], binding["graph_revision"] = _DIGEST_A, _DIGEST_B
    component = deepcopy(binding["component_role_bindings"][0])
    component.update(component_role_id="other_robot", controllable_component_id="robot.other")
    binding["component_role_bindings"].append(component)
    target = deepcopy(next(row for row in binding["target_role_bindings"] if row["target_role_id"] == "arm"))
    target.update(target_role_id="other_arm", component_role_id="other_robot",
                  controllable_component_id="robot.other")
    binding["target_role_bindings"].append(target)
    # A model-free Mock I/O device sharing the ROS runtime unit, as Applied.
    binding["component_role_bindings"].append({**deepcopy(component), "component_role_id": "station_device",
                                               "controllable_component_id": "station_io", "model_role_bindings": []})
    binding["target_role_bindings"].append({**deepcopy(target), "target_role_id": "station",
        "component_role_id": "station_device", "controllable_component_id": "station_io",
        "command_target_id": "io"})
    graph = {
        "schema_version": "manyforge.manager_graph_projection.v1",
        "source": {"available": True, "freshness": "fresh", "known_gaps": [],
                   "source_owner_id": "manager.test", "source_owner_epoch": "manager.epoch.1"},
        "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B,
        "cell_graph": {"graph_revision": _DIGEST_B,
            "controllable_components": [
                {"controllable_component_id": "lite6_arm", "runtime_unit_ids": ["cell.ros"]},
                {"controllable_component_id": "robot.other", "runtime_unit_ids": ["cell.ros"]},
                {"controllable_component_id": "station_io", "runtime_unit_ids": ["cell.ros"]}],
            "control_domains": [{"control_domain_id": "io.provider.station", "kind": "provider",
                                 "runtime_unit_ids": ["cell.ros"],
                                 "provider": {"controllable_component_id": "station_io",
                                              "provider_runtime_unit_id": "cell.ros"}}],
            "control_bindings": [],
            "runtime_units": [{"runtime_unit_id": "cell.ros",
                "runtime_profile_id": "manyforge.ros2_control_execution_runtime",
                "configuration": {"controller_profiles": [
                    {"controllable_component_id": name, **_SIMULATION_LANE_PROFILES[lane]}
                    for name, lane in zip(("lite6_arm", "robot.other"), lanes, strict=True)]}}]},
        # Deployment-derived and sorted by component; Mock and Real need none.
        "component_simulator_dependencies": [
            {"controllable_component_id": name, "simulator": "isaac" if lane == "isaac" else "none"}
            for name, lane in zip(("lite6_arm", "robot.other"), lanes, strict=True)],
    }
    headless = _SimulationLaneHeadless(graph)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="program-runner.test"),
        headless_client=headless, execution_transport=headless,
        simulation_control=simulation, admission_client=admission, cycle_journal=journal)
    return runtime, headless, binding


def _simulation_lane_program(*roles: str, node_id="motion.move_to_joint_target", **parameters):
    children = [{"node_id": "gripper.command_position" if role == "gripper" else node_id,
                 "instance_id": f"step.{index}",
                 "parameters": {**parameters, "target_role": role}, "children": []}
                for index, role in enumerate(roles)]
    return {"tree": {"node_id": "sequence", "instance_id": "root",
                     "parameters": {}, "children": children},
            # Declared, unused roles must not become simulator dependencies.
            "target_roles": {"arm": {}, "other_arm": {}, "gripper": {}}}


def _simulation_node_program(node_id):
    parameters = {"simulation.reset": {}, "simulation.set_state": {"state": "play"},
                  "simulation.step": {"steps": 1},
                  "simulation.set_entity_pose": {"scene_resource_id": "part", "pose": {
                      "frame_ref": {"kind": "cell_frame"}, "position_m": [0.0, 0.0, 0.0],
                      "orientation": {"rpy_deg": [0.0, 0.0, 0.0]}}}}[node_id]
    return {"tree": {"node_id": node_id, "instance_id": "simulation",
                     "parameters": parameters, "children": []},
            "scene": [{"scene_resource_id": "part"}] if node_id == "simulation.set_entity_pose" else []}


def _simulation_lane_requirement(runtime, binding, program):
    return runtime._simulation_context_requirement(  # noqa: SLF001
        program=program, program_binding=binding,
        retained_bundle_id=_DIGEST_A, graph_revision=_DIGEST_B)


def _simulation_lane_required(runtime, binding, program):
    return _simulation_lane_requirement(runtime, binding, program)[0]


@pytest.mark.parametrize(("lanes", "used_roles", "required"), [
    (("mock", "mock"), ("arm",), False),
    (("isaac", "isaac"), ("arm",), True),
    (("mock", "isaac"), ("arm",), False),  # unused declared Isaac target
    (("mock", "isaac"), ("gripper",), False),
    (("mock", "isaac"), ("other_arm",), True),
    (("mock", "isaac"), ("arm", "other_arm"), True),
    (("isaac", "mock"), ("other_arm",), False),
    (("real", "mock"), ("arm",), False),
    (("real", "isaac"), ("arm",), False),
    (("real", "isaac"), ("other_arm",), True),
    (("real", "real"), ("arm", "other_arm"), False),
])
def test_simulation_dependency_follows_only_actual_used_target_components(lanes, used_roles, required):
    runtime, headless, binding = _simulation_lane_fixture(lanes=lanes)
    try:
        assert _simulation_lane_required(runtime, binding, _simulation_lane_program(*used_roles)) is required
        assert headless.graph_reads == 1
    finally:
        runtime.shutdown()


def test_resume_motion_reads_run_readiness_before_host_effect():
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._control_effect = (  # type: ignore[method-assign]  # noqa: SLF001
        lambda request, *, kind, effect: effect()
    )
    headless = _Headless()
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "rule_holds", "message": "guard holds",
                             "rule_id": "guard"}],
    }
    runtime._headless = headless  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = SimpleNamespace(  # noqa: SLF001
        resume_motion=lambda *_args, **_kwargs: pytest.fail(
            "a readiness refusal must precede the host Resume effect"
        )
    )
    request = ProgramSelectionResumeRequest.model_validate({
        "request_id": "request.motion-resume.blocked",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "program_run_id": "run.1",
        "expected_owner_epoch": "program-runner.test",
        "expected_selection_revision": 1,
        "group_ids": ["arm"],
        "reason": "operator Resume",
    })
    with pytest.raises(ProgramRunnerError) as blocked:
        runtime.resume_motion("run.1", request)
    assert blocked.value.code == "rule_holds"
    assert blocked.value.details == {"rule_id": "guard"}


def test_pure_timer_program_has_no_simulator_dependency_or_manager_read():
    runtime, headless, binding = _simulation_lane_fixture(simulation=_SimulationControl())
    program = _timer_only_program()
    try:
        assert _simulation_lane_requirement(runtime, binding, program) == (
            False, AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        assert headless.graph_reads == 0
        assert runtime.health()["simulation_control_available"] is True
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("node_id", ["simulation.reset", "simulation.set_state", "simulation.step",
                                     "simulation.set_entity_pose"])
@pytest.mark.parametrize(("lanes", "eligible"), [
    (("mock", "mock"), False), (("isaac", "isaac"), True), (("mock", "isaac"), True),
    (("real", "real"), False), (("real", "mock"), False), (("real", "isaac"), True),
])
def test_actual_cell_global_simulator_capability_has_typed_cell_eligibility(node_id, lanes, eligible):
    runtime, headless, binding = _simulation_lane_fixture(lanes=lanes, simulation=_SimulationControl())
    program = _simulation_node_program(node_id)
    try:
        required, available = _simulation_lane_requirement(runtime, binding, program)
        assert required is True
        assert available == (AvailabilityFact(True) if eligible else
                             AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        assert headless.graph_reads == 1
        # This is configured ownership, not live simulator service evidence.
        assert runtime.health()["simulation_control_available"] is True
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("case", ["stale", "unavailable", "gap", "bundle", "graph", "nested_graph",
                                  "duplicate_profile", "foreign_profile",
                                  "foreign_component_role", "duplicate_component_role", "duplicate_target_role",
                                  "duplicate_graph_component", "foreign_graph_component", "duplicate_runtime",
                                  "foreign_runtime", "missing_runtime_profile", "duplicate_runtime_owner"])
def test_simulation_dependency_refuses_inexact_source_or_used_component_join(case):
    runtime, headless, binding = _simulation_lane_fixture()
    graph = headless.graph
    if case == "stale":
        graph["source"]["freshness"] = "stale"
    elif case == "unavailable":
        graph["source"]["available"] = False
    elif case == "gap":
        graph["source"]["known_gaps"] = ["missing_runtime"]
    elif case == "bundle":
        graph["retained_bundle_id"] = _DIGEST_B
    elif case == "graph":
        graph["graph_revision"] = _DIGEST_A
    elif case == "nested_graph":
        graph["cell_graph"]["graph_revision"] = _DIGEST_A
    elif case in {"duplicate_profile", "foreign_profile"}:
        rows = graph["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"]
        if case == "duplicate_profile":
            rows.append(deepcopy(rows[0]))
        else:
            rows[0]["controllable_component_id"] = "foreign.component"
    elif case == "foreign_component_role":
        binding["target_role_bindings"][0]["component_role_id"] = "foreign.role"
    elif case == "duplicate_component_role":
        binding["component_role_bindings"].append(deepcopy(binding["component_role_bindings"][0]))
    elif case == "duplicate_target_role":
        binding["target_role_bindings"].append(deepcopy(binding["target_role_bindings"][0]))
    elif case == "duplicate_graph_component":
        graph["cell_graph"]["controllable_components"].append(
            deepcopy(graph["cell_graph"]["controllable_components"][0]))
    elif case == "foreign_graph_component":
        graph["cell_graph"]["controllable_components"][0]["controllable_component_id"] = "foreign.component"
    elif case == "duplicate_runtime":
        graph["cell_graph"]["runtime_units"].append(deepcopy(graph["cell_graph"]["runtime_units"][0]))
    elif case == "foreign_runtime":
        graph["cell_graph"]["runtime_units"][0]["runtime_unit_id"] = "foreign.runtime"
    elif case == "missing_runtime_profile":
        del graph["cell_graph"]["runtime_units"][0]["runtime_profile_id"]
    elif case == "duplicate_runtime_owner":
        graph["cell_graph"]["controllable_components"][0]["runtime_unit_ids"].append("cell.ros")
    try:
        with pytest.raises(ProgramRunOwnerError):
            _simulation_lane_required(runtime, binding, _simulation_lane_program("arm"))
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("parameters", "required", "reads"), [
    ({}, True, 1),  # default state selects the condition; default target is Isaac
    ({"expected_state": "idle"}, False, 0),  # inactive target requirement
    ({"expected_state": "moving", "target_role": "arm"}, False, 1),
    ({"expected_state": "moving", "target_role": "other_arm"}, True, 1),
])
def test_simulation_dependency_respects_conditional_and_defaulted_target_requirements(
    monkeypatch, parameters, required, reads
):
    runtime, headless, binding = _simulation_lane_fixture()
    try:
        catalog = runtime._environment.catalog  # noqa: SLF001
        original_get = catalog.get
        entry = original_get("gripper.check_status")
        # The retained catalog has real conditional feedback requirements, but
        # no default target-role parameter. This unit-only catalog variation
        # isolates that supported catalog rule without inventing a product node.
        entry = replace(entry,
            required_capabilities=(next(row for row in entry.required_capabilities
                                        if row.when_equals == "moving"),),
            parameters=tuple(replace(row, required=False, has_default=True,
                                     default="other_arm" if row.id == "target_role" else "moving")
                             for row in entry.parameters))
        monkeypatch.setattr(catalog, "get", lambda node_id:
                            entry if node_id == "gripper.check_status" else original_get(node_id))
        program = {"tree": {"node_id": "gripper.check_status", "instance_id": "status",
                            "parameters": parameters, "children": []}}
        assert _simulation_lane_required(runtime, binding, program) is required
        assert headless.graph_reads == reads
    finally:
        runtime.shutdown()


def _simulation_lane_context(runtime, binding, *, scenario="isaac-cell", role="arm", program=None):
    from manyforge_behavior.orchestration_ports import ProgramRunPortContext

    program = _simulation_lane_program(role) if program is None else deepcopy(program)
    program.setdefault("scene", [])
    program.update(program_id=binding["program_id"], program_revision=binding["program_revision"])
    return ProgramRunPortContext(program_run_id="run.selected-lane", applied_record_digest=_DIGEST_B,
        scenario_id=scenario, source_document_digest=_DIGEST_A,
        program_id=binding["program_id"], program_revision=binding["program_revision"],
        program_binding_id=binding["program_binding_id"], retained_bundle_id=_DIGEST_A,
        graph_revision=_DIGEST_B, node_environment_digest=runtime._environment.environment_digest,
        program=program, program_binding=binding)


@pytest.mark.parametrize("lane", ["mock", "isaac"])
@pytest.mark.parametrize("program_kind", ["target", "entity"])
def test_selected_context_binds_real_isaac_owner_and_keeps_mock_independent(lane, program_kind):
    # The simulator entity mapping comes from the Deployment links in the
    # context; binding never depends on a per-scenario site mapping.
    from manyforge_program_runner.runtime import _SelectedSimulationContextPort
    from manyforge_ros.program_simulation_port import (
        SimulationProgramSiteConfig, SimulationProgramTaskPort,
    )

    def unexpected(*_args, **_kwargs):
        raise AssertionError("context selection must not issue a simulator command")

    control = SimpleNamespace(reset=unexpected, set_simulation_state=unexpected,
                              step=unexpected, set_entity_state=unexpected)
    raw = SimulationProgramTaskPort(control, SimulationProgramSiteConfig(
        service_prefix="/isaacsim", service_timeout_s=1.0))
    runtime, headless, binding = _simulation_lane_fixture(
        lanes=(lane, "isaac" if program_kind == "target" else "mock"), simulation=raw)
    context = _simulation_lane_context(runtime, binding,
        program=_simulation_node_program("simulation.set_entity_pose") if program_kind == "entity" else None)
    selected = _SelectedSimulationContextPort(raw, lambda captured:
        runtime._simulation_context_requirement(program=captured.program, program_binding=captured.program_binding,
            retained_bundle_id=captured.retained_bundle_id, graph_revision=captured.graph_revision))
    try:
        if lane == "mock" and program_kind == "entity":
            with pytest.raises(ProgramRunOwnerError, match="simulator"):
                selected.bind(context)
        else:
            selected.bind(context)
            selected.unbind(context.program_run_id)
        assert headless.graph_reads == 1
        assert runtime.health()["simulation_control_available"] is True
    finally:
        runtime.shutdown()


def test_one_runtime_rechecks_used_lane_after_each_current_graph_change():
    simulation = _SimulationControl()
    runtime, headless, binding = _simulation_lane_fixture(lanes=("mock", "mock"), simulation=simulation)
    applied = {"program": _simulation_lane_program("arm"), "program_binding": binding,
               "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B}
    try:
        assert runtime._simulation_context_requirement(program=applied["program"],
            program_binding=binding, retained_bundle_id=_DIGEST_A, graph_revision=applied["graph_revision"]) == (
                False, AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        _set_simulation_lane(headless.graph, "lite6_arm", "isaac")
        applied["graph_revision"] = binding["graph_revision"] = "sha256:" + "c" * 64
        headless.graph["graph_revision"] = headless.graph["cell_graph"]["graph_revision"] = applied["graph_revision"]
        assert runtime._simulation_context_requirement(program=applied["program"],
            program_binding=binding, retained_bundle_id=_DIGEST_A, graph_revision=applied["graph_revision"]) == (
                True, AvailabilityFact(True))
        _set_simulation_lane(headless.graph, "lite6_arm", "real")
        applied["graph_revision"] = binding["graph_revision"] = "sha256:" + "d" * 64
        headless.graph["graph_revision"] = headless.graph["cell_graph"]["graph_revision"] = applied["graph_revision"]
        assert runtime._simulation_context_requirement(program=applied["program"],
            program_binding=binding, retained_bundle_id=_DIGEST_A, graph_revision=applied["graph_revision"]) == (
                False, AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        assert headless.graph_reads == 3
        assert runtime.health()["simulation_control_available"] is True
    finally:
        runtime.shutdown()


def test_used_isaac_target_requires_but_cannot_supply_an_unconfigured_simulator_owner():
    runtime, _headless, binding = _simulation_lane_fixture(lanes=("isaac", "mock"))
    try:
        assert _simulation_lane_requirement(runtime, binding, _simulation_lane_program("arm")) == (
            True, AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
    finally:
        runtime.shutdown()


def _io_station_program(*motion_roles: str) -> dict[str, Any]:
    """Mock station_io I/O nodes (trial d) plus optional motion on robot roles."""
    io = [{"node_id": node_id, "instance_id": f"io.{index}",
           "parameters": {"target_role": "station", "resource_role": "clamp", **extra}, "children": []}
          for index, (node_id, extra) in enumerate((("io.set_output", {"value": True}),
                                                    ("io.get_input", {}), ("io.check_input", {}),
                                                    ("io.wait_for_input", {})))]
    program = _simulation_lane_program(*motion_roles)
    program["tree"]["children"] = io + program["tree"]["children"]
    program["target_roles"]["station"] = {}
    return program


@pytest.mark.parametrize("lanes", [("mock", "mock"), ("isaac", "isaac")])
def test_io_provider_target_needs_no_ros_controller_profile(lanes):
    # station_io shares the ROS runtime unit but owns a provider-kind domain:
    # no controller profile, no simulator lane (its component state gates it).
    runtime, headless, binding = _simulation_lane_fixture(lanes=lanes, simulation=_SimulationControl())
    try:
        assert _simulation_lane_requirement(runtime, binding, _io_station_program()) == (
            False, AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        assert headless.graph_reads == 1
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("lanes", "motion_role", "required"), [
    (("mock", "isaac"), "arm", False),
    (("isaac", "mock"), "arm", True),
    (("mock", "isaac"), "other_arm", True),
])
def test_mixed_motion_and_io_program_selects_only_the_controller_target(lanes, motion_role, required):
    runtime, headless, binding = _simulation_lane_fixture(lanes=lanes, simulation=_SimulationControl())
    try:
        assert _simulation_lane_required(runtime, binding, _io_station_program(motion_role)) is required
        # The controller target keeps its exact unique-profile requirement.
        rows = headless.graph["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"]
        component = {"arm": "lite6_arm", "other_arm": "robot.other"}[motion_role]
        rows[:] = [row for row in rows if row["controllable_component_id"] != component]
        with pytest.raises(ProgramRunOwnerError, match="no unique ROS controller profile"):
            _simulation_lane_required(runtime, binding, _io_station_program(motion_role))
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("case", ["no_provider_domain", "controller_kind_domain"])
def test_io_exemption_follows_provider_domain_kind_not_role_or_node_name(case):
    runtime, headless, binding = _simulation_lane_fixture()
    domain = headless.graph["cell_graph"]["control_domains"][0]
    if case == "no_provider_domain":
        headless.graph["cell_graph"]["control_domains"] = []
    else:
        del domain["kind"]  # the controller alternative carries no kind on the wire
    try:
        with pytest.raises(ProgramRunOwnerError, match="no unique ROS controller profile"):
            _simulation_lane_required(runtime, binding, _io_station_program())
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("case", "reason"), [
    ("missing_unit", "runtime is not current"),
    ("duplicate_unit", "runtime is not current"),
    ("empty_runtime_profile", "no runtime profile"),
])
def test_io_provider_still_passes_exact_runtime_unit_checks(case, reason):
    runtime, headless, binding = _simulation_lane_fixture()
    graph = headless.graph["cell_graph"]
    station = next(row for row in graph["controllable_components"]
                   if row["controllable_component_id"] == "station_io")
    if case == "missing_unit":
        station["runtime_unit_ids"] = ["missing.unit"]
    elif case == "duplicate_unit":
        graph["runtime_units"].append(deepcopy(graph["runtime_units"][0]))
    else:
        station["runtime_unit_ids"] = ["io.unit"]
        graph["runtime_units"].append({"runtime_unit_id": "io.unit", "runtime_profile_id": "",
                                       "configuration": {}})
    try:
        with pytest.raises(ProgramRunOwnerError, match=reason):
            _simulation_lane_required(runtime, binding, _io_station_program())
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("profile", [None, "mock"])
def test_mixed_provider_and_controller_component_keeps_controller_profile_check(profile):
    runtime, headless, binding = _simulation_lane_fixture()
    graph = headless.graph["cell_graph"]
    # station_io also owns a controller binding: it is not provider-only.
    graph["control_bindings"].append({"control_binding_id": "station_io.controller",
                                      "controllable_component_id": "station_io"})
    if profile is not None:
        graph["runtime_units"][0]["configuration"]["controller_profiles"].append(
            {"controllable_component_id": "station_io", "lane": "mock"})
        headless.graph["component_simulator_dependencies"].append(
            {"controllable_component_id": "station_io", "simulator": "none"})
    try:
        if profile is None:
            with pytest.raises(ProgramRunOwnerError, match="no unique ROS controller profile"):
                _simulation_lane_required(runtime, binding, _io_station_program())
        else:
            assert _simulation_lane_required(runtime, binding, _io_station_program()) is False
    finally:
        runtime.shutdown()


def test_motion_target_without_profile_still_refused_with_controller_profile_reason():
    runtime, headless, binding = _simulation_lane_fixture()
    headless.graph["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"][0][
        "controllable_component_id"] = "foreign.component"
    try:
        with pytest.raises(ProgramRunOwnerError, match="no unique ROS controller profile"):
            _simulation_lane_required(runtime, binding, _simulation_lane_program("arm"))
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("no_output", [
    {"output_mode": "none"},
    {"output_mode": "commanded", "pneumatic_io": {"inputs": {}, "outputs": []}},
])
def test_simulation_dependency_skips_a_lane_free_pneumatic_tool(no_output):
    # mock_lite6_pneumatic_pick_and_place (merge 6 D4): the mounted no-output pneumatic tool
    # has no controller profile; Run was refused before this fix.
    runtime, headless, binding = _simulation_lane_fixture(lanes=("mock", "mock"))
    graph = headless.graph
    configuration = graph["cell_graph"]["runtime_units"][0]["configuration"]
    configuration["controller_profiles"] = [
        row for row in configuration["controller_profiles"] if row["controllable_component_id"] != "robot.other"]
    graph["component_simulator_dependencies"] = [
        row for row in graph["component_simulator_dependencies"] if row["controllable_component_id"] != "robot.other"]
    tool = {"controllable_component_id": "robot.other", "command_target_id": "gripper",
            "actuation_kind": "pneumatic", **no_output}
    configuration["gripper_target_configurations"] = [tool]
    try:
        assert _simulation_lane_required(runtime, binding, _simulation_lane_program("arm", "other_arm")) is False
        # A commanded (lane-bound) tool still needs its controller profile.
        tool.pop("pneumatic_io", None)
        tool.update(actuation_kind="position_commanded", output_mode="commanded")
        with pytest.raises(ProgramRunOwnerError, match="no unique ROS controller profile"):
            _simulation_lane_required(runtime, binding, _simulation_lane_program("arm", "other_arm"))
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("program_kind", "reason"), [
    ("io_only", "reached Applied Program run admission"),
    ("real_motion", "reached Applied Program run admission"),
    ("motion_without_profile", "no unique ROS controller profile"),
])
def test_run_start_admits_io_only_program_past_simulation_dependency(monkeypatch, tmp_path, program_kind, reason):
    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "io-only.sqlite3", owner_epoch="program-runner.test")
    runtime, headless, binding = _simulation_lane_fixture(
        lanes=("real", "real") if program_kind == "real_motion" else ("mock", "mock"),
        simulation=_SimulationControl(), admission=admission, journal=journal)
    if program_kind == "io_only":
        program = _io_station_program()
    elif program_kind == "real_motion":
        program = _simulation_lane_program("arm", "other_arm")
    else:
        program = _simulation_lane_program("arm")
        del headless.graph["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"][0]
    applied = _Headless.program_applied(headless)
    applied.update(program=program, program_binding=binding)
    monkeypatch.setattr(headless, "program_applied", lambda: deepcopy(applied))
    reached = []

    def run_admission(*_args, **kwargs):
        reached.append(kwargs["owner_availability"]["simulator_control"])
        raise ProgramRunOwnerError("reached Applied Program run admission")

    def forbidden(*_args, **_kwargs):
        pytest.fail("no Run effect is expected in this admission probe")

    monkeypatch.setattr(runtime._host, "start_run", forbidden)  # noqa: SLF001
    monkeypatch.setattr(runtime, "_prepare_run_scene", forbidden)
    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission", run_admission)
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.start_run(_start_request())
        assert refused.value.code == "program_run_admission_failed"
        assert reason in str(refused.value)
        assert reached == ([AvailabilityFact(False, ("program_runtime_owner_unavailable",))]
                           if program_kind != "motion_without_profile" else [])
        assert journal.read().state == "idle_after_reset" and admission.claims == []
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("with_target", [False, True])
@pytest.mark.parametrize("node_id", ["simulation.reset", "simulation.set_entity_pose"])
def test_mock_simulator_node_start_is_refused_before_claim_capture_or_host_start(monkeypatch, tmp_path, with_target, node_id):
    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "mock-simulator.sqlite3", owner_epoch="program-runner.test")
    runtime, headless, binding = _simulation_lane_fixture(lanes=("mock", "mock"),
        simulation=_SimulationControl(), admission=admission, journal=journal)
    program = _simulation_node_program(node_id)
    simulator = program["tree"]
    if with_target:
        program["tree"] = _simulation_lane_program("arm")["tree"]
    if with_target:
        program["tree"]["children"].append(simulator)
    applied = _Headless.program_applied(headless)
    applied.update(program=program, program_binding=binding)
    monkeypatch.setattr(headless, "program_applied", lambda: deepcopy(applied))

    def forbidden(*_args, **_kwargs):
        pytest.fail("Unavailable simulator capability must refuse before claims or Run effects")

    monkeypatch.setattr(runtime._host, "start_run", forbidden)  # noqa: SLF001
    monkeypatch.setattr(runtime, "_prepare_run_scene", forbidden)
    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission", forbidden)
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.start_run(_start_request())
        assert refused.value.code == "program_run_admission_failed"
        assert "simulator" in str(refused.value).lower()
        assert journal.read().state == "idle_after_reset"
        assert journal.read().claim is None and journal.read().program_run_id is None
        assert admission.claims == [] and headless.registered_captures == []
        assert headless.graph_reads == 1
        assert runtime.health()["simulation_control_available"] is True
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("scope", "capability", "owner", "required"), [
    ("cell_global", "simulator.control.reset", "kernel.command_target", False),
    ("cell_global", "unrelated.global.capability", "simulator.control", None),
    ("cell_global", "simulator.control.future", "simulator.control", None),
    ("target_binding", "simulator.control.reset", "simulator.control", False),
])
def test_simulator_dependency_uses_exact_catalog_scope_capability_and_configured_owner(
    monkeypatch, scope, capability, owner, required
):
    runtime, headless, binding = _simulation_lane_fixture(lanes=("mock", "isaac"),
                                                         simulation=_SimulationControl())
    try:
        catalog = runtime._environment.catalog  # noqa: SLF001
        original_get = catalog.get
        original = original_get("simulation.reset")
        # Actual catalog entries lack these mismatch cases. A local variation
        # proves the approved predicate without introducing a product node.
        requirement = replace(original.required_capabilities[0], scope=scope,
                              capability_id=capability,
                              target_parameter="target_role" if scope == "target_binding" else None)
        entry = replace(original, required_capabilities=(requirement,),
                        execution=replace(original.execution, owner_api=owner))
        monkeypatch.setattr(catalog, "get", lambda node_id:
                            entry if node_id == "simulation.reset" else original_get(node_id))
        program = {"tree": {"node_id": "simulation.reset", "instance_id": "reset",
                            "parameters": {"target_role": "arm"}, "children": []}}
        if required is None:
            # The owner/scope is authoritative; the known capability set is a
            # consistency assertion, never permission to skip a future node.
            with pytest.raises(ProgramRunOwnerError, match="capability"):
                _simulation_lane_required(runtime, binding, program)
            assert headless.graph_reads == 0
        else:
            assert _simulation_lane_required(runtime, binding, program) is required
            assert headless.graph_reads == (0 if scope == "cell_global" and not required else 1)
    finally:
        runtime.shutdown()


def test_context_bind_rechecks_current_source_and_unbinds_raw_owner():
    from manyforge_program_runner.runtime import _SelectedSimulationContextPort

    class Simulation(_SimulationControl):
        def __init__(self):
            super().__init__()
            self.binds = []
            self.unbinds = []

        def bind(self, context):
            self.binds.append(context.program_run_id)

        def unbind(self, run_id):
            self.unbinds.append(run_id)

    simulation = Simulation()
    runtime, headless, binding = _simulation_lane_fixture(lanes=("isaac", "mock"), simulation=simulation)
    context = _simulation_lane_context(runtime, binding)
    selected = _SelectedSimulationContextPort(simulation, lambda captured:
        runtime._simulation_context_requirement(program=captured.program, program_binding=captured.program_binding,
            retained_bundle_id=captured.retained_bundle_id, graph_revision=captured.graph_revision))
    try:
        assert _simulation_lane_requirement(runtime, binding, context.program)[0] is True
        headless.graph["source"]["freshness"] = "stale"
        with pytest.raises(ProgramRunOwnerError):
            selected.bind(context)
        assert simulation.binds == []
        # Existing absent-context cleanup remains delegated even after refusal.
        selected.unbind(context.program_run_id)
        assert simulation.unbinds == [context.program_run_id]
        assert headless.graph_reads == 2
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("lanes", "node_id", "available", "graph_reads"), [
    (("mock", "isaac"), "motion.move_to_joint_target", True, 0),
    (("mock", "mock"), "simulation.reset", False, 1),
    (("mock", "mock"), "simulation.set_entity_pose", False, 1),
    (("mock", "isaac"), "simulation.reset", True, 1),
])
def test_public_node_availability_projects_real_simulator_eligibility_without_target_poll_reads(
    monkeypatch, tmp_path, lanes, node_id, available, graph_reads
):
    from manyforge_behavior.executable_node_resolver import build_executable_node_resolver

    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "availability.sqlite3", owner_epoch="program-runner.test")
    runtime, headless, binding = _simulation_lane_fixture(lanes=lanes, simulation=_SimulationControl(),
                                                       admission=admission, journal=journal)
    program = (_simulation_lane_program("arm") if node_id == "motion.move_to_joint_target" else
               _simulation_node_program(node_id))
    raw_node = (program["tree"]["children"][0] if node_id == "motion.move_to_joint_target" else program["tree"])
    applied = _Headless.program_applied(headless)
    applied.update(program=program, program_binding=binding)

    def admit(_applied, **kwargs):
        # Supply the same existing binding facts as admitted Program data; the
        # real projector and runtime eligibility determine the negative result.
        return SimpleNamespace(applied_record_digest=_DIGEST_B, graph_revision=_DIGEST_B,
            program_binding_id=binding["program_binding_id"], retained_bundle_id=_DIGEST_A,
            scenario_id="cell-pick", readiness=NodeExecutionReadiness(
                program_binding_id=binding["program_binding_id"],
                binding_by_node={row.row.node_id: AvailabilityFact(True) for row in
                    build_executable_node_resolver(runtime._environment).dispositions},  # noqa: SLF001
                owner_by_port=kwargs["owner_availability"]),
            program=SimpleNamespace(program_id=binding["program_id"], program_revision=_DIGEST_A,
                tree=SimpleNamespace(node_id=node_id, parameters=raw_node["parameters"], children=())))

    monkeypatch.setattr("manyforge_program_runner.runtime.build_applied_program_run_admission", admit)
    runtime.start()
    try:
        result = runtime.node_availability(ProgramNodeAvailabilityRequest.model_validate({"applied": applied}))
        assert result["availability"]["execution_available"] is available
        if not available:
            assert "program_runtime_owner_unavailable" in result["availability"]["reason_codes"]
        assert headless.graph_reads == graph_reads
        assert runtime.health()["simulation_control_available"] is True
        assert admission.claims == [] and headless.registered_captures == []
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("program_kind", ["target", "simulator"])
def test_real_ros_configuration_is_admitted_without_simulator_dependency(program_kind):
    # CP8 step 1: a unit hosting only real robots has no `mode`; the Manager
    # row, not the lane selector, decides that no simulator is needed.
    runtime, headless, binding = _simulation_lane_fixture(lanes=("real", "real"),
                                                         simulation=_SimulationControl())
    configuration = headless.graph["cell_graph"]["runtime_units"][0]["configuration"]
    assert "mode" not in configuration
    assert all(row["lane"] == "real" for row in configuration["controller_profiles"])
    program = (_simulation_lane_program("arm", "other_arm") if program_kind == "target" else
               _simulation_node_program("simulation.reset"))
    try:
        # The target needs no simulator; a simulator node stays required but
        # is ineligible because no hosted component depends on Isaac.
        assert _simulation_lane_requirement(runtime, binding, program) == (
            program_kind == "simulator", AvailabilityFact(False, ("program_runtime_owner_unavailable",)))
        assert headless.graph_reads == 1
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("program_kind", ["target", "simulator"])
@pytest.mark.parametrize("case", ["absent", "not_rows", "not_row", "missing_row", "extra_row", "duplicate_row",
                                  "unknown_simulator", "open_row", "empty_component", "lane_mismatch"])
def test_component_simulator_dependency_rows_must_match_ros_controller_profiles(program_kind, case):
    runtime, headless, binding = _simulation_lane_fixture(lanes=("isaac", "mock"),
                                                         simulation=_SimulationControl())
    dependencies = headless.graph["component_simulator_dependencies"]
    if case == "absent":
        del headless.graph["component_simulator_dependencies"]
    elif case == "not_rows":
        headless.graph["component_simulator_dependencies"] = {"lite6_arm": "isaac"}
    elif case == "not_row":
        dependencies[0] = "lite6_arm"
    elif case == "missing_row":
        del dependencies[1]
    elif case == "extra_row":
        dependencies.append({"controllable_component_id": "station_io", "simulator": "none"})
    elif case == "duplicate_row":
        dependencies.append(deepcopy(dependencies[0]))
    elif case == "unknown_simulator":
        dependencies[0]["simulator"] = "mock"
    elif case == "open_row":
        dependencies[0]["lane"] = "isaac"
    elif case == "empty_component":
        dependencies[0]["controllable_component_id"] = ""
    else:
        # A row for a component the ROS profiles do not host.
        dependencies[0]["controllable_component_id"] = "foreign.component"
    program = (_simulation_lane_program("arm") if program_kind == "target" else
               _simulation_node_program("simulation.reset"))
    try:
        with pytest.raises(ProgramRunOwnerError, match="component simulator rows|do not match"):
            _simulation_lane_requirement(runtime, binding, program)
        assert headless.graph_reads == 1
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("crossing", ["accept", "complete"])
def test_liveness_cycle_projection_keeps_one_snapshot_across_reset(monkeypatch, tmp_path, crossing):
    from manyforge_program_runner.liveness import (
        ProgramRunnerLivenessError, ProgramRunnerLivenessReceiver,
    )

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.snapshot")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.snapshot"),
        headless_client=_Headless(), execution_transport=_Headless(),
        admission_client=Admission(), cycle_journal=journal,
    )
    effect_entered, release_effect = Event(), Event()
    errors = []

    def scene_effect(_request, _authorization):
        effect_entered.set()
        assert release_effect.wait(5), "test did not release the Reset effect"

    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", scene_effect)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.snapshot", "request_owner_epoch": "caller.snapshot",
        "source_label": "operator_ui", "correlation_id": "reset.snapshot.correlation",
        "program_run_id": None, "expected_owner_epoch": runtime.owner_epoch,
        "expected_cycle_generation": 1, "reload_initial_scene": True,
    })

    def reset():
        try:
            runtime.reset_cycle(request)
        except BaseException as error:
            errors.append(error)

    reset_thread = Thread(target=reset)
    read_snapshot = journal.status_snapshot

    def snapshot_then_transition():
        snapshot = read_snapshot()
        if crossing == "accept":
            reset_thread.start()
            assert effect_entered.wait(2)
        else:
            release_effect.set()
            reset_thread.join(2)
            assert not reset_thread.is_alive()
        return snapshot

    try:
        if crossing == "complete":
            reset_thread.start()
            assert effect_entered.wait(2)
        monkeypatch.setattr(journal, "status_snapshot", snapshot_then_transition)
        # Reset holds the lifecycle transition lock during its effect. This
        # dependency-free read must stay available without taking that lock.
        projected = runtime._current_cycle_status_projection()  # noqa: SLF001
        monkeypatch.setattr(journal, "status_snapshot", read_snapshot)
        receiver = object.__new__(ProgramRunnerLivenessReceiver)
        receiver._cycle_status = lambda: projected  # noqa: SLF001
        receiver._project_cycle_status()  # noqa: SLF001
        if crossing == "accept":
            assert projected["state"] == "idle_after_reset"
            assert projected["accepted_reset"] is None
            assert journal.read().state == "resetting"
            # Reconstruct the former double-read result: the unchanged strict
            # receiver rejects exactly the live failure's mixed state/context.
            mixed = {**projected, "accepted_reset": runtime._accepted_reset_context(journal, journal.read())}
            receiver._cycle_status = lambda: mixed  # noqa: SLF001
            with pytest.raises(ProgramRunnerLivenessError, match="no current Reset"):
                receiver._project_cycle_status()  # noqa: SLF001
        else:
            assert projected["state"] == "resetting"
            assert projected["accepted_reset"]["request_id"] == request.request_id
            assert journal.read().state == "idle_after_reset"
            assert journal.read().cycle_generation == 2
        assert not errors
    finally:
        release_effect.set()
        if reset_thread.ident is not None:
            reset_thread.join(2)
        runtime.shutdown()
    assert not reset_thread.is_alive()
    assert not errors


@pytest.mark.parametrize("idle", [False, True])
@pytest.mark.parametrize("reload_scene,reset_position", [(False, False), (True, False), (False, True), (True, True)])
def test_reset_selectors_order_and_plain_preservation(monkeypatch, tmp_path, idle, reload_scene, reset_position):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim("runner.options"))
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    admission = Admission()
    headless = _Headless()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
                             headless_client=headless, execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    events = []
    def effect(name):
        def apply(_request, _authorization):
            status = runtime.cycle_status()
            assert status["state"] == "resetting"
            assert status["program_run_id"] == (None if idle else "program-cycle.1")
            assert status["reset_phase"] == ("restoring_scene" if name == "scene" else "restoring_position")
            events.append(name)
        return apply
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", effect("scene"))
    monkeypatch.setattr(runtime, "_reset_active_tcp", lambda: events.append("tcp"))
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", effect("position"))
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: events.append("preflight"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.options", "request_owner_epoch": "composer.1", "source_label": "operator_ui",
        "correlation_id": "reset.options.correlation", "program_run_id": None if idle else "program-cycle.1",
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reload_initial_scene": reload_scene,
        "reset_robot_position": reset_position,
    })
    try:
        result = runtime.reset_cycle(request)
        assert events == (["preflight", "preflight"] if reset_position else []) + (["scene", "tcp"] if reload_scene else []) + (["position"] if reset_position else [])
        assert result["idle_cycle_generation"] == 2
        assert journal.read().state == "idle_after_reset"
        assert len(admission.releases) == (0 if idle and not (reload_scene or reset_position) else 1)
        assert not journal.pending_commands()
        if not reload_scene and not reset_position:
            assert headless.applied_reads == 0
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [False, True])
def test_position_reset_refuses_before_scene_effects(monkeypatch, tmp_path, idle):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim("runner.options"))
    admission = _Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
                             headless_client=_Headless(), execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: pytest.fail("refused position Reset touched Scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.unsupported", "request_owner_epoch": "composer.1", "source_label": "operator_ui",
        "correlation_id": None, "program_run_id": None if idle else "program-cycle.1",
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reload_initial_scene": True, "reset_robot_position": True,
    })
    before = journal.read()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.reset_cycle(request)
        assert refused.value.code == "program_cycle_reset_position_unavailable"
        assert journal.read() == before
        assert not journal.pending_commands()
        assert not admission.releases
    finally:
        runtime.shutdown()


def test_idle_reload_without_applied_scenario_is_refused_before_any_effect(monkeypatch, tmp_path):
    """No Applied Scenario: Reload is refused, the cycle stays idle, no claim
    is taken, and once a Scenario is loaded the same Reset is admitted."""

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    class EmptyCell(_Headless):
        loaded = False

        def program_applied(self) -> dict[str, Any]:
            if not self.loaded:
                self.applied_reads += 1
                raise HeadlessClientError(
                    "program_applied_selection_not_found",
                    "the active configuration was not installed from a Scenario", 404,
                )
            return super().program_applied()

    admission = Admission()
    headless = EmptyCell()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
                             headless_client=headless, execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    scenes = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: scenes.append("scene"))
    monkeypatch.setattr(runtime, "_reset_persistent_keys_to_initial", lambda _request: scenes.append("keys"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.empty", "request_owner_epoch": "composer.1", "source_label": "operator_ui",
        "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "reload_initial_scene": True,
    })
    before = journal.read()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.reset_cycle(request)
        assert refused.value.code == "program_applied_selection_not_found"
        assert headless.applied_reads == 1
        # Nothing changed: still idle, no Reset phase, no claim, nothing pending.
        assert journal.read() == before
        assert runtime.cycle_status()["state"] == "idle_after_reset"
        assert journal.read().reset_effect_claim is None
        assert not admission.claims and not admission.releases
        assert not journal.pending_commands()
        assert scenes == []
        # Load stays admitted: the cycle is idle and no claim is held.
        assert admission.get()["authoring_allowed"] is True
        # After a Load, a new Reset with Reload is admitted and completes.
        headless.loaded = True
        result = runtime.reset_cycle(request.model_copy(update={"request_id": "reset.loaded"}))
        assert result["idle_cycle_generation"] == 2
        assert scenes == ["keys", "scene"]
        assert journal.read().state == "idle_after_reset"
        assert len(admission.claims) == 1 and len(admission.releases) == 1
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [False, True])
@pytest.mark.parametrize("failed_effect", ["scene", "position"])
def test_selected_reset_failure_retains_phase_and_original_options(monkeypatch, tmp_path, idle, failed_effect):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim("runner.options"))
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
                             headless_client=_Headless(), execution_transport=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    events = []
    should_fail = True
    def effect(name):
        def apply(_request, _authorization):
            events.append((name, _request.request_id))
            if name == failed_effect and should_fail:
                raise ProgramRunnerError("test_effect_refused", name)
        return apply
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", effect("scene"))
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", effect("position"))
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.original", "request_owner_epoch": "composer.1", "source_label": "operator_ui",
        "correlation_id": "reset.original.correlation", "program_run_id": None if idle else "program-cycle.1",
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reload_initial_scene": True, "reset_robot_position": True,
    })
    try:
        with pytest.raises(ProgramRunnerError) as failed:
            runtime.reset_cycle(request)
        assert failed.value.code == "program_cycle_recovery_required"
        state = runtime.cycle_status()
        assert state["state"] == "recovery_required"
        assert state["reset_phase"] == ("restoring_scene" if failed_effect == "scene" else "restoring_position")
        assert state["failure"]["failed_phase"] == state["reset_phase"]
        assert not admission.releases
        pending = journal.pending_commands()
        with pytest.raises(ProgramRunnerError) as conflict:
            runtime.reset_cycle(request.model_copy(update={"request_id": "reset.conflict", "reload_initial_scene": False}))
        assert conflict.value.code == "program_run_request_conflict"
        assert journal.pending_commands() == pending
        should_fail = False
        runtime.reset_cycle(request.model_copy(update={"request_id": "reset.confirmed", "correlation_id": "fresh.confirmation"}))
        expected = (["scene", "scene", "position"] if failed_effect == "scene" else ["scene", "position", "position"])
        assert events == [(name, "reset.original") for name in expected]
        assert journal.read().causal_correlation_id == "fresh.confirmation"
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()


def test_plain_idle_reset_preserves_scene_failure_without_observation(monkeypatch, tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    failure = {"code": "program_scene_preparation_failed", "message": "Scene owner unavailable"}
    journal.record_scene_preparation_failure(expected_generation=1, owner_epoch="runner.options", failure=failure)
    headless = _Headless()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
                             headless_client=headless, execution_transport=_Headless(),
                             admission_client=_Admission(), cycle_journal=journal)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.plain", "request_owner_epoch": "composer.1", "source_label": "operator_ui",
        "correlation_id": None, "program_run_id": None, "expected_owner_epoch": runtime.owner_epoch,
        "expected_cycle_generation": 1,
    })
    try:
        result = runtime.reset_cycle(request)
        assert result["idle_cycle_generation"] == 2
        assert journal.read().failure == failure
        status = runtime.cycle_status()
        assert status["reset_options"] is None
        assert next(row for row in status["allowed_commands"] if row["command_kind"] == "program_run")["allowed"] is False
        assert headless.applied_reads == 0
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("unknown_at", ["acquire", "release"])
def test_idle_reset_unknown_claim_outcome_retains_exact_intent_without_resend(monkeypatch, tmp_path, unknown_at):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 41, "authoring_allowed": True}
        def acquire(self, intent):
            assert journal.read().reset_effect_claim["intent"] == intent
            assert intent["expected_admission_generation"] == 41
            self.claims.append(deepcopy(intent))
            if unknown_at == "acquire":
                raise CellAdmissionError("transport_unavailable", "Acquire reply lost", outcome_unknown=True)
            return {"intent": deepcopy(intent), "claim_generation": 7,
                    "acquired_admission_generation": 41, "accepted_at_ns": 1}
        def release(self, release):
            assert journal.read().reset_effect_claim["release_intent"] == release
            self.releases.append(deepcopy(release))
            raise CellAdmissionError("transport_unavailable", "Release reply lost", outcome_unknown=True)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: effects.append("scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.unknown", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1, "reload_initial_scene": True})
    try:
        with pytest.raises(ProgramRunnerError) as failed:
            runtime.reset_cycle(request)
        assert failed.value.details == {"cause_code": "program_cycle_reconciliation_required"}
        cycle = journal.read()
        assert cycle.state == "recovery_required" and cycle.reconciliation_required
        retained = deepcopy(cycle.reset_effect_claim)
        assert retained["intent"]["expected_admission_generation"] == 41
        context = runtime._current_cycle_status_projection()["accepted_reset"]
        assert context["request_payload_digest"] == runtime._digest(request.model_dump(mode="json"))
        assert context["reset_claim_generation"] == (None if unknown_at == "acquire" else 7)
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request.model_copy(update={"request_id": "reset.fresh.confirmation"}))
        assert len(admission.claims) == 1
        assert effects == ([] if unknown_at == "acquire" else ["scene"])
        assert len(admission.releases) == (0 if unknown_at == "acquire" else 1)
        assert journal.read().reset_effect_claim == retained
    finally:
        runtime.shutdown()


class _RefusingResetAdmission(_Admission):
    """Kernel admission whose first program_reset acquire is refused (no claim)."""

    def __init__(self, journal: ProgramCycleJournal) -> None:
        super().__init__()
        self.journal = journal
        self.generation = 1
        self.refuse = True

    def get(self):
        return {"admission_generation": self.generation, "authoring_allowed": True}

    def acquire(self, intent):
        # The exact intent sent is the one durably retained before crossing.
        assert self.journal.read().reset_effect_claim["intent"] == intent
        self.claims.append(deepcopy(intent))
        if self.refuse:
            raise CellAdmissionError("program_reset_effects_not_settled", "operations not settled")
        return {"intent": deepcopy(intent), "claim_generation": 3,
                "acquired_admission_generation": self.generation, "accepted_at_ns": 1}


def _refused_idle_reset(monkeypatch, tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    admission = _RefusingResetAdmission(journal)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda request, _auth: effects.append(request.request_id))
    writes = []
    remember = journal.remember_reset_effect_claim
    def spy(**kwargs):
        writes.append(deepcopy(kwargs))
        return remember(**kwargs)
    monkeypatch.setattr(journal, "remember_reset_effect_claim", spy)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.refused", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1, "reload_initial_scene": True})
    with pytest.raises(ProgramRunnerError) as failed:
        runtime.reset_cycle(request)
    assert failed.value.details == {"cause_code": "program_reset_effects_not_settled"}
    cycle = journal.read()
    assert cycle.state == "recovery_required" and not cycle.reconciliation_required
    assert cycle.reset_phase == "restoring_scene" == cycle.failure["failed_phase"]
    retained = deepcopy(cycle.reset_effect_claim)
    assert retained["claim"] is None and retained["intent"]["expected_admission_generation"] == 1
    assert admission.claims == [retained["intent"]] and not effects
    writes.clear()
    return journal, admission, runtime, request, retained, effects, writes


@pytest.mark.parametrize("generation_advanced", [True, False])
def test_idle_reset_retry_after_refused_acquire_replays_retained_intent(
    monkeypatch, tmp_path, generation_advanced,
):
    journal, admission, runtime, request, retained, effects, writes = _refused_idle_reset(monkeypatch, tmp_path)
    try:
        if generation_advanced:
            admission.generation = 2  # the blocking operation closed
        admission.refuse = False
        runtime.reset_cycle(request.model_copy(update={"request_id": "reset.retry"}))
        assert len(admission.claims) == 2
        first, retry = admission.claims
        # The generation is evidence only: the retry never rewrites it.
        assert retry == first == retained["intent"]
        assert retry["expected_admission_generation"] == 1
        assert not [w for w in writes if w["retained"]["claim"] is None]
        assert writes[0]["expected"]["intent"] == retry
        assert effects == ["reset.refused"]
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()


def test_idle_reset_reconciliation_replays_retained_intent_without_refresh(monkeypatch, tmp_path):
    journal, admission, runtime, request, retained, effects, writes = _refused_idle_reset(monkeypatch, tmp_path)

    class ReconcilingJournal:
        """The cycle read by the acquire step reports an uncertain prior acquire."""
        def __init__(self, inner):
            self._inner = inner
        def read(self):
            return replace(self._inner.read(), reconciliation_required=True)
        def __getattr__(self, name):
            return getattr(self._inner, name)

    acquire = runtime._acquire_reset_effect_claim
    monkeypatch.setattr(runtime, "_acquire_reset_effect_claim",
                        lambda admission_client, inner: acquire(admission_client, ReconcilingJournal(inner)))
    try:
        admission.generation = 2
        admission.refuse = False
        runtime.reset_cycle(request.model_copy(update={"request_id": "reset.retry"}))
        assert admission.claims == [retained["intent"], retained["intent"]]
        assert admission.claims[1]["expected_admission_generation"] == 1
        assert not [w for w in writes if w["retained"]["claim"] is None]
        assert writes[0]["expected"] == retained
        assert effects == ["reset.refused"]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("fail_position", [False, True])
def test_position_only_idle_reset_preserves_prior_scene_failure_until_selected_reload(monkeypatch, tmp_path, fail_position):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    original = {"code": "program_scene_preparation_failed", "message": "original Scene failure"}
    journal.record_scene_preparation_failure(expected_generation=1, owner_epoch="runner.options", failure=original)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=Admission(), cycle_journal=journal)
    scene_calls = []
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    def position(*_args):
        assert journal.read().failure == original
        if fail_position:
            raise ProgramRunnerError("position_refused", "known position refusal")
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", position)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: scene_calls.append("scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.position", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1, "reset_robot_position": True})
    try:
        if fail_position:
            with pytest.raises(ProgramRunnerError):
                runtime.reset_cycle(request)
            assert journal.read().failure["prior_scene_preparation_failure"] == original
            fail_position = False
            runtime.reset_cycle(request.model_copy(update={"request_id": "reset.position.confirmed"}))
        else:
            runtime.reset_cycle(request)
        assert journal.read().failure == original and not scene_calls
        assert next(row for row in runtime.cycle_status()["allowed_commands"] if row["command_kind"] == "program_run")["allowed"] is False
        runtime.reset_cycle(request.model_copy(update={"request_id": "reset.reload", "expected_cycle_generation": 2,
            "reset_robot_position": False, "reload_initial_scene": True}))
        assert journal.read().failure is None and scene_calls == ["scene"]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("restart", [False, True])
def test_completed_reload_clears_prior_scene_fault_before_later_position_failure(monkeypatch, tmp_path, restart):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner.options")
    original = {"code": "program_scene_preparation_failed", "message": "Scene needs reload"}
    journal.record_scene_preparation_failure(expected_generation=1, owner_epoch="runner.options", failure=original)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=Admission(), cycle_journal=journal)
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: None)
    def position(*_args):
        assert journal.read().reset_phase == "restoring_position"
        assert journal.read().failure is None
        raise ProgramRunnerError("position_refused", "known position refusal")
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", position)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.both", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "reset_robot_position": True, "reload_initial_scene": True})
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        assert journal.read().failure["code"] == "position_refused"
        assert "prior_scene_preparation_failure" not in journal.read().failure
    finally:
        runtime.shutdown()
    if restart:
        reopened = ProgramCycleJournal(path, owner_epoch="runner.options")
        try:
            assert "prior_scene_preparation_failure" not in reopened.read().failure
            assert reopened.read().reset_phase == "restoring_position"
        finally:
            reopened.close()


def test_private_heartbeat_never_reads_position_capability_but_public_status_does(tmp_path):
    transport = _Headless()
    def blocked_capability():
        raise AssertionError("Private heartbeat called the capability port")
    transport.reset_robot_position_capability = blocked_capability
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=transport, admission_client=_Admission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options"))
    runtime.start()
    reads = []
    try:
        assert runtime._current_cycle_status_projection()["accepted_reset"] is None
        transport.reset_robot_position_capability = lambda: reads.append("current") or {
            "reset_robot_position": {"available": True, "reason": None}}
        assert runtime.cycle_status()["reset_robot_position"] == {"available": True, "reason": None}
        assert reads == ["current"]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("invalid", [None, "missing", "digest", "kind", "claim"])
def test_selected_reload_after_proven_never_started_run_uses_reset_claim(monkeypatch, tmp_path, invalid):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    settlement = {"schema_version": "manyforge.program_cycle_start_settlement.v1", "kind": "proven_no_effect",
                  "claim": None, "program_run_id": "never-started.1", "prepared_scene": {}}
    if invalid == "kind": settlement["kind"] = "assumed"
    if invalid == "claim": settlement["claim"] = {"claim_generation": 1}
    reference = ProgramRuntime._digest(settlement) if invalid != "digest" else _DIGEST_B
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",), state="terminal_awaiting_reset",
        program_run_id="never-started.1", terminal_reference=reference,
        start_settlement=None if invalid == "missing" else settlement)
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    def scene(_request, authorization):
        assert journal.read().program_run_id == "never-started.1"
        assert journal.read().claim is None
        assert authorization["reset_claim"]["reset_claim_generation"] == 1
        assert journal.read().reset_effect_claim["claim"]["intent"]["claim_kind"] == "program_reset"
        effects.append("scene")
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", scene)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.never-started", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": "never-started.1",
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "expected_claim_generation": None, "terminal_reference": reference, "settlement_evidence_digest": reference,
        "reload_initial_scene": True})
    try:
        if invalid is not None:
            with pytest.raises(ProgramRunnerError):
                runtime.reset_cycle(request)
            assert not effects and not admission.claims and not admission.releases
        else:
            runtime.reset_cycle(request)
            assert effects == ["scene"]
            assert len(admission.claims) == len(admission.releases) == 1
            assert journal.read().state == "idle_after_reset"
            assert journal.read().reset_effect_claim is None
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("effect_disposition", ["known_no_effect", "unknown", None])
def test_native_position_errors_preserve_known_vs_unknown_effect_policy(monkeypatch, tmp_path, effect_disposition):
    from manyforge_headless_api.native import HeadlessApiError
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    transport = _Headless()
    identity = {"owner_epoch": "kernel.1", "active_publication_generation": 3,
                "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B}
    transport.reset_robot_position_capability = lambda: {
        "reset_robot_position": {"available": True, "reason": None}, "identity": {**identity, "scene_revision": "ignored"}}
    effects = []
    def position(body):
        effects.append(body)
        raise HeadlessApiError("position_failed", "Native position failure",
            details={} if effect_disposition is None else {"effect_disposition": effect_disposition})
    transport.reset_robot_position = position
    admission = Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.options")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.options"),
        headless_client=_Headless(), execution_transport=transport, admission_client=admission, cycle_journal=journal)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_id": "reset.position", "request_owner_epoch": "caller.1",
        "source_label": "operator_ui", "correlation_id": None, "program_run_id": None,
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1, "reset_robot_position": True})
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        assert len(effects) == 1 and not admission.releases
        assert effects[0]["expected_deployment_identity"] == identity
        assert effects[0]["authorization"]["request_id"] == request.request_id
        assert effects[0]["authorization"]["reset_claim_generation"] == 1
        assert journal.read().reset_phase == "restoring_position"
        assert journal.read().reconciliation_required == (effect_disposition != "known_no_effect")
        if effect_disposition != "known_no_effect":
            assert "Restart the workbench" in journal.read().failure["message"]
            with pytest.raises(ProgramRunnerError):
                runtime.reset_cycle(request.model_copy(update={"request_id": "fresh.confirmation"}))
            assert len(effects) == 1
    finally:
        runtime.shutdown()


def test_continue_closes_releases_and_fails_the_held_node(monkeypatch, tmp_path):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
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
    owner.rows["program-cycle.1"].capture.node_policy_by_instance_id = {
        "move.2": SimpleNamespace(
            recovery_policy={"allow_intervention": True, "max_retries": 1}
        )
    }
    runtime._controls("program-cycle.1").note_retry("move.2")  # Retries spent.
    focus = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id="program-cycle.1", node_instance_id="move.1", node_attempt_id="attempt.1"
        ),
        owner_task_id="operation.1",
        policy_parameter="recovery_policy",
    )
    second = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id="program-cycle.1", node_instance_id="move.2", node_attempt_id="attempt.2"
        ),
        owner_task_id="operation.2",
        policy_parameter="recovery_policy",
    )
    lookups: list[Any] = []

    def intervention(_run, *, node_attempt_id=None):
        lookups.append(node_attempt_id)
        return second if node_attempt_id == "attempt.2" else focus

    owner.intervention = intervention
    focus_offer = {"offer_id": _DIGEST_A, "offer_revision": 1, "node_attempt_id": "attempt.1",
                   "actions": [{"action_id": "retry_runtime_step", "enabled": True}]}
    second_offer = {"offer_id": _DIGEST_B, "offer_revision": 2, "node_attempt_id": "attempt.2",
                    "actions": [{"action_id": "continue_with_failure", "enabled": True}]}
    runtime._recovery_offer = lambda *_args, **_kwargs: deepcopy(focus_offer)
    runtime._recovery_offers = lambda *_args, **_kwargs: [
        {"motion_group_ids": ["tools"], "intervention_stop_scope": "all", "offer": deepcopy(second_offer)}
    ]
    calls: list[Any] = []

    def continue_failure_pause(run_id, **kwargs):
        calls.append(("continue_failure_pause", kwargs["node_attempt_id"], kwargs["request_id"]))
        return owner.read(run_id)

    def resume_recovery(run_id, **kwargs):
        calls.append(("resume_recovery", kwargs["node_attempt_id"], kwargs["action"],
                      kwargs.get("result")))
        return owner.read(run_id)

    def forbidden(*_args, **_kwargs):
        raise AssertionError("Continue is neither a Retry nor an owner action")

    owner.continue_failure_pause = continue_failure_pause
    owner.retry_failure_pause = forbidden
    owner.apply_owner_recovery = forbidden
    host.resume_recovery = resume_recovery
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_id": "continue.1",
            "request_owner_epoch": "client.1",
            "source_label": "test",
            "expected_owner_epoch": runtime.owner_epoch,
            "offer_id": second_offer["offer_id"],
            "offer_revision": second_offer["offer_revision"],
            "action_id": "continue_with_failure",
        }
    )
    try:
        # Continue lets held motion continue: it needs motion_clear (§10),
        # refused with the readiness reason before any owner effect.
        headless = runtime._headless
        headless.readiness = {
            "can_execute": False, "can_resume": False, "motion_clear": False,
            "blocked_reasons": [{"code": "cell_latched", "message": "robot.1: offline"}],
        }
        with pytest.raises(ProgramRunnerError) as blocked:
            runtime.apply_recovery_action(request.model_copy(update={"request_id": "continue.0"}))
        assert blocked.value.code == "cell_latched"
        assert owner.attention_checks == [] and calls == []
        headless.readiness = {
            "can_execute": True, "can_resume": True, "motion_clear": True, "blocked_reasons": []}
        # Continue releases the hold's groups like Retry, so the same
        # operator-alarm attention gate refuses it before any owner effect.
        refusal = ProgramRunOwnerError("an operator-alarm condition interlock gates this motion")
        refusal.code = "operator_alarm_interlock_active"  # type: ignore[attr-defined]
        owner.attention_refusal = refusal
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.apply_recovery_action(request)
        assert refused.value.code == "operator_alarm_interlock_active"
        assert owner.attention_checks == [("program-cycle.1", "move.2", "continue.1")]
        assert calls == []
        assert journal.read().state == "running"
        assert not journal.read().reconciliation_required
        owner.attention_refusal = None
        lookups.clear()
        request = request.model_copy(update={"request_id": "continue.2"})
        result = runtime.apply_recovery_action(request)
        assert owner.attention_checks[-1] == ("program-cycle.1", "move.2", "continue.2")
        # The offer id selected the second hold, not the focused one.
        assert lookups == ["attempt.2"]
        assert calls == [
            ("continue_failure_pause", "attempt.2", "continue.2"),
            # Continue's marker (item 11: the Acknowledge of an unconfirmed Stop).
            ("resume_recovery", "attempt.2", "manual_failure", "continue_with_failure"),
        ]
        assert result["cycle_resumed"] is True
        assert result["owner_result"] == {"accepted": True, "action": "continue_with_failure"}
        # Continue counts no Retry, even with the Retry budget spent.
        assert runtime._controls("program-cycle.1").retry_count("move.2") == 1
        assert not journal.read().reconciliation_required
    finally:
        runtime.shutdown()


def _terminal(state: str, run_end: dict[str, Any], run_id: str = "program-cycle.1") -> dict[str, Any]:
    return {"state": state, "capture": {"program_run_id": run_id}, "run_end": run_end}


def _run_end_runtime(headless: _Headless) -> ProgramRuntime:
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]  # noqa: SLF001
    runtime._kernel = KernelClient(headless)  # type: ignore[arg-type]  # noqa: SLF001
    runtime._journal = None  # noqa: SLF001
    runtime._run_ends = {}  # noqa: SLF001
    return runtime


def test_a_run_that_ends_while_the_cell_is_latched_ends_by_a_cell_fault() -> None:
    """CONTRACTS §9: the latch is the error; the run end names its component.
    Every rule returns to `active_at_start` once at the end of the run."""

    headless = _Headless()
    headless.latch = {"latched": True, "causes": [
        {"component_id": "robot.1", "kind": "fault", "fault": {"code": "fault_estop"}}]}
    runtime = _run_end_runtime(headless)
    failed = {"end_reason": "failed", "details": {"node_instance_id": "move",
              "error": {"code": "interrupt_fault", "message": "stopped"}}}
    runtime._record_terminal_snapshot(_terminal("failed", failed))  # noqa: SLF001
    runtime._record_terminal_snapshot(_terminal("failed", failed))  # noqa: SLF001
    expected = {"end_reason": "cell_fault", "details": {"component_id": "robot.1"}}
    assert runtime._project_run(_terminal("failed", failed))["run_end"] == expected  # noqa: SLF001
    assert headless.rule_resets == 1


@pytest.mark.parametrize(("state", "run_end"), [
    ("succeeded", {"end_reason": "completed", "details": {}}),
    ("stopped", {"end_reason": "stopped", "details": {"stopped_by": "rule", "rule_id": "door"}}),
    ("failed", {"end_reason": "failed", "details": {"error": {"code": "x", "message": "y"}}}),
])
def test_a_run_end_without_a_latch_keeps_its_own_reason(state, run_end) -> None:
    headless = _Headless()
    runtime = _run_end_runtime(headless)
    runtime._record_terminal_snapshot(_terminal(state, run_end))  # noqa: SLF001
    assert runtime._project_run(_terminal(state, run_end))["run_end"] == run_end  # noqa: SLF001
    assert headless.rule_resets == 1
    # A running run has no end and starts no reset.
    runtime._record_terminal_snapshot(_terminal("running", None, "program-cycle.2"))  # noqa: SLF001
    assert headless.rule_resets == 1


def test_start_refuses_a_set_rule_active_naming_an_unknown_rule(monkeypatch, tmp_path) -> None:
    """CONTRACTS §9: a node naming a rule the Deployment lacks blocks Execute."""

    admitted = _PreparedRunAdmission()
    admitted.program.rule_activation_rule_ids = ("missing",)
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: admitted,
    )
    headless = _Headless()
    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch="runner.1")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", owner_epoch="runner.1"),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.start_run(_start_request())
        assert refused.value.code == "rule_unknown"
        assert refused.value.details == {"rule_id": "missing"}
        assert admission.claims == []
    finally:
        runtime.shutdown()
