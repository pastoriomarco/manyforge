from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import re

import pytest
from pydantic import ValidationError

from manyforge_behavior.failure_details import FAILURE_DETAIL_KEYS
from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    CapabilityCommandParticipantRequest,
    ManagerApplyPreparedActivation,
    ManagerApplyRequest,
    ManagerApplyAcknowledgementContext,
    ManagerApplyAcknowledgementRequest,
    ManagerApplyStatus,
    ManagerDeploymentCandidatePreviewRequest,
    MAX_APPLY_FAILURE_MESSAGE_BYTES,
    ManualProgramHandover,
    ManualProgramHandoverContext,
    MotionParticipantRequest,
    MAX_OPERATION_PARTICIPANTS,
    OperationControlRequest,
    OperationIntentRequest,
    OperationParticipantRequest,
    OperationRecoveryActionRequest,
    ProgramDiagnosticRequest,
    ProgramSelectionStopRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    SceneTransactionRequest,
    StopIntentRequest,
)


def test_candidate_preview_requires_exact_prepared_activation_identity() -> None:
    body = {
        "deployment_artifact_id": "deployment.1",
        "deployment_revision": "sha256:" + "a" * 64,
        "candidate_retained_bundle_id": "bundle.2",
        "candidate_graph_revision": "graph.2",
        "prepared_activation_id": "sha256:" + "e" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "expected_admission_generation": 7,
        "request_owner_epoch": "manager.1",
    }
    parsed = ManagerDeploymentCandidatePreviewRequest.model_validate(body)
    assert parsed.prepared_activation_id == "sha256:" + "e" * 64
    missing = dict(body)
    del missing["prepared_activation_id"]
    with pytest.raises(ValidationError):
        ManagerDeploymentCandidatePreviewRequest.model_validate(missing)


@pytest.mark.parametrize("groups", [["arm", "grippers"], [], ["bad group"], "arm"])
def test_program_stop_group_list_is_bounded_and_explicit(groups):
    body = {
        "request_id": "stop.groups",
        "request_owner_epoch": "caller.1",
        "source_label": "operator_ui",
        "program_run_id": "run.1",
        "expected_owner_epoch": "runner.1",
        "group_ids": groups,
    }
    if groups == ["arm", "grippers"]:
        assert ProgramSelectionStopRequest.model_validate(body).group_ids == groups
    else:
        with pytest.raises(ValidationError):
            ProgramSelectionStopRequest.model_validate(body)


@pytest.mark.parametrize("value", [float("inf"), float("nan"), 2**53, object()])
def test_program_diagnostic_values_reject_nonportable_json(value):
    with pytest.raises(ValidationError):
        ProgramDiagnosticRequest.model_validate(
            {
                "schema_version": "manyforge.program_diagnostic.v3",
                "program_run_id": "run.1",
                "owner_epoch": "runner.1",
                "program_revision": "sha256:" + "a" * 64,
                "program_binding_id": "sha256:" + "b" * 64,
                "retained_bundle_id": "sha256:" + "c" * 64,
                "graph_revision": "sha256:" + "d" * 64,
                "state": "succeeded",
                "reason_code": None,
                "message": "Completed",
                "occurred_at_ns": "9007199254740993",
                "occurred_at_wall": "2026-09-08T10:00:00Z",
                "blackboard": {"value": value},
                "omitted_blackboard_count": 0,
                "delivery_loss_count": 0,
                "details": None,
                "origin": None,
            }
        )


_CONTRACT_FAILURE_DETAIL_KEYS = frozenset(
    {
        "reason",
        "reference_type",
        "reference_id",
        "target_role_id",
        "input_problem",
        "path",
        "source_kind",
        "source_reference",
        "rule_id",
        "component_id",
        "retryable",
        "required_action",
    }
)


def test_native_failure_detail_keys_match_the_runtime_constant() -> None:
    assert len(set(FAILURE_DETAIL_KEYS)) == len(FAILURE_DETAIL_KEYS)
    assert frozenset(FAILURE_DETAIL_KEYS) == _CONTRACT_FAILURE_DETAIL_KEYS
    source = (
        Path(__file__).resolve().parents[2]
        / "manyforge_headless/src/headless_api_core.cpp"
    ).read_text(encoding="utf-8")
    block = source.split("kFailureDetailKeys{", maxsplit=1)[1].split("};", maxsplit=1)[0]
    assert frozenset(re.findall(r'"([a-z_]+)"', block)) == frozenset(FAILURE_DETAIL_KEYS)
    # The Composer parser rejects any other key, which closes the control
    # socket, so its copy must not drift either.
    frontend = (
        Path(__file__).resolve().parents[2]
        / "manyforge_composer/frontend/src/api/parsers/programRuntime.ts"
    ).read_text(encoding="utf-8")
    block = frontend.split("FAILURE_DETAIL_KEYS: ReadonlySet<string> = new Set([", maxsplit=1)[
        1
    ].split("]);", maxsplit=1)[0]
    block = re.sub(r"//[^\n]*", "", block)
    assert frozenset(re.findall(r"'([a-z_]+)'", block)) == frozenset(FAILURE_DETAIL_KEYS)


def _caller() -> dict[str, object]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }


def _prepared_activation() -> dict[str, object]:
    activation = ManagerApplyPreparedActivation.model_validate(
        {
            "schema_version": "manyforge.prepared_activation_input.v6",
            "origin": {"kind": "scenario", "scenario_id": "scenario.1",
                       "source_document_digest": "sha256:" + "1" * 64,
                       "project_path": "/projects/scenario.1.scenario"},
            "captured_input_digest": "sha256:" + "d" * 64,
            "deployment_artifact_id": "deployment.1",
            "deployment_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
            "program_id": "program.1",
            "program_revision": "sha256:" + "4" * 64,
            "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
            "node_environment_digest": "sha256:" + "5" * 64,
            "assistant_mode": "optional",
            "prepared_activation_id": "sha256:" + "e" * 64,
        }
    )
    return activation.model_dump(mode="json")


def _planning_pose(z_m: float = 0.0) -> dict[str, object]:
    return {
        "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": z_m},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }


def test_no_program_activation_requires_exact_absence() -> None:
    value = {
        **_prepared_activation(),
        "schema_version": "manyforge.prepared_activation_input.v6",
        "program_id": None,
        "program_revision": None,
        "program_outcome": {"kind": "no_program"},
    }
    assert ManagerApplyPreparedActivation.model_validate(value).program_id is None
    for field, inconsistent in (
        ("schema_version", "manyforge.prepared_activation_input.v4"),
        ("program_id", "old.program"),
        ("program_revision", "sha256:" + "4" * 64),
        ("program_outcome", {"kind": "no_program", "program_binding_id": "sha256:" + "9" * 64}),
        ("typed_parameter_overrides", {"speed": 0.1}),
        ("assistant_mode", "required"),
    ):
        with pytest.raises(ValidationError):
            ManagerApplyPreparedActivation.model_validate({**value, field: inconsistent})


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
        "tcp_endpoint": {
            "kind": "entry", "entry": {"component_id": "robot", "target_id": "arm",
                                       "frame_role_id": "frame-role:tcp"}
        },
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
                    "tool.gripper.command" if tool else "execution.trajectory.execute"
                ),
                "action_span": None,
                "payload": payload,
            }
        ],
        "coordination_policy": "independent",
    }


