"""Run selection shared by both launchers; never starts old journal owners.

Before any application process starts, the launcher moves the previous run's
``current/`` directory into ``previous-runs/<id>``, seals it, writes a
previous-run report and creates a fresh, empty ``current/`` (build spec
2026-11-12, 4.2). The development workbench and the operator launcher both
call :func:`prepare_session`; each passes the top-level entries its own root
may hold. The caller holds the root directory's flock for the whole start.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import sys
import tempfile
from typing import Callable, Mapping
import uuid

MARKER_SCHEMA = "manyforge.workbench_session.v2"
SELECTION_SCHEMA = "manyforge.workbench_session_selection.v2"
# v2: the archive is previous-runs/<id>; unfinished operations are listed
# (contract 1.7 "Session report", 1.14; BS 4.4).
REPORT_SCHEMA = "manyforge.workbench_previous_session.v2"
MAXIMUM_REPORTED_OPERATIONS = 64
_TERMINAL_OPERATION_STATES = (
    "completed", "stopped", "failed", "interrupted", "intervention_required", "cancelled_pre_dispatch",
)
_ID = re.compile(r"[0-9a-f]{32}\Z")
_MAX_SCAN_ENTRIES = 100_000
# Exact atomic-write leftovers of this module at the root.
_ROOT_TEMPORARIES = frozenset({"..session-selection.json.next"})
# Entries every session root holds once a session was selected.
SESSION_ENTRIES = frozenset({
    "current", "previous-runs", "session-reports", ".session-selection.json",
})


def _safe_path(path: Path, root: Path | None = None) -> Path:
    # Resolve the selected root once; only descendants are forbidden symlinks.
    if root is None:
        return path.resolve()
    relative = path.relative_to(root)
    current = root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"symlink below session root: {current}")
    return path


def _check_root_identity(root: Path, root_fd: int) -> None:
    path_row = root.stat(follow_symlinks=False)
    held_row = os.fstat(root_fd)
    if (path_row.st_dev, path_row.st_ino) != (held_row.st_dev, held_row.st_ino):
        raise ValueError("selected state root changed during session selection")


def _mounted_children(root: Path) -> set[Path]:
    mounts = set()
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        mount = Path(re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), line.split()[4]))
        if mount != root and mount.is_relative_to(root):
            mounts.add(mount)
    return mounts


def _sync(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write(path: Path, value: dict) -> None:
    """One durable bounded marker, with no symlink following."""
    payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
    if len(payload) > 64 * 1024:
        raise ValueError("session metadata exceeds 64 KiB")
    temporary = path.with_name("." + path.name + ".next")
    # A crash's temporary file contains no owner evidence and is safe to replace.
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    if path.is_symlink():
        raise ValueError("symlinked session metadata")
    os.replace(temporary, path)
    _sync(path.parent)


def _read(path: Path) -> dict:
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("session metadata is not a regular file")
        data = source.read(64 * 1024 + 1)
    if len(data) > 64 * 1024:
        raise ValueError("oversized session metadata")
    value = json.loads(data)
    if not isinstance(value, dict):
        raise ValueError("invalid session metadata")
    return value


def _directory(root: Path, name: str) -> Path:
    path = _safe_path(root / name, root)
    path.mkdir(exist_ok=True)
    if not path.is_dir() or path.stat().st_dev != root.stat().st_dev:
        raise ValueError("session directory is not on the state filesystem")
    _sync(root)
    return path


def _marker(folder: Path) -> dict | None:
    try:
        marker = _read(folder / "session.json")
        if (set(marker) != {"schema_version", "session_id", "started_at_wall", "disposition", "shutdown", "effects", "launcher_claimed", "previous_session", "process_boundary"}
                or marker.get("schema_version") != MARKER_SCHEMA
                or not _ID.fullmatch(str(marker.get("session_id", "")))
                or marker.get("disposition") not in {"started", "stopped"}
                or marker.get("shutdown") not in {"unknown", "clean"}
                or marker.get("effects") not in {"unknown", "closed", "unresolved"}
                or type(marker.get("launcher_claimed")) is not bool
                or not isinstance(marker.get("started_at_wall"), str)
                or len(marker["started_at_wall"]) > 64
                or datetime.fromisoformat(marker["started_at_wall"]).utcoffset() is None
                or (marker.get("previous_session") is not None and not isinstance(marker.get("previous_session"), dict))):
            return None
        boundary = marker["process_boundary"]
        if (boundary != {"kind": "direct"}
                and (not isinstance(boundary, dict)
                     or set(boundary) != {"kind", "container_id"}
                     or boundary.get("kind") != "docker"
                     or (boundary.get("container_id") is not None
                         and re.fullmatch(r"[0-9a-f]{64}", str(boundary["container_id"])) is None))):
            return None
        return marker
    except (OSError, ValueError):
        return None


def _seal_archive(folder: Path) -> None:
    """Remove write access without opening/checkpointing any archived database."""
    if any(mount == folder or mount.is_relative_to(folder) for mount in _mounted_children(folder.parent)):
        raise ValueError("mounted child in previous session; preserved")
    device = folder.stat(follow_symlinks=False).st_dev
    count = 0
    for _, directories, files, descriptor in os.fwalk(folder, topdown=False, follow_symlinks=False):
        count += 1
        if count > _MAX_SCAN_ENTRIES:
            raise ValueError("previous-session inventory is incomplete; preserved")
        for name in files:
            row = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            count += 1
            if count > _MAX_SCAN_ENTRIES or row.st_dev != device:
                raise ValueError("previous-session inventory is incomplete; preserved")
            if stat.S_ISREG(row.st_mode):
                if row.st_nlink != 1:
                    raise ValueError("multiply linked previous-session file; preserved")
                held = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=descriptor)
                try:
                    os.fchmod(held, stat.S_IMODE(row.st_mode) & ~0o222)
                finally:
                    os.close(held)
        os.fchmod(descriptor, stat.S_IMODE(os.fstat(descriptor).st_mode) & ~0o222)
        os.fsync(descriptor)


def _readonly_database(path: Path) -> sqlite3.Connection:
    """Never pretend immutable main contains an uncheckpointed WAL."""
    _safe_path(path, path.parent)
    row = path.stat(follow_symlinks=False)
    if not stat.S_ISREG(row.st_mode):
        raise ValueError("journal is not a regular file")
    for suffix in ("-wal", "-journal"):
        companion = path.with_name(path.name + suffix)
        if companion.is_symlink() or (companion.exists() and companion.stat().st_size):
            raise ValueError("uncheckpointed journal state is unknown")
    return sqlite3.connect(path.resolve().as_uri() + "?mode=ro&immutable=1", uri=True)


# The metadata table holding each writer's `records_lost` mark: records of
# that start were not written. Files of older versions have no such column.
_RECORDS_LOST_TABLES = {
    "program-cycle.sqlite3": "program_cycle_metadata",
    "manager/manager-bootstrap-journal.sqlite3": "bootstrap_journal_metadata",
    "manager/manager-apply-journal.sqlite3": "apply_journal_metadata",
    "manager/active-bundle-publication.sqlite3": "publication_store_metadata",
    "logical-scene.sqlite3": "logical_scene_store_metadata",
    "execution-state/operation-journal.sqlite3": "journal_metadata",
}
_INCOMPLETE_REASON = " (some records of that start could not be written)"


def _effect_observations(folder: Path) -> tuple[str, list[str], list[str], list[str]]:
    """Bounded scalar diagnostics only; never decode opaque owner formats.

    Unknown formats
    and missing/native-WAL evidence remain unknown, independently of shutdown.
    The schema version is the journal's own metadata (user_version, or the
    Program metadata row); it is read, not pinned. A predicate that does not
    fit the journal's layout fails and leaves that family unknown. A family
    whose writer marked a file `records_lost` is incomplete: unknown, and its
    unfinished items are not reported as unresolved.
    """
    unknown, unresolved, incomplete = [], [], []
    sources = [
        ("program_cycle", "program-cycle.sqlite3", None, [
            "SELECT 1 FROM program_cycle WHERE state != 'idle_after_reset' OR program_run_id IS NOT NULL OR claim_json IS NOT NULL OR acquisition_pending_json IS NOT NULL LIMIT 1",
            "SELECT 1 FROM program_cycle_inflight_commands LIMIT 1",
        ], []),
        # Journal schema 22: no disposition, reconciliation or Stop
        # surveillance columns. An unfinished Stop is an accepted Stop event
        # with no completion row whose ring record was not yet overwritten
        # (more than request_capacity newer events finish it for good). A held
        # claim is a cell_claim_acquired row with no cell_claim_released row of
        # the same claim_generation, in either order.
        ("kernel_operation", "execution-state/operation-journal.sqlite3", 0x4D46454A, [
            "SELECT 1 FROM operation_snapshots WHERE state NOT IN ('completed','stopped','failed','interrupted','intervention_required','cancelled_pre_dispatch') LIMIT 1",
            "SELECT 1 FROM journal_records a WHERE a.record_kind='cell_claim_acquired' AND NOT EXISTS (SELECT 1 FROM journal_records r WHERE r.record_kind='cell_claim_released' AND json_extract(r.payload_json,'$.claim_generation')=json_extract(a.payload_json,'$.claim_generation')) LIMIT 1",
            "SELECT 1 FROM journal_records a WHERE a.record_kind='stop_event_accepted' AND NOT EXISTS (SELECT 1 FROM journal_records c WHERE c.record_kind='stop_event_completed' AND c.stop_event_id=a.stop_event_id) AND a.stop_event_id > (SELECT MAX(stop_event_id) FROM journal_records) - (SELECT request_capacity FROM journal_metadata) LIMIT 1",
        ], []),
        ("manager", "manager/manager-bootstrap-journal.sqlite3", 1296450122, [
            "SELECT 1 FROM manager_bootstrap_intents i LEFT JOIN manager_bootstrap_accepted_targets t USING(bootstrap_id,bootstrap_generation) WHERE t.bootstrap_id IS NULL LIMIT 1",
        ], ["manager_bootstrap_intents"]),
        ("manager", "manager/manager-apply-journal.sqlite3", 1296450128, [
            "SELECT 1 FROM manager_apply_intents i LEFT JOIN manager_apply_terminal_outcomes t USING(apply_id,apply_generation) WHERE t.apply_id IS NULL LIMIT 1",
            "SELECT 1 FROM manager_apply_terminal_outcomes WHERE outcome_state='unknown_effect' OR effect_disposition='unknown' LIMIT 1",
        ], ["manager_apply_intents"]),
        ("manager", "manager/active-bundle-publication.sqlite3", 1296449872, [], ["active_bundle_publication_receipts"]),
        ("scene", "logical-scene.sqlite3", 1296454467, [
            "SELECT 1 FROM logical_scene_transaction_intents i LEFT JOIN logical_scene_request_outcomes t USING(transaction_sequence) WHERE t.transaction_sequence IS NULL LIMIT 1",
            "SELECT 1 FROM logical_scene_request_outcomes WHERE projection_status='failed_projection_uncertain' LIMIT 1",
            "SELECT 1 FROM logical_scene_maintenance_initialization_intents i LEFT JOIN logical_scene_maintenance_initialization_results r USING(initialization_sequence) WHERE r.initialization_sequence IS NULL LIMIT 1",
            "SELECT 1 FROM logical_scene_maintenance_initialization_results r LEFT JOIN logical_scene_maintenance_initialization_completions c USING(initialization_sequence) WHERE r.effects_unknown=1 OR r.state='interrupted' OR (r.state='initialized' AND c.initialization_sequence IS NULL) LIMIT 1",
            "SELECT 1 FROM logical_scene_initial_baseline_requests i LEFT JOIN logical_scene_initial_baseline_results r USING(initialization_sequence) WHERE r.initialization_sequence IS NULL OR r.state='interrupted' LIMIT 1",
        ], ["canonical_empty_scene_acceptance", "logical_scene_transaction_intents", "logical_scene_maintenance_initialization_intents", "logical_scene_initial_baseline_requests"]),
    ]
    for family, relative, application, queries, opaque in sources:
        path = folder / relative
        try:
            _safe_path(path, folder)
            with closing(_readonly_database(path)) as database:
                if database.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
                    raise ValueError("journal integrity unknown")
                if database.execute("PRAGMA foreign_key_check").fetchone() is not None:
                    raise ValueError("journal references unknown")
                if application is not None and (database.execute("PRAGMA application_id").fetchone() != (application,) or database.execute("PRAGMA user_version").fetchone()[0] < 1):
                    raise ValueError("unformatted journal schema")
                if family == "program_cycle":
                    if database.execute("SELECT 1 FROM program_cycle_metadata WHERE singleton=1 AND schema_version >= 1").fetchone() is None:
                        raise ValueError("unformatted Program journal schema")
                    if database.execute("SELECT singleton FROM program_cycle").fetchall() != [(1,)]:
                        raise ValueError("Program owner row absent or invalid")
                    # A retained reconciliation bit on an idle cycle with no
                    # run/effect is unknown evidence, not a positive
                    # unfinished operation.
                    if database.execute("SELECT 1 FROM program_cycle WHERE state='idle_after_reset' AND program_run_id IS NULL AND claim_json IS NULL AND acquisition_pending_json IS NULL AND reconciliation_required != 0 AND NOT EXISTS(SELECT 1 FROM program_cycle_inflight_commands) LIMIT 1").fetchone():
                        unknown.append(family)
                for query in queries:
                    if database.execute(query).fetchone() is not None:
                        unresolved.append(family)
                if any(database.execute("SELECT 1 FROM " + table + " LIMIT 1").fetchone() is not None for table in opaque):
                    unknown.append(family)
                if family == "kernel_operation":
                    # The journal names its start once (schema 22); a journal
                    # no start opened is unknown evidence.
                    starts = database.execute("SELECT application_start_id FROM journal_metadata").fetchall()
                    if len(starts) != 1 or not isinstance(starts[0][0], str) or re.fullmatch(r"[0-9a-f]{16}", starts[0][0]) is None or starts[0][0] == "0" * 16:
                        unknown.append(family)
                table = _RECORDS_LOST_TABLES.get(relative)
                if (table is not None
                        and database.execute("SELECT 1 FROM pragma_table_info(?) WHERE name='records_lost'", (table,)).fetchone() is not None
                        and database.execute("SELECT 1 FROM " + table + " WHERE records_lost != 0 LIMIT 1").fetchone() is not None):
                    incomplete.append(family)
        except (OSError, ValueError, TypeError, sqlite3.Error):
            unknown.append(family)
    incomplete = sorted(set(incomplete))
    unknown = sorted(set(unknown) | set(incomplete))
    unresolved = sorted(set(unresolved) - set(incomplete))
    return ("unresolved" if unresolved else "unknown" if unknown else "closed", unknown, unresolved, incomplete)


def classify_effects(folder: Path) -> str:
    """No unresolved journal work is distinct from physical rest or admission."""
    return _effect_observations(folder)[0]


def _unfinished_operations(root: Path, folder: Path) -> tuple[list[int], int, str] | None:
    """The previous run's unfinished operations: first ids, count and start (BS 4.4).

    Read from a copy of the run's Kernel journal and its WAL, opened normally
    so SQLite replays the WAL; the copy is deleted after the read. The start is
    the journal's own (``journal_metadata.application_start_id``); History
    files the report under it. This is a record for the operator, not
    recovery: nothing is adopted. None when the journal is absent or cannot be
    read, or names no start.
    """
    journal = folder / "execution-state/operation-journal.sqlite3"
    try:
        _safe_path(journal, folder)
        if not stat.S_ISREG(journal.stat(follow_symlinks=False).st_mode):
            return None
    except (OSError, ValueError):
        return None
    unfinished = f"state NOT IN ({','.join('?' * len(_TERMINAL_OPERATION_STATES))})"
    # The scratch copy lives beside the reports, never at the root, so a copy
    # left by a crash is not an unknown state-root entry.
    with tempfile.TemporaryDirectory(prefix=".journal-copy.", dir=_directory(root, "session-reports")) as scratch:
        copy = Path(scratch) / journal.name
        try:
            shutil.copyfile(journal, copy, follow_symlinks=False)
            wal = journal.with_name(journal.name + "-wal")
            if wal.exists() and not wal.is_symlink():
                shutil.copyfile(wal, copy.with_name(copy.name + "-wal"), follow_symlinks=False)
            with closing(sqlite3.connect(copy)) as database:
                rows = database.execute(
                    f"SELECT operation_id FROM operation_snapshots WHERE {unfinished} "
                    "ORDER BY operation_id LIMIT ?",
                    (*_TERMINAL_OPERATION_STATES, MAXIMUM_REPORTED_OPERATIONS)).fetchall()
                count = database.execute(
                    f"SELECT count(*) FROM operation_snapshots WHERE {unfinished}",
                    _TERMINAL_OPERATION_STATES).fetchone()[0]
                starts = database.execute("SELECT application_start_id FROM journal_metadata").fetchall()
        except (OSError, sqlite3.Error):
            return None
    if (len(starts) != 1 or not isinstance(starts[0][0], str)
            or re.fullmatch(r"[0-9a-f]{16}", starts[0][0]) is None or starts[0][0] == "0" * 16):
        return None
    return [int(row[0]) for row in rows], int(count), starts[0][0]


def _report(root: Path, identity: str, marker: dict | None) -> dict:
    shutdown = "clean" if marker and marker.get("disposition") == "stopped" and marker.get("shutdown") == "clean" else "unknown"
    source = _safe_path(root / "previous-runs" / identity, root)
    if not source.exists():
        source = root / "current"
    effects, unknown, unresolved, incomplete = _effect_observations(source) if marker is not None else ("unknown", ["program_cycle", "scene", "manager", "kernel_operation"], [], [])
    operations = _unfinished_operations(root, source) if marker is not None else None
    message = (f"Previous ManyForge session {identity}: effects are {effects}"
               + ("; shutdown was unclean or could not be verified." if shutdown != "clean" else "; owned processes stopped normally.")
               + " The new cell is empty; no previous state was adopted or replayed."
               + (" Unresolved journal families: " + ", ".join(unresolved) + "." if unresolved else "")
               + (" Unknown journal families: " + ", ".join(family + (_INCOMPLETE_REASON if family in incomplete else "") for family in unknown) + "." if unknown else "")
               + ((f" {operations[1]} operations have no final record and are marked host_replaced; the records of that"
                   " start are incomplete, so some of them may have finished."
                   if "kernel_operation" in incomplete
                   else f" {operations[1]} unfinished operations are marked host_replaced.")
                  if operations and operations[1] else "")
               + " Previous journal events may be absent from History.")
    legacy_message = message + " All previous journals are kept for investigation."
    value = {
        "schema_version": REPORT_SCHEMA,
        "session_id": identity,
        "archive_relative_path": "previous-runs/" + identity,
        "shutdown": shutdown,
        "effects": effects,
        "history_coverage": "not_imported",
        "journal_families": ["program_cycle", "scene", "manager", "kernel_operation"],
        "message": (message + " Up to five previous runs (2 GiB in total) are normally kept; "
                    "copy any journals you want to retain."),
        # Unknown (null) when the run's Kernel journal could not be read.
        "unfinished_operations": None if operations is None else [
            {"operation_id": operation_id, "disposition": "host_replaced"}
            for operation_id in operations[0]],
        "unfinished_operation_count": None if operations is None else operations[1],
        # The start this report describes (null with the operations).
        "application_start_id": None if operations is None else operations[2],
    }
    reports = _directory(root, "session-reports")
    completed = reports / (identity + ".json")
    pending = _directory(reports, "pending") / (identity + ".json")
    existing = completed if completed.exists() or completed.is_symlink() else pending
    if existing.exists() or existing.is_symlink():
        recorded = _read(existing)
        # An interrupted selection can contain the exact old report text.
        # Keep that report byte-for-byte so the durable selection still matches.
        legacy = dict(value, message=legacy_message)
        if recorded != value and recorded != legacy:
            raise ValueError("recorded previous-session report conflicts with its archive; preserved")
        value = recorded
    else:
        _write(pending, value)
    return value


def _check_root_entries(root: Path, stable: frozenset[str]) -> None:
    """Refuse a root holding anything besides its own kept and session entries."""
    unknown = set(os.listdir(root)) - stable - SESSION_ENTRIES
    # Only our exact atomic-write leftovers are recognized.
    unknown -= _ROOT_TEMPORARIES
    if unknown:
        raise ValueError("unknown state-root entries preserved; refusing session switch: " + ", ".join(sorted(unknown)[:20]))


def _finish_selection(root: Path, plan: dict) -> dict:
    marker = plan.get("marker")
    archive_id = plan.get("archive_id")
    if (plan.get("schema_version") != SELECTION_SCHEMA
            or not isinstance(marker, dict)
            or not _ID.fullmatch(str(marker.get("session_id", "")))
            or (archive_id is not None and not _ID.fullmatch(str(archive_id)))):
        raise ValueError("partial session selection is unknown; preserved")
    current = root / "current"
    if archive_id is not None:
        previous = _directory(root, "previous-runs")
        archived = _safe_path(previous / archive_id, root)
        if not archived.exists():
            if not current.is_dir() or (_marker(current) or {}).get("session_id") == marker["session_id"]:
                raise ValueError("partial session selection lost its source; preserved")
            if any(mount == current or mount.is_relative_to(current) for mount in _mounted_children(root)):
                raise ValueError("mounted child in current session; preserved")
            os.rename(_safe_path(current, root), archived)
            _sync(previous)
            _sync(root)
        elif current.exists() and (_marker(current) or {}).get("session_id") != marker["session_id"]:
            # A newly created but not yet marked current is safe to finish only
            # when it has no owner data. Its exact intended identity is durable.
            if set(os.listdir(current)) - {".session.json.next"}:
                raise ValueError("conflicting partial session selection; preserved")
        report = _report(root, archive_id, _marker(archived))
        if marker.get("previous_session") != report:
            raise ValueError("partial selection report identity changed; preserved")
        _seal_archive(archived)
    if not current.exists():
        _directory(root, "current")
    else:
        _safe_path(current, root)
    existing = _marker(current)
    if existing is not None:
        if existing != marker:
            raise ValueError("partial current marker conflicts with selection; preserved")
    else:
        if set(os.listdir(current)) - {".session.json.next"}:
            raise ValueError("unmarked current contains unknown data; preserved")
        _write(current / "session.json", marker)
    _sync(root)
    (root / ".session-selection.json").unlink()
    _sync(root)
    return marker


def prepare_session(root: Path, session_id: str, *, lock_fd: int,
                    stable_entries: frozenset[str],
                    process_boundary: dict | None = None) -> dict:
    """Select a fresh, empty current before any owner starts.

    ``stable_entries`` names the root's own kept entries besides the session
    entries; any other entry is preserved and refused.
    """
    if not _ID.fullmatch(session_id):
        raise ValueError("invalid session identity")
    root = _safe_path(root)
    _check_root_identity(root, lock_fd)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    selected_archive = _safe_path(root / "previous-runs" / session_id, root)
    if selected_archive.exists() or selected_archive.is_symlink():
        raise ValueError("session identity already names a previous session; preserved")
    _directory(_directory(root, "session-reports"), "pending")
    selection = root / ".session-selection.json"
    if selection.exists():
        _finish_selection(root, _read(selection))
    _check_root_entries(root, stable_entries)
    report = None
    archive_id = None
    if (root / "current").exists() or (root / "current").is_symlink():
        current = _safe_path(root / "current", root)
        marker = _marker(current)
        if marker and marker["session_id"] == session_id:
            raise ValueError("session identity has already been selected; preserved")
        archive_id = marker["session_id"] if marker else uuid.uuid4().hex
        previous = _directory(root, "previous-runs")
        if (previous / archive_id).exists() or (previous / archive_id).is_symlink():
            raise ValueError("previous session archive already exists; preserved")
        report = _report(root, archive_id, marker)
    marker = {"schema_version": MARKER_SCHEMA, "session_id": session_id,
              "started_at_wall": datetime.now(timezone.utc).isoformat(),
              "disposition": "started", "shutdown": "unknown", "effects": "unknown",
              "launcher_claimed": False, "previous_session": report,
              "process_boundary": process_boundary or {"kind": "direct"}}
    plan = {"schema_version": SELECTION_SCHEMA,
            "archive_id": archive_id, "marker": marker}
    _write(selection, plan)
    return _finish_selection(root, plan)



def claim_selected(root: Path, marker: dict) -> dict:
    """Record that the launcher that selected ``marker`` owns its session."""
    claimed = dict(marker, launcher_claimed=True)
    _write(root / "current/session.json", claimed)
    return claimed


def record_stopped(root: Path, lock_fd: int, marker: dict, *, verified: bool) -> None:
    """Close the session marker after every application process exited."""
    _check_root_identity(root, lock_fd)
    stopped = dict(marker, disposition="stopped", shutdown="clean" if verified else "unknown",
                   effects=classify_effects(root / "current"))
    _write(root / "current/session.json", stopped)


# Retention of previous runs (contract 1.14; BS 4.3, 4.8; Q6).
MAXIMUM_PREVIOUS_RUNS = 5
MAXIMUM_ARCHIVE_BYTES = 2 * 1024 ** 3
_MAX_DELETIONS = 8
_HISTORY_SUFFIXES = ("", "-wal", "-shm", "-journal", ".headless-owner.lock")
_HISTORY_VERSION = re.compile(r"history\.v([1-9][0-9]{0,8})\.sqlite3\Z")


def _open_relative_directory(root_fd: int, relative: Path) -> int:
    """Anchor every parent component to the held owner root, without symlinks."""
    current = os.dup(root_fd)
    try:
        for part in relative.parts:
            if part in ("..", "/"):
                raise ValueError("retained unit is outside the held state root")
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=current)
            os.close(current)
            current = child
        return current
    except BaseException:
        os.close(current)
        raise


def _allocated_bytes(root: Path, device: int, mounts: set[Path], seen: set[tuple[int, int]], root_fd: int) -> tuple[int, bool]:
    if any(mount == root or mount.is_relative_to(root) for mount in mounts):
        raise ValueError(f"mounted child in {root}; accounting incomplete")
    size, entries = 0, 0
    for _, directories, files, fd in os.fwalk(".", topdown=True, follow_symlinks=False, dir_fd=root_fd):
        for name in (".", *directories, *files):
            entries += 1
            if entries > _MAX_SCAN_ENTRIES:
                raise ValueError("retained-log accounting exceeds bounded scan")
            row = os.stat(name, dir_fd=fd, follow_symlinks=False)
            if row.st_dev != device:
                raise ValueError(f"cross-filesystem child in {root}; accounting incomplete")
            if stat.S_ISLNK(row.st_mode) and name in directories:
                directories.remove(name)
            identity = (row.st_dev, row.st_ino)
            if identity not in seen:
                seen.add(identity)
                size += row.st_blocks * 512
    return size, True



def _children(path: Path, warnings: list[str], label: str) -> list[Path]:
    children = []
    with os.scandir(path) as entries:
        for entry in entries:
            if len(children) == 1000:
                warnings.append(f"{label}: inventory incomplete (1001+ entries; 1000 observed, unseen 1+ protected; measured usage is a lower bound)")
                break
            children.append(path / entry.name)
    return children


def _safe_inventory_path(path: Path, root: Path, owner: Path, warnings: list[str]) -> bool:
    try:
        _safe_path(path, root)
        return True
    except ValueError:
        warnings.append(f"{owner / path.relative_to(root)}: symlink below the state root; protected, accounting incomplete (lower bound)")
        return False



def _session_metadata(path: Path) -> dict | None:
    """Read a session marker without following a symlink or an unbounded file."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as source:
            if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                return None
            data = source.read(64 * 1024 + 1)
        if len(data) > 64 * 1024:
            return None
        value = json.loads(data)
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def _session_order(folder: Path, row: os.stat_result) -> tuple[int, str]:
    marker = _session_metadata(folder / "session.json")
    if marker and marker.get("session_id") == folder.name:
        try:
            started = datetime.fromisoformat(marker["started_at_wall"])
            if started.utcoffset() is not None:
                return int(started.timestamp() * 1_000_000_000), folder.name
        except (KeyError, TypeError, ValueError, OverflowError):
            pass
    return row.st_mtime_ns, folder.name


