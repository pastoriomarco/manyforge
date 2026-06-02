"""Assistant routes - /api/assistant/*"""
from __future__ import annotations

import hashlib
import json
import os
import re
import time
import uuid
from typing import Any

from fastapi import APIRouter, HTTPException, Query
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import JSONResponse
from manyforge_assistant_bridge.protocol import compute_catalog_hash
from manyforge_behavior.deployment import DeploymentValidationError
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
)
from pydantic import ValidationError

from .assistant_tools import (
    AssistantToolError,
    assistant_tool_catalog,
    inspect_isaac_scene_report,
    propose_scene_object_nodes,
    propose_scene_object_resources,
)
from . import assistant_recovery
from . import _assistant_delta
from .assistant_tool_schemas import (
    assistant_tool_input_schema,
    enrich_assistant_tool_descriptors,
)
from .assistant_provider import (
    AssistantProviderContext,
    AssistantProviderError,
    MockAssistantProvider,
    assistant_provider_catalog,
    build_assistant_provider,
)
from .models import (
    AssistantBridgeToolRequest,
    AssistantBridgeToolResponse,
    AssistantCancelResponse,
    AssistantChatRequest,
    AssistantChatResponse,
    AssistantInspectIsaacSceneRequest,
    AssistantInspectIsaacSceneResponse,
    AssistantProposalDiscardRequest,
    AssistantProposalListResponse,
    AssistantProposalMaterializeResponse,
    AssistantProposalRecordResponse,
    AssistantProposalStageRequest,
    AssistantProposalStageResponse,
    AssistantProposeSceneObjectNodesRequest,
    AssistantProposeSceneObjectNodesResponse,
    AssistantProposeSceneObjectsRequest,
    AssistantProposeSceneObjectsResponse,
    AssistantProviderCatalogResponse,
    AssistantRequestStatusResponse,
    AssistantToolCatalogResponse,
    BlackboardKeyResponse,
    BlackboardNamespaceResponse,
    ProgramSceneResourceResponse,
    ProgramParameterResponse,
    TreeNodeResponse,
)
from .routes_scene import build_scene_state_payload, canonical_scene_world_frame
from .state import ComposerState


READ_ONLY_EFFECT = "read_only"
COMPOSER_DRAFT_MUTATING_EFFECT = "composer_draft_mutating"
COMPOSER_ASSISTANT_MODE = "composer-assistant"
_WORLD_FRAME_ALIASES = {"world", "/world", "/World", "World", "universe"}
_BOX_SHAPE_ALIASES = {"box", "cube", "cuboid", "rectangular_prism", "rectangular-prism"}
_SPHERE_SHAPE_ALIASES = {"sphere", "ball"}
_CYLINDER_SHAPE_ALIASES = {"cylinder", "tube", "can"}
_MESH_SHAPE_ALIASES = {"mesh", "trimesh", "stl", "obj", "dae", "usd"}


