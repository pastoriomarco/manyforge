"""``history export``: HTTP paging and read-only History file reads, oldest first."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sqlite3
from typing import Any

import pytest

from manyforge_headless_api import client as client_module
from manyforge_headless_api import history_export

STORE = Path(__file__).resolve().parents[2] / "manyforge_headless/src/sqlite_event_history_store.cpp"
START = "1" * 16
OTHER = "2" * 16


def store_schema() -> str:
    """The store's own table statements (format 9), read from its source."""
    source = STORE.read_text(encoding="utf-8")
    parts = [re.search(r"constexpr std::string_view " + name + r' = R"sql\((.*?)\)sql";', source, re.S)[1]
             for name in ("kSchema", "kQueryIndexSchema", "kCaptureSchema", "kSummarySchema")]
    return "\n".join(parts)


def history_file(folder: Path, rows: list[tuple[str, int, int | None]]) -> Path:
    """rows: (application start id, program run id or None, UTC ns); closed cleanly."""
    path = folder / "history.v9.sqlite3"
    database = sqlite3.connect(path)
    database.execute("PRAGMA journal_mode=WAL")
    database.executescript(store_schema())
    for index, (start, run, utc) in enumerate(rows, 1):
        payload = {"message": f"event {index}", **({"program_run_id": run} if run else {})}
        database.execute(
            "INSERT INTO historian_events(event_id,component,event_type,severity,occurred_at_ns,"
            "occurred_at_wall,payload_json,payload_digest,event_digest,application_start_id,"
            "occurred_at_utc_ns,clock_owner,condition_severity) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (f"event-{index:032x}", "manyforge.test", "test_event", "INFO", 1000 + index,
             "2026-11-24T14:10:00Z", json.dumps(payload), "d", "e", start, utc, "headless:" + start,
             "WARN" if index == 2 else "INFO"))
        if run:
            database.execute("INSERT INTO historian_event_keys VALUES(?,?,?,?)",
                             (index, "program_run_id", str(run), start))
    database.commit()
    database.close()
    return path


def _export(path: Path, **selection: Any) -> list[dict]:
    with (path.parent / "out.ndjson").open("wb") as output:
        history_export.write_oldest_first(
            history_export.file_rows_newest_first(path, **selection), output, max_rows=100)
    lines = (path.parent / "out.ndjson").read_text().splitlines()
    (path.parent / "out.ndjson").unlink()
    return [json.loads(line) for line in lines]


def test_file_mode_reads_one_start_oldest_first_without_writing_beside_the_file(tmp_path: Path) -> None:
    folder = tmp_path / "history"
    folder.mkdir()
    path = history_file(folder, [(START, 1, 10), (START, 1, 20), (OTHER, 1, 30), (START, 2, 40)])
    before = sorted(entry.name for entry in folder.iterdir())
    rows = _export(path, application_start_id=START)
    assert sorted(entry.name for entry in folder.iterdir()) == before == ["history.v9.sqlite3"]
    assert [row["sequence"] for row in rows] == [1, 2, 4]
    assert rows[1] == {
        "sequence": 2,
        "application_start_id": START,
        "event": {"component": "manyforge.test", "event_id": f"event-{2:032x}", "event_type": "test_event",
                  "occurred_at_ns": "1002", "occurred_at_wall": "2026-11-24T14:10:00Z",
                  "payload": {"message": "event 2", "program_run_id": 1}, "severity": "INFO"},
        "clock_owner": "headless:" + START,
        "occurred_at_utc_ns": "20",
        "wall_clock_regressed": False,
        "condition_severity": "WARN",
    }


def test_file_mode_filters_program_run_and_utc_window(tmp_path: Path) -> None:
    path = history_file(tmp_path, [(START, 1, 10), (START, 1, 20), (OTHER, 1, 25), (START, 2, 30), (START, 1, 40)])
    assert [row["sequence"] for row in _export(path, application_start_id=START, program_run_id=1)] == [1, 2, 5]
    assert [row["sequence"] for row in _export(
        path, application_start_id=START, program_run_id=1, since_utc_ns=15, until_utc_ns=40)] == [2, 5]


