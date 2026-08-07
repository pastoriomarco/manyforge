from __future__ import annotations

import pytest
from pydantic import ValidationError

from manyforge_headless_api.models import (
    AcknowledgeAlarmRequest,
    AcknowledgeAllAlarmsRequest,
    ManagerApplyRequest,
    OperationControlRequest,
    OperationParticipantRequest,
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
            "request_owner_epoch": scene_id,
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

    wrong_epoch = _scene_transaction()
    wrong_epoch["request_context"] = {
        **wrong_epoch["request_context"],
        "request_owner_epoch": "sha256:" + "e" * 64,
    }
    with pytest.raises(ValidationError, match="must equal the expected scene_id"):
        SceneTransactionRequest.model_validate(wrong_epoch)

    malformed_revision = _scene_transaction()
    malformed_revision["expected_projection_revision"] = {
        **malformed_revision["expected_projection_revision"],
        "graph_revision": "graph.1",
    }
    with pytest.raises(ValidationError):
        SceneTransactionRequest.model_validate(malformed_revision)


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
