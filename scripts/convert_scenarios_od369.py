#!/usr/bin/env python3
"""Convert Scenarios saved before OD-369 to the current file format.

    python3 scripts/convert_scenarios_od369.py <Scenario folder | Program file> [...]
    python3 scripts/convert_scenarios_od369.py --dry-run <path> [...]

A Scenario has one identity, its folder name (`<name>.scenario`), and one
description, in `scenario.yaml`. This script removes the fields that used to
name or describe its parts:

- `program.yaml`: `program_id`, `name`, `description`;
- `deployment.yaml`: `deployment_artifact_id`, `name`, `description`.

A Scenario folder is rewritten in place the way Save writes it: its members
and `scenario.yaml` with the member digests and the checked-content digest
recomputed by the Composer's own functions. A standalone `*.program.yaml`
file only loses the top-level lines of those fields; its `program_revision`
is then stale and `python3 scripts/regen.py --update --build-dir <build>`
recomputes it. Paths already in the current format are left alone. When a
path cannot be converted, the script names it, writes nothing for it and
exits with status 1.

Run it where the ManyForge Python packages import (the workbench or dev
image).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
for _path in (ROOT, ROOT / "manyforge_behavior"):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from manyforge_behavior.canonical_artifact import (  # noqa: E402
    StrictArtifactError,
    load_strict_json_yaml_bytes,
)
from manyforge_composer.backend import scenario_artifacts as sa  # noqa: E402

DROPPED = {
    "program_draft": ("program_id", "name", "description"),
    "deployment_draft": ("deployment_artifact_id", "name", "description"),
}


def _strip(value: Any, fields: tuple[str, ...]) -> list[str]:
    if not isinstance(value, dict):
        return []
    found = [field for field in fields if field in value]
    for field in found:
        del value[field]
    return found


def convert_scenario(project: Path, *, dry_run: bool) -> list[str]:
    manifest_path = project / sa.SCENARIO_MANIFEST
    manifest = load_strict_json_yaml_bytes(
        manifest_path.read_bytes(), source=str(manifest_path)
    )
    values: dict[str, Any] = {}
    removed: list[str] = []
    for member, filename in sa.SCENARIO_PROJECT_MEMBERS.items():
        path = project / filename
        values[member] = sa._load_project_member(
            path.read_bytes(), source=str(path), nullable=True
        )
        removed += [f"{filename}: {field}"
                    for field in _strip(values[member], DROPPED[member])]
    if not removed:
        return []
    document = {
        "api_version": sa.SCENARIO_API_VERSION,
        "scenario_id": manifest["scenario_id"],
        "name": manifest["name"],
        "description": manifest["description"],
        **values,
        "saved_status": {
            **manifest["saved_status"],
            "checked_content_digest": sa.scenario_content_digest(
                values["program_draft"], values["deployment_draft"]
            ),
        },
    }
    # The same encoding, member digests and manifest as Save.
    member_bytes, manifest_raw = sa._project_payload(
        document, source_snapshot=manifest["source_snapshot"]
    )
    if not dry_run:
        for filename, raw in sorted(member_bytes.items()):
            (project / filename).write_bytes(raw)
        manifest_path.write_bytes(manifest_raw)
    return removed


def convert_program_file(path: Path, *, dry_run: bool) -> list[str]:
    raw = path.read_bytes()
    before = load_strict_json_yaml_bytes(raw, source=str(path))
    fields = DROPPED["program_draft"]
    removed = [field for field in fields if field in before]
    if not removed:
        return []
    kept: list[str] = []
    dropping = False
    for line in raw.decode("utf-8").splitlines(keepends=True):
        if line[:1] not in (" ", "\t", "\n", "\r", "") or line.startswith("#"):
            dropping = line.split(":", 1)[0] in removed
        if not dropping:
            kept.append(line)
    text = "".join(kept)
    after = load_strict_json_yaml_bytes(text.encode("utf-8"), source=str(path))
    expected = {key: value for key, value in before.items() if key not in removed}
    if after != expected:
        raise ValueError("the top-level fields could not be removed line by line")
    if not dry_run:
        path.write_text(text, encoding="utf-8")
    return removed


def convert_path(path: Path, *, dry_run: bool) -> list[str] | None:
    try:
        if path.is_dir() and path.name.endswith(sa.SCENARIO_PROJECT_SUFFIX):
            return convert_scenario(path, dry_run=dry_run)
        if path.is_file() and path.name.endswith((".program.yaml", ".program.yml")):
            return convert_program_file(path, dry_run=dry_run)
        print(f"{path}: not a Scenario folder or Program file", file=sys.stderr)
    except (OSError, StrictArtifactError, sa.ScenarioArtifactError, KeyError,
            ValueError) as error:
        print(f"{path}: not converted: {error}", file=sys.stderr)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would change, write nothing")
    args = parser.parse_args(argv)
    failed = False
    for path in args.paths:
        removed = convert_path(path, dry_run=args.dry_run)
        if removed is None:
            failed = True
        elif removed:
            verb = "would remove" if args.dry_run else "removed"
            print(f"{path}: {verb} {', '.join(removed)}")
        else:
            print(f"{path}: already current")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
