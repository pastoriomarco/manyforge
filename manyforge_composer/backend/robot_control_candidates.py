"""Validated Robot Control candidate routes - /api/robot-control/candidates/*

Phase 4-P5.1 (candidate preview planning + validation) and Phase 4-P5.3
(Approve through normal execution). See
`docs/implementation/kernel-architecture-sprint-sequencing.md`.

This surface is the server-owned candidate lifecycle for default Robot Control:
the operator authors a draft target, the server owns a candidate and plans +
validates it WITHOUT moving the robot, and the robot moves ONLY after an
explicit Approve of a fresh validated candidate.

Hard invariants (enforced and tested):
  - Creating, getting, reverting, and re-planning a candidate NEVER moves the
    robot and NEVER mutates `/api/scene/state` (candidate artifacts are
    overlays). Preview uses `KernelClient.preview_motion` (plan + validate, no
    dispatch).
  - The ONLY path that can move the robot is Approve/Execute, and it dispatches
    exclusively through the normal intervention path
    (`CycleManager.execute_intervention` -> `ExecutionKernel::execute`,
    DIRECT_CONTROL lifecycle) so runtime collision monitoring, interrupts, and
    stop/reset semantics match behavior-tree motion. No direct backend dispatch,
    no live streaming.
  - Approve fails closed: it rechecks candidate freshness against the current
    planning snapshot and re-validates against current robot state before
    dispatch; if freshness/validation cannot be proven it rejects with a
    structured stale / replan-required reason and the robot stays still.

Freshness model (Phase 1, Composer-route): the kernel's collision-world
revision is not plumbed to the Composer Python layer, so the planning snapshot
fingerprint is built from the stable signals that ARE available here
(authoritative robot joint positions, execution-state source, and loaded
program/deployment identity). Scene / collision-world changes are additionally
caught by the MANDATORY re-validation that Approve runs against the current
state before any dispatch. Promoting candidate ownership into the kernel with a
real collision-world revision is the documented Phase 2 follow-up.
"""
from __future__ import annotations

import json
import logging
import math
import threading
import time
import uuid
from dataclasses import dataclass
from enum import Enum
from typing import Any, Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.encoders import jsonable_encoder
from fastapi.responses import JSONResponse
from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecutionState,
    GripperCommandState,
    JointDynamicsLimit,
    MotionPreviewResult,
    RobotStateSnapshot,
)
from manyforge_behavior.program import (
    collect_tree_validation_issues,
    tree_node_from_dict,
)
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
    field_validator,
    model_validator,
)

from .gripper_command import (
    GRIPPER_REASON_BUSY,
    GRIPPER_REASON_LIMIT,
    GRIPPER_REASON_MALFORMED,
    gripper_busy_detail,
    gripper_cycle_busy_detail,
    validate_gripper_command,
)
from .intervention_validation import merged_intervention_blackboard_keys
from .state import ComposerState

_LOGGER = logging.getLogger(__name__)

# Output models (server-constructed) only need alias population.
_ALIAS_CFG = ConfigDict(populate_by_name=True)
# Client-facing input models additionally reject unknown/typo'd fields.
_STRICT_CFG = ConfigDict(populate_by_name=True, extra="forbid")

# A candidate that has not been approved expires after this age so a stale
# green indicator can never linger; the UI must re-author to refresh.
_CANDIDATE_MAX_AGE_NS = 30_000_000_000  # 30 s

# Positions are rounded to this many decimals before they enter the planning
# snapshot fingerprint so sensor jitter does not spuriously stale a candidate.
_POSITION_FINGERPRINT_DECIMALS = 5

# Scene-object poses are rounded to this many decimals before they enter the
# scene fingerprint so jitter from a continuous pose source does not spuriously
# stale a candidate while a genuine relocation / (de)attach still flips it.
_SCENE_POSE_FINGERPRINT_DECIMALS = 4


# --- Per-joint robot_control_joint_limits enforcement (Phase 4-P5.1K) ---
# `deployment.robot_config.robot_control_joint_limits` carries per-joint Robot
# Control velocity / acceleration / jerk caps. `_build_execute_request` forwards
# them on `ExecuteRequest.per_joint_dynamics_limits` for BOTH the preview plan
# and the approval-time recheck (identical caps for the candidate's planning
# snapshot). The native kernel resolves the effective per-joint cap as the
# conservative minimum across model/backend/controller and these deployment
# caps, enforces velocity/acceleration in RoboPlan TOPP-RA time-parameterization
# (a tight cap lengthens/smooths the trajectory), and fails closed before
# dispatch for unknown joints, malformed values, or caps the active path cannot
# enforce. A jerk cap is carried but the discrete TOPP-RA path cannot enforce
# jerk, so the native request validation rejects it as
# `unsupported_joint_dynamics_limit` rather than claiming enforcement. Joint-name
# validity is enforced at deployment load
# (`Deployment.validate_robot_control_joint_limits_against`).


# --- KERNEL-PROMOTION GAP: collision-world revision freshness -----------------
# The plan's P5.3 freshness model is anchored on the kernel's collision-world
# REVISION (a monotonic id the kernel bumps on any collision-world change,
# including pure shape/policy edits). That revision is NOT plumbed to the
# Composer Python layer. `_compute_scene_fingerprint` is a Composer-side STOPGAP
# that observes only what `get_scene_objects()` exposes (object id / attached /
# frame / rounded universe pose), so it catches add/remove/relocate/(de)attach
# but CANNOT see a pure collision shape/parameter/policy change. It is therefore
# NOT equivalent to revision-based freshness and we do NOT claim it as such. The
# safety backstop for what the fingerprint misses is the MANDATORY approve-time
# revalidation (preview_motion) in the execute route, which always re-runs
# against current state before any motion. Promoting candidate ownership into
# the kernel with a real collision-world revision is a deferred kernel-promotion
# follow-up; this aspect of P5.3 is NOT fully satisfied until that lands.


# --- Stable diagnostic reason codes ---------------------------------------
# Stable string codes the UI may switch on. Never classify by English text.
REASON_READY = "ready"
REASON_IK_UNREACHABLE = "ik_unreachable"
REASON_JOINT_LIMIT = "joint_limit_violation"
REASON_COLLISION = "collision_detected"
REASON_VALIDATION_TIMEOUT = "validation_timeout"
REASON_MALFORMED_TARGET = "malformed_target"
REASON_BACKEND_UNAVAILABLE = "backend_unavailable"
REASON_NO_PROGRAM = "no_program_loaded"
REASON_NO_ROBOT_STATE = "no_robot_state"
REASON_PLANNING_FAILED = "planning_failed"
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
# Phase 4-P5.1K: a per-joint Robot Control dynamics cap could not be enforced by
# the active planning path (e.g. a jerk cap on the discrete TOPP-RA path, or an
# unknown/non-active cap joint), or was malformed. Distinct from a generic
# planning failure so the UI can tell the operator the dynamics cap is the cause.
REASON_JOINT_DYNAMICS_UNSUPPORTED = "joint_dynamics_limit_unsupported"
REASON_JOINT_DYNAMICS_INVALID = "joint_dynamics_limit_invalid"

_KNOWN_REASON_CODES = frozenset(
    {
        REASON_IK_UNREACHABLE,
        REASON_JOINT_LIMIT,
        REASON_COLLISION,
        REASON_VALIDATION_TIMEOUT,
        REASON_MALFORMED_TARGET,
        REASON_BACKEND_UNAVAILABLE,
        REASON_PLANNING_FAILED,
        REASON_JOINT_DYNAMICS_UNSUPPORTED,
        REASON_JOINT_DYNAMICS_INVALID,
    }
)

# Map the backend's structured per-joint dynamics-limit failure reasons
# (spec/210 §3.3.1, emitted in ErrorDTO.details["reason"]) onto stable candidate
# reason codes, so preview/recheck diagnostics do not degrade to a generic
# `planning_failed`.
_BACKEND_DYNAMICS_REASON_TO_CANDIDATE = {
    "unsupported_joint_dynamics_limit": REASON_JOINT_DYNAMICS_UNSUPPORTED,
    "invalid_joint_dynamics_limit": REASON_JOINT_DYNAMICS_INVALID,
}


def _reject_json_constant(token: str) -> float:
    """Reject non-standard JSON constants (NaN, Infinity, -Infinity).

    Used as `json.loads(parse_constant=...)` so a non-finite payload fails at
    parse time with a clean 422, rather than reaching validation and crashing
    response encoding when FastAPI echoes the non-finite value (a 500).
    """
    raise ValueError(f"non-standard JSON constant not allowed: {token}")


class CandidateStatus(str, Enum):
    """Server-visible candidate lifecycle states.

    `idle` and `drafting` are client-local only (no server candidate yet) and
    are deliberately absent here. `approving` is API-visible so the UI can show
    the freshness recheck running distinctly from `executing`.
    """

    PLANNING = "planning"
    READY = "ready"
    BLOCKED = "blocked"
    STALE = "stale"
    SUPERSEDED = "superseded"
    EXPIRED = "expired"
    APPROVING = "approving"
    EXECUTING = "executing"
    COMPLETED = "completed"
    STOPPED = "stopped"
    INTERRUPTED = "interrupted"
    FAILED = "failed"
    REVERTED = "reverted"
    ERROR = "error"


class CandidateSourceSurface(str, Enum):
    JOINT_SLIDER = "joint_slider"
    JOINT_SPINNER = "joint_spinner"
    JOINT_INPUT = "joint_input"
    CARTESIAN_GIZMO = "cartesian_gizmo"
    CARTESIAN_INPUT = "cartesian_input"
    MOVE_TO = "move_to"
    GRIPPER_SLIDER = "gripper_slider"
    GRIPPER_PRESET = "gripper_preset"


class CandidateInputMode(str, Enum):
    JOINT = "joint"
    CARTESIAN = "cartesian"
    GRIPPER = "gripper"


# Statuses for a candidate that is still the live draft for its surface and may
# therefore be superseded by a newer draft.
_SUPERSEDABLE_STATUSES = frozenset(
    {CandidateStatus.PLANNING, CandidateStatus.READY, CandidateStatus.BLOCKED}
)
# Terminal statuses never transition further (and are never executable).
_TERMINAL_STATUSES = frozenset(
    {
        CandidateStatus.SUPERSEDED,
        CandidateStatus.EXPIRED,
        CandidateStatus.REVERTED,
        CandidateStatus.COMPLETED,
        CandidateStatus.STOPPED,
        CandidateStatus.INTERRUPTED,
        CandidateStatus.FAILED,
        CandidateStatus.ERROR,
    }
)


# --- Input models ----------------------------------------------------------


class CandidateJointTarget(BaseModel):
    model_config = _STRICT_CFG
    joint_names: list[str] = Field(default_factory=list, alias="jointNames")
    positions_rad: list[float] = Field(default_factory=list, alias="positionsRad")

    @field_validator("joint_names")
    @classmethod
    def _validate_joint_names(cls, names: list[str]) -> list[str]:
        if not names:
            raise ValueError("jointNames must be non-empty")
        if any(not isinstance(n, str) or n.strip() == "" for n in names):
            raise ValueError("jointNames entries must be non-empty strings")
        if len(set(names)) != len(names):
            raise ValueError("jointNames must be unique")
        return names

    @field_validator("positions_rad")
    @classmethod
    def _validate_positions(cls, positions: list[float]) -> list[float]:
        if not positions:
            raise ValueError("positionsRad must be non-empty")
        if any(not math.isfinite(p) for p in positions):
            raise ValueError("positionsRad must all be finite")
        return positions

    @model_validator(mode="after")
    def _validate_lengths(self) -> "CandidateJointTarget":
        if len(self.joint_names) != len(self.positions_rad):
            raise ValueError("jointNames and positionsRad must have equal length")
        return self


