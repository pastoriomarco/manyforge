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


@pytest.fixture(autouse=True)
def roomy_disk(monkeypatch):
    """Selection checks the free-space floor first; these tests are not about it."""
    monkeypatch.setattr(sessions.run_session, "read_disk",
                        lambda _fd: sessions.run_session.DiskReading(10**15, 10**15, 10**6))


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
        assert not (root / "previous-runs").exists()
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


@pytest.mark.parametrize("captured", [False, True])
def test_removed_occurrence_warns_once_when_its_console_was_not_captured(state, monkeypatch, capsys, captured):
    root, descriptor, _ = selected_docker(state)
    # A partial capture or another container's capture is not this console.
    for name, identity in (("20261124T000000Z.partial", "a" * 64), ("20261124T000001Z", "b" * 64),
                           *((("20261124T000002Z", "a" * 64),) if captured else ())):
        (root / "evidence" / name).mkdir(parents=True)
        (root / "evidence" / name / "docker-inspect.json").write_text(json.dumps({"container_id": identity}))
    monkeypatch.setattr(sessions.subprocess, "run", lambda command, **_k:
        SimpleNamespace(returncode=1 if command[1] == "inspect" else 0, stdout=""))
    sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    warning = f"WARNING ManyForge previous container {'a' * 64} was removed without console evidence\n"
    assert capsys.readouterr().err == ("" if captured else warning)