def _remove_old_run(root: Path, root_fd: int, folder: Path,
                    before: os.stat_result) -> None:
    parent_fd = _open_relative_directory(root_fd, folder.relative_to(root).parent)
    try:
        if (os.fstat(parent_fd).st_dev != os.fstat(root_fd).st_dev or
                folder.parent in _mounted_children(root)):
            raise ValueError("mounted previous-runs during removal")
        row = os.stat(folder.name, dir_fd=parent_fd, follow_symlinks=False)
        if (row.st_dev, row.st_ino) != (before.st_dev, before.st_ino):
            raise ValueError("previous session changed before removal")
        if not shutil.rmtree.avoids_symlink_attacks:
            raise ValueError("platform cannot delete without following directory symlinks")
        unit_fd = os.open(folder.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                          dir_fd=parent_fd)
        try:
            held = os.fstat(unit_fd)
            if (held.st_dev, held.st_ino) != (before.st_dev, before.st_ino):
                raise ValueError("previous session changed during open")
            _allocated_bytes(folder, os.fstat(root_fd).st_dev,
                             _mounted_children(root), set(), unit_fd)
            # Archives are sealed read-only. Removing children requires write
            # permission on directories, not on the archived files.
            try:
                for _, _, _, fd in os.fwalk(".", topdown=False, follow_symlinks=False,
                                            dir_fd=unit_fd):
                    os.fchmod(fd, stat.S_IMODE(os.fstat(fd).st_mode) | stat.S_IWUSR | stat.S_IXUSR)
                shutil.rmtree(folder.name, dir_fd=parent_fd)
            except OSError:
                # If removal failed, leave any surviving archive sealed.
                for _, _, _, fd in os.fwalk(".", topdown=False, follow_symlinks=False,
                                            dir_fd=unit_fd):
                    os.fchmod(fd, stat.S_IMODE(os.fstat(fd).st_mode) & ~0o222)
                raise
        finally:
            os.close(unit_fd)
    finally:
        os.close(parent_fd)


