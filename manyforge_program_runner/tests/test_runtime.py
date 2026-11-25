"""Production Program-runner admission and lifecycle glue."""

from __future__ import annotations

from contextlib import contextmanager
import hashlib
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from threading import Barrier, Event, Lock, Thread, get_ident
import time
from types import SimpleNamespace
from typing import Any, Mapping

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_availability import AvailabilityFact, NodeExecutionReadiness
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.orchestration_values import (
    ProgramValueStore,
)
from manyforge_behavior.program import Program
from manyforge_behavior.program_run_owner import (
    PROGRAM_RESET_STOP_REASON,
    ProgramRunOwnerError,
    ProgramRunOwnerOutcomeUnknownError,
    ProgramRunOwnerRecoveryIncompleteError,
)
from manyforge_behavior.program_scheduling import (
    ProgramRunHoldFacts,
    ProgramSchedulingError,
)
from manyforge_behavior.request_keys import (
    RUNNER_KEYS,
    KeyClass,
    Minter,
    Part,
    format_request_key,
    key_class,
    key_origin,
    parse_request_key,
    with_part,
)
from manyforge_headless_api.models import (
    CycleRecoveryActionRequest,
    ProgramHaltResumeRequest,
    ProgramRunStartRequest,
    ProgramManualHandoverRequest,
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
from manyforge_headless_api.component_model import run_cycle_facts
from manyforge_program_runner.cell_admission import CellAdmissionError
from manyforge_program_runner.bbkey_publication import (
    FAULT_CODE as BBKEY_PUBLICATION_FAULT_CODE,
)
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.runtime_controls import (
    ProgramRuntimeControls,
)
from manyforge_program_runner.models import (
    ManualResultStageRequest,
    PersistentKeyValuesClearRequest,
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
)
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
# ``A`` of the test start: the runner's launch value and its text form.
_A = 0x3F9A00000000002A
_A_TEXT = "3f9a00000000002a"


def _kb(*parts: Any) -> bytes:
    """A deterministic 16-byte request key named by text, for readable tests."""

    return hashlib.md5("/".join(map(str, parts)).encode()).digest()


def _key(*parts: Any) -> str:
    """The wire form (32 lowercase hex) of ``_kb``."""

    return _kb(*parts).hex()
_ROOT = Path(__file__).resolve().parents[2]
def _hold_facts(
    program_run_id: int,
    hold_revision: int,
    *,
    frozen: str = "halt",
    event_id: int | None = None,
) -> ProgramRunHoldFacts:
    """The Kernel's ``program_run_hold`` facts as the stream state carries
    them (with its Stop markers)."""

    return ProgramRunHoldFacts.from_wire(
        {
            "program_run_id": program_run_id,
            "hold_revision": hold_revision,
            "frozen": frozen,
            "frozen_by": None if frozen == "none" else event_id,
            "all_groups_held": False,
            "held_by_rule": None,
            "frozen_kind": None if frozen == "none" else frozen,
            "frozen_source_label": None,
        },
        last_stop_event_id=event_id,
        stop_progress_sequence=0,
    )


_ACTIVE_STATES = {
    "running",
    "intervention_required",
    "halting",
    "halted",
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
        "read_stop",
        "submit_component_lifecycle",
        "submit_route_selection",
        "route_selection_status",
        "route_selection_request_result",
    }

    def __init__(self) -> None:
        self.applied_reads = 0
        self.registered_captures: list[dict[str, Any]] = []
        # RunReadiness and CellLatch, as served by the API.
        self.readiness: dict[str, Any] = {
            "can_execute": True, "can_resume": True, "motion_clear": True, "blocked_reasons": []}
        self.latch: dict[str, Any] = {"latched": False, "causes": []}
        self.readiness_reads = 0
        # The runner's cycle facts sent with each readiness request.
        self.readiness_cycles: list[dict[str, Any]] = []
        self.rule_resets = 0
        self.rule_activations: list[tuple[str, bool]] = []
        self.outside_clears: list[tuple[str, str]] = []
        self.output_writes: list[tuple[str, Any]] = []
        # Node-attempt history batches the runner's background publisher sends.
        self.node_attempt_batches: list[dict[str, Any]] = []

    def apply_tool_payload(self) -> None:
        pass

    # The execution gateway's commands (one method per wire command, taking
    # its request members).
    def reset_active_tcp(self, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
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

    def committed_scenario_activation(self) -> dict[str, Any]:
        # The Apply number names the scene context of a preparation that
        # belongs to no run (1.16.5).
        return {"apply_id": 1, "apply_generation": 1, "record_digest": _DIGEST_B,
                "scene_initialization": "initialize", "origin": {"kind": "scenario"}}

    def request(
        self, method: str, path: str, *, body: Mapping[str, Any] | None = None
    ) -> dict[str, Any]:
        if path == "/api/program/readiness":
            assert method == "POST"
            assert body is not None
            assert body["schema_version"] == "manyforge.run_readiness_request.v1"
            self.readiness_reads += 1
            self.readiness_cycles.append(deepcopy(body["program_cycle"]))
            return deepcopy(self.readiness)
        assert method == "GET" and body is None
        assert path == "/api/cell/latch"
        return deepcopy(self.latch)

    def identity(self, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "application_start_id": "3f9a00000000002a",
                "program_binding_id": None,
                "retained_bundle_id": _DIGEST_A,
                "graph_revision": _DIGEST_B,
                "active_publication_generation": 1,
            },
        }

    def targets(self, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
        """The execution owner's targets; tests set rows in ``target_components``."""
        return {
            "ok": True,
            "targets": {
                "components": deepcopy(getattr(self, "target_components", [])),
                "schema_version": "manyforge.execution_targets.v3",
            },
        }

    def register_program_run_capture(self, members: Mapping[str, Any]) -> dict[str, Any]:
        body = members["capture"]
        self.registered_captures.append(deepcopy(dict(body)))
        return {
            "ok": True,
            "command": "register_program_run_capture",
            "program_run_id": body["program_run_id"],
            "claim_generation": body["claim_generation"],
        }

    def reset_rule_activation(self, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
        self.rule_resets += 1
        return {"ok": True, "command": "reset_rule_activation"}

    def set_rule_active(self, members: Mapping[str, Any]) -> dict[str, Any]:
        self.rule_activations.append((members["rule_id"], members["active"]))
        return {"ok": True, "command": "set_rule_active"}

    def clear_outside_change(self, members: Mapping[str, Any]) -> dict[str, Any]:
        self.outside_clears.append(
            (members["controllable_component_id"], members["signal_name"])
        )
        return {"ok": True, "command": "clear_outside_change"}

    def set_io_output(self, members: Mapping[str, Any]) -> dict[str, Any]:
        request = members["request"]
        assert request["origin"] == "program"
        self.output_writes.append((request["signal_id"], request["value"]))
        return {"ok": True, "command": "set_io_output", "result": {"outcome": "succeeded"}}

    def record_program_node_attempts(self, batch: Mapping[str, Any]) -> dict[str, Any]:
        """The Historian's answer: every attempt and run event recorded."""

        self.node_attempt_batches.append(dict(batch))
        return {
            "schema_version": "manyforge.program_node_attempt_record.v1",
            "recorded": len(batch["attempts"]) + len(batch["run_events"]),
        }

    def __getattr__(self, name: str):
        if name not in self._PORT_METHODS:
            raise AttributeError(name)

        def unexpected(*_args: object, **_kwargs: object) -> None:
            raise AssertionError(f"unexpected effectful headless call: {name}")

        return unexpected


def test_run_readiness_refuses_with_its_first_reason() -> None:
    """The Runner reads RunReadiness and computes nothing."""

    headless = _Headless()
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]
    runtime.cycle_projection = lambda: _cycle_status("idle_after_reset")  # type: ignore[method-assign]
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


def _cycle_status(state: str, *, resume: bool = False) -> dict[str, Any]:
    """The part of the runner's cycle status that readiness reads."""

    return {"state": state, "allowed_commands": [
        {"command_kind": "program_resume_halt", "allowed": resume}]}


def test_run_readiness_is_asked_with_the_runners_current_cycle_facts() -> None:
    """Reset then Execute, and Stop then Resume, are judged on the facts the
    runner holds at the moment it asks, never on an earlier published frame."""

    headless = _Headless()
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]
    cycle = {"status": _cycle_status("terminal_awaiting_reset")}
    runtime.cycle_projection = lambda: cycle["status"]  # type: ignore[method-assign]
    # A Reset completes; Execute right after it asks with the idle cycle.
    cycle["status"] = _cycle_status("idle_after_reset")
    runtime._require_run_readiness("can_execute")
    # A Stop holds the run; Resume right after it asks with Resume allowed.
    cycle["status"] = _cycle_status("running", resume=True)
    runtime._require_run_readiness("can_resume")
    assert headless.readiness_cycles == [
        {"state": "idle_after_reset", "resume_halt_allowed": False},
        {"state": "running", "resume_halt_allowed": True},
    ]


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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
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
        state: str,
        revision: int,
    ) -> None:
        capture = {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": program_run_id,
            "applied_record_digest": _DIGEST_B,
            "scenario_id": "scenario.1",
            "source_document_digest": _DIGEST_A,
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
            "schema_version": "manyforge.program_run_status.v5",
            "owner_id": owner_id,
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
            "halt": (
                {"kind": "halt", "source_label": "operator.halt"}
                if state in {"halting", "halted"}
                else None
            ),
        }

    @property
    def state(self) -> str:
        return self._value["state"]

    @property
    def run_revision(self) -> int:
        return self._value["run_revision"]

    @property
    def node_status_revision(self) -> int:
        return self._value["node_status_revision"]

    @property
    def node_statuses(self) -> dict[str, str]:
        return dict(self._value["node_statuses"])

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
        self.wait_values: dict[str, dict[str, Any]] = {}

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

    def has_run(self, program_run_id: str) -> bool:
        return program_run_id in self.rows

    def intervention(self, _program_run_id: str, *, node_attempt_id=None):
        return None

    def failure_holds(self, _program_run_id: str):
        return ()

    def operator_questions(self, _program_run_id: str):
        # The open operator questions the run's tick last published (OD-342).
        return ()

    def hold_motion_groups(self, _program_run_id: str, _node_instance_id: str):
        return ("all",), "all"

    def take_node_history_events(self, _program_run_id: str):
        return ()

    def wait_observations(self, _program_run_id: str):
        return self.wait_values

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
            "hold_revision": 0,
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


class _KeyValuesOwner(_Owner):
    def read_runtime_blackboard(self, program_run_id: int):
        if program_run_id not in self.rows:
            raise ProgramRunOwnerError("Program run does not exist")
        return self.rows[program_run_id], [
            {"key": "count", "type": "int64", "value_state": "value", "value": 2},
        ]


class _Host:
    def __init__(self, owner: _Owner, *, owner_id: str) -> None:
        self.owner = owner
        self.owner_id = owner_id
        self.started = 0
        self.start_admissions: list[object] = []
        self.scheduled: list[str] = []
        self.stop_calls: list[tuple[int, str | None]] = []
        self.stop_keys: list[bytes | None] = []
        self.applied_hold_facts: list[object] = []
        self.motion_start_calls: list[tuple[str, object, int, int, str]] = []
        self.halt_calls: list[tuple[int, int]] = []
        self.halt_resume_calls: list[tuple[int, str | None]] = []
        self.reset_calls: list[str] = []
        self.reset_containment_calls: list[str] = []
        self.retained_reset_calls: list[str] = []
        self.forgotten_reset_runs: list[str] = []
        self.cleanup_retries: list[int] = []
        self.shutdowns = 0

    def start(self) -> None:
        self.started += 1

    def start_run(
        self,
        admission: object,
        *,
        program_run_id: int | None = None,
        defer_ticks: bool = False,
    ) -> _Snapshot:
        assert defer_ticks
        self.start_admissions.append(admission)
        admitted_program_run_id = (
            len(self.start_admissions)
            if program_run_id is None
            else program_run_id
        )
        snapshot = _Snapshot(
            admitted_program_run_id,
            owner_id=self.owner_id,
            state="running",
            revision=1,
        )
        self.owner.rows[admitted_program_run_id] = snapshot
        return snapshot

    def schedule(self, program_run_id: int) -> None:
        self.scheduled.append(program_run_id)

    def retry_cleanup(self, program_run_id: int) -> None:
        self.cleanup_retries.append(program_run_id)

    def stop_run(
        self,
        program_run_id: int,
        *,
        reason: str | None,
        stop_request_key: bytes | None = None,
    ) -> _Snapshot:
        self.stop_calls.append((program_run_id, reason))
        self.stop_keys.append(stop_request_key)
        current = self.owner.rows.get(program_run_id)
        if current is not None and current.state in {"stopped", "succeeded", "failed"}:
            return current
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            state="stopped",
            revision=1 if current is None else current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def wait_terminal(self, program_run_id: int) -> _Snapshot:
        return self.owner.rows[program_run_id]

    def contain_reset_run(self, program_run_id, *, stop_request_key):
        assert len(stop_request_key) == 16
        self.reset_containment_calls.append(program_run_id)
        return self.owner.rows[program_run_id]

    def start_motion(
        self,
        program_run_id: int,
        *,
        expected_hold_revision: int,
        request_key: bytes,
        group_id: str | list[str] = "all",
        reason: str | None = None,
    ) -> object:
        del reason
        self.motion_start_calls.append(
            (
                program_run_id,
                group_id,
                expected_hold_revision,
                request_key,
            )
        )
        current = self.owner.rows[program_run_id]
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            state="running",
            revision=current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return SimpleNamespace(
            state=SimpleNamespace(value="running"),
            run_revision=snapshot.run_revision,
        )

    def apply_hold_facts(self, facts) -> None:
        """The scheduling host's facts, reduced to what these tests read: the
        run's tick follows a Halt or runner-loss freeze (Halting and Halted
        folded into one step here)."""

        self.applied_hold_facts.append(facts)
        current = self.owner.rows.get(facts.program_run_id)
        if (
            facts.frozen in {"halt", "runner_loss"}
            and current is not None
            and current.state in {"running", "intervention_required"}
        ):
            self.halt_calls.append((facts.program_run_id, facts.hold_revision))
            self.owner.rows[facts.program_run_id] = _Snapshot(
                facts.program_run_id,
                owner_id=self.owner_id,
                state="halted",
                revision=current.run_revision + 1,
            )

    def resume_halt(
        self,
        program_run_id: int,
        *,
        reason: str | None,
    ) -> _Snapshot:
        self.halt_resume_calls.append((program_run_id, reason))
        current = self.owner.rows[program_run_id]
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            state="running",
            revision=current.run_revision + 1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def reset_run(self, program_run_id: int) -> _Snapshot:
        self.reset_calls.append(program_run_id)
        return self.owner.rows.get(
            program_run_id,
            _Snapshot(
                program_run_id,
                owner_id=self.owner_id,
                state="stopped",
                revision=2,
            ),
        )

    def reset_retained_run(self, program_run_id: int) -> None:
        self.retained_reset_calls.append(program_run_id)

    def forget_reset_run(self, program_run_id: int) -> None:
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
            "admission_generation": 1,
            "held_claims": [],
            "matched_claim": None,
            "matched_release": None,
            "status": "unknown",
        }

    def get(self) -> dict[str, Any]:
        return {"admission_generation": 1}

    def acquire(
        self, intent: dict[str, Any], *, program_cycle_status: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        # Only a Program Reset claim carries the runner's cycle status (OD-139).
        assert (intent["claim_kind"] == "program_reset") == (program_cycle_status is not None)
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

    def recovery_receipt(self, _query: dict[str, Any]) -> dict[str, Any]:
        return deepcopy(self.inventory)


def _reconcile_runtime(
    tmp_path,
    *,
    inventory: dict[str, Any],
) -> tuple[ProgramRuntime, ProgramCycleJournal, _Admission]:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    admission = _Admission()
    admission.inventory = deepcopy(inventory)
    execution_run_root = tmp_path / "execution-run"
    execution_run_root.mkdir(exist_ok=True)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
            execution_run_root=execution_run_root,
            cycle_journal_path=tmp_path / "cycle.sqlite3",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    return runtime, journal, admission


def test_shutdown_waits_for_the_in_flight_diagnostic_delivery(tmp_path):
    """OD-309: no join limit; the delivery's own end ends the shutdown wait."""
    from manyforge_program_runner.diagnostics import ProgramDiagnosticPublisher

    runtime, _journal, _admission = _reconcile_runtime(
        tmp_path,
        inventory={},
    )
    entered, answer = Event(), Event()
    delivered: list[dict[str, Any]] = []

    def send(record):
        entered.set()
        answer.wait()
        delivered.append(record)
        return {"recorded": True}

    publisher = ProgramDiagnosticPublisher(send)
    runtime._diagnostics = publisher
    publisher.submit({"state": "succeeded"})
    assert entered.wait(timeout=5)
    shutdown_errors: list[BaseException] = []

    def shut_down() -> None:
        try:
            runtime.shutdown()
        except BaseException as error:  # pragma: no cover - reported below
            shutdown_errors.append(error)

    closer = Thread(target=shut_down, name="test-runtime-shutdown")
    closer.start()
    # Shutdown is held by the delivery, not by a timer: it has marked the
    # publisher closed and waits on its thread.
    with publisher._condition:
        assert publisher._condition.wait_for(lambda: publisher._closed, timeout=5)
    assert closer.is_alive() and not delivered
    answer.set()  # Headless answers
    closer.join(timeout=5)
    assert not closer.is_alive() and not shutdown_errors
    assert len(delivered) == 1
    assert not publisher._thread.is_alive()
    assert not publisher.health()["delivery_failed"]


def _program_cycle_claim() -> dict[str, Any]:
    intent = {
        "claim_kind": "program_cycle",
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "request_key": _key("start.1"),
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
        "schema_version": "manyforge.program_cycle_start_settlement.v2",
        "kind": "proven_no_effect",
        "claim": claim,
        "program_run_id": 1,
        "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
    }
    reference = ProgramRuntime._digest(settlement)  # noqa: SLF001
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="terminal_awaiting_reset",
        program_run_id=1,
        claim=claim,
        terminal_reference=reference,
        start_settlement=settlement,
    )
    return reference