def create_assistant_router(state: ComposerState) -> APIRouter:
    """Create assistant routes.

    Model-backed providers are deliberately behind an explicit provider mode.
    Returned model proposals remain drafts. Bridge callbacks may apply explicit
    composer draft mutations to the in-memory session only.
    """
    router = APIRouter()

    @router.post(
        "/chat",
        response_model=AssistantChatResponse,
        response_model_by_alias=True,
    )
    def chat(request: AssistantChatRequest) -> dict[str, Any]:
        """Return reviewable assistant proposals without mutating Composer state."""
        request_id = request.request_id or f"mf-{uuid.uuid4().hex[:12]}"
        conversation_id = request.conversation_id or uuid.uuid4().hex
        cycle_state = state.cycle_manager.state.value if state.cycle_manager else "idle"
        program_loaded = state.program is not None
        provider = _resolve_provider(state, request)
        provider_context = _assistant_provider_context(
            state,
            request,
            conversation_id=conversation_id,
            request_id=request_id,
            program_loaded=program_loaded,
            cycle_state=cycle_state,
        )
        state.start_assistant_request(
            request_id=request_id,
            conversation_id=conversation_id,
            provider_id=provider.provider_id,
            assistant_mode=provider_context.assistant_mode,
        )

        try:
            result = provider.run(request, provider_context)
        except AssistantProviderError as exc:
            status_code = _provider_error_status_code(exc)
            status = "cancelled" if exc.code == "cancelled" else "failed"
            error = {"code": exc.code, "detail": exc.detail}
            state.finish_assistant_request(
                request_id,
                status=status,
                message=str(exc),
                error=error,
                warnings=[exc.detail],
            )
            return JSONResponse(
                status_code=status_code,
                content={
                    "requestId": request_id,
                    "conversationId": conversation_id,
                    "mode": request.mode,
                    "providerId": provider.provider_id,
                    "assistantMode": provider_context.assistant_mode,
                    "message": str(exc),
                    "proposals": [],
                    "toolCalls": [],
                    "warnings": [exc.detail],
                    "mutated": False,
                    "draftMutated": False,
                    "requiresReview": True,
                    "programLoaded": program_loaded,
                    "cycleState": cycle_state,
                    "timestampNs": time.monotonic_ns(),
                    "error": error,
                },
            )

        # Aggregate the actual draft-mutation flag from the live
        # tool-call recorder. The OpenClaw bridge envelope hard-codes
        # draftMutated=False because tool-call accounting happens on
        # the ManyForge side, not in the bridge response. So we OR
        # the provider's own claim with the recorded per-call flags
        # — whichever is true wins. This drives the frontend hot-
        # reload trigger; without it the canvas would silently miss
        # successful assistant mutations on the OpenClaw lane.
        recorded_history = state.get_assistant_tool_history(
            conversation_id=conversation_id,
            limit=200,
        )
        recorded_draft_mutated = any(
            entry.get("draft_mutated") for entry in recorded_history
        )
        aggregated_draft_mutated = bool(result.draft_mutated) or recorded_draft_mutated

        state.finish_assistant_request(
            request_id,
            status="completed",
            message=result.message,
            warnings=result.warnings,
            draft_mutated=aggregated_draft_mutated,
            tool_call_count=len(result.tool_calls),
        )

        return {
            "requestId": request_id,
            "conversationId": conversation_id,
            "mode": result.mode,
            "providerId": provider.provider_id,
            "assistantMode": provider_context.assistant_mode,
            "message": result.message,
            "proposals": result.proposals,
            "toolCalls": result.tool_calls,
            "warnings": result.warnings,
            "mutated": result.mutated,
            "draftMutated": aggregated_draft_mutated,
            "requiresReview": result.requires_review,
            "programLoaded": program_loaded,
            "cycleState": cycle_state,
            "timestampNs": result.timestamp_ns,
            "error": None,
        }

    @router.get(
        "/tools",
        response_model=AssistantToolCatalogResponse,
        response_model_by_alias=True,
    )
    def list_tools() -> dict[str, Any]:
        """List deterministic assistant tools available before model integration."""
        return {"tools": assistant_tool_catalog()}

    @router.get("/modes/{mode}")
    def describe_assistant_mode(mode: str) -> dict[str, Any]:
        """Mode-scoped tool/node manifest for an external MCP wrapper.

        Returns exactly the tools and node kinds the bridge accepts in this
        mode, the catalog hash the caller must echo on each tool call, and
        each tool's input schema. External agent runtimes (OpenClaw via
        manyforge-mcp-bridge) consume this to expose a narrow MCP surface
        that maps 1-to-1 onto the hardened bridge endpoint.
        """
        resolved_catalog, expected_hash = _resolve_bridge_tool_context(
            state,
            assistant_mode=mode,
        )
        deployment = state.deployment
        assert deployment is not None
        tools_payload: list[dict[str, Any]] = []
        for entry in resolved_catalog["tools"]:
            tool_id = entry.get("id")
            if not isinstance(tool_id, str):
                continue
            descriptor = deployment.tool_catalog.get(tool_id)
            if descriptor is None:
                continue
            input_schema = assistant_tool_input_schema(tool_id) or {
                "type": "object",
                "additionalProperties": True,
            }
            tools_payload.append(
                {
                    "id": tool_id,
                    "description": descriptor.description or "",
                    "effect": descriptor.effect,
                    "inputSchema": input_schema,
                }
            )
        # Rich per-node descriptors keyed by id, alongside the legacy
        # `nodes` string list. The legacy field is preserved verbatim
        # for back-compat with the direct-lane manyforge_assistant_bridge,
        # which validates `catalog.nodes` entries as non-empty strings.
        #
        # The nodeCatalog payload is the OUTPUT of catalog_read filtered
        # to mode-allowed entries. Using the same payload-shape function
        # the model would get from a catalog_read tool call eliminates
        # skew: there is one source of truth for catalog metadata, and
        # whether the model reads it from this preamble or from a
        # tool call, the structure and fields are identical. This is
        # the load-bearing fix for the "model invents node-kind ids"
        # failure mode — if every parameter description, every enum
        # option, every required capability is in the preamble, the
        # model has no excuse to guess. Filtering by mode-allowed
        # nodes is the only deviation from a full catalog_read.
        node_id_set = set(resolved_catalog["nodes"])
        node_catalog_payload: list[dict[str, Any]] = [
            _catalog_entry_payload(entry, include_parameters=True)
            for entry in state.catalog.entries
            if entry.id in node_id_set
        ]
        # Skill payload also follows the read-function-shape rule:
        # the same _skills_payload that powers skills_read serves the
        # preamble. Bodies-included so the model has the full skill
        # expansion available without burning a tool call.
        visible_skill_ids = list(resolved_catalog.get("skills") or [])
        try:
            skills_full_payload = _skills_payload(
                state, skill_ids=visible_skill_ids, include_bodies=True,
            ) if visible_skill_ids else []
        except HTTPException:
            # If a visible skill id is unknown (deployment skew between
            # mode allowlist and skill catalog), degrade gracefully to
            # the bare-id list rather than 500 the manifest.
            skills_full_payload = []
        return {
            "mode": mode,
            "catalogHash": expected_hash,
            "tools": tools_payload,
            "nodeCatalog": node_catalog_payload,
            "skillCatalog": skills_full_payload,
        }

    @router.get(
        "/providers",
        response_model=AssistantProviderCatalogResponse,
        response_model_by_alias=True,
    )
    def list_providers() -> dict[str, Any]:
        """List assistant provider modes available to Composer."""
        configured = _configured_assistant_provider(state)
        configured_provider_id = configured.provider_id if configured is not None else "mock"
        return {
            "providers": assistant_provider_catalog(configured),
            "configuredProviderId": configured_provider_id,
        }

    @router.get(
        "/requests/{request_id}",
        response_model=AssistantRequestStatusResponse,
        response_model_by_alias=True,
    )
    def get_request_status(request_id: str) -> dict[str, Any]:
        """Return Composer's status record for an assistant request."""
        record = state.get_assistant_request(request_id)
        if record is None:
            raise HTTPException(status_code=404, detail="Assistant request not found")
        return _assistant_request_payload(record)

    @router.post(
        "/requests/{request_id}/cancel",
        response_model=AssistantCancelResponse,
        response_model_by_alias=True,
    )
    def cancel_request(request_id: str) -> dict[str, Any]:
        """Ask the configured provider to cancel an in-flight assistant request."""
        record = state.get_assistant_request(request_id)
        if record is None:
            raise HTTPException(status_code=404, detail="Assistant request not found")
        if record.status != "running":
            return {
                "requestId": request_id,
                "cancelled": False,
                "status": record.status,
                "detail": f"Assistant request is already {record.status}.",
                "providerResponse": {},
                "timestampNs": time.monotonic_ns(),
            }
        provider = _configured_assistant_provider(state)
        if provider is None:
            state.finish_assistant_request(
                request_id,
                status="failed",
                message="No assistant provider is configured.",
                error={
                    "code": "provider_unavailable",
                    "detail": "No assistant provider is configured.",
                },
            )
            return {
                "requestId": request_id,
                "cancelled": False,
                "status": "failed",
                "detail": "No assistant provider is configured.",
                "providerResponse": {},
                "timestampNs": time.monotonic_ns(),
            }
        cancel_fn = getattr(provider, "cancel", None)
        if not callable(cancel_fn):
            return {
                "requestId": request_id,
                "cancelled": False,
                "status": record.status,
                "detail": "Configured assistant provider does not support cancellation.",
                "providerResponse": {},
                "timestampNs": time.monotonic_ns(),
            }
        state.mark_assistant_request_cancel_requested(request_id)
        try:
            provider_response = cancel_fn(request_id)
        except AssistantProviderError as exc:
            state.finish_assistant_request(
                request_id,
                status="failed",
                message=str(exc),
                error={"code": exc.code, "detail": exc.detail},
                warnings=[exc.detail],
            )
            raise HTTPException(status_code=_provider_error_status_code(exc), detail=str(exc)) from exc
        cancelled = bool(provider_response.get("cancelled", True))
        status = "cancel_requested" if cancelled else record.status
        return {
            "requestId": request_id,
            "cancelled": cancelled,
            "status": status,
            "detail": str(provider_response.get("detail") or ""),
            "providerResponse": provider_response,
            "timestampNs": time.monotonic_ns(),
        }

    @router.put("/bridge/principal-binding/{principal}")
    def bind_principal(principal: str, body: dict[str, Any]) -> dict[str, Any]:
        """Bind a bridge `principal` to the in-flight chat assistant request.

        Used by the OpenClaw bridge service so that tool calls coming
        from the MCP bridge subprocess (which substitutes its own
        bridge-process conversation id in the envelope) can still be
        correlated back to the active chat for live tool-call streaming.
        Single-tenant per sandbox: there is one chat per principal at a
        time. Idempotent.
        """
        request_id = (body or {}).get("requestId") or (body or {}).get("request_id")
        if not isinstance(request_id, str) or not request_id.strip():
            raise HTTPException(status_code=400, detail="requestId is required")
        state.bind_principal_to_request(
            principal=principal, request_id=request_id.strip()
        )
        return {"principal": principal, "requestId": request_id.strip(), "bound": True}

    @router.delete("/bridge/principal-binding/{principal}")
    def unbind_principal(principal: str) -> dict[str, Any]:
        state.unbind_principal(principal)
        return {"principal": principal, "bound": False}

    @router.post(
        "/bridge/tools/{tool_id}",
        response_model=AssistantBridgeToolResponse,
        response_model_by_alias=True,
    )
    async def execute_bridge_tool(
        tool_id: str,
        request: AssistantBridgeToolRequest,
    ) -> dict[str, Any]:
        """Execute a mode-authorized callback for the assistant bridge.

        The bridge may call this endpoint inside its multi-turn loop. Composer
        revalidates the active mode and catalog hash. Read-only tools never
        mutate state. Composer-draft-mutating tools may update only the
        in-memory session draft, and only in composer-assistant mode.
        """
        resolved_catalog, expected_hash = _resolve_bridge_tool_context(
            state,
            assistant_mode=request.assistant_mode,
        )
        if request.catalog_hash != expected_hash:
            raise HTTPException(
                status_code=409,
                detail="catalogHash does not match the active assistantMode catalog",
            )

        allowed_tool_ids = {
            tool.get("id")
            for tool in resolved_catalog["tools"]
            if isinstance(tool.get("id"), str)
        }
        if tool_id not in allowed_tool_ids:
            raise HTTPException(
                status_code=403,
                detail=f"tool '{tool_id}' is outside assistantMode '{request.assistant_mode}'",
            )

        deployment = state.deployment
        assert deployment is not None
        tool_definition = deployment.tool_catalog.get(tool_id)
        if tool_definition is None:
            raise HTTPException(status_code=404, detail=f"unknown tool '{tool_id}'")
        if tool_definition.effect == READ_ONLY_EFFECT:
            may_execute = True
        elif (
            tool_definition.effect == COMPOSER_DRAFT_MUTATING_EFFECT
            and request.assistant_mode == COMPOSER_ASSISTANT_MODE
        ):
            may_execute = True
        else:
            may_execute = False
        if not may_execute:
            raise HTTPException(
                status_code=403,
                detail=(
                    f"tool '{tool_id}' has effect '{tool_definition.effect}', "
                    "but bridge callbacks may execute only read_only tools, or "
                    "composer_draft_mutating tools in composer-assistant mode"
                ),
            )

        # Live tool-call streaming: surface this call on the active
        # assistant request as soon as it lands so the UI can render
        # progress without waiting for the full agent loop to finish.
        # We key on conversation_id from the envelope; there is at
        # most one in-flight assistant request per conversation.
        live_started_ns = time.monotonic_ns()
        try:
            result = await _execute_bridge_tool(
                state,
                tool_id=tool_id,
                effect=tool_definition.effect,
                arguments=request.arguments,
                assistant_mode=request.assistant_mode,
                node_allowlist=set(resolved_catalog["nodes"]),
            )
            success = True
            error = None
        except HTTPException as exc:
            # Compose a recovery envelope on top of the validator's
            # diagnostic before surfacing to the model. Four pieces:
            # (1) the diagnostic itself, (2) a class-specific Action
            # clause picked by classifying the detail, (3) a rotating
            # nudge that varies between consecutive failures so a
            # repeat-call loop has a different prefix each turn, and
            # (4) after several failures, a fresh program-tree
            # snapshot so the model can replan against current state
            # without burning a tool call on program_read. The counter
            # is per-conversation; it resets on any successful call.
            # When the counter crosses IMPASSE_FAILURE_THRESHOLD the
            # endpoint emits a structural-impasse response that
            # instructs the agent loop to halt and reply to the user.
            conversation_id = request.conversation_id or ""
            failure_index = state.bump_assistant_failure(conversation_id)
            error_class = assistant_recovery.classify_detail(exc.detail)
            # context_aware_action overrides the generic ACTION_BY_CLASS
            # entry when the (tool_id, arguments, error_class) triple
            # matches a known intent trap (e.g. arity_insufficient on
            # @root with an empty decorator → wrap_node hint). Falls
            # back to the generic action lookup.
            action = assistant_recovery.context_aware_action(
                error_class=error_class,
                tool_id=tool_id,
                arguments=request.arguments,
            ) or assistant_recovery.action_for_class(error_class)
            nudge = assistant_recovery.nudge_for_index(failure_index)
            # Build a classified history slice for the layered nudges
            # to walk. Each entry carries (tool, status, error_class)
            # so the detectors can analyse repetition patterns without
            # re-classifying. Append the current failure so the
            # detectors include the call we're about to surface.
            raw_history = state.get_assistant_tool_history(
                conversation_id=conversation_id,
                principal=request.principal or "",
            )
            classified_history: list[dict[str, Any]] = []
            for entry in raw_history:
                err = entry.get("error")
                cls = (
                    assistant_recovery.classify_detail(err) if err else None
                )
                classified_history.append({
                    "tool": entry.get("tool"),
                    "status": entry.get("status"),
                    "error": err,
                    "error_class": cls,
                })
            classified_history.append({
                "tool": tool_id,
                "status": "failed",
                "error": exc.detail,
                "error_class": error_class,
            })
            # Layered nudges. truncation_pattern_nudge and
            # format_stuck_nudge are mutually exclusive — the more
            # specific truncation nudge wins when both match.
            # meta_nudge stacks on top once enough failures accumulate.
            extra_lines: list[str] = []
            truncation_line = assistant_recovery.truncation_pattern_nudge(
                classified_history
            )
            if truncation_line:
                extra_lines.append(truncation_line)
            else:
                stuck_line = assistant_recovery.format_stuck_nudge(classified_history)
                if stuck_line:
                    extra_lines.append(stuck_line)
            meta_line = assistant_recovery.meta_nudge(
                classified_history,
                threshold=assistant_recovery.META_NUDGE_AFTER_FAILURES,
            )
            if meta_line:
                extra_lines.append(meta_line)
            enriched_detail = assistant_recovery.append_recovery(
                exc.detail, action, nudge, extra_lines=extra_lines
            )
            # After repeated failures (>= ATTACH_SNAPSHOT_AFTER_FAILURES)
            # attach a fresh program snapshot to the dict-shaped error
            # so the model has current state inline without burning a
            # tool call on program_read. Coerce a string detail into
            # a dict envelope when needed; the dict form is safe for
            # the bridge MCP wrapper.
            if (
                failure_index >= assistant_recovery.ATTACH_SNAPSHOT_AFTER_FAILURES
                and state.program is not None
            ):
                snapshot_envelope: dict[str, Any]
                if isinstance(enriched_detail, dict):
                    snapshot_envelope = enriched_detail
                else:
                    snapshot_envelope = {"message": enriched_detail}
                snapshot_envelope["programSnapshot"] = {
                    "programLoaded": True,
                    "programSourcePath": state.program_source_path,
                    "programSessionGeneration": state.program_session_generation,
                    "programTreeHash": state.program_tree_hash,
                    "program": state.program.to_dict(),
                }
                snapshot_envelope["snapshotReason"] = (
                    "Attached after repeated failures so the next call "
                    "can plan against current state without a separate "
                    "program_read."
                )
                enriched_detail = snapshot_envelope
            # Hard cap (IMPASSE_FAILURE_THRESHOLD): tell the agent to
            # stop and explain to the
            # user. We compose a synthetic response shape rather than
            # raising, so the OpenClaw lane sees a structured message
            # in the tool-call result and the agent loop terminates
            # cleanly instead of bouncing on the cap.
            impasse_envelope: dict[str, Any] | None = None
            if failure_index >= assistant_recovery.IMPASSE_FAILURE_THRESHOLD:
                tool_history = state.get_assistant_tool_history(
                    conversation_id=conversation_id,
                    principal=request.principal or "",
                )
                # Classify each historical error to summarise patterns.
                annotated_history: list[dict[str, Any]] = []
                for entry in tool_history:
                    err_class = (
                        assistant_recovery.classify_detail(entry.get("error"))
                        if entry.get("error")
                        else None
                    )
                    annotated_history.append(
                        {
                            "tool": entry.get("tool"),
                            "status": entry.get("status"),
                            "error_class": err_class,
                        }
                    )
                impasse_envelope = {
                    "message": assistant_recovery.impasse_message(
                        failure_count=failure_index,
                        tool_history=annotated_history,
                    ),
                    "structuralImpasse": True,
                    "failureCount": failure_index,
                    "originalError": enriched_detail
                    if isinstance(enriched_detail, dict)
                    else {"message": enriched_detail},
                }
                # Also reset the counter so a follow-up turn starts
                # fresh; the cap should fire once per stuck turn, not
                # cascade into the next.
                state.reset_assistant_failure(conversation_id)
                enriched_detail = impasse_envelope
            error_text = (
                enriched_detail
                if isinstance(enriched_detail, str)
                else str(enriched_detail)
            )
            state.record_assistant_request_tool_call(
                conversation_id=conversation_id,
                principal=request.principal or "",
                tool_id=tool_id,
                status="failed",
                args_summary=_summarize_for_live(request.arguments),
                error=error_text or "rejected by policy/validation",
                started_at_ns=live_started_ns,
            )
            # OpenClaw 2026.4.24+ applies `erroredAssistantResultPolicy:
            # "drop"` (hardcoded in src/agents/embedded-agent-runner/run/
            # attempt.ts:3102-3107 as of 2026.5.22): when the upstream
            # returns 4xx, the agent turn that triggered the tool call
            # is silently REMOVED from chat history along with its
            # result. The model then loses the validator's feedback and
            # repeats the same bad call.
            #
            # To work around this without forking OpenClaw, the
            # composer MCP returns HTTP 200 with a structured
            # `success: false` envelope for ALL validation failures —
            # not just the structural impasse path. The MCP bridge
            # treats this as a successful tool call producing an
            # error-shaped result; OpenClaw keeps the turn in history
            # and the model can read the error and adjust.
            #
            # The impasse envelope (which provides a tree snapshot +
            # rotating nudge) is still preferred when available; falls
            # back to the enriched detail.
            envelope = impasse_envelope if impasse_envelope is not None else {
                "kind": "validation_error",
                "httpStatus": exc.status_code,
                "message": (
                    enriched_detail.get("message", str(enriched_detail))
                    if isinstance(enriched_detail, dict)
                    else str(enriched_detail)
                ),
                "detail": enriched_detail,
            }
            return {
                "toolId": tool_id,
                "success": False,
                "result": envelope,
                "error": (
                    envelope.get("message", "")
                    if isinstance(envelope, dict)
                    else str(envelope)
                ),
                "mutated": False,
                "draftMutated": False,
                "timestampNs": time.monotonic_ns(),
            }
        except Exception as exc:
            result = {}
            success = False
            error = str(exc)

        # On any successful call, clear the consecutive-failure counter
        # so future failures start the rotation fresh. record_assistant
        # _request_tool_call below still surfaces the call in the live
        # feed regardless of success.
        if success:
            state.reset_assistant_failure(request.conversation_id or "")
        state.record_assistant_request_tool_call(
            conversation_id=request.conversation_id or "",
            principal=request.principal or "",
            tool_id=tool_id,
            status="completed" if success else "failed",
            args_summary=_summarize_for_live(request.arguments),
            result_summary=_summarize_for_live(result),
            error=error,
            draft_mutated=(
                tool_definition.effect == COMPOSER_DRAFT_MUTATING_EFFECT and success
            ),
            started_at_ns=live_started_ns,
        )

        # Attach a delta block so the wrapper passes fresh tree+scene
        # state confirmation back to the model on every tool call.
        # The delta is intentionally embedded inside `result` rather
        # than added as a top-level field — the response model
        # (AssistantBridgeToolResponse) is strict and would strip an
        # unknown top-level key, but `result` is `Dict[str, Any]` and
        # accepts arbitrary nested data. The wrapper forwards `result`
        # verbatim into its MCP tool-result content, so the model sees
        # `result.delta.{rootName, rootKind, sinceCallStart, …}`.
        try:
            current_program = (
                state.program.to_dict() if state.program is not None else None
            )
            delta_block = _assistant_delta.compute_delta(
                conversation_id=request.conversation_id or "",
                current_program_dict=current_program,
            )
            if isinstance(result, dict):
                result["delta"] = delta_block
            else:
                result = {"value": result, "delta": delta_block}
        except Exception:
            # Delta is advisory; never break a tool call on it.
            pass

        return {
            "toolId": tool_id,
            "success": success,
            "result": result,
            "error": error,
            "mutated": tool_definition.effect == COMPOSER_DRAFT_MUTATING_EFFECT and success,
            "draftMutated": tool_definition.effect == COMPOSER_DRAFT_MUTATING_EFFECT and success,
            "timestampNs": time.monotonic_ns(),
        }

    @router.post(
        "/tools/inspect-isaac-scene",
        response_model=AssistantInspectIsaacSceneResponse,
        response_model_by_alias=True,
    )
    async def inspect_isaac_scene(request: AssistantInspectIsaacSceneRequest) -> dict[str, Any]:
        """Inspect Isaac Sim through the validated ROS client without mutating state."""
        try:
            report = await run_in_threadpool(inspect_isaac_scene_report, request)
        except AssistantToolError as exc:
            raise HTTPException(status_code=503, detail=str(exc))
        except TimeoutError as exc:
            raise HTTPException(status_code=504, detail=str(exc))
        except Exception as exc:
            raise HTTPException(status_code=502, detail=str(exc))
        return {
            "toolName": "inspect_isaac_scene",
            "success": True,
            "report": report,
            "summary": report.get("summary", {}) if isinstance(report, dict) else {},
            "warnings": report.get("warnings", []) if isinstance(report, dict) else [],
            "mutated": False,
            "timestampNs": time.monotonic_ns(),
        }

    @router.post(
        "/tools/propose-scene-objects",
        response_model=AssistantProposeSceneObjectsResponse,
        response_model_by_alias=True,
    )
    def propose_scene_objects(request: AssistantProposeSceneObjectsRequest) -> dict[str, Any]:
        """Convert inspected Isaac objects into reviewable scene-resource drafts."""
        try:
            return propose_scene_object_resources(
                request.inspection_report,
                source_roots=request.source_roots,
                object_id_prefix=request.object_id_prefix,
                mesh_resource_overrides=request.mesh_resource_overrides,
                max_proposals=request.max_proposals,
            )
        except AssistantToolError as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post(
        "/tools/propose-scene-object-nodes",
        response_model=AssistantProposeSceneObjectNodesResponse,
        response_model_by_alias=True,
    )
    def propose_scene_nodes(request: AssistantProposeSceneObjectNodesRequest) -> dict[str, Any]:
        """Build reviewable tree-node drafts for scene object operations."""
        try:
            return propose_scene_object_nodes(
                [
                    resource.model_dump(by_alias=True)
                    for resource in request.scene_resources
                ],
                operations=request.operations,
                link_frame=request.link_frame,
                gripper_id=request.gripper_id,
                name_prefix=request.name_prefix,
            )
        except AssistantToolError as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    @router.post(
        "/proposals/stage",
        response_model=AssistantProposalStageResponse,
        response_model_by_alias=True,
    )
    def stage_proposals(request: AssistantProposalStageRequest) -> dict[str, Any]:
        """Stage assistant proposals for later review without changing the program."""
        proposal_dicts = [
            _validated_proposal_payload(proposal.model_dump(by_alias=True))
            for proposal in request.proposals
        ]
        staged = state.stage_assistant_proposals(
            conversation_id=request.conversation_id,
            proposals=proposal_dicts,
            source=request.source,
        )
        return {
            "conversationId": request.conversation_id,
            "proposals": [_proposal_record_payload(record) for record in staged],
            "mutated": False,
            "programMutated": False,
            "timestampNs": time.monotonic_ns(),
        }

    @router.get(
        "/proposals",
        response_model=AssistantProposalListResponse,
        response_model_by_alias=True,
    )
    def list_proposals(
        lifecycle_status: str | None = Query(default=None, alias="lifecycleStatus"),
        conversation_id: str | None = Query(default=None, alias="conversationId"),
    ) -> dict[str, Any]:
        """List staged assistant proposals."""
        records = state.list_assistant_proposals(
            lifecycle_status=lifecycle_status,
            conversation_id=conversation_id,
        )
        return {
            "proposals": [_proposal_record_payload(record) for record in records],
            "timestampNs": time.monotonic_ns(),
        }

    @router.post(
        "/proposals/{proposal_id}/discard",
        response_model=AssistantProposalRecordResponse,
        response_model_by_alias=True,
    )
    def discard_proposal(
        proposal_id: str,
        request: AssistantProposalDiscardRequest,
    ) -> dict[str, Any]:
        """Mark a staged proposal as discarded."""
        record = state.discard_assistant_proposal(proposal_id, reason=request.reason)
        if record is None:
            raise HTTPException(status_code=404, detail="Assistant proposal not found")
        return _proposal_record_payload(record)

    @router.post(
        "/proposals/{proposal_id}/materialize",
        response_model=AssistantProposalMaterializeResponse,
        response_model_by_alias=True,
    )
    def materialize_proposal(proposal_id: str) -> dict[str, Any]:
        """Convert a staged proposal into a validated session patch.

        The returned patch is intended for Composer's normal apply/discard flow.
        This endpoint does not mutate the program.
        """
        record = state.get_assistant_proposal(proposal_id)
        if record is None:
            raise HTTPException(status_code=404, detail="Assistant proposal not found")
        if record.lifecycle_status != "staged":
            raise HTTPException(status_code=409, detail="Assistant proposal is not staged")

        proposal = record.proposal
        if proposal.get("type") != "scene_resource_add":
            raise HTTPException(
                status_code=400,
                detail=f"Unsupported proposal type for materialization: {proposal.get('type')}",
            )
        session_patch = _materialize_scene_resource_add(state, proposal)
        return {
            "proposalId": record.proposal_id,
            "proposal": proposal,
            "sessionPatch": session_patch,
            "mutated": False,
            "programMutated": False,
            "timestampNs": time.monotonic_ns(),
        }

    return router


