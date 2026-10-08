"""V13.7.5: the node-correction preview is one effect-free query.

A real Program owner runs the minimal Program until its motion node fails
into a held failure pause. Its owner request goes through the production
motion builder, so the preview can be compared with what execution sent.
"""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any, Mapping

import pytest

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.executable_node_resolver import (
    build_executable_node_resolver,
)
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    NodeExecutionReadiness,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_ports import (
    CompositeProgramRunContextPort,
    NodeAdapterPorts,
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.production_ports import NativeKernelProgramPort
from manyforge_behavior.program import Program
from manyforge_behavior.program_run_owner import (
    ProgramRunAdmission,
    ProgramRunOwner,
    ProgramRunState,
)
from manyforge_behavior.program_runtime import ProgramRunOwnerIdentity
from manyforge_behavior.program_scheduling import ProgramInterventionRegistry
from manyforge_headless_api.models import (
    OperationIntentRequest,
    ProgramNodeCorrectionPreviewRequest,
)
from manyforge_program_runner.runtime import ProgramRunnerError, ProgramRuntime


_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_DIGEST_D = "sha256:" + "d" * 64
_DIGEST_E = "sha256:" + "e" * 64
_RUN_OWNER_EPOCH = "program-runner.epoch.1"
_NODE = "move_to_pick"


class _KernelTransport:
    """Native owner fake: identity reads are allowed, every effect is recorded."""

    def __init__(self) -> None:
        self.submissions: list[dict[str, Any]] = []
        self.effects: list[str] = []
        self.identity_reads = 0

    def identity(self) -> dict[str, Any]:
        self.identity_reads += 1
        return {
            "ok": True,
            "identity": {
                "owner_epoch": "kernel.epoch.1",
                "graph_revision": _DIGEST_C,
                "retained_bundle_id": _DIGEST_B,
                "scene_revision": {
                    "scene_id": _DIGEST_D,
                    "revision_id": 3,
                    "content_hash": _DIGEST_E,
                },
            },
        }

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submissions.append(deepcopy(dict(body)))
        return {
            "ok": True,
            "accepted": {"operation_id": "operation-1", "operation_revision": 1},
        }

    def _effect(self, name: str):
        def record(*_args: Any, **_kwargs: Any) -> dict[str, Any]:
            self.effects.append(name)
            raise AssertionError(f"preview must not call {name}")

        return record

    def __getattr__(self, name: str):
        if name in {
            "read",
            "reset",
            "acknowledge_completed",
            "stop",
            "recover",
            "operation_recovery",
            "apply_operation_recovery",
        }:
            return self._effect(name)
        raise AttributeError(name)


class _FailingMotionPort:
    """Send the node's request through the execution builder, then fail it
    pre-dispatch into an operator failure pause (like `native_ik_no_solution`)."""

    def __init__(self, native: NativeKernelProgramPort) -> None:
        self.native = native
        self.starts = 0
        self.recoveries = 0
        self.cancels = 0

    def start(
        self, node_id: str, identity: NodeAttemptIdentity, inputs: OwnerNodeInputs
    ) -> OwnerTaskSnapshot:
        self.starts += 1
        self.native.operations.start(node_id, identity, inputs)
        return OwnerTaskSnapshot(
            task_id=f"task-{self.starts}",
            state=OwnerTaskState.FAILED,
            metadata={
                "authored_failure_policy": "pause_for_operator",
                "authored_failure_policy_parameter": "no_feasible_plan_policy",
            },
            reason_code="native_ik_no_solution",
            message="IK has no solution for the goal",
        )

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return OwnerTaskSnapshot(task_id=task_id, state=OwnerTaskState.ACTIVE)

    def cancel(self, _task_id: str, _identity: NodeAttemptIdentity) -> None:
        self.cancels += 1

    def acknowledge(self, _task_id: str, _identity: NodeAttemptIdentity) -> None:
        raise AssertionError("a failed attempt is never acknowledged")

    def recover(self, *_args: Any, **_kwargs: Any) -> Mapping[str, Any]:
        self.recoveries += 1
        raise AssertionError("preview must not recover or retry")


class _InvocationPort:
    def invoke(self, *_args: Any) -> OwnerInvocationResult:
        return OwnerInvocationResult(True)


class _ScopePort:
    def __init__(self) -> None:
        self.stop_requests = 0
        self.release_requests = 0
        self.selection_revision = 0
        self.held = False
        self.node_count = 0

    def bind(self, context: ProgramRunPortContext) -> None:
        pending = [context.program["tree"]]
        while pending:
            node = pending.pop()
            self.node_count += 1
            pending.extend(node.get("children", ()))

    def unbind(self, _program_run_id: str) -> None:
        pass

    def execution_scope(self, program_run_id: str) -> Mapping[str, Any]:
        return {
            "schema_version": "manyforge.program_execution_scope.v2",
            "request_owner_epoch": "kernel.epoch.1",
            "program_runner_owner_epoch": _RUN_OWNER_EPOCH,
            "claim_generation": 1,
            "program_run_id": program_run_id,
            "selection_revision": self.selection_revision,
            "held_node_indices": list(range(self.node_count)) if self.held else [],
            "ready_node_indices": [],
            "stopping_node_indices": [],
            "faulted_node_indices": [],
        }

    def stop_selection(self, request):
        self.request_stop(request["program_run_id"], "", None)
        return {
            "state": self.execution_scope(request["program_run_id"]),
            "selection_error": None,
        }

    def release_selection(self, request, _expected_selection_revision):
        self.release_requests += 1
        raise AssertionError("preview must not release a hold")

    def start_selection(self, request, _expected_selection_revision):
        raise AssertionError("preview must not start motion")

    def request_stop(self, program_run_id, _request_id, _reason):
        self.stop_requests += 1
        self.held = True
        self.selection_revision += 1
        return ProgramRunScopeSnapshot(program_run_id, ProgramRunScopeState.SETTLED, 0)

    def observe(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        return ProgramRunScopeSnapshot(program_run_id, ProgramRunScopeState.SETTLED, 0)

    def capture_stop(self, _program_run_id):
        return ("stop.1", 1) if self.stop_requests else None

    def release_stop(self, program_run_id, *, expected_stop=None):
        self.release_requests += 1
        raise AssertionError("preview must not release a Stop")

    def reset(self, program_run_id: str) -> ProgramRunScopeSnapshot:
        raise AssertionError("preview must not reset")


def _held_run():
    environment = build_node_environment()
    program = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml",
        NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml"),
    )
    transport = _KernelTransport()
    native = NativeKernelProgramPort(KernelClient(transport))  # type: ignore[arg-type]
    motion = _FailingMotionPort(native)
    scope = _ScopePort()
    interventions = ProgramInterventionRegistry(maximum_retained_runs=8)
    invocation = _InvocationPort()
    ports = NodeAdapterPorts(
        kernel_operations=motion,
        scene_transactions=invocation,
        gripper_status=invocation,
        model_queries=invocation,
        kernel_controls=invocation,
        program_run_scope=scope,
        program_run_context=CompositeProgramRunContextPort(scope, native),
        program_interventions=interventions,
    )
    resolver = build_executable_node_resolver(environment)
    readiness = NodeExecutionReadiness(
        program_binding_id=_DIGEST_A,
        binding_by_node={
            disposition.row.node_id: AvailabilityFact(True)
            for disposition in resolver.dispositions
        },
        owner_by_port={
            disposition.owner_port_name: AvailabilityFact(True)
            for disposition in resolver.dispositions
            if disposition.owner_port_name is not None
        },
    )
    admission = ProgramRunAdmission(
        scenario_id="ur5-pick-place",
        applied_record_digest="sha256:" + "9" * 64,
        source_document_digest=_DIGEST_A,
        program=program,
        environment=environment,
        program_binding_id=_DIGEST_A,
        retained_bundle_id=_DIGEST_B,
        graph_revision=_DIGEST_C,
        program_binding={
            "program_binding_id": _DIGEST_A,
            "program_id": program.program_id,
            "program_revision": program.program_revision,
            "retained_bundle_id": _DIGEST_B,
            "graph_revision": _DIGEST_C,
            "node_policies": [],
            "target_role_bindings": [
                {
                    "target_role_id": "arm",
                    "component_role_id": "robot",
                    "controllable_component_id": "ur5.1",
                    "command_target_id": "arm.command",
                    "capability_bindings": [
                        {"capability_id": "execution.trajectory.execute"},
                    ],
                }
            ],
            "contact_grasp_bindings": [],
        },
        readiness=readiness,
        owner_identities=(
            ProgramRunOwnerIdentity(
                owner_role="kernel_operations",
                owner_id="kernel.cell-a",
                owner_epoch="kernel.epoch.1",
            ),
        ),
    )
    owner = ProgramRunOwner(
        owner_id="program-runner.cell-a",
        owner_epoch=_RUN_OWNER_EPOCH,
        ports=ports,
        run_id_factory=lambda: "program-run-1",
    )
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)
    assert paused.state is ProgramRunState.INTERVENTION_REQUIRED
    held = interventions.read(run_id)
    assert held is not None and held.identity.node_instance_id == _NODE
    runtime = object.__new__(ProgramRuntime)
    runtime._closed = False  # type: ignore[attr-defined]  # noqa: SLF001
    runtime._owner = owner  # type: ignore[attr-defined]  # noqa: SLF001
    runtime._native = native  # type: ignore[attr-defined]  # noqa: SLF001
    return runtime, owner, transport, motion, scope, interventions, run_id, held


