"""Scenario-based qualification support for ManyForge."""

from .runner import (
    archive_artifacts,
    expand_matrix_scenarios,
    format_scenario_condition_failure,
    match_scenario_expectation,
    resolve_scenario_payload_path,
    scenario_condition_summary,
    ScenarioAssertionError,
    ScenarioDefinition,
    ScenarioExecutionError,
    ScenarioExecutionUnavailableError,
    ScenarioMatrixDefinition,
    ScenarioRunner,
    load_matrix_definition,
    load_scenario_definition,
    write_matrix_artifacts,
    write_scenario_artifacts,
)

__all__ = [
    "ScenarioAssertionError",
    "ScenarioDefinition",
    "ScenarioExecutionError",
    "ScenarioExecutionUnavailableError",
    "ScenarioMatrixDefinition",
    "ScenarioRunner",
    "archive_artifacts",
    "expand_matrix_scenarios",
    "format_scenario_condition_failure",
    "load_matrix_definition",
    "load_scenario_definition",
    "match_scenario_expectation",
    "resolve_scenario_payload_path",
    "scenario_condition_summary",
    "write_matrix_artifacts",
    "write_scenario_artifacts",
]
