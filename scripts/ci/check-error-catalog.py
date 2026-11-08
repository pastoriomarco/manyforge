#!/usr/bin/env python3
"""Refuse any error code the program uses that the error catalog lacks.

The catalog is `manyforge_core/resources/error_catalog.tsv`: one line per error,
three tab-separated columns `code`, `compact text`, `full text`. The area is the
code prefix; there is no severity column.

A code is "used" when a snake_case string literal (two or more segments) appears
at one of these error-construction sites in non-test sources:

- C++: the first literal argument of a call whose name ends in `Error`,
  `Failure`, `Refusal`, `Rejection` or `Exception`, or is one of the helpers in
  `_CPP_CALLS`; an `ErrorDTO{...}` initializer; a `kError*`/`k*ReasonCode`
  constant; a `{"code", ...}` canonical member.
- Python: the first literal argument of a call whose name ends in `Error`,
  `Failure`, `Refusal`, `Rejection` or `Exception`, or a listed helper with
  an optional leading underscore; a `"code": "..."` member; a `code="..."`
  keyword.
- TypeScript: a code compared in a branch (`code === '...'`, `code !== '...'`).

Codes built at runtime (for example `section + "_source_absent"`) are invisible
to this check by construction. A few operator-facing codes are produced where no
pattern can see them (a reason assignment, a projection reason, a reason-code
constant); `_INDIRECT_CODES` names each with its producer file, and the code
counts as used while that file still contains it. `--list` prints every used code; `--seed` prints
the missing ones as catalog rows with a humanized compact text and an empty
full text, for a human to complete.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "manyforge_core/resources/error_catalog.tsv"

_CODE = r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+"
_ERROR_NAME = r"[A-Za-z_]\w*(?:Error|Failure|Refusal|Rejection|Exception)"
# Helpers whose literal argument is an error code (C++ and Python).
_CALLS = (
    "fail",
    "invalid",
    "reject",
    "refuse",
    "refused",
    "failure",
    "coordinator_error",
    "stage_error",
    "ErrorResponse",
    "DetailedErrorResponse",
    "ApplyErrorResponse",
    "ApplyKnownNoEffectErrorResponse",
    "ApplyErrorResponseFromJournalEvidence",
    "RetainTerminalFailure",
    "retain_pre_effect_failure",
    "TypedError",
    "RequestInvalid",
)
# The literal may follow up to three non-literal arguments (for example
# `RetainTerminalFailure(journal, intent, "code", ...)`).
_CALL = re.compile(
    r"\b(?:" + _ERROR_NAME + r"|_?(?:" + "|".join(_CALLS) + r"))\s*\(\s*"
    r"(?:[A-Za-z_][\w:.\->*&]*\s*,\s*){0,3}"
    r"(?:std::string\s*\(\s*)?[\"'](" + _CODE + r")[\"']"
)
_PATTERNS = {
    ".cpp": (
        _CALL,
        re.compile(r"ErrorDTO\s*\{\s*(?:std::string\s*\(\s*)?\"(" + _CODE + r")\""),
        re.compile(r"\bk\w*(?:Error|ReasonCode|Code)\w*\s*=\s*\"(" + _CODE + r")\""),
        re.compile(r"\{\s*\"code\"\s*,\s*(?:CanonicalValue\s*\(\s*)?\"(" + _CODE + r")\""),
    ),
    ".py": (
        _CALL,
        re.compile(r"[\"']code[\"']\s*:\s*[\"'](" + _CODE + r")[\"']"),
        re.compile(r"\bcode\s*=\s*[\"'](" + _CODE + r")[\"']"),
    ),
    ".ts": (
        re.compile(r"\b[cC]ode\s*[!=]==\s*'(" + _CODE + r")'"),
        re.compile(r"'(" + _CODE + r")'\s*[!=]==\s*[\w.?]*[cC]ode\b"),
    ),
}
_PATTERNS[".hpp"] = _PATTERNS[".cpp"]
# Producer file -> codes it produces at a site no pattern above can see.
_INDIRECT_CODES = {
    "manyforge_behavior/manyforge_behavior/program_run_owner.py": (
        "selected_motion_stop_settling",
    ),
    "manyforge_core/include/manyforge_core/types/reason_codes.hpp": (
        "terminal_cleanup_pending",
    ),
    "manyforge_core/include/manyforge_core/types/carried_object_lost.hpp": (
        "carried_object_lost",
    ),
}
_PATTERNS[".tsx"] = _PATTERNS[".ts"]

_SKIP_PARTS = {
    "node_modules",
    "build",
    "install",
    "log",
    "third_party",
    "test",
    "tests",
    "process_tests",
    "__pycache__",
    ".manyforge-dev",
    "qualification",
    "verification",
    "examples",
}


def _is_test(path: Path) -> bool:
    name = path.name
    return (
        name.startswith("test_")
        or name.endswith(("_test.py", ".test.ts", ".test.tsx", "_test.cpp"))
        or name == "conftest.py"
    )


def source_files(root: Path = ROOT):
    for suffix in _PATTERNS:
        for path in root.rglob(f"*{suffix}"):
            relative = path.relative_to(root)
            if _SKIP_PARTS.intersection(relative.parts) or _is_test(path):
                continue
            if relative.parts[0] in {"scripts", "docs", "tools"}:
                continue
            yield path


def used_codes(root: Path = ROOT) -> dict[str, str]:
    """Every used code with the first file that uses it."""
    found: dict[str, str] = {}
    for path in sorted(source_files(root)):
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in _PATTERNS[path.suffix]:
            for match in pattern.finditer(text):
                found.setdefault(match.group(1), str(path.relative_to(root)))
    for relative, codes in _INDIRECT_CODES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for code in codes:
            if f'"{code}"' in text:
                found.setdefault(code, relative)
    return found


def catalog_codes(path: Path = CATALOG) -> dict[str, tuple[str, str]]:
    rows: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) != 3 or not re.fullmatch(_CODE, columns[0]) or not columns[1]:
            raise SystemExit(f"{path}:{number}: expected code<TAB>compact<TAB>full")
        if columns[0] in rows:
            raise SystemExit(f"{path}:{number}: duplicate code {columns[0]}")
        rows[columns[0]] = (columns[1], columns[2])
    return rows


def humanized(code: str) -> str:
    text = code.replace("_", " ")
    return text[:1].upper() + text[1:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list", action="store_true", help="print every used code")
    mode.add_argument("--seed", action="store_true", help="print missing codes as rows")
    args = parser.parse_args()
    used = used_codes()
    if args.list:
        for code in sorted(used):
            print(f"{code}\t{used[code]}")
        return 0
    catalog = catalog_codes()
    missing = sorted(set(used) - set(catalog))
    if args.seed:
        for code in missing:
            print(f"{code}\t{humanized(code)}\t")
        return 0
    if missing:
        for code in missing:
            print(f"error code {code!r} (used in {used[code]}) is missing from "
                  f"{CATALOG.relative_to(ROOT)}", file=sys.stderr)
        return 1
    print(f"error catalog covers all {len(used)} used codes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
