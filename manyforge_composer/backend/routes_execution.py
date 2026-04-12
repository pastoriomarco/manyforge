"""Execution observability routes - /api/execution/*"""
from __future__ import annotations

import time
from typing import Any, Iterable

from fastapi import APIRouter

from .models import (
    BlackboardSnapshotResponse,
    ExecutionDigestObjectResponse,
    ExecutionDigestResponse,
    ExecutionHistoryResponse,
)
from .routes_scene import build_scene_diagnostics_payload
from .state import ComposerState


def _iter_tree_names(node) -> Iterable[str]:
    yield node.name
    for child in node.children:
        yield from _iter_tree_names(child)


def build_execution_history_payload(
    state: ComposerState,
    *,
    run_id: str | None = None,
    limit: int | None = None,
) -> dict[str, Any]:
    """Build the execution history payload."""
    state.event_recorder.observe(state)
    return ExecutionHistoryResponse(
        **state.event_recorder.history(run_id=run_id, limit=limit),
    ).model_dump(by_alias=True)


def build_blackboard_snapshot_payload(state: ComposerState) -> dict[str, Any]:
    """Build a JSON-safe blackboard snapshot."""
    keys = {}
    if state.cycle_manager is not None:
        keys = state.cycle_manager.get_blackboard_snapshot()
    return BlackboardSnapshotResponse(
        keys=keys,
        timestampNs=time.monotonic_ns(),
    ).model_dump(by_alias=True)