def _start_request(*, request_id: str = "request.start.1") -> ProgramRunStartRequest:
    return ProgramRunStartRequest.model_validate(
        {
            "request_key": _key(request_id),
            "source_label": "operator_ui",
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
        "schema_version": "manyforge.program_cycle_start_settlement.v2",
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

    def port_context(self, program_run_id: int, *, context_kind: str = "run") -> object:
        return SimpleNamespace(
            program_run_id=program_run_id,
            context_kind=context_kind,
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
            return {"program_revision": None, "record_digest": _DIGEST_B,
                    "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B}

        def committed_scenario_activation(self):
            return {"apply_id": 1, "apply_generation": 1,
                    "record_digest": _DIGEST_B, "scene_initialization": "initialize",
                    "origin": {"kind": "scenario"}}

    headless = EmptyHeadless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless, gateway=_Headless(), admission_client=IdleAdmission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    calls = []
    def clear(**kwargs):
        calls.append(kwargs)
        assert kwargs["is_current"]()
        return {"state": "succeeded", "last_observed_remaining": [], "refused": []}
    monkeypatch.setattr(runtime._scene, "clear_applied_scene", clear)
    runtime.start()
    result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
        "request_key": _key("apply-scene.1"),
        "source_label": "headless_apply_completion",
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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless, gateway=_Headless(), admission_client=IdleAdmission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    def clear(**kwargs):
        assert kwargs["is_current"]()
        headless.generation = 2
        assert not kwargs["is_current"]()
        return {"state": "superseded", "last_observed_remaining": ["part"]}
    monkeypatch.setattr(runtime._scene, "clear_applied_scene", clear)
    runtime.start()
    result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
        "request_key": _key("clear.1"),
        "source_label": "composer_ui",
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
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    prepared: list[tuple[str, int]] = []
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "prepare_initial_scene",
        lambda context, _scene: (
            prepared.append((context.context_kind, context.program_run_id))
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
                "request_key": _key("activation.1"),
                "source_label": "manyforge_composer_activation",
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 1,
            }
        )
    )

    assert result["status"] == "prepared"
    assert result["admission_generation"] == 1
    # The idle preparation's scene context is its Apply number, not a run.
    assert prepared and prepared[0][0] == "apply"
    assert type(prepared[0][1]) is int and prepared[0][1] >= 1
    assert admission.claims == []
    status = runtime.cycle_status()
    assert status["state"] == "idle_after_reset"
    availability = {row["command_kind"]: row for row in status["allowed_commands"]}
    assert availability["execution_recovery"]["allowed"] is False
    runtime.shutdown()


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
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
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
        "request_key": _key("applied.scene"),
        "source_label": "composer",
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless, gateway=_Headless(), admission_client=admission,
        cycle_journal=journal,
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id)
    monkeypatch.setattr(runtime._scene, "prepare_initial_scene",
                        lambda *_args: {"schema_version": "manyforge.prepared_program_scene.v1"})
    monkeypatch.setattr(runtime._scene, "verify_initial_scene", lambda *_args: None)
    runtime.start()
    try:
        for generation in (1, 2):
            result = runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
                "request_key": _key("idle.reset." + str(generation)),
                "source_label": "operator_ui", "program_run_id": None,
                "expected_cycle_generation": generation,
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
    runtime._carried_object_losses_unusable = False  # noqa: SLF001
    run_ids = tuple(stores) if isinstance(stores, Mapping) else (1,)
    runtime._runtime_controls_lock = Lock()  # noqa: SLF001
    runtime._runtime_controls = {  # noqa: SLF001
        run_id: ProgramRuntimeControls(
            {
                "program_run_id": run_id,
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
        catalog=RuleCatalog(None, 1, _DIGEST_A, _DIGEST_B, (), ()),
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
        api_version="manyforge.program.v4",
    )
    return Program.from_dict(document, catalog)


def test_carried_object_loss_known_last_losses_at_run_start_are_already_seen() -> None:
    loss = _carried_object_last_loss("loss.before-run")
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, _snapshot = _carried_object_loss_runtime((loss,), store)

    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert runtime._carried_object_lost_seen_event_ids == {"loss.before-run"}  # noqa: SLF001
    assert store.writes == []


def test_carried_object_lost_unknown_at_run_start_reports_first_known_losses() -> None:
    loss = _carried_object_last_loss("loss.after-unknown-start")
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, snapshot = _carried_object_loss_runtime(None, store)

    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001
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
    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001

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
        read_events=lambda _after: {},
        on_change=lambda _states: None,
        on_carried_object_lost=runtime._mirror_carried_object_lost_signals,  # noqa: SLF001
    )
    runtime._interrupt_state_mirror = mirror  # noqa: SLF001

    def apply_state(last_losses: tuple[Mapping[str, Any], ...]) -> None:
        mirror.apply_stream_message(
            {
                "type": "snapshot" if mirror.carried_object_last_losses() is None else "state",
                "cursor": 0,
                "reason": "subscribed",
                "state": {
                    "interrupt_rule_catalog": None,
                    "interrupt_rule_state_baseline": None,
                    "carried_object_lost_signals": [],
                    "carried_object_last_losses": list(last_losses),
                    "program_run_hold": None,
                    "last_stop_event_id": None,
                    "stop_progress_sequence": 0,
                },
            }
        )

    apply_state(())
    assert mirror.carried_object_lost_signals() == ()
    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001

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

    mirror._stream_lost(RuntimeError("dead link"))  # noqa: SLF001
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
            assert runtime._carried_object_lost_run_id == 1  # noqa: SLF001
            assert runtime._interrupt_state_run_id is None  # noqa: SLF001  (not exposed before admission)
            assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
            state["last_losses"] = (loss,)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert store.writes == []
            intercepted.append(True)
        original_transition(**kwargs)

    original_complete_request = journal.complete_request
    completed: list[bool] = []

    def complete_request(**kwargs: Any) -> Any:
        # The staged loss is still unwritten when admission completes.
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
    """A raising admission completion leaves the staged loss unwritten."""

    runtime, journal, _admission, owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch,
        tmp_path,
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
        assert bindings_at_complete[0] == 1
        assert store.writes == []
        assert runtime._carried_object_lost_run_id is None  # noqa: SLF001
        assert runtime._carried_object_lost_run_active is False  # noqa: SLF001
        # A later known loss state cannot reach the retired binding.
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
        assert store.writes == []
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
        finish_cycle=False,
    )
    motion = {
        "command_target_id": "arm",
        "operation_capability_id": "execution.trajectory.execute",
        "presentation": "motion",
        "command": {"tcp_frame": None},
    }
    runtime._kernel._gateway.target_components = [  # noqa: SLF001
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
        motion["command"]["tcp_frame"] = {"component_id": "lite_gripper", "frame_id": "tcp"}
        runtime._kernel._gateway.target_components = [  # noqa: SLF001
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
        finish_cycle=False,
    )
    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    state = _install_startable_loss_mirror(runtime, owner, store)

    def fail_schedule(program_run_id: str) -> None:
        assert program_run_id == 1
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
            program_run_id: int, *, reason: str | None, stop_request_key: bytes | None = None
        ) -> _Snapshot:
            # The Forced Program End is sent while the cycle still runs.
            assert journal.read().state == "running"
            state["last_losses"] = (during_stop,)
            runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
            assert [write[:2] for write in store.writes] == [("part_lost", True)]
            return original_stop_run(
                program_run_id, reason=reason, stop_request_key=stop_request_key
            )

        monkeypatch.setattr(host, "stop_run", stop_while_loss_arrives)
        # No client can stop a run through the runner any more: the run
        # reaches its stopped terminal through an internal Forced Program End
        # (here the BB-key publication fault's).
        runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
        assert journal.read_run_snapshot(1)["state"] == "stopped"
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
        assert runtime._carried_object_lost_run_id == 1  # noqa: SLF001

        # The explicit Run Reset retires the consumer binding.
        runtime._reset_bbkey_publication(1)  # noqa: SLF001
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
        finish_cycle=False,
    )
    program = _real_persistent_loss_program()
    project_path = "/projects/terminal-loss-persistence.scenario"
    register = PersistentKeyRegister(tmp_path, application_start_id=_A_TEXT)
    activation_digest = "sha256:" + "c" * 64
    register.reconcile(
        CommittedActivation(
            record_digest=activation_digest,
            project_path=project_path,
            declarations=program.blackboard_keys,
            apply_id=1,
        )
    )
    current_store = ProgramValueStore.from_program(
        program,
        project_path=project_path,
        register=register,
        program_run_id=1,
    )
    state = _install_startable_loss_mirror(runtime, owner, current_store)
    try:
        runtime.start_run(_start_request())
        # The stopped terminal through an internal Forced Program End.
        runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
        assert journal.read_run_snapshot(1)["state"] == "stopped"
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
            program_run_id=2,
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
    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001

    _snapshot["last_losses"] = (row,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert store.writes == []
    assert runtime._carried_object_lost_seen_event_ids == {"loss.invalid-key"}  # noqa: SLF001
    assert "not a writable bool key" in caplog.text


def test_unusable_last_losses_log_once_per_episode(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """OD-381 L4: every mirror message re-reads the last losses; an unusable
    set is one line per episode, not one per message."""

    store = _CarriedObjectLossStore({"part_lost": _writable_bool_declaration()})
    runtime, snapshot = _carried_object_loss_runtime((), store)
    caplog.set_level("INFO", logger="manyforge_program_runner.runtime")
    snapshot["last_losses"] = ({"loss_event_id": ""},)
    for _ in range(5):
        runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    snapshot["last_losses"] = ()
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    snapshot["last_losses"] = ({"loss_event_id": ""},)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert [record.getMessage() for record in caplog.records] == [
        "carried-object last losses are not usable",
        "carried-object last losses are usable again",
        "carried-object last losses are not usable",
    ]


def test_old_attachment_loss_during_new_run_sets_current_key() -> None:
    prior_loss = _carried_object_last_loss("loss.first-run")
    loss_during_new_run = _carried_object_last_loss("loss.second-run")
    declarations = {"part_lost": _writable_bool_declaration()}
    old_run_store = _CarriedObjectLossStore(declarations)
    current_run_store = _CarriedObjectLossStore(declarations)
    runtime, snapshot = _carried_object_loss_runtime(
        (), {1: old_run_store, 2: current_run_store}
    )

    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001
    snapshot["last_losses"] = (prior_loss,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001
    assert [write[2]["loss_event_id"] for write in old_run_store.writes] == [
        "loss.first-run"
    ]

    # At the next Run boundary, the retained last-loss entry is already
    # baseline state. A later event on the earlier-Run attachment updates the
    # same per-key entry and belongs to Run 2.
    runtime._begin_carried_object_lost_run(2)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(2)  # noqa: SLF001
    snapshot["last_losses"] = (loss_during_new_run,)
    runtime._mirror_carried_object_lost_signals(())  # noqa: SLF001

    assert [write[2]["loss_event_id"] for write in old_run_store.writes] == [
        "loss.first-run"
    ]
    assert [write[2]["loss_event_id"] for write in current_run_store.writes] == [
        "loss.second-run"
    ]


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
    runtime._begin_carried_object_lost_run(1)  # noqa: SLF001
    runtime._activate_carried_object_lost_run(1)  # noqa: SLF001

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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=admission,
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id)
    monkeypatch.setattr(runtime._scene, "prepare_initial_scene",
                        lambda *_args: (_ for _ in ()).throw(RuntimeError("scene unavailable")))
    runtime.start()
    preparation = ProgramScenePreparationRequest.model_validate({
        "request_key": _key("apply.scene.failure"),
        "source_label": "composer",
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
        "request_key": _key("idle.reset"), "reload_initial_scene": True,
        "source_label": "operator_ui",
        "program_run_id": None,
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
    # A Reset claim has no owner transaction number; the Run's is its number.
    reset_claim, run_claim = admission.claims
    assert reset_claim["owner_transaction_id"] is None
    assert reset_claim["request_key"] == format_request_key(
        with_part(parse_request_key(reset.request_key), Part.RESET_CLAIM)
    )
    assert run_claim["owner_transaction_id"] == runtime.cycle_status()["program_run_id"]
    assert set(reset_claim) == set(run_claim) == {
        "claim_kind", "owner_role", "owner_transaction_id", "request_key",
        "expected_admission_generation", "scope", "program_cycle_evidence",
        "intervention_evidence",
    }
    assert set(admission.releases[0]) == {
        "owner_role", "owner_transaction_id", "request_key",
        "acquired_admission_generation", "claim_generation", "settlement",
    }
    assert admission.releases[0]["owner_transaction_id"] is None
    runtime.shutdown()


def _terminal_cycle_reset_runtime(
    monkeypatch, tmp_path, *, finish_cycle: bool = True
):
    """A public Run, then the run's internal Forced Program End, with
    deterministic owner observations."""

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", application_start_id=_A
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    if not finish_cycle:
        return runtime, runtime._journal, admission, owner, host  # noqa: SLF001
    runtime.start_run(_start_request())
    # The runner has no Stop route: the stopped terminal comes from an
    # internal Forced Program End (the BB-key publication fault's).
    runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
    assert runtime._journal.read_run_snapshot(1)["state"] == "stopped"  # noqa: SLF001
    assert runtime.cycle_status()["state"] == "terminal_awaiting_reset"
    return runtime, runtime._journal, admission, owner, host  # noqa: SLF001


def test_program_runtime_reset_from_terminal_partial_stop_releases_cycle_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path
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
        # The release copies the Run claim's run number; it carries no digest.
        assert admission.releases[0]["owner_transaction_id"] == 1
        assert "request_payload_digest" not in admission.releases[0]
        assert host.reset_calls == [1]
        assert host.forgotten_reset_runs == [1]
        assert 1 not in owner.rows
    finally:
        runtime.shutdown()


def test_program_runtime_reset_retry_from_recovery_required_retires_same_cycle(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, journal, admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path
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
            "admission_generation": 1,
            "held_claims": [claim],
            "matched_claim": claim,
            "matched_release": None,
            "status": "held_exact",
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
            1,
            "operator requested Program cycle Reset",
        )
        assert host.reset_calls == [1]
        assert host.forgotten_reset_runs == [1]
        assert 1 not in owner.rows
    finally:
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
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
        stop_request_key: bytes | None = None,
    ) -> _Snapshot:
        assert reason == PROGRAM_RESET_STOP_REASON
        # The Reset's Forced Program End: part 1 of the Reset's key.
        assert stop_request_key == with_part(
            _kb("request.reset.running"), Part.RESET_FORCED_END
        )
        current = owner.rows[program_run_id]
        stopping = _Snapshot(
            program_run_id,
            owner_id=runtime.owner_id,
            state="succeeded" if completion_wins_before_stop else "stopping",
            revision=current.run_revision + 1,
        )
        owner.rows[program_run_id] = stopping
        return stopping

    def wait_terminal(program_run_id: str) -> _Snapshot:
        settlement_wait_entered.set()
        assert permit_settlement.wait(timeout=1.0)
        stopped = _Snapshot(
            program_run_id,
            owner_id=runtime.owner_id,
            state="stopped",
            revision=owner.rows[program_run_id].run_revision + 1,
        )
        owner.rows[program_run_id] = stopped
        return stopped

    host.stop_run = begin_stop  # type: ignore[method-assign]
    host.wait_terminal = wait_terminal  # type: ignore[method-assign]

    def contain_terminal(program_run_id, *, stop_request_key):
        assert stop_request_key == with_part(
            _kb("request.reset.running"), Part.RESET_FORCED_END
        )
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
            "request_key": _key("request.reset.running"),
            "source_label": "operator_ui",
            "program_run_id": 1,
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
    assert "program_stop" not in availability  # The public Program Stop is gone.
    assert availability["program_reset"]["allowed"] is False
    assert host.reset_calls == [] and admission.releases == []

    permit_settlement.set()
    thread.join(timeout=1.0)
    assert not thread.is_alive()
    assert len(outcomes) == 1 and isinstance(outcomes[0], dict)
    assert outcomes[0]["idle_cycle_generation"] == 2
    assert host.reset_containment_calls == [1]
    assert runtime.cycle_status()["state"] == "idle_after_reset"
    runtime.shutdown()


def test_reset_fails_typed_on_failed_cleanup_and_the_next_reset_retries_it(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """OD-279: a failed run cleanup ends the Reset failed with its typed
    cause (no wait without end); the next Reset retries the cleanup once."""
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    runtime.start_run(_start_request())
    cleanup = {"fails": True}

    def finalizing_stop(program_run_id, *, reason, stop_request_key=None):
        del reason, stop_request_key
        owner.rows[program_run_id] = _Snapshot(
            program_run_id, owner_id=runtime.owner_id, state="finalizing",
            revision=owner.rows[program_run_id].run_revision + 1,
        )
        return owner.rows[program_run_id]

    def wait_terminal(program_run_id):
        if cleanup["fails"]:
            raise ProgramSchedulingError(
                "context unbind failed", code="program_run_cleanup_failed"
            )
        owner.rows[program_run_id] = _Snapshot(
            program_run_id, owner_id=runtime.owner_id, state="stopped",
            revision=owner.rows[program_run_id].run_revision + 1,
        )
        return owner.rows[program_run_id]

    host.stop_run = finalizing_stop  # type: ignore[method-assign]
    host.wait_terminal = wait_terminal  # type: ignore[method-assign]
    try:
        with pytest.raises(ProgramRunnerError) as failed:
            runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
        assert failed.value.code == "program_cycle_recovery_required"
        assert failed.value.details == {"cause_code": "program_run_cleanup_failed"}
        cycle = journal.read()
        assert cycle.state == "recovery_required"
        assert not cycle.reconciliation_required
        assert cycle.failure["code"] == "program_run_cleanup_failed"
        assert host.cleanup_retries == [1]

        cleanup["fails"] = False
        reset = runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.1"))
        assert reset["idle_cycle_generation"] == 2
        assert host.cleanup_retries == [1, 1]
        assert runtime.cycle_status()["state"] == "idle_after_reset"
    finally:
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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=admission,
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id)
    for method in ("prepare_initial_scene", "verify_initial_scene"):
        monkeypatch.setattr(runtime._scene, method,
                            lambda *_args: pytest.fail("Run edited or compared the Scene"))
    runtime.start()
    runtime.start_run(_start_request())
    assert runtime.cycle_status()["state"] == "running"
    assert len(admission.claims) == 1
    runtime.shutdown()


def test_run_status_shows_since_when_nodes_run_and_what_waits_read_in_memory_only(
    monkeypatch: pytest.MonkeyPatch, tmp_path,
) -> None:
    """OD-382 C6: the served run status (v6) names since when each running
    node runs (stamped when the observer first sees it running) and what
    each running wait last read; neither reaches the cycle journal."""
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=_Admission(), cycle_journal=journal,
    )
    runtime._host.shutdown()
    owner = _ObservationOwner()
    runtime._owner = owner
    runtime._host = _Host(owner, owner_id=runtime.owner_id)
    try:
        runtime.start()
        runtime.start_run(_start_request())
        run_id = runtime.cycle_status()["program_run_id"]
        before = time.time_ns()
        snapshot = _Snapshot(run_id, owner_id=runtime.owner_id, state="running",
                             revision=owner.rows[run_id].run_revision + 1)
        snapshot._value["node_statuses"] = {"root": "running", "wait": "running", "done": "success"}
        owner.rows[run_id] = snapshot
        owner.wait_values = {
            "wait": {"kind": "key", "key": "part_ready", "value": "x" * 300,
                     "observed_at_utc_ns": 7},
            "done": {"kind": "gripper", "component_id": "gripper", "state": "moving",
                     "observed_at_utc_ns": 8},
        }
        runtime._observe_run_state(snapshot)  # noqa: SLF001
        run = runtime.read_run(run_id)
        assert run["schema_version"] == "manyforge.program_run_status.v6"
        since = run["node_running_since_utc_ns"]
        assert set(since) == {"root", "wait"}
        assert before <= since["root"] == since["wait"] <= time.time_ns()
        # Only a running wait shows; text is cut to 256 bytes.
        assert run["node_wait_values"] == {"wait": {
            "kind": "key", "key": "part_ready", "value": "x" * 256, "observed_at_utc_ns": 7}}
        assert runtime.cycle_status()["run"]["node_running_since_utc_ns"] == since
        # A node keeps its stamp while it runs; one that starts later is new.
        later = _Snapshot(run_id, owner_id=runtime.owner_id, state="running",
                          revision=snapshot.run_revision + 1)
        later._value["node_statuses"] = {"root": "running", "wait": "success", "next": "running"}
        owner.rows[run_id] = later
        runtime._observe_run_state(later)  # noqa: SLF001
        runtime._observe_run_state(snapshot)  # noqa: SLF001 - an older one changes nothing
        again = runtime.read_run(run_id)["node_running_since_utc_ns"]
        assert set(again) == {"root", "next"} and again["root"] == since["root"]
        assert again["next"] >= since["root"]
        assert runtime.read_run(run_id)["node_wait_values"] == {}
        retained = journal.read_run_snapshot(run_id)
        assert retained["node_running_since_utc_ns"] == {}
        assert retained["node_wait_values"] == {}
    finally:
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
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
        application_start_id=_A,
        execution_run_root=execution_run_root,
        cycle_journal_path=journal,
    )
    assert config.execution_run_root == execution_run_root
    assert config.cycle_journal_path == journal

    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
            execution_run_root=execution_run_root,
        )
    with pytest.raises(ValueError, match="configured together"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
            cycle_journal_path=journal,
        )
    with pytest.raises(ValueError, match="outside disposable"):
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
            execution_run_root=execution_run_root,
            cycle_journal_path=execution_run_root / "cycle.sqlite3",
        )


