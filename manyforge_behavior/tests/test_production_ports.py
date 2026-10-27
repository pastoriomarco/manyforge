"""Captured Program operation ports remain thin, exact native clients."""

from __future__ import annotations

import json
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
from manyforge_behavior.kernel_client import (
    KernelOperationObservationUnavailableError,
    KernelClient,
    KernelClientResponseError,
    KernelOperationAdmissionRejectedError,
)
from manyforge_behavior.orchestration_ports import (
    OwnerTaskSettlementPendingError,
    OwnerTaskState,
    PoseSourceLatchError,
)
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter, OwnerTaskAdapter
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    FrameTransformError,
    NodeAttemptIdentity,
    OwnerInvocationHeldError,
    OwnerInvocationRefusedByPriorityError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSettlementPendingError,
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
    ProgramMotionPreviewUnavailableError,
    SimulationResetPoseSourceTaskPort,
    _owner_failure_message,
    program_motion_participant,
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
        self.stop_fault_offer: Mapping[str, Any] | None = None
        self.released_scope_stops: list[tuple[str, str, int]] = []
        self.identity_reads = 0
        self.owner_epoch = "kernel.epoch.7"

    def targets(self):
        # Numeric commands now share the real descriptor prerequisite. The
        # default fixture remains a configured position gripper.
        _preset_transport(self, kind="linear_m")
        return self.targets()

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

    def stop_fault(self):
        return {
            "ok": True,
            "command": "get_stop_fault",
            "stop_fault": deepcopy(self.stop_fault_offer),
            "global_rule_recovery_offers": [],
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
        ("kernel.reset_operation", "stopped"),
    ],
)
def test_control_node_submits_once_then_observes_owner_outcome(node, state):
    owner, transport = _control_port()
    identity = NodeAttemptIdentity("program-run.1", "control", "attempt.1")
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
            "kernel.reset_operation",
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
            "kernel.reset_operation", identity, OwnerNodeInputs({"target_role": "arm"})
        )
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
                    "controllable_component_id": "robot.1",
                    "command_target_id": "arm.command",
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
                    "controllable_component_id": "tool.1",
                    "command_target_id": "gripper.command",
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
            "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"},
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
    assert transport.identity_reads == 1  # bind only, not a second read per query
    assert transport.submissions == []
    assert transport.controls == []


def test_relative_pose_submission_uses_capture_and_preserves_portable_roles() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())

    assert started.state is OwnerTaskState.ACTIVE
    request = transport.submissions[0]
    assert request["program_id"] == "lite6_program"
    assert request["program_binding_id"] == _DIGEST_A
    assert request["expected_scene_revision"] is None
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


_ARM = {
    "target_role_id": "arm",
    "controllable_component_id": "robot.1",
    "command_target_id": "arm.command",
}
_GRIPPER = {
    "target_role_id": "gripper",
    "controllable_component_id": "tool.1",
    "command_target_id": "gripper.command",
}


def _absolute_inputs(node_id: str, frames: list[Any]) -> OwnerNodeInputs:
    parameters = dict(_motion_inputs().parameters)
    parameters.update(target_mode="absolute", target_reference=None, offset_reference=None)

    def pose(frame: Any) -> dict[str, Any]:
        value = {"position_m": [0.1, 0.0, 0.2], "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]}}
        return value if frame is None else {**value, "frame_ref": frame}

    if node_id == "motion.move_to_pose":
        parameters["goal"] = pose(frames[0])
    else:
        parameters.pop("goal")
        parameters.update(
            path=[pose(frame) for frame in frames],
            max_translation_step_m=0.005,
            max_rotation_step_rad=0.05,
            max_angular_speed_rad_s=0.5,
            max_linear_acceleration_mps2=0.5,
            max_angular_acceleration_rad_s2=2.5,
            fallback_policy="fail_closed",
            max_blend_deviation_rad=0.01,
        )
    return OwnerNodeInputs(parameters=parameters)


_BASE = {"kind": "target_frame", "target_role": "arm", "frame_role": "base"}


@pytest.mark.parametrize(
    ("frame", "expected"),
    [
        (None, None),
        (_BASE, None),
        ({"kind": "cell_frame"}, {"type": "cell_frame"}),
        (
            {"kind": "scene_resource", "scene_resource_id": "fixture"},
            {"type": "scene_resource", "scene_resource_id": "fixture"},
        ),
        (
            {"kind": "target_frame", "target_role": "arm", "frame_role": "tcp"},
            {"type": "frame_role", "frame_role_id": "tcp", "target": _ARM},
        ),
        (
            {"kind": "target_frame", "target_role": "gripper", "frame_role": "tcp"},
            {"type": "frame_role", "frame_role_id": "tcp", "target": _GRIPPER},
        ),
    ],
)
@pytest.mark.parametrize("node_id", ["motion.move_to_pose", "motion.follow_cartesian_path"])
def test_absolute_goal_frame_is_forwarded_as_one_goal_frame_reference(
    node_id, frame, expected
) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    frames = [frame] if node_id == "motion.move_to_pose" else [frame, frame]

    port.operations.start(node_id, _identity(), _absolute_inputs(node_id, frames))

    payload = transport.submissions[0]["participants"][0]["payload"]
    assert payload["target_mode"] == "absolute"
    assert payload.get("goal_frame_reference") == expected
    assert ("goal_frame_reference" in payload) is (expected is not None)
    # The goal coordinates are forwarded unchanged; the owner resolves the frame.
    goal = payload["motion"]["goal"] if node_id == "motion.move_to_pose" else (
        payload["motion"]["waypoints"][0]
    )
    assert goal["position_m"] == {"x_m": 0.1, "y_m": 0.0, "z_m": 0.2}


@pytest.mark.parametrize(
    ("node_id", "frames", "target_mode", "message"),
    [
        (
            "motion.follow_cartesian_path",
            [{"kind": "cell_frame"}, _BASE],
            "absolute",
            "waypoints must all use one frame",
        ),
        ("motion.move_to_pose", [{"kind": "cell_frame"}], "relative", "relative goal"),
        (
            "motion.move_to_pose",
            [{"kind": "target_frame", "target_role": "ghost", "frame_role": "tcp"}],
            "absolute",
            "absent from the Program Binding",
        ),
    ],
)
def test_goal_frames_the_owner_cannot_take_fail_before_submission(
    node_id, frames, target_mode, message
) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = _absolute_inputs(node_id, frames)
    if target_mode == "relative":
        inputs = OwnerNodeInputs(
            parameters={
                **inputs.parameters,
                "target_mode": "relative",
                "target_reference": {"type": "cell_frame"},
            }
        )

    with pytest.raises(ProductionPortError, match=message):
        port.operations.start(node_id, _identity(), inputs)

    assert transport.submissions == []


def test_reference_names_another_robot_frame_or_the_cell_by_binding_identity() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = OwnerNodeInputs(
        parameters={
            **_motion_inputs().parameters,
            "target_reference": {
                "type": "frame_role",
                "frame_role_id": "tcp",
                "target_role": "gripper",
            },
            "offset_reference": {"type": "cell_frame"},
        }
    )

    port.operations.start("motion.move_to_pose", _identity(), inputs)

    payload = transport.submissions[0]["participants"][0]["payload"]
    assert payload["target_reference"] == {
        "type": "frame_role",
        "frame_role_id": "tcp",
        "target": _GRIPPER,
    }
    assert payload["offset_reference"] == {"type": "cell_frame"}
    assert "goal_frame_reference" not in payload


def test_frame_transform_query_forwards_both_frames_and_names_owner_refusals() -> None:
    queries: list[dict[str, Any]] = []

    class TransformTransport(_Transport):
        refusal: str | None = None

        def query_target(self, query):
            queries.append(deepcopy(query))
            if self.refusal is not None:
                raise KernelOperationAdmissionRejectedError(
                    "missing_reference", "robot is hosted elsewhere", {"reason": self.refusal}
                )
            return {
                "ok": True,
                "target_observation": {
                    **query,
                    "schema_version": "manyforge.target_observation.v1",
                    "evaluated_at_ns": 12,
                    "scene_revision": {"scene_id": _DIGEST_A, "revision_id": 4, "content_hash": _DIGEST_B},
                    "resolution_evidence_digests": [_DIGEST_C],
                    "value": {
                        "position_m": {"x_m": 1.0, "y_m": 2.0, "z_m": 3.0},
                        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                    },
                },
            }

    transport = TransformTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    pose = {
        "frame_ref": {"kind": "target_frame", "target_role": "gripper", "frame_role": "tcp"},
        "position_m": [0.0, 0.0, 0.1],
        "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
    }

    result = port.queries.express_pose(_identity("bounds"), pose, {"kind": "cell_frame"})

    assert queries == [
        {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": "kernel.epoch.7",
            "program_binding_id": _DIGEST_A,
            "kind": "frame_transform",
            "pose": {
                "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.1},
                "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
            },
            "source_frame": {"type": "frame_role", "frame_role_id": "tcp", "target": _GRIPPER},
            "target_frame": {"type": "cell_frame"},
        }
    ]
    assert result.pose == {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [1.0, 2.0, 3.0],
        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }
    assert result.metadata["evaluated_at_ns"] == 12
    transport.refusal = "program_reference_robot_not_local"
    with pytest.raises(FrameTransformError) as refused:
        port.queries.express_pose(_identity("bounds"), pose, {"kind": "cell_frame"})
    assert refused.value.reason_code == "program_reference_robot_not_local"
    assert transport.submissions == []


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


def test_partial_stop_reset_requires_resident_operations_to_be_reset_first() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity("partial-reset"), _motion_inputs()
    )
    transport.operations[started.task_id]["state"] = "executing"
    port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
    transport.stop_states["scope-stop-1"] = "partial"

    with pytest.raises(
        ProductionPortError, match="cannot reset a Program run with active native operations"
    ):
        port.scope.reset("program-run.1")

    assert transport.resident[started.task_id] is True
    assert not [control for control in transport.controls if control[0] == "reset"]


def test_partial_stop_reset_accepts_exact_generation_after_intervention_is_retired() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity("partial-reset"), _motion_inputs()
    )
    transport.operations[started.task_id].update(
        {"state": "intervention_required", "operation_revision": 4}
    )
    port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
    transport.stop_states["scope-stop-1"] = "partial"

    reset = port.scope.reset("program-run.1")

    assert reset.state is ProgramRunScopeState.SETTLED
    assert transport.resident[started.task_id] is False
    assert [control[0] for control in transport.controls].count("reset") == 1
    assert not transport.released_scope_stops


def test_partial_stop_reset_keeps_intervention_task_and_fence_on_nonquiescent_refusal() -> None:
    transport = _Transport()
    refusal: dict[str, Any] = {}
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity("partial-not-quiescent"), _motion_inputs()
    )
    transport.operations[started.task_id].update(
        {"state": "intervention_required", "operation_revision": 4}
    )
    port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
    transport.stop_states["scope-stop-1"] = "partial"

    def refuse_nonquiescent(_operation_id, _body):
        transport.controls.append(("reset", started.task_id, {}))
        refusal.update({
            "ok": False,
            "error": {
                "code": "execution_rejected",
                "message": "prepared execution is not quiescent",
                "details": {"reason": "prepared_execution_not_quiescent"},
            },
        })
        return refusal

    transport.reset = refuse_nonquiescent
    with pytest.raises(
        KernelClientResponseError, match="did not return a successful envelope"
    ):
        port.scope.reset("program-run.1")

    assert refusal["error"]["details"]["reason"] == "prepared_execution_not_quiescent"
    assert transport.resident[started.task_id] is True
    assert port._task(started.task_id).task_id == started.task_id
    assert port.scope.capture_stop("program-run.1") == ("scope-stop-1", 1)
    assert not transport.released_scope_stops


def test_partial_stop_reset_refuses_active_fault_and_unknown_fault_observation() -> None:
    for fault_offer in ({"latch": {"latch_id": "stop-fault.1"}}, "missing-read"):
        transport = _Transport()
        port = NativeKernelProgramPort(KernelClient(transport))
        port.bind(_context())
        port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
        transport.stop_states["scope-stop-1"] = "partial"
        if fault_offer == "missing-read":
            transport.stop_fault = lambda: {"ok": True, "command": "get_stop_fault"}
        else:
            transport.stop_fault_offer = fault_offer

        with pytest.raises(ProductionPortError, match="Stop fault"):
            port.scope.reset("program-run.1")


def test_partial_stop_does_not_reconcile_unknown_admission() -> None:
    transport = _LostAcceptanceReplyTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.operations.start(
        "motion.move_to_pose", _identity("partial-unknown"), _motion_inputs()
    )
    port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
    transport.stop_states["scope-stop-1"] = "partial"

    with pytest.raises(
        ProductionPortError,
        match="unknown operation admission requires settled Program Stop",
    ):
        port.scope.reset("program-run.1")

    assert len(transport.submit_attempts) == 1
    assert not [control for control in transport.controls if control[0] == "reset"]


def test_partial_stop_cannot_release_resume_fence_or_accept_wrong_generation() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.scope.request_stop("program-run.1", "stop.partial", "Reset containment")
    transport.stop_states["scope-stop-1"] = "partial"

    with pytest.raises(ProductionPortError, match="cannot be released before settlement"):
        port.scope.release_stop("program-run.1")
    assert not transport.released_scope_stops

    transport.stop_states["scope-stop-1"] = "succeeded"
    status = transport.stop_status

    def wrong_generation(stop_id):
        response = status(stop_id)
        response["stop_status"]["stop_generation"] += 1
        return response

    transport.stop_status = wrong_generation
    with pytest.raises(ProductionPortError, match="settlement generation is stale"):
        port.scope.reset("program-run.1")


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


