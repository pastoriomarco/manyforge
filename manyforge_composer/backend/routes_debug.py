"""Debug/export routes - /api/debug/*"""
from __future__ import annotations

import time
from typing import Any

from fastapi import APIRouter, HTTPException

from .models import (
    DebugSignalPublishRequest,
    DebugExportDeploymentResponse,
    DebugExportProgramResponse,
    DebugExportResponse,
    Ros2MedkitTraceResponse,
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
    """Build combined backend/native-viewer scene trace payload."""
    backend_trace = []
    if state.cycle_manager is not None:
        backend_trace = state.cycle_manager.get_scene_trace(limit=limit)

    visual_objects, visual_objects_meta = state.visual_scene_objects_snapshot()
    visual_state, visual_state_meta = state.visual_state_snapshot()
    visual_objects_meta = visual_objects_meta or {}
    visual_state_meta = visual_state_meta or {}
    visual_trace = [
        {
            "source": "native_visual_cache",
            "event": "visual_scene_objects_snapshot",
            "object_count": len(visual_objects),
            "objects_source": visual_objects_meta.get("source"),
            "age_ms": visual_objects_meta.get("age_ms"),
            "refresh_running": visual_objects_meta.get("refresh_running"),
            "last_error": visual_objects_meta.get("last_error"),
        },
        {
            "source": "native_visual_cache",
            "event": "visual_state_snapshot",
            "available": visual_state is not None,
            "sequence_number": visual_state_meta.get("sequence_number"),
            "age_ms": visual_state_meta.get("age_ms"),
            "publisher_running": visual_state_meta.get("publisher_running"),
            "last_error": visual_state_meta.get("last_error"),
        },
    ]

    return {
        "scope": {
            "included": [
                "backend scene mutation trace (add/remove/update_pose/attach/detach)",
                "native visual cache scene/object snapshot state",
            ],
            "excluded": [
                "browser-side WebGL/client rendering and pixels",
                "persistent trace storage beyond execution JSONL event sink",
                "planner/collision internals beyond scene mutations and queried scene state",
            ],
        },
        "backend": backend_trace,
        "visual": visual_trace,
    }


def build_debug_export_payload(state: ComposerState, *, run_id: str | None = None) -> dict[str, Any]:
    """Build single-call trace/debug export payload."""
    state.event_recorder.observe(state, force=True)
    selected_run_id = run_id or state.event_recorder.last_run_id
    medkit_trace = build_ros2_medkit_trace_payload(state, run_id=selected_run_id)
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
        eventChannels=state.event_recorder.channel_histories(),
        incidentSnapshots=state.event_recorder.incident_snapshots(),
        sceneState=build_scene_state_payload(state),
        sceneDiagnostics=build_scene_diagnostics_payload(state),
        sceneTrace=build_scene_trace_payload(state),
        blackboard=build_blackboard_snapshot_payload(state),
        signalSnapshot=(
            state.cycle_manager.get_signal_snapshot()
            if state.cycle_manager is not None
            else {"providers": {}, "signals": {}}
        ),
        nodeStatuses=(state.cycle_manager.get_node_statuses() if state.cycle_manager is not None else {}),
        interventionSession=intervention_session,
        runtimeOverrides=(
            state.runtime_override_status()
            if state.program is not None
            else {}
        ),
        modelEvents=model_events,
        medkitTrace=medkit_trace,
    ).model_dump(by_alias=True)


def build_ros2_medkit_trace_payload(
    state: ComposerState,
    *,
    run_id: str | None = None,
) -> dict[str, Any]:
    """Build a normalized execution trace for ros2_medkit-style correlation tooling."""
    state.event_recorder.observe(state, force=True)
    payload = state.event_recorder.export_ros2_medkit_trace(
        run_id=run_id,
        exported_at_ns=time.monotonic_ns(),
    )
    return Ros2MedkitTraceResponse(
        schemaVersion=payload["schema_version"],
        traceSource=payload["trace_source"],
        exportedAtNs=payload["exported_at_ns"],
        runId=payload["run_id"],
        programRunId=payload["program_run_id"],
        interventionRunIds=payload["intervention_run_ids"],
        parentRunIds=payload["parent_run_ids"],
        records=payload["records"],
    ).model_dump(by_alias=True)


