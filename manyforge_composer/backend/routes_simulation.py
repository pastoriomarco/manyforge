"""Simulator lifecycle/control routes - /api/simulation/*"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException

from .models import (
    SimulationControlResponse,
    SimulationResetResponse,
    SimulationSetEntityStateRequest,
    SimulationSetStateRequest,
    SimulationStepRequest,
)
from .state import ComposerState


def create_simulation_router(state: ComposerState) -> APIRouter:
    """Create the simulator-control router with the given state."""
    router = APIRouter()

    @router.post(
        "/reset",
        response_model=SimulationResetResponse,
        response_model_by_alias=True,
    )
    def reset_simulation() -> dict[str, Any]:
        """Reset simulator state through the deployment control contract."""
        try:
            return state.reset_simulation(
                reason="api_simulation_reset",
                source="api",
                allow_running=False,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @router.post(
        "/state",
        response_model=SimulationControlResponse,
        response_model_by_alias=True,
    )
    def set_simulation_state(request: SimulationSetStateRequest) -> dict[str, Any]:
        """Set simulator lifecycle state through the deployment control contract."""
        try:
            return state.set_simulation_state(
                state=request.state,
                reason=request.reason or "api_simulation_state",
                source="api",
                allow_running=False,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @router.post(
        "/step",
        response_model=SimulationControlResponse,
        response_model_by_alias=True,
    )
    def step_simulation(request: SimulationStepRequest) -> dict[str, Any]:
        """Step a paused simulator through the deployment control contract."""
        try:
            return state.step_simulation(
                steps=request.steps,
                reason=request.reason or "api_simulation_step",
                source="api",
                allow_running=False,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @router.post(
        "/entity-state",
        response_model=SimulationControlResponse,
        response_model_by_alias=True,
    )
    def set_entity_state(request: SimulationSetEntityStateRequest) -> dict[str, Any]:
        """Set simulator entity pose through the deployment control contract."""
        try:
            return state.set_simulation_entity_state(
                entity=request.entity,
                pose=request.pose,
                reason=request.reason or "api_simulation_entity_state",
                source="api",
                allow_running=False,
            )
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    return router
