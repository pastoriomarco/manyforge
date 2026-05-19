"""Kernel routes - /api/kernel/*"""
from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter, HTTPException, Query

from .models import KernelSnapshotResponse
from .state import ComposerState

logger = logging.getLogger(__name__)


def build_kernel_snapshot_payload(state: ComposerState) -> dict[str, Any]:
    """Build the current kernel execution snapshot payload."""
    now_ns = time.monotonic_ns()
    if state.kernel is None:
        return {
            "state": "idle",
            "execution_id": 0,
            "active_move_id": None,
            "progress": 0.0,
            "joint_positions_rad": None,
            "error": None,
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

    return KernelSnapshotResponse(
        state=status.state.value,
        execution_id=status.execution_id,
        active_move_id=status.active_move_id if status.active_move_id else None,
        progress=status.progress,
        joint_positions_rad=joint_positions,
        error=snapshot.error,
        last_transition_ns=None,
        timestamp_ns=snapshot.timestamp_ns,
    ).model_dump()


def build_kernel_metrics_payload(state: ComposerState) -> dict[str, int]:
    """Build kernel metrics payload."""
    if state.kernel is None:
        raise HTTPException(status_code=500, detail="Kernel not available")
    return state.kernel.get_metrics()


def create_kernel_router(state: ComposerState) -> APIRouter:
    """Create the kernel router with the given state."""
    router = APIRouter()

    @router.get("/snapshot")
    def get_kernel_snapshot() -> dict[str, Any]:
        """Get current kernel execution snapshot."""
        return build_kernel_snapshot_payload(state)

    @router.get("/metrics")
    def get_kernel_metrics() -> dict[str, int]:
        """Get kernel performance metrics."""
        return build_kernel_metrics_payload(state)

    @router.get("/events")
    def get_kernel_events(
        since: int = Query(default=0, ge=0),
    ) -> dict[str, Any]:
        """Get native kernel events since a given event index.

        Poll this endpoint to receive real-time kernel events (state
        transitions, plan/execute lifecycle, cache hits, collisions).
        Pass the last received event_index as `since` to get only new events.
        """
        if state.cycle_manager is None:
            return {
                "events": [],
                "last_event_index": 0,
                "timestamp_ns": time.monotonic_ns(),
            }
        state.cycle_manager.drain_kernel_events_now()
        events = state.cycle_manager.get_kernel_events_since(since)
        last_index = events[-1]["event_index"] if events else since
        return {
            "events": events,
            "last_event_index": last_index,
            "timestamp_ns": time.monotonic_ns(),
        }

    return router
