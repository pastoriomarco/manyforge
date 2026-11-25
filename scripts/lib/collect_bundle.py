#!/usr/bin/env python3
"""One bounded bug-report bundle (tar.gz) of one ManyForge run, read from the host.

``./manyforge collect`` and ``scripts/compose-dev.sh collect`` call this with
the runtime state root. It runs outside the application, only reads the state
root and writes nothing there (the bundle goes elsewhere; temporary copies go
to the system temporary folder; a live database is read as any SQLite reader
reads it, through its existing ``-shm`` index), and deletes nothing.

The bundle holds, in this order while the uncompressed total stays within
``--max-bytes``: the run's ``session.json`` and previous-run report, the
evidence folders of the run's container, its History rows
(``history.ndjson``, through the History exporter's file mode), copies of its
SQLite files, its assistant audit files, the rest of the run folder, and the
motion traces written during the run, newest first. A file that does not fit
is skipped; ``manifest.json`` lists what was included and what was skipped.

``--list`` writes no bundle: it prints one line per run under the state root,
oldest first, read from the same run folders, reports and evidence summaries.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import sys
import tarfile
import tempfile
import time

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))
from scripts.lib.export_retention import run_session  # noqa: E402

MANIFEST_SCHEMA = "manyforge.collect_bundle.v1"
DEFAULT_MAX_BYTES = 512 * 1024 * 1024
HISTORY_MAX_ROWS = 1_000_000
_ID = re.compile(r"[0-9a-f]{32}\Z")
_START_ID = re.compile(r"[0-9a-f]{16}\Z")
_SQLITE = re.compile(r".*\.sqlite3?\Z")
_SQLITE_COMPANION = re.compile(r".*\.sqlite3?-(wal|shm|journal)\Z")
_TRACE = re.compile(r"trace-[0-9a-f]{64}\.json\Z")
_COPY_CHUNK = 1024 * 1024


def _load_history_export():
    """The History exporter, loaded by file (no API dependencies on the host)."""
    path = REPOSITORY / "manyforge_headless_api/history_export.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("manyforge_history_export", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


history_export = _load_history_export()


class CollectError(Exception):
    pass


# ---------------------------------------------------------------------------
# The runs under a state root


def _read_json(path: Path) -> dict | None:
    return run_session._session_metadata(path)


def _report(root: Path, session_id: str) -> Path | None:
    for path in (root / "session-reports" / (session_id + ".json"),
                 root / "session-reports/pending" / (session_id + ".json")):
        if path.is_file() and not path.is_symlink():
            return path
    return None


def _journal_start_id(folder: Path) -> str | None:
    journal = folder / "execution-state/operation-journal.sqlite3"
    if history_export is None or not journal.is_file() or journal.is_symlink():
        return None
    try:
        with history_export.open_readonly(journal) as database:
            rows = database.execute("SELECT application_start_id FROM journal_metadata").fetchall()
    except (OSError, sqlite3.Error):
        return None
    return rows[0][0] if len(rows) == 1 and isinstance(rows[0][0], str) else None


def list_runs(root: Path) -> list[dict]:
    """The runs under ``root``, oldest first: current/ and previous-runs/<id>."""
    folders = []
    previous = root / "previous-runs"
    if previous.is_dir():
        folders += [folder for folder in previous.iterdir()
                    if _ID.fullmatch(folder.name) and folder.is_dir() and not folder.is_symlink()]
    current = root / "current"
    if current.is_dir() and not current.is_symlink():
        folders.append(current)
    runs = []
    for folder in folders:
        marker = _read_json(folder / "session.json") or {}
        session_id = marker.get("session_id") if isinstance(marker.get("session_id"), str) else (
            folder.name if folder.name != "current" else None)
        report_path = _report(root, session_id) if session_id else None
        report = _read_json(report_path) if report_path else None
        start = (report or {}).get("application_start_id")
        if not isinstance(start, str):
            start = _journal_start_id(folder)
        boundary = marker.get("process_boundary")
        runs.append({
            "folder": folder,
            "archived": folder.name != "current",
            "session_id": session_id,
            "application_start_id": start if isinstance(start, str) and _START_ID.fullmatch(start) else None,
            "started_at_wall": marker.get("started_at_wall"),
            "shutdown": marker.get("shutdown"),
            "container_id": boundary.get("container_id") if isinstance(boundary, dict) else None,
            "report": report_path,
            "order": run_session._session_order(folder, folder.stat(follow_symlinks=False)),
        })
    # current/ is always the newest run.
    runs.sort(key=lambda run: (not run["archived"], run["order"]))
    return runs


def _wall_ns(text: object) -> int | None:
    try:
        value = datetime.fromisoformat(text)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
    return int(value.timestamp() * 1_000_000_000) if value.utcoffset() is not None else None


# ---------------------------------------------------------------------------
# The bundle


class Bundle:
    """Members of one tar.gz within an uncompressed byte budget."""

    def __init__(self, tar: tarfile.TarFile, prefix: str, max_bytes: int, scratch: Path):
        self.tar, self.prefix, self.remaining, self.scratch = tar, prefix, max_bytes, scratch
        self.included: list[dict] = []
        self.skipped: list[dict] = []
        self.names: set[str] = set()

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append({"path": name, "reason": reason})

    def fits(self, name: str, size: int) -> bool:
        if size > self.remaining:
            self.skip(name, f"max_bytes: {size} bytes, {self.remaining} left")
            return False
        return True

    def add_spooled(self, name: str, spool, size: int, source: str, mtime: float) -> None:
        info = tarfile.TarInfo(self.prefix + name)
        info.size, info.mtime, info.mode = size, mtime, 0o644
        spool.seek(0)
        self.tar.addfile(info, spool)
        self.remaining -= size
        self.names.add(name)
        self.included.append({"path": name, "bytes": size, "source": source})

    def add_file(self, name: str, path: Path) -> None:
        """A point-in-time copy of up to the file's size when opened."""
        if name in self.names:
            return
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except OSError as error:
            self.skip(name, f"unreadable: {error.strerror}")
            return
        with os.fdopen(descriptor, "rb") as source:
            row = os.fstat(source.fileno())
            if not stat.S_ISREG(row.st_mode):
                self.skip(name, "not a regular file")
                return
            if not self.fits(name, row.st_size):
                return
            with tempfile.TemporaryFile(dir=self.scratch) as spool:
                left = row.st_size
                while left and (chunk := source.read(min(_COPY_CHUNK, left))):
                    spool.write(chunk)
                    left -= len(chunk)
                self.add_spooled(name, spool, row.st_size - left, str(path), row.st_mtime)

    def add_sqlite_backup(self, name: str, path: Path) -> None:
        """A consistent copy of a database that may still be written."""
        if name in self.names:
            return
        if history_export is None:
            self.skip(name, "the SQLite reader (manyforge_headless_api/history_export.py) is not beside this launcher")
            return
        try:
            size = sum(candidate.stat().st_size for candidate in
                       (path, path.with_name(path.name + "-wal")) if candidate.exists())
        except OSError as error:
            self.skip(name, f"unreadable: {error.strerror}")
            return
        if not self.fits(name, size):
            return
        target = self.scratch / ("backup-" + str(len(self.included)) + ".sqlite3")
        try:
            with history_export.open_readonly(path) as source:
                copy = sqlite3.connect(target)
                try:
                    source.backup(copy)
                finally:
                    copy.close()
        except (OSError, sqlite3.Error) as error:
            self.skip(name, f"unreadable: {error}")
            return
        try:
            if self.fits(name, target.stat().st_size):
                with target.open("rb") as spool:
                    self.add_spooled(name, spool, target.stat().st_size, str(path) + " (SQLite backup)",
                                     path.stat().st_mtime)
        finally:
            target.unlink()


