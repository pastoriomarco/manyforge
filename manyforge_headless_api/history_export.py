"""Export one start's or one Program run's History rows as NDJSON, oldest first.

``python -m manyforge_headless_api history export``: read-only. Over HTTP it
pages the existing full-row export route (``/api/execution/history/export``,
newest first, ``before_sequence``); from a History file
(``history/history.v<N>.sqlite3`` under the runtime state root) it reads the
rows with one ``SELECT`` and never writes beside the file. Both keep the
newest matching rows within ``--max-rows`` and write them oldest first.

Each line is one JSON object: ``sequence``, ``application_start_id`` (in
HTTP mode only when the command names the start), ``event`` (component,
event id and type, severity, ``occurred_at_ns`` and ``occurred_at_wall``,
payload), ``clock_owner``, ``occurred_at_utc_ns``, ``wall_clock_regressed``
and ``condition_severity``; HTTP rows also carry the server's
``persistence_digest``.

Only the standard library is imported at module level: the host-side bug
report bundle (``scripts/lib/collect_bundle.py``) loads this file by path and
uses its file mode without the API's dependencies.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import sqlite3
import sys
import tempfile
from typing import Any, BinaryIO, Callable, Iterator

DEFAULT_MAX_ROWS = 1_000_000
DEFAULT_HEADLESS_API_URL = "http://127.0.0.1:9010"
EXPORT_ROUTE = "/api/execution/history/export"
_START_ID = re.compile(r"[0-9a-f]{16}\Z")
_MAX_SAFE_INTEGER = 9_007_199_254_740_991
_FILE_BATCH_ROWS = 1000


@dataclass(frozen=True)
class ExportResult:
    rows: int
    bytes: int
    # None when every matching row was written; else "max_rows" or "max_bytes".
    stopped_at: str | None


def _start_id(text: str) -> str:
    if _START_ID.fullmatch(text) is None or int(text, 16) == 0:
        raise argparse.ArgumentTypeError(
            "application start id must be 16 lowercase hex characters, not all zero")
    return text


def _program_run_id(text: str) -> int:
    if not text.isdigit() or not 0 < int(text) <= _MAX_SAFE_INTEGER:
        raise argparse.ArgumentTypeError("program run id must be a positive integer")
    return int(text)


def _utc_ns(text: str) -> int:
    """An ISO 8601 time with its UTC offset, e.g. ``2026-11-24T14:10:00Z``."""
    try:
        value = datetime.fromisoformat(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if value.utcoffset() is None:
        raise argparse.ArgumentTypeError("time must state its UTC offset, e.g. 2026-11-24T14:10:00Z")
    delta = value - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return (delta.days * 86_400 + delta.seconds) * 1_000_000_000 + delta.microseconds * 1000


def _positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def add_parser(subparsers: Any) -> argparse.ArgumentParser:
    """Add ``export`` to the ``history`` command's subparsers."""
    parser = subparsers.add_parser(
        "export", help="write one start's or one Program run's History rows as NDJSON, oldest first")
    parser.add_argument("--application-start-id", type=_start_id)
    parser.add_argument(
        "--program-run-id", type=_program_run_id,
        help="one Program run; without --application-start-id over HTTP: of the current start")
    parser.add_argument("--since", type=_utc_ns, help="first recorded UTC time, ISO 8601 with offset")
    parser.add_argument("--until", type=_utc_ns, help="last recorded UTC time, ISO 8601 with offset")
    parser.add_argument("--out", type=Path, required=True, help="new NDJSON file")
    parser.add_argument("--max-rows", type=_positive, default=DEFAULT_MAX_ROWS)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--headless-api-url", default=DEFAULT_HEADLESS_API_URL)
    source.add_argument("--history-file", type=Path,
                        help="a History file (history.v<N>.sqlite3), read without writing beside it")
    return parser


# ---------------------------------------------------------------------------
# File mode


