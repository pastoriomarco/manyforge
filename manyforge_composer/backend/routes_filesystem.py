"""Bounded filesystem browsing for structural artifact selection.

The same-role route is retained from
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` source blob
``5586209deda5b95b899961c88552491771e5d5b0``.  Its authoring response
contract remains current; this version adds descriptor-bound containment and
hard request, scan, sort, and output bounds without becoming an artifact
loader or runtime owner.
"""
from __future__ import annotations

import errno
import json
import os
import stat
from pathlib import Path
from typing import Any, Literal

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .security import normalize_allowed_roots

_PROGRAM_SUFFIXES = (".program.yaml", ".program.yml")
_DEPLOYMENT_SUFFIXES = (".deployment.yaml", ".deployment.yml")
_MESH_SUFFIXES = (".obj", ".stl", ".ply", ".glb", ".gltf")
# A folder with this suffix is a Scenario project (the project marker).
_SCENARIO_PROJECT_SUFFIX = ".scenario"

# Browsing is an authoring convenience, not a bulk filesystem-indexing API.
# These limits bound every caller-controlled dimension before sorting or JSON
# serialization.  Linux filesystems normally impose tighter component limits,
# but the application bounds remain explicit so mocked, remote, or unusual
# filesystems cannot turn one request into unbounded work.
_MAX_BROWSE_PATH_BYTES = 4096
_MAX_BROWSE_NAME_BYTES = 255
_MAX_SCANNED_DIRECTORY_ENTRIES = 4096
_MAX_RETURNED_DIRECTORY_ENTRIES = 1024
_MAX_BROWSE_OUTPUT_BYTES = 1024 * 1024
_MAX_BROWSE_ROOTS = 64

_DIRECTORY_STABILITY_FIELDS = (
    "st_dev",
    "st_ino",
    "st_mode",
    "st_mtime_ns",
    "st_ctime_ns",
)

_BROWSE_DTO_CONFIG = ConfigDict(
    populate_by_name=True,
    extra="forbid",
    allow_inf_nan=False,
)


class FileBrowserEntryResponse(BaseModel):
    """One bounded authoring-browser entry."""

    model_config = _BROWSE_DTO_CONFIG

    name: str = Field(min_length=1, max_length=_MAX_BROWSE_NAME_BYTES)
    path: str = Field(min_length=1, max_length=_MAX_BROWSE_PATH_BYTES)
    entry_type: Literal["directory", "file"] = Field(alias="entryType")
    selectable: bool
    # A directory that is itself a Scenario project folder (``*.scenario``).
    scenario_project: bool = Field(default=False, alias="scenarioProject")

    @model_validator(mode="after")
    def _validate_selectability(self) -> "FileBrowserEntryResponse":
        expected = self.entry_type == "file"
        if self.selectable is not expected:
            raise ValueError("only file entries may be selectable")
        if self.scenario_project and self.entry_type != "directory":
            raise ValueError("only directory entries may be Scenario projects")
        return self


class FileBrowserResponse(BaseModel):
    """Current structural filesystem-browser response contract."""

    model_config = _BROWSE_DTO_CONFIG

    current_path: str = Field(
        alias="currentPath",
        min_length=1,
        max_length=_MAX_BROWSE_PATH_BYTES,
    )
    parent_path: str | None = Field(
        default=None,
        alias="parentPath",
        min_length=1,
        max_length=_MAX_BROWSE_PATH_BYTES,
    )
    roots: list[str] = Field(default_factory=list, max_length=_MAX_BROWSE_ROOTS)
    entries: list[FileBrowserEntryResponse] = Field(
        default_factory=list,
        max_length=_MAX_RETURNED_DIRECTORY_ENTRIES,
    )