def test_owner_failures_name_the_reference_that_failed() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start("motion.move_to_pose", _identity(), _motion_inputs())
    operation = transport.operations[started.task_id]
    operation.update({"state": "failed", "operation_revision": 3})
    details = {
        "reason": "program_reference_robot_frame_missing",
        "reference_type": "frame_role",
        "reference_id": "tcp",
        "target_role_id": "gripper",
    }
    operation["participants"] = [
        {
            "terminal_error": {
                "code": "missing_reference",
                "message": "robot frame role is absent from the retained target map",
                "details": dict(details),
            }
        }
    ]

    snapshot = port.operations.observe(started.task_id)

    assert snapshot.state is OwnerTaskState.FAILED
    assert snapshot.reason_code == "missing_reference"
    assert snapshot.message == (
        "robot frame role is absent from the retained target map "
        "(program_reference_robot_frame_missing; "
        "reference frame_role 'tcp' of target role 'gripper')"
    )
    rejected = KernelOperationAdmissionRejectedError(
        "missing_reference",
        "Program reference names a scene resource absent from the retained snapshot",
        {
            "reason": "program_scene_resource_missing",
            "reference_type": "scene_resource",
            "reference_id": "bin",
        },
    )
    assert _owner_failure_message(rejected) == (
        "Program reference names a scene resource absent from the retained snapshot "
        "(program_scene_resource_missing; reference scene_resource 'bin')"
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


def _gripper_inputs(duration_s: float = 0.8) -> OwnerNodeInputs:
    return OwnerNodeInputs(parameters={
        "target_role": "gripper",
        "position": {"kind": "linear_m", "value_m": 0.008},
        "max_effort": {"kind": "linear_newton", "value_n": 10.0},
        "command_duration_s": duration_s,
        "completion_timeout_s": 30.0,
    })


def _final_pneumatic_failure(reason: str = "delivery_reply_lost") -> dict[str, Any]:
    return {
        "state": "failed",
        "terminal_error": {
            "code": "command_failed",
            "message": "pneumatic output was not confirmed as sent",
            "details": {"reason": reason, "pneumatic_transaction_final": "true"},
        },
    }


def _preset_transport(transport=None, *, kind="angular_rad"):
    transport = _Transport() if transport is None else transport
    schema = {"command_schema_id": "gripper.command_position", "command_schema_revision": _DIGEST_E}
    transport.preset_descriptor = {
        "schema_version": "manyforge.manual_control_gripper_descriptor.v3",
        "controllable_component_id": "tool.1", "command_target_id": "gripper.command",
        "component_role_id": "manual_component", "target_role_id": "manual_target",
        "source_kind": "model_role", "coordinate_kind": kind,
        "unit": "rad" if kind == "angular_rad" else "m",
        "min_position": 0.0, "max_position": 1.0,
        "open_state_ref": "released", "closed_state_ref": "grasp",
        "actuation_kind": "position_commanded", "output_mode": "commanded",
        "completion_policy": "goal_fenced_target_reached",
        "grasp_confirmation_policy": "binding_feedback_required",
        "command_kind": "gripper_position", "command_duration_s": 0.8,
        "description": "Fixture model-backed position gripper.",
        "mount_frame": "gripper_mount", "attach_frame": "gripper_tcp", "tcp_frame": "gripper_tcp",
        "touch_links": ["finger_link"], "auxiliary_frames": [], "mimic_followers": [],
        "source_coordinate_name": "finger_joint", "source_group_name": "finger_group",
        "state_authority_id": None, "state_authority_class": None,
        "control_interface_contract_id": None, "transport_kind": "resolved_binding",
        "contracts": {"command_schema": schema, "operation_capability_id": "tool.gripper.command",
                      "operation_capability_definition_revision": _DIGEST_E,
                      "control_path": None, "supporting_providers": []},
        "named_presets": [
            {"name": name, "master_position": value,
             "coordinates": [{"source_coordinate_name": "finger_joint", "position": value}]}
            for name, value in (("released", 0.8), ("grasp", 0.1), ("open", 0.2), ("closed", 0.3))
        ],
    }
    transport.preset_envelope = {
        "ok": True,
        "identity": {"owner_epoch": "kernel.epoch.7", "graph_revision": _DIGEST_C,
                     "retained_bundle_id": _DIGEST_B},
        "targets": {
            "schema_version": "manyforge.execution_targets.v3",
            "components": [{"controllable_component_id": "tool.1", "targets": [{
                "command_target_id": "gripper.command", "operation_capability_id": "tool.gripper.command",
                "presentation": "gripper", "command": {
                    **schema, "target_descriptor": transport.preset_descriptor,
                },
            }]}],
        },
    }
    transport.target_reads = 0

    def targets():
        transport.target_reads += 1
        return deepcopy(transport.preset_envelope)

    transport.targets = targets
    return transport


@pytest.mark.parametrize("actuation,mode,completion,confirmation", [
    ("position_commanded", "commanded", "goal_fenced_target_reached", "binding_feedback_required"),
    ("pneumatic", "commanded", "output_delivery", "not_required"),
    ("pneumatic", "none", "no_output", "not_required"),
])
@pytest.mark.parametrize("alias,value", [("open", 0.8), ("closed", 0.1)])
def test_named_gripper_alias_consumes_exact_v3_mode_policy_matrix(
    actuation, mode, completion, confirmation, alias, value,
):
    transport = _preset_transport(kind="linear_m")
    transport.preset_descriptor.update({
        "actuation_kind": actuation, "output_mode": mode,
        "completion_policy": completion, "grasp_confirmation_policy": confirmation,
    })
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    parameters["position"] = {"kind": "named_preset", "name": alias}
    port.operations.start("gripper.command_position", _identity(), OwnerNodeInputs(parameters=parameters))
    position = transport.submissions[0]["participants"][0]["payload"]["command_value"]["position"]
    assert position == {"kind": "linear_m", "value_m": value}
    assert transport.target_reads == 1
    # This adapter sends the existing numeric Kernel intent, including for
    # no-output targets. The exact backend owner decides output delivery.
    assert transport.controls == []


@pytest.mark.parametrize("case", [
    "old_version", "missing_mode", "null_mode", "unknown_mode", "position_none",
    "missing_actuation", "unknown_actuation", "missing_completion", "wrong_completion",
    "missing_confirmation", "wrong_confirmation", "unconfigured_model",
])
def test_named_gripper_alias_refuses_old_or_incomplete_v3_descriptor(case):
    transport = _preset_transport()
    descriptor = transport.preset_descriptor
    if case == "old_version":
        descriptor["schema_version"] = "manyforge.manual_control_gripper_descriptor.v2"
    elif case == "missing_mode":
        descriptor.pop("output_mode")
    elif case == "null_mode":
        descriptor["output_mode"] = None
    elif case == "unknown_mode":
        descriptor["output_mode"] = "off"
    elif case == "position_none":
        descriptor["output_mode"] = "none"
    elif case == "missing_actuation":
        descriptor.pop("actuation_kind")
    elif case == "unknown_actuation":
        descriptor["actuation_kind"] = "unknown"
    elif case == "missing_completion":
        descriptor.pop("completion_policy")
    elif case == "wrong_completion":
        descriptor["actuation_kind"] = "pneumatic"
        descriptor["grasp_confirmation_policy"] = "not_required"
    elif case == "missing_confirmation":
        descriptor.pop("grasp_confirmation_policy")
    elif case == "wrong_confirmation":
        descriptor["grasp_confirmation_policy"] = "not_required"
    else:
        for field in ("actuation_kind", "output_mode", "description", "command_duration_s",
                      "open_state_ref", "closed_state_ref", "mount_frame", "attach_frame", "tcp_frame"):
            descriptor[field] = None
        descriptor["touch_links"] = []
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    parameters["position"] = {"kind": "named_preset", "name": "open"}
    with pytest.raises(ProductionPortError):
        port.operations.start("gripper.command_position", _identity(), OwnerNodeInputs(parameters=parameters))
    assert transport.target_reads == 1
    assert transport.submissions == []
    assert transport.controls == []


@pytest.mark.parametrize("kind", ["angular_rad", "linear_m"])
@pytest.mark.parametrize("alias,value", [("open", 0.8), ("closed", 0.1)])
def test_named_gripper_alias_resolves_configured_reference_once_per_attempt(kind, alias, value):
    transport = _preset_transport(kind=kind)
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    parameters["position"] = {"kind": "named_preset", "name": alias}
    parameters["max_effort"] = (
        {"kind": "angular_newton_meter", "value_nm": 1.0}
        if kind == "angular_rad" else {"kind": "linear_newton", "value_n": 1.0}
    )
    identity = _identity("first-preset")
    task = port.operations.start("gripper.command_position", identity, OwnerNodeInputs(parameters=parameters))
    field = "value_rad" if kind == "angular_rad" else "value_m"
    assert transport.submissions[0]["participants"][0]["payload"]["command_value"]["position"] == {
        "kind": kind, field: value,
    }
    assert parameters["position"] == {"kind": "named_preset", "name": alias}
    assert transport.target_reads == 1
    transport.operations[task.task_id].update({"state": "completed", "operation_revision": 3})
    port.operations.observe(task.task_id)
    port.operations.acknowledge(task.task_id, identity)
    selected = next(row for row in transport.preset_descriptor["named_presets"] if
                    row["name"] == transport.preset_descriptor[f"{alias}_state_ref"])
    selected["master_position"] = 0.5
    selected["coordinates"][0]["position"] = 0.5
    port.operations.start("gripper.command_position", _identity("next-preset"), OwnerNodeInputs(parameters=parameters))
    assert transport.target_reads == 2
    assert transport.submissions[1]["participants"][0]["payload"]["command_value"]["position"] == {
        "kind": kind, field: 0.5,
    }


@pytest.mark.parametrize("case", [
    "missing_reference", "missing_preset", "duplicate_preset", "nonfinite", "out_of_limits",
    "unit", "source_kind", "target", "command_schema", "owner_epoch", "graph", "bundle",
    "duplicate_component", "duplicate_target",
])
def test_named_gripper_alias_refuses_unverified_metadata_before_command(case):
    transport = _preset_transport()
    descriptor = transport.preset_descriptor
    component = transport.preset_envelope["targets"]["components"][0]
    if case == "missing_reference":
        descriptor["open_state_ref"] = None
    elif case == "missing_preset":
        descriptor["named_presets"] = [row for row in descriptor["named_presets"] if row["name"] != "released"]
    elif case == "duplicate_preset":
        descriptor["named_presets"].append(deepcopy(descriptor["named_presets"][0]))
    elif case in {"nonfinite", "out_of_limits"}:
        descriptor["named_presets"][0]["master_position"] = float("nan") if case == "nonfinite" else 2.0
    elif case == "unit":
        descriptor["unit"] = "m"
    elif case == "source_kind":
        descriptor["source_kind"] = "component_local"
    elif case == "target":
        descriptor["command_target_id"] = "foreign"
    elif case == "command_schema":
        component["targets"][0]["command"]["command_schema_revision"] = _DIGEST_F
    elif case in {"owner_epoch", "graph", "bundle"}:
        field = {"owner_epoch": "owner_epoch", "graph": "graph_revision", "bundle": "retained_bundle_id"}[case]
        transport.preset_envelope["identity"][field] = "foreign" if case == "owner_epoch" else _DIGEST_F
    elif case == "duplicate_component":
        transport.preset_envelope["targets"]["components"].append(deepcopy(component))
    else:
        component["targets"].append(deepcopy(component["targets"][0]))
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    parameters["position"] = {"kind": "named_preset", "name": "open"}
    with pytest.raises(ProductionPortError):
        port.operations.start("gripper.command_position", _identity(), OwnerNodeInputs(parameters=parameters))
    assert transport.target_reads == 1
    assert transport.submissions == []
    assert transport.controls == []


def test_numeric_gripper_command_reads_descriptor_without_resolving_named_presets():
    transport = _preset_transport(kind="linear_m")
    # Numeric payloads do not consult aliases, even if presets are unavailable.
    transport.preset_descriptor["named_presets"] = None
    transport.preset_descriptor["open_state_ref"] = None
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert transport.target_reads == 1
    assert transport.submissions[0]["participants"][0]["payload"]["command_value"]["position"] == {
        "kind": "linear_m", "value_m": 0.008,
    }


def test_numeric_gripper_descriptor_unavailability_refuses_before_effect():
    transport = _preset_transport()
    transport.preset_envelope = None
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    with pytest.raises(Exception):
        port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert transport.target_reads == 1
    assert transport.submissions == []
    assert transport.controls == []


def test_named_gripper_keeps_resolved_epoch_for_owner_admission_cas():
    transport = _preset_transport()
    targets = transport.targets

    def change_owner_after_read():
        envelope = targets()
        transport.owner_epoch = "kernel.epoch.replaced"
        return envelope

    def refuse_old_owner(body):
        assert body["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
        assert body["request_context"]["request_owner_epoch"] != transport.owner_epoch
        return {"ok": False, "error": {
            "code": "invalid_state_transition", "message": "request owner was replaced",
            "details": {"reason": "request_owner_epoch_stale"},
        }}

    transport.targets, transport.submit = change_owner_after_read, refuse_old_owner
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    parameters["position"] = {"kind": "named_preset", "name": "open"}
    with pytest.raises(KernelOperationAdmissionRejectedError):
        port.operations.start("gripper.command_position", _identity(), OwnerNodeInputs(parameters=parameters))
    assert transport.target_reads == 1
    assert transport.submissions == []
    assert port._tasks == {}


@pytest.mark.parametrize("duration_s", [0.0, 0.35])
@pytest.mark.parametrize("succeeded", [True, False])
@pytest.mark.parametrize("composite", ["sequence", "fallback", "retry"])
def test_pneumatic_held_result_drains_without_successor_retry_or_resend(
    duration_s, succeeded, composite,
) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    state = OperatorMotionStopState({
        "command": (), "next": (), "root": ("command", "next"),
    })

    def action(instance):
        return OwnerTaskAdapter(
            name=instance,
            node_id="gripper.command_position",
            node_instance_id=instance,
            port=port.operations,
            owner_port_name="kernel_operations",
            parameters=dict(_gripper_inputs(duration_s).parameters),
            value_sources={},
            attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
            input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
                parameters=kwargs["parameters"], blackboard={},
            )),
        )

    command, next_action = action("command"), action("next")
    leaves = [OperatorMotionStopPolicy(
        child=child, node_instance_id=child.name, stop_state=state,
    ) for child in (command, next_action)]
    if composite == "sequence":
        parent = py_trees.composites.Sequence("sequence", memory=True, children=leaves)
    elif composite == "fallback":
        parent = py_trees.composites.Selector("fallback", memory=True, children=leaves)
    else:
        parent = py_trees.decorators.Retry("retry", child=leaves[0], num_failures=2)
    root = OperatorMotionStopPolicy(
        child=parent, node_instance_id="root", stop_state=state, drain_children=True,
    )
    list(root.tick())
    operation = transport.operations["operation-1"]
    operation.update({"state": "executing", "operation_revision": 3})
    port.scope.request_stop("program-run.1", "held-command", "operator Stop")
    transport.stop_states["scope-stop-1"] = "succeeded"
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"command", "next", "root"})))
    list(root.tick())
    assert command.has_active_task
    assert len(transport.submissions) == 1
    assert [row[0] for row in transport.controls] == ["stop_program_run"]
    operation.update({
        "state": "completed" if succeeded else "failed",
        "operation_revision": 7,
        "active_stop_id": "scope-stop-1",
        "active_stop_generation": 1,
        "participants": [{"state": "completed"}] if succeeded else [_final_pneumatic_failure()],
    })
    original_participants = deepcopy(operation["participants"])
    list(root.tick())
    assert leaves[0].presented_status is (
        py_trees.common.Status.SUCCESS if succeeded else py_trees.common.Status.FAILURE
    )
    assert command.cached_control_state() == "settled"
    assert next_action.status is py_trees.common.Status.INVALID
    for _ in range(3):
        list(root.tick())
    assert len(transport.submissions) == 1
    assert [row[0] for row in transport.controls] == ["stop_program_run", "acknowledge"]
    assert transport.controls[-1][2]["expected_operation_revision"] == 7
    assert transport.released_scope_stops == []
    assert operation["participants"] == original_participants
    assert transport.submissions[0]["participants"][0]["payload"]["caller_options"] == [
        {"option_id": "command_duration_s", "value_s": duration_s},
    ]


