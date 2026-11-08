"""Cold sessions for the development workbench; never starts old journal owners.

Selection is called only after compose proves the old container stopped and no
live container has an overlapping bind. The caller holds the installation root
flock across selection and docker run. The child takes that same lock for life.
"""
from __future__ import annotations

import argparse
from contextlib import closing
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.lib.export_retention import (
    _check_root_identity, _mounted_children, _safe_path, _MAX_SCAN_ENTRIES,
    _LEGACY_SESSION_ENTRIES,
)

MARKER_SCHEMA = "manyforge.workbench_session.v1"
REPORT_SCHEMA = "manyforge.workbench_previous_session.v1"
_ID = re.compile(r"[0-9a-f]{32}\Z")
_LEGACY = _LEGACY_SESSION_ENTRIES
_STABLE = frozenset({
    "current", "previous-sessions", "session-reports", "evidence",
    # The simulated PLC's own per-device settings, kept across sessions.
    "sim_plc",
    # Optional, bounded diagnostic snapshots survive cold Workbench sessions.
    "motion-traces",
    ".legacy-session-transition.json", ".session-selection.json",
    *("history.sqlite3" + suffix for suffix in ("", "-wal", "-shm", "-journal", ".headless-owner.lock")),
})


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
        raise ValueError("workbench session metadata exceeds 64 KiB")
    temporary = path.with_name("." + path.name + ".next")
    # A crash's temporary file contains no owner evidence and is safe to replace.
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())
    if path.is_symlink():
        raise ValueError("symlinked workbench session metadata")
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