def _matches_kind(path: Path, kind: str) -> bool:
    name = path.name.lower()
    if kind == "program":
        return name.endswith(_PROGRAM_SUFFIXES)
    if kind == "deployment":
        return name.endswith(_DEPLOYMENT_SUFFIXES)
    if kind == "mesh":
        return name.endswith(_MESH_SUFFIXES)
    return False


def _is_within_normalized_roots(path: Path, roots: list[str]) -> bool:
    """Check containment without letting a later root symlink redefine trust."""

    for root in roots:
        try:
            path.relative_to(Path(root))
            return True
        except ValueError:
            continue
    return False


def _utf8_size(value: str, *, label: str, limit: int, status_code: int) -> int:
    try:
        size = len(value.encode("utf-8"))
    except UnicodeEncodeError as exc:
        raise HTTPException(
            status_code=400,
            detail=f"{label} is not valid UTF-8",
        ) from exc
    if size > limit:
        raise HTTPException(
            status_code=status_code,
            detail=f"{label} exceeds the browse limit",
        )
    return size


def _validate_path_text(
    value: str,
    *,
    label: str = "Path",
    status_code: int = 414,
) -> None:
    _utf8_size(
        value,
        label=label,
        limit=_MAX_BROWSE_PATH_BYTES,
        status_code=status_code,
    )


def _normalize_browse_path(raw_path: str, allowed_roots: list[str]) -> Path:
    _validate_path_text(raw_path)
    try:
        candidate = Path(raw_path).expanduser()
        resolved = candidate.resolve(strict=True)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail="Path not found") from exc
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail="Permission denied") from exc
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=400, detail="Invalid path") from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Filesystem error") from exc

    _validate_path_text(str(resolved))

    if not _is_within_normalized_roots(resolved, allowed_roots):
        raise HTTPException(status_code=403, detail="Path is outside allowed roots")

    try:
        metadata = resolved.stat()
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail="Path not found") from exc
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail="Permission denied") from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Filesystem error") from exc

    if stat.S_ISREG(metadata.st_mode):
        resolved = resolved.parent
        if not _is_within_normalized_roots(resolved, allowed_roots):
            raise HTTPException(status_code=403, detail="Path is outside allowed roots")
    elif not stat.S_ISDIR(metadata.st_mode):
        raise HTTPException(status_code=400, detail="Not a directory")
    return resolved


def _root_shortcuts(allowed_roots: list[str]) -> list[str]:
    roots: list[str] = []

    def add_candidate(path: Path) -> None:
        if len(roots) >= _MAX_BROWSE_ROOTS:
            return
        try:
            resolved = path.resolve(strict=True)
            metadata = resolved.stat()
        except (OSError, RuntimeError, ValueError):
            return
        if not stat.S_ISDIR(metadata.st_mode):
            return
        if not _is_within_normalized_roots(resolved, allowed_roots):
            return
        as_str = str(resolved)
        try:
            _validate_path_text(as_str, label="Root path")
        except HTTPException:
            return
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

    # Only add allowed roots as shortcuts (not the filesystem root)
    for root in allowed_roots:
        add_candidate(Path(root))

    return roots


def _directory_fd_path(descriptor: int) -> Path:
    """Resolve the directory actually held by *descriptor* through procfs."""

    try:
        resolved = Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
    except (OSError, RuntimeError, ValueError) as exc:
        raise HTTPException(
            status_code=409,
            detail="Directory changed while being browsed",
        ) from exc
    _validate_path_text(str(resolved), label="Directory path")
    return resolved