@pytest.mark.parametrize("reason", [
    "delivery_refused", "delivery_exception", "delivery_reply_lost", "delivery_timeout",
])
def test_final_pneumatic_failure_keeps_result_and_exact_ack_revision(reason) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("pneumatic")
    started = port.operations.start("gripper.command_position", identity, _gripper_inputs())
    operation = transport.operations[started.task_id]
    operation.update({
        "state": "failed", "operation_revision": 7,
        "participants": [_final_pneumatic_failure(reason), {"state": "completed"}],
    })
    observed = port.operations.observe(started.task_id)
    assert observed.state is OwnerTaskState.FAILED
    assert observed.settlement_proven is True
    assert observed.reason_code == "command_failed"
    assert observed.details == {"reason": reason}
    assert observed.metadata["operation_revision"] == 7
    reads = transport.operation_reads
    operation.update({"state": "completed", "operation_revision": 8})
    assert port.operations.observe(started.task_id) == observed
    assert transport.operation_reads == reads
    with pytest.raises(ProductionPortError, match="changed node attempt"):
        port.operations.acknowledge(started.task_id, _identity("foreign"))
    assert transport.controls == []
    port.operations.acknowledge(started.task_id, identity)
    assert [row[0] for row in transport.controls] == ["acknowledge"]
    _, operation_id, body = transport.controls[0]
    assert operation_id == started.task_id
    assert body["expected_operation_revision"] == 7
    assert body["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
    assert transport.operation_reads == reads
    assert len(transport.submissions) == 1


@pytest.mark.parametrize("case", [
    "empty", "position_failure", "active_sibling", "interrupted_sibling",
    "unmarked_sibling", "boolean_marker", "intervention_required", "all_completed",
])
def test_pneumatic_settlement_marker_does_not_settle_other_failures(case) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("gripper")
    started = port.operations.start("gripper.command_position", identity, _gripper_inputs())
    operation = transport.operations[started.task_id]
    row = _final_pneumatic_failure()
    participants = [row]
    if case == "empty":
        participants = []
    elif case == "position_failure":
        row["terminal_error"]["details"].pop("pneumatic_transaction_final")
    elif case in {"active_sibling", "interrupted_sibling", "unmarked_sibling"}:
        participants.append({"state": {
            "active_sibling": "executing", "interrupted_sibling": "stopped",
            "unmarked_sibling": "failed",
        }[case]})
    elif case == "boolean_marker":
        row["terminal_error"]["details"]["pneumatic_transaction_final"] = True
    elif case == "all_completed":
        participants = [{"state": "completed"}]
    operation.update({
        "state": "intervention_required" if case == "intervention_required" else "failed",
        "operation_revision": 3, "participants": participants,
    })
    observed = port.operations.observe(started.task_id)
    assert observed.state is OwnerTaskState.FAILED
    assert observed.settlement_proven is False
    with pytest.raises(ProductionPortError, match="only a completed operation"):
        port.operations.acknowledge(started.task_id, identity)
    assert transport.controls == []


def test_final_pneumatic_failure_unbind_consumes_exact_owner_result_without_reset() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    task = port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    transport.operations[task.task_id].update({
        "state": "failed", "operation_revision": 5,
        "participants": [_final_pneumatic_failure()],
    })
    port.unbind("program-run.1")
    assert [row[0] for row in transport.controls] == ["acknowledge"]
    assert transport.controls[0][2]["expected_operation_revision"] == 5
    assert port._tasks == {}


def test_final_pneumatic_failure_waits_for_owner_acknowledgement_without_resending() -> None:
    class EffectInFlight(RuntimeError):
        code = "operation_effect_in_flight"

    class Pending(_Transport):
        dispatch_returned = False

        def acknowledge_completed(self, operation_id, body):
            if not self.dispatch_returned:
                raise EffectInFlight("the admitted dispatch has not returned")
            return super().acknowledge_completed(operation_id, body)

    transport = Pending()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("pneumatic")
    task = port.operations.start("gripper.command_position", identity, _gripper_inputs())
    operation = transport.operations[task.task_id]
    operation.update({
        "state": "failed", "operation_revision": 7,
        "participants": [_final_pneumatic_failure()],
    })
    failed = port.operations.observe(task.task_id)
    with pytest.raises(OwnerTaskSettlementPendingError):
        port.operations.acknowledge(task.task_id, identity)
    assert port.operations.observe(task.task_id) == failed
    assert transport.controls == []
    assert len(transport.submissions) == 1
    transport.dispatch_returned = True
    port.operations.acknowledge(task.task_id, identity)
    assert [row[0] for row in transport.controls] == ["acknowledge"]
    assert transport.controls[0][2]["expected_operation_revision"] == 7
    assert port._tasks == {}


def test_pneumatic_unknown_kernel_admission_reply_keeps_existing_exclusion() -> None:
    transport = _LostAcceptanceReplyTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    task = port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert task.state is OwnerTaskState.FAILED
    assert task.reason_code == "operation_admission_outcome_unknown"
    assert task.settlement_proven is False
    assert port.operations.observe(task.task_id) == task
    assert port.scope.observe("program-run.1").state is ProgramRunScopeState.UNKNOWN
    assert len(transport.submit_attempts) == 1
    assert len(transport.submissions) == 1
    assert transport.controls == []


def test_unadmitted_pneumatic_command_remains_held_without_a_send() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    state = OperatorMotionStopState({"command": ()})
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"command"})))
    command = OwnerTaskAdapter(
        name="command", node_id="gripper.command_position", node_instance_id="command",
        port=port.operations, parameters=dict(_gripper_inputs().parameters), value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={},
        )),
    )
    leaf = OperatorMotionStopPolicy(child=command, node_instance_id="command", stop_state=state)
    for _ in range(3):
        list(leaf.tick())
    assert leaf.status is py_trees.common.Status.RUNNING
    assert command.status is py_trees.common.Status.INVALID
    assert transport.submissions == []
    assert transport.controls == []


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
            "tcp_endpoint": {"target_role": "arm", "frame_role": "tcp"},
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


def test_pick_offsets_accept_cell_and_other_robot_frames_like_motion_references() -> None:
    transport = _PickPlaceTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = _pick_inputs()
    parameters = dict(inputs.parameters)
    synthesis = dict(parameters["synthesis"])
    synthesis["pick_offset_reference"] = {"type": "cell_frame"}
    synthesis["approach_offset_reference"] = {
        "type": "frame_role",
        "frame_role_id": "tcp",
        "target_role": "gripper",
    }
    parameters["synthesis"] = synthesis

    result = port.operations.start(
        "planning.plan_pick_sequence",
        _identity("plan-pick-offsets"),
        OwnerNodeInputs(parameters=parameters),
    )

    assert result.state is OwnerTaskState.SUCCEEDED
    forwarded = transport.pick_requests[-1]["synthesis"]
    assert forwarded["pick_offset_reference"] == {"type": "cell_frame"}
    # The robot qualifier resolves through the captured Program Binding, the
    # same mapping a motion reference uses.
    assert forwarded["approach_offset_reference"] == {
        "type": "frame_role",
        "frame_role_id": "tcp",
        "target": _GRIPPER,
    }

    synthesis["approach_offset_reference"] = {
        "type": "frame_role",
        "frame_role_id": "tcp",
        "target_role": "unbound_robot",
    }
    requests = len(transport.pick_requests)
    with pytest.raises(ProductionPortError, match="absent from the Program Binding"):
        port.operations.start(
            "planning.plan_pick_sequence",
            _identity("plan-pick-unbound-offset"),
            OwnerNodeInputs(parameters=parameters),
        )
    assert len(transport.pick_requests) == requests


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


@pytest.mark.parametrize("phase,alias,value", [("pick", "closed", 0.1), ("place", "open", 0.8)])
@pytest.mark.parametrize("kind", ["angular_rad", "linear_m"])
def test_named_planning_output_reuses_resolved_numeric_tool_position(phase, alias, value, kind):
    transport = _preset_transport(_PickPlaceTransport(), kind=kind)
    plan = transport.plan_pick if phase == "pick" else transport.plan_place

    def change_presets_after_planning(intent):
        result = plan(intent)
        for row in transport.preset_descriptor["named_presets"]:
            row["master_position"] = 0.5
            row["coordinates"][0]["position"] = 0.5
        return result

    if phase == "pick":
        transport.plan_pick = change_presets_after_planning
    else:
        transport.plan_place = change_presets_after_planning
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_pick_inputs().parameters)
    parameters["tool_position"] = {"kind": "named_preset", "name": alias}
    parameters["tool_max_effort"] = None
    if phase == "place":
        parameters["place_pose"] = {
            "position_m": [0.4, 0.1, 0.2], "orientation": {"rpy_deg": [180.0, 0.0, 0.0]},
        }
    result = port.operations.start(f"planning.plan_{phase}_sequence", _identity(phase),
                                   OwnerNodeInputs(parameters=parameters))
    requests = transport.pick_requests if phase == "pick" else transport.place_requests
    field = "value_rad" if kind == "angular_rad" else "value_m"
    expected = {"kind": kind, field: value}
    numeric = requests[0]["tool_operation"]["participants"][0]["payload"]["command_value"]["position"]
    assert numeric == expected
    assert result.output["tool_position"] == expected
    assert transport.target_reads == 1
    assert parameters["tool_position"] == {"kind": "named_preset", "name": alias}
    assert transport.submissions == []


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


def _pose_latch_owner_response() -> dict[str, Any]:
    return {
        "api_version": "manyforge.manager_command_response.v1",
        "cell_id": "cell-a",
        "initialization_generation": 1,
        "initialization_state": "ready",
        "ok": True,
        "runtime_identity": {
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "deployment_generation": 1,
            "program_generation": 1,
            "runtime_generation": 1,
        },
        "scene_runtime_owner_epoch": "scene-runtime.epoch.1",
    }


def _pose_latch_success_response(request: Mapping[str, Any]) -> dict[str, Any]:
    projection = deepcopy(_SceneTransport().revision)
    committed_projection = deepcopy(projection)
    committed_projection["scene_revision"]["revision_id"] += 1
    committed_projection["scene_revision"]["content_hash"] = _DIGEST_F
    return {
        "already_applied_observation_count": 0,
        "api_version": "manyforge.manager_command_response.v1",
        "committed_observation_count": 1,
        "initial_projection_revision": projection,
        "latched_observation_count": 1,
        "ok": True,
        "projection_revision": committed_projection,
        "runtime_identity": deepcopy(request["expected_runtime_identity"]),
        "scene_runtime_source_generation": 2,
        "skipped_attached_observation_count": 0,
    }