def prune_previous_runs(root: Path, root_fd: int, *,
                        pressure: Callable[[int], bool] | None = None) -> tuple[tuple[str, ...], str | None]:
    """One bounded pass over previous runs, their reports and older History files.

    Keeps at most ``MAXIMUM_PREVIOUS_RUNS`` runs (one slot reserved for
    ``current/``) and at most ``MAXIMUM_ARCHIVE_BYTES`` of archived runs and
    older History files together, deleting oldest first; the newest run, the
    run the current session reports and the current History file are never
    deleted (contract 1.14; BS 4.3, 4.8; Q6). While ``pressure(freed_bytes)``
    is true (ManyForge's files above their share of the disk, or free space
    below the part that always stays free), the same units are deleted, oldest
    first, below those limits too. Returns (deleted, warning).
    """
    if (root / ".session-selection.json").exists() or (root / ".session-selection.json").is_symlink():
        return ((), "previous-run retention deferred during session selection")

    warnings: list[str] = []
    previous = _safe_path(root / "previous-runs", root)
    archives: list[tuple[Path, os.stat_result]] = []
    if previous.exists():
        if previous in _mounted_children(root):
            return ((), "previous-run retention deferred: mounted previous-runs")
        for folder in _children(previous, warnings, str(previous)):
            if _ID.fullmatch(folder.name) and _safe_inventory_path(folder, root, root, warnings):
                row = folder.stat(follow_symlinks=False)
                if stat.S_ISDIR(row.st_mode):
                    archives.append((folder, row))
    reports = _safe_path(root / "session-reports", root)
    finalized: list[tuple[Path, os.stat_result]] = []
    if reports.exists():
        if any(mount == reports or mount.is_relative_to(reports) for mount in _mounted_children(root)):
            return ((), "previous-run retention deferred: mounted session-reports")
        for report in _children(reports, warnings, str(reports)):
            if report.name.endswith(".json") and _ID.fullmatch(report.stem):
                row = report.stat(follow_symlinks=False)
                if stat.S_ISREG(row.st_mode):
                    finalized.append((report, row))
    if warnings:
        return ((), "; ".join(warnings))

    reserve = "current" in os.listdir(root)
    target = MAXIMUM_PREVIOUS_RUNS - int(reserve)
    excess_archives = max(0, len(archives) - target)
    excess_reports = max(0, len(finalized) - target)

    protected: set[str] = set()
    if archives:
        protected.add(max(archives, key=lambda item: _session_order(*item))[0].name)
    marker = _session_metadata(_safe_path(root / "current" / "session.json", root))
    if marker and isinstance(marker.get("previous_session"), dict):
        identity = marker["previous_session"].get("session_id")
        if isinstance(identity, str):
            protected.add(identity)
    pending = _safe_path(reports / "pending", root)
    if pending.exists():
        for entry in _children(pending, warnings, str(pending)):
            if entry.name.endswith(".json") and _ID.fullmatch(entry.stem):
                protected.add(entry.stem)
    if warnings:
        return ((), "; ".join(warnings))

    deleted: list[str] = []
    deleted_units = 0
    retained_archives = {folder.name for folder, _ in archives}

    def remove_report(report: Path, before: os.stat_result) -> None:
        parent_fd = _open_relative_directory(root_fd, report.relative_to(root).parent)
        try:
            if (os.fstat(parent_fd).st_dev != os.fstat(root_fd).st_dev or
                    any(mount == reports or mount.is_relative_to(reports)
                        for mount in _mounted_children(root))):
                raise ValueError("mounted session-reports during removal")
            row = os.stat(report.name, dir_fd=parent_fd, follow_symlinks=False)
            if not stat.S_ISREG(row.st_mode) or (row.st_dev, row.st_ino) != (before.st_dev, before.st_ino):
                raise ValueError("session report changed before removal")
            os.unlink(report.name, dir_fd=parent_fd)
        finally:
            os.close(parent_fd)
        deleted.append(str(report.relative_to(root)))

    try:
        for folder, row in sorted(archives, key=lambda item: _session_order(*item)):
            if not excess_archives or deleted_units == _MAX_DELETIONS:
                break
            if folder.name in protected:
                continue
            _check_root_identity(root, root_fd)
            _remove_old_run(root, root_fd, folder, row)
            retained_archives.remove(folder.name)
            deleted.append(str(folder.relative_to(root)))
            deleted_units += 1
            excess_archives -= 1
            for report, report_row in finalized[:]:
                if report.stem == folder.name:
                    remove_report(report, report_row)
                    finalized.remove((report, report_row))
                    excess_reports = max(0, excess_reports - 1)
                    break
        for report, row in sorted(finalized, key=lambda item: (item[1].st_mtime_ns, item[0].name)):
            if not excess_reports or deleted_units == _MAX_DELETIONS:
                break
            if report.stem in protected or report.stem in retained_archives:
                continue
            _check_root_identity(root, root_fd)
            remove_report(report, row)
            deleted_units += 1
            excess_reports -= 1
    except (OSError, ValueError) as error:
        return tuple(deleted), f"previous-run retention failed: {error}"
    if excess_archives or excess_reports:
        return tuple(deleted), "previous-run retention remains above its count limit"
    return _prune_previous_bytes(root, root_fd, archives, retained_archives, finalized,
                                 protected, deleted, deleted_units, remove_report, pressure)




