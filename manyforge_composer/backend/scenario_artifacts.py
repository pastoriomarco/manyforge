"""Editable Scenario documents with atomic, sandboxed persistence."""

from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import re
import stat
from typing import Any, NoReturn
import uuid

from fastapi import HTTPException

from manyforge_behavior.canonical_artifact import (
    MAX_DOCUMENT_BYTES,
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml,
    raw_sha256_digest,
)

from .security import (
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
    normalize_writable_file_path,
)


SCENARIO_API_VERSION = "manyforge.scenario.authoring.v1"
SCENARIO_SUFFIXES = (".scenario.yaml", ".scenario.yml")
_SCENARIO_ID = re.compile(r"^[a-z][a-z0-9._-]{0,127}$")
_STATUS_VALUES = frozenset({"ready", "incomplete", "unknown"})


class ScenarioArtifactError(RuntimeError):
    def __init__(self, code: str, message: str, *, status_code: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


def _fail(code: str, message: str, *, status_code: int = 400) -> NoReturn:
    raise ScenarioArtifactError(code, message, status_code=status_code)


def _roots(values: list[str]) -> list[str]:
    roots = normalize_allowed_roots(values)
    if not roots:
        _fail(
            "scenario_artifact_roots_unavailable",
            "no Scenario directory is configured",
            status_code=503,
        )
    return roots


def normalize_scenario_save_path(raw_path: str) -> str:
    try:
        path = Path(raw_path).expanduser()
        if not path.name:
            raise ValueError
        if path.name.lower().endswith(SCENARIO_SUFFIXES):
            return str(path)
        return str(path.with_name(path.name + ".scenario.yaml"))
    except (RuntimeError, TypeError, ValueError):
        _fail("scenario_artifact_path_invalid", "invalid Scenario path")


def scenario_content_digest(
    program_draft: dict[str, Any] | None,
    deployment_draft: dict[str, Any] | None,
    base_scenario_id: str | None,
) -> str:
    try:
        return raw_sha256_digest(
            canonical_json_bytes(
                {
                    "program_draft": program_draft,
                    "deployment_draft": deployment_draft,
                    "base_scenario_id": base_scenario_id,
                }
            )
        )
    except StrictArtifactError as error:
        _fail("scenario_artifact_invalid", str(error))


def build_scenario_document(
    *,
    scenario_id: str,
    name: str,
    description: str,
    base_scenario_id: str | None,
    program_draft: dict[str, Any] | None,
    deployment_draft: dict[str, Any] | None,
    manyforge_version: str,
    last_known_status: str,
) -> dict[str, Any]:
    digest = scenario_content_digest(
        program_draft, deployment_draft, base_scenario_id
    )
    document: dict[str, Any] = {
        "api_version": SCENARIO_API_VERSION,
        "scenario_id": scenario_id,
        "name": name,
        "description": description,
        "base_scenario_id": base_scenario_id,
        "program_draft": deepcopy(program_draft),
        "deployment_draft": deepcopy(deployment_draft),
        "saved_status": {
            "manyforge_version": manyforge_version,
            "last_known_status": last_known_status,
            "checked_content_digest": digest,
        },
    }
    return validate_scenario_document(document, source="<scenario-save>")


def validate_scenario_document(
    value: Any,
    *,
    source: str,
) -> dict[str, Any]:
    expected = {
        "api_version",
        "scenario_id",
        "name",
        "description",
        "base_scenario_id",
        "program_draft",
        "deployment_draft",
        "saved_status",
    }
    if type(value) is not dict or set(value) != expected:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: Scenario fields do not match {SCENARIO_API_VERSION}",
        )
    scenario_id = value["scenario_id"]
    name = value["name"]
    description = value["description"]
    base = value["base_scenario_id"]
    program = value["program_draft"]
    deployment = value["deployment_draft"]
    saved = value["saved_status"]
    if value["api_version"] != SCENARIO_API_VERSION:
        _fail(
            "scenario_artifact_version_unsupported",
            f"{source}: unsupported Scenario version {value['api_version']!r}",
        )
    if type(scenario_id) is not str or _SCENARIO_ID.fullmatch(scenario_id) is None:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: scenario_id must be a portable identifier",
        )
    if type(name) is not str or not name.strip() or len(name) > 256:
        _fail("scenario_artifact_invalid", f"{source}: name is invalid")
    if type(description) is not str or len(description) > 4096:
        _fail("scenario_artifact_invalid", f"{source}: description is invalid")
    if base is not None and (
        type(base) is not str or _SCENARIO_ID.fullmatch(base) is None
    ):
        _fail(
            "scenario_artifact_invalid",
            f"{source}: base_scenario_id is invalid",
        )
    if program is not None and type(program) is not dict:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: program_draft must be an object or null",
        )
    if deployment is not None and type(deployment) is not dict:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: deployment_draft must be an object or null",
        )
    if type(saved) is not dict or set(saved) != {
        "manyforge_version",
        "last_known_status",
        "checked_content_digest",
    }:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: saved_status is invalid",
        )
    if (
        type(saved["manyforge_version"]) is not str
        or not saved["manyforge_version"]
        or len(saved["manyforge_version"]) > 128
        or saved["last_known_status"] not in _STATUS_VALUES
        or type(saved["checked_content_digest"]) is not str
        or not re.fullmatch(r"sha256:[0-9a-f]{64}", saved["checked_content_digest"])
    ):
        _fail(
            "scenario_artifact_invalid",
            f"{source}: saved_status fields are invalid",
        )
    try:
        canonical_json_bytes(value)
    except StrictArtifactError as error:
        _fail("scenario_artifact_invalid", f"{source}: {error}")
    return deepcopy(value)