@contextmanager
def open_readonly(path: Path) -> Iterator[sqlite3.Connection]:
    """Open an SQLite file without writing into its folder.

    A file without a write-ahead log is opened ``immutable``. With its log
    and shared index present (a live or interrupted writer) it is opened
    ``mode=ro``, which reads through them as a reader does. A log without its
    index is copied with the file into a temporary folder and read there.
    """
    path = path.resolve()
    wal = path.with_name(path.name + "-wal")
    shm = path.with_name(path.name + "-shm")
    if not path.is_file():
        raise FileNotFoundError(f"no SQLite file at {path}")
    if wal.exists() and shm.exists():
        connection = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
        scratch = None
    elif wal.exists() and wal.stat().st_size:
        scratch = tempfile.TemporaryDirectory(prefix="manyforge-sqlite-copy.")
        copy = Path(scratch.name) / path.name
        shutil.copyfile(path, copy)
        shutil.copyfile(wal, copy.with_name(copy.name + "-wal"))
        connection = sqlite3.connect(copy)
    else:
        connection = sqlite3.connect(path.as_uri() + "?mode=ro&immutable=1", uri=True)
        scratch = None
    try:
        yield connection
    finally:
        connection.close()
        if scratch is not None:
            scratch.cleanup()


_FILE_COLUMNS = (
    "sequence, application_start_id, component, event_id, event_type, severity, occurred_at_ns, "
    "occurred_at_wall, payload_json, clock_owner, occurred_at_utc_ns, wall_clock_regressed, "
    "condition_severity")


def file_rows_newest_first(path: Path, *, application_start_id: str, program_run_id: int | None = None,
                           since_utc_ns: int | None = None, until_utc_ns: int | None = None) -> Iterator[dict]:
    """The matching rows of one History file, newest first."""
    sql = f"SELECT {_FILE_COLUMNS} FROM historian_events WHERE application_start_id=?"
    parameters: list[Any] = [application_start_id]
    if program_run_id is not None:
        # Program run numbers are keyed as decimal text (store format 7).
        sql += (" AND sequence IN (SELECT sequence FROM historian_event_keys"
                " WHERE kind='program_run_id' AND value=?)")
        parameters.append(str(program_run_id))
    if since_utc_ns is not None:
        sql += " AND occurred_at_utc_ns>=?"
        parameters.append(since_utc_ns)
    if until_utc_ns is not None:
        sql += " AND occurred_at_utc_ns<=?"
        parameters.append(until_utc_ns)
    sql += " AND sequence<? ORDER BY sequence DESC LIMIT ?"
    # Short read transactions, one per batch: a reader of the live file never
    # holds the writer's log from being reclaimed for the whole export.
    before = _MAX_SAFE_INTEGER + 1
    with open_readonly(path) as database:
        while True:
            batch = database.execute(sql, [*parameters, before, _FILE_BATCH_ROWS]).fetchall()
            if not batch:
                return
            before = batch[-1][0]
            yield from (_file_row(row) for row in batch)


def _file_row(row: tuple) -> dict:
    (sequence, start, component, event_id, event_type, severity, occurred_at_ns,
     occurred_at_wall, payload_json, clock_owner, utc_ns, regressed, condition) = row
    return {
        "sequence": sequence,
        "application_start_id": start,
        "event": {
            "component": component,
            "event_id": event_id,
            "event_type": event_type,
            "occurred_at_ns": str(occurred_at_ns),
            "occurred_at_wall": occurred_at_wall,
            "payload": json.loads(payload_json),
            "severity": severity,
        },
        "clock_owner": clock_owner,
        "occurred_at_utc_ns": None if utc_ns is None else str(utc_ns),
        "wall_clock_regressed": bool(regressed),
        "condition_severity": condition,
    }


# ---------------------------------------------------------------------------
# HTTP mode


