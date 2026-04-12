"""Debug/export routes - /api/debug/*"""
from __future__ import annotations

import time
from typing import Any

from fastapi import APIRouter

from .models import (
    DebugExportDeploymentResponse,
    DebugExportProgramResponse,
    DebugExportResponse,
)
from .routes_execution import (
    build_blackboard_snapshot_payload,
    build_execution_digest_payload,
    build_execution_history_payload,
)
from .routes_kernel import build_kernel_metrics_payload, build_kernel_snapshot_payload
from .routes_scene import build_scene_diagnostics_payload, build_scene_state_payload
from .state import ComposerState


def build_scene_trace_payload(state: ComposerState, *, limit: int = 200) -> dict[str, Any]:
    """Build combined backend/HMI scene trace payload."""
    backend_trace = []
    if state.cycle_manager is not None:
        backend_trace = state.cycle_manager.get_scene_trace(limit=limit)

    hmi_trace = []
    object_diagnostics = {}
    hmi_server = getattr(state, "hmi_server", None)
    if hmi_server is not None:
        if hasattr(hmi_server, "get_scene_trace"):
            hmi_trace = hmi_server.get_scene_trace(limit=limit)
        if hasattr(hmi_server, "get_object_render_diagnostics"):
            object_ids = set()
            if state.kernel is not None:
                object_ids.update(
                    scene_object.object_id for scene_object in state.kernel.get_scene_objects()
                )
            if state.program is not None:
                object_ids.update(resource.object_id for resource in state.program.scene or [])
            for object_id in sorted(object_ids):
                object_diagnostics[object_id] = (
                    hmi_server.get_object_render_diagnostics(object_id)
                )

    return {
        "scope": {
            "included": [
                "backend scene mutation trace (add/remove/update_pose/attach/detach)",
                "HMI invalidation hints and per-object reconciliation decisions",
                "current per-object backend-vs-render diagnostics",
            ],
            "excluded": [
                "browser-side WebGL/client rendering and pixels",
                "persistent trace storage beyond execution JSONL event sink",
                "planner/collision internals beyond scene mutations and queried scene state",
            ],
        },
        "backend": backend_trace,
        "hmi": hmi_trace,
        "objects": object_diagnostics,
    }


def build_debug_export_payload(state: ComposerState, *, run_id: str | None = None) -> dict[str, Any]:
    """Build single-call trace/debug export payload."""
    state.event_recorder.observe(state)
    selected_run_id = run_id or state.event_recorder.last_run_id
    history = build_execution_history_payload(state, run_id=selected_run_id)
    intervention_session = None
    if state.cycle_manager is not None and state.cycle_manager.intervention is not None:
        session = state.cycle_manager.intervention
        intervention_session = {
            "session_id": session.session_id,
            "session_kind": session.session_kind,
            "authority_source": session.authority_source,
            "principal_id": session.principal_id,
            "grant_type": session.grant_type,
            "parent_run_id": session.parent_run_id,
            "entry_state": session.entry_state,
            "lease_timeout_ms": session.lease_timeout_ms,
            "lease_expires_at_ns": session.lease_expires_at_ns,
            "resume_target": session.resume_target,
            "trigger_reason": session.trigger_reason,
            "trigger_node_instance": session.trigger_node_instance,
            "approval_mode": session.approval_mode,
            "artifact_origin": session.artifact_origin,
            "status": session.status,
            "actual_outcome": session.actual_outcome,
            "error": session.error,
            "started_at_ns": session.started_at_ns,
            "finished_at_ns": session.finished_at_ns,
            "events": list(session.events),
        }
    # Filter model call events from the event recorder
    model_events: list[dict] = []
    if selected_run_id:
        all_events = state.event_recorder.export_events(run_id=selected_run_id)
        model_events = [
            e for e in all_events
            if isinstance(e.get("type"), str) and e["type"].startswith("model_call_")
        ]

    return DebugExportResponse(
        exportedAtNs=time.monotonic_ns(),
        runId=selected_run_id,
        program=DebugExportProgramResponse(
            name=state.program.name if state.program is not None else None,
            sourcePath=state.program_source_path,
        ),
        deployment=DebugExportDeploymentResponse(
            name=state.deployment.name if state.deployment is not None else None,
            sourcePath=state.deployment_source_path,
        ),
        kernelSnapshot=build_kernel_snapshot_payload(state),
        kernelMetrics=build_kernel_metrics_payload(state) if state.kernel is not None else {},
        executionDigest=build_execution_digest_payload(state),
        executionHistory=history,
        sceneState=build_scene_state_payload(state),
        sceneDiagnostics=build_scene_diagnostics_payload(state),
        sceneTrace=build_scene_trace_payload(state),
        blackboard=build_blackboard_snapshot_payload(state),
        nodeStatuses=(state.cycle_manager.get_node_statuses() if state.cycle_manager is not None else {}),
        interventionSession=intervention_session,
        runtimeOverrides=(
            state.runtime_override_status()
            if state.program is not None
            else {}
        ),
        modelEvents=model_events,
    ).model_dump(by_alias=True)


def create_debug_router(state: ComposerState) -> APIRouter:
    """Create debug/export routes."""
    router = APIRouter()

    @router.get("/scene-trace")
    def debug_scene_trace(limit: int = 200) -> dict[str, Any]:
        """Return current backend/HMI scene trace data."""
        return build_scene_trace_payload(state, limit=limit)

    @router.get("/export")
    def debug_export(run_id: str | None = None) -> dict[str, Any]:
        """Return single-call debug export bundle."""
        return build_debug_export_payload(state, run_id=run_id)

    return router
