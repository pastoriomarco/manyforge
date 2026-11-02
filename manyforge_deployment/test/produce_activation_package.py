#!/usr/bin/env python3
"""Prepare Scenario projects into one package root (test fixture producer).

Each `--scenario <folder>=<cell_id>` runs the ordinary activation preparer
(`manyforge_prepare_activation`) on the Scenario's request under the named
runtime cell. All products land in the same content-addressed package root,
which the capture tests read.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from manyforge_behavior.canonical_artifact import canonical_json_bytes  # noqa: E402
from prepare_activation_tool import _scenario_request  # noqa: E402


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--package-root", required=True, type=Path)
    parser.add_argument("--scenario", required=True, action="append")
    parser.add_argument("--publish-canonical-empty", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    shutil.rmtree(args.package_root, ignore_errors=True)
    args.package_root.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as scratch:
        for entry in args.scenario:
            scenario, _, cell_id = entry.partition("=")
            _, _, request = _scenario_request(args.workspace, scenario)
            request["runtime_cell_id"] = cell_id
            request_path = Path(scratch) / "request.json"
            request_path.write_bytes(canonical_json_bytes(request))
            command = [
                str(args.tool),
                "--input", str(request_path),
                "--capability-root", str(args.workspace),
                "--activation-root", str(args.package_root),
            ]
            if args.publish_canonical_empty:
                command.append("--publish-canonical-empty")
            result = subprocess.run(command, check=False, capture_output=True, text=True)
            if result.returncode != 0:
                sys.stderr.write(f"{scenario}: {result.stderr}")
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
