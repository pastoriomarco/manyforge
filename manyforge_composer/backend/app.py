"""Composer structural/Program-authoring shell plus thin runtime projections."""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.exception_handlers import http_exception_handler
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.types import ASGIApp, Message, Receive, Scope, Send

from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
)
from manyforge_headless_api.request_validation import request_validation_detail

from .composer_settings import ComposerSettingsStore
from .log_throttle import ThrottledLog
from .models import UnavailableDetail, UnavailableResponse
from .routes_assistant import create_assistant_router
from .routes_assistant_draft import create_assistant_draft_router
from .routes_authoring_workspace import create_authoring_workspace_router
from .routes_catalog import create_catalog_router
from .routes_composer_settings import create_composer_settings_router
from .routes_draft import create_draft_router
from .routes_deployment_draft import create_deployment_draft_router
from .routes_filesystem import create_filesystem_router
from .routes_nodes import create_nodes_router
from .routes_scenario_documents import create_scenario_document_router
from .routes_urdf import create_urdf_router
from .routes_viewer import create_viewer_router
from .security import SecurityConfig
from .workbench_session import previous_session_notice
from .state import (
    RUNTIME_UNAVAILABLE_MESSAGE,
    RUNTIME_UNAVAILABLE_REASON_CODE,
    ComposerState,
)

logger = logging.getLogger(__name__)

_MAXIMUM_REQUEST_BODY_BYTES = 1024 * 1024
# A structured error code is one short identifier; nothing else of an error
# body (messages may carry user data) reaches the request log.
_LOGGED_ERROR_CODE = re.compile(r"[a-z][a-z0-9_.-]{0,127}")
# A refusal's structured details are logged only when every key and value is
# an identifier list (blocking conditions, operation and resource ids): what a
# refusal waits for must be recoverable from the log, never free text.
_LOGGED_DETAIL_KEY = re.compile(r"[a-z][a-z0-9_]{0,63}")
_LOGGED_DETAIL_VALUE = re.compile(r"[A-Za-z0-9_.:/@,-]{0,1024}")
_MAXIMUM_LOGGED_DETAILS = 8


def _structured_error_code(body: bytes) -> str | None:
    """Return only ``detail.code`` of one JSON error body, when it has one."""

    try:
        payload = json.loads(body)
    except ValueError:
        return None
    detail = payload.get("detail") if isinstance(payload, dict) else None
    code = detail.get("code") if isinstance(detail, dict) else None
    return (
        code
        if isinstance(code, str) and _LOGGED_ERROR_CODE.fullmatch(code)
        else None
    )

def _structured_error_details(body: bytes) -> dict[str, str] | None:
    """Return the identifier-only ``detail.details`` of one JSON error body."""

    try:
        payload = json.loads(body)
    except ValueError:
        return None
    detail = payload.get("detail") if isinstance(payload, dict) else None
    details = detail.get("details") if isinstance(detail, dict) else None
    if not isinstance(details, dict):
        return None
    logged = {
        key: value
        for key, value in sorted(details.items())
        if isinstance(key, str)
        and _LOGGED_DETAIL_KEY.fullmatch(key)
        and isinstance(value, str)
        and _LOGGED_DETAIL_VALUE.fullmatch(value)
    }
    return dict(list(logged.items())[:_MAXIMUM_LOGGED_DETAILS]) or None


_UNAVAILABLE_PAYLOAD = UnavailableResponse(
    detail=UnavailableDetail(
        code=RUNTIME_UNAVAILABLE_REASON_CODE,
        message=RUNTIME_UNAVAILABLE_MESSAGE,
        restoration_gates=["N7", "L0", "P5", "I", "R", "T"],
    )
).detail.model_dump(by_alias=True)