def build_execution_digest_payload(state: ComposerState) -> dict[str, Any]:
    """Build high-level execution digest optimized for polling agents."""
    state.event_recorder.observe(state)

    cycle_state = state.cycle_manager.state.value if state.cycle_manager is not None else "idle"
    velocity_scale = state.cycle_manager.velocity_scale if state.cycle_manager is not None else 1.0
    node_statuses = state.cycle_manager.get_node_statuses() if state.cycle_manager is not None else {}
    kernel_status = (
        state.cycle_manager.get_status()
        if state.cycle_manager is not None
        else state.kernel.get_status()
        if state.kernel is not None
        else None
    )

    ordered_names: list[str] = []
    root_name = None
    if state.program is not None and state.program.tree is not None:
        ordered_names = list(_iter_tree_names(state.program.tree))
        root_name = state.program.tree.name

    current_node = None
    for name in ordered_names:
        if name == root_name:
            continue
        if node_statuses.get(name) == "RUNNING":
            current_node = name
            break
    if current_node is None and node_statuses.get(root_name or "") == "RUNNING":
        current_node = root_name
    current_node_status = node_statuses.get(current_node) if current_node else None

    completed_nodes = [
        name
        for name in ordered_names
        if name != root_name and node_statuses.get(name) == "SUCCESS"
    ]
    remaining_nodes = [
        name
        for name in ordered_names
        if name != root_name
        and name != current_node
        and node_statuses.get(name) != "SUCCESS"
    ]

    object_summary: list[ExecutionDigestObjectResponse] = []
    if state.kernel is not None:
        for scene_object in sorted(state.kernel.get_scene_objects(), key=lambda item: item.object_id):
            pose = scene_object.pose_in_universe or {}
            position_m = pose.get("position_m")
            object_summary.append(
                ExecutionDigestObjectResponse(
                    id=scene_object.object_id,
                    attached=scene_object.attached,
                    position_m=list(position_m) if position_m is not None else None,
                )
            )

    scene_diagnostics = None
    warnings: list[str] = []
    try:
        scene_diagnostics = build_scene_diagnostics_payload(state)
    except Exception:
        scene_diagnostics = None

    if scene_diagnostics is not None:
        if scene_diagnostics.get("summary", {}).get("hmiAvailable", False):
            for obj in scene_diagnostics.get("objects", []):
                if not obj.get("match", False) and obj.get("mismatch_reason"):
                    warnings.append(f"{obj['object_id']}: {obj['mismatch_reason']}")

    runtime_override_state = (
        state.runtime_override_status()
        if state.program is not None
        else {
            "active_count": 0,
            "staged_count": 0,
            "has_active": False,
            "has_staged": False,
            "can_activate_now": False,
            "requires_reset_to_activate": False,
            "current_tree_origin": "saved_program",
            "timestamp_ns": time.monotonic_ns(),
        }
    )
    delegated_recovery_request = None
    if state.cycle_manager is not None and state.cycle_manager.pending_delegated_recovery is not None:
        request = state.cycle_manager.pending_delegated_recovery
        delegated_recovery_request = {
            "requestId": request.request_id,
            "parentRunId": request.parent_run_id,
            "requestedFromState": request.requested_from_state,
            "triggerNodeInstance": request.trigger_node_instance,
            "triggerNodeId": request.trigger_node_id,
            "triggerReason": request.trigger_reason.value if hasattr(request.trigger_reason, "value") else request.trigger_reason,
            "delegatePolicy": request.delegate_policy,
            "allowedAuthoritySources": list(request.allowed_authority_sources),
            "allowedActions": list(request.allowed_actions),
            "allowedResumeTargets": list(request.allowed_resume_targets),
            "defaultResumeTarget": request.default_resume_target,
            "runtimeOverridePolicy": request.runtime_override_policy,
            "persistentEditPolicy": request.persistent_edit_policy,
            "requestedAtNs": request.requested_at_ns,
            "timestampNs": time.monotonic_ns(),
        }
        warnings.append(
            f"Delegated recovery requested by '{request.trigger_node_instance}' ({request.trigger_reason.value if hasattr(request.trigger_reason, 'value') else request.trigger_reason})"
        )
    if runtime_override_state.get("has_active"):
        warnings.append(
            f"{runtime_override_state['active_count']} active runtime override(s) applied"
        )
    if runtime_override_state.get("has_staged"):
        if runtime_override_state.get("requires_reset_to_activate"):
            warnings.append(
                "Staged runtime overrides require reset before they can take effect"
            )
        else:
            warnings.append(
                f"{runtime_override_state['staged_count']} staged runtime override(s) pending activation"
            )

    started_at_ns = state.event_recorder.current_run_started_at_ns()
    if (
        started_at_ns is None
        and cycle_state == "direct_control"
        and state.cycle_manager is not None
        and state.cycle_manager.intervention is not None
    ):
        started_at_ns = state.cycle_manager.intervention.started_at_ns
    elapsed_s = 0.0
    if started_at_ns is not None:
        elapsed_s = max(0.0, (time.monotonic_ns() - started_at_ns) / 1_000_000_000.0)

    return ExecutionDigestResponse(
        state=cycle_state,
        runId=state.event_recorder.last_run_id,
        executionId=kernel_status.execution_id if kernel_status is not None else 0,
        currentNode=current_node,
        currentNodeStatus=current_node_status,
        activeMoveId=(kernel_status.active_move_id if kernel_status and kernel_status.active_move_id else None),
        progress=kernel_status.progress if kernel_status is not None else 0.0,
        elapsedS=elapsed_s,
        velocityScale=velocity_scale,
        completedNodes=completed_nodes,
        remainingNodes=remaining_nodes,
        objectSummary=object_summary,
        error=(
            state.cycle_manager.error
            if state.cycle_manager is not None and state.cycle_manager.error is not None
            else getattr(kernel_status, "error", None)
        ),
        warnings=warnings,
        runtimeOverrideState=runtime_override_state,
        delegatedRecoveryRequest=delegated_recovery_request,
        timestampNs=time.monotonic_ns(),
    ).model_dump(by_alias=True)


def create_execution_router(state: ComposerState) -> APIRouter:
    """Create execution observability routes."""
    router = APIRouter()

    @router.get("/history")
    def get_execution_history(run_id: str | None = None, limit: int | None = None) -> dict[str, Any]:
        """Return execution history for the current or most recent run."""
        return build_execution_history_payload(state, run_id=run_id, limit=limit)

    @router.get("/digest")
    def get_execution_digest() -> dict[str, Any]:
        """Return high-level execution digest."""
        return build_execution_digest_payload(state)

    @router.get("/blackboard")
    def get_blackboard_snapshot() -> dict[str, Any]:
        """Return a JSON-safe blackboard snapshot."""
        return build_blackboard_snapshot_payload(state)

    return router
