#!/usr/bin/env python3
"""Capture one commit-, tree-, and delta-bound Git source identity.

The framing is intentionally identical to the v1 identity used by the live
qualification collector.  Qualification shell runners use this standalone
entry point so they do not need to carry subtly different digest clones.
Normal Git directories, gitfile-based linked/separate worktrees, common
directories, and split-index files are descriptor-bound; unsupported or
mutable control layouts fail closed instead of weakening the v1 claim.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import selectors
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Iterator, NamedTuple


SCHEMA = "manyforge.source_identity.v1"
SPECS_CONTRACT_SCHEMA = "manyforge.specs_contract_identity.v1"
SPECS_CONTRACT_EXCLUDED_PATH = (
    "docs/implementation/"
    "contact-release-evidence-modes-step18-evidence-2026-07-19.md"
)
FIELDS = (
    "schema_version",
    "head_commit",
    "head_tree",
    "dirty",
    "delta_sha256",
    "fingerprint_sha256",
)

GIT_TIMEOUT_SECONDS = 30
MAX_GIT_STDOUT_BYTES = 256 * 1024 * 1024
MAX_GIT_STDERR_BYTES = 1024 * 1024
MAX_GIT_LIST_BYTES = 16 * 1024 * 1024
MAX_SOURCE_PATH_BYTES = 4096
MAX_SOURCE_FILES = 100_000
MAX_UNTRACKED_FILE_BYTES = 128 * 1024 * 1024
MAX_UNTRACKED_TOTAL_BYTES = 512 * 1024 * 1024
MAX_SYMLINK_TOTAL_BYTES = 16 * 1024 * 1024
MAX_GIT_CONTROL_FILE_BYTES = 4096
MAX_GIT_SHARED_INDEX_FILES = 256
MAX_GIT_SHARED_INDEX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_GIT_LOCAL_INPUT_TOTAL_BYTES = 32 * 1024 * 1024

# These command-scope values remove local-machine choices from the v1 source
# bytes. Repository/worktree config is still inspected separately below so a
# hidden local rule cannot be mistaken for a harmless overridden preference.
CANONICAL_GIT_CONFIG = (
    ("color.diff", "false"),
    ("color.ui", "false"),
    ("core.attributesFile", os.devnull),
    ("core.autocrlf", "false"),
    ("core.bigFileThreshold", "512m"),
    ("core.checkStat", "default"),
    ("core.commitGraph", "false"),
    ("core.eol", "lf"),
    ("core.excludesFile", os.devnull),
    ("core.fileMode", "true"),
    ("core.fsmonitor", "false"),
    ("core.ignoreCase", "false"),
    ("core.ignoreStat", "false"),
    ("core.precomposeUnicode", "false"),
    ("core.quotePath", "true"),
    ("core.safecrlf", "false"),
    ("core.sparseCheckout", "false"),
    ("core.sparseCheckoutCone", "false"),
    ("core.symlinks", "true"),
    ("core.trustCtime", "true"),
    ("core.untrackedCache", "false"),
    ("diff.algorithm", "default"),
    ("diff.context", "3"),
    ("diff.ignoreSubmodules", "dirty"),
    ("diff.indentHeuristic", "true"),
    ("diff.interHunkContext", "0"),
    ("diff.mnemonicPrefix", "false"),
    ("diff.noprefix", "false"),
    ("diff.relative", "false"),
    ("diff.renameLimit", "1000"),
    ("diff.renames", "true"),
    ("diff.submodule", "short"),
    ("diff.suppressBlankEmpty", "false"),
    ("index.sparse", "false"),
)


class _FileSnapshot(NamedTuple):
    payload: bytes
    metadata: os.stat_result


class _UntrackedSignature(NamedTuple):
    encoded_name: bytes
    metadata_signature: tuple[int, int, int, int, int, int, int]
    regular: bool


class _BoundSharedIndex(NamedTuple):
    parent_descriptor: int
    name: str
    descriptor: int
    binding_signature: tuple[int, int, int, int, int]
    content_sha256: str


class _BoundDirectory:
    """One no-symlink absolute directory chain retained by descriptors."""

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
                raise SourceIdentityError(
                    f"{label} directory path changed: {error}"
                ) from error
            if (
                not stat.S_ISDIR(path_metadata.st_mode)
                or (path_metadata.st_dev, path_metadata.st_ino)
                != (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
            ):
                raise SourceIdentityError(f"{label} directory path changed")

    def close(self) -> None:
        for descriptor in reversed(self._descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        self._descriptors.clear()


class _RepositoryBinding:
    """Descriptor-bound work tree, Git directory, common directory, and index."""

    def __init__(
        self,
        *,
        root: _BoundDirectory,
        git_directory: _BoundDirectory | None,
        git_descriptor: int,
        common_directory: _BoundDirectory | None,
        common_descriptor: int,
        control_files: list[
            tuple[int, str, int, tuple[int, int, int, int, int, int, int]]
        ],
        shared_indexes: list[_BoundSharedIndex],
        index_descriptor: int,
        index_signature: tuple[int, int, int, int, int, int, int],
    ) -> None:
        self.root = root
        self.git_directory = git_directory
        self.git_descriptor = git_descriptor
        self.common_directory = common_directory
        self.common_descriptor = common_descriptor
        self.control_files = control_files
        self.shared_indexes = shared_indexes
        self.index_descriptor = index_descriptor
        self.index_signature = index_signature

    @property
    def pass_fds(self) -> tuple[int, ...]:
        return tuple(
            sorted(
                {
                    self.root.descriptor,
                    self.git_descriptor,
                    self.common_descriptor,
                    self.index_descriptor,
                }
            )
        )

    @property
    def environment(self) -> dict[str, str]:
        return {
            "GCM_INTERACTIVE": "Never",
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_ASKPASS": "/bin/false",
            "GIT_COMMON_DIR": f"/proc/self/fd/{self.common_descriptor}",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_DIR": f"/proc/self/fd/{self.git_descriptor}",
            "GIT_INDEX_FILE": f"/proc/self/fd/{self.index_descriptor}",
            "GIT_NO_REPLACE_OBJECTS": "1",
            "GIT_NO_LAZY_FETCH": "1",
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_WORK_TREE": f"/proc/self/fd/{self.root.descriptor}",
            "LANG": "C",
            "LC_ALL": "C",
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "SSH_ASKPASS": "/bin/false",
            "SSH_ASKPASS_REQUIRE": "never",
        }

    def _assert_control_files_current(self) -> None:
        for parent, name, descriptor, signature in self.control_files:
            try:
                path_metadata = os.stat(
                    name,
                    dir_fd=parent,
                    follow_symlinks=False,
                )
                descriptor_metadata = os.fstat(descriptor)
            except OSError as error:
                raise SourceIdentityError(
                    f"Git control file {name} changed: {error}"
                ) from error
            if (
                _metadata_signature(path_metadata) != signature
                or _metadata_signature(descriptor_metadata) != signature
            ):
                raise SourceIdentityError(f"Git control file {name} changed")

    def _assert_index_current(self) -> None:
        try:
            index_path_metadata = os.stat(
                "index",
                dir_fd=self.git_descriptor,
                follow_symlinks=False,
            )
            index_metadata = os.fstat(self.index_descriptor)
        except OSError as error:
            raise SourceIdentityError(f"Git index changed: {error}") from error
        if (
            _metadata_signature(index_path_metadata) != self.index_signature
            or _metadata_signature(index_metadata) != self.index_signature
        ):
            raise SourceIdentityError("Git index changed")

    def _assert_directories_current(self) -> None:
        self.root.assert_current("source repository")
        if self.git_directory is not None:
            self.git_directory.assert_current("Git directory")
        if self.common_directory is not None:
            self.common_directory.assert_current("Git common directory")

    def assert_current(self) -> None:
        self._assert_directories_current()
        self._assert_control_files_current()
        self._assert_index_current()
        shared_index_total = 0
        for shared_index in self.shared_indexes:
            try:
                path_metadata = os.stat(
                    shared_index.name,
                    dir_fd=shared_index.parent_descriptor,
                    follow_symlinks=False,
                )
                descriptor_metadata = os.fstat(shared_index.descriptor)
            except OSError as error:
                raise SourceIdentityError(
                    f"Git split index {shared_index.name} changed: {error}"
                ) from error
            if (
                _shared_index_binding_signature(path_metadata)
                != shared_index.binding_signature
                or _shared_index_binding_signature(descriptor_metadata)
                != shared_index.binding_signature
            ):
                raise SourceIdentityError(
                    f"Git split index {shared_index.name} changed"
                )
            shared_index_total += descriptor_metadata.st_size
            if shared_index_total > MAX_GIT_SHARED_INDEX_TOTAL_BYTES:
                raise SourceIdentityError(
                    "Git split-index payload exceeds the aggregate byte bound"
                )
            if (
                _descriptor_content_sha256(
                    shared_index.descriptor,
                    expected_size=descriptor_metadata.st_size,
                    label=f"Git split index {shared_index.name}",
                )
                != shared_index.content_sha256
            ):
                raise SourceIdentityError(
                    f"Git split index {shared_index.name} content changed"
                )
            try:
                final_path_metadata = os.stat(
                    shared_index.name,
                    dir_fd=shared_index.parent_descriptor,
                    follow_symlinks=False,
                )
                final_descriptor_metadata = os.fstat(shared_index.descriptor)
            except OSError as error:
                raise SourceIdentityError(
                    f"Git split index {shared_index.name} changed: {error}"
                ) from error
            if (
                _shared_index_binding_signature(final_path_metadata)
                != shared_index.binding_signature
                or _shared_index_binding_signature(final_descriptor_metadata)
                != shared_index.binding_signature
            ):
                raise SourceIdentityError(
                    f"Git split index {shared_index.name} changed during hashing"
                )
        self._assert_directories_current()
        self._assert_control_files_current()
        self._assert_index_current()

    def close(self) -> None:
        try:
            os.close(self.index_descriptor)
        except OSError:
            pass
        for _, _, descriptor, _ in self.control_files:
            try:
                os.close(descriptor)
            except OSError:
                pass
        for shared_index in self.shared_indexes:
            try:
                os.close(shared_index.descriptor)
            except OSError:
                pass
        if self.common_directory is not None:
            self.common_directory.close()
        if self.git_directory is not None:
            self.git_directory.close()
        else:
            try:
                os.close(self.git_descriptor)
            except OSError:
                pass
        self.root.close()


class SourceIdentityError(RuntimeError):
    """The requested checkout cannot be represented by the v1 contract."""


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


def _shared_index_binding_signature(
    metadata: os.stat_result,
) -> tuple[int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_size,
    )


def _descriptor_content_sha256(
    descriptor: int,
    *,
    expected_size: int,
    label: str,
) -> str:
    try:
        before = os.fstat(descriptor)
    except OSError as error:
        raise SourceIdentityError(f"cannot inspect {label}: {error}") from error
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
        or before.st_size != expected_size
    ):
        raise SourceIdentityError(f"{label} binding changed before hashing")
    digest = hashlib.sha256()
    offset = 0
    try:
        while offset < expected_size:
            payload = os.pread(
                descriptor,
                min(1024 * 1024, expected_size - offset),
                offset,
            )
            if not payload:
                raise SourceIdentityError(
                    f"{label} ended before its recorded size"
                )
            digest.update(payload)
            offset += len(payload)
        if os.pread(descriptor, 1, expected_size):
            raise SourceIdentityError(f"{label} grew while being hashed")
        after = os.fstat(descriptor)
    except OSError as error:
        raise SourceIdentityError(f"cannot hash {label}: {error}") from error
    if (
        _shared_index_binding_signature(after)
        != _shared_index_binding_signature(before)
    ):
        raise SourceIdentityError(f"{label} changed while being hashed")
    return digest.hexdigest()


def _lexical_absolute(path: Path, label: str) -> Path:
    raw = os.fspath(path)
    if not isinstance(raw, str) or not raw or "\0" in raw:
        raise SourceIdentityError(f"{label} path is invalid")
    if len(os.fsencode(raw)) > MAX_SOURCE_PATH_BYTES:
        raise SourceIdentityError(f"{label} path exceeds the source path bound")
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
        raise SourceIdentityError(f"cannot bind filesystem root: {error}") from error
    names: list[str] = []
    try:
        for name in absolute.parts[1:]:
            if name in ("", ".", ".."):
                raise SourceIdentityError(f"{label} path is not canonical")
            try:
                descriptor = os.open(
                    name,
                    os.O_RDONLY
                    | os.O_CLOEXEC
                    | os.O_DIRECTORY
                    | os.O_NOFOLLOW,
                    dir_fd=descriptors[-1],
                )
            except OSError as error:
                raise SourceIdentityError(
                    f"cannot bind {label} without symbolic-link traversal: {error}"
                ) from error
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                os.close(descriptor)
                raise SourceIdentityError(f"{label} is not a directory")
            descriptors.append(descriptor)
            names.append(name)
        binding = _BoundDirectory(absolute, descriptors, names)
        binding.assert_current(label)
        return binding
    except BaseException:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise


def _path_has_identity(
    parent_descriptor: int,
    name: str,
    identity: tuple[int, int],
    *,
    regular: bool,
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
        (stat.S_ISREG(metadata.st_mode) if regular else stat.S_ISLNK(metadata.st_mode))
        and metadata.st_nlink == 1
        and (metadata.st_dev, metadata.st_ino) == identity
    )


def _read_regular_at(
    parent_descriptor: int,
    name: str,
    *,
    label: str,
    max_bytes: int,
    keep_descriptor: bool = False,
) -> tuple[_FileSnapshot, int | None]:
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_descriptor,
        )
    except OSError as error:
        raise SourceIdentityError(f"cannot open {label}: {error}") from error
    retained: int | None = None
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise SourceIdentityError(
                f"{label} must be one non-hardlinked regular file"
            )
        if metadata.st_size < 0 or metadata.st_size > max_bytes:
            raise SourceIdentityError(f"{label} exceeds the source byte bound")
        chunks: list[bytes] = []
        remaining = metadata.st_size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise SourceIdentityError(f"{label} ended before its recorded size")
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise SourceIdentityError(f"{label} grew while being read")
        after = os.fstat(descriptor)
        identity = (metadata.st_dev, metadata.st_ino)
        if (
            _metadata_signature(after) != _metadata_signature(metadata)
            or not _path_has_identity(
                parent_descriptor,
                name,
                identity,
                regular=True,
            )
        ):
            raise SourceIdentityError(f"{label} changed while being read")
        if keep_descriptor:
            retained = descriptor
            descriptor = -1
        return _FileSnapshot(b"".join(chunks), metadata), retained
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _bind_regular_at(
    parent_descriptor: int,
    name: str,
    *,
    label: str,
    max_bytes: int,
) -> tuple[int, tuple[int, int, int, int, int, int, int]]:
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_descriptor,
        )
    except OSError as error:
        raise SourceIdentityError(f"cannot bind {label}: {error}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise SourceIdentityError(
                f"{label} must be one non-hardlinked regular file"
            )
        if metadata.st_size < 0 or metadata.st_size > max_bytes:
            raise SourceIdentityError(f"{label} exceeds the source byte bound")
        signature = _metadata_signature(metadata)
        if not _path_has_identity(
            parent_descriptor,
            name,
            (metadata.st_dev, metadata.st_ino),
            regular=True,
        ):
            raise SourceIdentityError(f"{label} path changed while being bound")
        return descriptor, signature
    except BaseException:
        os.close(descriptor)
        raise


def _bind_shared_index_files(
    parent_descriptor: int,
    shared_indexes: list[_BoundSharedIndex],
    *,
    total_bytes: int,
) -> int:
    names: list[str] = []
    entries_seen = 0
    try:
        with os.scandir(parent_descriptor) as entries:
            for entry in entries:
                entries_seen += 1
                if entries_seen > MAX_SOURCE_FILES:
                    raise SourceIdentityError(
                        "Git directory inventory exceeds the file bound"
                    )
                name = entry.name
                if not name.startswith("sharedindex."):
                    continue
                suffix = name.removeprefix("sharedindex.")
                if len(suffix) not in (40, 64) or any(
                    character not in "0123456789abcdef" for character in suffix
                ):
                    continue
                names.append(name)
    except OSError as error:
        raise SourceIdentityError(
            f"cannot inspect Git split-index files: {error}"
        ) from error
    if len(names) > MAX_GIT_SHARED_INDEX_FILES:
        raise SourceIdentityError("Git split-index inventory exceeds the file bound")
    for name in sorted(names):
        if len(shared_indexes) >= MAX_GIT_SHARED_INDEX_FILES:
            raise SourceIdentityError(
                "Git split-index inventory exceeds the aggregate file bound"
            )
        descriptor, _ = _bind_regular_at(
            parent_descriptor,
            name,
            label=f"Git split index {name}",
            max_bytes=MAX_GIT_STDOUT_BYTES,
        )
        try:
            metadata = os.fstat(descriptor)
            total_bytes += metadata.st_size
            if total_bytes > MAX_GIT_SHARED_INDEX_TOTAL_BYTES:
                raise SourceIdentityError(
                    "Git split-index payload exceeds the aggregate byte bound"
                )
            shared_indexes.append(
                _BoundSharedIndex(
                    parent_descriptor=parent_descriptor,
                    name=name,
                    descriptor=descriptor,
                    binding_signature=_shared_index_binding_signature(metadata),
                    content_sha256=_descriptor_content_sha256(
                        descriptor,
                        expected_size=metadata.st_size,
                        label=f"Git split index {name}",
                    ),
                )
            )
        except BaseException:
            os.close(descriptor)
            raise
    return total_bytes


def _open_child_directory(
    parent_descriptor: int,
    parent_path: Path,
    name: str,
    label: str,
) -> _BoundDirectory:
    descriptor = -1
    parent_copy = -1
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
            dir_fd=parent_descriptor,
        )
        parent_copy = os.dup(parent_descriptor)
    except OSError as error:
        if descriptor >= 0:
            os.close(descriptor)
        if parent_copy >= 0:
            os.close(parent_copy)
        raise SourceIdentityError(f"cannot bind {label}: {error}") from error
    binding = _BoundDirectory(parent_path / name, [parent_copy, descriptor], [name])
    try:
        binding.assert_current(label)
    except BaseException:
        binding.close()
        raise
    return binding


def _decode_git_pointer(payload: bytes, prefix: bytes, label: str) -> str:
    if len(payload) > MAX_GIT_CONTROL_FILE_BYTES or b"\0" in payload:
        raise SourceIdentityError(f"{label} is invalid")
    line = payload.rstrip(b"\r\n")
    if not line.startswith(prefix) or not line[len(prefix) :].strip():
        raise SourceIdentityError(f"{label} is invalid")
    return os.fsdecode(line[len(prefix) :].strip())


@contextlib.contextmanager
def _bind_repository(repo_root: Path) -> Iterator[_RepositoryBinding]:
    root = _open_absolute_directory(repo_root, "source repository")
    git_directory: _BoundDirectory | None = None
    common_directory: _BoundDirectory | None = None
    git_descriptor = -1
    common_descriptor = -1
    index_descriptor = -1
    control_files: list[
        tuple[int, str, int, tuple[int, int, int, int, int, int, int]]
    ] = []
    shared_indexes: list[_BoundSharedIndex] = []
    binding: _RepositoryBinding | None = None
    try:
        try:
            git_metadata = os.stat(
                ".git",
                dir_fd=root.descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            raise SourceIdentityError(
                f"source repository has no bound .git entry: {error}"
            ) from error
        if stat.S_ISDIR(git_metadata.st_mode):
            git_directory = _open_child_directory(
                root.descriptor,
                root.path,
                ".git",
                "Git directory",
            )
            git_descriptor = git_directory.descriptor
        elif stat.S_ISREG(git_metadata.st_mode):
            snapshot, marker_descriptor = _read_regular_at(
                root.descriptor,
                ".git",
                label="Git directory link",
                max_bytes=MAX_GIT_CONTROL_FILE_BYTES,
                keep_descriptor=True,
            )
            assert marker_descriptor is not None
            control_files.append(
                (
                    root.descriptor,
                    ".git",
                    marker_descriptor,
                    _metadata_signature(snapshot.metadata),
                )
            )
            raw_git_path = _decode_git_pointer(
                snapshot.payload,
                b"gitdir:",
                "Git directory link",
            )
            git_path = Path(raw_git_path)
            if not git_path.is_absolute():
                git_path = root.path / git_path
            git_directory = _open_absolute_directory(git_path, "Git directory")
            git_descriptor = git_directory.descriptor
        else:
            raise SourceIdentityError(".git is not a directory or Git directory link")

        try:
            common_metadata = os.stat(
                "commondir",
                dir_fd=git_descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            common_metadata = None
        except OSError as error:
            raise SourceIdentityError(f"cannot inspect Git common directory: {error}") from error
        if common_metadata is None:
            common_descriptor = git_descriptor
        elif stat.S_ISREG(common_metadata.st_mode):
            snapshot, marker_descriptor = _read_regular_at(
                git_descriptor,
                "commondir",
                label="Git common-directory link",
                max_bytes=MAX_GIT_CONTROL_FILE_BYTES,
                keep_descriptor=True,
            )
            assert marker_descriptor is not None
            control_files.append(
                (
                    git_descriptor,
                    "commondir",
                    marker_descriptor,
                    _metadata_signature(snapshot.metadata),
                )
            )
            raw_common_path = _decode_git_pointer(
                b"gitdir:" + snapshot.payload,
                b"gitdir:",
                "Git common-directory link",
            )
            common_path = Path(raw_common_path)
            if not common_path.is_absolute():
                common_path = git_directory.path / common_path
            common_directory = _open_absolute_directory(
                common_path,
                "Git common directory",
            )
            common_descriptor = common_directory.descriptor
        else:
            raise SourceIdentityError("Git commondir entry is not a regular file")

        retained_index, index_signature = _bind_regular_at(
            git_descriptor,
            "index",
            label="Git index",
            max_bytes=MAX_GIT_STDOUT_BYTES,
        )
        index_descriptor = retained_index
        shared_index_parents = {git_descriptor, common_descriptor}
        shared_index_total_bytes = 0
        for shared_index_parent in sorted(shared_index_parents):
            shared_index_total_bytes = _bind_shared_index_files(
                shared_index_parent,
                shared_indexes,
                total_bytes=shared_index_total_bytes,
            )
        binding = _RepositoryBinding(
            root=root,
            git_directory=git_directory,
            git_descriptor=git_descriptor,
            common_directory=common_directory,
            common_descriptor=common_descriptor,
            control_files=control_files,
            shared_indexes=shared_indexes,
            index_descriptor=index_descriptor,
            index_signature=index_signature,
        )
        binding.assert_current()
        yield binding
        binding.assert_current()
    finally:
        if binding is not None:
            binding.close()
        else:
            if index_descriptor >= 0:
                os.close(index_descriptor)
            for _, _, descriptor, _ in control_files:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            for shared_index in shared_indexes:
                try:
                    os.close(shared_index.descriptor)
                except OSError:
                    pass
            if common_directory is not None:
                common_directory.close()
            if git_directory is not None:
                git_directory.close()
            root.close()


def _kill_process(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        try:
            process.kill()
        except OSError:
            pass


def _run_bounded_process(
    argv: list[str],
    *,
    environment: dict[str, str],
    pass_fds: tuple[int, ...],
    stdout_limit: int,
    stderr_limit: int,
    timeout_seconds: int,
    working_directory: str | None = None,
) -> tuple[int, bytes, bytes]:
    try:
        process = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=working_directory,
            env=environment,
            pass_fds=pass_fds,
            start_new_session=True,
        )
    except OSError as error:
        raise SourceIdentityError(f"cannot execute Git: {error}") from error
    assert process.stdout is not None and process.stderr is not None
    output = {"stdout": bytearray(), "stderr": bytearray()}
    deadline = time.monotonic() + timeout_seconds
    selector: selectors.BaseSelector | None = None
    try:
        selector = selectors.DefaultSelector()
        selector.register(
            process.stdout,
            selectors.EVENT_READ,
            ("stdout", stdout_limit),
        )
        selector.register(
            process.stderr,
            selectors.EVENT_READ,
            ("stderr", stderr_limit),
        )
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SourceIdentityError("Git command exceeded the time bound")
            events = selector.select(remaining)
            if not events:
                raise SourceIdentityError("Git command exceeded the time bound")
            for key, _ in events:
                label, limit = key.data
                chunk = os.read(key.fd, 64 * 1024)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                output[label].extend(chunk)
                if len(output[label]) > limit:
                    raise SourceIdentityError(
                        f"Git {label} exceeded the source byte bound"
                    )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise SourceIdentityError("Git command exceeded the time bound")
        return_code = process.wait(timeout=remaining)
    except BaseException as error:
        _kill_process(process)
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        raise SourceIdentityError(
            "Git command failed its resource bounds"
        ) from error
    finally:
        if selector is not None:
            selector.close()
        process.stdout.close()
        process.stderr.close()
    return return_code, bytes(output["stdout"]), bytes(output["stderr"])


def _run_git_command(
    repository: _RepositoryBinding,
    *args: str,
    max_output_bytes: int = MAX_GIT_STDOUT_BYTES,
) -> tuple[int, bytes, bytes]:
    argv = ["git", "--no-pager"]
    for key, value in CANONICAL_GIT_CONFIG:
        argv.extend(("-c", f"{key}={value}"))
    argv.extend(args)
    try:
        return_code, stdout, stderr = _run_bounded_process(
            argv,
            environment=repository.environment,
            pass_fds=repository.pass_fds,
            stdout_limit=max_output_bytes,
            stderr_limit=MAX_GIT_STDERR_BYTES,
            timeout_seconds=GIT_TIMEOUT_SECONDS,
            working_directory=f"/proc/self/fd/{repository.root.descriptor}",
        )
    except SourceIdentityError as error:
        raise SourceIdentityError(
            f"git {' '.join(args)} failed for {repository.root.path}: {error}"
        ) from error
    return return_code, stdout, stderr


def _git_bytes(
    repository: _RepositoryBinding,
    *args: str,
    max_output_bytes: int = MAX_GIT_STDOUT_BYTES,
) -> bytes:
    return_code, stdout, stderr = _run_git_command(
        repository,
        *args,
        max_output_bytes=max_output_bytes,
    )
    if return_code != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()
        raise SourceIdentityError(
            f"git {' '.join(args)} failed for {repository.root.path}: {detail}"
        )
    return stdout


def _git_config_key_names_for_scope(
    repository: _RepositoryBinding,
    scope: str,
) -> tuple[bytes, ...]:
    return_code, stdout, stderr = _run_git_command(
        repository,
        "config",
        "--no-includes",
        scope,
        "--null",
        "--name-only",
        "--get-regexp",
        ".*",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    if return_code == 1:
        return ()
    if return_code != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()
        raise SourceIdentityError(
            f"cannot inspect {scope} Git configuration: {detail}"
        )
    return tuple(item for item in stdout.split(b"\0") if item)


def _worktree_configuration_is_enabled(
    repository: _RepositoryBinding,
) -> bool:
    """Return Git's effective local ``extensions.worktreeConfig`` boolean.

    ``git config --worktree`` aliases the local scope while this extension is
    disabled, but Git rejects that spelling once an ordinary linked worktree
    exists.  In the disabled case the already-inspected local scope is the
    complete effective scope, so it is both safer and more portable not to ask
    Git for the unavailable alias.
    """

    return_code, stdout, stderr = _run_git_command(
        repository,
        "config",
        "--no-includes",
        "--local",
        "--type=bool",
        "--get",
        "extensions.worktreeConfig",
        max_output_bytes=MAX_GIT_CONTROL_FILE_BYTES,
    )
    if return_code == 1:
        return False
    if return_code != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()
        raise SourceIdentityError(
            "cannot inspect extensions.worktreeConfig: " + detail
        )
    value = stdout.strip()
    if value == b"true":
        return True
    if value == b"false":
        return False
    raise SourceIdentityError(
        "extensions.worktreeConfig is not one exact Git boolean"
    )


def _assert_safe_git_configuration(repository: _RepositoryBinding) -> None:
    forbidden: list[bytes] = []
    raw_keys = set(_git_config_key_names_for_scope(repository, "--local"))
    if _worktree_configuration_is_enabled(repository):
        raw_keys.update(
            _git_config_key_names_for_scope(repository, "--worktree")
        )
    for raw_key in sorted(raw_keys):
        key = raw_key.lower()
        if (
            key == b"include.path"
            or (key.startswith(b"includeif.") and key.endswith(b".path"))
            or key in {
                b"core.attributesfile",
                b"core.alternaterefscommand",
                b"core.excludesfile",
                b"core.fsmonitor",
                b"core.sparsecheckout",
                b"core.sparsecheckoutcone",
                b"core.worktree",
                b"extensions.partialclone",
                b"index.sparse",
            }
            or key.startswith(b"attr.")
            or key.startswith(b"diff.")
            or key.startswith(b"filter.")
            or key.startswith(b"promisor.")
            or (
                key.startswith(b"remote.")
                and key.endswith((b".promisor", b".partialclonefilter"))
            )
            or key.startswith(b"submodule.")
        ):
            forbidden.append(raw_key)
    if forbidden:
        rendered = ", ".join(
            item.decode("utf-8", errors="backslashreplace")
            for item in sorted(forbidden)
        )
        raise SourceIdentityError(
            "effective local Git configuration contains a private "
            f"source-shaping, executable, or external input: {rendered}"
        )


def _git_text(repository: _RepositoryBinding, *args: str) -> str:
    try:
        return _git_bytes(
            repository,
            *args,
            max_output_bytes=MAX_GIT_CONTROL_FILE_BYTES,
        ).decode("utf-8").strip()
    except UnicodeDecodeError as error:
        raise SourceIdentityError("Git control output is not UTF-8") from error


def _relative_source_parent(
    root_descriptor: int,
    encoded_name: bytes,
) -> tuple[_BoundDirectory, str]:
    if (
        not encoded_name
        or len(encoded_name) > MAX_SOURCE_PATH_BYTES
        or encoded_name.startswith(b"/")
        or b"\0" in encoded_name
    ):
        raise SourceIdentityError("Git returned an invalid source path")
    encoded_parts = encoded_name.split(b"/")
    if any(part in (b"", b".", b"..") for part in encoded_parts):
        raise SourceIdentityError("Git returned a non-canonical source path")
    parts = [os.fsdecode(part) for part in encoded_parts]
    descriptors = [os.dup(root_descriptor)]
    names: list[str] = []
    try:
        for part in parts[:-1]:
            descriptor = os.open(
                part,
                os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=descriptors[-1],
            )
            metadata = os.fstat(descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                os.close(descriptor)
                raise SourceIdentityError("source path parent is not a directory")
            descriptors.append(descriptor)
            names.append(part)
        binding = _BoundDirectory(Path("."), descriptors, names)
        binding.assert_current("source path")
        return binding, parts[-1]
    except OSError as error:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise SourceIdentityError(
            f"cannot traverse source path without symbolic links: {error}"
        ) from error
    except BaseException:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise


def _optional_git_regular_snapshot(
    root_descriptor: int,
    encoded_name: bytes,
    *,
    label: str,
    reject_active_lines: bool = False,
    reject_nonempty: bool = False,
) -> bytes:
    if (
        not encoded_name
        or len(encoded_name) > MAX_SOURCE_PATH_BYTES
        or encoded_name.startswith(b"/")
        or b"\0" in encoded_name
    ):
        raise SourceIdentityError(f"{label} path is invalid")
    encoded_parts = encoded_name.split(b"/")
    if any(part in (b"", b".", b"..") for part in encoded_parts):
        raise SourceIdentityError(f"{label} path is non-canonical")
    parts = [os.fsdecode(part) for part in encoded_parts]
    descriptors = [os.dup(root_descriptor)]
    names: list[str] = []
    try:
        for part in parts[:-1]:
            try:
                descriptor = os.open(
                    part,
                    os.O_RDONLY
                    | os.O_CLOEXEC
                    | os.O_DIRECTORY
                    | os.O_NOFOLLOW,
                    dir_fd=descriptors[-1],
                )
            except FileNotFoundError:
                return b"missing\0"
            except OSError as error:
                raise SourceIdentityError(
                    f"cannot bind {label} without symbolic-link traversal: {error}"
                ) from error
            descriptors.append(descriptor)
            names.append(part)
        binding = _BoundDirectory(Path("."), descriptors, names)
        binding.assert_current(label)
        try:
            metadata = os.stat(
                parts[-1],
                dir_fd=binding.descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            return b"missing\0"
        except OSError as error:
            raise SourceIdentityError(f"cannot inspect {label}: {error}") from error
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise SourceIdentityError(
                f"{label} must be one non-hardlinked regular file"
            )
        snapshot, _ = _read_regular_at(
            binding.descriptor,
            parts[-1],
            label=label,
            max_bytes=MAX_GIT_LIST_BYTES,
        )
        if reject_active_lines:
            for line in snapshot.payload.split(b"\n"):
                if line.endswith(b"\r"):
                    line = line[:-1]
                if line != b"" and not line.startswith(b"#"):
                    raise SourceIdentityError(
                        f"{label} contains an active private source rule"
                    )
        if reject_nonempty and snapshot.payload != b"":
            raise SourceIdentityError(f"{label} must be a zero-byte file")
        signature = repr(_metadata_signature(snapshot.metadata)).encode("ascii")
        binding.assert_current(label)
        return (
            b"present\0"
            + len(signature).to_bytes(8, "big")
            + signature
            + len(snapshot.payload).to_bytes(8, "big")
            + snapshot.payload
        )
    finally:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass


def _git_local_inputs_sha256(repository: _RepositoryBinding) -> str:
    _assert_safe_git_configuration(repository)
    effective_config = _git_bytes(
        repository,
        "config",
        "--no-includes",
        "--null",
        "--list",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    inputs = (
        (
            b"git-head",
            _optional_git_regular_snapshot(
                repository.git_descriptor,
                b"HEAD",
                label="Git HEAD control file",
            ),
        ),
        (
            b"git-worktree-config",
            _optional_git_regular_snapshot(
                repository.git_descriptor,
                b"config.worktree",
                label="Git worktree configuration",
            ),
        ),
        (
            b"git-common-config",
            _optional_git_regular_snapshot(
                repository.common_descriptor,
                b"config",
                label="Git common configuration",
            ),
        ),
        (
            b"git-packed-refs",
            _optional_git_regular_snapshot(
                repository.common_descriptor,
                b"packed-refs",
                label="Git packed refs",
            ),
        ),
        (
            b"git-worktree-info-exclude",
            _optional_git_regular_snapshot(
                repository.git_descriptor,
                b"info/exclude",
                label="Git worktree exclude configuration",
                reject_active_lines=True,
            ),
        ),
        (
            b"git-common-info-exclude",
            _optional_git_regular_snapshot(
                repository.common_descriptor,
                b"info/exclude",
                label="Git common exclude configuration",
                reject_active_lines=True,
            ),
        ),
        (
            b"git-worktree-info-attributes",
            _optional_git_regular_snapshot(
                repository.git_descriptor,
                b"info/attributes",
                label="Git worktree attributes configuration",
                reject_active_lines=True,
            ),
        ),
        (
            b"git-common-info-attributes",
            _optional_git_regular_snapshot(
                repository.common_descriptor,
                b"info/attributes",
                label="Git common attributes configuration",
                reject_active_lines=True,
            ),
        ),
        (
            b"git-worktree-object-alternates",
            _optional_git_regular_snapshot(
                repository.git_descriptor,
                b"objects/info/alternates",
                label="Git worktree object alternates configuration",
                reject_nonempty=True,
            ),
        ),
        (
            b"git-common-object-alternates",
            _optional_git_regular_snapshot(
                repository.common_descriptor,
                b"objects/info/alternates",
                label="Git common object alternates configuration",
                reject_nonempty=True,
            ),
        ),
    )
    local_input_bytes = len(effective_config) + sum(
        len(payload) for _, payload in inputs
    )
    if local_input_bytes > MAX_GIT_LOCAL_INPUT_TOTAL_BYTES:
        raise SourceIdentityError(
            "Git local control inputs exceed the aggregate byte bound"
        )
    digest = hashlib.sha256()
    digest.update(b"manyforge.source-identity.git-local-inputs.v1\0")
    digest.update(len(effective_config).to_bytes(8, "big"))
    digest.update(effective_config)
    for name, payload in inputs:
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def _hash_untracked_files(
    repository: _RepositoryBinding,
    digest: Any,
    untracked_output: bytes,
    *,
    excluded_path: str | None,
) -> tuple[str, list[_UntrackedSignature]]:
    section_digest = hashlib.sha256()

    def update(payload: bytes) -> None:
        digest.update(payload)
        section_digest.update(payload)

    excluded = os.fsencode(excluded_path) if excluded_path is not None else None
    untracked = sorted(
        name
        for name in untracked_output.split(b"\0")
        if name and name != excluded
    )
    if len(untracked) > MAX_SOURCE_FILES:
        raise SourceIdentityError("untracked source inventory exceeds the file bound")
    update(b"untracked-files\0")
    update(len(untracked).to_bytes(8, "big"))
    total_bytes = 0
    symlink_bytes = 0
    seen_identities: set[tuple[int, int]] = set()
    signatures: list[_UntrackedSignature] = []
    for encoded_name in untracked:
        parent, name = _relative_source_parent(
            repository.root.descriptor,
            encoded_name,
        )
        try:
            metadata = os.stat(
                name,
                dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
            if metadata.st_nlink != 1:
                raise SourceIdentityError(
                    f"{os.fsdecode(encoded_name)}: untracked hardlinks are unsupported"
                )
            identity = (metadata.st_dev, metadata.st_ino)
            if identity in seen_identities:
                raise SourceIdentityError("untracked source files alias one inode")
            seen_identities.add(identity)
            mode = stat.S_IMODE(metadata.st_mode)
            update(len(encoded_name).to_bytes(8, "big"))
            update(encoded_name)
            update(mode.to_bytes(4, "big"))
            if stat.S_ISLNK(metadata.st_mode):
                payload = os.fsencode(os.readlink(name, dir_fd=parent.descriptor))
                if len(payload) > MAX_SOURCE_PATH_BYTES:
                    raise SourceIdentityError("untracked symlink target exceeds the path bound")
                after = os.stat(
                    name,
                    dir_fd=parent.descriptor,
                    follow_symlinks=False,
                )
                if _metadata_signature(after) != _metadata_signature(metadata):
                    raise SourceIdentityError("untracked symlink changed while being read")
                update(b"symlink\0")
                update(len(payload).to_bytes(8, "big"))
                update(payload)
                total_bytes += len(payload)
                symlink_bytes += len(payload)
                if symlink_bytes > MAX_SYMLINK_TOTAL_BYTES:
                    raise SourceIdentityError(
                        "untracked symlink targets exceed the aggregate byte bound"
                    )
            elif stat.S_ISREG(metadata.st_mode):
                if metadata.st_size < 0 or metadata.st_size > MAX_UNTRACKED_FILE_BYTES:
                    raise SourceIdentityError("untracked source file exceeds the byte bound")
                total_bytes += metadata.st_size
                descriptor = os.open(
                    name,
                    os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=parent.descriptor,
                )
                try:
                    opened = os.fstat(descriptor)
                    if _metadata_signature(opened) != _metadata_signature(metadata):
                        raise SourceIdentityError(
                            "untracked source file changed before it was opened"
                        )
                    update(b"regular\0")
                    update(metadata.st_size.to_bytes(8, "big"))
                    remaining = metadata.st_size
                    while remaining:
                        payload = os.read(descriptor, min(1024 * 1024, remaining))
                        if not payload:
                            raise SourceIdentityError(
                                "untracked source file ended before its recorded size"
                            )
                        update(payload)
                        remaining -= len(payload)
                    if os.read(descriptor, 1):
                        raise SourceIdentityError(
                            "untracked source file grew while being read"
                        )
                    after = os.fstat(descriptor)
                    if _metadata_signature(after) != _metadata_signature(metadata):
                        raise SourceIdentityError(
                            "untracked source file changed while being read"
                        )
                finally:
                    os.close(descriptor)
            else:
                raise SourceIdentityError(
                    f"{os.fsdecode(encoded_name)}: unsupported untracked source file type"
                )
            if total_bytes > MAX_UNTRACKED_TOTAL_BYTES:
                raise SourceIdentityError(
                    "untracked source payload exceeds the aggregate byte bound"
                )
            parent.assert_current("source path")
            if not _path_has_identity(
                parent.descriptor,
                name,
                identity,
                regular=stat.S_ISREG(metadata.st_mode),
            ):
                raise SourceIdentityError("untracked source path changed while hashing")
            signatures.append(
                _UntrackedSignature(
                    encoded_name=encoded_name,
                    metadata_signature=_metadata_signature(metadata),
                    regular=stat.S_ISREG(metadata.st_mode),
                )
            )
        finally:
            parent.close()
    return section_digest.hexdigest(), signatures


def _revalidate_untracked_signatures(
    repository: _RepositoryBinding,
    signatures: list[_UntrackedSignature],
) -> None:
    for signature in signatures:
        parent, name = _relative_source_parent(
            repository.root.descriptor,
            signature.encoded_name,
        )
        try:
            try:
                metadata = os.stat(
                    name,
                    dir_fd=parent.descriptor,
                    follow_symlinks=False,
                )
            except OSError as error:
                raise SourceIdentityError(
                    "untracked source path changed during final revalidation"
                ) from error
            if (
                _metadata_signature(metadata) != signature.metadata_signature
                or not _path_has_identity(
                    parent.descriptor,
                    name,
                    (metadata.st_dev, metadata.st_ino),
                    regular=signature.regular,
                )
            ):
                raise SourceIdentityError(
                    "untracked source changed during final revalidation"
                )
            parent.assert_current("source path")
        finally:
            parent.close()


def _tracked_entry_signature(
    root_descriptor: int,
    encoded_name: bytes,
    expected_mode: bytes,
) -> bytes:
    if (
        not encoded_name
        or len(encoded_name) > MAX_SOURCE_PATH_BYTES
        or encoded_name.startswith(b"/")
        or b"\0" in encoded_name
    ):
        raise SourceIdentityError("Git returned an invalid tracked source path")
    encoded_parts = encoded_name.split(b"/")
    if any(part in (b"", b".", b"..") for part in encoded_parts):
        raise SourceIdentityError("Git returned a non-canonical tracked source path")
    if expected_mode == b"160000":
        raise SourceIdentityError("tracked Git links are unsupported source inputs")
    parts = [os.fsdecode(part) for part in encoded_parts]
    descriptors = [os.dup(root_descriptor)]
    names: list[str] = []
    try:
        for part in parts[:-1]:
            try:
                descriptor = os.open(
                    part,
                    os.O_RDONLY
                    | os.O_CLOEXEC
                    | os.O_DIRECTORY
                    | os.O_NOFOLLOW,
                    dir_fd=descriptors[-1],
                )
            except FileNotFoundError:
                return b"missing"
            except OSError as error:
                raise SourceIdentityError(
                    "tracked source path traverses a symbolic link or non-directory"
                ) from error
            descriptors.append(descriptor)
            names.append(part)
        try:
            metadata = os.stat(
                parts[-1],
                dir_fd=descriptors[-1],
                follow_symlinks=False,
            )
        except FileNotFoundError:
            return b"missing"
        signature = repr(_metadata_signature(metadata)).encode("ascii")
        if stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1:
            kind = b"regular\0"
            payload = b""
        elif stat.S_ISLNK(metadata.st_mode) and metadata.st_nlink == 1:
            kind = b"symlink\0"
            payload = os.fsencode(
                os.readlink(parts[-1], dir_fd=descriptors[-1])
            )
            if len(payload) > MAX_SOURCE_PATH_BYTES:
                raise SourceIdentityError("tracked symlink target exceeds the path bound")
        else:
            raise SourceIdentityError(
                "tracked source is hardlinked or has an unsupported file type"
            )
        binding = _BoundDirectory(Path("."), descriptors, names)
        binding.assert_current("tracked source path")
        after = os.stat(
            parts[-1],
            dir_fd=descriptors[-1],
            follow_symlinks=False,
        )
        if _metadata_signature(after) != _metadata_signature(metadata):
            raise SourceIdentityError("tracked source changed during traversal")
        return kind + signature + b"\0" + payload
    finally:
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError:
                pass


def _tagged_tracked_inventory(
    repository: _RepositoryBinding,
    *,
    flag: str,
    label: str,
) -> list[bytes]:
    output = _git_bytes(
        repository,
        "ls-files",
        "--cached",
        "--full-name",
        flag,
        "-z",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    records = [record for record in output.split(b"\0") if record]
    if len(records) > MAX_SOURCE_FILES:
        raise SourceIdentityError(
            f"{label} tracked source inventory exceeds the file bound"
        )
    names: list[bytes] = []
    seen: set[bytes] = set()
    for record in records:
        if len(record) < 3 or record[:2] != b"H ":
            raise SourceIdentityError(
                f"{label} tracked source flags are not canonical uppercase H"
            )
        encoded_name = record[2:]
        if encoded_name in seen:
            raise SourceIdentityError(f"{label} tracked source inventory is duplicated")
        seen.add(encoded_name)
        names.append(encoded_name)
    return names


def _tracked_shape_sha256(repository: _RepositoryBinding) -> str:
    output = _git_bytes(
        repository,
        "ls-files",
        "--cached",
        "--stage",
        "-z",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    records = [record for record in output.split(b"\0") if record]
    if len(records) > MAX_SOURCE_FILES:
        raise SourceIdentityError("tracked source inventory exceeds the file bound")
    digest = hashlib.sha256()
    digest.update(b"tracked-source-shapes\0")
    digest.update(len(records).to_bytes(8, "big"))
    seen: set[bytes] = set()
    stage_zero_names: list[bytes] = []
    symlink_bytes = 0
    for record in records:
        try:
            header, encoded_name = record.split(b"\t", 1)
            mode, object_id, stage = header.split(b" ")
        except ValueError as error:
            raise SourceIdentityError("Git returned an invalid tracked source record") from error
        if (
            stage != b"0"
            or len(object_id) not in (40, 64)
            or encoded_name in seen
        ):
            raise SourceIdentityError(
                "tracked source inventory is unmerged, duplicated, or invalid"
            )
        seen.add(encoded_name)
        stage_zero_names.append(encoded_name)
        shape = _tracked_entry_signature(
            repository.root.descriptor,
            encoded_name,
            mode,
        )
        if shape.startswith(b"symlink\0"):
            symlink_bytes += len(shape.rsplit(b"\0", 1)[1])
            if symlink_bytes > MAX_SYMLINK_TOTAL_BYTES:
                raise SourceIdentityError(
                    "tracked symlink targets exceed the aggregate byte bound"
                )
        digest.update(len(record).to_bytes(8, "big"))
        digest.update(record)
        digest.update(len(shape).to_bytes(8, "big"))
        digest.update(shape)
    assume_unchanged_names = _tagged_tracked_inventory(
        repository,
        flag="-v",
        label="assume-unchanged/skip-worktree",
    )
    # FSMonitor validity is an index cache, not a source input.  Every Git
    # subprocess above is command-scoped to ``core.fsmonitor=false`` and local
    # ``core.fsmonitor`` configuration is rejected, so Git discards any FSMN
    # cache before it can suppress a stat or diff.  Keep the independent ``-f``
    # inventory check to prove that effective, neutralized view is canonical;
    # unlike assume-unchanged and skip-worktree, raw FSMonitor cache state need
    # not make two otherwise identical source trees have different identities.
    fsmonitor_names = _tagged_tracked_inventory(
        repository,
        flag="-f",
        label="fsmonitor-valid/skip-worktree",
    )
    if assume_unchanged_names != stage_zero_names:
        raise SourceIdentityError(
            "assume-unchanged tracked inventory differs from stage zero"
        )
    if fsmonitor_names != stage_zero_names:
        raise SourceIdentityError(
            "fsmonitor-valid tracked inventory differs from stage zero"
        )
    return digest.hexdigest()


def _worktree_delta_sha256(
    repository: _RepositoryBinding,
    *,
    base_commit: str,
    excluded_path: str | None,
) -> str:
    _validate_git_sha(base_commit, "Git delta base")
    digest = hashlib.sha256()
    diff_args = [
        "diff",
        "--binary",
        "--full-index",
        "--no-ext-diff",
        "--no-textconv",
        "--no-color",
        "--no-color-moved",
        "--no-color-moved-ws",
        "--diff-algorithm=default",
        "--indent-heuristic",
        "--find-renames=50%",
        "-l1000",
        "--unified=3",
        "--inter-hunk-context=0",
        "--src-prefix=a/",
        "--dst-prefix=b/",
        "--line-prefix=",
        "--output-indicator-new=+",
        "--output-indicator-old=-",
        "--output-indicator-context= ",
        "--no-relative",
        "--ignore-submodules=dirty",
        "--submodule=short",
        base_commit,
        "--",
    ]
    if excluded_path is not None:
        diff_args.extend([".", f":(exclude){excluded_path}"])
    tracked = _git_bytes(
        repository,
        *diff_args,
        max_output_bytes=MAX_GIT_STDOUT_BYTES,
    )
    digest.update(b"tracked-diff\0")
    digest.update(len(tracked).to_bytes(8, "big"))
    digest.update(tracked)
    untracked_output = _git_bytes(
        repository,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    first_untracked_sha256, _ = _hash_untracked_files(
        repository,
        digest,
        untracked_output,
        excluded_path=excluded_path,
    )
    middle_untracked_output = _git_bytes(
        repository,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    if middle_untracked_output != untracked_output:
        raise SourceIdentityError(
            "untracked source inventory changed while collecting source identity"
        )
    verification_digest = hashlib.sha256()
    second_untracked_sha256, final_signatures = _hash_untracked_files(
        repository,
        verification_digest,
        middle_untracked_output,
        excluded_path=excluded_path,
    )
    final_untracked_output = _git_bytes(
        repository,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        max_output_bytes=MAX_GIT_LIST_BYTES,
    )
    if final_untracked_output != untracked_output:
        raise SourceIdentityError(
            "untracked source inventory changed during final revalidation"
        )
    _revalidate_untracked_signatures(repository, final_signatures)
    if second_untracked_sha256 != first_untracked_sha256:
        raise SourceIdentityError(
            "untracked source content changed while collecting source identity"
        )
    return digest.hexdigest()


def worktree_delta_sha256(repo_root: Path) -> str:
    """Fingerprint every tracked and non-ignored untracked delta from HEAD."""

    with _bind_repository(repo_root) as repository:
        _, _, delta_sha256 = _collect_stable_delta(
            repository,
            excluded_path=None,
            label="source",
        )
        return delta_sha256


def source_fingerprint_sha256(
    *, head_commit: str, head_tree: str, dirty: bool, delta_sha256: str
) -> str:
    """Frame and hash the complete v1 source identity deterministically."""

    digest = hashlib.sha256()
    digest.update(b"manyforge.source_identity.v1\0")
    fields = (
        (b"head_commit", head_commit.encode("ascii")),
        (b"head_tree", head_tree.encode("ascii")),
        (b"dirty", b"true" if dirty else b"false"),
        (b"delta_sha256", delta_sha256.encode("ascii")),
    )
    for name, payload in fields:
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def specs_contract_delta_sha256(repo_root: Path) -> str:
    """Return the live-authority Specs delta with one exact evidence exclusion."""

    with _bind_repository(repo_root) as repository:
        _, _, delta_sha256 = _collect_stable_delta(
            repository,
            excluded_path=SPECS_CONTRACT_EXCLUDED_PATH,
            label="Specs contract source",
        )
        return delta_sha256


def empty_delta_sha256() -> str:
    digest = hashlib.sha256()
    digest.update(b"tracked-diff\0")
    digest.update((0).to_bytes(8, "big"))
    digest.update(b"untracked-files\0")
    digest.update((0).to_bytes(8, "big"))
    return digest.hexdigest()


def specs_contract_fingerprint_sha256(
    *,
    head_commit: str,
    head_tree: str,
    excluded_paths: list[str],
    contract_dirty: bool,
    contract_delta_sha256: str,
) -> str:
    digest = hashlib.sha256()
    digest.update(b"manyforge.specs_contract_identity.v1\0")
    fields = (
        (b"head_commit", head_commit.encode("ascii")),
        (b"head_tree", head_tree.encode("ascii")),
        (
            b"excluded_paths",
            json.dumps(excluded_paths, separators=(",", ":")).encode("utf-8"),
        ),
        (b"contract_dirty", b"true" if contract_dirty else b"false"),
        (b"contract_delta_sha256", contract_delta_sha256.encode("ascii")),
    )
    for name, payload in fields:
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def _collect_specs_contract_identity(
    repository: _RepositoryBinding,
    source: dict[str, Any],
) -> dict[str, Any]:
    contract_head, contract_tree, delta = _collect_stable_delta(
        repository,
        excluded_path=SPECS_CONTRACT_EXCLUDED_PATH,
        label="Specs contract source",
    )
    if (
        contract_head != source["head_commit"]
        or contract_tree != source["head_tree"]
    ):
        raise SourceIdentityError(
            "Specs HEAD/tree changed while collecting its contract identity"
        )
    excluded_paths = [SPECS_CONTRACT_EXCLUDED_PATH]
    dirty = delta != empty_delta_sha256()
    return {
        "schema_version": SPECS_CONTRACT_SCHEMA,
        "head_commit": source["head_commit"],
        "head_tree": source["head_tree"],
        "excluded_paths": excluded_paths,
        "contract_dirty": dirty,
        "contract_delta_sha256": delta,
        "contract_fingerprint_sha256": specs_contract_fingerprint_sha256(
            head_commit=source["head_commit"],
            head_tree=source["head_tree"],
            excluded_paths=excluded_paths,
            contract_dirty=dirty,
            contract_delta_sha256=delta,
        ),
    }


def _collect_bracketed_source_and_specs_contract_identities(
    repository: _RepositoryBinding,
) -> tuple[dict[str, Any], dict[str, Any]]:
    source_before = _collect_source_identity(repository)
    contract = _collect_specs_contract_identity(repository, source_before)
    source_after = _collect_source_identity(repository)
    if source_after != source_before:
        raise SourceIdentityError(
            "full source identity changed while collecting Specs contract identity"
        )
    return source_before, contract


def source_and_specs_contract_identities(
    repo_root: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Collect one bracketed source/Specs pair under one repository binding."""

    with _bind_repository(repo_root) as repository:
        return _collect_bracketed_source_and_specs_contract_identities(repository)


