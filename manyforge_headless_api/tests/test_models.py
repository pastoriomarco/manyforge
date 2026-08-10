from __future__ import annotations

from copy import deepcopy

import pytest
from pydantic import ValidationError

from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ManagerApplyRequest,
    MotionParticipantRequest,
    OperationControlRequest,
    OperationParticipantRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    SceneTransactionRequest,
    StopIntentRequest,
)


def _caller() -> dict[str, object]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }


def _planning_pose(z_m: float = 0.0) -> dict[str, object]:
    return {
        "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": z_m},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }


def _planning_motion(
    move_id: str,
    motion: dict[str, object],
    *,
    contact_policy_id: str | None = None,
    selected_object_id: str | None = None,
) -> dict[str, object]:
    return {
        "kind": "motion",
        "move_id": move_id,
        "base_frame_role_id": "frame-role:base",
        "tcp_frame_role_id": "frame-role:tcp",
        "tolerances": {
            "joint_position_tol_rad": 0.01,
            "translation_tol_m": 0.001,
            "rotation_tol_rad": 0.01,
        },
        "speed_profile": {
            "velocity_scale": 0.5,
            "acceleration_scale": 0.5,
            "max_tcp_linear_speed_mps": None,
        },
        "timeouts": {"planning_timeout_s": 1.0, "execution_timeout_s": 2.0},
        "cache_mode": "default",
        "planning_profile": "first_feasible",
        "target_mode": "absolute",
        "target_reference": None,
        "offset_reference": None,
        "motion": motion,
        "contact_policy_id": contact_policy_id,
        "per_joint_dynamics_limits": {},
        "motion_interrupt_scopes": [],
        "runtime_object_bindings": (
            [
                {
                    "runtime_object_id": "selected_object",
                    "scene_resource_id": selected_object_id,
                }
            ]
            if selected_object_id is not None
            else []
        ),
    }


def _planning_operation(
    request_id: str, payload: dict[str, object], *, tool: bool = False
) -> dict[str, object]:
    return {
        "request_context": {**_caller(), "request_id": request_id},
        "program_id": "program:test",
        "program_revision": "program-revision:test",
        "program_binding_id": "program-binding:test",
        "program_run_id": "run:test",
        "expected_graph_revision": "graph:test",
        "expected_retained_bundle_id": "bundle:test",
        "expected_scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 1,
            "content_hash": "sha256:" + "2" * 64,
        },
        "participants": [
            {
                "component_role_id": "tool" if tool else "arm",
                "target_role_id": "gripper" if tool else "arm",
                "required_capability_id": (
                    "tool.gripper.command"
                    if tool
                    else "execution.trajectory.execute"
                ),
                "action_span": None,
                "payload": payload,
            }
        ],
        "coordination_policy": "independent",
    }


def _tool_payload() -> dict[str, object]:
    return {
        "kind": "capability_command",
        "command_schema_id": "gripper.command_position",
        "command_schema_revision": "gripper-command-revision:test",
        "command_value": {
            "kind": "gripper_position",
            "position": {"kind": "linear_m", "value_m": 0.0},
            "max_effort": None,
        },
        "completion_timeout_s": 2.0,
        "caller_options": [],
    }


def _cartesian_path() -> dict[str, object]:
    return {
        "kind": "cartesian_path",
        "waypoints": [_planning_pose(0.1), _planning_pose(0.0)],
        "max_translation_step_m": 0.01,
        "max_rotation_step_rad": 0.1,
        "speed_mode": "bounded",
        "max_linear_speed_mps": 0.1,
        "max_angular_speed_rad_s": 0.2,
        "max_linear_acceleration_mps2": 0.3,
        "max_angular_acceleration_rad_s2": 0.4,
        "fallback_policy": "fail_closed",
    }