class ComposerRequestBodyLimitMiddleware:
    """Reject oversized bodies on one bounded callback path family."""

    def __init__(
        self,
        app: ASGIApp,
        *,
        maximum_bytes: int,
        path_prefix: str,
        recognized_stop: bool = False,
    ) -> None:
        self.app = app
        self.maximum_bytes = maximum_bytes
        self.path_prefix = path_prefix
        self.recognized_stop = recognized_stop

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        path = str(scope.get("path", ""))
        path_matches = (
            path == self.path_prefix
            if self.recognized_stop
            else path.startswith(self.path_prefix)
        )
        if scope["type"] != "http" or not path_matches:
            await self.app(scope, receive, send)
            return

        def mark_malformed(*, close_connection: bool) -> None:
            state = scope.setdefault("state", {})
            state["recognized_stop_malformed"] = True
            if close_connection:
                state["recognized_stop_close_connection"] = True

        headers = dict(scope.get("headers", ()))
        raw_length = headers.get(b"content-length")
        declared_oversized = False
        if raw_length is not None:
            try:
                declared = int(raw_length)
            except ValueError:
                await self._reject(scope, receive, send)
                return
            if declared < 0:
                await self._reject(scope, receive, send)
                return
            if declared > self.maximum_bytes:
                if not self.recognized_stop:
                    await self._reject(scope, receive, send)
                    return
                mark_malformed(close_connection=True)
                declared_oversized = True

        body = bytearray()
        disconnected = False
        if declared_oversized:
            body.extend(b"{}")
        else:
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    disconnected = True
                    break
                if message["type"] != "http.request":
                    continue
                body.extend(message.get("body", b""))
                if len(body) > self.maximum_bytes:
                    if not self.recognized_stop:
                        await self._reject(scope, receive, send)
                        return
                    mark_malformed(close_connection=True)
                    body.clear()
                    body.extend(b"{}")
                    break
                if not message.get("more_body", False):
                    break

        if self.recognized_stop and not declared_oversized and not disconnected:
            try:
                from manyforge_headless_api.native import strict_json_object

                strict_json_object(
                    bytes(body).decode("utf-8"),
                    label="Composer recognized Stop request",
                )
            except (RuntimeError, UnicodeError):
                mark_malformed(close_connection=False)
                body.clear()
                body.extend(b"{}")

        delivered = False

        async def replay_receive() -> Message:
            nonlocal delivered
            if not delivered:
                delivered = True
                if disconnected:
                    return {"type": "http.disconnect"}
                return {
                    "type": "http.request",
                    "body": bytes(body),
                    "more_body": False,
                }
            return await receive()

        await self.app(scope, replay_receive, send)

    @staticmethod
    async def _reject(scope: Scope, receive: Receive, send: Send) -> None:
        response = JSONResponse(
            status_code=413,
            content={
                "detail": {
                    "code": "composer_request_body_oversized",
                    "message": "request body exceeds the configured byte bound",
                }
            },
        )
        await response(scope, receive, send)


async def _typed_http_exception_handler(
    request: Request, error: StarletteHTTPException
) -> Response:
    """Render one HTTP refusal; Composer's own typed refusals name Composer.

    A refusal forwarded from the headless API keeps the producer it carries.
    """

    detail = error.detail
    if isinstance(detail, dict) and "code" in detail and "producer" not in detail:
        error = StarletteHTTPException(
            status_code=error.status_code,
            detail={**detail, "producer": "composer"},
            headers=error.headers,
        )
    return await http_exception_handler(request, error)


async def _request_validation_response(
    request: Request, error: RequestValidationError
) -> Response:
    """The one request-validation refusal shape (see the headless API)."""

    return JSONResponse(
        status_code=422,
        content={
            "detail": request_validation_detail(
                error.errors(),
                producer="composer",
                request_id=getattr(request.state, "request_id", None),
            )
        },
    )


