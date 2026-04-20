"""Execution observability routes - /api/execution/*"""
from __future__ import annotations

import time
from typing import Any, Iterable

from fastapi import APIRouter, Query

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
    start_ns: int | None = None,
    end_ns: int | None = None,
    min_severity: str | None = None,
    components: list[str] | None = None,
    event_types: list[str] | None = None,
    execution_id: int | None = None,
    program_run_id: str | None = None,
    intervention_run_id: str | None = None,
) -> dict[str, Any]:
    """Build the execution history payload."""
    state.event_recorder.observe(state)
    return ExecutionHistoryResponse(
        **state.event_recorder.history(
            run_id=run_id,
            limit=limit,
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
        ),
    ).model_dump(by_alias=True)


def _split_csv_query(value: str | None) -> list[str] | None:
    if value is None:
        return None
    items = [item.strip() for item in value.split(",") if item.strip()]
    return items or None


def _interrupt_timing_payload(details: dict[str, Any]) -> dict[str, Any]:
    return {
        "quiesceCompletedAtNs": details.get("quiesce_completed_at_ns"),
        "quiesceLatencyNs": details.get("quiesce_latency_ns"),
        "handlerStartedAtNs": details.get("handler_started_at_ns"),
        "handlerStartLatencyNs": details.get("handler_start_latency_ns"),
        "handlerPostQuiesceLatencyNs": details.get("handler_post_quiesce_latency_ns"),
        "handlerCompletedAtNs": details.get("handler_completed_at_ns"),
        "handlerDurationNs": details.get("handler_duration_ns"),
        "handlerCompletionLatencyNs": details.get("handler_completion_latency_ns"),
        "deferredWaitNs": details.get("deferred_wait_ns"),
        "restartCompletedAtNs": details.get("restart_completed_at_ns"),
        "restartCompletionLatencyNs": details.get("restart_completion_latency_ns"),
        "clearedAtNs": details.get("cleared_at_ns"),
        "clearLatencyNs": details.get("clear_latency_ns"),
        "operatorResumeLatencyNs": details.get("operator_resume_latency_ns"),
        "autoResumeLatencyNs": details.get("auto_resume_latency_ns"),
        "restartClearLatencyNs": details.get("restart_clear_latency_ns"),
    }


