"""Descriptor-bound Program reads and atomic writes for Composer authoring."""

from __future__ import annotations

import errno
import logging
import os
from pathlib import Path
import stat
from typing import NoReturn
import uuid

from fastapi import HTTPException
import yaml

from manyforge_behavior.canonical_artifact import (
    MAX_DOCUMENT_BYTES,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program, ProgramValidationError

from .security import (
    SCENARIO_PROJECT_READ_ONLY_MESSAGE,
    inside_untitled_scenario,
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
    normalize_writable_file_path,
)


_PROGRAM_SUFFIXES = (".program.yaml", ".program.yml")
_LOGGER = logging.getLogger(__name__)


class ProgramArtifactError(RuntimeError):
    """One bounded Program artifact operation failed."""

    def __init__(self, code: str, message: str, *, status_code: int) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


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


def normalize_program_save_path(raw_path: str) -> str:
    """Append the default Program suffix when the selected name has none."""

    try:
        candidate = Path(raw_path).expanduser()
        if not candidate.name:
            raise ValueError("Program path has no file name")
        if _has_program_suffix(candidate):
            return str(candidate)
        return str(candidate.with_name(f"{candidate.name}.program.yaml"))
    except (RuntimeError, TypeError, ValueError):
        _fail(
            "program_artifact_path_invalid",
            "invalid Program artifact path",
            status_code=400,
        )


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
            loaded = Program(
                f"/proc/self/fd/{descriptor}", catalog, authoring_load=True
            )
        except ProgramValidationError as error:
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


def _serialized_program(program: Program) -> bytes:
    try:
        text = yaml.safe_dump(
            program.to_dict(),
            allow_unicode=True,
            default_flow_style=False,
            sort_keys=False,
            width=100,
        )
        raw = text.encode("utf-8")
        if len(raw) > MAX_DOCUMENT_BYTES:
            _fail(
                "program_artifact_oversized",
                "serialized Program exceeds the artifact byte bound",
                status_code=413,
            )
        return raw
    except ProgramArtifactError:
        raise
    except (UnicodeError, yaml.YAMLError) as error:
        _fail(
            "program_artifact_serialization_failed",
            f"cannot serialize Program artifact: {error}",
            status_code=500,
        )


def _attest_parent(descriptor: int, expected: Path, roots: list[str]) -> None:
    try:
        metadata = os.fstat(descriptor)
    except OSError as error:
        _fail(
            "program_artifact_changed",
            f"cannot inspect Program artifact directory: {error}",
            status_code=409,
        )
    actual = _descriptor_path(descriptor, label="Program artifact directory")
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or actual != expected
        or not is_path_within_roots(actual, roots)
    ):
        _fail(
            "program_artifact_changed",
            "Program artifact directory changed or escaped its configured root",
            status_code=409,
        )


def _write_all(descriptor: int, raw: bytes) -> None:
    offset = 0
    while offset < len(raw):
        try:
            written = os.write(descriptor, raw[offset:])
        except InterruptedError:
            continue
        if written <= 0:
            _fail(
                "program_artifact_write_failed",
                "Program artifact write made no progress",
                status_code=500,
            )
        offset += written


def save_program_artifact(
    program: Program,
    raw_path: str,
    *,
    allowed_roots: list[str],
    overwrite: bool,
) -> str:
    """Atomically persist one exact Program inside an attested allowed root."""

    roots = _roots(allowed_roots)
    normalized_path = normalize_program_save_path(raw_path)
    try:
        target = normalize_writable_file_path(
            normalized_path,
            roots,
            kind_label="Program artifact",
        )
    except HTTPException as error:
        _from_http(error)
    if inside_untitled_scenario(target):
        _fail("scenario_project_read_only", SCENARIO_PROJECT_READ_ONLY_MESSAGE, status_code=409)
    raw = _serialized_program(program)
    parent = target.parent.resolve()
    try:
        parent_fd = os.open(
            parent,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except FileNotFoundError:
        _fail(
            "program_artifact_parent_not_found",
            "Program artifact parent does not exist",
            status_code=404,
        )
    except PermissionError:
        _fail(
            "program_artifact_permission_denied",
            "permission denied while opening Program artifact directory",
            status_code=403,
        )
    except OSError as error:
        _fail(
            "program_artifact_write_failed",
            f"cannot open Program artifact directory: {error}",
            status_code=400,
        )

    temporary_name = f".{target.name}.tmp-{uuid.uuid4().hex}"
    temporary_exists = False
    temporary_fd = -1
    try:
        _attest_parent(parent_fd, parent, roots)
        existing: os.stat_result | None
        try:
            existing = os.stat(target.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is not None:
            if not stat.S_ISREG(existing.st_mode):
                _fail(
                    "program_artifact_target_invalid",
                    "Program artifact target must be a regular file",
                    status_code=400,
                )
            if not overwrite:
                _fail(
                    "program_artifact_overwrite_confirmation_required",
                    "Save As target already exists and requires explicit confirmation",
                    status_code=409,
                )
        mode = stat.S_IMODE(existing.st_mode) if existing is not None else 0o644
        try:
            temporary_fd = os.open(
                temporary_name,
                os.O_WRONLY
                | os.O_CREAT
                | os.O_EXCL
                | os.O_CLOEXEC
                | os.O_NOFOLLOW,
                mode,
                dir_fd=parent_fd,
            )
            temporary_exists = True
            _write_all(temporary_fd, raw)
            os.fsync(temporary_fd)
            os.close(temporary_fd)
            temporary_fd = -1
            if existing is None and not overwrite:
                try:
                    os.link(
                        temporary_name,
                        target.name,
                        src_dir_fd=parent_fd,
                        dst_dir_fd=parent_fd,
                        follow_symlinks=False,
                    )
                except FileExistsError:
                    _fail(
                        "program_artifact_overwrite_confirmation_required",
                        "Save As target appeared and requires explicit confirmation",
                        status_code=409,
                    )
                os.unlink(temporary_name, dir_fd=parent_fd)
                temporary_exists = False
            else:
                os.replace(
                    temporary_name,
                    target.name,
                    src_dir_fd=parent_fd,
                    dst_dir_fd=parent_fd,
                )
                temporary_exists = False
            os.fsync(parent_fd)
            _attest_parent(parent_fd, parent, roots)
        except ProgramArtifactError:
            raise
        except PermissionError:
            _fail(
                "program_artifact_permission_denied",
                "permission denied while writing Program artifact",
                status_code=403,
            )
        except OSError as error:
            status_code = 409 if error.errno in {errno.EEXIST, errno.ESTALE} else 500
            _fail(
                "program_artifact_write_failed",
                f"cannot atomically write Program artifact: {error}",
                status_code=status_code,
            )
    finally:
        if temporary_fd >= 0:
            os.close(temporary_fd)
        if temporary_exists:
            try:
                os.unlink(temporary_name, dir_fd=parent_fd)
            except OSError:
                pass
        os.close(parent_fd)
    return str(target)


__all__ = [
    "ProgramArtifactError",
    "load_program_artifact",
    "normalize_program_save_path",
    "save_program_artifact",
]
