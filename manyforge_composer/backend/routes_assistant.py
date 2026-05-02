"""Assistant routes - /api/assistant/*"""
from __future__ import annotations

import hashlib
import json
import os
import time
import uuid
from typing import Any

from fastapi import APIRouter, HTTPException, Query
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import JSONResponse
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
from .assistant_tool_schemas import enrich_assistant_tool_descriptors
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
from .routes_scene import build_scene_state_payload
from .state import ComposerState


READ_ONLY_EFFECT = "read_only"
COMPOSER_DRAFT_MUTATING_EFFECT = "composer_draft_mutating"
COMPOSER_ASSISTANT_MODE = "composer-assistant"


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

        state.finish_assistant_request(
            request_id,
            status="completed",
            message=result.message,
            warnings=result.warnings,
            draft_mutated=result.draft_mutated,
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
            "draftMutated": result.draft_mutated,
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

        try:
            result = await _execute_bridge_tool(
                state,
                tool_id=tool_id,
                effect=tool_definition.effect,
                arguments=request.arguments,
            )
            success = True
            error = None
        except HTTPException:
            raise
        except Exception as exc:
            result = {}
            success = False
            error = str(exc)

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
    payload = {
        "mode": mode,
        "skills": sorted(catalog.get("skills", []) or []),
        "tools": sorted(catalog.get("tools", []) or []),
        "nodes": sorted(catalog.get("nodes", []) or []),
    }
    blob = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


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
    if tool_id == "catalog.read":
        entry_ids = arguments.get("entryIds") or arguments.get("entry_ids") or []
        if entry_ids is None:
            entry_ids = []
        if not isinstance(entry_ids, list) or not all(isinstance(item, str) for item in entry_ids):
            raise HTTPException(status_code=400, detail="catalog.read entryIds must be a list of strings")
        category = arguments.get("category")
        kind = arguments.get("kind")
        include_parameters = arguments.get("includeParameters", arguments.get("include_parameters", True))
        if not isinstance(include_parameters, bool):
            raise HTTPException(status_code=400, detail="catalog.read includeParameters must be boolean")
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

    if tool_id == "skills.read":
        skill_ids = arguments.get("skillIds") or arguments.get("skill_ids") or []
        if skill_ids is None:
            skill_ids = []
        if not isinstance(skill_ids, list) or not all(isinstance(item, str) for item in skill_ids):
            raise HTTPException(status_code=400, detail="skills.read skillIds must be a list of strings")
        include_bodies = arguments.get("includeBodies", arguments.get("include_bodies", False))
        if not isinstance(include_bodies, bool):
            raise HTTPException(status_code=400, detail="skills.read includeBodies must be boolean")
        skills = _skills_payload(state, skill_ids=skill_ids, include_bodies=include_bodies)
        return {
            "skillCount": len(skills),
            "skills": skills,
            "timestampNs": time.monotonic_ns(),
        }

    if tool_id == "deployment.capabilities.read":
        return _deployment_capabilities_payload(state)

    if tool_id == "program.read":
        return {
            "programLoaded": state.program is not None,
            "programSourcePath": state.program_source_path,
            "programSessionGeneration": state.program_session_generation,
            "programTreeHash": state.program_tree_hash,
            "program": state.program.to_dict() if state.program is not None else None,
        }

    if tool_id == "program.validate":
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

    if tool_id == "status.read":
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

    if tool_id == "scene.inspect":
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
) -> dict[str, Any]:
    if tool_id == "scene.draft.add_object":
        return _apply_scene_draft_resource_single(
            state,
            arguments=arguments,
            operation="add",
        )
    if tool_id == "scene.draft.update_object":
        return _apply_scene_draft_resource_single(
            state,
            arguments=arguments,
            operation="update",
        )
    if tool_id == "scene.draft.upsert_objects":
        return _apply_scene_draft_resources_upsert(state, arguments=arguments)
    if tool_id == "scene.draft.remove_objects":
        return _apply_scene_draft_resources_remove(state, arguments=arguments)
    if tool_id == "tree.draft.insert_node":
        return _apply_tree_draft_insert_node(state, arguments=arguments)
    if tool_id == "tree.draft.update_node_params":
        return _apply_tree_draft_update_node_params(state, arguments=arguments)
    if tool_id == "tree.draft.delete_node":
        return _apply_tree_draft_delete_node(state, arguments=arguments)
    if tool_id == "tree.draft.move_node":
        return _apply_tree_draft_move_node(state, arguments=arguments)
    if tool_id == "tree.draft.replace_subtree":
        return _apply_tree_draft_replace_subtree(state, arguments=arguments)
    if tool_id == "program.draft.upsert_parameters":
        return _apply_program_draft_upsert_parameters(state, arguments=arguments)
    if tool_id == "program.draft.remove_parameters":
        return _apply_program_draft_remove_parameters(state, arguments=arguments)
    if tool_id == "blackboard.draft.upsert_namespaces":
        return _apply_blackboard_draft_upsert_namespaces(state, arguments=arguments)
    if tool_id == "blackboard.draft.remove_namespaces":
        return _apply_blackboard_draft_remove_namespaces(state, arguments=arguments)
    if tool_id == "blackboard.draft.upsert_keys":
        return _apply_blackboard_draft_upsert_keys(state, arguments=arguments)
    if tool_id == "blackboard.draft.remove_keys":
        return _apply_blackboard_draft_remove_keys(state, arguments=arguments)
    raise HTTPException(
        status_code=404,
        detail=f"composer draft bridge callback is not implemented for tool '{tool_id}'",
    )


