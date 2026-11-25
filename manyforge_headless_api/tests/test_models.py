from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import re
import zlib

import pytest
from pydantic import ValidationError

from manyforge_behavior.failure_details import FAILURE_DETAIL_KEYS
from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    CapabilityCommandParticipantRequest,
    CollisionPolicyRequest,
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
    FullStopIntentRequest,
    StopIntentRequest,
)


_A = "3f9a0000000000a1"
# A formed key (class 3, part 0).
_KEY = "c0" + "01" * 15


def _key(sequence: int) -> str:
    return "c0" + "01" * 7 + f"{sequence:016x}"


def test_candidate_preview_requires_exact_prepared_activation_identity() -> None:
    body = {
        "deployment_revision": "sha256:" + "a" * 64,
        "candidate_retained_bundle_id": "bundle.2",
        "candidate_graph_revision": "graph.2",
        "prepared_activation_id": "sha256:" + "e" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "expected_admission_generation": 7,
        "application_start_id": _A,
    }
    parsed = ManagerDeploymentCandidatePreviewRequest.model_validate(body)
    # ``A`` is checked at the edge and never serialized downstream.
    assert "application_start_id" not in parsed.model_dump(mode="json")
    assert parsed.prepared_activation_id == "sha256:" + "e" * 64
    missing = dict(body)
    del missing["prepared_activation_id"]
    with pytest.raises(ValidationError):
        ManagerDeploymentCandidatePreviewRequest.model_validate(missing)


@pytest.mark.parametrize("groups", [["arm", "grippers"], [], ["bad group"], "arm"])
def test_program_stop_group_list_is_bounded_and_explicit(groups):
    body = {
        "request_key": _KEY,
        "source_label": "operator_ui",
        "program_run_id": 1,
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
                "schema_version": "manyforge.program_diagnostic.v5",
                "program_run_id": 1,
                "application_start_id": "3f9a0000000000a1",
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
        "reference",
        "reference_id",
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
        "request_key": _KEY,
        "source_label": "operator_ui",
    }


