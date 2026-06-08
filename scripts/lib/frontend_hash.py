#!/usr/bin/env python3
"""Hash frontend source and build trees for launch-time cache stamps."""
from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import subprocess
import sys
from pathlib import Path


_SOURCE_EXCLUDES = {
    ".git",
    ".pytest_cache",
    ".vite",
    "dist",
    "dist-ssr",
    "node_modules",
}
_DEPENDENCY_FILES = ("package.json", "package-lock.json", "npm-shrinkwrap.json")
_GIT = ("git", "-c", "safe.directory=*")


def _emit(key: str, value: object) -> None:
    print(f"{key}={shlex.quote(str(value))}")


def _update_file_hash(hasher: "hashlib._Hash", base: Path, rel: Path) -> None:
    path = base / rel
    rel_posix = rel.as_posix().encode("utf-8", "surrogateescape")
    hasher.update(rel_posix)
    hasher.update(b"\0")
    if path.is_symlink():
        hasher.update(b"symlink\0")
        hasher.update(os.readlink(path).encode("utf-8", "surrogateescape"))
        hasher.update(b"\0")
        return
    if not path.is_file():
        return
    hasher.update(b"file\0")
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            hasher.update(chunk)
    hasher.update(b"\0")


def _hash_files(base: Path, rels: list[Path]) -> tuple[str, int]:
    hasher = hashlib.sha256()
    count = 0
    for rel in sorted(rels, key=lambda p: p.as_posix()):
        path = base / rel
        if path.is_file() or path.is_symlink():
            _update_file_hash(hasher, base, rel)
            count += 1
    return hasher.hexdigest(), count