def http_rows_newest_first(fetch: Callable[[list[tuple[str, str | int]]], dict], *,
                           application_start_id: str | None, program_run_id: int | None,
                           since_utc_ns: int | None, until_utc_ns: int | None) -> Iterator[dict]:
    """Page the export route with ``before_sequence`` until it is done."""
    base: list[tuple[str, str | int]] = []
    if application_start_id is not None:
        base.append(("application_start_id", application_start_id))
    if program_run_id is not None:
        base.append(("program_run_id", str(program_run_id)))
    if since_utc_ns is not None:
        base.append(("start_utc_ns", since_utc_ns))
    if until_utc_ns is not None:
        base.append(("end_utc_ns", until_utc_ns))
    before: int | None = None
    while True:
        page = fetch(base + ([("before_sequence", before)] if before is not None else []))
        for row in page["events"]:
            yield row if application_start_id is None else {**row, "application_start_id": application_start_id}
        if not page["truncated"]:
            return
        before = page["next_before_sequence"]


# ---------------------------------------------------------------------------
# Output


def _line(row: dict) -> bytes:
    return (json.dumps(row, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n").encode()


def write_oldest_first(rows: Iterator[dict], output: BinaryIO, *, max_rows: int,
                       max_bytes: int | None = None) -> ExportResult:
    """Write newest-first rows oldest first through a temporary spool file.

    Reading stops at ``max_rows`` rows or before the row that would pass
    ``max_bytes``; the newest rows read are the ones written.
    """
    offsets: list[int] = []
    stopped = None
    with tempfile.TemporaryFile() as spool:
        for row in rows:
            if len(offsets) == max_rows:
                stopped = "max_rows"
                break
            line = _line(row)
            if max_bytes is not None and spool.tell() + len(line) > max_bytes:
                stopped = "max_bytes"
                break
            offsets.append(spool.tell())
            spool.write(line)
        end = spool.tell()
        written = 0
        for offset in reversed(offsets):
            spool.seek(offset)
            data = spool.read(end - offset)
            output.write(data)
            written += len(data)
            end = offset
    return ExportResult(len(offsets), written, stopped)


def run(args: argparse.Namespace) -> int:
    if args.history_file is not None and args.application_start_id is None:
        print("history_export_invalid: --history-file requires --application-start-id "
              "(the file holds several starts)", file=sys.stderr)
        return 64
    if args.since is not None and args.until is not None and args.since > args.until:
        print("history_export_invalid: --since is after --until", file=sys.stderr)
        return 64
    if args.application_start_id is None and args.program_run_id is None:
        print("history_export_invalid: name --application-start-id or --program-run-id", file=sys.stderr)
        return 64
    selection = dict(application_start_id=args.application_start_id, program_run_id=args.program_run_id,
                     since_utc_ns=args.since, until_utc_ns=args.until)
    try:
        output = args.out.open("xb")
    except OSError as error:
        print(f"history_export_output_unavailable: {error}", file=sys.stderr)
        return 73
    with output:
        if args.history_file is not None:
            try:
                result = write_oldest_first(file_rows_newest_first(args.history_file, **selection),
                                            output, max_rows=args.max_rows)
            except (OSError, sqlite3.Error, ValueError) as error:
                args.out.unlink()
                print(f"history_export_file_unreadable: {args.history_file}: {error}", file=sys.stderr)
                return 66
        else:
            from .client import HeadlessClientError, HeadlessHttpClient

            try:
                with HeadlessHttpClient(args.headless_api_url, timeout_seconds=30.0) as client:
                    result = write_oldest_first(http_rows_newest_first(
                        lambda params: client.request("GET", EXPORT_ROUTE, params=params), **selection),
                        output, max_rows=args.max_rows)
            except HeadlessClientError as error:
                args.out.unlink()
                print(f"{error.code}: {error}", file=sys.stderr)
                return 69
    if result.stopped_at == "max_rows":
        print(f"History export stopped at --max-rows {args.max_rows}: {args.out} holds the newest "
              f"{result.rows} matching rows; narrow it with --since/--until or raise --max-rows",
              file=sys.stderr)
        return 0
    print(f"Wrote {result.rows} History rows to {args.out}", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    """Run ``export`` alone (``python history_export.py export ...``)."""
    parser = argparse.ArgumentParser(prog="history")
    add_parser(parser.add_subparsers(dest="history_command", required=True))
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