class CandidateCartesianTarget(BaseModel):
    model_config = _STRICT_CFG
    position_m: list[float] = Field(
        default_factory=lambda: [0.0, 0.0, 0.0], alias="positionM"
    )
    orientation_xyzw: list[float] = Field(
        default_factory=lambda: [0.0, 0.0, 0.0, 1.0], alias="orientationXyzw"
    )
    frame_id: str = Field(default="base", alias="frameId")
    tcp_frame: Optional[str] = Field(default=None, alias="tcpFrame")

    @field_validator("position_m")
    @classmethod
    def _validate_position(cls, position: list[float]) -> list[float]:
        if len(position) != 3:
            raise ValueError("positionM must have exactly 3 elements")
        if any(not math.isfinite(p) for p in position):
            raise ValueError("positionM elements must all be finite")
        return position

    @field_validator("orientation_xyzw")
    @classmethod
    def _validate_orientation(cls, quat: list[float]) -> list[float]:
        if len(quat) != 4:
            raise ValueError("orientationXyzw must have exactly 4 elements")
        if any(not math.isfinite(q) for q in quat):
            raise ValueError("orientationXyzw elements must all be finite")
        if sum(q * q for q in quat) == 0.0:
            raise ValueError("orientationXyzw must have non-zero norm")
        return quat

    @field_validator("frame_id")
    @classmethod
    def _validate_frame_id(cls, frame_id: str) -> str:
        if frame_id.strip() == "":
            raise ValueError("frameId must be non-empty")
        return frame_id

    @field_validator("tcp_frame")
    @classmethod
    def _validate_tcp_frame(cls, tcp_frame: Optional[str]) -> Optional[str]:
        if tcp_frame is not None and tcp_frame.strip() == "":
            raise ValueError("tcpFrame must be non-empty when provided")
        return tcp_frame


class CandidateGripperTarget(BaseModel):
    model_config = _STRICT_CFG
    gripper_id: str = Field(alias="gripperId")
    target_position: float = Field(alias="targetPosition")
    max_effort: Optional[float] = Field(default=None, alias="maxEffort")
    duration_s: Optional[float] = Field(default=None, alias="durationS")

    @field_validator("gripper_id")
    @classmethod
    def _validate_gripper_id(cls, gripper_id: str) -> str:
        if gripper_id.strip() == "":
            raise ValueError("gripperId must be non-empty")
        return gripper_id

    @field_validator("target_position")
    @classmethod
    def _validate_target(cls, target_position: float) -> float:
        if not math.isfinite(target_position):
            raise ValueError("targetPosition must be finite")
        return target_position

    @field_validator("max_effort")
    @classmethod
    def _validate_max_effort(cls, max_effort: Optional[float]) -> Optional[float]:
        if max_effort is not None and (not math.isfinite(max_effort) or max_effort < 0.0):
            raise ValueError("maxEffort must be finite and >= 0 when provided")
        return max_effort

    @field_validator("duration_s")
    @classmethod
    def _validate_duration(cls, duration_s: Optional[float]) -> Optional[float]:
        if duration_s is not None and (not math.isfinite(duration_s) or duration_s < 0.0):
            raise ValueError("durationS must be finite and >= 0 when provided")
        return duration_s


class CandidateCreateRequest(BaseModel):
    model_config = _STRICT_CFG
    client_request_id: Optional[str] = Field(default=None, alias="clientRequestId")
    source_surface: CandidateSourceSurface = Field(alias="sourceSurface")
    input_mode: CandidateInputMode = Field(alias="inputMode")
    joint_target: Optional[CandidateJointTarget] = Field(
        default=None, alias="jointTarget"
    )
    cartesian_target: Optional[CandidateCartesianTarget] = Field(
        default=None, alias="cartesianTarget"
    )
    gripper_target: Optional[CandidateGripperTarget] = Field(
        default=None, alias="gripperTarget"
    )

    @model_validator(mode="after")
    def _validate_mode_target(self) -> "CandidateCreateRequest":
        # sourceSurface must be consistent with inputMode. Joint/cartesian
        # surfaces are mode-locked; move_to may author either mode.
        joint_surfaces = {
            CandidateSourceSurface.JOINT_SLIDER,
            CandidateSourceSurface.JOINT_SPINNER,
            CandidateSourceSurface.JOINT_INPUT,
        }
        cartesian_surfaces = {
            CandidateSourceSurface.CARTESIAN_GIZMO,
            CandidateSourceSurface.CARTESIAN_INPUT,
        }
        gripper_surfaces = {
            CandidateSourceSurface.GRIPPER_SLIDER,
            CandidateSourceSurface.GRIPPER_PRESET,
        }
        if (
            self.source_surface in joint_surfaces
            and self.input_mode is not CandidateInputMode.JOINT
        ):
            raise ValueError(
                f"sourceSurface {self.source_surface.value} requires inputMode=joint"
            )
        if (
            self.source_surface in cartesian_surfaces
            and self.input_mode is not CandidateInputMode.CARTESIAN
        ):
            raise ValueError(
                f"sourceSurface {self.source_surface.value} requires inputMode=cartesian"
            )
        if (
            self.source_surface in gripper_surfaces
            and self.input_mode is not CandidateInputMode.GRIPPER
        ):
            raise ValueError(
                f"sourceSurface {self.source_surface.value} requires inputMode=gripper"
            )

        if self.input_mode is CandidateInputMode.JOINT:
            if self.joint_target is None:
                raise ValueError("jointTarget is required when inputMode=joint")
            if self.cartesian_target is not None:
                raise ValueError("cartesianTarget must be absent when inputMode=joint")
            if self.gripper_target is not None:
                raise ValueError("gripperTarget must be absent when inputMode=joint")
        elif self.input_mode is CandidateInputMode.CARTESIAN:
            if self.cartesian_target is None:
                raise ValueError("cartesianTarget is required when inputMode=cartesian")
            if self.joint_target is not None:
                raise ValueError("jointTarget must be absent when inputMode=cartesian")
            if self.gripper_target is not None:
                raise ValueError("gripperTarget must be absent when inputMode=cartesian")
        elif self.input_mode is CandidateInputMode.GRIPPER:
            # Inverse of the surface->mode rule above: a gripper candidate must
            # come from a gripper surface (not move_to or any arm surface), so
            # the command source shown in the UI always matches the input mode.
            if self.source_surface not in gripper_surfaces:
                raise ValueError(
                    "inputMode=gripper requires a gripper sourceSurface "
                    "(gripper_slider or gripper_preset)"
                )
            if self.gripper_target is None:
                raise ValueError("gripperTarget is required when inputMode=gripper")
            if self.joint_target is not None:
                raise ValueError("jointTarget must be absent when inputMode=gripper")
            if self.cartesian_target is not None:
                raise ValueError("cartesianTarget must be absent when inputMode=gripper")
        return self


# --- Output / artifact models ---------------------------------------------


class CandidatePose(BaseModel):
    model_config = _ALIAS_CFG
    position_m: list[float] = Field(alias="positionM")
    orientation_xyzw: list[float] = Field(alias="orientationXyzw")


class CandidateDiagnostics(BaseModel):
    model_config = _ALIAS_CFG
    reason_code: str = Field(alias="reasonCode")
    summary: str = Field(default="")
    details: dict[str, Any] = Field(default_factory=dict)


class CandidateRobotState(BaseModel):
    model_config = _ALIAS_CFG
    joint_names: list[str] = Field(default_factory=list, alias="jointNames")
    positions_rad: list[float] = Field(default_factory=list, alias="positionsRad")


class CandidateGhost(BaseModel):
    """Visual preview artifact. Never mutates `/api/scene/state`."""

    model_config = _ALIAS_CFG
    # True when the candidate is invalid; the UI renders a blocked/red ghost.
    blocked: bool = Field(default=False)
    final_joint_names: Optional[list[str]] = Field(
        default=None, alias="finalJointNames"
    )
    final_joint_positions_rad: Optional[list[float]] = Field(
        default=None, alias="finalJointPositionsRad"
    )
    final_tcp_pose: Optional[CandidatePose] = Field(
        default=None, alias="finalTcpPose"
    )
    # Per-link poses at the candidate's final configuration when the backend
    # exposes FK at a hypothetical configuration; omitted otherwise (the UI can
    # still render the ghost from final joint positions via its own FK).
    link_poses: Optional[dict[str, CandidatePose]] = Field(
        default=None, alias="linkPoses"
    )
    # The requested target pose for a blocked Cartesian candidate so the UI can
    # render the red TCP ghost at the unreachable/colliding pose.
    blocked_target_pose: Optional[CandidatePose] = Field(
        default=None, alias="blockedTargetPose"
    )


class CandidateTrajectorySample(BaseModel):
    model_config = _ALIAS_CFG
    time_from_start_s: float = Field(alias="timeFromStartS")
    positions_rad: list[float] = Field(alias="positionsRad")


class CandidatePreviewTrajectory(BaseModel):
    """Optional sampled intermediate-motion preview for a valid candidate."""

    model_config = _ALIAS_CFG
    joint_names: list[str] = Field(default_factory=list, alias="jointNames")
    samples: list[CandidateTrajectorySample] = Field(default_factory=list)
    sample_count: int = Field(default=0, alias="sampleCount")
    time_span_s: float = Field(default=0.0, alias="timeSpanS")


class CandidateSourceRevisions(BaseModel):
    """Coarse, UI-safe freshness descriptors recorded at plan time. The
    fine-grained server fingerprint stays internal (see `_PlanningSnapshot`).

    `sceneObjectCount` and `sceneFingerprint` expose the
    scene/attached-object signal the planning snapshot uses, so the UI can show
    that a candidate's scene context changed. The fingerprint is an opaque
    string — the UI must treat it as compare-only, never parse it."""

    model_config = _ALIAS_CFG
    state_source: str = Field(default="", alias="stateSource")
    robot_state_known: bool = Field(default=False, alias="robotStateKnown")
    program_loaded: bool = Field(default=False, alias="programLoaded")
    deployment_loaded: bool = Field(default=False, alias="deploymentLoaded")
    scene_object_count: int = Field(default=0, alias="sceneObjectCount")
    scene_fingerprint: str = Field(default="", alias="sceneFingerprint")


class CandidateExecution(BaseModel):
    """Runtime lifecycle observed after Approve dispatches a candidate."""

    model_config = _ALIAS_CFG
    move_id: Optional[str] = Field(default=None, alias="moveId")
    execution_id: int = Field(default=0, alias="executionId")
    state: Optional[str] = None
    progress: float = 0.0
    session_id: Optional[str] = Field(default=None, alias="sessionId")
    session_status: Optional[str] = Field(default=None, alias="sessionStatus")
    session_actual_outcome: Optional[str] = Field(
        default=None, alias="sessionActualOutcome"
    )
    session_error: Optional[str] = Field(default=None, alias="sessionError")
    error: Optional[str] = None
    error_code: Optional[str] = Field(default=None, alias="errorCode")
    error_details: Optional[dict[str, Any]] = Field(
        default=None, alias="errorDetails"
    )


