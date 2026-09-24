"""Strict public request DTOs for the H0 HTTP projection."""

from __future__ import annotations

import json
import unicodedata
from typing import Annotated, Any, Literal

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    WithJsonSchema,
    field_validator,
    field_serializer,
    model_validator,
    model_serializer,
)

from manyforge_behavior.json_scalars import (
    MAXIMUM_INT64,
    MAXIMUM_SAFE_INTEGER,
    nonnegative_int64,
    nonnegative_int64_value,
)


MAX_IDENTITY_BYTES = 256
MAX_ACTIVATION_ID_BYTES = 2_048
MAX_REASON_BYTES = 4096
MAX_BULK_MEMBERS = 256
MAX_SCENE_POSE_QUERY_RESOURCES = 128
MAX_PROGRAM_RUN_REASON_BYTES = 4096
MAX_STOP_IDENTITY_BYTES = 128
MAX_STOP_REASON_BYTES = 1024
MAX_STOP_SELECTOR_TARGETS = 4096
MAX_OPERATION_PARTICIPANTS = 4096
MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES = 2 * 1024 * 1024
SHA256_PATTERN = r"^sha256:[0-9a-f]{64}$"


def _bounded_nfc(value: str, *, label: str, maximum_bytes: int) -> str:
    if not value or unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"{label} must be non-empty NFC text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{label} exceeds {maximum_bytes} UTF-8 bytes")
    return value


def decode_runtime_scale_selector_query(value: str) -> Any:
    """Decode the grouped selector query using the public JSON strictness rules."""
    if len(value.encode("utf-8")) > MAX_RUNTIME_SCALE_SELECTOR_JSON_BYTES:
        raise ValueError(
            "group_targets exceeds the runtime-scale selector byte bound"
        )

    def reject_constant(token: str) -> None:
        raise ValueError(f"non-finite JSON number {token} is forbidden")

    def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = item
        return result

    try:
        return json.loads(
            value,
            parse_constant=reject_constant,
            object_pairs_hook=reject_duplicate_keys,
        )
    except RecursionError as exc:
        raise ValueError("group_targets JSON nesting is too deep") from exc


class StrictRequest(BaseModel):
    """Forbid coercion and caller-invented fields at public mutation ingress."""

    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)


class TargetObservationQuery(StrictRequest):
    """One exact, effect-free read of a captured Program target."""

    schema_version: Literal["manyforge.target_observation_query.v1"]
    owner_epoch: str
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    target_role_id: str
    kind: Literal["frame_pose", "gripper_status", "gripper_confirmation"]
    frame_role_id: str | None = None
    relative_to_frame_role_id: str | None = None

    @field_validator(
        "owner_epoch", "target_role_id", "frame_role_id", "relative_to_frame_role_id"
    )
    @classmethod
    def validate_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )

    @model_validator(mode="after")
    def exact_query(self) -> "TargetObservationQuery":
        frame_query = self.kind == "frame_pose"
        if frame_query != (self.frame_role_id is not None):
            raise ValueError("only a frame-pose query names a frame role")
        if not frame_query and self.relative_to_frame_role_id is not None:
            raise ValueError("only a frame-pose query names a relative frame role")
        return self


class SkillExpansionRequest(StrictRequest):
    """Effect-free authoring expansion against one selected build environment."""

    schema_version: Literal["manyforge.skill_expansion_request.v1"]
    expected_environment_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    skill_id: str = Field(pattern=r"^[a-z][a-z0-9_.-]{0,127}$")
    instance_prefix: str = Field(pattern=r"^[a-z][a-z0-9_.-]{0,127}$")
    arguments: dict[str, Any] = Field(max_length=32)
    component_roles: dict[str, str] = Field(max_length=32)
    target_roles: dict[str, str] = Field(max_length=32)
    program_context: dict[str, Any] = Field(
        description="Portable Program declarations without tree/revision or Composer forest/layout fields."
    )


class ProgramOperationQuery(StrictRequest):
    """Select one resident operation from exact backend-owned Program scope."""

    schema_version: Literal["manyforge.program_operation_query.v1"]
    owner_epoch: str
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program_run_id: str
    target_role_id: str

    @field_validator("owner_epoch", "program_run_id", "target_role_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )


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


class GripperConfirmationRequest(StrictRequest):
    """Operator input, never a caller-selected source policy or timestamp."""

    schema_version: Literal["manyforge.gripper_confirmation_request.v1"]
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    target_role_id: str
    command_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_status_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    confirmed: bool
    request_context: CallerAttributionRequest

    @field_validator("target_role_id")
    @classmethod
    def bounded_target(cls, value: str) -> str:
        return _bounded_nfc(value, label="target_role_id", maximum_bytes=256)


class GripperGraspEvidenceObservation(StrictRequest):
    """Qualified-source observation, not an inferred grasp or control grant."""

    source_kind: Literal["operator"]
    source_id: str
    source_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    source_epoch: str
    observation_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    attribution: str
    command_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    confirmed: bool
    observed_at_ns: int
    expires_at_ns: int

    @field_validator("source_id", "source_epoch", "attribution")
    @classmethod
    def bounded_identity(cls, value: str, info) -> str:
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=256)

    @field_validator("observed_at_ns", "expires_at_ns", mode="before")
    @classmethod
    def exact_timestamp(cls, value):
        from manyforge_behavior.json_scalars import nonnegative_int64

        return nonnegative_int64(value)

    @model_validator(mode="after")
    def positive_validity(self) -> "GripperGraspEvidenceObservation":
        if not 0 < self.expires_at_ns - self.observed_at_ns <= 3_600_000_000_000:
            raise ValueError("confirmation must have an explicit bounded validity")
        return self


class ExecutionControlAttributionRequest(CallerAttributionRequest):
    """Optional causal identity of one persistent execution-control session."""

    session_id: str | None = None
    client_sequence: str | None = None

    @field_validator("session_id")
    @classmethod
    def validate_session_identity(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="session_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("client_sequence")
    @classmethod
    def validate_client_sequence(cls, value: str | None) -> str | None:
        if value is None:
            return None
        if (
            not value.isascii()
            or not value.isdigit()
            or value.startswith("0")
            or not 1 <= int(value) <= 18_446_744_073_709_551_615
            or str(int(value)) != value
        ):
            raise ValueError("client_sequence must be canonical positive uint64 text")
        return value

    @model_validator(mode="after")
    def paired_control_session(self) -> "ExecutionControlAttributionRequest":
        if (self.session_id is None) != (self.client_sequence is None):
            raise ValueError("session_id and client_sequence must be paired")
        return self


class AuthoringMutationContext(CallerAttributionRequest):
    """Exact owner-issued context carried by one authoring transaction."""

    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    request_payload_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    owner_transaction_id: str

    @field_validator("owner_transaction_id")
    @classmethod
    def validate_transaction_identity(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="owner_transaction_id",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class AuthoringMutationSettlementRequest(StrictRequest):
    """Settle exactly one previously issued authoring claim."""

    claim: dict[str, Any]
    settlement_kind: Literal["owner_commit", "proven_no_effect"]
    evidence_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]


class AuthoringMutationRecoveryRequest(StrictRequest):
    """Private Composer-owner lookup for one exact durable claim intent."""

    intent: dict[str, Any]
    expected_claim: dict[str, Any] | None
    expected_release: dict[str, Any] | None


class AuthoringOwnerReconciliationRequest(StrictRequest):
    """Rotate only the Composer authoring-owner epoch after local recovery."""

    kernel_owner_epoch: str
    retained_claims: list[dict[str, Any]]
    current_owner_epochs: list[dict[str, Any]]
    reconciliation_evidence_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]


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


