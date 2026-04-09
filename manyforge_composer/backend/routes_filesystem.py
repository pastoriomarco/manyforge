"""Filesystem browsing routes for Composer artifact selection."""
from __future__ import annotations

from pathlib import Path

from fastapi import APIRouter, HTTPException, Query

from .models import FileBrowserEntryResponse, FileBrowserResponse

_PROGRAM_SUFFIXES = (".program.yaml", ".program.yml")
_DEPLOYMENT_SUFFIXES = (".deployment.yaml", ".deployment.yml")


def _matches_kind(path: Path, kind: str) -> bool:
    name = path.name.lower()
    if kind == "program":
        return name.endswith(_PROGRAM_SUFFIXES)
    if kind == "deployment":
        return name.endswith(_DEPLOYMENT_SUFFIXES)
    return False


def _normalize_browse_path(raw_path: str | None) -> Path:
    candidate = Path(raw_path).expanduser() if raw_path else Path.cwd()
    try:
        resolved = candidate.resolve()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    if not resolved.exists():
        raise HTTPException(status_code=404, detail=f"Path not found: {resolved}")
    if resolved.is_file():
        return resolved.parent
    if not resolved.is_dir():
        raise HTTPException(status_code=400, detail=f"Not a directory: {resolved}")
    return resolved


def _root_shortcuts() -> list[str]:
    roots: list[str] = []

    def add_candidate(path: Path) -> None:
        try:
            resolved = path.resolve()
        except Exception:
            return
        if not resolved.exists() or not resolved.is_dir():
            return
        as_str = str(resolved)
        if as_str not in roots:
            roots.append(as_str)

    add_candidate(Path.cwd())
    add_candidate(Path("/workspace"))

    try:
        home = Path.home().resolve()
    except Exception:
        home = None
    if home is not None and str(home) != "/root":
        add_candidate(home)

    add_candidate(Path("/"))
    return roots


def create_filesystem_router() -> APIRouter:
    router = APIRouter()

    @router.get("/browse", response_model=FileBrowserResponse, response_model_by_alias=True)
    def browse_filesystem(
        path: str | None = Query(default=None),
        kind: str = Query(default="program", pattern="^(program|deployment)$"),
    ) -> dict:
        current_dir = _normalize_browse_path(path)

        try:
            children = list(current_dir.iterdir())
        except PermissionError as exc:
            raise HTTPException(status_code=403, detail=str(exc)) from exc
        except OSError as exc:
            raise HTTPException(status_code=500, detail=str(exc)) from exc

        directory_entries: list[dict] = []
        file_entries: list[dict] = []
        for child in children:
            try:
                is_dir = child.is_dir()
            except OSError:
                continue

            if is_dir:
                directory_entries.append(
                    FileBrowserEntryResponse(
                        name=child.name,
                        path=str(child),
                        entryType="directory",
                        selectable=False,
                    ).model_dump(by_alias=True)
                )
                continue

            if _matches_kind(child, kind):
                file_entries.append(
                    FileBrowserEntryResponse(
                        name=child.name,
                        path=str(child),
                        entryType="file",
                        selectable=True,
                    ).model_dump(by_alias=True)
                )

        directory_entries.sort(key=lambda entry: entry["name"].casefold())
        file_entries.sort(key=lambda entry: entry["name"].casefold())

        parent_path = None if current_dir.parent == current_dir else str(current_dir.parent)

        return FileBrowserResponse(
            currentPath=str(current_dir),
            parentPath=parent_path,
            roots=_root_shortcuts(),
            entries=[*directory_entries, *file_entries],
        ).model_dump(by_alias=True)

    return router