def _prepared_activation() -> dict[str, object]:
    activation = ManagerApplyPreparedActivation.model_validate(
        {
            "schema_version": "manyforge.prepared_activation_input.v8",
            "origin": {"kind": "scenario", "scenario_id": "scenario.1",
                       "source_document_digest": "sha256:" + "1" * 64,
                       "project_path": "/projects/scenario.1.scenario"},
            "captured_input_digest": "sha256:" + "d" * 64,
            "deployment_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
            "program_revision": "sha256:" + "4" * 64,
            "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
            "node_environment_digest": "sha256:" + "5" * 64,
            "assistant_mode": "optional",
            "prepared_activation_id": "sha256:" + "e" * 64,
            "scene_meshes": [{"byte_size": 3, "content_digest": "sha256:" + "6" * 64, "resource_uri": "meshes/part.stl"}],
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
        "schema_version": "manyforge.prepared_activation_input.v8",
        "program_revision": None,
        "program_outcome": {"kind": "no_program"},
        "scene_meshes": [],
    }
    assert ManagerApplyPreparedActivation.model_validate(value).program_revision is None
    for field, inconsistent in (
        ("schema_version", "manyforge.prepared_activation_input.v4"),
        ("program_revision", "sha256:" + "4" * 64),
        ("program_outcome", {"kind": "no_program", "program_binding_id": "sha256:" + "9" * 64}),
        ("typed_parameter_overrides", {"speed": 0.1}),
        ("assistant_mode", "required"),
        ("scene_meshes", [{"byte_size": 3, "content_digest": "sha256:" + "6" * 64, "resource_uri": "meshes/part.stl"}]),
    ):
        with pytest.raises(ValidationError):
            ManagerApplyPreparedActivation.model_validate({**value, field: inconsistent})


def test_prepared_scene_meshes_are_sorted_unique_rows() -> None:
    value = _prepared_activation()
    row = value["scene_meshes"][0]
    second = {**row, "resource_uri": "meshes/zeta.stl"}
    assert ManagerApplyPreparedActivation.model_validate(
        {**value, "scene_meshes": [row, second]}).scene_meshes[1].resource_uri == "meshes/zeta.stl"
    for bad in (
        [second, row], [row, row], [{**row, "byte_size": 0}],
        [{**row, "content_digest": "sha256:short"}], [{**row, "extra": 1}],
    ):
        with pytest.raises(ValidationError):
            ManagerApplyPreparedActivation.model_validate({**value, "scene_meshes": bad})
    missing = dict(value)
    del missing["scene_meshes"]
    with pytest.raises(ValidationError):
        ManagerApplyPreparedActivation.model_validate(missing)


def _planning_motion(
    move_id: str,
    motion: dict[str, object],
    *,
    collision_rules: list[dict[str, object]] | None = None,
    selected_object_id: str | None = None,
) -> dict[str, object]:
    return {
        "kind": "motion",
        "move_id": move_id,
        "base_frame": {"component_id": "robot", "frame_id": "base"},
        "tcp_endpoint": {
            "kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"},
            "frame": {"component_id": "tool", "frame_id": "tcp"},
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
        "collision_rules": list(collision_rules or []),
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


def _contact_rule(rule_id: str, scene_resource_id: str) -> dict[str, object]:
    return {
        "rule_id": rule_id,
        "a": [{"kind": "active_tcp_contact_bodies"}],
        "b": [{"kind": "scene_object", "scene_resource_id": scene_resource_id}],
        "contact": {"max_penetration_m": 0.002, "end_from_fraction": 0.8},
    }


_RELEASE = {
    "release_evidence_mode": "signed_penetration",
    "enforce_max_start_penetration": True,
    "max_start_penetration_m": 0.002,
    "release_tolerance_m": 1e-5,
    "min_release_progress_m": 0.0,
    "require_separation_by_end": True,
    "require_strictly_decreasing_penetration": True,
    "allow_recontact": False,
}


def _release_rule(rule_id: str, scene_resource_id: str) -> dict[str, object]:
    return {
        "rule_id": rule_id,
        "a": [{"kind": "attached_object", "component_id": "tool"}],
        "b": [{"kind": "scene_object", "scene_resource_id": scene_resource_id}],
        "release": dict(_RELEASE),
    }


def _planning_operation(
    label: str, payload: dict[str, object], *, tool: bool = False
) -> dict[str, object]:
    # Each phase is its own request: its own key, derived from its label.
    return {
        "request_context": {**_caller(), "request_key": _key(zlib.crc32(label.encode()))},
        "program_revision": "program-revision:test",
        "program_binding_id": "program-binding:test",
        "program_run_id": 1,
        "expected_graph_revision": "graph:test",
        "expected_retained_bundle_id": "bundle:test",
        "expected_scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 1,
            "content_hash": "sha256:" + "2" * 64,
        },
        "participants": [
            {
                "target": {"component_id": "tool" if tool else "robot",
                           "command_target_id": "gripper" if tool else "arm"},
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
        "program_run_id": 3,
        "claim_generation": 1,
        "hold_revision": 0,
    }
    # A manual handover has no run of its own; its parent run is the context.
    with pytest.raises(ValidationError, match="without a Run"):
        OperationIntentRequest.model_validate(body)
    body["program_run_id"] = None
    assert OperationIntentRequest.model_validate(body).model_dump(mode="json") == body


def test_manual_intent_has_no_run_and_sends_null():
    body = _planning_operation("manual.2", _tool_payload(), tool=True)
    del body["program_run_id"]
    manual = OperationIntentRequest.model_validate(body)
    assert manual.program_run_id is None
    assert manual.model_dump(mode="json") == {**body, "program_run_id": None}


@pytest.mark.parametrize(
    "change",
    [
        {"claim_generation": 0},
        {"hold_revision": -1},
        {"hold_revision": 2**53},
        {"hold_revision": True},
        # The hold revision replaced the selection revision (no alias).
        {"selection_revision": 0},
        {"program_run_id": ""},
        {"program_run_id": "3"},
        {"program_run_id": 0},
        # The runner owner epoch left the Kernel-held coordinates.
        {"program_runner_owner_epoch": "runner.1"},
        {"unknown": 1},
    ],
)
def test_manual_context_rejects_invalid_coordinates(change):
    with pytest.raises(ValidationError):
        ManualProgramHandoverContext.model_validate(
            {
                "program_run_id": 3,
                "claim_generation": 1,
                "hold_revision": 0,
                **change,
            }
        )


@pytest.mark.parametrize("indices", [[1, 0], [0, 0], [-1], [100_000], [True]])
def test_manual_producer_indices_are_closed_and_canonical(indices):
    with pytest.raises(ValidationError):
        ManualProgramHandover.model_validate(
            {
                "context": {
                    "program_run_id": 3,
                    "claim_generation": 1,
                    "hold_revision": 0,
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
        "program_run_id": 2,
    }
    with pytest.raises(ValidationError, match="must match the operation"):
        OperationIntentRequest.model_validate(operation)
    operation["participants"][1]["action_span"]["program_run_id"] = 1
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
        "carrier_frame": {"component_id": "tool", "frame_id": "tcp"},
        "synthesis": {
            "pick_offset_reference": {
                "type": "runtime_object",
                "runtime_object_id": "selected_object",
            },
            "approach_offset_reference": {
                "type": "component_frame",
                "frame": {"component_id": "tool", "frame_id": "tcp"},
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
                collision_rules=[_contact_rule("pick.contact", "workpiece")],
                selected_object_id="workpiece",
            ),
        ),
        "tool_operation": _planning_operation("pick.tool", _tool_payload(), tool=True),
        "retreat_operation": _planning_operation(
            "pick.retreat",
            _planning_motion(
                "retreat",
                _cartesian_path(),
                collision_rules=[_release_rule("lift.table", "table")],
            ),
        ),
    }


@pytest.mark.parametrize(
    "reference",
    [
        {"type": "component_frame", "frame": {"component_id": "robot", "frame_id": "wrist"}},
        {
            "type": "component_body",
            "body": {"component_id": "robot", "collision_body_id": "wrist"},
        },
        {"type": "scene_resource", "scene_resource_id": "workpiece"},
        {"type": "runtime_object", "runtime_object_id": "selected_object"},
        {"type": "pose", "pose": _planning_pose()},
        # Pick offsets accept the same forms as motion references, including
        # the cell frame and another robot's frame.
        {"type": "cell_frame"},
        {
            "type": "component_frame",
            "frame": {"component_id": "observer.component", "frame_id": "tcp"},
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
    assert parsed.synthesis.approach_offset_reference.type == "component_frame"
    descend_rules = parsed.descend_operation.participants[0].payload.collision_rules
    assert [rule.rule_id for rule in descend_rules] == ["pick.contact"]
    assert descend_rules[0].contact.end_from_fraction == 0.8
    retreat_rules = parsed.retreat_operation.participants[0].payload.collision_rules
    assert [rule.rule_id for rule in retreat_rules] == ["lift.table"]
    assert retreat_rules[0].release.require_separation_by_end is True

    # No rule is required on any leg.
    without_rules = _pick_request()
    without_rules["descend_operation"] = _planning_operation(
        "pick.descend",
        _planning_motion("descend", _cartesian_path(), selected_object_id="workpiece"),
    )
    PickSequencePlanningRequest.model_validate(without_rules)

    # The descend leg binds the selected object exactly when the request names
    # its runtime object (an offset reference then resolves through it).
    unbound = _pick_request()
    unbound["descend_operation"] = _planning_operation(
        "pick.descend", _planning_motion("descend", _cartesian_path())
    )
    with pytest.raises(ValidationError, match="pick descend binds exactly"):
        PickSequencePlanningRequest.model_validate(unbound)
    del unbound["selected_runtime_object_id"]
    unbound["synthesis"] = {}
    parsed_unbound = PickSequencePlanningRequest.model_validate(unbound)
    assert "selected_runtime_object_id" not in parsed_unbound.model_dump(mode="json")
    stale_binding = _pick_request()
    del stale_binding["selected_runtime_object_id"]
    stale_binding["synthesis"] = {}
    with pytest.raises(ValidationError, match="pick descend binds exactly"):
        PickSequencePlanningRequest.model_validate(stale_binding)

    # Legs carry no runtime-object bindings; the selected object is named by
    # the request.
    for phase, message in (
        ("approach", "approach cannot carry"),
        ("retreat", "retreat cannot carry"),
    ):
        bound = _pick_request()
        payload = _planning_motion(
            phase,
            _cartesian_path(),
            selected_object_id="workpiece",
        )
        payload["target_mode"] = "relative"
        payload["target_reference"] = {
            "type": "runtime_object", "runtime_object_id": "selected_object",
        }
        bound[f"{phase}_operation"] = _planning_operation(f"pick.{phase}", payload)
        with pytest.raises(ValidationError, match=message):
            PickSequencePlanningRequest.model_validate(bound)

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
        "carrier_frame": {"component_id": "tool", "frame_id": "tcp"},
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
            "descend",
            _cartesian_path(),
            collision_rules=[_contact_rule("place.contact", "table")],
        ),
    )
    assert PlaceSequencePlanningRequest.model_validate(place).sequence_id == "place.1"
    bound_payload = _planning_motion(
        "descend", _cartesian_path(), selected_object_id="workpiece"
    )
    bound_payload["target_mode"] = "relative"
    bound_payload["target_reference"] = {
        "type": "runtime_object", "runtime_object_id": "selected_object",
    }
    place["descend_operation"] = _planning_operation("place.descend", bound_payload)
    with pytest.raises(ValidationError, match="place motion phases"):
        PlaceSequencePlanningRequest.model_validate(place)


@pytest.mark.parametrize("selector", [
    {"kind": "current"},
    {"kind": "default_of_target", "target": {"component_id": "tool", "command_target_id": "gripper"}},
    {"kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"}, "frame": {"component_id": "tool", "frame_id": "tcp"}},
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
    selector = {"kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"}, "frame": {"component_id": "tool", "frame_id": "tcp"}}
    operation = _planning_operation("set-tcp", {"kind": "set_tcp", "tcp": selector})
    parsed = OperationIntentRequest.model_validate(operation)
    assert parsed.model_dump(mode="json")["participants"][0]["payload"] == {
        "kind": "set_tcp", "tcp": selector,
    }
    operation["participants"][0]["payload"]["tcp"] = {"target_role_id": "arm", "frame_role_id": "tcp"}
    with pytest.raises(ValidationError):
        OperationIntentRequest.model_validate(operation)


@pytest.mark.parametrize("section,crossed_field", [("target", "component_id"), ("target", "command_target_id"), ("frame", "frame_id")])
def test_pick_motion_phases_compare_the_exact_qualified_tcp(section: str, crossed_field: str) -> None:
    request = _pick_request()
    for phase in ("approach_operation", "descend_operation", "retreat_operation"):
        payload = request[phase]["participants"][0]["payload"]
        payload["tcp_endpoint"] = {
            "kind": "entry", "target": {"component_id": "robot", "command_target_id": "arm"},
            "frame": {"component_id": "tool", "frame_id": "fingertip"},
        }
    parsed = PickSequencePlanningRequest.model_validate(request)
    assert parsed.carrier_frame.frame_id == "tcp"
    wire = parsed.model_dump(mode="json")
    assert PickSequencePlanningRequest.model_validate_json(
        parsed.model_dump_json()
    ).model_dump(mode="json") == wire
    request["retreat_operation"]["participants"][0]["payload"]["tcp_endpoint"][section][crossed_field] = "other"
    with pytest.raises(ValidationError, match="share one target and frame set"):
        PickSequencePlanningRequest.model_validate(request)


def test_motion_payload_accepts_joint_rules_but_not_spatial_references() -> None:
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
        collision_rules=[{
            "rule_id": "withdrawal.contact",
            "a": [{"kind": "active_tcp_contact_bodies"}],
            "b": [{"kind": "scene_object", "scene_resource_id": "probe_block"}],
            "release": dict(_RELEASE),
        }],
    )
    parsed = MotionParticipantRequest.model_validate(joint)
    assert parsed.motion.kind == "joint_goal"
    assert parsed.collision_rules[0].rule_id == "withdrawal.contact"
    assert parsed.collision_rules[0].b[0].scene_resource_id == "probe_block"
    assert parsed.runtime_object_bindings == []
    for mode in ("absolute", "relative"):
        for field in ("target_reference", "offset_reference"):
            spatial = {**joint, "target_mode": mode, field: {"type": "cell_frame"}}
            with pytest.raises(ValidationError, match="joint_goal cannot carry spatial references"):
                MotionParticipantRequest.model_validate(spatial)
    binding = {"runtime_object_id": "selected_object", "scene_resource_id": "probe_block"}
    referencing = _planning_motion("pose", {"kind": "pose_goal", "goal": _planning_pose()})
    referencing["target_mode"] = "relative"
    referencing["target_reference"] = {
        "type": "runtime_object", "runtime_object_id": "selected_object",
    }
    with pytest.raises(ValidationError, match="must cover motion references"):
        MotionParticipantRequest.model_validate(referencing)
    MotionParticipantRequest.model_validate(
        {**referencing, "runtime_object_bindings": [binding]}
    )
    with pytest.raises(ValidationError, match="unique runtime_object_id"):
        MotionParticipantRequest.model_validate(
            {**joint, "runtime_object_bindings": [binding, binding]}
        )


@pytest.mark.parametrize(
    "reference",
    [
        {"type": "component_frame", "frame": {"component_id": "robot", "frame_id": "base"}},
        {
            "type": "component_frame",
            "frame": {"component_id": "robot.2", "frame_id": "tcp"},
        },
        {"type": "cell_frame"},
        {
            "type": "component_body",
            "body": {"component_id": "robot", "collision_body_id": "wrist_collision"},
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
            "type": "component_frame",
            "frame": {"component_id": "robot.2", "frame_id": "tcp"},
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


def _motion_with_rules(rules: list[dict[str, object]]) -> dict[str, object]:
    return _planning_motion(
        "rules", {"kind": "pose_goal", "goal": _planning_pose()}, collision_rules=rules
    )


def test_motion_collision_rules_are_required_and_closed() -> None:
    request = _motion_with_rules([])
    assert MotionParticipantRequest.model_validate(request).model_dump(mode="json")[
        "collision_rules"
    ] == []
    missing = dict(request)
    del missing["collision_rules"]
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(missing)
    for stale in ("contact_policy_id", "clearance_m"):
        with pytest.raises(ValidationError):
            MotionParticipantRequest.model_validate({**request, stale: None})


def test_motion_clearance_row_has_no_selections_and_is_bounded() -> None:
    for value in (0.0, 0.0005, 0.05):
        row = {"rule_id": "everything_else", "clearance_m": value}
        wire = MotionParticipantRequest.model_validate(
            _motion_with_rules([row])
        ).model_dump(mode="json")
        # The row without selections is sent without a and b.
        assert wire["collision_rules"] == [row]
    for value in (-0.001, 0.0501, float("nan")):
        with pytest.raises(ValidationError):
            MotionParticipantRequest.model_validate(
                _motion_with_rules([{"rule_id": "everything_else", "clearance_m": value}])
            )
    with pytest.raises(ValidationError, match="at most one row without selections"):
        MotionParticipantRequest.model_validate(_motion_with_rules([
            {"rule_id": "one", "clearance_m": 0.001},
            {"rule_id": "two", "clearance_m": 0.002},
        ]))
    with pytest.raises(ValidationError, match="carries a clearance only"):
        MotionParticipantRequest.model_validate(
            _motion_with_rules([{"rule_id": "everything_else", "contact": {}}])
        )


def test_motion_collision_rule_values_and_selections_round_trip() -> None:
    selections = [
        {"kind": "component", "component_id": "robot"},
        {"kind": "collision_body", "component_id": "tool", "collision_body_id": "fingers"},
        {"kind": "link", "component_id": "robot", "link": "link5"},
        {"kind": "attached_object", "component_id": "tool"},
        {"kind": "scene_objects"},
        {"kind": "scene_object", "scene_resource_id": "table"},
        {"kind": "active_tcp_contact_bodies"},
    ]
    rules = [
        {"rule_id": "wide", "a": selections[:3], "b": selections[4:6], "clearance_m": 0.01},
        {"rule_id": "touch", "a": [selections[6]], "b": [selections[5]], "contact": {}},
        {
            "rule_id": "press",
            "a": [selections[3]],
            "b": [selections[5]],
            "contact": {"max_penetration_m": 0.001},
        },
        {
            "rule_id": "spring",
            "a": [selections[3]],
            "b": [selections[4]],
            "contact": {"max_penetration_m": 0.01, "max_compression_step_m": 0.002},
        },
        _contact_rule("tip", "probe_block"),
        _release_rule("lift", "table"),
    ]
    parsed = MotionParticipantRequest.model_validate(_motion_with_rules(rules))
    # Absent contact keys stay absent: no depth check, the default window.
    assert parsed.model_dump(mode="json")["collision_rules"] == rules
    assert parsed.collision_rules[1].contact.max_penetration_m is None
    assert parsed.collision_rules[1].contact.end_from_fraction is None


@pytest.mark.parametrize(("rule", "message"), [
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "clearance_m": 0.001},
     "both a and b or neither"),
    ({"rule_id": "r", "a": [], "b": [{"kind": "scene_objects"}], "clearance_m": 0.001},
     "at least 1 item"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}]},
     "exactly one of clearance_m, contact, release"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "clearance_m": 0.001, "contact": {}},
     "exactly one of clearance_m, contact, release"),
    ({"rule_id": "", "clearance_m": 0.001}, "rule_id"),
    ({"rule_id": "r" * 129, "clearance_m": 0.001}, "rule_id"),
    ({"rule_id": "r", "a": [{"kind": "model_group", "component_id": "robot", "group": "arm"}],
      "b": [{"kind": "scene_objects"}], "contact": {}}, "kind"),
    ({"rule_id": "r", "a": [{"kind": "scene_object", "blackboard_key": "trial.object"}],
      "b": [{"kind": "scene_objects"}], "contact": {}}, "scene_resource_id"),
    ({"rule_id": "r", "leg": "descend", "clearance_m": 0.001}, "leg"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "contact": {"end_from_fraction": 1.5}}, "end_from_fraction"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "contact": {"max_penetration_m": -0.001}}, "max_penetration_m"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "contact": {"max_compression_step_m": 0.002}}, "needs max_penetration_m"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "contact": {"max_penetration_m": 0.01, "max_compression_step_m": 0}},
     "max_compression_step_m"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "release": {"release_evidence_mode": "signed_penetration"}}, "release"),
    ({"rule_id": "r", "a": [{"kind": "scene_objects"}], "b": [{"kind": "scene_objects"}],
      "release": {**_RELEASE, "release_evidence_mode": "binary_contact"}},
     "neutral for binary_contact"),
])
def test_motion_collision_rule_shape_is_refused(
    rule: dict[str, object], message: str
) -> None:
    with pytest.raises(ValidationError, match=message):
        MotionParticipantRequest.model_validate(_motion_with_rules([rule]))


