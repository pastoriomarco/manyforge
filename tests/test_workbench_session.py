from __future__ import annotations

from contextlib import closing, contextmanager
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
    assert "Up to five previous sessions are normally kept" in report["message"]
    assert set(report["journal_families"]) == {"program_cycle", "scene", "manager", "cclc", "kernel_operation"}


def test_old_report_text_keeps_an_interrupted_session_selection_recoverable(tmp_path, monkeypatch):
    root = tmp_path / "runtime"
    select(root, FIRST)
    original = evidence(root / "current")
    write = sessions._write

    def interrupted(path, value):
        if path == root / "current/session.json":
            raise OSError("injected interruption before current marker")
        return write(path, value)

    monkeypatch.setattr(sessions, "_write", interrupted)
    with pytest.raises(OSError, match="injected interruption"):
        select(root, SECOND)
    monkeypatch.setattr(sessions, "_write", write)

    selection_path = root / ".session-selection.json"
    selection = json.loads(selection_path.read_text())
    previous = selection["marker"]["previous_session"]
    previous["message"] = previous["message"].replace(
        " Up to five previous sessions are normally kept; copy any journals you want to retain.",
        " All previous journals are kept for investigation.")
    pending = root / "session-reports/pending" / (FIRST + ".json")
    pending.write_text(json.dumps(previous))
    selection_path.write_text(json.dumps(selection))

    marker = select(root, THIRD)
    assert marker["session_id"] == THIRD
    assert json.loads(pending.read_text()) == previous
    assert_evidence(root / "previous-sessions" / FIRST, original)
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
    ("idle", "unknown"), ("pending_acquire", "unresolved"),
    ("uncertain_storage", "unresolved"), ("reset_effect", "unresolved"),
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
    if condition == "reset_effect":
        owner.begin_idle_resetting(expected_generation=1, owner_epoch="fixture.old", correlation_id="reset.1",
            session_id=None, client_sequence=None, phase="restoring_position")
        owner.remember_reset_effect_claim(expected_generation=1, expected=None,
            retained={"intent": {"expected_admission_generation": 41}, "claim": None, "release_intent": None})
    owner.close()
    original = {p.name: (p.read_bytes(), p.stat().st_ino) for p in tmp_path.iterdir() if p.is_file()}
    assert sessions.classify_effects(tmp_path) == expected
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
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert ("program_cycle" in unresolved and "program_cycle" not in unknown) is (expected == "unresolved")
    assert ("program_cycle" in unknown) is (expected == "unknown")
    assert path.read_bytes() == original


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


# Owner columns read by the kernel_operation classifier. stop_projections is the
# v16 layout (payload copies, CHECK-paired with each sequence) or the v17
# sequences-only layout; the classifier predicate reads only *_sequence.
KERNEL_COMMON_DDL = """
    CREATE TABLE journal_records(sequence INTEGER PRIMARY KEY, owner_epoch TEXT NOT NULL,
      record_kind TEXT NOT NULL, payload_json TEXT NOT NULL, payload_digest TEXT NOT NULL,
      occurred_at_ns INTEGER NOT NULL, effect_pending INTEGER NOT NULL);
    CREATE TABLE owner_epochs(owner_epoch TEXT PRIMARY KEY, active INTEGER NOT NULL,
      closed_at_ns INTEGER, close_sequence INTEGER);
    CREATE TABLE operation_snapshots(operation_id TEXT PRIMARY KEY, state TEXT NOT NULL, disposition TEXT);
    CREATE TABLE cell_admission_claims(claim_id TEXT PRIMARY KEY, release_owner_role TEXT, reconciled_sequence INTEGER);
"""
STOP_PROJECTIONS_V16 = """
    CREATE TABLE stop_projections(stop_id TEXT NOT NULL, stop_generation INTEGER NOT NULL,
      acceptance_sequence INTEGER NOT NULL UNIQUE, acceptance_payload TEXT NOT NULL, acceptance_digest TEXT NOT NULL,
      terminal_sequence INTEGER UNIQUE, terminal_payload TEXT, terminal_digest TEXT,
      surveillance_sequence INTEGER UNIQUE, surveillance_payload TEXT, surveillance_digest TEXT,
      clear_sequence INTEGER UNIQUE, clear_request_id TEXT, clear_payload TEXT, clear_digest TEXT,
      CHECK ((terminal_sequence IS NULL) = (terminal_payload IS NULL)),
      CHECK ((terminal_sequence IS NULL) = (terminal_digest IS NULL)),
      CHECK ((surveillance_sequence IS NULL) = (surveillance_payload IS NULL)),
      CHECK ((surveillance_sequence IS NULL) = (surveillance_digest IS NULL)),
      CHECK ((clear_sequence IS NULL) = (clear_request_id IS NULL)),
      CHECK ((clear_sequence IS NULL) = (clear_payload IS NULL)),
      CHECK ((clear_sequence IS NULL) = (clear_digest IS NULL)),
      PRIMARY KEY(stop_id, stop_generation),
      FOREIGN KEY(acceptance_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(terminal_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(surveillance_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(clear_sequence) REFERENCES journal_records(sequence)) STRICT;
"""
STOP_PROJECTIONS_V17 = """
    CREATE TABLE stop_projections(stop_id TEXT NOT NULL, stop_generation INTEGER NOT NULL,
      acceptance_sequence INTEGER NOT NULL UNIQUE, accepted_payload TEXT NOT NULL,
      accepted_digest TEXT NOT NULL,
      terminal_sequence INTEGER UNIQUE, surveillance_sequence INTEGER UNIQUE, clear_sequence INTEGER UNIQUE,
      PRIMARY KEY(stop_id, stop_generation),
      FOREIGN KEY(acceptance_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(terminal_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(surveillance_sequence) REFERENCES journal_records(sequence),
      FOREIGN KEY(clear_sequence) REFERENCES journal_records(sequence)) STRICT;
"""


