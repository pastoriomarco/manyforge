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

    def identity(self) -> dict[str, Any]:
        # execution_identity.v2: runtime identity only (H1a-IF.2).
        return {
            "ok": True,
            "identity": {
                "schema_version": "manyforge.execution_identity.v2",
                "owner_epoch": "owner-epoch-7",
                "cell_id": "cell-1",
                "active_publication_generation": 1,
                "control_management_mode": "component_lifecycle",
                "graph_revision": "sha256:" + "2" * 64,
                "retained_bundle_id": "sha256:" + "3" * 64,
            },
        }

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "accepted": {"operation_id": "operation-1"},
        }

    def read(self, operation_id: str) -> dict[str, Any]:
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

    def reset(self, operation_id, body):
        return self._control("reset", operation_id, body)

    def acknowledge_completed(self, operation_id, body):
        return self._control("acknowledge_completed", operation_id, body)

    def stop(self, body):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)


def _capability(capability_id: str, command_schema: dict[str, str] | None) -> dict:
    return {
        "capability_id": capability_id,
        "capability_definition_revision": "sha256:" + "6" * 64,
        "control_binding": {
            "control_binding_id": f"{capability_id}.binding",
            "control_binding_revision": "sha256:" + "7" * 64,
        },
        "capability_provider_binding": None,
        "command_schema": command_schema,
    }


def _applied_binding() -> dict[str, Any]:
    """The Applied Program binding a run was admitted with (subset)."""

    return {
        "program_id": "program-1",
        "program_revision": "sha256:" + "1" * 64,
        "program_binding_id": "binding-1",
        "graph_revision": "sha256:" + "2" * 64,
        "retained_bundle_id": "sha256:" + "3" * 64,
        "target_role_bindings": [
            {
                "target_role_id": "arm",
                "component_role_id": "robot",
                "controllable_component_id": "lite6_arm",
                "command_target_id": "arm",
                "robot_tcps": [{"entry": {"component_id": "mounted_tool",
                                           "target_id": "gripper", "frame_role_id": "tip"},
                                "end_effector": True}],
                "capability_bindings": [
                    _capability("execution.trajectory.execute", None)
                ],
            },
            {
                "target_role_id": "gripper",
                "component_role_id": "robot",
                "controllable_component_id": "lite6_gripper",
                "command_target_id": "gripper",
                "capability_bindings": [
                    _capability(
                        "tool.gripper.command",
                        {
                            "command_schema_id": "gripper.command_position",
                            "command_schema_revision": "sha256:" + "5" * 64,
                        },
                    )
                ],
            },
        ],
    }


def test_runner_submits_source_joint_names_against_exact_owner_selection() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())

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
    assert participant["payload"]["tcp_endpoint"] == {
        "kind": "entry", "entry": {
            "component_id": "mounted_tool", "target_id": "gripper", "frame_role_id": "tip",
        },
    }
    assert participant["payload"]["motion"] == {
        "kind": "joint_goal",
        "joint_names": ["joint2", "joint1"],
        "positions_rad": [0.2, -0.1],
    }
    assert "robot_id" not in repr(request)
    assert "group_id" not in repr(request)


def test_runner_refuses_joint_goal_without_end_effector_tcp() -> None:
    transport = RecordingTransport()
    binding = _applied_binding()
    binding["target_role_bindings"][0]["robot_tcps"] = []
    runner = OrchestratorRunner(KernelClient(transport), binding)
    with pytest.raises(ValueError, match="no unique end-effector TCP"):
        runner.submit_joint_goal(
            request_id="request-1", program_run_id="run-1", component_role_id="robot",
            target_role_id="arm", node_instance_id="move", node_attempt_id="attempt",
            move_id="move-1", joint_names=("joint1",), positions_rad=(0.1,),
        )
    assert transport.submitted is None


def test_runner_reads_owner_projection_without_reducing_it() -> None:
    runner = OrchestratorRunner(KernelClient(RecordingTransport()), _applied_binding())
    assert runner.operation("operation-1")["operation"] == {
        "operation_id": "operation-1",
        "state": "completed",
    }


def test_runner_submits_unit_tagged_gripper_command_to_selected_target() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())

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


def test_runner_passes_wait_for_duration_as_a_bool_caller_option() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())

    runner.submit_gripper_position(
        request_id="request-gripper-1",
        program_run_id="run-1",
        component_role_id="robot",
        target_role_id="gripper",
        node_instance_id="root/gripper[0]",
        node_attempt_id="run-1/gripper[0]/attempt-1",
        position_kind="linear_m",
        position=0.004,
        command_duration_s=0.25,
        wait_for_duration=True,
        completion_timeout_s=2.0,
    )

    assert transport.submitted is not None
    assert transport.submitted["participants"][0]["payload"]["caller_options"] == [
        {"option_id": "command_duration_s", "value_s": 0.25},
        {"option_id": "wait_for_duration", "value_bool": True},
    ]


def test_runner_stop_is_attributed_fenced_and_exactly_replayable() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())

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
    class IncompleteIdentityTransport(RecordingTransport):
        def identity(self) -> dict[str, Any]:
            return {"ok": True, "identity": {"owner_epoch": "owner-epoch-7"}}

    transport = IncompleteIdentityTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())

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


def test_runner_takes_program_identity_from_the_applied_binding_only() -> None:
    transport = RecordingTransport()
    runner = OrchestratorRunner(KernelClient(transport), _applied_binding())
    arguments = {
        "request_id": "request-1",
        "program_run_id": "run-1",
        "node_instance_id": "root/move[0]",
        "node_attempt_id": "run-1/move[0]/attempt-1",
        "move_id": "move-1",
        "joint_names": ("joint1",),
        "positions_rad": (0.1,),
    }

    # A role the applied Program does not bind to motion is refused.
    with pytest.raises(ValueError, match="not bound"):
        runner.submit_joint_goal(
            component_role_id="robot", target_role_id="gripper", **arguments
        )
    with pytest.raises(ValueError, match="not bound"):
        runner.submit_joint_goal(
            component_role_id="other", target_role_id="arm", **arguments
        )
    assert transport.submitted is None

    # The binding runs only against the publication it was compiled for.
    stale = _applied_binding()
    stale["retained_bundle_id"] = "sha256:" + "9" * 64
    stale_runner = OrchestratorRunner(KernelClient(transport), stale)
    with pytest.raises(KernelClientResponseError, match="differs"):
        stale_runner.submit_joint_goal(
            component_role_id="robot", target_role_id="arm", **arguments
        )
    assert transport.submitted is None

    runner.submit_joint_goal(
        component_role_id="robot", target_role_id="arm", **arguments
    )
    assert transport.submitted is not None
    assert transport.submitted["program_id"] == "program-1"
    assert transport.submitted["program_revision"] == "sha256:" + "1" * 64
    assert "motion_target_role_id" not in repr(transport.submitted)
