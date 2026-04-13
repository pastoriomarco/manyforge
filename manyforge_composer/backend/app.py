"""FastAPI app factory for ManyForge Composer."""
from __future__ import annotations

import logging
import time
from contextlib import asynccontextmanager
import asyncio
from pathlib import Path

from fastapi import FastAPI, Request, Response, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

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
from .routes_execution import create_execution_router
from .routes_debug import create_debug_router
from .routes_skills import create_skills_router
from .routes_direct import create_direct_router
from .routes_mcp import create_mcp_router
from .routes_infra import create_infra_router

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
        hub.stop()

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

    # Request logging middleware
    @app.middleware("http")
    async def log_requests(request: Request, call_next) -> Response:
        start = time.monotonic()
        response: Response = await call_next(request)
        duration_ms = (time.monotonic() - start) * 1000
        logger.info(
            "%s %s %d %.1fms",
            request.method,
            request.url.path,
            response.status_code,
            duration_ms,
            extra={
                "method": request.method,
                "path": str(request.url.path),
                "status_code": response.status_code,
                "duration_ms": round(duration_ms, 1),
                "client": request.client.host if request.client else None,
            },
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
    app.include_router(create_execution_router(state), prefix="/api/execution", tags=["execution"])
    app.include_router(create_skills_router(state), prefix="/api", tags=["skills"])
    app.include_router(create_debug_router(state), prefix="/api/debug", tags=["debug"])
    app.include_router(create_direct_router(state), prefix="/api/direct", tags=["direct"])
    app.include_router(create_infra_router(state), prefix="/api/infra", tags=["infra"])

    if enable_mcp_http:
        app.include_router(create_mcp_router(state), prefix="/api/mcp", tags=["mcp"])
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
        app.mount("/", StaticFiles(directory=str(resolved_static), html=True), name="static")

    return app


def _find_static_dir(explicit: str | None) -> Path | None:
    """Find the frontend dist directory."""
    candidates = [
        Path(explicit) if explicit else None,
        Path(__file__).parent.parent / "frontend" / "dist",  # Dev (relative to backend/)
        Path("/opt/manyforge-frontend/dist"),  # Docker image (fallback)
    ]
    for p in candidates:
        if p and p.is_dir() and (p / "index.html").exists():
            return p
    return None