class CandidateResponse(BaseModel):
    model_config = _ALIAS_CFG
    candidate_id: str = Field(alias="candidateId")
    generation: int = Field(alias="generation")
    source_surface: CandidateSourceSurface = Field(alias="sourceSurface")
    input_mode: CandidateInputMode = Field(alias="inputMode")
    status: CandidateStatus = Field()
    commit_eligible: bool = Field(default=False, alias="commitEligible")
    joint_target: Optional[CandidateJointTarget] = Field(
        default=None, alias="jointTarget"
    )
    cartesian_target: Optional[CandidateCartesianTarget] = Field(
        default=None, alias="cartesianTarget"
    )
    gripper_target: Optional[CandidateGripperTarget] = Field(
        default=None, alias="gripperTarget"
    )
    # Full-vector joint goal used for planning/dispatch (partial joint targets
    # are overlaid onto the current full joint vector).
    normalized_joint_target: Optional[CandidateJointTarget] = Field(
        default=None, alias="normalizedJointTarget"
    )
    client_request_id: Optional[str] = Field(default=None, alias="clientRequestId")
    diagnostics: Optional[CandidateDiagnostics] = Field(default=None)
    start_state: Optional[CandidateRobotState] = Field(
        default=None, alias="startState"
    )
    source_revisions: Optional[CandidateSourceRevisions] = Field(
        default=None, alias="sourceRevisions"
    )
    ghost: Optional[CandidateGhost] = Field(default=None)
    preview_trajectory: Optional[CandidatePreviewTrajectory] = Field(
        default=None, alias="previewTrajectory"
    )
    execution: Optional[CandidateExecution] = Field(default=None)
    trajectory_duration_s: Optional[float] = Field(
        default=None, alias="trajectoryDurationS"
    )
    max_age_ns: int = Field(default=_CANDIDATE_MAX_AGE_NS, alias="maxAgeNs")
    expires_at_ns: int = Field(default=0, alias="expiresAtNs")
    created_at_ns: int = Field(alias="createdAtNs")
    updated_at_ns: int = Field(alias="updatedAtNs")


# --- Server-side planning snapshot (freshness fingerprint) -----------------


@dataclass(frozen=True)
class _PlanningSnapshot:
    """Stable fingerprint of the inputs a candidate was planned against.

    Only signals that are observable at the Composer layer AND stable across
    repeated polls are included. Notably the execution-snapshot
    `sequence_number`/`source_timestamp_ns` are EXCLUDED because they change on
    every poll; meaningful robot motion is detected through the rounded joint
    positions instead.

    `scene_object_count` + `scene_fingerprint` additionally capture
    the SCENE / attached-object state so a candidate goes STALE when objects are
    added/removed/relocated or (de)attached WITHOUT a new user edit. These are
    derived defensively from `state.kernel.get_scene_objects()` (object_id +
    attached + rounded pose). The kernel's collision-world REVISION is not
    plumbed to Python, so a pure shape/parameter change the snapshot cannot see
    is still caught by the MANDATORY approve-time revalidation (documented
    kernel-promotion follow-up).
    """

    state_source: str
    robot_positions_key: tuple
    robot_state_known: bool
    program_token: int
    deployment_token: int
    scene_object_count: int
    scene_fingerprint: str


def _compute_scene_fingerprint(state: ComposerState) -> tuple[int, str]:
    """Derive (object_count, opaque_fingerprint) from the backend scene.

    Captured defensively: any failure collapses to (0, "") so freshness
    computation never raises. The fingerprint folds each object's id, attached
    flag, frame, and rounded universe pose so a relocation or (de)attach flips
    it while continuous-pose-source jitter (rounded away) does not.
    """
    kernel = state.kernel
    if kernel is None:
        return 0, ""
    get_scene_objects = getattr(kernel, "get_scene_objects", None)
    if not callable(get_scene_objects):
        return 0, ""
    try:
        scene_objects = get_scene_objects()
    except Exception:  # noqa: BLE001 — freshness must never raise
        return 0, ""
    if not scene_objects:
        return 0, ""
    descriptors: list[tuple] = []
    for obj in scene_objects:
        try:
            object_id = str(getattr(obj, "object_id", "") or "")
            attached = bool(getattr(obj, "attached", False))
            frame_id = getattr(obj, "frame_id", None)
            frame_key = "" if frame_id is None else str(frame_id)
            pose = getattr(obj, "pose_in_universe", None)
            pose_key: tuple = ()
            if isinstance(pose, dict):
                position = pose.get("position_m")
                orientation = pose.get("orientation_xyzw")
                values: list[float] = []
                for raw_seq in (position, orientation):
                    if isinstance(raw_seq, (list, tuple)):
                        for v in raw_seq:
                            if isinstance(v, (int, float)) and math.isfinite(v):
                                values.append(
                                    round(float(v), _SCENE_POSE_FINGERPRINT_DECIMALS)
                                )
                pose_key = tuple(values)
            descriptors.append((object_id, attached, frame_key, pose_key))
        except Exception:  # noqa: BLE001 — per-object best-effort
            continue
    descriptors.sort(key=lambda d: d[0])
    try:
        fingerprint = json.dumps(descriptors, default=str, sort_keys=True)
    except Exception:  # noqa: BLE001
        fingerprint = ""
    return len(descriptors), fingerprint


def _compute_planning_snapshot(state: ComposerState) -> _PlanningSnapshot:
    kernel = state.kernel
    state_source = ""
    if kernel is not None:
        try:
            snapshot = kernel.get_execution_snapshot()
            state_source = str(getattr(snapshot, "state_source", "") or "")
        except Exception:  # noqa: BLE001 — freshness must never raise
            state_source = ""
    positions: Optional[list[float]] = None
    try:
        positions = state.get_current_joint_positions()
    except Exception:  # noqa: BLE001
        positions = None
    if positions is None:
        positions_key: tuple = ()
        robot_state_known = False
    else:
        positions_key = tuple(
            round(float(p), _POSITION_FINGERPRINT_DECIMALS) for p in positions
        )
        robot_state_known = True
    scene_object_count, scene_fingerprint = _compute_scene_fingerprint(state)
    return _PlanningSnapshot(
        state_source=state_source,
        robot_positions_key=positions_key,
        robot_state_known=robot_state_known,
        program_token=id(state.program) if state.program is not None else 0,
        deployment_token=id(state.deployment) if state.deployment is not None else 0,
        scene_object_count=scene_object_count,
        scene_fingerprint=scene_fingerprint,
    )


# Sentinel for a reserved-but-not-yet-planned candidate. Never compared for
# staleness (a PLANNING placeholder has no planned-against snapshot yet).
_EMPTY_PLANNING_SNAPSHOT = _PlanningSnapshot(
    state_source="",
    robot_positions_key=(),
    robot_state_known=False,
    program_token=0,
    deployment_token=0,
    scene_object_count=0,
    scene_fingerprint="",
)


# --- Internal stored candidate ---------------------------------------------


@dataclass
class _StoredCandidate:
    response: CandidateResponse
    snapshot: _PlanningSnapshot
    # move_manipulator_action params used to (a) re-validate at Approve and
    # (b) build the intervention tree for dispatch. None when the candidate is
    # not executable (error/blocked).
    dispatch_params: Optional[dict]


# --- Diagnostic classification ---------------------------------------------


def _classify_preview_failure(preview: MotionPreviewResult) -> CandidateDiagnostics:
    """Map a failed `MotionPreviewResult` onto a stable candidate reason code.

    Prefers an explicit structured `reason_code`/`reason`/`failure_kind` carried
    in `error_details`; otherwise classifies defensively by keyword. Never
    raises.
    """
    details: dict[str, Any] = {}
    if isinstance(preview.error_details, dict):
        details = dict(preview.error_details)
    summary = preview.error or "Candidate planning failed"

    explicit = None
    for key in ("reason_code", "reason", "failure_kind"):
        value = details.get(key)
        if isinstance(value, str) and value.strip():
            explicit = value.strip()
            break
    # Backend per-joint dynamics-limit reasons (Phase 4-P5.1K) use distinct
    # strings from the candidate reason codes; translate them explicitly so they
    # do not fall through to the keyword classifier (which would otherwise
    # degrade them to a generic planning_failed).
    mapped_dynamics = _BACKEND_DYNAMICS_REASON_TO_CANDIDATE.get(explicit or "")
    if mapped_dynamics is not None:
        return CandidateDiagnostics(
            reason_code=mapped_dynamics, summary=summary, details=details
        )
    if explicit in _KNOWN_REASON_CODES:
        return CandidateDiagnostics(
            reason_code=explicit, summary=summary, details=details
        )

    haystack = " ".join(
        [
            (preview.error or ""),
            json.dumps(details, default=str, sort_keys=True),
            explicit or "",
        ]
    ).lower()

    def has(*needles: str) -> bool:
        return any(n in haystack for n in needles)

    if has("collision", "in_collision", "colliding"):
        reason = REASON_COLLISION
    elif has("joint limit", "joint_limit", "out of range", "exceeds limit", "outside limit"):
        reason = REASON_JOINT_LIMIT
    elif has("ik", "unreachable", "no solution", "no ik", "out of reach", "not reachable"):
        reason = REASON_IK_UNREACHABLE
    elif has("timeout", "timed out", "deadline"):
        reason = REASON_VALIDATION_TIMEOUT
    elif has("malformed", "invalid target", "invalid request", "bad target"):
        reason = REASON_MALFORMED_TARGET
    elif has("backend", "unavailable", "no controller", "not connected"):
        reason = REASON_BACKEND_UNAVAILABLE
    else:
        reason = REASON_PLANNING_FAILED
    return CandidateDiagnostics(reason_code=reason, summary=summary, details=details)


# --- Candidate planner (no motion) -----------------------------------------


def _to_pose(raw: Optional[dict]) -> Optional[CandidatePose]:
    if not isinstance(raw, dict):
        return None
    pos = raw.get("position_m")
    orient = raw.get("orientation_xyzw")
    if (
        isinstance(pos, (list, tuple))
        and isinstance(orient, (list, tuple))
        and len(pos) == 3
        and len(orient) == 4
        and all(isinstance(v, (int, float)) and math.isfinite(v) for v in pos)
        and all(isinstance(v, (int, float)) and math.isfinite(v) for v in orient)
    ):
        return CandidatePose(
            position_m=[float(v) for v in pos],
            orientation_xyzw=[float(v) for v in orient],
        )
    return None