def specs_contract_identity(repo_root: Path) -> dict[str, Any]:
    with _bind_repository(repo_root) as repository:
        _, contract = _collect_bracketed_source_and_specs_contract_identities(
            repository
        )
        return contract


def validate_specs_contract_identity(value: Any) -> dict[str, Any]:
    fields = {
        "schema_version",
        "head_commit",
        "head_tree",
        "excluded_paths",
        "contract_dirty",
        "contract_delta_sha256",
        "contract_fingerprint_sha256",
    }
    if not isinstance(value, dict) or set(value) != fields:
        raise SourceIdentityError(
            "Specs contract identity fields differ from the exact v1 contract"
        )
    if value.get("schema_version") != SPECS_CONTRACT_SCHEMA:
        raise SourceIdentityError("Specs contract identity schema is invalid")
    if value.get("excluded_paths") != [SPECS_CONTRACT_EXCLUDED_PATH]:
        raise SourceIdentityError("Specs contract exclusion is not the exact singleton")
    if not isinstance(value.get("contract_dirty"), bool):
        raise SourceIdentityError("Specs contract dirty state must be boolean")
    for name, length in (("head_commit", 40), ("head_tree", 40)):
        field = value.get(name)
        if (
            not isinstance(field, str)
            or len(field) != length
            or any(character not in "0123456789abcdef" for character in field)
        ):
            raise SourceIdentityError(f"Specs contract {name} is not a Git SHA")
    for name in ("contract_delta_sha256", "contract_fingerprint_sha256"):
        field = value.get(name)
        if (
            not isinstance(field, str)
            or len(field) != 64
            or any(character not in "0123456789abcdef" for character in field)
        ):
            raise SourceIdentityError(f"Specs contract {name} is not a SHA-256")
    calculated = specs_contract_fingerprint_sha256(
        head_commit=value["head_commit"],
        head_tree=value["head_tree"],
        excluded_paths=value["excluded_paths"],
        contract_dirty=value["contract_dirty"],
        contract_delta_sha256=value["contract_delta_sha256"],
    )
    if value["contract_fingerprint_sha256"] != calculated:
        raise SourceIdentityError("Specs contract fingerprint is inconsistent")
    return value