def test_manual_context_is_optional_and_does_not_change_ordinary_wire():
    body = _planning_operation("manual.1", _tool_payload(), tool=True)
    ordinary = OperationIntentRequest.model_validate(body)
    assert ordinary.model_dump(mode="json") == body
    body["manual_handover"] = {
        "program_runner_owner_epoch": "runner.1",
        "program_run_id": "parent.1",
        "claim_generation": 1,
        "selection_revision": 0,
    }
    assert OperationIntentRequest.model_validate(body).model_dump(mode="json") == body
    body["manual_handover"]["program_run_id"] = body["program_run_id"]
    with pytest.raises(ValidationError, match="own Run"):
        OperationIntentRequest.model_validate(body)


@pytest.mark.parametrize(
    "change",
    [
        {"claim_generation": 0},
        {"selection_revision": -1},
        {"selection_revision": 2**53},
        {"selection_revision": True},
        {"program_run_id": ""},
        {"program_run_id": "x" * 129},
        {"program_runner_owner_epoch": "a\0b"},
        {"unknown": 1},
    ],
)
def test_manual_context_rejects_invalid_coordinates(change):
    with pytest.raises(ValidationError):
        ManualProgramHandoverContext.model_validate(
            {
                "program_runner_owner_epoch": "runner.1",
                "program_run_id": "parent.1",
                "claim_generation": 1,
                "selection_revision": 0,
                **change,
            }
        )


@pytest.mark.parametrize("indices", [[1, 0], [0, 0], [-1], [100_000], [True]])
def test_manual_producer_indices_are_closed_and_canonical(indices):
    with pytest.raises(ValidationError):
        ManualProgramHandover.model_validate(
            {
                "context": {
                    "program_runner_owner_epoch": "runner.1",
                    "program_run_id": "parent.1",
                    "claim_generation": 1,
                    "selection_revision": 0,
                },
                "producer_node_indices": indices,
            }
        )


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


@pytest.mark.parametrize(
    ("count", "policy", "valid"),
    [
        (0, "independent", False),
        (1, "independent", True),
        (1, "coordinated_all", False),
        (2, "independent", False),
        (2, "coordinated_all", True),
        (2, "invented", False),
        (MAX_OPERATION_PARTICIPANTS, "coordinated_all", True),
        (MAX_OPERATION_PARTICIPANTS + 1, "coordinated_all", False),
    ],
)
def test_operation_participant_bounds_and_coordination(count, policy, valid) -> None:
    operation = _planning_operation("bounded-participants", _tool_payload(), tool=True)
    operation["participants"] *= count
    operation["coordination_policy"] = policy
    if valid:
        parsed = OperationIntentRequest.model_validate(operation)
        assert len(parsed.participants) == count
        assert parsed.coordination_policy == policy
    else:
        with pytest.raises(ValidationError):
            OperationIntentRequest.model_validate(operation)


def test_coordinated_operation_checks_every_participant_action_span() -> None:
    operation = _planning_operation("spans", _tool_payload(), tool=True)
    operation["coordination_policy"] = "coordinated_all"
    operation["participants"].append(deepcopy(operation["participants"][0]))
    operation["participants"][1]["action_span"] = {
        "action_source": "system",
        "action_span_id": "span.second",
        "program_run_id": "run:other",
    }
    with pytest.raises(ValidationError, match="must match the operation"):
        OperationIntentRequest.model_validate(operation)
    operation["participants"][1]["action_span"]["program_run_id"] = "run:test"
    assert OperationIntentRequest.model_validate(operation).participants[1].action_span


