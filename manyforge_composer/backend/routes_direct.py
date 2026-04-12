"""Direct-control / intervention routes - /api/direct/*"""
from __future__ import annotations

import logging
import time

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
from manyforge_behavior.program import collect_tree_validation_issues, tree_node_from_dict

from .models import (
    DelegatedRecoveryRequestResponse,
    InterventionExecuteRequest,
    InterventionPromoteRequest,
    InterventionSessionResponse,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


def _deployment_capability_keys(state: ComposerState) -> set[str] | None:
    if state.deployment is None:
        return None
    capability_keys: set[str] = set()
    for adapter in state.deployment.adapters.values():
        capability_keys.update(adapter.capability_keys)
    return capability_keys


def _parent_run_id_for_intervention(state: ComposerState) -> str | None:
    if state.cycle_manager is None:
        return None
    current = state.event_recorder.current_run_id
    if current is not None:
        return current
    if state.cycle_manager.state.value == "stopped":
        return state.event_recorder.last_run_id
    return None


def _session_response(session, status_code: int = 200) -> JSONResponse:
    """Build a JSON response from an InterventionSession."""
    return JSONResponse(
        status_code=status_code,
        content=InterventionSessionResponse(
            session_id=session.session_id,
            session_kind=session.session_kind,
            authority_source=session.authority_source,
            principal_id=session.principal_id,
            grant_type=session.grant_type,
            parent_run_id=session.parent_run_id,
            entry_state=session.entry_state,
            lease_timeout_ms=session.lease_timeout_ms,
            lease_expires_at_ns=session.lease_expires_at_ns,
            resume_target=session.resume_target,
            trigger_reason=session.trigger_reason,
            trigger_node_instance=session.trigger_node_instance,
            approval_mode=session.approval_mode,
            artifact_origin=session.artifact_origin,
            status=session.status,
            actual_outcome=session.actual_outcome,
            error=session.error,
            started_at_ns=session.started_at_ns,
            finished_at_ns=session.finished_at_ns,
            events=list(session.events),
            promotions=list(session.promotions),
            timestamp_ns=time.monotonic_ns(),
        ).model_dump(by_alias=True),
    )


def _delegated_recovery_response(request, status_code: int = 200) -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content=DelegatedRecoveryRequestResponse(
            request_id=request.request_id,
            parent_run_id=request.parent_run_id,
            requested_from_state=request.requested_from_state,
            trigger_node_instance=request.trigger_node_instance,
            trigger_node_id=request.trigger_node_id,
            trigger_reason=request.trigger_reason,
            delegate_policy=request.delegate_policy,
            allowed_authority_sources=list(request.allowed_authority_sources),
            allowed_actions=list(request.allowed_actions),
            allowed_resume_targets=list(request.allowed_resume_targets),
            default_resume_target=request.default_resume_target,
            runtime_override_policy=request.runtime_override_policy,
            persistent_edit_policy=request.persistent_edit_policy,
            requested_at_ns=request.requested_at_ns,
            timestamp_ns=time.monotonic_ns(),
        ).model_dump(by_alias=True),
    )


def create_direct_router(state: ComposerState) -> APIRouter:
    """Create the direct-control router with the given state."""
    router = APIRouter()

    @router.post("/execute")
    def execute_intervention(request: InterventionExecuteRequest, http_request: Request):
        """Execute a temporary subtree as a direct-control intervention.

        Requires the cycle to be IDLE or STOPPED. The subtree runs on a
        dedicated tick thread; the manager transitions to DIRECT_CONTROL
        while it executes and then restores the pre-intervention session state
        or the explicit resume target.
        """
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Use server-authoritative principal from auth middleware when available.
        # Falls back to body-supplied principal_id when auth is disabled (dev).
        auth_principal = getattr(http_request.state, "principal_id", None)
        effective_principal = auth_principal or request.principal_id

        tree_dict = request.tree.model_dump()
        try:
            tree = tree_node_from_dict(tree_dict)
        except Exception as exc:
            return JSONResponse(
                status_code=400,
                content={
                    "error": f"Invalid intervention tree: {exc}",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        errors, _warnings = collect_tree_validation_issues(
            tree,
            state.catalog,
            parameter_defaults=(
                state.program.get_parameter_defaults()
                if state.program is not None
                else None
            ),
            skill_catalog=state.skill_catalog,
            program_profile=(
                state.program.program_profile
                if state.program is not None
                else None
            ),
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        if errors:
            return JSONResponse(
                status_code=400,
                content={
                    "error": errors[0].message,
                    "details": [{"path": issue.path, "message": issue.message} for issue in errors],
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.execute_intervention(
                tree_dict,
                parent_run_id=_parent_run_id_for_intervention(state),
                lease_timeout_ms=request.lease_timeout_ms,
                resume_target=request.resume_target,
                session_kind=request.session_kind,
                authority_source=request.authority_source,
                principal_id=effective_principal,
                grant_type=request.grant_type,
                trigger_reason=request.trigger_reason,
                trigger_node_instance=request.trigger_node_instance,
                approval_mode=request.approval_mode,
                artifact_origin=request.artifact_origin,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _session_response(session)

    @router.post("/heartbeat")
    def renew_intervention_lease(lease_timeout_ms: int | None = None):
        """Renew the active intervention lease, optionally changing its duration."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.renew_intervention_lease(
                lease_timeout_ms=lease_timeout_ms,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        return _session_response(session)

    @router.post("/promote")
    def promote_intervention_blackboard(request: InterventionPromoteRequest):
        """Promote scoped intervention blackboard values into parent keys."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.promote_intervention_values(
                request.promote_keys,
                session_id=request.session_id,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        return _session_response(session)

    @router.get("/status")
    def get_intervention_status():
        """Get the current or most recent intervention session status."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        session = state.cycle_manager.intervention
        if session is None:
            return JSONResponse(
                status_code=404,
                content={
                    "error": "No intervention session",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _session_response(session)

    @router.get("/pending-recovery")
    def get_pending_delegated_recovery():
        """Get the currently pending delegated recovery request, if any."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        request = state.cycle_manager.pending_delegated_recovery
        if request is None:
            return JSONResponse(
                status_code=404,
                content={
                    "error": "No delegated recovery request",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _delegated_recovery_response(request)

    @router.post("/abort")
    def abort_intervention():
        """Abort a running intervention and restore the pre-intervention state."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        session = state.cycle_manager.intervention
        if session is None or session.status != "running":
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No running intervention to abort",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        state.cycle_manager.abort_intervention()

        session = state.cycle_manager.intervention
        return _session_response(session)

    return router
