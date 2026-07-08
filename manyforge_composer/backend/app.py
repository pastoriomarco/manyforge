"""FastAPI app factory for ManyForge Composer."""
from __future__ import annotations

import logging
import re
import time
import uuid
from contextlib import asynccontextmanager
import asyncio
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.exception_handlers import http_exception_handler
from fastapi.responses import JSONResponse
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from starlette.exceptions import HTTPException as StarletteHTTPException

from .log_throttle import ThrottledLog
from .security import (
    BearerAuthMiddleware,
    SecurityConfig,
    WS_MAX_MESSAGE_SIZE,
    check_ws_bearer_token,
)
from .state import ComposerState
from .ws_state import StateHub
from .routes_cycle import create_cycle_router
from .routes_catalog import create_catalog_router
from .routes_program import create_program_router
from .routes_filesystem import create_filesystem_router
from .routes_kernel import create_kernel_router
from .routes_scene import create_scene_router
from .routes_scene_meshes import create_scene_meshes_router
from .routes_pointcloud import create_pointcloud_router
from .routes_snapshot import create_snapshot_router
from .routes_urdf import create_urdf_router
from .routes_execution import create_execution_router
from .routes_debug import create_debug_router
from .routes_skills import create_skills_router
from .routes_direct import create_direct_router
from .robot_control_candidates import create_robot_control_candidates_router
from .routes_mcp import create_mcp_router
from .routes_infra import create_infra_router
from .routes_runtime import create_runtime_router
from .routes_assistant import create_assistant_router
from .routes_simulation import create_simulation_router
from .route_execution import shutdown_route_executors

logger = logging.getLogger(__name__)


