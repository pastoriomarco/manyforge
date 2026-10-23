from __future__ import annotations

from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat

import pytest

from scripts.lib import workbench_session as sessions


FIRST = "a" * 32
SECOND = "b" * 32
THIRD = "c" * 32
JOURNALS = (
    "program-cycle.sqlite3", "logical-scene.sqlite3", "manager/journal.sqlite3",
    "execution-state/cclc-journal.sqlite3", "execution-state/operation-journal.sqlite3",
)


@contextmanager
def locked_root(root: Path):
    root.mkdir(exist_ok=True)
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield descriptor
    finally:
        os.close(descriptor)


def select(root: Path, identity: str) -> dict:
    with locked_root(root) as descriptor:
        return sessions.prepare_session(root, identity, lock_fd=descriptor)


def evidence(folder: Path) -> dict[str, tuple[bytes, int]]:
    result = {}
    for index, name in enumerate(JOURNALS):
        for suffix in ("", "-wal", "-shm"):
            path = folder / (name + suffix)
            path.parent.mkdir(parents=True, exist_ok=True)
            content = f"original owner evidence {index} {suffix}".encode()
            path.write_bytes(content)
            result[name + suffix] = (content, path.stat().st_ino)
    return result


def assert_evidence(folder: Path, expected: dict[str, tuple[bytes, int]], *, sealed=True):
    for name, (content, inode) in expected.items():
        path = folder / name
        assert path.read_bytes() == content
        assert path.stat().st_ino == inode
        if sealed:
            assert stat.S_IMODE(path.stat().st_mode) & 0o222 == 0
    if sealed:
        assert stat.S_IMODE(folder.stat().st_mode) & 0o222 == 0


def test_fresh_selection_contains_only_marker_and_history_is_stable(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    stable = {}
    for suffix in ("", "-wal", "-shm", "-journal", ".headless-owner.lock"):
        path = root / ("history.sqlite3" + suffix)
        path.write_bytes(("history" + suffix).encode())
        stable[path.name] = (path.read_bytes(), path.stat().st_ino)
    marker = select(root, FIRST)
    assert marker["session_id"] == FIRST
    assert marker["previous_session"] is None
    assert marker["launcher_claimed"] is False
    expected = evidence(root / "current")
    successor = select(root, SECOND)
    assert successor["session_id"] == SECOND
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}
    assert_evidence(root / "previous-sessions" / FIRST, expected)
    for name, (content, inode) in stable.items():
        path = root / name
        assert (path.read_bytes(), path.stat().st_ino) == (content, inode)
    assert not (root / "current/history.sqlite3").exists()
    report = json.loads((root / "session-reports/pending" / (FIRST + ".json")).read_text())
    assert successor["previous_session"] == report
    assert report["archive_relative_path"] == "previous-sessions/" + FIRST
    assert report["history_coverage"] == "not_imported"
    assert set(report["journal_families"]) == {"program_cycle", "scene", "manager", "cclc", "kernel_operation"}


def test_motion_trace_files_remain_stable_across_cold_sessions(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    traces = root / "motion-traces"
    traces.mkdir(mode=0o700)
    trace = traces / ("trace-" + "a" * 64 + ".json")
    trace.write_bytes(b"saved diagnostic evidence")
    trace.chmod(0o600)
    inode = trace.stat().st_ino
    select(root, FIRST)
    select(root, SECOND)
    assert trace.read_bytes() == b"saved diagnostic evidence"
    assert trace.stat().st_ino == inode
    assert stat.S_IMODE(trace.stat().st_mode) == 0o600
    assert not (root / "previous-sessions" / FIRST / "motion-traces").exists()


def test_legacy_layout_moves_all_journals_without_opening_or_checkpointing_owners(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    original = evidence(root)
    history = root / "history.sqlite3"
    history.write_bytes(b"historian survives legacy transition")
    marker = select(root, FIRST)
    archive = root / marker["previous_session"]["archive_relative_path"]
    assert_evidence(archive, original)
    assert all(not (root / name).exists() for name in original)
    assert history.read_bytes() == b"historian survives legacy transition"
    assert not (root / ".legacy-session-transition.json").exists()
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}


@pytest.mark.parametrize("phase", ["after_archive_rename", "before_current_marker", "after_current_marker"])
def test_crash_during_selection_preserves_old_owner_bytes_and_recovers_one_fresh_current(tmp_path, monkeypatch, phase):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = evidence(root / "current")
    write, rename = sessions._write, sessions.os.rename
    crashed = False

    def interrupted_write(path, value):
        nonlocal crashed
        if path == root / "current/session.json" and not crashed:
            if phase == "before_current_marker":
                crashed = True
                raise OSError("injected power loss before current marker")
            if phase == "after_current_marker":
                write(path, value)
                crashed = True
                raise OSError("injected power loss after current marker")
        return write(path, value)

    def interrupted_rename(source, destination):
        nonlocal crashed
        result = rename(source, destination)
        if phase == "after_archive_rename" and Path(source) == root / "current" and not crashed:
            crashed = True
            raise OSError("injected power loss after old owner archive")
        return result

    monkeypatch.setattr(sessions, "_write", interrupted_write)
    monkeypatch.setattr(sessions.os, "rename", interrupted_rename)
    with pytest.raises(OSError, match="injected power loss"):
        select(root, SECOND)
    assert crashed and (root / ".session-selection.json").exists()
    monkeypatch.setattr(sessions, "_write", write)
    monkeypatch.setattr(sessions.os, "rename", rename)
    marker = select(root, THIRD)
    assert marker["session_id"] == THIRD
    assert_evidence(root / "previous-sessions" / FIRST, original)
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}
    assert json.loads((root / "current/session.json").read_text())["session_id"] == THIRD
    assert not (root / ".session-selection.json").exists()
    assert not (root / "previous-sessions" / THIRD).exists()


