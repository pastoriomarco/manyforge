"""Artifact-owned scenario discovery contracts."""

from __future__ import annotations

from pathlib import Path

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.scenarios import (
    Scenario,
    ScenarioCatalog,
    ScenarioDiscoveryUnavailableError,
    configured_scenario_root_values,
    discover_scenarios,
)


def test_scenario_discovery_is_fenced_before_reading_roots(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()

    with pytest.raises(
        ScenarioDiscoveryUnavailableError,
        match="composable_cell_scenario_discovery_unavailable",
    ):
        discover_scenarios(
            [tmp_path / "missing"],
            node_environment=environment,
        )


def test_pure_scenario_catalog_alias_resolution_remains_available(
    tmp_path: Path,
) -> None:
    scenario = Scenario(
        name="canonical-cell",
        lane="real-robot",
        assistant="off",
        program_path=tmp_path / "cell.program.yaml",
        deployment_path=tmp_path / "cell.deployment.yaml",
        description="metadata only",
        aliases=("cell",),
    )
    catalog = ScenarioCatalog([scenario])

    resolved, deprecated = catalog.resolve("cell")

    assert resolved is scenario
    assert deprecated is True


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
