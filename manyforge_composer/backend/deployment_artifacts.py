"""Bounded, descriptor-owned Deployment artifact reads and writes."""

from __future__ import annotations

import os
from pathlib import Path
import stat
from typing import NoReturn
import uuid

from fastapi import HTTPException

from .deployment_draft import DeploymentDraftError
from .security import (
    SCENARIO_PROJECT_READ_ONLY_MESSAGE,
    inside_untitled_scenario,
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
    normalize_writable_file_path,
)


_SUFFIXES = (".deployment.yaml", ".deployment.yml")
_MAX_BYTES = 16 * 1024 * 1024


def _fail(code: str, message: str, *, status_code: int) -> NoReturn:
    raise DeploymentDraftError(code, message, status_code=status_code)


def _roots(values: list[str]) -> list[str]:
    roots = normalize_allowed_roots(values)
    if not roots:
        _fail(
            "deployment_artifact_roots_unavailable",
            "no Deployment artifact root is configured",
            status_code=503,
        )
    return roots


def normalize_deployment_save_path(raw_path: str) -> str:
    try:
        path = Path(raw_path).expanduser()
        if not path.name:
            raise ValueError
        return str(path) if path.name.lower().endswith(_SUFFIXES) else str(
            path.with_name(path.name + ".deployment.yaml")
        )
    except (TypeError, ValueError, RuntimeError):
        _fail(
            "deployment_artifact_path_invalid",
            "invalid Deployment artifact path",
            status_code=400,
        )


def load_deployment_artifact(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[str, str]:
    path = Path(raw_path)
    if not path.name.lower().endswith(_SUFFIXES):
        _fail(
            "deployment_artifact_suffix_invalid",
            "Deployment artifact must end in .deployment.yaml or .deployment.yml",
            status_code=400,
        )
    roots = _roots(allowed_roots)
    try:
        resolved = normalize_existing_file_path(
            raw_path, roots, kind_label="Deployment artifact"
        )
    except HTTPException as error:
        if error.status_code == 404:
            _fail(
                "deployment_artifact_not_found",
                "Deployment artifact not found",
                status_code=404,
            )
        _fail(
            "deployment_artifact_path_invalid",
            str(error.detail),
            status_code=error.status_code,
        )
    try:
        descriptor = os.open(resolved, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except FileNotFoundError:
        _fail("deployment_artifact_not_found", "Deployment artifact not found", status_code=404)
    except PermissionError:
        _fail("deployment_artifact_permission_denied", "permission denied", status_code=403)
    except OSError as error:
        _fail("deployment_artifact_read_failed", str(error), status_code=400)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > _MAX_BYTES:
            _fail(
                "deployment_artifact_invalid",
                "Deployment artifact must be one non-empty bounded regular file",
                status_code=400,
            )
        actual = Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
        if actual != resolved or not is_path_within_roots(actual, roots):
            _fail("deployment_artifact_changed", "Deployment artifact changed", status_code=409)
        raw = bytearray()
        while len(raw) < before.st_size:
            chunk = os.read(descriptor, before.st_size - len(raw))
            if not chunk:
                break
            raw.extend(chunk)
        after = os.fstat(descriptor)
        if len(raw) != before.st_size or (
            before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns
        ) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            _fail("deployment_artifact_changed", "Deployment artifact changed", status_code=409)
        try:
            source = raw.decode("utf-8")
        except UnicodeError as error:
            _fail("deployment_artifact_invalid", str(error), status_code=400)
        return source, str(resolved)
    finally:
        os.close(descriptor)


def save_deployment_artifact(
    canonical_json: str,
    raw_path: str,
    *,
    allowed_roots: list[str],
    overwrite: bool,
) -> str:
    raw = (canonical_json + "\n").encode("utf-8")
    if len(raw) > _MAX_BYTES:
        _fail("deployment_artifact_oversized", "Deployment artifact is oversized", status_code=413)
    roots = _roots(allowed_roots)
    normalized = normalize_deployment_save_path(raw_path)
    try:
        target = normalize_writable_file_path(
            normalized, roots, kind_label="Deployment artifact"
        )
    except HTTPException as error:
        _fail(
            "deployment_artifact_path_invalid",
            str(error.detail),
            status_code=error.status_code,
        )
    if inside_untitled_scenario(target):
        _fail("scenario_project_read_only", SCENARIO_PROJECT_READ_ONLY_MESSAGE, status_code=409)
    parent = target.parent.resolve()
    try:
        parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError as error:
        _fail("deployment_artifact_write_failed", str(error), status_code=400)
    temporary = f".{target.name}.tmp-{uuid.uuid4().hex}"
    temporary_exists = False
    descriptor = -1
    try:
        actual_parent = Path(f"/proc/self/fd/{parent_fd}").resolve(strict=True)
        if actual_parent != parent or not is_path_within_roots(actual_parent, roots):
            _fail("deployment_artifact_changed", "Deployment directory changed", status_code=409)
        try:
            existing = os.stat(target.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is not None:
            if not stat.S_ISREG(existing.st_mode):
                _fail("deployment_artifact_target_invalid", "target is not a regular file", status_code=400)
            if not overwrite:
                _fail(
                    "deployment_artifact_overwrite_confirmation_required",
                    "Save As target exists and requires confirmation",
                    status_code=409,
                )
        mode = stat.S_IMODE(existing.st_mode) if existing is not None else 0o644
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
            dir_fd=parent_fd,
        )
        temporary_exists = True
        view = memoryview(raw)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                _fail("deployment_artifact_write_failed", "write made no progress", status_code=500)
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        if existing is None and not overwrite:
            try:
                os.link(temporary, target.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
            except FileExistsError:
                _fail(
                    "deployment_artifact_overwrite_confirmation_required",
                    "Save As target appeared and requires confirmation",
                    status_code=409,
                )
            os.unlink(temporary, dir_fd=parent_fd)
        else:
            os.replace(temporary, target.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
        temporary_exists = False
        os.fsync(parent_fd)
        return str(target)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary_exists:
            try:
                os.unlink(temporary, dir_fd=parent_fd)
            except OSError:
                pass
        os.close(parent_fd)