def _scene_resource_from_draft_arguments(
    arguments: dict[str, Any],
    *,
    tool_id: str,
) -> ProgramSceneResourceResponse:
    raw_resource = (
        arguments.get("sceneResource")
        or arguments.get("scene_resource")
        or arguments
    )
    if not isinstance(raw_resource, dict):
        raise HTTPException(
            status_code=400,
            detail=f"{tool_id} requires a sceneResource object",
        )
    try:
        return ProgramSceneResourceResponse.model_validate(raw_resource)
    except ValidationError as exc:
        raise HTTPException(
            status_code=400,
            detail=f"{tool_id} sceneResource is invalid: {exc}",
        ) from exc


def _scene_resources_from_upsert_arguments(
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
            detail="scene.draft.upsert_objects requires a non-empty sceneResources list",
        )

    resources: list[ProgramSceneResourceResponse] = []
    seen: set[str] = set()
    duplicates: set[str] = set()
    for index, raw_resource in enumerate(raw_resources):
        if not isinstance(raw_resource, dict):
            raise HTTPException(
                status_code=400,
                detail=f"sceneResources[{index}] must be an object",
            )
        try:
            resource = ProgramSceneResourceResponse.model_validate(raw_resource)
        except ValidationError as exc:
            raise HTTPException(
                status_code=400,
                detail=f"sceneResources[{index}] is invalid: {exc}",
            ) from exc
        if resource.object_id in seen:
            duplicates.add(resource.object_id)
        seen.add(resource.object_id)
        resources.append(resource)

    if duplicates:
        raise HTTPException(
            status_code=400,
            detail=f"scene.draft.upsert_objects duplicate objectId values: {sorted(duplicates)}",
        )
    return resources


def _assert_scene_draft_edit_allowed(state: ComposerState) -> None:
    _assert_program_draft_edit_allowed(state)


