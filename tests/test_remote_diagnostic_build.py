"""Exact laptop snapshot and remote runner ownership, without SSH or Docker graphs."""
from __future__ import annotations

import fcntl
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/remote-diagnostic-build.py"
SPEC = importlib.util.spec_from_file_location("remote_diagnostic", SCRIPT)
WRAPPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WRAPPER)


@pytest.fixture
def source(tmp_path):
    root = tmp_path / "worktree"
    root.mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / ".gitignore").write_text(".docker/\nbuild/\n")
    (root / "tracked.cpp").write_text("original\n")
    (root / "deleted.cpp").write_text("deleted\n")
    (root / "link").symlink_to("tracked.cpp")
    scripts = root / "scripts"
    scripts.mkdir()
    # Double only the existing diagnostic runner. The wrapper's real snapshot,
    # locking, request serialization, mirror update and evidence paths execute.
    (scripts / "diagnostic-build.sh").write_text(
        '#!/bin/bash\nset -u\n'
        'runner="$MANYFORGE_DIAGNOSTIC_CACHE_BASE/double/build"\n'
        'mkdir -p "$runner"\nprintf "fixture\\n" > "$runner/../configuration.txt"\n'
        'echo "Diagnostic tree: $runner"\n'
        'shift 2\n"$@"\n'
    )
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.test",
                    "commit", "-qm", "fixture"], check=True)
    return root


@pytest.fixture
def doubles(tmp_path, monkeypatch):
    binaries = tmp_path / "bin"
    binaries.mkdir()
    ssh = binaries / "ssh"
    ssh.write_text(f'#!{sys.executable}\nimport subprocess, sys\n'
                   'raise SystemExit(subprocess.run(["bash", "-c", sys.argv[-1]]).returncode)\n')
    docker = binaries / "docker"
    docker.write_text(f'#!{sys.executable}\nimport json, os, sys\n'
                     'from pathlib import Path\n'
                     'with Path(os.environ["DOCKER_CALLS"]).open("a") as log: log.write(json.dumps(sys.argv[1:])+"\\n")\n'
                     'assert sys.argv[1:3] == ["image", "inspect"]\n'
                     'arch = os.environ.get("IMAGE_ARCH", {"aarch64":"arm64", "x86_64":"amd64"}[os.uname().machine])\n'
                     'print(json.dumps([{ "Id": "sha256:"+"1"*64, "Architecture":arch }]))\n')
    for executable in (ssh, docker):
        executable.chmod(0o755)
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    monkeypatch.setenv("DOCKER_CALLS", str(tmp_path / "docker-calls.jsonl"))
    return tmp_path / "remote"


def launch(source, output, remote, *extra, command=("true",)):
    return subprocess.run([sys.executable, str(SCRIPT), "--host", "fixture", "--source", str(source),
                           "--image", "selected-image", "--variant", "core-release", "--output", str(output),
                           "--remote-root", str(remote), *extra, "--", *command], capture_output=True, text=True)


def receipt(output):
    return json.loads((output / "returned/receipt.json").read_text())


def test_snapshot_records_real_commit_deletion_untracked_ignored_and_symlink(source, tmp_path):
    (source / "deleted.cpp").unlink()
    (source / "new.cpp").write_text("new\n")
    vendor = source / ".docker/vendor"
    vendor.mkdir(parents=True)
    (vendor / "input.hpp").write_text("vendor\n")
    (vendor / "build").mkdir()
    (vendor / "build/object.o").write_text("generated")
    (vendor / ".git").mkdir()
    (vendor / ".git/config").write_text("private")
    output = tmp_path / "snapshot"
    output.mkdir()
    manifest = WRAPPER.snapshot(source, output, [".docker/vendor"])
    assert manifest["git_identity"]["head_commit"] == subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    assert manifest["git_identity"]["dirty"] is True
    assert manifest["tracked_deletions"] == ["deleted.cpp"]
    assert manifest["files"]["new.cpp"]["origin"] == "untracked"
    assert manifest["files"][".docker/vendor/input.hpp"]["origin"] == "ignored"
    assert manifest["files"]["link"]["target"] == "tracked.cpp"
    assert not any(".git" in p.split("/") or "build" in p.split("/") for p in manifest["files"])
    assert not (output / "source/.git").exists()