def test_motion_collision_rule_ids_are_unique_and_bounded() -> None:
    row = _contact_rule("tip", "probe_block")
    with pytest.raises(ValidationError, match="unique rule_id"):
        MotionParticipantRequest.model_validate(_motion_with_rules([row, row]))
    many = [{**row, "rule_id": f"rule_{index}"} for index in range(65)]
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(_motion_with_rules(many))
    MotionParticipantRequest.model_validate(_motion_with_rules(many[:64]))


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


def test_linear_pose_goal_carries_its_line_settings_and_joint_stays_two_fields() -> None:
    # OD-341: a joint pose goal keeps its two-field wire; a linear one carries
    # motion_type and every line setting, speed_mode optional.
    joint = _planning_motion("pose", {"kind": "pose_goal", "goal": _planning_pose()})
    wire = MotionParticipantRequest.model_validate(joint).model_dump(mode="json")
    assert set(wire["motion"]) == {"kind", "goal"}
    line = {
        "max_translation_step_m": 0.005,
        "max_rotation_step_rad": 0.05,
        "max_angular_speed_rad_s": 0.5,
        "max_linear_acceleration_mps2": 0.5,
        "max_angular_acceleration_rad_s2": 2.5,
    }
    linear = _planning_motion(
        "line",
        {"kind": "pose_goal", "goal": _planning_pose(), "motion_type": "linear", **line},
    )
    wire = MotionParticipantRequest.model_validate(linear).model_dump(mode="json")
    assert wire["motion"] == {
        "kind": "pose_goal", "goal": wire["motion"]["goal"], "motion_type": "linear", **line,
    }
    linear["motion"]["speed_mode"] = "time_optimal"
    parsed = MotionParticipantRequest.model_validate(linear)
    assert parsed.model_dump(mode="json")["motion"]["speed_mode"] == "time_optimal"
    with pytest.raises(ValidationError, match="require motion_type linear"):
        MotionParticipantRequest.model_validate(
            {**joint, "motion": {**joint["motion"], "max_translation_step_m": 0.005}}
        )
    missing = {**linear["motion"]}
    del missing["max_rotation_step_rad"]
    with pytest.raises(ValidationError, match="requires every line setting"):
        MotionParticipantRequest.model_validate({**linear, "motion": missing})
    for value in (0.0, float("nan")):
        with pytest.raises(ValidationError):
            MotionParticipantRequest.model_validate(
                {**linear, "motion": {**linear["motion"], "max_translation_step_m": value}}
            )
    with pytest.raises(ValidationError):
        MotionParticipantRequest.model_validate(
            {**linear, "motion": {**linear["motion"], "motion_type": "joint"}}
        )


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
        "type": "component_frame",
        "frame": {"component_id": "robot", "frame_id": "base"},
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
    with pytest.raises(ValidationError, match="must cover motion references"):
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
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "reject_if_affected_busy": False,
        "prepared_activation": _prepared_activation(),
        "expected_admission_generation": 7,
        "candidate_receipt": {
            "schema_version": "manyforge.manager_deployment_candidate_receipt.v3",
            "deployment_revision": "sha256:" + "a" * 64,
            "prepared_activation_id": "sha256:" + "e" * 64,
            "candidate_retained_bundle_id": "bundle.2",
            "candidate_graph_revision": "graph.2",
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "expected_admission_generation": 7,
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


def test_apply_requires_one_exact_prepared_activation() -> None:
    body = {
        **_caller(),
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "reject_if_affected_busy": True,
        "prepared_activation": _prepared_activation(),
        "expected_admission_generation": 7,
        "candidate_receipt": {
            "schema_version": "manyforge.manager_deployment_candidate_receipt.v3",
            "deployment_revision": "sha256:" + "a" * 64,
            "prepared_activation_id": "sha256:" + "e" * 64,
            "candidate_retained_bundle_id": "bundle.2",
            "candidate_graph_revision": "graph.2",
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "expected_admission_generation": 7,
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

    # A non-executable outcome must name its unresolved references.
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
                    "deployment_revision": "sha256:" + "b" * 64,
                },
            }
        )
    assert "prepared activation does not match" in str(captured.value)