def _assert_program_draft_edit_allowed(state: ComposerState) -> None:
    if state.program is None:
        raise HTTPException(status_code=404, detail="No program loaded")
    if state.cycle_manager is not None and state.cycle_manager.state.value in {
        "running",
        "interrupt_handler",
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
    tool_id = f"scene.draft.{operation}_object"
    force_discard_overrides = _force_discard_overrides(arguments)
    resource = _scene_resource_from_draft_arguments(arguments, tool_id=tool_id)
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
    resources = _scene_resources_from_upsert_arguments(arguments)
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
        tool_id="scene.draft.remove_objects",
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


def _tree_node_from_draft_argument(value: Any, *, field_name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise HTTPException(status_code=400, detail=f"{field_name} must be an object")
    try:
        node = TreeNodeResponse.model_validate(value)
    except ValidationError as exc:
        raise HTTPException(status_code=400, detail=f"{field_name} is invalid: {exc}") from exc
    return node.model_dump(by_alias=False)


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


def _apply_tree_draft_insert_node(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    parent_name = arguments.get("parentName") or arguments.get("parent_name")
    if not isinstance(parent_name, str) or not parent_name.strip():
        raise HTTPException(status_code=400, detail="tree.draft.insert_node requires parentName")
    new_node = _tree_node_from_draft_argument(arguments.get("node"), field_name="node")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    parent_ref = _find_tree_node_ref(tree, parent_name.strip()) if isinstance(tree, dict) else None
    if parent_ref is None:
        raise HTTPException(status_code=404, detail=f"Parent node not found: {parent_name}")
    parent_node = parent_ref[0]
    children = list(parent_node.get("children") or [])
    index = _insert_index(arguments, child_count=len(children))
    children.insert(index, new_node)
    parent_node["children"] = children
    _apply_program_draft_data(
        state,
        program_data=program_data,
        force_discard_overrides=_force_discard_overrides(arguments),
    )
    return _program_draft_result(
        state,
        draft_mutation="tree_node_insert",
        extra={
            "nodeName": new_node["name"],
            "parentName": parent_name.strip(),
            "index": index,
        },
    )


def _apply_tree_draft_update_node_params(
    state: ComposerState,
    *,
    arguments: dict[str, Any],
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree.draft.update_node_params requires nodeName")
    params = arguments.get("params")
    if not isinstance(params, dict):
        raise HTTPException(status_code=400, detail="tree.draft.update_node_params requires params object")
    merge = arguments.get("merge", True)
    if not isinstance(merge, bool):
        raise HTTPException(status_code=400, detail="merge must be boolean")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    target_ref = _find_tree_node_ref(tree, node_name.strip()) if isinstance(tree, dict) else None
    if target_ref is None:
        raise HTTPException(status_code=404, detail=f"Node not found: {node_name}")
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
        raise HTTPException(status_code=400, detail="tree.draft.delete_node requires nodeName")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    target_ref = _find_tree_node_ref(tree, node_name.strip()) if isinstance(tree, dict) else None
    if target_ref is None:
        raise HTTPException(status_code=404, detail=f"Node not found: {node_name}")
    target, parent, index = target_ref
    if parent is None or index is None:
        raise HTTPException(status_code=400, detail="tree.draft.delete_node cannot delete the root node")
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
        raise HTTPException(status_code=400, detail="tree.draft.move_node requires nodeName")
    if not isinstance(new_parent_name, str) or not new_parent_name.strip():
        raise HTTPException(status_code=400, detail="tree.draft.move_node requires newParentName")
    node_name = node_name.strip()
    new_parent_name = new_parent_name.strip()
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    target_ref = _find_tree_node_ref(tree, node_name) if isinstance(tree, dict) else None
    if target_ref is None:
        raise HTTPException(status_code=404, detail=f"Node not found: {node_name}")
    target, old_parent, old_index = target_ref
    if old_parent is None or old_index is None:
        raise HTTPException(status_code=400, detail="tree.draft.move_node cannot move the root node")
    if _tree_contains_name(target, new_parent_name):
        raise HTTPException(status_code=400, detail="Cannot move a node under itself or its descendants")
    old_children = list(old_parent.get("children") or [])
    moving_node = old_children.pop(old_index)
    old_parent["children"] = old_children
    new_parent_ref = _find_tree_node_ref(tree, new_parent_name)
    if new_parent_ref is None:
        raise HTTPException(status_code=404, detail=f"New parent node not found: {new_parent_name}")
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
) -> dict[str, Any]:
    _assert_program_draft_edit_allowed(state)
    node_name = arguments.get("nodeName") or arguments.get("node_name")
    if not isinstance(node_name, str) or not node_name.strip():
        raise HTTPException(status_code=400, detail="tree.draft.replace_subtree requires nodeName")
    replacement = _tree_node_from_draft_argument(arguments.get("subtree"), field_name="subtree")
    program_data = state.program.to_dict()
    tree = program_data.get("tree")
    target_ref = _find_tree_node_ref(tree, node_name.strip()) if isinstance(tree, dict) else None
    if target_ref is None:
        raise HTTPException(status_code=404, detail=f"Node not found: {node_name}")
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
    return _program_draft_result(
        state,
        draft_mutation="tree_subtree_replace",
        extra={
            "nodeName": node_name.strip(),
            "replacementRootName": replacement["name"],
        },
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
        raise HTTPException(status_code=400, detail="program.draft.upsert_parameters requires parameters")
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
    names = _string_list_argument(arguments, key="names", tool_id="program.draft.remove_parameters")
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
        raise HTTPException(status_code=400, detail="blackboard.draft.upsert_namespaces requires namespaces")
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
    ids = _string_list_argument(arguments, key="ids", tool_id="blackboard.draft.remove_namespaces")
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
        raise HTTPException(status_code=400, detail="blackboard.draft.upsert_keys requires keys")
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
    ids = _string_list_argument(arguments, key="ids", tool_id="blackboard.draft.remove_keys")
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
            visible_skills=list(resolved_catalog["skills"]),
            visible_nodes=list(resolved_catalog["nodes"]),
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
