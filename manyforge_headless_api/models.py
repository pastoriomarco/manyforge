"""Strict public request DTOs for the H0 HTTP projection."""

from __future__ import annotations

import unicodedata
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


MAX_IDENTITY_BYTES = 256
MAX_ACTIVATION_ID_BYTES = 2_048
MAX_REASON_BYTES = 4096
MAX_BULK_MEMBERS = 256
MAX_SCENE_POSE_QUERY_RESOURCES = 128
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


class ProgramApplyRequest(StrictRequest):
    scenario_id: Annotated[
        str,
        Field(min_length=1, max_length=128, pattern=r"^[a-z][a-z0-9_.-]*$"),
    ]
    scenario_catalog_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program: dict[str, Any]
    expected_applied_record_digest: Annotated[
        str | None,
        Field(pattern=SHA256_PATTERN),
    ] = None

    @model_validator(mode="after")
    def require_program_object(self) -> "ProgramApplyRequest":
        if not self.program:
            raise ValueError("program must be a non-empty object")
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


class SceneResourcePoseQuery(StrictRequest):
    api_version: Literal["manyforge.scene_resource_pose_query.v1"]
    expected_projection_revision: ProjectionRevisionRequest
    scene_resource_ids: Annotated[
        list[str],
        Field(min_length=1, max_length=MAX_SCENE_POSE_QUERY_RESOURCES),
    ]

    @field_validator("scene_resource_ids")
    @classmethod
    def validate_scene_resource_ids(cls, values: list[str]) -> list[str]:
        normalized = [
            _bounded_nfc(
                value,
                label="scene_resource_id",
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
            for value in values
        ]
        if normalized != sorted(set(normalized)):
            raise ValueError(
                "scene_resource_ids must be strictly sorted and unique"
            )
        return normalized


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


class SceneResourceSceneFrameRequest(StrictRequest):
    kind: Literal["scene_resource"]
    scene_resource_id: str

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


SceneTransactionFrameRequest = Annotated[
    CellSceneFrameRequest
    | ComponentSceneFrameRequest
    | PassiveModelSceneFrameRequest
    | SceneResourceSceneFrameRequest,
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


class SceneSupportContactRequest(StrictRequest):
    """Portable object-to-object support intent, never a native body pair."""

    support_object_id: str
    mode: Literal[
        "contact_release", "bounded_penetration", "contact_process"
    ] = "contact_release"
    release_evidence_mode: Literal[
        "signed_penetration", "binary_contact"
    ] = "signed_penetration"
    enforce_max_start_penetration: bool = True
    max_start_penetration_m: Annotated[
        float, Field(ge=0, allow_inf_nan=False)
    ] = 0.0
    release_tolerance_m: Annotated[
        float, Field(ge=0, allow_inf_nan=False)
    ] = 1e-5
    min_release_progress_m: Annotated[
        float, Field(ge=0, allow_inf_nan=False)
    ] = 0.0
    max_contact_penetration_m: Annotated[
        float, Field(ge=0, allow_inf_nan=False)
    ] = 0.0
    require_separation_by_end: bool = True
    require_strictly_decreasing_penetration: bool = True
    allow_recontact: bool = False

    @model_validator(mode="before")
    @classmethod
    def fill_mode_scoped_neutral_defaults(cls, value):
        if not isinstance(value, dict):
            return value
        normalized = dict(value)
        mode = normalized.get("mode", "contact_release")
        if mode in {"bounded_penetration", "contact_process"}:
            normalized.setdefault("release_evidence_mode", "signed_penetration")
            normalized.setdefault("enforce_max_start_penetration", False)
            normalized.setdefault("max_start_penetration_m", 0.0)
            normalized.setdefault("release_tolerance_m", 0.0)
            normalized.setdefault("min_release_progress_m", 0.0)
            normalized.setdefault("require_separation_by_end", False)
            normalized.setdefault(
                "require_strictly_decreasing_penetration", False
            )
            normalized.setdefault("allow_recontact", False)
        elif normalized.get(
            "release_evidence_mode", "signed_penetration"
        ) == "binary_contact":
            normalized.setdefault("enforce_max_start_penetration", False)
            normalized.setdefault("max_start_penetration_m", 0.0)
            normalized.setdefault("release_tolerance_m", 0.0)
            normalized.setdefault("min_release_progress_m", 0.0)
            normalized.setdefault(
                "require_strictly_decreasing_penetration", False
            )
        return normalized

    @field_validator("support_object_id")
    @classmethod
    def validate_support_object_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="support_object_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_mode_scoped_fields(self) -> "SceneSupportContactRequest":
        if self.mode == "contact_release":
            if self.max_contact_penetration_m != 0.0:
                raise ValueError(
                    "max_contact_penetration_m is valid only for "
                    "bounded_penetration"
                )
            if self.release_evidence_mode == "binary_contact" and (
                self.enforce_max_start_penetration
                or self.max_start_penetration_m != 0.0
                or self.release_tolerance_m != 0.0
                or self.min_release_progress_m != 0.0
                or self.require_strictly_decreasing_penetration
            ):
                raise ValueError(
                    "signed-distance metric fields must be neutral for "
                    "binary_contact"
                )
            return self

        if self.release_evidence_mode != "signed_penetration":
            raise ValueError(
                "release_evidence_mode is not applicable outside "
                "contact_release"
            )
        if (
            self.enforce_max_start_penetration
            or self.max_start_penetration_m != 0.0
            or self.release_tolerance_m != 0.0
            or self.min_release_progress_m != 0.0
            or self.require_separation_by_end
            or self.require_strictly_decreasing_penetration
            or self.allow_recontact
        ):
            raise ValueError(
                "release-only fields must be neutral outside contact_release"
            )
        if (
            self.mode == "contact_process"
            and self.max_contact_penetration_m != 0.0
        ):
            raise ValueError(
                "max_contact_penetration_m is valid only for "
                "bounded_penetration"
            )
        return self


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
    frame_ref: SceneTransactionFrameRequest
    support_contacts: Annotated[
        list[SceneSupportContactRequest], Field(max_length=4096)
    ] = Field(default_factory=list)

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("support_contacts")
    @classmethod
    def canonicalize_support_contacts(
        cls, values: list[SceneSupportContactRequest]
    ) -> list[SceneSupportContactRequest]:
        ordered = sorted(values, key=lambda value: value.support_object_id)
        ids = [value.support_object_id for value in ordered]
        if len(ids) != len(set(ids)):
            raise ValueError("support_contacts contains a duplicate support")
        return ordered

    @model_validator(mode="after")
    def reject_self_support(self) -> "AddSceneObjectRequest":
        if any(
            contact.support_object_id == self.scene_resource_id
            for contact in self.support_contacts
        ):
            raise ValueError("scene resource cannot support itself")
        return self


class UpsertSceneObjectRequest(AddSceneObjectRequest):
    kind: Literal["upsert_object"]


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
    frame_ref: SceneTransactionFrameRequest
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
    frame_ref: SceneTransactionFrameRequest

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


class DetachCarriedObjectAtCurrentPoseRequest(StrictRequest):
    kind: Literal["detach_carried_object_at_current_pose"]
    carried_object_attachment_id: str
    expected_carried_object_attachment_revision: Annotated[
        str, Field(pattern=SHA256_PATTERN)
    ]
    expected_scene_resource_id: str
    expected_controllable_component_id: str
    expected_command_target_id: str

    @field_validator(
        "carried_object_attachment_id",
        "expected_carried_object_attachment_revision",
        "expected_scene_resource_id",
        "expected_controllable_component_id",
        "expected_command_target_id",
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
    | UpsertSceneObjectRequest
    | RemoveSceneObjectRequest
    | UpdateSceneObjectPoseRequest
    | AttachCarriedObjectRequest
    | DetachCarriedObjectRequest
    | DetachCarriedObjectAtCurrentPoseRequest
    | SetCollisionPolicyRequest,
    Field(discriminator="kind"),
]


class SceneTransactionRequest(StrictRequest):
    request_context: CallerAttributionRequest
    expected_projection_revision: ProjectionRevisionRequest
    operation: SceneOperationRequest


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


class PoseGoalRequest(StrictRequest):
    kind: Literal["pose_goal"]
    goal: Pose3Request


class CartesianPathRequest(StrictRequest):
    kind: Literal["cartesian_path"]
    waypoints: Annotated[list[Pose3Request], Field(min_length=1, max_length=4096)]
    max_translation_step_m: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    max_rotation_step_rad: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    speed_mode: Literal["bounded"]
    max_linear_speed_mps: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    max_angular_speed_rad_s: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    max_linear_acceleration_mps2: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    max_angular_acceleration_rad_s2: Annotated[
        float, Field(gt=0, allow_inf_nan=False)
    ]
    fallback_policy: Literal["fail_closed", "allow_joint_space_fallback"]


MotionSpecRequest = Annotated[
    JointGoalRequest | PoseGoalRequest | CartesianPathRequest,
    Field(discriminator="kind"),
]


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


class FrameRoleReferenceRequest(StrictRequest):
    type: Literal["frame_role"]
    frame_role_id: str

    @field_validator("frame_role_id")
    @classmethod
    def validate_frame_role_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="frame_role_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ComponentBodyRoleReferenceRequest(StrictRequest):
    type: Literal["component_body_role"]
    collision_body_role_id: str

    @field_validator("collision_body_role_id")
    @classmethod
    def validate_collision_body_role_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="collision_body_role_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class SceneResourceReferenceRequest(StrictRequest):
    type: Literal["scene_resource"]
    scene_resource_id: str

    @field_validator("scene_resource_id")
    @classmethod
    def validate_scene_resource_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="scene_resource_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class RuntimeObjectReferenceRequest(StrictRequest):
    type: Literal["runtime_object"]
    runtime_object_id: str

    @field_validator("runtime_object_id")
    @classmethod
    def validate_runtime_object_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="runtime_object_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class InlinePoseReferenceRequest(StrictRequest):
    type: Literal["pose"]
    pose: Pose3Request


MotionReferenceRequest = Annotated[
    FrameRoleReferenceRequest
    | ComponentBodyRoleReferenceRequest
    | SceneResourceReferenceRequest
    | RuntimeObjectReferenceRequest
    | InlinePoseReferenceRequest,
    Field(discriminator="type"),
]


class RuntimeObjectReferenceBindingRequest(StrictRequest):
    runtime_object_id: str
    scene_resource_id: str

    @field_validator("runtime_object_id", "scene_resource_id")
    @classmethod
    def validate_runtime_object_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


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
    target_mode: Literal["absolute", "relative"]
    target_reference: MotionReferenceRequest | None = None
    offset_reference: MotionReferenceRequest | None = None
    motion: MotionSpecRequest
    contact_policy_id: str | None = None
    per_joint_dynamics_limits: dict[str, JointDynamicsLimitRequest]
    motion_interrupt_scopes: Annotated[list[None], Field(max_length=0)]
    runtime_object_bindings: list[RuntimeObjectReferenceBindingRequest] = Field(
        default_factory=list, max_length=4096
    )

    @field_validator(
        "move_id", "base_frame_role_id", "tcp_frame_role_id", "contact_policy_id"
    )
    @classmethod
    def validate_motion_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
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

    @model_validator(mode="after")
    def validate_motion_scoped_fields(self) -> "MotionParticipantRequest":
        runtime_objects = [
            value.runtime_object_id for value in self.runtime_object_bindings
        ]
        if len(runtime_objects) != len(set(runtime_objects)):
            raise ValueError(
                "runtime_object_bindings must have unique runtime_object_id values"
            )
        referenced_runtime_objects = {
            reference.runtime_object_id
            for reference in (self.target_reference, self.offset_reference)
            if isinstance(reference, RuntimeObjectReferenceRequest)
        }
        bound_runtime_objects = set(runtime_objects)
        if not referenced_runtime_objects.issubset(bound_runtime_objects) or (
            self.contact_policy_id is None
            and bound_runtime_objects != referenced_runtime_objects
        ):
            raise ValueError(
                "runtime_object_bindings must exactly cover motion references"
            )
        is_joint = isinstance(self.motion, JointGoalRequest)
        if is_joint and (
            self.contact_policy_id is not None
            or self.runtime_object_bindings
            or self.target_reference is not None
            or self.offset_reference is not None
        ):
            raise ValueError(
                "joint_goal cannot carry contact-policy, spatial references, or "
                "runtime-object bindings"
            )
        if self.target_mode == "absolute" and (
            self.target_reference is not None or self.offset_reference is not None
        ):
            raise ValueError("absolute motion cannot carry references")
        if self.target_mode == "relative" and not is_joint:
            if self.target_reference is None:
                raise ValueError(
                    "relative pose and Cartesian motion require target_reference"
                )
        return self


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


class PickOffsetEnvelopeRequest(StrictRequest):
    position_min_m: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=3, max_length=3),
    ]
    position_max_m: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=3, max_length=3),
    ]
    orientation_rpy_min_deg: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=3, max_length=3),
    ]
    orientation_rpy_max_deg: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=3, max_length=3),
    ]

    @model_validator(mode="after")
    def validate_ordered_envelope(self) -> "PickOffsetEnvelopeRequest":
        if any(
            minimum > maximum
            for minimum, maximum in zip(
                self.position_min_m, self.position_max_m, strict=True
            )
        ) or any(
            minimum > maximum
            for minimum, maximum in zip(
                self.orientation_rpy_min_deg,
                self.orientation_rpy_max_deg,
                strict=True,
            )
        ):
            raise ValueError("pick envelope minima cannot exceed maxima")
        return self