def test_repeated_concurrent_run_observation_is_strictly_read_only(
    tmp_path,
) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    history = _Snapshot(
        1,
        owner_id="program-runner.test",
        state="succeeded",
        revision=3,
    )
    journal.retain_run_snapshot(history.to_dict())
    owner = _ObservationOwner()
    owner.rows[2] = _Snapshot(
        2,
        owner_id="program-runner.test",
        state="running",
        revision=2,
    )
    owner.rows[3] = _Snapshot(
        3,
        owner_id="program-runner.test",
        state="stopped",
        revision=4,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    retain_calls: list[dict[str, Any]] = []

    def forbidden_retention(snapshot: dict[str, Any]) -> None:
        retain_calls.append(snapshot)
        raise AssertionError("a Program-run GET attempted durable retention")

    journal.retain_run_snapshot = forbidden_retention  # type: ignore[method-assign]
    # Every journal change posts one numbered record; a GET posts none.
    sequence_before = journal._sequence  # noqa: SLF001
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
                    1,
                    2,
                    3,
                }
                assert (
                    runtime.runs(active_only=True, limit=10)["runs"][0]["capture"][
                        "program_run_id"
                    ]
                    == 2
                )
                assert runtime.read_run(2)["state"] == "running"
                assert runtime.read_run(1)["state"] == "succeeded"
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
    assert journal._sequence == sequence_before  # noqa: SLF001
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
            application_start_id=_A,
        ),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]

    runtime.start()
    request = _start_request()
    register = headless.register_program_run_capture

    def register_after_claim(members: Mapping[str, Any]) -> dict[str, Any]:
        body = members["capture"]
        assert host.start_admissions == []
        cycle = runtime._journal.read()
        assert cycle.state == "starting" and cycle.claim is not None
        assert body["claim_generation"] == cycle.claim["claim_generation"]
        assert body["schema_version"] == "manyforge.program_run_execution_capture.v2"
        assert "program_runner_owner_epoch" not in body
        assert body["program_run_id"] == cycle.program_run_id
        return register(members)

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
    assert started["capture"]["program_run_id"] == 1
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
            "request_key": _key("request.motion-start.1"),
            "source_label": "operator_ui",
            "program_run_id": 1,
            "expected_hold_revision": 0,
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
        "request_key": _key("request.motion-start.blocked"),
    })
    with pytest.raises(ProgramRunnerError) as blocked:
        runtime.start_motion(1, blocked_start)
    assert blocked.value.code == "rule_holds"
    assert host.motion_start_calls == []
    # OD-214: accepted, then refused by the readiness it asked for; no effect.
    assert _last_row(runtime, "start_motion", "refused", "rule_holds") == {
        "step": "admitting", "accepted": True, "details": {"rule_id": "guard"}}
    headless.readiness = ready

    # Run is fenced on run number and selection revision only.
    for request_id, run_id, changes, code in (
        ("stale-run", 9, {"program_run_id": 9},
         "program_runtime_control_stale"),
    ):
        with pytest.raises(ProgramRunnerError) as stale:
            runtime.start_motion(
                run_id,
                ProgramSelectionStartRequest.model_validate({
                    **motion_start.model_dump(mode="json"),
                    "request_key": _key(f"request.motion-start.{request_id}"),
                    **changes,
                }),
            )
        assert stale.value.code == code
        # Refused before acceptance (a memory check): no step, no acceptance.
        assert _last_row(runtime, "start_motion", "refused", code) == {
            "step": None, "accepted": False, "details": None}
    assert host.motion_start_calls == []
    # Tree ticks right after Execute raise the run revision; Run is admitted.
    ticked = owner.rows[1]
    owner.rows[1] = _Snapshot(
        1,
        owner_id=runtime.owner_id,
        state="running",
        revision=ticked.run_revision + 3,
    )
    motion_started = runtime.start_motion(1, motion_start)
    assert motion_started["state"] == "running"
    assert _last_row(runtime, "start_motion", "succeeded", None) == {
        "step": "applying", "accepted": True, "details": None}
    assert host.motion_start_calls == [
        (1, ["robot_group"], 0, _kb("request.motion-start.1"))
    ]

    # A motion Stop is Kernel-first (Headless to the Kernel); the runner
    # only meets its settled hold, an intervention-required run.
    settled_motion_stop = _Snapshot(
        1,
        owner_id=runtime.owner_id,
        state="intervention_required",
        revision=owner.rows[1].run_revision + 1,
    )
    owner.rows[1] = settled_motion_stop
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

    # A Halt reaches the runner as the Kernel's hold facts on the event
    # stream (level-triggered); the scheduling host keeps them for the tick.
    runtime._apply_program_run_hold(_hold_facts(1, 12, event_id=12))  # noqa: SLF001
    halted = owner.rows[1].to_dict()
    assert halted["state"] == "halted"
    assert host.halt_calls == [(1, 12)]
    halted_status = runtime.cycle_status()
    assert halted_status["state"] == "running"
    halted_commands = {
        row["command_kind"]: row for row in halted_status["allowed_commands"]
    }
    assert halted_commands["program_resume_halt"]["allowed"] is True
    assert halted_commands["program_discard_halt"]["allowed"] is True
    assert halted_commands["program_motion_stop"]["allowed"] is False
    assert halted_commands["program_manual_handover"]["allowed"] is False
    assert halted_commands["stop_all"]["allowed"] is True
    assert halted_commands["halt"]["allowed"] is True
    assert "program_resume_full_stop" not in halted_commands
    assert "cell_full_stop" not in halted_commands

    resume_halt = ProgramHaltResumeRequest.model_validate(
        {
            "request_key": _key("request.resume-halt.1"),
            "source_label": "operator_ui",
            "program_run_id": 1,
            "expected_run_revision": halted["run_revision"],
            "reason": "operator Execute after Halt",
        }
    )
    # Resume after Halt checks readiness like Resume.
    ready = headless.readiness
    headless.readiness = {
        "can_execute": False,
        "can_resume": False,
        "blocked_reasons": [{"code": "cell_latched", "message": "Cell latched"}],
    }
    blocked_resume = ProgramHaltResumeRequest.model_validate({
        **resume_halt.model_dump(mode="json"),
        "request_key": _key("request.resume-halt.blocked"),
    })
    with pytest.raises(ProgramRunnerError) as blocked_halt_resume:
        runtime.resume_halt(1, blocked_resume)
    assert blocked_halt_resume.value.code == "cell_latched"
    assert host.halt_resume_calls == []
    headless.readiness = ready
    resumed = runtime.resume_halt(1, resume_halt)
    assert resumed["state"] == "running"
    assert host.halt_resume_calls == [
        (1, "operator Execute after Halt")
    ]
    resumed_status = runtime.cycle_status()
    assert resumed_status["state"] == "running"
    resumed_commands = {
        row["command_kind"]: row for row in resumed_status["allowed_commands"]
    }
    assert resumed_commands["program_resume_halt"]["allowed"] is False
    assert resumed_commands["program_discard_halt"]["allowed"] is False
    assert resumed_commands["program_motion_stop"]["allowed"] is True

    owner_stop = host.stop_run

    def stop_with_terminal_observer(
        program_run_id: int,
        *,
        reason: str | None,
        stop_request_key: bytes | None = None,
    ) -> _Snapshot:
        snapshot = owner_stop(
            program_run_id,
            reason=reason,
            stop_request_key=stop_request_key,
        )
        runtime._record_terminal_snapshot(snapshot)  # noqa: SLF001
        return snapshot

    host.stop_run = stop_with_terminal_observer  # type: ignore[method-assign]
    # The stopped terminal comes from an internal Forced Program End; a
    # resend of that end reuses its runner key.
    runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
    runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
    assert runtime.cycle_status()["state"] == "terminal_awaiting_reset"
    assert host.stop_calls == [
        (1, BBKEY_PUBLICATION_FAULT_CODE),
        (1, BBKEY_PUBLICATION_FAULT_CODE),
    ]
    assert host.stop_keys[0] == host.stop_keys[1]
    assert key_class(host.stop_keys[0]) is KeyClass.INTERNAL
    assert key_origin(host.stop_keys[0]) == Minter.RUNNER
    assert runtime.cycle_status()["cycle_claim_generation"] == 1
    resident_terminal = owner.rows[1]
    owner.rows.clear()
    with pytest.raises(ProgramRunnerError) as terminal:
        runtime.start_run(_start_request(request_id="request.start.terminal"))
    assert terminal.value.code == "program_cycle_reset_required"
    # Historical status alone still blocks Run. Actual Reset containment uses
    # the terminal resident Run; its absence is covered by the recovery tests.
    owner.rows[1] = resident_terminal

    status = runtime.cycle_status()
    reset = ProgramCycleResetRequest.model_validate(
        {
            "request_key": _key("request.reset.1"),
            "source_label": "operator_ui",
            "program_run_id": 1,
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
    controls = runtime._optional_controls(1)  # noqa: SLF001
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
    assert host.reset_calls == [1]
    assert runtime.cycle_status()["state"] == "idle_after_reset"

    second = runtime.start_run(_start_request(request_id="request.start.after-reset"))
    assert second["capture"]["program_run_id"] == 2
    assert runtime.cycle_status()["state"] == "running"
    assert len(host.start_admissions) == 2
    runtime._apply_program_run_hold(_hold_facts(2, 13, event_id=13))  # noqa: SLF001
    assert owner.rows[2].state == "halted"
    # Reset of the halted run: its Forced Program End (part 1 of the
    # Reset's key) replaces the deleted discard route.
    terminal_cycle = runtime.cycle_status()
    assert terminal_cycle["state"] == "running"
    commands = {row["command_kind"]: row for row in terminal_cycle["allowed_commands"]}
    assert commands["program_reset"]["allowed"] is True
    second_reset = ProgramCycleResetRequest.model_validate(
        {
            "request_key": _key("request.reset.2"),
            "source_label": "operator_ui",
            "program_run_id": 2,
            "expected_cycle_generation": terminal_cycle["cycle_generation"],
            "expected_claim_generation": terminal_cycle["cycle_claim_generation"],
            "terminal_reference": None,
            "settlement_evidence_digest": None,
        }
    )
    runtime.reset_cycle(second_reset)
    assert host.stop_keys[-1] == with_part(_kb("request.reset.2"), Part.RESET_FORCED_END)
    assert runtime.cycle_status()["state"] == "idle_after_reset"

    third = runtime.start_run(
        _start_request(request_id="request.start.after-halt-reset")
    )
    runtime._stop_for_bbkey_publication_fault(  # noqa: SLF001
        third["capture"]["program_run_id"]
    )

    runtime.shutdown()
    assert host.shutdowns == 1
    assert [call[0] for call in host.stop_calls] == [1, 1, 2, 3]
    assert host.stop_calls[2][1] == PROGRAM_RESET_STOP_REASON
    # Each run's Forced Program End without a request has its own key.
    assert host.stop_keys[3] != host.stop_keys[0]


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
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    run_id = 1

    class ManualSnapshot(_Snapshot):
        @property
        def state(self):
            return ProgramRunState(super().state)

    owner.rows[run_id] = ManualSnapshot(
        run_id,
        owner_id=runtime.owner_id,
        state="running",
        revision=owner.read(run_id).run_revision,
    )
    context = {
        "program_run_id": run_id,
        "claim_generation": journal.read().claim["claim_generation"],
        "hold_revision": 0 if case == "unused" else 3,
    }
    request = ProgramManualHandoverRequest.model_validate(
        {
            "request_key": _key("manual.cleanup"),
            "source_label": "test",
            "program_run_id": run_id,
            "expected_run_revision": owner.read(run_id).run_revision,
            "operation": {
                "request_context": {
                    "request_key": _key("manual.1"),
                    "source_label": "test",
                },
                "program_revision": _DIGEST_A,
                "program_binding_id": _DIGEST_B,
                # Manual work carries no run number; its parent run is in
                # `manual_handover` (design C45).
                "program_run_id": None,
                "expected_graph_revision": _DIGEST_A,
                "expected_retained_bundle_id": _DIGEST_B,
                "expected_scene_revision": None,
                "coordination_policy": "independent",
                "manual_handover": context,
                "participants": [
                    {
                        "target": {"component_id": "gripper", "command_target_id": "gripper"},
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
                {**context, "hold_revision": 4} if case == "stale" else context
            ),
            "producer_node_indices": [] if case == "unused" else [0],
        }

    def cleanup(actual_run, **kwargs):
        calls.append("cleanup")
        assert actual_run == run_id and kwargs["node_indices"] == (0,)
        assert kwargs["expected_hold_revision"] == 3
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
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    intervention = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1,
            node_instance_id="move.1",
            node_attempt_id=1,
        ),
        owner_task_id=1,
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
        # Resume is fenced on the hold revision, not the run revision.
        assert "expected_run_revision" not in _kwargs
        assert _kwargs["expected_hold_revision"] == 4
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
            "request_key": _key("resume.1"),
            "source_label": "test",
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
            pending = journal._inflight_rows()  # noqa: SLF001
            assert len(pending) == 1
            claim = journal.read().claim
            state = journal.read().state
            # Containment is the Kernel's Halt (no runner Stop route): the
            # runner follows its hold facts.
            runtime._apply_program_run_hold(_hold_facts(1, 5, event_id=7))  # noqa: SLF001
            assert owner.rows[1].state == "halted"
            assert journal.read().state == state
            assert journal.read().claim == claim
            assert journal.read().reconciliation_required
            assert journal._inflight_rows() == pending  # noqa: SLF001
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=(transport := _Headless()),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
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
        controls = runtime._controls(1)
        maximum = 9_007_199_254_740_991
        # No public path reaches the revision limit in a test; seed the ledger.
        with controls._lock:  # noqa: SLF001
            controls._revision = maximum - 1  # noqa: SLF001
            controls._audit = [  # noqa: SLF001
                {"event_revision": index, "kind": "prior_control"}
                for index in range(maximum - 257, maximum - 1)
            ]

    intervention = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1,
            node_instance_id="move.1",
            node_attempt_id=1,
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
            "request_key": _key("request.operator-stop-reset.1"),
            "source_label": "operator_ui",
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
            pending = journal._inflight_rows()  # noqa: SLF001
            # The wrapper retires only itself. One canonical Reset survives
            # both known and unknown phase failures until successful recovery.
            assert bytes.fromhex(request.request_key) not in pending
            assert [row[0] for row in pending.values()] == ["reset"]
        finally:
            runtime.shutdown()
        return

    # The recovery action holds the runner; its nested Reset is its own
    # step, never refused as busy (OD-144).
    nested_reset = runtime.reset_cycle
    nested_held_by: list[Any] = []

    def observed_nested_reset(reset_request):
        nested_held_by.append(
            runtime._cycle_status_core()["runner_commands"]["in_progress"]["kind"]  # noqa: SLF001
        )
        return nested_reset(reset_request)

    runtime.reset_cycle = observed_nested_reset  # type: ignore[method-assign]
    result = runtime.apply_recovery_action(request)
    del runtime.reset_cycle
    assert nested_held_by == ["recovery_action"]
    commands = runtime._cycle_status_core()["runner_commands"]  # noqa: SLF001
    assert commands["in_progress"] is None
    # OD-214: the nested Reset is the recovery action's row; it has no own.
    ended = commands["ended"][0]
    assert (ended["kind"], ended["request_key"], ended["step"], ended["end"]["outcome"]) == (
        "recovery_action", request.request_key, "releasing", "succeeded"
    )
    assert [row["kind"] for row in commands["ended"]].count("reset") == 0

    with pytest.raises(ProgramRunnerError) as completed_recovery:
        runtime.apply_recovery_action(request)
    assert completed_recovery.value.code == "cycle_recovery_offer_stale"
    assert result["schema_version"] == "manyforge.cycle_recovery_result.v3"
    assert result["request_key"] == request.request_key
    assert result["action_id"] == "reset_cycle"
    assert result["cycle_resumed"] is False
    assert result["run"]["state"] == "stopped"
    assert result["recovery_offer"] is None
    assert result["runtime_controls"] is None
    assert host.stop_calls == [
        (1, "operator requested Program cycle Reset")
    ]
    assert host.reset_calls == [1]
    # Reset returns every global rule to its `active_at_start`.
    assert transport.rule_resets >= 1
    assert len(admission.releases) == 1
    assert journal.read().state == "idle_after_reset"
    if near_revision_limit:
        controls_result = result["owner_result"]["cycle_reset"]["runtime_controls"]
        assert controls_result["runtime_control_revision"] == maximum
        assert controls_result["audit"][-1]["kind"] == "program_runtime_controls_reset"
        # Audited with the nested Reset's key: the action's key, nested part.
        assert controls_result["audit"][-1]["request_key"] == with_part(
            bytes.fromhex(request.request_key), 3
        ).hex()
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
        inventory={},
    )
    owner = _RuntimeControlOwner()
    runtime._owner = owner
    runtime._host = _Host(
        owner, owner_id=runtime.owner_id
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id=1,
        evidence={"request_key": _key("request.start.1")},
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", application_start_id=_A
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )

    status = runtime._cycle_status_core()  # noqa: SLF001

    assert status["state"] == "starting"
    # The run number is announced only after its Kernel claim (contract
    # 1.15.5, K-6): a pending acquire has neither run nor claim yet.
    assert status["program_run_id"] is None
    assert status["cycle_claim_generation"] is None
    runtime.shutdown()


def test_key_values_read_projects_the_owner_store_under_the_run_identity(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _KeyValuesOwner()
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = _Host(owner, owner_id=runtime.owner_id)  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    program_run_id = runtime.start_run(_start_request())["capture"]["program_run_id"]

    values = runtime.read_runtime_blackboard(program_run_id)

    assert values == {
        "schema_version": "manyforge.program_runtime_blackboard_values.v1",
        "owner_id": runtime.owner_id,
        "program_run_id": program_run_id,
        "run_revision": 1,
        "node_status_revision": 1,
        "keys": [{"key": "count", "type": "int64", "value_state": "value", "value": 2}],
    }
    with pytest.raises(ProgramRunnerError) as unknown:
        runtime.read_runtime_blackboard(program_run_id + 1)
    assert unknown.value.code == "program_run_not_found"


def test_runtime_controls_are_state_conditioned_and_project_current_controls(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    journal_path = tmp_path / "cycle.sqlite3"
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(journal_path),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _RuntimeControlOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    started = runtime.start_run(_start_request())
    program_run_id = started["capture"]["program_run_id"]

    def request_base(request_id: str, expected_run_revision: int) -> dict[str, Any]:
        return {
            "request_key": _key(request_id),
            "source_label": "operator_ui",
            "program_run_id": program_run_id,
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
    assert (
        runtime.read_runtime_controls(program_run_id)["runtime_controls"]
        == expected_controls
    )
    runtime.shutdown()


def test_tree_refinement_never_widens_reconciliation_permissions(tmp_path) -> None:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    claim = _program_cycle_claim()
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id=1,
        evidence=claim["intent"],
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id=1,
        claim=claim,
    )
    running = _Snapshot(
        1,
        owner_id="program-runner",
        state="running",
        revision=2,
    )
    journal.transition(
        expected_generation=1,
        expected_states=("starting",),
        state="running",
        program_run_id=1,
        run_snapshot=running.to_dict(),
    )
    journal.record_request(
        request_key=_kb("operator.1", "resume.unknown"),
        kind="resume_motion",
        intent={"program_run_id": 1},
        cycle_generation=1,
    )
    journal.require_reconciliation(expected_generation=1)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    owner.rows[1] = running

    def unexpected_intervention_lookup(_run_id: str) -> None:
        raise AssertionError("reconciliation must suppress node recovery lookup")

    owner.intervention = unexpected_intervention_lookup  # type: ignore[method-assign]
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    assert (
        runtime._recovery_offer(  # noqa: SLF001
            1, cycle_generation=1
        )
        is None
    )
    owner.intervention = lambda _run_id: None  # type: ignore[method-assign]

    status = runtime.cycle_status()
    assert status["recovery_offer"] is None
    assert status["failure"]["command_kind"] == "resume_motion"
    assert "blocked until the Program is Reset" in status["failure"][
        "message"
    ]
    permissions = {
        command["command_kind"]: command["allowed"]
        for command in status["allowed_commands"]
    }

    # Owner recovery offers never exist in recovery_required. The uncertain
    # command is this run's own, so Reset settles it.
    assert {kind for kind, allowed in permissions.items() if allowed} == {
        "stop_all",
        "halt",
        "program_reset",
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
        maximum_inflight_commands=1,
    )
    intent = {
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "request_key": _key("start.1"),
    }
    claim = {
        "intent": intent,
        "claim_generation": 7,
        "acquired_admission_generation": 6,
        "accepted_at_ns": 1,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id=1,
        evidence=intent,
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id=1,
        claim=claim,
    )
    terminal_reference = _retain_no_effect_terminal(
        journal, program_run_id=1
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
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    request = ProgramCycleResetRequest.model_validate(
        {
            "request_key": _key("reset.1"),
            "source_label": "operator_ui",
            "program_run_id": 1,
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
        # Reset is offered: it settles the unknown release from the receipt
        # (test_unknown_release_reset_reads_receipt_before_any_resend).
        assert reset_availability["program_reset"]["allowed"] is True
        assert reset_availability["execution_recovery"]["allowed"] is False
        assert len(journal._inflight_rows()) == 1  # noqa: SLF001
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
    retry = request.model_copy(update={"request_key": _key("reset.2")})
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
        maximum_inflight_commands=1,
    )
    claim = _program_cycle_claim()
    terminal_reference = _retain_terminal_cycle(journal, claim)
    admission = OrderedAdmission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
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
        status = program_reset_scene_authorization["program_cycle_status"]
        assert program_reset_scene_authorization == {
            "program_run_id": 1,
            "cycle_claim_generation": 1,
            "terminal_or_settlement_reference": terminal_reference,
            # The runner's current cycle status (OD-139).
            "program_cycle_status": status,
        }
        assert (status["state"], status["program_run_id"], status["cycle_claim_generation"],
                status["terminal_or_settlement_reference"]) == ("resetting", 1, 1, terminal_reference)
        assert status["accepted_reset"]["reload_initial_scene"] is True
        contexts.append(context_id)
        events.append(f"scene:{len(contexts)}")
        if len(contexts) == 1 and not phase_write_failure:
            raise RuntimeError("logical scene owner temporarily unavailable")
        return ({"source": {"record_digest": _DIGEST_A}}, {"proof": True}, None)

    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", prepare_scene)
    request = ProgramCycleResetRequest.model_validate(
        {
            "request_key": _key("reset.scene.1"), "reload_initial_scene": True,
            "source_label": "operator_ui",
            "program_run_id": 1,
            "expected_cycle_generation": 1,
            "expected_claim_generation": 1,
            "terminal_reference": terminal_reference,
            "settlement_evidence_digest": terminal_reference,
        }
    )
    runtime.start()

    if phase_write_failure:
        capture = {
            "program_run_id": 1,
            "program_revision": _DIGEST_A,
            "program_binding_id": _DIGEST_B,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "node_environment_digest": _DIGEST_A,
        }
        controls = ProgramRuntimeControls(capture)
        maximum = 9_007_199_254_740_991
        # No public path reaches the revision limit in a test; seed the ledger.
        with controls._lock:  # noqa: SLF001
            controls._revision = maximum - 1  # noqa: SLF001
            controls._audit = [  # noqa: SLF001
                {"event_revision": index, "kind": "prior_control"}
                for index in range(maximum - 257, maximum - 1)
            ]
        runtime._runtime_controls[1] = controls
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

    retry = request.model_copy(update={"request_key": _key("reset.scene.2")})
    assert runtime.reset_cycle(retry)["idle_cycle_generation"] == 2
    assert events == ["scene:1", "scene:2", "release", "idle"]
    assert len(admission.releases) == 1
    assert journal.read().state == "idle_after_reset"
    if phase_write_failure:
        assert controls.revision == maximum
        assert controls.snapshot()["audit"][-1]["request_key"] == _key("reset.scene.1")
    runtime.shutdown()


def test_rejected_and_completed_resets_do_not_consume_inflight_capacity(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    _bypass_reset_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3",
        maximum_inflight_commands=2,
    )
    intent = {
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "request_key": _key("start.1"),
    }
    claim = {
        "intent": intent,
        "claim_generation": 7,
        "acquired_admission_generation": 6,
        "accepted_at_ns": 1,
    }
    journal.begin_acquire(
        expected_generation=1,
        provisional_run_id=1,
        evidence=intent,
    )
    journal.remember_acquired_claim(
        expected_generation=1,
        provisional_run_id=1,
        claim=claim,
    )
    terminal_reference = _retain_no_effect_terminal(
        journal, program_run_id=1
    )
    admission = _Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
            maximum_inflight_commands=2,
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()

    stale = ProgramCycleResetRequest.model_validate(
        {
            "request_key": _key("reset.stale"),
            "source_label": "operator_ui",
            "program_run_id": 1,
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
            "request_key": _key("reset.valid"),
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
        {**stale.model_dump(mode="json"), "request_key": _key("reset.capacity")}
    )
    with pytest.raises(ProgramRunnerError) as rejected_again:
        runtime.reset_cycle(another_stale)
    assert rejected_again.value.code == "program_cycle_reset_stale"
    assert journal._inflight_rows() == {}  # noqa: SLF001
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
        inventory={},
    )
    journal._maximum_inflight_commands = 1
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    claim = journal.read().claim
    admission.inventory = {
        "admission_generation": 1,
        "held_claims": [claim],
        "matched_claim": claim,
        "matched_release": None,
        "status": "held_exact",
    }

    def unknown_resume(*_args, **_kwargs):
        raise ProgramRunOwnerOutcomeUnknownError("Resume response lost")

    host.resume_halt = unknown_resume
    request = ProgramHaltResumeRequest.model_validate(
        {
            "request_key": _key("resume.unknown"),
            "source_label": "operator_ui",
            "program_run_id": 1,
            "expected_run_revision": 1,
        }
    )
    with pytest.raises(ProgramRunnerError) as unknown:
        runtime.resume_halt(1, request)
    assert unknown.value.code == "program_cycle_reconciliation_required"
    retained = journal.read()
    assert retained.reconciliation_required
    assert retained.failure == {
        "cause_code": "ProgramRunOwnerOutcomeUnknownError",
        "code": "program_runtime_control_outcome_unknown",
        "command_kind": "resume_halt",
        "message": (
            "The Resume Halt outcome could not be confirmed. Further execution "
            "is blocked until the Program is Reset."
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
            "request_key": _key(request_id),
            "source_label": "operator_ui",
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

    def contain_or_lose(run_id, *, stop_request_key):
        stop_ids.append(stop_request_key)
        assert journal.read().reconciliation_required
        assert original[0] in journal.pending_commands()
        assert not admission.releases
        if len(stop_ids) <= lost_confirmations:
            raise ProgramRunOwnerOutcomeUnknownError("Stop response lost")
        return contain(run_id, stop_request_key=stop_request_key)

    def restore(request, _authorization):
        assert not journal.read().reconciliation_required
        assert journal.read().reset_phase == "restoring_scene"
        assert len(journal.pending_commands()) == 2
        assert original[0] in journal.pending_commands()
        restored_ids.append(request.request_key)

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
            # OD-214: an accepted Reset whose phase outcome is unknown.
            row = runtime.cycle_status()["runner_commands"]["ended"][0]
            assert (row["kind"], row["request_key"], row["end"]["outcome"]) == (
                "reset", request.request_key, "unknown"
            )
            assert row["end"]["code"] == "program_cycle_recovery_required"
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
    assert restored_ids == [_key("reset.0")]
    assert len(admission.releases) == 1
    assert journal.pending_commands() == ()
    assert journal.read().state == "idle_after_reset"
    assert not owner.rows


@pytest.mark.parametrize(
    "crossing",
    [
        "claim",
        "program_node_evidence",
        "intervention_evidence",
        "active_run",
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
        if crossing == "claim":
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
    # Every uncertain command kind of the current cycle, the in-flight start
    # included; Reset's exact Kernel proof settles each the same way.
    for kind in (
        "resume_motion", "resume_halt", "prepare_manual_handover", "start",
        "discard_halt", "runtime_blackboard_write", "persistent_values_clear",
        "manual_result_stage",
    ):
        assert runtime._current_cycle_recovery_reset(cycle, [{**row, "kind": kind}])
    assert not runtime._current_cycle_recovery_reset(cycle, [{**row, "cycle_generation": 2}])
    for field, value in (("program_run_id", 9),):
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
        "request_key": _kb("reset.0"),
        "intent": _current_reset_request(runtime, journal, "reset.0").model_dump(
            mode="json"
        ),
    }
    assert runtime._current_cycle_recovery_reset(cycle, [reset])
    assert not runtime._current_cycle_recovery_reset(
        cycle, [reset, {**reset, "request_key": _kb("reset.1")}]
    )
    action = {
        **row,
        "kind": "recovery_action",
        "intent": {
            "action_id": "replan_from_stopped_state",
        },
    }
    assert runtime._current_cycle_recovery_reset(cycle, [action])
    for action_id in (
        "retry_motion_from_current_state", "acknowledge_and_continue",
        "retry_after_scene_repair", "retry_runtime_step", "continue_with_failure",
    ):
        assert runtime._current_cycle_recovery_reset(
            cycle, [{**action, "intent": {"action_id": action_id}}]
        )
    action["intent"]["action_id"] = "reset_cycle"
    assert runtime._current_cycle_recovery_reset(cycle, [action])
    # A retained Reset beside the action must be the action's own nested Reset.
    assert not runtime._current_cycle_recovery_reset(cycle, [action, reset])
    # The nested Reset of the reset_cycle action carries the action's key
    # with the nested-Reset part.
    reset["request_key"] = with_part(row["request_key"], 3)
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
        maximum_inflight_commands=1,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", application_start_id=_A
        ),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
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
            request.model_copy(update={"request_key": _key("request.start.capacity")})
        )
    assert still_stale.value.code == "program_run_draft_applied_mismatch"
    assert events == ["generation", "applied"] * 3
    assert admission.claims == []
    assert journal._inflight_rows() == {}  # noqa: SLF001
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
    journal = FailingJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test", application_start_id=_A
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
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
            headless_url="http://headless.test", application_start_id=_A
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
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


def _running_cycle_runtime(tmp_path) -> tuple[ProgramRuntime, ProgramCycleJournal, _Host]:
    tmp_path.mkdir(exist_ok=True)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    journal.transition(
        expected_generation=1,
        expected_states=("idle_after_reset",),
        state="running",
        program_run_id=1,
        claim={"claim_generation": 1},
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime._host.shutdown()
    owner = _Owner()
    owner.rows[1] = _Snapshot(
        1,
        owner_id=runtime.owner_id,
        state="running",
        revision=1,
    )
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]
    runtime.start()
    return runtime, journal, host


def test_runtime_rejects_a_stale_applied_selection_before_tree_admission(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    headless = _Headless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=_A,
        ),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
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
            application_start_id=_A,
        ),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
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
        "schema_version": "manyforge.node_execution_availability.v2",
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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
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
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
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
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    # The run-start captured policy, not the caller, permits the Retry.
    owner.rows[1].capture.node_policy_by_instance_id = {
        "move.1": SimpleNamespace(
            recovery_policy={
                "allow_intervention": case != "forbidden",
                "max_retries": 1 if case == "exhausted" else 0,
            }
        )
    }
    if case == "exhausted":
        runtime._controls(1).note_retry("move.1")
    intervention = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1,
            node_instance_id="move.1",
            node_attempt_id=1,
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

    host.retry_failure_pause = retry
    owner.apply_owner_recovery = forbidden
    host.resume_motion = forbidden
    host.resume_recovery = resume_recovery
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_key": _key("retry.1"),
            "source_label": "test",
            "offer_id": offer["offer_id"],
            "offer_revision": offer["offer_revision"],
            "action_id": action_id,
        }
    )
    owner_action = None if action_id == "retry_runtime_step" else action_id
    try:
        if case == "alarm":
            # The Kernel's operator-alarm attention refuses the motion this
            # Retry would release, before any owner effect.
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
            assert owner.attention_checks == [(1, "move.1", _kb("retry.1"))]
            assert calls == []
            assert journal.read().state == "running"
            assert not journal.read().reconciliation_required
            # Acknowledged: the same offer's Retry proceeds.
            owner.attention_refusal = None
            result = runtime.apply_recovery_action(
                request.model_copy(update={"request_key": _key("retry.after.ack")})
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
                                "request_key": _key(f"retry.label.{index}"),
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
            assert runtime._controls(1).retry_count("move.1") == 1
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=headless, gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    runtime._host.shutdown()
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
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
            assert error.details["program_run_id"] == cycle.program_run_id == 1
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
            runtime.start_run(_start_request().model_copy(update={"request_key": _key("run.after.failed-start.reset")}))
            assert journal.read().state == "running"
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("not_ready", [True, False])
def test_scene_preparation_refuses_not_ready_but_not_a_moved_generation(monkeypatch, tmp_path, not_ready):
    class ObservedAdmission(_Admission):
        def get(self):
            return {"admission_generation": 35 if not_ready else 37,
                    "authoring_allowed": not not_ready}
    admission = ObservedAdmission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=_Headless(), gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    prepared: list[str] = []
    def scene_effect(**kwargs):
        if not_ready:
            raise AssertionError("Refused admission must not prepare the Scene")
        prepared.append(kwargs["context_id"])
        return ({"program_revision": "program.rev"},
                {"schema_version": "manyforge.prepared_program_scene.v1"}, None)
    monkeypatch.setattr(runtime, "_prepare_current_applied_scene", scene_effect)
    runtime.start()
    try:
        if not_ready:
            with pytest.raises(ProgramRunnerError) as caught:
                runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
                    "request_key": _key("idle.reset.not-ready"), "reload_initial_scene": True,
                    "source_label": "operator",
                    "program_run_id": None,
                    "expected_cycle_generation": 1, "expected_claim_generation": None,
                    "terminal_reference": None, "settlement_evidence_digest": None}))
            assert caught.value.code == "authoring_not_ready"
            assert caught.value.details == {"admission_generation": 35,
                                           "authoring_allowed": False,
                                           "expected_admission_generation": 35}
        else:
            # The caller read generation 35; only the generation moved since.
            result = runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
                "request_key": _key("scene.older-generation"),
                "source_label": "operator",
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=headless, gateway=headless,
                             admission_client=admission, cycle_journal=journal)
    runtime._host.shutdown()
    owner = _ObservationOwner()
    host = UnknownHost(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    request = _start_request()
    try:
        with pytest.raises(ProgramRunnerError) as caught:
            runtime.start_run(request)
        assert caught.value.code == "program_cycle_reconciliation_required"
        assert caught.value.details["program_run_id"] == 1
        cycle = journal.read()
        assert cycle.state == "recovery_required" and cycle.reconciliation_required
        assert cycle.claim["claim_generation"] == caught.value.details["claim_generation"]
        assert cycle.terminal_reference is None and cycle.start_settlement is None
        assert any(row["request_key"].hex() == request.request_key for row in journal.pending_commands())
        with pytest.raises(ProgramRunnerError) as repeated:
            runtime.start_run(request)
        assert repeated.value.code == "program_cycle_reconciliation_required"
        assert len(host.start_admissions) == 1 and host.scheduled == []
        assert admission.releases == []
    finally:
        runtime.shutdown()


# Dependency follows used targets/nodes, not declared roles or site names.
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
    component = deepcopy(next(row for row in binding["component_bindings"]
                              if row["component_id"] == "lite6_robot"))
    component["component_id"] = "robot.other"
    binding["component_bindings"].append(component)
    target_row = deepcopy(next(row for row in binding["target_bindings"]
                               if row["target"] == {"component_id": "lite6_robot", "command_target_id": "arm"}))
    target_row["target"] = {"component_id": "robot.other", "command_target_id": "arm"}
    binding["target_bindings"].append(target_row)
    binding["component_bindings"].append({**deepcopy(component), "component_id": "station_io", "model_bindings": []})
    binding["target_bindings"].append({**deepcopy(target_row),
        "target": {"component_id": "station_io", "command_target_id": "io"}})
    binding["signal_bindings"].append({"reference": {"component_id": "station_io", "signal_name": "clamp"},
        "target": {"component_id": "station_io", "command_target_id": "io"}})
    graph = {
        "schema_version": "manyforge.manager_graph_projection.v2",
        "source": {"available": True, "freshness": "fresh", "known_gaps": [],
                   "source_owner_id": "manager.test"},
        "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B,
        "cell_graph": {"graph_revision": _DIGEST_B,
            "controllable_components": [
                {"controllable_component_id": "lite6_robot", "runtime_unit_ids": ["cell.ros"]},
                {"controllable_component_id": "lite_gripper", "runtime_unit_ids": ["cell.ros"]},
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
                    for name, lane in (("lite6_robot", lanes[0]), ("lite_gripper", lanes[0]), ("robot.other", lanes[1]))]}}]},
        # Deployment-derived and sorted by component; Mock and Real need none.
        "component_simulator_dependencies": [
            {"controllable_component_id": name, "simulator": "isaac" if lane == "isaac" else "none"}
            for name, lane in (("lite6_robot", lanes[0]), ("lite_gripper", lanes[0]), ("robot.other", lanes[1]))],
    }
    headless = _SimulationLaneHeadless(graph)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless, gateway=headless,
        simulation_control=simulation, admission_client=admission, cycle_journal=journal)
    return runtime, headless, binding


