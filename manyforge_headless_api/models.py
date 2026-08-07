"""Strict public request DTOs for the H0 HTTP projection."""

from __future__ import annotations

import unicodedata
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


MAX_IDENTITY_BYTES = 256
MAX_ACTIVATION_ID_BYTES = 2_048
MAX_REASON_BYTES = 4096
MAX_BULK_MEMBERS = 256
SHA256_PATTERN = r"^sha256:[0-9a-f]{64}$"


def _bounded_nfc(value: str, *, label: str, maximum_bytes: int) -> str:
    if not value or unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"{label} must be non-empty NFC text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{label} exceeds {maximum_bytes} UTF-8 bytes")
    return value


class StrictRequest(BaseModel):
    """Forbid coercion and caller-invented fields at public mutation ingress."""

    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)


class CallerAttributionRequest(StrictRequest):
    request_id: str
    request_owner_epoch: str
    source_label: str
    correlation_id: str | None = None

    @field_validator("request_id", "request_owner_epoch", "source_label")
    @classmethod
    def validate_required_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("correlation_id")
    @classmethod
    def validate_optional_identity(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="correlation_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class AcknowledgeAlarmRequest(CallerAttributionRequest):
    activation_id: str
    expected_record_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    reason: str | None = None
    confirmed: bool

    @field_validator("activation_id")
    @classmethod
    def validate_activation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="activation_id",
            maximum_bytes=MAX_ACTIVATION_ID_BYTES,
        )

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_REASON_BYTES)


class AlarmMemberRequest(StrictRequest):
    condition_id: str
    activation_id: str
    record_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]

    @field_validator("condition_id")
    @classmethod
    def validate_member_condition_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="condition_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("activation_id")
    @classmethod
    def validate_member_activation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="activation_id",
            maximum_bytes=MAX_ACTIVATION_ID_BYTES,
        )


class AcknowledgeAllAlarmsRequest(CallerAttributionRequest):
    members: Annotated[list[AlarmMemberRequest], Field(max_length=MAX_BULK_MEMBERS)]
    expected_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    reason: str | None = None
    confirmed: bool

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_REASON_BYTES)

    @model_validator(mode="after")
    def validate_exact_membership(self) -> "AcknowledgeAllAlarmsRequest":
        keys = [
            (item.condition_id, item.activation_id, item.record_revision)
            for item in self.members
        ]
        if keys != sorted(keys) or len(keys) != len(set(keys)):
            raise ValueError("members must be strictly sorted and unique")
        return self


