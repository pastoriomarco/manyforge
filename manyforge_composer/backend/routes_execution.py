"""Execution observability routes - /api/execution/*"""
from __future__ import annotations

import time
from typing import Any, Iterable

from fastapi import APIRouter, HTTPException, Query, Request

from manyforge_behavior.cycle_manager import preferred_structured_error_code
from manyforge_behavior.cycle_recovery import (
    CycleRecoveryActionId,
    RUN_AUTOMATIC_RECOVERY_ACTION_IDS,
)
from .cycle_diagnostics import manager_cycle_error_fields
from .event_recorder import _wall_iso_to_ns
from .operator_alarms import (
    AlarmAcknowledgementMember,
    OperatorAlarmError,
)

from .models import (
    BlackboardSnapshotResponse,
    ExecutionDigestObjectResponse,
    ExecutionDigestResponse,
    ExecutionHistoryResponse,
    OperatorAlarmAcknowledgeAllRequest,
    OperatorAlarmAcknowledgeRequest,
    OperatorAlarmAcknowledgeResponse,
    OperatorAlarmSnapshotResponse,
)
from .route_execution import run_route_background
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
    kernel_execution_id: int | None = None,
    program_run_id: str | None = None,
    intervention_run_id: str | None = None,
    action_span_id: str | None = None,
    node_instance_id: str | None = None,
    node_attempt_id: str | None = None,
    move_id: str | None = None,
    contact_policy_id: str | None = None,
    from_wall_ns: int | None = None,
    to_wall_ns: int | None = None,
) -> dict[str, Any]:
    """Build the execution history payload."""
    if (
        (from_wall_ns is not None or to_wall_ns is not None)
        and (start_ns is not None or end_ns is not None)
    ):
        raise HTTPException(
            status_code=400,
            detail=(
                "fromWall/toWall cross-run windows cannot be combined with "
                "startNs/endNs monotonic filters"
            ),
        )
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
            kernel_execution_id=kernel_execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
            action_span_id=action_span_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            move_id=move_id,
            contact_policy_id=contact_policy_id,
            from_wall_ns=from_wall_ns,
            to_wall_ns=to_wall_ns,
        ),
    ).model_dump(by_alias=True)


def build_execution_channels_payload(
    state: ComposerState,
    *,
    limit: int | None = None,
) -> dict[str, Any]:
    """Build protected per-channel execution history payload."""
    state.event_recorder.observe(state)
    return {
        "channels": state.event_recorder.channel_histories(limit=limit),
        "incidentSnapshots": state.event_recorder.incident_snapshots(),
        "timestampNs": time.monotonic_ns(),
    }


def build_operator_alarms_payload(
    state: ComposerState,
    *,
    limit: int = 3,
    states: list[str] | None = None,
) -> dict[str, Any]:
    """Return the backend-driven condition/alarm snapshot without side effects."""

    return OperatorAlarmSnapshotResponse(
        **state.operator_alarm_manager.snapshot(limit=limit, states=states)
    ).model_dump(by_alias=True)


def _operator_alarm_http_error(
    state: ComposerState,
    error: OperatorAlarmError,
) -> HTTPException:
    status_code = (
        404
        if error.code == "operator_alarm_not_found"
        else 409
        if error.code
        in {
            "operator_alarm_activation_stale",
            "operator_alarm_record_revision_stale",
            "operator_alarm_manager_generation_stale",
            "operator_alarm_membership_mismatch",
        }
        else 400
    )
    return HTTPException(
        status_code=status_code,
        detail={
            "message": str(error),
            "errorCode": error.code,
            "errorDetails": {
                "operatorAlarmSnapshot": state.operator_alarm_manager.snapshot(
                    limit=3
                )
            },
        },
    )


def _split_csv_query(value: str | None) -> list[str] | None:
    if value is None:
        return None
    items = [item.strip() for item in value.split(",") if item.strip()]
    return items or None