def _operator_action_state(
    *,
    cycle_state: str,
    pending_interrupt: dict[str, Any] | None,
) -> tuple[str, bool, bool, bool]:
    if cycle_state == "interrupt_handler":
        return ("wait", False, False, False)
    if pending_interrupt is not None:
        details = pending_interrupt.get("details") or {}
        if bool(details.get("ack_required")):
            return ("acknowledge", False, False, True)
        handler_state = str(details.get("handler_state", "latched"))
        after_handler = str(pending_interrupt.get("afterHandler") or "")
        if cycle_state == "stopped" and after_handler == "resumable_hold" and handler_state == "completed":
            return ("resume", True, False, False)
        if after_handler in {"halted", "failed", "escalate"}:
            return ("reset", False, True, False)
        if handler_state in {"running", "latched"}:
            return ("wait", False, False, False)
        return ("reset", False, True, False)
    if cycle_state == "stopped":
        return ("resume", True, False, False)
    return ("none", False, False, False)


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
    signal_snapshot = (
        state.cycle_manager.get_signal_snapshot()
        if state.cycle_manager is not None
        else {"providers": {}, "signals": {}}
    )
    for adapter_id, provider in signal_snapshot.get("providers", {}).items():
        if not provider.get("connected", True):
            message = f"Signal provider '{adapter_id}' is disconnected"
            if provider.get("lastError"):
                message += f": {provider['lastError']}"
            warnings.append(message)
    for signal_data in signal_snapshot.get("signals", {}).values():
        if signal_data.get("freshness") == "stale":
            warnings.append(
                "Signal "
                f"'{signal_data.get('signal')}' from adapter "
                f"'{signal_data.get('adapter')}' is stale"
            )
    delegated_recovery_request = None
    pending_interrupt = None
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
    if state.cycle_manager is not None and state.cycle_manager.pending_interrupt is not None:
        pending = state.cycle_manager.pending_interrupt
        interrupt_details = dict(pending.details)
        pending_interrupt = {
            "interruptId": pending.interrupt_id,
            "sourceId": pending.source_id,
            "sourceAdapter": pending.source_adapter,
            "signal": pending.signal,
            "kind": pending.kind,
            "priority": pending.priority,
            "maskable": pending.maskable,
            "policyId": pending.policy_id,
            "handlerId": pending.handler_id,
            "preemptionMode": pending.preemption_mode,
            "afterHandler": pending.after_handler,
            "activeNodeName": pending.active_node_name,
            "activeNodeId": pending.active_node_id,
            "activeMoveId": pending.active_move_id,
            "latchedAtNs": pending.latched_at_ns,
            "requiredAction": "none",
            "resumeAllowed": False,
            "resetRequired": False,
            "ackRequired": False,
            "timing": _interrupt_timing_payload(interrupt_details),
            "details": interrupt_details,
        }
        handler_state = interrupt_details.get("handler_state")
        if cycle_state == "interrupt_handler":
            warnings.append(
                f"Interrupt handler '{pending.handler_id or pending.source_id}' is executing "
                f"for source '{pending.source_id}'"
            )
        else:
            warnings.append(
                f"Interrupt latched from '{pending.source_id}' ({pending.preemption_mode} -> {pending.after_handler})"
            )
        if isinstance(handler_state, str) and handler_state not in {"latched", "running"}:
            warnings.append(
                f"Interrupt handler state: {handler_state}"
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

    required_operator_action, resume_allowed, reset_required, ack_required = (
        _operator_action_state(
            cycle_state=cycle_state,
            pending_interrupt=pending_interrupt,
        )
    )
    if pending_interrupt is not None:
        pending_interrupt["requiredAction"] = required_operator_action
        pending_interrupt["resumeAllowed"] = resume_allowed
        pending_interrupt["resetRequired"] = reset_required
        pending_interrupt["ackRequired"] = ack_required
    if required_operator_action != "none":
        warnings.append(f"Operator action required: {required_operator_action}")

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
        pendingInterrupt=pending_interrupt,
        requiredOperatorAction=required_operator_action,
        resumeAllowed=resume_allowed,
        resetRequired=reset_required,
        ackRequired=ack_required,
        timestampNs=time.monotonic_ns(),
    ).model_dump(by_alias=True)


def create_execution_router(state: ComposerState) -> APIRouter:
    """Create execution observability routes."""
    router = APIRouter()

    @router.get("/history")
    def get_execution_history(
        run_id: str | None = None,
        limit: int | None = None,
        start_ns: int | None = Query(default=None, alias="startNs"),
        end_ns: int | None = Query(default=None, alias="endNs"),
        min_severity: str | None = Query(default=None, alias="minSeverity"),
        components: str | None = Query(default=None),
        event_types: str | None = Query(default=None, alias="eventTypes"),
        execution_id: int | None = Query(default=None, alias="executionId"),
        program_run_id: str | None = Query(default=None, alias="programRunId"),
        intervention_run_id: str | None = Query(default=None, alias="interventionRunId"),
    ) -> dict[str, Any]:
        """Return execution history for the current or most recent run."""
        return build_execution_history_payload(
            state,
            run_id=run_id,
            limit=limit,
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=_split_csv_query(components),
            event_types=_split_csv_query(event_types),
            execution_id=execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
        )

    @router.get("/digest")
    def get_execution_digest() -> dict[str, Any]:
        """Return high-level execution digest."""
        return build_execution_digest_payload(state)

    @router.get("/blackboard")
    def get_blackboard_snapshot() -> dict[str, Any]:
        """Return a JSON-safe blackboard snapshot."""
        return build_blackboard_snapshot_payload(state)

    return router
