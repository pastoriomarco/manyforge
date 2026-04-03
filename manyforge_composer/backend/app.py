"""FastAPI app factory for ManyForge Composer."""
from __future__ import annotations

from contextlib import asynccontextmanager
import asyncio

from fastapi import FastAPI, WebSocket

from .state import ComposerState
from .ws_state import StateHub
from .routes_cycle import create_cycle_router
from .routes_catalog import create_catalog_router
from .routes_program import create_program_router


def create_app(state: ComposerState) -> FastAPI:
    """Create the FastAPI application with the given state."""
    hub = StateHub()
    
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        hub.start(state)
        hub._task = asyncio.create_task(hub._broadcast_loop())
        yield
        hub.stop()
    
    app = FastAPI(title="ManyForge Composer", lifespan=lifespan)
    
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
    
    return app
