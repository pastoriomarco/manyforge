"""Bounded Composer routes for physical-robot lifecycle operations."""
from __future__ import annotations

from typing import Any

from fastapi import APIRouter, Body, HTTPException
from pydantic import BaseModel, ConfigDict, StrictBool

from .route_execution import run_cycle_lifecycle
from .state import ComposerState


class RobotLifecycleConfirmationRequest(BaseModel):
    """Strict explicit confirmation shared by bounded lifecycle intents."""

    model_config = ConfigDict(extra="forbid")
    confirmed: StrictBool


def _confirmed(
    payload: RobotLifecycleConfirmationRequest,
    operation: str,
) -> None:
    if payload.confirmed is not True:
        raise HTTPException(
            status_code=409,
            detail={
                "message": f"Explicit confirmation is required to {operation}.",
                "errorCode": "robot_lifecycle_confirmation_required",
                "errorDetails": {"operation": operation},
            },
        )


def _command_error(operation: str, exc: Exception) -> HTTPException:
    reason = str(exc) or type(exc).__name__
    return HTTPException(
        status_code=409,
        detail={
            "message": f"Robot lifecycle {operation} did not complete: {reason}",
            "errorCode": "robot_lifecycle_command_failed",
            "errorDetails": {
                "operation": operation,
                "reason": reason,
            },
        },
    )


def create_robot_lifecycle_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/status")
    def status() -> dict[str, Any]:
        return state.robot_lifecycle_status()

    @router.post("/enable")
    async def enable(
        payload: RobotLifecycleConfirmationRequest = Body(...),
    ) -> dict[str, Any]:
        _confirmed(payload, "enable the robot")

        def command() -> dict[str, Any]:
            try:
                return state.enable_robot_lifecycle()
            except (RuntimeError, ValueError) as exc:
                raise _command_error("enable", exc) from exc

        return await run_cycle_lifecycle(command)

    @router.post("/disable")
    async def disable(
        payload: RobotLifecycleConfirmationRequest = Body(...),
    ) -> dict[str, Any]:
        _confirmed(payload, "stop the cycle and disable the robot")

        def command() -> dict[str, Any]:
            try:
                return state.disable_robot_lifecycle()
            except (RuntimeError, ValueError) as exc:
                raise _command_error("disable", exc) from exc

        return await run_cycle_lifecycle(command)

    @router.post("/recovery/{action_id}")
    async def recovery(
        action_id: str,
        payload: RobotLifecycleConfirmationRequest = Body(...),
    ) -> dict[str, Any]:
        allowed = {
            "clear-fault": "robot.clear_fault",
            "clear-warning": "robot.clear_warning",
            "shutdown-controller": "shutdown_controller",
        }
        action = allowed.get(action_id)
        if action is None:
            raise HTTPException(
                status_code=404,
                detail="Unsupported robot lifecycle recovery action",
            )
        _confirmed(payload, action_id.replace("-", " "))

        def command() -> dict[str, Any]:
            try:
                return state.robot_lifecycle_recovery_action(action)
            except (RuntimeError, ValueError) as exc:
                raise _command_error(action_id, exc) from exc

        return await run_cycle_lifecycle(command)

    @router.post("/reconcile")
    async def reconcile(
        payload: RobotLifecycleConfirmationRequest = Body(...),
    ) -> dict[str, Any]:
        _confirmed(payload, "reconcile the uncertain lifecycle outcome")

        def command() -> dict[str, Any]:
            try:
                return state.reconcile_robot_lifecycle()
            except (RuntimeError, ValueError) as exc:
                raise _command_error("reconcile", exc) from exc

        return await run_cycle_lifecycle(command)

    return router
