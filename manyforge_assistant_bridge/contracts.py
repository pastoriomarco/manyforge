"""Closed current HTTP contracts for assistant bridge mediation."""

from __future__ import annotations

from typing import Annotated, Any, Literal
import unicodedata

from pydantic import BaseModel, ConfigDict, Field, field_validator


PROVIDER_REQUEST_VERSION = "manyforge.assistant.provider_request.v1"
PROVIDER_RESPONSE_VERSION = "manyforge.assistant.provider_response.v1"
PROVIDER_STATUS_VERSION = "manyforge.assistant.provider_status.v1"
_DIGEST = r"^sha256:[0-9a-f]{64}$"
_MODE = r"^[a-z][a-z0-9.-]*$"
_TOOL = r"^[a-z][a-z0-9_]*$"
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


class _ClosedModel(BaseModel):
    model_config = ConfigDict(
        extra="forbid",
        strict=True,
        frozen=True,
        populate_by_name=False,
    )


class ResolvedCatalogWire(_ClosedModel):
    catalog_hash: Annotated[str, Field(alias="catalogHash", pattern=_DIGEST)]
    grant_policy: dict[str, Any] = Field(alias="grantPolicy")
    nodes: Annotated[list[dict[str, Any]], Field(max_length=4_096)]
    skills: Annotated[list[dict[str, Any]], Field(max_length=4_096)]
    tools: Annotated[list[dict[str, Any]], Field(max_length=4_096)]


class ComposerDraftContext(_ClosedModel):
    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: Annotated[
        int,
        Field(alias="expectedDraftRevision", ge=0, le=_MAXIMUM_SAFE_INTEGER),
    ]
    program_loaded: bool = Field(alias="programLoaded")
    program_revision: Annotated[
        int | None,
        Field(alias="programRevision", ge=0, le=_MAXIMUM_SAFE_INTEGER),
    ] = None
    program: dict[str, Any] | None
    node_environment_digest: Annotated[
        str, Field(alias="nodeEnvironmentDigest", pattern=_DIGEST)
    ]
    node_environment_projection_digest: Annotated[
        str, Field(alias="nodeEnvironmentProjectionDigest", pattern=_DIGEST)
    ]
    source_model_projection_digest: Annotated[
        str, Field(alias="sourceModelProjectionDigest", pattern=_DIGEST)
    ]

    @field_validator("draft_id")
    @classmethod
    def canonical_draft_id(cls, value: str) -> str:
        return _canonical_text(value, "draftId")


class ProviderRequest(_ClosedModel):
    schema_version: Literal["manyforge.assistant.provider_request.v1"] = Field(
        alias="schemaVersion"
    )
    request_id: str = Field(alias="requestId", min_length=1, max_length=256)
    request_owner_epoch: str = Field(
        alias="requestOwnerEpoch", min_length=1, max_length=256
    )
    source_label: str = Field(alias="sourceLabel", min_length=1, max_length=256)
    correlation_id: str | None = Field(
        default=None, alias="correlationId", max_length=256
    )
    conversation_id: str = Field(
        alias="conversationId", min_length=1, max_length=256
    )
    assistant_mode: str = Field(
        alias="assistantMode", min_length=1, max_length=256, pattern=_MODE
    )
    message: str = Field(min_length=1, max_length=65_536)
    requested_tools: Annotated[
        list[Annotated[str, Field(pattern=_TOOL, max_length=256)]],
        Field(alias="requestedTools", max_length=4_096),
    ]
    field_policy: dict[str, Any] = Field(alias="fieldPolicy")
    mode_projection: dict[str, Any] = Field(alias="modeProjection")
    resolved_catalog: ResolvedCatalogWire = Field(alias="resolvedCatalog")
    composer_draft: ComposerDraftContext = Field(alias="composerDraft")

    @field_validator(
        "request_id",
        "request_owner_epoch",
        "source_label",
        "correlation_id",
        "conversation_id",
        "assistant_mode",
        "message",
    )
    @classmethod
    def canonical_text(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _canonical_text(value, info.field_name)

    @field_validator("requested_tools")
    @classmethod
    def canonical_requested_tools(cls, value: list[str]) -> list[str]:
        if value != sorted(set(value)):
            raise ValueError("requestedTools must be sorted and unique")
        return value


def _canonical_text(value: str, label: str) -> str:
    if value != value.strip() or unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"{label} must be canonical NFC text")
    return value


def provider_status(
    *,
    request: ProviderRequest,
    state: Literal["running", "completed", "failed", "cancelled"],
    response: dict[str, Any] | None = None,
    error: dict[str, str] | None = None,
) -> dict[str, Any]:
    return {
        "schemaVersion": PROVIDER_STATUS_VERSION,
        "requestId": request.request_id,
        "requestOwnerEpoch": request.request_owner_epoch,
        "sourceLabel": request.source_label,
        "correlationId": request.correlation_id,
        "conversationId": request.conversation_id,
        "assistantMode": request.assistant_mode,
        "state": state,
        "response": response,
        "error": error,
    }


__all__ = [
    "ComposerDraftContext",
    "PROVIDER_REQUEST_VERSION",
    "PROVIDER_RESPONSE_VERSION",
    "PROVIDER_STATUS_VERSION",
    "ProviderRequest",
    "ResolvedCatalogWire",
    "provider_status",
]
