"""Artifact-owned named scenario discovery and compatibility checks."""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path
import re
from typing import Iterable

import yaml

from manyforge_behavior.deployment import Deployment, DeploymentValidationError
from manyforge_behavior.node_environment import (
    NodeEnvironment,
    build_node_environment,
    configured_node_roots,
    validate_environment_availability,
)
from manyforge_behavior.program import Program, ProgramValidationError


class ScenarioDiscoveryError(ValueError):
    """Scenario artifacts are malformed or mutually incompatible."""


def configured_scenario_root_values(value: str | None = None) -> tuple[str, ...]:
    """Return the one process-level scenario-root setting without defaults."""
    raw = os.environ.get("MANYFORGE_SCENARIO_ROOTS", "") if value is None else value
    return tuple(item for item in raw.split(os.pathsep) if item)


@dataclass(frozen=True)
class Scenario:
    name: str
    lane: str
    assistant: str
    program_path: Path
    deployment_path: Path
    description: str
    aliases: tuple[str, ...] = ()
    runtime_resolver: str = ""
    cell_path: Path | None = None


class ScenarioCatalog:
    def __init__(self, scenarios: Iterable[Scenario]):
        by_name: dict[str, Scenario] = {}
        aliases: dict[str, str] = {}
        for scenario in scenarios:
            if scenario.name in by_name or scenario.name in aliases:
                raise ScenarioDiscoveryError(
                    f"duplicate scenario identifier '{scenario.name}'"
                )
            by_name[scenario.name] = scenario
            for alias in scenario.aliases:
                if alias in by_name or alias in aliases:
                    raise ScenarioDiscoveryError(
                        f"duplicate scenario identifier '{alias}'"
                    )
                aliases[alias] = scenario.name
        self._by_name = by_name
        self._aliases = aliases

    @property
    def scenarios(self) -> tuple[Scenario, ...]:
        return tuple(self._by_name.values())

    def resolve(self, name: str) -> tuple[Scenario, bool]:
        canonical = self._aliases.get(name, name)
        try:
            return self._by_name[canonical], canonical != name
        except KeyError as exc:
            raise KeyError(name) from exc


def _resolve_program_path(deployment_path: Path, value: str) -> Path:
    if Path(value).is_absolute():
        raise ScenarioDiscoveryError(
            f"{deployment_path}: scenario program path must be relative"
        )
    try:
        candidate = (deployment_path.parent / value).resolve(strict=True)
    except OSError as exc:
        raise ScenarioDiscoveryError(
            f"{deployment_path}: scenario program does not exist: {value}"
        ) from exc
    try:
        candidate.relative_to(deployment_path.parent.resolve())
    except ValueError as exc:
        raise ScenarioDiscoveryError(
            f"{deployment_path}: scenario program escapes deployment directory"
        ) from exc
    if not candidate.is_file():
        raise ScenarioDiscoveryError(f"scenario program is not a file: {candidate}")
    return candidate


_PORTABLE_ID = re.compile(r"^[A-Za-z][A-Za-z0-9_.:-]{0,127}$")
_SCENARIO_ID = re.compile(r"^[a-z0-9][a-z0-9-]*$")


def _resolve_cell_relative_path(
    cell_path: Path,
    value: object,
    *,
    field: str,
) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ScenarioDiscoveryError(
            f"{cell_path}: {field} must be a non-empty relative path"
        )
    relative = Path(value)
    if relative.is_absolute():
        raise ScenarioDiscoveryError(
            f"{cell_path}: {field} must be relative"
        )
    try:
        candidate = (cell_path.parent / relative).resolve(strict=True)
    except OSError as exc:
        raise ScenarioDiscoveryError(
            f"{cell_path}: {field} does not exist: {value}"
        ) from exc
    try:
        candidate.relative_to(cell_path.parent.resolve())
    except ValueError as exc:
        raise ScenarioDiscoveryError(
            f"{cell_path}: {field} escapes the cell directory"
        ) from exc
    if not candidate.is_file():
        raise ScenarioDiscoveryError(
            f"{cell_path}: {field} is not a file: {value}"
        )
    return candidate