def _pick_request() -> dict[str, object]:
    return {
        "sequence_id": "pick.1",
        "selected_object_id": "workpiece",
        "selected_runtime_object_id": "selected_object",
        "carrier_frame_role_id": "frame-role:tcp",
        "synthesis": {
            "pick_offset_reference": {
                "type": "runtime_object",
                "runtime_object_id": "selected_object",
            },
            "approach_offset_reference": {
                "type": "frame_role",
                "frame_role_id": "frame-role:tcp",
            },
        },
        "approach_operation": _planning_operation(
            "pick.approach",
            _planning_motion(
                "approach",
                {"kind": "pose_goal", "goal": _planning_pose(0.1)},
            ),
        ),
        "descend_operation": _planning_operation(
            "pick.descend",
            _planning_motion(
                "descend",
                _cartesian_path(),
                contact_policy_id="pick.contact",
                selected_object_id="workpiece",
            ),
        ),
        "tool_operation": _planning_operation(
            "pick.tool", _tool_payload(), tool=True
        ),
        "retreat_operation": _planning_operation(
            "pick.retreat", _planning_motion("retreat", _cartesian_path())
        ),
    }


@pytest.mark.parametrize(
    "reference",
    [
        {"type": "frame_role", "frame_role_id": "frame-role:wrist"},
        {
            "type": "component_body_role",
            "collision_body_role_id": "body-role:wrist",
        },
        {"type": "scene_resource", "scene_resource_id": "workpiece"},
        {"type": "runtime_object", "runtime_object_id": "selected_object"},
        {"type": "pose", "pose": _planning_pose()},
    ],
)
def test_pick_offset_reference_uses_closed_portable_forms(
    reference: dict[str, object],
) -> None:
    request = _pick_request()
    synthesis = request["synthesis"]
    assert isinstance(synthesis, dict)
    synthesis["pick_offset_reference"] = reference

    parsed = PickSequencePlanningRequest.model_validate(request)

    assert parsed.synthesis.pick_offset_reference is not None
    assert parsed.synthesis.pick_offset_reference.model_dump(mode="json") == (
        reference
    )


def test_pick_place_requests_preserve_defaults_and_phase_boundaries() -> None:
    parsed = PickSequencePlanningRequest.model_validate(_pick_request())
    assert parsed.synthesis.approach_offset_envelope.position_min_m == [
        0.0,
        0.0,
        0.05,
    ]
    assert parsed.synthesis.approach_offset_envelope.position_max_m == [
        0.0,
        0.0,
        0.1,
    ]
    assert parsed.synthesis.pick_offset_reference is not None
    assert parsed.synthesis.pick_offset_reference.type == "runtime_object"
    assert parsed.synthesis.approach_offset_reference is not None
    assert parsed.synthesis.approach_offset_reference.type == "frame_role"
    assert parsed.descend_operation.participants[0].payload.contact_policy_id == (
        "pick.contact"
    )

    invalid_contact = _pick_request()
    invalid_contact["retreat_operation"] = _planning_operation(
        "pick.retreat",
        _planning_motion(
            "retreat", _cartesian_path(), contact_policy_id="pick.contact"
        ),
    )
    with pytest.raises(ValidationError, match="retreat cannot carry"):
        PickSequencePlanningRequest.model_validate(invalid_contact)

    missing_scene = _pick_request()
    for phase in (
        "approach_operation",
        "descend_operation",
        "tool_operation",
        "retreat_operation",
    ):
        operation = missing_scene[phase]
        assert isinstance(operation, dict)
        operation["expected_scene_revision"] = None
    with pytest.raises(ValidationError, match="requires one exact scene revision"):
        PickSequencePlanningRequest.model_validate(missing_scene)

    untyped_reference = _pick_request()
    synthesis = untyped_reference["synthesis"]
    assert isinstance(synthesis, dict)
    synthesis["pick_offset_reference"] = {
        "type": "scene_resource",
        "id": "workpiece",
    }
    with pytest.raises(ValidationError):
        PickSequencePlanningRequest.model_validate(untyped_reference)

    place = {
        "sequence_id": "place.1",
        "selected_object_id": "workpiece",
        "carrier_frame_role_id": "frame-role:tcp",
        "approach_operation": _planning_operation(
            "place.approach",
            _planning_motion(
                "approach",
                {"kind": "pose_goal", "goal": _planning_pose(0.1)},
            ),
        ),
        "descend_operation": _planning_operation(
            "place.descend", _planning_motion("descend", _cartesian_path())
        ),
        "tool_operation": _planning_operation(
            "place.tool", _tool_payload(), tool=True
        ),
        "retreat_operation": _planning_operation(
            "place.retreat", _planning_motion("retreat", _cartesian_path())
        ),
    }
    assert PlaceSequencePlanningRequest.model_validate(place).sequence_id == "place.1"
    place["descend_operation"] = _planning_operation(
        "place.descend",
        _planning_motion(
            "descend", _cartesian_path(), contact_policy_id="pick.contact"
        ),
    )
    with pytest.raises(ValidationError, match="place motion phases"):
        PlaceSequencePlanningRequest.model_validate(place)


