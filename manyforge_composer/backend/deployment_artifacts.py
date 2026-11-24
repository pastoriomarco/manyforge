"""Bounded, descriptor-owned Deployment artifact reads."""

from __future__ import annotations

import os
from pathlib import Path
import stat
from typing import NoReturn

from fastapi import HTTPException

from .deployment_draft import DeploymentDraftError
from .security import (
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
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
