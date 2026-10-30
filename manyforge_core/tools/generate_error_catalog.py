#!/usr/bin/env python3
"""Render the error catalog into C++, Python and TypeScript constants.

The one source is `manyforge_core/resources/error_catalog.tsv` (`code`,
`compact text`, `full text`). The outputs are generated, checked in and kept
fresh by `--check` in CI; nothing reads the TSV at runtime.

- C++: `manyforge::core::error_codes::k<CamelCode>` code constants.
- Python: `manyforge_headless_api.error_codes.<UPPER_CODE>` code constants.
- TypeScript: `ERROR_CATALOG` (code -> [compact, full]) for the GUI.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "manyforge_core/resources/error_catalog.tsv"
CPP = ROOT / "manyforge_core/include/manyforge_core/types/error_catalog.generated.hpp"
PYTHON = ROOT / "manyforge_headless_api/error_codes.py"
TYPESCRIPT = ROOT / "manyforge_composer/frontend/src/api/errorCatalog.generated.ts"

_CODE = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+")
_HEADER = "Generated from manyforge_core/resources/error_catalog.tsv by manyforge_core/tools/generate_error_catalog.py; do not edit."


def read_catalog(path: Path = CATALOG) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) != 3 or not _CODE.fullmatch(columns[0]) or not columns[1]:
            raise SystemExit(f"{path}:{number}: expected code<TAB>compact<TAB>full")
        if columns[0] in seen:
            raise SystemExit(f"{path}:{number}: duplicate code {columns[0]}")
        seen.add(columns[0])
        rows.append((columns[0], columns[1], columns[2]))
    return sorted(rows)


def _camel(code: str) -> str:
    return "k" + "".join(part[:1].upper() + part[1:] for part in code.split("_"))


def render_cpp(rows: list[tuple[str, str, str]]) -> str:
    names: dict[str, str] = {}
    lines = [
        f"// {_HEADER}",
        "#pragma once",
        "",
        "#include <string_view>",
        "",
        "namespace manyforge::core::error_codes {",
        "",
    ]
    for code, _compact, _full in rows:
        name = _camel(code)
        if name in names:
            raise SystemExit(f"codes {names[name]} and {code} share the C++ name {name}")
        names[name] = code
        lines.append(f'inline constexpr std::string_view {name}{{"{code}"}};')
    lines += ["", "}  // namespace manyforge::core::error_codes", ""]
    return "\n".join(lines)


def render_python(rows: list[tuple[str, str, str]]) -> str:
    lines = [f'"""{_HEADER}"""', "", "from typing import Final", ""]
    lines += [f'{code.upper()}: Final = "{code}"' for code, _compact, _full in rows]
    return "\n".join(lines) + "\n"


def render_typescript(rows: list[tuple[str, str, str]]) -> str:
    lines = [
        f"// {_HEADER}",
        "",
        "/** code -> [compact text, full text]; an empty full text is not written yet. */",
        "export const ERROR_CATALOG: Readonly<Record<string, readonly [string, string]>> = {",
    ]
    lines += [
        f"  {code}: [{json.dumps(compact, ensure_ascii=False)}, {json.dumps(full, ensure_ascii=False)}],"
        for code, compact, full in rows
    ]
    lines += ["};", ""]
    return "\n".join(lines)


def outputs() -> dict[Path, str]:
    rows = read_catalog()
    return {
        CPP: render_cpp(rows),
        PYTHON: render_python(rows),
        TYPESCRIPT: render_typescript(rows),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when an output is stale")
    args = parser.parse_args()
    stale = []
    for path, text in outputs().items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.check:
            stale.append(path.relative_to(ROOT))
        else:
            path.write_text(text, encoding="utf-8")
    if stale:
        for path in stale:
            print(f"{path} is stale; run manyforge_core/tools/generate_error_catalog.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