def _run_files(folder: Path, skipped: list[dict], prefix: str) -> list[Path]:
    files: list[Path] = []
    for directory, directories, names in os.walk(folder, followlinks=False):
        for name in sorted(names):
            path = Path(directory) / name
            if len(files) == run_session._MAX_SCAN_ENTRIES:
                skipped.append({"path": prefix, "reason": "more files than the scan limit"})
                return files
            if path.is_symlink():
                skipped.append({"path": prefix + str(path.relative_to(folder)), "reason": "symbolic link"})
            else:
                files.append(path)
        directories.sort()
    return files


def _evidence_folders(root: Path, container_id: str | None) -> list[Path]:
    evidence = root / "evidence"
    if container_id is None or not evidence.is_dir():
        return []
    matched = []
    for folder in evidence.iterdir():
        if folder.is_dir() and not folder.is_symlink():
            summary = _read_json(folder / "docker-inspect.json")
            if summary and summary.get("container_id") == container_id:
                matched.append(folder)
    return sorted(matched, key=lambda folder: folder.name, reverse=True)


def _history_files(root: Path) -> list[Path]:
    history = root / "history"
    if not history.is_dir():
        return []
    versions = []
    for path in history.iterdir():
        match = run_session._HISTORY_VERSION.fullmatch(path.name)
        if match and path.is_file() and not path.is_symlink():
            versions.append((int(match[1]), path))
    return [path for _, path in sorted(versions, reverse=True)]


