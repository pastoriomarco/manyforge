"""Assistant routes - /api/assistant/*"""
from __future__ import annotations

import time
import uuid
from typing import Any

from fastapi import APIRouter, HTTPException
from fastapi.concurrency import run_in_threadpool

from .assistant_tools import (
    AssistantToolError,
    assistant_tool_catalog,
    inspect_isaac_scene_report,
    propose_scene_object_resources,
)
from .models import (
    AssistantChatRequest,
    AssistantChatResponse,
    AssistantInspectIsaacSceneRequest,
    AssistantInspectIsaacSceneResponse,
    AssistantProposeSceneObjectsRequest,
    AssistantProposeSceneObjectsResponse,
    AssistantToolCatalogResponse,
)
from .state import ComposerState


_SIMULATION_TERMS = (
    "isaac",
    "sim",
    "simulation",
    "scene",
    "collider",
    "object",
    "soup",
    "grasp",
)


def create_assistant_router(state: ComposerState) -> APIRouter:
    """Create the model-free assistant router."""
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
        proposals = _mock_proposals(request, conversation_id, program_loaded, cycle_state)

        return {
            "conversationId": conversation_id,
            "mode": "mock",
            "message": _mock_message(request, proposals),
            "proposals": proposals,
            "toolCalls": [],
            "warnings": [
                "Assistant scaffold is running in mock mode; no model or external tool was invoked.",
                "Returned proposals are drafts and require explicit user review before application.",
            ],
            "mutated": False,
            "requiresReview": True,
            "programLoaded": program_loaded,
            "cycleState": cycle_state,
            "timestampNs": time.monotonic_ns(),
        }

    @router.get(
        "/tools",
        response_model=AssistantToolCatalogResponse,
        response_model_by_alias=True,
    )
    def list_tools() -> dict[str, Any]:
        """List deterministic assistant tools available before model integration."""
        return {"tools": assistant_tool_catalog()}

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

    return router


def _mock_proposals(
    request: AssistantChatRequest,
    conversation_id: str,
    program_loaded: bool,
    cycle_state: str,
) -> list[dict[str, Any]]:
    message = request.message.lower()
    context_keys = set(request.context.keys())
    scene_related = any(term in message for term in _SIMULATION_TERMS) or bool(
        context_keys & {"isaac_scene_report", "scene_report", "collider_report"}
    )

    if scene_related:
        return [
            {
                "id": f"proposal-{conversation_id[:12]}-scene-review",
                "type": "scene_authoring_review",
                "title": "Inspect simulator scene before authoring objects",
                "summary": (
                    "Use the Isaac scene inspection tool to gather collider/object data, "
                    "then propose explicit ManyForge scene draft edits for user review."
                ),
                "status": "draft",
                "reviewRequired": True,
                "payload": {
                    "recommendedTool": "inspect_isaac_scene",
                    "requestedTools": request.requested_tools,
                    "nextActions": [
                        "inspect simulator colliders with configured include/exclude filters",
                        "ask the user which objects should become planning geometry",
                        "propose add/update/remove scene draft edits without applying them",
                    ],
                    "programLoaded": program_loaded,
                    "cycleState": cycle_state,
                },
                "warnings": [
                    "Do not infer safety-critical planning geometry without user approval.",
                    "Do not mutate Composer state from the assistant response path.",
                ],
                "source": {
                    "kind": "mock_assistant_scaffold",
                    "mode": request.mode,
                },
            }
        ]

    return [
        {
            "id": f"proposal-{conversation_id[:12]}-assistant-scaffold",
            "type": "assistant_scaffold",
            "title": "No model-backed assistant configured",
            "summary": (
                "The assistant API is available, but this route currently returns only "
                "reviewable mock proposals and performs no actions."
            ),
            "status": "draft",
            "reviewRequired": True,
            "payload": {
                "requestedTools": request.requested_tools,
                "programLoaded": program_loaded,
                "cycleState": cycle_state,
            },
            "warnings": [
                "Configure a bounded model/tool provider before relying on assistant output.",
            ],
            "source": {
                "kind": "mock_assistant_scaffold",
                "mode": request.mode,
            },
        }
    ]


def _mock_message(
    request: AssistantChatRequest,
    proposals: list[dict[str, Any]],
) -> str:
    proposal_type = proposals[0]["type"] if proposals else "none"
    if proposal_type == "scene_authoring_review":
        return (
            "I can help with simulator-backed scene authoring, but this scaffold only "
            "returns draft proposals. It did not inspect Isaac Sim or change the program."
        )
    return (
        "Assistant scaffold is active in mock mode. It did not call a model, invoke tools, "
        "or change Composer state."
    )
