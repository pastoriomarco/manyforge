#!/usr/bin/env python3
"""Emit artifact-owned scenarios in the launcher's stable row format."""

from __future__ import annotations

import os
from pathlib import Path
import sys


def _host_path(value: str, repo_root: Path) -> Path:
    path = Path(value).expanduser()
    if path == Path("/workspace"):
        return repo_root
    if path.is_absolute() and str(path).startswith("/workspace/"):
        return repo_root / path.relative_to("/workspace")
    return path


def _runtime_path(path: Path, repo_root: Path) -> str:
    resolved = path.resolve()
    try:
        return str(Path("/workspace") / resolved.relative_to(repo_root))
    except ValueError:
        return str(resolved)


def main() -> int:
    repo_root = Path(
        os.environ.get("MANYFORGE_ROOT", Path(__file__).resolve().parents[2])
    ).resolve()
    for source_root in (repo_root, repo_root / "manyforge_behavior"):
        source = str(source_root)
        if source not in sys.path:
            sys.path.insert(0, source)

    from manyforge_behavior.node_environment import (
        build_node_environment,
        configured_node_roots,
    )
    from manyforge_behavior.scenarios import (
        configured_scenario_root_values,
        discover_scenarios,
    )

    configured = configured_scenario_root_values()
    roots = (
        [_host_path(item, repo_root) for item in configured]
        if configured
        else [repo_root / "examples"]
    )
    node_roots = tuple(
        _host_path(str(path), repo_root) for path in configured_node_roots()
    )
    environment = build_node_environment(
        repo_root / "manyforge_behavior" / "resources" / "node_catalog.yaml",
        external_roots=node_roots,
    )
    scenarios = discover_scenarios(roots, node_environment=environment)
    for scenario in scenarios.scenarios:
        fields = (
            scenario.name,
            scenario.lane,
            scenario.assistant,
            _runtime_path(scenario.program_path, repo_root),
            _runtime_path(scenario.deployment_path, repo_root),
            scenario.description.replace("|", " ").replace("\n", " "),
            ",".join(scenario.aliases),
            scenario.runtime_resolver,
            (
                _runtime_path(scenario.cell_path, repo_root)
                if scenario.cell_path is not None
                else ""
            ),
        )
        print("|".join(fields))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