PickFrameRoleReferenceRequest = FrameRoleReferenceRequest
PickComponentBodyRoleReferenceRequest = ComponentBodyRoleReferenceRequest
PickSceneResourceReferenceRequest = SceneResourceReferenceRequest
PickRuntimeObjectReferenceRequest = RuntimeObjectReferenceRequest
PickInlinePoseReferenceRequest = InlinePoseReferenceRequest
PickOffsetReferenceRequest = MotionReferenceRequest


class PickCandidateSynthesisRequest(StrictRequest):
    pick_offset_envelope: PickOffsetEnvelopeRequest = Field(
        default_factory=lambda: PickOffsetEnvelopeRequest(
            position_min_m=[0.0, 0.0, 0.0],
            position_max_m=[0.0, 0.0, 0.0],
            orientation_rpy_min_deg=[0.0, 0.0, 0.0],
            orientation_rpy_max_deg=[0.0, 0.0, 0.0],
        )
    )
    approach_offset_envelope: PickOffsetEnvelopeRequest = Field(
        default_factory=lambda: PickOffsetEnvelopeRequest(
            position_min_m=[0.0, 0.0, 0.05],
            position_max_m=[0.0, 0.0, 0.1],
            orientation_rpy_min_deg=[0.0, 0.0, 0.0],
            orientation_rpy_max_deg=[0.0, 0.0, 0.0],
        )
    )
    pick_offset_reference: PickOffsetReferenceRequest | None = None
    approach_offset_reference: PickOffsetReferenceRequest | None = None
    base_tool_orientation_rpy_deg: Annotated[
        list[Annotated[float, Field(allow_inf_nan=False)]],
        Field(min_length=3, max_length=3),
    ] = Field(default_factory=lambda: [180.0, 0.0, 0.0])
    translation_step_m: Annotated[float, Field(gt=0, allow_inf_nan=False)] = 0.01
    rotation_step_deg: Annotated[float, Field(gt=0, allow_inf_nan=False)] = 15.0
    force_vertical_pick: bool = False
    min_tcp_z_m: Annotated[float | None, Field(ge=0, allow_inf_nan=False)] = None
    maximum_candidate_count: Annotated[int, Field(ge=1, le=4096)] = 4096
    search_strategy: Literal["first_feasible", "best_cost"] = "first_feasible"