def kernel_archive(folder: Path, version: int, stop: str) -> Path:
    import hashlib

    path = folder / "execution-state/operation-journal.sqlite3"
    path.parent.mkdir(parents=True, exist_ok=True)
    recorded = {"accepted": (2,), "terminal": (2, 3), "surveillance": (2, 3, 4), "cleared": (2, 3, 4, 5)}[stop]
    acceptance, terminal, surveillance, clear = (recorded + (None,) * 4)[:4]
    with closing(sqlite3.connect(path)) as database, database:
        database.executescript(KERNEL_COMMON_DDL + (STOP_PROJECTIONS_V16 if version == 16 else STOP_PROJECTIONS_V17))
        database.execute(f"PRAGMA application_id={0x4D46454A}")
        database.execute(f"PRAGMA user_version={version}")
        close = json.dumps({"closed_at_ns": 100, "owner_epoch": "epoch.1"}, sort_keys=True, separators=(",", ":"))
        database.execute("INSERT INTO journal_records VALUES(1,'epoch.1','owner_epoch_closed',?,?,100,0)",
                         (close, hashlib.sha256(close.encode()).hexdigest()))
        for sequence in recorded:
            database.execute("INSERT INTO journal_records VALUES(?,'epoch.1','stop','{}','digest',1,0)", (sequence,))
        database.execute("INSERT INTO owner_epochs VALUES('epoch.1',0,100,1)")
        if version == 16:
            payload = lambda sequence: None if sequence is None else "{}"
            database.execute("INSERT INTO stop_projections VALUES('stop.1',1,?,'{}','d',?,?,?,?,?,?,?,?,?,?)",
                (acceptance, terminal, payload(terminal), payload(terminal), surveillance, payload(surveillance),
                 payload(surveillance), clear, payload(clear), payload(clear), payload(clear)))
        else:
            database.execute("INSERT INTO stop_projections VALUES('stop.1',1,?,'{}','d',?,?,?)",
                             (acceptance, terminal, surveillance, clear))
    return path


@pytest.mark.parametrize("version", [16, 17])
@pytest.mark.parametrize("stop,expected", [
    ("accepted", "unresolved"), ("terminal", "closed"), ("surveillance", "unresolved"), ("cleared", "closed"),
])
def test_classifier_reads_v16_and_v17_kernel_stop_records_alike(tmp_path, version, stop, expected):
    path = kernel_archive(tmp_path, version, stop)
    original = path.read_bytes()
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert "kernel_operation" not in unknown
    assert ("unresolved" if "kernel_operation" in unresolved else "closed") == expected
    assert path.read_bytes() == original


