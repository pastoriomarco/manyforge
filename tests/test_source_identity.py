"""Tests for the shared ``manyforge.source_identity.v1`` authority.

``scripts/lib/source_identity.py`` serves the operator and the remote
diagnostic build.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(relative: str, name: str) -> ModuleType:
    path = REPO_ROOT / relative
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True)


def init_repository(root: Path) -> None:
    root.mkdir()
    run_git(root, "init", "-q")
    run_git(root, "config", "user.email", "source-identity@example.invalid")
    run_git(root, "config", "user.name", "Source Identity Test")
    (root / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    run_git(root, "add", ".")
    run_git(root, "commit", "-qm", "baseline")


def test_source_identity_rejects_symlink_roots_and_untracked_hardlinks(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_path_hardening",
    )
    repository = tmp_path / "repository"
    repository.mkdir()
    run_git(repository, "init", "-q")
    run_git(repository, "config", "user.email", "source-identity@example.invalid")
    run_git(repository, "config", "user.name", "Source Identity Test")
    (repository / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    run_git(repository, "add", ".")
    run_git(repository, "commit", "-qm", "baseline")

    linked_root = tmp_path / "linked-repository"
    linked_root.symlink_to(repository, target_is_directory=True)
    with pytest.raises(identity.SourceIdentityError, match="symbolic-link traversal"):
        identity.source_identity(linked_root)

    first = repository / "untracked-first.txt"
    second = repository / "untracked-second.txt"
    first.write_text("same inode\n", encoding="utf-8")
    second.hardlink_to(first)
    with pytest.raises(identity.SourceIdentityError, match="hardlinks"):
        identity.source_identity(repository)


def test_source_identity_bounds_git_processes_and_untracked_payloads(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_resource_hardening",
    )
    with pytest.raises(identity.SourceIdentityError, match="resource bounds"):
        identity._run_bounded_process(  # noqa: SLF001
            [sys.executable, "-c", "import sys; sys.stdout.write('x' * 1024)"],
            environment={},
            pass_fds=(),
            stdout_limit=32,
            stderr_limit=32,
            timeout_seconds=5,
        )
    with pytest.raises(identity.SourceIdentityError, match="resource bounds"):
        identity._run_bounded_process(  # noqa: SLF001
            [sys.executable, "-c", "import time; time.sleep(5)"],
            environment={},
            pass_fds=(),
            stdout_limit=32,
            stderr_limit=32,
            timeout_seconds=0.01,
        )

    repository = tmp_path / "bounded-repository"
    repository.mkdir()
    run_git(repository, "init", "-q")
    run_git(repository, "config", "user.email", "source-identity@example.invalid")
    run_git(repository, "config", "user.name", "Source Identity Test")
    (repository / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    run_git(repository, "add", ".")
    run_git(repository, "commit", "-qm", "baseline")
    (repository / "untracked.bin").write_bytes(b"1234")
    monkeypatch.setattr(identity, "MAX_UNTRACKED_FILE_BYTES", 3)
    with pytest.raises(identity.SourceIdentityError, match="byte bound"):
        identity.source_identity(repository)


def test_bounded_process_cleans_up_when_selector_registration_fails(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_selector_cleanup",
    )
    closed = False

    class RejectingSelector:
        def register(self, *args, **kwargs):
            raise OSError("selector registration failed")

        def close(self) -> None:
            nonlocal closed
            closed = True

    monkeypatch.setattr(identity.selectors, "DefaultSelector", RejectingSelector)
    with pytest.raises(identity.SourceIdentityError, match="resource bounds"):
        identity._run_bounded_process(  # noqa: SLF001
            [sys.executable, "-c", "import time; time.sleep(5)"],
            environment={},
            pass_fds=(),
            stdout_limit=32,
            stderr_limit=32,
            timeout_seconds=5,
        )
    assert closed is True


def test_source_identity_binds_the_delta_to_the_captured_commit_and_v1_fields(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_commit_binding",
    )
    repository = tmp_path / "commit-bound-repository"
    init_repository(repository)
    head_commit = subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        text=True,
    ).strip()
    (repository / "tracked.txt").write_text("changed\n", encoding="utf-8")
    observed_diff_arguments: list[tuple[str, ...]] = []
    real_git_bytes = identity._git_bytes  # noqa: SLF001

    def record_git_bytes(repository_binding, *args: str, **kwargs):
        if args and args[0] == "diff":
            observed_diff_arguments.append(args)
        return real_git_bytes(repository_binding, *args, **kwargs)

    monkeypatch.setattr(identity, "_git_bytes", record_git_bytes)
    value = identity.source_identity(repository)

    assert set(value) == set(identity.FIELDS)
    assert value["schema_version"] == "manyforge.source_identity.v1"
    assert value["head_commit"] == head_commit
    assert value["dirty"] is True
    assert value["delta_sha256"] != identity.empty_delta_sha256()
    assert len(observed_diff_arguments) == 1
    assert head_commit in observed_diff_arguments[0]
    assert "HEAD" not in observed_diff_arguments[0]
    assert "--no-textconv" in observed_diff_arguments[0]
    assert "--no-ext-diff" in observed_diff_arguments[0]
    assert "--diff-algorithm=default" in observed_diff_arguments[0]
    assert "--find-renames=50%" in observed_diff_arguments[0]
    assert "--ignore-submodules=dirty" in observed_diff_arguments[0]
    assert "--submodule=short" in observed_diff_arguments[0]


def test_source_identity_rejects_executable_or_external_local_git_inputs(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_config_hardening",
    )
    repository = tmp_path / "configured-repository"
    init_repository(repository)
    sentinel = tmp_path / "textconv-executed"
    textconv = tmp_path / "textconv.sh"
    textconv.write_text(
        f'#!/bin/sh\nprintf executed > "{sentinel}"\ncat "$1"\n',
        encoding="utf-8",
    )
    textconv.chmod(0o700)
    run_git(repository, "config", "diff.sourceid.textconv", str(textconv))
    (repository / ".gitattributes").write_text(
        "tracked.txt diff=sourceid\n",
        encoding="utf-8",
    )
    (repository / "tracked.txt").write_text("changed\n", encoding="utf-8")

    with pytest.raises(
        identity.SourceIdentityError,
        match="source-shaping, executable, or external input",
    ):
        identity.source_identity(repository)
    assert not sentinel.exists()


@pytest.mark.parametrize(
    ("key", "value"),
    (
        ("diff.algorithm", "histogram"),
        ("filter.sourceid.clean", "cat"),
        ("submodule.sourceid.ignore", "all"),
        ("attr.tree", "HEAD"),
        ("core.sparseCheckout", "true"),
        ("extensions.partialClone", "origin"),
        ("index.sparse", "true"),
        ("promisor.quiet", "true"),
        ("remote.origin.partialCloneFilter", "blob:none"),
        ("remote.origin.promisor", "true"),
    ),
)
def test_source_identity_rejects_private_source_shaping_configuration(
    tmp_path: Path,
    key: str,
    value: str,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_source_shaping_config",
    )
    repository = tmp_path / "source-shaping-repository"
    init_repository(repository)
    run_git(repository, "config", key, value)

    with pytest.raises(
        identity.SourceIdentityError,
        match="source-shaping, executable, or external input",
    ):
        identity.source_identity(repository)


def test_source_identity_rejects_private_rules_and_object_alternates(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_private_rules",
    )
    repository = tmp_path / "private-rules-repository"
    init_repository(repository)
    info = repository / ".git/info"
    exclude = info / "exclude"
    attributes = info / "attributes"
    alternates = repository / ".git/objects/info/alternates"
    (repository / "hidden.txt").write_text("hidden\n", encoding="utf-8")

    exclude.write_text("# local comment\nhidden.txt\n", encoding="utf-8")
    with pytest.raises(identity.SourceIdentityError, match="active private source rule"):
        identity.source_identity(repository)

    exclude.write_bytes(b"# comments are inert\n\n")
    identity.source_identity(repository)
    exclude.write_bytes(b" # leading-space comments are active\n")
    with pytest.raises(identity.SourceIdentityError, match="active private source rule"):
        identity.source_identity(repository)

    exclude.write_bytes(b" \n")
    with pytest.raises(identity.SourceIdentityError, match="active private source rule"):
        identity.source_identity(repository)

    exclude.write_bytes(b"# comments are inert\n")
    attributes.write_text("*.txt -diff\n", encoding="utf-8")
    with pytest.raises(identity.SourceIdentityError, match="active private source rule"):
        identity.source_identity(repository)

    attributes.write_bytes(b" # leading-space comments are active\n")
    with pytest.raises(identity.SourceIdentityError, match="active private source rule"):
        identity.source_identity(repository)

    attributes.write_bytes(b"# comments are inert\n\n")
    alternates.write_text(str(tmp_path) + "\n", encoding="utf-8")
    with pytest.raises(identity.SourceIdentityError, match="zero-byte"):
        identity.source_identity(repository)

    alternates.write_bytes(b"\n")
    with pytest.raises(identity.SourceIdentityError, match="zero-byte"):
        identity.source_identity(repository)

    alternates.write_bytes(b"")
    identity.source_identity(repository)


def test_source_identity_rejects_source_shaping_worktree_configuration(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_worktree_config",
    )
    repository = tmp_path / "worktree-config-repository"
    init_repository(repository)
    run_git(repository, "config", "extensions.worktreeConfig", "true")
    run_git(repository, "config", "--worktree", "diff.algorithm", "histogram")

    with pytest.raises(
        identity.SourceIdentityError,
        match="source-shaping, executable, or external input",
    ):
        identity.source_identity(repository)


def test_source_identity_rejects_local_git_input_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_config_stability",
    )
    repository = tmp_path / "local-input-repository"
    init_repository(repository)
    real_delta = identity._worktree_delta_sha256  # noqa: SLF001
    changed = False

    def mutate_config_after_delta(*args, **kwargs):
        nonlocal changed
        value = real_delta(*args, **kwargs)
        if not changed:
            changed = True
            run_git(repository, "config", "user.name", "Changed During Identity")
        return value

    monkeypatch.setattr(identity, "_worktree_delta_sha256", mutate_config_after_delta)
    with pytest.raises(identity.SourceIdentityError, match="local Git inputs changed"):
        identity.source_identity(repository)


def test_source_identity_rejects_untracked_content_drift_after_the_first_hash(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_untracked_stability",
    )
    repository = tmp_path / "untracked-drift-repository"
    init_repository(repository)
    untracked = repository / "untracked.txt"
    untracked.write_text("first\n", encoding="utf-8")
    real_hash = identity._hash_untracked_files  # noqa: SLF001
    calls = 0

    def mutate_after_first_hash(*args, **kwargs):
        nonlocal calls
        value = real_hash(*args, **kwargs)
        calls += 1
        if calls == 1:
            untracked.write_text("second\n", encoding="utf-8")
        return value

    monkeypatch.setattr(identity, "_hash_untracked_files", mutate_after_first_hash)
    with pytest.raises(identity.SourceIdentityError, match="untracked source content changed"):
        identity.source_identity(repository)


def test_source_identity_supports_linked_and_separate_git_directories(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_gitfile_layouts",
    )
    repository = tmp_path / "primary-repository"
    init_repository(repository)
    linked = tmp_path / "linked-worktree"
    run_git(
        repository,
        "worktree",
        "add",
        "--detach",
        "--quiet",
        str(linked),
        "HEAD",
    )
    assert (linked / ".git").is_file()
    primary = identity.source_identity(repository)
    linked_identity = identity.source_identity(linked)
    assert linked_identity == primary

    separate_worktree = tmp_path / "separate-worktree"
    separate_git_directory = tmp_path / "separate-git-directory"
    subprocess.run(
        [
            "git",
            "init",
            "-q",
            f"--separate-git-dir={separate_git_directory}",
            str(separate_worktree),
        ],
        check=True,
    )
    run_git(separate_worktree, "config", "user.email", "source-identity@example.invalid")
    run_git(separate_worktree, "config", "user.name", "Source Identity Test")
    (separate_worktree / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    run_git(separate_worktree, "add", ".")
    run_git(separate_worktree, "commit", "-qm", "baseline")
    assert (separate_worktree / ".git").is_file()
    separate_identity = identity.source_identity(separate_worktree)
    assert separate_identity["dirty"] is False
    assert separate_identity["delta_sha256"] == identity.empty_delta_sha256()


def test_source_identity_supports_a_bound_split_index(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_split_index",
    )
    repository = tmp_path / "split-index-repository"
    init_repository(repository)
    before = identity.source_identity(repository)
    run_git(repository, "update-index", "--split-index")
    shared_indexes = list((repository / ".git").glob("sharedindex.*"))
    assert shared_indexes
    after = identity.source_identity(repository)
    assert after == before

    shared_index = shared_indexes[0]
    with identity._bind_repository(repository) as binding:  # noqa: SLF001
        shared_index.touch()
        binding.assert_current()

    original_bound = identity.MAX_GIT_SHARED_INDEX_TOTAL_BYTES
    monkeypatch.setattr(
        identity,
        "MAX_GIT_SHARED_INDEX_TOTAL_BYTES",
        shared_index.stat().st_size - 1,
    )
    with pytest.raises(identity.SourceIdentityError, match="aggregate byte bound"):
        identity.source_identity(repository)
    monkeypatch.setattr(
        identity,
        "MAX_GIT_SHARED_INDEX_TOTAL_BYTES",
        original_bound,
    )

    with pytest.raises(identity.SourceIdentityError, match="content changed"):
        with identity._bind_repository(repository):  # noqa: SLF001
            payload = bytearray(shared_index.read_bytes())
            payload[0] ^= 1
            shared_index.write_bytes(payload)


def test_source_identity_rechecks_a_split_index_path_after_content_hashing(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_split_index_path_window",
    )
    repository = tmp_path / "split-index-path-window-repository"
    init_repository(repository)
    run_git(repository, "update-index", "--split-index")
    shared_index = next((repository / ".git").glob("sharedindex.*"))
    real_hash = identity._descriptor_content_sha256  # noqa: SLF001
    armed = False
    replaced = False

    def replace_path_after_hash(*args, **kwargs):
        nonlocal replaced
        value = real_hash(*args, **kwargs)
        if armed and not replaced and "split index" in kwargs["label"]:
            replaced = True
            replacement = shared_index.with_name("replacement-shared-index")
            replacement.write_bytes(shared_index.read_bytes())
            replacement.replace(shared_index)
        return value

    monkeypatch.setattr(identity, "_descriptor_content_sha256", replace_path_after_hash)
    with pytest.raises(identity.SourceIdentityError, match="changed during hashing"):
        with identity._bind_repository(repository) as binding:  # noqa: SLF001
            armed = True
            binding.assert_current()


def test_source_identity_uses_separate_canonical_tracked_flag_inventories(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_index_flag_inventory",
    )
    repository = tmp_path / "index-flag-inventory-repository"
    init_repository(repository)
    observed: list[tuple[str, ...]] = []
    real_git_bytes = identity._git_bytes  # noqa: SLF001

    def record_git_bytes(repository_binding, *args: str, **kwargs):
        observed.append(args)
        return real_git_bytes(repository_binding, *args, **kwargs)

    monkeypatch.setattr(identity, "_git_bytes", record_git_bytes)
    identity.source_identity(repository)

    assert (
        "ls-files",
        "--cached",
        "--full-name",
        "-v",
        "-z",
    ) in observed
    assert (
        "ls-files",
        "--cached",
        "--full-name",
        "-f",
        "-z",
    ) in observed


@pytest.mark.parametrize(
    "flag",
    ("--assume-unchanged", "--skip-worktree"),
)
def test_source_identity_rejects_noncanonical_tracked_index_flags(
    tmp_path: Path,
    flag: str,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_noncanonical_index_flags",
    )
    repository = tmp_path / "noncanonical-index-flag-repository"
    init_repository(repository)
    run_git(repository, "update-index", flag, "tracked.txt")

    with pytest.raises(identity.SourceIdentityError, match="canonical uppercase H"):
        identity.source_identity(repository)


@pytest.mark.parametrize("split_index", (False, True))
def test_source_identity_neutralizes_persisted_fsmonitor_valid_state(
    tmp_path: Path,
    split_index: bool,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_fsmonitor_cache",
    )
    repository = tmp_path / "fsmonitor-cache-repository"
    init_repository(repository)
    hook = tmp_path / "fsmonitor-hook"
    hook.write_text("#!/bin/sh\nprintf 'token\\0'\n", encoding="utf-8")
    hook.chmod(0o700)
    run_git(repository, "config", "core.fsmonitor", str(hook))
    run_git(repository, "update-index", "--fsmonitor")
    run_git(repository, "update-index", "--fsmonitor-valid", "tracked.txt")
    if split_index:
        run_git(repository, "update-index", "--split-index")
    tagged = subprocess.check_output(
        ["git", "-C", str(repository), "ls-files", "-f"],
        text=True,
    )
    assert tagged == "h tracked.txt\n"

    # Once the executable input is removed, the persisted FSMN cache is inert:
    # source identity forces core.fsmonitor=false and must still observe edits.
    run_git(repository, "config", "--unset", "core.fsmonitor")
    clean = identity.source_identity(repository)
    (repository / "tracked.txt").write_text("modified\n", encoding="utf-8")
    modified = identity.source_identity(repository)
    assert modified != clean
    assert modified["dirty"] is True


def test_source_identity_rejects_tracked_gitlinks(tmp_path: Path) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_gitlink",
    )
    repository = tmp_path / "gitlink-repository"
    init_repository(repository)
    head_commit = subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        text=True,
    ).strip()
    run_git(
        repository,
        "update-index",
        "--add",
        "--cacheinfo",
        f"160000,{head_commit},nested-repository",
    )

    with pytest.raises(identity.SourceIdentityError, match="Git links"):
        identity.source_identity(repository)


def test_source_identity_rejects_a_noncommit_head(tmp_path: Path) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_noncommit_head",
    )
    repository = tmp_path / "noncommit-head-repository"
    init_repository(repository)
    head_tree = subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD^{tree}"],
        text=True,
    ).strip()
    (repository / ".git/HEAD").write_text(head_tree + "\n", encoding="ascii")

    with pytest.raises(identity.SourceIdentityError, match="not exactly a commit"):
        identity.source_identity(repository)


def test_source_identity_disables_lazy_fetch_and_interactive_prompts(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_noninteractive_environment",
    )
    repository = tmp_path / "noninteractive-repository"
    init_repository(repository)

    with identity._bind_repository(repository) as binding:  # noqa: SLF001
        assert binding.environment["GIT_NO_LAZY_FETCH"] == "1"
        assert binding.environment["GIT_TERMINAL_PROMPT"] == "0"
        assert binding.environment["GCM_INTERACTIVE"] == "Never"
    assert ("core.commitGraph", "false") in identity.CANONICAL_GIT_CONFIG


def test_source_identity_bounds_control_and_symlink_payloads(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/lib/source_identity.py",
        "source_identity_under_test_aggregate_bounds",
    )
    repository = tmp_path / "aggregate-bound-repository"
    init_repository(repository)
    (repository / "untracked-link").symlink_to("target")

    monkeypatch.setattr(identity, "MAX_UNTRACKED_TOTAL_BYTES", 3)
    with pytest.raises(identity.SourceIdentityError, match="aggregate byte bound"):
        identity.source_identity(repository)

    monkeypatch.setattr(identity, "MAX_UNTRACKED_TOTAL_BYTES", 512 * 1024 * 1024)
    run_git(repository, "add", "untracked-link")
    run_git(repository, "commit", "-qm", "tracked symlink")
    monkeypatch.setattr(identity, "MAX_SYMLINK_TOTAL_BYTES", 3)
    with pytest.raises(identity.SourceIdentityError, match="aggregate byte bound"):
        identity.source_identity(repository)

    monkeypatch.setattr(identity, "MAX_SYMLINK_TOTAL_BYTES", 16 * 1024 * 1024)
    monkeypatch.setattr(identity, "MAX_GIT_LOCAL_INPUT_TOTAL_BYTES", 1)
    with pytest.raises(identity.SourceIdentityError, match="local control inputs"):
        identity.source_identity(repository)

