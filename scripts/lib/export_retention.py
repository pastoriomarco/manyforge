#!/usr/bin/env python3
"""Bounded console capture and quiescent log retention, excluding live records."""

from __future__ import annotations

from datetime import datetime, timezone

import argparse
from collections import deque
from contextlib import ExitStack
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import time
from typing import BinaryIO, Callable
import importlib.util


def _load_run_session():
    # The run selection and its retention pass are shared with the operator
    # launcher (manyforge_operator/run_session.py). It is loaded by file: this
    # helper runs on the host, where the operator package's contract
    # dependencies (pydantic) are not installed.
    name = "manyforge_run_session"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).resolve().parents[2] / "manyforge_operator/run_session.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


run_session = _load_run_session()
_safe_path = run_session._safe_path
_check_root_identity = run_session._check_root_identity
_open_relative_directory = run_session._open_relative_directory
_mounted_children = run_session._mounted_children
_allocated_bytes = run_session._allocated_bytes
_children = run_session._children
_safe_inventory_path = run_session._safe_inventory_path


CONSOLE_TAIL_BYTES = 10_000_000
# Completed console captures kept in a state root's evidence/, whatever the
# disk use; the oldest beyond are deleted by the same bounded pass.
MAXIMUM_CONSOLE_CAPTURES = 10
_MAX_DELETIONS = 8
_MAX_SCAN_ENTRIES = 100_000
_RECEIPT_PARTIAL = re.compile(r"\.mounted-[0-9a-f]{64}\.partial\Z")
_EXPORT_NAME = re.compile(r"[0-9]{8}T[0-9]{6}Z(?:\.[0-9]{9})?(?:\.partial)?\Z")


# One set of disk limits (from the site settings file) and one free-space
# reading for both launchers (run_session).
SiteRetentionPolicy = run_session.DiskLimits
DiskFacts = run_session.DiskReading


@dataclass(frozen=True)
class RetentionResult:
    deleted: tuple[str, ...] = ()
    warning: str | None = None


def capture_tail(source: BinaryIO, destination: Path, *, limit: int = CONSOLE_TAIL_BYTES) -> None:
    """Keep the last bytes, including one arbitrarily long console line."""
    chunks: deque[bytes] = deque()
    retained = 0
    while chunk := source.read(64 * 1024):
        chunks.append(chunk)
        retained += len(chunk)
        while retained > limit:
            first = chunks.popleft()
            excess = retained - limit
            if len(first) > excess:
                chunks.appendleft(first[excess:])
                retained = limit
            else:
                retained -= len(first)
    with destination.open("xb") as output:
        for chunk in chunks:
            output.write(chunk)


def _identity(row: os.stat_result) -> tuple[int, ...]:
    return row.st_dev, row.st_ino, row.st_size, row.st_mtime_ns, row.st_ctime_ns


_disk_facts = run_session.read_disk


def _read_summary(folder: Path) -> dict:
    path = folder / "docker-inspect.json"
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 64 * 1024:
        raise ValueError("unsafe or oversized capture summary")
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError("unknown capture summary")
    return value


def _owned_capture(root: Path, summary: dict) -> bool:
    mounts = summary.get("mounts", [])
    return (summary.get("manyforge_export_kind") == "console_tail" and
            summary.get("state", {}).get("running") is False and
            re.fullmatch(r"[0-9a-f]{64}", summary.get("container_id", "")) is not None and
            re.fullmatch(r"sha256:[0-9a-f]{64}", summary.get("image", "")) is not None and
            any(mount.get("Type") == "bind" and mount.get("Destination") == "/workspace" for mount in mounts) and
            any(mount.get("Type") == "bind" and mount.get("Destination") == "/tmp/manyforge-state" and
                Path(mount.get("Source", "")).resolve() == root.resolve() for mount in mounts))


