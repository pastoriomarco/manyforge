"""Composer structural shell plus optional thin headless-runtime projection."""

from __future__ import annotations

import logging
from pathlib import Path
import re
import time
import uuid

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.exception_handlers import http_exception_handler
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.exceptions import HTTPException as StarletteHTTPException

from .log_throttle import ThrottledLog
from .models import UnavailableDetail, UnavailableResponse
from .routes_catalog import create_catalog_router
from .routes_filesystem import create_filesystem_router
from .routes_nodes import create_nodes_router
from .routes_urdf import create_urdf_router
from .security import BearerAuthMiddleware, SecurityConfig, check_ws_bearer_token
from .state import (
    ComposerState,
    RUNTIME_UNAVAILABLE_MESSAGE,
    RUNTIME_UNAVAILABLE_REASON_CODE,
)


logger = logging.getLogger(__name__)

_UNAVAILABLE_PAYLOAD = UnavailableResponse(
    detail=UnavailableDetail(
        code=RUNTIME_UNAVAILABLE_REASON_CODE,
        message=RUNTIME_UNAVAILABLE_MESSAGE,
        restoration_gates=["N", "L0", "P5", "I", "R", "T"],
    )
).detail.model_dump(by_alias=True)


def create_app(
    state: ComposerState,
    static_dir: str | None = None,
    security: SecurityConfig | None = None,
) -> FastAPI:
    """Create the structural Composer and optional headless client routes.

    Only immutable catalog reads, sandboxed artifact browsing, and structural
    URDF assets are owned locally. When configured, runtime routes proxy the
    independent headless API; every later-lane route remains caught by the
    typed unavailable boundary before it can create a process or mutate robot
    state.
    """

    security = security or SecurityConfig()
    state.allowed_fs_roots = list(security.allowed_fs_roots)

    app = FastAPI(title="ManyForge Composer (replaceable client)")
    if security.api_token:
        app.add_middleware(BearerAuthMiddleware, token=security.api_token)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=security.cors_origins,
        allow_methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
        allow_headers=["Authorization", "Content-Type"],
    )

    # Request correlation and bounded access logging are shell infrastructure,
    # not runtime ownership. Keep HTTPException rendering inside this
    # BaseHTTPMiddleware boundary so typed route errors are not flattened to
    # anonymous 500 responses.
    request_log_throttle = ThrottledLog(min_interval_s=5.0)

    @app.middleware("http")
    async def log_requests(request: Request, call_next) -> Response:
        incoming_id = re.sub(
            r"[^A-Za-z0-9._-]", "", request.headers.get("x-request-id", "")
        )[:64]
        request_id = incoming_id or uuid.uuid4().hex[:16]
        request.state.request_id = request_id
        started_at = time.monotonic()
        try:
            response: Response = await call_next(request)
        except (HTTPException, StarletteHTTPException) as exc:
            response = await http_exception_handler(request, exc)
        except Exception:
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
        duration_ms = (time.monotonic() - started_at) * 1000.0
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

    @app.get("/api/health")
    def health() -> dict:
        if state.headless_client is not None:
            from manyforge_headless_api.client import HeadlessClientError

            try:
                headless = state.headless_client.health()
                return {
                    "status": headless.get("status", "degraded"),
                    "mode": "headless_client",
                    "components": {
                        "backend": "structural_client",
                        "catalog": "available",
                        "deployment": (
                            "authored" if state.deployment is not None else "none"
                        ),
                        "headless_projection": (
                            "ready"
                            if headless.get("headless_projection_ready")
                            else "degraded"
                        ),
                    },
                    "headless": headless,
                    "runtimeAvailability": None,
                    "timestamp": time.time(),
                }
            except HeadlessClientError as error:
                return {
                    "status": "degraded",
                    "mode": "headless_client",
                    "components": {
                        "backend": "structural_client",
                        "catalog": "available",
                        "deployment": (
                            "authored" if state.deployment is not None else "none"
                        ),
                        "headless_projection": "unavailable",
                    },
                    "headless": {
                        "status_scope": "headless_projection",
                        "status": "unavailable",
                        "headless_projection_ready": False,
                        "robot_readiness": {
                            "available": False,
                            "state": None,
                            "source_owner_id": None,
                            "reason_code": "robot_readiness_owner_unavailable",
                        },
                        "error": {"code": error.code, "message": str(error)},
                    },
                    "runtimeAvailability": {
                        "code": error.code,
                        "message": str(error),
                        "restorationGates": [],
                    },
                    "timestamp": time.time(),
                }
        return {
            "status": "degraded",
            "mode": "structural_only",
            "components": {
                "backend": "structural_only",
                "catalog": "available",
                "deployment": (
                    "authored" if state.deployment is not None else "none"
                ),
                "runtime": "unavailable",
            },
            "runtimeAvailability": _UNAVAILABLE_PAYLOAD,
            "timestamp": time.time(),
        }

    app.include_router(
        create_catalog_router(state),
        prefix="/api/catalog",
        tags=["catalog"],
    )
    app.include_router(
        create_nodes_router(state),
        prefix="/api/nodes",
        tags=["node-environment"],
    )
    app.include_router(
        create_filesystem_router(allowed_roots=security.allowed_fs_roots),
        prefix="/api/files",
        tags=["files"],
    )
    app.include_router(
        create_urdf_router(state),
        prefix="/api/scene/urdf",
        tags=["structural-urdf"],
    )

    if state.headless_client is not None:
        from .headless_ws import serve_headless_state
        from .routes_headless import create_headless_router

        app.include_router(
            create_headless_router(state.headless_client),
            tags=["headless-runtime-client"],
        )

        @app.websocket("/ws/state")
        async def headless_state_websocket(websocket: WebSocket) -> None:
            if security.api_token and not check_ws_bearer_token(
                websocket,
                security.api_token,
            ):
                await websocket.close(code=1008, reason="Unauthorized")
                return
            await serve_headless_state(websocket, state.headless_client)

    async def unavailable_api(
        unavailable_path: str,
        request: Request,
    ) -> JSONResponse:
        del unavailable_path, request
        return JSONResponse(status_code=503, content={"detail": _UNAVAILABLE_PAYLOAD})

    for unavailable_method in ("GET", "POST", "PUT", "PATCH", "DELETE"):
        app.add_api_route(
            "/api/{unavailable_path:path}",
            unavailable_api,
            methods=[unavailable_method],
            operation_id=f"unavailable_api_{unavailable_method.lower()}",
        )

    @app.websocket("/ws/{unavailable_path:path}")
    async def unavailable_websocket(
        websocket: WebSocket,
        unavailable_path: str,
    ) -> None:
        del unavailable_path
        await websocket.close(
            code=1013,
            reason=RUNTIME_UNAVAILABLE_REASON_CODE,
        )

    resolved_static = _find_static_dir(static_dir)
    if resolved_static is not None:
        app.mount(
            "/",
            StaticFiles(directory=str(resolved_static), html=True),
            name="static",
        )
    elif static_dir is not None:
        raise RuntimeError(
            f"Explicit frontend static dir is missing or has no index.html: {static_dir}"
        )

    return app


def _find_static_dir(explicit: str | None) -> Path | None:
    if explicit is not None:
        candidate = Path(explicit)
        if candidate.is_dir() and (candidate / "index.html").exists():
            return candidate
        return None

    candidates = (
        Path(__file__).parent.parent / "frontend" / "dist",
        Path("/opt/manyforge-frontend/dist"),
    )
    for candidate in candidates:
        if candidate.is_dir() and (candidate / "index.html").exists():
            return candidate
    return None