def _compute_assistant_catalog_hash(catalog: dict[str, list[str]], mode: str) -> str:
    """Composer-side wrapper around the shared protocol helper.

    Kept as a thin wrapper so the existing Composer call sites do not
    have to reach across packages directly; the actual hashing logic
    lives in `manyforge_assistant_bridge.protocol.compute_catalog_hash`,
    the single source of truth for the catalog-freshness contract.
    """
    return compute_catalog_hash(catalog, mode)


def _catalog_entry_payload(entry: Any, *, include_parameters: bool = True) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "id": entry.id,
        "name": entry.name,
        "category": entry.category,
        "description": entry.description,
        "stability": entry.stability,
        "kind": entry.kind,
        "childrenMin": entry.children_min,
        "childrenMax": entry.children_max,
        "bbReaderParams": list(entry.bb_reader_params),
        "bbWriterParams": list(entry.bb_writer_params),
        "requiredCapabilities": list(entry.required_capabilities),
        "profilesMin": entry.profiles_min,
        "interruptible": entry.interruptible,
        "defaultInterruptScope": entry.default_interrupt_scope,
        # swap_class and swap_param_aliases drive tree_draft_change_node_kind's
        # compatibility check. Surfacing them on the catalog payload
        # means the model can plan swaps from the same data structure
        # it gets back from catalog_read, with no skew between the
        # preamble snapshot and the live tool call.
        "swapClass": entry.swap_class,
        "swapParamAliases": dict(entry.swap_param_aliases) or None,
    }
    if include_parameters:
        payload["parameters"] = [
            {
                "name": param.name,
                "type": param.type,
                "required": param.required,
                "default": param.default,
                "enumValues": list(param.enum_values),
                "enumOptions": [
                    {
                        "value": option.value,
                        "label": option.label,
                        "description": option.description,
                        "profilesMin": option.profiles_min,
                        "requiredCapabilities": list(option.required_capabilities),
                    }
                    for option in getattr(param, "enum_options", [])
                ],
                "constraints": dict(param.constraints),
                "parameterizable": param.parameterizable,
                "profilesMin": param.profiles_min,
                "visibleWhen": (
                    {
                        "param": param.visible_when.param,
                        "equals": param.visible_when.equals,
                    }
                    if param.visible_when is not None
                    else None
                ),
                "uiEditor": param.ui_editor,
                "blackboardRole": param.blackboard_role,
                "blackboardContract": param.blackboard_contract,
                "blackboardValueType": param.blackboard_value_type,
                "description": param.description,
            }
            for param in entry.parameters
        ]
    return payload


def _skills_payload(state: ComposerState, *, skill_ids: list[str] | None, include_bodies: bool) -> list[dict[str, Any]]:
    selected = set(skill_ids or [])
    skills: list[dict[str, Any]] = []
    missing = sorted(selected - {skill.id for skill in state.skill_catalog.skills})
    if missing:
        raise HTTPException(status_code=404, detail=f"Unknown skill IDs: {missing}")
    for skill in state.skill_catalog.skills:
        if selected and skill.id not in selected:
            continue
        payload: dict[str, Any] = {
            "id": skill.id,
            "version": skill.version,
            "name": skill.name,
            "description": skill.description,
            "compatibleProfiles": list(skill.compatible_profiles),
            "requires": list(skill.requires),
            "provides": list(skill.provides),
            "modifies": list(skill.modifies),
            "requiresCapabilities": list(skill.requires_capabilities),
            "requiresTools": list(skill.requires_tools),
            "parameters": [
                {
                    "name": param.name,
                    "type": param.type,
                    "required": param.required,
                    "default": param.default,
                    "description": param.description,
                }
                for param in skill.parameters
            ],
            "example": skill.example,
        }
        if include_bodies:
            payload["body"] = skill.body
            payload["expandsTo"] = skill.expands_to
        skills.append(payload)
    return skills


def _deployment_capabilities_payload(state: ComposerState) -> dict[str, Any]:
    if state.deployment is None:
        raise HTTPException(status_code=404, detail="No deployment loaded")
    by_category: dict[str, set[str]] = {
        "planning": set(),
        "context": set(),
        "scene": set(),
        "execution": set(),
        "sensing": set(),
        "other": set(),
    }
    adapter_ids: dict[str, str] = {}
    all_keys: set[str] = set()
    for slot, adapter in state.deployment.adapters.items():
        adapter_ids[slot] = adapter.adapter_id
        for key in adapter.capability_keys:
            all_keys.add(key)
            prefix = key.split(".", 1)[0]
            by_category[prefix if prefix in by_category else "other"].add(key)
    return {
        "programProfile": state.deployment.program_profile,
        "adapterIds": adapter_ids,
        "capabilityKeys": sorted(all_keys),
        "planning": sorted(by_category["planning"]),
        "context": sorted(by_category["context"]),
        "scene": sorted(by_category["scene"]),
        "execution": sorted(by_category["execution"]),
        "sensing": sorted(by_category["sensing"]),
        "other": sorted(by_category["other"]),
        "timestampNs": time.monotonic_ns(),
    }


_LIVE_SUMMARY_MAX_CHARS = 240


def _summarize_for_live(value: Any) -> str:
    """Compact JSON summary suitable for the live-stream UI feed.

    The summary is truncated and never includes raw tool result bodies
    that may be large. The full audit log keeps the unredacted record;
    this is for at-a-glance "what's happening now".
    """
    if value is None:
        return ""
    try:
        rendered = json.dumps(value, separators=(",", ":"), default=str)
    except Exception:
        rendered = str(value)
    if len(rendered) > _LIVE_SUMMARY_MAX_CHARS:
        return rendered[: _LIVE_SUMMARY_MAX_CHARS - 1] + "…"
    return rendered


def _live_tool_call_payload(entry: Any) -> dict[str, Any]:
    return {
        "toolId": entry.tool_id,
        "status": entry.status,
        "startedAtNs": entry.started_at_ns,
        "finishedAtNs": entry.finished_at_ns,
        "argsSummary": entry.args_summary,
        "resultSummary": entry.result_summary,
        "error": entry.error,
        "draftMutated": entry.draft_mutated,
    }


def _assistant_request_payload(record: Any) -> dict[str, Any]:
    return {
        "requestId": record.request_id,
        "conversationId": record.conversation_id,
        "providerId": record.provider_id,
        "assistantMode": record.assistant_mode,
        "status": record.status,
        "message": record.message,
        "error": record.error,
        "warnings": list(record.warnings),
        "draftMutated": record.draft_mutated,
        "toolCallCount": record.tool_call_count,
        "recentToolCalls": [_live_tool_call_payload(c) for c in record.tool_calls],
        "startedAtNs": record.started_at_ns,
        "updatedAtNs": record.updated_at_ns,
        "finishedAtNs": record.finished_at_ns,
    }


def _provider_error_status_code(exc: AssistantProviderError) -> int:
    if exc.code == "cancelled":
        return 200
    if exc.code == "timeout":
        return 504
    return 502


def _resolve_bridge_tool_context(
    state: ComposerState,
    *,
    assistant_mode: str,
) -> tuple[dict[str, Any], str]:
    deployment = state.deployment
    if deployment is None or not deployment.assistant_modes:
        raise HTTPException(
            status_code=404,
            detail="No deployment assistant_modes are loaded",
        )
    try:
        deployment.validate_assistant_modes(state.catalog, state.skill_catalog)
        resolved_catalog = deployment.resolve_assistant_mode_catalog(assistant_mode)
    except DeploymentValidationError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    catalog_for_hash = {
        "skills": list(resolved_catalog["skills"]),
        "tools": [
            tool.get("id")
            for tool in resolved_catalog["tools"]
            if isinstance(tool.get("id"), str)
        ],
        "nodes": list(resolved_catalog["nodes"]),
    }
    return resolved_catalog, _compute_assistant_catalog_hash(
        catalog_for_hash,
        assistant_mode,
    )


async def _execute_bridge_read_only_tool(
    state: ComposerState,
    *,
    tool_id: str,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    if tool_id == "catalog_read":
        entry_ids = arguments.get("entryIds") or arguments.get("entry_ids") or []
        if entry_ids is None:
            entry_ids = []
        if not isinstance(entry_ids, list) or not all(isinstance(item, str) for item in entry_ids):
            raise HTTPException(status_code=400, detail="catalog_read entryIds must be a list of strings")
        category = arguments.get("category")
        kind = arguments.get("kind")
        include_parameters = arguments.get("includeParameters", arguments.get("include_parameters", True))
        if not isinstance(include_parameters, bool):
            raise HTTPException(status_code=400, detail="catalog_read includeParameters must be boolean")
        selected = set(entry_ids)
        missing = sorted(selected - {entry.id for entry in state.catalog.entries})
        if missing:
            raise HTTPException(status_code=404, detail=f"Unknown catalog entry IDs: {missing}")
        entries = []
        for entry in state.catalog.entries:
            if selected and entry.id not in selected:
                continue
            if isinstance(category, str) and category and entry.category != category:
                continue
            if isinstance(kind, str) and kind and entry.kind != kind:
                continue
            entries.append(_catalog_entry_payload(entry, include_parameters=include_parameters))
        return {
            "version": state.catalog.version,
            "entryCount": len(entries),
            "entries": entries,
        }

    if tool_id == "skills_read":
        skill_ids = arguments.get("skillIds") or arguments.get("skill_ids") or []
        if skill_ids is None:
            skill_ids = []
        if not isinstance(skill_ids, list) or not all(isinstance(item, str) for item in skill_ids):
            raise HTTPException(status_code=400, detail="skills_read skillIds must be a list of strings")
        include_bodies = arguments.get("includeBodies", arguments.get("include_bodies", False))
        if not isinstance(include_bodies, bool):
            raise HTTPException(status_code=400, detail="skills_read includeBodies must be boolean")
        skills = _skills_payload(state, skill_ids=skill_ids, include_bodies=include_bodies)
        return {
            "skillCount": len(skills),
            "skills": skills,
            "timestampNs": time.monotonic_ns(),
        }

    if tool_id == "deployment_capabilities_read":
        return _deployment_capabilities_payload(state)

    if tool_id == "program_read":
        return {
            "programLoaded": state.program is not None,
            "programSourcePath": state.program_source_path,
            "programSessionGeneration": state.program_session_generation,
            "programTreeHash": state.program_tree_hash,
            "program": state.program.to_dict() if state.program is not None else None,
        }

    if tool_id == "program_validate":
        issues: list[dict[str, str]] = []
        if state.program is None:
            issues.append({"path": "program", "message": "No program loaded"})
        else:
            try:
                candidate = Program.from_dict(
                    state.program.to_dict(),
                    state.catalog,
                    path=state.program_source_path or "<memory>",
                    skill_catalog=state.skill_catalog,
                    deployment_capability_keys=_deployment_capability_keys(state),
                )
                if state.deployment is not None:
                    state.deployment.validate_against_program(candidate)
                    state.deployment.validate_capabilities(candidate)
            except (ProgramValidationError, DeploymentValidationError) as exc:
                issues.append({"path": "program", "message": str(exc)})
        return {"valid": not issues, "issues": issues}

    if tool_id == "status_read":
        return {
            "programLoaded": state.program is not None,
            "cycleState": state.cycle_manager.state.value if state.cycle_manager else "idle",
            "programSessionGeneration": state.program_session_generation,
            "programTreeHash": state.program_tree_hash,
            "runtimeOverrides": {
                node_name: {
                    "authoritySource": record.authority_source,
                    "approvalMode": record.approval_mode,
                    "activationState": record.activation_state,
                }
                for node_name, record in state.runtime_overrides.items()
            },
        }

    if tool_id == "scene_inspect":
        return build_scene_state_payload(state)

    if tool_id == "inspect_isaac_scene":
        request = AssistantInspectIsaacSceneRequest.model_validate(arguments or {})
        try:
            report = await run_in_threadpool(inspect_isaac_scene_report, request)
        except AssistantToolError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc
        except TimeoutError as exc:
            raise HTTPException(status_code=504, detail=str(exc)) from exc
        return {
            "report": report,
            "summary": report.get("summary", {}) if isinstance(report, dict) else {},
            "warnings": report.get("warnings", []) if isinstance(report, dict) else [],
        }

    raise HTTPException(
        status_code=404,
        detail=f"read-only bridge callback is not implemented for tool '{tool_id}'",
    )


async def _execute_bridge_tool(
    state: ComposerState,
    *,
    tool_id: str,
    effect: str,
    arguments: dict[str, Any],
    assistant_mode: str | None = None,
    node_allowlist: set[str] | None = None,
) -> dict[str, Any]:
    if effect == READ_ONLY_EFFECT:
        return await _execute_bridge_read_only_tool(
            state,
            tool_id=tool_id,
            arguments=arguments,
        )
    if effect == COMPOSER_DRAFT_MUTATING_EFFECT:
        return _execute_bridge_composer_draft_tool(
            state,
            tool_id=tool_id,
            arguments=arguments,
            assistant_mode=assistant_mode,
            node_allowlist=node_allowlist,
        )
    raise HTTPException(
        status_code=403,
        detail=f"bridge callback effect is not executable: {effect}",
    )


def _execute_bridge_composer_draft_tool(
    state: ComposerState,
    *,
    tool_id: str,
    arguments: dict[str, Any],
    assistant_mode: str | None = None,
    node_allowlist: set[str] | None = None,
) -> dict[str, Any]:
    if tool_id == "scene_draft_add_object":
        return _apply_scene_draft_resource_single(
            state,
            arguments=arguments,
            operation="add",
        )
    if tool_id == "scene_draft_update_object":
        return _apply_scene_draft_resource_single(
            state,
            arguments=arguments,
            operation="update",
        )
    if tool_id == "scene_draft_upsert_objects":
        return _apply_scene_draft_resources_upsert(state, arguments=arguments)
    if tool_id == "scene_draft_remove_objects":
        return _apply_scene_draft_resources_remove(state, arguments=arguments)
    if tool_id == "tree_draft_insert_node":
        return _apply_tree_draft_insert_node(
            state,
            arguments=arguments,
            node_allowlist=node_allowlist,
            assistant_mode=assistant_mode,
        )
    if tool_id == "tree_draft_update_node_params":
        return _apply_tree_draft_update_node_params(state, arguments=arguments)
    if tool_id == "tree_draft_delete_node":
        return _apply_tree_draft_delete_node(state, arguments=arguments)
    if tool_id == "tree_draft_move_node":
        return _apply_tree_draft_move_node(state, arguments=arguments)
    if tool_id == "tree_draft_replace_subtree":
        return _apply_tree_draft_replace_subtree(
            state,
            arguments=arguments,
            node_allowlist=node_allowlist,
            assistant_mode=assistant_mode,
        )
    if tool_id == "tree_draft_change_node_kind":
        return _apply_tree_draft_change_node_kind(
            state,
            arguments=arguments,
            node_allowlist=node_allowlist,
            assistant_mode=assistant_mode,
        )
    if tool_id == "tree_draft_wrap_node":
        return _apply_tree_draft_wrap_node(
            state,
            arguments=arguments,
            node_allowlist=node_allowlist,
            assistant_mode=assistant_mode,
        )
    if tool_id == "program_draft_upsert_parameters":
        return _apply_program_draft_upsert_parameters(state, arguments=arguments)
    if tool_id == "program_draft_remove_parameters":
        return _apply_program_draft_remove_parameters(state, arguments=arguments)
    if tool_id == "blackboard_draft_upsert_namespaces":
        return _apply_blackboard_draft_upsert_namespaces(state, arguments=arguments)
    if tool_id == "blackboard_draft_remove_namespaces":
        return _apply_blackboard_draft_remove_namespaces(state, arguments=arguments)
    if tool_id == "blackboard_draft_upsert_keys":
        return _apply_blackboard_draft_upsert_keys(state, arguments=arguments)
    if tool_id == "blackboard_draft_remove_keys":
        return _apply_blackboard_draft_remove_keys(state, arguments=arguments)
    raise HTTPException(
        status_code=404,
        detail=f"composer draft bridge callback is not implemented for tool '{tool_id}'",
    )


def _scene_resource_from_draft_arguments(
    state: ComposerState,
    arguments: dict[str, Any],
    *,
    tool_id: str,
) -> ProgramSceneResourceResponse:
    raw_resource = _single_scene_resource_argument(arguments, tool_id=tool_id)
    normalized_resource = _normalize_scene_resource_argument(
        state,
        raw_resource,
        tool_id=tool_id,
        label="sceneResource",
    )
    try:
        resource = ProgramSceneResourceResponse.model_validate(normalized_resource)
    except ValidationError as exc:
        raise HTTPException(
            status_code=400,
            detail=f"{tool_id} sceneResource is invalid: {exc}",
        ) from exc
    return _normalize_scene_draft_resource_frame(
        state=state,
        resource=resource,
        tool_id=tool_id,
        label="sceneResource",
    )


def _scene_resources_from_upsert_arguments(
    state: ComposerState,
    arguments: dict[str, Any],
) -> list[ProgramSceneResourceResponse]:
    raw_resources = (
        arguments.get("sceneResources")
        or arguments.get("scene_resources")
        or arguments.get("resources")
    )
    if not isinstance(raw_resources, list) or not raw_resources:
        raise HTTPException(
            status_code=400,
            detail="scene_draft_upsert_objects requires a non-empty sceneResources list",
        )

    resources: list[ProgramSceneResourceResponse] = []
    seen: set[str] = set()
    duplicates: set[str] = set()
    for index, raw_resource in enumerate(raw_resources):
        raw_resource = _single_scene_resource_argument(
            raw_resource,
            tool_id="scene_draft_upsert_objects",
            label=f"sceneResources[{index}]",
        )
        normalized_resource = _normalize_scene_resource_argument(
            state,
            raw_resource,
            tool_id="scene_draft_upsert_objects",
            label=f"sceneResources[{index}]",
        )
        try:
            resource = ProgramSceneResourceResponse.model_validate(normalized_resource)
        except ValidationError as exc:
            raise HTTPException(
                status_code=400,
                detail=f"sceneResources[{index}] is invalid: {exc}",
            ) from exc
        resource = _normalize_scene_draft_resource_frame(
            state=state,
            resource=resource,
            tool_id="scene_draft_upsert_objects",
            label=f"sceneResources[{index}]",
        )
        if resource.object_id in seen:
            duplicates.add(resource.object_id)
        seen.add(resource.object_id)
        resources.append(resource)

    if duplicates:
        raise HTTPException(
            status_code=400,
            detail=f"scene_draft_upsert_objects duplicate objectId values: {sorted(duplicates)}",
        )
    return resources


def _single_scene_resource_argument(
    value: Any,
    *,
    tool_id: str,
    label: str = "sceneResource",
) -> dict[str, Any]:
    if isinstance(value, dict):
        found_wrapper = False
        for key in ("sceneResource", "scene_resource", "resource", "object"):
            if key in value:
                value = value[key]
                found_wrapper = True
                break
        if not found_wrapper and "value" in value and len(value) == 1:
            value = value["value"]
    if isinstance(value, list):
        if len(value) == 1 and isinstance(value[0], dict):
            value = value[0]
        else:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label} must be a single scene object, not a list",
            )
    if not isinstance(value, dict):
        raise HTTPException(
            status_code=400,
            detail=f"{tool_id} {label} must be an object",
        )
    return value


