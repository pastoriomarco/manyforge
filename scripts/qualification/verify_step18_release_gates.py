#!/usr/bin/env python3
"""Verify the corrected-source Step 18 release-gate campaign.

This verifier owns only the conventional regression, architecture-hygiene,
and sanitizer gates.  A passing summary deliberately does not claim overall
Step 18 closure; the common-candidate aggregator must combine it with the
proof, benchmark, loaded-kernel, and live-service evidence families.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import re
import secrets
import selectors
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Iterator, NamedTuple

from source_identity import (
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity,
    validate_specs_contract_identity,
)


LANE_SCHEMA = "manyforge.step18.release_gate_lane.v1"
ARTIFACT_MANIFEST_SCHEMA = "manyforge.step18.release_gate_artifact_manifest.v1"
SUMMARY_SCHEMA = "manyforge.step18.release_gate_verification.v1"
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
SANITIZER_LANES = frozenset(
    {
        "core_asan_ubsan",
        "core_tsan",
        "collision_asan_ubsan",
        "collision_tsan",
    }
)
ROS_FULL_LANES = frozenset({"full_ros_regression", *SANITIZER_LANES})
ROS_AMBIENT_PASSTHROUGH_VARIABLES = (
    "FASTDDS_DEFAULT_PROFILES_FILE",
    "FASTRTPS_DEFAULT_PROFILES_FILE",
    "ROS_AUTOMATIC_DISCOVERY_RANGE",
    "ROS_STATIC_PEERS",
)

_LANE_SERVICE = {
    "full_dev_regression": "dev",
    "full_ros_regression": "ros-jazzy-full",
    "contract_architecture_hygiene": "dev",
    "core_asan_ubsan": "ros-jazzy-full",
    "core_tsan": "ros-jazzy-full",
    "collision_asan_ubsan": "ros-jazzy-full",
    "collision_tsan": "ros-jazzy-full",
}
_SANITIZER_PREFIX = {
    "core_asan_ubsan": "CORE_ASAN_UBSAN",
    "core_tsan": "CORE_TSAN",
    "collision_asan_ubsan": "COLLISION_ASAN_UBSAN",
    "collision_tsan": "COLLISION_TSAN",
}
_SANITIZER_SCRIPT = {
    "core_asan_ubsan": "scripts/ci/run-core-asan.sh",
    "core_tsan": "scripts/ci/run-core-tsan.sh",
    "collision_asan_ubsan": "scripts/ci/run-collision-engine-asan.sh",
    "collision_tsan": "scripts/ci/run-collision-engine-tsan.sh",
}
HYGIENE_CHECKS = (
    (
        "generated_public_contracts",
        "bash scripts/ci/check-public-contracts-fresh.sh structural-rest",
    ),
    ("version_sync", "bash scripts/ci/verify-version-sync.sh"),
    ("private_spec_boundary", "bash scripts/ci/check-private-spec-boundary.sh"),
    ("terminal_commit_boundary", "bash scripts/ci/check-terminal-commit-boundary.sh"),
    ("authority_port_alignment", "python3 scripts/ci/check-authority-port-alignment.py"),
    ("planner_portability", "python3 scripts/ci/check-planner-portability.py"),
    ("roboplan_dependency_stack", "python3 scripts/ci/check-roboplan-dependency-stack.py"),
    ("viewer_boundary", "python3 scripts/ci/check-viewer-data-plane-boundaries.py"),
    (
        "step18_removed_field_and_distance_scope",
        "python3 scripts/qualification/verify_step18_release_gates.py "
        "hygiene --repo-root /workspace",
    ),
)

_BASE_INPUT_PATHS = {
    "docker-compose.yaml",
    "scripts/qualification/run_step18_release_gates.py",
    "scripts/qualification/source_identity.py",
    "scripts/qualification/verify_step18_release_gates.py",
}
_HYGIENE_INPUT_PATHS = {
    command.split()[1]
    for _, command in HYGIENE_CHECKS[:-1]
}
_LANE_INPUT_PATHS = {
    "full_dev_regression": {"scripts/test-all.sh"},
    "full_ros_regression": {
        "scripts/ros/build-ros2-control-runtime.sh",
    },
    "contract_architecture_hygiene": _HYGIENE_INPUT_PATHS,
    "core_asan_ubsan": {
        "scripts/ci/run-core-asan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
    },
    "core_tsan": {
        "scripts/ci/run-core-tsan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
        "scripts/ci/tsan.supp",
    },
    "collision_asan_ubsan": {
        "scripts/ci/run-collision-engine-asan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
    },
    "collision_tsan": {
        "scripts/ci/run-collision-engine-tsan.sh",
        "scripts/ci/run-ctest-gtest-binaries-grouped.py",
        "scripts/ci/tsan.supp",
    },
}

_SHA256 = re.compile(r"[0-9a-f]{64}")
_UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)
_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_SANITIZER_FINDING = re.compile(
    r"(?:"
    r"ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|"
    r"ERROR: LeakSanitizer|SUMMARY: LeakSanitizer|"
    r"WARNING: ThreadSanitizer|SUMMARY: ThreadSanitizer|"
    r"ThreadSanitizer: reported|"
    r"UndefinedBehaviorSanitizer|runtime error:|"
    r"Sanitizer CHECK failed"
    r")",
    flags=re.IGNORECASE,
)

MAX_ARTIFACT_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_LANE_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_CONTAINER_INSPECT_BYTES = 4 * 1024 * 1024
MAX_CONTAINER_LOG_BYTES = 64 * 1024 * 1024
MAX_SECURITY_OVERRIDE_BYTES = 64 * 1024
MAX_QUALIFICATION_INPUT_BYTES = 16 * 1024 * 1024
MAX_OUTPUT_ARTIFACT_BYTES = 8 * 1024 * 1024
MAX_REFERENCE_PATH_BYTES = 4096
MAX_JSON_COLLECTION_ITEMS = 4096
MAX_JSON_DEPTH = 32
MAX_JSON_NODES = 65_536
MAX_JSON_KEY_BYTES = 256
MAX_JSON_STRING_BYTES = 1024 * 1024
GIT_CHECK_TIMEOUT_SECONDS = 5
MAX_HYGIENE_DIRECTORY_ENTRIES = 8192
MAX_HYGIENE_DIRECTORIES = 8192
MAX_HYGIENE_FILES = 8192
MAX_HYGIENE_FILE_BYTES = 64 * 1024 * 1024
MAX_HYGIENE_TOTAL_BYTES = 512 * 1024 * 1024
MAX_HYGIENE_DEPTH = 64
HYGIENE_TIMEOUT_SECONDS = 120
MAX_HYGIENE_GIT_INVENTORY_BYTES = 16 * 1024 * 1024

_HYGIENE_EXCLUDED_COMPONENTS = frozenset(
    {
        ".git",
        ".manyforge-var",
        ".pytest_cache",
        "build",
        "__pycache__",
        "node_modules",
    }
)
_HYGIENE_ALLOWED_SYMLINKS = {
    "agent-skills/manyforge-composer/manyforge-mcp-bridge.py": (
        "../../scripts/manyforge-mcp-bridge.py"
    ),
}


class _FileSnapshot(NamedTuple):
    path: Path
    payload: bytes
    sha256: str
    size: int
    identity: tuple[int, int]


class _BoundDirectory:
    def __init__(self, path: Path, descriptors: list[int], names: list[str]) -> None:
        self.path = path
        self._descriptors = descriptors
        self._names = names

    @property
    def descriptor(self) -> int:
        return self._descriptors[-1]

    def assert_current(self, label: str) -> None:
        for parent, name, child in zip(
            self._descriptors,
            self._names,
            self._descriptors[1:],
        ):
            try:
                path_metadata = os.stat(
                    name,
                    dir_fd=parent,
                    follow_symlinks=False,
                )
                descriptor_metadata = os.fstat(child)
            except OSError as error:
                raise ValidationError(f"{label} path changed: {error}") from error
            if (
                not stat.S_ISDIR(path_metadata.st_mode)
                or (path_metadata.st_dev, path_metadata.st_ino)
                != (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
            ):
                raise ValidationError(f"{label} path changed")

    def close(self) -> None:
        for descriptor in reversed(self._descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        self._descriptors.clear()


class ValidationError(RuntimeError):
    """A retained release-gate claim is incomplete or inconsistent."""


def _metadata_signature(
    metadata: os.stat_result,
) -> tuple[int, int, int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


def _lexical_absolute(path: Path, label: str) -> Path:
    raw = os.fspath(path)
    if not isinstance(raw, str) or not raw or "\0" in raw:
        raise ValidationError(f"{label} path is invalid")
    if len(os.fsencode(raw)) > MAX_REFERENCE_PATH_BYTES:
        raise ValidationError(f"{label} path exceeds the reference bound")
    return Path(os.path.abspath(os.path.normpath(raw)))


def _open_absolute_directory(path: Path, label: str) -> _BoundDirectory:
    absolute = _lexical_absolute(path, label)
    try:
        descriptors = [
            os.open(
                os.path.sep,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            )
        ]
    except OSError as error:
        raise ValidationError(f"cannot bind filesystem root: {error}") from error
    names: list[str] = []
    try:
        for name in absolute.parts[1:]:
            if name in ("", ".", ".."):
                raise ValidationError(f"{label} path is not canonical")
            descriptor = os.open(
                name,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=descriptors[-1],
            )
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                os.close(descriptor)
                raise ValidationError(f"{label} is not a directory")
            descriptors.append(descriptor)
            names.append(name)
        binding = _BoundDirectory(absolute, descriptors, names)
        binding.assert_current(label)
        return binding
    except OSError as error:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise ValidationError(
            f"cannot bind {label} without symbolic-link traversal: {error}"
        ) from error
    except BaseException:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise


def _path_matches_file(
    parent_descriptor: int,
    name: str,
    identity: tuple[int, int],
    *,
    links: int = 1,
) -> bool:
    try:
        metadata = os.stat(
            name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
    except OSError:
        return False
    return (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == links
        and (metadata.st_dev, metadata.st_ino) == identity
    )


def _read_bounded_regular_file(
    path: Path,
    label: str,
    max_bytes: int,
) -> _FileSnapshot:
    absolute = _lexical_absolute(path, label)
    if absolute.name in ("", ".", ".."):
        raise ValidationError(f"{label} must name a file")
    parent = _open_absolute_directory(absolute.parent, f"{label} parent")
    descriptor = -1
    try:
        try:
            descriptor = os.open(
                absolute.name,
                os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=parent.descriptor,
            )
        except OSError as error:
            raise ValidationError(f"cannot open {label}: {error}") from error
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise ValidationError(
                f"{label} must be one non-hardlinked regular file"
            )
        if metadata.st_size < 0 or metadata.st_size > max_bytes:
            raise ValidationError(f"{label} exceeds the {max_bytes}-byte bound")
        payload = bytearray()
        remaining = metadata.st_size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise ValidationError(f"{label} ended before its recorded size")
            payload.extend(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ValidationError(f"{label} grew while being read")
        after = os.fstat(descriptor)
        identity = (metadata.st_dev, metadata.st_ino)
        parent.assert_current(f"{label} parent")
        if (
            _metadata_signature(after) != _metadata_signature(metadata)
            or not _path_matches_file(
                parent.descriptor,
                absolute.name,
                identity,
            )
        ):
            raise ValidationError(f"{label} changed while being read")
        data = bytes(payload)
        return _FileSnapshot(
            absolute,
            data,
            hashlib.sha256(data).hexdigest(),
            len(data),
            identity,
        )
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        parent.close()


def _relative_parts(value: str | Path, label: str) -> tuple[str, ...]:
    raw = os.fspath(value)
    if (
        not isinstance(raw, str)
        or not raw
        or "\0" in raw
        or os.path.isabs(raw)
        or len(os.fsencode(raw)) > MAX_REFERENCE_PATH_BYTES
    ):
        raise ValidationError(f"{label} relative path is invalid")
    parts = Path(raw).parts
    if not parts or any(part in ("", ".", "..") for part in parts):
        raise ValidationError(f"{label} relative path is not canonical")
    return tuple(parts)


def _open_relative_directory(
    root_descriptor: int,
    relative: str | Path,
    label: str,
) -> _BoundDirectory:
    parts = _relative_parts(relative, label)
    descriptors = [os.dup(root_descriptor)]
    names: list[str] = []
    try:
        for name in parts:
            descriptor = os.open(
                name,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=descriptors[-1],
            )
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                os.close(descriptor)
                raise ValidationError(f"{label} is not a directory")
            descriptors.append(descriptor)
            names.append(name)
        binding = _BoundDirectory(Path(*parts), descriptors, names)
        binding.assert_current(label)
        return binding
    except OSError as error:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise ValidationError(
            f"cannot bind {label} without symbolic-link traversal: {error}"
        ) from error
    except BaseException:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise


def _read_bounded_regular_file_at(
    root_descriptor: int,
    relative: str | Path,
    label: str,
    max_bytes: int,
) -> _FileSnapshot:
    parts = _relative_parts(relative, label)
    parent: _BoundDirectory | None = None
    descriptor = -1
    try:
        if len(parts) == 1:
            parent_descriptor = root_descriptor
        else:
            parent = _open_relative_directory(
                root_descriptor,
                Path(*parts[:-1]),
                f"{label} parent",
            )
            parent_descriptor = parent.descriptor
        try:
            descriptor = os.open(
                parts[-1],
                os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise ValidationError(f"cannot open {label}: {error}") from error
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise ValidationError(
                f"{label} must be one non-hardlinked regular file"
            )
        if before.st_size < 0 or before.st_size > max_bytes:
            raise ValidationError(f"{label} exceeds the {max_bytes}-byte bound")
        payload = bytearray()
        remaining = before.st_size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise ValidationError(f"{label} ended before its recorded size")
            payload.extend(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ValidationError(f"{label} grew while being read")
        after = os.fstat(descriptor)
        identity = (before.st_dev, before.st_ino)
        if parent is not None:
            parent.assert_current(f"{label} parent")
        if (
            _metadata_signature(after) != _metadata_signature(before)
            or not _path_matches_file(
                parent_descriptor,
                parts[-1],
                identity,
            )
        ):
            raise ValidationError(f"{label} changed while being read")
        data = bytes(payload)
        return _FileSnapshot(
            Path(*parts),
            data,
            hashlib.sha256(data).hexdigest(),
            len(data),
            identity,
        )
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if parent is not None:
            parent.close()


def _open_relative_parent_binding(
    repository: _BoundDirectory,
    parts: tuple[str, ...],
    label: str,
) -> _BoundDirectory:
    if len(parts) == 1:
        return _BoundDirectory(Path("."), [os.dup(repository.descriptor)], [])
    return _open_relative_directory(
        repository.descriptor,
        Path(*parts[:-1]),
        label,
    )


def _resolve_hygiene_symlink_target(relative: str, target: str) -> str:
    if not target or "\0" in target or target.startswith("/"):
        raise ValidationError(
            f"hygiene symbolic link target is not repository-relative: {relative}"
        )
    resolved = list(_relative_parts(relative, "hygiene symbolic link"))[:-1]
    for component in target.split("/"):
        if component in ("", "."):
            continue
        if component == "..":
            if not resolved:
                raise ValidationError(
                    f"hygiene symbolic link escapes the repository: {relative}"
                )
            resolved.pop()
            continue
        if "\0" in component:
            raise ValidationError(
                f"hygiene symbolic link target is invalid: {relative}"
            )
        resolved.append(component)
    if not resolved:
        raise ValidationError(
            f"hygiene symbolic link does not name a file: {relative}"
        )
    result = "/".join(resolved)
    _relative_parts(result, "hygiene symbolic link target")
    return result


class _AllowedHygieneSymlink:
    def __init__(
        self,
        *,
        repository: _BoundDirectory,
        relative: str,
        expected_target: str,
        observed_metadata: os.stat_result,
        root_device: int,
        attested_paths: frozenset[str] | None,
    ) -> None:
        self.relative = relative
        self.expected_target = expected_target
        self.target_relative = _resolve_hygiene_symlink_target(
            relative,
            expected_target,
        )
        if (
            attested_paths is not None
            and self.target_relative not in attested_paths
        ):
            raise ValidationError(
                "hygiene symbolic link target is absent from the candidate "
                f"identity: {self.target_relative}"
            )
        link_parts = _relative_parts(relative, "hygiene symbolic link")
        target_parts = _relative_parts(
            self.target_relative,
            "hygiene symbolic link target",
        )
        self.link_parent = _open_relative_parent_binding(
            repository,
            link_parts,
            f"hygiene symbolic link parent {relative}",
        )
        self.link_descriptor = -1
        self.target_parent: _BoundDirectory | None = None
        self.target_descriptor = -1
        try:
            self.target_parent = _open_relative_parent_binding(
                repository,
                target_parts,
                f"hygiene symbolic link target parent {self.target_relative}",
            )
            link_path_flag = getattr(os, "O_PATH", None)
            if link_path_flag is None:
                raise ValidationError(
                    "cannot bind hygiene symbolic link identity without O_PATH"
                )
            try:
                self.link_descriptor = os.open(
                    link_parts[-1],
                    link_path_flag | os.O_NOFOLLOW | os.O_CLOEXEC,
                    dir_fd=self.link_parent.descriptor,
                )
            except OSError as error:
                raise ValidationError(
                    f"cannot bind hygiene symbolic link {relative}: {error}"
                ) from error
            link_metadata = os.fstat(self.link_descriptor)
            current_link_metadata = os.stat(
                link_parts[-1],
                dir_fd=self.link_parent.descriptor,
                follow_symlinks=False,
            )
            target_text = os.readlink(
                link_parts[-1],
                dir_fd=self.link_parent.descriptor,
            )
            if (
                not stat.S_ISLNK(link_metadata.st_mode)
                or _metadata_signature(link_metadata)
                != _metadata_signature(observed_metadata)
                or _metadata_signature(current_link_metadata)
                != _metadata_signature(link_metadata)
                or target_text != expected_target
            ):
                raise ValidationError(
                    f"hygiene symbolic link changed while being bound: {relative}"
                )
            self.link_signature = _metadata_signature(link_metadata)
            try:
                self.target_descriptor = os.open(
                    target_parts[-1],
                    os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=self.target_parent.descriptor,
                )
            except OSError as error:
                raise ValidationError(
                    "cannot bind hygiene symbolic link target "
                    f"{self.target_relative}: {error}"
                ) from error
            target_metadata = os.fstat(self.target_descriptor)
            if (
                not stat.S_ISREG(target_metadata.st_mode)
                or target_metadata.st_nlink != 1
                or target_metadata.st_dev != root_device
            ):
                raise ValidationError(
                    "hygiene symbolic link target must be one in-repository "
                    f"non-hardlinked regular file: {self.target_relative}"
                )
            self.target_signature = _metadata_signature(target_metadata)
            self.target_identity = (
                target_metadata.st_dev,
                target_metadata.st_ino,
            )
            self.link_parent.assert_current(
                f"hygiene symbolic link parent {relative}"
            )
            self.target_parent.assert_current(
                f"hygiene symbolic link target parent {self.target_relative}"
            )
            if not _path_matches_file(
                self.target_parent.descriptor,
                target_parts[-1],
                self.target_identity,
            ):
                raise ValidationError(
                    f"hygiene symbolic link target changed: {self.target_relative}"
                )
        except BaseException:
            self.close()
            raise

    def assert_current(self, scanned_regular_paths: set[str]) -> None:
        if self.target_relative not in scanned_regular_paths:
            raise ValidationError(
                "hygiene symbolic link target was not scanned as source: "
                f"{self.target_relative}"
            )
        link_parts = _relative_parts(self.relative, "hygiene symbolic link")
        target_parts = _relative_parts(
            self.target_relative,
            "hygiene symbolic link target",
        )
        assert self.target_parent is not None
        self.link_parent.assert_current(
            f"hygiene symbolic link parent {self.relative}"
        )
        self.target_parent.assert_current(
            f"hygiene symbolic link target parent {self.target_relative}"
        )
        try:
            link_metadata = os.stat(
                link_parts[-1],
                dir_fd=self.link_parent.descriptor,
                follow_symlinks=False,
            )
            bound_link_metadata = os.fstat(self.link_descriptor)
            target_text = os.readlink(
                link_parts[-1],
                dir_fd=self.link_parent.descriptor,
            )
            target_metadata = os.fstat(self.target_descriptor)
        except OSError as error:
            raise ValidationError(
                f"hygiene symbolic link changed during traversal: {self.relative}: "
                f"{error}"
            ) from error
        if (
            not stat.S_ISLNK(link_metadata.st_mode)
            or _metadata_signature(bound_link_metadata) != self.link_signature
            or _metadata_signature(link_metadata)
            != _metadata_signature(bound_link_metadata)
            or target_text != self.expected_target
        ):
            raise ValidationError(
                f"hygiene symbolic link changed during traversal: {self.relative}"
            )
        if (
            _metadata_signature(target_metadata) != self.target_signature
            or not _path_matches_file(
                self.target_parent.descriptor,
                target_parts[-1],
                self.target_identity,
            )
        ):
            raise ValidationError(
                "hygiene symbolic link target changed during traversal: "
                f"{self.target_relative}"
            )

    def close(self) -> None:
        if self.link_descriptor >= 0:
            try:
                os.close(self.link_descriptor)
            except OSError:
                pass
            self.link_descriptor = -1
        if self.target_descriptor >= 0:
            try:
                os.close(self.target_descriptor)
            except OSError:
                pass
            self.target_descriptor = -1
        if self.target_parent is not None:
            self.target_parent.close()
            self.target_parent = None
        self.link_parent.close()


@contextlib.contextmanager
def _hygiene_repository(
    value: Path | _BoundDirectory,
) -> Iterator[_BoundDirectory]:
    if isinstance(value, _BoundDirectory):
        value.assert_current("hygiene repository")
        yield value
        value.assert_current("hygiene repository")
        return
    binding = _open_absolute_directory(value, "hygiene repository")
    try:
        yield binding
        binding.assert_current("hygiene repository")
    finally:
        binding.close()


def _hygiene_component_excluded(name: str) -> bool:
    return name in _HYGIENE_EXCLUDED_COMPONENTS or name.startswith("build-")


def _read_tree_file(
    directory_descriptor: int,
    name: str,
    relative: str,
    budget: dict[str, int],
) -> bytes:
    descriptor = -1
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=directory_descriptor,
        )
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise ValidationError(
                f"hygiene source must be one non-hardlinked regular file: {relative}"
            )
        if before.st_size < 0 or before.st_size > MAX_HYGIENE_FILE_BYTES:
            raise ValidationError(
                f"hygiene source exceeds the per-file byte bound: {relative}"
            )
        if budget["bytes"] + before.st_size > MAX_HYGIENE_TOTAL_BYTES:
            raise ValidationError("hygiene source exceeds the aggregate byte bound")
        budget["files"] += 1
        budget["bytes"] += before.st_size
        if budget["files"] > MAX_HYGIENE_FILES:
            raise ValidationError("hygiene source exceeds the file-count bound")
        payload = bytearray()
        remaining = before.st_size
        while remaining:
            if time.monotonic_ns() > budget["deadline_ns"]:
                raise ValidationError("hygiene source exceeds the wall-time bound")
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise ValidationError(
                    f"hygiene source ended before its recorded size: {relative}"
                )
            payload.extend(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ValidationError(f"hygiene source grew while being read: {relative}")
        after = os.fstat(descriptor)
        if (
            _metadata_signature(after) != _metadata_signature(before)
            or not _path_matches_file(
                directory_descriptor,
                name,
                (before.st_dev, before.st_ino),
            )
        ):
            raise ValidationError(f"hygiene source changed while being read: {relative}")
        return bytes(payload)
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _hygiene_attested_directories(
    attested_paths: frozenset[str] | None,
) -> frozenset[str] | None:
    if attested_paths is None:
        return None
    if not isinstance(attested_paths, frozenset):
        raise ValidationError("candidate source path inventory must be a frozenset")
    if len(attested_paths) > MAX_HYGIENE_FILES:
        raise ValidationError(
            "candidate source identity inventory exceeds the file-count bound"
        )
    directories: set[str] = set()
    input_bytes = 0
    for path in attested_paths:
        if not isinstance(path, str):
            raise ValidationError("candidate source identity path must be text")
        try:
            encoded = os.fsencode(path)
        except (TypeError, UnicodeEncodeError) as error:
            raise ValidationError(
                "candidate source identity path cannot be encoded"
            ) from error
        input_bytes += len(encoded) + 1
        if input_bytes > MAX_HYGIENE_GIT_INVENTORY_BYTES:
            raise ValidationError(
                "candidate source identity inventory exceeds the byte bound"
            )
        parts = _relative_parts(path, "candidate source identity")
        if "/".join(parts) != path:
            raise ValidationError(
                "candidate source identity path is not canonical"
            )
        if len(parts) > MAX_HYGIENE_DEPTH + 1:
            raise ValidationError(
                "candidate source identity path exceeds the directory-depth bound"
            )
        for index in range(1, len(parts)):
            directories.add("/".join(parts[:index]))
            if len(directories) > MAX_HYGIENE_DIRECTORIES:
                raise ValidationError(
                    "candidate source directory inventory exceeds the "
                    "directory-count bound"
                )
    return frozenset(directories)


def _iter_hygiene_files(
    repository: _BoundDirectory,
    *,
    relative_root: str | None = None,
    suffixes: frozenset[str] | None = None,
    attested_paths: frozenset[str] | None = None,
    reject_unattested: bool = False,
) -> Iterator[tuple[str, bytes]]:
    budget = {
        "directories": 0,
        "files": 0,
        "bytes": 0,
        "deadline_ns": time.monotonic_ns()
        + HYGIENE_TIMEOUT_SECONDS * 1_000_000_000,
    }
    attested_directories = _hygiene_attested_directories(attested_paths)
    root_binding: _BoundDirectory | None = None
    if relative_root is None:
        root_descriptor = os.dup(repository.descriptor)
        root_prefix = ""
    else:
        root_binding = _open_relative_directory(
            repository.descriptor,
            relative_root,
            f"production source root {relative_root}",
        )
        root_descriptor = os.dup(root_binding.descriptor)
        root_prefix = "/".join(_relative_parts(relative_root, "production source root"))
    root_device = os.fstat(root_descriptor).st_dev
    allowed_symlinks: list[_AllowedHygieneSymlink] = []
    scanned_regular_paths: set[str] = set()

    def directory_names(descriptor: int, prefix: str) -> tuple[str, ...]:
        names: list[str] = []
        with os.scandir(descriptor) as entries:
            for entry in entries:
                if time.monotonic_ns() > budget["deadline_ns"]:
                    raise ValidationError(
                        "hygiene source exceeds the wall-time bound"
                    )
                names.append(entry.name)
                if len(names) > MAX_HYGIENE_DIRECTORY_ENTRIES:
                    raise ValidationError(
                        "hygiene directory exceeds the entry bound: "
                        f"{prefix or '.'}"
                    )
        return tuple(sorted(names))

    def visit(
        descriptor: int,
        prefix: str,
        depth: int,
    ) -> Iterator[tuple[str, bytes]]:
        if depth > MAX_HYGIENE_DEPTH:
            raise ValidationError("hygiene source exceeds the directory-depth bound")
        budget["directories"] += 1
        if budget["directories"] > MAX_HYGIENE_DIRECTORIES:
            raise ValidationError("hygiene source exceeds the directory-count bound")
        directory_before = os.fstat(descriptor)
        if (
            not stat.S_ISDIR(directory_before.st_mode)
            or directory_before.st_dev != root_device
        ):
            raise ValidationError(
                f"hygiene directory binding changed: {prefix or '.'}"
            )
        try:
            names = directory_names(descriptor, prefix)
            for name in names:
                if time.monotonic_ns() > budget["deadline_ns"]:
                    raise ValidationError("hygiene source exceeds the wall-time bound")
                if name in ("", ".", "..") or "\0" in name:
                    raise ValidationError("hygiene source contains an invalid path name")
                relative = f"{prefix}/{name}" if prefix else name
                if len(os.fsencode(relative)) > MAX_REFERENCE_PATH_BYTES:
                    raise ValidationError("hygiene source path exceeds the reference bound")
                try:
                    metadata = os.stat(
                        name,
                        dir_fd=descriptor,
                        follow_symlinks=False,
                    )
                except OSError as error:
                    raise ValidationError(
                        f"cannot inspect hygiene source {relative}: {error}"
                    ) from error
                if stat.S_ISDIR(metadata.st_mode):
                    excluded_without_attested_source = (
                        _hygiene_component_excluded(name)
                        and attested_directories is not None
                        and relative not in attested_directories
                    )
                    if excluded_without_attested_source and not reject_unattested:
                        continue
                    if (
                        attested_directories is not None
                        and relative not in attested_directories
                    ):
                        if reject_unattested:
                            raise ValidationError(
                                "production source directory is absent from the "
                                f"candidate identity: {relative}"
                            )
                        continue
                    if metadata.st_dev != root_device:
                        raise ValidationError(
                            f"hygiene source crosses a filesystem boundary: {relative}"
                        )
                    try:
                        child = os.open(
                            name,
                            os.O_RDONLY
                            | os.O_CLOEXEC
                            | os.O_DIRECTORY
                            | os.O_NOFOLLOW,
                            dir_fd=descriptor,
                        )
                    except OSError as error:
                        raise ValidationError(
                            f"cannot bind hygiene directory {relative}: {error}"
                        ) from error
                    try:
                        child_metadata = os.fstat(child)
                        if (
                            not stat.S_ISDIR(child_metadata.st_mode)
                            or _metadata_signature(child_metadata)
                            != _metadata_signature(metadata)
                        ):
                            raise ValidationError(
                                f"hygiene directory changed before traversal: {relative}"
                            )
                        yield from visit(child, relative, depth + 1)
                        current = os.stat(
                            name,
                            dir_fd=descriptor,
                            follow_symlinks=False,
                        )
                        if (
                            _metadata_signature(os.fstat(child))
                            != _metadata_signature(child_metadata)
                            or _metadata_signature(current)
                            != _metadata_signature(child_metadata)
                        ):
                            raise ValidationError(
                                f"hygiene directory changed during traversal: {relative}"
                            )
                    finally:
                        os.close(child)
                    continue
                if stat.S_ISLNK(metadata.st_mode):
                    if attested_paths is not None and relative not in attested_paths:
                        if reject_unattested:
                            raise ValidationError(
                                f"production source is absent from the candidate "
                                f"identity: {relative}"
                            )
                        continue
                    expected_target = _HYGIENE_ALLOWED_SYMLINKS.get(relative)
                    if expected_target is None:
                        raise ValidationError(
                            f"hygiene source contains a symbolic link: {relative}"
                        )
                    try:
                        target = os.readlink(name, dir_fd=descriptor)
                    except OSError as error:
                        raise ValidationError(
                            f"cannot inspect hygiene symbolic link {relative}: {error}"
                        ) from error
                    if target != expected_target:
                        raise ValidationError(
                            f"hygiene symbolic link target changed: {relative}"
                        )
                    allowed_symlinks.append(
                        _AllowedHygieneSymlink(
                            repository=repository,
                            relative=relative,
                            expected_target=expected_target,
                            observed_metadata=metadata,
                            root_device=root_device,
                            attested_paths=attested_paths,
                        )
                    )
                    continue
                if not stat.S_ISREG(metadata.st_mode):
                    raise ValidationError(
                        f"hygiene source contains a special file: {relative}"
                    )
                if attested_paths is not None and relative not in attested_paths:
                    if reject_unattested:
                        raise ValidationError(
                            f"production source is absent from the candidate "
                            f"identity: {relative}"
                        )
                    continue
                if suffixes is not None and Path(name).suffix not in suffixes:
                    continue
                payload = _read_tree_file(descriptor, name, relative, budget)
                scanned_regular_paths.add(relative)
                yield relative, payload
            if directory_names(descriptor, prefix) != names:
                raise ValidationError(
                    f"hygiene directory changed during traversal: {prefix or '.'}"
                )
        except OSError as error:
            raise ValidationError(
                f"cannot traverse hygiene directory {prefix or '.'}: {error}"
            ) from error
        finally:
            try:
                directory_after = os.fstat(descriptor)
            except OSError as error:
                raise ValidationError(
                    f"hygiene directory changed during traversal: "
                    f"{prefix or '.'}: {error}"
                ) from error
            if _metadata_signature(directory_after) != _metadata_signature(
                directory_before
            ):
                raise ValidationError(
                    f"hygiene directory changed during traversal: {prefix or '.'}"
                )

    try:
        if (
            reject_unattested
            and attested_directories is not None
            and root_prefix
            and root_prefix not in attested_directories
        ):
            raise ValidationError(
                "production source root is absent from the candidate identity: "
                f"{root_prefix}"
            )
        yield from visit(root_descriptor, root_prefix, 0)
        for allowed_symlink in allowed_symlinks:
            allowed_symlink.assert_current(scanned_regular_paths)
        if root_binding is not None:
            root_binding.assert_current(
                f"production source root {root_prefix}"
            )
        repository.assert_current("hygiene repository")
    finally:
        for allowed_symlink in reversed(allowed_symlinks):
            allowed_symlink.close()
        os.close(root_descriptor)
        if root_binding is not None:
            root_binding.close()


def _step18_attested_source_paths(
    repository: _BoundDirectory,
) -> frozenset[str]:
    environment = {
        "GIT_ATTR_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    }
    argv = [
        "git",
        "--no-pager",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.untrackedCache=false",
        "-C",
        f"/proc/self/fd/{repository.descriptor}",
        "ls-files",
        "--cached",
        "--others",
        "--exclude-standard",
        "-z",
    ]
    try:
        process = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=environment,
            pass_fds=(repository.descriptor,),
            start_new_session=True,
        )
    except OSError as error:
        raise ValidationError(
            f"cannot enumerate candidate source identity paths: {error}"
        ) from error
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    deadline = time.monotonic() + GIT_CHECK_TIMEOUT_SECONDS
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ValidationError(
                    "candidate source identity inventory exceeded the time bound"
                )
            events = selector.select(remaining)
            if not events:
                raise ValidationError(
                    "candidate source identity inventory exceeded the time bound"
                )
            for key, _ in events:
                chunk = os.read(key.fd, 64 * 1024)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                output.extend(chunk)
                if len(output) > MAX_HYGIENE_GIT_INVENTORY_BYTES:
                    raise ValidationError(
                        "candidate source identity inventory exceeds the byte bound"
                    )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ValidationError(
                "candidate source identity inventory exceeded the time bound"
            )
        return_code = process.wait(timeout=remaining)
        if return_code != 0:
            raise ValidationError("Git could not enumerate candidate source paths")
    except (OSError, subprocess.TimeoutExpired, ValidationError):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            try:
                process.kill()
            except OSError:
                pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        raise
    finally:
        selector.close()
        process.stdout.close()

    paths: set[str] = set()
    for encoded_buffer in output.split(b"\0"):
        encoded = bytes(encoded_buffer)
        if not encoded:
            continue
        if (
            len(encoded) > MAX_REFERENCE_PATH_BYTES
            or encoded.startswith(b"/")
            or any(part in (b"", b".", b"..") for part in encoded.split(b"/"))
        ):
            raise ValidationError("Git returned a non-canonical candidate source path")
        paths.add(os.fsdecode(encoded))
        if len(paths) > MAX_HYGIENE_FILES:
            raise ValidationError(
                "candidate source identity inventory exceeds the file-count bound"
            )
    repository.assert_current("hygiene repository")
    return frozenset(paths)


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _sha256_value(value: Any, label: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ValidationError(f"{label} must be a lowercase SHA-256")
    return value


def _exact_keys(value: Any, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must be an object")
    actual = set(value)
    if actual != expected:
        raise ValidationError(
            f"{label} fields differ: missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )
    return value


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError("JSON object repeats a field")
        value[key] = item
    return value


def _reject_json_constant(token: str) -> None:
    raise ValidationError(f"JSON contains non-finite number: {token}")


def _validate_json_bounds(value: Any, label: str) -> None:
    stack = [(value, 0)]
    nodes = 0
    while stack:
        item, depth = stack.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise ValidationError(f"{label} exceeds the JSON node bound")
        if depth > MAX_JSON_DEPTH:
            raise ValidationError(f"{label} exceeds the JSON depth bound")
        if isinstance(item, dict):
            if len(item) > MAX_JSON_COLLECTION_ITEMS:
                raise ValidationError(f"{label} object exceeds the member bound")
            for key, child in item.items():
                try:
                    key_bytes = len(key.encode("utf-8"))
                except UnicodeEncodeError as error:
                    raise ValidationError(f"{label} key is not valid Unicode") from error
                if key_bytes > MAX_JSON_KEY_BYTES:
                    raise ValidationError(f"{label} key exceeds the byte bound")
                stack.append((child, depth + 1))
        elif isinstance(item, list):
            if len(item) > MAX_JSON_COLLECTION_ITEMS:
                raise ValidationError(f"{label} array exceeds the item bound")
            stack.extend((child, depth + 1) for child in item)
        elif isinstance(item, str):
            try:
                size = len(item.encode("utf-8"))
            except UnicodeEncodeError as error:
                raise ValidationError(f"{label} string is not valid Unicode") from error
            if size > MAX_JSON_STRING_BYTES:
                raise ValidationError(f"{label} string exceeds the byte bound")
        elif isinstance(item, float) and not math.isfinite(item):
            raise ValidationError(f"{label} contains a non-finite number")
        elif item is not None and not isinstance(item, (bool, int, float)):
            raise ValidationError(f"{label} contains an unsupported JSON value")


def _parse_json_value(snapshot: _FileSnapshot, label: str) -> Any:
    try:
        value = json.loads(
            snapshot.payload.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValidationError(f"{label} is not readable JSON: {error}") from error
    _validate_json_bounds(value, label)
    return value


def _parse_json_object(snapshot: _FileSnapshot, label: str) -> dict[str, Any]:
    value = _parse_json_value(snapshot, label)
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must contain one JSON object")
    return value


def _load_json_snapshot(
    path: Path,
    label: str,
    max_bytes: int,
) -> tuple[dict[str, Any], _FileSnapshot]:
    snapshot = _read_bounded_regular_file(path, label, max_bytes)
    return _parse_json_object(snapshot, label), snapshot


def _load_json(
    path: Path,
    label: str,
    max_bytes: int = MAX_ARTIFACT_MANIFEST_BYTES,
) -> dict[str, Any]:
    return _load_json_snapshot(path, label, max_bytes)[0]


def _resolve_file(
    owner_path: Path,
    raw_path: Any,
    expected_hash: Any,
    expected_bytes: Any,
    label: str,
    max_bytes: int,
) -> _FileSnapshot:
    if (
        not isinstance(raw_path, str)
        or not raw_path
        or "\0" in raw_path
        or len(os.fsencode(raw_path)) > MAX_REFERENCE_PATH_BYTES
    ):
        raise ValidationError(f"{label} path must be a non-empty string")
    candidate = Path(raw_path)
    path = candidate if candidate.is_absolute() else owner_path.parent / candidate
    if (
        not isinstance(expected_bytes, int)
        or isinstance(expected_bytes, bool)
        or expected_bytes < 0
        or expected_bytes > max_bytes
    ):
        raise ValidationError(f"{label} byte count is invalid or unbounded")
    snapshot = _read_bounded_regular_file(path, label, max_bytes)
    if snapshot.sha256 != _sha256_value(expected_hash, f"{label} hash"):
        raise ValidationError(f"{label} hash changed")
    if snapshot.size != expected_bytes:
        raise ValidationError(f"{label} byte count changed")
    return snapshot


def _identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _contract_identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_specs_contract_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def lane_input_paths(lane: str) -> tuple[str, ...]:
    if lane not in LANES:
        raise ValidationError(f"unknown release-gate lane: {lane}")
    return tuple(sorted(_BASE_INPUT_PATHS | _LANE_INPUT_PATHS[lane]))


def lane_service(lane: str) -> str:
    try:
        return _LANE_SERVICE[lane]
    except KeyError as error:
        raise ValidationError(f"unknown release-gate lane: {lane}") from error


def lane_profile(lane: str) -> str | None:
    if lane in ROS_FULL_LANES:
        return "ros-full"
    if lane in LANES:
        return None
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_security_options(lane: str) -> list[str]:
    # The build-only ``dev`` service carries this option in the repository's
    # canonical Compose contract; sanitizer lanes add the same option to the
    # ROS service through their retained override.
    if lane in SANITIZER_LANES or lane_service(lane) == "dev":
        return ["seccomp=unconfined"]
    if lane in LANES:
        return []
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_security_override(lane: str) -> str | None:
    if lane in SANITIZER_LANES:
        return (
            "services:\n"
            "  ros-jazzy-full:\n"
            "    security_opt:\n"
            "      - seccomp=unconfined\n"
        )
    if lane in LANES:
        return None
    raise ValidationError(f"unknown release-gate lane: {lane}")


def lane_build_root(run_id: str, lane: str) -> str:
    if _UUID.fullmatch(run_id) is None or lane not in LANES:
        raise ValidationError("invalid run id or lane for build root")
    return f"/workspace/build/step18-release-gates/{run_id}/{lane}"


def lane_environment(
    lane: str,
    run_id: str,
    source_fingerprint: str,
    specs_source_fingerprint: str,
    specs_contract_fingerprint: str,
) -> dict[str, str]:
    build_root = lane_build_root(run_id, lane)
    environment = {
        "CC": "gcc",
        "CFLAGS": "",
        "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "CXX": "g++",
        "CXXFLAGS": "",
        "LDFLAGS": "",
        "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256": source_fingerprint,
        "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256": (
            specs_contract_fingerprint
        ),
        "MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256": (
            specs_source_fingerprint
        ),
        "MANYFORGE_RELEASE_GATE_BUILD_ROOT": build_root,
        "MANYFORGE_RELEASE_GATE_LANE": lane,
        "MANYFORGE_RELEASE_GATE_RUN_ID": run_id,
        "NO_COLOR": "1",
        "PYTHONPATH": (
            "/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace"
        ),
        "PYTHONPYCACHEPREFIX": (
            f"/tmp/manyforge-step18-release-gate-pycache/{run_id}/{lane}"
        ),
        "PYTEST_ADDOPTS": "--color=no",
        "WORKSPACE": "/workspace",
    }
    if lane in ROS_FULL_LANES:
        # Compose otherwise preserves these as bare host-passthrough keys.
        # Formal lanes neutralize them explicitly so ambient DDS discovery or
        # profile settings cannot alter the isolated test process.
        environment.update(
            {key: "" for key in ROS_AMBIENT_PASSTHROUGH_VARIABLES}
        )
    if lane == "full_dev_regression":
        environment["MANYFORGE_BUILD_DIR"] = build_root
    elif lane == "full_ros_regression":
        environment.update(
            {
                "MANYFORGE_ROBOPLAN_BUILD_DIR": f"{build_root}/roboplan",
                "MANYFORGE_TEST_ROS_DOMAIN_ID": "119",
                "ROS_DOMAIN_ID": "119",
            }
        )
    elif lane == "contract_architecture_hygiene":
        environment["INSIDE_MANYFORGE_DEV_CONTAINER"] = "1"
    elif lane in SANITIZER_LANES:
        environment["BUILD_DIR"] = build_root
        if lane.endswith("asan_ubsan"):
            environment.update(
                {
                    "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:exitcode=66",
                    "UBSAN_OPTIONS": (
                        "halt_on_error=1:print_stacktrace=1:exitcode=66"
                    ),
                }
            )
        else:
            environment.update(
                {
                    "TSAN_OPTIONS": (
                        "halt_on_error=1 second_deadlock_stack=1 history_size=7 "
                        "exitcode=66 suppressions=/workspace/scripts/ci/tsan.supp"
                    ),
                    "TSAN_SUPPRESSIONS": "/workspace/scripts/ci/tsan.supp",
                }
            )
    return dict(sorted(environment.items()))


def lane_labels(
    lane: str,
    run_id: str,
    source_fingerprint: str,
    specs_source_fingerprint: str,
    specs_contract_fingerprint: str,
    inner_script_sha256: str,
) -> dict[str, str]:
    return dict(
        sorted(
            {
                "manyforge.step18.release-gate.schema": LANE_SCHEMA,
                "manyforge.step18.release-gate.lane": lane,
                "manyforge.step18.release-gate.run-id": run_id,
                "manyforge.step18.release-gate.source": source_fingerprint,
                "manyforge.step18.release-gate.specs-source": (
                    specs_source_fingerprint
                ),
                "manyforge.step18.release-gate.specs-contract": (
                    specs_contract_fingerprint
                ),
                "manyforge.step18.release-gate.inner-script": inner_script_sha256,
            }.items()
        )
    )


def _identity_preamble() -> str:
    return r'''python3 /workspace/scripts/qualification/source_identity.py \
  --repo-root /workspace \
  --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" \
  >/dev/null
python3 /workspace/scripts/qualification/source_identity.py \
  --repo-root /step18-specs \
  --include-specs-contract-identity \
  --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SPECS_SOURCE_FINGERPRINT_SHA256}" \
  --expect-contract-fingerprint-sha256 "${MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256}" \
  >/dev/null'''


def _gcc13_probe() -> str:
    return r'''step18_gxx_version="$(g++ -dumpfullversion -dumpversion)"
printf 'STEP18_GXX_VERSION=%s\n' "${step18_gxx_version}"
[[ "${step18_gxx_version}" == 13.* ]]'''


def _ros_setup() -> str:
    return r'''set +u
source /opt/ros/jazzy/setup.bash
source /opt/manyforge/topic_based_ros2_control/install/setup.bash
source /opt/manyforge/venv/bin/activate
set -u'''


def _lane_body(lane: str) -> str:
    if lane == "full_dev_regression":
        # Historical artifact contract only. The current command exits 78 and
        # must not be replaced with the narrower `retained` aggregate: the
        # unavailable Step 18 producer promised the complete product matrix.
        return "./scripts/test-all.sh all"
    if lane == "full_ros_regression":
        return rf'''{_gcc13_probe()}
{_ros_setup()}
ros_build="${{MANYFORGE_RELEASE_GATE_BUILD_ROOT}}/ros2_control_execution"
cmake -S /workspace/manyforge_ros2_control_execution -B "${{ros_build}}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-Werror \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS=300
cmake --build "${{ros_build}}" --parallel 2
printf 'STEP18_ROS_CTEST_BEGIN\n'
ctest --test-dir "${{ros_build}}" --output-on-failure --timeout 300 --no-tests=error
printf 'STEP18_ROS_CTEST_END status=0\n'
MANYFORGE_ROBOPLAN_BUILD_DIR="${{MANYFORGE_ROBOPLAN_BUILD_DIR}}" \
  /workspace/scripts/ros/build-ros2-control-runtime.sh
export PYTHONPATH="${{MANYFORGE_ROBOPLAN_BUILD_DIR}}:/workspace/manyforge_roboplan/python:\
/workspace/manyforge_ros:/workspace/manyforge_behavior:/workspace:${{PYTHONPATH:-}}"
printf 'STEP18_ROS_PYTEST_BEGIN\n'
pytest /workspace/manyforge_ros/tests -q --color=no
printf 'STEP18_ROS_PYTEST_END status=0\n' '''.rstrip()
    if lane == "contract_architecture_hygiene":
        commands: list[str] = []
        for check_id, command in HYGIENE_CHECKS:
            commands.extend(
                [
                    f"printf 'STEP18_HYGIENE_CHECK_BEGIN id={check_id}\\n'",
                    command,
                    f"printf 'STEP18_HYGIENE_CHECK_END id={check_id} status=0\\n'",
                ]
            )
        return "\n".join(commands)
    if lane in SANITIZER_LANES:
        # Collision sanitizer paths likewise retain the historical no-selector
        # command in Step 18 evidence. Their current owner-neutral subset is a
        # separate `retained-primitives` qualification and cannot satisfy this
        # old cross-package lane.
        return f"{_gcc13_probe()}\nbash /workspace/{_SANITIZER_SCRIPT[lane]}"
    raise ValidationError(f"unknown release-gate lane: {lane}")


def _ros_ambient_environment_preamble() -> str:
    ros_keys = " ".join(ROS_AMBIENT_PASSTHROUGH_VARIABLES)
    return f'''builtin unset -v -- {ros_keys}
for step18_ros_ambient_key in {ros_keys}; do
  if [[ -v "${{step18_ros_ambient_key}}" ]]; then
    builtin printf 'Step 18 ROS ambient environment neutralization failed: %s\\n' \
      "${{step18_ros_ambient_key}}" >&2
    exit 2
  fi
done
builtin unset -v -- step18_ros_ambient_key'''


def lane_inner_script(lane: str) -> str:
    if lane not in LANES:
        raise ValidationError(f"unknown release-gate lane: {lane}")
    preamble = _identity_preamble()
    body = _lane_body(lane)
    ros_environment_preamble = ""
    ros_environment_marker = ""
    if lane in ROS_FULL_LANES:
        ros_environment_preamble = _ros_ambient_environment_preamble()
        ros_environment_marker = (
            "builtin printf 'STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4\\n'"
        )
    return f'''set -euo pipefail
cd /workspace
{ros_environment_preamble}
{preamble}
printf 'STEP18_RELEASE_GATE_BEGIN lane={lane}\\n'
{ros_environment_marker}
{body}
printf 'STEP18_RELEASE_GATE_END lane={lane} status=0\\n'
{preamble}'''


def expected_outer_command(
    *,
    repo_root: Path,
    specs_root: Path,
    lane: str,
    container_name: str,
    environment: dict[str, str],
    labels: dict[str, str],
    security_override_path: Path | None = None,
) -> list[str]:
    command = [
        "docker",
        "compose",
        "-f",
        str(_lexical_absolute(repo_root / "docker-compose.yaml", "Compose file")),
    ]
    override = lane_security_override(lane)
    if override is not None:
        if security_override_path is None:
            raise ValidationError(f"{lane} requires a Compose security override")
        command.extend(
            [
                "-f",
                str(
                    _lexical_absolute(
                        security_override_path,
                        "Compose security override",
                    )
                ),
            ]
        )
    elif security_override_path is not None:
        raise ValidationError(f"{lane} must not use a Compose security override")
    profile = lane_profile(lane)
    if profile is not None:
        command.extend(["--profile", profile])
    command.extend(
        [
            "run",
            "--detach",
            "--no-deps",
            "-T",
            "--name",
            container_name,
            "--entrypoint",
            "/bin/bash",
        ]
    )
    command.extend(
        [
            "-v",
            f"{_lexical_absolute(specs_root, 'Specs root')}:/step18-specs:ro",
        ]
    )
    for key, value in sorted(labels.items()):
        command.extend(["--label", f"{key}={value}"])
    for key, value in sorted(environment.items()):
        command.extend(["-e", f"{key}={value}"])
    command.extend([lane_service(lane), "-lc", lane_inner_script(lane)])
    return command


def normalize_mounts(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ValidationError("container inspect mounts must be a list")
    mounts: list[dict[str, Any]] = []
    for index, raw in enumerate(value):
        if not isinstance(raw, dict):
            raise ValidationError(f"container mount {index} is not an object")
        destination = raw.get("Destination")
        source = raw.get("Source")
        mount_type = raw.get("Type")
        if not all(isinstance(item, str) and item for item in (destination, source, mount_type)):
            raise ValidationError(f"container mount {index} lacks type/source/destination")
        rw = raw.get("RW")
        if not isinstance(rw, bool):
            raise ValidationError(f"container mount {index} lacks boolean RW")
        mounts.append(
            {
                "type": mount_type,
                "source": source,
                "destination": destination,
                "mode": str(raw.get("Mode", "")),
                "rw": rw,
                "propagation": str(raw.get("Propagation", "")),
                "name": str(raw.get("Name", "")),
                "driver": str(raw.get("Driver", "")),
            }
        )
    destinations = [mount["destination"] for mount in mounts]
    if len(set(destinations)) != len(destinations):
        raise ValidationError("container inspect has duplicate mount destinations")
    return sorted(mounts, key=lambda item: item["destination"])


def _clean_log(raw: str) -> str:
    return _ANSI.sub("", raw).replace("\r", "")


def _one_marked_section(log: str, begin: str, end: str, label: str) -> str:
    begin_matches = list(
        re.finditer(rf"^{re.escape(begin)}$", log, flags=re.MULTILINE)
    )
    end_matches = list(re.finditer(rf"^{re.escape(end)}$", log, flags=re.MULTILINE))
    if len(begin_matches) != 1 or len(end_matches) != 1:
        raise ValidationError(f"{label} has missing or ambiguous boundary markers")
    start = begin_matches[0].end()
    finish = end_matches[0].start()
    if finish <= start:
        raise ValidationError(f"{label} section is empty")
    return log[start:finish]


def _verify_lane_log(lane: str, log_snapshot: _FileSnapshot) -> dict[str, Any]:
    if log_snapshot.size <= 0:
        raise ValidationError(f"{lane} container log is empty")
    text = _clean_log(log_snapshot.payload.decode("utf-8", errors="replace"))
    begin = f"STEP18_RELEASE_GATE_BEGIN lane={lane}"
    end = f"STEP18_RELEASE_GATE_END lane={lane} status=0"
    section = _one_marked_section(text, begin, end, f"{lane} release gate")

    gcc_version: str | None = None
    neutralization_pattern = r"^STEP18_ROS_AMBIENT_ENV_NEUTRALIZED keys=4$"
    log_neutralization_matches = re.findall(
        neutralization_pattern,
        text,
        flags=re.MULTILINE,
    )
    section_neutralization_matches = re.findall(
        neutralization_pattern,
        section,
        flags=re.MULTILINE,
    )
    if lane in ROS_FULL_LANES:
        if (
            len(log_neutralization_matches) != 1
            or len(section_neutralization_matches) != 1
        ):
            raise ValidationError(
                f"{lane} does not prove exact ROS ambient environment neutralization"
            )
    elif log_neutralization_matches or section_neutralization_matches:
        raise ValidationError(f"{lane} unexpectedly claims ROS environment neutralization")
    gcc_matches = re.findall(r"^STEP18_GXX_VERSION=([^\s]+)$", section, re.MULTILINE)
    if lane in ROS_FULL_LANES:
        if len(gcc_matches) != 1 or not gcc_matches[0].startswith("13."):
            raise ValidationError(f"{lane} does not prove the GCC 13 ros-full lane")
        gcc_version = gcc_matches[0]
    elif gcc_matches:
        raise ValidationError(f"{lane} unexpectedly claims a GCC probe")

    summary: dict[str, Any]
    if lane == "full_dev_regression":
        if re.search(r"^\s*SKIP:", section, flags=re.MULTILINE):
            raise ValidationError("full_dev_regression skipped a capability lane")
        if "already passed" in section or "skipping re-test" in section:
            raise ValidationError(
                "full_dev_regression used a forbidden re-test shortcut"
            )
        matches = re.findall(
            r"^All ([0-9]+) test suite\(s\) passed\.$",
            section,
            flags=re.MULTILINE,
        )
        if matches != ["11"]:
            raise ValidationError(
                "full_dev_regression lacks one exact 11-suite passing summary"
            )
        summary = {"suite_count": 11, "capability_skips": 0}
    elif lane == "full_ros_regression":
        ctest = _one_marked_section(
            section,
            "STEP18_ROS_CTEST_BEGIN",
            "STEP18_ROS_CTEST_END status=0",
            "full_ros_regression CTest",
        )
        ctest_matches = re.findall(
            r"^100% tests passed, 0 tests failed out of ([1-9][0-9]*)$",
            ctest,
            flags=re.MULTILINE,
        )
        if len(ctest_matches) != 1 or re.search(
            r"No tests were found|\bNot Run\b", ctest
        ):
            raise ValidationError(
                "full_ros_regression CTest summary is missing or ambiguous"
            )
        pytest = _one_marked_section(
            section,
            "STEP18_ROS_PYTEST_BEGIN",
            "STEP18_ROS_PYTEST_END status=0",
            "full_ros_regression pytest",
        )
        pytest_matches = re.findall(
            r"^(?:=+ )?([1-9][0-9]*) passed in [^\n]+?(?: =+)?$",
            pytest,
            flags=re.MULTILINE,
        )
        if len(pytest_matches) != 1 or re.search(
            r"[0-9]+ (?:failed|skipped|errors?|xfailed|xpassed)\b",
            pytest,
            flags=re.IGNORECASE,
        ):
            raise ValidationError(
                "full_ros_regression pytest did not prove one non-skipped passing suite"
            )
        summary = {
            "ctest_tests": int(ctest_matches[0]),
            "pytest_tests": int(pytest_matches[0]),
            "pytest_skipped": 0,
            "gcc_version": gcc_version,
        }
    elif lane == "contract_architecture_hygiene":
        completed: list[str] = []
        for check_id, _ in HYGIENE_CHECKS:
            _one_marked_section(
                section,
                f"STEP18_HYGIENE_CHECK_BEGIN id={check_id}",
                f"STEP18_HYGIENE_CHECK_END id={check_id} status=0",
                f"hygiene check {check_id}",
            )
            completed.append(check_id)
        if section.count("STEP18_HYGIENE_CHECK_BEGIN") != len(HYGIENE_CHECKS):
            raise ValidationError("hygiene lane contains duplicate or unknown checks")
        if section.count("STEP18_HYGIENE_CHECK_END") != len(HYGIENE_CHECKS):
            raise ValidationError("hygiene lane contains ambiguous completion markers")
        if section.count("STEP18_SPECIFIC_HYGIENE passed") != 1:
            raise ValidationError("Step 18-specific hygiene result is ambiguous")
        summary = {"checks": completed, "check_count": len(completed)}
    elif lane in SANITIZER_LANES:
        if _SANITIZER_FINDING.search(section):
            raise ValidationError(f"{lane} log contains a sanitizer signature")
        prefix = _SANITIZER_PREFIX[lane]
        pattern = re.compile(
            rf"^{prefix}_GROUP_SUMMARY binaries=([0-9]+) "
            r"registered_tests=([0-9]+) enabled_tests=([0-9]+) "
            r"disabled_tests=([0-9]+) ran=([0-9]+) passed=([0-9]+) "
            r"skipped=([0-9]+) fallback_binaries=([0-9]+)$",
            flags=re.MULTILINE,
        )
        matches = pattern.findall(section)
        if len(matches) != 1:
            raise ValidationError(f"{lane} grouped test summary is missing or ambiguous")
        (
            binaries,
            registered,
            enabled,
            disabled,
            ran,
            passed,
            skipped,
            fallback,
        ) = (int(item) for item in matches[0])
        if (
            binaries <= 0
            or registered <= 0
            or enabled <= 0
            or ran != enabled
            or passed != enabled
            or skipped != 0
            or fallback != 0
        ):
            raise ValidationError(
                f"{lane} grouped lane used fallback/skip or did not run every enabled test"
            )
        summary = {
            "binaries": binaries,
            "registered_tests": registered,
            "enabled_tests": enabled,
            "disabled_tests": disabled,
            "ran": ran,
            "passed": passed,
            "skipped": skipped,
            "fallback_binaries": fallback,
            "gcc_version": gcc_version,
        }
    else:  # pragma: no cover - guarded by caller
        raise ValidationError(f"unknown release-gate lane: {lane}")
    return summary


def _verify_container(
    *,
    inspect_snapshot: _FileSnapshot,
    repo_root: Path,
    specs_root: Path,
    lane: str,
    execution: dict[str, Any],
    security_override_path: Path | None,
) -> dict[str, Any]:
    value = _parse_json_value(inspect_snapshot, f"{lane} container inspect")
    if not isinstance(value, list) or len(value) != 1 or not isinstance(value[0], dict):
        raise ValidationError(f"{lane} inspect must contain exactly one container")
    record = value[0]
    state = _exact_keys(
        execution["terminal_status"],
        {"docker_wait_status", "exit_code", "oom_killed", "error"},
        f"{lane} terminal status",
    )
    inspect_state = record.get("State")
    if not isinstance(inspect_state, dict):
        raise ValidationError(f"{lane} inspect omits container state")
    observed_status = {
        "docker_wait_status": execution["terminal_status"]["docker_wait_status"],
        "exit_code": inspect_state.get("ExitCode"),
        "oom_killed": inspect_state.get("OOMKilled"),
        "error": inspect_state.get("Error"),
    }
    if observed_status != state:
        raise ValidationError(f"{lane} terminal status differs from inspect")
    if (
        state["docker_wait_status"] != 0
        or state["exit_code"] != 0
        or state["oom_killed"] is not False
        or state["error"] not in ("", None)
    ):
        raise ValidationError(f"{lane} container did not exit successfully")

    container_id = execution.get("container_id")
    container_name = execution.get("container_name")
    declared_image_id = str(execution.get("declared_image_id", "")).removeprefix(
        "sha256:"
    )
    image_id = str(execution.get("image_id", "")).removeprefix("sha256:")
    if (
        not isinstance(container_id, str)
        or _SHA256.fullmatch(container_id) is None
        or not isinstance(container_name, str)
        or not container_name
        or _SHA256.fullmatch(declared_image_id) is None
        or _SHA256.fullmatch(image_id) is None
    ):
        raise ValidationError(f"{lane} container identity fields are invalid")
    if (
        record.get("Id") != container_id
        or str(record.get("Name", "")).removeprefix("/") != container_name
        or str(record.get("Image", "")).removeprefix("sha256:") != image_id
        or declared_image_id != image_id
    ):
        raise ValidationError(f"{lane} container id/name/image differs from inspect")

    config = record.get("Config")
    host_config = record.get("HostConfig")
    if not isinstance(config, dict) or not isinstance(host_config, dict):
        raise ValidationError(f"{lane} inspect omits Config or HostConfig")
    if config.get("Image") != execution.get("image_reference"):
        raise ValidationError(f"{lane} configured image reference differs")
    configured_process = [record.get("Path"), *record.get("Args", [])]
    expected_process = ["/bin/bash", "-lc", lane_inner_script(lane)]
    if (
        configured_process != execution.get("configured_process_argv")
        or configured_process != expected_process
    ):
        raise ValidationError(f"{lane} configured process argv differs")
    requested_argv = execution.get("requested_container_argv")
    expected_requested = expected_process
    if requested_argv != expected_requested:
        raise ValidationError(f"{lane} requested container argv is not canonical")
    if config.get("Entrypoint") != ["/bin/bash"]:
        raise ValidationError(f"{lane} configured entrypoint is not canonical")
    if config.get("Cmd") != ["-lc", lane_inner_script(lane)]:
        raise ValidationError(f"{lane} configured command is not canonical")

    configured_environment = config.get("Env")
    if (
        not isinstance(configured_environment, list)
        or not all(isinstance(item, str) and "=" in item for item in configured_environment)
        or configured_environment != execution.get("configured_environment")
    ):
        raise ValidationError(f"{lane} configured environment is not exact")
    required_environment = execution.get("required_environment")
    if not isinstance(required_environment, dict):
        raise ValidationError(f"{lane} required environment is not an object")
    observed_environment: dict[str, list[str]] = {}
    for item in configured_environment:
        key, value = item.split("=", 1)
        observed_environment.setdefault(key, []).append(value)
    for key, expected in required_environment.items():
        if observed_environment.get(key) != [expected]:
            raise ValidationError(f"{lane} environment {key} differs or is duplicated")

    labels = config.get("Labels")
    required_labels = execution.get("required_labels")
    if not isinstance(labels, dict) or not isinstance(required_labels, dict):
        raise ValidationError(f"{lane} container labels are unavailable")
    for key, expected in required_labels.items():
        if labels.get(key) != expected:
            raise ValidationError(f"{lane} container label {key} differs")

    security = host_config.get("SecurityOpt") or []
    if security != execution.get("security_options"):
        raise ValidationError(f"{lane} security options differ from inspect")
    if security != lane_security_options(lane):
        raise ValidationError(f"{lane} security profile is not canonical")

    mounts = normalize_mounts(record.get("Mounts"))
    if mounts != execution.get("mounts"):
        raise ValidationError(f"{lane} exact mount inventory differs from inspect")
    workspace = [item for item in mounts if item["destination"] == "/workspace"]
    specs = [item for item in mounts if item["destination"] == "/step18-specs"]
    try:
        workspace_source = (
            _lexical_absolute(
                Path(workspace[0]["source"]),
                f"{lane} workspace mount",
            )
            if len(workspace) == 1
            else None
        )
        specs_source = (
            _lexical_absolute(
                Path(specs[0]["source"]),
                f"{lane} Specs mount",
            )
            if len(specs) == 1
            else None
        )
    except (TypeError, ValueError) as error:
        raise ValidationError(f"{lane} source mount path is invalid") from error
    if (
        len(workspace) != 1
        or workspace[0]["type"] != "bind"
        or workspace_source != repo_root
        or workspace[0]["rw"] is not True
        or len(specs) != 1
        or specs[0]["type"] != "bind"
        or specs_source != specs_root
        or specs[0]["rw"] is not False
    ):
        raise ValidationError(f"{lane} source or Specs mount is not exact")

    expected_outer = expected_outer_command(
        repo_root=repo_root,
        specs_root=specs_root,
        lane=lane,
        container_name=container_name,
        environment=required_environment,
        labels=required_labels,
        security_override_path=security_override_path,
    )
    if execution.get("outer_command_argv") != expected_outer:
        raise ValidationError(f"{lane} outer Docker command is not canonical")
    return {
        "id": container_id,
        "name": container_name,
        "service": lane_service(lane),
        "image_reference": execution["image_reference"],
        "image_id": image_id,
        "security_options": security,
        "mounts": mounts,
    }


def _verify_lane_manifest(
    *,
    manifest_snapshot: _FileSnapshot,
    candidate_source: dict[str, Any],
    candidate_specs_source: dict[str, Any],
    candidate_specs_contract: dict[str, Any],
    repo_root: Path,
    specs_root: Path,
) -> dict[str, Any]:
    manifest = _parse_json_object(manifest_snapshot, "release-gate lane manifest")
    manifest_path = manifest_snapshot.path
    _exact_keys(
        manifest,
        {
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
        },
        "release-gate lane manifest",
    )
    if manifest["schema_version"] != LANE_SCHEMA:
        raise ValidationError(f"lane manifest schema must be {LANE_SCHEMA}")
    if manifest["verification_scope"] != VERIFICATION_SCOPE:
        raise ValidationError("lane manifest verification scope differs")
    lane = manifest.get("lane")
    if lane not in LANES:
        raise ValidationError(f"lane manifest has unknown lane: {lane}")
    run_id = manifest.get("run_id")
    if not isinstance(run_id, str) or _UUID.fullmatch(run_id) is None:
        raise ValidationError(f"{lane} run id is not a UUIDv4")
    if (
        manifest["overall_step18_closure_claimed"] is not False
        or manifest["requires_common_candidate_aggregator"] is not True
    ):
        raise ValidationError(f"{lane} improperly claims overall Step 18 closure")

    paths = _exact_keys(
        manifest["paths"],
        {"manyforge_root", "specs_root"},
        f"{lane} paths",
    )
    if not all(isinstance(paths[name], str) for name in ("manyforge_root", "specs_root")):
        raise ValidationError(f"{lane} source roots must be strings")
    if (
        _lexical_absolute(Path(paths["manyforge_root"]), f"{lane} ManyForge root")
        != repo_root
        or _lexical_absolute(Path(paths["specs_root"]), f"{lane} Specs root")
        != specs_root
    ):
        raise ValidationError(f"{lane} source roots differ from the campaign")

    identities = _exact_keys(
        manifest["identities"],
        {
            "manyforge_before",
            "manyforge_after",
            "specs_before",
            "specs_after",
            "specs_contract_before",
            "specs_contract_after",
        },
        f"{lane} identities",
    )
    manyforge_before = _identity(identities["manyforge_before"], f"{lane} ManyForge before")
    manyforge_after = _identity(identities["manyforge_after"], f"{lane} ManyForge after")
    specs_before = _identity(identities["specs_before"], f"{lane} Specs before")
    specs_after = _identity(identities["specs_after"], f"{lane} Specs after")
    contract_before = _contract_identity(
        identities["specs_contract_before"], f"{lane} Specs contract before"
    )
    contract_after = _contract_identity(
        identities["specs_contract_after"], f"{lane} Specs contract after"
    )
    if manyforge_before != manyforge_after:
        raise ValidationError(f"{lane} ManyForge source changed during execution")
    if specs_before != specs_after:
        raise ValidationError(f"{lane} Specs source changed during execution")
    if contract_before != contract_after:
        raise ValidationError(f"{lane} Specs contract changed during execution")
    if manyforge_after != candidate_source:
        raise ValidationError(f"{lane} uses a mixed ManyForge source")
    if specs_after != candidate_specs_source:
        raise ValidationError(f"{lane} uses a mixed Specs source")
    if contract_after != candidate_specs_contract:
        raise ValidationError(f"{lane} uses a mixed Specs contract")

    artifacts = _exact_keys(
        manifest["artifacts"],
        {"container_inspect", "container_log"},
        f"{lane} artifacts",
    )
    resolved_artifacts: dict[str, _FileSnapshot] = {}
    for name in ("container_inspect", "container_log"):
        record = _exact_keys(
            artifacts[name], {"path", "sha256", "bytes"}, f"{lane} {name}"
        )
        resolved_artifacts[name] = _resolve_file(
            manifest_path,
            record["path"],
            record["sha256"],
            record["bytes"],
            f"{lane} {name}",
            (
                MAX_CONTAINER_INSPECT_BYTES
                if name == "container_inspect"
                else MAX_CONTAINER_LOG_BYTES
            ),
        )
    inspect_snapshot = resolved_artifacts["container_inspect"]
    log_snapshot = resolved_artifacts["container_log"]
    if inspect_snapshot.identity == log_snapshot.identity:
        raise ValidationError(f"{lane} inspect and log artifacts alias")

    script_hash_bundle = _exact_keys(
        manifest.get("script_hashes"),
        {"before", "after"},
        f"{lane} script hash bundle",
    )
    expected_inputs = lane_input_paths(lane)
    for phase in ("before", "after"):
        if (
            not isinstance(script_hash_bundle[phase], dict)
            or set(script_hash_bundle[phase]) != set(expected_inputs)
        ):
            raise ValidationError(
                f"{lane} {phase}-run script hash inventory is not exact"
            )
    if script_hash_bundle["before"] != script_hash_bundle["after"]:
        raise ValidationError(f"{lane} qualification inputs changed during execution")
    for relative in expected_inputs:
        path = _lexical_absolute(repo_root / relative, f"{lane} qualification input")
        try:
            path.relative_to(repo_root)
        except ValueError as error:
            raise ValidationError(f"{lane} script path escapes source root") from error
        input_snapshot = _read_bounded_regular_file(
            path,
            f"{lane} qualification input {relative}",
            MAX_QUALIFICATION_INPUT_BYTES,
        )
        if input_snapshot.sha256 != _sha256_value(
            script_hash_bundle["after"][relative], relative
        ):
            raise ValidationError(f"{lane} qualification input hash changed: {relative}")

    execution = _exact_keys(
        manifest["execution"],
        {
            "compose_service",
            "compose_profile",
            "container_id",
            "container_name",
            "image_reference",
            "declared_image_id",
            "image_id",
            "outer_command_argv",
            "requested_container_argv",
            "configured_process_argv",
            "required_environment",
            "configured_environment",
            "required_labels",
            "security_override",
            "security_options",
            "mounts",
            "terminal_status",
        },
        f"{lane} execution",
    )
    if execution["compose_service"] != lane_service(lane):
        raise ValidationError(f"{lane} compose service differs")
    if execution["compose_profile"] != lane_profile(lane):
        raise ValidationError(f"{lane} compose profile differs")
    expected_environment = lane_environment(
        lane,
        run_id,
        candidate_source["fingerprint_sha256"],
        candidate_specs_source["fingerprint_sha256"],
        candidate_specs_contract["contract_fingerprint_sha256"],
    )
    if execution["required_environment"] != expected_environment:
        raise ValidationError(f"{lane} required environment differs from the frozen profile")
    expected_inner = lane_inner_script(lane)
    expected_labels = lane_labels(
        lane,
        run_id,
        candidate_source["fingerprint_sha256"],
        candidate_specs_source["fingerprint_sha256"],
        candidate_specs_contract["contract_fingerprint_sha256"],
        _sha256_text(expected_inner),
    )
    if execution["required_labels"] != expected_labels:
        raise ValidationError(f"{lane} source-binding labels differ")

    security_override_path: Path | None = None
    security_override_binding: dict[str, Any] | None = None
    expected_override = lane_security_override(lane)
    override_record = execution["security_override"]
    if expected_override is None:
        if override_record is not None:
            raise ValidationError(f"{lane} unexpectedly retains a security override")
    else:
        override = _exact_keys(
            override_record,
            {"path", "sha256", "bytes"},
            f"{lane} security override",
        )
        override_snapshot = _resolve_file(
            manifest_path,
            override["path"],
            override["sha256"],
            override["bytes"],
            f"{lane} security override",
            MAX_SECURITY_OVERRIDE_BYTES,
        )
        try:
            observed_override = override_snapshot.payload.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValidationError(f"cannot read {lane} security override: {error}") from error
        if observed_override != expected_override:
            raise ValidationError(f"{lane} security override content differs")
        security_override_path = override_snapshot.path
        security_override_binding = {
            "path": str(security_override_path),
            "sha256": override_snapshot.sha256,
            "bytes": override_snapshot.size,
        }
    container = _verify_container(
        inspect_snapshot=inspect_snapshot,
        repo_root=repo_root,
        specs_root=specs_root,
        lane=lane,
        execution=execution,
        security_override_path=security_override_path,
    )
    test_summary = _verify_lane_log(lane, log_snapshot)
    return {
        "name": lane,
        "run_id": run_id,
        "lane_manifest": {
            "path": str(manifest_path),
            "sha256": manifest_snapshot.sha256,
        },
        "container": container,
        "container_inspect": {
            "path": str(inspect_snapshot.path),
            "sha256": inspect_snapshot.sha256,
        },
        "container_log": {
            "path": str(log_snapshot.path),
            "sha256": log_snapshot.sha256,
            "bytes": log_snapshot.size,
        },
        "security_override": security_override_binding,
        "test_summary": test_summary,
        "passed": True,
    }


def verify_artifact_manifest(manifest_path: Path) -> dict[str, Any]:
    manifest, manifest_snapshot = _load_json_snapshot(
        manifest_path,
        "release-gate artifact manifest",
        MAX_ARTIFACT_MANIFEST_BYTES,
    )
    manifest_path = manifest_snapshot.path
    _exact_keys(
        manifest,
        {
            "schema_version",
            "verification_scope",
            "candidate",
            "lanes",
            "overall_step18_closure_claimed",
            "requires_common_candidate_aggregator",
        },
        "release-gate artifact manifest",
    )
    if manifest["schema_version"] != ARTIFACT_MANIFEST_SCHEMA:
        raise ValidationError(
            f"artifact manifest schema must be {ARTIFACT_MANIFEST_SCHEMA}"
        )
    if manifest["verification_scope"] != VERIFICATION_SCOPE:
        raise ValidationError("artifact manifest verification scope differs")
    if (
        manifest["overall_step18_closure_claimed"] is not False
        or manifest["requires_common_candidate_aggregator"] is not True
    ):
        raise ValidationError("release-gate artifact manifest claims overall closure")

    candidate = _exact_keys(
        manifest["candidate"],
        {
            "manyforge_root",
            "specs_root",
            "source_identity",
            "specs_source_identity",
            "specs_contract_identity",
        },
        "release-gate candidate",
    )
    if not isinstance(candidate["manyforge_root"], str) or not isinstance(
        candidate["specs_root"], str
    ):
        raise ValidationError("release-gate candidate roots must be strings")
    repo_root = _lexical_absolute(
        Path(candidate["manyforge_root"]),
        "release-gate ManyForge root",
    )
    specs_root = _lexical_absolute(
        Path(candidate["specs_root"]),
        "release-gate Specs root",
    )
    candidate_source = _identity(candidate["source_identity"], "candidate source")
    candidate_specs_source = _identity(
        candidate["specs_source_identity"], "candidate Specs source"
    )
    candidate_specs_contract = _contract_identity(
        candidate["specs_contract_identity"], "candidate Specs contract"
    )
    if (
        candidate_specs_source["head_commit"]
        != candidate_specs_contract["head_commit"]
        or candidate_specs_source["head_tree"]
        != candidate_specs_contract["head_tree"]
    ):
        raise ValidationError(
            "candidate Specs source and contract do not share one HEAD/tree"
        )
    try:
        current_source = source_identity(repo_root)
        current_specs_source = source_identity(specs_root)
        current_specs_contract = specs_contract_identity(specs_root)
    except SourceIdentityError as error:
        raise ValidationError(f"cannot collect current candidate identity: {error}") from error
    if current_source != candidate_source:
        raise ValidationError("current ManyForge source differs from the release-gate candidate")
    if (
        current_specs_source["head_commit"] != candidate_specs_source["head_commit"]
        or current_specs_source["head_tree"] != candidate_specs_source["head_tree"]
    ):
        raise ValidationError(
            "current Specs HEAD/tree differs from the release-gate candidate"
        )
    if current_specs_contract != candidate_specs_contract:
        raise ValidationError("current Specs contract differs from the release-gate candidate")

    references = manifest.get("lanes")
    if not isinstance(references, list) or len(references) != len(LANES):
        raise ValidationError("artifact manifest must contain exactly seven lane references")
    lane_names: list[str] = []
    manifest_paths: list[Path] = []
    manifest_identities: list[tuple[int, int]] = []
    verified: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(references):
        reference = _exact_keys(
            raw,
            {"lane", "manifest_path", "manifest_sha256", "manifest_bytes"},
            f"lane reference {index}",
        )
        lane = reference["lane"]
        if lane not in LANES:
            raise ValidationError(f"lane reference {index} has unknown lane: {lane}")
        lane_names.append(lane)
        lane_snapshot = _resolve_file(
            manifest_path,
            reference["manifest_path"],
            reference["manifest_sha256"],
            reference["manifest_bytes"],
            f"{lane} lane manifest",
            MAX_LANE_MANIFEST_BYTES,
        )
        manifest_paths.append(lane_snapshot.path)
        manifest_identities.append(lane_snapshot.identity)
        result = _verify_lane_manifest(
            manifest_snapshot=lane_snapshot,
            candidate_source=candidate_source,
            candidate_specs_source=candidate_specs_source,
            candidate_specs_contract=candidate_specs_contract,
            repo_root=repo_root,
            specs_root=specs_root,
        )
        if result["name"] != lane:
            raise ValidationError(f"lane reference {lane} points to {result['name']}")
        verified[lane] = result
    if len(set(lane_names)) != len(LANES) or set(lane_names) != set(LANES):
        raise ValidationError("release-gate lanes are missing or duplicated")
    if len(set(manifest_paths)) != len(LANES):
        raise ValidationError("release-gate lane references alias the same manifest")
    if len(set(manifest_identities)) != len(LANES):
        raise ValidationError("release-gate lane manifests alias one inode")

    run_ids = [verified[lane]["run_id"] for lane in LANES]
    container_ids = [verified[lane]["container"]["id"] for lane in LANES]
    container_names = [verified[lane]["container"]["name"] for lane in LANES]
    log_paths = [Path(verified[lane]["container_log"]["path"]) for lane in LANES]
    if len(set(run_ids)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a run id")
    if len(set(container_ids)) != len(LANES) or len(set(container_names)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a container identity")
    if len(set(log_paths)) != len(LANES):
        raise ValidationError("release-gate lanes reuse a log artifact")

    dev_images = {
        verified[lane]["container"]["image_id"]
        for lane in ("full_dev_regression", "contract_architecture_hygiene")
    }
    ros_images = {
        verified[lane]["container"]["image_id"] for lane in ROS_FULL_LANES
    }
    if len(dev_images) != 1:
        raise ValidationError("dev regression and hygiene used different images")
    if len(ros_images) != 1:
        raise ValidationError("ROS and sanitizer lanes used different ros-full images")

    verifier_path = _lexical_absolute(
        repo_root / "scripts/qualification/verify_step18_release_gates.py",
        "release verifier",
    )
    runner_path = _lexical_absolute(
        repo_root / "scripts/qualification/run_step18_release_gates.py",
        "release runner",
    )
    verifier_snapshot = _read_bounded_regular_file(
        verifier_path,
        "release verifier",
        MAX_QUALIFICATION_INPUT_BYTES,
    )
    runner_snapshot = _read_bounded_regular_file(
        runner_path,
        "release runner",
        MAX_QUALIFICATION_INPUT_BYTES,
    )
    retained_artifact_manifest: list[dict[str, Any]] = [
        {
            "role": "release_artifact_manifest",
            "path": str(manifest_path),
            "sha256": manifest_snapshot.sha256,
        },
        {
            "role": "release_verifier",
            "path": str(verifier_path),
            "sha256": verifier_snapshot.sha256,
        },
        {
            "role": "release_runner",
            "path": str(runner_path),
            "sha256": runner_snapshot.sha256,
        },
    ]
    for lane in LANES:
        result = verified[lane]
        for field, role in (
            ("lane_manifest", "release_lane_manifest"),
            ("container_inspect", "release_container_inspect"),
            ("container_log", "release_container_log"),
        ):
            retained_artifact_manifest.append(
                {"role": role, "lane": lane, **result[field]}
            )
        if result["security_override"] is not None:
            retained_artifact_manifest.append(
                {
                    "role": "release_security_override",
                    "lane": lane,
                    **result["security_override"],
                }
            )
    _assert_retained_artifact_manifest_current(retained_artifact_manifest)

    return {
        "schema_version": SUMMARY_SCHEMA,
        "verification_scope": VERIFICATION_SCOPE,
        "candidate_source_identity": candidate_source,
        "candidate_specs_source_identity": candidate_specs_source,
        "candidate_specs_contract_identity": candidate_specs_contract,
        "lanes": [verified[lane] for lane in LANES],
        "artifact_manifest": {
            "path": str(manifest_path),
            "sha256": manifest_snapshot.sha256,
        },
        "retained_artifact_manifest": retained_artifact_manifest,
        "runner": {
            "path": str(runner_path),
            "sha256": runner_snapshot.sha256,
        },
        "verifier": {
            "path": str(verifier_path),
            "sha256": verifier_snapshot.sha256,
        },
        "failure_count": 0,
        "release_gates_passed": True,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
    }


class _RetainedArtifactHandle:
    def __init__(
        self,
        *,
        path: Path,
        label: str,
        role: str,
        expected_sha256: str,
        expected_bytes: int | None,
        max_bytes: int,
    ) -> None:
        self.path = _lexical_absolute(path, label)
        self.label = label
        self.role = role
        self.expected_sha256 = expected_sha256
        self.expected_bytes = expected_bytes
        self.parent = _open_absolute_directory(
            self.path.parent,
            f"{label} parent",
        )
        self.descriptor = -1
        try:
            self.descriptor = os.open(
                self.path.name,
                os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=self.parent.descriptor,
            )
            before = os.fstat(self.descriptor)
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                raise ValidationError(
                    f"{label} must be one non-hardlinked regular file"
                )
            if before.st_size <= 0 or before.st_size > max_bytes:
                raise ValidationError(f"{label} is empty or exceeds its byte bound")
            self.signature = _metadata_signature(before)
            self.identity = (before.st_dev, before.st_ino)
            self.size = before.st_size
            self._assert_payload_current()
        except BaseException:
            self.close()
            raise

    def _read_payload(self) -> bytes:
        os.lseek(self.descriptor, 0, os.SEEK_SET)
        payload = bytearray()
        remaining = self.size
        while remaining:
            chunk = os.read(self.descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise ValidationError(
                    f"{self.label} ended before its recorded size"
                )
            payload.extend(chunk)
            remaining -= len(chunk)
        if os.read(self.descriptor, 1):
            raise ValidationError(f"{self.label} grew while being read")
        return bytes(payload)

    def _assert_payload_current(self) -> None:
        payload = self._read_payload()
        if hashlib.sha256(payload).hexdigest() != self.expected_sha256:
            raise ValidationError(f"release retained artifact changed: {self.path}")
        if self.expected_bytes is not None and len(payload) != self.expected_bytes:
            raise ValidationError(
                f"release retained artifact byte count changed: {self.path}"
            )
        self.assert_metadata_current()

    def assert_metadata_current(self) -> None:
        after = os.fstat(self.descriptor)
        self.parent.assert_current(f"{self.label} parent")
        if (
            _metadata_signature(after) != self.signature
            or not _path_matches_file(
                self.parent.descriptor,
                self.path.name,
                self.identity,
            )
        ):
            raise ValidationError(f"release retained artifact changed: {self.path}")

    def assert_payload_current(self) -> None:
        self._assert_payload_current()

    def close(self) -> None:
        if self.descriptor >= 0:
            try:
                os.close(self.descriptor)
            except OSError:
                pass
            self.descriptor = -1
        self.parent.close()


def _retained_artifact_inventory(
    value: Any,
) -> list[tuple[int, dict[str, Any], str, int, str, int | None]]:
    if not isinstance(value, list) or len(value) != 28:
        raise ValidationError(
            "release retained artifact manifest must contain exactly 28 files"
        )
    inventory: list[tuple[int, dict[str, Any], str, int, str, int | None]] = []
    roles: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict) or set(item) not in (
            {"role", "path", "sha256"},
            {"role", "lane", "path", "sha256"},
            {"role", "lane", "path", "sha256", "bytes"},
        ):
            raise ValidationError(
                f"release retained artifact {index} has unexpected fields"
            )
        role = item.get("role")
        if not isinstance(role, str) or not role:
            raise ValidationError(f"release retained artifact {index} has no role")
        raw_path = item.get("path")
        if not isinstance(raw_path, str) or not raw_path:
            raise ValidationError(f"release retained artifact {index} has no path")
        role_bound = {
            "release_artifact_manifest": MAX_ARTIFACT_MANIFEST_BYTES,
            "release_verifier": MAX_QUALIFICATION_INPUT_BYTES,
            "release_runner": MAX_QUALIFICATION_INPUT_BYTES,
            "release_lane_manifest": MAX_LANE_MANIFEST_BYTES,
            "release_container_inspect": MAX_CONTAINER_INSPECT_BYTES,
            "release_container_log": MAX_CONTAINER_LOG_BYTES,
            "release_security_override": MAX_SECURITY_OVERRIDE_BYTES,
        }.get(role)
        if role_bound is None:
            raise ValidationError(f"release retained artifact {index} has unknown role")
        expected_hash = _sha256_value(
            item.get("sha256"),
            f"release retained artifact {index}",
        )
        expected_bytes = item.get("bytes") if "bytes" in item else None
        if expected_bytes is not None and (
            not isinstance(expected_bytes, int)
            or isinstance(expected_bytes, bool)
            or expected_bytes <= 0
            or expected_bytes > role_bound
        ):
            raise ValidationError(
                f"release retained artifact {index} byte count is invalid"
            )
        inventory.append(
            (
                index,
                item,
                role,
                role_bound,
                expected_hash,
                expected_bytes,
            )
        )
        roles.append(role)
    expected_counts = {
        "release_artifact_manifest": 1,
        "release_verifier": 1,
        "release_runner": 1,
        "release_lane_manifest": 7,
        "release_container_inspect": 7,
        "release_container_log": 7,
        "release_security_override": 4,
    }
    if set(roles) != set(expected_counts) or any(
        roles.count(role) != count for role, count in expected_counts.items()
    ):
        raise ValidationError("release retained artifact roles/counts are not exact")
    expected_lane_roles = {
        "release_lane_manifest": list(LANES),
        "release_container_inspect": list(LANES),
        "release_container_log": list(LANES),
        "release_security_override": [lane for lane in LANES if lane in SANITIZER_LANES],
    }
    for role, expected_lanes in expected_lane_roles.items():
        observed_lanes = [
            item.get("lane") for item in value if item.get("role") == role
        ]
        if observed_lanes != expected_lanes:
            raise ValidationError(
                f"release retained {role} lane order/inventory is not exact"
            )
    return inventory


@contextlib.contextmanager
def _bind_retained_artifact_manifest(
    value: Any,
) -> Iterator[list[_RetainedArtifactHandle]]:
    inventory = _retained_artifact_inventory(value)
    handles: list[_RetainedArtifactHandle] = []
    try:
        for index, item, role, role_bound, expected_hash, expected_bytes in inventory:
            handles.append(
                _RetainedArtifactHandle(
                    path=Path(item["path"]),
                    label=f"release retained artifact {index}",
                    role=role,
                    expected_sha256=expected_hash,
                    expected_bytes=expected_bytes,
                    max_bytes=role_bound,
                )
            )
        paths = [handle.path for handle in handles]
        identities = [handle.identity for handle in handles]
        if len(set(paths)) != len(paths):
            raise ValidationError(
                "release retained artifact paths are not globally unique"
            )
        if len(set(identities)) != len(identities):
            raise ValidationError("release retained artifacts alias one inode")
        yield handles
    finally:
        for handle in reversed(handles):
            handle.close()


def _assert_retained_handles_current(
    handles: list[_RetainedArtifactHandle],
    *,
    payload: bool,
) -> None:
    for handle in handles:
        if payload:
            handle.assert_payload_current()
        else:
            handle.assert_metadata_current()
    # A second metadata pass detects a mutation of an early file while a later
    # file was being read. Retained descriptors remain open through publication.
    for handle in handles:
        handle.assert_metadata_current()


def _assert_retained_artifact_manifest_current(value: Any) -> None:
    with _bind_retained_artifact_manifest(value) as handles:
        _assert_retained_handles_current(handles, payload=True)


def _hygiene_text(payload: bytes, relative: str) -> str:
    try:
        return payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(
            f"hygiene source is not valid UTF-8: {relative}"
        ) from error


def _step18_removed_field_hygiene(
    repo_root: Path | _BoundDirectory,
    *,
    attested_paths: frozenset[str] | None = None,
) -> list[str]:
    removed_field = "min_separation_" + "clearance_m"
    removed_bytes = removed_field.encode("ascii")
    allowed_test_paths = {
        "manyforge_behavior/tests/test_kernel_client.py",
        "manyforge_behavior/tests/test_program.py",
        "manyforge_composer/tests/test_roboplan_05_qualification.py",
        "manyforge_roboplan/test/test_native_e2e.py",
    }
    failures: list[str] = []
    with _hygiene_repository(repo_root) as repository:
        files = _iter_hygiene_files(
            repository,
            attested_paths=attested_paths,
        )
        for relative, payload in files:
            if removed_bytes not in payload:
                continue
            text = _hygiene_text(payload, relative)
            if relative.startswith(
                "scripts/qualification/baseline/"
            ) and relative.endswith(".patch"):
                continue
            relative_parts = Path(relative).parts
            is_test_source = (
                relative in allowed_test_paths
                or (
                    "tests" in relative_parts
                    and Path(relative).name.startswith("test_")
                )
                or (
                    "test" in relative_parts
                    and Path(relative).name.startswith("test_")
                )
            )
            if is_test_source:
                lowered = text.lower()
                if not any(
                    word in lowered
                    for word in ("unknown", "reject", "legacy", "baseline")
                ):
                    failures.append(
                        f"{relative}: removed field is not rejection/baseline evidence"
                    )
                continue
            if relative == "scripts/qualification/capture_isaac_drop_approach.py":
                required_boundary_fragments = {
                    f'if "{removed_field}" in normalized:',
                    f'clearance = normalized.pop("{removed_field}")',
                    f'"Step 18 capture rejects nonzero {removed_field}"',
                    f'contact.pop("{removed_field}", None)',
                }
                matching = {
                    line.strip()
                    for line in text.splitlines()
                    if removed_field in line
                }
                if (
                    matching == required_boundary_fragments
                    and "historical baseline" in text
                    and "never preserves or silently emulates it" in text
                ):
                    continue
            if relative == (
                "tests/cpp/perf/"
                "perf_runtime_collision_schedule_qualification.cpp"
            ):
                if (
                    text.count(removed_field) == 4
                    and text.count("#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE") >= 2
                    and (
                        "baseline mechanism replay requires captured "
                        "contact_release policy"
                    )
                    in text
                ):
                    continue
            failures.append(
                f"{relative}: removed field remains outside strict "
                "test/baseline evidence"
            )
    if failures:
        raise ValidationError("; ".join(failures))
    return [
        "removed_field_absent_from_production",
        "removed_field_occurrences_restricted_to_rejection_or_baseline_evidence",
    ]


def _step18_distance_scope_hygiene(
    repo_root: Path | _BoundDirectory,
    *,
    attested_paths: frozenset[str] | None = None,
) -> list[str]:
    producer_roots = (
        "manyforge_core/include",
        "manyforge_core/src",
        "manyforge_collision/include",
        "manyforge_behavior/manyforge_behavior",
        "manyforge_composer/backend",
        "manyforge_roboplan/include",
        "manyforge_roboplan/src",
        "manyforge_ros2_control_execution/include",
        "manyforge_ros2_control_execution/src",
    )
    forbidden_tokens = ("kAllActivePairs", "all_active_pairs")
    failures: list[str] = []
    fixed_paths = {
        "evaluator": "manyforge_core/src/collision_policy_evaluator.cpp",
        "coordinator": "manyforge_core/src/collision_validation_coordinator.cpp",
        "runtime": "manyforge_core/src/runtime_collision_query_session.cpp",
        "engine": "manyforge_collision/src/manyforge_collision_engine.cpp",
    }
    with _hygiene_repository(repo_root) as repository:
        for relative_root in producer_roots:
            try:
                files = _iter_hygiene_files(
                    repository,
                    relative_root=relative_root,
                    suffixes=frozenset({".cpp", ".hpp", ".h", ".py"}),
                    attested_paths=attested_paths,
                    reject_unattested=attested_paths is not None,
                )
                for relative, payload in files:
                    if (
                        relative
                        == "manyforge_core/include/manyforge_core/types/collision.hpp"
                    ):
                        continue
                    text = _hygiene_text(payload, relative)
                    for token in forbidden_tokens:
                        if token in text:
                            failures.append(
                                f"{relative}: production selects {token}"
                            )
            except ValidationError as error:
                failures.append(str(error))

        texts: dict[str, str] = {}
        for name, relative in fixed_paths.items():
            try:
                if attested_paths is not None and relative not in attested_paths:
                    raise ValidationError(
                        "Step 18 distance-scope source is absent from the "
                        f"candidate identity: {relative}"
                    )
                snapshot = _read_bounded_regular_file_at(
                    repository.descriptor,
                    relative,
                    f"Step 18 distance-scope source {relative}",
                    MAX_HYGIENE_FILE_BYTES,
                )
                texts[name] = _hygiene_text(snapshot.payload, relative)
            except ValidationError as error:
                failures.append(str(error))
        if failures:
            raise ValidationError("; ".join(failures))

    evaluator_text = texts["evaluator"]
    required_evaluator_fragments = (
        "entry.mode == CollisionContactPolicyMode::kBoundedPenetration",
        "ContactReleaseEvidenceMode::kSignedPenetration",
        "selected.push_back(canonicalize(entry.pair))",
        "contact_grasp_pair_matches(pair, *grasp)",
    )
    for fragment in required_evaluator_fragments:
        if fragment not in evaluator_text:
            failures.append(f"distance selection lost required fragment: {fragment}")
    for name in ("coordinator", "runtime"):
        relative = fixed_paths[name]
        text = texts[name]
        if "CollisionDistanceRequestMode::kSelectedCollidingPairs" not in text:
            failures.append(f"{relative} no longer selects colliding pairs")
        modes = set(re.findall(r"CollisionDistanceRequestMode::(k[A-Za-z]+)", text))
        unexpected = modes - {"kNone", "kSelectedCollidingPairs"}
        if unexpected:
            failures.append(
                f"{relative} selects unexpected distance "
                f"modes: {sorted(unexpected)}"
            )

    engine_text = texts["engine"]
    collision_calls = [
        match.start()
        for match in re.finditer(r"pinocchio::computeCollisions\(", engine_text)
    ]
    distance_calls = [
        match.start()
        for match in re.finditer(
            r"pinocchio::computeDistance(?:s)?\(", engine_text
        )
    ]
    if len(collision_calls) != 2 or len(distance_calls) != 4:
        failures.append(
            "collision engine call topology differs from the reviewed "
            "two-path contract"
        )
    elif not (
        collision_calls[0] < distance_calls[0] < distance_calls[1] < collision_calls[1]
        < distance_calls[2] < distance_calls[3]
    ):
        failures.append("collision engine no longer enumerates collisions before distance work")
    if engine_text.count("stop_at_first_collision=*/false") + engine_text.count(
        "stop_at_first_collision */ false"
    ) != 2:
        failures.append("collision engine does not prove complete enumeration in both paths")
    if failures:
        raise ValidationError("; ".join(failures))
    checks = [
        "production_request_producers_never_select_all_active_pairs",
        "binary_contact_selects_no_distance_pairs",
        "signed_bounded_and_contact_grasp_select_configured_pairs",
        "collision_enumeration_precedes_sparse_distance_work",
    ]
    checks.extend(
        _step18_distance_capability_hygiene(
            repo_root,
            attested_paths=attested_paths,
        )
    )
    return checks


def _step18_distance_capability_hygiene(
    repo_root: Path | _BoundDirectory,
    *,
    attested_paths: frozenset[str] | None = None,
) -> list[str]:
    paths = {
        "types": "manyforge_core/include/manyforge_core/types/collision.hpp",
        "interface": (
            "manyforge_core/include/manyforge_core/collision/"
            "i_collision_query_engine.hpp"
        ),
        "engine": "manyforge_collision/src/manyforge_collision_engine.cpp",
        "coordinator": "manyforge_core/src/collision_validation_coordinator.cpp",
        "runtime": "manyforge_core/src/runtime_collision_query_session.cpp",
    }
    failures: list[str] = []
    texts: dict[str, str] = {}
    with _hygiene_repository(repo_root) as repository:
        for name, relative in paths.items():
            try:
                if attested_paths is not None and relative not in attested_paths:
                    raise ValidationError(
                        "distance-capability source is absent from the "
                        f"candidate identity: {relative}"
                    )
                snapshot = _read_bounded_regular_file_at(
                    repository.descriptor,
                    relative,
                    f"distance-capability source {relative}",
                    MAX_HYGIENE_FILE_BYTES,
                )
                texts[name] = _hygiene_text(snapshot.payload, relative)
            except ValidationError as error:
                failures.append(str(error))
    if failures:
        raise ValidationError("; ".join(failures))

    types = texts["types"]
    descriptor_declarations = re.findall(
        r"std::vector\s*<\s*CollisionDistanceRequestMode\s*>\s*"
        r"supported_distance_request_modes\s*;",
        types,
    )
    if len(descriptor_declarations) != 1:
        failures.append(
            "distance capability descriptor does not declare one exact mode inventory"
        )
    validity_start = types.find("collision_distance_request_modes_valid(")
    exact_start = types.find("collision_engine_supports_distance_request_mode(")
    exact_end = types.find("/// Compare-mode policy", exact_start)
    if validity_start < 0 or exact_start < 0 or exact_end < 0:
        failures.append("shared exact distance-scope admission helpers are missing")
        validity = ""
        exact = ""
    else:
        validity = types[validity_start:exact_start]
        exact = types[exact_start:exact_end]
    required_validity_fragments = (
        "modes.empty()",
        "static_cast<int>(modes[index - 1]) >=",
        "!capability.supports_distance_query",
    )
    if any(fragment not in validity for fragment in required_validity_fragments):
        failures.append(
            "distance mode declaration no longer fails closed on empty, unordered, "
            "duplicate, or contradictory scopes"
        )
    if (
        "collision_distance_request_modes_valid(capability)" not in exact
        or "std::binary_search(" not in exact
        or "requested_mode" not in exact
        or "kAllCollidingPairs" in exact
        or "kAllActivePairs" in exact
    ):
        failures.append(
            "shared distance-scope admission is not an exact declared-mode lookup"
        )

    engine = texts["engine"]
    declarations = re.findall(
        r"capability_\.supported_distance_request_modes\s*=\s*\{(.*?)\};",
        engine,
        flags=re.DOTALL,
    )
    declared_modes = (
        re.findall(r"CollisionDistanceRequestMode::(k[A-Za-z]+)", declarations[0])
        if len(declarations) == 1
        else []
    )
    expected_modes = [
        "kNone",
        "kSelectedCollidingPairs",
        "kAllCollidingPairs",
        "kAllActivePairs",
    ]
    if declared_modes != expected_modes:
        failures.append(
            "ManyForge collision engine distance scopes are not the exact sorted "
            "none/selected/all-colliding/all-active inventory"
        )

    interface = texts["interface"]
    status_start = interface.find("runtime_collision_query_capability_status(")
    status_end = interface.find(
        "runtime_collision_query_capability_status_name(", status_start
    )
    status = (
        interface[status_start:status_end]
        if status_start >= 0 and status_end >= 0
        else ""
    )
    if "collision_distance_request_modes_valid(capability)" not in status:
        failures.append(
            "runtime capability admission no longer rejects an invalid exact-scope inventory"
        )

    coordinator = texts["coordinator"]
    admission_start = coordinator.find("bool engine_admits_query_capabilities(")
    admission_end = coordinator.find(
        "CollisionResult capability_admission_failure(", admission_start
    )
    admission = (
        coordinator[admission_start:admission_end]
        if admission_start >= 0 and admission_end >= 0
        else ""
    )
    if (
        "collision_engine_supports_distance_request_mode(" not in admission
        or "capability, distance_request_mode" not in admission
        or coordinator.count("engine_admits_query_capabilities(") != 3
    ):
        failures.append(
            "validation does not admit both query paths through exact distance scope"
        )
    required_start = coordinator.find("std::vector<std::string> required_capabilities_for(")
    required_end = coordinator.find("bool engine_admits_query_capabilities(", required_start)
    required = (
        coordinator[required_start:required_end]
        if required_start >= 0 and required_end >= 0
        else ""
    )
    if (
        "CollisionDistanceRequestMode distance_request_mode" not in required
        or "distance_request_mode != CollisionDistanceRequestMode::kNone" not in required
        or 'capabilities.push_back("penetration_depth")' not in required
    ):
        failures.append(
            "validation required capabilities are no longer scoped by the selected mode"
        )

    runtime = texts["runtime"]
    bind_start = runtime.find("bool RuntimeCollisionQuerySession::bind_distance_selection(")
    bind_end = runtime.find("bool RuntimeCollisionQuerySession::bind_policy(", bind_start)
    binding = (
        runtime[bind_start:bind_end]
        if bind_start >= 0 and bind_end >= 0
        else ""
    )
    if (
        runtime.count("collision_engine_supports_distance_request_mode(") != 1
        or "collision_engine_supports_distance_request_mode(*capability_," not in binding
        or "requested_mode" not in binding
    ):
        failures.append(
            "runtime policy binding does not use exact distance-scope admission"
        )
    if any(
        token in coordinator or token in runtime
        for token in (
            "CollisionDistanceRequestMode::kAllCollidingPairs",
            "CollisionDistanceRequestMode::kAllActivePairs",
        )
    ):
        failures.append(
            "validation/runtime contains a broader distance-scope fallback"
        )

    if failures:
        raise ValidationError("; ".join(failures))
    return [
        "distance_scope_capability_descriptor_is_explicit",
        "distance_scope_declaration_is_sorted_unique_and_exactly_admitted",
        "manyforge_engine_publishes_all_four_exact_distance_scopes",
        "validation_uses_exact_scope_admission_on_both_query_paths",
        "runtime_uses_exact_scope_admission",
        "distance_capabilities_are_mode_scoped_without_broader_fallback",
    ]


def run_step18_hygiene(repo_root: Path) -> dict[str, Any]:
    binding = _open_absolute_directory(repo_root, "hygiene repository")
    try:
        try:
            initial_identity = source_identity(binding.path)
        except SourceIdentityError as error:
            raise ValidationError(
                f"cannot capture initial hygiene source identity: {error}"
            ) from error
        attested_paths = _step18_attested_source_paths(binding)
        checks = _step18_removed_field_hygiene(
            binding,
            attested_paths=attested_paths,
        )
        checks.extend(
            _step18_distance_scope_hygiene(
                binding,
                attested_paths=attested_paths,
            )
        )
        binding.assert_current("hygiene repository")
        final_attested_paths = _step18_attested_source_paths(binding)
        try:
            final_identity = source_identity(binding.path)
        except SourceIdentityError as error:
            raise ValidationError(
                f"cannot capture final hygiene source identity: {error}"
            ) from error
        # Exact pre/post identity and inventory equality detects every
        # persistent mutation of candidate source while the scan runs.  Like
        # any userspace observer, it cannot detect an equally privileged
        # process that edits and restores the same candidate bytes and path
        # inventory between the two identity observations.
        if final_attested_paths != attested_paths:
            raise ValidationError(
                "candidate source path inventory changed during Step 18 hygiene"
            )
        if final_identity != initial_identity:
            raise ValidationError(
                "candidate source identity changed during Step 18 hygiene"
            )
        binding.assert_current("hygiene repository")
        return {
            "schema_version": "manyforge.step18.release_gate_hygiene.v1",
            "checks": checks,
            "check_count": len(checks),
            "passed": True,
            "overall_step18_closure_claimed": False,
        }
    finally:
        binding.close()


@contextlib.contextmanager
def _bind_publication_parent(
    repo_root: Path,
    destination: Path,
) -> Iterator[tuple[_BoundDirectory, _BoundDirectory, str, Path]]:
    repo = _open_absolute_directory(repo_root, "publication repository")
    parent: _BoundDirectory | None = None
    try:
        absolute_destination = _lexical_absolute(destination, "release summary")
        try:
            relative = absolute_destination.relative_to(repo.path)
        except ValueError as error:
            raise ValidationError(
                "release summary output must be inside the candidate repository"
            ) from error
        if (
            len(relative.parts) < 3
            or relative.parts[:2] != (".manyforge-var", "qualification")
            or any(part in ("", ".", "..") for part in relative.parts)
        ):
            raise ValidationError(
                "release summary output must be below .manyforge-var/qualification"
            )
        descriptors = [os.dup(repo.descriptor)]
        names: list[str] = []
        try:
            for name in relative.parts[:-1]:
                try:
                    os.mkdir(name, mode=0o700, dir_fd=descriptors[-1])
                    os.fsync(descriptors[-1])
                except FileExistsError:
                    pass
                descriptor = os.open(
                    name,
                    os.O_RDONLY
                    | os.O_CLOEXEC
                    | os.O_DIRECTORY
                    | os.O_NOFOLLOW,
                    dir_fd=descriptors[-1],
                )
                metadata = os.fstat(descriptor)
                if (
                    not stat.S_ISDIR(metadata.st_mode)
                    or metadata.st_uid != os.getuid()
                    or metadata.st_mode & 0o022
                ):
                    os.close(descriptor)
                    raise ValidationError(
                        "release summary directories must be current-user-owned "
                        "and not group/world writable"
                    )
                descriptors.append(descriptor)
                names.append(name)
            parent = _BoundDirectory(
                absolute_destination.parent,
                descriptors,
                names,
            )
            parent.assert_current("release summary parent")
        except BaseException:
            for descriptor in reversed(descriptors):
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            raise
        yield repo, parent, relative.parts[-1], relative
    finally:
        if parent is not None:
            parent.close()
        repo.close()


def _publication_path_is_ignored(
    repo: _BoundDirectory,
    relative: Path,
    head_commit: str,
) -> bool:
    environment = {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    }
    git_prefix = [
        "git",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.untrackedCache=false",
        "-C",
        f"/proc/self/fd/{repo.descriptor}",
        "cat-file",
    ]
    object_name = f"{head_commit}:.gitignore"

    def run_cat_file(mode: str) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [*git_prefix, mode, object_name],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=environment,
            pass_fds=(repo.descriptor,),
            timeout=GIT_CHECK_TIMEOUT_SECONDS,
        )

    try:
        size_result = run_cat_file("-s")
        if (
            size_result.returncode != 0
            or len(size_result.stdout) > 32
            or not size_result.stdout.strip().isdigit()
        ):
            raise ValidationError(
                "candidate HEAD does not provide a bounded .gitignore blob"
            )
        declared_size = int(size_result.stdout.strip())
        if declared_size > MAX_QUALIFICATION_INPUT_BYTES:
            raise ValidationError("candidate .gitignore exceeds the input byte bound")
        ignored = run_cat_file("blob")
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValidationError(
            f"cannot prove release summary ignore provenance: {error}"
        ) from error
    if ignored.returncode != 0:
        raise ValidationError(
            "candidate HEAD does not provide the release summary ignore rule"
        )
    if len(ignored.stdout) != declared_size:
        raise ValidationError("candidate .gitignore changed while being read")
    try:
        committed_text = ignored.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError("candidate .gitignore is not valid UTF-8") from error
    if committed_text.splitlines().count(".manyforge-var/") != 1:
        return False
    current = _read_bounded_regular_file_at(
        repo.descriptor,
        ".gitignore",
        "publication .gitignore",
        MAX_QUALIFICATION_INPUT_BYTES,
    )
    if current.payload != ignored.stdout:
        raise ValidationError(
            "publication .gitignore differs from its source-controlled version"
        )
    if relative.parts[:1] != (".manyforge-var",):
        return False
    try:
        effective = subprocess.run(
            [
                "git",
                "--no-pager",
                "-c",
                "core.fsmonitor=false",
                "-c",
                "core.untrackedCache=false",
                "-c",
                "core.excludesFile=/dev/null",
                "-C",
                f"/proc/self/fd/{repo.descriptor}",
                "check-ignore",
                "--quiet",
                "--no-index",
                "--",
                relative.as_posix(),
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=environment,
            pass_fds=(repo.descriptor,),
            timeout=GIT_CHECK_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValidationError(
            f"cannot prove effective release summary ignore rule: {error}"
        ) from error
    if effective.returncode not in (0, 1):
        raise ValidationError(
            "Git could not evaluate the committed release summary ignore rule"
        )
    return effective.returncode == 0


def _unlink_matching_file(
    directory_descriptor: int,
    name: str,
    identity: tuple[int, int] | None,
) -> bool:
    if identity is None:
        return False
    try:
        metadata = os.stat(
            name,
            dir_fd=directory_descriptor,
            follow_symlinks=False,
        )
    except FileNotFoundError:
        return False
    except OSError as error:
        raise ValidationError(
            f"cannot inspect owned publication rollback file {name}: {error}"
        ) from error
    if (
        not stat.S_ISREG(metadata.st_mode)
        or (metadata.st_dev, metadata.st_ino) != identity
    ):
        return False
    try:
        os.unlink(name, dir_fd=directory_descriptor)
        os.fsync(directory_descriptor)
    except OSError as error:
        raise ValidationError(
            f"cannot durably remove owned publication rollback file {name}: {error}"
        ) from error
    return True


def _render_summary(result: dict[str, Any]) -> bytes:
    try:
        payload = (
            json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError, RecursionError) as error:
        raise ValidationError(f"cannot serialize release summary: {error}") from error
    if len(payload) > MAX_OUTPUT_ARTIFACT_BYTES:
        raise ValidationError("release summary exceeds the output artifact bound")
    return payload


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise ValidationError("short write while staging release summary")
        offset += written


def _assert_descriptor_payload(
    descriptor: int,
    payload: bytes,
    label: str,
) -> os.stat_result:
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode) or before.st_size != len(payload):
        raise ValidationError(f"{label} identity or size changed")
    os.lseek(descriptor, 0, os.SEEK_SET)
    observed = bytearray()
    while len(observed) < len(payload):
        chunk = os.read(
            descriptor,
            min(1024 * 1024, len(payload) - len(observed)),
        )
        if not chunk:
            break
        observed.extend(chunk)
    if bytes(observed) != payload or os.read(descriptor, 1):
        raise ValidationError(f"{label} content changed")
    after = os.fstat(descriptor)
    if _metadata_signature(after) != _metadata_signature(before):
        raise ValidationError(f"{label} changed while being read")
    return after


def _assert_publication_candidate_current(
    *,
    repo_root: Path,
    specs_root: Path,
    campaign_source: dict[str, Any],
    campaign_specs: dict[str, Any],
    campaign_contract: dict[str, Any],
    phase: str,
) -> None:
    if source_identity(repo_root) != campaign_source:
        raise ValidationError(
            f"ManyForge source changed {phase} release summary publication"
        )
    publication_specs = source_identity(specs_root)
    if (
        publication_specs["head_commit"] != campaign_specs["head_commit"]
        or publication_specs["head_tree"] != campaign_specs["head_tree"]
    ):
        raise ValidationError(
            f"full Specs Git base changed {phase} release summary publication"
        )
    if specs_contract_identity(specs_root) != campaign_contract:
        raise ValidationError(
            f"Specs contract changed {phase} release summary publication"
        )


def publish_verification_summary(
    output: Path, result: dict[str, Any], manifest_path: Path
) -> None:
    payload = _render_summary(result)
    campaign = _load_json(
        manifest_path,
        "release-gate artifact manifest",
        MAX_ARTIFACT_MANIFEST_BYTES,
    )
    candidate = campaign.get("candidate")
    if not isinstance(candidate, dict):
        raise ValidationError("release-gate publication candidate is missing")
    repo_value = candidate.get("manyforge_root")
    specs_value = candidate.get("specs_root")
    if not isinstance(repo_value, str) or not isinstance(specs_value, str):
        raise ValidationError("release-gate publication roots must be strings")
    repo_root = _lexical_absolute(Path(repo_value), "publication ManyForge root")
    specs_root = _lexical_absolute(Path(specs_value), "publication Specs root")
    campaign_source = _identity(candidate.get("source_identity"), "campaign source")
    campaign_specs = _identity(
        candidate.get("specs_source_identity"),
        "campaign Specs source",
    )
    campaign_contract = _contract_identity(
        candidate.get("specs_contract_identity"),
        "campaign Specs contract",
    )
    if campaign_source != _identity(
        result.get("candidate_source_identity"),
        "summary source",
    ) or campaign_specs != _identity(
        result.get("candidate_specs_source_identity"),
        "summary Specs source",
    ) or campaign_contract != _contract_identity(
        result.get("candidate_specs_contract_identity"),
        "summary Specs contract",
    ):
        raise ValidationError("release summary candidate differs from its campaign")

    retained_value = result.get("retained_artifact_manifest")
    # Preserve the public assertion hook and then retain the same exact files
    # by descriptor until the publication linearization point.
    _assert_retained_artifact_manifest_current(retained_value)
    with contextlib.ExitStack() as publication_stack:
        retained_handles = publication_stack.enter_context(
            _bind_retained_artifact_manifest(retained_value)
        )
        repo, parent, destination_name, relative = publication_stack.enter_context(
            _bind_publication_parent(repo_root, output)
        )
        if not _publication_path_is_ignored(
            repo,
            relative,
            campaign_source["head_commit"],
        ):
            raise ValidationError("release summary output is not ignored")
        try:
            os.stat(
                destination_name,
                dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            pass
        except OSError as error:
            raise ValidationError(f"cannot inspect release summary output: {error}") from error
        else:
            raise ValidationError(f"refusing to overwrite release summary: {output}")

        temporary_name = (
            f".{destination_name}.tmp-{os.getpid()}-{secrets.token_hex(8)}"
        )
        temporary_descriptor = -1
        temporary_identity: tuple[int, int] | None = None
        publication_identity: tuple[int, int] | None = None
        try:
            temporary_descriptor = os.open(
                temporary_name,
                os.O_RDWR
                | os.O_CREAT
                | os.O_EXCL
                | os.O_CLOEXEC
                | os.O_NOFOLLOW,
                0o600,
                dir_fd=parent.descriptor,
            )
            created = os.fstat(temporary_descriptor)
            temporary_identity = (created.st_dev, created.st_ino)
            if (
                not stat.S_ISREG(created.st_mode)
                or created.st_nlink != 1
                or created.st_uid != os.getuid()
            ):
                raise ValidationError("release summary staging file is unsafe")
            _write_all(temporary_descriptor, payload)
            os.fsync(temporary_descriptor)

            _assert_publication_candidate_current(
                repo_root=repo_root,
                specs_root=specs_root,
                campaign_source=campaign_source,
                campaign_specs=campaign_specs,
                campaign_contract=campaign_contract,
                phase="before",
            )
            repo.assert_current("publication repository")
            parent.assert_current("release summary parent")
            _assert_retained_handles_current(retained_handles, payload=True)

            staged = _assert_descriptor_payload(
                temporary_descriptor,
                payload,
                "release summary staging",
            )
            if (
                staged.st_nlink != 1
                or (staged.st_dev, staged.st_ino) != temporary_identity
                or staged.st_size != len(payload)
                or not _path_matches_file(
                    parent.descriptor,
                    temporary_name,
                    temporary_identity,
                )
            ):
                raise ValidationError("release summary staging identity changed")

            # Bind the would-be destination identity before the syscall so
            # rollback can safely detect and remove our inode even if link(2)
            # creates the name but its wrapper still reports an error.
            publication_identity = temporary_identity
            os.link(
                temporary_name,
                destination_name,
                src_dir_fd=parent.descriptor,
                dst_dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
            if not _path_matches_file(
                parent.descriptor,
                destination_name,
                temporary_identity,
                links=2,
            ) or not _path_matches_file(
                parent.descriptor,
                temporary_name,
                temporary_identity,
                links=2,
            ):
                raise ValidationError("release summary publication identity changed")
            os.unlink(temporary_name, dir_fd=parent.descriptor)
            temporary_identity = None
            if not _path_matches_file(
                parent.descriptor,
                destination_name,
                publication_identity,
            ):
                raise ValidationError("published release summary is not one bound file")
            repo.assert_current("publication repository")
            parent.assert_current("release summary parent")
            os.fsync(temporary_descriptor)
            os.fsync(parent.descriptor)
            _assert_descriptor_payload(
                temporary_descriptor,
                payload,
                "published release summary",
            )
            if not _path_matches_file(
                parent.descriptor,
                destination_name,
                publication_identity,
            ):
                raise ValidationError(
                    "published release summary changed after directory sync"
                )
            _assert_retained_handles_current(retained_handles, payload=False)
            _assert_publication_candidate_current(
                repo_root=repo_root,
                specs_root=specs_root,
                campaign_source=campaign_source,
                campaign_specs=campaign_specs,
                campaign_contract=campaign_contract,
                phase="during",
            )
            repo.assert_current("publication repository")
            parent.assert_current("release summary parent")
            # The final pass re-reads every retained byte through the descriptors
            # held since binding, then proves each pathname still names that inode.
            _assert_retained_handles_current(retained_handles, payload=True)
            _assert_descriptor_payload(
                temporary_descriptor,
                payload,
                "published release summary",
            )
            if not _path_matches_file(
                parent.descriptor,
                destination_name,
                publication_identity,
            ):
                raise ValidationError(
                    "published release summary path changed at final validation"
                )
        except BaseException as publication_error:
            cleanup_errors: list[BaseException] = []
            cleanup_targets = []
            if publication_identity is not None:
                cleanup_targets.append(
                    (destination_name, publication_identity)
                )
            cleanup_targets.append((temporary_name, temporary_identity))
            for cleanup_name, cleanup_identity in cleanup_targets:
                try:
                    _unlink_matching_file(
                        parent.descriptor,
                        cleanup_name,
                        cleanup_identity,
                    )
                except BaseException as cleanup_error:
                    cleanup_errors.append(cleanup_error)
            if cleanup_errors:
                raise ValidationError(
                    "release summary rollback could not durably remove an "
                    f"owned file: {cleanup_errors[0]}"
                ) from publication_error
            raise
        finally:
            if temporary_descriptor >= 0:
                try:
                    os.close(temporary_descriptor)
                except OSError:
                    pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify", help="verify all seven retained lanes")
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.add_argument("--output", type=Path, required=True)
    hygiene_parser = subparsers.add_parser(
        "hygiene", help="run Step 18 removed-field and distance-scope guards"
    )
    hygiene_parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "hygiene":
            result = run_step18_hygiene(args.repo_root)
            print(
                "STEP18_SPECIFIC_HYGIENE passed "
                f"checks={result['check_count']}"
            )
            print(json.dumps(result, sort_keys=True, allow_nan=False))
            return 0
        result = verify_artifact_manifest(args.manifest)
        publish_verification_summary(args.output, result, args.manifest)
    except (OSError, SourceIdentityError, ValidationError, ValueError) as error:
        print(f"Step 18 release-gate verification failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"passed": True, "lanes": len(LANES)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