class _CandidatePlanner:
    """Plans + validates one candidate synchronously, without moving the robot.

    The planner only ever calls read-only / preview kernel surfaces
    (`preview_motion`, and opportunistically `plan_motion` /
    `compute_target_link_poses` when the client exposes them). It never calls
    `execute`, `scene_edit`, or any dispatch path.
    """

    def __init__(self, state: ComposerState) -> None:
        self._state = state

    # -- helpers ----------------------------------------------------------

    def _global_tcp_speed_cap(self) -> Optional[float]:
        deployment = self._state.deployment
        if deployment is None or getattr(deployment, "robot_config", None) is None:
            return None
        cap = getattr(deployment.robot_config, "global_max_tcp_linear_speed_mps", None)
        if isinstance(cap, (int, float)) and math.isfinite(cap) and cap > 0:
            return float(cap)
        return None

    def _per_joint_dynamics_limits(self) -> dict[str, JointDynamicsLimit]:
        """Resolve the deployment's per-joint Robot Control dynamics caps into
        the request channel (Phase 4-P5.1K). The deployment is one source only;
        the kernel combines these with model/backend/controller limits by
        conservative minimum and enforces velocity/acceleration in TOPP-RA.
        Joint-name validity is enforced at deployment load
        (`validate_robot_control_joint_limits_against`); a jerk cap is carried
        and rejected by the native path as `unsupported_joint_dynamics_limit`
        where the discrete planner cannot enforce it."""
        deployment = self._state.deployment
        if deployment is None or getattr(deployment, "robot_config", None) is None:
            return {}
        raw = getattr(deployment.robot_config, "robot_control_joint_limits", None)
        if not isinstance(raw, dict):
            return {}
        out: dict[str, JointDynamicsLimit] = {}
        for joint_name, limit in raw.items():
            out[str(joint_name)] = JointDynamicsLimit(
                max_velocity_rad_s=getattr(limit, "max_velocity_rad_s", None),
                max_acceleration_rad_s2=getattr(limit, "max_acceleration_rad_s2", None),
                max_jerk_rad_s3=getattr(limit, "max_jerk_rad_s3", None),
            )
        return out

    def _build_execute_request(
        self,
        *,
        move_id: str,
        motion_type: str,
        joint_names: Optional[list[str]] = None,
        joint_positions: Optional[list[float]] = None,
        pose_goal: Optional[dict] = None,
        base_frame: Optional[str] = None,
        tcp_frame: Optional[str] = None,
    ) -> ExecuteRequest:
        robot = self._state.program.robot  # type: ignore[union-attr]
        # Honor the candidate's requested cartesian frameId / tcpFrame when set;
        # fall back to the program robot's frames otherwise. The same resolved
        # frames are used for preview planning AND the dispatch tree params at
        # approve, so execute moves against exactly the frames that were
        # validated.
        resolved_base = base_frame if base_frame else robot.base_frame
        resolved_tcp = tcp_frame if tcp_frame else robot.tcp_frame
        # Forward both deployment dynamics channels: the Cartesian TCP speed cap
        # and the per-joint Robot Control dynamics caps (Phase 4-P5.1K). The
        # SAME caps are built for preview planning and the approval-time recheck
        # (both call this helper), so the candidate is validated and executed
        # against identical limits.
        return ExecuteRequest(
            robot_id=robot.robot_id,
            group_id=robot.group_id,
            move_id=move_id,
            base_frame=resolved_base,
            tcp_frame=resolved_tcp,
            motion_type=motion_type,
            joint_goal_names=list(joint_names or []),
            joint_goal_positions_rad=list(joint_positions or []),
            pose_goal=pose_goal,
            max_tcp_linear_speed_mps=self._global_tcp_speed_cap(),
            per_joint_dynamics_limits=self._per_joint_dynamics_limits(),
        )

    def _maybe_link_poses(
        self,
        *,
        kind: str,
        joint_names: Optional[list[str]],
        joint_positions: Optional[list[float]],
        pose_goal: Optional[dict],
        base_frame: Optional[str] = None,
        tcp_frame: Optional[str] = None,
    ) -> Optional[dict[str, CandidatePose]]:
        """Best-effort FK at the candidate configuration for a ghost overlay.

        Uses the native `compute_target_link_poses` surface when present. Never
        moves the robot or mutates the scene. Returns None when unavailable.

        The ghost FK is computed against the SAME frames the candidate was
        planned with (the resolved base/tcp frames, which honor a Cartesian
        candidate's requested frameId/tcpFrame); it falls back to the program
        robot's frames when a frame is not supplied, so the rendered ghost
        matches the validated motion instead of always assuming program frames.
        """
        kernel = self._state.kernel
        compute = getattr(kernel, "compute_target_link_poses", None)
        if not callable(compute):
            return None
        robot = self._state.program.robot  # type: ignore[union-attr]
        request: dict[str, Any] = {
            "kind": kind,
            "base_frame": base_frame if base_frame else robot.base_frame,
            "link_names": [],
        }
        if kind == "joint":
            request["joint_names"] = list(joint_names or [])
            request["joint_positions_rad"] = list(joint_positions or [])
        else:
            request["tcp_frame"] = tcp_frame if tcp_frame else robot.tcp_frame
            if pose_goal is not None:
                request["tcp_position_m"] = list(pose_goal.get("position_m", []))
                request["tcp_orientation_xyzw"] = list(
                    pose_goal.get("orientation_xyzw", [])
                )
        try:
            result = compute(request)
        except Exception:  # noqa: BLE001 — preview FK is best-effort only
            return None
        if not isinstance(result, dict):
            return None
        raw_poses = result.get("link_poses")
        if not isinstance(raw_poses, dict):
            return None
        poses: dict[str, CandidatePose] = {}
        for link_name, raw in raw_poses.items():
            pose = _to_pose(raw)
            if pose is not None:
                poses[str(link_name)] = pose
        return poses or None

    def _maybe_preview_trajectory(
        self,
        request: ExecuteRequest,
        start_state: RobotStateSnapshot,
        joint_names: list[str],
    ) -> Optional[CandidatePreviewTrajectory]:
        """Sampled intermediate-motion preview, when the client exposes the
        native `plan_motion` trajectory surface. Best-effort and no-motion."""
        kernel = self._state.kernel
        plan = getattr(kernel, "plan_motion", None)
        if not callable(plan):
            return None
        try:
            _preview, trajectory = plan(request, start_state)
        except Exception:  # noqa: BLE001
            return None
        if trajectory is None or not getattr(trajectory, "waypoints", None):
            return None
        traj_joint_names = list(getattr(trajectory, "joint_names", []) or joint_names)
        samples = [
            CandidateTrajectorySample(
                time_from_start_s=float(wp.time_from_start_s),
                positions_rad=[float(p) for p in wp.positions_rad],
            )
            for wp in trajectory.waypoints
        ]
        time_span = samples[-1].time_from_start_s if samples else 0.0
        return CandidatePreviewTrajectory(
            joint_names=traj_joint_names,
            samples=samples,
            sample_count=len(samples),
            time_span_s=time_span,
        )

    def _start_state(self) -> Optional[RobotStateSnapshot]:
        kernel = self._state.kernel
        if kernel is None:
            return None
        joint_names = list(kernel.get_joint_names())
        if not joint_names:
            return None
        positions = self._state.get_current_joint_positions()
        if positions is None or len(positions) != len(joint_names):
            return None
        return RobotStateSnapshot(
            joint_names=joint_names,
            positions_rad=[float(p) for p in positions],
            velocities_rad_s=[],
        )

    # -- main entry -------------------------------------------------------

    def plan(self, req: CandidateCreateRequest) -> tuple[CandidateResponse, _StoredCandidate]:
        now = time.monotonic_ns()
        snapshot = _compute_planning_snapshot(self._state)
        revisions = CandidateSourceRevisions(
            state_source=snapshot.state_source,
            robot_state_known=snapshot.robot_state_known,
            program_loaded=self._state.program is not None,
            deployment_loaded=self._state.deployment is not None,
            scene_object_count=snapshot.scene_object_count,
            scene_fingerprint=snapshot.scene_fingerprint,
        )

        # Build a baseline response; the store assigns id/generation/timestamps.
        def _make(
            status: CandidateStatus,
            *,
            commit_eligible: bool = False,
            diagnostics: Optional[CandidateDiagnostics] = None,
            normalized: Optional[CandidateJointTarget] = None,
            start_state: Optional[CandidateRobotState] = None,
            ghost: Optional[CandidateGhost] = None,
            preview: Optional[CandidatePreviewTrajectory] = None,
            duration_s: Optional[float] = None,
        ) -> CandidateResponse:
            return CandidateResponse(
                candidate_id="",  # store fills
                generation=0,  # store fills
                source_surface=req.source_surface,
                input_mode=req.input_mode,
                status=status,
                commit_eligible=commit_eligible,
                joint_target=req.joint_target,
                cartesian_target=req.cartesian_target,
                gripper_target=req.gripper_target,
                normalized_joint_target=normalized,
                client_request_id=req.client_request_id,
                diagnostics=diagnostics,
                start_state=start_state,
                source_revisions=revisions,
                ghost=ghost,
                preview_trajectory=preview,
                trajectory_duration_s=duration_s,
                created_at_ns=now,
                updated_at_ns=now,
            )

        # Gripper candidates validate against the deployment gripper config and
        # the kernel (via the shared gripper-command validator); they need no arm
        # program/robot profile or joint start state, so branch before those
        # checks.
        if req.input_mode is CandidateInputMode.GRIPPER:
            return self._plan_gripper(req, snapshot, _make)

        # Fail closed when prerequisites for no-motion planning are missing.
        if self._state.program is None or self._state.program.robot is None:
            response = _make(
                CandidateStatus.ERROR,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_NO_PROGRAM,
                    summary="No program/robot profile loaded; cannot plan a candidate.",
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)
        if self._state.kernel is None:
            response = _make(
                CandidateStatus.ERROR,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_BACKEND_UNAVAILABLE,
                    summary="No kernel available; cannot plan a candidate.",
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)

        start = self._start_state()
        if start is None:
            response = _make(
                CandidateStatus.ERROR,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_NO_ROBOT_STATE,
                    summary=(
                        "No current robot joint state available; load a deployment "
                        "so the initial configuration is known."
                    ),
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)
        start_state_model = CandidateRobotState(
            joint_names=list(start.joint_names),
            positions_rad=list(start.positions_rad),
        )

        if req.input_mode is CandidateInputMode.JOINT:
            return self._plan_joint(req, start, start_state_model, snapshot, _make)
        return self._plan_cartesian(req, start, start_state_model, snapshot, _make)

    def _plan_joint(self, req, start, start_state_model, snapshot, _make):
        assert req.joint_target is not None
        kernel_joint_names = list(start.joint_names)
        # Overlay the (possibly partial) requested joints onto the full current
        # vector so the planner does not zero untouched joints.
        target_positions = list(start.positions_rad)
        for name, pos in zip(
            req.joint_target.joint_names, req.joint_target.positions_rad
        ):
            try:
                idx = kernel_joint_names.index(name)
            except ValueError:
                response = _make(
                    CandidateStatus.BLOCKED,
                    start_state=start_state_model,
                    diagnostics=CandidateDiagnostics(
                        reason_code=REASON_MALFORMED_TARGET,
                        summary=f"Unknown joint '{name}' for the active robot group.",
                        details={"unknown_joint": name},
                    ),
                )
                return response, _StoredCandidate(response, snapshot, None)
            target_positions[idx] = float(pos)

        normalized = CandidateJointTarget(
            joint_names=kernel_joint_names, positions_rad=target_positions
        )
        move_id = f"rc_candidate_joint_{uuid.uuid4().hex}"
        exec_request = self._build_execute_request(
            move_id=move_id,
            motion_type="joint_goal",
            joint_names=kernel_joint_names,
            joint_positions=target_positions,
        )
        dispatch_params = {
            "motion_type": "joint_goal",
            "joint_goal_names": kernel_joint_names,
            "joint_goal_positions_rad": target_positions,
        }
        return self._run_preview(
            req,
            exec_request,
            start,
            start_state_model,
            snapshot,
            _make,
            normalized=normalized,
            dispatch_params=dispatch_params,
            ghost_kind="joint",
            ghost_joint_names=kernel_joint_names,
            ghost_joint_positions=target_positions,
            blocked_target_pose=None,
        )

    def _plan_cartesian(self, req, start, start_state_model, snapshot, _make):
        assert req.cartesian_target is not None
        # Honor the candidate's frameId / tcpFrame. `frame_id`
        # defaults to the sentinel "base" at the model layer and `tcp_frame` is
        # optional; both sentinels mean "use the program robot's frame". A
        # non-default frameId / an explicit tcpFrame is forwarded verbatim into
        # BOTH the preview ExecuteRequest and the dispatch tree params used at
        # approve, so execute validates and moves against the same frames that
        # were planned. This keeps behavior identical when frameId="base" and
        # tcpFrame is unset (the prior always-program-frames behavior).
        robot = self._state.program.robot  # type: ignore[union-attr]
        requested_frame_id = req.cartesian_target.frame_id
        candidate_base_frame = (
            robot.base_frame
            if not requested_frame_id or requested_frame_id == "base"
            else requested_frame_id
        )
        candidate_tcp_frame = req.cartesian_target.tcp_frame or robot.tcp_frame
        pose_goal = {
            "frame_id": candidate_base_frame,
            "position_m": [float(v) for v in req.cartesian_target.position_m],
            "orientation_xyzw": [
                float(v) for v in req.cartesian_target.orientation_xyzw
            ],
        }
        move_id = f"rc_candidate_pose_{uuid.uuid4().hex}"
        exec_request = self._build_execute_request(
            move_id=move_id,
            motion_type="pose_goal",
            pose_goal=pose_goal,
            base_frame=candidate_base_frame,
            tcp_frame=candidate_tcp_frame,
        )
        dispatch_params = {
            "motion_type": "pose_goal",
            "pose_goal": pose_goal,
            "base_frame": candidate_base_frame,
            "tcp_frame": candidate_tcp_frame,
        }
        # The requested TCP pose is always known for a Cartesian target, so a
        # blocked candidate can still render a red ghost at that pose.
        requested_pose = CandidatePose(
            position_m=list(pose_goal["position_m"]),
            orientation_xyzw=list(pose_goal["orientation_xyzw"]),
        )
        return self._run_preview(
            req,
            exec_request,
            start,
            start_state_model,
            snapshot,
            _make,
            normalized=None,
            dispatch_params=dispatch_params,
            ghost_kind="cartesian",
            ghost_joint_names=None,
            ghost_joint_positions=None,
            blocked_target_pose=requested_pose,
        )

    def _plan_gripper(self, req, snapshot, _make):
        """Validate a gripper candidate with the lightweight gripper-command
        path (no arm IK/collision planning, no motion, no scene mutation). A
        ready candidate stages dispatch params for the execute route to send
        exactly one gripper command through the existing bridge after an
        approval-time revalidation."""
        assert req.gripper_target is not None
        gt = req.gripper_target
        validation = validate_gripper_command(
            self._state,
            gripper_id=gt.gripper_id,
            target_position=gt.target_position,
            max_effort=gt.max_effort,
            duration_s=gt.duration_s,
        )
        if not validation.ok or validation.request is None:
            # A limit/range/malformed target is a non-executable BLOCKED
            # candidate; missing deployment/kernel/gripper config is an
            # infrastructure ERROR.
            status = (
                CandidateStatus.BLOCKED
                if validation.reason_code
                in {GRIPPER_REASON_LIMIT, GRIPPER_REASON_MALFORMED}
                else CandidateStatus.ERROR
            )
            response = _make(
                status,
                diagnostics=CandidateDiagnostics(
                    reason_code=validation.reason_code,
                    summary=validation.detail,
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)
        # Current gripper command state: a target staged while the gripper is
        # already moving is a non-executable BLOCKED candidate (Approve also
        # re-checks this at dispatch).
        busy = gripper_busy_detail(self._state, gt.gripper_id)
        if busy is not None:
            response = _make(
                CandidateStatus.BLOCKED,
                diagnostics=CandidateDiagnostics(
                    reason_code=GRIPPER_REASON_BUSY, summary=busy
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)
        request = validation.request
        # Stage the gripper dispatch for the execute route. `motion_type`
        # discriminates the gripper bridge from the arm intervention path.
        dispatch_params = {
            "motion_type": "gripper",
            "gripper_id": request.gripper_id,
            "target_position": request.target_position,
            "max_effort": request.max_effort,
            "duration_s": request.duration_s,
        }
        details: dict[str, Any] = {}
        if validation.limit_lower is not None and validation.limit_upper is not None:
            details = {
                "limitLower": validation.limit_lower,
                "limitUpper": validation.limit_upper,
            }
        response = _make(
            CandidateStatus.READY,
            commit_eligible=True,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_READY,
                summary="Gripper target validated; ready to approve.",
                details=details,
            ),
        )
        return response, _StoredCandidate(response, snapshot, dispatch_params)

    def _run_preview(
        self,
        req,
        exec_request: ExecuteRequest,
        start: RobotStateSnapshot,
        start_state_model: CandidateRobotState,
        snapshot: _PlanningSnapshot,
        _make,
        *,
        normalized: Optional[CandidateJointTarget],
        dispatch_params: dict,
        ghost_kind: str,
        ghost_joint_names: Optional[list[str]],
        ghost_joint_positions: Optional[list[float]],
        blocked_target_pose: Optional[CandidatePose],
    ):
        try:
            preview = self._state.kernel.preview_motion(exec_request, start)
        except Exception as exc:  # noqa: BLE001 — preview failures are fail-closed
            response = _make(
                CandidateStatus.ERROR,
                normalized=normalized,
                start_state=start_state_model,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_BACKEND_UNAVAILABLE,
                    summary=f"Candidate preview planning raised: {exc}",
                ),
            )
            return response, _StoredCandidate(response, snapshot, None)

        if not preview.ok:
            diagnostics = _classify_preview_failure(preview)
            ghost = CandidateGhost(
                blocked=True,
                blocked_target_pose=blocked_target_pose,
            )
            # For a blocked joint candidate the requested joints are known, so
            # we can still try to render the configuration ghost.
            if ghost_kind == "joint":
                link_poses = self._maybe_link_poses(
                    kind="joint",
                    joint_names=ghost_joint_names,
                    joint_positions=ghost_joint_positions,
                    pose_goal=None,
                    base_frame=exec_request.base_frame,
                    tcp_frame=exec_request.tcp_frame,
                )
                ghost.final_joint_names = ghost_joint_names
                ghost.final_joint_positions_rad = ghost_joint_positions
                ghost.link_poses = link_poses
            response = _make(
                CandidateStatus.BLOCKED,
                normalized=normalized,
                start_state=start_state_model,
                diagnostics=diagnostics,
                ghost=ghost,
            )
            # Blocked candidates are not executable: no dispatch params.
            return response, _StoredCandidate(response, snapshot, None)

        # Valid candidate: build the final ghost + optional intermediate
        # trajectory preview.
        if ghost_kind == "joint":
            # For a joint goal the final configuration IS the commanded joint
            # vector by definition; use it directly (some backends/mocks echo
            # the start state as final_state).
            final_joint_names = ghost_joint_names
            final_joint_positions = ghost_joint_positions
            final_tcp = _to_pose(preview.final_tcp_pose)
        else:
            final_state = preview.final_state
            final_joint_names = (
                list(final_state.joint_names) if final_state is not None else None
            )
            final_joint_positions = (
                list(final_state.positions_rad) if final_state is not None else None
            )
            # Achieved TCP pose if the backend reports it, else the requested
            # target pose (which a valid candidate reaches).
            final_tcp = _to_pose(preview.final_tcp_pose) or blocked_target_pose
        ghost = CandidateGhost(
            blocked=False,
            final_joint_names=final_joint_names,
            final_joint_positions_rad=final_joint_positions,
            final_tcp_pose=final_tcp,
            link_poses=self._maybe_link_poses(
                kind="joint",
                joint_names=final_joint_names,
                joint_positions=final_joint_positions,
                pose_goal=None,
                base_frame=exec_request.base_frame,
                tcp_frame=exec_request.tcp_frame,
            )
            if final_joint_positions is not None
            else None,
        )
        preview_traj = self._maybe_preview_trajectory(
            exec_request, start, final_joint_names or []
        )
        response = _make(
            CandidateStatus.READY,
            commit_eligible=True,
            normalized=normalized,
            start_state=start_state_model,
            ghost=ghost,
            preview=preview_traj,
            duration_s=preview.trajectory_duration_s,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_READY,
                summary="Candidate planned and validated; ready to approve.",
            ),
        )
        return response, _StoredCandidate(response, snapshot, dispatch_params)