def _real_robot_cell_scenario(
    cell_path: Path,
    *,
    node_environment: NodeEnvironment,
) -> Scenario:
    try:
        raw = yaml.safe_load(cell_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ScenarioDiscoveryError(
            f"{cell_path}: real-robot cell YAML is invalid: {exc}"
        ) from exc
    if not isinstance(raw, dict):
        raise ScenarioDiscoveryError(
            f"{cell_path}: real-robot cell must be a mapping"
        )
    unknown = sorted(set(raw) - {"schema", "cell_id", "adapter", "scenario"})
    if unknown:
        raise ScenarioDiscoveryError(
            f"{cell_path}: real-robot cell has unknown fields: {unknown}"
        )
    if raw.get("schema") != "manyforge.real_robot_cell.v1":
        raise ScenarioDiscoveryError(
            f"{cell_path}: unsupported real-robot cell schema"
        )
    cell_id = raw.get("cell_id")
    if not isinstance(cell_id, str) or not _PORTABLE_ID.fullmatch(cell_id):
        raise ScenarioDiscoveryError(
            f"{cell_path}: cell_id must be a portable identifier"
        )
    adapter = raw.get("adapter")
    if not isinstance(adapter, dict):
        raise ScenarioDiscoveryError(
            f"{cell_path}: adapter must be a mapping"
        )
    unknown_adapter = sorted(set(adapter) - {"resolver_id", "config"})
    if unknown_adapter:
        raise ScenarioDiscoveryError(
            f"{cell_path}: adapter has unknown fields: {unknown_adapter}"
        )
    resolver_id = adapter.get("resolver_id")
    if (
        not isinstance(resolver_id, str)
        or not _PORTABLE_ID.fullmatch(resolver_id)
    ):
        raise ScenarioDiscoveryError(
            f"{cell_path}: adapter.resolver_id must be a portable identifier"
        )
    if not isinstance(adapter.get("config"), dict):
        raise ScenarioDiscoveryError(
            f"{cell_path}: adapter.config must be a mapping"
        )
    scenario = raw.get("scenario")
    if not isinstance(scenario, dict):
        raise ScenarioDiscoveryError(
            f"{cell_path}: scenario must be a mapping"
        )
    unknown_scenario = sorted(
        set(scenario)
        - {
            "name",
            "program",
            "deployment_template",
            "description",
            "aliases",
        }
    )
    if unknown_scenario:
        raise ScenarioDiscoveryError(
            f"{cell_path}: scenario has unknown fields: {unknown_scenario}"
        )
    name = scenario.get("name")
    description = scenario.get("description")
    if not isinstance(name, str) or not _SCENARIO_ID.fullmatch(name):
        raise ScenarioDiscoveryError(
            f"{cell_path}: scenario.name must use lowercase letters, "
            "digits, and hyphens"
        )
    if not isinstance(description, str) or not description.strip():
        raise ScenarioDiscoveryError(
            f"{cell_path}: scenario.description must be non-empty"
        )
    aliases_raw = scenario.get("aliases", [])
    if (
        not isinstance(aliases_raw, list)
        or not all(
            isinstance(alias, str) and _SCENARIO_ID.fullmatch(alias)
            for alias in aliases_raw
        )
        or len(aliases_raw) != len(set(aliases_raw))
    ):
        raise ScenarioDiscoveryError(
            f"{cell_path}: scenario.aliases must be unique scenario ids"
        )
    program_path = _resolve_cell_relative_path(
        cell_path,
        scenario.get("program"),
        field="scenario.program",
    )
    deployment_path = _resolve_cell_relative_path(
        cell_path,
        scenario.get("deployment_template"),
        field="scenario.deployment_template",
    )
    if not deployment_path.name.endswith(".deployment.template.yaml"):
        raise ScenarioDiscoveryError(
            f"{cell_path}: deployment template must end with "
            ".deployment.template.yaml"
        )
    try:
        deployment = Deployment(deployment_path)
        program = Program(program_path, node_environment.catalog)
        deployment.validate_against_program(program)
        deployment.validate_capabilities(program)
        validate_environment_availability(
            node_environment,
            deployment,
            program,
        )
        _validate_compatibility(program, deployment)
    except (
        ProgramValidationError,
        DeploymentValidationError,
        ScenarioDiscoveryError,
        ValueError,
    ) as exc:
        raise ScenarioDiscoveryError(
            f"{cell_path}: real-robot scenario is invalid: {exc}"
        ) from exc
    compatibility = deployment.compatibility
    if compatibility is None or compatibility.lane != "real-robot":
        raise ScenarioDiscoveryError(
            f"{cell_path}: deployment template must select real-robot"
        )
    if compatibility.scenarios:
        raise ScenarioDiscoveryError(
            f"{cell_path}: deployment-template scenarios must be empty; "
            "the cell artifact owns the runtime-resolved scenario"
        )
    assistant = (
        "on"
        if deployment.assistant and deployment.assistant.enabled
        else "off"
    )
    return Scenario(
        name=name,
        lane="real-robot",
        assistant=assistant,
        program_path=program_path,
        deployment_path=deployment_path,
        description=description.strip(),
        aliases=tuple(aliases_raw),
        runtime_resolver=resolver_id,
        cell_path=cell_path,
    )


def _validate_compatibility(program: Program, deployment: Deployment) -> None:
    program_compatibility = program.compatibility
    deployment_compatibility = deployment.compatibility
    if program_compatibility is None:
        raise ScenarioDiscoveryError(
            f"program '{program.name}' is named by a scenario but has no compatibility block"
        )
    if deployment_compatibility is None:
        raise ScenarioDiscoveryError(
            f"deployment '{deployment.name}' has no compatibility block"
        )
    if deployment_compatibility.lane not in program_compatibility.lanes:
        raise ScenarioDiscoveryError(
            f"program '{program.name}' does not allow lane "
            f"'{deployment_compatibility.lane}'"
        )
    assistant_enabled = bool(deployment.assistant and deployment.assistant.enabled)
    if program_compatibility.assistant == "required" and not assistant_enabled:
        raise ScenarioDiscoveryError(
            f"program '{program.name}' requires an enabled assistant"
        )
    if program_compatibility.assistant == "forbidden" and assistant_enabled:
        raise ScenarioDiscoveryError(
            f"program '{program.name}' forbids an enabled assistant"
        )
    missing = sorted(
        set(program_compatibility.requires) - set(deployment_compatibility.provides)
    )
    if missing:
        raise ScenarioDiscoveryError(
            f"deployment '{deployment.name}' is missing compatibility tokens {missing}"
        )


def discover_scenarios(
    roots: Iterable[str | Path],
    *,
    node_environment: NodeEnvironment,
) -> ScenarioCatalog:
    """Discover every deployment-owned scenario in deterministic path order."""
    scenarios: list[Scenario] = []
    deployment_paths: set[Path] = set()
    resolved_roots: list[Path] = []
    for root_value in roots:
        try:
            root = Path(root_value).resolve(strict=True)
        except OSError as exc:
            raise ScenarioDiscoveryError(
                f"scenario root does not exist: {root_value}"
            ) from exc
        if not root.is_dir():
            raise ScenarioDiscoveryError(
                f"scenario root is not a directory: {root}"
            )
        resolved_roots.append(root)
        for path in sorted(root.glob("*.deployment.yaml")):
            deployment_paths.add(path.resolve())
    cell_paths: set[Path] = set()
    for root in resolved_roots:
        for path in sorted(root.glob("*.real-robot-cell.yaml")):
            cell_paths.add(path.resolve())
    for deployment_path in sorted(deployment_paths):
        try:
            deployment = Deployment(deployment_path)
        except DeploymentValidationError as exc:
            raise ScenarioDiscoveryError(f"{deployment_path}: {exc}") from exc
        if deployment.compatibility is None:
            continue
        for declared in deployment.compatibility.scenarios:
            program_path = _resolve_program_path(deployment_path, declared.program)
            try:
                program = Program(program_path, node_environment.catalog)
                deployment.validate_against_program(program)
                deployment.validate_capabilities(program)
                validate_environment_availability(
                    node_environment, deployment, program
                )
                _validate_compatibility(program, deployment)
            except (
                ProgramValidationError,
                DeploymentValidationError,
                ScenarioDiscoveryError,
                ValueError,
            ) as exc:
                raise ScenarioDiscoveryError(
                    f"scenario '{declared.name}' is invalid: {exc}"
                ) from exc
            assistant = "on" if deployment.assistant and deployment.assistant.enabled else "off"
            scenarios.append(
                Scenario(
                    name=declared.name,
                    lane=deployment.compatibility.lane,
                    assistant=assistant,
                    program_path=program_path,
                    deployment_path=deployment_path,
                    description=declared.description,
                    aliases=declared.aliases,
                )
            )
    for cell_path in sorted(cell_paths):
        scenarios.append(
            _real_robot_cell_scenario(
                cell_path,
                node_environment=node_environment,
            )
        )
    return ScenarioCatalog(scenarios)


def _main() -> int:
    parser = argparse.ArgumentParser(description="Discover ManyForge scenarios")
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--root", action="append", required=True)
    parser.add_argument("--format", choices=("json", "shell"), default="json")
    args = parser.parse_args()
    environment = build_node_environment(
        args.catalog,
        external_roots=configured_node_roots(),
    )
    discovered = discover_scenarios(
        args.root,
        node_environment=environment,
    )
    if args.format == "shell":
        for scenario in discovered.scenarios:
            print(
                "|".join(
                    (
                        scenario.name,
                        scenario.lane,
                        scenario.assistant,
                        str(scenario.program_path),
                        str(scenario.deployment_path),
                        scenario.description.replace("|", " "),
                        ",".join(scenario.aliases),
                        scenario.runtime_resolver,
                        str(scenario.cell_path or ""),
                    )
                )
            )
    else:
        print(
            json.dumps(
                [
                    {
                        **asdict(scenario),
                        "program_path": str(scenario.program_path),
                        "deployment_path": str(scenario.deployment_path),
                        "cell_path": (
                            str(scenario.cell_path)
                            if scenario.cell_path is not None
                            else None
                        ),
                    }
                    for scenario in discovered.scenarios
                ],
                sort_keys=True,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