def _epoch_order(root: Path, epoch: Path, warnings: list[str] | None = None, label: str | None = None) -> int | None:
    """Recognize the existing generated metadata, never an epoch name alone."""
    try:
        observations: list[str] = []
        components = _children(epoch, observations, label or str(epoch))
        if warnings is not None:
            warnings.extend(observations)
        if observations or not components:
            return None
        marker_times = []
        for component in components:
            if component.is_symlink() or not component.is_dir():
                continue
            marker = component / "controller_launch_artifacts.json"
            if not marker.is_symlink() and not marker.exists():
                continue
            if marker.is_symlink() or not marker.is_file() or marker.stat().st_size > 64 * 1024:
                return None
            value = json.loads(marker.read_text())
            expected = Path("/tmp/manyforge-state") / component.relative_to(root)
            if value.get("controllable_component_id") != component.name:
                return None
            for key, name in (("robot_description", "robot_description.urdf"), ("controllers", "controllers.yaml")):
                if value.get(key + "_file") != str(expected / name) or not re.fullmatch(r"[0-9a-f]{64}", value.get(key + "_sha256", "")):
                    return None
            marker_times.append(marker.stat().st_mtime_ns)
        return max(marker_times) if marker_times else None
    except (OSError, ValueError, AttributeError, TypeError):
        return None


def retained_candidates(rows: list[dict], workspace: Path, current_id: str,
                        legacy_evidence: Path) -> list[tuple[str, bool]]:
    """Reuse partial summary receipts; unidentified stopped containers stay put."""
    candidates = []
    for row in rows:
        identity = row.get("Id", "")
        state = row.get("State", {})
        if identity == current_id or state.get("Running") is not False or state.get("Paused") or state.get("Restarting"):
            continue
        mounts = row.get("Mounts", [])
        if not any(mount.get("Type") == "bind" and mount.get("Destination") == "/workspace" and
                   Path(mount.get("Source", "")).resolve() == workspace.resolve() for mount in mounts):
            continue
        root = next((Path(mount["Source"]) for mount in mounts if mount.get("Type") == "bind" and
                     mount.get("Destination") == "/tmp/manyforge-state"), None)
        matched = False
        try:
            evidence = _safe_path(root.resolve() / "evidence", root.resolve()) if root else legacy_evidence.resolve()
            observations: list[str] = []
            folders = _children(evidence, observations, str(evidence)) if evidence.exists() else []
            for message in observations:
                print("WARNING ManyForge " + message, file=sys.stderr)
            for folder in folders:
                if not (_EXPORT_NAME.fullmatch(folder.name) or _RECEIPT_PARTIAL.fullmatch(folder.name)) or not folder.name.endswith(".partial") or folder.is_symlink():
                    continue
                receipt = _read_summary(folder)
                if receipt.get("container_id") == identity and receipt.get("image") == row.get("Image") and receipt.get("mounts") == mounts and receipt.get("state", {}).get("running") is False:
                    matched = True
                    break
        except (OSError, ValueError, TypeError):
            matched = False
        if matched and re.fullmatch(r"[0-9a-f]{64}", identity):
            candidates.append((identity, root is not None))
        else:
            print(f"WARNING ManyForge kept unidentified stopped container {identity[:64]}; manually export it or explicitly remove it with docker rm after investigation", file=sys.stderr)
    return candidates