def _older_history_files(root: Path, warnings: list[str]) -> list[tuple[tuple[int, str], list[Path], int]]:
    """Older History files beside the current one: (order, paths, bytes).

    The current file is the highest ``history/history.v<N>.sqlite3``; it is
    never listed. ``history.sqlite3`` (no version) is always older (BS 4.8).
    """
    history = root / "history"
    versions: dict[int, Path] = {}
    if _safe_inventory_path(history, root, root, warnings) and history.is_dir():
        for entry in _children(history, warnings, str(history)):
            match = _HISTORY_VERSION.fullmatch(entry.name)
            if match is not None:
                versions[int(match[1])] = entry
    current = max(versions, default=None)
    mains = [root / "history.sqlite3", history / "history.sqlite3",
             *(path for version, path in sorted(versions.items()) if version != current)]
    groups: list[tuple[tuple[int, str], list[Path], int]] = []
    for main in mains:
        if not _safe_inventory_path(main, root, root, warnings):
            continue
        try:
            main_row = main.stat(follow_symlinks=False)
        except FileNotFoundError:
            continue
        if not stat.S_ISREG(main_row.st_mode):
            continue
        paths, size = [], 0
        for suffix in _HISTORY_SUFFIXES:
            path = main.with_name(main.name + suffix)
            try:
                row = path.stat(follow_symlinks=False)
            except FileNotFoundError:
                continue
            if stat.S_ISREG(row.st_mode):
                paths.append(path)
                size += row.st_blocks * 512
        groups.append(((main_row.st_mtime_ns, str(main.relative_to(root))), paths, size))
    return groups