def test_motion_payload_accepts_pose_and_cartesian_but_not_joint_contact() -> None:
    pose = MotionParticipantRequest.model_validate(
        _planning_motion(
            "pose", {"kind": "pose_goal", "goal": _planning_pose()}
        )
    )
    assert pose.motion.kind == "pose_goal"
    path = MotionParticipantRequest.model_validate(
        _planning_motion("path", _cartesian_path())
    )
    assert path.motion.kind == "cartesian_path"
    with pytest.raises(ValidationError, match="joint_goal cannot carry"):
        MotionParticipantRequest.model_validate(
            _planning_motion(
                "joint",
                {
                    "kind": "joint_goal",
                    "joint_names": ["joint1"],
                    "positions_rad": [0.0],
                },
                contact_policy_id="pick.contact",
            )
        )


@pytest.mark.parametrize(
    "reference",
    [
        {"type": "frame_role", "frame_role_id": "base"},
        {
            "type": "component_body_role",
            "collision_body_role_id": "wrist_collision",
        },
        {"type": "scene_resource", "scene_resource_id": "workpiece"},
        {"type": "pose", "pose": _planning_pose(0.2)},
    ],
)
def test_relative_motion_accepts_closed_portable_references(
    reference: dict[str, object],
) -> None:
    request = _planning_motion(
        "relative-pose",
        {"kind": "pose_goal", "goal": _planning_pose(0.1)},
    )
    request["target_mode"] = "relative"
    request["target_reference"] = reference

    parsed = MotionParticipantRequest.model_validate(request)

    assert parsed.target_mode == "relative"
    assert parsed.target_reference is not None
    assert parsed.target_reference.model_dump(mode="json") == reference


def test_motion_reference_shape_fails_closed_before_native_admission() -> None:
    missing_reference = _planning_motion(
        "relative-pose",
        {"kind": "pose_goal", "goal": _planning_pose()},
    )
    missing_reference["target_mode"] = "relative"
    with pytest.raises(ValidationError, match="require target_reference"):
        MotionParticipantRequest.model_validate(missing_reference)

    absolute_reference = _planning_motion(
        "absolute-pose",
        {"kind": "pose_goal", "goal": _planning_pose()},
    )
    absolute_reference["target_reference"] = {
        "type": "frame_role",
        "frame_role_id": "base",
    }
    with pytest.raises(ValidationError, match="absolute motion"):
        MotionParticipantRequest.model_validate(absolute_reference)

    relative_joint = _planning_motion(
        "relative-joint",
        {
            "kind": "joint_goal",
            "joint_names": ["joint1"],
            "positions_rad": [0.1],
        },
    )
    relative_joint["target_mode"] = "relative"
    assert MotionParticipantRequest.model_validate(relative_joint).target_mode == (
        "relative"
    )

    stale_runtime_binding = _planning_motion(
        "relative-runtime-object",
        {"kind": "pose_goal", "goal": _planning_pose()},
    )
    stale_runtime_binding["target_mode"] = "relative"
    stale_runtime_binding["target_reference"] = {
        "type": "runtime_object",
        "runtime_object_id": "selected_object",
    }
    with pytest.raises(ValidationError, match="exactly cover"):
        MotionParticipantRequest.model_validate(stale_runtime_binding)