# --- Candidate store -------------------------------------------------------


class _CandidateStore:
    """In-memory, per-app candidate store with latest-wins supersede.

    Ownership lives server-side for the duration of the Composer process. A
    later chunk promotes lifecycle ownership into the candidate planning
    service / kernel; this store holds no motion or scene state.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        # Native preview planning is CPU-heavy and may contend on backend
        # scene/planner locks. Serialize Composer candidate previews so input
        # bursts do not fan out into many obsolete native planning jobs. Queued
        # jobs call `begin_planning()` after acquiring this lock and skip if a
        # newer draft already superseded them.
        self._planning_lock = threading.Lock()
        self._by_id: dict[str, _StoredCandidate] = {}
        self._latest_by_surface: dict[str, str] = {}
        self._generation_by_surface: dict[str, int] = {}

    def planning_lock(self) -> threading.Lock:
        return self._planning_lock

    def _mark_superseded_locked(self, candidate_id: str, now: int) -> None:
        stored = self._by_id.get(candidate_id)
        if stored is None or stored.response.status not in _SUPERSEDABLE_STATUSES:
            return
        response = stored.response
        response.status = CandidateStatus.SUPERSEDED
        response.commit_eligible = False
        response.diagnostics = CandidateDiagnostics(
            reason_code=REASON_SUPERSEDED,
            summary="Replaced by a newer draft for Robot Control.",
        )
        response.updated_at_ns = now
        stored.dispatch_params = None
        surface = response.source_surface.value
        if self._latest_by_surface.get(surface) == candidate_id:
            self._latest_by_surface.pop(surface, None)

    def reserve(self, req: CandidateCreateRequest) -> tuple[str, int]:
        """Reserve a slot for a new candidate, ordered by REQUEST ARRIVAL.

        This runs FIRST (before the synchronous preview plan), so
        the generation + supersede ordering reflects when each request ARRIVED,
        not when its planning happened to finish. Holding the lock it:
          - assigns the next generation for the surface,
          - supersedes every prior live Robot Control draft,
          - stores a PLANNING placeholder candidate keyed by a new id.
        `fill()` later updates that same id in place with the planning result
        (unless a newer reserve() already superseded it). Returns
        (candidate_id, generation).
        """
        with self._lock:
            now = time.monotonic_ns()
            surface = req.source_surface.value
            # One shared Approve button means one active unapproved Robot
            # Control command. Enforce that server-side, including candidates
            # from another source surface or move group whose preview has not
            # finished yet.
            for prior_id in list(self._by_id):
                self._mark_superseded_locked(prior_id, now)
            generation = self._generation_by_surface.get(surface, 0) + 1
            self._generation_by_surface[surface] = generation
            candidate_id = str(uuid.uuid4())
            placeholder = CandidateResponse(
                candidate_id=candidate_id,
                generation=generation,
                source_surface=req.source_surface,
                input_mode=req.input_mode,
                status=CandidateStatus.PLANNING,
                joint_target=req.joint_target,
                cartesian_target=req.cartesian_target,
                gripper_target=req.gripper_target,
                client_request_id=req.client_request_id,
                max_age_ns=_CANDIDATE_MAX_AGE_NS,
                expires_at_ns=now + _CANDIDATE_MAX_AGE_NS,
                created_at_ns=now,
                updated_at_ns=now,
            )
            # The placeholder has not been planned against any snapshot yet;
            # `_refresh_locked` intentionally does not stale/expire a PLANNING
            # candidate, so this empty snapshot is never compared. `fill()`
            # replaces the whole stored entry (snapshot included) on success.
            self._by_id[candidate_id] = _StoredCandidate(
                response=placeholder,
                snapshot=_EMPTY_PLANNING_SNAPSHOT,
                dispatch_params=None,
            )
            self._latest_by_surface[surface] = candidate_id
            return candidate_id, generation

    def begin_planning(
        self, candidate_id: str
    ) -> tuple[bool, Optional[CandidateResponse]]:
        """Claim a reserved candidate for preview planning.

        Call after acquiring `planning_lock()`. Returns (True, None) only while
        the candidate is still the live PLANNING placeholder. If a newer draft
        or Revert superseded it while it was queued behind another preview,
        return (False, current-copy) so the route can respond without touching
        the native planner.
        """
        with self._lock:
            existing = self._by_id.get(candidate_id)
            if existing is None:
                return False, None
            if existing.response.status is CandidateStatus.PLANNING:
                return True, None
            return False, existing.response.model_copy(deep=True)

    def fill(
        self,
        candidate_id: str,
        response: CandidateResponse,
        stored: _StoredCandidate,
    ) -> CandidateResponse:
        """Update a reserved candidate IN PLACE with its planning result.

        If a newer reserve() already superseded this candidate
        while planning ran, it MUST remain superseded — we do not resurrect it
        to ready/blocked. We keep the reserved id, generation, and the original
        created_at/expires_at timestamps so the candidate's identity and arrival
        order are stable.
        """
        with self._lock:
            existing = self._by_id.get(candidate_id)
            if existing is None:
                # Reserved slot vanished (e.g. reverted). Nothing to fill.
                return response.model_copy(deep=True)
            if existing.response.status not in _SUPERSEDABLE_STATUSES:
                # A newer reserve() superseded this candidate (or it was
                # reverted/expired) while planning ran. Keep it as-is.
                return existing.response.model_copy(deep=True)
            if existing.response.status is not CandidateStatus.PLANNING:
                # Defensive: only a PLANNING placeholder should be filled.
                return existing.response.model_copy(deep=True)

            # Preserve reserved identity + arrival-order timestamps.
            generation = existing.response.generation
            created_at_ns = existing.response.created_at_ns
            expires_at_ns = existing.response.expires_at_ns
            response.candidate_id = candidate_id
            response.generation = generation
            response.max_age_ns = _CANDIDATE_MAX_AGE_NS
            response.created_at_ns = created_at_ns
            response.expires_at_ns = expires_at_ns
            response.updated_at_ns = time.monotonic_ns()
            stored.response = response
            self._by_id[candidate_id] = stored
            return response.model_copy(deep=True)

    def _refresh_locked(
        self, stored: _StoredCandidate, current: _PlanningSnapshot
    ) -> None:
        """Recompute lazily-derived lifecycle status (expired/stale) in place."""
        response = stored.response
        if response.status not in _SUPERSEDABLE_STATUSES:
            return
        # A reserved PLANNING placeholder has not been planned against any
        # snapshot yet: it must not be staled/expired before
        # `fill()` lands the real result, only superseded by a newer reserve().
        if response.status is CandidateStatus.PLANNING:
            return
        now = time.monotonic_ns()
        if response.expires_at_ns and now > response.expires_at_ns:
            response.status = CandidateStatus.EXPIRED
            response.commit_eligible = False
            response.diagnostics = CandidateDiagnostics(
                reason_code=REASON_EXPIRED,
                summary="Candidate expired without approval; re-author to refresh.",
            )
            response.updated_at_ns = now
            stored.dispatch_params = None
            return
        if stored.snapshot != current:
            response.status = CandidateStatus.STALE
            response.commit_eligible = False
            response.diagnostics = CandidateDiagnostics(
                reason_code=REASON_STALE,
                summary=(
                    "Robot state / loaded program changed since planning; "
                    "re-author or re-approve to revalidate."
                ),
            )
            response.updated_at_ns = now
            stored.dispatch_params = None

    def get(
        self, candidate_id: str, current: _PlanningSnapshot
    ) -> Optional[CandidateResponse]:
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None
            self._refresh_locked(stored, current)
            return stored.response.model_copy(deep=True)

    def contains(self, candidate_id: str) -> bool:
        with self._lock:
            return candidate_id in self._by_id

    def revert(self, candidate_id: str) -> Optional[CandidateResponse]:
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None
            response = stored.response
            if response.status is CandidateStatus.EXECUTING:
                # Already committed to dispatch (see `commit_for_dispatch`): a
                # revert cannot recall motion, so leave the candidate as-is
                # rather than mark it REVERTED and falsely imply no move ran.
                return response.model_copy(deep=True)
            response.status = CandidateStatus.REVERTED
            response.commit_eligible = False
            response.diagnostics = CandidateDiagnostics(
                reason_code=REASON_REVERTED,
                summary="Draft discarded by the operator; no motion.",
            )
            response.updated_at_ns = time.monotonic_ns()
            stored.dispatch_params = None
            surface = response.source_surface.value
            if self._latest_by_surface.get(surface) == candidate_id:
                self._latest_by_surface.pop(surface, None)
            return response.model_copy(deep=True)

    def begin_approval(
        self, candidate_id: str, current: _PlanningSnapshot
    ) -> tuple[Optional[_StoredCandidate], Optional[CandidateResponse]]:
        """Return (stored, snapshot-copy) when the candidate is gate-eligible
        for approval. Refreshes staleness/expiry first. Returns (None, copy)
        when present-but-ineligible, (None, None) when unknown."""
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None, None
            self._refresh_locked(stored, current)
            response = stored.response
            if response.status is CandidateStatus.READY and response.commit_eligible:
                response.status = CandidateStatus.APPROVING
                response.updated_at_ns = time.monotonic_ns()
                return stored, response.model_copy(deep=True)
            return None, response.model_copy(deep=True)

    def finish_approval(
        self,
        candidate_id: str,
        *,
        status: CandidateStatus,
        diagnostics: Optional[CandidateDiagnostics] = None,
        execution: Optional[CandidateExecution] = None,
    ) -> Optional[CandidateResponse]:
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None
            response = stored.response
            response.status = status
            response.commit_eligible = False
            if diagnostics is not None:
                response.diagnostics = diagnostics
            if execution is not None:
                response.execution = execution
            response.updated_at_ns = time.monotonic_ns()
            if status is not CandidateStatus.EXECUTING:
                stored.dispatch_params = None
            return response.model_copy(deep=True)

    def refresh_execution(
        self,
        candidate_id: str,
        *,
        status: CandidateStatus,
        diagnostics: CandidateDiagnostics,
        execution: CandidateExecution,
    ) -> Optional[CandidateResponse]:
        """Update the runtime lifecycle for a dispatched candidate.

        Execution state is observed from the cycle/kernel outside the store
        lock, then committed here only if the candidate is still the same
        dispatched candidate. This keeps poll-time lifecycle updates
        idempotent and prevents a stale poll from resurrecting a discarded or
        already-terminal candidate.
        """
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None
            response = stored.response
            if response.status is not CandidateStatus.EXECUTING:
                return response.model_copy(deep=True)
            response.execution = execution
            response.status = status
            response.commit_eligible = False
            response.diagnostics = diagnostics
            response.updated_at_ns = time.monotonic_ns()
            if status is not CandidateStatus.EXECUTING:
                stored.dispatch_params = None
            return response.model_copy(deep=True)

    def refresh_gripper_execution(
        self,
        candidate_id: str,
        *,
        status: CandidateStatus,
        diagnostics: CandidateDiagnostics,
    ) -> Optional[CandidateResponse]:
        """Update the lifecycle for a dispatched gripper candidate.

        Gripper candidates dispatch through the lightweight gripper bridge, so
        they intentionally do not carry an arm `CandidateExecution` record or
        intervention session. Poll-time lifecycle still has to terminate from
        the kernel gripper status; otherwise the browser can keep a stale local
        `executing` candidate forever and block subsequent gripper commands.
        """
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return None
            response = stored.response
            if response.status is not CandidateStatus.EXECUTING:
                return response.model_copy(deep=True)
            response.status = status
            response.commit_eligible = False
            response.diagnostics = diagnostics
            response.updated_at_ns = time.monotonic_ns()
            if status is not CandidateStatus.EXECUTING:
                stored.dispatch_params = None
            return response.model_copy(deep=True)

    def commit_for_dispatch(
        self, candidate_id: str
    ) -> tuple[bool, Optional[CandidateResponse]]:
        """Atomically confirm the candidate is STILL approving and flip it to
        EXECUTING under the lock, returning (committed, current-view).

        This closes the begin_approval -> revalidate -> dispatch TOCTOU window.
        `begin_approval` sets APPROVING and the caller caches dispatch_params,
        but the mandatory revalidation (a kernel preview) runs WITHOUT the lock;
        a concurrent `revert` could flip the candidate to REVERTED in that gap
        while the caller still holds the cached params. Re-checking here, under
        the lock, immediately before dispatch makes the decision atomic:
          - a revert that lands BEFORE this commit wins -> (False, ...), the
            caller must NOT dispatch (fail-closed, no motion);
          - a revert that lands AFTER sees EXECUTING and is refused (see
            `revert`), so it cannot falsely claim to have prevented a move.
        """
        with self._lock:
            stored = self._by_id.get(candidate_id)
            if stored is None:
                return False, None
            response = stored.response
            if (
                response.status is not CandidateStatus.APPROVING
                or stored.dispatch_params is None
            ):
                return False, response.model_copy(deep=True)
            response.status = CandidateStatus.EXECUTING
            response.commit_eligible = False
            response.updated_at_ns = time.monotonic_ns()
            return True, response.model_copy(deep=True)


# --- Intervention dispatch helpers (mirror routes_direct) ------------------


def _deployment_capability_keys(state: ComposerState) -> set[str] | None:
    if state.deployment is None:
        return None
    return state.deployment.capability_keys()


def _parent_run_id_for_intervention(state: ComposerState) -> str | None:
    if state.cycle_manager is None:
        return None
    current = state.event_recorder.current_run_id
    if current is not None:
        return current
    if state.cycle_manager.state.value == "stopped":
        return state.event_recorder.last_run_id
    return None


_KERNEL_TERMINAL_TO_CANDIDATE = {
    ExecutionState.COMPLETED: CandidateStatus.COMPLETED,
    ExecutionState.STOPPED: CandidateStatus.STOPPED,
    ExecutionState.INTERRUPTED: CandidateStatus.INTERRUPTED,
    ExecutionState.INTERVENTION_REQUIRED: CandidateStatus.INTERRUPTED,
    ExecutionState.FAILED: CandidateStatus.FAILED,
}


def _execution_reason_for_status(status: CandidateStatus) -> str:
    if status is CandidateStatus.EXECUTING:
        return REASON_EXECUTION_RUNNING
    if status is CandidateStatus.COMPLETED:
        return REASON_EXECUTION_COMPLETED
    if status is CandidateStatus.STOPPED:
        return REASON_EXECUTION_STOPPED
    if status is CandidateStatus.INTERRUPTED:
        return REASON_EXECUTION_INTERRUPTED
    if status is CandidateStatus.FAILED:
        return REASON_EXECUTION_FAILED
    return REASON_READY


def _execution_summary_for_status(
    status: CandidateStatus,
    *,
    runtime_error: Optional[str],
    session_error: Optional[str],
) -> str:
    if status is CandidateStatus.COMPLETED:
        return "Candidate execution completed."
    cause = runtime_error or session_error
    if cause:
        return cause
    if status is CandidateStatus.STOPPED:
        return "Candidate execution stopped before completion."
    if status is CandidateStatus.INTERRUPTED:
        return "Candidate execution was interrupted; recover or reset before retrying."
    if status is CandidateStatus.FAILED:
        return "Candidate execution failed."
    return "Candidate execution is running."


def _snapshot_error_details(snapshot: Any) -> Optional[dict[str, Any]]:
    details = getattr(snapshot, "error_details", None)
    if isinstance(details, dict) and details:
        return dict(details)
    return None


def _refresh_gripper_candidate_execution_from_runtime(
    state: ComposerState,
    store: _CandidateStore,
    candidate: CandidateResponse,
) -> CandidateResponse:
    """Refresh an EXECUTING gripper candidate from the kernel gripper status."""
    if candidate.status is not CandidateStatus.EXECUTING:
        return candidate
    if candidate.input_mode is not CandidateInputMode.GRIPPER:
        return candidate
    if candidate.gripper_target is None or state.kernel is None:
        return candidate

    gripper_id = candidate.gripper_target.gripper_id
    advance = getattr(state.kernel, "update_gripper_commands", None)
    if callable(advance):
        try:
            advance()
        except Exception:  # noqa: BLE001 - observer path must stay best-effort
            pass
    get_status = getattr(state.kernel, "get_gripper_status", None)
    if not callable(get_status):
        return candidate
    try:
        gripper_status = get_status(gripper_id)
    except Exception:  # noqa: BLE001 - observer path must stay best-effort
        return candidate

    raw_state = getattr(gripper_status, "state", None)
    candidate_status = CandidateStatus.EXECUTING
    if raw_state == GripperCommandState.COMPLETED:
        candidate_status = CandidateStatus.COMPLETED
    elif raw_state == GripperCommandState.FAILED:
        candidate_status = CandidateStatus.FAILED
    elif raw_state == GripperCommandState.IDLE:
        # A bridge that drops completed gripper commands from its status table
        # is still no longer executing. If the reported position is at the
        # requested target, classify as completed; otherwise fail closed so the
        # operator sees that the lifecycle could not be proven successful.
        current = getattr(gripper_status, "current_position", None)
        target = candidate.gripper_target.target_position
        if (
            isinstance(current, (int, float))
            and math.isfinite(float(current))
            and abs(float(current) - float(target)) <= 1e-4
        ):
            candidate_status = CandidateStatus.COMPLETED
        else:
            candidate_status = CandidateStatus.FAILED

    detail_error = getattr(gripper_status, "error", None)
    diagnostics = CandidateDiagnostics(
        reason_code=_execution_reason_for_status(candidate_status),
        summary=_execution_summary_for_status(
            candidate_status,
            runtime_error=detail_error if isinstance(detail_error, str) else None,
            session_error=None,
        ),
        details={
            "gripperId": gripper_id,
            "gripperState": (
                raw_state.value if isinstance(raw_state, GripperCommandState) else str(raw_state)
            ),
            "progress": float(getattr(gripper_status, "progress", 0.0) or 0.0),
            "currentPosition": getattr(gripper_status, "current_position", None),
            "targetPosition": getattr(gripper_status, "target_position", None),
            "commandedPosition": getattr(
                gripper_status, "commanded_position", None
            ),
            "observedPosition": getattr(gripper_status, "observed_position", None),
            "commandedState": getattr(gripper_status, "commanded_state", ""),
            "actuationKind": getattr(gripper_status, "actuation_kind", ""),
            "stateAuthority": getattr(gripper_status, "state_authority", ""),
            "completionPolicy": getattr(gripper_status, "completion_policy", ""),
            "graspConfirmationPolicy": getattr(
                gripper_status, "grasp_confirmation_policy", ""
            ),
            "transportKind": getattr(gripper_status, "transport_kind", ""),
            "evidence": list(getattr(gripper_status, "evidence", []) or []),
            "evidenceState": getattr(gripper_status, "evidence_state", ""),
            "error": detail_error,
        },
    )
    refreshed = store.refresh_gripper_execution(
        candidate.candidate_id,
        status=candidate_status,
        diagnostics=diagnostics,
    )
    return refreshed if refreshed is not None else candidate


def _refresh_candidate_execution_from_runtime(
    state: ComposerState,
    store: _CandidateStore,
    candidate: CandidateResponse,
) -> CandidateResponse:
    """Refresh an EXECUTING candidate from cycle/kernel runtime ownership.

    The candidate endpoint is the UI's lifecycle authority after Approve. It
    tracks the intervention session and the kernel move id, so the panel can
    show completed/stopped/interrupted/failed instead of a stale generic
    `executing` state once the normal execution machinery reaches a terminal
    outcome.
    """
    if candidate.status is not CandidateStatus.EXECUTING:
        return candidate
    if candidate.input_mode is CandidateInputMode.GRIPPER:
        return _refresh_gripper_candidate_execution_from_runtime(state, store, candidate)
    execution = candidate.execution
    if execution is None:
        return candidate
    if state.cycle_manager is None:
        return candidate

    view = execution.model_copy(deep=True)
    session_error: Optional[str] = None
    session = state.cycle_manager.intervention
    if session is not None and session.session_id == view.session_id:
        view.session_status = session.status
        view.session_actual_outcome = session.actual_outcome
        view.session_error = session.error
        session_error = session.error
    else:
        session = None

    runtime_error: Optional[str] = None
    snapshot = None
    status = None
    try:
        status = state.cycle_manager.get_status()
    except Exception:  # noqa: BLE001 - observer path must stay best-effort
        status = None
    try:
        snapshot = state.cycle_manager.get_execution_snapshot()
    except Exception:  # noqa: BLE001 - older/test kernels may omit snapshots
        snapshot = None

    if snapshot is not None:
        snapshot_error = getattr(snapshot, "error", None)
        if isinstance(snapshot_error, str) and snapshot_error.strip():
            runtime_error = snapshot_error.strip()
        view.error = runtime_error
        view.error_code = getattr(snapshot, "error_code", None)
        view.error_details = _snapshot_error_details(snapshot)

    candidate_status = CandidateStatus.EXECUTING
    matched_kernel_state = None
    if status is not None:
        status_move_id = getattr(status, "active_move_id", "") or ""
        status_execution_id = int(getattr(status, "execution_id", 0) or 0)
        same_move = bool(view.move_id) and status_move_id == view.move_id
        same_execution = bool(view.execution_id) and status_execution_id == view.execution_id
        if same_move or same_execution:
            matched_kernel_state = status.state
            view.execution_id = status_execution_id
            view.state = status.state.value
            view.progress = float(getattr(status, "progress", 0.0) or 0.0)
            candidate_status = _KERNEL_TERMINAL_TO_CANDIDATE.get(
                status.state, CandidateStatus.EXECUTING
            )

    if session is not None and session.status != "running":
        if session.status == "completed" and matched_kernel_state is None:
            # The intervention BT only reports "completed" after the motion
            # node has seen kernel Completed and acknowledged it. That ack
            # transitions the kernel to Idle and clears active_move_id, so the
            # status read above no longer matches this move. Treat the session
            # as the terminal proof in that post-ack case, and clear any stale
            # pre-ack "executing" view carried from an earlier poll.
            candidate_status = CandidateStatus.COMPLETED
            view.state = ExecutionState.COMPLETED.value
            view.progress = 1.0
            view.error = None
            view.error_code = None
            view.error_details = None
            runtime_error = None
        elif session.status == "completed":
            # If the kernel still reports the same motion in a non-terminal
            # state, keep the candidate executing. This avoids using session
            # bookkeeping to mask an active or stale in-flight kernel motion.
            pass
        elif session.status == "aborted":
            candidate_status = CandidateStatus.STOPPED
            view.state = ExecutionState.STOPPED.value
        else:
            candidate_status = CandidateStatus.FAILED
            view.state = ExecutionState.FAILED.value

    # Motion soft-fail / interrupt paths can leave the intervention session
    # running while the kernel has already latched a terminal state. The kernel
    # terminal mapping above wins in that case. If no matching kernel terminal
    # was visible but the cycle has surfaced an error, fail closed in the UI.
    if (
        candidate_status is CandidateStatus.EXECUTING
        and state.cycle_manager.state.value == "stopped"
        and state.cycle_manager.error
    ):
        runtime_error = state.cycle_manager.error
        candidate_status = CandidateStatus.FAILED

    diagnostics = CandidateDiagnostics(
        reason_code=_execution_reason_for_status(candidate_status),
        summary=_execution_summary_for_status(
            candidate_status,
            runtime_error=runtime_error,
            session_error=session_error,
        ),
        details={
            "moveId": view.move_id,
            "executionId": view.execution_id,
            "executionState": view.state,
            "sessionId": view.session_id,
            "sessionStatus": view.session_status,
            "sessionActualOutcome": view.session_actual_outcome,
            "errorCode": view.error_code,
            "errorDetails": view.error_details,
        },
    )
    refreshed = store.refresh_execution(
        candidate.candidate_id,
        status=candidate_status,
        diagnostics=diagnostics,
        execution=view,
    )
    return refreshed if refreshed is not None else candidate


@dataclass
class _DispatchError(Exception):
    status_code: int
    reason_code: str
    message: str
    details: Optional[list] = None


def _dispatch_candidate(
    state: ComposerState, dispatch_params: dict, principal_id
) -> tuple[Any, str]:
    """Build, validate, and dispatch the candidate's motion through the normal
    intervention path. Raises `_DispatchError` on any gate failure. Mirrors the
    `/api/direct/{joint,cartesian}-jog` machinery so Robot Control motion shares
    the kernel's runtime collision monitoring, interrupts, and stop/reset."""
    if state.cycle_manager is None:
        raise _DispatchError(409, REASON_NO_PROGRAM, "No program loaded")
    if state.kernel is None:
        raise _DispatchError(409, REASON_BACKEND_UNAVAILABLE, "No kernel available")

    instance_name = f"rc_approve_{time.monotonic_ns()}"
    params = {"move_id": instance_name, **dispatch_params}
    tree_dict = {
        "id": "move_manipulator_action",
        "name": instance_name,
        "params": params,
        "children": [],
    }
    try:
        tree = tree_node_from_dict(tree_dict)
    except Exception as exc:  # noqa: BLE001
        raise _DispatchError(
            400, REASON_MALFORMED_TARGET, f"Failed to build candidate intervention: {exc}"
        )
    errors, _warnings = collect_tree_validation_issues(
        tree,
        state.catalog,
        parameter_defaults=(
            state.program.get_parameter_defaults() if state.program is not None else None
        ),
        blackboard_namespaces=(
            state.program.blackboard_namespaces if state.program is not None else None
        ),
        blackboard_keys=merged_intervention_blackboard_keys(
            tree,
            state.catalog,
            base_keys=(
                state.program.blackboard_keys if state.program is not None else None
            ),
        ),
        skill_catalog=state.skill_catalog,
        program_profile=(
            state.program.program_profile if state.program is not None else None
        ),
        deployment_capability_keys=_deployment_capability_keys(state),
    )
    if errors:
        raise _DispatchError(
            400,
            REASON_MALFORMED_TARGET,
            errors[0].message,
            details=[{"path": issue.path, "message": issue.message} for issue in errors],
        )
    try:
        session = state.cycle_manager.execute_intervention(
            tree_dict,
            parent_run_id=_parent_run_id_for_intervention(state),
            lease_timeout_ms=10_000,
            resume_target="idle",
            session_kind="manual_intervention",
            authority_source="human",
            principal_id=principal_id,
            grant_type="human_approved",
            trigger_reason="composer_robot_control_candidate_approve",
            trigger_node_instance=None,
            approval_mode="human_approved",
            artifact_origin="runtime_override",
        )
    except RuntimeError as exc:
        raise _DispatchError(409, REASON_BACKEND_UNAVAILABLE, str(exc))
    return session, instance_name