def _first_mapping_value(mapping: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in mapping and mapping[key] is not None:
            return mapping[key]
    return None


def _float_argument(value: Any, *, label: str) -> float:
    if isinstance(value, bool):
        raise HTTPException(status_code=400, detail=f"{label} must be numeric")
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=f"{label} must be numeric") from exc
    raise HTTPException(status_code=400, detail=f"{label} must be numeric")


def _numeric_vector_argument(value: Any, *, length: int, label: str) -> list[float]:
    if isinstance(value, dict):
        keys = ("x", "y", "z", "w")[:length]
        if all(key in value for key in keys):
            value = [value[key] for key in keys]
    elif isinstance(value, str):
        tokens = re.findall(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", value)
        if len(tokens) == length:
            value = tokens
    if not isinstance(value, list) or len(value) != length:
        raise HTTPException(
            status_code=400,
            detail=f"{label} must be a list of {length} numeric values",
        )
    return [
        _float_argument(component, label=f"{label}[{index}]")
        for index, component in enumerate(value)
    ]


def _normalize_scene_resource_argument(
    state: ComposerState,
    raw_resource: dict[str, Any],
    *,
    tool_id: str,
    label: str,
) -> dict[str, Any]:
    object_id = _first_mapping_value(raw_resource, "objectId", "object_id", "id", "name")
    if not isinstance(object_id, str) or not object_id.strip():
        raise HTTPException(status_code=400, detail=f"{tool_id} {label}.objectId is required")

    shape = _normalize_scene_shape_argument(raw_resource, tool_id=tool_id, label=label)
    pose = _normalize_scene_pose_argument(state, raw_resource, tool_id=tool_id, label=label)
    return {
        "objectId": object_id.strip(),
        "shape": shape,
        "pose": pose,
    }


def _normalize_scene_shape_argument(
    raw_resource: dict[str, Any],
    *,
    tool_id: str,
    label: str,
) -> dict[str, Any]:
    raw_shape = raw_resource.get("shape") if isinstance(raw_resource.get("shape"), dict) else {}
    assert isinstance(raw_shape, dict)

    shape_type = _first_mapping_value(
        raw_shape,
        "type",
        "shapeType",
        "shape_type",
    )
    if shape_type is None:
        shape_type = _first_mapping_value(raw_resource, "shapeType", "shape_type", "type")
    shape_type_normalized = str(shape_type).strip().lower() if shape_type is not None else ""

    box_dimensions = _first_mapping_value(
        raw_shape,
        "box_dimensions_m",
        "boxDimensionsM",
        "dimensions_m",
        "dimensions",
        "size_m",
        "size",
    )
    if box_dimensions is None:
        box_dimensions = _first_mapping_value(
            raw_resource,
            "box_dimensions_m",
            "boxDimensionsM",
            "dimensions_m",
            "dimensions",
            "size_m",
            "size",
        )

    radius = _first_mapping_value(
        raw_shape,
        "sphere_radius_m",
        "sphereRadiusM",
        "radius_m",
        "radius",
    )
    if radius is None:
        radius = _first_mapping_value(
            raw_resource,
            "sphere_radius_m",
            "sphereRadiusM",
            "radius_m",
            "radius",
        )
    diameter = _first_mapping_value(raw_shape, "diameter_m", "diameter")
    if diameter is None:
        diameter = _first_mapping_value(raw_resource, "diameter_m", "diameter")

    cylinder_radius = _first_mapping_value(
        raw_shape,
        "cylinder_radius_m",
        "cylinderRadiusM",
        "radius_m",
        "radius",
    )
    if cylinder_radius is None:
        cylinder_radius = _first_mapping_value(
            raw_resource,
            "cylinder_radius_m",
            "cylinderRadiusM",
            "radius_m",
            "radius",
        )
    cylinder_diameter = _first_mapping_value(
        raw_shape,
        "cylinder_diameter_m",
        "cylinderDiameterM",
    )
    if cylinder_diameter is None:
        cylinder_diameter = _first_mapping_value(
            raw_resource,
            "cylinder_diameter_m",
            "cylinderDiameterM",
        )
    cylinder_height = _first_mapping_value(
        raw_shape,
        "cylinder_height_m",
        "cylinderHeightM",
        "height_m",
        "height",
    )
    if cylinder_height is None:
        cylinder_height = _first_mapping_value(
            raw_resource,
            "cylinder_height_m",
            "cylinderHeightM",
            "height_m",
            "height",
        )

    mesh_resource_uri = _first_mapping_value(
        raw_shape,
        "mesh_resource_uri",
        "meshResourceUri",
        "resource_uri",
        "resourceUri",
        "uri",
        "path",
        "mesh_path",
        "meshPath",
    )
    if mesh_resource_uri is None:
        mesh_resource_uri = _first_mapping_value(
            raw_resource,
            "mesh_resource_uri",
            "meshResourceUri",
            "resource_uri",
            "resourceUri",
            "uri",
            "path",
            "mesh_path",
            "meshPath",
        )

    mesh_scale = _first_mapping_value(raw_shape, "mesh_scale", "meshScale", "scale")
    if mesh_scale is None:
        mesh_scale = _first_mapping_value(raw_resource, "mesh_scale", "meshScale", "scale")

    if not shape_type_normalized:
        if box_dimensions is not None:
            shape_type_normalized = "box"
        elif cylinder_height is not None and (
            cylinder_radius is not None or cylinder_diameter is not None
        ):
            shape_type_normalized = "cylinder"
        elif radius is not None or diameter is not None:
            shape_type_normalized = "sphere"
        elif mesh_resource_uri is not None:
            shape_type_normalized = "mesh"

    if shape_type_normalized in _BOX_SHAPE_ALIASES:
        if box_dimensions is None:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape requires box dimensions",
            )
        return {
            "type": "box",
            "box_dimensions_m": _numeric_vector_argument(
                box_dimensions,
                length=3,
                label=f"{label}.shape.box_dimensions_m",
            ),
        }

    if shape_type_normalized in _SPHERE_SHAPE_ALIASES:
        sphere_radius = radius
        if sphere_radius is None and diameter is not None:
            sphere_radius = _float_argument(diameter, label=f"{label}.shape.diameter") / 2.0
        if sphere_radius is None:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape requires sphere radius or diameter",
            )
        radius_value = _float_argument(sphere_radius, label=f"{label}.shape.sphere_radius_m")
        if radius_value <= 0.0:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape.sphere_radius_m must be > 0",
            )
        return {"type": "sphere", "sphere_radius_m": radius_value}

    if shape_type_normalized in _CYLINDER_SHAPE_ALIASES:
        radius_value = cylinder_radius
        if radius_value is None and cylinder_diameter is not None:
            radius_value = (
                _float_argument(cylinder_diameter, label=f"{label}.shape.cylinder_diameter_m")
                / 2.0
            )
        if radius_value is None:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape requires cylinder radius or diameter",
            )
        if cylinder_height is None:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape requires cylinder height",
            )
        radius_float = _float_argument(radius_value, label=f"{label}.shape.cylinder_radius_m")
        height_float = _float_argument(cylinder_height, label=f"{label}.shape.cylinder_height_m")
        if radius_float <= 0.0:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape.cylinder_radius_m must be > 0",
            )
        if height_float <= 0.0:
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape.cylinder_height_m must be > 0",
            )
        return {
            "type": "cylinder",
            "cylinder_radius_m": radius_float,
            "cylinder_height_m": height_float,
        }

    if shape_type_normalized in _MESH_SHAPE_ALIASES:
        if not isinstance(mesh_resource_uri, str) or not mesh_resource_uri.strip():
            raise HTTPException(
                status_code=400,
                detail=f"{tool_id} {label}.shape requires mesh_resource_uri",
            )
        scale = [1.0, 1.0, 1.0]
        if mesh_scale is not None:
            scale = _numeric_vector_argument(
                mesh_scale,
                length=3,
                label=f"{label}.shape.mesh_scale",
            )
            if any(component <= 0.0 for component in scale):
                raise HTTPException(
                    status_code=400,
                    detail=f"{tool_id} {label}.shape.mesh_scale values must be > 0",
                )
        return {
            "type": "mesh",
            "mesh_resource_uri": mesh_resource_uri.strip(),
            "mesh_scale": scale,
        }

    raise HTTPException(
        status_code=400,
        detail=f"{tool_id} {label}.shape.type must be one of: box, sphere, cylinder, mesh",
    )


def _normalize_scene_pose_argument(
    state: ComposerState,
    raw_resource: dict[str, Any],
    *,
    tool_id: str,
    label: str,
) -> dict[str, Any]:
    raw_pose = raw_resource.get("pose") if isinstance(raw_resource.get("pose"), dict) else {}
    assert isinstance(raw_pose, dict)

    position = _first_mapping_value(raw_pose, "position_m", "positionM", "position", "xyz")
    if position is None:
        position = _first_mapping_value(raw_resource, "position_m", "positionM", "position", "xyz")
    if position is None:
        raise HTTPException(
            status_code=400,
            detail=f"{tool_id} {label}.pose.position_m is required",
        )

    frame_id = _first_mapping_value(raw_pose, "frame_id", "frameId")
    if frame_id is None:
        frame_id = _first_mapping_value(raw_resource, "frame_id", "frameId")
    if not isinstance(frame_id, str) or not frame_id.strip():
        frame_id = canonical_scene_world_frame(state)

    pose: dict[str, Any] = {
        "frame_id": frame_id.strip(),
        "position_m": _numeric_vector_argument(
            position,
            length=3,
            label=f"{label}.pose.position_m",
        ),
    }

    orientation_xyzw = _first_mapping_value(raw_pose, "orientation_xyzw", "orientationXyzw")
    if orientation_xyzw is None:
        orientation_xyzw = _first_mapping_value(raw_resource, "orientation_xyzw", "orientationXyzw")
    orientation_rpy = _first_mapping_value(raw_pose, "orientation_rpy_deg", "orientationRpyDeg", "rpy_deg")
    if orientation_rpy is None:
        orientation_rpy = _first_mapping_value(raw_resource, "orientation_rpy_deg", "orientationRpyDeg", "rpy_deg")
    orientation = _first_mapping_value(raw_pose, "orientation")
    if orientation is None:
        orientation = raw_resource.get("orientation")

    if orientation_xyzw is not None:
        pose["orientation_xyzw"] = _numeric_vector_argument(
            orientation_xyzw,
            length=4,
            label=f"{label}.pose.orientation_xyzw",
        )
    elif orientation_rpy is not None:
        pose["orientation_rpy_deg"] = _numeric_vector_argument(
            orientation_rpy,
            length=3,
            label=f"{label}.pose.orientation_rpy_deg",
        )
    elif orientation is not None:
        if isinstance(orientation, dict) and all(key in orientation for key in ("x", "y", "z", "w")):
            pose["orientation_xyzw"] = _numeric_vector_argument(
                orientation,
                length=4,
                label=f"{label}.pose.orientation_xyzw",
            )
        else:
            pose["orientation_rpy_deg"] = _numeric_vector_argument(
                orientation,
                length=3,
                label=f"{label}.pose.orientation_rpy_deg",
            )
    else:
        pose["orientation_xyzw"] = [0.0, 0.0, 0.0, 1.0]

    return pose


