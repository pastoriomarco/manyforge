"""Pure scenario metadata plus the fenced I6/N discovery client."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, NoReturn, Final

from manyforge_behavior.node_environment import NodeEnvironment


SCENARIO_DISCOVERY_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_scenario_discovery_unavailable"
)
SCENARIO_DISCOVERY_RESTORATION_GATES: Final = ("I6", "N")


class ScenarioDiscoveryError(ValueError):
    """Scenario metadata is malformed or mutually incompatible."""


class ScenarioDiscoveryUnavailableError(ScenarioDiscoveryError):
    """Scenario artifact discovery awaits native Program/addressing."""


def _raise_scenario_discovery_unavailable() -> NoReturn:
    raise ScenarioDiscoveryUnavailableError(
        f"{SCENARIO_DISCOVERY_UNAVAILABLE_REASON_CODE}: scenario discovery "
        "depends on the fenced Program client and returns at I6/N"
    )


def configured_scenario_root_values(value: str | None = None) -> tuple[str, ...]:
    """Return the one process-level scenario-root setting without defaults."""
    raw = os.environ.get("MANYFORGE_SCENARIO_ROOTS", "") if value is None else value
    return tuple(item for item in raw.split(os.pathsep) if item)


@dataclass(frozen=True)
class Scenario:
    """Presentation metadata; paths are not opened or validated here."""

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
    """Small deterministic name/alias index with no filesystem ownership."""

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


def discover_scenarios(
    roots: Iterable[str | Path],
    *,
    node_environment: NodeEnvironment,
) -> ScenarioCatalog:
    """Fail before reading roots or consulting environment availability."""
    del roots, node_environment
    _raise_scenario_discovery_unavailable()


def _main() -> int:
    print(
        f"{SCENARIO_DISCOVERY_UNAVAILABLE_REASON_CODE}: scenario discovery "
        "returns at I6/N",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(_main())