def _validate_pick_place_operations(
    approach: OperationIntentRequest,
    descend: OperationIntentRequest,
    tool: OperationIntentRequest,
    retreat: OperationIntentRequest,
) -> None:
    operations = (approach, descend, tool, retreat)
    request_ids = [operation.request_context.request_id for operation in operations]
    if len(request_ids) != len(set(request_ids)):
        raise ValueError("pick/place phase request_id values must be distinct")
    contexts = {
        (
            operation.program_id,
            operation.program_revision,
            operation.program_binding_id,
            operation.program_run_id,
            operation.expected_graph_revision,
            operation.expected_retained_bundle_id,
            operation.expected_scene_revision,
            operation.request_context.request_owner_epoch,
        )
        for operation in operations
    }
    if len(contexts) != 1:
        raise ValueError("pick/place phases must share one Program and owner context")
    if approach.expected_scene_revision is None:
        raise ValueError("pick/place planning requires one exact scene revision")
    if not isinstance(approach.participants[0].payload, MotionParticipantRequest):
        raise ValueError("pick/place approach must be a motion operation")
    if not isinstance(descend.participants[0].payload, MotionParticipantRequest):
        raise ValueError("pick/place descend must be a motion operation")
    if not isinstance(tool.participants[0].payload, CapabilityCommandParticipantRequest):
        raise ValueError("pick/place tool phase must be a capability command")
    if not isinstance(retreat.participants[0].payload, MotionParticipantRequest):
        raise ValueError("pick/place retreat must be a motion operation")
    motion_participants = (
        approach.participants[0],
        descend.participants[0],
        retreat.participants[0],
    )
    motion_targets = {
        (
            participant.component_role_id,
            participant.target_role_id,
            participant.required_capability_id,
            participant.payload.base_frame_role_id,
            participant.payload.tcp_frame_role_id,
        )
        for participant in motion_participants
        if isinstance(participant.payload, MotionParticipantRequest)
    }
    if len(motion_targets) != 1:
        raise ValueError("pick/place motion phases must share one target and frame set")