def _normalize_scene_draft_resource_frame(
    state: ComposerState,
    resource: ProgramSceneResourceResponse,
    *,
    tool_id: str,
    label: str,
) -> ProgramSceneResourceResponse:
    """Keep Composer draft scene resources detached and world-authored.

    Attachment is represented by explicit behavior-tree nodes
    (attach_object_to_link / detach_object_from_link), not by giving an
    authored scene resource an arbitrary robot/link frame.
    """
    canonical_world_frame = canonical_scene_world_frame(state)
    data = resource.model_dump(by_alias=True)
    pose = dict(data.get("pose") or {})
    frame_id = pose.get("frame_id")
    if isinstance(frame_id, str):
        frame_id = frame_id.strip()
        if frame_id in _WORLD_FRAME_ALIASES:
            frame_id = canonical_world_frame
        pose["frame_id"] = frame_id
    if pose.get("frame_id") != canonical_world_frame:
        raise HTTPException(
            status_code=400,
            detail=(
                f"{tool_id} {label}.pose.frame_id must be the backend canonical "
                f"world frame '{canonical_world_frame}' for Composer draft scene "
                "resources; use attach_object_to_link or detach_object_from_link "
                "tree nodes for attachment semantics"
            ),
        )
    data["pose"] = pose
    return ProgramSceneResourceResponse.model_validate(data)


def _assert_scene_draft_edit_allowed(state: ComposerState) -> None:
    _assert_program_draft_edit_allowed(state)


def _assert_program_draft_edit_allowed(state: ComposerState) -> None:
    if state.program is None:
        raise HTTPException(status_code=404, detail="No program loaded")
    if state.cycle_manager is not None and state.cycle_manager.state.value in {
        "running",
        "direct_control",
    }:
        raise HTTPException(
            status_code=409,
            detail=(
                "Assistant draft edits are not allowed while execution is "
                f"active (state={state.cycle_manager.state.value})"
            ),
        )


def _force_discard_overrides(arguments: dict[str, Any]) -> bool:
    return bool(
        arguments.get("forceDiscardOverrides")
        or arguments.get("force_discard_overrides")
    )


def _scene_response(scene: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        ProgramSceneResourceResponse(
            objectId=item["object_id"],
            shape=item["shape"],
            pose=item["pose"],
        ).model_dump(by_alias=True)
        for item in scene
    ]


def _resource_dump(resource: ProgramSceneResourceResponse) -> dict[str, Any]:
    return resource.model_dump(by_alias=False)


def _apply_scene_draft_resources(
    state: ComposerState,
    *,
    scene: list[dict[str, Any]],
    force_discard_overrides: bool,
) -> None:
    program_data = state.program.to_dict()
    program_data["scene"] = scene
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=force_discard_overrides,
    )