def _cartesian_path() -> dict[str, object]:
    return {
        "kind": "cartesian_path",
        "waypoints": [_planning_pose(0.1), _planning_pose(0.0)],
        "max_translation_step_m": 0.01,
        "max_rotation_step_rad": 0.1,
        "max_angular_speed_rad_s": 0.2,
        "max_linear_acceleration_mps2": 0.3,
        "max_angular_acceleration_rad_s2": 0.4,
        "fallback_policy": "fail_closed",
        "blend_radii_m": [0.0, 0.0],
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
        "tool_operation": _planning_operation("pick.tool", _tool_payload(), tool=True),
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
        # Pick offsets accept the same forms as motion references, including
        # the cell frame and another robot's frame.
        {"type": "cell_frame"},
        {
            "type": "frame_role",
            "frame_role_id": "frame-role:tcp",
            "target": {
                "target_role_id": "observer_arm",
                "controllable_component_id": "observer.component",
                "command_target_id": "observer.command",
            },
        },
    ],
)
def test_pick_offset_reference_uses_closed_portable_forms(
    reference: dict[str, object],
) -> None:
    request = _pick_request()
    synthesis = request["synthesis"]
    assert isinstance(synthesis, dict)
    synthesis["pick_offset_reference"] = reference
    synthesis["approach_offset_reference"] = reference

    parsed = PickSequencePlanningRequest.model_validate(request)

    assert parsed.synthesis.pick_offset_reference is not None
    assert parsed.synthesis.pick_offset_reference.model_dump(mode="json") == (reference)
    assert parsed.synthesis.approach_offset_reference is not None
    assert parsed.synthesis.approach_offset_reference.model_dump(mode="json") == (
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
    # Planning uses the latest Scene; no revision is required.
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
        "tool_operation": _planning_operation("place.tool", _tool_payload(), tool=True),
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


@pytest.mark.parametrize("selector", [
    {"kind": "current"},
    {"kind": "default_of_role", "role": "tool"},
    {"kind": "entry", "entry": {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}},
])
def test_motion_tcp_endpoint_is_the_only_wire_form_and_round_trips(selector) -> None:
    payload = _planning_motion("tcp", {"kind": "pose_goal", "goal": _planning_pose()})
    payload["tcp_endpoint"] = selector
    operation = _planning_operation("endpoint", payload)
    parsed = OperationIntentRequest.model_validate(operation)
    wire = parsed.model_dump(mode="json")
    motion = wire["participants"][0]["payload"]
    assert "tcp_frame_role_id" not in motion
    assert motion["tcp_endpoint"] == payload["tcp_endpoint"]
    assert OperationIntentRequest.model_validate_json(
        parsed.model_dump_json()
    ).model_dump(mode="json") == wire


@pytest.mark.parametrize(
    "selectors",
    [
        {},
        {"tcp_frame_role_id": None},
        {"tcp_frame_role_id": ""},
        {"tcp_endpoint": None},
        {"tcp_endpoint": {}},
        {"tcp_endpoint": {"kind": "current", "role": "tool"}},
        {"tcp_endpoint": {"kind": "default_of_role", "role": ""}},
        {"tcp_endpoint": {"kind": "entry", "entry": {"component_id": "robot", "target_id": "arm"}}},
        {"tcp_endpoint": {"kind": "entry", "entry": {"component_id": "robot", "target_id": "arm", "frame_role_id": "tcp", "extra": 1}}},
        {"tcp_endpoint": {"target_role_id": "tool"}},
        {"tcp_endpoint": {"target_role_id": "", "frame_role_id": "tcp"}},
        {"tcp_endpoint": {"target_role_id": "tool", "frame_role_id": ""}},
        {"tcp_endpoint": {"target_role_id": 1, "frame_role_id": "tcp"}},
        {"tcp_endpoint": {
            "target_role_id": "tool", "frame_role_id": "tcp", "offset": {},
        }},
        {
            "tcp_frame_role_id": "tcp",
            "tcp_endpoint": {"target_role_id": "tool", "frame_role_id": "tcp"},
        },
        {
            "tcp_frame_role_id": None,
            "tcp_endpoint": {"target_role_id": "tool", "frame_role_id": "tcp"},
        },
        {"tcp_frame_role_id": "tcp", "tcp_endpoint": None},
    ],
)
def test_motion_tcp_endpoint_rejects_missing_null_legacy_and_open_shapes(
    selectors: dict[str, object],
) -> None:
    payload = _planning_motion("tcp", {"kind": "pose_goal", "goal": _planning_pose()})
    payload.pop("tcp_endpoint")
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate({**payload, **selectors})


def test_set_tcp_payload_round_trips_and_rejects_legacy_shape() -> None:
    selector = {"kind": "entry", "entry": {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}}
    operation = _planning_operation("set-tcp", {"kind": "set_tcp", "tcp": selector})
    parsed = OperationIntentRequest.model_validate(operation)
    assert parsed.model_dump(mode="json")["participants"][0]["payload"] == {
        "kind": "set_tcp", "tcp": selector,
    }
    operation["participants"][0]["payload"]["tcp"] = {"target_role_id": "arm", "frame_role_id": "tcp"}
    with pytest.raises(ValidationError):
        OperationIntentRequest.model_validate(operation)


@pytest.mark.parametrize("crossed_field", ["component_id", "target_id", "frame_role_id"])
def test_pick_motion_phases_compare_the_exact_qualified_tcp(crossed_field: str) -> None:
    request = _pick_request()
    for phase in ("approach_operation", "descend_operation", "retreat_operation"):
        payload = request[phase]["participants"][0]["payload"]
        payload["tcp_endpoint"] = {
            "kind": "entry", "entry": {"component_id": "tool", "target_id": "gripper", "frame_role_id": "fingertip"},
        }
    parsed = PickSequencePlanningRequest.model_validate(request)
    assert parsed.carrier_frame_role_id == "frame-role:tcp"
    wire = parsed.model_dump(mode="json")
    assert PickSequencePlanningRequest.model_validate_json(
        parsed.model_dump_json()
    ).model_dump(mode="json") == wire
    request["retreat_operation"]["participants"][0]["payload"]["tcp_endpoint"]["entry"][crossed_field] = "other"
    with pytest.raises(ValidationError, match="share one target and frame set"):
        PickSequencePlanningRequest.model_validate(request)


def test_motion_payload_accepts_explicit_joint_contact_but_not_spatial_references() -> None:
    pose = MotionParticipantRequest.model_validate(
        _planning_motion("pose", {"kind": "pose_goal", "goal": _planning_pose()})
    )
    assert pose.motion.kind == "pose_goal"
    path = MotionParticipantRequest.model_validate(
        _planning_motion("path", _cartesian_path())
    )
    assert path.motion.kind == "cartesian_path"
    joint = _planning_motion(
        "joint", {"kind": "joint_goal", "joint_names": ["joint1"], "positions_rad": [0.0]},
        contact_policy_id="withdrawal.contact", selected_object_id="probe_block",
    )
    parsed = MotionParticipantRequest.model_validate(joint)
    assert parsed.motion.kind == "joint_goal"
    assert parsed.contact_policy_id == "withdrawal.contact"
    assert parsed.runtime_object_bindings[0].runtime_object_id == "selected_object"
    assert parsed.runtime_object_bindings[0].scene_resource_id == "probe_block"
    for mode in ("absolute", "relative"):
        for field in ("target_reference", "offset_reference"):
            spatial = {**joint, "target_mode": mode, field: {"type": "cell_frame"}}
            with pytest.raises(ValidationError, match="joint_goal cannot carry spatial references"):
                MotionParticipantRequest.model_validate(spatial)
    with pytest.raises(ValidationError, match="exactly cover motion references"):
        MotionParticipantRequest.model_validate({**joint, "contact_policy_id": None})
    with pytest.raises(ValidationError, match="unique runtime_object_id"):
        MotionParticipantRequest.model_validate({
            **joint, "runtime_object_bindings": joint["runtime_object_bindings"] * 2,
        })


@pytest.mark.parametrize(
    "reference",
    [
        {"type": "frame_role", "frame_role_id": "base"},
        {
            "type": "frame_role",
            "frame_role_id": "tcp",
            "target": {
                "target_role_id": "peer",
                "controllable_component_id": "robot.2",
                "command_target_id": "arm.command",
            },
        },
        {"type": "cell_frame"},
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


@pytest.mark.parametrize(
    "frame",
    [
        {"type": "cell_frame"},
        {"type": "scene_resource", "scene_resource_id": "fixture"},
        {
            "type": "frame_role",
            "frame_role_id": "tcp",
            "target": {
                "target_role_id": "peer",
                "controllable_component_id": "robot.2",
                "command_target_id": "arm.command",
            },
        },
    ],
)
def test_absolute_goal_frame_is_one_named_frame_and_absent_means_base(frame) -> None:
    request = _planning_motion("framed", {"kind": "pose_goal", "goal": _planning_pose()})
    assert "goal_frame_reference" not in MotionParticipantRequest.model_validate(
        request
    ).model_dump(mode="json")
    request["goal_frame_reference"] = frame
    parsed = MotionParticipantRequest.model_validate(request)
    assert parsed.model_dump(mode="json")["goal_frame_reference"] == frame
    path = _planning_motion("framed-path", _cartesian_path())
    path["goal_frame_reference"] = frame
    MotionParticipantRequest.model_validate(path)
    relative = {**request, "target_mode": "relative", "target_reference": {"type": "cell_frame"}}
    with pytest.raises(ValidationError, match="goal_frame_reference requires an absolute"):
        MotionParticipantRequest.model_validate(relative)
    joint = _planning_motion(
        "joint", {"kind": "joint_goal", "joint_names": ["joint1"], "positions_rad": [0.0]}
    )
    joint["goal_frame_reference"] = frame
    with pytest.raises(ValidationError, match="goal_frame_reference requires an absolute"):
        MotionParticipantRequest.model_validate(joint)
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(
            {**request, "goal_frame_reference": {"type": "pose", "pose": _planning_pose()}}
        )


def test_motion_clearance_is_optional_and_bounded() -> None:
    request = _planning_motion("clear", {"kind": "pose_goal", "goal": _planning_pose()})
    assert "clearance_m" not in MotionParticipantRequest.model_validate(request).model_dump(
        mode="json"
    )
    for value in (0.0, 0.0005, 0.05):
        parsed = MotionParticipantRequest.model_validate({**request, "clearance_m": value})
        assert parsed.model_dump(mode="json")["clearance_m"] == value
    for value in (-0.001, 0.0501, float("nan")):
        with pytest.raises(ValidationError):
            MotionParticipantRequest.model_validate({**request, "clearance_m": value})


def test_cartesian_path_speed_mode_optional_and_blend_radii_per_waypoint() -> None:
    request = _planning_motion("path", _cartesian_path())
    wire = MotionParticipantRequest.model_validate(request).model_dump(mode="json")
    assert "speed_mode" not in wire["motion"]
    assert wire["motion"]["blend_radii_m"] == [0.0, 0.0]
    for mode in ("constant_speed", "time_optimal"):
        request["motion"]["speed_mode"] = mode
        parsed = MotionParticipantRequest.model_validate(request)
        assert parsed.model_dump(mode="json")["motion"]["speed_mode"] == mode
    request["motion"]["speed_mode"] = "bounded"
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(request)
    del request["motion"]["speed_mode"]
    request["motion"]["blend_radii_m"] = [0.0, 0.02]
    assert MotionParticipantRequest.model_validate(request).motion.blend_radii_m == [0.0, 0.02]
    for radii in ([0.0], [0.0, 0.0, 0.0]):
        request["motion"]["blend_radii_m"] = radii
        with pytest.raises(ValidationError, match="one entry per waypoint"):
            MotionParticipantRequest.model_validate(request)
    for value in (-0.001, float("nan"), float("inf")):
        request["motion"]["blend_radii_m"] = [0.0, value]
        with pytest.raises(ValidationError):
            MotionParticipantRequest.model_validate(request)
    del request["motion"]["blend_radii_m"]
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(request)


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
        len(AcknowledgeAlarmRequest.model_validate(acknowledgement).activation_id)
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
        "prepared_activation": _prepared_activation(),
        "expected_admission_generation": 7,
        "candidate_receipt": {
            "schema_version": "manyforge.manager_deployment_candidate_receipt.v2",
            "deployment_artifact_id": "deployment.1",
            "deployment_revision": "sha256:" + "a" * 64,
            "prepared_activation_id": "sha256:" + "e" * 64,
            "candidate_retained_bundle_id": "bundle.2",
            "candidate_graph_revision": "graph.2",
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "expected_admission_generation": 7,
            "manager_owner_epoch": "owner.1",
            "impact_digest": "sha256:" + "c" * 64,
            "destructive_impact": False,
            "receipt_digest": "sha256:" + "c" * 64,
        },
    }
    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(body)
    assert "reject_if_affected_busy must be true" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {**body, "reject_if_affected_busy": True, "principal_id": "forged"}
        )
    assert "Extra inputs are not permitted" in str(captured.value)


def test_direct_empty_scene_preparation_requires_publication_and_no_applied_digest() -> None:
    from manyforge_headless_api.models import ProgramScenePreparationRequest
    body = {**_caller(), "clear_empty_scene": True, "expected_applied_record_digest": None,
            "expected_admission_generation": 1, "expected_publication_generation": 1}
    assert ProgramScenePreparationRequest.model_validate(body).expected_publication_generation == 1
    with pytest.raises(ValidationError):
        ProgramScenePreparationRequest.model_validate({**body, "expected_publication_generation": None})
    with pytest.raises(ValidationError):
        ProgramScenePreparationRequest.model_validate({**body, "clear_empty_scene": False})
    with pytest.raises(ValidationError):
        ProgramScenePreparationRequest.model_validate({**body, "expected_applied_record_digest": "sha256:" + "a" * 64})


def test_apply_requires_one_exact_ua2a_prepared_activation() -> None:
    body = {
        **_caller(),
        "deployment_artifact_id": "deployment.1",
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "reject_if_affected_busy": True,
        "prepared_activation": _prepared_activation(),
        "expected_admission_generation": 7,
        "candidate_receipt": {
            "schema_version": "manyforge.manager_deployment_candidate_receipt.v2",
            "deployment_artifact_id": "deployment.1",
            "deployment_revision": "sha256:" + "a" * 64,
            "prepared_activation_id": "sha256:" + "e" * 64,
            "candidate_retained_bundle_id": "bundle.2",
            "candidate_graph_revision": "graph.2",
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "expected_admission_generation": 7,
            "manager_owner_epoch": "owner.1",
            "impact_digest": "sha256:" + "c" * 64,
            "destructive_impact": False,
            "receipt_digest": "sha256:" + "c" * 64,
        },
    }
    assert ManagerApplyRequest.model_validate(body).scene_initialization == "preserve"
    assert ManagerApplyRequest.model_validate({**body, "scene_initialization": "initialize"}).scene_initialization == "initialize"
    assert ManagerApplyRequest.model_validate({**body, "prepared_activation": _empty_cell_prepared_activation(),
                                               "scene_initialization": "initialize"}).scene_initialization == "initialize"
    with pytest.raises(ValidationError):
        ManagerApplyRequest.model_validate({**body, "scene_initialization": "inferred"})
    request = ManagerApplyRequest.model_validate(body)
    assert request.prepared_activation.program_outcome == {
        "kind": "bound",
        "program_binding_id": "sha256:" + "9" * 64,
    }

    # The accepted operation must name the input it accepted, so the exact
    # prepared activation is mandatory rather than optional.
    missing = dict(body)
    del missing["prepared_activation"]
    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(missing)
    assert "prepared_activation" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {
                **body,
                "prepared_activation": {
                    **_prepared_activation(),
                    "unexpected_member": "value",
                },
            }
        )
    assert "Extra inputs are not permitted" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {
                **body,
                "prepared_activation": {
                    **_prepared_activation(),
                    "captured_input_digest": "not-a-digest",
                },
            }
        )
    assert "captured_input_digest" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {
                **body,
                "prepared_activation": {
                    **_prepared_activation(),
                    "program_outcome": {
                        "kind": "bound",
                        "program_binding_id": "not-a-content-digest",
                    },
                },
            }
        )
    assert "program_outcome" in str(captured.value)

    # A non-executable outcome must name its unresolved references (H1a).
    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {
                **body,
                "prepared_activation": {
                    **_prepared_activation(),
                    "program_outcome": {"kind": "non_executable", "outcome": {}},
                },
            }
        )
    assert "program_outcome" in str(captured.value)

    # The prepared activation cannot name a different candidate Deployment than
    # the request it is submitted with.
    with pytest.raises(ValidationError) as captured:
        ManagerApplyRequest.model_validate(
            {
                **body,
                "prepared_activation": {
                    **_prepared_activation(),
                    "deployment_artifact_id": "deployment.2",
                },
            }
        )
    assert "prepared activation does not match" in str(captured.value)


