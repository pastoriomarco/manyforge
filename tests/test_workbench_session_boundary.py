from __future__ import annotations

import json
import fcntl
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import uuid

import pytest

from scripts.lib import workbench_session as sessions


@pytest.fixture
def state(tmp_path: Path):
    root = tmp_path / "state"
    root.mkdir()
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        yield root, descriptor
    finally:
        os.close(descriptor)


def selected_docker(state):
    root, descriptor = state
    identity = uuid.uuid4().hex
    marker = sessions.prepare_session(root, identity, lock_fd=descriptor,
        process_boundary={"kind": "docker", "container_id": None})
    sessions.bind_container(root, identity, "a" * 64, lock_fd=descriptor)
    return root, descriptor, marker


def stopped_row(root: Path, marker: dict) -> dict:
    return {"Id": "a" * 64, "State": {"Status": "exited", "Running": False,
        "Paused": False, "Restarting": False}, "Config": {"Labels": {
            "manyforge.workbench-session": marker["session_id"]}},
        "Mounts": [{"Type": "bind", "Source": str(root),
            "Destination": "/tmp/manyforge-state"}]}


def test_killed_direct_launcher_does_not_let_compose_archive_a_live_child(state, monkeypatch):
    root, descriptor = state
    marker = sessions.prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    # Model a launcher killed after its child opens an owner journal: the root
    # lock disappears, while the child still owns the old file descriptor.
    fcntl.flock(descriptor, fcntl.LOCK_UN)
    journal = root / "current/program-cycle.sqlite3"
    child = subprocess.Popen([sys.executable, "-c",
        "import pathlib,sys; f=pathlib.Path(sys.argv[1]).open('wb'); "
        "f.write(b'old owner'); f.flush(); print('ready',flush=True); "
        "sys.stdin.readline(); f.close()", str(journal)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout is not None and child.stdout.readline() == "ready\n"
        inode = journal.stat().st_ino
        monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k: pytest.fail("direct origin has no Docker authority"))
        with pytest.raises(ValueError, match="termination is unproved"):
            sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
        assert child.poll() is None
        assert journal.stat().st_ino == inode
        assert journal.read_bytes() == b"old owner"
        assert not (root / "previous-sessions").exists()
        assert sessions._marker(root / "current")["session_id"] == marker["session_id"]
    finally:
        child.communicate("finish\n", timeout=5)


def test_recorded_stopped_docker_occurrence_permits_selection(state, monkeypatch):
    root, descriptor, marker = selected_docker(state)
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k:
        SimpleNamespace(returncode=0, stdout=json.dumps(stopped_row(root, marker))))
    current = sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert current["previous_session"]["session_id"] == marker["session_id"]
    assert current["process_boundary"] == {"kind": "docker", "container_id": None}


@pytest.mark.parametrize("change", ["running", "paused", "restarting", "foreign_label", "foreign_root", "foreign_id"])
def test_recorded_docker_must_be_exact_and_stopped(state, monkeypatch, change):
    root, descriptor, marker = selected_docker(state)
    row = stopped_row(root, marker)
    if change in {"running", "paused", "restarting"}:
        row["State"][{"running": "Running", "paused": "Paused", "restarting": "Restarting"}[change]] = True
    elif change == "foreign_label":
        row["Config"]["Labels"]["manyforge.workbench-session"] = uuid.uuid4().hex
    elif change == "foreign_root":
        row["Mounts"][0]["Source"] = str(root.parent / "foreign")
    else:
        row["Id"] = "b" * 64
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k:
        SimpleNamespace(returncode=0, stdout=json.dumps(row)))
    with pytest.raises(ValueError, match="live, changed or foreign"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert sessions._marker(root / "current")["session_id"] == marker["session_id"]


@pytest.mark.parametrize("returncode,inventory,permitted", [(0, "", True), (1, "", False), (0, "a" * 64 + "\n", False), (0, "invalid\n", False)])
def test_removed_exact_occurrence_requires_successful_docker_authority(state, monkeypatch, returncode, inventory, permitted):
    root, descriptor, marker = selected_docker(state)
    calls = []
    def run(command, **_kwargs):
        calls.append(command)
        return SimpleNamespace(returncode=1, stdout="") if len(calls) == 1 else SimpleNamespace(returncode=returncode, stdout=inventory)
    monkeypatch.setattr(sessions.subprocess, "run", run)
    if permitted:
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    else:
        with pytest.raises(ValueError, match="could not prove"):
            sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
        assert sessions._marker(root / "current")["session_id"] == marker["session_id"]
    assert calls[0][-1] == "a" * 64
    assert calls[1] == ["docker", "container", "ls", "--all", "--quiet", "--no-trunc"]


def test_unbound_docker_crash_is_preserved(state, monkeypatch):
    root, descriptor = state
    marker = sessions.prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor,
        process_boundary={"kind": "docker", "container_id": None})
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k: pytest.fail("no recorded occurrence to inspect"))
    with pytest.raises(ValueError, match="termination is unproved"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert sessions._marker(root / "current")["session_id"] == marker["session_id"]


def test_unknown_root_needs_exact_stopped_capture(state):
    root, descriptor = state
    (root / "program-cycle.sqlite3").write_bytes(b"preserved legacy")
    with pytest.raises(ValueError, match="no exact stopped process-set proof"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    receipt = root.parent / "docker-inspect.json"
    receipt.write_text(json.dumps({"container_id": "b" * 64,
        "state": {"running": False, "status": "exited"},
        "mounts": [{"Type": "bind", "Source": str(root), "Destination": "/tmp/manyforge-state"}]}))
    current = sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor, stopped_container_receipt=receipt)
    assert current["previous_session"]["shutdown"] == "unknown"
    assert (root / current["previous_session"]["archive_relative_path"] / "program-cycle.sqlite3").read_bytes() == b"preserved legacy"


def test_direct_start_can_use_recorded_exact_docker_proof(state, monkeypatch):
    root, descriptor, marker = selected_docker(state)
    fcntl.flock(descriptor, fcntl.LOCK_UN)
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k:
        SimpleNamespace(returncode=0, stdout=json.dumps(stopped_row(root, marker))))
    direct = sessions.Session.direct(root)
    try:
        assert direct.marker["process_boundary"] == {"kind": "direct"}
        assert direct.marker["previous_session"]["session_id"] == marker["session_id"]
    finally:
        direct.close()


def test_direct_fresh_and_clean_closed_starts_remain_supported(state):
    root, _descriptor = state
    first = sessions.Session.direct(root)
    identity = first.marker["session_id"]
    first.stopped(verified=True)
    first.close()
    successor = sessions.Session.direct(root)
    try:
        assert successor.marker["previous_session"]["session_id"] == identity
        assert successor.marker["previous_session"]["shutdown"] == "clean"
    finally:
        successor.close()
