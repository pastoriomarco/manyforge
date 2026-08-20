"""Read-only package-root provenance aggregate.

The aggregate is an integrity/provenance observation only.  It is not a
package version, a compatibility identity, or an admission substitute.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import stat
import struct
import sys
import unicodedata


_DOMAIN = b"manyforge.package_root_aggregate.v1\0"
_READ_SIZE = 1024 * 1024
_NOFOLLOW = getattr(os, "O_NOFOLLOW", 0)
_DIRECTORY = getattr(os, "O_DIRECTORY", 0)


class PackageAggregateUnavailable(RuntimeError):
    """The requested root could not be observed safely and consistently."""


@dataclass(frozen=True, slots=True)
class _Entry:
    kind: bytes
    relative_path: str
    path_bytes: bytes
    stat_fingerprint: tuple[int, ...]
    size: int


def _stat_fingerprint(value: os.stat_result) -> tuple[int, ...]:
    # atime is deliberately omitted: reading the package may update it.
    return (
        value.st_mode,
        value.st_ino,
        value.st_dev,
        value.st_nlink,
        value.st_uid,
        value.st_gid,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _encoded_relative_path(parts: tuple[str, ...]) -> tuple[str, bytes]:
    relative = "/".join(parts)
    if not relative or unicodedata.normalize("NFC", relative) != relative:
        raise PackageAggregateUnavailable("package path is not non-empty NFC text")
    try:
        encoded = relative.encode("utf-8")
    except UnicodeEncodeError as error:
        raise PackageAggregateUnavailable("package path is not valid UTF-8") from error
    return relative, encoded


def _scan_directory(
    directory_fd: int,
    parts: tuple[str, ...],
    entries: list[_Entry],
) -> None:
    with os.scandir(directory_fd) as iterator:
        children = [
            (child.name, child.stat(follow_symlinks=False)) for child in iterator
        ]
    for child_name, observed in children:
        child_parts = (*parts, child_name)
        relative, encoded = _encoded_relative_path(child_parts)
        fingerprint = _stat_fingerprint(observed)
        if stat.S_ISDIR(observed.st_mode):
            entries.append(_Entry(b"D", relative, encoded, fingerprint, 0))
            child_fd = os.open(
                child_name,
                os.O_RDONLY | _DIRECTORY | _NOFOLLOW,
                dir_fd=directory_fd,
            )
            try:
                if _stat_fingerprint(os.fstat(child_fd)) != fingerprint:
                    raise PackageAggregateUnavailable(
                        f"package directory changed during scan: {relative}"
                    )
                _scan_directory(child_fd, child_parts, entries)
                if _stat_fingerprint(os.fstat(child_fd)) != fingerprint:
                    raise PackageAggregateUnavailable(
                        f"package directory changed during scan: {relative}"
                    )
            finally:
                os.close(child_fd)
        elif stat.S_ISREG(observed.st_mode):
            entries.append(
                _Entry(b"F", relative, encoded, fingerprint, observed.st_size)
            )
        else:
            raise PackageAggregateUnavailable(
                f"package contains a nonregular entry: {relative}"
            )


def _enumerate(root_fd: int) -> tuple[_Entry, ...]:
    entries: list[_Entry] = []
    _scan_directory(root_fd, (), entries)
    entries.sort(key=lambda item: item.path_bytes)
    return tuple(entries)


def _open_parent(root_fd: int, parts: tuple[str, ...]) -> int:
    current = os.dup(root_fd)
    try:
        for part in parts:
            following = os.open(
                part,
                os.O_RDONLY | _DIRECTORY | _NOFOLLOW,
                dir_fd=current,
            )
            os.close(current)
            current = following
        return current
    except Exception:
        os.close(current)
        raise


def _hash_file(digest: object, root_fd: int, entry: _Entry) -> None:
    parts = tuple(entry.relative_path.split("/"))
    parent_fd = _open_parent(root_fd, parts[:-1])
    try:
        file_fd = os.open(
            parts[-1],
            os.O_RDONLY | os.O_NONBLOCK | _NOFOLLOW,
            dir_fd=parent_fd,
        )
        try:
            opened = os.fstat(file_fd)
            if (
                not stat.S_ISREG(opened.st_mode)
                or _stat_fingerprint(opened) != entry.stat_fingerprint
            ):
                raise PackageAggregateUnavailable(
                    f"package file changed before read: {entry.relative_path}"
                )
            remaining = entry.size
            while remaining:
                block = os.read(file_fd, min(_READ_SIZE, remaining))
                if not block:
                    raise PackageAggregateUnavailable(
                        f"package file changed during read: {entry.relative_path}"
                    )
                digest.update(block)
                remaining -= len(block)
            if os.read(file_fd, 1):
                raise PackageAggregateUnavailable(
                    f"package file changed during read: {entry.relative_path}"
                )
            if _stat_fingerprint(os.fstat(file_fd)) != entry.stat_fingerprint:
                raise PackageAggregateUnavailable(
                    f"package file changed during read: {entry.relative_path}"
                )
        finally:
            os.close(file_fd)
        after = os.stat(parts[-1], dir_fd=parent_fd, follow_symlinks=False)
        if _stat_fingerprint(after) != entry.stat_fingerprint:
            raise PackageAggregateUnavailable(
                f"package file changed after read: {entry.relative_path}"
            )
    finally:
        os.close(parent_fd)


def package_aggregate_sha256(package_root: Path | str) -> str:
    """Return the canonical v1 aggregate for one stable package root."""

    root = Path(package_root)
    if not root.is_absolute():
        raise PackageAggregateUnavailable("package root must be absolute")
    if not _NOFOLLOW or not _DIRECTORY:
        raise PackageAggregateUnavailable(
            "platform does not provide no-follow directory scanning"
        )
    try:
        direct = os.lstat(root)
        if stat.S_ISLNK(direct.st_mode) or not stat.S_ISDIR(direct.st_mode):
            raise PackageAggregateUnavailable(
                "package root must be a directly non-symlink directory"
            )
        canonical = root.resolve(strict=True)
        root_fd = os.open(canonical, os.O_RDONLY | _DIRECTORY | _NOFOLLOW)
        try:
            root_fingerprint = _stat_fingerprint(os.fstat(root_fd))
            if (
                _stat_fingerprint(direct) != root_fingerprint
                or _stat_fingerprint(os.lstat(canonical)) != root_fingerprint
            ):
                raise PackageAggregateUnavailable(
                    "package root changed before scan"
                )
            entries = _enumerate(root_fd)
            if not any(entry.kind == b"F" for entry in entries):
                raise PackageAggregateUnavailable(
                    "package root must contain at least one regular file"
                )
            digest = hashlib.sha256()
            digest.update(_DOMAIN)
            digest.update(struct.pack(">Q", len(entries)))
            for entry in entries:
                digest.update(entry.kind)
                digest.update(struct.pack(">Q", len(entry.path_bytes)))
                digest.update(entry.path_bytes)
                if entry.kind == b"F":
                    digest.update(struct.pack(">Q", entry.size))
                    _hash_file(digest, root_fd, entry)
            if _enumerate(root_fd) != entries:
                raise PackageAggregateUnavailable(
                    "package root changed during scan"
                )
            if (
                _stat_fingerprint(os.fstat(root_fd)) != root_fingerprint
                or _stat_fingerprint(os.lstat(canonical)) != root_fingerprint
                or _stat_fingerprint(os.lstat(root)) != root_fingerprint
            ):
                raise PackageAggregateUnavailable(
                    "package root changed during scan"
                )
            return digest.hexdigest()
        finally:
            os.close(root_fd)
    except PackageAggregateUnavailable:
        raise
    except (OSError, OverflowError, ValueError) as error:
        raise PackageAggregateUnavailable(
            f"package root cannot be observed safely: {error}"
        ) from error


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if len(arguments) != 2 or arguments[0] != "--package-root":
        print("package_aggregate_unavailable", file=sys.stderr)
        return 70
    try:
        print(package_aggregate_sha256(arguments[1]))
        return 0
    except PackageAggregateUnavailable:
        print("package_aggregate_unavailable", file=sys.stderr)
        return 70


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "PackageAggregateUnavailable",
    "package_aggregate_sha256",
    "main",
]
