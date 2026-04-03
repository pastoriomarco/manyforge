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

from .state import ComposerState
from .ws_state import StateHub
from .routes_cycle import create_cycle_router
from .routes_catalog import create_catalog_router
from .routes_program import create_program_router

logger = logging.getLogger(__name__)


def create_app(state: ComposerState, static_dir: str | None = None) -> FastAPI:
    """Create the FastAPI application with the given state."""
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

    # CORS for dev mode (Vite on port 3000 → FastAPI on 9000)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
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

    # Health endpoint
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

    # WebSocket endpoint
    @app.websocket("/ws/state")
    async def ws_state(ws: WebSocket):
        await hub.connect(ws)
        try:
            while True:
                await ws.receive_text()  # keep alive
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
        Path("/opt/manyforge-frontend/dist"),  # Docker image
        Path(__file__).parent.parent / "frontend" / "dist",  # Dev (relative to backend/)
    ]
    for p in candidates:
        if p and p.is_dir() and (p / "index.html").exists():
            return p
    return None
