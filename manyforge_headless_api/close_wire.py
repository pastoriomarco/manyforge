"""Closed CAS wire DTOs for existing Close owner commands, without orchestration."""

from __future__ import annotations

from typing import Annotated, Any, Literal

import rfc8785
from pydantic import Field, ValidationError, field_validator, model_validator, model_serializer

from manyforge_behavior.json_scalars import MAXIMUM_SAFE_INTEGER

from .models import (
    BoundedPublicRequestContext,
    SHA256_PATTERN,
    StrictRequest,
    _bounded_nfc,
)
from .native import HeadlessApiError


class LifecycleTargetRetirementScope(StrictRequest):
    request_owner_epoch: str
    publication_generation: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    active_public_pointer_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("request_owner_epoch")
    @classmethod
    def validate_owner(cls, value: str) -> str:
        return _bounded_nfc(value, label="request_owner_epoch", maximum_bytes=128)


class LifecycleTargetRetirementRow(StrictRequest):
    controllable_component_id: str
    requested_state: Literal["active", "prepared", "disabled"]
    origin: Literal["startup", "operator", "program"]
    target_generation: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    reached: bool
    recovered_cause: Literal[
        "none", "availability", "emergency_stop", "protective_stop", "safeguard_stop",
        "fault", "configuration", "unknown", "collision", "external_control", "provider_lost",
    ] | None

    @field_validator("controllable_component_id")
    @classmethod
    def validate_component(cls, value: str) -> str:
        return _bounded_nfc(value, label="controllable_component_id", maximum_bytes=128)


def _ordered_targets(rows: list[LifecycleTargetRetirementRow]) -> None:
    identifiers = [row.controllable_component_id for row in rows]
    if identifiers != sorted(set(identifiers)):
        raise ValueError("retirement targets must be unique and sorted by component ID")


class LifecycleTargetRetirementIntent(StrictRequest):
    request_context: BoundedPublicRequestContext
    scope: LifecycleTargetRetirementScope
    expected_targets: Annotated[list[LifecycleTargetRetirementRow], Field(max_length=64)]

    @model_serializer(mode="wrap")
    def serialize_optional_context(self, handler):
        payload = handler(self)
        if payload["request_context"].get("correlation_id") is None:
            payload["request_context"].pop("correlation_id", None)
        return payload

    @model_validator(mode="after")
    def validate_cas(self) -> "LifecycleTargetRetirementIntent":
        if self.request_context.request_owner_epoch != self.scope.request_owner_epoch:
            raise ValueError("retirement request owner must equal its scope owner")
        _ordered_targets(self.expected_targets)
        return self


class LifecycleTargetRetirementOffer(StrictRequest):
    scope: LifecycleTargetRetirementScope
    targets: Annotated[list[LifecycleTargetRetirementRow], Field(max_length=64)]

    @field_validator("targets")
    @classmethod
    def validate_order(cls, rows: list[LifecycleTargetRetirementRow]) -> list[LifecycleTargetRetirementRow]:
        _ordered_targets(rows)
        return rows


class LifecycleTargetRetirementResult(StrictRequest):
    request_context: BoundedPublicRequestContext
    replay_status: Literal["accepted_new", "replayed_exact"]
    scope: LifecycleTargetRetirementScope
    retired_targets: Annotated[list[LifecycleTargetRetirementRow], Field(max_length=64)]

    @model_serializer(mode="wrap")
    def serialize_optional_context(self, handler):
        payload = handler(self)
        if payload["request_context"].get("correlation_id") is None:
            payload["request_context"].pop("correlation_id", None)
        return payload

    @field_validator("retired_targets")
    @classmethod
    def validate_order(cls, rows: list[LifecycleTargetRetirementRow]) -> list[LifecycleTargetRetirementRow]:
        _ordered_targets(rows)
        return rows


class LifecycleOwnerSettlementRequest(StrictRequest):
    scope: LifecycleTargetRetirementScope


class LifecycleOwnerSettlementResult(LifecycleOwnerSettlementRequest):
    settled: Literal[True]

    @field_validator("settled", mode="before")
    @classmethod
    def validate_boolean(cls, value: Any) -> bool:
        if value is not True:
            raise ValueError("settlement requires the actual boolean true")
        return value


class CanonicalEmptySelection(StrictRequest):
    request_owner_epoch: str
    cell_id: str
    deployment_artifact_id: str
    deployment_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    candidate_retained_bundle_id: Annotated[str, Field(pattern=SHA256_PATTERN)]
    candidate_graph_revision: Annotated[str, Field(pattern=SHA256_PATTERN)]
    admission_digest: Annotated[str, Field(pattern=SHA256_PATTERN)]

    @field_validator("request_owner_epoch", "cell_id", "deployment_artifact_id")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=128)

    @model_validator(mode="before")
    @classmethod
    def validate_record_bound(cls, value):
        try:
            encoded = rfc8785.dumps(value)
        except (rfc8785.CanonicalizationError, TypeError, ValueError) as error:
            raise ValueError("canonical-empty selection is not valid canonical JSON") from error
        if not isinstance(value, dict) or len(encoded) > 16 * 1024:
            raise ValueError("canonical-empty selection exceeds its existing 16 KiB record bound")
        return value


def retirement_result(value: Any, intent: LifecycleTargetRetirementIntent) -> dict[str, Any]:
    try:
        result = LifecycleTargetRetirementResult.model_validate(value)
        if (result.request_context != intent.request_context or result.scope != intent.scope
                or result.retired_targets != intent.expected_targets):
            raise ValueError("retirement result does not match its complete CAS request")
    except (ValidationError, ValueError) as error:
        raise HeadlessApiError("cclc_open_response_invalid", "Retirement result is invalid") from error
    return result.model_dump(mode="json")


def retirement_offer(value: Any) -> dict[str, Any]:
    try:
        LifecycleTargetRetirementOffer.model_validate(value)
    except ValidationError as error:
        raise HeadlessApiError("cclc_open_response_invalid", "Retirement offer is invalid") from error
    return value


def settlement_result(value: Any, scope: LifecycleTargetRetirementScope) -> dict[str, Any]:
    try:
        result = LifecycleOwnerSettlementResult.model_validate(value)
        if result.scope != scope:
            raise ValueError("settlement result does not match its scope")
    except (ValidationError, ValueError) as error:
        raise HeadlessApiError("cclc_open_response_invalid", "Owner settlement proof is invalid") from error
    return value
