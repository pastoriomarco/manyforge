#!/usr/bin/env python3
"""Capture one commit-, tree-, and delta-bound Git source identity.

The framing is intentionally identical to the v1 identity used by the live
qualification collector.  Qualification shell runners use this standalone
entry point so they do not need to carry subtly different digest clones.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any


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


class SourceIdentityError(RuntimeError):
    """The requested checkout cannot be represented by the v1 contract."""


def _git_bytes(repo_root: Path, *args: str) -> bytes:
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo_root), *args],
            stderr=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace").strip()
        raise SourceIdentityError(
            f"git {' '.join(args)} failed for {repo_root}: {detail}"
        ) from error


def _git_text(repo_root: Path, *args: str) -> str:
    return _git_bytes(repo_root, *args).decode("utf-8").strip()


def worktree_delta_sha256(repo_root: Path) -> str:
    """Fingerprint every tracked and non-ignored untracked delta from HEAD."""

    digest = hashlib.sha256()
    tracked = _git_bytes(
        repo_root,
        "diff",
        "--binary",
        "--full-index",
        "--no-ext-diff",
        "HEAD",
        "--",
    )
    digest.update(b"tracked-diff\0")
    digest.update(len(tracked).to_bytes(8, "big"))
    digest.update(tracked)

    untracked_output = _git_bytes(
        repo_root,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
    )
    untracked = sorted(name for name in untracked_output.split(b"\0") if name)
    digest.update(b"untracked-files\0")
    digest.update(len(untracked).to_bytes(8, "big"))
    for encoded_name in untracked:
        relative = os.fsdecode(encoded_name)
        path = repo_root / relative
        metadata = path.lstat()
        mode = stat.S_IMODE(metadata.st_mode)
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(mode.to_bytes(4, "big"))
        if stat.S_ISLNK(metadata.st_mode):
            digest.update(b"symlink\0")
            payload = os.fsencode(os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            digest.update(b"regular\0")
            payload = path.read_bytes()
        else:
            raise SourceIdentityError(
                f"{path}: unsupported untracked source file type"
            )
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


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

    digest = hashlib.sha256()
    tracked = _git_bytes(
        repo_root,
        "diff",
        "--binary",
        "--full-index",
        "--no-ext-diff",
        "HEAD",
        "--",
        ".",
        f":(exclude){SPECS_CONTRACT_EXCLUDED_PATH}",
    )
    digest.update(b"tracked-diff\0")
    digest.update(len(tracked).to_bytes(8, "big"))
    digest.update(tracked)
    untracked = sorted(
        name
        for name in _git_bytes(
            repo_root, "ls-files", "--others", "--exclude-standard", "-z"
        ).split(b"\0")
        if name and os.fsdecode(name) != SPECS_CONTRACT_EXCLUDED_PATH
    )
    digest.update(b"untracked-files\0")
    digest.update(len(untracked).to_bytes(8, "big"))
    for encoded_name in untracked:
        path = repo_root / os.fsdecode(encoded_name)
        metadata = path.lstat()
        mode = stat.S_IMODE(metadata.st_mode)
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(mode.to_bytes(4, "big"))
        if stat.S_ISLNK(metadata.st_mode):
            digest.update(b"symlink\0")
            payload = os.fsencode(os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            digest.update(b"regular\0")
            payload = path.read_bytes()
        else:
            raise SourceIdentityError(
                f"{path}: unsupported Specs source file type"
            )
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


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


def specs_contract_identity(repo_root: Path) -> dict[str, Any]:
    source = source_identity(repo_root)
    delta = specs_contract_delta_sha256(repo_root.resolve())
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


def source_identity(repo_root: Path) -> dict[str, Any]:
    """Collect one canonical v1 identity for ``repo_root``."""

    resolved = repo_root.resolve()
    if not resolved.is_dir():
        raise SourceIdentityError(f"source repository is not a directory: {resolved}")
    dirty = bool(_git_text(resolved, "status", "--porcelain"))
    head_commit = _git_text(resolved, "rev-parse", "HEAD")
    head_tree = _git_text(resolved, "rev-parse", "HEAD^{tree}")
    delta_sha256 = worktree_delta_sha256(resolved)
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
        identity = source_identity(args.repo_root)
        contract_identity = (
            specs_contract_identity(args.repo_root)
            if args.include_specs_contract_identity
            else None
        )
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
