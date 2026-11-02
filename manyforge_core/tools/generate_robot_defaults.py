#!/usr/bin/env python3
"""Render the component runtime defaults into C++ constants and the backend tables.

The one source is `manyforge_core/resources/robot_defaults.tsv` (`table`,
`field`, JSON `value`, C++ constant): the effective value of a robot's (or,
table `component`, any lifecycle component's) Deployment field when the
Deployment leaves it out. The outputs are
generated, checked in and kept fresh by `scripts/regen.py --check`; nothing
reads the TSV at runtime.

- C++: `manyforge::core::k<Default...>` constants (a string value is the
  enum's name; its header turns it into the enumerator).
- Python: the backend's `ROBOT_RUNTIME_DEFAULTS`, `ROBOT_OPTIONAL_DEFAULTS`,
  `REAL_ROBOT_OPTIONAL_DEFAULTS` and `COMPONENT_DEFAULTS`
  (`GET /api/deployment/effective`).

A field whose ros2_control runtime closed schema declares a materialized
default must have the same value here (`--check` and the write both refuse a
difference).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

import yaml

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "manyforge_core/resources/robot_defaults.tsv"
SCHEMA = (ROOT / "manyforge_deployment/resources/closed_schema/value/"
          "manyforge.value_schema.runtime.ros2_control_execution.v1.yaml")
CPP = ROOT / "manyforge_core/include/manyforge_core/types/robot_defaults.generated.hpp"
PYTHON = ROOT / "manyforge_composer/backend/robot_defaults.py"

TABLES = {
    "robot_runtime": "ROBOT_RUNTIME_DEFAULTS",
    "robot_optional": "ROBOT_OPTIONAL_DEFAULTS",
    "real_robot_optional": "REAL_ROBOT_OPTIONAL_DEFAULTS",
    "component": "COMPONENT_DEFAULTS",
}
_FIELD = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*")
_CONSTANT = re.compile(r"kDefault[A-Z][A-Za-z0-9]*")
_HEADER = ("Generated from manyforge_core/resources/robot_defaults.tsv by "
           "manyforge_core/tools/generate_robot_defaults.py; do not edit.")

Row = tuple[str, str, object, str]


def read_rows(path: Path = SOURCE) -> list[Row]:
    rows: list[Row] = []
    fields: set[str] = set()
    constants: set[str] = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if (len(columns) != 4 or columns[0] not in TABLES or not _FIELD.fullmatch(columns[1])
                or not _CONSTANT.fullmatch(columns[3])):
            raise SystemExit(f"{path}:{number}: expected table<TAB>field<TAB>value<TAB>kDefault...")
        value = json.loads(columns[2])
        if not isinstance(value, (bool, int, float, str)):
            raise SystemExit(f"{path}:{number}: a value is a number, a boolean or a string")
        if isinstance(value, int) and not isinstance(value, bool) and value < 0:
            raise SystemExit(f"{path}:{number}: an integer default is unsigned")
        if columns[1] in fields or columns[3] in constants:
            raise SystemExit(f"{path}:{number}: duplicate field or constant")
        fields.add(columns[1])
        constants.add(columns[3])
        rows.append((columns[0], columns[1], value, columns[3]))
    return rows


def schema_defaults(path: Path = SCHEMA) -> dict[str, object]:
    """field_id -> value of every materialized default in the schema."""
    found: dict[str, object] = {}

    def walk(node: object) -> None:
        if isinstance(node, dict):
            default = node.get("default")
            if ("field_id" in node and isinstance(default, dict)
                    and default.get("kind") == "materialized"):
                found[node["field_id"]] = default.get("value")
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(yaml.safe_load(path.read_text(encoding="utf-8")))
    return found


def check_schema(rows: list[Row]) -> list[str]:
    materialized = schema_defaults()
    return [
        f"{field}: {SOURCE.relative_to(ROOT)} has {value!r}, "
        f"{SCHEMA.relative_to(ROOT)} materializes {materialized[field]!r}"
        for _table, field, value, _constant in rows
        if field in materialized and materialized[field] != value
    ]


def _cpp_constant(value: object, name: str) -> str:
    if isinstance(value, bool):
        return f"inline constexpr bool {name} = {'true' if value else 'false'};"
    if isinstance(value, int):
        return f"inline constexpr std::uint32_t {name} = {value}U;"
    if isinstance(value, float):
        return f"inline constexpr double {name} = {value!r};"
    return f'inline constexpr std::string_view {name}{{"{value}"}};'


def render_cpp(rows: list[Row]) -> str:
    lines = [
        f"// {_HEADER}",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace manyforge::core {",
        "",
        "/// A Deployment component's runtime defaults: the value of each field the",
        "/// Deployment leaves out (the GUI shows the same table as \"Default\").",
    ]
    for _table, field, value, name in rows:
        lines.append(f"/// `{field}`")
        lines.append(_cpp_constant(value, name))
    lines += ["", "}  // namespace manyforge::core", ""]
    return "\n".join(lines)


def render_python(rows: list[Row]) -> str:
    lines = [f'"""{_HEADER}"""', "", "from typing import Any"]
    for table, variable in TABLES.items():
        lines += ["", f"{variable}: dict[str, Any] = {{"]
        lines += [f"    {json.dumps(field)}: {json.dumps(value) if isinstance(value, str) else repr(value)},"
                  for row_table, field, value, _name in rows if row_table == table]
        lines.append("}")
    return "\n".join(lines) + "\n"


def outputs() -> dict[Path, str]:
    rows = read_rows()
    return {CPP: render_cpp(rows), PYTHON: render_python(rows)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when an output is stale")
    args = parser.parse_args()
    differences = check_schema(read_rows())
    for difference in differences:
        print(difference, file=sys.stderr)
    stale = []
    for path, text in outputs().items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.check:
            stale.append(path.relative_to(ROOT))
        else:
            path.write_text(text, encoding="utf-8")
    for path in stale:
        print(f"{path} is stale; run manyforge_core/tools/generate_robot_defaults.py",
              file=sys.stderr)
    return 1 if stale or differences else 0


if __name__ == "__main__":
    sys.exit(main())