def _parse_wall_query(name: str, value: str | None) -> int | None:
    """Parse an ISO-8601 wall-clock query bound to epoch ns, or 400."""
    if value is None or not value.strip():
        return None
    parsed = _wall_iso_to_ns(value)
    if parsed is None:
        raise HTTPException(
            status_code=400,
            detail=f"{name} must be an ISO-8601 timestamp (e.g. 2026-07-07T06:00:00Z)",
        )
    return parsed


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

    cycle_state = (
        state.cycle_manager.state.value
        if state.cycle_manager is not None
        else "idle"
    )
    motion_stop_state = (
        state.cycle_manager.motion_stop_state
        if state.cycle_manager is not None
        else "idle"
    )
    velocity_scale = (
        state.cycle_manager.velocity_scale
        if state.cycle_manager is not None
        else 1.0
    )
    node_statuses = state.cycle_manager.get_node_statuses() if state.cycle_manager is not None else {}
    kernel_status = (
        state.cycle_manager.get_status()
        if state.cycle_manager is not None
        else state.kernel.get_status()
        if state.kernel is not None
        else None
    )
    kernel_snapshot = (
        state.cycle_manager.get_execution_snapshot()
        if state.cycle_manager is not None
        else state.kernel.get_execution_snapshot()
        if state.kernel is not None
        else None
    )
    snapshot_error_details = (
        getattr(kernel_snapshot, "error_details", None)
        if kernel_snapshot is not None
        else None
    )
    if not snapshot_error_details:
        snapshot_error_details = None
    cycle_error = None
    cycle_error_code = None
    cycle_error_details = None
    if state.cycle_manager is not None:
        (
            cycle_error,
            cycle_error_code,
            cycle_error_details,
        ) = manager_cycle_error_fields(state.cycle_manager)
    error_details = cycle_error_details or snapshot_error_details
    raw_error_code = (
        cycle_error_code
        if cycle_error_code
        else getattr(kernel_snapshot, "error_code", None)
        if kernel_snapshot is not None
        else None
    )
    error_code = preferred_structured_error_code(raw_error_code, error_details)
    error_message = (
        cycle_error
        if cycle_error is not None
        else getattr(kernel_snapshot, "error", None)
        if kernel_snapshot is not None
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
        if scene_diagnostics.get("summary", {}).get("viewerAvailable", False):
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

    recovery_offer = (
        state.cycle_manager.get_recovery_offer()
        if state.cycle_manager is not None
        else None
    )
    enabled_actions = {
        str(action.get("actionId"))
        for action in (recovery_offer or {}).get("actions", [])
        if action.get("enabled")
    }
    primary_action = (recovery_offer or {}).get("primaryAction")
    required_operator_action = str(primary_action or "none")
    if (recovery_offer or {}).get("status") == "action_required":
        required_operator_action = "choose_recovery_action"
    kernel_state = getattr(getattr(kernel_status, "state", None), "value", "")
    ordinary_resume_available = bool(
        recovery_offer is None
        and cycle_state in {"running", "stopped"}
        and motion_stop_state == "held"
        and kernel_state in {"paused", "stopped", "idle"}
        and error_code is None
        and error_message is None
    )
    if ordinary_resume_available:
        # The ordinary Stop/Pause lifecycle is deliberately outside the
        # versioned recovery-offer surface (spec/450 section 5.1).
        required_operator_action = "resume"
    resume_allowed = bool(
        enabled_actions
        & RUN_AUTOMATIC_RECOVERY_ACTION_IDS
    ) or ordinary_resume_available
    reset_required = (recovery_offer or {}).get("status") == "reset_required"
    ack_required = (
        CycleRecoveryActionId.ACKNOWLEDGE_AND_CONTINUE.value
        in enabled_actions
    )
    if required_operator_action != "none":
        warnings.append(f"Operator action required: {required_operator_action}")
    latest_incident_snapshots = (
        state.event_recorder.incident_snapshots(limit=8)
        if error_message or error_code
        else []
    )
    latest_incident = None
    for candidate in reversed(latest_incident_snapshots):
        if not isinstance(candidate, dict):
            continue
        headline = candidate.get("headlineCause")
        if candidate.get("actionSpan") or (
            isinstance(headline, dict) and headline.get("genericWrapperSuppressed")
        ):
            latest_incident = candidate
            break
    if latest_incident is None and latest_incident_snapshots:
        latest_incident = latest_incident_snapshots[-1]
    active_action_span = (
        latest_incident.get("actionSpan")
        if isinstance(latest_incident, dict)
        and isinstance(latest_incident.get("actionSpan"), dict)
        else None
    )
    headline_cause = (
        latest_incident.get("headlineCause")
        if isinstance(latest_incident, dict)
        and isinstance(latest_incident.get("headlineCause"), dict)
        else None
    )
    recovery = (
        str(latest_incident.get("recovery"))
        if isinstance(latest_incident, dict) and latest_incident.get("recovery")
        else None
    )

    return ExecutionDigestResponse(
        state=cycle_state,
        motionStopState=motion_stop_state,
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
        error=error_message,
        errorCode=error_code,
        errorDetails=error_details,
        activeActionSpan=active_action_span,
        headlineCause=headline_cause,
        recovery=recovery,
        warnings=warnings,
        runtimeOverrideState=runtime_override_state,
        delegatedRecoveryRequest=delegated_recovery_request,
        requiredOperatorAction=required_operator_action,
        resumeAllowed=resume_allowed,
        resetRequired=reset_required,
        ackRequired=ack_required,
        recoveryOffer=recovery_offer,
        lastStopDecision=state.event_recorder.last_stop_decision(),
        timestampNs=time.monotonic_ns(),
    ).model_dump(by_alias=True)


def create_execution_router(state: ComposerState) -> APIRouter:
    """Create execution observability routes."""
    router = APIRouter()

    @router.get(
        "/alarms",
        response_model=OperatorAlarmSnapshotResponse,
        response_model_by_alias=True,
    )
    async def get_operator_alarms(
        limit: int = Query(default=100, ge=0, le=1000),
        states: str | None = Query(default=None),
    ) -> dict[str, Any]:
        """Return the four-state condition-backed alarm projection."""

        def read_snapshot() -> dict[str, Any]:
            try:
                return build_operator_alarms_payload(
                    state,
                    limit=limit,
                    states=_split_csv_query(states),
                )
            except OperatorAlarmError as exc:
                raise _operator_alarm_http_error(state, exc) from exc

        return await run_route_background(
            read_snapshot
        )

    @router.post(
        "/alarms/acknowledge-all",
        response_model=OperatorAlarmAcknowledgeResponse,
        response_model_by_alias=True,
    )
    async def acknowledge_all_operator_alarms(
        payload: OperatorAlarmAcknowledgeAllRequest,
        request: Request,
    ) -> dict[str, Any]:
        """Acknowledge the exact activation/revision membership atomically."""
        principal_id = getattr(request.state, "principal_id", None) or "operator_ui"

        def command() -> dict[str, Any]:
            try:
                result = state.operator_alarm_manager.acknowledge_all(
                    members=(
                        AlarmAcknowledgementMember(
                            condition_id=member.condition_id,
                            activation_id=member.activation_id,
                            record_revision=member.record_revision,
                        )
                        for member in payload.members
                    ),
                    expected_generation=payload.expected_generation,
                    principal_id=principal_id,
                    reason=payload.reason,
                    confirmed=payload.confirmed,
                )
            except OperatorAlarmError as exc:
                raise _operator_alarm_http_error(state, exc) from exc
            audit_indices = state.persist_operator_alarm_lifecycle_facts(
                result.lifecycle_facts
            )
            response = {
                "status": result.status,
                "changed": result.changed,
                "acknowledgedConditionIds": [
                    record.condition_id for record in result.records
                ],
                "auditEventIndices": audit_indices,
                "snapshot": state.operator_alarm_manager.snapshot(limit=1000),
            }
            return OperatorAlarmAcknowledgeResponse(**response).model_dump(
                by_alias=True
            )

        return await run_route_background(command)

    @router.post(
        "/alarms/{condition_id}/acknowledge",
        response_model=OperatorAlarmAcknowledgeResponse,
        response_model_by_alias=True,
    )
    async def acknowledge_operator_alarm(
        condition_id: str,
        payload: OperatorAlarmAcknowledgeRequest,
        request: Request,
    ) -> dict[str, Any]:
        """Acknowledge one exact activation without changing motion authority."""
        principal_id = getattr(request.state, "principal_id", None) or "operator_ui"

        def command() -> dict[str, Any]:
            try:
                result = state.operator_alarm_manager.acknowledge(
                    condition_id=condition_id,
                    activation_id=payload.activation_id,
                    expected_record_revision=payload.expected_record_revision,
                    principal_id=principal_id,
                    reason=payload.reason,
                    confirmed=payload.confirmed,
                )
            except OperatorAlarmError as exc:
                raise _operator_alarm_http_error(state, exc) from exc
            audit_indices = state.persist_operator_alarm_lifecycle_facts(
                result.lifecycle_facts
            )
            response = {
                "status": result.status,
                "changed": result.changed,
                "acknowledgedConditionIds": [
                    record.condition_id for record in result.records
                ],
                "auditEventIndices": audit_indices,
                "snapshot": state.operator_alarm_manager.snapshot(limit=1000),
            }
            return OperatorAlarmAcknowledgeResponse(**response).model_dump(
                by_alias=True
            )

        return await run_route_background(command)

    @router.get("/runs")
    async def list_execution_runs(
        limit: int = Query(default=200, ge=1, le=2000),
    ) -> dict:
        """Cross-run listing from the append-only run index (plan OW5).

        Serves run ids, lineage, wall times, event counts, and max severity
        so post-restart forensics can find a run without out-of-band ids.
        """
        return await run_route_background(
            lambda: {"runs": state.event_recorder.list_runs(limit=limit)}
        )

    @router.get("/history")
    async def get_execution_history(
        run_id: str | None = None,
        limit: int | None = Query(default=None, ge=0, le=10000),
        start_ns: int | None = Query(default=None, alias="startNs"),
        end_ns: int | None = Query(default=None, alias="endNs"),
        min_severity: str | None = Query(default=None, alias="minSeverity"),
        components: str | None = Query(default=None),
        event_types: str | None = Query(default=None, alias="eventTypes"),
        execution_id: int | None = Query(default=None, alias="executionId"),
        kernel_execution_id: int | None = Query(default=None, alias="kernelExecutionId"),
        program_run_id: str | None = Query(default=None, alias="programRunId"),
        intervention_run_id: str | None = Query(default=None, alias="interventionRunId"),
        action_span_id: str | None = Query(default=None, alias="actionSpanId"),
        node_instance_id: str | None = Query(default=None, alias="nodeInstanceId"),
        node_attempt_id: str | None = Query(default=None, alias="nodeAttemptId"),
        move_id: str | None = Query(default=None, alias="moveId"),
        contact_policy_id: str | None = Query(default=None, alias="contactPolicyId"),
        from_wall: str | None = Query(default=None, alias="fromWall"),
        to_wall: str | None = Query(default=None, alias="toWall"),
    ) -> dict[str, Any]:
        """Return execution history for a single run, or — when a wall-time
        window (fromWall/toWall, ISO-8601) is given — across runs resolved via
        the run index (OW7)."""
        return await run_route_background(
            lambda: build_execution_history_payload(
                state,
                run_id=run_id,
                limit=limit,
                start_ns=start_ns,
                end_ns=end_ns,
                min_severity=min_severity,
                components=_split_csv_query(components),
                event_types=_split_csv_query(event_types),
                execution_id=execution_id,
                kernel_execution_id=kernel_execution_id,
                program_run_id=program_run_id,
                intervention_run_id=intervention_run_id,
                action_span_id=action_span_id,
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                move_id=move_id,
                contact_policy_id=contact_policy_id,
                from_wall_ns=_parse_wall_query("fromWall", from_wall),
                to_wall_ns=_parse_wall_query("toWall", to_wall),
            )
        )

    @router.get("/channels")
    async def get_execution_channels(
        limit: int | None = Query(default=None, ge=0, le=10000),
    ) -> dict[str, Any]:
        """Return independently retained event streams by diagnostic channel."""
        return await run_route_background(
            lambda: build_execution_channels_payload(state, limit=limit)
        )

    @router.get("/digest")
    async def get_execution_digest() -> dict[str, Any]:
        """Return high-level execution digest."""
        return await run_route_background(lambda: build_execution_digest_payload(state))

    @router.get("/blackboard")
    async def get_blackboard_snapshot() -> dict[str, Any]:
        """Return a JSON-safe blackboard snapshot."""
        return await run_route_background(lambda: build_blackboard_snapshot_payload(state))

    @router.get("/latency-histograms")
    async def get_latency_histograms() -> dict[str, Any]:
        """Return per-run sampled latency histograms (spec 500 §5)."""
        return await run_route_background(
            lambda: state.event_recorder.latency_histograms()
        )

    return router