def create_app(
    state: ComposerState,
    static_dir: str | None = None,
    security: SecurityConfig | None = None,
    enable_mcp_http: bool = False,
) -> FastAPI:
    """Create the FastAPI application with the given state."""
    if security is None:
        security = SecurityConfig()
    state.allowed_fs_roots = security.allowed_fs_roots

    hub = StateHub()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        logger.info("ManyForge Composer starting up")
        hub.start(state)
        hub._task = asyncio.create_task(hub._broadcast_loop())
        yield
        logger.info("ManyForge Composer shutting down")
        # Ordered quiesce, each step independently guarded. Motion first —
        # a graceful shutdown must never leave the kernel executing on the
        # chance that interpreter-exit GC runs the native teardown. The
        # event recorder closes LAST so the stop/fault events produced by
        # the quiesce itself reach disk before the writer threads die.
        if state.cycle_manager is not None:
            try:
                await asyncio.to_thread(state.cycle_manager.shutdown)
            except Exception:  # noqa: BLE001 — continue to the remaining steps
                logger.exception("Shutdown: cycle quiesce failed")
        try:
            routes_idle = await asyncio.to_thread(
                shutdown_route_executors,
                wait_timeout_s=1.0,
            )
            if not routes_idle:
                logger.error("Shutdown: route executors did not drain within budget")
        except Exception:  # noqa: BLE001 — continue to state teardown
            logger.exception("Shutdown: route executor quiesce failed")
        # Final observability flush: the quiesce's own kernel events (stop
        # request, state transitions) sit in the kernel client's buffer and
        # nothing drains them once the tick loop is gone. Project them and a
        # forced status observation into durable history now — before the
        # kernel client is closed (the drain needs the live bundle) and
        # before the recorder's writers exit. record_native_kernel_events
        # dedupes by event_index, so replaying from 0 is safe.
        recorder = getattr(state, "event_recorder", None)
        if recorder is not None:

            def _flush_final_events() -> None:
                if state.cycle_manager is not None:
                    state.cycle_manager.drain_kernel_events_now()
                    recorder.record_native_kernel_events(
                        state.cycle_manager.get_kernel_events_since(0),
                        pipeline_status=(
                            state.cycle_manager.kernel_event_pipeline_status()
                        ),
                    )
                recorder.observe(state, force=True)

            try:
                await asyncio.to_thread(_flush_final_events)
            except Exception:  # noqa: BLE001 — continue to the remaining steps
                logger.exception("Shutdown: final event flush failed")
        state.stop_visual_state_publisher()
        hub.stop()
        if state.kernel is not None:
            try:
                await asyncio.to_thread(state.kernel.close)
            except Exception:  # noqa: BLE001 — continue to the remaining steps
                logger.exception("Shutdown: kernel close failed")
        if recorder is not None:
            try:
                flushed = await asyncio.to_thread(recorder.close)
                if not flushed:
                    logger.error(
                        "Shutdown: event recorder writers did not flush within budget"
                    )
            except Exception:  # noqa: BLE001 — nothing left to fail after this
                logger.exception("Shutdown: event recorder close failed")

    app = FastAPI(title="ManyForge Composer", lifespan=lifespan)

    # Bearer-token auth (when configured)
    if security.api_token:
        app.add_middleware(BearerAuthMiddleware, token=security.api_token)

    # CORS — restricted to configured origins
    app.add_middleware(
        CORSMiddleware,
        allow_origins=security.cors_origins,
        allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
        allow_headers=["Authorization", "Content-Type"],
    )

    # Request logging + request-ID middleware.
    #
    # Starlette gotcha: `@app.middleware("http")` is `BaseHTTPMiddleware`,
    # which interrupts the normal exception chain. Without the explicit
    # HTTPException catch below, an `HTTPException(400, detail="...")`
    # raised by a route handler escapes this middleware to
    # ServerErrorMiddleware, which renders it as a flat HTTP 500
    # "Internal Server Error" with no detail (verified 2026-05-06 against
    # /api/assistant/bridge/tools/tree_draft_wrap_node returning duplicate-
    # node errors as 500 → the LLM-side agent loops forever because the
    # error has no actionable info). Catching HTTPException here and
    # delegating to FastAPI's built-in handler renders the correct
    # status + JSON body so callers (and LLM agents) see the real reason.
    #
    # Log volume: the UI polls several GETs at up to 10 Hz, which at
    # one INFO line per request is ~1.7M lines/day of steady-state noise.
    # Successful GETs are therefore throttled per route (one structured
    # line per route per window, carrying suppressed_since_last so the
    # true rate stays readable); a one-off GET still logs immediately as
    # the first hit in its window. Mutations and failures always log
    # unthrottled — those are the lines an operator greps for.
    request_log_throttle = ThrottledLog(min_interval_s=5.0)

    @app.middleware("http")
    async def log_requests(request: Request, call_next) -> Response:
        incoming_id = re.sub(
            r"[^A-Za-z0-9._-]", "", request.headers.get("x-request-id", "")
        )[:64]
        request_id = incoming_id or uuid.uuid4().hex[:16]
        request.state.request_id = request_id
        start = time.monotonic()
        try:
            response: Response = await call_next(request)
        except (HTTPException, StarletteHTTPException) as exc:
            response = await http_exception_handler(request, exc)
        except Exception:
            # Without this, an unhandled route exception escapes to
            # ServerErrorMiddleware, whose 500 is built OUTSIDE this
            # middleware — no X-Request-ID header, no structured request
            # log, on exactly the failure path where ticket correlation
            # matters most. Log the traceback with the ID, answer 500.
            logger.exception(
                "Unhandled exception for %s %s",
                request.method,
                request.url.path,
                extra={
                    "method": request.method,
                    "path": str(request.url.path),
                    "request_id": request_id,
                },
            )
            response = JSONResponse(
                status_code=500,
                content={
                    "detail": "Internal Server Error",
                    "request_id": request_id,
                },
            )
        duration_ms = (time.monotonic() - start) * 1000
        response.headers["X-Request-ID"] = request_id
        fields = {
            "method": request.method,
            "path": str(request.url.path),
            "status_code": response.status_code,
            "duration_ms": round(duration_ms, 1),
            "client": request.client.host if request.client else None,
            "request_id": request_id,
        }
        if request.method == "GET" and response.status_code < 400:
            # Throttle-key on the ROUTE TEMPLATE, not the raw path: paths
            # embedding ids would grow the throttle table without bound.
            route = request.scope.get("route")
            route_key = getattr(route, "path", None) or str(request.url.path)
            request_log_throttle.info(
                logger,
                f"request:GET:{route_key}",
                "%s %s %d %.1fms",
                request.method,
                request.url.path,
                response.status_code,
                duration_ms,
                extra=fields,
            )
        else:
            logger.info(
                "%s %s %d %.1fms",
                request.method,
                request.url.path,
                response.status_code,
                duration_ms,
                extra=fields,
            )
        return response

    # Health endpoint (always public — used by liveness probes)
    @app.get("/api/health")
    def health():
        return {
            "status": "ok",
            "components": {
                "backend": "ok",
                "catalog": "ok" if state.catalog else "unavailable",
                "program": "loaded" if state.program else "none",
                "cycle": state.cycle_manager.state.value if state.cycle_manager else "none",
            },
            "timestamp": time.time(),
        }

    app.include_router(create_cycle_router(state), prefix="/api/cycle", tags=["cycle"])
    app.include_router(create_catalog_router(state), prefix="/api/catalog", tags=["catalog"])
    app.include_router(create_program_router(state), prefix="/api", tags=["program"])
    app.include_router(
        create_filesystem_router(allowed_roots=security.allowed_fs_roots),
        prefix="/api/files",
        tags=["files"],
    )
    app.include_router(create_kernel_router(state), prefix="/api/kernel", tags=["kernel"])
    app.include_router(create_scene_router(state), prefix="/api/scene", tags=["scene"])
    app.include_router(
        create_scene_meshes_router(state),
        prefix="/api/scene/object-meshes",
        tags=["scene-object-meshes"],
    )
    app.include_router(create_urdf_router(state), prefix="/api/scene/urdf", tags=["scene-urdf"])
    # Snapshot router carries the /ws/scene/snapshot WS + the upload
    # POST under /api/scene/snapshot/upload/{request_id}; both paths
    # are absolute, so include without a prefix.
    app.include_router(create_snapshot_router(state, security), tags=["scene-snapshot"])
    # Point-cloud / octomap streaming WS. Mounts at app root (path
    # absolute). Sits idle until a perception source registers itself
    # via state.point_cloud_publisher.publish(...).
    app.include_router(create_pointcloud_router(state, security), tags=["scene-pointcloud"])
    app.include_router(create_execution_router(state), prefix="/api/execution", tags=["execution"])
    app.include_router(create_skills_router(state), prefix="/api", tags=["skills"])
    app.include_router(create_debug_router(state), prefix="/api/debug", tags=["debug"])
    app.include_router(create_direct_router(state), prefix="/api/direct", tags=["direct"])
    app.include_router(
        create_robot_control_candidates_router(state),
        prefix="/api/robot-control",
        tags=["robot-control"],
    )
    app.include_router(create_infra_router(state), prefix="/api/infra", tags=["infra"])
    app.include_router(create_runtime_router(state), prefix="/api/runtime", tags=["runtime"])
    app.include_router(create_assistant_router(state), prefix="/api/assistant", tags=["assistant"])
    app.include_router(
        create_simulation_router(state),
        prefix="/api/simulation",
        tags=["simulation"],
    )

    if enable_mcp_http:
        mcp_router = create_mcp_router(state)
        mcp_sessions = getattr(mcp_router, "_manyforge_mcp_sessions", None)
        if mcp_sessions is not None:
            app.state.manyforge_mcp_sessions = mcp_sessions
        app.include_router(mcp_router, prefix="/api/mcp", tags=["mcp"])
        logger.info("HTTP/SSE MCP transport enabled on /api/mcp")

    # WebSocket endpoint
    @app.websocket("/ws/state")
    async def ws_state(ws: WebSocket):
        # Authenticate WebSocket upgrades when token is configured
        if security.api_token and not check_ws_bearer_token(ws, security.api_token):
            await ws.close(code=4401, reason="Unauthorized")
            return

        await hub.connect(ws)
        try:
            while True:
                msg = await ws.receive_text()
                if len(msg) > WS_MAX_MESSAGE_SIZE:
                    await ws.close(code=1009, reason="Message too large")
                    break
                if msg == "ping":
                    await ws.send_text("pong")
        except Exception:
            hub.disconnect(ws)

    # Serve built frontend in production mode (must be last — catches all routes)
    resolved_static = _find_static_dir(static_dir)
    if resolved_static:
        logger.info("Serving frontend static files from %s", resolved_static)
        app.mount("/", StaticFiles(directory=str(resolved_static), html=True), name="static")
    elif static_dir:
        raise RuntimeError(
            f"Explicit frontend static dir is missing or has no index.html: {static_dir}"
        )
    else:
        logger.warning("No frontend static dist found; '/' will not serve UI")

    return app


def _find_static_dir(explicit: str | None) -> Path | None:
    """Find the frontend dist directory."""
    if explicit:
        p = Path(explicit)
        if p.is_dir() and (p / "index.html").exists():
            return p
        return None

    candidates = [
        Path(__file__).parent.parent / "frontend" / "dist",  # Dev (relative to backend/)
        Path("/opt/manyforge-frontend/dist"),  # Docker image (fallback)
    ]
    for p in candidates:
        if p and p.is_dir() and (p / "index.html").exists():
            return p
    return None
