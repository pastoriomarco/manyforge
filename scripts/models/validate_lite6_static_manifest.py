#!/usr/bin/env python3
"""Validate the vendored Lite6 static model manifest.

This is a ROS-free checksum/provenance gate for the ManyForge-only Lite6 model
bundle. It validates the generated URDF/SRDF/license/mesh manifest and ignores
the separate ManyMove runtime-capture evidence directory, which has its own
comparison gate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Sequence


EXPECTED_SCHEMA = "manyforge.lite6_static_model_manifest.v1"
EXPECTED_BRANCH = "lyrical-no-gazebo"
EXPECTED_COMMIT = "ef7351f90e451a37ef73de69c643b7e85040401a"
MANIFEST_NAME = "STATIC_MANIFEST.json"
IGNORED_DIRS = {"manymove_runtime_capture"}
# The component descriptor describes the bundle; it is not model content.
DESCRIPTOR_NAME = "component.yaml"


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_model_dir() -> Path:
    return repo_root() / "manyforge_example_models" / "models" / "lite6_lite_gripper"


def require_dict(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{path} must be an object")
    return value


def require_string(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{path} must be a non-empty string")
    return value


def require_int(value: Any, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail(f"{path} must be an integer")
    if value < 0:
        fail(f"{path} must be non-negative")
    return value


def require_list(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        fail(f"{path} must be a list")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relative_static_files(model_dir: Path) -> set[str]:
    result: set[str] = set()
    for path in model_dir.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(model_dir)
        parts = relative.parts
        if parts[0] in (MANIFEST_NAME, DESCRIPTOR_NAME) or parts[0] in IGNORED_DIRS:
            continue
        result.add(relative.as_posix())
    return result


def validate_safe_relative_path(path_text: str, entry_path: str) -> None:
    path = Path(path_text)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        fail(f"{entry_path}.path must be a safe relative path")
    if path.parts[0] in IGNORED_DIRS:
        fail(f"{entry_path}.path points into ignored runtime evidence")


def validate_manifest(
    model_dir: Path,
    expected_branch: str,
    expected_commit: str,
) -> dict[str, Any]:
    manifest_path = model_dir / MANIFEST_NAME
    if not manifest_path.is_file():
        fail(f"missing {MANIFEST_NAME} in {model_dir}")
    manifest = require_dict(
        json.loads(manifest_path.read_text(encoding="utf-8")), "manifest"
    )
    if manifest.get("schema") != EXPECTED_SCHEMA:
        fail(f"manifest.schema must be {EXPECTED_SCHEMA}")

    source = require_dict(manifest.get("source"), "manifest.source")
    branch = require_string(source.get("branch"), "manifest.source.branch")
    commit = require_string(source.get("commit"), "manifest.source.commit")
    if branch != expected_branch:
        fail(f"manifest.source.branch={branch!r} does not match {expected_branch!r}")
    if commit != expected_commit:
        fail(f"manifest.source.commit={commit!r} does not match {expected_commit!r}")

    files = require_list(manifest.get("files"), "manifest.files")
    seen_paths: set[str] = set()
    for index, raw_entry in enumerate(files):
        entry_path = f"manifest.files[{index}]"
        entry = require_dict(raw_entry, entry_path)
        relative_path = require_string(entry.get("path"), f"{entry_path}.path")
        validate_safe_relative_path(relative_path, entry_path)
        if relative_path in seen_paths:
            fail(f"duplicate manifest path: {relative_path}")
        seen_paths.add(relative_path)

        path = model_dir / relative_path
        if not path.is_file():
            fail(f"manifested file missing: {relative_path}")
        size = require_int(entry.get("size_bytes"), f"{entry_path}.size_bytes")
        digest = require_string(entry.get("sha256"), f"{entry_path}.sha256")
        if path.stat().st_size != size:
            fail(f"{relative_path} size drifted")
        if sha256_file(path) != digest:
            fail(f"{relative_path} sha256 drifted")

    actual_paths = relative_static_files(model_dir)
    if seen_paths != actual_paths:
        missing = sorted(actual_paths - seen_paths)
        stale = sorted(seen_paths - actual_paths)
        details = []
        if missing:
            details.append("unmanifested static files: " + ", ".join(missing))
        if stale:
            details.append("stale manifest files: " + ", ".join(stale))
        fail("; ".join(details))

    return {
        "schema": EXPECTED_SCHEMA,
        "model_dir": str(model_dir),
        "file_count": len(seen_paths),
        "source_branch": branch,
        "source_commit": commit,
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, default=default_model_dir())
    parser.add_argument("--expected-branch", default=EXPECTED_BRANCH)
    parser.add_argument("--expected-commit", default=EXPECTED_COMMIT)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = validate_manifest(
            args.model_dir.resolve(), args.expected_branch, args.expected_commit
        )
    except Exception as exc:  # noqa: BLE001 - CLI validation surface
        if args.json:
            print(json.dumps({"passed": False, "error": str(exc)}, indent=2))
        else:
            print(f"Lite6 static manifest rejected: {exc}")
        return 1

    if args.json:
        print(json.dumps({"passed": True, **result}, indent=2, sort_keys=True))
    else:
        print(
            "Lite6 static manifest accepted: "
            f"{result['file_count']} files from {result['source_commit']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
