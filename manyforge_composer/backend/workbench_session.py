"""Read the launcher's bounded, immutable previous-session notice projection."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat


_REPORT_FIELDS = frozenset({
    "schema_version", "session_id", "archive_relative_path", "shutdown", "effects",
    "history_coverage", "journal_families", "message",
    "unfinished_operations", "unfinished_operation_count", "application_start_id",
})
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


def _unfinished_operations_valid(report: dict) -> bool:
    """The previous run's unfinished operations: the first 64 and their count,
    each marked host_replaced, and the start they belong to; all null when the
    journal was unreadable."""

    rows, count = report.get("unfinished_operations"), report.get("unfinished_operation_count")
    start = report.get("application_start_id")
    if rows is None or count is None or start is None:
        return rows is None and count is None and start is None
    if not isinstance(start, str) or re.fullmatch(r"[0-9a-f]{16}", start) is None or start == "0" * 16:
        return False
    return (
        isinstance(rows, list) and len(rows) <= 64
        and type(count) is int and len(rows) <= count <= _MAXIMUM_SAFE_INTEGER
        and all(
            isinstance(row, dict) and set(row) == {"operation_id", "disposition"}
            and row["disposition"] == "host_replaced"
            and type(row["operation_id"]) is int
            and 0 < row["operation_id"] <= _MAXIMUM_SAFE_INTEGER
            for row in rows
        )
    )


def previous_session_notice(path: str | None) -> dict | None:
    if path is None:
        return None
    descriptor = os.open(Path(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("previous ManyForge session report is not a regular file")
        raw = source.read(64 * 1024 + 1)
    if len(raw) > 64 * 1024:
        raise ValueError("previous ManyForge session report is oversized")
    report = json.loads(raw)
    if (not isinstance(report, dict)
            or set(report) != _REPORT_FIELDS
            or report.get("schema_version") != "manyforge.workbench_previous_session.v2"
            or re.fullmatch(r"[0-9a-f]{32}", str(report.get("session_id", ""))) is None
            or report.get("archive_relative_path") != "previous-runs/" + report["session_id"]
            or report.get("shutdown") not in {"clean", "unknown"}
            or report.get("effects") not in {"closed", "unknown", "unresolved"}
            or report.get("history_coverage") != "not_imported"
            or report.get("journal_families") != ["program_cycle", "scene", "manager", "kernel_operation"]
            or not isinstance(report.get("message"), str)
            or not 0 < len(report["message"].encode()) <= 1024
            or not _unfinished_operations_valid(report)):
        raise ValueError("previous ManyForge session report is invalid")
    # Opacity after an ordinary closed shutdown is a History-only
    # diagnostic. A positive unresolved fact remains a visible warning.
    return None if report["shutdown"] == "clean" and report["effects"] != "unresolved" else report