class ManagerDeploymentCandidateReceipt(StrictRequest):
    schema_version: Literal["manyforge.manager_deployment_candidate_receipt.v1"]
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    prepared_activation_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    candidate_retained_bundle_id: str
    candidate_graph_revision: str
    expected_current: ExpectedCurrentRequest
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    manager_owner_epoch: str
    receipt_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator(
        "deployment_artifact_id",
        "candidate_retained_bundle_id",
        "candidate_graph_revision",
        "manager_owner_epoch",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ManagerDeploymentCandidatePreviewRequest(StrictRequest):
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    candidate_retained_bundle_id: str
    candidate_graph_revision: str
    prepared_activation_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    expected_current: ExpectedCurrentRequest
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    request_owner_epoch: str

    @field_validator(
        "deployment_artifact_id",
        "candidate_retained_bundle_id",
        "candidate_graph_revision",
        "request_owner_epoch",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ActivationPreparationRequest(StrictRequest):
    source_document_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    scenario_id: str
    captured_input_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    deployment: dict[str, Any]
    model_source_closure: dict[str, Any]
    program: dict[str, Any]
    program_role_assignment: dict[str, Any]
    typed_parameter_overrides: dict[str, Any]
    expected_admission_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]

    @field_validator("scenario_id")
    @classmethod
    def validate_scenario_id(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="scenario_id", maximum_bytes=MAX_IDENTITY_BYTES
        )

    @model_validator(mode="after")
    def require_complete_input(self) -> "ActivationPreparationRequest":
        if (
            not self.deployment
            or not self.model_source_closure
            or not self.program
            or not self.program_role_assignment
        ):
            raise ValueError(
                "deployment, model_source_closure, program, and "
                "program_role_assignment must be non-empty objects"
            )
        return self


class ManagerApplyPreparedActivation(StrictRequest):
    """Exact UA2A prepared activation accepted by one named Apply.

    Every member is captured once before acceptance. The Manager records these
    facts in its durable Apply intent and never reopens a mutable
    Scenario/catalog selection, draft or current-selection fact to reconstruct
    the operation input. The bounded prepared runtime input itself stays with
    its existing owner: ``prepared_activation_id`` is that owner's exact
    transfer handle and binds the whole prepared value.
    """

    captured_input_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    schema_version: Literal["manyforge.prepared_activation_input.v2"]
    scenario_id: str
    source_document_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    retained_bundle_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    graph_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program_id: str
    program_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    node_environment_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    assistant_mode: Literal["required", "optional", "forbidden"]
    typed_parameter_overrides: dict[str, Any]
    prepared_activation_id: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("deployment_artifact_id", "scenario_id", "program_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
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
    prepared_activation: ManagerApplyPreparedActivation
    expected_admission_generation: Annotated[
        int | None, Field(ge=1, le=9_007_199_254_740_991)
    ] = None
    parent_authoring_claim: dict[str, Any] | None = None
    candidate_receipt: ManagerDeploymentCandidateReceipt

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
        if (self.expected_admission_generation is None) == (
            self.parent_authoring_claim is None
        ):
            raise ValueError(
                "exactly one direct admission generation or parent authoring "
                "claim is required"
            )
        expected_generation = self.expected_admission_generation
        if expected_generation is not None and (
            self.candidate_receipt.expected_admission_generation != expected_generation
        ):
            raise ValueError("candidate receipt admission generation differs")
        if (
            self.candidate_receipt.deployment_artifact_id != self.deployment_artifact_id
            or self.candidate_receipt.deployment_revision != self.deployment_revision
            or self.candidate_receipt.expected_current != self.expected_current
        ):
            raise ValueError("candidate receipt does not match Apply selection")
        if (
            self.prepared_activation.deployment_artifact_id
            != self.deployment_artifact_id
            or self.prepared_activation.deployment_revision
            != self.deployment_revision
            or self.candidate_receipt.prepared_activation_id
            != self.prepared_activation.prepared_activation_id
        ):
            raise ValueError(
                "prepared activation does not match the Apply deployment selection"
            )
        return self


class ProgramRunStartRequest(ExecutionControlAttributionRequest):
    """Start exactly the Applied Program selected by these immutable facts."""

    scenario_id: Annotated[
        str,
        Field(min_length=1, max_length=128, pattern=r"^[a-z][a-z0-9_.-]*$"),
    ]
    source_document_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    expected_applied_record_digest: Annotated[
        str,
        Field(pattern=SHA256_PATTERN),
    ]
    draft_program_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]


class ProgramScenePreparationRequest(CallerAttributionRequest):
    """Prepare the exact current Applied Program scene while the cycle is Idle."""

    expected_applied_record_digest: Annotated[
        str,
        Field(pattern=SHA256_PATTERN),
    ]
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]


class ProgramRunStopRequest(ExecutionControlAttributionRequest):
    """Request Kernel-first Stop for one exact runner-owned Program run."""

    program_run_id: str
    expected_owner_epoch: str
    reason: str | None = None

    @field_validator("program_run_id", "expected_owner_epoch")
    @classmethod
    def validate_run_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("reason")
    @classmethod
    def validate_program_stop_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="reason",
            maximum_bytes=MAX_PROGRAM_RUN_REASON_BYTES,
        )


class ProgramSelectionStopRequest(ProgramRunStopRequest):
    """Stop immutable Program membership, never caller-selected operations."""

    group_ids: Annotated[
        list[
            Annotated[
                str, Field(pattern=r"^[a-z][a-z0-9_.-]*$", min_length=1, max_length=128)
            ]
        ],
        Field(min_length=1, max_length=4096),
    ] = Field(default_factory=lambda: ["all"])


class ProgramSelectionResumeRequest(ProgramSelectionStopRequest):
    """Reassess and release one selection of the exact current Program run."""

    expected_run_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_selection_revision: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]