def test_between_nodes_latch_sends_sorted_scope_and_accepts_authoritative_commit() -> None:
    transport = _SceneTransport()
    requests: list[dict[str, Any]] = []

    def manager_command(request: dict[str, Any]) -> dict[str, Any]:
        requests.append(deepcopy(request))
        if request["command"] == "scene_runtime_owner_read":
            return _pose_latch_owner_response()
        assert request["command"] == "scene_runtime_latch_between_nodes"
        return _pose_latch_success_response(request)

    scene = HeadlessSceneProgramPort(transport, manager_command=manager_command)
    scene.bind(_scene_context())

    scene.latch_between_nodes(
        _identity("latch"), scene_resource_ids=("alpha", "zeta")
    )

    assert requests[1] == {
        "api_version": "manyforge.manager_command.v1",
        "command": "scene_runtime_latch_between_nodes",
        "expected_runtime_identity": _pose_latch_owner_response()["runtime_identity"],
        "scene_resource_ids": ["alpha", "zeta"],
    }


def test_between_nodes_capture_uses_manager_scene_without_a_stale_pre_read() -> None:
    class NoScenePreRead(_SceneTransport):
        def scene(self):
            raise AssertionError("between-nodes capture must not pre-read Scene")

    def manager_command(request):
        if request["command"] == "scene_runtime_owner_read":
            return _pose_latch_owner_response()
        assert "expected_projection_revision" not in request
        result = _pose_latch_success_response(request)
        result["initial_projection_revision"]["scene_revision"]["revision_id"] = 20
        result["projection_revision"]["scene_revision"]["revision_id"] = 21
        return result

    scene = HeadlessSceneProgramPort(NoScenePreRead(), manager_command=manager_command)
    scene.bind(_scene_context())
    scene.latch_between_nodes(_identity("latch"), scene_resource_ids=("part",))


@pytest.mark.parametrize("crossed", ["bundle", "scene", "revision"])
def test_between_nodes_capture_retains_deployment_and_scene_fences(crossed) -> None:
    def manager_command(request):
        if request["command"] == "scene_runtime_owner_read":
            return _pose_latch_owner_response()
        result = _pose_latch_success_response(request)
        if crossed == "bundle":
            result["initial_projection_revision"]["retained_bundle_id"] = _DIGEST_A
        elif crossed == "scene":
            result["projection_revision"]["scene_revision"]["scene_id"] = _DIGEST_A
        else:
            result["initial_projection_revision"]["scene_revision"]["revision_id"] = 3
        return result

    scene = HeadlessSceneProgramPort(_SceneTransport(), manager_command=manager_command)
    scene.bind(_scene_context())
    with pytest.raises(PoseSourceLatchError):
        scene.latch_between_nodes(_identity("latch"), scene_resource_ids=("part",))


@pytest.mark.parametrize(
    "latch_response,expected_reason",
    [
        ({"api_version": "manyforge.manager_command_response.v1", "ok": True},
         "scene_runtime_latch_failed"),
        (
            {
                "api_version": "manyforge.manager_command_response.v1",
                "ok": False,
                "error": {"code": "stale_scene_projection", "message": "stale"},
            },
            "stale_scene_projection",
        ),
    ],
)
def test_between_nodes_latch_malformed_or_refused_response_fails_before_motion_dispatch(
    latch_response: Mapping[str, Any], expected_reason: str
) -> None:
    scene_transport = _SceneTransport()
    latch_requests: list[dict[str, Any]] = []

    def manager_command(request: dict[str, Any]) -> dict[str, Any]:
        if request["command"] == "scene_runtime_owner_read":
            return _pose_latch_owner_response()
        latch_requests.append(deepcopy(request))
        return deepcopy(dict(latch_response))

    scene = HeadlessSceneProgramPort(
        scene_transport, manager_command=manager_command
    )
    scene.bind(_scene_context())
    kernel_transport = _Transport()
    native = NativeKernelProgramPort(
        KernelClient(kernel_transport),
        scene_pose_source_latch=scene.latch_between_nodes,
    )
    native.bind(_context())
    parameters = dict(_motion_inputs().parameters)
    parameters["target_reference"] = {
        "type": "scene_resource",
        "scene_resource_id": "zeta",
    }
    parameters["offset_reference"] = {
        "type": "scene_resource",
        "scene_resource_id": "alpha",
    }

    with pytest.raises(PoseSourceLatchError) as error:
        native.operations.start(
            "motion.move_to_pose",
            _identity("refused-motion"),
            OwnerNodeInputs(parameters=parameters),
        )

    assert error.value.reason_code == "pose_source_apply_failed"
    assert error.value.details == {"reason": expected_reason}
    assert latch_requests[0]["scene_resource_ids"] == ["alpha", "zeta"]
    assert kernel_transport.submissions == []


def test_simulation_reset_latches_once_after_successful_terminal_observation() -> None:
    class ResetTaskPort:
        def __init__(self) -> None:
            self.observed = OwnerTaskSnapshot(
                task_id="reset-task.1", state=OwnerTaskState.SUCCEEDED
            )

        def start(self, _node_id, _identity, _inputs):
            return OwnerTaskSnapshot(
                task_id="reset-task.1", state=OwnerTaskState.ACTIVE
            )

        def observe(self, _task_id):
            return self.observed

        def cancel(self, _task_id, _identity):
            pass

        def acknowledge(self, _task_id, _identity):
            pass

    calls: list[tuple[NodeAttemptIdentity, Sequence[str] | None]] = []
    scene = SimpleNamespace(
        latch_between_nodes=lambda identity, *, scene_resource_ids: calls.append(
            (identity, scene_resource_ids)
        )
    )
    identity = _identity("reset")
    reset = SimulationResetPoseSourceTaskPort(ResetTaskPort(), scene)

    started = reset.start(
        "simulation.reset", identity, OwnerNodeInputs(parameters={})
    )
    assert started.state is OwnerTaskState.ACTIVE
    assert calls == []

    terminal = reset.observe(started.task_id)

    assert terminal.state is OwnerTaskState.SUCCEEDED
    assert calls == [(identity, None)]


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


def _scene_compaction_no_effect_rejection(body, expected_owner_epoch):
    error = _scene_rejection(
        body,
        "stale_request_owner_epoch",
        "Scene request owner epoch changed during history compaction",
    )
    error.response["error"]["details"] = {
        "reason": "scene_history_compaction_no_effect",
        "previous_request_owner_epoch": body["request_context"]["request_owner_epoch"],
        "expected_request_owner_epoch": expected_owner_epoch,
    }
    error.response["replay_status"] = "stale_request_owner_epoch"
    return error


def _scene_stale_owner_epoch_rejection(body, expected_owner_epoch):
    """The owner's plain rejection when another writer compacted history."""
    error = _scene_rejection(
        body,
        "stale_request_owner_epoch",
        "scene request names a stale logical-owner epoch",
    )
    error.response["error"]["details"] = {
        "expected_request_owner_epoch": expected_owner_epoch,
    }
    error.response["replay_status"] = "stale_request_owner_epoch"
    return error


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


def test_scene_initialization_retries_proven_compaction_epoch_rotation_once() -> None:
    class _CompactionTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if not self.rejected_requests:
                request = deepcopy(dict(body))
                self.rejected_requests.append(request)
                expected_epoch = "scene-owner.epoch.compacted"
                self.owner_epoch = expected_epoch
                raise _scene_compaction_no_effect_rejection(body, expected_epoch)
            assert body["request_context"]["request_owner_epoch"] == self.owner_epoch
            return super().commit_scene_transaction(body)

    transport = _CompactionTransport()
    port = _scene_port(transport)

    proof = port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])

    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    rejected = transport.rejected_requests[0]
    committed = transport.submissions[0]
    assert rejected["request_context"]["request_id"] != committed["request_context"]["request_id"]
    assert rejected["request_context"]["request_owner_epoch"] == "scene-owner.epoch.1"
    assert committed["request_context"]["request_owner_epoch"] == "scene-owner.epoch.compacted"
    assert rejected["expected_projection_revision"] == committed["expected_projection_revision"]
    # The proof names the prepared scene, not the owner epoch compaction rotates.
    assert "scene_owner_epoch" not in proof


@pytest.mark.parametrize("rejection", ["stale_request_owner_epoch", "stale_revision"])
def test_scene_initialization_follows_observer_compaction_of_the_same_scene(rejection) -> None:
    """Reset/run-start preparation continues across another writer's rotation."""

    class _ObserverTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if not self.rejected_requests:
                self.rejected_requests.append(deepcopy(dict(body)))
                # Pose-source commits advanced the revision and compacted.
                self.owner_epoch = "scene-owner.epoch.compacted"
                revision_id = self.revision["scene_revision"]["revision_id"] + 2
                self.revision["scene_revision"] = {
                    **self.revision["scene_revision"],
                    "revision_id": revision_id,
                    "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
                }
                if rejection == "stale_revision":
                    raise _scene_rejection(body, "stale_revision", "revision advanced")
                raise _scene_stale_owner_epoch_rejection(body, self.owner_epoch)
            assert body["request_context"]["request_owner_epoch"] == self.owner_epoch
            return super().commit_scene_transaction(body)

    transport = _ObserverTransport()
    port = _scene_port(transport)

    proof = port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])

    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    committed = transport.submissions[0]
    assert committed["request_context"]["request_owner_epoch"] == "scene-owner.epoch.compacted"
    assert proof["scene_identity"]["scene_id"] == transport.revision["scene_revision"]["scene_id"]


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


def test_prepared_scene_verification_accepts_a_newer_revision_of_the_same_scene() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    context = _scene_context()
    scene = [_authored_scene()[0]]
    proof = port.prepare_initial_scene(context, scene)
    # The proof names the prepared scene, never one revision of it.
    assert "projection_revision" not in proof
    # A continuous pose source advances the revision of the prepared scene.
    revision_id = transport.revision["scene_revision"]["revision_id"] + 1
    scene_id = transport.revision["scene_revision"]["scene_id"]
    transport.revision["scene_revision"] = {
        "scene_id": scene_id,
        "revision_id": revision_id,
        "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
    }
    port.verify_initial_scene(context, scene, proof)

    # A different scene is not the prepared one.
    transport.revision["scene_revision"] = {
        **transport.revision["scene_revision"],
        "scene_id": _DIGEST_A if scene_id != _DIGEST_A else _DIGEST_B,
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
    # Without a remembered proof the port compares the current objects. Only
    # the component-frame object, which the native owner resolves, is
    # committed again; the matching cell-frame object is not.
    repeated = transport.submissions[first_submission_count:]
    assert [
        (body["operation"]["kind"], body["operation"]["scene_resource_id"])
        for body in repeated
    ] == [("upsert_object", "table")]

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
                parameters=kwargs["parameters"], blackboard={}
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
                parameters=kwargs["parameters"], blackboard={}
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


def test_attach_node_sends_its_grasp_lost_bound_with_the_attachment() -> None:
    transport = _SceneTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), [_authored_scene()[0]])
    transport.submissions.clear()
    parameters = {
        "scene_resource_id": "graspable",
        "target_role": "arm",
        "carrier_frame_role": "tcp",
        "touch_body_roles": ["tool_collision"],
    }

    plain = port.invoke(
        "scene.attach_carried_object",
        _identity("attach-plain"),
        OwnerNodeInputs(parameters=parameters),
    )
    assert plain.succeeded is True
    assert "grasp_lost_bound" not in transport.submissions[0]["operation"]
    port.invoke(
        "scene.detach_carried_object",
        _identity("detach-plain"),
        OwnerNodeInputs(parameters={"scene_resource_id": "graspable", "target_role": "arm"}),
    )

    bounded = port.invoke(
        "scene.attach_carried_object",
        _identity("attach-bounded"),
        OwnerNodeInputs(
            parameters={
                **parameters,
                "grasp_lost_translation_m": 0.03,
                "grasp_lost_rotation_rad": 0.26,
            }
        ),
    )
    assert bounded.succeeded is True
    assert transport.submissions[-1]["operation"]["grasp_lost_bound"] == {
        "translation_m": 0.03,
        "rotation_rad": 0.26,
    }


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


