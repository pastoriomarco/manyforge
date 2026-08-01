#!/usr/bin/env python3
"""Describe or assemble Step 18 release evidence without launching old lanes.

The historical seven-lane producer is pinned at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1. Its lane command
created containers and ran the pre-cutover full product. During relocation
that command returns a typed gate before subprocess or filesystem-output
work. The describe and assemble commands remain offline artifact helpers.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import math
import os
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any, NamedTuple


UNAVAILABLE_EXIT_CODE = 78
HISTORICAL_SOURCE = "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
HISTORICAL_SOURCE_BLOB = "398a61263e5418242b313e713d632370c8342e1a"
HISTORICAL_TEST_BLOB = "b39353a830e4158e5369953a42427ceb81fa63fa"
HISTORICAL_PRODUCER_SOURCE_LINES = (
    "historical source blob lines 17, 32-42, 63-64, 74-97, 119-120, "
    "148-437, 509-523, 530-534, and 551-557"
)
# Keep the unavailable ``lane`` selector independent of filesystem traversal at
# module import time. Assemble resolves, binds, and validates this lexical
# default before using it.
REPO_ROOT = Path(__file__).parent.parent.parent
LANE_SCHEMA = "manyforge.step18.release_gate_lane.v1"
ARTIFACT_MANIFEST_SCHEMA = (
    "manyforge.step18.release_gate_artifact_manifest.v1"
)
VERIFICATION_SCOPE = "corrected_source_release_gates"
LANES = (
    "full_dev_regression",
    "full_ros_regression",
    "contract_architecture_hygiene",
    "core_asan_ubsan",
    "core_tsan",
    "collision_asan_ubsan",
    "collision_tsan",
)
LANE_MANIFEST_FIELDS = {
    "schema_version",
    "verification_scope",
    "lane",
    "run_id",
    "paths",
    "identities",
    "execution",
    "artifacts",
    "script_hashes",
    "overall_step18_closure_claimed",
    "requires_common_candidate_aggregator",
}
MAX_LANE_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_JSON_COLLECTION_ITEMS = 4096
MAX_JSON_DEPTH = 32
MAX_JSON_NODES = 65536
MAX_JSON_KEY_BYTES = 256
MAX_JSON_STRING_BYTES = 1024 * 1024
MAX_OUTPUT_ARTIFACT_BYTES = 8 * 1024 * 1024
MAX_REFERENCE_PATH_BYTES = 4096
MAX_CONTAINER_INSPECT_BYTES = 4 * 1024 * 1024
MAX_CONTAINER_LOG_BYTES = 64 * 1024 * 1024
MAX_SECURITY_OVERRIDE_BYTES = 64 * 1024
GIT_CHECK_TIMEOUT_SECONDS = 5


class RunnerError(RuntimeError):
    """An offline release-evidence artifact is invalid or ambiguous."""


class RestorationAssignment(NamedTuple):
    historical_case: str
    historical_source_lines: str
    restoration_gate: str
    required_behavior: str


class OutputDirectoryHandles(NamedTuple):
    repo: int
    variable: int
    qualification: int
    output: int


LANE_ASSIGNMENTS = {
    "full_dev_regression": RestorationAssignment(
        "full_dev_regression",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "I5/H0+I6/N+I8/P5",
        "rerun the complete dev regression only after headless/native/final "
        "Stop restoration",
    ),
    "full_ros_regression": RestorationAssignment(
        "full_ros_regression",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "I6/N+I8/P5+I9/I+I10/R",
        "rerun the complete ROS matrix only after native, Isaac, and real lanes return",
    ),
    "contract_architecture_hygiene": RestorationAssignment(
        "contract_architecture_hygiene",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "retained/focused-checks+I5/H0+I6/N",
        "keep focused hygiene checks active; rebuild the aggregate lane on "
        "current contracts",
    ),
    "core_asan_ubsan": RestorationAssignment(
        "core_asan_ubsan",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "retained/dedicated-sanitizer+I8/P5-aggregation",
        "use the dedicated Core sanitizer lane now and aggregate it again at P5",
    ),
    "core_tsan": RestorationAssignment(
        "core_tsan",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "retained/dedicated-sanitizer+I8/P5-aggregation",
        "use the dedicated Core TSan lane now and aggregate it again at P5",
    ),
    "collision_asan_ubsan": RestorationAssignment(
        "collision_asan_ubsan",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "retained/dedicated-sanitizer+I6/N+I8/P5-aggregation",
        "use the owner-neutral collision sanitizer lane now and aggregate at N/P5",
    ),
    "collision_tsan": RestorationAssignment(
        "collision_tsan",
        HISTORICAL_PRODUCER_SOURCE_LINES,
        "retained/dedicated-sanitizer+I6/N+I8/P5-aggregation",
        "use the owner-neutral collision TSan lane now and aggregate at N/P5",
    ),
}

if set(LANE_ASSIGNMENTS) != set(LANES):
    raise RuntimeError("release-lane restoration map is incomplete")


def _source_identity_module() -> Any:
    try:
        import source_identity as module
    except ImportError as error:
        raise RunnerError(
            f"source-identity helper is unavailable: {error}"
        ) from error
    required = (
        "SourceIdentityError",
        "source_identity",
        "specs_contract_identity",
    )
    if any(not hasattr(module, name) for name in required):
        raise RunnerError("source-identity helper has an incompatible API")
    return module


def _verifier_module() -> Any:
    try:
        import verify_step18_release_gates as module
    except ImportError as error:
        raise RunnerError(f"release verifier is unavailable: {error}") from error
    expected = {
        "LANE_SCHEMA": LANE_SCHEMA,
        "ARTIFACT_MANIFEST_SCHEMA": ARTIFACT_MANIFEST_SCHEMA,
        "VERIFICATION_SCOPE": VERIFICATION_SCOPE,
        "LANES": LANES,
    }
    if any(getattr(module, name, None) != value for name, value in expected.items()):
        raise RunnerError("release verifier constants differ from the assembler")
    required = (
        "ValidationError",
        "publish_verification_summary",
        "verify_artifact_manifest",
    )
    if any(not hasattr(module, name) for name in required):
        raise RunnerError("release verifier has an incompatible API")
    return module


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            # The duplicate key is untrusted and can occupy nearly the entire
            # input bound.  Do not reflect it into terminal output.
            raise RunnerError("lane manifest repeats a JSON field")
        value[key] = item
    return value


def _reject_json_constant(token: str) -> None:
    raise RunnerError(f"lane manifest contains non-finite number: {token}")


def _utf8_size(value: str, label: str) -> int:
    try:
        return len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise RunnerError(f"{label} is not valid Unicode text") from error


def _validate_json_bounds(value: Any) -> None:
    stack = [(value, 0)]
    nodes = 0
    while stack:
        item, depth = stack.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise RunnerError("lane manifest exceeds the JSON node limit")
        if depth > MAX_JSON_DEPTH:
            raise RunnerError("lane manifest exceeds the JSON depth limit")
        if isinstance(item, dict):
            if len(item) > MAX_JSON_COLLECTION_ITEMS:
                raise RunnerError("lane manifest object has too many fields")
            for key, child in item.items():
                if (
                    _utf8_size(key, "lane manifest JSON field name")
                    > MAX_JSON_KEY_BYTES
                ):
                    raise RunnerError("lane manifest JSON field name is too long")
                stack.append((child, depth + 1))
        elif isinstance(item, list):
            if len(item) > MAX_JSON_COLLECTION_ITEMS:
                raise RunnerError("lane manifest array has too many items")
            stack.extend((child, depth + 1) for child in item)
        elif isinstance(item, str):
            if _utf8_size(item, "lane manifest JSON string") > MAX_JSON_STRING_BYTES:
                raise RunnerError("lane manifest JSON string is too long")
        elif isinstance(item, bool) or item is None:
            continue
        elif isinstance(item, int):
            if not -(2**63) <= item < 2**63:
                raise RunnerError("lane manifest integer exceeds signed 64-bit range")
        elif isinstance(item, float):
            if not math.isfinite(item):
                raise RunnerError("lane manifest contains a non-finite number")
        else:
            raise RunnerError("lane manifest contains an unsupported JSON value")


def _read_lane_manifest(
    raw: Path,
) -> tuple[Path, dict[str, Any], str, int, tuple[int, int]]:
    try:
        descriptor = os.open(
            raw,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise RunnerError(
                f"lane manifest must not be a symbolic link: {raw}"
            ) from error
        raise RunnerError(f"lane manifest is unavailable: {raw}: {error}") from error
    try:
        try:
            path = Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
        except OSError as error:
            raise RunnerError(
                f"cannot bind lane manifest path to its open file: {raw}: {error}"
            ) from error
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RunnerError(f"lane manifest is not a regular file: {path}")
        if metadata.st_size > MAX_LANE_MANIFEST_BYTES:
            raise RunnerError(
                f"lane manifest exceeds {MAX_LANE_MANIFEST_BYTES} bytes: {path}"
            )
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(MAX_LANE_MANIFEST_BYTES + 1)
        if len(payload) > MAX_LANE_MANIFEST_BYTES:
            raise RunnerError(
                f"lane manifest exceeds {MAX_LANE_MANIFEST_BYTES} bytes: {path}"
            )
        after = os.fstat(descriptor)
        stable_fields = (
            "st_dev",
            "st_ino",
            "st_size",
            "st_mtime_ns",
            "st_ctime_ns",
        )
        if any(
            getattr(metadata, field) != getattr(after, field)
            for field in stable_fields
        ):
            raise RunnerError(f"lane manifest changed while being read: {path}")
        try:
            path_metadata = path.stat(follow_symlinks=False)
        except OSError as error:
            raise RunnerError(
                f"lane manifest path changed while being read: {path}: {error}"
            ) from error
        if (path_metadata.st_dev, path_metadata.st_ino) != (
            metadata.st_dev,
            metadata.st_ino,
        ):
            raise RunnerError(f"lane manifest path changed while being read: {path}")
    finally:
        os.close(descriptor)
    if len(payload) != metadata.st_size:
        raise RunnerError(f"lane manifest size changed while being read: {path}")
    try:
        value = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise RunnerError(f"lane manifest is invalid JSON: {path}: {error}") from error
    _validate_json_bounds(value)
    if not isinstance(value, dict):
        raise RunnerError(f"lane manifest must contain one JSON object: {path}")
    return (
        path,
        value,
        hashlib.sha256(payload).hexdigest(),
        len(payload),
        (metadata.st_dev, metadata.st_ino),
    )


def _exact_json_object(
    value: Any,
    expected: set[str],
    label: str,
) -> dict[str, Any]:
    if type(value) is not dict or set(value) != expected:
        raise RunnerError(f"{label} does not use its exact JSON object contract")
    return value


def _validate_referenced_regular_file_record(
    record: Any,
    *,
    label: str,
    maximum_bytes: int,
) -> dict[str, Any]:
    binding = _exact_json_object(
        record,
        {"path", "sha256", "bytes"},
        label,
    )
    raw_path = binding["path"]
    if (
        type(raw_path) is not str
        or not raw_path
        or "\x00" in raw_path
        or _utf8_size(raw_path, f"{label} path") > MAX_REFERENCE_PATH_BYTES
    ):
        raise RunnerError(f"{label} path is invalid or unbounded")
    digest = binding["sha256"]
    if (
        type(digest) is not str
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        raise RunnerError(f"{label} hash is not a lowercase SHA-256")
    expected_bytes = binding["bytes"]
    if (
        type(expected_bytes) is not int
        or expected_bytes < 0
        or expected_bytes > maximum_bytes
    ):
        raise RunnerError(f"{label} byte count exceeds its retained-artifact bound")
    return binding


def _bind_referenced_regular_file(
    owner_path: Path,
    binding: dict[str, Any],
    *,
    label: str,
    maximum_bytes: int,
) -> tuple[Path, tuple[int, int], int]:
    raw_path = binding["path"]
    expected_bytes = binding["bytes"]

    candidate = Path(raw_path)
    path = candidate if candidate.is_absolute() else owner_path.parent / candidate
    lexical_path = Path(os.path.abspath(os.path.normpath(os.fspath(path))))
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise RunnerError(f"{label} must not be a symbolic link") from error
        raise RunnerError(f"{label} is unavailable: {error}") from error
    try:
        try:
            canonical = Path(f"/proc/self/fd/{descriptor}").resolve(strict=True)
        except OSError as error:
            raise RunnerError(
                f"cannot bind {label} to its open file: {error}"
            ) from error
        if canonical != lexical_path:
            raise RunnerError(f"{label} path must not traverse symbolic links")
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RunnerError(f"{label} is not a regular file")
        if metadata.st_size != expected_bytes:
            raise RunnerError(f"{label} byte count differs from its open file")
        if metadata.st_size > maximum_bytes:
            raise RunnerError(f"{label} exceeds its retained-artifact bound")
        try:
            path_metadata = canonical.stat(follow_symlinks=False)
        except OSError as error:
            raise RunnerError(
                f"{label} path changed while being bound: {error}"
            ) from error
        if (path_metadata.st_dev, path_metadata.st_ino) != (
            metadata.st_dev,
            metadata.st_ino,
        ):
            raise RunnerError(f"{label} path changed while being bound")
    finally:
        os.close(descriptor)
    return lexical_path, (metadata.st_dev, metadata.st_ino), metadata.st_size


def _preflight_lane_artifacts(
    manifest_path: Path,
    value: dict[str, Any],
    lane: str,
) -> list[tuple[Path, tuple[int, int], int]]:
    artifacts = _exact_json_object(
        value.get("artifacts"),
        {"container_inspect", "container_log"},
        f"{lane} artifacts",
    )
    reference_specs = [
        (
            artifacts["container_inspect"],
            f"{lane} container inspect",
            MAX_CONTAINER_INSPECT_BYTES,
        ),
        (
            artifacts["container_log"],
            f"{lane} container log",
            MAX_CONTAINER_LOG_BYTES,
        ),
    ]
    execution = value.get("execution")
    if type(execution) is not dict or "security_override" not in execution:
        raise RunnerError(f"{lane} execution omits its security-override binding")
    if execution["security_override"] is not None:
        reference_specs.append(
            (
                execution["security_override"],
                f"{lane} security override",
                MAX_SECURITY_OVERRIDE_BYTES,
            )
        )
    validated = [
        (
            _validate_referenced_regular_file_record(
                record,
                label=label,
                maximum_bytes=maximum_bytes,
            ),
            label,
            maximum_bytes,
        )
        for record, label, maximum_bytes in reference_specs
    ]
    return [
        _bind_referenced_regular_file(
            manifest_path,
            binding,
            label=label,
            maximum_bytes=maximum_bytes,
        )
        for binding, label, maximum_bytes in validated
    ]


def _open_owned_child_directory(
    parent_descriptor: int,
    name: str,
    *,
    create: bool,
) -> int:
    if create:
        try:
            os.mkdir(name, mode=0o700, dir_fd=parent_descriptor)
        except FileExistsError:
            pass
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            dir_fd=parent_descriptor,
        )
    except OSError as error:
        raise RunnerError(f"unsafe qualification directory {name}: {error}") from error
    metadata = os.fstat(descriptor)
    if metadata.st_uid != os.getuid() or metadata.st_mode & 0o022:
        os.close(descriptor)
        raise RunnerError(
            f"qualification directory {name} must be current-user-owned "
            "and not group/world writable"
        )
    return descriptor


def _create_output_directory(
    repo_root: Path,
    output: Path,
    expected_repo_identity: tuple[int, int],
) -> OutputDirectoryHandles:
    try:
        repo_descriptor = os.open(
            repo_root,
            os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
        )
    except OSError as error:
        raise RunnerError(
            f"cannot bind the ManyForge repository root: {error}"
        ) from error
    repo_metadata = os.fstat(repo_descriptor)
    if (repo_metadata.st_dev, repo_metadata.st_ino) != expected_repo_identity:
        os.close(repo_descriptor)
        raise RunnerError("ManyForge repository root identity changed")
    variable_descriptor = -1
    qualification_descriptor = -1
    output_descriptor = -1
    output_created = False
    try:
        variable_descriptor = _open_owned_child_directory(
            repo_descriptor,
            ".manyforge-var",
            create=True,
        )
        qualification_descriptor = _open_owned_child_directory(
            variable_descriptor,
            "qualification",
            create=True,
        )
        try:
            os.mkdir(output.name, mode=0o700, dir_fd=qualification_descriptor)
            output_created = True
        except FileExistsError as error:
            raise RunnerError(f"output already exists: {output}") from error
        output_descriptor = _open_owned_child_directory(
            qualification_descriptor,
            output.name,
            create=False,
        )
        return OutputDirectoryHandles(
            repo_descriptor,
            variable_descriptor,
            qualification_descriptor,
            output_descriptor,
        )
    except BaseException:
        if output_descriptor >= 0:
            os.close(output_descriptor)
        if output_created and qualification_descriptor >= 0:
            try:
                os.rmdir(output.name, dir_fd=qualification_descriptor)
            except OSError:
                pass
        if qualification_descriptor >= 0:
            os.close(qualification_descriptor)
        if variable_descriptor >= 0:
            os.close(variable_descriptor)
        os.close(repo_descriptor)
        raise


def _close_output_handles(handles: OutputDirectoryHandles) -> None:
    os.close(handles.output)
    os.close(handles.qualification)
    os.close(handles.variable)
    os.close(handles.repo)


def _render_bounded_json(name: str, value: Any) -> bytes:
    try:
        payload = (
            json.dumps(
                value,
                indent=2,
                sort_keys=True,
                allow_nan=False,
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise RunnerError(f"cannot serialize {name}: {error}") from error
    if len(payload) > MAX_OUTPUT_ARTIFACT_BYTES:
        raise RunnerError(
            f"{name} exceeds the {MAX_OUTPUT_ARTIFACT_BYTES}-byte output bound"
        )
    return payload


def _write_exclusive_at(
    directory_descriptor: int,
    name: str,
    value: dict[str, Any],
) -> tuple[int, int]:
    payload = _render_bounded_json(name, value)
    descriptor = os.open(
        name,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
        dir_fd=directory_descriptor,
    )
    identity: tuple[int, int] | None = None
    try:
        created_metadata = os.fstat(descriptor)
        identity = (created_metadata.st_dev, created_metadata.st_ino)
        offset = 0
        while offset < len(payload):
            written = os.write(descriptor, payload[offset:])
            if written <= 0:
                raise RunnerError(f"short write while publishing {name}")
            offset += written
        os.fsync(descriptor)
        metadata = os.fstat(descriptor)
    except BaseException:
        try:
            os.close(descriptor)
        finally:
            if identity is not None:
                _unlink_matching_regular_file(
                    directory_descriptor,
                    name,
                    identity,
                )
        raise
    try:
        os.close(descriptor)
    except OSError:
        if identity is not None:
            _unlink_matching_regular_file(
                directory_descriptor,
                name,
                identity,
            )
        raise
    return metadata.st_dev, metadata.st_ino


def _directory_path_matches_descriptor(
    parent_descriptor: int,
    name: str,
    directory_descriptor: int,
) -> bool:
    try:
        path_metadata = os.stat(
            name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        descriptor_metadata = os.fstat(directory_descriptor)
    except OSError:
        return False
    return (
        stat.S_ISDIR(path_metadata.st_mode)
        and (path_metadata.st_dev, path_metadata.st_ino)
        == (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
    )


def _root_path_matches_descriptor(path: Path, descriptor: int) -> bool:
    try:
        path_metadata = path.stat(follow_symlinks=False)
        descriptor_metadata = os.fstat(descriptor)
    except OSError:
        return False
    return (
        stat.S_ISDIR(path_metadata.st_mode)
        and (path_metadata.st_dev, path_metadata.st_ino)
        == (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
    )


def _directory_descriptor_is_owned_and_not_writable(descriptor: int) -> bool:
    try:
        metadata = os.fstat(descriptor)
    except OSError:
        return False
    return metadata.st_uid == os.getuid() and not metadata.st_mode & 0o022


def _output_directory_chain_matches(
    repo_root: Path,
    output_name: str,
    handles: OutputDirectoryHandles,
) -> bool:
    return (
        _root_path_matches_descriptor(repo_root, handles.repo)
        and _directory_descriptor_is_owned_and_not_writable(handles.variable)
        and _directory_descriptor_is_owned_and_not_writable(handles.qualification)
        and _directory_descriptor_is_owned_and_not_writable(handles.output)
        and _directory_path_matches_descriptor(
            handles.repo,
            ".manyforge-var",
            handles.variable,
        )
        and _directory_path_matches_descriptor(
            handles.variable,
            "qualification",
            handles.qualification,
        )
        and _directory_path_matches_descriptor(
            handles.qualification,
            output_name,
            handles.output,
        )
    )


def _regular_file_matches_identity(
    directory_descriptor: int,
    name: str,
    identity: tuple[int, int],
) -> bool:
    try:
        metadata = os.stat(
            name,
            dir_fd=directory_descriptor,
            follow_symlinks=False,
        )
    except OSError:
        return False
    return (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and (metadata.st_dev, metadata.st_ino) == identity
    )


def _regular_file_identity_at(
    directory_descriptor: int,
    name: str,
) -> tuple[int, int]:
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=directory_descriptor,
        )
    except OSError as error:
        raise RunnerError(f"cannot bind published {name}: {error}") from error
    try:
        metadata = os.fstat(descriptor)
        identity = (metadata.st_dev, metadata.st_ino)
        if not stat.S_ISREG(metadata.st_mode) or not _regular_file_matches_identity(
            directory_descriptor,
            name,
            identity,
        ):
            raise RunnerError(f"published {name} is not one bound regular file")
    finally:
        os.close(descriptor)
    return identity


def _read_bounded_regular_file_at(
    directory_descriptor: int,
    name: str,
) -> tuple[bytes, tuple[int, int]]:
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=directory_descriptor,
        )
    except OSError as error:
        raise RunnerError(f"cannot bind published {name}: {error}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RunnerError(f"published {name} is not a regular file")
        if metadata.st_size > MAX_OUTPUT_ARTIFACT_BYTES:
            raise RunnerError(
                f"published {name} exceeds the output artifact bound"
            )
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(MAX_OUTPUT_ARTIFACT_BYTES + 1)
        if len(payload) > MAX_OUTPUT_ARTIFACT_BYTES:
            raise RunnerError(
                f"published {name} exceeds the output artifact bound"
            )
        after = os.fstat(descriptor)
        stable_fields = (
            "st_dev",
            "st_ino",
            "st_size",
            "st_mtime_ns",
            "st_ctime_ns",
        )
        if any(
            getattr(metadata, field) != getattr(after, field)
            for field in stable_fields
        ):
            raise RunnerError(f"published {name} changed while being read")
        identity = (metadata.st_dev, metadata.st_ino)
        if len(payload) != metadata.st_size or not _regular_file_matches_identity(
            directory_descriptor,
            name,
            identity,
        ):
            raise RunnerError(f"published {name} path changed while being read")
    finally:
        os.close(descriptor)
    return payload, identity


def _unlink_matching_regular_file(
    directory_descriptor: int,
    name: str,
    identity: tuple[int, int],
) -> None:
    try:
        metadata = os.stat(
            name,
            dir_fd=directory_descriptor,
            follow_symlinks=False,
        )
    except FileNotFoundError:
        return
    except OSError:
        return
    if not stat.S_ISREG(metadata.st_mode):
        return
    if (metadata.st_dev, metadata.st_ino) != identity:
        return
    try:
        os.unlink(name, dir_fd=directory_descriptor)
    except OSError:
        pass


def _cleanup_failed_output(
    handles: OutputDirectoryHandles,
    output_name: str,
    artifact_identity: tuple[int, int] | None,
    summary_identity: tuple[int, int] | None,
) -> None:
    if artifact_identity is not None:
        _unlink_matching_regular_file(
            handles.output,
            "artifact-manifest.json",
            artifact_identity,
        )
    if summary_identity is not None:
        _unlink_matching_regular_file(
            handles.output,
            "verification-summary.json",
            summary_identity,
        )
    # The verifier owns and cleans its temporary publication path.  Without a
    # captured inode, deleting a name here could erase a racing publisher's
    # file rather than one created by this assembly.
    if _directory_path_matches_descriptor(
        handles.qualification,
        output_name,
        handles.output,
    ):
        try:
            os.rmdir(output_name, dir_fd=handles.qualification)
        except OSError:
            pass


def _source_bundle(
    specs_root: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    module = _source_identity_module()
    try:
        return (
            module.source_identity(specs_root),
            module.specs_contract_identity(specs_root),
        )
    except module.SourceIdentityError as error:
        raise RunnerError(f"cannot collect Specs identity: {error}") from error


def _candidate_source(repo_root: Path) -> dict[str, Any]:
    module = _source_identity_module()
    try:
        return module.source_identity(repo_root)
    except module.SourceIdentityError as error:
        raise RunnerError(
            f"cannot collect ManyForge identity: {error}"
        ) from error


def _validate_roots(
    repo_root: Path, specs_root: Path
) -> tuple[Path, Path]:
    try:
        repo = repo_root.resolve(strict=True)
        specs = specs_root.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise RunnerError(
            f"cannot resolve qualification checkout roots: {error}"
        ) from error
    for root, label in ((repo, "ManyForge"), (specs, "Specs")):
        if not root.is_dir() or not (root / ".git").exists():
            raise RunnerError(f"{label} checkout is unavailable: {root}")
    return repo, specs


def _directory_identity(path: Path, label: str) -> tuple[int, int]:
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
        )
    except OSError as error:
        raise RunnerError(f"cannot bind {label} checkout root: {error}") from error
    try:
        metadata = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    return metadata.st_dev, metadata.st_ino


def _directory_path_has_identity(
    path: Path,
    identity: tuple[int, int],
) -> bool:
    try:
        metadata = path.stat(follow_symlinks=False)
    except OSError:
        return False
    return stat.S_ISDIR(metadata.st_mode) and (
        metadata.st_dev,
        metadata.st_ino,
    ) == identity


def _path_regular_file_has_identity(
    path: Path,
    identity: tuple[int, int],
    size: int,
) -> bool:
    try:
        metadata = path.stat(follow_symlinks=False)
    except OSError:
        return False
    return (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_size == size
        and (metadata.st_dev, metadata.st_ino) == identity
    )


def _validate_output_path(repo_root: Path, output_dir: Path) -> Path:
    raw_output = output_dir if output_dir.is_absolute() else Path.cwd() / output_dir
    output = Path(os.path.abspath(os.path.normpath(os.fspath(raw_output))))
    allowed = repo_root / ".manyforge-var" / "qualification"
    if output.parent != allowed:
        raise RunnerError(
            f"output must be one direct child of the qualification root: {allowed}"
        )
    if (
        not output.name
        or output.name in (".", "..")
        or "\x00" in output.name
        or "\n" in output.name
        or "\r" in output.name
        or _utf8_size(output.name, "output directory name") > 255
    ):
        raise RunnerError("output directory name is invalid or unbounded")
    if os.path.lexists(output):
        raise RunnerError(f"output already exists: {output}")
    try:
        ignored = subprocess.run(
            [
                "git",
                "-C",
                str(repo_root),
                "check-ignore",
                "--quiet",
                "--",
                str(output),
            ],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=GIT_CHECK_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RunnerError(f"cannot check output ignore coverage: {error}") from error
    if ignored.returncode != 0:
        raise RunnerError(
            "output is not covered by the candidate Git ignore rules"
        )
    return output


def describe_lane(lane: str) -> dict[str, Any]:
    """Return the relocation assignment without reconstructing old commands."""

    if lane not in LANE_ASSIGNMENTS:
        raise RunnerError(f"unknown release lane: {lane}")
    assignment = LANE_ASSIGNMENTS[lane]
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "lane": lane,
        "execution_available": False,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "historical_source": HISTORICAL_SOURCE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
        "assignment": assignment._asdict(),
        "overall_step18_closure_claimed": False,
    }


def assemble(
    *,
    repo_root: Path,
    specs_root: Path,
    output_dir: Path,
    lane_manifests: list[Path],
) -> tuple[Path, Path]:
    """Verify and aggregate already-produced lane artifacts only."""

    if len(lane_manifests) != len(LANES):
        raise RunnerError(
            "assemble requires exactly seven --lane-manifest values"
        )
    repo, specs = _validate_roots(repo_root, specs_root)
    repo_identity = _directory_identity(repo, "ManyForge")
    specs_identity = _directory_identity(specs, "Specs")

    references: list[dict[str, Any]] = []
    parsed_manifests: list[tuple[Path, dict[str, Any], str]] = []
    manifest_bindings: list[tuple[Path, tuple[int, int], int]] = []
    observed_lanes: list[str] = []
    observed_files: set[tuple[int, int]] = set()
    for raw in lane_manifests:
        path, value, digest, size, file_identity = _read_lane_manifest(raw)
        if file_identity in observed_files:
            raise RunnerError("lane manifest paths alias the same file")
        observed_files.add(file_identity)
        _exact_json_object(value, LANE_MANIFEST_FIELDS, "lane manifest")
        if value.get("schema_version") != LANE_SCHEMA:
            raise RunnerError(
                f"lane manifest does not use {LANE_SCHEMA}: {path}"
            )
        if value.get("verification_scope") != VERIFICATION_SCOPE:
            raise RunnerError("lane manifest verification scope differs")
        if (
            value.get("overall_step18_closure_claimed") is not False
            or value.get("requires_common_candidate_aggregator") is not True
        ):
            raise RunnerError("lane manifest improperly claims overall closure")
        lane = value.get("lane")
        if lane not in LANES:
            raise RunnerError(
                f"lane manifest names an unknown lane: {path}"
            )
        observed_lanes.append(lane)
        parsed_manifests.append((path, value, lane))
        manifest_bindings.append((path, file_identity, size))
        references.append(
            {
                "lane": lane,
                "manifest_path": str(path),
                "manifest_sha256": digest,
                "manifest_bytes": size,
            }
        )
    if (
        len(set(observed_lanes)) != len(LANES)
        or set(observed_lanes) != set(LANES)
    ):
        raise RunnerError("lane manifests are missing or duplicated")
    for path, value, lane in parsed_manifests:
        referenced_bindings = _preflight_lane_artifacts(path, value, lane)
        for _, referenced_identity, _ in referenced_bindings:
            if referenced_identity in observed_files:
                raise RunnerError("release evidence paths alias the same file")
            observed_files.add(referenced_identity)
        manifest_bindings.extend(referenced_bindings)
    references.sort(key=lambda item: LANES.index(item["lane"]))

    verifier = _verifier_module()
    output = _validate_output_path(repo, output_dir)
    if (
        not _directory_path_has_identity(repo, repo_identity)
        or not _directory_path_has_identity(specs, specs_identity)
        or not all(
            _path_regular_file_has_identity(path, identity, size)
            for path, identity, size in manifest_bindings
        )
    ):
        raise RunnerError("qualification input identity changed during preflight")
    manyforge = _candidate_source(repo)
    specs_source, specs_contract = _source_bundle(specs)
    if (
        not _directory_path_has_identity(repo, repo_identity)
        or not _directory_path_has_identity(specs, specs_identity)
        or not all(
            _path_regular_file_has_identity(path, identity, size)
            for path, identity, size in manifest_bindings
        )
    ):
        raise RunnerError("qualification input identity changed during source binding")

    artifact_manifest = output / "artifact-manifest.json"
    verification_summary = output / "verification-summary.json"
    handles = _create_output_directory(
        repo,
        output,
        repo_identity,
    )
    artifact_identity: tuple[int, int] | None = None
    summary_identity: tuple[int, int] | None = None
    succeeded = False
    try:
        if not _output_directory_chain_matches(
            repo,
            output.name,
            handles,
        ):
            raise RunnerError("qualification output directory identity changed")
        artifact_identity = _write_exclusive_at(
            handles.output,
            "artifact-manifest.json",
            {
                "schema_version": ARTIFACT_MANIFEST_SCHEMA,
                "verification_scope": VERIFICATION_SCOPE,
                "candidate": {
                    "manyforge_root": str(repo),
                    "specs_root": str(specs),
                    "source_identity": manyforge,
                    "specs_source_identity": specs_source,
                    "specs_contract_identity": specs_contract,
                },
                "lanes": references,
                "overall_step18_closure_claimed": False,
                "requires_common_candidate_aggregator": True,
            },
        )
        if not _regular_file_matches_identity(
            handles.output,
            "artifact-manifest.json",
            artifact_identity,
        ):
            raise RunnerError("artifact manifest path identity changed")
        try:
            result = verifier.verify_artifact_manifest(artifact_manifest)
            if (
                not _output_directory_chain_matches(repo, output.name, handles)
                or not _directory_path_has_identity(specs, specs_identity)
                or not all(
                    _path_regular_file_has_identity(path, identity, size)
                    for path, identity, size in manifest_bindings
                )
                or not _regular_file_matches_identity(
                    handles.output,
                    "artifact-manifest.json",
                    artifact_identity,
                )
            ):
                raise RunnerError(
                    "qualification paths changed during artifact verification"
                )
            expected_summary = _render_bounded_json(
                "verification-summary.json",
                result,
            )
            verifier.publish_verification_summary(
                verification_summary, result, artifact_manifest
            )
        except (verifier.ValidationError, RecursionError, TypeError) as error:
            raise RunnerError(
                f"release artifact verification failed: {error}"
            ) from error
        summary_identity = _regular_file_identity_at(
            handles.output,
            "verification-summary.json",
        )
        (
            observed_summary,
            observed_summary_identity,
        ) = _read_bounded_regular_file_at(
            handles.output,
            "verification-summary.json",
        )
        if (
            observed_summary_identity != summary_identity
            or observed_summary != expected_summary
        ):
            raise RunnerError("published verification summary content differs")
        if not _output_directory_chain_matches(
            repo,
            output.name,
            handles,
        ) or not all(
            _path_regular_file_has_identity(path, identity, size)
            for path, identity, size in manifest_bindings
        ):
            raise RunnerError("qualification output directory identity changed")
        if not _regular_file_matches_identity(
            handles.output,
            "artifact-manifest.json",
            artifact_identity,
        ) or not _regular_file_matches_identity(
            handles.output,
            "verification-summary.json",
            summary_identity,
        ):
            raise RunnerError("published qualification artifact identity changed")
        os.fsync(handles.output)
        os.fsync(handles.qualification)
        os.fsync(handles.variable)
        os.fsync(handles.repo)
        succeeded = True
    finally:
        if not succeeded:
            _cleanup_failed_output(
                handles,
                output.name,
                artifact_identity,
                summary_identity,
            )
        _close_output_handles(handles)
    print("Step 18 retained release artifacts passed offline verification")
    print(f"artifact manifest: {artifact_manifest}")
    print(f"verification summary: {verification_summary}")
    return artifact_manifest, verification_summary


def emit_lane_unavailable() -> int:
    payload = {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "execution_available": False,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "historical_source": HISTORICAL_SOURCE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
        "assignments": {
            lane: assignment._asdict()
            for lane, assignment in LANE_ASSIGNMENTS.items()
        },
        "overall_step18_closure_claimed": False,
    }
    print(json.dumps(payload, sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


def main(argv: list[str] | None = None) -> int:
    effective_argv = list(sys.argv[1:] if argv is None else argv)
    # The historical producer selector is recognized without parsing any of
    # its lane/path inputs. Even malformed invocations therefore reach the
    # same stable unavailable result before helper imports, caller-supplied
    # path traversal, subprocesses, or filesystem output.
    if effective_argv and effective_argv[0] == "lane":
        return emit_lane_unavailable()

    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    lane_parser = subparsers.add_parser(
        "lane", help="report the restoration gate for one historical lane"
    )
    lane_parser.add_argument("--lane", choices=LANES, required=True)
    # Retain the historical CLI shape, but do not resolve or inspect these
    # paths on the unavailable producer branch.
    lane_parser.add_argument("--specs-root", type=Path, required=True)
    lane_parser.add_argument("--output-dir", type=Path, required=True)
    lane_parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)

    assemble_parser = subparsers.add_parser(
        "assemble", help="verify and aggregate seven existing lane artifacts"
    )
    assemble_parser.add_argument("--specs-root", type=Path, required=True)
    assemble_parser.add_argument("--output-dir", type=Path, required=True)
    assemble_parser.add_argument(
        "--lane-manifest", type=Path, action="append", required=True
    )
    assemble_parser.add_argument(
        "--repo-root", type=Path, default=REPO_ROOT
    )

    describe_parser = subparsers.add_parser(
        "describe", help="print one lane's relocation assignment"
    )
    describe_parser.add_argument("--lane", choices=LANES, required=True)

    args = parser.parse_args(effective_argv)
    if args.command == "describe":
        print(json.dumps(describe_lane(args.lane), indent=2, sort_keys=True))
        return 0
    try:
        assemble(
            repo_root=args.repo_root,
            specs_root=args.specs_root,
            output_dir=args.output_dir,
            lane_manifests=args.lane_manifest,
        )
    except (OSError, RunnerError, ValueError) as error:
        print(f"Step 18 release artifact assembly failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