def _request(owner, run_id, held, **changes) -> ProgramNodeCorrectionPreviewRequest:
    capture = owner.read(run_id).capture
    body = {
        "schema_version": "manyforge.program_node_correction_preview_request.v1",
        "request_id": "node-preview.1",
        "program_run_id": run_id,
        "expected_owner_epoch": _RUN_OWNER_EPOCH,
        "program_id": capture.program_id,
        "program_revision": capture.program_revision,
        "program_binding_id": capture.program_binding_id,
        "node_instance_id": _NODE,
        "node_attempt_id": held.identity.node_attempt_id,
        "proposed_inputs": [],
        **changes,
    }
    return ProgramNodeCorrectionPreviewRequest.model_validate(body)


def _effects(owner, transport, motion, scope, interventions, run_id):
    live = owner.read(run_id)
    run = owner._runs[run_id].run  # noqa: SLF001
    return {
        "submissions": deepcopy(transport.submissions),
        "native_effects": list(transport.effects),
        "starts": motion.starts,
        "recoveries": motion.recoveries,
        "cancels": motion.cancels,
        "stops": scope.stop_requests,
        "releases": scope.release_requests,
        "selection_revision": scope.selection_revision,
        "interventions": [
            (row.identity, row.policy_parameter) for row in interventions.read_all(run_id)
        ],
        "state": live.state,
        "run_revision": live.run_revision,
        "node_statuses": dict(live.node_statuses),
        "values": run.value_store.diagnostic_snapshot(),
    }