def test_scene_node_retries_one_proven_compaction_epoch_rotation() -> None:
    class _CompactionTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.reject_next_node = False
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if self.reject_next_node:
                self.reject_next_node = False
                self.rejected_requests.append(deepcopy(dict(body)))
                expected_epoch = "scene-owner.epoch.compacted"
                self.owner_epoch = expected_epoch
                raise _scene_compaction_no_effect_rejection(body, expected_epoch)
            assert body["request_context"]["request_owner_epoch"] == self.owner_epoch
            return super().commit_scene_transaction(body)

    transport = _CompactionTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), _authored_scene())
    transport.submissions.clear()
    transport.reject_next_node = True

    result = port.invoke(
        "scene.attach_carried_object",
        _identity("attach-after-compaction"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )

    assert result.succeeded is True
    assert result.metadata["stale_revision_retry_count"] == 0
    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    rejected = transport.rejected_requests[0]
    committed = transport.submissions[0]
    assert rejected["request_context"]["request_id"] != committed["request_context"]["request_id"]
    assert rejected["request_context"]["correlation_id"] == committed["request_context"]["correlation_id"]
    assert rejected["request_context"]["request_owner_epoch"] == "scene-owner.epoch.1"
    assert committed["request_context"]["request_owner_epoch"] == "scene-owner.epoch.compacted"
    assert rejected["expected_projection_revision"] == committed["expected_projection_revision"]


@pytest.mark.parametrize("receipt", ["observer_compaction", "foreign_epoch_detail"])
def test_scene_node_retries_once_after_compaction_by_another_writer(receipt) -> None:
    """A pose source's commit compacted history; this request had no effect."""

    class _CompactionTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.reject_next_node = False
            self.rejected_requests: list[dict[str, Any]] = []

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if self.reject_next_node:
                self.reject_next_node = False
                self.rejected_requests.append(deepcopy(dict(body)))
                self.owner_epoch = "scene-owner.epoch.compacted"
                # The observer's commits also advanced the revision.
                revision_id = self.revision["scene_revision"]["revision_id"] + 3
                self.revision["scene_revision"] = {
                    **self.revision["scene_revision"],
                    "revision_id": revision_id,
                    "content_hash": "sha256:" + f"{revision_id:064x}"[-64:],
                }
                if receipt == "observer_compaction":
                    raise _scene_stale_owner_epoch_rejection(body, self.owner_epoch)
                # Details are not trusted; the re-read view names the epoch.
                raise _scene_compaction_no_effect_rejection(body, "scene-owner.epoch.other")
            assert body["request_context"]["request_owner_epoch"] == self.owner_epoch
            return super().commit_scene_transaction(body)

    transport = _CompactionTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), _authored_scene())
    transport.submissions.clear()
    transport.reject_next_node = True

    result = port.invoke(
        "scene.attach_carried_object",
        _identity(f"attach-after-{receipt}"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )

    assert result.succeeded is True
    assert len(transport.rejected_requests) == 1
    assert len(transport.submissions) == 1
    rejected = transport.rejected_requests[0]
    committed = transport.submissions[0]
    assert rejected["request_context"]["request_id"] != committed["request_context"]["request_id"]
    assert committed["request_context"]["request_owner_epoch"] == "scene-owner.epoch.compacted"
    # The retry names the re-read (newer) revision of the same Scene.
    assert (
        committed["expected_projection_revision"]["scene_revision"]["revision_id"]
        == rejected["expected_projection_revision"]["scene_revision"]["revision_id"] + 3
    )


@pytest.mark.parametrize("fault", ["effect_unproven", "scene_changed", "second_rotation"])
def test_scene_node_blocks_unproven_or_repeated_compaction_epoch_rotation(fault) -> None:
    class _CompactionTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__()
            self.rejected_requests: list[dict[str, Any]] = []
            self.reject_compaction = False

        def commit_scene_transaction(self, body: Mapping[str, Any]) -> dict[str, Any]:
            if not self.reject_compaction:
                return super().commit_scene_transaction(body)
            request = deepcopy(dict(body))
            self.rejected_requests.append(request)
            old_epoch = self.owner_epoch
            expected_epoch = f"scene-owner.epoch.compacted.{len(self.rejected_requests)}"
            self.owner_epoch = expected_epoch
            if fault == "scene_changed" and len(self.rejected_requests) == 1:
                self.revision["scene_revision"] = {
                    **self.revision["scene_revision"],
                    "scene_id": _DIGEST_E,
                }
            error = _scene_stale_owner_epoch_rejection(body, expected_epoch)
            if fault == "effect_unproven":
                error.response["projection_status"] = "rejected_rollback_proven"
            raise error

    transport = _CompactionTransport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), _authored_scene())
    transport.submissions.clear()
    transport.reject_compaction = True

    result = port.invoke(
        "scene.attach_carried_object",
        _identity(f"attach-compaction-{fault}"),
        OwnerNodeInputs(
            parameters={
                "scene_resource_id": "graspable",
                "target_role": "arm",
                "carrier_frame_role": "tcp",
                "touch_body_roles": ["tool_collision"],
            }
        ),
    )

    assert result.succeeded is False
    assert result.reason_code == "stale_request_owner_epoch"
    assert not transport.submissions
    if fault == "second_rotation":
        assert len(transport.rejected_requests) == 2
        assert (
            transport.rejected_requests[0]["request_context"]["request_id"]
            != transport.rejected_requests[1]["request_context"]["request_id"]
        )
    else:
        assert len(transport.rejected_requests) == 1


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


def test_scene_object_pose_is_a_read_of_owner_truth_never_a_transaction() -> None:
    class _ResolvingTransport(_SceneTransport):
        def __init__(self) -> None:
            super().__init__(populated=True)
            self.queries: list[dict[str, Any]] = []
            self.response_revision: dict[str, Any] | None = None

        def scene(self) -> dict[str, Any]:
            projection = super().scene()
            # A read does not need Scene writes to be open.
            projection["logical_scene"]["payload"]["access"]["scene_writes_open"] = False
            return projection

        def resolve_scene_resources(self, body: Mapping[str, Any]) -> dict[str, Any]:
            self.queries.append(deepcopy(dict(body)))
            return {
                "api_version": "manyforge.scene_resource_pose_response.v1",
                "projection_revision": deepcopy(self.response_revision or self.revision),
                "poses": [
                    {
                        "scene_resource_id": body["scene_resource_ids"][0],
                        "pose_in_cell": {
                            "position_m": {"x": 0.5, "y": 0.0, "z": 0.25},
                            "orientation_xyzw": [0.0, 0.0, 0.0, 2.0],
                        },
                    }
                ],
            }

    transport = _ResolvingTransport()
    port = _scene_port(transport)
    port.bind(_scene_context())

    def read(resource_id: str):
        return port.invoke(
            "scene.get_object_pose",
            _identity(f"read-{resource_id}"),
            OwnerNodeInputs(
                parameters={"scene_resource_id": resource_id, "output_key": "pose"}
            ),
        )

    fixed = read("old-support")
    assert fixed.succeeded is True
    assert fixed.output == {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [1.0, 2.0, 3.0],
        "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
    }
    assert fixed.metadata == {"placement": "cell_fixed"}
    carried = read("old-part")
    assert carried.succeeded is True
    assert carried.output["position_m"] == [0.5, 0.0, 0.25]
    assert carried.output["orientation"] == {"xyzw": [0.0, 0.0, 0.0, 1.0]}
    assert transport.queries == [
        {
            "api_version": "manyforge.scene_resource_pose_query.v1",
            "expected_projection_revision": transport.revision,
            "scene_resource_ids": ["old-part"],
        }
    ]
    # A pose-source commit between the read and the query: the owner answers
    # from a newer revision of the same Scene, which is the latest truth.
    scene_revision = transport.revision["scene_revision"]
    transport.response_revision = {
        **transport.revision,
        "scene_revision": {
            **scene_revision,
            "revision_id": scene_revision["revision_id"] + 1,
            "content_hash": _DIGEST_F,
        },
    }
    assert read("old-part").succeeded is True
    for foreign in (
        {**transport.revision, "graph_revision": _DIGEST_F},
        {
            **transport.revision,
            "scene_revision": {**scene_revision, "content_hash": _DIGEST_F},
        },
    ):
        transport.response_revision = foreign
        changed = read("old-part")
        assert changed.succeeded is False
        assert "changed owner revision" in changed.message
    transport.response_revision = None
    absent = read("missing")
    assert absent.succeeded is False
    assert "absent from the logical scene" in absent.message
    assert transport.submissions == []

    plain = _SceneTransport(populated=True)
    plain_port = _scene_port(plain)
    plain_port.bind(_scene_context())
    unresolved = plain_port.invoke(
        "scene.get_object_pose",
        _identity("read-carried"),
        OwnerNodeInputs(parameters={"scene_resource_id": "old-part", "output_key": "pose"}),
    )
    assert unresolved.succeeded is False
    assert "carried-object pose projection is unavailable" in unresolved.message


def _joint_inputs() -> OwnerNodeInputs:
    parameters = dict(_motion_inputs().parameters)
    for name in ("target_reference", "offset_reference", "contact_policy_id"):
        parameters.pop(name)
    parameters.update(
        target_mode="absolute",
        goal=[
            {"joint_name": "joint_1", "position_rad": 0.25},
            {"joint_name": "joint_2", "position_rad": -0.5},
        ],
        per_joint_dynamics_limits={"joint_1": {"max_velocity_rad_s": 0.75}},
    )
    return OwnerNodeInputs(parameters=parameters)


def _contact_pose_inputs() -> OwnerNodeInputs:
    parameters = dict(_absolute_inputs("motion.move_to_pose", [_BASE]).parameters)
    parameters.update(
        contact_policy_id="pick.contact",
        contact_scene_resource_id="part.7",
        max_tcp_linear_speed_mps=0.2,
        planning_profile="quality_bounded",
        cache_mode="disable",
    )
    return OwnerNodeInputs(parameters=parameters)


_PREVIEW_VARIANTS = {
    "joint_goal": ("motion.move_to_joint_target", _joint_inputs, None),
    "pose_base_frame_with_contact": (
        "motion.move_to_pose",
        _contact_pose_inputs,
        None,
    ),
    "cartesian_base_frame": (
        "motion.follow_cartesian_path",
        lambda: _absolute_inputs("motion.follow_cartesian_path", [None, _BASE]),
        None,
    ),
    "pose_relative": (
        "motion.move_to_pose",
        _motion_inputs,
        "relative_motion_preview_unsupported",
    ),
    "pose_cell_goal_frame": (
        "motion.move_to_pose",
        lambda: _absolute_inputs("motion.move_to_pose", [{"kind": "cell_frame"}]),
        "goal_frame_motion_preview_unsupported",
    ),
    "cartesian_other_robot_goal_frame": (
        "motion.follow_cartesian_path",
        lambda: _absolute_inputs(
            "motion.follow_cartesian_path",
            [{"kind": "target_frame", "target_role": "gripper", "frame_role": "tcp"}] * 2,
        ),
        "goal_frame_motion_preview_unsupported",
    ),
}


@pytest.mark.parametrize("variant", sorted(_PREVIEW_VARIANTS))
def test_node_preview_uses_the_execution_motion_builder_exactly(variant) -> None:
    node_id, inputs_factory, unavailable = _PREVIEW_VARIANTS[variant]
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = inputs_factory()
    identity = _identity("move-preview")

    port.operations.start(node_id, identity, inputs)
    executed = transport.submissions[0]
    executed_payload = executed["participants"][0]["payload"]
    bound = port._context(identity.program_run_id)  # noqa: SLF001
    shared = program_motion_participant(
        node_id,
        inputs.parameters,
        targets=bound.targets,
        contact_grasps=bound.contact_grasps,
        move_id=executed_payload["move_id"],
    )
    # One builder: relative targets, goal frames, contact inputs and limits
    # are the execution payload itself, not a second translation.
    assert dict(shared.payload) == executed_payload
    submissions_before = deepcopy(transport.submissions)
    controls_before = deepcopy(transport.controls)

    if unavailable is not None:
        with pytest.raises(ProgramMotionPreviewUnavailableError) as refused:
            port.motion_preview_intent(identity, node_id, inputs.parameters, request_id="p.1")
        assert refused.value.reason_code == unavailable
    else:
        participant, intent = port.motion_preview_intent(
            identity, node_id, inputs.parameters, request_id="preview.1"
        )
        previewed = intent["participants"][0]
        assert previewed["payload"]["move_id"] != executed_payload["move_id"]
        assert {**previewed["payload"], "move_id": None} == {
            **executed_payload,
            "move_id": None,
        }
        assert participant.target.target_role_id == "arm"
        assert previewed == {
            "component_role_id": executed["participants"][0]["component_role_id"],
            "target_role_id": executed["participants"][0]["target_role_id"],
            "required_capability_id": "execution.trajectory.execute",
            "action_span": None,
            "payload": previewed["payload"],
        }
        assert intent["request_context"]["request_id"] == "preview.1"
        assert {
            key: value
            for key, value in intent.items()
            if key not in {"request_context", "participants"}
        } == {
            key: value
            for key, value in executed.items()
            if key not in {"request_context", "participants"}
        }
    # Preview submitted, controlled and retained nothing.
    assert transport.submissions == submissions_before
    assert transport.controls == controls_before


def test_node_preview_refuses_unbound_runs_and_non_motion_nodes() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    identity = _identity("move-preview")

    with pytest.raises(ProgramMotionPreviewUnavailableError) as unbound:
        port.motion_preview_intent(
            identity, "motion.move_to_pose", _contact_pose_inputs().parameters, request_id="p"
        )
    assert unbound.value.reason_code == "program_run_context_unavailable"
    port.bind(_context())
    with pytest.raises(ProgramMotionPreviewUnavailableError) as gripper:
        port.motion_preview_intent(
            identity, "gripper.command_position", {"target_role": "gripper"}, request_id="p"
        )
    assert gripper.value.reason_code == "node_motion_preview_unsupported"
    # A captured frame naming no Program Binding robot is refused, not guessed.
    with pytest.raises(ProductionPortError, match="absent from the Program Binding"):
        port.motion_preview_intent(
            identity,
            "motion.move_to_pose",
            _absolute_inputs(
                "motion.move_to_pose",
                [{"kind": "target_frame", "target_role": "ghost", "frame_role": "tcp"}],
            ).parameters,
            request_id="p",
        )
    assert transport.submissions == []


def _interrupt_inputs(node_id: str) -> OwnerNodeInputs:
    """Resolved inputs of one interrupt move: the rule fields travel only in
    the captured Program; the motion fields build the ordinary payload."""

    parameters = {
        key: value
        for key, value in _absolute_inputs("motion.move_to_pose", [_BASE]).parameters.items()
        if key not in {"target_mode", "target_reference", "offset_reference", "contact_policy_id"}
    }
    parameters.update(
        stop_mode="soft_on_path",
        priority=0,
        interrupt_condition={"compare": {
            "op": "==",
            "operand": {"signal": {"target_role": "station", "resource_role": "edge"}},
            "value": True,
        }},
        on_signal="success",
    )
    if node_id == "motion.probe_search":
        # The same explicit contact-object selection as the plain motion nodes.
        parameters["contact_policy_id"] = "pick.contact"
        parameters["contact_scene_resource_id"] = "part.7"
    return OwnerNodeInputs(parameters=parameters)


