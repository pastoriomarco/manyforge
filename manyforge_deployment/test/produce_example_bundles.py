#!/usr/bin/env python3
"""Compile every example Scenario's Deployment into its retained bundle.

Test fixture producer for the component build spec tests (parity with the earlier
identity, collision-exclusion edits): for each `examples/*.scenario`, the
ordinary activation preparer compiles the Deployment without a Program (the
bundle does not depend on it), except that a Deployment with a global rule
scoped to execution groups is compiled with its Scenario's Program, which
resolves those groups; `<output>/<scenario>.json` receives the sealed retained
bundle as RFC 8785 canonical JSON, and `<output>/packages/<scenario>/activation`
keeps its retained package (model map, planning model and model sources) for
the description parity test. Each compile is also checked: every arm's
controller profile carries its authored operator Stop decelerations, the same
values as its compiled Stop rows.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from manyforge_behavior.canonical_artifact import (  # noqa: E402
    canonical_json_bytes,
    load_strict_json_yaml,
)
from prepare_activation_tool import (  # noqa: E402
    _rehash_request,
    _request,
    _require_launched_stop_decelerations,
    _run,
    _scenario_request,
)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    args.workspace = args.workspace.resolve()
    shutil.rmtree(args.output, ignore_errors=True)
    args.output.mkdir(parents=True)
    scenarios = sorted((args.workspace / "examples").glob("*.scenario"))
    if not scenarios:
        raise AssertionError("no example Scenario found")
    packages = args.output / "packages"
    packages.mkdir()
    for scenario in scenarios:
        deployment = load_strict_json_yaml(scenario / "deployment.yaml")
        rules = deployment.get("policies", {}).get("interrupt_rules", [])
        if any(isinstance(rule["scope"], dict) for rule in rules):
            _, _, request = _scenario_request(args.workspace, str(scenario))
            prepared = _run(args, request, packages / scenario.stem)
        else:
            manifest = load_strict_json_yaml(scenario / "scenario.yaml")
            document = {
                "scenario_id": manifest["scenario_id"],
                "deployment_draft": deployment,
                "program_role_assignment": {"component_roles": [], "target_roles": []},
            }
            request = _request(scenario, document, {})
            request["program"] = None
            _rehash_request(request)
            prepared = _run(
                args,
                request,
                packages / scenario.stem,
                expected_program_outcome_kind="no_program",
            )
        assert prepared is not None
        _require_launched_stop_decelerations(args.workspace, deployment, prepared)
        bundle = prepared["activation_payload"]["retained_bundle"]
        (args.output / f"{scenario.stem}.json").write_bytes(canonical_json_bytes(bundle))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