def _apply_program_draft_data(
    state: ComposerState,
    *,
    program_data: dict[str, Any],
    force_discard_overrides: bool,
) -> Program:
    try:
        candidate = Program.from_dict(
            program_data,
            state.catalog,
            path=state.program_source_path or "<memory>",
            skill_catalog=state.skill_catalog,
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        deployment = state.deployment
        if deployment is not None:
            deployment.validate_against_program(candidate)
            deployment.validate_capabilities(candidate)
        state.apply_models(
            candidate,
            deployment,
            force_discard_overrides=force_discard_overrides,
        )
        return candidate
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except (ProgramValidationError, DeploymentValidationError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


def _program_draft_result(
    state: ComposerState,
    *,
    draft_mutation: str,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    program = state.program
    assert program is not None
    payload: dict[str, Any] = {
        "draftMutation": draft_mutation,
        "programLoaded": True,
        "programSessionGeneration": state.program_session_generation,
        "programTreeHash": state.program_tree_hash,
        "tree": program.tree.to_dict() if program.tree is not None else {},
        "parameters": [
            {
                "name": parameter.name,
                "type": parameter.type,
                "default": parameter.default,
                "constraints": parameter.constraints,
                "description": parameter.description,
            }
            for parameter in program.parameters.values()
        ],
        "blackboard": {
            "namespaces": [
                {
                    "id": namespace.id,
                    "contract": namespace.contract,
                    "prefix": namespace.prefix,
                    "description": namespace.description,
                    "fields": [
                        {
                            "name": field.name,
                            "type": field.type,
                            "description": field.description,
                        }
                        for field in namespace.fields
                    ],
                }
                for namespace in program.blackboard_namespaces.values()
            ],
            "keys": [
                {
                    "id": key.id,
                    "type": key.type,
                    "key": key.key,
                    "description": key.description,
                }
                for key in program.blackboard_keys.values()
            ],
        },
    }
    if extra:
        payload.update(extra)
    return payload


def _apply_scene_draft_resource_single(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
    operation: str,
) -> dict[str, Any]:
    _assert_scene_draft_edit_allowed(state)
    tool_id = f"scene_draft_{operation}_object"
    force_discard_overrides = _force_discard_overrides(arguments)
    resource = _scene_resource_from_draft_arguments(state, arguments, tool_id=tool_id)
    _validate_scene_resource_mesh_boundary(state, resource)
    program_data = state.program.to_dict()
    scene = list(program_data.get("scene") or [])
    scene_index = next(
        (index for index, item in enumerate(scene) if item.get("object_id") == resource.object_id),
        None,
    )

    if operation == "add":
        if scene_index is not None:
            raise HTTPException(
                status_code=409,
                detail=f"Scene object already exists: {resource.object_id}",
            )
        scene.append(_resource_dump(resource))
        inserted = [resource.object_id]
        updated: list[str] = []
        draft_mutation = "scene_resource_add"
    elif operation == "update":
        if scene_index is None:
            raise HTTPException(
                status_code=404,
                detail=f"Scene object does not exist: {resource.object_id}",
            )
        scene[scene_index] = _resource_dump(resource)
        inserted = []
        updated = [resource.object_id]
        draft_mutation = "scene_resource_update"
    else:
        raise HTTPException(status_code=500, detail=f"unsupported single scene draft operation: {operation}")

    _apply_scene_draft_resources(
        state,
        scene=scene,
        force_discard_overrides=force_discard_overrides,
    )
    return {
        "draftMutation": draft_mutation,
        "objectId": resource.object_id,
        "objectIds": [resource.object_id],
        "inserted": inserted,
        "updated": updated,
        "unchanged": [],
        "programLoaded": state.program is not None,
        "programSessionGeneration": state.program_session_generation,
        "programTreeHash": state.program_tree_hash,
        "scene": _scene_response(scene),
    }


def _validate_scene_resource_mesh_boundary(state: ComposerState, resource) -> None:
    shape = getattr(resource, "shape", {}) or {}
    if shape.get("type") != "mesh":
        return
    from .routes_scene_meshes import validate_scene_mesh_resource_or_409

    validate_scene_mesh_resource_or_409(
        state,
        shape.get("mesh_resource_uri"),
        shape.get("mesh_scale"),
    )


def _apply_scene_draft_resources_upsert(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_scene_draft_edit_allowed(state)
    force_discard_overrides = bool(
        arguments.get("forceDiscardOverrides")
        or arguments.get("force_discard_overrides")
    )
    program_data = state.program.to_dict()
    scene = list(program_data.get("scene") or [])
    index_by_id = {
        item.get("object_id"): index
        for index, item in enumerate(scene)
        if isinstance(item.get("object_id"), str)
    }
    resources = _scene_resources_from_upsert_arguments(state, arguments)
    for resource in resources:
        _validate_scene_resource_mesh_boundary(state, resource)
    inserted: list[str] = []
    updated: list[str] = []
    unchanged: list[str] = []

    for resource in resources:
        resource_data = _resource_dump(resource)
        existing_index = index_by_id.get(resource.object_id)
        if existing_index is None:
            index_by_id[resource.object_id] = len(scene)
            scene.append(resource_data)
            inserted.append(resource.object_id)
            continue
        if scene[existing_index] == resource_data:
            unchanged.append(resource.object_id)
            continue
        scene[existing_index] = resource_data
        updated.append(resource.object_id)

    _apply_scene_draft_resources(
        state,
        scene=scene,
        force_discard_overrides=force_discard_overrides,
    )

    return {
        "draftMutation": "scene_resources_upsert",
        "objectIds": [resource.object_id for resource in resources],
        "inserted": inserted,
        "updated": updated,
        "unchanged": unchanged,
        "programLoaded": state.program is not None,
        "programSessionGeneration": state.program_session_generation,
        "programTreeHash": state.program_tree_hash,
        "scene": _scene_response(scene),
    }


def _string_list_argument(arguments: dict[str, Any], *, key: str, tool_id: str) -> list[str]:
    value = arguments.get(key)
    if value is None:
        snake_key = "".join(["_" + char.lower() if char.isupper() else char for char in key]).lstrip("_")
        value = arguments.get(snake_key)
    if not isinstance(value, list) or not value:
        raise HTTPException(status_code=400, detail=f"{tool_id} requires a non-empty {key} list")
    normalized: list[str] = []
    seen: set[str] = set()
    duplicates: set[str] = set()
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item.strip():
            raise HTTPException(status_code=400, detail=f"{key}[{index}] must be a non-empty string")
        item = item.strip()
        if item in seen:
            duplicates.add(item)
        seen.add(item)
        normalized.append(item)
    if duplicates:
        raise HTTPException(status_code=400, detail=f"{tool_id} duplicate {key} values: {sorted(duplicates)}")
    return normalized


def _apply_scene_draft_resources_remove(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_scene_draft_edit_allowed(state)
    force_discard_overrides = _force_discard_overrides(arguments)
    object_ids = _string_list_argument(
        arguments,
        key="objectIds",
        tool_id="scene_draft_remove_objects",
    )
    program_data = state.program.to_dict()
    scene = list(program_data.get("scene") or [])
    existing_ids = {
        item.get("object_id")
        for item in scene
        if isinstance(item.get("object_id"), str)
    }
    missing = sorted(set(object_ids) - existing_ids)
    if missing:
        raise HTTPException(status_code=404, detail=f"Scene objects do not exist: {missing}")
    removed_set = set(object_ids)
    updated_scene = [
        item for item in scene
        if item.get("object_id") not in removed_set
    ]
    _apply_scene_draft_resources(
        state,
        scene=updated_scene,
        force_discard_overrides=force_discard_overrides,
    )
    return {
        "draftMutation": "scene_resources_remove",
        "objectIds": object_ids,
        "removed": object_ids,
        "programLoaded": state.program is not None,
        "programSessionGeneration": state.program_session_generation,
        "programTreeHash": state.program_tree_hash,
        "scene": _scene_response(updated_scene),
    }


def _tree_node_from_draft_argument(
    value: Any,
    *,
    field_name: str,
) -> tuple[dict[str, Any], list[str]]:
    """Validate + normalize an assistant-draft tree-node argument.

    Returns the validated node dict alongside a list of human-readable
    normalization warnings so callers can surface them in tool-result
    responses and audit records (rather than silently rewriting the
    caller's payload). Empty list when no normalization fired.
    """
    # Accept stringified-JSON form for `value` and parse it back to a
    # dict. Some small models (Nemotron-3-Nano-Omni T=0.6 top_p=0.95
    # observed 2026-05-06) serialize the inline node object as a JSON
    # string instead of an object. The MCP tool schema also accepts the
    # string form (anyOf) so OpenClaw doesn't reject upfront — and we
    # parse it here before any further processing. Same defensive
    # precedent as _strip_policy_stubs.
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (TypeError, ValueError) as exc:
            raise HTTPException(
                status_code=400,
                detail=(
                    f"{field_name} was a string but could not be parsed as "
                    f"JSON: {exc}. Send {field_name} as an inline JSON "
                    f"object instead."
                ),
            ) from exc
        if not isinstance(parsed, dict):
            raise HTTPException(
                status_code=400,
                detail=(
                    f"{field_name} string parsed to a "
                    f"{type(parsed).__name__}, not an object. Send "
                    f"{field_name} as an inline JSON object."
                ),
            )
        value = parsed
    if not isinstance(value, dict):
        raise HTTPException(status_code=400, detail=f"{field_name} must be an object")
    # Pragmatic normalization for small-model emissions:
    #   1. `name` is required by the schema but small models often omit it.
    #      Synthesize `<id>_<short_random>` rather than reject.
    #   2. `modificationPolicy` and `recoveryPolicy` are optional; the model
    #      sometimes emits invalid placeholders like `{"type": "string"}` that
    #      pass schema validation but get rejected by program validation as
    #      "not a recognized policy field". Strip empty/invalid values
    #      defensively. Real callers can still provide valid values; only
    #      stubs are dropped.
    warnings: list[str] = []

    def _strip_policy_stubs(d: dict[str, Any], path: str) -> None:
        for key in ("modificationPolicy", "recoveryPolicy"):
            if key in d:
                policy = d[key]
                if not isinstance(policy, dict) or not policy:
                    d.pop(key, None)
                    warnings.append(
                        f"{path}.{key} was empty/invalid and stripped"
                    )
                    continue
                # Drop ANY single-key {"type": ...} payload, regardless
                # of the value. `type` is NEVER a valid modification or
                # recovery policy key — the real keys are
                # {runtime_override, persistent_edit, intervention_delegate}
                # for modificationPolicy and {action, persistence_ceiling}
                # for recoveryPolicy. Models trained on generic JSON-Schema
                # examples reliably emit `{"type": "string"}` (pydantic
                # default placeholder) or `{"type": "replace"}` (general-
                # purpose patch verb they invent). Both used to slip
                # through with only the "string"/"object"/"any"/"" filter
                # below; this filter catches the invented-verb variant
                # too. Verified 2026-05-06 against
                # `{"type": "replace"}` which loops the model 130x on
                # the deeper program-validation error.
                if (
                    set(policy.keys()) == {"type"}
                    and isinstance(policy.get("type"), str)
                ):
                    d.pop(key, None)
                    warnings.append(
                        f"{path}.{key} was a stub placeholder "
                        f"({{'type': {policy.get('type')!r}}}) and stripped"
                    )

    def _normalize(node: Any, path: str) -> Any:
        if not isinstance(node, dict):
            return node
        cleaned = dict(node)
        if "name" not in cleaned or not str(cleaned.get("name") or "").strip():
            node_id = str(cleaned.get("id") or "").strip()
            if node_id:
                synthesized = f"{node_id}_{uuid.uuid4().hex[:6]}"
                cleaned["name"] = synthesized
                warnings.append(
                    f"{path}.name was missing; synthesized '{synthesized}' "
                    f"from id '{node_id}'"
                )
        _strip_policy_stubs(cleaned, path)
        children = cleaned.get("children")
        if isinstance(children, list):
            cleaned["children"] = [
                _normalize(c, f"{path}.children[{i}]")
                for i, c in enumerate(children)
            ]
        return cleaned

    normalized = _normalize(value, field_name)
    try:
        node = TreeNodeResponse.model_validate(normalized)
    except ValidationError as exc:
        raise HTTPException(status_code=400, detail=f"{field_name} is invalid: {exc}") from exc
    return node.model_dump(by_alias=False), warnings


def _find_tree_node_ref(
    node: dict[str, Any],
    node_name: str,
    *,
    parent: dict[str, Any] | None = None,
    index: int | None = None,
) -> tuple[dict[str, Any], dict[str, Any] | None, int | None] | None:
    if node.get("name") == node_name:
        return node, parent, index
    for child_index, child in enumerate(node.get("children") or []):
        if isinstance(child, dict):
            found = _find_tree_node_ref(
                child,
                node_name,
                parent=node,
                index=child_index,
            )
            if found is not None:
                return found
    return None


def _resolve_node_name_alias(node_name: str, tree: dict[str, Any] | None) -> str:
    """Resolve `@root` alias to the actual root node name. No-op for other names.

    Lets the model target the root without needing to discover its name first —
    e.g., `targetName: "@root"` resolves to whatever the root is called in the
    current program tree. Only used by tools where root operations are valid;
    tools like delete_node/move_node that always reject root pass the raw name
    through and produce their existing "cannot delete/move the root" errors.
    """
    if node_name.strip() == "@root":
        if isinstance(tree, dict) and isinstance(tree.get("name"), str):
            return tree["name"]
    return node_name


def _collect_tree_node_names(node: dict[str, Any]) -> list[str]:
    """Flat list of every node name in the tree, depth-first.

    Surfaced in 404 responses so the model can recover from name typos in one
    turn instead of looping. For trees with hundreds of nodes this is still
    sub-kilobyte; if it ever becomes a concern we can cap at e.g. 50 and
    indicate truncation.
    """
    names: list[str] = []
    name = node.get("name")
    if isinstance(name, str):
        names.append(name)
    for child in node.get("children") or []:
        if isinstance(child, dict):
            names.extend(_collect_tree_node_names(child))
    return names


def _collect_node_kinds(node_spec: dict[str, Any]) -> list[str]:
    """Flat list of every `id` field reachable in a node spec, depth-first.

    Used to walk a model-supplied subtree (insert_node, replace_subtree,
    wrap_node) and check that every node kind it introduces is permitted by
    the active mode's `nodes:` allowlist.
    """
    kinds: list[str] = []
    node_id = node_spec.get("id")
    if isinstance(node_id, str):
        kinds.append(node_id)
    for child in node_spec.get("children") or []:
        if isinstance(child, dict):
            kinds.extend(_collect_node_kinds(child))
    return kinds


def _assert_node_kinds_allowed(
    node_spec: dict[str, Any] | list[dict[str, Any]],
    allowlist: set[str] | None,
    *,
    assistant_mode: str | None,
    field_name: str,
) -> None:
    """Reject a model-supplied subtree that introduces a node kind outside the
    active mode's `nodes:` allowlist.

    Bounded-autonomy guarantee: the assistant mode's `nodes:` list is the
    runtime contract for which BT primitives the assistant may insert. Today
    this is enforced only at this point; out-of-mode inserts that slipped
    through previously could only be caught at promotion-time program
    validation. Now they're caught up-front with a clear list of what IS
    allowed so the model can recover.

    `allowlist=None` means the caller didn't pass a context (e.g., direct unit
    test); skip enforcement.
    """
    if allowlist is None:
        return
    nodes_to_walk: list[dict[str, Any]]
    if isinstance(node_spec, list):
        nodes_to_walk = [n for n in node_spec if isinstance(n, dict)]
    elif isinstance(node_spec, dict):
        nodes_to_walk = [node_spec]
    else:
        return
    seen_kinds: list[str] = []
    for node in nodes_to_walk:
        seen_kinds.extend(_collect_node_kinds(node))
    bad_kinds = sorted({kind for kind in seen_kinds if kind not in allowlist})
    if not bad_kinds:
        return
    mode_label = assistant_mode or "active assistant mode"
    raise HTTPException(
        status_code=403,
        detail={
            "message": (
                f"node kind(s) {bad_kinds} not allowed by {mode_label}'s catalog.nodes"
            ),
            "field": field_name,
            "rejectedNodeKinds": bad_kinds,
            "allowedNodeKinds": sorted(allowlist),
            "hint": (
                "Each id in the subtree must appear in the active mode's "
                "catalog.nodes. Update the deployment's assistant_modes."
                f"{mode_label}.catalog.nodes to expand this list."
            ),
        },
    )


def _tree_contains_name(node: dict[str, Any], node_name: str) -> bool:
    if node.get("name") == node_name:
        return True
    return any(
        _tree_contains_name(child, node_name)
        for child in node.get("children") or []
        if isinstance(child, dict)
    )


def _insert_index(arguments: dict[str, Any], *, child_count: int) -> int:
    raw_index = arguments.get("index")
    if raw_index is None:
        return child_count
    if not isinstance(raw_index, int) or isinstance(raw_index, bool):
        raise HTTPException(status_code=400, detail="index must be an integer")
    if raw_index < 0 or raw_index > child_count:
        raise HTTPException(
            status_code=400,
            detail=f"index must be between 0 and {child_count}",
        )
    return raw_index


def _resolve_insert_position(
    arguments: dict[str, Any],
    tree: Any,
) -> tuple[str, int | None]:
    """Resolve the three mutually-exclusive insert-position forms to a
    (parent_name, optional_index) pair.

    Forms:
      - parentName (optional + index) — explicit parent
      - afterName — sibling-relative; insert AFTER the named sibling
      - beforeName — sibling-relative; insert BEFORE the named sibling

    Returns the parent name to feed into the existing parent-resolution
    path. The numeric index (when known up front) is returned too;
    callers compute index against the resolved parent's child count if
    None is returned.

    Raises HTTPException(400) on conflicting form selection. Raises
    HTTPException(404) when a sibling-relative form references a name
    that doesn't exist or is the tree root (no parent).
    """
    parent_name = arguments.get("parentName") or arguments.get("parent_name")
    after_name = arguments.get("afterName") or arguments.get("after_name")
    before_name = arguments.get("beforeName") or arguments.get("before_name")
    raw_index = arguments.get("index")

    if after_name or before_name:
        if parent_name and parent_name.strip():
            raise HTTPException(
                status_code=400,
                detail=(
                    "tree_draft_insert_node: do not pass parentName "
                    "alongside afterName/beforeName — the parent is "
                    "resolved from the sibling automatically."
                ),
            )
        if after_name and before_name:
            raise HTTPException(
                status_code=400,
                detail=(
                    "tree_draft_insert_node: pass at most one of "
                    "afterName, beforeName."
                ),
            )
        if raw_index is not None:
            raise HTTPException(
                status_code=400,
                detail=(
                    "tree_draft_insert_node: index is mutually exclusive "
                    "with afterName/beforeName."
                ),
            )
        sibling_name = (after_name or before_name).strip()
        sibling_ref = (
            _find_tree_node_ref(tree, sibling_name)
            if isinstance(tree, dict)
            else None
        )
        if sibling_ref is None or sibling_ref[1] is None:
            valid_names = (
                _collect_tree_node_names(tree)
                if isinstance(tree, dict)
                else []
            )
            label = "afterName" if after_name else "beforeName"
            raise HTTPException(
                status_code=404,
                detail={
                    "message": (
                        f"{label} sibling not found or is the tree root: "
                        f"{sibling_name}"
                    ),
                    "validNodeNames": valid_names,
                },
            )
        parent_node, sibling_index = sibling_ref[1], sibling_ref[2]
        derived_parent_name = parent_node.get("name")
        if not isinstance(derived_parent_name, str):
            raise HTTPException(
                status_code=500,
                detail="resolved sibling parent has no name",
            )
        derived_index = sibling_index + 1 if after_name else sibling_index
        return derived_parent_name, derived_index

    if not (isinstance(parent_name, str) and parent_name.strip()):
        raise HTTPException(
            status_code=400,
            detail=(
                "tree_draft_insert_node requires one of: parentName, "
                "afterName, beforeName."
            ),
        )
    return parent_name.strip(), None  # numeric index resolved later


def _apply_tree_draft_insert_node(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
    node_allowlist: set[str] | None = None,
    assistant_mode: str | None = None,
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    # 2026-05-10 schema change: the new node's instance name is now a
    # top-level `nodeName` argument (matching the rest of the
    # tree_draft_* family) instead of nested as `node.name`. Inject it
    # into the node payload here so the existing helper validates the
    # familiar shape. If the caller still nested `name` inside `node`,
    # the top-level `nodeName` wins.
    node_name_arg = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name_arg, str) or not node_name_arg.strip():
        raise HTTPException(
            status_code=400,
            detail=(
                "tree_draft_insert_node requires `nodeName` (the new "
                "node's instance name) as a top-level argument."
            ),
        )
    node_name_arg = node_name_arg.strip()
    raw_node = arguments.get("node")
    if isinstance(raw_node, str):
        try:
            parsed = json.loads(raw_node)
        except (TypeError, ValueError):
            parsed = raw_node  # let the helper raise the right error
        else:
            if isinstance(parsed, dict):
                raw_node = parsed
    if isinstance(raw_node, dict):
        raw_node = {**raw_node, "name": node_name_arg}

    new_node, normalization_warnings = _tree_node_from_draft_argument(
        raw_node, field_name="node"
    )
    _assert_node_kinds_allowed(
        new_node,
        node_allowlist,
        assistant_mode=assistant_mode,
        field_name="node",
    )
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    parent_name, derived_index = _resolve_insert_position(
        arguments, tree if isinstance(tree, dict) else None
    )
    # Accept `@root` as parentName since inserting under the root is always valid.
    resolved_parent_name = _resolve_node_name_alias(parent_name, tree if isinstance(tree, dict) else None)
    parent_ref = _find_tree_node_ref(tree, resolved_parent_name) if isinstance(tree, dict) else None
    if parent_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        # If the bad parentName actually exists in the tree but is a
        # leaf (not a parent), point the model at afterName/beforeName.
        hint: str | None = None
        if (
            isinstance(tree, dict)
            and parent_name in valid_names
            and not arguments.get("afterName")
            and not arguments.get("beforeName")
        ):
            hint = (
                f"`{parent_name}` exists in the tree but is not a parent. "
                f"To insert relative to it, use `afterName: \"{parent_name}\"` "
                f"or `beforeName: \"{parent_name}\"` instead."
            )
        detail: dict[str, Any] = {
            "message": f"Parent node not found: {parent_name}",
            "validParentNames": valid_names,
            "rootAlias": "@root",
        }
        if hint is not None:
            detail["hint"] = hint
        raise HTTPException(status_code=404, detail=detail)
    parent_node = parent_ref[0]
    children = list(parent_node.get("children") or [])
    if derived_index is not None:
        # Sibling-derived index — bounded by sibling lookup, but bound-check
        # defensively in case the tree shape changed under us.
        index = max(0, min(derived_index, len(children)))
    else:
        index = _insert_index(arguments, child_count=len(children))
    children.insert(index, new_node)
    parent_node["children"] = children
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    extra: dict[str, Any] = {
        "nodeName": new_node["name"],
        "parentName": parent_name,
        "index": index,
    }
    if normalization_warnings:
        extra["warnings"] = normalization_warnings
    return _program_draft_result(
        state,
        draft_mutation="tree_node_insert",
        extra=extra,
    )


def _apply_tree_draft_update_node_params(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree_draft_update_node_params requires nodeName")
    params = arguments.get("params")
    if not isinstance(params, dict):
        raise HTTPException(status_code=400, detail="tree_draft_update_node_params requires params object")
    merge = arguments.get("merge", True)
    if not isinstance(merge, bool):
        raise HTTPException(status_code=400, detail="merge must be boolean")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    # Accept `@root` since updating root params is valid.
    resolved_node_name = _resolve_node_name_alias(
        node_name.strip(), tree if isinstance(tree, dict) else None
    )
    target_ref = _find_tree_node_ref(tree, resolved_node_name) if isinstance(tree, dict) else None
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Node not found: {node_name}",
                "validNodeNames": valid_names,
                "rootAlias": "@root",
            },
        )
    target = target_ref[0]
    target["params"] = {**dict(target.get("params") or {}), **params} if merge else dict(params)
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="tree_node_params_update",
        extra={"nodeName": node_name.strip(), "params": target["params"]},
    )


def _apply_tree_draft_delete_node(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree_draft_delete_node requires nodeName")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    # delete_node does NOT accept `@root` — root deletion is rejected separately
    # below, and we want the dedicated "cannot delete the root" error rather
    # than a name-resolution detour.
    target_ref = _find_tree_node_ref(tree, node_name.strip()) if isinstance(tree, dict) else None
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Node not found: {node_name}",
                "validNodeNames": valid_names,
                "note": "root cannot be deleted; use tree_draft_replace_subtree to swap the entire tree",
            },
        )
    target, parent, index = target_ref
    if parent is None or index is None:
        raise HTTPException(status_code=400, detail="tree_draft_delete_node cannot delete the root node")
    children = list(parent.get("children") or [])
    removed = children.pop(index)
    parent["children"] = children
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="tree_node_delete",
        extra={"nodeName": target["name"], "deletedSubtree": removed},
    )


def _apply_tree_draft_move_node(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    new_parent_name = arguments.get("newParentName") or arguments.get("new_parent_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree_draft_move_node requires nodeName")
    if not isinstance(new_parent_name, str) or not new_parent_name.strip():
        raise HTTPException(status_code=400, detail="tree_draft_move_node requires newParentName")
    node_name = node_name.strip()
    new_parent_name = new_parent_name.strip()
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    # move_node does NOT accept `@root` for nodeName (root cannot be moved) but
    # DOES accept `@root` for newParentName (moving something under the root is
    # valid).
    target_ref = _find_tree_node_ref(tree, node_name) if isinstance(tree, dict) else None
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Node not found: {node_name}",
                "validNodeNames": valid_names,
                "note": "root cannot be moved; @root may only be used as newParentName",
            },
        )
    target, old_parent, old_index = target_ref
    if old_parent is None or old_index is None:
        raise HTTPException(status_code=400, detail="tree_draft_move_node cannot move the root node")
    resolved_new_parent_name = _resolve_node_name_alias(
        new_parent_name, tree if isinstance(tree, dict) else None
    )
    if _tree_contains_name(target, resolved_new_parent_name):
        raise HTTPException(status_code=400, detail="Cannot move a node under itself or its descendants")
    old_children = list(old_parent.get("children") or [])
    moving_node = old_children.pop(old_index)
    old_parent["children"] = old_children
    new_parent_ref = _find_tree_node_ref(tree, resolved_new_parent_name)
    if new_parent_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"New parent node not found: {new_parent_name}",
                "validParentNames": valid_names,
                "rootAlias": "@root",
            },
        )
    new_parent = new_parent_ref[0]
    new_children = list(new_parent.get("children") or [])
    index = _insert_index(arguments, child_count=len(new_children))
    new_children.insert(index, moving_node)
    new_parent["children"] = new_children
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="tree_node_move",
        extra={
            "nodeName": node_name,
            "newParentName": new_parent_name,
            "index": index,
        },
    )


