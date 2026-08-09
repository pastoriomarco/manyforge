"""Strict assistant callbacks into the Composer-owned in-memory Draft."""

from __future__ import annotations

from typing import Annotated, Any
import unicodedata

from fastapi import APIRouter, HTTPException, Path
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field, field_validator

from .assistant_mode_client import (
    ActiveAssistantModeProjectionError,
    read_active_assistant_mode,
)
from .assistant_catalog_narrowing import (
    AssistantCatalogNarrowingError,
    normalize_field_policy,
    normalize_requested_tools,
    resolve_assistant_catalog,
)
from .assistant_schema_validation import (
    CallbackSchemaValidationError,
    validate_callback_arguments,
)
from .draft_session import DraftSessionError, SUPPORTED_DRAFT_MUTATIONS
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .source_model_client import (
    ActiveSourceModelProjectionError,
    read_active_source_model,
)
from .state import ComposerState


_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_DIGEST_PATTERN = r"^sha256:[0-9a-f]{64}$"
_DRAFT_READ_TOOLS = {"program_read", "program_validate"}


class AssistantDraftCallbackRequest(BaseModel):
    """Closed bridge-to-Composer callback context and arguments."""

    model_config = ConfigDict(
        extra="forbid",
        strict=True,
        frozen=True,
        populate_by_name=False,
    )

    assistant_mode: str = Field(
        alias="assistantMode",
        min_length=1,
        max_length=256,
        pattern=r"^[a-z][a-z0-9.-]*$",
    )
    retained_bundle_id: str = Field(
        alias="retainedBundleId", min_length=1, max_length=256
    )
    graph_revision: str = Field(alias="graphRevision", min_length=1, max_length=256)
    node_environment_digest: Annotated[
        str, Field(alias="nodeEnvironmentDigest", pattern=_DIGEST_PATTERN)
    ]
    node_environment_projection_digest: Annotated[
        str,
        Field(alias="nodeEnvironmentProjectionDigest", pattern=_DIGEST_PATTERN),
    ]
    source_model_projection_digest: Annotated[
        str, Field(alias="sourceModelProjectionDigest", pattern=_DIGEST_PATTERN)
    ]
    assistant_projection_digest: Annotated[
        str, Field(alias="assistantProjectionDigest", pattern=_DIGEST_PATTERN)
    ]
    assistant_mode_projection_digest: Annotated[
        str, Field(alias="assistantModeProjectionDigest", pattern=_DIGEST_PATTERN)
    ]
    catalog_hash: Annotated[
        str, Field(alias="catalogHash", pattern=_DIGEST_PATTERN)
    ]
    requested_tools: list[str] = Field(alias="requestedTools", max_length=4096)
    field_policy: dict[str, Any] = Field(alias="fieldPolicy")
    input_schema_hash: Annotated[
        str, Field(alias="inputSchemaHash", pattern=_DIGEST_PATTERN)
    ]
    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: Annotated[
        int,
        Field(alias="expectedDraftRevision", ge=0, le=_MAXIMUM_SAFE_INTEGER),
    ]
    request_id: str = Field(alias="requestId", min_length=1, max_length=256)
    request_owner_epoch: str = Field(
        alias="requestOwnerEpoch", min_length=1, max_length=256
    )
    source_label: str = Field(alias="sourceLabel", min_length=1, max_length=256)
    correlation_id: str | None = Field(
        default=None,
        alias="correlationId",
        max_length=256,
    )
    arguments: dict[str, Any]

    @field_validator(
        "assistant_mode",
        "retained_bundle_id",
        "graph_revision",
        "draft_id",
        "request_id",
        "request_owner_epoch",
        "source_label",
        "correlation_id",
    )
    @classmethod
    def validate_text(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        if value != value.strip() or unicodedata.normalize("NFC", value) != value:
            raise ValueError(f"{info.field_name} must be canonical NFC text")
        return value


def _http_error(error: Exception) -> HTTPException:
    return HTTPException(
        status_code=getattr(error, "status_code", 503),
        detail={
            "code": getattr(error, "code", "assistant_callback_unavailable"),
            "message": str(error),
        },
    )


def _validation_result(
    body: AssistantDraftCallbackRequest,
    *,
    status_code: int,
    message: str,
    detail: Any,
) -> JSONResponse:
    return JSONResponse(
        status_code=200,
        content={
            "success": False,
            "mutated": False,
            "draftMutated": False,
            "requestId": body.request_id,
            "requestOwnerEpoch": body.request_owner_epoch,
            "sourceLabel": body.source_label,
            "correlationId": body.correlation_id,
            "result": {
                "kind": "validation_error",
                "httpStatus": status_code,
                "message": message,
                "detail": detail,
            },
        },
    )


def _require_equal(received: Any, expected: Any, *, field: str) -> None:
    if received != expected:
        raise HTTPException(
            status_code=409,
            detail={
                "code": "assistant_callback_context_stale",
                "message": f"callback {field} does not match the active projection",
            },
        )


def create_assistant_draft_router(state: ComposerState) -> APIRouter:
    """Bind exact active projection context to Draft-only callback effects."""

    router = APIRouter()

    @router.post("/api/assistant/bridge/tools/{tool_id}")
    def invoke_draft_tool(
        tool_id: Annotated[
            str,
            Path(min_length=1, max_length=256, pattern=r"^[a-z][a-z0-9_]*$"),
        ],
        body: AssistantDraftCallbackRequest,
    ) -> JSONResponse:
        try:
            node_environment = read_active_node_environment(state)
            source_model = read_active_source_model(
                state,
                node_environment=node_environment,
            )
            assistant_mode = read_active_assistant_mode(
                state,
                mode=body.assistant_mode,
                node_environment=node_environment,
            )
        except (
            ActiveNodeEnvironmentProjectionError,
            ActiveSourceModelProjectionError,
            ActiveAssistantModeProjectionError,
        ) as error:
            raise _http_error(error) from error

        node_payload = node_environment.payload
        node_source = node_payload["source"]
        source_payload = source_model.payload
        source_source = source_payload["source"]
        mode_payload = assistant_mode.payload
        mode_source = mode_payload["source"]
        for received, expected, field in (
            (body.retained_bundle_id, node_payload["retained_bundle_id"], "retainedBundleId"),
            (body.graph_revision, node_payload["graph_revision"], "graphRevision"),
            (
                body.node_environment_digest,
                node_payload["environment_digest"],
                "nodeEnvironmentDigest",
            ),
            (
                body.node_environment_projection_digest,
                node_source["source_content_digest"],
                "nodeEnvironmentProjectionDigest",
            ),
            (
                body.source_model_projection_digest,
                source_source["source_content_digest"],
                "sourceModelProjectionDigest",
            ),
            (
                body.assistant_projection_digest,
                mode_payload["assistant_projection_digest"],
                "assistantProjectionDigest",
            ),
            (
                body.assistant_mode_projection_digest,
                mode_source["source_content_digest"],
                "assistantModeProjectionDigest",
            ),
        ):
            _require_equal(received, expected, field=field)

        try:
            requested_tools = normalize_requested_tools(body.requested_tools)
            field_policy = normalize_field_policy(body.field_policy)
            resolved_catalog = resolve_assistant_catalog(
                mode_payload,
                requested_tools=requested_tools,
                field_policy=field_policy,
            )
        except AssistantCatalogNarrowingError as error:
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "assistant_callback_context_invalid",
                    "message": str(error),
                },
            ) from error
        _require_equal(body.catalog_hash, resolved_catalog["catalogHash"], field="catalogHash")
        resolved_tools = {
            row["tool_id"]: row for row in resolved_catalog["tools"]
        }
        tool = resolved_tools.get(tool_id)
        if tool is None:
            raise HTTPException(
                status_code=404,
                detail={
                    "code": "assistant_callback_tool_not_found",
                    "message": "tool is absent from the exact active assistant mode",
                },
            )
        _require_equal(
            body.input_schema_hash,
            tool["input_schema_hash"],
            field="inputSchemaHash",
        )
        effect = tool["effect"]
        if effect == "composer_draft_mutating":
            if body.assistant_mode != "composer-assistant":
                raise HTTPException(
                    status_code=400,
                    detail={
                        "code": "assistant_callback_mode_rejected",
                        "message": (
                            "Composer Draft mutation requires composer-assistant mode"
                        ),
                    },
                )
            if tool_id not in SUPPORTED_DRAFT_MUTATIONS:
                raise HTTPException(
                    status_code=503,
                    detail={
                        "code": "assistant_callback_tool_unavailable",
                        "message": "selected Draft callback has no Composer implementation",
                    },
                )
        elif not (effect == "read_only" and tool_id in _DRAFT_READ_TOOLS):
            raise HTTPException(
                status_code=400,
                detail={
                    "code": "assistant_callback_effect_rejected",
                    "message": "tool effect is not owned by the Composer Draft callback",
                },
            )

        snapshot = state.draft_session.snapshot()
        if effect == "composer_draft_mutating":
            program = snapshot.get("program")
            compatibility = (
                program.get("compatibility") if isinstance(program, dict) else None
            )
            if (
                isinstance(compatibility, dict)
                and compatibility.get("assistant") == "forbidden"
            ):
                raise HTTPException(
                    status_code=409,
                    detail={
                        "code": "assistant_callback_program_incompatible",
                        "message": "the current Program forbids assistant authoring",
                    },
                )
        _require_equal(body.draft_id, snapshot["draft_id"], field="draftId")
        _require_equal(
            body.expected_draft_revision,
            snapshot["draft_revision"],
            field="expectedDraftRevision",
        )
        try:
            validate_callback_arguments(body.arguments, tool["input_schema"])
        except CallbackSchemaValidationError as error:
            return _validation_result(
                body,
                status_code=400,
                message=str(error),
                detail={"path": error.path, "message": error.message},
            )

        try:
            if tool_id == "program_read":
                next_snapshot = snapshot
                mutation = None
                details: dict[str, Any] = {"program": snapshot["program"]}
            elif tool_id == "program_validate":
                next_snapshot = state.draft_session.validate(node_environment.catalog)
                mutation = None
                details = {"valid": True}
            else:
                result = state.draft_session.mutate(
                    tool_id=tool_id,
                    arguments=body.arguments,
                    expected_revision=body.expected_draft_revision,
                    catalog=node_environment.catalog,
                )
                next_snapshot = result.snapshot
                mutation = result.mutation
                details = result.details
        except DraftSessionError as error:
            if error.code in {
                "composer_draft_revision_stale",
                "composer_draft_unavailable",
            }:
                raise _http_error(error) from error
            return _validation_result(
                body,
                status_code=error.status_code,
                message=str(error),
                detail=error.detail,
            )

        mutated = mutation is not None
        return JSONResponse(
            status_code=200,
            content={
                "success": True,
                "mutated": mutated,
                "draftMutated": mutated,
                "requestId": body.request_id,
                "requestOwnerEpoch": body.request_owner_epoch,
                "sourceLabel": body.source_label,
                "correlationId": body.correlation_id,
                "result": {
                    "kind": (
                        "composer_draft_mutation" if mutated else "composer_draft_read"
                    ),
                    "mutation": mutation,
                    "details": details,
                    "draftId": next_snapshot["draft_id"],
                    "draftRevision": next_snapshot["draft_revision"],
                    "programRevision": next_snapshot["program_revision"],
                },
            },
        )

    return router


__all__ = ["AssistantDraftCallbackRequest", "create_assistant_draft_router"]
