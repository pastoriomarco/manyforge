"""Fail-closed contracts for narrowed historical qualification wrappers."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SOAK = _REPO_ROOT / "scripts" / "ci" / "run-soak-qualification.sh"
_COLLISION_SANITIZERS = (
    _REPO_ROOT / "scripts" / "ci" / "run-collision-engine-asan.sh",
    _REPO_ROOT / "scripts" / "ci" / "run-collision-engine-tsan.sh",
)
_SOAK_UNAVAILABLE = (
    "composable_cell_runtime_unavailable: historical Composer "
    "runtime/viewer/historian soak qualification returns at I5/H0, I6/N, "
    "and I9/I; retained structural cache tests remain in the ordinary "
    "Composer suite"
)
_COLLISION_UNAVAILABLE = (
    "composable_cell_runtime_unavailable: historical cross-package collision "
    "sanitizer qualification returns at I6/N and I8/P5; use "
    "retained-primitives only for the owner-neutral manyforge_collision subset"
)


def _poison_effect_commands(tmp_path: Path) -> tuple[dict[str, str], Path]:
    marker = tmp_path / "external-effect"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    for command in ("cmake", "docker", "dirname", "pwd", "python3"):
        stub = fake_bin / command
        stub.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' {command!r} >> {str(marker)!r}\n"
            "exit 97\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)
    return {**os.environ, "PATH": str(fake_bin)}, marker


def test_historical_soak_wrapper_fails_before_docker_or_path_resolution(
    tmp_path: Path,
) -> None:
    environment, marker = _poison_effect_commands(tmp_path)

    completed = subprocess.run(
        ["/bin/bash", str(_SOAK)],
        cwd=_REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 78
    assert completed.stdout == ""
    assert completed.stderr.strip() == _SOAK_UNAVAILABLE
    assert not marker.exists()


@pytest.mark.parametrize("script", _COLLISION_SANITIZERS)
@pytest.mark.parametrize(
    "arguments", ((), ("--help",), ("retained-primitives", "extra"))
)
def test_historical_collision_sanitizer_shapes_fail_before_cmake(
    script: Path,
    arguments: tuple[str, ...],
    tmp_path: Path,
) -> None:
    environment, marker = _poison_effect_commands(tmp_path)

    completed = subprocess.run(
        ["/bin/bash", str(script), *arguments],
        cwd=_REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 78
    assert completed.stdout == ""
    assert completed.stderr.strip() == _COLLISION_UNAVAILABLE
    assert not marker.exists()


@pytest.mark.parametrize("script", _COLLISION_SANITIZERS)
def test_retained_collision_sanitizer_selector_is_narrow_and_explicit(
    script: Path,
) -> None:
    source = script.read_text(encoding="utf-8")
    gate_offset = source.index('"$1" != "retained-primitives"')
    path_resolution_offset = source.index('SCRIPT_DIR="$(')

    assert gate_offset < path_resolution_offset
    assert '-S "${REPO_ROOT}/manyforge_collision"' in source
    assert '-S "${REPO_ROOT}/manyforge_roboplan"' not in source
    assert (
        '--test-regex "(ManyForgeCollision|CollisionQualificationPrimitives)"'
        in source
    )


def test_soak_wrapper_contains_no_reduced_campaign_body() -> None:
    source = _SOAK.read_text(encoding="utf-8")
    executable = "\n".join(
        line for line in source.splitlines() if not line.lstrip().startswith("#")
    )

    assert "MANYFORGE_RUN_SOAK_TESTS" not in executable
    assert "docker compose" not in executable
    assert "pytest -q" not in executable
