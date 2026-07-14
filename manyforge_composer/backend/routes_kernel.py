"""Kernel routes - /api/kernel/*"""
from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter, HTTPException, Query

from .models import KernelSnapshotResponse
from .route_execution import run_kernel_stop_backstop, run_route_background
from .state import ComposerState

logger = logging.getLogger(__name__)


def build_kernel_snapshot_payload(state: ComposerState) -> dict[str, Any]:
    """Build the current kernel execution snapshot payload.

    The error surface exposes both the human-readable `error` message and
    the structured `error_code` / `error_details` map the kernel produces
    via its ErrorDTO. The viewer uses the structured fields to render a
    cause-specific panel (colliding pair, classification, waypoint index,
    fault reason code, etc.) instead of a generic banner.
    """
    now_ns = time.monotonic_ns()
    if state.kernel is None:
        return {
            "state": "idle",
            "execution_id": 0,
            "active_move_id": None,
            "progress": 0.0,
            "joint_positions_rad": None,
            "state_source": None,
            "source_timestamp_ns": None,
            "source_age_ns": None,
            "error": None,
            "error_code": None,
            "error_details": None,
            "last_transition_ns": None,
            "timestamp_ns": now_ns,
        }

    snapshot = (
        state.cycle_manager.get_execution_snapshot()
        if state.cycle_manager is not None
        else state.kernel.get_execution_snapshot()
    )
    status = (
        state.cycle_manager.get_status()
        if state.cycle_manager is not None
        else state.kernel.get_status()
    )

    joint_positions = snapshot.joint_positions_rad if snapshot.joint_positions_rad else None
    snapshot_error_details = getattr(snapshot, "error_details", None)
    # The kernel client populates `error_details` as a dict[str, str]; an
    # empty dict has no diagnostic value to the viewer, so collapse it to None
    # so the frontend can fall back to the headline message cleanly.
    if not snapshot_error_details:
        snapshot_error_details = None

    return KernelSnapshotResponse(
        state=status.state.value,
        execution_id=status.execution_id,
        active_move_id=status.active_move_id if status.active_move_id else None,
        progress=status.progress,
        joint_positions_rad=joint_positions,
        state_source=getattr(snapshot, "state_source", "") or None,
        source_timestamp_ns=getattr(snapshot, "source_timestamp_ns", None),
        source_age_ns=getattr(snapshot, "source_age_ns", None),
        error=snapshot.error,
        error_code=getattr(snapshot, "error_code", None),
        error_details=snapshot_error_details,
        last_transition_ns=None,
        timestamp_ns=snapshot.timestamp_ns,
    ).model_dump()


def build_kernel_metrics_payload(state: ComposerState) -> dict[str, int | str]:
    """Build kernel metrics payload."""
    if state.kernel is None:
        raise HTTPException(status_code=500, detail="Kernel not available")
    return state.kernel.get_metrics()


def create_kernel_router(state: ComposerState) -> APIRouter:
    """Create the kernel router with the given state."""
    router = APIRouter()

    @router.get("/snapshot")
    async def get_kernel_snapshot() -> dict[str, Any]:
        """Get current kernel execution snapshot."""
        return await run_route_background(lambda: build_kernel_snapshot_payload(state))

    @router.get("/metrics")
    async def get_kernel_metrics() -> dict[str, int | str]:
        """Get kernel performance metrics."""
        return await run_route_background(lambda: build_kernel_metrics_payload(state))

    @router.post("/stop")
    async def kernel_stop() -> dict[str, Any]:
        """Issue a kernel-level controlled stop, bypassing the cycle layer.

        Operator backstop: works regardless of what the cycle manager
        believes its state is, so a cycle-layer fault can never leave the
        operator without a working stop. The kernel routes the request
        through its operator-stop profile (controlled stop with hard-stop
        escalation). A rejection is reported honestly — a kernel already in
        a terminal state has nothing to stop.
        """
        def coalesced() -> dict[str, Any]:
            return {
                "accepted": True,
                "error": None,
                "reason_code": "kernel_stop_backstop_already_in_flight",
                "kernel_state": None,
                "coalesced": True,
                "timestamp_ns": time.monotonic_ns(),
            }

        def command() -> dict[str, Any]:
            if state.kernel is None:
                raise HTTPException(status_code=503, detail="Kernel not available")
            result = state.kernel.stop()
            try:
                kernel_state = state.kernel.get_status().state.value
            except Exception:  # noqa: BLE001 — report the stop result even if status is unreadable
                kernel_state = None
            return {
                "accepted": result.accepted,
                "error": result.error,
                "reason_code": result.reason_code,
                "kernel_state": kernel_state,
                "coalesced": False,
                "timestamp_ns": time.monotonic_ns(),
            }

        return await run_kernel_stop_backstop(command, coalesced=coalesced)

    @router.get("/events")
    async def get_kernel_events(
        since: int = Query(default=0, ge=0),
    ) -> dict[str, Any]:
        """Get native kernel events since a given event index.

        Poll this endpoint to receive real-time kernel events (state
        transitions, plan/execute lifecycle, cache hits, collisions).
        Pass the last received event_index as `since` to get only new events.
        """
        def command() -> dict[str, Any]:
            manager = state.cycle_manager
            if manager is None:
                return {
                    "events": [],
                    "last_event_index": 0,
                    "timestamp_ns": time.monotonic_ns(),
                }
            manager.drain_kernel_events_now()
            events = manager.get_kernel_events_since(since)
            if hasattr(state, "event_recorder"):
                # Loss counters can advance even when this consumer's cursor
                # yields no events (another drain may already have consumed the
                # retained batch). The empty call is therefore load-bearing:
                # it lets the recorder make otherwise invisible loss durable.
                state.event_recorder.record_native_kernel_events(
                    events,
                    pipeline_status=(
                        manager.kernel_event_pipeline_status()
                    ),
                    source_identity=id(manager),
                )
            last_index = events[-1]["event_index"] if events else since
            return {
                "events": events,
                "last_event_index": last_index,
                "timestamp_ns": time.monotonic_ns(),
            }

        return await run_route_background(command)

    return router
