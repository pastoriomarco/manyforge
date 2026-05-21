"""Cycle control routes - /api/cycle/*"""
from __future__ import annotations

import logging

from fastapi import APIRouter, HTTPException
from .models import (
    CycleStatusResponse,
    CycleCommandResponse,
    VelocityScaleRequest,
    VelocityScaleResponse,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


def create_cycle_router(state: ComposerState) -> APIRouter:
    """Create the cycle router with the given state."""
    router = APIRouter()
    
    @router.get("/status", response_model=CycleStatusResponse, response_model_by_alias=True)
    def get_status() -> dict:
        """Get current cycle status."""
        if state.cycle_manager is None:
            return {"state": "idle", "velocityScale": 1.0}
        return {
            "state": state.cycle_manager.state.value,
            "velocityScale": state.cycle_manager.velocity_scale,
        }
    
    @router.post("/run", response_model=CycleCommandResponse, response_model_by_alias=True)
    def run_cycle() -> dict:
        """Start the cycle (IDLE→RUNNING or STOPPED→RUNNING)."""
        if state.cycle_manager is None:
            raise HTTPException(status_code=409, detail="No program loaded")
        override_status = state.runtime_override_status()
        if (
            state.cycle_manager.state.value == "stopped"
            and override_status["has_staged"]
        ):
            raise HTTPException(
                status_code=409,
                detail=(
                    "Staged runtime overrides cannot be activated while resuming from STOPPED; "
                    "reset the cycle first"
                ),
            )
        if (
            state.cycle_manager.state.value == "idle"
            and override_status["has_staged"]
        ):
            try:
                state.activate_staged_runtime_overrides()
            except RuntimeError as exc:
                raise HTTPException(status_code=409, detail=str(exc))
        prev = state.cycle_manager.state.value
        try:
            state.cycle_manager.run()
        except RuntimeError as exc:
            # Cycle-manager Run-side auto-ack refusals (e.g. "Cannot
            # auto-acknowledge kernel INTERVENTION_REQUIRED on Run:
            # intervention_session_running.") surface as RuntimeError.
            # Translate to a structured 409 so clients see a clean
            # conflict rather than a generic 500.
            raise HTTPException(status_code=409, detail=str(exc))
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.post("/stop", response_model=CycleCommandResponse, response_model_by_alias=True)
    def stop_cycle() -> dict:
        """Stop the cycle (RUNNING→STOPPED)."""
        if state.cycle_manager is None:
            raise HTTPException(status_code=409, detail="No program loaded")
        prev = state.cycle_manager.state.value
        state.cycle_manager.stop()
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.post("/reset", response_model=CycleCommandResponse, response_model_by_alias=True)
    def reset_cycle() -> dict:
        """Reset the cycle (any→IDLE)."""
        if state.cycle_manager is None:
            raise HTTPException(status_code=409, detail="No program loaded")
        override_status = state.runtime_override_status()
        prev = state.cycle_manager.state.value
        if override_status["has_staged"]:
            try:
                state.activate_staged_runtime_overrides()
            except RuntimeError as exc:
                raise HTTPException(status_code=409, detail=str(exc))
        else:
            state.cycle_manager.reset()
        try:
            state.reset_backend_authoritative_sim_if_needed(reason="cycle_reset")
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        return {"state": state.cycle_manager.state.value}

    @router.put("/velocity-scale", response_model=VelocityScaleResponse, response_model_by_alias=True)
    def set_velocity_scale(request: VelocityScaleRequest) -> dict:
        """Set the velocity scale (0.0-1.0)."""
        if state.cycle_manager is None:
            raise HTTPException(status_code=409, detail="No program loaded")
        state.cycle_manager.set_velocity_scale(request.value)
        logger.info("Velocity scale set to %.2f", request.value)
        return {"velocityScale": state.cycle_manager.velocity_scale}
    
    return router
