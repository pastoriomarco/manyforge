"""Thin Composer request/status/cancel mediation for the assistant bridge."""

from __future__ import annotations

from typing import Annotated, Any
import unicodedata

from fastapi import APIRouter, HTTPException, Path
from pydantic import BaseModel, ConfigDict, Field, field_validator

from .assistant_catalog_narrowing import (
    AssistantCatalogNarrowingError,
    normalize_field_policy,
    normalize_requested_tools,
    resolve_assistant_catalog,
)
from .assistant_mode_client import (
    ActiveAssistantModeProjectionError,
    read_active_assistant_mode,
)
from .assistant_provider_client import AssistantProviderClientError
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .source_model_client import (
    ActiveSourceModelProjectionError,
    read_active_source_model,
)
from .state import ComposerState


_PROVIDER_REQUEST_VERSION = "manyforge.assistant.provider_request.v1"


class AssistantConversationRequest(BaseModel):
    model_config = ConfigDict(
        extra="forbid", strict=True, frozen=True, populate_by_name=False
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
        alias="assistantMode",
        min_length=1,
        max_length=256,
        pattern=r"^[a-z][a-z0-9.-]*$",
    )
    message: str = Field(min_length=1, max_length=65_536)
    requested_tools: list[
        Annotated[str, Field(pattern=r"^[a-z][a-z0-9_]*$", max_length=256)]
    ] = Field(default_factory=list, alias="requestedTools", max_length=4_096)
    field_policy: dict[str, Any] = Field(
        default_factory=dict, alias="fieldPolicy"
    )

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
        if value != value.strip() or unicodedata.normalize("NFC", value) != value:
            raise ValueError(f"{info.field_name} must be canonical NFC text")
        return value


def _error(error: Exception) -> HTTPException:
    return HTTPException(
        status_code=getattr(error, "status_code", 503),
        detail={
            "code": getattr(error, "code", "assistant_request_unavailable"),
            "message": str(error),
        },
    )


def _require_matching_status(status: dict[str, Any], body: dict[str, Any]) -> None:
    for status_name, request_name in (
        ("requestId", "requestId"),
        ("requestOwnerEpoch", "requestOwnerEpoch"),
        ("sourceLabel", "sourceLabel"),
        ("correlationId", "correlationId"),
        ("conversationId", "conversationId"),
        ("assistantMode", "assistantMode"),
    ):
        if status.get(status_name) != body[request_name]:
            raise HTTPException(
                status_code=502,
                detail={
                    "code": "assistant_provider_response_invalid",
                    "message": f"assistant bridge changed {status_name}",
                },
            )


def create_assistant_router(state: ComposerState) -> APIRouter:
    if state.assistant_client is None:
        raise ValueError("assistant client is required for assistant routes")
    provider = state.assistant_client
    router = APIRouter()

    @router.post("/api/assistant/requests", status_code=202)
    def submit(body: AssistantConversationRequest) -> dict[str, Any]:
        try:
            requested_tools = normalize_requested_tools(body.requested_tools)
            field_policy = normalize_field_policy(body.field_policy)
            node_environment = read_active_node_environment(state)
            source_model = read_active_source_model(
                state, node_environment=node_environment
            )
            assistant_mode = read_active_assistant_mode(
                state,
                mode=body.assistant_mode,
                node_environment=node_environment,
            )
            resolved_catalog = resolve_assistant_catalog(
                assistant_mode.payload,
                requested_tools=requested_tools,
                field_policy=field_policy,
            )
        except AssistantCatalogNarrowingError as error:
            raise HTTPException(
                status_code=400,
                detail={"code": "assistant_request_invalid", "message": str(error)},
            ) from error
        except (
            ActiveNodeEnvironmentProjectionError,
            ActiveSourceModelProjectionError,
            ActiveAssistantModeProjectionError,
        ) as error:
            raise _error(error) from error

        draft = state.draft_session.snapshot()
        mode = assistant_mode.payload
        payload = {
            "schemaVersion": _PROVIDER_REQUEST_VERSION,
            "requestId": body.request_id,
            "requestOwnerEpoch": body.request_owner_epoch,
            "sourceLabel": body.source_label,
            "correlationId": body.correlation_id,
            "conversationId": body.conversation_id,
            "assistantMode": body.assistant_mode,
            "message": body.message,
            "requestedTools": requested_tools,
            "fieldPolicy": field_policy,
            "modeProjection": mode,
            "resolvedCatalog": resolved_catalog,
            "composerDraft": {
                "draftId": draft["draft_id"],
                "expectedDraftRevision": draft["draft_revision"],
                "programLoaded": draft["program_loaded"],
                "programRevision": draft["program_revision"],
                "program": draft["program"],
                "nodeEnvironmentDigest": node_environment.payload[
                    "environment_digest"
                ],
                "nodeEnvironmentProjectionDigest": node_environment.payload[
                    "source"
                ]["source_content_digest"],
                "sourceModelProjectionDigest": source_model.payload["source"][
                    "source_content_digest"
                ],
            },
        }
        try:
            status = provider.submit(payload)
        except AssistantProviderClientError as error:
            raise _error(error) from error
        _require_matching_status(status, payload)
        return status

    @router.get("/api/assistant/requests/{request_id}")
    def status(
        request_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        try:
            return provider.status(request_id)
        except AssistantProviderClientError as error:
            raise _error(error) from error

    @router.post("/api/assistant/requests/{request_id}/cancel")
    def cancel(
        request_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        try:
            return provider.cancel(request_id)
        except AssistantProviderClientError as error:
            raise _error(error) from error

    return router


__all__ = ["AssistantConversationRequest", "create_assistant_router"]
