"""Editable Scenario documents with atomic, sandboxed persistence."""

from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shutil
import stat
from typing import Any, NoReturn
import uuid

from fastapi import HTTPException

from manyforge_behavior.canonical_artifact import (
    MAX_DOCUMENT_BYTES,
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml_bytes,
    raw_sha256_digest,
)
from manyforge_behavior.model_source_capture import (
    MAX_ARTIFACT_BYTES,
    MAX_XML_BYTES,
    ModelSourceCaptureError,
    capture_deployment_model_sources,
    decode_model_source_artifact,
)

from .security import (
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
)
from .deployment_intent import (
    DeploymentIntentError,
    rebase_deployment_intent_sources,
    validate_deployment_intent,
)


SCENARIO_API_VERSION = "manyforge.scenario.authoring.v2"
SCENARIO_SUFFIXES = (".scenario.yaml", ".scenario.yml")
SCENARIO_PROJECT_API_VERSION = "manyforge.scenario.project.v1"
SCENARIO_MANIFEST = "scenario.yaml"
SCENARIO_PROJECT_MEMBERS = {
    "deployment_draft": "deployment.yaml",
    "program_draft": "program.yaml",
    "program_role_assignment": "assignment.yaml",
    "typed_parameter_overrides": "overrides.yaml",
}
SCENARIO_SAVE_MARKER = ".manyforge-save-pending.json"
SCENARIO_SOURCE_SNAPSHOT_VERSION = "manyforge.scenario.source_snapshot.v1"
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
        lowered = path.name.lower()
        for suffix in SCENARIO_SUFFIXES:
            if lowered.endswith(suffix):
                return str(path.with_name(path.name[: -len(suffix)]))
        return str(path)
    except (RuntimeError, TypeError, ValueError):
        _fail("scenario_artifact_path_invalid", "invalid Scenario path")