def _validate_git_sha(value: str, label: str) -> str:
    if (
        len(value) != 40
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise SourceIdentityError(f"{label} is not one full lowercase Git SHA")
    return value


def _collect_stable_delta(
    repository: _RepositoryBinding,
    *,
    excluded_path: str | None,
    label: str,
) -> tuple[str, str, str]:
    local_inputs_before = _git_local_inputs_sha256(repository)
    try:
        head_commit = _validate_git_sha(
            _git_text(repository, "rev-parse", "--verify", "HEAD"),
            f"{label} HEAD",
        )
        peeled_commit = _validate_git_sha(
            _git_text(
                repository,
                "rev-parse",
                "--verify",
                f"{head_commit}^{{commit}}",
            ),
            f"{label} HEAD commit",
        )
    except SourceIdentityError as error:
        raise SourceIdentityError(f"{label} HEAD is not exactly a commit") from error
    if peeled_commit != head_commit:
        raise SourceIdentityError(f"{label} HEAD is not exactly a commit")
    head_tree = _validate_git_sha(
        _git_text(
            repository,
            "rev-parse",
            "--verify",
            f"{head_commit}^{{tree}}",
        ),
        f"{label} HEAD tree",
    )
    tracked_before = _tracked_shape_sha256(repository)
    delta_sha256 = _worktree_delta_sha256(
        repository,
        base_commit=head_commit,
        excluded_path=excluded_path,
    )
    if _tracked_shape_sha256(repository) != tracked_before:
        raise SourceIdentityError(
            f"{label} tracked paths changed while collecting source identity"
        )
    if _git_local_inputs_sha256(repository) != local_inputs_before:
        raise SourceIdentityError(
            f"{label} local Git inputs changed while collecting source identity"
        )
    if _git_text(repository, "rev-parse", "--verify", "HEAD") != head_commit:
        raise SourceIdentityError(
            f"{label} HEAD changed while collecting source identity"
        )
    repository.assert_current()
    return head_commit, head_tree, delta_sha256


def _collect_source_identity(repository: _RepositoryBinding) -> dict[str, Any]:
    head_commit, head_tree, delta_sha256 = _collect_stable_delta(
        repository,
        excluded_path=None,
        label="source",
    )
    dirty = delta_sha256 != empty_delta_sha256()
    return {
        "schema_version": SCHEMA,
        "head_commit": head_commit,
        "head_tree": head_tree,
        "dirty": dirty,
        "delta_sha256": delta_sha256,
        "fingerprint_sha256": source_fingerprint_sha256(
            head_commit=head_commit,
            head_tree=head_tree,
            dirty=dirty,
            delta_sha256=delta_sha256,
        ),
    }


def source_identity(repo_root: Path) -> dict[str, Any]:
    """Collect one canonical v1 identity for ``repo_root``."""

    with _bind_repository(repo_root) as repository:
        return _collect_source_identity(repository)


def validate_identity(value: Any) -> dict[str, Any]:
    """Validate and return one exact v1 identity object."""

    if not isinstance(value, dict):
        raise SourceIdentityError("source identity must be a JSON object")
    if set(value) != set(FIELDS):
        raise SourceIdentityError(
            "source identity fields differ from the exact v1 contract"
        )
    if value.get("schema_version") != SCHEMA:
        raise SourceIdentityError(f"source identity schema must be {SCHEMA}")
    for name, length in (("head_commit", 40), ("head_tree", 40)):
        field = value.get(name)
        if (
            not isinstance(field, str)
            or len(field) != length
            or any(character not in "0123456789abcdef" for character in field)
        ):
            raise SourceIdentityError(f"source identity {name} is not a full Git SHA")
    if not isinstance(value.get("dirty"), bool):
        raise SourceIdentityError("source identity dirty must be boolean")
    for name in ("delta_sha256", "fingerprint_sha256"):
        field = value.get(name)
        if (
            not isinstance(field, str)
            or len(field) != 64
            or any(character not in "0123456789abcdef" for character in field)
        ):
            raise SourceIdentityError(f"source identity {name} is not a SHA-256")
    calculated = source_fingerprint_sha256(
        head_commit=value["head_commit"],
        head_tree=value["head_tree"],
        dirty=value["dirty"],
        delta_sha256=value["delta_sha256"],
    )
    if value["fingerprint_sha256"] != calculated:
        raise SourceIdentityError("source identity fingerprint is inconsistent")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--include-specs-contract-identity",
        action="store_true",
        help="emit source and stable Step 18 Specs contract identities",
    )
    parser.add_argument(
        "--field",
        action="append",
        choices=FIELDS,
        help="print selected fields one per line instead of JSON",
    )
    parser.add_argument(
        "--expect-fingerprint-sha256",
        help="fail unless the observed v1 fingerprint equals this value",
    )
    parser.add_argument("--expect-contract-fingerprint-sha256")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.include_specs_contract_identity:
            identity, contract_identity = source_and_specs_contract_identities(
                args.repo_root
            )
        else:
            identity = source_identity(args.repo_root)
            contract_identity = None
    except SourceIdentityError as error:
        print(f"source identity failed: {error}", file=sys.stderr)
        return 2
    if (
        args.expect_fingerprint_sha256 is not None
        and identity["fingerprint_sha256"] != args.expect_fingerprint_sha256
    ):
        print(
            "source identity fingerprint mismatch: "
            f"expected={args.expect_fingerprint_sha256} "
            f"observed={identity['fingerprint_sha256']}",
            file=sys.stderr,
        )
        return 1
    if args.expect_contract_fingerprint_sha256 is not None:
        if contract_identity is None:
            print(
                "--expect-contract-fingerprint-sha256 requires "
                "--include-specs-contract-identity",
                file=sys.stderr,
            )
            return 2
        if (
            contract_identity["contract_fingerprint_sha256"]
            != args.expect_contract_fingerprint_sha256
        ):
            print(
                "Specs contract fingerprint mismatch: "
                f"expected={args.expect_contract_fingerprint_sha256} "
                f"observed={contract_identity['contract_fingerprint_sha256']}",
                file=sys.stderr,
            )
            return 1
    if args.field:
        if args.include_specs_contract_identity:
            print(
                "--field cannot be combined with "
                "--include-specs-contract-identity",
                file=sys.stderr,
            )
            return 2
        if args.output is not None:
            print("--output cannot be combined with --field", file=sys.stderr)
            return 2
        for name in args.field:
            value = identity[name]
            print("1" if value is True else "0" if value is False else value)
        return 0
    output_value = (
        {
            "source_identity": identity,
            "specs_contract_identity": contract_identity,
        }
        if contract_identity is not None
        else identity
    )
    rendered = json.dumps(
        output_value, indent=2, sort_keys=True, allow_nan=False
    ) + "\n"
    if args.output is None:
        print(rendered, end="")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