def _session_summary(session) -> dict:
    return {
        "sessionId": session.session_id,
        "status": session.status,
        "entryState": session.entry_state,
        "actualOutcome": session.actual_outcome,
        "error": session.error,
    }


def _execute_gripper_candidate(
    state: ComposerState,
    store: "_CandidateStore",
    candidate_id: str,
    dispatch_params: dict,
) -> JSONResponse:
    """Approve a gripper candidate: revalidate the same deployment/config/limit
    facts, confirm no other authority owns execution, atomically commit, then
    send EXACTLY ONE gripper command through the shared bridge. Fails closed
    (no command) on any revalidation / cycle / commit gap."""
    validation = validate_gripper_command(
        state,
        gripper_id=dispatch_params["gripper_id"],
        target_position=dispatch_params["target_position"],
        max_effort=dispatch_params.get("max_effort"),
        duration_s=dispatch_params.get("duration_s"),
    )
    if not validation.ok or validation.request is None:
        failed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.STALE,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_REPLAN_REQUIRED,
                summary=validation.detail or "gripper revalidation failed",
            ),
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": REASON_REPLAN_REQUIRED,
                "detail": "gripper candidate failed pre-dispatch revalidation; no command",
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    busy = gripper_cycle_busy_detail(state)
    if busy is not None:
        failed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.STALE,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_REPLAN_REQUIRED, summary=busy
            ),
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": REASON_REPLAN_REQUIRED,
                "detail": busy,
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    # Current gripper command state: fail closed (no command, no commit) if the
    # gripper is already executing, instead of committing to EXECUTING and
    # relying on the kernel to reject after dispatch.
    gripper_busy = gripper_busy_detail(state, dispatch_params["gripper_id"])
    if gripper_busy is not None:
        failed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.STALE,
            diagnostics=CandidateDiagnostics(
                reason_code=GRIPPER_REASON_BUSY, summary=gripper_busy
            ),
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": GRIPPER_REASON_BUSY,
                "detail": gripper_busy,
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    # Atomic commit closes the begin_approval -> revalidate -> dispatch TOCTOU:
    # a concurrent revert that beat us wins (no command).
    committed, commit_view = store.commit_for_dispatch(candidate_id)
    if not committed:
        reason = (
            commit_view.diagnostics.reason_code
            if commit_view is not None and commit_view.diagnostics is not None
            else REASON_NOT_READY
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": reason,
                "detail": "candidate was reverted or changed during approval; no command",
                "candidate": (
                    commit_view.model_dump(by_alias=True)
                    if commit_view is not None
                    else None
                ),
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    try:
        result = state.kernel.command_gripper(validation.request)
    except Exception as exc:  # noqa: BLE001 — dispatch failures are fail-closed
        failed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.ERROR,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_BACKEND_UNAVAILABLE,
                summary=f"gripper command raised: {exc}",
            ),
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": REASON_BACKEND_UNAVAILABLE,
                "detail": "gripper command dispatch failed; check kernel",
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    if not getattr(result, "accepted", False):
        detail = getattr(result, "error", None) or "gripper command rejected"
        failed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.ERROR,
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_BACKEND_UNAVAILABLE, summary=detail
            ),
        )
        return JSONResponse(
            status_code=409,
            content={
                "reasonCode": REASON_BACKEND_UNAVAILABLE,
                "detail": detail,
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            },
        )
    executed = store.finish_approval(
        candidate_id,
        status=CandidateStatus.EXECUTING,
        diagnostics=CandidateDiagnostics(
            reason_code=REASON_READY,
            summary="Gripper candidate approved; one gripper command dispatched.",
        ),
    )
    return JSONResponse(
        status_code=200,
        content={
            "candidate": executed.model_dump(by_alias=True) if executed else None,
            "session": None,
            "timestamp_ns": time.monotonic_ns(),
        },
    )