def test_bulk_acknowledgement_requires_exact_sorted_unique_membership() -> None:
    body = {
        **_caller(),
        "members": [
            {
                "condition_id": "condition-b",
                "activation_id": "activation-2",
                "record_revision": 2,
            },
            {
                "condition_id": "condition-a",
                "activation_id": "activation-1",
                "record_revision": 1,
            },
        ],
        "expected_generation": 3,
        "reason": None,
        "confirmed": True,
    }
    with pytest.raises(ValidationError, match="strictly sorted and unique"):
        AcknowledgeAllAlarmsRequest.model_validate(body)


def test_alarm_activation_and_bulk_cardinality_match_owner_profile() -> None:
    acknowledgement = {
        **_caller(),
        "activation_id": "a" * 2_048,
        "expected_record_revision": 1,
        "reason": None,
        "confirmed": True,
    }
    assert (
        len(
            AcknowledgeAlarmRequest.model_validate(
                acknowledgement
            ).activation_id
        )
        == 2_048
    )
    with pytest.raises(ValidationError, match="2048 UTF-8 bytes"):
        AcknowledgeAlarmRequest.model_validate(
            {**acknowledgement, "activation_id": "a" * 2_049}
        )

    members = [
        {
            "condition_id": f"condition-{index:024x}",
            "activation_id": f"activation-{index:04d}",
            "record_revision": 1,
        }
        for index in range(257)
    ]
    with pytest.raises(ValidationError):
        AcknowledgeAllAlarmsRequest.model_validate(
            {
                **_caller(),
                "members": members,
                "expected_generation": 1,
                "reason": None,
                "confirmed": True,
            }
        )


def test_apply_rejects_unknown_identity_and_noncanonical_busy_policy() -> None:
    body = {
        **_caller(),
        "deployment_artifact_id": "deployment.1",
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "reject_if_affected_busy": False,
    }
    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(body)
    assert "reject_if_affected_busy must be true" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {**body, "reject_if_affected_busy": True, "principal_id": "forged"}
        )
    assert "Extra inputs are not permitted" in str(captured.value)


def test_operation_mutations_are_strict_bounded_and_generation_fenced() -> None:
    context = _caller()
    control = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 1,
        "reason": "r" * 4096,
    }
    assert OperationControlRequest.model_validate(control).reason == "r" * 4096
    with pytest.raises(ValidationError, match="4096 UTF-8 bytes"):
        OperationControlRequest.model_validate(
            {**control, "reason": "r" * 4097}
        )
    with pytest.raises(ValidationError):
        OperationControlRequest.model_validate(
            {**control, "expected_operation_revision": 0}
        )

    stop_target = {
        "operation_id": "operation-1",
        "expected_operation_revision": 1,
        "expected_stop_generation": 0,
    }
    assert StopIntentRequest.model_validate(
        {
            "request_context": context,
            "operations": [stop_target],
            "reason": None,
        }
    ).operations[0].expected_stop_generation == 0
    with pytest.raises(ValidationError):
        StopIntentRequest.model_validate(
            {
                "request_context": context,
                "operations": [stop_target, {**stop_target, "operation_id": "operation-2"}],
                "reason": None,
            }
        )

    scale = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 1,
        "expected_runtime_scale_generation": 0,
        "scale": 0.0,
    }
    assert RuntimeScaleRequest.model_validate(scale).scale == 0.0
    with pytest.raises(ValidationError):
        RuntimeScaleRequest.model_validate({**scale, "scale": float("nan")})
    with pytest.raises(ValidationError):
        RuntimeScaleRequest.model_validate({**scale, "scale": 1.0001})


