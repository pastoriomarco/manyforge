"""Captured Program operation ports remain thin, exact native clients."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from threading import Event, Thread
from types import SimpleNamespace
from typing import Any, Mapping

import py_trees
import pytest

from manyforge_behavior.assistant_program_port import (
    HeadlessProgramSceneSource,
    ProgramAssistantPort,
)
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.orchestration_ports import OwnerTaskState
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAttemptIdentity,
    OwnerInvocationHeldError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.production_ports import (
    HeadlessSceneProgramPort,
    NativeKernelProgramPort,
    ProductionPortError,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_DIGEST_D = "sha256:" + "d" * 64
_DIGEST_E = "sha256:" + "e" * 64
_DIGEST_F = "sha256:" + "f" * 64


class _Transport:
    def __init__(self) -> None:
        self.submissions: list[Mapping[str, Any]] = []
        self.controls: list[tuple[str, str, Mapping[str, Any]]] = []
        self.operations: dict[str, dict[str, Any]] = {}
        self.resident: dict[str, bool] = {}
        self.operation_reads = 0
        self.stop_status_reads = 0
        self.stop_states: dict[str, str] = {}
        self.stop_generations: dict[str, int] = {}
        self.released_scope_stops: list[tuple[str, str, int]] = []
        self.identity_reads = 0
        self.owner_epoch = "kernel.epoch.7"

    def identity(self) -> dict[str, Any]:
        self.identity_reads += 1
        return {
            "ok": True,
            "identity": {
                "owner_epoch": "kernel.epoch.7",
                # The native owner may expose a direct-control status Program;
                # production Program requests must use their captured context.
                "program_id": "manyforge.direct_control.motion",
                "program_revision": _DIGEST_F,
                "program_binding_id": _DIGEST_F,
                "graph_revision": _DIGEST_C,
                "retained_bundle_id": _DIGEST_B,
                "scene_revision": {
                    "scene_id": _DIGEST_D,
                    "revision_id": 4,
                    "content_hash": _DIGEST_E,
                },
            },
        }

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submissions.append(deepcopy(body))
        operation_id = f"operation-{len(self.submissions)}"
        self.operations[operation_id] = {
            "operation_id": operation_id,
            "operation_revision": 1,
            "program_run_id": body["program_run_id"],
            "state": "queued",
            "active_stop_generation": None,
            "participants": [],
        }
        self.resident[operation_id] = True
        return {
            "ok": True,
            "accepted": {
                "operation_id": operation_id,
                "operation_revision": 1,
            },
        }

    def read(self, operation_id: str) -> dict[str, Any]:
        self.operation_reads += 1
        return {
            "ok": True,
            "operation": deepcopy(self.operations[operation_id]),
            "resident": self.resident.get(operation_id, True),
        }

    def _control(self, kind: str, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        self.controls.append((kind, operation_id, deepcopy(body)))
        return {
            "ok": True,
            "accepted": {
                "operation_id": operation_id,
                "operation_revision_after_acceptance": (
                    body.get("expected_operation_revision", 0) + 1
                ),
            },
        }

    def pause(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("pause", operation_id, body)

    def resume(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("resume", operation_id, body)

    def reset(self, operation_id: str, body: Mapping[str, Any]):
        result = self._control("reset", operation_id, body)
        self.resident[operation_id] = False
        return result

    def acknowledge_completed(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("acknowledge", operation_id, body)

    def stop(self, body: Mapping[str, Any]):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)

    def stop_program_run(self, body: Mapping[str, Any]):
        self.controls.append(("stop_program_run", body["program_run_id"], deepcopy(body)))
        stop_id = f"scope-stop-{len(self.stop_states) + 1}"
        self.stop_states[stop_id] = "stopping"
        self.stop_generations[stop_id] = len(self.stop_generations) + 1
        return {
            "ok": True,
            "accepted": {
                "request_id": body["request_context"]["request_id"],
                "stop_id": stop_id,
                "stop_generation": self.stop_generations[stop_id],
                "replay_status": "accepted_new",
            },
            "owner_epoch": self.owner_epoch,
        }

    def release_program_run_stop_fence(
        self, program_run_id: str, stop_id: str, stop_generation: int
    ) -> dict[str, Any]:
        assert self.stop_states[stop_id] == "succeeded"
        assert self.stop_generations[stop_id] == stop_generation
        self.released_scope_stops.append((program_run_id, stop_id, stop_generation))
        return {"ok": True}

    def stop_status(self, stop_id: str):
        self.stop_status_reads += 1
        return {
            "ok": True,
            "stop_status": {
                "stop_id": stop_id,
                "state": self.stop_states[stop_id],
                "stop_generation": self.stop_generations[stop_id],
            },
        }


class _LostAcceptanceReplyTransport(_Transport):
    def __init__(self) -> None:
        super().__init__()
        self.submit_attempts: list[Mapping[str, Any]] = []
        self._retained_request: Mapping[str, Any] | None = None
        self._retained_response: dict[str, Any] | None = None

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submit_attempts.append(deepcopy(body))
        if self._retained_response is not None:
            assert body == self._retained_request
            response = deepcopy(self._retained_response)
            response["accepted"]["replay_status"] = "replayed_exact"
            return response
        self._retained_request = deepcopy(body)
        self._retained_response = super().submit(body)
        raise TimeoutError("acceptance reply lost")


class _ControlTransport(_Transport):
    def __init__(self):
        super().__init__()
        self.queries = []
        self.owner_epoch = "kernel.epoch.7"
        self.last_control = None
        self.control_pending = False
        self.operations["controlled"] = {
            "operation_id": "controlled",
            "operation_revision": 7,
            "program_run_id": "program-run.1",
            "program_binding_id": _DIGEST_A,
            "state": "executing",
            "active_stop_generation": None,
            "runtime_scale_generation": 0,
            "participants": [{"command_target_id": "robot.arm"}],
        }
        self.scale = {"generation": 0, "state": "pending"}

    def _control(self, kind, operation_id, body):
        response = super()._control(kind, operation_id, body)
        self.last_control = response["accepted"] = {
            **response["accepted"],
            "request_id": body["request_context"]["request_id"],
            "control": kind,
        }
        self.operations[operation_id]["operation_revision"] = self.last_control[
            "operation_revision_after_acceptance"
        ]
        return response

    def read(self, operation_id):
        return {
            **super().read(operation_id),
            "owner_epoch": self.owner_epoch,
            "last_control": deepcopy(self.last_control),
            "control_pending": self.control_pending,
        }

    def query_operation(self, body):
        self.queries.append(deepcopy(body))
        return {
            "ok": True,
            "owner_epoch": "kernel.epoch.7",
            "target_role_id": "arm",
            "operation": deepcopy(self.operations["controlled"]),
            "runtime_scale": deepcopy(self.scale),
        }

    def control_state(self):
        return {
            **self.identity(),
            "control_state": {"cell_stop_action": {"expected_stop_generation": 4}},
        }

    def stop(self, body):
        self.controls.append(("stop", "controlled", deepcopy(body)))
        self.stop_states["control-stop"] = "stopping"
        self.stop_generations["control-stop"] = 5
        return {
            "ok": True,
            "accepted": {"stop_id": "control-stop", "stop_generation": 5},
        }

    def full_stop(self, body):
        result = self.stop(body)
        self.controls[-1] = ("full_stop", "cell", deepcopy(body))
        return result

    def set_operation_runtime_scale(self, body):
        self.controls.append(("scale", "controlled", deepcopy(body)))
        self.scale["generation"] = body["expected_runtime_scale_generation"] + 1
        self.operations["controlled"]["runtime_scale_generation"] = self.scale["generation"]
        return {
            "ok": True,
            "accepted": {"runtime_scale_generation": self.scale["generation"]},
        }


def _control_port():
    transport = _ControlTransport()
    owner = NativeKernelProgramPort(KernelClient(transport))
    context = _context()
    binding = {
        **context.program_binding,
        "target_role_bindings": [
            {
                **row,
                "capability_bindings": [
                    *row["capability_bindings"],
                    *(
                        {"capability_id": cap, "command_schema": None}
                        for cap in (
                            "execution.pause",
                            "execution.resume",
                            "execution.reset",
                            "execution.stop.soft",
                            "execution.runtime_scale.operation_scoped",
                        )
                    ),
                ],
            }
            for row in context.program_binding["target_role_bindings"]
        ],
    }
    owner.bind(replace(context, program_binding=binding))
    return owner, transport


@pytest.mark.parametrize("accepted", [True, False, "stale_after_capture"])
@pytest.mark.parametrize("independent_stop", [None, "before_assessment", "after_effect"])
def test_recovery_node_submits_only_the_authored_current_offer_once(accepted, independent_stop):
    owner, transport = _control_port()
    transport.operations["controlled"]["state"] = "stopped"
    operation = transport.operations["controlled"]
    assessment = {
        "assessment_revision": 7,
        "control_command_epoch": "epoch.8",
        "execution_id": "execution.9",
        "latch_id": "latch.1",
        "stop_certificate_id": "certificate.1",
        "stop_owner_generation": 2,
        "allowed_actions": ["acknowledge_and_continue", "replan_from_stopped_state"],
    }

    def stop_before_recovery_returns():
        owner.scope.request_stop("program-run.1", "independent-stop.1", "new operator Stop")
        transport.stop_states["scope-stop-1"] = "succeeded"

        def unavailable(_stop_id):
            raise RuntimeError("Stop observation is unavailable")

        transport.stop_status = unavailable

    def current_assessment(_operation_id):
        if independent_stop == "before_assessment":
            stop_before_recovery_returns()
        return {
            "ok": True,
            "operation_revision": operation["operation_revision"],
            "participants": [{"operation_participant_id": "participant.1"}],
            "recovery_assessment": assessment,
        }

    transport.operation_recovery = current_assessment
    calls = []

    def recover(operation_id, revision, request):
        calls.append((operation_id, revision, dict(request)))
        if independent_stop == "after_effect":
            stop_before_recovery_returns()
        if accepted == "stale_after_capture":

            class StaleRecovery(RuntimeError):
                code = "operation_recovery_stale"

            raise StaleRecovery("the current offer changed before acceptance")
        return {
            "ok": True,
            "result": {
                "request_id": request["request_id"],
                "action": request["action"],
                "accepted": accepted,
                "resulting_assessment": assessment,
            },
        }

    transport.apply_operation_recovery = recover
    identity = _identity("recovery")
    result = owner.controls.start(
        "recovery.apply_offered_action",
        identity,
        OwnerNodeInputs({"target_role": "arm", "recovery_action": "acknowledge_and_continue"}),
    )
    assert result.state is (OwnerTaskState.SUCCEEDED if accepted is True else OwnerTaskState.FAILED)
    assert not owner.controls._pending
    assert len(calls) == 1
    operation_id, revision, request = calls[0]
    assert (operation_id, revision) == ("controlled", operation["operation_revision"])
    assert request == {
        "request_id": request["request_id"],
        "action": "acknowledge_and_continue",
        "principal_id": "program_runner",
        "expected_assessment_revision": 7,
        "expected_control_command_epoch": "epoch.8",
        "expected_execution_id": "execution.9",
        "expected_latch_id": "latch.1",
        "expected_stop_certificate_id": "certificate.1",
        "expected_stop_owner_generation": 2,
        "reason": "Program recovery node",
    }
    owner.controls.acknowledge(result.task_id, identity)
    assert len(calls) == 1
    assert not transport.released_scope_stops
    if independent_stop is not None:
        with pytest.raises(ProductionPortError, match="scope is stopped"):
            owner.operations.start(
                "motion.move_to_pose", _identity("fresh-motion"), _motion_inputs()
            )


def test_motion_resume_cannot_release_another_stop_occurrence():
    owner, transport = _control_port()
    owner.scope.request_stop("program-run.1", "new-stop", "independent Stop")
    transport.stop_states["scope-stop-1"] = "succeeded"
    with pytest.raises(ProductionPortError, match="handoff is stale"):
        owner.scope.release_stop("program-run.1", expected_stop=("previous-stop", 1))
    assert not transport.released_scope_stops
    with pytest.raises(ProductionPortError, match="scope is stopped"):
        owner.operations.start("motion.move_to_pose", _identity("fresh-motion"), _motion_inputs())


@pytest.mark.parametrize(
    "failure",
    ["stale", "active", "unoffered", "reset", "missing_action", "foreign_run"],
)
def test_recovery_node_rejects_ambiguous_stale_or_unoffered_work_before_effect(failure):
    owner, transport = _control_port()
    operation = transport.operations["controlled"]
    operation["state"] = "executing" if failure == "active" else "stopped"
    if failure == "foreign_run":
        operation["program_run_id"] = "foreign.run"
    assessment = {"allowed_actions": ["acknowledge_and_continue", "reset_cycle"]}
    transport.operation_recovery = lambda operation_id: {
        "ok": True,
        "operation_revision": operation["operation_revision"] + (failure == "stale"),
        "participants": [{"operation_participant_id": "participant.1"}],
        "recovery_assessment": assessment,
    }
    calls = []
    transport.apply_operation_recovery = lambda *args: calls.append(args)
    action = {
        "reset": "reset_cycle",
        "unoffered": "replan_from_stopped_state",
        "missing_action": None,
    }.get(failure, "acknowledge_and_continue")
    with pytest.raises(ProductionPortError):
        owner.controls.start(
            "recovery.apply_offered_action",
            _identity("recovery"),
            OwnerNodeInputs({"target_role": "arm", "recovery_action": action}),
        )
    assert not calls
    assert not owner.controls._pending


@pytest.mark.parametrize(
    "node,state",
    [
        ("kernel.pause_operation", "paused"),
        ("kernel.pause_operation", "completed"),
        ("kernel.resume_operation", "executing"),
        ("kernel.reset_operation", "stopped"),
    ],
)
def test_control_node_submits_once_then_observes_owner_outcome(node, state):
    owner, transport = _control_port()
    identity = NodeAttemptIdentity("program-run.1", "control", "attempt.1")
    transport.operations["controlled"]["state"] = "paused" if "resume" in node else "executing"
    task = owner.controls.start(node, identity, OwnerNodeInputs({"target_role": "arm"}))
    assert task.state is OwnerTaskState.ACTIVE
    assert len(transport.controls) == len(transport.queries) == 1
    request = transport.controls[0][2]
    assert request["expected_operation_revision"] == 7
    assert request["execution_scope"] == {
        "node_instance_id": "control",
        "node_attempt_id": "attempt.1",
    }
    assert request["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
    transport.operations["controlled"]["state"] = state
    result = owner.controls.observe(task.task_id)
    assert result.state is OwnerTaskState.SUCCEEDED
    owner.controls.acknowledge(task.task_id, identity)
    assert len(transport.controls) == 1  # consumption does not Reset/ACK the robot
    assert owner.controls._pending == {}


@pytest.mark.parametrize("full", [False, True])
@pytest.mark.parametrize("terminal,succeeded", [("succeeded", True), ("partial", False)])
def test_stop_control_node_waits_for_exact_stop_certificate(full, terminal, succeeded):
    owner, transport = _control_port()
    identity = NodeAttemptIdentity("program-run.1", "control", "attempt.stop")
    task = owner.controls.start(
        "kernel.full_stop" if full else "kernel.stop_operation",
        identity,
        OwnerNodeInputs({} if full else {"target_role": "arm"}),
    )
    for _ in range(3):
        assert owner.controls.observe(task.task_id).state is OwnerTaskState.ACTIVE
    assert len(transport.controls) == 1
    if full:
        assert transport.controls[0][2]["expected_stop_generation"] == 0
        assert transport.queries == []
    else:
        assert transport.controls[0][2]["operations"] == [
            {
                "operation_id": "controlled",
                "expected_operation_revision": 7,
                "expected_stop_generation": 0,
            }
        ]
    transport.stop_states["control-stop"] = terminal
    result = owner.controls.observe(task.task_id)
    assert result.state is (OwnerTaskState.SUCCEEDED if succeeded else OwnerTaskState.FAILED)
    assert owner.controls._pending == {}


@pytest.mark.parametrize(
    "node,observed",
    [
        ("kernel.pause_operation", "paused"),
        ("kernel.pause_operation", "completed"),
        ("kernel.resume_operation", "executing"),
        ("kernel.reset_operation", "intervention_required"),
    ],
)
@pytest.mark.parametrize("succeeds", [True, False])
def test_control_node_waits_for_provider_completion_not_just_observed_state(
    node, observed, succeeds
):
    owner, transport = _control_port()
    task = owner.controls.start(
        node,
        NodeAttemptIdentity("program-run.1", "control", "held"),
        OwnerNodeInputs({"target_role": "arm"}),
    )
    transport.control_pending = True
    transport.resident["controlled"] = True
    transport.operations["controlled"]["state"] = observed
    assert owner.controls.observe(task.task_id).state is OwnerTaskState.ACTIVE
    transport.control_pending = False
    if succeeds and node == "kernel.reset_operation":
        transport.resident["controlled"] = False
    elif not succeeds:
        transport.operations["controlled"]["state"] = "intervention_required"
    assert owner.controls.observe(task.task_id).state is (
        OwnerTaskState.SUCCEEDED if succeeds else OwnerTaskState.FAILED
    )
    assert len(transport.controls) == 1
    assert owner.controls._pending == {}


def test_control_node_cannot_treat_replacement_owner_history_as_completed_reset():
    owner, transport = _control_port()
    task = owner.controls.start(
        "kernel.reset_operation",
        NodeAttemptIdentity("program-run.1", "control", "replaced"),
        OwnerNodeInputs({"target_role": "arm"}),
    )
    transport.owner_epoch = "replacement"
    transport.resident["controlled"] = False
    with pytest.raises(ProductionPortError, match="changed owner"):
        owner.controls.observe(task.task_id)
    assert len(transport.controls) == 1
    assert owner.controls._pending == {}


def test_control_node_cannot_claim_a_later_controls_completion():
    owner, transport = _control_port()
    task = owner.controls.start(
        "kernel.reset_operation",
        NodeAttemptIdentity("program-run.1", "control", "superseded"),
        OwnerNodeInputs({"target_role": "arm"}),
    )
    transport.last_control["request_id"] = "another-reset"
    assert owner.controls.observe(task.task_id).reason_code == "operation_control_superseded"


def test_full_stop_never_needs_ordinary_read_or_fresh_owner(monkeypatch):
    owner, transport = _control_port()
    transport.owner_epoch = "replacement"

    def unavailable(*_args):
        raise AssertionError("Full Stop must not use ordinary reads")

    monkeypatch.setattr(transport, "control_state", unavailable)
    monkeypatch.setattr(transport, "identity", unavailable)
    monkeypatch.setattr(transport, "query_operation", unavailable)
    owner.controls.start(
        "kernel.full_stop",
        NodeAttemptIdentity("program-run.1", "control", "full-stop"),
        OwnerNodeInputs({}),
    )
    assert len(transport.controls) == 1
    assert transport.controls[0][0] == "full_stop"
    assert transport.controls[0][2]["request_context"]["request_owner_epoch"] == "kernel.epoch.7"


def test_operation_scale_node_waits_for_application_without_changing_scope_setting():
    owner, transport = _control_port()
    identity = NodeAttemptIdentity("program-run.1", "control", "attempt.scale")
    task = owner.controls.start(
        "kernel.set_runtime_scale",
        identity,
        OwnerNodeInputs({"target_role": "arm", "scale": 0.5}),
    )
    assert owner.controls.observe(task.task_id).state is OwnerTaskState.ACTIVE
    transport.scale["state"] = "applied"
    assert owner.controls.observe(task.task_id).state is OwnerTaskState.SUCCEEDED
    assert [kind for kind, _, _ in transport.controls] == ["scale"]
    request = transport.controls[0][2]
    assert request["operation_id"] == "controlled"
    assert request["expected_runtime_scale_generation"] == 0
    assert "selector" not in request  # no persistent operator-setting mutation


@pytest.mark.parametrize(
    "field,value", [("program_run_id", "other"), ("program_binding_id", _DIGEST_B)]
)
def test_control_query_cannot_cross_program_scope(field, value):
    owner, transport = _control_port()
    transport.operations["controlled"][field] = value
    with pytest.raises(ProductionPortError, match="crossed"):
        owner.controls.start(
            "kernel.pause_operation",
            NodeAttemptIdentity("program-run.1", "control", "attempt.bad"),
            OwnerNodeInputs({"target_role": "arm"}),
        )
    assert transport.controls == []
    assert owner.controls._pending == {}


def test_control_task_retention_is_pending_only_and_cancel_never_reissues_effect(
    monkeypatch,
):
    import manyforge_behavior.production_ports as ports

    owner, transport = _control_port()
    monkeypatch.setattr(ports, "_MAX_TASKS", 2)
    for index in range(1000):
        identity = NodeAttemptIdentity("program-run.1", "control", f"attempt.{index}")
        task = owner.controls.start(
            "kernel.pause_operation", identity, OwnerNodeInputs({"target_role": "arm"})
        )
        transport.operations["controlled"]["state"] = "paused"
        assert owner.controls.observe(task.task_id).state is OwnerTaskState.SUCCEEDED
        owner.controls.acknowledge(task.task_id, identity)
    identity = NodeAttemptIdentity("program-run.1", "control", "attempt.cancel")
    task = owner.controls.start(
        "kernel.stop_operation", identity, OwnerNodeInputs({"target_role": "arm"})
    )
    owner.controls.cancel(task.task_id, identity)
    assert len(transport.controls) == 1001
    assert owner.controls._pending == {}


def _context() -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="program-run.1",
        applied_record_digest="sha256:" + "9" * 64,
        scenario_id="lite6_pick_and_place",
        source_document_digest=_DIGEST_A,
        program_id="lite6_program",
        program_revision=_DIGEST_A,
        program_binding_id=_DIGEST_A,
        retained_bundle_id=_DIGEST_B,
        graph_revision=_DIGEST_C,
        node_environment_digest=_DIGEST_D,
        program_binding={
            "program_binding_id": _DIGEST_A,
            "program_id": "lite6_program",
            "program_revision": _DIGEST_A,
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "target_role_bindings": [
                {
                    "target_role_id": "arm",
                    "component_role_id": "robot",
                    "capability_bindings": [
                        {
                            "capability_id": "execution.trajectory.execute",
                            "command_schema": None,
                        },
                        {
                            "capability_id": "model.kinematics.fk",
                            "command_schema": None,
                        },
                        {
                            "capability_id": "state.observed.current",
                            "command_schema": None,
                        },
                    ],
                },
                {
                    "target_role_id": "gripper",
                    "component_role_id": "tool",
                    "capability_bindings": [
                        {
                            "capability_id": "tool.gripper.command",
                            "command_schema": {
                                "command_schema_id": "gripper.command_position",
                                "command_schema_revision": _DIGEST_E,
                            },
                        },
                        {
                            "capability_id": "tool.gripper.feedback",
                            "command_schema": None,
                        },
                        {
                            "capability_id": "tool.gripper.confirmation.operator",
                            "command_schema": None,
                        },
                    ],
                },
            ],
            "contact_grasp_bindings": [
                {
                    "contact_policy_id": "pick.contact",
                    "runtime_object_id": "selected_object",
                }
            ],
        },
    )


def _deferred_policy_context() -> ProgramRunPortContext:
    return replace(
        _context(),
        program={
            "program_id": "lite6_program",
            "program_revision": _DIGEST_A,
            "execution_groups": {
                "robot_group": {"execution_policy": "common_start"}
            },
            "tree": {
                "node_id": "parallel",
                "instance_id": "paired_motion",
                "parameters": {"success_policy": "all"},
                "children": [
                    {
                        "node_id": "motion.move_to_pose",
                        "instance_id": instance,
                        "execution_groups": ["robot_group"],
                        "parameters": {},
                        "children": [],
                    }
                    for instance in ("arm_a", "arm_b")
                ],
            },
        },
    )


def _identity(instance: str = "move-relative") -> NodeAttemptIdentity:
    return NodeAttemptIdentity(
        program_run_id="program-run.1",
        node_instance_id=instance,
        node_attempt_id=f"attempt:{instance}",
    )


def _motion_inputs() -> OwnerNodeInputs:
    return OwnerNodeInputs(
        parameters={
            "target_role": "arm",
            "base_frame_role": "base",
            "tcp_frame_role": "tcp",
            "goal": {
                "position_m": [0.0, 0.0, 0.1],
                "orientation": {"rpy_deg": [180.0, 0.0, 0.0]},
            },
            "target_mode": "relative",
            "target_reference": {
                "type": "scene_resource",
                "scene_resource_id": "graspable",
            },
            "offset_reference": {
                "type": "frame_role",
                "frame_role_id": "base",
            },
            "planning_profile": "balanced",
            "velocity_scale": 0.4,
            "acceleration_scale": 0.4,
            "max_tcp_linear_speed_mps": None,
            "per_joint_dynamics_limits": {},
            "joint_position_tol_rad": 0.05,
            "translation_tol_m": 0.005,
            "rotation_tol_rad": 0.05,
            "planning_timeout_s": 2.0,
            "execution_timeout_s": 10.0,
            "cache_mode": "default",
            "contact_policy_id": None,
        }
    )


def test_deferred_execution_policy_is_rejected_before_binding_effects() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))

    with pytest.raises(ProductionPortError, match="unavailable until F2"):
        port.bind(_deferred_policy_context())

    assert transport.submissions == []


@pytest.mark.parametrize(
    "node_id,expected_state,owner_state,succeeded",
    [
        ("model.get_frame_pose", None, None, True),
        ("gripper.check_status", "moving", "executing", True),
        ("gripper.check_status", "reached", "completed", True),
        ("gripper.check_status", "failed", "failed", True),
        ("gripper.check_status", "idle", "idle", True),
        ("gripper.check_status", "reached", "executing", False),
        ("gripper.check_status", "grasp_confirmed", "completed", False),
    ],
)
def test_native_queries_use_one_captured_read_without_effects(
    node_id, expected_state, owner_state, succeeded
):
    class QueryTransport(_Transport):
        def query_target(self, query):
            assert query["owner_epoch"] == "kernel.epoch.7"
            assert query["program_binding_id"] == _DIGEST_A
            frame = query["kind"] == "frame_pose"
            assert query["target_role_id"] == ("arm" if frame else "gripper")
            value = (
                {
                    "position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                }
                if frame
                else {
                    "state": owner_state,
                    "grasp_confirmed": None,
                    "command_id": 1,
                    "generation": 2,
                    "source_id": "binding.gripper",
                    "source_revision": _DIGEST_A,
                    "source_epoch": "provider.1",
                    "active_control_configuration_id": "route.1",
                    "active_control_configuration_generation": 3,
                }
            )
            return {
                "ok": True,
                "target_observation": {
                    **query,
                    "schema_version": "manyforge.target_observation.v1",
                    "source_epoch": "source.1",
                    "source_sequence": 7,
                    "value": value,
                },
            }

    transport = QueryTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = (
        {"target_role": "arm", "frame_role": "tcp", "output_key": "pose"}
        if node_id == "model.get_frame_pose"
        else {"target_role": "gripper", "expected_state": expected_state}
    )
    result = port.queries.invoke(
        node_id, _identity("query"), OwnerNodeInputs(parameters=parameters)
    )
    assert result.succeeded is succeeded
    assert result.metadata["source_epoch"] == "source.1"
    assert result.metadata["source_sequence"] == 7
    if node_id != "model.get_frame_pose":
        assert result.metadata["command_scope"] == (
            1,
            2,
            "binding.gripper",
            _DIGEST_A,
            "provider.1",
            "route.1",
            3,
        )
    if node_id == "model.get_frame_pose":
        assert result.output == {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.1, 0.2, 0.3],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        }
    if expected_state == "grasp_confirmed":
        assert result.reason_code == "gripper_status_not_matched"
    assert transport.identity_reads == 1  # bind only, not a second read per query
    assert transport.submissions == []
    assert transport.controls == []


@pytest.mark.parametrize("node_id", ["gripper.check_status", "gripper.wait_for_status"])
@pytest.mark.parametrize("arrival_ns", [99, 100, 101])
def test_grasp_consumption_does_not_extend_proof_by_http_or_wait_timeout(
    node_id, arrival_ns, monkeypatch
):
    clock = [10]
    monkeypatch.setattr("manyforge_behavior.production_ports.time.monotonic_ns", lambda: clock[0])

    class QueryTransport(_Transport):
        def query_target(self, query):
            # The native sample was valid at 10 ns; transport can consume the
            # remaining validity without consuming the wait's 10-second budget.
            clock[0] = arrival_ns
            return {
                "ok": True,
                "target_observation": {
                    **query,
                    "schema_version": "manyforge.target_observation.v1",
                    "source_epoch": "source.1",
                    "source_sequence": 7,
                    "value": {
                        "state": "completed",
                        "grasp_confirmed": True,
                        "command_id": 1,
                        "generation": 2,
                        "source_id": "binding.gripper",
                        "source_revision": _DIGEST_A,
                        "source_epoch": "provider.1",
                        "active_control_configuration_id": "route.1",
                        "active_control_configuration_generation": 3,
                        "evidence": {"observed_at_ns": "10", "expires_at_ns": "100"},
                    },
                },
            }

    transport = QueryTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    result = port.queries.invoke(
        node_id,
        _identity("query"),
        OwnerNodeInputs(
            parameters={
                "target_role": "gripper",
                "expected_state": "grasp_confirmed",
                "timeout_s": 10.0,
            }
        ),
    )
    assert result.succeeded is (arrival_ns < 100)
    assert result.reason_code == (None if arrival_ns < 100 else "grasp_confirmation_not_current")
    assert transport.submissions == []


def test_relative_pose_submission_uses_capture_and_preserves_portable_roles() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())

    assert started.state is OwnerTaskState.ACTIVE
    request = transport.submissions[0]
    assert request["program_id"] == "lite6_program"
    assert request["program_binding_id"] == _DIGEST_A
    assert request["expected_scene_revision"]["revision_id"] == 4
    participant = request["participants"][0]
    assert participant["component_role_id"] == "robot"
    assert participant["target_role_id"] == "arm"
    assert participant["execution_scope"] == {
        "node_instance_id": _identity().node_instance_id,
        "node_attempt_id": _identity().node_attempt_id,
    }
    payload = participant["payload"]
    assert payload["target_mode"] == "relative"
    assert payload["target_reference"] == {
        "type": "scene_resource",
        "scene_resource_id": "graspable",
    }
    assert payload["offset_reference"] == {
        "type": "frame_role",
        "frame_role_id": "base",
    }
    assert payload["motion"]["goal"]["orientation"]["x"] == pytest.approx(1.0)
    assert "compiled_frame_name" not in repr(request)


def test_independent_lost_acceptance_reply_is_reconciled_by_reset() -> None:
    transport = _LostAcceptanceReplyTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("independent")

    started = port.operations.start(
        "motion.move_to_pose", identity, _motion_inputs()
    )

    assert started.state is OwnerTaskState.FAILED
    assert started.reason_code == "operation_admission_outcome_unknown"
    assert started.settlement_proven is False
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN
    with pytest.raises(
        ProductionPortError,
        match="unknown operation admission requires settled Program Stop",
    ):
        port.scope.reset("program-run.1")
    assert len(transport.submit_attempts) == 1
    port.scope.request_stop(
        "program-run.1", "stop.independent-unknown", "contain unknown admission"
    )
    transport.stop_states["scope-stop-1"] = "succeeded"
    next(iter(transport.operations.values()))["state"] = "stopped"

    reset = port.scope.reset("program-run.1")

    assert reset.state is ProgramRunScopeState.SETTLED
    assert len(transport.submit_attempts) == 2
    assert len(transport.operations) == 1
    assert [control[0] for control in transport.controls].count("reset") == 1


def test_reset_does_not_turn_a_pre_acceptance_lost_reply_into_motion() -> None:
    class LostBeforeAcceptance(_Transport):
        def __init__(self) -> None:
            super().__init__()
            self.submit_attempts = 0

        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            self.submit_attempts += 1
            if self.submit_attempts == 1:
                raise TimeoutError("request delivery outcome unknown")
            return {
                "ok": False,
                "error": {
                    "code": "invalid_state_transition",
                    "message": "fresh motion is closed by Program Stop",
                    "details": {"reason": "motion_admission_closed"},
                },
            }

    transport = LostBeforeAcceptance()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity("not-accepted"), _motion_inputs()
    )
    assert started.settlement_proven is False
    port.scope.request_stop(
        "program-run.1", "stop.not-accepted", "contain unknown admission"
    )
    transport.stop_states["scope-stop-1"] = "succeeded"

    reset = port.scope.reset("program-run.1")

    assert reset.state is ProgramRunScopeState.SETTLED
    assert transport.submit_attempts == 2
    assert transport.operations == {}
    assert not [control for control in transport.controls if control[0] == "reset"]


def test_completed_operation_is_acknowledged_with_exact_revision() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity()
    started = port.operations.start("motion.move_to_pose", identity, _motion_inputs())
    transport.operations[started.task_id].update({"state": "completed", "operation_revision": 7})

    observed = port.operations.observe(started.task_id)
    assert isinstance(observed, OwnerTaskSnapshot)
    assert observed.state is OwnerTaskState.SUCCEEDED
    reads_after_terminal_observation = transport.operation_reads
    port.operations.acknowledge(started.task_id, identity)

    kind, operation_id, body = transport.controls[-1]
    assert (kind, operation_id) == ("acknowledge", started.task_id)
    assert body["expected_operation_revision"] == 7
    assert transport.operation_reads == reads_after_terminal_observation
    assert body["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
    scope = port.scope.observe("program-run.1")
    assert isinstance(scope, ProgramRunScopeSnapshot)
    assert scope.state is ProgramRunScopeState.SETTLED


def test_accepted_operation_owner_unavailability_has_exact_task_port_type() -> None:
    class OwnerUnavailableError(RuntimeError):
        code = "execution_owner_unavailable"

    class _UnavailableTransport(_Transport):
        def read(self, operation_id: str) -> dict[str, Any]:
            raise OwnerUnavailableError("HTTP 503")

    transport = _UnavailableTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())

    with pytest.raises(OwnerTaskObservationUnavailableError):
        port.operations.observe(started.task_id)

    assert len(transport.submissions) == 1


def test_operation_identity_change_remains_a_terminal_observation_error() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[started.task_id]["operation_id"] = "operation-other"

    with pytest.raises(ProductionPortError, match="changed identity"):
        port.operations.observe(started.task_id)


@pytest.mark.parametrize("state", ["failed", "intervention_required"])
def test_failed_native_operation_is_observed_as_a_failure_with_its_owner_reason(state) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    operation = transport.operations[started.task_id]
    operation.update({"state": state, "operation_revision": 3})
    for participant in operation.get("participants", []):
        participant["terminal_error"] = {
            "code": "planning_failed", "message": "goal is unreachable",
        }

    snapshot = port.operations.observe(started.task_id)

    assert snapshot.state is OwnerTaskState.FAILED
    assert snapshot.reason_code == (
        "planning_failed" if operation.get("participants") else "operation_failed"
    )


def test_native_context_cleanup_consumes_settled_tasks_but_refuses_active_work() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    active = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())

    with pytest.raises(ProductionPortError, match="active native operations"):
        port.unbind("program-run.1")

    transport.operations[active.task_id].update({"state": "completed", "operation_revision": 5})
    port.unbind("program-run.1")

    assert transport.controls[-1][0] == "acknowledge"
    with pytest.raises(ProductionPortError, match="not bound"):
        port.operations.start("motion.move_to_pose", _identity("after-cleanup"), _motion_inputs())


def test_program_scope_stop_is_one_exact_batch_without_operation_reads() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[started.task_id].update({"state": "executing", "operation_revision": 3})
    second = port.operations.start("motion.move_to_pose", _identity("second"), _motion_inputs())
    transport.operations[second.task_id].update({"state": "executing", "operation_revision": 4})
    reads_before_stop = transport.operation_reads

    first = port.scope.request_stop("program-run.1", "execution:caller.1:request.1", "Program Stop")

    assert first.state is ProgramRunScopeState.ACTIVE
    assert first.active_operation_count == 2
    assert transport.operation_reads == reads_before_stop
    stop_calls = [item for item in transport.controls if item[0] == "stop_program_run"]
    assert len(stop_calls) == 1
    body = stop_calls[0][2]
    assert body["program_run_id"] == "program-run.1"
    assert body["request_context"]["correlation_id"] == "program-run.1"
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    assert transport.operation_reads == reads_before_stop
    assert transport.stop_status_reads == 1
    transport.stop_states["scope-stop-1"] = "succeeded"
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.SETTLED
    assert transport.operation_reads == reads_before_stop
    assert transport.stop_status_reads == 2
    transport.operations[started.task_id]["state"] = "stopped"
    transport.operations[second.task_id]["state"] = "stopped"
    released = port.scope.release_stop("program-run.1")
    assert released.state is ProgramRunScopeState.SETTLED
    assert transport.released_scope_stops == [("program-run.1", "scope-stop-1", 1)]


def test_program_scope_stop_joins_inflight_admission_and_fences_fresh_work() -> None:
    admission_entered = Event()
    release_admission = Event()

    class _BlockingAdmissionTransport(_Transport):
        def __init__(self) -> None:
            super().__init__()
            self.block_next_admission = False
            self.program_run_fenced = False

        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if self.block_next_admission:
                admission_entered.set()
                assert release_admission.wait(timeout=2.0)
                if self.program_run_fenced:
                    return {
                        "ok": False,
                        "error": {
                            "code": "invalid_state_transition",
                            "message": "Program-run Stop fence is active",
                            "details": {"reason": "program_run_stop_fence_active"},
                        },
                    }
            return super().submit(body)

        def stop_program_run(self, body: Mapping[str, Any]):
            self.program_run_fenced = True
            return super().stop_program_run(body)

    transport = _BlockingAdmissionTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    first = port.operations.start(
        "motion.move_to_pose",
        _identity("already-active"),
        _motion_inputs(),
    )
    transport.operations[first.task_id].update({"state": "executing", "operation_revision": 3})
    transport.block_next_admission = True
    admission_errors: list[Exception] = []
    stopped: list[ProgramRunScopeSnapshot] = []

    def admit() -> None:
        try:
            port.operations.start(
                "motion.move_to_pose",
                _identity("crossing-admission"),
                _motion_inputs(),
            )
        except Exception as error:
            admission_errors.append(error)

    admission = Thread(target=admit)
    admission.start()
    assert admission_entered.wait(timeout=1.0)

    stop_completed = Event()

    def stop() -> None:
        stopped.append(
            port.scope.request_stop(
                "program-run.1",
                "execution:caller.1:request.concurrent",
                "priority Stop",
            )
        )
        stop_completed.set()

    stopping = Thread(target=stop)
    stopping.start()
    assert stop_completed.wait(timeout=0.5)
    release_admission.set()
    admission.join(timeout=1.0)
    stopping.join(timeout=1.0)

    assert not admission.is_alive()
    assert not stopping.is_alive()
    assert len(admission_errors) == 1
    assert isinstance(admission_errors[0], OwnerInvocationHeldError)
    assert stopped[0].state is ProgramRunScopeState.ACTIVE
    assert stopped[0].active_operation_count == 1
    assert [row[0] for row in transport.controls] == ["stop_program_run"]
    with pytest.raises(ProductionPortError, match="scope is stopped"):
        port.operations.start(
            "motion.move_to_pose",
            _identity("after-scope-stop"),
            _motion_inputs(),
        )
    assert len(transport.submissions) == 1


def test_program_scope_stop_owns_operation_accepted_before_local_retention() -> None:
    accepted = Event()
    release_reply = Event()

    class _AcceptedBeforeReplyTransport(_Transport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            response = super().submit(body)
            accepted.set()
            assert release_reply.wait(timeout=2.0)
            return response

    transport = _AcceptedBeforeReplyTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started: list[OwnerTaskSnapshot] = []

    admission = Thread(target=lambda: started.append(port.operations.start(
        "motion.move_to_pose",
        _identity("accepted-before-local-retention"),
        _motion_inputs(),
    )))
    admission.start()
    assert accepted.wait(timeout=1.0)

    stopped = port.scope.request_stop(
        "program-run.1",
        "execution:caller.1:request.crossing-retention",
        "priority Stop",
    )
    assert stopped.state is ProgramRunScopeState.ACTIVE
    assert stopped.active_operation_count == 0

    release_reply.set()
    admission.join(timeout=1.0)
    assert not admission.is_alive()
    assert len(started) == 1

    port.operations.cancel(
        started[0].task_id,
        _identity("accepted-before-local-retention"),
    )

    assert [row[0] for row in transport.controls] == ["stop_program_run"]


def test_program_scope_stop_reaches_kernel_before_first_task_is_retained() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    stopped = port.scope.request_stop(
        "program-run.1",
        "execution:caller.1:request.before-first-task",
        "priority Stop during first admission",
    )

    assert stopped.state is ProgramRunScopeState.ACTIVE
    assert stopped.active_operation_count == 0
    assert [row[0] for row in transport.controls] == ["stop_program_run"]
    with pytest.raises(ProductionPortError, match="scope is stopped"):
        port.operations.start(
            "motion.move_to_pose",
            _identity("after-empty-scope-stop"),
            _motion_inputs(),
        )
    assert transport.submissions == []


def test_unbound_program_scope_stop_uses_stable_kernel_epoch_source() -> None:
    transport = _Transport()
    transport.owner_epoch = "kernel.epoch.recovered"
    port = NativeKernelProgramPort(
        KernelClient(transport),
        current_owner_epoch=lambda: "kernel.epoch.recovered",
    )

    stopped = port.scope.request_stop(
        "program-run.retained",
        "execution:runner.new:retained-stop",
        "restart containment",
    )

    assert stopped.state is ProgramRunScopeState.ACTIVE
    assert transport.identity_reads == 0
    assert (
        transport.controls[-1][2]["request_context"]["request_owner_epoch"]
        == "kernel.epoch.recovered"
    )


def test_program_scope_stop_never_waits_on_the_ordinary_identity_lane() -> None:
    identity_blocked = Event()
    release_identity = Event()

    class _BlockedOrdinaryTransport(_Transport):
        def __init__(self) -> None:
            super().__init__()
            self.block_identity = False

        def identity(self) -> dict[str, Any]:
            if self.block_identity:
                identity_blocked.set()
                assert release_identity.wait(timeout=2.0)
            return super().identity()

    transport = _BlockedOrdinaryTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    assert transport.identity_reads == 1
    transport.block_identity = True

    stopped: list[ProgramRunScopeSnapshot] = []
    stop_completed = Event()

    def stop() -> None:
        stopped.append(
            port.scope.request_stop(
                "program-run.1",
                "execution:caller.1:request.priority",
                "priority Stop",
            )
        )
        stop_completed.set()

    stopping = Thread(target=stop)
    stopping.start()
    completed_without_ordinary_lane = stop_completed.wait(timeout=0.5)
    if not completed_without_ordinary_lane:
        release_identity.set()
    stopping.join(timeout=1.0)
    release_identity.set()

    assert completed_without_ordinary_lane
    assert not stopping.is_alive()
    assert not identity_blocked.is_set()
    assert transport.identity_reads == 1
    assert stopped[0].state is ProgramRunScopeState.ACTIVE
    assert [row[0] for row in transport.controls] == ["stop_program_run"]


def test_distinct_program_stops_refresh_the_native_fence_without_local_replay() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[started.task_id].update({"state": "executing", "operation_revision": 3})

    first = port.scope.request_stop("program-run.1", "motion:caller.1:request.1", "Motion Stop")
    repeated = port.scope.request_stop("program-run.1", "full:caller.1:request.2", "Full Stop")

    assert first.state is ProgramRunScopeState.ACTIVE
    assert repeated.state is ProgramRunScopeState.ACTIVE
    assert [item[0] for item in transport.controls if item[0] == "stop_program_run"] == [
        "stop_program_run",
        "stop_program_run",
    ]
    assert port._scope_stops["program-run.1"].active[:2] == ("scope-stop-2", 2)
    assert transport.stop_status_reads == 0


def test_new_stop_overtakes_a_held_resume_handoff_without_waiting():
    release_entered, finish_release = Event(), Event()

    class FenceTransport(_Transport):
        current_fence = None

        def stop_program_run(self, body):
            result = super().stop_program_run(body)
            accepted = result["accepted"]
            self.current_fence = (accepted["stop_id"], accepted["stop_generation"])
            self.stop_states[accepted["stop_id"]] = "succeeded"
            return result

        def release_program_run_stop_fence(self, program_run_id, stop_id, generation):
            release_entered.set()
            assert finish_release.wait(timeout=2.0)
            if (stop_id, generation) != self.current_fence:
                raise ProductionPortError("native Program Stop fence changed")
            self.current_fence = None
            return super().release_program_run_stop_fence(program_run_id, stop_id, generation)

    transport = FenceTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "motion-stop", "Motion Stop")
    errors = []

    def resume():
        try:
            port.scope.release_stop("program-run.1", expected_stop=("scope-stop-1", 1))
        except Exception as error:
            errors.append(error)

    worker = Thread(target=resume)
    worker.start()
    try:
        assert release_entered.wait(timeout=2.0)
        # Same physical rest, but a newer instruction must close the fence.
        port.scope.request_stop("program-run.1", "full-stop", "Full Stop")
        assert transport.current_fence == ("scope-stop-2", 2)
    finally:
        finish_release.set()
        worker.join(timeout=2.0)
    assert not worker.is_alive()
    assert len(errors) == 1 and isinstance(errors[0], ProductionPortError)
    assert not transport.released_scope_stops
    assert port._scope_stops["program-run.1"].active[:2] == transport.current_fence
    with pytest.raises(ProductionPortError, match="scope is stopped"):
        port.operations.start("motion.move_to_pose", _identity("late-motion"), _motion_inputs())


@pytest.mark.parametrize("release_newer", [False, True])
def test_reordered_stop_responses_cannot_replace_the_newest_native_fence(release_newer):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    original = transport.stop_program_run

    def delayed_first(body):
        result = original(body)
        if result["accepted"]["stop_generation"] == 1:
            port.scope.request_stop("program-run.1", "second-stop", "newer Stop")
            if release_newer:
                transport.stop_states["scope-stop-2"] = "succeeded"
                with pytest.raises(ProductionPortError, match="still in flight"):
                    port.scope.release_stop("program-run.1", expected_stop=("scope-stop-2", 2))
                assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
        return result

    transport.stop_program_run = delayed_first
    port.scope.request_stop("program-run.1", "first-stop", "older Stop")
    if release_newer:
        port.scope.release_stop("program-run.1", expected_stop=("scope-stop-2", 2))
        assert not port._scope_stops
        port._require_scope_open("program-run.1")
    else:
        assert port._scope_stops["program-run.1"].active[:2] == ("scope-stop-2", 2)
        assert len(port._scope_stops) == 1


def test_stop_reply_watermark_is_retired_after_each_complete_handoff():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    for index in range(128):
        port.scope.request_stop("program-run.1", f"stop-{index}", "Stop")
        stop_id, generation, _count = port._scope_stops["program-run.1"].active
        transport.stop_states[stop_id] = "succeeded"
        port.scope.release_stop("program-run.1", expected_stop=(stop_id, generation))
        assert not port._scope_stops


def test_late_stop_reply_cannot_replace_a_new_kernel_incarnation():
    transport = _Transport()
    epoch = ["kernel.old"]
    transport.owner_epoch = epoch[0]
    port = NativeKernelProgramPort(KernelClient(transport), current_owner_epoch=lambda: epoch[0])
    original = transport.stop_program_run

    def replace_owner(body):
        result = original(body)
        if result["accepted"]["stop_generation"] == 1:
            result["accepted"]["stop_generation"] = 1000
            epoch[0] = "kernel.new"
            transport.owner_epoch = epoch[0]
            with pytest.raises(ProductionPortError, match="incarnation changed"):
                port.scope.request_stop("program-run.1", "crossed-owner-stop", "Stop")
            port.scope.request_stop("program-run.1", "new-owner-stop", "Stop")
        return result

    transport.stop_program_run = replace_owner
    port.scope.request_stop("program-run.1", "old-owner-stop", "Stop")
    retained = port._scope_stops["program-run.1"]
    assert retained.owner_epoch == "kernel.new"
    assert retained.active[:2] == ("scope-stop-3", 3)
    assert retained.pending == 0


def test_stop_with_stale_attribution_cannot_compare_different_owner_generations():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "old", "Stop")
    transport.owner_epoch = "kernel.replacement"
    with pytest.raises(ProductionPortError, match="incarnation changed"):
        port.scope.request_stop("program-run.1", "new", "Stop")
    assert len(transport.stop_states) == 2  # Stale attribution did not block containment.
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN
    with pytest.raises(ProductionPortError, match="reconciled"):
        port.scope.release_stop("program-run.1")


def test_bound_stop_can_recapture_replacement_without_rebinding_execution():
    from types import MethodType, SimpleNamespace
    from manyforge_program_runner.runtime import ProgramRuntime

    transport = _Transport()

    def ordinary_owner_read(**_kwargs):
        raise AssertionError("Stop waited for ordinary recovery inventory")

    runtime = SimpleNamespace(
        owner_epoch="runner.1",
        _require_cycle_owner=lambda: (
            SimpleNamespace(recovery_inventory=ordinary_owner_read),
            object(),
        ),
    )
    port = NativeKernelProgramPort(
        KernelClient(transport),
        current_owner_epoch=MethodType(ProgramRuntime._current_kernel_owner_epoch, runtime),
    )
    port.bind(_context())
    original_context = port._context("program-run.1")
    port.scope.request_stop("program-run.1", "first", "Stop")
    transport.owner_epoch = "replacement.owner"
    with pytest.raises(ProductionPortError, match="incarnation changed"):
        port.scope.request_stop("program-run.1", "crossed", "Stop")
    port.scope.request_stop("program-run.1", "recapture", "Stop")
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    assert port._scope_stops["program-run.1"].owner_epoch == "replacement.owner"
    assert port._context("program-run.1") is original_context
    assert transport.identity_reads == 1


def test_old_replayed_stop_cannot_reconcile_a_different_lost_stop():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    original, receipts = transport.stop_program_run, {}
    lose = [False]

    def replay_or_accept(body):
        key = body["request_context"]["request_id"]
        if key in receipts:
            result = deepcopy(receipts[key])
            result["accepted"]["replay_status"] = "replayed_exact"
            return result
        result = original(body)
        receipts[key] = deepcopy(result)
        if lose[0]:
            raise OSError("lost Stop reply")
        return result

    transport.stop_program_run = replay_or_accept
    port.scope.request_stop("program-run.1", "old", "Stop")
    transport.stop_states["scope-stop-1"] = "succeeded"
    port.scope.release_stop("program-run.1")
    lose[0] = True
    with pytest.raises(OSError):
        port.scope.request_stop("program-run.1", "lost", "Stop")
    lose[0] = False
    port.scope.request_stop("program-run.1", "old", "Stop")
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN
    port.scope.request_stop("program-run.1", "fresh-recapture", "Stop")
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    assert port._scope_stops["program-run.1"].active[:2] == ("scope-stop-3", 3)


def test_lost_stop_reply_with_no_local_tasks_remains_unknown_until_recaptured():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    original = transport.stop_program_run

    def lose_reply(body):
        original(body)
        raise OSError("lost native Stop reply")

    transport.stop_program_run = lose_reply
    with pytest.raises(OSError, match="lost native Stop"):
        port.scope.request_stop("program-run.1", "lost", "Stop")
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN
    with pytest.raises(ProductionPortError, match="scope is stopped"):
        port._require_scope_open("program-run.1")
    with pytest.raises(ProductionPortError, match="settlement"):
        port.unbind("program-run.1")
    assert len(port._scope_stops) == 1
    transport.stop_program_run = original
    port.scope.request_stop("program-run.1", "recapture", "Stop")
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    transport.stop_states["scope-stop-2"] = "succeeded"
    port.scope.release_stop("program-run.1", expected_stop=("scope-stop-2", 2))
    assert not port._scope_stops


def test_pending_first_stop_with_no_local_tasks_cannot_report_settlement():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    entered, finish = Event(), Event()
    original = transport.stop_program_run

    def hold_reply(body):
        result = original(body)
        entered.set()
        assert finish.wait(timeout=2)
        return result

    transport.stop_program_run = hold_reply
    worker = Thread(target=lambda: port.scope.request_stop("program-run.1", "held", "Stop"))
    worker.start()
    try:
        assert entered.wait(timeout=2)
        assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
        with pytest.raises(ProductionPortError, match="still in flight"):
            port.scope.capture_stop("program-run.1")
        with pytest.raises(ProductionPortError, match="scope is stopped"):
            port._require_scope_open("program-run.1")
        with pytest.raises(ProductionPortError, match="settlement"):
            port.unbind("program-run.1")
        with pytest.raises(ProductionPortError, match="still in flight"):
            port.scope.release_stop("program-run.1")
    finally:
        finish.set()
        worker.join(timeout=2)
    assert not worker.is_alive()


@pytest.mark.parametrize("replacement", [False, True])
def test_old_settled_read_cannot_hide_a_new_unknown_stop(replacement):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "first", "Stop")
    transport.stop_states["scope-stop-1"] = "succeeded"
    read, request = transport.stop_status, transport.stop_program_run

    def failed_reply(body):
        request(body)
        raise OSError("Stop reply lost")

    def delayed_read(stop_id):
        result = read(stop_id)
        if replacement:
            transport.owner_epoch = "kernel.replaced"
        else:
            transport.stop_program_run = failed_reply
        with pytest.raises((OSError, ProductionPortError)):
            port.scope.request_stop("program-run.1", "newer", "Stop")
        return result

    transport.stop_status = delayed_read
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN


def test_first_stop_during_task_read_cannot_publish_false_settlement():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    task = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[task.task_id].update(state="completed", operation_revision=3)
    read, request = transport.read, transport.stop_program_run

    def failed_reply(body):
        request(body)
        raise OSError("Stop reply lost")

    def delayed_read(operation_id):
        result = read(operation_id)
        with pytest.raises(OSError):
            port.scope.request_stop("program-run.1", "first", "Stop")
        return result

    transport.stop_program_run = failed_reply
    transport.read = delayed_read
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN


def test_operation_only_stop_does_not_acquire_a_program_scope_handoff():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.operations.start("motion.move_to_pose", _identity("unrelated"), _motion_inputs())
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    assert port.scope.capture_stop("program-run.1") is None
    assert transport.stop_status_reads == 0


def test_terminal_reset_stop_reuses_identical_native_payload():
    from manyforge_behavior.program_run_owner import PROGRAM_RESET_STOP_REASON

    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "reset.1", PROGRAM_RESET_STOP_REASON)
    transport.stop_states["scope-stop-1"] = "succeeded"
    port.unbind("program-run.1")
    port.scope.request_stop("program-run.1", "reset.1", PROGRAM_RESET_STOP_REASON)
    assert transport.controls[0] == transport.controls[1]


@pytest.mark.parametrize("program_stop", [False, True])
def test_program_scope_reset_releases_terminal_residency_once(program_stop) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[started.task_id].update({"state": "stopped", "operation_revision": 4})
    if program_stop:
        port.scope.request_stop("program-run.1", "reset-owned-stop", "Reset containment")
        transport.stop_states["scope-stop-1"] = "succeeded"

    # Program terminalization releases its captured context but preserves the
    # exact operation identity needed by the later explicit Program Reset.
    port.unbind("program-run.1")
    first = port.scope.reset("program-run.1")
    second = port.scope.reset("program-run.1")

    assert first.state is ProgramRunScopeState.SETTLED
    assert second == first
    reset_calls = [item for item in transport.controls if item[0] == "reset"]
    assert len(reset_calls) == 1
    kind, operation_id, body = reset_calls[0]
    assert (kind, operation_id) == ("reset", started.task_id)
    assert body["expected_operation_revision"] == 4
    assert body["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
    assert body["request_context"]["correlation_id"] == "program-run.1"
    assert transport.resident[started.task_id] is False
    assert not transport.released_scope_stops
    assert "program-run.1" not in port._scope_stops


@pytest.mark.parametrize("generation", [None, True, 0, 2])
def test_program_scope_reset_retains_unproven_stop_generation(generation):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "first", "Reset containment")
    transport.stop_states["scope-stop-1"] = "succeeded"
    read = transport.stop_status

    def mismatched_status(stop_id):
        result = read(stop_id)
        result["stop_status"]["stop_generation"] = generation
        return result

    transport.stop_status = mismatched_status
    observed = port.scope.observe("program-run.1")
    assert observed.state is ProgramRunScopeState.UNKNOWN
    assert observed.reason_code == "program_run_stop_generation_mismatch"
    with pytest.raises(ProductionPortError, match="settlement generation is stale"):
        port.scope.reset("program-run.1")
    assert port._scope_stops["program-run.1"].active[:2] == ("scope-stop-1", 1)
    assert not transport.released_scope_stops


def test_program_scope_reset_retains_a_crossing_new_stop():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "first", "Reset containment")
    transport.stop_states["scope-stop-1"] = "succeeded"
    read = transport.stop_status

    def crossed_status(stop_id):
        result = read(stop_id)
        if stop_id == "scope-stop-1":
            port.scope.request_stop("program-run.1", "second", "newer Stop")
        return result

    transport.stop_status = crossed_status
    with pytest.raises(ProductionPortError, match="changed during Reset cleanup"):
        port.scope.reset("program-run.1")
    assert port._scope_stops["program-run.1"].active[:2] == ("scope-stop-2", 2)
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.ACTIVE
    assert not transport.released_scope_stops


def test_failed_stop_submission_can_be_retried() -> None:
    class _OneStopFailureTransport(_Transport):
        def __init__(self) -> None:
            super().__init__()
            self.fail_next_stop = True

        def stop_program_run(self, body: Mapping[str, Any]):
            if self.fail_next_stop:
                self.fail_next_stop = False
                raise RuntimeError("temporary Stop transport failure")
            return super().stop_program_run(body)

    transport = _OneStopFailureTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    transport.operations[started.task_id].update({"state": "executing", "operation_revision": 3})

    with pytest.raises(RuntimeError, match="temporary Stop"):
        port.scope.request_stop("program-run.1", "execution:caller.1:request.1", "Program Stop")
    port.scope.request_stop("program-run.1", "execution:caller.1:request.1", "Program Stop")

    assert [item[0] for item in transport.controls] == ["stop_program_run"]


def test_gripper_schema_comes_from_captured_program_binding() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = OwnerNodeInputs(
        parameters={
            "target_role": "gripper",
            "position": {"kind": "linear_m", "value_m": 0.008},
            "max_effort": {"kind": "linear_newton", "value_n": 10.0},
            "command_duration_s": 0.8,
            "completion_timeout_s": 30.0,
        }
    )

    port.operations.start("gripper.command_position", _identity("open-gripper"), inputs)

    participant = transport.submissions[0]["participants"][0]
    assert participant["component_role_id"] == "tool"
    assert participant["payload"]["command_schema_revision"] == _DIGEST_E


def test_stale_native_deployment_fails_before_dispatch() -> None:
    class _StaleTransport(_Transport):
        def identity(self) -> dict[str, Any]:
            result = super().identity()
            result["identity"]["graph_revision"] = _DIGEST_F
            return result

    transport = _StaleTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    with pytest.raises(ProductionPortError, match="captured Program"):
        port.bind(_context())

    assert transport.submissions == []


class _PickPlaceTransport(_Transport):
    def __init__(self) -> None:
        super().__init__()
        self.pick_requests: list[dict[str, Any]] = []
        self.place_requests: list[dict[str, Any]] = []
        self.no_feasible = False

    def _planning_result(
        self,
        intent: Mapping[str, Any],
        *,
        kind: str,
    ) -> dict[str, Any]:
        planned = not self.no_feasible
        pose = {
            "position_m": {"x_m": 0.4, "y_m": 0.0, "z_m": 0.2},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        }
        return {
            "schema_version": "manyforge.pick_place_planning_result.v1",
            "sequence_id": intent["sequence_id"],
            "kind": kind,
            "selected_object_id": intent["selected_object_id"],
            "status": "planned" if planned else "no_feasible_candidate",
            "attempted_candidate_count": 1,
            "reports": [
                {
                    "candidate_index": 1,
                    "stages": [
                        {
                            "error": {
                                "code": "invalid_request",
                                "details": {"reason": "motion_start_state_in_collision"},
                                "message": "start state is in collision",
                            },
                            "stage": "approach",
                            "succeeded": False,
                            "trajectory_duration_s": 0.0,
                            "trajectory_waypoint_count": 0,
                        }
                    ],
                }
            ],
            "last_attempted_candidate": {
                "candidate_index": 1,
                "approach_pose": pose,
                "pick_pose": pose,
                "retreat_pose": pose,
            },
            "plan": (
                {
                    "kind": kind,
                    "approach_motion": {"kind": "pose_goal", "goal": pose},
                    "descend_motion": {
                        "kind": "cartesian_path",
                        "waypoints": [pose, pose],
                    },
                    "retreat_motion": {
                        "kind": "cartesian_path",
                        "waypoints": [pose, pose],
                    },
                }
                if planned
                else None
            ),
        }

    def plan_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        self.pick_requests.append(deepcopy(dict(intent)))
        return {
            "ok": True,
            "planning_result": self._planning_result(intent, kind="pick"),
        }

    def plan_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        self.place_requests.append(deepcopy(dict(intent)))
        return {
            "ok": True,
            "planning_result": self._planning_result(intent, kind="place"),
        }


def _pick_inputs(*, policy: str = "pause_for_operator") -> OwnerNodeInputs:
    return OwnerNodeInputs(
        parameters={
            "target_role": "arm",
            "tool_target_role": "gripper",
            "base_frame_role": "base",
            "tcp_frame_role": "tcp",
            "carrier_frame_role": "tcp",
            "selected_object_id": "graspable",
            "synthesis": {
                "pick_offset_envelope": {
                    "position_min_m": [0.0, 0.0, 0.0],
                    "position_max_m": [0.0, 0.0, 0.0],
                    "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                    "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
                },
                "approach_offset_envelope": {
                    "position_min_m": [0.0, 0.0, 0.05],
                    "position_max_m": [0.0, 0.0, 0.10],
                    "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                    "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
                },
            },
            "planning_profile": "first_feasible",
            "motion_options": {},
            "tool_position": {"kind": "linear_m", "value_m": 0.0},
            "tool_max_effort": {
                "kind": "linear_newton",
                "value_n": 8.0,
            },
            "tool_command_duration_s": 0.5,
            "tool_completion_timeout_s": 30.0,
            "output_key": "pick_plan",
            "contact_policy_id": "pick.contact",
            "no_feasible_plan_policy": policy,
        }
    )


def test_pick_planning_assembles_portable_intent_without_prepared_authority() -> None:
    transport = _PickPlaceTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("plan-pick")

    result = port.operations.start("planning.plan_pick_sequence", identity, _pick_inputs())

    assert result.state is OwnerTaskState.SUCCEEDED
    assert result.output == {
        "approach_pose": {
            "frame_ref": {
                "kind": "target_frame",
                "target_role": "arm",
                "frame_role": "base",
            },
            "position_m": [0.4, 0.0, 0.2],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
        "descend_path": [
            {
                "frame_ref": {
                    "kind": "target_frame",
                    "target_role": "arm",
                    "frame_role": "base",
                },
                "position_m": [0.4, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
            {
                "frame_ref": {
                    "kind": "target_frame",
                    "target_role": "arm",
                    "frame_role": "base",
                },
                "position_m": [0.4, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
        ],
        "pick_pose": {
            "frame_ref": {
                "kind": "target_frame",
                "target_role": "arm",
                "frame_role": "base",
            },
            "position_m": [0.4, 0.0, 0.2],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
        "retreat_pose": {
            "frame_ref": {
                "kind": "target_frame",
                "target_role": "arm",
                "frame_role": "base",
            },
            "position_m": [0.4, 0.0, 0.2],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
        "retreat_path": [
            {
                "frame_ref": {
                    "kind": "target_frame",
                    "target_role": "arm",
                    "frame_role": "base",
                },
                "position_m": [0.4, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
            {
                "frame_ref": {
                    "kind": "target_frame",
                    "target_role": "arm",
                    "frame_role": "base",
                },
                "position_m": [0.4, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
        ],
        "selected_object_id": "graspable",
        "tool_position": {"kind": "linear_m", "value_m": 0.0},
    }
    intent = transport.pick_requests[0]
    assert intent["selected_runtime_object_id"] == "selected_object"
    assert intent["synthesis"]["base_tool_orientation_rpy_deg"] == [
        180.0,
        0.0,
        0.0,
    ]
    assert intent["synthesis"]["maximum_candidate_count"] == 4096
    assert intent["descend_operation"]["participants"][0]["payload"]["runtime_object_bindings"] == [
        {
            "runtime_object_id": "selected_object",
            "scene_resource_id": "graspable",
        }
    ]
    assert (
        intent["approach_operation"]["participants"][0]["payload"]["runtime_object_bindings"] == []
    )
    assert (
        intent["tool_operation"]["participants"][0]["payload"]["command_schema_revision"]
        == _DIGEST_E
    )
    request_ids = {
        intent[f"{phase}_operation"]["request_context"]["request_id"]
        for phase in ("approach", "descend", "tool", "retreat")
    }
    assert len(request_ids) == 4
    for phase in ("approach", "descend", "tool", "retreat"):
        participant = intent[f"{phase}_operation"]["participants"][0]
        assert participant["execution_scope"] == {
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
        }
        assert participant["action_span"]["node_instance_id"] == (
            f"{identity.node_instance_id}/{phase}"
        )
    assert "prepared" not in repr(intent).lower()
    port.operations.acknowledge(result.task_id, identity)


def test_pick_no_feasible_candidate_preserves_authored_policy_and_diagnostics() -> None:
    transport = _PickPlaceTransport()
    transport.no_feasible = True
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    result = port.operations.start(
        "planning.plan_pick_sequence",
        _identity("pick-none"),
        _pick_inputs(policy="condition_failure"),
    )

    assert result.state is OwnerTaskState.FAILED
    assert result.reason_code == "no_feasible_candidate"
    assert result.message == (
        "No feasible pick/place candidate was found; last rejection at "
        "approach: start state is in collision"
    )
    assert result.metadata["authored_failure_policy"] == "condition_failure"
    assert result.metadata["planning_result"]["reports"] == [
        {
            "candidate_index": 1,
            "stages": [
                {
                    "error": {
                        "code": "invalid_request",
                        "details": {"reason": "motion_start_state_in_collision"},
                        "message": "start state is in collision",
                    },
                    "stage": "approach",
                    "succeeded": False,
                    "trajectory_duration_s": 0.0,
                    "trajectory_waypoint_count": 0,
                }
            ],
        }
    ]


@pytest.mark.parametrize(
    "recovery_class",
    ["ordinary_operator_stop", "stop_fault_intervention"],
)
def test_native_motion_stop_projects_exact_operator_recovery_policy(
    recovery_class: str,
) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("motion")
    started = port.operations.start(
        "motion.move_to_pose",
        identity,
        _motion_inputs(),
    )
    operation = transport.operations[started.task_id]
    operation.update(
        {
            "state": "stopped",
            "active_stop_id": "stop.1",
            "active_stop_generation": 2,
        }
    )
    transport.operation_recovery = lambda _operation_id: {
        "ok": True,
        "operation_revision": operation["operation_revision"],
        "participants": [{"operation_participant_id": "participant.1"}],
        "recovery_assessment": {
            "recovery_class": recovery_class,
        },
        "stop_generation": 2,
        "stop_id": "stop.1",
    }
    observed = port.operations.observe(started.task_id)
    assert observed.state is OwnerTaskState.INTERRUPTED
    assert observed.metadata["active_stop_id"] == "stop.1"
    assert observed.metadata["active_stop_generation"] == 2
    assert observed.metadata["authored_failure_policy"] == "pause_for_operator"
    assert observed.metadata["authored_failure_policy_parameter"] == "operator_stop_policy"
    port.unbind("program-run.1")


def test_accepted_native_motion_recovery_releases_the_obsolete_task() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("motion-recovery")
    started = port.operations.start(
        "motion.move_to_pose",
        identity,
        _motion_inputs(),
    )
    operation = transport.operations[started.task_id]
    operation.update(
        {
            "state": "stopped",
            "operation_revision": 2,
            "active_stop_id": "stop.1",
            "active_stop_generation": 1,
        }
    )
    transport.operation_recovery = lambda _operation_id: {
        "ok": True,
        "operation_revision": 2,
        "participants": [{"operation_participant_id": "participant.arm"}],
        "recovery_assessment": {
            "recovery_class": "ordinary_operator_stop",
            "assessment_revision": 3,
            "control_command_epoch": "control.epoch.1",
            "execution_id": "execution.1",
            "latch_id": "",
            "stop_certificate_id": "certificate.1",
            "stop_owner_generation": 1,
        },
        "stop_generation": 1,
        "stop_id": "stop.1",
    }
    transport.apply_operation_recovery = lambda _id, _revision, body: {
        "ok": True,
        "result": {
            "request_id": body["request_id"],
            "action": body["action"],
            "accepted": True,
            "resulting_assessment": {"recovery_class": "recovered"},
        },
    }

    result = port.operations.recover(
        started.task_id,
        identity,
        action="replan_from_stopped_state",
        request_id="motion-recovery.1",
        reason="operator Resume Motion",
    )

    assert result["accepted"] is True
    assert port.scope.observe("program-run.1") == ProgramRunScopeSnapshot(
        "program-run.1",
        ProgramRunScopeState.SETTLED,
        0,
    )
    with pytest.raises(ProductionPortError, match="not retained"):
        port.operations.observe(started.task_id)


def test_place_intent_preserves_authored_pose_and_has_no_grasp_exception() -> None:
    transport = _PickPlaceTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_pick_inputs().parameters)
    parameters.pop("synthesis")
    parameters.pop("contact_policy_id")
    parameters.pop("no_feasible_plan_policy")
    parameters.pop("output_key")
    parameters.update(
        {
            "place_pose": {
                "position_m": [0.4, 0.1, 0.2],
                "orientation": {"rpy_deg": [180.0, 0.0, 0.0]},
            },
            "approach_offset_m": [0.0, 0.0, 0.08],
            "tool_position": {"kind": "linear_m", "value_m": 0.04},
        }
    )

    parameters["output_key"] = "place_plan"
    result = port.operations.start(
        "planning.plan_place_sequence",
        _identity("place"),
        OwnerNodeInputs(parameters=parameters),
    )

    assert result.state is OwnerTaskState.SUCCEEDED
    intent = transport.place_requests[0]
    approach = intent["approach_operation"]["participants"][0]["payload"]
    assert approach["motion"]["goal"]["position_m"]["z_m"] == pytest.approx(0.28)
    for phase in ("approach", "descend", "retreat"):
        payload = intent[f"{phase}_operation"]["participants"][0]["payload"]
        assert payload["contact_policy_id"] is None
        assert payload["runtime_object_bindings"] == []


def _scene_context() -> ProgramRunPortContext:
    value = _context()
    binding = dict(value.program_binding)
    binding["component_role_bindings"] = [
        {
            "component_role_id": "robot",
            "controllable_component_id": "robot.1",
            "component_type_id": "robot.type",
            "component_type_revision": _DIGEST_D,
            "model_role_bindings": [
                {
                    "model_role_id": "arm_model",
                    "model_instance_id": "robot.1.model",
                    "model_revision": _DIGEST_E,
                }
            ],
        }
    ]
    binding["target_role_bindings"] = [
        {
            "target_role_id": "arm",
            "component_role_id": "robot",
            "controllable_component_id": "robot.1",
            "command_target_id": "arm.command",
            "frame_role_bindings": [
                {
                    "frame_role_id": "base",
                    "model_instance_id": "robot.1.model",
                    "model_revision": _DIGEST_E,
                    "compiled_frame_name": "robot_1_base",
                },
                {
                    "frame_role_id": "tcp",
                    "model_instance_id": "robot.1.model",
                    "model_revision": _DIGEST_E,
                    "compiled_frame_name": "robot_1_tcp",
                },
            ],
            "resource_role_bindings": [],
            "collision_body_role_bindings": [
                {
                    "collision_body_role_id": "tool_collision",
                    "model_instance_id": "robot.1.model",
                    "model_revision": _DIGEST_E,
                    "compiled_collision_body_names": ["robot_1_tool"],
                }
            ],
            "capability_bindings": [],
        }
    ]
    return ProgramRunPortContext(
        program_run_id=value.program_run_id,
        applied_record_digest=value.applied_record_digest,
        scenario_id=value.scenario_id,
        source_document_digest=value.source_document_digest,
        program_id=value.program_id,
        program_revision=value.program_revision,
        program_binding_id=value.program_binding_id,
        retained_bundle_id=value.retained_bundle_id,
        graph_revision=value.graph_revision,
        node_environment_digest=value.node_environment_digest,
        program_binding=binding,
    )


def _fixed_scene_object(
    scene_resource_id: str,
    *,
    support_object_id: str | None = None,
) -> dict[str, Any]:
    return {
        "scene_resource_id": scene_resource_id,
        "collision_shapes": [
            {
                "type": "box",
                "dimensions_m": {"x": 0.1, "y": 0.1, "z": 0.1},
            }
        ],
        "placement": {
            "kind": "cell_fixed",
            "cell_id": "cell-a",
            "pose_in_cell": {
                "position_m": {"x": 1.0, "y": 2.0, "z": 3.0},
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        },
        "provider_evidence": None,
        "support_contacts": (
            [] if support_object_id is None else [{"support_object_id": support_object_id}]
        ),
    }


class _SceneTransport:
    def __init__(self, *, populated: bool = False) -> None:
        self.owner_epoch = "scene-owner.epoch.1"
        self.revision = {
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "scene_revision": {
                "scene_id": _DIGEST_D,
                "revision_id": 1,
                "content_hash": _DIGEST_E,
            },
        }
        self.objects: dict[str, dict[str, Any]] = {}
        self.attachments: list[dict[str, Any]] = []
        if populated:
            self.objects = {
                "old-part": _fixed_scene_object("old-part", support_object_id="old-support"),
                "old-support": _fixed_scene_object("old-support"),
            }
            self.objects["old-part"]["placement"] = {
                "kind": "carried",
                "carried_object_attachment_id": "attachment.old",
                "carried_object_attachment_revision": _DIGEST_F,
            }
            self.attachments = [
                {
                    "carried_object_attachment_id": "attachment.old",
                    "carried_object_attachment_revision": _DIGEST_F,
                    "scene_resource_id": "old-part",
                    "controllable_component_id": "robot.1",
                    "command_target_id": "arm.command",
                    "carrier_frame_ref": {
                        "kind": "component",
                        "controllable_component_id": "robot.1",
                        "model_instance_id": "robot.1.model",
                        "frame_role_id": "tcp",
                    },
                    "pose_in_carrier_frame": {
                        "position_m": {"x": 0.0, "y": 0.0, "z": 0.1},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "touch_body_roles": ["tool_collision"],
                }
            ]
        self.submissions: list[dict[str, Any]] = []
        self.admission_generation = 1

    def authoring_admission(self) -> dict[str, Any]:
        return {
            "authoring_allowed": True,
            "admission_generation": self.admission_generation,
        }

    def scene(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "projection_generation": self.revision["scene_revision"]["revision_id"],
            "logical_scene": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "payload": {
                    "api_version": "manyforge.logical_scene_source.v1",
                    "access": {
                        "active_projection": deepcopy(self.revision),
                        "phase": "active",
                        "projection_leases_open": True,
                        "scene_writes_open": True,
                    },
                    "snapshot": {
                        "cell_id": "cell-a",
                        "revision": deepcopy(self.revision["scene_revision"]),
                        "transaction_sequence": len(self.submissions),
                        "topology_epoch": 1,
                        "policy_epoch": 1,
                        "configured_collision_policy": {},
                        "objects": [deepcopy(self.objects[key]) for key in sorted(self.objects)],
                        "carried_object_attachments": deepcopy(self.attachments),
                    },
                },
                "projection_generation": 1,
                "receiver_age_ns": 1,
                "source_content_digest": _DIGEST_A,
                "source_generation": 1,
                "source_observed_at_ns": 1,
                "source_owner_epoch": self.owner_epoch,
                "source_owner_id": "logical-scene.cell-a",
                "source_owner_kind": "logical_scene",
            },
            "scene_runtime": {},
        }

    def manager_graph(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.manager_graph_projection.v1",
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
            },
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "cell_graph": {
                "controllable_components": [
                    {"controllable_component_id": "pedestal.1"},
                    {"controllable_component_id": "robot.1"},
                ],
                "component_attachments": [
                    {
                        "component_attachment_id": "robot-on-pedestal",
                        "component_attachment_revision": _DIGEST_A,
                        "parent": {
                            "controllable_component_id": "pedestal.1",
                            "attachment_interface_id": "top",
                        },
                        "child": {
                            "controllable_component_id": "robot.1",
                            "attachment_interface_id": "base",
                        },
                    }
                ],
            },
        }

    def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
        request = deepcopy(dict(body))
        assert request["expected_projection_revision"] == self.revision
        self.submissions.append(request)
        operation = request["operation"]
        kind = operation["kind"]
        if kind == "detach_carried_object_at_current_pose":
            self.attachments = [
                row
                for row in self.attachments
                if row["carried_object_attachment_id"] != operation["carried_object_attachment_id"]
            ]
            self.objects[operation["expected_scene_resource_id"]]["placement"] = (
                _fixed_scene_object(operation["expected_scene_resource_id"])["placement"]
            )
        elif kind == "remove_object":
            self.objects.pop(operation["scene_resource_id"])
        elif kind in {"add_object", "upsert_object"}:
            frame_ref = operation["frame_ref"]
            shape = operation["shape"]
            # Match the public native Scene snapshot, not the command DTO.
            assert shape["type"] == "box"
            scene_object = {
                "scene_resource_id": operation["scene_resource_id"],
                "collision_shapes": [{
                    "type": "box",
                    "dimensions_m": {
                        axis: shape["box_dimensions_m"][f"{axis}_m"] for axis in "xyz"
                    },
                }],
                "placement": {
                    "kind": "cell_fixed",
                    "cell_id": "cell-a",
                    "pose_in_cell": {
                        "position_m": {
                            axis: operation["pose"]["position_m"][f"{axis}_m"] for axis in "xyz"
                        },
                        "orientation_xyzw": [
                            operation["pose"]["orientation"][axis] for axis in "xyzw"
                        ],
                    },
                },
                "provider_evidence": None,
                "support_contacts": deepcopy(operation["support_contacts"]),
            }
            assert frame_ref["kind"] in {"cell_frame", "component_frame"}
            self.objects[operation["scene_resource_id"]] = scene_object
        elif kind == "update_object_pose":
            pass
        elif kind == "attach_carried_object":
            attachment_revision = _DIGEST_F
            self.attachments.append(
                {
                    "carried_object_attachment_id": operation["carried_object_attachment_id"],
                    "carried_object_attachment_revision": attachment_revision,
                    "scene_resource_id": operation["scene_resource_id"],
                    "controllable_component_id": operation["controllable_component_id"],
                    "command_target_id": operation["command_target_id"],
                    "carrier_frame_ref": {},
                    "pose_in_carrier_frame": {},
                    "touch_body_roles": operation["touch_body_roles"],
                }
            )
            self.attachments.sort(key=lambda row: row["carried_object_attachment_id"])
            self.objects[operation["scene_resource_id"]]["placement"] = {
                "kind": "carried",
                "carried_object_attachment_id": operation["carried_object_attachment_id"],
                "carried_object_attachment_revision": attachment_revision,
            }
        else:
            raise AssertionError(f"unexpected scene operation {kind}")
        self.revision["scene_revision"] = {
            "scene_id": _DIGEST_D,
            "revision_id": self.revision["scene_revision"]["revision_id"] + 1,
            "content_hash": "sha256:" + f"{len(self.submissions):064x}"[-64:],
        }
        if kind in {
            "add_object",
            "upsert_object",
            "remove_object",
            "update_object_pose",
            "set_collision_policy",
        }:
            assert request["expected_admission_generation"] == (self.admission_generation)
            self.admission_generation += 2
        context = request["request_context"]
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "committed_projection_revision": deepcopy(self.revision),
            "error": None,
            "ok": True,
            "projection_status": "committed",
            "replay_status": "accepted_new",
            "request_id": context["request_id"],
            "request_owner_epoch": context["request_owner_epoch"],
            "transaction_sequence": len(self.submissions),
            "update_status": "geometry_changed",
        }

    def scene_transaction_status(self, body):
        return {"api_version": "manyforge.manager_command_response.v1", "ok": True, "result": None}


def _scene_port(transport):
    def captured_node(identity, owner_api):
        return {
            "program_runner_owner_epoch": "runner.owner",
            "program_run_id": identity.program_run_id,
            "parent_program_cycle_claim_generation": 1,
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            "owner_api": owner_api,
        }

    return HeadlessSceneProgramPort(transport, program_node_evidence=captured_node)


def _scene_rejection(body, code, message):
    from manyforge_behavior.scene_client import SceneTransactionRejectedError

    context = body["request_context"]
    return SceneTransactionRejectedError(
        {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": False,
            "request_id": context["request_id"],
            "request_owner_epoch": context["request_owner_epoch"],
            "projection_status": "not_attempted",
            "committed_projection_revision": None,
            "error": {"code": code, "message": message, "details": {}},
            "replay_status": "accepted_new",
            "transaction_sequence": None,
            "update_status": None,
        }
    )


def _authored_scene() -> list[dict[str, Any]]:
    return [
        {
            "scene_resource_id": "graspable",
            "resource_roles": ["pick_object"],
            "shape": {
                "type": "box",
                "box_dimensions_m": [0.1, 0.1, 0.1],
            },
            "pose": {
                "frame_ref": {"kind": "cell_frame"},
                "position_m": [0.3, 0.0, 0.2],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
        },
        {
            "scene_resource_id": "table",
            "shape": {
                "type": "box",
                "box_dimensions_m": [1.0, 0.6, 0.02],
            },
            "pose": {
                "frame_ref": {
                    "kind": "component_frame",
                    "component_role": "robot",
                    "model_role": "arm_model",
                    "frame_role": "base",
                },
                "position_m": [0.5, 0.0, 0.4],
                "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
        },
    ]


def test_scene_initialization_adds_pick_resources_last() -> None:
    transport = _SceneTransport(populated=True)
    port = _scene_port(transport)

    port.prepare_initial_scene(_scene_context(), _authored_scene())

    assert [request["operation"]["kind"] for request in transport.submissions] == [
        "detach_carried_object_at_current_pose",
        "add_object",
        "add_object",
        "remove_object",
        "remove_object",
    ]
    assert [
        request["operation"].get("scene_resource_id") for request in transport.submissions[1:]
    ] == ["table", "graspable", "old-part", "old-support"]
    table = transport.submissions[1]["operation"]
    assert table["frame_ref"] == {
        "kind": "component_frame",
        "controllable_component_id": "robot.1",
        "model_instance_id": "robot.1.model",
        "frame_role_id": "base",
    }
    assert sorted(transport.objects) == ["graspable", "table"]


def test_scene_initialization_forwards_authored_support_contact_policy() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    scene = _authored_scene()
    support_contact = {
        "support_object_id": "table",
        "mode": "contact_release",
        "release_evidence_mode": "binary_contact",
        "enforce_max_start_penetration": False,
        "max_start_penetration_m": 0.0,
        "release_tolerance_m": 0.0,
        "min_release_progress_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": False,
        "allow_recontact": False,
    }
    scene[0]["metadata"] = {"support_contacts": [support_contact]}

    port.prepare_initial_scene(_scene_context(), scene)

    add_operations = [
        request["operation"]
        for request in transport.submissions
        if request["operation"]["kind"] in {"add_object", "upsert_object"}
    ]
    assert [operation["scene_resource_id"] for operation in add_operations] == [
        "table",
        "graspable",
    ]
    assert add_operations[1]["support_contacts"] == [support_contact]
    assert transport.objects["graspable"]["support_contacts"] == [support_contact]


def test_scene_initialization_retries_concurrent_pose_revision_without_losing_shape() -> None:
    class _ConcurrentPoseTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.rejected_requests: list[dict[str, Any]] = []
            self.injected_stale = False

        def _advance_pose_revision(self) -> None:
            revision_id = self.revision["scene_revision"]["revision_id"] + 1
            self.revision["scene_revision"] = {
                "scene_id": _DIGEST_D,
                "revision_id": revision_id,
                "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
            }

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if not self.injected_stale:
                self.injected_stale = True
                self.rejected_requests.append(deepcopy(dict(body)))
                self._advance_pose_revision()
                raise _scene_rejection(
                    body, "stale_revision", "external pose update advanced the scene"
                )
            return super().commit_scene_transaction(body)

    transport = _ConcurrentPoseTransport()
    port = _scene_port(transport)

    port.prepare_initial_scene(_scene_context(), _authored_scene())

    assert transport.injected_stale is True
    assert sorted(transport.objects) == ["graspable", "table"]
    assert (
        transport.rejected_requests[0]["request_context"]["request_id"]
        != transport.submissions[0]["request_context"]["request_id"]
    )


def test_scene_initialization_retries_no_effect_admission_race_with_new_identity() -> None:
    class _AdmissionRaceTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if not self.rejected_requests:
                self.rejected_requests.append(deepcopy(dict(body)))
                self.admission_generation += 1
                raise _scene_rejection(
                    body, "authoring_not_ready", "Idle admission projection advanced"
                )
            return super().commit_scene_transaction(body)

    transport = _AdmissionRaceTransport()
    port = _scene_port(transport)

    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])

    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    rejected = transport.rejected_requests[0]
    committed = transport.submissions[0]
    assert rejected["expected_admission_generation"] == 1
    assert committed["expected_admission_generation"] == 2
    assert rejected["request_context"]["request_id"] != committed["request_context"]["request_id"]


def test_scene_initialization_upserts_only_the_changed_authored_object() -> None:
    transport = _SceneTransport()
    scene = [_authored_scene()[0]]
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), scene)
    transport.submissions.clear()
    transport.objects["graspable"]["placement"]["pose_in_cell"]["position_m"] = {
        "x": 0.8,
        "y": 0.1,
        "z": 0.4,
    }
    revision_id = transport.revision["scene_revision"]["revision_id"] + 1
    transport.revision["scene_revision"] = {
        "scene_id": _DIGEST_D,
        "revision_id": revision_id,
        "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
    }

    port.prepare_initial_scene(_scene_context(), scene)

    assert len(transport.submissions) == 1
    assert transport.submissions[0]["operation"]["kind"] == "upsert_object"
    assert transport.submissions[0]["operation"]["scene_resource_id"] == "graspable"
    assert transport.objects["graspable"]["placement"]["pose_in_cell"] == {
        "position_m": {"x": 0.3, "y": 0.0, "z": 0.2},
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }


@pytest.mark.parametrize(
    ("authored_shape", "published_shape"),
    [
        ({"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
         {"type": "box", "dimensions_m": {"x": 0.1, "y": 0.1, "z": 0.1}}),
        ({"type": "sphere", "sphere_radius_m": 0.1},
         {"type": "sphere", "radius_m": 0.1}),
        ({"type": "cylinder", "cylinder_radius_m": 0.1, "cylinder_height_m": 0.2},
         {"type": "cylinder", "radius_m": 0.1, "height_m": 0.2}),
        ({"type": "mesh", "mesh_resource_uri": "asset://part.stl", "mesh_scale": [1, 2, 3]},
         {"type": "mesh", "resource_uri": "asset://part.stl", "scale": {"x": 1, "y": 2, "z": 3}}),
    ],
)
def test_exact_cell_frame_initial_scene_is_a_read_only_fast_path(
    authored_shape, published_shape,
) -> None:
    transport = _SceneTransport()
    scene = [_authored_scene()[0]]
    scene[0]["shape"] = authored_shape
    # Activation already installed the Scene. A fresh port has no cached proof
    # and must compare the native publication without submitting any writes.
    obj = _fixed_scene_object("graspable")
    obj["collision_shapes"] = [published_shape]
    obj["placement"]["pose_in_cell"]["position_m"] = {"x": 0.3, "y": 0.0, "z": 0.2}
    transport.objects["graspable"] = obj
    replacement = _scene_port(transport)
    proof = replacement.prepare_initial_scene(_scene_context(), scene)

    assert transport.submissions == []
    replacement.verify_initial_scene(_scene_context(), scene, proof)


def test_prepared_scene_verification_rejects_an_intervening_owner_revision() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    context = _scene_context()
    scene = [_authored_scene()[0]]
    proof = port.prepare_initial_scene(context, scene)
    revision_id = transport.revision["scene_revision"]["revision_id"] + 1
    transport.revision["scene_revision"] = {
        "scene_id": _DIGEST_D,
        "revision_id": revision_id,
        "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
    }

    with pytest.raises(ProductionPortError, match="changed before Program capture"):
        port.verify_initial_scene(context, scene, proof)


def test_scene_context_can_bind_atomically_before_initialization_and_release() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    context = _scene_context()

    port.bind(context)
    port.prepare_initial_scene(context, _authored_scene())
    first_submission_count = len(transport.submissions)
    port.prepare_initial_scene(context, _authored_scene())
    assert len(transport.submissions) == first_submission_count

    port.release(context.program_run_id)
    result = port.invoke(
        "scene.remove_object",
        _identity("after-release"),
        OwnerNodeInputs(parameters={"scene_resource_id": "graspable"}),
    )
    assert result.succeeded is False
    assert "not initialized" in result.message


def test_lost_scene_reply_is_observed_while_held_without_replaying_the_action() -> None:
    class LostReply(_SceneTransport):
        lose_reply = False
        receipt_ready = False
        receipt = None

        def commit_scene_transaction(self, body):
            result = super().commit_scene_transaction(body)
            if self.lose_reply:
                self.receipt = result
                raise TimeoutError("accepted transaction reply was lost")
            return result

        def scene_transaction_status(self, body):
            assert body == self.submissions[-1]
            return {
                "api_version": "manyforge.manager_command_response.v1",
                "ok": True,
                "result": self.receipt if self.receipt_ready else None,
            }

    transport = LostReply()
    port = _scene_port(transport)
    context = _scene_context()
    port.prepare_initial_scene(context, _authored_scene())
    port.bind(context)
    before = len(transport.submissions)
    transport.lose_reply = True
    action = OwnerInvocationAdapter(
        name="remove",
        node_id="scene.remove_object",
        node_instance_id="remove",
        port=port,
        parameters={"scene_resource_id": "graspable"},
        value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(
            snapshot=lambda **kwargs: SimpleNamespace(
                parameters=kwargs["parameters"], blackboard={}, embedded_values={}
            )
        ),
    )
    next_action = py_trees.behaviours.Success("next")
    state = OperatorMotionStopState({"remove": (), "next": (), "root": ("remove", "next")})
    leaves = [
        OperatorMotionStopPolicy(child=child, node_instance_id=child.name, stop_state=state)
        for child in (action, next_action)
    ]
    root = OperatorMotionStopPolicy(
        child=py_trees.composites.Sequence("sequence", memory=True, children=leaves),
        node_instance_id="root",
        stop_state=state,
        drain_children=True,
    )
    list(root.tick())
    assert action.has_active_task
    assert "graspable" not in transport.objects
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"remove", "next", "root"})))
    assert action.reconcile_full_stop_task(selected_resume=True) == "active"
    list(root.tick())
    assert next_action.status is py_trees.common.Status.INVALID
    transport.receipt_ready = True
    assert action.reconcile_full_stop_task(selected_resume=True) == "settled"
    list(root.tick())
    assert leaves[0].presented_status is py_trees.common.Status.SUCCESS
    assert next_action.status is py_trees.common.Status.INVALID
    state.publish(OperatorMotionStopSnapshot(2))
    list(root.tick())
    assert root.status is py_trees.common.Status.SUCCESS
    assert len(transport.submissions) == before + 1


@pytest.mark.parametrize("lose_reply", [False, True])
def test_scene_rejected_by_stop_stays_unstarted_until_a_newer_release(lose_reply) -> None:
    class StopWins(_SceneTransport):
        held = False
        rejections = []
        receipt = None

        def commit_scene_transaction(self, body):
            if not self.held:
                return super().commit_scene_transaction(body)
            self.rejections.append(deepcopy(body))
            error = _scene_rejection(body, "invalid_state_transition", "held by Stop")
            error.response["error"]["details"] = {
                "reason": "program_node_stop_fence_active",
                "selection_revision": "1",
            }
            self.receipt = error.response
            if lose_reply:
                raise TimeoutError("rejection reply lost")
            raise error

        def scene_transaction_status(self, body):
            assert body == self.rejections[-1]
            return {
                "api_version": "manyforge.manager_command_response.v1",
                "ok": True,
                "result": self.receipt,
            }

    transport = StopWins()
    port = _scene_port(transport)
    context = _scene_context()
    port.prepare_initial_scene(context, _authored_scene())
    port.bind(context)
    before = len(transport.submissions)
    transport.held = True
    scope = OperatorMotionStopSnapshot(1, frozenset({"remove"}))
    state = OperatorMotionStopState({"remove": ()}, read_scope=lambda: scope)
    attempts = []

    def begin(node):
        identity = _identity(f"{node}:{len(attempts)}")
        identity = replace(identity, node_instance_id=node)
        attempts.append(identity)
        return identity

    action = OwnerInvocationAdapter(
        name="remove",
        node_id="scene.remove_object",
        node_instance_id="remove",
        port=port,
        parameters={"scene_resource_id": "graspable"},
        value_sources={},
        operator_stop_state=state,
        attempt_source=SimpleNamespace(begin=begin),
        input_resolver=SimpleNamespace(
            snapshot=lambda **kwargs: SimpleNamespace(
                parameters=kwargs["parameters"], blackboard={}, embedded_values={}
            )
        ),
    )
    list(action.tick())
    if lose_reply:
        action.observe_while_operator_stop_held()
    assert not action.has_active_task
    assert action.presented_status is py_trees.common.Status.INVALID
    assert action.failure_reason_code is None
    for _ in range(3):
        list(action.tick())  # Even an explicit continue policy cannot resubmit.
    assert len(transport.rejections) == 1
    assert "graspable" in transport.objects
    assert action.reconcile_full_stop_task(selected_resume=True) == "inactive"
    scope = OperatorMotionStopSnapshot(2)
    state.publish(scope)
    transport.held = False
    list(action.tick())
    assert action.status is py_trees.common.Status.SUCCESS
    assert len(transport.submissions) == before + 1
    assert len(attempts) == 2 and attempts[0] != attempts[1]
    assert (
        transport.submissions[-1]["request_context"]["request_id"]
        != transport.rejections[0]["request_context"]["request_id"]
    )


def test_three_production_context_ports_rollback_as_one_before_dispatch() -> None:
    class _NoModelTransport:
        def validate_endpoint(self, **_endpoint: Any) -> None:
            pass

        def complete(self, _request: object, _cancel_event: object) -> object:
            raise AssertionError("model transport must not be reached")

    base = _scene_context()
    context = ProgramRunPortContext(
        program_run_id=base.program_run_id,
        applied_record_digest=base.applied_record_digest,
        scenario_id=base.scenario_id,
        source_document_digest=base.source_document_digest,
        program_id=base.program_id,
        program_revision=base.program_revision,
        program_binding_id=base.program_binding_id,
        retained_bundle_id=base.retained_bundle_id,
        graph_revision=base.graph_revision,
        node_environment_digest=base.node_environment_digest,
        program_binding=base.program_binding,
        program={
            "program_id": base.program_id,
            "program_revision": base.program_revision,
            "blackboard": {"namespaces": [], "keys": []},
            "tree": {
                "node_id": "assistant.query_model",
                "instance_id": "query",
                "parameters": {},
                "children": [],
            },
        },
    )
    kernel_transport = _Transport()
    scene_transport = _SceneTransport()
    native = NativeKernelProgramPort(KernelClient(kernel_transport))
    scene = _scene_port(scene_transport)
    assistant = ProgramAssistantPort(
        _NoModelTransport(),  # type: ignore[arg-type]
        HeadlessProgramSceneSource(scene_transport),
    )
    composite = CompositeProgramRunContextPort(native, scene, assistant)
    try:
        with pytest.raises(RuntimeError, match="no retained assistant"):
            composite.bind(context)

        with pytest.raises(ProductionPortError, match="not bound"):
            native.operations.start(
                "motion.move_to_pose",
                _identity("after-rollback"),
                _motion_inputs(),
            )
        scene_result = scene.invoke(
            "scene.remove_object",
            _identity("scene-after-rollback"),
            OwnerNodeInputs(parameters={"scene_resource_id": "missing"}),
        )
        assert scene_result.succeeded is False
        assert "not initialized" in scene_result.message
    finally:
        assistant.close()


def test_scene_nodes_translate_roles_and_detach_at_current_pose() -> None:
    transport = _SceneTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])
    transport.submissions.clear()

    attached = port.invoke(
        "scene.attach_carried_object",
        _identity("attach"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )
    detached = port.invoke(
        "scene.detach_carried_object",
        _identity("detach"),
        OwnerNodeInputs(parameters={"scene_resource_id": "graspable", "target_role": "arm"}),
    )

    assert attached.succeeded is True
    attach = transport.submissions[0]["operation"]
    assert attach["controllable_component_id"] == "robot.1"
    assert attach["command_target_id"] == "arm.command"
    assert attach["expected_component_attachment_revisions"] == [_DIGEST_A]
    assert detached.succeeded is True
    detach = transport.submissions[1]["operation"]
    assert detach["kind"] == "detach_carried_object_at_current_pose"
    assert detach["expected_scene_resource_id"] == "graspable"
    assert transport.attachments == []


def test_scene_node_failure_keeps_the_owner_reason() -> None:
    class _UnavailableStateTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.reject_detach = False

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if (
                self.reject_detach
                and body["operation"]["kind"] == "detach_carried_object_at_current_pose"
            ):
                error = _scene_rejection(
                    body, "invalid_request", "addressed observed state is unavailable"
                )
                error.response["error"]["details"]["reason"] = (
                    "observed_state_unavailable_at_evaluation_time"
                )
                raise error
            return super().commit_scene_transaction(body)

    transport = _UnavailableStateTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])
    attached = port.invoke(
        "scene.attach_carried_object",
        _identity("attach"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )
    assert attached.succeeded is True
    transport.reject_detach = True

    detached = port.invoke(
        "scene.detach_carried_object",
        _identity("detach"),
        OwnerNodeInputs(parameters={"scene_resource_id": "graspable", "target_role": "arm"}),
    )

    assert detached.succeeded is False
    assert detached.reason_code == "invalid_request"
    assert detached.message == (
        "addressed observed state is unavailable "
        "(observed_state_unavailable_at_evaluation_time)"
    )


@pytest.mark.parametrize(
    "code,reason,retry",
    [
        ("stale_revision", "scene_advanced", True),
        ("stale_generation", "expected_generation_stale", True),
        ("stale_generation", "claim_owner_epoch_stale", False),
    ],
)
def test_scene_node_retries_a_concurrent_revision_with_one_correlation(
    code, reason, retry
) -> None:
    class _ConcurrentPoseTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.reject_next_node = False
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if self.reject_next_node:
                self.reject_next_node = False
                self.rejected_requests.append(deepcopy(dict(body)))
                revision_id = self.revision["scene_revision"]["revision_id"] + 1
                self.revision["scene_revision"] = {
                    "scene_id": _DIGEST_D,
                    "revision_id": revision_id,
                    "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
                }
                self.admission_generation += 1
                error = _scene_rejection(
                    body, code, "concurrent owner revision advanced"
                )
                error.response["error"]["details"]["reason"] = reason
                raise error
            return super().commit_scene_transaction(body)

    transport = _ConcurrentPoseTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])
    transport.submissions.clear()
    transport.reject_next_node = True

    result = port.invoke(
        "scene.attach_carried_object",
        _identity("attach-after-pose-update"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )

    if not retry:
        assert not result.succeeded
        assert result.reason_code == code
        assert len(transport.rejected_requests) == 1
        assert not transport.submissions
        return
    assert result.succeeded is True
    assert result.metadata["stale_revision_retry_count"] == 1
    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    rejected_context = transport.rejected_requests[0]["request_context"]
    committed_context = transport.submissions[0]["request_context"]
    assert rejected_context["request_id"] != committed_context["request_id"]
    assert rejected_context["correlation_id"] == committed_context["correlation_id"]
    assert transport.submissions[0]["expected_admission_generation"] == (
        transport.rejected_requests[0]["expected_admission_generation"] + 1
    )


def test_scene_resource_relative_pose_is_forwarded_to_the_scene_owner() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), _authored_scene())
    transport.submissions.clear()

    result = port.invoke(
        "scene.update_object_pose",
        _identity("relative-update"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "pose": {
                    "frame_ref": {
                        "kind": "scene_resource",
                        "scene_resource_id": "table",
                    },
                    "position_m": [0.1, 0.2, 0.3],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
            }
        ),
    )

    assert result.succeeded is True
    operation = transport.submissions[0]["operation"]
    assert operation["frame_ref"] == {
        "kind": "scene_resource",
        "scene_resource_id": "table",
    }
    assert operation["pose"]["position_m"] == {
        "x_m": pytest.approx(0.1),
        "y_m": pytest.approx(0.2),
        "z_m": pytest.approx(0.3),
    }


def test_stale_scene_projection_fails_before_a_node_commit() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [])
    transport.submissions.clear()
    transport.revision["graph_revision"] = _DIGEST_F

    result = port.invoke(
        "scene.remove_object",
        _identity("remove"),
        OwnerNodeInputs(parameters={"scene_resource_id": "missing"}),
    )

    assert result.succeeded is False
    assert "captured Program deployment" in result.message
    assert transport.submissions == []
