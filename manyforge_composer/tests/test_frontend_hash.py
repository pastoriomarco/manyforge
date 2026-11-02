from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_HASH_SCRIPT = _REPO_ROOT / "scripts" / "lib" / "frontend_hash.py"


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
    payload = json.loads(result.stdout)
    assert isinstance(payload, dict)
    return {key: str(value) for key, value in payload.items()}


def _run_hash_raw(
    repo: Path,
    *args: str,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    merged_env.pop("MANYFORGE_FRONTEND_SOURCE_MODE", None)
    if env:
        merged_env.update(env)
    return subprocess.run(
        [sys.executable, str(_HASH_SCRIPT), *args],
        cwd=repo,
        check=False,
        capture_output=True,
        text=True,
        env=merged_env,
    )


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
    worktree = _run_hash(
        repo,
        "source",
        str(frontend),
        env={"MANYFORGE_FRONTEND_SOURCE_MODE": "worktree"},
    )

    tree_hash = _git(repo, "rev-parse", "HEAD:manyforge_composer/frontend")
    expected = hashlib.sha256(f"git-head\0{tree_hash}".encode()).hexdigest()

    assert values["MANYFORGE_FRONTEND_SOURCE_MODE"] == "git-head"
    assert values["MANYFORGE_FRONTEND_GIT_TREE_HASH"] == tree_hash
    assert values["MANYFORGE_FRONTEND_SOURCE_HASH"] == expected
    assert values["MANYFORGE_FRONTEND_SNAPSHOT_HASH"] == worktree[
        "MANYFORGE_FRONTEND_SOURCE_HASH"
    ]
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


def test_worktree_hash_rejects_escaping_symlink_and_oversize_file(tmp_path: Path):
    source = tmp_path / "frontend"
    source.mkdir()
    outside = tmp_path / "outside.js"
    outside.write_text("secret\n", encoding="utf-8")
    (source / "escape.js").symlink_to(outside)

    escaped = _run_hash_raw(
        tmp_path,
        "source",
        str(source),
        env={"MANYFORGE_FRONTEND_SOURCE_MODE": "worktree"},
    )
    assert escaped.returncode == 2
    assert "symlink escapes source root" in escaped.stderr

    (source / "escape.js").unlink()
    with (source / "oversize.js").open("wb") as stream:
        stream.truncate(64 * 1024 * 1024 + 1)
    oversized = _run_hash_raw(
        tmp_path,
        "source",
        str(source),
        env={"MANYFORGE_FRONTEND_SOURCE_MODE": "worktree"},
    )
    assert oversized.returncode == 2
    assert "file exceeds" in oversized.stderr


def test_nul_record_is_data_not_shell_assignments(frontend_repo):
    repo, frontend = frontend_repo
    result = subprocess.run(
        [
            sys.executable,
            str(_HASH_SCRIPT),
            "--output-format",
            "nul",
            "source",
            str(frontend),
        ],
        cwd=repo,
        check=True,
        capture_output=True,
    )
    fields = result.stdout.split(b"\0")
    assert fields[-1] == b""
    fields.pop()
    pairs = dict(zip(fields[0::2], fields[1::2], strict=True))
    assert len(pairs) == 10
    assert pairs[b"MANYFORGE_FRONTEND_SOURCE_MODE"] == b"git-head"
    assert b"=" not in fields[0]


def test_committed_record_stays_on_resolved_tree_when_head_moves(frontend_repo, tmp_path: Path):
    repo, frontend = frontend_repo
    first_tree = _git(repo, "rev-parse", "HEAD:manyforge_composer/frontend")
    first_dependency_hash = _run_hash(repo, "source", str(frontend))[
        "MANYFORGE_FRONTEND_DEP_HASH"
    ]
    first_snapshot_hash = _run_hash(repo, "source", str(frontend))[
        "MANYFORGE_FRONTEND_SNAPSHOT_HASH"
    ]

    (frontend / "package-lock.json").write_text(
        '{"lockfileVersion":3,"packages":{"changed":{}}}\n',
        encoding="utf-8",
    )
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
            "second frontend",
        ],
        cwd=repo,
        check=True,
        capture_output=True,
    )
    second_commit = _git(repo, "rev-parse", "HEAD")
    first_commit = _git(repo, "rev-parse", "HEAD^")
    subprocess.run(
        ["git", "update-ref", "HEAD", first_commit],
        cwd=repo,
        check=True,
        capture_output=True,
    )

    wrapper_dir = tmp_path / "bin"
    wrapper_dir.mkdir()
    wrapper = wrapper_dir / "git"
    wrapper.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
if [[ " $* " == *" rev-parse HEAD:manyforge_composer/frontend "* ]]; then
    output="$(${FRONTEND_TEST_REAL_GIT:?} "$@")"
    "${FRONTEND_TEST_REAL_GIT}" -C "${FRONTEND_TEST_REPO}" update-ref HEAD "${FRONTEND_TEST_NEXT_HEAD}"
    printf '%s\\n' "${output}"
    exit 0
fi
exec "${FRONTEND_TEST_REAL_GIT:?}" "$@"
""",
        encoding="utf-8",
    )
    wrapper.chmod(0o755)
    real_git = shutil.which("git")
    assert real_git is not None
    record = _run_hash(
        repo,
        "source",
        str(frontend),
        env={
            "PATH": f"{wrapper_dir}{os.pathsep}{os.environ.get('PATH', '')}",
            "FRONTEND_TEST_REAL_GIT": real_git,
            "FRONTEND_TEST_REPO": str(repo),
            "FRONTEND_TEST_NEXT_HEAD": second_commit,
        },
    )

    assert record["MANYFORGE_FRONTEND_GIT_TREE_HASH"] == first_tree
    assert record["MANYFORGE_FRONTEND_DEP_HASH"] == first_dependency_hash
    assert record["MANYFORGE_FRONTEND_SNAPSHOT_HASH"] == first_snapshot_hash
    assert _git(repo, "rev-parse", "HEAD") == second_commit


def test_git_environment_and_fsmonitor_cannot_redirect_fingerprint(frontend_repo, tmp_path: Path):
    repo, frontend = frontend_repo
    expected = _run_hash(repo, "source", str(frontend))

    other = tmp_path / "other"
    other.mkdir()
    subprocess.run(["git", "init"], cwd=other, check=True, capture_output=True)
    marker = tmp_path / "fsmonitor-called"
    hook = tmp_path / "fsmonitor.sh"
    hook.write_text(
        f"#!/usr/bin/env bash\nprintf called > {str(marker)!r}\n",
        encoding="utf-8",
    )
    hook.chmod(0o755)
    subprocess.run(
        ["git", "config", "core.fsmonitor", str(hook)],
        cwd=repo,
        check=True,
        capture_output=True,
    )

    actual = _run_hash(
        repo,
        "source",
        str(frontend),
        env={"GIT_DIR": str(other / ".git"), "GIT_WORK_TREE": str(other)},
    )
    assert actual["MANYFORGE_FRONTEND_GIT_TREE_HASH"] == expected[
        "MANYFORGE_FRONTEND_GIT_TREE_HASH"
    ]
    assert not marker.exists()
