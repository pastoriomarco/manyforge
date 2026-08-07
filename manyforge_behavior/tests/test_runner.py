"""First Program-Binding Behavior caller remains transport-only."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    KernelClient,
    KernelClientResponseError,
)
from manyforge_behavior.runner import OrchestratorRunner


class RecordingTransport:
    def __init__(self) -> None:
        self.submitted: Mapping[str, Any] | None = None
        self.controls: list[tuple[str, str, Mapping[str, Any]]] = []

    def execution_status(self) -> dict[str, Any]:
        return {
            "ok": True,
            "status": {
                "owner_epoch": "owner-epoch-7",
                "program_id": "program-1",
                "program_revision": "sha256:" + "1" * 64,
                "program_binding_id": "binding-1",
                "graph_revision": "sha256:" + "2" * 64,
                "retained_bundle_id": "sha256:" + "3" * 64,
                "motion_target_role_id": "arm",
                "gripper_target_role_id": "gripper",
                "gripper_command_schema_id": "gripper.command_position",
                "gripper_command_schema_revision": "sha256:" + "5" * 64,
                "scene_revision": {
                    "scene_id": "scene-1",
                    "revision_id": 1,
                    "content_hash": "sha256:" + "4" * 64,
                },
            },
        }

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "accepted": {"operation_id": "operation-1"},
        }

    def operation(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
        }

    def _control(
        self, kind: str, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.controls.append((kind, operation_id, body))
        return {
            "ok": True,
            "accepted": {
                "operation_id": operation_id,
                "operations": [operation_id],
                "stop_id": "stop-1",
            },
        }

    def pause_operation(self, operation_id, body):
        return self._control("pause", operation_id, body)

    def resume_operation(self, operation_id, body):
        return self._control("resume", operation_id, body)

    def reset_operation(self, operation_id, body):
        return self._control("reset", operation_id, body)

    def acknowledge_completed_operation(self, operation_id, body):
        return self._control("acknowledge_completed", operation_id, body)

    def stop_operations(self, body):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)

    def set_operation_runtime_scale(self, operation_id, body):
        return self._control("runtime_scale", operation_id, body)

    def operation_runtime_scale(self, operation_id):
        return {
            "ok": True,
            "runtime_scale": {"operation_id": operation_id},
        }


def test_runner_submits_source_joint_names_against_exact_owner_selection() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport))

    accepted = runner.submit_joint_goal(
        request_id="request-1",
        program_run_id="run-1",
        component_role_id="robot",
        target_role_id="arm",
        node_instance_id="root/move[0]",
        node_attempt_id="run-1/move[0]/attempt-1",
        move_id="move-1",
        joint_names=("joint2", "joint1"),
        positions_rad=(0.2, -0.1),
    )

    assert accepted["accepted"]["operation_id"] == "operation-1"
    assert transport.submitted is not None
    request = transport.submitted
    assert request["request_context"]["request_owner_epoch"] == "owner-epoch-7"
    assert request["program_binding_id"] == "binding-1"
    assert request["expected_retained_bundle_id"] == "sha256:" + "3" * 64
    participant = request["participants"][0]
    assert participant["component_role_id"] == "robot"
    assert participant["target_role_id"] == "arm"
    assert participant["payload"]["motion"] == {
        "kind": "joint_goal",
        "joint_names": ["joint2", "joint1"],
        "positions_rad": [0.2, -0.1],
    }
    assert "robot_id" not in repr(request)
    assert "group_id" not in repr(request)


def test_runner_reads_owner_projection_without_reducing_it() -> None:
    runner = OrchestratorRunner(KernelClient(RecordingTransport()))
    assert runner.operation("operation-1")["operation"] == {
        "operation_id": "operation-1",
        "state": "completed",
    }


def test_runner_submits_unit_tagged_gripper_command_to_selected_target() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport))

    accepted = runner.submit_gripper_position(
        request_id="request-gripper-1",
        program_run_id="run-1",
        component_role_id="robot",
        target_role_id="gripper",
        node_instance_id="root/gripper[0]",
        node_attempt_id="run-1/gripper[0]/attempt-1",
        position_kind="linear_m",
        position=0.004,
        max_effort=5.0,
        command_duration_s=0.25,
        completion_timeout_s=2.0,
    )

    assert accepted["accepted"]["operation_id"] == "operation-1"
    assert transport.submitted is not None
    participant = transport.submitted["participants"][0]
    assert participant["target_role_id"] == "gripper"
    assert participant["required_capability_id"] == "tool.gripper.command"
    assert participant["payload"] == {
        "kind": "capability_command",
        "command_schema_id": "gripper.command_position",
        "command_schema_revision": "sha256:" + "5" * 64,
        "command_value": {
            "kind": "gripper_position",
            "position": {"kind": "linear_m", "value_m": 0.004},
            "max_effort": {"kind": "linear_newton", "value_n": 5.0},
        },
        "completion_timeout_s": 2.0,
        "caller_options": [
            {"option_id": "command_duration_s", "value_s": 0.25}
        ],
    }
    assert "robot_id" not in repr(transport.submitted)
    assert "group_id" not in repr(transport.submitted)


def test_runner_stop_is_attributed_fenced_and_exactly_replayable() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport))

    first = runner.stop_operation(
        request_id="request-stop-1",
        operation_id="operation-1",
        expected_operation_revision=4,
        expected_stop_generation=0,
        reason="operator requested Stop",
        correlation_id="cycle-1",
    )
    replay = runner.stop_operation(
        request_id="request-stop-1",
        operation_id="operation-1",
        expected_operation_revision=4,
        expected_stop_generation=0,
        reason="operator requested Stop",
        correlation_id="cycle-1",
    )

    assert first["accepted"]["stop_id"] == "stop-1"
    assert replay["accepted"]["stop_id"] == "stop-1"
    assert transport.controls[0] == transport.controls[1]
    kind, operation_id, request = transport.controls[0]
    assert kind == "stop"
    assert operation_id == "operation-1"
    assert request == {
        "request_context": {
            "request_id": "request-stop-1",
            "request_owner_epoch": "owner-epoch-7",
            "source_label": "manyforge_behavior",
            "correlation_id": "cycle-1",
        },
        "operations": [
            {
                "operation_id": "operation-1",
                "expected_operation_revision": 4,
                "expected_stop_generation": 0,
            }
        ],
        "reason": "operator requested Stop",
    }


def test_runner_rejects_an_incomplete_owner_projection_before_submission() -> None:
    class IncompleteStatusTransport(RecordingTransport):
        def execution_status(self) -> dict[str, Any]:
            return {"ok": True, "status": {"owner_epoch": "owner-epoch-7"}}

    transport = IncompleteStatusTransport()
    runner = OrchestratorRunner(KernelClient(transport))

    with pytest.raises(KernelClientResponseError, match="incomplete"):
        runner.submit_joint_goal(
            request_id="request-1",
            program_run_id="run-1",
            component_role_id="robot",
            target_role_id="arm",
            node_instance_id="root/move[0]",
            node_attempt_id="run-1/move[0]/attempt-1",
            move_id="move-1",
            joint_names=("joint1",),
            positions_rad=(0.1,),
        )

    assert transport.submitted is None
