"""Security configuration and middleware for ManyForge Composer."""
from __future__ import annotations

import os
import re
import secrets
from dataclasses import dataclass, field
from pathlib import Path

from fastapi import HTTPException, Request, WebSocket
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware, RequestResponseEndpoint
from starlette.types import ASGIApp

# Maximum WebSocket text message size (bytes).
WS_MAX_MESSAGE_SIZE = 256

# Principal-ID format: printable ASCII, 1-128 chars, no control characters.
# Matches identifiers like "human:alice", "token:ab12cd34", "service:planner-v2".
_PRINCIPAL_ID_RE = re.compile(r"^[\x20-\x7E]{1,128}$")
_PRINCIPAL_ID_MAX_LEN = 128


@dataclass
class SecurityConfig:
    """Runtime security settings for the Composer backend."""

    # CORS allowed origins.  Empty list → deny all cross-origin requests.
    cors_origins: list[str] = field(default_factory=lambda: ["http://localhost:3000"])

    # Bearer token for API authentication.  None → auth disabled (dev-only).
    api_token: str | None = None

    # Filesystem roots that the file-browser endpoint is allowed to traverse.
    # Paths outside these roots (after symlink resolution) are rejected.
    allowed_fs_roots: list[str] = field(default_factory=lambda: ["/workspace"])

    @classmethod
    def from_env(cls) -> SecurityConfig:
        """Build from environment variables (MANYFORGE_CORS_ORIGINS, MANYFORGE_API_TOKEN, MANYFORGE_FS_ROOTS)."""
        def _csv(var: str) -> list[str] | None:
            raw = os.environ.get(var, "")
            items = [s.strip() for s in raw.split(",") if s.strip()]
            return items or None

        return cls(
            cors_origins=_csv("MANYFORGE_CORS_ORIGINS") or ["http://localhost:3000"],
            api_token=os.environ.get("MANYFORGE_API_TOKEN"),
            allowed_fs_roots=_csv("MANYFORGE_FS_ROOTS") or ["/workspace"],
        )


def check_bearer_token(request: Request, token: str) -> bool:
    """Return True if the request carries a valid bearer token."""
    auth = request.headers.get("authorization", "")
    if not auth.startswith("Bearer "):
        return False
    return secrets.compare_digest(auth[7:], token)


def check_ws_bearer_token(ws: WebSocket, token: str) -> bool:
    """Return True if the WebSocket upgrade carries a valid bearer token.

    Checks both the Authorization header and a ``token`` query parameter
    (browsers cannot set headers on WebSocket connections).
    """
    auth = ws.headers.get("authorization", "")
    if auth.startswith("Bearer ") and secrets.compare_digest(auth[7:], token):
        return True
    query_token = ws.query_params.get("token", "")
    if query_token and secrets.compare_digest(query_token, token):
        return True
    return False


class BearerAuthMiddleware(BaseHTTPMiddleware):
    """Reject HTTP requests that do not carry a valid bearer token.

    The /api/health endpoint is always allowed (for liveness probes).
    """

    def __init__(self, app: ASGIApp, *, token: str) -> None:
        super().__init__(app)
        self._token = token

    async def dispatch(self, request: Request, call_next: RequestResponseEndpoint):
        if request.url.path == "/api/health":
            return await call_next(request)
        if not check_bearer_token(request, self._token):
            return JSONResponse(status_code=401, content={"detail": "Unauthorized"})
        # Set server-authoritative principal identity.
        # X-Principal-ID header is accepted only because the bearer token was
        # already validated — the token proves the caller is trusted, and the
        # header identifies *which* trusted principal is acting.  When no
        # header is supplied, fall back to a token-derived sentinel so that
        # every authenticated request carries a non-empty principal_id.
        principal_header = request.headers.get("x-principal-id", "").strip()
        if principal_header and not _PRINCIPAL_ID_RE.match(principal_header):
            return JSONResponse(
                status_code=400,
                content={
                    "detail": (
                        "Invalid X-Principal-ID: must be 1-128 printable ASCII characters"
                    )
                },
            )
        request.state.principal_id = (
            principal_header or f"token:{self._token[:8]}"
        )
        return await call_next(request)


def is_path_within_roots(path: Path, roots: list[str]) -> bool:
    """Return True if *path* (after resolve) is inside one of *roots*."""
    try:
        resolved = path.resolve()
    except Exception:
        return False
    for root in roots:
        try:
            root_resolved = Path(root).resolve()
        except Exception:
            continue
        try:
            resolved.relative_to(root_resolved)
            return True
        except ValueError:
            continue
    return False


def normalize_allowed_roots(roots: list[str] | None) -> list[str]:
    """Normalize configured filesystem roots into unique resolved absolute paths.

    If a root is explicitly configured, do not silently widen access by
    dropping it and falling back to `/workspace`. Non-existent roots remain
    valid allowlist entries; route handlers will surface a filesystem error
    when callers try to use them.
    """
    if not roots:
        return ["/workspace"]

    normalized: list[str] = []
    for root in roots:
        try:
            resolved = Path(root).expanduser().resolve()
        except Exception:
            continue
        root_str = str(resolved)
        if root_str not in normalized:
            normalized.append(root_str)

    return normalized or [str(Path("/workspace").resolve())]


def normalize_existing_file_path(
    raw_path: str,
    allowed_roots: list[str],
    *,
    kind_label: str = "Path",
) -> Path:
    """Resolve and validate an existing readable file path within allowed roots."""
    try:
        resolved = Path(raw_path).expanduser().resolve()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Invalid {kind_label.lower()}") from exc

    if not is_path_within_roots(resolved, allowed_roots):
        raise HTTPException(status_code=403, detail=f"{kind_label} is outside allowed roots")
    if not resolved.exists():
        raise HTTPException(status_code=404, detail=f"{kind_label} not found")
    if not resolved.is_file():
        raise HTTPException(status_code=400, detail=f"{kind_label} is not a file")
    return resolved


def normalize_writable_file_path(
    raw_path: str,
    allowed_roots: list[str],
    *,
    kind_label: str = "Path",
) -> Path:
    """Resolve and validate a writable file path within allowed roots.

    Existing symlink targets are rejected so writes cannot be redirected
    outside the allowlisted roots by planting a symlink inside an allowed
    directory.
    """
    try:
        candidate = Path(raw_path).expanduser()
        resolved_parent = candidate.parent.resolve()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Invalid {kind_label.lower()}") from exc

    if candidate.name in {"", ".", ".."}:
        raise HTTPException(status_code=400, detail=f"Invalid {kind_label.lower()} filename")

    if not resolved_parent.exists():
        raise HTTPException(status_code=404, detail=f"{kind_label} parent does not exist")
    if not resolved_parent.is_dir():
        raise HTTPException(status_code=400, detail=f"{kind_label} parent is not a directory")
    if not is_path_within_roots(resolved_parent, allowed_roots):
        raise HTTPException(status_code=403, detail=f"{kind_label} is outside allowed roots")
    target = resolved_parent / candidate.name
    if target.exists():
        if target.is_symlink():
            raise HTTPException(status_code=400, detail=f"{kind_label} must not be a symlink")
        resolved_target = target.resolve()
        if not is_path_within_roots(resolved_target, allowed_roots):
            raise HTTPException(status_code=403, detail=f"{kind_label} is outside allowed roots")
        if resolved_target.is_dir():
            raise HTTPException(status_code=400, detail=f"{kind_label} is a directory")

    return target
