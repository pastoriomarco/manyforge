#!/usr/bin/env python3
"""Convert a Scenario saved before collision rules to the current file formats.

    python3 scripts/convert_scenario_od46.py <Scenario folder | Program file> [...]
    python3 scripts/convert_scenario_od46.py --dry-run <path> [...]

A Scenario folder is converted as a whole: `program.yaml` (Program v3 -> v4),
`deployment.yaml` (Deployment v2 -> v3) and the member digests in
`scenario.yaml`. A single Program file is converted alone. Files already in
the current format are left alone. When something cannot be converted, the
script names it, writes nothing for that path and exits with status 1.

What changes:

Deployment
- the robots' `collision_clearance_m` becomes the first collision rule
  (`collision_rules[0]`, `rule_id: everything`); 1 mm when no robot set it;
- gripper `touch_links` are removed (the component type's grip body replaces
  them).

Program
- a node's `clearance_m` becomes its collision rule without selections;
- a node's `contact_policy_id` (with `contact_scene_resource_id`) becomes
  collision rules on that node: a contact-grasp policy a `contact` rule, a
  contact-release policy a `release` rule; on a pick sequence the rule is for
  the descend leg;
- the Program's `contact_policies` list is removed;
- a Scene object's `support_contacts` become rules on single moves: a
  `release` rule on the lift (the first move after the object is attached, and
  the retreat leg of its pick sequence) and a `contact` rule on the last place
  move (the last move before the object is detached, and the descend leg of
  its place sequence);
- the node catalog pin becomes 4.0.0.

The conversion is `manyforge_composer/backend/scenario_convert_od46.py`, which
the Composer's Load dialog also runs (Upgrade and load). The script needs only
Python 3, PyYAML and the checkout.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from manyforge_composer.backend.scenario_convert_od46 import (  # noqa: E402
    ConversionError,
    convert_path,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert Scenario folders or Program files saved before collision rules.")
    parser.add_argument("paths", nargs="+", type=Path,
                        help="a Scenario folder (program.yaml, deployment.yaml) or a Program file")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would change and write nothing")
    arguments = parser.parse_args(argv)
    failed = False
    for path in arguments.paths:
        try:
            if not path.exists():
                raise ConversionError(f"{path}: no such file or folder")
            notes = convert_path(path, dry_run=arguments.dry_run)
        except (ConversionError, OSError, ValueError, yaml.YAMLError) as error:
            failed = True
            print(f"NOT CONVERTED {error}", file=sys.stderr)
            continue
        if notes is None:
            print(f"{path}: already in the current format")
            continue
        print(f"{path}: " + ("would convert" if arguments.dry_run else "converted"))
        for note in notes:
            print(f"  {note}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