def _status_body() -> dict[str, object]:
    prepared = _prepared_activation()
    return {
        "apply_id": "apply.3",
        "scene_initialization": "preserve",
        "apply_generation": 3,
        "request_id": "request.3",
        "request_owner_epoch": "client.epoch.3",
        "prepared_activation": {
            **{
                key: prepared[key]
                for key in (
                    "captured_input_digest",
                    "deployment_artifact_id",
                    "deployment_revision",
                    "prepared_activation_id",
                )
            },
            "program_binding_id": prepared["program_outcome"]["program_binding_id"],
        },
        "phase": "accepted",
        "last_progress": {
            "phase": "accepted",
            "record_digest": "sha256:" + "f" * 64,
        },
        "state": "pending",
        "failure_acknowledgement": "unavailable",
        "candidate_projection_revision": None,
    }


def _candidate_projection_revision() -> dict[str, object]:
    return {
        "retained_bundle_id": "sha256:" + "2" * 64,
        "graph_revision": "sha256:" + "3" * 64,
        "scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 4,
            "content_hash": "sha256:" + "2" * 64,
        },
    }


def _retained_failure(
    *, phase: str = "runtime_converged", disposition: str = "known"
) -> dict[str, object]:
    return {
        "code": "runtime_initialization_failed",
        "message": "replacement initialization failed after teardown",
        "phase": phase,
        "effect_disposition": disposition,
        "cause_code": "runtime_initialization_failed",
        "cause_details": {},
        "producer": "component_manager",
        "effect_boundary": "unknown",
    }


