from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def _load_matrix(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    package = tmp_path / "package"
    installed = tmp_path / "installed"
    package.mkdir()
    installed.mkdir()
    monkeypatch.setenv("MANYFORGE_ROOT", str(REPO_ROOT))
    monkeypatch.setenv("N7_MATRIX_PACKAGE", str(package))
    monkeypatch.setenv("N7_MATRIX_INSTALLED", str(installed))
    path = REPO_ROOT / "scripts/qualification/run_i6_n7_product_matrix.py"
    spec = importlib.util.spec_from_file_location("i6_n7_product_matrix", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_process_commands_cover_the_complete_typed_runtime_boundary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    matrix = _load_matrix(tmp_path, monkeypatch)
    layout = matrix.CellInstanceLayout.open_or_create(
        cell_id="cell.fixture",
        cell_root=tmp_path / "cell",
        install_root=matrix.PACKAGE,
    )
    commands = matrix._process_commands(  # noqa: SLF001
        scenario_id="lite6_pick_and_place",
        cell_id="cell.fixture",
        admission_filename="admission.json",
        model_config=tmp_path / "models.json",
        api_port=9010,
        runner_port=9011,
        composer_port=9000,
        layout=layout,
    )

    def value(arguments: list[str], option: str) -> str:
        return arguments[arguments.index(option) + 1]

    manager = commands["manager"]
    for removed in ("--package-root", "--runtime-state-root", "--publication-root"):
        assert removed not in manager
    for required in (
        "--install-root",
        "--manager-state-root",
        "--manager-run-root",
        "--execution-state-root",
        "--execution-run-root",
        "--program-cycle-journal",
        "--projection-root",
        "--program-runner-liveness-socket",
    ):
        assert manager.count(required) == 1

    for required in (
        "--execution-run-root",
        "--cycle-journal-path",
        "--manager-liveness-socket",
        "--cell-id",
    ):
        assert commands["program"].count(required) == 1
    assert value(manager, "--execution-run-root") == value(
        commands["program"], "--execution-run-root"
    )
    assert value(manager, "--program-cycle-journal") == value(
        commands["program"], "--cycle-journal-path"
    )
    assert value(manager, "--program-runner-liveness-socket") == value(
        commands["program"], "--manager-liveness-socket"
    )

    headless = commands["headless"]
    for removed in (
        "--publication-root",
        "--package-root",
        "--manager-state-root",
        "--execution-state-root",
    ):
        assert removed not in headless
    for required in (
        "--projection-root",
        "--install-root",
        "--artifact-root",
        "--node-environment-manifest-path",
        "--manager-run-root",
        "--execution-run-root",
    ):
        assert headless.count(required) == 1
    assert value(manager, "--manager-run-root") == value(headless, "--manager-run-root")
    assert value(manager, "--execution-run-root") == value(headless, "--execution-run-root")
    assert commands["composer"].count("--authoring-state-root") == 0
