"""Thin Composer request/status/cancel mediation for the assistant bridge.

The bridge is the process of the Deployment's `assistant` component. Each
call resolves it: the Headless mode index names the component, and its
published `assistant_request` endpoint is the URL while it is enabled.
Composer is the single gate between chat turns and execution: a turn starts
only while no Program run exists, and Execute admission refuses while one of
the turns started here is still running (`require_no_assistant_turn`).
"""

from __future__ import annotations

from typing import Annotated, Any
import unicodedata

from fastapi import APIRouter, HTTPException, Path
from manyforge_headless_api.client import HeadlessClientError
from pydantic import BaseModel, ConfigDict, Field, field_validator

from .assistant_catalog_narrowing import (
    AssistantCatalogNarrowingError,
    normalize_field_policy,
    normalize_requested_tools,
)
from .assistant_mode_client import (
    ActiveAssistantModeProjectionError,
    read_active_assistant_context,
)
from .assistant_provider_client import AssistantProviderClientError
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
)
from .routes_draft import assistant_reference_validation
from .source_model_client import ActiveSourceModelProjectionError
from .state import ComposerState


_PROVIDER_REQUEST_VERSION = "manyforge.assistant.provider_request.v1"
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


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


_ENDPOINT_ROLE = "assistant_request"
_TERMINAL_STATES = frozenset({"completed", "failed", "cancelled"})
# Refusals that mean no bridge serves: its turns are gone with it.
_BRIDGE_GONE = frozenset(
    {
        "assistant_component_absent",
        "assistant_component_not_enabled",
        "assistant_provider_unavailable",
        "assistant_request_not_found",
    }
)