def test_apply_status_failure_carries_the_typed_cause() -> None:
    failure = {
        **_retained_failure(phase="accepted"),
        "code": "apply_admission_failed",
        "cause_code": "stale_generation",
        "cause_details": {"reason": "expected_generation_stale"},
        "producer": "kernel",
        "effect_boundary": "before_effect",
    }
    body = {
        **_status_body(),
        "phase": "accepted",
        "last_progress": {"phase": "accepted", "record_digest": "sha256:" + "f" * 64},
        "state": "failed",
        "failure": failure,
    }
    status = ManagerApplyStatus.model_validate(body)
    assert status.failure is not None
    assert status.failure.cause_code == "stale_generation"
    assert status.failure.cause_details == {"reason": "expected_generation_stale"}
    assert status.failure.producer == "kernel"
    assert status.failure.effect_boundary == "before_effect"
    for bad in (
        {"cause_code": ""},
        {"producer": ""},
        {"cause_details": {"": "empty key"}},
        {"cause_details": {f"key{index}": "v" for index in range(17)}},
        {"effect_boundary": "after_effect"},
    ):
        with pytest.raises(ValidationError):
            ManagerApplyStatus.model_validate({**body, "failure": {**failure, **bad}})
    without_cause = {key: value for key, value in failure.items() if key != "cause_code"}
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate({**body, "failure": without_cause})


@pytest.mark.parametrize(
    "state,disposition",
    [("failed", "known"), ("unknown_effect", "unknown")],
)
def test_apply_status_reports_one_exact_failure_disposition(
    state: str, disposition: str
) -> None:
    body = {
        **_status_body(),
        "phase": "runtime_converged",
        "last_progress": {
            "phase": "runtime_converged",
            "record_digest": "sha256:" + "f" * 64,
        },
        "state": state,
        "failure": _retained_failure(disposition=disposition),
    }
    status = ManagerApplyStatus.model_validate(body)
    assert status.state == state
    assert status.failure is not None
    assert status.failure.effect_disposition == disposition
    # A failure never published a configuration.
    assert status.candidate_projection_revision is None

    # A known failure and an unknown effect are distinct: neither may be
    # presented as the other.
    wrong = "unknown" if disposition == "known" else "known"
    with pytest.raises(ValidationError, match="disposition must match"):
        ManagerApplyStatus.model_validate(
            {**body, "failure": _retained_failure(disposition=wrong)}
        )

    # The retained failure must describe the reported phase.
    with pytest.raises(ValidationError, match="must describe the reported"):
        ManagerApplyStatus.model_validate(
            {
                **body,
                "failure": _retained_failure(
                    phase="published", disposition=disposition
                ),
            }
        )

    # A terminal failure carries no success evidence of any kind.
    with pytest.raises(ValidationError, match="only a terminal success"):
        ManagerApplyStatus.model_validate(
            {
                **body,
                "candidate_projection_revision": _candidate_projection_revision(),
            }
        )

    # Without its retained failure the state is not a terminal failure.
    without_failure = dict(body)
    del without_failure["failure"]
    with pytest.raises(ValidationError, match="only a terminal failure"):
        ManagerApplyStatus.model_validate(without_failure)

    # Empty and oversized retained failure text is rejected.
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate(
            {**body, "failure": {**_retained_failure(), "code": ""}}
        )
    with pytest.raises(ValidationError, match="exceeds"):
        ManagerApplyStatus.model_validate(
            {
                **body,
                "failure": {
                    **_retained_failure(),
                    "message": "m" * (MAX_APPLY_FAILURE_MESSAGE_BYTES + 1),
                },
            }
        )

    # A pending or successful operation never carries a failure.
    for consistent_state in ("pending", "succeeded"):
        other = {**_status_body(), "state": consistent_state,
                 "failure": _retained_failure()}
        if consistent_state == "succeeded":
            other.update(
                phase="settled",
                last_progress={
                    "phase": "settled",
                    "record_digest": "sha256:" + "f" * 64,
                },
                candidate_projection_revision=_candidate_projection_revision(),
            )
        with pytest.raises(ValidationError, match="only a terminal failure"):
            ManagerApplyStatus.model_validate(other)


def test_apply_status_reports_a_succeeded_apply_with_a_non_executable_program() -> None:
    # H1a: the runtime transition succeeded; the preserved Program no longer
    # binds. Binding validity is reported separately from the terminal state.
    body = _status_body()
    body["prepared_activation"] = {
        **body["prepared_activation"],  # type: ignore[dict-item]
        "program_binding_id": None,
    }
    status = ManagerApplyStatus.model_validate(
        {
            **body,
            "phase": "settled",
            "last_progress": {
                "phase": "settled",
                "record_digest": "sha256:" + "f" * 64,
            },
            "state": "succeeded",
            "candidate_projection_revision": _candidate_projection_revision(),
        }
    )
    assert status.state == "succeeded"
    assert status.prepared_activation.program_binding_id is None