_NEW_GOAL = [
    {"joint_name": "shoulder_pan_joint", "position_rad": 0.1},
    {"joint_name": "shoulder_lift_joint", "position_rad": -1.0},
    {"joint_name": "elbow_joint", "position_rad": 0.9},
    {"joint_name": "wrist_1_joint", "position_rad": -1.4},
    {"joint_name": "wrist_2_joint", "position_rad": -1.2},
    {"joint_name": "wrist_3_joint", "position_rad": 0.3},
]


def test_held_run_preview_is_the_execution_request_and_has_no_effects() -> None:
    runtime, owner, transport, motion, scope, interventions, run_id, held = _held_run()
    executed = transport.submissions[0]
    executed_payload = executed["participants"][0]["payload"]
    before = _effects(owner, transport, motion, scope, interventions, run_id)

    exact = runtime.preview_node_correction(run_id, _request(owner, run_id, held))
    proposed = runtime.preview_node_correction(
        run_id,
        _request(
            owner,
            run_id,
            held,
            request_id="node-preview.2",
            proposed_inputs=[
                {"path": "goal", "value": _NEW_GOAL},
                # Shadows the node's Program-parameter value source.
                {"path": "velocity_scale", "value": 0.25},
            ],
        ),
    )

    assert _effects(owner, transport, motion, scope, interventions, run_id) == before
    for result in (exact, proposed):
        assert result["unavailable_reason"] is None
        assert result["node_id"] == "motion.move_to_joint_target"
        assert result["node_attempt_id"] == held.identity.node_attempt_id
        assert result["program_runner_owner_epoch"] == _RUN_OWNER_EPOCH
        assert result["target"] == {
            "target_role_id": "arm",
            "component_role_id": "robot",
            "controllable_component_id": "ur5.1",
            "command_target_id": "arm.command",
        }
        operation = result["operation"]
        # The Headless manual-preview wire accepts the intent unchanged.
        OperationIntentRequest.model_validate(operation)
        assert {
            key: value
            for key, value in operation.items()
            if key not in {"request_context", "participants"}
        } == {
            key: value
            for key, value in executed.items()
            if key not in {"request_context", "participants"}
        }
        participant = operation["participants"][0]
        assert "execution_scope" not in participant
        assert participant["action_span"] is None
    assert exact["proposed_paths"] == []
    exact_payload = exact["operation"]["participants"][0]["payload"]
    assert {**exact_payload, "move_id": None} == {**executed_payload, "move_id": None}
    assert exact_payload["speed_profile"]["velocity_scale"] == 0.5
    proposed_payload = proposed["operation"]["participants"][0]["payload"]
    assert proposed["proposed_paths"] == ["goal", "velocity_scale"]
    assert proposed_payload["motion"]["positions_rad"] == [
        row["position_rad"] for row in _NEW_GOAL
    ]
    assert proposed_payload["speed_profile"]["velocity_scale"] == 0.25
    assert {
        **proposed_payload,
        "move_id": None,
        "motion": None,
        "speed_profile": None,
    } == {**executed_payload, "move_id": None, "motion": None, "speed_profile": None}
    assert exact["parameters_digest"] != proposed["parameters_digest"]