def scenario_content_digest(
    program_draft: dict[str, Any] | None,
    deployment_draft: dict[str, Any] | None,
    program_role_assignment: dict[str, Any] | None,
    typed_parameter_overrides: dict[str, Any],
) -> str:
    try:
        return raw_sha256_digest(
            canonical_json_bytes(
                {
                    "program_draft": program_draft,
                    "deployment_draft": deployment_draft,
                    "program_role_assignment": program_role_assignment,
                    "typed_parameter_overrides": typed_parameter_overrides,
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
    program_draft: dict[str, Any] | None,
    deployment_draft: dict[str, Any] | None,
    program_role_assignment: dict[str, Any] | None,
    typed_parameter_overrides: dict[str, Any],
    manyforge_version: str,
    last_known_status: str,
) -> dict[str, Any]:
    digest = scenario_content_digest(
        program_draft,
        deployment_draft,
        program_role_assignment,
        typed_parameter_overrides,
    )
    document: dict[str, Any] = {
        "api_version": SCENARIO_API_VERSION,
        "scenario_id": scenario_id,
        "name": name,
        "description": description,
        "program_draft": deepcopy(program_draft),
        "deployment_draft": deepcopy(deployment_draft),
        "program_role_assignment": deepcopy(program_role_assignment),
        "typed_parameter_overrides": deepcopy(typed_parameter_overrides),
        "saved_status": {
            "manyforge_version": manyforge_version,
            "last_known_status": last_known_status,
            "checked_content_digest": digest,
        },
    }
    return validate_scenario_document(document, source="<scenario-save>")


def _validate_role_assignment(
    value: Any,
    *,
    source: str,
) -> dict[str, Any] | None:
    if value is None:
        return None
    if type(value) is not dict or set(value) != {"component_roles", "target_roles"}:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: program_role_assignment must contain component_roles and target_roles",
        )
    normalized: dict[str, list[dict[str, str]]] = {}
    for field, role_field, target_field in (
        ("component_roles", "component_role_id", "controllable_component_id"),
        ("target_roles", "target_role_id", "command_target_id"),
    ):
        rows = value[field]
        if type(rows) is not list:
            _fail("scenario_artifact_invalid", f"{source}: {field} must be a list")
        seen: set[str] = set()
        normalized_rows: list[dict[str, str]] = []
        for index, row in enumerate(rows):
            if type(row) is not dict or set(row) != {role_field, target_field}:
                _fail(
                    "scenario_artifact_invalid",
                    f"{source}: {field}[{index}] has invalid fields",
                )
            role = row[role_field]
            target = row[target_field]
            if (
                type(role) is not str
                or not role
                or len(role.encode("utf-8")) > 256
                or type(target) is not str
                or not target
                or len(target.encode("utf-8")) > 256
            ):
                _fail(
                    "scenario_artifact_invalid",
                    f"{source}: {field}[{index}] identifiers are invalid",
                )
            if role in seen:
                _fail(
                    "scenario_artifact_invalid",
                    f"{source}: duplicate {role_field} {role!r}",
                )
            seen.add(role)
            normalized_rows.append({role_field: role, target_field: target})
        normalized[field] = sorted(normalized_rows, key=lambda row: row[role_field])
    return normalized


def _validate_saved_status(value: Any, *, source: str) -> dict[str, str]:
    if type(value) is not dict or set(value) != {
        "manyforge_version",
        "last_known_status",
        "checked_content_digest",
    }:
        _fail("scenario_artifact_invalid", f"{source}: saved_status is invalid")
    if (
        type(value["manyforge_version"]) is not str
        or not value["manyforge_version"]
        or len(value["manyforge_version"]) > 128
        or value["last_known_status"] not in _STATUS_VALUES
        or type(value["checked_content_digest"]) is not str
        or not re.fullmatch(r"sha256:[0-9a-f]{64}", value["checked_content_digest"])
    ):
        _fail("scenario_artifact_invalid", f"{source}: saved_status fields are invalid")
    return deepcopy(value)


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
        "program_draft",
        "deployment_draft",
        "program_role_assignment",
        "typed_parameter_overrides",
        "saved_status",
    }
    if type(value) is not dict:
        _fail("scenario_artifact_invalid", f"{source}: Scenario must be an object")
    if set(value) != expected:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: Scenario fields do not match {SCENARIO_API_VERSION}",
        )
    scenario_id = value["scenario_id"]
    name = value["name"]
    description = value["description"]
    program = value["program_draft"]
    deployment = value["deployment_draft"]
    assignment = _validate_role_assignment(
        value["program_role_assignment"], source=source
    )
    overrides = value["typed_parameter_overrides"]
    saved = _validate_saved_status(value["saved_status"], source=source)
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
    if program is not None and type(program) is not dict:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: program_draft must be an object or null",
        )
    if deployment is not None:
        try:
            deployment = validate_deployment_intent(
                deployment, source=f"{source}: deployment.yaml"
            )
        except DeploymentIntentError as error:
            _fail("scenario_deployment_intent_invalid", str(error))
    if type(overrides) is not dict:
        _fail(
            "scenario_artifact_invalid",
            f"{source}: typed_parameter_overrides must be an object",
        )
    try:
        canonical_json_bytes(value)
    except StrictArtifactError as error:
        _fail("scenario_artifact_invalid", f"{source}: {error}")
    normalized = deepcopy(value)
    normalized["deployment_draft"] = deepcopy(deployment)
    normalized["program_role_assignment"] = assignment
    normalized["typed_parameter_overrides"] = deepcopy(overrides)
    return normalized