def test_source_change_during_copy_refuses_before_archive(source, tmp_path, monkeypatch):
    original = WRAPPER.copy_entry
    def change(*args):
        original(*args)
        (source / "tracked.cpp").write_text("changed-during-snapshot")
    monkeypatch.setattr(WRAPPER, "copy_entry", change)
    output = tmp_path / "snapshot"
    output.mkdir()
    with pytest.raises(WRAPPER.SnapshotError, match="changed|differ"):
        WRAPPER.snapshot(source, output, [])
    assert not (output / "source.tar").exists()


@pytest.mark.parametrize("name", ("../outside", "/absolute", "folder/../outside", "folder/.git/config"))
def test_refuses_unsafe_included_paths(name):
    with pytest.raises(WRAPPER.SnapshotError):
        WRAPPER.relative(name)


def test_refuses_external_symlink(source, tmp_path):
    (source / "outside").symlink_to(tmp_path / "secret")
    with pytest.raises(WRAPPER.SnapshotError, match="escapes"):
        WRAPPER.inventory(source, [])


def test_mirror_preserves_unchanged_timestamps_invalidates_changed_and_removes_deleted(source, tmp_path):
    entries, _ = WRAPPER.inventory(source, [])
    mirror = tmp_path / "mirror"
    WRAPPER.update_mirror(source, mirror, entries)
    old = (mirror / "tracked.cpp").stat().st_mtime_ns
    unchanged = (mirror / "deleted.cpp").stat().st_mtime_ns
    # The prior source timestamp may even be ahead of wall time. A changed
    # input must advance beyond it, while identical files remain untouched.
    future = old + 10_000_000_000
    os.utime(mirror / "tracked.cpp", ns=(future, future))
    (source / "tracked.cpp").write_text("new bytes")
    (source / "link").unlink()
    newer, _ = WRAPPER.inventory(source, [])
    WRAPPER.update_mirror(source, mirror, newer)
    assert (mirror / "tracked.cpp").stat().st_mtime_ns > future
    assert (mirror / "deleted.cpp").stat().st_mtime_ns == unchanged
    assert not (mirror / "link").exists()
    assert WRAPPER.tree(mirror) == WRAPPER.content(newer)


