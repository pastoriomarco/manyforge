"""Every launch mode lets the shipped examples' model sources resolve (OD-335).

Composer and Headless read Scenario files only inside MANYFORGE_FS_ROOTS
(the project path rule). The examples name model sources outside examples/
(manyforge_example_models), so each launcher's allowed folders must contain
ManyForge's own content root.
"""

from __future__ import annotations

from dataclasses import replace
import importlib.util
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.project_paths import ProjectPathRule
from manyforge_operator.filesystem_roots import (
    filesystem_roots_value,
    launch_filesystem_roots,
    shipped_content_roots,
)
from manyforge_operator.orchestrator import native_commands

from .test_launcher import _config, journald  # noqa: F401  (fixture)

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = REPOSITORY_ROOT / "examples"


def _require_shipped_model_sources_resolve(roots_value: str) -> int:
    """Resolve every model-source path of every shipped example Scenario
    under the allowed folders `roots_value`; returns how many resolved."""

    rule = ProjectPathRule(roots_value.split(","))
    resolved = 0
    for deployment_path in sorted(EXAMPLES.glob("*.scenario/deployment.yaml")):
        project = deployment_path.parent
        deployment = load_strict_json_yaml(deployment_path)
        for component in deployment.get("components", []):
            source = component.get("model_source")
            if not source:
                continue
            references = [("URDF", source["urdf"], "file")]
            if source.get("srdf") is not None:
                references.append(("SRDF", source["srdf"], "file"))
            references += [
                (f"package root {name!r}", reference, "directory")
                for name, reference in source.get("package_roots", {}).items()
            ]
            for label, reference, kind in references:
                (text,) = reference.values()
                rule.resolve(
                    text, project_folder=project,
                    label=f"{project.name} {component['component_id']} {label}",
                    kind=kind,
                )
                resolved += 1
    return resolved


def test_launch_roots_keep_their_order_once_and_name_the_shipped_content() -> None:
    assert launch_filesystem_roots("/a", "/b", "/a") == (Path("/a"), Path("/b"))
    assert shipped_content_roots("/src") == (
        Path("/src/examples"), Path("/src/manyforge_example_models"))
    assert filesystem_roots_value((Path("/a"), Path("/b"))) == "/a,/b"
    with pytest.raises(ValueError, match="absolute"):
        launch_filesystem_roots("relative")


def test_operator_launcher_roots_resolve_shipped_model_sources(
    tmp_path: Path, journald  # noqa: F811
) -> None:
    config = replace(_config(tmp_path), source_root=REPOSITORY_ROOT)
    _manager, processes = native_commands(config)
    roots = {process.role: process.environment for process in processes}
    value = roots["composer"]["MANYFORGE_FS_ROOTS"]
    assert roots["headless"]["MANYFORGE_FS_ROOTS"] == value
    assert _require_shipped_model_sources_resolve(value) > 0
    # Only the shipped content of the source tree is allowed, not the tree.
    assert str(REPOSITORY_ROOT) not in value.split(",")
    assert value.split(",")[:2] == [str(path) for path in shipped_content_roots(REPOSITORY_ROOT)]


def test_workbench_and_prebuilt_image_roots_resolve_shipped_model_sources(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # `./manyforge run` starts scripts/dev-workbench.py inside the image,
    # whose /workspace is this tree: one function serves both launches.
    spec = importlib.util.spec_from_file_location(
        "manyforge_dev_workbench_roots", REPOSITORY_ROOT / "scripts/dev-workbench.py")
    assert spec is not None and spec.loader is not None
    workbench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(workbench)
    monkeypatch.setattr(workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.delenv("MANYFORGE_DEV_PROJECT_ROOTS", raising=False)
    value = filesystem_roots_value(workbench._read_filesystem_roots(EXAMPLES))
    assert _require_shipped_model_sources_resolve(value) > 0


def test_examples_alone_do_not_resolve_shipped_model_sources() -> None:
    """The regression this guards: examples/ alone is not enough."""

    from manyforge_behavior.project_paths import ProjectPathError

    with pytest.raises(ProjectPathError, match="outside the allowed folders"):
        _require_shipped_model_sources_resolve(str(EXAMPLES))