class ProgramSelectionStartRequest(ProgramSelectionResumeRequest):
    """Release one initially Ready selection of the current Program run."""


class ProgramCycleResetRequest(ExecutionControlAttributionRequest):
    program_run_id: str
    expected_owner_epoch: str
    expected_cycle_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_claim_generation: (
        Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None
    ) = None
    terminal_reference: Annotated[str, Field(pattern=SHA256_PATTERN)] | None = None
    settlement_evidence_digest: Annotated[str, Field(pattern=SHA256_PATTERN)] | None = (
        None
    )

    @field_validator("program_run_id", "expected_owner_epoch")
    @classmethod
    def validate_cycle_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )


class ProgramRunControlRequest(ExecutionControlAttributionRequest):
    """Common exact fence for one current-run-only control."""

    program_run_id: str
    expected_owner_epoch: str
    expected_run_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]

    @field_validator("program_run_id", "expected_owner_epoch")
    @classmethod
    def validate_runtime_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )


class ProgramFullStopResumeRequest(ProgramRunControlRequest):
    """Resume one exact tree preserved by Program Full Stop."""

    reason: str | None = None

    @field_validator("reason")
    @classmethod
    def validate_resume_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="reason",
            maximum_bytes=MAX_PROGRAM_RUN_REASON_BYTES,
        )


class ProgramFullStopResetRequest(ProgramFullStopResumeRequest):
    """Discard one exact Full-stopped tree and reset its Program cycle."""

    expected_cycle_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_claim_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]


class RuntimeOverrideStageRequest(ProgramRunControlRequest):
    node_instance_id: str
    node_attempt_id: str
    parameter_id: str
    value: Any
    source_observation_id: str | None = None
    source_owner_generation: (
        Annotated[int, Field(ge=1, le=9_007_199_254_740_991)] | None
    ) = None

    @field_validator(
        "node_instance_id",
        "node_attempt_id",
        "parameter_id",
        "source_observation_id",
    )
    @classmethod
    def validate_override_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )

    @model_validator(mode="after")
    def exact_source(self) -> "RuntimeOverrideStageRequest":
        if (self.source_observation_id is None) != (
            self.source_owner_generation is None
        ):
            raise ValueError(
                "runtime override source identity and generation are paired"
            )
        return self


class RuntimeOverrideActionRequest(ProgramRunControlRequest):
    override_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    expected_override_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]


class RuntimeOverridePromotionRequest(RuntimeOverrideActionRequest):
    expected_draft_id: str
    expected_draft_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]

    @field_validator("expected_draft_id")
    @classmethod
    def validate_draft_identity(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="expected_draft_id", maximum_bytes=MAX_IDENTITY_BYTES
        )


class RuntimeBlackboardWriteRequest(ProgramRunControlRequest):
    key: str
    value: Any

    @field_validator("key")
    @classmethod
    def validate_key(cls, value: str) -> str:
        return _bounded_nfc(value, label="key", maximum_bytes=MAX_IDENTITY_BYTES)


class ManualResultStageRequest(ProgramRunControlRequest):
    node_instance_id: str
    node_attempt_id: str
    outcome: Literal["succeeded", "failed"]
    output: Any = None

    @field_validator("node_instance_id", "node_attempt_id")
    @classmethod
    def validate_attempt_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )


class CycleRecoveryActionRequest(ExecutionControlAttributionRequest):
    """Consume exactly one current owner-issued cycle recovery offer."""

    expected_owner_epoch: str
    offer_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    offer_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    action_id: Literal[
        "retry_motion_from_current_state",
        "acknowledge_and_continue",
        "retry_after_scene_repair",
        "replan_from_stopped_state",
        "retry_runtime_step",
        "reset_cycle",
    ]
    reason: str | None = None

    @field_validator("expected_owner_epoch")
    @classmethod
    def validate_recovery_owner(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="expected_owner_epoch", maximum_bytes=MAX_IDENTITY_BYTES
        )

    @field_validator("reason")
    @classmethod
    def validate_recovery_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_STOP_REASON_BYTES)


class ProgramModelAuditPolicy(StrictRequest):
    """Exact retained-data policy captured before the model attempt."""

    redact_argument_values: bool
    store_full_prompts: bool
    store_full_responses: bool


class ProgramDiagnosticRequest(StrictRequest):
    """Asynchronous attributed evidence; never a command or recovered state."""

    schema_version: Literal["manyforge.program_diagnostic.v1"]
    program_run_id: str
    owner_epoch: str
    program_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    retained_bundle_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    graph_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    state: Literal["succeeded", "failed", "held", "intervention_required"]
    reason_code: str | None
    message: Annotated[str, Field(max_length=4096)]
    occurred_at_ns: Annotated[str, Field(pattern=r"^(0|[1-9][0-9]{0,18})$")]
    occurred_at_wall: Annotated[str, Field(max_length=64)]
    blackboard: Annotated[dict[str, Any], Field(max_length=32)]
    runtime_overrides: Annotated[dict[str, Any], Field(max_length=32)]
    omitted_blackboard_count: Annotated[int, Field(ge=0, le=100_000)]
    omitted_override_count: Annotated[int, Field(ge=0, le=100_000)]
    delivery_loss_count: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]

    @field_validator("program_run_id", "owner_epoch", "reason_code")
    @classmethod
    def bounded_identity(cls, value, info):
        return (
            None
            if value is None
            else _bounded_nfc(value, label=info.field_name, maximum_bytes=512)
        )

    @field_validator("occurred_at_ns")
    @classmethod
    def exact_clock(cls, value):
        nonnegative_int64(value)
        return value

    @model_validator(mode="after")
    def bounded_portable_record(self):
        from manyforge_behavior.canonical_artifact import canonical_json_bytes
        from manyforge_behavior.program_diagnostics import diagnostic_values

        if len(canonical_json_bytes(self.model_dump(mode="python"))) > 64 * 1024:
            raise ValueError("Program diagnostic exceeds its byte bound")
        for values in (self.blackboard, self.runtime_overrides):
            if diagnostic_values(values.items(), total_count=len(values))[
                "omitted_count"
            ]:
                raise ValueError("Program diagnostic values exceed capture bounds")
        return self