def _git_source_files(source_dir: Path) -> list[Path] | None:
    try:
        root_result = subprocess.run(
            [*_GIT, "-C", str(source_dir), "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )
        root = Path(root_result.stdout.strip()).resolve()
        rel_source = source_dir.relative_to(root)
    except (ValueError, subprocess.CalledProcessError):
        return None

    files_result = subprocess.run(
        [
            *_GIT,
            "-C",
            str(root),
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            rel_source.as_posix(),
        ],
        check=True,
        capture_output=True,
    )
    rels: list[Path] = []
    for raw in files_result.stdout.split(b"\0"):
        if not raw:
            continue
        repo_rel = Path(raw.decode("utf-8", "surrogateescape"))
        try:
            source_rel = repo_rel.relative_to(rel_source)
        except ValueError:
            continue
        if any(part in _SOURCE_EXCLUDES for part in source_rel.parts):
            continue
        rels.append(source_rel)
    return rels


def _git_context(source_dir: Path) -> tuple[Path, Path] | None:
    try:
        root_result = subprocess.run(
            [*_GIT, "-C", str(source_dir), "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )
        root = Path(root_result.stdout.strip()).resolve()
        rel_source = source_dir.relative_to(root)
    except (ValueError, subprocess.CalledProcessError):
        return None
    return root, rel_source


def _git_bytes(root: Path, object_name: str) -> bytes | None:
    exists = subprocess.run(
        [*_GIT, "-C", str(root), "cat-file", "-e", object_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if exists.returncode != 0:
        return None
    return subprocess.run(
        [*_GIT, "-C", str(root), "show", object_name],
        check=True,
        capture_output=True,
    ).stdout


def _hash_named_blobs(blobs: list[tuple[str, bytes]]) -> tuple[str, int]:
    hasher = hashlib.sha256()
    for name, content in sorted(blobs, key=lambda item: item[0]):
        hasher.update(name.encode("utf-8", "surrogateescape"))
        hasher.update(b"\0file\0")
        hasher.update(content)
        hasher.update(b"\0")
    return hasher.hexdigest(), len(blobs)


def _hash_committed_source(source_dir: Path) -> bool:
    context = _git_context(source_dir)
    if context is None:
        return False
    root, rel_source = context
    rel_posix = rel_source.as_posix()
    tree_result = subprocess.run(
        [*_GIT, "-C", str(root), "rev-parse", f"HEAD:{rel_posix}"],
        check=True,
        capture_output=True,
        text=True,
    )
    tree_hash = tree_result.stdout.strip()
    source_hash = hashlib.sha256(f"git-head\0{tree_hash}".encode()).hexdigest()

    files_result = subprocess.run(
        [*_GIT, "-C", str(root), "ls-tree", "-rz", "-r", "--name-only", "HEAD", "--", rel_posix],
        check=True,
        capture_output=True,
    )
    file_count = sum(1 for raw in files_result.stdout.split(b"\0") if raw)

    dep_blobs: list[tuple[str, bytes]] = []
    for name in _DEPENDENCY_FILES:
        content = _git_bytes(root, f"HEAD:{rel_posix}/{name}")
        if content is not None:
            dep_blobs.append((name, content))
    dependency_hash, dependency_count = _hash_named_blobs(dep_blobs)

    status_result = subprocess.run(
        [
            *_GIT,
            "-C",
            str(root),
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--",
            rel_posix,
        ],
        check=True,
        capture_output=True,
    )
    dirty_count = sum(1 for raw in status_result.stdout.split(b"\0") if raw)

    _emit("MANYFORGE_FRONTEND_SOURCE_MODE", "git-head")
    _emit("MANYFORGE_FRONTEND_SOURCE_HASH", source_hash)
    _emit("MANYFORGE_FRONTEND_SOURCE_FILE_COUNT", file_count)
    _emit("MANYFORGE_FRONTEND_DEP_HASH", dependency_hash)
    _emit("MANYFORGE_FRONTEND_DEP_FILE_COUNT", dependency_count)
    _emit("MANYFORGE_FRONTEND_GIT_ROOT", root)
    _emit("MANYFORGE_FRONTEND_GIT_REL", rel_posix)
    _emit("MANYFORGE_FRONTEND_GIT_TREE_HASH", tree_hash)
    _emit("MANYFORGE_FRONTEND_DIRTY_FILE_COUNT", dirty_count)
    return True


def _walk_source_files(source_dir: Path) -> list[Path]:
    rels: list[Path] = []
    for path in source_dir.rglob("*"):
        rel = path.relative_to(source_dir)
        if any(part in _SOURCE_EXCLUDES for part in rel.parts):
            continue
        if path.is_file() or path.is_symlink():
            rels.append(rel)
    return rels


def hash_source(source_dir: Path) -> None:
    source_dir = source_dir.resolve()
    mode = os.environ.get("MANYFORGE_FRONTEND_SOURCE_MODE", "committed").strip().lower()
    if mode in {"committed", "git-head", "head"}:
        if _hash_committed_source(source_dir):
            return
        print(
            "ERROR: MANYFORGE_FRONTEND_SOURCE_MODE=committed requires a git HEAD "
            f"containing {source_dir}",
            file=sys.stderr,
        )
        raise SystemExit(2)
    if mode != "worktree":
        print(
            "ERROR: MANYFORGE_FRONTEND_SOURCE_MODE must be 'committed' or 'worktree'",
            file=sys.stderr,
        )
        raise SystemExit(2)

    rels = _git_source_files(source_dir)
    if rels is None:
        rels = _walk_source_files(source_dir)
    source_hash, source_count = _hash_files(source_dir, rels)
    dep_rels = [Path(name) for name in _DEPENDENCY_FILES if (source_dir / name).exists()]
    dependency_hash, dependency_count = _hash_files(source_dir, dep_rels)
    _emit("MANYFORGE_FRONTEND_SOURCE_MODE", "worktree")
    _emit("MANYFORGE_FRONTEND_SOURCE_HASH", source_hash)
    _emit("MANYFORGE_FRONTEND_SOURCE_FILE_COUNT", source_count)
    _emit("MANYFORGE_FRONTEND_DEP_HASH", dependency_hash)
    _emit("MANYFORGE_FRONTEND_DEP_FILE_COUNT", dependency_count)
    _emit("MANYFORGE_FRONTEND_DIRTY_FILE_COUNT", 0)


def hash_tree(tree_dir: Path) -> None:
    tree_dir = tree_dir.resolve()
    rels = [
        path.relative_to(tree_dir)
        for path in tree_dir.rglob("*")
        if path.is_file() or path.is_symlink()
    ]
    tree_hash, tree_count = _hash_files(tree_dir, rels)
    _emit("MANYFORGE_FRONTEND_TREE_HASH", tree_hash)
    _emit("MANYFORGE_FRONTEND_TREE_FILE_COUNT", tree_count)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    source_parser = subparsers.add_parser("source")
    source_parser.add_argument("source_dir")
    tree_parser = subparsers.add_parser("tree")
    tree_parser.add_argument("tree_dir")
    args = parser.parse_args()

    if args.command == "source":
        hash_source(Path(args.source_dir))
    elif args.command == "tree":
        hash_tree(Path(args.tree_dir))


if __name__ == "__main__":
    main()
