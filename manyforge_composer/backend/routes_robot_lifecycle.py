"""Strict operator-confirmation model and the fenced I7/L0 route factory."""

from __future__ import annotations

from typing import Final

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, ConfigDict, StrictBool

from .robot_lifecycle import (
    ROBOT_LIFECYCLE_CLIENT_UNAVAILABLE_REASON_CODE,
    RobotLifecycleClientUnavailableError,
)


ROBOT_LIFECYCLE_ROUTE_RESTORATION_GATES: Final = ("I7", "L0")


class RobotLifecycleConfirmationRequest(BaseModel):
    """Transport-neutral explicit confirmation for a future bounded intent."""

    model_config = ConfigDict(extra="forbid")
    confirmed: StrictBool


def _confirmed(
    payload: RobotLifecycleConfirmationRequest,
    operation: str,
) -> None:
    """Reject false confirmation without selecting lifecycle policy."""
    if payload.confirmed is not True:
        raise HTTPException(
            status_code=409,
            detail={
                "message": f"Explicit confirmation is required to {operation}.",
                "error_code": "robot_lifecycle_confirmation_required",
                "error_details": {"operation": operation},
            },
        )


def create_robot_lifecycle_router(_state: object) -> APIRouter:
    """Fail before state access, route closure creation, or robot effects."""
    raise RobotLifecycleClientUnavailableError(
        f"{ROBOT_LIFECYCLE_CLIENT_UNAVAILABLE_REASON_CODE}: lifecycle routes "
        "return at I7/L0 on the caller-neutral open-request contract"
    )


__all__ = [
    "RobotLifecycleConfirmationRequest",
    "create_robot_lifecycle_router",
]
