"""Focused source-level acceptance for the stale E0 production verifier."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tomllib

import pytest


_ROOT = Path(__file__).resolve().parents[1]
_E0_DIR = _ROOT / "verification" / "composable_cell_e0"
_SCRIPT = (
    _E0_DIR / "verify_production_coexistence.py"
)
_PINNED_SOURCE_COMMIT = "637894c460172b47faf64c7ece2e865bb8d69593"


def _load_module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_e0_production_gate_test",
        _SCRIPT,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _git_blob_id(path: Path) -> str:
    payload = path.read_bytes()
    framed = b"blob " + str(len(payload)).encode("ascii") + b"\0" + payload
    return hashlib.sha1(framed, usedforsecurity=False).hexdigest()


def test_production_coexistence_gate_fails_before_importing_old_bindings(
    capsys,
) -> None:
    module = _load_module()
    source = _SCRIPT.read_text(encoding="utf-8")

    assert "importlib.import_module" not in source
    assert "sys.path.insert" not in source
    assert "roboplan.create_native_kernel" not in source
    assert "hasattr(roboplan" not in source
    assert module.main(["--module-dir", "must-not-be-read"]) == 78
    assert capsys.readouterr().err.strip() == module.UNAVAILABLE_MESSAGE
    assert module.RESTORATION_GATE == "I2"


def test_historical_proof_translation_units_remain_exact() -> None:
    assert _git_blob_id(_E0_DIR / "pybind_module.cpp") == (
        "23313ca7b9b5a3bdcece6aaf09523ffb2103b361"
    )
    assert _git_blob_id(_E0_DIR / "nanobind_module.cpp") == (
        "644941de7f2778d0b3500dac8f4b11cfb8f7379d"
    )


def test_current_cmake_entrypoint_is_a_pre_dependency_historical_gate() -> None:
    source = (_E0_DIR / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "composable_cell_runtime_unavailable" in source
    assert _PINNED_SOURCE_COMMIT in source
    assert "atomic I2 Python/compiler binding cutover" in source
    no_language_project = "project(manyforge_e0_historical_proof LANGUAGES NONE)"
    project_lines = [
        line.strip()
        for line in source.splitlines()
        if line.lstrip().startswith("project(")
    ]
    assert project_lines == [no_language_project]
    assert source.index("message(FATAL_ERROR") < source.index(no_language_project)
    assert "find_package(" not in source
    assert "add_subdirectory(" not in source
    assert "execute_process(" not in source
    assert "include(" not in source
    assert "file(" not in source
    assert "enable_language(" not in source
    assert "LANGUAGES C" not in source
    assert "add_library(" not in source
    assert "add_executable(" not in source
    assert "add_custom_target(" not in source
    assert "install(" not in source


def test_current_package_entrypoint_uses_dependency_free_unavailable_backend() -> None:
    pyproject = tomllib.loads(
        (_E0_DIR / "pyproject.toml").read_text(encoding="utf-8")
    )
    build_system = pyproject["build-system"]

    assert build_system == {
        "requires": [],
        "build-backend": "unavailable_build_backend",
        "backend-path": ["."],
    }

    backend_path = _E0_DIR / "unavailable_build_backend.py"
    spec = importlib.util.spec_from_file_location(
        "manyforge_e0_unavailable_backend_test",
        backend_path,
    )
    assert spec is not None
    assert spec.loader is not None
    backend = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(backend)

    assert backend.PINNED_SOURCE_COMMIT == _PINNED_SOURCE_COMMIT
    assert backend.RESTORATION_GATE == "I2"
    for hook_name in (
        "get_requires_for_build_wheel",
        "prepare_metadata_for_build_wheel",
        "build_wheel",
        "get_requires_for_build_sdist",
        "build_sdist",
        "get_requires_for_build_editable",
        "prepare_metadata_for_build_editable",
        "build_editable",
    ):
        with pytest.raises(
            backend.HistoricalProofUnavailable,
            match="composable_cell_runtime_unavailable",
        ) as error:
            getattr(backend, hook_name)()
        assert str(error.value) == backend.UNAVAILABLE_MESSAGE


def test_readme_points_replay_to_the_pinned_source_only() -> None:
    source = (_E0_DIR / "README.md").read_text(encoding="utf-8")

    assert _PINNED_SOURCE_COMMIT in source
    assert "historical recipe, not current-branch instructions" in source
    assert "Do not restore the removed" in source
    assert f"export E0_SOURCE_COMMIT={_PINNED_SOURCE_COMMIT}" in source
    assert "There is no current-branch bypass" in source


def test_cmake_frontend_fails_before_language_or_dependency_discovery(
    tmp_path: Path,
) -> None:
    completed = subprocess.run(
        [
            "cmake",
            "-S",
            str(_E0_DIR),
            "-B",
            str(tmp_path / "cmake-build"),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    output = " ".join((completed.stdout + completed.stderr).split())

    assert completed.returncode != 0
    assert "composable_cell_runtime_unavailable" in output
    assert _PINNED_SOURCE_COMMIT in output
    assert "atomic I2 Python/compiler binding cutover" in output
    assert "compiler identification" not in output.lower()
    assert "pybind11" not in output
    assert "nanobind" not in output


def test_pep517_frontend_fails_before_external_build_dependency_install(
    tmp_path: Path,
) -> None:
    wheel_dir = tmp_path / "wheels"
    wheel_dir.mkdir()
    environment = os.environ.copy()
    environment.update(
        {
            "PIP_DISABLE_PIP_VERSION_CHECK": "1",
            "PIP_NO_INDEX": "1",
            "PYTHONPYCACHEPREFIX": str(tmp_path / "pycache"),
        }
    )
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "pip",
            "wheel",
            "--no-deps",
            "--wheel-dir",
            str(wheel_dir),
            str(_E0_DIR),
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
        cwd=tmp_path,
    )
    output = " ".join((completed.stdout + completed.stderr).split())

    assert completed.returncode != 0
    assert "composable_cell_runtime_unavailable" in output
    assert _PINNED_SOURCE_COMMIT in output
    assert "atomic I2 Python/compiler binding cutover" in output
    assert "scikit-build" not in output.lower()
    assert list(wheel_dir.iterdir()) == []