def _json_bytes(value: Any, *, label: str) -> bytes:
    try:
        canonical_json_bytes(value)
        raw = (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    except (StrictArtifactError, TypeError, ValueError) as error:
        _fail("scenario_artifact_invalid", f"{label}: {error}")
    if len(raw) > MAX_DOCUMENT_BYTES:
        _fail(
            "scenario_artifact_oversized",
            f"{label} exceeds the document byte limit",
            status_code=413,
        )
    return raw


def _project_payload(
    document: dict[str, Any],
    *,
    source_snapshot: dict[str, Any] | None = None,
) -> tuple[dict[str, bytes], bytes]:
    normalized = validate_scenario_document(document, source="<scenario-save>")
    member_bytes = {
        filename: _json_bytes(normalized[field], label=filename)
        for field, filename in SCENARIO_PROJECT_MEMBERS.items()
    }
    if sum(map(len, member_bytes.values())) > MAX_DOCUMENT_BYTES:
        _fail(
            "scenario_artifact_oversized",
            "Scenario project members exceed the document byte limit",
            status_code=413,
        )
    manifest = {
        "api_version": SCENARIO_PROJECT_API_VERSION,
        "scenario_id": normalized["scenario_id"],
        "name": normalized["name"],
        "description": normalized["description"],
        "content": {
            "deployment_present": normalized["deployment_draft"] is not None,
            "program_present": normalized["program_draft"] is not None,
            "assignment_present": normalized["program_role_assignment"] is not None,
        },
        "members": {
            filename: raw_sha256_digest(raw)
            for filename, raw in sorted(member_bytes.items())
        },
        "source_snapshot": source_snapshot,
        "saved_status": deepcopy(normalized["saved_status"]),
    }
    return member_bytes, _json_bytes(manifest, label=SCENARIO_MANIFEST)


def _validate_project_manifest(value: Any, *, source: str) -> dict[str, Any]:
    expected = {
        "api_version",
        "scenario_id",
        "name",
        "description",
        "content",
        "members",
        "source_snapshot",
        "saved_status",
    }
    if type(value) is not dict or set(value) != expected:
        _fail("scenario_artifact_invalid", f"{source}: invalid Scenario manifest")
    if value["api_version"] != SCENARIO_PROJECT_API_VERSION:
        _fail(
            "scenario_artifact_version_unsupported",
            f"{source}: unsupported Scenario project version {value['api_version']!r}",
        )
    scenario_id = value["scenario_id"]
    if type(scenario_id) is not str or _SCENARIO_ID.fullmatch(scenario_id) is None:
        _fail("scenario_artifact_invalid", f"{source}: scenario_id is invalid")
    if type(value["name"]) is not str or not value["name"].strip() or len(value["name"]) > 256:
        _fail("scenario_artifact_invalid", f"{source}: name is invalid")
    if type(value["description"]) is not str or len(value["description"]) > 4096:
        _fail("scenario_artifact_invalid", f"{source}: description is invalid")
    content = value["content"]
    if type(content) is not dict or set(content) != {
        "deployment_present",
        "program_present",
        "assignment_present",
    } or any(type(item) is not bool for item in content.values()):
        _fail("scenario_artifact_invalid", f"{source}: content flags are invalid")
    members = value["members"]
    expected_members = set(SCENARIO_PROJECT_MEMBERS.values())
    if type(members) is not dict or set(members) != expected_members:
        _fail("scenario_artifact_invalid", f"{source}: member table is invalid")
    if any(
        type(digest) is not str
        or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None
        for digest in members.values()
    ):
        _fail("scenario_artifact_invalid", f"{source}: member digest is invalid")
    snapshot = value["source_snapshot"]
    if snapshot is not None:
        if (
            type(snapshot) is not dict
            or set(snapshot) != {"schema_version", "entries"}
            or snapshot.get("schema_version") != SCENARIO_SOURCE_SNAPSHOT_VERSION
            or type(snapshot.get("entries")) is not list
        ):
            _fail("scenario_artifact_invalid", f"{source}: source snapshot is invalid")
        previous_key = ""
        for entry in snapshot["entries"]:
            if (
                type(entry) is not dict
                or set(entry) != {"content_digest", "source_id"}
                or type(entry.get("source_id")) is not str
                or not entry["source_id"]
                or entry["source_id"] <= previous_key
                or type(entry.get("content_digest")) is not str
                or re.fullmatch(r"sha256:[0-9a-f]{64}", entry["content_digest"])
                is None
            ):
                _fail(
                    "scenario_artifact_invalid",
                    f"{source}: source snapshot contains an invalid entry",
                )
            previous_key = entry["source_id"]
    normalized = deepcopy(value)
    normalized["saved_status"] = _validate_saved_status(
        value["saved_status"], source=source
    )
    try:
        canonical_json_bytes(normalized)
    except StrictArtifactError as error:
        _fail("scenario_artifact_invalid", f"{source}: {error}")
    return normalized


def _source_snapshot(
    deployment: dict[str, Any] | None,
    *,
    project_root: Path,
) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    """Capture the admitted model closure and its small advisory identity table."""

    if deployment is None:
        return None, None
    try:
        closure = capture_deployment_model_sources(
            deployment, project_root=project_root
        )
    except ModelSourceCaptureError as error:
        _fail("scenario_source_invalid", str(error), status_code=409)
    entries: list[dict[str, str]] = []
    for component in closure["components"]:
        component_id = component["component_id"]
        for name, artifact in sorted(component["entrypoints"].items()):
            entries.append(
                {
                    "source_id": f"component:{component_id}:{name}",
                    "content_digest": artifact["content_digest"],
                }
            )
        for artifact in component["resources"]:
            entries.append(
                {
                    "source_id": (
                        f"component:{component_id}:resource:{artifact['logical_uri']}"
                    ),
                    "content_digest": artifact["content_digest"],
                }
            )
    entries.sort(key=lambda row: row["source_id"])
    return (
        {"schema_version": SCENARIO_SOURCE_SNAPSHOT_VERSION, "entries": entries},
        closure,
    )


def _source_change_diagnostics(
    saved: dict[str, Any] | None,
    current: dict[str, Any] | None,
) -> list[dict[str, str]]:
    if saved is None and current is None:
        return []
    saved_rows = {
        row["source_id"]: row["content_digest"]
        for row in ([] if saved is None else saved["entries"])
    }
    current_rows = {
        row["source_id"]: row["content_digest"]
        for row in ([] if current is None else current["entries"])
    }
    diagnostics: list[dict[str, str]] = []
    for source_id in sorted(saved_rows.keys() | current_rows.keys()):
        before = saved_rows.get(source_id)
        after = current_rows.get(source_id)
        if before == after:
            continue
        diagnostics.append(
            {
                "code": "scenario_source_changed",
                "source_id": source_id,
                "message": (
                    "Referenced source was added"
                    if before is None
                    else "Referenced source is no longer required"
                    if after is None
                    else "Referenced source content changed"
                ),
            }
        )
    return diagnostics


def _existing_project(raw_path: str, roots: list[str]) -> Path:
    candidate = Path(raw_path).expanduser()
    try:
        if candidate.is_symlink():
            _fail("scenario_artifact_path_invalid", "Scenario project must not be a symlink")
        resolved = candidate.resolve(strict=True)
    except FileNotFoundError:
        _fail("scenario_artifact_path_invalid", "Scenario project not found", status_code=404)
    except (OSError, RuntimeError, ValueError) as error:
        _fail("scenario_artifact_path_invalid", str(error))
    if not resolved.is_dir():
        _fail("scenario_artifact_path_invalid", "Scenario project is not a directory")
    if not is_path_within_roots(resolved, roots):
        _fail(
            "scenario_artifact_path_invalid",
            "Scenario project is outside allowed roots",
            status_code=403,
        )
    return resolved


def _read_regular_at(directory_fd: int, name: str, *, source: str) -> bytes:
    descriptor = -1
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=directory_fd,
        )
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            _fail("scenario_artifact_invalid", f"{source}: member is not a regular file")
        if metadata.st_size > MAX_DOCUMENT_BYTES:
            _fail(
                "scenario_artifact_oversized",
                f"{source}: member exceeds the document byte limit",
                status_code=413,
            )
        chunks: list[bytes] = []
        remaining = MAX_DOCUMENT_BYTES + 1
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        if len(raw) > MAX_DOCUMENT_BYTES:
            _fail(
                "scenario_artifact_oversized",
                f"{source}: member exceeds the document byte limit",
                status_code=413,
            )
        return raw
    except FileNotFoundError:
        _fail(
            "scenario_project_incomplete",
            f"{source}: required project member is missing",
            status_code=409,
        )
    except OSError as error:
        _fail("scenario_artifact_invalid", f"{source}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _load_project_member(raw: bytes, *, source: str, nullable: bool) -> Any:
    if nullable and raw.strip() == b"null":
        return None
    return load_strict_json_yaml_bytes(raw, source=source)


def _capture_project(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str, str, bool, list[dict[str, str]]]:
    roots = _roots(allowed_roots)
    project = _existing_project(raw_path, roots)
    directory_fd = -1
    try:
        directory_fd = os.open(
            project, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
        actual = Path(f"/proc/self/fd/{directory_fd}").resolve(strict=True)
        if actual != project or not is_path_within_roots(actual, roots):
            _fail("scenario_artifact_changed", "Scenario project changed", status_code=409)
        try:
            os.stat(SCENARIO_SAVE_MARKER, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            _fail(
                "scenario_project_save_interrupted",
                "Scenario project contains an interrupted application save",
                status_code=409,
            )
        manifest_raw = _read_regular_at(
            directory_fd,
            SCENARIO_MANIFEST,
            source=str(project / SCENARIO_MANIFEST),
        )
        try:
            manifest_value = load_strict_json_yaml_bytes(
                manifest_raw, source=str(project / SCENARIO_MANIFEST)
            )
        except StrictArtifactError as error:
            _fail("scenario_artifact_invalid", str(error))
        manifest = _validate_project_manifest(
            manifest_value, source=str(project / SCENARIO_MANIFEST)
        )
        member_values: dict[str, Any] = {}
        member_digests: dict[str, str] = {}
        total_bytes = len(manifest_raw)
        for field, filename in SCENARIO_PROJECT_MEMBERS.items():
            raw = _read_regular_at(
                directory_fd, filename, source=str(project / filename)
            )
            total_bytes += len(raw)
            if total_bytes > MAX_DOCUMENT_BYTES:
                _fail(
                    "scenario_artifact_oversized",
                    "Scenario project exceeds the document byte limit",
                    status_code=413,
                )
            try:
                member_values[field] = _load_project_member(
                    raw,
                    source=str(project / filename),
                    nullable=field != "typed_parameter_overrides",
                )
            except StrictArtifactError as error:
                _fail("scenario_artifact_invalid", str(error))
            member_digests[filename] = raw_sha256_digest(raw)
        document = validate_scenario_document(
            {
                "api_version": SCENARIO_API_VERSION,
                "scenario_id": manifest["scenario_id"],
                "name": manifest["name"],
                "description": manifest["description"],
                **member_values,
                "saved_status": manifest["saved_status"],
            },
            source=str(project),
        )
        current_digest = scenario_content_digest(
            document["program_draft"],
            document["deployment_draft"],
            document["program_role_assignment"],
            document["typed_parameter_overrides"],
        )
        actual_presence = {
            "deployment_present": document["deployment_draft"] is not None,
            "program_present": document["program_draft"] is not None,
            "assignment_present": document["program_role_assignment"] is not None,
        }
        changed = (
            member_digests != manifest["members"]
            or actual_presence != manifest["content"]
            or current_digest != manifest["saved_status"]["checked_content_digest"]
        )
        current_snapshot, _ = _source_snapshot(
            document["deployment_draft"], project_root=project
        )
        source_diagnostics = _source_change_diagnostics(
            manifest["source_snapshot"], current_snapshot
        )
        capture_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "manifest": raw_sha256_digest(manifest_raw),
                    "members": member_digests,
                }
            )
        )
        return document, str(project), capture_digest, changed, source_diagnostics
    finally:
        if directory_fd >= 0:
            os.close(directory_fd)