def _plan_reserved_candidate(
    state: ComposerState,
    store: _CandidateStore,
    candidate_id: str,
    req: CandidateCreateRequest,
) -> CandidateResponse:
    """Plan a reserved candidate if it is still current.

    This is the backend half of latest-wins coalescing. `reserve()` records
    request-arrival order immediately; this helper serializes the expensive
    preview work and checks the reserved placeholder again before entering the
    native planner. Superseded queued drafts return their current terminal
    response without consuming planner time.
    """
    with store.planning_lock():
        should_plan, current = store.begin_planning(candidate_id)
        if not should_plan:
            if current is not None:
                _LOGGER.info(
                    "robot-control candidate preview skipped: "
                    "candidate_id=%s status=%s input_mode=%s source_surface=%s",
                    candidate_id,
                    current.status.value,
                    current.input_mode.value,
                    current.source_surface.value,
                )
                return current
            raise RuntimeError(f"reserved candidate {candidate_id} disappeared")

        started_ns = time.monotonic_ns()
        planner = _CandidatePlanner(state)
        response, stored = planner.plan(req)
        committed = store.fill(candidate_id, response, stored)
        duration_ms = (time.monotonic_ns() - started_ns) / 1_000_000.0
        _LOGGER.info(
            "robot-control candidate preview finished: "
            "candidate_id=%s status=%s input_mode=%s source_surface=%s duration_ms=%.1f",
            candidate_id,
            committed.status.value,
            committed.input_mode.value,
            committed.source_surface.value,
            duration_ms,
        )
        return committed


