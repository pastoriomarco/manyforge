#!/usr/bin/env python3
"""Generate or verify the build-owned v2 node-environment manifest."""

from __future__ import annotations

import argparse
import json

from manyforge_behavior.node_environment import (
    AUTHORITATIVE_PROJECT_PATH,
    BUILTIN_ENVIRONMENT_MANIFEST_PATH,
    PACKAGED_PROJECT_SNAPSHOT_PATH,
    generate_builtin_node_environment_manifest,
)
from manyforge_behavior.qualified_node_extensions import (
    PACKAGE_ROOT,
    PROJECT_SNAPSHOT_PATH,
)


def _render() -> str:
    manifest = generate_builtin_node_environment_manifest()
    return (
        json.dumps(
            manifest.to_dict(),
            ensure_ascii=False,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if the checked-in manifest is absent or stale.",
    )
    args = parser.parse_args()
    try:
        authoritative_project = AUTHORITATIVE_PROJECT_PATH.read_bytes()
        packaged_snapshot = PACKAGED_PROJECT_SNAPSHOT_PATH.read_bytes()
    except OSError as exc:
        parser.error(str(exc))
    if packaged_snapshot != authoritative_project:
        parser.error(
            f"{PACKAGED_PROJECT_SNAPSHOT_PATH} is stale; it must be an exact "
            f"byte snapshot of {AUTHORITATIVE_PROJECT_PATH}"
        )
    if (PACKAGE_ROOT / "pyproject.toml").read_bytes() != PROJECT_SNAPSHOT_PATH.read_bytes():
        parser.error("selected extension project snapshot is stale")
    rendered = _render()
    if args.check:
        try:
            current = BUILTIN_ENVIRONMENT_MANIFEST_PATH.read_text(encoding="utf-8")
        except OSError as exc:
            parser.error(str(exc))
        if current != rendered:
            parser.error(
                f"{BUILTIN_ENVIRONMENT_MANIFEST_PATH} is stale; " "run this command without --check"
            )
        return 0
    BUILTIN_ENVIRONMENT_MANIFEST_PATH.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
