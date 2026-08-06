#!/usr/bin/env python3
"""Verify exact external source/build content before any configure step."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys
from collections.abc import Iterable, Sequence
from typing import Any


_LEDGER_SCHEMA = "manyforge.composable_cell.e1.external_delta_ledger.v1"
_CONTENT_DOMAIN = b"manyforge.external-source-content.v1\0"


class VerificationError(RuntimeError):
    """The supplied source is not the exact declared closure."""


def _frame(digest: Any, label: bytes, payload: bytes) -> None:
    digest.update(len(label).to_bytes(8, "little"))
    digest.update(label)
    digest.update(len(payload).to_bytes(8, "little"))
    digest.update(payload)


def _source_paths(root: pathlib.Path) -> Iterable[pathlib.Path]:
    for directory, names, filenames in os.walk(root, followlinks=False):
        names[:] = sorted(name for name in names if name != ".git")
        relative_directory = pathlib.Path(directory).relative_to(root)
        for name in sorted(filenames):
            relative = relative_directory / name
            if ".git" in relative.parts:
                continue
            yield root / relative
        for name in tuple(names):
            path = pathlib.Path(directory) / name
            if path.is_symlink():
                names.remove(name)
                yield path


def canonical_content_digest(root: pathlib.Path) -> str:
    root = root.resolve()
    if not root.is_dir():
        raise VerificationError(f"source root is not a directory: {root}")
    digest = hashlib.sha256()
    digest.update(_CONTENT_DOMAIN)
    for path in sorted(
        _source_paths(root), key=lambda item: item.relative_to(root).as_posix()
    ):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        metadata = path.lstat()
        permissions = f"{stat.S_IMODE(metadata.st_mode):04o}".encode("ascii")
        if stat.S_ISREG(metadata.st_mode):
            kind = b"file"
            payload = path.read_bytes()
        elif stat.S_ISLNK(metadata.st_mode):
            kind = b"symlink"
            payload = os.readlink(path).encode("utf-8")
        else:
            raise VerificationError(
                f"source closure contains a special file: {path.relative_to(root)}"
            )
        _frame(digest, b"path", relative)
        _frame(digest, b"kind", kind)
        _frame(digest, b"mode", permissions)
        _frame(digest, b"content", payload)
    return digest.hexdigest()


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _git(root: pathlib.Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise VerificationError(f"git {' '.join(arguments)} failed: {detail}")
    return result.stdout.strip()


def _git_archive_sha256(root: pathlib.Path) -> str:
    process = subprocess.Popen(
        ["git", "-C", str(root), "archive", "--format=tar", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if process.stdout is None or process.stderr is None:
        raise VerificationError("git archive pipes are unavailable")
    digest = hashlib.sha256()
    for chunk in iter(lambda: process.stdout.read(1024 * 1024), b""):
        digest.update(chunk)
    error = process.stderr.read().decode("utf-8", errors="replace").strip()
    returncode = process.wait()
    if returncode != 0:
        raise VerificationError(f"git archive failed: {error}")
    return digest.hexdigest()


def _entry_map(ledger: dict[str, object]) -> dict[str, dict[str, object]]:
    sources = ledger.get("frozen_sources")
    if type(sources) is not list:
        raise VerificationError("ledger frozen_sources is unavailable")
    result: dict[str, dict[str, object]] = {}
    for value in sources:
        if type(value) is not dict or type(value.get("id")) is not str:
            raise VerificationError("ledger source entry is invalid")
        source_id = str(value["id"])
        if source_id in result:
            raise VerificationError(f"duplicate ledger source id: {source_id}")
        result[source_id] = value
    return result


def _validate_git_identity(
    root: pathlib.Path,
    entry: dict[str, object],
    entries: dict[str, dict[str, object]],
) -> None:
    identity = entry["identity"]
    assert type(identity) is dict
    expected_commit = identity.get("commit")
    expected_tree = identity.get("tree")
    expected_archive = identity.get("git_archive_sha256")
    if type(expected_commit) is not str or type(expected_tree) is not str:
        raise VerificationError("source has no exact Git commit/tree identity")
    if _git(root, "rev-parse", "HEAD") != expected_commit:
        raise VerificationError("source HEAD does not match the declared commit")
    if _git(root, "rev-parse", "HEAD^{tree}") != expected_tree:
        raise VerificationError("source HEAD tree does not match the declared tree")
    if type(expected_archive) is str and _git_archive_sha256(root) != expected_archive:
        raise VerificationError("source Git archive digest does not match")

    submodules = entry.get("submodules", [])
    if type(submodules) is not list:
        raise VerificationError("source submodule declaration is invalid")
    for submodule in submodules:
        if (
            type(submodule) is not dict
            or type(submodule.get("path")) is not str
            or type(submodule.get("commit")) is not str
            or submodule.get("state") not in {"initialized", "uninitialized"}
        ):
            raise VerificationError("source submodule entry is invalid")
        path = str(submodule["path"])
        tree_row = _git(root, "ls-tree", "HEAD", "--", path).split()
        if (
            len(tree_row) < 3
            or tree_row[0] != "160000"
            or tree_row[1] != "commit"
            or tree_row[2] != submodule["commit"]
        ):
            raise VerificationError(f"submodule gitlink does not match: {path}")
        submodule_root = root / path
        populated = submodule_root.is_dir() and any(submodule_root.iterdir())
        if submodule["state"] == "uninitialized" and populated:
            raise VerificationError(f"unqualified submodule is populated: {path}")
        if submodule["state"] == "initialized":
            source_id = submodule.get("source_id")
            if type(source_id) is not str or source_id not in entries or not populated:
                raise VerificationError(
                    f"initialized submodule has no frozen source identity: {path}"
                )
            expected_nested = entries[source_id]["identity"]
            if (
                type(expected_nested) is not dict
                or expected_nested.get("commit") != submodule["commit"]
            ):
                raise VerificationError(
                    f"initialized submodule identity does not match: {path}"
                )


def verify_source(
    ledger_path: pathlib.Path,
    source_id: str,
    root: pathlib.Path,
    *,
    content_kind: str,
    allow_stripped_git: bool,
) -> dict[str, str]:
    ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
    if type(ledger) is not dict or ledger.get("schema") != _LEDGER_SCHEMA:
        raise VerificationError("external-source ledger schema mismatch")
    entries = _entry_map(ledger)
    if source_id not in entries:
        raise VerificationError(f"source id is absent from the ledger: {source_id}")
    entry = entries[source_id]
    identity = entry.get("identity")
    license_identity = entry.get("license")
    if type(identity) is not dict or type(license_identity) is not dict:
        raise VerificationError("source identity/license declaration is incomplete")
    digest_key = f"{content_kind}_content_sha256"
    expected_content = identity.get(digest_key)
    if type(expected_content) is not str or len(expected_content) != 64:
        raise VerificationError(f"source has no {digest_key}")
    actual_content = canonical_content_digest(root)
    if actual_content != expected_content:
        raise VerificationError(
            f"source canonical {content_kind} content digest does not match: "
            f"expected {expected_content}, observed {actual_content}"
        )

    license_path = license_identity.get("path")
    license_sha256 = license_identity.get("sha256")
    if type(license_path) is not str or type(license_sha256) is not str:
        raise VerificationError("source license identity is incomplete")
    resolved_license = root / license_path
    if not resolved_license.is_file() or resolved_license.is_symlink():
        raise VerificationError("source license file is unavailable")
    if _sha256_file(resolved_license) != license_sha256:
        raise VerificationError("source license digest does not match")

    git_available = (
        subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--git-dir"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )
    if git_available:
        _validate_git_identity(root, entry, entries)
    elif not allow_stripped_git:
        raise VerificationError("source Git identity is unavailable")
    return {
        "source_id": source_id,
        "content_kind": content_kind,
        "content_sha256": actual_content,
        "git_identity": "verified" if git_available else "stripped_content_verified",
        "license_sha256": license_sha256,
    }


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ledger", type=pathlib.Path)
    parser.add_argument("--source-id")
    parser.add_argument("--root", type=pathlib.Path)
    parser.add_argument("--content-kind", choices=("source", "build"), default="build")
    parser.add_argument("--allow-stripped-git", action="store_true")
    parser.add_argument("--print-content-digest", type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.print_content_digest is not None:
        if (
            args.ledger is not None
            or args.source_id is not None
            or args.root is not None
        ):
            parser.error(
                "--print-content-digest cannot be combined with verification inputs"
            )
    elif args.ledger is None or args.source_id is None or args.root is None:
        parser.error("--ledger, --source-id, and --root are required")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.print_content_digest is not None:
            print(canonical_content_digest(args.print_content_digest))
            return 0
        result = verify_source(
            args.ledger,
            args.source_id,
            args.root,
            content_kind=args.content_kind,
            allow_stripped_git=args.allow_stripped_git,
        )
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return 0
    except (OSError, ValueError, VerificationError, json.JSONDecodeError) as exc:
        print(f"external source verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