@pytest.mark.parametrize("node_id", ["motion.move_until_signal", "motion.probe_search"])
def test_interrupt_move_submits_the_ordinary_motion_with_its_node_context(node_id) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("search")

    started = port.operations.start(node_id, identity, _interrupt_inputs(node_id))

    assert started.state is OwnerTaskState.ACTIVE
    participant = transport.submissions[0]["participants"][0]
    assert participant["required_capability_id"] == "execution.trajectory.execute"
    # The Core lowers the rule from the captured node this scope names.
    assert participant["execution_scope"] == {
        "node_instance_id": "search",
        "node_attempt_id": "attempt:search",
    }
    payload = participant["payload"]
    # Public caller scopes stay empty; no rule field reaches the wire.
    assert payload["motion_interrupt_scopes"] == []
    assert payload["motion"]["kind"] == "pose_goal"
    assert payload["target_mode"] == "absolute"
    assert "goal_frame_reference" not in payload
    for field in ("interrupt_condition", "stop_mode", "on_signal", "priority"):
        assert field not in repr(payload)
    if node_id == "motion.probe_search":
        # The Program contact grasp selects its object like any motion node.
        assert payload["contact_policy_id"] == "pick.contact"
        assert payload["runtime_object_bindings"] == [
            {"runtime_object_id": "selected_object", "scene_resource_id": "part.7"}
        ]
    else:
        assert payload["contact_policy_id"] is None
        assert payload["runtime_object_bindings"] == []


@pytest.mark.parametrize("missing", ["contact_policy_id", "contact_scene_resource_id"])
def test_probe_search_requires_its_contact_policy_and_object_together(missing) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_interrupt_inputs("motion.probe_search").parameters)
    parameters.pop(missing)

    with pytest.raises(ProductionPortError, match="must be supplied together"):
        port.operations.start(
            "motion.probe_search", _identity("probe"), OwnerNodeInputs(parameters=parameters)
        )
    assert transport.submissions == []


def test_probe_search_policy_without_a_program_grasp_is_refused() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    parameters = dict(_interrupt_inputs("motion.probe_search").parameters)
    parameters["contact_policy_id"] = "probe.unbound"

    with pytest.raises(ProductionPortError, match="no exact motion contact-grasp policy"):
        port.operations.start(
            "motion.probe_search", _identity("probe"), OwnerNodeInputs(parameters=parameters)
        )
    assert transport.submissions == []


@pytest.mark.parametrize("node_id", ["motion.move_until_signal", "motion.probe_search"])
def test_interrupt_move_preview_keeps_the_original_node_context(node_id) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("search")
    inputs = _interrupt_inputs(node_id)
    port.operations.start(node_id, identity, inputs)
    executed = transport.submissions[0]["participants"][0]
    submissions_before = deepcopy(transport.submissions)

    _, intent = port.motion_preview_intent(
        identity, node_id, inputs.parameters, request_id="preview.search"
    )

    previewed = intent["participants"][0]
    assert {**previewed["payload"], "move_id": None} == {**executed["payload"], "move_id": None}
    assert previewed["action_span"] is None
    assert previewed["execution_scope"] == executed["execution_scope"]
    assert transport.submissions == submissions_before


def test_idle_scene_restoration_reuses_the_exact_applied_program_convergence() -> None:
    transport = _SceneTransport()
    port = _scene_port(transport)
    context = _scene_context()
    scene = [_authored_scene()[0]]
    # Successful Program Apply/Load invokes this same existing primitive.
    proof = port.prepare_initial_scene(context, scene)
    port.verify_initial_scene(context, scene, proof)
    assert list(transport.objects) == ["graspable"]
    assert len(transport.submissions) == 1
    # A deliberate authored edit while idle is allowed. Ordinary idle Reset
    # restores the Program objects through the same convergence, never Run.
    edit = deepcopy(transport.submissions[0])
    edit["request_context"]["request_id"] = "operator.authored.scene.edit"
    edit["expected_projection_revision"] = deepcopy(transport.revision)
    edit["expected_admission_generation"] = transport.admission_generation
    edit["operation"]["kind"] = "upsert_object"
    edit["operation"]["pose"]["position_m"]["x_m"] = 0.7
    transport.commit_scene_transaction(edit)
    assert transport.objects["graspable"]["placement"]["pose_in_cell"]["position_m"]["x"] == 0.7
    proof = port.prepare_initial_scene(context, scene)
    port.verify_initial_scene(context, scene, proof)
    assert len(transport.submissions) == 3
    assert transport.submissions[-1]["operation"]["kind"] == "upsert_object"
    assert transport.objects["graspable"]["placement"]["pose_in_cell"]["position_m"]["x"] == 0.3
    port.release(context.program_run_id)


@pytest.mark.parametrize("kind", ["position_commanded", "pneumatic"])
@pytest.mark.parametrize("authored", [None, 0.0, 0.4])
def test_gripper_attempt_captures_typed_default_and_override_once(kind, authored):
    transport = _preset_transport(kind="linear_m")
    if kind == "pneumatic":
        transport.preset_descriptor.update(actuation_kind=kind, completion_policy="output_delivery",
                                           grasp_confirmation_policy="not_required", command_duration_s=1.25)
    port = NativeKernelProgramPort(KernelClient(transport)); port.bind(_context())
    parameters = dict(_gripper_inputs().parameters)
    if authored is None:
        parameters.pop("command_duration_s")
    else:
        parameters["command_duration_s"] = authored
    # Position override validation remains the backend's existing responsibility.
    started = port.operations.start("gripper.command_position", _identity(), OwnerNodeInputs(parameters=parameters))
    timing = dict(started.metadata)
    assert timing["gripper_actuation_kind"] == kind
    assert timing["gripper_dwell_s"] == ((1.25 if authored is None else authored) if kind == "pneumatic" else None)
    payload = transport.submissions[0]["participants"][0]["payload"]
    assert payload["caller_options"] == [{"option_id": "command_duration_s", "value_s": authored or 0.0}]
    transport.preset_descriptor["command_duration_s"] = 99.0
    transport.preset_descriptor["actuation_kind"] = "foreign"
    transport.operations[started.task_id].update(state="completed", operation_revision=3)
    completed = port.operations.observe(started.task_id)
    assert completed.metadata["gripper_dwell_s"] == timing["gripper_dwell_s"]
    port.operations.acknowledge(started.task_id, _identity())
    assert transport.target_reads == 1
    transport.preset_descriptor["actuation_kind"] = kind
    next_attempt = port.operations.start(
        "gripper.command_position", _identity("next-duration"), OwnerNodeInputs(parameters=parameters),
    )
    assert transport.target_reads == 2
    assert next_attempt.metadata["gripper_dwell_s"] == (
        (99.0 if authored is None else authored) if kind == "pneumatic" else None
    )


@pytest.mark.parametrize("fault", ["missing_type", "policy", "duration_nan", "duration_negative", "owner", "graph", "bundle", "schema", "limits", "unit"])
def test_numeric_gripper_reuses_verified_descriptor_and_refuses_before_effect(fault):
    transport = _preset_transport(kind="linear_m")
    descriptor = transport.preset_descriptor
    if fault == "missing_type": descriptor.pop("actuation_kind")
    elif fault == "policy": descriptor["completion_policy"] = "output_delivery"
    elif fault == "duration_nan": descriptor["command_duration_s"] = float("nan")
    elif fault == "duration_negative": descriptor["command_duration_s"] = -1.0
    elif fault in {"owner", "graph", "bundle"}:
        transport.preset_envelope["identity"][{"owner":"owner_epoch", "graph":"graph_revision", "bundle":"retained_bundle_id"}[fault]] = "foreign"
    elif fault == "schema": descriptor["contracts"]["command_schema"] = {}
    elif fault == "limits": descriptor["min_position"] = 2.0
    else: descriptor["unit"] = "rad"
    port = NativeKernelProgramPort(KernelClient(transport)); port.bind(_context())
    with pytest.raises(ProductionPortError):
        port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert transport.submissions == transport.controls == []
    assert transport.target_reads == 1


def test_numeric_gripper_accepts_explicit_unconfigured_legacy_descriptor_without_dwell():
    transport = _preset_transport(kind="linear_m")
    for key in ("actuation_kind", "output_mode", "completion_policy", "grasp_confirmation_policy", "command_duration_s"):
        transport.preset_descriptor[key] = None
    transport.preset_descriptor["named_presets"] = None
    port = NativeKernelProgramPort(KernelClient(transport)); port.bind(_context())
    started = port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert started.metadata["gripper_actuation_kind"] is None
    assert started.metadata["gripper_dwell_s"] is None
    assert transport.target_reads == 1


@pytest.mark.parametrize("field", ["owner_epoch", "graph_revision", "retained_bundle_id"])
def test_numeric_gripper_keeps_descriptor_owner_for_effect_submit_cas(field):
    transport = _preset_transport(kind="linear_m")
    read = transport.targets
    def replace_after_read():
        result = read()
        transport.preset_envelope["identity"][field] = "foreign"
        return result
    def refuse_stale(body):
        assert body["request_context"]["request_owner_epoch"] == "kernel.epoch.7"
        assert body["expected_graph_revision"] == _DIGEST_C
        assert body["expected_retained_bundle_id"] == _DIGEST_B
        return {"ok": False, "error": {"code": "invalid_state_transition", "message": "owner CAS refused", "details": {"reason":"stale_capture"}}}
    transport.targets, transport.submit = replace_after_read, refuse_stale
    port = NativeKernelProgramPort(KernelClient(transport)); port.bind(_context())
    with pytest.raises(KernelOperationAdmissionRejectedError):
        port.operations.start("gripper.command_position", _identity(), _gripper_inputs())
    assert transport.submissions == transport.controls == []
    assert port._tasks == {}


class _HeldPreparationTransport(_Transport):
    """Python transport projection fixture, not a simulated/native Kernel proof.

    Core tests must independently prove reset is local/effect-free. This fake
    exposes the real wire fields to test Behavior identity, hold and ordering.
    """
    def __init__(self):
        super().__init__()
        _preset_transport(self, kind="linear_m")
        self.preset_descriptor.update(actuation_kind="pneumatic", completion_policy="output_delivery",
                                      grasp_confirmation_policy="not_required", command_duration_s=0.25)
        self.selection_revision = 0
        self.held = False
        self.last_control = None
        self.cleanup_mode = "complete"
        self.releases = 0
        self.events = []

    def submit(self, body):
        result = super().submit(body)
        opid = result["accepted"]["operation_id"]
        result["accepted"].update(request_id=body["request_context"]["request_id"],
                                 participant_assignments=[{"input_index":0,"participant_id":"participant.1"}])
        self.operations[opid].update(program_binding_id=body["program_binding_id"], state="prepared", participants=[{
            "participant_id":"participant.1", "payload_kind":"capability_command", "state":"prepared",
            "terminal_error":None, "capability_id":"tool.gripper.command", "controllable_component_id":"tool.1",
            "command_target_id":"gripper.command", "resources":["gripper.resource"],
            "control_binding_id":"gripper.binding", "control_binding_revision":_DIGEST_E,
            "command_id":"goal.1", "command_generation":1,
        }])
        return result

    def read(self, opid):
        result = super().read(opid)
        return {**result, "owner_epoch": self.owner_epoch, "control_pending":False,
                "last_control":deepcopy(self.last_control)}

    def read_program_execution_scope(self, run_id):
        return {"ok":True,"scope":{
            "schema_version":"manyforge.program_execution_scope.v2", "program_run_id":run_id,
            "request_owner_epoch":self.owner_epoch,"program_runner_owner_epoch":"runner.1","claim_generation":1,
            "selection_revision":self.selection_revision,"held_node_indices":[0] if self.held else [],
            "ready_node_indices":[],"stopping_node_indices":[],"faulted_node_indices":[],
        }}

    def reset(self, opid, body):
        self.events.append("cleanup")
        assert body["expected_operation_revision"] == 3
        assert body["request_context"]["request_owner_epoch"] == self.owner_epoch
        if self.cleanup_mode == "borrow":
            raise RuntimeError("operation_effect_in_flight")
        self.controls.append(("reset",opid,deepcopy(body)))
        self.last_control = {
            "operation_id":opid,"request_id":body["request_context"]["request_id"],"control":"reset",
            "accepted_against_revision":3,"operation_revision_after_acceptance":4,"replay_status":"accepted_new",
        }
        self.operations[opid]["operation_revision"] = 4
        self.resident[opid] = self.cleanup_mode == "pending"
        if self.cleanup_mode == "lost_reply": raise RuntimeError("reset reply lost")
        if self.cleanup_mode == "new_hold": self.selection_revision += 1
        return {"ok":True,"accepted":deepcopy(self.last_control)}

    def release_program_selection(self, request, expected):
        self.events.append("release")
        assert expected == self.selection_revision
        assert not any(self.resident.values())
        self.releases += 1; self.selection_revision += 1; self.held = False
        return {"ok":True,"command":"release_program_selection",
                "scope":self.read_program_execution_scope(request["program_run_id"])["scope"]}


