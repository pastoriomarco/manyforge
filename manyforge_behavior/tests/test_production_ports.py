"""Captured Program operation ports remain thin, exact native clients."""

from __future__ import annotations

from copy import deepcopy
from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
    ProgramRunScopeSnapshot,
    ProgramRunScopeState,
)
from manyforge_behavior.production_ports import (
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

    def execution_status(self) -> dict[str, Any]:
        return {
            "ok": True,
            "status": {
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

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
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
        return {
            "ok": True,
            "accepted": {
                "operation_id": operation_id,
                "operation_revision": 1,
            },
        }

    def operation(self, operation_id: str) -> dict[str, Any]:
        return {"ok": True, "operation": deepcopy(self.operations[operation_id])}

    def _control(
        self, kind: str, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
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

    def pause_operation(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("pause", operation_id, body)

    def resume_operation(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("resume", operation_id, body)

    def reset_operation(self, operation_id: str, body: Mapping[str, Any]):
        return self._control("reset", operation_id, body)

    def acknowledge_completed_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ):
        return self._control("acknowledge", operation_id, body)

    def stop_operations(self, body: Mapping[str, Any]):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)

    def set_operation_runtime_scale(
        self, operation_id: str, body: Mapping[str, Any]
    ):
        return self._control("scale", operation_id, body)

    def operation_runtime_scale(self, operation_id: str):
        return {"ok": True, "runtime_scale": {"operation_id": operation_id}}


def _context() -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="program-run.1",
        scenario_id="lite6_pick_and_place",
        scenario_catalog_revision=_DIGEST_A,
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
                        }
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
                        }
                    ],
                },
            ],
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


def test_relative_pose_submission_uses_capture_and_preserves_portable_roles() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    started = port.operations.start(
        "motion.move_to_pose", _identity(), _motion_inputs()
    )

    assert started.state is OwnerTaskState.ACTIVE
    request = transport.submissions[0]
    assert request["program_id"] == "lite6_program"
    assert request["program_binding_id"] == _DIGEST_A
    assert request["expected_scene_revision"]["revision_id"] == 4
    participant = request["participants"][0]
    assert participant["component_role_id"] == "robot"
    assert participant["target_role_id"] == "arm"
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


def test_completed_operation_is_acknowledged_with_exact_revision() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    identity = _identity()
    started = port.operations.start(
        "motion.move_to_pose", identity, _motion_inputs()
    )
    transport.operations[started.task_id].update(
        {"state": "completed", "operation_revision": 7}
    )

    observed = port.operations.observe(started.task_id)
    assert isinstance(observed, OwnerTaskSnapshot)
    assert observed.state is OwnerTaskState.SUCCEEDED
    port.operations.acknowledge(started.task_id, identity)

    kind, operation_id, body = transport.controls[-1]
    assert (kind, operation_id) == ("acknowledge", started.task_id)
    assert body["expected_operation_revision"] == 7
    scope = port.scope.observe("program-run.1")
    assert isinstance(scope, ProgramRunScopeSnapshot)
    assert scope.state is ProgramRunScopeState.SETTLED


def test_program_scope_stop_is_exact_and_not_repeated() -> None:
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity(), _motion_inputs()
    )
    transport.operations[started.task_id].update(
        {"state": "executing", "operation_revision": 3}
    )

    first = port.scope.request_stop("program-run.1", "Program Stop")
    port.scope.request_stop("program-run.1", "Program Stop")

    assert first.state is ProgramRunScopeState.ACTIVE
    stop_calls = [item for item in transport.controls if item[0] == "stop"]
    assert len(stop_calls) == 1
    body = stop_calls[0][2]
    assert body["operations"] == [
        {
            "operation_id": started.task_id,
            "expected_operation_revision": 3,
            "expected_stop_generation": 0,
        }
    ]


def test_failed_stop_submission_can_be_retried() -> None:
    class _OneStopFailureTransport(_Transport):
        def __init__(self) -> None:
            super().__init__()
            self.fail_next_stop = True

        def stop_operations(self, body: Mapping[str, Any]):
            if self.fail_next_stop:
                self.fail_next_stop = False
                raise RuntimeError("temporary Stop transport failure")
            return super().stop_operations(body)

    transport = _OneStopFailureTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())
    started = port.operations.start(
        "motion.move_to_pose", _identity(), _motion_inputs()
    )
    transport.operations[started.task_id].update(
        {"state": "executing", "operation_revision": 3}
    )

    with pytest.raises(RuntimeError, match="temporary Stop"):
        port.scope.request_stop("program-run.1", "Program Stop")
    port.scope.request_stop("program-run.1", "Program Stop")

    assert [item[0] for item in transport.controls] == ["stop"]


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

    port.operations.start(
        "gripper.command_position", _identity("open-gripper"), inputs
    )

    participant = transport.submissions[0]["participants"][0]
    assert participant["component_role_id"] == "tool"
    assert participant["payload"]["command_schema_revision"] == _DIGEST_E


def test_stale_native_deployment_fails_before_dispatch() -> None:
    class _StaleTransport(_Transport):
        def execution_status(self) -> dict[str, Any]:
            result = super().execution_status()
            result["status"]["graph_revision"] = _DIGEST_F
            return result

    transport = _StaleTransport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context())

    with pytest.raises(ProductionPortError, match="captured Program"):
        port.operations.start(
            "motion.move_to_pose", _identity(), _motion_inputs()
        )

    assert transport.submissions == []