def _prune_previous_bytes(root, root_fd, archives, retained_archives, finalized, protected,
                          deleted, deleted_units, remove_report,
                          pressure=None) -> tuple[tuple[str, ...], str | None]:
    """The byte cap and the disk limits of the same pass: oldest first, across runs and History."""
    warnings: list[str] = []
    device = os.fstat(root_fd).st_dev
    mounts = _mounted_children(root)
    candidates: list[tuple[tuple[int, str], str, object, int]] = []
    total = 0
    try:
        for folder, row in archives:
            if folder.name not in retained_archives:
                continue
            unit_fd = _open_relative_directory(root_fd, folder.relative_to(root))
            try:
                size, _complete = _allocated_bytes(folder, device, mounts, set(), unit_fd)
            finally:
                os.close(unit_fd)
            total += size
            candidates.append((_session_order(folder, row), "run", (folder, row), size))
        for order, paths, size in _older_history_files(root, warnings):
            total += size
            candidates.append((order, "history", paths, size))
    except (OSError, ValueError) as error:
        return tuple(deleted), f"previous-run byte accounting unavailable: {error}"
    if warnings:
        return tuple(deleted), "; ".join(warnings)
    freed = 0
    try:
        for _order, kind, unit, size in sorted(candidates, key=lambda item: item[0]):
            if ((total <= MAXIMUM_ARCHIVE_BYTES and (pressure is None or not pressure(freed)))
                    or deleted_units == _MAX_DELETIONS):
                break
            _check_root_identity(root, root_fd)
            if kind == "run":
                folder, row = unit
                if folder.name in protected:
                    continue
                _remove_old_run(root, root_fd, folder, row)
                deleted.append(str(folder.relative_to(root)))
                for report, report_row in finalized[:]:
                    if report.stem == folder.name:
                        remove_report(report, report_row)
                        finalized.remove((report, report_row))
                        break
            else:
                for path in unit:
                    path.unlink()
                    deleted.append(str(path.relative_to(root)))
            total -= size
            freed += size
            deleted_units += 1
    except (OSError, ValueError) as error:
        return tuple(deleted), f"previous-run retention failed: {error}"
    if total > MAXIMUM_ARCHIVE_BYTES:
        return tuple(deleted), "previous-run retention remains above its byte limit"
    return tuple(deleted), None