def load_scenario_artifact(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str]:
    path = Path(raw_path)
    if not path.name.lower().endswith(SCENARIO_SUFFIXES):
        _fail(
            "scenario_artifact_suffix_invalid",
            "Scenario file must end in .scenario.yaml or .scenario.yml",
        )
    roots = _roots(allowed_roots)
    try:
        resolved = normalize_existing_file_path(
            raw_path, roots, kind_label="Scenario artifact"
        )
    except HTTPException as error:
        _fail(
            "scenario_artifact_path_invalid",
            str(error.detail),
            status_code=error.status_code,
        )
    try:
        value = load_strict_json_yaml(resolved)
    except StrictArtifactError as error:
        _fail("scenario_artifact_invalid", f"{resolved}: {error}")
    return validate_scenario_document(value, source=str(resolved)), str(resolved)


def save_scenario_artifact(
    document: dict[str, Any],
    raw_path: str,
    *,
    allowed_roots: list[str],
    overwrite: bool,
) -> str:
    normalized_document = validate_scenario_document(
        document, source="<scenario-save>"
    )
    raw = (json.dumps(normalized_document, indent=2, ensure_ascii=False) + "\n").encode(
        "utf-8"
    )
    if len(raw) > MAX_DOCUMENT_BYTES:
        _fail(
            "scenario_artifact_oversized",
            "Scenario file exceeds the document byte limit",
            status_code=413,
        )
    roots = _roots(allowed_roots)
    target_name = normalize_scenario_save_path(raw_path)
    try:
        target = normalize_writable_file_path(
            target_name, roots, kind_label="Scenario artifact"
        )
    except HTTPException as error:
        _fail(
            "scenario_artifact_path_invalid",
            str(error.detail),
            status_code=error.status_code,
        )
    parent = target.parent.resolve()
    try:
        parent_fd = os.open(
            parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
    except OSError as error:
        _fail("scenario_artifact_write_failed", str(error))
    temporary = f".{target.name}.tmp-{uuid.uuid4().hex}"
    descriptor = -1
    temporary_exists = False
    try:
        actual_parent = Path(f"/proc/self/fd/{parent_fd}").resolve(strict=True)
        if actual_parent != parent or not is_path_within_roots(actual_parent, roots):
            _fail("scenario_artifact_changed", "Scenario directory changed", status_code=409)
        try:
            existing = os.stat(target.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is not None:
            if not stat.S_ISREG(existing.st_mode):
                _fail(
                    "scenario_artifact_target_invalid",
                    "Scenario target is not a regular file",
                )
            if not overwrite:
                _fail(
                    "scenario_artifact_overwrite_confirmation_required",
                    "Scenario target exists and requires confirmation",
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
                _fail("scenario_artifact_write_failed", "write made no progress")
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        if existing is None and not overwrite:
            try:
                os.link(
                    temporary,
                    target.name,
                    src_dir_fd=parent_fd,
                    dst_dir_fd=parent_fd,
                )
            except FileExistsError:
                _fail(
                    "scenario_artifact_overwrite_confirmation_required",
                    "Scenario target appeared and requires confirmation",
                    status_code=409,
                )
            os.unlink(temporary, dir_fd=parent_fd)
        else:
            os.replace(
                temporary,
                target.name,
                src_dir_fd=parent_fd,
                dst_dir_fd=parent_fd,
            )
        temporary_exists = False
        os.fsync(parent_fd)
        return str(target)
    except ScenarioArtifactError:
        raise
    except OSError as error:
        _fail("scenario_artifact_write_failed", str(error))
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary_exists:
            try:
                os.unlink(temporary, dir_fd=parent_fd)
            except OSError:
                pass
        os.close(parent_fd)


def scenario_directory(
    raw_path: str | None,
    *,
    allowed_roots: list[str],
) -> Path:
    roots = _roots(allowed_roots)
    selected = raw_path or roots[0]
    try:
        resolved = Path(selected).expanduser().resolve(strict=True)
    except FileNotFoundError:
        _fail("scenario_directory_not_found", "Scenario directory not found", status_code=404)
    except (OSError, RuntimeError, ValueError) as error:
        _fail("scenario_directory_invalid", str(error))
    if not resolved.is_dir() or resolved.is_symlink():
        _fail("scenario_directory_invalid", "Scenario directory is invalid")
    if not is_path_within_roots(resolved, roots):
        _fail(
            "scenario_directory_outside_roots",
            "Scenario directory is outside configured roots",
            status_code=403,
        )
    return resolved


def discover_scenario_artifacts(
    raw_directory: str | None,
    *,
    allowed_roots: list[str],
    manyforge_version: str,
) -> tuple[str, list[dict[str, Any]]]:
    directory = scenario_directory(raw_directory, allowed_roots=allowed_roots)
    rows: list[dict[str, Any]] = []
    for path in sorted(directory.iterdir(), key=lambda item: item.name.casefold()):
        if path.is_symlink() or not path.is_file() or not path.name.lower().endswith(
            SCENARIO_SUFFIXES
        ):
            continue
        try:
            document, resolved = load_scenario_artifact(
                str(path), allowed_roots=allowed_roots
            )
            saved = document["saved_status"]
            current_digest = scenario_content_digest(
                document["program_draft"],
                document["deployment_draft"],
                document["base_scenario_id"],
            )
            modified = current_digest != saved["checked_content_digest"]
            version_changed = saved["manyforge_version"] != manyforge_version
            display_status = (
                "changed"
                if modified
                else "version_changed"
                if version_changed
                else saved["last_known_status"]
            )
            rows.append(
                {
                    "source_kind": "saved",
                    "path": resolved,
                    "scenario_id": document["scenario_id"],
                    "name": document["name"],
                    "description": document["description"],
                    "base_scenario_id": document["base_scenario_id"],
                    "saved_manyforge_version": saved["manyforge_version"],
                    "display_status": display_status,
                    "status_detail": (
                        "Content changed since the recorded status."
                        if modified
                        else (
                            "Saved by another ManyForge version."
                            if version_changed
                            else (
                                "Configuration was load-ready when last checked."
                                if display_status == "ready"
                                else "Configuration was incomplete when saved."
                                if display_status == "incomplete"
                                else "Configuration has no recorded readiness result."
                            )
                        )
                    ),
                }
            )
        except ScenarioArtifactError as error:
            rows.append(
                {
                    "source_kind": "saved",
                    "path": str(path.resolve()),
                    "scenario_id": None,
                    "name": path.name,
                    "description": "",
                    "base_scenario_id": None,
                    "saved_manyforge_version": None,
                    "display_status": "unsupported",
                    "status_detail": str(error),
                }
            )
    return str(directory), rows