def _attest_directory(
    descriptor: int,
    current_dir: Path,
    allowed_roots: list[str],
    *,
    expected: os.stat_result | None = None,
) -> os.stat_result:
    """Bind one browse snapshot to the checked pathname and stable inode."""

    try:
        metadata = os.fstat(descriptor)
        path_metadata = os.stat(current_dir, follow_symlinks=False)
    except OSError as exc:
        raise HTTPException(
            status_code=409,
            detail="Directory changed while being browsed",
        ) from exc

    actual_path = _directory_fd_path(descriptor)
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or not stat.S_ISDIR(path_metadata.st_mode)
        or (metadata.st_dev, metadata.st_ino)
        != (path_metadata.st_dev, path_metadata.st_ino)
        or actual_path != current_dir
        or not _is_within_normalized_roots(actual_path, allowed_roots)
    ):
        raise HTTPException(
            status_code=409,
            detail="Directory changed while being browsed",
        )

    if expected is not None and any(
        getattr(metadata, field) != getattr(expected, field)
        for field in _DIRECTORY_STABILITY_FIELDS
    ):
        raise HTTPException(
            status_code=409,
            detail="Directory changed while being browsed",
        )
    return metadata


def _open_directory(
    current_dir: Path,
    allowed_roots: list[str],
) -> tuple[int, os.stat_result]:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        descriptor = os.open(current_dir, flags)
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail="Permission denied") from exc
    except OSError as exc:
        if exc.errno in {
            errno.ENOENT,
            errno.ELOOP,
            errno.ENOTDIR,
            errno.ESTALE,
        }:
            raise HTTPException(
                status_code=409,
                detail="Directory changed while being browsed",
            ) from exc
        raise HTTPException(status_code=500, detail="Filesystem error") from exc

    try:
        metadata = _attest_directory(descriptor, current_dir, allowed_roots)
    except Exception:
        os.close(descriptor)
        raise
    return descriptor, metadata


def _open_default_browse_root(
    allowed_roots: list[str],
) -> tuple[Path, int, os.stat_result]:
    """Open the first configured root that is still an accessible directory."""

    for root in allowed_roots:
        current_dir = Path(root)
        try:
            descriptor, metadata = _open_directory(current_dir, allowed_roots)
        except HTTPException:
            continue
        return current_dir, descriptor, metadata
    raise HTTPException(
        status_code=503,
        detail="No accessible filesystem root",
    )


def _entry_payload(current_dir: Path, name: str, *, is_dir: bool) -> dict[str, Any]:
    _utf8_size(
        name,
        label="Directory entry name",
        limit=_MAX_BROWSE_NAME_BYTES,
        status_code=413,
    )
    path = str(current_dir / name)
    _validate_path_text(
        path,
        label="Directory entry path",
        status_code=413,
    )
    return {
        "name": name,
        "path": path,
        "entryType": "directory" if is_dir else "file",
        "selectable": not is_dir,
        "scenarioProject": is_dir and name.endswith(_SCENARIO_PROJECT_SUFFIX),
    }


def _scan_directory(
    descriptor: int,
    current_dir: Path,
    kind: str,
) -> list[dict[str, Any]]:
    """Collect one bounded directory page without materializing all names."""

    directory_entries: list[dict[str, Any]] = []
    file_entries: list[dict[str, Any]] = []
    scanned_entries = 0

    try:
        with os.scandir(descriptor) as iterator:
            for child in iterator:
                scanned_entries += 1
                if scanned_entries > _MAX_SCANNED_DIRECTORY_ENTRIES:
                    raise HTTPException(
                        status_code=413,
                        detail="Directory exceeds the browse entry limit",
                    )

                name = child.name
                _utf8_size(
                    name,
                    label="Directory entry name",
                    limit=_MAX_BROWSE_NAME_BYTES,
                    status_code=413,
                )
                try:
                    if child.is_symlink():
                        continue
                    is_dir = child.is_dir(follow_symlinks=False)
                    is_file = child.is_file(follow_symlinks=False)
                except OSError as exc:
                    raise HTTPException(
                        status_code=500,
                        detail="Filesystem error",
                    ) from exc

                if not is_dir and not (is_file and _matches_kind(Path(name), kind)):
                    continue
                if (
                    len(directory_entries) + len(file_entries)
                    >= _MAX_RETURNED_DIRECTORY_ENTRIES
                ):
                    raise HTTPException(
                        status_code=413,
                        detail="Directory exceeds the returned entry limit",
                    )
                payload = _entry_payload(current_dir, name, is_dir=is_dir)
                if is_dir:
                    directory_entries.append(payload)
                else:
                    file_entries.append(payload)
    except HTTPException:
        raise
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail="Permission denied") from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Filesystem error") from exc

    # Both lists are capped before reaching this point, so sorting cannot grow
    # with an untrusted directory beyond the declared response-entry limit.
    sort_key = lambda entry: (entry["name"].casefold(), entry["name"])
    directory_entries.sort(key=sort_key)
    file_entries.sort(key=sort_key)
    return [*directory_entries, *file_entries]


