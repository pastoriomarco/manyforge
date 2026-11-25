from __future__ import annotations

from contextlib import closing, contextmanager
from datetime import datetime, timedelta
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import stat

import pytest

from scripts.lib import workbench_session as sessions


FIRST = "a" * 32
SECOND = "b" * 32
THIRD = "c" * 32
JOURNALS = (
    "program-cycle.sqlite3", "manager/journal.sqlite3",
    "execution-state/operation-journal.sqlite3",
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


@pytest.fixture(autouse=True)
def roomy_disk(monkeypatch):
    """Selection checks the free-space floor first; these tests are not about it."""
    monkeypatch.setattr(sessions.run_session, "read_disk",
                        lambda _fd: sessions.run_session.DiskReading(10**15, 10**15, 10**6))


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
    history = root / "history"
    history.mkdir()
    current_history = history / "history.v7.sqlite3"
    current_history.write_bytes(b"current history")
    marker = select(root, FIRST)
    assert marker["session_id"] == FIRST
    assert marker["previous_session"] is None
    assert marker["launcher_claimed"] is False
    expected = evidence(root / "current")
    successor = select(root, SECOND)
    assert successor["session_id"] == SECOND
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}
    assert_evidence(root / "previous-runs" / FIRST, expected)
    for name, (content, inode) in stable.items():
        path = root / name
        assert (path.read_bytes(), path.stat().st_ino) == (content, inode)
    assert not (root / "current/history.sqlite3").exists()
    assert current_history.read_bytes() == b"current history"
    report = json.loads((root / "session-reports/pending" / (FIRST + ".json")).read_text())
    assert successor["previous_session"] == report
    assert report["archive_relative_path"] == "previous-runs/" + FIRST
    assert report["history_coverage"] == "not_imported"
    assert "Up to five previous runs (2 GiB in total) are normally kept" in report["message"]
    assert report["journal_families"] == ["program_cycle", "manager", "kernel_operation"]
    assert report["schema_version"] == "manyforge.workbench_previous_session.v4"
    # No Kernel journal in that run: the unfinished operations are unknown.
    assert report["unfinished_operations"] is None
    assert report["unfinished_operation_count"] is None
    assert report["application_start_id"] is None
    # The Manager wrote no application end in that run.
    assert report["application_end"] is None
    assert report["archived_older_marker"] is None
    assert marker["application_end"] is None and marker["manager_exit"] is None
    # History files the report at the previous start; the report time is UTC.
    assert report["started_at_wall"] == marker["started_at_wall"]
    assert datetime.fromisoformat(report["reported_at_wall"]).utcoffset() == timedelta(0)
    assert len(report) == 15


