#!/usr/bin/env python3
"""Render the error catalog into C++, Python and TypeScript constants.

The one source is `manyforge_core/resources/error_catalog.tsv` (`code`,
`compact text`, `full text`). The outputs are generated, checked in and kept
fresh by `--check` in CI; nothing reads the TSV at runtime.

- C++: `manyforge::core::error_codes::k<CamelCode>` code constants, plus the
  sorted code array `kSortedCodes` and `error_catalog_code_index`,
  which maps a code to 1 + its position (0 when not catalogued) without
  allocating; Stop event records store that index (stop state model §2.4).
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
        "#include <algorithm>",
        "#include <array>",
        "#include <cstdint>",
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
    if len(rows) >= 0xFFFF:
        raise SystemExit("error catalog index no longer fits uint16 below 0xFFFF")
    lines += [
        "",
        "/// Every catalogued code, sorted by code text (byte order).",
        f"inline constexpr std::array<std::string_view, {len(rows)}> kSortedCodes{{",
    ]
    lines += [f"    {_camel(code)}," for code, _compact, _full in rows]
    lines += [
        "};",
        "",
        "/// 1 + the position of `code` in `kSortedCodes`; 0 when the",
        "/// code is not catalogued. Binary search; no allocation.",
        "[[nodiscard]] constexpr std::uint16_t error_catalog_code_index(std::string_view code) noexcept {",
        "  const auto found = std::lower_bound(kSortedCodes.begin(),",
        "                                      kSortedCodes.end(), code);",
        "  if (found == kSortedCodes.end() || *found != code) return 0U;",
        "  return static_cast<std::uint16_t>(1 + (found - kSortedCodes.begin()));",
        "}",
        "",
        "}  // namespace manyforge::core::error_codes",
        "",
    ]
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