def prepare_capture(evidence: Path, timestamp: str, summary: dict,
                    state_root: Path | None = None) -> Path:
    """Reuse one receipt-proven partial; unknown copies stay put."""
    if (not re.fullmatch(r"[0-9a-f]{64}", summary.get("container_id", "")) or
            summary.get("state", {}).get("running") is not False or
            summary.get("state", {}).get("status", "exited") not in ("exited", "dead", "created")):
        raise ValueError("invalid stopped-container capture receipt")
    if not _EXPORT_NAME.fullmatch(timestamp) or timestamp.endswith(".partial"):
        raise ValueError("invalid capture timestamp")
    encoded = (json.dumps(summary, separators=(",", ":")) + "\n").encode()
    if len(encoded) > 65536:
        raise ValueError("capture summary exceeds 64KB")
    with ExitStack() as resources:
        if state_root is not None:
            root = state_root.resolve()
            if evidence != root / "evidence":
                raise ValueError("capture path is outside the actual state root")
            root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            resources.callback(os.close, root_fd)
            try:
                os.mkdir("evidence", dir_fd=root_fd)
            except FileExistsError:
                pass
            evidence_fd = os.open("evidence", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
        else:
            evidence = evidence.resolve()
            evidence.mkdir(parents=True, exist_ok=True)
            evidence_fd = os.open(evidence, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        resources.callback(os.close, evidence_fd)
        alias = Path(f"/proc/self/fd/{evidence_fd}")
        partial_name = ".mounted-" + summary["container_id"] + ".partial"
        matches: list[tuple[str, tuple[int, ...]]] = []
        observations: list[str] = []
        for folder in _children(alias, observations, str(evidence)):
            if folder.is_symlink() or not folder.name.endswith(".partial"):
                continue
            if not (_EXPORT_NAME.fullmatch(folder.name) or folder.name == partial_name):
                continue
            try:
                receipt = _read_summary(folder)
            except (OSError, ValueError):
                continue
            if (receipt.get("container_id"), receipt.get("image"), receipt.get("mounts"), receipt.get("state", {}).get("running")) != (summary["container_id"], summary.get("image"), summary.get("mounts"), False):
                continue
            if receipt.get("manyforge_export_kind") != "console_tail":
                continue
            matches.append((folder.name, _identity(folder.stat(follow_symlinks=False))))
        for message in observations:
            print("WARNING ManyForge " + message, file=sys.stderr)
        existing = alias / partial_name
        if (existing.exists() or existing.is_symlink()) and not any(name == partial_name for name, _ in matches):
            raise ValueError("mounted partial is unknown or changed; refusing replacement")
        if len(matches) > 1:
            raise ValueError("multiple historical mounted partials for " + summary["container_id"] + "; preserved, no additional partial; inspect before manual cleanup")
        for name, identity in matches:
            if _identity(os.stat(name, dir_fd=evidence_fd, follow_symlinks=False)) != identity:
                raise ValueError("partial changed before replacement")
            proof_fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=evidence_fd)
            try:
                _allocated_bytes(evidence / name, os.fstat(proof_fd).st_dev,
                                 _mounted_children(evidence / name), set(), proof_fd)
            finally:
                os.close(proof_fd)
            if name != partial_name:
                os.rename(name, partial_name, src_dir_fd=evidence_fd, dst_dir_fd=evidence_fd)
        if not matches:
            os.mkdir(partial_name, dir_fd=evidence_fd)
        partial_fd = os.open(partial_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=evidence_fd)
        resources.callback(os.close, partial_fd)
        summary_name = ".docker-inspect.next.json"
        # Receipt proves ownership; extra entries are removed only within it.
        _allocated_bytes(evidence / partial_name, os.fstat(partial_fd).st_dev,
                         _mounted_children(evidence / partial_name), set(), partial_fd)
        for entry in os.listdir(partial_fd):
            row = os.stat(entry, dir_fd=partial_fd, follow_symlinks=False)
            if entry in {"docker-inspect.json", "docker.log"} and stat.S_ISREG(row.st_mode) and row.st_size <= (65536 if entry.endswith("json") else CONSOLE_TAIL_BYTES):
                continue
            if stat.S_ISDIR(row.st_mode):
                if not shutil.rmtree.avoids_symlink_attacks:
                    raise ValueError("platform cannot clean owned capture safely")
                shutil.rmtree(entry, dir_fd=partial_fd)
            else:
                os.unlink(entry, dir_fd=partial_fd)
        output_fd = os.open(summary_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=partial_fd)
        with os.fdopen(output_fd, "wb") as output:
            output.write(encoded)
        os.replace(summary_name, "docker-inspect.json", src_dir_fd=partial_fd, dst_dir_fd=partial_fd)
        return evidence / partial_name


def _ros_session_name(name: str) -> tuple[int, int] | None:
    match = re.fullmatch(r"([1-9][0-9]*)-([1-9][0-9]{0,19})", name)
    return (int(match[1]), int(match[2])) if match else None


def create_host_ros_log_session(root: Path) -> Path:
    """Mint order in existing directory names under the state-root lock."""
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(root_fd, fcntl.LOCK_EX)
        logs = root / "execution-run/ros"
        logs.mkdir(parents=True, exist_ok=True)
        sequence = 0
        # Complete, nonrecursive name scan: never mint from a partial maximum.
        with os.scandir(logs) as entries:
            for entry in entries:
                parsed = _ros_session_name(entry.name)
                if parsed is not None and entry.is_dir(follow_symlinks=False):
                    sequence = max(sequence, parsed[0])
        folder = logs / f"{sequence + 1}-{time.time_ns()}"
        folder.mkdir()
        return folder
    finally:
        os.close(root_fd)


def _ros_session_order(folder: Path, latest: tuple[int, int], warnings: list[str]) -> int | None:
    """Sequence chooses latest; actual start time conservatively checks files."""
    parsed = _ros_session_name(folder.name)
    if parsed is None or parsed[0] >= latest[0]:
        return None
    order, cutoff = parsed[1], latest[1]
    entries = 0
    try:
        for _, directories, files, fd in os.fwalk(folder, follow_symlinks=False):
            for name in (*directories, *files):
                entries += 1
                if entries > _MAX_SCAN_ENTRIES:
                    raise ValueError("host ROS log scan exceeds bounded inventory")
                row = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if stat.S_ISLNK(row.st_mode) and name in directories:
                    directories.remove(name)
                if not stat.S_ISDIR(row.st_mode) and row.st_mtime_ns >= cutoff:
                    return None
        return order
    except (OSError, ValueError) as error:
        warnings.append(f"{folder}: host ROS session protected: {str(error)[:200]}")
        return None


def _ros_session_candidates(logs: Path, warnings: list[str], label: str) -> tuple[list[Path], tuple[int, int] | None]:
    """List all session names; cap only the expensive candidate work."""
    candidates: list[Path] = []
    unproved: list[Path] = []
    latest = None
    sequences: set[int] = set()
    ambiguous = False
    complete = True
    count = 0
    try:
        with os.scandir(logs) as entries:
            for entry in entries:
                if not entry.is_dir(follow_symlinks=False):
                    continue
                count += 1
                # Protected legacy names must not starve sequenced candidates
                # of the existing 1,000-unit deep inventory budget.
                parsed = _ros_session_name(entry.name)
                if parsed is None:
                    if len(unproved) < 1000:
                        unproved.append(logs / entry.name)
                    continue
                if len(candidates) < 1000:
                    candidates.append(logs / entry.name)
                ambiguous = ambiguous or parsed[0] in sequences
                sequences.add(parsed[0])
                if latest is None or parsed[0] > latest[0]:
                    latest = parsed
    except OSError as error:
        complete = False
        warnings.append(f"{label}: session name listing failed; all observed ROS sessions protected: {str(error)[:200]}")
    if ambiguous:
        warnings.append(f"{label}: ambiguous session sequence; all observed ROS sessions protected")
    candidates.extend(unproved[:1000 - len(candidates)])
    if count > len(candidates):
        warnings.append(f"{label}: bounded deep inventory ({len(candidates)} of {count} session units); accounting incomplete (lower bound)")
    return candidates, latest if complete and not ambiguous else None


def _units(root: Path, *, sibling: bool = False, ownership_root: Path | None = None, protection: str | None = None, warnings: list[str] | None = None) -> list[tuple[Path, str | None, int | None]]:
    units: list[tuple[Path, str | None, int | None]] = []
    ownership_root = ownership_root or root
    warnings = warnings if warnings is not None else []
    protection = "sibling root (not this quiescent owner)" if sibling else protection
    evidence = root / "evidence"
    if _safe_inventory_path(evidence, root, ownership_root, warnings) and evidence.exists():
        completed = []
        folders = _children(evidence, warnings, str(ownership_root / "evidence"))
        for folder in folders:
            if not (_EXPORT_NAME.fullmatch(folder.name) or _RECEIPT_PARTIAL.fullmatch(folder.name)):
                continue
            if not _safe_inventory_path(folder, root, ownership_root, warnings):
                continue
            order = None
            reason = "capture incomplete" if folder.name.endswith(".partial") else protection
            try:
                summary = _read_summary(folder)
                if not _owned_capture(ownership_root, summary):
                    reason = reason or "unknown capture ownership"
                elif not folder.name.endswith(".partial"):
                    stamp, _, nanos = folder.name.partition(".")
                    order = int(datetime.strptime(stamp, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc).timestamp()) * 1_000_000_000 + int(nanos or "0")
                    completed.append(folder)
            except (OSError, ValueError, AttributeError, TypeError):
                reason = reason or "unknown capture ownership"
            units.append((folder, reason, order))
        if completed:
            latest = max(completed, key=lambda folder: folder.name)
            units = [(folder, reason or ("most recent console capture" if folder == latest else None), order) for folder, reason, order in units]
    # Cold-session archives contain the original owner journals, including
    # facts not imported into History. This loop only accounts for them; the
    # shared run pruning (run_session.prune_own_files) alone deletes them.
    previous = root / "previous-runs"
    if _safe_inventory_path(previous, root, ownership_root, warnings) and previous.exists():
        for folder in _children(previous, warnings, str(ownership_root / "previous-runs")):
            if _safe_inventory_path(folder, root, ownership_root, warnings) and folder.is_dir():
                units.append((folder, "previous-session journals kept; history coverage not_imported or unknown", None))
    runtime = root / "current" if (root / "current").exists() else root
    artifacts = runtime / "execution-run/controller-launch-artifacts"
    if _safe_inventory_path(artifacts, root, ownership_root, warnings) and artifacts.exists():
        for bundle in _children(artifacts, warnings, str(ownership_root / "execution-run/controller-launch-artifacts")):
            if not _safe_inventory_path(bundle, root, ownership_root, warnings) or not bundle.is_dir():
                continue
            epochs = [epoch for epoch in _children(bundle, warnings, str(ownership_root / bundle.relative_to(root)))
                      if _safe_inventory_path(epoch, root, ownership_root, warnings)]
            owned = {epoch: _epoch_order(root, epoch, warnings, str(ownership_root / epoch.relative_to(root)))
                     for epoch in epochs if not epoch.is_symlink() and epoch.is_dir()}
            newest = max((order for order in owned.values() if order is not None), default=None)
            for epoch in epochs:
                reason = protection
                if not epoch.is_dir():
                    continue
                order = owned.get(epoch)
                if order is None:
                    reason = reason or "unknown controller-launch artifact ownership or marker time"
                elif order == newest:
                    reason = reason or "latest controller-launch artifact epoch (equal marker times protected)"
                units.append((epoch, reason, order))
    ros_logs = runtime / "execution-run/ros"
    if _safe_inventory_path(ros_logs, root, ownership_root, warnings) and ros_logs.exists():
        folders, latest = _ros_session_candidates(ros_logs, warnings, str(ownership_root / "execution-run/ros"))
        sessions = [folder for folder in folders
                    if _safe_inventory_path(folder, root, ownership_root, warnings) and folder.is_dir()]
        for folder in sessions:
            order = _ros_session_order(folder, latest, warnings) if latest is not None else None
            reason = protection
            if order is None:
                reason = reason or "latest, current-session or unproved host ROS logs"
            units.append((folder, reason, order))
    return [(folder, "sibling root (not this quiescent owner)" if sibling else reason, order) for folder, reason, order in units]


def _capture_beyond_count(root: Path, units: list) -> Path | None:
    """The oldest deletable completed, owned capture of ``root`` beyond the count.

    Every completed, owned capture counts; the newest, partial, unknown and
    otherwise protected ones are never chosen.
    """
    captures = [(order, str(folder), reason) for folder, _, reason, order in units
                if folder.parent == root / "evidence" and order is not None]
    if len(captures) <= MAXIMUM_CONSOLE_CAPTURES:
        return None
    eligible = sorted((order, name) for order, name, reason in captures if reason is None)
    return Path(eligible[0][1]) if eligible else None


def rotate_retained_logs(root: Path, policy: SiteRetentionPolicy, *,
                         disk_facts: Callable[[int], DiskFacts] = _disk_facts,
                         default_roots: Path | None = None) -> RetentionResult:
    """One pass, only before start/after confirmed stop and a no-users check.

    Console captures beyond MAXIMUM_CONSOLE_CAPTURES go first, then units
    under disk pressure; at most _MAX_DELETIONS attempts in all.
    """
    deleted: list[str] = []
    root_fd = -1
    resources = ExitStack()
    failures: dict[Path, str] = {}
    session_warning: str | None = None
    try:
        root = _safe_path(root)
        if not root.exists():
            return RetentionResult()
        if root == Path("/"):
            raise ValueError("filesystem root is not a ManyForge state root")
        root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        fcntl.flock(root_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        pruned, session_warning = run_session.prune_own_files(
            root, root_fd, policy, reader=disk_facts)
        deleted.extend(pruned)
        for _ in range(_MAX_DELETIONS + 1):
            _check_root_identity(root, root_fd)
            units, owned_bytes, seen = [], 0, set()
            mounts = _mounted_children(root)
            root_device = os.fstat(root_fd).st_dev
            observations: list[str] = [session_warning] if session_warning else []
            with ExitStack() as inventory:
                roots = [(root, root_fd)]
                base_fd = None
                if default_roots is not None:
                    base = _safe_path(default_roots)
                    if root.parent != base:
                        raise ValueError("selected root is outside the declared default roots")
                    base_fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                    inventory.callback(os.close, base_fd)
                    base_alias = Path(f"/proc/self/fd/{base_fd}")
                    for sibling in _children(base_alias, observations, str(base)):
                        if sibling.name == root.name:
                            continue
                        row = sibling.stat(follow_symlinks=False)
                        if stat.S_ISDIR(row.st_mode) and row.st_dev == root_device:
                            roots.append((base / sibling.name, None))
                        elif stat.S_ISLNK(row.st_mode):
                            observations.append(f"{base / sibling.name}: symlinked sibling protected, accounting incomplete")
                for owner, held_fd in roots:
                    with ExitStack() as owner_resources:
                        owner_fd = held_fd
                        if owner_fd is None:
                            owner_fd = os.open(owner.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=base_fd)
                            owner_resources.callback(os.close, owner_fd)
                            mounts.update(_mounted_children(owner))
                        alias = Path(f"/proc/self/fd/{owner_fd}")
                        for aliased, reason, order in _units(alias, sibling=owner != root, ownership_root=owner, warnings=observations):
                            relative = aliased.relative_to(alias)
                            folder = owner / relative
                            try:
                                _safe_path(aliased, alias)
                                unit_fd = _open_relative_directory(owner_fd, relative)
                            except (OSError, ValueError) as error:
                                observations.append(f"{folder}: unit unavailable; protected, accounting incomplete (lower bound): {str(error).replace(str(alias), str(owner))[:200]}")
                                continue
                            try:
                                row = os.fstat(unit_fd)
                                if row.st_dev != root_device:
                                    raise ValueError(f"cross-mount retained unit: {folder}")
                                try:
                                    size, safe = _allocated_bytes(folder, root_device, mounts, seen, unit_fd)
                                except (OSError, ValueError) as error:
                                    size, safe = 0, False
                                    reason = f"accounting incomplete; protected: {str(error)[:200]}"
                                    observations.append(f"{folder}: {reason}")
                            finally:
                                os.close(unit_fd)
                            owned_bytes += size
                            reason = failures.get(folder) or reason or (None if safe else "symlink or special file in retained unit")
                            units.append((folder, row, reason, order))
            victim = _capture_beyond_count(root, units) if len(deleted) + len(failures) < _MAX_DELETIONS else None
            if victim is None:
                facts = disk_facts(root_fd)
                _check_root_identity(root, root_fd)
                if facts.total_bytes <= 0 or facts.free_bytes < 0:
                    raise ValueError("invalid filesystem capacity facts")
                if owned_bytes * 100 <= facts.total_bytes * policy.max_disk_percent and facts.free_bytes >= policy.min_free_bytes:
                    warning = "; ".join([*observations, *(f"{folder}: {reason}" for folder, reason in failures.items())])[:4096] or None
                    return RetentionResult(tuple(deleted), warning)
                eligible = sorted((folder for folder, _, reason, order in units if reason is None and order is not None),
                                  key=lambda folder: (next(order for path, _, _, order in units if path == folder), str(folder)))
                if not eligible or len(deleted) + len(failures) >= _MAX_DELETIONS:
                    detail = "; ".join([*(f"{folder}: {reason}" for folder, reason in failures.items()), *observations,
                                        *(f"{folder}: {reason}" for folder, _, reason, _ in units if reason is not None and folder not in failures)])
                    scope = "default-root aggregate" if default_roots is not None else "configured-root"
                    return RetentionResult(tuple(deleted), f"{scope} log/evidence pressure remains (measured owned={owned_bytes} bytes{', lower bound' if observations else ''}, filesystem free={facts.free_bytes} bytes); "
                        f"{'bounded deletion pass complete' if eligible else 'only protected material remains'}; protected: {detail[:4096]}")
                victim = eligible[0]
            before = next(row for folder, row, _, _ in units if folder == victim)
            try:
                parent_fd = _open_relative_directory(root_fd, victim.relative_to(root).parent)
            except (OSError, ValueError) as error:
                failures[victim] = f"unit parent unavailable; protected for this pass: {str(error)[:200]}"
                continue
            try:
                current = os.stat(victim.name, dir_fd=parent_fd, follow_symlinks=False)
                if (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino):
                    raise ValueError("retained unit changed before deletion")
                if not shutil.rmtree.avoids_symlink_attacks:
                    raise ValueError("platform cannot delete without following directory symlinks")
                victim_fd = os.open(victim.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent_fd)
                try:
                    held_victim = os.fstat(victim_fd)
                    if (before.st_dev, before.st_ino) != (held_victim.st_dev, held_victim.st_ino):
                        raise ValueError("retained unit changed during open")
                    try:
                        _allocated_bytes(victim, root_device, _mounted_children(victim), set(), victim_fd)
                    except (OSError, ValueError) as error:
                        failures[victim] = f"deletion boundary unavailable; protected for this pass: {str(error)[:200]}"
                        continue
                finally:
                    os.close(victim_fd)
                try:
                    shutil.rmtree(victim.name, dir_fd=parent_fd)
                except OSError as error:
                    failures[victim] = f"deletion failed; protected for this pass: {str(error)[:200]}"
                    continue
            finally:
                os.close(parent_fd)
            deleted.append(str(victim.relative_to(root)))
    except (OSError, ValueError) as error:
        detail = "; ".join(f"{folder}: {reason}" for folder, reason in failures.items())
        return RetentionResult(tuple(deleted), f"retention could not complete safely: {error}" + (f"; protected: {detail[:4096]}" if detail else ""))
    finally:
        resources.close()
        if root_fd >= 0:
            os.close(root_fd)
    return RetentionResult(tuple(deleted))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("tail", "rotate", "retained", "prepare"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--current-id", default="")
    parser.add_argument("--legacy-evidence", type=Path)
    parser.add_argument("--default-roots", type=Path)
    parser.add_argument("--timestamp")
    parser.add_argument("--state-root", type=Path)
    parser.add_argument("--site-settings", type=Path)
    arguments = parser.parse_args()
    if arguments.command == "prepare":
        try:
            data = sys.stdin.buffer.read(65537)
            if len(data) > 65536:
                raise ValueError("stopped-container summary exceeds 64KB")
            summary = json.loads(data)
            print(prepare_capture(arguments.path, arguments.timestamp or "", summary,
                                  state_root=arguments.state_root))
            return 0
        except (OSError, ValueError, TypeError, AttributeError) as error:
            print(f"Could not prepare capture: {str(error)[:300]}", file=sys.stderr)
            return 69
    if arguments.command == "tail":
        try:
            capture_tail(sys.stdin.buffer, arguments.path)
            return 0
        except OSError as error:
            print(f"Could not write console tail: {str(error)[:300]}", file=sys.stderr)
            return 69
    if arguments.command == "retained":
        try:
            rows = json.load(sys.stdin)
            if not isinstance(rows, list) or arguments.legacy_evidence is None:
                raise ValueError("incomplete stopped-container inventory")
            for identity, mounted in retained_candidates(rows, arguments.path, arguments.current_id, arguments.legacy_evidence):
                print(identity + " " + ("mounted" if mounted else "legacy"))
            return 0
        except (ValueError, TypeError, OSError) as error:
            print(f"WARNING ManyForge could not qualify retained containers: {str(error)[:300]}", file=sys.stderr)
            return 69
    try:
        policy = run_session.read_site_disk_limits(arguments.site_settings)
    except (OSError, ValueError) as error:
        result = RetentionResult(warning=f"invalid retention site setting: {error}")
    else:
        result = rotate_retained_logs(arguments.path, policy, default_roots=arguments.default_roots)
    for name in result.deleted:
        print(f"ManyForge removed oldest closed log/evidence unit: {name}", file=sys.stderr)
    if result.warning:
        print(f"WARNING ManyForge {result.warning}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