def _simulation_lane_program(*roles: str, node_id="motion.move_to_joint_target", **parameters):
    identities = {"arm": ("lite6_robot", "arm"), "other_arm": ("robot.other", "arm"),
                  "gripper": ("lite_gripper", "gripper")}
    children = [{"node_id": "gripper.command_position" if role == "gripper" else node_id,
                 "instance_id": f"step.{index}",
                 "parameters": {**parameters, "target": {"component_id": identities[role][0],
                                                            "command_target_id": identities[role][1]}},
                 "children": []} for index, role in enumerate(roles)]
    return {"tree": {"node_id": "sequence", "instance_id": "root", "parameters": {}, "children": children}}



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
    runtime.cycle_projection = lambda: _cycle_status("running")  # type: ignore[method-assign]
    runtime._host = SimpleNamespace(  # noqa: SLF001
        resume_motion=lambda *_args, **_kwargs: pytest.fail(
            "a readiness refusal must precede the host Resume effect"
        )
    )
    request = ProgramSelectionResumeRequest.model_validate({
        "request_key": _key("request.motion-resume.blocked"),
        "source_label": "operator_ui",
        "program_run_id": 1,
        "expected_hold_revision": 1,
        "group_ids": ["arm"],
        "reason": "operator Resume",
    })
    with pytest.raises(ProgramRunnerError) as blocked:
        runtime.resume_motion(1, request)
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
                                  "missing_bound_component", "duplicate_component", "duplicate_target",
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
    elif case == "missing_bound_component":
        binding["component_bindings"] = [row for row in binding["component_bindings"] if row["component_id"] != "lite6_robot"]
    elif case == "duplicate_component":
        binding["component_bindings"].append(deepcopy(binding["component_bindings"][0]))
    elif case == "duplicate_target":
        binding["target_bindings"].append(deepcopy(binding["target_bindings"][0]))
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
    ({"expected_state": "moving", "target": {"component_id": "lite6_robot", "command_target_id": "arm"}}, False, 1),
    ({"expected_state": "moving", "target": {"component_id": "robot.other", "command_target_id": "arm"}}, True, 1),
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
                                     default={"component_id": "robot.other", "command_target_id": "arm"} if row.id == "target" else "moving")
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
    program.update(program_revision=binding["program_revision"])
    return ProgramRunPortContext(program_run_id=51, applied_record_digest=_DIGEST_B,
        scenario_id=scenario, source_document_digest=_DIGEST_A,
        program_revision=binding["program_revision"],
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
        _set_simulation_lane(headless.graph, "lite6_robot", "isaac")
        applied["graph_revision"] = binding["graph_revision"] = "sha256:" + "c" * 64
        headless.graph["graph_revision"] = headless.graph["cell_graph"]["graph_revision"] = applied["graph_revision"]
        assert runtime._simulation_context_requirement(program=applied["program"],
            program_binding=binding, retained_bundle_id=_DIGEST_A, graph_revision=applied["graph_revision"]) == (
                True, AvailabilityFact(True))
        _set_simulation_lane(headless.graph, "lite6_robot", "real")
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
           "parameters": {"signal": {"component_id": "station_io", "signal_name": "clamp"}, **extra}, "children": []}
          for index, (node_id, extra) in enumerate((("io.set_output", {"value": True}),
                                                    ("io.get_input", {}), ("io.check_input", {}),
                                                    ("io.wait_for_input", {})))]
    program = _simulation_lane_program(*motion_roles)
    program["tree"]["children"] = io + program["tree"]["children"]
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
        component = {"arm": "lite6_robot", "other_arm": "robot.other"}[motion_role]
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
    # mock_lite6_pneumatic_pick_and_place: the mounted no-output pneumatic tool
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
    journal = ProgramCycleJournal(tmp_path / "io-only.sqlite3")
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
    journal = ProgramCycleJournal(tmp_path / "mock-simulator.sqlite3")
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
                              target_parameter="target" if scope == "target_binding" else None)
        entry = replace(original, required_capabilities=(requirement,),
                        execution=replace(original.execution, owner_api=owner))
        monkeypatch.setattr(catalog, "get", lambda node_id:
                            entry if node_id == "simulation.reset" else original_get(node_id))
        program = {"tree": {"node_id": "simulation.reset", "instance_id": "reset",
                            "parameters": {"target": {"component_id": "lite6_robot", "command_target_id": "arm"}}, "children": []}}
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
    journal = ProgramCycleJournal(tmp_path / "availability.sqlite3")
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
            program=SimpleNamespace(program_revision=_DIGEST_A,
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
    # A unit hosting only real robots has no `mode`; the Manager
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
        headless.graph["component_simulator_dependencies"] = {"lite6_robot": "isaac"}
    elif case == "not_row":
        dependencies[0] = "lite6_robot"
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

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
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
        "request_key": _key("reset.snapshot"),
        "source_label": "operator_ui",
        "program_run_id": None,
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
        if get_ident() == reset_thread.ident:
            # Reset reads the cycle status for its Kernel proof.
            return read_snapshot()
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
            assert projected["accepted_reset"]["request_key"] == request.request_key
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


def test_liveness_projection_answers_while_the_writer_and_a_runner_command_are_held(
    monkeypatch, tmp_path
):
    from manyforge_program_runner.liveness import ProgramRunnerLivenessReceiver

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=Admission(), cycle_journal=journal,
    )
    receiver = object.__new__(ProgramRunnerLivenessReceiver)
    receiver._cycle_status = runtime._current_cycle_status_projection  # noqa: SLF001
    writer = journal._writer  # noqa: SLF001
    apply = writer._apply  # noqa: SLF001
    writing, release_writer = Event(), Event()
    lock_held, release_lock = Event(), Event()

    def blocked_apply(record):
        writing.set()
        assert release_writer.wait(5), "test did not release the writer"
        apply(record)

    def hold_command():
        # Every runner command is keyed (OD-214 rows).
        with runtime._runner_command("reset", _key("liveness.hold")):  # noqa: SLF001
            lock_held.set()
            release_lock.wait(5)

    def answer() -> dict[str, Any]:
        answers: list[dict[str, Any]] = []
        thread = Thread(target=lambda: answers.append(receiver._project_cycle_status()))  # noqa: SLF001
        thread.start()
        thread.join(2)
        assert not thread.is_alive(), "the liveness projection waited"
        return answers[0]

    holder = Thread(target=hold_command)
    runtime.start()
    monkeypatch.setattr(writer, "_apply", blocked_apply)
    try:
        failure = {"code": "program_scene_preparation_failed", "message": "Scene needs reload"}
        journal.record_scene_preparation_failure(expected_generation=1, failure=failure)
        assert writing.wait(2)
        assert answer()["state"] == "idle_after_reset"
        holder.start()
        assert lock_held.wait(2)
        projected = answer()
        assert (projected["state"], projected["cycle_generation"]) == ("idle_after_reset", 1)
        status = runtime._current_cycle_status_projection()  # noqa: SLF001
        assert _in_progress_kind(status) == "reset"
    finally:
        release_lock.set()
        release_writer.set()
        if holder.ident is not None:
            holder.join(2)
        runtime.shutdown()
    # Released, the writer wrote every posted record before the file closed.
    assert writer.failed_commits == 0
    assert writer.dropped_records == 0