# Disk limits (OD-215). ManyForge's own files take at most a share of the
# disk, and a part of the disk always stays free. The floor below which
# ManyForge does not start (and the Headless API stops operation) is that
# part plus the machine's own minimum. Both launchers prune and check through
# the functions below.
DISK_SPACE_BELOW_FLOOR = "disk_space_below_floor"
# The Headless API's disk check leaves its stop at the floor plus this margin.
DISK_FLOOR_CLEAR_MARGIN_BYTES = 1_000_000_000
# Pruning while running: a sampling period of the launcher's wait loop, not a
# real-time bound.
DISK_PRUNE_PERIOD_SECONDS = 60.0


@dataclass(frozen=True)
class DiskLimits:
    """The disk settings, with their starting values."""

    # The largest share of the disk ManyForge's own files may take, in percent.
    max_disk_percent: int = 10
    # The space that always stays free, in bytes.
    min_free_bytes: int = 10_000_000_000
    # The space the machine itself needs besides ManyForge, in bytes; not
    # determined yet.
    machine_minimum_bytes: int = 0

    def __post_init__(self) -> None:
        if (not 1 <= self.max_disk_percent <= 100 or self.min_free_bytes < 0
                or self.machine_minimum_bytes < 0):
            raise ValueError("disk percent must be in [1,100] and free-space bytes nonnegative")

    @property
    def floor_bytes(self) -> int:
        return self.min_free_bytes + self.machine_minimum_bytes