def _prepared_held_behavior():
    from manyforge_behavior.nodes.v2_owner import GripperCommandAdapter
    from manyforge_behavior.program_clock import ProgramClock
    transport = _HeldPreparationTransport()
    context = replace(_context(), program={"program_id":"lite6_program","program_revision":_DIGEST_A,
        "tree":{"instance_id":"command","node_id":"gripper.command_position","parameters":{},"children":[]}})
    port = NativeKernelProgramPort(KernelClient(transport)); port.bind(context)
    now = [100.0]; clock = ProgramClock(lambda: now[0])
    def scope_snapshot():
        return OperatorMotionStopSnapshot(transport.selection_revision,
            frozenset({"command"}) if transport.held else frozenset())
    state = OperatorMotionStopState({"command":()},read_scope=scope_snapshot,clock=clock.now)
    attempts = []
    def begin(node):
        identity = _identity(f"attempt.{len(attempts)}")
        identity = replace(identity,node_instance_id=node)
        attempts.append(identity); return identity
    parameters = dict(_gripper_inputs().parameters); parameters.pop("command_duration_s")
    node = GripperCommandAdapter(name="command",node_id="gripper.command_position",node_instance_id="command",
        port=port.operations,owner_port_name="kernel_operations",parameters=parameters,value_sources={},
        attempt_source=SimpleNamespace(begin=begin),input_resolver=SimpleNamespace(snapshot=lambda **kw:SimpleNamespace(parameters=kw["parameters"],blackboard={})),
        operator_stop_state=state,operator_stop_hold_scope="command",program_clock=clock)
    held = OperatorMotionStopPolicy(child=node,node_instance_id="command",stop_state=state)
    list(held.tick())
    original = transport.operations["operation-1"]
    original.update(state="cancelled_pre_dispatch",operation_revision=3)
    original["participants"][0]["state"] = "cancelled_pre_dispatch"
    transport.held=True;transport.selection_revision=1;state.refresh()
    return transport,port,node,held,state,attempts,now


def test_actual_behavior_owner_keeps_prepared_cancel_running_then_retires_before_resume():
    transport,port,node,held,state,attempts,now = _prepared_held_behavior()
    list(held.tick())
    assert node.status is py_trees.common.Status.RUNNING
    assert not node.presented_interrupted
    assert node.presented_held
    retained = port._tasks["operation-1"]
    assert retained.operation.operation_id == "operation-1"
    assert retained.terminal is None and retained.held_preparation is not None
    assert transport.controls == []
    for _ in range(3): list(held.tick())
    assert len(transport.submissions) == 1  # Original preparation only; no output/resubmit.
    port.scope.retire_held_preparations("program-run.1",frozenset({"command"}),1)
    assert port._tasks == {} and not transport.resident["operation-1"]
    assert transport.releases == 0
    released = port.scope.release_selection({"program_run_id":"program-run.1"},1)
    assert released["selection_revision"] == 2
    state.refresh(); list(held.tick())
    assert len(attempts) == len(transport.submissions) == 2
    assert attempts[0] != attempts[1] and attempts[0].node_instance_id == attempts[1].node_instance_id
    transport.operations["operation-2"].update(state="completed",operation_revision=5)
    transport.operations["operation-2"]["participants"][0]["state"] = "completed"
    list(held.tick())
    assert node.status is py_trees.common.Status.RUNNING
    now[0] = 100.25; list(held.tick())
    assert node.status is py_trees.common.Status.SUCCESS
    assert transport.events == ["cleanup","release"]
    assert [item[0] for item in transport.controls] == ["reset","acknowledge"]


@pytest.mark.parametrize("mode", ["pending","lost_reply","borrow","new_hold"])
def test_held_preparation_unknown_cleanup_or_new_hold_keeps_node_running(mode):
    transport,port,node,held,state,attempts,now = _prepared_held_behavior()
    list(held.tick()); transport.cleanup_mode = mode
    with pytest.raises(Exception):
        port.scope.retire_held_preparations("program-run.1",frozenset({"command"}),1)
    assert "operation-1" in port._tasks
    assert transport.held and transport.releases == 0
    list(held.tick())
    assert node.status is py_trees.common.Status.RUNNING and len(attempts) == 1
    assert len(transport.submissions) == 1
    if mode == "lost_reply":
        port.scope.retire_held_preparations("program-run.1",frozenset({"command"}),1)
        assert [row[0] for row in transport.controls] == ["reset"]
        assert port._tasks == {}


@pytest.mark.parametrize("fault", ["owner","selection","participant","committed_sibling","wrong_binding"])
def test_held_preparation_stale_or_mixed_projection_never_enters_cleanup(fault):
    transport,port,node,held,state,attempts,now = _prepared_held_behavior()
    if fault == "owner": transport.owner_epoch = "foreign"
    elif fault == "selection": transport.selection_revision = 2
    elif fault == "participant": transport.operations["operation-1"]["participants"][0]["participant_id"] = "foreign"
    elif fault == "committed_sibling":
        sibling=deepcopy(transport.operations["operation-1"]["participants"][0]);sibling["state"]="executing"
        transport.operations["operation-1"]["participants"].append(sibling)
    else: transport.operations["operation-1"]["program_binding_id"] = _DIGEST_F
    list(held.tick())
    if fault == "selection":
        with pytest.raises(ProductionPortError):
            port.scope.retire_held_preparations("program-run.1",frozenset({"command"}),1)
    assert transport.controls == []
    assert node.status is py_trees.common.Status.RUNNING
    assert len(transport.submissions) == 1 and "operation-1" in port._tasks


def test_idle_reset_scene_preparation_forwards_exact_claim_without_authoring_admission():
    authorization = {"program_runner_owner_epoch": "runner.1", "cycle_generation": 3,
                     "reset_claim": {"request_owner_epoch": "caller.1", "request_id": "reset.1",
                                    "request_payload_digest": _DIGEST_A, "reset_claim_generation": 7}}
    class Transport(_SceneTransport):
        def __init__(self):
            super().__init__()
            self.forwarded = []
        def authoring_admission(self):
            raise AssertionError("Idle Reset used ordinary authoring admission")
        def commit_scene_transaction(self, request):
            self.forwarded.append(deepcopy(request))
            # Existing fixture models ordinary authoring for its local object
            # mutations; the actual forwarded Reset request has no such grant.
            return super().commit_scene_transaction({**request, "expected_admission_generation": self.admission_generation})
    transport = Transport()
    port = _scene_port(transport)
    port.prepare_initial_scene(_scene_context(), _authored_scene(), program_reset_scene_authorization=authorization)
    assert transport.forwarded
    assert all(row["program_reset_scene_authorization"] == authorization for row in transport.forwarded)
    assert all("expected_admission_generation" not in row for row in transport.forwarded)


_IDLE_RESET_CLAIM = {"request_owner_epoch": "caller.1", "request_id": "reset.1",
                     "request_payload_digest": _DIGEST_A, "reset_claim_generation": 7}
_RUN_RESET_AUTHORIZATION = {"program_runner_owner_epoch": "runner.1", "program_run_id": "program-run.1",
                            "cycle_claim_generation": 7, "terminal_or_settlement_reference": _DIGEST_A}


@pytest.mark.parametrize(
    ("authorization", "message"),
    [
        ({"program_runner_owner_epoch": "runner.1", "cycle_generation": 3,
          "reset_claim": {**_IDLE_RESET_CLAIM, "reset_claim_generation": 0}},
         "Program Reset scene claim generation is invalid"),
        ({"program_runner_owner_epoch": "runner.1", "cycle_generation": 3,
          "reset_claim": {**_IDLE_RESET_CLAIM, "request_payload_digest": "sha256:bad"}},
         "Program Reset scene evidence digest is invalid"),
        ({**_RUN_RESET_AUTHORIZATION, "cycle_claim_generation": True},
         "Program Reset scene claim generation is invalid"),
        ({**_RUN_RESET_AUTHORIZATION, "terminal_or_settlement_reference": "sha256:bad"},
         "Program Reset scene settlement reference is invalid"),
    ],
)
def test_reset_scene_authorization_rejects_malformed_claim_before_commit(authorization, message):
    class Transport(_SceneTransport):
        def __init__(self):
            super().__init__()
            self.forwarded = []
        def authoring_admission(self):
            raise AssertionError("Reset used ordinary authoring admission")
        def commit_scene_transaction(self, request):
            self.forwarded.append(deepcopy(request))
            raise AssertionError("malformed Reset authorization reached the Scene owner")
    transport = Transport()
    port = _scene_port(transport)
    with pytest.raises(ProductionPortError, match=message):
        port.prepare_initial_scene(_scene_context(), _authored_scene(), program_reset_scene_authorization=authorization)
    assert transport.forwarded == []