def _last_row(
    runtime: ProgramRuntime, kind: str, outcome: str, code: str | None
) -> dict[str, Any]:
    """The newest ended command row (OD-214), checked for its kind and end."""

    row = runtime.cycle_status()["runner_commands"]["ended"][0]
    assert (row["kind"], row["end"]["outcome"], row["end"]["code"]) == (
        kind, outcome, code
    ), row
    return {"step": row["step"], "accepted": row["accepted_at_ns"] is not None,
            "details": row["end"]["details"]}


def _in_progress_kind(status: dict[str, Any]) -> str | None:
    """The kind of the command holding the runner (OD-144/OD-214), or None."""

    row = status["runner_commands"]["in_progress"]
    return None if row is None else row["kind"]


def _busy(error: ProgramRunnerError, kind: str) -> bool:
    return (
        error.code == "execution_control_ingress_busy"
        and error.details == {"command_in_progress": kind}
    )


def test_reset_position_is_read_only_where_it_is_consumed(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """The projection and the readiness facts read no Reset-position
    availability from the host; ``cycle_status()`` reads it once and equals
    the projection completed with it."""

    runtime, _owner = _running_control_runtime(monkeypatch, tmp_path)
    reads: list[int] = []
    runtime._gateway.reset_robot_position_capability = lambda _members=None: (  # noqa: SLF001
        reads.append(1) or {"ok": True, "reset_robot_position": {"available": True, "reason": None}})
    status = runtime.cycle_status()
    assert len(reads) == 1
    assert status["reset_robot_position"] == {"available": True, "reason": None}
    projection = runtime.cycle_projection()
    assert len(reads) == 1
    assert {**projection, "reset_robot_position": status["reset_robot_position"]} == status
    assert runtime.add_reset_robot_position(projection) == status
    assert len(reads) == 2
    runtime._require_run_readiness("can_execute")  # noqa: SLF001
    assert len(reads) == 2
    assert runtime._headless.readiness_cycles[-1] == run_cycle_facts(status)  # noqa: SLF001
    runtime.shutdown()


def _running_control_runtime(monkeypatch: pytest.MonkeyPatch, tmp_path):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _RuntimeControlOwner()
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = _Host(owner, owner_id=runtime.owner_id)  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    runtime.start_run(_start_request())
    return runtime, owner


def test_a_typed_owner_refusal_of_resume_keeps_its_code_and_details(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """A selected Resume's typed owner refusal reaches the caller with its
    catalog code and details, not the generic control rejection."""

    runtime, _owner = _running_control_runtime(monkeypatch, tmp_path)

    def refused(*_args, **_kwargs):
        raise ProgramRunOwnerError(
            "selected_owner_assessment_unavailable",
            code="selected_owner_assessment_unavailable",
            details={"node_instance_id": "move.1"},
        )

    runtime._host.resume_motion = refused  # type: ignore[method-assign]  # noqa: SLF001
    try:
        with pytest.raises(ProgramRunnerError) as rejected:
            runtime.resume_motion(1, ProgramSelectionResumeRequest.model_validate({
                "request_key": _key("typed.motion-resume"), "source_label": "operator_ui",
                "program_run_id": 1, "expected_hold_revision": 0, "group_ids": ["arm"],
            }))
        assert rejected.value.code == "selected_owner_assessment_unavailable"
        assert rejected.value.details == {"node_instance_id": "move.1"}
    finally:
        runtime.shutdown()


def test_a_runner_command_meeting_another_is_refused_at_once_and_never_runs_later(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """OD-144 (a): no command waits for another; the one in progress is published."""

    runtime, owner = _running_control_runtime(monkeypatch, tmp_path)
    runtime._persistent_keys = PersistentKeyRegister(  # noqa: SLF001
        tmp_path / "values", application_start_id=_A_TEXT
    )
    cycle = runtime.cycle_status()
    assert _in_progress_kind(cycle) is None
    entered, release = Event(), Event()
    write = owner.write_runtime_blackboard

    def held_write(program_run_id, **values):
        # Scripted hold point of the first command (a value write).
        entered.set()
        assert release.wait(5), "test did not release the held write"
        return write(program_run_id, **values)

    owner.write_runtime_blackboard = held_write  # type: ignore[method-assign]
    first = RuntimeBlackboardWriteRequest.model_validate({
        "request_key": _key("busy.write.1"), "source_label": "operator_ui",
        "program_run_id": 1, "expected_run_revision": 1,
        "key": "operator.confirmed", "value": True,
    })
    outcomes: list[Any] = []

    def run_first() -> None:
        try:
            outcomes.append(runtime.write_runtime_blackboard(1, first))
        except Exception as error:  # noqa: BLE001
            outcomes.append(error)

    thread = Thread(target=run_first)
    thread.start()
    kind = "runtime_blackboard_write"
    try:
        assert entered.wait(2)
        assert _in_progress_kind(runtime.cycle_status()) == kind
        base = {"source_label": "operator_ui", "program_run_id": 1}
        refused = {
            "reset": lambda: runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
                **base, "request_key": _key("busy.reset"),
                "expected_cycle_generation": cycle["cycle_generation"],
                "expected_claim_generation": cycle["cycle_claim_generation"],
            })),
            "execute": lambda: runtime.start_run(_start_request(request_id="busy.start")),
            "second group run": lambda: runtime.start_motion(1, ProgramSelectionStartRequest.model_validate({
                **base, "request_key": _key("busy.motion-start"),
                "expected_hold_revision": 0, "group_ids": ["robot_group"],
            })),
            "second group resume": lambda: runtime.resume_motion(1, ProgramSelectionResumeRequest.model_validate({
                **base, "request_key": _key("busy.motion-resume"),
                "expected_hold_revision": 0, "group_ids": ["other_group"],
            })),
            "execute after halt": lambda: runtime.resume_halt(1, ProgramHaltResumeRequest.model_validate({
                **base, "request_key": _key("busy.resume-halt"), "expected_run_revision": 1,
            })),
            "value write": lambda: runtime.write_runtime_blackboard(1, RuntimeBlackboardWriteRequest.model_validate({
                **base, "request_key": _key("busy.write.2"), "expected_run_revision": 1,
                "key": "operator.confirmed", "value": False,
            })),
            "manual result": lambda: runtime.stage_manual_result(1, ManualResultStageRequest.model_validate({
                **base, "request_key": _key("busy.manual"), "expected_run_revision": 1,
                "node_instance_id": "move.1", "node_attempt_id": 1, "outcome": "succeeded",
            })),
            "recovery action": lambda: runtime.apply_recovery_action(CycleRecoveryActionRequest.model_validate({
                "source_label": "operator_ui", "request_key": _key("busy.recovery"),
                "offer_id": _DIGEST_A, "offer_revision": 1, "action_id": "retry_runtime_step",
            })),
            "stored-value clear": lambda: runtime.clear_persistent_values(PersistentKeyValuesClearRequest.model_validate({
                "source_label": "operator_ui", "request_key": _key("busy.clear"),
                "project_path": "/projects/cell-pick.scenario",
            })),
            "scene preparation": lambda: runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
                "source_label": "headless_apply_completion", "request_key": _key("busy.scene"),
                "expected_applied_record_digest": _DIGEST_A, "expected_admission_generation": 1,
            })),
        }
        for label, send in refused.items():
            with pytest.raises(ProgramRunnerError) as busy:
                send()
            assert _busy(busy.value, kind), label
        # The same key sent again while its first send runs.
        with pytest.raises(ProgramRunnerError) as resent:
            runtime.write_runtime_blackboard(1, first)
        assert resent.value.code == "execution_control_command_in_progress"
        assert thread.is_alive()
    finally:
        release.set()
        thread.join(5)
    assert not thread.is_alive()
    # The first command is unaffected; no refused command ran later.
    assert len(outcomes) == 1 and isinstance(outcomes[0], dict), outcomes
    assert outcomes[0]["result"] == {"key": "operator.confirmed", "status": "written"}
    assert [effect for effect, _values in owner.effects] == ["write"]
    status = runtime.cycle_status()
    assert _in_progress_kind(status) is None
    assert status["state"] == "running"
    runtime.shutdown()


def test_published_command_in_progress_clears_when_the_command_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, owner = _running_control_runtime(monkeypatch, tmp_path)
    seen: list[Any] = []
    wakes: list[Any] = []
    runtime.add_control_state_observer(
        lambda: wakes.append(runtime._command_in_progress())  # noqa: SLF001
    )

    def failing_write(program_run_id, **_values):
        seen.append(_in_progress_kind(runtime.cycle_status()))
        raise ProgramRunnerError("program_runtime_control_stale", "scripted refusal")

    owner.write_runtime_blackboard = failing_write  # type: ignore[method-assign]
    with pytest.raises(ProgramRunnerError):
        runtime.write_runtime_blackboard(1, RuntimeBlackboardWriteRequest.model_validate({
            "request_key": _key("raise.write"), "source_label": "operator_ui",
            "program_run_id": 1, "expected_run_revision": 1,
            "key": "operator.confirmed", "value": True,
        }))
    assert seen == ["runtime_blackboard_write"]
    assert _in_progress_kind(runtime.cycle_status()) is None
    # Every panel is woken when the command starts and when it ends.
    assert wakes[0] == "runtime_blackboard_write" and wakes[-1] is None
    runtime.shutdown()


def test_execute_just_before_a_reset_marks_resetting_is_refused(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """An idle Reset holds the runner from its first step: an Execute that
    arrives before the cycle shows "resetting" is refused, not run after it."""

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    _bypass_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=Admission(), cycle_journal=journal,
    )
    runtime.start()
    entered, release = Event(), Event()
    effect_entered, release_effect = Event(), Event()
    require_owner = runtime._require_cycle_owner  # noqa: SLF001
    reset_thread_ident: list[int] = []

    def held_owner():
        # Scripted hold point: the Reset's first step inside the runner.
        if reset_thread_ident and get_ident() == reset_thread_ident[0] and not entered.is_set():
            entered.set()
            assert release.wait(5), "test did not release the Reset"
        return require_owner()

    def scene_effect(_request, _authorization):
        effect_entered.set()
        assert release_effect.wait(5), "test did not release the Reset effect"

    monkeypatch.setattr(runtime, "_require_cycle_owner", held_owner)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", scene_effect)
    reset = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.before-mark"), "source_label": "operator_ui",
        "program_run_id": None, "expected_cycle_generation": 1,
        "reload_initial_scene": True,
    })
    errors: list[BaseException] = []

    def run_reset() -> None:
        reset_thread_ident.append(get_ident())
        try:
            runtime.reset_cycle(reset)
        except BaseException as error:  # noqa: BLE001
            errors.append(error)

    thread = Thread(target=run_reset)
    thread.start()
    try:
        assert entered.wait(2)
        status = runtime.cycle_status()
        assert status["state"] == "idle_after_reset"
        assert _in_progress_kind(status) == "reset"
        with pytest.raises(ProgramRunnerError) as execute:
            runtime.start_run(_start_request(request_id="start.before-mark"))
        assert _busy(execute.value, "reset")
        release.set()
        assert effect_entered.wait(2)
        assert runtime.cycle_status()["state"] == "resetting"
        # The same Reset sent again while it runs.
        with pytest.raises(ProgramRunnerError) as resent:
            runtime.reset_cycle(reset)
        assert resent.value.code == "execution_control_command_in_progress"
    finally:
        release.set()
        release_effect.set()
        thread.join(5)
    assert not thread.is_alive() and errors == []
    status = runtime.cycle_status()
    assert (status["state"], _in_progress_kind(status)) == ("idle_after_reset", None)
    assert journal.read().cycle_generation == 2
    runtime.shutdown()


def test_shutdown_writes_the_final_cycle_row_and_leaves_no_wal(tmp_path):
    import sqlite3

    from manyforge_program_runner.cycle_journal_writer import CYCLE_COLUMNS

    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=_Admission(), cycle_journal=journal,
    )
    failure = {"code": "program_scene_preparation_failed", "message": "Scene needs reload"}
    journal.record_scene_preparation_failure(expected_generation=1, failure=failure)
    with journal._lock:  # noqa: SLF001
        final = tuple(journal._cycle[column] for column in CYCLE_COLUMNS)  # noqa: SLF001

    runtime.shutdown()

    assert not Path(f"{path}-wal").exists()
    database = sqlite3.connect(path)
    try:
        row = database.execute(
            "SELECT " + ",".join(CYCLE_COLUMNS) + " FROM program_cycle"
        ).fetchone()
    finally:
        database.close()
    assert row == final
    assert row[CYCLE_COLUMNS.index("failure_json")] is not None


@pytest.mark.parametrize("idle", [False, True])
@pytest.mark.parametrize("reload_scene,reset_position", [(False, False), (True, False), (False, True), (True, True)])
def test_reset_selectors_order_and_plain_preservation(monkeypatch, tmp_path, idle, reload_scene, reset_position):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim())
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    admission = Admission()
    headless = _Headless()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=headless, gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    events = []
    def effect(name):
        def apply(_request, _authorization):
            status = runtime.cycle_status()
            assert status["state"] == "resetting"
            assert status["program_run_id"] == (None if idle else 1)
            assert status["reset_phase"] == ("restoring_scene" if name == "scene" else "restoring_position")
            events.append(name)
        return apply
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", effect("scene"))
    monkeypatch.setattr(runtime, "_reset_active_tcp", lambda: events.append("tcp"))
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", effect("position"))
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: events.append("preflight"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.options"), "source_label": "operator_ui",
        "program_run_id": None if idle else 1, "expected_cycle_generation": 1,
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim())
    admission = _Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=_Headless(), gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: pytest.fail("refused position Reset touched Scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.unsupported"), "source_label": "operator_ui",
        "program_run_id": None if idle else 1, "expected_cycle_generation": 1,
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


@pytest.mark.parametrize("idle", [False, True])
def test_disk_reason_refuses_position_reset_up_front_and_leaves_reset_without_it(monkeypatch, tmp_path, idle):
    """OD-219: while the Kernel would refuse the move back for the disk, the
    availability answer carries that reason, a Reset asking for the move back
    is refused before anything is done, and one without it is accepted."""

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim())
    admission = Admission()
    transport = _Headless()
    disk_reason = "reset_position_disk_cannot_keep_records"
    condition = {"active": True}
    transport.reset_robot_position_capability = lambda _members=None: {
        "ok": True,
        "reset_robot_position": (
            {"available": False, "reason": disk_reason} if condition["active"]
            else {"available": True, "reason": None}
        )}
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=_Headless(), gateway=transport,
                             admission_client=admission, cycle_journal=journal)
    scene_calls = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: scene_calls.append("scene"))
    monkeypatch.setattr(runtime, "_reset_active_tcp", lambda: None)
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset",
                        lambda *_args: pytest.fail("refused position Reset moved the robot"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.disk.position"), "source_label": "operator_ui",
        "program_run_id": None if idle else 1, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reload_initial_scene": True, "reset_robot_position": True,
    })
    before = journal.read()
    try:
        assert runtime.cycle_status()["reset_robot_position"] == {"available": False, "reason": disk_reason}
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.reset_cycle(request)
        assert refused.value.code == "program_cycle_reset_position_unavailable"
        assert str(refused.value) == disk_reason
        assert journal.read() == before
        assert not journal.pending_commands()
        assert not admission.releases and not scene_calls
        # The same Reset without the move back is accepted while the reason lasts.
        runtime.reset_cycle(request.model_copy(update={
            "request_key": _key("reset.disk.reload"), "reset_robot_position": False}))
        assert scene_calls == ["scene"]
        assert journal.read().state == "idle_after_reset"
        # When the condition ends, the move back is available again.
        condition["active"] = False
        assert runtime.cycle_status()["reset_robot_position"] == {"available": True, "reason": None}
    finally:
        runtime.shutdown()


def test_idle_reload_without_applied_scenario_is_refused_before_any_effect(monkeypatch, tmp_path):
    """No Applied Scenario: Reload is refused, the cycle stays idle, no claim
    is taken, and once a Scenario is loaded the same Reset is admitted."""

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")

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
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=headless, gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    scenes = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: scenes.append("scene"))
    monkeypatch.setattr(runtime, "_reset_persistent_keys_to_initial", lambda _request: scenes.append("keys"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.empty"), "source_label": "operator_ui",
        "program_run_id": None, "expected_cycle_generation": 1,
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
        result = runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.loaded")}))
        assert result["idle_cycle_generation"] == 2
        # Reload alone keeps stored values (OD-393).
        assert scenes == ["scene"]
        assert journal.read().state == "idle_after_reset"
        assert len(admission.claims) == 1 and len(admission.releases) == 1
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [False, True])
@pytest.mark.parametrize("failed_effect", ["scene", "position"])
def test_selected_reset_failure_retains_phase_and_original_options(monkeypatch, tmp_path, idle, failed_effect):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim())
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=_Headless(), gateway=_Headless(),
                             admission_client=admission, cycle_journal=journal)
    events = []
    should_fail = True
    def effect(name):
        def apply(_request, _authorization):
            events.append((name, _request.request_key))
            if name == failed_effect and should_fail:
                raise ProgramRunnerError("test_effect_refused", name)
        return apply
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", effect("scene"))
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", effect("position"))
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.original"), "source_label": "operator_ui",
        "program_run_id": None if idle else 1, "expected_cycle_generation": 1,
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
            runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.conflict"), "reload_initial_scene": False}))
        assert conflict.value.code == "program_run_request_conflict"
        assert journal.pending_commands() == pending
        should_fail = False
        runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.confirmed")}))
        expected = (["scene", "scene", "position"] if failed_effect == "scene" else ["scene", "position", "position"])
        assert events == [(name, _key("reset.original")) for name in expected]
        assert journal.read().causal_request_key == _kb("reset.confirmed")
        # 1.16.2: the cycle status names its causal request by the key only.
        status = runtime._cycle_status_core()  # noqa: SLF001
        assert status["causal_request_key"] == _key("reset.confirmed")
        assert not {"causal_session_number", "causal_client_sequence"} & set(status)
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()