def test_report_lists_unfinished_operations_from_a_copy_of_the_journal_and_its_wal(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    path = root / "current/execution-state/operation-journal.sqlite3"
    path.parent.mkdir(parents=True)
    database = sqlite3.connect(path)
    try:
        # The rows exist only in the WAL while this connection stays open, as
        # after a crash; an immutable read would call them unknown (BS 4.4).
        database.execute("PRAGMA journal_mode=WAL")
        database.execute("PRAGMA wal_autocheckpoint=0")
        database.execute("CREATE TABLE operation_snapshots(operation_id INTEGER PRIMARY KEY,"
                         " state TEXT NOT NULL)")
        database.executemany("INSERT INTO operation_snapshots VALUES(?,?)",
                             [(index, "executing") for index in range(1, 72)])
        database.execute("INSERT INTO operation_snapshots VALUES(72,'completed')")
        # The start these numbers belong to; History files the report under it.
        database.execute("CREATE TABLE journal_metadata(singleton INTEGER PRIMARY KEY,"
                         " application_start_id TEXT)")
        database.execute("INSERT INTO journal_metadata VALUES(1,'3f9a0000000000a1')")
        database.commit()
        assert path.with_name(path.name + "-wal").stat().st_size > 0
        marker = select(root, SECOND)
    finally:
        database.close()
    report = marker["previous_session"]
    assert report["archive_relative_path"] == "previous-runs/" + FIRST
    assert report["unfinished_operation_count"] == 71
    assert report["unfinished_operations"] == [
        {"operation_id": operation_id, "disposition": "host_replaced"}
        for operation_id in range(1, 65)]
    assert "71 unfinished operations are marked host_replaced." in report["message"]
    assert report["application_start_id"] == "3f9a0000000000a1"
    # The copy is deleted after the read; nothing is left beside the reports.
    assert not any(entry.name.startswith(".journal-copy.")
                   for entry in (root / "session-reports").iterdir())


def test_old_report_text_keeps_an_interrupted_session_selection_recoverable(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = evidence(root / "current")
    write = sessions.run_session._write

    def interrupted(path, value):
        if path == root / "current/session.json":
            raise OSError("injected interruption before current marker")
        return write(path, value)

    monkeypatch.setattr(sessions.run_session, "_write", interrupted)
    with pytest.raises(OSError, match="injected interruption"):
        select(root, SECOND)
    monkeypatch.setattr(sessions.run_session, "_write", write)

    selection_path = root / ".session-selection.json"
    selection = json.loads(selection_path.read_text())
    previous = selection["marker"]["previous_session"]
    previous["message"] = previous["message"].replace(
        " Up to five previous runs (2 GiB in total) are normally kept; copy any journals you want to retain.",
        " All previous journals are kept for investigation.")
    pending = root / "session-reports/pending" / (FIRST + ".json")
    pending.write_text(json.dumps(previous))
    selection_path.write_text(json.dumps(selection))

    marker = select(root, THIRD)
    assert marker["session_id"] == THIRD
    # The recorded report, its report time included, is kept.
    assert json.loads(pending.read_text()) == previous
    assert_evidence(root / "previous-runs" / FIRST, original)
    assert not selection_path.exists()


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
    assert not (root / "previous-runs" / FIRST / "motion-traces").exists()


def test_an_old_pre_session_layout_is_refused_and_preserved(tmp_path):
    root = tmp_path / "runtime"
    root.mkdir()
    original = evidence(root)
    with pytest.raises(ValueError, match="unknown state-root entries preserved"):
        select(root, FIRST)
    assert_evidence(root, original, sealed=False)
    assert not (root / "current").exists() and not (root / "previous-runs").exists()


@pytest.mark.parametrize("phase", ["after_archive_rename", "before_current_marker", "after_current_marker"])
def test_crash_during_selection_preserves_old_owner_bytes_and_recovers_one_fresh_current(tmp_path, monkeypatch, phase):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = evidence(root / "current")
    write, rename = sessions.run_session._write, sessions.os.rename
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

    monkeypatch.setattr(sessions.run_session, "_write", interrupted_write)
    monkeypatch.setattr(sessions.os, "rename", interrupted_rename)
    with pytest.raises(OSError, match="injected power loss"):
        select(root, SECOND)
    assert crashed and (root / ".session-selection.json").exists()
    monkeypatch.setattr(sessions.run_session, "_write", write)
    monkeypatch.setattr(sessions.os, "rename", rename)
    marker = select(root, THIRD)
    assert marker["session_id"] == THIRD
    assert_evidence(root / "previous-runs" / FIRST, original)
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}
    assert json.loads((root / "current/session.json").read_text())["session_id"] == THIRD
    assert not (root / ".session-selection.json").exists()
    assert not (root / "previous-runs" / THIRD).exists()


def test_lifetime_lock_prevents_competing_session_selection(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    running = sessions.Session(root, FIRST)
    try:
        with pytest.raises(BlockingIOError):
            select(root, SECOND)
        assert (root / "current/session.json").exists()
        assert not (root / "previous-runs").exists()
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
    assert marker["schema_version"] == "manyforge.workbench_session.v3"
    assert marker["application_end"] is None
    assert marker["manager_exit"] == {"exit_status": None, "signal": None}


APPLICATION_END = {
    "schema_version": "manyforge.application_end.v1",
    "ended_by": "process_exit",
    "process": {"role": "execution_host", "pid": 4242, "exit_status": None,
                "signal": 11, "core_dumped": False},
    "ended_at_wall": "2026-11-24T10:15:30.123Z",
}


def write_application_end(current: Path, value: object) -> None:
    path = current / "manager-run/application-end.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))


@pytest.mark.parametrize(("returncode", "manager_exit"), [
    (72, {"exit_status": 72, "signal": None}),
    (-9, {"exit_status": None, "signal": 9}),
])
def test_stopped_marker_carries_the_application_end_and_the_manager_exit(tmp_path, returncode, manager_exit):
    root = tmp_path / "runtime"
    select(root, FIRST)
    running = sessions.Session(root, FIRST)
    write_application_end(root / "current", APPLICATION_END)
    running.stopped(verified=False, manager_returncode=returncode)
    running.close()
    marker = json.loads((root / "current/session.json").read_text())
    assert marker["application_end"] == APPLICATION_END
    assert marker["manager_exit"] == manager_exit
    # The next selection reads the closed marker and reports the end.
    successor = select(root, SECOND)
    assert successor["previous_session"]["application_end"] == APPLICATION_END
    assert successor["previous_session"]["shutdown"] == "unknown"


@pytest.mark.parametrize("value", [
    dict(APPLICATION_END, schema_version="manyforge.application_end.v2"),
    dict(APPLICATION_END, ended_by="crash"),
    dict(APPLICATION_END, ended_by="stop_signal"),
    dict(APPLICATION_END, ended_at_wall="2026-11-24T10:15:30"),
    dict(APPLICATION_END, process=dict(APPLICATION_END["process"], exit_status=1)),
    dict(APPLICATION_END, process=dict(APPLICATION_END["process"], core_dumped=1)),
    dict(APPLICATION_END, extra=True),
    [APPLICATION_END],
])
def test_an_invalid_application_end_is_reported_as_unknown(tmp_path, value):
    root = tmp_path / "runtime"
    select(root, FIRST)
    write_application_end(root / "current", value)
    assert sessions.run_session.application_end(root / "current") is None
    assert select(root, SECOND)["previous_session"]["application_end"] is None


def test_a_signal_shutdown_or_manager_failure_end_has_no_process(tmp_path):
    for ended_by in ("stop_signal", "shutdown_runtime", "manager_failure"):
        value = dict(APPLICATION_END, ended_by=ended_by, process=None)
        write_application_end(tmp_path, value)
        assert sessions.run_session.application_end(tmp_path) == value
    # A Manager failure names no process.
    write_application_end(tmp_path, dict(APPLICATION_END, ended_by="manager_failure"))
    assert sessions.run_session.application_end(tmp_path) is None


def test_a_manager_failure_end_reaches_the_next_report(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    running = sessions.Session(root, FIRST)
    value = dict(APPLICATION_END, ended_by="manager_failure", process=None)
    write_application_end(root / "current", value)
    running.stopped(verified=False, manager_returncode=70)
    running.close()
    marker = json.loads((root / "current/session.json").read_text())
    assert marker["application_end"] == value
    assert marker["manager_exit"] == {"exit_status": 70, "signal": None}
    assert select(root, SECOND)["previous_session"]["application_end"] == value


def test_an_application_end_of_an_unclosed_run_is_reported_from_its_archive(tmp_path):
    # The launcher ended without closing the marker; the Manager's record is
    # still in the archived run.
    root = tmp_path / "runtime"
    select(root, FIRST)
    write_application_end(root / "current", APPLICATION_END)
    assert select(root, SECOND)["previous_session"]["application_end"] == APPLICATION_END


def test_a_marker_of_the_earlier_version_is_not_trusted(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    path = root / "current/session.json"
    marker = json.loads(path.read_text())
    del marker["application_end"], marker["manager_exit"]
    path.write_text(json.dumps(dict(marker, schema_version="manyforge.workbench_session.v2",
                                    disposition="stopped", shutdown="clean")))
    report = select(root, SECOND)["previous_session"]
    assert report["shutdown"] == "unknown" and report["effects"] == "unknown"
    assert report["started_at_wall"] is None
    # The report says the session came from an older build and where it went
    # (its untrusted marker's identity is not used).
    archive = report["archive_relative_path"]
    assert (root / archive / "session.json").is_file()
    assert report["archived_older_marker"] == "manyforge.workbench_session.v2"
    assert (" It ran an older build (marker manyforge.workbench_session.v2) and was archived to "
            f"{archive} at the upgrade.") in report["message"]


@pytest.mark.parametrize("schema_version", ["manyforge.workbench_session", "foreign.v2", 2])
def test_an_unreadable_marker_names_no_older_build(tmp_path, schema_version):
    root = tmp_path / "runtime"
    select(root, FIRST)
    path = root / "current/session.json"
    path.write_text(json.dumps(dict(json.loads(path.read_text()), schema_version=schema_version)))
    report = select(root, SECOND)["previous_session"]
    assert report["archived_older_marker"] is None
    assert "older build" not in report["message"]


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
    assert not (root / "previous-runs").exists()


def test_session_id_cannot_be_reused_as_both_current_and_archive(tmp_path):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = (root / "current/session.json").read_bytes()
    with pytest.raises(ValueError):
        select(root, FIRST)
    assert (root / "current/session.json").read_bytes() == original
    assert not (root / "previous-runs" / FIRST).exists()


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
    monkeypatch.setattr(sessions.run_session, "_mounted_children", lambda folder: {root / "current/mount"} if folder == root else set())
    with pytest.raises(ValueError, match="mounted child"):
        select(root, SECOND)
    assert_evidence(root / "current", original, sealed=False)
    assert not (root / "previous-runs" / FIRST).exists()


@pytest.mark.parametrize("condition,expected", [
    ("idle", "unknown"), ("pending_acquire", "unresolved"),
    ("reset_effect", "unresolved"),
])
def test_classifier_observes_actual_program_journal_without_instantiating_old_owner(tmp_path, condition, expected):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    path = tmp_path / "program-cycle.sqlite3"
    # Build the fixture through the real producer and close it before the
    # offline read. The classifier must never reopen it through that owner.
    owner = ProgramCycleJournal(path)
    if condition == "pending_acquire":
        owner.begin_acquire(expected_generation=1, provisional_run_id=1, evidence={"request_id": "acquire.1"})
    if condition == "reset_effect":
        owner.begin_idle_resetting(expected_generation=1, causal_request_key=bytes(16),
            phase="restoring_position")
        owner.remember_reset_effect_claim(expected_generation=1, expected=None,
            retained={"intent": {"expected_admission_generation": 41}, "claim": None, "release_intent": None})
    owner.close()
    original = {p.name: (p.read_bytes(), p.stat().st_ino) for p in tmp_path.iterdir() if p.is_file()}
    assert sessions.run_session.classify_effects(tmp_path) == expected
    assert {p.name: (p.read_bytes(), p.stat().st_ino) for p in tmp_path.iterdir() if p.is_file()} == original


# The schema version is read from the journal's own metadata row, not pinned:
# any formatted version is classified by its rows; an unformatted row is unknown.
@pytest.mark.parametrize("schema,expected", [(7, "unresolved"), (9, "unresolved"), (0, "unknown")])
def test_classifier_reads_program_journal_schema_from_its_metadata_row(tmp_path, schema, expected):
    path = tmp_path / "program-cycle.sqlite3"
    # These are the schema-7 owner columns and two request tables read by the
    # cold-session classifier (85893f0c^). Schema 8 only added the Reset claim.
    with closing(sqlite3.connect(path)) as database, database:
        database.executescript("""
            CREATE TABLE program_cycle_metadata(singleton INTEGER PRIMARY KEY, schema_version INTEGER NOT NULL);
            CREATE TABLE program_cycle(singleton INTEGER PRIMARY KEY, cycle_generation INTEGER NOT NULL,
              state TEXT NOT NULL, owner_epoch TEXT NOT NULL, program_run_id TEXT, claim_json TEXT,
              capture_identities_json TEXT, terminal_reference TEXT, start_settlement_json TEXT,
              release_intent_json TEXT, reset_result_json TEXT, reconciliation_required INTEGER NOT NULL,
              acquisition_pending_json TEXT, reconciliation_evidence_digest TEXT,
              execution_control_revision INTEGER NOT NULL, causal_correlation_id TEXT, causal_session_id TEXT,
              causal_client_sequence TEXT, current_command_kind TEXT, failure_json TEXT, reset_phase TEXT,
              runtime_controls_json TEXT);
            CREATE TABLE program_cycle_inflight_commands(request_owner_epoch TEXT NOT NULL, request_id TEXT NOT NULL,
              request_kind TEXT NOT NULL, intent_json TEXT NOT NULL, cycle_generation INTEGER NOT NULL,
              PRIMARY KEY(request_owner_epoch, request_id));
            CREATE TABLE program_persistent_value_requests(request_owner_epoch TEXT NOT NULL, request_id TEXT NOT NULL,
              action TEXT NOT NULL, outcome TEXT NOT NULL, position INTEGER NOT NULL,
              PRIMARY KEY(request_owner_epoch, request_id));
        """)
        database.execute("INSERT INTO program_cycle_metadata VALUES(1, ?)", (schema,))
        database.execute("""INSERT INTO program_cycle
            (singleton,cycle_generation,state,owner_epoch,program_run_id,reconciliation_required,
             acquisition_pending_json,execution_control_revision)
            VALUES(1,1,'starting','fixture.old','run.1',0,'{}',1)""")
    original = path.read_bytes()
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert ("program_cycle" in unresolved and "program_cycle" not in unknown) is (expected == "unresolved")
    assert ("program_cycle" in unknown) is (expected == "unknown")
    assert path.read_bytes() == original


def test_classifier_never_mistakes_uncheckpointed_wal_for_closed_owner_facts(tmp_path):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    path = tmp_path / "program-cycle.sqlite3"
    owner = ProgramCycleJournal(path)
    owner.begin_acquire(expected_generation=1, provisional_run_id=1, evidence={"request_id": "acquire.1"})
    owner.close()
    wal = path.with_name(path.name + "-wal")
    wal.write_bytes(b"uncheckpointed evidence cannot be ignored")
    before = {p.name: p.read_bytes() for p in tmp_path.iterdir() if p.is_file()}
    assert sessions.run_session.classify_effects(tmp_path) == "unknown"
    assert {p.name: p.read_bytes() for p in tmp_path.iterdir() if p.is_file()} == before


# Columns of journal schema 22 read by the kernel_operation classifier: the
# start named once in the metadata, no owner-epoch or Stop surveillance columns.
KERNEL_DDL = """
    CREATE TABLE journal_metadata(singleton INTEGER PRIMARY KEY, application_start_id TEXT,
      request_capacity INTEGER);
    CREATE TABLE journal_records(sequence INTEGER PRIMARY KEY, record_kind TEXT NOT NULL,
      stop_event_id INTEGER, payload_json TEXT NOT NULL, payload_digest TEXT NOT NULL,
      occurred_at_ns INTEGER NOT NULL);
    CREATE TABLE operation_snapshots(operation_id INTEGER PRIMARY KEY, state TEXT NOT NULL);
"""


# Stop event rows per case: (event id, row kind). With request capacity 2,
# event 1 of "overwritten" has three newer events, so its ring record is gone
# and its completion row can never come: it is finished, not unfinished.
KERNEL_STOP_ROWS = {
    "accepted": ((1, "accepted"),),
    "terminal": ((1, "accepted"), (1, "completed")),
    "overwritten": ((1, "accepted"),) + tuple(
        (event, kind) for event in (2, 3, 4) for kind in ("accepted", "completed")),
}


def kernel_archive(folder: Path, stop: str, start: str | None = "3f9a0000000000a1",
                   claims: tuple[tuple[str, int], ...] = ()) -> Path:
    path = folder / "execution-state/operation-journal.sqlite3"
    path.parent.mkdir(parents=True, exist_ok=True)
    with closing(sqlite3.connect(path)) as database, database:
        database.executescript(KERNEL_DDL)
        database.execute(f"PRAGMA application_id={0x4D46454A}")
        database.execute("PRAGMA user_version=23")
        database.execute("INSERT INTO journal_metadata VALUES(1,?,?)", (start, None if start is None else 2))
        for sequence, (event, kind) in enumerate(KERNEL_STOP_ROWS[stop], start=1):
            database.execute("INSERT INTO journal_records VALUES(?,?,?,'{}','digest',1)",
                             (sequence, "stop_event_" + kind, event))
        # Claim rows (row kind, claim generation), in sequence order.
        for kind, generation in claims:
            database.execute(
                "INSERT INTO journal_records(record_kind,payload_json,payload_digest,occurred_at_ns) "
                "VALUES(?,?,'digest',1)",
                ("cell_claim_" + kind, json.dumps({"claim_generation": generation})))
    return path


@pytest.mark.parametrize("stop,expected", [
    ("accepted", "unresolved"), ("terminal", "closed"), ("overwritten", "closed")])
def test_classifier_reads_kernel_stop_records(tmp_path, stop, expected):
    path = kernel_archive(tmp_path, stop)
    original = path.read_bytes()
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert "kernel_operation" not in unknown
    assert ("unresolved" if "kernel_operation" in unresolved else "closed") == expected
    assert path.read_bytes() == original


@pytest.mark.parametrize("claims,expected", [
    ((("acquired", 1),), "unresolved"),
    ((("acquired", 1), ("released", 1)), "closed"),
    ((("released", 1), ("acquired", 1)), "closed"),
    ((("acquired", 1), ("released", 2)), "unresolved"),
])
def test_classifier_reads_kernel_claim_rows(tmp_path, claims, expected):
    path = kernel_archive(tmp_path, "terminal", claims=claims)
    original = path.read_bytes()
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert "kernel_operation" not in unknown
    assert ("unresolved" if "kernel_operation" in unresolved else "closed") == expected
    assert path.read_bytes() == original


def test_classifier_calls_a_kernel_journal_no_start_opened_unknown(tmp_path):
    kernel_archive(tmp_path, "terminal", start=None)
    _, unknown, _, _ = sessions.run_session._effect_observations(tmp_path)
    assert "kernel_operation" in unknown


@pytest.mark.parametrize("version,known", [(15, True), (22, True), (0, False)])
def test_classifier_reads_kernel_journal_version_from_its_metadata(tmp_path, version, known):
    path = kernel_archive(tmp_path, "terminal")
    with closing(sqlite3.connect(path)) as database:
        database.execute(f"PRAGMA user_version={version}")
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert ("kernel_operation" not in unknown) is known
    assert "kernel_operation" not in unresolved


@pytest.mark.parametrize("version,known", [(19, True), (20, True), (0, False)])
def test_classifier_reads_manager_apply_journal_version_from_its_metadata(tmp_path, version, known):
    # The Manager owners' own resource DDL, including their SQLite identity.
    resources = Path(__file__).resolve().parents[1] / "manyforge_component_manager/resources"
    (tmp_path / "manager").mkdir()
    for name, resource in (("manager-bootstrap-journal", "001_manager_bootstrap_journal"),
                           ("active-bundle-publication", "002_active_bundle_publication_store"),
                           ("manager-apply-journal", "003_manager_apply_journal")):
        with closing(sqlite3.connect(tmp_path / "manager" / (name + ".sqlite3"))) as database:
            database.executescript((resources / (resource + ".sql")).read_text())
    with closing(sqlite3.connect(tmp_path / "manager/manager-apply-journal.sqlite3")) as database:
        database.execute(f"PRAGMA user_version={version}")
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert ("manager" not in unknown) is known
    assert "manager" not in unresolved


def test_classifier_never_reports_a_family_unknown_for_a_newer_schema_version(tmp_path):
    # A journal of a schema version the classifier has never seen: the version
    # comes from the journal itself, so a clean journal of that family is
    # classified by its rows, not marked unknown.
    kernel_archive(tmp_path, "terminal")
    with closing(sqlite3.connect(tmp_path / "execution-state/operation-journal.sqlite3")) as database:
        database.execute("PRAGMA user_version=99")
    _, unknown, unresolved, _ = sessions.run_session._effect_observations(tmp_path)
    assert "kernel_operation" not in unknown
    assert "kernel_operation" not in unresolved


def _insert_metadata_row(database: sqlite3.Connection, table: str) -> None:
    """A metadata row as the store writes it at creation (values are placeholders)."""
    database.execute("PRAGMA ignore_check_constraints=1")
    columns = [(row[1], row[2]) for row in database.execute(f"PRAGMA table_info({table})") if row[1] != "records_lost"]
    database.execute(f"INSERT INTO {table}({','.join(name for name, _ in columns)}) VALUES({','.join('?' for _ in columns)})",
                     [1 if kind == "INTEGER" else "x" for _, kind in columns])


def _mark_records_lost(path: Path, table: str) -> None:
    """Set the mark a record writer leaves when records of its start were not written."""
    with closing(sqlite3.connect(path)) as database, database:
        database.execute(f"UPDATE {table} SET records_lost=1 WHERE singleton=1")


@pytest.mark.parametrize("lost", [False, True])
def test_report_marks_the_program_family_incomplete_from_its_writer_mark(tmp_path, lost):
    from manyforge_program_runner.cycle_journal import ProgramCycleJournal

    root = tmp_path / "runtime"
    select(root, FIRST)
    path = root / "current/program-cycle.sqlite3"
    owner = ProgramCycleJournal(path)
    # A pending acquisition: an unfinished item in the file.
    owner.begin_acquire(expected_generation=1, provisional_run_id=1, evidence={"request_id": "acquire.1"})
    owner.close()
    if lost:
        _mark_records_lost(path, "program_cycle_metadata")
    report = select(root, SECOND)["previous_session"]
    if lost:
        assert report["effects"] == "unknown"
        assert ("Unknown journal families: kernel_operation, manager, program_cycle "
                "(some records of that start could not be written).") in report["message"]
        assert "Unresolved journal families" not in report["message"]
    else:
        assert report["effects"] == "unresolved"
        assert "Unresolved journal families: program_cycle." in report["message"]
        assert "could not be written" not in report["message"]


@pytest.mark.parametrize("store,table", [
    ("manager-bootstrap-journal", "bootstrap_journal_metadata"),
    ("active-bundle-publication", "publication_store_metadata"),
    ("manager-apply-journal", "apply_journal_metadata"),
])
def test_classifier_marks_the_manager_family_incomplete_from_any_store_mark(tmp_path, store, table):
    resources = Path(__file__).resolve().parents[1] / "manyforge_component_manager/resources"
    (tmp_path / "manager").mkdir()
    for name, resource in (("manager-bootstrap-journal", "001_manager_bootstrap_journal"),
                           ("active-bundle-publication", "002_active_bundle_publication_store"),
                           ("manager-apply-journal", "003_manager_apply_journal")):
        with closing(sqlite3.connect(tmp_path / "manager" / (name + ".sqlite3"))) as database:
            database.executescript((resources / (resource + ".sql")).read_text())
    _, unknown, unresolved, incomplete = sessions.run_session._effect_observations(tmp_path)
    assert "manager" not in unknown and "manager" not in incomplete
    path = tmp_path / "manager" / (store + ".sqlite3")
    with closing(sqlite3.connect(path)) as database, database:
        _insert_metadata_row(database, table)
    _mark_records_lost(path, table)
    _, unknown, unresolved, incomplete = sessions.run_session._effect_observations(tmp_path)
    assert "manager" in unknown and "manager" in incomplete
    assert "manager" not in unresolved


def test_the_scene_has_no_journal_family(tmp_path):
    # The Scene is held in memory; a scene file left by an earlier version is
    # archived with the run but is not a journal family.
    (tmp_path / "logical-scene.sqlite3").write_bytes(b"earlier scene file")
    _, unknown, unresolved, incomplete = sessions.run_session._effect_observations(tmp_path)
    assert sorted(unknown) == ["kernel_operation", "manager", "program_cycle"]
    assert unresolved == [] and incomplete == []


def test_a_pending_report_of_the_earlier_version_is_kept_as_recorded(tmp_path):
    # A v2 report (four families, "scene" among them) that Headless had not
    # filed yet stays pending, byte for byte; Headless files it as recorded.
    # (A v3 report is kept the same way.)
    root = tmp_path / "runtime"
    select(root, FIRST)
    select(root, SECOND)
    pending = root / "session-reports/pending" / (FIRST + ".json")
    earlier = dict(json.loads(pending.read_text()),
                   schema_version="manyforge.workbench_previous_session.v2",
                   journal_families=["program_cycle", "scene", "manager", "kernel_operation"])
    pending.write_text(json.dumps(earlier))
    recorded = pending.read_bytes()
    marker = select(root, THIRD)
    assert pending.read_bytes() == recorded
    assert marker["previous_session"]["schema_version"] == "manyforge.workbench_previous_session.v4"
    assert json.loads((root / "session-reports/pending" / (SECOND + ".json")).read_text()) == marker["previous_session"]


def test_selection_below_the_disk_floor_is_refused_before_anything_is_written(tmp_path, monkeypatch, capsys):
    root = tmp_path / "runtime"
    select(root, FIRST)
    before = sorted(path.relative_to(root) for path in root.rglob("*"))
    monkeypatch.setattr(sessions.run_session, "read_disk",
                        lambda _fd: sessions.run_session.DiskReading(10**15, 4_000_000_000, 10**6))
    with pytest.raises(sessions.run_session.DiskBelowFloor) as refused:
        select(root, SECOND)
    assert refused.value.code == "disk_space_below_floor"
    assert str(refused.value) == (
        f"Not started: the disk that holds {root} has 4 GB free. ManyForge needs at least 10 GB free "
        "(10 GB plus 0 GB for the machine). Free 6 GB and start again.")
    assert sorted(path.relative_to(root) for path in root.rglob("*")) == before
    # The host helper reports it typed, with the development exit status.
    with locked_root(root) as descriptor:
        monkeypatch.setattr("sys.argv", ["workbench_session.py", str(root), "--session-id", SECOND,
                                         "--lock-fd", str(descriptor)])
        monkeypatch.setattr(sessions, "_docker_boundary_stopped", lambda *_args: None)
        assert sessions.main() == 69
    assert capsys.readouterr().err.startswith("disk_space_below_floor: Not started: the disk that holds ")


def test_pruning_runs_before_the_selection_writes(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    select(root, FIRST)
    archives = []
    for index in range(6):
        folder = root / "previous-runs" / f"{index + 16:032x}"
        folder.mkdir(parents=True)
        os.utime(folder, ns=(index + 1, index + 1))
        archives.append(folder)
    seen = []
    original = sessions.run_session.prepare_session

    def selection(*args, **kwargs):
        seen.append(sorted(path.name for path in (root / "previous-runs").iterdir()))
        return original(*args, **kwargs)

    monkeypatch.setattr(sessions.run_session, "prepare_session", selection)
    select(root, SECOND)
    # Four previous runs plus current/ (five runs) were kept before the
    # selection archived current/.
    assert seen == [[folder.name for folder in archives[2:]]]
