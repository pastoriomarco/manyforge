#!/usr/bin/env python3
"""Production boundary check for the ROS2 execution package."""

from __future__ import annotations

import pathlib
import sys


def _tokens() -> tuple[str, ...]:
    return (
        "robo" + "plan",
        "Robo" + "Plan",
        "pino" + "cchio",
        "manyforge_" + "collision",
        "manyforge_" + "robo" + "plan",
        "<" + "Ei" + "gen",
        "Ei" + "gen" + "::",
    )


def _production_files(root: pathlib.Path) -> list[pathlib.Path]:
    files = [root / "CMakeLists.txt"]
    for subdir in ("include", "src"):
        files.extend(
            path
            for path in (root / subdir).rglob("*")
            if path.is_file() and path.suffix in {".hpp", ".cpp", ".h", ".cc"}
        )
    return sorted(files)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_boundary.py <package-root>", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    failures: list[str] = []
    for path in _production_files(root):
        text = path.read_text(encoding="utf-8")
        for token in _tokens():
            if token in text:
                rel = path.relative_to(root)
                failures.append(f"{rel}: contains disallowed dependency token {token!r}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
