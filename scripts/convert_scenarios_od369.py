#!/usr/bin/env python3
"""Convert Scenarios saved before OD-369 to the current file format.

    python3 scripts/convert_scenarios_od369.py <Scenario folder | Program file> [...]
    python3 scripts/convert_scenarios_od369.py --dry-run <path> [...]

A Scenario has one identity, its folder name (`<name>.scenario`), and one
description, in `scenario.yaml`. This script removes the fields that used to
name or describe its parts:

- `program.yaml`: `program_id`, `name`, `description`;
- `deployment.yaml`: `deployment_artifact_id`, `name`, `description`.

It also renames the retired Program profile in `program_profile`:
`composable-cell-v1` becomes `manyforge-cell-v1`, and removes the lane labels:
the Program's `compatibility.lanes` and the Deployment's `compatibility.lane`.

A Scenario folder is rewritten in place the way Save writes it: its members
and `scenario.yaml` with the member digests and the checked-content digest
recomputed by the Composer's own functions. In a standalone `*.program.yaml`
file only the top-level lines of those fields change, and its
`program_revision`, when present, is recomputed for the new content. Paths
already in the current format are left alone. When a path cannot be
converted, the script names it, writes nothing for it and exits with
status 1.

The conversion is `manyforge_composer/backend/scenario_convert_od369.py`,
which the Composer's Load dialog also runs (Upgrade and load). Run the script
where the ManyForge Python packages import (the workbench or dev image).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
for _path in (ROOT, ROOT / "manyforge_behavior"):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from manyforge_behavior.canonical_artifact import StrictArtifactError  # noqa: E402
from manyforge_composer.backend import scenario_artifacts as sa  # noqa: E402
from manyforge_composer.backend.scenario_convert_od369 import (  # noqa: E402
    convert_program_file,
    convert_scenario,
)


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
        changes = convert_path(path, dry_run=args.dry_run)
        if changes is None:
            failed = True
        elif changes:
            verb = "would convert" if args.dry_run else "converted"
            print(f"{path}: {verb}: {'; '.join(changes)}")
        else:
            print(f"{path}: already current")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