def test_shared_lock_and_priority_recheck_release_lock(tmp_path, monkeypatch):
    with (tmp_path / ".docker-build.lock").open("a") as owner:
        fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(BlockingIOError):
            WRAPPER.slot(tmp_path)
    original = fcntl.flock
    def priority_arrives(fd, operation):
        original(fd, operation)
        (tmp_path / ".t2-priority").write_text("other owner")
    monkeypatch.setattr(WRAPPER.fcntl, "flock", priority_arrives)
    with pytest.raises(BlockingIOError, match="appeared"):
        WRAPPER.slot(tmp_path)
    monkeypatch.setattr(WRAPPER.fcntl, "flock", original)
    with (tmp_path / ".docker-build.lock").open("a") as lock:
        original(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert (tmp_path / ".t2-priority").read_text() == "other owner"


def test_full_command_double_roundtrip_and_same_path_cache_reuse(source, doubles, tmp_path):
    first, second = tmp_path / "first", tmp_path / "second"
    for output in (first, second):
        result = launch(source, output, doubles)
        assert result.returncode == 0, result.stderr + result.stdout
    a, b = receipt(first), receipt(second)
    assert a["remote_source"] == b["remote_source"]
    assert a["diagnostic_build_tree"] == b["diagnostic_build_tree"]
    assert a["image_id"] == "sha256:" + "1" * 64
    assert Path(a["remote_source"]).joinpath(".git").exists() is False
    assert (first / "returned/artifacts/configuration.txt").read_text() == "fixture\n"


def test_same_basename_different_laptop_paths_have_distinct_remote_mirrors(source, doubles, tmp_path):
    second = tmp_path / "other/worktree"
    second.parent.mkdir()
    # Create genuine linked-worktree metadata. Copying an old Git index carries
    # stale inode/stat cache entries: Git diff refreshes it and the existing
    # identity helper correctly refuses that mutation during collection.
    subprocess.run(["git", "-C", str(source), "worktree", "add", "--detach", str(second), "HEAD"],
                   check=True, capture_output=True)
    outputs = (tmp_path / "one", tmp_path / "two")
    assert launch(source, outputs[0], doubles).returncode == 0
    second_run = launch(second, outputs[1], doubles)
    assert second_run.returncode == 0, second_run.stderr + second_run.stdout
    assert receipt(outputs[0])["remote_source"] != receipt(outputs[1])["remote_source"]


def test_remote_command_error_is_returned_with_log(source, doubles, tmp_path):
    output = tmp_path / "failed"
    result = launch(source, output, doubles, command=("bash", "-c", "echo actual-failure; exit 23"))
    assert result.returncode == 23, result.stderr
    assert receipt(output)["command_exit"] == 23
    assert "actual-failure" in (output / "returned/command.log").read_text()


def test_remote_priority_preserved_and_no_image_read(source, doubles, tmp_path):
    doubles.mkdir()
    (doubles / ".t2-priority").write_text("reserved")
    output = tmp_path / "busy"
    result = launch(source, output, doubles)
    assert result.returncode == 75, result.stderr
    assert (doubles / ".t2-priority").read_text() == "reserved"
    assert not (tmp_path / "docker-calls.jsonl").exists()


def test_wrong_image_architecture_refused_before_runner(source, doubles, tmp_path, monkeypatch):
    monkeypatch.setenv("IMAGE_ARCH", "wrong")
    output = tmp_path / "wrong-arch"
    assert launch(source, output, doubles).returncode == 70
    assert "architecture" in receipt(output)["error"]
    assert not (output / "returned/command.log").exists()


def test_missing_requested_evidence_cannot_be_a_pass(source, doubles, tmp_path):
    output = tmp_path / "missing"
    assert launch(source, output, doubles, "--artifact", "build/missing.xml").returncode == 70
    assert receipt(output)["command_exit"] == 0
    assert "missing" in receipt(output)["error"]


def test_remote_source_change_returned_separately_never_applied_laptop(source, doubles, tmp_path):
    output = tmp_path / "remote-drift"
    command = ("bash", "-c", 'echo remote-change > "$MANYFORGE_DIAGNOSTIC_SOURCE/tracked.cpp"')
    assert launch(source, output, doubles, command=command).returncode == 70
    assert (source / "tracked.cpp").read_text() == "original\n"
    assert (output / "returned/remote-source-changes/tracked.cpp").read_text() == "remote-change\n"
    assert "changed" in receipt(output)["error"]


def test_tampered_archive_is_refused(source, tmp_path):
    output = tmp_path / "snapshot"
    output.mkdir()
    manifest = WRAPPER.snapshot(source, output, [])
    (output / "source/tracked.cpp").write_text("tamper")
    with tarfile.open(output / "tampered.tar", "w") as archive:
        for name in manifest["files"]:
            archive.add(output / "source" / name, arcname=name, recursive=False)
    with pytest.raises(WRAPPER.SnapshotError, match="differ"):
        WRAPPER.unpack(output / "tampered.tar", tmp_path / "received", manifest["files"])


@pytest.mark.parametrize("level", ("source", "worktree"))
def test_mirror_container_symlink_refused_before_pruning_external_sentinel(source, tmp_path, level):
    root = tmp_path / "managed"
    root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    sentinel = outside / "sentinel"
    sentinel.write_text("must survive")
    worktree = root / "worktree"
    if level == "source":
        worktree.mkdir()
        (worktree / "source").symlink_to(outside, target_is_directory=True)
    else:
        worktree.symlink_to(outside, target_is_directory=True)
        (outside / "source").mkdir()
        (outside / "source/sentinel").write_text("must also survive")
    entries, _ = WRAPPER.inventory(source, [])
    with pytest.raises(WRAPPER.SnapshotError, match="symlink|escapes|resolves"):
        WRAPPER.update_mirror(source, worktree / "source", entries, root=root)
    assert sentinel.read_text() == "must survive"
    if level == "worktree":
        assert (outside / "source/sentinel").read_text() == "must also survive"


def test_existing_worktree_symlink_refused_before_remote_upload(source, doubles, tmp_path):
    doubles.mkdir()
    outside = tmp_path / "external"
    outside.mkdir()
    worktree_id = source.name + "-" + WRAPPER.digest(os.fsencode(source.resolve()))[:12]
    (doubles / worktree_id).symlink_to(outside, target_is_directory=True)
    output = tmp_path / "escaped-upload"
    assert launch(source, output, doubles).returncode == 70
    assert list(outside.iterdir()) == []
    assert not (tmp_path / "docker-calls.jsonl").exists()


def test_signal_during_runner_creation_is_forwarded_then_reaped(tmp_path, monkeypatch):
    import signal
    previous = signal.getsignal(signal.SIGTERM)
    events = []
    class Child:
        pid = 12345
        def wait(self):
            events.append("reaped")
            return 143
    def creating(*args, **kwargs):
        assert kwargs["start_new_session"] is True
        signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
        events.append("created")
        return Child()
    monkeypatch.setattr(WRAPPER.subprocess, "Popen", creating)
    monkeypatch.setattr(WRAPPER.os, "killpg", lambda pid, sig: events.append((pid, sig)))
    assert WRAPPER.run_command(["runner"], tmp_path / "log", {}) == 143
    assert events == ["created", (12345, signal.SIGTERM), "reaped"]
    assert signal.getsignal(signal.SIGTERM) is previous


@pytest.mark.parametrize("tracked", (True, False))
def test_build_prefix_source_filename_is_preserved(source, tmp_path, tracked):
    name = "build_owned_lifecycle_plan_catalog_source.hpp"
    (source / name).write_text("actual source, not a build directory\n")
    if tracked:
        subprocess.run(["git", "-C", str(source), "add", name], check=True)
    output = tmp_path / "snapshot"
    output.mkdir()
    manifest = WRAPPER.snapshot(source, output, [])
    assert manifest["files"][name]["origin"] == ("tracked" if tracked else "untracked")
    assert (output / "source" / name).read_bytes() == (source / name).read_bytes()


def test_tracked_generated_directory_still_refused(source):
    (source / "build").mkdir()
    (source / "build/generated.cpp").write_text("generated\n")
    subprocess.run(["git", "-C", str(source), "add", "-f", "build/generated.cpp"], check=True)
    with pytest.raises(WRAPPER.SnapshotError, match="Tracked path matches generated-output exclusion"):
        WRAPPER.inventory(source, [])


def test_coordinated_priority_roundtrip_preserves_caller_flag(source, doubles, tmp_path):
    doubles.mkdir()
    flag = doubles / ".t2-priority"
    flag.write_text("caller owns reservation")
    output = tmp_path / "priority-run"
    assert launch(source, output, doubles, "--priority", "--wait-seconds", "1").returncode == 0
    assert flag.read_text() == "caller owns reservation"
    receipt = json.loads((output / "returned/receipt.json").read_text())
    assert receipt["priority"] is True and receipt["wait_seconds"] == 1
    with (doubles / ".docker-build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


def test_priority_bounded_wait_does_not_interrupt_held_lock(tmp_path, monkeypatch):
    flag = tmp_path / ".t2-priority"
    flag.write_text("caller owns reservation")
    ticks = iter((0, 0.5, 1.0))
    sleeps = []
    monkeypatch.setattr(WRAPPER.time, "monotonic", lambda: next(ticks))
    monkeypatch.setattr(WRAPPER.time, "sleep", sleeps.append)
    with (tmp_path / ".docker-build.lock").open("a") as owner:
        fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(BlockingIOError, match="timed out"):
            WRAPPER.slot(tmp_path, priority=True, wait_seconds=1)
        assert sleeps == [0.1]
        with (tmp_path / ".docker-build.lock").open("a") as contender:
            with pytest.raises(BlockingIOError):
                fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert flag.read_text() == "caller owns reservation"
    monkeypatch.undo()
    with WRAPPER.slot(tmp_path, priority=True):
        pass


def test_bounded_wait_acquires_released_slot_without_flag_management(tmp_path, monkeypatch):
    original = fcntl.flock
    calls = []
    def busy_once(fd, operation):
        calls.append(operation)
        if len(calls) == 1:
            raise BlockingIOError("other active lane")
        original(fd, operation)
    monkeypatch.setattr(WRAPPER.fcntl, "flock", busy_once)
    monkeypatch.setattr(WRAPPER.time, "sleep", lambda _: None)
    with WRAPPER.slot(tmp_path, wait_seconds=1):
        assert len(calls) == 2
    assert not (tmp_path / ".t2-priority").exists()
