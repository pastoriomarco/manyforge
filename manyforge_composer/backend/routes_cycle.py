"""Cycle control routes - /api/cycle/*"""
from __future__ import annotations

import logging
from typing import Any, Optional

from fastapi import APIRouter, HTTPException, Request
from manyforge_behavior.cycle_recovery import RUN_AUTOMATIC_RECOVERY_ACTION_IDS
from .models import (
    CycleCommandErrorResponse,
    CycleStatusResponse,
    CycleCommandResponse,
    CycleResetRequest,
    CycleRecoveryActionRequestDTO,
    CycleRecoveryActionResultDTO,
    VelocityScaleRequest,
    VelocityScaleResponse,
)
from .cycle_diagnostics import active_cycle_error_fields, manager_cycle_error_fields
from .route_execution import (
    run_cycle_control as _run_cycle_control,
    run_cycle_lifecycle as _run_cycle_lifecycle,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


def _structured_command_error(
    state: ComposerState,
    message: str,
    *,
    error_code: Optional[str] = None,
    error_details: Optional[dict[str, Any]] = None,
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
    details: Optional[dict[str, Any]] = error_details
    manager = state.cycle_manager
    if manager is not None:
        _, manager_code, manager_details = active_cycle_error_fields(manager)
        code = code or manager_code
        if details is None:
            details = manager_details
    return CycleCommandErrorResponse(
        message=message,
        error_code=code,
        error_details=details,
        recovery_offer=(manager.get_recovery_offer() if manager is not None else None),
    ).model_dump(by_alias=True)


def _require_no_blocking_operator_alarms(state: ComposerState) -> None:
    """Apply the fail-closed alarm-attention gate without replacing interlocks."""

    block = state.operator_alarm_manager.attention_block_reason()
    if block is None:
        return
    raise HTTPException(
        status_code=409,
        detail=_structured_command_error(
            state,
            str(block["message"]),
            error_code=str(block["errorCode"]),
            error_details=dict(block.get("details") or {}),
        ),
    )


def _refresh_visual_scene_after_reset(state: ComposerState) -> None:
    """Drop stale observer state and republish the post-reset scene."""
    state.clear_pose_source_snapshots()
    state.clear_visual_state_snapshot()
    state.clear_visual_scene_objects_snapshot()
    refresh_result = state.refresh_visual_scene_objects_sync(reason="cycle_reset")
    if not refresh_result.get("success", False):
        logger.warning(
            "cycle reset visual scene refresh failed: %s",
            refresh_result.get("error"),
        )
    state.request_visual_state_publish(reason="cycle_reset")


def _cycle_command_payload(manager: Any) -> dict[str, Any]:
    error, error_code, error_details = manager_cycle_error_fields(manager)
    return {
        "state": manager.state.value,
        "motionStopState": manager.motion_stop_state,
        "error": error,
        "errorCode": error_code,
        "errorDetails": error_details,
        "recoveryOffer": manager.get_recovery_offer(),
    }


def _perform_cycle_reset(
    state: ComposerState,
    *,
    request_id: Optional[str],
    scene_initialization_token: Optional[str] = None,
) -> dict[str, Any]:
    if state.cycle_manager is None:
        raise RuntimeError("No program loaded")
    pending_token = state.prepare_scene_initialization_reset(
        scene_initialization_token,
        reason="cycle_reset",
    )
    try:
        override_status = state.runtime_override_status()
        prev = state.cycle_manager.state.value
        if override_status["has_staged"]:
            state.activate_staged_runtime_overrides()
        else:
            state.clear_pose_source_snapshots()
            with state.scene_edit_lock:
                state.cycle_manager.reset(request_id=request_id)
        state.reset_backend_authoritative_sim_if_needed(reason="cycle_reset")
        _refresh_visual_scene_after_reset(state)
        if pending_token is not None:
            state.mark_scene_initialization_seeded(pending_token)
            state.complete_scene_initialization(
                pending_token,
                reason="cycle_reset",
            )
        logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
        result = _cycle_command_payload(state.cycle_manager)
        result["sceneInitialization"] = state.scene_initialization_status()
        return result
    except Exception as exc:
        state.fail_scene_initialization(
            pending_token,
            error=str(exc) or type(exc).__name__,
        )
        raise


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
                "motionStopState": "idle",
                "velocityScale": 1.0,
                "error": None,
                "errorCode": None,
                "errorDetails": None,
                "recoveryOffer": None,
            }
        manager = state.cycle_manager
        manager.drain_kernel_events_now()
        error, error_code, error_details = active_cycle_error_fields(manager)
        return {
            "state": manager.state.value,
            "motionStopState": getattr(manager, "motion_stop_state", "idle"),
            "velocityScale": manager.velocity_scale,
            "error": error,
            "errorCode": error_code,
            "errorDetails": error_details,
            "recoveryOffer": manager.get_recovery_offer(),
        }

    @router.post("/run", response_model=CycleCommandResponse, response_model_by_alias=True)
    async def run_cycle(request: Request) -> dict:
        """Start the cycle (IDLE→RUNNING or STOPPED→RUNNING)."""

        def command_locked() -> dict:
            if state.cycle_manager is None:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state, "No program loaded", error_code="no_program_loaded"
                    ),
                )
            _require_no_blocking_operator_alarms(state)
            # Fail closed: never arm a program whose configured operator/fault stop
            # profile names a stop mode the execution backend does not advertise.
            # Running it would mean an operator Stop silently degrades to a
            # different stop behavior than the deployment declared. The operator
            # must select a supported profile first (the runtime status already
            # surfaces this as canExecute=false / degraded).
            stop_block = state.motion_arm_admission_block_reason()
            if stop_block:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state,
                        stop_block["message"],
                        error_code=stop_block["error_code"],
                        error_details=stop_block.get("error_details"),
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
                state.cycle_manager.run(
                    principal_id=(
                        getattr(request.state, "principal_id", None)
                        or "operator_ui"
                    )
                )
            except RuntimeError as exc:
                # Cycle-manager recovery refusals (e.g. a latched
                # INTERVENTION_REQUIRED that needs an explicit operator choice)
                # surface as RuntimeError. Translate to a structured 409 so the
                # viewer can render the kernel's actual cause from the
                # cycle-manager's preserved error_code / error_details instead of
                # a generic banner.
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(state, str(exc)),
                )
            logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
            return _cycle_command_payload(state.cycle_manager)

        def command() -> dict:
            try:
                with state.runtime_admission_guard():
                    return command_locked()
            except RuntimeError as exc:
                if str(exc) != "node_environment_reload_in_progress":
                    raise
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state,
                        "Run is temporarily unavailable while the node catalog reloads",
                        error_code="node_environment_reload_in_progress",
                    ),
                ) from exc

        return await _run_cycle_lifecycle(command)

    @router.post("/stop", response_model=CycleCommandResponse, response_model_by_alias=True)
    async def stop_cycle() -> dict:
        """Stop active robot motion while eligible tree branches keep ticking."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )

        def command() -> dict:
            assert state.cycle_manager is not None
            prev = state.cycle_manager.state.value
            state.cycle_manager.stop()
            logger.info("Cycle state: %s → %s", prev, state.cycle_manager.state.value)
            return _cycle_command_payload(state.cycle_manager)

        return await _run_cycle_control(command)

    @router.post("/full-stop", response_model=CycleCommandResponse, response_model_by_alias=True)
    async def full_stop_cycle() -> dict:
        """Quiesce the whole cycle (RUNNING→STOPPING→STOPPED)."""
        if state.cycle_manager is None:
            raise HTTPException(
                status_code=409,
                detail=_structured_command_error(
                    state, "No program loaded", error_code="no_program_loaded"
                ),
            )

        def command() -> dict:
            assert state.cycle_manager is not None
            prev = state.cycle_manager.state.value
            state.cycle_manager.full_stop()
            logger.info(
                "Cycle full-stop state: %s → %s",
                prev,
                state.cycle_manager.state.value,
            )
            return _cycle_command_payload(state.cycle_manager)

        return await _run_cycle_control(command)

    @router.post("/reset", response_model=CycleCommandResponse, response_model_by_alias=True)
    async def reset_cycle(
        request: Request,
        payload: Optional[CycleResetRequest] = None,
    ) -> dict:
        """Reset the cycle (any→IDLE)."""
        request_id = getattr(request.state, "request_id", None)

        def command() -> dict:
            if state.cycle_manager is None:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state, "No program loaded", error_code="no_program_loaded"
                    ),
                )
            try:
                return _perform_cycle_reset(
                    state,
                    request_id=request_id if isinstance(request_id, str) else None,
                    scene_initialization_token=(
                        payload.scene_initialization_token
                        if payload is not None
                        else None
                    ),
                )
            except RuntimeError as exc:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(state, str(exc)),
                )

        return await _run_cycle_lifecycle(command)

    @router.post(
        "/recovery-actions",
        response_model=CycleRecoveryActionResultDTO,
        response_model_by_alias=True,
    )
    async def apply_recovery_action(
        payload: CycleRecoveryActionRequestDTO,
        request: Request,
    ) -> dict:
        """Apply one exact immutable recovery offer action."""

        def command() -> dict:
            if state.cycle_manager is None:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(
                        state, "No program loaded", error_code="no_program_loaded"
                    ),
                )
            principal_id = (
                getattr(request.state, "principal_id", None) or "operator_ui"
            )
            if payload.action_id in RUN_AUTOMATIC_RECOVERY_ACTION_IDS:
                _require_no_blocking_operator_alarms(state)
            try:
                return state.cycle_manager.apply_recovery_action(
                    payload.model_dump(),
                    principal_id=principal_id,
                    reset_callback=lambda recovery_request_id: _perform_cycle_reset(
                        state,
                        request_id=recovery_request_id,
                    ),
                )
            except RuntimeError as exc:
                raise HTTPException(
                    status_code=409,
                    detail=_structured_command_error(state, str(exc)),
                )

        return await _run_cycle_lifecycle(command)

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
