#!/usr/bin/env python3
"""Collect MANYFORGE_QUALIFICATION_EVIDENCE lines into an annotated JSONL log.

The collector is intentionally generic: it can wrap a command after ``--`` or
read from stdin. In command mode it streams the command output through
unchanged, validates every evidence payload, then appends annotated records to
``<repo>/qualification/evidence-YYYY-MM-DD.jsonl`` using one O_APPEND write per
record so parallel qualification jobs cannot interleave partial JSON lines.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Iterable, Sequence


EVIDENCE_PREFIX = "MANYFORGE_QUALIFICATION_EVIDENCE "
SCHEMA_VERSION = "manyforge.qualification_evidence.v1"


@dataclass(frozen=True)
class EvidenceLine:
    payload: dict[str, Any]
    line_number: int
    collected_at_wall: str


class EvidenceError(ValueError):
    """Raised when a prefixed evidence line is malformed."""


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def parse_evidence_date(value: str) -> str:
    try:
        parsed = datetime.strptime(value, "%Y-%m-%d")
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "invalid evidence date; expected YYYY-MM-DD"
        ) from exc
    return parsed.date().isoformat()


def parse_evidence_line(line: str, line_number: int) -> EvidenceLine | None:
    if not line.startswith(EVIDENCE_PREFIX):
        return None
    raw_payload = line[len(EVIDENCE_PREFIX) :].strip()
    if not raw_payload:
        raise EvidenceError(f"line {line_number}: empty evidence payload")
    try:
        payload = json.loads(raw_payload)
    except json.JSONDecodeError as exc:
        raise EvidenceError(
            f"line {line_number}: invalid evidence JSON at column {exc.colno}: {exc.msg}"
        ) from exc
    if not isinstance(payload, dict):
        raise EvidenceError(f"line {line_number}: evidence payload must be a JSON object")
    return EvidenceLine(
        payload=payload,
        line_number=line_number,
        collected_at_wall=utc_now_iso(),
    )


def repo_commit(repo_root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def repo_dirty(repo_root: Path) -> bool:
    try:
        status = subprocess.check_output(
            ["git", "-C", str(repo_root), "status", "--porcelain"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return bool(status.strip())


def evidence_output_path(
    *,
    repo_root: Path,
    output_dir: Path | None,
    evidence_date: str | None,
) -> Path:
    env_output_dir = os.environ.get("MANYFORGE_QUALIFICATION_EVIDENCE_DIR")
    if output_dir is not None:
        root = output_dir
    elif env_output_dir:
        root = Path(env_output_dir)
    else:
        root = repo_root / "qualification"
    date_text = evidence_date or datetime.now(timezone.utc).date().isoformat()
    return root / f"evidence-{date_text}.jsonl"


def annotate_records(
    evidence: Iterable[EvidenceLine],
    *,
    repo_root: Path,
    commit: str,
    dirty: bool,
    lane: str,
    collector_run_id: str,
    source: str | None,
    command: Sequence[str] | None,
    command_exit_code: int,
) -> list[dict[str, Any]]:
    command_list = list(command) if command is not None else None
    records: list[dict[str, Any]] = []
    for item in evidence:
        records.append(
            {
                "schema_version": SCHEMA_VERSION,
                "collected_at_wall": item.collected_at_wall,
                "lane": lane,
                "collector_run_id": collector_run_id,
                "repo": {
                    "root": str(repo_root),
                    "commit": commit,
                    "dirty": dirty,
                },
                "source": {
                    "name": source,
                    "command": command_list,
                    "line_number": item.line_number,
                    "command_exit_code": command_exit_code,
                },
                "evidence": item.payload,
            }
        )
    return records


def append_jsonl(path: Path, records: Iterable[dict[str, Any]]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_APPEND | os.O_CREAT | os.O_WRONLY
    fd = os.open(path, flags, 0o644)
    count = 0
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        for record in records:
            encoded = (
                json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
            ).encode("utf-8")
            view = memoryview(encoded)
            while view:
                written = os.write(fd, view)
                if written <= 0:
                    raise OSError("short write while appending qualification evidence")
                view = view[written:]
            count += 1
        os.fsync(fd)
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)
    return count


def collect_from_lines(lines: Iterable[str]) -> tuple[list[EvidenceLine], list[str]]:
    evidence: list[EvidenceLine] = []
    errors: list[str] = []
    for line_number, line in enumerate(lines, start=1):
        try:
            parsed = parse_evidence_line(line.rstrip("\n"), line_number)
        except EvidenceError as exc:
            errors.append(str(exc))
            continue
        if parsed is not None:
            evidence.append(parsed)
    return evidence, errors


def run_command(command: Sequence[str]) -> tuple[int, list[EvidenceLine], list[str]]:
    try:
        process = subprocess.Popen(
            list(command),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as exc:
        print(
            f"qualification evidence error: failed to launch {command[0]!r}: {exc}",
            file=sys.stderr,
        )
        return 127, [], []
    assert process.stdout is not None
    evidence: list[EvidenceLine] = []
    errors: list[str] = []
    for line_number, line in enumerate(process.stdout, start=1):
        print(line, end="")
        try:
            parsed = parse_evidence_line(line.rstrip("\n"), line_number)
        except EvidenceError as exc:
            errors.append(str(exc))
            continue
        if parsed is not None:
            evidence.append(parsed)
    return process.wait(), evidence, errors


def normalize_process_status(returncode: int) -> int:
    if returncode < 0:
        return 128 + abs(returncode)
    return returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Collect MANYFORGE_QUALIFICATION_EVIDENCE JSON lines.",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="Repository root used for commit annotation and default output path.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for evidence-YYYY-MM-DD.jsonl. Defaults to <repo>/qualification.",
    )
    parser.add_argument(
        "--date",
        type=parse_evidence_date,
        default=None,
        help="Evidence file date (YYYY-MM-DD). Defaults to current UTC date.",
    )
    parser.add_argument(
        "--lane",
        default=os.environ.get("MANYFORGE_QUALIFICATION_LANE", "local"),
        help="Qualification lane label stored in each record.",
    )
    parser.add_argument(
        "--run-id",
        default=None,
        help="Collector run id. Defaults to UTC timestamp plus process id.",
    )
    parser.add_argument(
        "--source",
        default=None,
        help="Optional source/probe name stored in each record.",
    )
    parser.add_argument(
        "--commit",
        default=None,
        help="Override git commit annotation.",
    )
    parser.add_argument(
        "--require-evidence",
        action="store_true",
        help="Fail with exit 4 when no evidence lines are collected.",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to wrap after --. Without a command, stdin is read.",
    )
    return parser


def normalize_command(command: Sequence[str]) -> list[str] | None:
    if not command:
        return None
    normalized = list(command)
    if normalized and normalized[0] == "--":
        normalized = normalized[1:]
    return normalized or None


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    repo_root = args.repo_root.resolve()
    command = normalize_command(args.command)

    if command is None:
        evidence, errors = collect_from_lines(sys.stdin)
        command_exit_code = 0
    else:
        command_exit_code, evidence, errors = run_command(command)

    if errors:
        for error in errors:
            print(f"qualification evidence error: {error}", file=sys.stderr)
        return 2

    if args.require_evidence and not evidence and command_exit_code == 0:
        print("qualification evidence error: no evidence lines collected", file=sys.stderr)
        return 4

    collector_run_id = args.run_id or (
        f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    )
    records = annotate_records(
        evidence,
        repo_root=repo_root,
        commit=args.commit or repo_commit(repo_root),
        dirty=repo_dirty(repo_root),
        lane=args.lane,
        collector_run_id=collector_run_id,
        source=args.source,
        command=command,
        command_exit_code=command_exit_code,
    )
    if records:
        output_path = evidence_output_path(
            repo_root=repo_root,
            output_dir=args.output_dir,
            evidence_date=args.date,
        )
        count = append_jsonl(output_path, records)
        print(
            f"collected {count} qualification evidence record(s) -> {output_path}",
            file=sys.stderr,
        )

    return normalize_process_status(command_exit_code)


if __name__ == "__main__":
    raise SystemExit(main())
