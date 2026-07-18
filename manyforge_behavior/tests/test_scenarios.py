"""Artifact-owned scenario discovery contracts."""

from __future__ import annotations

from pathlib import Path

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.scenarios import (
    ScenarioDiscoveryError,
    configured_scenario_root_values,
    discover_scenarios,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOG_PATH = REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"


def test_shipped_scenario_inventory_and_aliases_are_artifact_owned() -> None:
    environment = build_node_environment(CATALOG_PATH)
    catalog = discover_scenarios(
        [REPO_ROOT / "examples"],
        node_environment=environment,
    )

    assert {scenario.name for scenario in catalog.scenarios} == {
        "isaac-lite6-pick-place",
        "isaac-lite6-trocar-foundationpose",
        "isaac-ur10e-light-scene",
        "isaac-ur10e-light-scene-assistant",
        "isaac-ur10e-light-scene-collision",
        "lite6-pick-place",
        "ur10e-pick-place",
        "ur10e-scene-authoring",
        "ur5-pick-place",
        "ur5-semantic-shape-pick",
        "ur5-simple-pick-place",
        "ur5-vision-pose-pick",
    }
    aliases = {
        alias: scenario.name
        for scenario in catalog.scenarios
        for alias in scenario.aliases
    }
    assert aliases == {
        "assistant-authoring": "ur10e-scene-authoring",
        "isaac-assistant": "isaac-ur10e-light-scene-assistant",
        "isaac-light": "isaac-ur10e-light-scene",
        "pick-place": "ur5-pick-place",
        "shape-pick": "ur5-semantic-shape-pick",
        "ur10e": "ur10e-pick-place",
        "vision-pose": "ur5-vision-pose-pick",
    }
    for alias, canonical in aliases.items():
        resolved, deprecated = catalog.resolve(alias)
        assert deprecated is True
        assert resolved.name == canonical


def test_missing_scenario_root_is_a_structured_discovery_error(
    tmp_path: Path,
) -> None:
    environment = build_node_environment(CATALOG_PATH)

    with pytest.raises(ScenarioDiscoveryError, match="scenario root does not exist"):
        discover_scenarios(
            [tmp_path / "missing"],
            node_environment=environment,
        )


def test_scenario_root_setting_has_one_shared_process_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(
        "MANYFORGE_SCENARIO_ROOTS",
        "/workspace/site-a:/opt/site-b",
    )
    # The removed pre-cutover spelling must not affect either launcher path.
    monkeypatch.setenv("MANYFORGE_SCENARIO_PATHS", "/wrong/legacy/path")

    assert configured_scenario_root_values() == (
        "/workspace/site-a",
        "/opt/site-b",
    )
