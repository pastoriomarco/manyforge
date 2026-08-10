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
        scenario_id=value.scenario_id,
        scenario_catalog_revision=value.scenario_catalog_revision,
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
            []
            if support_object_id is None
            else [{"support_object_id": support_object_id}]
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
                "old-part": _fixed_scene_object(
                    "old-part", support_object_id="old-support"
                ),
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
                        "objects": [
                            deepcopy(self.objects[key]) for key in sorted(self.objects)
                        ],
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

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        request = deepcopy(dict(body))
        assert request["expected_projection_revision"] == self.revision
        self.submissions.append(request)
        operation = request["operation"]
        kind = operation["kind"]
        if kind == "detach_carried_object_at_current_pose":
            self.attachments = [
                row
                for row in self.attachments
                if row["carried_object_attachment_id"]
                != operation["carried_object_attachment_id"]
            ]
            self.objects[operation["expected_scene_resource_id"]]["placement"] = (
                _fixed_scene_object(operation["expected_scene_resource_id"])[
                    "placement"
                ]
            )
        elif kind == "remove_object":
            self.objects.pop(operation["scene_resource_id"])
        elif kind in {"add_object", "upsert_object"}:
            self.objects[operation["scene_resource_id"]] = _fixed_scene_object(
                operation["scene_resource_id"]
            )
        elif kind == "update_object_pose":
            pass
        elif kind == "attach_carried_object":
            attachment_revision = _DIGEST_F
            self.attachments.append(
                {
                    "carried_object_attachment_id": operation[
                        "carried_object_attachment_id"
                    ],
                    "carried_object_attachment_revision": attachment_revision,
                    "scene_resource_id": operation["scene_resource_id"],
                    "controllable_component_id": operation[
                        "controllable_component_id"
                    ],
                    "command_target_id": operation["command_target_id"],
                    "carrier_frame_ref": {},
                    "pose_in_carrier_frame": {},
                    "touch_body_roles": operation["touch_body_roles"],
                }
            )
            self.attachments.sort(key=lambda row: row["carried_object_attachment_id"])
            self.objects[operation["scene_resource_id"]]["placement"] = {
                "kind": "carried",
                "carried_object_attachment_id": operation[
                    "carried_object_attachment_id"
                ],
                "carried_object_attachment_revision": attachment_revision,
            }
        else:
            raise AssertionError(f"unexpected scene operation {kind}")
        self.revision["scene_revision"] = {
            "scene_id": _DIGEST_D,
            "revision_id": self.revision["scene_revision"]["revision_id"] + 1,
            "content_hash": "sha256:"
            + f"{len(self.submissions):064x}"[-64:],
        }
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


def _authored_scene() -> list[dict[str, Any]]:
    return [
        {
            "scene_resource_id": "graspable",
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


def test_scene_initialization_clears_dependencies_then_applies_authored_order() -> None:
    transport = _SceneTransport(populated=True)
    port = HeadlessSceneProgramPort(transport)

    port.initialize(_scene_context(), _authored_scene())

    assert [request["operation"]["kind"] for request in transport.submissions] == [
        "detach_carried_object_at_current_pose",
        "remove_object",
        "remove_object",
        "add_object",
        "add_object",
    ]
    assert [
        request["operation"].get("scene_resource_id")
        for request in transport.submissions[1:]
    ] == ["old-part", "old-support", "graspable", "table"]
    table = transport.submissions[-1]["operation"]
    assert table["frame_ref"] == {
        "kind": "component_frame",
        "controllable_component_id": "robot.1",
        "model_instance_id": "robot.1.model",
        "frame_role_id": "base",
    }
    assert sorted(transport.objects) == ["graspable", "table"]


def test_scene_nodes_translate_roles_and_detach_at_current_pose() -> None:
    transport = _SceneTransport()
    transport.objects["graspable"] = _fixed_scene_object("graspable")
    port = HeadlessSceneProgramPort(transport)
    port.initialize(_scene_context(), [_authored_scene()[0]])
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
        OwnerNodeInputs(
            parameters={"scene_resource_id": "graspable", "target_role": "arm"}
        ),
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


def test_scene_resource_relative_pose_is_forwarded_to_the_scene_owner() -> None:
    transport = _SceneTransport()
    port = HeadlessSceneProgramPort(transport)
    port.initialize(_scene_context(), _authored_scene())
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
    port = HeadlessSceneProgramPort(transport)
    port.initialize(_scene_context(), [])
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