def test_plain_idle_reset_preserves_scene_failure_without_observation(monkeypatch, tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    failure = {"code": "program_scene_preparation_failed", "message": "Scene owner unavailable"}
    journal.record_scene_preparation_failure(expected_generation=1, failure=failure)
    headless = _Headless()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
                             headless_client=headless, gateway=_Headless(),
                             admission_client=_Admission(), cycle_journal=journal)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.plain"), "source_label": "operator_ui",
        "program_run_id": None,
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
        def acquire(self, intent, *, program_cycle_status=None):
            assert journal.read().reset_effect_claim["intent"] == intent
            assert program_cycle_status["state"] == "resetting"
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
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: effects.append("scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.unknown"),
        "source_label": "operator_ui", "program_run_id": None,
        "expected_cycle_generation": 1, "reload_initial_scene": True})
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
        # Reset stays offered; one press makes one settling attempt: the
        # acquire is replayed exactly, a release is not resent before a receipt.
        assert next(row for row in runtime.cycle_status()["allowed_commands"]
                    if row["command_kind"] == "program_reset")["allowed"] is True
        with pytest.raises(ProgramRunnerError) as again:
            runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.fresh.confirmation")}))
        assert again.value.details == {"cause_code": "program_cycle_reconciliation_required"}
        assert admission.claims == [retained["intent"]] * (2 if unknown_at == "acquire" else 1)
        assert effects == ([] if unknown_at == "acquire" else ["scene"])
        assert len(admission.releases) == (0 if unknown_at == "acquire" else 1)
        assert journal.read().reset_effect_claim == retained
        assert journal.read().state == "recovery_required" and journal.read().reconciliation_required
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

    def acquire(self, intent, *, program_cycle_status=None):
        # The exact intent sent is the one durably retained before crossing.
        assert self.journal.read().reset_effect_claim["intent"] == intent
        # The runner's current status travels beside it (OD-139): the
        # accepted Reset, before its claim exists.
        accepted = program_cycle_status["accepted_reset"]
        assert program_cycle_status["state"] == "resetting"
        assert program_cycle_status["cycle_claim_generation"] is None
        assert accepted["reset_claim_generation"] is None
        assert format_request_key(with_part(parse_request_key(accepted["request_key"]),
                                            Part.RESET_CLAIM)) == intent["request_key"]
        self.claims.append(deepcopy(intent))
        if self.refuse:
            raise CellAdmissionError("program_reset_effects_not_settled", "operations not settled")
        return {"intent": deepcopy(intent), "claim_generation": 3,
                "acquired_admission_generation": self.generation, "accepted_at_ns": 1}


def _refused_idle_reset(monkeypatch, tmp_path):
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    admission = _RefusingResetAdmission(journal)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda request, _auth: effects.append(request.request_key))
    writes = []
    remember = journal.remember_reset_effect_claim
    def spy(**kwargs):
        writes.append(deepcopy(kwargs))
        return remember(**kwargs)
    monkeypatch.setattr(journal, "remember_reset_effect_claim", spy)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.refused"),
        "source_label": "operator_ui", "program_run_id": None,
        "expected_cycle_generation": 1, "reload_initial_scene": True})
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
        runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.retry")}))
        assert len(admission.claims) == 2
        first, retry = admission.claims
        # The generation is evidence only: the retry never rewrites it.
        assert retry == first == retained["intent"]
        assert retry["expected_admission_generation"] == 1
        assert not [w for w in writes if w["retained"]["claim"] is None]
        assert writes[0]["expected"]["intent"] == retry
        assert effects == [_key("reset.refused")]
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
        runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.retry")}))
        assert admission.claims == [retained["intent"], retained["intent"]]
        assert admission.claims[1]["expected_admission_generation"] == 1
        assert not [w for w in writes if w["retained"]["claim"] is None]
        assert writes[0]["expected"] == retained
        assert effects == [_key("reset.refused")]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("fail_position", [False, True])
def test_position_only_idle_reset_preserves_prior_scene_failure_until_selected_reload(monkeypatch, tmp_path, fail_position):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    original = {"code": "program_scene_preparation_failed", "message": "original Scene failure"}
    journal.record_scene_preparation_failure(expected_generation=1, failure=original)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=Admission(), cycle_journal=journal)
    scene_calls = []
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    def position(*_args):
        assert journal.read().failure == original
        if fail_position:
            raise ProgramRunnerError("position_refused", "known position refusal")
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", position)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: scene_calls.append("scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.position"),
        "source_label": "operator_ui", "program_run_id": None,
        "expected_cycle_generation": 1, "reset_robot_position": True})
    try:
        if fail_position:
            with pytest.raises(ProgramRunnerError):
                runtime.reset_cycle(request)
            assert journal.read().failure["prior_scene_preparation_failure"] == original
            fail_position = False
            runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.position.confirmed")}))
        else:
            runtime.reset_cycle(request)
        assert journal.read().failure == original and not scene_calls
        assert next(row for row in runtime.cycle_status()["allowed_commands"] if row["command_kind"] == "program_run")["allowed"] is False
        runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.reload"), "expected_cycle_generation": 2,
            "reset_robot_position": False, "reload_initial_scene": True}))
        assert journal.read().failure is None and scene_calls == ["scene"]
    finally:
        runtime.shutdown()


def test_completed_reload_clears_prior_scene_fault_before_later_position_failure(monkeypatch, tmp_path):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    original = {"code": "program_scene_preparation_failed", "message": "Scene needs reload"}
    journal.record_scene_preparation_failure(expected_generation=1, failure=original)
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=Admission(), cycle_journal=journal)
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: None)
    def position(*_args):
        assert journal.read().reset_phase == "restoring_position"
        assert journal.read().failure is None
        raise ProgramRunnerError("position_refused", "known position refusal")
    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", position)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.both"),
        "source_label": "operator_ui", "program_run_id": None, "expected_cycle_generation": 1,
        "reset_robot_position": True, "reload_initial_scene": True})
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        assert journal.read().failure["code"] == "position_refused"
        assert "prior_scene_preparation_failure" not in journal.read().failure
    finally:
        runtime.shutdown()


def test_private_heartbeat_never_reads_position_capability_but_public_status_does(tmp_path):
    transport = _Headless()
    def blocked_capability(_members=None):
        raise AssertionError("Private heartbeat called the capability port")
    transport.reset_robot_position_capability = blocked_capability
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=transport, admission_client=_Admission(),
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"))
    runtime.start()
    reads = []
    try:
        assert runtime._current_cycle_status_projection()["accepted_reset"] is None
        transport.reset_robot_position_capability = lambda _members=None: reads.append("current") or {
            "ok": True, "reset_robot_position": {"available": True, "reason": None}}
        assert runtime.cycle_status()["reset_robot_position"] == {"available": True, "reason": None}
        assert reads == ["current"]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("invalid", [None, "missing", "digest", "kind", "claim"])
def test_selected_reload_after_proven_never_started_run_uses_reset_claim(monkeypatch, tmp_path, invalid):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    settlement = {"schema_version": "manyforge.program_cycle_start_settlement.v2", "kind": "proven_no_effect",
                  "claim": None, "program_run_id": 61, "prepared_scene": {}}
    if invalid == "kind": settlement["kind"] = "assumed"
    if invalid == "claim": settlement["claim"] = {"claim_generation": 1}
    reference = ProgramRuntime._digest(settlement) if invalid != "digest" else _DIGEST_B
    journal.transition(expected_generation=1, expected_states=("idle_after_reset",), state="terminal_awaiting_reset",
        program_run_id=61, terminal_reference=reference,
        start_settlement=None if invalid == "missing" else settlement)
    admission = Admission()
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(), admission_client=admission, cycle_journal=journal)
    effects = []
    def scene(_request, authorization):
        assert journal.read().program_run_id == 61
        assert journal.read().claim is None
        assert authorization["reset_claim"]["reset_claim_generation"] == 1
        assert journal.read().reset_effect_claim["claim"]["intent"]["claim_kind"] == "program_reset"
        effects.append("scene")
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", scene)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.never-started"),
        "source_label": "operator_ui", "program_run_id": 61, "expected_cycle_generation": 1,
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
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}
    transport = _Headless()
    identity = {"application_start_id": "3f9a00000000002a", "active_publication_generation": 3,
                "retained_bundle_id": _DIGEST_A, "graph_revision": _DIGEST_B}
    transport.reset_robot_position_capability = lambda _members=None: {
        "ok": True, "reset_robot_position": {"available": True, "reason": None},
        "identity": {**identity, "scene_revision": "ignored"}}
    effects = []
    def position(members):
        effects.append(members)
        # The host's refusal reply.
        return {"ok": False, "error": {"code": "position_failed", "message": "Native position failure",
            "details": {} if effect_disposition is None else {"effect_disposition": effect_disposition}}}
    transport.reset_robot_position = position
    admission = Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=transport, admission_client=admission, cycle_journal=journal)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({"request_key": _key("reset.position"),
        "source_label": "operator_ui", "program_run_id": None,
        "expected_cycle_generation": 1, "reset_robot_position": True})
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        assert len(effects) == 1 and not admission.releases
        assert effects[0]["expected_deployment_identity"] == identity
        assert effects[0]["authorization"]["request_key"] == request.request_key
        assert effects[0]["authorization"]["reset_claim_generation"] == 1
        # The runner's current cycle status names the same claimed Reset (OD-139).
        carried = effects[0]["authorization"]["program_cycle_status"]
        assert carried["state"] == "resetting" and carried["program_run_id"] is None
        assert carried["accepted_reset"]["request_key"] == request.request_key
        assert carried["accepted_reset"]["reset_claim_generation"] == 1
        assert journal.read().reset_phase == "restoring_position"
        assert journal.read().reconciliation_required == (effect_disposition != "known_no_effect")
        if effect_disposition != "known_no_effect":
            assert journal.read().failure["message"] == (
                "Robot position unknown after a failed position reset."
            )
            # A fresh confirmation runs the retained position reset again, once.
            with pytest.raises(ProgramRunnerError):
                runtime.reset_cycle(request.model_copy(update={"request_key": _key("fresh.confirmation")}))
            assert len(effects) == 2
            assert effects[1]["authorization"]["request_key"] == request.request_key
            assert journal.read().reconciliation_required
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
        inventory={},
    )
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    owner.rows[1].capture.node_policy_by_instance_id = {
        "move.2": SimpleNamespace(
            recovery_policy={"allow_intervention": True, "max_retries": 1}
        )
    }
    runtime._controls(1).note_retry("move.2")  # Retries spent.
    focus = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1, node_instance_id="move.1", node_attempt_id=1
        ),
        owner_task_id="operation.1",
        policy_parameter="recovery_policy",
    )
    second = SimpleNamespace(
        identity=SimpleNamespace(
            program_run_id=1, node_instance_id="move.2", node_attempt_id=2
        ),
        owner_task_id="operation.2",
        policy_parameter="recovery_policy",
    )
    lookups: list[Any] = []

    def intervention(_run, *, node_attempt_id=None):
        lookups.append(node_attempt_id)
        return second if node_attempt_id == 2 else focus

    owner.intervention = intervention
    focus_offer = {"offer_id": _DIGEST_A, "offer_revision": 1, "node_attempt_id": 1,
                   "actions": [{"action_id": "retry_runtime_step", "enabled": True}]}
    second_offer = {"offer_id": _DIGEST_B, "offer_revision": 2, "node_attempt_id": 2,
                    "actions": [{"action_id": "continue_with_failure", "enabled": True}]}
    runtime._recovery_offer = lambda *_args, **_kwargs: deepcopy(focus_offer)
    runtime._recovery_offers = lambda *_args, **_kwargs: [
        {"motion_group_ids": ["tools"], "intervention_stop_scope": "all", "offer": deepcopy(second_offer)}
    ]
    calls: list[Any] = []

    def continue_failure_pause(run_id, **kwargs):
        calls.append(("continue_failure_pause", kwargs["node_attempt_id"], kwargs["request_key"]))
        return owner.read(run_id)

    def resume_recovery(run_id, **kwargs):
        calls.append(("resume_recovery", kwargs["node_attempt_id"], kwargs["action"],
                      kwargs.get("result")))
        return owner.read(run_id)

    def forbidden(*_args, **_kwargs):
        raise AssertionError("Continue is neither a Retry nor an owner action")

    host.continue_failure_pause = continue_failure_pause
    host.retry_failure_pause = forbidden
    owner.apply_owner_recovery = forbidden
    host.resume_recovery = resume_recovery
    request = CycleRecoveryActionRequest.model_validate(
        {
            "request_key": _key("continue.1"),
            "source_label": "test",
            "offer_id": second_offer["offer_id"],
            "offer_revision": second_offer["offer_revision"],
            "action_id": "continue_with_failure",
        }
    )
    try:
        # Continue lets held motion continue: it needs motion_clear,
        # refused with the readiness reason before any owner effect.
        headless = runtime._headless
        headless.readiness = {
            "can_execute": False, "can_resume": False, "motion_clear": False,
            "blocked_reasons": [{"code": "cell_latched", "message": "robot.1: offline"}],
        }
        with pytest.raises(ProgramRunnerError) as blocked:
            runtime.apply_recovery_action(request.model_copy(update={"request_key": _key("continue.0")}))
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
        assert owner.attention_checks == [(1, "move.2", _kb("continue.1"))]
        assert calls == []
        assert journal.read().state == "running"
        assert not journal.read().reconciliation_required
        owner.attention_refusal = None
        lookups.clear()
        request = request.model_copy(update={"request_key": _key("continue.2")})
        result = runtime.apply_recovery_action(request)
        assert owner.attention_checks[-1] == (1, "move.2", _kb("continue.2"))
        # The offer id selected the second hold, not the focused one.
        assert lookups == [2]
        assert calls == [
            ("continue_failure_pause", 2, _kb("continue.2")),
            # Continue's marker (the Acknowledge of an unconfirmed Stop).
            ("resume_recovery", 2, "manual_failure", "continue_with_failure"),
        ]
        assert result["cycle_resumed"] is True
        assert result["owner_result"] == {"accepted": True, "action": "continue_with_failure"}
        # Continue counts no Retry, even with the Retry budget spent.
        assert runtime._controls(1).retry_count("move.2") == 1
        assert not journal.read().reconciliation_required
    finally:
        runtime.shutdown()


def test_an_operator_question_is_answered_through_the_recovery_action(
    monkeypatch, tmp_path
):
    """OD-342: the question is published with the runtime controls (v11) and
    answered with its option id as the action; no motion gate, no Resume."""
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, journal, _admission = _reconcile_runtime(tmp_path, inventory={})
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    runtime.start()
    runtime.start_run(_start_request())
    options = (
        {"option_id": "continue", "label": "Bin emptied, continue", "result": "success"},
        {"option_id": "abort", "label": "Abort this cycle", "result": "failure"},
    )
    open_questions = [
        {
            "node_instance_id": "ask_bin_full",
            "node_attempt_id": 41,
            "message": "Bin is full. Empty it, then choose.",
            "severity": "warning",
            "options": options,
            "hold_motion_group_ids": ("left_arm",),
        }
    ]
    owner.operator_questions = lambda _run_id: tuple(open_questions)
    answers: list[tuple[int, dict[str, Any]]] = []

    def answer_operator_question(run_id, **kwargs):
        answers.append((run_id, kwargs))
        open_questions.clear()  # The run's tick no longer publishes it.
        return next(row for row in options if row["option_id"] == kwargs["option_id"])

    def forbidden(*_args, **_kwargs):
        raise AssertionError("an answer is neither a Resume nor an owner recovery")

    host.answer_operator_question = answer_operator_question
    host.resume_recovery = forbidden
    host.retry_failure_pause = forbidden
    host.continue_failure_pause = forbidden
    try:
        projection = runtime.read_runtime_controls(1)
        assert projection["schema_version"] == (
            "manyforge.program_runtime_control_projection.v11"
        )
        [offer] = projection["operator_questions"]
        assert offer["schema_version"] == "manyforge.program_operator_question.v1"
        assert offer["program_run_id"] == 1
        assert offer["node_instance_id"] == "ask_bin_full"
        assert offer["node_attempt_id"] == 41
        assert offer["options"] == [dict(row) for row in options]
        assert offer["hold_motion_group_ids"] == ["left_arm"]

        def request(name: str, **changes: Any) -> CycleRecoveryActionRequest:
            return CycleRecoveryActionRequest.model_validate(
                {
                    "request_key": _key(name),
                    "source_label": "test",
                    "offer_id": offer["offer_id"],
                    "offer_revision": offer["offer_revision"],
                    "action_id": "abort",
                    **changes,
                }
            )

        for name, changes in (
            ("question.stale", {"offer_revision": offer["offer_revision"] + 1}),
            ("question.unknown", {"action_id": "retry_runtime_step"}),
        ):
            with pytest.raises(ProgramRunnerError) as stale:
                runtime.apply_recovery_action(request(name, **changes))
            assert stale.value.code == "cycle_recovery_offer_stale"
        assert answers == []

        # Answering moves nothing: no motion_clear or motion-attention gate.
        runtime._headless.readiness = {
            "can_execute": False, "can_resume": False, "motion_clear": False,
            "blocked_reasons": [{"code": "cell_latched", "message": "robot.1: offline"}],
        }
        result = runtime.apply_recovery_action(request("question.answer"))
        assert answers == [
            (1, {"node_instance_id": "ask_bin_full", "node_attempt_id": 41,
                 "option_id": "abort"})
        ]
        assert owner.attention_checks == []
        assert result["schema_version"] == "manyforge.cycle_recovery_result.v3"
        assert result["request_key"] == _key("question.answer")
        assert result["accepted"] is True
        assert result["action_id"] == "abort"
        assert result["cycle_resumed"] is False
        assert result["result_code"] == "accepted"
        assert result["owner_result"] == {
            "accepted": True, "action": "operator_choice", "option_id": "abort",
        }
        assert result["run"]["capture"]["program_run_id"] == 1
        assert result["runtime_controls"]["schema_version"] == (
            "manyforge.program_runtime_control_projection.v11"
        )
        assert result["runtime_controls"]["operator_questions"] == []
        assert "recovery_offer" in result
        audit = runtime._controls(1).snapshot()["audit"][-1]
        assert {key: value for key, value in audit.items() if key != "event_revision"} == {
            "kind": "program_operator_choice_answered",
            "node_instance_id": "ask_bin_full",
            "node_attempt_id": 41,
            "option_id": "abort",
            "label": "Abort this cycle",
            "result": "failure",
            "request_key": _key("question.answer"),
            "source_label": "test",
        }
        assert journal.read().state == "running"
        assert not journal.read().reconciliation_required

        # Answered: the same question is stale.
        with pytest.raises(ProgramRunnerError) as again:
            runtime.apply_recovery_action(request("question.again", action_id="continue"))
        assert again.value.code == "cycle_recovery_offer_stale"
        assert len(answers) == 1
    finally:
        runtime.shutdown()


def _terminal(state: str, run_end: dict[str, Any], run_id: str = 1) -> dict[str, Any]:
    return {"state": state, "capture": {"program_run_id": run_id}, "run_end": run_end}


def _run_end_runtime(headless: _Headless) -> ProgramRuntime:
    runtime = ProgramRuntime.__new__(ProgramRuntime)
    runtime._headless = headless  # type: ignore[assignment]  # noqa: SLF001
    runtime._kernel = KernelClient(headless)  # type: ignore[arg-type]  # noqa: SLF001
    runtime._journal = None  # noqa: SLF001
    runtime._run_ends = {}  # noqa: SLF001
    runtime._run_finished_event_id = None  # noqa: SLF001
    runtime._run_finished_event_lock = Lock()  # noqa: SLF001
    runtime.run_events = []  # type: ignore[attr-defined]
    runtime._node_attempts = SimpleNamespace(  # noqa: SLF001
        submit_run_event=runtime.run_events.append,  # type: ignore[attr-defined]
    )
    return runtime


def test_a_run_that_ends_while_the_cell_is_latched_ends_by_a_cell_fault() -> None:
    """The latch is the error; the run end names its component.
    Every rule returns to `active_at_start` once at the end of the run."""

    headless = _Headless()
    headless.latch = {"latched": True, "causes": [
        {"component_id": "robot.1", "kind": "fault", "fault": {"code": "fault_protective_stop"}}]}
    runtime = _run_end_runtime(headless)
    failed = {"end_reason": "failed", "details": {"node_instance_id": "move",
              "error": {"code": "interrupt_fault", "message": "stopped"}}}
    runtime._record_terminal_snapshot(_terminal("failed", failed))  # noqa: SLF001
    runtime._record_terminal_snapshot(_terminal("failed", failed))  # noqa: SLF001
    expected = {"end_reason": "cell_fault", "details": {"component_id": "robot.1"}}
    assert runtime._project_run(_terminal("failed", failed))["run_end"] == expected  # noqa: SLF001
    assert headless.rule_resets == 1
    # History's one end event names the cell fault (OD-381 P2).
    [finished] = runtime.run_events  # type: ignore[attr-defined]
    assert (finished["event"], finished["outcome"], finished["facts"]) == (
        "program_run_finished", "failed",
        {"end_reason": "cell_fault", "component_id": "robot.1"})
    assert finished["error"]["code"] == "program_tree_failed"


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
    runtime._record_terminal_snapshot(_terminal("running", None, 2))  # noqa: SLF001
    assert headless.rule_resets == 1