def test_gripper_command_requires_unit_tags_and_matching_nonnegative_effort() -> None:
    participant = {
        "component_role_id": "robot",
        "target_role_id": "gripper",
        "required_capability_id": "tool.gripper.command",
        "action_span": None,
        "payload": {
            "kind": "capability_command",
            "command_schema_id": "gripper.command_position",
            "command_schema_revision": "sha256:" + "a" * 64,
            "command_value": {
                "kind": "gripper_position",
                "position": {"kind": "linear_m", "value_m": 0.004},
                "max_effort": {"kind": "linear_newton", "value_n": 5.0},
            },
            "completion_timeout_s": 2.0,
            "caller_options": [
                {"option_id": "command_duration_s", "value_s": 0.25}
            ],
        },
    }
    parsed = OperationParticipantRequest.model_validate(participant)
    assert parsed.payload.command_value.position.value_m == 0.004

    mismatched = {
        **participant,
        "payload": {
            **participant["payload"],
            "command_value": {
                **participant["payload"]["command_value"],
                "max_effort": {
                    "kind": "angular_newton_meter",
                    "value_nm": 1.0,
                },
            },
        },
    }
    with pytest.raises(ValidationError, match="must match"):
        OperationParticipantRequest.model_validate(mismatched)

    bare_position = {
        **participant,
        "payload": {
            **participant["payload"],
            "command_value": {
                **participant["payload"]["command_value"],
                "position": 0.004,
            },
        },
    }
    with pytest.raises(ValidationError):
        OperationParticipantRequest.model_validate(bare_position)


def _scene_transaction() -> dict[str, object]:
    scene_id = "sha256:" + "d" * 64
    return {
        "request_context": {
            **_caller(),
            "request_owner_epoch": "scene-owner.epoch.7",
        },
        "expected_projection_revision": {
            "retained_bundle_id": "sha256:" + "b" * 64,
            "graph_revision": "sha256:" + "c" * 64,
            "scene_revision": {
                "scene_id": scene_id,
                "revision_id": 1,
                "content_hash": "sha256:" + "a" * 64,
            },
        },
        "operation": {
            "kind": "add_object",
            "scene_resource_id": "part.1",
            "shape": {
                "type": "box",
                "box_dimensions_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
            },
            "pose": {
                "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.15},
                "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
            },
            "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
        },
    }


def test_scene_transaction_is_strict_and_cannot_assert_attribution() -> None:
    parsed = SceneTransactionRequest.model_validate(_scene_transaction())
    assert parsed.operation.scene_resource_id == "part.1"
    assert (
        parsed.request_context.request_owner_epoch
        != parsed.expected_projection_revision.scene_revision.scene_id
    )

    with pytest.raises(ValidationError, match="Extra inputs are not permitted"):
        SceneTransactionRequest.model_validate(
            {**_scene_transaction(), "attribution": {"transport": "http"}}
        )

    body = _scene_transaction()
    body["operation"] = {
        "kind": "update_object_pose",
        "scene_resource_id": "part.1",
        "pose": {
            "position_m": {"x_m": 0.1, "y_m": 0.0, "z_m": 0.0},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        },
        "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
        "provider_evidence": {
            "provider_instance_id": "forged-provider",
        },
    }
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(body)

    malformed_revision = _scene_transaction()
    malformed_revision["expected_projection_revision"] = {
        **malformed_revision["expected_projection_revision"],
        "graph_revision": "graph.1",
    }
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(malformed_revision)