class ExpectedCurrentRequest(StrictRequest):
    graph_revision: str
    retained_bundle_id: str

    @field_validator("graph_revision", "retained_bundle_id")
    @classmethod
    def validate_revision(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ManagerApplyRequest(CallerAttributionRequest):
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    expected_current: ExpectedCurrentRequest
    reject_if_affected_busy: bool

    @field_validator("deployment_artifact_id")
    @classmethod
    def validate_artifact_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="deployment_artifact_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def require_busy_rejection(self) -> "ManagerApplyRequest":
        if not self.reject_if_affected_busy:
            raise ValueError("reject_if_affected_busy must be true")
        return self


class SceneRevisionRequest(StrictRequest):
    scene_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    revision_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    content_hash: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("scene_id")
    @classmethod
    def validate_scene_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ProjectionRevisionRequest(StrictRequest):
    retained_bundle_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    graph_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    scene_revision: SceneRevisionRequest

    @field_validator("retained_bundle_id", "graph_revision")
    @classmethod
    def validate_projection_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class Vector3Request(StrictRequest):
    x_m: Annotated[float, Field(allow_inf_nan=False)]
    y_m: Annotated[float, Field(allow_inf_nan=False)]
    z_m: Annotated[float, Field(allow_inf_nan=False)]


class QuaternionRequest(StrictRequest):
    x: Annotated[float, Field(allow_inf_nan=False)]
    y: Annotated[float, Field(allow_inf_nan=False)]
    z: Annotated[float, Field(allow_inf_nan=False)]
    w: Annotated[float, Field(allow_inf_nan=False)]


class Pose3Request(StrictRequest):
    position_m: Vector3Request
    orientation: QuaternionRequest


class BoxShapeRequest(StrictRequest):
    type: Literal["box"]
    box_dimensions_m: Vector3Request

    @model_validator(mode="after")
    def require_positive_dimensions(self) -> "BoxShapeRequest":
        dimensions = self.box_dimensions_m
        if min(dimensions.x_m, dimensions.y_m, dimensions.z_m) <= 0:
            raise ValueError("box dimensions must be positive")
        return self


class SphereShapeRequest(StrictRequest):
    type: Literal["sphere"]
    sphere_radius_m: Annotated[float, Field(gt=0, allow_inf_nan=False)]


class CylinderShapeRequest(StrictRequest):
    type: Literal["cylinder"]
    cylinder_radius_m: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    cylinder_height_m: Annotated[float, Field(gt=0, allow_inf_nan=False)]


class MeshShapeRequest(StrictRequest):
    type: Literal["mesh"]
    mesh_resource_uri: str
    mesh_scale: Vector3Request

    @field_validator("mesh_resource_uri")
    @classmethod
    def validate_mesh_uri(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="mesh_resource_uri",
            maximum_bytes=MAX_ACTIVATION_ID_BYTES,
        )

    @model_validator(mode="after")
    def require_positive_scale(self) -> "MeshShapeRequest":
        if min(self.mesh_scale.x_m, self.mesh_scale.y_m, self.mesh_scale.z_m) <= 0:
            raise ValueError("mesh scale must be positive")
        return self


CollisionShapeRequest = Annotated[
    BoxShapeRequest | SphereShapeRequest | CylinderShapeRequest | MeshShapeRequest,
    Field(discriminator="type"),
]


class CellSceneFrameRequest(StrictRequest):
    kind: Literal["cell_frame"]
    cell_id: str

    @field_validator("cell_id")
    @classmethod
    def validate_cell_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="cell_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ComponentSceneFrameRequest(StrictRequest):
    kind: Literal["component_frame"]
    controllable_component_id: str
    model_instance_id: str
    frame_role_id: str

    @field_validator(
        "controllable_component_id", "model_instance_id", "frame_role_id"
    )
    @classmethod
    def validate_frame_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class PassiveModelSceneFrameRequest(StrictRequest):
    kind: Literal["passive_model_frame"]
    model_instance_id: str
    frame_role_id: str

    @field_validator("model_instance_id", "frame_role_id")
    @classmethod
    def validate_frame_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


SceneFrameRequest = Annotated[
    CellSceneFrameRequest
    | ComponentSceneFrameRequest
    | PassiveModelSceneFrameRequest,
    Field(discriminator="kind"),
]


class CollisionPairRequest(StrictRequest):
    body_a: str
    body_b: str

    @field_validator("body_a", "body_b")
    @classmethod
    def validate_body(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def require_canonical_pair(self) -> "CollisionPairRequest":
        if self.body_a >= self.body_b:
            raise ValueError("collision pair must use canonical distinct body order")
        return self


class CollisionPolicyRevisionRequest(StrictRequest):
    revision_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    content_hash: Annotated[str, Field(pattern=SHA256_PATTERN)]


class SupportContactPolicyRequest(StrictRequest):
    pair: CollisionPairRequest
    mode: Literal["contact_release", "bounded_penetration", "contact_process"]
    release_evidence_mode: Literal["signed_penetration", "binary_contact"]
    enforce_max_start_penetration: bool
    max_start_penetration_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    release_tolerance_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    min_release_progress_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    max_contact_penetration_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    require_separation_by_end: bool
    require_strictly_decreasing_penetration: bool
    allow_recontact: bool


class ContactGraspPolicyRequest(StrictRequest):
    policy_id: str
    selected_object_id: str
    allowed_robot_links: Annotated[list[str], Field(max_length=4096)]
    max_penetration_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    terminal_window_start_fraction: Annotated[
        float, Field(ge=0, le=1, allow_inf_nan=False)
    ]

    @field_validator("policy_id", "selected_object_id")
    @classmethod
    def validate_grasp_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("allowed_robot_links")
    @classmethod
    def validate_allowed_links(cls, values: list[str]) -> list[str]:
        normalized = [
            _bounded_nfc(
                value,
                label="allowed_robot_link",
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
            for value in values
        ]
        if normalized != sorted(set(normalized)):
            raise ValueError("allowed_robot_links must be sorted and unique")
        return normalized


class CollisionPolicyRequest(StrictRequest):
    policy_id: str
    revision: CollisionPolicyRevisionRequest
    allowed_static_static_pairs: Annotated[
        list[CollisionPairRequest], Field(max_length=4096)
    ]
    allowed_touch_link_pairs: Annotated[
        list[CollisionPairRequest], Field(max_length=4096)
    ]
    support_contacts: Annotated[
        list[SupportContactPolicyRequest], Field(max_length=4096)
    ]
    contact_grasps: Annotated[
        list[ContactGraspPolicyRequest], Field(max_length=4096)
    ]
    require_full_diagnostics: bool

    @field_validator("policy_id")
    @classmethod
    def validate_policy_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="policy_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def require_canonical_collections(self) -> "CollisionPolicyRequest":
        for label, values in (
            ("allowed_static_static_pairs", self.allowed_static_static_pairs),
            ("allowed_touch_link_pairs", self.allowed_touch_link_pairs),
        ):
            keys = [(item.body_a, item.body_b) for item in values]
            if keys != sorted(set(keys)):
                raise ValueError(f"{label} must be sorted and unique")
        support_keys = [
            (item.pair.body_a, item.pair.body_b) for item in self.support_contacts
        ]
        if support_keys != sorted(set(support_keys)):
            raise ValueError("support_contacts must be sorted and unique by pair")
        grasp_keys = [item.policy_id for item in self.contact_grasps]
        if grasp_keys != sorted(set(grasp_keys)):
            raise ValueError("contact_grasps must be sorted and unique by policy_id")
        return self


class AddSceneObjectRequest(StrictRequest):
    kind: Literal["add_object"]
    scene_resource_id: str
    shape: CollisionShapeRequest
    pose: Pose3Request
    frame_ref: SceneFrameRequest

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class RemoveSceneObjectRequest(StrictRequest):
    kind: Literal["remove_object"]
    scene_resource_id: str

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class UpdateSceneObjectPoseRequest(StrictRequest):
    kind: Literal["update_object_pose"]
    scene_resource_id: str
    pose: Pose3Request
    frame_ref: SceneFrameRequest
    # Provider-backed ingress requires a separately configured trusted
    # provider adapter so received_at_ns cannot be forged by an open caller.
    provider_evidence: None = None
    expected_attached: bool | None = None

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class AttachCarriedObjectRequest(StrictRequest):
    kind: Literal["attach_carried_object"]
    carried_object_attachment_id: str
    scene_resource_id: str
    controllable_component_id: str
    command_target_id: str
    frame_role_id: str
    touch_body_roles: Annotated[list[str], Field(max_length=4096)]
    expected_component_attachment_revisions: Annotated[
        list[Annotated[str, Field(pattern=SHA256_PATTERN)]],
        Field(max_length=4096),
    ]

    @field_validator(
        "carried_object_attachment_id",
        "scene_resource_id",
        "controllable_component_id",
        "command_target_id",
        "frame_role_id",
    )
    @classmethod
    def validate_attachment_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator(
        "touch_body_roles", "expected_component_attachment_revisions"
    )
    @classmethod
    def validate_sorted_identities(cls, values: list[str], info) -> list[str]:
        normalized = [
            _bounded_nfc(
                value,
                label=info.field_name,
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
            for value in values
        ]
        if normalized != sorted(set(normalized)):
            raise ValueError(f"{info.field_name} must be sorted and unique")
        return normalized


class DetachCarriedObjectRequest(StrictRequest):
    kind: Literal["detach_carried_object"]
    carried_object_attachment_id: str
    expected_carried_object_attachment_revision: Annotated[
        str, Field(pattern=SHA256_PATTERN)
    ]
    pose: Pose3Request
    frame_ref: SceneFrameRequest

    @field_validator(
        "carried_object_attachment_id",
        "expected_carried_object_attachment_revision",
    )
    @classmethod
    def validate_attachment_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class SetCollisionPolicyRequest(StrictRequest):
    kind: Literal["set_collision_policy"]
    configured_policy: CollisionPolicyRequest
    expected_configured_policy_revision: CollisionPolicyRevisionRequest


SceneOperationRequest = Annotated[
    AddSceneObjectRequest
    | RemoveSceneObjectRequest
    | UpdateSceneObjectPoseRequest
    | AttachCarriedObjectRequest
    | DetachCarriedObjectRequest
    | SetCollisionPolicyRequest,
    Field(discriminator="kind"),
]


class SceneTransactionRequest(StrictRequest):
    request_context: CallerAttributionRequest
    expected_projection_revision: ProjectionRevisionRequest
    operation: SceneOperationRequest

    @model_validator(mode="after")
    def require_scene_owner_epoch(self) -> "SceneTransactionRequest":
        if (
            self.request_context.request_owner_epoch
            != self.expected_projection_revision.scene_revision.scene_id
        ):
            raise ValueError(
                "request_owner_epoch must equal the expected scene_id"
            )
        return self


class ActionSpanRequest(StrictRequest):
    action_source: Literal[
        "program_node", "direct_control", "robot_control", "mcp", "system"
    ]
    action_span_id: str
    program_run_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None
    parent_node_attempt_id: str | None = None

    @field_validator(
        "action_span_id",
        "program_run_id",
        "node_instance_id",
        "node_attempt_id",
        "parent_node_attempt_id",
    )
    @classmethod
    def validate_span_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_program_span(self) -> "ActionSpanRequest":
        if self.action_source == "program_node" and (
            self.node_instance_id is None
            or self.node_attempt_id is None
            or self.action_span_id != self.node_attempt_id
        ):
            raise ValueError(
                "program_node spans require a node instance and an equal "
                "action-span/node-attempt id"
            )
        return self


class MotionTolerancesRequest(StrictRequest):
    joint_position_tol_rad: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    translation_tol_m: Annotated[float, Field(ge=0, allow_inf_nan=False)]
    rotation_tol_rad: Annotated[float, Field(ge=0, allow_inf_nan=False)]


class MotionSpeedProfileRequest(StrictRequest):
    velocity_scale: Annotated[
        float, Field(gt=0, le=1, allow_inf_nan=False)
    ]
    acceleration_scale: Annotated[
        float, Field(gt=0, le=1, allow_inf_nan=False)
    ]
    max_tcp_linear_speed_mps: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None


class MotionTimeoutsRequest(StrictRequest):
    planning_timeout_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    execution_timeout_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]


class JointDynamicsLimitRequest(StrictRequest):
    max_velocity_rad_s: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None
    max_acceleration_rad_s2: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None
    max_jerk_rad_s3: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None


class JointGoalRequest(StrictRequest):
    kind: Literal["joint_goal"]
    joint_names: Annotated[list[str], Field(min_length=1, max_length=4096)]
    positions_rad: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=1, max_length=4096),
    ]

    @field_validator("joint_names")
    @classmethod
    def validate_joint_names(cls, values: list[str]) -> list[str]:
        normalized = [
            _bounded_nfc(
                value,
                label="joint_name",
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
            for value in values
        ]
        if len(normalized) != len(set(normalized)):
            raise ValueError("joint_names must be unique")
        return normalized

    @model_validator(mode="after")
    def validate_joint_coordinates(self) -> "JointGoalRequest":
        if len(self.joint_names) != len(self.positions_rad):
            raise ValueError("joint_names and positions_rad must have equal length")
        return self


class AngularPositionRequest(StrictRequest):
    kind: Literal["angular_rad"]
    value_rad: Annotated[float, Field(allow_inf_nan=False)]


class LinearPositionRequest(StrictRequest):
    kind: Literal["linear_m"]
    value_m: Annotated[float, Field(allow_inf_nan=False)]


CoordinatePositionRequest = Annotated[
    AngularPositionRequest | LinearPositionRequest,
    Field(discriminator="kind"),
]


class AngularEffortRequest(StrictRequest):
    kind: Literal["angular_newton_meter"]
    value_nm: Annotated[float, Field(ge=0, allow_inf_nan=False)]


class LinearEffortRequest(StrictRequest):
    kind: Literal["linear_newton"]
    value_n: Annotated[float, Field(ge=0, allow_inf_nan=False)]


CoordinateEffortRequest = Annotated[
    AngularEffortRequest | LinearEffortRequest,
    Field(discriminator="kind"),
]


class GripperPositionCommandValueRequest(StrictRequest):
    kind: Literal["gripper_position"]
    position: CoordinatePositionRequest
    max_effort: CoordinateEffortRequest | None

    @model_validator(mode="after")
    def require_matching_effort_kind(self) -> "GripperPositionCommandValueRequest":
        if self.max_effort is None:
            return self
        angular_position = isinstance(self.position, AngularPositionRequest)
        angular_effort = isinstance(self.max_effort, AngularEffortRequest)
        if angular_position != angular_effort:
            raise ValueError("max_effort must match the gripper position kind")
        return self


class CommandDurationOptionRequest(StrictRequest):
    option_id: str
    value_s: Annotated[float, Field(ge=0, allow_inf_nan=False)]

    @field_validator("option_id")
    @classmethod
    def validate_option_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="option_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class CapabilityCommandParticipantRequest(StrictRequest):
    kind: Literal["capability_command"]
    command_schema_id: str
    command_schema_revision: str
    command_value: GripperPositionCommandValueRequest
    completion_timeout_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    caller_options: Annotated[
        list[CommandDurationOptionRequest], Field(max_length=64)
    ]

    @field_validator("command_schema_id", "command_schema_revision")
    @classmethod
    def validate_command_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def require_unique_options(self) -> "CapabilityCommandParticipantRequest":
        option_ids = [option.option_id for option in self.caller_options]
        if len(option_ids) != len(set(option_ids)):
            raise ValueError("caller_options must have unique option_id values")
        return self


class MotionParticipantRequest(StrictRequest):
    kind: Literal["motion"]
    move_id: str
    base_frame_role_id: str
    tcp_frame_role_id: str
    tolerances: MotionTolerancesRequest
    speed_profile: MotionSpeedProfileRequest
    timeouts: MotionTimeoutsRequest
    cache_mode: Literal["default", "disable"]
    planning_profile: Literal["first_feasible", "balanced", "quality_bounded"]
    target_mode: Literal["absolute"]
    target_reference: None = None
    offset_reference: None = None
    motion: JointGoalRequest
    contact_policy_id: None = None
    per_joint_dynamics_limits: dict[str, JointDynamicsLimitRequest]
    motion_interrupt_scopes: Annotated[list[None], Field(max_length=0)]

    @field_validator("move_id", "base_frame_role_id", "tcp_frame_role_id")
    @classmethod
    def validate_motion_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("per_joint_dynamics_limits")
    @classmethod
    def validate_dynamics_keys(
        cls, values: dict[str, JointDynamicsLimitRequest]
    ) -> dict[str, JointDynamicsLimitRequest]:
        if len(values) > 4096:
            raise ValueError("per_joint_dynamics_limits exceeds 4096 entries")
        for key in values:
            _bounded_nfc(
                key,
                label="per_joint_dynamics_limits key",
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
        return values


class OperationParticipantRequest(StrictRequest):
    component_role_id: str
    target_role_id: str
    required_capability_id: str
    action_span: ActionSpanRequest | None
    payload: Annotated[
        MotionParticipantRequest | CapabilityCommandParticipantRequest,
        Field(discriminator="kind"),
    ]

    @field_validator(
        "component_role_id", "target_role_id", "required_capability_id"
    )
    @classmethod
    def validate_participant_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class OperationIntentRequest(StrictRequest):
    request_context: CallerAttributionRequest
    program_id: str
    program_revision: str
    program_binding_id: str
    program_run_id: str
    expected_graph_revision: str
    expected_retained_bundle_id: str
    expected_scene_revision: SceneRevisionRequest | None
    participants: Annotated[
        list[OperationParticipantRequest], Field(min_length=1, max_length=1)
    ]
    coordination_policy: Literal["independent"]

    @field_validator(
        "program_id",
        "program_revision",
        "program_binding_id",
        "program_run_id",
        "expected_graph_revision",
        "expected_retained_bundle_id",
    )
    @classmethod
    def validate_operation_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_program_span_run(self) -> "OperationIntentRequest":
        span = self.participants[0].action_span
        if (
            span is not None
            and span.program_run_id is not None
            and span.program_run_id != self.program_run_id
        ):
            raise ValueError("action span program_run_id must match the operation")
        return self


class OperationControlRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operation_id: str
    expected_operation_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    reason: str | None = None

    @field_validator("operation_id")
    @classmethod
    def validate_operation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="operation_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_REASON_BYTES)


class StopOperationTargetRequest(StrictRequest):
    operation_id: str
    expected_operation_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_stop_generation: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_991)
    ]

    @field_validator("operation_id")
    @classmethod
    def validate_operation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="operation_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class StopIntentRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operations: Annotated[
        list[StopOperationTargetRequest], Field(min_length=1, max_length=1)
    ]
    reason: str | None = None

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_REASON_BYTES)


class RuntimeScaleRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operation_id: str
    expected_operation_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_runtime_scale_generation: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_991)
    ]
    scale: Annotated[float, Field(ge=0, le=1, allow_inf_nan=False)]

    @field_validator("operation_id")
    @classmethod
    def validate_operation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="operation_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class HistorianReconcileRequest(CallerAttributionRequest):
    expected_health_generation: Annotated[
        int,
        Field(ge=1, le=9_007_199_254_740_991),
    ]
