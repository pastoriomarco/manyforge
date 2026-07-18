"""Artifact-owned named scenario discovery and compatibility checks."""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

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
        for path in sorted(root.glob("*.deployment.yaml")):
            deployment_paths.add(path.resolve())
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
                    }
                    for scenario in discovered.scenarios
                ],
                sort_keys=True,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
