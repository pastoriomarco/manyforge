"""Shared gripper-command validation + request building.

Single source of truth used by both the direct ``/api/scene/gripper/command``
route and the Validated Robot Control gripper candidate path (Phase 4-P5.2G),
so the no-motion candidate preview, the approval-time revalidation, and the
legacy direct route all agree on the deployment / config / limit / finite
checks and build the same ``GripperCommandRequest``.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import GripperCommandRequest, GripperCommandState

from .state import ComposerState

# Stable reason codes. The candidate path mirrors these as diagnostics reason
# codes; the direct route maps them onto HTTP status codes.
GRIPPER_REASON_READY = "ready"
GRIPPER_REASON_NO_DEPLOYMENT = "no_deployment_loaded"
GRIPPER_REASON_UNKNOWN_GRIPPER = "gripper_not_configured"
GRIPPER_REASON_BACKEND_UNAVAILABLE = "backend_unavailable"
GRIPPER_REASON_MALFORMED = "malformed_target"
GRIPPER_REASON_LIMIT = "gripper_limit_violation"
GRIPPER_REASON_BUSY = "gripper_busy"


@dataclass(frozen=True)
class GripperValidation:
    ok: bool
    request: Optional[GripperCommandRequest] = None
    reason_code: str = ""
    detail: str = ""
    limit_lower: Optional[float] = None
    limit_upper: Optional[float] = None


def gripper_joint_limits(
    state: ComposerState, joint_name: str
) -> tuple[Optional[float], Optional[float]]:
    """URDF limit pair for a gripper joint (lazy URDF import avoids a cycle)."""
    try:
        from .routes_urdf import get_urdf_entry
    except Exception:  # noqa: BLE001 — limit lookup must never raise
        return (None, None)
    entry = get_urdf_entry(state)
    if entry is None:
        return (None, None)
    for joint in entry.joints:
        if joint.name == joint_name:
            return (joint.limit_lower, joint.limit_upper)
    return (None, None)


def validate_gripper_command(
    state: ComposerState,
    *,
    gripper_id: str,
    target_position: float,
    max_effort: Optional[float],
    duration_s: Optional[float],
) -> GripperValidation:
    """Validate a gripper command WITHOUT dispatching it.

    Returns a built ``GripperCommandRequest`` when valid, or a structured reason
    when not. Pure read: never calls ``kernel.command_gripper`` or mutates
    ``/api/scene/state``. Does NOT check cycle ownership (a dispatch-time
    concern); see ``gripper_cycle_busy_detail``.
    """
    deployment = state.deployment
    if deployment is None:
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_NO_DEPLOYMENT,
            detail="No deployment loaded",
        )
    config = deployment.grippers.get(gripper_id)
    if config is None:
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_UNKNOWN_GRIPPER,
            detail=(
                f"Gripper '{gripper_id}' is not declared in the active deployment"
            ),
        )
    if state.kernel is None:
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_BACKEND_UNAVAILABLE,
            detail="Kernel is not attached",
        )
    tp = float(target_position)
    if not math.isfinite(tp):
        return GripperValidation(
            False, reason_code=GRIPPER_REASON_MALFORMED, detail="targetPosition must be finite"
        )
    me = None if max_effort is None else float(max_effort)
    if me is not None and (not math.isfinite(me) or me < 0.0):
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_MALFORMED,
            detail="maxEffort must be finite and >= 0",
        )
    ds = float(config.motion_time_s) if duration_s is None else float(duration_s)
    if not math.isfinite(ds) or ds < 0.0:
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_MALFORMED,
            detail="durationS must be finite and >= 0",
        )
    limit_lower, limit_upper = gripper_joint_limits(state, config.joint_name)
    if limit_lower is None or limit_upper is None:
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_LIMIT,
            detail=(
                f"Cannot command gripper '{gripper_id}' because joint "
                f"'{config.joint_name}' has no finite URDF limits"
            ),
        )
    if tp < float(limit_lower) or tp > float(limit_upper):
        return GripperValidation(
            False,
            reason_code=GRIPPER_REASON_LIMIT,
            detail=(
                f"targetPosition {tp} is outside limits "
                f"[{float(limit_lower)}, {float(limit_upper)}] for gripper joint "
                f"'{config.joint_name}'"
            ),
            limit_lower=float(limit_lower),
            limit_upper=float(limit_upper),
        )
    request = GripperCommandRequest(
        gripper_id=gripper_id,
        group_id=str(config.group_id),
        joint_name=str(config.joint_name),
        target_position=tp,
        duration_s=ds,
        max_effort=me,
    )
    return GripperValidation(
        True,
        request=request,
        reason_code=GRIPPER_REASON_READY,
        limit_lower=float(limit_lower),
        limit_upper=float(limit_upper),
    )


def gripper_cycle_busy_detail(state: ComposerState) -> Optional[str]:
    """Return a reason string when gripper dispatch must be blocked because
    another authority owns execution (cycle RUNNING / DIRECT_CONTROL), else
    None. Applies only at dispatch time, not to no-motion candidate preview."""
    cycle_manager = state.cycle_manager
    if cycle_manager is None:
        return None
    cycle_state = cycle_manager.state
    if cycle_state in {CycleState.RUNNING, CycleState.DIRECT_CONTROL}:
        return (
            "Gripper commands are allowed only while the cycle is idle or "
            f"stopped; current cycle state is {cycle_state.value}"
        )
    return None


def gripper_busy_detail(state: ComposerState, gripper_id: str) -> Optional[str]:
    """Return a reason string when the named gripper is already EXECUTING a
    command, else None. The "current gripper command state" check: a candidate
    must not be approved/dispatched (nor a fresh one started) while the gripper
    is mid-motion, instead of relying on the kernel to reject after dispatch.
    Defensive: any lookup failure collapses to None (not busy)."""
    kernel = state.kernel
    if kernel is None:
        return None
    # Advance any in-flight simulated gripper command before reading status:
    # outside a BT cycle nothing else ticks it, so a stale `executing` would
    # otherwise falsely block forever. Idempotent wall-clock progression.
    advance = getattr(kernel, "update_gripper_commands", None)
    if callable(advance):
        try:
            advance()
        except Exception:  # noqa: BLE001 — progression must not raise here
            pass
    get_status = getattr(kernel, "get_gripper_status", None)
    if not callable(get_status):
        return None
    try:
        status = get_status(gripper_id)
    except Exception:  # noqa: BLE001 — status lookup must never raise here
        return None
    if getattr(status, "state", None) == GripperCommandState.EXECUTING:
        return (
            f"Gripper '{gripper_id}' is already executing a command; wait for it "
            "to finish before commanding a new target"
        )
    return None
