"""Strict public request DTOs for the H0 HTTP projection."""

from __future__ import annotations

from typing import Annotated
import unicodedata

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


class HistorianReconcileRequest(CallerAttributionRequest):
    expected_health_generation: Annotated[
        int,
        Field(ge=1, le=9_007_199_254_740_991),
    ]
