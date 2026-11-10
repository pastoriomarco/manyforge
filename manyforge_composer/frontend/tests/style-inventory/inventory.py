#!/usr/bin/env python3
"""Style inventory for the Composer frontend.

Reads the stylesheets and the TS/TSX sources and writes a JSON summary:
value histograms, near-duplicate colour groups, selectors defined more than
once per context, classes used without a rule, classes never referenced, and
colour literals in TS/TSX.

Usage: python3 tests/style-inventory/inventory.py <output.json>

It reads src/styles/index.css (following its @import lines) when present,
otherwise the historical single src/index.css.
"""

from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


def style_files() -> list[tuple[str, str]]:
    entry = SRC / "styles" / "index.css"
    if not entry.exists():
        return [("src/index.css", (SRC / "index.css").read_text(encoding="utf-8"))]
    files = []
    for match in re.finditer(r"@import\s+['\"]([^'\"]+)['\"]", entry.read_text(encoding="utf-8")):
        path = (SRC / "styles" / match.group(1)).resolve()
        files.append((str(path.relative_to(ROOT)), path.read_text(encoding="utf-8")))
    return files


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def selector_counts(text: str) -> Counter:
    counts: Counter = Counter()
    context, depth, buffer = "", 0, ""
    for character in strip_comments(text):
        if character == "{":
            prelude = buffer.strip()
            if prelude.startswith("@"):
                context = prelude
            elif not re.match(r"^(from|to|\d+%)", prelude):
                for selector in prelude.split(","):
                    counts[f"{context}|{' '.join(selector.split())}"] += 1
            depth += 1
            buffer = ""
        elif character == "}":
            depth -= 1
            if depth == 0:
                context = ""
            buffer = ""
        elif character == ";":
            buffer = ""
        else:
            buffer += character
    return counts


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python3 tests/style-inventory/inventory.py <output.json>", file=sys.stderr)
        return 2
    styles = style_files()
    css = "\n".join(strip_comments(text) for _, text in styles)

    def declarations(prop: str) -> list[str]:
        return [m.group(1).strip() for m in re.finditer(rf"(?<![-\w]){prop}\s*:\s*([^;}}]+)", css)]

    def histogram(values: list[str]) -> dict[str, int]:
        return dict(Counter(values).most_common())

    hexes = [m.group(0).lower() for m in re.finditer(r"#[0-9a-fA-F]{6}\b", css)]
    rgbs = re.findall(r"rgba?\([^)]*\)", css)
    groups: list[list[str]] = []
    for color in sorted(set(hexes)):
        channels = [int(color[i : i + 2], 16) for i in (1, 3, 5)]
        for group in groups:
            first = [int(group[0][i : i + 2], 16) for i in (1, 3, 5)]
            if all(abs(a - b) <= 8 for a, b in zip(first, channels)):
                group.append(color)
                break
        else:
            groups.append([color])

    counts: Counter = Counter()
    for _, text in styles:
        counts.update(selector_counts(text))
    duplicates = [{"selector": key, "count": count} for key, count in counts.items() if count > 1]

    defined = set(re.findall(r"\.([a-zA-Z_][\w-]*)", css))
    sources = [
        (str(path.relative_to(ROOT)), path.read_text(encoding="utf-8"))
        for path in sorted(SRC.rglob("*"))
        if path.suffix in {".ts", ".tsx"} and not re.search(r"\.test\.tsx?$", path.name)
    ]
    all_source = "\n".join(text for _, text in sources)
    used: dict[str, set[str]] = {}
    for path, text in sources:
        for match in re.finditer(r'className=(?:"([^"]*)"|\{`([^`]*)`\})', text):
            value = re.sub(r"\$\{[^}]*\}", " ", match.group(1) or match.group(2) or "")
            for token in value.split():
                if re.fullmatch(r"[a-z][\w]*(?:-[\w]+)+", token):
                    used.setdefault(token, set()).add(path)
    unstyled = [{"class": name, "files": sorted(files)} for name, files in sorted(used.items()) if name not in defined]
    unreferenced = sorted(
        name for name in defined if not re.search(rf"(?<![\w-]){re.escape(name)}(?![\w-])", all_source)
    )
    ts_colors = [
        {"file": path, "color": m.group(0)[1:-1].lower()}
        for path, text in sources
        for m in re.finditer(r"['\"`]#[0-9a-fA-F]{6}['\"`]", text)
    ]

    inventory = {
        "schema_version": "manyforge.composer_style_inventory.v1",
        "stylesheets": [{"path": path, "lines": text.count("\n") + 1} for path, text in styles],
        "counts": {
            "hex_distinct": len(set(hexes)),
            "hex_groups_within_8": len(groups),
            "rgb_distinct": len(set(rgbs)),
            "duplicate_selectors": len(duplicates),
            "classes_defined": len(defined),
            "classes_used_without_rule": len(unstyled),
            "classes_never_referenced": len(unreferenced),
            "ts_color_literals": len(ts_colors),
        },
        "histograms": {
            "font_size": histogram(declarations("font-size")),
            "font_weight": histogram(declarations("font-weight")),
            "border_radius": histogram(declarations("border-radius")),
            "gap": histogram(declarations("gap")),
            "z_index": histogram(declarations("z-index")),
        },
        "near_duplicate_color_groups": [group for group in groups if len(group) > 1],
        "duplicate_selectors": duplicates,
        "classes_used_without_rule": unstyled,
        "classes_never_referenced": unreferenced,
        "ts_color_literals": ts_colors,
    }
    Path(sys.argv[1]).write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(inventory["counts"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
