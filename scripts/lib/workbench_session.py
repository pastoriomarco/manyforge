"""Cold sessions for the development workbench; never starts old journal owners.

Selection is called only after compose proves the old container stopped and no
live container has an overlapping bind. The caller holds the installation root
flock across selection and docker run. The child takes that same lock for life.
The selection itself is shared with the operator launcher
(``manyforge_operator.run_session``, loaded by ``export_retention``); this
module adds the Docker process-set boundary and the dev state root's own
entries.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.lib.export_retention import run_session

_check_root_identity = run_session._check_root_identity
_marker = run_session._marker
_read = run_session._read
_safe_path = run_session._safe_path
_write = run_session._write
claim_selected = run_session.claim_selected
record_stopped = run_session.record_stopped

# The dev state root's own kept entries besides the session entries.
STABLE_ENTRIES = frozenset({
    "evidence",
    # The simulated PLC's own per-device settings, kept across sessions.
    "sim_plc",
    # Optional, bounded diagnostic snapshots survive cold Workbench sessions.
    "motion-traces",
    # The History directory, and the earlier History file kept beside it.
    "history",
    *("history.sqlite3" + suffix for suffix in ("", "-wal", "-shm", "-journal", ".headless-owner.lock")),
})


def prepare_session(root: Path, session_id: str, *, lock_fd: int,
                    process_boundary: dict | None = None,
                    disk_limits: run_session.DiskLimits | None = None) -> dict:
    """Select fresh current for the dev state root. Requires Docker exclusion.

    First ManyForge's own obsolete files are pruned and the free-space floor
    is checked (run_session.DiskBelowFloor), before anything is written.
    ``disk_limits`` come from the site settings file (default: the defaults).
    """
    run_session.prepare_disk(_safe_path(root), lock_fd, disk_limits or run_session.DiskLimits())
    return run_session.prepare_session(root, session_id, lock_fd=lock_fd, stable_entries=STABLE_ENTRIES,
                            process_boundary=process_boundary)


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
                           stopped_container_receipt: Path | None = None,
                           disk_limits: run_session.DiskLimits | None = None) -> dict:
    root = _safe_path(root)
    _check_root_identity(root, lock_fd)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    marker = _marker(_safe_path(root / "current", root))
    if marker is not None:
        if marker.get("disposition") != "stopped" or marker.get("shutdown") != "clean":
            _docker_boundary_stopped(root, marker)
    elif (root / "current").exists() and not (root / ".session-selection.json").exists():
        if stopped_container_receipt is None:
            raise ValueError("previous session has no exact stopped process-set proof; preserved")
        receipt = _read(stopped_container_receipt)
        state = receipt.get("state", {})
        roots = [Path(mount["Source"]).resolve() for mount in receipt.get("mounts", []) if mount.get("Type") == "bind" and mount.get("Destination") == "/tmp/manyforge-state"]
        if (re.fullmatch(r"[0-9a-f]{64}", str(receipt.get("container_id", ""))) is None
                or state.get("running") is not False or state.get("status") not in {"exited", "dead", "created"}
                or roots != [root]):
            raise ValueError("stopped-container receipt does not prove this root; preserved")
    return prepare_session(root, session_id, lock_fd=lock_fd, process_boundary={"kind": "docker", "container_id": None},
                           disk_limits=disk_limits)


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
    def direct(cls, root: Path, disk_limits: run_session.DiskLimits | None = None) -> Session:
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
            marker = claim_selected(root, prepare_session(root, uuid.uuid4().hex, lock_fd=descriptor,
                                                          disk_limits=disk_limits))
            instance = cls.__new__(cls)
            instance.root, instance.fd, instance.marker = root, descriptor, marker
            return instance
        except BaseException:
            os.close(descriptor)
            raise

    def stopped(self, *, verified: bool) -> None:
        record_stopped(self.root, self.fd, self.marker, verified=verified)

    def close(self) -> None:
        os.close(self.fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--lock-fd", required=True, type=int)
    parser.add_argument("--bind-container-id")
    parser.add_argument("--stopped-container-receipt", type=Path)
    parser.add_argument("--site-settings", type=Path)
    args = parser.parse_args()
    try:
        if args.bind_container_id is not None:
            bind_container(args.root, args.session_id, args.bind_container_id, lock_fd=args.lock_fd)
            return 0
        marker = prepare_docker_session(args.root, args.session_id, lock_fd=args.lock_fd, stopped_container_receipt=args.stopped_container_receipt,
                                        disk_limits=run_session.read_site_disk_limits(args.site_settings))
    except run_session.DiskBelowFloor as error:
        print(f"{error.code}: {error}", file=sys.stderr)
        return 69
    except (OSError, ValueError) as error:
        print("development_workbench_session_unavailable: " + str(error)[:1024], file=sys.stderr)
        return 69
    print(marker["session_id"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