def test_apply_status_is_one_closed_bounded_lifecycle() -> None:
    status = ManagerApplyStatus.model_validate(_status_body())
    assert status.phase == "accepted"
    assert status.last_progress.phase == status.phase
    assert status.state == "pending"

    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate(
            {**_status_body(), "unexpected_member": "value"}
        )
    assert "Extra inputs are not permitted" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate(
            {**_status_body(), "phase": "not_a_phase"}
        )
    assert "phase" in str(captured.value)

    # A status may not claim progress that its reported phase does not describe.
    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate(
            {
                **_status_body(),
                "last_progress": {
                    "phase": "published",
                    "record_digest": "sha256:" + "f" * 64,
                },
            }
        )
    assert "last progress must describe" in str(captured.value)

    # The terminal success carries its candidate configuration identity and
    # nothing else may claim one.
    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate({**_status_body(), "state": "succeeded"})
    assert "terminal success carries" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate(
            {
                **_status_body(),
                "state": "succeeded",
                "candidate_projection_revision": _candidate_projection_revision(),
            }
        )
    assert "successful operation must be settled" in str(captured.value)

    with pytest.raises(ValidationError) as captured:
        ManagerApplyStatus.model_validate(
            {
                **_status_body(),
                "candidate_projection_revision": _candidate_projection_revision(),
            }
        )
    assert "terminal success carries" in str(captured.value)

    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate(
            {**_status_body(), "apply_generation": 0}
        )


def test_apply_status_requires_strict_failure_acknowledgement_state() -> None:
    body = _status_body()
    assert ManagerApplyStatus.model_validate(body).failure_acknowledgement == "unavailable"
    for bad in (None, 0, 1, "false", "unknown"):
        with pytest.raises(ValidationError):
            ManagerApplyStatus.model_validate(
                {**body, "failure_acknowledgement": bad}
            )
    missing = dict(body)
    del missing["failure_acknowledgement"]
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate(missing)

    before_effect_failure = {
        **_retained_failure(phase="runtime_converged"),
        "effect_boundary": "before_effect",
    }
    eligible = {
        **body,
        "phase": "runtime_converged",
        "last_progress": {
            "phase": "runtime_converged",
            "record_digest": "sha256:" + "f" * 64,
        },
        "state": "failed",
        "failure": before_effect_failure,
        "failure_acknowledgement": "available",
    }
    assert ManagerApplyStatus.model_validate(eligible).failure_acknowledgement == "available"
    assert ManagerApplyStatus.model_validate({
        **eligible,
        "failure_acknowledgement": "acknowledged",
    }).failure_acknowledgement == "acknowledged"

    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate({
            **body,
            "failure_acknowledgement": "available",
        })
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate({
            **eligible,
            "failure_acknowledgement": "available",
            "failure": _retained_failure(phase="runtime_converged"),
        })
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate({
            **eligible,
            "failure_acknowledgement": "acknowledged",
            "failure": _retained_failure(phase="runtime_converged"),
        })
    with pytest.raises(ValidationError):
        ManagerApplyStatus.model_validate({
            **eligible,
            "state": "unknown_effect",
            "failure_acknowledgement": "acknowledged",
            "failure": _retained_failure(
                phase="runtime_converged", disposition="unknown"
            ),
        })


def test_apply_acknowledgement_context_is_bounded_and_allows_missing_epoch() -> None:
    request = ManagerApplyAcknowledgementRequest.model_validate({
        "request_context": {
            "request_id": "composer-ack.request.1",
            "source_label": "manyforge_composer",
        }
    })
    assert request.request_context.model_dump(mode="json", exclude_none=True) == {
        "request_id": "composer-ack.request.1",
        "source_label": "manyforge_composer",
    }
    with_epoch = ManagerApplyAcknowledgementContext.model_validate({
        "request_id": "composer-ack.request.2",
        "source_label": "manyforge_composer",
        "request_owner_epoch": "operator-command.epoch.2",
        "correlation_id": "correlation.2",
    })
    assert with_epoch.request_owner_epoch == "operator-command.epoch.2"
    for context in (
        {"request_id": "", "source_label": "manyforge_composer"},
        {"request_id": "request.1", "source_label": ""},
        {
            "request_id": "request.1",
            "source_label": "manyforge_composer",
            "request_owner_epoch": "e\u0301",
        },
        {
            "request_id": "request.1",
            "source_label": "manyforge_composer",
            "unexpected": True,
        },
    ):
        with pytest.raises(ValidationError):
            ManagerApplyAcknowledgementContext.model_validate(context)


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
        OperationControlRequest.model_validate({**control, "reason": "r" * 4097})
    with pytest.raises(ValidationError):
        OperationControlRequest.model_validate(
            {**control, "expected_operation_revision": 0}
        )

    stop_target = {
        "operation_id": "operation-1",
        "expected_operation_revision": 1,
        "expected_stop_generation": 0,
    }
    assert (
        StopIntentRequest.model_validate(
            {
                "request_context": context,
                "operations": [stop_target],
                "reason": None,
            }
        )
        .operations[0]
        .expected_stop_generation
        == 0
    )
    multi_stop = StopIntentRequest.model_validate(
        {
            "request_context": context,
            "operations": [
                stop_target,
                {**stop_target, "operation_id": "operation-2"},
            ],
            "reason": "r" * 1025,
        }
    )
    assert len(multi_stop.operations or []) == 2
    assert multi_stop.reason is None
    with pytest.raises(ValidationError):
        StopIntentRequest.model_validate(
            {
                "request_context": context,
                "operations": [stop_target],
                "resources": [
                    {
                        "resource_id": "resource-1",
                        "expected_graph_revision": "graph.1",
                        "expected_operation_resource_generation": 1,
                        "expected_stop_generation": 0,
                    }
                ],
            }
        )

    scale_control = {
        "request_context": context,
        "selector": {
            "program_binding_id": "binding-1",
            "component_role_id": "robot",
            "target_role_id": "arm",
        },
        "expected_control_generation": 0,
        "scale": 0.4,
    }
    assert RuntimeScaleControlRequest.model_validate(scale_control).scale == 0.4
    group_selector = {
        "program_binding_id": "binding-1",
        "group_targets": [
            {"component_role_id": "robot-a", "target_role_id": "arm"},
            {"component_role_id": "robot-b", "target_role_id": "arm"},
        ],
    }
    group = RuntimeScaleControlRequest.model_validate(
        {**scale_control, "selector": group_selector}
    )
    assert group.selector.model_dump(mode="json") == group_selector
    with pytest.raises(ValidationError):
        RuntimeScaleControlRequest.model_validate(
            {
                **scale_control,
                "selector": {**group_selector, "component_role_id": "robot"},
            }
        )
    with pytest.raises(ValidationError):
        RuntimeScaleControlRequest.model_validate(
            {
                **scale_control,
                "selector": {
                    **group_selector,
                    "group_targets": [
                        {"component_role_id": "robot-a", "target_role_id": "arm"},
                        {"component_role_id": "robot-a", "target_role_id": "arm"},
                    ],
                },
            }
        )
    with pytest.raises(ValidationError):
        RuntimeScaleControlRequest.model_validate(
            {**scale_control, "expected_control_generation": -1}
        )
    with pytest.raises(ValidationError):
        RuntimeScaleControlRequest.model_validate(
            {**scale_control, "selector": {"program_binding_id": "binding-1"}}
        )
    for field, value in (
        ("component_role_id", ""),
        ("target_role_id", "x" * 257),
        ("component_role_id", "e\u0301"),
    ):
        with pytest.raises(ValidationError):
            RuntimeScaleControlRequest.model_validate(
                {
                    **scale_control,
                    "selector": {
                        "program_binding_id": "binding-1",
                        "component_role_id": (
                            value if field == "component_role_id" else "robot"
                        ),
                        "target_role_id": value if field == "target_role_id" else "arm",
                    },
                }
            )
    for field, value in (
        ("component_role_id", ""),
        ("target_role_id", "x" * 257),
        ("target_role_id", "e\u0301"),
    ):
        invalid_group = deepcopy(group_selector)
        invalid_group["group_targets"][0][field] = value
        with pytest.raises(ValidationError):
            RuntimeScaleControlRequest.model_validate(
                {**scale_control, "selector": invalid_group}
            )


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
            "caller_options": [{"option_id": "command_duration_s", "value_s": 0.25}],
        },
    }
    parsed = OperationParticipantRequest.model_validate(participant)
    assert parsed.payload.command_value.position.value_m == 0.004

    # GRP-2: a bool option travels as value_bool; it is never coerced.
    flagged = deepcopy(participant)
    flagged["payload"]["caller_options"].append(
        {"option_id": "wait_for_duration", "value_bool": True}
    )
    parsed = OperationParticipantRequest.model_validate(flagged)
    assert parsed.payload.caller_options[1].value_bool is True
    flagged["payload"]["caller_options"][1]["value_bool"] = 1
    with pytest.raises(ValidationError):
        OperationParticipantRequest.model_validate(flagged)

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

    reset = _scene_transaction()
    reset["program_reset_scene_authorization"] = {
        "program_runner_owner_epoch": "program-runner.epoch.2",
        "program_run_id": "program-cycle.2",
        "cycle_claim_generation": 9,
        "terminal_or_settlement_reference": "sha256:" + "e" * 64,
    }
    parsed_reset = SceneTransactionRequest.model_validate(reset)
    assert parsed_reset.program_reset_scene_authorization is not None
    assert parsed_reset.program_reset_scene_authorization.cycle_claim_generation == 9


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