def _status_body() -> dict[str, object]:
    prepared = _prepared_activation()
    return {
        "apply_id": 3,
        "scene_initialization": "preserve",
        "apply_generation": 3,
        "request_key": _key(3),
        "prepared_activation": {
            **{
                key: prepared[key]
                for key in (
                    "captured_input_digest",
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
    # The runtime transition succeeded; the preserved Program no longer
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


def test_apply_acknowledgement_context_is_bounded_and_names_only_key_and_label() -> None:
    request = ManagerApplyAcknowledgementRequest.model_validate({
        "application_start_id": _A,
        "request_context": {
            "request_key": _KEY,
            "source_label": "manyforge_composer",
        },
    })
    assert request.model_dump(mode="json", exclude_none=True) == {
        "request_context": {
            "request_key": _KEY,
            "source_label": "manyforge_composer",
        },
    }
    for context in (
        {"request_key": _KEY, "source_label": ""},
        {"request_key": _KEY, "source_label": "manyforge\ncomposer"},
        # The owner epoch and the correlation id are deleted (BS 2.8).
        {
            "request_key": _KEY,
            "source_label": "manyforge_composer",
            "request_owner_epoch": "operator-command.epoch.2",
        },
        {
            "request_key": _KEY,
            "source_label": "manyforge_composer",
            "correlation_id": "correlation.2",
        },
        {
            "request_key": _KEY,
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
        "operation_id": 1,
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
        "operation_id": 1,
        "expected_operation_revision": 1,
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
        .expected_operation_revision
        == 1
    )
    # A Stop carries no Stop generation (stop state model §7.3): no alias.
    with pytest.raises(ValidationError):
        StopIntentRequest.model_validate(
            {
                "request_context": context,
                "operations": [{**stop_target, "expected_stop_generation": 0}],
            }
        )
    multi_stop = StopIntentRequest.model_validate(
        {
            "request_context": context,
            "operations": [
                stop_target,
                {**stop_target, "operation_id": 2},
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
                    }
                ],
            }
        )

    scale_control = {
        "request_context": context,
        "selector": {
            "program_binding_id": "binding-1",
            "component_id": "robot",
            "command_target_id": "arm",
        },
        "expected_control_generation": 0,
        "scale": 0.4,
    }
    assert RuntimeScaleControlRequest.model_validate(scale_control).scale == 0.4
    group_selector = {
        "program_binding_id": "binding-1",
        "group_targets": [
            {"component_id": "robot-a", "command_target_id": "arm"},
            {"component_id": "robot-b", "command_target_id": "arm"},
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
                "selector": {**group_selector, "component_id": "robot"},
            }
        )
    with pytest.raises(ValidationError):
        RuntimeScaleControlRequest.model_validate(
            {
                **scale_control,
                "selector": {
                    **group_selector,
                    "group_targets": [
                        {"component_id": "robot-a", "command_target_id": "arm"},
                        {"component_id": "robot-a", "command_target_id": "arm"},
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
        ("component_id", ""),
        ("command_target_id", "x" * 257),
        ("component_id", "e\u0301"),
    ):
        with pytest.raises(ValidationError):
            RuntimeScaleControlRequest.model_validate(
                {
                    **scale_control,
                    "selector": {
                        "program_binding_id": "binding-1",
                        "component_id": (
                            value if field == "component_id" else "robot"
                        ),
                        "command_target_id": value if field == "command_target_id" else "arm",
                    },
                }
            )
    for field, value in (
        ("component_id", ""),
        ("command_target_id", "x" * 257),
        ("command_target_id", "e\u0301"),
    ):
        invalid_group = deepcopy(group_selector)
        invalid_group["group_targets"][0][field] = value
        with pytest.raises(ValidationError):
            RuntimeScaleControlRequest.model_validate(
                {**scale_control, "selector": invalid_group}
            )


def test_gripper_command_requires_unit_tags_and_matching_nonnegative_effort() -> None:
    participant = {
        "target": {"component_id": "robot", "command_target_id": "gripper"},
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
    assert parsed.target.component_id == "robot"
    assert parsed.target.command_target_id == "gripper"
    assert parsed.payload.command_value.position.value_m == 0.004

    # A bool option travels as value_bool; it is never coerced.
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
        "application_start_id": _A,
        "request_context": _caller(),
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
    # The context names the request only; ``A`` is checked at the edge and
    # never serialized downstream.
    dumped = parsed.model_dump(mode="json")
    assert dumped["request_context"] == {"request_key": _KEY, "source_label": "operator_ui"}
    assert "application_start_id" not in dumped
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(
            {
                **_scene_transaction(),
                "request_context": {**_caller(), "request_owner_epoch": "scene-owner.epoch.7"},
            }
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
        "program_run_id": 2,
        "cycle_claim_generation": 9,
        "terminal_or_settlement_reference": "sha256:" + "e" * 64,
        "program_cycle_status": {
            "cycle_generation": 4, "state": "resetting", "program_run_id": 2,
            "cycle_claim_generation": 9, "terminal_or_settlement_reference": "sha256:" + "e" * 64,
            "accepted_reset": {"request_key": _KEY, "request_payload_digest": "sha256:" + "a" * 64,
                               "reload_initial_scene": True, "reset_robot_position": False,
                               "reset_claim_generation": None}},
    }
    parsed_reset = SceneTransactionRequest.model_validate(reset)
    assert parsed_reset.program_reset_scene_authorization is not None
    assert parsed_reset.program_reset_scene_authorization.cycle_claim_generation == 9
    # OD-139: the runner's current cycle status is required.
    del reset["program_reset_scene_authorization"]["program_cycle_status"]
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(reset)


def test_scene_objects_carry_no_support_contacts() -> None:
    body = _scene_transaction()
    body["operation"] = {
        **body["operation"],
        "support_contacts": [{"support_object_id": "support-a"}],
    }
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(body)


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
    assert "grasp_lost_bound" not in _transaction_body(plain, None)["operation"]

    bound = {"translation_m": 0.03, "rotation_rad": 0.26}
    body["operation"] = {**body["operation"], "grasp_lost_bound": bound}
    bounded = SceneTransactionRequest.model_validate(body)
    assert _transaction_body(bounded, None)["operation"]["grasp_lost_bound"] == bound

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
    assert "mass_kg" not in _transaction_body(plain, None)["operation"]

    for kind in ("add_object", "upsert_object"):
        body["operation"] = {**body["operation"], "kind": kind, "mass_kg": 2.5}
        heavy = SceneTransactionRequest.model_validate(body)
        assert _transaction_body(heavy, None)["operation"]["mass_kg"] == 2.5

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
    assert "on_grasp_lost" not in _transaction_body(plain, None)["operation"]

    for response in (
        {"raise_alarm": False, "stop_motion": True, "set_key": "part_lost", "detach": False},
        {"raise_alarm": False, "stop_motion": False, "set_key": None, "detach": True},
    ):
        body["operation"] = {**body["operation"], "on_grasp_lost": response}
        model = SceneTransactionRequest.model_validate(body)
        assert _transaction_body(model, None)["operation"]["on_grasp_lost"] == response

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

    return {**_prepared_activation(), "program_revision": None,
            "program_outcome": {"kind": "no_program"}, "assistant_mode": "optional",
            "scene_meshes": []}


def test_prepared_origins_are_closed_and_preserve_ordinary_identity_bound() -> None:
    value = _prepared_activation()
    value["origin"]["scenario_id"] = "s" * 256
    assert ManagerApplyPreparedActivation.model_validate(value).origin.scenario_id == "s" * 256
    for bad in (
        {**value, "schema_version": "manyforge.prepared_activation_input.v5"},
        {**value, "schema_version": "manyforge.prepared_activation_input.v7"},
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


@pytest.mark.parametrize("option", ["reload_initial_scene", "reset_robot_position", "reset_stored_values"])
@pytest.mark.parametrize("value", [None, 0, 1, "true", "false", [], {}])
def test_program_reset_options_are_strict_default_off(option, value):
    from manyforge_headless_api.models import ProgramCycleResetRequest
    body = {"request_key": _KEY, "source_label": "operator_ui",
            "program_run_id": None, "expected_cycle_generation": 1}
    parsed = ProgramCycleResetRequest.model_validate(body)
    assert parsed.reload_initial_scene is False and parsed.reset_robot_position is False
    assert parsed.reset_stored_values is False
    with pytest.raises(ValidationError):
        ProgramCycleResetRequest.model_validate({**body, option: value})


def test_no_run_selected_reset_requires_retained_options_and_truthful_phase():
    from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO
    cycle = {"cycle_generation": 1,
             "state": "resetting", "program_run_id": None, "cycle_claim_generation": None,
             "terminal_or_settlement_reference": None, "execution_control_revision": 2,
             "causal_request_key": _KEY,
             "current_command_kind": "program_reset",
             "runner_commands": {"in_progress": {
                 "kind": "reset", "request_key": _KEY, "step": "restoring_scene",
                 "accepted_at_ns": "1", "end": None}, "ended": []},
             "failure": None, "reset_phase": "restoring_scene",
             "reset_options": {"reload_initial_scene": True, "reset_robot_position": False,
                               "reset_stored_values": False},
             "reset_robot_position": {"available": False, "reason": "unsupported"}, "allowed_commands": []}
    ProgramCycleStatusDTO.model_validate(cycle)
    failure = {"code": "scene_failed", "failed_phase": "restoring_scene"}
    recovery = {**cycle, "state": "recovery_required", "failure": failure}
    ProgramCycleStatusDTO.model_validate(recovery)
    for change in ({"reset_options": None}, {"reset_phase": "stopping"},
                   {"cycle_claim_generation": 1}, {"current_command_kind": None},
                   {"reset_options": {"reload_initial_scene": True, "reset_robot_position": False}},
                   # Stored values alone never enter a no-run Reset phase.
                   {"reset_options": {"reload_initial_scene": False, "reset_robot_position": False,
                                      "reset_stored_values": True}},
                   {"reset_robot_position": {"available": True, "reason": "unsupported"}},
                   {"reset_robot_position": {"available": False, "reason": None}}):
        with pytest.raises(ValidationError):
            ProgramCycleStatusDTO.model_validate({**cycle, **change})
    with pytest.raises(ValidationError):
        ProgramCycleStatusDTO.model_validate({**recovery, "failure": {"code": "scene_failed", "failed_phase": "releasing"}})


def test_idle_reset_scene_authorization_is_a_closed_union_with_the_existing_run_shape():
    authorization = {"cycle_generation": 3,
                     "reset_claim": {"request_key": _KEY,
                                    "request_payload_digest": "sha256:" + "a" * 64, "reset_claim_generation": 7},
                     "program_cycle_status": {
                         "cycle_generation": 3, "state": "resetting", "program_run_id": None,
                         "cycle_claim_generation": None, "terminal_or_settlement_reference": None,
                         "accepted_reset": {"request_key": _KEY, "request_payload_digest": "sha256:" + "a" * 64,
                                            "reload_initial_scene": True, "reset_robot_position": False,
                                            "reset_claim_generation": 7}}}
    body = {**_scene_transaction(), "program_reset_scene_authorization": authorization}
    parsed = SceneTransactionRequest.model_validate(body)
    assert parsed.program_reset_scene_authorization.model_dump(mode="json") == authorization
    for malformed in ({**authorization, "program_run_id": 1},
                      {key: value for key, value in authorization.items() if key != "program_cycle_status"},
                      {**authorization, "program_cycle_status": {
                          **authorization["program_cycle_status"], "reset_phase": "restoring_scene"}},
                      {**authorization, "reset_claim": {**authorization["reset_claim"], "request_key": "reset.1"}},
                      {**authorization, "reset_claim": {**authorization["reset_claim"], "reset_claim_generation": None}},
                      {**authorization, "reset_claim": {**authorization["reset_claim"], "reload_initial_scene": True}}):
        with pytest.raises(ValidationError):
            SceneTransactionRequest.model_validate({**body, "program_reset_scene_authorization": malformed})


def _recovery_action(**changes: object) -> dict[str, object]:
    body = {
        "request_key": _KEY,
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
        "cycle_generation": 1,
        "state": "running",
        "program_run_id": 1,
        "cycle_claim_generation": 1,
        "terminal_or_settlement_reference": None,
        "execution_control_revision": 1,
        "causal_request_key": None,
        "current_command_kind": None,
        "runner_commands": {"in_progress": None, "ended": []},
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
            "capture": {"program_run_id": 1},
            "run_revision": 1,
            "node_status_revision": 1,
            "state": "running",
        },
        "runtime_controls": {
            "program_run_id": 1,
            "run_revision": 1,
            "runtime_controls": {
                "schema_version": "manyforge.program_runtime_controls.v4",
                "program_run_id": 1,
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


def test_run_failure_details_accept_qualified_signal_without_truncation() -> None:
    from manyforge_headless_api.models import validate_failure_details_and_origin

    origin = {"kind": "program_node", "node_instance_id": "node_1", "owner_api": None}
    details = {"reference_type": "signal", "reference": {
        "component_id": "robot", "signal_name": "é" * 256,
    }}
    validate_failure_details_and_origin(details, origin)
    assert details["reference"]["signal_name"] == "é" * 256
    details["reference"]["signal_name"] += "x"
    with pytest.raises(ValueError):
        validate_failure_details_and_origin(details, origin)


def test_a_stop_body_is_never_invalid_because_of_its_key() -> None:
    """The Stop models carry the client's key unchecked: Headless forms it,
    and a Stop without a valid key acts with a Headless key (1.8, Q8)."""
    context = _caller()
    target = {
        "operation_id": 1,
        "expected_operation_revision": 1,
    }
    for request_key in (_KEY, _KEY.upper(), "", "not-a-key", None):
        stop_context = {**context, "request_key": request_key}
        StopIntentRequest.model_validate(
            {"request_context": stop_context, "operations": [target]}
        )
        FullStopIntentRequest.model_validate({"request_context": stop_context})
    for removed in ("request_id", "request_owner_epoch", "correlation_id"):
        stop_context = {**context, removed: "x"}
        with pytest.raises(ValidationError):
            StopIntentRequest.model_validate(
                {"request_context": stop_context, "operations": [target]}
            )


def _cycle_status(**changes: object) -> dict[str, object]:
    return {
        "cycle_generation": 1,
        "state": "starting",
        "program_run_id": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
        "execution_control_revision": 1,
        "causal_request_key": _KEY,
        "current_command_kind": "program_run",
        "runner_commands": {"in_progress": _row("start"), "ended": []},
        "failure": None,
        "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
        "allowed_commands": [],
        **changes,
    }


def _row(kind: str, **changes: object) -> dict[str, object]:
    return {"kind": kind, "request_key": _KEY, "step": None,
            "accepted_at_ns": None, "end": None, **changes}


def _end(outcome: str, code: str | None = None, **changes: object) -> dict[str, object]:
    return {"outcome": outcome, "code": code, "details": None,
            "finished_at_ns": "2", **changes}


def test_the_runner_commands_are_closed_rows() -> None:
    """OD-144/OD-214: the command holding the runner and the last 8 ended."""
    from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO

    def status(in_progress=None, ended=()):
        return _cycle_status(runner_commands={"in_progress": in_progress,
                                              "ended": list(ended)})

    for kind in ("start", "reset", "scene_preparation", "resume_motion",
                 "persistent_values_clear", "recovery_action"):
        ProgramCycleStatusDTO.model_validate(status(_row(kind)))
    ProgramCycleStatusDTO.model_validate(status(
        _row("start", step="claiming", accepted_at_ns="1"),
        [_row("reset", step="releasing", accepted_at_ns="1", end=_end("succeeded")),
         _row("start", end=_end("refused", "program_cycle_reset_required",
                                details={"x": 1})),
         _row("resume_motion", step="applying", accepted_at_ns="1",
              end=_end("unknown", "program_cycle_reconciliation_required"))]))
    ProgramCycleStatusDTO.model_validate(status(None, [
        _row("start", step="admitting", accepted_at_ns="1", end=_end("refused", "x"))] * 8))
    invalid = [
        status(_row("program_run")),
        status(_row("stop_all")),
        status(_row("start", request_key="KEY")),
        status(_row("start", step="flying", accepted_at_ns="1")),
        # Times are decimal text (wall-clock ns exceed a JSON-safe integer).
        status(_row("start", step="admitting", accepted_at_ns=1)),
        status(_row("start", step="admitting", accepted_at_ns="01")),
        # A step before acceptance; an end in progress; no end when ended.
        status(_row("start", step="claiming")),
        status(_row("start", end=_end("refused", "x"))),
        status(None, [_row("start")]),
        # Before acceptance only refused or unknown; a code exactly unless succeeded.
        status(None, [_row("start", end=_end("succeeded"))]),
        status(None, [_row("start", accepted_at_ns="1", end=_end("succeeded", "x"))]),
        status(None, [_row("start", accepted_at_ns="1", end=_end("failed"))]),
        status(None, [_row("start", accepted_at_ns="1", end=_end("cancelled", "x"))]),
        status(None, [_row("start", accepted_at_ns="1", end=_end("refused", "x"))] * 9),
    ]
    for value in invalid:
        with pytest.raises(ValidationError):
            ProgramCycleStatusDTO.model_validate(value)
    without = _cycle_status()
    del without["runner_commands"]
    with pytest.raises(ValidationError):
        ProgramCycleStatusDTO.model_validate(without)
    for removed in ({"command_in_progress": None}, {"command_in_progress": "start"}):
        with pytest.raises(ValidationError):
            ProgramCycleStatusDTO.model_validate({**_cycle_status(), **removed})


def test_the_command_reply_is_one_versioned_row() -> None:
    from manyforge_headless_api.program_cycle import (
        PROGRAM_COMMAND_REPLY_SCHEMA,
        ProgramCommandReplyDTO,
    )

    assert PROGRAM_COMMAND_REPLY_SCHEMA == "manyforge.program_command_reply.v1"
    ProgramCommandReplyDTO.model_validate({
        "schema_version": PROGRAM_COMMAND_REPLY_SCHEMA,
        "command": _row("start", step="admitting", accepted_at_ns="1")})
    with pytest.raises(ValidationError):
        ProgramCommandReplyDTO.model_validate({
            "schema_version": "manyforge.program_runtime_control_result.v2",
            "command": _row("start", step="admitting", accepted_at_ns="1")})


def test_a_start_window_cycle_has_no_run_and_no_claim_yet() -> None:
    """K-6: a run number is announced only after its Kernel claim (1.15.5)."""
    from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO

    ProgramCycleStatusDTO.model_validate(_cycle_status())
    ProgramCycleStatusDTO.model_validate(
        _cycle_status(program_run_id=3, cycle_claim_generation=2)
    )
    for change in (
        {"cycle_claim_generation": 2},
        {"state": "running"},
        {"causal_request_key": "reset.1"},
        {"causal_request_key": _KEY.upper()},
        {"causal_correlation_id": None},
    ):
        with pytest.raises(ValidationError):
            ProgramCycleStatusDTO.model_validate(_cycle_status(**change))


def test_runner_requests_forward_the_formed_key_and_never_a():
    """1.15.1: the runner receives the formed key; ``A`` stays at Headless."""
    from manyforge_headless_api.models import (
        CycleRecoveryActionRequest,
        ProgramCycleResetRequest,
        ProgramHaltResumeRequest,
        ProgramRunStartRequest,
        ProgramSelectionResumeRequest,
    )

    digest = "sha256:" + "0" * 64
    bodies = (
        (ProgramRunStartRequest, {"scenario_id": "lite6", "source_document_digest": digest,
                                  "expected_applied_record_digest": digest,
                                  "draft_program_revision": digest}),
        (ProgramCycleResetRequest, {"program_run_id": 3, "expected_cycle_generation": 1}),
        (ProgramHaltResumeRequest, {"program_run_id": 3, "expected_run_revision": 1}),
        (ProgramSelectionResumeRequest, {"program_run_id": 3, "expected_hold_revision": 0}),
        (CycleRecoveryActionRequest, {"offer_id": digest, "offer_revision": 1,
                                      "action_id": "reset_cycle"}),
    )
    for model, fields in bodies:
        parsed = model.model_validate({**_caller(), "application_start_id": _A, **fields})
        dumped = parsed.model_dump(mode="json")
        assert dumped["request_key"] == _KEY
        assert "application_start_id" not in dumped
        for removed in ("expected_owner_epoch", "control_session_number", "client_sequence"):
            assert removed not in dumped
            with pytest.raises(ValidationError):
                model.model_validate({**_caller(), **fields, removed: 1})
        with pytest.raises(ValidationError):
            model.model_validate({**_caller(), **fields, "program_run_id": "run.1"})


def test_recovery_action_names_an_offered_action_or_an_operator_choice_option():
    """OD-342: the action is an identifier the runner checks against the
    offer: a recovery action or the option id answering an operator question."""
    from manyforge_headless_api.models import CycleRecoveryActionRequest

    body = {
        **_caller(),
        "application_start_id": _A,
        "offer_id": "sha256:" + "0" * 64,
        "offer_revision": 1,
    }
    for action_id in (
        "retry_runtime_step",
        "reset_cycle",
        "continue_with_failure",
        "continue",
        "bin.emptied-2",
        "a" * 128,
    ):
        parsed = CycleRecoveryActionRequest.model_validate({**body, "action_id": action_id})
        assert parsed.action_id == action_id
    for action_id in (
        "",
        "Continue",
        "2nd_try",
        "_continue",
        "bin emptied",
        "a" * 129,
        "\u00e9",
        7,
        None,
    ):
        with pytest.raises(ValidationError):
            CycleRecoveryActionRequest.model_validate({**body, "action_id": action_id})
    with pytest.raises(ValidationError):
        CycleRecoveryActionRequest.model_validate(body)


def test_history_filters_by_rule_id():
    from manyforge_headless_api.history_query import HistorianQueryRequest

    query = HistorianQueryRequest(rule_id="tip_on_part")
    assert query.native_arguments(now_utc_ns=1)["correlations"] == {
        "rule_id": "tip_on_part"}
    # Earlier filter names are refused like any unknown filter.
    for stale in ({"collision_rule_id": "tip_on_part"}, {"contact_policy_id": "probe_contact_policy"}):
        with pytest.raises(ValidationError):
            HistorianQueryRequest(**stale)


def test_history_filters_accept_the_display_and_copy_forms():
    from manyforge_headless_api.history_query import HistorianQueryRequest

    for form in ("7", "3f9a/op7", _A + "/op7"):
        query = HistorianQueryRequest(operation_id=form)
        assert query.native_arguments(now_utc_ns=1)["correlations"] == {"operation_id": "7"}
    copy = HistorianQueryRequest(operation_id=_A + "/op7", stop_event_id=_A[:4] + "/stop2")
    assert copy.start_identities() == ({_A}, {_A[:4]})
    # The named start is the store's filter; none means the current start.
    assert copy.native_arguments(now_utc_ns=1)["application_start_id"] == _A
    assert HistorianQueryRequest(operation_id="7").native_arguments(now_utc_ns=1)["application_start_id"] is None
    whole = HistorianQueryRequest(application_start_id=_A).native_arguments(now_utc_ns=1)
    assert (whole["application_start_id"], whole["correlations"]) == (_A, {})
    for invalid in ("op.7", "3f9a/stop7", "07"):
        with pytest.raises(ValidationError):
            HistorianQueryRequest(operation_id=invalid)
    for incoherent in (
        {"application_start_id": "3F9A"},
        {"application_start_id": "0" * 16},
        {"application_start_id": _A, "operation_id": "1" * 16 + "/op7"},
        {"application_start_id": _A, "operation_id": "ffff/op7"},
        {"source_owner_epoch": _A},
    ):
        with pytest.raises(ValidationError):
            HistorianQueryRequest(**incoherent)


# Bulk change 1, third pass: documents the runner and behaviour layer produce
# and the owner-epoch fields deleted with their compares (contract 1.16.4).


def _diagnostic(**changes: object) -> dict[str, object]:
    return {
        "schema_version": "manyforge.program_diagnostic.v5",
        "program_run_id": 1,
        "application_start_id": _A,
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "retained_bundle_id": "sha256:" + "c" * 64,
        "graph_revision": "sha256:" + "d" * 64,
        "state": "succeeded",
        "reason_code": None,
        "message": "Completed",
        "occurred_at_ns": "1",
        "occurred_at_wall": "2026-09-08T10:00:00Z",
        "blackboard": {},
        "omitted_blackboard_count": 0,
        "delivery_loss_count": 0,
        "details": None,
        "origin": None,
        **changes,
    }


def test_program_diagnostic_v5_keeps_its_start_identity_in_the_record() -> None:
    parsed = ProgramDiagnosticRequest.model_validate(_diagnostic())
    assert parsed.model_dump(mode="json")["application_start_id"] == _A
    for invalid in (
        _diagnostic(schema_version="manyforge.program_diagnostic.v4"),
        _diagnostic(owner_epoch="runner.1"),
    ):
        with pytest.raises(ValidationError):
            ProgramDiagnosticRequest.model_validate(invalid)


def test_cycle_status_and_its_frame_name_no_runner_owner_epoch() -> None:
    from manyforge_headless_api.program_cycle import ProgramCycleStatusDTO

    ProgramCycleStatusDTO.model_validate(_cycle_status())
    with pytest.raises(ValidationError):
        ProgramCycleStatusDTO.model_validate(
            _cycle_status(program_runner_owner_epoch=_A)
        )


def test_node_admission_evidence_and_reset_scene_authorization_carry_no_epoch() -> None:
    from manyforge_headless_api.models import (
        ProgramNodeAdmissionEvidenceRequest,
        ProgramResetSceneAuthorizationRequest,
    )

    evidence = {
        "program_run_id": 1,
        "parent_program_cycle_claim_generation": 2,
        "node_instance_id": "place",
        "node_attempt_id": 3,
        "owner_api": "scene.transaction",
    }
    ProgramNodeAdmissionEvidenceRequest.model_validate(evidence)
    authorization = {
        "program_run_id": 1,
        "cycle_claim_generation": 2,
        "terminal_or_settlement_reference": "sha256:" + "e" * 64,
        "program_cycle_status": {
            "cycle_generation": 4, "state": "resetting", "program_run_id": 1,
            "cycle_claim_generation": 2, "terminal_or_settlement_reference": "sha256:" + "e" * 64,
            "accepted_reset": {"request_key": _KEY, "request_payload_digest": "sha256:" + "a" * 64,
                               "reload_initial_scene": True, "reset_robot_position": False,
                               "reset_claim_generation": None}},
    }
    ProgramResetSceneAuthorizationRequest.model_validate(authorization)
    for model, body in (
        (ProgramNodeAdmissionEvidenceRequest, evidence),
        (ProgramResetSceneAuthorizationRequest, authorization),
    ):
        with pytest.raises(ValidationError):
            model.model_validate({**body, "program_runner_owner_epoch": _A})


def test_io_signal_read_is_the_v3_query_without_an_owner_epoch() -> None:
    from manyforge_headless_api.models import IoSignalReadRequest

    read = {
        "schema_version": "manyforge.target_observation_query.v3",
        "program_binding_id": "sha256:" + "a" * 64,
        "kind": "io_signal",
        "signal": {"component_id": "station", "signal_name": "part_present"},
    }
    assert IoSignalReadRequest.model_validate(read).target_query().model_dump(
        mode="json", exclude_none=True
    ) == read
    with pytest.raises(ValidationError):
        IoSignalReadRequest.model_validate({**read, "owner_epoch": "owner.1"})


def test_stop_all_and_halt_body_is_the_request_context_alone() -> None:
    """Stop all and Halt name no run and carry no Stop generation."""
    context = _caller()
    parsed = FullStopIntentRequest.model_validate({"request_context": context})
    assert set(parsed.model_dump(mode="json")) == {"request_context"}
    for extra in ({"expected_stop_generation": 0}, {"program_run_id": 1}):
        with pytest.raises(ValidationError):
            FullStopIntentRequest.model_validate({"request_context": context, **extra})


def test_recovery_action_carries_no_certificate_fence() -> None:
    """The operation revision alone fences a recovery action (L5, N3)."""
    for removed in ("expected_stop_certificate_id", "expected_stop_owner_generation"):
        with pytest.raises(ValidationError):
            OperationRecoveryActionRequest.model_validate(
                _recovery_action(**{removed: "x" if removed.endswith("_id") else 1})
            )


def test_program_run_end_carries_the_run_and_its_context_only() -> None:
    from manyforge_headless_api.models import ProgramRunStopIntentRequest

    body = {"request_context": {**_caller(), "request_key": _KEY}, "program_run_id": 3}
    assert ProgramRunStopIntentRequest.model_validate(body).program_run_id == 3
    with pytest.raises(ValidationError):
        ProgramRunStopIntentRequest.model_validate({**body, "reason": "x"})


def test_selection_release_is_fenced_by_the_hold_revision_alone() -> None:
    from manyforge_headless_api.models import ProgramSelectionResumeRequest

    body = {**_caller(), "program_run_id": 3, "expected_hold_revision": 2}
    assert ProgramSelectionResumeRequest.model_validate(body).expected_hold_revision == 2
    with pytest.raises(ValidationError):
        ProgramSelectionResumeRequest.model_validate(
            {**_caller(), "program_run_id": 3, "expected_selection_revision": 2}
        )


@pytest.mark.parametrize(
    "old_member",
    [
        {"request_payload_digest": "sha256:" + "e" * 64},
        {"owner_transaction_id": "authoring-transaction.1"},
    ],
)
def test_authoring_transaction_request_has_no_digest_or_transaction_id(old_member) -> None:
    from manyforge_headless_api.models import AuthoringMutationContext

    body = {**_caller(), "expected_admission_generation": 7}
    assert AuthoringMutationContext.model_validate(body).expected_admission_generation == 7
    with pytest.raises(ValidationError):
        AuthoringMutationContext.model_validate({**body, **old_member})


def _collision_policy(**changes) -> dict:
    body = {
        "policy_id": "free_space",
        "revision": {"revision_id": 1, "content_hash": "sha256:" + "a" * 64},
        "static_body_ids": [],
        "allowed_touch_link_pairs": [],
        "require_full_diagnostics": True,
    }
    body.update(changes)
    return body


def test_collision_policy_names_static_bodies_not_static_pairs() -> None:
    bodies = [f"scene/fixed-{index:03d}/shape/0" for index in range(200)]
    policy = CollisionPolicyRequest.model_validate(
        _collision_policy(static_body_ids=bodies)
    )
    assert policy.static_body_ids == bodies
    for invalid in (list(reversed(bodies)), [bodies[0], bodies[0]], [""]):
        with pytest.raises(ValidationError):
            CollisionPolicyRequest.model_validate(
                _collision_policy(static_body_ids=invalid)
            )
    # The static pair list is gone: one rule by body class.
    with pytest.raises(ValidationError):
        CollisionPolicyRequest.model_validate(
            _collision_policy(allowed_static_static_pairs=[])
        )