def test_scene_support_declarations_are_portable_and_mode_normalized() -> None:
    body = _scene_transaction()
    body["operation"] = {
        **body["operation"],
        "support_contacts": [
            {
                "support_object_id": "support-b",
                "mode": "bounded_penetration",
                "max_contact_penetration_m": 0.002,
            },
            {
                "support_object_id": "support-a",
                "release_evidence_mode": "binary_contact",
            },
        ],
    }
    parsed = SceneTransactionRequest.model_validate(body)
    contacts = parsed.operation.support_contacts
    assert [contact.support_object_id for contact in contacts] == [
        "support-a",
        "support-b",
    ]
    assert contacts[0].enforce_max_start_penetration is False
    assert contacts[0].release_tolerance_m == 0.0
    assert contacts[1].require_separation_by_end is False
    assert contacts[1].max_contact_penetration_m == 0.002

    self_supported = _scene_transaction()
    self_supported["operation"] = {
        **self_supported["operation"],
        "support_contacts": [{"support_object_id": "part.1"}],
    }
    with pytest.raises(ValidationError, match="cannot support itself"):
        SceneTransactionRequest.model_validate(self_supported)

    duplicate = _scene_transaction()
    duplicate["operation"] = {
        **duplicate["operation"],
        "support_contacts": [
            {"support_object_id": "support-a"},
            {"support_object_id": "support-a"},
        ],
    }
    with pytest.raises(ValidationError, match="duplicate support"):
        SceneTransactionRequest.model_validate(duplicate)

    invalid_mode_mix = _scene_transaction()
    invalid_mode_mix["operation"] = {
        **invalid_mode_mix["operation"],
        "support_contacts": [
            {
                "support_object_id": "support-a",
                "mode": "contact_process",
                "require_separation_by_end": True,
            }
        ],
    }
    with pytest.raises(ValidationError, match="release-only fields"):
        SceneTransactionRequest.model_validate(invalid_mode_mix)


def test_scene_policy_and_attachment_sets_require_canonical_order() -> None:
    body = _scene_transaction()
    body["operation"] = {
        "kind": "attach_carried_object",
        "carried_object_attachment_id": "attachment.1",
        "scene_resource_id": "part.1",
        "controllable_component_id": "robot.1",
        "command_target_id": "arm",
        "frame_role_id": "tool0",
        "touch_body_roles": ["finger.right", "finger.left"],
        "expected_component_attachment_revisions": [],
    }
    with pytest.raises(ValidationError, match="sorted and unique"):
        SceneTransactionRequest.model_validate(body)

    body["operation"] = {
        **body["operation"],
        "touch_body_roles": [],
        "expected_component_attachment_revisions": ["not-a-digest"],
    }
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(body)


def test_scene_upsert_and_current_pose_detach_are_closed_typed_operations() -> None:
    upsert = _scene_transaction()
    upsert["operation"] = {
        **upsert["operation"],
        "kind": "upsert_object",
        "frame_ref": {
            "kind": "scene_resource",
            "scene_resource_id": "fixture.1",
        },
        "support_contacts": [],
    }
    parsed_upsert = SceneTransactionRequest.model_validate(upsert)
    assert parsed_upsert.operation.kind == "upsert_object"
    assert parsed_upsert.operation.frame_ref.kind == "scene_resource"

    detach = _scene_transaction()
    detach["operation"] = {
        "kind": "detach_carried_object_at_current_pose",
        "carried_object_attachment_id": "attachment.1",
        "expected_carried_object_attachment_revision": "sha256:" + "d" * 64,
        "expected_scene_resource_id": "part.1",
        "expected_controllable_component_id": "robot.1",
        "expected_command_target_id": "arm",
    }
    parsed_detach = SceneTransactionRequest.model_validate(detach)
    assert (
        parsed_detach.operation.kind
        == "detach_carried_object_at_current_pose"
    )

    malformed = deepcopy(detach)
    malformed["operation"]["pose"] = {
        "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }
    with pytest.raises(ValidationError, match="Extra inputs"):
        SceneTransactionRequest.model_validate(malformed)