def _apply_tree_draft_replace_subtree(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
    node_allowlist: set[str] | None = None,
    assistant_mode: str | None = None,
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree_draft_replace_subtree requires nodeName")
    replacement, normalization_warnings = _tree_node_from_draft_argument(
        arguments.get("subtree"), field_name="subtree"
    )
    _assert_node_kinds_allowed(
        replacement,
        node_allowlist,
        assistant_mode=assistant_mode,
        field_name="subtree",
    )
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    # Accept `@root` since replacing the root subtree is a valid full-tree swap.
    resolved_node_name = _resolve_node_name_alias(
        node_name.strip(), tree if isinstance(tree, dict) else None
    )
    target_ref = _find_tree_node_ref(tree, resolved_node_name) if isinstance(tree, dict) else None
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Node not found: {node_name}",
                "validNodeNames": valid_names,
                "rootAlias": "@root",
            },
        )
    _target, parent, index = target_ref
    if parent is None:
        program_data["tree"] = replacement
    else:
        assert index is not None
        children = list(parent.get("children") or [])
        children[index] = replacement
        parent["children"] = children
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    extra: dict[str, Any] = {
        "nodeName": node_name.strip(),
        "replacementRootName": replacement["name"],
    }
    if normalization_warnings:
        extra["warnings"] = normalization_warnings
    return _program_draft_result(
        state,
        draft_mutation="tree_subtree_replace",
        extra=extra,
    )


def _apply_tree_draft_change_node_kind(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
    node_allowlist: set[str] | None = None,
    assistant_mode: str | None = None,
) -> dict[str, Any]:
    """Change a node's catalog id in place, preserving name/parent/children.

    Validation pipeline:
      1. nodeName exists in the draft tree
      2. newId is in the catalog and (if scoped) in node_allowlist
      3. existing child count fits newId's arity range
      4. merged params (controlled by paramsMode) satisfy newId's
         parameter declarations
      5. newId's required_capabilities present in the deployment
      6. swap_class compatibility (only when both sides declare it),
         bypassable via force=true

    All validation runs before any mutation; on success we update id +
    params atomically and re-emit the draft.
    """
    _assert_program_draft_edit_allowed(state)
    node_name_raw = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name_raw, str) or not node_name_raw.strip():
        raise HTTPException(
            status_code=400, detail="tree_draft_change_node_kind requires nodeName"
        )
    node_name = node_name_raw.strip()
    new_id = arguments.get("newId") or arguments.get("new_id")
    if not isinstance(new_id, str) or not new_id.strip():
        raise HTTPException(
            status_code=400, detail="tree_draft_change_node_kind requires newId"
        )
    new_id = new_id.strip()
    params_overlay = arguments.get("params") or {}
    if not isinstance(params_overlay, dict):
        raise HTTPException(
            status_code=400, detail="tree_draft_change_node_kind params must be an object"
        )
    params_mode = arguments.get("paramsMode") or arguments.get("params_mode") or "replace"
    if params_mode not in ("replace", "merge", "strict"):
        raise HTTPException(
            status_code=400,
            detail="tree_draft_change_node_kind paramsMode must be one of: replace, merge, strict",
        )
    force = bool(arguments.get("force", False))

    program_data = state.program.to_dict()
    tree = program_data.get("tree") if isinstance(program_data, dict) else None
    resolved_node_name = _resolve_node_name_alias(node_name, tree if isinstance(tree, dict) else None)
    target_ref = _find_tree_node_ref(tree, resolved_node_name) if isinstance(tree, dict) else None
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree) if isinstance(tree, dict) else []
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Node not found: {node_name}",
                "validNodeNames": valid_names,
                "rootAlias": "@root",
            },
        )
    target = target_ref[0]
    old_id = target.get("id")

    # Catalog lookup for the new id.
    new_entry = state.catalog.entries_by_id().get(new_id) if hasattr(state.catalog, "entries_by_id") else None
    if new_entry is None:
        for candidate in state.catalog.entries:
            if candidate.id == new_id:
                new_entry = candidate
                break
    if new_entry is None:
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Unknown catalog id for swap: {new_id}",
                "field": "newId",
            },
        )
    # Mode allowlist gate.
    if node_allowlist is not None and new_id not in node_allowlist:
        raise HTTPException(
            status_code=403,
            detail={
                "message": (
                    f"Swap target id '{new_id}' not allowed by "
                    f"{assistant_mode or 'this assistant mode'}'s catalog.nodes"
                ),
                "field": "newId",
                "rejectedNodeKinds": [new_id],
                "allowedNodeKinds": sorted(node_allowlist),
            },
        )
    # Arity gate.
    current_children = list(target.get("children") or [])
    n_children = len(current_children)
    cmin = new_entry.children_min
    cmax = new_entry.children_max
    if cmax >= 0 and n_children > cmax:
        raise HTTPException(
            status_code=400,
            detail=(
                f"swap arity incompatible: new id '{new_id}' allows at most "
                f"{cmax} children, current node has {n_children}. Remove or "
                "move excess children before retrying."
            ),
        )
    if n_children < cmin:
        raise HTTPException(
            status_code=400,
            detail=(
                f"swap arity incompatible: new id '{new_id}' requires at least "
                f"{cmin} children, current node has {n_children}. Insert the "
                "missing children before retrying."
            ),
        )
    # Capability gate.
    deployment = state.deployment
    deployment_caps: set[str] = set()
    if deployment is not None and hasattr(deployment, "capability_keys"):
        try:
            deployment_caps = set(deployment.capability_keys())
        except Exception:  # noqa: BLE001
            deployment_caps = set()
    elif deployment is not None and hasattr(deployment, "capabilities"):
        deployment_caps = set(getattr(deployment, "capabilities", []))
    required = list(getattr(new_entry, "required_capabilities", []) or [])
    missing_caps = [cap for cap in required if cap not in deployment_caps]
    # Treat missing capability info as a soft pass (no-deployment dev path).
    if deployment_caps and missing_caps:
        raise HTTPException(
            status_code=400,
            detail={
                "message": (
                    f"swap capability missing: new id '{new_id}' requires "
                    f"capabilities not present in the active deployment: {missing_caps}"
                ),
                "missingCapabilities": missing_caps,
            },
        )
    # Param merge per paramsMode.
    old_params = dict(target.get("params") or {})
    if params_mode == "replace":
        merged_params = dict(params_overlay)
    elif params_mode == "merge":
        merged_params = {**old_params, **params_overlay}
    else:  # strict
        merged_params = {**old_params, **params_overlay}
    accepted_param_names = {p.name for p in new_entry.parameters}
    dropped_params = {
        k: v for k, v in merged_params.items() if k not in accepted_param_names
    }
    if params_mode == "strict" and dropped_params:
        raise HTTPException(
            status_code=400,
            detail={
                "message": (
                    f"swap params invalid: existing params {sorted(dropped_params.keys())} "
                    f"not accepted by new id '{new_id}' under strict paramsMode"
                ),
                "droppedParams": sorted(dropped_params.keys()),
                "acceptedParamNames": sorted(accepted_param_names),
            },
        )
    cleaned_params = {
        k: v for k, v in merged_params.items() if k in accepted_param_names
    }
    # Required-param check on the cleaned set.
    missing_required = [
        p.name
        for p in new_entry.parameters
        if p.required and p.default is None and p.name not in cleaned_params
    ]
    if missing_required:
        raise HTTPException(
            status_code=400,
            detail={
                "message": (
                    f"swap params invalid: new id '{new_id}' requires "
                    f"parameters not present after merge: {missing_required}"
                ),
                "missingRequiredParams": missing_required,
                "droppedParams": sorted(dropped_params.keys()),
                "providedParamNames": sorted(cleaned_params.keys()),
            },
        )
    # Swap-class compatibility (advisory; only enforced when both sides
    # carry a swap_class field and they differ).
    old_entry = None
    if isinstance(old_id, str):
        for candidate in state.catalog.entries:
            if candidate.id == old_id:
                old_entry = candidate
                break
    old_class = getattr(old_entry, "swap_class", None) if old_entry is not None else None
    new_class = getattr(new_entry, "swap_class", None)
    swap_class_matched: bool | None = None
    if old_class and new_class:
        if old_class == new_class:
            swap_class_matched = True
        elif force:
            swap_class_matched = False
        else:
            raise HTTPException(
                status_code=400,
                detail={
                    "message": (
                        f"swap class mismatch: '{old_id}' is in swap_class "
                        f"'{old_class}' but '{new_id}' is in '{new_class}'. "
                        "Pass force=true to bypass if the cross-class swap "
                        "is intentional."
                    ),
                    "oldSwapClass": old_class,
                    "newSwapClass": new_class,
                },
            )
    # All gates passed — atomic mutation.
    target["id"] = new_id
    target["params"] = cleaned_params
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    extra: dict[str, Any] = {
        "nodeName": target.get("name"),
        "oldId": old_id,
        "newId": new_id,
        "swapClassMatched": swap_class_matched,
        "droppedParams": sorted(dropped_params.keys()),
        "providedParamNames": sorted(cleaned_params.keys()),
    }
    return _program_draft_result(
        state,
        draft_mutation="tree_node_swap",
        extra=extra,
    )


def _apply_tree_draft_wrap_node(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
    node_allowlist: set[str] | None = None,
    assistant_mode: str | None = None,
) -> dict[str, Any]:
    """Wrap a target node with a new parent.

    Single semantic op for "give this node a new parent" — useful for inserting
    a Repeat/Retry decorator above an existing subtree, or for adding a new
    Sequence root that wraps the previous program. The handler inserts the
    target's existing subtree as the wrapper's only child; the wrapper itself
    must NOT carry `children` (rejected at schema level, double-checked here).
    """
    _assert_program_draft_edit_allowed(state)
    target_name_raw = arguments.get("targetName") or arguments.get("target_name")
    if not isinstance(target_name_raw, str) or not target_name_raw.strip():
        raise HTTPException(status_code=400, detail="tree_draft_wrap_node requires targetName")
    target_name = target_name_raw.strip()

    wrapper_arg = arguments.get("wrapper")
    if not isinstance(wrapper_arg, dict):
        raise HTTPException(status_code=400, detail="tree_draft_wrap_node requires wrapper object")
    # The contract is: the wrapper's children are filled in by the handler.
    # Any caller-supplied children value is discarded.
    #
    # Non-empty children is a real intent conflict (the caller is trying to
    # specify a different child than the wrap target) and is rejected with a
    # 400. Empty children: [] is just structural noise — the model often emits
    # it as a default for tree-node-shaped fields — so we strip it silently
    # and surface a warning in the result rather than fail the call. This is
    # a conscious empirical concession: strict key-presence rejection caused
    # a recovery loop with this model; warn-and-strip preserves forward
    # progress while keeping the contract visible.
    children_warning: str | None = None
    if "children" in wrapper_arg:
        provided_children = wrapper_arg["children"]
        if isinstance(provided_children, list) and len(provided_children) > 0:
            raise HTTPException(
                status_code=400,
                detail=(
                    "tree_draft_wrap_node wrapper must not include non-empty "
                    "'children'. The handler inserts the target's existing "
                    "subtree as the wrapper's only child. Resubmit without "
                    "the children field, or with an empty list."
                ),
            )
        # Empty list (or null/missing) — strip and warn.
        children_warning = (
            "wrapper.children was provided but ignored; the handler always "
            "fills in the target's existing subtree as the only child. "
            "Future calls should omit the children field."
        )
        wrapper_arg = {k: v for k, v in wrapper_arg.items() if k != "children"}
    wrapper, normalization_warnings = _tree_node_from_draft_argument(
        wrapper_arg, field_name="wrapper"
    )

    # Mode-allowlist enforcement: the wrapper's id must be in the active
    # mode's catalog.nodes. The wrapper has no children to walk (we just
    # rejected non-empty children above), so this is a single-id check.
    _assert_node_kinds_allowed(
        wrapper,
        node_allowlist,
        assistant_mode=assistant_mode,
        field_name="wrapper",
    )

    # Pre-validate the wrapper's catalog id with a helpful suggestion list when
    # the model invents a non-existent id (e.g., "repeat_node" instead of
    # "repeat"). Without this, the model gets a generic "Unknown catalog
    # entry: 'X'" from program validation with no recovery hint and loops.
    wrapper_id = wrapper.get("id")
    if isinstance(wrapper_id, str):
        catalog_ids = sorted({entry.id for entry in state.catalog.entries})
        if wrapper_id not in catalog_ids:
            # Heuristic shortlist: ids that share a substring with the wrong one.
            related = sorted(
                cid for cid in catalog_ids
                if wrapper_id and (wrapper_id in cid or cid in wrapper_id)
            )
            raise HTTPException(
                status_code=400,
                detail={
                    "message": f"Unknown catalog entry: '{wrapper_id}'",
                    "wrapperIdSuggestions": related,
                    "validCatalogIds": catalog_ids,
                    "hint": (
                        "wrap_node's wrapper.id must be a node kind from the "
                        "active node catalog. Common control-flow ids: "
                        "sequence, fallback, parallel, repeat, retry, inverter."
                    ),
                },
            )

    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    if not isinstance(tree, dict):
        raise HTTPException(status_code=400, detail="program has no tree to wrap")

    resolved_target_name = _resolve_node_name_alias(target_name, tree)
    target_ref = _find_tree_node_ref(tree, resolved_target_name)
    if target_ref is None:
        valid_names = _collect_tree_node_names(tree)
        raise HTTPException(
            status_code=404,
            detail={
                "message": f"Target node not found: {target_name}",
                "validTargetNames": valid_names,
                "rootAlias": "@root",
            },
        )

    target, parent, index = target_ref
    # Attach the target's full existing subtree as the wrapper's only child.
    # Use the target node dict by reference — _apply_program_draft_data will
    # validate and round-trip the structure.
    wrapper["children"] = [target]

    if parent is None:
        # Wrapping the current root produces a new root.
        program_data["tree"] = wrapper
    else:
        assert index is not None
        children = list(parent.get("children") or [])
        children[index] = wrapper
        parent["children"] = children

    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    extra: dict[str, Any] = {
        "targetName": resolved_target_name,
        "wrapperName": wrapper["name"],
        "newRootName": program_data["tree"].get("name"),
    }
    combined_warnings: list[str] = list(normalization_warnings)
    if children_warning is not None:
        combined_warnings.append(children_warning)
    if combined_warnings:
        extra["warnings"] = combined_warnings
    return _program_draft_result(
        state,
        draft_mutation="tree_node_wrap",
        extra=extra,
    )


def _program_parameter_dump(raw: Any, *, index: int) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise HTTPException(status_code=400, detail=f"parameters[{index}] must be an object")
    try:
        parameter = ProgramParameterResponse.model_validate(raw)
    except ValidationError as exc:
        raise HTTPException(status_code=400, detail=f"parameters[{index}] is invalid: {exc}") from exc
    return parameter.model_dump()


def _apply_program_draft_upsert_parameters(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    raw_parameters = arguments.get("parameters")
    if not isinstance(raw_parameters, list) or not raw_parameters:
        raise HTTPException(status_code=400, detail="program_draft_upsert_parameters requires parameters")
    parameters = [_program_parameter_dump(raw, index=index) for index, raw in enumerate(raw_parameters)]
    names = [parameter["name"] for parameter in parameters]
    if len(set(names)) != len(names):
        duplicates = sorted({name for name in names if names.count(name) > 1})
        raise HTTPException(status_code=400, detail=f"Duplicate parameter names: {duplicates}")
    program_data = state.program.to_dict()
    existing = {
        item.get("name"): item
        for item in program_data.get("parameters") or []
        if isinstance(item.get("name"), str)
    }
    inserted: list[str] = []
    updated: list[str] = []
    for parameter in parameters:
        if parameter["name"] in existing:
            updated.append(parameter["name"])
        else:
            inserted.append(parameter["name"])
        existing[parameter["name"]] = parameter
    program_data["parameters"] = list(existing.values())
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="program_parameters_upsert",
        extra={"names": names, "inserted": inserted, "updated": updated},
    )


