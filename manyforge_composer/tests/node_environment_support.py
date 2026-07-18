"""Shared immutable node environments for Composer tests."""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

import yaml

from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment


@lru_cache(maxsize=None)
def node_environment_for_catalog(catalog_path: str | Path) -> NodeEnvironment:
    """Build each immutable built-in environment once per test process."""
    return build_node_environment(Path(catalog_path).resolve())


def write_external_test_node(
    root: Path,
    node_id: str,
    *,
    parameters: list[dict] | None = None,
) -> Path:
    """Create one minimal valid external leaf for cross-surface tests."""
    folder = root / node_id
    folder.mkdir(parents=True)
    (folder / "node.yaml").write_text(
        yaml.safe_dump(
            {
                "api_version": "manyforge.external_node.v0",
                "id": node_id,
                "name": "Generated Test Node",
                "version": "0.1.0",
                "category": "IO",
                "description": "Cross-surface external-node test fixture.",
                "kind": "leaf",
                "children": {"min": 0, "max": 0},
                "profiles": {"min_profile": "v0.0"},
                "runtime": {
                    "language": "python",
                    "file": "node.py",
                    "class": "GeneratedNode",
                    "py_trees": "==2.4.0",
                    "constructor": "name_kwargs",
                    "tick_thread_policy": "nonblocking",
                },
                "required_capabilities": [],
                "parameters": parameters or [],
                "operator_stop_policy": "continue",
            },
            sort_keys=False,
        ),
        encoding="utf-8",
    )
    (folder / "node.py").write_text(
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params): super().__init__(name=name)
    def update(self): return py_trees.common.Status.SUCCESS
""",
        encoding="utf-8",
    )
    return folder