class ProgramModelAttemptRequest(StrictRequest):
    """Closed durable audit record emitted by the isolated Program runner."""

    schema_version: Literal["manyforge.program_model_attempt.v1"]
    model_attempt_id: str
    program_run_id: str
    node_instance_id: str
    node_attempt_id: str
    external_attempt_index: Annotated[int, Field(ge=1, le=2)]
    assistant_projection_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    model_endpoint_alias: str
    external_resource_alias: str
    model_id: str
    prompt_template_id: str
    prompt_template_version: str
    prompt_template_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    prompt_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    argument_digests: Annotated[
        dict[str, Annotated[str, Field(pattern=SHA256_PATTERN)]],
        Field(max_length=256),
    ]
    attachment_digest: Annotated[
        str | None,
        Field(pattern=SHA256_PATTERN),
    ]
    audit_policy: ProgramModelAuditPolicy
    prompt_text: str | None
    argument_values: dict[str, Any] | None
    outcome: Literal[
        "success",
        "timeout",
        "parse_error",
        "schema_error",
        "confidence_below_threshold",
        "cancelled",
        "transport_error",
        "invalid_response",
    ]
    error_code: str | None
    error_message: str
    response_digest: Annotated[
        str | None,
        Field(pattern=SHA256_PATTERN),
    ]
    validated_output_digest: Annotated[
        str | None,
        Field(pattern=SHA256_PATTERN),
    ]
    model_version: str
    occurred_at_ns: Annotated[
        int,
        Field(ge=0, le=MAXIMUM_INT64),
        WithJsonSchema(
            {
                "anyOf": [
                    {"type": "integer", "minimum": 0, "maximum": MAXIMUM_SAFE_INTEGER},
                    {
                        "type": "string",
                        "pattern": r"^(0|[1-9][0-9]{0,18})$",
                        "maxLength": 19,
                        "format": "nonnegative-int64-decimal",
                        "description": "Canonical decimal int64 in [0, 9223372036854775807].",
                    },
                ],
            }
        ),
    ]
    occurred_at_wall: str
    latency_ms: Annotated[
        float,
        Field(ge=0, le=3_600_000, allow_inf_nan=False),
    ]
    usage: dict[str, Any] | None
    response_value: Any | None

    @field_validator("occurred_at_ns", mode="before")
    @classmethod
    def exact_occurrence_time(cls, value: Any) -> int:
        return nonnegative_int64(value)

    @field_serializer("occurred_at_ns")
    def encode_occurrence_time(self, value: int) -> int | str:
        return nonnegative_int64_value(value)

    @field_validator(
        "model_attempt_id",
        "program_run_id",
        "node_instance_id",
        "node_attempt_id",
        "model_endpoint_alias",
        "external_resource_alias",
        "model_id",
        "prompt_template_id",
        "prompt_template_version",
    )
    @classmethod
    def validate_model_attempt_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator("error_code")
    @classmethod
    def validate_model_attempt_error_code(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="error_code",
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @field_validator(
        "prompt_text", "error_message", "model_version", "occurred_at_wall"
    )
    @classmethod
    def validate_model_attempt_text(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        maximum = {
            "prompt_text": 8 * 1024 * 1024,
            "occurred_at_wall": 128,
        }.get(info.field_name, 4096)
        if info.field_name == "occurred_at_wall" and not value:
            raise ValueError("occurred_at_wall must be non-empty")
        if unicodedata.normalize("NFC", value) != value:
            raise ValueError(f"{info.field_name} must be NFC text")
        if len(value.encode("utf-8")) > maximum:
            raise ValueError(f"{info.field_name} exceeds its UTF-8 byte bound")
        return value


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


ManagerApplyPhase = Literal[
    "accepted",
    "fence_acquired",
    "fence_accepted",
    "runtime_converged",
    "scene_staged",
    "scene_activated",
    "composed",
    "published",
    "settled",
]


class ManagerApplyProgress(StrictRequest):
    """Last real progress: the deepest durably reached milestone and its exact
    evidence digest. Never a heartbeat or socket responsiveness."""

    phase: ManagerApplyPhase
    record_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]


MAX_APPLY_FAILURE_CODE_BYTES = 128
MAX_APPLY_FAILURE_MESSAGE_BYTES = 1024

ManagerApplyTerminalState = Literal[
    "pending",
    "succeeded",
    "failed",
    "unknown_effect",
]


class ManagerApplyFailure(StrictRequest):
    """The first meaningful failure retained by one terminal Apply operation.

    It is reported once and is never repaired, retried, rolled back or assumed
    to have had no effect. The effect disposition states whether every effect of
    the operation is exactly established; an unknown disposition is never
    presented as if the failed work left nothing behind.
    """

    code: str
    message: str
    phase: ManagerApplyPhase
    effect_disposition: Literal["known", "unknown"]

    @field_validator("code")
    @classmethod
    def validate_code(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="code",
            maximum_bytes=MAX_APPLY_FAILURE_CODE_BYTES,
        )

    @field_validator("message")
    @classmethod
    def validate_message(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="message",
            maximum_bytes=MAX_APPLY_FAILURE_MESSAGE_BYTES,
        )


class ManagerApplyStatusPreparedActivation(StrictRequest):
    """The compact immutable activation identity published by Manager status."""

    captured_input_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    program_binding_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    prepared_activation_id: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("deployment_artifact_id")
    @classmethod
    def validate_identity(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="deployment_artifact_id", maximum_bytes=MAX_IDENTITY_BYTES
        )


class ManagerApplyStatus(StrictRequest):
    """One bounded lifecycle status for one durable Manager Apply operation.

    Shared by Apply, Revert and activating a loaded Scenario, because all three
    submit the same operation owner. Every member is a durable fact: the
    operation identity (Apply ID/generation plus the caller request owner/id),
    the accepted UA2A prepared activation, the deepest durably reached phase
    with its evidence digest, and exactly one terminal outcome: pending while
    the operation remains resumable, otherwise succeeded, failed or
    unknown_effect.
    """

    apply_id: str
    apply_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    request_id: str
    request_owner_epoch: str
    prepared_activation: ManagerApplyStatusPreparedActivation
    phase: ManagerApplyPhase
    last_progress: ManagerApplyProgress
    state: ManagerApplyTerminalState
    candidate_projection_revision: ProjectionRevisionRequest | None = None
    failure: ManagerApplyFailure | None = None

    @field_validator("apply_id", "request_id", "request_owner_epoch")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def require_one_consistent_lifecycle(self) -> "ManagerApplyStatus":
        if self.last_progress.phase != self.phase:
            raise ValueError(
                "last progress must describe the reported operation phase"
            )
        if (self.state == "succeeded") != (
            self.candidate_projection_revision is not None
        ):
            raise ValueError(
                "only a terminal success carries its candidate projection "
                "revision"
            )
        if (self.state in ("failed", "unknown_effect")) != (
            self.failure is not None
        ):
            raise ValueError(
                "a terminal failure, and only a terminal failure, carries its "
                "retained first failure"
            )
        if self.state == "succeeded" and self.phase != "settled":
            raise ValueError("a successful operation must be settled")
        if self.failure is not None:
            if self.failure.phase != self.phase:
                raise ValueError(
                    "the retained failure must describe the reported operation "
                    "phase"
                )
            # A known failure and an unknown effect are distinct dispositions:
            # neither may be presented as the other, and a failure never implies
            # that the failed work left nothing behind.
            required_disposition = (
                "known" if self.state == "failed" else "unknown"
            )
            if self.failure.effect_disposition != required_disposition:
                raise ValueError(
                    "the retained failure disposition must match the terminal "
                    "state"
                )
        return self


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
            raise ValueError("scene_resource_ids must be strictly sorted and unique")
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

    @field_validator("controllable_component_id", "model_instance_id", "frame_role_id")
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
    CellSceneFrameRequest | ComponentSceneFrameRequest | PassiveModelSceneFrameRequest,
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
    mode: Literal["contact_release", "bounded_penetration", "contact_process"] = (
        "contact_release"
    )
    release_evidence_mode: Literal["signed_penetration", "binary_contact"] = (
        "signed_penetration"
    )
    enforce_max_start_penetration: bool = True
    max_start_penetration_m: Annotated[float, Field(ge=0, allow_inf_nan=False)] = 0.0
    release_tolerance_m: Annotated[float, Field(ge=0, allow_inf_nan=False)] = 1e-5
    min_release_progress_m: Annotated[float, Field(ge=0, allow_inf_nan=False)] = 0.0
    max_contact_penetration_m: Annotated[float, Field(ge=0, allow_inf_nan=False)] = 0.0
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
            normalized.setdefault("require_strictly_decreasing_penetration", False)
            normalized.setdefault("allow_recontact", False)
        elif (
            normalized.get("release_evidence_mode", "signed_penetration")
            == "binary_contact"
        ):
            normalized.setdefault("enforce_max_start_penetration", False)
            normalized.setdefault("max_start_penetration_m", 0.0)
            normalized.setdefault("release_tolerance_m", 0.0)
            normalized.setdefault("min_release_progress_m", 0.0)
            normalized.setdefault("require_strictly_decreasing_penetration", False)
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
                    "max_contact_penetration_m is valid only for " "bounded_penetration"
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
                "release_evidence_mode is not applicable outside " "contact_release"
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
        if self.mode == "contact_process" and self.max_contact_penetration_m != 0.0:
            raise ValueError(
                "max_contact_penetration_m is valid only for " "bounded_penetration"
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
    contact_grasps: Annotated[list[ContactGraspPolicyRequest], Field(max_length=4096)]
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

    @field_validator("touch_body_roles", "expected_component_attachment_revisions")
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


class ProgramResetSceneAuthorizationRequest(StrictRequest):
    """Internal exact scope for one backend-owned Reset scene mutation."""

    program_runner_owner_epoch: str
    program_run_id: str
    cycle_claim_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    terminal_or_settlement_reference: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("program_runner_owner_epoch", "program_run_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ProgramNodeAdmissionEvidenceRequest(StrictRequest):
    """Exact runtime-node attribution; the Kernel checks the sealed owner API."""

    program_runner_owner_epoch: str
    program_run_id: str
    parent_program_cycle_claim_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    node_instance_id: str
    node_attempt_id: str
    owner_api: Literal[
        "scene.transaction",
        "scene.carried_attachment",
        "cclc.lifecycle",
        "cclc.control_route",
    ]

    @field_validator(
        "program_runner_owner_epoch",
        "program_run_id",
        "node_instance_id",
        "node_attempt_id",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=(
                512 if info.field_name == "node_attempt_id" else MAX_IDENTITY_BYTES
            ),
        )


class SceneTransactionRequest(StrictRequest):
    request_context: CallerAttributionRequest
    expected_projection_revision: ProjectionRevisionRequest
    operation: SceneOperationRequest
    expected_admission_generation: Annotated[
        int | None, Field(ge=1, le=9_007_199_254_740_991)
    ] = None
    program_reset_scene_authorization: ProgramResetSceneAuthorizationRequest | None = (
        None
    )
    program_node_evidence: ProgramNodeAdmissionEvidenceRequest | None = None


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
    velocity_scale: Annotated[float, Field(gt=0, le=1, allow_inf_nan=False)]
    acceleration_scale: Annotated[float, Field(gt=0, le=1, allow_inf_nan=False)]
    max_tcp_linear_speed_mps: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None


class MotionTimeoutsRequest(StrictRequest):
    planning_timeout_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    execution_timeout_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]


class JointDynamicsLimitRequest(StrictRequest):
    max_velocity_rad_s: Annotated[float | None, Field(gt=0, allow_inf_nan=False)] = None
    max_acceleration_rad_s2: Annotated[
        float | None, Field(gt=0, allow_inf_nan=False)
    ] = None
    max_jerk_rad_s3: Annotated[float | None, Field(gt=0, allow_inf_nan=False)] = None


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
    max_translation_step_m: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    max_rotation_step_rad: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    speed_mode: Literal["bounded"]
    max_angular_speed_rad_s: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    max_linear_acceleration_mps2: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    max_angular_acceleration_rad_s2: Annotated[float, Field(gt=0, allow_inf_nan=False)]
    fallback_policy: Literal["fail_closed", "allow_joint_space_fallback"]
    max_blend_deviation_rad: Annotated[
        float, Field(ge=0, allow_inf_nan=False)
    ] = 0.01


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


class GripperCommandObservation(StrictRequest):
    state: Literal["idle", "executing", "completed", "failed"]
    command_id: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]
    generation: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]
    revision: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]
    active_control_configuration_id: str
    active_control_configuration_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    source_id: str
    source_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    source_epoch: str

    @field_validator("active_control_configuration_id", "source_id", "source_epoch")
    @classmethod
    def bounded_identity(cls, value: str, info) -> str:
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=256)