def test_legacy_partial_rename_resumes_without_losing_journal_or_wal(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    root.mkdir()
    original = evidence(root)
    rename = sessions.os.rename
    crashed = False

    def interrupted(source, destination):
        nonlocal crashed
        result = rename(source, destination)
        if Path(source) == root / "execution-state" and not crashed:
            crashed = True
            raise OSError("injected legacy move crash")
        return result

    monkeypatch.setattr(sessions.os, "rename", interrupted)
    with pytest.raises(OSError, match="injected legacy"):
        select(root, FIRST)
    monkeypatch.setattr(sessions.os, "rename", rename)
    marker = select(root, SECOND)
    assert_evidence(root / marker["previous_session"]["archive_relative_path"], original)
    assert not (root / ".legacy-session-transition.json").exists()


def test_crash_after_legacy_archival_before_selection_keeps_unclean_previous_session(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    root.mkdir()
    original = evidence(root)
    write = sessions._write

    def interrupted(path, value):
        if path == root / ".session-selection.json":
            raise OSError("injected crash before successor selection is durable")
        return write(path, value)

    monkeypatch.setattr(sessions, "_write", interrupted)
    with pytest.raises(OSError, match="injected crash"):
        select(root, FIRST)
    assert not (root / "current").exists()
    archives = list((root / "previous-sessions").iterdir())
    assert len(archives) == 1
    archive = archives[0]
    assert_evidence(archive, original)
    report_path = root / "session-reports/pending" / (archive.name + ".json")
    preserved_report = report_path.read_bytes()
    assert json.loads(preserved_report)["shutdown"] == "unknown"

    monkeypatch.setattr(sessions, "_write", write)
    marker = select(root, SECOND)
    assert marker["previous_session"] == json.loads(preserved_report)
    assert marker["previous_session"]["archive_relative_path"] == "previous-sessions/" + archive.name
    assert report_path.read_bytes() == preserved_report
    assert_evidence(archive, original)
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}


def test_lifetime_lock_prevents_competing_session_selection(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    running = sessions.Session(root, FIRST)
    try:
        with pytest.raises(BlockingIOError):
            select(root, SECOND)
        assert (root / "current/session.json").exists()
        assert not (root / "previous-sessions").exists()
    finally:
        running.close()


def test_preselected_launcher_claim_is_single_use_and_wrong_identity_cannot_start(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    with pytest.raises(ValueError, match="changed"):
        sessions.Session(root, SECOND)
    running = sessions.Session(root, FIRST)
    running.stopped(verified=True)
    running.close()
    with pytest.raises(ValueError, match="already has a launcher"):
        sessions.Session(root, FIRST)
    marker = json.loads((root / "current/session.json").read_text())
    assert marker["disposition"] == "stopped" and marker["shutdown"] == "clean"


def test_contender_rechecks_claimed_marker_after_acquiring_the_lifetime_lock(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    select(root, FIRST)
    flock = sessions.fcntl.flock
    first_launcher_won = False
    winner_marker = None

    def another_launcher_wins_before_acquisition(descriptor, operation):
        nonlocal first_launcher_won, winner_marker
        if not first_launcher_won:
            # The contender already read launcher_claimed=false. A real
            # competing launcher now claims the same selection, then crashes
            # without closing the marker before this contender gets flock.
            first_launcher_won = True
            winner = sessions.Session(root, FIRST)
            winner.close()
            winner_marker = (root / "current/session.json").read_bytes()
        return flock(descriptor, operation)

    monkeypatch.setattr(sessions.fcntl, "flock", another_launcher_wins_before_acquisition)
    contender = None
    try:
        with pytest.raises(ValueError, match="selected workbench session"):
            contender = sessions.Session(root, FIRST)
    finally:
        if contender is not None:
            contender.close()
    assert first_launcher_won
    assert (root / "current/session.json").read_bytes() == winner_marker
    assert not (root / "previous-sessions").exists()


def test_session_id_cannot_be_reused_as_both_current_and_archive(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = (root / "current/session.json").read_bytes()
    with pytest.raises(ValueError):
        select(root, FIRST)
    assert (root / "current/session.json").read_bytes() == original
    assert not (root / "previous-sessions" / FIRST).exists()


@pytest.mark.parametrize("invalid", ["../elsewhere", "A" * 32, "a" * 31, "a" * 33])
def test_invalid_selection_identity_is_refused_before_mutation(tmp_path, invalid):
    root = tmp_path / "runtime"
    root.mkdir()
    with pytest.raises(ValueError, match="invalid session identity"):
        select(root, invalid)
    assert list(root.iterdir()) == []


def test_unknown_root_entry_is_preserved_and_refuses_selection(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    unknown = root / "operator-owned-file"
    unknown.write_bytes(b"not ours to archive")
    with pytest.raises(ValueError, match="unknown state-root"):
        select(root, FIRST)
    assert unknown.read_bytes() == b"not ours to archive"
    assert not (root / "current").exists()


def test_symlinked_current_never_moves_or_rewrites_external_owner_evidence(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    external = tmp_path / "other-product"
    external.mkdir()
    protected = external / "session.json"
    protected.write_bytes(b"external owner")
    (root / "current").symlink_to(external, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink"):
        select(root, FIRST)
    assert protected.read_bytes() == b"external owner"
    assert (root / "current").is_symlink()


def test_mounted_current_is_not_archived(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = evidence(root / "current")
    monkeypatch.setattr(sessions, "_mounted_children", lambda folder: {root / "current/mount"} if folder == root else set())
    with pytest.raises(ValueError, match="mounted child"):
        select(root, SECOND)
    assert_evidence(root / "current", original, sealed=False)
    assert not (root / "previous-sessions" / FIRST).exists()


@pytest.mark.parametrize("condition,expected", [
    ("idle", "unknown"), ("pending_acquire", "unresolved"), ("uncertain_storage", "unresolved"),
])
def test_classifier_observes_actual_program_journal_without_instantiating_old_owner(tmp_path, condition, expected):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    path = tmp_path / "program-cycle.sqlite3"
    # Build the fixture through the real producer and close it before the
    # offline read. The classifier must never reopen it through that owner.
    owner = ProgramCycleJournal(path, owner_epoch="fixture.old")
    if condition == "pending_acquire":
        owner.begin_acquire(expected_generation=1, provisional_run_id="run.1", evidence={"request_id": "acquire.1"})
    if condition == "uncertain_storage":
        owner.record_request(owner_epoch="composer.1", request_id="persist.1", kind="persistent_value_set",
            intent={"request_id": "persist.1"}, cycle_generation=1)
        owner.complete_persistent_value_request(owner_epoch="composer.1", request_id="persist.1", outcome="storage_uncertain")
    owner.close()
    original = {p.name: (p.read_bytes(), p.stat().st_ino) for p in tmp_path.iterdir() if p.is_file()}
    assert sessions.classify_effects(tmp_path) == expected
    assert {p.name: (p.read_bytes(), p.stat().st_ino) for p in tmp_path.iterdir() if p.is_file()} == original


def test_classifier_never_mistakes_uncheckpointed_wal_for_closed_owner_facts(tmp_path):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    path = tmp_path / "program-cycle.sqlite3"
    owner = ProgramCycleJournal(path, owner_epoch="fixture.old")
    owner.begin_acquire(expected_generation=1, provisional_run_id="run.1", evidence={"request_id": "acquire.1"})
    owner.close()
    wal = path.with_name(path.name + "-wal")
    wal.write_bytes(b"uncheckpointed evidence cannot be ignored")
    before = {p.name: p.read_bytes() for p in tmp_path.iterdir() if p.is_file()}
    assert sessions.classify_effects(tmp_path) == "unknown"
    assert {p.name: p.read_bytes() for p in tmp_path.iterdir() if p.is_file()} == before