def _error(error: Exception) -> HTTPException:
    return HTTPException(
        status_code=getattr(error, "status_code", None) or 503,
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


def _current_admission_generation(state: ComposerState) -> int:
    if state.headless_client is None:
        raise HTTPException(
            status_code=503,
            detail={
                "code": "assistant_request_unavailable",
                "message": "assistant authoring requires Headless admission",
            },
        )
    try:
        admission = state.headless_client.authoring_admission()
    except HeadlessClientError as error:
        raise _error(error) from error
    generation = admission.get("admission_generation")
    if type(generation) is not int or not 1 <= generation <= _MAXIMUM_SAFE_INTEGER:
        raise HTTPException(
            status_code=502,
            detail={
                "code": "assistant_owner_response_invalid",
                "message": "Headless omitted a valid authoring admission generation",
            },
        )
    return generation


def _bridge_url(state: ComposerState, *, new_turn: bool) -> str:
    """The assistant component's endpoint, or its typed refusal.

    ``new_turn`` adds the submit-only checks: no Program run exists and the
    referenced AI model is enabled.
    """

    client = state.headless_client
    assert client is not None
    index = client.assistant_modes()
    assistant_id = index.get("assistant_component_id")
    if assistant_id is None:
        raise AssistantProviderClientError(
            "assistant_component_absent",
            "Add an Assistant component to the Deployment to use the AI assistant",
            409,
        )
    if new_turn and client.program_runs(active_only=True).get("runs"):
        raise AssistantProviderClientError(
            "assistant_program_active",
            "The AI assistant runs only while no Program is active: "
            "Stop or finish the run first",
            409,
        )
    components = {row.get("component_id"): row for row in client.components()}
    component = components.get(assistant_id) or {}
    url = next(
        (
            endpoint.get("url")
            for endpoint in component.get("endpoints", [])
            if endpoint.get("role_id") == _ENDPOINT_ROLE
        ),
        None,
    )
    if component.get("lifecycle") != "enabled" or type(url) is not str or not url:
        raise AssistantProviderClientError(
            "assistant_component_not_enabled",
            f"Assistant {assistant_id} is {component.get('lifecycle', 'absent')}: "
            "Enable it",
            409,
        )
    if new_turn:
        model_id = index.get("ai_model_component_id")
        model = components.get(model_id) or {}
        if model.get("lifecycle") != "enabled":
            raise AssistantProviderClientError(
                "assistant_ai_model_not_enabled",
                f"AI model {model_id} is {model.get('lifecycle', 'absent')}",
                409,
            )
    return url


def _drop_turn(state: ComposerState, status: dict[str, Any]) -> dict[str, Any]:
    if status.get("state") in _TERMINAL_STATES:
        with state.assistant_turns_lock:
            state.assistant_turns.discard(status.get("requestId"))
    return status


def require_no_assistant_turn(state: ComposerState) -> None:
    """Refuse Execute while one of Composer's chat turns is running.

    The in-flight set is refreshed first: a turn whose status is terminal or
    not found, or whose bridge no longer serves, is dropped. Nothing is
    cancelled. Callers hold the workspace fence, so no turn starts meanwhile.
    """

    with state.assistant_turns_lock:
        request_ids = sorted(state.assistant_turns)
    if not request_ids:
        return
    provider = state.assistant_client
    assert provider is not None
    gone: set[str] = set()
    try:
        url = _bridge_url(state, new_turn=False)
        for request_id in request_ids:
            try:
                if provider.status(url, request_id)["state"] in _TERMINAL_STATES:
                    gone.add(request_id)
            except AssistantProviderClientError as error:
                # Any other failure (a timeout) leaves the turn running.
                if error.code in _BRIDGE_GONE:
                    gone.add(request_id)
    except HeadlessClientError as error:
        raise _error(error) from error
    except AssistantProviderClientError as error:
        if error.code not in _BRIDGE_GONE:
            raise _error(error) from error
        gone.update(request_ids)
    with state.assistant_turns_lock:
        state.assistant_turns.difference_update(gone)
        running = bool(state.assistant_turns)
    if running:
        raise HTTPException(
            status_code=409,
            detail={
                "code": "execution_assistant_turn_in_progress",
                "message": (
                    "Cancel or wait for the AI assistant first: a chat turn "
                    "is running"
                ),
            },
        )


def create_assistant_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    def resolved(call, *, new_turn: bool = False) -> dict[str, Any]:
        provider = state.assistant_client
        assert provider is not None
        try:
            return call(provider, _bridge_url(state, new_turn=new_turn))
        except (HeadlessClientError, AssistantProviderClientError) as error:
            raise _error(error) from error

    @router.post("/api/assistant/requests", status_code=202)
    def submit(body: AssistantConversationRequest) -> dict[str, Any]:
        try:
            requested_tools = normalize_requested_tools(body.requested_tools)
            field_policy = normalize_field_policy(body.field_policy)
            context = read_active_assistant_context(
                state,
                mode=body.assistant_mode,
            )
            node_environment = context.node_environment
            source_model = context.source_model
            assistant_mode = context.assistant_mode
            resolved_catalog = state.assistant_context_cache.resolved_catalog(
                context,
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
        # The assistant callback carries the composite workspace fence, so the
        # submitted context must name the same workspace identity the browser
        # holds; it is captured once here and rechecked by the callback commit.
        workspace_identity = state.authoring_workspace.identity()
        admission_generation = _current_admission_generation(state)
        # One reference validation per request: the prompt's cell facts and
        # the reference diagnostics the conversation starts with.
        selector_facts, reference_diagnostics = (
            assistant_reference_validation(state, draft["authoring"], workspace_identity)
            if draft["program_loaded"] else (None, [])
        )
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
                "expectedWorkspaceEpoch": workspace_identity.workspace_epoch,
                "expectedWorkspaceRevision": (
                    workspace_identity.workspace_revision
                ),
                "expectedAdmissionGeneration": admission_generation,
                "programLoaded": draft["program_loaded"],
                "authoring": draft["authoring"],
                "candidateAvailable": draft["candidate_available"],
                "programRevision": draft["program_revision"],
                "program": draft["program"],
                "diagnostics": draft["diagnostics"],
                "selectorFacts": selector_facts,
                "referenceDiagnostics": reference_diagnostics,
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

        def submit_turn() -> dict[str, Any]:
            status = resolved(
                lambda provider, url: provider.submit(url, payload), new_turn=True
            )
            _require_matching_status(status, payload)
            if status["state"] == "running":
                with state.assistant_turns_lock:
                    state.assistant_turns.add(status["requestId"])
            return status

        # Under the workspace fence, as Execute admission: a turn and an
        # Execute can't start together.
        assert state.authoring_admission is not None
        return state.authoring_admission.submit_workspace_fenced(submit_turn)

    @router.get("/api/assistant/requests/{request_id}")
    def status(
        request_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _drop_turn(state, resolved(
            lambda provider, url: provider.status(url, request_id)
        ))

    @router.post("/api/assistant/requests/{request_id}/cancel")
    def cancel(
        request_id: Annotated[str, Path(min_length=1, max_length=256)],
    ) -> dict[str, Any]:
        return _drop_turn(state, resolved(
            lambda provider, url: provider.cancel(url, request_id)
        ))

    return router


__all__ = [
    "AssistantConversationRequest",
    "create_assistant_router",
    "require_no_assistant_turn",
]