def test_file_mode_reads_more_rows_than_one_batch(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(history_export, "_FILE_BATCH_ROWS", 2)
    path = history_file(tmp_path, [(START, None, index) for index in range(7)])
    assert [row["sequence"] for row in _export(path, application_start_id=START)] == list(range(1, 8))


def test_file_mode_reads_a_live_log_through_its_index(tmp_path: Path) -> None:
    path = history_file(tmp_path, [(START, None, 1)])
    writer = sqlite3.connect(path)
    writer.execute("PRAGMA wal_autocheckpoint=0")
    writer.execute("UPDATE historian_events SET occurred_at_wall='2026-11-24T15:00:00Z'")
    writer.commit()
    try:
        assert {entry.name for entry in tmp_path.iterdir()} >= {"history.v9.sqlite3-wal", "history.v9.sqlite3-shm"}
        assert _export(path, application_start_id=START)[0]["event"]["occurred_at_wall"] == "2026-11-24T15:00:00Z"
    finally:
        writer.close()


def test_max_rows_keeps_the_newest_rows(tmp_path: Path) -> None:
    rows = iter([{"sequence": sequence} for sequence in (5, 4, 3, 2, 1)])
    with (tmp_path / "out").open("wb") as output:
        result = history_export.write_oldest_first(rows, output, max_rows=3)
    assert result == history_export.ExportResult(3, len((tmp_path / "out").read_bytes()), "max_rows")
    assert [json.loads(line)["sequence"] for line in (tmp_path / "out").read_text().splitlines()] == [3, 4, 5]


def test_max_bytes_stops_before_the_row_that_does_not_fit(tmp_path: Path) -> None:
    line = len(b'{"sequence":5}\n')
    with (tmp_path / "out").open("wb") as output:
        result = history_export.write_oldest_first(
            iter([{"sequence": sequence} for sequence in (5, 4, 3)]), output, max_rows=10, max_bytes=2 * line + 1)
    assert result == history_export.ExportResult(2, 2 * line, "max_bytes")


class _Client:
    pages: list[dict[str, Any]] = []
    calls: list[list[tuple[str, Any]]] = []

    def __init__(self, url: str, *, timeout_seconds: float) -> None:
        self.url = url

    def __enter__(self) -> "_Client":
        return self

    def __exit__(self, *_exc: object) -> None:
        return None

    def request(self, method: str, path: str, *, params: list[tuple[str, Any]]) -> dict[str, Any]:
        assert (method, path) == ("GET", "/api/execution/history/export")
        self.calls.append(params)
        return self.pages[len(self.calls) - 1]


def _parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command")
    history = commands.add_parser("history")
    history.add_argument("--url", default="http://127.0.0.1:9010")
    history_export.add_parser(history.add_subparsers(dest="history_command"))
    return parser.parse_args(argv)


def test_http_mode_pages_with_before_sequence_and_writes_oldest_first(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setattr(client_module, "HeadlessHttpClient", _Client)
    _Client.calls = []
    _Client.pages = [
        {"events": [{"sequence": 9}, {"sequence": 7}], "truncated": True, "next_before_sequence": 7},
        {"events": [{"sequence": 3}], "truncated": False, "next_before_sequence": None},
    ]
    out = tmp_path / "rows.ndjson"
    args = _parse(["history", "export", "--application-start-id", START, "--program-run-id", "4",
                   "--since", "1970-01-01T00:00:01Z", "--out", str(out)])
    assert history_export.run(args) == 0
    assert _Client.calls == [
        [("application_start_id", START), ("program_run_id", "4"), ("start_utc_ns", 1_000_000_000)],
        [("application_start_id", START), ("program_run_id", "4"), ("start_utc_ns", 1_000_000_000),
         ("before_sequence", 7)],
    ]
    assert [json.loads(line) for line in out.read_text().splitlines()] == [
        {"sequence": sequence, "application_start_id": START} for sequence in (3, 7, 9)]
    assert "Wrote 3 History rows" in capsys.readouterr().err


def test_http_mode_reports_max_rows(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                    capsys: pytest.CaptureFixture[str]) -> None:
    monkeypatch.setattr(client_module, "HeadlessHttpClient", _Client)
    _Client.calls = []
    _Client.pages = [{"events": [{"sequence": 9}, {"sequence": 7}], "truncated": True, "next_before_sequence": 7}]
    out = tmp_path / "rows.ndjson"
    assert history_export.run(_parse(["history", "export", "--program-run-id", "4", "--max-rows", "1",
                                       "--out", str(out)])) == 0
    assert [json.loads(line) for line in out.read_text().splitlines()] == [{"sequence": 9}]
    assert "stopped at --max-rows 1" in capsys.readouterr().err


def test_file_mode_through_run_and_its_refusals(tmp_path: Path) -> None:
    path = history_file(tmp_path, [(START, 1, 10), (OTHER, 1, 20)])
    out = tmp_path / "rows.ndjson"
    assert history_export.run(_parse(["history", "export", "--history-file", str(path), "--program-run-id", "1",
                                      "--out", str(out)])) == 64
    assert history_export.run(_parse(["history", "export", "--history-file", str(path),
                                      "--application-start-id", START, "--out", str(out)])) == 0
    assert [json.loads(line)["sequence"] for line in out.read_text().splitlines()] == [1]
    # The output is never overwritten.
    assert history_export.run(_parse(["history", "export", "--history-file", str(path),
                                      "--application-start-id", START, "--out", str(out)])) == 73
    missing = tmp_path / "missing.ndjson"
    assert history_export.run(_parse(["history", "export", "--history-file", str(tmp_path / "absent.sqlite3"),
                                      "--application-start-id", START, "--out", str(missing)])) == 66
    assert not missing.exists()
    with pytest.raises(SystemExit):
        _parse(["history", "export", "--application-start-id", "0" * 16, "--out", str(out)])
    with pytest.raises(SystemExit):
        _parse(["history", "export", "--application-start-id", START, "--since", "2026-11-24T14:10:00",
                "--out", str(out)])