@pytest.mark.parametrize("version,known", [(15, True), (18, True), (0, False)])
def test_classifier_reads_kernel_journal_version_from_its_metadata(tmp_path, version, known):
    path = kernel_archive(tmp_path, 17, "terminal")
    with closing(sqlite3.connect(path)) as database:
        database.execute(f"PRAGMA user_version={version}")
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert ("kernel_operation" not in unknown) is known
    assert "kernel_operation" not in unresolved


@pytest.mark.parametrize("version,known", [(21, True), (19, True), (22, True), (0, False)])
def test_classifier_reads_cclc_journal_version_from_its_metadata(tmp_path, version, known):
    path = tmp_path / "execution-state/cclc-journal.sqlite3"
    path.parent.mkdir(parents=True)
    # Columns read by the cclc predicates; all exist in the v21 owner DDL.
    with closing(sqlite3.connect(path)) as database, database:
        database.executescript("""
            CREATE TABLE lifecycle_transactions(transaction_sequence INTEGER PRIMARY KEY, terminal INTEGER NOT NULL);
            CREATE TABLE route_selection_transactions(transaction_sequence INTEGER PRIMARY KEY, terminal INTEGER NOT NULL);
            CREATE TABLE cclc_effect_attempts(attempt_sequence INTEGER PRIMARY KEY, completed_attempt_blob BLOB);
            CREATE TABLE cclc_route_effect_attempts(attempt_sequence INTEGER PRIMARY KEY, completed_attempt_blob BLOB);
            CREATE TABLE cclc_lifecycle_cell_claims(claim_sequence INTEGER PRIMARY KEY, release_receipt_recorded INTEGER NOT NULL);
            CREATE TABLE cclc_native_apply_reservations(reservation_id TEXT PRIMARY KEY);
            CREATE TABLE cclc_component_incidents(incident_id TEXT PRIMARY KEY, acknowledged INTEGER);
        """)
        database.execute(f"PRAGMA application_id={0x4D464C4A}")
        database.execute(f"PRAGMA user_version={version}")
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert ("cclc" not in unknown) is known
    assert "cclc" not in unresolved


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
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert ("manager" not in unknown) is known
    assert "manager" not in unresolved


def test_classifier_never_reports_a_family_unknown_for_a_newer_schema_version(tmp_path):
    # Tiny archive with the cclc metadata of a schema version the classifier
    # has never seen: the version comes from the journal itself, so a clean
    # journal of that family is classified by its rows, not marked unknown.
    path = tmp_path / "execution-state/cclc-journal.sqlite3"
    path.parent.mkdir(parents=True)
    with closing(sqlite3.connect(path)) as database, database:
        database.executescript("""
            CREATE TABLE lifecycle_transactions(transaction_sequence INTEGER PRIMARY KEY, terminal INTEGER NOT NULL);
            CREATE TABLE route_selection_transactions(transaction_sequence INTEGER PRIMARY KEY, terminal INTEGER NOT NULL);
            CREATE TABLE cclc_effect_attempts(attempt_sequence INTEGER PRIMARY KEY, completed_attempt_blob BLOB);
            CREATE TABLE cclc_route_effect_attempts(attempt_sequence INTEGER PRIMARY KEY, completed_attempt_blob BLOB);
            CREATE TABLE cclc_lifecycle_cell_claims(claim_sequence INTEGER PRIMARY KEY, release_receipt_recorded INTEGER NOT NULL);
            CREATE TABLE cclc_native_apply_reservations(reservation_id TEXT PRIMARY KEY);
            CREATE TABLE cclc_component_incidents(incident_id TEXT PRIMARY KEY, acknowledged INTEGER);
            INSERT INTO cclc_component_incidents VALUES('incident.1', 1);
        """)
        database.execute(f"PRAGMA application_id={0x4D464C4A}")
        database.execute("PRAGMA user_version=99")
    kernel_archive(tmp_path, 17, "cleared")
    with closing(sqlite3.connect(tmp_path / "execution-state/operation-journal.sqlite3")) as database:
        database.execute("PRAGMA user_version=99")
    _, unknown, unresolved = sessions._effect_observations(tmp_path)
    assert "cclc" not in unknown and "kernel_operation" not in unknown
    assert "cclc" not in unresolved and "kernel_operation" not in unresolved
