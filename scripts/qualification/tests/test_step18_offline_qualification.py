from __future__ import annotations

import contextlib
import importlib.util
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


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
    run_git(root, "config", "user.email", "step18@example.invalid")
    run_git(root, "config", "user.name", "Step 18 Test")
    (root / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    run_git(root, "add", ".")
    run_git(root, "commit", "-qm", "baseline")


def test_specs_contract_identity_excludes_only_the_evidence_record(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py", "step18_source_identity"
    )
    run_git(tmp_path, "init", "-q")
    run_git(tmp_path, "config", "user.email", "step18@example.invalid")
    run_git(tmp_path, "config", "user.name", "Step 18 Test")
    evidence = tmp_path / identity.SPECS_CONTRACT_EXCLUDED_PATH
    evidence.parent.mkdir(parents=True)
    evidence.write_text("initial evidence\n", encoding="utf-8")
    contract = tmp_path / "docs/spec/250-contact-policy.md"
    contract.parent.mkdir(parents=True)
    contract.write_text("contract v1\n", encoding="utf-8")
    run_git(tmp_path, "add", ".")
    run_git(tmp_path, "commit", "-qm", "baseline")

    initial_contract = identity.specs_contract_identity(tmp_path)
    initial_source = identity.source_identity(tmp_path)
    evidence.write_text("post-run evidence update\n", encoding="utf-8")
    assert identity.specs_contract_identity(tmp_path) == initial_contract
    assert identity.source_identity(tmp_path) != initial_source

    contract.write_text("contract v2\n", encoding="utf-8")
    assert identity.specs_contract_identity(tmp_path) != initial_contract


def test_shared_source_identity_matches_the_live_v1_authority() -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_shared_source_identity",
    )
    live = load_module(
        "scripts/qualification/roboplan_05_qualification.py",
        "step18_live_source_identity",
    )
    assert identity.source_identity(REPO_ROOT) == live._manyforge_source_identity(  # noqa: SLF001
        REPO_ROOT
    )


def test_source_identity_rejects_symlink_roots_and_untracked_hardlinks(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_source_identity_path_hardening",
    )
    repository = tmp_path / "repository"
    repository.mkdir()
    run_git(repository, "init", "-q")
    run_git(repository, "config", "user.email", "step18@example.invalid")
    run_git(repository, "config", "user.name", "Step 18 Test")
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_resource_hardening",
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
    run_git(repository, "config", "user.email", "step18@example.invalid")
    run_git(repository, "config", "user.name", "Step 18 Test")
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_selector_cleanup",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_commit_binding",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_config_hardening",
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
    run_git(repository, "config", "diff.step18.textconv", str(textconv))
    (repository / ".gitattributes").write_text(
        "tracked.txt diff=step18\n",
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
        ("filter.step18.clean", "cat"),
        ("submodule.step18.ignore", "all"),
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_source_shaping_config",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_private_rules",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_worktree_config",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_config_stability",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_untracked_stability",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_gitfile_layouts",
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
    run_git(separate_worktree, "config", "user.email", "step18@example.invalid")
    run_git(separate_worktree, "config", "user.name", "Step 18 Test")
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_split_index",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_split_index_path_window",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_index_flag_inventory",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_noncanonical_index_flags",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_fsmonitor_cache",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_gitlink",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_noncommit_head",
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


@pytest.mark.parametrize("combined", (False, True))
def test_specs_contract_collection_is_bracketed_by_full_source_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    combined: bool,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_source_identity_contract_bracket",
    )
    repository = tmp_path / "contract-bracket-repository"
    init_repository(repository)
    evidence = repository / identity.SPECS_CONTRACT_EXCLUDED_PATH
    evidence.parent.mkdir(parents=True)
    evidence.write_text("baseline evidence\n", encoding="utf-8")
    run_git(repository, "add", ".")
    run_git(repository, "commit", "-qm", "tracked evidence")
    real_collect = identity._collect_specs_contract_identity  # noqa: SLF001

    def mutate_excluded_evidence(repository_binding, source):
        value = real_collect(repository_binding, source)
        evidence.write_text("changed after contract\n", encoding="utf-8")
        return value

    monkeypatch.setattr(
        identity,
        "_collect_specs_contract_identity",
        mutate_excluded_evidence,
    )
    entrypoint = (
        identity.source_and_specs_contract_identities
        if combined
        else identity.specs_contract_identity
    )
    with pytest.raises(identity.SourceIdentityError, match="full source identity changed"):
        entrypoint(repository)