def test_attach_grasp_lost_bound_is_optional_positive_and_absent_on_the_wire() -> None:
    from manyforge_headless_api.manager import _transaction_body

    body = _scene_transaction()
    body["operation"] = {
        "kind": "attach_carried_object",
        "carried_object_attachment_id": "attachment.1",
        "scene_resource_id": "part.1",
        "controllable_component_id": "robot.1",
        "command_target_id": "arm",
        "frame_role_id": "tool0",
        "touch_body_roles": [],
        "expected_component_attachment_revisions": [],
    }
    plain = SceneTransactionRequest.model_validate(body)
    assert plain.operation.grasp_lost_bound is None
    assert "grasp_lost_bound" not in _transaction_body(plain)["operation"]

    bound = {"translation_m": 0.03, "rotation_rad": 0.26}
    body["operation"] = {**body["operation"], "grasp_lost_bound": bound}
    bounded = SceneTransactionRequest.model_validate(body)
    assert _transaction_body(bounded)["operation"]["grasp_lost_bound"] == bound

    for invalid in (
        {"translation_m": 0.0, "rotation_rad": 0.26},
        {"translation_m": 0.03, "rotation_rad": -1.0},
        {"translation_m": float("inf"), "rotation_rad": 0.26},
        {"translation_m": 0.03},
        {"translation_m": 0.03, "rotation_rad": 0.26, "extra": 1},
    ):
        body["operation"] = {**body["operation"], "grasp_lost_bound": invalid}
        with pytest.raises(ValidationError):
            SceneTransactionRequest.model_validate(body)


def test_scene_object_mass_is_optional_positive_and_absent_on_the_wire() -> None:
    from manyforge_headless_api.manager import _transaction_body

    body = _scene_transaction()
    plain = SceneTransactionRequest.model_validate(body)
    assert plain.operation.mass_kg is None
    assert "mass_kg" not in _transaction_body(plain)["operation"]

    for kind in ("add_object", "upsert_object"):
        body["operation"] = {**body["operation"], "kind": kind, "mass_kg": 2.5}
        heavy = SceneTransactionRequest.model_validate(body)
        assert _transaction_body(heavy)["operation"]["mass_kg"] == 2.5

    for invalid in (0.0, -1.0, float("inf"), float("nan"), "2.5"):
        body["operation"] = {**body["operation"], "mass_kg": invalid}
        with pytest.raises(ValidationError):
            SceneTransactionRequest.model_validate(body)