def load_scenario_artifact(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str]:
    document, source, _, _, _ = _capture_project(
        raw_path, allowed_roots=allowed_roots
    )
    return document, source


def capture_scenario_artifact(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str, str]:
    """Capture one complete fixed-member Scenario project exactly once."""

    document, source, digest, _, _ = _capture_project(
        raw_path, allowed_roots=allowed_roots
    )
    return document, source, digest


def capture_scenario_artifact_with_diagnostics(
    raw_path: str,
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str, str, list[dict[str, str]]]:
    """Capture one project and report advisory changes in referenced sources."""

    document, source, digest, _, diagnostics = _capture_project(
        raw_path, allowed_roots=allowed_roots
    )
    return document, source, digest, diagnostics


def _replace_regular_at(directory_fd: int, name: str, raw: bytes) -> None:
    temporary = f".{name}.tmp-{uuid.uuid4().hex}"
    descriptor = -1
    temporary_exists = False
    try:
        try:
            existing = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is not None and not stat.S_ISREG(existing.st_mode):
            _fail(
                "scenario_artifact_target_invalid",
                f"Scenario project member {name} is not a regular file",
            )
        mode = stat.S_IMODE(existing.st_mode) if existing is not None else 0o644
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
            dir_fd=directory_fd,
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
        os.replace(
            temporary, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd
        )
        temporary_exists = False
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary_exists:
            try:
                os.unlink(temporary, dir_fd=directory_fd)
            except OSError:
                pass


