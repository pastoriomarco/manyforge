"""Closed CAS wire DTOs for existing Close owner commands, without orchestration."""

from __future__ import annotations

from typing import Annotated, Any, Literal

import rfc8785
from pydantic import Field, ValidationError, field_validator, model_validator, model_serializer

from manyforge_behavior.json_scalars import MAXIMUM_SAFE_INTEGER

from .models import (
    BoundedPublicRequestContext,
    CallerAttributionRequest,
    SHA256_PATTERN,
    StrictRequest,
    _bounded_nfc,
)
from .native import HeadlessApiError


# Reference note for the activation-preparation route (rendered into the REST
# references): Close is a caller-ordered sequence and refuses an attached part.
SCENARIO_CLOSE_REFERENCE_NOTE = (
    "With `origin.kind: sealed_canonical_empty` this prepares Scenario Close. "
    "Close is a sequence the caller runs in order: Reset the Program cycle; "
    "detach every carried object (`POST /api/scene/transactions`, "
    "`detach_carried_object_at_current_pose`); disable each component and stop "
    "its processes; then prepare, preview and apply this canonical-empty "
    "configuration (`scene_initialization: preserve`). Only then New or Load. "
    "The Apply keeps the Scene, so this preparation is refused with 409 "
    "`scenario_close_attachment_present` while any carried object remains: "
    "`details.carried_objects` names each `scene_resource_id` with its carrier "
    "(`controllable_component_id`, `command_target_id`). Nothing is prepared; "
    "detach, then prepare again. Only the first preparation after a Force "
    "close step `close_operations`, before its step `end`, keeps an attachment "
    "(a part whose detach was refused because the robot's process is already "
    "gone). The Composer Close runs this sequence."
)


# Item 37: Force close, the operator action after a refused Close.
SCENARIO_FORCE_CLOSE_REFERENCE_NOTE = (
    "Force close, the operator's explicit action after a Close refused for an "
    "uncertain outcome. The caller runs it in order. Step `stop` sends a cell "
    "Full Stop (`request_context` names the execution owner epoch; "
    "`expected_stop_generation` is `cell_stop_action.expected_stop_generation`) "
    "and waits briefly until the Kernel confirms it: its certificate is the "
    "newest and succeeded. Otherwise it is refused with 409 "
    "`scenario_force_close_stop_unconfirmed` (`details.reason`: `stop_pending`, "
    "`stop_superseded`, `stop_not_contained`) and the caller does nothing more. "
    "Then the caller retires the lifecycle targets (now admitted with resident "
    "operations). Step `close_operations` then moves every resident operation "
    "into history marked `cell_force_closed_by_operator`, frees its resources, "
    "clears the cell-wide motion block an unresolved operation closure left "
    "(`motion_block_cleared`; motion still needs Prepare and Enable), "
    "and records one History row (`scenario_force_closed`) naming the "
    "operations and outputs left in an unknown state. It is refused "
    "`scenario_force_close_not_closing` before the retirement and "
    "`scenario_force_close_operation_busy` while an operation is being "
    "updated (retry); a pending I/O-rule Stop response is not waited for. "
    "Orphan resources from an earlier restart are freed and their operations "
    "recorded settled, which ends that motion block too. Then the caller runs "
    "the ordinary Reset and detach while "
    "the component processes still run (a part whose detach is refused, such "
    "as when the robot's process is already gone, stays attached; the next "
    "empty preparation then skips the attachment check), disables each component where "
    "possible, stops its processes, and runs the canonical-empty Apply. On "
    "every exit, success or failure, the caller sends step `end`, which ends "
    "that attachment exception. Force "
    "close never commands an output: an output's "
    "physical state stays unknown. The cell Stop fault is kept."
)


class ScenarioForceCloseRequest(StrictRequest):
    """Item 37: one step of an operator Force close; Composer runs the order."""

    request_context: CallerAttributionRequest
    # `end` (SX-616): the caller's sequence ended, success or failure.
    step: Literal["stop", "close_operations", "end"]
    expected_stop_generation: Annotated[int, Field(ge=0, le=MAXIMUM_SAFE_INTEGER)] | None = None

    @model_validator(mode="after")
    def validate_step(self) -> ScenarioForceCloseRequest:
        if (self.step == "stop") != (self.expected_stop_generation is not None):
            raise ValueError("expected_stop_generation is required exactly for step stop")
        return self


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