def create_app(
    state: ComposerState,
    static_dir: str | None = None,
    security: SecurityConfig | None = None,
    *,
    enable_mcp: bool = False,
    settings_store: ComposerSettingsStore | None = None,
) -> FastAPI:
    """Create structural, Draft/Saved authoring, and thin headless routes.

    Composer locally owns immutable catalog reads, sandboxed artifact browsing,
    structural URDF assets, and one revision-fenced editable Program Draft with
    bounded Saved-artifact persistence. Applied/runtime truth remains an
    immutable projection from independent headless owners; every later-lane
    effect route is caught before it can create a process or mutate robot state.
    """

    session_notice = previous_session_notice(os.environ.get("MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT"))
    security = security or SecurityConfig()
    state.allowed_fs_roots = list(security.allowed_fs_roots)
    state.allowed_program_write_roots = (
        None
        if security.allowed_program_write_roots is None
        else list(security.allowed_program_write_roots)
    )
    state.allowed_deployment_write_roots = (
        None
        if security.allowed_deployment_write_roots is None
        else list(security.allowed_deployment_write_roots)
    )

    execution_controls = ExecutionControlClient(
        thread_name_prefix="manyforge_composer_control",
    )
    headless_state_source = None
    if state.headless_client is not None and state.authoring_admission is not None:
        from .headless_ws import HeadlessStateSource

        headless_state_source = HeadlessStateSource(
            state.headless_client,
            admission=state.authoring_admission,
            workspace=state.authoring_workspace,
        )

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        try:
            if state.authoring_admission is not None:
                try:
                    await asyncio.to_thread(state.authoring_admission.recover)
                except HTTPException as error:
                    if error.status_code != 503:
                        raise
                    # The authoring owner has already failed closed. Keep
                    # read-only Composer projections available while its
                    # required execution owner is absent; startup is not an
                    # implicit recovery or a grant of edit authority.
                    logger.warning(
                        "Composer authoring remains unavailable at startup: %s",
                        error.detail,
                    )
            yield
        finally:
            if state.execution_observation_source is not None:
                state.execution_observation_source.close()
            if headless_state_source is not None:
                await headless_state_source.close()
            execution_controls.close()

    app = FastAPI(
        title="ManyForge Composer (replaceable client)",
        lifespan=lifespan,
    )
    app.add_exception_handler(StarletteHTTPException, _typed_http_exception_handler)
    app.add_exception_handler(RequestValidationError, _request_validation_response)
    app.add_middleware(
        ComposerRequestBodyLimitMiddleware,
        maximum_bytes=_MAXIMUM_REQUEST_BODY_BYTES,
        path_prefix="/api/assistant/bridge/tools/",
    )
    app.add_middleware(
        ComposerRequestBodyLimitMiddleware,
        maximum_bytes=4 * _MAXIMUM_REQUEST_BODY_BYTES,
        path_prefix="/api/assistant/requests",
    )
    for program_authoring_prefix in (
        "/api/program/draft/",
        "/api/program/open-draft",
        "/api/program/apply",
        "/api/program/new-draft",
        "/api/program/discard",
        "/api/program/revert",
        "/api/program/runs",
        "/api/program/save",
        "/api/program/validate",
        "/api/scenario-documents/",
    ):
        app.add_middleware(
            ComposerRequestBodyLimitMiddleware,
            maximum_bytes=_MAXIMUM_REQUEST_BODY_BYTES,
            path_prefix=program_authoring_prefix,
        )
    app.add_middleware(
        ComposerRequestBodyLimitMiddleware,
        maximum_bytes=_MAXIMUM_REQUEST_BODY_BYTES,
        path_prefix="/api/deployment/",
    )
    app.add_middleware(
        CORSMiddleware,
        allow_origins=security.cors_origins,
        allow_methods=["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
        allow_headers=[
            "Content-Type",
            "X-Request-ID",
            "X-ManyForge-Viewer-Owner-Epoch",
            "X-ManyForge-Viewer-Generation",
        ],
    )
    if state.headless_client is not None:
        for recognized_stop_path in (
            "/api/execution/stops",
            "/api/execution/full-stop",
        ):
            app.add_middleware(
                ComposerRequestBodyLimitMiddleware,
                maximum_bytes=_MAXIMUM_REQUEST_BODY_BYTES,
                path_prefix=recognized_stop_path,
                recognized_stop=True,
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
            response = await _typed_http_exception_handler(request, exc)
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
                    "detail": {
                        "code": "composer_internal_failure",
                        "message": "Internal Server Error",
                        "details": {},
                        "producer": "composer",
                        "request_id": request_id,
                    },
                    "request_id": request_id,
                },
            )
        duration_ms = (time.monotonic() - started_at) * 1000.0
        error_code = None
        error_details = None
        if response.status_code >= 400 and response.headers.get(
            "content-type", ""
        ).startswith("application/json"):
            body = getattr(response, "body", None)
            if body is None:
                # A routed response streams; error bodies are small and are
                # replayed unchanged after their code is read, with the raw
                # header list (repeated headers such as Set-Cookie included).
                body = b"".join([chunk async for chunk in response.body_iterator])
                streamed = response
                response = Response(
                    content=body,
                    status_code=streamed.status_code,
                    background=streamed.background,
                )
                response.raw_headers = list(streamed.raw_headers)
            error_code = _structured_error_code(body)
            error_details = _structured_error_details(body)
        response.headers["X-Request-ID"] = request_id
        fields = {
            "method": request.method,
            "path": str(request.url.path),
            "status_code": response.status_code,
            "error_code": error_code,
            **({"error_details": error_details} if error_details else {}),
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
                    "workbench_session_notice": session_notice,
                    "startup_scenario_notice": state.startup_scenario_notice,
                    "mode": "headless_client",
                    "components": {
                        "backend": "structural_client",
                        "catalog": "available",
                        "viewer_model": "headless",
                        "program_draft": (
                            "loaded" if state.draft_session.program_loaded else "none"
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
                    "workbench_session_notice": session_notice,
                    "startup_scenario_notice": state.startup_scenario_notice,
                    "mode": "headless_client",
                    "components": {
                        "backend": "structural_client",
                        "catalog": "available",
                        "viewer_model": "headless_unavailable",
                        "program_draft": (
                            "loaded" if state.draft_session.program_loaded else "none"
                        ),
                        "headless_projection": "unavailable",
                    },
                    "headless": {
                        "status_scope": "headless_projection",
                        "status": "unavailable",
                        "headless_projection_ready": False,
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
            "workbench_session_notice": session_notice,
            "startup_scenario_notice": state.startup_scenario_notice,
            "mode": "structural_only",
            "components": {
                "backend": "structural_only",
                "catalog": "available",
                "viewer_model": "unavailable",
                "program_draft": (
                    "loaded" if state.draft_session.program_loaded else "none"
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
    # Installation presentation settings: Composer-owned, never execution.
    app.include_router(
        create_composer_settings_router(
            settings_store if settings_store is not None
            else ComposerSettingsStore(None)
        ),
        prefix="/api/composer/settings",
        tags=["composer-settings"],
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
    app.include_router(
        create_draft_router(state),
        tags=["composer-draft"],
    )
    app.include_router(
        create_authoring_workspace_router(state),
        tags=["composer-authoring-workspace"],
    )
    app.include_router(
        create_deployment_draft_router(state),
        tags=["composer-deployment-draft"],
    )
    app.include_router(
        create_scenario_document_router(state),
        tags=["composer-scenario-documents"],
    )
    if state.headless_client is not None:
        app.include_router(
            create_assistant_draft_router(state),
            tags=["assistant-draft-callback"],
        )
        app.include_router(
            create_assistant_router(state),
            tags=["assistant-provider-client"],
        )

    if state.headless_client is not None:
        from .headless_ws import serve_headless_state
        from .routes_headless import create_headless_router
        from .routes_pointcloud import create_pointcloud_router
        from .routes_scene_meshes import create_scene_meshes_router
        from .routes_snapshot import create_snapshot_router

        app.include_router(
            create_headless_router(
                state.headless_client,
                state,
                execution_controls,
            ),
            tags=["headless-runtime-client"],
        )
        app.include_router(
            create_scene_meshes_router(state),
            tags=["headless-scene-mesh-client"],
        )
        app.include_router(
            create_pointcloud_router(state, security),
            tags=["headless-pointcloud-observer-client"],
        )
        app.include_router(
            create_snapshot_router(state, security),
            tags=["headless-viewer-snapshot-client"],
        )
        app.include_router(
            create_viewer_router(state, security),
            tags=["headless-viewer-observation-client"],
        )

        @app.exception_handler(RequestValidationError)
        async def recognized_stop_validation_handler(
            request: Request,
            error: RequestValidationError,
        ) -> Response:
            if request.method == "POST" and request.url.path in {
                "/api/execution/stops",
                "/api/execution/full-stop",
            }:
                from manyforge_headless_api.client import HeadlessClientError

                try:
                    payload = await execution_controls.stop(
                        lambda: state.headless_client.recognized_malformed_stop(
                            request.url.path
                        )
                    )
                    response: Response = JSONResponse(content=payload)
                except ExecutionControlBusyError as busy_error:
                    response = JSONResponse(
                        status_code=503,
                        content={
                            "detail": {
                                "code": "execution_control_ingress_busy",
                                "message": str(busy_error),
                            }
                        },
                    )
                except HeadlessClientError as client_error:
                    response = JSONResponse(
                        status_code=client_error.status_code or 503,
                        content={
                            "detail": {
                                "code": client_error.code,
                                "message": str(client_error),
                            }
                        },
                    )
                if getattr(
                    request.state, "recognized_stop_close_connection", False
                ):
                    response.headers["Connection"] = "close"
                return response
            return await _request_validation_response(request, error)

        @app.websocket("/ws/control")
        async def headless_state_websocket(websocket: WebSocket) -> None:
            if headless_state_source is None:
                await websocket.close(code=1008)
                return
            origin = websocket.headers.get("origin")
            host = websocket.headers.get("host")
            if origin is not None:
                from urllib.parse import urlsplit

                parsed_origin = urlsplit(origin)
                same_origin = (
                    parsed_origin.scheme in {"http", "https"}
                    and parsed_origin.netloc == host
                )
                if not same_origin and origin not in security.cors_origins:
                    await websocket.close(code=1008, reason="control origin denied")
                    return
            await serve_headless_state(
                websocket,
                headless_state_source,
            )

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

    if enable_mcp:
        from .mcp_adapter import ComposerMcpAdapter

        mcp_adapter = ComposerMcpAdapter(app, security)
        app.add_middleware(
            ComposerRequestBodyLimitMiddleware,
            maximum_bytes=_MAXIMUM_REQUEST_BODY_BYTES,
            path_prefix="/mcp",
        )
        app.mount("/mcp", mcp_adapter.handle_request, name="mcp")

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