def test_start_refuses_a_set_rule_active_naming_an_unknown_rule(monkeypatch, tmp_path) -> None:
    """A node naming a rule the Deployment lacks blocks Execute."""

    admitted = _PreparedRunAdmission()
    admitted.program.rule_activation_rule_ids = ("missing",)
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: admitted,
    )
    headless = _Headless()
    admission = _Admission()
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=headless,  # type: ignore[arg-type]
        gateway=headless,  # type: ignore[arg-type]
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


def test_run_number_is_announced_only_after_the_kernel_claim(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """K-6: until the claim is journaled no client sees the run number, so
    every Stop naming the run finds the Kernel's fence."""

    runtime, journal, admission, _owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, finish_cycle=False
    )
    seen: list[tuple[str, Any]] = []
    acquire = admission.acquire

    def acquire_observed(intent: dict[str, Any]) -> dict[str, Any]:
        status = runtime.cycle_status()
        seen.append((status["state"], status["program_run_id"]))
        # The journal already holds the provisional number internally.
        assert journal.read().program_run_id == 1
        return acquire(intent)

    monkeypatch.setattr(admission, "acquire", acquire_observed)
    try:
        request = _start_request()
        runtime.start_run(request)
        assert seen == [("starting", None)]
        assert runtime.cycle_status()["program_run_id"] == 1
        # The Run's claim is its first Kernel action: the Run's key, part 0.
        assert admission.claims[0]["request_key"] == request.request_key
        # The Run's claim names its run number as a JSON integer.
        assert admission.claims[0]["owner_transaction_id"] == 1
        assert "owner_epoch" not in admission.claims[0]
        assert "request_payload_digest" not in admission.claims[0]
    finally:
        runtime.shutdown()


def test_reset_and_run_keys_never_self_conflict(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    """Every Kernel action a request causes takes its own fixed part of that
    request's key, so a Run, its Reset and the next selected Reset never
    reuse one key for different content."""

    runtime, journal, admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, finish_cycle=False
    )
    host.contain_reset_run = lambda program_run_id, **_kwargs: owner.rows[program_run_id]
    try:
        run = _start_request(request_id="keys.run")
        runtime.start_run(run)
        cycle = journal.read()
        reset = ProgramCycleResetRequest.model_validate(
            {
                "request_key": _key("keys.reset"),
                "source_label": "operator_ui",
                "program_run_id": cycle.program_run_id,
                "expected_cycle_generation": cycle.cycle_generation,
                "expected_claim_generation": cycle.claim["claim_generation"],
                "terminal_reference": None,
                "settlement_evidence_digest": None,
            }
        )
        runtime.reset_cycle(reset)
        selected = ProgramCycleResetRequest.model_validate(
            {
                "request_key": _key("keys.selected-reset"),
                "source_label": "operator_ui",
                "program_run_id": None,
                "expected_cycle_generation": 2,
                "reload_initial_scene": True,
            }
        )
        monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: None)
        monkeypatch.setattr(runtime._headless, "program_applied", lambda: {}, raising=False)  # noqa: SLF001
        admission.get = lambda: {"admission_generation": 1, "authoring_allowed": True}
        runtime.reset_cycle(selected)

        run_claim = bytes.fromhex(admission.claims[0]["request_key"])
        reset_end = host.stop_keys[-1]
        reset_claim = bytes.fromhex(admission.claims[1]["request_key"])
        assert run_claim == _kb("keys.run")
        assert reset_end == with_part(_kb("keys.reset"), Part.RESET_FORCED_END)
        assert reset_claim == with_part(_kb("keys.selected-reset"), Part.RESET_CLAIM)
        # A release carries the key of the Reset that causes it.
        assert [bytes.fromhex(row["request_key"]) for row in admission.releases] == [
            _kb("keys.reset"),
            _kb("keys.selected-reset"),
        ]
        keys = [run_claim, reset_end, reset_claim, _kb("keys.reset"), _kb("keys.selected-reset")]
        assert len(set(keys)) == len(keys)
        # The start-race Forced Program End part never meets a Reset's parts.
        assert with_part(run_claim, Part.START_RACE_FORCED_END) not in keys
    finally:
        runtime.shutdown()


def test_hold_facts_go_to_the_scheduling_host_and_a_failure_only_logs(
    monkeypatch: pytest.MonkeyPatch, tmp_path, caplog
) -> None:
    """§6.3.2: the follower hands the Kernel's hold facts to
    ProgramRunTickHost.apply_hold_facts (memory only); a failure there is
    logged once and touches neither the journal nor the run (no journal read
    or reconciliation on the follower thread)."""

    runtime, journal, _admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, finish_cycle=False
    )
    try:
        runtime.start_run(_start_request())
        facts = _hold_facts(1, 21, event_id=21)
        runtime._apply_program_run_hold(facts)  # noqa: SLF001
        assert host.applied_hold_facts == [facts]
        assert owner.rows[1].state == "halted"
        assert not journal.read().reconciliation_required

        calls: list[object] = []

        def failing(taken) -> None:
            calls.append(taken)
            raise RuntimeError("hold facts defect")

        def no_journal_read(*_args, **_kwargs):
            raise AssertionError("the follower must not read the cycle journal")

        before = journal.read()
        later = replace(facts, hold_revision=22, stop_progress_sequence=1)
        with monkeypatch.context() as patch, caplog.at_level("ERROR"):
            patch.setattr(host, "apply_hold_facts", failing)
            patch.setattr(journal, "read", no_journal_read)
            patch.setattr(journal, "require_reconciliation", no_journal_read)
            runtime._apply_program_run_hold(later)  # noqa: SLF001
        assert calls == [later]
        assert any(
            "hold revision 22" in record.getMessage() for record in caplog.records
        )
        cycle = journal.read()
        assert cycle.reconciliation_required == before.reconciliation_required
        assert cycle.failure == before.failure
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(("state", "halt"), [
    ("halting", {"kind": "internal_fault", "source_label": "kernel.internal_fault"}),
    ("halted", {"kind": "halt", "source_label": None}),
    ("running", None),
])
def test_the_run_projection_carries_the_cause_of_its_halt(state, halt) -> None:
    runtime = _run_end_runtime(_Headless())
    run = {"state": state, "capture": {"program_run_id": 1}, "run_end": None, "halt": halt}
    assert runtime._project_run(run)["halt"] == halt  # noqa: SLF001


# Reset is offered in every recovery_required variant and first settles the
# unknown Kernel exchange from the Kernel's own record.


def _allowed(runtime: ProgramRuntime) -> dict[str, dict[str, Any]]:
    return {
        row["command_kind"]: row for row in runtime.cycle_status()["allowed_commands"]
    }


class _UnknownAcquireAdmission(_Admission):
    """The cycle claim's first acquire loses its reply; the replay does `replay`."""

    def __init__(self, replay: str) -> None:
        super().__init__()
        self.replay = replay

    def acquire(self, intent, *, program_cycle_status=None):
        assert program_cycle_status is None
        self.claims.append(deepcopy(intent))
        if len(self.claims) == 1 or self.replay == "unknown":
            raise CellAdmissionError(
                "execution_owner_unavailable", "acquire reply lost", outcome_unknown=True
            )
        if self.replay == "refused":
            raise CellAdmissionError("authoring_not_ready", "admission refused")
        return {"intent": deepcopy(intent), "claim_generation": 1,
                "acquired_admission_generation": 1, "accepted_at_ns": 1}


@pytest.mark.parametrize("replay", ["claim", "refused", "unknown"])
def test_reset_replays_unknown_cycle_acquire_once_per_press(monkeypatch, tmp_path, replay):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    admission = _UnknownAcquireAdmission(replay)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.start_run(_start_request())
        unknown = journal.read()
        assert unknown.state == "recovery_required" and unknown.reconciliation_required
        assert unknown.claim is None and unknown.acquisition_pending is not None
        assert unknown.failure == {"code": "program_cycle_acquire_outcome_unknown"}
        assert journal.pending_commands() == ()
        # The status names the cycle (by its generation; no run is announced
        # before the Kernel holds its claim) and offers Reset, not recovery.
        status = runtime.cycle_status()
        assert status["program_run_id"] is None and status["cycle_generation"] == 1
        allowed = _allowed(runtime)
        assert allowed["program_reset"]["allowed"] is True
        assert allowed["program_reset"]["semantic_target"] == {
            "cycle_generation": 1, "program_run_id": None,
        }
        assert allowed["execution_recovery"]["allowed"] is False
        request = ProgramCycleResetRequest.model_validate({
            "request_key": _key("reset.acquire"), "source_label": "operator_ui",
            "program_run_id": None, "expected_cycle_generation": 1,
        })
        if replay == "unknown":
            with pytest.raises(ProgramRunnerError) as again:
                runtime.reset_cycle(request)
            assert again.value.code == "program_cycle_reconciliation_required"
            assert admission.claims == [unknown.acquisition_pending] * 2
            assert journal.read() == unknown
            assert _allowed(runtime)["program_reset"]["allowed"] is True
            return
        result = runtime.reset_cycle(request)
        # One press: one exact replay of the retained acquire.
        assert admission.claims == [unknown.acquisition_pending] * 2
        assert result["idle_cycle_generation"] == 2
        idle = journal.read()
        assert idle.state == "idle_after_reset" and idle.cycle_generation == 2
        assert not idle.reconciliation_required and idle.acquisition_pending is None
        assert idle.claim is None and idle.failure is None
        assert journal.pending_commands() == ()
        if replay == "claim":
            # A claimed start that never reached the host, then the ordinary release.
            assert result["program_run_id"] == 1
            assert [release["claim_generation"] for release in admission.releases] == [1]
        else:
            # No claim exists: the rejected-acquire path, then an idle Reset.
            assert result["program_run_id"] is None
            assert admission.releases == []
    finally:
        runtime.shutdown()


class _ReceiptAdmission(_Admission):
    def __init__(self) -> None:
        super().__init__()
        self.receipt_queries: list[dict[str, Any]] = []

    def recovery_receipt(self, query: dict[str, Any]) -> dict[str, Any]:
        self.receipt_queries.append(deepcopy(query))
        return super().recovery_receipt(query)


@pytest.mark.parametrize("receipt", ["released_exact", "held_exact", "unknown"])
def test_reset_settles_unknown_run_release_from_receipt_before_any_resend(
    monkeypatch, tmp_path, receipt,
):
    _bypass_reset_scene_preparation(monkeypatch)
    journal = ProgramCycleJournal(
        tmp_path / "cycle.sqlite3", maximum_inflight_commands=1,
    )
    intent = {"owner_role": "program_runner", "owner_transaction_id": 1,
              "request_key": _key("start.1")}
    claim = {"intent": intent, "claim_generation": 7,
             "acquired_admission_generation": 6, "accepted_at_ns": 1}
    journal.begin_acquire(expected_generation=1, provisional_run_id=1, evidence=intent)
    journal.remember_acquired_claim(expected_generation=1, provisional_run_id=1, claim=claim)
    terminal_reference = _retain_no_effect_terminal(journal, program_run_id=1)
    admission = _ReceiptAdmission()
    admission.release_error = CellAdmissionError(
        "execution_owner_unavailable", "release reply lost", outcome_unknown=True,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=admission,  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.1"), "source_label": "operator_ui",
        "program_run_id": 1, "expected_cycle_generation": 1,
        "expected_claim_generation": 7, "terminal_reference": terminal_reference,
        "settlement_evidence_digest": terminal_reference,
    })
    runtime.start()
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        latched = journal.read()
        assert latched.reconciliation_required and latched.reset_phase == "releasing"
        sent = admission.releases[0]
        assert latched.release_intent == sent
        allowed = _allowed(runtime)
        assert allowed["program_reset"]["allowed"] is True
        assert allowed["execution_recovery"]["allowed"] is False

        admission.release_error = None
        admission.inventory = {
            "admission_generation": 2,
            "held_claims": [] if receipt == "released_exact" else [claim],
            "matched_claim": None if receipt == "unknown" else claim,
            "matched_release": sent if receipt == "released_exact" else None,
            "status": receipt,
        }
        retry = request.model_copy(update={"request_key": _key("reset.2")})
        if receipt == "unknown":
            with pytest.raises(ProgramRunnerError) as again:
                runtime.reset_cycle(retry)
            assert again.value.code == "program_cycle_recovery_required"
            assert again.value.details == {"cause_code": "program_cycle_reconciliation_required"}
            assert admission.releases == [sent]
            still = journal.read()
            assert still.state == "recovery_required" and still.reconciliation_required
            assert still.reset_phase == "releasing" and still.release_intent == sent
            assert _allowed(runtime)["program_reset"]["allowed"] is True
        else:
            assert runtime.reset_cycle(retry)["idle_cycle_generation"] == 2
            assert journal.read().state == "idle_after_reset"
            assert journal.pending_commands() == ()
            # Released exactly: nothing is resent. Held exactly: the retained
            # release is resent once.
            assert admission.releases == [sent] * (1 if receipt == "released_exact" else 2)
        assert admission.receipt_queries == [
            {"intent": intent, "expected_claim": claim, "expected_release": sent}
        ]
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("settle", ["acquire_claim", "release_released_exact", "release_held_exact"])
def test_no_run_reset_settles_unknown_reset_claim_exchange(monkeypatch, tmp_path, settle):
    unknown_at = "acquire" if settle == "acquire_claim" else "release"

    class Admission(_ReceiptAdmission):
        lose = True

        def get(self):
            return {"admission_generation": 41, "authoring_allowed": True}

        def acquire(self, intent, *, program_cycle_status=None):
            assert journal.read().reset_effect_claim["intent"] == intent
            assert program_cycle_status["state"] == "resetting"
            self.claims.append(deepcopy(intent))
            if unknown_at == "acquire" and self.lose:
                raise CellAdmissionError("transport_unavailable", "Acquire reply lost", outcome_unknown=True)
            return {"intent": deepcopy(intent), "claim_generation": 7,
                    "acquired_admission_generation": 41, "accepted_at_ns": 1}

        def release(self, release):
            self.releases.append(deepcopy(release))
            if unknown_at == "release" and self.lose:
                raise CellAdmissionError("transport_unavailable", "Release reply lost", outcome_unknown=True)
            return {"admission_generation": 42, "authoring_allowed": True}

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    admission = Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=admission, cycle_journal=journal,
    )
    effects = []
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", lambda *_args: effects.append("scene"))
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.unknown"), "source_label": "operator_ui",
        "program_run_id": None, "expected_cycle_generation": 1, "reload_initial_scene": True,
    })
    try:
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        latched = journal.read()
        assert latched.state == "recovery_required" and latched.reconciliation_required
        retained = deepcopy(latched.reset_effect_claim)
        assert _allowed(runtime)["program_reset"]["allowed"] is True
        admission.lose = False
        if settle != "acquire_claim":
            claim = retained["claim"]
            admission.inventory = {
                "admission_generation": 42,
                "held_claims": [] if settle == "release_released_exact" else [claim],
                "matched_claim": claim,
                "matched_release": (retained["release_intent"]
                                    if settle == "release_released_exact" else None),
                "status": "released_exact" if settle == "release_released_exact" else "held_exact",
            }
        result = runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.again")}))
        assert result["idle_cycle_generation"] == 2
        assert journal.read().state == "idle_after_reset"
        assert not journal.read().reconciliation_required
        # The scene effect ran exactly once over both presses.
        assert effects == ["scene"]
        if settle == "acquire_claim":
            # The retained intent was replayed exactly, never rebuilt.
            assert admission.claims == [retained["intent"]] * 2
            assert admission.receipt_queries == []
            assert len(admission.releases) == 1
        else:
            assert admission.claims == [retained["intent"]]
            assert admission.receipt_queries == [{
                "intent": retained["intent"], "expected_claim": retained["claim"],
                "expected_release": retained["release_intent"],
            }]
            assert admission.releases == (
                [retained["release_intent"]] * (1 if settle == "release_released_exact" else 2)
            )
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [True, False])
def test_mock_position_failure_after_possible_effect_is_retried_by_reset(monkeypatch, tmp_path, idle):
    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

        def acquire(self, intent, *, program_cycle_status=None):
            self.claims.append(deepcopy(intent))
            return {"intent": deepcopy(intent), "claim_generation": 3,
                    "acquired_admission_generation": 1, "accepted_at_ns": 1}

    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3")
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim())
    admission = Admission()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),
        admission_client=admission, cycle_journal=journal,
    )
    monkeypatch.setattr(runtime, "_require_reset_robot_position_available", lambda _request: None)
    attempts = []
    failures = 2

    def position(request, _authorization):
        attempts.append(request.request_key)
        assert journal.read().state == "resetting"
        if len(attempts) <= failures:
            raise ProgramRunnerError(
                "program_cycle_reconciliation_required",
                "Robot position unknown after a failed position reset.",
            )

    monkeypatch.setattr(runtime, "_reset_robot_position_after_reset", position)
    runtime.start()
    request = ProgramCycleResetRequest.model_validate({
        "request_key": _key("reset.position"), "source_label": "operator_ui",
        "program_run_id": None if idle else 1, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reset_robot_position": True,
    })
    try:
        for press in range(failures):
            with pytest.raises(ProgramRunnerError) as failed:
                runtime.reset_cycle(request.model_copy(update={"request_key": _key(f"reset.{press}")}))
            assert failed.value.code == "program_cycle_recovery_required"
            assert failed.value.details == {"cause_code": "program_cycle_reconciliation_required"}
            latched = journal.read()
            assert latched.state == "recovery_required" and latched.reconciliation_required
            assert latched.reset_phase == "restoring_position"
            assert "Restart ManyForge" not in latched.failure["message"]
            # One attempt per press; Reset is offered again.
            assert len(attempts) == press + 1
            assert _allowed(runtime)["program_reset"]["allowed"] is True
            assert not admission.releases
        result = runtime.reset_cycle(request.model_copy(update={"request_key": _key("reset.last")}))
        assert result["idle_cycle_generation"] == 2
        # Every attempt runs the retained Reset, exactly as journaled.
        assert attempts == [_key("reset.0")] * (failures + 1)
        settled = journal.read()
        assert settled.state == "idle_after_reset" and not settled.reconciliation_required
        assert journal.pending_commands() == ()
        assert len(admission.releases) == 1
        # An idle Reset's claim is acquired once and kept across its retries.
        assert len(admission.claims) == (1 if idle else 0)
    finally:
        runtime.shutdown()


@pytest.mark.parametrize(
    "kind",
    ["runtime_blackboard_write", "persistent_values_clear", "manual_result_stage",
     "recovery_action"],
)
def test_reset_recovers_any_uncertain_command_of_the_run(uncertain_reset_runtime, kind):
    runtime, journal, admission, owner, host = uncertain_reset_runtime
    # The fixture's lost Resume, retained as another uncertain command kind.
    with journal._lock:  # noqa: SLF001
        journal._inflight = {  # noqa: SLF001
            key: (kind, '{"action_id":"continue_with_failure","program_run_id":1}', generation)
            for key, (_kind, _intent, generation) in journal._inflight.items()  # noqa: SLF001
        }
    assert _allowed(runtime)["program_reset"]["allowed"] is True
    assert _allowed(runtime)["execution_recovery"]["allowed"] is False
    result = runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
    assert result["idle_cycle_generation"] == 2
    # One press: one Stop, one containment, one release.
    assert len(host.stop_calls) == 1 and host.reset_containment_calls == [1]
    assert len(admission.releases) == 1
    assert journal.pending_commands() == ()
    assert journal.read().state == "idle_after_reset"