class GripperFeedbackObservation(GripperCommandObservation):
    observed_position: CoordinatePositionRequest
    grasp_confirmed: bool | None


class GripperConfirmationStatusObservation(GripperCommandObservation):
    command_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    grasp_confirmed: bool | None
    evidence: GripperGraspEvidenceObservation | None

    @model_validator(mode="after")
    def coherent_evidence(self) -> "GripperConfirmationStatusObservation":
        if self.evidence is None:
            if self.grasp_confirmed is not None:
                raise ValueError("grasp confirmation requires evidence")
            return self
        if (
            self.evidence.command_id != self.command_id
            or self.evidence.generation != self.generation
            or self.evidence.source_id != self.source_id
            or self.evidence.source_revision != self.source_revision
            or self.evidence.source_epoch != self.source_epoch
            or self.state != "completed"
            or (
                self.grasp_confirmed is not None
                and self.grasp_confirmed is not self.evidence.confirmed
            )
        ):
            raise ValueError("grasp evidence crosses its current command or source")
        return self


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
    caller_options: Annotated[list[CommandDurationOptionRequest], Field(max_length=64)]

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

    @field_validator("component_role_id", "target_role_id", "required_capability_id")
    @classmethod
    def validate_participant_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )


class ManualProgramHandoverContext(StrictRequest):
    """Kernel-held Program coordinates, not a client-issued permission."""

    program_runner_owner_epoch: str
    program_run_id: str
    claim_generation: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    selection_revision: Annotated[int, Field(ge=0, le=MAXIMUM_SAFE_INTEGER)]

    @field_validator("program_runner_owner_epoch", "program_run_id")
    @classmethod
    def bounded_identity(cls, value: str, info) -> str:
        if "\0" in value:
            raise ValueError("manual handover identity contains NUL")
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=128)


