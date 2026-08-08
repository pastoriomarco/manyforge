"""Fenced Composer scenario route for the native-authoring cutover."""

from __future__ import annotations

from fastapi import APIRouter, HTTPException

from .state import ComposerState

SCENARIO_ROUTE_UNAVAILABLE_REASON_CODE = "composable_cell_scenario_route_unavailable"


def create_scenarios_router(state: ComposerState) -> APIRouter:
    del state
    router = APIRouter()

    @router.get("")
    def list_scenarios() -> dict:
        raise HTTPException(
            status_code=503,
            detail={
                "code": SCENARIO_ROUTE_UNAVAILABLE_REASON_CODE,
                "message": (
                    "scenario discovery returns with native Program/addressing "
                    "at I6/N"
                ),
                "restorationGates": ["I6", "N"],
            },
        )

    return router
