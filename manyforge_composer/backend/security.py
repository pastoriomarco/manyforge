"""Bounded transport and filesystem configuration for ManyForge Composer."""

from __future__ import annotations

import os
import stat
from dataclasses import dataclass, field
from pathlib import Path

from fastapi import HTTPException

# Maximum WebSocket text message size (bytes).
WS_MAX_MESSAGE_SIZE = 256


@dataclass
class SecurityConfig:
    """Transport and filesystem safety settings for the Composer backend.

    Composer ingress is deliberately open and attributed. Authentication, if
    a deployment needs it, belongs in an external trusted adapter and cannot
    become ManyForge caller admission.
    """

    # CORS allowed origins.  Empty list → deny all cross-origin requests.
    cors_origins: list[str] = field(default_factory=lambda: ["http://localhost:3000"])

    # Filesystem roots that the file-browser endpoint is allowed to traverse.
    # Paths outside these roots (after symlink resolution) are rejected.
    allowed_fs_roots: list[str] = field(default_factory=lambda: ["/workspace"])

    # Program-artifact write roots.  ``None`` preserves the historical
    # behavior of using ``allowed_fs_roots`` for writes; an explicit empty
    # list is a fail-closed configuration.
    allowed_program_write_roots: list[str] | None = None

    @classmethod
    def from_env(cls) -> SecurityConfig:
        """Build the CORS and filesystem bounds from environment variables."""

        def _csv(var: str) -> list[str] | None:
            if var not in os.environ:
                return None
            raw = os.environ[var]
            items = [s.strip() for s in raw.split(",") if s.strip()]
            return items

        program_write_roots = _csv("MANYFORGE_PROGRAM_WRITE_ROOTS")
        return cls(
            cors_origins=_csv("MANYFORGE_CORS_ORIGINS") or ["http://localhost:3000"],
            allowed_fs_roots=_csv("MANYFORGE_FS_ROOTS") or ["/workspace"],
            allowed_program_write_roots=program_write_roots,
        )


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
    if roots is None:
        return ["/workspace"]

    normalized: list[str] = []
    for root in roots:
        # ``Path("")`` means the current working directory.  Treating an
        # explicitly empty configured root that way would silently widen a
        # malformed allowlist to whichever directory happens to launch the
        # server.
        if type(root) is not str or not root or "\x00" in root:
            continue
        try:
            resolved = Path(root).expanduser().resolve()
        except Exception:
            continue
        root_str = str(resolved)
        if root_str not in normalized:
            normalized.append(root_str)

    # An explicit empty or wholly invalid allowlist must not silently widen to
    # the development default. The route constructor rejects this empty result
    # before serving a filesystem request.
    return normalized


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
        raise HTTPException(
            status_code=400, detail=f"Invalid {kind_label.lower()}"
        ) from exc

    if not is_path_within_roots(resolved, allowed_roots):
        raise HTTPException(
            status_code=403, detail=f"{kind_label} is outside allowed roots"
        )
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
    """Perform lexical preflight for a prospective file path.

    Existing symlinks and non-regular filesystem objects are rejected.  The
    returned ``Path`` is not a write capability: a later write must still use
    descriptor-relative no-follow creation/opening and attest the opened
    inode, because another process can replace any pathname after this check.
    """
    try:
        candidate = Path(raw_path).expanduser()
        resolved_parent = candidate.parent.resolve()
    except Exception as exc:
        raise HTTPException(
            status_code=400, detail=f"Invalid {kind_label.lower()}"
        ) from exc

    if candidate.name in {"", ".", ".."}:
        raise HTTPException(
            status_code=400, detail=f"Invalid {kind_label.lower()} filename"
        )

    if not resolved_parent.exists():
        raise HTTPException(
            status_code=404, detail=f"{kind_label} parent does not exist"
        )
    if not resolved_parent.is_dir():
        raise HTTPException(
            status_code=400, detail=f"{kind_label} parent is not a directory"
        )
    if not is_path_within_roots(resolved_parent, allowed_roots):
        raise HTTPException(
            status_code=403, detail=f"{kind_label} is outside allowed roots"
        )
    target = resolved_parent / candidate.name
    try:
        target_metadata = target.lstat()
    except FileNotFoundError:
        return target
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail="Permission denied") from exc
    except OSError as exc:
        raise HTTPException(
            status_code=400,
            detail=f"Invalid {kind_label.lower()}",
        ) from exc

    if stat.S_ISLNK(target_metadata.st_mode):
        # ``lstat`` makes this include dangling symlinks, for which
        # ``Path.exists()`` would otherwise be false.
        raise HTTPException(
            status_code=400, detail=f"{kind_label} must not be a symlink"
        )
    if not stat.S_ISREG(target_metadata.st_mode):
        raise HTTPException(
            status_code=400,
            detail=f"{kind_label} must be a regular file",
        )
    try:
        resolved_target = target.resolve(strict=True)
    except FileNotFoundError as exc:
        raise HTTPException(
            status_code=409,
            detail=f"{kind_label} changed during lexical preflight",
        ) from exc
    except OSError as exc:
        raise HTTPException(
            status_code=400,
            detail=f"Invalid {kind_label.lower()}",
        ) from exc
    if not is_path_within_roots(resolved_target, allowed_roots):
        raise HTTPException(
            status_code=403, detail=f"{kind_label} is outside allowed roots"
        )

    return target