class ManualControlAvailability(StrictRequest):
    """Current target eligibility; execution still performs atomic admission."""

    available: bool
    unavailable_reason: str | None
    cleanup_required: bool

    @model_validator(mode="after")
    def exact_reason(self) -> "ManualControlAvailability":
        if self.available == (self.unavailable_reason is not None):
            raise ValueError("manual availability must have an exact disabled reason")
        if self.unavailable_reason is not None:
            _bounded_nfc(
                self.unavailable_reason, label="unavailable_reason", maximum_bytes=1024
            )
        if self.cleanup_required and not self.available:
            raise ValueError(
                "unavailable manual target cannot request ordinary cleanup"
            )
        return self


class ManualProgramHandover(StrictRequest):
    """Read-only cleanup input derived from the exact Kernel capture."""

    context: ManualProgramHandoverContext
    producer_node_indices: Annotated[
        list[Annotated[int, Field(ge=0, lt=100_000)]], Field(max_length=100_000)
    ]

    @field_validator("producer_node_indices")
    @classmethod
    def canonical_indices(cls, values: list[int]) -> list[int]:
        if any(left >= right for left, right in zip(values, values[1:])):
            raise ValueError("manual producer indices must be sorted and unique")
        return values


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
        list[OperationParticipantRequest],
        Field(min_length=1, max_length=MAX_OPERATION_PARTICIPANTS),
    ]
    coordination_policy: Literal["independent", "coordinated_all"]
    manual_handover: ManualProgramHandoverContext | None = None

    @model_serializer(mode="wrap")
    def serialize_operation(self, handler) -> dict[str, Any]:
        result = handler(self)
        if self.manual_handover is None:
            result.pop("manual_handover", None)
        return result

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
    def validate_operation_scope(self) -> "OperationIntentRequest":
        if (len(self.participants) == 1) != (self.coordination_policy == "independent"):
            raise ValueError("participant count must match the coordination policy")
        if self.manual_handover is not None and (
            self.coordination_policy != "independent"
            or self.manual_handover.program_run_id == self.program_run_id
            or any(
                getattr(row, "execution_scope", None) is not None
                for row in self.participants
            )
        ):
            raise ValueError(
                "manual handover requires independent non-node work with its own Run"
            )
        for participant in self.participants:
            span = participant.action_span
            if (
                span is not None
                and span.program_run_id is not None
                and span.program_run_id != self.program_run_id
            ):
                raise ValueError("action span program_run_id must match the operation")
        return self


class ProgramManualHandoverRequest(ProgramRunStopRequest):
    """Retire Kernel-selected held tasks, without releasing their holds."""

    expected_run_revision: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    operation: OperationIntentRequest

    @model_validator(mode="after")
    def exact_parent(self) -> "ProgramManualHandoverRequest":
        context = self.operation.manual_handover
        if (
            context is None
            or context.program_run_id != self.program_run_id
            or context.program_runner_owner_epoch != self.expected_owner_epoch
        ):
            raise ValueError(
                "manual cleanup must name the operation's exact held Program"
            )
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
    if not isinstance(
        tool.participants[0].payload, CapabilityCommandParticipantRequest
    ):
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
            raise ValueError("place motion phases cannot carry contact-policy state")
        return self


class OperationControlRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operation_id: str
    expected_operation_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
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


class OperationRecoveryActionRequest(StrictRequest):
    """Private Program-Runner request to one exact native operation owner."""

    request_id: str
    action: Literal[
        "retry_motion_from_current_state",
        "acknowledge_and_continue",
        "retry_after_scene_repair",
        "replan_from_stopped_state",
        "reset_cycle",
    ]
    principal_id: str = ""
    expected_assessment_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_control_command_epoch: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_991)
    ]
    expected_execution_id: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_latch_id: str = ""
    expected_stop_certificate_id: str = ""
    expected_stop_owner_generation: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_991)
    ] = 0
    reason: str = ""

    @field_validator(
        "request_id",
        "principal_id",
        "expected_latch_id",
        "expected_stop_certificate_id",
        "reason",
    )
    @classmethod
    def validate_text(cls, value: str, info) -> str:
        if info.field_name == "request_id" and not value:
            raise ValueError("request_id must be non-empty")
        if not value:
            return value
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=(
                MAX_REASON_BYTES if info.field_name == "reason" else MAX_IDENTITY_BYTES
            ),
        )


class StopOperationTargetRequest(StrictRequest):
    operation_id: str
    expected_operation_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_stop_generation: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]

    @field_validator("operation_id")
    @classmethod
    def validate_operation_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="operation_id",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )


class StopComponentTargetRequest(StrictRequest):
    controllable_component_id: str
    expected_graph_revision: str
    expected_component_lifecycle_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]

    @field_validator("controllable_component_id", "expected_graph_revision")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )


class StopResourceTargetRequest(StrictRequest):
    resource_id: str
    expected_graph_revision: str
    expected_operation_resource_generation: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_991)
    ]
    expected_stop_generation: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]

    @field_validator("resource_id", "expected_graph_revision")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )


class StopIntentRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operations: (
        Annotated[
            list[StopOperationTargetRequest],
            Field(min_length=1, max_length=MAX_STOP_SELECTOR_TARGETS),
        ]
        | None
    ) = None
    components: (
        Annotated[
            list[StopComponentTargetRequest],
            Field(min_length=1, max_length=MAX_STOP_SELECTOR_TARGETS),
        ]
        | None
    ) = None
    resources: (
        Annotated[
            list[StopResourceTargetRequest],
            Field(min_length=1, max_length=MAX_STOP_SELECTOR_TARGETS),
        ]
        | None
    ) = None
    reason: str | None = None

    @field_validator("reason", mode="before")
    @classmethod
    def normalize_reason(cls, value: object) -> str | None:
        if value is None:
            return None
        if (
            not isinstance(value, str)
            or not value
            or unicodedata.normalize("NFC", value) != value
            or len(value.encode("utf-8")) > MAX_STOP_REASON_BYTES
        ):
            return None
        return value

    @model_validator(mode="after")
    def validate_selector(self) -> StopIntentRequest:
        selected = [
            rows
            for rows in (self.operations, self.components, self.resources)
            if rows is not None
        ]
        if len(selected) != 1:
            raise ValueError("Stop request must contain exactly one selector kind")
        rows = selected[0]
        identities = [
            getattr(row, "operation_id", None)
            or getattr(row, "controllable_component_id", None)
            or getattr(row, "resource_id", None)
            for row in rows
        ]
        if identities != sorted(identities) or len(set(identities)) != len(identities):
            raise ValueError("Stop selector rows must be sorted and unique")
        context = self.request_context
        for label, value, maximum in (
            ("request_id", context.request_id, MAX_STOP_IDENTITY_BYTES),
            (
                "request_owner_epoch",
                context.request_owner_epoch,
                MAX_STOP_IDENTITY_BYTES,
            ),
            ("source_label", context.source_label, MAX_IDENTITY_BYTES),
        ):
            _bounded_nfc(value, label=label, maximum_bytes=maximum)
        if context.correlation_id is not None:
            _bounded_nfc(
                context.correlation_id,
                label="correlation_id",
                maximum_bytes=MAX_IDENTITY_BYTES,
            )
        return self