# The site settings file (machine facts). The launchers read only its disk
# members; the Manager and the execution host read the file themselves.
SITE_SETTINGS_SCHEMA = "manyforge.site.v2"
SITE_SETTINGS_MAX_BYTES = 1024 * 1024
_SITE_MEMBERS = frozenset({"schema", "device_addresses", "project_folders",
                           "disk_max_percent", "disk_always_free_bytes", "disk_machine_minimum_bytes"})
# The largest integer a JSON number carries exactly.
_SITE_MAX_BYTES_VALUE = 2 ** 53 - 1


_SITE_DISK_MEMBERS = ("disk_max_percent", "disk_always_free_bytes", "disk_machine_minimum_bytes")


def site_schema_has_disk_limits(schema: object) -> bool:
    """The one Python decision on which site settings schema lines are accepted.

    True when the schema carries the disk limits. Decided exception: version 1
    files carry no disk limits and use the defaults.
    """
    if schema == SITE_SETTINGS_SCHEMA:
        return True
    if schema == "manyforge.site.v1":
        return False
    raise ValueError("site settings schema must be manyforge.site.v2")


def _site_integer(site: Mapping[str, object], name: str, low: int, high: int, default: int) -> int:
    if name not in site:
        return default
    value = site[name]
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or (isinstance(value, float) and not value.is_integer()) or not low <= value <= high):
        raise ValueError(f"site settings {name} must be an integer from {low} to {high}")
    return int(value)


def site_disk_limits(site: object) -> DiskLimits:
    """The disk limits of a decoded site settings document; absent members take their default."""
    if not isinstance(site, dict):
        raise ValueError("site settings must be a JSON object")
    unknown = site.keys() - _SITE_MEMBERS
    if unknown:
        raise ValueError(f"site settings has unknown fields: {', '.join(sorted(unknown))}")
    if not site_schema_has_disk_limits(site.get("schema")):
        for name in _SITE_DISK_MEMBERS:
            if name in site:
                raise ValueError(f"site settings {name} needs schema manyforge.site.v2: change the first "
                                 "line's schema to manyforge.site.v2; no other change is needed")
    defaults = DiskLimits()
    return DiskLimits(
        _site_integer(site, "disk_max_percent", 1, 100, defaults.max_disk_percent),
        _site_integer(site, "disk_always_free_bytes", 0, _SITE_MAX_BYTES_VALUE, defaults.min_free_bytes),
        _site_integer(site, "disk_machine_minimum_bytes", 0, _SITE_MAX_BYTES_VALUE,
                      defaults.machine_minimum_bytes))


def read_site_disk_limits(path: Path | None) -> DiskLimits:
    """The disk limits from the site settings file; without a file, the defaults."""
    if path is None:
        return DiskLimits()
    if not Path(path).is_absolute():
        raise ValueError("site settings path must be absolute")
    with open(path, "rb") as source:
        data = source.read(SITE_SETTINGS_MAX_BYTES + 1)
    if len(data) > SITE_SETTINGS_MAX_BYTES:
        raise ValueError("site settings file exceeds 1 MiB")
    try:
        site = json.loads(data)
    except ValueError as error:
        raise ValueError(f"site settings file must be JSON: {error}") from error
    return site_disk_limits(site)