def _stopped_motion_for_recovery(*, lost_replies: int = 0):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity("motion-recovery-body")
    started = port.operations.start("motion.move_to_pose", identity, _motion_inputs())
    transport.operations[started.task_id].update(
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
    bodies: list[dict[str, Any]] = []
    revisions: list[int] = []
    lost = [lost_replies]

    def apply(_operation_id, revision, body):
        bodies.append(deepcopy(dict(body)))
        revisions.append(revision)
        if lost[0] > 0:
            lost[0] -= 1
            raise RuntimeError("the reply was lost")
        return {
            "ok": True,
            "result": {
                "request_id": body["request_id"],
                "action": body["action"],
                "accepted": True,
                "resulting_assessment": {"recovery_class": "recovered"},
            },
        }

    transport.apply_operation_recovery = apply
    return SimpleNamespace(
        port=port,
        identity=identity,
        started=started,
        bodies=bodies,
        revisions=revisions,
        transport=transport,
    )


def _moved_on(previous):
    """The owner's observation after a Stop or a refreshed assessment."""

    def observed(operation_id):
        response = deepcopy(previous(operation_id))
        response["recovery_assessment"]["assessment_revision"] = 4
        response["recovery_assessment"]["control_command_epoch"] = "control.epoch.2"
        return response

    return observed


def test_manual_recovery_request_sends_no_fresh_attempt_flag() -> None:
    fixture = _stopped_motion_for_recovery()

    result = fixture.port.operations.recover(
        fixture.started.task_id,
        fixture.identity,
        action="replan_from_stopped_state",
        request_id="operator.retry.1",
        reason="operator Retry",
    )

    assert result["accepted"] is True
    assert len(fixture.bodies) == 1
    assert "fresh_motion_attempt" not in fixture.bodies[0]


def test_close_operation_is_fenced_without_a_fresh_attempt_and_retires_the_task() -> None:
    fixture = _stopped_motion_for_recovery()

    closed = fixture.port.operations.close(
        fixture.started.task_id, fixture.identity, request_id="program-close:1"
    )

    assert closed is True
    (body,) = fixture.bodies
    assert body["action"] == "close_operation"
    assert body["expected_assessment_revision"] == 3
    assert body["expected_stop_certificate_id"] == "certificate.1"
    assert "fresh_motion_attempt" not in body
    assert fixture.started.task_id not in fixture.port._tasks  # Nothing resident.
    # Closing what is no longer retained is the already-closed outcome.
    assert fixture.port.operations.close(
        fixture.started.task_id, fixture.identity, request_id="program-close:1"
    ) is True
    assert len(fixture.bodies) == 1


def test_an_unknown_close_replays_the_exact_original_request_body() -> None:
    fixture = _stopped_motion_for_recovery(lost_replies=1)
    with pytest.raises(OwnerTaskSettlementPendingError):
        fixture.port.operations.close(
            fixture.started.task_id, fixture.identity, request_id="program-close:2"
        )
    # The owner's assessment moves before the replay: a rebuilt request would
    # carry these new fences under the same request identity.
    fixture.transport.operation_recovery = _moved_on(fixture.transport.operation_recovery)

    assert fixture.port.operations.close(
        fixture.started.task_id, fixture.identity, request_id="program-close:2"
    ) is True

    assert len(fixture.bodies) == 2
    assert json.dumps(fixture.bodies[1], sort_keys=True) == json.dumps(
        fixture.bodies[0], sort_keys=True
    )
    assert fixture.revisions[1] == fixture.revisions[0]


def test_an_unknown_close_cannot_be_replaced_by_another_request() -> None:
    fixture = _stopped_motion_for_recovery(lost_replies=1)
    with pytest.raises(OwnerTaskSettlementPendingError):
        fixture.port.operations.close(
            fixture.started.task_id, fixture.identity, request_id="program-close:3"
        )
    with pytest.raises(ProductionPortError, match="changed request"):
        fixture.port.operations.close(
            fixture.started.task_id, fixture.identity, request_id="program-close:other"
        )


def test_a_refused_close_keeps_the_operation_retained_for_reset() -> None:
    fixture = _stopped_motion_for_recovery()

    class _Refusal(RuntimeError):
        code = "operation_recovery_stale"

    def refuse_with_code(_operation_id, revision, body):
        fixture.bodies.append(deepcopy(dict(body)))
        raise _Refusal("operation closure is stale or not eligible")

    fixture.transport.apply_operation_recovery = refuse_with_code
    assert fixture.port.operations.close(
        fixture.started.task_id, fixture.identity, request_id="program-close:4"
    ) is False
    assert fixture.started.task_id in fixture.port._tasks


def test_a_transient_assessment_absence_is_not_a_failure_and_a_malformed_read_is_surfaced() -> None:
    fixture = _stopped_motion_for_recovery()

    def unreachable(_operation_id):
        raise KernelOperationObservationUnavailableError("the owner is unreachable")

    fixture.transport.operation_recovery = unreachable
    transient = fixture.port.operations.observe(fixture.started.task_id)
    assert transient.state is OwnerTaskState.INTERRUPTED
    assert "recovery_assessment" not in transient.metadata
    assert "recovery_assessment_failure" not in transient.metadata

    fixture.transport.operation_recovery = lambda _operation_id: {"ok": True}
    malformed = fixture.port.operations.observe(fixture.started.task_id)
    assert malformed.state is OwnerTaskState.INTERRUPTED
    assert "recovery_assessment" not in malformed.metadata
    failure = malformed.metadata["recovery_assessment_failure"]
    assert failure["reason_code"] == "owner_recovery_assessment_invalid"
    assert "no owner projection" in failure["message"]


def test_motion_participants_carry_no_automatic_retry_context() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    port.operations.start("motion.move_to_pose", _identity("motion-no-context"), _motion_inputs())

    participant = transport.submissions[-1]["participants"][0]
    assert "automatic_retry_context" not in participant
    assert "automatic_retry_context" not in participant["payload"]


def test_a_logical_preemption_is_read_from_the_operation_state_exactly() -> None:
    fixture = _stopped_motion_for_recovery()
    successor = {"operation_id": "op.9", "node_instance_id": "other", "node_attempt_id": "a.9"}
    fixture.transport.operations[fixture.started.task_id].update(
        {"state": "executing", "preemption": {"kind": "preempted", "by": successor}}
    )

    observed = fixture.port.operations.observe(fixture.started.task_id)
    assert observed.state is OwnerTaskState.ACTIVE  # Physically it still moves.
    assert observed.metadata["preemption"] == {"kind": "preempted", "by": successor}

    fixture.port.operations.release_preempted(fixture.started.task_id, fixture.identity)
    assert fixture.started.task_id not in fixture.port._tasks

    fixture = _stopped_motion_for_recovery()
    fixture.transport.operations[fixture.started.task_id]["preemption"] = {
        "kind": "won", "by": successor,
    }
    with pytest.raises(ProductionPortError, match="preemption fact is invalid"):
        fixture.port.operations.observe(fixture.started.task_id)


def _takeover(**overrides: Any) -> dict[str, Any]:
    fact = {
        "incumbent_operation_id": "op.1",
        "path": "spring_back_hold",
        "cause": {"code": "takeover_preparation_late", "message": "late", "details": {}},
        "decided_at_ns": 123,
        "elapsed_since_acceptance_ns": 40_500_000,
        "preparation_budget_ns": 30_000_000,
        "stop_certificate": None,
    }
    fact.update(overrides)
    return fact


def test_a_motion_takeover_is_read_from_the_newcomer_operation_state_exactly() -> None:
    fixture = _stopped_motion_for_recovery()
    operation = fixture.transport.operations[fixture.started.task_id]
    operation["state"] = "executing"
    observed = fixture.port.operations.observe(fixture.started.task_id)
    assert "motion_takeover" not in observed.metadata  # Omitted when absent.

    for fact in (
        _takeover(),
        _takeover(path="moving_replacement", cause=None),
        _takeover(path="ordinary_containment", stop_certificate={"stop_certificate_id": "c"}),
        _takeover(cause={
            "code": "planning_timeout", "message": "out of time",
            "details": {"failure_stage": "spring_back_planning", "planning_budget_ns": "500"},
        }),
    ):
        operation["motion_takeover"] = fact
        observed = fixture.port.operations.observe(fixture.started.task_id)
        assert observed.state is OwnerTaskState.ACTIVE
        assert observed.metadata["motion_takeover"] == fact


@pytest.mark.parametrize(
    "fact",
    [
        _takeover(path="other"),
        _takeover(path=[]),
        _takeover(path={}),
        _takeover(cause={"code": "", "message": "empty code", "details": {}}),
        _takeover(cause={"code": "planning_timeout", "message": "", "details": {}}),
        _takeover(elapsed_since_acceptance_ns=-1),
        _takeover(elapsed_since_acceptance_ns=10**400),
        _takeover(preparation_budget_ns=2**63),
        _takeover(decided_at_ns=-1),
        _takeover(preparation_budget_ns=0),
        _takeover(decided_at_ns=1.5),
        _takeover(cause={"message": "no code"}),
        _takeover(incumbent_operation_id=""),
        _takeover(extra=1),
        {key: value for key, value in _takeover().items() if key != "stop_certificate"},
    ],
)
def test_a_malformed_motion_takeover_is_ignored_and_never_fails_the_node(fact) -> None:
    fixture = _stopped_motion_for_recovery()
    operation = fixture.transport.operations[fixture.started.task_id]
    operation.update({"state": "executing", "motion_takeover": fact})
    observed = fixture.port.operations.observe(fixture.started.task_id)
    assert observed.state is OwnerTaskState.ACTIVE
    assert "motion_takeover" not in observed.metadata


def test_a_newcomer_refused_by_priority_names_the_winner() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    def refuse(body):
        transport.submissions.append(deepcopy(body))
        return {
            "ok": False,
            "error": {
                "code": "refused_by_priority",
                "message": "a higher-priority motion holds the robot",
                "details": {
                    "winner_operation_id": "op.1",
                    "winner_node_instance_id": "winner",
                    "winner_node_attempt_id": "a.1",
                },
            },
        }

    transport.submit = refuse
    with pytest.raises(OwnerInvocationRefusedByPriorityError) as raised:
        port.operations.start("motion.move_to_pose", _identity("motion-refused"), _motion_inputs())
    assert raised.value.winner == {
        "operation_id": "op.1",
        "node_instance_id": "winner",
        "node_attempt_id": "a.1",
    }
    assert port._tasks == {}


def test_a_failed_operation_carries_its_own_failure_facts() -> None:
    fixture = _stopped_motion_for_recovery()
    operation = fixture.transport.operations[fixture.started.task_id]
    operation.update(
        {
            "state": "failed",
            "participants": [
                {
                    "state": "failed",
                    "resources": ["robot.arm", "robot.arm.joints"],
                    "terminal_error": {"code": "planning_failed", "message": "no path"},
                    "terminal_fact": {"kind": "local_failure", "effect": "no_effect", "error": None},
                }
            ],
        }
    )
    read = fixture.transport.read
    fixture.transport.read = lambda operation_id: {**read(operation_id), "owner_epoch": "kernel.epoch.7"}

    observed = fixture.port.operations.observe(fixture.started.task_id)

    assert observed.metadata["operation_failure"] == {
        "operation": {
            "source_owner_epoch": "kernel.epoch.7",
            "operation_id": fixture.started.task_id,
            "operation_revision": 2,
        },
        "resource_ids": ["robot.arm", "robot.arm.joints"],
        "failure": {"code": "planning_failed", "message": observed.message},
        "terminal_facts": [{"kind": "local_failure", "effect": "no_effect"}],
    }


def test_an_observed_operation_carries_its_exact_state_and_residency() -> None:
    fixture = _stopped_motion_for_recovery()
    observed = fixture.port.operations.observe(fixture.started.task_id)
    assert observed.metadata["operation_state"] == "stopped"
    assert observed.metadata["resident"] is True

    # An exact successful read showing the operation no longer resident:
    # its retirement already completed (SX-1974).
    fixture.transport.resident[observed.metadata["operation_id"]] = False
    retired = fixture.port.operations.observe(fixture.started.task_id)
    assert retired.metadata["resident"] is False


@pytest.mark.parametrize('node_id', ['motion.move_to_pose', 'motion.probe_search'])
def test_qualified_tcp_endpoint_is_identical_for_execution_and_preview(node_id) -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    inputs = _contact_pose_inputs() if node_id == 'motion.move_to_pose' else _interrupt_inputs(node_id)
    parameters = dict(inputs.parameters)
    parameters['tcp_endpoint'] = {'target_role': 'gripper', 'frame_role': 'tcp'}
    identity = _identity('endpoint-preview')
    port.operations.start(node_id, identity, OwnerNodeInputs(parameters))
    payload = transport.submissions[0]['participants'][0]['payload']
    assert payload['tcp_endpoint'] == {'target_role_id': 'gripper', 'frame_role_id': 'tcp'}
    assert 'tcp_frame_role_id' not in payload
    # Exercise the first real strict ingress, rather than only our recording
    # transport: the previous legacy-only model rejected this emitted payload.
    from manyforge_headless_api.models import MotionParticipantRequest

    validated = MotionParticipantRequest.model_validate(payload)
    wire = validated.model_dump(mode="json")
    assert wire["tcp_endpoint"] == payload["tcp_endpoint"]
    assert "tcp_frame_role_id" not in wire
    assert MotionParticipantRequest.model_validate_json(
        validated.model_dump_json()
    ).model_dump(mode="json") == wire
    _, preview = port.motion_preview_intent(identity, node_id, parameters, request_id='endpoint.preview')
    preview_payload = preview['participants'][0]['payload']
    assert {**preview_payload, 'move_id': None} == {**payload, 'move_id': None}
    assert len(transport.submissions) == 1


def test_pick_place_phase_motion_preserves_qualified_tcp_endpoint() -> None:
    parameters = {'base_frame_role': 'base', 'tcp_endpoint': {'target_role': 'gripper', 'frame_role': 'tcp'}}
    payload = NativeKernelProgramPort._phase_motion_payload(
        identity=_identity('pick-endpoint'), parameters=parameters,
        phase='approach', motion={
            'kind': 'pose_goal',
            'goal': {
                'position_m': {'x_m': 0.0, 'y_m': 0.0, 'z_m': 0.1},
                'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
            },
        },
    )
    assert payload['tcp_endpoint'] == {'target_role_id': 'gripper', 'frame_role_id': 'tcp'}
    assert 'tcp_frame_role_id' not in payload
    from manyforge_headless_api.models import MotionParticipantRequest

    wire = MotionParticipantRequest.model_validate(payload).model_dump(mode="json")
    assert wire["tcp_endpoint"] == payload["tcp_endpoint"]
    assert "tcp_frame_role_id" not in wire
    parameters['tcp_frame_role'] = ''
    with pytest.raises(ProductionPortError, match='tcp_frame_role is unsupported'):
        NativeKernelProgramPort._phase_motion_payload(
            identity=_identity('pick-endpoint'), parameters=parameters,
            phase='approach', motion={'kind': 'pose_goal'},
        )


@pytest.mark.parametrize('missing', [None, 'contact_policy_id', 'contact_scene_resource_id'])
def test_joint_goal_uses_the_common_exact_contact_binding_before_strict_ingress(missing) -> None:
    parameters = dict(_motion_inputs().parameters)
    parameters.update(
        target_mode='absolute', target_reference=None, offset_reference=None,
        tcp_endpoint={'target_role': 'gripper', 'frame_role': 'tcp'},
        goal=[{'joint_name': 'joint1', 'position_rad': 0.0}],
        contact_policy_id='pick.contact', contact_scene_resource_id='part.7',
    )
    if missing is not None:
        parameters.pop(missing)
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    if missing is not None:
        with pytest.raises(ProductionPortError, match='supplied together'):
            port.operations.start('motion.move_to_joint_target', _identity('withdraw'), OwnerNodeInputs(parameters))
        assert transport.submissions == []
        return
    port.operations.start('motion.move_to_joint_target', _identity('withdraw'), OwnerNodeInputs(parameters))
    payload = transport.submissions[0]['participants'][0]['payload']
    assert payload['motion']['kind'] == 'joint_goal'
    assert payload['contact_policy_id'] == 'pick.contact'
    assert payload['runtime_object_bindings'] == [
        {'runtime_object_id': 'selected_object', 'scene_resource_id': 'part.7'},
    ]
    from manyforge_headless_api.models import MotionParticipantRequest

    parsed = MotionParticipantRequest.model_validate(payload)
    assert parsed.model_dump(mode='json')['tcp_endpoint'] == {
        'target_role_id': 'gripper', 'frame_role_id': 'tcp',
    }


def _thawed(value: Any) -> Any:
    """An ordinary mutable copy of a recursively frozen JSON value."""

    if isinstance(value, Mapping):
        return {key: _thawed(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_thawed(nested) for nested in value]
    return value


def _release_bound_context():
    context = _context()
    binding = _thawed(context.program_binding)
    binding["contact_grasp_bindings"][0]["mode"] = "contact_release"
    return replace(context, program_binding=binding)


def test_contact_release_motion_lowers_only_its_exact_policy_and_object():
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_release_bound_context())
    parameters = dict(_motion_inputs().parameters)
    parameters.update(
        target_mode="absolute", target_reference=None, offset_reference=None,
        tcp_endpoint={"target_role": "gripper", "frame_role": "tcp"},
        goal=[{"joint_name": "joint1", "position_rad": 0.0}],
        contact_policy_id="pick.contact", contact_scene_resource_id="part.7",
    )
    port.operations.start(
        "motion.move_to_joint_target", _identity("release"), OwnerNodeInputs(parameters)
    )
    payload = transport.submissions[0]["participants"][0]["payload"]
    assert payload["motion"]["kind"] == "joint_goal"
    assert payload["tcp_endpoint"] == {"target_role_id": "gripper", "frame_role_id": "tcp"}
    assert payload["contact_policy_id"] == "pick.contact"
    assert payload["runtime_object_bindings"] == [
        {"runtime_object_id": "selected_object", "scene_resource_id": "part.7"},
    ]
    from manyforge_headless_api.models import MotionParticipantRequest

    assert MotionParticipantRequest.model_validate(payload).contact_policy_id == "pick.contact"


def test_contact_release_binding_cannot_be_used_for_pick_synthesis():
    transport = _PickPlaceTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_release_bound_context())
    with pytest.raises(ProductionPortError, match="pick requires a contact-grasp policy"):
        port.operations.start("planning.plan_pick_sequence", _identity("pick"), _pick_inputs())
    assert transport.submissions == []
    assert transport.pick_requests == []