def _writable_project(raw_path: str, roots: list[str]) -> tuple[Path, Path]:
    target = Path(normalize_scenario_save_path(raw_path)).expanduser()
    try:
        parent = target.parent.resolve(strict=True)
    except FileNotFoundError:
        _fail(
            "scenario_artifact_path_invalid",
            "Scenario project parent does not exist",
            status_code=404,
        )
    except (OSError, RuntimeError, ValueError) as error:
        _fail("scenario_artifact_path_invalid", str(error))
    if target.name in {"", ".", ".."}:
        _fail("scenario_artifact_path_invalid", "invalid Scenario project name")
    if not parent.is_dir() or not is_path_within_roots(parent, roots):
        _fail(
            "scenario_artifact_path_invalid",
            "Scenario project is outside allowed roots",
            status_code=403,
        )
    return parent / target.name, parent


def save_scenario_artifact(
    document: dict[str, Any],
    raw_path: str,
    *,
    allowed_roots: list[str],
    overwrite: bool,
    source_project_path: str | None = None,
) -> str:
    roots = _roots(allowed_roots)
    target, parent = _writable_project(raw_path, roots)
    prepared_document = deepcopy(document)
    if source_project_path is not None:
        source_project = _existing_project(source_project_path, roots)
        if source_project != target:
            deployment_intent = prepared_document.get("deployment_draft")
            if deployment_intent is not None:
                try:
                    prepared_document["deployment_draft"] = (
                        rebase_deployment_intent_sources(
                            deployment_intent,
                            source_project_root=source_project,
                            destination_project_root=target,
                        )
                    )
                except DeploymentIntentError as error:
                    _fail("scenario_deployment_intent_invalid", str(error))
    source_snapshot, _ = _source_snapshot(
        prepared_document.get("deployment_draft"), project_root=target
    )
    member_bytes, manifest_raw = _project_payload(
        prepared_document, source_snapshot=source_snapshot
    )
    parent_fd = -1
    project_fd = -1
    try:
        parent_fd = os.open(
            parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
        actual_parent = Path(f"/proc/self/fd/{parent_fd}").resolve(strict=True)
        if actual_parent != parent or not is_path_within_roots(actual_parent, roots):
            _fail("scenario_artifact_changed", "Scenario directory changed", status_code=409)
        try:
            existing = os.stat(target.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is None:
            try:
                os.mkdir(target.name, mode=0o755, dir_fd=parent_fd)
            except FileExistsError:
                _fail(
                    "scenario_artifact_overwrite_confirmation_required",
                    "Scenario project appeared and requires confirmation",
                    status_code=409,
                )
            os.fsync(parent_fd)
        else:
            if not stat.S_ISDIR(existing.st_mode):
                _fail(
                    "scenario_artifact_target_invalid",
                    "Scenario target is not a directory",
                )
            if not overwrite:
                _fail(
                    "scenario_artifact_overwrite_confirmation_required",
                    "Scenario project exists and requires confirmation",
                    status_code=409,
                )
        project_fd = os.open(
            target.name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
        actual_project = Path(f"/proc/self/fd/{project_fd}").resolve(strict=True)
        if actual_project != target or not is_path_within_roots(actual_project, roots):
            _fail("scenario_artifact_changed", "Scenario project changed", status_code=409)
        existing_names = set(os.listdir(project_fd))
        if existing_names and SCENARIO_MANIFEST not in existing_names and not existing_names <= {
            "sources",
            "assets",
        }:
            _fail(
                "scenario_artifact_target_invalid",
                "Scenario target is a non-project directory",
            )
        marker = _json_bytes(
            {
                "api_version": "manyforge.scenario.project_save.v1",
                "members": {
                    filename: raw_sha256_digest(raw)
                    for filename, raw in sorted(member_bytes.items())
                },
                "manifest": raw_sha256_digest(manifest_raw),
            },
            label=SCENARIO_SAVE_MARKER,
        )
        _replace_regular_at(project_fd, SCENARIO_SAVE_MARKER, marker)
        os.fsync(project_fd)
        for filename, raw in sorted(member_bytes.items()):
            _replace_regular_at(project_fd, filename, raw)
        _replace_regular_at(project_fd, SCENARIO_MANIFEST, manifest_raw)
        os.fsync(project_fd)
        os.unlink(SCENARIO_SAVE_MARKER, dir_fd=project_fd)
        os.fsync(project_fd)
        return str(target)
    except ScenarioArtifactError:
        raise
    except OSError as error:
        _fail("scenario_artifact_write_failed", str(error))
    finally:
        if project_fd >= 0:
            os.close(project_fd)
        if parent_fd >= 0:
            os.close(parent_fd)


def save_scenario_member(
    project_path: str,
    field: str,
    value: dict[str, Any],
    *,
    allowed_roots: list[str],
) -> tuple[dict[str, Any], str, str]:
    """Save one authored member through the project's manifest-owning writer.

    Other members come from disk, never from unrelated unsaved Drafts. Capture
    rejects an interrupted project rather than overwriting its evidence.
    """
    if field not in {"program_draft", "deployment_draft"}:
        raise ValueError("unsupported Scenario member")
    document, path, digest = capture_scenario_artifact(
        project_path, allowed_roots=allowed_roots
    )
    if document[field] == value:
        return document, path, digest
    document[field] = deepcopy(value)
    document["saved_status"] = {
        **document["saved_status"],
        "last_known_status": "unknown",
        "checked_content_digest": scenario_content_digest(
            document["program_draft"], document["deployment_draft"],
            document["program_role_assignment"], document["typed_parameter_overrides"],
        ),
    }
    save_scenario_artifact(
        document, path, allowed_roots=allowed_roots, overwrite=True,
        source_project_path=path,
    )
    return capture_scenario_artifact(path, allowed_roots=allowed_roots)


def _export_relative_path(*parts: str) -> str:
    path = PurePosixPath(*parts)
    if any(part in {"", ".", ".."} for part in path.parts):
        _fail("scenario_export_path_invalid", "Export produced an unsafe path")
    return path.as_posix()


def _write_export_member(root: Path, relative: str, value: bytes) -> None:
    destination = root.joinpath(*PurePosixPath(relative).parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with destination.open("xb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        _fail("scenario_export_failed", f"Could not write {relative}: {error}")


def _read_export_asset(path: Path, *, roots: list[str]) -> bytes:
    try:
        resolved = normalize_existing_file_path(
            str(path), roots, kind_label="Scenario asset"
        )
        if resolved.is_symlink():
            _fail("scenario_export_asset_invalid", f"Asset is a symlink: {path}")
        descriptor = os.open(
            resolved, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
        try:
            before = os.fstat(descriptor)
            if (
                not stat.S_ISREG(before.st_mode)
                or not 0 < before.st_size <= MAX_ARTIFACT_BYTES
            ):
                _fail(
                    "scenario_export_asset_invalid",
                    f"Asset is not one bounded plain file: {path}",
                )
            chunks: list[bytes] = []
            remaining = before.st_size
            while remaining:
                chunk = os.read(descriptor, min(remaining, 1024 * 1024))
                if not chunk:
                    _fail("scenario_export_asset_changed", f"Asset changed: {path}")
                chunks.append(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                _fail("scenario_export_asset_changed", f"Asset changed: {path}")
            after = os.fstat(descriptor)
            if (
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
                != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            ):
                _fail("scenario_export_asset_changed", f"Asset changed: {path}")
            return b"".join(chunks)
        finally:
            os.close(descriptor)
    except HTTPException as error:
        _fail(
            "scenario_export_asset_invalid",
            str(error.detail),
            status_code=error.status_code,
        )
    except OSError as error:
        _fail("scenario_export_asset_invalid", f"Could not read {path}: {error}")


def _scene_mesh_shapes(value: Any):
    if type(value) is dict:
        if value.get("type") == "mesh" and isinstance(
            value.get("mesh_resource_uri"), str
        ):
            yield value
        for child in value.values():
            yield from _scene_mesh_shapes(child)
    elif type(value) is list:
        for child in value:
            yield from _scene_mesh_shapes(child)


def _resolve_scene_mesh(
    uri: str, *, source_project: Path, roots: list[str]
) -> Path:
    if not uri or "\x00" in uri or "://" in uri:
        _fail(
            "scenario_export_asset_invalid",
            f"Unsupported scene mesh reference: {uri!r}",
        )
    raw = Path(uri)
    candidates = [raw] if raw.is_absolute() else [source_project / raw]
    if not raw.is_absolute():
        candidates.extend(Path(root) / raw for root in roots)
    for candidate in candidates:
        try:
            return normalize_existing_file_path(
                str(candidate), roots, kind_label="Scenario mesh"
            )
        except HTTPException:
            continue
    _fail(
        "scenario_export_asset_invalid",
        f"Scene mesh does not resolve to an allowed plain file: {uri!r}",
    )


def export_scenario_artifact(
    document: dict[str, Any],
    raw_destination_path: str,
    *,
    source_project_path: str,
    allowed_roots: list[str],
) -> str:
    """Create one self-contained project without changing the source project."""

    roots = _roots(allowed_roots)
    source_project = _existing_project(source_project_path, roots)
    target, parent = _writable_project(raw_destination_path, roots)
    if target.exists() or target.is_symlink():
        _fail(
            "scenario_artifact_overwrite_confirmation_required",
            "Export destination already exists; choose a new project directory",
            status_code=409,
        )
    prepared = validate_scenario_document(deepcopy(document), source="<export>")
    deployment = prepared.get("deployment_draft")
    closure: dict[str, Any] | None = None
    if deployment is not None:
        _, closure = _source_snapshot(deployment, project_root=source_project)
        if closure is None:
            raise AssertionError("Deployment source capture returned no closure")

    temporary = parent / f".{target.name}.export-{uuid.uuid4().hex}"
    temporary.mkdir(mode=0o755)
    try:
        if deployment is not None and closure is not None:
            components = {
                row["component_id"]: row for row in closure["components"]
            }
            for component in deployment["components"]:
                component_id = component["component_id"]
                captured = components[component_id]
                source_root = _export_relative_path("sources", component_id)
                urdf_relative = _export_relative_path(source_root, "model.urdf")
                _write_export_member(
                    temporary,
                    urdf_relative,
                    decode_model_source_artifact(
                        captured["entrypoints"]["urdf"],
                        maximum_bytes=MAX_XML_BYTES,
                        label=f"{component_id} URDF",
                    ),
                )
                model_source: dict[str, Any] = {
                    "urdf": {"project_path": urdf_relative},
                    "package_roots": {},
                }
                if "srdf" in captured["entrypoints"]:
                    srdf_relative = _export_relative_path(source_root, "model.srdf")
                    _write_export_member(
                        temporary,
                        srdf_relative,
                        decode_model_source_artifact(
                            captured["entrypoints"]["srdf"],
                            maximum_bytes=MAX_XML_BYTES,
                            label=f"{component_id} SRDF",
                        ),
                    )
                    model_source["srdf"] = {"project_path": srdf_relative}
                for resource in captured["resources"]:
                    package_ref = resource["logical_uri"].removeprefix("package://")
                    package_name, separator, relative = package_ref.partition("/")
                    if not separator:
                        _fail(
                            "scenario_export_asset_invalid",
                            f"Invalid package resource URI: {resource['logical_uri']!r}",
                        )
                    package_root = _export_relative_path(
                        "assets", "models", component_id, package_name
                    )
                    destination = _export_relative_path(package_root, relative)
                    _write_export_member(
                        temporary,
                        destination,
                        decode_model_source_artifact(
                            resource,
                            maximum_bytes=MAX_ARTIFACT_BYTES,
                            label=resource["logical_uri"],
                        ),
                    )
                    model_source["package_roots"][package_name] = {
                        "project_path": package_root
                    }
                component["model_source"] = model_source

        scene_assets: dict[tuple[str, str], str] = {}
        for shape in _scene_mesh_shapes(prepared.get("program_draft")):
            uri = shape["mesh_resource_uri"]
            source = _resolve_scene_mesh(
                uri, source_project=source_project, roots=roots
            )
            value = _read_export_asset(source, roots=roots)
            digest = raw_sha256_digest(value)
            key = (digest, source.name)
            relative = scene_assets.get(key)
            if relative is None:
                relative = _export_relative_path(
                    "assets", "scene", f"{digest[7:23]}-{source.name}"
                )
                _write_export_member(temporary, relative, value)
                scene_assets[key] = relative
            shape["mesh_resource_uri"] = relative

        save_scenario_artifact(
            prepared,
            str(temporary),
            allowed_roots=roots,
            overwrite=True,
        )
        exported, _, _, diagnostics = capture_scenario_artifact_with_diagnostics(
            str(temporary), allowed_roots=[str(temporary)]
        )
        if diagnostics or exported != prepared:
            _fail(
                "scenario_export_validation_failed",
                "Exported project did not reopen from its local dependencies",
            )
        os.replace(temporary, target)
        parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
        return str(target)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def scenario_directory(
    raw_path: str | None,
    *,
    allowed_roots: list[str],
) -> Path:
    roots = _roots(allowed_roots)
    selected = raw_path or roots[0]
    try:
        candidate = Path(selected).expanduser()
        if candidate.is_symlink():
            _fail("scenario_directory_invalid", "Scenario directory is a symlink")
        resolved = candidate.resolve(strict=True)
    except FileNotFoundError:
        _fail("scenario_directory_not_found", "Scenario directory not found", status_code=404)
    except (OSError, RuntimeError, ValueError) as error:
        _fail("scenario_directory_invalid", str(error))
    if not resolved.is_dir():
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
        if path.is_symlink() or not path.is_dir() or not (path / SCENARIO_MANIFEST).is_file():
            continue
        try:
            document, resolved, _, member_changed, source_diagnostics = _capture_project(
                str(path), allowed_roots=allowed_roots
            )
            saved = document["saved_status"]
            current_digest = scenario_content_digest(
                document["program_draft"],
                document["deployment_draft"],
                document["program_role_assignment"],
                document["typed_parameter_overrides"],
            )
            modified = member_changed or current_digest != saved["checked_content_digest"]
            version_changed = saved["manyforge_version"] != manyforge_version
            display_status = (
                "source_changed"
                if source_diagnostics
                else "changed"
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
                    "saved_manyforge_version": saved["manyforge_version"],
                    "display_status": display_status,
                    "status_detail": (
                        "One or more referenced source files changed."
                        if source_diagnostics
                        else "Content changed since the recorded status."
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
                    "saved_manyforge_version": None,
                    "display_status": "unsupported",
                    "status_detail": str(error),
                }
            )
    return str(directory), rows