def _add_history(bundle: Bundle, root: Path, start: str | None) -> dict:
    name = "history.ndjson"
    if history_export is None:
        bundle.skip(name, "the History exporter (manyforge_headless_api/history_export.py) is not beside this launcher")
        return {"rows": 0}
    if start is None:
        bundle.skip(name, "the run's application start id is unknown")
        return {"rows": 0}
    for path in _history_files(root):
        with tempfile.TemporaryFile(dir=bundle.scratch) as spool:
            try:
                result = history_export.write_oldest_first(
                    history_export.file_rows_newest_first(path, application_start_id=start),
                    spool, max_rows=HISTORY_MAX_ROWS, max_bytes=bundle.remaining)
            except (OSError, sqlite3.Error, ValueError) as error:
                bundle.skip(name + " from " + path.name, f"unreadable: {error}")
                continue
            if result.rows == 0 and result.stopped_at is None:
                continue
            if result.rows == 0:
                bundle.skip(name, f"max_bytes: {bundle.remaining} bytes left")
                return {"file": str(path.relative_to(root)), "rows": 0, "stopped_at": result.stopped_at}
            bundle.add_spooled(name, spool, result.bytes, str(path), time.time())
            return {"file": str(path.relative_to(root)), "rows": result.rows, "stopped_at": result.stopped_at}
    bundle.skip(name, "no History rows of this start")
    return {"rows": 0}


def describe_runs(root: Path) -> list[str]:
    """One line per run, oldest first; ``-`` where a value is unknown."""
    root = root.resolve()
    if not root.is_dir():
        raise CollectError(f"no runtime state root at {root}")
    lines = []
    for run in list_runs(root):
        evidence = ",".join(str(folder.relative_to(root))
                            for folder in _evidence_folders(root, run["container_id"]))
        fields = (("session", run["session_id"]), ("start", run["application_start_id"]),
                  ("started_at_wall", run["started_at_wall"]), ("shutdown", run["shutdown"]),
                  ("container", run["container_id"]), ("evidence", evidence))
        lines.append(" ".join(f"{name}={value if isinstance(value, str) and value else '-'}"
                              for name, value in fields))
    return lines