class ProgramRunStopIntentRequest(StrictRequest):
    """Private exact Program-owner selector for one addressed Kernel."""

    request_context: CallerAttributionRequest
    program_run_id: str
    reason: str | None = None

    @field_validator("program_run_id")
    @classmethod
    def validate_program_run_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="program_run_id",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )

    @field_validator("reason", mode="before")
    @classmethod
    def normalize_reason(cls, value: object) -> str | None:
        return StopIntentRequest.normalize_reason(value)

    @model_validator(mode="after")
    def validate_context(self) -> ProgramRunStopIntentRequest:
        context = self.request_context
        for label, value, maximum in (
            ("request_id", context.request_id, MAX_STOP_IDENTITY_BYTES),
            (
                "request_owner_epoch",
                context.request_owner_epoch,
                MAX_STOP_IDENTITY_BYTES,
            ),
            ("source_label", context.source_label, MAX_IDENTITY_BYTES),
        ):
            _bounded_nfc(value, label=label, maximum_bytes=maximum)
        return self


class FullStopIntentRequest(StrictRequest):
    request_context: CallerAttributionRequest
    expected_stop_generation: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]

    @model_validator(mode="after")
    def validate_context(self) -> FullStopIntentRequest:
        context = self.request_context
        _bounded_nfc(
            context.request_id,
            label="request_id",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )
        _bounded_nfc(
            context.request_owner_epoch,
            label="request_owner_epoch",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )
        return self


class StopFaultRecoveryConfigurationExpectationRequest(StrictRequest):
    control_domain_id: str
    active_control_configuration_id: str
    active_control_configuration_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]

    @field_validator("control_domain_id", "active_control_configuration_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )


class StopFaultRecoveryIntentRequest(StrictRequest):
    request_context: CallerAttributionRequest
    expected_latch_id: str
    expected_latch_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_recovery_assessment_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_stop_certificate_id: str
    expected_stop_certificate_revision: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_cell_id: str
    expected_retained_bundle_id: str
    expected_graph_revision: str
    configuration_expectations: Annotated[
        list[StopFaultRecoveryConfigurationExpectationRequest],
        Field(min_length=1, max_length=MAX_STOP_SELECTOR_TARGETS),
    ]
    reason: str | None = None

    @field_validator(
        "expected_latch_id",
        "expected_stop_certificate_id",
        "expected_cell_id",
        "expected_retained_bundle_id",
        "expected_graph_revision",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=MAX_STOP_REASON_BYTES)

    @model_validator(mode="after")
    def validate_expectations(self) -> StopFaultRecoveryIntentRequest:
        domains = [row.control_domain_id for row in self.configuration_expectations]
        if domains != sorted(domains) or len(set(domains)) != len(domains):
            raise ValueError(
                "Stop fault-recovery configuration expectations must be sorted and unique"
            )
        _bounded_nfc(
            self.request_context.request_id,
            label="request_id",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )
        _bounded_nfc(
            self.request_context.request_owner_epoch,
            label="request_owner_epoch",
            maximum_bytes=MAX_STOP_IDENTITY_BYTES,
        )
        return self


class RuntimeScaleControlTargetRequest(StrictRequest):
    component_role_id: str
    target_role_id: str

    @field_validator("component_role_id", "target_role_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value, label=info.field_name, maximum_bytes=MAX_IDENTITY_BYTES
        )


class RuntimeScaleControlSelectorRequest(StrictRequest):
    program_binding_id: str
    component_role_id: str | None = None
    target_role_id: str | None = None
    group_targets: Annotated[
        list[RuntimeScaleControlTargetRequest] | None,
        Field(min_length=2, max_length=MAX_OPERATION_PARTICIPANTS),
    ] = None

    @field_validator(
        "program_binding_id",
        "component_role_id",
        "target_role_id",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAX_IDENTITY_BYTES,
        )

    @model_validator(mode="after")
    def validate_form(self) -> "RuntimeScaleControlSelectorRequest":
        singular = self.component_role_id is not None or self.target_role_id is not None
        group = self.group_targets is not None
        if singular == group or (
            singular and (self.component_role_id is None or self.target_role_id is None)
        ):
            raise ValueError(
                "runtime-scale selector must contain exactly one selector form"
            )
        if group:
            targets = [
                (x.component_role_id, x.target_role_id)
                for x in self.group_targets or []
            ]
            if len(set(targets)) != len(targets):
                raise ValueError("group runtime-scale targets must be unique")
        return self

    @model_serializer(mode="wrap")
    def serialize_form(self, handler) -> dict[str, Any]:
        result = handler(self)
        if self.group_targets is None:
            result.pop("group_targets", None)
        else:
            result.pop("component_role_id", None)
            result.pop("target_role_id", None)
        if self.component_role_id is None:
            result.pop("component_role_id", None)
        if self.target_role_id is None:
            result.pop("target_role_id", None)
        return result


class OperationRuntimeScaleRequest(StrictRequest):
    request_context: CallerAttributionRequest
    operation_id: str
    expected_operation_revision: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    expected_runtime_scale_generation: Annotated[
        int, Field(ge=0, le=9_007_199_254_740_990)
    ]
    scale: Annotated[float, Field(gt=0, le=1, allow_inf_nan=False)]

    @field_validator("operation_id")
    @classmethod
    def validate_identity(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="operation_id", maximum_bytes=MAX_IDENTITY_BYTES
        )


class RuntimeScaleControlRequest(StrictRequest):
    request_context: CallerAttributionRequest
    selector: RuntimeScaleControlSelectorRequest
    expected_control_generation: Annotated[int, Field(ge=0, le=9_007_199_254_740_991)]
    scale: Annotated[float, Field(ge=0, le=1, allow_inf_nan=False)]


class HistorianReconcileRequest(CallerAttributionRequest):
    expected_health_generation: Annotated[
        int,
        Field(ge=1, le=9_007_199_254_740_991),
    ]


class CCLCExpectedActiveConfigurationRequest(StrictRequest):
    presence: Literal["absent", "present"]
    active_control_configuration_id: str | None = None
    active_control_configuration_generation: Annotated[
        int | None, Field(ge=1, le=9_007_199_254_740_991)
    ] = None

    @field_validator("active_control_configuration_id")
    @classmethod
    def validate_identifier(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label="active_control_configuration_id",
            maximum_bytes=128,
        )

    @model_validator(mode="after")
    def validate_presence(self) -> "CCLCExpectedActiveConfigurationRequest":
        present = self.active_control_configuration_id is not None
        if present != (self.active_control_configuration_generation is not None):
            raise ValueError(
                "active control configuration identity and generation must be paired"
            )
        if present != (self.presence == "present"):
            raise ValueError(
                "active control configuration fields do not match presence"
            )
        return self


class CCLCControlBindingRequest(StrictRequest):
    control_binding_id: str
    control_binding_revision: str
    control_route_id: str

    @field_validator(
        "control_binding_id", "control_binding_revision", "control_route_id"
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=128,
        )


class CCLCExpectedControlDomainRequest(StrictRequest):
    control_domain_id: str
    control_domain_revision: str
    controller_topology_id: str
    topology_revision: str
    expected_active_control_configuration: CCLCExpectedActiveConfigurationRequest
    expected_control_bindings: Annotated[
        list[CCLCControlBindingRequest], Field(min_length=1, max_length=4_096)
    ]

    @field_validator(
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=128,
        )

    @model_validator(mode="after")
    def validate_binding_order(self) -> "CCLCExpectedControlDomainRequest":
        identifiers = [
            item.control_binding_id for item in self.expected_control_bindings
        ]
        if identifiers != sorted(identifiers) or len(identifiers) != len(
            set(identifiers)
        ):
            raise ValueError(
                "expected_control_bindings must be strictly ordered by binding ID"
            )
        return self


class ComponentLifecycleRequest(StrictRequest):
    request_context: CallerAttributionRequest
    controllable_component_id: str
    intent: Literal["prepare", "enable", "disable", "recover"]
    expected_graph_revision: str
    expected_component_lifecycle_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_control_domains: Annotated[
        list[CCLCExpectedControlDomainRequest],
        Field(min_length=1, max_length=256),
    ]
    reason: str | None = None
    confirmed: bool
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    program_node_evidence: ProgramNodeAdmissionEvidenceRequest | None = None

    @field_validator("controllable_component_id", "expected_graph_revision")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=128,
        )

    @field_validator("reason")
    @classmethod
    def validate_reason(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="reason", maximum_bytes=1_024)

    @model_validator(mode="after")
    def validate_domain_order(self) -> "ComponentLifecycleRequest":
        if (
            self.program_node_evidence
            and self.program_node_evidence.owner_api != "cclc.lifecycle"
        ):
            raise ValueError(
                "lifecycle requests require the exact cclc.lifecycle node API"
            )
        identifiers = [item.control_domain_id for item in self.expected_control_domains]
        if identifiers != sorted(identifiers) or len(identifiers) != len(
            set(identifiers)
        ):
            raise ValueError(
                "expected_control_domains must be strictly ordered by domain ID"
            )
        if (
            sum(
                len(item.expected_control_bindings)
                for item in self.expected_control_domains
            )
            > 4_096
        ):
            raise ValueError("expected control bindings exceed the aggregate bound")
        return self


