"""Focused adversarial tests for the private-spec repository boundary guard."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SOURCE_CHECKER = _REPO_ROOT / "scripts/ci/check-private-spec-boundary.sh"
_ALLOWLIST_FILES = (
    "AGENTS.md",
    "docs/operations/STACK_SETUP.md",
    "docs/operations/ORIN_STACK_SETUP.md",
    "scripts/dev-agent-session-start.sh",
    "scripts/regen-public-contracts.py",
    "scripts/qualification/runtime_collision_schedule_baseline.md",
    "scripts/qualification/step18_offline_proof.md",
    "scripts/run-step18-loaded-kernel-qualification.sh",
)
_RUNTIME_DIRECTORIES = (
    "docs/operations",
    "docs/guides",
    "docs/reference",
    "manyforge_assistant_bridge",
    "manyforge_composer/backend",
    "manyforge_behavior",
    "manyforge_core",
    "manyforge_roboplan",
    "manyforge_ros",
    "scripts",
    "examples",
)


def _boundary_repository(tmp_path: Path) -> tuple[Path, Path]:
    root = tmp_path / "repository"
    for relative in _RUNTIME_DIRECTORIES:
        (root / relative).mkdir(parents=True, exist_ok=True)
    for relative in _ALLOWLIST_FILES:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("owner-neutral fixture\n", encoding="utf-8")
    (root / "README.md").write_text("fixture\n", encoding="utf-8")

    checker = root / "scripts/ci/check-private-spec-boundary.sh"
    checker.parent.mkdir(parents=True, exist_ok=True)
    checker.write_bytes(_SOURCE_CHECKER.read_bytes())
    checker.chmod(0o755)
    return root, checker


def _run_checker(
    root: Path,
    checker: Path,
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(checker)],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def test_private_spec_boundary_preserves_allowlist_and_rejects_runtime_file(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    (root / "AGENTS.md").write_text(
        "maintainer-only manyforge_specs reference\n",
        encoding="utf-8",
    )

    clean = _run_checker(root, checker)
    assert clean.returncode == 0

    violation = root / "manyforge_core/private_reference.py"
    violation.write_text(
        'PRIVATE_REPOSITORY = "manyforge_specs"\n',
        encoding="utf-8",
    )
    rejected = _run_checker(root, checker)

    assert rejected.returncode == 1
    assert "manyforge_core/private_reference.py" in rejected.stderr
    assert "PRIVATE_REPOSITORY" not in rejected.stderr


def test_private_spec_boundary_does_not_copy_unbounded_matching_line(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    (root / "manyforge_core/large_reference.py").write_text(
        "x" * 2_000_000 + "manyforge_specs\n",
        encoding="utf-8",
    )

    completed = _run_checker(root, checker)

    assert completed.returncode == 1
    assert "manyforge_core/large_reference.py" in completed.stderr
    assert len(completed.stderr) < 16_384


def test_private_spec_boundary_bounds_violation_inventory(tmp_path: Path) -> None:
    root, checker = _boundary_repository(tmp_path)
    for index in range(257):
        (root / "manyforge_core" / f"violation_{index:03d}.py").write_text(
            'PRIVATE_REPOSITORY = "manyforge_specs"\n',
            encoding="utf-8",
        )

    completed = _run_checker(root, checker)

    assert completed.returncode == 1
    assert "more than 256 violating files" in completed.stderr
    assert completed.stderr.count("manyforge_core/violation_") == 256


def test_private_spec_boundary_fails_closed_when_grep_fails(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    grep = fake_bin / "grep"
    grep.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n' 'synthetic grep failure' >&2\n"
        "exit 2\n",
        encoding="utf-8",
    )
    grep.chmod(0o755)

    completed = _run_checker(
        root,
        checker,
        env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"},
    )

    assert completed.returncode == 2
    assert "grep failed while scanning" in completed.stderr
    assert len(completed.stderr) < 4_096
    assert "OK:" not in completed.stdout


def test_private_spec_boundary_fails_closed_on_malformed_tool_output(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    grep = fake_bin / "grep"
    grep.write_text(
        "#!/bin/sh\n"
        "printf '%s' 'not-nul-terminated'\n"
        "exit 0\n",
        encoding="utf-8",
    )
    grep.chmod(0o755)

    completed = _run_checker(
        root,
        checker,
        env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"},
    )

    assert completed.returncode == 2
    assert "non-NUL-terminated path inventory" in completed.stderr
    assert "OK:" not in completed.stdout


def test_private_spec_boundary_rejects_tool_path_outside_scan_root(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    grep = fake_bin / "grep"
    grep.write_text(
        "#!/bin/sh\n"
        "printf 'AGENTS.md\\0'\n"
        "exit 0\n",
        encoding="utf-8",
    )
    grep.chmod(0o755)

    completed = _run_checker(
        root,
        checker,
        env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"},
    )

    assert completed.returncode == 2
    assert "path outside the file scan root" in completed.stderr
    assert "OK:" not in completed.stdout


def test_private_spec_boundary_fails_closed_on_symlinked_scan_root(
    tmp_path: Path,
) -> None:
    root, checker = _boundary_repository(tmp_path)
    (root / "examples").rmdir()
    external = tmp_path / "external"
    external.mkdir()
    (root / "examples").symlink_to(external, target_is_directory=True)

    completed = _run_checker(root, checker)

    assert completed.returncode == 2
    assert "runtime scan root is a symlink" in completed.stderr
    assert "OK:" not in completed.stdout