def _effect_observations(folder: Path) -> tuple[str, list[str], list[str]]:
    """Bounded scalar diagnostics only; never decode opaque owner formats.

    Unknown formats
    and missing/native-WAL evidence remain unknown, independently of shutdown.
    The schema version is the journal's own metadata (user_version, or the
    Program metadata row); it is read, not pinned. A predicate that does not
    fit the journal's layout fails and leaves that family unknown.
    """
    unknown, unresolved = [], []
    sources = [
        ("program_cycle", "program-cycle.sqlite3", None, [
            "SELECT 1 FROM program_cycle WHERE state != 'idle_after_reset' OR program_run_id IS NOT NULL OR claim_json IS NOT NULL OR acquisition_pending_json IS NOT NULL LIMIT 1",
            "SELECT 1 FROM program_cycle_inflight_commands LIMIT 1",
            "SELECT 1 FROM program_persistent_value_requests WHERE outcome='storage_uncertain' LIMIT 1",
        ], []),
        # v16 and v17 journals share the stop_projections *_sequence columns
        # (v16 CHECKs pair each with its payload column).
        ("kernel_operation", "execution-state/operation-journal.sqlite3", 0x4D46454A, [
            "SELECT 1 FROM operation_snapshots WHERE disposition='orphaned_by_host_replacement' OR state NOT IN ('completed','stopped','failed','intervention_required','cancelled_pre_dispatch') LIMIT 1",
            "SELECT 1 FROM cell_admission_claims WHERE release_owner_role IS NULL AND reconciled_sequence IS NULL LIMIT 1",
            "SELECT 1 FROM stop_projections WHERE terminal_sequence IS NULL OR (surveillance_sequence IS NOT NULL AND clear_sequence IS NULL) LIMIT 1",
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
            "SELECT 1 FROM logical_scene_maintenance_migration_state WHERE legacy_native_effects_uncertain != 0 LIMIT 1",
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
                    # Ordinary Manager shutdown retains liveness reconciliation
                    # even on an idle cycle with no run/effect. That bit is
                    # unknown evidence, not a positive unfinished operation.
                    if database.execute("SELECT 1 FROM program_cycle WHERE state='idle_after_reset' AND program_run_id IS NULL AND claim_json IS NULL AND acquisition_pending_json IS NULL AND reconciliation_required != 0 AND NOT EXISTS(SELECT 1 FROM program_cycle_inflight_commands) LIMIT 1").fetchone():
                        unknown.append(family)
                for query in queries:
                    if database.execute(query).fetchone() is not None:
                        unresolved.append(family)
                if any(database.execute("SELECT 1 FROM " + table + " LIMIT 1").fetchone() is not None for table in opaque):
                    unknown.append(family)
                if family == "kernel_operation":
                    # Reuse the native close receipt: it applies the precise
                    # Stop decoder/fault-clear gate after all lanes drain.
                    owners = database.execute("SELECT owner_epoch,active,closed_at_ns,close_sequence FROM owner_epochs").fetchall()
                    for epoch, active, closed_at, sequence in owners:
                        if active != 0 or closed_at is None or sequence is None:
                            unknown.append(family)
                            continue
                        receipt = database.execute("SELECT owner_epoch,record_kind,payload_json,payload_digest,occurred_at_ns,effect_pending FROM journal_records WHERE sequence=?", (sequence,)).fetchone()
                        if receipt is None or receipt[:2] != (epoch, "owner_epoch_closed"):
                            unknown.append(family)
                            continue
                        payload = json.loads(receipt[2])
                        digest = hashlib.sha256(receipt[2].encode()).hexdigest()
                        expected_time = closed_at if closed_at <= 9_007_199_254_740_991 else str(closed_at)
                        expected_payload = {"closed_at_ns": expected_time, "owner_epoch": epoch}
                        if (payload != expected_payload
                                or receipt[2] != json.dumps(expected_payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
                                or receipt[3] not in {digest, "sha256:" + digest}
                                or receipt[4:] != (closed_at, 0)):
                            unknown.append(family)
        except (OSError, ValueError, TypeError, sqlite3.Error):
            unknown.append(family)
    unknown, unresolved = sorted(set(unknown)), sorted(set(unresolved))
    return ("unresolved" if unresolved else "unknown" if unknown else "closed", unknown, unresolved)


def classify_effects(folder: Path) -> str:
    """No unresolved journal work is distinct from physical rest or admission."""
    return _effect_observations(folder)[0]


def _report(root: Path, identity: str, marker: dict | None) -> dict:
    shutdown = "clean" if marker and marker.get("disposition") == "stopped" and marker.get("shutdown") == "clean" else "unknown"
    source = _safe_path(root / "previous-sessions" / identity, root)
    if not source.exists():
        source = root / "current"
    effects, unknown, unresolved = _effect_observations(source) if marker is not None else ("unknown", ["program_cycle", "scene", "manager", "kernel_operation"], [])
    message = (f"Previous workbench session {identity}: effects are {effects}"
               + ("; shutdown was unclean or could not be verified." if shutdown != "clean" else "; owned processes stopped normally.")
               + " The new cell is empty; no previous state was adopted or replayed."
               + (" Unresolved journal families: " + ", ".join(unresolved) + "." if unresolved else "")
               + (" Unknown journal families: " + ", ".join(unknown) + "." if unknown else "")
               + " Previous journal events may be absent from History.")
    legacy_message = message + " All previous journals are kept for investigation."
    value = {
        "schema_version": REPORT_SCHEMA,
        "session_id": identity,
        "archive_relative_path": "previous-sessions/" + identity,
        "shutdown": shutdown,
        "effects": effects,
        "history_coverage": "not_imported",
        "journal_families": ["program_cycle", "scene", "manager", "kernel_operation"],
        "message": (message + " Up to five previous sessions are normally kept; "
                    "copy any journals you want to retain."),
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


def _legacy(root: Path) -> dict | None:
    transition = root / ".legacy-session-transition.json"
    names = set(os.listdir(root))
    unknown = names - _LEGACY - _STABLE
    # Only our exact atomic-write leftovers are recognized.
    unknown -= {"..legacy-session-transition.json.next", "..session-selection.json.next"}
    if unknown:
        raise ValueError("unknown state-root entries preserved; refusing session switch: " + ", ".join(sorted(unknown)[:20]))
    if transition.exists():
        plan = _read(transition)
        if (plan.get("schema_version") != "manyforge.workbench_legacy_transition.v1"
                or not _ID.fullmatch(str(plan.get("session_id", "")))
                or not isinstance(plan.get("entries"), list)
                or not set(plan["entries"]) <= _LEGACY):
            raise ValueError("partial legacy session inventory is invalid; preserved")
    else:
        entries = sorted(names & _LEGACY)
        if not entries:
            return None
        if (root / "current").exists():
            raise ValueError("mixed legacy/current layout is unknown; preserved")
        plan = {"schema_version": "manyforge.workbench_legacy_transition.v1", "session_id": uuid.uuid4().hex, "entries": entries}
        _write(transition, plan)
    previous = _directory(root, "previous-sessions")
    sealed = _safe_path(previous / plan["session_id"], root)
    if sealed.exists():
        if any((root / name).exists() or (root / name).is_symlink() for name in plan["entries"]):
            raise ValueError("legacy archive conflicts with remaining source entries")
    else:
        partial = _directory(previous, "." + plan["session_id"] + ".partial")
        for name in plan["entries"]:
            source, destination = root / name, partial / name
            source_present = source.exists() or source.is_symlink()
            destination_present = destination.exists() or destination.is_symlink()
            if source_present == destination_present:
                raise ValueError("partial legacy inventory is missing or conflicting: " + name)
            if source_present:
                _safe_path(source, root)
                if source.stat(follow_symlinks=False).st_dev != root.stat().st_dev:
                    raise ValueError("cross-mount legacy source; preserved")
                if source.is_dir() and any(mount == source or mount.is_relative_to(source) for mount in _mounted_children(root)):
                    raise ValueError("mounted child in legacy session; preserved")
                os.rename(source, destination)
                _sync(partial)
                _sync(root)
        os.rename(partial, sealed)
        _sync(previous)
    _seal_archive(sealed)
    report = _report(root, plan["session_id"], None)
    # Keep the exact legacy handoff until a new-current marker containing this
    # report is durable. A crash before selection must not lose its notice.
    return report


def _finish_selection(root: Path, plan: dict) -> dict:
    marker = plan.get("marker")
    archive_id = plan.get("archive_id")
    if (plan.get("schema_version") != "manyforge.workbench_session_selection.v1"
            or not isinstance(marker, dict)
            or not _ID.fullmatch(str(marker.get("session_id", "")))
            or (archive_id is not None and not _ID.fullmatch(str(archive_id)))):
        raise ValueError("partial session selection is unknown; preserved")
    current = root / "current"
    if archive_id is not None:
        previous = _directory(root, "previous-sessions")
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
    legacy = root / ".legacy-session-transition.json"
    if legacy.exists():
        legacy_plan = _read(legacy)
        previous = marker.get("previous_session")
        if not isinstance(previous, dict) or previous.get("session_id") != legacy_plan.get("session_id"):
            raise ValueError("legacy notice handoff conflicts with selected session; preserved")
        legacy.unlink()
        _sync(root)
    (root / ".session-selection.json").unlink()
    _sync(root)
    return marker


def prepare_session(root: Path, session_id: str, *, lock_fd: int,
                    process_boundary: dict | None = None) -> dict:
    """Select fresh current before any owner starts. Requires Docker exclusion."""
    if not _ID.fullmatch(session_id):
        raise ValueError("invalid session identity")
    root = _safe_path(root)
    _check_root_identity(root, lock_fd)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    selected_archive = _safe_path(root / "previous-sessions" / session_id, root)
    if selected_archive.exists() or selected_archive.is_symlink():
        raise ValueError("session identity already names a previous session; preserved")
    _directory(_directory(root, "session-reports"), "pending")
    selection = root / ".session-selection.json"
    if selection.exists():
        _finish_selection(root, _read(selection))
    report = _legacy(root)
    archive_id = None
    if (root / "current").exists() or (root / "current").is_symlink():
        current = _safe_path(root / "current", root)
        marker = _marker(current)
        if marker and marker["session_id"] == session_id:
            raise ValueError("session identity has already been selected; preserved")
        archive_id = marker["session_id"] if marker else uuid.uuid4().hex
        previous = _directory(root, "previous-sessions")
        if (previous / archive_id).exists() or (previous / archive_id).is_symlink():
            raise ValueError("previous session archive already exists; preserved")
        report = _report(root, archive_id, marker)
    marker = {"schema_version": MARKER_SCHEMA, "session_id": session_id,
              "started_at_wall": datetime.now(timezone.utc).isoformat(),
              "disposition": "started", "shutdown": "unknown", "effects": "unknown",
              "launcher_claimed": False, "previous_session": report,
              "process_boundary": process_boundary or {"kind": "direct"}}
    plan = {"schema_version": "manyforge.workbench_session_selection.v1",
            "archive_id": archive_id, "marker": marker}
    _write(selection, plan)
    return _finish_selection(root, plan)


def _docker_boundary_stopped(root: Path, marker: dict) -> None:
    """Docker authority over the recorded occurrence, never a bare PID/name."""
    boundary = marker.get("process_boundary")
    if not isinstance(boundary, dict) or boundary.get("kind") != "docker" or not boundary.get("container_id"):
        raise ValueError("old process-set termination is unproved; preserve this session and obtain its exact stopped-process evidence")
    identity = boundary["container_id"]
    inspected = subprocess.run(["docker", "inspect", "--format", "{{json .}}", identity], capture_output=True, text=True, check=False)
    if inspected.returncode != 0:
        # A successful complete Docker inventory distinguishes removed exact
        # occurrence from an unavailable daemon/failed inspection.
        listed = subprocess.run(["docker", "container", "ls", "--all", "--quiet", "--no-trunc"], capture_output=True, text=True, check=False)
        identities = listed.stdout.splitlines()
        if listed.returncode == 0 and all(re.fullmatch(r"[0-9a-f]{64}", value) for value in identities) and identity not in identities:
            return
        raise ValueError("Docker could not prove recorded previous container terminated: " + identity)
    if len(inspected.stdout.encode()) > 64 * 1024:
        raise ValueError("oversized previous-container inspection; preserved")
    row = json.loads(inspected.stdout)
    state = row.get("State", {})
    labels = row.get("Config", {}).get("Labels") or {}
    roots = [Path(mount["Source"]).resolve() for mount in row.get("Mounts", []) if mount.get("Type") == "bind" and mount.get("Destination") == "/tmp/manyforge-state"]
    if (row.get("Id") != identity or state.get("Status") not in {"exited", "dead", "created"}
            or state.get("Running") is not False or state.get("Paused", False) is not False
            or state.get("Restarting", False) is not False
            or labels.get("manyforge.workbench-session") != marker["session_id"]
            or roots != [root.resolve()]):
        raise ValueError("recorded previous container is live, changed or foreign; preserved: " + identity)


def prepare_docker_session(root: Path, session_id: str, *, lock_fd: int,
                           stopped_container_receipt: Path | None = None) -> dict:
    root = _safe_path(root)
    _check_root_identity(root, lock_fd)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    marker = _marker(_safe_path(root / "current", root))
    previous_material = (root / "current").exists() or bool(set(os.listdir(root)) & _LEGACY)
    if marker is not None:
        if marker.get("disposition") != "stopped" or marker.get("shutdown") != "clean":
            _docker_boundary_stopped(root, marker)
    elif previous_material and not (root / ".legacy-session-transition.json").exists() and not (root / ".session-selection.json").exists():
        if stopped_container_receipt is None:
            raise ValueError("previous session has no exact stopped process-set proof; preserved")
        receipt = _read(stopped_container_receipt)
        state = receipt.get("state", {})
        roots = [Path(mount["Source"]).resolve() for mount in receipt.get("mounts", []) if mount.get("Type") == "bind" and mount.get("Destination") == "/tmp/manyforge-state"]
        if (re.fullmatch(r"[0-9a-f]{64}", str(receipt.get("container_id", ""))) is None
                or state.get("running") is not False or state.get("status") not in {"exited", "dead", "created"}
                or roots != [root]):
            raise ValueError("stopped-container receipt does not prove this root; preserved")
    return prepare_session(root, session_id, lock_fd=lock_fd, process_boundary={"kind": "docker", "container_id": None})


def bind_container(root: Path, session_id: str, container_id: str, *, lock_fd: int) -> None:
    root = _safe_path(root)
    _check_root_identity(root, lock_fd)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    marker = _marker(root / "current")
    if (not re.fullmatch(r"[0-9a-f]{64}", container_id) or marker is None
            or marker["session_id"] != session_id or marker["launcher_claimed"]
            or marker["process_boundary"] != {"kind": "docker", "container_id": None}):
        raise ValueError("container occurrence cannot bind this selected session; preserved")
    marker["process_boundary"] = {"kind": "docker", "container_id": container_id}
    _write(root / "current/session.json", marker)


class Session:
    """One lifetime root lock, never inherited by service children."""
    def __init__(self, root: Path, session_id: str):
        self.root = _safe_path(root)
        self.fd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            # Host holds this through docker run and releases immediately after.
            while True:
                selected = _marker(_safe_path(self.root / "current", self.root))
                if not selected or selected.get("session_id") != session_id:
                    raise ValueError("selected workbench session changed before launch")
                if selected.get("launcher_claimed") is True:
                    raise ValueError("selected workbench session already has a launcher")
                try:
                    fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    time.sleep(0.05)
            _check_root_identity(self.root, self.fd)
            self.marker = _marker(_safe_path(self.root / "current", self.root))
            if not self.marker or self.marker["session_id"] != session_id or self.marker["disposition"] != "started" or self.marker.get("launcher_claimed") is True:
                raise ValueError("selected workbench session is missing, changed or partial")
            if self.marker["process_boundary"] == {"kind": "docker", "container_id": None}:
                raise ValueError("selected Docker occurrence was not durably bound; preserved")
            self.marker = dict(self.marker, launcher_claimed=True)
            _write(self.root / "current/session.json", self.marker)
        except BaseException:
            os.close(self.fd)
            raise

    @classmethod
    def direct(cls, root: Path) -> Session:
        """Direct startup shares the lock; never infer legacy PID termination."""
        root = _safe_path(root)
        descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            _check_root_identity(root, descriptor)
            current = root / "current"
            if current.exists():
                marker = _marker(_safe_path(current, root))
                if not marker or marker.get("disposition") != "stopped" or marker.get("shutdown") != "clean":
                    if marker is None:
                        raise ValueError("direct startup cannot prove the previous unclosed process set stopped; preserve its exact stopped-process evidence")
                    _docker_boundary_stopped(root, marker)
            elif set(os.listdir(root)) & _LEGACY:
                raise ValueError("direct startup cannot prove legacy process termination; preserve its exact stopped-process evidence")
            marker = prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor)
            marker = dict(marker, launcher_claimed=True)
            _write(root / "current/session.json", marker)
            instance = cls.__new__(cls)
            instance.root, instance.fd, instance.marker = root, descriptor, marker
            return instance
        except BaseException:
            os.close(descriptor)
            raise

    def stopped(self, *, verified: bool) -> None:
        _check_root_identity(self.root, self.fd)
        marker = dict(self.marker, disposition="stopped", shutdown="clean" if verified else "unknown", effects=classify_effects(self.root / "current"))
        _write(self.root / "current/session.json", marker)

    def close(self) -> None:
        os.close(self.fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--lock-fd", required=True, type=int)
    parser.add_argument("--bind-container-id")
    parser.add_argument("--stopped-container-receipt", type=Path)
    args = parser.parse_args()
    try:
        if args.bind_container_id is not None:
            bind_container(args.root, args.session_id, args.bind_container_id, lock_fd=args.lock_fd)
            return 0
        marker = prepare_docker_session(args.root, args.session_id, lock_fd=args.lock_fd, stopped_container_receipt=args.stopped_container_receipt)
    except (OSError, ValueError) as error:
        print("development_workbench_session_unavailable: " + str(error)[:1024], file=sys.stderr)
        return 69
    print(marker["session_id"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