class CCLCDesiredControlBindingRequest(StrictRequest):
    control_binding_id: str
    control_binding_revision: str
    control_route_id: str

    @field_validator(
        "control_binding_id", "control_binding_revision", "control_route_id"
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=256,
        )


class CCLCDesiredControlConfigurationRequest(StrictRequest):
    control_domain_id: str
    control_domain_revision: str
    controller_topology_id: str
    topology_revision: str
    activation_set_id: str
    binding_routes: Annotated[
        list[CCLCDesiredControlBindingRequest],
        Field(min_length=1, max_length=4_096),
    ]

    @field_validator(
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
        "activation_set_id",
    )
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=256,
        )

    @model_validator(mode="after")
    def validate_binding_order(self) -> "CCLCDesiredControlConfigurationRequest":
        keys = [
            (
                item.control_binding_id,
                item.control_binding_revision,
                item.control_route_id,
            )
            for item in self.binding_routes
        ]
        binding_ids = [item.control_binding_id for item in self.binding_routes]
        if keys != sorted(keys) or len(binding_ids) != len(set(binding_ids)):
            raise ValueError("binding_routes must be canonical with unique binding IDs")
        return self


class CCLCRouteComponentExpectationRequest(StrictRequest):
    controllable_component_id: str
    expected_component_lifecycle_generation: Annotated[
        int, Field(ge=1, le=9_007_199_254_740_991)
    ]
    expected_component_lifecycle_state: Literal["prepared", "active"]

    @field_validator("controllable_component_id")
    @classmethod
    def validate_component_id(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="controllable_component_id",
            maximum_bytes=256,
        )


class RouteSelectionRequestResultQuery(StrictRequest):
    request_context: CallerAttributionRequest
    expected_admission_generation: Annotated[
        int | None, Field(ge=1, le=9_007_199_254_740_991)
    ] = None
    program_node_evidence: ProgramNodeAdmissionEvidenceRequest | None = None
    expected_retained_bundle_id: str
    expected_graph_revision: str
    desired_configuration: CCLCDesiredControlConfigurationRequest
    expected_active_control_configuration: CCLCExpectedActiveConfigurationRequest
    component_expectations: Annotated[
        list[CCLCRouteComponentExpectationRequest],
        Field(min_length=1, max_length=4_096),
    ]
    confirmed: bool = False

    @field_validator("expected_retained_bundle_id", "expected_graph_revision")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=256,
        )

    @model_validator(mode="after")
    def validate_component_order(self) -> "RouteSelectionRequestResultQuery":
        identifiers = [
            item.controllable_component_id for item in self.component_expectations
        ]
        if identifiers != sorted(identifiers) or len(identifiers) != len(
            set(identifiers)
        ):
            raise ValueError(
                "component_expectations must be strictly ordered by component ID"
            )
        if self.program_node_evidence and (
            self.program_node_evidence.owner_api != "cclc.control_route"
            or self.expected_admission_generation is None
        ):
            raise ValueError(
                "route node evidence requires cclc.control_route and admission generation"
            )
        return self


class RouteSelectionRequest(RouteSelectionRequestResultQuery):
    expected_admission_generation: Annotated[int, Field(ge=1, le=9_007_199_254_740_991)]
    confirmed: bool
