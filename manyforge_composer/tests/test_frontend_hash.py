from __future__ import annotations

import hashlib
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HASH_SCRIPT = _REPO_ROOT / "scripts" / "lib" / "frontend_hash.py"


def _parse_env(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, raw = line.split("=", 1)
        parts = shlex.split(raw)
        values[key] = parts[0] if parts else ""
    return values


def _run_hash(repo: Path, *args: str, env: dict[str, str] | None = None) -> dict[str, str]:
    merged_env = os.environ.copy()
    merged_env.pop("MANYFORGE_FRONTEND_SOURCE_MODE", None)
    if env:
        merged_env.update(env)
    result = subprocess.run(
        [sys.executable, str(_HASH_SCRIPT), *args],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
        env=merged_env,
    )
    return _parse_env(result.stdout)


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-c", "safe.directory=*", *args],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


@pytest.fixture
def frontend_repo(tmp_path: Path):
    if shutil.which("git") is None:
        pytest.skip("git not available")

    repo = tmp_path / "repo"
    frontend = repo / "manyforge_composer" / "frontend"
    (frontend / "src").mkdir(parents=True)
    (frontend / ".gitignore").write_text("dist\nnode_modules\n", encoding="utf-8")
    (frontend / "package.json").write_text('{"scripts":{"build":"true"}}\n', encoding="utf-8")
    (frontend / "package-lock.json").write_text('{"lockfileVersion":3}\n', encoding="utf-8")
    (frontend / "src" / "App.tsx").write_text("export const value = 1;\n", encoding="utf-8")

    subprocess.run(["git", "init"], cwd=repo, check=True, capture_output=True)
    subprocess.run(["git", "add", "."], cwd=repo, check=True, capture_output=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "user.name=Test User",
            "commit",
            "-m",
            "initial frontend",
        ],
        cwd=repo,
        check=True,
        capture_output=True,
    )
    return repo, frontend


def test_committed_source_hash_matches_git_tree(frontend_repo):
    repo, frontend = frontend_repo
    values = _run_hash(repo, "source", str(frontend))

    tree_hash = _git(repo, "rev-parse", "HEAD:manyforge_composer/frontend")
    expected = hashlib.sha256(f"git-head\0{tree_hash}".encode()).hexdigest()

    assert values["MANYFORGE_FRONTEND_SOURCE_MODE"] == "git-head"
    assert values["MANYFORGE_FRONTEND_GIT_TREE_HASH"] == tree_hash
    assert values["MANYFORGE_FRONTEND_SOURCE_HASH"] == expected
    assert values["MANYFORGE_FRONTEND_DEP_FILE_COUNT"] == "2"


def test_committed_source_hash_ignores_artifacts_and_reports_dirty(frontend_repo):
    repo, frontend = frontend_repo
    base = _run_hash(repo, "source", str(frontend))

    (frontend / "dist").mkdir()
    (frontend / "dist" / "bundle.js").write_text("ignored artifact\n", encoding="utf-8")
    ignored = _run_hash(repo, "source", str(frontend))

    assert ignored["MANYFORGE_FRONTEND_SOURCE_HASH"] == base["MANYFORGE_FRONTEND_SOURCE_HASH"]
    assert ignored["MANYFORGE_FRONTEND_DIRTY_FILE_COUNT"] == "0"

    (frontend / "src" / "scratch.ts").write_text("untracked\n", encoding="utf-8")
    dirty = _run_hash(repo, "source", str(frontend))

    assert dirty["MANYFORGE_FRONTEND_SOURCE_HASH"] == base["MANYFORGE_FRONTEND_SOURCE_HASH"]
    assert dirty["MANYFORGE_FRONTEND_DIRTY_FILE_COUNT"] == "1"


def test_worktree_mode_is_explicit_and_sees_dirty_changes(frontend_repo):
    repo, frontend = frontend_repo
    committed = _run_hash(repo, "source", str(frontend))
    (frontend / "src" / "App.tsx").write_text("export const value = 2;\n", encoding="utf-8")

    still_committed = _run_hash(repo, "source", str(frontend))
    worktree = _run_hash(
        repo,
        "source",
        str(frontend),
        env={"MANYFORGE_FRONTEND_SOURCE_MODE": "worktree"},
    )

    assert still_committed["MANYFORGE_FRONTEND_SOURCE_HASH"] == committed["MANYFORGE_FRONTEND_SOURCE_HASH"]
    assert still_committed["MANYFORGE_FRONTEND_DIRTY_FILE_COUNT"] == "1"
    assert worktree["MANYFORGE_FRONTEND_SOURCE_MODE"] == "worktree"
    assert worktree["MANYFORGE_FRONTEND_SOURCE_HASH"] != committed["MANYFORGE_FRONTEND_SOURCE_HASH"]


def test_tree_hash_changes_when_dist_changes(tmp_path: Path):
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "index.html").write_text("<!doctype html>\n", encoding="utf-8")
    (dist / "bundle.js").write_text("one\n", encoding="utf-8")

    first = _run_hash(_REPO_ROOT, "tree", str(dist))
    (dist / "bundle.js").write_text("two\n", encoding="utf-8")
    second = _run_hash(_REPO_ROOT, "tree", str(dist))

    assert first["MANYFORGE_FRONTEND_TREE_FILE_COUNT"] == "2"
    assert first["MANYFORGE_FRONTEND_TREE_HASH"] != second["MANYFORGE_FRONTEND_TREE_HASH"]