def build_collision_compare_trajectory_waypoint_payload(
    state: ComposerState,
    *,
    move_id: str,
    waypoint_index: int = 0,
) -> dict[str, Any]:
    """Compare legacy and ManyForge collision engines for a cached waypoint."""
    if state.kernel is None:
        raise HTTPException(status_code=503, detail="Kernel is not available")
    if not hasattr(state.kernel, "get_cached_trajectory"):
        raise HTTPException(
            status_code=501,
            detail="Kernel client does not expose cached trajectories",
        )
    if not hasattr(state.kernel, "collision_compare_positions"):
        raise HTTPException(
            status_code=501,
            detail="Kernel client does not expose collision comparison",
        )

    trajectory = state.kernel.get_cached_trajectory(move_id)
    if trajectory is None:
        raise HTTPException(
            status_code=404,
            detail=f"No cached trajectory for move_id '{move_id}'",
        )
    if waypoint_index < 0 or waypoint_index >= len(trajectory.waypoints):
        raise HTTPException(
            status_code=400,
            detail=(
                "waypoint_index must be in [0, "
                f"{max(len(trajectory.waypoints) - 1, 0)}]"
            ),
        )

    waypoint = trajectory.waypoints[waypoint_index]
    comparison = state.kernel.collision_compare_positions(
        waypoint.positions_rad,
        waypoint_index=waypoint_index,
    )
    return {
        "move_id": move_id,
        "waypoint_index": waypoint_index,
        "time_from_start_s": waypoint.time_from_start_s,
        "joint_names": list(trajectory.joint_names),
        "positions_rad": list(waypoint.positions_rad),
        "comparison": comparison,
    }


def build_collision_compare_positions_payload(
    state: ComposerState,
    *,
    positions_rad: list[float],
    waypoint_index: int | None = None,
) -> dict[str, Any]:
    """Compare legacy and ManyForge collision engines for explicit positions."""
    if state.kernel is None:
        raise HTTPException(status_code=503, detail="Kernel is not available")
    if not hasattr(state.kernel, "collision_compare_positions"):
        raise HTTPException(
            status_code=501,
            detail="Kernel client does not expose collision comparison",
        )
    if not positions_rad:
        raise HTTPException(status_code=400, detail="positions_rad is required")
    comparison = state.kernel.collision_compare_positions(
        [float(value) for value in positions_rad],
        waypoint_index=waypoint_index,
    )
    return {
        "positions_rad": [float(value) for value in positions_rad],
        "waypoint_index": waypoint_index,
        "comparison": comparison,
    }


def create_debug_router(state: ComposerState) -> APIRouter:
    """Create debug/export routes."""
    router = APIRouter()

    @router.get("/scene-trace")
    def debug_scene_trace(limit: int = 200) -> dict[str, Any]:
        """Return current backend/native-viewer scene trace data."""
        return build_scene_trace_payload(state, limit=limit)

    @router.get("/export")
    def debug_export(run_id: str | None = None) -> dict[str, Any]:
        """Return single-call debug export bundle."""
        return build_debug_export_payload(state, run_id=run_id)

    @router.get("/export/ros2-medkit")
    def debug_export_ros2_medkit(run_id: str | None = None) -> dict[str, Any]:
        """Return normalized trace export for ros2_medkit-style tooling."""
        return build_ros2_medkit_trace_payload(state, run_id=run_id)

    @router.get("/collision/compare-trajectory/{move_id}")
    def debug_collision_compare_trajectory_waypoint(
        move_id: str,
        waypoint_index: int = 0,
    ) -> dict[str, Any]:
        """Compare legacy and ManyForge collision checks for a cached waypoint."""
        return build_collision_compare_trajectory_waypoint_payload(
            state,
            move_id=move_id,
            waypoint_index=waypoint_index,
        )

    @router.post("/collision/compare-positions")
    def debug_collision_compare_positions(request: dict[str, Any]) -> dict[str, Any]:
        """Compare legacy and ManyForge collision checks for explicit positions."""
        positions = request.get("positions_rad")
        if not isinstance(positions, list):
            raise HTTPException(status_code=400, detail="positions_rad must be a list")
        waypoint_index = request.get("waypoint_index")
        if waypoint_index is not None:
            waypoint_index = int(waypoint_index)
        return build_collision_compare_positions_payload(
            state,
            positions_rad=positions,
            waypoint_index=waypoint_index,
        )

    @router.post("/signals/publish")
    def debug_publish_signal(request: DebugSignalPublishRequest) -> dict[str, Any]:
        """Publish one manual signal sample into the runtime signal registry."""
        if state.cycle_manager is None:
            raise HTTPException(status_code=503, detail="Cycle manager is not available")

        adapter = request.adapter.strip()
        signal = request.signal.strip()
        if not adapter:
            raise HTTPException(status_code=400, detail="adapter is required")
        if not signal:
            raise HTTPException(status_code=400, detail="signal is required")

        state.cycle_manager.publish_signal(
            adapter=adapter,
            signal=signal,
            value=request.value,
            stale_after_ms=request.stale_after_ms,
            mirror_key=request.mirror_key,
            clear_on_stale=request.clear_on_stale,
            metadata=dict(request.metadata),
        )
        return state.cycle_manager.get_signal_snapshot()

    return router
