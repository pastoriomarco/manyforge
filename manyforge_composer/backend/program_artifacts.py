"""Descriptor-bound Program reads for Composer authoring."""

from __future__ import annotations

import logging
import os
from pathlib import Path
import stat
from typing import Any, NoReturn

from fastapi import HTTPException

from manyforge_behavior.canonical_artifact import (
    MAX_DOCUMENT_BYTES, StrictArtifactError, load_strict_json_yaml,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program, ProgramFormatUnsupported, ProgramValidationError,
    require_program_format,
)

from .security import (
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
)


_PROGRAM_SUFFIXES = (".program.yaml", ".program.yml")
_LOGGER = logging.getLogger(__name__)


class ProgramArtifactError(RuntimeError):
    """One bounded Program artifact operation failed."""

    def __init__(self, code: str, message: str, *, status_code: int,
                 details: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.details = details


def _fail(code: str, message: str, *, status_code: int) -> NoReturn:
    raise ProgramArtifactError(code, message, status_code=status_code)


def _from_http(error: HTTPException) -> NoReturn:
    detail = error.detail if isinstance(error.detail, str) else "invalid Program path"
    _fail("program_artifact_path_invalid", detail, status_code=error.status_code)


def _roots(allowed_roots: list[str]) -> list[str]:
    normalized = normalize_allowed_roots(allowed_roots)
    if not normalized:
        _fail(
            "program_artifact_roots_unavailable",
            "no valid Program artifact root is configured",
            status_code=503,
        )
    return normalized


def _has_program_suffix(path: Path) -> bool:
    return path.name.lower().endswith(_PROGRAM_SUFFIXES)


def _descriptor_path(descriptor: int, *, label: str) -> Path:
    try:
        return Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
    except (OSError, RuntimeError, ValueError):
        _fail(
            "program_artifact_changed",
            f"{label} changed during the artifact operation",
            status_code=409,
        )


def _regular_descriptor_state(descriptor: int) -> os.stat_result:
    try:
        metadata = os.fstat(descriptor)
    except OSError as error:
        _fail(
            "program_artifact_read_failed",
            f"cannot inspect Program artifact: {error}",
            status_code=500,
        )
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_size <= 0
        or metadata.st_size > MAX_DOCUMENT_BYTES
    ):
        _fail(
            "program_artifact_invalid",
            "Program artifact must be one non-empty bounded regular file",
            status_code=400,
        )
    return metadata


def load_program_artifact(
    raw_path: str,
    *,
    allowed_roots: list[str],
    catalog: NodeCatalog,
) -> tuple[Program, str]:
    """Read and validate one Program through an attested no-follow descriptor."""

    try:
        candidate = Path(raw_path)
    except (TypeError, ValueError):
        _fail(
            "program_artifact_path_invalid",
            "invalid Program artifact path",
            status_code=400,
        )
    if not _has_program_suffix(candidate):
        _fail(
            "program_artifact_suffix_invalid",
            "Program artifact must end in .program.yaml or .program.yml",
            status_code=400,
        )
    roots = _roots(allowed_roots)
    try:
        resolved = normalize_existing_file_path(
            raw_path,
            roots,
            kind_label="Program artifact",
        )
    except HTTPException as error:
        _from_http(error)
    try:
        descriptor = os.open(
            resolved,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except FileNotFoundError:
        _fail(
            "program_artifact_not_found",
            "Program artifact not found",
            status_code=404,
        )
    except PermissionError:
        _fail(
            "program_artifact_permission_denied",
            "permission denied while opening Program artifact",
            status_code=403,
        )
    except OSError as error:
        _fail(
            "program_artifact_read_failed",
            f"cannot open Program artifact: {error}",
            status_code=400,
        )
    try:
        actual = _descriptor_path(descriptor, label="Program artifact")
        if actual != resolved or not is_path_within_roots(actual, roots):
            _fail(
                "program_artifact_changed",
                "Program artifact changed or escaped its configured root",
                status_code=409,
            )
        before = _regular_descriptor_state(descriptor)
        try:
            # An authored file opened for editing: a stale self-identity is
            # an `edited_outside_manyforge` warning, not a refusal, and a
            # compatible older catalog selector loads as the active one.
            raw = load_strict_json_yaml(f"/proc/self/fd/{descriptor}")
            require_program_format(raw, supported_api_version="manyforge.program.v4")
            loaded = Program.from_dict(raw, catalog, path=resolved, authoring_load=True)
        except ProgramFormatUnsupported as error:
            raise ProgramArtifactError(
                error.code, str(error), status_code=400, details=error.details
            ) from error
        except (ProgramValidationError, StrictArtifactError) as error:
            _fail(
                "program_artifact_validation_failed",
                str(error),
                status_code=400,
            )
        after = _regular_descriptor_state(descriptor)
        if (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        ):
            _fail(
                "program_artifact_changed",
                f"Program artifact changed while being read: {resolved}",
                status_code=409,
            )
        program = Program.from_dict(loaded.to_dict(), catalog, path=resolved)
        # The reload sees the computed identity; keep this read's diagnostics.
        program.load_diagnostics = loaded.load_diagnostics
        for diagnostic in loaded.load_diagnostics:
            _LOGGER.warning(
                "%s: %s: %s", diagnostic["code"], resolved, diagnostic["message"]
            )
        return program, str(resolved)
    finally:
        os.close(descriptor)


__all__ = [
    "ProgramArtifactError",
    "load_program_artifact",
]
