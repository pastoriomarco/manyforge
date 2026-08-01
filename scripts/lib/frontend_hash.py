#!/usr/bin/env python3
"""Bounded fingerprints for the retained frontend build/cache helper.

Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
blob c9855c86150de04349e9ac33a9200b9b18bd0898.

This tool describes source and built bytes. It does not select or launch a
runtime. Output is JSON by default; ``--output-format nul`` is a fixed
key/value record for the shell helper and is never evaluated as shell code.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Iterable, Mapping


_SOURCE_EXCLUDES = {
    ".git",
    ".pytest_cache",
    ".vite",
    "dist",
    "dist-ssr",
    "node_modules",
}
_DEPENDENCY_FILES = ("package.json", "package-lock.json", "npm-shrinkwrap.json")
_MAX_PATH_BYTES = 4096
_MAX_DEPTH = 64
_MAX_NODES = 40_000
_MAX_FILES = 20_000
_MAX_FILE_BYTES = 64 * 1024 * 1024
_MAX_TOTAL_BYTES = 512 * 1024 * 1024
_MAX_GIT_STDOUT_BYTES = 32 * 1024 * 1024
_MAX_GIT_STDERR_BYTES = 1024 * 1024
_GIT_TIMEOUT_SECONDS = 30.0


class FrontendHashError(RuntimeError):
    """A bounded-input, filesystem, or subprocess contract failed."""


def _path_bytes(value: str) -> bytes:
    encoded = os.fsencode(value)
    if not encoded or len(encoded) > _MAX_PATH_BYTES or b"\0" in encoded:
        raise FrontendHashError("path is empty or exceeds the supported bound")
    return encoded


def _stop_process_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        # SIGKILL cannot make an uninterruptible kernel wait complete. Do not
        # turn that host condition into an unbounded helper wait.
        pass


def _run_bounded(
    argv: list[str],
    *,
    stdout_limit: int = _MAX_GIT_STDOUT_BYTES,
    check: bool = True,
    environment: Mapping[str, str] | None = None,
) -> tuple[int, bytes, bytes]:
    try:
        process = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            env=environment,
        )
    except OSError as exc:
        raise FrontendHashError(f"could not execute {argv[0]!r}: {exc}") from exc
    assert process.stdout is not None
    assert process.stderr is not None

    streams: dict[int, tuple[str, bytearray, int]] = {
        process.stdout.fileno(): ("stdout", bytearray(), stdout_limit),
        process.stderr.fileno(): ("stderr", bytearray(), _MAX_GIT_STDERR_BYTES),
    }
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    selector.register(process.stderr, selectors.EVENT_READ)
    deadline = time.monotonic() + _GIT_TIMEOUT_SECONDS
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _stop_process_group(process)
                raise FrontendHashError(f"{argv[0]!r} exceeded {_GIT_TIMEOUT_SECONDS:g}s")
            events = selector.select(min(remaining, 0.25))
            if not events:
                continue
            for key, _ in events:
                descriptor = key.fileobj.fileno()
                chunk = os.read(descriptor, 64 * 1024)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                name, content, limit = streams[descriptor]
                if len(content) + len(chunk) > limit:
                    _stop_process_group(process)
                    raise FrontendHashError(
                        f"{argv[0]!r} {name} exceeds {limit} bytes"
                    )
                content.extend(chunk)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            _stop_process_group(process)
            raise FrontendHashError(f"{argv[0]!r} exceeded {_GIT_TIMEOUT_SECONDS:g}s")
        return_code = process.wait(timeout=remaining)
    except subprocess.TimeoutExpired as exc:
        _stop_process_group(process)
        raise FrontendHashError(f"{argv[0]!r} exceeded {_GIT_TIMEOUT_SECONDS:g}s") from exc
    finally:
        selector.close()

    stdout = bytes(streams[process.stdout.fileno()][1])
    stderr = bytes(streams[process.stderr.fileno()][1])
    if check and return_code != 0:
        detail = stderr.decode("utf-8", "replace").strip()
        raise FrontendHashError(
            f"{argv[0]!r} failed with exit {return_code}"
            + (f": {detail}" if detail else "")
        )
    return return_code, stdout, stderr


def _git_invocation(
    arguments: Iterable[str], *, trusted_path: Path
) -> tuple[list[str], dict[str, str]]:
    resolved = trusted_path.resolve(strict=True)
    safe_directories = [resolved, *resolved.parents]
    argv = [
        "git",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.untrackedCache=false",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.attributesFile=/dev/null",
        "-c",
        "diff.external=",
    ]
    for directory in safe_directories:
        argv.extend(("-c", f"safe.directory={directory}"))
    argv.extend(arguments)
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
    }
    environment.update(
        {
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_PAGER": "cat",
            "GIT_TERMINAL_PROMPT": "0",
            "LC_ALL": "C",
        }
    )
    return argv, environment


def _git(
    *arguments: str,
    trusted_path: Path,
    stdout_limit: int = _MAX_GIT_STDOUT_BYTES,
    check: bool = True,
) -> tuple[int, bytes]:
    argv, environment = _git_invocation(arguments, trusted_path=trusted_path)
    return_code, stdout, _ = _run_bounded(
        argv,
        stdout_limit=stdout_limit,
        check=check,
        environment=environment,
    )
    return return_code, stdout


def _validate_git_symlink(relative: Path, target: bytes) -> None:
    target_text = os.fsdecode(target)
    _path_bytes(target_text)
    target_path = Path(target_text)
    if target_path.is_absolute():
        raise FrontendHashError(f"symlink escapes source root: {relative}")
    parts = list(relative.parent.parts)
    for part in target_path.parts:
        if part in {"", "."}:
            continue
        if part == "..":
            if not parts:
                raise FrontendHashError(f"symlink escapes source root: {relative}")
            parts.pop()
        else:
            parts.append(part)


def _git_snapshot_hash(
    root: Path, entries: list[tuple[Path, bytes, bytes, int]]
) -> str:
    ordered = sorted(entries, key=lambda entry: os.fsencode(entry[0].as_posix()))
    request = b"".join(object_id + b"\n" for _, _, object_id, _ in ordered)
    argv, environment = _git_invocation(
        ("-C", os.fspath(root), "cat-file", "--batch"),
        trusted_path=root,
    )
    output_limit = sum(size + 128 for _, _, _, size in ordered)

    with tempfile.TemporaryFile() as input_file, tempfile.TemporaryFile() as output_file, tempfile.TemporaryFile() as error_file:
        input_file.write(request)
        input_file.seek(0)
        try:
            process = subprocess.Popen(
                argv,
                stdin=input_file,
                stdout=output_file,
                stderr=error_file,
                start_new_session=True,
                env=environment,
            )
        except OSError as exc:
            raise FrontendHashError(f"could not execute 'git': {exc}") from exc

        deadline = time.monotonic() + _GIT_TIMEOUT_SECONDS
        while process.poll() is None:
            if time.monotonic() >= deadline:
                _stop_process_group(process)
                raise FrontendHashError(
                    f"'git' exceeded {_GIT_TIMEOUT_SECONDS:g}s while reading tree blobs"
                )
            if os.fstat(output_file.fileno()).st_size > output_limit:
                _stop_process_group(process)
                raise FrontendHashError("Git batch output exceeds its tree-derived bound")
            if os.fstat(error_file.fileno()).st_size > _MAX_GIT_STDERR_BYTES:
                _stop_process_group(process)
                raise FrontendHashError("Git batch stderr exceeds its supported bound")
            time.sleep(0.05)

        return_code = process.wait()
        output_size = os.fstat(output_file.fileno()).st_size
        error_size = os.fstat(error_file.fileno()).st_size
        if output_size > output_limit or error_size > _MAX_GIT_STDERR_BYTES:
            raise FrontendHashError("Git batch output exceeds its supported bound")
        if return_code != 0:
            error_file.seek(0)
            detail = error_file.read(_MAX_GIT_STDERR_BYTES).decode(
                "utf-8", "replace"
            ).strip()
            raise FrontendHashError(
                f"'git' failed with exit {return_code}"
                + (f": {detail}" if detail else "")
            )

        output_file.seek(0)
        hasher = hashlib.sha256()
        for relative, mode, expected_object_id, expected_size in ordered:
            header = output_file.readline(257)
            if len(header) > 256 or not header.endswith(b"\n"):
                raise FrontendHashError("malformed Git batch header")
            fields = header[:-1].split(b" ")
            if len(fields) != 3:
                raise FrontendHashError("malformed Git batch header")
            object_id, object_type, raw_size = fields
            try:
                size = int(raw_size)
            except ValueError as exc:
                raise FrontendHashError("malformed Git batch object size") from exc
            if (
                object_id != expected_object_id
                or object_type != b"blob"
                or size != expected_size
            ):
                raise FrontendHashError("Git batch object does not match tree inventory")

            hasher.update(_path_bytes(relative.as_posix()))
            hasher.update(b"\0")
            if mode == b"120000":
                if size > _MAX_PATH_BYTES:
                    raise FrontendHashError("Git symlink target exceeds the path bound")
                content = output_file.read(size)
                if len(content) != size:
                    raise FrontendHashError("truncated Git symlink blob")
                _validate_git_symlink(relative, content)
                hasher.update(b"symlink\0")
                hasher.update(content)
            elif mode in {b"100644", b"100755"}:
                hasher.update(b"file\0")
                remaining = size
                while remaining:
                    chunk = output_file.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise FrontendHashError("truncated Git file blob")
                    hasher.update(chunk)
                    remaining -= len(chunk)
            else:
                raise FrontendHashError("unsupported Git frontend file mode")
            if output_file.read(1) != b"\n":
                raise FrontendHashError("malformed Git batch record terminator")
            hasher.update(b"\0")
        if output_file.read(1):
            raise FrontendHashError("unexpected trailing Git batch output")
        return hasher.hexdigest()


def _safe_root(raw: Path) -> Path:
    _path_bytes(os.fspath(raw))
    try:
        resolved = raw.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise FrontendHashError(f"path cannot be resolved: {raw}") from exc
    _path_bytes(os.fspath(resolved))
    if not resolved.is_dir():
        raise FrontendHashError(f"path is not a directory: {resolved}")
    return resolved


def _require_trusted_git_directory(path: Path, role: str) -> None:
    try:
        metadata = path.stat()
    except OSError as exc:
        raise FrontendHashError(f"cannot inspect Git {role}: {exc}") from exc
    if not stat.S_ISDIR(metadata.st_mode):
        raise FrontendHashError(f"Git {role} is not a directory")
    if metadata.st_uid not in {os.geteuid(), 0}:
        raise FrontendHashError(f"Git {role} is not owned by the invoking user")
    if metadata.st_mode & stat.S_IWOTH:
        raise FrontendHashError(f"Git {role} is world-writable")


def _safe_relative_path(raw: bytes) -> Path:
    if not raw:
        raise FrontendHashError("empty relative path")
    if len(raw) > _MAX_PATH_BYTES or b"\0" in raw:
        raise FrontendHashError("relative path exceeds the supported bound")
    path = Path(os.fsdecode(raw))
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise FrontendHashError("non-canonical relative path")
    return path


def _walk_files(base: Path, *, excludes: set[str]) -> list[Path]:
    files: list[Path] = []
    nodes = 0
    stack: list[tuple[Path, Path, int]] = [(base, Path(), 0)]
    while stack:
        absolute, relative, depth = stack.pop()
        if depth > _MAX_DEPTH:
            raise FrontendHashError(f"filesystem depth exceeds {_MAX_DEPTH}")
        try:
            with os.scandir(absolute) as iterator:
                entries = []
                for entry in iterator:
                    nodes += 1
                    if nodes > _MAX_NODES:
                        raise FrontendHashError(
                            f"filesystem entry count exceeds {_MAX_NODES}"
                        )
                    entries.append(entry)
        except OSError as exc:
            raise FrontendHashError(f"cannot enumerate {absolute}: {exc}") from exc
        entries.sort(key=lambda entry: os.fsencode(entry.name))
        directories: list[tuple[Path, Path, int]] = []
        for entry in entries:
            if entry.name in excludes:
                continue
            child_relative = relative / entry.name
            _path_bytes(child_relative.as_posix())
            try:
                if entry.is_symlink():
                    files.append(child_relative)
                elif entry.is_dir(follow_symlinks=False):
                    directories.append((Path(entry.path), child_relative, depth + 1))
                elif entry.is_file(follow_symlinks=False):
                    files.append(child_relative)
                else:
                    raise FrontendHashError(f"unsupported filesystem entry: {child_relative}")
            except OSError as exc:
                raise FrontendHashError(f"cannot inspect {child_relative}: {exc}") from exc
            if len(files) > _MAX_FILES:
                raise FrontendHashError(f"file count exceeds {_MAX_FILES}")
        stack.extend(reversed(directories))
    return files


def _safe_symlink_target(base: Path, path: Path) -> bytes:
    try:
        target = os.readlink(path)
    except OSError as exc:
        raise FrontendHashError(f"cannot read symlink {path}: {exc}") from exc
    target_bytes = _path_bytes(target)
    try:
        resolved = path.resolve(strict=False)
        resolved.relative_to(base)
    except (OSError, RuntimeError, ValueError) as exc:
        raise FrontendHashError(f"symlink escapes source root: {path}") from exc
    return target_bytes


def _hash_regular_file(
    hasher: "hashlib._Hash", base: Path, relative: Path, total: list[int]
) -> None:
    path = base / relative
    try:
        before = path.lstat()
    except FileNotFoundError as exc:
        raise FrontendHashError(f"file disappeared while hashing: {relative}") from exc
    if stat.S_ISLNK(before.st_mode):
        target = _safe_symlink_target(base, path)
        total[0] += len(target)
        if total[0] > _MAX_TOTAL_BYTES:
            raise FrontendHashError(f"hashed byte total exceeds {_MAX_TOTAL_BYTES}")
        hasher.update(b"symlink\0")
        hasher.update(target)
        hasher.update(b"\0")
        return
    if not stat.S_ISREG(before.st_mode):
        raise FrontendHashError(f"unsupported filesystem entry: {relative}")
    if before.st_size > _MAX_FILE_BYTES:
        raise FrontendHashError(f"file exceeds {_MAX_FILE_BYTES} bytes: {relative}")

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise FrontendHashError(f"cannot open {relative}: {exc}") from exc
    read_size = 0
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (
            before.st_dev,
            before.st_ino,
        ):
            raise FrontendHashError(f"file identity changed while opening: {relative}")
        hasher.update(b"file\0")
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            read_size += len(chunk)
            if read_size > _MAX_FILE_BYTES:
                raise FrontendHashError(f"file grew beyond {_MAX_FILE_BYTES} bytes: {relative}")
            total[0] += len(chunk)
            if total[0] > _MAX_TOTAL_BYTES:
                raise FrontendHashError(f"hashed byte total exceeds {_MAX_TOTAL_BYTES}")
            hasher.update(chunk)
        after = os.fstat(descriptor)
        if read_size != opened.st_size or (
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        ) != (
            opened.st_size,
            opened.st_mtime_ns,
            opened.st_ctime_ns,
        ):
            raise FrontendHashError(f"file changed while hashing: {relative}")
    finally:
        os.close(descriptor)
    hasher.update(b"\0")


def _hash_files(base: Path, relatives: Iterable[Path]) -> tuple[str, int]:
    ordered = sorted(set(relatives), key=lambda path: os.fsencode(path.as_posix()))
    if len(ordered) > _MAX_FILES:
        raise FrontendHashError(f"file count exceeds {_MAX_FILES}")
    hasher = hashlib.sha256()
    total = [0]
    count = 0
    for relative in ordered:
        path_bytes = _path_bytes(relative.as_posix())
        path = base / relative
        if not path.exists() and not path.is_symlink():
            continue
        hasher.update(path_bytes)
        hasher.update(b"\0")
        _hash_regular_file(hasher, base, relative, total)
        count += 1
    return hasher.hexdigest(), count


def _git_context(source_dir: Path) -> tuple[Path, Path] | None:
    return_code, stdout = _git(
        "-C",
        os.fspath(source_dir),
        "rev-parse",
        "--show-toplevel",
        trusted_path=source_dir,
        check=False,
    )
    if return_code != 0:
        return None
    root_text = os.fsdecode(stdout.rstrip(b"\n"))
    root = _safe_root(Path(root_text))
    _, git_directory_output = _git(
        "-C",
        os.fspath(root),
        "rev-parse",
        "--absolute-git-dir",
        trusted_path=root,
    )
    git_directory = _safe_root(Path(os.fsdecode(git_directory_output.rstrip(b"\n"))))
    _require_trusted_git_directory(root, "worktree root")
    _require_trusted_git_directory(git_directory, "control directory")
    try:
        relative = source_dir.relative_to(root)
    except ValueError:
        return None
    _path_bytes(relative.as_posix())
    return root, relative


def _git_bytes(root: Path, object_name: str) -> bytes | None:
    return_code, _ = _git(
        "-C",
        os.fspath(root),
        "cat-file",
        "-e",
        object_name,
        trusted_path=root,
        check=False,
    )
    if return_code != 0:
        return None
    _, content = _git(
        "-C",
        os.fspath(root),
        "show",
        object_name,
        trusted_path=root,
        stdout_limit=_MAX_FILE_BYTES + 1,
    )
    if len(content) > _MAX_FILE_BYTES:
        raise FrontendHashError(f"Git blob exceeds {_MAX_FILE_BYTES} bytes")
    return content


def _git_tree_inventory(root: Path, tree_hash: str) -> tuple[int, str]:
    _, output = _git(
        "-C",
        os.fspath(root),
        "ls-tree",
        "-lrz",
        "-r",
        tree_hash,
        trusted_path=root,
    )
    count = 0
    included_count = 0
    total = 0
    included_entries: list[tuple[Path, bytes, bytes, int]] = []
    for record in output.split(b"\0"):
        if not record:
            continue
        try:
            metadata, raw_name = record.split(b"\t", 1)
            mode, object_type, object_id, raw_size = metadata.split(b" ", 3)
        except ValueError as exc:
            raise FrontendHashError("malformed Git tree inventory") from exc
        if object_type != b"blob" or len(object_id) != 40 or len(mode) != 6:
            raise FrontendHashError("unexpected Git tree entry")
        path = _safe_relative_path(raw_name)
        try:
            size = int(raw_size)
        except ValueError as exc:
            raise FrontendHashError("Git tree entry has no bounded blob size") from exc
        if size < 0 or size > _MAX_FILE_BYTES:
            raise FrontendHashError(f"Git blob size exceeds {_MAX_FILE_BYTES}")
        count += 1
        total += size
        if count > _MAX_FILES or total > _MAX_TOTAL_BYTES:
            raise FrontendHashError("Git frontend tree exceeds file or byte bounds")
        if not any(part in _SOURCE_EXCLUDES for part in path.parts):
            included_count += 1
            included_entries.append((path, mode, object_id, size))
    return included_count, _git_snapshot_hash(root, included_entries)


def _hash_named_blobs(blobs: list[tuple[str, bytes]]) -> tuple[str, int]:
    hasher = hashlib.sha256()
    total = 0
    for name, content in sorted(blobs):
        total += len(content)
        if len(content) > _MAX_FILE_BYTES or total > _MAX_TOTAL_BYTES:
            raise FrontendHashError("dependency blobs exceed byte bounds")
        hasher.update(name.encode("utf-8"))
        hasher.update(b"\0file\0")
        hasher.update(content)
        hasher.update(b"\0")
    return hasher.hexdigest(), len(blobs)


def _worktree_relatives(source_dir: Path) -> list[Path]:
    # Worktree mode is an explicit dirty-source build. Walk the selected root
    # directly so ignored-but-present authoring inputs cannot affect npm while
    # being absent from the fingerprint/snapshot list.
    return _walk_files(source_dir, excludes=_SOURCE_EXCLUDES)


def _source_record(source_dir: Path) -> dict[str, object]:
    mode = os.environ.get("MANYFORGE_FRONTEND_SOURCE_MODE", "committed")
    if len(mode) > 32:
        raise FrontendHashError("MANYFORGE_FRONTEND_SOURCE_MODE is too long")
    mode = mode.strip().lower()
    if mode in {"committed", "git-head", "head"}:
        context = _git_context(source_dir)
        if context is None:
            raise FrontendHashError(
                "MANYFORGE_FRONTEND_SOURCE_MODE=committed requires a containing Git HEAD"
            )
        root, relative = context
        relative_text = relative.as_posix()
        _, tree_output = _git(
            "-C",
            os.fspath(root),
            "rev-parse",
            f"HEAD:{relative_text}",
            trusted_path=root,
        )
        tree_hash = tree_output.decode("ascii").strip()
        if len(tree_hash) != 40 or any(character not in "0123456789abcdef" for character in tree_hash):
            raise FrontendHashError("Git returned an invalid frontend tree identity")
        file_count, snapshot_hash = _git_tree_inventory(root, tree_hash)
        source_hash = hashlib.sha256(f"git-head\0{tree_hash}".encode()).hexdigest()

        dependency_blobs: list[tuple[str, bytes]] = []
        for name in _DEPENDENCY_FILES:
            content = _git_bytes(root, f"{tree_hash}:{name}")
            if content is not None:
                dependency_blobs.append((name, content))
        dependency_hash, dependency_count = _hash_named_blobs(dependency_blobs)

        _, status_output = _git(
            "-C",
            os.fspath(root),
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--",
            relative_text,
            trusted_path=root,
        )
        dirty_records = [record for record in status_output.split(b"\0") if record]
        if len(dirty_records) > _MAX_NODES:
            raise FrontendHashError("Git dirty-entry count exceeds the supported bound")
        return {
            "MANYFORGE_FRONTEND_DEP_FILE_COUNT": dependency_count,
            "MANYFORGE_FRONTEND_DEP_HASH": dependency_hash,
            "MANYFORGE_FRONTEND_DIRTY_FILE_COUNT": len(dirty_records),
            "MANYFORGE_FRONTEND_GIT_REL": relative_text,
            "MANYFORGE_FRONTEND_GIT_ROOT": os.fspath(root),
            "MANYFORGE_FRONTEND_GIT_TREE_HASH": tree_hash,
            "MANYFORGE_FRONTEND_SOURCE_FILE_COUNT": file_count,
            "MANYFORGE_FRONTEND_SOURCE_HASH": source_hash,
            "MANYFORGE_FRONTEND_SOURCE_MODE": "git-head",
            "MANYFORGE_FRONTEND_SNAPSHOT_HASH": snapshot_hash,
        }
    if mode != "worktree":
        raise FrontendHashError(
            "MANYFORGE_FRONTEND_SOURCE_MODE must be 'committed' or 'worktree'"
        )

    relatives = _worktree_relatives(source_dir)
    source_hash, source_count = _hash_files(source_dir, relatives)
    dependency_relatives = [
        Path(name) for name in _DEPENDENCY_FILES if (source_dir / name).is_file()
    ]
    dependency_hash, dependency_count = _hash_files(source_dir, dependency_relatives)
    return {
        "MANYFORGE_FRONTEND_DEP_FILE_COUNT": dependency_count,
        "MANYFORGE_FRONTEND_DEP_HASH": dependency_hash,
        "MANYFORGE_FRONTEND_DIRTY_FILE_COUNT": 0,
        "MANYFORGE_FRONTEND_GIT_REL": "",
        "MANYFORGE_FRONTEND_GIT_ROOT": "",
        "MANYFORGE_FRONTEND_GIT_TREE_HASH": "",
        "MANYFORGE_FRONTEND_SOURCE_FILE_COUNT": source_count,
        "MANYFORGE_FRONTEND_SOURCE_HASH": source_hash,
        "MANYFORGE_FRONTEND_SOURCE_MODE": "worktree",
        "MANYFORGE_FRONTEND_SNAPSHOT_HASH": source_hash,
    }


def _tree_record(tree_dir: Path) -> dict[str, object]:
    relatives = _walk_files(tree_dir, excludes=set())
    tree_hash, tree_count = _hash_files(tree_dir, relatives)
    return {
        "MANYFORGE_FRONTEND_TREE_FILE_COUNT": tree_count,
        "MANYFORGE_FRONTEND_TREE_HASH": tree_hash,
    }


def _write_record(record: Mapping[str, object], output_format: str) -> None:
    if output_format == "json":
        json.dump(record, sys.stdout, sort_keys=True, separators=(",", ":"))
        sys.stdout.write("\n")
        return
    for key in sorted(record):
        value = str(record[key])
        if "\0" in key or "\0" in value:
            raise FrontendHashError("NUL is forbidden in frontend hash output")
        sys.stdout.buffer.write(key.encode("ascii") + b"\0")
        sys.stdout.buffer.write(value.encode("utf-8", "surrogateescape") + b"\0")


def _write_file_list(source_dir: Path) -> None:
    relatives = sorted(
        set(_worktree_relatives(source_dir)),
        key=lambda path: os.fsencode(path.as_posix()),
    )
    # Hashing here both enforces byte/symlink/same-file bounds and rejects a
    # list that changed into a special or escaping entry before publication.
    _hash_files(source_dir, relatives)
    for relative in relatives:
        raw = _path_bytes(relative.as_posix())
        sys.stdout.buffer.write(raw + b"\0")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-format", choices=("json", "nul"), default="json")
    subparsers = parser.add_subparsers(dest="command", required=True)
    source_parser = subparsers.add_parser("source")
    source_parser.add_argument("source_dir")
    tree_parser = subparsers.add_parser("tree")
    tree_parser.add_argument("tree_dir")
    files_parser = subparsers.add_parser("files")
    files_parser.add_argument("source_dir")
    args = parser.parse_args()

    try:
        if args.command == "source":
            record = _source_record(_safe_root(Path(args.source_dir)))
            _write_record(record, args.output_format)
        elif args.command == "tree":
            record = _tree_record(_safe_root(Path(args.tree_dir)))
            _write_record(record, args.output_format)
        else:
            if args.output_format != "json":
                raise FrontendHashError("files output is always a NUL-delimited path list")
            _write_file_list(_safe_root(Path(args.source_dir)))
    except (FrontendHashError, OSError, UnicodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2) from None


if __name__ == "__main__":
    main()