def collect(root: Path, out: Path, *, start: str | None, max_bytes: int) -> dict:
    root = root.resolve()
    if not root.is_dir():
        raise CollectError(f"no runtime state root at {root}")
    out = out.absolute()
    if out.resolve().is_relative_to(root):
        raise CollectError("the bundle must be written outside the runtime state root")
    runs = list_runs(root)
    if not runs:
        raise CollectError(f"no runs under {root}")
    if start is None:
        index = len(runs) - 1
    else:
        matches = [i for i, run in enumerate(runs) if run["application_start_id"] == start]
        if not matches:
            known = ", ".join(run["application_start_id"] for run in runs if run["application_start_id"])
            raise CollectError(f"no run of application start {start} under {root} (known: {known or 'none'})")
        index = matches[-1]
    run = runs[index]
    folder = run["folder"]
    begin = _wall_ns(run["started_at_wall"])
    end = _wall_ns(runs[index + 1]["started_at_wall"]) if index + 1 < len(runs) else None

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    prefix = f"manyforge-collect-{stamp}/"
    run_prefix = "run/"
    with tempfile.TemporaryDirectory(prefix="manyforge-collect.") as scratch, \
            tarfile.open(out, "x:gz") as tar:
        bundle = Bundle(tar, prefix, max_bytes, Path(scratch))
        files = _run_files(folder, bundle.skipped, run_prefix)
        relative = {path: run_prefix + str(path.relative_to(folder)) for path in files}

        if (folder / "session.json").is_file():
            bundle.add_file(run_prefix + "session.json", folder / "session.json")
        if run["report"] is not None:
            bundle.add_file(str(run["report"].relative_to(root)), run["report"])
        for evidence in _evidence_folders(root, run["container_id"]):
            for path in sorted(evidence.iterdir()):
                bundle.add_file(str(path.relative_to(root)), path)
        history = _add_history(bundle, root, run["application_start_id"])
        for path in files:
            if _SQLITE.fullmatch(path.name):
                if run["archived"]:
                    # Archived runs are stopped and sealed: their files and
                    # logs are copied as they are.
                    bundle.add_file(relative[path], path)
                    wal = path.with_name(path.name + "-wal")
                    if wal in relative:
                        bundle.add_file(relative[wal], wal)
                else:
                    bundle.add_sqlite_backup(relative[path], path)
        for path in files:
            if path.name.startswith("assistant-audit"):
                bundle.add_file(relative[path], path)
        for path in files:
            if _SQLITE_COMPANION.fullmatch(path.name):
                continue
            bundle.add_file(relative[path], path)
        traces = root / "motion-traces"
        if traces.is_dir() and begin is not None:
            written = []
            for path in traces.iterdir():
                if _TRACE.fullmatch(path.name) and path.is_file() and not path.is_symlink():
                    mtime = path.stat().st_mtime_ns
                    if mtime >= begin and (end is None or mtime < end):
                        written.append((mtime, path))
            for _, path in sorted(written, reverse=True):
                bundle.add_file("motion-traces/" + path.name, path)

        manifest = {
            "schema_version": MANIFEST_SCHEMA,
            "created_at_utc": datetime.now(timezone.utc).isoformat(),
            "state_root": str(root),
            "run_folder": str(folder.relative_to(root)),
            "session_id": run["session_id"],
            "application_start_id": run["application_start_id"],
            "started_at_wall": run["started_at_wall"],
            "container_id": run["container_id"],
            "max_bytes": max_bytes,
            "history": history,
            "included": bundle.included,
            "skipped": bundle.skipped,
        }
        data = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
        info = tarfile.TarInfo(prefix + "manifest.json")
        info.size, info.mtime, info.mode = len(data), time.time(), 0o644
        tar.addfile(info, io.BytesIO(data))
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("state_root", type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--list", action="store_true",
                           help="print one line per run (no bundle); takes no other option")
    selection.add_argument("--start", metavar="APPLICATION_START_ID",
                           help="the run of this application start (16 hex characters)")
    selection.add_argument("--last", action="store_true", help="the newest run (default)")
    parser.add_argument("--out", type=Path, help="new .tar.gz file (default: manyforge-collect-<UTC>.tar.gz here)")
    parser.add_argument("--max-bytes", type=int,
                        help="uncompressed bytes of the bundled files (default 512 MiB)")
    args = parser.parse_args(argv)
    if args.list:
        if args.out is not None or args.max_bytes is not None:
            parser.error("--list takes no other option")
        try:
            lines = describe_runs(args.state_root)
        except CollectError as error:
            print(f"collect_bundle_invalid: {error}", file=sys.stderr)
            return 66
        for line in lines:
            print(line)
        return 0
    if args.max_bytes is None:
        args.max_bytes = DEFAULT_MAX_BYTES
    if args.start is not None and (_START_ID.fullmatch(args.start) is None or int(args.start, 16) == 0):
        parser.error("--start takes a 16-character lowercase hex application start id")
    if args.max_bytes < 1:
        parser.error("--max-bytes must be positive")
    out = args.out or Path(f"manyforge-collect-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}.tar.gz")
    try:
        manifest = collect(args.state_root, out, start=args.start, max_bytes=args.max_bytes)
    except FileExistsError:
        print(f"collect_bundle_invalid: {out} already exists", file=sys.stderr)
        return 73
    except CollectError as error:
        print(f"collect_bundle_invalid: {error}", file=sys.stderr)
        return 66
    total = sum(row["bytes"] for row in manifest["included"])
    print(f"Wrote {out}: run {manifest['run_folder']} (start {manifest['application_start_id'] or 'unknown'}), "
          f"{len(manifest['included'])} files, {total} bytes before compression; "
          f"{len(manifest['skipped'])} skipped (see manifest.json)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
