#!/usr/bin/env python3
"""Exercise the maintained catalog through the production Python/native seam."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
import time
from pathlib import Path

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.scenarios import (
    NativeScenarioArtifactClosureVerifier,
    discover_scenarios,
)


_EXPECTED_SCENARIOS = (
    "lite6_pick_and_place",
    "ur10e_assistant_scene_authoring",
    "ur10e_pick_and_place",
    "ur5_pick_and_place",
    "ur5_pick_and_place_minimal",
    "ur5_semantic_shape_pick",
    "ur5_vision_guided_bin_pick",
    "ur5_vision_guided_pose_pick",
)

_EXPECTED_ALIASES = {
    "assistant-authoring": "ur10e_assistant_scene_authoring",
    "lite6-pick-place": "lite6_pick_and_place",
    "pick-place": "ur5_pick_and_place",
    "shape-pick": "ur5_semantic_shape_pick",
    "ur10e": "ur10e_pick_and_place",
    "ur10e-pick-place": "ur10e_pick_and_place",
    "ur10e-scene-authoring": "ur10e_assistant_scene_authoring",
    "ur5-pick-place": "ur5_pick_and_place",
    "ur5-semantic-shape-pick": "ur5_semantic_shape_pick",
    "ur5-simple-pick-place": "ur5_pick_and_place_minimal",
    "ur5-vision-pose-pick": "ur5_vision_guided_pose_pick",
    "vision-pose": "ur5_vision_guided_pose_pick",
}


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    parser.add_argument("--prepared", action="append", required=True)
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    if len(arguments.prepared) != 3:
        raise ValueError("maintained discovery requires three prepared families")
    with tempfile.TemporaryDirectory(prefix="manyforge-n6-a3-discovery-") as raw_root:
        root = Path(raw_root)
        command = [
            str(arguments.producer),
            "--check",
            "--workspace-root",
            str(arguments.workspace_root),
            "--package-root",
            str(root),
        ]
        for prepared in arguments.prepared:
            command.extend(("--prepared", prepared))
        subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        environment = build_node_environment()
        discovery_started = time.perf_counter()
        catalog = discover_scenarios(
            [root],
            node_environment=environment,
            closure_verifier=NativeScenarioArtifactClosureVerifier(environment),
        )
        discovery_elapsed_s = time.perf_counter() - discovery_started
        names = tuple(scenario.name for scenario in catalog.scenarios)
        if names != _EXPECTED_SCENARIOS:
            raise ValueError(f"maintained scenario set changed: {names!r}")
        if any(scenario.lane != "manyforge-only" for scenario in catalog.scenarios):
            raise ValueError("maintained scenario lane projection changed")
        if sum(scenario.assistant == "on" for scenario in catalog.scenarios) != 4:
            raise ValueError("maintained scenario assistant projection changed")
        aliases = {
            alias: scenario.name
            for scenario in catalog.scenarios
            for alias in scenario.aliases
        }
        if aliases != _EXPECTED_ALIASES:
            raise ValueError(f"maintained scenario aliases changed: {aliases!r}")
        for alias, expected_name in _EXPECTED_ALIASES.items():
            resolved, deprecated = catalog.resolve(alias)
            if resolved.name != expected_name or deprecated is not True:
                raise ValueError(f"maintained scenario alias does not resolve: {alias}")
        print(f"maintained_scenario_discovery_elapsed_s={discovery_elapsed_s:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
