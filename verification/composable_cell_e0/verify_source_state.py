from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


FULL_GIT_OBJECT = re.compile(r"[0-9a-f]{40,64}")


def git(source_root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(source_root), *args],
        text=True,
        stderr=subprocess.STDOUT,
    ).strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    expected = args.expected_commit.lower()
    if FULL_GIT_OBJECT.fullmatch(expected) is None:
        raise SystemExit("--expected-commit must be a full 40-64 digit object ID")

    actual = git(source_root, "rev-parse", "HEAD").lower()
    if actual != expected:
        raise SystemExit(f"source commit mismatch: expected {expected}, got {actual}")

    status = git(
        source_root,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
    )
    if status:
        raise SystemExit(f"source tree is not clean:\n{status}")

    proof_readme = "verification/composable_cell_e0/README.md"
    try:
        git(source_root, "ls-files", "--error-unmatch", proof_readme)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"proof source is not tracked: {proof_readme}") from error

    print(
        json.dumps(
            {
                "source_root": str(source_root),
                "source_commit": actual,
                "source_status": "clean",
                "proof_readme": proof_readme,
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