def test_unbound_docker_crash_is_preserved(state, monkeypatch):
    root, descriptor = state
    marker = sessions.prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor,
        process_boundary={"kind": "docker", "container_id": None})
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k: pytest.fail("no recorded occurrence to inspect"))
    with pytest.raises(ValueError, match="termination is unproved"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert sessions._marker(root / "current")["session_id"] == marker["session_id"]


def test_simulated_plc_settings_survive_a_session_switch(state):
    root, descriptor = state
    settings = root / "sim_plc"
    settings.mkdir()
    (settings / "plc_io.yaml").write_text("ready: false\n")
    sessions.prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    sessions.prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert (settings / "plc_io.yaml").read_text() == "ready: false\n"


def test_an_old_pre_session_layout_is_refused_and_preserved(state):
    root, descriptor = state
    (root / "program-cycle.sqlite3").write_bytes(b"preserved legacy")
    with pytest.raises(ValueError, match="unknown state-root entries preserved"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert (root / "program-cycle.sqlite3").read_bytes() == b"preserved legacy"
    assert not (root / "current").exists()


def test_unmarked_current_needs_exact_stopped_capture(state):
    root, descriptor = state
    (root / "current").mkdir()
    (root / "current" / "program-cycle.sqlite3").write_bytes(b"preserved unmarked")
    with pytest.raises(ValueError, match="no exact stopped process-set proof"):
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    receipt = root.parent / "docker-inspect.json"
    receipt.write_text(json.dumps({"container_id": "b" * 64,
        "state": {"running": False, "status": "exited"},
        "mounts": [{"Type": "bind", "Source": str(root), "Destination": "/tmp/manyforge-state"}]}))
    current = sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor, stopped_container_receipt=receipt)
    assert current["previous_session"]["shutdown"] == "unknown"
    assert (root / current["previous_session"]["archive_relative_path"] / "program-cycle.sqlite3").read_bytes() == b"preserved unmarked"


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


def older_marker(state, *, container_id: str | None = "a" * 64):
    """A stopped, clean session of the earlier marker format (an older build)."""
    root, descriptor, marker = selected_docker(state)
    path = root / "current/session.json"
    older = json.loads(path.read_text())
    del older["application_end"], older["manager_exit"]
    older.update(schema_version="manyforge.workbench_session.v2", disposition="stopped",
                 shutdown="clean", process_boundary={"kind": "docker", "container_id": container_id})
    path.write_text(json.dumps(older))
    return root, descriptor, marker


def docker(row: dict | None, running_labelled: str = ""):
    """inspect answers ``row`` (None: no such container); ls and ps answer."""
    def run(command, **_kwargs):
        if command[1] == "inspect":
            return SimpleNamespace(returncode=1, stdout="") if row is None else SimpleNamespace(
                returncode=0, stdout=json.dumps(row))
        if command[1] == "ps":
            return SimpleNamespace(returncode=0, stdout=running_labelled)
        return SimpleNamespace(returncode=0, stdout="")
    return run


@pytest.mark.parametrize("removed", [False, True])
def test_a_stopped_session_of_an_older_build_is_archived_and_the_start_proceeds(state, monkeypatch, capsys, removed):
    root, descriptor, marker = older_marker(state)
    monkeypatch.setattr(sessions.subprocess, "run",
                        docker(None if removed else stopped_row(root, marker)))
    identity = uuid.uuid4().hex
    current = sessions.prepare_docker_session(root, identity, lock_fd=descriptor)
    assert current["session_id"] == identity
    assert sessions._marker(root / "current")["session_id"] == identity
    archive = current["previous_session"]["archive_relative_path"]
    assert (root / archive / "session.json").is_file()
    # The report names the same older marker format and archive as the line.
    assert current["previous_session"]["archived_older_marker"] == "manyforge.workbench_session.v2"
    assert f"was archived to {archive} at the upgrade." in current["previous_session"]["message"]
    line = capsys.readouterr().err.splitlines()[-1]
    assert line == ("ManyForge archived the previous session (marker manyforge.workbench_session.v2, "
                    f"an older build) to {root / archive}")


def test_an_older_session_without_a_bound_container_is_archived_when_none_of_its_containers_runs(state, monkeypatch):
    root, descriptor, _ = older_marker(state, container_id=None)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return docker(None)(command, **kwargs)

    monkeypatch.setattr(sessions.subprocess, "run", run)
    sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert [command[1] for command in calls] == ["ps"]


@pytest.mark.parametrize("live", ["running_container", "running_labelled", "docker_unavailable"])
def test_an_older_session_that_may_still_run_is_refused_with_guidance(state, monkeypatch, capsys, live):
    root, descriptor, marker = older_marker(state)
    row = stopped_row(root, marker)
    if live == "running_container":
        row["State"].update(Status="running", Running=True)
        run = docker(row)
    elif live == "running_labelled":
        run = docker(row, running_labelled="c" * 64 + "\n")
    else:
        run = lambda command, **_k: SimpleNamespace(returncode=1, stdout="")  # noqa: E731
    monkeypatch.setattr(sessions.subprocess, "run", run)
    before = (root / "current/session.json").read_bytes()
    with pytest.raises(sessions.SessionMarkerOutdated) as refused:
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    message = str(refused.value)
    assert "manyforge.workbench_session.v2 (an older build)" in message
    assert "stop it with the build that started it" in message
    assert (root / "current/session.json").read_bytes() == before
    assert not (root / "previous-runs").exists() or not any((root / "previous-runs").iterdir())
    monkeypatch.setattr("sys.argv", ["workbench_session.py", str(root), "--session-id",
                                     uuid.uuid4().hex, "--lock-fd", str(descriptor)])
    assert sessions.main() == 69
    assert capsys.readouterr().err.startswith("development_workbench_session_marker_outdated: ")


def test_an_unreadable_marker_keeps_the_proof_refusal(state, monkeypatch):
    root, descriptor, _ = selected_docker(state)
    (root / "current/session.json").write_text("{}")
    monkeypatch.setattr(sessions.subprocess, "run", lambda *_a, **_k: pytest.fail("no Docker question"))
    with pytest.raises(ValueError, match="no exact stopped process-set proof") as refused:
        sessions.prepare_docker_session(root, uuid.uuid4().hex, lock_fd=descriptor)
    assert not isinstance(refused.value, sessions.SessionMarkerOutdated)
