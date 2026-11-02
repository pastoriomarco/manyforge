"""Read the launcher's bounded, immutable previous-session notice projection."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat


def previous_session_notice(path: str | None) -> dict | None:
    if path is None:
        return None
    descriptor = os.open(Path(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("previous workbench session report is not a regular file")
        raw = source.read(64 * 1024 + 1)
    if len(raw) > 64 * 1024:
        raise ValueError("previous workbench session report is oversized")
    report = json.loads(raw)
    if (not isinstance(report, dict)
            or report.get("schema_version") != "manyforge.workbench_previous_session.v1"
            or re.fullmatch(r"[0-9a-f]{32}", str(report.get("session_id", ""))) is None
            or report.get("archive_relative_path") != "previous-sessions/" + report["session_id"]
            or report.get("shutdown") not in {"clean", "unknown"}
            or report.get("effects") not in {"closed", "unknown", "unresolved"}
            or report.get("history_coverage") != "not_imported"
            or report.get("journal_families") != ["program_cycle", "scene", "manager", "kernel_operation"]
            or not isinstance(report.get("message"), str)
            or not 0 < len(report["message"].encode()) <= 1024):
        raise ValueError("previous workbench session report is invalid")
    # DX1005: opacity after an ordinary closed shutdown is a History-only
    # diagnostic. A positive unresolved fact remains a visible warning.
    return None if report["shutdown"] == "clean" and report["effects"] != "unresolved" else report