def test_combined_source_and_contract_collection_uses_one_repository_binding(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_source_identity_combined_binding",
    )
    repository = tmp_path / "combined-binding-repository"
    init_repository(repository)
    real_bind = identity._bind_repository  # noqa: SLF001
    bindings = 0

    @contextlib.contextmanager
    def count_bindings(repo_root):
        nonlocal bindings
        bindings += 1
        with real_bind(repo_root) as binding:
            yield binding

    monkeypatch.setattr(identity, "_bind_repository", count_bindings)
    source, contract = identity.source_and_specs_contract_identities(repository)

    assert bindings == 1
    assert source["head_commit"] == contract["head_commit"]
    assert source["head_tree"] == contract["head_tree"]


def test_source_identity_disables_lazy_fetch_and_interactive_prompts(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_source_identity_noninteractive_environment",
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
        "scripts/qualification/source_identity.py",
        "step18_source_identity_aggregate_bounds",
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


def test_roboplan_source_identity_wrappers_delegate_to_the_shared_authority(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    poison = ModuleType("source_identity")
    monkeypatch.setitem(sys.modules, "source_identity", poison)
    live = load_module(
        "scripts/qualification/roboplan_05_qualification.py",
        "step18_roboplan_shared_source_identity",
    )
    expected = {
        "schema_version": "manyforge.source_identity.v1",
        "head_commit": "1" * 40,
        "head_tree": "2" * 40,
        "dirty": True,
        "delta_sha256": "3" * 64,
        "fingerprint_sha256": "4" * 64,
    }
    monkeypatch.setattr(live, "_shared_source_identity", lambda root: expected)
    monkeypatch.setattr(
        live,
        "_shared_worktree_delta_sha256",
        lambda root: "5" * 64,
    )
    monkeypatch.setattr(
        live,
        "_shared_source_fingerprint_sha256",
        lambda **kwargs: "6" * 64,
    )

    assert Path(live._SHARED_SOURCE_IDENTITY.__file__).resolve() == (  # noqa: SLF001
        REPO_ROOT / "scripts/qualification/source_identity.py"
    ).resolve()
    assert live._manyforge_source_identity(tmp_path) is expected  # noqa: SLF001
    assert live._manyforge_worktree_delta_sha256(tmp_path) == "5" * 64  # noqa: SLF001
    assert live._manyforge_source_fingerprint_sha256(  # noqa: SLF001
        head_commit="1" * 40,
        head_tree="2" * 40,
        dirty=True,
        delta_sha256="3" * 64,
    ) == "6" * 64
    source = (
        REPO_ROOT / "scripts/qualification/roboplan_05_qualification.py"
    ).read_text(encoding="utf-8")
    assert 'b"manyforge.source_identity.v1\\0"' not in source
    assert "from source_identity import" not in source
    assert "_manyforge_source_identity(repo_root, dirty=" not in source
    assert '"manyforge_dirty": manyforge_identity["dirty"]' in source


def test_runtime_verifier_rejects_fewer_than_three_formal_runs() -> None:
    verifier = REPO_ROOT / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    result = subprocess.run(
        [
            sys.executable,
            str(verifier),
            "--minimum-runs",
            "1",
            "/does/not/exist.json",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "formal --minimum-runs must be exactly 3" in result.stderr


def test_runtime_group_requires_unique_process_and_artifact_identity() -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_verifier",
    )
    group = [
        {
            "identity": {
                "run_id": f"run-{index}",
                "process_instance_id": f"process-{index}",
            },
            "_artifact": {
                "path": f"/evidence/run-{index}.json",
                "sha256": f"hash-{index}",
            },
        }
        for index in range(3)
    ]
    assert verifier.independent_process_group(group, 3)[0] is True
    group[2]["identity"]["process_instance_id"] = "process-1"
    assert verifier.independent_process_group(group, 3)[0] is False
    group[2]["identity"]["process_instance_id"] = "process-2"
    group[2]["_artifact"]["sha256"] = "hash-1"
    assert verifier.independent_process_group(group, 3)[0] is False
    group[2]["_artifact"]["sha256"] = "hash-2"
    group.append(
        {
            "identity": {
                "run_id": "run-4",
                "process_instance_id": "process-4",
            },
            "_artifact": {
                "path": "/evidence/run-4.json",
                "sha256": "hash-4",
            },
        }
    )
    assert verifier.independent_process_group(group, 3)[0] is False


def test_runtime_verifier_detects_post_snapshot_artifact_replacement(
    tmp_path: Path,
) -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_snapshot_verifier",
    )
    artifact = tmp_path / "runtime.json"
    artifact.write_text('{"payload":"first"}\n', encoding="utf-8")
    records = verifier.load_artifact(artifact)
    artifact.write_text('{"payload":"second"}\n', encoding="utf-8")
    checks = verifier.Checks()
    verifier.require_artifacts_unchanged(
        [{"path": str(artifact), "sha256": records[0]["_artifact"]["sha256"]}],
        checks,
    )
    assert len(checks.failures) == 1
    assert checks.failures[0]["name"] == "retained_input_unchanged:0"


def test_runtime_summary_publication_discards_staging_on_parity_probe_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_publication_verifier",
    )
    repository = tmp_path / "manyforge"
    repository.mkdir()
    run_git(repository, "init", "-q")
    (repository / ".gitignore").write_text("qualification/\n", encoding="utf-8")
    artifact = tmp_path / "artifact.json"
    probe = tmp_path / "probe.json"
    tool = tmp_path / "tool.py"
    artifact.write_text("artifact-v1\n", encoding="utf-8")
    probe.write_text("probe-v1\n", encoding="utf-8")
    tool.write_text("tool-v1\n", encoding="utf-8")

    def manifest(path: Path) -> list[dict[str, str]]:
        return [
            {
                "path": str(path),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        ]

    destination = repository / "qualification" / "step18" / "summary.json"
    original_stage = verifier.stage_output

    def stage_then_mutate(path: Path, payload: bytes) -> Path:
        staged = original_stage(path, payload)
        probe.write_text("probe-v2\n", encoding="utf-8")
        return staged

    monkeypatch.setattr(verifier, "stage_output", stage_then_mutate)
    with pytest.raises(ValueError, match="retained parity probe changed"):
        verifier.publish_summary(
            destination,
            b'{"runtime_schedule_subgate_passed":true}\n',
            [],
            repository,
            manifest(artifact),
            manifest(probe),
            manifest(tool),
        )
    assert not destination.exists()
    assert not list(destination.parent.glob(".*.pending-*.tmp"))


def test_runtime_verifier_rejects_multi_record_formal_input(
    tmp_path: Path,
) -> None:
    artifact = tmp_path / "records.json"
    artifact.write_text("[{}, {}]\n", encoding="utf-8")
    verifier = (
        REPO_ROOT
        / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    )
    result = subprocess.run(
        [sys.executable, str(verifier), str(artifact)],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2
    assert "must contain exactly one record" in result.stderr


def test_runtime_record_rejects_a_reduced_formal_sampling_profile() -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_profile_verifier",
    )
    record = {
        "_artifact": {"path": "reduced.json", "record_index": 0},
        "identity": {},
        "inputs": {"bounded_threshold_m": 2.0},
        "config": {**verifier.FORMAL_PROFILE, "measured_ticks_requested": 1},
    }
    checks = verifier.Checks()
    verifier.validate_record(record, checks)
    profile_checks = [
        item
        for item in checks.items
        if item["name"].endswith(":exact_formal_sampling_profile")
    ]
    assert profile_checks == [
        {
            "name": "reduced.json#0:exact_formal_sampling_profile",
            "passed": False,
            "detail": "",
        }
    ]


def test_runtime_schedule_runners_are_historical_typed_gates() -> None:
    paths = (
        (
            "scripts/prepare-runtime-collision-schedule-baseline.sh",
            "7f0732e68f337c70a8fc5e0a5c2e86caa7276740",
        ),
        (
            "scripts/run-runtime-collision-schedule-baseline.sh",
            "4b5dffe72562d8dcf7399fc0df3d17908eb817ee",
        ),
        (
            "scripts/run-runtime-collision-schedule-qualification.sh",
            "01415f5e74dfa0b979d98c13425c084be339eaa5",
        ),
    )
    expected_body = [
        "set -euo pipefail",
        'SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',
        'source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"',
        "manyforge_fail_composable_cell_runtime_unavailable",
    ]
    for relative, historical_blob in paths:
        text = (REPO_ROOT / relative).read_text(encoding="utf-8")
        executable_body = [
            line.strip()
            for line in text.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        assert text.startswith("#!/usr/bin/env bash\n")
        assert "08222188293df8e12f90dc72e204c21f22f92ea1" in text
        assert historical_blob in text
        assert "working_tree_candidate_sha256" not in text
        assert "git -C \"${REPO_ROOT}\" diff --binary HEAD" not in text
        assert executable_body == expected_body

    for relative in (
        "scripts/qualification/wrap_runtime_collision_schedule_artifact.py",
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
    ):
        text = (REPO_ROOT / relative).read_text(encoding="utf-8")
        assert "source_identity.py" in text
        assert "working_tree_candidate_sha256" not in text


@pytest.mark.parametrize(
    "relative",
    (
        "scripts/prepare-runtime-collision-schedule-baseline.sh",
        "scripts/run-runtime-collision-schedule-baseline.sh",
        "scripts/run-runtime-collision-schedule-qualification.sh",
    ),
)
def test_runtime_schedule_gates_overwrite_inherited_guard_and_function(
    tmp_path: Path,
    relative: str,
) -> None:
    marker = tmp_path / (Path(relative).name + ".forged")
    poison = r"""
set -euo pipefail
manyforge_fail_composable_cell_runtime_unavailable() {
    builtin printf 'forged gate executed\n' >"${MANYFORGE_TEST_POISON_MARKER}"
    return 0
}
export -f manyforge_fail_composable_cell_runtime_unavailable
export _MANYFORGE_COMPOSABLE_CELL_RUNTIME_AVAILABILITY=1
export MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE=forged
export MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE=forged
export MANYFORGE_TEST_POISON_MARKER="$2"
exec bash "$1"
"""
    result = subprocess.run(
        [
            "bash",
            "-c",
            poison,
            "poison-runtime-gate",
            str(REPO_ROOT / relative),
            str(marker),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 78
    assert result.stdout == ""
    assert result.stderr == (
        "composable_cell_runtime_unavailable: this legacy aggregate entrypoint "
        "has no single qualified current replacement; use the Component "
        "Manager, Headless, Program runner, and Composer entrypoints for "
        "maintained ManyForge-only runtime functions.\n"
    )
    assert not marker.exists()


def test_runtime_artifact_schema_exposes_no_overall_step18_close() -> None:
    record_schema = "manyforge.runtime_collision_schedule_qualification.v2"
    wrapper = (
        REPO_ROOT
        / "scripts/qualification/wrap_runtime_collision_schedule_artifact.py"
    ).read_text(encoding="utf-8")
    verifier = (
        REPO_ROOT
        / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    ).read_text(encoding="utf-8")
    harness = (
        REPO_ROOT
        / "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
    ).read_text(encoding="utf-8")
    assert record_schema in wrapper
    assert record_schema in verifier
    assert '"step18_closed"' not in verifier
    assert '"requires_common_candidate_aggregator": True' in verifier
    assert "working_tree_candidate_sha256" not in harness
    assert record_schema not in harness
    assert "std::array<AcceptanceDisposition, 8>" in harness
    assert "emit_unavailable(" in harness
    assert '"I6/N+I8/P5"' in harness
    for acceptance_family in (
        "schedule_configuration_and_lookahead_selection",
        "fixture_snapshot_policy_and_trajectory",
        "immediate_monitor_campaign",
        "lookahead_worker_campaign",
        "cold_snapshot_preparation",
        "warm_query_allocation_attribution",
        "binary_release_and_bounded_penetration_arms",
        "timing_observation_and_replay_evidence",
    ):
        assert acceptance_family in harness


def test_runtime_wrapper_rejects_host_native_process_mismatch(
    tmp_path: Path,
) -> None:
    wrapper = (
        REPO_ROOT
        / "scripts/qualification/wrap_runtime_collision_schedule_artifact.py"
    )
    identity_authority = load_module(
        "scripts/qualification/source_identity.py",
        "step18_runtime_wrapper_identity",
    )
    checkout = tmp_path / "checkout"
    checkout.mkdir()
    run_git(checkout, "init", "-q")
    run_git(checkout, "config", "user.email", "step18@example.invalid")
    run_git(checkout, "config", "user.name", "Step 18 Test")
    (checkout / "input.txt").write_text("identity\n", encoding="utf-8")
    run_git(checkout, "add", ".")
    run_git(checkout, "commit", "-qm", "identity")
    source = identity_authority.source_identity(checkout)
    contract = identity_authority.specs_contract_identity(checkout)
    artifact = tmp_path / "native.json"
    artifact.write_text(
        json.dumps(
            {
                "schema_version": (
                    "manyforge.runtime_collision_schedule_qualification.v2"
                ),
                "identity": {
                    "process_instance_id": "native-process",
                    "source_identity": source,
                    "specs_source_identity": source,
                    "specs_contract_identity": contract,
                },
                "provenance": {},
            }
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            sys.executable,
            str(wrapper),
            "--input",
            str(artifact),
            "--process-instance-id",
            "host-process",
            "--source-identity-json",
            json.dumps(source),
            "--specs-source-identity-json",
            json.dumps(source),
            "--specs-contract-identity-json",
            json.dumps(contract),
            "--runner-command-json",
            '["runner"]',
            "--build-commands-json",
            '[["build"]]',
            "--outer-command-json",
            '["docker","compose","run"]',
            "--runner-environment-json",
            '{"fixed":"environment"}',
            "--runner-script-sha256",
            "0" * 64,
            "--source-identity-script-sha256",
            "1" * 64,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "native process instance does not match" in result.stderr


def test_runtime_parity_generator_rejects_a_formal_record_as_a_probe() -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_generator",
    )
    record = {
        "schema_version": generator.RECORD_SCHEMA,
        "identity": {
            "measurement_kind": "timing",
            "run_id": "formal-run",
            "process_instance_id": "5e7f1f7e-fc10-4d36-b412-cce396291647",
            "source_identity": {"head_commit": generator.BASELINE_SOURCE},
            "specs_source_identity": {"head_commit": generator.BASELINE_SPECS},
            "specs_contract_identity": {
                "head_commit": generator.BASELINE_SPECS,
                "contract_dirty": False,
            },
        },
        "inputs": {"arm": "current_all_active_distance"},
        "provenance": {
            "source_role": "pristine_baseline_timing",
            "runner_environment": {"purpose": "formal"},
        },
    }
    with pytest.raises(ValueError, match="not labeled as a parity probe"):
        generator.validate_identity(
            record,
            "timing",
            "pristine_baseline_timing",
        )


def test_runtime_parity_output_cannot_enter_qualification_checkouts(
    tmp_path: Path,
) -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_output",
    )
    timing_source = tmp_path / "timing-source"
    specs_source = tmp_path / "specs-source"
    timing_source.mkdir()
    specs_source.mkdir()
    records = (
        {
            "provenance": {
                "runner_environment": {
                    "source_root": str(timing_source),
                    "specs_root": str(specs_source),
                }
            }
        },
        {
            "provenance": {
                "runner_environment": {
                    "source_root": str(timing_source),
                    "specs_root": str(specs_source),
                }
            }
        },
    )
    with pytest.raises(ValueError, match="inside qualification checkout"):
        generator.validated_output(timing_source / "parity.json", records)
    with pytest.raises(ValueError, match="inside qualification checkout"):
        generator.validated_output(specs_source / "parity.json", records)
    with pytest.raises(ValueError, match="ignored qualification or build"):
        generator.validated_output(REPO_ROOT / "parity.json", records)
    assert generator.validated_output(
        REPO_ROOT / "qualification" / "step18" / "parity.json", records
    ) == REPO_ROOT / "qualification" / "step18" / "parity.json"


def test_runtime_parity_atomic_publish_never_overwrites(
    tmp_path: Path,
) -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_atomic_publish",
    )
    destination = tmp_path / "parity.json"
    destination.write_text("existing\n", encoding="utf-8")
    staged = generator.stage_output(destination, b"replacement\n")
    with pytest.raises(ValueError, match="refusing to overwrite"):
        generator.publish_staged_exclusive(staged, destination)
    assert destination.read_text(encoding="utf-8") == "existing\n"
    assert not staged.exists()