def _failed_host_start_runtime(monkeypatch, tmp_path, *, run_created: bool):
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)
    runtime, journal, admission = _reconcile_runtime(tmp_path, inventory={})
    owner = _ObservationOwner()
    host = _Host(owner, owner_id=runtime.owner_id)
    runtime._owner, runtime._host = owner, host
    start = host.start_run

    def lost_start(run_admission, **kwargs):
        if run_created:
            start(run_admission, **kwargs)
        raise RuntimeError("owner response was lost")

    host.start_run = lost_start
    runtime.start()
    with pytest.raises(ProgramRunnerError) as unknown:
        runtime.start_run(_start_request())
    assert unknown.value.code == "program_cycle_reconciliation_required"
    cycle = journal.read()
    assert cycle.reconciliation_required and cycle.claim is not None
    assert cycle.start_settlement is None
    assert [row["kind"] for row in journal.pending_commands()] == ["start"]
    admission.inventory = {
        "admission_generation": 1, "held_claims": [cycle.claim],
        "matched_claim": cycle.claim, "matched_release": None, "status": "held_exact",
    }
    return runtime, journal, admission, owner, host


def test_reset_recovers_failed_start_with_unknown_settlement(monkeypatch, tmp_path):
    runtime, journal, admission, _owner, host = _failed_host_start_runtime(
        monkeypatch, tmp_path, run_created=True,
    )
    try:
        assert _allowed(runtime)["program_reset"]["allowed"] is True
        result = runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
        assert result["idle_cycle_generation"] == 2
        assert len(host.stop_calls) == 1 and host.reset_containment_calls == [1]
        assert len(admission.releases) == 1
        assert journal.pending_commands() == ()
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()


def test_failed_start_without_a_host_run_is_reset_without_stop(monkeypatch, tmp_path):
    # The run owner never held the run: Reset settles the start as without
    # effect, releases the claim (and with it the Kernel's capture), ends Idle.
    runtime, journal, admission, owner, host = _failed_host_start_runtime(
        monkeypatch, tmp_path, run_created=False,
    )
    try:
        failed = journal.read()
        claim = failed.claim
        assert not owner.has_run(failed.program_run_id)
        assert _allowed(runtime)["program_reset"]["allowed"] is True
        result = runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.0"))
        assert result["idle_cycle_generation"] == failed.cycle_generation + 1
        # No run to stop or contain; one release of the exact cycle claim.
        assert not host.stop_calls and not host.reset_containment_calls
        assert [release["claim_generation"] for release in admission.releases] == [
            claim["claim_generation"]
        ]
        assert admission.releases[0]["settlement"]["settlement_kind"] == "explicit_reset"
        settled = journal.read()
        assert settled.state == "idle_after_reset" and not settled.reconciliation_required
        assert settled.claim is None and settled.program_run_id is None
        # The pending start row is retired with the Reset.
        assert journal.pending_commands() == ()
        # The next Execute starts normally.
        del host.start_run
        runtime.start_run(_start_request(request_id="request.start.2"))
        assert journal.read().state == "running"
    finally:
        runtime.shutdown()


def test_failed_start_without_a_host_run_resets_once_per_press(monkeypatch, tmp_path):
    runtime, journal, admission, _owner, host = _failed_host_start_runtime(
        monkeypatch, tmp_path, run_created=False,
    )
    try:
        admission.release_error = CellAdmissionError("admission_refused", "release refused")
        request = _current_reset_request(runtime, journal, "reset.0")
        with pytest.raises(ProgramRunnerError):
            runtime.reset_cycle(request)
        assert len(admission.releases) == 1 and not host.stop_calls
        # The start is settled without effect; the next press continues the Reset.
        retained = journal.read()
        assert retained.start_settlement["kind"] == "proven_no_effect"
        assert _allowed(runtime)["program_reset"]["allowed"] is True
        admission.release_error = None
        runtime.reset_cycle(request.model_copy(update={
            "request_key": _key("reset.1"),
            "terminal_reference": retained.terminal_reference,
            "settlement_evidence_digest": retained.terminal_reference,
        }))
        assert len(admission.releases) == 2 and not host.stop_calls
        assert journal.read().state == "idle_after_reset"
    finally:
        runtime.shutdown()


def _record_heap_settles(monkeypatch, runtime) -> list[tuple[Any, ...]]:
    """Each OD-234 collection point: its name, the command holding the runner,
    and the real tick host's scheduled and in-flight runs at that moment."""

    settles: list[tuple[Any, ...]] = []

    def record(point: str) -> None:
        host = runtime._host  # noqa: SLF001
        settles.append((
            point,
            runtime._command_in_progress(),  # noqa: SLF001
            dict(getattr(host, "_scheduled", {})),
            dict(getattr(host, "_inflight", {})),
            list(getattr(host, "forgotten_reset_runs", ())),
        ))

    monkeypatch.setattr("manyforge_program_runner.runtime.settle_heap", record)
    return settles


def test_load_collects_once_while_idle_and_holding_the_runner(monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )

    class IdleAdmission(_Admission):
        def get(self) -> dict[str, Any]:
            return {"admission_generation": 1, "authoring_allowed": True}

    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(),  # type: ignore[arg-type]
        gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=IdleAdmission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    monkeypatch.setattr(
        runtime._scene,  # noqa: SLF001
        "prepare_initial_scene",
        lambda _context, _scene: {"schema_version": "manyforge.prepared_program_scene.v1"},
    )
    monkeypatch.setattr(runtime._scene, "verify_initial_scene", lambda *_args: None)  # noqa: SLF001
    settles = _record_heap_settles(monkeypatch, runtime)
    runtime.start()
    try:
        result = runtime.prepare_applied_scene(
            ProgramScenePreparationRequest.model_validate({
                "request_key": _key("load.1"),
                "source_label": "manyforge_composer_activation",
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 1,
            })
        )
        assert result["status"] == "prepared"
        # Inside the Load command, with no run scheduled or in a tick.
        assert settles == [("load", "scene_preparation", {}, {}, [])]
    finally:
        runtime.shutdown()


def test_idle_reset_collects_once_holding_the_runner(monkeypatch, tmp_path) -> None:
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=_A),
        headless_client=_Headless(), gateway=_Headless(),  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / "cycle.sqlite3"),
    )
    settles = _record_heap_settles(monkeypatch, runtime)
    runtime.start()
    try:
        runtime.reset_cycle(ProgramCycleResetRequest.model_validate({
            "request_key": _key("reset.idle.heap"), "source_label": "operator_ui",
            "program_run_id": None, "expected_cycle_generation": 1,
        }))
        assert settles == [("reset", "reset", {}, {}, [])]
    finally:
        runtime.shutdown()


def test_run_reset_collects_once_after_the_run_is_forgotten(monkeypatch, tmp_path) -> None:
    runtime, journal, _admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path
    )
    host.contain_reset_run = lambda program_run_id, **_kwargs: owner.rows[program_run_id]
    settles = _record_heap_settles(monkeypatch, runtime)
    try:
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.heap"))
        # After the Reset's Stop and once the tick host let the run go.
        assert [(point, command, forgotten) for point, command, _s, _i, forgotten in settles] == [
            ("reset", "reset", [1])
        ]
    finally:
        runtime.shutdown()


class _TreeNode:
    def __init__(self) -> None:
        self.peer: _TreeNode | None = None


def test_run_start_freezes_the_built_run_before_its_first_schedule_and_reset_frees_it(
    monkeypatch, tmp_path
) -> None:
    import gc
    import weakref

    runtime, journal, _admission, owner, host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, finish_cycle=False
    )
    host.contain_reset_run = lambda program_run_id, **_kwargs: owner.rows[program_run_id]
    # A stand-in for the run's tree: a parent/child cycle built by the host's
    # start, released when the run is forgotten.
    trees: dict[int, _TreeNode] = {}
    built = host.start_run
    forget = host.forget_reset_run

    def start_run(admission, *, program_run_id=None, defer_ticks=False):
        snapshot = built(admission, program_run_id=program_run_id, defer_ticks=defer_ticks)
        root = _TreeNode()
        root.peer = _TreeNode()
        root.peer.peer = root
        trees[snapshot.capture.program_run_id] = root
        return snapshot

    def forget_reset_run(program_run_id):
        forget(program_run_id)
        trees.pop(program_run_id, None)

    def frozen(item: object) -> bool:
        return all(other is not item for other in gc.get_objects())

    frozen_at_schedule: list[bool] = []
    scheduled = host.schedule
    host.start_run = start_run
    host.forget_reset_run = forget_reset_run
    host.schedule = lambda run_id: (
        frozen_at_schedule.append(frozen(trees[run_id])) or scheduled(run_id)
    )
    full_collections: list[str] = []

    def observe(phase: str, info: dict[str, Any]) -> None:
        if info["generation"] == 2:
            full_collections.append(phase)

    gc.callbacks.append(observe)
    try:
        before = gc.get_freeze_count()
        runtime.start_run(_start_request())
        tree = weakref.ref(trees[1])

        assert gc.get_freeze_count() > before
        # Set aside before the run's first schedule, without any collection.
        assert frozen_at_schedule == [True]
        assert full_collections == []

        runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
        runtime.reset_cycle(_current_reset_request(runtime, journal, "reset.frozen"))
        # Reset's unfreeze and full collection free the run's cycle.
        assert tree() is None
    finally:
        gc.callbacks.remove(observe)
        gc.unfreeze()
        runtime.shutdown()


# OD-214: commands answer at acceptance and publish their step and end.


def _join_command_threads() -> None:
    import threading

    for thread in threading.enumerate():
        if thread.name == "manyforge_program_runner_command":
            thread.join(5)
            assert not thread.is_alive()


def _value_write(request_id: str, **changes: Any) -> RuntimeBlackboardWriteRequest:
    return RuntimeBlackboardWriteRequest.model_validate({
        "request_key": _key(request_id), "source_label": "operator_ui",
        "program_run_id": 1, "expected_run_revision": 1,
        "key": "operator.confirmed", "value": True, **changes,
    })


def test_a_command_is_answered_at_acceptance_and_publishes_its_end(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, owner = _running_control_runtime(monkeypatch, tmp_path)
    # Execute's own row: accepted, through its stages, succeeded.
    assert _last_row(runtime, "start", "succeeded", None) == {
        "step": "starting_run", "accepted": True, "details": None}
    entered, release = Event(), Event()
    write = owner.write_runtime_blackboard

    def held_write(program_run_id, **values):
        entered.set()
        assert release.wait(5), "test did not release the held write"
        return write(program_run_id, **values)

    owner.write_runtime_blackboard = held_write  # type: ignore[method-assign]
    request = _value_write("accept.write.1")
    try:
        reply = runtime.run_command(
            request.request_key, lambda: runtime.write_runtime_blackboard(1, request)
        )
        # The reply came while the effect is still held.
        assert entered.wait(2)
        assert reply["schema_version"] == "manyforge.program_command_reply.v1"
        row = reply["command"]
        assert (row["kind"], row["request_key"], row["step"], row["end"]) == (
            "runtime_blackboard_write", request.request_key, "applying", None
        )
        assert row["accepted_at_ns"].isdigit()
        status = runtime.cycle_status()["runner_commands"]
        assert status["in_progress"] == row
        # A Busy refusal is answered at once and is not a row (OP-1 (b)).
        other = _value_write("accept.write.2")
        with pytest.raises(ProgramRunnerError) as busy:
            runtime.run_command(
                other.request_key, lambda: runtime.write_runtime_blackboard(1, other)
            )
        assert _busy(busy.value, "runtime_blackboard_write")
        # The same key while it runs: its row, nothing run again.
        assert runtime.run_command(
            request.request_key, lambda: pytest.fail("a replay ran its command")
        )["command"] == row
    finally:
        release.set()
        _join_command_threads()
    ended = runtime.cycle_status()["runner_commands"]
    assert ended["in_progress"] is None
    assert [r["request_key"] for r in ended["ended"]][:1] == [request.request_key]
    assert _last_row(runtime, "runtime_blackboard_write", "succeeded", None) == {
        "step": "applying", "accepted": True, "details": None}
    assert all(r["request_key"] != _key("accept.write.2") for r in ended["ended"])
    # A replay after the end returns the ended row; the write ran once.
    replay = runtime.run_command(
        request.request_key, lambda: pytest.fail("a replay ran its command")
    )
    assert replay["command"] == ended["ended"][0]
    assert [effect for effect, _values in owner.effects] == ["write"]
    runtime.shutdown()


def test_a_refusal_before_acceptance_is_a_row_and_its_replay_is_refused_again(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, owner = _running_control_runtime(monkeypatch, tmp_path)
    stale = _value_write("refuse.write.1", expected_run_revision=9)
    for _ in range(2):
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.run_command(
                stale.request_key, lambda: runtime.write_runtime_blackboard(1, stale)
            )
        assert refused.value.code == "program_runtime_control_stale"
        assert refused.value.details == {
            "expected_run_revision": 9, "current_run_revision": 1}
    rows = runtime.cycle_status()["runner_commands"]["ended"]
    # One row: the replay refused again without running.
    assert [r["request_key"] for r in rows].count(stale.request_key) == 1
    assert _last_row(runtime, "runtime_blackboard_write", "refused",
                     "program_runtime_control_stale") == {
        "step": None, "accepted": False,
        "details": {"expected_run_revision": 9, "current_run_revision": 1}}
    # Scene preparation needs an idle cycle: refused at once, kept as a row.
    with pytest.raises(ProgramRunnerError) as scene:
        runtime.prepare_applied_scene(ProgramScenePreparationRequest.model_validate({
            "source_label": "headless_apply_completion", "request_key": _key("refuse.scene"),
            "expected_applied_record_digest": _DIGEST_A, "expected_admission_generation": 1,
        }))
    assert scene.value.code == "authoring_not_ready"
    assert _last_row(runtime, "scene_preparation", "refused", "authoring_not_ready") == {
        "step": None, "accepted": False, "details": None}
    assert owner.effects == []
    runtime.shutdown()


def test_an_accepted_command_ends_refused_or_unknown_as_the_journal_records_it(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    runtime, owner = _running_control_runtime(monkeypatch, tmp_path)

    def refusing_write(_program_run_id, **_values):
        raise ProgramRunnerError("program_runtime_control_stale", "scripted refusal")

    owner.write_runtime_blackboard = refusing_write  # type: ignore[method-assign]
    refused = _value_write("end.write.refused")
    reply = runtime.run_command(
        refused.request_key, lambda: runtime.write_runtime_blackboard(1, refused)
    )
    assert reply["command"]["accepted_at_ns"] is not None
    _join_command_threads()
    # Proven no effect: the journal forgot the request; the row says refused.
    assert _last_row(runtime, "runtime_blackboard_write", "refused",
                     "program_runtime_control_stale")["accepted"] is True
    assert runtime._journal.pending_commands() == ()  # noqa: SLF001

    def lost_write(_program_run_id, **_values):
        raise ProgramRunOwnerOutcomeUnknownError("scripted lost answer")

    owner.write_runtime_blackboard = lost_write  # type: ignore[method-assign]
    unknown = _value_write("end.write.unknown")
    runtime.run_command(
        unknown.request_key, lambda: runtime.write_runtime_blackboard(1, unknown)
    )
    _join_command_threads()
    assert _last_row(runtime, "runtime_blackboard_write", "unknown",
                     "program_cycle_reconciliation_required")["step"] == "applying"
    assert runtime.cycle_status()["runner_commands"]["in_progress"] is None
    runtime.shutdown()


def test_the_end_of_a_row_follows_the_codes_of_the_contract() -> None:
    from manyforge_program_runner.runtime import _CommandRow, _command_end

    def end(error, *, accepted=True, rejected=False):
        row = _CommandRow("reset", _key("end.rule"), 1, None,
                          accepted_at_ns="1" if accepted else None, rejected=rejected)
        result = _command_end(row, error)
        assert result.pop("finished_at_ns").isdigit()
        return result

    def error(code, **details):
        return ProgramRunnerError(code, "scripted", details=details)

    assert end(None) == {"outcome": "succeeded", "code": None, "details": None}
    assert end(error("program_cycle_recovery_required",
                     cause_code="program_cycle_reset_failed")) == {
        "outcome": "failed", "code": "program_cycle_recovery_required",
        "details": {"cause_code": "program_cycle_reset_failed"}}
    assert end(error("program_cycle_recovery_required",
                     cause_code="program_cycle_reconciliation_required"),
               rejected=True)["outcome"] == "unknown"
    assert end(error("program_cycle_reconciliation_required"))["outcome"] == "unknown"
    assert end(error("program_run_started_then_failed"))["outcome"] == "failed"
    assert end(error("rule_holds"), rejected=True)["outcome"] == "refused"
    assert end(error("program_motion_resume_incomplete", cleanup_accepted=True),
               rejected=True) == {
        "outcome": "refused", "code": "program_motion_resume_incomplete",
        "details": {"cleanup_accepted": True}}
    assert end(error("cycle_recovery_offer_stale"), accepted=False)["outcome"] == "refused"
    # Before acceptance every typed refusal is `refused`, whatever its code.
    assert end(error("program_cycle_reconciliation_required"),
               accepted=False)["outcome"] == "refused"
    # The current recovery offer is in the status, not copied into the row.
    assert end(error("cycle_recovery_offer_stale", recovery_offer={"offer_id": 1}),
               rejected=True)["details"] is None
    assert end(RuntimeError("boom")) == {
        "outcome": "unknown", "code": "program_runtime_control_outcome_unknown",
        "details": {"command_kind": "reset", "cause_code": "RuntimeError"}}


@pytest.mark.parametrize(
    ("state", "run_end", "error", "facts"),
    [
        ("succeeded", {"end_reason": "completed", "details": {}}, None,
         {"run_revision": 2, "end_reason": "completed"}),
        ("failed", {"end_reason": "failed", "details": {
            "error": {"code": "motion_plan_failed", "message": "No path"},
            "node_instance_id": "pick_1"}},
         {"code": "motion_plan_failed", "message": "No path", "details": None},
         {"run_revision": 2, "end_reason": "failed", "node_instance_id": "pick_1"}),
        ("stopped", {"end_reason": "stopped", "details": {
            "stopped_by": "rule", "rule_id": "door_open"}}, None,
         {"run_revision": 2, "end_reason": "stopped", "stopped_by": "rule",
          "rule_id": "door_open"}),
        # The internal Forced Program End (no `run_end` from the fake host).
        ("stopped", None, None, {"run_revision": 2}),
    ],
)
def test_each_run_has_one_start_and_one_end_event_for_history(
    monkeypatch: pytest.MonkeyPatch, tmp_path, state, run_end, error, facts
) -> None:
    """OD-381 P2: Execute records the run's start; every end state records
    one end, stamped when the Runner observes it; a repeated terminal
    observation records nothing more."""

    runtime, _journal, _admission, _owner, _host = _terminal_cycle_reset_runtime(
        monkeypatch, tmp_path, finish_cycle=False
    )
    events: list[dict[str, Any]] = []
    runtime._node_attempts = SimpleNamespace(  # noqa: SLF001
        submit=lambda _facts: None, submit_run_event=events.append,
        close=lambda: None, health=lambda: {},
    )
    try:
        before_ns = time.monotonic_ns()
        runtime.start_run(_start_request())
        # The run's audit events go to History too (P3).
        runtime._controls(1).audit_value(  # noqa: SLF001
            "program_node_preempted", node_instance_id="pick_1", node_attempt_id=1)
        if run_end is None:
            runtime._stop_for_bbkey_publication_fault(1)  # noqa: SLF001
        else:
            terminal = _Snapshot(1, owner_id=runtime.owner_id, state=state, revision=2)
            terminal._value.update(  # noqa: SLF001
                run_end=run_end,
                reason_code=None if error is None else error["code"],
                message="" if error is None else error["message"],
            )
            runtime._record_terminal_snapshot(terminal.to_dict())  # noqa: SLF001
            runtime._record_terminal_snapshot(terminal.to_dict())  # noqa: SLF001
        after_ns = time.monotonic_ns()
    finally:
        runtime.shutdown()
    assert [event["event"] for event in events] == [
        "program_run_started", "program_node_preempted", "program_run_finished"]
    started, audit, finished = events
    assert (audit["program_run_id"], audit["outcome"], audit["facts"]) == (
        1, "recorded", {"event_revision": 1, "node_instance_id": "pick_1", "node_attempt_id": 1})
    assert started["program_run_id"] == 1 and started["outcome"] == "started"
    assert started["error"] is None
    assert started["facts"] == {
        "scenario_id": "scenario.1", "program_revision": _DIGEST_A,
        "applied_record_digest": _DIGEST_B, "program_binding_id": _DIGEST_B,
        "request_key": _key("request.start.1"), "source_label": "operator_ui",
    }
    assert (finished["program_run_id"], finished["outcome"]) == (1, state)
    assert finished["error"] == error
    assert finished["facts"] == facts
    for event in events:
        assert before_ns <= int(event["occurred_at_ns"]) <= after_ns
        assert event["occurred_at_wall"].endswith("+00:00")