def _apply_program_draft_remove_parameters(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    names = _string_list_argument(arguments, key="names", tool_id="program_draft_remove_parameters")
    program_data = state.program.to_dict()
    parameters = list(program_data.get("parameters") or [])
    existing_names = {
        item.get("name")
        for item in parameters
        if isinstance(item.get("name"), str)
    }
    missing = sorted(set(names) - existing_names)
    if missing:
        raise HTTPException(status_code=404, detail=f"Program parameters do not exist: {missing}")
    removed = set(names)
    program_data["parameters"] = [
        item for item in parameters
        if item.get("name") not in removed
    ]
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="program_parameters_remove",
        extra={"names": names, "removed": names},
    )


def _blackboard_payload(program_data: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    raw = program_data.get("blackboard")
    if not isinstance(raw, dict):
        return {"namespaces": [], "keys": []}
    return {
        "namespaces": list(raw.get("namespaces") or []),
        "keys": list(raw.get("keys") or []),
    }


def _blackboard_namespace_dump(raw: Any, *, index: int) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise HTTPException(status_code=400, detail=f"namespaces[{index}] must be an object")
    try:
        namespace = BlackboardNamespaceResponse.model_validate(raw)
    except ValidationError as exc:
        raise HTTPException(status_code=400, detail=f"namespaces[{index}] is invalid: {exc}") from exc
    return namespace.model_dump()


def _blackboard_key_dump(raw: Any, *, index: int) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise HTTPException(status_code=400, detail=f"keys[{index}] must be an object")
    try:
        key = BlackboardKeyResponse.model_validate(raw)
    except ValidationError as exc:
        raise HTTPException(status_code=400, detail=f"keys[{index}] is invalid: {exc}") from exc
    return key.model_dump()


def _apply_blackboard_draft_upsert_namespaces(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    raw_namespaces = arguments.get("namespaces")
    if not isinstance(raw_namespaces, list) or not raw_namespaces:
        raise HTTPException(status_code=400, detail="blackboard_draft_upsert_namespaces requires namespaces")
    namespaces = [
        _blackboard_namespace_dump(raw, index=index)
        for index, raw in enumerate(raw_namespaces)
    ]
    ids = [namespace["id"] for namespace in namespaces]
    if len(set(ids)) != len(ids):
        duplicates = sorted({item for item in ids if ids.count(item) > 1})
        raise HTTPException(status_code=400, detail=f"Duplicate namespace IDs: {duplicates}")
    program_data = state.program.to_dict()
    blackboard = _blackboard_payload(program_data)
    existing = {
        item.get("id"): item
        for item in blackboard["namespaces"]
        if isinstance(item.get("id"), str)
    }
    inserted: list[str] = []
    updated: list[str] = []
    for namespace in namespaces:
        if namespace["id"] in existing:
            updated.append(namespace["id"])
        else:
            inserted.append(namespace["id"])
        existing[namespace["id"]] = namespace
    blackboard["namespaces"] = list(existing.values())
    program_data["blackboard"] = blackboard
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="blackboard_namespaces_upsert",
        extra={"ids": ids, "inserted": inserted, "updated": updated},
    )


def _apply_blackboard_draft_remove_namespaces(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    ids = _string_list_argument(arguments, key="ids", tool_id="blackboard_draft_remove_namespaces")
    program_data = state.program.to_dict()
    blackboard = _blackboard_payload(program_data)
    existing_ids = {
        item.get("id")
        for item in blackboard["namespaces"]
        if isinstance(item.get("id"), str)
    }
    missing = sorted(set(ids) - existing_ids)
    if missing:
        raise HTTPException(status_code=404, detail=f"Blackboard namespaces do not exist: {missing}")
    removed = set(ids)
    blackboard["namespaces"] = [
        item for item in blackboard["namespaces"]
        if item.get("id") not in removed
    ]
    program_data["blackboard"] = blackboard
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="blackboard_namespaces_remove",
        extra={"ids": ids, "removed": ids},
    )


def _apply_blackboard_draft_upsert_keys(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    raw_keys = arguments.get("keys")
    if not isinstance(raw_keys, list) or not raw_keys:
        raise HTTPException(status_code=400, detail="blackboard_draft_upsert_keys requires keys")
    keys = [_blackboard_key_dump(raw, index=index) for index, raw in enumerate(raw_keys)]
    ids = [key["id"] for key in keys]
    if len(set(ids)) != len(ids):
        duplicates = sorted({item for item in ids if ids.count(item) > 1})
        raise HTTPException(status_code=400, detail=f"Duplicate blackboard key IDs: {duplicates}")
    program_data = state.program.to_dict()
    blackboard = _blackboard_payload(program_data)
    existing = {
        item.get("id"): item
        for item in blackboard["keys"]
        if isinstance(item.get("id"), str)
    }
    inserted: list[str] = []
    updated: list[str] = []
    for key in keys:
        if key["id"] in existing:
            updated.append(key["id"])
        else:
            inserted.append(key["id"])
        existing[key["id"]] = key
    blackboard["keys"] = list(existing.values())
    program_data["blackboard"] = blackboard
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="blackboard_keys_upsert",
        extra={"ids": ids, "inserted": inserted, "updated": updated},
    )


def _apply_blackboard_draft_remove_keys(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    ids = _string_list_argument(arguments, key="ids", tool_id="blackboard_draft_remove_keys")
    program_data = state.program.to_dict()
    blackboard = _blackboard_payload(program_data)
    existing_ids = {
        item.get("id")
        for item in blackboard["keys"]
        if isinstance(item.get("id"), str)
    }
    missing = sorted(set(ids) - existing_ids)
    if missing:
        raise HTTPException(status_code=404, detail=f"Blackboard keys do not exist: {missing}")
    removed = set(ids)
    blackboard["keys"] = [
        item for item in blackboard["keys"]
        if item.get("id") not in removed
    ]
    program_data["blackboard"] = blackboard
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="blackboard_keys_remove",
        extra={"ids": ids, "removed": ids},
    )


def _assistant_provider_context(
    state: ComposerState,
    request: AssistantChatRequest,
    *,
    conversation_id: str,
    request_id: str,
    program_loaded: bool,
    cycle_state: str,
) -> AssistantProviderContext:
    deployment = state.deployment
    if request.mode == "provider" and deployment is not None and deployment.assistant_modes:
        assistant_mode = (request.assistant_mode or "").strip()
        if not assistant_mode:
            raise HTTPException(
                status_code=400,
                detail="assistantMode is required for this deployment's assistant modes",
            )
        try:
            deployment.validate_assistant_modes(state.catalog, state.skill_catalog)
            resolved_catalog = deployment.resolve_assistant_mode_catalog(assistant_mode)
        except DeploymentValidationError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

        allowed_tool_ids = {
            tool.get("id")
            for tool in resolved_catalog["tools"]
            if isinstance(tool.get("id"), str)
        }
        requested_tool_ids = set(request.requested_tools)
        unknown_requested = sorted(requested_tool_ids - allowed_tool_ids)
        if unknown_requested:
            raise HTTPException(
                status_code=400,
                detail=(
                    "requestedTools contains tools outside assistantMode "
                    f"'{assistant_mode}': {unknown_requested}"
                ),
            )

        # Build the rich nodeCatalog using the same _catalog_entry_payload
        # function that powers catalog_read. Mode-filtered to nodes
        # visible in this mode; otherwise the payload is byte-identical
        # to what the model would receive from a catalog_read tool call.
        # Single source of truth for catalog metadata between preamble
        # and live tool calls.
        _visible_node_id_set = set(resolved_catalog["nodes"])
        _visible_node_catalog: list[dict[str, Any]] = [
            _catalog_entry_payload(entry, include_parameters=True)
            for entry in state.catalog.entries
            if entry.id in _visible_node_id_set
        ]
        # Skill payload uses _skills_payload for the same single-source-
        # of-truth reason as the catalog: identical shape between
        # preamble and live skills_read.
        _visible_skill_ids = list(resolved_catalog.get("skills") or [])
        try:
            _visible_skill_catalog = _skills_payload(
                state, skill_ids=_visible_skill_ids, include_bodies=True,
            ) if _visible_skill_ids else []
        except HTTPException:
            _visible_skill_catalog = []
        # Auto-inject snapshots of the current program tree and scene
        # state (added 2026-05-06) so the LLM can plan from real state
        # without spending turns on `program_read` and `scene_inspect`. Same
        # data those tools would return — payload-shape parity. The
        # model is still expected to call those tools to *refresh*
        # state mid-task (e.g. after applying a mutation), and the
        # schema descriptions for those tools have been updated to
        # spell that out.
        _program_snapshot: dict[str, Any] | None = None
        if state.program is not None:
            _program_snapshot = {
                "programLoaded": True,
                "programSourcePath": state.program_source_path,
                "programSessionGeneration": state.program_session_generation,
                "programTreeHash": state.program_tree_hash,
                "program": state.program.to_dict(),
            }
        _scene_snapshot: dict[str, Any] | None = None
        try:
            _scene_snapshot = build_scene_state_payload(state)
        except Exception:  # noqa: BLE001 — fall back gracefully
            _scene_snapshot = None
        return AssistantProviderContext(
            conversation_id=conversation_id,
            request_id=request_id,
            program_loaded=program_loaded,
            cycle_state=cycle_state,
            assistant_mode=assistant_mode,
            schema_version="0.1.0",
            tool_catalog=enrich_assistant_tool_descriptors(
                list(resolved_catalog["tools"])
            ),
            visible_node_catalog=_visible_node_catalog,
            visible_skill_catalog=_visible_skill_catalog,
            program_snapshot=_program_snapshot,
            scene_snapshot=_scene_snapshot,
        )

    if request.mode == "provider" and request.assistant_mode:
        raise HTTPException(
            status_code=400,
            detail="assistantMode requires deployment assistant_modes",
        )

    return AssistantProviderContext(
        conversation_id=conversation_id,
        request_id=request_id,
        program_loaded=program_loaded,
        cycle_state=cycle_state,
        tool_catalog=assistant_tool_catalog(),
    )


def _resolve_provider(state: ComposerState, request: AssistantChatRequest):
    if request.mode == "mock":
        return MockAssistantProvider()
    provider = _configured_assistant_provider(state)
    if provider is None:
        raise HTTPException(status_code=503, detail="No assistant provider is configured")
    if request.provider_id and request.provider_id != provider.provider_id:
        raise HTTPException(status_code=404, detail="Requested assistant provider is not configured")
    return provider


def _configured_assistant_provider(state: ComposerState):
    if state.assistant_provider is not None:
        return state.assistant_provider
    deployment = state.deployment
    if deployment is None or deployment.assistant is None:
        return None
    assistant_config = deployment.assistant
    if not assistant_config.enabled:
        return None
    if assistant_config.provider == "mock":
        return MockAssistantProvider()

    endpoint_url: str | None = None
    timeout_s: float | None = None
    api_token: str | None = None
    if assistant_config.endpoint_alias:
        endpoint = deployment.model_endpoints.get(assistant_config.endpoint_alias)
        if endpoint is None:
            raise HTTPException(
                status_code=500,
                detail=(
                    "Deployment assistant endpoint alias was not found after "
                    "deployment validation"
                ),
            )
        endpoint_url = endpoint.base_url
        timeout_s = endpoint.default_timeout_s
        if endpoint.api_key_env:
            api_token = os.environ.get(endpoint.api_key_env)

    try:
        return build_assistant_provider(
            assistant_config.provider,
            endpoint_url=endpoint_url,
            timeout_s=timeout_s,
            api_token=api_token,
        )
    except AssistantProviderError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc


def _validated_proposal_payload(proposal: dict[str, Any]) -> dict[str, Any]:
    """Validate supported proposal payloads before staging."""
    proposal_type = proposal.get("type")
    if proposal_type == "scene_resource_add":
        payload = proposal.get("payload")
        if not isinstance(payload, dict):
            raise HTTPException(status_code=400, detail="scene_resource_add payload must be an object")
        scene_resource = payload.get("sceneResource")
        if not isinstance(scene_resource, dict):
            raise HTTPException(
                status_code=400,
                detail="scene_resource_add payload.sceneResource must be an object",
            )
        try:
            ProgramSceneResourceResponse.model_validate(scene_resource)
        except ValidationError as exc:
            raise HTTPException(
                status_code=400,
                detail=f"scene_resource_add payload.sceneResource is invalid: {exc}",
            ) from exc
    elif proposal_type == "tree_node_add":
        payload = proposal.get("payload")
        if not isinstance(payload, dict):
            raise HTTPException(status_code=400, detail="tree_node_add payload must be an object")
        tree_node = payload.get("treeNode")
        if not isinstance(tree_node, dict):
            raise HTTPException(
                status_code=400,
                detail="tree_node_add payload.treeNode must be an object",
            )
        try:
            TreeNodeResponse.model_validate(tree_node)
        except ValidationError as exc:
            raise HTTPException(
                status_code=400,
                detail=f"tree_node_add payload.treeNode is invalid: {exc}",
            ) from exc
    return proposal


def _proposal_record_payload(record: Any) -> dict[str, Any]:
    return {
        "proposalId": record.proposal_id,
        "conversationId": record.conversation_id,
        "lifecycleStatus": record.lifecycle_status,
        "proposal": record.proposal,
        "source": record.source,
        "createdAtNs": record.created_at_ns,
        "updatedAtNs": record.updated_at_ns,
    }


def _materialize_scene_resource_add(
    state: ComposerState,
    proposal: dict[str, Any],
) -> dict[str, Any]:
    if state.program is None:
        raise HTTPException(status_code=404, detail="No program loaded")
    payload = proposal.get("payload")
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400, detail="scene_resource_add payload must be an object")
    resource = ProgramSceneResourceResponse.model_validate(payload.get("sceneResource"))

    program_data = state.program.to_dict()
    scene = list(program_data.get("scene") or [])
    if any(item.get("object_id") == resource.object_id for item in scene):
        raise HTTPException(
            status_code=409,
            detail=f"Scene object already exists: {resource.object_id}",
        )

    scene.append(resource.model_dump(by_alias=False))
    program_data["scene"] = scene
    try:
        candidate = Program.from_dict(
            program_data,
            state.catalog,
            path=state.program_source_path or "<memory>",
            skill_catalog=state.skill_catalog,
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        if state.deployment is not None:
            state.deployment.validate_against_program(candidate)
            state.deployment.validate_capabilities(candidate)
    except (ProgramValidationError, DeploymentValidationError) as exc:
        raise HTTPException(status_code=400, detail=str(exc))

    return {
        "scene": [
            ProgramSceneResourceResponse(
                objectId=item["object_id"],
                shape=item["shape"],
                pose=item["pose"],
            ).model_dump(by_alias=True)
            for item in scene
        ],
    }


def _deployment_capability_keys(state: ComposerState) -> set[str] | None:
    if state.deployment is None:
        return None
    capability_keys: set[str] = set()
    for adapter in state.deployment.adapters.values():
        capability_keys.update(adapter.capability_keys)
    return capability_keys