def _bounded_browse_payload(payload: dict[str, Any]) -> dict[str, Any]:
    try:
        # Match Starlette's compact UTF-8 JSON rendering for this
        # string/bool/null-only response.
        encoded = json.dumps(
            payload,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as exc:
        raise HTTPException(status_code=500, detail="Filesystem error") from exc
    if len(encoded) > _MAX_BROWSE_OUTPUT_BYTES:
        raise HTTPException(
            status_code=413,
            detail="Filesystem browse response exceeds the output limit",
        )
    return payload


def create_filesystem_router(allowed_roots: list[str] | None = None) -> APIRouter:
    if allowed_roots is not None:
        if type(allowed_roots) is not list:
            raise ValueError("filesystem browser allowed roots must be a list")
        if len(allowed_roots) > _MAX_BROWSE_ROOTS:
            raise ValueError("filesystem browser has too many allowed roots")
        for root in allowed_roots:
            if type(root) is not str:
                raise ValueError(
                    "filesystem browser allowed roots must be strings"
                )
            try:
                _validate_path_text(root, label="Allowed root")
            except HTTPException as exc:
                raise ValueError(
                    "filesystem browser allowed root is invalid"
                ) from exc
    roots = normalize_allowed_roots(allowed_roots)
    if not roots:
        raise ValueError("filesystem browser has no valid allowed root")
    if len(roots) > _MAX_BROWSE_ROOTS:
        raise ValueError("filesystem browser has too many allowed roots")
    for root in roots:
        try:
            _validate_path_text(root, label="Allowed root")
        except HTTPException as exc:
            raise ValueError("filesystem browser allowed root is invalid") from exc
    router = APIRouter()

    @router.get(
        "/browse",
        response_model=FileBrowserResponse,
        response_model_by_alias=True,
    )
    def browse_filesystem(
        # The byte-exact check stays inside the handler so an oversized value
        # gets one bounded error instead of a validation body echoing the path.
        path: str | None = Query(default=None),
        # ``scenario`` lists folders only, marking Scenario project folders.
        kind: str = Query(
            default="program", pattern="^(program|deployment|mesh|scenario)$"
        ),
    ) -> dict:
        if path is None:
            current_dir, descriptor, initial_metadata = _open_default_browse_root(
                roots
            )
        elif not path.strip():
            _validate_path_text(path)
            current_dir, descriptor, initial_metadata = _open_default_browse_root(
                roots
            )
        else:
            current_dir = _normalize_browse_path(path, roots)
            descriptor, initial_metadata = _open_directory(current_dir, roots)
        try:
            entries = _scan_directory(descriptor, current_dir, kind)
            _attest_directory(
                descriptor,
                current_dir,
                roots,
                expected=initial_metadata,
            )
        finally:
            os.close(descriptor)

        parent_path = None
        parent_dir = current_dir.parent
        if parent_dir != current_dir and _is_within_normalized_roots(
            parent_dir, roots
        ):
            parent_path = str(parent_dir)

        return _bounded_browse_payload(
            {
                "currentPath": str(current_dir),
                "parentPath": parent_path,
                "roots": _root_shortcuts(roots),
                "entries": entries,
            }
        )

    return router