@dataclass(frozen=True)
class DiskReading:
    total_bytes: int
    # Space available to an ordinary user (the root reserve excluded).
    free_bytes: int
    # None on a file system that counts no inodes (f_files == 0, e.g. btrfs).
    free_inodes: int | None = None


def read_disk(fd: int) -> DiskReading:
    row = os.fstatvfs(fd)
    return DiskReading(row.f_blocks * row.f_frsize, row.f_bavail * row.f_frsize,
                       row.f_favail if row.f_files else None)


def free_for_floor(reading: DiskReading) -> int:
    """Free space as the floor rule counts it: no free inode counts as none.

    The one rule of the launchers' start check and the Headless API's disk
    check (OD-215): below the floor when this is less than the floor.
    """
    return 0 if reading.free_inodes == 0 else reading.free_bytes


def _size_text(value: int, *, up: bool = False) -> str:
    tenths = (math.ceil if up else math.floor)(value / 100_000_000) / 10
    return f"{tenths:g} GB"


class DiskBelowFloor(Exception):
    """Free space (or free inodes) below the floor; typed for the launchers."""

    code = DISK_SPACE_BELOW_FLOOR

    def __init__(self, path: Path, free_bytes: int, limits: DiskLimits) -> None:
        super().__init__(
            f"Not started: the disk that holds {path} has {_size_text(free_bytes)} free. "
            f"ManyForge needs at least {_size_text(limits.floor_bytes)} free "
            f"({_size_text(limits.min_free_bytes)} plus {_size_text(limits.machine_minimum_bytes)} "
            f"for the machine). Free {_size_text(limits.floor_bytes - free_bytes, up=True)} and start again.")
        self.path = path
        self.free_bytes = free_bytes
        self.floor_bytes = limits.floor_bytes


def _disk_pressure(root: Path, root_fd: int, limits: DiskLimits,
                   reader: Callable[[int], DiskReading],
                   warnings: list[str]) -> Callable[[int], bool]:
    """``over(freed_bytes)``: ManyForge's files above their share, or free below the minimum.

    ManyForge's files are everything under ``root``, measured once, at the
    first question (a pass with nothing deletable measures nothing).
    """
    measured: list[int] = []

    def over(freed: int) -> bool:
        reading = reader(root_fd)
        if not measured:
            try:
                owned, _ = _allocated_bytes(root, os.fstat(root_fd).st_dev, _mounted_children(root),
                                            set(), root_fd)
            except (OSError, ValueError) as error:
                # Unmeasured: only the free-space minimum applies in this pass.
                owned = 0
                warnings.append(f"ManyForge's files could not be measured: {str(error)[:200]}")
            measured.append(owned)
        return ((measured[0] - freed) * 100 > reading.total_bytes * limits.max_disk_percent
                or reading.free_bytes < limits.min_free_bytes)
    return over


def prune_own_files(root: Path, root_fd: int, limits: DiskLimits, *,
                    reader: Callable[[int], DiskReading] | None = None,
                    only_over_limit: bool = False) -> tuple[tuple[str, ...], str | None]:
    """One bounded pass of :func:`prune_previous_runs` under the disk limits.

    With ``only_over_limit`` (while running) nothing is deleted unless
    ManyForge's files are above their share or free space is below the
    minimum. Returns (deleted, warning).
    """
    warnings: list[str] = []
    over = _disk_pressure(root, root_fd, limits, reader or read_disk, warnings)
    if only_over_limit and not over(0):
        return (), None
    deleted, warning = prune_previous_runs(root, root_fd, pressure=over)
    return deleted, "; ".join([*warnings, *([warning] if warning else [])]) or None


def _log_pruning(deleted: tuple[str, ...], warning: str | None, limits: DiskLimits) -> None:
    reason = (f"oldest first; ManyForge keeps at most {MAXIMUM_PREVIOUS_RUNS} previous runs and "
              f"{MAXIMUM_ARCHIVE_BYTES // 1024 ** 3} GiB of them, its files take at most "
              f"{limits.max_disk_percent} % of the disk and {_size_text(limits.min_free_bytes)} stay free")
    for item in deleted:
        print(f"ManyForge removed {item}: {reason}", file=sys.stderr, flush=True)
    if warning:
        print(f"WARNING ManyForge disk pruning: {warning[:4096]}", file=sys.stderr, flush=True)


def check_disk_floor(root: Path, root_fd: int, limits: DiskLimits, *,
                     reader: Callable[[int], DiskReading] | None = None) -> None:
    """Raise DiskBelowFloor when a file system holding the records is below the floor.

    The records are under ``root`` (``current/``) and ``root/history``; each
    file system is read once. No free inode counts as no free space.
    """
    reader = reader or read_disk
    devices: set[int] = set()
    for path in (root, root / "history"):
        if path == root:
            fd = root_fd
        else:
            try:
                fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            except FileNotFoundError:
                continue
        try:
            device = os.fstat(fd).st_dev
            if device in devices:
                continue
            devices.add(device)
            reading = reader(fd)
        finally:
            if fd != root_fd:
                os.close(fd)
        free = free_for_floor(reading)
        if free < limits.floor_bytes:
            raise DiskBelowFloor(path, free, limits)


def prepare_disk(root: Path, root_fd: int, limits: DiskLimits, *,
                 reader: Callable[[int], DiskReading] | None = None) -> None:
    """At start, before the session selection writes anything: prune, then check the floor."""
    deleted, warning = prune_own_files(root, root_fd, limits, reader=reader)
    _log_pruning(deleted, warning, limits)
    check_disk_floor(root, root_fd, limits, reader=reader)


def prune_while_running(root: Path, root_fd: int, limits: DiskLimits, *,
                        reader: Callable[[int], DiskReading] | None = None) -> None:
    """The launcher's periodic pass; a failure is logged and never ends the run."""
    try:
        deleted, warning = prune_own_files(root, root_fd, limits, reader=reader, only_over_limit=True)
    except (OSError, ValueError) as error:
        deleted, warning = (), f"pass failed: {str(error)[:200]}"
    _log_pruning(deleted, warning, limits)