def create_robot_control_candidates_router(state: ComposerState) -> APIRouter:
    router = APIRouter()
    store = _CandidateStore()

    @router.post("/candidates")
    async def create_candidate(request: Request) -> JSONResponse:
        raw = await request.body()
        try:
            # Reject NaN / Infinity / -Infinity at parse time so a non-finite
            # payload returns a clean 422 rather than a 500.
            data: Any = json.loads(raw or b"{}", parse_constant=_reject_json_constant)
        except ValueError as exc:
            raise HTTPException(
                status_code=422, detail=f"malformed request body: {exc}"
            )
        try:
            req = CandidateCreateRequest.model_validate(data)
        except ValidationError as exc:
            raise HTTPException(status_code=422, detail=jsonable_encoder(exc.errors()))
        # Reserve the generation slot FIRST (ordered by request ARRIVAL), then
        # run the synchronous preview plan OFF the event loop. Preview work is
        # serialized and rechecked before it enters the native planner, so
        # obsolete queued drafts are skipped instead of consuming planner time.
        candidate_id, _generation = store.reserve(req)
        committed = await run_in_threadpool(
            _plan_reserved_candidate, state, store, candidate_id, req
        )
        return JSONResponse(status_code=200, content=committed.model_dump(by_alias=True))

    @router.get("/candidates/{candidate_id}")
    def get_candidate(candidate_id: str) -> JSONResponse:
        current = _compute_planning_snapshot(state)
        candidate = store.get(candidate_id, current)
        if candidate is None:
            raise HTTPException(status_code=404, detail="candidate not found")
        candidate = _refresh_candidate_execution_from_runtime(state, store, candidate)
        return JSONResponse(status_code=200, content=candidate.model_dump(by_alias=True))

    @router.post("/candidates/{candidate_id}/revert")
    def revert_candidate(candidate_id: str) -> JSONResponse:
        candidate = store.revert(candidate_id)
        if candidate is None:
            raise HTTPException(status_code=404, detail="candidate not found")
        return JSONResponse(status_code=200, content=candidate.model_dump(by_alias=True))

    @router.post("/candidates/{candidate_id}/execute")
    def execute_candidate(candidate_id: str, http_request: Request) -> JSONResponse:
        """Approve one fresh candidate and commit it through the normal
        execution path (Phase 4-P5.3). Fails closed on any freshness/validation
        gap; never moves the robot otherwise."""
        if not store.contains(candidate_id):
            raise HTTPException(status_code=404, detail="candidate not found")

        stop_block = state.motion_arm_admission_block_reason()
        if stop_block:
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": stop_block["error_code"],
                    "detail": stop_block["message"],
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        current = _compute_planning_snapshot(state)
        stored, snapshot_view = store.begin_approval(candidate_id, current)
        if stored is None:
            # Present but not gate-eligible (stale/expired/superseded/blocked/...).
            assert snapshot_view is not None
            reason = (
                snapshot_view.diagnostics.reason_code
                if snapshot_view.diagnostics is not None
                else REASON_NOT_READY
            )
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": reason,
                    "detail": "candidate is not a fresh ready candidate; no motion",
                    "candidate": snapshot_view.model_dump(by_alias=True),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        dispatch_params = stored.dispatch_params
        if dispatch_params is None:
            failed = store.finish_approval(
                candidate_id,
                status=CandidateStatus.ERROR,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_NOT_READY,
                    summary="Candidate has no executable plan.",
                ),
            )
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": REASON_NOT_READY,
                    "detail": "candidate has no executable plan; no motion",
                    "candidate": failed.model_dump(by_alias=True) if failed else None,
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Gripper candidates dispatch through the lightweight gripper bridge (not
        # the arm intervention path): revalidate the same deployment/config/limit
        # facts, atomically commit, then send exactly one gripper command.
        if dispatch_params.get("motion_type") == "gripper":
            return _execute_gripper_candidate(
                state, store, candidate_id, dispatch_params
            )

        # MANDATORY pre-dispatch revalidation against the CURRENT robot state.
        # This is the freshness gate that also catches scene/collision-world
        # changes the coarse fingerprint cannot see: if the motion no longer
        # validates, fail closed with no motion.
        planner = _CandidatePlanner(state)
        start = planner._start_state()
        if start is None:
            failed = store.finish_approval(
                candidate_id,
                status=CandidateStatus.STALE,
                diagnostics=CandidateDiagnostics(
                    reason_code=REASON_NO_ROBOT_STATE,
                    summary="Robot state unavailable at approval; no motion.",
                ),
            )
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": REASON_NO_ROBOT_STATE,
                    "detail": "robot state unavailable at approval; no motion",
                    "candidate": failed.model_dump(by_alias=True) if failed else None,
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        recheck_request = planner._build_execute_request(
            move_id=f"rc_candidate_recheck_{uuid.uuid4().hex}",
            motion_type=dispatch_params["motion_type"],
            joint_names=dispatch_params.get("joint_goal_names"),
            joint_positions=dispatch_params.get("joint_goal_positions_rad"),
            pose_goal=dispatch_params.get("pose_goal"),
            # Revalidate against the same frames the candidate was
            # planned with (cartesian frameId / tcpFrame), not the program
            # defaults, so the pre-dispatch gate matches the planned motion.
            base_frame=dispatch_params.get("base_frame"),
            tcp_frame=dispatch_params.get("tcp_frame"),
        )
        try:
            recheck = state.kernel.preview_motion(recheck_request, start)
        except Exception as exc:  # noqa: BLE001
            recheck = MotionPreviewResult(ok=False, error=f"revalidation raised: {exc}")
        if not recheck.ok:
            diagnostics = _classify_preview_failure(recheck)
            diagnostics.reason_code = REASON_REPLAN_REQUIRED
            failed = store.finish_approval(
                candidate_id, status=CandidateStatus.STALE, diagnostics=diagnostics
            )
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": REASON_REPLAN_REQUIRED,
                    "detail": (
                        "candidate failed pre-dispatch revalidation against current "
                        "state; no motion"
                    ),
                    "candidate": failed.model_dump(by_alias=True) if failed else None,
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Close the begin_approval -> revalidate -> dispatch TOCTOU: a concurrent
        # revert can flip the candidate to REVERTED after begin_approval cached
        # dispatch_params and while the (lock-free) revalidation ran. Re-commit
        # atomically under the store lock immediately before dispatch so a revert
        # that beat us wins (no motion) and a revert that arrives after sees
        # EXECUTING. Fail closed if the candidate is no longer approving.
        committed, commit_view = store.commit_for_dispatch(candidate_id)
        if not committed:
            reason = (
                commit_view.diagnostics.reason_code
                if commit_view is not None and commit_view.diagnostics is not None
                else REASON_NOT_READY
            )
            return JSONResponse(
                status_code=409,
                content={
                    "reasonCode": reason,
                    "detail": (
                        "candidate was reverted or changed during approval; no motion"
                    ),
                    "candidate": (
                        commit_view.model_dump(by_alias=True)
                        if commit_view is not None
                        else None
                    ),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Freshness proven and dispatch committed: run the normal intervention
        # path. Approve is an explicit human action; use the authenticated
        # principal when present, else a stable operator id so the intervention
        # audit trail always identifies the Robot Control surface.
        auth_principal = getattr(http_request.state, "principal_id", None)
        principal = auth_principal or "robot_control_operator"
        try:
            session, move_id = _dispatch_candidate(state, dispatch_params, principal)
        except _DispatchError as exc:
            failed = store.finish_approval(
                candidate_id,
                status=CandidateStatus.ERROR,
                diagnostics=CandidateDiagnostics(
                    reason_code=exc.reason_code, summary=exc.message
                ),
            )
            content: dict[str, Any] = {
                "reasonCode": exc.reason_code,
                "detail": exc.message,
                "candidate": failed.model_dump(by_alias=True) if failed else None,
                "timestamp_ns": time.monotonic_ns(),
            }
            if exc.details is not None:
                content["details"] = exc.details
            return JSONResponse(status_code=exc.status_code, content=content)

        # Optimistic joint cache update for joint goals (mirrors direct-jog) so
        # the panel does not snap back while the move completes.
        if dispatch_params["motion_type"] == "joint_goal":
            state.record_current_joint_positions(
                list(dispatch_params["joint_goal_positions_rad"])
            )

        executed = store.finish_approval(
            candidate_id,
            status=CandidateStatus.EXECUTING,
            execution=CandidateExecution(
                move_id=move_id,
                session_id=session.session_id,
                session_status=session.status,
                session_actual_outcome=session.actual_outcome,
                session_error=session.error,
            ),
            diagnostics=CandidateDiagnostics(
                reason_code=REASON_EXECUTION_RUNNING,
                summary="Candidate execution is running.",
            ),
        )
        return JSONResponse(
            status_code=200,
            content={
                "candidate": executed.model_dump(by_alias=True) if executed else None,
                "session": _session_summary(session),
                "timestamp_ns": time.monotonic_ns(),
            },
        )

    return router
