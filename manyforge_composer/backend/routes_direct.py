"""Direct-control / intervention routes - /api/direct/*"""
from __future__ import annotations

import logging
import time
from functools import wraps
from typing import Any, Callable

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
from manyforge_behavior.program import collect_tree_validation_issues, tree_node_from_dict

from .intervention_validation import merged_intervention_blackboard_keys
from .models import (
    CartesianJogRequest,
    DelegatedRecoveryRequestResponse,
    InterventionExecuteRequest,
    InterventionPromoteRequest,
    InterventionSessionResponse,
    JointJogRequest,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


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


def _session_response(session, status_code: int = 200) -> JSONResponse:
    """Build a JSON response from an InterventionSession."""
    return JSONResponse(
        status_code=status_code,
        content=InterventionSessionResponse(
            session_id=session.session_id,
            session_kind=session.session_kind,
            authority_source=session.authority_source,
            principal_id=session.principal_id,
            grant_type=session.grant_type,
            parent_run_id=session.parent_run_id,
            entry_state=session.entry_state,
            lease_timeout_ms=session.lease_timeout_ms,
            lease_expires_at_ns=session.lease_expires_at_ns,
            resume_target=session.resume_target,
            trigger_reason=session.trigger_reason,
            trigger_node_instance=session.trigger_node_instance,
            approval_mode=session.approval_mode,
            artifact_origin=session.artifact_origin,
            status=session.status,
            actual_outcome=session.actual_outcome,
            error=session.error,
            started_at_ns=session.started_at_ns,
            finished_at_ns=session.finished_at_ns,
            events=list(session.events),
            promotions=list(session.promotions),
            timestamp_ns=time.monotonic_ns(),
        ).model_dump(by_alias=True),
    )


def _motion_arm_admission_error(state: ComposerState) -> JSONResponse | None:
    stop_block = state.motion_arm_admission_block_reason()
    if not stop_block:
        return None
    return JSONResponse(
        status_code=409,
        content={
            "error": stop_block["message"],
            "error_code": stop_block["error_code"],
            "error_details": stop_block.get("error_details"),
            "timestamp_ns": time.monotonic_ns(),
        },
    )


def _delegated_recovery_response(request, status_code: int = 200) -> JSONResponse:
    return JSONResponse(
        status_code=status_code,
        content=DelegatedRecoveryRequestResponse(
            request_id=request.request_id,
            parent_run_id=request.parent_run_id,
            requested_from_state=request.requested_from_state,
            trigger_node_instance=request.trigger_node_instance,
            trigger_node_id=request.trigger_node_id,
            trigger_reason=request.trigger_reason,
            delegate_policy=request.delegate_policy,
            allowed_authority_sources=list(request.allowed_authority_sources),
            allowed_actions=list(request.allowed_actions),
            allowed_resume_targets=list(request.allowed_resume_targets),
            default_resume_target=request.default_resume_target,
            runtime_override_policy=request.runtime_override_policy,
            persistent_edit_policy=request.persistent_edit_policy,
            requested_at_ns=request.requested_at_ns,
            timestamp_ns=time.monotonic_ns(),
        ).model_dump(by_alias=True),
    )


def create_direct_router(state: ComposerState) -> APIRouter:
    """Create the direct-control router with the given state."""
    router = APIRouter()

    def artifact_transactional(
        handler: Callable[..., Any],
    ) -> Callable[..., Any]:
        """Keep active artifacts stable through synchronous dispatch acceptance."""

        @wraps(handler)
        def wrapped(*args, **kwargs):
            with state.artifact_read_transaction():
                return handler(*args, **kwargs)

        return wrapped

    @router.post("/execute")
    @artifact_transactional
    def execute_intervention(request: InterventionExecuteRequest, http_request: Request):
        """Execute a temporary subtree as a direct-control intervention.

        Requires the cycle to be IDLE or STOPPED. The subtree runs on a
        dedicated tick thread; the manager transitions to DIRECT_CONTROL
        while it executes and then restores the pre-intervention session state
        or the explicit resume target.
        """
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Use server-authoritative principal from auth middleware when available.
        # Falls back to body-supplied principal_id when auth is disabled (dev).
        auth_principal = getattr(http_request.state, "principal_id", None)
        effective_principal = auth_principal or request.principal_id

        tree_dict = request.tree.model_dump()
        try:
            tree = tree_node_from_dict(tree_dict)
        except Exception as exc:
            return JSONResponse(
                status_code=400,
                content={
                    "error": f"Invalid intervention tree: {exc}",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        errors, _warnings = collect_tree_validation_issues(
            tree,
            state.node_catalog,
            parameter_defaults=(
                state.program.get_parameter_defaults()
                if state.program is not None
                else None
            ),
            blackboard_namespaces=(
                state.program.blackboard_namespaces
                if state.program is not None
                else None
            ),
            blackboard_keys=merged_intervention_blackboard_keys(
                tree,
                state.node_catalog,
                base_keys=(
                    state.program.blackboard_keys
                    if state.program is not None
                    else None
                ),
            ),
            skill_catalog=state.skill_catalog,
            program_profile=(
                state.program.program_profile
                if state.program is not None
                else None
            ),
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        if errors:
            return JSONResponse(
                status_code=400,
                content={
                    "error": errors[0].message,
                    "details": [{"path": issue.path, "message": issue.message} for issue in errors],
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        admission_error = _motion_arm_admission_error(state)
        if admission_error is not None:
            return admission_error

        try:
            session = state.cycle_manager.execute_intervention(
                tree_dict,
                parent_run_id=_parent_run_id_for_intervention(state),
                lease_timeout_ms=request.lease_timeout_ms,
                resume_target=request.resume_target,
                session_kind=request.session_kind,
                authority_source=request.authority_source,
                principal_id=effective_principal,
                grant_type=request.grant_type,
                trigger_reason=request.trigger_reason,
                trigger_node_instance=request.trigger_node_instance,
                approval_mode=request.approval_mode,
                artifact_origin=request.artifact_origin,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _session_response(session)

    @router.post("/joint-jog")
    @artifact_transactional
    def joint_jog(request: JointJogRequest, http_request: Request):
        """Drive one or more joints to a target position from the viewer's
        robot-control overlay panel (Day 5b).

        The operator's slider commit posts a partial joint update (just
        the joints they touched); the backend reads the current full
        joint vector from the kernel and overlays the requested
        positions before submitting a single-leaf intervention tree
        with `move_manipulator_action` set to `motion_type=joint`.
        This reuses the existing intervention machinery so the cycle
        manager transitions to DIRECT_CONTROL, the planner produces a
        bounded trajectory, the kernel executes it, and the cycle
        returns to IDLE — same lifecycle as `/api/direct/execute` with
        no new kernel-side API.

        The 409 / 400 / 200 branches mirror /api/direct/execute so
        operators see consistent error semantics across the direct-
        control surface.
        """
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        if state.kernel is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No kernel available",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        if len(request.joint_names) != len(request.joint_positions_rad):
            return JSONResponse(
                status_code=400,
                content={
                    "error": "jointNames and jointPositionsRad must have the same length",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Read current joint vector. We need a full goal across every joint the
        # kernel exposes, not just the ones the operator touched, otherwise the
        # planner produces a trajectory that tries to zero out unspecified
        # joints. ComposerState applies the deployment's authority profile:
        # authored lanes may use their backend cache/seed, while physical
        # observed lanes require a complete fresh measured snapshot.
        kernel_joint_names = list(state.kernel.get_joint_names())
        if not kernel_joint_names:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "Kernel reports no joints; cannot jog",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        unknown_joint = next(
            (name for name in request.joint_names if name not in kernel_joint_names),
            None,
        )
        if unknown_joint is not None:
            return JSONResponse(
                status_code=400,
                content={
                    "error": f"Unknown joint '{unknown_joint}'",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Surface lifecycle/stop-capability interlocks before position
        # availability. They are the stronger operator action required and stay
        # stable even when the observed joint stream is unavailable.
        admission_error = _motion_arm_admission_error(state)
        if admission_error is not None:
            return admission_error

        joint_state_authority = state.joint_state_authority_profile()
        current_positions = state.get_current_joint_positions(
            joint_names=kernel_joint_names
        )
        if current_positions is None:
            if joint_state_authority.measurement_authoritative:
                detail = (
                    "No complete fresh measured robot joint state is available; "
                    "cannot jog from a cached, initial, or commanded pose"
                )
            else:
                detail = (
                    "Kernel has no joint state yet — load a deployment so the "
                    "initial joint configuration is available"
                )
            return JSONResponse(
                status_code=409,
                content={
                    "error": detail,
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Apply the partial update from the request onto the full
        # current-positions vector.
        target_positions = list(current_positions)
        for name, pos in zip(request.joint_names, request.joint_positions_rad):
            idx = kernel_joint_names.index(name)
            target_positions[idx] = float(pos)

        auth_principal = getattr(http_request.state, "principal_id", None)
        effective_principal = auth_principal or request.principal_id

        # One-leaf intervention tree. move_manipulator_action is a
        # `leaf` in the catalog so it can be the root of the
        # intervention tree directly. In ManyForge's BT tree
        # convention, `id` is the catalog-entry id (kind) the
        # validator looks up via `catalog.get(node.id)`, and `name`
        # is the per-instance identifier used for trajectory
        # caching and event traces.
        instance_name = f"joint_jog_{time.monotonic_ns()}"
        tree_dict = {
            "id": "move_manipulator_action",
            "name": instance_name,
            "params": {
                "move_id": instance_name,
                "motion_type": "joint",
                "joint_goal_names": kernel_joint_names,
                "joint_goal_positions_rad": target_positions,
            },
            "children": [],
        }

        try:
            tree = tree_node_from_dict(tree_dict)
        except Exception as exc:
            return JSONResponse(
                status_code=400,
                content={
                    "error": f"Failed to build joint-jog intervention: {exc}",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        errors, _warnings = collect_tree_validation_issues(
            tree,
            state.node_catalog,
            parameter_defaults=(
                state.program.get_parameter_defaults()
                if state.program is not None
                else None
            ),
            blackboard_namespaces=(
                state.program.blackboard_namespaces
                if state.program is not None
                else None
            ),
            blackboard_keys=merged_intervention_blackboard_keys(
                tree,
                state.node_catalog,
                base_keys=(
                    state.program.blackboard_keys
                    if state.program is not None
                    else None
                ),
            ),
            skill_catalog=state.skill_catalog,
            program_profile=(
                state.program.program_profile
                if state.program is not None
                else None
            ),
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        if errors:
            return JSONResponse(
                status_code=400,
                content={
                    "error": errors[0].message,
                    "details": [
                        {"path": issue.path, "message": issue.message}
                        for issue in errors
                    ],
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.execute_intervention(
                tree_dict,
                parent_run_id=_parent_run_id_for_intervention(state),
                lease_timeout_ms=request.lease_timeout_ms,
                # Day 5c.1 fix: explicit "idle" resume target so the
                # cycle returns to IDLE after the intervention. With
                # the prior `None` value the cycle ended up in STOPPED
                # and the next slider commit couldn't fire until the
                # operator manually reset.
                resume_target="idle",
                session_kind="manual_intervention",
                authority_source="human",
                principal_id=effective_principal,
                grant_type="human_approved",
                trigger_reason="composer_viewer_joint_jog",
                trigger_node_instance=None,
                approval_mode="human_approved",
                artifact_origin="runtime_override",
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        # Authored lanes may optimistically project command intent while their
        # backend executes it. On a physical/observed lane the measured robot is
        # the only position authority, so never overwrite its observer cache.
        if not joint_state_authority.measurement_authoritative:
            state.record_current_joint_positions(list(target_positions))

        return _session_response(session)

    @router.post("/cartesian-jog")
    @artifact_transactional
    def cartesian_jog(request: CartesianJogRequest, http_request: Request):
        """Drive the TCP to a target pose from the viewer's Cartesian
        jog gizmo (Day 5c.2).

        Same intervention-machinery wrapping as /api/direct/joint-jog
        but with motion_type=pose. The frontend sends the
        target TCP pose already expressed in the robot's base frame
        (the same convention the catalog's `pose_goal` parameter
        speaks). The cycle returns to IDLE on success, enabling
        rapid sequential gizmo commits.
        """
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        if state.kernel is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No kernel available",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        auth_principal = getattr(http_request.state, "principal_id", None)
        effective_principal = auth_principal or request.principal_id

        instance_name = f"cartesian_jog_{time.monotonic_ns()}"
        robot = state.program.robot
        requested_frame_id = (request.frame_id or "").strip()
        requested_tcp_frame = (request.tcp_frame or "").strip()
        base_frame = requested_frame_id or robot.base_frame
        tcp_frame = requested_tcp_frame or robot.tcp_frame
        tree_dict = {
            "id": "move_manipulator_action",
            "name": instance_name,
            "params": {
                "move_id": instance_name,
                "motion_type": "pose",
                "pose_goal": {
                    "frame_id": base_frame,
                    "position_m": [float(v) for v in request.position_m],
                    "orientation_xyzw": [float(v) for v in request.orientation_xyzw],
                },
                "base_frame": base_frame,
                "tcp_frame": tcp_frame,
            },
            "children": [],
        }

        try:
            tree = tree_node_from_dict(tree_dict)
        except Exception as exc:
            return JSONResponse(
                status_code=400,
                content={
                    "error": f"Failed to build cartesian-jog intervention: {exc}",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        errors, _warnings = collect_tree_validation_issues(
            tree,
            state.node_catalog,
            parameter_defaults=(
                state.program.get_parameter_defaults()
                if state.program is not None
                else None
            ),
            blackboard_namespaces=(
                state.program.blackboard_namespaces
                if state.program is not None
                else None
            ),
            blackboard_keys=merged_intervention_blackboard_keys(
                tree,
                state.node_catalog,
                base_keys=(
                    state.program.blackboard_keys
                    if state.program is not None
                    else None
                ),
            ),
            skill_catalog=state.skill_catalog,
            program_profile=(
                state.program.program_profile
                if state.program is not None
                else None
            ),
            deployment_capability_keys=_deployment_capability_keys(state),
        )
        if errors:
            return JSONResponse(
                status_code=400,
                content={
                    "error": errors[0].message,
                    "details": [
                        {"path": issue.path, "message": issue.message}
                        for issue in errors
                    ],
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        admission_error = _motion_arm_admission_error(state)
        if admission_error is not None:
            return admission_error

        try:
            session = state.cycle_manager.execute_intervention(
                tree_dict,
                parent_run_id=_parent_run_id_for_intervention(state),
                lease_timeout_ms=request.lease_timeout_ms,
                resume_target="idle",
                session_kind="manual_intervention",
                authority_source="human",
                principal_id=effective_principal,
                grant_type="human_approved",
                trigger_reason="composer_viewer_cartesian_jog",
                trigger_node_instance=None,
                approval_mode="human_approved",
                artifact_origin="runtime_override",
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _session_response(session)

    @router.post("/heartbeat")
    def renew_intervention_lease(lease_timeout_ms: int | None = None):
        """Renew the active intervention lease, optionally changing its duration."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.renew_intervention_lease(
                lease_timeout_ms=lease_timeout_ms,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        return _session_response(session)

    @router.post("/promote")
    def promote_intervention_blackboard(request: InterventionPromoteRequest):
        """Promote scoped intervention blackboard values into parent keys."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        try:
            session = state.cycle_manager.promote_intervention_values(
                request.promote_keys,
                session_id=request.session_id,
            )
        except RuntimeError as exc:
            return JSONResponse(
                status_code=409,
                content={
                    "error": str(exc),
                    "timestamp_ns": time.monotonic_ns(),
                },
            )
        return _session_response(session)

    @router.get("/status")
    def get_intervention_status():
        """Get the current or most recent intervention session status."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        session = state.cycle_manager.intervention
        if session is None:
            return JSONResponse(
                status_code=404,
                content={
                    "error": "No intervention session",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _session_response(session)

    @router.get("/pending-recovery")
    def get_pending_delegated_recovery():
        """Get the currently pending delegated recovery request, if any."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        request = state.cycle_manager.pending_delegated_recovery
        if request is None:
            return JSONResponse(
                status_code=404,
                content={
                    "error": "No delegated recovery request",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        return _delegated_recovery_response(request)

    @router.post("/abort")
    def abort_intervention():
        """Abort a running intervention and restore the pre-intervention state."""
        if state.cycle_manager is None:
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No program loaded",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        session = state.cycle_manager.intervention
        if session is None or session.status != "running":
            return JSONResponse(
                status_code=409,
                content={
                    "error": "No running intervention to abort",
                    "timestamp_ns": time.monotonic_ns(),
                },
            )

        state.cycle_manager.abort_intervention()

        session = state.cycle_manager.intervention
        return _session_response(session)

    return router