class PickSequencePlanningRequest(StrictRequest):
    sequence_id: str
    selected_object_id: str
    selected_runtime_object_id: str
    carrier_frame_role_id: str
    synthesis: PickCandidateSynthesisRequest = Field(
        default_factory=PickCandidateSynthesisRequest
    )
    approach_operation: OperationIntentRequest
    descend_operation: OperationIntentRequest
    tool_operation: OperationIntentRequest
    retreat_operation: OperationIntentRequest

    @field_validator("sequence_id")
    @classmethod
    def validate_sequence_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="sequence_id", maximum_bytes=96)

    @field_validator(
        "selected_object_id", "selected_runtime_object_id", "carrier_frame_role_id"
    )
    @classmethod
    def validate_pick_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_pick_phases(self) -> "PickSequencePlanningRequest":
        _validate_pick_place_operations(
            self.approach_operation,
            self.descend_operation,
            self.tool_operation,
            self.retreat_operation,
        )
        approach = self.approach_operation.participants[0].payload
        descend = self.descend_operation.participants[0].payload
        retreat = self.retreat_operation.participants[0].payload
        if not all(
            isinstance(value, MotionParticipantRequest)
            for value in (approach, descend, retreat)
        ):
            return self
        if approach.contact_policy_id is not None or approach.runtime_object_bindings:
            raise ValueError("pick approach cannot carry contact-policy state")
        if (
            descend.contact_policy_id is None
            or len(descend.runtime_object_bindings) != 1
            or descend.runtime_object_bindings[0].runtime_object_id
            != self.selected_runtime_object_id
            or descend.runtime_object_bindings[0].scene_resource_id
            != self.selected_object_id
        ):
            raise ValueError(
                "pick descend requires one exact selected-object contact binding"
            )
        if retreat.contact_policy_id is not None or retreat.runtime_object_bindings:
            raise ValueError("pick retreat cannot carry contact-policy state")
        return self


class PlaceSequencePlanningRequest(StrictRequest):
    sequence_id: str
    selected_object_id: str
    carrier_frame_role_id: str
    approach_operation: OperationIntentRequest
    descend_operation: OperationIntentRequest
    tool_operation: OperationIntentRequest
    retreat_operation: OperationIntentRequest

    @field_validator("sequence_id")
    @classmethod
    def validate_sequence_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="sequence_id", maximum_bytes=96)

    @field_validator("selected_object_id", "carrier_frame_role_id")
    @classmethod
    def validate_place_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_place_phases(self) -> "PlaceSequencePlanningRequest":
        _validate_pick_place_operations(
            self.approach_operation,
            self.descend_operation,
            self.tool_operation,
            self.retreat_operation,
        )
        motions = (
            self.approach_operation.participants[0].payload,
            self.descend_operation.participants[0].payload,
            self.retreat_operation.participants[0].payload,
        )
        if any(
            not isinstance(value, MotionParticipantRequest)
            or value.contact_policy_id is not None
            or bool(value.runtime_object_bindings)
            for value in motions
        ):
            raise ValueError(
                "place motion phases cannot carry contact-policy state"
            )
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