@pytest.mark.parametrize(
    ("changes", "code"),
    [
        ({"expected_owner_epoch": "program-runner.epoch.0"}, "program_node_preview_identity_stale"),
        ({"program_id": "another_program"}, "program_node_preview_identity_stale"),
        ({"node_attempt_id": "attempt:foreign"}, "program_node_preview_attempt_stale"),
        ({"node_instance_id": "remove_table"}, "program_node_preview_attempt_stale"),
        (
            {"proposed_inputs": [{"path": "target_role", "value": "other_arm"}]},
            "program_node_preview_input_invalid",
        ),
        (
            {"proposed_inputs": [{"path": "planning_profile", "value": "quality_bounded"}]},
            "program_node_preview_input_invalid",
        ),
        (
            {"proposed_inputs": [{"path": "speed", "value": 1.0}]},
            "program_node_preview_input_invalid",
        ),
        (
            {"proposed_inputs": [{"path": "velocity_scale", "value": "fast"}]},
            "program_node_preview_input_invalid",
        ),
        (
            {"proposed_inputs": [{"path": "goal[9].position_rad", "value": 0.1}]},
            "program_node_preview_input_invalid",
        ),
        (
            {
                "proposed_inputs": [
                    {"path": "goal", "value": _NEW_GOAL},
                    {"path": "goal[0].position_rad", "value": 0.1},
                ]
            },
            "program_node_preview_input_invalid",
        ),
    ],
)
def test_foreign_or_invalid_preview_input_is_refused_without_effect(changes, code) -> None:
    runtime, owner, transport, motion, scope, interventions, run_id, held = _held_run()
    before = _effects(owner, transport, motion, scope, interventions, run_id)

    with pytest.raises(ProgramRunnerError) as refused:
        runtime.preview_node_correction(run_id, _request(owner, run_id, held, **changes))

    assert refused.value.code == code
    assert _effects(owner, transport, motion, scope, interventions, run_id) == before


def test_deep_proposal_patches_the_resolved_value() -> None:
    runtime, owner, transport, motion, scope, interventions, run_id, held = _held_run()

    result = runtime.preview_node_correction(
        run_id,
        _request(
            owner,
            run_id,
            held,
            proposed_inputs=[{"path": "goal[0].position_rad", "value": -0.3}],
        ),
    )

    positions = result["operation"]["participants"][0]["payload"]["motion"]["positions_rad"]
    assert positions[0] == -0.3
    assert positions[1:] == [-1.2, 1.0, -1.5, -1.3, 0.4]


def test_path_mismatch_is_refused() -> None:
    runtime, owner, _transport, _motion, _scope, _interventions, run_id, held = _held_run()

    with pytest.raises(ProgramRunnerError) as refused:
        runtime.preview_node_correction("program-run-2", _request(owner, run_id, held))

    assert refused.value.code == "program_run_path_invalid"