def test_attach_on_grasp_lost_is_optional_closed_and_absent_on_the_wire() -> None:
    from manyforge_headless_api.manager import _transaction_body

    body = _scene_transaction()
    body["operation"] = {
        "kind": "attach_carried_object",
        "carried_object_attachment_id": "attachment.1",
        "scene_resource_id": "part.1",
        "controllable_component_id": "robot.1",
        "command_target_id": "gripper",
        "frame_role_id": "tool0",
        "touch_body_roles": [],
        "expected_component_attachment_revisions": [],
        "grasp_lost_bound": {"translation_m": 0.03, "rotation_rad": 0.26},
    }
    plain = SceneTransactionRequest.model_validate(body)
    assert plain.operation.on_grasp_lost is None
    assert "on_grasp_lost" not in _transaction_body(plain)["operation"]

    for response in (
        {"raise_alarm": False, "stop_motion": True, "set_key": "part_lost", "detach": False},
        {"raise_alarm": False, "stop_motion": False, "set_key": None, "detach": True},
    ):
        body["operation"] = {**body["operation"], "on_grasp_lost": response}
        model = SceneTransactionRequest.model_validate(body)
        assert _transaction_body(model)["operation"]["on_grasp_lost"] == response

    for invalid in (
        {"raise_alarm": False, "stop_motion": True, "detach": False},
        {"raise_alarm": "no", "stop_motion": True, "set_key": None, "detach": False},
        {"raise_alarm": False, "stop_motion": True, "set_key": None, "detach": False, "extra": 1},
    ):
        body["operation"] = {**body["operation"], "on_grasp_lost": invalid}
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
    assert parsed_detach.operation.kind == "detach_carried_object_at_current_pose"

    malformed = deepcopy(detach)
    malformed["operation"]["pose"] = {
        "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }
    with pytest.raises(ValidationError, match="Extra inputs"):
        SceneTransactionRequest.model_validate(malformed)


def _empty_cell_prepared_activation() -> dict[str, object]:
    """Close/New: an ordinary Scenario preparation of the empty cell."""

    return {**_prepared_activation(), "program_id": None, "program_revision": None,
            "program_outcome": {"kind": "no_program"}, "assistant_mode": "optional"}


def test_prepared_origins_are_closed_and_preserve_ordinary_identity_bound() -> None:
    value = _prepared_activation()
    value["origin"]["scenario_id"] = "s" * 256
    assert ManagerApplyPreparedActivation.model_validate(value).origin.scenario_id == "s" * 256
    for bad in (
        {**value, "schema_version": "manyforge.prepared_activation_input.v5"},
        {**value, "scenario_id": "fabricated"},
        {**value, "source_document_digest": "sha256:" + "1" * 64},
        {**value, "origin": {**value["origin"], "cell_id": "cell_a"}},
        {**value, "origin": {**value["origin"], "kind": "unknown"}},
        {**value, "origin": {**value["origin"], "scenario_id": "s" * 257}},
    ):
        with pytest.raises(ValidationError):
            ManagerApplyPreparedActivation.model_validate(bad)


def test_the_empty_cell_preparation_has_a_scenario_origin() -> None:
    value = _empty_cell_prepared_activation()
    assert ManagerApplyPreparedActivation.model_validate(value).origin.kind == "scenario"


@pytest.mark.parametrize("option", ["reload_initial_scene", "reset_robot_position"])
@pytest.mark.parametrize("value", [None, 0, 1, "true", "false", [], {}])
def test_program_reset_options_are_strict_default_off(option, value):
    from manyforge_headless_api.models import ProgramCycleResetRequest
    body = {"request_id": "reset.1", "request_owner_epoch": "caller.1", "source_label": "operator_ui",
            "correlation_id": None, "program_run_id": None, "expected_owner_epoch": "runner.1",
            "expected_cycle_generation": 1}
    parsed = ProgramCycleResetRequest.model_validate(body)
    assert parsed.reload_initial_scene is False and parsed.reset_robot_position is False
    with pytest.raises(ValidationError):
        ProgramCycleResetRequest.model_validate({**body, option: value})


def test_no_run_selected_reset_requires_retained_options_and_truthful_phase():
    from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO
    cycle = {"program_runner_owner_epoch": "runner.1", "cycle_generation": 1,
             "state": "resetting", "program_run_id": None, "cycle_claim_generation": None,
             "terminal_or_settlement_reference": None, "execution_control_revision": 2,
             "causal_correlation_id": "reset.1", "causal_session_id": None, "causal_client_sequence": None,
             "current_command_kind": "program_reset", "failure": None, "reset_phase": "restoring_scene",
             "reset_options": {"reload_initial_scene": True, "reset_robot_position": False},
             "reset_robot_position": {"available": False, "reason": "unsupported"}, "allowed_commands": []}
    ProgramCycleStatusDTO.model_validate(cycle)
    failure = {"code": "scene_failed", "failed_phase": "restoring_scene"}
    recovery = {**cycle, "state": "recovery_required", "failure": failure}
    ProgramCycleStatusDTO.model_validate(recovery)
    for change in ({"reset_options": None}, {"reset_phase": "stopping"},
                   {"cycle_claim_generation": 1}, {"current_command_kind": None},
                   {"reset_options": {"reload_initial_scene": False, "reset_robot_position": False}},
                   {"reset_robot_position": {"available": True, "reason": "unsupported"}},
                   {"reset_robot_position": {"available": False, "reason": None}}):
        with pytest.raises(ValidationError):
            ProgramCycleStatusDTO.model_validate({**cycle, **change})
    with pytest.raises(ValidationError):
        ProgramCycleStatusDTO.model_validate({**recovery, "failure": {"code": "scene_failed", "failed_phase": "releasing"}})


def test_idle_reset_scene_authorization_is_a_closed_union_with_the_existing_run_shape():
    authorization = {"program_runner_owner_epoch": "runner.1", "cycle_generation": 3,
                     "reset_claim": {"request_owner_epoch": "caller.1", "request_id": "reset.1",
                                    "request_payload_digest": "sha256:" + "a" * 64, "reset_claim_generation": 7}}
    body = {**_scene_transaction(), "program_reset_scene_authorization": authorization}
    parsed = SceneTransactionRequest.model_validate(body)
    assert parsed.program_reset_scene_authorization.model_dump(mode="json") == authorization
    for malformed in ({**authorization, "program_run_id": "invented"},
                      {**authorization, "reset_claim": {**authorization["reset_claim"], "reset_claim_generation": None}},
                      {**authorization, "reset_claim": {**authorization["reset_claim"], "reload_initial_scene": True}}):
        with pytest.raises(ValidationError):
            SceneTransactionRequest.model_validate({**body, "program_reset_scene_authorization": malformed})


def _recovery_action(**changes: object) -> dict[str, object]:
    body = {
        "request_id": "recovery.1",
        "action": "retry_motion_from_current_state",
        "expected_assessment_revision": 2,
        "expected_control_command_epoch": 0,
        "expected_execution_id": 1,
    }
    return {**body, **changes}


def test_recovery_action_accepts_close_operation() -> None:
    parsed = OperationRecoveryActionRequest.model_validate(
        _recovery_action(action="close_operation")
    )
    assert parsed.action == "close_operation"
    for unknown in ("report_final_motion_failure", "Close_operation"):
        with pytest.raises(ValidationError):
            OperationRecoveryActionRequest.model_validate(_recovery_action(action=unknown))


def test_runtime_control_audit_rows_pass_through_the_cycle_frame_unchanged():
    from manyforge_headless_api.program_cycle import ProgramCycleFrameDTO

    audit = [
        {
            "event_revision": 7,
            "kind": "program_node_automatic_retry",
            "node_instance_id": "move.1",
            "attempt": 2,
            "max_attempts": 4,
            "cause": "planning_no_effect",
            "delay_s": 1.0,
        }
    ]
    frame = {
        "program_runner_owner_epoch": "runner.1",
        "cycle_generation": 1,
        "state": "running",
        "program_run_id": "run.1",
        "cycle_claim_generation": 1,
        "terminal_or_settlement_reference": None,
        "execution_control_revision": 1,
        "causal_correlation_id": None,
        "causal_session_id": None,
        "causal_client_sequence": None,
        "current_command_kind": None,
        "failure": None,
        "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
        "allowed_commands": [],
        "run_revision": 1,
        "node_status_revision": 1,
        "runtime_control_revision": 3,
        "recovery_offer": None,
        "run": {
            "owner_epoch": "runner.1",
            "capture": {"program_run_id": "run.1"},
            "run_revision": 1,
            "node_status_revision": 1,
            "state": "running",
        },
        "runtime_controls": {
            "program_run_id": "run.1",
            "owner_epoch": "runner.1",
            "run_revision": 1,
            "runtime_controls": {
                "schema_version": "manyforge.program_runtime_controls.v3",
                "program_run_id": "run.1",
                "runtime_control_revision": 3,
                "recovery_fences": {},
                "retry_counts": {"move.1": 1},
                "audit": audit,
            },
        },
    }
    parsed = ProgramCycleFrameDTO.model_validate(frame)
    assert parsed.runtime_controls["runtime_controls"]["audit"] == audit


def test_gripper_command_requires_the_caller_deadline() -> None:
    gripper = _tool_payload()
    assert CapabilityCommandParticipantRequest.model_validate(gripper).model_dump(
        mode="json"
    )["completion_timeout_s"] == gripper["completion_timeout_s"]
    gripper.pop("completion_timeout_s")
    with pytest.raises(ValidationError, match="completion_timeout_s"):
        CapabilityCommandParticipantRequest.model_validate(gripper)
