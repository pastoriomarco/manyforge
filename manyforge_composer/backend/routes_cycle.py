"""Cycle control routes - /api/cycle/*"""
from __future__ import annotations

import logging
from typing import Any, Optional

from fastapi import APIRouter, HTTPException
from .models import (
    CycleCommandErrorResponse,
    CycleStatusResponse,
    CycleCommandResponse,
    VelocityScaleRequest,
    VelocityScaleResponse,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


def _structured_command_error(
    state: ComposerState,
    message: str,
    *,
    error_code: Optional[str] = None,
) -> dict[str, Any]:
    """Build a structured `detail` payload for a cycle-command 409.

    The native viewer renders `errorCode` + `errorDetails` into a cause panel so the
    operator sees the actual kernel diagnostics (collision pair, waypoint
    index, fault reason code) instead of only the human-readable message.
    Pulls structured fields from the cycle manager when available, falling
    back to the explicit `error_code` argument for cases the cycle manager
    doesn't track (e.g. "no program loaded").
    """
    code = error_code
    details: Optional[dict[str, Any]] = None
    manager = state.cycle_manager
    if manager is not None:
        code = code or manager.error_code
        details = manager.error_details
    return CycleCommandErrorResponse(
        message=message,
        error_code=code,
        error_details=details,
    ).model_dump(by_alias=True)


def _refresh_visual_scene_after_reset(state: ComposerState) -> None:
    """Drop stale observer state and republish the post-reset scene."""
    state.clear_visual_state_snapshot()
    state.clear_visual_scene_objects_snapshot()
    refresh_result = state.refresh_visual_scene_objects_sync(reason="cycle_reset")
    if not refresh_result.get("success", False):
        logger.warning(
            "cycle reset visual scene refresh failed: %s",
            refresh_result.get("error"),
        )
    state.request_visual_state_publish(reason="cycle_reset")


def create_cycle_router(state: ComposerState) -> APIRouter:
    """Create the cycle router with the given state."""
    router = APIRouter()
    
    @router.get("/status", response_model=CycleStatusResponse, response_model_by_alias=True)
    def get_status() -> dict:
        """Get current cycle status.

        The error fields surface the kernel/cycle-manager's structured fault
        so the viewer can render a cause panel without polling the kernel
        snapshot separately. Empty when no error is active.
        """
        if state.cycle_manager is None:
            return {
                "state": "idle",
                "velocityScale": 1.0,
                "error": None,
                "errorCode": None,
                "errorDetails": None,
            }
        manager = state.cycle_manager
        return {
            "state": manager.state.value,
            "velocityScale": manager.velocity_scale,
            "error": manager.error,
            "errorCode": manager.error_code,
            "errorDetails": manager.error_details,
        }

    @router.post("/run", response_model=CycleCommandResponse, response_model_by_alias=True)
    def run_cycle() -> dict:
        """Start the cycle (IDLE→RUNNING or STOPPED→RUNNING)."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )
        override_status = state.runtime_override_status()
        if (
            state.cycle_manager.state.value == "stopped"
            and override_status["has_staged"]
        ):
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state,
                    (
                        "Staged runtime overrides cannot be activated while "
                        "resuming from STOPPED; reset the cycle first"
                    ),
                    error_code="staged_overrides_blocked_resume",
                ),
            )
        if (
            state.cycle_manager.state.value == "idle"
            and override_status["has_staged"]
        ):
            try:
                state.activate_staged_runtime_overrides()
            except RuntimeError as exc:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state,
                        str(exc),
                        error_code="staged_overrides_activate_failed",
                    ),
                )
        prev = state.cycle_manager.state.value
        try:
            state.cycle_manager.run()
        except RuntimeError as exc:
            # Cycle-manager Run-side auto-ack refusals (e.g. "Cannot
            # auto-acknowledge kernel INTERVENTION_REQUIRED on Run:
            # intervention_session_running.") surface as RuntimeError.
            # Translate to a structured 409 so the viewer can render the
            # kernel's actual cause from the cycle-manager's preserved
            # error_code / error_details instead of a generic banner.
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(state, str(exc)),
            )
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.post("/stop", response_model=CycleCommandResponse, response_model_by_alias=True)
    def stop_cycle() -> dict:
        """Pause/hold the cycle (RUNNING→STOPPED); hard kernel stop is separate."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )
        prev = state.cycle_manager.state.value
        state.cycle_manager.stop()
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.post("/reset", response_model=CycleCommandResponse, response_model_by_alias=True)
    def reset_cycle() -> dict:
        """Reset the cycle (any→IDLE)."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )
        override_status = state.runtime_override_status()
        prev = state.cycle_manager.state.value
        if override_status["has_staged"]:
            try:
                state.activate_staged_runtime_overrides()
            except RuntimeError as exc:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state,
                        str(exc),
                        error_code="staged_overrides_activate_failed",
                    ),
                )
        else:
            state.cycle_manager.reset()
        try:
            state.reset_backend_authoritative_sim_if_needed(reason="cycle_reset")
        except RuntimeError as exc:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, str(exc), error_code="backend_sim_reset_failed"
                ),
            )
        _refresh_visual_scene_after_reset(state)
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.put("/velocity-scale", response_model=VelocityScaleResponse, response_model_by_alias=True)
    def set_velocity_scale(request: VelocityScaleRequest) -> dict:
        """Set the velocity scale (0.0-1.0)."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )
        state.cycle_manager.set_velocity_scale(
            request.value,
            principal_id=request.principal_id,
        )
        logger.info("Velocity scale set to %.2f", request.value)
        return {"velocityScale": state.cycle_manager.velocity_scale}
    
    return router
