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
from manyforge_behavior.deployment import DeploymentValidationError
from manyforge_behavior.program import Program, ProgramValidationError
from pydantic import ValidationError

from .assistant_tools import (
    AssistantToolError,
    assistant_tool_catalog,
    inspect_isaac_scene_report,
    propose_scene_object_nodes,
    propose_scene_object_resources,
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
    AssistantToolCatalogResponse,
    ProgramSceneResourceResponse,
    TreeNodeResponse,
)
from .routes_scene import build_scene_state_payload
from .state import ComposerState


def create_assistant_router(state: ComposerState) -> APIRouter:
    """Create assistant routes.

    Model-backed providers are deliberately behind an explicit provider mode.
    All returned model proposals remain drafts until a user-facing flow applies
    a materialized session patch.
    """
    router = APIRouter()

    @router.post(
        "/chat",
        response_model=AssistantChatResponse,
        response_model_by_alias=True,
    )
    def chat(request: AssistantChatRequest) -> dict[str, Any]:
        """Return reviewable assistant proposals without mutating Composer state."""
        conversation_id = request.conversation_id or uuid.uuid4().hex
        cycle_state = state.cycle_manager.state.value if state.cycle_manager else "idle"
        program_loaded = state.program is not None
        provider = _resolve_provider(state, request)
        provider_context = _assistant_provider_context(
            state,
            request,
            conversation_id=conversation_id,
            program_loaded=program_loaded,
            cycle_state=cycle_state,
        )

        try:
            result = provider.run(request, provider_context)
        except AssistantProviderError as exc:
            raise HTTPException(status_code=502, detail=str(exc))

        return {
            "conversationId": conversation_id,
            "mode": result.mode,
            "providerId": provider.provider_id,
            "assistantMode": provider_context.assistant_mode,
            "message": result.message,
            "proposals": result.proposals,
            "toolCalls": result.tool_calls,
            "warnings": result.warnings,
            "mutated": result.mutated,
            "requiresReview": result.requires_review,
            "programLoaded": program_loaded,
            "cycleState": cycle_state,
            "timestampNs": result.timestamp_ns,
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

    @router.post(
        "/bridge/tools/{tool_id}",
        response_model=AssistantBridgeToolResponse,
        response_model_by_alias=True,
    )
    async def execute_bridge_read_tool(
        tool_id: str,
        request: AssistantBridgeToolRequest,
    ) -> dict[str, Any]:
        """Execute a mode-authorized read-only callback for the assistant bridge.

        The bridge may call this endpoint inside its multi-turn loop. Composer
        revalidates the active mode and catalog hash, and it refuses all
        proposal/runtime-mutating tools. This endpoint never mutates Composer
        or runtime state.
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
        if tool_definition.effect != "read_only":
            raise HTTPException(
                status_code=403,
                detail=(
                    f"tool '{tool_id}' has effect '{tool_definition.effect}', "
                    "but bridge callbacks may execute read_only tools only"
                ),
            )

        try:
            result = await _execute_bridge_read_only_tool(
                state,
                tool_id=tool_id,
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
            "mutated": False,
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


def _assistant_provider_context(
    state: ComposerState,
    request: AssistantChatRequest,
    *,
    conversation_id: str,
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
            program_loaded=program_loaded,
            cycle_state=cycle_state,
            assistant_mode=assistant_mode,
            schema_version="0.1.0",
            tool_catalog=list(resolved_catalog["tools"]),
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
