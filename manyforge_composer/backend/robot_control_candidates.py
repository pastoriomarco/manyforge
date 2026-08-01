"""Robot Control value seeds and the typed I7/I8 client gate.

This module deliberately contains only transport-neutral strict draft values,
preview presentation values, and stable reason codes.  It deliberately does
not retain the old unaddressed ``CandidateCreateRequest``, status state
machine, default ``frame_id='base'`` Cartesian target, or unitless gripper
target.  Those definitions encoded the singleton-era request wire rather than
neutral validation.  Their exact source is historical blob
``18ac258fea001dfda9cbe9e31eb5df057960df2a`` (class cohort lines 392-771),
with 73 acceptance definitions in historical test blob
``43172a3df1909e2faecd6ff0cccd0cd11a1ff405``.

The retained joint and presentation values are not a complete request or
response contract and cannot establish component, command-target, resource,
control-domain, planning, or execution truth.  The addressed client wire
returns only with its I6/I7/I8 owners.
"""

from __future__ import annotations

import math
from typing import Any, Final, Optional

from fastapi import APIRouter
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    field_validator,
    model_validator,
)


ROBOT_CONTROL_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_robot_control_client_unavailable"
)
ROBOT_CONTROL_RESTORATION_GATES: Final = ("I7", "I8")


class RobotControlClientUnavailableError(RuntimeError):
    """Raised before any candidate, lifecycle, planning, or motion effect."""


_PRESENTATION_CFG = ConfigDict(extra="forbid")
_STRICT_CFG = ConfigDict(extra="forbid")


# Stable presentation codes.  They are client vocabulary, not proof that the
# corresponding planning/execution owner currently exists.
REASON_READY = "ready"
REASON_IK_UNREACHABLE = "ik_unreachable"
REASON_JOINT_LIMIT = "joint_limit_violation"
REASON_COLLISION = "collision_detected"
REASON_VALIDATION_TIMEOUT = "validation_timeout"
REASON_VALIDATION_DEFERRED = "validation_deferred_runtime_monitor_active"
REASON_MALFORMED_TARGET = "malformed_target"
REASON_BACKEND_UNAVAILABLE = "backend_unavailable"
REASON_NO_PROGRAM = "no_program_loaded"
REASON_NO_ROBOT_STATE = "no_robot_state"
REASON_PLANNING_FAILED = "planning_failed"
REASON_CAPABILITY_UNSUPPORTED = "unsupported_capability"
REASON_STALE = "stale_planning_snapshot"
REASON_EXPIRED = "candidate_expired"
REASON_SUPERSEDED = "candidate_superseded"
REASON_REVERTED = "candidate_reverted"
REASON_NOT_READY = "candidate_not_ready"
REASON_REPLAN_REQUIRED = "replan_required"
REASON_EXECUTION_COMPLETED = "execution_completed"
REASON_EXECUTION_RUNNING = "execution_running"
REASON_EXECUTION_STOPPED = "execution_stopped"
REASON_EXECUTION_INTERRUPTED = "execution_interrupted"
REASON_EXECUTION_FAILED = "execution_failed"
REASON_JOINT_DYNAMICS_UNSUPPORTED = "joint_dynamics_limit_unsupported"
REASON_JOINT_DYNAMICS_INVALID = "joint_dynamics_limit_invalid"


def _reject_json_constant(token: str) -> float:
    """Reject JSON's non-standard NaN and infinity spellings."""
    raise ValueError(f"non-standard JSON constant not allowed: {token}")


class CandidateJointTarget(BaseModel):
    """Finite unit-tagged joint authoring value, not an addressed request."""

    model_config = _STRICT_CFG
    joint_names: list[str] = Field(default_factory=list)
    positions_rad: list[float] = Field(default_factory=list)

    @field_validator("joint_names")
    @classmethod
    def _validate_joint_names(cls, names: list[str]) -> list[str]:
        if not names or any(not name.strip() for name in names):
            raise ValueError("jointNames entries must be non-empty strings")
        if len(set(names)) != len(names):
            raise ValueError("jointNames must be unique")
        return names

    @field_validator("positions_rad")
    @classmethod
    def _validate_positions(cls, positions: list[float]) -> list[float]:
        if not positions or any(not math.isfinite(value) for value in positions):
            raise ValueError("positionsRad must be non-empty and finite")
        return positions

    @model_validator(mode="after")
    def _validate_lengths(self) -> "CandidateJointTarget":
        if len(self.joint_names) != len(self.positions_rad):
            raise ValueError("jointNames and positionsRad must have equal length")
        return self


class CandidatePose(BaseModel):
    model_config = _PRESENTATION_CFG
    position_m: list[float]
    orientation_xyzw: list[float]


class CandidateDiagnostics(BaseModel):
    model_config = _PRESENTATION_CFG
    reason_code: str
    summary: str = ""
    details: dict[str, Any] = Field(default_factory=dict)


class CandidateRobotState(BaseModel):
    model_config = _PRESENTATION_CFG
    joint_names: list[str] = Field(default_factory=list)
    positions_rad: list[float] = Field(default_factory=list)


class CandidateGhost(BaseModel):
    model_config = _PRESENTATION_CFG
    blocked: bool = False
    final_joint_names: Optional[list[str]] = None
    final_joint_positions_rad: Optional[list[float]] = None
    final_tcp_pose: Optional[CandidatePose] = None
    link_poses: Optional[dict[str, CandidatePose]] = None
    blocked_target_pose: Optional[CandidatePose] = None


class CandidateTrajectorySample(BaseModel):
    model_config = _PRESENTATION_CFG
    time_from_start_s: float
    positions_rad: list[float]


class CandidatePreviewTrajectory(BaseModel):
    model_config = _PRESENTATION_CFG
    joint_names: list[str] = Field(default_factory=list)
    samples: list[CandidateTrajectorySample] = Field(default_factory=list)
    sample_count: int = 0
    time_span_s: float = 0.0


def create_robot_control_candidates_router(_state: object) -> APIRouter:
    """Fail before reading state or creating a store, planner, or route."""
    raise RobotControlClientUnavailableError(
        f"{ROBOT_CONTROL_CLIENT_UNAVAILABLE_REASON_CODE}: Robot Control "
        "authoring reconnects at I7 and execution/Stop settlement at I8"
    )


__all__ = [
    "CandidateDiagnostics",
    "CandidateGhost",
    "CandidateJointTarget",
    "CandidatePose",
    "CandidatePreviewTrajectory",
    "CandidateRobotState",
    "CandidateTrajectorySample",
    "RobotControlClientUnavailableError",
    "create_robot_control_candidates_router",
]
