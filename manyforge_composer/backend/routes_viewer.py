"""Demand-scoped native viewer data-plane routes."""
from __future__ import annotations

import asyncio
import json
import time
import uuid
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from pydantic import ValidationError

from .models import (
    ViewerClientDiagnosticsControlV1,
    ViewerDiagnosticsV1,
    ViewerLeaseAckV1,
    ViewerLeaseControlV1,
    ViewerResyncControlV1,
)
from .routes_scene import ensure_viewer_state_publisher
from .security import (
    SecurityConfig,
    check_ws_bearer_token,
)
from .state import ComposerState
from .viewer_data_plane import BoundedViewerFanout

_DEFAULT_LAYERS = ["canonical", "diagnostics"]
_VIEWER_CONTROL_MAX_MESSAGE_SIZE = 4096


async def _watch_viewer_lease(
    state: ComposerState,
    websocket: WebSocket,
    client_id: str,
) -> None:
    """Close a connected socket once its renewable demand lease expires."""

    while True:
        try:
            grant = state.viewer_demand_grant(client_id)
        except KeyError:
            await websocket.close(code=1001, reason="Viewer lease expired")
            return
        remaining_s = (grant.expires_at_ns - time.monotonic_ns()) / 1e9
        await asyncio.sleep(max(0.001, remaining_s))


def create_viewer_router(
    state: ComposerState,
    security: SecurityConfig,
) -> APIRouter:
    router = APIRouter()
    fanout = BoundedViewerFanout()
    state.configure_visual_state_publication_sink(fanout.publish_threadsafe)

    async def publish_server_diagnostics(client_id: str) -> None:
        status = state.viewer_data_plane_status()
        fanout_status = await fanout.status()
        worker = status.get("worker", {})
        await fanout.publish_diagnostics(
            client_id,
            ViewerDiagnosticsV1(
                type="diagnostics",
                schema_version="manyforge.viewer.diagnostics.v1",
                producer_epoch=str(status.get("producer_epoch") or "unavailable"),
                timestamp_ns=time.monotonic_ns(),
                worker_epoch=worker.get("worker_epoch"),
                producer_healthy=bool(status.get("producer_healthy", False)),
                producer_health_reason=status.get("producer_health_reason"),
                publisher_running=bool(status.get("publisher_running", False)),
                worker_alive=bool(worker.get("alive", False)),
                last_publication_age_ms=status.get("last_publication_age_ms"),
                worker_restarts=int(worker.get("restarts") or 0),
                worker_errors=int(worker.get("failures") or 0),
                demand_client_count=int(status.get("client_count") or 0),
                active_client_count=int(status.get("active_client_count") or 0),
                build_stage_durations_ms=dict(
                    status.get("last_stage_durations_ms") or {}
                ),
                input_replacements=int(worker.get("queue_saturation") or 0),
                fanout_replacements=(
                    int(fanout_status.get("ingress_replacements") or 0)
                    + int(fanout_status.get("replaced_frames_total") or 0)
                ),
                fanout_errors=(
                    int(fanout_status.get("ingress_schedule_failures") or 0)
                    + int(fanout_status.get("send_errors_total") or 0)
                    + int(fanout_status.get("send_timeouts_total") or 0)
                ),
                last_error=worker.get("last_error"),
            ).model_dump(mode="json"),
        )

    @router.get("/api/scene/viewer/status")
    async def viewer_status() -> dict[str, Any]:
        return {
            **state.viewer_data_plane_status(),
            "fanout": await fanout.status(),
        }

    @router.websocket("/ws/scene/viewer")
    async def ws_viewer(websocket: WebSocket) -> None:
        if security.api_token and not check_ws_bearer_token(
            websocket,
            security.api_token,
        ):
            await websocket.close(code=4401, reason="Unauthorized")
            return

        client_id = uuid.uuid4().hex
        await websocket.accept()
        await fanout.attach(websocket, client_id=client_id)
        ensure_viewer_state_publisher(state, reason="viewer_websocket_attach")
        state.attach_viewer_demand(
            client_id,
            visibility="visible",
            requested_rate_hz=15.0,
            layers=_DEFAULT_LAYERS,
        )
        current = state.last_viewer_publication()
        if current is not None:
            await fanout.offer_to_client(client_id, current)
        state.request_visual_state_publish(reason="viewer_websocket_attach")

        async def receive_controls() -> None:
            while True:
                raw = await websocket.receive_text()
                if len(raw.encode("utf-8")) > _VIEWER_CONTROL_MAX_MESSAGE_SIZE:
                    await websocket.close(code=1009, reason="Message too large")
                    return
                try:
                    payload = json.loads(raw)
                    if not isinstance(payload, dict):
                        raise ValueError("viewer control message must be an object")
                    if payload.get("type") == "resync":
                        ViewerResyncControlV1.model_validate(payload)
                        await fanout.request_resync(client_id)
                        current_publication = state.last_viewer_publication()
                        if current_publication is not None:
                            await fanout.offer_to_client(
                                client_id,
                                current_publication,
                            )
                        continue
                    if payload.get("type") == "client_diagnostics":
                        diagnostics = ViewerClientDiagnosticsControlV1.model_validate(
                            payload
                        )
                        await fanout.update_client_diagnostics(
                            client_id,
                            diagnostics.model_dump(mode="json"),
                        )
                        continue
                    control = ViewerLeaseControlV1.model_validate(payload)
                    state.update_viewer_demand(
                        client_id,
                        visibility=control.visibility,
                        requested_rate_hz=control.requested_rate_hz,
                        layers=control.layers,
                    )
                    grant = state.viewer_demand_grant(client_id)
                    await fanout.publish_control(
                        client_id,
                        ViewerLeaseAckV1(
                            type="lease_ack",
                            schema_version="manyforge.viewer.diagnostics.v1",
                            request_id=control.request_id,
                            client_id=client_id,
                            timestamp_ns=time.monotonic_ns(),
                            expires_at_ns=grant.expires_at_ns,
                            lease_revision=grant.lease_revision,
                            visibility=grant.visibility,
                            granted_rate_hz=grant.requested_rate_hz,
                            granted_layers=sorted(grant.layers),
                            server_max_rate_hz=state.viewer_max_rate_hz(),
                        ).model_dump(mode="json"),
                    )
                    await publish_server_diagnostics(client_id)
                except (
                    TypeError,
                    ValueError,
                    KeyError,
                    ValidationError,
                    json.JSONDecodeError,
                ):
                    await websocket.close(
                        code=1008,
                        reason="Invalid viewer control message",
                    )
                    return

        writer = asyncio.create_task(fanout.send_loop(client_id))
        reader = asyncio.create_task(receive_controls())
        lease_watcher = asyncio.create_task(
            _watch_viewer_lease(state, websocket, client_id)
        )
        connection_tasks = (writer, reader, lease_watcher)
        try:
            done, _pending = await asyncio.wait(
                connection_tasks,
                return_when=asyncio.FIRST_COMPLETED,
            )
            for task in done:
                try:
                    task.result()
                except (WebSocketDisconnect, asyncio.TimeoutError):
                    pass
        except (WebSocketDisconnect, asyncio.CancelledError):
            # ASGI servers may cancel the endpoint itself when the peer closes.
            # Treat that as connection lifecycle, then collect every owned task
            # below so a concurrent receive-side disconnect is never leaked as
            # an unobserved task exception.
            pass
        finally:
            for task in connection_tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*connection_tasks, return_exceptions=True)
            state.detach_viewer_demand(client_id)
            await fanout.detach(client_id)

    router._manyforge_viewer_fanout = fanout  # type: ignore[attr-defined]
    return router
